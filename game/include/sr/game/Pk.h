#pragma once

// Murderers: the PvP states, the PK counts and their penalties. Headless, shared by the game
// server (which keeps and applies them) and the client (which shows them). The rules and numbers are the original's;
// the messages are ours (PkProtocol.h).
//
// - A player's PvP state: 0 an innocent, 1 an assaulter (he struck an innocent in the last 20 s: his hostility
//   records), 2 a murderer (he has penalty points).
// - Three counters in `_Char`: DailyPK (0..15, reset at the server's local midnight for those online), TotalPK
//   (0..15, one off every 48 h of the PK keeper, `_TimedJob` 2/0) and PKPenaltyPoint (0..200,000; off by killing
//   monsters and by dying).
// - The attack check of a player on a player is one function over both sides' facts (AttackCheck).

#include <cstdint>

#include "sr/game/WorldProtocol.h"

namespace sr::game {

// The PvP state.
enum class PvpState : std::uint8_t { Neutral = 0, Assaulter = 1, Murderer = 2 };

inline constexpr int kPkLevel = 20;              // both sides level 20 or more (else 0x3016 / 0x3017)
inline constexpr int kDailyPkMost = 15;          // DailyPK's range and the daily limit (0x3014)
inline constexpr int kTotalPkMost = 15;          // TotalPK's range
inline constexpr std::int32_t kPenaltyMost = 200'000; // PKPenaltyPoint's range and the limit (0x30D40; 0x3015)
inline constexpr int kHostileSeconds = 20;       // a hostility record's life
inline constexpr std::int32_t kPenaltyStep = 1'200; // a murder's points per step of TotalPK (0x4B0)
inline constexpr std::int64_t kKeeperSeconds = 172'800; // the PK keeper's period: one TotalPK off every 48 h
inline constexpr std::int32_t kMonsterDeathRelief = 600; // points off for a death to a monster (0x258)
inline constexpr std::int32_t kGuildWarDeathRelief = 200; // in a guild war (0xC8)

// The state the points and the records give.
inline PvpState StateFor(std::int32_t points, bool hasRecords) {
    return points > 0 ? PvpState::Murderer : hasRecords ? PvpState::Assaulter : PvpState::Neutral;
}

// --- A kill ---------------------------------------------------------------------------------------------

// The kill's relation: which penalty a death takes and what a kill counts.
enum class KillRelation : std::uint8_t {
    None = 0,
    Job = 1,         // a thief and a trader or hunter in their suits
    Team = 2,        // a timed event, the arena, free PvP foes: no penalty
    GuildWar = 3,    // guilds at war
    Monster = 4,     // a monster (or no killer)
    Open = 5,        // anything else between players: a murder, or a kill of a murderer or an assaulter
    FortressWar = 6, // the fortress world during a war (its own penalty)
};

// A murder's points: trunc((TotalPK + 1) / 2) x 1,200 with the TotalPK after it (1 the first murder).
std::int32_t PenaltyForMurder(int totalPkAfter);
// A murder's 1 when the victim's level is at least the killer's, else 1 + 2 x trunc(gap / 5).
int DailyForMurder(int killerLevel, int victimLevel);
// Points off for a monster killed (its table), by the
// monster's level minus the player's: 4 or more 10, 1..3 7, -3..0 5, -6..-4 3, -7 or less 1.
std::int32_t MonsterKillRelief(int monsterLevel, int playerLevel);
// Points off for a death: 600 to a monster, 200 in a guild war, none otherwise. Such a death also takes 1
// from DailyPK (DeathTakesDaily).
std::int32_t DeathRelief(KillRelation relation);
inline bool DeathTakesDaily(KillRelation relation) { return relation == KillRelation::Monster || relation == KillRelation::GuildWar; }

// --- A death ----------------------------------------------------------------------------------------------

// The chance (percent) an item drops at a death above level 10, by the
// points: 0 5, 1..3,999 30, 4,000..14,999 50, 15,000..29,999 70, 30,000+ 100; drawn as rand() % 101 <= chance.
int DeathItemChance(std::int32_t points);
// Whether a death of that relation may drop an item (relations 3, 4, 5; level 11 or more).
inline bool DeathMayDrop(KillRelation relation, int level) {
    return level > 10 && (relation == KillRelation::GuildWar || relation == KillRelation::Monster || relation == KillRelation::Open);
}
// Whether the murderer's numbers apply to a death: the victim has points and the relation is not 5 (a
// murderer killed by a player in the open field takes the ordinary player-kill numbers).
inline bool MurdererNumbers(std::int32_t points, KillRelation relation) {
    return points > 0 && relation != KillRelation::Open && relation != KillRelation::Job && relation != KillRelation::Team &&
           relation != KillRelation::FortressWar;
}
// The skill points a murderer's death takes: 20 to a player, 60 to a monster.
inline std::uint32_t MurdererSkillPointLoss(bool byPlayer) { return byPlayer ? 20u : 60u; }

// The experience a death takes:
// above level 10 the least of the level's experience x rate and its monster experience x factor, at least 1 (none when
// the share is under 1), less the percent of 0x101 it has:
//   by a player 0.4 % and x 20; by a monster 2 % and x 100;
//   a murderer by a player 2 % and x 100; by a monster 6 % and x 300;
//   in a fortress's area at war the factor x 4 by a player, x 20 by a monster;
//   a thief monster's kill halves the cap (x 0.5).
struct DeathFacts {
    int level = 1;
    std::uint64_t levelExperience = 0;
    std::uint32_t monsterExperience = 0;
    double reducePercent = 0.0;
    bool byPlayer = false;
    bool murderer = false; // MurdererNumbers
    bool fortressWar = false;
    bool thiefMonster = false;
};
std::uint64_t DeathExperienceLoss(const DeathFacts& facts);

// Which worn place a murderer's death rolls first: `roll13` a draw of 0..12, `roll6` one of 0..5 used when the first
// lands on the weapon (6), the job suit (8) or a
// shield place holding ammunition (7).
int MurdererEquipmentPlace(int roll13, int roll6, bool ammunitionInShield);

// --- The killer's reward ----------------------------------------------------------------------------------

// The level-gap factor by d = victim's level - killer's: d -3..10 1.0;
// above 10 1 - 0.02 x (d - 10); -6..-4 0.9; -7 or less 1 - 0.15 x (-d - 3); held to 0.1..1.
double PvpLevelGapFactor(int victimLevel, int killerLevel);
// The party factor: 1.0 when the victim is 3 or more levels below;
// else 1 + a bonus in an exp-shared party (European 0.9 / 0.6 / 0.3 at d >= 9 / 7 / 5, Chinese 0.45 / 0.3 / 0.15; floats
// + 0.03 x min(d + 3, 13), held to 1..4.
double PvpPartyFactor(int victimLevel, int killerLevel, bool expSharedParty, bool european);
// The killer's experience: the lower level's monster experience x the gap
// factor x the killer's rate (ours the shard's experience rate) x the party factor, kept as a float (1 when that is 0),
// cut to a whole number; doubled when the victim was a murderer and the relation is not 5.
std::uint64_t PvpExperience(std::uint32_t lowerMonsterExperience, int victimLevel, int killerLevel, double rate, bool expSharedParty,
                            bool european, bool doubled);

// --- The clocks -------------------------------------------------------------------------------------------

// The PK keeper (`_TimedJob` 2/0): `secondsLeft` of the period running at
// `stamp`, its TotalPK; after `elapsed` more seconds each whole period passed takes one off and starts the next; past the
// last both TotalPK and the points are cleared (`cleared`) and the keeper ends. Online and offline alike (the catch-up
// at log-in is the same walk).
struct KeeperStep {
    int totalPk = 0;
    std::int64_t secondsLeft = 0; // of the period running now (0 with the keeper ended)
    int taken = 0;                // TotalPK taken off
    bool cleared = false;         // TotalPK reached 0: the points cleared too, the keeper gone
};
KeeperStep KeeperAfter(int totalPk, std::int64_t secondsLeft, std::int64_t elapsed);

// --- The attack check -------------------------------------------------------------------------------------

// The modes (the attack check's argument): 0 a chosen target (an attack, a
// skill's target, a COS's command); 1 skill objects and a non-player's area (no new victims); 3 the targets a player's
// area attack gathers (no new victims; an innocent's area never hits an assaulter).
enum class AttackMode : std::uint8_t { Chosen = 0, Object = 1, Area = 3 };

inline constexpr std::uint8_t kEventFlagWorld = 0x20, kEventArena = 0x40;

// One side of a player-on-player attack, as the check reads it.
struct PvpSide {
    int level = 1;
    PvpState state = PvpState::Neutral;
    bool hasRecords = false; // holds hostility records (an assaulter's)
    int dailyPk = 0;
    std::int32_t points = 0;
    bool battlefield = true; // stands on a battlefield region (not a town)
};
// What else the two share, each a rule of the check's order.
struct PvpPair {
    PvpSide striker, target;
    bool sameParty = false;
    bool fortressFoes = false; // the fortress war's sides (during a war)
    bool freePvpFoes = false;  // foes in a free PvP (a team's or a party's)
    game::proto::SkillUseStatus safeTrade = game::proto::SkillUseStatus::Ok; // the one-star safe trade's refusal (0x3024 / 0x3006)
    bool guildWar = false;     // guilds at war
    bool pkProhibited = false; // the server's PK switch (ours a shard setting)
    bool hostileJobs = false;  // hostile jobs, both in suits
    bool jobLock = false;      // the striker's 10 s suit lock against a hostile job (0x3019)
    bool strikerHolds = false; // the striker holds a record on the target
    // The original's legal relations of the events (its table):
    // kEventFlagWorld (0x20) two players of one Capture the Flag match on different teams, kEventArena (0x40) the Battle
    // Arena's; 0 none. Foes at once (no other rule asked), no hostility records, no PK; their kills are KillRelation::Team.
    std::uint8_t eventRelation = 0;
    AttackMode mode = AttackMode::Chosen;
};
struct AttackVerdict {
    bool allowed = false;
    game::proto::SkillUseStatus code = game::proto::SkillUseStatus::Target; // why not (Target: refused without a code)
    bool pk = false; // a PK attack on an innocent (allowed; he becomes an assaulter)
};
// The order: not both on a battlefield 0x3018; one party 0x3022; fortress or free PvP foes allowed; safe trade's code;
// guilds at war allowed; the PK switch 0x3025; hostile jobs allowed (in the suit lock 0x3019); an area's innocent on an
// assaulter refused; a natural enemy allowed; a record held allowed; an area's new victim refused; the striker below 20
// 0x3016; the target below 20 0x3017; the striker's points at the limit 0x3015; his DailyPK at 15 0x3014; else a PK
// attack.
AttackVerdict AttackCheck(const PvpPair& pair);
// The natural enemy of a player for another: both level 20 or more and the
// target has hostility records, is a murderer, of a hostile job, or of a guild at war with the striker's.
bool NaturalEnemy(const PvpPair& pair);
// Whether a hit of `pair` sets the striker's record on the target: not one
// party; not a legal foe (fortress, free PvP, hostile jobs, guild war; in the open field both 20+ and the target a
// murderer); not a natural enemy without records of his own (a hostile job, a guild at war). An assaulter struck gives
// one: striking an assaulter makes one an assaulter too.
bool HitMakesRecord(const PvpPair& pair);
// Whether a kill of relation 5 counts as a murder: the victim was not the killer's natural enemy.
bool KillIsMurder(const PvpPair& pair);

} // namespace sr::game
