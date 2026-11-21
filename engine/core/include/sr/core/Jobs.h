#pragma once

// The job system: one scheduler for all parallel work.
//
//   core::JobSystem jobs;                        // workers = hardware threads - 3 (at least 2)
//   core::JobCounter done;
//   jobs.Run([&] { DecodeRegion(...); }, core::JobPriority::Streaming, &done);
//   if (done.Done()) ...                         // poll from the frame, never block on streaming
//
//   jobs.ParallelFor(instances.size(), 256, [&](std::size_t begin, std::size_t end) {
//       for (std::size_t i = begin; i < end; ++i) Cull(instances[i]);
//   });                                          // returns when every batch ran
//
// Workers take their own newest job first, then the shared queue, then steal the oldest job of
// another worker; priorities are strict (all Frame jobs before Normal before Streaming).
// Wait() runs Frame jobs while it waits instead of blocking, so it may be called from the main
// thread and from inside a job. Rules that keep it deadlock-free: only wait on groups of Frame
// jobs, and a Streaming job never waits. Jobs must not throw (an escaping exception ends the
// program, as on std::thread). Inside jobs, time with SR_ZONE (Trace.h), not SR_PROFILE.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace sr::core {

enum class JobPriority : std::uint8_t {
    Frame = 0,     // this frame's critical path (culling, animation, recording)
    Normal = 1,    // work without a frame deadline
    Streaming = 2, // decoding and background loading
};
inline constexpr int kJobPriorityCount = 3;

// Counts the unfinished jobs of a group. Must outlive its jobs.
class JobCounter {
public:
    JobCounter() = default;
    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;

    bool Done() const { return pending_.load(std::memory_order_acquire) == 0; }
    std::int32_t Pending() const { return pending_.load(std::memory_order_acquire); }

private:
    friend class JobSystem;
    std::atomic<std::int32_t> pending_{0};
};

// A move-only callable with room for small captures inline (larger ones go to the heap).
class JobFunction {
public:
    static constexpr std::size_t kInlineBytes = 48;

    JobFunction() = default;
    template <typename Fn, typename F = std::decay_t<Fn>,
              typename = std::enable_if_t<!std::is_same_v<F, JobFunction> && std::is_invocable_v<F&>>>
    JobFunction(Fn&& fn) { // NOLINT: implicit on purpose, so Run() takes lambdas directly
        if constexpr (sizeof(F) <= kInlineBytes && alignof(F) <= alignof(std::max_align_t) &&
                      std::is_nothrow_move_constructible_v<F>) {
            new (storage_) F(std::forward<Fn>(fn));
            ops_ = InlineOps<F>();
        } else {
            *reinterpret_cast<F**>(storage_) = new F(std::forward<Fn>(fn));
            ops_ = HeapOps<F>();
        }
    }
    JobFunction(JobFunction&& other) noexcept : ops_(other.ops_) {
        if (ops_ != nullptr) {
            ops_->move(storage_, other.storage_);
            other.ops_ = nullptr;
        }
    }
    JobFunction& operator=(JobFunction&& other) noexcept {
        if (this != &other) {
            Reset();
            ops_ = other.ops_;
            if (ops_ != nullptr) {
                ops_->move(storage_, other.storage_);
                other.ops_ = nullptr;
            }
        }
        return *this;
    }
    JobFunction(const JobFunction&) = delete;
    JobFunction& operator=(const JobFunction&) = delete;
    ~JobFunction() { Reset(); }

    explicit operator bool() const { return ops_ != nullptr; }
    void operator()() { ops_->invoke(storage_); }

private:
    struct Ops {
        void (*invoke)(void* storage);
        void (*move)(void* dst, void* src); // move-constructs into dst and destroys src
        void (*destroy)(void* storage);
    };
    template <typename F>
    static const Ops* InlineOps() {
        static const Ops ops{
            [](void* s) { (*std::launder(reinterpret_cast<F*>(s)))(); },
            [](void* d, void* s) {
                F* from = std::launder(reinterpret_cast<F*>(s));
                new (d) F(std::move(*from));
                from->~F();
            },
            [](void* s) { std::launder(reinterpret_cast<F*>(s))->~F(); },
        };
        return &ops;
    }
    template <typename F>
    static const Ops* HeapOps() {
        static const Ops ops{
            [](void* s) { (**reinterpret_cast<F**>(s))(); },
            [](void* d, void* s) { *reinterpret_cast<F**>(d) = *reinterpret_cast<F**>(s); },
            [](void* s) { delete *reinterpret_cast<F**>(s); },
        };
        return &ops;
    }

    void Reset() {
        if (ops_ != nullptr) {
            ops_->destroy(storage_);
            ops_ = nullptr;
        }
    }

    alignas(std::max_align_t) std::byte storage_[kInlineBytes];
    const Ops* ops_ = nullptr;
};

class JobSystem {
public:
    struct Stats {
        std::uint64_t executed = 0; // jobs run (by workers and by waiting threads)
        std::uint64_t stolen = 0;   // jobs a worker took from another worker's queue
        std::uint64_t helped = 0;   // jobs run by a thread inside Wait()
    };

    static constexpr int kDefaultWorkers = -1;
    // `workers` kDefaultWorkers: hardware threads - 3 (the game, render and network threads keep
    // a core each), at least 2. 0: no worker threads; every job runs inline on the thread that
    // runs it or waits for it (`--workers 0`, for debugging). The first
    // JobSystem created becomes the process-wide one (Get()) until it is destroyed.
    explicit JobSystem(int workers = kDefaultWorkers);
    // Runs every queued job, then stops the workers.
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    static JobSystem& Get();
    static bool Exists();

    // Queues a job. With a counter, the counter counts it until it has run.
    void Run(JobFunction job, JobPriority priority = JobPriority::Normal, JobCounter* counter = nullptr);

    // Returns when the counter reaches zero, running Frame jobs meanwhile.
    void Wait(JobCounter& counter);

    // Calls fn(begin, end) for [0, count) in batches of `batch` (Frame priority); the calling
    // thread takes part; returns when all batches ran.
    //
    // The batches are not queued one job each: the caller and up to one helper job per worker take
    // the next batch from a shared counter until none is left. So a thousand small batches cost a
    // few jobs, not a thousand trips through the queues' locks; and the call returns the moment the
    // last batch is done, not when every helper has run: a helper the system has not given a core
    // yet (another process's turn, a parked core) finds nothing left and does nothing, instead of
    // holding the caller for the operating system's time slice (milliseconds; on Windows 15-30).
    template <typename Fn>
    void ParallelFor(std::size_t count, std::size_t batch, Fn&& fn) {
        if (count == 0) return;
        if (batch == 0) batch = 1;
        const std::size_t batches = (count + batch - 1) / batch;
        if (batches == 1 || workers_.empty()) {
            fn(std::size_t{0}, count);
            return;
        }
        using F = std::remove_reference_t<Fn>;
        ParallelBatches(count, batch, batches,
                        [](void* f, std::size_t begin, std::size_t end) { (*static_cast<F*>(f))(begin, end); },
                        const_cast<void*>(static_cast<const void*>(std::addressof(fn))));
    }

    unsigned WorkerCount() const { return static_cast<unsigned>(workers_.size()); }
    // Per-thread slots for data a parallel phase writes (command buffers):
    // WorkerCount() + 1. SlotIndex() is the calling thread's: 0 for any thread that is not one
    // of this system's workers, 1 + its index for a worker.
    unsigned SlotCount() const { return WorkerCount() + 1; }
    unsigned SlotIndex() const;
    // The calling thread is one of this system's workers.
    bool OnWorkerThread() const;
    Stats GetStats() const;

private:
    struct Job {
        JobFunction fn;
        JobCounter* counter = nullptr;
    };
    struct Queue; // a locked double-ended queue (Jobs.cpp)
    struct Worker;
    struct Batches; // one ParallelFor's shared state (Jobs.cpp)
    using BatchFunction = void (*)(void* fn, std::size_t begin, std::size_t end);

    void ParallelBatches(std::size_t count, std::size_t batch, std::size_t batches, BatchFunction call, void* fn);
    void RunBatches(Batches& work);
    void Push(JobFunction fn, JobPriority priority, JobCounter* counter, bool wake);
    void WakeWorkers(std::size_t jobs);
    bool TakeJob(int self, int maxPriority, Job& out, bool& stolen);
    void Execute(Job& job);
    void WorkerLoop(int index);

    std::vector<std::unique_ptr<Worker>> workers_;
    std::unique_ptr<Queue[]> shared_; // one per priority, for jobs from threads that are not workers
    std::atomic<std::uint32_t> wake_{0};        // bumped when jobs arrive; idle workers sleep on it
    // Bumped when a counter reaches zero; Wait() sleeps on this, never on the counter itself, so a
    // waiter may destroy its counter the moment it reads zero.
    std::atomic<std::uint32_t> completions_{0};
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> executed_{0}, stolen_{0}, helped_{0};
};

} // namespace sr::core
