#include <vxe/core/stats.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <thread>
#include <vector>

using namespace vxe;
using namespace vxe::stats;

TEST_CASE("Stats register once per name and type") {
    StatRegistry registry;
    const auto first =
        registry.registerStat({.name = "Test.Draws", .description = "Draw calls", .type = StatType::Counter});
    REQUIRE(first.hasValue());
    const auto again = registry.registerStat({.name = "Test.Draws", .type = StatType::Counter});
    REQUIRE(again.hasValue());
    CHECK(again.value() == first.value()); // Registering twice is harmless.
    CHECK(registry.registerStat({.name = "Test.Draws", .type = StatType::Gauge}).hasError());
    CHECK(registry.registerStat({.name = "", .type = StatType::Counter}).hasError());
    CHECK(registry.find("Test.Draws") == first.value());
    CHECK_FALSE(registry.find("Test.Missing").isValid());
    CHECK(registry.info(first.value()).description == "Draw calls");
    // The registry measures itself.
    CHECK(registry.find("Debug.Stats.MergeTime").isValid());
    CHECK(registry.info(registry.find("Debug.Stats.MergeTime")).unit == StatUnit::Milliseconds);
}

TEST_CASE("Counters sum across threads per frame and reset") {
    StatRegistry registry;
    const StatId counter = registry.registerStat({.name = "Test.Jobs", .type = StatType::Counter}).value();
    constexpr int kThreads = 8;
    constexpr int kAdds = 100'000;
    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&registry, counter] {
                for (int i = 0; i < kAdds; ++i) {
                    registry.add(counter);
                }
            });
        }
    }
    registry.add(counter, 5);
    registry.endFrame(1.0 / 60.0);
    CHECK(registry.value(counter) == static_cast<f64>(kThreads * kAdds + 5));
    CHECK(registry.count(counter) == static_cast<u64>(kThreads * kAdds + 1));
    registry.endFrame(1.0 / 60.0);
    CHECK(registry.value(counter) == 0.0); // Counters start over every frame.
}

TEST_CASE("No count is lost while frames close concurrently") {
    StatRegistry registry;
    const StatId counter = registry.registerStat({.name = "Test.Stress", .type = StatType::Counter}).value();
    constexpr int kThreads = 4;
    constexpr int kAdds = 200'000;
    std::atomic<int> running{kThreads};
    f64 total = 0.0;
    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&registry, &running, counter] {
                for (int i = 0; i < kAdds; ++i) {
                    registry.add(counter);
                }
                running.fetch_sub(1);
            });
        }
        while (running.load() > 0) {
            registry.endFrame(0.001);
            total += registry.value(counter);
        }
    }
    registry.endFrame(0.001);
    total += registry.value(counter);
    CHECK(total == static_cast<f64>(kThreads * kAdds));
}

TEST_CASE("Gauges keep their value, timers report milliseconds") {
    StatRegistry registry;
    const StatId gauge = registry.registerStat({.name = "Test.Resident", .type = StatType::Gauge}).value();
    const StatId timer = registry.registerStat({.name = "Test.MeshTime", .type = StatType::Timer}).value();
    registry.set(gauge, 1234.0);
    registry.addTime(timer, std::chrono::microseconds(1500));
    registry.addTime(timer, std::chrono::microseconds(500));
    registry.endFrame(0.016);
    CHECK(registry.value(gauge) == 1234.0);
    CHECK(registry.value(timer) == doctest::Approx(2.0));
    CHECK(registry.count(timer) == 2);
    registry.endFrame(0.016);
    CHECK(registry.value(gauge) == 1234.0); // Still set.
    CHECK(registry.value(timer) == 0.0);
    {
        const ScopedStatTimer scope(registry, timer);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    registry.endFrame(0.016);
    CHECK(registry.value(timer) >= 2.0);
    CHECK(registry.count(timer) == 1);
}

TEST_CASE("Histograms give percentiles within one bucket") {
    StatRegistry registry;
    const StatId latency =
        registry.registerStat({.name = "Test.Latency", .type = StatType::Histogram, .histogramMin = 0.1})
            .value();
    for (int value = 1; value <= 100; ++value) {
        registry.sample(latency, static_cast<f64>(value));
    }
    registry.endFrame(0.016);
    CHECK(registry.value(latency) == 100.0); // Samples this frame.
    const HistogramSnapshot snapshot = registry.histogram(latency);
    CHECK(snapshot.samples == 100);
    // A percentile is a bucket's upper edge: at least the true value, at most
    // a quarter octave (about 19 percent) above it.
    const f64 median = snapshot.percentile(0.5);
    CHECK(median >= 50.0);
    CHECK(median <= 50.0 * 1.19);
    CHECK(snapshot.percentile(0.99) >= 99.0);
    CHECK(snapshot.percentile(0.0) > 0.0);
    registry.sample(latency, 0.01);  // Below the first edge: bucket 0.
    registry.sample(latency, 1.0e9); // Far above the range: the last bucket.
    CHECK(registry.histogram(latency).samples == 102);
    registry.resetHistogram(latency);
    CHECK(registry.histogram(latency).samples == 0);
}

TEST_CASE("History keeps the newest frames in order") {
    StatRegistry registry;
    const StatId gauge =
        registry.registerStat({.name = "Test.History", .type = StatType::Gauge, .historyFrames = 4}).value();
    for (int frame = 1; frame <= 6; ++frame) {
        registry.set(gauge, static_cast<f64>(frame));
        registry.endFrame(0.016);
    }
    std::vector<f64> history;
    registry.copyHistory(gauge, history);
    CHECK(history == std::vector<f64>{3.0, 4.0, 5.0, 6.0});
    CHECK(registry.average(gauge, 2) == doctest::Approx(5.5));
    CHECK(registry.maximum(gauge, 100) == 6.0);
    CHECK(registry.framesClosed() == 6);
}

TEST_CASE("Rates per second use the frame durations") {
    StatRegistry registry;
    const StatId counter = registry.registerStat({.name = "Test.Chunks", .type = StatType::Counter}).value();
    for (int frame = 0; frame < 20; ++frame) {
        registry.add(counter, 10);
        registry.endFrame(0.1);
    }
    // The last 10 frames make one second with 100 chunks.
    CHECK(registry.perSecond(counter) == doctest::Approx(100.0));
}

TEST_CASE("Closing a frame with 500 stats stays within budget") {
    StatRegistry registry;
    std::vector<StatId> ids;
    ids.reserve(500);
    for (int i = 0; i < 500; ++i) {
        // A quarter timers, a quarter gauges, half counters.
        StatType type = StatType::Counter;
        if (i % 4 == 0) {
            type = StatType::Timer;
        } else if (i % 4 == 1) {
            type = StatType::Gauge;
        }
        ids.push_back(registry.registerStat({.name = std::format("Bench.Stat{}", i), .type = type}).value());
    }
    // Four workers keep recording their own 50 stats, like job threads.
    std::atomic<bool> stop{false};
    std::vector<std::jthread> workers;
    workers.reserve(4);
    for (int w = 0; w < 4; ++w) {
        workers.emplace_back([&registry, &ids, &stop, w] {
            while (!stop.load(std::memory_order_relaxed)) {
                for (int i = 0; i < 50; ++i) {
                    const StatId id = ids[static_cast<usize>(w * 50 + i) * 2 % 500];
                    if (registry.info(id).type == StatType::Counter) {
                        registry.add(id);
                    }
                }
                std::this_thread::yield();
            }
        });
    }
    constexpr int kFrames = 1000;
    Duration closing{};
    for (int frame = 0; frame < kFrames; ++frame) {
        // The main thread touches every stat once per frame.
        for (const StatId id : ids) {
            switch (registry.info(id).type) {
            case StatType::Counter:
                registry.add(id);
                break;
            case StatType::Gauge:
                registry.set(id, static_cast<f64>(frame));
                break;
            case StatType::Timer:
                registry.addTime(id, std::chrono::microseconds(1));
                break;
            case StatType::Histogram:
                break;
            }
        }
        const TimePoint start = Clock::now();
        registry.endFrame(0.001);
        closing += Clock::now() - start;
    }
    stop = true;
    workers.clear();
    const f64 averageMs = toMilliseconds(closing) / kFrames;
    MESSAGE("endFrame with 500 stats and 5 recording threads: " << averageMs << " ms");
#if defined(NDEBUG) && !defined(VXE_SANITIZER_ACTIVE)
    CHECK(averageMs < 0.05); // The E7.1 budget.
#endif
    CHECK(registry.average(registry.find("Debug.Stats.MergeTime"), 100) > 0.0);
}
