#include "sr/game/SkillBench.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "sr/assets/GameData.h"
#include "sr/game/Items.h"
#include "sr/game/Protocol.h"
#include "sr/game/Skills.h"
#include "sr/game/States.h"
#include "sr/game/WorldProtocol.h"

namespace sr::game {

namespace {

// A weapon of a kind (TID4) for a race (0 China, 1 Europe): the one in service with a model and the
// lowest level asked, so the bench holds what a player first holds.
const assets::ItemRow* WeaponOf(const assets::ItemTable& items, int kind, int race) {
    const assets::ItemRow* best = nullptr;
    int bestLevel = 1 << 30;
    for (const auto& [code, row] : items.rows()) {
        if (!row.inService || row.modelPath.empty() || row.Rare()) continue;
        if (EquipPlaceFor(row, false, false) != slot::kWeapon || row.typeId[3] != kind) continue;
        if (row.country != race && row.country != 3) continue;
        int level = 0;
        for (std::size_t i = 0; i < row.requirementTypes.size(); ++i) {
            if (row.requirementTypes[i] == 1) level = std::max(level, row.requirementLevels[i]);
        }
        if (level < bestLevel || (level == bestLevel && best != nullptr && row.codename < best->codename)) {
            best = &row;
            bestLevel = level;
        }
    }
    return best;
}

template <typename T>
void Put(WorldRecording& r, double at, const T& message) {
    r.messages.push_back({at, T::kId, proto::Encode(message)});
}

// The basic attack of a weapon kind (the server's list, server/game Skills.cpp), else the bare hand's.
const assets::SkillRow* BasicAttackOf(const assets::SkillTable& skills, int weaponKind) {
    const assets::SkillRow* bare = nullptr;
    for (const char* code : {"SKILL_PUNCH_01", "SKILL_CH_SWORD_BASE_01", "SKILL_CH_SPEAR_BASE_01", "SKILL_CH_BOW_BASE_01", "SKILL_EU_SWORD_BASE_01",
                             "SKILL_EU_TSWORD_BASE_01", "SKILL_EU_AXE_BASE_01", "SKILL_EU_CROSSBOW_BASE_01", "SKILL_EU_DAGGER_BASE_01",
                             "SKILL_EU_STAFF_BASE_01", "SKILL_EU_WAND_WARLOCK_BASE_01", "SKILL_EU_HARP_BASE_01", "SKILL_EU_WAND_CLERIC_BASE_01"}) {
        const assets::SkillRow* row = skills.Find(std::string_view(code));
        if (row == nullptr) continue;
        if (bare == nullptr) bare = row;
        if (row->weaponKinds[0] == weaponKind || row->weaponKinds[1] == weaponKind) return row;
    }
    return bare;
}

} // namespace

bool MakeSkillBench(const assets::SkillTable& skills, const assets::ItemTable& items, const assets::CharacterTable& characters,
                    std::string_view filter, SkillBench& out, std::string& error, const SkillBenchOptions& options) {
    out = {};
    const assets::ObjectRow* chinese = characters.Find(std::string_view("CHAR_CH_MAN_ADVENTURER"));
    const assets::ObjectRow* european = characters.Find(std::string_view("CHAR_EU_MAN_NOBLE"));
    const assets::ObjectRow* monster = nullptr;
    for (const char* code : {"MOB_CH_MANGNYANG", "MOB_CH_TIGER", "MOB_EU_KOBOLD"}) {
        if ((monster = characters.Find(std::string_view(code))) != nullptr) break;
    }
    if (chinese == nullptr || european == nullptr || monster == nullptr) {
        error = "the bench's bodies or its monster are not in characterdata";
        return false;
    }
    // Outside Jangan's west gate: the casters side by side, the monster 40 in front.
    const proto::WorldPosition here{std::uint16_t(98 << 8 | 168), 960.0f, 50.0f, 460.0f};
    const auto at = [&](float dx, float dz) { return proto::WorldPosition{here.region, here.x + dx, here.y, here.z + dz}; };
    proto::WorldEntered entered;
    entered.entity = SkillBench::kChinese;
    entered.character.model = chinese->refObjId;
    entered.character.name = "Bench";
    entered.character.level = 110;
    entered.character.hp = entered.maxHp = 50000;
    entered.character.mp = entered.maxMp = 50000;
    entered.position = at(-12.0f, 0.0f);
    entered.walkSpeed = 16.0f;
    entered.runSpeed = 50.0f;
    entered.tickRate = 30;
    Put(out.recording, 0.0, entered);
    proto::EntityAppeared eu;
    eu.entity = SkillBench::kEuropean;
    eu.character.model = european->refObjId;
    eu.character.name = "BenchEU";
    eu.character.level = 110;
    eu.character.hp = 50000;
    eu.position = at(12.0f, 0.0f);
    Put(out.recording, 0.0, eu);
    proto::EntityAppeared target;
    target.entity = SkillBench::kTarget;
    target.character.model = monster->refObjId;
    target.character.hp = 10'000'000;
    target.position = at(0.0f, 40.0f);
    target.yaw = 3.14159f;
    target.kind = proto::EntityKind::Monster;
    Put(out.recording, 0.0, target);

    const bool all = filter.empty() || filter == "all";
    std::map<std::uint32_t, std::uint32_t> held; // caster -> the weapon in its hand
    std::map<std::uint32_t, int> heldKind;        // caster -> its kind (TID4; 0: a bare hand)
    std::map<std::uint32_t, float> heldRange;     // caster -> its weapon's range (itemdata col 94, tenths)
    std::uint32_t token = 1;                      // the buffs put on the player's board
    // Where each caster stands (offsets from `here`): from its home spot it walks to each skill's
    // reach of the monster before casting it, so a flight is seen at its length (K10).
    std::map<std::uint32_t, std::pair<float, float>> stand{{SkillBench::kChinese, {-12.0f, 0.0f}}, {SkillBench::kEuropean, {12.0f, 0.0f}}};
    double t = 3.0; // the world in, the casters standing
    int taken = 0;  // the skills the filter took (its part's every parts-th)
    for (const assets::MasteryRow& m : skills.masteries()) {
        const bool masteryWanted = !all && m.nameKey == filter;
        for (const assets::SkillRow* s : skills.Board(m.id)) {
            if (!s->inService || s->activity == 0) continue;
            if (!all && !masteryWanted && s->codename.find(filter) == std::string::npos) continue;
            if (const int n = taken++; options.parts > 1 && n % options.parts != options.part) continue;
            const std::uint32_t caster = s->race == 1 ? SkillBench::kEuropean : SkillBench::kChinese;
            // Its weapon in hand (a skill that asks for none keeps what is held, or takes the asked weapon of its race).
            std::uint32_t weapon = 0;
            const assets::ItemRow* anyWeapon = nullptr;
            if (s->weaponKinds[0] == 255 && !options.weapon.empty()) {
                const assets::ItemRow* asked = items.Find(std::string_view(options.weapon));
                if (asked != nullptr && (asked->country == (s->race == 1 ? 1 : 0) || asked->country == 3)) anyWeapon = asked;
            }
            if (anyWeapon != nullptr) {
                weapon = anyWeapon->refObjId;
                if (held[caster] != weapon) {
                    Put(out.recording, t, proto::EquipmentChanged{caster, std::uint8_t(slot::kWeapon), weapon, options.plus, held[caster]});
                    held[caster] = weapon;
                    heldKind[caster] = anyWeapon->typeId[3];
                    heldRange[caster] = anyWeapon->Stat(94) / 10.0f;
                    t += 0.8; // the stance changes before the swing
                }
            } else if (s->weaponKinds[0] != 255) {
                const int kind = options.weaponKind != 0 && (s->weaponKinds[0] == options.weaponKind || s->weaponKinds[1] == options.weaponKind)
                                     ? options.weaponKind
                                     : s->weaponKinds[0];
                const assets::ItemRow* w = WeaponOf(items, kind, s->race == 1 ? 1 : 0);
                if (!options.weapon.empty()) {
                    const assets::ItemRow* asked = items.Find(std::string_view(options.weapon));
                    if (asked != nullptr && w != nullptr && asked->typeId[3] == w->typeId[3]) w = asked;
                }
                if (w == nullptr) {
                    error = "no weapon of kind " + std::to_string(kind) + " for " + s->codename;
                    return false;
                }
                weapon = w->refObjId;
                if (held[caster] != weapon) {
                    Put(out.recording, t, proto::EquipmentChanged{caster, std::uint8_t(slot::kWeapon), weapon, options.plus, held[caster]});
                    held[caster] = weapon;
                    heldKind[caster] = w->typeId[3];
                    heldRange[caster] = w->Stat(94) / 10.0f;
                    t += 0.8; // the stance changes before the swing
                }
            }
            const SkillTraits traits = TraitsOf(*s);
            // A skill object (an area of kind 3: Harmony, the poison field, the fire trap) stands at the
            // caster's feet; its strikes come from it, not from the swing.
            const bool object = static_cast<bool>(assets::FindSkillParam(*s, assets::SkillTag("efr"), 3));
            // An aura that strikes ('puls' with a hostile area of kind 2: the Warlock's soul chaos): a buff
            // on its caster whose pulses strike around it (the server's CastBuff and Pulse).
            const assets::SkillParam pulse = assets::FindSkillParam(*s, assets::SkillTag("puls"));
            const assets::SkillParam aura = assets::FindSkillParam(*s, assets::SkillTag("efr"), 2);
            const bool pulses = pulse && pulse.Int(0) > 0 && aura && (aura[5] & 0x08) != 0 && !s->targetRequired;
            const bool onTarget = !object && !pulses && (traits.attack || traits.debuff || traits.target == SkillTraits::Target::Enemy);
            const std::uint32_t on = onTarget ? SkillBench::kTarget : caster;
            if (onTarget) {
                // Where the server's walk would stop it (Skills::Fight): the two bodies and
                // 70 % of its range (Action_Range, col 21, else the weapon's: a bow's 180, a glaive's 18), at least 15.
                const float r = s->range > 0 ? float(s->range) : std::max(heldRange[caster], 6.0f);
                const float reach = options.reach > 0.0f ? options.reach : std::max(15.0f, 10.0f + (r < 10.0f ? r : r * 0.7f));
                const float hx = caster == SkillBench::kChinese ? -12.0f : 12.0f, hz = 0.0f;
                const float dx = hx - 0.0f, dz = hz - 40.0f, d = std::hypot(dx, dz);
                const std::pair<float, float> want{dx / d * reach, 40.0f + dz / d * reach};
                std::pair<float, float>& now = stand[caster];
                const float walk = std::hypot(want.first - now.first, want.second - now.second);
                if (walk > 1.0f) {
                    Put(out.recording, t, proto::EntityMoved{caster, at(now.first, now.second), at(want.first, want.second), 200.0f});
                    t += walk / 200.0f + 0.3;
                    now = want;
                }
            }
            const double lead = std::max(0, s->preparingMs) / 1000.0 + std::max(0, s->castingMs) / 1000.0;
            const double length = std::max(0.8, std::max(0, s->durationMs) / 1000.0);
            SkillBenchStep step;
            step.at = t;
            step.skill = s->id;
            step.caster = caster;
            step.weapon = weapon;
            step.codename = s->codename;
            step.capture = t + 0.5;
            // A teleport (Ghost Walk, the Wizard's): its move told before its swing (EntityBlink): a dash 120
            // units on, a put 40 (in the bench's view).
            const bool dashes = bool(assets::FindSkillParam(*s, assets::SkillTag("tele")));
            if (dashes || assets::FindSkillParam(*s, assets::SkillTag("tel2"))) {
                std::pair<float, float>& now = stand[caster];
                const std::pair<float, float> to{now.first, now.second + (dashes ? 120.0f : 40.0f)};
                Put(out.recording, t, proto::EntityBlink{caster, s->id, dashes ? proto::BlinkKind::Dash : proto::BlinkKind::Put, at(to.first, to.second)});
                Put(out.recording, t + 0.05, proto::EntityMoved{caster, at(to.first, to.second), at(to.first, to.second), 0.0f});
                now = to;
                step.capture = t + 0.12;
            }
            if (options.casterState > 0) {
                Put(out.recording, t - 0.01, proto::EntityStates{caster, {{std::uint8_t(options.casterState), std::uint8_t(1), 8000u}}});
            }
            Put(out.recording, t, proto::EntitySwing{caster, on, s->id});
            // A heal or a cure on its caster: taking hold at its impact (its ACT_S on it, as the server tells).
            if ((traits.heal || traits.cure) && !traits.attack && !traits.resurrect && !(traits.buff || traits.toggle)) {
                Put(out.recording, t + lead, proto::SkillTook{caster, caster, s->id});
            }
            // A resurrection: the other bench player raised a second after its cast (its ACT_S on it).
            if (traits.resurrect) {
                const std::uint32_t raised = caster == SkillBench::kChinese ? SkillBench::kEuropean : SkillBench::kChinese;
                Put(out.recording, t + lead + 1.0, proto::SkillTook{raised, caster, s->id});
                step.capture = t + lead + 1.3;
            }
            if (options.state > 0) {
                Put(out.recording, t + 0.05, proto::EntityStates{SkillBench::kTarget, {{std::uint8_t(options.state), std::uint8_t(1), 4000u}}});
                Put(out.recording, t + 4.0, proto::EntityStates{SkillBench::kTarget, {}});
            }
            if (object) {
                // The server's entity from the impact, 6 s here (a trap goes off after 1.5 s: its strike,
                // a hit on the monster, then it is gone).
                proto::EntityAppeared o;
                o.entity = 1000 + std::uint32_t(out.steps.size());
                o.kind = proto::EntityKind::SkillObject;
                o.character.model = s->id;
                o.amount = caster;
                o.position = at(stand[caster].first, stand[caster].second);
                Put(out.recording, t + lead, o);
                const bool trap = static_cast<bool>(assets::FindSkillParam(*s, assets::SkillTag("trap")));
                const bool strikes = (assets::FindSkillParam(*s, assets::SkillTag("efr"), 3)[5] & 0x08) != 0;
                if (strikes) {
                    const double struck = t + lead + 1.5;
                    Put(out.recording, struck, proto::SkillObjectStruck{o.entity, s->id, trap});
                    Put(out.recording, struck + 0.05, proto::EntityDamaged{SkillBench::kTarget, caster, 1u, 10'000'000u, std::uint8_t(0)});
                    step.capture = struck + 0.2;
                }
                Put(out.recording, trap ? t + lead + 1.6 : t + lead + 6.0, proto::EntityGone{o.entity});
                if (!strikes) step.capture = t + lead + 1.0;
                t += trap ? 1.6 : 6.0;
            }
            // Its hits, as the server tells them: each row's (the skill, then its chain's) at once as
            // the row starts (the first after its preparing and casting), numbered on through the
            // chain; the client shows them at its clip's damage moments.
            // A DOT (the Warlock's: a link with 'lks2' and a 'puls'): no hit of its swing; its copy on the
            // target from the impact, a strike of its own each period (three here), then the copy gone.
            const int dotPulseMs = assets::FindSkillParam(*s, assets::SkillTag("lks2")) ? assets::FindSkillParam(*s, assets::SkillTag("puls")).Int(0) : 0;
            if (onTarget && dotPulseMs > 0) {
                const double first = t + lead, period = dotPulseMs / 1000.0;
                Put(out.recording, first, proto::EntityBuffs{SkillBench::kTarget, {s->id}});
                for (int k = 0; k < 3; ++k) {
                    Put(out.recording, first + k * period,
                        proto::EntityDamaged{SkillBench::kTarget, caster, 1u, 10'000'000u, std::uint8_t(proto::kDamageReturn), s->id});
                }
                Put(out.recording, first + 3 * period, proto::EntityBuffs{SkillBench::kTarget, {}});
                step.capture = first + period + 0.2;
                t = std::max(t, first + 3 * period - lead - length - 1.2 + 0.3);
            } else if (onTarget) {
                double rowAt = t + lead + std::max(0, options.impactDelayMs) / 1000.0;
                int strike = 0;
                for (const assets::SkillRow* part = s; part != nullptr && strike < 255;
                     part = part->chainNext != 0 ? skills.Find(part->chainNext) : nullptr) {
                    for (int h = 0; h < std::max(1, part->hits); ++h, ++strike) {
                        proto::EntityDamaged hit{SkillBench::kTarget, caster, 1u, 10'000'000u, std::uint8_t(0)};
                        hit.strike = std::uint8_t(std::min(255, strike));
                        Put(out.recording, rowAt, hit);
                    }
                    rowAt += std::max(0, part->durationMs) / 1000.0;
                }
                // The states its rows give (K3) from its first hit to the step's end, as on a level-1
                // target that resists nothing; a knock-down or knock-back as its first hit lands.
                // With the first hit, as the server sends them (the client holds them with it).
                const double first = t + lead + std::max(0, options.impactDelayMs) / 1000.0, end = t + lead + length + 1.2;
                proto::EntityStates given{SkillBench::kTarget, {}};
                AttackSpec knocks;
                // A time bomb bursts as its state ends (a fuse of 1 s here): its S_RETURN on the target.
                std::uint32_t bombSkill = 0, bombDamage = 0;
                const assets::SkillRow* row = s;
                for (int n = 0; row != nullptr && n < 16; ++n) {
                    for (const StateGift& g : StateGiftsOf(*row)) {
                        if (g.kind == StateKind::TimeBomb) {
                            bombSkill = row->id;
                            bombDamage = std::uint32_t(std::max(1, g.words[3]));
                        }
                        const int level = IsElement(g.kind) ? g.words[0] : g.words[2];
                        given.states.push_back({std::uint8_t(g.kind), std::uint8_t(std::clamp(level, 0, 255)), std::uint32_t((end - first) * 1000.0)});
                    }
                    const AttackSpec a = AttackOf(*row);
                    if (a.knockDownChance > 0) knocks.knockDownChance = a.knockDownChance;
                    if (a.knockBackChance > 0) knocks.knockBackChance = a.knockBackChance;
                    row = row->chainNext != 0 ? skills.Find(row->chainNext) : nullptr;
                }
                if (!given.states.empty() || knocks.knockDownChance > 0 || knocks.knockBackChance > 0) step.capture = first + 0.8;
                if (!given.states.empty()) {
                    const double over = bombSkill != 0 ? first + 1.0 : end - 0.05;
                    Put(out.recording, first, given);
                    Put(out.recording, over, proto::EntityStates{SkillBench::kTarget, {}});
                    if (bombSkill != 0) {
                        Put(out.recording, over, proto::EntityDamaged{SkillBench::kTarget, caster, bombDamage, 10'000'000u, proto::kDamageBurst, bombSkill});
                        step.capture = over + 0.3;
                    }
                }
                if (knocks.knockDownChance > 0 || knocks.knockBackChance > 0) {
                    const proto::KnockKind kind = knocks.knockDownChance > 0 ? proto::KnockKind::Down : proto::KnockKind::Back;
                    Put(out.recording, first, proto::EntityKnocked{SkillBench::kTarget, caster, kind, at(0.0f, 40.0f), std::uint32_t((end - first) * 1000.0)});
                }
            }
            // An imbue (K5): on the player's board while it lasts (its loop on the hand), then a basic
            // attack of the weapon in hand whose hit carries its force: the imbue's hit effect shows.
            if (traits.imbue) {
                const std::uint32_t mine = caster == SkillBench::kChinese ? token++ : 0u;
                if (mine != 0) Put(out.recording, t, proto::BuffAdded{mine, s->id, s->buffDurationMs});
                if (mine == 0) Put(out.recording, t, proto::EntityBuffs{caster, {s->id}}); // another player's, as its observers hear
                const assets::SkillRow* basic = BasicAttackOf(skills, heldKind[caster]);
                if (basic != nullptr) {
                    const double swing = t + 1.0, basicLength = std::max(0.8, std::max(0, basic->durationMs) / 1000.0);
                    const double hit = swing + basicLength * 0.3;
                    Put(out.recording, swing, proto::EntitySwing{caster, SkillBench::kTarget, basic->id});
                    Put(out.recording, hit, proto::EntityDamaged{SkillBench::kTarget, caster, 1u, 10'000'000u, std::uint8_t(0), s->id});
                    step.capture = hit + 0.25;
                    t = swing + basicLength - length - lead;
                }
                // Its loop held 6 s at least, to be seen going round (K10).
                if (mine != 0) Put(out.recording, std::max(t + lead + length + 1.1, step.at + 6.0), proto::BuffRemoved{mine});
                if (mine == 0) Put(out.recording, std::max(t + lead + length + 1.1, step.at + 6.0), proto::EntityBuffs{caster, {}});
                t = std::max(t, step.at + 6.0 - lead - length - 1.2);
            } else if (object) {
                // Its copy on whoever stands in it comes from the server's object (none here).
            } else if ((traits.buff || traits.toggle || pulses) && caster == SkillBench::kChinese) {
                // A buff of the player's: on its board from its swing's end to the step's end (its
                // start, its loop and its end show; K10), held a little longer to be seen.
                const std::uint32_t mine = token++;
                Put(out.recording, t + lead, proto::BuffAdded{mine, s->id, s->buffDurationMs});
                // A companion ('summ': the Bow's hawk): two of its strikes on the monster, as the server
                // tells them (it flies there, attacks, comes back), the buff held to the second's end.
                const bool companion = static_cast<bool>(assets::FindSkillParam(*s, assets::SkillTag("summ")));
                double holds = 6.0;
                if (companion) {
                    for (const double strike : {t + lead + 1.5, t + lead + 9.0}) {
                        proto::EntityDamaged hit{SkillBench::kTarget, caster, 1u, 10'000'000u, std::uint8_t(proto::kDamageCompanion), s->id};
                        Put(out.recording, strike, hit);
                    }
                    holds = lead + 16.0;
                }
                Put(out.recording, std::max(t + lead + length + 2.1, t + holds), proto::BuffRemoved{mine});
                step.capture = t + lead + length + 0.5;
                t += std::max(1.0, holds - lead - length - 1.2);
            } else if (traits.buff || traits.toggle || pulses) {
                // Another player's (the European's): as those who see it are told its buffs.
                Put(out.recording, t + lead, proto::EntityBuffs{caster, {s->id}});
                // An aura's: two of its pulses on the monster, as the server tells them (no swing's: its
                // S_RETURN shows).
                if (pulses) {
                    for (const double at : {t + lead + 1.0, t + lead + 1.0 + std::min(4.0, pulse.Int(0) / 1000.0)}) {
                        Put(out.recording, at, proto::EntityDamaged{SkillBench::kTarget, caster, 1u, 10'000'000u, std::uint8_t(proto::kDamageReturn), s->id});
                    }
                }
                Put(out.recording, t + 6.0, proto::EntityBuffs{caster, {}});
                step.capture = t + lead + length + 0.5;
                t += std::max(1.0, 6.0 - lead - length - 1.2);
            }
            // A transform: the Rogue's mask shown as the mask used (the
            // caster in the bench monster's shape), its duplicate in the other caster's look; its own
            // again after 2.5 s.
            const bool masked = static_cast<bool>(assets::FindSkillParam(*s, assets::SkillTag("mcap")));
            const assets::SkillParam change = assets::FindSkillParam(*s, assets::SkillTag("msch"));
            if (masked || (change && change[0] == 2)) {
                const std::uint32_t other = caster == SkillBench::kEuropean ? SkillBench::kChinese : SkillBench::kEuropean;
                const double from = t + lead + length + 0.3; // after the swing (a monster has not its clips)
                proto::EntityLook shape{caster};
                if (masked) {
                    shape.model = monster->refObjId;
                } else {
                    shape.model = other == SkillBench::kEuropean ? european->refObjId : chinese->refObjId;
                    if (held[other] != 0) shape.equipment = {{held[other], options.plus}};
                }
                Put(out.recording, from, shape);
                proto::EntityLook own{caster, caster == SkillBench::kEuropean ? european->refObjId : chinese->refObjId};
                if (held[caster] != 0) own.equipment = {{held[caster], options.plus}};
                Put(out.recording, from + 2.5, own);
                step.capture = from + 1.0;
                t = std::max(t, from + 2.8 - lead - length - 1.2);
            }
            t += lead + length + 1.2 + std::max(0.0, options.pauseSeconds);
            step.until = t;
            out.steps.push_back(std::move(step));
        }
    }
    if (out.steps.empty()) {
        error = "no skill matches '" + std::string(filter) + "'";
        return false;
    }
    // The last picture held a moment (a swing of no skill: nothing shows).
    Put(out.recording, t + 1.0, proto::EntitySwing{SkillBench::kChinese, SkillBench::kChinese, 0u});
    std::stable_sort(out.recording.messages.begin(), out.recording.messages.end(),
                     [](const RecordedMessage& a, const RecordedMessage& b) { return a.time < b.time; });
    return true;
}

} // namespace sr::game
