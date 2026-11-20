#pragma once

// Buffs: what a buff changes on its target's numbers (each tag into the parameter keeper: game/Stats.h), and which
// buff may stand beside which. Headless; the server applies them, the client shows the board and the numbers it is told.

#include <cstdint>
#include <span>
#include <vector>

#include "sr/game/Stats.h"

namespace sr::assets {
struct SkillRow;
}

namespace sr::game {

// The target's numbers a buff takes a share of when it starts (the keeper's values then): STR and
// INT ('stri', 'inti' are capped by a percent of them), the defences ('defp' capped the same), the
// shield's own physical defence ('spda' turns a share of it into attack; 0: no shield).
struct BuffBasis {
    float strength = 0, intelligence = 0;
    float phyDefence = 0, magDefence = 0;
    float shieldDefence = 0;
};

// What a buff carries into its 'stri', 'inti' and 'defp' (the Cleric's blesses' passives fill them as it is cast), added
// before their caps.
struct BuffBonus {
    float strength = 0, intelligence = 0;
    float phyDefence = 0, magDefence = 0;
};

// The changes a buff skill's tags make, in the original's order. Tags of other steps are
// left out (the named values, 'setv': game/Passives.h; links, stealth, barriers: K12).
std::vector<StatChange> BuffChanges(const assets::SkillRow& skill, const BuffBasis& basis, const BuffBonus& bonus = {});

// The overlap groups of a skill: the low three bytes of its Action_Overlap column (skilldata col 18;
// the top one is its wait's group) and of its 'ovl2', but 0, 0x1D and 0x23 (a lasting effect holds its groups while it
// runs).
std::vector<std::uint8_t> OverlapGroups(const assets::SkillRow& skill);

// Whether a buff may start beside the ones running, and which of them it ends:
// - another rank of the same skill group: it replaces one of a lower or equal level, a higher one
//   refuses it;
// - the same 'dttp' family: its rank (second word) the same way;
// - otherwise one running in an overlap group it shares refuses it.
struct BuffAdmission {
    bool admitted = false;
    std::vector<std::size_t> replaces; // indices into `running`
};
BuffAdmission AdmitBuff(const assets::SkillRow& incoming, std::span<const assets::SkillRow* const> running);

} // namespace sr::game
