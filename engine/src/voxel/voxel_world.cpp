#include "vxe/voxel/voxel_world.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/profile.hpp>
#include <vxe/core/time.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace vxe::voxel {
namespace {

// The part of the snapshot box one neighbor fills, along one axis: where it
// starts in snapshot coordinates, how long it is, and which local
// coordinate of the neighbor it starts from. The neighbor at -1 gives its
// last layer (15) to the border at -1; the one at +1 its first layer (0) to
// the border at 16; offset 0 covers the whole chunk width.
struct AxisSpan {
    i32 first;
    i32 count;
    i32 source;
};

constexpr AxisSpan axisSpan(i32 offset) noexcept {
    if (offset < 0) {
        return {.first = -1, .count = 1, .source = kChunkMask};
    }
    if (offset > 0) {
        return {.first = kChunkSize, .count = 1, .source = 0};
    }
    return {.first = 0, .count = kChunkSize, .source = 0};
}

constexpr u16 blockIndexOf(i32 x, i32 y, i32 z) noexcept {
    return static_cast<u16>((static_cast<u32>(y) << 8U) | (static_cast<u32>(z) << 4U) | static_cast<u32>(x));
}

// Which snapshot cells each neighbor fills and from which of its blocks, as
// flat index lists built at compile time. Walking the 1736 border cells
// through per neighbor loops over spans cost an index computation and a
// call per cell (2.7 us for an all air snapshot); with the lists, filling a
// uniform neighbor is one tight store loop and copying a mixed one a tight
// gather. Neighbor k is (dx, dy, dz) = (k % 3 - 1, k / 9 - 1, k / 3 % 3 - 1),
// the bit order of neighborBit(); its cells are begin[k] to begin[k + 1].
struct BorderTable {
    static constexpr usize kCells = static_cast<usize>(kSnapshotVolume - kChunkVolume);
    std::array<u16, kCells> destination{}; // Index into ChunkSnapshot::states.
    std::array<u16, kCells> source{};      // Block index inside the neighbor.
    std::array<u16, 28> begin{};
};

constexpr BorderTable makeBorderTable() noexcept {
    BorderTable table;
    usize cell = 0;
    for (i32 k = 0; k < 27; ++k) {
        table.begin.at(static_cast<usize>(k)) = static_cast<u16>(cell);
        if (k == 13) {
            continue; // The chunk itself.
        }
        const AxisSpan sx = axisSpan((k % 3) - 1);
        const AxisSpan sy = axisSpan((k / 9) - 1);
        const AxisSpan sz = axisSpan(((k / 3) % 3) - 1);
        for (i32 y = 0; y < sy.count; ++y) {
            for (i32 z = 0; z < sz.count; ++z) {
                for (i32 x = 0; x < sx.count; ++x) {
                    table.destination.at(cell) =
                        static_cast<u16>(ChunkSnapshot::index(sx.first + x, sy.first + y, sz.first + z));
                    table.source.at(cell) = blockIndexOf(sx.source + x, sy.source + y, sz.source + z);
                    ++cell;
                }
            }
        }
    }
    table.begin.back() = static_cast<u16>(cell);
    return table;
}

constexpr BorderTable kBorder = makeBorderTable();
static_assert(kBorder.begin.back() == BorderTable::kCells, "Every border cell belongs to one neighbor");

// Block heights are kept as i32 (see ColumnSummary); positions beyond that
// saturate, which no world height of Section 9.2 comes near.
constexpr i32 clampHeight(i64 blockY) noexcept {
    return static_cast<i32>(
        std::clamp<i64>(blockY, i64{std::numeric_limits<i32>::min()} + 1, std::numeric_limits<i32>::max()));
}

constexpr i64 chunkBase(i32 chunkY) noexcept {
    return static_cast<i64>(chunkY) * kChunkSize;
}

} // namespace

VoxelWorld::VoxelWorld(const VoxelWorldConfig& config, stats::StatRegistry* stats)
    : m_chunkPool(config.chunksPerPage)
    , m_columnPool(config.columnsPerPage)
    , m_stats(stats)
    , m_statIds{} {
    registerStats();
}

VoxelWorld::~VoxelWorld() {
    clear();
}

void VoxelWorld::registerStats() {
    if (m_stats == nullptr) {
        return;
    }
    const auto add = [this](const stats::StatDesc& desc) {
        auto id = m_stats->registerStat(desc);
        return id ? id.value() : stats::StatId{};
    };
    const auto gauge = [&](std::string_view name, std::string_view description,
                           stats::StatUnit unit = stats::StatUnit::Count) {
        return add({.name = name, .description = description, .type = stats::StatType::Gauge, .unit = unit});
    };
    m_statIds.chunks = gauge("Voxel.World.Chunks", "Loaded chunks.");
    m_statIds.uniform = gauge("Voxel.World.UniformChunks", "Loaded chunks of one state (16 bytes each).");
    m_statIds.palette[0] = gauge("Voxel.World.Palette1Chunks", "Loaded chunks with 1 bit palette indices.");
    m_statIds.palette[1] = gauge("Voxel.World.Palette2Chunks", "Loaded chunks with 2 bit palette indices.");
    m_statIds.palette[2] = gauge("Voxel.World.Palette4Chunks", "Loaded chunks with 4 bit palette indices.");
    m_statIds.palette[3] = gauge("Voxel.World.Palette8Chunks", "Loaded chunks with 8 bit palette indices.");
    m_statIds.raw = gauge("Voxel.World.RawChunks", "Loaded chunks with more than 256 states.");
    m_statIds.blockBytes =
        gauge("Voxel.World.BlockBytes", "Block storage of all loaded chunks.", stats::StatUnit::Bytes);
    m_statIds.fullLight = gauge("Voxel.World.FullLightChunks", "Loaded chunks whose light is not uniform.");
    m_statIds.lightBytes =
        gauge("Voxel.World.LightBytes", "Light storage of all loaded chunks.", stats::StatUnit::Bytes);
    m_statIds.columns = gauge("Voxel.World.Columns", "Chunk columns with a summary.");
    m_statIds.snapshots = add({.name = "Voxel.World.Snapshots",
                               .description = "Chunk snapshots created this frame.",
                               .type = stats::StatType::Counter});
    m_statIds.snapshotTime = add({.name = "Voxel.World.SnapshotTime",
                                  .description = "Time spent creating chunk snapshots this frame.",
                                  .type = stats::StatType::Timer,
                                  .unit = stats::StatUnit::Milliseconds});
}

void VoxelWorld::publishStats() {
    if (m_stats == nullptr) {
        return;
    }
    const auto set = [this](stats::StatId id, usize value) {
        if (id.isValid()) {
            m_stats->set(id, static_cast<f64>(value));
        }
    };
    set(m_statIds.chunks, m_chunks.size());
    set(m_statIds.uniform, m_counts.uniform);
    for (usize i = 0; i < m_counts.palette.size(); ++i) {
        set(m_statIds.palette.at(i), m_counts.palette.at(i));
    }
    set(m_statIds.raw, m_counts.raw);
    set(m_statIds.blockBytes, m_counts.blockBytes);
    set(m_statIds.fullLight, m_counts.fullLight);
    set(m_statIds.lightBytes, m_counts.lightBytes);
    set(m_statIds.columns, m_columns.size());
}

void VoxelWorld::account(const ChunkStorage& blocks, bool add) noexcept {
    usize* slot = &m_counts.uniform;
    if (blocks.mode() == ChunkStorage::Mode::Raw) {
        slot = &m_counts.raw;
    } else if (blocks.mode() == ChunkStorage::Mode::Palette) {
        // 1, 2, 4, 8 bits are slots 0 to 3.
        slot = &m_counts.palette.at(static_cast<usize>(std::countr_zero(blocks.bitsPerIndex())));
    }
    if (add) {
        ++*slot;
        m_counts.blockBytes += blocks.memoryBytes();
    } else {
        --*slot;
        m_counts.blockBytes -= blocks.memoryBytes();
    }
}

void VoxelWorld::accountLight(const ChunkLight& light, bool add) noexcept {
    const usize full = light.isUniform() ? 0 : 1;
    if (add) {
        m_counts.fullLight += full;
        m_counts.lightBytes += light.memoryBytes();
    } else {
        m_counts.fullLight -= full;
        m_counts.lightBytes -= light.memoryBytes();
    }
}

Chunk* VoxelWorld::findChunk(ChunkPos position) noexcept {
    const auto it = m_chunks.find(position);
    return it == m_chunks.end() ? nullptr : it->second;
}

const Chunk* VoxelWorld::findChunk(ChunkPos position) const noexcept {
    const auto it = m_chunks.find(position);
    return it == m_chunks.end() ? nullptr : it->second;
}

std::optional<ChunkPos> VoxelWorld::neighborPosition(ChunkPos position, i32 dx, i32 dy, i32 dz) noexcept {
    // Chunks at the very edge of the int32 grid have no neighbor beyond it;
    // computing one would overflow.
    const auto inGrid = [](i32 value, i32 offset) {
        const i64 moved = static_cast<i64>(value) + offset;
        return moved >= std::numeric_limits<i32>::min() && moved <= std::numeric_limits<i32>::max();
    };
    if (!inGrid(position.x, dx) || !inGrid(position.y, dy) || !inGrid(position.z, dz)) {
        return std::nullopt;
    }
    return position.offset(dx, dy, dz);
}

void VoxelWorld::markChanged(Chunk& chunk, u64 stamp) {
    const bool wasCurrent = !chunk.isDirty(kDirtyMesh);
    chunk.changed(stamp);
    if (wasCurrent) {
        m_changedChunks.push_back(chunk.position());
    }
}

void VoxelWorld::neighborChanged(ChunkPos position, i32 dx, i32 dy, i32 dz, u64 stamp) {
    if (const auto at = neighborPosition(position, dx, dy, dz)) {
        if (Chunk* found = findChunk(*at); found != nullptr) {
            markChanged(*found, stamp);
        }
    }
}

void VoxelWorld::neighborsChanged(ChunkPos position, u64 stamp) {
    for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (dx != 0 || dy != 0 || dz != 0) {
                    neighborChanged(position, dx, dy, dz, stamp);
                }
            }
        }
    }
}

Chunk& VoxelWorld::insertChunk(ChunkPos position, ChunkStorage blocks, ChunkLight light) {
    VXE_PROFILE_FUNCTION();
    const u64 stamp = ++m_changeCounter;
    if (Chunk* existing = findChunk(position); existing != nullptr) {
        // Replacing: heights in this chunk's range may go down as well as up.
        account(existing->m_blocks, false);
        accountLight(existing->m_light, false);
        existing->m_blocks = std::move(blocks);
        existing->m_light = std::move(light);
        account(existing->m_blocks, true);
        accountLight(existing->m_light, true);
        markChanged(*existing, stamp);
        neighborsChanged(position, stamp);
        ColumnSummary& column = *m_columns.find(columnOf(position))->second;
        const i64 top = chunkBase(position.y) + kChunkMask;
        const i64 bottom = chunkBase(position.y);
        for (u32 z = 0; z < 16; ++z) {
            for (u32 x = 0; x < 16; ++x) {
                const i32 highest = column.highestBlock(x, z);
                if (highest >= clampHeight(bottom) && highest <= clampHeight(top)) {
                    rescanColumn(column, x, z, clampHeight(top + 1));
                }
            }
        }
        raiseHeights(column, *existing);
        return *existing;
    }
    Chunk* chunk = m_chunkPool.create(position);
    chunk->m_blocks = std::move(blocks);
    chunk->m_light = std::move(light);
    chunk->changed(stamp);
    m_chunks.emplace(position, chunk);
    account(chunk->m_blocks, true);
    accountLight(chunk->m_light, true);
    addToColumn(*chunk);
    neighborsChanged(position, stamp);
    return *chunk;
}

bool VoxelWorld::removeChunk(ChunkPos position) {
    const auto it = m_chunks.find(position);
    if (it == m_chunks.end()) {
        return false;
    }
    Chunk* chunk = it->second;
    m_chunks.erase(it);
    removeFromColumn(position);
    neighborsChanged(position, ++m_changeCounter);
    destroyChunk(chunk);
    return true;
}

void VoxelWorld::destroyChunk(Chunk* chunk) noexcept {
    account(chunk->m_blocks, false);
    accountLight(chunk->m_light, false);
    m_chunkPool.destroy(chunk);
}

void VoxelWorld::clear() {
    for (auto& entry : m_chunks) {
        destroyChunk(entry.second);
    }
    m_chunks.clear();
    for (auto& entry : m_columns) {
        m_columnPool.destroy(entry.second);
    }
    m_columns.clear();
    m_changedChunks.clear();
}

void VoxelWorld::addToColumn(const Chunk& chunk) {
    const ColumnPos position = columnOf(chunk.position());
    auto [it, inserted] = m_columns.try_emplace(position, nullptr);
    if (inserted) {
        it->second = m_columnPool.create(position);
    }
    ColumnSummary& column = *it->second;
    const auto at = std::ranges::lower_bound(column.m_chunkYs, chunk.position().y);
    column.m_chunkYs.insert(at, chunk.position().y);
    raiseHeights(column, chunk);
}

void VoxelWorld::raiseHeights(ColumnSummary& column, const Chunk& chunk) {
    const i64 base = chunkBase(chunk.position().y);
    const i32 top = clampHeight(base + kChunkMask);
    const ChunkStorage& blocks = chunk.blocks();
    if (blocks.isUniform()) {
        if (blocks.uniformState() != kAirState) {
            for (i32& highest : column.m_highest) {
                highest = std::max(highest, top);
            }
        }
        return;
    }
    // Nothing to learn if every block column already reaches above this
    // chunk, which is the case for most chunks below the surface.
    if (std::ranges::min(column.m_highest) >= top) {
        return;
    }
    std::array<BlockStateId, kChunkVolume> states{};
    blocks.decompressTo(states);
    for (u32 z = 0; z < 16; ++z) {
        for (u32 x = 0; x < 16; ++x) {
            i32& highest = column.m_highest.at((z * 16) + x);
            for (i32 y = kChunkMask; y >= 0; --y) {
                if (states.at(blockIndexOf(static_cast<i32>(x), y, static_cast<i32>(z))) != kAirState) {
                    highest = std::max(highest, clampHeight(base + y));
                    break;
                }
            }
        }
    }
}

void VoxelWorld::removeFromColumn(ChunkPos position) {
    const auto it = m_columns.find(columnOf(position));
    VXE_ASSERT(it != m_columns.end(), "Every loaded chunk has a column");
    ColumnSummary& column = *it->second;
    const auto at = std::ranges::lower_bound(column.m_chunkYs, position.y);
    VXE_ASSERT(at != column.m_chunkYs.end() && *at == position.y, "Chunk missing from its column");
    column.m_chunkYs.erase(at);
    if (column.m_chunkYs.empty()) {
        m_columnPool.destroy(&column);
        m_columns.erase(it);
        return;
    }
    // Block columns whose highest block was in this chunk fall back to the
    // highest block below it. Nothing above can be higher, by definition.
    const i64 bottom = chunkBase(position.y);
    for (u32 z = 0; z < 16; ++z) {
        for (u32 x = 0; x < 16; ++x) {
            const i32 highest = column.highestBlock(x, z);
            if (highest >= clampHeight(bottom) && highest <= clampHeight(bottom + kChunkMask)) {
                rescanColumn(column, x, z, clampHeight(bottom));
            }
        }
    }
}

void VoxelWorld::rescanColumn(ColumnSummary& column, u32 x, u32 z, i32 belowBlockY) const noexcept {
    // Walks the loaded chunks of the column downwards from belowBlockY - 1
    // and stops at the first non air block. Uniform chunks answer at once.
    i32& highest = column.m_highest.at((z * 16) + x);
    highest = ColumnSummary::kNoBlock;
    const ColumnPos position = column.position();
    for (auto it = column.m_chunkYs.rbegin(); it != column.m_chunkYs.rend(); ++it) {
        const i64 base = chunkBase(*it);
        if (base >= belowBlockY) {
            continue;
        }
        const Chunk* chunk = findChunk({.x = position.x, .y = *it, .z = position.z});
        VXE_ASSERT(chunk != nullptr, "Column lists a chunk that is not loaded");
        const ChunkStorage& blocks = chunk->blocks();
        const i32 startY = static_cast<i32>(std::min<i64>(kChunkMask, belowBlockY - 1 - base));
        if (blocks.isUniform()) {
            if (blocks.uniformState() != kAirState) {
                highest = clampHeight(base + startY);
                return;
            }
            continue;
        }
        for (i32 y = startY; y >= 0; --y) {
            if (blocks.getIndex(blockIndexOf(static_cast<i32>(x), y, static_cast<i32>(z))) != kAirState) {
                highest = clampHeight(base + y);
                return;
            }
        }
    }
}

const ColumnSummary* VoxelWorld::findColumn(ColumnPos position) const noexcept {
    const auto it = m_columns.find(position);
    return it == m_columns.end() ? nullptr : it->second;
}

void VoxelWorld::setSurfaceEstimate(ColumnPos position, i32 blockY) noexcept {
    if (const auto it = m_columns.find(position); it != m_columns.end()) {
        it->second->m_surfaceEstimate = blockY;
    }
}

BlockStateId VoxelWorld::getBlock(const BlockPos& position, BlockStateId unloaded) const noexcept {
    const Chunk* chunk = findChunk(toChunk(position));
    return chunk == nullptr ? unloaded : chunk->blocks().get(toLocal(position));
}

bool VoxelWorld::setBlock(const BlockPos& position, BlockStateId state) {
    const ChunkPos chunkPosition = toChunk(position);
    Chunk* chunk = findChunk(chunkPosition);
    if (chunk == nullptr) {
        return false;
    }
    const LocalPos local = toLocal(position);
    if (chunk->m_blocks.get(local) == state) {
        return true;
    }
    const u64 stamp = ++m_changeCounter;
    account(chunk->m_blocks, false);
    chunk->m_blocks.set(local, state);
    account(chunk->m_blocks, true);
    markChanged(*chunk, stamp);
    chunk->m_dirty = static_cast<u8>(chunk->m_dirty | kDirtySave);
    borderChanged(chunkPosition, local, stamp);

    ColumnSummary& column = *m_columns.find(columnOf(chunkPosition))->second;
    const i32 y = clampHeight(position.y);
    i32& highest = column.m_highest.at((usize{local.z} * 16) + local.x);
    if (state != kAirState) {
        highest = std::max(highest, y);
    } else if (highest == y) {
        rescanColumn(column, local.x, local.z, y);
    }
    return true;
}

// A block on the chunk's border is in the snapshot borders of up to 7
// neighbors: across each face it touches, and across the edges and corner
// between those faces.
void VoxelWorld::borderChanged(ChunkPos chunkPosition, LocalPos local, u64 stamp) {
    const auto offsets = [](u8 value) {
        std::array<i32, 2> result{0, 0};
        if (value == 0) {
            result[1] = -1;
        } else if (value == kChunkMask) {
            result[1] = 1;
        }
        return result;
    };
    const std::array<i32, 2> xs = offsets(local.x);
    const std::array<i32, 2> ys = offsets(local.y);
    const std::array<i32, 2> zs = offsets(local.z);
    for (usize iy = 0; iy < (ys[1] != 0 ? 2U : 1U); ++iy) {
        for (usize iz = 0; iz < (zs[1] != 0 ? 2U : 1U); ++iz) {
            for (usize ix = 0; ix < (xs[1] != 0 ? 2U : 1U); ++ix) {
                if (ix == 0 && iy == 0 && iz == 0) {
                    continue;
                }
                neighborChanged(chunkPosition, xs.at(ix), ys.at(iy), zs.at(iz), stamp);
            }
        }
    }
}

u8 VoxelWorld::getLight(const BlockPos& position, u8 unloaded) const noexcept {
    const Chunk* chunk = findChunk(toChunk(position));
    return chunk == nullptr ? unloaded : chunk->light().get(toLocal(position));
}

bool VoxelWorld::setLight(const BlockPos& position, u8 packed) {
    const ChunkPos chunkPosition = toChunk(position);
    Chunk* chunk = findChunk(chunkPosition);
    if (chunk == nullptr) {
        return false;
    }
    const LocalPos local = toLocal(position);
    if (chunk->m_light.get(local) == packed) {
        return true;
    }
    const u64 stamp = ++m_changeCounter;
    accountLight(chunk->m_light, false);
    chunk->m_light.set(local, packed);
    accountLight(chunk->m_light, true);
    markChanged(*chunk, stamp);
    borderChanged(chunkPosition, local, stamp);
    return true;
}

bool VoxelWorld::replaceLight(ChunkPos position, ChunkLight light) {
    Chunk* chunk = findChunk(position);
    if (chunk == nullptr) {
        return false;
    }
    if (chunk->m_light == light) {
        return true;
    }
    const u64 stamp = ++m_changeCounter;
    // Neighbor k (bit order of neighborBit) reads this chunk's cells listed
    // for the opposite direction 26 - k in the border table.
    for (i32 k = 0; k < 27; ++k) {
        if (k == 13) {
            continue;
        }
        const auto opposite = static_cast<usize>(26 - k);
        bool differs = false;
        for (usize cell = kBorder.begin.at(opposite); cell < kBorder.begin.at(opposite + 1) && !differs;
             ++cell) {
            differs = chunk->m_light.getIndex(kBorder.source[cell]) != light.getIndex(kBorder.source[cell]);
        }
        if (differs) {
            neighborChanged(position, (k % 3) - 1, (k / 9) - 1, ((k / 3) % 3) - 1, stamp);
        }
    }
    accountLight(chunk->m_light, false);
    chunk->m_light = std::move(light);
    accountLight(chunk->m_light, true);
    markChanged(*chunk, stamp);
    return true;
}

bool VoxelWorld::createSnapshot(ChunkPos position, ChunkSnapshot& out, BlockStateId unloaded,
                                u8 unloadedLight) const {
    VXE_PROFILE_FUNCTION();
    const Stopwatch stopwatch;
    const Chunk* center = findChunk(position);
    if (center == nullptr) {
        return false;
    }
    out.position = position;
    out.version = center->version();
    out.neighbors = 0;
    std::array<const Chunk*, 27> around{};
    around[13] = center;
    for (i32 k = 0; k < 27; ++k) {
        if (k != 13) {
            const auto at = neighborPosition(position, (k % 3) - 1, (k / 9) - 1, ((k / 3) % 3) - 1);
            around.at(static_cast<usize>(k)) = at ? findChunk(*at) : nullptr;
            if (around.at(static_cast<usize>(k)) != nullptr) {
                out.neighbors |= 1U << static_cast<u32>(k);
            }
        }
    }
    snapshotBlocks(around, out, unloaded);
    snapshotLight(around, out, unloadedLight);
    if (m_stats != nullptr && m_statIds.snapshots.isValid() && m_statIds.snapshotTime.isValid()) {
        m_stats->add(m_statIds.snapshots);
        m_stats->addTime(m_statIds.snapshotTime, stopwatch.elapsed());
    }
    return true;
}

void VoxelWorld::snapshotBlocks(const std::array<const Chunk*, 27>& around, ChunkSnapshot& out,
                                BlockStateId unloaded) noexcept {
    BlockStateId* states = out.states.data();
    around[13]->blocks().decompressTo(std::span(out.states).subspan(ChunkSnapshot::index(0, 0, 0)),
                                      kSnapshotSize, kSnapshotArea);
    for (usize k = 0; k < 27; ++k) {
        if (k == 13) {
            continue;
        }
        const Chunk* found = around.at(k);
        const usize first = kBorder.begin.at(k);
        const usize last = kBorder.begin.at(k + 1);
        if (found == nullptr || found->blocks().isUniform()) {
            // One state for every cell: missing neighbors and the common all
            // air or all stone ones.
            const BlockStateId fill = found == nullptr ? unloaded : found->blocks().uniformState();
            for (usize cell = first; cell < last; ++cell) {
                states[kBorder.destination[cell]] = fill;
            }
        } else {
            const ChunkStorage& blocks = found->blocks();
            for (usize cell = first; cell < last; ++cell) {
                states[kBorder.destination[cell]] = blocks.getIndex(kBorder.source[cell]);
            }
        }
    }
}

// Most snapshots are lit the same everywhere (open sky above the ground,
// dark deep below), and then one fill of the whole box replaces 27 copies.
void VoxelWorld::snapshotLight(const std::array<const Chunk*, 27>& around, ChunkSnapshot& out,
                               u8 unloadedLight) noexcept {
    const auto uniformLight = [unloadedLight](const Chunk* chunk) -> i32 {
        if (chunk == nullptr) {
            return unloadedLight;
        }
        return chunk->light().isUniform() ? chunk->light().uniformValue() : -1;
    };
    const i32 shared = uniformLight(around[13]);
    if (shared >= 0 &&
        std::ranges::all_of(around, [&](const Chunk* chunk) { return uniformLight(chunk) == shared; })) {
        out.light.fill(static_cast<u8>(shared));
        return;
    }
    u8* light = out.light.data();
    around[13]->light().copyTo(std::span(out.light).subspan(ChunkSnapshot::index(0, 0, 0)), kSnapshotSize,
                               kSnapshotArea);
    for (usize k = 0; k < 27; ++k) {
        if (k == 13) {
            continue;
        }
        const Chunk* found = around.at(k);
        const usize first = kBorder.begin.at(k);
        const usize last = kBorder.begin.at(k + 1);
        if (found == nullptr || found->light().isUniform()) {
            const u8 fill = found == nullptr ? unloadedLight : found->light().uniformValue();
            for (usize cell = first; cell < last; ++cell) {
                light[kBorder.destination[cell]] = fill;
            }
        } else {
            const ChunkLight& neighborLight = found->light();
            for (usize cell = first; cell < last; ++cell) {
                light[kBorder.destination[cell]] = neighborLight.getIndex(kBorder.source[cell]);
            }
        }
    }
}

} // namespace vxe::voxel
