#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "sr/core/Jobs.h"
#include "sr/game/PhaseCommands.h"
#include "sr/game/SyntheticCrowd.h"

using sr::core::JobSystem;
using sr::game::CrowdMessage;
using sr::game::CrowdMessageSource;
using sr::game::SyntheticCrowd;

namespace {

// Runs `steps` ticks of the crowd with `workers` worker threads and returns its state hash.
std::uint64_t RunCrowd(int workers, std::uint64_t steps, std::size_t* finalCount = nullptr) {
    JobSystem jobs(workers);
    CrowdMessageSource::Settings settings;
    settings.population = 1500;
    settings.burstEveryTicks = 40;
    settings.burstSize = 500;
    settings.burstLifeTicks = 25;
    CrowdMessageSource source(settings);
    SyntheticCrowd crowd;
    std::vector<CrowdMessage> messages;
    for (std::uint64_t tick = 1; tick <= steps; ++tick) {
        messages.clear();
        source.Generate(tick, messages);
        for (const CrowdMessage& m : messages) crowd.Apply(m);
        REQUIRE(crowd.CanStep());
        crowd.Step(jobs, 1.0 / 30.0);
    }
    if (finalCount != nullptr) *finalCount = crowd.Entities().size();
    return crowd.Hash();
}

} // namespace

TEST_CASE("PhaseCommands: merged by entity, each entity's in order, whatever thread added them") {
    JobSystem jobs(4);
    sr::game::PhaseCommands<int> commands;
    commands.Begin(jobs);
    jobs.ParallelFor(1000, 7, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const auto entity = static_cast<std::uint32_t>(999 - i);
            commands.Add(entity, 0);
            commands.Add(entity, 1);
        }
    });
    const auto& merged = commands.Merge();
    REQUIRE(merged.size() == 2000);
    for (std::size_t i = 0; i < merged.size(); ++i) {
        CHECK(merged[i].entity == i / 2);
        CHECK(merged[i].command == static_cast<int>(i % 2));
    }
}

TEST_CASE("SyntheticCrowd: the same messages give the same state with any number of threads") {
    std::size_t inlineCount = 0, threadedCount = 0;
    const std::uint64_t inlineHash = RunCrowd(0, 300, &inlineCount);  // --workers 0: everything inline
    const std::uint64_t threaded = RunCrowd(3, 300, &threadedCount);
    const std::uint64_t defaultWorkers = RunCrowd(JobSystem::kDefaultWorkers, 300);
    CHECK(inlineCount == threadedCount);
    CHECK(inlineCount > 1500); // the bursts came and went while the residents stayed
    CHECK(inlineHash == threaded);
    CHECK(inlineHash == defaultWorkers);
}

TEST_CASE("SyntheticCrowd: a step waits for its tick's messages") {
    SyntheticCrowd crowd;
    CHECK_FALSE(crowd.CanStep());
    crowd.Apply({CrowdMessage::Kind::Spawn, 1, 1, 10.0f, 10.0f});
    CHECK_FALSE(crowd.CanStep()); // tick 1 is not complete yet
    crowd.Apply({CrowdMessage::Kind::TickEnd, 0, 1, 0, 0});
    CHECK(crowd.CanStep());
    JobSystem jobs(0);
    crowd.Step(jobs, 1.0 / 30.0);
    CHECK_FALSE(crowd.CanStep());
    CHECK(crowd.Entities().size() == 1);
}
