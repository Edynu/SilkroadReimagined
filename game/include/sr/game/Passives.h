#pragma once

// Passives and the skills' conditions: a player's learned passives (Basic_Activity 0) run as lasting effects on it
// through the buff routine, which first asks
// for what the skill's conditions want worn ('reqi'); their buff tags change its numbers (the keeper,
// game/Stats.h), their named values ('setv') are what other skills read ('getv': a weapon's attack,
// a hit rate, a range, an MP cost, a time, the poison's, the blesses'). Headless; the server applies
// them, the client shows the numbers it is told.

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "sr/game/Buffs.h"
#include "sr/game/Combat.h"
#include "sr/game/Skills.h"
#include "sr/game/States.h"
#include "sr/game/Stats.h"

namespace sr::assets {
class SkillTable;
struct SkillRow;
} // namespace sr::assets

namespace sr::game {

// What a player wears that the conditions ask about (an item's type: TID3 its kind, TID4 its sort).
struct WornKinds {
    int weapon = 0;              // the weapon's TID4 (0: none)
    int shield = 0;              // the shield's TID4 in the shield hand (0: none; arrows and bolts are no shield)
    std::array<int, 7> armour{}; // by part (an armour's TID4, 1..6): its kind (TID3; 0: nothing there)
    bool spirit = false;         // a Devil's Spirit in avatar place 4, awake ('reqi 14 1')
};
// From the worn items (a broken one counts as not there, as for its bonuses).
WornKinds WornKindsOf(const std::vector<WornItem>& worn);

// A skill's conditions on what is worn (for its effect, for a cast):
// each 'reqi' (up to five) a kind and a sort: 6 the weapon's TID4, 4 the shield's, 1..3 and 9..11
// armour of that kind (sort 0: on all six parts; else the part of that number); 14 with sort 1 an awake item in avatar
// place 4, the Devil's Spirit (the cast's, the run's and the re-check's); others never. With
// 'reqn' every one must hold, else any one. A skill of none: always.
bool EquipmentFits(const assets::SkillRow& skill, const WornKinds& worn);

// The named values the passives give ('setv' code value), by code: a later one of a code takes the
// earlier's place.
class SkillModifiers {
public:
    void Set(std::uint32_t code, int value);
    std::optional<int> Find(std::uint32_t code) const;
    bool Empty() const { return values_.empty(); }
    const std::vector<std::pair<std::uint32_t, int>>& Values() const { return values_; }

private:
    std::vector<std::pair<std::uint32_t, int>> values_;
};

// What the values do to one skill, by the codes it names ('getv').
struct SkillBoosts {
    int spread = 0;            // added to the variance's roll (percent points): the dagger's, the axes' hit rate
    double pointFactor = 1.0;  // the variance's point times it: a weapon's or an element's attack percent
    int fixedDamage = 0;       // added to its own damage ('pdmg')
    int lifeSteal = 0;         // added to its life steal ('lfst')
    int range = 0;             // added to its reach
    std::vector<int> manaCuts; // percents off its MP, one after another
    int upkeepCut = 0;         // percent off a toggle's MP each period
    std::uint32_t extraMs = 0; // its effect's time: plus this, else plus this percent of its 'dura'
    int extraPercent = 0;
    int poisonLevel = 0, poisonDamage = 0; // its poison's level (a second each) and a tick's damage
    int heal = 0;              // added to its heal's amounts (K7)
    BuffBonus bonus;           // a buff's bonuses (the blesses: 'stri', 'inti', 'defp')
};
// `magical`: which of the two variances the skill's attack goes through (its factors differ).
SkillBoosts BoostsOf(const assets::SkillRow& skill, bool magical, const SkillModifiers& modifiers);
// Its MP after the cuts (each a whole number), and its effect's time after the extra.
std::uint32_t CutMana(std::uint32_t mana, const std::vector<int>& cuts);
std::uint32_t ExtendedMs(std::uint32_t durationMs, const SkillBoosts& boosts);

// A player's passives as they stand: each learned passive group (at its level) with its changes,
// counted while its conditions hold, their named values, and their state resistances ('real' mask
// flat grade: by bucket the highest grade and its highest flat amount).
struct Passives {
    struct Entry {
        std::uint32_t skill = 0;
        bool active = false;
        std::vector<StatChange> changes;
    };
    std::vector<Entry> entries;
    SkillModifiers modifiers;
    std::array<int, kResistBuckets> resistFlat{}, resistGrade{};
    // Each active passive's changes into a sheet, its own source (kPassiveSource | its group).
    void AddTo(StatSheet& sheet) const;
};
inline constexpr std::uint32_t kPassiveSource = 0xFE000000u;
Passives PassivesOf(const SkillBook& book, const assets::SkillTable& table, const WornKinds& worn, const BuffBasis& basis);

// A player as a state's roll sees it: its level; its worn items' resistance options (the
// jewellery's MATTR_RESIST_*: 'fz' adds its value
// to the freeze and the frostbite resistances (stats 0x1B, 0x1C), 'es' shock 0x1D, 'bu' burn 0x1E, 'ps' poison 0x1F,
// 'zb' zombie 0x20; 'rsli' sleep, 'rfei' fear, 'rsti' stun, 'rdii' disease, 'rcsi' combustion to their buckets'
// percent); and its passives' 'real' flat amounts and grades.
StateDefence PlayerStateDefence(int level, const std::vector<WornItem>& worn, const std::vector<assets::MagicOptionRow>* options,
                                const Passives* passives);

} // namespace sr::game
