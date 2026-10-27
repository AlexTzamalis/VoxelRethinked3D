#include <vxe/jobs/job_system.hpp>

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using vxe::jobs::CancelSource;
using vxe::jobs::CancelToken;
using vxe::jobs::JobCounter;
using vxe::jobs::JobHandle;
using vxe::jobs::JobOptions;
using vxe::jobs::JobSystem;
using vxe::jobs::JobSystemConfig;
using vxe::jobs::Pool;
using vxe::jobs::Priority;

namespace {

// Records the order in which jobs ran, from any thread.
struct OrderLog {
    std::mutex mutex;
    std::vector<int> order;
    void add(int value) {
        const std::scoped_lock lock(mutex);
        order.push_back(value);
    }
};

} // namespace

TEST_CASE("A chain of dependent jobs runs in order") {
    OrderLog log;
    JobSystem jobs(JobSystemConfig{.workerCount = 4});
    const JobHandle first = jobs.schedule([&log] {
        std::this_thread::sleep_for(2ms); // Gives the later jobs a chance to run too early, if they could.
        log.add(1);
    });
    const JobHandle second = jobs.scheduleAfter(std::span(&first, 1), [&log] { log.add(2); });
    const JobHandle third = jobs.scheduleAfter(std::span(&second, 1), [&log] { log.add(3); });
    jobs.wait(third);
    CHECK(first.isDone());
    CHECK(second.isDone());
    CHECK(log.order == std::vector<int>{1, 2, 3});
}

TEST_CASE("A job waits for all of its dependencies") {
    // Diamond: top runs after both left and right, which both follow root.
    OrderLog log;
    JobSystem jobs(JobSystemConfig{.workerCount = 4});
    const JobHandle root = jobs.schedule([&log] { log.add(0); });
    const std::array<JobHandle, 2> sides{
        jobs.scheduleAfter(std::span(&root, 1),
                           [&log] {
                               std::this_thread::sleep_for(1ms);
                               log.add(1);
                           }),
        jobs.scheduleAfter(std::span(&root, 1), [&log] { log.add(1); }),
    };
    const JobHandle top = jobs.scheduleAfter(sides, [&log] { log.add(2); });
    jobs.wait(top);
    REQUIRE(log.order.size() == 4);
    CHECK(log.order.front() == 0);
    CHECK(log.order.back() == 2);
}

TEST_CASE("Finished, empty and missing dependencies do not block") {
    JobSystem jobs(JobSystemConfig{.workerCount = 2});
    const JobHandle done = jobs.schedule([] {});
    jobs.wait(done);

    std::atomic<int> ran{0};
    const std::array<JobHandle, 2> dependencies{done, JobHandle{}};
    jobs.wait(jobs.scheduleAfter(dependencies, [&ran] { ran.fetch_add(1); }));
    jobs.wait(jobs.scheduleAfter({}, [&ran] { ran.fetch_add(1); }));
    CHECK(ran.load() == 2);
}

TEST_CASE("Many jobs can depend on one, and one on many") {
    std::atomic<int> fanOut{0};
    std::atomic<int> fanIn{0};
    JobSystem jobs(JobSystemConfig{.workerCount = 4});
    const JobHandle source = jobs.schedule([] { std::this_thread::sleep_for(1ms); });

    std::vector<JobHandle> dependents;
    dependents.reserve(200);
    for (int i = 0; i < 200; ++i) {
        dependents.push_back(jobs.scheduleAfter(std::span(&source, 1), [&fanOut] { fanOut.fetch_add(1); }));
    }
    const JobHandle sink = jobs.scheduleAfter(dependents, [&fanOut, &fanIn] { fanIn.store(fanOut.load()); });
    jobs.wait(sink);
    CHECK(fanIn.load() == 200);
}

TEST_CASE("A job cancelled before it starts is skipped but still completes") {
    JobSystem jobs(JobSystemConfig{.workerCount = 1});
    std::atomic<bool> gateOpen{false};
    std::atomic<bool> gateRunning{false};
    JobCounter counter;
    jobs.schedule(counter, [&] {
        gateRunning.store(true);
        while (!gateOpen.load()) {
            std::this_thread::yield();
        }
    });
    while (!gateRunning.load()) {
        std::this_thread::yield();
    }

    CancelSource source;
    std::atomic<int> ran{0};
    jobs.schedule(counter, [&ran] { ran.fetch_add(1); }, JobOptions(Priority::Normal, source.token()));
    const JobHandle dependent =
        jobs.scheduleAfter({}, [&ran] { ran.fetch_add(10); }, {Priority::Low, source.token()});
    source.cancel();
    gateOpen.store(true);

    jobs.wait(counter);
    jobs.wait(dependent);
    CHECK(ran.load() == 0);
    CHECK(counter.isDone());
    CHECK(dependent.isDone());
}

TEST_CASE("A running job can notice cancellation and stop early") {
    JobSystem jobs(JobSystemConfig{.workerCount = 1});
    CancelSource source;
    std::atomic<bool> started{false};
    std::atomic<bool> stoppedEarly{false};
    const JobHandle handle = jobs.schedule([token = source.token(), &started, &stoppedEarly] {
        started.store(true);
        for (int i = 0; i < 10'000; ++i) {
            if (token.isCancelled()) {
                stoppedEarly.store(true);
                return;
            }
            std::this_thread::sleep_for(100us);
        }
    });
    while (!started.load()) {
        std::this_thread::yield();
    }
    source.cancel();
    jobs.wait(handle);
    CHECK(stoppedEarly.load());

    CHECK_FALSE(CancelToken{}.isCancelled());
    CHECK_FALSE(CancelToken{}.canBeCancelled());
}

TEST_CASE("Tasks posted to the main thread run there, in order") {
    JobSystem jobs(JobSystemConfig{.workerCount = 4});
    const std::thread::id mainThread = std::this_thread::get_id();

    std::vector<int> order; // Only touched on the main thread.
    std::atomic<int> wrongThread{0};
    JobCounter counter;
    for (int i = 0; i < 50; ++i) {
        jobs.schedule(counter, [&jobs, &order, &wrongThread, mainThread, i] {
            jobs.runOnMainThread([&order, &wrongThread, mainThread, i] {
                if (std::this_thread::get_id() != mainThread) {
                    wrongThread.fetch_add(1);
                }
                order.push_back(i);
            });
        });
    }
    jobs.wait(counter);
    CHECK(order.empty()); // Nothing runs until the main thread asks.

    CHECK(jobs.runMainThreadTasks() == 50);
    CHECK(order.size() == 50);
    CHECK(wrongThread.load() == 0);

    // A task posted by a task waits for the next call.
    jobs.runOnMainThread([&jobs, &order] { jobs.runOnMainThread([&order] { order.push_back(-1); }); });
    CHECK(jobs.runMainThreadTasks() == 1);
    CHECK(jobs.runMainThreadTasks() == 1);
    CHECK(order.back() == -1);
    CHECK(jobs.runMainThreadTasks() == 0);
}

TEST_CASE("IO jobs run on IO threads and never block compute workers") {
    JobSystem jobs(JobSystemConfig{.workerCount = 1, .ioThreadCount = 1});
    CHECK(jobs.ioThreadCount() == 1);

    // The IO job blocks until a compute job releases it. If IO jobs ran on the
    // single compute worker, the compute job could never run: a deadlock.
    std::atomic<bool> released{false};
    std::atomic<bool> ranOnIoThread{false};
    std::atomic<bool> ioSawWorkerIndex{false};
    const JobHandle io = jobs.schedule(
        [&] {
            ranOnIoThread.store(jobs.isIoThread());
            ioSawWorkerIndex.store(jobs.currentWorkerIndex().has_value());
            while (!released.load()) {
                std::this_thread::sleep_for(100us);
            }
        },
        {Priority::Normal, CancelToken{}, Pool::Io});
    const JobHandle compute = jobs.schedule([&released] { released.store(true); });
    jobs.wait(compute);
    jobs.wait(io);
    CHECK(ranOnIoThread.load());
    CHECK_FALSE(ioSawWorkerIndex.load());
    CHECK_FALSE(jobs.isIoThread());
}

TEST_CASE("IO jobs fall back to compute workers when there are no IO threads") {
    JobSystem jobs(JobSystemConfig{.workerCount = 1, .ioThreadCount = 0});
    std::atomic<bool> ran{false};
    jobs.wait(jobs.schedule([&ran] { ran.store(true); }, {Priority::Normal, CancelToken{}, Pool::Io}));
    CHECK(ran.load());
}

TEST_CASE("Shutdown finishes continuations and IO jobs") {
    std::atomic<int> ran{0};
    {
        JobSystem jobs(JobSystemConfig{.workerCount = 2, .ioThreadCount = 1});
        const JobHandle slow = jobs.schedule([&ran] {
            std::this_thread::sleep_for(2ms);
            ran.fetch_add(1);
        });
        static_cast<void>(jobs.scheduleAfter(std::span(&slow, 1), [&ran] { ran.fetch_add(1); }));
        static_cast<void>(
            jobs.schedule([&ran] { ran.fetch_add(1); }, {Priority::Low, CancelToken{}, Pool::Io}));
    }
    CHECK(ran.load() == 3);
}
