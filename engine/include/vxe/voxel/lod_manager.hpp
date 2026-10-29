#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/containers.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/voxel/lod_cache.hpp>
#include <vxe/voxel/lod_tile.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace vxe::voxel {

// What a sampler is asked for, besides the tile.
struct LodSampleOptions {
    u32 maxSpansPerCell = 4; // Graphics.LOD.MaxSpansPerCell: keep at most this many spans per cell.
    bool caves = false;      // Graphics.LOD.CavesInLod: carve caves into the spans.
};

// Turns the world generator into LOD tiles (Section 13.4): the game's column
// sampler (G3.6) evaluates the same terrain functions as the chunk generator
// at the tile's cell spacing. The engine only schedules it.
//
// Thread safety: sampleTile() is called on worker threads, several tiles at
// the same time, so it must not touch shared mutable state.
class LodTileSampler {
public:
    LodTileSampler() = default;
    virtual ~LodTileSampler() = default;
    LodTileSampler(const LodTileSampler&) = delete;
    LodTileSampler& operator=(const LodTileSampler&) = delete;
    LodTileSampler(LodTileSampler&&) = delete;
    LodTileSampler& operator=(LodTileSampler&&) = delete;

    // Adds all 4096 cells of the tile to `builder`, in index order
    // (cz * 64 + cx; cell (cx, cz) covers blocks origin + c * cellSize to
    // origin + (c + 1) * cellSize - 1), at most options.maxSpansPerCell spans
    // each. Returns false to give up, for example when `cancelled` turned
    // true (the tile left the view); the builder is then thrown away.
    [[nodiscard]] virtual bool sampleTile(const LodTileKey& key, const LodSampleOptions& options,
                                          LodTileBuilder& builder, const std::atomic<bool>& cancelled) = 0;

    // Identifies what the sampler generates: lodWorldHash(seed, generator
    // version, preset hash). Cached tiles made with another hash are stale.
    [[nodiscard]] virtual u64 worldHash() const = 0;
};

// Receives the tiles to draw (the LOD renderer, E11.3). Called on the main
// thread only, from LodManager::update().
class LodTileSink {
public:
    LodTileSink() = default;
    virtual ~LodTileSink() = default;
    LodTileSink(const LodTileSink&) = delete;
    LodTileSink& operator=(const LodTileSink&) = delete;
    LodTileSink(LodTileSink&&) = delete;
    LodTileSink& operator=(LodTileSink&&) = delete;

    // A tile to draw from now on. A tile with the same key replaces the old one.
    virtual void addTile(std::shared_ptr<const LodTile> tile) = 0;
    virtual void removeTile(const LodTileKey& key) = 0;
};

struct LodManagerDesc {
    jobs::JobSystem* jobs = nullptr;
    config::SettingsRegistry* settings = nullptr;
    LodTileSampler* sampler = nullptr;
    LodTileCache* cache = nullptr;        // Optional: without it every tile is sampled.
    LodTileSink* sink = nullptr;          // Optional (tests and tools read tiles directly).
    stats::StatRegistry* stats = nullptr; // Optional.
};

// Keeps the LOD tiles around the camera ready (Section 13.4): follows the
// clipmap (LodClipmap) and, for every tile it wants, tries the cache on an IO
// thread, samples the tile on a worker when the cache misses (low priority,
// below real chunks), saves sampled tiles back to the cache, and hands
// finished tiles to the sink.
//
// No holes: a tile that leaves the clipmap stays with the sink until the
// tiles that take over its ground are ready (the finer level after the
// camera moves, the coarser one after it moves away), so the far view never
// shows the sky through the ground while new tiles generate.
//
// Order: nearest first, by the distance in tiles of the tile's own level, then
// finer levels first; every ring of every level comes before the next ring,
// so the horizon appears as fast as the near ground.
//
// Settings (Graphics.LOD): Enabled, Levels, HalfExtentTiles, MaxSpansPerCell,
// CavesInLod and MaxJobsInFlight, all live. Changing the spans or caves makes
// every tile stale (they are part of the cache hash) and samples them again.
//
// Stats: Voxel.Lod.Tiles (ready), .Queued, .Running, .Retiring (gauges),
// .MemoryBytes (gauge), .Sampled, .FromCache (counters) and .SampleTime
// (timer, per tile, on workers).
//
// Thread safety: update() and every accessor on the main thread only. The
// jobs work on slots the main thread hands out and takes back; the
// destructor cancels running jobs and waits for them.
class LodManager {
public:
    explicit LodManager(const LodManagerDesc& desc);
    ~LodManager();
    LodManager(const LodManager&) = delete;
    LodManager& operator=(const LodManager&) = delete;
    LodManager(LodManager&&) = delete;
    LodManager& operator=(LodManager&&) = delete;

    // Moves the clipmap to the camera, takes finished tiles and starts jobs.
    // Call once per frame. Cheap when nothing changed (no allocation).
    void update(const DVec3& cameraPosition);

    // The ready tile with this key, or null.
    [[nodiscard]] std::shared_ptr<const LodTile> tile(const LodTileKey& key) const;
    // Every tile the sink holds right now: the clipmap's ready tiles plus
    // retiring ones.
    [[nodiscard]] usize shownTiles() const noexcept { return m_ready.size(); }
    [[nodiscard]] const LodClipmap& clipmap() const noexcept { return m_clipmap; }

    struct Counts {
        usize wanted = 0;   // Tiles in the clipmap.
        usize ready = 0;    // Of those, ready.
        usize queued = 0;   // Waiting for a job slot.
        usize running = 0;  // Loading, sampling or saving.
        usize retiring = 0; // Out of the clipmap, still shown until replaced.
        std::array<usize, kLodMaxLevel + 1> readyPerLevel{};
    };
    [[nodiscard]] Counts counts() const;
    // True when every wanted tile is ready, nothing is retiring and no job
    // is running.
    [[nodiscard]] bool isSettled() const noexcept;

private:
    // Work on one tile. The main thread fills it and schedules a job; the
    // job chain writes `tile` and `sampled`, then sets `done`.
    struct Slot {
        LodTileKey key{};
        u64 hash = 0;
        LodSampleOptions options;
        std::atomic<bool> done{false};
        std::atomic<bool> cancelled{false};
        bool sampled = false; // True if the tile came from the sampler, not the cache.
        std::shared_ptr<LodTile> tile;
    };
    struct Wanted {
        bool ready = false;   // A tile of the current hash is shown.
        bool running = false; // A slot works on it.
    };
    struct Shown {
        std::shared_ptr<const LodTile> tile;
        u64 hash = 0; // The cache hash it was made with.
    };
    struct KeyHash {
        [[nodiscard]] u64 operator()(const LodTileKey& key) const noexcept {
            return hashCombine(hashCombine(key.level, static_cast<u32>(key.x)), static_cast<u32>(key.z));
        }
    };

    void registerSettings(config::SettingsRegistry& settings);
    void registerStats();
    [[nodiscard]] LodClipmapDesc clipmapDesc() const;
    [[nodiscard]] LodSampleOptions sampleOptions() const;
    [[nodiscard]] u64 currentHash() const;
    void clearAll();
    void restartAll();
    void applyClipmapChanges();
    void collect();
    void finish(Slot& slot);
    void retire();
    void schedule();
    void sortQueue();
    void cancelSlot(const LodTileKey& key);
    void runLoad(Slot& slot);
    void runSample(Slot& slot);
    void saveTile(std::shared_ptr<const LodTile> tile, u64 hash);
    [[nodiscard]] bool replaced(const LodTileKey& key) const;
    void publishStats();

    jobs::JobSystem& m_jobs;
    LodTileSampler* m_sampler;
    LodTileCache* m_cache;
    LodTileSink* m_sink;
    stats::StatRegistry* m_stats;

    config::Setting<bool> m_enabled;
    config::Setting<i32> m_levels;
    config::Setting<i32> m_halfExtent;
    config::Setting<i32> m_maxSpans;
    config::Setting<bool> m_caves;
    config::Setting<i32> m_maxJobs;

    LodClipmap m_clipmap;
    LodClipmapDesc m_appliedDesc;
    u64 m_appliedHash = 0;
    bool m_wasEnabled = false;
    DVec3 m_camera{0.0};

    HashMap<LodTileKey, Wanted, KeyHash> m_wanted;
    HashMap<LodTileKey, Shown, KeyHash> m_ready; // What the sink holds: wanted and retiring tiles.
    std::vector<LodTileKey> m_retiring;
    std::vector<LodTileKey> m_queue; // Wanted tiles without a job, in order (front first).
    bool m_queueDirty = false;
    bool m_retireDirty = false;
    usize m_queueHead = 0;
    std::atomic<u32> m_savesRunning{0};
    std::atomic<bool> m_warnedIncomplete{false}; // Set by workers.

    // Jobs hold a Slot pointer, never an index: the vector grows while jobs run.
    std::vector<std::unique_ptr<Slot>> m_slots;
    std::vector<u16> m_freeSlots;
    std::vector<u16> m_busySlots;
    jobs::JobCounter m_jobCounter;

    struct StatIds {
        stats::StatId tiles;
        stats::StatId queued;
        stats::StatId running;
        stats::StatId retiring;
        stats::StatId memory;
        stats::StatId sampled;
        stats::StatId fromCache;
        stats::StatId sampleTime;
    } m_statIds;
};

} // namespace vxe::voxel
