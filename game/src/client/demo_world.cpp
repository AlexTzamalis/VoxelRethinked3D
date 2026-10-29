#include "demo_world.hpp"

#include <vxe/core/log.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <string_view>
#include <utility>

namespace vr {

using vxe::usize;
using vxe::voxel::BlockStateId;

namespace {

constexpr vxe::u32 rgba(vxe::u32 red, vxe::u32 green, vxe::u32 blue, vxe::u32 alpha = 255) noexcept {
    return red | (green << 8U) | (blue << 16U) | (alpha << 24U);
}

// Placeholder colors by texture key, until the default texture pack (G2).
// Keys not listed show magenta, like a missing texture will.
vxe::u32 placeholderColor(std::string_view key) noexcept {
    constexpr std::array<std::pair<std::string_view, vxe::u32>, 17> kColors = {{
        {"stone", rgba(125, 125, 128)},
        {"dirt", rgba(121, 85, 58)},
        {"grass_top", rgba(95, 159, 53)},
        {"grass_side", rgba(110, 120, 60)},
        {"sand", rgba(220, 205, 150)},
        {"gravel", rgba(130, 122, 118)},
        {"water_still", rgba(38, 82, 170, 200)},
        {"oak_log_side", rgba(102, 81, 51)},
        {"oak_log_top", rgba(160, 130, 80)},
        {"oak_leaves", rgba(60, 120, 40, 255)},
        {"glass", rgba(200, 225, 235, 90)},
        {"lumen_block", rgba(250, 220, 140)},
        {"snow", rgba(240, 245, 250)},
        {"coal_ore", rgba(60, 60, 64)},
        {"iron_ore", rgba(170, 140, 115)},
        {"gold_ore", rgba(220, 190, 60)},
        {"foundation_stone", rgba(45, 45, 50)},
    }};
    for (const auto& [name, color] : kColors) {
        if (name == key) {
            return color;
        }
    }
    return rgba(255, 0, 255);
}

// What each worker thread keeps between jobs, so meshing does not allocate
// per chunk (Section 23.3).
struct WorkerScratch {
    std::array<BlockStateId, vxe::kChunkVolume> states{};
    vxe::voxel::Mesher mesher;
    std::unique_ptr<vxe::voxel::ChunkSnapshot> snapshot = std::make_unique<vxe::voxel::ChunkSnapshot>();
};

WorkerScratch& scratch() {
    thread_local WorkerScratch worker;
    return worker;
}

} // namespace

DemoWorld::DemoWorld(const DemoWorldDesc& desc, const DemoBlocks& blocks)
    : m_jobs(*desc.jobs)
    , m_device(desc.device)
    , m_chunks(desc.chunks)
    , m_catalog(*desc.catalog)
    , m_blocks(desc.blocks)
    , m_terrain(blocks, desc.seed) {
    for (vxe::i32 z = -desc.radiusChunks; z < desc.radiusChunks; ++z) {
        for (vxe::i32 x = -desc.radiusChunks; x < desc.radiusChunks; ++x) {
            for (vxe::i32 y = 0; y < desc.heightChunks; ++y) {
                m_positions.push_back({.x = x, .y = y, .z = z});
            }
        }
    }
    m_generated.resize(m_positions.size());
    m_meshes.resize(m_positions.size());
}

vxe::Result<std::unique_ptr<DemoWorld>> DemoWorld::create(const DemoWorldDesc& desc) {
    VXE_ASSERT(desc.jobs != nullptr && desc.catalog != nullptr && desc.blocks != nullptr,
               "DemoWorld needs jobs and blocks");
    auto blocks = DemoBlocks::fromCatalog(*desc.catalog);
    if (!blocks) {
        return std::move(blocks).error();
    }
    std::unique_ptr<DemoWorld> world(new DemoWorld(desc, blocks.value()));
    if (world->m_device != nullptr && world->m_chunks != nullptr) {
        VXE_TRY(world->createTextures());
    }
    // One job per chunk; each writes only its own slot of m_generated.
    for (usize index = 0; index < world->m_positions.size(); ++index) {
        world->m_jobs.schedule(world->m_counter, [self = world.get(), index] { self->generateChunk(index); });
    }
    return world;
}

DemoWorld::~DemoWorld() {
    shutdown();
}

vxe::Result<void> DemoWorld::createTextures() {
    std::vector<vxe::u32> colors;
    for (const std::string& key : m_catalog.textureKeys()) {
        colors.push_back(placeholderColor(key));
    }
    auto textures = vxe::render::createPlaceholderBlockTextures(*m_device, colors);
    if (!textures) {
        return std::move(textures).error();
    }
    m_textures = textures.value();
    m_chunks->setTextures(m_textures.texture, m_textures.sampler);
    return {};
}

void DemoWorld::shutdown() {
    m_jobs.wait(m_counter);
    if (m_device != nullptr) {
        if (m_chunks != nullptr) {
            m_chunks->setTextures({}, {});
        }
        if (m_textures.texture.isValid()) {
            m_device->destroy(m_textures.texture);
        }
        if (m_textures.sampler.isValid()) {
            m_device->destroy(m_textures.sampler);
        }
        m_textures = {};
    }
}

void DemoWorld::generateChunk(usize index) {
    WorkerScratch& worker = scratch();
    m_terrain.generate(m_positions[index], worker.states);
    m_generated[index].assign(worker.states);
}

void DemoWorld::meshChunk(usize index) {
    // The world does not change while meshing runs, so reading it from many
    // workers at once is safe.
    WorkerScratch& worker = scratch();
    if (m_world.createSnapshot(m_positions[index], *worker.snapshot)) {
        worker.mesher.mesh(*worker.snapshot, *m_blocks, {}, m_meshes[index]);
    }
}

void DemoWorld::startMeshing() {
    for (usize index = 0; index < m_positions.size(); ++index) {
        m_world.insertChunk(m_positions[index], std::move(m_generated[index]));
    }
    m_generated.clear();
    m_generated.shrink_to_fit();
    // Chunks of only air next to only air have nothing to draw; checking the
    // chunk alone is a cheap first filter, the mesher finds the rest.
    for (usize index = 0; index < m_positions.size(); ++index) {
        const vxe::voxel::ChunkStorage& blocks = m_world.findChunk(m_positions[index])->blocks();
        if (!blocks.isUniform() || blocks.uniformState() != vxe::voxel::kAirState) {
            m_meshOrder.push_back(index);
        }
    }
    // Nearest to the spawn first, so the view fills in from the middle.
    const vxe::ChunkPos center{.x = 0, .y = 4, .z = 0};
    std::ranges::sort(m_meshOrder, [&](usize a, usize b) {
        return vxe::chebyshevDistance(m_positions[a], center) <
               vxe::chebyshevDistance(m_positions[b], center);
    });
    for (const usize index : m_meshOrder) {
        m_jobs.schedule(m_counter, [this, index] { meshChunk(index); });
    }
}

void DemoWorld::advance() {
    if (!m_counter.isDone()) {
        return;
    }
    switch (m_phase) {
    case Phase::Generating:
        m_generateMs = m_stopwatch.elapsedMilliseconds();
        m_stopwatch.restart();
        startMeshing();
        m_phase = Phase::Meshing;
        break;
    case Phase::Meshing:
        m_meshMs = m_stopwatch.elapsedMilliseconds();
        m_phase = Phase::Uploading;
        VXE_LOG_INFO(vxe::log::core, "Demo world: {} chunks generated in {:.0f} ms, {} meshed in {:.0f} ms",
                     m_positions.size(), m_generateMs, m_meshOrder.size(), m_meshMs);
        break;
    case Phase::Uploading:
    case Phase::Ready:
        break;
    }
}

void DemoWorld::upload() {
    if (m_phase != Phase::Uploading) {
        return;
    }
    if (m_chunks == nullptr) {
        m_phase = Phase::Ready;
        return;
    }
    while (m_uploaded < m_meshOrder.size()) {
        const vxe::voxel::ChunkMesh& mesh = m_meshes[m_meshOrder[m_uploaded]];
        if (!mesh.empty()) {
            const vxe::render::ChunkGeometry geometry{.position = mesh.position,
                                                      .quads = std::as_bytes(std::span(mesh.quads)),
                                                      .groupBegin = mesh.groupBegin};
            if (m_chunks->upload(geometry) != vxe::render::ChunkRenderer::UploadResult::Uploaded) {
                return; // Budget used up; the rest goes in the next frames.
            }
        }
        ++m_uploaded;
    }
    m_meshes.clear();
    m_meshes.shrink_to_fit();
    m_phase = Phase::Ready;
    VXE_LOG_INFO(vxe::log::core, "Demo world on the GPU: {} chunks, {} quads", m_chunks->residentChunks(),
                 m_chunks->residentQuads());
}

std::string DemoWorld::progressText() const {
    switch (m_phase) {
    case Phase::Generating:
        return std::format("Generating {} chunks", m_positions.size());
    case Phase::Meshing:
        return std::format("Meshing {} chunks", m_meshOrder.size());
    case Phase::Uploading:
        return std::format("Uploading {} / {}", m_uploaded, m_meshOrder.size());
    case Phase::Ready:
        break;
    }
    return std::format("{} chunks resident", m_chunks != nullptr ? m_chunks->residentChunks() : 0);
}

vxe::DVec3 DemoWorld::spawnPosition() const {
    const auto ground = static_cast<vxe::f64>(m_terrain.surfaceHeight(8, 40));
    return {8.5, std::max(ground, static_cast<vxe::f64>(DemoTerrain::kSeaLevel)) + 24.0, 40.5};
}

} // namespace vr
