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

} // namespace

void DemoChunkSource::produce(const vxe::voxel::ChunkRequest& request, vxe::voxel::ChunkProduct& out) {
    // Generation writes every block, so a reused array per worker thread
    // needs no clearing and the job allocates nothing but the result.
    thread_local std::array<BlockStateId, vxe::kChunkVolume> states{};
    const vxe::ChunkPos position = request.position();
    m_terrain.generate(position, states);
    out.blocks.assign(states);
    out.surfaceEstimate = static_cast<vxe::i32>(m_terrain.surfaceHeight(
        (vxe::i64{position.x} * vxe::kChunkSize) + 8, (vxe::i64{position.z} * vxe::kChunkSize) + 8));
}

DemoWorld::DemoWorld(const DemoWorldDesc& desc, const DemoBlocks& blocks)
    : m_device(desc.device)
    , m_chunks(desc.chunks)
    , m_catalog(*desc.catalog)
    , m_terrain(blocks, desc.seed)
    , m_world({}, desc.stats)
    , m_source(m_terrain)
    , m_lodRenderer(desc.lod)
    , m_view(desc.blocks->current())
    , m_lodSampler(m_terrain, blocks, *m_view, desc.seed) {
    if (m_chunks != nullptr) {
        m_sink.emplace(*m_chunks);
    }
    vxe::voxel::ChunkMeshSink* chunkSink = m_sink ? &*m_sink : nullptr;
    if (m_lodRenderer != nullptr) {
        m_lodSink.emplace(*m_lodRenderer, *desc.jobs, &m_world);
        if (chunkSink != nullptr) {
            m_coveringSink.emplace(*chunkSink, *m_lodSink);
            chunkSink = &*m_coveringSink;
        }
    }
    m_lod = std::make_unique<vxe::voxel::LodManager>(
        vxe::voxel::LodManagerDesc{.jobs = desc.jobs,
                                   .settings = desc.settings,
                                   .sampler = &m_lodSampler,
                                   .sink = m_lodSink ? &*m_lodSink : nullptr,
                                   .stats = desc.stats});
    m_streaming =
        std::make_unique<vxe::voxel::StreamingManager>(vxe::voxel::StreamingDesc{.world = &m_world,
                                                                                 .source = &m_source,
                                                                                 .jobs = desc.jobs,
                                                                                 .settings = desc.settings,
                                                                                 .sink = chunkSink,
                                                                                 .blocks = desc.blocks,
                                                                                 .stats = desc.stats,
                                                                                 .minChunkY = kMinChunkY,
                                                                                 .maxChunkY = kMaxChunkY});
    m_streaming->startBurst();
}

vxe::Result<std::unique_ptr<DemoWorld>> DemoWorld::create(const DemoWorldDesc& desc) {
    VXE_ASSERT(desc.jobs != nullptr && desc.settings != nullptr && desc.catalog != nullptr &&
                   desc.blocks != nullptr,
               "DemoWorld needs jobs, settings and blocks");
    auto blocks = DemoBlocks::fromCatalog(*desc.catalog);
    if (!blocks) {
        return std::move(blocks).error();
    }
    std::unique_ptr<DemoWorld> world(new DemoWorld(desc, blocks.value()));
    if (world->m_device != nullptr && world->m_chunks != nullptr) {
        VXE_TRY(world->createTextures());
    }
    return world;
}

DemoWorld::~DemoWorld() {
    release();
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
    if (m_lodRenderer != nullptr) {
        m_lodRenderer->setMaterialColors(colors);
    }
    return {};
}

void DemoWorld::shutdown() {
    if (m_shutDown) {
        return;
    }
    const vxe::voxel::StreamingManager::UpdateTimes& times = m_streaming->updateTimes();
    if (times.updates > 0) {
        VXE_LOG_INFO(
            vxe::log::core,
            "Streaming main thread time outside burst mode: {} updates, {:.3f} ms average, {:.3f} ms "
            "max, {} over 1 ms",
            times.updates, times.totalMilliseconds / static_cast<vxe::f64>(times.updates),
            times.maxMilliseconds, times.overOneMillisecond);
    }
    release();
}

void DemoWorld::release() noexcept {
    if (m_shutDown) {
        return;
    }
    m_shutDown = true;
    m_streaming->unloadAll();
    m_lod.reset(); // Waits for its jobs; the renderer forgets the tiles below.
    if (m_lodRenderer != nullptr) {
        m_lodRenderer->clear();
    }
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

void DemoWorld::update(const vxe::render::Camera& camera) {
    if (m_shutDown) {
        return;
    }
    m_streaming->update({.position = camera.position, .forward = camera.forward()});
    m_lod->update(camera.position);
    if (m_lodSink) {
        m_lodSink->update();
    }
    m_world.publishStats();
    if (m_loaded) {
        return;
    }
    const vxe::i32 radius = std::min(kSpawnRadius, m_streaming->renderDistance());
    if (m_streaming->isAreaReady(radius)) {
        m_loaded = true;
        VXE_LOG_INFO(vxe::log::core, "Demo world playable after {:.0f} ms: {} chunks loaded, {} meshed",
                     m_loadClock.elapsedMilliseconds(), m_streaming->counts().loaded,
                     m_streaming->counts().meshed);
    } else if (m_loadClock.elapsedMilliseconds() > kLoadTimeoutMs) {
        m_loaded = true;
        VXE_LOG_WARN(vxe::log::core,
                     "Demo world: the spawn area was not ready after {:.0f} ms, entering anyway",
                     kLoadTimeoutMs);
    }
}

std::string DemoWorld::progressText() const {
    return m_streaming->summary();
}

vxe::DVec3 DemoWorld::spawnPosition() const {
    const auto ground = static_cast<vxe::f64>(m_terrain.surfaceHeight(8, 40));
    return {8.5, std::max(ground, static_cast<vxe::f64>(DemoTerrain::kSeaLevel)) + 24.0, 40.5};
}

} // namespace vr
