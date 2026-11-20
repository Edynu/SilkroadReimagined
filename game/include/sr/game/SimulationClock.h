#pragma once

// Fixed-step simulation time. Real frame time accumulates and is
// spent in steps of 1 / rate seconds; rendering interpolates between the last two simulated
// states with Alpha(). The rate is configurable (default 30 steps a second) and independent of
// both the frame rate and the server's tick.
//
//   const int steps = clock.Advance(frameSeconds);
//   for (int i = 0; i < steps; ++i) world.Step(clock.StepSeconds());
//   presenter.Draw(world, clock.Alpha());

#include <cstdint>

namespace sr::game {

class SimulationClock {
public:
    static constexpr double kDefaultRate = 30.0;

    // `maxStepsPerAdvance` bounds the catch-up after a long frame (a hitch or a breakpoint):
    // time beyond it is dropped rather than simulated in a burst.
    explicit SimulationClock(double stepsPerSecond = kDefaultRate, int maxStepsPerAdvance = 8);

    // Changes the rate; the fraction of a step already accumulated is kept as a fraction.
    void SetRate(double stepsPerSecond);
    double Rate() const { return rate_; }
    double StepSeconds() const { return step_; }

    // Adds a frame's real time (negative counts as 0) and returns the steps to run now.
    int Advance(double frameSeconds);
    // Gives back steps Advance() granted that could not run yet (their server tick has not
    // arrived): they stay owed and run in a later Advance(), so nothing is lost while waiting.
    void Return(int steps);

    // How far the current moment is between the last step and the next, in [0, 1) (1 while
    // steps are owed).
    double Alpha() const { return accumulator_ >= step_ ? 1.0 : accumulator_ / step_; }
    // Steps run since the start, simulated time, and steps dropped by the catch-up bound.
    std::uint64_t Steps() const { return steps_; }
    double Time() const { return static_cast<double>(steps_) * step_; }
    std::uint64_t DroppedSteps() const { return dropped_; }

private:
    double rate_ = kDefaultRate;
    double step_ = 1.0 / kDefaultRate;
    int maxSteps_ = 8;
    double accumulator_ = 0.0;
    std::uint64_t steps_ = 0;
    std::uint64_t dropped_ = 0;
};

} // namespace sr::game
