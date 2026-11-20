#pragma once

// A fixed set of worker threads that run submitted jobs in order. Submit returns a
// std::future for the job's result; dropping the future does not wait for the job.
// Jobs must only touch thread-safe state (the file system, their own data).

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace sr::core {

class ThreadPool {
public:
    // `threads` 0: one less than the hardware threads (at least one).
    explicit ThreadPool(std::size_t threads = 0);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <typename Fn>
    auto Submit(Fn&& fn) -> std::future<std::invoke_result_t<Fn>> {
        using Result = std::invoke_result_t<Fn>;
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(fn));
        std::future<Result> future = task->get_future();
        {
            std::lock_guard lock(mutex_);
            jobs_.emplace_back([task] { (*task)(); });
        }
        wake_.notify_one();
        return future;
    }
    std::size_t size() const { return workers_.size(); }
    // Jobs waiting to start.
    std::size_t pending() const;

    // One pool for the whole program.
    static ThreadPool& Shared();

private:
    void Run();

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> jobs_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
};

// A finished future (true) or one still running (false).
template <typename T>
bool Ready(const std::future<T>& future) {
    return future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

} // namespace sr::core
