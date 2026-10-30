#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/containers.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/core/types.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/voxel/block_registry.hpp>
#include <vxe/voxel/chunk_lighter.hpp>
#include <vxe/voxel/chunk_snapshot.hpp>
#include <vxe/voxel/chunk_source.hpp>
#include <vxe/voxel/mesher.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vxe::voxel {

// Receives the meshes the streaming manager makes; in a game client the
// chunk renderer (ChunkRenderSink). Tests use their own.
//
// Thread safety: called on the thread that runs StreamingManager::update.
class ChunkMeshSink {
public:
    enum class Result : u8 {
        Accepted,
        OverBudget, // Nothing more fits this frame; the same mesh is offered again next frame.
        Refused,    // Out of space; the chunk stays without its new mesh.
    };

    virtual ~ChunkMeshSink() = default;
    ChunkMeshSink() = default;
    ChunkMeshSink(const ChunkMeshSink&) = delete;
    ChunkMeshSink& operator=(const ChunkMeshSink&) = delete;
    ChunkMeshSink(ChunkMeshSink&&) = delete;
    ChunkMeshSink& operator=(ChunkMeshSink&&) = delete;

    // Makes `mesh` the chunk's mesh, replacing an older one. The mesh is only
    // valid during the call. Empty meshes come too: a chunk of solid stone
    // draws nothing but still hides what is behind it.
    [[nodiscard]] virtual Result upload(const ChunkMesh& mesh) = 0;
    // Forgets the chunk's mesh.
    virtual void remove(ChunkPos position) = 0;
    // Multiplies the per frame upload budget; burst mode (Section 11.6)
    // raises it while the loading screen shows. 1 is the normal budget.
    virtual void setBudgetScale(f32 scale) { static_cast<void>(scale); }
};

// In which order chunks of equal need are requested (World.Streaming.LoadOrder).
enum class LoadOrder : u8 {
    Spiral,    // Square rings around the camera, nearest ring first.
    Distance,  // By straight line distance.
    ViewFirst, // By distance, with chunks ahead of the camera and its movement first.
};

// Where streaming is centered this frame, normally the camera.
struct StreamingView {
    DVec3 position{};
    Vec3 forward{0.0F, 0.0F, -1.0F}; // Where the camera looks (for LoadOrder::ViewFirst).
};

struct StreamingDesc {
    VoxelWorld* world = nullptr;
    ChunkSource* source = nullptr;
    jobs::JobSystem* jobs = nullptr;
    config::SettingsRegistry* settings = nullptr;
    // Optional: without a sink chunks are loaded and lit but never meshed,
    // which is what a server needs. With one, `blocks` is required.
    ChunkMeshSink* sink = nullptr;
    // The block registry, for meshing and lighting. Without it chunks are not
    // lit (they keep the light they were loaded with).
    const BlockRegistry* blocks = nullptr;
    stats::StatRegistry* stats = nullptr; // Optional.
    // The world's height in chunks, both ends included (Section 9.2): the
    // chunks above and below are never requested and count as present
    // neighbors, so the top and bottom layers still get meshed.
    u64 variationSeed = 0; // The world's seed, for plant variations.
    i32 minChunkY = -64;
    i32 maxChunkY = 191;
    // Job slots are allocated once, so these cap the in flight settings
    // (also in burst mode, which raises them).
    u32 loadSlots = 512;
    u32 meshSlots = 256;
    u32 lightSlots = 64;
};

// The streaming settings (World.Streaming and the light job settings). The
// manager registers them, and a game registers them at startup too, so that
// its settings menu shows them while no world exists. Registering a setting
// again returns the same setting, so both can happen. The slot counts must
// match the manager's: they cap the in flight settings.
struct StreamingSettings {
    explicit StreamingSettings(config::SettingsRegistry& settings, u32 loadSlots = 512, u32 meshSlots = 256,
                               u32 lightSlots = 64);

    config::Setting<i32> renderDistance;
    config::Setting<i32> verticalDistance;
    config::Setting<i32> maxLoadJobs;
    config::Setting<i32> maxMeshJobs;
    config::Setting<i32> maxLightJobs;
    config::Setting<bool> blockLight;
    config::Setting<i32> skyRelightDepth;
    config::Setting<i32> maxChunks;
    config::Setting<i32> unloadHysteresis;
    config::Setting<LoadOrder> loadOrder;
    config::Setting<f32> budgetMs;
    config::Setting<f32> burstSeconds;
};

// Where a chunk the manager tracks is in the pipeline (Section 11.2). The
// plan's Generating and LoadingFromDisk are one stage here, since the game's
// ChunkSource decides which it is; Featuring happens inside generation
// (gathered features, Section 12.2). "Resident" is a Ready chunk whose mesh
// is with the sink.
enum class ChunkStage : u8 {
    Pending,    // Wanted, waiting for a load job slot.
    Loading,    // A load job runs (generation or disk).
    Ready,      // In the world. Meshed or not; waits for neighbors or a change.
    Lighting,   // Waiting for, or running, a light job (E10).
    MeshQueued, // Waiting for a mesh job slot.
    Meshing,    // A mesh job runs on a snapshot.
    Uploading,  // The mesh is done and waits for the sink's upload budget.
};
inline constexpr usize kChunkStageCount = 7;

// How many tracked chunks are in each stage, plus derived totals.
struct StreamingCounts {
    std::array<usize, kChunkStageCount> stages{};
    usize loaded = 0; // In the world: Ready and later.
    usize meshed = 0; // With a mesh at the sink.

    [[nodiscard]] usize at(ChunkStage stage) const noexcept { return stages.at(static_cast<usize>(stage)); }
};

// Decides which chunks exist around the camera and drives each through the
// chunk pipeline (Section 11): request, load or generate on workers, insert
// into the VoxelWorld, light once all 26 neighbors are there, mesh once the
// chunk and all 26 neighbors are lit, upload within the sink's budget, and
// unload when the camera moved away.
//
// Ranges, around the camera's chunk (R = render distance, V = vertical
// render distance, H = unload hysteresis), horizontally as the larger of the
// X and Z distance (square rings) and vertically on their own:
//   meshed     within R and V
//   lit        within R + 1 and V + 1, so every meshed chunk has lit
//              neighbors for its border
//   loaded     within R + 2 and V + 2, so every lit chunk has all of its
//              neighbors (a light source up to 15 blocks away can reach it)
//   kept       meshes within R + H, light within R + 1 + H, loaded chunks
//              within R + 2 + H; beyond that they go. The gap stops chunks at
//              the edge from loading and unloading over and over.
// A meshed chunk therefore never sees a neighbor arrive or get its first
// light, so a chunk is meshed once, and again only when the world says it
// changed (VoxelWorld::changedChunks, edits and their light) or the mesh
// settings or block registry changed.
//
// Lighting (E10): sky and block light (ChunkLighter) of a chunk come from
// its neighborhood and the open sky above it (column summaries). Chunks of
// one opaque block, and chunks of air under open sky with no light source in
// reach, are lit right away; the others (the surface, caves, near light
// sources) run a light job. Block edits (VoxelWorld::editedChunks) light the
// edited chunk, its 26 neighbors and the chunks below them down to
// Graphics.Lighting.SkyRelightDepth again, as do loaded chunks that bring
// blocks above lit ones; a light result from blocks that changed since its
// job copied them is thrown away and computed again.
//
// Priority (Section 11.3): candidates come from a list of offsets sorted by
// distance (with vertical distance counting double), scanned from the
// nearest whenever the camera enters another chunk. The nearest few hundred
// unrequested chunks wait in a heap ordered by distance times a direction
// bias (LoadOrder::ViewFirst: up to half the distance off for chunks ahead
// of the view and the movement). Mesh jobs run at a higher job priority than
// loads, so nearly finished chunks win over new ones (stage bias).
// Priorities are recomputed when the camera crosses a chunk border.
//
// Cancellation (Section 11.4): every job has a slot with a cancel flag, set
// when its chunk leaves the range; results of cancelled jobs and meshes made
// from an outdated snapshot (the chunk's version moved on) are thrown away.
//
// Budgets: load and mesh jobs in flight, chunks tracked at most, and the
// main thread time per update (World.Streaming.MainThreadBudgetMs). Every
// main thread loop (inserting results, snapshots, uploads, unloads, scans)
// stops when the time is up and continues next frame, so streaming cannot
// cause a long frame. Burst mode (Section 11.6, startBurst) multiplies the
// job and time budgets and the sink's upload budget by 4 for
// World.Streaming.BurstSeconds or until everything in range is done.
//
// Settings (World.Streaming): RenderDistance, VerticalRenderDistance,
// MaxLoadJobsInFlight, MaxLightJobsInFlight, MaxMeshJobsInFlight, MaxChunks,
// UnloadHysteresisChunks, LoadOrder, MainThreadBudgetMs, BurstSeconds; the
// upload budget is the sink's (MaxUploadKBPerFrame of the chunk renderer).
// Mesh options come from MesherSettings (Graphics.Terrain);
// Graphics.Lighting.BlockLight turns block light off and on.
//
// Stats (Voxel.Streaming): chunks per stage, chunks loaded, meshed,
// uploaded, unloaded, cancelled and stale meshes per frame, the main thread
// time per update, and the latency from requesting a chunk to its first
// mesh at the sink as a histogram; Voxel.Light.ChunksLit, .Skipped and
// .JobTime for lighting.
//
// Thread safety: main thread only (the thread that owns the VoxelWorld).
// Jobs touch only their own slot and the ChunkSource.
class StreamingManager {
public:
    explicit StreamingManager(const StreamingDesc& desc);
    // Waits for running jobs. Meshes stay with the sink; call unloadAll()
    // first to take them back.
    ~StreamingManager();
    StreamingManager(const StreamingManager&) = delete;
    StreamingManager& operator=(const StreamingManager&) = delete;
    StreamingManager(StreamingManager&&) = delete;
    StreamingManager& operator=(StreamingManager&&) = delete;

    // One step of the pipeline, once per frame. With a sink, call it after
    // the sink can take uploads (after ChunkRenderer::beginFrame).
    void update(const StreamingView& view);

    // Raises the budgets for World.Streaming.BurstSeconds (world entry).
    void startBurst();
    [[nodiscard]] bool isBursting() const noexcept { return m_bursting; }
    // Nothing left to do: every chunk in range is loaded and, with a sink,
    // meshed, and no job runs.
    [[nodiscard]] bool isSettled() const noexcept;
    // Every chunk within `radius` chunks horizontally of the latest view (at
    // most the render distance) and within `layers` chunk layers (or the vertical
    // render distance, the default and the most) is
    // loaded and, with a sink, has its mesh there. "Playable" in Section 2.
    [[nodiscard]] bool isAreaReady(i32 radius, i32 layers = -1) const;

    // Meshes every meshed chunk again, a few per frame (after a change the
    // manager does not notice by itself, such as new textures).
    void remeshAll();
    // Cancels all jobs, waits for them, takes every mesh back from the sink
    // and removes every chunk from the world.
    void unloadAll();

    [[nodiscard]] const StreamingCounts& counts() const noexcept { return m_counts; }
    [[nodiscard]] std::optional<ChunkStage> stageOf(ChunkPos position) const;
    [[nodiscard]] bool hasMesh(ChunkPos position) const;
    // The chunk's light was computed at least once.
    [[nodiscard]] bool isLit(ChunkPos position) const;
    [[nodiscard]] ChunkPos center() const noexcept { return m_center; }
    [[nodiscard]] i32 renderDistance() const noexcept;
    [[nodiscard]] i32 verticalRenderDistance() const noexcept;
    // One line for the debug overlay.
    [[nodiscard]] std::string summary() const;

    // Main thread time of update() outside burst mode (where the budget is
    // raised on purpose), since creation or resetUpdateTimes(). For
    // measurement runs.
    struct UpdateTimes {
        u64 updates = 0;
        f64 totalMilliseconds = 0.0;
        f64 maxMilliseconds = 0.0;
        u64 overOneMillisecond = 0;
    };
    [[nodiscard]] const UpdateTimes& updateTimes() const noexcept { return m_times; }
    void resetUpdateTimes() noexcept { m_times = {}; }

private:
    struct Entry {
        TimePoint requested{};
        ChunkStage stage = ChunkStage::Pending;
        u8 neighbors = 0;          // Neighbors in the world or outside its height, 0 to 26.
        u8 litNeighbors = 0;       // Neighbors lit at least once (or outside the world's height).
        bool lit = false;          // Its light was computed at least once.
        bool lightDirty = true;    // Its light must be computed (again).
        bool lightRunning = false; // In Lighting: a job has its slot (else it waits in the queue).
        bool meshed = false;       // The sink holds a mesh of this chunk.
        bool meshDirty = false;    // The chunk needs a (new) mesh.
        bool latencyDone = false;  // The request to first mesh time was recorded.
        u16 slot = 0;              // The job slot while Loading, Lighting, Meshing or Uploading.
    };

    // A load job's slot: the chunk, the result and the flags shared with
    // the worker. `done` is written last by the worker (release) and read
    // first by the main thread (acquire), which hands the product over.
    struct LoadSlot {
        ChunkPos position{};
        std::atomic<bool> done{false};
        std::atomic<bool> cancelled{false};
        ChunkProduct product;
    };
    struct MeshSlot {
        ChunkPos position{};
        std::atomic<bool> done{false};
        std::atomic<bool> cancelled{false};
        u32 epoch = 0; // m_meshEpoch when the job started.
        MeshOptions options{};
        std::shared_ptr<const BlockRegistryView> blocks;
        ChunkSnapshot snapshot;
        ChunkMesh mesh;
    };
    // A light job's slot: copies of the chunk and its 26 neighbors (the job
    // must not read the live world), their block versions to tell later
    // whether the result is still current, and the result.
    struct LightSlot {
        ChunkPos position{};
        std::atomic<bool> done{false};
        std::atomic<bool> cancelled{false};
        std::array<ChunkStorage, 27> chunks;
        std::array<bool, 27> present{};
        std::array<u64, 27> versions{};
        std::shared_ptr<const BlockRegistryView> blocks;
        SkyOpenings sky;
        bool blockLight = true;
        std::array<u8, kChunkVolume> result{}; // Packed light (packLight).
    };
    struct Candidate {
        f32 score = 0.0F; // Lower goes first.
        ChunkPos position{};
    };
    struct Offset {
        i16 x = 0;
        i16 y = 0;
        i16 z = 0;
    };

    void registerStats();
    void readSettings();
    void rebuildOffsets();
    void moveCenter(const StreamingView& view);
    void reprioritize();
    void checkMeshInputs();
    void collectLoads();
    void collectMeshes();
    void collectLights();
    void processEditedChunks();
    void scheduleLights();
    void runLight(u16 index);
    void considerLight(ChunkPos position, Entry& entry);
    void considerWork(ChunkPos position, Entry& entry);
    void markLit(ChunkPos position, Entry& entry);
    void relightAll();
    void relightBelow(ChunkPos position, i32 above);
    // With `middleOnly`, only the chunk's own 16 x 16 columns are worked out.
    [[nodiscard]] SkyOpenings skyOpenings(ChunkPos position, bool middleOnly) const;
    [[nodiscard]] std::optional<u8> uniformLight(ChunkPos position, const ChunkNeighborhood& around) const;
    [[nodiscard]] bool wantsLight(ChunkPos position, const Entry& entry) const noexcept;
    [[nodiscard]] u32 lightLimit() const noexcept;
    void processChangedChunks();
    void uploadMeshes();
    void scheduleMeshes();
    void requestMaintenance();
    void maintain();
    void maintainEntry(ChunkPos position, Entry& entry);
    void requestChunks();
    void scheduleLoads();
    void updateBurst();
    void publishStats();

    void runLoad(u16 index);
    void runMesh(u16 index);
    void setStage(Entry& entry, ChunkStage stage) noexcept;
    void eraseEntry(ChunkPos position);
    void unload(ChunkPos position, Entry& entry);
    void unmesh(ChunkPos position, Entry& entry);
    void considerMesh(ChunkPos position, Entry& entry);
    void insertLoaded(ChunkPos position, Entry& entry, LoadSlot& slot);
    void freeMeshSlot(u16 slot);
    void sampleLatency(Entry& entry);
    [[nodiscard]] bool overBudget() const noexcept;
    [[nodiscard]] u32 loadLimit() const noexcept;
    [[nodiscard]] u32 meshLimit() const noexcept;
    [[nodiscard]] usize meshQueueCap() const noexcept;
    [[nodiscard]] f32 score(ChunkPos position) const noexcept;
    [[nodiscard]] bool inWorldHeight(i64 chunkY) const noexcept;
    [[nodiscard]] bool within(ChunkPos position, i32 horizontal, i32 vertical) const noexcept;
    [[nodiscard]] bool wantsMesh(ChunkPos position, const Entry& entry) const noexcept;

    VoxelWorld& m_world;
    ChunkSource& m_source;
    jobs::JobSystem& m_jobs;
    ChunkMeshSink* m_sink;
    const BlockRegistry* m_blocks;
    stats::StatRegistry* m_stats;
    i32 m_minChunkY;
    i32 m_maxChunkY;
    u64 m_id; // Tells worker threads' cached meshers which manager they belong to.

    [[nodiscard]] MeshOptions meshOptions() const {
        MeshOptions options = m_mesherSettings.options();
        options.variationSeed = m_variationSeed;
        return options;
    }
    MesherSettings m_mesherSettings;
    u64 m_variationSeed;
    MeshOptions m_options{};
    std::shared_ptr<const BlockRegistryView> m_view;
    u32 m_meshEpoch = 0;

    HashMap<ChunkPos, Entry> m_entries;
    StreamingCounts m_counts;
    std::vector<Candidate> m_pending;   // Min heap on score.
    std::vector<Candidate> m_meshQueue; // Min heap on score.
    std::vector<Offset> m_offsets;      // Load range offsets, nearest first.
    usize m_scanCursor = 0;
    bool m_scanDone = false;
    usize m_maintainCursor = 0;
    bool m_maintaining = false;          // A maintenance pass runs.
    bool m_maintainAgain = false;        // Another pass follows the running one.
    bool m_passRemesh = false;           // The running pass marks meshed chunks for a new mesh.
    bool m_remeshAll = false;            // The next pass does.
    bool m_meshBacklog = false;          // Chunks wait for room in the mesh queue.
    bool m_passRelight = false;          // The running pass marks lit chunks for new light.
    bool m_relightAll = false;           // The next pass does.
    bool m_lightBacklog = false;         // Chunks wait for room in the light queue.
    bool m_blockLightOn = true;          // Graphics.Lighting.BlockLight as last seen.
    std::vector<Candidate> m_lightQueue; // Min heap on score.
    std::vector<ChunkPos> m_unloadQueue;

    std::vector<std::unique_ptr<LoadSlot>> m_loadSlots;
    std::vector<u16> m_freeLoadSlots;
    std::vector<u16> m_busyLoadSlots;
    std::vector<std::unique_ptr<MeshSlot>> m_meshSlots;
    std::vector<u16> m_freeMeshSlots;
    std::vector<u16> m_busyMeshSlots; // Jobs running.
    std::vector<u16> m_uploadQueue;   // Meshes done, oldest first.
    std::vector<std::unique_ptr<LightSlot>> m_lightSlots;
    std::vector<u16> m_freeLightSlots;
    std::vector<u16> m_busyLightSlots;
    jobs::JobCounter m_jobCounter;

    ChunkPos m_center{};
    bool m_hasCenter = false;
    Vec3 m_look{0.0F, 0.0F, -1.0F}; // View and movement direction, for ViewFirst.
    DVec3 m_lastPosition{};
    i32 m_rangeR = -1; // Settings the offsets and priorities were built for.
    i32 m_rangeV = -1;
    i32 m_hysteresis = -1;
    LoadOrder m_order = LoadOrder::ViewFirst;

    TimePoint m_deadline{};
    bool m_bursting = false;
    TimePoint m_burstEnd{};
    Stopwatch m_burstClock;
    UpdateTimes m_times;
    bool m_warnedRefused = false;

    StreamingSettings m_settings;

    struct StatIds {
        std::array<stats::StatId, kChunkStageCount> stages;
        stats::StatId loaded;
        stats::StatId meshed;
        stats::StatId loadedPerFrame;
        stats::StatId meshedPerFrame;
        stats::StatId uploadedPerFrame;
        stats::StatId unloadedPerFrame;
        stats::StatId cancelled;
        stats::StatId stale;
        stats::StatId lightJobs;
        stats::StatId lightSkipped;
        stats::StatId lightTime;
        stats::StatId updateTime;
        stats::StatId latency;
        stats::StatId burst;
    } m_statIds{};
};

} // namespace vxe::voxel
