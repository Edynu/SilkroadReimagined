// The game screen's effects: what the world's events
// start in the world screen's effect system, through world/StageEffects.h (which places them on the
// stage slots the entities are shown in).
//
// - A skill's stages on its caster and target, when it swings (StageEffects::Skill).
// - Hits: the hit (or critical) effect of the attacker's last skill, the hit effect of the imbue whose
//   force the hit carried, and the target's blood (characterInfo), on the
//   bone hits land on.
// - Buffs on oneself: the skill's ACT_S stages once as it is cast, its ACT_L stages while the buff
//   lasts (matched every frame to the buff board, so the buffs had when entering the world loop too);
//   others' buffs show their ACT_S stages when cast (our protocol does not send others' buffs yet).
// - States: skilleffect.txt's PARAM_<code> list of each state a walker is
//   in: its ACT_L stages while it lasts, its DEACT stages when it ends (freeze's thaw). Which list a
//   state plays is read from the list's name (the buff board's titles; inferred).
// - Level ups: SYSTEM_LEVELUP on the one who rose.
// - Arrows and thrown models (the skill's stages, world/StageEffects.h): the nocked arrow while the bow
//   is drawn and the arrow in flight, each with the skill's trail and force (SCT_ARROW).
// - The glow of rare weapons and shields: on every player shown
//   wearing a Seal of Star / Moon / Sun item in the weapon's or shield's place, from when the dressed
//   model shows until the item comes off.

#include "play/PlayScreen.h"

#include "audio/GameAudio.h"

#include <algorithm>
#include <cmath>

#include "sr/assets/Dress.h"
#include "sr/assets/ItemGlow.h"
#include "sr/core/Path.h"
#include "sr/game/Skills.h"

namespace sr::client {

namespace proto = game::proto;

const assets::SkillRow& PlayScreen::Shown(const assets::SkillRow& skill) const {
    if (skill.original == 0 || !tables_) return skill;
    const assets::SkillRow* copied = tables_->skills.Find(skill.original);
    return copied != nullptr ? *copied : skill;
}

std::size_t PlayScreen::SlotOf(std::uint32_t entity) const {
    const auto slot = slots_.find(entity);
    return slot == slots_.end() ? StageEffects::kNone : slot->second;
}

StageEffects::Cast PlayScreen::CastOf(std::uint32_t caster, std::uint32_t target, std::string_view set, std::uint32_t shotState, std::string_view weapon,
                                      std::vector<double>& events) {
    // The shot clip's hit moments as the caster plays it (its code-1 events): the stages started by an event start at them.
    const std::size_t from = SlotOf(caster);
    events = from != StageEffects::kNone && world_ ? world_->StageEvents(from, set, shotState) : std::vector<double>{};
    for (double& t : events) t *= Stretch(caster); // a slowed caster's clip plays longer
    StageEffects::Cast cast;
    cast.shotEvents = events;
    cast.weapon = weapon;
    cast.shotState = shotState;
    const game::WorldView::Entity* casterEntity = view_.Find(caster);
    const game::WorldView::Entity* targetEntity = target != 0 ? view_.Find(target) : nullptr;
    if (casterEntity != nullptr && targetEntity != nullptr) {
        const proto::FlatPosition a = proto::ToFlat(casterEntity->position), b = proto::ToFlat(targetEntity->position);
        cast.distance = float(std::hypot(a.x - b.x, a.z - b.z));
        const assets::ObjectRow* row = tables_->characters.Find(targetEntity->look.model);
        cast.targetHit = StageEffects::HitAnchor(row != nullptr ? tables_->skillAnimations.Character(row->codename) : nullptr);
    }
    if (casterEntity != nullptr) {
        for (const proto::EquippedItem& item : casterEntity->look.equipment) {
            const assets::ItemRow* row = tables_->items.Find(item.item);
            if (row != nullptr && assets::EquipSlot(*row) == 6) cast.weaponKind = row->typeId[3];
        }
    }
    return cast;
}

void PlayScreen::SkillEffects(bool clipSounds, const assets::SkillRow& skill, std::uint32_t caster, std::uint32_t target, double now,
                              const StageEffects::Swing& swing, std::string_view set, std::uint32_t shotState, std::string_view weapon, bool deferShot) {
    if (!world_) return;
    lastSkill_[caster] = skill.id;
    const std::size_t from = SlotOf(caster);
    std::vector<double> events;
    StageEffects::Cast cast = CastOf(caster, target, set, shotState, weapon, events);
    // Its second swing sound when its clip names no sounds (K8, ours): at the shot's last hit moment
    // when it has two or more, else its first, else 0.3 into the shot.
    const bool hasBase = !skill.baseName.empty() && skill.baseName != "xxx";
    const double second = events.size() >= 2 ? events.back() : !events.empty() ? events.front() : 0.3 * (swing.end - swing.shot);
    if (!clipSounds) secondSwings_.push_back({caster, hasBase ? skill.baseName : skill.codename, now + swing.shot + second});
    const assets::SkillRow& shown = Shown(skill); // the stages of the skill it copies, if any
    const auto* stages = StageEffects::StagesOf(tables_->skillAnimations, shown);
    // Its hits wait for its damage moments (HandleEvent), the caster shown or not.
    strikeTimes_.erase(caster);
    resultsAt_.erase(caster);
    if (stages != nullptr) {
        std::vector<double> moments = StageEffects::DamageMoments(*stages, swing, events, cast.distance);
        for (double& m : moments) m += now;
        if (!moments.empty()) {
            strikeTimes_[caster] = std::move(moments);
        } else if (!stages->empty()) {
            resultsAt_[caster] = now + swing.end; // no damage group: its results at its end (P22)
        }
    }
    if (stages == nullptr || from == StageEffects::kNone) return; // the caster not shown
    // A skill's ACT_S shows on whom it takes hold: a buff's with the buff (BuffAdded, EntityBuffs), a
    // heal's, a cure's and a resurrection's with proto::SkillTook, a link's with its copy (EntityBuffs on its
    // holder, a monster too); the rest's with the swing, at its shot.
    const game::SkillTraits traits = game::TraitsOf(skill);
    const game::ThreatSpec threat = game::ThreatOf(skill);
    const bool actStart = !(traits.buff || traits.toggle || traits.imbue || traits.heal || traits.cure || traits.resurrect || threat.raise || threat.cut || traits.link);
    const auto* animation = tables_->skillAnimations.Find(AnimationKey(shown));
    const std::uint32_t castNo = stageEffects_.Skill(*world_, *stages, from, target == 0 ? StageEffects::kNone : SlotOf(target), now, swing, actStart, animation,
                                                     cast, deferShot ? StageEffects::Part::BeforeShot : StageEffects::Part::All);
    if (deferShot) {
        PendingShot p;
        p.skill = skill.id;
        p.target = target;
        p.state = shotState;
        p.castNo = castNo;
        p.swing = swing;
        p.start = now;
        p.planned = now + swing.shot;
        p.set = std::string(set);
        p.weapon = std::string(weapon);
        p.actStart = actStart;
        pendingShots_[caster] = std::move(p);
    }
}

void PlayScreen::StartShot(std::uint32_t caster, double at) {
    const auto it = pendingShots_.find(caster);
    if (it == pendingShots_.end()) return;
    const PendingShot p = std::move(it->second);
    pendingShots_.erase(it);
    const double delay = at - p.planned;
    const double now = p.start + delay; // the swing's start as if its shot had been planned for `at`
    const assets::SkillRow* own = tables_ ? tables_->skills.Find(p.skill) : nullptr;
    const assets::SkillRow* skill = own != nullptr ? &Shown(*own) : nullptr; // its presentation's
    const auto* stages = skill != nullptr ? StageEffects::StagesOf(tables_->skillAnimations, Shown(*skill)) : nullptr;
    std::vector<double> events;
    const StageEffects::Cast cast = CastOf(caster, p.target, p.set, p.state, p.weapon, events);
    // Its hits' moments from its shot now.
    if (stages != nullptr) {
        std::vector<double> moments = StageEffects::DamageMoments(*stages, p.swing, events, cast.distance);
        for (double& m : moments) m += now;
        if (!moments.empty()) strikeTimes_[caster] = std::move(moments);
    }
    if (const auto r = resultsAt_.find(caster); r != resultsAt_.end()) r->second += delay;
    // Its shot's stages.
    if (world_ && stages != nullptr && skill != nullptr) {
        if (const std::size_t from = SlotOf(caster); from != StageEffects::kNone) {
            stageEffects_.Skill(*world_, *stages, from, p.target == 0 ? StageEffects::kNone : SlotOf(p.target), now, p.swing, p.actStart,
                                tables_->skillAnimations.Find(AnimationKey(*skill)), cast, StageEffects::Part::Shot, p.castNo, at);
        }
    }
    // Its body: the steps before the shot end at `at`, the shot's moves with it.
    if (const auto m = motions_.find(caster); m != motions_.end() && !m->second.steps.empty()) {
        Motion& motion = m->second;
        for (std::size_t i = 0; i + 1 < motion.steps.size(); ++i) {
            motion.steps[i].until = i + 2 == motion.steps.size() ? at : std::min(motion.steps[i].until, at);
        }
        motion.steps.back().until += delay;
        if (motion.shotFrom >= 0.0) motion.shotFrom += delay;
        if (motion.trailOn >= 0.0) {
            motion.trailOn += delay;
            motion.trailOff += delay;
        }
        if (motion.hideFrom >= p.planned) motion.hideFrom += delay;
    }
    // Its shot clip's sounds and its second swing sound.
    for (ClipSound& s : clipSounds_) {
        if (s.entity == caster && s.at >= p.planned) s.at += delay;
    }
    for (SecondSwing& s : secondSwings_) {
        if (s.entity == caster && s.at >= p.planned) s.at += delay;
    }
}

void PlayScreen::HitEffects(const proto::EntityDamaged& hit) {
    const bool blocked = (hit.flags & proto::kDamageBlocked) != 0;
    if (!world_ || (hit.damage == 0 && !blocked)) return;
    const game::WorldView::Entity* target = view_.Find(hit.target);
    const std::size_t slot = SlotOf(hit.target);
    if (target == nullptr || slot == StageEffects::kNone) return;
    // Where hits land on the target's body (characterInfo), else its middle.
    const assets::ObjectRow* row = tables_->characters.Find(target->look.model);
    const assets::CharacterEffectInfo* info = row != nullptr ? tables_->skillAnimations.Character(row->codename) : nullptr;
    const StageEffects::Stage::Anchor at = StageEffects::HitAnchor(info);
    // The attacker's skill (its last swing): its hit or critical effect. A companion's strike shows its
    // own skill's (its col 13, below, as an imbue's) and the blood.
    const bool companion = (hit.flags & (proto::kDamageCompanion | proto::kDamageReturn | proto::kDamageBurst)) != 0;
    // The target's own guard first: the DefenseEfp (col 12) of a buff it holds (a shield-type one),
    // else the attacker's (then).
    std::string defence;
    {
        std::vector<std::uint32_t> held;
        if (hit.target == view_.Self()) {
            for (const auto& [token, skill] : buffSkills_) held.push_back(skill);
        } else if (const auto b = buffed_.find(hit.target); b != buffed_.end()) {
            held = b->second.skills;
        }
        for (const std::uint32_t id : held) {
            const assets::SkillRow* s = tables_->skills.Find(id);
            const assets::SkillAnimation* anim = s != nullptr ? tables_->skillAnimations.Find(AnimationKey(Shown(*s))) : nullptr;
            if (anim != nullptr && !anim->defenseEffect.empty()) {
                defence = anim->defenseEffect;
                break;
            }
        }
    }
    if (!defence.empty()) {
        stageEffects_.Play(*world_, slot, defence, at, gfx::EffectSystem::Kind::Hit);
        if (blocked) return;
    } else if (!companion) {
        // One effect: from the attacker's live presentation of the highest Priority (col 2) with a DamageEfp, its imbue's
        // (2) over its attack's (0) while one is on, whether or not this hit carried its share. Its DamageEfp (col 13; a
        // blocked one's too: the target holds no guard
        // of its own), its critical one (col 18) on a critical; no blood on a blocked one.
        const assets::SkillAnimation* best = nullptr;
        const auto consider = [&](std::uint32_t id) {
            const assets::SkillRow* s = id != 0 ? tables_->skills.Find(id) : nullptr;
            if (s == nullptr) return;
            const assets::SkillAnimation* anim = tables_->skillAnimations.Find(AnimationKey(Shown(*s)));
            if (anim != nullptr && !anim->damageEffect.empty() && (best == nullptr || anim->priority > best->priority)) best = anim;
        };
        if (const auto skill = lastSkill_.find(hit.attacker); skill != lastSkill_.end()) consider(skill->second);
        consider(hit.imbue);
        if (hit.attacker == view_.Self()) {
            for (const auto& [token, skill] : buffSkills_) consider(skill);
        } else if (const auto b = buffed_.find(hit.attacker); b != buffed_.end()) {
            for (const std::uint32_t id : b->second.skills) consider(id);
        }
        if (best != nullptr) {
            const bool critical = (hit.flags & proto::kDamageCritical) != 0;
            const std::string& effect = critical && !best->criticalEffect.empty() ? best->criticalEffect : best->damageEffect;
            stageEffects_.Play(*world_, slot, effect, at, gfx::EffectSystem::Kind::Hit);
            if (blocked) return;
        }
    }
    // A companion's strike, a pulse's, an object's, a burst's: its own skill's hit effect (its row's column 13).
    if (const assets::SkillRow* imbue = companion && hit.imbue != 0 && !blocked ? tables_->skills.Find(hit.imbue) : nullptr) {
        const bool named = !imbue->baseName.empty() && imbue->baseName != "xxx";
        if (const assets::SkillAnimation* anim = tables_->skillAnimations.Find(named ? imbue->baseName : imbue->codename)) {
            stageEffects_.Play(*world_, slot, anim->damageEffect, at, gfx::EffectSystem::Kind::Hit);
        }
    }
    if (info != nullptr && !blocked) stageEffects_.Play(*world_, slot, info->blood, at, gfx::EffectSystem::Kind::Hit);
    // A pulse's, an object's, a reflection's: its S_RETURN on the target; a time bomb's burst: the stages
    // at its source, the holder.
    const bool burst = (hit.flags & proto::kDamageBurst) != 0;
    if (const assets::SkillRow* returned = (hit.flags & proto::kDamageReturn) != 0 || burst ? tables_->skills.Find(hit.imbue) : nullptr) {
        if (const auto* stages = StageEffects::StagesOf(tables_->skillAnimations, Shown(*returned))) {
            stageEffects_.ReturnHit(*world_, *stages, slot, SlotOf(hit.attacker), cameraNow_, burst);
        }
    }
}

void PlayScreen::BuffEffects(std::uint32_t token, std::uint32_t skillId, bool added) {
    if (!world_) return;
    if (!added) {
        if (const auto kept = buffEffects_.find(token); kept != buffEffects_.end()) {
            for (const gfx::EffectSystem::Handle h : kept->second) stageEffects_.Release(*world_, h);
            buffEffects_.erase(kept);
        }
        // Its end's stages (DEACT, a toggle's ACT_OE) and their sounds (K8).
        if (const auto known = buffSkills_.find(token); known != buffSkills_.end()) {
            const assets::SkillRow* skill = tables_->skills.Find(known->second);
            const std::size_t slot = SlotOf(view_.Self());
            const auto* stages = skill != nullptr ? StageEffects::StagesOf(tables_->skillAnimations, Shown(*skill)) : nullptr;
            if (stages != nullptr && slot != StageEffects::kNone) stageEffects_.StateEnd(*world_, *stages, slot);
            buffSkills_.erase(known);
        }
        return;
    }
    buffSkills_[token] = skillId;
    // Its start (ACT_S) once as it is cast; not for the buffs the server sends again at an entry
    // (the loading screen is up). Its loop comes with BuffLoops, for those too.
    const std::size_t slot = SlotOf(view_.Self());
    if (!shown_ || slot == StageEffects::kNone) return;
    const assets::SkillRow* skill = tables_->skills.Find(skillId);
    if (const auto* stages = skill != nullptr ? StageEffects::StagesOf(tables_->skillAnimations, Shown(*skill)) : nullptr) {
        stageEffects_.BuffStart(*world_, *stages, slot);
    }
}

void PlayScreen::BerserkEffects(std::uint32_t entity, bool on, std::uint8_t variant, bool burst) {
    // The mode's stages (skilleffect.txt, both races): the first mode SYSTEM_CH_HWANMODE (the body's
    // colour dark red to red), the second SYSTEM_CH_HWAN2MODE (dark blue to light blue), the party link's
    // SYSTEM_CH_HWAN2LINKMODE; its start (the burst and hwanchange.wav), its loops while it lasts (the bones' glows,
    // the body's colour, SCT_MAT), its end (the vanishing and hwanreturn.wav); and the body's growth, SCT_CHAR_SCALE (1.1
    // the first, 1.2 the second and the link's, over 1,000 ms).
    if (!world_ || !tables_ || !tables_->Usable()) return;
    const char* name = variant == 2 ? "SYSTEM_CH_HWAN2LINKMODE" : variant == 1 ? "SYSTEM_CH_HWAN2MODE" : "SYSTEM_CH_HWANMODE";
    const auto* stages = tables_->skillAnimations.Stages(name);
    if (stages == nullptr) stages = tables_->skillAnimations.Stages("SYSTEM_CH_HWANMODE");
    if (stages != nullptr) {
        float scale = 1.0f, ms = 0.0f;
        if (StageEffects::CharScaleOf(*stages, scale, ms)) {
            Grow& g = grows_[entity];
            const float current = g.At(now_);
            g.from = current;
            g.to = on ? scale : 1.0f;
            g.start = now_;
            g.seconds = std::max(0.0f, ms) / 1000.0;
        }
    }
    const std::size_t slot = SlotOf(entity);
    if (stages == nullptr || slot == StageEffects::kNone) return;
    if (const auto kept = berserkEffects_.find(entity); kept != berserkEffects_.end()) {
        for (const gfx::EffectSystem::Handle h : kept->second) stageEffects_.Release(*world_, h);
        berserkEffects_.erase(kept);
        if (!on) stageEffects_.StateEnd(*world_, *stages, slot);
    }
    if (!on) return;
    if (shown_ && burst) stageEffects_.BuffStart(*world_, *stages, slot);
    berserkEffects_[entity] = stageEffects_.BuffLoops(*world_, *stages, slot);
}

void PlayScreen::TakeBuffs(const proto::EntityBuffs& message, double now) {
    if (message.entity == view_.Self()) return; // the player's own: BuffAdded / BuffRemoved
    Buffed& b = buffed_[message.entity];
    const std::size_t slot = SlotOf(message.entity);
    const auto stagesOf = [&](std::uint32_t id) -> const StageEffects::Stages* {
        const assets::SkillRow* skill = tables_ && tables_->Usable() ? tables_->skills.Find(id) : nullptr;
        return skill != nullptr ? StageEffects::StagesOf(tables_->skillAnimations, Shown(*skill)) : nullptr;
    };
    // Those gone: their loops fade, their end shows (DEACT).
    for (const std::uint32_t id : b.skills) {
        if (std::find(message.skills.begin(), message.skills.end(), id) != message.skills.end()) continue;
        if (const auto l = b.loops.find(id); l != b.loops.end()) {
            if (world_) {
                for (const StageEffects::Handle h : l->second) stageEffects_.Release(*world_, h);
            }
            b.loops.erase(l);
        }
        if (const auto* stages = stagesOf(id); world_ && stages != nullptr && slot != StageEffects::kNone) stageEffects_.StateEnd(*world_, *stages, slot);
    }
    // Those come: their start (ACT_S), but not for those told as it appears (the original shows only
    // their loop on a spawn); their loops come with OthersBuffLoops.
    const auto appeared = appeared_.find(message.entity);
    const bool justAppeared = appeared != appeared_.end() && now - appeared->second < 0.5;
    for (const std::uint32_t id : message.skills) {
        if (std::find(b.skills.begin(), b.skills.end(), id) != b.skills.end() || justAppeared) continue;
        if (const auto* stages = stagesOf(id); world_ && shown_ && stages != nullptr && slot != StageEffects::kNone) stageEffects_.BuffStart(*world_, *stages, slot);
    }
    b.skills = message.skills;
    // A bandit's imbue stays on it (it is its look, not one of the server's buffs).
    if (const auto imbue = banditImbues_.find(message.entity);
        imbue != banditImbues_.end() && std::find(b.skills.begin(), b.skills.end(), imbue->second) == b.skills.end()) {
        b.skills.push_back(imbue->second);
    }
    if (b.skills.empty() && b.loops.empty()) buffed_.erase(message.entity);
}

void PlayScreen::OthersBuffLoops() {
    // The bandits' imbues (the 1.188 client's look: the dresser starts the imbue's buff presentation on the bandit):
    // among their buffs while they are in view, so their loops show and their hits take the imbue's effect.
    for (auto it = banditImbues_.begin(); it != banditImbues_.end();) {
        if (view_.Find(it->first) == nullptr) {
            it = banditImbues_.erase(it);
            continue;
        }
        std::vector<std::uint32_t>& skills = buffed_[it->first].skills;
        if (std::find(skills.begin(), skills.end(), it->second) == skills.end()) skills.push_back(it->second);
        ++it;
    }
    for (auto it = buffed_.begin(); it != buffed_.end();) {
        Buffed& b = it->second;
        // Gone from view, or shown in another slot: its loops go (they come again on the new slot).
        const bool known = view_.Find(it->first) != nullptr;
        const std::size_t slot = known ? SlotOf(it->first) : StageEffects::kNone;
        if (slot != b.slot) {
            for (auto& [id, handles] : b.loops) {
                for (const StageEffects::Handle h : handles) stageEffects_.Release(*world_, h);
            }
            b.loops.clear();
            b.slot = slot;
        }
        if (!known) {
            it = buffed_.erase(it);
            continue;
        }
        // Each buff's loops once its walker shows (tried once, even with no loop).
        if (slot != StageEffects::kNone && world_->StageReady(slot)) {
            for (const std::uint32_t id : b.skills) {
                if (b.loops.contains(id)) continue;
                std::vector<StageEffects::Handle>& loops = b.loops[id];
                const assets::SkillRow* skill = tables_->skills.Find(id);
                if (const auto* stages = skill != nullptr ? StageEffects::StagesOf(tables_->skillAnimations, Shown(*skill)) : nullptr) {
                    loops = stageEffects_.BuffLoops(*world_, *stages, slot);
                }
            }
        }
        ++it;
    }
}

glm::vec3 PlayScreen::ObjectPoint(const game::WorldView::Entity& e) const {
    // Where it stands: the floor its owner stands on when the owner is there (it comes at the owner's
    // feet), else the floor nearest its height (the terrain, or a bridge's or stairs' object floor).
    if (const auto shown = objectEffects_.find(e.id); shown != objectEffects_.end()) return shown->second.at;
    proto::WorldPosition p = e.position;
    const game::WorldView::Entity* owner = e.amount != 0 ? view_.Find(e.amount) : nullptr;
    const proto::FlatPosition a = proto::ToFlat(p);
    if (owner != nullptr) {
        const proto::FlatPosition b = proto::ToFlat(owner->position);
        if (std::hypot(a.x - b.x, a.z - b.z) < 3.0) p.y = owner->position.y;
    }
    if (owner == nullptr || p.y != owner->position.y) {
        if (const game::NavPosition floor = world_->Navigation().Drop(p.region, p.x, p.y, p.z); floor.Valid()) p.y = floor.y;
    }
    return world_->OriginRelative(p);
}

void PlayScreen::SkillObjects() {
    // Those gone: their effect fades (released: what it made finishes).
    for (auto it = objectEffects_.begin(); it != objectEffects_.end();) {
        const game::WorldView::Entity* e = view_.Find(it->first);
        if (e != nullptr && e->kind == proto::EntityKind::SkillObject) {
            ++it;
            continue;
        }
        if (it->second.effect != 0) stageEffects_.Release(*world_, it->second.effect);
        it = objectEffects_.erase(it);
    }
    // Those come: their skill's object effect on the ground where they stand (tried once).
    for (const auto& [id, e] : view_.Entities()) {
        if (e.kind != proto::EntityKind::SkillObject || objectEffects_.contains(id)) continue;
        const assets::SkillRow* skill = tables_->skills.Find(e.look.model);
        const bool named = skill != nullptr && !skill->baseName.empty() && skill->baseName != "xxx";
        const assets::SkillAnimation* anim = skill != nullptr ? tables_->skillAnimations.Find(named ? skill->baseName : skill->codename) : nullptr;
        const glm::vec3 at = ObjectPoint(e);
        objectEffects_[id] = {anim != nullptr ? stageEffects_.ObjectEffect(*world_, anim->objectEffect, at, e.yaw) : 0, at};
    }
}

void PlayScreen::ObjectStruck(const proto::SkillObjectStruck& message, double now) {
    if (!world_ || !tables_ || !tables_->Usable()) return;
    const game::WorldView::Entity* e = view_.Find(message.object);
    const assets::SkillRow* skill = tables_->skills.Find(message.skill);
    const auto* stages = skill != nullptr ? StageEffects::StagesOf(tables_->skillAnimations, Shown(*skill)) : nullptr;
    if (e == nullptr || stages == nullptr) return;
    stageEffects_.ObjectStruck(*world_, *stages, ObjectPoint(*e), SlotOf(e->amount), now);
}

void PlayScreen::KeptEffects(ScreenContext& context) {
    if (!world_ || !tables_ || !tables_->Usable()) return;
    BuffLoops();
    OthersBuffLoops();
    SkillObjects();
    StateLoops(context.time);
    ItemGlows(context);
    ActionMarks(context);
}

// --- States -------------------------------------------------------------------------------------

void PlayScreen::TakeStates(const proto::EntityStates& message, double now) {
    // The server's list is all the states the entity is in: those gone end (their end effect, their
    // loops released), those still there keep when they began.
    Afflicted& a = afflicted_[message.entity];
    std::vector<ShownState> next;
    for (const proto::StateRecord& r : message.states) {
        ShownState s{game::StateKind(r.kind), now, now + double(r.ms) / 1000.0};
        for (const ShownState& old : a.states) {
            if (old.kind == s.kind) s.since = old.since;
        }
        next.push_back(s);
    }
    for (const ShownState& old : a.states) {
        if (std::any_of(next.begin(), next.end(), [&](const ShownState& s) { return s.kind == old.kind; })) continue;
        const auto loops = a.loops.find(std::uint8_t(old.kind));
        if (loops != a.loops.end()) {
            if (world_) {
                for (const gfx::EffectSystem::Handle h : loops->second) stageEffects_.Release(*world_, h);
            }
            a.loops.erase(loops);
        }
        const std::size_t slot = SlotOf(message.entity);
        const auto* stages = tables_ && tables_->Usable() ? tables_->skillAnimations.Stages(StateTitle(game::StateBit(old.kind))) : nullptr;
        if (world_ && stages != nullptr && slot != StageEffects::kNone) stageEffects_.StateEnd(*world_, *stages, slot);
    }
    a.states = std::move(next);
    // The player's own on its buff board (the lower rows; their gauge from when each was first seen).
    if (message.entity == view_.Self()) {
        std::erase_if(player_.effects, [](const game::ActiveEffect& e) { return e.kind == game::ActiveEffect::Kind::State; });
        for (const game::ActiveEffect& e : StatesOf(message.entity, now)) player_.effects.push_back(e);
    }
    if (a.states.empty() && a.loops.empty()) afflicted_.erase(message.entity);
}

void PlayScreen::StateLoops(double now) {
    for (auto it = afflicted_.begin(); it != afflicted_.end();) {
        Afflicted& a = it->second;
        const std::uint32_t entity = it->first;
        // Gone from view, or shown in another slot: its loops go (they come again on the new slot).
        const bool known = view_.Find(entity) != nullptr;
        const std::size_t slot = known ? SlotOf(entity) : StageEffects::kNone;
        const auto release = [&](std::vector<gfx::EffectSystem::Handle>& handles) {
            for (const gfx::EffectSystem::Handle h : handles) stageEffects_.Release(*world_, h);
            handles.clear();
        };
        if (slot != a.slot) {
            for (auto& [kind, handles] : a.loops) release(handles);
            a.loops.clear();
            a.slot = slot;
        }
        // Past their time (the server's end is on its way): out of the picture.
        std::erase_if(a.states, [&](const ShownState& s) { return now > s.until + 1.0; });
        for (auto l = a.loops.begin(); l != a.loops.end();) {
            if (std::none_of(a.states.begin(), a.states.end(), [&](const ShownState& s) { return std::uint8_t(s.kind) == l->first; })) {
                release(l->second);
                l = a.loops.erase(l);
            } else {
                ++l;
            }
        }
        if (!known || (a.states.empty() && a.loops.empty())) {
            it = afflicted_.erase(it);
            continue;
        }
        // Each state's loops once its walker shows.
        if (slot != StageEffects::kNone && world_->StageReady(slot)) {
            for (const ShownState& s : a.states) {
                if (a.loops.contains(std::uint8_t(s.kind))) continue;
                std::vector<gfx::EffectSystem::Handle>& loops = a.loops[std::uint8_t(s.kind)]; // tried once, even with no loop
                if (const auto* stages = tables_->skillAnimations.Stages(StateTitle(game::StateBit(s.kind)))) {
                    loops = stageEffects_.BuffLoops(*world_, *stages, slot);
                }
            }
        }
        ++it;
    }
}

double PlayScreen::Stretch(std::uint32_t entity) const {
    std::uint32_t mask = 0;
    if (const auto it = afflicted_.find(entity); it != afflicted_.end()) {
        for (const ShownState& s : it->second.states) mask |= game::StateBit(s.kind);
    }
    return double(game::AttackTimePercent(mask)) / 100.0;
}

bool PlayScreen::InState(std::uint32_t entity, game::StateKind kind, double now) const {
    const auto it = afflicted_.find(entity);
    if (it == afflicted_.end()) return false;
    return std::any_of(it->second.states.begin(), it->second.states.end(), [&](const ShownState& s) { return s.kind == kind && now <= s.until; });
}

std::vector<game::ActiveEffect> PlayScreen::StatesOf(std::uint32_t entity, double now) const {
    std::vector<game::ActiveEffect> out;
    const auto it = afflicted_.find(entity);
    if (it == afflicted_.end()) return out;
    for (const ShownState& s : it->second.states) {
        if (now > s.until) continue;
        game::ActiveEffect e;
        e.kind = game::ActiveEffect::Kind::State;
        e.id = game::StateBit(s.kind);
        e.token = std::uint8_t(s.kind);
        e.debuff = true;
        e.started = s.since;
        e.duration = std::max(0.0, s.until - s.since);
        out.push_back(e);
    }
    return out;
}

void PlayScreen::BuffLoops() {
    const auto active = [&](std::uint32_t token) {
        return std::any_of(player_.effects.begin(), player_.effects.end(),
                           [&](const game::ActiveEffect& a) { return a.kind == game::ActiveEffect::Kind::Skill && a.token == token; });
    };
    // Buffs that ended: their loops fade out.
    for (auto it = buffEffects_.begin(); it != buffEffects_.end();) {
        if (active(it->first)) {
            ++it;
            continue;
        }
        for (const gfx::EffectSystem::Handle h : it->second) stageEffects_.Release(*world_, h);
        it = buffEffects_.erase(it);
    }
    // Buffs without their loop (just cast, or had when entering the world): once the player shows.
    const std::size_t slot = SlotOf(view_.Self());
    if (slot == StageEffects::kNone || !world_->StageReady(slot)) return;
    for (const game::ActiveEffect& a : player_.effects) {
        if (a.kind != game::ActiveEffect::Kind::Skill || buffEffects_.contains(a.token)) continue;
        std::vector<gfx::EffectSystem::Handle>& loops = buffEffects_[a.token]; // tried once, even with no loop
        const assets::SkillRow* skill = tables_->skills.Find(a.id);
        if (const auto* stages = skill != nullptr ? StageEffects::StagesOf(tables_->skillAnimations, Shown(*skill)) : nullptr) {
            loops = stageEffects_.BuffLoops(*world_, *stages, slot);
        }
    }
}

void PlayScreen::ItemGlows(ScreenContext& context) {
    gfx::EffectSystem& effects = world_->Effects();
    const auto stop = [&](ItemGlow& glow) {
        for (const gfx::EffectSystem::Handle h : glow.effects) effects.Stop(h);
        glow.effects.clear();
    };
    // Those no longer shown, shown in another slot or hidden by a quest scene: their glows go with them.
    for (auto it = itemGlows_.begin(); it != itemGlows_.end();) {
        const auto slot = slots_.find(it->first);
        if (slot != slots_.end() && slot->second == it->second.slot && !SceneHides(it->first)) {
            ++it;
            continue;
        }
        stop(it->second);
        it = itemGlows_.erase(it);
    }
    std::vector<std::uint32_t> worn;
    std::vector<std::uint8_t> plus;
    for (const auto& [id, slot] : slots_) {
        const game::WorldView::Entity* e = view_.Find(id);
        if (e == nullptr || e->kind != proto::EntityKind::Character || IsNpc(*e) || SceneHides(id)) continue; // one fading out keeps what it had
        auto known = itemGlows_.find(id);
        if (known != itemGlows_.end() && known->second.lookVersion == e->lookVersion) continue;
        // Dressed anew: the old glows go at once, the new ones come when the model wearing them shows.
        if (known != itemGlows_.end()) stop(known->second);
        if (!world_->StageReady(slot)) continue;
        ItemGlow& glow = itemGlows_[id];
        glow.lookVersion = e->lookVersion;
        glow.slot = slot;
        worn.clear();
        plus.clear();
        for (const proto::EquippedItem& item : e->look.equipment) {
            worn.push_back(item.item);
            plus.push_back(item.plus);
        }
        glow.effects = StageEffects::ItemGlows(*world_, slot, context.files, tables_->items, tables_->itemGlows, worn, plus);
    }
}

void PlayScreen::LevelUpEffect(std::uint32_t entity, double now) {
    if (!world_ || !tables_ || !tables_->Usable()) return;
    // Once (the own player hears of it twice: its progress and the broadcast).
    if (const auto at = leveledAt_.find(entity); at != leveledAt_.end() && now - at->second < 2.0) return;
    leveledAt_[entity] = now;
    const auto* stages = tables_->skillAnimations.Stages("SYSTEM_LEVELUP");
    const std::size_t slot = SlotOf(entity);
    if (stages == nullptr || slot == StageEffects::kNone) return;
    for (const assets::SkillEffectStage& s : *stages) {
        // The original shows it whatever the effect quality.
        stageEffects_.Play(*world_, slot, s.object, s.start, gfx::EffectSystem::Kind::World);
    }
}

// A kill's orbs, from the monster to the player, a party member's kill too (each member is told its
// share). Their number
// by what was gained (the experience over a general monster's of the player's level, the skill
// experience over 100); none below the effects' quality 2, as the original.
void PlayScreen::KillOrbs(const proto::ExperienceGained& gained, double now) {
    if (!world_ || !tables_ || !tables_->Usable()) return;
    const std::size_t from = SlotOf(gained.from), self = SlotOf(view_.Self());
    if (from == StageEffects::kNone || self == StageEffects::kNone) return;
    const auto count = [](double r) {
        for (const auto& [least, n] : {std::pair{64.0, 10}, {32.0, 9}, {16.0, 8}, {8.0, 7}, {4.0, 6}, {2.0, 5}, {1.0, 4}}) {
            if (r >= least) return n;
        }
        return std::max(1, 3 - int(5.0 * (1.0 - r)));
    };
    // Each lands as the original's orbs do: its kind's absorption (the experience's blue indraft, the skill experience's
    // red) and hyanget.wav.
    const auto send = [&](const char* effect, const char* arrival, int n) {
        for (int i = 0; i < n; ++i) stageEffects_.Homing(*world_, from, self, effect, now, orbsSent_++, arrival, "prim\\snd\\ui\\hyanget.wav");
    };
    const std::size_t level = player_.self.level;
    const double basis = level < tables_->monsterExperience.size() ? double(tables_->monsterExperience[level]) : 0.0;
    if (gained.experience > 0) send("battle\\hwan_g.efp", "battle\\hwn_blue_indraft.efp", basis > 0.0 ? count(double(gained.experience) / basis) : 3);
    if (gained.skillExperience > 0) send("battle\\hwan_y.efp", "battle\\hwn_red_indraft.efp", count(double(gained.skillExperience) / 100.0));
}

// An item used (ItemUsed, the original's 0x305C), on the user, as the 1.188 client picks it: a potion's
// SYSTEM_HPPOTION (TID4 1), SYSTEM_MPPOTION (2), SYSTEM_LIFE (3), the pets' SYSTEM_COS_HPPOTION (4),
// SYSTEM_COS_MPPOTION (5), SYSTEM_COS_HGPPOTION (9); a pill's PARAM_CURE_ALL (TID4 1, 6) or
// STATUS_CURE_COS (7); any scroll's SYSTEM_RETURNSCROLL; else a list named as the item's codename
// (fireworks), when there is one.
std::string PlayScreen::ItemUseEffectName(const assets::ItemRow& row) {
    if (row.typeId[0] == 3 && row.typeId[1] == 3) {
        const int kind = row.typeId[3];
        switch (row.typeId[2]) {
        case 1:
            switch (kind) {
            case 1: return "SYSTEM_HPPOTION";
            case 2: return "SYSTEM_MPPOTION";
            case 3: return "SYSTEM_LIFE";
            case 4: return "SYSTEM_COS_HPPOTION";
            case 5: return "SYSTEM_COS_MPPOTION";
            case 9: return "SYSTEM_COS_HGPPOTION";
            default: return {};
            }
        case 2: return kind == 1 || kind == 6 ? "PARAM_CURE_ALL" : kind == 7 ? "STATUS_CURE_COS" : std::string();
        case 3: return "SYSTEM_RETURNSCROLL";
        default: break;
        }
    }
    return row.codename;
}

void PlayScreen::ItemUseEffect(std::uint32_t entity, std::uint32_t item) {
    if (!world_ || !tables_ || !tables_->Usable()) return;
    const assets::ItemRow* row = tables_->items.Find(item);
    const std::string name = row != nullptr ? ItemUseEffectName(*row) : std::string();
    const auto* stages = name.empty() ? nullptr : tables_->skillAnimations.Stages(name);
    const std::size_t slot = SlotOf(entity);
    if (stages == nullptr || slot == StageEffects::kNone) return;
    for (const assets::SkillEffectStage& s : *stages) {
        if (!s.object.empty()) stageEffects_.Play(*world_, slot, s.object, s.start, gfx::EffectSystem::Kind::World);
        // Its sound (SndBegin), after the stage's delay (the move column's): a firework's launch at once, its bursts
        // 1.3 s and 2 s on (skilleffect.txt's rows of a sound and no object; inferred from them).
        stageEffects_.Sound(s.soundBegin, now_ + s.moveDelayMs / 1000.0, slot);
    }
    core::LogInfo("game: {} uses {} ({})", entity, row->codename, name);
}

void PlayScreen::FlyEffects(ScreenContext& context) {
    if (!world_) return;
    // A shot whose impact never came: from 400 ms past its time by the data (a skill tick and the way here).
    for (auto it = pendingShots_.begin(); it != pendingShots_.end();) {
        if (context.time < it->second.planned + 0.4) {
            ++it;
            continue;
        }
        const std::uint32_t caster = it->first;
        ++it; // (StartShot takes it out)
        ++shotsLate_;
        StartShot(caster, context.time);
    }
    stageEffects_.Fly(*world_, context.time);
    // The player's own skills shake the camera (others' do not).
    const std::size_t self = SlotOf(view_.Self());
    for (const StageEffects::ShakeCue& cue : stageEffects_.TakeShakes(context.time)) {
        if (cue.caster != self || self == StageEffects::kNone) continue;
        shake_.amplitude = std::max(shake_.amplitude * float(context.time < shake_.until), cue.amplitude); // a running one's kept
        shake_.from = context.time;
        shake_.until = context.time + cue.ms / 1000.0;
        shake_.step = std::max(0.005f, cue.step / 1000.0f);
        shake_.next = context.time;
    }
    // The stages' sounds due now, where they are: a slot's character about its middle, a flight's landing.
    for (const StageEffects::SoundCue& cue : stageEffects_.TakeSounds(context.time)) {
        glm::vec3 p = cue.point;
        if (cue.slot != StageEffects::kNone) {
            glm::mat4 m(1.0f);
            if (!world_->StageBone(cue.slot, {}, m)) continue;
            p = glm::vec3(m[3]) + glm::vec3(0.0f, 10.0f, 0.0f);
        }
        ++benchSoundCues_;
        benchSounds_.insert(cue.path);
        if (context.audio != nullptr) context.audio->PlayEffect(cue.path, p);
    }
    // The hits' sounds 100 ms after them, where their targets are then (P12).
    for (auto it = hitSounds_.begin(); it != hitSounds_.end();) {
        if (it->at > context.time) {
            ++it;
            continue;
        }
        const game::WorldView::Entity* e = view_.Find(it->target);
        if (e != nullptr && context.audio != nullptr) {
            const game::WorldView::Entity* a = view_.Find(it->attacker);
            const bool player = a != nullptr && IsPlayer(*a);
            context.audio->Hit(it->attacker, it->critical, world_->OriginRelative(e->position) + glm::vec3(0.0f, 10.0f, 0.0f), player,
                               berserkers_.contains(it->attacker));
        }
        it = hitSounds_.erase(it);
    }
    // The swings' second sounds, where the attacker is then.
    for (auto it = secondSwings_.begin(); it != secondSwings_.end();) {
        if (it->at > context.time) {
            ++it;
            continue;
        }
        const game::WorldView::Entity* e = view_.Find(it->entity);
        if (e != nullptr && context.audio != nullptr) context.audio->SecondSwing(it->skill, world_->OriginRelative(e->position) + glm::vec3(0.0f, 10.0f, 0.0f));
        it = secondSwings_.erase(it);
    }
    // The clips' own sounds at their moments, unless the clip they belong to was cut short.
    for (auto it = clipSounds_.begin(); it != clipSounds_.end();) {
        if (it->at > context.time) {
            ++it;
            continue;
        }
        const game::WorldView::Entity* e = view_.Find(it->entity);
        const auto m = motions_.find(it->entity);
        const bool playing = e != nullptr && m != motions_.end() && !m->second.steps.empty() && // over a walk too (P6)
                             std::any_of(m->second.steps.begin(), m->second.steps.end(), [&](const Motion::Step& s) { return s.take == it->take; });
        if (playing && context.audio != nullptr) {
            context.audio->ClipSound(it->handle, it->skill, it->weapon, it->codename, it->wave, world_->OriginRelative(e->position) + glm::vec3(0.0f, 10.0f, 0.0f));
        }
        it = clipSounds_.erase(it);
    }
}

} // namespace sr::client
