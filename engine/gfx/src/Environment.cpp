#include "sr/gfx/Environment.h"

#include <algorithm>
#include <cmath>

namespace sr::gfx {

namespace {

using assets::EnvironmentCurve;
constexpr float kTwoPi = 6.28318531f;
constexpr float kSkyRadius = 20000.0f;

std::size_t Index(EnvironmentCurve c) { return static_cast<std::size_t>(c); }

glm::vec3 Clamp01(const glm::vec3& v) { return glm::clamp(v, 0.0f, 1.0f); }

} // namespace

void EnvironmentState::Update(const assets::EnvironmentEntry& entry, float time, float dt, int moonDay, const EnvironmentOptions& options) {
    options_ = options;
    time_ = time;

    // The targets: each curve sampled and transformed as the original's update does.
    std::array<glm::vec3, kCurves> target{};
    for (int c = 0; c < kCurves; ++c) {
        const auto curve = static_cast<EnvironmentCurve>(c);
        const auto& keys = entry.Curve(curve);
        if (assets::IsColourCurve(curve)) {
            target[static_cast<std::size_t>(c)] = Clamp01(glm::vec3(assets::SampleEnvironment(keys, time, 0, 0.0f),
                                                                    assets::SampleEnvironment(keys, time, 1, 0.0f),
                                                                    assets::SampleEnvironment(keys, time, 2, 0.0f)));
        } else {
            target[static_cast<std::size_t>(c)] = glm::vec3(assets::SampleEnvironment(keys, time, 0, 0.0f), 0.0f, 0.0f);
        }
    }
    float& glow = target[Index(EnvironmentCurve::GlowRadius)].x;
    glow = (glow + 1.0f) * 0.5f;
    float& gradient = target[Index(EnvironmentCurve::Gradient)].x;
    gradient = 1.0f - std::clamp(gradient, -1.0f, 1.0f);
    float& clouds = target[Index(EnvironmentCurve::CloudAlpha)].x;
    clouds = std::clamp((clouds + 1.0f) * 0.5f, 0.0f, 1.0f);
    float& stars = target[Index(EnvironmentCurve::Stars)].x;
    stars = std::clamp(stars, -1.0f, 1.0f);
    if (options.weather) {
        // The original's override under rain or snow (whatever the amount): the targets change and
        // the values approach them like any other change.
        const auto grey = [](const glm::vec3& c) { return glm::vec3((c.r + c.g + c.b) / 3.0f); };
        glm::vec3& zenith = target[Index(EnvironmentCurve::Zenith)];
        glm::vec3& fog = target[Index(EnvironmentCurve::Fog)];
        zenith = grey(zenith);
        fog = grey(fog);
        target[Index(EnvironmentCurve::SunGlow)] = zenith;
        target[Index(EnvironmentCurve::Horizon)] = fog;
        target[Index(EnvironmentCurve::Diffuse)] = Clamp01(target[Index(EnvironmentCurve::Diffuse)] - 0.2f);
        target[Index(EnvironmentCurve::TerrainAmbient)] = Clamp01(target[Index(EnvironmentCurve::TerrainAmbient)] - 0.2f);
        gradient = 1.0f;
        target[Index(EnvironmentCurve::FogStart)].x *= 0.5f;
        stars = -1.0f;
        clouds = 0.0f;
        glow = 0.0f; // the sun's glow reaches nowhere
    }

    // Approach the targets (the first update snaps).
    const float step = valid_ ? std::min(1.0f, 0.5f * std::max(dt, 0.0f)) : 1.0f;
    for (std::size_t c = 0; c < values_.size(); ++c) values_[c] += (target[c] - values_[c]) * step;
    valid_ = true;

    const auto value = [&](EnvironmentCurve c) { return values_[Index(c)]; };
    SkyState& sky = sky_;
    sky.active = true;
    sky.zenith = value(EnvironmentCurve::Zenith);
    sky.horizon = value(EnvironmentCurve::Horizon);
    sky.sunGlow = value(EnvironmentCurve::SunGlow);
    sky.sunColor = value(EnvironmentCurve::Sun);
    sky.band = value(EnvironmentCurve::Fog);
    sky.glowRadius = std::max(1.0f, 80000.0f * value(EnvironmentCurve::GlowRadius).x);
    sky.gradient = value(EnvironmentCurve::Gradient).x;
    sky.cloudAlpha = value(EnvironmentCurve::CloudAlpha).x;
    sky.stars = value(EnvironmentCurve::Stars).x;

    // The sun turns about Z: up at +X at 0.25, overhead at noon, down at -X at 0.75.
    const float sunAngle = kTwoPi * (time - 0.25f);
    const float tc = std::cos(options.sunTilt), ts = std::sin(options.sunTilt);
    sky.sunPosition = kSkyRadius * glm::vec3(std::cos(sunAngle), std::sin(sunAngle) * tc, std::sin(sunAngle) * ts);
    sky.sunVisible = time >= 0.25f && time <= 0.875f && !options.weather;
    // The moon in the same plane at 1.2 times its pace: up about 0.79, overhead at midnight.
    const float m = time - 0.75f - std::floor(time - 0.75f);
    const float moonAngle = kTwoPi * ((m - 0.25f) * 1.2f + 0.25f);
    sky.moonPosition = kSkyRadius * glm::vec3(std::cos(moonAngle), std::sin(moonAngle) * tc, std::sin(moonAngle) * ts);
    sky.moonVisible = moonDay >= 0 && moonDay < 29 && (time < 0.5f || time > 0.75f) && !options.weather;
    sky.moonPhase = std::clamp(moonDay, 0, 28);

    // The bright stars' twinkle: 10 times a second each batch has 1 chance in 10 to change.
    twinkleClock_ += std::max(dt, 0.0f);
    while (twinkleClock_ >= 0.1f) {
        twinkleClock_ -= 0.1f;
        for (float& b : sky.twinkle) {
            random_ = random_ * 1664525u + 1013904223u;
            if (((random_ >> 16) % 10) == 0 || b == 0.0f) {
                random_ = random_ * 1664525u + 1013904223u;
                b = static_cast<float>(128 + (random_ >> 16) % 128) / 255.0f;
            }
        }
    }
}

SceneLighting EnvironmentState::Lighting() const {
    SceneLighting l;
    if (!valid_) return l;
    const auto value = [&](EnvironmentCurve c) { return values_[Index(c)]; };
    l.terrainLight = glm::vec3(5.0f, 1.0f, 0.0f); // the light travels along (-5, -1, 0)
    l.objectLight = glm::vec3(1.0f, 1.0f, 0.0f);  // and (-1, -1, 0) for objects
    l.diffuse = value(EnvironmentCurve::Diffuse) * (options_.glow ? 0.8f : 0.6f) * (options_.underWater ? 0.5f : 1.0f);
    l.sunDiffuse = glm::clamp(value(EnvironmentCurve::Diffuse), glm::vec3(0.0f), glm::vec3(1.0f));
    l.terrainAmbient = value(EnvironmentCurve::TerrainAmbient);
    l.objectAmbient = value(EnvironmentCurve::ObjectAmbient);
    l.lift = value(EnvironmentCurve::Lift);
    l.fog = true;
    l.fogColor = value(EnvironmentCurve::Fog);
    l.terrainFogColor = glm::sqrt(l.fogColor);
    const float range = options_.fogRange;
    l.fogEnd = value(EnvironmentCurve::FogEnd).x * range;
    l.fogStart = options_.underWater ? -200.0f : value(EnvironmentCurve::FogStart).x * range;
    if (options_.underWater) l.fogEnd /= 10.0f;
    l.waterColor = value(EnvironmentCurve::Water);
    l.timeOfDay = time_;
    return l;
}

} // namespace sr::gfx
