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
}
