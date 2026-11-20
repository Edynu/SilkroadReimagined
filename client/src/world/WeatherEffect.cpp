#include "world/WeatherEffect.h"

#include <algorithm>
#include <cmath>

#include "sr/core/Trace.h"

namespace sr::client {

namespace {

constexpr float kBelow = 500.0f;          // particles end this far below the eye
constexpr float kRippleSeconds = 1.2f;
constexpr float kLightningPerSecond = 0.02f; // the original's 1 in 3000 a frame, at 60 frames a second
constexpr float kJitterWindow = 16.0f / 60.0f; // the original's 16 frames of jitter, then 16 steady
constexpr float kFlashIn = 0.1f, kFlashOut = 0.1f;
const glm::vec4 kFlashPeak(156.0f / 255.0f, 156.0f / 255.0f, 156.0f / 255.0f, 128.0f / 255.0f);
const glm::vec4 kFlashNone(1.0f, 1.0f, 1.0f, 0.0f);

// Wraps v into centre +- 100 by steps of 200.
float Wrap(float v, float centre) {
    while (v - centre > 100.0f) v -= 200.0f;
    while (v - centre < -100.0f) v += 200.0f;
    return v;
}

} // namespace

std::uint32_t WeatherEffect::Rand() {
    random_ = random_ * 214013u + 2531011u;
    return (random_ >> 16) & 0x7FFF;
}

void WeatherEffect::Clear() {
    rain_.clear();
    snow_.clear();
    ripples_.clear();
    flashesDue_.clear();
    flashAge_ = -1.0f;
    owed_ = 0.0f;
}

void WeatherEffect::Strike(int pattern) {
    ++strikes_;
    // The original's three patterns (its thunder sound picks the same one): a flash now and one
    // after 1 s; one after 1 s; a flash now and one after 0.5 s.
    switch (pattern % 3) {
    case 0: flashesDue_.insert(flashesDue_.end(), {0.0f, 1.0f}); break;
    case 1: flashesDue_.push_back(1.0f); break;
    default: flashesDue_.insert(flashesDue_.end(), {0.0f, 0.5f}); break;
    }
}

void WeatherEffect::Update(game::WeatherKind kind, int amount, const glm::vec3& eye, float dt, const WaterAt& water) {
    SR_ZONE("world: weather");
    dt = std::max(dt, 0.0f);
    // Spawn: whole clusters, the fraction carried over.
    const float rate = kind == game::WeatherKind::Rain ? 2.0f * static_cast<float>(amount)
                       : kind == game::WeatherKind::Snow ? 0.5f * static_cast<float>(amount)
                                                         : 0.0f;
    owed_ = rate > 0.0f ? owed_ + rate * dt : 0.0f;
    for (; owed_ >= 1.0f; owed_ -= 1.0f) {
        if (kind == game::WeatherKind::Rain) {
            Rain r;
            r.y0 = eye.y + static_cast<float>(Rand() % 1000 + 500) / 10.0f;
            r.x = eye.x + static_cast<float>(static_cast<int>(Rand() % 1200) - 600) / 10.0f;
            r.z = eye.z + static_cast<float>(static_cast<int>(Rand() % 1200) - 600) / 10.0f;
            r.y = r.y0;
            float surface = 0.0f;
            if (water && water(r.x, r.z, surface)) r.stop = surface;
            rain_.push_back(r);
        } else {
            Flake f;
            f.type = (Rand() & 3) == 0 ? 1 : 0;
            f.x = eye.x + static_cast<float>(static_cast<int>(Rand() % 2000) - 1000) / 10.0f;
            f.y = eye.y + static_cast<float>(Rand() % 1000 + 500) / 10.0f;
            f.z = eye.z + static_cast<float>(static_cast<int>(Rand() % 2000) - 1000) / 10.0f;
            f.dx = (Rand() & 1) ? 2.0f : -2.0f;
            f.dz = (Rand() & 1) ? 2.0f : -2.0f;
            f.clock = static_cast<float>(Rand() % 1000) / 1000.0f * 2.0f * kJitterWindow;
            snow_.push_back(f);
        }
    }

    // Rain falls; over water it ends in a ripple.
    std::size_t kept = 0;
    for (Rain& r : rain_) {
        r.age += dt;
        r.y = r.y0 - 98.0f * r.age - 4.9f * r.age * r.age;
        if (r.y <= r.stop) {
            ripples_.push_back({r.x, r.stop + 0.5f, r.z, 0.0f});
            continue;
        }
        if (r.y < eye.y - kBelow) continue;
        rain_[kept++] = r;
    }
    rain_.resize(kept);
    kept = 0;
    for (Ripple& p : ripples_) {
        p.age += dt;
        if (p.age < kRippleSeconds) ripples_[kept++] = p;
    }
    ripples_.resize(kept);

    // Snow falls, drifts and stays around the eye. Its drift signs change at 60 Hz in alternate
    // windows (the original re-rolls them on half its frames).
    const double before = jitterClock_;
    jitterClock_ += dt;
    const int ticks = static_cast<int>(std::floor(jitterClock_ * 60.0) - std::floor(before * 60.0));
    kept = 0;
    for (Flake& f : snow_) {
        f.y -= (f.type == 0 ? 12.0f : 10.0f) * dt;
        if (f.y < eye.y - kBelow) continue;
        f.clock = std::fmod(f.clock + dt, 2.0f * kJitterWindow);
        if (f.clock < kJitterWindow) {
            for (int t = 0; t < ticks; ++t) {
                if (Rand() & 1) f.dx = -f.dx;
                if (Rand() & 1) f.dz = -f.dz;
            }
        }
        f.x = Wrap(f.x + f.dx * dt, eye.x);
        f.z = Wrap(f.z + f.dz * dt, eye.z);
        snow_[kept++] = f;
    }
    snow_.resize(kept);

    // Lightning while it rains and no flash is running.
    if (kind == game::WeatherKind::Rain && flashAge_ < 0.0f && flashesDue_.empty()) {
        const float chance = 1.0f - std::exp(-kLightningPerSecond * dt);
        if (static_cast<float>(Rand()) / 32768.0f < chance) Strike(static_cast<int>(Rand() % 3));
    }
    for (float& due : flashesDue_) due -= dt;
    if (std::any_of(flashesDue_.begin(), flashesDue_.end(), [](float due) { return due <= 0.0f; })) flashAge_ = 0.0f;
    std::erase_if(flashesDue_, [](float due) { return due <= 0.0f; });
    if (flashAge_ >= 0.0f) {
        flashAge_ += dt;
        if (flashAge_ > kFlashIn + kFlashOut) flashAge_ = -1.0f;
    }
}

void WeatherEffect::Fill(gfx::WeatherView& view) const {
    view = {};
    view.rain.reserve(rain_.size());
    for (const Rain& r : rain_) view.rain.emplace_back(r.x, r.y, r.z, 0.0f);
    for (const Flake& f : snow_) view.snow[f.type].emplace_back(f.x, f.y, f.z, 0.0f);
    view.ripples.reserve(ripples_.size());
    for (const Ripple& p : ripples_) view.ripples.emplace_back(p.x, p.y, p.z, p.age);
    // The flash: to its peak in 0.1 s, back in 0.1 s.
    if (flashAge_ >= 0.0f) {
        const float t = flashAge_ < kFlashIn ? flashAge_ / kFlashIn : 1.0f - (flashAge_ - kFlashIn) / kFlashOut;
        view.flash = kFlashNone + (kFlashPeak - kFlashNone) * std::clamp(t, 0.0f, 1.0f);
    }
}

} // namespace sr::client
