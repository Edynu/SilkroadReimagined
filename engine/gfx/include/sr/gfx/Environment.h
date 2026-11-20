#pragma once

// The environment at a moment ("Time of day, sky and fog (C3e)"): environment.ifo's
// 16 curves sampled at the time of day for the entry of the block under the camera, turned into
// what the passes read the way the original does, and approached over about 2 s (every value
// moves by min(1, 0.5 x dt) toward its target a frame), so a change of time or entry fades.
// Game thread: the result goes into the snapshot.

#include <array>
#include <cstdint>

#include <glm/glm.hpp>

#include "sr/assets/Environment.h"
#include "sr/gfx/FrameConstants.h"

namespace sr::gfx {

// What the sky pass draws (SkyPass), from the environment.
struct SkyState {
    bool active = false;
    glm::vec3 zenith{0.35f, 0.55f, 0.85f};
    glm::vec3 horizon{0.7f, 0.8f, 0.9f};
    glm::vec3 sunGlow{1.0f};       // the dome around the sun, and the clouds' colour
    glm::vec3 sunColor{1.0f};
    glm::vec3 band{0.7f, 0.8f, 0.9f}; // the horizon band below the horizon (the fog colour)
    float glowRadius = 1.0f;       // W: the glow fades out over this distance from the sun
    float gradient = 1.0f;         // g: the dome's horizon-to-zenith gradient
    float cloudAlpha = 0.0f;
    float stars = -1.0f;           // drawn when > 0
    glm::vec3 sunPosition{0, 20000, 0}; // on the dome (radius 20000), camera-centred
    glm::vec3 moonPosition{0, -20000, 0};
    bool sunVisible = false, moonVisible = false;
    int moonPhase = 0;             // 0..28: sun\moon01 .. moon29
    std::array<float, 10> twinkle{}; // brightness 0..1 of the 10 batches of bright stars
};

struct EnvironmentOptions {
    float fogRange = 2500.0f; // R = min(0.8 x view distance, 2500)
    bool glow = false;        // the light's diffuse is the curve x 0.8 instead of 0.6
    bool underWater = false;  // the camera is below the water: fog from -200 to end / 10, light halved
    // Rain or snow (C4c): the zenith and fog go grey, the light and the terrain
    // darken, the fog starts nearer; no sun, moon, stars, clouds or sun glow.
    bool weather = false;
    // Ours (the Modern look's): the sun's and moon's path tilted by this many radians from passing
    // overhead, so their noon stands to the south and shadows turn through the day; 0 the original's.
    float sunTilt = 0.0f;
};

class EnvironmentState {
public:
    // Samples `entry` at `time` (0..1) and moves toward it; the first call snaps. `moonDay` is the
    // game clock's day count (no moon after day 28, as the original).
    void Update(const assets::EnvironmentEntry& entry, float time, float dt, int moonDay, const EnvironmentOptions& options);
    bool Valid() const { return valid_; }

    SceneLighting Lighting() const;
    const SkyState& Sky() const { return sky_; }

private:
    static constexpr int kCurves = static_cast<int>(assets::EnvironmentCurve::Count);
    std::array<glm::vec3, kCurves> values_{}; // transformed, smoothed (scalars in .x)
    EnvironmentOptions options_;
    float time_ = 0.5f;
    SkyState sky_;
    float twinkleClock_ = 0.0f;
    std::uint32_t random_ = 0x2545F491u;
    bool valid_ = false;
};

} // namespace sr::gfx
