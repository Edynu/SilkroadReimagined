#include "sr/core/Jobs.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "sr/core/Trace.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace sr::core {

namespace {

std::atomic<JobSystem*> g_instance{nullptr};
thread_local const JobSystem* t_system = nullptr;
thread_local int t_workerIndex = -1;

// A worker running streaming work (decoding, loading) gives way to every other thread: with the
// workers on all but three cores, a burst of streaming must not take time from the game, render
// and audio threads (frames held up, the sound breaking). Changed only when it changes.
void Background(bool on) {
    thread_local bool low = false;
    if (on == low) return;
    low = on;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), on ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
#endif
}

} // namespace

// A double-ended queue behind a mutex: the owner pushes and pops at the back (newest first,
// cache-warm), other threads take from the front (oldest first). The size is mirrored in an
// atomic so empty queues are skipped without taking the lock.
struct JobSystem::Queue {
    std::mutex mutex;
    std::deque<Job> jobs;
    std::atomic<std::size_t> size{0};

    void PushBack(Job&& job) {
        std::lock_guard lock(mutex);
        jobs.push_back(std::move(job));
        size.store(jobs.size(), std::memory_order_relaxed);
    }
    bool PopBack(Job& out) {
        if (size.load(std::memory_order_relaxed) == 0) return false;
        std::lock_guard lock(mutex);
        if (jobs.empty()) return false;
        out = std::move(jobs.back());
        jobs.pop_back();
        size.store(jobs.size(), std::memory_order_relaxed);
        return true;
    }
    bool PopFront(Job& out) {
        if (size.load(std::memory_order_relaxed) == 0) return false;
        std::lock_guard lock(mutex);
        if (jobs.empty()) return false;
        out = std::move(jobs.front());
        jobs.pop_front();
        size.store(jobs.size(), std::memory_order_relaxed);
        return true;
    }
};

struct JobSystem::Worker {
    std::thread thread;
    Queue queues[kJobPriorityCount];
};

JobSystem::JobSystem(int requested) {
    unsigned workers = 0;
    if (requested < 0) {
        // The game, render and network threads keep a core each.
        const unsigned hardware = std::thread::hardware_concurrency();
        workers = hardware > 5 ? hardware - 3 : 2;
    } else {
        workers = static_cast<unsigned>(requested);
    }
    shared_ = std::make_unique<Queue[]>(kJobPriorityCount);
    workers_.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) workers_.push_back(std::make_unique<Worker>());
    // Threads start once every worker exists: a thief may look at any of them.
    for (unsigned i = 0; i < workers; ++i) {
        workers_[i]->thread = std::thread([this, i] { WorkerLoop(static_cast<int>(i)); });
    }
    JobSystem* expected = nullptr;
    g_instance.compare_exchange_strong(expected, this);
}

JobSystem::~JobSystem() {
    stopping_.store(true, std::memory_order_release);
    wake_.fetch_add(1, std::memory_order_release);
    wake_.notify_all();
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) worker->thread.join();
    }
    // Jobs queued while the workers were leaving still run.
    Job job;
    bool stolen = false;
    while (TakeJob(-1, kJobPriorityCount - 1, job, stolen)) Execute(job);
    JobSystem* self = this;
    g_instance.compare_exchange_strong(self, nullptr);
}

JobSystem& JobSystem::Get() {
    JobSystem* instance = g_instance.load(std::memory_order_acquire);
    if (instance == nullptr) {
        std::fprintf(stderr, "fatal: JobSystem::Get() before a JobSystem exists\n");
        std::abort();
    }
    return *instance;
}

bool JobSystem::Exists() {
    return g_instance.load(std::memory_order_acquire) != nullptr;
}

bool JobSystem::OnWorkerThread() const {
    return t_system == this;
}

void JobSystem::Run(JobFunction job, JobPriority priority, JobCounter* counter) {
    if (workers_.empty()) {
        // Inline mode: the job runs now, on this thread.
        Job inline_{std::move(job), counter};
        if (counter != nullptr) counter->pending_.fetch_add(1, std::memory_order_relaxed);
        Execute(inline_);
        return;
    }
    Push(std::move(job), priority, counter, true);
}

unsigned JobSystem::SlotIndex() const {
    return t_system == this && t_workerIndex >= 0 ? static_cast<unsigned>(t_workerIndex) + 1 : 0;
}

void JobSystem::Push(JobFunction fn, JobPriority priority, JobCounter* counter, bool wake) {
    if (counter != nullptr) counter->pending_.fetch_add(1, std::memory_order_relaxed);
    Job job{std::move(fn), counter};
    const int p = static_cast<int>(priority);
    if (t_system == this && t_workerIndex >= 0) {
        workers_[static_cast<std::size_t>(t_workerIndex)]->queues[p].PushBack(std::move(job));
    } else {
        shared_[p].PushBack(std::move(job));
    }
    if (wake) WakeWorkers(1);
}

void JobSystem::WakeWorkers(std::size_t jobs) {
    wake_.fetch_add(1, std::memory_order_release);
    if (jobs == 1) {
        wake_.notify_one();
    } else {
        wake_.notify_all();
    }
}

bool JobSystem::TakeJob(int self, int maxPriority, Job& out, bool& stolen) {
    const int count = static_cast<int>(workers_.size());
    for (int p = 0; p <= maxPriority; ++p) {
        if (self >= 0 && workers_[static_cast<std::size_t>(self)]->queues[p].PopBack(out)) return true;
        if (shared_[p].PopFront(out)) return true;
        // Steal the oldest job of another worker, starting after ourselves so thieves spread out.
        const int start = self >= 0 ? self + 1 : 0;
        for (int k = 0; k < count; ++k) {
            const int victim = (start + k) % count;
            if (victim == self) continue;
            if (workers_[static_cast<std::size_t>(victim)]->queues[p].PopFront(out)) {
                stolen = true;
                return true;
            }
        }
    }
    return false;
}

void JobSystem::Execute(Job& job) {
    job.fn();
    executed_.fetch_add(1, std::memory_order_relaxed);
    if (JobCounter* counter = job.counter) {
        job.counter = nullptr;
        // After the last decrement the waiter may destroy the counter: touch only our own atomic.
        if (counter->pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            completions_.fetch_add(1, std::memory_order_release);
            completions_.notify_all();
        }
    }
    // Captures die last: they may own the counter (a shared state the job and its owner hold).
    job.fn = JobFunction();
}

void JobSystem::Wait(JobCounter& counter) {
    const int self = t_system == this ? t_workerIndex : -1;
    while (!counter.Done()) {
        Job job;
        bool stolen = false;
        if (TakeJob(self, static_cast<int>(JobPriority::Frame), job, stolen)) {
            helped_.fetch_add(1, std::memory_order_relaxed);
            Execute(job);
            continue;
        }
        const std::uint32_t seen = completions_.load(std::memory_order_acquire);
        if (counter.Done()) break;
        completions_.wait(seen, std::memory_order_acquire);
    }
}

void JobSystem::WorkerLoop(int index) {
    t_system = this;
    t_workerIndex = index;
    const std::string name = "sr worker " + std::to_string(index);
    SetThreadName(name.c_str());
    for (;;) {
        // Read the wake counter before looking: a job pushed after this read changes it, so the
        // wait below returns at once instead of sleeping past the job.
        const std::uint32_t seen = wake_.load(std::memory_order_acquire);
        Job job;
        bool stolen = false;
        bool streaming = false;
        if (TakeJob(index, static_cast<int>(JobPriority::Normal), job, stolen) ||
            (streaming = TakeJob(index, kJobPriorityCount - 1, job, stolen))) {
            if (stolen) stolen_.fetch_add(1, std::memory_order_relaxed);
            Background(streaming);
            Execute(job);
            continue;
        }
        if (stopping_.load(std::memory_order_acquire)) break;
        wake_.wait(seen, std::memory_order_acquire);
    }
    t_system = nullptr;
    t_workerIndex = -1;
}

JobSystem::Stats JobSystem::GetStats() const {
    Stats stats;
    stats.executed = executed_.load(std::memory_order_relaxed);
    stats.stolen = stolen_.load(std::memory_order_relaxed);
    stats.helped = helped_.load(std::memory_order_relaxed);
    return stats;
}

} // namespace sr::core
