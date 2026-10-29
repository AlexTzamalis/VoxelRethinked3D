#include "vxe/voxel/lod_render_sink.hpp"

#include <vxe/core/profile.hpp>
#include <vxe/voxel/column_summary.hpp>

#include <algorithm>
#include <limits>

namespace vxe::voxel {

u64 lodTileId(const LodTileKey& key) noexcept {
    return (u64{key.level} << 58U) ^ (u64{static_cast<u32>(key.x)} << 29U) ^ u64 { static_cast<u32>(key.z) };
}

LodRenderSink::LodRenderSink(render::LodRenderer& renderer, jobs::JobSystem& jobs, const VoxelWorld* world)
    : m_renderer(renderer)
    , m_jobs(jobs)
    , m_world(world) {}

LodRenderSink::~LodRenderSink() {
    for (Slot* slot : m_busy) {
        slot->cancelled.store(true, std::memory_order_relaxed);
    }
    m_jobs.wait(m_jobCounter);
}

void LodRenderSink::addTile(std::shared_ptr<const LodTile> tile) {
    const u64 id = lodTileId(tile->key());
    // A newer tile replaces one still being meshed or waiting.
    if (const auto it = m_pending.find(id); it != m_pending.end()) {
        it->second->cancelled.store(true, std::memory_order_relaxed);
    }
    if (m_free.empty()) {
        m_slots.push_back(std::make_unique<Slot>());
        m_free.push_back(m_slots.back().get());
    }
    Slot* slot = m_free.back();
    m_free.pop_back();
    slot->id = id;
    slot->tile = std::move(tile);
    slot->done.store(false, std::memory_order_relaxed);
    slot->cancelled.store(false, std::memory_order_relaxed);
    m_pending.insert_or_assign(id, slot);
    m_busy.push_back(slot);
    m_jobs.schedule(
        m_jobCounter,
        [slot] {
            if (!slot->cancelled.load(std::memory_order_relaxed)) {
                thread_local LodMesher mesher;
                mesher.mesh(*slot->tile, {}, slot->mesh);
            }
            slot->done.store(true, std::memory_order_release);
        },
        jobs::Priority::Low);
}

void LodRenderSink::removeTile(const LodTileKey& key) {
    const u64 id = lodTileId(key);
    if (const auto it = m_pending.find(id); it != m_pending.end()) {
        it->second->cancelled.store(true, std::memory_order_relaxed);
        m_pending.erase(it);
    }
    m_renderer.remove(id);
}

void LodRenderSink::update() {
    VXE_PROFILE_FUNCTION();
    collect();
    uploadReady();
    updateCoverage();
}

void LodRenderSink::freeSlot(Slot* slot) {
    slot->tile.reset();
    m_free.push_back(slot);
}

void LodRenderSink::collect() {
    for (usize i = 0; i < m_busy.size();) {
        Slot* slot = m_busy[i];
        if (!slot->done.load(std::memory_order_acquire)) {
            ++i;
            continue;
        }
        m_busy[i] = m_busy.back();
        m_busy.pop_back();
        if (slot->cancelled.load(std::memory_order_relaxed)) {
            freeSlot(slot);
        } else {
            m_ready.push_back(slot);
        }
    }
}

// Uploads meshed tiles in the order they finished until the renderer's
// budget for this frame is used.
void LodRenderSink::uploadReady() {
    while (!m_ready.empty()) {
        Slot* slot = m_ready.front();
        if (slot->cancelled.load(std::memory_order_relaxed)) {
            m_ready.pop_front();
            freeSlot(slot);
            continue;
        }
        const LodTileKey& key = slot->mesh.key;
        const render::LodGeometry geometry{.id = slot->id,
                                           .originX = key.originX(),
                                           .originZ = key.originZ(),
                                           .cellSize = static_cast<u32>(key.cellSize()),
                                           .level = key.level,
                                           .baseY = slot->mesh.baseY,
                                           .minY = slot->mesh.minY,
                                           .maxY = slot->mesh.maxY,
                                           .quads = std::as_bytes(std::span(slot->mesh.quads))};
        if (m_renderer.upload(geometry) == render::LodRenderer::UploadResult::OverBudget) {
            return;
        }
        m_ready.pop_front();
        if (const auto it = m_pending.find(slot->id); it != m_pending.end() && it->second == slot) {
            m_pending.erase(it);
        }
        freeSlot(slot);
    }
}

void LodRenderSink::chunkMeshed(ChunkPos position, bool meshed) {
    if (m_world == nullptr) {
        return;
    }
    const bool changed = meshed ? m_meshed.insert(position).second : m_meshed.erase(position) > 0;
    if (changed) {
        m_dirtyColumns.insert({.x = position.x, .z = position.z});
    }
}

// Works out the coverage of columns whose chunks changed (see the class
// comment) and passes it to the renderer, which uploads the map once.
void LodRenderSink::updateCoverage() {
    if (m_world == nullptr || m_dirtyColumns.empty()) {
        return;
    }
    for (const ColumnPos& column : m_dirtyColumns) {
        m_renderer.setCovered(column.x, column.z, isCovered(column));
    }
    m_dirtyColumns.clear();
}

// The surfaces of a column's 256 block columns can sit in different chunks
// (an island in a lake): every chunk from the lowest surface to the chunk
// above the highest must be meshed, or some of the ground far terrain would
// hide is missing (for example below the vertical render distance). The chunk
// above only counts if it exists (the world may end there).
bool LodRenderSink::isCovered(ColumnPos column) const {
    const ColumnSummary* summary = m_world->findColumn(column);
    if (summary == nullptr) {
        return false;
    }
    i32 lowest = std::numeric_limits<i32>::max();
    i32 highest = ColumnSummary::kNoBlock;
    for (u32 z = 0; z < 16; ++z) {
        for (u32 x = 0; x < 16; ++x) {
            const i32 top = summary->highestBlock(x, z);
            if (top == ColumnSummary::kNoBlock) {
                continue;
            }
            lowest = std::min(lowest, top);
            highest = std::max(highest, top);
        }
    }
    if (highest == ColumnSummary::kNoBlock) {
        return false;
    }
    for (i32 y = lowest >> kChunkShift; y <= highest >> kChunkShift; ++y) {
        if (!m_meshed.contains({.x = column.x, .y = y, .z = column.z})) {
            return false;
        }
    }
    const ChunkPos above{.x = column.x, .y = (highest >> kChunkShift) + 1, .z = column.z};
    return m_meshed.contains(above) || m_world->findChunk(above) == nullptr;
}

ChunkMeshSink::Result CoveringChunkSink::upload(const ChunkMesh& mesh) {
    const Result result = m_inner.upload(mesh);
    if (result == Result::Accepted) {
        m_lod.chunkMeshed(mesh.position, true);
    }
    return result;
}

void CoveringChunkSink::remove(ChunkPos position) {
    m_inner.remove(position);
    m_lod.chunkMeshed(position, false);
}

} // namespace vxe::voxel
