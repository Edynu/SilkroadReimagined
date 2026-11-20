#pragma once

// The original's game clock ("Time of day, sky and fog"): it starts at 00:00 when the
// client starts and a game second lasts 20 ms, so a game day passes in 28.8 real minutes. Its
// hour maps unevenly to the environment's time of day (assets::EnvironmentTimeFromClock): the
// day's 16 hours from 04:00 to 20:00 are the environment's 12 day hours. No server message sets
// it in v1.150; the day count picks the moon's phase.

#include <cstdint>

namespace sr::game {

class GameClock {
public:
    explicit GameClock(double startHours = 0.0, int startDay = 0) : hours_(startHours), day_(startDay) { Normalise(); }

    // Real seconds pass.
    void Advance(double realSeconds);
    void Set(double hours, int day) {
        hours_ = hours;
        day_ = day;
        Normalise();
    }

    double Hours() const { return hours_; } // 0..24
    int Day() const { return day_; }        // days since the start
    // The environment's time of day (0 midnight, 0.25 sunrise, 0.5 noon, 0.75 sunset).
    float TimeOfDay() const;

private:
    void Normalise();
    double hours_ = 0.0;
    int day_ = 0;
};

} // namespace sr::game
