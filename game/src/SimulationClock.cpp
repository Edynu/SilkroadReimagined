#include "sr/game/SimulationClock.h"

#include <algorithm>
#include <cmath>

namespace sr::game {

namespace {

double SaneRate(double stepsPerSecond) {
    // Anything from one step a second to 1000 is accepted; nonsense falls back to the default.
    if (!std::isfinite(stepsPerSecond) || stepsPerSecond < 1.0) return SimulationClock::kDefaultRate;
    return std::min(stepsPerSecond, 1000.0);
}

} // namespace

SimulationClock::SimulationClock(double stepsPerSecond, int maxStepsPerAdvance)
    : maxSteps_(std::max(1, maxStepsPerAdvance)) {
    rate_ = SaneRate(stepsPerSecond);
    step_ = 1.0 / rate_;
}

void SimulationClock::SetRate(double stepsPerSecond) {
    const double fraction = accumulator_ / step_;
    rate_ = SaneRate(stepsPerSecond);
    step_ = 1.0 / rate_;
    accumulator_ = fraction * step_;
}

int SimulationClock::Advance(double frameSeconds) {
    if (!(frameSeconds > 0.0)) return 0; // negative, zero or NaN
    accumulator_ += frameSeconds;
    // A small epsilon so 1/30 s frames give exactly one step despite rounding.
    const double epsilon = step_ * 1e-9;
    std::uint64_t due = static_cast<std::uint64_t>((accumulator_ + epsilon) / step_);
    accumulator_ = std::max(0.0, accumulator_ - static_cast<double>(due) * step_);
    if (due > static_cast<std::uint64_t>(maxSteps_)) {
        dropped_ += due - static_cast<std::uint64_t>(maxSteps_);
        due = static_cast<std::uint64_t>(maxSteps_);
    }
    steps_ += due;
    return static_cast<int>(due);
}

void SimulationClock::Return(int steps) {
    if (steps <= 0) return;
    const auto given = static_cast<std::uint64_t>(steps) < steps_ ? static_cast<std::uint64_t>(steps) : steps_;
    steps_ -= given;
    accumulator_ += static_cast<double>(given) * step_;
}

} // namespace sr::game
