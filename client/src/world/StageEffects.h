#pragma once

// What the game's events start in a WorldScreen's effect system (V1,
// section 5), on its stage characters by slot: shared by the game screen (play/PlayEffects.cpp, which
// maps entities to slots) and the world screen's test walkers (screens/WorldCrowd.cpp).
//
// - A skill's stages (skilleffect.txt "skilleffectset", assets::SkillEffectStage) on its caster and
//   target: each stage's .efp at its phase's start (READY at the swing, WAIT after the preparing
//   time, SHOT after the casting time; a stage started by an animation event later in the shot, at
//   0.3 of it as the arrows' release, ours until the clips' events are read), on the stage's bone
//   with its offset and turn. AT_LOOP stages last until their phase ends or a later stage kills
//   their slot; AT_ONE_FOLLOW ones follow the caster; AT_TARGET / AT_TARGET_F / AT_DMG_POS ones
//   stand on the target; AT_MOV_* ones fly from the caster's bone to the target at the stage's
//   speed (MOV_UP / MOV_UPR arcing 5 % of the way, as the arrows), their arrival effect at the end.
// - A stage's model (.bsr: an arrow, a thrown sword), shown
//   as one of the world screen's models in flight (at full effect quality, as the original): held
//   on its bone from its start until its phase ends or a later stage kills it or takes it over
//   (the nocked arrow, until the shot's arrow flies), or flying as the effects above. A stage with
//   SCT_ARROW hangs the skill's trail and force effects (skillaniset2 cols 19, 20) on the model's
//   Bone01 while it is held or flies: the bow skills' glow (268 of the 4,037 player skills show
//   nothing else).
// - Its sounds (K8): each stage's SndBegin as it starts, its SndEnd as it
//   ends (a loop at its end, a flight where it lands, another at its start plus its life); cues the
//   screen takes and plays where they are (StageEffects does not play sound).
// - A buff's ACT_S stages once, its ACT_L stages kept while it lasts.
// - The glow of rare weapons and shields: itemrare.txt's effects
//   on the item model's own bones (sockets of the dressed model), for a rarity-2 item in the weapon's
//   or shield's place.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sr/assets/SkillAnimation.h"
#include "sr/gfx/Effects.h"

namespace sr::assets {
class FileSystem;
class ItemTable;
class ItemGlowTable;
struct SkillRow;
} // namespace sr::assets

namespace sr::client {

class WorldScreen;

class StageEffects {
public:
    using Stage = assets::SkillEffectStage;
    using Stages = std::vector<Stage>;
    using Handle = gfx::EffectSystem::Handle;
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    static constexpr int kReady = 0, kWait = 1, kShot = 2, kActOnStart = 3, kActOnLoop = 4, kActOnEnd = 5, kActStart = 6, kActLoop = 7, kDeact = 8;

    // A skill's stage list: by its codename (or codename + "_01"), else its animation row's (by its
    // base name, a monster skill's by its codename). Null when it has none.
    static const Stages* StagesOf(const assets::SkillAnimationTable& table, const assets::SkillRow& skill);
    static bool HasLoop(const Stages& stages);
    // Where an effect hangs on a slot's character.
    static gfx::EffectSystem::Anchor Anchor(std::size_t slot, const Stage::Anchor& a, bool follow);
    // Where hits land on a body (characterInfo), else its middle.
    static Stage::Anchor HitAnchor(const assets::CharacterEffectInfo* info);

    // A swing's phases as the caster's body plays them, in seconds from the cast (PlayScreen::Fight
    // moves the body by it): a phase whose clip the skill's row does not name takes no time of its
    // own, the next clip starting in its place with that time inside it. A melee skill names no wait
    // clip: its casting time runs in its shot clip, the clip's time to the blow (its hit moment: 0.410
    // of SKILL_CH_SWORD_SMASH_A's 0.411, 0.658 of SMASH_C's 0.657, 0.760 of SPECIAL's 0.760).
    // A one-shot clip holds its first frame for its 200 ms blend-in and only then runs: its events
    // (and its end) come kClipHold later (A 2).
    static constexpr double kClipHold = 0.2;
    struct Swing {
        double wait = 0, shot = 0, end = 0; // when the wait and the shot phases start, when the shot ends
        bool readyClip = false, shotClip = true; // which phases play a one-shot clip (their events held)
        // From the skill's preparing, casting and shot seconds (the shot's clip length when known) and
        // which of the first two have a clip.
        static Swing Of(double ready, double wait, double shot, bool readyClip, bool waitClip);
    };

    // When a stage starts in its swing (seconds from the cast): its phase's start, or the shot clip's
    // hit moment its start event names (`shotEvents`: WorldScreen::StageEvents; none: 0.3 into the shot).
    static double StageStart(const Stage& s, const Swing& swing, std::span<const double> shotEvents);
    // A swing's damage moments (seconds from the cast) in the order its hits show: each distinct phase
    // and start event of its stages that show damage (col 4), by phase, then event. The 1.188 client
    // hands its Nth result to the Nth (a damage group takes the next of the results).
    // A damage group's hit is carried by its first projectile and shown as it lands:
    // its moment is then the projectile's arrival (`distance` the caster to the target).
    static std::vector<double> DamageMoments(const Stages& stages, const Swing& swing, std::span<const double> shotEvents,
                                             float distance = 0.0f);

    // What a cast's stages need of it beyond the swing: the shot clip's hit moments (seconds into it,
    // WorldScreen::StageEvents), the caster's weapon model (the WEAPON object), the target's hit
    // anchor (its characterInfo DamageBone / DamagePos), the weapon's kind and the shot clip's state
    // (SCT_RUT's automatic angle), the caster to target distance (a projectile's arrival).
    struct Cast {
        std::span<const double> shotEvents;
        std::string_view weapon;
        Stage::Anchor targetHit;
        int weaponKind = 0;
        std::uint32_t shotState = 0;
        float distance = 0;
    };
    // A flight's speed: the middle of its stage's two (the original draws one between them).
    static float SpeedOf(const Stage& s);

    // An effect homing from one slot's body to another's (a kill's orbs): after a pause of its own it
    // flies on a sideways curve to where the target is then, following it. `seed` varies the pause and curve.
    void Homing(WorldScreen& world, std::size_t from, std::size_t to, const std::string& effect, double now, std::uint32_t seed,
                const std::string& arrival = {}, const std::string& landSound = {});

    // A skill's stages from its swing; `target` kNone for none (the caster). `actStart`: its buff start
    // (ACT_S) too, at the shot (another's buff: one's own comes with the buff itself). `animation`: the
    // skill's skillaniset2 row (its trail and force for SCT_ARROW), null when it has none.
    // `shotEvents`: the shot clip's hit moments (seconds into it, WorldScreen::StageEvents): a stage
    // started by event N starts at the Nth (none: 0.3 into the shot).
    // `weapon`: the caster's weapon model (.bsr), flown by the stages whose object is "WEAPON" (the
    // spear's and glaive's throws). An AT_LOOP stage lasts to the cast's end (`swing.end`) unless a
    // later stage kills its slot (1.188's presentation end clears them).
    // `part`: all its stages, or those before its shot (READY, WAIT) now and its shot's (SHOT, the cast's ACT_S) when
    // the impact comes (the 1.188 client starts SHOT at the server's impact message): the shot's call gets the swing's
    // own cast number (`castNo`: its held models
    // traded into its flights) and a `now` moved by the impact's delay. Its cast number.
    enum class Part : std::uint8_t { All, BeforeShot, Shot };
    std::uint32_t Skill(WorldScreen& world, const Stages& stages, std::size_t caster, std::size_t target, double now, const Swing& swing,
                        bool actStart, const assets::SkillAnimation* animation, const Cast& cast, Part part = Part::All, std::uint32_t castNo = 0,
                        double clock = -1.0);
    // A buff's start (ACT_S) once on a slot, and its loops (ACT_L) kept until released (their handles).
    // A state's (skilleffect.txt PARAM_<code>) the same: its loops while it lasts,
    // its end (DEACT) once when it ends.
    // A buff's stages carry models too (the bow's hawks over the caster, balloons, flags): each an
    // animated stage character on a slot set aside for them (SetCompanionSlots), placed on its
    // anchor every frame (Fly); a loop's until released, a start's or an end's for its life.
    // Whether a buff's holder shows its owner's form: ACT_OS / ACT_OL / ACT_OE instead of ACT_S / ACT_L /
    // DEACT (the Bard's auras, the Cleric's group recovery: the one playing it shows its other
    // form). Every holder is its owner while there are no party copies (ours: the
    // copies' ACT_S / ACT_L come with the parties).
    static bool OwnerForm(const Stages& stages);
    // A stage's SCT_EFFECT_SCALE (its effect's scale; 1 none).
    static float EffectScaleOf(const Stage& s);
    // A list's SCT_CHAR_SCALE,<scale>,<ms> (the 1.188 client's script flag 0x1000; berserk's modes: the body 1.1 or 1.2
    // times its size over a second): false when none of its stages has one.
    static bool CharScaleOf(const Stages& stages, float& scale, float& ms);
    void BuffStart(WorldScreen& world, const Stages& stages, std::size_t slot);
    std::vector<Handle> BuffLoops(WorldScreen& world, const Stages& stages, std::size_t slot);
    void StateEnd(WorldScreen& world, const Stages& stages, std::size_t slot);
    // Ends a loop BuffLoops gave: an effect fades out, a carried model goes.
    void Release(WorldScreen& world, Handle handle);
    // A skill object (the server's entity of a skill with an area of kind 3): its effect where it stands
    // (skilleffect's column 22), kept until released; and its strike there (a trap going off, a field's
    // pulse: its S_RETURN stages' effects, sounds and shakes; B 5.4).
    Handle ObjectEffect(WorldScreen& world, const std::string& path, const glm::vec3& at, float yaw);
    // A companion's strike (the Bow's hawk, a buff's SCT_MOVER model; 1.188 CIDPHawk): the
    // holder's hawk flies to the target, attacks it and lingers, then comes back over its holder. The
    // seconds until its attack lands (the hit then shows); negative when the holder shows no hawk.
    double Strike(WorldScreen& world, std::size_t holder, std::size_t target, double now);
    void ObjectStruck(WorldScreen& world, const Stages& stages, const glm::vec3& at, std::size_t owner, double now);
    // A strike that is no swing's on a target (proto::kDamageReturn: an aura's pulse, a skill object's):
    // its skill's S_RETURN stages on the target (AT_TARGET, AT_DMG_POS), their sounds and the owner's
    // shake.
    void ReturnHit(WorldScreen& world, const Stages& stages, std::size_t target, std::size_t owner, double now, bool atSource = false);
    // The slots a caster's latest swing struck (PlayScreen's hits): where a splash hops on and
    // AT_SOURCE flies back from. Unset: the swing's own target only.
    std::function<std::vector<std::size_t>(std::size_t caster)> struckBy;
    // The world's stage slots set aside for the carried models: [first, first + count).
    void SetCompanionSlots(std::size_t first, std::size_t count) {
        companionFirst_ = first;
        companionUsed_.assign(count, false);
    }
    // One effect on a slot's anchor (a hit, a blood, a level up).
    Handle Play(WorldScreen& world, std::size_t slot, const std::string& path, const Stage::Anchor& at, gfx::EffectSystem::Kind kind);
    // The effects of the weapons and shields among `worn` (itemdata ids, their pluses beside them in `plus`) on a
    // slot, kept: the rare ones' glows and, from +8, the enchantment's sparks (assets::ItemGlowTable).
    static std::vector<Handle> ItemGlows(WorldScreen& world, std::size_t slot, const assets::FileSystem& files,
                                         const assets::ItemTable& items, const assets::ItemGlowTable& glows,
                                         std::span<const std::uint32_t> worn, std::span<const std::uint8_t> plus = {});

    // Moves the flying stages and the held models; their arrival effects where they land.
    void Fly(WorldScreen& world, double now);
    // The sounds due by `now`: on a slot's character, or (slot kNone) at a point (a flight's landing).
    struct SoundCue {
        std::string path;
        double at = 0;
        std::size_t slot = kNone;
        glm::vec3 point{0.0f};
        std::size_t caster = kNone; // the cast's caster (Cancel)
    };
    std::vector<SoundCue> TakeSounds(double now);
    // The camera shakes due by `now` (SCT_SHAKECAM0..3 as a stage starts, SCT_SHAKECAM_MOV0..3 as its
    // flight lands): the caster's slot and the amplitude, milliseconds and
    // step of the shake (the original's table: 50/500/20, 200/500/25, 300/500/25, 400/500/25).
    struct ShakeCue {
        double at = 0;
        std::size_t caster = kNone;
        float amplitude = 0, ms = 0, step = 0;
    };
    std::vector<ShakeCue> TakeShakes(double now);
    // Forgets what flies and is held (the world that showed them went).
    // A caster's presentation given up: a new swing of its own, its knock-down, its death, the server's cancel (B071 mode
    // 2). Its later stages never run (their effects,
    // sounds, shakes, target models and flights not released yet dropped), its held objects end (an effect's col 12
    // fade, a loop no longer emitting, a held model gone); what already flies flies on.
    void Cancel(WorldScreen& world, std::size_t caster, double now);
    void Clear() {
        planned_.clear();
        flights_.clear();
        rays_.clear();
        held_.clear();
        cues_.clear();
        shakes_.clear();
        companions_.clear();
        companionUsed_.assign(companionUsed_.size(), false);
        targetModels_.clear();
    }

    // A model a stage shows where its target stands (AT_TARGET: Hotan's Blood Devil's roots out of the ground under
    // its target, 114 monster skills' stages so far): its clip played once there on a stage slot of its own
    // (PlayScreen::StageProps), from its stage's start.
    struct TargetModel {
        std::string model;
        std::size_t target = kNone; // the target's stage slot
        double at = 0;              // its stage's start
        std::size_t caster = kNone; // whose cast (Cancel)
    };
    // Those due by `now`, taken out.
    std::vector<TargetModel> TakeTargetModels(double now) {
        std::vector<TargetModel> due;
        std::erase_if(targetModels_, [&](TargetModel& m) {
            if (m.at > now) return false;
            due.push_back(std::move(m));
            return true;
        });
        return due;
    }

private:
    std::vector<TargetModel> targetModels_;
    struct Flight {
        Handle effect = 0;           // an .efp in flight, else 0
        std::uint32_t key = 0;       // a model in flight (WorldScreen::SetFlying), else 0
        std::string model;
        std::vector<std::string> carried; // effects on the model's Bone01 once it flies (SCT_ARROW)
        bool carrying = false;       // they were started
        bool weapon = false;         // the caster's own weapon thrown ("WEAPON": its effects at its origin, not on Bone01's turn)
        std::uint32_t cast = 0;      // the swing it belongs to
        std::size_t owner = kNone;   // its caster (Cancel; `caster` is the target for a mover on it)
        int trade = 0;               // takes over the held model of this stage slot as it flies
        std::size_t caster = 0, target = 0;
        Stage::Anchor from, to;
        double release = 0, arrive = 0;
        float speed = 450.0f, arc = 0.0f, tilt = 0.0f; // arc: its height a share of the length; tilt: radians about the line
        glm::vec3 start{0.0f}, end{0.0f}; // taken at the release
        bool flying = false;
        std::string arrival; // shown where it lands
        std::string landSound; // heard where it lands (its stage's SndEnd)
        int landShake = -1;    // its stage's SCT_SHAKECAM_MOVn as it lands
        // AT_MOV_SPLASH: on to the swing's next struck target as it lands, from where it is.
        bool splash = false, hop = false;
        glm::vec3 hopFrom{0.0f};
        std::string hopEffect; // its .efp, started again for each hop (the one that landed is let go)
        float hopScale = 1.0f;
        std::vector<std::size_t> visited;
        bool homing = false;   // its end follows the caster all the way (a kill's orbs)
        float seconds = 0.0f;  // how long it flies (0: by its speed)
        bool toCaster = false; // AT_SOURCE: from the target to the caster's anchor (`to` on the caster)
        // A mover on the target (AT_TARGET with a speed: the frost's falling ice): from the target's
        // place plus `fromOffset` to it plus `toOffset`, both in the world's axes (from, to unused).
        bool onTarget = false;
        glm::vec3 fromOffset{0.0f}, toOffset{0.0f};
    };
    // A radial flight (AT_MOV_OPTION): a ray out of its caster leaving a copy of its effect every
    // `spacing` (the frost's star).
    struct Ray {
        std::string object;
        std::size_t caster = 0;
        Stage::Anchor from;
        float direction = 0, distance = 0, spacing = 0, speed = 100.0f;
        double release = 0;
        bool started = false;
        glm::vec3 origin{0.0f}, dir{0.0f, 0.0f, -1.0f};
        float next = 0; // where the next copy goes along it
    };
    std::vector<Ray> rays_;
    // A model held on a bone of its caster for a while.
    struct Held {
        std::uint32_t key = 0;
        std::string model;
        std::vector<std::string> carried; // effects on its Bone01 while it is held (SCT_ARROW)
        std::string trail;                // the tail among them: it fades in over 3 s while held
        std::uint32_t cast = 0;
        int slot = 0; // its stage's slot (col 7)
        std::size_t caster = 0;
        Stage::Anchor at;
        glm::mat4 turn{1.0f};
        double from = 0, until = 0;
        bool shown = false;
    };
    std::uint32_t NewKey() { return kKeyBase + (nextKey_++ & 0x0FFFFFFFu); }
    // SCT_ARROW's effects on a model: on its Bone01 with its turn, or at its origin
    // (`atOrigin`: a thrown spear or glaive, whose Bone01 is turned 180 degrees); `fadeTrail` fades that one in over 3 s.
    static void Carry(WorldScreen& world, std::uint32_t key, const std::vector<std::string>& effects, bool atOrigin = false,
                      const std::string& fadeTrail = {});
    void Cue(const std::string& path, double at, std::size_t slot, std::size_t caster = kNone) {
        if (!path.empty() && slot != kNone) cues_.push_back({path, at, slot, {}, caster});
    }
    // The effects a cast started (its stages' own; Cancel): their caster, when each starts, whether it is held.
    struct Planned {
        Handle effect = 0;
        std::size_t caster = kNone;
        double start = 0;
        bool held = false;
        double end = -1.0; // a held one's release (the presentation's end, or a later stage's kill of its slot); -1 none
    };
    std::vector<Planned> planned_;
    // A model a buff's stage carries on its holder: an animated stage character on a set-aside slot.
    struct Companion {
        Handle handle = 0;
        std::size_t slot = kNone, holder = 0;
        std::string model;
        Stage::Anchor at;
        glm::mat4 turn{1.0f};
        double life = -1.0, until = -1.0; // seconds it lasts (negative: until released), its end once shown
        bool shown = false;
        // SCT_MOVER (a hawk): it leaves its anchor to strike. Its state (0 over its holder, 1 flying to
        // the target, 2 attacking, 3 near it, 4 coming back), since when, the target, where it is and
        // which way it faces, the clip it plays (0 standing, 7 flying, 2 attacking: its default set).
        bool mover = false;
        int state = 0;
        double stateAt = 0;
        std::size_t target = kNone;
        glm::vec3 pos{0.0f};
        float yaw = 0;
        std::uint32_t clip = 0;
    };
    double lastFly_ = -1.0;
    // A stage's SCT_MAT on its holder: its model's
    // material from the first colour to the second over the time and back, again and again,
    // until released or `until`. The model holds one colour: the newest on a slot shows
    // (a later SCT_MAT overwrites it, from its first colour), and the end of any of them turns the slot's colour off,
    // the newer's too: `off` a replaced one, kept only for its end.
    struct Tint {
        Handle handle = 0;
        std::size_t slot = 0;
        glm::vec3 from{1.0f}, to{1.0f};
        double ms = 1000, since = 0, until = -1.0;
        bool off = false;
    };
    void EndTint(std::size_t slot); // the slot's colour off: every tint on it gone
    std::vector<Tint> tints_;
    std::vector<std::size_t> tinted_; // the slots tinted last frame
    Handle AddTint(const Stage& s, std::size_t slot, double now, double until);
    // A model stage a buff carries (its phase's own handle; 0 when none or no slot is free).
    Handle AddCompanion(const Stage& s, std::size_t holder, double life);
    std::vector<Companion> companions_;
    std::vector<bool> companionUsed_;
    std::size_t companionFirst_ = 0;
    std::uint32_t nextCompanion_ = 1;
    static constexpr Handle kCompanionHandle = 0x80000000u; // above the effect system's
    std::vector<Flight> flights_;
    std::vector<Held> held_;
    std::vector<SoundCue> cues_;
    std::vector<ShakeCue> shakes_;
    // A stage's shake (mode 0..3 of SCT_SHAKECAMn, `onArrival`: of SCT_SHAKECAM_MOVn); -1 none.
    static int ShakeOf(const Stage& s, bool onArrival);
    void Shake(int mode, double at, std::size_t caster);
    // Models in flight share the world screen's keys with the game screen's own: these from here.
    static constexpr std::uint32_t kKeyBase = 0x40000000u;
    std::uint32_t nextKey_ = 1, casts_ = 0;
};

} // namespace sr::client
