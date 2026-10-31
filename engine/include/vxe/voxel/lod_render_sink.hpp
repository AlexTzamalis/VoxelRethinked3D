#pragma once

#include <vxe/core/containers.hpp>
#include <vxe/core/types.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/render/lod_renderer.hpp>
#include <vxe/voxel/lod_manager.hpp>
#include <vxe/voxel/lod_mesher.hpp>
#include <vxe/voxel/streaming.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <atomic>
#include <deque>
#include <memory>
#include <vector>

namespace vxe::voxel {

// A packed tile key, unique per tile, for the renderer.
[[nodiscard]] u64 lodTileId(const LodTileKey& key) noexcept;

// The usual LOD tile sink in a game client: meshes each tile on a worker
// (LodMesher), uploads the meshes to a LodRenderer within its per frame
// budget, and keeps the renderer's coverage map up to date from the chunks
// streaming meshes (chunkMeshed, fed by CoveringChunkSink).
//
// Coverage: a chunk column counts as drawn by real chunks when every chunk
// from its lowest to its highest surface block (column summary) and the
// chunk above are meshed. That is where far terrain and real terrain would
// overlap; deeper chunks do not matter, since far terrain only shows
// surfaces.
//
// Thread safety: main thread only; mesh jobs work on slots it hands out.
// The destructor cancels and waits for them.
class LodRenderSink final : public LodTileSink {
public:
    // `world` is optional: without it there is no coverage.
    LodRenderSink(render::LodRenderer& renderer, jobs::JobSystem& jobs, const VoxelWorld* world = nullptr);
    ~LodRenderSink() override;
    LodRenderSink(const LodRenderSink&) = delete;
    LodRenderSink& operator=(const LodRenderSink&) = delete;
    LodRenderSink(LodRenderSink&&) = delete;
    LodRenderSink& operator=(LodRenderSink&&) = delete;

    void addTile(std::shared_ptr<const LodTile> tile) override;
    void removeTile(const LodTileKey& key) override;

    // Takes finished meshes, uploads what the budget allows and updates the
    // coverage map. Once per frame, after the renderer's beginFrame.
    void update();

    // A chunk's mesh is now drawn (or no longer).
    void chunkMeshed(ChunkPos position, bool meshed);

    // No tile is being meshed or waiting for upload.
    [[nodiscard]] bool isIdle() const noexcept { return m_busy.empty() && m_ready.empty(); }

private:
    struct Slot {
        u64 id = 0;
        std::shared_ptr<const LodTile> tile;
        LodMesh mesh;
        std::atomic<bool> done{false};
        std::atomic<bool> cancelled{false};
    };
    struct ColumnHash {
        [[nodiscard]] u64 operator()(const ColumnPos& column) const noexcept {
            return hashCombine(static_cast<u32>(column.x), static_cast<u32>(column.z));
        }
    };

    void collect();
    void uploadReady();
    void updateCoverage();
    void updateColumn(ColumnPos column);
    void freeSlot(Slot* slot);

    render::LodRenderer& m_renderer;
    jobs::JobSystem& m_jobs;
    const VoxelWorld* m_world;
    std::vector<std::unique_ptr<Slot>> m_slots;
    std::vector<Slot*> m_free;
    std::vector<Slot*> m_busy;
    std::deque<Slot*> m_ready;     // Meshed, waiting for upload budget.
    HashMap<u64, Slot*> m_pending; // The newest slot of each tile not yet uploaded.
    jobs::JobCounter m_jobCounter;

    HashSet<ChunkPos> m_meshed;
    HashSet<ColumnPos, ColumnHash> m_dirtyColumns;
};

// Passes chunk meshes on to another sink (the chunk renderer's) and tells a
// LodRenderSink which chunks are drawn, for the coverage map.
class CoveringChunkSink final : public ChunkMeshSink {
public:
    CoveringChunkSink(ChunkMeshSink& inner, LodRenderSink& lod) noexcept
        : m_inner(inner)
        , m_lod(lod) {}

    [[nodiscard]] Result upload(const ChunkMesh& mesh) override;
    void remove(ChunkPos position) override;
    void setBudgetScale(f32 scale) override { m_inner.setBudgetScale(scale); }

private:
    ChunkMeshSink& m_inner;
    LodRenderSink& m_lod;
};

} // namespace vxe::voxel
