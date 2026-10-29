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
    , m_mesherSettings(*desc.settings) {
    VXE_ASSERT(desc.world != nullptr && desc.source != nullptr && desc.jobs != nullptr &&
                   desc.settings != nullptr,
               "StreamingManager needs a world, a source, jobs and settings");
    VXE_ASSERT(desc.sink == nullptr || desc.blocks != nullptr, "Meshing needs the block registry");
    VXE_ASSERT(desc.minChunkY <= desc.maxChunkY, "The world needs at least one chunk layer");
    constexpr u32 kMaxSlots = std::numeric_limits<u16>::max();
    const u32 loadSlots = std::clamp<u32>(desc.loadSlots, 1, kMaxSlots);
    const u32 meshSlots = std::clamp<u32>(desc.meshSlots, 1, kMaxSlots);
    registerSettings(*desc.settings, loadSlots, meshSlots);
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
        m_options = m_mesherSettings.options();
        m_view = m_blocks->current();
    }
}

StreamingManager::~StreamingManager() {
    for (const u16 index : m_busyLoadSlots) {
        m_loadSlots[index]->cancelled.store(true, std::memory_order_relaxed);
    }
    for (const u16 index : m_busyMeshSlots) {
        m_meshSlots[index]->cancelled.store(true, std::memory_order_relaxed);
    }
    m_jobs.wait(m_jobCounter);
}

void StreamingManager::registerSettings(config::SettingsRegistry& settings, u32 loadSlots, u32 meshSlots) {
    const auto integer = [&settings](std::string_view id, std::string_view name, std::string_view description,
                                     i32 value, i32 min, i32 max, config::Visibility visibility) {
        return settings.registerInt(
            {{.id = id, .displayName = name, .description = description, .visibility = visibility},
             value,
             min,
             max,
             1});
    };
    m_renderDistance =
        integer("World.Streaming.RenderDistance", "Render distance",
                "How far around the camera chunks are loaded and drawn, in chunks of 16 blocks.", 16, 2, 64,
                config::Visibility::Normal);
    m_verticalDistance = integer("World.Streaming.VerticalRenderDistance", "Vertical render distance",
                                 "How far up and down chunks are loaded and drawn, in chunks.", 8, 1, 32,
                                 config::Visibility::Normal);
    m_maxLoadJobs = integer("World.Streaming.MaxLoadJobsInFlight", "Load jobs in flight",
                            "Chunks generated or read at the same time. More fills the world in faster but "
                            "delays other jobs.",
                            std::min(48, static_cast<i32>(loadSlots)), 1, static_cast<i32>(loadSlots),
                            config::Visibility::Advanced);
    m_maxMeshJobs = integer("World.Streaming.MaxMeshJobsInFlight", "Mesh jobs in flight",
                            "Chunks meshed or waiting for upload at the same time.",
                            std::min(32, static_cast<i32>(meshSlots)), 1, static_cast<i32>(meshSlots),
                            config::Visibility::Advanced);
    m_maxChunks = integer("World.Streaming.MaxChunks", "Most chunks",
                          "The most chunks loaded or waiting to load, a memory cap. Normally the render "
                          "distance decides.",
                          262144, 1024, 4194304, config::Visibility::Advanced);
    m_unloadHysteresis = integer("World.Streaming.UnloadHysteresisChunks", "Unload margin",
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
    m_loadOrder = settings.registerEnum(order);
    m_budgetMs = settings.registerFloat({{.id = "World.Streaming.MainThreadBudgetMs",
                                          .displayName = "Main thread budget",
                                          .description = "Most time per frame the main thread spends on "
                                                         "streaming, in milliseconds. The rest waits for the "
                                                         "next frame.",
                                          .visibility = config::Visibility::Advanced},
                                         0.5F,
                                         0.05F,
                                         8.0F,
                                         0.05F});
    m_burstSeconds =
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
        gauge("Voxel.Streaming.MeshQueued", "Chunks waiting for a mesh job."),
        gauge("Voxel.Streaming.Meshing", "Chunks being meshed."),
        gauge("Voxel.Streaming.Uploading", "Meshes waiting for the upload budget."),
    };
    m_statIds.loaded = gauge("Voxel.Streaming.Loaded", "Chunks in the world.");
    m_statIds.meshed = gauge("Voxel.Streaming.Meshed", "Chunks with a mesh on the GPU.");
    m_statIds.loadedPerFrame = counter("Voxel.Streaming.ChunksLoaded", "Chunks put into the world.");
    m_statIds.meshedPerFrame = counter("Voxel.Streaming.ChunksMeshed", "Chunk meshes finished.");
    m_statIds.uploadedPerFrame = counter("Voxel.Streaming.ChunksUploaded", "Chunk meshes handed to the GPU.");
    m_statIds.unloadedPerFrame = counter("Voxel.Streaming.ChunksUnloaded", "Chunks taken out of the world.");
    m_statIds.cancelled = counter("Voxel.Streaming.Cancelled", "Jobs whose chunk left the range.");
    m_statIds.stale = counter("Voxel.Streaming.StaleMeshes", "Meshes thrown away because the chunk changed.");
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
    return std::max(m_renderDistance.get(), 1);
}

i32 StreamingManager::verticalRenderDistance() const noexcept {
    return std::max(m_verticalDistance.get(), 0);
}

u32 StreamingManager::loadLimit() const noexcept {
    const auto limit = static_cast<u32>(std::max(m_maxLoadJobs.get(), 1)) * (m_bursting ? kBurstFactor : 1U);
    return std::min(limit, static_cast<u32>(m_loadSlots.size()));
}

usize StreamingManager::meshQueueCap() const noexcept {
    return std::max<usize>(kMinPending, usize{4} * meshLimit());
}

u32 StreamingManager::meshLimit() const noexcept {
    const auto limit = static_cast<u32>(std::max(m_maxMeshJobs.get(), 1)) * (m_bursting ? kBurstFactor : 1U);
    return std::min(limit, static_cast<u32>(m_meshSlots.size()));
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

bool StreamingManager::wantsMesh(ChunkPos position, const Entry& entry) const noexcept {
    return within(position, m_rangeR, m_rangeV) ||
           (entry.meshed && within(position, m_rangeR + m_hysteresis, m_rangeV + m_hysteresis));
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
    const LoadOrder order = m_loadOrder.get();
    const i32 hysteresis = std::max(m_unloadHysteresis.get(), 0);
    const bool rangeChanged = range != m_rangeR || vertical != m_rangeV || order != m_order;
    if (!rangeChanged && hysteresis == m_hysteresis) {
        return;
    }
    m_rangeR = range;
    m_rangeV = vertical;
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
    const i32 horizontal = m_rangeR + 1;
    const i32 vertical = m_rangeV + 1;
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
        if (!within(candidate.position, m_rangeR + 1, m_rangeV + 1)) {
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
    m_scanCursor = 0;
    m_scanDone = false;
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
}

void StreamingManager::checkMeshInputs() {
    if (m_sink == nullptr) {
        return;
    }
    const MeshOptions options = m_mesherSettings.options();
    std::shared_ptr<const BlockRegistryView> view = m_blocks->current();
    if (options != m_options || view != m_view) {
        m_options = options;
        m_view = std::move(view);
        remeshAll();
    }
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
    const f64 budgetMs = static_cast<f64>(m_budgetMs.get()) * (m_bursting ? kBurstFactor : 1U);
    m_deadline =
        Clock::now() + std::chrono::duration_cast<Duration>(std::chrono::duration<f64, std::milli>(budgetMs));

    readSettings();
    moveCenter(view);
    checkMeshInputs();
    // Finishing work first (stage bias): results in, then uploads and new
    // mesh jobs, and only then new chunks.
    collectLoads();
    collectMeshes();
    processChangedChunks();
    uploadMeshes();
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
    // Count the neighbors that are there, and tell them about this one.
    // Chunks above and below the world never come, so they count as there.
    u8 neighbors = 0;
    for (const auto& offset : kNeighborOffsets) {
        const auto at = offsetPosition(position, offset[0], offset[1], offset[2]);
        if (!at || !inWorldHeight(at->y)) {
            ++neighbors;
            continue;
        }
        const auto it = m_entries.find(*at);
        if (it == m_entries.end() || !inWorld(it->second.stage)) {
            continue;
        }
        ++neighbors;
        VXE_ASSERT(it->second.neighbors < kNeighborCount, "A chunk has more than 26 neighbors");
        ++it->second.neighbors;
        considerMesh(*at, it->second);
    }
    entry.neighbors = neighbors;
    considerMesh(position, entry);
    if (m_stats != nullptr && m_statIds.loadedPerFrame.isValid()) {
        m_stats->add(m_statIds.loadedPerFrame);
    }
    if (m_sink == nullptr) {
        sampleLatency(entry); // Without meshing, loaded is done.
    }
}

void StreamingManager::considerMesh(ChunkPos position, Entry& entry) {
    if (m_sink == nullptr || entry.stage != ChunkStage::Ready || !entry.meshDirty ||
        entry.neighbors != kNeighborCount || !wantsMesh(position, entry)) {
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
            considerMesh(slot.position, entry);
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
        considerMesh(position, it->second);
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
        } else if (!m_warnedRefused) {
            m_warnedRefused = true;
            VXE_LOG_WARN(log::core, "Streaming: the renderer refused a chunk mesh (out of space); chunks "
                                    "will be missing until some unload");
        }
        setStage(entry, ChunkStage::Ready);
        freeMeshSlot(index);
        considerMesh(slot.position, entry);
    }
    m_uploadQueue.erase(m_uploadQueue.begin(), m_uploadQueue.begin() + static_cast<std::ptrdiff_t>(used));
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
        if (entry.neighbors != kNeighborCount || !wantsMesh(position, entry) || chunk == nullptr) {
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
    }
    if (inWorld(entry.stage)) {
        for (const auto& offset : kNeighborOffsets) {
            const auto at = offsetPosition(position, offset[0], offset[1], offset[2]);
            if (!at || !inWorldHeight(at->y)) {
                continue;
            }
            if (const auto it = m_entries.find(*at); it != m_entries.end() && inWorld(it->second.stage)) {
                --it->second.neighbors;
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
            const ChunkPos position = it->first;
            Entry& entry = it->second;
            if (!within(position, m_rangeR + 1 + m_hysteresis, m_rangeV + 1 + m_hysteresis)) {
                m_unloadQueue.push_back(position);
                continue;
            }
            if (entry.meshed && !within(position, m_rangeR + m_hysteresis, m_rangeV + m_hysteresis)) {
                unmesh(position, entry);
            }
            if (m_passRemesh && entry.meshed) {
                entry.meshDirty = true;
            }
            considerMesh(position, entry);
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
            if (within(position, m_rangeR + 1 + m_hysteresis, m_rangeV + 1 + m_hysteresis)) {
                continue; // Back in range since it was listed.
            }
            unload(position, it->second);
        }
        if (!m_maintainAgain && !m_remeshAll) {
            return;
        }
        m_maintainAgain = false;
        requestMaintenance(); // Starts the next pass, run right away while time is left.
    }
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
    const auto chunkCap = static_cast<usize>(std::max(m_maxChunks.get(), 1));
    usize examined = 0;
    while (m_scanCursor < m_offsets.size()) {
        if (m_pending.size() >= heapCap || m_entries.size() >= chunkCap || examined >= kMaxScanPerUpdate) {
            return;
        }
        if (examined > 0 && examined % kClockStride == 0 && overBudget()) {
            return;
        }
        ++examined;
        const Offset offset = m_offsets[m_scanCursor++];
        const auto position = offsetPosition(m_center, offset.x, offset.y, offset.z);
        if (!position || !inWorldHeight(position->y)) {
            continue;
        }
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
    m_burstEnd = Clock::now() + std::chrono::duration_cast<Duration>(
                                    std::chrono::duration<f64>(static_cast<f64>(m_burstSeconds.get())));
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
           !m_meshBacklog && m_unloadQueue.empty() && m_pending.empty() && m_busyLoadSlots.empty() &&
           m_busyMeshSlots.empty() && m_uploadQueue.empty() && m_meshQueue.empty();
}

bool StreamingManager::isAreaReady(i32 radius) const {
    if (!m_hasCenter) {
        return false;
    }
    const i32 horizontal = std::clamp(radius, 0, renderDistance());
    const i32 vertical = verticalRenderDistance();
    for (i32 dy = -vertical; dy <= vertical; ++dy) {
        for (i32 dz = -horizontal; dz <= horizontal; ++dz) {
            for (i32 dx = -horizontal; dx <= horizontal; ++dx) {
                const auto position = offsetPosition(m_center, dx, dy, dz);
                if (!position || !inWorldHeight(position->y)) {
                    continue;
                }
                const auto it = m_entries.find(*position);
                if (it == m_entries.end() || !inWorld(it->second.stage) ||
                    (m_sink != nullptr && !it->second.meshed)) {
                    return false;
                }
            }
        }
    }
    return true;
}

void StreamingManager::unloadAll() {
    for (const u16 index : m_busyLoadSlots) {
        m_loadSlots[index]->cancelled.store(true, std::memory_order_relaxed);
    }
    for (const u16 index : m_busyMeshSlots) {
        m_meshSlots[index]->cancelled.store(true, std::memory_order_relaxed);
    }
    m_jobs.wait(m_jobCounter);
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
    m_scanCursor = 0;
    m_scanDone = false;
    m_maintaining = false;
    m_maintainAgain = false;
    m_passRemesh = false;
    m_remeshAll = false;
    m_meshBacklog = false;
    m_hasCenter = false;
}

std::optional<ChunkStage> StreamingManager::stageOf(ChunkPos position) const {
    const auto it = m_entries.find(position);
    if (it == m_entries.end()) {
        return std::nullopt;
    }
    return it->second.stage;
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
}

} // namespace vxe::voxel
