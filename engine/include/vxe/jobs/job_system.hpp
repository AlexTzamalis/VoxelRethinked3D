#pragma once

#include <vxe/core/inline_function.hpp>
#include <vxe/core/types.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
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

// Bytes a job's callable may capture. 48 bytes hold six pointers or a few
// small values; larger data is passed by pointer. The limit keeps every job
// in a fixed size slot, so scheduling never allocates.
inline constexpr usize kJobCaptureBytes = 48;

using JobFunction = InlineFunction<kJobCaptureBytes>;

// Counts unfinished jobs. Scheduling a job with a counter increments it, and
// finishing the job decrements it, so a counter shared by many jobs tells when
// the whole batch is done. It lives wherever the caller wants (usually on the
// stack), so batches need no allocation at all.
//
// The counter must outlive every job that uses it. Waiting on it before it
// goes out of scope guarantees that.
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

// A handle to one scheduled job, for code that does not want to manage a
// counter. Copyable; all copies refer to the same job. Creating one costs a
// single small allocation, so prefer a JobCounter for large batches.
class JobHandle {
public:
    JobHandle() noexcept = default;

    // True once the job has finished. An empty handle counts as done.
    [[nodiscard]] bool isDone() const noexcept { return !m_counter || m_counter->isDone(); }
    [[nodiscard]] bool isValid() const noexcept { return m_counter != nullptr; }

private:
    friend class JobSystem;
    explicit JobHandle(std::shared_ptr<JobCounter> counter) noexcept
        : m_counter(std::move(counter)) {}
    std::shared_ptr<JobCounter> m_counter;
};

struct JobSystemConfig {
    // Number of worker threads. 0 picks hardware_concurrency - 2 (leaving the
    // main and server threads a core each, Section 6.4), but at least 1.
    u32 workerCount = 0;
};

// A work stealing thread pool with priorities (ADR-012).
//
// Each worker owns one queue per priority. Jobs scheduled from a worker go to
// its own queue, where the worker takes the newest first (good for cache, the
// data a job just produced is still hot). Jobs scheduled from other threads
// go to a shared queue per priority. A worker looks for work from the highest
// priority down: its own queue, then the shared queue, then the oldest job of
// another worker (stealing), so busy workers are relieved automatically.
//
// Idle workers sleep and are woken when work arrives, so an idle engine uses
// no CPU.
//
// The queues are short critical sections behind a mutex rather than a lock
// free deque. That is simple to prove correct and fast enough for chunk sized
// jobs; a lock free version only replaces the queue type if profiling ever
// shows contention (Section 0.2: measure before optimizing).
//
// Thread safety: schedule(), wait() and the queries may be called from any
// thread, including from inside jobs.
class JobSystem {
public:
    explicit JobSystem(const JobSystemConfig& config = {});
    // Runs every job that is still queued, then stops and joins the workers.
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    // Schedules `function` and counts it in `counter`.
    void schedule(JobCounter& counter, JobFunction function, Priority priority = Priority::Normal);

    // Schedules `function` and returns a handle to wait on.
    [[nodiscard]] JobHandle schedule(JobFunction function, Priority priority = Priority::Normal);

    // Blocks until the counter reaches zero. While waiting, the calling thread
    // runs other queued jobs, so waiting inside a job cannot deadlock the pool
    // and a waiting main thread still contributes work.
    void wait(const JobCounter& counter);
    void wait(const JobHandle& handle);

    [[nodiscard]] u32 workerCount() const noexcept { return static_cast<u32>(m_workers.size()); }

    // The index of the calling worker thread, or nothing on other threads.
    [[nodiscard]] std::optional<u32> currentWorkerIndex() const noexcept;

    // Jobs scheduled but not yet started, over all queues.
    [[nodiscard]] u64 queuedJobCount() const noexcept { return m_queuedJobs.load(std::memory_order_relaxed); }

private:
    struct Job {
        JobFunction function;
        JobCounter* counter = nullptr;
        // Set for jobs scheduled with a JobHandle: keeps the counter alive even
        // if every handle is dropped before the job finishes.
        std::shared_ptr<JobCounter> keepAlive;
    };

    // A growable ring buffer of jobs behind a mutex. The owner pushes and pops
    // at the back, thieves take from the front. It doubles when full and never
    // shrinks, so after warm up no operation allocates.
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
    };

    struct Worker {
        std::array<JobQueue, kPriorityCount> queues;
        std::thread thread;
        std::string name; // Kept here because Tracy stores the pointer for the thread's life.
    };

    void enqueue(Job&& job, Priority priority);
    [[nodiscard]] bool findJob(Job& out, std::optional<u32> self);
    void runJob(Job& job);
    void workerLoop(u32 index);

    std::vector<std::unique_ptr<Worker>> m_workers;
    std::array<JobQueue, kPriorityCount> m_sharedQueues;

    // Queued plus running jobs. The destructor drains until both are zero; a
    // running job may still schedule more, so queued alone is not enough.
    std::atomic<u64> m_queuedJobs{0};
    std::atomic<u64> m_runningJobs{0};
    std::atomic<u32> m_sleepingWorkers{0};
    std::atomic<bool> m_stopping{false};
    std::mutex m_sleepMutex;
    std::condition_variable m_wakeUp;
};

} // namespace vxe::jobs
