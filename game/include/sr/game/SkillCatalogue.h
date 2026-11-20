#pragma once

// The skill lab's list: every board skill group of the game data with all its ranks, its kinds
// (TraitsOf), the path our server's Skills::Use takes for it (SkillRouteOf: Use dispatches on this very function, so
// the list cannot drift from the server), its cast's times and whether its caster stands through them. Headless:
// the skill lab's tool writes it to the database for the site's /dev, the client's F1 Skill lab shows it from its own
// tables, the exam reads its expectations from it.

#include <cstdint>
#include <string>
#include <vector>

#include "sr/game/Skills.h"

namespace sr::assets {
struct MasteryRow;
} // namespace sr::assets

namespace sr::game {

// The path Skills::Use takes for a skill, in the order it checks them: the first that fits wins.
enum class SkillRoute : std::uint8_t {
    Passive,         // col 8 = 0: never cast; its numbers in the sheet while learned and its conditions hold (K6)
    Detection,       // 'dttp': a copy on every player around, revealing a hidden one to all
    Teleport,        // 'tele' (a dash) or 'tel2' (a put) with a distance: to the point asked, at once
    Mask,            // 'mcap': a normal monster's body taken into an item
    Duplicate,       // 'msch' 2: another player's look for the buff's time
    Object,          // 'efr' kind 3: a skill object at the caster's feet (Harmony, the poison field, the fire trap)
    SelfBuff,        // no target asked and a time it lasts, or a toggle: a buff on the caster at once (the imbues too)
    EnemyPlayerBuff, // a buff on an enemy player alone (the Rogue's Scorn, 'hitm': it aims only at the caster meanwhile)
    PlayerBuff,      // a buff on the caster or on another player (walked into its reach first)
    Resurrect,       // a dead player's revival, offered to it (K9)
    Support,         // a heal or a cure (K7)
    Taunt,           // a threat alone ('tnt2', 'dtnt' without an attack): on the monsters its area takes
    Unhandled,       // nothing it does is known to the server: refused (NotYet)
    Around,          // states around the caster, no target asked (the cold's freezing ground, the rogue's poison round)
    Attack,          // on a target: walked into its reach, then the next swing (Skills::Fight)
};
inline constexpr int kSkillRoutes = int(SkillRoute::Attack) + 1;
SkillRoute SkillRouteOf(const assets::SkillRow& skill);
const char* SkillRouteName(SkillRoute route); // "attack", "self-buff", ...: the list's and the site's
const char* SkillRouteText(SkillRoute route); // what our server does with it, in a line

// How its caster moves while it is cast.
enum class SkillMotion : std::uint8_t {
    None,   // a passive: never cast
    Stands, // through the whole hold: preparing, casting and every row of its chain
    Free,   // an imbue (Basic_Activity 1): at once, never on the action list, the caster goes on
    Moves,  // the skill moves its caster itself: a teleport, a charge ('tel3', after its hits)
};
SkillMotion SkillMotionOf(const assets::SkillRow& skill);
const char* SkillMotionName(SkillMotion motion);

// A cast's times from the data (skilldata cols 11..14, 18): preparing, casting, each row's duration (the skill's own,
// then each its chain adds through col 9), the wait and its group.
struct SkillTimes {
    int preparingMs = 0, castingMs = 0;
    std::vector<int> rowMs;
    std::vector<int> rowLeadMs; // each row's own preparing + casting (the head's first)
    int reuseMs = 0;
    int cooldownGroup = 0;
    // Preparing, casting and every row's duration: the presentation's length.
    int HoldMs() const;
    // How long the original holds its caster from the cast: a command's budget of 500 ms; a row with a next waits its
    // duration less what is left of the budget (the
    // budget used up first; a row no longer than what is left waits 0, used does not grow) and its own release; the
    // last row holds its preparing, casting and duration from its cast, plus the budget used when the basic attack
    // follows (`handsOn`: AutoAttackType not 0) and nothing waits. Each release may come up to a skill tick (300 ms)
    // later than this (the impact on the caster's tick).
    int CycleMs(bool handsOn) const;
    int LeadRows() const; // the rows with a preparing or a casting (each may add a tick)
};
SkillTimes TimesOf(const assets::SkillTable& skills, const assets::SkillRow& skill);

// The rows a chain adds after the skill (col 9, one after another; at most 16, as the server casts them).
std::vector<const assets::SkillRow*> ChainOf(const assets::SkillTable& skills, const assets::SkillRow& skill);

// The hits one cast strikes its selected target with: each row's 'mc' count (1 without), over the chain; a row strikes
// with an 'att', its own damage ('pdmg') or a life steal ('lfst'). A damage over time's pulses are its link's (not counted).
int HitsOf(const assets::SkillTable& skills, const assets::SkillRow& skill);

struct SkillCatalogueEntry {
    std::string code;                           // the group's code: its first rank's base name (SKILL_CH_BOW_CALL_A)
    const assets::MasteryRow* mastery = nullptr; // the board it is on
    std::vector<const assets::SkillRow*> ranks; // its levels, the first first
    SkillTraits traits;                         // the first rank's (TraitsOf)
    SkillRoute route = SkillRoute::Unhandled;   // the first rank's
    SkillMotion motion = SkillMotion::None;
    const assets::SkillRow& First() const { return *ranks.front(); }
    const assets::SkillRow& Top() const { return *ranks.back(); }
};

// Every in-service board group of every mastery (each mastery's board in its order), as the catalogue's tests and
// the client's skill window count them (468 in 1.188).
std::vector<SkillCatalogueEntry> BuildSkillCatalogue(const assets::SkillTable& skills);

// A group's code from one of its rows: the base name (col 5), else the codename less its "_NN" rank.
std::string SkillGroupCode(const assets::SkillRow& skill);

} // namespace sr::game
