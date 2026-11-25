#include <doctest/doctest.h>

#include "sr/game/GameClock.h"

#include <initializer_list>

#include "sr/game/SimulationClock.h"
#include "sr/game/Loading.h"
#include "sr/game/Weather.h"

using sr::game::SimulationClock;

TEST_CASE("SimulationClock: steps at the configured rate, whatever the frame rate") {
    for (double fps : {30.0, 60.0, 144.0, 240.0, 7.0}) {
        SimulationClock clock(30.0);
        int steps = 0;
        const int frames = static_cast<int>(fps * 10.0); // ten seconds
        for (int i = 0; i < frames; ++i) steps += clock.Advance(1.0 / fps);
        CHECK(steps == doctest::Approx(300).epsilon(0.005));
        CHECK(clock.Alpha() >= 0.0);
        CHECK(clock.Alpha() < 1.0);
    }
}

TEST_CASE("SimulationClock: exact frames give exact steps") {
    SimulationClock clock(30.0);
    for (int i = 0; i < 1000; ++i) CHECK(clock.Advance(1.0 / 30.0) == 1);
    CHECK(clock.Steps() == 1000);
    CHECK(clock.Time() == doctest::Approx(1000.0 / 30.0));
}

TEST_CASE("SimulationClock: a long frame is bounded, the rest dropped") {
    SimulationClock clock(30.0, 8);
    CHECK(clock.Advance(2.0) == 8); // 60 steps due, 8 run
    CHECK(clock.DroppedSteps() == 52);
    CHECK(clock.Advance(1.0 / 30.0) == 1); // back to normal at once
}

TEST_CASE("SimulationClock: rate changes keep the fraction, bad input is ignored") {
    SimulationClock clock(20.0);
    CHECK(clock.Advance(0.025) == 0); // half a 20 Hz step
    CHECK(clock.Alpha() == doctest::Approx(0.5));
    clock.SetRate(30.0);
    CHECK(clock.Rate() == 30.0);
    CHECK(clock.Alpha() == doctest::Approx(0.5));
    CHECK(clock.Advance(-1.0) == 0);
    clock.SetRate(0.0); // nonsense: the default
    CHECK(clock.Rate() == SimulationClock::kDefaultRate);
}

TEST_CASE("SimulationClock: steps that could not run stay owed") {
    SimulationClock clock(30.0);
    CHECK(clock.Advance(1.0 / 30.0) == 1);
    clock.Return(1); // its tick had not arrived
    CHECK(clock.Steps() == 0);
    CHECK(clock.Alpha() == doctest::Approx(1.0));
    CHECK(clock.Advance(1.0 / 30.0) == 2); // the owed step and this frame's
    CHECK(clock.Steps() == 2);
}

TEST_CASE("the game clock runs 50 game seconds a real second and rolls the day") {
    sr::game::GameClock clock(12.0, 0);
    CHECK(clock.TimeOfDay() == doctest::Approx(0.5f)); // noon
    clock.Advance(72.0);                               // 3600 game seconds: an hour
    CHECK(clock.Hours() == doctest::Approx(13.0));
    clock.Advance(1728.0 - 72.0);                      // the rest of a game day (28.8 real minutes)
    CHECK(clock.Hours() == doctest::Approx(12.0));
    CHECK(clock.Day() == 1);
    clock.Set(20.0, 3);
    CHECK(clock.TimeOfDay() == doctest::Approx(0.75f)); // sunset at 20:00
    clock.Set(26.0, 0);                                 // past midnight rolls over
    CHECK(clock.Hours() == doctest::Approx(2.0));
    CHECK(clock.Day() == 1);
}

TEST_CASE("the weather's effective amount moves linearly to a new amount over 5 seconds") {
    using sr::game::Weather;
    using sr::game::WeatherKind;
    Weather w;
    CHECK(w.Kind() == WeatherKind::Clear);
    CHECK_FALSE(w.Falling());
    w.Set(WeatherKind::Rain, 200);
    CHECK(w.Falling());
    CHECK(w.Effective() == 0);
    w.Advance(1.0);
    CHECK(w.Effective() == 40);
    w.Advance(1.5);
    CHECK(w.Effective() == 100);
    // A new amount mid-way ramps from where it is.
    w.Set(WeatherKind::Snow, 20);
    CHECK(w.Kind() == WeatherKind::Snow);
    w.Advance(2.5);
    CHECK(w.Effective() == 60);
    w.Advance(10.0);
    CHECK(w.Effective() == 20);
    // Clear forces the amount to 0, still ramped.
    w.Set(WeatherKind::Clear, 255);
    CHECK(w.Amount() == 0);
    CHECK_FALSE(w.Falling());
    w.Advance(2.5);
    CHECK(w.Effective() == 10);
    w.Advance(2.5);
    CHECK(w.Effective() == 0);
    w.Set(WeatherKind::Rain, 999);
    CHECK(w.Amount() == 255);
}

TEST_CASE("a climate's weather: the original shard manager's rolls, spells and 3-second steps") {
    using sr::game::ClimateRow;
    using sr::game::ClimateWeather;
    using sr::game::WeatherKind;
    // _RefClimate 1001 with rain certain: Division 4 (a roll every 52.5 minutes), 240 +- 80 s of rain 100 + 0..30.
    ClimateRow rainy{1001, 1, 180, true, 4, 240000, 80000, 0, 0, 0, 100, 30, 100};
    const std::int64_t period = ClimateWeather::kDayMs / 4;
    const std::int64_t start = period * 1000 + 5; // just past a roll's mark
    ClimateWeather w;
    w.Start(rainy, start, 42);
    CHECK(w.Kind() == WeatherKind::Clear);
    CHECK(w.Amount() == 180);
    // Nothing before the next mark.
    CHECK_FALSE(w.Advance(start + period - 10));
    // The roll: the clear thins by a tenth of (180 + the rain's) every 3 s, then the rain grows.
    std::int64_t t = start + period;
    int steps = 0;
    while (w.Kind() != WeatherKind::Rain && steps < 100) {
        w.Advance(t);
        t += ClimateWeather::kStepMs;
        ++steps;
    }
    CHECK(w.Kind() == WeatherKind::Rain);
    CHECK(steps <= 12);
    int amount = 0;
    for (int i = 0; i < 20; ++i, t += ClimateWeather::kStepMs) {
        w.Advance(t);
        amount = w.Amount();
    }
    CHECK(amount >= 100);
    CHECK(amount <= 130);
    // The spell over (at most 320 s): back to the initial clear.
    for (int i = 0; i < 200; ++i, t += ClimateWeather::kStepMs) w.Advance(t);
    CHECK(w.Kind() == WeatherKind::Clear);
    CHECK(w.Amount() == 180);

    // Every server of a shard alike: the same seed and clock, the same weather, whenever it started.
    ClimateWeather a, b;
    a.Start(rainy, start, 7);
    b.Start(rainy, start + 40'000, 7);
    for (std::int64_t at = start + 60'000; at < start + 3 * period; at += 1000) {
        a.Advance(at);
        b.Advance(at);
        CHECK(a.Packed() == b.Packed());
    }
    // ChangeWeather 0 (_RefClimate 3001): its initial weather for good.
    ClimateRow still{3001, 1, 250, false, 0, 240000, 30000, 0, 0, 0, 0, 0, 100};
    ClimateWeather s;
    s.Start(still, start, 1);
    for (std::int64_t at = start; at < start + 10 * ClimateWeather::kDayMs; at += 60'000) CHECK_FALSE(s.Advance(at));
    CHECK(s.Packed() == (1 | 250 << 8));
}

TEST_CASE("the loading screen's picture: by kind, then by the teleport's destination") {
    using sr::game::LoadingKind;
    using sr::game::LoadingPicture;
    CHECK(LoadingPicture(LoadingKind::FirstEntry, 0, 0) == "interface\\loading\\loading_europe_1.ddj");
    CHECK(LoadingPicture(LoadingKind::FirstEntry, 0, 1) == "interface\\loading\\loading_europe_2.ddj");
    CHECK(LoadingPicture(LoadingKind::Rebirth, 0x694F, 0) == "interface\\loading\\loading_rebirth.ddj");
    CHECK(LoadingPicture(LoadingKind::Teleport, 0x694F, 0) == "interface\\loading\\loading_constantinople.ddj");
    CHECK(LoadingPicture(LoadingKind::Teleport, 0x8001, 0) == "interface\\loading\\loading_dungeons_donwhang.ddj");
    CHECK(LoadingPicture(LoadingKind::Teleport, 0x8003, 0) == "interface\\loading\\loading_royalmausoleum.ddj");
    CHECK(LoadingPicture(LoadingKind::Teleport, 0x609E, 0) == "interface\\loading\\loading_river.ddj");
    CHECK(LoadingPicture(LoadingKind::Teleport, 0x6759, 0) == "interface\\loading\\loading_port2.ddj");
    CHECK(LoadingPicture(LoadingKind::Teleport, 0x1234, 0) == "interface\\loading\\loading_china_1.ddj");
    // 1.188's additions.
    CHECK(LoadingPicture(LoadingKind::Teleport, 0x5A30, 0) == "interface\\loading\\loading_alex.ddj");
    CHECK(LoadingPicture(LoadingKind::Teleport, 0x5441, 0) == "interface\\loading\\loading_desert.ddj");
    CHECK(LoadingPicture(LoadingKind::Teleport, 0x4A4B, 0) == "interface\\loading\\loading_pharaoh.ddj");
    CHECK(LoadingPicture(LoadingKind::Teleport, 0x5C33, 0) == "interface\\loading\\loading_thiefzone.ddj");
    CHECK(LoadingPicture(LoadingKind::Teleport, 0x65BD, 0) == "interface\\loading\\loading_loksan.ddj");
}
