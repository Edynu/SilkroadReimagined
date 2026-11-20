#pragma once

// A character's masteries and skills (C7f) and the rules for raising and learning them, shared by
// the server (which decides) and the client (which shows what may be pressed). The numbers follow
// the original server and its tables:
//
// - A mastery is a weapon or force line (skillmastery.txt) a race may train: one whose board holds
//   skills of the character's race (skilldata col 47: 0 China, 1 Europe, 3 any). Its first level is
//   free; each next costs leveldata's col 2 of the level it is at, in skill points. A mastery's level
//   never passes the character's; all masteries together stay within the race's allowance (China 330,
//   Europe 2 a character level, at most 240).
// - A skill is learned a level at a time (the group's next row): its skill points (col 46), its
//   masteries at their levels (cols 34..37), its prerequisite groups at their levels (cols 40..45),
//   STR and INT (cols 38, 39), its race.

#include <cstdint>
#include <map>
#include <vector>

namespace sr::assets {
class SkillTable;
struct SkillRow;
} // namespace sr::assets

namespace sr::game {

struct SkillBook {
    std::map<std::uint32_t, int> masteries; // mastery id -> level
    std::map<std::uint32_t, int> groups;    // skill group -> the level learned
    // The skills what it wears gives it (its sockets' stones), skilldata ids: the passives run
    // while worn, the actives are cast as learned ones. Never saved: made again from the worn items.
    std::vector<std::uint32_t> itemSkills;
    int MasteryLevel(std::uint32_t mastery) const;
    int GroupLevel(std::uint32_t group) const;
    int MasteryTotal() const;
    bool Knows(const assets::SkillRow& skill) const; // its group at its level or higher, or an item's skill
    // The learned rows (each group's level's skilldata id).
    std::vector<std::uint32_t> Skills(const assets::SkillTable& table) const;
};

enum class SkillCheck : std::uint8_t {
    Ok = 0,
    NoSkillPoints = 1,  // not enough skill points
    MasteryLevel = 2,   // a mastery too low (or the mastery past the character's level)
    MasteryTotal = 3,   // the race's allowance used up
    Race = 5,           // not this race's
    Prerequisite = 6,   // a prerequisite skill not learned (or too low)
    Unknown = 9,        // no such mastery or skill, or not the group's next level
    Learned = 10,       // already learned
    Strength = 11,      // too little STR (col 38)
    Intelligence = 12,  // too little INT (col 39)
};

// What a skill does, from its parameter tags and its target columns (the
// catalogue; for the tags). A skill may do several things (an
// attack that stuns, a buff that heals); every player skill on a board has at least one.
struct SkillTraits {
    bool passive = false;   // col 8 = 0: always on once learned
    bool attack = false;    // an 'att' that is not an imbue's, or 'pdmg' damage of its own (a trap's waits for K12)
    bool area = false;      // 'efr': it strikes an area
    bool chain = false;     // col 9: several hits, each its own row
    bool states = false;    // it gives states on its target (stun, knock-down, burn, a curse, ...)
    bool imbue = false;     // IsImbue: its force and its states join the caster's hits while it lasts
    bool buff = false;      // 'dura' and no attack: it changes its target for a time
    bool debuff = false;    // states or curses without an attack: on an enemy, or in an area
    bool toggle = false;    // 'onff': on until turned off, burning MP
    bool heal = false;      // 'heal'
    bool cure = false;      // 'cura', 'curt', 'curl'
    bool resurrect = false; // 'resu' (cast on a dead body)
    bool harmony = false;   // 'pola': monsters up to a level do not attack first in its area
    bool taunt = false;     // 'tant', 'tnt2', 'dtnt'
    bool stealth = false;   // 'hide'
    bool detect = false;    // 'dtt', 'dttp'
    bool teleport = false;  // 'tele', 'tel2', 'tel3', 'tel4'
    bool transform = false; // 'msch', 'mcap'
    bool summon = false;    // 'summ'
    bool trap = false;      // 'trap'
    bool link = false;      // 'lnks': shared with the party members linked
    bool reflect = false;   // absorption and reflection ('lkdr', 'lkag', 'odar', 'pwtt' .. 'mwhs')
    // Whom it is cast on (cols 22, 26..33).
    enum class Target : std::uint8_t { Self, Party, Ally, Enemy, DeadBody, Area } target = Target::Self;
    bool Any() const;
};
SkillTraits TraitsOf(const assets::SkillRow& skill);
// An imbue: a lasting effect (its 'dura') of Basic_Activity 1 that carries an
// 'att' (puts such an effect in its holder's imbue place). The Chinese forces'
// 15 groups and the Rogue's two poison blades; the Warlock's curses and the Bard's explosion (an
// 'att' of the magical lane too, but Activity 2) are attacks, the Wizard's fire traps traps.
bool IsImbue(const assets::SkillRow& skill);

// The most all masteries may add up to. country: 0 China, 1 Europe. China 330 (the 1.188 server refuses a total past
// it with 0x3805; v1.150's client showed 300); Europe twice the level, at most 240.
int MasteryAllowance(int country, int level);
bool MasteryOfRace(const assets::SkillTable& table, std::uint32_t mastery, int country);
// The skill points the mastery's next level costs (`masteryCosts`: leveldata col 2 by level).
std::uint32_t MasteryCost(const std::vector<std::uint32_t>& masteryCosts, int current);
SkillCheck CanRaiseMastery(const SkillBook& book, const assets::SkillTable& table, const std::vector<std::uint32_t>& masteryCosts,
                           std::uint32_t mastery, int country, int level, std::uint32_t skillPoints);
SkillCheck CanLearnSkill(const SkillBook& book, const assets::SkillTable& table, const assets::SkillRow& skill, int country,
                         std::uint32_t skillPoints, int strength, int intelligence);

} // namespace sr::game
