#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/time.hpp>
#include <vxe/core/types.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The stat registry (Section 18.1, E7.1): named numbers every system reports
// so the debug overlay, CSV export and Tracy plots can show where time and
// memory go.
//
// It lives in core, not in the debug module, because every layer records
// stats (jobs, the RHI, the renderer, voxel streaming) and modules may only
// depend on lower layers (ADR-015, ADR-017). The overlay pages that display
// the numbers stay in the debug module.
namespace vxe::stats {

enum class StatType : u8 {
    Counter,   // Summed per frame, then reset: draw calls, chunks meshed, bytes uploaded.
    Gauge,     // Holds its last value until set again: resident chunks, memory in use.
    Timer,     // Total time per frame plus how often it was measured: mesh time, merge time.
    Histogram, // Distribution of samples since the last reset: chunk latency, frame times.
};

enum class StatUnit : u8 {
    Count,
    Bytes,
    Milliseconds, // Timers always report milliseconds.
    Percent,
};

struct StatDesc {
    // Dotted name; the part before the last dot is the category, which groups
    // stats on overlay pages. For example "Streaming.ChunksMeshed".
    std::string_view name{};
    std::string_view description{};
    StatType type = StatType::Counter;
    StatUnit unit = StatUnit::Count;
    // How many past frames the overlay graphs can show. The frame time graph
    // uses 1000 (Section 18.2); most stats need far fewer.
    u32 historyFrames = 240;
    // Histograms only: the lower edge of the second bucket. Buckets grow by a
    // quarter octave, so 64 buckets cover histogramMin to histogramMin * 65536.
    f64 histogramMin = 0.001;
};

struct StatId {
    static constexpr u32 kInvalid = 0xFFFFFFFF;
    u32 index = kInvalid;

    [[nodiscard]] constexpr bool isValid() const noexcept { return index != kInvalid; }
    friend constexpr bool operator==(StatId, StatId) = default;
};

// A copy of a histogram, for the overlay and tests.
struct HistogramSnapshot {
    static constexpr usize kBuckets = 64;
    std::array<u64, kBuckets> buckets{};
    u64 samples = 0;
    f64 minimumEdge = 0.0;

    // The value below which `fraction` (0 to 1) of the samples fall, with the
    // precision of one bucket (about 19 percent at quarter octaves).
    [[nodiscard]] f64 percentile(f64 fraction) const noexcept;
    // The upper edge of bucket i.
    [[nodiscard]] f64 bucketUpperEdge(usize index) const noexcept;
};

// Registers stats, records them from any thread and turns them into one value
// per frame with a history.
//
// Recording is lock free and nearly contention free: every thread writes into
// its own slots (created on the thread's first record) and marks them in a
// per thread dirty bitset. endFrame() on the main thread collects only the
// marked slots, so its cost follows the number of stats actually touched.
// Gauges and histograms live in shared atomics because they are set rarely.
//
// Thread safety: registerStat and the record functions may be called from any
// thread. endFrame and every query belong to the main thread.
class StatRegistry {
public:
    static constexpr u32 kMaxStats = 2048;

    StatRegistry();
    ~StatRegistry();
    StatRegistry(const StatRegistry&) = delete;
    StatRegistry& operator=(const StatRegistry&) = delete;
    StatRegistry(StatRegistry&&) = delete;
    StatRegistry& operator=(StatRegistry&&) = delete;

    // Registers a stat, or returns the existing one with the same name and
    // type, so systems can register in their constructors without caring who
    // was first. Fails for an empty name, a name taken by another type, or
    // when kMaxStats are registered.
    [[nodiscard]] Result<StatId> registerStat(const StatDesc& desc);
    [[nodiscard]] StatId find(std::string_view name) const;

    // Recording, from any thread. Invalid ids and type mismatches are ignored
    // in release builds and assert in debug builds. A thread's first record
    // allocates its slots (about 33 KB), so threads that record in hot loops
    // should record once at startup to keep frames allocation free.
    void add(StatId counter, i64 amount = 1);
    void set(StatId gauge, f64 value) noexcept;
    void addTime(StatId timer, Duration duration);
    void sample(StatId histogram, f64 value);

    // Closes the frame: merges what every thread recorded, stores this
    // frame's value of every stat in its history and resets counters and
    // timers. `frameSeconds` is the frame's length, used for per second rates.
    void endFrame(f64 frameSeconds);

    // Queries for the frame most recently closed by endFrame (main thread).
    // Counter: the frame's sum. Gauge: the current value. Timer: total
    // milliseconds. Histogram: samples taken this frame.
    [[nodiscard]] f64 value(StatId id) const noexcept;
    // Timer measurements or histogram samples in that frame.
    [[nodiscard]] u64 count(StatId id) const noexcept;
    // Per frame values, oldest first, at most historyFrames of them.
    void copyHistory(StatId id, std::vector<f64>& out) const;
    // Mean and maximum over the last `frames` frames (fewer if not recorded yet).
    [[nodiscard]] f64 average(StatId id, u32 frames) const noexcept;
    [[nodiscard]] f64 maximum(StatId id, u32 frames) const noexcept;
    // The value summed over the most recent frames covering about one second,
    // divided by their duration. For counters such as "chunks per second".
    [[nodiscard]] f64 perSecond(StatId id) const noexcept;
    [[nodiscard]] HistogramSnapshot histogram(StatId id) const;
    void resetHistogram(StatId id) noexcept;

    struct StatInfo {
        std::string name{};
        std::string description{};
        StatType type = StatType::Counter;
        StatUnit unit = StatUnit::Count;
    };
    [[nodiscard]] const StatInfo& info(StatId id) const;
    [[nodiscard]] u32 statCount() const noexcept { return m_count.load(std::memory_order_acquire); }
    [[nodiscard]] u64 framesClosed() const noexcept { return m_framesClosed; }

private:
    struct ThreadSlots;
    struct Stat;

    ThreadSlots& localSlots();
    // Records into this thread's slot and marks it dirty.
    void record(StatId id, i64 amount, i64 count);
    void mergeThreadSlots();
    void closeStat(u32 index);

    const u64 m_instance; // Unique per registry, so thread local caches never confuse two.
    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<Stat>> m_stats; // Reserved to kMaxStats; never reallocates.
    std::atomic<u32> m_count{0};
    std::vector<std::unique_ptr<ThreadSlots>> m_threads;

    // Main thread frame state, preallocated so endFrame never allocates.
    std::vector<i64> m_frameSums;
    std::vector<i64> m_frameCounts;
    std::vector<f64> m_frameSeconds; // Ring of frame durations, for perSecond.
    usize m_frameSecondsHead = 0;
    u64 m_framesClosed = 0;
    StatId m_mergeTime;
};

// Adds the time from construction to destruction to a timer stat.
class ScopedStatTimer {
public:
    ScopedStatTimer(StatRegistry& registry, StatId timer) noexcept
        : m_registry(registry)
        , m_timer(timer)
        , m_start(Clock::now()) {}
    ~ScopedStatTimer() {
        // A thread's first record allocates its slots. If that fails, losing
        // one measurement is the right outcome, not a crash in a destructor.
        try {
            m_registry.addTime(m_timer, Clock::now() - m_start);
        } catch (...) { // NOLINT(bugprone-empty-catch): deliberately dropped, see above.
        }
    }
    ScopedStatTimer(const ScopedStatTimer&) = delete;
    ScopedStatTimer& operator=(const ScopedStatTimer&) = delete;
    ScopedStatTimer(ScopedStatTimer&&) = delete;
    ScopedStatTimer& operator=(ScopedStatTimer&&) = delete;

private:
    StatRegistry& m_registry;
    StatId m_timer;
    TimePoint m_start;
};

} // namespace vxe::stats
