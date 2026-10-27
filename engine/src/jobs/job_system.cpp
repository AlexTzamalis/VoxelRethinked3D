#include "vxe/jobs/job_system.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/profile.hpp>

#include <algorithm>
#include <format>

namespace vxe::jobs {

struct JobSystem::PendingJob {
    std::atomic<u32> remaining{0};
    Job job;
    JobOptions options;
};

namespace detail {

// The heap state behind a JobHandle: the completion counter plus the jobs
// waiting for this one. `finished` and `continuations` are guarded by the
// mutex, which makes "add a continuation" and "finish" ordered: a
// continuation added before the job finishes is released by the finishing
// thread, one added after sees `finished` and releases itself.
struct JobState {
    JobCounter counter;
    std::mutex mutex;
    bool finished = false;
    std::vector<std::shared_ptr<JobSystem::PendingJob>> continuations;
};

} // namespace detail

namespace {

// Which job system the current thread works for. Plain thread locals, because
// a thread belongs to at most one job system.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables): per thread identity, set once per thread.
thread_local const JobSystem* workerOwner = nullptr;
thread_local u32 workerIndex = 0;
thread_local const JobSystem* ioOwner = nullptr;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// An idle thread yields this many times before it goes to sleep. Spinning a
// little keeps latency low when jobs arrive in quick bursts (meshing a batch
// of chunks), sleeping keeps an idle engine at zero CPU.
constexpr u32 kSpinsBeforeSleep = 64;

constexpr usize kInitialQueueCapacity = 64;

u32 resolveWorkerCount(u32 requested) {
    if (requested != 0) {
        return requested;
    }
    const u32 hardware = std::thread::hardware_concurrency();
    return hardware > 3 ? hardware - 2 : 1;
}

} // namespace

bool JobHandle::isDone() const noexcept {
    return !m_state || m_state->counter.isDone();
}

// ---------------------------------------------------------------------------
// JobQueue

void JobSystem::JobQueue::grow() {
    // Capacities stay powers of two so the index wraps with a mask.
    const usize newCapacity = m_ring.empty() ? kInitialQueueCapacity : m_ring.size() * 2;
    std::vector<Job> grown(newCapacity);
    const usize mask = m_ring.size() - 1;
    for (usize i = 0; i < m_size; ++i) {
        grown[i] = std::move(m_ring[(m_head + i) & mask]);
    }
    m_ring = std::move(grown);
    m_head = 0;
}

void JobSystem::JobQueue::pushBack(Job&& job) {
    const std::scoped_lock lock(m_mutex);
    if (m_size == m_ring.size()) {
        grow();
    }
    m_ring[(m_head + m_size) & (m_ring.size() - 1)] = std::move(job);
    ++m_size;
}

bool JobSystem::JobQueue::popBack(Job& out) {
    const std::scoped_lock lock(m_mutex);
    if (m_size == 0) {
        return false;
    }
    --m_size;
    out = std::move(m_ring[(m_head + m_size) & (m_ring.size() - 1)]);
    return true;
}

bool JobSystem::JobQueue::popFront(Job& out) {
    const std::scoped_lock lock(m_mutex);
    if (m_size == 0) {
        return false;
    }
    out = std::move(m_ring[m_head]);
    m_head = (m_head + 1) & (m_ring.size() - 1);
    --m_size;
    return true;
}

// ---------------------------------------------------------------------------
// Construction and shutdown

JobSystem::JobSystem(const JobSystemConfig& config)
    : m_ownerThread(std::this_thread::get_id()) {
    const u32 count = resolveWorkerCount(config.workerCount);
    m_workers.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        auto worker = std::make_unique<Worker>();
        worker->name = std::format("Worker {}", i);
        m_workers.push_back(std::move(worker));
    }
    m_ioThreadNames.reserve(config.ioThreadCount);
    for (u32 i = 0; i < config.ioThreadCount; ++i) {
        m_ioThreadNames.push_back(std::format("IO {}", i));
    }

    // Threads start only after every worker exists, because a worker may try
    // to steal from any other worker the moment it runs.
    for (u32 i = 0; i < count; ++i) {
        m_workers[i]->thread = std::thread([this, i] { workerLoop(i); });
    }
    m_ioThreads.reserve(config.ioThreadCount);
    for (u32 i = 0; i < config.ioThreadCount; ++i) {
        m_ioThreads.emplace_back([this, i] { ioLoop(i); });
    }
}

// A thread join failure or a throwing job during shutdown cannot be recovered
// from; terminating (what an escaping exception does here) is the intended outcome.
// NOLINTNEXTLINE(bugprone-exception-escape)
JobSystem::~JobSystem() {
    // Finish everything that was scheduled, helping with compute jobs from
    // this thread. Jobs may schedule more jobs and finishing jobs may release
    // continuations, so the loop ends only when nothing is queued, running or
    // waiting in either pool.
    const auto outstanding = [this] {
        return m_queuedJobs.load(std::memory_order_acquire) + m_runningJobs.load(std::memory_order_acquire) +
               m_queuedIoJobs.load(std::memory_order_acquire) +
               m_runningIoJobs.load(std::memory_order_acquire) +
               m_waitingJobs.load(std::memory_order_acquire);
    };
    Job job;
    while (outstanding() > 0) {
        if (findJob(job, currentWorkerIndex())) {
            runJob(job, m_runningJobs);
        } else {
            std::this_thread::yield();
        }
    }

    // Setting the flag under each group's mutex orders it with a thread that
    // is about to check its wait condition, so nothing sleeps through it.
    for (SleepGroup* group : {&m_workerSleep, &m_ioSleep}) {
        const std::scoped_lock lock(group->mutex);
        m_stopping.store(true, std::memory_order_seq_cst);
    }
    m_workerSleep.wakeUp.notify_all();
    m_ioSleep.wakeUp.notify_all();
    for (const std::unique_ptr<Worker>& worker : m_workers) {
        worker->thread.join();
    }
    for (std::thread& thread : m_ioThreads) {
        thread.join();
    }
}

std::optional<u32> JobSystem::currentWorkerIndex() const noexcept {
    if (workerOwner == this) {
        return workerIndex;
    }
    return std::nullopt;
}

bool JobSystem::isIoThread() const noexcept {
    return ioOwner == this;
}

// ---------------------------------------------------------------------------
// Scheduling

void JobSystem::schedule(JobCounter& counter, JobFunction function, const JobOptions& options) {
    VXE_ASSERT(static_cast<bool>(function), "Scheduling an empty job");
    // Counted before the job becomes visible, so a waiter can never see zero
    // while the job is still queued.
    counter.m_pending.fetch_add(1, std::memory_order_relaxed);
    enqueue(
        Job{.function = std::move(function), .counter = &counter, .state = nullptr, .cancel = options.cancel},
        options);
}

JobHandle JobSystem::schedule(JobFunction function, const JobOptions& options) {
    VXE_ASSERT(static_cast<bool>(function), "Scheduling an empty job");
    auto state = std::make_shared<detail::JobState>();
    state->counter.m_pending.store(1, std::memory_order_relaxed);
    JobCounter* counter = &state->counter;
    enqueue(
        Job{.function = std::move(function), .counter = counter, .state = state, .cancel = options.cancel},
        options);
    return JobHandle(std::move(state));
}

JobHandle JobSystem::scheduleAfter(std::span<const JobHandle> dependencies, JobFunction function,
                                   const JobOptions& options) {
    VXE_ASSERT(static_cast<bool>(function), "Scheduling an empty job");
    auto state = std::make_shared<detail::JobState>();
    state->counter.m_pending.store(1, std::memory_order_relaxed);

    auto pending = std::make_shared<PendingJob>();
    pending->job = Job{.function = std::move(function),
                       .counter = &state->counter,
                       .state = state,
                       .cancel = options.cancel};
    pending->options = options;
    // One count per dependency plus one held by this function, so the job
    // cannot be queued while the dependencies are still being registered.
    pending->remaining.store(static_cast<u32>(dependencies.size()) + 1, std::memory_order_relaxed);
    m_waitingJobs.fetch_add(1, std::memory_order_acq_rel);

    for (const JobHandle& dependency : dependencies) {
        bool alreadyFinished = true;
        if (dependency.m_state) {
            const std::scoped_lock lock(dependency.m_state->mutex);
            if (!dependency.m_state->finished) {
                dependency.m_state->continuations.push_back(pending);
                alreadyFinished = false;
            }
        }
        if (alreadyFinished) {
            releaseDependency(pending);
        }
    }
    releaseDependency(pending); // Drop the count held during registration.
    return JobHandle(std::move(state));
}

void JobSystem::releaseDependency(const std::shared_ptr<PendingJob>& pending) {
    if (pending->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Queued before it stops counting as waiting, so the shutdown drain
        // always sees it in one of the two counts.
        enqueue(std::move(pending->job), pending->options);
        m_waitingJobs.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void JobSystem::enqueue(Job&& job, const JobOptions& options) {
    const auto level = static_cast<usize>(options.priority);
    // Without IO threads an IO job would never run, so it falls back to the
    // compute pool rather than hanging forever.
    if (options.pool == Pool::Io && !m_ioThreads.empty()) {
        m_ioQueues[level].pushBack(std::move(job));
        m_queuedIoJobs.fetch_add(1, std::memory_order_seq_cst);
        wake(m_ioSleep);
        return;
    }

    if (const std::optional<u32> self = currentWorkerIndex()) {
        m_workers[*self]->queues[level].pushBack(std::move(job));
    } else {
        m_sharedQueues[level].pushBack(std::move(job));
    }
    // See sleepUntil() for why both sides use sequentially consistent order.
    m_queuedJobs.fetch_add(1, std::memory_order_seq_cst);
    wake(m_workerSleep);
}

// ---------------------------------------------------------------------------
// Running

bool JobSystem::findJob(Job& out, std::optional<u32> self) {
    const auto workerCount = static_cast<u32>(m_workers.size());
    for (usize level = 0; level < kPriorityCount; ++level) {
        bool found = self.has_value() && m_workers[*self]->queues[level].popBack(out);
        if (!found) {
            found = m_sharedQueues[level].popFront(out);
        }
        // Steal the oldest job of another worker, starting after ourselves so
        // thieves spread over different victims.
        const u32 start = self.value_or(0) + 1;
        for (u32 k = 0; !found && k < workerCount; ++k) {
            const u32 victim = (start + k) % workerCount;
            if (self.has_value() && victim == *self) {
                continue;
            }
            found = m_workers[victim]->queues[level].popFront(out);
        }
        if (found) {
            // Running is raised before queued drops, so the shutdown drain
            // never sees both at zero while this job is in flight.
            m_runningJobs.fetch_add(1, std::memory_order_acq_rel);
            m_queuedJobs.fetch_sub(1, std::memory_order_acq_rel);
            return true;
        }
    }
    return false;
}

bool JobSystem::findIoJob(Job& out) {
    for (JobQueue& queue : m_ioQueues) {
        if (queue.popFront(out)) {
            m_runningIoJobs.fetch_add(1, std::memory_order_acq_rel);
            m_queuedIoJobs.fetch_sub(1, std::memory_order_acq_rel);
            return true;
        }
    }
    return false;
}

void JobSystem::runJob(Job& job, std::atomic<u64>& runningCounter) {
    // A job cancelled before it started is skipped, but it still completes:
    // counters and dependents must not wait forever for work nobody wants.
    if (!job.cancel.isCancelled()) {
        job.function();
    }
    // Captures are destroyed before the job counts as done, so a waiter that
    // continues never races with a destructor still running here.
    job.function.reset();
    job.cancel = CancelToken{};

    std::shared_ptr<detail::JobState> state = std::move(job.state);
    JobCounter* counter = std::exchange(job.counter, nullptr);

    std::vector<std::shared_ptr<PendingJob>> released;
    if (state) {
        const std::scoped_lock lock(state->mutex);
        state->finished = true;
        released.swap(state->continuations);
    }
    if (counter != nullptr) {
        // Counted as done before any dependent is released, so a finished
        // dependent always implies finished dependencies. After this a waiter
        // may destroy a stack counter, so it is not touched again; handle
        // counters stay alive through `state`.
        counter->m_pending.fetch_sub(1, std::memory_order_acq_rel);
    }
    // Releasing dependents uses only the heap state. They are queued before
    // this job stops counting as running, so the shutdown drain never misses them.
    for (const std::shared_ptr<PendingJob>& pending : released) {
        releaseDependency(pending);
    }
    state.reset();
    runningCounter.fetch_sub(1, std::memory_order_acq_rel);
}

void JobSystem::wait(const JobCounter& counter) {
    VXE_PROFILE_FUNCTION();
    Job job;
    const std::optional<u32> self = currentWorkerIndex();
    while (!counter.isDone()) {
        if (findJob(job, self)) {
            runJob(job, m_runningJobs);
        } else {
            std::this_thread::yield();
        }
    }
}

void JobSystem::wait(const JobHandle& handle) {
    if (handle.m_state) {
        wait(handle.m_state->counter);
    }
}

// ---------------------------------------------------------------------------
// Threads

void JobSystem::sleepUntil(SleepGroup& group, const std::atomic<u64>& queued) {
    std::unique_lock lock(group.mutex);
    // Announce the sleep first, then check for work. enqueue() does the mirror
    // image (count the job first, then check for sleepers), and both use
    // sequentially consistent order, so at least one side always sees the
    // other: either this thread sees the job and stays awake, or the
    // scheduler sees the sleeper and wakes it. No wakeup can be lost.
    group.sleeping.fetch_add(1, std::memory_order_seq_cst);
    group.wakeUp.wait(lock, [this, &queued] {
        return queued.load(std::memory_order_seq_cst) > 0 || m_stopping.load(std::memory_order_seq_cst);
    });
    group.sleeping.fetch_sub(1, std::memory_order_seq_cst);
}

void JobSystem::wake(SleepGroup& group) {
    if (group.sleeping.load(std::memory_order_seq_cst) > 0) {
        const std::scoped_lock lock(group.mutex);
        group.wakeUp.notify_one();
    }
}

void JobSystem::workerLoop(u32 index) {
    workerOwner = this;
    workerIndex = index;
    profiling::setThreadName(m_workers[index]->name.c_str());

    Job job;
    u32 idleSpins = 0;
    while (true) {
        if (findJob(job, index)) {
            runJob(job, m_runningJobs);
            idleSpins = 0;
            continue;
        }
        if (m_stopping.load(std::memory_order_acquire)) {
            break;
        }
        if (++idleSpins < kSpinsBeforeSleep) {
            std::this_thread::yield();
            continue;
        }
        idleSpins = 0;
        sleepUntil(m_workerSleep, m_queuedJobs);
    }
}

void JobSystem::ioLoop(u32 index) {
    ioOwner = this;
    profiling::setThreadName(m_ioThreadNames[index].c_str());

    Job job;
    u32 idleSpins = 0;
    while (true) {
        if (findIoJob(job)) {
            runJob(job, m_runningIoJobs);
            idleSpins = 0;
            continue;
        }
        if (m_stopping.load(std::memory_order_acquire)) {
            break;
        }
        if (++idleSpins < kSpinsBeforeSleep) {
            std::this_thread::yield();
            continue;
        }
        idleSpins = 0;
        sleepUntil(m_ioSleep, m_queuedIoJobs);
    }
}

// ---------------------------------------------------------------------------
// Main thread tasks

void JobSystem::runOnMainThread(JobFunction task) {
    VXE_ASSERT(static_cast<bool>(task), "Posting an empty main thread task");
    const std::scoped_lock lock(m_mainThreadMutex);
    m_mainThreadTasks.push_back(std::move(task));
}

usize JobSystem::runMainThreadTasks() {
    VXE_ASSERT(std::this_thread::get_id() == m_ownerThread,
               "runMainThreadTasks must run on the thread that created the JobSystem");
    {
        // Swap the lists, so tasks posted while these run go to the next call
        // and the lock is never held while running foreign code.
        const std::scoped_lock lock(m_mainThreadMutex);
        m_mainThreadRunning.swap(m_mainThreadTasks);
    }
    for (JobFunction& task : m_mainThreadRunning) {
        task();
    }
    const usize count = m_mainThreadRunning.size();
    m_mainThreadRunning.clear(); // Keeps the capacity for the next frame.
    return count;
}

} // namespace vxe::jobs
