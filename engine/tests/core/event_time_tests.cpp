#include <vxe/core/event_bus.hpp>
#include <vxe/core/time.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct Resized {
    int width = 0;
    int height = 0;
};

struct Named {
    std::string name;
};

} // namespace

TEST_CASE("Events are delivered on dispatch, in publish order") {
    vxe::EventBus bus;
    std::vector<int> widths;
    const vxe::Subscription subscription =
        bus.subscribe<Resized>([&widths](const Resized& e) { widths.push_back(e.width); });

    bus.publish(Resized{.width = 1, .height = 1});
    bus.publish(Resized{.width = 2, .height = 2});
    CHECK(widths.empty()); // Deferred until dispatch.
    CHECK(bus.pendingCount() == 2);

    bus.dispatch();
    CHECK(widths == std::vector<int>{1, 2});
    CHECK(bus.pendingCount() == 0);
}

TEST_CASE("Handlers only receive their own event type") {
    vxe::EventBus bus;
    int resizes = 0;
    std::string lastName;
    const vxe::Subscription a = bus.subscribe<Resized>([&resizes](const Resized&) { ++resizes; });
    const vxe::Subscription b = bus.subscribe<Named>([&lastName](const Named& e) { lastName = e.name; });

    bus.publish(Named{"stone"});
    bus.publish(Resized{});
    bus.dispatch();
    CHECK(resizes == 1);
    CHECK(lastName == "stone");
}

TEST_CASE("Destroying a subscription unsubscribes") {
    vxe::EventBus bus;
    int calls = 0;
    {
        const vxe::Subscription scoped = bus.subscribe<Resized>([&calls](const Resized&) { ++calls; });
        bus.publish(Resized{});
        bus.dispatch();
    }
    bus.publish(Resized{});
    bus.dispatch();
    CHECK(calls == 1);

    vxe::Subscription manual = bus.subscribe<Resized>([&calls](const Resized&) { ++calls; });
    CHECK(manual.isActive());
    manual.reset();
    CHECK_FALSE(manual.isActive());
    manual.reset(); // Twice is fine.
}

TEST_CASE("A moved subscription keeps the handler alive") {
    vxe::EventBus bus;
    int calls = 0;
    vxe::Subscription moved;
    {
        vxe::Subscription original = bus.subscribe<Resized>([&calls](const Resized&) { ++calls; });
        moved = std::move(original);
    }
    bus.publish(Resized{});
    bus.dispatch();
    CHECK(calls == 1);
}

TEST_CASE("Handlers may unsubscribe themselves and others while dispatching") {
    vxe::EventBus bus;
    int firstCalls = 0;
    int secondCalls = 0;
    std::optional<vxe::Subscription> first;
    std::optional<vxe::Subscription> second;
    first = bus.subscribe<Resized>([&](const Resized&) {
        ++firstCalls;
        second.reset(); // Removes the next handler before it runs.
        first.reset();  // Removes itself.
    });
    second = bus.subscribe<Resized>([&secondCalls](const Resized&) { ++secondCalls; });

    bus.publish(Resized{});
    bus.publish(Resized{});
    bus.dispatch();
    CHECK(firstCalls == 1);
    CHECK(secondCalls == 0);
}

TEST_CASE("Handlers added or events published while dispatching wait for the next dispatch") {
    vxe::EventBus bus;
    int lateCalls = 0;
    int echoes = 0;
    std::optional<vxe::Subscription> late;
    const vxe::Subscription spawner = bus.subscribe<Resized>([&](const Resized& e) {
        if (!late) {
            late = bus.subscribe<Resized>([&lateCalls](const Resized&) { ++lateCalls; });
        }
        if (e.width == 0) {
            ++echoes;
            bus.publish(
                Resized{.width = 1, .height = 1}); // Would loop forever if delivered in this dispatch.
        }
    });

    bus.publish(Resized{});
    bus.dispatch();
    CHECK(lateCalls == 0);
    CHECK(echoes == 1);
    CHECK(bus.pendingCount() == 1);

    bus.dispatch();
    CHECK(lateCalls == 1);
    late.reset();
}

TEST_CASE("Events published from other threads all arrive") {
    vxe::EventBus bus;
    int received = 0;
    const vxe::Subscription subscription =
        bus.subscribe<Resized>([&received](const Resized&) { ++received; });

    constexpr int kThreads = 4;
    constexpr int kEventsPerThread = 500;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&bus] {
            for (int i = 0; i < kEventsPerThread; ++i) {
                bus.publish(Resized{.width = i, .height = i});
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    bus.dispatch();
    CHECK(received == kThreads * kEventsPerThread);
}

TEST_CASE("FrameTimer measures, clamps and accumulates") {
    vxe::FrameTimer timer(100ms);
    const vxe::TimePoint start{};

    const vxe::FrameTime first = timer.tick(start);
    CHECK(first.frameIndex == 0);
    CHECK(first.deltaSeconds == 0.0);

    const vxe::FrameTime second = timer.tick(start + 16ms);
    CHECK(second.frameIndex == 1);
    CHECK(second.delta == 16ms);
    CHECK(second.deltaSeconds == doctest::Approx(0.016));

    // A two second stall (breakpoint) is clamped but still reported raw.
    const vxe::FrameTime stalled = timer.tick(start + 16ms + 2s);
    CHECK(stalled.delta == 100ms);
    CHECK(stalled.rawDelta == 2s);
    CHECK(stalled.elapsedSeconds == doctest::Approx(0.116));

    // Time never runs backwards, even if the caller passes an older point.
    const vxe::FrameTime backwards = timer.tick(start);
    CHECK(backwards.delta == vxe::Duration::zero());
}

TEST_CASE("Stopwatch and ScopedTimer measure elapsed time") {
    const vxe::Stopwatch stopwatch;
    vxe::Duration measured{};
    {
        const vxe::ScopedTimer timer(measured);
        std::this_thread::sleep_for(2ms);
    }
    CHECK(measured >= 2ms);
    CHECK(stopwatch.elapsed() >= measured);
    CHECK(vxe::toMilliseconds(1s) == doctest::Approx(1000.0));
    CHECK(vxe::fromSeconds(0.5) == 500ms);
}
