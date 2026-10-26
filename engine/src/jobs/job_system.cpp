#include "vxe/jobs/job_system.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/profile.hpp>

#include <algorithm>
#include <format>

namespace vxe::jobs {
namespace {

// Which job system and worker the current thread belongs to. Plain thread
// locals, because a thread is a worker of at most one job system.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables): per thread identity, set once per worker.
thread_local const JobSystem* workerOwner = nullptr;
thread_local u32 workerIndex = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// An idle worker yields this many times before it goes to sleep. Spinning a
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
// JobSystem

JobSystem::JobSystem(const JobSystemConfig& config) {
    const u32 count = resolveWorkerCount(config.workerCount);
    m_workers.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        auto worker = std::make_unique<Worker>();
        worker->name = std::format("Worker {}", i);
        m_workers.push_back(std::move(worker));
    }
    // Threads start only after every worker exists, because a worker may try
    // to steal from any other worker the moment it runs.
    for (u32 i = 0; i < count; ++i) {
        m_workers[i]->thread = std::thread([this, i] { workerLoop(i); });
    }
}

JobSystem::~JobSystem() {
    // Finish everything that was scheduled, helping from this thread. Jobs may
    // schedule more jobs, so the loop ends only when nothing is queued and
    // nothing is running.
    Job job;
    while (m_queuedJobs.load(std::memory_order_acquire) > 0 ||
           m_runningJobs.load(std::memory_order_acquire) > 0) {
        if (findJob(job, currentWorkerIndex())) {
            runJob(job);
        } else {
            std::this_thread::yield();
        }
    }

    {
        // Taking the mutex orders the stop flag with a worker that is about to
        // check its wait condition, so no worker sleeps through the shutdown.
        const std::scoped_lock lock(m_sleepMutex);
        m_stopping.store(true, std::memory_order_seq_cst);
    }
    m_wakeUp.notify_all();
    for (const std::unique_ptr<Worker>& worker : m_workers) {
        worker->thread.join();
    }
}

std::optional<u32> JobSystem::currentWorkerIndex() const noexcept {
    if (workerOwner == this) {
        return workerIndex;
    }
    return std::nullopt;
}

void JobSystem::schedule(JobCounter& counter, JobFunction function, Priority priority) {
    VXE_ASSERT(static_cast<bool>(function), "Scheduling an empty job");
    // Counted before the job becomes visible, so a waiter can never see zero
    // while the job is still queued.
    counter.m_pending.fetch_add(1, std::memory_order_relaxed);
    enqueue(Job{.function = std::move(function), .counter = &counter, .keepAlive = nullptr}, priority);
}

JobHandle JobSystem::schedule(JobFunction function, Priority priority) {
    VXE_ASSERT(static_cast<bool>(function), "Scheduling an empty job");
    auto counter = std::make_shared<JobCounter>();
    counter->m_pending.fetch_add(1, std::memory_order_relaxed);
    JobCounter* raw = counter.get();
    enqueue(Job{.function = std::move(function), .counter = raw, .keepAlive = counter}, priority);
    return JobHandle(std::move(counter));
}

void JobSystem::enqueue(Job&& job, Priority priority) {
    const auto level = static_cast<usize>(priority);
    if (const std::optional<u32> self = currentWorkerIndex()) {
        m_workers[*self]->queues[level].pushBack(std::move(job));
    } else {
        m_sharedQueues[level].pushBack(std::move(job));
    }

    // Wake a sleeping worker. Both this counter update and the sleeper count
    // below use sequentially consistent order, and a worker going to sleep
    // updates them in the opposite order (see workerLoop). That guarantees at
    // least one side sees the other: either the worker sees the new job and
    // stays awake, or this thread sees the sleeper and wakes it.
    m_queuedJobs.fetch_add(1, std::memory_order_seq_cst);
    if (m_sleepingWorkers.load(std::memory_order_seq_cst) > 0) {
        const std::scoped_lock lock(m_sleepMutex);
        m_wakeUp.notify_one();
    }
}

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

void JobSystem::runJob(Job& job) {
    job.function();
    // Captures are destroyed before the job counts as done, so a waiter that
    // continues never races with a destructor still running here.
    job.function.reset();
    JobCounter* counter = std::exchange(job.counter, nullptr);
    std::shared_ptr<JobCounter> keepAlive = std::move(job.keepAlive);
    if (counter != nullptr) {
        // After this the waiter may destroy the counter (unless keepAlive
        // holds it), so it is not touched again.
        counter->m_pending.fetch_sub(1, std::memory_order_acq_rel);
    }
    keepAlive.reset();
    m_runningJobs.fetch_sub(1, std::memory_order_acq_rel);
}

void JobSystem::wait(const JobCounter& counter) {
    VXE_PROFILE_FUNCTION();
    Job job;
    const std::optional<u32> self = currentWorkerIndex();
    while (!counter.isDone()) {
        if (findJob(job, self)) {
            runJob(job);
        } else {
            std::this_thread::yield();
        }
    }
}

void JobSystem::wait(const JobHandle& handle) {
    if (handle.m_counter) {
        wait(*handle.m_counter);
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
            runJob(job);
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

        std::unique_lock lock(m_sleepMutex);
        // Announce the sleep first, then check for work (the mirror image of
        // enqueue), so a job scheduled right now cannot be missed.
        m_sleepingWorkers.fetch_add(1, std::memory_order_seq_cst);
        m_wakeUp.wait(lock, [this] {
            return m_queuedJobs.load(std::memory_order_seq_cst) > 0 ||
                   m_stopping.load(std::memory_order_seq_cst);
        });
        m_sleepingWorkers.fetch_sub(1, std::memory_order_seq_cst);
    }
}

} // namespace vxe::jobs
