#include "sr/assets/Environment.h"

#include <algorithm>
#include <cmath>

#include "sr/core/BinaryReader.h"

namespace sr::assets {

const EnvironmentEntry* EnvironmentData::Find(std::uint16_t key) const {
    for (const EnvironmentEntry& e : entries) {
        if (e.key == key) return &e;
    }
    return nullptr;
}

bool DecodeEnvironment(std::span<const std::byte> file, EnvironmentData& out, std::string& error) {
    out = {};
    core::BinaryReader r(file);
    if (!r.Matches("JMXVENVI")) {
        error = "not a JMXVENVI file";
        return false;
    }
    r.Skip(12);
    const std::uint16_t count = r.U16();
    r.CountedString();
    out.entries.reserve(count);
    for (std::uint16_t i = 0; i < count && r.Ok(); ++i) {
        EnvironmentEntry entry;
        entry.key = r.U16();
        entry.name = r.CountedString();
        r.CountedString();
        r.CountedString();
        for (int c = 0; c < static_cast<int>(EnvironmentCurve::Count) && r.Ok(); ++c) {
            const bool colour = IsColourCurve(static_cast<EnvironmentCurve>(c));
            const std::uint32_t keys = r.U32();
            if (keys > 4096 || !r.Ok() || r.Remaining() < std::size_t{keys} * (colour ? 16 : 8)) {
                error = "environment " + std::to_string(entry.key) + ": bad curve";
                return false;
            }
            auto& curve = entry.curves[static_cast<std::size_t>(c)];
            curve.resize(keys);
            for (EnvironmentKey& k : curve) {
                if (colour) {
                    k.value = {r.F32(), r.F32(), r.F32()};
                } else {
                    k.value[0] = r.F32();
                }
                k.t = r.F32();
            }
        }
        out.entries.push_back(std::move(entry));
    }
    if (!r.Ok()) {
        error = "environment.ifo is truncated";
        return false;
    }
    return true;
}

float SampleEnvironment(const std::vector<EnvironmentKey>& curve, float time, int channel, float fallback) {
    if (curve.empty()) return fallback;
    const std::size_t c = static_cast<std::size_t>(std::clamp(channel, 0, 2));
    // The first key at or after `time`; before the first or after the last key, that key's value.
    std::size_t i = 0;
    while (i < curve.size() && time > curve[i].t) ++i;
    if (i == 0) return curve.front().value[c];
    if (i == curve.size()) return curve.back().value[c];
    const float span = curve[i].t - curve[i - 1].t;
    const float f = span > 1e-4f ? std::clamp((time - curve[i - 1].t) / span, 0.0f, 1.0f) : 0.0f;
    // A uniform Catmull-Rom segment through keys i-2 .. i+1; the end keys stand in for missing ones.
    const float p0 = curve[i >= 2 ? i - 2 : i - 1].value[c];
    const float p1 = curve[i - 1].value[c];
    const float p2 = curve[i].value[c];
    const float p3 = curve[i + 1 < curve.size() ? i + 1 : i].value[c];
    return 0.5f * (2.0f * p1 + (p2 - p0) * f + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * f * f +
                   (3.0f * p1 - p0 - 3.0f * p2 + p3) * f * f * f);
}

float EnvironmentTimeFromClock(double clockHours) {
    double h = std::fmod(clockHours, 24.0);
    if (h < 0) h += 24.0;
    double e = 0;
    if (h >= 4.0 && h <= 20.0) {
        e = 6.0 + (h - 4.0) * 0.75; // the day: 16 clock hours for the environment's 12
    } else {
        e = 18.0 + ((h < 4.0 ? h + 24.0 : h) - 20.0) * 1.5; // the night: 8 for 12
        if (e >= 24.0) e -= 24.0;
    }
    return static_cast<float>(e / 24.0);
}

} // namespace sr::assets
