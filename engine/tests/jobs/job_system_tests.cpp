#include <vxe/jobs/job_system.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using vxe::jobs::JobCounter;
using vxe::jobs::JobHandle;
using vxe::jobs::JobSystem;
using vxe::jobs::JobSystemConfig;
using vxe::jobs::Priority;

TEST_CASE("Every scheduled job runs exactly once") {
    JobSystem jobs(JobSystemConfig{.workerCount = 4});
    CHECK(jobs.workerCount() == 4);

    std::vector<std::atomic<int>> runs(1000);
    JobCounter counter;
    for (std::atomic<int>& run : runs) {
        jobs.schedule(counter, [&run] { run.fetch_add(1, std::memory_order_relaxed); });
    }
    jobs.wait(counter);
    CHECK(counter.isDone());
    for (const std::atomic<int>& run : runs) {
        REQUIRE(run.load() == 1);
    }
}

TEST_CASE("Stress: one million tiny jobs") {
    JobSystem jobs;
    std::atomic<vxe::u64> sum{0};
    JobCounter counter;
    constexpr vxe::u64 kJobs = 1'000'000;
    for (vxe::u64 i = 1; i <= kJobs; ++i) {
        jobs.schedule(counter, [&sum, i] { sum.fetch_add(i, std::memory_order_relaxed); });
    }
    jobs.wait(counter);
    CHECK(sum.load() == kJobs * (kJobs + 1) / 2);
    CHECK(jobs.queuedJobCount() == 0);
}

TEST_CASE("Jobs can schedule and wait for jobs without deadlocking") {
    // More nested waits than workers: only possible because waiting threads
    // run other jobs instead of blocking.
    JobSystem jobs(JobSystemConfig{.workerCount = 2});
    std::atomic<int> leaves{0};

    struct Context {
        JobSystem* jobs;
        std::atomic<int>* leaves;
    };
    const Context context{.jobs = &jobs, .leaves = &leaves};

    JobCounter outer;
    for (int i = 0; i < 16; ++i) {
        jobs.schedule(outer, [&context] {
            JobCounter inner;
            for (int j = 0; j < 16; ++j) {
                context.jobs->schedule(inner, [&context] { context.leaves->fetch_add(1); });
            }
            context.jobs->wait(inner);
        });
    }
    jobs.wait(outer);
    CHECK(leaves.load() == 16 * 16);
}

TEST_CASE("Higher priorities run first") {
    // One worker, blocked by a gate job while the other jobs are queued, so
    // the order in which it picks them up is purely the priority order.
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

    std::mutex orderMutex;
    std::vector<Priority> order;
    for (const Priority priority :
         {Priority::Background, Priority::Low, Priority::Normal, Priority::High, Priority::Critical}) {
        jobs.schedule(
            counter,
            [&orderMutex, &order, priority] {
                const std::scoped_lock lock(orderMutex);
                order.push_back(priority);
            },
            priority);
    }
    gateOpen.store(true);
    // Wait without helping, so the single worker does all the picking.
    while (!counter.isDone()) {
        std::this_thread::yield();
    }
    CHECK(order == std::vector<Priority>{Priority::Critical, Priority::High, Priority::Normal, Priority::Low,
                                         Priority::Background});
}

TEST_CASE("Job handles report completion and outlive dropped copies") {
    // Declared before the job system: the jobs below are never waited for, so
    // they may still run while the job system drains in its destructor.
    std::atomic<int> value{0};
    JobSystem jobs(JobSystemConfig{.workerCount = 2});
    const JobHandle handle = jobs.schedule([&value] { value.store(42); });
    jobs.wait(handle);
    CHECK(handle.isDone());
    CHECK(value.load() == 42);

    // A handle dropped right away must not free the counter under the job.
    for (int i = 0; i < 100; ++i) {
        static_cast<void>(jobs.schedule([&value] { value.fetch_add(1); }));
    }
    CHECK(JobHandle().isDone());
}

TEST_CASE("Destroying the job system runs every queued job first") {
    std::atomic<int> ran{0};
    {
        // The counter is declared first so it outlives the job system, whose
        // destructor runs the jobs that still decrement it.
        JobCounter counter;
        JobSystem jobs(JobSystemConfig{.workerCount = 2});
        for (int i = 0; i < 500; ++i) {
            jobs.schedule(counter, [&ran, &jobs, &counter] {
                ran.fetch_add(1);
                // Jobs scheduled while shutting down still run.
                if (ran.load() == 1) {
                    jobs.schedule(counter, [&ran] { ran.fetch_add(1); });
                }
            });
        }
    }
    CHECK(ran.load() == 501);
}

TEST_CASE("Jobs run on worker threads that know their index") {
    JobSystem jobs(JobSystemConfig{.workerCount = 3});
    CHECK_FALSE(jobs.currentWorkerIndex().has_value());

    std::mutex mutex;
    std::set<vxe::u32> indices;
    JobCounter counter;
    for (int i = 0; i < 300; ++i) {
        jobs.schedule(counter, [&] {
            const auto index = jobs.currentWorkerIndex();
            const std::scoped_lock lock(mutex);
            indices.insert(index.value_or(999));
        });
    }
    jobs.wait(counter);
    for (const vxe::u32 index : indices) {
        CHECK((index < 3 || index == 999)); // 999: run by the waiting main thread.
    }
}

TEST_CASE("Jobs may own move only captures") {
    JobSystem jobs(JobSystemConfig{.workerCount = 1});
    auto owned = std::make_unique<int>(7);
    std::atomic<int> seen{0};
    const JobHandle handle = jobs.schedule([value = std::move(owned), &seen] { seen.store(*value); });
    jobs.wait(handle);
    CHECK(seen.load() == 7);
}
