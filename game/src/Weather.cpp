#include "sr/game/Weather.h"

#include <algorithm>

namespace sr::game {

void Weather::Set(WeatherKind kind, int amount) {
    kind_ = kind;
    const int target = kind == WeatherKind::Clear ? 0 : std::clamp(amount, 0, 255);
    if (target == target_) return;
    // A new ramp from wherever the effective amount is now.
    target_ = target;
    start_ = effective_;
    progress_ = 0.0;
}

void Weather::Advance(double seconds) {
    if (progress_ >= 1.0 || seconds <= 0.0) return;
    progress_ = std::min(1.0, progress_ + seconds / kRampSeconds);
    effective_ = static_cast<int>(start_ + progress_ * (target_ - start_));
}

} // namespace sr::game
