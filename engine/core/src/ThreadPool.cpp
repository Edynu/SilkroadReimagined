#include "sr/core/ThreadPool.h"

#include <algorithm>

namespace sr::core {

ThreadPool::ThreadPool(std::size_t threads) {
    if (threads == 0) {
        const unsigned hardware = std::thread::hardware_concurrency();
        threads = hardware > 1 ? hardware - 1 : 1;
    }
    for (std::size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { Run(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        jobs_.clear(); // jobs not started never run (their futures report broken promises)
    }
    wake_.notify_all();
    for (std::thread& t : workers_) t.join();
}

std::size_t ThreadPool::pending() const {
    std::lock_guard lock(mutex_);
    return jobs_.size();
}

ThreadPool& ThreadPool::Shared() {
    static ThreadPool pool;
    return pool;
}

void ThreadPool::Run() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
}

} // namespace sr::core
