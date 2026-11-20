#pragma once

// The original's rain and snow as particles (C4c), on the game thread around
// the eye. Each particle is a cluster of 9 drops or flakes on a 50-unit grid (expanded by the
// weather pass):
// - rain: 2 x amount clusters a second, 50..150 above and within 60 of the eye, falling
//   y0 - 98 t - 4.9 t^2; a cluster spawned over water ends at the surface in one ripple (1.2 s),
//   others below the eye - 500. Lightning about every 50 s: one or two flashes;
// - snow: 0.5 x amount clusters a second within 100 of the eye, 1 in 4 of the second kind; falling
//   12 (or 10) units a second, drifting 2 units a second each way with signs that jitter in
//   alternate 0.27 s windows, wrapped to stay within 100 of the eye, ending below the eye - 500.
// Rates are the original's per second (its lightning chance, per frame, becomes 0.02 a second).

#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "sr/game/Weather.h"
#include "sr/gfx/WeatherPass.h"

namespace sr::client {

class WeatherEffect {
public:
    // The water surface under (x, z), when the 320-unit block there has water.
    using WaterAt = std::function<bool(float x, float z, float& height)>;

    void Update(game::WeatherKind kind, int amount, const glm::vec3& eye, float dt, const WaterAt& water);
    void Fill(gfx::WeatherView& view) const;
    // A lightning strike now (tests and captures; rain brings its own).
    void Strike(int pattern);
    void Clear();

    struct Stats {
        std::size_t rain = 0, snow = 0, ripples = 0;
        std::uint64_t strikes = 0;
    };
    Stats GetStats() const { return {rain_.size(), snow_.size(), ripples_.size(), strikes_}; }

private:
    std::uint32_t Rand(); // 15 bits, as the C runtime's rand()

    struct Rain {
        float x = 0, y0 = 0, z = 0, age = 0;
        float stop = -1e38f; // the water surface under its start, if any
        float y = 0;
    };
    struct Flake {
        float x = 0, y = 0, z = 0;
        std::uint8_t type = 0;
        float dx = 2.0f, dz = 2.0f; // drift, units a second
        float clock = 0;            // its jitter windows
    };
    struct Ripple {
        float x = 0, y = 0, z = 0, age = 0;
    };
    std::vector<Rain> rain_;
    std::vector<Flake> snow_;
    std::vector<Ripple> ripples_;
    float owed_ = 0.0f;     // clusters owed to the spawn rate
    double jitterClock_ = 0; // 60 Hz ticks for the flakes' sign changes
    std::uint32_t random_ = 0x5EED1234u;
    std::vector<float> flashesDue_; // seconds until each scheduled flash
    float flashAge_ = -1.0f;        // seconds into the current flash; < 0 none
    std::uint64_t strikes_ = 0;
};

} // namespace sr::client
