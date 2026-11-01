#pragma once

#include "common/block_catalog.hpp"
#include "common/catalog_state_names.hpp"
#include "server/world_saver.hpp"
#include "worldgen/terrain.hpp"
#include "worldgen/terrain_lod_sampler.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/core/types.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/chunk_renderer.hpp>
#include <vxe/render/debug_draw.hpp>
#include <vxe/render/lod_renderer.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/voxel/block_registry.hpp>
#include <vxe/voxel/chunk_render_sink.hpp>
#include <vxe/voxel/chunk_source.hpp>
#include <vxe/voxel/lod_cache.hpp>
#include <vxe/voxel/lod_manager.hpp>
#include <vxe/voxel/lod_premapper.hpp>
#include <vxe/voxel/lod_render_sink.hpp>
#include <vxe/voxel/streaming.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace vr {

struct DemoWorldDesc {
    vxe::jobs::JobSystem* jobs = nullptr;
    vxe::config::SettingsRegistry* settings = nullptr;
    const BlockCatalog* catalog = nullptr;
    const vxe::voxel::BlockRegistry* blocks = nullptr;
    vxe::stats::StatRegistry* stats = nullptr;    // Optional.
    vxe::render::ChunkRenderer* chunks = nullptr; // Optional, with the device.
    vxe::render::LodRenderer* lod = nullptr;      // Optional, with the device: far terrain.
    vxe::render::DebugDraw* debugDraw = nullptr;  // Optional: far terrain debug views.
    vxe::u64 seed = 12345;
    // Optional: where far terrain tiles are cached (saves:/demo/lod). With it,
    // the tiles around the spawn are premapped before the world shows, so the
    // horizon is there on the first frame (Section 13.4).
    std::filesystem::path lodCacheFolder{};
    // Optional: where edited chunks are saved (saves/<world>/regions). Without
    // it edits last until the world closes.
    std::filesystem::path saveFolder{};
};

// Makes the demo terrain's chunks for the streaming manager, on its jobs.
class DemoChunkSource final : public vxe::voxel::ChunkSource {
public:
    explicit DemoChunkSource(const Terrain& terrain) noexcept
        : m_terrain(terrain) {}
    // Saved chunks then win over generated ones. Optional; set before streaming.
    void setSaver(WorldSaver* saver) noexcept { m_saver = saver; }
    void produce(const vxe::voxel::ChunkRequest& request, vxe::voxel::ChunkProduct& out) override;
    [[nodiscard]] std::optional<vxe::i32> surfaceLayer(vxe::i32 chunkX, vxe::i32 chunkZ) const override;

private:
    const Terrain& m_terrain;
    WorldSaver* m_saver = nullptr;
};

// The temporary demo world agreed for the first playable build (until the
// integrated server, G0.2, and world generation, G3, replace it): the demo
// terrain streamed around the camera without end on X and Z by the engine's
// StreamingManager (E9.7), 256 chunk layers tall (blocks -1024 to 3071, the Tall height preset).
//
// Entering it waits until every chunk within 8 chunks of the spawn is
// meshed (the "playable" radius of Section 2, at most the render distance),
// with streaming in burst mode; the rest fills in while playing.
//
// Block textures come from the app's texture packs; setTextureColors()
// passes their average colors on to the far terrain.
//
// Far terrain (Section 13) comes from TerrainLodSampler through the engine's
// LodManager, drawn by the renderer's LodRenderer and hidden over chunk
// columns streaming has meshed. With a cache folder, the clipmap around the
// spawn is premapped (LodPremapper) while loading, and entering also waits
// for every far terrain tile, so the first frame shows the whole horizon.
//
// Thread safety: main thread only. Call shutdown() before the job system
// and the renderer go away.
class DemoWorld {
public:
    static constexpr vxe::i32 kMinChunkY = -64;
    static constexpr vxe::i32 kMaxChunkY = 191;
    // Chunks around the spawn that must be meshed before the world shows.
    static constexpr vxe::i32 kSpawnRadius = 8;
    // Chunk layers above and below the player that must be ready. The rest of
    // the vertical range is mostly rock nobody can see yet.
    static constexpr vxe::i32 kSpawnLayers = 4;
    // Shows the world anyway after this long, for example when the renderer
    // refused chunks.
    static constexpr vxe::f64 kLoadTimeoutMs = 20000.0;

    [[nodiscard]] static vxe::Result<std::unique_ptr<DemoWorld>> create(const DemoWorldDesc& desc);
    ~DemoWorld();
    DemoWorld(const DemoWorld&) = delete;
    DemoWorld& operator=(const DemoWorld&) = delete;
    DemoWorld(DemoWorld&&) = delete;
    DemoWorld& operator=(DemoWorld&&) = delete;

    // The average color of each texture layer (render::BlockTextures), for
    // the far terrain's color table. Call whenever the textures change.
    void setTextureColors(std::span<const vxe::u32> averageColors);

    // Streams around the camera. Call once per frame, after the renderer's
    // beginFrame (uploads need its staging memory).
    void update(const vxe::render::Camera& camera);
    // Takes the meshes back from the renderer, waits for jobs and releases
    // the textures. Saves every edit first. Logs the streaming time per frame.
    void shutdown();

    // Changes a block and marks its chunk for saving. All edits of the world
    // go through here, so the integrated server (G0.2) has one place to take
    // over. False if the chunk is not loaded.
    bool setBlock(const vxe::BlockPos& position, vxe::voxel::BlockStateId state);
    // Commits edits in the background (autosave). The world stays playable.
    void saveEdits();

    // The spawn area is ready (or loading took too long).
    [[nodiscard]] bool isLoaded() const noexcept { return m_loaded; }
    [[nodiscard]] std::string progressText() const;
    [[nodiscard]] const Terrain& terrain() const noexcept { return m_terrain; }
    // A spot above dry land near the origin, looking across the terrain.
    [[nodiscard]] vxe::DVec3 spawnPosition() const;
    // Where a new player's feet stand: on the ground or the water at the spawn.
    [[nodiscard]] vxe::DVec3 spawnGround() const;
    [[nodiscard]] const vxe::voxel::VoxelWorld& world() const noexcept { return m_world; }
    [[nodiscard]] const vxe::voxel::StreamingManager& streaming() const noexcept { return *m_streaming; }

private:
    explicit DemoWorld(const DemoWorldDesc& desc, const TerrainBlocks& blocks);

    // shutdown() without its log line, for the destructor.
    void release() noexcept;

    vxe::render::ChunkRenderer* m_chunks;
    const BlockCatalog& m_catalog;
    Terrain m_terrain;
    vxe::BlockPos m_spawn;
    vxe::voxel::VoxelWorld m_world;
    DemoChunkSource m_source;
    vxe::u32 m_premapJobs = 1;
    CatalogStateNames m_names;
    std::optional<WorldSaver> m_saver;
    std::optional<vxe::voxel::ChunkRenderSink> m_sink;
    vxe::render::LodRenderer* m_lodRenderer;
    vxe::render::DebugDraw* m_debugDraw;
    std::shared_ptr<const vxe::voxel::BlockRegistryView> m_view;
    TerrainLodSampler m_lodSampler;
    std::optional<vxe::voxel::LodRenderSink> m_lodSink;
    std::optional<vxe::voxel::CoveringChunkSink> m_coveringSink;
    std::optional<vxe::voxel::LodTileCache> m_lodCache;
    std::unique_ptr<vxe::voxel::LodManager> m_lod;
    std::unique_ptr<vxe::voxel::LodPremapper> m_premap; // While loading, with a cache.
    bool m_lodReady = false;
    std::unique_ptr<vxe::voxel::StreamingManager> m_streaming; // Last: it waits for its jobs first.
    vxe::Stopwatch m_loadClock;
    bool m_loaded = false;
    bool m_shutDown = false;
};

} // namespace vr
