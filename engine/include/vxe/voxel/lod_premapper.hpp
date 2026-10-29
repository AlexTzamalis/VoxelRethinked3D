#pragma once

#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/voxel/lod_cache.hpp>
#include <vxe/voxel/lod_manager.hpp>
#include <vxe/voxel/lod_tile.hpp>

#include <atomic>
#include <memory>
#include <vector>

namespace vxe::voxel {

struct LodPremapDesc {
    jobs::JobSystem* jobs = nullptr;
    LodTileSampler* sampler = nullptr;
    LodTileCache* cache = nullptr;        // Where the tiles go (saves/<world>/lod).
    stats::StatRegistry* stats = nullptr; // Optional.
    LodSampleOptions options{};           // As the LodManager will ask (its settings).
    LodClipmapDesc clipmap{};             // The LodManager's clipmap shape (its settings).
    i64 centerX = 0;                      // The spawn, in blocks.
    i64 centerZ = 0;
    i64 radius = 16384; // Blocks: tiles starting within this distance of the center are baked.
    // Tiles in work at once. The default keeps every worker busy.
    u32 maxJobsInFlight = 64;
    jobs::Priority priority = jobs::Priority::Normal;
};

// Bakes the LOD tiles around a spawn into the cache ahead of time (Section
// 13.4, "Pre-map distance" at world creation), so the horizon is there on the
// first frame of the first visit. The tiles are the clipmap's around the
// spawn that start within the radius: exactly what the LodManager asks for
// when the player stands there. A radius beyond the clipmap's reach needs
// more levels (Graphics.LOD.Levels), or the outer tiles are never asked for.
//
// Each tile: an IO job checks whether the cache already holds it (current
// world hash), a worker samples it if not, and an IO job writes it. Tiles go
// nearest first, so cancelling early still leaves the near view baked.
//
// Progress: progress() any time; the loading screen shows done / total.
// Stats: Voxel.Lod.Premap.Done, .Skipped (already cached), .Total (gauges).
//
// Thread safety: start, update, cancel and the accessors on one thread (the
// main thread); the destructor cancels and waits for running jobs.
class LodPremapper {
public:
    explicit LodPremapper(const LodPremapDesc& desc);
    ~LodPremapper();
    LodPremapper(const LodPremapper&) = delete;
    LodPremapper& operator=(const LodPremapper&) = delete;
    LodPremapper(LodPremapper&&) = delete;
    LodPremapper& operator=(LodPremapper&&) = delete;

    // Starts more tiles as slots free up. Call every frame until isDone().
    void update();
    // Stops starting tiles; running ones finish (or stop early) by themselves.
    void cancel();

    struct Progress {
        usize total = 0;
        usize done = 0;    // Baked now or already in the cache.
        usize skipped = 0; // Of done, already in the cache.
        usize failed = 0;  // Could not be saved (see the log).
        [[nodiscard]] f32 fraction() const noexcept {
            return total == 0 ? 1.0F : static_cast<f32>(done) / static_cast<f32>(total);
        }
    };
    [[nodiscard]] Progress progress() const noexcept;
    [[nodiscard]] bool isDone() const noexcept;
    // The tiles, nearest first.
    [[nodiscard]] std::span<const LodTileKey> tiles() const noexcept { return m_tiles; }

private:
    struct Slot {
        LodTileKey key{};
        std::atomic<bool> done{false};
    };
    void runCheck(Slot& slot);
    void runSample(Slot& slot);
    void runSave(Slot& slot, const std::shared_ptr<const LodTile>& tile);
    void finish(Slot& slot, bool skipped, bool failed);

    jobs::JobSystem& m_jobs;
    LodTileSampler* m_sampler;
    LodTileCache* m_cache;
    stats::StatRegistry* m_stats;
    LodSampleOptions m_options;
    u64 m_hash;
    u32 m_maxJobs;
    jobs::Priority m_priority;

    std::vector<LodTileKey> m_tiles;
    usize m_next = 0;
    std::vector<std::unique_ptr<Slot>> m_slots;
    std::vector<Slot*> m_free;
    std::vector<Slot*> m_busy;
    std::atomic<bool> m_cancelled{false};
    std::atomic<usize> m_done{0};
    std::atomic<usize> m_skipped{0};
    std::atomic<usize> m_failed{0};
    jobs::JobCounter m_jobCounter;

    struct StatIds {
        stats::StatId done;
        stats::StatId skipped;
        stats::StatId total;
    } m_statIds;
};

} // namespace vxe::voxel
