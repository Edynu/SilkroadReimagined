#include "sr/game/Weather.h"

#include <algorithm>
#include <cstdlib>

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

// --- ClimateWeather -----------------------------------------------------------------------------------------------------

namespace {
std::uint64_t Mix(std::uint64_t x) { // splitmix64's finaliser
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
} // namespace

std::uint32_t ClimateWeather::Random(std::int64_t roll, std::uint32_t salt) const {
    return std::uint32_t(Mix(seed_ ^ Mix(std::uint64_t(std::uint32_t(row_.id)) << 32 | salt) ^ Mix(std::uint64_t(roll))) >> 1);
}

void ClimateWeather::Start(const ClimateRow& row, std::int64_t nowMs, std::uint64_t seed) {
    row_ = row;
    seed_ = seed;
    // The original's clamps: 1..60 rolls a "day", a spell of 30 s to 30 minutes.
    row_.division = std::uint8_t(std::clamp<int>(row_.division, 1, 60));
    row_.durationMs = std::clamp<std::int32_t>(row_.durationMs, 30'000, 1'800'000);
    row_.durationVarianceMs = std::max<std::int32_t>(row_.durationVarianceMs, 0);
    periodMs_ = kDayMs / row_.division;
    lastRoll_ = nowMs / periodMs_;
    kind_ = targetKind_ = std::uint8_t(std::clamp<int>(row_.initialWeather, 1, 3));
    // No initial amount: a random 50..200 (the original's when its column is 0).
    if (row_.initialAmount == 0) row_.initialAmount = std::uint8_t(50 + Random(0, 1) % 151);
    amount_ = targetAmount_ = row_.initialAmount;
    spellStart_ = lastStep_ = nowMs;
    spellMs_ = 0;
    step_ = 1;
}

void ClimateWeather::PlanSteps() {
    // A tenth of the way: the amounts' difference within a kind, else the old one's down and the new one's up.
    const int way = kind_ == targetKind_ ? std::abs(int(amount_) - int(targetAmount_)) : int(amount_) + int(targetAmount_);
    step_ = std::uint8_t(std::clamp(way, 10, 510) / 10);
}

void ClimateWeather::Roll(std::int64_t roll, std::int64_t nowMs) {
    const int r = int(Random(roll, 2) % 100) + 1;
    if (r <= row_.probSnow) {
        targetKind_ = std::uint8_t(WeatherKind::Snow);
        targetAmount_ = std::uint8_t(std::min<int>(255, row_.snowfall + int(Random(roll, 3) % (row_.snowfallVariance + 1u))));
    } else if (r <= row_.probSnow + row_.probRain) {
        targetKind_ = std::uint8_t(WeatherKind::Rain);
        targetAmount_ = std::uint8_t(std::min<int>(255, row_.rainfall + int(Random(roll, 3) % (row_.rainfallVariance + 1u))));
    } else {
        targetKind_ = std::uint8_t(WeatherKind::Clear);
        targetAmount_ = std::uint8_t(150 + Random(roll, 3) % 51);
    }
    spellStart_ = nowMs;
    spellMs_ = row_.durationMs + std::int64_t(Random(roll, 4) % std::uint32_t(row_.durationVarianceMs + 1));
    PlanSteps();
}

void ClimateWeather::Force(WeatherKind kind, std::uint8_t amount, std::int64_t nowMs, std::int64_t ms) {
    kind_ = targetKind_ = std::uint8_t(kind);
    amount_ = targetAmount_ = kind == WeatherKind::Clear ? 0 : amount;
    spellStart_ = nowMs;
    spellMs_ = std::max<std::int64_t>(ms, 1);
    lastStep_ = nowMs;
}

bool ClimateWeather::Advance(std::int64_t nowMs) {
    const std::uint16_t before = Packed();
    // A roll once the period's mark has passed, while no spell lasts (a roll during one is skipped, as the original's).
    if (row_.changes) {
        if (const std::int64_t roll = nowMs / periodMs_; roll > lastRoll_) {
            lastRoll_ = roll;
            if (nowMs - spellStart_ >= spellMs_) Roll(roll, nowMs);
        }
    }
    // A spell reached and over: back to the climate's own weather.
    if (kind_ == targetKind_ && amount_ == targetAmount_ && spellMs_ != 0 && nowMs - spellStart_ > spellMs_) {
        targetKind_ = std::uint8_t(std::clamp<int>(row_.initialWeather, 1, 3));
        targetAmount_ = row_.initialAmount;
        spellMs_ = 0;
        PlanSteps();
    }
    // One step every 3 s towards the target.
    if ((kind_ != targetKind_ || amount_ != targetAmount_) && nowMs - lastStep_ >= kStepMs) {
        lastStep_ = nowMs;
        if (kind_ != targetKind_) {
            amount_ = amount_ > step_ ? std::uint8_t(amount_ - step_) : 0;
            if (amount_ == 0) kind_ = targetKind_;
        } else if (amount_ < targetAmount_) {
            amount_ = std::uint8_t(std::min<int>(targetAmount_, amount_ + step_));
        } else {
            amount_ = std::uint8_t(std::max<int>(targetAmount_, amount_ - step_));
        }
    }
    return Packed() != before;
}

} // namespace sr::game
