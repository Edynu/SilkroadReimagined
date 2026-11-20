#pragma once

// A character's numbers as the original's parameter keeper holds them: each
// a parameter by its id, its value the base and every change made to it by a source (a buff, a
// state, later a passive), summed by the change's kind:
//   (the base + the flat amounts) x (100 + the percents) / 100 x the factors x the percents of it / 100
// where a factor's change v multiplies by 1 + v / 100 (a value that comes to 0 before the factors is the
// factors alone) and a "percent of" change v by v / 100. Headless; the server keeps one per player and
// monster, the client shows what the server tells.

#include <array>
#include <cstdint>
#include <vector>

namespace sr::game {

struct CombatStats;

// The keeper's parameters our numbers use (the original's ids).
namespace stat {
inline constexpr std::uint16_t kStr = 1, kInt = 2, kMaxHp = 3, kMaxMp = 4, kPhyDefence = 5, kMagDefence = 6, kPhyAbsorption = 7,
                               kMagAbsorption = 8, kParry = 9, kBlock = 0xA, kHitRate = 0xB, kCritical = 0xC, kPhyAttackMin = 0xD,
                               kPhyAttackMax = 0xE, kMagAttackMin = 0xF, kMagAttackMax = 0x10, kWalkSpeed = 0x17, kRunSpeed = 0x18,
                               kHpRecovery = 0x19, kMpRecovery = 0x1A, kElementResist = 0x1B /* .. 0x20 by element bit */, kRange = 0x21,
                               kBlockIgnore = 0x38, kCriticalResist = 0x39, kDamage = 0x80 /* .. 0x83 by lane */,
                               kDamageAbsorption = 0x84 /* .. 0x87 */, kBlockBonus = 0x88 /* .. 0x8B */, kMpCost = 0x8D,
                               kElementGrade = 0x91 /* .. 0x96 */, kDamageTaken = 0xAE /* .. 0xB1 */, kFinalFactor = 0xB2 /* B3 */,
                               kFinalTaken = 0xB4 /* B5 */, kReach = 0xB7, kIgnoreDefence = 0xBC, kBerserk = 0xBD;
// The Devil's Spirit's: the STR part of the most HP and the INT part of the most MP, a
// percent ("hpna", "mpna"; links HP 3 <- 0x36 <- 0x34 <- STR, MP 4 <- 0x37 <-
// 0x35 <- INT), and the damage on a unique monster ("umdu").
inline constexpr std::uint16_t kHpFromStr = 0x34, kMpFromInt = 0x35, kUniqueDamage = 0x103;
}

// One change of a parameter: its kind (0 a flat amount, 1 a
// percent added, 2 a factor's percent, 3 a percent of it) and value.
struct StatChange {
    std::uint16_t stat = 0;
    std::uint8_t kind = 0;
    float value = 0;
};

// A worn item's blue on the keeper: its option's code (`_RefMagicOpt.Param1`, MagicOptionRow::attribute) and value to
// the parameters it changes, the
// item their source. None for the blues the keeper does not take: the element and status resists (the state roll reads
// the items: PlayerStateDefence), the item's own hit rate, parry and durability ("hr", "er", "dura": ComputeStats),
// Athanasia, Solid, Repair, Astral, Luck (alchemy and wear read them), and those not built yet (the avatars' "stra").
// The block rate's "bri" and "brna" put trunc(the item's own block) + the
// value in place of the item's white block entry (the same source and kind); ours keeps the whites in the base, so the
// change is that sum less `ownBlock`, the item's white block as ours counts it (0: none, the Devil's Spirit's).
std::vector<StatChange> BlueChanges(std::uint32_t code, std::uint32_t value, float ownBlock = 0.0f);
// The avatars' codes that add to what their item already put on the parameter ('stra', 'inta',
// 'mdia', 'luca': what the item put there plus the value): STR, STR_2, STR_3 on one hat count together;
// every other code replaces its item's change of the same parameter and kind (the keeper's upsert).
bool BlueSums(std::uint32_t code);

class StatSheet {
public:
    // A source's change of a kind of a parameter replaces the one it made before.
    void Add(std::uint32_t source, const StatChange& change);
    void Add(std::uint32_t source, const std::vector<StatChange>& changes);
    // Every change of a source; false when it had none.
    bool Remove(std::uint32_t source);
    void Clear() { entries_.clear(); }
    // The parameter's value from its base with every change (float, as the keeper keeps it).
    float Value(std::uint16_t stat, float base) const;
    bool Changes(std::uint16_t stat) const;
    bool Empty() const { return entries_.empty(); }
    struct Entry {
        std::uint32_t source = 0;
        StatChange change;
    };
    const std::vector<Entry>& Entries() const { return entries_; }

private:
    std::vector<Entry> entries_;
};

// The fighting numbers with a sheet's changes: each of CombatStats' numbers through its parameter
// (STR and INT are not in CombatStats: a player's are taken first, and its numbers made from them).
void ApplySheet(CombatStats& stats, const StatSheet& sheet);

} // namespace sr::game
