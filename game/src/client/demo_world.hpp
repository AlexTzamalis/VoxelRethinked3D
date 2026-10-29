#pragma once

#include "common/block_catalog.hpp"
#include "worldgen/demo_terrain.hpp"

#include <vxe/core/result.hpp>
#include <vxe/core/time.hpp>
#include <vxe/core/types.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/chunk_renderer.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/voxel/block_registry.hpp>
#include <vxe/voxel/chunk_storage.hpp>
#include <vxe/voxel/mesher.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <memory>
#include <string>
#include <vector>

namespace vr {

struct DemoWorldDesc {
    vxe::jobs::JobSystem* jobs = nullptr;
    const BlockCatalog* catalog = nullptr;
    std::shared_ptr<const vxe::voxel::BlockRegistryView> blocks;
    vxe::rhi::Device* device = nullptr;           // Optional: no textures and no drawing without it.
    vxe::render::ChunkRenderer* chunks = nullptr; // Optional, with the device.
    vxe::u64 seed = 12345;
    vxe::i32 radiusChunks = 16; // The field spans 2 * radius chunks on X and Z around the origin.
    vxe::i32 heightChunks = 8;  // Chunk layers from y = 0 (blocks 0 to 16 * height - 1).
};

// The temporary demo world agreed for the first playable build (until the
// integrated server, G3 world generation and E9.7 streaming replace it): a
// fixed field of chunks around the origin, generated and meshed on the job
// system while the game shows LoadingWorld, then uploaded to the chunk
// renderer within the per frame budget, nearest chunks first.
//
// Phases: Generating (one job per chunk), Meshing (one job per chunk with
// anything to draw; the world is not changed meanwhile, so the jobs take
// their snapshots from it directly), Uploading, Ready.
//
// Textures are flat placeholder colors per texture key until the default
// texture pack (G2).
//
// Thread safety: main thread only; its jobs touch only their own slots.
// Call shutdown() before the job system goes away.
class DemoWorld {
public:
    enum class Phase : vxe::u8 {
        Generating,
        Meshing,
        Uploading,
        Ready,
    };

    [[nodiscard]] static vxe::Result<std::unique_ptr<DemoWorld>> create(const DemoWorldDesc& desc);
    ~DemoWorld();
    DemoWorld(const DemoWorld&) = delete;
    DemoWorld& operator=(const DemoWorld&) = delete;
    DemoWorld(DemoWorld&&) = delete;
    DemoWorld& operator=(DemoWorld&&) = delete;

    // Moves to the next phase when the current one's jobs are done.
    void advance();
    // Uploads finished meshes until the frame's budget is used. Call after
    // the renderer's beginFrame.
    void upload();
    // Waits for running jobs and releases the textures.
    void shutdown();

    [[nodiscard]] Phase phase() const noexcept { return m_phase; }
    // Generated and meshed: the world can be entered; uploads may go on.
    [[nodiscard]] bool isLoaded() const noexcept { return m_phase >= Phase::Uploading; }
    [[nodiscard]] std::string progressText() const;
    // A spot above the middle of the field, looking across it.
    [[nodiscard]] vxe::DVec3 spawnPosition() const;
    [[nodiscard]] const vxe::voxel::VoxelWorld& world() const noexcept { return m_world; }

private:
    explicit DemoWorld(const DemoWorldDesc& desc, const DemoBlocks& blocks);
    void generateChunk(vxe::usize index);
    void meshChunk(vxe::usize index);
    void startMeshing();
    [[nodiscard]] vxe::Result<void> createTextures();

    vxe::jobs::JobSystem& m_jobs;
    vxe::rhi::Device* m_device;
    vxe::render::ChunkRenderer* m_chunks;
    const BlockCatalog& m_catalog;
    std::shared_ptr<const vxe::voxel::BlockRegistryView> m_blocks;
    DemoTerrain m_terrain;
    vxe::render::BlockTextures m_textures{};

    Phase m_phase = Phase::Generating;
    vxe::jobs::JobCounter m_counter;
    std::vector<vxe::ChunkPos> m_positions;
    std::vector<vxe::voxel::ChunkStorage> m_generated;
    std::vector<vxe::usize> m_meshOrder; // Chunks with something to mesh, nearest first.
    std::vector<vxe::voxel::ChunkMesh> m_meshes;
    vxe::usize m_uploaded = 0;
    vxe::voxel::VoxelWorld m_world;
    vxe::Stopwatch m_stopwatch;
    vxe::f64 m_generateMs = 0.0;
    vxe::f64 m_meshMs = 0.0;
};

} // namespace vr
