#include "vxe/voxel/streaming.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/log.hpp>
#include <vxe/core/profile.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <utility>

namespace vxe::voxel {
namespace {

// Burst mode multiplies the job, time and upload budgets by this.
constexpr u32 kBurstFactor = 4;
// Vertical chunk distance counts double in priorities: the view reaches
// much farther sideways than up and down.
constexpr f32 kVerticalWeight = 2.0F;
// Offsets looked at per update at most, even with time left: with all
// chunks near the camera loaded, a scan is mostly hash lookups that find
// something, and this keeps one such update far below the budget.
constexpr usize kMaxScanPerUpdate = 16384;
constexpr i64 kCameraLayers = 3; // Layers around the camera that load with the ground.
constexpr i64 kGroundLayers = 4; // Layers around the ground that load first.
// The pending heap holds at least this many candidates, else twice the
// load jobs in flight: enough to pick well, small enough to rescore at once.
constexpr usize kMinPending = 64;
// Loops check the clock every so many cheap steps instead of every step.
constexpr usize kClockStride = 64;
constexpr u8 kNeighborCount = 26;

constexpr std::array<config::EnumSettingDesc<LoadOrder>::Option, 3> kOrders = {{
    {.value = LoadOrder::Spiral, .name = "Spiral", .displayName = "Spiral"},
    {.value = LoadOrder::Distance, .name = "Distance", .displayName = "Distance"},
    {.value = LoadOrder::ViewFirst, .name = "ViewFirst", .displayName = "View first"},
}};

constexpr std::array<std::array<i32, 3>, kNeighborCount> kNeighborOffsets = [] {
    std::array<std::array<i32, 3>, kNeighborCount> offsets{};
    usize next = 0;
    for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (dx != 0 || dy != 0 || dz != 0) {
                    offsets.at(next++) = {dx, dy, dz};
                }
            }
        }
    }
    return offsets;
}();

// Heap order for candidates: the lowest score on top.
constexpr auto kLater = [](const auto& a, const auto& b) noexcept { return a.score > b.score; };

constexpr bool inWorld(ChunkStage stage) noexcept {
    return stage >= ChunkStage::Ready;
}

// The chunk at an offset, or nothing beyond the edge of the int32 grid.
std::optional<ChunkPos> offsetPosition(ChunkPos position, i64 dx, i64 dy, i64 dz) noexcept {
    const i64 x = position.x + dx;
    const i64 y = position.y + dy;
    const i64 z = position.z + dz;
    constexpr i64 kLow = std::numeric_limits<i32>::min();
    constexpr i64 kHigh = std::numeric_limits<i32>::max();
    if (x < kLow || x > kHigh || y < kLow || y > kHigh || z < kLow || z > kHigh) {
        return std::nullopt;
    }
    return ChunkPos{.x = static_cast<i32>(x), .y = static_cast<i32>(y), .z = static_cast<i32>(z)};
}

// Every manager gets its own id, so a worker thread's cached mesher (which
// records into that manager's stat registry) is never reused for another
// manager whose registry may be a different one, or gone.
// Slot indices are 16 bits.
u32 clampedSlots(u32 slots) noexcept {
    return std::clamp<u32>(slots, 1, std::numeric_limits<u16>::max());
}

u64 nextManagerId() noexcept {
    static std::atomic<u64> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

// One mesher per worker thread: meshers hold scratch memory, so they are
// made once per thread instead of per job.
Mesher& workerMesher(u64 owner, stats::StatRegistry* stats) {
    struct Cached {
        u64 owner = 0;
        std::optional<Mesher> mesher;
    };
    thread_local Cached cached;
    if (cached.owner != owner) {
        cached.mesher.emplace(stats);
        cached.owner = owner;
    }
    return *cached.mesher;
}

} // namespace

StreamingManager::StreamingManager(const StreamingDesc& desc)
    : m_world(*desc.world)
    , m_source(*desc.source)
    , m_jobs(*desc.jobs)
    , m_sink(desc.sink)
    , m_blocks(desc.blocks)
    , m_stats(desc.stats)
    , m_minChunkY(desc.minChunkY)
    , m_maxChunkY(desc.maxChunkY)
    , m_id(nextManagerId())
    , m_mesherSettings(*desc.settings)
    , m_variationSeed(desc.variationSeed)
    , m_settings(*desc.settings, clampedSlots(desc.loadSlots), clampedSlots(desc.meshSlots),
                 clampedSlots(desc.lightSlots)) {
    VXE_ASSERT(desc.world != nullptr && desc.source != nullptr && desc.jobs != nullptr &&
                   desc.settings != nullptr,
               "StreamingManager needs a world, a source, jobs and settings");
    VXE_ASSERT(desc.sink == nullptr || desc.blocks != nullptr, "Meshing needs the block registry");
    VXE_ASSERT(desc.minChunkY <= desc.maxChunkY, "The world needs at least one chunk layer");
    const u32 loadSlots = clampedSlots(desc.loadSlots);
    const u32 meshSlots = clampedSlots(desc.meshSlots);
    const u32 lightSlots = clampedSlots(desc.lightSlots);
    registerStats();

    m_loadSlots.reserve(loadSlots);
    m_freeLoadSlots.reserve(loadSlots);
    m_busyLoadSlots.reserve(loadSlots);
    for (u32 i = 0; i < loadSlots; ++i) {
        m_loadSlots.push_back(std::make_unique<LoadSlot>());
        m_freeLoadSlots.push_back(static_cast<u16>(loadSlots - 1 - i));
    }
    if (m_sink != nullptr) {
        m_meshSlots.reserve(meshSlots);
        m_freeMeshSlots.reserve(meshSlots);
        m_busyMeshSlots.reserve(meshSlots);
        m_uploadQueue.reserve(meshSlots);
        for (u32 i = 0; i < meshSlots; ++i) {
            m_meshSlots.push_back(std::make_unique<MeshSlot>());
            m_freeMeshSlots.push_back(static_cast<u16>(meshSlots - 1 - i));
        }
        m_options = meshOptions();
    }
    if (m_blocks != nullptr) {
        m_view = m_blocks->current();
        m_lightSlots.reserve(lightSlots);
        m_freeLightSlots.reserve(lightSlots);
        m_busyLightSlots.reserve(lightSlots);
        for (u32 i = 0; i < lightSlots; ++i) {
            m_lightSlots.push_back(std::make_unique<LightSlot>());
            m_freeLightSlots.push_back(static_cast<u16>(lightSlots - 1 - i));
        }
    }
    m_blockLightOn = m_settings.blockLight.get();
}

StreamingManager::~StreamingManager() {
    for (const u16 index : m_busyLoadSlots) {
        m_loadSlots[index]->cancelled.store(true, std::memory_order_relaxed);
    }
    for (const u16 index : m_busyMeshSlots) {
        m_meshSlots[index]->cancelled.store(true, std::memory_order_relaxed);
    }
    for (const u16 index : m_busyLightSlots) {
        m_lightSlots[index]->cancelled.store(true, std::memory_order_relaxed);
    }
    m_jobs.wait(m_jobCounter);
}

StreamingSettings::StreamingSettings(config::SettingsRegistry& settings, u32 loadSlots, u32 meshSlots,
                                     u32 lightSlots) {
    const auto integer = [&settings](std::string_view id, std::string_view name, std::string_view description,
                                     i32 value, i32 min, i32 max, config::Visibility visibility) {
        return settings.registerInt(
            {{.id = id, .displayName = name, .description = description, .visibility = visibility},
             value,
             min,
             max,
             1});
    };
    renderDistance = integer("World.Streaming.RenderDistance", "Render distance",
                             "How far around the camera chunks are loaded and drawn, in chunks of 16 blocks.",
                             16, 2, 64, config::Visibility::Normal);
    verticalDistance = integer("World.Streaming.VerticalRenderDistance", "Vertical render distance",
                               "How far up and down chunks are loaded and drawn, in chunks.", 8, 1, 32,
                               config::Visibility::Normal);
    maxLoadJobs = integer("World.Streaming.MaxLoadJobsInFlight", "Load jobs in flight",
                          "Chunks generated or read at the same time. More fills the world in faster but "
                          "delays other jobs.",
                          std::min(48, static_cast<i32>(loadSlots)), 1, static_cast<i32>(loadSlots),
                          config::Visibility::Advanced);
    maxMeshJobs = integer("World.Streaming.MaxMeshJobsInFlight", "Mesh jobs in flight",
                          "Chunks meshed or waiting for upload at the same time.",
                          std::min(32, static_cast<i32>(meshSlots)), 1, static_cast<i32>(meshSlots),
                          config::Visibility::Advanced);
    maxLightJobs = integer("World.Streaming.MaxLightJobsInFlight", "Light jobs in flight",
                           "Chunks lit at the same time. Only chunks near light sources need a job.",
                           std::min(16, static_cast<i32>(lightSlots)), 1, static_cast<i32>(lightSlots),
                           config::Visibility::Advanced);
    blockLight = settings.registerBool({{.id = "Graphics.Lighting.BlockLight",
                                         .displayName = "Block light",
                                         .description = "Light from torches, lava and glowing blocks. Off "
                                                        "leaves only sky light."},
                                        true});
    skyRelightDepth = integer("Graphics.Lighting.SkyRelightDepth", "Sky relight depth",
                              "How many chunks below a change sky light is computed again. Deeper "
                              "is more exact under tall overhangs and costs more per edit.",
                              4, 0, 32, config::Visibility::Advanced);
    maxChunks = integer("World.Streaming.MaxChunks", "Most chunks",
                        "The most chunks loaded or waiting to load, a memory cap. Normally the render "
                        "distance decides.",
                        524288, 1024, 4194304, config::Visibility::Advanced);
    unloadHysteresis = integer("World.Streaming.UnloadHysteresisChunks", "Unload margin",
                               "How many chunks past the render distance chunks stay, so moving back and "
                               "forth at the edge does not load them again and again.",
                               2, 0, 8, config::Visibility::Advanced);
    config::EnumSettingDesc<LoadOrder> order;
    order.id = "World.Streaming.LoadOrder";
    order.displayName = "Load order";
    order.description = "Which chunks load first: square rings, by distance, or what is ahead first.";
    order.visibility = config::Visibility::Advanced;
    order.defaultValue = LoadOrder::ViewFirst;
    order.options = kOrders;
    loadOrder = settings.registerEnum(order);
    budgetMs = settings.registerFloat({{.id = "World.Streaming.MainThreadBudgetMs",
                                        .displayName = "Main thread budget",
                                        .description = "Most time per frame the main thread spends on "
                                                       "streaming, in milliseconds. The rest waits for the "
                                                       "next frame.",
                                        .visibility = config::Visibility::Advanced},
                                       0.5F,
                                       0.05F,
                                       8.0F,
                                       0.05F});
    burstSeconds =
        settings.registerFloat({{.id = "World.Streaming.BurstSeconds",
                                 .displayName = "Burst on world entry",
                                 .description = "How long entering a world streams with raised "
                                                "budgets, in seconds. Ends early once everything in "
                                                "range is ready.",
                                 .visibility = config::Visibility::Advanced},
                                5.0F,
                                0.0F,
                                60.0F,
                                0.5F});
}

void StreamingManager::registerStats() {
    if (m_stats == nullptr) {
        return;
    }
    const auto add = [this](const stats::StatDesc& desc) {
        auto id = m_stats->registerStat(desc);
        return id ? id.value() : stats::StatId{};
    };
    const auto gauge = [&](std::string_view name, std::string_view description) {
        return add({.name = name, .description = description, .type = stats::StatType::Gauge});
    };
    const auto counter = [&](std::string_view name, std::string_view description) {
        return add({.name = name, .description = description, .type = stats::StatType::Counter});
    };
    m_statIds.stages = {
        gauge("Voxel.Streaming.Pending", "Chunks wanted, waiting for a load job."),
        gauge("Voxel.Streaming.Loading", "Chunks being generated or read."),
        gauge("Voxel.Streaming.Ready", "Chunks in the world, not in the mesh pipeline."),
        gauge("Voxel.Streaming.Lighting", "Chunks waiting for or running a light job."),
        gauge("Voxel.Streaming.MeshQueued", "Chunks waiting for a mesh job."),
        gauge("Voxel.Streaming.Meshing", "Chunks being meshed."),
        gauge("Voxel.Streaming.Uploading", "Meshes waiting for the upload budget."),
    };
    m_statIds.loaded = gauge("Voxel.Streaming.Loaded", "Chunks in the world.");
    m_statIds.meshed = gauge("Voxel.Streaming.Meshed", "Chunks with a mesh on the GPU.");
    m_statIds.meshReach = gauge("Voxel.Streaming.MeshReach", "How far real chunks reach, in chunks.");
    m_statIds.loadedPerFrame = counter("Voxel.Streaming.ChunksLoaded", "Chunks put into the world.");
    m_statIds.meshedPerFrame = counter("Voxel.Streaming.ChunksMeshed", "Chunk meshes finished.");
    m_statIds.uploadedPerFrame = counter("Voxel.Streaming.ChunksUploaded", "Chunk meshes handed to the GPU.");
    m_statIds.unloadedPerFrame = counter("Voxel.Streaming.ChunksUnloaded", "Chunks taken out of the world.");
    m_statIds.cancelled = counter("Voxel.Streaming.Cancelled", "Jobs whose chunk left the range.");
    m_statIds.stale = counter("Voxel.Streaming.StaleMeshes", "Meshes thrown away because the chunk changed.");
    m_statIds.lightJobs = counter("Voxel.Light.ChunksLit", "Chunks whose block light a job computed.");
    m_statIds.lightSkipped =
        counter("Voxel.Light.Skipped", "Chunks lit without a job: no light source within reach.");
    m_statIds.lightTime = add({.name = "Voxel.Light.JobTime",
                               .description = "Time in block light jobs, on all workers.",
                               .type = stats::StatType::Timer,
                               .unit = stats::StatUnit::Milliseconds});
    m_statIds.updateTime = add({.name = "Voxel.Streaming.UpdateTime",
                                .description = "Main thread time of the streaming update.",
                                .type = stats::StatType::Timer,
                                .unit = stats::StatUnit::Milliseconds});
    m_statIds.latency = add({.name = "Voxel.Streaming.Latency",
                             .description = "Time from requesting a chunk to its first mesh on the GPU.",
                             .type = stats::StatType::Histogram,
                             .unit = stats::StatUnit::Milliseconds,
                             .histogramMin = 0.5});
    m_statIds.burst = gauge("Voxel.Streaming.Burst", "1 while burst mode raises the budgets.");
}

i32 StreamingManager::renderDistance() const noexcept {
    return std::max(m_settings.renderDistance.get(), 1);
}

i32 StreamingManager::verticalRenderDistance() const noexcept {
    return std::max(m_settings.verticalDistance.get(), 0);
}

u32 StreamingManager::loadLimit() const noexcept {
    const auto limit =
        static_cast<u32>(std::max(m_settings.maxLoadJobs.get(), 1)) * (m_bursting ? kBurstFactor : 1U);
    return std::min(limit, static_cast<u32>(m_loadSlots.size()));
}

usize StreamingManager::meshQueueCap() const noexcept {
    return std::max<usize>(kMinPending, usize{4} * meshLimit());
}

u32 StreamingManager::meshLimit() const noexcept {
    const auto limit =
        static_cast<u32>(std::max(m_settings.maxMeshJobs.get(), 1)) * (m_bursting ? kBurstFactor : 1U);
    return std::min(limit, static_cast<u32>(m_meshSlots.size()));
}

u32 StreamingManager::lightLimit() const noexcept {
    const auto limit =
        static_cast<u32>(std::max(m_settings.maxLightJobs.get(), 1)) * (m_bursting ? kBurstFactor : 1U);
    return std::min(limit, static_cast<u32>(m_lightSlots.size()));
}

bool StreamingManager::overBudget() const noexcept {
    return Clock::now() >= m_deadline;
}

bool StreamingManager::inWorldHeight(i64 chunkY) const noexcept {
    return chunkY >= m_minChunkY && chunkY <= m_maxChunkY;
}

bool StreamingManager::within(ChunkPos position, i32 horizontal, i32 vertical) const noexcept {
    const i64 dx = std::abs(i64{position.x} - m_center.x);
    const i64 dy = std::abs(i64{position.y} - m_center.y);
    const i64 dz = std::abs(i64{position.z} - m_center.z);
    return std::max(dx, dz) <= horizontal && dy <= vertical;
}

// How far existing meshes stay. The margin saves work at the edge, but with
// memory short it would hold room that nearer chunks need: meshes arrive out of
// order, so the outer ring can fill up while a chunk inside it waits.
i32 StreamingManager::meshKept() const noexcept {
    return m_memoryReach == kNoReachLimit ? meshReach() + m_hysteresis : meshReach();
}

bool StreamingManager::wantsLight(ChunkPos position, const Entry& entry) const noexcept {
    return within(position, m_rangeR + 1, m_rangeV + 1) ||
           (entry.lit && within(position, m_rangeR + 1 + m_hysteresis, m_rangeV + 1 + m_hysteresis));
}

bool StreamingManager::wantsMesh(ChunkPos position, const Entry& entry) const noexcept {
    return within(position, meshReach(), m_rangeV) ||
           (entry.meshed && within(position, meshKept(), m_rangeV + m_hysteresis));
}

f32 StreamingManager::score(ChunkPos position) const noexcept {
    const auto dx = static_cast<f32>(i64{position.x} - m_center.x);
    const auto dy = static_cast<f32>(i64{position.y} - m_center.y) * kVerticalWeight;
    const auto dz = static_cast<f32>(i64{position.z} - m_center.z);
    if (m_order == LoadOrder::Spiral) {
        return std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
    }
    const f32 distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
    if (m_order == LoadOrder::Distance || distance == 0.0F) {
        return distance;
    }
    // Ahead of the view and the movement: up to half the distance off.
    const f32 facing = std::clamp(glm::dot(Vec3(dx, dy, dz) / distance, m_look), 0.0F, 1.0F);
    return distance * (1.0F - (0.5F * facing));
}

void StreamingManager::setStage(Entry& entry, ChunkStage stage) noexcept {
    --m_counts.stages.at(static_cast<usize>(entry.stage));
    ++m_counts.stages.at(static_cast<usize>(stage));
    if (inWorld(entry.stage) != inWorld(stage)) {
        m_counts.loaded = inWorld(stage) ? m_counts.loaded + 1 : m_counts.loaded - 1;
    }
    entry.stage = stage;
}

void StreamingManager::eraseEntry(ChunkPos position) {
    const auto it = m_entries.find(position);
    VXE_ASSERT(it != m_entries.end(), "Erasing a chunk the manager does not track");
    const Entry& entry = it->second;
    --m_counts.stages.at(static_cast<usize>(entry.stage));
    if (inWorld(entry.stage)) {
        --m_counts.loaded;
    }
    if (entry.meshed) {
        --m_counts.meshed;
    }
    m_entries.erase(it);
}

void StreamingManager::readSettings() {
    const i32 range = renderDistance();
    const i32 vertical = verticalRenderDistance();
    const LoadOrder order = m_settings.loadOrder.get();
    const i32 hysteresis = std::max(m_settings.unloadHysteresis.get(), 0);
    const bool rangeChanged = range != m_rangeR || vertical != m_rangeV || order != m_order;
    if (!rangeChanged && hysteresis == m_hysteresis) {
        return;
    }
    m_rangeR = range;
    m_rangeV = vertical;
    if (rangeChanged) {
        m_memoryReach = kNoReachLimit; // A new range gets a new try.
    }
    m_order = order;
    m_hysteresis = hysteresis;
    if (rangeChanged) {
        rebuildOffsets();
    }
    if (m_hasCenter) {
        reprioritize();
    }
}

// Every offset of the load range, nearest first by the load order's
// distance. Built when the range or order changes; about 85,000 offsets at
// render distance 32 and vertical distance 8.
void StreamingManager::rebuildOffsets() {
    VXE_PROFILE_FUNCTION();
    const i32 horizontal = m_rangeR + 2;
    const i32 vertical = m_rangeV + 2;
    std::vector<std::pair<f32, Offset>> sorted;
    sorted.reserve(static_cast<usize>((2 * horizontal) + 1) * static_cast<usize>((2 * horizontal) + 1) *
                   static_cast<usize>((2 * vertical) + 1));
    for (i32 y = -vertical; y <= vertical; ++y) {
        for (i32 z = -horizontal; z <= horizontal; ++z) {
            for (i32 x = -horizontal; x <= horizontal; ++x) {
                const auto fx = static_cast<f32>(x);
                const f32 fy = static_cast<f32>(y) * kVerticalWeight;
                const auto fz = static_cast<f32>(z);
                const f32 distance = m_order == LoadOrder::Spiral
                                         ? std::max({std::abs(fx), std::abs(fy), std::abs(fz)})
                                         : std::sqrt((fx * fx) + (fy * fy) + (fz * fz));
                sorted.emplace_back(
                    distance,
                    Offset{.x = static_cast<i16>(x), .y = static_cast<i16>(y), .z = static_cast<i16>(z)});
            }
        }
    }
    // Stable, so equal distances keep a fixed order and runs are repeatable.
    std::ranges::stable_sort(sorted, {}, &std::pair<f32, Offset>::first);
    m_offsets.clear();
    m_offsets.reserve(sorted.size());
    for (const auto& entry : sorted) {
        m_offsets.push_back(entry.second);
    }
}

void StreamingManager::moveCenter(const StreamingView& view) {
    // The view direction, pulled towards the movement so a fast flight
    // loads what it flies into before what it looks at sideways.
    Vec3 look = glm::length(view.forward) > 0.0F ? glm::normalize(view.forward) : Vec3(0.0F, 0.0F, -1.0F);
    if (m_hasCenter) {
        const DVec3 moved = view.position - m_lastPosition;
        if (glm::length(moved) > 1.0e-3) {
            look += 1.5F * Vec3(glm::normalize(moved));
        }
    }
    m_lastPosition = view.position;
    if (glm::length(look) > 0.0F) {
        m_look = glm::normalize(look);
    }
    const ChunkPos chunk = toChunk(BlockPos{.x = static_cast<i64>(std::floor(view.position.x)),
                                            .y = static_cast<i64>(std::floor(view.position.y)),
                                            .z = static_cast<i64>(std::floor(view.position.z))});
    if (!m_hasCenter || chunk != m_center) {
        m_center = chunk;
        m_hasCenter = true;
        reprioritize();
    }
}

// After the camera entered another chunk or the ranges changed: drop
// candidates that left the range, score the rest again, restart the scan
// from the nearest offset and walk all tracked chunks once (maintain()).
void StreamingManager::reprioritize() {
    VXE_PROFILE_FUNCTION();
    std::erase_if(m_pending, [this](Candidate& candidate) {
        const auto it = m_entries.find(candidate.position);
        if (it == m_entries.end() || it->second.stage != ChunkStage::Pending) {
            return true;
        }
        if (!within(candidate.position, m_rangeR + 2, m_rangeV + 2)) {
            m_unloadQueue.push_back(candidate.position); // Erased between maintenance passes.
            return true;
        }
        candidate.score = score(candidate.position);
        return false;
    });
    std::ranges::make_heap(m_pending, kLater);
    std::erase_if(m_meshQueue, [this](Candidate& candidate) {
        const auto it = m_entries.find(candidate.position);
        if (it == m_entries.end() || it->second.stage != ChunkStage::MeshQueued) {
            return true;
        }
        if (!wantsMesh(candidate.position, it->second)) {
            setStage(it->second, ChunkStage::Ready);
            return true;
        }
        candidate.score = score(candidate.position);
        return false;
    });
    std::ranges::make_heap(m_meshQueue, kLater);
    std::erase_if(m_lightQueue, [this](Candidate& candidate) {
        const auto it = m_entries.find(candidate.position);
        if (it == m_entries.end() || it->second.stage != ChunkStage::Lighting || it->second.lightRunning) {
            return true;
        }
        if (!wantsLight(candidate.position, it->second)) {
            setStage(it->second, ChunkStage::Ready);
            return true;
        }
        candidate.score = score(candidate.position);
        return false;
    });
    std::ranges::make_heap(m_lightQueue, kLater);
    m_scanCursor = 0;
    m_scanDone = false;
    m_scanGroundOnly = true;
    requestMaintenance();
}

void StreamingManager::requestMaintenance() {
    if (m_maintaining || !m_unloadQueue.empty()) {
        m_maintainAgain = true; // After the running pass and its unloads.
        return;
    }
    m_maintainCursor = 0;
    m_maintaining = true;
    m_maintainAgain = false;
    m_passRemesh = m_remeshAll;
    m_remeshAll = false;
    m_passRelight = m_relightAll;
    m_relightAll = false;
}

void StreamingManager::checkMeshInputs() {
    if (m_blocks == nullptr) {
        return;
    }
    const std::shared_ptr<const BlockRegistryView> view = m_blocks->current();
    const bool blockLight = m_settings.blockLight.get();
    if (view != m_view || blockLight != m_blockLightOn) {
        // New block data can change what emits light and what blocks it.
        m_view = view;
        m_blockLightOn = blockLight;
        relightAll();
        if (m_sink != nullptr) {
            remeshAll();
        }
    }
    if (m_sink != nullptr && meshOptions() != m_options) {
        m_options = meshOptions();
        remeshAll();
    }
}

void StreamingManager::relightAll() {
    m_relightAll = true;
    requestMaintenance();
}

void StreamingManager::remeshAll() {
    // In flight meshes of the old epoch are thrown away when they finish;
    // the walk over all chunks marks the meshed ones for a new mesh.
    ++m_meshEpoch;
    m_remeshAll = true;
    requestMaintenance();
}

void StreamingManager::update(const StreamingView& view) {
    VXE_PROFILE_FUNCTION();
    const Stopwatch stopwatch;
    const bool burst = m_bursting;
    const f64 budgetMs = static_cast<f64>(m_settings.budgetMs.get()) * (m_bursting ? kBurstFactor : 1U);
    m_deadline =
        Clock::now() + std::chrono::duration_cast<Duration>(std::chrono::duration<f64, std::milli>(budgetMs));

    readSettings();
    moveCenter(view);
    checkMeshInputs();
    // Finishing work first (stage bias): results in, then uploads and new
    // mesh jobs, and only then new chunks.
    collectLoads();
    collectLights();
    collectMeshes();
    processEditedChunks();
    processChangedChunks();
    uploadMeshes();
    makeMeshRoom();
    scheduleLights();
    scheduleMeshes();
    maintain();
    requestChunks();
    scheduleLoads();
    updateBurst();
    publishStats();

    const Duration elapsed = stopwatch.elapsed();
    if (!burst) {
        const f64 milliseconds = toMilliseconds(elapsed);
        ++m_times.updates;
        m_times.totalMilliseconds += milliseconds;
        m_times.maxMilliseconds = std::max(m_times.maxMilliseconds, milliseconds);
        if (milliseconds > 1.0) {
            ++m_times.overOneMillisecond;
        }
    }
    if (m_stats != nullptr && m_statIds.updateTime.isValid()) {
        m_stats->addTime(m_statIds.updateTime, elapsed);
    }
}

void StreamingManager::collectLoads() {
    VXE_PROFILE_FUNCTION();
    usize handled = 0;
    for (usize i = 0; i < m_busyLoadSlots.size();) {
        const u16 index = m_busyLoadSlots[i];
        LoadSlot& slot = *m_loadSlots[index];
        if (!slot.done.load(std::memory_order_acquire)) {
            ++i;
            continue;
        }
        if (handled > 0 && overBudget()) {
            return;
        }
        ++handled;
        const auto it = m_entries.find(slot.position);
        if (it != m_entries.end() && it->second.stage == ChunkStage::Loading && it->second.slot == index &&
            !slot.cancelled.load(std::memory_order_relaxed)) {
            insertLoaded(slot.position, it->second, slot);
        } else if (m_stats != nullptr && m_statIds.cancelled.isValid()) {
            m_stats->add(m_statIds.cancelled);
        }
        m_busyLoadSlots[i] = m_busyLoadSlots.back();
        m_busyLoadSlots.pop_back();
        m_freeLoadSlots.push_back(index);
    }
}

void StreamingManager::insertLoaded(ChunkPos position, Entry& entry, LoadSlot& slot) {
    m_world.insertChunk(position, std::move(slot.product.blocks),
                        slot.product.light ? std::move(*slot.product.light) : ChunkLight{});
    if (slot.product.surfaceEstimate) {
        m_world.setSurfaceEstimate(columnOf(position), *slot.product.surfaceEstimate);
    }
    setStage(entry, ChunkStage::Ready);
    entry.meshDirty = true;
    entry.lightDirty = true;
    // Count the neighbors that are there (and lit), and tell them about this
    // one. Chunks above and below the world never come, so they count as
    // there and lit.
    u8 neighbors = 0;
    u8 litNeighbors = 0;
    for (const auto& offset : kNeighborOffsets) {
        const auto at = offsetPosition(position, offset[0], offset[1], offset[2]);
        if (!at || !inWorldHeight(at->y)) {
            ++neighbors;
            ++litNeighbors;
            continue;
        }
        const auto it = m_entries.find(*at);
        if (it == m_entries.end() || !inWorld(it->second.stage)) {
            continue;
        }
        ++neighbors;
        litNeighbors = static_cast<u8>(litNeighbors + (it->second.lit ? 1U : 0U));
        VXE_ASSERT(it->second.neighbors < kNeighborCount, "A chunk has more than 26 neighbors");
        ++it->second.neighbors;
        considerWork(*at, it->second);
    }
    entry.neighbors = neighbors;
    entry.litNeighbors = litNeighbors;
    considerWork(position, entry);
    // Blocks arriving above lit chunks can shade them (moving upwards
    // streams in terrain above what is already lit). All air shades nothing.
    const ChunkStorage& blocks = m_world.findChunk(position)->blocks();
    if (!blocks.isUniform() || blocks.uniformState() != kAirState) {
        relightBelow(position, -2);
    }
    if (m_stats != nullptr && m_statIds.loadedPerFrame.isValid()) {
        m_stats->add(m_statIds.loadedPerFrame);
    }
}

void StreamingManager::considerWork(ChunkPos position, Entry& entry) {
    considerLight(position, entry);
    considerMesh(position, entry);
}

// Queues the chunk for light once all its neighbors are loaded: a light
// source up to 15 blocks away can reach it, which is at most one chunk over.
void StreamingManager::considerLight(ChunkPos position, Entry& entry) {
    if (entry.stage != ChunkStage::Ready || !entry.lightDirty || entry.neighbors != kNeighborCount ||
        !wantsLight(position, entry)) {
        return;
    }
    if (m_lightQueue.size() >= std::max<usize>(kMinPending, usize{4} * std::max(lightLimit(), 1U))) {
        m_lightBacklog = true;
        return;
    }
    setStage(entry, ChunkStage::Lighting);
    entry.lightRunning = false;
    m_lightQueue.push_back({.score = score(position), .position = position});
    std::ranges::push_heap(m_lightQueue, kLater);
}

// The chunk has light now. The first time, its neighbors learn it: a chunk
// is meshed only when it and all its neighbors are lit, so its mesh never
// has to be redone because a neighbor's light arrived late.
void StreamingManager::markLit(ChunkPos position, Entry& entry) {
    if (!entry.lit) {
        entry.lit = true;
        for (const auto& offset : kNeighborOffsets) {
            const auto at = offsetPosition(position, offset[0], offset[1], offset[2]);
            if (!at || !inWorldHeight(at->y)) {
                continue;
            }
            if (const auto it = m_entries.find(*at); it != m_entries.end() && inWorld(it->second.stage)) {
                VXE_ASSERT(it->second.litNeighbors < kNeighborCount,
                           "A chunk has more than 26 lit neighbors");
                ++it->second.litNeighbors;
                considerWork(*at, it->second);
            }
        }
        if (m_sink == nullptr) {
            sampleLatency(entry); // Without meshing, lit is done.
        }
    }
    considerWork(position, entry);
}

// Which block columns of the chunk's neighborhood see open sky above the
// neighborhood's top (two chunks above the chunk). From the column summaries:
// a column is open if no loaded block stands above the top, unless the
// loaded chunks of that chunk column do not reach up to the generator's
// surface estimate and the estimate is above the top (deep underground with
// the surface not loaded: dark).
SkyOpenings StreamingManager::skyOpenings(ChunkPos position, bool middleOnly) const {
    SkyOpenings open;
    const i64 top = (i64{position.y} + 2) * kChunkSize;
    const i32 reach = middleOnly ? 0 : 1;
    for (i32 dz = -reach; dz <= reach; ++dz) {
        for (i32 dx = -reach; dx <= reach; ++dx) {
            const ColumnSummary* column = m_world.findColumn({.x = position.x + dx, .z = position.z + dz});
            if (column == nullptr) {
                continue;
            }
            const i32 estimate = column->surfaceEstimate();
            const std::span<const i32> loaded = column->loadedChunkYs();
            const i64 loadedTop =
                loaded.empty() ? i64{std::numeric_limits<i32>::min()} : (i64{loaded.back()} + 1) * kChunkSize;
            const bool buried =
                estimate != ColumnSummary::kNoBlock && estimate >= top && loadedTop <= estimate;
            if (buried) {
                continue;
            }
            for (u32 bz = 0; bz < 16; ++bz) {
                for (u32 bx = 0; bx < 16; ++bx) {
                    const i32 highest = column->highestBlock(bx, bz);
                    if (highest == ColumnSummary::kNoBlock || highest < top) {
                        const auto x = static_cast<usize>((dx + 1) * 16) + bx;
                        const auto z = static_cast<usize>((dz + 1) * 16) + bz;
                        open.set((z * 48) + x);
                    }
                }
            }
        }
    }
    return open;
}

// The light of a chunk that is lit the same everywhere, worked out without a
// job: a chunk of one fully opaque block has no light in it (only its own
// glow); a chunk of clear air under open sky everywhere, with no light
// source in reach, has full sky light and no block light. Nothing otherwise.
std::optional<u8> StreamingManager::uniformLight(ChunkPos position, const ChunkNeighborhood& around) const {
    if (!m_view) {
        return std::nullopt;
    }
    const ChunkStorage& blocks = *around[13];
    if (!blocks.isUniform()) {
        return std::nullopt;
    }
    const BlockRenderInfo& info = m_view->info(blocks.uniformState());
    if (info.lightOpacity >= kMaxLightLevel) {
        return packLight(0, m_blockLightOn ? info.emittedLight : u8{0});
    }
    if (info.lightOpacity != 0 || info.emittedLight != 0) {
        return std::nullopt;
    }
    const SkyOpenings sky = skyOpenings(position, true);
    for (usize z = 16; z < 32; ++z) {
        for (usize x = 16; x < 32; ++x) {
            if (!sky.test((z * 48) + x)) {
                return std::nullopt;
            }
        }
    }
    if (m_blockLightOn && neighborhoodEmitsLight(around, *m_view)) {
        return std::nullopt;
    }
    return kOpenSkyLight;
}

// Takes lit chunks off the light queue: chunks with no light source in
// reach right here (block light 0 everywhere, no job), the others into job
// slots with copies of their neighborhood.
void StreamingManager::scheduleLights() {
    VXE_PROFILE_FUNCTION();
    usize handled = 0;
    while (!m_lightQueue.empty()) {
        if (handled > 0 && overBudget()) {
            break;
        }
        std::ranges::pop_heap(m_lightQueue, kLater);
        const Candidate candidate = m_lightQueue.back();
        m_lightQueue.pop_back();
        const auto it = m_entries.find(candidate.position);
        if (it == m_entries.end() || it->second.stage != ChunkStage::Lighting || it->second.lightRunning) {
            continue;
        }
        Entry& entry = it->second;
        if (entry.neighbors != kNeighborCount || !wantsLight(candidate.position, entry)) {
            setStage(entry, ChunkStage::Ready);
            continue;
        }
        ++handled;
        ChunkNeighborhood around{};
        std::array<const Chunk*, 27> chunks{};
        for (usize k = 0; k < 27; ++k) {
            const auto at = offsetPosition(candidate.position, static_cast<i64>(k % 3) - 1,
                                           static_cast<i64>(k / 9) - 1, static_cast<i64>((k / 3) % 3) - 1);
            chunks.at(k) = at ? m_world.findChunk(*at) : nullptr;
            around.at(k) = chunks.at(k) != nullptr ? &chunks.at(k)->blocks() : nullptr;
        }
        Chunk* chunk = m_world.findChunk(candidate.position);
        const std::optional<u8> uniform = uniformLight(candidate.position, around);
        if (uniform || !m_view) {
            if (uniform) {
                static_cast<void>(m_world.replaceLight(candidate.position, ChunkLight(*uniform)));
            }
            chunk->clearDirty(kDirtyLight);
            entry.lightDirty = false;
            setStage(entry, ChunkStage::Ready);
            if (m_stats != nullptr && m_statIds.lightSkipped.isValid()) {
                m_stats->add(m_statIds.lightSkipped);
            }
            markLit(candidate.position, entry);
            continue;
        }
        if (m_freeLightSlots.empty() || m_busyLightSlots.size() >= lightLimit()) {
            m_lightQueue.push_back(candidate); // Waits for a slot.
            std::ranges::push_heap(m_lightQueue, kLater);
            break;
        }
        const u16 index = m_freeLightSlots.back();
        m_freeLightSlots.pop_back();
        LightSlot& slot = *m_lightSlots[index];
        slot.position = candidate.position;
        slot.done.store(false, std::memory_order_relaxed);
        slot.cancelled.store(false, std::memory_order_relaxed);
        for (usize k = 0; k < 27; ++k) {
            slot.present.at(k) = chunks.at(k) != nullptr;
            if (chunks.at(k) != nullptr) {
                slot.chunks.at(k) = chunks.at(k)->blocks();
                slot.versions.at(k) = chunks.at(k)->blocksVersion();
            }
        }
        slot.blocks = m_view;
        slot.sky = skyOpenings(candidate.position, false);
        slot.blockLight = m_blockLightOn;
        chunk->clearDirty(kDirtyLight);
        entry.lightDirty = false;
        entry.lightRunning = true;
        entry.slot = index;
        m_busyLightSlots.push_back(index);
        m_jobs.schedule(m_jobCounter, [this, index] { runLight(index); }, jobs::Priority::High);
    }
    const usize cap = std::max<usize>(kMinPending, usize{4} * std::max(lightLimit(), 1U));
    if (m_lightBacklog && m_lightQueue.size() < cap / 2) {
        m_lightBacklog = false;
        requestMaintenance();
    }
}

void StreamingManager::runLight(u16 index) {
    LightSlot& slot = *m_lightSlots[index];
    if (!slot.cancelled.load(std::memory_order_relaxed)) {
        const Stopwatch stopwatch;
        thread_local ChunkLighter lighter;
        ChunkNeighborhood around{};
        for (usize k = 0; k < 27; ++k) {
            around.at(k) = slot.present.at(k) ? &slot.chunks.at(k) : nullptr;
        }
        lighter.compute(around, *slot.blocks, slot.sky, slot.blockLight, slot.result);
        if (m_stats != nullptr && m_statIds.lightTime.isValid()) {
            m_stats->addTime(m_statIds.lightTime, stopwatch.elapsed());
        }
    }
    slot.done.store(true, std::memory_order_release);
}

void StreamingManager::collectLights() {
    VXE_PROFILE_FUNCTION();
    usize handled = 0;
    for (usize i = 0; i < m_busyLightSlots.size();) {
        const u16 index = m_busyLightSlots[i];
        LightSlot& slot = *m_lightSlots[index];
        if (!slot.done.load(std::memory_order_acquire)) {
            ++i;
            continue;
        }
        if (handled > 0 && overBudget()) {
            return;
        }
        ++handled;
        m_busyLightSlots[i] = m_busyLightSlots.back();
        m_busyLightSlots.pop_back();
        m_freeLightSlots.push_back(index);
        slot.blocks.reset();
        const auto it = m_entries.find(slot.position);
        if (it == m_entries.end() || it->second.stage != ChunkStage::Lighting || !it->second.lightRunning ||
            it->second.slot != index || slot.cancelled.load(std::memory_order_relaxed)) {
            continue;
        }
        Entry& entry = it->second;
        entry.lightRunning = false;
        setStage(entry, ChunkStage::Ready);
        // Blocks that changed since the copy make the result outdated.
        bool current = true;
        for (usize k = 0; k < 27 && current; ++k) {
            const auto at = offsetPosition(slot.position, static_cast<i64>(k % 3) - 1,
                                           static_cast<i64>(k / 9) - 1, static_cast<i64>((k / 3) % 3) - 1);
            const Chunk* chunk = at ? m_world.findChunk(*at) : nullptr;
            current = (chunk != nullptr) == slot.present.at(k) &&
                      (chunk == nullptr || chunk->blocksVersion() == slot.versions.at(k));
        }
        if (!current) {
            entry.lightDirty = true;
            considerWork(slot.position, entry);
            continue;
        }
        ChunkLight light;
        light.assign(slot.result);
        static_cast<void>(m_world.replaceLight(slot.position, std::move(light)));
        if (m_stats != nullptr && m_statIds.lightJobs.isValid()) {
            m_stats->add(m_statIds.lightJobs);
        }
        markLit(slot.position, entry);
    }
}

// Block edits change the light up to 15 blocks around them, and the sky
// light of the columns below: the edited chunk and its 26 neighbors are lit
// again, and the chunks below them down to Graphics.Lighting.SkyRelightDepth
// chunks (Section 14.1: bounded relight).
void StreamingManager::processEditedChunks() {
    for (const ChunkPos position : m_world.editedChunks()) {
        if (Chunk* chunk = m_world.findChunk(position); chunk != nullptr) {
            chunk->clearDirty(kDirtyLight);
        }
        relightBelow(position, 1);
    }
    m_world.clearEditedChunks();
}

// Marks the lit chunks of the 3 x 3 chunk columns around `position` for new
// light, from `above` chunks above it down to the sky relight depth below
// the neighborhoods it is part of.
void StreamingManager::relightBelow(ChunkPos position, i32 above) {
    const i32 depth = std::max(m_settings.skyRelightDepth.get(), 0);
    for (i32 dy = above; dy >= -1 - depth; --dy) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const auto at = offsetPosition(position, dx, dy, dz);
                if (!at) {
                    continue;
                }
                const auto it = m_entries.find(*at);
                if (it == m_entries.end() || !inWorld(it->second.stage)) {
                    continue;
                }
                it->second.lightDirty = true;
                considerWork(*at, it->second);
            }
        }
    }
}

void StreamingManager::considerMesh(ChunkPos position, Entry& entry) {
    if (m_sink == nullptr || entry.stage != ChunkStage::Ready || !entry.meshDirty || !entry.lit ||
        entry.litNeighbors != kNeighborCount || !wantsMesh(position, entry)) {
        return;
    }
    // A full queue leaves the chunk Ready and dirty; a maintenance pass
    // finds it once the queue has room again. Capping the queue keeps
    // rescoring it on a chunk crossing cheap during a fast flight.
    if (m_meshQueue.size() >= meshQueueCap()) {
        m_meshBacklog = true;
        return;
    }
    setStage(entry, ChunkStage::MeshQueued);
    m_meshQueue.push_back({.score = score(position), .position = position});
    std::ranges::push_heap(m_meshQueue, kLater);
}

void StreamingManager::freeMeshSlot(u16 slot) {
    m_meshSlots[slot]->blocks.reset(); // A swapped out registry view can go.
    m_freeMeshSlots.push_back(slot);
}

void StreamingManager::sampleLatency(Entry& entry) {
    if (entry.latencyDone) {
        return;
    }
    entry.latencyDone = true;
    if (m_stats != nullptr && m_statIds.latency.isValid()) {
        m_stats->sample(m_statIds.latency, toMilliseconds(Clock::now() - entry.requested));
    }
}

void StreamingManager::collectMeshes() {
    VXE_PROFILE_FUNCTION();
    usize handled = 0;
    for (usize i = 0; i < m_busyMeshSlots.size();) {
        const u16 index = m_busyMeshSlots[i];
        const MeshSlot& slot = *m_meshSlots[index];
        if (!slot.done.load(std::memory_order_acquire)) {
            ++i;
            continue;
        }
        if (handled > 0 && overBudget()) {
            return;
        }
        ++handled;
        m_busyMeshSlots[i] = m_busyMeshSlots.back();
        m_busyMeshSlots.pop_back();

        const auto it = m_entries.find(slot.position);
        if (it == m_entries.end() || it->second.stage != ChunkStage::Meshing || it->second.slot != index ||
            slot.cancelled.load(std::memory_order_relaxed)) {
            if (m_stats != nullptr && m_statIds.cancelled.isValid()) {
                m_stats->add(m_statIds.cancelled);
            }
            freeMeshSlot(index);
            continue;
        }
        Entry& entry = it->second;
        const Chunk* chunk = m_world.findChunk(slot.position);
        const bool current = slot.epoch == m_meshEpoch && chunk != nullptr &&
                             chunk->version() == slot.snapshot.version && wantsMesh(slot.position, entry);
        if (!current) {
            // The chunk changed after its snapshot (or the mesh settings did,
            // or it left the range): a newer mesh follows if still wanted.
            if (m_stats != nullptr && m_statIds.stale.isValid()) {
                m_stats->add(m_statIds.stale);
            }
            setStage(entry, ChunkStage::Ready);
            entry.meshDirty = true;
            freeMeshSlot(index);
            considerWork(slot.position, entry);
            continue;
        }
        if (m_stats != nullptr && m_statIds.meshedPerFrame.isValid()) {
            m_stats->add(m_statIds.meshedPerFrame);
        }
        setStage(entry, ChunkStage::Uploading);
        m_uploadQueue.push_back(index);
    }
}

void StreamingManager::processChangedChunks() {
    for (const ChunkPos position : m_world.changedChunks()) {
        const auto it = m_entries.find(position);
        if (it == m_entries.end()) {
            continue;
        }
        it->second.meshDirty = true;
        considerWork(position, it->second);
    }
    m_world.clearChangedChunks();
}

void StreamingManager::uploadMeshes() {
    VXE_PROFILE_FUNCTION();
    if (m_sink == nullptr) {
        return;
    }
    usize used = 0;
    for (; used < m_uploadQueue.size(); ++used) {
        if (used > 0 && overBudget()) {
            break;
        }
        const u16 index = m_uploadQueue[used];
        const MeshSlot& slot = *m_meshSlots[index];
        const auto it = m_entries.find(slot.position);
        if (it == m_entries.end() || it->second.stage != ChunkStage::Uploading || it->second.slot != index) {
            freeMeshSlot(index);
            continue;
        }
        Entry& entry = it->second;
        if (!wantsMesh(slot.position, entry)) {
            setStage(entry, ChunkStage::Ready);
            entry.meshDirty = true;
            freeMeshSlot(index);
            continue;
        }
        const ChunkMeshSink::Result result = m_sink->upload(slot.mesh);
        if (result == ChunkMeshSink::Result::OverBudget) {
            break; // This one and the rest go next frame.
        }
        if (result == ChunkMeshSink::Result::Accepted) {
            if (!entry.meshed) {
                entry.meshed = true;
                ++m_counts.meshed;
            }
            sampleLatency(entry);
            if (m_stats != nullptr && m_statIds.uploadedPerFrame.isValid()) {
                m_stats->add(m_statIds.uploadedPerFrame);
            }
        } else {
            // Still wanted: it stays dirty and comes again after room is made,
            // whether it is a new chunk or an edit of one on screen.
            entry.meshDirty = true;
            m_meshRefused = true;
            setStage(entry, ChunkStage::Ready);
            freeMeshSlot(index);
            continue;
        }
        setStage(entry, ChunkStage::Ready);
        freeMeshSlot(index);
        considerWork(slot.position, entry);
    }
    m_uploadQueue.erase(m_uploadQueue.begin(), m_uploadQueue.begin() + static_cast<std::ptrdiff_t>(used));
}

// Called every update. When the renderer is out of space, real chunks stop one
// ring earlier and far terrain shows beyond them, so that the chunks nearest
// the player always have their meshes. Dropping a ring takes a moment to free
// memory, so the next step waits and a burst of refusals counts as one.
void StreamingManager::makeMeshRoom() {
    if (m_framesSinceShrink < kShrinkPause) {
        ++m_framesSinceShrink;
        return;
    }
    if (!m_meshRefused) {
        return;
    }
    m_meshRefused = false;
    m_framesSinceShrink = 0;
    // The floor stops the shrinking, not the retrying: room can still appear.
    if (meshReach() > kLeastMeshReach) {
        m_memoryReach = meshReach() - 1;
        VXE_LOG_WARN(log::core,
                     "Streaming: chunk memory is full; real chunks now reach {} chunks, far "
                     "terrain shows the rest",
                     m_memoryReach);
    } else {
        m_memoryReach = meshReach();
    }
    requestMaintenance(); // Drops the far rings and offers the refused chunks again.
}

void StreamingManager::scheduleMeshes() {
    VXE_PROFILE_FUNCTION();
    if (m_sink == nullptr) {
        return;
    }
    const u32 limit = meshLimit();
    usize scheduled = 0;
    while (!m_meshQueue.empty() && !m_freeMeshSlots.empty() &&
           m_meshSlots.size() - m_freeMeshSlots.size() < limit) {
        if (scheduled > 0 && overBudget()) {
            break;
        }
        std::ranges::pop_heap(m_meshQueue, kLater);
        const ChunkPos position = m_meshQueue.back().position;
        m_meshQueue.pop_back();
        const auto it = m_entries.find(position);
        if (it == m_entries.end() || it->second.stage != ChunkStage::MeshQueued) {
            continue;
        }
        Entry& entry = it->second;
        Chunk* chunk = m_world.findChunk(position);
        if (entry.litNeighbors != kNeighborCount || !entry.lit || !wantsMesh(position, entry) ||
            chunk == nullptr) {
            setStage(entry, ChunkStage::Ready);
            continue;
        }
        const u16 index = m_freeMeshSlots.back();
        m_freeMeshSlots.pop_back();
        MeshSlot& slot = *m_meshSlots[index];
        static_cast<void>(m_world.createSnapshot(position, slot.snapshot));
        // Changes from here on put the chunk on the world's changed list.
        chunk->clearDirty(kDirtyMesh);
        entry.meshDirty = false;
        slot.position = position;
        slot.done.store(false, std::memory_order_relaxed);
        slot.cancelled.store(false, std::memory_order_relaxed);
        slot.epoch = m_meshEpoch;
        slot.options = m_options;
        slot.blocks = m_view;
        entry.slot = index;
        setStage(entry, ChunkStage::Meshing);
        m_busyMeshSlots.push_back(index);
        m_jobs.schedule(m_jobCounter, [this, index] { runMesh(index); }, jobs::Priority::High);
        ++scheduled;
    }
    if (m_meshBacklog && m_meshQueue.size() < meshQueueCap() / 2) {
        m_meshBacklog = false;
        requestMaintenance();
    }
}

void StreamingManager::runMesh(u16 index) {
    MeshSlot& slot = *m_meshSlots[index];
    if (!slot.cancelled.load(std::memory_order_relaxed)) {
        workerMesher(m_id, m_stats).mesh(slot.snapshot, *slot.blocks, slot.options, slot.mesh);
    }
    slot.done.store(true, std::memory_order_release);
}

void StreamingManager::runLoad(u16 index) {
    LoadSlot& slot = *m_loadSlots[index];
    if (!slot.cancelled.load(std::memory_order_relaxed)) {
        m_source.produce(ChunkRequest(slot.position, slot.cancelled), slot.product);
    }
    slot.done.store(true, std::memory_order_release);
}

void StreamingManager::unmesh(ChunkPos position, Entry& entry) {
    if (entry.meshed) {
        m_sink->remove(position);
        entry.meshed = false;
        --m_counts.meshed;
    }
    entry.meshDirty = true;
    if (entry.stage == ChunkStage::MeshQueued) {
        setStage(entry, ChunkStage::Ready); // Its heap entry is skipped when popped.
    }
    // Meshing and Uploading chunks are thrown away when their turn comes,
    // since they are no longer wanted.
}

void StreamingManager::unload(ChunkPos position, Entry& entry) {
    if (entry.stage == ChunkStage::Loading) {
        m_loadSlots[entry.slot]->cancelled.store(true, std::memory_order_relaxed);
    } else if (entry.stage == ChunkStage::Meshing) {
        m_meshSlots[entry.slot]->cancelled.store(true, std::memory_order_relaxed);
    } else if (entry.stage == ChunkStage::Lighting && entry.lightRunning) {
        m_lightSlots[entry.slot]->cancelled.store(true, std::memory_order_relaxed);
    }
    if (inWorld(entry.stage)) {
        for (const auto& offset : kNeighborOffsets) {
            const auto at = offsetPosition(position, offset[0], offset[1], offset[2]);
            if (!at || !inWorldHeight(at->y)) {
                continue;
            }
            if (const auto it = m_entries.find(*at); it != m_entries.end() && inWorld(it->second.stage)) {
                --it->second.neighbors;
                if (entry.lit) {
                    --it->second.litNeighbors;
                }
            }
        }
        m_world.removeChunk(position);
        if (m_stats != nullptr && m_statIds.unloadedPerFrame.isValid()) {
            m_stats->add(m_statIds.unloadedPerFrame);
        }
    }
    if (entry.meshed) {
        m_sink->remove(position);
    }
    eraseEntry(position);
}

// One walk over every tracked chunk, a slice per update, after the camera
// changed chunk or the ranges or mesh inputs changed: take meshes back past
// the kept mesh range, queue meshes for chunks that came into the mesh
// range, and list chunks past the kept range for unloading. The unloads
// happen after the pass, before the next one starts: erasing moves entries
// around in the map, which would make a walk by index skip some. A pass
// asked for while one runs follows it, so passes always finish even when
// the camera crosses a chunk border every frame.
// One chunk of a maintenance pass.
void StreamingManager::maintainEntry(ChunkPos position, Entry& entry) {
    if (!within(position, m_rangeR + 2 + m_hysteresis, m_rangeV + 2 + m_hysteresis)) {
        m_unloadQueue.push_back(position);
        return;
    }
    if (entry.meshed && !within(position, meshKept(), m_rangeV + m_hysteresis)) {
        unmesh(position, entry);
    }
    if (m_passRemesh && entry.meshed) {
        entry.meshDirty = true;
    }
    if (m_passRelight && entry.lit) {
        entry.lightDirty = true;
    }
    considerWork(position, entry);
}

void StreamingManager::maintain() {
    VXE_PROFILE_FUNCTION();
    usize steps = 0;
    for (;;) {
        while (m_maintaining) {
            if (m_maintainCursor >= m_entries.size()) {
                m_maintaining = false;
                break;
            }
            if (steps > 0 && steps % kClockStride == 0 && overBudget()) {
                return;
            }
            ++steps;
            const auto it = m_entries.begin() + static_cast<std::ptrdiff_t>(m_maintainCursor++);
            maintainEntry(it->first, it->second);
        }
        while (!m_unloadQueue.empty()) {
            if (steps > 0 && overBudget()) {
                return;
            }
            const ChunkPos position = m_unloadQueue.back();
            m_unloadQueue.pop_back();
            const auto it = m_entries.find(position);
            if (it == m_entries.end()) {
                continue;
            }
            ++steps;
            if (it->second.stage == ChunkStage::Pending) {
                // Dropped from the pending heap; the scan asks again if it
                // is (or comes back) in range.
                eraseEntry(position);
                m_scanCursor = 0;
                m_scanDone = false;
                continue;
            }
            if (within(position, m_rangeR + 2 + m_hysteresis, m_rangeV + 2 + m_hysteresis)) {
                continue; // Back in range since it was listed.
            }
            unload(position, it->second);
        }
        if (!m_maintainAgain && !m_remeshAll && !m_relightAll) {
            return;
        }
        m_maintainAgain = false;
        requestMaintenance(); // Starts the next pass, run right away while time is left.
    }
}

// True for what the player sees first: chunks around the camera and around the
// ground of their column. Everything else is air or rock.
bool StreamingManager::isNearGround(ChunkPos position) {
    if (std::abs(i64{position.y} - m_center.y) <= kCameraLayers) {
        return true;
    }
    const ColumnPos column{.x = position.x, .z = position.z};
    auto it = m_groundLayers.find(column);
    if (it == m_groundLayers.end()) {
        // Forgets columns long left behind; the far side of a ring is at most a
        // few times the size of the view.
        const usize side = static_cast<usize>(m_rangeR) * 2 + 5;
        if (m_groundLayers.size() > side * side * 4) {
            m_groundLayers.clear();
        }
        const std::optional<i32> layer = m_source.surfaceLayer(position.x, position.z);
        it = m_groundLayers.emplace(column, layer.value_or(position.y)).first;
    }
    return std::abs(i64{position.y} - it->second) <= kGroundLayers;
}

// Walks the offsets from the nearest, adding every chunk not tracked yet
// to the pending heap until the heap is full, the chunk cap is reached or
// the time is up. The cursor stays where it stopped.
void StreamingManager::requestChunks() {
    VXE_PROFILE_FUNCTION();
    if (m_scanDone) {
        return;
    }
    const usize heapCap = std::max<usize>(kMinPending, usize{2} * loadLimit());
    const auto chunkCap = static_cast<usize>(std::max(m_settings.maxChunks.get(), 1));
    usize examined = 0;
    usize looked = 0;
    for (;;) {
        if (m_scanCursor >= m_offsets.size()) {
            // Everything near the ground is asked for; now all the rest.
            if (!m_scanGroundOnly) {
                break;
            }
            m_scanGroundOnly = false;
            m_scanCursor = 0;
        }
        if (m_pending.size() >= heapCap || m_entries.size() >= chunkCap || examined >= kMaxScanPerUpdate) {
            return;
        }
        if (++looked % kClockStride == 0 && overBudget()) {
            return;
        }
        const Offset offset = m_offsets[m_scanCursor++];
        const auto position = offsetPosition(m_center, offset.x, offset.y, offset.z);
        if (!position || !inWorldHeight(position->y)) {
            continue;
        }
        if (m_scanGroundOnly && !isNearGround(*position)) {
            continue;
        }
        ++examined;
        if (!m_entries.try_emplace(*position).second) {
            continue;
        }
        ++m_counts.stages.at(static_cast<usize>(ChunkStage::Pending));
        m_pending.push_back({.score = score(*position), .position = *position});
        std::ranges::push_heap(m_pending, kLater);
    }
    m_scanDone = true;
}

void StreamingManager::scheduleLoads() {
    VXE_PROFILE_FUNCTION();
    const u32 limit = loadLimit();
    usize scheduled = 0;
    while (!m_pending.empty() && !m_freeLoadSlots.empty() && m_busyLoadSlots.size() < limit) {
        if (scheduled > 0 && overBudget()) {
            return;
        }
        std::ranges::pop_heap(m_pending, kLater);
        const ChunkPos position = m_pending.back().position;
        m_pending.pop_back();
        const auto it = m_entries.find(position);
        if (it == m_entries.end() || it->second.stage != ChunkStage::Pending) {
            continue;
        }
        const u16 index = m_freeLoadSlots.back();
        m_freeLoadSlots.pop_back();
        LoadSlot& slot = *m_loadSlots[index];
        slot.position = position;
        slot.done.store(false, std::memory_order_relaxed);
        slot.cancelled.store(false, std::memory_order_relaxed);
        slot.product.origin = ChunkOrigin::Generated;
        slot.product.surfaceEstimate.reset();
        slot.product.light.reset();
        Entry& entry = it->second;
        entry.slot = index;
        entry.requested = Clock::now();
        setStage(entry, ChunkStage::Loading);
        m_busyLoadSlots.push_back(index);
        m_jobs.schedule(m_jobCounter, [this, index] { runLoad(index); }, jobs::Priority::Normal);
        ++scheduled;
    }
}

void StreamingManager::startBurst() {
    m_bursting = true;
    m_burstEnd = Clock::now() + std::chrono::duration_cast<Duration>(std::chrono::duration<f64>(
                                    static_cast<f64>(m_settings.burstSeconds.get())));
    m_burstClock.restart();
    if (m_sink != nullptr) {
        m_sink->setBudgetScale(static_cast<f32>(kBurstFactor));
    }
}

void StreamingManager::updateBurst() {
    if (!m_bursting || (Clock::now() < m_burstEnd && !isSettled())) {
        return;
    }
    m_bursting = false;
    if (m_sink != nullptr) {
        m_sink->setBudgetScale(1.0F);
    }
    VXE_LOG_INFO(log::core, "Streaming burst ended after {:.0f} ms: {} chunks loaded, {} meshed{}",
                 m_burstClock.elapsedMilliseconds(), m_counts.loaded, m_counts.meshed,
                 isSettled() ? "" : " (time limit)");
}

bool StreamingManager::isSettled() const noexcept {
    return m_hasCenter && m_scanDone && !m_maintaining && !m_maintainAgain && !m_remeshAll &&
           !m_meshRefused && !m_meshBacklog && !m_relightAll && !m_lightBacklog && m_unloadQueue.empty() &&
           m_pending.empty() && m_busyLoadSlots.empty() && m_busyLightSlots.empty() && m_lightQueue.empty() &&
           m_busyMeshSlots.empty() && m_uploadQueue.empty() && m_meshQueue.empty();
}

bool StreamingManager::isAreaReady(i32 radius, i32 layers) const {
    if (!m_hasCenter) {
        return false;
    }
    const i32 horizontal = std::clamp(radius, 0, renderDistance());
    const i32 vertical = layers < 0 ? verticalRenderDistance() : std::min(layers, verticalRenderDistance());
    for (i32 dy = -vertical; dy <= vertical; ++dy) {
        for (i32 dz = -horizontal; dz <= horizontal; ++dz) {
            for (i32 dx = -horizontal; dx <= horizontal; ++dx) {
                const auto position = offsetPosition(m_center, dx, dy, dz);
                if (!position || !inWorldHeight(position->y)) {
                    continue;
                }
                const auto it = m_entries.find(*position);
                if (it == m_entries.end() || !inWorld(it->second.stage) ||
                    (m_blocks != nullptr && !it->second.lit) || (m_sink != nullptr && !it->second.meshed)) {
                    return false;
                }
            }
        }
    }
    return true;
}

void StreamingManager::unloadAll() {
    m_memoryReach = kNoReachLimit;
    m_meshRefused = false;
    for (const u16 index : m_busyLoadSlots) {
        m_loadSlots[index]->cancelled.store(true, std::memory_order_relaxed);
    }
    for (const u16 index : m_busyMeshSlots) {
        m_meshSlots[index]->cancelled.store(true, std::memory_order_relaxed);
    }
    for (const u16 index : m_busyLightSlots) {
        m_lightSlots[index]->cancelled.store(true, std::memory_order_relaxed);
    }
    m_jobs.wait(m_jobCounter);
    for (const u16 index : m_busyLightSlots) {
        m_lightSlots[index]->blocks.reset();
        m_freeLightSlots.push_back(index);
    }
    m_busyLightSlots.clear();
    m_lightQueue.clear();
    m_world.clearEditedChunks();
    m_freeLoadSlots.insert(m_freeLoadSlots.end(), m_busyLoadSlots.begin(), m_busyLoadSlots.end());
    m_busyLoadSlots.clear();
    for (const u16 index : m_busyMeshSlots) {
        freeMeshSlot(index);
    }
    m_busyMeshSlots.clear();
    for (const u16 index : m_uploadQueue) {
        freeMeshSlot(index);
    }
    m_uploadQueue.clear();
    for (const auto& [position, entry] : m_entries) {
        if (entry.meshed) {
            m_sink->remove(position);
        }
        if (inWorld(entry.stage)) {
            m_world.removeChunk(position);
        }
    }
    m_entries.clear();
    m_counts = {};
    m_pending.clear();
    m_meshQueue.clear();
    m_world.clearChangedChunks();
    m_unloadQueue.clear();
    m_scanGroundOnly = true;
    m_scanCursor = 0;
    m_scanDone = false;
    m_maintaining = false;
    m_maintainAgain = false;
    m_passRemesh = false;
    m_remeshAll = false;
    m_meshBacklog = false;
    m_lightBacklog = false;
    m_relightAll = false;
    m_passRelight = false;
    m_hasCenter = false;
}

std::optional<ChunkStage> StreamingManager::stageOf(ChunkPos position) const {
    const auto it = m_entries.find(position);
    if (it == m_entries.end()) {
        return std::nullopt;
    }
    return it->second.stage;
}

bool StreamingManager::isLit(ChunkPos position) const {
    const auto it = m_entries.find(position);
    return it != m_entries.end() && it->second.lit;
}

bool StreamingManager::hasMesh(ChunkPos position) const {
    const auto it = m_entries.find(position);
    return it != m_entries.end() && it->second.meshed;
}

std::string StreamingManager::summary() const {
    return std::format(
        "Streaming: distance {}x{}, {} loaded, {} meshed, {} pending, {} loading, {} meshing, {} "
        "uploading{}",
        renderDistance(), verticalRenderDistance(), m_counts.loaded, m_counts.meshed,
        m_counts.at(ChunkStage::Pending), m_counts.at(ChunkStage::Loading),
        m_counts.at(ChunkStage::MeshQueued) + m_counts.at(ChunkStage::Meshing),
        m_counts.at(ChunkStage::Uploading), m_bursting ? ", burst" : "");
}

void StreamingManager::publishStats() {
    if (m_stats == nullptr) {
        return;
    }
    const auto set = [this](stats::StatId id, usize value) {
        if (id.isValid()) {
            m_stats->set(id, static_cast<f64>(value));
        }
    };
    for (usize stage = 0; stage < kChunkStageCount; ++stage) {
        set(m_statIds.stages.at(stage), m_counts.stages.at(stage));
    }
    set(m_statIds.loaded, m_counts.loaded);
    set(m_statIds.meshed, m_counts.meshed);
    set(m_statIds.burst, m_bursting ? 1 : 0);
    set(m_statIds.meshReach, static_cast<usize>(meshReach()));
}

} // namespace vxe::voxel
