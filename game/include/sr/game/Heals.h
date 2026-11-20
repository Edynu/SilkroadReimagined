#pragma once

// Heals: how much HP and MP a skill's 'heal' gives. Who it reaches is the skill unit's
// (server/game Skills.h); the cures are game::StateSet::Cure (States.h). Headless.

#include <cstdint>

#include "sr/game/Combat.h"

namespace sr::assets {
struct SkillRow;
}

namespace sr::game {

// A skill's 'heal' hp hp% mp mp% (the Cleric's heal passive, 'HLRU', added to both percents),
// 'eshp' (the HP percent is of the target's most HP, in place of the amount), its weapon bonuses
// ('mwhh' HP, 'mwmh' MP: percents of the healer's weapon's magical attack).
struct HealSpec {
    int hp = 0, hpPercent = 0, mp = 0, mpPercent = 0;
    bool ofMostHp = false;
    int weaponHp = 0, weaponMp = 0;
    explicit operator bool() const { return hp != 0 || hpPercent != 0 || mp != 0 || mpPercent != 0; }
};
HealSpec HealOf(const assets::SkillRow& skill, int healBonus = 0);

struct HealAmount {
    std::uint32_t hp = 0, mp = 0;
};
// The amounts on one target, its share of a transfer's (the flat amounts at it):
// - HP: the amount, plus its percent of it; with 'eshp' a percent of the target's most HP;
// - MP: the amount, plus its percent of it;
// - each times 1 + the target's percent more (0xAA, 0xAB), cut to a whole number;
// - plus the weapon's: (its magical attack's two ends) x 0.5 x the healer's INT balance x the tag / 100.
HealAmount HealFor(const HealSpec& heal, int sharePercent, const CombatStats& healer, const CombatStats& target);

// The experience a death takes: above level 10 the least of the level's monster experience x
// 100 (leveldata col 5) and 2 % of its experience to the next level (col 1), at least 1 (none when that
// 2 % is under 1); killed by a player (`byPlayer`): x 20 and 0.4 %; less the percent of
// 0x101 it has (a buff's). The murderers' rows and the thief monster's cap: game::DeathExperienceLoss (Pk.h).
// `fortressWar`: killed in a fortress's area during its war: the monster
// experience counts x 4 (by a player) or x 20, instead of x 20 or x 100.
std::uint64_t DeathPenalty(int level, std::uint64_t levelExperience, std::uint32_t monsterExperience, double reducePercent = 0.0,
                           bool byPlayer = false, bool fortressWar = false);

// A resurrection skill's 'resu' (the most level it raises, the percent of the experience a death took
// it gives back) and its 'heal' (HP the amount and its percent of the most, MP the same).
struct ReviveSpec {
    int maxLevel = 0, experiencePercent = 0;
    int hp = 0, hpPercent = 0, mp = 0, mpPercent = 0;
    explicit operator bool() const { return maxLevel > 0; }
};
ReviveSpec ReviveOf(const assets::SkillRow& skill);
// What it gives a dead player of that most HP and MP who lost `lost` experience at its death: the experience rounded
// up; a murderer (PvP state 2) gets half the percent.
struct Revival {
    std::uint32_t hp = 0, mp = 0;
    std::uint64_t experience = 0;
};
Revival RevivalFor(const ReviveSpec& revive, std::uint32_t maxHp, std::uint32_t maxMp, std::uint64_t lost, bool murderer = false);

} // namespace sr::game
