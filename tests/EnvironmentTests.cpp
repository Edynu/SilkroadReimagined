#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "sr/assets/Environment.h"

using namespace sr::assets;

namespace {

struct Writer {
    std::vector<std::byte> bytes;
    void Raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        bytes.insert(bytes.end(), b, b + n);
    }
    void U16(std::uint16_t v) { Raw(&v, 2); }
    void U32(std::uint32_t v) { Raw(&v, 4); }
    void F32(float v) { Raw(&v, 4); }
    void Str(const std::string& s) {
        U32(static_cast<std::uint32_t>(s.size()));
        Raw(s.data(), s.size());
    }
};

} // namespace

TEST_CASE("environment.ifo entries decode in curve order") {
    Writer w;
    w.Raw("JMXVENVI1003", 12);
    w.U16(1);
    w.Str("");
    w.U16(7);
    w.Str("Env7");
    w.Str("");
    w.Str("");
    for (int c = 0; c < static_cast<int>(EnvironmentCurve::Count); ++c) {
        const bool colour = IsColourCurve(static_cast<EnvironmentCurve>(c));
        w.U32(2);
        for (int k = 0; k < 2; ++k) {
            const float base = static_cast<float>(c * 10 + k);
            if (colour) {
                w.F32(base);
                w.F32(base + 0.25f);
                w.F32(base + 0.5f);
            } else {
                w.F32(base);
            }
            w.F32(k == 0 ? 0.25f : 0.75f);
        }
    }
    w.Raw("tree", 4); // the region tree follows; not read

    EnvironmentData data;
    std::string error;
    REQUIRE_MESSAGE(DecodeEnvironment(w.bytes, data, error), error);
    REQUIRE(data.entries.size() == 1);
    const EnvironmentEntry* e = data.Find(7);
    REQUIRE(e != nullptr);
    CHECK(e->name == "Env7");
    const auto& diffuse = e->Curve(EnvironmentCurve::Diffuse);
    REQUIRE(diffuse.size() == 2);
    CHECK(diffuse[1].value[2] == doctest::Approx(21.5f));
    CHECK(diffuse[1].t == doctest::Approx(0.75f));
    const auto& fogEnd = e->Curve(EnvironmentCurve::FogEnd);
    CHECK(fogEnd[0].value[0] == doctest::Approx(110.0f));
    CHECK(e->Curve(EnvironmentCurve::Stars)[1].value[0] == doctest::Approx(151.0f));
    CHECK(data.Find(8) == nullptr);

    // Clamped ends; with two keys the original's spline is the straight line between them.
    CHECK(SampleEnvironment(diffuse, 0.0f, 0, -1.0f) == doctest::Approx(20.0f));
    CHECK(SampleEnvironment(diffuse, 1.0f, 0, -1.0f) == doctest::Approx(21.0f));
    CHECK(SampleEnvironment(diffuse, 0.5f, 0, -1.0f) == doctest::Approx(20.5f));
    CHECK(SampleEnvironment(diffuse, 0.5f, 1, -1.0f) == doctest::Approx(20.75f));
    CHECK(SampleEnvironment({}, 0.5f, 0, -1.0f) == doctest::Approx(-1.0f));

    w.bytes.resize(40);
    CHECK_FALSE(DecodeEnvironment(w.bytes, data, error));
}

TEST_CASE("environment curves follow the original's Catmull-Rom spline") {
    using namespace sr::assets;
    // Keys 0, 1, 0, 1 at t 0, 1/3, 2/3, 1.
    std::vector<EnvironmentKey> curve(4);
    for (int i = 0; i < 4; ++i) curve[static_cast<std::size_t>(i)] = {static_cast<float>(i) / 3.0f, {static_cast<float>(i & 1), 0, 0}};
    // Middle segment, f = 0.5: p0 0, p1 1, p2 0, p3 1 -> 0.5 (2 - 0.5 + (0 - 5 + 0 - 1) / 4 + (3 - 0 - 0 + 1) / 8) = 0.5.
    CHECK(SampleEnvironment(curve, 0.5f, 0, -1.0f) == doctest::Approx(0.5f));
    // First segment at f = 0.5 (p0 = p1 = 0, p2 = 1, p3 = 0): 0.5 (0.5 + 4 / 4 - 3 / 8) = 0.5625, not the linear 0.5.
    CHECK(SampleEnvironment(curve, 1.0f / 6.0f, 0, -1.0f) == doctest::Approx(0.5625f));
    // Exactly on keys, and clamped outside.
    CHECK(SampleEnvironment(curve, 1.0f / 3.0f, 0, -1.0f) == doctest::Approx(1.0f));
    CHECK(SampleEnvironment(curve, -0.5f, 0, -1.0f) == doctest::Approx(0.0f));
    CHECK(SampleEnvironment(curve, 1.5f, 0, -1.0f) == doctest::Approx(1.0f));
}

TEST_CASE("the game clock maps to the environment's time unevenly") {
    using namespace sr::assets;
    CHECK(EnvironmentTimeFromClock(4.0) == doctest::Approx(0.25f));  // sunrise
    CHECK(EnvironmentTimeFromClock(12.0) == doctest::Approx(0.5f));  // noon
    CHECK(EnvironmentTimeFromClock(20.0) == doctest::Approx(0.75f)); // sunset
    CHECK(EnvironmentTimeFromClock(0.0) == doctest::Approx(0.0f));   // midnight (the client starts here)
    CHECK(EnvironmentTimeFromClock(22.0) == doctest::Approx(21.0f / 24.0f));
    CHECK(EnvironmentTimeFromClock(2.0) == doctest::Approx(3.0f / 24.0f));
    CHECK(EnvironmentTimeFromClock(26.0) == doctest::Approx(3.0f / 24.0f));
}
