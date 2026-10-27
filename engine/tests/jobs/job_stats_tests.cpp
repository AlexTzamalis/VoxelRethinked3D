#include <vxe/jobs/job_system.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using vxe::jobs::CancelSource;
using vxe::jobs::CancelToken;
using vxe::jobs::JobCounter;
using vxe::jobs::JobSystem;
using vxe::jobs::JobSystemConfig;
using vxe::jobs::JobSystemStats;
using vxe::jobs::Pool;
using vxe::jobs::Priority;

namespace {

vxe::u64 totalJobsRun(const JobSystemStats& stats) {
    vxe::u64 total = stats.helpers.jobsRun;
    for (const vxe::jobs::ThreadStats& worker : stats.workers) {
        total += worker.jobsRun;
    }
    for (const vxe::jobs::ThreadStats& io : stats.ioThreads) {
        total += io.jobsRun;
    }
    return total;
}

} // namespace

TEST_CASE("Stats count every job on the thread that ran it") {
    JobSystem jobs(JobSystemConfig{.workerCount = 3, .ioThreadCount = 1});
    JobCounter counter;
    for (int i = 0; i < 300; ++i) {
        jobs.schedule(counter, [] {});
    }
    for (int i = 0; i < 20; ++i) {
        jobs.schedule(counter, [] {}, {Priority::Normal, CancelToken{}, Pool::Io});
    }
    jobs.wait(counter);

    const JobSystemStats stats = jobs.stats();
    CHECK(stats.workers.size() == 3);
    CHECK(stats.ioThreads.size() == 1);
    CHECK(stats.jobsCompleted == 320);
    CHECK(totalJobsRun(stats) == 320);
    CHECK(stats.ioThreads[0].jobsRun == 20);
}

TEST_CASE("Busy time reflects time spent inside jobs") {
    JobSystem jobs(JobSystemConfig{.workerCount = 1, .ioThreadCount = 0});
    JobCounter counter;
    jobs.schedule(counter, [] { std::this_thread::sleep_for(5ms); });
    // Wait without helping so the worker, not this thread, runs the job.
    while (!counter.isDone()) {
        std::this_thread::yield();
    }
    const JobSystemStats stats = jobs.stats();
    CHECK(stats.workers[0].jobsRun == 1);
    CHECK(stats.workers[0].busy >= 5ms);
}

TEST_CASE("Queue depth is reported per priority") {
    JobSystem jobs(JobSystemConfig{.workerCount = 1, .ioThreadCount = 0});
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
    for (int i = 0; i < 2; ++i) {
        jobs.schedule(counter, [] {}, Priority::High);
    }
    for (int i = 0; i < 3; ++i) {
        jobs.schedule(counter, [] {}, Priority::Background);
    }

    const JobSystemStats blocked = jobs.stats();
    CHECK(blocked.queued[static_cast<std::size_t>(Priority::High)] == 2);
    CHECK(blocked.queued[static_cast<std::size_t>(Priority::Background)] == 3);
    CHECK(blocked.queued[static_cast<std::size_t>(Priority::Normal)] == 0);

    gateOpen.store(true);
    jobs.wait(counter);
    const JobSystemStats drained = jobs.stats();
    for (const vxe::u64 depth : drained.queued) {
        CHECK(depth == 0);
    }
}

TEST_CASE("Cancelled and dependency waiting jobs are counted") {
    JobSystem jobs(JobSystemConfig{.workerCount = 2, .ioThreadCount = 0});
    CancelSource source;
    source.cancel();
    JobCounter counter;
    for (int i = 0; i < 5; ++i) {
        jobs.schedule(counter, [] {}, {Priority::Normal, source.token()});
    }
    jobs.wait(counter);

    std::atomic<bool> release{false};
    const vxe::jobs::JobHandle blocker = jobs.schedule([&release] {
        while (!release.load()) {
            std::this_thread::yield();
        }
    });
    const vxe::jobs::JobHandle dependent = jobs.scheduleAfter(std::span(&blocker, 1), [] {});
    const JobSystemStats waiting = jobs.stats();
    CHECK(waiting.jobsCancelled == 5);
    CHECK(waiting.waitingForDependencies == 1);

    release.store(true);
    jobs.wait(dependent);
    CHECK(jobs.stats().waitingForDependencies == 0);
}

TEST_CASE("Jobs per second is measured between calls") {
    JobSystem jobs(JobSystemConfig{.workerCount = 2, .ioThreadCount = 0});
    static_cast<void>(jobs.stats()); // Starts the measurement window.
    JobCounter counter;
    for (int i = 0; i < 1000; ++i) {
        jobs.schedule(counter, [] {});
    }
    jobs.wait(counter);
    std::this_thread::sleep_for(1ms);
    const JobSystemStats stats = jobs.stats();
    CHECK(stats.jobsPerSecond > 0.0);
    // The next window saw no jobs at all.
    std::this_thread::sleep_for(1ms);
    CHECK(jobs.stats().jobsPerSecond == 0.0);
}
