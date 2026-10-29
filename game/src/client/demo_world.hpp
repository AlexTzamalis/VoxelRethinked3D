#pragma once

#include "common/block_catalog.hpp"
#include "worldgen/demo_terrain.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/core/types.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/chunk_renderer.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/voxel/block_registry.hpp>
#include <vxe/voxel/chunk_render_sink.hpp>
#include <vxe/voxel/chunk_source.hpp>
#include <vxe/voxel/streaming.hpp>
#include <vxe/voxel/voxel_world.hpp>

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
    vxe::rhi::Device* device = nullptr;           // Optional: no textures and no drawing without it.
    vxe::render::ChunkRenderer* chunks = nullptr; // Optional, with the device.
    vxe::u64 seed = 12345;
};

// Makes the demo terrain's chunks for the streaming manager, on its jobs.
class DemoChunkSource final : public vxe::voxel::ChunkSource {
public:
    explicit DemoChunkSource(const DemoTerrain& terrain) noexcept
        : m_terrain(terrain) {}
    void produce(const vxe::voxel::ChunkRequest& request, vxe::voxel::ChunkProduct& out) override;

private:
    const DemoTerrain& m_terrain;
};

// The temporary demo world agreed for the first playable build (until the
// integrated server, G0.2, and world generation, G3, replace it): the demo
// terrain streamed around the camera without end on X and Z by the engine's
// StreamingManager (E9.7), 8 chunk layers tall (blocks 0 to 127).
//
// Entering it waits until every chunk within 8 chunks of the spawn is
// meshed (the "playable" radius of Section 2, at most the render distance),
// with streaming in burst mode; the rest fills in while playing.
//
// Textures are flat placeholder colors per texture key until the default
// texture pack (G2).
//
// Thread safety: main thread only. Call shutdown() before the job system
// and the renderer go away.
class DemoWorld {
public:
    static constexpr vxe::i32 kMinChunkY = 0;
    static constexpr vxe::i32 kMaxChunkY = 7;
    // Chunks around the spawn that must be meshed before the world shows.
    static constexpr vxe::i32 kSpawnRadius = 8;
    // Shows the world anyway after this long, for example when the renderer
    // refused chunks.
    static constexpr vxe::f64 kLoadTimeoutMs = 20000.0;

    [[nodiscard]] static vxe::Result<std::unique_ptr<DemoWorld>> create(const DemoWorldDesc& desc);
    ~DemoWorld();
    DemoWorld(const DemoWorld&) = delete;
    DemoWorld& operator=(const DemoWorld&) = delete;
    DemoWorld(DemoWorld&&) = delete;
    DemoWorld& operator=(DemoWorld&&) = delete;

    // Streams around the camera. Call once per frame, after the renderer's
    // beginFrame (uploads need its staging memory).
    void update(const vxe::render::Camera& camera);
    // Takes the meshes back from the renderer, waits for jobs and releases
    // the textures. Logs the streaming time per frame.
    void shutdown();

    // The spawn area is ready (or loading took too long).
    [[nodiscard]] bool isLoaded() const noexcept { return m_loaded; }
    [[nodiscard]] std::string progressText() const;
    // A spot above the ground near the origin, looking across the terrain.
    [[nodiscard]] vxe::DVec3 spawnPosition() const;
    [[nodiscard]] const vxe::voxel::VoxelWorld& world() const noexcept { return m_world; }
    [[nodiscard]] const vxe::voxel::StreamingManager& streaming() const noexcept { return *m_streaming; }

private:
    explicit DemoWorld(const DemoWorldDesc& desc, const DemoBlocks& blocks);
    [[nodiscard]] vxe::Result<void> createTextures();
    // shutdown() without its log line, for the destructor.
    void release() noexcept;

    vxe::rhi::Device* m_device;
    vxe::render::ChunkRenderer* m_chunks;
    const BlockCatalog& m_catalog;
    DemoTerrain m_terrain;
    vxe::render::BlockTextures m_textures{};
    vxe::voxel::VoxelWorld m_world;
    DemoChunkSource m_source;
    std::optional<vxe::voxel::ChunkRenderSink> m_sink;
    std::unique_ptr<vxe::voxel::StreamingManager> m_streaming; // Last: it waits for its jobs first.
    vxe::Stopwatch m_loadClock;
    bool m_loaded = false;
    bool m_shutDown = false;
};

} // namespace vr
