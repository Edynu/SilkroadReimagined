#pragma once

// Berserk (the Chinese "Hwan"): the gauge's points from kills and the mode they
// buy, shared by the game server and the client.
//
// - A player's killing hit, made while it is not in berserk, rolls for points (rand % 10000): 800 in 10,000; a
//   monster more than 3 levels below the player 8 - 0.25 x (the player's level - the monster's - 5)
//   percent, at least 0.25 and at most 8; a giant five times, an elite twice; a unique all five points
//   at once. The player's stat 0xB8, when it has one, takes that percent of the chance.
// - An exp-shared party's members alive in the same world get the points the roll gave.
// - The gauge holds five; none come while in berserk.
// - Five points buy the mode for its time: the speed twice
//   (the hwan level's SPDU 200: walk and run speed at 200 % of themselves), the damage dealt twice plus
//   the stat 0xBD (game::CombatStats::berserk), the damage taken less the hwan level's DABR percent
//   (_RefHWANLevel; levels 1..6 cut 10..35 %).

#include <cstdint>
#include <vector>

namespace sr::game {

inline constexpr std::uint8_t kBerserkPoints = 5;
// The mode's time for the five points. Ours: 12 s a point; the original's base (its stat 0x3C, added the stat 0xC3's
// seconds) is not known yet.
inline constexpr std::uint32_t kBerserkMs = 60000;

// A killing hit's roll: its chance in 10,000 and the points it gives.
struct BerserkGain {
    int chance = 0;
    std::uint8_t points = 1;
};
// `rarity` the monster's kind (0 general, 1 champion, 3 unique, 4 giant, 5 titan, 6 elite, 8 a unique
// too); `chanceFactor` the player's stat 0xB8 (a percent; 0 none).
BerserkGain BerserkPointGain(int playerLevel, int monsterLevel, std::uint8_t rarity, int chanceFactor = 0);

// A hwan level's mode (_RefHWANLevel: its ParamFourcc / ParamValue pairs): the speed's percent of itself (SPDU) and
// the cut of the damage taken in every lane (DABR).
struct HwanLevel {
    int level = 0;
    int speedPercent = 200;
    int damageCut = 0;
};
// The level's row (the highest at or below it; none: the mode's speed alone).
HwanLevel HwanLevelOf(const std::vector<HwanLevel>& table, int level);
// The mode's look told with it (proto::EntityBerserk::variant): 1, the blue second mode (SYSTEM_CH_HWAN2MODE), from
// hwan level 5, else 0 the red first (ours: the original's rule is not known yet).
constexpr int kBlueBerserkHwanLevel = 5;
constexpr std::uint8_t BerserkVariant(int hwanLevel) { return hwanLevel >= kBlueBerserkHwanLevel ? 1 : 0; }

} // namespace sr::game
