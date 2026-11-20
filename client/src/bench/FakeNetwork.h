#pragma once

// Stands in for the network thread until C6: a thread that produces the
// crowd's messages tick by tick, ahead of the simulation, the way a server's stream arrives. The
// game thread takes what has arrived once a frame; message content depends only on the tick, so
// a run is repeatable whatever the timing (except interactive bursts).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "sr/game/SyntheticCrowd.h"

namespace sr::client {

class FakeNetwork {
public:
    static constexpr std::uint64_t kLookahead = 15; // ticks produced ahead of the simulation

    explicit FakeNetwork(const game::CrowdMessageSource::Settings& settings);
    ~FakeNetwork();
    FakeNetwork(const FakeNetwork&) = delete;
    FakeNetwork& operator=(const FakeNetwork&) = delete;

    // Game thread: the simulation has stepped up to `tick` (the producer stays kLookahead ahead).
    void SetConsumedTick(std::uint64_t tick);
    // Game thread: appends every message received so far.
    void Take(std::vector<game::CrowdMessage>& out);
    void RequestBurst() { burst_ = true; }

private:
    void Run();

    game::CrowdMessageSource source_;
    std::vector<game::CrowdMessage> inbox_;
    std::uint64_t consumed_ = 0, produced_ = 0;
    bool stopping_ = false;
    std::atomic<bool> burst_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
};

} // namespace sr::client
