#pragma once

// The weather: a kind (clear, rain, snow) and an amount
// (0..255). In v1.150 no packet carries it: a server event (0x3347, event 1) turns on snow at 20, and
// developer chat commands set any kind and amount. What the drawing uses is the effective amount,
// which moves linearly from where it is to a new amount over 5 seconds.

#include <cstdint>

namespace sr::game {

enum class WeatherKind : std::uint8_t { Clear = 1, Rain = 2, Snow = 3 };

class Weather {
public:
    static constexpr double kRampSeconds = 5.0;

    // Clear forces the amount to 0 (as the original's option setter does).
    void Set(WeatherKind kind, int amount);
    void Advance(double seconds);

    WeatherKind Kind() const { return kind_; }
    int Amount() const { return target_; }        // the amount set
    int Effective() const { return effective_; } // the amount the drawing uses now
    // Rain or snow: the environment greys and darkens (whatever the amount).
    bool Falling() const { return kind_ != WeatherKind::Clear; }

private:
    WeatherKind kind_ = WeatherKind::Clear;
    int target_ = 0, effective_ = 0;
    int start_ = 0;
    double progress_ = 1.0; // 0..1 through the ramp
};

// --- The server's weather: a climate's (the original shard manager's, 1.188) -------------------------------------------

// One row of the original's _RefClimate; every region names one (_RefRegion.Climate).
struct ClimateRow {
    std::int32_t id = 0;
    std::uint8_t initialWeather = 1; // a WeatherKind
    std::uint8_t initialAmount = 0;  // 0: a random 50..200 when it starts
    bool changes = false;            // ChangeWeather: 0 keeps the initial weather for good
    std::uint8_t division = 1;       // rolls a day: one every 3.5 hours / division (1..60)
    std::int32_t durationMs = 0, durationVarianceMs = 0;
    std::uint8_t snowfall = 0, snowfallVariance = 0, probSnow = 0;
    std::uint8_t rainfall = 0, rainfallVariance = 0, probRain = 0;
};

// A climate's weather as the original's shard runs it. At the start the initial weather. A roll comes every 12,600,000 ms / division (once the last spell is over): a number 1..100
// gives snow up to ProbSnow, rain up to ProbSnow + ProbRain, else clear; a fall's amount is its base plus 0..its
// variance (at most 255), clear's 150..200; the spell lasts Duration + 0..DurationVariance ms (30 s to 30 min).
// Once reached and over, the weather goes back to the initial one. The amount moves one step every 3 s, a step a
// tenth of the way (10..510 / 10): a new kind's fall comes after the old one has thinned to nothing.
//
// Ours: the rolls fall at whole multiples of the period on the wall clock and their numbers come from (seed,
// climate, roll), so every game server of a shard runs the same weather without a word between them.
class ClimateWeather {
public:
    static constexpr std::int64_t kDayMs = 12'600'000; // the original's 3.5 hours a "day" of rolls
    static constexpr std::int64_t kStepMs = 3000;

    void Start(const ClimateRow& row, std::int64_t nowMs, std::uint64_t seed);
    // Rolls and steps due by `nowMs` (ms on a clock every server shares); whether what it sends changed.
    bool Advance(std::int64_t nowMs);

    WeatherKind Kind() const { return WeatherKind(kind_); }
    std::uint8_t Amount() const { return amount_; }
    // As the original sends it: the kind, then the amount.
    std::uint16_t Packed() const { return std::uint16_t(kind_ | (amount_ << 8)); }
    // Forces a weather (a game master's): reached at once, kept for `ms`, then back to the climate's own.
    void Force(WeatherKind kind, std::uint8_t amount, std::int64_t nowMs, std::int64_t ms);

private:
    std::uint32_t Random(std::int64_t roll, std::uint32_t salt) const;
    void Roll(std::int64_t roll, std::int64_t nowMs);
    void PlanSteps();

    ClimateRow row_;
    std::uint64_t seed_ = 0;
    std::int64_t periodMs_ = kDayMs;
    std::int64_t lastRoll_ = 0;
    std::uint8_t kind_ = 1, amount_ = 0;             // what is sent
    std::uint8_t targetKind_ = 1, targetAmount_ = 0; // where it goes
    std::uint8_t step_ = 1;
    std::int64_t spellStart_ = 0, spellMs_ = 0, lastStep_ = 0;
};

} // namespace sr::game
