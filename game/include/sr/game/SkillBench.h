#pragma once

// The client's skill bench (K1): a made-up session in which skills are cast one
// after another, for the game screen to play as it plays a recorded one (--screen game
// --skill-bench FILTER). A Chinese caster (the player's character) and a European one beside it
// take each skill's weapon in hand (a change of the weapon place, as the server tells it) and cast
// it on a monster in front of them (a buff on themselves), its hits told as the server tells
// them; then the next, after the skill's own time and a pause. An imbue is followed by a basic attack
// of the weapon in hand whose hit carries it (its hit effect, K5).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sr/game/WorldRecording.h"

namespace sr::assets {
class CharacterTable;
class ItemTable;
class SkillTable;
struct SkillRow;
} // namespace sr::assets

namespace sr::game {

struct SkillBenchStep {
    double at = 0;          // the swing, seconds into the session
    double until = 0;       // when the next step starts
    double capture = 0;     // when its picture is taken (half a second in; a skill giving states once they show)
    std::uint32_t skill = 0;
    std::uint32_t caster = 0;
    std::uint32_t weapon = 0; // the item in hand (0: none, or the one held already)
    std::string codename;
};

struct SkillBench {
    WorldRecording recording;
    std::vector<SkillBenchStep> steps;
    // The entities: the player's character (Chinese), the European caster, the monster.
    static constexpr std::uint32_t kChinese = 1, kEuropean = 3, kTarget = 2;
};

// How the bench stands: the weapon kind a skill that takes it is cast with (itemdata TID4, the
// glaive 5 for a spear skill; 0: the first it takes), how far from the monster its casters stand (0:
// each skill's reach).
struct SkillBenchOptions {
    int weaponKind = 0;
    float reach = 0;
    // A state (game::StateKind) put on the monster through each step, to see its look (0: none).
    int state = 0;
    // The weapons' plus (their glow and, from +8, their sparks).
    std::uint8_t plus = 0;
    // A weapon by codename held in place of the first one of its kind, for the skills it fits (e.g. a rare one, its
    // seal's glow: itemrare.txt), and in hand for the skills that take any weapon (its race's; empty: none).
    std::string weapon;
    // The hits told this much later than the skill's preparing and casting (the server's impact on its 300 ms skill
    // tick comes 0..300 ms late: the client's shot waits for it).
    int impactDelayMs = 0;
    // Seconds of rest after each skill (the videos': its effects play out before the next starts).
    double pauseSeconds = 0.0;
    // Part `part` (0-based) of `parts`: every parts-th skill of those the filter takes (clients rendering side by side).
    int part = 0, parts = 1;
};

// Every active skill on a board whose codename contains `filter` ("all": every one; a mastery's
// name key, "UIIT_STT_MASTERY_VI": that mastery's), each group by its first level, in the boards'
// order. False (and why) when the tables lack the bodies, the monster or any skill.
bool MakeSkillBench(const assets::SkillTable& skills, const assets::ItemTable& items, const assets::CharacterTable& characters,
                    std::string_view filter, SkillBench& out, std::string& error, const SkillBenchOptions& options = {});

} // namespace sr::game
