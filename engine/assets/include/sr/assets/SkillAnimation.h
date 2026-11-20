#pragma once

// Which animations a skill (basic attacks are skills too) plays, from textdata
// skilleffect.txt, section "skillaniset2": per row, after
// the service column, 1 the base name (skilldata col 5, or col 3 for monster skills),
// 5 the animation group (the .bsr animation set: DEFAULT, SWORD, SPEAR, BOW, ...),
// 6 / 7 / 8 the ready / wait / shot animations (comma-separated ANI_* names).
// The same row names the hit effects: 12 a blocked hit's, 13 a hit's, 18 a critical's,
// 19 / 20 the arrow's trail / force (.efp under the Particles root). Section
// "skilleffectset" lists a skill's effect stages (rows of 28+ columns sharing column 1,
// the row's base name or base name + "_01"): 2 the phase (READY 0, WAIT 1, SHOT 2, ...),
// 3 the start event (0: the phase begins; N: the Nth code-1 event of its animation),
// 4 damage lands here, 6 scale (none / CHAR_BASE / MOB_BASE), 11 how many, 13 the action
// (AT_*), 14 the move "kind,delay,start speed,end speed", 17 + 18 the object (.efp / .bsr),
// 19 / 21 the start / target bone ("none" the root, "*" the root at the body's height,
// "@Bone" its place only, "Bone" with its turn), 20 / 22 their offsets "x,y,z", 24 the
// object's own turn (see SkillEffectStage), 25 SCT_* scripts, 26 / 27 its sounds at its start / end
// (the table's own header: SndBegin, SndEnd; under prim\snd\). Section "characterInfo": 0 codename, 2 height, 7 the bone hits land on,
// 8 its offset, 9 the blood (hiteffect\<name>.efp).
// AnimationState maps an ANI_* name to its .bsr state id.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sr::assets {

class FileSystem;

struct SkillEffectStage {
    int phase = 2;          // 0 READY, 1 WAIT, 2 SHOT, 3 ACT_OS, ... 9 S_RETURN
    int startEvent = 0;     // 0: as the phase begins; N: at its animation's Nth code-1 event
    bool damageEvent = false;
    int scale = 0;          // 0 none, 1 the character's height, 2 the monster's size
    int createCount = 1;
    int slot = 0;           // col 7: a held object's slot; a later stage whose `trade` names it takes it over
    int trade = 0;          // col 9
    int kill = 0;           // col 10: removes the held object of that slot
    float lifeMs = 0;       // col 16, 5th number: how long a one-shot stays (0: its own length)
    std::string arrival;    // col 23: what appears where a flight ends (.efp / .bsr), empty when none
    std::string action;     // AT_STOP, AT_ONE_FOLLOW, AT_LOOP, AT_TARGET, AT_MOV_1TAR, ...
    std::string move;       // MOV_NONE, MOV_STRAIGHT, ...
    float moveDelayMs = 0, startSpeed = 0, endSpeed = 0;
    std::string object;     // .efp / .bsr (Particles root), empty when none
    // col 24: a turn put on the object in its own space, as the original decodes the number:
    // 1..360 degrees about Y, 361..720 about X (minus 360), above 720 about Z (minus 720).
    // Col 16's radial flight (AT_MOV_OPTION, its first word true): a ray from the start anchor at
    // `rayDirection` degrees from where the caster faces, `rayDistance` long, a copy of the object
    // left every `raySpacing` (the frost's star: six rays 60 apart, 50 long, every 10).
    float rayDirection = 0, rayDistance = 0, raySpacing = 0;
    // Col 12: the fade (ms) its object comes in with and goes out with; a mover's end at its arrival
    // (< 0 gone at once, 0 its effect plays out, > 0 faded).
    float fadeMs = 0, arrivalFade = 0;
    // Col 15: a flight's arc height in thousandths of its length (MOV_UP, MOV_UPR) and MOV_UPR's tilt
    // range about the flight's line, degrees.
    float arc = 0, tiltMin = 0, tiltMax = 0;
    char rotateAxis = 0;    // 'x', 'y', 'z', or 0 for none
    float rotateRadians = 0;
    // Cols 19-22 : a bone's place and turn ("Bone",
    // flags 2), its place with no turn ("@Bone", 0), the offset in its frame as a point with no turn
    // and no stage turn ("#Bone", 6), the root raised to the body's height ("*", 3), the root (none).
    // The offset is in the root's turned frame but for "#Bone".
    struct Anchor {
        std::string bone;       // empty: the model's root
        bool atHeight = false;  // "*": the root raised to the body's height
        bool atMiddle = false;  // ours (no column says it): the root raised to half the body's height
        bool keepTurn = true;   // false for "@Bone" and "#Bone"
        bool inBone = false;    // "#Bone"
        std::array<float, 3> offset{};
    } start, target;
    std::vector<std::string> scripts; // SCT_ARROW, SCT_SHAKECAM, ...
    std::string soundBegin, soundEnd; // cols 26, 27: prim\snd\<path> (.wav), empty when none
};

struct SkillAnimation {
    std::string group; // lower case: "default", "sword", ...
    std::vector<int> ready, wait, shot; // .bsr state ids
    std::string defenseEffect, damageEffect, criticalEffect, arrowTrail, arrowForce; // .efp, empty when none
    int priority = 0;       // col 2: of an attacker's live presentations, the highest with a DamageEfp gives a hit's effect (imbues 2, attacks 0)
    bool overlap = true;    // col 3: anything but FALSE
    // Col 4: 1 the weapon hides at each phase clip's start (the force skills' hand casts); 2 (the throws)
    // it hides only by a WEAPON flight; either shows it again at the presentation's end (1.188
    // A 9).
    std::uint8_t hideWeapon = 0;
    // Col 22: its skill object's effect (.efp; empty when none): Harmony's circle, the poison field, the
    // fire trap, shown where the server's object stands while it does.
    std::string objectEffect;
    // Cols 14..17: its weapon trail (the "mirage", 219 of the skills): its length (ours: the
    // milliseconds a place stays in it), its colour (A,R,G,B; 0,0,0,0 none given: untinted, ours),
    // its blend (ONE: additive, every row) and its texture (textures\<name> in Particles; empty: none).
    // The clip's events 5 and 6 turn it on and off (A 9).
    // Col 23: its spine aim (61 player skills: the bows', the cold's): the axis the caster's spine
    // turns on towards a target above or below it (0 none; x Yaw, z Roll, and the R forms the other
    // way: Yaw (a,0,0), Roll (0,0,-a), Pitch (0,a,0)).
    std::array<float, 3> spineAxis{};
    int trailMs = 0;
    std::uint32_t trailColour = 0xFFFFFFFFu; // RGBA8, red in the low byte
    std::string trailTexture;
    std::vector<SkillEffectStage> stages;
};

// skilleffect.txt "characterInfo": where hits land on a body and its blood.
struct CharacterEffectInfo {
    float height = 0;
    std::string anchorBone;
    std::array<float, 3> anchorOffset{};
    std::string blood; // hiteffect\<name>.efp, empty when none
    bool bringsRain = false; // col 11, bit 0 of its first value: rain falls while it is near (the original's event rain)
};

// The .bsr state of an ANI_* name; -1 when unknown.
int AnimationState(std::string_view aniName);
// The ANI_* name of a .bsr state (its number when it has none).
std::string AnimationStateName(int state);

bool DecodeSkillAnimations(std::string_view utf8Text, std::unordered_map<std::string, SkillAnimation>& out, std::string& error,
                           std::unordered_map<std::string, CharacterEffectInfo>* characters = nullptr,
                           std::unordered_map<std::string, std::vector<SkillEffectStage>>* stageLists = nullptr);

class SkillAnimationTable {
public:
    bool Load(const FileSystem& fs, std::string& error);
    const SkillAnimation* Find(std::string_view baseName) const;
    const CharacterEffectInfo* Character(std::string_view codename) const;
    // Any stage list by its name (or name + "_01"): also those of no skill, such as
    // SYSTEM_LEVELUP, SYSTEM_HPPOTION, SYSTEM_MPPOTION.
    const std::vector<SkillEffectStage>* Stages(std::string_view name) const;
    std::size_t size() const { return rows_.size(); }

private:
    std::unordered_map<std::string, SkillAnimation> rows_;
    std::unordered_map<std::string, CharacterEffectInfo> characters_;
    std::unordered_map<std::string, std::vector<SkillEffectStage>> stages_;
};

// The .bsr state ids of the reactions and motions combat uses.
namespace anim {
inline constexpr int kStand = 0, kWalk = 1, kAttack1 = 2, kDamage1 = 3, kDie1 = 4, kRun = 7, kDamage2 = 9, kDie2 = 18,
                     kDieLoop1 = 36, kDieLoop2 = 37, kPick = 38, kDown = 62, kDownLoop = 63, kDownDamage = 64,
                     kDownUp = 65, kDownDie = 66;
}

} // namespace sr::assets
