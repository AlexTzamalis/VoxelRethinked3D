#pragma once

#include <vxe/core/inline_function.hpp>
#include <vxe/core/time.hpp>
#include <vxe/core/types.hpp>
#include <vxe/jobs/cancel.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vxe::jobs {

// Job priorities, highest first. A worker always runs the highest priority
// job available anywhere before a lower one (Section 11.3: chunks near the
// player first, background work like LOD tiles last).
enum class Priority : u8 {
    Critical,   // Needed this frame (for example the mesh of an edited chunk).
    High,       // Needed very soon (chunks right around the player).
    Normal,     // The default.
    Low,        // Nice to have soon (distant chunks).
    Background, // Whenever nothing else is waiting (LOD premapping, compaction).
};

inline constexpr usize kPriorityCount = 5;

// Which threads run a job.
enum class Pool : u8 {
    Compute, // The worker threads. For CPU work that never blocks.
    Io,      // The IO threads. For work that blocks on the disk (Section 6.4).
};

// Bytes a job's callable may capture. 48 bytes hold six pointers or a few
// small values; larger data is passed by pointer. The limit keeps every job
// in a fixed size slot, so scheduling never allocates.
inline constexpr usize kJobCaptureBytes = 48;

using JobFunction = InlineFunction<kJobCaptureBytes>;

// How to run a job. Converts from a Priority, so the common case stays short:
//   jobs.schedule(counter, fn, Priority::High);
//   jobs.schedule(counter, fn, {.priority = Priority::Low, .cancel = token});
struct JobOptions {
    Priority priority = Priority::Normal;
    CancelToken cancel; // A job cancelled before it starts is skipped.
    Pool pool = Pool::Compute;

    JobOptions() = default;
    // NOLINTNEXTLINE(google-explicit-constructor, hicpp-explicit-conversions): allows passing a bare
    // Priority.
    JobOptions(Priority p) noexcept
        : priority(p) {}
    JobOptions(Priority p, CancelToken token, Pool where = Pool::Compute) noexcept
        : priority(p)
        , cancel(std::move(token))
        , pool(where) {}
};

// Counts unfinished jobs. Scheduling a job with a counter increments it, and
// finishing the job decrements it, so a counter shared by many jobs tells when
// the whole batch is done. It lives wherever the caller wants (usually on the
// stack), so batches need no allocation at all.
//
// The counter must outlive every job that uses it: wait on it, or declare it
// before the JobSystem so the system's shutdown drain finishes first.
class JobCounter {
public:
    JobCounter() noexcept = default;
    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;
    JobCounter(JobCounter&&) = delete;
    JobCounter& operator=(JobCounter&&) = delete;
    ~JobCounter() = default;

    [[nodiscard]] bool isDone() const noexcept { return m_pending.load(std::memory_order_acquire) == 0; }
    [[nodiscard]] u32 pending() const noexcept { return m_pending.load(std::memory_order_acquire); }

private:
    friend class JobSystem;
    std::atomic<u32> m_pending{0};
};

namespace detail {
struct JobState;
} // namespace detail

// A handle to one scheduled job. Copyable; all copies refer to the same job.
// Unlike a JobCounter, a handle keeps its state alive on the heap, which is
// what lets other jobs depend on it (JobSystem::scheduleAfter). Creating one
// costs a single small allocation, so prefer a JobCounter for large batches.
class JobHandle {
public:
    JobHandle() noexcept = default;

    // True once the job has finished. An empty handle counts as done.
    [[nodiscard]] bool isDone() const noexcept;
    [[nodiscard]] bool isValid() const noexcept { return m_state != nullptr; }

private:
    friend class JobSystem;
    explicit JobHandle(std::shared_ptr<detail::JobState> state) noexcept
        : m_state(std::move(state)) {}
    std::shared_ptr<detail::JobState> m_state;
};

// Work done by one thread (or one group of threads) since startup.
struct ThreadStats {
    u64 jobsRun = 0; // Jobs finished, including cancelled ones that were skipped.
    Duration busy{}; // Time spent inside job functions.
};

// A snapshot of the job system for the debug overlay (Section 18.2, page
// "Streaming and Jobs") and tests. Values are read with relaxed atomics, so a
// snapshot taken while jobs run is consistent per field, not across fields.
struct JobSystemStats {
    std::vector<ThreadStats> workers;           // One per compute worker, by index.
    std::vector<ThreadStats> ioThreads;         // One per IO thread.
    ThreadStats helpers;                        // Jobs run by waiting threads (wait(), shutdown).
    std::array<u64, kPriorityCount> queued{};   // Compute jobs waiting, per priority.
    std::array<u64, kPriorityCount> queuedIo{}; // IO jobs waiting, per priority.
    u64 waitingForDependencies = 0;
    u64 jobsCompleted = 0; // All pools, since startup.
    u64 jobsCancelled = 0; // Skipped because their token was cancelled before they started.
    // Completed jobs per second since the previous stats() call (0 on the first).
    f64 jobsPerSecond = 0.0;
};

struct JobSystemConfig {
    // Number of worker threads. 0 picks hardware_concurrency - 2 (leaving the
    // main and server threads a core each, Section 6.4), but at least 1.
    u32 workerCount = 0;
    // Number of IO threads (Section 6.4 suggests 1 to 2).
    u32 ioThreadCount = 2;
};

// The per thread counters are cache line aligned on purpose; MSVC warns
// (C4324) about the padding that adds, which is the point.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

// A work stealing thread pool with priorities, dependencies, cancellation, a
// main thread task queue and separate IO threads (ADR-012).
//
// Compute jobs: each worker owns one queue per priority. Jobs scheduled from
// a worker go to its own queue, where the worker takes the newest first (good
// for cache: the data a job just produced is still hot). Jobs scheduled from
// other threads go to a shared queue per priority. A worker looks for work
// from the highest priority down: its own queue, then the shared queue, then
// the oldest job of another worker (stealing).
//
// IO jobs run on their own threads from their own queues. They may block on
// the disk without ever holding up a compute worker, and compute workers and
// wait() never pick them up.
//
// Idle threads spin briefly, then sleep until work arrives, so an idle engine
// uses no CPU.
//
// The queues are short critical sections behind a mutex rather than a lock
// free deque. That is simple to prove correct and fast enough for chunk sized
// jobs; a lock free version only replaces the queue type if profiling ever
// shows contention (Section 0.2: measure before optimizing).
//
// Thread safety: everything may be called from any thread, including from
// inside jobs, except runMainThreadTasks(), which belongs to the thread that
// created the JobSystem.
class JobSystem {
public:
    explicit JobSystem(const JobSystemConfig& config = {});
    // Runs every job that is still queued (including continuations and IO
    // jobs), then stops and joins all threads. Main thread tasks that were
    // never run are dropped.
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    // Schedules `function` and counts it in `counter`.
    void schedule(JobCounter& counter, JobFunction function, const JobOptions& options = {});

    // Schedules `function` and returns a handle to wait on or depend on.
    [[nodiscard]] JobHandle schedule(JobFunction function, const JobOptions& options = {});

    // Schedules `function` to run once every job in `dependencies` has
    // finished. Empty or finished dependencies are fine. The job does not
    // occupy a thread while it waits; it is queued when the last dependency
    // completes.
    [[nodiscard]] JobHandle scheduleAfter(std::span<const JobHandle> dependencies, JobFunction function,
                                          const JobOptions& options = {});

    // Blocks until the counter reaches zero. While waiting, the calling thread
    // runs other compute jobs, so waiting inside a job cannot deadlock the
    // pool and a waiting main thread still contributes work.
    void wait(const JobCounter& counter);
    void wait(const JobHandle& handle);

    // Queues `task` to run on the main thread during the next call to
    // runMainThreadTasks(). Any thread may call this; it is how a job hands a
    // result to code that must run on the main thread (GPU uploads, UI).
    void runOnMainThread(JobFunction task);

    // Runs every task posted with runOnMainThread() so far, in posting order,
    // and returns how many ran. Tasks posted while running wait for the next
    // call. Only the thread that created the JobSystem may call this.
    usize runMainThreadTasks();

    [[nodiscard]] u32 workerCount() const noexcept { return static_cast<u32>(m_workers.size()); }
    [[nodiscard]] u32 ioThreadCount() const noexcept { return static_cast<u32>(m_ioThreads.size()); }

    // The index of the calling compute worker, or nothing on other threads.
    [[nodiscard]] std::optional<u32> currentWorkerIndex() const noexcept;
    [[nodiscard]] bool isIoThread() const noexcept;

    // Compute jobs scheduled but not yet started.
    [[nodiscard]] u64 queuedJobCount() const noexcept { return m_queuedJobs.load(std::memory_order_relaxed); }

    // Takes a snapshot of the counters. Cheap enough to call every frame.
    // Not const: it remembers the time and total of the call to compute the
    // rate of the next one.
    [[nodiscard]] JobSystemStats stats();

private:
    struct Job {
        JobFunction function;
        JobCounter* counter = nullptr;
        // Set for jobs with a JobHandle: keeps the state alive even if every
        // handle is dropped, and holds the jobs that depend on this one.
        std::shared_ptr<detail::JobState> state;
        CancelToken cancel;
    };

    // A growable ring buffer of jobs behind a mutex. The owner pushes and pops
    // at the back, thieves take from the front. It doubles when full and never
    // shrinks, so after warm up no operation allocates.
    //
    // Pops first read a copy of the size without the lock and give up on an
    // empty queue. Idle workers sweep every queue of every priority, so
    // without that check they kept the mutexes so busy that a thread pushing
    // a job (the main thread scheduling chunk work) sometimes waited over a
    // millisecond for one. A stale copy only delays finding a job to the next
    // sweep; workers do not sleep while the queued job counter is above 0.
    class JobQueue {
    public:
        void pushBack(Job&& job);
        [[nodiscard]] bool popBack(Job& out);
        [[nodiscard]] bool popFront(Job& out);

    private:
        void grow();
        std::mutex m_mutex;
        std::vector<Job> m_ring;
        usize m_head = 0; // Index of the oldest job.
        usize m_size = 0;
        std::atomic<usize> m_approxSize{0}; // m_size, written under the lock, read without it.
    };

    // Per thread counters. Each one is written only by its own thread, so a
    // relaxed add is enough; stats() reads them from another thread. Aligned
    // to a cache line so threads never write to the same line.
    struct alignas(64) ThreadCounters {
        std::atomic<u64> jobsRun{0};
        std::atomic<i64> busyNanoseconds{0};
    };

    struct Worker {
        std::array<JobQueue, kPriorityCount> queues;
        std::thread thread;
        std::string name; // Kept here because Tracy stores the pointer for the thread's life.
        ThreadCounters counters;
    };

    // A group of threads that sleep on the same condition. Compute workers and
    // IO threads each have one, so waking one kind never wakes the other.
    struct SleepGroup {
        std::mutex mutex;
        std::condition_variable wakeUp;
        std::atomic<u32> sleeping{0};
    };

    // A job waiting for its dependencies. Each finished dependency counts
    // `remaining` down; the last one queues the job.
    struct PendingJob;

    friend struct detail::JobState;
    friend class JobHandle;

    void releaseDependency(const std::shared_ptr<PendingJob>& pending);

    void enqueue(Job&& job, const JobOptions& options);
    [[nodiscard]] bool findJob(Job& out, std::optional<u32> self);
    [[nodiscard]] bool findIoJob(Job& out);
    void runJob(Job& job, std::atomic<u64>& runningCounter, ThreadCounters& counters);
    [[nodiscard]] ThreadCounters& countersOfCurrentThread() noexcept;
    void workerLoop(u32 index);
    void ioLoop(u32 index);
    void sleepUntil(SleepGroup& group, const std::atomic<u64>& queued);
    static void wake(SleepGroup& group);

    std::vector<std::unique_ptr<Worker>> m_workers;
    std::array<JobQueue, kPriorityCount> m_sharedQueues;
    std::vector<std::thread> m_ioThreads;
    std::vector<std::string> m_ioThreadNames;
    std::vector<std::unique_ptr<ThreadCounters>> m_ioCounters;
    ThreadCounters m_helperCounters;
    std::array<JobQueue, kPriorityCount> m_ioQueues;

    // Queued plus running jobs per pool. The destructor drains until all are
    // zero; a running job may still schedule more, so queued alone is not enough.
    std::atomic<u64> m_queuedJobs{0};
    std::atomic<u64> m_runningJobs{0};
    std::atomic<u64> m_queuedIoJobs{0};
    std::atomic<u64> m_runningIoJobs{0};
    // Jobs waiting for dependencies. They count as outstanding work for the drain.
    std::atomic<u64> m_waitingJobs{0};

    // Per priority queue depths and totals for stats().
    std::array<std::atomic<u64>, kPriorityCount> m_queuedPerPriority{};
    std::array<std::atomic<u64>, kPriorityCount> m_queuedIoPerPriority{};
    std::atomic<u64> m_completedJobs{0};
    std::atomic<u64> m_cancelledJobs{0};
    TimePoint m_lastStatsTime{};
    u64 m_lastStatsCompleted = 0;

    SleepGroup m_workerSleep;
    SleepGroup m_ioSleep;
    std::atomic<bool> m_stopping{false};

    std::thread::id m_ownerThread;
    std::mutex m_mainThreadMutex;
    std::vector<JobFunction> m_mainThreadTasks;
    std::vector<JobFunction> m_mainThreadRunning; // Reused to avoid reallocating.
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace vxe::jobs
