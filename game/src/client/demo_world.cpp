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

void DemoChunkSource::produce(const vxe::voxel::ChunkRequest& request, vxe::voxel::ChunkProduct& out) {
    // Generation writes every block, so a reused array per worker thread
    // needs no clearing and the job allocates nothing but the result.
    thread_local std::array<BlockStateId, vxe::kChunkVolume> states{};
    const vxe::ChunkPos position = request.position();
    if (m_saver == nullptr || !m_saver->load(position, out)) {
        m_terrain.generate(position, states);
        out.blocks.assign(states);
        out.origin = vxe::voxel::ChunkOrigin::Generated;
    }
    out.surfaceEstimate = static_cast<vxe::i32>(m_terrain.surfaceHeight(
        (vxe::i64{position.x} * vxe::kChunkSize) + 8, (vxe::i64{position.z} * vxe::kChunkSize) + 8));
}

std::optional<vxe::i32> DemoChunkSource::surfaceLayer(vxe::i32 chunkX, vxe::i32 chunkZ) const {
    const auto middle = [](vxe::i32 chunk) {
        return static_cast<vxe::f64>((vxe::i64{chunk} * vxe::kChunkSize) + 8);
    };
    const vxe::f64 height = std::max(m_terrain.roughGround(middle(chunkX), middle(chunkZ)).height,
                                     static_cast<vxe::f64>(Terrain::kSeaLevel));
    return static_cast<vxe::i32>(std::floor(height / vxe::kChunkSize));
}

DemoWorld::DemoWorld(const DemoWorldDesc& desc, const TerrainBlocks& blocks)
    : m_chunks(desc.chunks)
    , m_catalog(*desc.catalog)
    , m_terrain(blocks, desc.seed)
    , m_spawn(m_terrain.spawnColumn())
    , m_world({}, desc.stats)
    , m_source(m_terrain)
    , m_names(*desc.catalog)
    , m_lodRenderer(desc.lod)
    , m_debugDraw(desc.debugDraw)
    , m_view(desc.blocks->current())
    , m_lodSampler(m_terrain, *m_view, static_cast<vxe::u16>(desc.catalog->textureKeys().size()), desc.seed) {
    if (!desc.saveFolder.empty()) {
        m_saver.emplace(WorldSaverDesc{.folder = desc.saveFolder,
                                       .jobs = desc.jobs,
                                       .settings = desc.settings,
                                       .stats = desc.stats,
                                       .names = &m_names});
        m_source.setSaver(&*m_saver);
    }
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
    if (!desc.lodCacheFolder.empty()) {
        m_lodCache.emplace(vxe::voxel::LodTileCacheDesc{.folder = desc.lodCacheFolder, .stats = desc.stats});
    }
    m_lod = std::make_unique<vxe::voxel::LodManager>(
        vxe::voxel::LodManagerDesc{.jobs = desc.jobs,
                                   .settings = desc.settings,
                                   .sampler = &m_lodSampler,
                                   .cache = m_lodCache ? &*m_lodCache : nullptr,
                                   .sink = m_lodSink ? &*m_lodSink : nullptr,
                                   .stats = desc.stats});
    m_waitForLod = desc.premapBlocks < 0;
    if (m_lodCache && desc.premapBlocks != 0) {
        m_premapping = true;
        m_premapJobs = desc.jobs->workerCount();
        const vxe::DVec3 spawn = spawnPosition();
        m_premap = std::make_unique<vxe::voxel::LodPremapper>(vxe::voxel::LodPremapDesc{
            .jobs = desc.jobs,
            .sampler = &m_lodSampler,
            .cache = &*m_lodCache,
            .stats = desc.stats,
            .options = m_lod->sampleOptions(),
            .clipmap = m_lod->clipmapDesc(),
            .centerX = static_cast<vxe::i64>(spawn.x),
            .centerZ = static_cast<vxe::i64>(spawn.z),
            .radius = desc.premapBlocks < 0 ? vxe::i64{1} << 30U : desc.premapBlocks,
            // Beside the chunks, not ahead of them: a sampling
            // job runs for milliseconds, and with every worker
            // on one the ground under the player has to wait.
            .maxJobsInFlight = std::max(desc.jobs->workerCount() / 3, 2U),
            .priority = vxe::jobs::Priority::Low});
    }
    m_streaming =
        std::make_unique<vxe::voxel::StreamingManager>(vxe::voxel::StreamingDesc{.world = &m_world,
                                                                                 .source = &m_source,
                                                                                 .jobs = desc.jobs,
                                                                                 .settings = desc.settings,
                                                                                 .sink = chunkSink,
                                                                                 .blocks = desc.blocks,
                                                                                 .stats = desc.stats,
                                                                                 .variationSeed = desc.seed,
                                                                                 .minChunkY = kMinChunkY,
                                                                                 .maxChunkY = kMaxChunkY});
    m_streaming->startBurst();
}

vxe::Result<std::unique_ptr<DemoWorld>> DemoWorld::create(const DemoWorldDesc& desc) {
    VXE_ASSERT(desc.jobs != nullptr && desc.settings != nullptr && desc.catalog != nullptr &&
                   desc.blocks != nullptr,
               "DemoWorld needs jobs, settings and blocks");
    auto blocks = TerrainBlocks::fromCatalog(*desc.catalog);
    if (!blocks) {
        return std::move(blocks).error();
    }
    std::unique_ptr<DemoWorld> world(new DemoWorld(desc, blocks.value()));
    return world;
}

DemoWorld::~DemoWorld() {
    release();
}

void DemoWorld::setTextureColors(std::span<const vxe::u32> averageColors) {
    if (m_lodRenderer != nullptr) {
        // What each texture looks like from far away, plus the far terrain's
        // own blends, so far terrain matches the chunks (Section 13.2).
        m_lodRenderer->setMaterialColors(m_lodSampler.materialColors(m_catalog.shownColors(averageColors)));
    }
}

bool DemoWorld::setBlock(const vxe::BlockPos& position, BlockStateId state) {
    if (!m_world.setBlock(position, state)) {
        return false;
    }
    if (m_saver) {
        m_saver->markEdited(vxe::toChunk(position));
    }
    return true;
}

void DemoWorld::saveEdits() {
    if (m_saver) {
        m_saver->encodeEdited(m_world);
        m_saver->save();
    }
}

void DemoWorld::shutdown() {
    if (m_shutDown) {
        return;
    }
    if (m_saver) {
        m_saver->encodeEdited(m_world);
        m_saver->flush();
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
    m_premap.reset();
    m_lod.reset(); // Waits for its jobs; the renderer forgets the tiles below.
    if (m_lodRenderer != nullptr) {
        m_lodRenderer->clear();
    }
}

void DemoWorld::update(const vxe::render::Camera& camera) {
    if (m_shutDown) {
        return;
    }
    if (m_saver) {
        m_saver->encodeEdited(m_world); // Before streaming can unload the chunks.
    }
    m_streaming->update({.position = camera.position, .forward = camera.forward()});
    // Far terrain waits for the premap, so its tiles come from the cache.
    if (m_premap) {
        // With the ground under the player done, the workers are free for it.
        if (m_streaming->isAreaReady(std::min(kSpawnRadius, m_streaming->renderDistance()), kSpawnLayers)) {
            m_premap->setMaxJobs(m_premapJobs);
        }
        m_premap->update();
        if (m_premap->isDone()) {
            const vxe::voxel::LodPremapper::Progress progress = m_premap->progress();
            VXE_LOG_INFO(vxe::log::core, "Far terrain premapped after {:.0f} ms: {} tiles, {} already cached",
                         m_loadClock.elapsedMilliseconds(), progress.total, progress.skipped);
            m_premap.reset();
        }
    }
    if (!m_premap) {
        m_lod->update(camera.position);
    }
    if (m_debugDraw != nullptr) {
        m_lod->drawDebug(*m_debugDraw);
    }
    if (m_lodSink) {
        m_lodSink->update();
    }
    m_world.publishStats();
    if (m_loaded) {
        return;
    }
    if (!m_lodReady && !m_premap && m_lod->isSettled() && (!m_lodSink || m_lodSink->isIdle())) {
        m_lodReady = true;
        VXE_LOG_INFO(vxe::log::core, "Far terrain ready after {:.0f} ms: {} tiles",
                     m_loadClock.elapsedMilliseconds(), m_lod->counts().ready);
    }
    const vxe::i32 radius = std::min(kSpawnRadius, m_streaming->renderDistance());
    const bool farReady = !m_premap && (m_lodReady || !m_waitForLod || m_lodRenderer == nullptr);
    if (m_streaming->isAreaReady(radius, kSpawnLayers) && farReady) {
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
    if (m_premap) {
        const vxe::voxel::LodPremapper::Progress progress = m_premap->progress();
        return std::format("{}   far terrain premap {} of {}", m_streaming->summary(), progress.done,
                           progress.total);
    }
    return m_streaming->summary();
}

vxe::f32 DemoWorld::loadFraction() const {
    if (m_loaded) {
        return 1.0F;
    }
    // Chunks around the spawn are most of the wait; far terrain the rest.
    const vxe::i32 radius = std::min(kSpawnRadius, m_streaming->renderDistance());
    const vxe::f32 chunks = m_streaming->areaReadiness(radius, kSpawnLayers);
    if (!m_premapping && !m_waitForLod) {
        return chunks; // Nothing far to wait for.
    }
    vxe::f32 far = 1.0F;
    if (m_premap) {
        far = m_premap->progress().fraction() * (m_waitForLod ? 0.8F : 1.0F);
    } else if (m_waitForLod && !m_lodReady && m_lodRenderer != nullptr) {
        const vxe::voxel::LodManager::Counts counts = m_lod->counts();
        far =
            counts.wanted == 0
                ? 0.8F
                : 0.8F + (0.2F * static_cast<vxe::f32>(counts.ready) / static_cast<vxe::f32>(counts.wanted));
    }
    return (chunks * 0.7F) + (far * 0.3F);
}

std::string_view DemoWorld::loadStage() const {
    if (m_premap) {
        return "Mapping far terrain";
    }
    const vxe::i32 radius = std::min(kSpawnRadius, m_streaming->renderDistance());
    return m_streaming->areaReadiness(radius, kSpawnLayers) < 1.0F ? "Building terrain"
                                                                   : "Preparing the horizon";
}

vxe::DVec3 DemoWorld::spawnGround() const {
    return spawnPosition() - vxe::DVec3(0.0, 23.0, 0.0);
}

vxe::DVec3 DemoWorld::spawnPosition() const {
    return {static_cast<vxe::f64>(m_spawn.x) + 0.5, static_cast<vxe::f64>(m_spawn.y) + 24.0,
            static_cast<vxe::f64>(m_spawn.z) + 0.5};
}

} // namespace vr
