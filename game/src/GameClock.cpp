#include "sr/game/GameClock.h"

#include <cmath>

#include "sr/assets/Environment.h"

namespace sr::game {

void GameClock::Advance(double realSeconds) {
    if (realSeconds <= 0.0) return;
    hours_ += realSeconds * assets::kGameSecondsPerRealSecond / 3600.0;
    Normalise();
}

void GameClock::Normalise() {
    if (hours_ >= 24.0 || hours_ < 0.0) {
        const double days = std::floor(hours_ / 24.0);
        hours_ -= days * 24.0;
        day_ += static_cast<int>(days);
    }
}

float GameClock::TimeOfDay() const { return assets::EnvironmentTimeFromClock(hours_); }

} // namespace sr::game
