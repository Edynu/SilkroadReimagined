#include "sr/server/Skills.h"

#include <algorithm>
#include <cmath>

#include "sr/assets/GameData.h"
#include "sr/core/Log.h"
#include "sr/game/SkillCatalogue.h"

namespace sr::server {

namespace proto = game::proto;

Skills::Skills(SkillHost& host, const assets::SkillTable* table, std::uint32_t seed) : host_(host), table_(table), random_(seed) {
    // The basic attack of each weapon kind: skilldata's base rows by the weapon kinds they take
    // (cols 50, 51; bare hands 1).
    if (table_ == nullptr) return;
    for (const char* code : {"SKILL_PUNCH_01", "SKILL_CH_SWORD_BASE_01", "SKILL_CH_SPEAR_BASE_01", "SKILL_CH_BOW_BASE_01", "SKILL_EU_SWORD_BASE_01",
                             "SKILL_EU_TSWORD_BASE_01", "SKILL_EU_AXE_BASE_01", "SKILL_EU_CROSSBOW_BASE_01", "SKILL_EU_DAGGER_BASE_01",
                             "SKILL_EU_STAFF_BASE_01", "SKILL_EU_WAND_WARLOCK_BASE_01", "SKILL_EU_HARP_BASE_01", "SKILL_EU_WAND_CLERIC_BASE_01"}) {
        const assets::SkillRow* skill = table_->Find(std::string_view(code));
        if (skill == nullptr) continue;
        for (const int kind : skill->weaponKinds) {
            if (kind != 255) basicAttacks_.emplace(kind, skill);
        }
    }
    hammerChina_ = table_->Find(std::string_view("SKILL_CH_FORTRESSWEAPON"));
    hammerEurope_ = table_->Find(std::string_view("SKILL_EU_FORTRESSWEAPON"));
}

const assets::SkillRow* Skills::BasicAttack(const game::CombatStats& stats) const {
    if (stats.weaponKind == 16) {
        if (const assets::SkillRow* hammer = stats.european ? hammerEurope_ : hammerChina_) return hammer;
    }
    if (const auto it = basicAttacks_.find(stats.weaponKind); it != basicAttacks_.end()) return it->second;
    const auto bare = basicAttacks_.find(1);
    return bare != basicAttacks_.end() ? bare->second : nullptr;
}

std::uint32_t Skills::Mana(const game::CombatStats& stats, const assets::SkillRow& skill, const game::SkillModifiers& modifiers) {
    // Its cost and its share of the most MP, at the caster's percent of costs (0x8D: a buff's 'dcmp'), then each of its
    // passives' cuts.
    const std::uint32_t cost = std::uint32_t(std::max(0, skill.mpCost)) + stats.maxMp * std::uint32_t(std::max(0, skill.mpCostRatio)) / 100;
    const std::uint32_t paid =
        stats.mpCostPercent == 100.0 ? cost : std::uint32_t(std::max(0.0, std::trunc(double(cost) * stats.mpCostPercent / 100.0)));
    return game::CutMana(paid, game::BoostsOf(skill, false, modifiers).manaCuts);
}

std::uint32_t Skills::PaidMana(SkillCaster& caster, const assets::SkillRow& skill) {
    const std::uint32_t mana = Mana(caster.stats, skill, caster.state.passives.modifiers);
    if (mana == 0) return 0;
    const game::StatSheet sheet = caster.state.Sheet();
    const int chance = int(sheet.Value(0xC4, 0.0f));
    if (chance <= 0 || std::uniform_int_distribution<int>(0, 99)(random_) >= chance) return mana;
    const int percent = std::clamp(int(sheet.Value(0xC5, 0.0f)), 0, 100);
    return mana - std::uint32_t(std::trunc(double(mana) * double(percent) / 100.0));
}

game::AttackSpec Skills::AttackFor(const game::SkillBook& book, const assets::SkillRow& skill, const game::SkillModifiers& modifiers) {
    game::AttackSpec a = game::AttackOf(skill);
    for (const std::uint32_t m : skill.masteries) {
        if (m != 0) a.mastery = std::max(a.mastery, book.MasteryLevel(m));
    }
    // The passives' values it reads (K6): the variance's roll and point, its own damage, its life steal.
    const game::SkillBoosts b = game::BoostsOf(skill, a.magical, modifiers);
    a.spreadBonus = b.spread;
    a.pointFactor = b.pointFactor;
    if (a.fixedDamage > 0) a.fixedDamage += b.fixedDamage;
    if (a.lifeSteal > 0) a.lifeSteal += b.lifeSteal;
    return a;
}

bool Skills::WeaponFits(int weaponKind, const assets::SkillRow& skill) {
    if (skill.weaponKinds[0] == 255 && skill.weaponKinds[1] == 255) return true;
    return skill.weaponKinds[0] == weaponKind || skill.weaponKinds[1] == weaponKind;
}

bool Skills::Attack(SkillCaster& caster, std::uint32_t target, Clock::time_point now, bool fromList) {
    SkillState& s = caster.state;
    if (caster.dead) return false;
    // The one running casting or mid-swing: it waits behind it, the newest waiting kept.
    if (!fromList && now < s.busyUntil) {
        s.waiting = SkillState::Waiting{true, {}, target};
        TellHeld(caster, now);
        return true;
    }
    if (now < caster.untouchableUntil) {
        s.attackTarget = 0;
        const assets::SkillRow* basic = BasicAttack(caster.stats);
        host_.ToCaster(caster.entity, proto::SkillUseAnswer{basic != nullptr ? basic->id : 0u, proto::SkillUseStatus::Untouchable});
        return false;
    }
    s.attackTarget = 0;
    // Scorned: its attacks go at its scorner alone.
    if (const std::uint32_t forced = ForcedTarget(s); forced != 0 && target != forced) return false;
    // The host's rules first (the jobs' PvP and job monsters): their refusal told.
    if (target != 0) {
        if (const proto::SkillUseStatus why = host_.AttackRefusal(caster.entity, target); why != proto::SkillUseStatus::Ok) {
            const assets::SkillRow* basic = BasicAttack(caster.stats);
            host_.ToCaster(caster.entity, proto::SkillUseAnswer{basic != nullptr ? basic->id : 0u, why});
            return false;
        }
    }
    if (target == 0 || (host_.Monster(target) == nullptr && !host_.Hostile(caster.entity, target))) return false;
    if (host_.IsPlayer(target) && !host_.Hostile(caster.entity, target)) return false; // a player: only a hostile one
    // It replaces the one running (closing in, or between swings): the list emptied, the attack at once.
    s.queued = nullptr;
    s.waiting.reset();
    s.afterSkill = -1;
    s.attackTarget = target;
    s.nextAim = {};
    s.aim = {};
    s.nextSwing = std::min(s.nextSwing, now);
    TellHeld(caster, now);
    return true;
}

bool Skills::HoldPickUp(SkillCaster& caster, std::uint32_t item, Clock::time_point now) {
    SkillState& s = caster.state;
    if (now < s.busyUntil) {
        SkillState::Waiting w;
        w.pickUp = item;
        s.waiting = w;
        TellHeld(caster, now);
        return true;
    }
    s.StopAttacking();
    TellHeld(caster, now);
    return false;
}

void Skills::Stop(SkillCaster& caster, Clock::time_point now) {
    SkillState& s = caster.state;
    const std::uint8_t held = s.Held(now);
    if (held >= 2) {
        s.waiting.reset();
    } else if (held == 1 && !(now < s.castUntil || now < s.chainUntil)) {
        s.StopAttacking();
    }
    TellHeld(caster, now);
}

void Skills::TellHeld(SkillCaster& caster, Clock::time_point now) {
    SkillState& s = caster.state;
    const std::uint8_t held = s.Held(now);
    if (held == s.heldTold) return;
    s.heldTold = held;
    host_.ToCaster(caster.entity, proto::ActionHeld{held});
}

void Skills::CastAround(SkillCaster& caster, const assets::SkillRow& skill, Clock::time_point now) {
    // Where the caster stands: its mana and wait now, the swing, then its area's targets (around it,
    // no selected one) struck at its impact, once its preparing and casting are over.
    const auto me = host_.Position(caster.entity);
    if (!me) return;
    if (skill.activity == 2 && host_.Moving(caster.entity)) host_.MoveTo(caster.entity, *me);
    StartCooldown(caster, skill, now);
    host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill.id, proto::SkillUseStatus::Ok});
    host_.ToObservers(caster.entity, proto::EntitySwing{caster.entity, 0, skill.id});
    const int lead = std::max(0, skill.preparingMs) + std::max(0, skill.castingMs);
    const Clock::time_point impact = lead > 0 ? OnTick(caster.state, now, now + std::chrono::milliseconds(lead)) : now;
    PayAtRelease(caster, skill, 0, impact, now);
    caster.state.castUntil = impact;
    Commanded(caster, skill, now);
    const game::AttackSpec attack = AttackFor(caster.book, skill, caster.state.passives.modifiers);
    const proto::FlatPosition a = proto::ToFlat(*me);
    const std::uint32_t use = ++caster.state.uses;
    for (const Struck& t : TargetsOf(caster, skill, 0, 1)) {
        const auto p = host_.Position(t.target);
        const proto::FlatPosition q = p ? proto::ToFlat(*p) : a;
        caster.state.pendingHits.push_back({impact, t.target, attack,
                                            float(std::hypot(a.x - q.x, a.z - q.z)) + 50.0f, t.percent, &skill});
        caster.state.pendingHits.back().use = use;
    }
}

void Skills::StartCooldown(SkillCaster& caster, const assets::SkillRow& skill, Clock::time_point now) {
    int ms = std::max(0, skill.cooldownMs);
    // Frostbite and slow (the caster's action-time percent p, the original's param 0x8C: 200, 125) stretch an action
    // skill's life (its preparing, casting and duration: activity 2 with a duration) inside its reuse, the original's
    // trunc(p x life / 100 + max(0, reuse - life)). At 100 the plain reuse (the original's max(reuse, life): its hold
    // covers the rest).
    if (const int p = caster.state.states.AttackTimePercent(); p != 100 && skill.activity == 2 && skill.durationMs > 0) {
        const int life = std::max(0, skill.preparingMs) + std::max(0, skill.castingMs) + skill.durationMs;
        ms = int(std::int64_t(p) * life / 100) + std::max(0, skill.cooldownMs - life);
    }
    if (ms <= 0) return;
    caster.state.skillCooling[skill.group] = now + std::chrono::milliseconds(ms);
    if (skill.cooldownGroup != 0) caster.state.groupCooling[std::uint32_t(skill.cooldownGroup)] = now + std::chrono::milliseconds(ms);
    host_.ToCaster(caster.entity, proto::Cooldown{proto::CooldownKind::Skill, skill.id, std::uint32_t(ms)});
    if (ms >= kLongCooldownMs) host_.LongCooldownStarted(caster.entity, skill.id);
}

void Skills::RestoreCooldown(SkillCaster& caster, const assets::SkillRow& skill, std::uint32_t ms, Clock::time_point now) {
    caster.state.skillCooling[skill.group] = now + std::chrono::milliseconds(ms);
    if (skill.cooldownGroup != 0) caster.state.groupCooling[std::uint32_t(skill.cooldownGroup)] = now + std::chrono::milliseconds(ms);
    host_.ToCaster(caster.entity, proto::Cooldown{proto::CooldownKind::Skill, skill.id, ms});
}

bool Skills::Use(SkillCaster& caster, const proto::UseSkill& u, Clock::time_point now, bool fromList) {
    // A skill from the quick slots: known, the weapon it takes, mana, not waiting; an attack skill
    // on a living monster goes off as the next swing (walked into its reach first). Others (buffs,
    // heals) come with their effects.
    const assets::SkillRow* skill = table_ != nullptr ? table_->Find(u.skill) : nullptr;
    // A chain's sub-row (a quick slot kept from before the heads were found) is cast from its head.
    if (skill != nullptr) skill = &table_->Head(*skill);
    // Scorned: its skills aim at its scorner alone.
    if (const std::uint32_t forced = ForcedTarget(caster.state); forced != 0 && u.target != 0 && u.target != caster.entity && u.target != forced) {
        host_.ToCaster(caster.entity, proto::SkillUseAnswer{u.skill, proto::SkillUseStatus::Target});
        return false;
    }
    const auto refuse = [&](proto::SkillUseStatus status) {
        host_.ToCaster(caster.entity, proto::SkillUseAnswer{u.skill, status});
        core::LogDebug("skills: {} is refused {} (status {})", caster.entity, skill != nullptr ? skill->codename : std::to_string(u.skill), int(status));
        return false;
    };
    if (caster.dead) return refuse(proto::SkillUseStatus::Dead);
    // In a monster's shape every skill is refused (0x3030).
    if (caster.state.transform.mode == 1) return refuse(proto::SkillUseStatus::Transformed);
    if (skill == nullptr || !caster.book.Knows(*skill)) return refuse(proto::SkillUseStatus::Unknown);
    if (!WeaponFits(caster.stats.weaponKind, *skill)) return refuse(proto::SkillUseStatus::Weapon);
    // A fortress weapon's skill: a fitting fortress position at every cast (0x3047), not only when it was put on.
    if (caster.stats.weaponKind == 16 && !host_.FortressWeaponAllowed(caster.entity)) return refuse(proto::SkillUseStatus::FortressPosition);
    // Its conditions on what is worn (a shield, a weapon, armour: 'reqi', 0x300D).
    if (!game::EquipmentFits(*skill, caster.state.worn)) return refuse(proto::SkillUseStatus::Weapon);
    // A dance ('reqc' 0x20) only while the caster plays music: one of its songs running (0x3032).
    if ((assets::FindSkillParam(*skill, assets::SkillTag("reqc")).Int(0) & 0x20) != 0 && !PlaysMusic(caster.state)) {
        return refuse(proto::SkillUseStatus::Music);
    }
    SkillState& s = caster.state;
    // A toggle of its group that is on: turned off whatever its wait (ours: how the client asks for it is not traced).
    for (std::size_t i = 0; i < s.buffs.size(); ++i) {
        const assets::SkillRow* running = s.buffs[i].upkeepMs > 0 && table_ != nullptr ? table_->Find(s.buffs[i].skill) : nullptr;
        if (running == nullptr || running->group != skill->group) continue;
        EndBuff(caster, i);
        host_.StatsChanged(caster.entity);
        host_.ToCaster(caster.entity, proto::SkillUseAnswer{u.skill, proto::SkillUseStatus::Ok});
        return false;
    }
    if (const auto it = s.skillCooling.find(skill->group); it != s.skillCooling.end() && now < it->second) return refuse(proto::SkillUseStatus::Cooling);
    if (const auto it = s.groupCooling.find(std::uint32_t(skill->cooldownGroup)); skill->cooldownGroup != 0 && it != s.groupCooling.end() && now < it->second) {
        return refuse(proto::SkillUseStatus::Cooling);
    }
    const game::SkillTraits traits = game::TraitsOf(*skill);
    if (caster.mp < Mana(caster.stats, *skill, caster.state.passives.modifiers)) return refuse(proto::SkillUseStatus::Mana);
    // A stealth or an invisibility: not in battle (the fire trap's type 4 is not refused).
    if (const assets::SkillParam hide = assets::FindSkillParam(*skill, assets::SkillTag("hide")); hide && (hide[0] == 1 || hide[0] == 2) && now < s.battleUntil) {
        return refuse(proto::SkillUseStatus::InBattle);
    }
    // In berserk: no stealth, no transform.
    if (s.berserk && (assets::FindSkillParam(*skill, assets::SkillTag("hide")) || assets::FindSkillParam(*skill, assets::SkillTag("msch")) ||
                      assets::FindSkillParam(*skill, assets::SkillTag("mcap")))) {
        return refuse(proto::SkillUseStatus::Berserk);
    }
    // The action list: an activity-1 skill (an imbue) goes at once and never enters it; another, with the one running
    // casting or mid-swing, waits behind it (the newest waiting kept), and
    // before that replaces it (the fight it was on ends with it).
    if (skill->activity != 1 && !fromList) {
        if (now < s.busyUntil) {
            s.waiting = SkillState::Waiting{false, u, 0};
            TellHeld(caster, now);
            return true;
        }
    }
    // Starting a skill ends what ends on one (skc word 1 bit 2: the stealth, the invisibility);
    // a buff on itself its overlap refuses starts nothing (its admission comes first, a lower
    // rank of the stealth running is refused, the stealth kept).
    bool refusedBuff = false;
    if ((!skill->targetRequired && (skill->hasDuration || traits.toggle)) ||
        (traits.buff && traits.target != game::SkillTraits::Target::Enemy && (u.target == 0 || u.target == caster.entity))) {
        std::vector<const assets::SkillRow*> running;
        for (const SkillState::Buff& b : caster.state.buffs) running.push_back(table_ != nullptr ? table_->Find(b.skill) : nullptr);
        refusedBuff = !game::AdmitBuff(*skill, running).admitted;
    }
    if (!refusedBuff && EndOnEvent(caster, 2)) host_.StatsChanged(caster.entity);
    // Its path, the first of these that fits (game::SkillRouteOf: the skill lab's list shows the same).
    const game::SkillRoute route = game::SkillRouteOf(*skill);
    // A passive is never cast (its numbers are in the sheet while learned).
    if (route == game::SkillRoute::Passive) return refuse(proto::SkillUseStatus::NotYet);
    // A detection on those around ('dttp': a copy on each player its area takes, revealing a hidden one
    // to all while it lasts).
    if (route == game::SkillRoute::Detection) {
        const auto me = host_.Position(caster.entity);
        if (!me) return refuse(proto::SkillUseStatus::Target);
        const assets::SkillParam area = assets::FindSkillParam(*skill, assets::SkillTag("efr"), 1);
        const float radius = area ? float(area[2]) : 100.0f;
        caster.mp -= PaidMana(caster, *skill);
        host_.VitalsChanged(caster.entity);
        StartCooldown(caster, *skill, now);
        host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill->id, proto::SkillUseStatus::Ok});
        host_.ToObservers(caster.entity, proto::EntitySwing{caster.entity, 0, skill->id});
        Stand(caster, *skill, now);
        const std::uint32_t ms = skill->hasDuration && skill->buffDurationMs > 0 ? std::uint32_t(skill->buffDurationMs) : 10000u;
        for (const std::uint32_t id : host_.PlayersNear(*me, radius)) {
            if (id == caster.entity) continue;
            if (std::optional<SkillCaster> other = host_.Player(id); other && !other->dead) {
                if (const SkillState::Buff* added = AddBuff(*other, *skill, ms, now)) {
                    for (SkillState::Buff& b : other->state.buffs) b.own = b.own && b.token != added->token;
                    host_.StatsChanged(id);
                }
            }
        }
        return false;
    }
    // A teleport ('tele': the Chinese Ghost Walk, a dash; 'tel2': the Wizard's Teleport, a put; its time, its
    // most distance): to the point asked, as far as the way on the navigation goes, at once (no preparing or
    // casting: the position effect at the cast's begin); paid, its wait started. A dash is
    // refused while rooted (ours: Target). Its move told before its swing (EntityBlink).
    if (route == game::SkillRoute::Teleport) {
        assets::SkillParam tele = assets::FindSkillParam(*skill, assets::SkillTag("tele"));
        const bool dash = bool(tele);
        if (!tele) tele = assets::FindSkillParam(*skill, assets::SkillTag("tel2"));
        if (!u.hasPoint) return refuse(proto::SkillUseStatus::Target);
        if (now < caster.untouchableUntil) return refuse(proto::SkillUseStatus::Untouchable);
        if (dash && caster.state.states.Rooted()) return refuse(proto::SkillUseStatus::Target);
        // Not while the caster holds its own wall (the skill manager's wall; inferred: an Ice or Fire Wall,
        // a buff whose row carries 'pw'): every teleport refused (ours: Target, the original's 0x3009).
        for (const SkillState::Buff& b : caster.state.buffs) {
            const assets::SkillRow* row = table_->Find(b.skill);
            if (row != nullptr && assets::FindSkillParam(*row, assets::SkillTag("pw"))) return refuse(proto::SkillUseStatus::Target);
        }
        const auto from = host_.Position(caster.entity);
        if (!from) return refuse(proto::SkillUseStatus::Target);
        s.StopFighting();
        const std::optional<proto::WorldPosition> to = host_.Blink(caster.entity, u.point, float(tele.Int(1)));
        if (!to) return refuse(proto::SkillUseStatus::Target);
        caster.mp -= PaidMana(caster, *skill);
        host_.VitalsChanged(caster.entity);
        StartCooldown(caster, *skill, now);
        host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill->id, proto::SkillUseStatus::Ok});
        host_.ToObservers(caster.entity, proto::EntityBlink{caster.entity, skill->id, dash ? proto::BlinkKind::Dash : proto::BlinkKind::Put, *to});
        host_.ToObservers(caster.entity, proto::EntitySwing{caster.entity, 0, skill->id});
        core::LogInfo("skills: {} blinks {:.0f} with {}", caster.entity,
                      std::hypot(proto::ToFlat(*to).x - proto::ToFlat(*from).x, proto::ToFlat(*to).z - proto::ToFlat(*from).z), skill->codename);
        return false;
    }
    // The mask ('mcap' level cap, item): on a monster's body of the
    // normal kind and no higher level, not a job's or a quest's monster (0x3033 the kind and the special ones (the
    // original's 0x3006 for TID4 2, 3 and 4 ours words as the kind), 0x3035 the level), the item dropped at the caster's feet, its own,
    // carrying the monster (ours: at the cast).
    if (route == game::SkillRoute::Mask) {
        const assets::SkillParam cap = assets::FindSkillParam(*skill, assets::SkillTag("mcap"));
        const std::optional<Monsters::Corpse> corpse = u.target != 0 ? host_.MonsterCorpse(u.target) : std::nullopt;
        const bool special = corpse && corpse->type >= 2 && corpse->type <= 4;
        if (!corpse) return refuse(proto::SkillUseStatus::Target);
        if (corpse->rarity != 0 || special) return refuse(proto::SkillUseStatus::MonsterClass);
        if (corpse->level > cap.Int(0)) return refuse(proto::SkillUseStatus::TargetLevel);
        caster.mp -= PaidMana(caster, *skill);
        host_.VitalsChanged(caster.entity);
        StartCooldown(caster, *skill, now);
        host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill->id, proto::SkillUseStatus::Ok});
        host_.ToObservers(caster.entity, proto::EntitySwing{caster.entity, u.target, skill->id});
        Stand(caster, *skill, now);
        host_.DropItem(caster.entity, cap[1], corpse->model);
        return false;
    }
    // The duplicate ('msch 2' level): on another player of no higher level, the caster takes
    // its look for the buff's time; the buff on the caster.
    if (route == game::SkillRoute::Duplicate) {
        const assets::SkillParam change = assets::FindSkillParam(*skill, assets::SkillTag("msch"));
        std::optional<SkillCaster> other = u.target != 0 && u.target != caster.entity ? host_.Player(u.target) : std::nullopt;
        if (!other || other->dead) return refuse(proto::SkillUseStatus::Target);
        // Not on one in a job's suit or in free battle, nor (after the level) on a murderer: 0x3039.
        if (host_.DuplicateBarred(u.target, false)) return refuse(proto::SkillUseStatus::Duplicate);
        if (other->stats.level > change.Int(1)) return refuse(proto::SkillUseStatus::Target);
        if (host_.DuplicateBarred(u.target, true)) return refuse(proto::SkillUseStatus::Duplicate);
        const std::size_t before = caster.state.buffs.size();
        CastBuff(caster, caster, *skill, now);
        if (caster.state.buffs.size() > before && caster.state.buffs.back().skill == skill->id) {
            caster.state.transform = {2, 0, u.target, 0, caster.state.buffs.back().token};
            host_.LookChanged(caster.entity);
        }
        return false;
    }
    // A skill object (an area of kind 3: Harmony, the poison field, the fire trap): paid now, standing
    // at the caster's feet from its impact.
    if (route == game::SkillRoute::Object) {
        if (traits.attack && now < caster.untouchableUntil) return refuse(proto::SkillUseStatus::Untouchable);
        CastObject(caster, *skill, now);
        return false;
    }
    // A skill on oneself (no target asked, a time it lasts, a toggle): a buff at once, an imbue among them.
    if (route == game::SkillRoute::SelfBuff) {
        CastBuff(caster, caster, *skill, now);
        return false;
    }
    // A buff on a player (K4): oneself (no target, or oneself, when it takes oneself), or another
    // player in its reach. One whose targets are players and never monsters (the Rogue's Tag Point:
    // a party member, an ally or an enemy player) is one too: no player is an enemy yet (no PvP), so
    // the others are its allies. One on enemy players alone (the Rogue's Scorn, 'hitm'; 33 2.6) has no
    // target until then, and never a monster ("You cannot use the skill against monsters").
    // On an enemy player alone (the Rogue's Scorn and Gross Scorn, 'hitm'): walked into
    // reach, its copy and link on that player at the release; while it lasts the player aims only at the caster
    // (ForcedTarget), the fight it was in turned onto the caster.
    if (route == game::SkillRoute::EnemyPlayerBuff) {
        std::optional<SkillCaster> other = u.target != 0 && u.target != caster.entity ? host_.Player(u.target) : std::nullopt;
        // Its enemy alone (a free PvP foe, a murderer, a job's or an event's: HostileInArea, no new victim of a murder).
        if (!other || other->dead || !host_.HostileInArea(caster.entity, u.target)) return refuse(proto::SkillUseStatus::Target);
        if (const proto::SkillUseStatus why = host_.AttackRefusal(caster.entity, u.target); why != proto::SkillUseStatus::Ok) return refuse(why);
        if (!ReachFriend(caster, u, *skill, now)) return true; // walked toward it first
        if (const proto::SkillUseStatus link = AdmitLink(caster.state, *skill, u.target); link != proto::SkillUseStatus::Ok) return refuse(link);
        CastBuff(caster, *other, *skill, now);
        return false;
    }
    if (route == game::SkillRoute::PlayerBuff) {
        if (u.target == 0 || u.target == caster.entity) {
            if (!skill->targetSelf && skill->targetRequired) return refuse(proto::SkillUseStatus::Target);
            CastBuff(caster, caster, *skill, now);
            return false;
        }
        std::optional<SkillCaster> other = host_.Player(u.target);
        const auto me = host_.Position(caster.entity), there = host_.Position(u.target);
        if (!other || other->dead || !me || !there) return refuse(proto::SkillUseStatus::Target);
        if (!ReachFriend(caster, u, *skill, now)) return true; // walked toward it first (D9)
        if (const proto::SkillUseStatus link = AdmitLink(caster.state, *skill, u.target); link != proto::SkillUseStatus::Ok) return refuse(link);
        CastBuff(caster, *other, *skill, now);
        return false;
    }
    // An attack (an 'att', its own damage 'pdmg', a life steal 'lfst'), states without one (a curse, a
    // root, a sleep: K3), or a buff's changes on an enemy (the Chinese water's cancels: K4).
    // A resurrection (K9), then a heal or a cure at once (K7; a heal over time is a buff, above).
    if (route == game::SkillRoute::Resurrect) return Revive(caster, u, *skill, now);
    if (route == game::SkillRoute::Support) return Support(caster, u, *skill, now);
    if (route == game::SkillRoute::Taunt) return Taunt(caster, u, *skill, now);
    if (route == game::SkillRoute::Unhandled) return refuse(proto::SkillUseStatus::NotYet);
    if (now < caster.untouchableUntil) return refuse(proto::SkillUseStatus::Untouchable);
    // States around the caster with no target asked (the cold's freezing ground, the rogue's poison
    // round, the wizard's untouchable): at once.
    if (route == game::SkillRoute::Around) {
        CastAround(caster, *skill, now);
        return false;
    }
    // Mana Switch: a link taking allies and the party as well as monsters and enemy
    // players, meant for a party member. On a player not hostile to the caster it is that player's buff, as a
    // player-buff's (its copy and link on the player, 'lnks 15 700'; the caster's MP from the player's damage, 'lkdh');
    // on an enemy, the attack's path below.
    if (u.target != 0 && u.target != caster.entity && traits.buff && (skill->targetAlly || skill->targetParty) &&
        assets::FindSkillParam(*skill, assets::SkillTag("lnks")) && host_.Player(u.target).has_value() && !host_.HostileInArea(caster.entity, u.target)) {
        // (A player, whatever Monster says: the game server's Monster gives a player any PvP rule lets one strike, an
        // innocent too, a murder's; and its enemy is HostileInArea's, Hostile allowing a chosen murder too.)
        std::optional<SkillCaster> other = host_.Player(u.target);
        if (!other || other->dead) return refuse(proto::SkillUseStatus::Target);
        if (!ReachFriend(caster, u, *skill, now)) return true; // walked toward it first (D9)
        if (const proto::SkillUseStatus link = AdmitLink(caster.state, *skill, u.target); link != proto::SkillUseStatus::Ok) return refuse(link);
        CastBuff(caster, *other, *skill, now);
        return false;
    }
    if (u.target != 0) {
        if (const proto::SkillUseStatus why = host_.AttackRefusal(caster.entity, u.target); why != proto::SkillUseStatus::Ok) return refuse(why);
    }
    if (u.target == 0 || (host_.Monster(u.target) == nullptr && !host_.Hostile(caster.entity, u.target))) return refuse(proto::SkillUseStatus::Target);
    if (host_.IsPlayer(u.target) && !host_.Hostile(caster.entity, u.target)) return refuse(proto::SkillUseStatus::Target);
    // A skill for enemy players alone (TargetGroup_Enemy_P without Enemy_M: the KB socket stone's knock-back) never on a
    // monster.
    if (skill->targetEnemyPlayer && !skill->targetEnemyMonster && !host_.IsPlayer(u.target)) return refuse(proto::SkillUseStatus::Target);
    // A structure takes the weapon's basic attack and what is made for buildings alone (0x3006, its text not traced).
    if (host_.IsStructure(u.target) && !skill->targetBuilding && skill != BasicAttack(caster.stats)) return refuse(proto::SkillUseStatus::Target);
    if (const proto::SkillUseStatus link = AdmitLink(caster.state, *skill, u.target); link != proto::SkillUseStatus::Ok) return refuse(link);
    if (s.attackTarget != u.target) {
        s.attackTarget = u.target;
        s.nextAim = {};
        s.aim = {};
    }
    s.queued = skill;
    s.waiting.reset();
    s.afterSkill = -1;
    s.nextSwing = std::min(s.nextSwing, now); // it replaces the one running at once (between swings: no wait)
    host_.ToCaster(caster.entity, proto::SkillUseAnswer{u.skill, proto::SkillUseStatus::Ok});
    TellHeld(caster, now);
    return true;
}

bool Skills::OutlivesLogout(const assets::SkillRow& skill) { return skill.cancelConfirm; }

game::BuffBasis Skills::BasisOf(const game::CombatStats& stats) {
    return {float(stats.strength), float(stats.intelligence), float(stats.phyDefence), float(stats.magDefence), float(stats.shieldDefence)};
}

SkillState::Buff Skills::MakeBuff(const SkillCaster& holder, const assets::SkillRow& skill, std::uint32_t ms, Clock::time_point now,
                                  const game::SkillBoosts* boosts) {
    // What it changes on the holder's numbers as they are now; a toggle's MP
    // every period ('onff': its period, its MP; the first after one period); one that pulses an area of
    // its own (its 'efr' of kind 2) every 'puls'.
    SkillState::Buff b;
    b.token = nextBuffToken_++;
    b.skill = skill.id;
    b.changes = game::BuffChanges(skill, BasisOf(holder.stats), boosts != nullptr ? boosts->bonus : game::BuffBonus{});
    b.until = ms == 0 ? Clock::time_point::max() : now + std::chrono::milliseconds(ms);
    if (const assets::SkillParam onff = assets::FindSkillParam(skill, assets::SkillTag("onff")); onff && onff.Int(0) > 0) {
        b.upkeepMs = onff.Int(0);
        b.upkeepMp = boosts != nullptr && boosts->upkeepCut != 0 ? game::CutMana(onff[1], {boosts->upkeepCut}) : onff[1];
        b.nextUpkeep = now + std::chrono::milliseconds(b.upkeepMs);
    }
    if (const assets::SkillParam pulse = assets::FindSkillParam(skill, assets::SkillTag("puls"));
        pulse && pulse.Int(0) > 0 && assets::FindSkillParam(skill, assets::SkillTag("efr"), 2)) {
        b.pulseMs = pulse.Int(0);
        b.nextPulse = now + std::chrono::milliseconds(b.pulseMs);
    }
    // A companion ('summ': its time, ?, its period, its power, its magical power).
    if (const assets::SkillParam summ = assets::FindSkillParam(skill, assets::SkillTag("summ")); summ && summ.Int(2) > 0) {
        b.companionMs = summ.Int(2);
        b.companionPower = summ[3];
        b.companionMagic = summ[4];
    }
    return b;
}

void Skills::EndBuff(SkillCaster& holder, std::size_t index) {
    std::vector<SkillState::Buff>& buffs = holder.state.buffs;
    if (index >= buffs.size()) return;
    const std::uint32_t token = buffs[index].token;
    const assets::SkillRow* ended = table_ != nullptr ? table_->Find(buffs[index].skill) : nullptr;
    host_.ToCaster(holder.entity, proto::BuffRemoved{token});
    buffs.erase(buffs.begin() + std::ptrdiff_t(index));
    if (ended != nullptr && OutlivesLogout(*ended)) host_.KeptBuffsChanged(holder.entity);
    // Its transform's: the look and speeds back (0x3208).
    if (SkillState::Transform& t = holder.state.transform; t.token != 0 && t.token == token) {
        const bool looked = t.mode == 1 || t.mode == 2;
        t = {};
        if (looked) host_.LookChanged(holder.entity);
    }
}

bool Skills::TransformInto(SkillCaster& holder, const assets::SkillRow& skill, std::uint32_t model, int level, std::uint32_t basic, Clock::time_point now) {
    const auto refuse = [&](proto::SkillUseStatus status) {
        host_.ToCaster(holder.entity, proto::SkillUseAnswer{skill.id, status});
        return false;
    };
    if (holder.dead) return refuse(proto::SkillUseStatus::Dead);
    if (holder.state.berserk) return refuse(proto::SkillUseStatus::Berserk); // 0x3031
    if (level > holder.stats.level) return refuse(proto::SkillUseStatus::Level); // 0x3008
    const std::uint32_t ms = skill.hasDuration && skill.buffDurationMs > 0 ? std::uint32_t(skill.buffDurationMs) : 900000u;
    const SkillState::Buff* buff = AddBuff(holder, skill, ms, now);
    if (buff == nullptr) return refuse(proto::SkillUseStatus::Overlap);
    // A transform held already gives its slot up (ours: the original's rule for a second not traced).
    const std::uint32_t token = buff->token;
    if (holder.state.transform.token != 0 && holder.state.transform.token != token) {
        for (std::size_t i = 0; i < holder.state.buffs.size(); ++i) {
            if (holder.state.buffs[i].token != holder.state.transform.token) continue;
            holder.state.transform = {};
            EndBuff(holder, i);
            break;
        }
    }
    holder.state.StopAttacking();
    holder.state.transform = {1, model, 0, basic, token};
    host_.LookChanged(holder.entity);
    host_.StatsChanged(holder.entity);
    core::LogInfo("skills: {} takes the shape of monster {} ({} ms)", holder.entity, model, ms);
    return true;
}

const assets::SkillRow* Skills::DefaultAttack(const SkillCaster& caster) const {
    if (caster.state.transform.mode == 1 && caster.state.transform.basic != 0 && table_ != nullptr) {
        if (const assets::SkillRow* basic = table_->Find(caster.state.transform.basic)) return basic;
    }
    return BasicAttack(caster.stats);
}

void Skills::CastBuff(SkillCaster& caster, SkillCaster& holder, const assets::SkillRow& skill, Clock::time_point now) {
    // A buff from a skill (its 'dura', a toggle's 'onff', an imbue) on its holder: first whether it may
    // stand beside the buffs the holder has (a higher rank of its group, or a
    // buff holding one of its overlap groups, refuses it; a lower or equal rank of its group gives way);
    // then its mana and wait, the swing to show it, the buff on the holder's board, its numbers again.
    std::vector<const assets::SkillRow*> running;
    for (const SkillState::Buff& b : holder.state.buffs) running.push_back(table_ != nullptr ? table_->Find(b.skill) : nullptr);
    const game::BuffAdmission admission = game::AdmitBuff(skill, running);
    if (!admission.admitted) {
        host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill.id, proto::SkillUseStatus::Overlap});
        return;
    }
    StartCooldown(caster, skill, now);
    host_.ToObservers(caster.entity, proto::EntitySwing{caster.entity, holder.entity, skill.id});
    Stand(caster, skill, now);
    host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill.id, proto::SkillUseStatus::Ok});
    // With a preparing or a casting: put on, and paid for, at its release (D7), the slot re-checked till then.
    if (const int lead = std::max(0, skill.preparingMs) + std::max(0, skill.castingMs); lead > 0) {
        const Clock::time_point release = OnTick(caster.state, now, now + std::chrono::milliseconds(lead));
        caster.state.pendingBuffs.push_back({release, holder.entity, &skill});
        Occupy(caster, skill, holder.entity, release, std::min(caster.mp, Mana(caster.stats, skill, caster.state.passives.modifiers)), now);
        return;
    }
    InstallBuff(caster, holder, skill, now);
}

bool Skills::InstallBuff(SkillCaster& caster, SkillCaster& holder, const assets::SkillRow& skill, Clock::time_point now, bool main) {
    std::vector<const assets::SkillRow*> running;
    for (const SkillState::Buff& b : holder.state.buffs) running.push_back(table_ != nullptr ? table_->Find(b.skill) : nullptr);
    const game::BuffAdmission admission = game::AdmitBuff(skill, running);
    if (!admission.admitted) return false;
    if (main) {
        caster.mp -= std::min(caster.mp, PaidMana(caster, skill));
        host_.VitalsChanged(caster.entity);
    }
    std::vector<std::size_t> replaced = admission.replaces;
    std::sort(replaced.rbegin(), replaced.rend());
    for (const std::size_t i : replaced) EndBuff(holder, i);
    const bool toggle = static_cast<bool>(assets::FindSkillParam(skill, assets::SkillTag("onff")));
    // Its time and its bonuses from the caster's passives (K6: the poison blade's, the Warlock's, the
    // blesses').
    const game::SkillBoosts boosts = game::BoostsOf(skill, false, caster.state.passives.modifiers);
    const std::uint32_t ms = skill.hasDuration && skill.buffDurationMs > 0 ? game::ExtendedMs(skill.buffDurationMs, boosts) : toggle ? 0u : 5000u;
    holder.state.buffs.push_back(MakeBuff(holder, skill, ms, now, &boosts));
    holder.state.buffs.back().own = holder.entity == caster.entity;
    if (OutlivesLogout(skill)) host_.KeptBuffsChanged(holder.entity);
    // A transform's buff takes the holder's transform slot (modes 3 and 4: the slot alone, 33 5).
    if (const assets::SkillParam change = assets::FindSkillParam(skill, assets::SkillTag("msch")); change && (change[0] == 3 || change[0] == 4)) {
        holder.state.transform = {int(change[0]), 0, 0, 0, holder.state.buffs.back().token};
    }
    // A link on another (33 4.1): the buff is its copy, its owner's record beside it (from the cast;
    // ours: the original's from the impact).
    if (holder.entity != caster.entity && assets::FindSkillParam(skill, assets::SkillTag("lnks"))) {
        SkillState::Buff& copy = holder.state.buffs.back();
        copy.linkOwner = caster.entity;
        StartLink(caster, skill, holder.entity, copy.token, now, copy.until);
    }
    // A heal over time on its holder: what each pulse gives, from the healer as it casts (K7).
    if (const game::HealSpec heal = game::HealOf(skill, boosts.heal); heal && holder.state.buffs.back().pulseMs == 0) {
        if (const assets::SkillParam pulse = assets::FindSkillParam(skill, assets::SkillTag("puls")); pulse && pulse.Int(0) > 0) {
            SkillState::Buff& b = holder.state.buffs.back();
            b.pulseMs = pulse.Int(0);
            b.nextPulse = now + std::chrono::milliseconds(b.pulseMs);
            b.pulseHeal = game::HealFor(heal, 100, caster.stats, holder.stats);
        }
    }
    // Scorn ('hitm'): the fight its holder was in turned onto the caster.
    if (holder.entity != caster.entity && assets::FindSkillParam(skill, assets::SkillTag("hitm")) && holder.state.attackTarget != 0) {
        holder.state.attackTarget = caster.entity;
        holder.state.aim = {};
        holder.state.nextAim = {};
    }
    proto::BuffAdded added = BuffMessage(holder.state.buffs.back(), now);
    if (holder.entity != caster.entity && assets::FindSkillParam(skill, assets::SkillTag("hitm"))) added.source = caster.entity; // who scorned it
    host_.ToCaster(holder.entity, added);
    host_.StatsChanged(holder.entity);
    core::LogInfo("skills: {} casts {} on {} ({} ms)", caster.entity, skill.codename,
                  holder.entity == caster.entity ? std::string("itself") : std::to_string(holder.entity), ms);
    // Gross Scorn ('hitm' with an area: efr shape 2, its range about the holder, the most at once counting the holder):
    // the caster's other enemy players there take its copy and link too (the original's efr on Scorn B), paid once.
    if (main && holder.entity != caster.entity && assets::FindSkillParam(skill, assets::SkillTag("hitm")) &&
        assets::FindSkillParam(skill, assets::SkillTag("efr"), 1)) {
        for (const Struck& t : TargetsOf(caster, skill, holder.entity, 1)) {
            if (t.target == holder.entity || t.target == caster.entity) continue;
            std::optional<SkillCaster> other = host_.Player(t.target);
            if (!other || other->dead || host_.AttackRefusal(caster.entity, t.target) != proto::SkillUseStatus::Ok) continue;
            InstallBuff(caster, *other, skill, now, false);
        }
    }
    return true;
}

void Skills::Occupy(SkillCaster& caster, const assets::SkillRow& skill, std::uint32_t target, Clock::time_point release, std::uint32_t mp,
                    Clock::time_point now) {
    caster.state.casting = SkillState::Casting{&skill, target == caster.entity ? 0u : target, release, OnTick(caster.state, now, now + std::chrono::milliseconds(1)),
                                               mp};
}

void Skills::PayAtRelease(SkillCaster& caster, const assets::SkillRow& skill, std::uint32_t target, Clock::time_point release, Clock::time_point now) {
    // A cast's mana at its release, the slot re-checked till then (pays at the release; D8); an
    // instant one's now.
    const std::uint32_t paid = PaidMana(caster, skill);
    if (release <= now) {
        caster.mp -= std::min(caster.mp, paid);
        host_.VitalsChanged(caster.entity);
        return;
    }
    if (paid > 0) caster.state.pendingCosts.push_back({release, paid});
    Occupy(caster, skill, target, release, std::min(caster.mp, Mana(caster.stats, skill, caster.state.passives.modifiers)), now);
}

std::uint32_t Skills::ForcedTarget(const SkillState& state) const {
    // A Scorn's copy on it ('hitm' with its link's owner): that owner.
    if (table_ == nullptr) return 0;
    for (const SkillState::Buff& b : state.buffs) {
        if (b.linkOwner == 0) continue;
        if (const assets::SkillRow* row = table_->Find(b.skill); row != nullptr && assets::FindSkillParam(*row, assets::SkillTag("hitm"))) return b.linkOwner;
    }
    return 0;
}

bool Skills::ReachFriend(SkillCaster& caster, const proto::UseSkill& u, const assets::SkillRow& skill, Clock::time_point now) {
    // In reach: its range (Action_Range, at least 10), both bodies, whole, and two more; the walk's end the
    // range itself when short, two under it below 10, else 70 % of it, as an attack's.
    SkillState& s = caster.state;
    const auto me = host_.Position(caster.entity), there = host_.Position(u.target);
    if (!me || !there) return true; // nothing to measure: the cast's own checks decide
    const proto::FlatPosition a = proto::ToFlat(*me), b = proto::ToFlat(*there);
    const float range = float(std::max(skill.range, 10)), base = 2.0f * kPlayerRadius;
    const double distance = std::hypot(a.x - b.x, a.z - b.z);
    if (std::trunc(distance) <= std::trunc(base + range) + 2.0f) {
        s.approach.reset();
        if (host_.Moving(caster.entity)) host_.MoveTo(caster.entity, *me); // stop where it stands
        return true;
    }
    // Out of reach: the command replaces the fight, the caster walks toward it (its next steps: Approach).
    s.StopAttacking();
    s.approach = SkillState::Approach{u};
    const float stop = range <= 4.0f ? range : range < 10.0f ? range - 2.0f : range * 0.7f;
    const double flat = std::max(1.0, distance);
    host_.MoveTo(caster.entity, proto::FromFlat({b.x + (a.x - b.x) / flat * (base + stop), b.z + (a.z - b.z) / flat * (base + stop)}, there->y));
    s.aim = *there;
    s.nextAim = now + std::chrono::milliseconds(300);
    core::LogInfo("skills: {} walks toward {} for {} ({:.0f} away)", caster.entity, u.target, skill.codename, distance);
    return false;
}

void Skills::Approach(SkillCaster& caster, Clock::time_point now) {
    // The walk toward another player for a skill on it: cast once in reach (its checks again then), given up when the
    // caster dies or its target is gone; the target walking away is followed (every 300 ms when it went 5 farther).
    SkillState& s = caster.state;
    const proto::UseSkill u = s.approach->use;
    const bool standing = host_.Player(u.target).has_value() || host_.Monster(u.target) != nullptr; // a taunt's monster too
    const assets::SkillRow* skill = table_ != nullptr ? table_->Find(u.skill) : nullptr;
    const auto me = host_.Position(caster.entity), there = host_.Position(u.target);
    if (caster.dead || !standing || skill == nullptr || !me || !there || caster.state.states.Locked()) {
        s.approach.reset();
        return;
    }
    const proto::FlatPosition a = proto::ToFlat(*me), b = proto::ToFlat(*there);
    const float range = float(std::max(skill->range, 10)), base = 2.0f * kPlayerRadius;
    const double distance = std::hypot(a.x - b.x, a.z - b.z);
    if (std::trunc(distance) <= std::trunc(base + range) + 2.0f) {
        s.approach.reset();
        Use(caster, u, now, true);
        return;
    }
    const proto::FlatPosition aimed = proto::ToFlat(s.aim);
    if (!host_.Moving(caster.entity) || (std::hypot(aimed.x - b.x, aimed.z - b.z) > 5.0 && now >= s.nextAim)) {
        const float stop = range <= 4.0f ? range : range < 10.0f ? range - 2.0f : range * 0.7f;
        const double flat = std::max(1.0, distance);
        host_.MoveTo(caster.entity, proto::FromFlat({b.x + (a.x - b.x) / flat * (base + stop), b.z + (a.z - b.z) / flat * (base + stop)}, there->y));
        s.aim = *there;
        s.nextAim = now + std::chrono::milliseconds(300);
    }
}

bool Skills::Taunt(SkillCaster& caster, const proto::UseSkill& u, const assets::SkillRow& skill, Clock::time_point now) {
    // The taunts and the aggro cuts: cast as any
    // action (the walk into reach for a targeted one, the swing), no damage; at the impact a zero-damage hit record on
    // each monster its area takes ('tnt2' or 'dtnt' as its payload), the threat at its
    // application. Its area: around the caster (shape 1) or its target (shape 2), monsters only (mask 0x10).
    const auto refuse = [&](proto::SkillUseStatus status) {
        host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill.id, status});
        return false;
    };
    std::uint32_t centre = 0, source = caster.entity;
    if (skill.targetRequired) {
        const std::uint32_t t = u.target == 0 ? caster.entity : u.target;
        if (t != caster.entity && !host_.Player(t).has_value() && host_.Monster(t) != nullptr) { // a monster (Monster gives fightable players too)
            if (!skill.targetEnemyMonster) return refuse(proto::SkillUseStatus::Target);
            centre = t;
        } else if (t == caster.entity) {
            if (!skill.targetSelf) return refuse(proto::SkillUseStatus::Target);
        } else if (const std::optional<SkillCaster> other = host_.Player(t); other && !other->dead && (skill.targetAlly || skill.targetParty)) {
            // The Bard's Discord Wave on an ally: the monsters about it lower their threat toward it (its source).
            centre = t;
            source = t;
        } else {
            return refuse(proto::SkillUseStatus::Target);
        }
        if (centre != 0 && !ReachFriend(caster, u, skill, now)) return true; // walked toward it first
    }
    StartCooldown(caster, skill, now);
    host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill.id, proto::SkillUseStatus::Ok});
    host_.ToObservers(caster.entity, proto::EntitySwing{caster.entity, centre != 0 ? centre : caster.entity, skill.id});
    const Clock::time_point at = Stand(caster, skill, now);
    PayAtRelease(caster, skill, centre, at, now);
    caster.state.pendingTaunts.push_back({at, &skill, centre, source});
    core::LogInfo("skills: {} casts {} ({})", caster.entity, skill.codename, centre != 0 ? "on its target's area" : "around itself");
    return false;
}

void Skills::LandTaunts(SkillCaster& caster, Clock::time_point now) {
    std::vector<SkillState::PendingTaunt>& pending = caster.state.pendingTaunts;
    for (auto it = pending.begin(); it != pending.end();) {
        if (now < it->at) {
            ++it;
            continue;
        }
        const SkillState::PendingTaunt t = *it;
        it = pending.erase(it);
        if (caster.dead || t.skill == nullptr) continue;
        const auto centre = host_.Position(t.centre != 0 ? t.centre : caster.entity);
        if (!centre) continue;
        // The area: within the caster's body, its range and the monster's own body of its centre, the selected monster
        // first and counted in its most at once, the others by their ids (as an attack's).
        const assets::SkillParam area = assets::FindSkillParam(*t.skill, assets::SkillTag("efr"), 1);
        const double range = area ? double(area[2]) : 0.0;
        const std::size_t cap = area ? std::size_t(area[3]) : 1;
        std::vector<std::uint32_t> struck;
        if (t.centre != 0 && host_.Monster(t.centre) != nullptr) struck.push_back(t.centre);
        if (area) {
            const proto::FlatPosition c = proto::ToFlat(*centre);
            std::vector<std::uint32_t> near = host_.MonstersNear(*centre, float(range) + 100.0f);
            std::sort(near.begin(), near.end());
            for (const std::uint32_t id : near) {
                if (cap != 0 && struck.size() >= cap) break;
                if (id == t.centre) continue;
                const game::CombatStats* m = host_.Monster(id);
                const auto where = host_.Position(id);
                if (m == nullptr || !where) continue;
                const proto::FlatPosition p = proto::ToFlat(*where);
                if (std::hypot(p.x - c.x, p.z - c.z) <= double(kPlayerRadius) + range + double(m->bodyRadius)) struck.push_back(id);
            }
        }
        // Its threat: a raise ('tnt2' and the weapon's 'pwtt') on no damage, or a cut ('dtnt' and the weapon's 'mwdt':
        // the source's record less them, and less its percent), told as a skill that took hold (B0BD).
        const game::ThreatSpec spec = game::ThreatOf(*t.skill);
        std::int64_t threat = game::RaiseThreat(0, 0, spec, caster.stats);
        if (spec.cut || spec.physicalCutWord != 0 || spec.magicalCutWord != 0) {
            threat -= spec.cutFlat + game::WeaponThreat(caster.stats, spec.physicalCutWord, false) + game::WeaponThreat(caster.stats, spec.magicalCutWord, true);
        }
        for (const std::uint32_t id : struck) {
            Monsters::Affliction affliction;
            affliction.threatGiven = true;
            affliction.threat = threat;
            affliction.threatPercent = -spec.cutPercent;
            host_.HitMonster(id, t.source, 0, now, &affliction);
            host_.ToObservers(id, proto::SkillTook{id, caster.entity, t.skill->id});
        }
        core::LogInfo("skills: {}'s {} on {} monster(s), threat {}", caster.entity, t.skill->codename, struck.size(), threat);
    }
}

void Skills::CheckCasting(SkillCaster& caster, Clock::time_point now) {
    // On each skill tick till its release: what is worn, its target, its mana (mask 0x1C); a failure gives the cast up
    // (B071 2: nothing paid, nothing struck).
    SkillState& s = caster.state;
    if (!s.casting) return;
    if (now >= s.casting->release) {
        s.casting.reset();
        return;
    }
    if (now < s.casting->nextCheck) return;
    s.casting->nextCheck += std::chrono::milliseconds(300);
    const SkillState::Casting& c = *s.casting;
    bool fits = !caster.dead && c.skill != nullptr && WeaponFits(caster.stats.weaponKind, *c.skill) && game::EquipmentFits(*c.skill, s.worn) &&
                caster.mp >= c.mp;
    if (fits && c.target != 0) {
        const std::optional<SkillCaster> player = host_.Player(c.target);
        fits = player ? !player->dead : host_.Monster(c.target) != nullptr;
    }
    if (!fits) {
        core::LogInfo("skills: {} gives up {} (a re-check failed)", caster.entity, c.skill != nullptr ? c.skill->codename : std::string("?"));
        Interrupt(caster, now, false);
    }
}

void Skills::LandBuffs(SkillCaster& caster, Clock::time_point now) {
    std::vector<SkillState::PendingBuff>& pending = caster.state.pendingBuffs;
    for (auto it = pending.begin(); it != pending.end();) {
        if (now < it->at) {
            ++it;
            continue;
        }
        const SkillState::PendingBuff b = *it;
        it = pending.erase(it);
        if (caster.dead || b.skill == nullptr) continue;
        if (b.holder == caster.entity || b.holder == 0) {
            InstallBuff(caster, caster, *b.skill, now);
        } else if (std::optional<SkillCaster> other = host_.Player(b.holder); other && !other->dead) {
            InstallBuff(caster, *other, *b.skill, now);
        }
    }
}

void Skills::Interrupt(SkillCaster& caster, Clock::time_point now, bool emptyList) {
    SkillState& s = caster.state;
    const std::uint32_t skill = s.casting && s.casting->skill != nullptr ? s.casting->skill->id : 0u;
    const bool casting = s.casting.has_value() || !s.pendingRows.empty() || !s.pendingBuffs.empty() || !s.pendingSupports.empty();
    const std::optional<SkillState::Waiting> waiting = emptyList ? std::nullopt : s.waiting;
    s.StopFighting();
    s.waiting = waiting;
    s.busyUntil = std::min(s.busyUntil, now);
    s.nextSwing = std::min(s.nextSwing, now);
    s.castUntil = {};
    std::erase_if(pendingObjects_, [&](const PendingObject& o) { return o.owner == caster.entity && o.at > now; });
    if (casting) host_.ToObservers(caster.entity, proto::EntityCastCancelled{caster.entity, skill});
}

const SkillState::Buff* Skills::AddBuff(SkillCaster& holder, const assets::SkillRow& skill, std::uint32_t ms, Clock::time_point now) {
    // A scroll's: the same one running starts its time again; otherwise the buffs running decide as
    // for a skill's.
    std::vector<SkillState::Buff>& buffs = holder.state.buffs;
    if (auto it = std::find_if(buffs.begin(), buffs.end(), [&](const SkillState::Buff& b) { return b.skill == skill.id; }); it != buffs.end()) {
        it->until = now + std::chrono::milliseconds(ms);
        host_.ToCaster(holder.entity, BuffMessage(*it, now));
        if (OutlivesLogout(skill)) host_.KeptBuffsChanged(holder.entity);
        return &*it;
    }
    std::vector<const assets::SkillRow*> running;
    for (const SkillState::Buff& b : buffs) running.push_back(table_ != nullptr ? table_->Find(b.skill) : nullptr);
    const game::BuffAdmission admission = game::AdmitBuff(skill, running);
    if (!admission.admitted) return nullptr;
    std::vector<std::size_t> replaced = admission.replaces;
    std::sort(replaced.rbegin(), replaced.rend());
    for (const std::size_t i : replaced) EndBuff(holder, i);
    buffs.push_back(MakeBuff(holder, skill, ms, now));
    const std::uint32_t token = buffs.back().token;
    host_.ToCaster(holder.entity, BuffMessage(buffs.back(), now));
    if (OutlivesLogout(skill)) host_.KeptBuffsChanged(holder.entity);
    // Its numbers made again may end it (a buff its holder's equipment does not fit: the game server's EndUnfitBuffs)
    // and move the others: found again by its token (null: gone already).
    host_.StatsChanged(holder.entity);
    const auto it = std::find_if(buffs.begin(), buffs.end(), [&](const SkillState::Buff& b) { return b.token == token; });
    return it != buffs.end() ? &*it : nullptr;
}

void Skills::RestoreBuff(SkillCaster& holder, const assets::SkillRow& skill, std::uint32_t ms, Clock::time_point now) {
    holder.state.buffs.push_back(MakeBuff(holder, skill, ms, now));
}

bool Skills::CancelBuff(SkillCaster& holder, std::uint32_t token) {
    std::vector<SkillState::Buff>& buffs = holder.state.buffs;
    const auto it = std::find_if(buffs.begin(), buffs.end(), [&](const SkillState::Buff& b) { return b.token == token; });
    if (it == buffs.end()) return false;
    const assets::SkillRow* skill = table_ != nullptr ? table_->Find(it->skill) : nullptr;
    if (skill != nullptr && skill->cancelBlocked) return false; // 'nbuf' without 'lnks'
    EndBuff(holder, std::size_t(it - buffs.begin()));
    host_.StatsChanged(holder.entity);
    return true;
}

void Skills::EndUnfitBuffs(SkillCaster& caster) {
    // A buff of its own whose conditions on what is worn no longer hold ends, but the Chinese fire shields (by their
    // codename); one another gave it stays (only its own are checked, a Tag Point on a swordsman).
    if (table_ == nullptr) return;
    std::vector<SkillState::Buff>& buffs = caster.state.buffs;
    for (std::size_t i = buffs.size(); i-- > 0;) {
        if (!buffs[i].own) continue;
        const assets::SkillRow* s = table_->Find(buffs[i].skill);
        if (s == nullptr || game::EquipmentFits(*s, caster.state.worn) || s->codename.find("SKILL_CH_FIRE_SHIELD_") != std::string::npos) continue;
        EndBuff(caster, i);
    }
}

std::optional<game::WallSpec> Skills::WallOf(const SkillState& state) const {
    if (table_ == nullptr) return std::nullopt;
    for (const SkillState::Buff& b : state.buffs) {
        const assets::SkillRow* row = table_->Find(b.skill);
        const assets::SkillParam pw = row != nullptr ? assets::FindSkillParam(*row, assets::SkillTag("pw")) : assets::SkillParam{};
        if (!pw) continue;
        game::WallSpec wall;
        wall.kinds = pw.Int(0);
        wall.defence = pw.Int(2);
        wall.absorption = pw.Int(3);
        const std::uint32_t hp = std::uint32_t(std::max(0, pw.Int(1)));
        const std::uint32_t taken = state.wallToken == b.token ? state.wallTaken : 0u;
        wall.hpLeft = hp > taken ? hp - taken : 0u;
        return wall;
    }
    return std::nullopt;
}

bool Skills::WallTook(SkillCaster& holder, std::uint32_t amount) {
    if (table_ == nullptr) return false;
    for (std::size_t i = 0; i < holder.state.buffs.size(); ++i) {
        const SkillState::Buff& b = holder.state.buffs[i];
        const assets::SkillRow* row = table_->Find(b.skill);
        const assets::SkillParam pw = row != nullptr ? assets::FindSkillParam(*row, assets::SkillTag("pw")) : assets::SkillParam{};
        if (!pw) continue;
        if (holder.state.wallToken != b.token) holder.state.wallToken = b.token, holder.state.wallTaken = 0;
        holder.state.wallTaken = std::uint32_t(std::min<std::uint64_t>(std::uint64_t(holder.state.wallTaken) + amount, 0xFFFFFFFFu));
        // Its HP reached: the wall's action ends (the original's at its next tick; ours at once).
        if (holder.state.wallTaken < std::uint32_t(std::max(0, pw.Int(1)))) return false;
        holder.state.wallToken = holder.state.wallTaken = 0;
        EndBuff(holder, i);
        return true;
    }
    return false;
}

void Skills::EndBuffsOf(SkillCaster& holder, const std::vector<std::uint32_t>& skills) {
    if (skills.empty()) return;
    std::vector<SkillState::Buff>& buffs = holder.state.buffs;
    for (std::size_t i = buffs.size(); i-- > 0;) {
        if (std::find(skills.begin(), skills.end(), buffs[i].skill) != skills.end()) EndBuff(holder, i);
    }
}

bool Skills::PlaysMusic(const SkillState& state, const std::vector<std::size_t>& ending) const {
    if (table_ == nullptr) return false;
    for (std::size_t i = 0; i < state.buffs.size(); ++i) {
        if (std::find(ending.begin(), ending.end(), i) != ending.end()) continue;
        const assets::SkillRow* row = table_->Find(state.buffs[i].skill);
        const assets::SkillParam scls = row != nullptr ? assets::FindSkillParam(*row, assets::SkillTag("scls")) : assets::SkillParam{};
        if (!scls) continue;
        // The original's music flag: a byte of the mask equal to 1 (CSkillManager_InstallSelector).
        const std::uint32_t mask = std::uint32_t(scls.Int(0));
        for (int shift = 0; shift < 32; shift += 8) {
            if (((mask >> shift) & 0xFF) == 1) return true;
        }
    }
    return false;
}

void Skills::ExpireBuffs(SkillCaster& caster, Clock::time_point now) {
    // The toggles' MP each period (out of MP: it ends), then the buffs whose time has passed.
    std::vector<SkillState::Buff>& buffs = caster.state.buffs;
    std::vector<std::size_t> ended;
    bool spent = false;
    for (std::size_t i = 0; i < buffs.size(); ++i) {
        SkillState::Buff& b = buffs[i];
        if (b.upkeepMs > 0 && now >= b.nextUpkeep) {
            if (caster.mp >= b.upkeepMp) {
                caster.mp -= b.upkeepMp;
                b.nextUpkeep += std::chrono::milliseconds(b.upkeepMs);
                spent = true;
            } else {
                ended.push_back(i);
                continue;
            }
        }
        if (b.until <= now) ended.push_back(i);
    }
    // Its own dances ('reqc' 0x20) end with its music (the original's persistent check on the music flag).
    if (table_ != nullptr && !PlaysMusic(caster.state, ended)) {
        for (std::size_t i = 0; i < buffs.size(); ++i) {
            const assets::SkillRow* row = buffs[i].own ? table_->Find(buffs[i].skill) : nullptr;
            if (row == nullptr || (assets::FindSkillParam(*row, assets::SkillTag("reqc")).Int(0) & 0x20) == 0) continue;
            if (std::find(ended.begin(), ended.end(), i) == ended.end()) ended.push_back(i);
        }
        std::sort(ended.begin(), ended.end());
    }
    if (spent) host_.VitalsChanged(caster.entity);
    if (ended.empty()) return;
    for (auto it = ended.rbegin(); it != ended.rend(); ++it) EndBuff(caster, *it);
    host_.StatsChanged(caster.entity);
}

const assets::SkillRow* Skills::Imbue(const SkillState& state, Clock::time_point now) const {
    if (table_ == nullptr) return nullptr;
    for (const SkillState::Buff& b : state.buffs) {
        const assets::SkillRow* s = b.until > now ? table_->Find(b.skill) : nullptr;
        if (s != nullptr && game::IsImbue(*s)) return s;
    }
    return nullptr;
}

proto::BuffAdded Skills::BuffMessage(const SkillState::Buff& b, Clock::time_point now) const {
    if (b.until == Clock::time_point::max()) return proto::BuffAdded{b.token, b.skill, 0u}; // no end (a toggle)
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(b.until - now).count();
    return proto::BuffAdded{b.token, b.skill, std::uint32_t(std::clamp<long long>(left, 0, 0xFFFFFFFFll))};
}

void Skills::Fight(SkillCaster& caster, Clock::time_point now) {
    // Walked into reach (the skill's range, else the weapon's, plus both bodies), then a swing each
    // time the last one is over; each swing's hits land a little into it (ours: the clips' hit
    // moments are the client's; a swing's share 0.3, then 0.3 apart).
    // The action list's count told whenever it changed since, whatever changed it (a swing's or a cast's time over, a
    // walk or a pick-up ending the fight, a waiting skill dropped): a count left at 2 kept the client from sending any
    // skill, the player clicking again and again (the user's report 2026-11-21).
    TellHeld(caster, now);
    if (!caster.dead) Companions(caster, now);
    // The swings' mana at their impact (no more than it has then).
    if (!caster.state.pendingCosts.empty()) {
        bool paid = false;
        std::erase_if(caster.state.pendingCosts, [&](const SkillState::PendingCost& c) {
            if (now < c.at) return false;
            if (!caster.dead) caster.mp -= std::min(caster.mp, c.mp);
            paid = true;
            return true;
        });
        if (paid) host_.VitalsChanged(caster.entity);
    }
    CheckCasting(caster, now);
    CastRows(caster, now);
    LandBuffs(caster, now);
    LandHits(caster, now);
    LandSupports(caster, now);
    LandTaunts(caster, now);
    // The resurrections whose cast is over: offered to those still dead.
    for (auto it = caster.state.pendingRevivals.begin(); it != caster.state.pendingRevivals.end();) {
        if (now < it->at) {
            ++it;
            continue;
        }
        const SkillState::PendingRevival r = *it;
        it = caster.state.pendingRevivals.erase(it);
        if (caster.dead || r.skill == nullptr) continue;
        for (const std::uint32_t target : r.targets) {
            if (std::optional<SkillCaster> other = host_.Player(target); other && other->dead) host_.OfferRevival(caster.entity, target, *r.skill);
        }
    }
    Links(caster, now);
    Pulse(caster, now);
    if (caster.state.approach) Approach(caster, now);
    SkillState& s = caster.state;
    // The action list: the front free (its swing or its skill's hold over), a waiting
    // command starts (the next actor tick); with none, a skill that went off hands on to the basic attack
    // only when its Action_AutoAttackType is 1, else the fight ends there.
    if (now + s.holdSpare >= s.busyUntil && s.waiting && !caster.dead) {
        const SkillState::Waiting w = *s.waiting;
        s.waiting.reset();
        s.queued = nullptr;
        s.afterSkill = -1;
        if (w.pickUp != 0) {
            s.attackTarget = 0; // the fight it waited behind ends; the walk to the item and its taking are the host's
            host_.PickUpWaited(caster.entity, w.pickUp);
        } else if (w.attack) {
            Attack(caster, w.target, now, true);
        } else {
            Use(caster, w.use, now, true);
        }
    } else if (now >= s.busyUntil && s.afterSkill >= 0) {
        // Type 1 hands on to the basic attack on its target, unless that target lies down (the fight ends); 0 and 2 end
        // the command.
        bool hand = s.afterSkill == 1;
        if (hand) {
            if (const auto t = host_.MonsterTarget(s.attackTarget); t && t->down) hand = false;
        }
        if (!hand && s.queued == nullptr) s.attackTarget = 0;
        s.afterSkill = -1;
    }
    TellHeld(caster, now);
    if (s.attackTarget == 0) return;
    // Frozen, asleep or stunned: no swing while it lasts (its fight goes on after it).
    if (s.states.Locked()) return;
    // A player no longer hostile (its war over): the attack ends.
    if (host_.IsPlayer(s.attackTarget) && !host_.Hostile(caster.entity, s.attackTarget)) {
        s.StopAttacking();
        return;
    }
    const game::CombatStats* them = host_.Monster(s.attackTarget);
    const assets::SkillRow* skill = s.queued != nullptr ? s.queued : DefaultAttack(caster);
    const auto me = host_.Position(caster.entity), there = host_.Position(s.attackTarget);
    if (caster.dead || them == nullptr || skill == nullptr || !me || !there) {
        // The fight ends; a command waiting behind it goes on, the action list's next.
        const std::optional<SkillState::Waiting> waiting = caster.dead ? std::nullopt : s.waiting;
        s.StopAttacking();
        s.waiting = waiting;
        return;
    }
    // Its range: its own (Action_Range), else the
    // caster's attack range (the weapon's and its buffs', e.g. the bow's 'ru' buffs: stats.range) plus the skill's
    // own 'ru' (BOW_POWER_C / D: +40 / +50), and what its passives add (the crossbow's, the Wizard's).
    float range = skill->range > 0 ? float(skill->range) : std::max(caster.stats.range, 6.0f);
    if (skill->range <= 0) {
        if (const assets::SkillParam ru = assets::FindSkillParam(*skill, assets::SkillTag("ru")); ru && skill->attack) range += float(ru.Int(0));
    }
    range += float(game::BoostsOf(*skill, false, caster.state.passives.modifiers).range);
    const bool plain = skill == DefaultAttack(caster);
    // A skill's bow or crossbow reaches less in a fortress's world: x 0.7 and x 0.6.
    if (!plain && host_.InFortressWorld(caster.entity)) {
        if (caster.stats.weaponKind == 6) range *= 0.7f;
        if (caster.stats.weaponKind == 12) range *= 0.6f;
    }
    const proto::FlatPosition a = proto::ToFlat(*me);
    proto::FlatPosition b = proto::ToFlat(*there);
    // In range within the two bodies and the range, whole, and two more; the walk's end: the range itself when short, two
    // under it below 10, else 70 % of it.
    float base = them->bodyRadius + kPlayerRadius;
    const auto shorter = [](float r) { return r <= 4.0f ? r : r < 10.0f ? r - 2.0f : r * 0.7f; };
    float hit = range, stop = shorter(range);
    double distance = 0.0;
    if (const std::optional<proto::WorldPosition> near = host_.NearestPointOf(s.attackTarget, a)) {
        // A fortress's structure: measured in 3D to its contact point on its navigation object, no bodies. A basic
        // attack's stop and hit are both the shortened range,
        // or a bow's or crossbow's 91 and 130, a staff's or harp's 70 and 100; a skill keeps its range
        // with the player's body (inferred: the original measures it to the last contact point a basic attack stored).
        b = proto::ToFlat(*near);
        distance = std::sqrt(std::pow(a.x - b.x, 2.0) + std::pow(a.z - b.z, 2.0) + std::pow(double(me->y) - double(near->y), 2.0));
        base = plain ? 0.0f : kPlayerRadius;
        if (plain) {
            const int kind = caster.stats.weaponKind;
            if (kind == 6 || kind == 12) {
                stop = 91.0f;
                hit = 130.0f;
            } else if (kind == 10 || kind == 11 || kind == 14 || kind == 15) {
                stop = 70.0f;
                hit = 100.0f;
            } else {
                hit = stop = shorter(range);
            }
        }
    } else {
        distance = std::hypot(a.x - b.x, a.z - b.z);
    }
    const float reach = std::trunc(base + hit) + 2.0f;
    if (std::trunc(distance) > reach) {
        // A swing going on finishes where it stands, its target gone farther or not (the original
        // fires it all the same, beyond its range); the walk after it waits for the swing's end.
        if (now < s.nextSwing) return;
        // Stopped short of it (its walk's end reached, the target gone on: a fleeing one): aimed again at once, as the
        // original's controller does every frame; walking, again when the target went 5 farther, 300 ms apart.
        const proto::FlatPosition aimed = proto::ToFlat(s.aim);
        if (!host_.Moving(caster.entity) || (std::hypot(aimed.x - b.x, aimed.z - b.z) > 5.0 && now >= s.nextAim)) {
            const double flat = std::max(1.0, std::hypot(a.x - b.x, a.z - b.z));
            const double end = double(base) + double(stop);
            host_.MoveTo(caster.entity, proto::FromFlat({b.x + (a.x - b.x) / flat * end, b.z + (a.z - b.z) / flat * end}, there->y));
            s.aim = *there;
            s.nextAim = now + std::chrono::milliseconds(300);
        }
        return;
    }
    if (host_.Moving(caster.entity)) host_.MoveTo(caster.entity, *me); // stop where it stands
    if (now < s.nextSwing) return;
    // A target that is down: only a down attack ('da') may strike it; the
    // swing waits for it to rise.
    if (const auto target = host_.MonsterTarget(s.attackTarget); target && target->down && game::AttackOf(*skill).downPercent <= 0) return;
    // Its target in sight, the walk on the floor to it not blocked (the cast's validation, mask 0x40, after the walk into
    // reach:; refused 0x3010 and the command ends; a player's skill and basic attack).
    if (!host_.InSight(caster.entity, s.attackTarget)) {
        s.StopAttacking();
        host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill->id, proto::SkillUseStatus::Obstacle});
        return;
    }
    // A queued skill: its wait now, its mana at its impact (it may have become unaffordable meanwhile).
    std::uint32_t cost = 0;
    if (skill == s.queued) {
        s.queued = nullptr;
        s.afterSkill = skill->autoAttack;
        const std::uint32_t mana = Mana(caster.stats, *skill, caster.state.passives.modifiers);
        if (caster.mp < mana) {
            host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill->id, proto::SkillUseStatus::Mana});
            return;
        }
        cost = std::min(mana, PaidMana(caster, *skill));
        StartCooldown(caster, *skill, now);
    }
    // A bow or a crossbow shoots one of the arrows (bolts) worn beside it; none: no shot.
    if (skill->consumesAmmo && host_.TakeAmmunition(caster.entity, AmmunitionOf(*skill)) <= 0) {
        s.attackTarget = 0;
        host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill->id, proto::SkillUseStatus::Ammunition});
        return;
    }
    host_.ToObservers(caster.entity, proto::EntitySwing{caster.entity, s.attackTarget, skill->id});
    // A companion strikes what its holder's attack skills strike.
    if (skill->attack) {
        s.companionTarget = s.attackTarget;
        s.lastAttack = now;
    }
    // Attacking: in battle for 20 s; and a skill started ends what ends on one.
    s.battleUntil = now + std::chrono::seconds(20);
    if (skill != DefaultAttack(caster) && EndOnEvent(caster, 2)) host_.StatsChanged(caster.entity);
    // Its rows: the skill, then those its col 9 chains (a skill of several hits), each a cast of its own with its own
    // attack, area and time. A row's hits land at its impact: its preparing and casting on the caster's skill tick
    // (their sum), none: at once as it is cast; a shot's as it is released, never after its flight (the client shows
    // them as its arrow arrives). The client
    // shows them at its clip's damage moments by their numbers. The imbue running as the
    // swing begins goes with every row (the cast keeps it, a chain hands it on).
    // The timing: every command has a budget of 500 ms; a row with a next waits its duration less
    // what is left of the budget (the budget used up first) and its own cast's release, then the next is cast; the last
    // row holds its preparing, casting and duration from its cast (and its release), plus the budget used when the basic
    // attack follows (Action_AutoAttackType not 0) and no command waits. The first row is struck now (its area chosen
    // now); the later rows at their casts (Skills::CastRows: the area then, the chain ended by a target dead or gone).
    const assets::SkillRow* imbue = Imbue(s, now);
    const auto ms = [](int v) { return std::chrono::milliseconds(v); };
    int budget = 500, used = 0;
    int rowAt = 0;      // the row's cast, ms from now
    int firstImpact = 0, lastImpact = 0, holdMs = 0;
    Clock::time_point firstAt = now;
    int strikes = 0;
    const assets::SkillRow* row = skill;
    for (int n = 0; row != nullptr && n < 16; ++n) {
        const int rowLead = std::max(0, row->preparingMs) + std::max(0, row->castingMs);
        const Clock::time_point rowCast = now + ms(rowAt);
        const Clock::time_point impact = rowLead > 0 ? OnTick(s, now, rowCast + ms(rowLead)) : rowCast;
        const int impactMs = int(std::chrono::duration_cast<std::chrono::milliseconds>(impact - now).count());
        if (n == 0) {
            firstImpact = impactMs;
            firstAt = impact;
            if (rowLead > 0) {
                // A cast: struck at its release, its target and its area then; the slot
                // re-checked till then (CheckCasting).
                s.pendingRows.push_back({impact, impact, row, imbue, s.attackTarget, strikes, reach, true});
                strikes += std::max(0, AttackFor(caster.book, *row, caster.state.passives.modifiers).hits);
                Occupy(caster, *row, s.attackTarget, impact, cost, now);
            } else {
                strikes += StrikeRow(caster, *row, imbue, s.attackTarget, reach, strikes, impact, now);
            }
        } else {
            s.pendingRows.push_back({rowCast, impact, row, imbue, s.attackTarget, strikes, reach});
            strikes += std::max(0, AttackFor(caster.book, *row, caster.state.passives.modifiers).hits);
        }
        lastImpact = impactMs;
        const assets::SkillRow* next = row->chainNext != 0 && table_ != nullptr ? table_->Find(row->chainNext) : nullptr;
        const int duration = std::max(0, row->durationMs);
        if (next != nullptr) {
            int wait = 0;
            if (duration > budget) {
                wait = duration - budget;
                used += budget;
                budget = 0;
            } else {
                budget -= duration; // a row no longer than what is left waits 0; the budget shrinks, used does not grow
            }
            rowAt = std::max(impactMs, rowAt + wait);
        } else {
            holdMs = std::max(impactMs, rowAt + rowLead + duration);
        }
        row = next;
    }
    holdMs = std::max({holdMs, rowAt, 1});
    if (cost > 0) s.pendingCosts.push_back({firstAt, cost});
    // Until its impact it stands; no walk till the chain's last row is released.
    s.castUntil = now + ms(firstImpact);
    s.chainUntil = now + ms(lastImpact);
    // A charge ('tel3': its time, its length): after the hits, toward the target
    // its length at most, else to its body; not while rooted.
    if (const assets::SkillParam charge = assets::FindSkillParam(*skill, assets::SkillTag("tel3")); charge && !s.states.Rooted()) {
        const double gap = std::max(0.0, distance - double(them->bodyRadius) - double(kPlayerRadius));
        const double step = std::min(double(charge.Int(1)), gap);
        if (step > 0.5 && distance > 0.0) {
            const proto::FlatPosition to{a.x + (b.x - a.x) / distance * step, a.z + (b.z - a.z) / distance * step};
            host_.Blink(caster.entity, proto::FromFlat(to, me->y), float(step));
        }
    }
    // The next swing and the hold. A basic attack: its hold (its casting and duration from the cast) or its reuse (col 14),
    // the longer, both counted from when this swing was due while the attack goes on, not from the update that found it
    // due, so the rhythm is the skill's at any tick rate (the original's action controller runs every frame); a
    // swing after a pause counts from now. The crossbow's 553 + 780 and the staff's 934 + 732 are their reuses, 1,333 and
    // 1,666 (ours added the casting to the reuse). A skill: its hold, then the
    // basic attack after it (its wait runs apart: StartCooldown).
    const bool basic = skill == DefaultAttack(caster);
    // (A skill's hold counts from its swing: the catch-up is the basic attack's rhythm alone.)
    const Clock::time_point due = basic && now - s.nextSwing < std::chrono::milliseconds(250) ? s.nextSwing : now;
    const int spare = !basic && skill->autoAttack != 0 ? used : 0;
    // Frostbite and slow stretch the time to its next swing (StateSet::AttackTimePercent: 200, 125; as a monster's).
    const int pace = basic ? std::max(holdMs, skill->cooldownMs) : holdMs + spare;
    s.nextSwing = due + ms(pace * s.states.AttackTimePercent() / 100);
    // Busy: a basic attack its hold (its reuse after it is between swings), a skill its whole hold (less its spare part
    // when a command waits: the Fight's waiting check); frostbite and slow stretch it as its pace (the original keeps
    // the scaled life and holds a new action skill till it has passed).
    const int stretch = s.states.AttackTimePercent();
    s.busyUntil = basic ? now + ms(holdMs * stretch / 100) : now + ms((holdMs + spare) * stretch / 100);
    s.holdSpare = ms(spare);
}

int Skills::StrikeRow(SkillCaster& caster, const assets::SkillRow& row, const assets::SkillRow* imbue, std::uint32_t target, float reach,
                      int strikes, Clock::time_point impact, Clock::time_point now) {
    // One row of a swing cast: its targets chosen now (the selected one and its area's), its hits at its impact.
    SkillState& s = caster.state;
    const game::AttackSpec attack = AttackFor(caster.book, row, caster.state.passives.modifiers);
    // A DOT (a link with 'lks2' and a 'puls': the Warlock's): no hit of its swing; its link from the impact strikes its
    // target each period (33 4.7).
    if (assets::FindSkillParam(row, assets::SkillTag("lks2")) && assets::FindSkillParam(row, assets::SkillTag("lnks")) &&
        assets::FindSkillParam(row, assets::SkillTag("puls")).Int(0) > 0) {
        if (AdmitLink(s, row, target) == proto::SkillUseStatus::Ok) {
            const std::uint32_t life = row.hasDuration && row.buffDurationMs > 0 ? std::uint32_t(row.buffDurationMs) : 5000u;
            StartLink(caster, row, target, 0, impact, impact + std::chrono::milliseconds(life));
        }
        return 0;
    }
    const auto me = host_.Position(caster.entity);
    const proto::FlatPosition a = me ? proto::ToFlat(*me) : proto::FlatPosition{};
    std::vector<Struck> targets = TargetsOf(caster, row, target);
    // An imbue's area (the lightning's jump): a row that takes a share of the imbue, on its selected target alone, strikes
    // the imbue's area too, those it adds with the magical parts only (the imbue's 'efr'; the
    // transfer adds only to a list of one; its entries marked).
    const std::size_t own = targets.size();
    if (imbue != nullptr && attack.formula && attack.imbuePercent > 0 && own == 1) {
        for (const Struck& j : TargetsOf(caster, *imbue, target)) {
            if (j.target != target) targets.push_back(j);
        }
    }
    const std::uint32_t use = ++s.uses;
    for (std::size_t k = 0; k < targets.size(); ++k) {
        const Struck& t = targets[k];
        // The selected target is struck wherever it went once the swing went off (the original's hit lands beyond the
        // skill's range); the area's others where they stood, a little more.
        float hitReach = std::max(reach * 1.5f, 2000.0f);
        if (t.target != target) {
            const auto p = host_.Position(t.target);
            const proto::FlatPosition q = p ? proto::ToFlat(*p) : a;
            hitReach = float(std::hypot(a.x - q.x, a.z - q.z)) + 50.0f;
        }
        for (int h = 0; h < attack.hits; ++h) {
            s.pendingHits.push_back({impact, t.target, attack, hitReach, t.percent, &row, imbue, k >= own, std::uint8_t(std::min(255, strikes + h))});
            s.pendingHits.back().use = use;
        }
    }
    (void)now;
    return std::max(0, attack.hits);
}

void Skills::CastRows(SkillCaster& caster, Clock::time_point now) {
    // A chain's later rows at their casts: a target dead or gone ends the chain there (the
    // command returns 0: no later row is cast, the hold is over).
    SkillState& s = caster.state;
    while (!s.pendingRows.empty() && s.pendingRows.front().castAt <= now) {
        const SkillState::PendingRow r = s.pendingRows.front();
        s.pendingRows.erase(s.pendingRows.begin());
        if (caster.dead || r.row == nullptr || host_.Monster(r.target) == nullptr) {
            s.pendingRows.clear();
            s.busyUntil = std::min(s.busyUntil, now);
            s.holdSpare = {};
            break;
        }
        if (r.row->consumesAmmo && !r.first) host_.TakeAmmunition(caster.entity, AmmunitionOf(*r.row)); // held at none: the row goes on
        StrikeRow(caster, *r.row, r.imbue, r.target, r.reach, r.strikes, r.impactAt, now);
    }
}

int Skills::AmmunitionOf(const assets::SkillRow& row) {
    // 'cnsm' (the item's TID3, TID4, the count a hit) x the row's hits ('mc').
    const assets::SkillParam c = assets::FindSkillParam(row, assets::SkillTag("cnsm"));
    return std::max(1, c ? c.Int(2) : 1) * std::max(1, game::AttackOf(row).hits);
}

namespace {

// A candidate in a line from the caster: within the line's length and both
// bodies of the caster, and within its own body and the line's half-width of the line (its
// distance times the sine of its angle to the line; one behind the caster within the length counts
// too, as in the original). On the ground: heights left out.
bool InLine(const proto::FlatPosition& from, const proto::FlatPosition& to, double dx, double dz, double width, double casterBody, double body) {
    const double rx = to.x - from.x, rz = to.z - from.z;
    const double d = std::hypot(rx, rz), length = std::hypot(dx, dz);
    if (d > length + casterBody + body) return false;
    double cosine = 0.0;
    if (d > 0.0 && length > 0.0) cosine = std::clamp((rx * dx + rz * dz) / (d * length), -1.0, 1.0);
    return std::sin(std::acos(cosine)) * d <= body + width;
}

} // namespace

std::vector<Skills::Struck> Skills::TargetsOf(const SkillCaster& caster, const assets::SkillRow& row, std::uint32_t selected, std::uint32_t kind,
                                              const proto::WorldPosition* centre) const {
    // The selected target takes all of it. An area (its 'efr' of the kind: shape, range, the most at
    // once, the falloff, whom it takes) adds the hostile others, the first
    // 100 - the falloff, each next the share before times that, the most at once counting the
    // selected target too (a pulse has none: empties the list first):
    // - 1 around the caster, 2 around the target: within the
    //   caster's body, the range and their own body of the centre, in the order of their ids;
    // - 3 a line from the caster through the target, the skill's range long (none: the weapon's),
    //   4 the line from the caster to the target: the range its half-width;
    // - 6 a transfer: from the target, each next the nearest not struck yet within the
    //   range of the last.
    std::vector<Struck> out;
    if (selected != 0) out.push_back({selected, 100});
    const assets::SkillParam area = assets::FindSkillParam(row, assets::SkillTag("efr"), kind);
    if (!area || (area[5] & 0x08) == 0) return out; // the hostile; self and party areas are not attacks'
    const int shape = area.Int(1);
    const double range = double(area[2]);
    const std::size_t cap = area[3];
    const int falloff = std::clamp(area.Int(4), 0, 100);
    const std::optional<proto::WorldPosition> me = centre != nullptr ? std::optional<proto::WorldPosition>(*centre) : host_.Position(caster.entity);
    const std::optional<proto::WorldPosition> target = selected != 0 ? host_.Position(selected) : me;
    if (!me || !target || (selected == 0 && shape != 1)) return out; // without a target: around the caster only
    const proto::FlatPosition c = proto::ToFlat(*me), t = proto::ToFlat(*target);
    int percent = 100 - falloff;
    const auto take = [&](std::uint32_t id) {
        out.push_back({id, percent});
        percent = percent * (100 - falloff) / 100;
    };
    const auto full = [&] { return cap != 0 && out.size() >= cap; };
    // Who the area may take: the monsters about; a skill for enemy players alone (TargetGroup_Enemy_P without Enemy_M:
    // the KB socket stone's knock-back) the players about that are the caster's enemies, never a monster.
    const bool playersOnly = row.targetEnemyPlayer && !row.targetEnemyMonster;
    const auto candidates = [&](const proto::WorldPosition& at, float radius) {
        if (!playersOnly) return host_.MonstersNear(at, radius);
        std::vector<std::uint32_t> ids = host_.PlayersNear(at, radius);
        std::erase_if(ids, [&](std::uint32_t id) { return id == caster.entity || !host_.HostileInArea(caster.entity, id); });
        return ids;
    };
    if (shape == 6) {
        // From the last struck, the nearest within the range, until the most at once.
        std::vector<std::uint32_t> left = candidates(*target, float(range * double(std::max<std::size_t>(cap, 1)) + 200.0));
        proto::FlatPosition last = t;
        while (!full()) {
            std::uint32_t best = 0;
            double nearest = 10000.0;
            for (const std::uint32_t id : left) {
                if (id == 0 || id == selected) continue;
                const std::optional<proto::WorldPosition> where = host_.Position(id);
                if (!where || host_.Monster(id) == nullptr) continue;
                const proto::FlatPosition p = proto::ToFlat(*where);
                const double d = std::hypot(p.x - last.x, p.z - last.z);
                if (d <= range && d < nearest) {
                    nearest = d;
                    best = id;
                }
            }
            if (best == 0) break;
            take(best);
            std::replace(left.begin(), left.end(), best, 0u);
            last = proto::ToFlat(*host_.Position(best));
        }
        return out;
    }
    if (shape == 1 || shape == 2) {
        const proto::FlatPosition centre = shape == 1 ? c : t;
        const double height = shape == 1 ? double(me->y) : double(target->y);
        const double search = double(kPlayerRadius) + range;
        for (const std::uint32_t id : candidates(shape == 1 ? *me : *target, float(search) + 100.0f)) {
            if (full()) break;
            if (id == selected) continue;
            const game::CombatStats* them = host_.Monster(id);
            const std::optional<proto::WorldPosition> where = host_.Position(id);
            if (them == nullptr || !where) continue;
            const proto::FlatPosition p = proto::ToFlat(*where);
            const double dy = double(where->y) - height;
            if (std::sqrt((p.x - centre.x) * (p.x - centre.x) + (p.z - centre.z) * (p.z - centre.z) + dy * dy) > search + double(them->bodyRadius)) continue;
            take(id);
        }
        return out;
    }
    if (shape == 3 || shape == 4) {
        double dx = t.x - c.x, dz = t.z - c.z;
        if (shape == 3) {
            const double length = row.range > 0 ? double(row.range) : double(caster.stats.range);
            const double d = std::hypot(dx, dz);
            dx = d > 0.0 ? dx / d * length : 0.0;
            dz = d > 0.0 ? dz / d * length : 0.0;
        }
        const float reach = float(std::hypot(dx, dz) + double(kPlayerRadius) + range + 100.0);
        for (const std::uint32_t id : candidates(*me, reach)) {
            if (full()) break;
            if (id == selected) continue;
            const game::CombatStats* them = host_.Monster(id);
            const std::optional<proto::WorldPosition> where = host_.Position(id);
            if (them == nullptr || !where) continue;
            if (InLine(c, proto::ToFlat(*where), dx, dz, range, double(kPlayerRadius), double(them->bodyRadius))) take(id);
        }
    }
    return out;
}

int Skills::ImbueMastery(int skillLevel, int imbueLevel) {
    if (skillLevel <= 90) return skillLevel;
    return imbueLevel > 90 ? imbueLevel : 90;
}

void Skills::Pulse(SkillCaster& caster, Clock::time_point now) {
    // A buff that pulses: each period while it lasts, its area around the caster struck at once with
    // the skill's attack (the Warlock's soul chaos: its own damage).
    if (caster.dead || table_ == nullptr) return;
    for (SkillState::Buff& b : caster.state.buffs) {
        if (b.pulseMs <= 0 || now < b.nextPulse || now >= b.until) continue;
        b.nextPulse += std::chrono::milliseconds(b.pulseMs);
        const assets::SkillRow* row = table_->Find(b.skill);
        const auto me = host_.Position(caster.entity);
        if (row == nullptr || !me) continue;
        // A heal: over time on its holder, or a field's on the players its area takes (K7).
        if (game::HealOf(*row)) {
            if (!assets::FindSkillParam(*row, assets::SkillTag("efr"), 2)) {
                Recover(caster, b.pulseHeal.hp, b.pulseHeal.mp);
                continue;
            }
            for (const Struck& t : FriendsOf(caster, *row, 0, 2)) {
                if (t.target == caster.entity) {
                    HealOne(caster, caster, *row, 100, now);
                } else if (std::optional<SkillCaster> other = host_.Player(t.target)) {
                    HealOne(caster, *other, *row, 100, now);
                }
            }
            continue;
        }
        const game::AttackSpec attack = AttackFor(caster.book, *row, caster.state.passives.modifiers);
        const proto::FlatPosition a = proto::ToFlat(*me);
        for (const Struck& t : TargetsOf(caster, *row, 0, 2)) {
            const auto p = host_.Position(t.target);
            const proto::FlatPosition q = p ? proto::ToFlat(*p) : a;
            SkillState::PendingHit hit{now, t.target, attack, float(std::hypot(a.x - q.x, a.z - q.z)) + 50.0f, t.percent, row};
            hit.returned = true;
            hit.use = ++caster.state.uses; // an aura's pulse: each target's hit a use of its own (as the original's per hit)
            caster.state.pendingHits.push_back(hit);
        }
    }
}

bool Skills::Support(SkillCaster& caster, const proto::UseSkill& u, const assets::SkillRow& skill, Clock::time_point now) {
    // A heal or a cure: on the player chosen (oneself, or another living one in
    // its reach), or, asking for none, on the caster; then the players its area takes. Its mana and
    // wait now, the swing; it lands on each as the swing's hits would (ours: 0.3 into its time).
    const auto refuse = [&](proto::SkillUseStatus status) {
        host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill.id, status});
        return false;
    };
    std::uint32_t selected = 0;
    if (skill.targetRequired) {
        if (u.target == 0 || u.target == caster.entity) {
            if (!skill.targetSelf) return refuse(proto::SkillUseStatus::Target);
            selected = caster.entity;
        } else {
            std::optional<SkillCaster> other = host_.Player(u.target);
            const auto me = host_.Position(caster.entity), there = host_.Position(u.target);
            if (!other || other->dead || !me || !there) return refuse(proto::SkillUseStatus::Target);
            if (!ReachFriend(caster, u, skill, now)) return true; // walked toward it first (D9)
            selected = u.target;
        }
    }
    StartCooldown(caster, skill, now);
    host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill.id, proto::SkillUseStatus::Ok});
    host_.ToObservers(caster.entity, proto::EntitySwing{caster.entity, selected != 0 ? selected : caster.entity, skill.id});
    std::vector<Struck> targets = FriendsOf(caster, skill, selected, 1);
    if (targets.empty()) targets.push_back({caster.entity, 100}); // no target, no area: the caster
    const assets::SkillParam area = assets::FindSkillParam(skill, assets::SkillTag("efr"), 1);
    const bool transfer = area && area.Int(1) == 6;
    // At its impact (the caster standing till then), wherever the healed went meanwhile.
    const Clock::time_point at = Stand(caster, skill, now);
    PayAtRelease(caster, skill, selected, at, now);
    for (const Struck& t : targets) caster.state.pendingSupports.push_back({at, t.target, &skill, transfer ? t.percent : 100});
    core::LogInfo("skills: {} casts {} on {} player(s)", caster.entity, skill.codename, targets.size());
    return false;
}

bool Skills::Revive(SkillCaster& caster, const proto::UseSkill& u, const assets::SkillRow& skill, Clock::time_point now) {
    // A resurrection: on a dead player in reach of no higher level
    // than its 'resu' raises, or the dead its area takes (a party's:
    // none until parties come). Its mana and wait now, the swing; each is offered it.
    const auto refuse = [&](proto::SkillUseStatus status) {
        host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill.id, status});
        return false;
    };
    const game::ReviveSpec revive = game::ReviveOf(skill);
    std::vector<std::uint32_t> raised;
    if (skill.targetRequired) {
        std::optional<SkillCaster> other = u.target != 0 && u.target != caster.entity ? host_.Player(u.target) : std::nullopt;
        const auto me = host_.Position(caster.entity), there = host_.Position(u.target);
        if (!other || !other->dead || !me || !there) return refuse(proto::SkillUseStatus::Target);
        if (other->stats.level > revive.maxLevel) return refuse(proto::SkillUseStatus::ReviveLevel);
        if (!ReachFriend(caster, u, skill, now)) return true; // walked toward it first (D9)
        raised.push_back(u.target);
    } else {
        for (const Struck& t : FriendsOf(caster, skill, 0, 1)) {
            if (t.target == caster.entity) continue;
            std::optional<SkillCaster> other = host_.Player(t.target);
            if (other && other->dead && other->stats.level <= revive.maxLevel) raised.push_back(t.target);
        }
    }
    // Its wait now, its mana and its offer at the impact: preparing and casting over, on the caster's
    // skill tick (the proposal from the impact's engage).
    StartCooldown(caster, skill, now);
    host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill.id, proto::SkillUseStatus::Ok});
    host_.ToObservers(caster.entity, proto::EntitySwing{caster.entity, raised.empty() ? caster.entity : raised[0], skill.id});
    const Clock::time_point impact = Stand(caster, skill, now);
    caster.state.pendingCosts.push_back({impact, Mana(caster.stats, skill, caster.state.passives.modifiers)});
    caster.state.pendingRevivals.push_back({impact, raised, &skill});
    core::LogInfo("skills: {} casts {} on {} dead player(s)", caster.entity, skill.codename, raised.size());
    return false;
}

void Skills::LandSupports(SkillCaster& caster, Clock::time_point now) {
    std::vector<SkillState::PendingSupport>& pending = caster.state.pendingSupports;
    for (auto it = pending.begin(); it != pending.end();) {
        if (now < it->at) {
            ++it;
            continue;
        }
        const SkillState::PendingSupport s = *it;
        it = pending.erase(it);
        if (caster.dead || s.row == nullptr) continue;
        if (s.target == caster.entity) {
            HealOne(caster, caster, *s.row, s.percent, now);
            host_.ToObservers(caster.entity, proto::SkillTook{caster.entity, caster.entity, s.row->id});
        } else if (std::optional<SkillCaster> other = host_.Player(s.target); other && !other->dead) {
            HealOne(caster, *other, *s.row, s.percent, now);
            host_.ToObservers(s.target, proto::SkillTook{s.target, caster.entity, s.row->id});
        }
    }
}

void Skills::HealOne(SkillCaster& healer, SkillCaster& holder, const assets::SkillRow& row, int percent, Clock::time_point now) {
    // On a living player: its heal, then its cure (the holder's own rolls).
    if (holder.dead) return;
    const game::SkillBoosts boosts = game::BoostsOf(row, false, healer.state.passives.modifiers);
    if (const game::HealSpec heal = game::HealOf(row, boosts.heal)) {
        const game::HealAmount amount = game::HealFor(heal, percent, healer.stats, holder.stats);
        Recover(holder, amount.hp, amount.mp);
        // A heal of AI type 72 ('H'): half of what it gives as threat, from the healer, on the monsters
        // fighting the healed one (the amounts handed to the recovery).
        // A Guard Aggro on the healer takes its share first (lkag).
        if (row.aiType == 72) {
            const std::int64_t whole = std::int64_t(std::trunc(double(amount.hp + amount.mp) * 0.5));
            std::int64_t kept = whole;
            for (const auto& [guard, link] : CopiesOf(healer.state, "lkag")) {
                const std::int64_t share = std::int64_t(std::trunc(double(whole) * double(assets::FindSkillParam(*link, assets::SkillTag("lkag"))[0]) / 100.0));
                if (share <= 0) continue;
                kept -= share;
                host_.HealThreat(guard, holder.entity, share);
            }
            host_.HealThreat(healer.entity, holder.entity, kept);
        }
    }
    const game::CureSpec cure = game::CureOf(row);
    if (cure && holder.state.states.Cure(cure, holder.state.rolls, random_, now)) {
        std::vector<proto::StateRecord> records;
        for (const game::ActiveState& a : holder.state.states.All()) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(a.Ends() - now).count();
            records.push_back({std::uint8_t(a.kind), std::uint8_t(std::clamp(a.level, 0, 255)), std::uint32_t(std::max<long long>(0, left))});
        }
        host_.ToObservers(holder.entity, proto::EntityStates{holder.entity, std::move(records)});
    }
}

std::vector<Skills::Struck> Skills::FriendsOf(const SkillCaster& caster, const assets::SkillRow& row, std::uint32_t selected, std::uint32_t kind) const {
    std::vector<Struck> out;
    if (selected != 0) out.push_back({selected, 100});
    const assets::SkillParam area = assets::FindSkillParam(row, assets::SkillTag("efr"), kind);
    if (!area) return out;
    int shape = area.Int(1);
    const std::uint32_t mask = area[5];
    if ((mask & 0x08) != 0) return out; // the hostile: an attack's
    if ((mask == 4 || mask == 5) && shape != 5 && shape != 6) shape = 5; // a party mask's area is the party
    const double range = double(area[2]);
    const std::size_t cap = area[3];
    const int falloff = std::clamp(area.Int(4), 0, 100);
    const std::vector<std::uint32_t> party = host_.PartyOf(caster.entity);
    const auto admits = [&](std::uint32_t id) {
        if (id == caster.entity) return (mask & 0x01) != 0;
        if ((mask & 0x02) != 0) return true;
        return (mask & 0x04) != 0 && std::find(party.begin(), party.end(), id) != party.end();
    };
    const auto taken = [&](std::uint32_t id) { return std::any_of(out.begin(), out.end(), [&](const Struck& s) { return s.target == id; }); };
    const auto full = [&] { return cap != 0 && out.size() >= cap; };
    const std::optional<proto::WorldPosition> me = host_.Position(caster.entity);
    const std::optional<proto::WorldPosition> from = selected != 0 ? host_.Position(selected) : me;
    if (!me || !from) return out;
    const auto candidates = [&](const proto::WorldPosition& at, double radius) {
        std::vector<std::uint32_t> ids = host_.PlayersNear(at, float(radius));
        if (std::find(ids.begin(), ids.end(), caster.entity) == ids.end()) ids.insert(ids.begin(), caster.entity);
        return ids;
    };
    if (shape == 6) {
        // A transfer: from the selected (none: nobody), each next the nearest admitted
        // within the range of the last.
        if (selected == 0) return out;
        int percent = 100 - falloff;
        proto::FlatPosition last = proto::ToFlat(*from);
        const std::vector<std::uint32_t> ids = candidates(*from, range * double(std::max<std::size_t>(cap, 1)) + 200.0);
        while (!full()) {
            std::uint32_t best = 0;
            double nearest = 1e9;
            for (const std::uint32_t id : ids) {
                if (taken(id) || !admits(id)) continue;
                const auto where = host_.Position(id);
                if (!where) continue;
                const proto::FlatPosition p = proto::ToFlat(*where);
                const double d = std::hypot(p.x - last.x, p.z - last.z);
                if (d <= range && d < nearest) {
                    nearest = d;
                    best = id;
                }
            }
            if (best == 0) break;
            out.push_back({best, percent});
            percent = percent * (100 - falloff) / 100;
            last = proto::ToFlat(*host_.Position(best));
        }
        return out;
    }
    // Around the caster (1, 5) or the selected (2): within the range and both bodies, by id.
    const proto::WorldPosition centre = shape == 2 ? *from : *me;
    const proto::FlatPosition c = proto::ToFlat(centre);
    for (const std::uint32_t id : candidates(centre, range + 2.0 * double(kPlayerRadius) + 100.0)) {
        if (full()) break;
        if (taken(id) || !admits(id)) continue;
        const auto where = host_.Position(id);
        if (!where) continue;
        const proto::FlatPosition p = proto::ToFlat(*where);
        if (std::hypot(p.x - c.x, p.z - c.z) <= range + 2.0 * double(kPlayerRadius)) out.push_back({id, 100});
    }
    return out;
}

void Skills::Recover(SkillCaster& caster, std::uint32_t hp, std::uint32_t mp) {
    // HP and MP back, to the most, each cut by its taker's recovery cut (panic's 0x8F, combustion's 0x90).
    if (caster.dead || (hp == 0 && mp == 0)) return;
    hp = game::RecoveryCut(caster.state.states, hp, true);
    mp = game::RecoveryCut(caster.state.states, mp, false);
    caster.hp = std::uint32_t(std::min<std::uint64_t>(std::uint64_t(caster.hp) + hp, caster.stats.maxHp));
    caster.mp = std::uint32_t(std::min<std::uint64_t>(std::uint64_t(caster.mp) + mp, caster.stats.maxMp));
    host_.VitalsChanged(caster.entity);
}

Skills::Clock::time_point Skills::Stand(SkillCaster& caster, const assets::SkillRow& skill, Clock::time_point now) {
    // Only an action (Basic_Activity 2) stops its caster;
    // the imbues, potions and scrolls (1) are cast on the move.
    if (skill.activity == 2 && host_.Moving(caster.entity)) {
        if (const auto me = host_.Position(caster.entity)) host_.MoveTo(caster.entity, *me);
    }
    const int lead = std::max(0, skill.preparingMs) + std::max(0, skill.castingMs);
    const Clock::time_point impact = lead > 0 ? OnTick(caster.state, now, now + std::chrono::milliseconds(lead)) : now;
    caster.state.castUntil = std::max(caster.state.castUntil, impact);
    Commanded(caster, skill, now);
    return impact;
}

void Skills::Commanded(SkillCaster& caster, const assets::SkillRow& skill, Clock::time_point now) {
    // An action that goes off at once (a buff, a heal, a cure, a cast about the caster):
    // it holds the action list's front its preparing, casting and duration; the fight it replaced ends unless it hands
    // on (Action_AutoAttackType 1). An imbue (activity 1) never enters the list.
    if (skill.activity == 1) return;
    SkillState& s = caster.state;
    if (skill.autoAttack != 1 && !game::TraitsOf(skill).attack) {
        s.attackTarget = 0;
        s.queued = nullptr;
    }
    const int hold = std::max(0, skill.preparingMs) + std::max(0, skill.castingMs) + std::max(0, skill.durationMs);
    s.busyUntil = std::max(s.busyUntil, now + std::chrono::milliseconds(hold));
    TellHeld(caster, now);
}

Skills::Clock::time_point Skills::OnTick(SkillState& state, Clock::time_point now, Clock::time_point at) {
    if (!state.skillTicking) {
        state.skillEpoch = now;
        state.skillTicking = true;
    }
    if (at <= state.skillEpoch) return state.skillEpoch;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(at - state.skillEpoch).count();
    return state.skillEpoch + std::chrono::milliseconds((ms + 299) / 300 * 300);
}

void Skills::Companions(SkillCaster& caster, Clock::time_point now) {
    // Each companion buff: its holder's last attack's target while it lives, struck once a period when
    // the holder attacked within one (it fights only beside its holder).
    SkillState& s = caster.state;
    for (SkillState::Buff& b : s.buffs) {
        if (b.companionMs <= 0 || now >= b.until) continue;
        if (s.companionTarget != 0 && host_.Monster(s.companionTarget) == nullptr) s.companionTarget = 0;
        const auto period = std::chrono::milliseconds(b.companionMs);
        if (s.companionTarget == 0 || now - b.lastStrike < period || now - s.lastAttack > period) continue;
        b.lastStrike = now;
        // A physical ranged strike at the mastery of the holder's weapon's base attack (ours: 100 %).
        game::AttackSpec attack;
        attack.skill = b.skill;
        attack.kind = 2 | 4;
        attack.power = 100;
        attack.flatMin = int(b.companionPower);
        attack.flatMax = int(b.companionMagic);
        if (const assets::SkillRow* basic = BasicAttack(caster.stats)) attack.mastery = AttackFor(caster.book, *basic).mastery;
        SkillState::PendingHit hit{now, s.companionTarget, attack, 0.0f, 100, nullptr};
        hit.companion = b.skill;
        hit.use = ++s.uses;
        s.pendingHits.push_back(hit);
    }
}

bool Skills::IsQuestTrap(const assets::SkillRow& skill) {
    return assets::FindSkillParam(skill, assets::SkillTag("trap")) && assets::FindSkillParam(skill, assets::SkillTag("qest"));
}

std::uint16_t Skills::SetTrap(SkillCaster& caster, const assets::SkillRow& skill, Clock::time_point now) {
    // The item's skill cast at once (its preparing and casting 0: SKILL_QNO_CA_GORIA_6_01_01, _EU_GENERAL_1_01_01,
    // _EU_IVY_2_01_01), its object at the user's feet: 'efr' 3 1 20 (a radius of 20), 'dura' 300000 (5 minutes), 'lnks' 0 300
    // (gone once its owner is 300 away). Refused, as the original's: a monster within 5 times its radius (0x3038), another
    // skill object within its radius (0x3037), its user's own trap still standing (0x3037: no second one).
    if (!IsQuestTrap(skill)) return 2;
    const auto me = host_.Position(caster.entity);
    if (!me) return 2;
    const assets::SkillParam area = assets::FindSkillParam(skill, assets::SkillTag("efr"), 3);
    const float radius = area ? float(std::max(1, area.Int(2))) : 20.0f;
    if (!host_.MonstersNear(*me, radius * 5.0f).empty()) return 0x3038;
    const proto::FlatPosition here = proto::ToFlat(*me);
    for (const SkillObject& o : objects_) {
        const proto::FlatPosition there = proto::ToFlat(o.at);
        if (std::hypot(there.x - here.x, there.z - here.z) < double(radius)) return 0x3037;
        if (o.owner == caster.entity && o.skill != nullptr && o.skill->group == skill.group) return 0x3037;
    }
    for (const PendingObject& p : pendingObjects_) {
        if (p.owner == caster.entity && p.skill != nullptr && p.skill->group == skill.group) return 0x3037;
    }
    host_.ToObservers(caster.entity, proto::EntitySwing{caster.entity, 0, skill.id});
    pendingObjects_.push_back({now, caster.entity, &skill});
    core::LogInfo("skills: {} sets {}", caster.entity, skill.codename);
    return 0;
}

void Skills::CastObject(SkillCaster& caster, const assets::SkillRow& skill, Clock::time_point now) {
    // Its mana and wait now, the swing; the object comes at the impact, once its preparing and casting
    // are over (at the skill's impact), at the caster's feet then.
    const auto me = host_.Position(caster.entity);
    if (!me) return;
    if (skill.activity == 2 && host_.Moving(caster.entity)) host_.MoveTo(caster.entity, *me);
    StartCooldown(caster, skill, now);
    host_.ToCaster(caster.entity, proto::SkillUseAnswer{skill.id, proto::SkillUseStatus::Ok});
    host_.ToObservers(caster.entity, proto::EntitySwing{caster.entity, 0, skill.id});
    const int lead = std::max(0, skill.preparingMs) + std::max(0, skill.castingMs);
    const Clock::time_point impact = lead > 0 ? OnTick(caster.state, now, now + std::chrono::milliseconds(lead)) : now;
    PayAtRelease(caster, skill, 0, impact, now);
    caster.state.castUntil = impact;
    Commanded(caster, skill, now);
    pendingObjects_.push_back({impact, caster.entity, &skill});
    core::LogInfo("skills: {} casts {} (an object in {} ms)", caster.entity, skill.codename, lead);
}

Skills::Stealth Skills::StealthOf(const SkillState& state) const {
    Stealth out;
    if (table_ == nullptr) return out;
    std::uint8_t revealMask = 0, revealLevel = 0;
    for (const SkillState::Buff& b : state.buffs) {
        const assets::SkillRow* s = table_->Find(b.skill);
        if (s == nullptr) continue;
        if (const assets::SkillParam hide = assets::FindSkillParam(*s, assets::SkillTag("hide"))) {
            out.mask = std::uint8_t(hide[0] & 0x07);
            out.level = std::uint8_t(std::min<std::uint32_t>(hide[1], 255));
            out.speed = std::max(0.0f, 1.0f - float(hide[2]) / 100.0f); // ours: the STSP passive's share not yet
        }
        if (const assets::SkillParam dtt = assets::FindSkillParam(*s, assets::SkillTag("dtt"))) {
            out.detectMask = std::uint8_t(out.detectMask | (dtt[0] & 0x07));
            out.detectLevel = std::max(out.detectLevel, std::uint8_t(std::min<std::uint32_t>(dtt[1], 255)));
        }
        if (const assets::SkillParam dttp = assets::FindSkillParam(*s, assets::SkillTag("dttp"))) {
            revealMask = std::uint8_t(revealMask | (dttp[0] & 0x07));
            revealLevel = std::max(revealLevel, std::uint8_t(std::min<std::uint32_t>(dttp[1], 255)));
        }
    }
    // An attached detector of its hide reveals it to all.
    if (out.mask != 0 && (revealMask & out.mask) != 0 && revealLevel >= out.level) out.mask = 0;
    return out;
}

bool Skills::EndOnEvent(SkillCaster& holder, std::uint32_t event) {
    if (table_ == nullptr) return false;
    bool ended = false;
    std::vector<SkillState::Buff>& buffs = holder.state.buffs;
    for (std::size_t i = buffs.size(); i-- > 0;) {
        const assets::SkillRow* s = table_->Find(buffs[i].skill);
        const assets::SkillParam skc = s != nullptr ? assets::FindSkillParam(*s, assets::SkillTag("skc")) : assets::SkillParam{};
        if (!skc || (skc[1] & event) == 0) continue;
        EndBuff(holder, i);
        ended = true;
    }
    return ended;
}

bool Skills::EndOnHit(SkillCaster& holder, int lanes) {
    if (table_ == nullptr) return false;
    bool ended = false;
    std::vector<SkillState::Buff>& buffs = holder.state.buffs;
    for (std::size_t i = buffs.size(); i-- > 0;) {
        const assets::SkillRow* s = table_->Find(buffs[i].skill);
        const assets::SkillParam skc = s != nullptr ? assets::FindSkillParam(*s, assets::SkillTag("skc")) : assets::SkillParam{};
        if (!skc || skc[0] == 0) continue;
        // Word 0 widened (4/8/12 also 1|2, 1/2/3 also 4|8), met by the hit's lanes.
        std::uint32_t word = skc[0];
        if ((word & 0x0C) != 0) word |= 0x03;
        if ((word & 0x03) != 0) word |= 0x0C;
        if ((word & std::uint32_t(lanes)) == 0) continue;
        const int keep = std::clamp(skc.Int(2), 0, 100); // ours: the MUCR / DSER passives' share not yet
        if (keep > 0 && std::uniform_int_distribution<int>(0, 99)(random_) < keep) continue;
        EndBuff(holder, i);
        ended = true;
    }
    return ended;
}

Skills::Guarded Skills::Guard(SkillCaster& holder, std::uint32_t attacker, std::uint32_t damage, bool magical, double distance, Clock::time_point now) {
    Guarded out;
    out.damage = damage;
    if (table_ == nullptr || damage == 0) return out;
    // The reflection: the passive's when it has one (activity 0), else a buff's.
    const assets::SkillRow* reflect = nullptr;
    for (const game::Passives::Entry& e : holder.state.passives.entries) {
        const assets::SkillRow* s = e.active ? table_->Find(e.skill) : nullptr;
        if (s != nullptr && assets::FindSkillParam(*s, assets::SkillTag("dmgr"))) reflect = s;
    }
    const assets::SkillRow* shield = nullptr;
    for (const SkillState::Buff& b : holder.state.buffs) {
        const assets::SkillRow* s = table_->Find(b.skill);
        if (s == nullptr) continue;
        if (reflect == nullptr && assets::FindSkillParam(*s, assets::SkillTag("dmgr"))) reflect = s;
        if (assets::FindSkillParam(*s, assets::SkillTag("dgmp"))) shield = s;
    }
    // dmgr [chance, physical %, magical %, range]: the attacker closer than the range, the holder's chance
    // (its history's key 0x46); the parts' shares, each truncated. Ours: a monster's
    // hit is all of one kind, its magical flag's.
    if (reflect != nullptr) {
        const assets::SkillParam r = assets::FindSkillParam(*reflect, assets::SkillTag("dmgr"));
        if (distance < double(r[3]) && holder.state.rolls.Roll(r.Int(0), char(0x46), 0, random_)) {
            const std::uint32_t percent = magical ? r[2] : r[1];
            out.returned = std::uint32_t(std::trunc(double(damage) * double(percent) / 100.0));
            if (out.returned > 0) out.reflectSkill = reflect->id;
        }
    }
    // A guard ('lkdr' lanes, percent, most hits): the part of the hit's kind in its
    // lanes (4 physical, 8 magical) at its percent (100: the whole hit) moves to the guard; at its most
    // hits the link ends.
    for (const SkillState::Buff& b : std::vector<SkillState::Buff>(holder.state.buffs)) {
        const assets::SkillRow* row = b.linkOwner != 0 ? table_->Find(b.skill) : nullptr;
        const assets::SkillParam p = row != nullptr ? assets::FindSkillParam(*row, assets::SkillTag("lkdr")) : assets::SkillParam{};
        if (!p || out.damage == 0) continue;
        std::uint32_t moved = 0;
        if (p[1] >= 100) {
            moved = out.damage;
        } else if ((p[0] & (magical ? 8u : 4u)) != 0) {
            moved = std::uint32_t(std::trunc(double(out.damage) * double(p[1]) / 100.0));
        }
        moved = std::min(moved, out.damage);
        if (moved == 0) continue;
        out.damage -= moved;
        out.moved.push_back({b.linkOwner, moved, row->id});
        if (p[2] == 0) continue;
        if (std::optional<SkillCaster> owner = host_.Player(b.linkOwner)) {
            std::vector<SkillState::Link>& links = owner->state.links;
            for (std::size_t i = 0; i < links.size(); ++i) {
                if (links[i].target != holder.entity || links[i].copy != b.token) continue;
                if (++links[i].hits >= int(p[2])) EndLink(*owner, i);
                break;
            }
        }
    }
    // A divide ('lkdd' percent): kept its rest, the share spread evenly over the party
    // members alive within 1000 of it (itself not among them), the remainder lost; none near, all kept.
    for (const auto& [owner, row] : CopiesOf(holder.state, "lkdd")) {
        const std::uint32_t percent = std::min<std::uint32_t>(assets::FindSkillParam(*row, assets::SkillTag("lkdd"))[0], 100);
        const auto here = host_.Position(holder.entity);
        if (!here || out.damage == 0) continue;
        std::vector<std::uint32_t> near;
        for (const std::uint32_t m : host_.PartyOf(holder.entity)) {
            if (m == holder.entity) continue;
            std::optional<SkillCaster> member = host_.Player(m);
            const auto there = host_.Position(m);
            if (!member || member->dead || !there) continue;
            const proto::FlatPosition a = proto::ToFlat(*here), c = proto::ToFlat(*there);
            const double dy = double(here->y) - double(there->y);
            if (std::sqrt((a.x - c.x) * (a.x - c.x) + (a.z - c.z) * (a.z - c.z) + dy * dy) <= 1000.0) near.push_back(m);
        }
        if (near.empty()) continue;
        const std::uint32_t kept = std::uint32_t(std::trunc(double(out.damage) * double(100 - percent) / 100.0));
        const std::uint32_t each = (out.damage - kept) / std::uint32_t(near.size());
        out.damage = kept;
        if (each == 0) continue;
        for (const std::uint32_t m : near) out.moved.push_back({m, each, row->id});
    }
    // dgmp [percent]: moved = the hit's share, its MP cost 1.5 a point; short of MP, the part it cannot pay
    // goes back on the hit and all its MP is taken.
    if (shield != nullptr) {
        const assets::SkillParam p = assets::FindSkillParam(*shield, assets::SkillTag("dgmp"));
        std::uint32_t moved = std::uint32_t(std::trunc(double(damage) * double(std::min<std::uint32_t>(p[0], 100)) / 100.0));
        const std::uint32_t cost = std::uint32_t(std::trunc(double(moved) * 1.5));
        if (cost > holder.mp) {
            const std::uint32_t back = std::uint32_t(std::trunc(double(cost - holder.mp) / 1.5));
            moved = moved > back ? moved - back : 0u;
            out.mpTaken = holder.mp;
        } else {
            out.mpTaken = cost;
        }
        holder.mp -= out.mpTaken;
        out.damage -= std::min(out.damage, moved);
    }
    // A stun link ('abnb' range): an attacker within its range takes the skill's
    // states as from the link's owner (ours: at once; the original on the attacker's next tick).
    for (const auto& [owner, row] : CopiesOf(holder.state, "abnb")) {
        if (distance > double(assets::FindSkillParam(*row, assets::SkillTag("abnb"))[0])) continue;
        if (std::optional<SkillCaster> o = host_.Player(owner); o && !o->dead && host_.Monster(attacker) != nullptr) Afflict(*o, attacker, *row, now);
    }
    return out;
}

proto::SkillUseStatus Skills::AdmitLink(const SkillState& owner, const assets::SkillRow& skill, std::uint32_t target) const {
    const assets::SkillParam lnks = assets::FindSkillParam(skill, assets::SkillTag("lnks"));
    if (!lnks || table_ == nullptr) return proto::SkillUseStatus::Ok;
    const bool shared = static_cast<bool>(assets::FindSkillParam(skill, assets::SkillTag("lks2")));
    const int kind = lnks.Int(0), most = lnks.Int(2);
    int count = 0;
    for (const SkillState::Link& l : owner.links) {
        const assets::SkillRow* other = table_->Find(l.skill);
        const assets::SkillParam theirs = other != nullptr ? assets::FindSkillParam(*other, assets::SkillTag("lnks")) : assets::SkillParam{};
        if (!theirs || theirs.Int(0) != kind) continue;
        const bool same = l.skill == skill.id || other->group == skill.group;
        if (same && l.target == target) return proto::SkillUseStatus::Overlap;
        if (!shared && kind != 0 && !same) return proto::SkillUseStatus::Overlap;
        if (!shared || l.target == target) ++count;
    }
    return most > 0 && count >= most ? proto::SkillUseStatus::Links : proto::SkillUseStatus::Ok;
}

std::size_t Skills::MonsterCopies(std::uint32_t monster) const {
    const auto it = monsterCopies_.find(monster);
    return it != monsterCopies_.end() ? it->second.size() : 0u;
}

void Skills::StartLink(SkillCaster& owner, const assets::SkillRow& skill, std::uint32_t target, std::uint32_t copy, Clock::time_point from,
                       Clock::time_point until) {
    SkillState::Link l;
    l.skill = skill.id;
    l.target = target;
    l.copy = copy;
    l.until = until;
    l.token = nextBuffToken_++;
    if (const assets::SkillParam pulse = assets::FindSkillParam(skill, assets::SkillTag("puls")); pulse && pulse.Int(0) > 0 && copy == 0) {
        l.pulseMs = pulse.Int(0);
        l.nextPulse = from; // the first strike on its first tick (as the auras'; inferred in 33 4.7)
    }
    // A mark (Tag Point, 'hntp'): its owner is told where its target is, from the start.
    l.mark = static_cast<bool>(assets::FindSkillParam(skill, assets::SkillTag("hntp")));
    l.nextReport = from;
    owner.state.links.push_back(l);
    // Its owner's side (the original's B0BE to the caster only): its board shows the link.
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - from).count();
    host_.ToCaster(owner.entity, proto::LinkAdded{l.token, skill.id, target, std::uint32_t(std::max<long long>(0, left))});
    if (copy == 0) {
        monsterCopies_[target].push_back({owner.entity, skill.id});
        TellCopies(target);
    }
}

void Skills::Links(SkillCaster& caster, Clock::time_point now) {
    if (table_ == nullptr) return;
    SkillState& s = caster.state;
    // The copies it holds whose owner's link is gone (its owner gone, dead, or its link ended): gone
    // with it.
    bool ended = false;
    for (std::size_t i = s.buffs.size(); i-- > 0;) {
        const SkillState::Buff& b = s.buffs[i];
        if (b.linkOwner == 0) continue;
        std::optional<SkillCaster> owner = host_.Player(b.linkOwner);
        if (owner && !owner->dead && std::any_of(owner->state.links.begin(), owner->state.links.end(), [&](const SkillState::Link& l) {
                return l.target == caster.entity && l.copy == b.token;
            })) {
            continue;
        }
        EndBuff(caster, i);
        ended = true;
    }
    if (ended) host_.StatsChanged(caster.entity);
    // Its own: ended by their time, their target gone (or its copy), the target
    // beyond their break range (3D); a DOT strikes each period.
    const auto me = host_.Position(caster.entity);
    for (std::size_t i = s.links.size(); i-- > 0;) {
        SkillState::Link& l = s.links[i];
        const assets::SkillRow* row = table_->Find(l.skill);
        const auto there = host_.Position(l.target);
        bool ends = caster.dead || row == nullptr || now >= l.until || !me || !there;
        if (!ends && l.copy != 0) {
            std::optional<SkillCaster> other = host_.Player(l.target);
            ends = !other || other->dead ||
                   std::none_of(other->state.buffs.begin(), other->state.buffs.end(), [&](const SkillState::Buff& b) { return b.token == l.copy; });
        } else if (!ends) {
            ends = host_.Monster(l.target) == nullptr;
        }
        if (!ends) {
            const int range = assets::FindSkillParam(*row, assets::SkillTag("lnks")).Int(1);
            const proto::FlatPosition a = proto::ToFlat(*me), b = proto::ToFlat(*there);
            const double dy = double(me->y) - double(there->y);
            ends = range > 0 && std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z) + dy * dy) > double(range);
        }
        if (ends) {
            EndLink(caster, i);
            continue;
        }
        if (l.mark && now >= l.nextReport) {
            l.nextReport = std::max(l.nextReport + std::chrono::seconds(1), now);
            host_.ToCaster(caster.entity, proto::LinkPosition{l.token, *there, host_.Facing(l.target)});
        }
        if (l.pulseMs > 0 && now >= l.nextPulse) {
            l.nextPulse += std::chrono::milliseconds(l.pulseMs);
            // The skill's 'att' and states on its target alone, shown as a strike of its own (B0BC form 2).
            SkillState::PendingHit hit{now, l.target, AttackFor(caster.book, *row, s.passives.modifiers), std::numeric_limits<float>::max(), 100, row};
            hit.returned = true;
            hit.use = ++s.uses;
            s.pendingHits.push_back(hit);
        }
    }
}

void Skills::EndLink(SkillCaster& owner, std::size_t index) {
    std::vector<SkillState::Link>& links = owner.state.links;
    if (index >= links.size()) return;
    const SkillState::Link l = links[index];
    links.erase(links.begin() + std::ptrdiff_t(index));
    host_.ToCaster(owner.entity, proto::BuffRemoved{l.token}); // its owner's board
    if (l.copy != 0) {
        if (std::optional<SkillCaster> other = host_.Player(l.target)) {
            std::vector<SkillState::Buff>& buffs = other->state.buffs;
            for (std::size_t i = 0; i < buffs.size(); ++i) {
                if (buffs[i].token != l.copy) continue;
                EndBuff(*other, i);
                host_.StatsChanged(l.target);
                break;
            }
        }
        return;
    }
    if (const auto it = monsterCopies_.find(l.target); it != monsterCopies_.end()) {
        auto& copies = it->second;
        if (const auto c = std::find(copies.begin(), copies.end(), std::make_pair(owner.entity, l.skill)); c != copies.end()) copies.erase(c);
        TellCopies(l.target);
    }
}

std::vector<std::pair<std::uint32_t, const assets::SkillRow*>> Skills::CopiesOf(const SkillState& holder, const char* tag) const {
    std::vector<std::pair<std::uint32_t, const assets::SkillRow*>> out;
    if (table_ == nullptr) return out;
    for (const SkillState::Buff& b : holder.buffs) {
        const assets::SkillRow* row = b.linkOwner != 0 ? table_->Find(b.skill) : nullptr;
        if (row != nullptr && assets::FindSkillParam(*row, assets::SkillTag(tag))) out.push_back({b.linkOwner, row});
    }
    return out;
}

void Skills::Afflict(SkillCaster& owner, std::uint32_t monster, const assets::SkillRow& row, Clock::time_point now) {
    const std::optional<Monsters::Target> target = host_.MonsterTarget(monster);
    const std::vector<game::StateGift> gifts = game::StateGiftsOf(row);
    if (!target || gifts.empty()) return;
    Monsters::Affliction a;
    a.states = game::RollStates(gifts, target->defence, target->susceptibility, owner.state.rolls, random_, owner.entity, row.id, 0, now);
    if (a.states.empty()) return;
    a.threatGiven = true; // its states alone: no threat (ours)
    if (const auto at = host_.Position(owner.entity)) a.from = *at;
    const Monsters::Damaged d = host_.HitMonster(monster, owner.entity, 0, now, &a);
    if (d.statesChanged) host_.ToObservers(monster, proto::EntityStates{monster, host_.MonsterStates(monster, now)});
}

void Skills::TellCopies(std::uint32_t monster) {
    const auto it = monsterCopies_.find(monster);
    std::vector<std::uint32_t> skills;
    if (it != monsterCopies_.end()) {
        for (const auto& c : it->second) skills.push_back(c.second);
        if (it->second.empty()) monsterCopies_.erase(it);
    }
    host_.MonsterBuffs(monster, std::move(skills));
}

Skills::Protection Skills::ProtectionOf(const SkillState& state) const {
    Protection p;
    if (table_ == nullptr) return p;
    for (const SkillState::Buff& b : state.buffs) {
        const assets::SkillRow* s = table_->Find(b.skill);
        const assets::SkillParam pola = s != nullptr ? assets::FindSkillParam(*s, assets::SkillTag("pola")) : assets::SkillParam{};
        if (!pola) continue;
        p.grades = std::uint8_t(p.grades | (pola[0] & 0x0F));
        p.level = std::max(p.level, pola.Int(1));
    }
    return p;
}

void Skills::EndObject(std::size_t index) {
    // Its copies end on their holders, then it is gone.
    if (index >= objects_.size()) return;
    const SkillObject object = objects_[index];
    objects_.erase(objects_.begin() + std::ptrdiff_t(index));
    for (const SkillObject::Member& m : object.members) {
        std::optional<SkillCaster> holder = host_.Player(m.entity);
        if (!holder) continue;
        const auto& buffs = holder->state.buffs;
        const auto it = std::find_if(buffs.begin(), buffs.end(), [&](const SkillState::Buff& b) { return b.token == m.token; });
        if (it == buffs.end()) continue;
        EndBuff(*holder, std::size_t(it - buffs.begin()));
        host_.StatsChanged(m.entity);
    }
    host_.RemoveObject(object.entity);
    core::LogInfo("skills: {}'s {} is gone", object.owner, object.skill != nullptr ? object.skill->codename : std::string("object"));
}

void Skills::ScanObject(SkillObject& o, SkillCaster& owner, Clock::time_point now) {
    const proto::FlatPosition c = proto::ToFlat(o.at);
    const auto within = [&](std::uint32_t entity) {
        const auto p = host_.Position(entity);
        if (!p) return false;
        const proto::FlatPosition q = proto::ToFlat(*p);
        return std::hypot(q.x - c.x, q.z - c.z) <= double(o.radius);
    };
    if (!o.quest.empty()) {
        // A quest's trap: the first monster its area takes
        // (within its radius and the monster's body) whose reference is one of its 'qest' three is its owner's quest event
        // 0x1D, then it goes. Ours takes every monster as unowned (a quest's own raised monsters belong to no other).
        for (const std::uint32_t id : host_.MonstersNear(o.at, o.radius + 50.0f)) {
            if (std::find(o.quest.begin(), o.quest.end(), host_.MonsterRefOf(id)) == o.quest.end()) continue;
            const game::CombatStats* them = host_.Monster(id);
            const auto p = host_.Position(id);
            if (them == nullptr || !p || host_.MonsterHp(id) == 0) continue;
            const proto::FlatPosition q = proto::ToFlat(*p);
            if (std::hypot(q.x - c.x, q.z - c.z) > double(o.radius) + double(them->bodyRadius)) continue;
            o.until = now; // gone after it caught (Objects ends it)
            host_.ToObservers(o.entity, proto::SkillObjectStruck{o.entity, o.skill->id, true});
            host_.QuestTrapped(o.owner, id);
            return;
        }
        return;
    }
    if (!o.attack) {
        // A buff object: those still in keep their copy, one out (or gone) loses it (B072 from it);
        // then the players its mask takes, up to its most (0: no limit), each a copy for the time it
        // has left.
        for (auto m = o.members.begin(); m != o.members.end();) {
            std::optional<SkillCaster> holder = host_.Player(m->entity);
            if (holder && !holder->dead && within(m->entity)) {
                ++m;
                continue;
            }
            if (holder) {
                const auto& buffs = holder->state.buffs;
                if (const auto it = std::find_if(buffs.begin(), buffs.end(), [&](const SkillState::Buff& b) { return b.token == m->token; });
                    it != buffs.end()) {
                    EndBuff(*holder, std::size_t(it - buffs.begin()));
                    host_.StatsChanged(m->entity);
                }
            }
            m = o.members.erase(m);
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(o.until - now).count();
        if (left <= 0) return;
        const std::vector<std::uint32_t> party = host_.PartyOf(o.owner);
        for (const std::uint32_t id : host_.PlayersNear(o.at, o.radius)) {
            if (o.most != 0 && o.members.size() >= o.most) break;
            if (std::any_of(o.members.begin(), o.members.end(), [&](const SkillObject::Member& m) { return m.entity == id; })) continue;
            const bool admitted = ((o.mask & 0x01) != 0 && id == o.owner) || (o.mask & 0x02) != 0 ||
                                  ((o.mask & 0x04) != 0 && std::find(party.begin(), party.end(), id) != party.end());
            if (!admitted) continue;
            std::optional<SkillCaster> holder = id == owner.entity ? std::optional<SkillCaster>(owner) : host_.Player(id);
            if (!holder || holder->dead) continue;
            const SkillState::Buff* copy = AddBuff(*holder, *o.skill, std::uint32_t(left), now);
            if (copy == nullptr) continue; // a buff it may not stand beside runs
            const std::uint32_t token = copy->token;
            for (SkillState::Buff& b : holder->state.buffs) b.own = b.own && b.token != token; // the object's, not its own
            o.members.push_back({id, token});
        }
        return;
    }
    // An attack object: its puls's period (between them, nothing), the monsters within its radius up to
    // its most, each with the area of kind 1 around it when the skill has one; a trap only the first. Its hits are its
    // owner's, struck from where it stands.
    if (o.pulseMs > 0) {
        if (now < o.nextPulse) return;
        o.nextPulse = now + std::chrono::milliseconds(o.pulseMs);
    }
    std::vector<Struck> struck;
    for (const std::uint32_t id : host_.MonstersNear(o.at, o.radius + 50.0f)) {
        if (o.most != 0 && struck.size() >= o.most) break;
        const game::CombatStats* them = host_.Monster(id);
        const auto p = host_.Position(id);
        if (them == nullptr || !p) continue;
        const proto::FlatPosition q = proto::ToFlat(*p);
        if (std::hypot(q.x - c.x, q.z - c.z) > double(o.radius) + double(them->bodyRadius)) continue;
        if (std::any_of(struck.begin(), struck.end(), [&](const Struck& s) { return s.target == id; })) continue;
        if (assets::FindSkillParam(*o.skill, assets::SkillTag("efr"), 1)) {
            for (const Struck& t : TargetsOf(owner, *o.skill, id, 1, &*p)) {
                if (std::none_of(struck.begin(), struck.end(), [&](const Struck& s) { return s.target == t.target; })) struck.push_back(t);
            }
        } else {
            struck.push_back({id, 100});
        }
        if (o.trap) break;
    }
    if (struck.empty()) return;
    host_.ToObservers(o.entity, proto::SkillObjectStruck{o.entity, o.skill->id, o.trap});
    const game::AttackSpec attack = AttackFor(owner.book, *o.skill, owner.state.passives.modifiers);
    for (const Struck& t : struck) {
        SkillState::PendingHit hit{now, t.target, attack, 0.0f, t.percent, o.skill};
        hit.fromObject = true;
        hit.returned = true;
        hit.origin = o.at;
        hit.use = ++owner.state.uses;
        owner.state.pendingHits.push_back(hit);
    }
    if (o.trap) o.until = now; // gone after it went off
}

void Skills::Objects(Clock::time_point now) {
    if (table_ == nullptr) return;
    // The DOTs' copies on monsters: gone with the monster, or with their owner's link (its owner gone).
    for (auto it = monsterCopies_.begin(); it != monsterCopies_.end();) {
        const std::uint32_t monster = it->first;
        std::vector<std::pair<std::uint32_t, std::uint32_t>>& copies = it->second;
        const bool alive = host_.Monster(monster) != nullptr;
        const std::size_t before = copies.size();
        std::erase_if(copies, [&](const std::pair<std::uint32_t, std::uint32_t>& c) {
            if (!alive) return true;
            std::optional<SkillCaster> owner = host_.Player(c.first);
            return !owner || std::none_of(owner->state.links.begin(), owner->state.links.end(), [&](const SkillState::Link& l) {
                       return l.target == monster && l.skill == c.second && l.copy == 0;
                   });
        });
        if (copies.size() != before) {
            std::vector<std::uint32_t> skills;
            for (const auto& c : copies) skills.push_back(c.second);
            host_.MonsterBuffs(monster, std::move(skills));
        }
        it = copies.empty() ? monsterCopies_.erase(it) : std::next(it);
    }
    // Those due: at their owner's feet, if it is still there and alive. A new one of the same skill
    // from the same owner takes the old one's place (ours: the original's rule for a second one is
    // not traced).
    for (auto it = pendingObjects_.begin(); it != pendingObjects_.end();) {
        if (now < it->at) {
            ++it;
            continue;
        }
        const PendingObject p = *it;
        it = pendingObjects_.erase(it);
        std::optional<SkillCaster> owner = host_.Player(p.owner);
        const auto where = host_.Position(p.owner);
        if (!owner || owner->dead || !where || p.skill == nullptr) continue;
        for (std::size_t i = objects_.size(); i-- > 0;) {
            if (objects_[i].owner == p.owner && objects_[i].skill != nullptr && objects_[i].skill->group == p.skill->group) EndObject(i);
        }
        const assets::SkillParam area = assets::FindSkillParam(*p.skill, assets::SkillTag("efr"), 3);
        SkillObject o;
        o.entity = host_.SpawnObject(p.owner, *p.skill, *where);
        if (o.entity == 0) continue;
        o.owner = p.owner;
        o.skill = p.skill;
        o.at = *where;
        o.until = now + std::chrono::milliseconds(p.skill->hasDuration && p.skill->buffDurationMs > 0 ? p.skill->buffDurationMs : 30000);
        o.nextScan = now;
        o.radius = float(area[2]);
        o.most = area[3];
        o.mask = area[5];
        o.attack = (o.mask & 0x08) != 0;
        o.trap = static_cast<bool>(assets::FindSkillParam(*p.skill, assets::SkillTag("trap")));
        // A quest's trap: the monsters its 'qest' names (its words after the first: up to three reference ids).
        if (const assets::SkillParam qest = assets::FindSkillParam(*p.skill, assets::SkillTag("qest")); qest && o.trap) {
            for (std::size_t i = 1; i <= 3; ++i)
                if (qest[i] != 0) o.quest.push_back(qest[i]);
        }
        if (const assets::SkillParam pulse = assets::FindSkillParam(*p.skill, assets::SkillTag("puls")); pulse && pulse.Int(0) > 0) {
            o.pulseMs = pulse.Int(0);
            o.nextPulse = now + std::chrono::milliseconds(o.pulseMs);
        }
        if (const assets::SkillParam lnks = assets::FindSkillParam(*p.skill, assets::SkillTag("lnks")); lnks && lnks.Int(1) > 0) o.link = float(lnks.Int(1));
        objects_.push_back(std::move(o));
        core::LogInfo("skills: {}'s {} stands (radius {}, mask {:#x})", p.owner, p.skill->codename, objects_.back().radius, objects_.back().mask);
    }
    // Each one's life, then its scan every 300 ms.
    for (std::size_t i = objects_.size(); i-- > 0;) {
        SkillObject& o = objects_[i];
        std::optional<SkillCaster> owner = host_.Player(o.owner);
        const auto where = host_.Position(o.owner);
        bool ends = !owner || owner->dead || !where || now >= o.until;
        if (!ends && o.link > 0.0f) {
            const proto::FlatPosition a = proto::ToFlat(*where), b = proto::ToFlat(o.at);
            ends = std::hypot(a.x - b.x, a.z - b.z) > double(o.link);
        }
        if (ends) {
            EndObject(i);
            continue;
        }
        if (now < o.nextScan) continue;
        o.nextScan = now + std::chrono::milliseconds(300);
        ScanObject(o, *owner, now);
        if (o.trap && now >= o.until) EndObject(i);
    }
}

void Skills::LandHits(SkillCaster& caster, Clock::time_point now) {
    std::vector<SkillState::PendingHit>& pending = caster.state.pendingHits;
    // A row's hits on a target land together: their threat runs on one total, the cut once; and the targets told it
    // took hold (B0BD token 0).
    std::vector<std::pair<std::pair<const assets::SkillRow*, std::uint32_t>, std::int64_t>> threatTotals;
    for (auto it = pending.begin(); it != pending.end();) {
        if (now < it->at) {
            ++it;
            continue;
        }
        const SkillState::PendingHit hit = *it;
        it = pending.erase(it);
        const game::CombatStats* them = host_.Monster(hit.target);
        const auto me = host_.Position(caster.entity), there = host_.Position(hit.target);
        if (caster.dead || them == nullptr || !me || !there) continue;
        if (host_.IsPlayer(hit.target) && !host_.Hostile(caster.entity, hit.target)) continue;
        const proto::FlatPosition a = proto::ToFlat(*me), b = proto::ToFlat(*there);
        if (!hit.fromObject && hit.companion == 0 && std::hypot(a.x - b.x, a.z - b.z) > hit.reach) continue;
        // The hit: the target's share of it, its HP (a life steal takes no more), its states (against
        // a state, 'atca') and whether it is down (a down attack's percent, 'da'), an imbue's force at
        // the skill's share of it.
        const std::uint32_t hpBefore = host_.MonsterHp(hit.target);
        const std::optional<Monsters::Target> target = host_.MonsterTarget(hit.target);
        game::HitRoll who{true, false, &caster.state.rolls, hit.percent, hpBefore};
        if (target) {
            who.defenderStates = target->states;
            who.defenderDown = target->down;
        }
        who.magicalOnly = hit.magicalOnly;
        // A player target's wall (an Ice Wall, a Fire Wall): it takes its parts of the hit first.
        const std::optional<game::WallSpec> wall = host_.IsPlayer(hit.target) ? host_.WallOf(hit.target) : std::nullopt;
        if (wall) who.wall = &*wall;
        // The imbue: the one running as the hit lands, else the one its swing began with
        // (over). It joins a hit whose 'att' takes a share of it (its
        // fifth word): its force at that share, and its states.
        const assets::SkillRow* imbue = Imbue(caster.state, now);
        if (imbue == nullptr) imbue = hit.imbue;
        const bool imbued = imbue != nullptr && hit.attack.formula && hit.attack.imbuePercent > 0;
        game::AttackSpec force;
        if (imbued) {
            force = AttackFor(caster.book, *imbue, caster.state.passives.modifiers);
            force.mastery = ImbueMastery(hit.attack.mastery, force.mastery);
        }
        // A row of states alone (no 'att', no damage of its own): no rolls, no damage, its states.
        const bool statusOnly = !hit.attack.formula && hit.attack.fixedDamage <= 0 && hit.attack.lifeSteal <= 0;
        // A companion's: its power the attack (summ's word in the place of the
        // holder's attack; ours: the rest of the formula as the holder's).
        game::CombatStats companion;
        if (hit.companion != 0) {
            companion = caster.stats;
            companion.phyAttackMin = companion.phyAttackMax = hit.attack.flatMin;
            companion.magAttackMin = companion.magAttackMax = hit.attack.flatMax;
        }
        game::AttackSpec spec = hit.attack;
        if (hit.companion != 0) spec.flatMin = spec.flatMax = 0;
        game::HitResult roll = statusOnly ? game::HitResult{}
                                          : game::RollHit(hit.companion != 0 ? companion : caster.stats, *them, spec, who, random_, imbued ? &force : nullptr);
        // What the target's wall took; covering the whole hit, the holder takes nothing of it (no damage, states, knock).
        if (roll.wallTaken > 0) host_.WallTook(hit.target, roll.wallTaken);
        if (roll.wallOnly) continue;
        // In a monster's shape (msch 1) its own hit: 5 x its level and up to 5 x its level more, never a critical (the
        // block rolled first, as any hit's).
        if (!statusOnly && hit.companion == 0 && caster.state.transform.mode == 1 && !roll.blocked && caster.stats.level > 0) {
            const std::uint32_t five = std::uint32_t(caster.stats.level) * 5u;
            roll.critical = false;
            roll.damage = five + std::uniform_int_distribution<std::uint32_t>(0, five - 1)(random_);
        }
        // A structure takes the weapon's own attack alone; only a player's hit counts.
        if (!statusOnly) {
            if (const std::optional<std::uint32_t> took = host_.StructureDamage(caster.entity, hit.target)) {
                roll = game::HitResult{};
                if (hit.companion == 0) roll.damage = *took;
            } else if (hit.companion == 0 && caster.stats.weaponKind == 16 && !roll.blocked) {
                roll.damage = std::min(roll.damage, 1u); // a fortress weapon on anything else
            }
        }
        // The equipment's wear: a landed hit's weapon, the target's armour or
        // its shield when it blocked; players only.
        if (!statusOnly && hit.companion == 0) {
            SkillState::UseWear& w = caster.state.wear[hit.use];
            if (!roll.blocked && roll.damage > 0 && host_.IsPlayer(caster.entity)) ++w.weaponHits;
            if (host_.IsPlayer(hit.target) && (roll.blocked || roll.damage > 0)) ++(roll.blocked ? w.targets[hit.target].second : w.targets[hit.target].first);
        }
        // Its states: the imbue's (when it joined), then the skill's own;
        // then a knock-down or knock-back. None on a blocked hit.
        Monsters::Affliction affliction;
        if (!roll.blocked && target) {
            affliction.magical = roll.magical;
            const auto roll_states = [&](const assets::SkillRow& row) {
                std::vector<game::StateGift> gifts = game::StateGiftsOf(row);
                if (gifts.empty()) return;
                // The poison's additions from the caster's passives (K6: the Rogue's).
                const game::SkillBoosts boost = game::BoostsOf(row, false, caster.state.passives.modifiers);
                for (game::StateGift& g : gifts) {
                    if (g.kind != game::StateKind::Poison) continue;
                    g.levelBonus = boost.poisonLevel;
                    g.valueBonus = boost.poisonDamage;
                }
                const int pulse = assets::FindSkillParam(row, assets::SkillTag("puls")).Int(0);
                std::vector<game::ActiveState> took =
                    game::RollStates(gifts, target->defence, target->susceptibility, caster.state.rolls, random_, caster.entity, row.id, pulse, now);
                affliction.states.insert(affliction.states.end(), took.begin(), took.end());
            };
            if (imbued) roll_states(*imbue);
            if (hit.row != nullptr) roll_states(*hit.row);
            // A buff's changes on the enemy for its time (the cancels' cuts).
            if (hit.row != nullptr && hit.row->hasDuration && !hit.attack.formula && game::TraitsOf(*hit.row).buff) {
                affliction.debuff = hit.row;
                affliction.debuffChanges = game::BuffChanges(*hit.row, BasisOf(*them));
                affliction.debuffMs = hit.row->buffDurationMs;
            }
            affliction.knock = game::RollKnock(hit.attack, target->defence.level, target->canFall, target->canPush, caster.state.rolls, random_);
            if (affliction.knock != game::Knock::None) {
                affliction.from = hit.fromObject ? hit.origin : *me;
                affliction.distance = float(hit.attack.knockBackDistance);
                affliction.knockMs = affliction.knock == game::Knock::Down
                                         ? game::KnockDownMs(target->recoverMs, hit.row != nullptr ? hit.row->durationMs : 0)
                                         : game::kKnockBackMs;
            }
        }
        // Its threat: the running total of its row on this target (its damage, the raise, the weapon's),
        // the increment this hit adds; the row's cut on its first hit. None on a blocked hit.
        if (hit.row != nullptr && hit.companion == 0) {
            const game::ThreatSpec spec = game::ThreatOf(*hit.row);
            const auto key = std::make_pair(hit.row, hit.target);
            auto total = std::find_if(threatTotals.begin(), threatTotals.end(), [&](const auto& t) { return t.first == key; });
            const bool first = total == threatTotals.end();
            if (first) total = threatTotals.insert(threatTotals.end(), {key, 0});
            const std::int64_t before = total->second;
            const std::int64_t after = roll.blocked ? before : game::RaiseThreat(before, roll.damage, spec, caster.stats);
            total->second = after;
            affliction.threatGiven = true;
            affliction.threat = after - before;
            if (first && spec.Any()) {
                affliction.threat -= spec.cutFlat + game::WeaponThreat(caster.stats, spec.physicalCutWord, false) +
                                     game::WeaponThreat(caster.stats, spec.magicalCutWord, true);
                affliction.threatPercent = -spec.cutPercent;
                if (spec.raise || spec.cut) host_.ToObservers(hit.target, proto::SkillTook{hit.target, caster.entity, hit.row->id});
            }
        }
        // A Guard Aggro on the hitter takes its share of the threat, told first from the guard as a
        // threat alone (lkag).
        if (affliction.threatGiven && affliction.threat > 0) {
            const std::int64_t whole = affliction.threat;
            for (const auto& [guard, link] : CopiesOf(caster.state, "lkag")) {
                const std::int64_t share = std::int64_t(std::trunc(double(whole) * double(assets::FindSkillParam(*link, assets::SkillTag("lkag"))[0]) / 100.0));
                if (share <= 0) continue;
                affliction.threat -= share;
                host_.AddThreat(hit.target, guard, share);
            }
        }
        const Monsters::Damaged d = host_.HitMonster(hit.target, caster.entity, roll.blocked ? 0u : roll.damage, now, &affliction);
        if (!d.hit) continue;
        // Its damage feeds the links on the hitter that share it (lkdh: the Bard's MP steal):
        // each copy's owner recovers its percents of an equal part, each at most its cap.
        if (!roll.blocked && roll.damage > 1) {
            const auto copies = CopiesOf(caster.state, "lkdh");
            const double per = copies.empty() ? 0.0 : double(roll.damage / std::uint32_t(copies.size()));
            for (const auto& [owner, link] : copies) {
                const assets::SkillParam p = assets::FindSkillParam(*link, assets::SkillTag("lkdh"));
                std::optional<SkillCaster> o = host_.Player(owner);
                if (!o || o->dead) continue;
                const auto part = [&](std::uint32_t percent) { return std::min<std::uint32_t>(std::uint32_t(std::trunc(per * double(percent) / 100.0)), p[2]); };
                Recover(*o, part(p[0]), part(p[1]));
            }
        }
        if (statusOnly) {
            // Told as a hit of no damage with the original's flag 0x10 (its client's "resist" mark).
            host_.ToObservers(hit.target, proto::EntityDamaged{hit.target, caster.entity, 0, d.hp, proto::kDamageResist, 0, hit.strike});
            if (d.statesChanged) host_.ToObservers(hit.target, proto::EntityStates{hit.target, host_.MonsterStates(hit.target, now)});
            continue;
        }
        // What the caster recovers from it: a life steal's take as HP, a
        // drain's shares of the damage as HP and MP; no more than the target lost.
        if (!roll.blocked && (hit.attack.lifeSteal > 0 || hit.attack.drainHp > 0 || hit.attack.drainMp > 0)) {
            const std::uint32_t lost = hpBefore > d.hp ? hpBefore - d.hp : 0u;
            const std::uint32_t taken = std::min(roll.damage, lost);
            const auto share = [&](int percent) { return std::uint32_t(std::trunc(double(roll.damage) * double(percent) / 100.0)); };
            const std::uint32_t hp = (hit.attack.lifeSteal > 0 ? taken : 0u) + std::min(share(hit.attack.drainHp), taken);
            Recover(caster, hp, std::min(share(hit.attack.drainMp), taken));
        }
        std::uint8_t flags = roll.critical ? proto::kDamageCritical : 0;
        if (roll.blocked) flags |= proto::kDamageBlocked;
        if (d.killed) flags |= proto::kDamageKilled;
        if (hit.companion != 0) flags |= proto::kDamageCompanion;
        const bool returned = hit.returned && !imbued && hit.row != nullptr;
        if (returned) flags |= proto::kDamageReturn;
        host_.ToObservers(hit.target, proto::EntityDamaged{hit.target, caster.entity, roll.damage, d.hp, flags,
                                                           hit.companion != 0 ? hit.companion : returned ? hit.row->id : imbued ? imbue->id : 0u,
                                                           hit.strike});
        if (d.knock != game::Knock::None) {
            host_.ToObservers(hit.target, proto::EntityKnocked{hit.target, caster.entity, proto::KnockKind(d.knock), d.knockTo, d.knockMs});
        }
        if (d.statesChanged) host_.ToObservers(hit.target, proto::EntityStates{hit.target, host_.MonsterStates(hit.target, now)});
        if (!d.killed) continue;
        host_.Killed(hit.target, d);
        if (caster.state.attackTarget == hit.target) caster.state.attackTarget = 0;
        pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const SkillState::PendingHit& p) { return p.target == hit.target; }), pending.end());
        it = pending.begin();
    }
    // Each finished use's wear: the caster's weapon one roll, each player target's armour
    // pass and shield one roll each, their chance 2 % x the hits counted.
    for (auto w = caster.state.wear.begin(); w != caster.state.wear.end();) {
        const std::uint32_t use = w->first;
        if (std::any_of(pending.begin(), pending.end(), [&](const SkillState::PendingHit& p) { return p.use == use; })) {
            ++w;
            continue;
        }
        if (w->second.weaponHits > 0) host_.EquipmentWorn(caster.entity, 0, w->second.weaponHits);
        for (const auto& [player, counts] : w->second.targets) {
            if (counts.first > 0) host_.EquipmentWorn(player, 1, counts.first);
            if (counts.second > 0) host_.EquipmentWorn(player, 2, counts.second);
        }
        w = caster.state.wear.erase(w);
    }
}

} // namespace sr::server
