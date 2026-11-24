#pragma once

// Fighting numbers (C10b): a character's or a monster's attack and defence, and one hit's damage.
// Headless; the server decides every hit, the client shows the same numbers (the stat panel).
//
// Where they come from:
// - The hit is the original game server's: a point between the attack's min and max, the range first raised 1 % a level
//   of the skill's mastery; the point's centre from the attacker's hit rate over the target's parry
//   rate and the level gap, moved by the least of three rolls; divided by 1 + the target's
//   absorption / 100, less its defence (unless the attacker ignores it), times the skill's power;
//   then the buffs' and passives' factors, berserk, a physical critical's double, 3 % a level above
//   the target (at most 30 %), a player's balance, a correction of a hit under 5 % of the point, the
//   final factors. Criticals, blocks and ignoring defence roll through each attacker's chance
//   history (ChanceRolls), as the original's do.
// - The numbers themselves (what items and STR / INT give), the original's parameter keeper: a player's physical
//   attack is the
//   weapon's (whites and plus) plus STR times the weapon's physical reinforcement in percent (bare
//   hands 30.6 / 34.2 for min / max); magical the same with INT (49 / 54.8). Defence is the armour's
//   plus STR (INT) times each part's reinforcement (an empty part its default, together 19 % /
//   30.5 %). Absorption is the armour's and the accessories'. Max HP is STR x 10 x 1.02^(level - 1),
//   max MP the same with INT. Hit rate and parry rate: the items' plus 10 each.

#include <array>
#include <cstdint>
#include <random>
#include <vector>

#include "sr/game/Items.h"
#include "sr/game/Stats.h"

namespace sr::assets {
struct ItemRow;
struct ObjectRow;
struct SkillRow;
struct MagicOptionRow;
} // namespace sr::assets

namespace sr::game {

struct CombatStats {
    int level = 1;
    double phyAttackMin = 0, phyAttackMax = 0, magAttackMin = 0, magAttackMax = 0;
    double phyDefence = 0, magDefence = 0;
    double phyAbsorption = 0, magAbsorption = 0; // percent: the hit is divided by 1 + it / 100 (stats 7, 8)
    double hitRate = 10, evasion = 10;
    double critical = 0; // percent chance of a physical critical
    double block = 0;
    std::uint32_t maxHp = 1, maxMp = 0;
    double phyBalance = 1, magBalance = 1; // players only (monsters 1)
    float range = 0;        // the weapon's reach in world units (0: bare hands)
    float bodyRadius = 5;   // added to reaches (monsters: characterdata col 50)
    int weaponKind = 1;     // the weapon's TID4 (1: none), which picks the basic attack
    bool european = false;  // a European character (the fortress hammer's swing is its race's)
    // What buffs, passives and states add (the original's stat slots; all neutral here,
    // their skills fill them). By an attack's lane: physical melee, physical ranged, magical melee,
    // magical ranged.
    double criticalResist = 0;  // percent: an attacker's critical chance divided by 1 + it / 100 (0x39)
    double blockIgnore = 0;     // percent: a target's block chance divided by 1 + it / 100 (0x38)
    std::array<double, 4> blockBonus{};    // added to the block rate against that lane (0x88..0x8B)
    std::array<double, 4> damagePercent{}; // the hits it deals in that lane, + percent (0x80..0x83)
    std::array<double, 4> damageTaken{};   // the hits it takes in that lane, times it (0: none; 0xAE..0xB1)
    std::array<double, 2> finalFactor{};      // its physical / magical hits, times it (0: none; 0xB2, 0xB3)
    std::array<double, 2> finalTakenFactor{}; // the physical / magical hits it takes, times it (0: none; 0xB4, 0xB5)
    double ignoreDefenceChance = 0; // percent: a player's hit on a monster skips its defence (0xBC)
    // A monster's by its rarity byte (MonsterCombatStats): its attack range's ends times `rangeFactor` (a party monster
    // 1.2, an elite 1.5 more), its hits times `hitScale` after the target's share (a party monster 1.3; a giant, a titan,
    // an elite 1.5, an event's strong one 1.8). Players 1.
    double rangeFactor = 1, hitScale = 1;
    double classBonus = 0;          // percent added against a target of class 3 (0x103)
    int targetClass = 0;            // a monster's class (3: the class classBonus is against)
    bool berserk = false;           // in berserk: hits times 2 + berserkBonus
    double berserkBonus = 0;        // (0xBD)
    double mpCostPercent = 100;     // a skill's MP is this percent of its cost (0x8D; a buff's 'dcmp' lowers it)
    double phyParts = 0, magParts = 0; // the armour parts' and the shield's reinforcements summed (percent of STR / INT)
    // The weapon's reinforcements (else the bare hands'): the attacks' percent of STR / INT (the keeper's 0x22 / 0x24 phy
    // min / max, 0x23 / 0x25 mag; 0x11..0x14 <- STR / INT times them).
    double phyReinforceMin = 0, phyReinforceMax = 0, magReinforceMin = 0, magReinforceMax = 0;
    // Heals (K7): the percents more HP and MP a heal gives it (0xAA, 0xAB: a buff's 'rhru'), and its
    // weapon's two magical attack ends (a heal's 'mwhh' / 'mwmh' bonus).
    std::array<double, 2> healReceived{};
    double weaponMagMin = 0, weaponMagMax = 0;
    double weaponPhyMin = 0, weaponPhyMax = 0; // the weapon's own (its whites with the plus; none: 0): its threat
    // A player's: the STR and INT its numbers were made from (its buffs' in), the worn shield's own
    // physical defence (a share of it is what 'spda' turns into attack).
    double strength = 0, intelligence = 0;
    double shieldDefence = 0;
};

// The original's chance rolls: each attacker keeps a running value per kind of
// roll and skill, so streaks are rare and the long-run rate is the chance. The first roll of a key
// compares 0..100 with the chance; each later one with the running value, which a success lowers
// by 100 - chance and a failure raises by the chance. A chance of 0 never succeeds, 100 always.
class ChanceRolls {
public:
    // kind: 'C' critical, 'B' block, 'M' ignoring a monster's defence; skill: its id.
    bool Roll(int chance, char kind, std::uint32_t skill, std::mt19937& random);

private:
    std::vector<std::pair<std::uint32_t, int>> history_;
};

// One worn item, by its place (0..12).
struct WornItem {
    int place = 0;
    const assets::ItemRow* row = nullptr;
    const ItemInstance* item = nullptr;
    int setPlus = 0; // its set's raise of its total plus (game::SetWear)
};

std::uint32_t PlayerMaxHp(int level, int strength);
std::uint32_t PlayerMaxMp(int level, int intelligence);
// The same from the keeper's STR (INT) part as it stands (params 0x34 / 0x35: STR x (100 + the Devil's Spirit's HPNA) /
// 100, a float): trunc(1.02^(level - 1) x it x 10).
std::uint32_t PlayerVitalFrom(int level, double part);
// The bare hands' reinforcements (no weapon worn): the keeper's as the character came in (phy 30.6..34.2,
// mag 49.0..54.8), else after a weapon left its hand since (taken off or broken:
// phy 30.6..49.0, mag 34.2..54.8, the phy max and the mag min swapped: the
// original's quirk, kept).
enum class BareHands : std::uint8_t { AsEntered, AfterWeapon };
CombatStats PlayerCombatStats(int level, int strength, int intelligence, const std::vector<WornItem>& worn,
                              const std::vector<assets::MagicOptionRow>* options, BareHands bare = BareHands::AsEntered);
// The same with a sheet of changes (its buffs): STR and INT through it first, the
// numbers made from them, then each number through it.
CombatStats PlayerCombatStats(int level, int strength, int intelligence, const std::vector<WornItem>& worn,
                              const std::vector<assets::MagicOptionRow>* options, const StatSheet& sheet,
                              BareHands bare = BareHands::AsEntered);
// `rarity` its grade: its low four bits are the class the formula's unique damage goes by (3 a unique).
CombatStats MonsterCombatStats(const assets::ObjectRow& row, std::uint8_t rarity);

// What one use of a skill hits with (its 'att' tag; a basic attack is a skill too).
struct AttackSpec {
    std::uint32_t skill = 0; // its id (the chance rolls' key)
    int kind = 5;            // 'att' first argument: bits 1 melee, 2 ranged, 4 physical, 8 magical
    int power = 100;         // percent
    int flatMin = 0, flatMax = 0;
    bool magical = false;
    int hits = 1;
    int criticalFlat = 0, criticalPercent = 0; // 'cr': the critical chance + flat + its percent of it
    int mastery = 0;         // the attacker's level of the skill's mastery (its range + 1 % a level)
    bool formula = true;     // an 'att': the rolls and the formula's damage (false: the skill's own damage alone)
    int imbuePercent = 0;    // 'att' fifth: the share of an imbue's force its hits take (the Chinese skills')
    int fixedDamage = 0;     // 'pdmg': damage of its own, past the defences (less on a target of a higher level)
    int lifeSteal = 0;       // 'lfst': HP taken from the target for the caster's own
    int drainHp = 0, drainMp = 0; // 'dmgt': the shares of its damage (percent) the attacker recovers as HP and MP
    // States and motions: 'da' strikes a target that is down, for this percent
    // of the formula's damage (an imbue's too); 'atca' adds its percent on a target in one of the mask's
    // states; 'ko' knocks down (level, chance), 'kb' knocks back (chance, distance).
    int downPercent = 0;
    std::uint32_t againstMask = 0;
    int againstPercent = 0;
    int knockDownLevel = 0, knockDownChance = 0;
    int knockBackChance = 0, knockBackDistance = 0;
    // The passives' values its 'getv' codes read (K6): added to the
    // variance's roll, and the variance's point times a factor.
    int spreadBonus = 0;
    double pointFactor = 1.0;
    int Lane() const;        // 0..3: physical melee, physical ranged, magical melee, magical ranged; -1 neither melee nor ranged
};
AttackSpec AttackOf(const assets::SkillRow& skill);

// A skill's threat on the monsters it strikes: on a hit's running
// total T (its damage added), `tant` (else `tnt2`): T = trunc(T x (1 + percent / 100)) + flat; then the
// weapon's threat by `pwtt` / `mwtt` (per mille of 20: 2000 = 20 x the weapon's mean attack); its cut
// (`dtnt`: flat, percent; `pwdt` / `mwdt` by the weapon) as the result is applied.
struct ThreatSpec {
    bool raise = false;          // a tant or tnt2
    std::int64_t flat = 0;
    int percent = 0;
    int physicalWord = 0, magicalWord = 0; // pwtt, mwtt
    bool cut = false;            // a dtnt
    std::int64_t cutFlat = 0;
    int cutPercent = 0;
    int physicalCutWord = 0, magicalCutWord = 0; // pwdt, mwdt
    bool Any() const { return raise || cut || physicalWord != 0 || magicalWord != 0 || physicalCutWord != 0 || magicalCutWord != 0; }
};
ThreatSpec ThreatOf(const assets::SkillRow& skill);
// The weapon's threat: trunc(mean attack x word / 100) (ours: the
// character's attack range for the weapon's, its STR / INT ratio at 1).
std::int64_t WeaponThreat(const CombatStats& attacker, int word, bool magical);
// A hit's running total after its damage: T + damage, the raise, the weapon's threat.
std::int64_t RaiseThreat(std::int64_t total, std::uint32_t damage, const ThreatSpec& spec, const CombatStats& attacker);

struct HitResult {
    std::uint32_t damage = 0;
    bool critical = false;
    bool blocked = false;
    bool magical = false; // it had a magical part (the skill's, or an imbue's force): it breaks a root
};
// Who hits whom (a player's balance; ignoring defence is a player's on a monster), the attacker's
// chance history (null: plain rolls), the target's share of the hit (an area's further targets get
// less), its HP now (a life steal takes no more; 0: not known), its states' mask (game::StateBit)
// and whether it is down (knocked down).
struct HitRoll {
    bool attackerPlayer = false, defenderPlayer = false;
    ChanceRolls* rolls = nullptr;
    int share = 100;
    std::uint32_t defenderHp = 0;
    std::uint32_t defenderStates = 0;
    bool defenderDown = false;
    // A target an imbue's area added (the lightning's jump): it takes the
    // magical parts only, the skill's and the imbue's.
    bool magicalOnly = false;
};
// One hit (of a skill's `hits`) from `attacker` on `defender`, with the force of the attacker's imbue
// (null: none; its own attack, the attacker's mastery level for it in its `mastery`). A hit that is
// not blocked does at least 1.
HitResult RollHit(const CombatStats& attacker, const CombatStats& defender, const AttackSpec& attack, const HitRoll& who, std::mt19937& random,
                  const AttackSpec* imbue = nullptr);
// A skill's own damage ('pdmg'): 2.5 % less a level the target is above the
// attacker, at least a tenth of it.
std::uint32_t FixedDamage(int damage, int attackerLevel, int defenderLevel);

} // namespace sr::game
