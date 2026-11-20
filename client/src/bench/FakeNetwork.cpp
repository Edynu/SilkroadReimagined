#include "bench/FakeNetwork.h"

#include "sr/core/Trace.h"

namespace sr::client {

FakeNetwork::FakeNetwork(const game::CrowdMessageSource::Settings& settings) : source_(settings) {
    thread_ = std::thread([this] { Run(); });
}

FakeNetwork::~FakeNetwork() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void FakeNetwork::SetConsumedTick(std::uint64_t tick) {
    {
        std::lock_guard lock(mutex_);
        if (tick <= consumed_) return;
        consumed_ = tick;
    }
    wake_.notify_all();
}

void FakeNetwork::Take(std::vector<game::CrowdMessage>& out) {
    std::lock_guard lock(mutex_);
    out.insert(out.end(), inbox_.begin(), inbox_.end());
    inbox_.clear();
}

void FakeNetwork::Run() {
    core::SetThreadName("network (fake)");
    std::vector<game::CrowdMessage> batch;
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return stopping_ || produced_ < consumed_ + kLookahead; });
        if (stopping_) return;
        const std::uint64_t tick = ++produced_;
        lock.unlock();
        {
            SR_ZONE("network: produce tick");
            if (burst_.exchange(false)) source_.RequestBurst();
            batch.clear();
            source_.Generate(tick, batch);
        }
        lock.lock();
        inbox_.insert(inbox_.end(), batch.begin(), batch.end());
    }
}

} // namespace sr::client
