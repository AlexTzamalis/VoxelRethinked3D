#include "vxe/voxel/lod_manager.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/log.hpp>
#include <vxe/core/profile.hpp>
#include <vxe/core/time.hpp>

#include <algorithm>
#include <cmath>

namespace vxe::voxel {
namespace {

// The most job slots, whatever Graphics.LOD.MaxJobsInFlight says.
constexpr i32 kMaxSlots = 64;

// True if the block rectangles of two tiles overlap.
bool overlaps(const LodTileKey& a, const LodTileKey& b) noexcept {
    return a.originX() < b.originX() + b.tileSize() && b.originX() < a.originX() + a.tileSize() &&
           a.originZ() < b.originZ() + b.tileSize() && b.originZ() < a.originZ() + a.tileSize();
}

} // namespace

LodManager::LodManager(const LodManagerDesc& desc)
    : m_jobs(*desc.jobs)
    , m_sampler(desc.sampler)
    , m_cache(desc.cache)
    , m_sink(desc.sink)
    , m_stats(desc.stats)
    , m_clipmap(LodClipmapDesc{}) {
    VXE_ASSERT(desc.jobs != nullptr && desc.settings != nullptr && desc.sampler != nullptr,
               "LodManager needs jobs, settings and a sampler");
    registerSettings(*desc.settings);
    registerStats();
    m_appliedDesc = clipmapDesc();
    m_clipmap = LodClipmap(m_appliedDesc);
    m_appliedHash = currentHash();
}

LodManager::~LodManager() {
    for (const u16 index : m_busySlots) {
        m_slots[index]->cancelled.store(true, std::memory_order_relaxed);
    }
    m_jobs.wait(m_jobCounter);
}

void LodManager::registerSettings(config::SettingsRegistry& settings) {
    const auto integer = [&settings](std::string_view id, std::string_view name, std::string_view description,
                                     i32 value, i32 min, i32 max, i32 step, config::Visibility visibility) {
        return settings.registerInt(
            {{.id = id, .displayName = name, .description = description, .visibility = visibility},
             value,
             min,
             max,
             step});
    };
    m_enabled = settings.registerBool({{.id = "Graphics.LOD.Enabled",
                                        .displayName = "Far terrain",
                                        .description = "Shows terrain far beyond the loaded chunks, sampled "
                                                       "from the world seed (Section 13)."},
                                       true});
    m_levels = integer("Graphics.LOD.Levels", "Far terrain levels",
                       "How many levels of detail. Each level doubles the cell size and the reach.", 6, 1, 12,
                       1, config::Visibility::Normal);
    m_halfExtent = integer("Graphics.LOD.HalfExtentTiles", "Far terrain level size",
                           "Tiles from the camera to the edge of each level. More is farther and smoother "
                           "between levels, and costs tiles. Odd values count as the even value below.",
                           4, 2, 16, 2, config::Visibility::Advanced);
    m_maxSpans = integer("Graphics.LOD.MaxSpansPerCell", "Far terrain spans per cell",
                         "Vertical runs of terrain per cell: more keeps overhangs and floating islands.", 4,
                         1, static_cast<i32>(kLodMaxSpansPerCell), 1, config::Visibility::Advanced);
    m_caves = settings.registerBool({{.id = "Graphics.LOD.CavesInLod",
                                      .displayName = "Caves in far terrain",
                                      .description = "Carves caves into far terrain. Rarely visible from "
                                                     "far away, and slower to sample.",
                                      .visibility = config::Visibility::Advanced},
                                     false});
    m_maxJobs = integer("Graphics.LOD.MaxJobsInFlight", "Far terrain jobs",
                        "Tiles loaded or sampled at the same time. They run below chunk work.", 4, 1,
                        kMaxSlots, 1, config::Visibility::Developer);
}

void LodManager::registerStats() {
    if (m_stats == nullptr) {
        return;
    }
    const auto add = [this](const stats::StatDesc& statDesc) {
        auto id = m_stats->registerStat(statDesc);
        return id ? id.value() : stats::StatId{};
    };
    const auto gauge = [&](std::string_view name, std::string_view description,
                           stats::StatUnit unit = stats::StatUnit::Count) {
        return add({.name = name, .description = description, .type = stats::StatType::Gauge, .unit = unit});
    };
    m_statIds.tiles = gauge("Voxel.Lod.Tiles", "LOD tiles ready in the clipmap.");
    m_statIds.queued = gauge("Voxel.Lod.Queued", "LOD tiles waiting for a job.");
    m_statIds.running = gauge("Voxel.Lod.Running", "LOD tiles loading or sampling.");
    m_statIds.retiring = gauge("Voxel.Lod.Retiring", "LOD tiles out of the clipmap, shown until replaced.");
    m_statIds.memory =
        gauge("Voxel.Lod.MemoryBytes", "Memory of the shown LOD tiles.", stats::StatUnit::Bytes);
    m_statIds.sampled =
        add({.name = "Voxel.Lod.Sampled", .description = "LOD tiles sampled from the world seed."});
    m_statIds.fromCache =
        add({.name = "Voxel.Lod.FromCache", .description = "LOD tiles loaded from the cache."});
    m_statIds.sampleTime = add({.name = "Voxel.Lod.SampleTime",
                                .description = "Time to sample one LOD tile, on a worker.",
                                .type = stats::StatType::Timer,
                                .unit = stats::StatUnit::Milliseconds});
}

LodClipmapDesc LodManager::clipmapDesc() const {
    const i32 half = std::max(2, m_halfExtent.get() & ~1);
    const i32 levels = std::clamp(m_levels.get(), 1, static_cast<i32>(kLodMaxLevel));
    return {.firstLevel = kLodMinLevel, .levels = static_cast<u8>(levels), .halfExtentTiles = half};
}

LodSampleOptions LodManager::sampleOptions() const {
    return {.maxSpansPerCell = static_cast<u32>(std::clamp<i32>(m_maxSpans.get(), 1, kLodMaxSpansPerCell)),
            .caves = m_caves.get()};
}

u64 lodCacheHash(u64 samplerHash, const LodSampleOptions& options) noexcept {
    return hashCombine(hashCombine(samplerHash, options.maxSpansPerCell), options.caves ? 1U : 0U);
}

// Tiles sampled with other options are stale, in memory and in the cache.
u64 LodManager::currentHash() const {
    return lodCacheHash(m_sampler->worldHash(), sampleOptions());
}

void LodManager::update(const DVec3& cameraPosition) {
    VXE_PROFILE_FUNCTION();
    m_camera = cameraPosition;
    collect();
    if (!m_enabled.get()) {
        if (m_wasEnabled) {
            clearAll();
        }
        m_wasEnabled = false;
        publishStats();
        return;
    }
    m_wasEnabled = true;
    if (const u64 hash = currentHash(); hash != m_appliedHash) {
        m_appliedHash = hash;
        restartAll();
    }
    if (const LodClipmapDesc desc = clipmapDesc(); desc != m_appliedDesc) {
        m_appliedDesc = desc;
        m_clipmap.setDesc(desc);
    }
    if (m_clipmap.update(static_cast<i64>(std::floor(cameraPosition.x)),
                         static_cast<i64>(std::floor(cameraPosition.z)))) {
        applyClipmapChanges();
    }
    retire();
    schedule();
    publishStats();
}

// Gives up every tile (Graphics.LOD.Enabled turned off). Running jobs are
// cancelled; their slots come back through collect().
void LodManager::clearAll() {
    for (const u16 index : m_busySlots) {
        m_slots[index]->cancelled.store(true, std::memory_order_relaxed);
    }
    if (m_sink != nullptr) {
        for (const auto& [key, shown] : m_ready) {
            m_sink->removeTile(key);
        }
    }
    m_ready.clear();
    m_wanted.clear();
    m_retiring.clear();
    m_queue.clear();
    m_queueHead = 0;
    m_clipmap = LodClipmap(m_appliedDesc); // Its next update adds every tile again.
}

// The sampling options or the sampler changed: every tile is stale. The old
// ones stay shown until their replacements arrive (addTile replaces a key).
void LodManager::restartAll() {
    for (const u16 index : m_busySlots) {
        m_slots[index]->cancelled.store(true, std::memory_order_relaxed);
    }
    for (auto& [key, wanted] : m_wanted) {
        wanted = {};
    }
    m_queueDirty = true;
}

void LodManager::applyClipmapChanges() {
    for (const LodTileKey& key : m_clipmap.removed()) {
        const auto it = m_wanted.find(key);
        if (it != m_wanted.end() && it->second.running) {
            cancelSlot(key);
        }
        m_wanted.erase(key);
        if (m_ready.contains(key)) {
            m_retiring.push_back(key); // Shown until the ground it covers is ready again.
        }
    }
    for (const LodTileKey& key : m_clipmap.added()) {
        Wanted wanted;
        if (const auto shown = m_ready.find(key); shown != m_ready.end()) {
            // Back before it retired: keep it if it is current.
            std::erase(m_retiring, key);
            wanted.ready = shown->second.hash == m_appliedHash;
        }
        m_wanted.emplace(key, wanted);
    }
    m_queueDirty = true;
    m_retireDirty = true;
}

void LodManager::cancelSlot(const LodTileKey& key) {
    for (const u16 index : m_busySlots) {
        if (m_slots[index]->key == key) {
            m_slots[index]->cancelled.store(true, std::memory_order_relaxed);
        }
    }
}

// Takes finished slots back and shows their tiles.
void LodManager::collect() {
    for (usize i = 0; i < m_busySlots.size();) {
        const u16 index = m_busySlots[i];
        Slot& slot = *m_slots[index];
        if (!slot.done.load(std::memory_order_acquire)) {
            ++i;
            continue;
        }
        m_busySlots[i] = m_busySlots.back();
        m_busySlots.pop_back();
        m_freeSlots.push_back(index);
        finish(slot);
        slot.tile.reset();
    }
}

void LodManager::finish(Slot& slot) {
    const auto it = m_wanted.find(slot.key);
    if (it == m_wanted.end()) {
        return;
    }
    it->second.running = false;
    if (slot.hash != m_appliedHash || slot.cancelled.load(std::memory_order_relaxed)) {
        m_queueDirty = true; // Outdated: sampled again.
        return;
    }
    if (slot.tile == nullptr) {
        return; // The sampler gave up: tried again when the queue is next rebuilt.
    }
    it->second.ready = true;
    std::shared_ptr<const LodTile> tile = std::move(slot.tile);
    m_ready.insert_or_assign(slot.key, Shown{.tile = tile, .hash = slot.hash});
    if (m_sink != nullptr) {
        m_sink->addTile(tile);
    }
    if (m_stats != nullptr) {
        const stats::StatId id = slot.sampled ? m_statIds.sampled : m_statIds.fromCache;
        if (id.isValid()) {
            m_stats->add(id);
        }
    }
    if (slot.sampled && m_cache != nullptr) {
        saveTile(std::move(tile), slot.hash);
    }
    m_retireDirty = true;
}

// Writes a sampled tile to the cache on an IO thread. The job holds the tile,
// so it may outlive the tile's place in the clipmap.
void LodManager::saveTile(std::shared_ptr<const LodTile> tile, u64 hash) {
    m_savesRunning.fetch_add(1, std::memory_order_relaxed);
    m_jobs.schedule(m_jobCounter,
                    [this, tile = std::move(tile), hash] {
                        VXE_PROFILE_SCOPE("LodManager::save");
                        if (auto saved = m_cache->save(*tile, hash); !saved) {
                            VXE_LOG_WARN(log::core, "LOD cache: saving tile {} {} {} failed: {}",
                                         tile->key().level, tile->key().x, tile->key().z,
                                         saved.error().message());
                        }
                        m_savesRunning.fetch_sub(1, std::memory_order_release);
                    },
                    {jobs::Priority::Low, {}, jobs::Pool::Io});
}

// Retiring tiles leave the sink once every wanted tile on their ground is
// ready (or none is: the camera went far away).
void LodManager::retire() {
    if (!m_retireDirty) {
        return;
    }
    m_retireDirty = false;
    for (usize i = 0; i < m_retiring.size();) {
        const LodTileKey key = m_retiring[i];
        if (!replaced(key)) {
            ++i;
            continue;
        }
        m_retiring[i] = m_retiring.back();
        m_retiring.pop_back();
        m_ready.erase(key);
        if (m_sink != nullptr) {
            m_sink->removeTile(key);
        }
    }
}

bool LodManager::replaced(const LodTileKey& key) const {
    return std::ranges::all_of(m_clipmap.tiles(), [&](const LodTileKey& other) {
        if (!overlaps(key, other)) {
            return true;
        }
        const auto it = m_wanted.find(other);
        return it != m_wanted.end() && it->second.ready;
    });
}

// The wanted tiles without a job, nearest first: by the distance in tiles of
// their own level (ring), then finer levels first.
void LodManager::sortQueue() {
    m_queue.clear();
    m_queueHead = 0;
    for (const auto& [key, wanted] : m_wanted) {
        if (!wanted.ready && !wanted.running) {
            m_queue.push_back(key);
        }
    }
    const auto ring = [this](const LodTileKey& key) {
        const auto size = static_cast<f64>(key.tileSize());
        const f64 dx = std::abs(m_camera.x - (static_cast<f64>(key.originX()) + (size * 0.5)));
        const f64 dz = std::abs(m_camera.z - (static_cast<f64>(key.originZ()) + (size * 0.5)));
        return static_cast<i64>(std::max(dx, dz) / size);
    };
    std::ranges::sort(m_queue, [&](const LodTileKey& a, const LodTileKey& b) {
        const i64 ringA = ring(a);
        const i64 ringB = ring(b);
        return ringA != ringB ? ringA < ringB : a.level < b.level;
    });
    m_queueDirty = false;
}

void LodManager::schedule() {
    if (m_queueDirty) {
        sortQueue();
    }
    const auto limit = static_cast<usize>(std::clamp(m_maxJobs.get(), 1, kMaxSlots));
    while (m_queueHead < m_queue.size() && m_busySlots.size() < limit) {
        const LodTileKey key = m_queue[m_queueHead++];
        const auto it = m_wanted.find(key);
        if (it == m_wanted.end() || it->second.ready || it->second.running) {
            continue;
        }
        if (m_freeSlots.empty()) {
            m_slots.push_back(std::make_unique<Slot>());
            m_freeSlots.push_back(static_cast<u16>(m_slots.size() - 1));
        }
        const u16 index = m_freeSlots.back();
        m_freeSlots.pop_back();
        Slot& slot = *m_slots[index];
        slot.key = key;
        slot.hash = m_appliedHash;
        slot.options = sampleOptions();
        slot.sampled = false;
        slot.done.store(false, std::memory_order_relaxed);
        slot.cancelled.store(false, std::memory_order_relaxed);
        it->second.running = true;
        m_busySlots.push_back(index);
        Slot* job = &slot;
        if (m_cache != nullptr) {
            m_jobs.schedule(m_jobCounter, [this, job] { runLoad(*job); },
                            {jobs::Priority::Low, {}, jobs::Pool::Io});
        } else {
            m_jobs.schedule(m_jobCounter, [this, job] { runSample(*job); }, jobs::Priority::Low);
        }
    }
}

// On an IO thread: the cached tile, or on a miss a sampling job.
void LodManager::runLoad(Slot& slot) {
    if (!slot.cancelled.load(std::memory_order_relaxed)) {
        if (auto cached = m_cache->load(slot.key, slot.hash)) {
            slot.tile = std::make_shared<LodTile>(std::move(*cached));
            slot.done.store(true, std::memory_order_release);
            return;
        }
        // Scheduled before this job finishes, so the counter never reaches
        // zero in between and the destructor's wait stays correct.
        Slot* job = &slot;
        m_jobs.schedule(m_jobCounter, [this, job] { runSample(*job); }, jobs::Priority::Low);
        return;
    }
    slot.done.store(true, std::memory_order_release);
}

// On a worker: the sampler fills the tile.
void LodManager::runSample(Slot& slot) {
    if (!slot.cancelled.load(std::memory_order_relaxed)) {
        VXE_PROFILE_SCOPE("LodManager::sample");
        const Stopwatch stopwatch;
        LodTileBuilder builder(slot.key);
        const bool ok = m_sampler->sampleTile(slot.key, slot.options, builder, slot.cancelled);
        if (ok && builder.cellsAdded() == kLodCellCount) {
            slot.tile = std::make_shared<LodTile>(std::move(builder).build());
            slot.sampled = true;
        } else if (ok && !m_warnedIncomplete.exchange(true)) {
            VXE_LOG_ERROR(log::core, "LOD sampler returned tile {} {} {} with {} of {} cells; dropped",
                          slot.key.level, slot.key.x, slot.key.z, builder.cellsAdded(), kLodCellCount);
        }
        if (m_stats != nullptr && m_statIds.sampleTime.isValid()) {
            m_stats->addTime(m_statIds.sampleTime, stopwatch.elapsed());
        }
    }
    slot.done.store(true, std::memory_order_release);
}

std::shared_ptr<const LodTile> LodManager::tile(const LodTileKey& key) const {
    const auto it = m_ready.find(key);
    return it == m_ready.end() ? nullptr : it->second.tile;
}

LodManager::Counts LodManager::counts() const {
    Counts counts;
    counts.wanted = m_wanted.size();
    counts.retiring = m_retiring.size();
    counts.running = m_busySlots.size() + m_savesRunning.load(std::memory_order_acquire);
    for (const auto& [key, wanted] : m_wanted) {
        if (wanted.ready) {
            ++counts.ready;
            ++counts.readyPerLevel.at(key.level);
        } else if (!wanted.running) {
            ++counts.queued;
        }
    }
    return counts;
}

bool LodManager::isSettled() const noexcept {
    return m_busySlots.empty() && m_savesRunning.load(std::memory_order_acquire) == 0 && m_retiring.empty() &&
           std::ranges::all_of(m_wanted, [](const auto& entry) { return entry.second.ready; });
}

void LodManager::publishStats() {
    if (m_stats == nullptr) {
        return;
    }
    usize memory = 0;
    for (const auto& [key, shown] : m_ready) {
        memory += shown.tile->memoryBytes();
    }
    const Counts now = counts();
    const auto set = [this](stats::StatId id, usize value) {
        if (id.isValid()) {
            m_stats->set(id, static_cast<f64>(value));
        }
    };
    set(m_statIds.tiles, now.ready);
    set(m_statIds.queued, now.queued);
    set(m_statIds.running, now.running);
    set(m_statIds.retiring, now.retiring);
    set(m_statIds.memory, memory);
}

} // namespace vxe::voxel
