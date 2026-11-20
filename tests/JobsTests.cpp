#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <numeric>
#include <thread>
#include <utility>
#include <vector>

#include "sr/core/Jobs.h"

using sr::core::JobCounter;
using sr::core::JobFunction;
using sr::core::JobPriority;
using sr::core::JobSystem;

TEST_CASE("JobFunction: small and large captures, moves") {
    int calls = 0;
    JobFunction small([&calls] { ++calls; });
    small();
    CHECK(calls == 1);

    // Larger than the inline buffer: goes to the heap, still callable and movable.
    std::array<std::uint64_t, 16> big{};
    big[15] = 7;
    std::uint64_t seen = 0;
    JobFunction large([big, &seen] { seen = big[15]; });
    JobFunction moved = std::move(large);
    CHECK_FALSE(static_cast<bool>(large));
    moved();
    CHECK(seen == 7);

    // Move-only captures work.
    auto owned = std::make_unique<int>(42);
    int value = 0;
    JobFunction withUnique([p = std::move(owned), &value] { value = *p; });
    withUnique();
    CHECK(value == 42);
}

TEST_CASE("JobSystem: every job runs exactly once") {
    JobSystem jobs(4);
    constexpr int kJobs = 100'000;
    std::vector<std::atomic<int>> runs(kJobs);
    JobCounter counter;
    for (int i = 0; i < kJobs; ++i) {
        jobs.Run([&runs, i] { runs[static_cast<std::size_t>(i)].fetch_add(1, std::memory_order_relaxed); },
                 static_cast<JobPriority>(i % 3 == 0 ? 0 : 1), &counter);
    }
    jobs.Wait(counter);
    CHECK(counter.Done());
    int wrong = 0;
    for (const auto& r : runs) wrong += r.load() != 1 ? 1 : 0;
    CHECK(wrong == 0);
}

TEST_CASE("JobSystem: jobs that spawn jobs, and nested waits") {
    JobSystem jobs(4);
    std::atomic<int> leaves{0};
    JobCounter outer;
    for (int i = 0; i < 64; ++i) {
        jobs.Run(
            [&jobs, &leaves] {
                // Each job fans out and waits for its children (Frame jobs: allowed to wait on).
                JobCounter inner;
                for (int k = 0; k < 64; ++k) {
                    jobs.Run([&leaves] { leaves.fetch_add(1, std::memory_order_relaxed); }, JobPriority::Frame, &inner);
                }
                jobs.Wait(inner);
            },
            JobPriority::Frame, &outer);
    }
    jobs.Wait(outer);
    CHECK(leaves.load() == 64 * 64);
}

TEST_CASE("JobSystem: ParallelFor covers the range once") {
    JobSystem jobs(3);
    for (std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{1000}, std::size_t{123'457}}) {
        std::vector<std::uint8_t> hit(count, 0);
        std::atomic<int> overlap{0};
        jobs.ParallelFor(count, 64, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                if (hit[i] != 0) overlap.fetch_add(1);
                hit[i] = 1;
            }
        });
        CHECK(overlap.load() == 0);
        CHECK(std::accumulate(hit.begin(), hit.end(), std::size_t{0}) == count);
    }
}

TEST_CASE("JobSystem: work from many threads at once") {
    JobSystem jobs(4);
    std::atomic<int> total{0};
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t) {
        producers.emplace_back([&jobs, &total] {
            JobCounter counter;
            for (int i = 0; i < 10'000; ++i) {
                jobs.Run([&total] { total.fetch_add(1, std::memory_order_relaxed); }, JobPriority::Frame, &counter);
            }
            jobs.Wait(counter);
        });
    }
    for (auto& p : producers) p.join();
    CHECK(total.load() == 40'000);
}

TEST_CASE("JobSystem: streaming jobs finish and can be polled") {
    JobSystem jobs(2);
    JobCounter counter;
    std::atomic<int> done{0};
    for (int i = 0; i < 100; ++i) {
        jobs.Run([&done] { done.fetch_add(1); }, JobPriority::Streaming, &counter);
    }
    // The frame never waits on streaming work; it polls.
    while (!counter.Done()) std::this_thread::yield();
    CHECK(done.load() == 100);
}

TEST_CASE("JobSystem: destruction runs what is queued") {
    std::atomic<int> ran{0};
    {
        JobSystem jobs(2);
        for (int i = 0; i < 1000; ++i) jobs.Run([&ran] { ran.fetch_add(1); }, JobPriority::Normal);
    }
    CHECK(ran.load() == 1000);
}

TEST_CASE("JobSystem: the first one is the process-wide instance") {
    CHECK_FALSE(JobSystem::Exists());
    {
        JobSystem jobs(2);
        CHECK(JobSystem::Exists());
        CHECK(&JobSystem::Get() == &jobs);
        CHECK_FALSE(jobs.OnWorkerThread());
        // A Normal job runs on a worker (Wait() would run a Frame job on this thread itself).
        std::atomic<bool> onWorker{false};
        JobCounter counter;
        jobs.Run([&] { onWorker = jobs.OnWorkerThread(); }, JobPriority::Normal, &counter);
        while (!counter.Done()) std::this_thread::yield();
        CHECK(onWorker.load());
    }
    CHECK_FALSE(JobSystem::Exists());
}
