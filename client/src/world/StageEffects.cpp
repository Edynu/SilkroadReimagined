#include "world/StageEffects.h"
#include "sr/core/Log.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>

#include "screens/WorldScreen.h"
#include "sr/assets/Dress.h"
#include "sr/assets/GameData.h"
#include "sr/assets/ItemGlow.h"
#include "sr/core/Path.h"

namespace sr::client {

namespace {

bool IsEffectFile(const std::string& path) { return core::NormalizeAssetPath(path).ends_with(".efp"); }
bool IsModelFile(const std::string& path) { return core::NormalizeAssetPath(path).ends_with(".bsr"); }

// A world matrix as the rows SetFlying takes.
std::array<float, 12> Rows(const glm::mat4& m) {
    return {m[0][0], m[1][0], m[2][0], m[3][0], m[0][1], m[1][1], m[2][1], m[3][1], m[0][2], m[1][2], m[2][2], m[3][2]};
}

// Where a stage's anchor is on a slot's character now, as its effects are placed.
bool AnchorPlace(WorldScreen& world, std::size_t slot, const assets::SkillEffectStage::Anchor& a, glm::mat4& out) {
    return world.PlaceEffect(StageEffects::Anchor(slot, a, true), out);
}

// The skill's trail and force (skillaniset2 cols 19, 20) when a stage has SCT_ARROW.
std::vector<std::string> ArrowEffects(const assets::SkillEffectStage& s, const assets::SkillAnimation* animation) {
    std::vector<std::string> out;
    if (animation == nullptr || std::find(s.scripts.begin(), s.scripts.end(), "SCT_ARROW") == s.scripts.end()) return out;
    for (const std::string* path : {&animation->arrowTrail, &animation->arrowForce}) {
        if (IsEffectFile(*path)) out.push_back(*path);
    }
    return out;
}

// A stage's turn (skilleffect col 24) in its anchor's frame, put on before the anchor's.
// The 1.188 client builds it by axis (y, x, z): about x and z the turn
// of D3D (and glm) by the angle, about y by minus the angle (the bow's nocked arrow, 90 on y on
// Bip01 R Hand, whose x points where the archer faces, points its tip, -z, ahead). A point in a
// bone's frame ("#Bone") is not turned.
glm::mat4 StageTurn(const assets::SkillEffectStage& s, const assets::SkillEffectStage::Anchor& at) {
    if (s.rotateAxis == 0 || at.inBone) return glm::mat4(1.0f);
    if (s.rotateAxis == 'y') return glm::rotate(glm::mat4(1.0f), -s.rotateRadians, glm::vec3(0, 1, 0));
    return glm::rotate(glm::mat4(1.0f), s.rotateRadians, s.rotateAxis == 'x' ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1));
}

} // namespace

const StageEffects::Stages* StageEffects::StagesOf(const assets::SkillAnimationTable& table, const assets::SkillRow& skill) {
    if (const auto* stages = table.Stages(skill.codename); stages != nullptr && !stages->empty()) return stages;
    const bool named = !skill.baseName.empty() && skill.baseName != "xxx";
    const assets::SkillAnimation* anim = table.Find(named ? skill.baseName : skill.codename);
    return anim != nullptr && !anim->stages.empty() ? &anim->stages : nullptr;
}

bool StageEffects::HasLoop(const Stages& stages) {
    return std::any_of(stages.begin(), stages.end(), [](const Stage& s) { return s.phase == kActLoop && IsEffectFile(s.object); });
}

gfx::EffectSystem::Anchor StageEffects::Anchor(std::size_t slot, const Stage::Anchor& a, bool follow) {
    gfx::EffectSystem::Anchor out;
    out.target = WorldScreen::StageAnchor(slot);
    out.bone = a.bone;
    out.atHeight = a.atHeight;
    out.atMiddle = a.atMiddle;
    out.keepTurn = a.keepTurn;
    out.inBone = a.inBone;
    out.offset = glm::vec3(a.offset[0], a.offset[1], a.offset[2]);
    out.follow = follow;
    return out;
}

StageEffects::Stage::Anchor StageEffects::HitAnchor(const assets::CharacterEffectInfo* info) {
    Stage::Anchor at;
    if (info != nullptr && !info->anchorBone.empty()) {
        at.bone = info->anchorBone;
        at.offset = info->anchorOffset;
    } else {
        at.atMiddle = true;
    }
    return at;
}

StageEffects::Swing StageEffects::Swing::Of(double ready, double wait, double shot, bool readyClip, bool waitClip) {
    // READY's clip runs its hold and its length (the preparing time); a wait clip loops
    // from the cast's start until the server's result (preparing + casting) starts the shot; without
    // one the shot follows READY. The shot's clip runs its hold and its length.
    Swing out;
    out.readyClip = readyClip && ready > 0.0;
    out.wait = out.readyClip ? kClipHold + ready : 0.0;
    out.shot = waitClip && wait > 0.0 ? std::max(out.wait, ready + wait) : out.wait;
    out.end = out.shot + kClipHold + (waitClip ? shot : wait + shot);
    return out;
}

double StageEffects::StageStart(const Stage& s, const Swing& swing, std::span<const double> shotEvents) {
    // Its phase's start as the body plays it; a stage of the shot started by an event at the shot
    // clip's event of that number (its code-1 events, the clip's hit moments), else 0.3 into the shot.
    const double begin = s.phase == kReady ? 0.0 : s.phase == kWait ? swing.wait : swing.shot;
    if (s.startEvent <= 0 || s.phase < kShot) return begin;
    // The shot clip's Nth hit moment, after its hold.
    const double hold = swing.shotClip ? kClipHold : 0.0;
    if (std::size_t(s.startEvent) <= shotEvents.size()) return begin + hold + shotEvents[std::size_t(s.startEvent) - 1];
    return begin + hold + (shotEvents.empty() ? 0.3 * (swing.end - swing.shot - hold) : shotEvents.back());
}

float StageEffects::SpeedOf(const Stage& s) {
    const float lo = s.startSpeed > 0.0f ? s.startSpeed : 450.0f;
    return 0.5f * (lo + std::max(lo, s.endSpeed));
}

namespace {

// A stage that flies and carries its group's hit (AT_MOV_1TAR / AT_MOV_SPLASH movers, AT_TARGET
// with a speed), and how long it flies.
bool Carries(const StageEffects::Stage& s) {
    return s.action == "AT_MOV_1TAR" || s.action == "AT_MOV_SPLASH" || (s.action == "AT_TARGET" && s.startSpeed > 0.0f);
}

} // namespace

std::vector<double> StageEffects::DamageMoments(const Stages& stages, const Swing& swing, std::span<const double> shotEvents,
                                                float distance) {
    std::vector<std::pair<int, int>> groups; // (phase, start event) of the stages that show damage
    for (const Stage& s : stages) {
        if (!s.damageEvent || s.phase > kShot) continue;
        const std::pair<int, int> g{s.phase, std::max(0, s.startEvent)};
        if (std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g);
    }
    std::sort(groups.begin(), groups.end());
    std::vector<double> out;
    for (const auto& [phase, event] : groups) {
        // The group's first projectile carries its hit: shown as it lands.
        const Stage* carrier = nullptr;
        for (const Stage& s : stages) {
            if (s.phase == phase && std::max(0, s.startEvent) == event && Carries(s)) {
                carrier = &s;
                break;
            }
        }
        Stage s;
        s.phase = phase;
        s.startEvent = event;
        double at = StageStart(s, swing, shotEvents);
        if (carrier != nullptr) {
            const float length = carrier->action == "AT_TARGET"
                                     ? std::hypot(carrier->target.offset[0] - carrier->start.offset[0], carrier->target.offset[1] - carrier->start.offset[1],
                                                  carrier->target.offset[2] - carrier->start.offset[2])
                                     : distance;
            at += carrier->moveDelayMs / 1000.0 + double(length / std::max(1.0f, SpeedOf(*carrier)));
        }
        out.push_back(at);
    }
    return out;
}

namespace {

// MOV_UPR's tilt about the flight's line (degrees): one drawn in col 15's range at the launch
// (ours: from the cast's number, so a swing's copies differ but a replay does not).
float TiltOf(const StageEffects::Stage& s, std::uint32_t cast) {
    const float lo = std::min(s.tiltMin, s.tiltMax), hi = std::max(s.tiltMin, s.tiltMax);
    const std::uint32_t r = (cast * 2654435761u) ^ (std::uint32_t(s.startEvent) * 40503u + std::uint32_t(s.slot) * 9176u);
    return lo + (hi - lo) * float(r % 1001u) / 1000.0f;
}

// SCT_RUT's angle (degrees) of a stage: the number after it in col 25; -1 automatic; none: no SCT_RUT.
std::optional<float> RutOf(const StageEffects::Stage& s) {
    for (std::size_t i = 0; i < s.scripts.size(); ++i) {
        if (s.scripts[i] != "SCT_RUT") continue;
        return i + 1 < s.scripts.size() ? std::strtof(s.scripts[i + 1].c_str(), nullptr) : 0.0f;
    }
    return std::nullopt;
}

// SCT_RUT -1's angles: the 1.188 client's table at (u16 degrees), read from a base by the
// weapon's class and the shot clip, plus the hit's number (A 7).
constexpr std::array<int, 23> kRutAngles{0, 45, 305, 90, 325, 55, 155, 270, 0, 25, 270, 270, 330, 0, 45, 30, 330, 150, 270, 45, 225, 30, 330};

// The automatic angle, or -1 when the weapon's class has none (or the entry is past the table).
int AutoRut(int weaponKind, std::uint32_t shotState, int hit) {
    const int clip = shotState == 2 ? 0 : shotState == 5 ? 1 : shotState == 0x10 ? 2 : shotState == 0x11 ? 3 : 0;
    int base = -1;
    if (weaponKind == 2 || weaponKind == 3) base = 14 + 2 * clip;              //, A2, A6, AA
    if (weaponKind == 4 || weaponKind == 5) base = 10 + clip;                  // .. 9C
    if (weaponKind == 7) base = 8 + std::min(clip, 1);                         //, 94
    if (weaponKind == 8) base = 4 + std::min(clip, 2);                         //, 8C, 8E
    if (weaponKind == 9) base = 2 * std::min(clip, 1);                         //, 86
    const int at = base + hit;
    return base < 0 || at < 0 || at >= int(kRutAngles.size()) ? -1 : kRutAngles[std::size_t(at)];
}

} // namespace

std::uint32_t StageEffects::Skill(WorldScreen& world, const Stages& stages, std::size_t caster, std::size_t target, double now, const Swing& swing,
                                  bool actStart, const assets::SkillAnimation* animation, const Cast& cast, Part part, std::uint32_t castNumber,
                                  double clock) {
    gfx::EffectSystem& effects = world.Effects();
    // `now` is the swing's start; the effects' delays count from the clock (a shot started at its impact is told
    // after its swing's start: P7).
    const double lag = clock >= 0.0 ? std::max(0.0, clock - now) : 0.0;
    const auto delayOf = [lag](double at) { return static_cast<float>(std::max(0.0, at - lag)); };
    const std::span<const double> shotEvents = cast.shotEvents;
    const std::string_view weapon = cast.weapon;
    const std::size_t flightsBefore = flights_.size();
    std::erase_if(planned_, [&](const Planned& p) { return p.start < now - 30.0; }); // long over
    // The damage groups' numbers (1 the first), in the order their hits show (DamageMoments).
    std::vector<std::pair<int, int>> damageGroups;
    for (const Stage& s : stages) {
        if (!s.damageEvent || s.phase > kShot) continue;
        const std::pair<int, int> g{s.phase, std::max(0, s.startEvent)};
        if (std::find(damageGroups.begin(), damageGroups.end(), g) == damageGroups.end()) damageGroups.push_back(g);
    }
    std::sort(damageGroups.begin(), damageGroups.end());
    // When each phase starts and ends, as the body plays them.
    const auto phaseStart = [&](const Stage& s) { return StageStart(s, swing, shotEvents); };
    const auto phaseEnd = [&](int phase) { return phase == kReady ? swing.wait : phase == kWait ? swing.shot : swing.end; };
    const bool buff = std::any_of(stages.begin(), stages.end(), [](const Stage& s) { return s.phase == kActStart || s.phase == kActLoop; });
    const std::uint32_t castNo = castNumber != 0 ? castNumber : ++casts_;
    for (const Stage& s : stages) {
        // Its part (the shot's: SHOT and the cast's ACT_S, at the impact; the rest before it).
        if (part != Part::All && (part == Part::Shot) != (s.phase == kShot || s.phase == kActStart)) continue;
        // Its sounds, whatever it shows: SndBegin as
        // the stage starts (on the target for the target's actions); a stage with an object its SndEnd then too, at
        // the caster (not an end's cue: nothing plays as a held object ends); a flight's SndEnd again where it lands (Fly).
        if (s.phase <= kShot || (s.phase == kActStart && actStart)) {
            const double start = s.phase == kActStart ? swing.shot : phaseStart(s);
            const bool onTarget = s.action == "AT_TARGET" || s.action == "AT_TARGET_F" || s.action == "AT_DMG_POS";
            const std::size_t where = onTarget && target != kNone ? target : caster;
            Cue(s.soundBegin, now + start, where, caster);
            if (s.phase <= kShot) Shake(ShakeOf(s, false), now + start, caster);
            // Its SCT_MAT on the caster from the stage's start to the presentation's end.
            if (s.phase <= kShot) AddTint(s, caster, now + start, now + swing.end);
            if (!s.object.empty()) Cue(s.soundEnd, now + start, caster, caster);
        }
        // Models: held on a bone, or thrown (the original draws them at full effect quality only).
        // "WEAPON" is the caster's own (a thrown spear or glaive).
        std::string object = s.object;
        if (core::NormalizeAssetPath(object) == "weapon") object = std::string(weapon);
        if (IsModelFile(object) && s.phase <= kShot && effects.Quality() >= 2) {
            const double start = phaseStart(s);
            // At its target: played once where the target stands (the game screen's slot of its own).
            if ((s.action == "AT_TARGET" || s.action == "AT_TARGET_F") && target != kNone) {
                targetModels_.push_back({object, target, now + start, caster});
                continue;
            }
            if (s.action.starts_with("AT_MOV")) {
                // Col 11 copies, each its own tilt; the first takes over the held one (Trade).
                for (int copy = 0; copy < std::max(1, s.createCount); ++copy) {
                    Flight flight;
                    flight.key = NewKey();
                    flight.model = object;
                    flight.carried = ArrowEffects(s, animation);
                    flight.weapon = core::NormalizeAssetPath(s.object) == "weapon";
                    flight.cast = castNo;
                    flight.trade = copy == 0 ? s.trade : 0;
                    flight.caster = caster;
                    flight.target = target == kNone ? caster : target;
                    flight.from = s.start;
                    flight.to = s.target;
                    flight.release = now + start + s.moveDelayMs / 1000.0;
                    flight.speed = SpeedOf(s);
                    flight.arc = s.move == "MOV_UP" || s.move == "MOV_UPR" ? s.arc / 1000.0f : 0.0f;
                    flight.tilt = s.move == "MOV_UPR" ? glm::radians(TiltOf(s, castNo + std::uint32_t(copy))) : 0.0f;
                    flight.splash = s.action == "AT_MOV_SPLASH";
                    flight.arrival = IsEffectFile(s.arrival) ? s.arrival : std::string();
                    flight.landSound = s.soundEnd;
                    flight.landShake = ShakeOf(s, true);
                    flights_.push_back(std::move(flight));
                }
                continue;
            }
            Held held;
            held.key = NewKey();
            held.model = object;
            held.carried = ArrowEffects(s, animation);
            held.trail = animation != nullptr ? animation->arrowTrail : std::string();
            held.cast = castNo;
            held.slot = s.slot;
            held.caster = caster;
            held.at = s.start;
            held.turn = StageTurn(s, s.start);
            held.from = now + start;
            // Until its phase ends (a loop: the cast), or until a later stage kills it; one a later mover takes over (the
            // shot's arrow) stays in hand until that flight takes it, its glow going with it (cases 7 and 10:
            // takes the held record whose slot is the mover's Trade out of the list as the mover is made; no
            // phase ends it before). A little past the release, should the flight never come.
            double until = s.action == "AT_LOOP" ? swing.end : phaseEnd(s.phase);
            bool traded = false;
            if (s.slot > 0) {
                for (const Stage& later : stages) {
                    if (&later == &s || later.phase < s.phase) continue;
                    if (later.trade == s.slot && later.action.starts_with("AT_MOV")) {
                        until = phaseStart(later) + later.moveDelayMs / 1000.0 + 0.5;
                        traded = true;
                    } else if (later.kill == s.slot || later.trade == s.slot) {
                        until = phaseStart(later);
                    }
                }
            }
            if (!traded && s.action != "AT_LOOP" && s.lifeMs > 0.0f) until = start + s.lifeMs / 1000.0;
            held.until = now + std::max(until, start + 0.1);
            held_.push_back(std::move(held));
            continue;
        }
        if (s.object.empty() || !IsEffectFile(s.object)) continue;
        // Buffs: their start shows on the cast when asked (one's own come with the buff itself).
        if (s.phase == kActLoop || s.phase > kActLoop || (s.phase == kActStart && !actStart)) continue;
        if (s.phase > kShot && s.phase != kActStart) continue;
        const double start = s.phase == kActStart ? swing.shot : phaseStart(s);
        gfx::EffectSystem::Request request;
        request.path = s.object;
        request.scale = EffectScaleOf(s);
        request.delay = delayOf(start);
        request.fadeIn = request.fadeOut = s.fadeMs / 1000.0f; // col 12
        request.local = StageTurn(s, s.start);
        request.kind = gfx::EffectSystem::Kind::Skill;
        const std::string& action = s.action;
        // A flight's speed: drawn between the stage's two.
        const auto speedOf = [&](const Stage& st) { return SpeedOf(st); };
        if (action == "AT_MOV_OPTION" && s.rayDistance > 0.0f && s.raySpacing > 0.0f) {
            // A ray out of the caster: a copy of its effect left every spacing along it (Fly).
            Ray ray;
            ray.object = s.object;
            ray.caster = caster;
            ray.from = s.start;
            ray.direction = s.rayDirection;
            ray.distance = s.rayDistance;
            ray.spacing = s.raySpacing;
            ray.speed = speedOf(s);
            ray.release = now + start + s.moveDelayMs / 1000.0;
            ray.next = s.raySpacing;
            rays_.push_back(std::move(ray));
            continue;
        }
        if (action == "AT_TARGET" && s.move == "MOV_STRAIGHT" && s.startSpeed > 0.0f && target != kNone) {
            // A mover on the target: from its place plus the start offset to its place plus the target
            // offset, in the world's axes, after its move delay (the frost's ice falling around it).
            glm::mat4 root(1.0f);
            if (!world.StageBone(target, {}, root)) continue;
            Flight flight;
            flight.caster = target;
            flight.target = target;
            flight.onTarget = true;
            flight.fromOffset = glm::vec3(s.start.offset[0], s.start.offset[1], s.start.offset[2]);
            flight.toOffset = glm::vec3(s.target.offset[0], s.target.offset[1], s.target.offset[2]);
            flight.release = now + start + s.moveDelayMs / 1000.0;
            flight.speed = speedOf(s);
            flight.arrival = IsEffectFile(s.arrival) ? s.arrival : std::string();
            flight.landSound = s.soundEnd;
            flight.landShake = ShakeOf(s, true);
            request.seconds = -1.0f; // until it lands
            request.delay = delayOf(start + s.moveDelayMs / 1000.0);
            request.at = glm::translate(glm::mat4(1.0f), glm::vec3(root[3]) + flight.fromOffset);
            flight.effect = effects.Start(request);
            if (flight.effect != 0) flights_.push_back(std::move(flight));
            continue;
        }
        if (action.starts_with("AT_MOV") || (action == "AT_SOURCE" && target != kNone)) {
            // A flight from the caster to the target: its effect shows from its release (after its delay).
            request.delay = delayOf(start + s.moveDelayMs / 1000.0);
            request.seconds = -1.0f; // until it lands
            glm::mat4 root(1.0f);
            if (!world.StageBone(caster, {}, root)) continue;
            request.at = glm::translate(glm::mat4(1.0f), glm::vec3(root[3]));
            // Col 11 copies (AT_SOURCE: so many from each target back to the caster).
            std::vector<std::size_t> from{target == kNone ? caster : target};
            if (action == "AT_SOURCE" && struckBy) {
                for (const std::size_t t : struckBy(caster)) {
                    if (std::find(from.begin(), from.end(), t) == from.end()) from.push_back(t);
                }
            }
            for (const std::size_t who : from) {
                for (int copy = 0; copy < std::max(1, s.createCount); ++copy) {
                    Flight flight;
                    flight.caster = caster;
                    flight.target = who;
                    flight.from = s.start;
                    flight.to = s.target;
                    flight.release = now + start + s.moveDelayMs / 1000.0;
                    flight.speed = speedOf(s);
                    flight.arc = s.move == "MOV_UP" || s.move == "MOV_UPR" ? s.arc / 1000.0f : 0.0f;
                    flight.tilt = s.move == "MOV_UPR" ? glm::radians(TiltOf(s, castNo + std::uint32_t(copy))) : 0.0f;
                    if (s.move == "MOV_HWAN") {
                        // Its wandering curve (a bend of -60..+60 degrees, homing): ours an arc
                        // to one side in a plane tilted by that bend.
                        flight.arc = 0.25f;
                        flight.tilt = glm::radians(90.0f + float(int((castNo * 2654435761u + std::uint32_t(copy) * 97u) % 121u)) - 60.0f);
                    }
                    flight.splash = action == "AT_MOV_SPLASH";
                    if (flight.splash) {
                        flight.hopEffect = request.path;
                        flight.hopScale = request.scale;
                    }
                    flight.toCaster = action == "AT_SOURCE";
                    if (flight.toCaster) {
                        flight.from = Stage::Anchor{};
                        flight.from.atMiddle = true;
                    }
                    flight.arrival = IsEffectFile(s.arrival) ? s.arrival : std::string();
                    flight.landSound = s.soundEnd;
                    flight.landShake = ShakeOf(s, true);
                    flight.effect = effects.Start(request);
                    if (flight.effect != 0) flights_.push_back(std::move(flight));
                }
            }
            continue;
        }
        const bool onTarget = action == "AT_TARGET" || action == "AT_TARGET_F" || action == "AT_DMG_POS";
        const bool follow = action == "AT_ONE_FOLLOW" || action == "AT_LOOP" || action.empty();
        // AT_TARGET_F: on the target, following its bone for its life (a deco added to it: case 4; P9).
        request.anchor = Anchor(onTarget && target != kNone ? target : caster, onTarget ? s.target : s.start, follow || action == "AT_TARGET_F");
        request.local = StageTurn(s, onTarget ? s.target : s.start);
        if (action == "AT_TARGET" && target != kNone) {
            // A still one on the target: turned to the caster's heading, after its delay.
            request.anchor.turnOf = WorldScreen::StageAnchor(caster);
            request.delay = delayOf(start + s.moveDelayMs / 1000.0);
        }
        if (action == "AT_DMG_POS") {
            // Only with SCT_RUT (else nothing; its hit still shows): at the target's damage point in the
            // caster's frame, rolled about its forward axis by the angle (about x from 360; -1 the
            // automatic table).
            const std::optional<float> rut = RutOf(s);
            if (!rut || target == kNone) continue;
            float degrees = *rut;
            if (degrees < 0.0f) {
                const auto g = std::find(damageGroups.begin(), damageGroups.end(), std::pair<int, int>{s.phase, std::max(0, s.startEvent)});
                const int hit = g != damageGroups.end() ? int(g - damageGroups.begin()) + 1 : 1;
                const int angle = AutoRut(cast.weaponKind, cast.shotState, hit);
                if (angle < 0) continue;
                degrees = float(angle);
                if (angle == 0 && (cast.weaponKind == 4 || cast.weaponKind == 5)) request.path = "hiteffect\\hit_1_pierce_critical.efp";
            }
            request.anchor = Anchor(target, cast.targetHit, false);
            request.anchor.turnOf = WorldScreen::StageAnchor(caster);
            request.local = degrees >= 360.0f ? glm::rotate(glm::mat4(1.0f), glm::radians(degrees - 360.0f), glm::vec3(1, 0, 0))
                                              : glm::rotate(glm::mat4(1.0f), glm::radians(degrees), glm::vec3(0, 0, 1));
        }
        if (action == "AT_LOOP") {
            // Until the cast ends, or until a later stage kills its slot.
            double until = swing.end;
            if (s.slot > 0) {
                for (const Stage& later : stages) {
                    if (later.kill == s.slot && later.phase > s.phase) until = std::min(until, phaseStart(later));
                }
            }
            request.seconds = static_cast<float>(std::max(0.1, until - start));
            if (buff && s.phase == kActStart) request.seconds = 0.0f;
        } else if (s.lifeMs > 0.0f) {
            request.seconds = s.lifeMs / 1000.0f;
        }
        // Kept for a cancel: a held one (AT_STOP, ONE_FOLLOW, LOOP, DMG_POS) ends with it, one not
        // started yet never runs.
        const bool held = follow || action == "AT_STOP" || action == "AT_DMG_POS";
        // A held one of the swing (AT_STOP, ONE_FOLLOW, DMG_POS) ends with its presentation, or sooner as a later stage
        // kills its slot (col 10), faded by its col 12 (P8, P10); a loop's end is its time above.
        double end = -1.0;
        if ((action == "AT_STOP" || action == "AT_ONE_FOLLOW" || action == "AT_DMG_POS") && s.phase <= kShot) {
            double until = swing.end;
            if (s.slot > 0) {
                for (const Stage& later : stages) {
                    if (later.kill != s.slot || later.phase > kShot) continue;
                    const double at = phaseStart(later);
                    if (later.phase > s.phase || (later.phase == s.phase && at > start)) until = std::min(until, at);
                }
            }
            end = now + std::max(until, start + 0.05);
        }
        if (const Handle h = effects.Start(request); h != 0) planned_.push_back({h, caster, now + lag + double(request.delay), held, end});
    }
    for (std::size_t i = flightsBefore; i < flights_.size(); ++i) flights_[i].owner = caster;
    return castNo;
}

void StageEffects::Cancel(WorldScreen& world, std::size_t caster, double now) {
    if (caster == kNone) return;
    gfx::EffectSystem& effects = world.Effects();
    std::erase_if(planned_, [&](const Planned& p) {
        if (p.caster != caster) return false;
        if (p.start > now) {
            effects.Stop(p.effect); // its stage never ran
        } else if (p.held && effects.Alive(p.effect)) {
            effects.Release(p.effect); // ends: no more of it, what it made fades
        }
        return true;
    });
    std::erase_if(cues_, [&](const SoundCue& c) { return c.caster == caster && c.at > now; }); // a sound playing is not stopped
    std::erase_if(shakes_, [&](const ShakeCue& s) { return s.caster == caster && s.at > now; });
    std::erase_if(targetModels_, [&](const TargetModel& m) { return m.caster == caster && m.at > now; });
    std::erase_if(rays_, [&](const Ray& r) { return r.caster == caster && !r.started && r.release > now; });
    std::erase_if(flights_, [&](const Flight& f) {
        if (f.owner != caster || f.flying || f.release <= now) return false;
        if (f.effect != 0) effects.Stop(f.effect);
        return true;
    });
    for (Held& h : held_) {
        if (h.caster == caster) h.until = std::min(h.until, now);
    }
    for (Tint& t : tints_) {
        if (t.slot == caster && t.until > now) t.until = now; // a cast's (a buff's, until released, stays)
    }
}

StageEffects::Handle StageEffects::AddCompanion(const Stage& s, std::size_t holder, double life) {
    // The actions a carried model takes (it stays on its anchor; a moving one is a flight's).
    if (!(s.action == "AT_LOOP" || s.action == "AT_ONE_FOLLOW" || s.action == "AT_STOP" || s.action.empty())) return 0;
    const auto free = std::find(companionUsed_.begin(), companionUsed_.end(), false);
    if (free == companionUsed_.end()) return 0;
    *free = true;
    Companion c;
    c.handle = kCompanionHandle | (nextCompanion_++ & 0x7FFFFFFFu);
    c.slot = companionFirst_ + std::size_t(free - companionUsed_.begin());
    c.holder = holder;
    c.model = s.object;
    c.at = s.start;
    c.turn = StageTurn(s, s.start);
    c.life = life;
    c.mover = std::find(s.scripts.begin(), s.scripts.end(), "SCT_MOVER") != s.scripts.end();
    companions_.push_back(std::move(c));
    return companions_.back().handle;
}

StageEffects::Handle StageEffects::AddTint(const Stage& s, std::size_t slot, double now, double until) {
    // SCT_MAT,r,g,b,r,g,b,ms in col 25 (the scripts list: the name, then its seven numbers).
    const auto at = std::find(s.scripts.begin(), s.scripts.end(), "SCT_MAT");
    if (at == s.scripts.end() || s.scripts.end() - at < 8 || slot == kNone) return 0;
    std::array<float, 7> v{};
    for (std::size_t i = 0; i < 7; ++i) v[i] = std::strtof((at + 1 + std::ptrdiff_t(i))->c_str(), nullptr);
    Tint t;
    t.handle = kCompanionHandle | (nextCompanion_++ & 0x7FFFFFFFu);
    t.slot = slot;
    t.from = glm::vec3(v[0], v[1], v[2]) / 255.0f;
    t.to = glm::vec3(v[3], v[4], v[5]) / 255.0f;
    t.ms = std::max(1.0f, v[6]);
    t.since = now;
    t.until = until;
    // One colour a model: the one on the slot is overwritten (its end still turns the new one off).
    for (Tint& older : tints_) {
        if (older.slot == slot) older.off = true;
    }
    tints_.push_back(t);
    return t.handle;
}

void StageEffects::EndTint(std::size_t slot) {
    std::erase_if(tints_, [&](const Tint& t) { return t.slot == slot; });
}

void StageEffects::Release(WorldScreen& world, Handle handle) {
    if (const auto t = std::find_if(tints_.begin(), tints_.end(), [&](const Tint& x) { return x.handle == handle; }); t != tints_.end()) {
        EndTint(t->slot); // any SCT_MAT's end turns the model's colour off
        return;
    }
    if ((handle & kCompanionHandle) == 0) {
        world.Effects().Release(handle);
        return;
    }
    for (auto it = companions_.begin(); it != companions_.end(); ++it) {
        if (it->handle != handle) continue;
        if (it->shown) world.ClearStage(it->slot);
        if (it->slot >= companionFirst_ && it->slot - companionFirst_ < companionUsed_.size()) companionUsed_[it->slot - companionFirst_] = false;
        companions_.erase(it);
        return;
    }
}

StageEffects::Handle StageEffects::ObjectEffect(WorldScreen& world, const std::string& path, const glm::vec3& at, float yaw) {
    if (path.empty()) return 0;
    gfx::EffectSystem::Request request;
    request.path = path;
    request.at = glm::rotate(glm::translate(glm::mat4(1.0f), at), yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    request.seconds = -1.0f;
    return world.Effects().Start(request);
}

double StageEffects::Strike(WorldScreen& world, std::size_t holder, std::size_t target, double now) {
    for (Companion& c : companions_) {
        if (!c.mover || !c.shown || c.holder != holder) continue;
        glm::mat4 root(1.0f);
        if (target == kNone || !world.StageBone(target, {}, root)) return -1.0;
        const glm::vec3 to = glm::vec3(root[3]) + glm::vec3(0.0f, 29.0f, 0.0f);
        const float d = std::max(0.0f, glm::length(to - c.pos) - 10.0f);
        c.state = 1;
        c.stateAt = now;
        c.target = target;
        // Its flight at 100 units a second (there at once from beyond 500), then its attack clip's hit.
        const double flight = d > 500.0f ? 0.0 : d / 100.0;
        const std::vector<double> events = world.StageEvents(c.slot, "default", 2);
        const double clip = world.StageClipSeconds(c.slot, "default", 2);
        return flight + (events.empty() ? (clip > 0.0 ? clip * 0.5 : 0.3) : events.front());
    }
    return -1.0;
}

void StageEffects::ReturnHit(WorldScreen& world, const Stages& stages, std::size_t target, std::size_t owner, double now, bool atSource) {
    for (const Stage& s : stages) {
        // S_RETURN on the target; a burst's, those at the source (its holder: the target).
        if (s.phase != 9) continue;
        if (atSource ? s.action != "AT_SOURCE" : (s.action != "AT_TARGET" && s.action != "AT_DMG_POS")) continue;
        Cue(s.soundBegin, now, target);
        Shake(ShakeOf(s, false), now, owner);
        if (s.object.empty() || !IsEffectFile(s.object)) continue;
        gfx::EffectSystem::Request request;
        request.path = s.object;
        request.scale = EffectScaleOf(s);
        request.anchor = Anchor(target, s.target, false);
        world.Effects().Start(request);
    }
}

void StageEffects::ObjectStruck(WorldScreen& world, const Stages& stages, const glm::vec3& at, std::size_t owner, double now) {
    for (const Stage& s : stages) {
        if (s.phase != 9 || s.action == "AT_TARGET" || s.action == "AT_DMG_POS") continue; // S_RETURN where it stands
        if (!s.soundBegin.empty()) cues_.push_back({s.soundBegin, now, kNone, at});
        Shake(ShakeOf(s, false), now, owner);
        if (s.object.empty() || !IsEffectFile(s.object)) continue;
        gfx::EffectSystem::Request request;
        request.path = s.object;
        request.scale = EffectScaleOf(s);
        request.at = glm::translate(glm::mat4(1.0f), at);
        world.Effects().Start(request);
    }
}

float StageEffects::EffectScaleOf(const Stage& s) {
    // SCT_EFFECT_SCALE,<scale> in col 25 (51 rows, the monsters' skills).
    for (std::size_t i = 0; i + 1 < s.scripts.size(); ++i) {
        if (s.scripts[i] != "SCT_EFFECT_SCALE") continue;
        const float scale = std::strtof(s.scripts[i + 1].c_str(), nullptr);
        return scale > 0.0f ? scale : 1.0f;
    }
    return 1.0f;
}

bool StageEffects::CharScaleOf(const Stages& stages, float& scale, float& ms) {
    for (const Stage& s : stages) {
        for (std::size_t i = 0; i + 1 < s.scripts.size(); ++i) {
            if (s.scripts[i] != "SCT_CHAR_SCALE") continue;
            scale = std::strtof(s.scripts[i + 1].c_str(), nullptr);
            ms = i + 2 < s.scripts.size() ? std::strtof(s.scripts[i + 2].c_str(), nullptr) : 0.0f;
            if (scale <= 0.0f) scale = 1.0f;
            return true;
        }
    }
    return false;
}

bool StageEffects::OwnerForm(const Stages& stages) {
    return std::any_of(stages.begin(), stages.end(), [](const Stage& s) { return s.phase >= kActOnStart && s.phase <= kActOnEnd; });
}

void StageEffects::BuffStart(WorldScreen& world, const Stages& stages, std::size_t slot) {
    const int start = OwnerForm(stages) ? kActOnStart : kActStart;
    for (const Stage& s : stages) {
        if (s.phase == start) Cue(s.soundBegin, 0.0, slot); // its start's sound
        // A model its start carries, for its life (2 s when it names none; ours).
        if (s.phase == start && IsModelFile(s.object) && world.Effects().Quality() >= 2) {
            AddCompanion(s, slot, s.lifeMs > 0.0f ? s.lifeMs / 1000.0 : 2.0);
            continue;
        }
        if (s.phase != start || s.object.empty() || !IsEffectFile(s.object)) continue;
        gfx::EffectSystem::Request request;
        request.path = s.object;
        request.scale = EffectScaleOf(s);
        request.fadeIn = request.fadeOut = s.fadeMs / 1000.0f;
        request.local = StageTurn(s, s.start);
        request.anchor = Anchor(slot, s.start, true);
        world.Effects().Start(request);
    }
}

std::vector<StageEffects::Handle> StageEffects::BuffLoops(WorldScreen& world, const Stages& stages, std::size_t slot) {
    std::vector<Handle> loops;
    const int loop = OwnerForm(stages) ? kActOnLoop : kActLoop;
    for (const Stage& s : stages) {
        if (s.phase == loop) Cue(s.soundBegin, 0.0, slot); // as its loop starts (ours: once)
        if (s.phase == loop) {
            if (const Handle h = AddTint(s, slot, lastFly_, -1.0); h != 0) loops.push_back(h);
        }
        // A model its loop carries (the hawk over the caster) until the buff ends.
        if (s.phase == loop && IsModelFile(s.object) && world.Effects().Quality() >= 2) {
            if (const Handle h = AddCompanion(s, slot, -1.0); h != 0) loops.push_back(h);
            continue;
        }
        if (s.phase != loop || s.object.empty() || !IsEffectFile(s.object)) continue;
        gfx::EffectSystem::Request request;
        request.path = s.object;
        request.scale = EffectScaleOf(s);
        request.fadeIn = request.fadeOut = s.fadeMs / 1000.0f;
        request.local = StageTurn(s, s.start);
        request.seconds = -1.0f; // until the buff ends
        request.anchor = Anchor(slot, s.start, true);
        if (const Handle h = world.Effects().Start(request); h != 0) loops.push_back(h);
    }
    return loops;
}

void StageEffects::StateEnd(WorldScreen& world, const Stages& stages, std::size_t slot) {
    const int end = OwnerForm(stages) ? kActOnEnd : kDeact;
    for (const Stage& s : stages) {
        if (s.phase == end) Cue(s.soundBegin, 0.0, slot); // its end's sound
        if (s.phase == end && IsModelFile(s.object) && world.Effects().Quality() >= 2) {
            AddCompanion(s, slot, s.lifeMs > 0.0f ? s.lifeMs / 1000.0 : 2.0);
            continue;
        }
        if (s.phase != end || s.object.empty() || !IsEffectFile(s.object)) continue;
        gfx::EffectSystem::Request request;
        request.path = s.object;
        request.scale = EffectScaleOf(s);
        request.fadeIn = request.fadeOut = s.fadeMs / 1000.0f;
        request.local = StageTurn(s, s.start);
        request.anchor = Anchor(slot, s.start, true);
        world.Effects().Start(request);
    }
}

StageEffects::Handle StageEffects::Play(WorldScreen& world, std::size_t slot, const std::string& path, const Stage::Anchor& at,
                                        gfx::EffectSystem::Kind kind) {
    if (path.empty() || !IsEffectFile(path)) return 0;
    gfx::EffectSystem::Request request;
    request.path = path;
    request.kind = kind;
    request.anchor = Anchor(slot, at, kind != gfx::EffectSystem::Kind::Hit);
    return world.Effects().Start(request);
}

std::vector<StageEffects::Handle> StageEffects::ItemGlows(WorldScreen& world, std::size_t slot, const assets::FileSystem& files,
                                                          const assets::ItemTable& items, const assets::ItemGlowTable& glows,
                                                          std::span<const std::uint32_t> worn, std::span<const std::uint8_t> plus) {
    std::vector<Handle> out;
    for (std::size_t i = 0; i < worn.size(); ++i) {
        // 1.188's dressing (for the weapon's and the shield's models): every effect the item's lines list
        // whose plus range holds its plus, no rarity asked (itemrare.txt lists the rare ones, whose range is any plus;
        // itemoptionefp.txt every weapon and shield from +8). v1.150's asked for rarity 2 first.
        const assets::ItemRow* row = items.Find(worn[i]);
        if (row == nullptr) continue;
        const int place = assets::EquipSlot(*row);
        if (place != 6 && place != 7) continue;
        const std::vector<assets::ItemGlowEffect>* list = glows.Find(row->codename);
        if (list == nullptr) continue;
        const int itemPlus = i < plus.size() ? plus[i] : 0;
        // On each model the item puts on (an axe's or dagger's two hands both glow), on that model's
        // own bone, kept until the item comes off.
        for (const std::string& model : assets::WornModels(files, items, *row)) {
            for (const assets::ItemGlowEffect& g : *list) {
                if (!g.ShowsAt(itemPlus)) continue;
                gfx::EffectSystem::Request request;
                request.path = g.effect;
                request.anchor.target = WorldScreen::StageAnchor(slot);
                request.anchor.bone = g.bone + "@" + model;
                request.anchor.offset = glm::vec3(g.offset[0], g.offset[1], g.offset[2]);
                request.scale = g.scale;
                request.seconds = -1.0f;
                if (const Handle h = world.Effects().Start(request); h != 0) out.push_back(h);
            }
        }
    }
    return out;
}

void StageEffects::Carry(WorldScreen& world, std::uint32_t key, const std::vector<std::string>& effects, bool atOrigin,
                         const std::string& fadeTrail) {
    // SCT_ARROW: on the model's Bone01, kept while the model is there; the held arrow's tail
    // fades in over 3,000 ms.
    for (const std::string& path : effects) {
        gfx::EffectSystem::Request request;
        request.path = path;
        request.anchor.target = WorldScreen::FlyingAnchor(key);
        if (!atOrigin) request.anchor.bone = "Bone01";
        request.seconds = -1.0f;
        if (!fadeTrail.empty() && path == fadeTrail) request.fadeIn = 3.0f;
        world.Effects().Start(request);
    }
}

void StageEffects::Fly(WorldScreen& world, double now) {
    gfx::EffectSystem& effects = world.Effects();
    // The held effects whose presentation is over (or whose slot a later stage killed): released, fading by their col 12.
    for (Planned& p : planned_) {
        if (p.end < 0.0 || p.end > now) continue;
        if (effects.Alive(p.effect)) effects.Release(p.effect);
        p.end = -1.0;
    }
    const auto point = [&](std::size_t slot, const Stage::Anchor& a, glm::vec3& out) {
        glm::mat4 m(1.0f);
        if (!AnchorPlace(world, slot, a, m)) return false;
        out = glm::vec3(m[3]);
        return true;
    };
    const auto release = [&](Held& h) {
        if (h.shown) world.ClearFlying(h.key); // its carried effects lose their anchor and fade where they are
    };
    // Rays: out of their caster at their speed, a copy of their effect left every spacing, each turned
    // to the ray's way.
    for (auto it = rays_.begin(); it != rays_.end();) {
        Ray& r = *it;
        if (now < r.release) {
            ++it;
            continue;
        }
        if (!r.started) {
            glm::mat4 m(1.0f);
            if (!AnchorPlace(world, r.caster, r.from, m)) {
                it = rays_.erase(it);
                continue;
            }
            r.origin = glm::vec3(m[3]);
            // Where the caster faces (its -z), level, then the ray's degrees about y (y turns the
            // other way from glm's, as a stage's turn: StageTurn).
            glm::vec3 ahead(-m[2][0], 0.0f, -m[2][2]);
            ahead = glm::length(ahead) > 1e-4f ? glm::normalize(ahead) : glm::vec3(0, 0, -1);
            r.dir = glm::vec3(glm::rotate(glm::mat4(1.0f), -glm::radians(r.direction), glm::vec3(0, 1, 0)) * glm::vec4(ahead, 0.0f));
            r.started = true;
        }
        const float travelled = std::min(r.distance, r.speed * static_cast<float>(now - r.release));
        while (r.next <= travelled + 1e-3f && r.next <= r.distance + 1e-3f) {
            const glm::vec3 at = r.origin + r.dir * r.next;
            const glm::vec3 side = glm::normalize(glm::cross(glm::vec3(0, 1, 0), -r.dir));
            gfx::EffectSystem::Request copy;
            copy.path = r.object;
            copy.kind = gfx::EffectSystem::Kind::Skill;
            copy.at = glm::mat4(glm::vec4(side, 0.0f), glm::vec4(0, 1, 0, 0), glm::vec4(-r.dir, 0.0f), glm::vec4(at, 1.0f));
            effects.Start(copy);
            r.next += r.spacing;
        }
        if (r.next > r.distance + 1e-3f) {
            it = rays_.erase(it);
            continue;
        }
        ++it;
    }
    // Carried models: on their holder's anchor, shown as a stage character there (its own clips play),
    // for their life or until released.
    for (auto it = companions_.begin(); it != companions_.end();) {
        Companion& c = *it;
        if (c.shown && c.until >= 0.0 && now >= c.until) {
            world.ClearStage(c.slot);
            if (c.slot - companionFirst_ < companionUsed_.size()) companionUsed_[c.slot - companionFirst_] = false;
            it = companions_.erase(it);
            continue;
        }
        glm::mat4 m(1.0f);
        if (!AnchorPlace(world, c.holder, c.at, m)) {
            if (c.shown) world.ClearStage(c.slot); // its holder gone for now
            c.shown = false;
            ++it;
            continue;
        }
        m = m * c.turn;
        if (!c.shown) {
            WorldScreen::StageLook look{c.model};
            look.shadow = false;
            world.SetStage(c.slot, look, m[3][0], m[3][1], m[3][2], 0.0f);
            c.shown = true;
            c.pos = glm::vec3(m[3]);
            if (c.life >= 0.0 && c.until < 0.0) c.until = now + c.life;
        }
        if (c.mover && c.state != 0) {
            // Away from its holder: to 10 short of the target at its height + 29, at 100 units a second
            // (beyond 500: there at once); its attack there, then near it for 5 s (or until it is gone),
            // then back to its place over its holder.
            const float dt = lastFly_ >= 0.0 ? float(std::clamp(now - lastFly_, 0.0, 0.1)) : 0.0f;
            glm::mat4 root(1.0f);
            const bool there = c.target != kNone && world.StageBone(c.target, {}, root);
            if (!there && c.state <= 3) {
                c.state = 4;
                c.stateAt = now;
            }
            glm::vec3 goal = glm::vec3(m[3]);
            glm::vec3 face = goal - c.pos;
            if (c.state <= 3) {
                const glm::vec3 t = glm::vec3(root[3]) + glm::vec3(0.0f, 29.0f, 0.0f);
                glm::vec3 flat = c.pos - t;
                flat.y = 0.0f;
                const float d = glm::length(flat);
                goal = t + (d > 1e-3f ? flat / d : glm::vec3(1.0f, 0.0f, 0.0f)) * 10.0f;
                face = t - c.pos;
            }
            const glm::vec3 left = goal - c.pos;
            const float distance = glm::length(left);
            if (c.state == 1 || c.state == 4) {
                if (distance > 500.0f || distance <= 100.0f * dt + 0.5f) {
                    c.pos = goal;
                    c.state = c.state == 1 ? 2 : 0;
                    c.stateAt = now;
                } else {
                    c.pos += left / distance * (100.0f * dt);
                }
            } else {
                c.pos = goal; // attacking, near it: kept by it
                const double clip = world.StageClipSeconds(c.slot, "default", 2);
                if (c.state == 2 && now - c.stateAt >= (clip > 0.0 ? clip : 0.8)) {
                    c.state = 3;
                    c.stateAt = now;
                } else if (c.state == 3 && now - c.stateAt >= 5.0) {
                    c.state = 4;
                    c.stateAt = now;
                }
            }
            if (std::hypot(face.x, face.z) > 0.01f) c.yaw = std::atan2(face.x, face.z);
            const std::uint32_t clip = c.state == 2 ? 2u : c.state == 1 || c.state == 4 ? 7u : 0u;
            if (clip != c.clip) {
                WorldScreen::StageLook look{c.model};
                look.shadow = false;
                look.state = clip;
                world.SetStage(c.slot, look, c.pos.x, c.pos.y, c.pos.z, c.yaw);
                c.clip = clip;
            }
            if (c.state != 0) {
                const float cs = std::cos(c.yaw), sn = std::sin(c.yaw);
                m = glm::mat4(glm::vec4(cs, 0.0f, sn, 0.0f), glm::vec4(0.0f, 1.0f, 0.0f, 0.0f), glm::vec4(-sn, 0.0f, cs, 0.0f), glm::vec4(c.pos, 1.0f));
            }
        } else {
            c.pos = glm::vec3(m[3]);
        }
        world.SetStageTransform(c.slot, Rows(m));
        ++it;
    }
    lastFly_ = now;
    // The tints: the one on each slot, its colour going there and back; one that ends, replaced or not,
    // turns its slot's colour off.
    std::vector<std::size_t> shown;
    std::vector<std::size_t> ended;
    for (const Tint& t : tints_) {
        if (t.until >= 0.0 && now >= t.until) ended.push_back(t.slot);
    }
    for (const std::size_t slot : ended) EndTint(slot);
    for (auto it = tints_.rbegin(); it != tints_.rend(); ++it) {
        if (it->off || now < it->since || std::find(shown.begin(), shown.end(), it->slot) != shown.end()) continue;
        const double phase = std::fmod(std::max(0.0, now - it->since) * 1000.0 / it->ms, 2.0);
        const float k = float(phase <= 1.0 ? phase : 2.0 - phase);
        const glm::vec3 colour = glm::mix(it->from, it->to, k);
        world.SetStageTint(it->slot, &colour);
        shown.push_back(it->slot);
    }
    for (const std::size_t slot : tinted_) {
        if (std::find(shown.begin(), shown.end(), slot) == shown.end()) world.SetStageTint(slot, nullptr);
    }
    tinted_ = std::move(shown);
    // Held models: on their bone while they last.
    for (auto it = held_.begin(); it != held_.end();) {
        Held& h = *it;
        glm::mat4 m(1.0f);
        if (now >= h.until || (now >= h.from && !AnchorPlace(world, h.caster, h.at, m))) {
            release(h);
            it = held_.erase(it);
            continue;
        }
        if (now < h.from) {
            ++it;
            continue;
        }
        world.SetFlying(h.key, h.model, Rows(m * h.turn));
        if (!h.shown) {
            h.shown = true;
            Carry(world, h.key, h.carried, false, h.trail);
        }
        ++it;
    }
    for (auto it = flights_.begin(); it != flights_.end();) {
        Flight& f = *it;
        if (f.effect != 0 && !effects.Alive(f.effect)) {
            it = flights_.erase(it);
            continue;
        }
        if (now < f.release) {
            ++it;
            continue;
        }
        if (!f.flying && f.onTarget) {
            // A mover on the target: its two points by the target's place now, in the world's axes.
            glm::mat4 root(1.0f);
            if (!world.StageBone(f.target, {}, root)) {
                if (f.effect != 0) effects.Stop(f.effect);
                it = flights_.erase(it);
                continue;
            }
            f.start = glm::vec3(root[3]) + f.fromOffset;
            f.end = glm::vec3(root[3]) + f.toOffset;
            f.arrive = f.release + glm::length(f.end - f.start) / f.speed;
            f.flying = true;
        }
        if (!f.flying) {
            // Released: from the caster's bone to where the target is now (not followed after); a hop
            // from where it landed; AT_SOURCE from the target back to the caster.
            bool placed = false;
            if (f.toCaster) {
                placed = point(f.target, f.from, f.start) && point(f.caster, f.to, f.end);
            } else if (f.hop) {
                f.start = f.hopFrom;
                placed = point(f.target, f.to, f.end);
            } else {
                placed = point(f.caster, f.from, f.start) && point(f.target, f.to, f.end);
            }
            if (!placed) {
                if (f.effect != 0) effects.Stop(f.effect);
                it = flights_.erase(it);
                continue;
            }
            f.arrive = f.release + (f.seconds > 0.0f ? double(f.seconds) : glm::length(f.end - f.start) / f.speed);
            f.flying = true;
            // It takes over the model its swing held in that slot: the nocked arrow itself flies, its glow with it, its
            // fade kept (the held record moves into the flier); the shot's own effects are
            // added on top (BOW_POWER).
            if (f.trade > 0) {
                for (auto h = held_.begin(); h != held_.end();) {
                    if (h->cast != f.cast || h->slot != f.trade) {
                        ++h;
                        continue;
                    }
                    if (h->shown && f.key != 0) {
                        f.key = h->key; // the same model and its carried effects
                        h->shown = false;
                    } else {
                        release(*h);
                    }
                    h = held_.erase(h);
                }
            }
        }
        if (f.homing) point(f.caster, f.to, f.end); // kept as it was if the caster is gone
        const float t = static_cast<float>(std::clamp((now - f.release) / std::max(1e-3, f.arrive - f.release), 0.0, 1.0));
        const float length = glm::length(f.end - f.start);
        // The arc (col 15: its height a share of the length), up, or tilted about the line (MOV_UPR).
        glm::vec3 line = f.end - f.start;
        glm::vec3 across = glm::cross(glm::vec3(0, 1, 0), line);
        across = glm::length(across) > 1e-4f ? glm::normalize(across) : glm::vec3(1, 0, 0);
        const glm::vec3 bend = std::cos(f.tilt) * glm::vec3(0, 1, 0) + std::sin(f.tilt) * across;
        glm::vec3 at = glm::mix(f.start, f.end, t) + bend * (f.arc * length * std::sin(3.14159265f * t));
        glm::vec3 forward = line + bend * (f.arc * length * 3.14159265f * std::cos(3.14159265f * t));
        forward = glm::length(forward) > 1e-4f ? glm::normalize(forward) : glm::vec3(0, 0, 1);
        // Models and effects face -z (an arrow's tip is at z -13.5, its nock at 0): -z along the
        // flight, x level across it, y up.
        glm::vec3 side = glm::cross(glm::vec3(0, 1, 0), -forward);
        side = glm::length(side) > 1e-4f ? glm::normalize(side) : glm::vec3(1, 0, 0);
        const glm::vec3 up = glm::cross(-forward, side);
        const glm::mat4 place(glm::vec4(side, 0.0f), glm::vec4(up, 0.0f), glm::vec4(-forward, 0.0f), glm::vec4(at, 1.0f));
        if (f.key != 0) {
            world.SetFlying(f.key, f.model, Rows(place));
            if (!f.carrying) {
                f.carrying = true;
                Carry(world, f.key, f.carried, f.weapon);
            }
        } else {
            effects.Move(f.effect, place);
        }
        if (t >= 1.0f) {
            if (f.effect != 0) effects.Release(f.effect);
            f.effect = 0;
            if (f.key != 0) world.ClearFlying(f.key);
            if (!f.landSound.empty()) cues_.push_back({f.landSound, now, kNone, f.end});
            Shake(f.landShake, now, f.caster);
            if (f.splash && struckBy) {
                // On to the swing's next struck target not yet reached.
                f.visited.push_back(f.target);
                std::size_t next = kNone;
                for (const std::size_t t : struckBy(f.caster)) {
                    if (t != f.caster && std::find(f.visited.begin(), f.visited.end(), t) == f.visited.end()) {
                        next = t;
                        break;
                    }
                }
                if (next != kNone) {
                    if (!f.arrival.empty()) {
                        gfx::EffectSystem::Request request;
                        request.path = f.arrival;
                        request.at = glm::translate(glm::mat4(1.0f), f.end);
                        effects.Start(request);
                    }
                    f.hopFrom = f.end;
                    f.hop = true;
                    f.target = next;
                    f.flying = false;
                    f.release = now;
                    // Its effect again from where it landed (the one that flew here played out its landing).
                    if (!f.hopEffect.empty()) {
                        gfx::EffectSystem::Request request;
                        request.path = f.hopEffect;
                        request.scale = f.hopScale;
                        request.seconds = -1.0f; // until it lands
                        request.kind = gfx::EffectSystem::Kind::Skill;
                        request.at = glm::translate(glm::mat4(1.0f), f.end);
                        f.effect = effects.Start(request);
                    }
                    if (f.effect == 0 && f.key == 0) {
                        it = flights_.erase(it);
                        continue;
                    }
                    ++it;
                    continue;
                }
            }
            if (!f.arrival.empty()) {
                gfx::EffectSystem::Request request;
                request.path = f.arrival;
                request.at = glm::translate(glm::mat4(1.0f), f.end);
                effects.Start(request);
            }
            it = flights_.erase(it);
            continue;
        }
        ++it;
    }
}

void StageEffects::Homing(WorldScreen& world, std::size_t from, std::size_t to, const std::string& effect, double now, std::uint32_t seed,
                          const std::string& arrival, const std::string& landSound) {
    glm::mat4 root(1.0f);
    if (!world.StageBone(from, {}, root)) return;
    // The original's mover: a pause of up to 800 ms, a bend of -60..+60 degrees drawn at
    // launch, re-aimed at the moving player. Ours: the same pause after the moment the
    // monster starts to fall, a curve to one side of that bend, a second's flight.
    const std::uint32_t h = (seed + 1u) * 2654435761u;
    const double pause = 0.35 + double((h >> 4) % 801u) / 1000.0;
    gfx::EffectSystem::Request request;
    request.path = effect;
    request.seconds = -1.0f; // until it lands
    request.delay = static_cast<float>(pause);
    request.at = glm::translate(glm::mat4(1.0f), glm::vec3(root[3]));
    Flight flight;
    flight.caster = to;
    flight.target = from;
    flight.toCaster = true;
    flight.homing = true;
    flight.from.atMiddle = true;
    flight.to.atMiddle = true;
    flight.release = now + pause;
    flight.seconds = 1.0f;
    flight.arc = 0.4f;
    flight.tilt = glm::radians(90.0f + float((h >> 16) % 121u) - 60.0f);
    flight.arrival = arrival; // the original's absorption into the player (its indraft effect)
    flight.landSound = landSound;
    flight.effect = world.Effects().Start(request);
    if (flight.effect != 0) flights_.push_back(std::move(flight));
}

int StageEffects::ShakeOf(const Stage& s, bool onArrival) {
    for (const std::string& script : s.scripts) {
        const std::string_view prefix = onArrival ? "SCT_SHAKECAM_MOV" : "SCT_SHAKECAM";
        if (!script.starts_with(prefix) || script.size() != prefix.size() + 1) continue;
        const char n = script.back();
        if (n >= '0' && n <= '3') return n - '0';
    }
    return -1;
}

void StageEffects::Shake(int mode, double at, std::size_t caster) {
    static constexpr std::array<std::array<float, 3>, 4> kShakes{{{50, 500, 20}, {200, 500, 25}, {300, 500, 25}, {400, 500, 25}}};
    if (mode < 0 || mode > 3 || caster == kNone) return;
    shakes_.push_back({at, caster, kShakes[std::size_t(mode)][0], kShakes[std::size_t(mode)][1], kShakes[std::size_t(mode)][2]});
}

std::vector<StageEffects::ShakeCue> StageEffects::TakeShakes(double now) {
    std::vector<ShakeCue> due;
    for (auto it = shakes_.begin(); it != shakes_.end();) {
        if (it->at > now) {
            ++it;
            continue;
        }
        due.push_back(*it);
        it = shakes_.erase(it);
    }
    return due;
}

std::vector<StageEffects::SoundCue> StageEffects::TakeSounds(double now) {
    std::vector<SoundCue> due;
    for (auto it = cues_.begin(); it != cues_.end();) {
        if (it->at > now) {
            ++it;
            continue;
        }
        due.push_back(std::move(*it));
        it = cues_.erase(it);
    }
    return due;
}

} // namespace sr::client
