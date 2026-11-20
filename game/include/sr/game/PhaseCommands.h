#pragma once

// Commands produced inside a parallel phase. A job may write only the
// entities it was given; anything else it wants changed becomes a command. Each thread appends to
// its own buffer (no locks, no false sharing); Merge() returns all of them ordered by entity id,
// and for one entity in the order its job added them, so the result does not depend on which
// thread ran which entity, or on how many threads there were.
//
//   commands.Begin(jobs);
//   jobs.ParallelFor(n, 64, [&](size_t b, size_t e) { ... commands.Add(entity.id, Arrived{}); });
//   for (const auto& c : commands.Merge()) Apply(c.entity, c.command);   // serial commit

#include <algorithm>
#include <cstdint>
#include <vector>

#include "sr/core/Jobs.h"

namespace sr::game {

template <typename Command>
class PhaseCommands {
public:
    struct Entry {
        std::uint32_t entity = 0;
        Command command{};
    };

    // Before the phase: one empty buffer per thread slot of `jobs`.
    void Begin(const core::JobSystem& jobs) {
        jobs_ = &jobs;
        if (buffers_.size() < jobs.SlotCount()) buffers_.resize(jobs.SlotCount());
        for (Slot& slot : buffers_) slot.entries.clear();
    }

    // Inside the phase, from the job that owns `entity`.
    void Add(std::uint32_t entity, Command command) {
        buffers_[jobs_->SlotIndex()].entries.push_back({entity, std::move(command)});
    }

    // After the phase (all jobs done): every command, by entity id, each entity's in its order.
    const std::vector<Entry>& Merge() {
        merged_.clear();
        for (Slot& slot : buffers_) merged_.insert(merged_.end(), slot.entries.begin(), slot.entries.end());
        // One entity's commands all sit in one buffer, in order: a stable sort keeps that order.
        std::stable_sort(merged_.begin(), merged_.end(), [](const Entry& a, const Entry& b) { return a.entity < b.entity; });
        return merged_;
    }

private:
    struct alignas(64) Slot { // a cache line each: threads append side by side
        std::vector<Entry> entries;
    };
    const core::JobSystem* jobs_ = nullptr;
    std::vector<Slot> buffers_;
    std::vector<Entry> merged_;
};

} // namespace sr::game
