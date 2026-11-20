#pragma once

// How a character's level against its highest mastery shares a kill between experience and skill
// experience: a character above its
// masteries gets less experience and more skill points, 10 % a level of the gap each way, within
// 0.1 and 1.9, so a gap of 9 is the most it counts.

#include <algorithm>

namespace sr::game {

struct MasteryGapRates {
    float experience = 1.0f;
    float skill = 1.0f;
};

inline MasteryGapRates MasteryGapRatesOf(int level, int highestMastery) {
    if (highestMastery > 120) return {}; // both 1
    const float gap = static_cast<float>(level - highestMastery);
    return {std::clamp(1.0f - gap * 0.1f, 0.1f, 1.9f), std::clamp(1.0f + gap * 0.1f, 0.1f, 1.9f)};
}

} // namespace sr::game
