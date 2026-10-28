#include "vxe/core/stats.hpp"

#include <vxe/core/assert.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <thread>

namespace vxe::stats {
namespace {

constexpr u32 kDirtyWords = StatRegistry::kMaxStats / 64;
constexpr usize kFrameSecondsHistory = 2048;
constexpr f64 kBucketsPerOctave = 4.0;

// Unique ids for registries, so a thread local cache that remembers "my slots
// in registry N" can never point into a newer registry at the same address.
std::atomic<u64>& instanceCounter() noexcept {
    static std::atomic<u64> counter{1};
    return counter;
}

usize bucketIndex(f64 value, f64 minimum) noexcept {
    if (!(value >= minimum)) { // Also catches NaN.
        return 0;
    }
    const f64 octaves = std::log2(value / minimum);
    const auto index = static_cast<usize>(1.0 + std::floor(octaves * kBucketsPerOctave));
    return std::min(index, HistogramSnapshot::kBuckets - 1);
}

} // namespace

// The slots one thread records into. Only that thread adds to them; the main
// thread empties them in endFrame. A slot is marked in `dirty` after every
// add, so endFrame only visits slots that changed.
struct StatRegistry::ThreadSlots {
    std::thread::id thread;
    std::array<std::atomic<i64>, kMaxStats> values{};
    std::array<std::atomic<i64>, kMaxStats> counts{};
    std::array<std::atomic<u64>, kDirtyWords> dirty{};
};

// Everything known about one stat. The atomics are written from any thread;
// the rest only at registration or by the main thread.
struct StatRegistry::Stat {
    StatInfo info;
    f64 histogramMin = 0.001;
    std::atomic<f64> gauge{0.0};
    std::unique_ptr<std::array<std::atomic<u64>, HistogramSnapshot::kBuckets>> buckets;
    std::vector<f64> history;
    usize historyHead = 0;
    usize historyFilled = 0;
    f64 lastValue = 0.0;
    u64 lastCount = 0;
};

f64 HistogramSnapshot::bucketUpperEdge(usize index) const noexcept {
    return minimumEdge * std::exp2(static_cast<f64>(index) / kBucketsPerOctave);
}

f64 HistogramSnapshot::percentile(f64 fraction) const noexcept {
    if (samples == 0) {
        return 0.0;
    }
    const auto target =
        static_cast<u64>(std::ceil(std::clamp(fraction, 0.0, 1.0) * static_cast<f64>(samples)));
    u64 cumulative = 0;
    for (usize i = 0; i < kBuckets; ++i) {
        cumulative += buckets[i];
        if (cumulative >= std::max<u64>(target, 1)) {
            return bucketUpperEdge(i);
        }
    }
    return bucketUpperEdge(kBuckets - 1);
}

StatRegistry::StatRegistry()
    : m_instance(instanceCounter().fetch_add(1, std::memory_order_relaxed))
    , m_stats(kMaxStats)
    , m_frameSums(kMaxStats, 0)
    , m_frameCounts(kMaxStats, 0)
    , m_frameSeconds(kFrameSecondsHistory, 0.0) {
    // The registry measures its own merge, which the overlay shows as the
    // cost of the stats system itself (budget: under 0.05 ms per frame).
    m_mergeTime = registerStat({.name = "Debug.Stats.MergeTime",
                                .description = "Time endFrame spends merging and closing all stats.",
                                .type = StatType::Timer,
                                .unit = StatUnit::Milliseconds})
                      .value();
}

StatRegistry::~StatRegistry() = default;

Result<StatId> StatRegistry::registerStat(const StatDesc& desc) {
    if (desc.name.empty()) {
        return Error(ErrorCode::InvalidArgument, "A stat needs a name");
    }
    const std::scoped_lock lock(m_mutex);
    const u32 count = m_count.load(std::memory_order_relaxed);
    for (u32 i = 0; i < count; ++i) {
        if (m_stats[i]->info.name == desc.name) {
            if (m_stats[i]->info.type != desc.type) {
                return Error(ErrorCode::AlreadyExists,
                             std::format("Stat '{}' is already registered with another type", desc.name));
            }
            return StatId{i};
        }
    }
    if (count >= kMaxStats) {
        return Error(ErrorCode::OutOfMemory,
                     std::format("Cannot register '{}': {} stats is the limit", desc.name, kMaxStats));
    }
    auto stat = std::make_unique<Stat>();
    stat->info = StatInfo{.name = std::string(desc.name),
                          .description = std::string(desc.description),
                          .type = desc.type,
                          .unit = desc.type == StatType::Timer ? StatUnit::Milliseconds : desc.unit};
    stat->histogramMin = desc.histogramMin > 0.0 ? desc.histogramMin : 0.001;
    stat->history.assign(std::max<u32>(desc.historyFrames, 1), 0.0);
    if (desc.type == StatType::Histogram) {
        stat->buckets = std::make_unique<std::array<std::atomic<u64>, HistogramSnapshot::kBuckets>>();
    }
    m_stats[count] = std::move(stat);
    // Release: a thread that sees the new count also sees the finished Stat.
    m_count.store(count + 1, std::memory_order_release);
    return StatId{count};
}

StatId StatRegistry::find(std::string_view name) const {
    const u32 count = m_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        if (m_stats[i]->info.name == name) {
            return StatId{i};
        }
    }
    return {};
}

StatRegistry::ThreadSlots& StatRegistry::localSlots() {
    // Fast path: this thread already looked up its slots in this registry.
    thread_local u64 cachedInstance = 0;
    thread_local ThreadSlots* cachedSlots = nullptr;
    if (cachedInstance == m_instance) {
        return *cachedSlots;
    }
    const std::scoped_lock lock(m_mutex);
    const std::thread::id self = std::this_thread::get_id();
    ThreadSlots* slots = nullptr;
    for (const auto& thread : m_threads) {
        if (thread->thread == self) {
            slots = thread.get();
        }
    }
    if (slots == nullptr) {
        m_threads.push_back(std::make_unique<ThreadSlots>());
        slots = m_threads.back().get();
        slots->thread = self;
    }
    cachedInstance = m_instance;
    cachedSlots = slots;
    return *slots;
}

void StatRegistry::record(StatId id, i64 amount, i64 count) {
    ThreadSlots& slots = localSlots();
    slots.values[id.index].fetch_add(amount, std::memory_order_relaxed);
    slots.counts[id.index].fetch_add(count, std::memory_order_relaxed);
    // Release: when endFrame sees this bit, it also sees the adds above. A
    // read-modify-write always acts on the newest value, so a bit cleared by
    // endFrame in the meantime is set again and nothing is ever lost.
    slots.dirty[id.index / 64].fetch_or(u64{1} << (id.index % 64), std::memory_order_release);
}

void StatRegistry::add(StatId counter, i64 amount) {
    if (!counter.isValid() || counter.index >= statCount()) {
        VXE_ASSERT(false, "Unknown stat id");
        return;
    }
    VXE_ASSERT(m_stats[counter.index]->info.type == StatType::Counter, "add() is for counters");
    record(counter, amount, 1);
}

void StatRegistry::set(StatId gauge, f64 value) noexcept {
    if (!gauge.isValid() || gauge.index >= statCount()) {
        VXE_ASSERT(false, "Unknown stat id");
        return;
    }
    VXE_ASSERT(m_stats[gauge.index]->info.type == StatType::Gauge, "set() is for gauges");
    m_stats[gauge.index]->gauge.store(value, std::memory_order_relaxed);
}

void StatRegistry::addTime(StatId timer, Duration duration) {
    if (!timer.isValid() || timer.index >= statCount()) {
        VXE_ASSERT(false, "Unknown stat id");
        return;
    }
    VXE_ASSERT(m_stats[timer.index]->info.type == StatType::Timer, "addTime() is for timers");
    record(timer, std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count(), 1);
}

void StatRegistry::sample(StatId histogram, f64 value) {
    if (!histogram.isValid() || histogram.index >= statCount()) {
        VXE_ASSERT(false, "Unknown stat id");
        return;
    }
    const Stat& stat = *m_stats[histogram.index];
    VXE_ASSERT(stat.info.type == StatType::Histogram, "sample() is for histograms");
    if (stat.buckets) {
        (*stat.buckets)[bucketIndex(value, stat.histogramMin)].fetch_add(1, std::memory_order_relaxed);
        record(histogram, 0, 1); // Counts samples per frame.
    }
}

void StatRegistry::mergeThreadSlots() {
    const std::scoped_lock lock(m_mutex);
    for (const auto& thread : m_threads) {
        for (u32 word = 0; word < kDirtyWords; ++word) {
            // Acquire pairs with the release in record(): the values below
            // include every add made before the bit was set.
            u64 bits = thread->dirty[word].exchange(0, std::memory_order_acquire);
            while (bits != 0) {
                const u32 index = word * 64 + static_cast<u32>(std::countr_zero(bits));
                bits &= bits - 1;
                m_frameSums[index] += thread->values[index].exchange(0, std::memory_order_relaxed);
                m_frameCounts[index] += thread->counts[index].exchange(0, std::memory_order_relaxed);
            }
        }
    }
}

void StatRegistry::closeStat(u32 index) {
    Stat& stat = *m_stats[index];
    f64 value = 0.0;
    u64 count = static_cast<u64>(std::max<i64>(m_frameCounts[index], 0));
    switch (stat.info.type) {
    case StatType::Counter:
        value = static_cast<f64>(m_frameSums[index]);
        break;
    case StatType::Timer:
        value = static_cast<f64>(m_frameSums[index]) / 1'000'000.0; // Nanoseconds to milliseconds.
        break;
    case StatType::Gauge:
        value = stat.gauge.load(std::memory_order_relaxed);
        count = 0;
        break;
    case StatType::Histogram:
        value = static_cast<f64>(count);
        break;
    }
    m_frameSums[index] = 0;
    m_frameCounts[index] = 0;
    stat.lastValue = value;
    stat.lastCount = count;
    stat.history[stat.historyHead] = value;
    stat.historyHead = (stat.historyHead + 1) % stat.history.size();
    stat.historyFilled = std::min(stat.historyFilled + 1, stat.history.size());
}

void StatRegistry::endFrame(f64 frameSeconds) {
    const TimePoint start = Clock::now();
    mergeThreadSlots();
    const u32 count = statCount();
    for (u32 i = 0; i < count; ++i) {
        closeStat(i);
    }
    m_frameSeconds[m_frameSecondsHead] = frameSeconds;
    m_frameSecondsHead = (m_frameSecondsHead + 1) % m_frameSeconds.size();
    ++m_framesClosed;
    // Lands in the next frame's value, which is fine for a cost readout.
    addTime(m_mergeTime, Clock::now() - start);
}

f64 StatRegistry::value(StatId id) const noexcept {
    return id.isValid() && id.index < statCount() ? m_stats[id.index]->lastValue : 0.0;
}

u64 StatRegistry::count(StatId id) const noexcept {
    return id.isValid() && id.index < statCount() ? m_stats[id.index]->lastCount : 0;
}

void StatRegistry::copyHistory(StatId id, std::vector<f64>& out) const {
    out.clear();
    if (!id.isValid() || id.index >= statCount()) {
        return;
    }
    const Stat& stat = *m_stats[id.index];
    const usize size = stat.history.size();
    out.reserve(stat.historyFilled);
    usize index = (stat.historyHead + size - stat.historyFilled) % size;
    for (usize i = 0; i < stat.historyFilled; ++i) {
        out.push_back(stat.history[index]);
        index = (index + 1) % size;
    }
}

f64 StatRegistry::average(StatId id, u32 frames) const noexcept {
    if (!id.isValid() || id.index >= statCount()) {
        return 0.0;
    }
    const Stat& stat = *m_stats[id.index];
    const usize n = std::min<usize>(frames, stat.historyFilled);
    if (n == 0) {
        return 0.0;
    }
    const usize size = stat.history.size();
    f64 sum = 0.0;
    for (usize i = 1; i <= n; ++i) {
        sum += stat.history[(stat.historyHead + size - i) % size];
    }
    return sum / static_cast<f64>(n);
}

f64 StatRegistry::maximum(StatId id, u32 frames) const noexcept {
    if (!id.isValid() || id.index >= statCount()) {
        return 0.0;
    }
    const Stat& stat = *m_stats[id.index];
    const usize n = std::min<usize>(frames, stat.historyFilled);
    const usize size = stat.history.size();
    f64 result = 0.0;
    for (usize i = 1; i <= n; ++i) {
        const f64 sample = stat.history[(stat.historyHead + size - i) % size];
        result = i == 1 ? sample : std::max(result, sample);
    }
    return result;
}

f64 StatRegistry::perSecond(StatId id) const noexcept {
    if (!id.isValid() || id.index >= statCount()) {
        return 0.0;
    }
    const Stat& stat = *m_stats[id.index];
    const usize frames =
        std::min({stat.historyFilled, static_cast<usize>(m_framesClosed), m_frameSeconds.size()});
    const usize statSize = stat.history.size();
    const usize secondsSize = m_frameSeconds.size();
    f64 sum = 0.0;
    f64 seconds = 0.0;
    for (usize i = 1; i <= frames && seconds < 1.0; ++i) {
        sum += stat.history[(stat.historyHead + statSize - i) % statSize];
        seconds += m_frameSeconds[(m_frameSecondsHead + secondsSize - i) % secondsSize];
    }
    return seconds > 0.0 ? sum / seconds : 0.0;
}

HistogramSnapshot StatRegistry::histogram(StatId id) const {
    HistogramSnapshot snapshot;
    if (!id.isValid() || id.index >= statCount() || !m_stats[id.index]->buckets) {
        return snapshot;
    }
    const Stat& stat = *m_stats[id.index];
    snapshot.minimumEdge = stat.histogramMin;
    for (usize i = 0; i < HistogramSnapshot::kBuckets; ++i) {
        snapshot.buckets[i] = (*stat.buckets)[i].load(std::memory_order_relaxed);
        snapshot.samples += snapshot.buckets[i];
    }
    return snapshot;
}

void StatRegistry::resetHistogram(StatId id) noexcept {
    if (id.isValid() && id.index < statCount() && m_stats[id.index]->buckets) {
        for (auto& bucket : *m_stats[id.index]->buckets) {
            bucket.store(0, std::memory_order_relaxed);
        }
    }
}

const StatRegistry::StatInfo& StatRegistry::info(StatId id) const {
    static const StatInfo kUnknown{.name = "unknown"};
    return id.isValid() && id.index < statCount() ? m_stats[id.index]->info : kUnknown;
}

} // namespace vxe::stats
