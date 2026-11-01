#include "vxe/voxel/lod_premapper.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/log.hpp>
#include <vxe/core/profile.hpp>

#include <algorithm>
#include <cmath>

namespace vxe::voxel {
namespace {

// The distance from a point to the nearest block of a tile, in blocks.
f64 distanceTo(const LodTileKey& key, i64 x, i64 z) {
    const auto gap = [](i64 value, i64 low, i64 size) {
        if (value < low) {
            return static_cast<f64>(low - value);
        }
        return value >= low + size ? static_cast<f64>(value - (low + size - 1)) : 0.0;
    };
    const f64 dx = gap(x, key.originX(), key.tileSize());
    const f64 dz = gap(z, key.originZ(), key.tileSize());
    return std::sqrt((dx * dx) + (dz * dz));
}

} // namespace

LodPremapper::LodPremapper(const LodPremapDesc& desc)
    : m_jobs(*desc.jobs)
    , m_sampler(desc.sampler)
    , m_cache(desc.cache)
    , m_stats(desc.stats)
    , m_options(desc.options)
    , m_hash(lodCacheHash(desc.sampler->worldHash(), desc.options))
    , m_maxJobs(std::max(desc.maxJobsInFlight, 1U))
    , m_priority(desc.priority) {
    VXE_ASSERT(desc.jobs != nullptr && desc.sampler != nullptr && desc.cache != nullptr,
               "LodPremapper needs jobs, a sampler and a cache");
    LodClipmap::tilesAround(desc.clipmap, desc.centerX, desc.centerZ, m_tiles);
    std::erase_if(m_tiles, [&](const LodTileKey& key) {
        return distanceTo(key, desc.centerX, desc.centerZ) > static_cast<f64>(desc.radius);
    });
    // Nearest first, finer levels first at the same distance.
    std::ranges::stable_sort(m_tiles, [&](const LodTileKey& a, const LodTileKey& b) {
        const f64 da = distanceTo(a, desc.centerX, desc.centerZ);
        const f64 db = distanceTo(b, desc.centerX, desc.centerZ);
        return da != db ? da < db : a.level < b.level;
    });
    if (m_stats != nullptr) {
        const auto gauge = [this](std::string_view name, std::string_view description) {
            auto id = m_stats->registerStat(
                {.name = name, .description = description, .type = stats::StatType::Gauge});
            return id ? id.value() : stats::StatId{};
        };
        m_statIds = {.done =
                         gauge("Voxel.Lod.Premap.Done", "LOD tiles premapped so far (or already cached)."),
                     .skipped = gauge("Voxel.Lod.Premap.Skipped", "LOD tiles the premap found in the cache."),
                     .total = gauge("Voxel.Lod.Premap.Total", "LOD tiles the premap covers.")};
    }
}

LodPremapper::~LodPremapper() {
    m_cancelled.store(true, std::memory_order_relaxed);
    m_jobs.wait(m_jobCounter);
}

void LodPremapper::cancel() {
    m_cancelled.store(true, std::memory_order_relaxed);
}

void LodPremapper::update() {
    VXE_PROFILE_FUNCTION();
    for (usize i = 0; i < m_busy.size();) {
        if (!m_busy[i]->done.load(std::memory_order_acquire)) {
            ++i;
            continue;
        }
        m_free.push_back(m_busy[i]);
        m_busy[i] = m_busy.back();
        m_busy.pop_back();
    }
    while (!m_cancelled.load(std::memory_order_relaxed) && m_next < m_tiles.size() &&
           m_busy.size() < m_maxJobs.load(std::memory_order_relaxed)) {
        if (m_free.empty()) {
            m_slots.push_back(std::make_unique<Slot>());
            m_free.push_back(m_slots.back().get());
        }
        Slot* slot = m_free.back();
        m_free.pop_back();
        slot->key = m_tiles[m_next++];
        slot->done.store(false, std::memory_order_relaxed);
        m_busy.push_back(slot);
        m_jobs.schedule(m_jobCounter, [this, slot] { runCheck(*slot); }, {m_priority, {}, jobs::Pool::Io});
    }
    if (m_stats != nullptr) {
        const Progress now = progress();
        const auto set = [this](stats::StatId id, usize value) {
            if (id.isValid()) {
                m_stats->set(id, static_cast<f64>(value));
            }
        };
        set(m_statIds.done, now.done);
        set(m_statIds.skipped, now.skipped);
        set(m_statIds.total, now.total);
    }
}

// On an IO thread: a tile already in the cache is done.
void LodPremapper::runCheck(Slot& slot) {
    if (m_cancelled.load(std::memory_order_relaxed)) {
        slot.done.store(true, std::memory_order_release);
        return;
    }
    if (m_cache->contains(slot.key, m_hash)) {
        finish(slot, true, false);
        return;
    }
    Slot* job = &slot;
    m_jobs.schedule(m_jobCounter, [this, job] { runSample(*job); }, m_priority);
}

// On a worker: sample, then write on an IO thread.
void LodPremapper::runSample(Slot& slot) {
    VXE_PROFILE_SCOPE("LodPremapper::sample");
    LodTileBuilder builder(slot.key);
    const bool ok = m_sampler->sampleTile(slot.key, m_options, builder, m_cancelled);
    if (!ok || builder.cellsAdded() != kLodCellCount) {
        // Cancelled, or a sampler that gave up: not baked.
        if (!m_cancelled.load(std::memory_order_relaxed)) {
            m_failed.fetch_add(1, std::memory_order_relaxed);
        }
        slot.done.store(true, std::memory_order_release);
        return;
    }
    auto tile = std::make_shared<const LodTile>(std::move(builder).build());
    Slot* job = &slot;
    m_jobs.schedule(m_jobCounter, [this, job, tile = std::move(tile)] { runSave(*job, tile); },
                    {m_priority, {}, jobs::Pool::Io});
}

void LodPremapper::runSave(Slot& slot, const std::shared_ptr<const LodTile>& tile) {
    const Result<void> saved = m_cache->save(*tile, m_hash);
    if (!saved) {
        VXE_LOG_WARN(log::core, "LOD premap: saving tile {} {} {} failed: {}", slot.key.level, slot.key.x,
                     slot.key.z, saved.error().message());
    }
    finish(slot, false, !saved);
}

void LodPremapper::finish(Slot& slot, bool skipped, bool failed) {
    if (failed) {
        m_failed.fetch_add(1, std::memory_order_relaxed);
    } else {
        m_done.fetch_add(1, std::memory_order_relaxed);
        if (skipped) {
            m_skipped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    slot.done.store(true, std::memory_order_release);
}

LodPremapper::Progress LodPremapper::progress() const noexcept {
    return {.total = m_tiles.size(),
            .done = m_done.load(std::memory_order_relaxed),
            .skipped = m_skipped.load(std::memory_order_relaxed),
            .failed = m_failed.load(std::memory_order_relaxed)};
}

bool LodPremapper::isDone() const noexcept {
    return m_busy.empty() && (m_cancelled.load(std::memory_order_relaxed) || m_next >= m_tiles.size());
}

} // namespace vxe::voxel
