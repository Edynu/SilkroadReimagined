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

} // namespace sr::game
