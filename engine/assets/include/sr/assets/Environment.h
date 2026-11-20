#pragma once

// environment.ifo (JMXVENVI, version 1003): the original's day / night environments, from our RE
// ("Time of day, sky and fog (C3e)"). Each entry (named by the key terrain blocks
// carry as their environmentId) holds 16 curves over the time of day (0..1: 0 midnight, 0.25
// sunrise, 0.5 noon, 0.75 sunset).
//
//   char[12] "JMXVENVI1003", u16 entryCount, counted string (empty)
//   entryCount x {
//     u16 key, counted string name, counted string, counted string,
//     16 curves in EnvironmentCurve order, each u32 count then count keys:
//       colour: f32 r, g, b, t        scalar: f32 value, t
//   }
//   then the editor's folder tree of entry keys (not read here).
//
// The curves are sampled with the original's uniform Catmull-Rom spline through the keys (the
// end keys stand in for missing neighbours), clamped outside the keys, the lower key's value when
// two keys are 1e-4 apart or less.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sr::assets {

// What each curve feeds in the original.
enum class EnvironmentCurve : int {
    Sun = 0,         // colour of the sun disc
    Zenith,          // sky dome top
    Diffuse,         // the sun light's diffuse, x 0.6 (x 0.8 with glow on)
    ObjectAmbient,   // the light's ambient while objects draw
    SunGlow,         // the dome's glow around the sun, and the clouds' colour
    TerrainAmbient,  // the light's ambient while the terrain draws; the world material's emissive
    Lift,            // added to the terrain lightmap before it multiplies
    GlowRadius,      // scalar: the dome glow's reach, max(1, 80000 x (v + 1) / 2)
    Gradient,        // scalar: the dome's horizon-to-zenith gradient, 1 - clamp(v, -1, 1)
    Fog,             // fog colour (its square root while the near terrain draws), the horizon band
    FogStart,        // scalar: x the fog range
    FogEnd,          // scalar: x the fog range
    CloudAlpha,      // scalar: the clouds' alpha, (v + 1) / 2
    Horizon,         // sky dome horizon
    Water,           // the water's colour
    Stars,           // scalar: star visibility, clamp(v, -1, 1) (drawn when > 0)
    Count
};
constexpr bool IsColourCurve(EnvironmentCurve c) {
    return c != EnvironmentCurve::GlowRadius && c != EnvironmentCurve::Gradient && c != EnvironmentCurve::FogStart &&
           c != EnvironmentCurve::FogEnd && c != EnvironmentCurve::CloudAlpha && c != EnvironmentCurve::Stars;
}

struct EnvironmentKey {
    float t = 0.0f;
    std::array<float, 3> value{}; // a scalar curve uses value[0]
};

struct EnvironmentEntry {
    std::uint16_t key = 0;
    std::string name;
    std::array<std::vector<EnvironmentKey>, static_cast<std::size_t>(EnvironmentCurve::Count)> curves;
    const std::vector<EnvironmentKey>& Curve(EnvironmentCurve c) const { return curves[static_cast<std::size_t>(c)]; }
};

struct EnvironmentData {
    std::vector<EnvironmentEntry> entries;
    const EnvironmentEntry* Find(std::uint16_t key) const;
};

bool DecodeEnvironment(std::span<const std::byte> file, EnvironmentData& out, std::string& error);

// One channel of a curve at `time` (0..1), the original's way; `fallback` when it has no keys.
float SampleEnvironment(const std::vector<EnvironmentKey>& curve, float time, int channel, float fallback);

// The original's game clock: a game day lasts 30 real minutes' worth of 20 ms game
// seconds (1728 real seconds), starting at 00:00 when the client starts. Its hour maps to the
// environment's time unevenly: clock 04:00-20:00 is the environment's 06:00-18:00, the 8 night
// hours its 12 night hours.
constexpr double kGameSecondsPerRealSecond = 50.0; // 20 ms a game second
float EnvironmentTimeFromClock(double clockHours);

} // namespace sr::assets
