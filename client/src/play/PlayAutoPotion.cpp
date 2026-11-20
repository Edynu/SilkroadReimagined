// The game screen's side of the auto potion: the window's Confirm sends
// the settings (the server keeps them and uses the potions itself), and the scripted tries (src/main.cpp
// lists the options): the window filled as a player would (the items dropped in from the bag, the rows
// ticked, their percents, Confirm), the window left open, a fight to get hurt in, and an observer
// following a player.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "play/PlayScreen.h"
#include "sr/core/Log.h"
#include "sr/game/Skills.h"

namespace sr::client {

namespace proto = game::proto;

void PlayScreen::AutoPotionEnter(ScreenContext& context) {
    const core::Config& c = context.config;
    AutoPotionTries& t = autoPotionTries_;
    t.script = c.GetString("auto-potion", "");
    t.after = float(c.GetDouble("auto-potion-after", 2.0));
    t.window = c.GetBool("auto-potion-window", false);
    t.attackAfter = float(c.GetDouble("attack-nearest", -1.0));
    t.follow = c.GetString("follow", "");
    hudContext_.autoPotionSave = [this](const game::AutoPotion& s) {
        if (replay_) return;
        shared_->session->SetAutoPotion(game::ToMessage(s));
        core::LogInfo("game: auto potion kept: HP {} {} % {}, MP {} {} % {}, abnormal {} {}, delay {} {} ms", s.rows[0].enabled, s.rows[0].percent,
                      s.rows[0].item, s.rows[1].enabled, s.rows[1].percent, s.rows[1].item, s.rows[2].enabled, s.rows[2].item, s.delayEnabled,
                      s.delayMs);
    };
}

void PlayScreen::AutoPotionScript(ScreenContext& context) {
    AutoPotionTries& t = autoPotionTries_;
    if (!t.script.empty() && tables_ && tables_->Usable() && (t.after -= float(context.dt)) < 0.0f) {
        // "hp,70,ITEM_ETC_HP_POTION_01;mp,60,ITEM_ETC_MP_POTION_01;abnormal,ITEM_ETC_CURE_ALL_01;delay,1000":
        // the window open, each row's item dropped in from the bag, the row ticked at its percent;
        // "mp,off" (or "delay,off") unticks a row.
        ui::Widget* window = hud_.Show(hud::kAutoPotion);
        std::stringstream rows(t.script);
        std::string entry;
        while (window != nullptr && std::getline(rows, entry, ';')) {
            std::stringstream parts(entry);
            std::string kind, a, b;
            std::getline(parts, kind, ',');
            std::getline(parts, a, ',');
            std::getline(parts, b, ',');
            if (kind == "delay") {
                AutoPotionSetDelay(window, a != "off", a == "off" ? 1000 : std::atoi(a.c_str()));
                continue;
            }
            const int row = kind == "hp"         ? game::AutoPotion::kHp
                            : kind == "mp"       ? game::AutoPotion::kMp
                            : kind == "abnormal" ? game::AutoPotion::kAbnormal
                            : kind == "vehicle"  ? game::AutoPotion::kVehicle
                            : kind == "pet"      ? game::AutoPotion::kAttackPet
                                                 : -1;
            if (row >= 0 && a == "off") {
                AutoPotionSetRow(window, row, false, row == 2 ? 0 : 50);
                continue;
            }
            const std::string& code = row == 2 ? a : b;
            const int percent = row == 2 ? 0 : std::atoi(a.c_str());
            const assets::ItemRow* item = row >= 0 ? tables_->items.Find(std::string_view(code)) : nullptr;
            int place = -1;
            for (const auto& [slot, it] : player_.inventory.places) {
                if (slot >= game::slot::kWorn && item != nullptr && it.refId == item->refObjId) {
                    place = slot;
                    break;
                }
            }
            const bool taken = row >= 0 && place >= 0 && AutoPotionDrop(window, row, place);
            core::LogInfo("game: --auto-potion: {} {} % {} from place {}: {}", kind, percent, code, place, taken ? "dropped in" : "refused");
            if (row >= 0) AutoPotionSetRow(window, row, true, percent);
        }
        if (window != nullptr) AutoPotionConfirm(window);
        t.script.clear();
        t.reopen = t.window;
    }
    // The window shown again once the server's word is in (what it kept), at the left so the
    // character in the middle of the screen stays in sight.
    ui::Widget* shown = nullptr;
    if (t.reopen && player_.autoPotionVersion > 1) {
        shown = hud_.Show(hud::kAutoPotion);
        t.reopen = false;
    } else if (t.window && t.script.empty() && !t.reopen && !t.shown) {
        shown = hud_.Show(hud::kAutoPotion);
    }
    if (shown != nullptr) shown->rect.x = 20.0f;
    if (hud_.Shown(hud::kAutoPotion)) t.shown = true;
    // A fight to get hurt in: the nearest monster attacked, again whenever the last one is gone; the
    // camera high over the fighter (big monsters stand in front of a low one).
    if (t.attackAfter >= 0.0f) {
        cameraPitch_ = 0.95f;
        cameraDistance_ = 110.0f;
    }
    if (t.attackAfter >= 0.0f && (t.attackAfter -= float(context.dt)) < 0.0f) {
        t.attackAfter = 2.0f;
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        bool fighting = t.attacking != 0 && view_.Find(t.attacking) != nullptr && !killed_.contains(t.attacking);
        // On its way to the monster (round a wall): the strike waits for the way's end, the 30 s count from there
        // (a way stuck short ends as failed, PlayRoute.cpp).
        const bool onTheWay = Routing() && route_.target == t.attacking;
        if (onTheWay) t.targetSince = context.time;
        // A hunt leaves a monster it found no way to, or one not dead in 30 s (out of reach behind something: the
        // walker stood at a wall), and takes the next.
        if (fighting && !t.hunt.empty() && ((route_.target == t.attacking && route_.failed) || context.time - t.targetSince > 30.0)) {
            core::LogInfo("game: hunt: leaves monster {} ({})", t.attacking, route_.failed ? "no way to it" : "not dead in 30 s");
            t.unreachable.insert(t.attacking);
            t.attacking = 0;
            route_ = {};
            fighting = false;
        }
        // A hunt strikes with the character's strongest: its imbue kept on (the learned imbue of the highest mastery
        // level), its single-target attack skills of the highest mastery levels in turn; a plain attack without any.
        const auto strike = [&](std::uint32_t target) {
            if (t.hunt.empty() || !tables_ || !tables_->Usable()) return shared_->session->Attack(target);
            const assets::SkillRow* imbue = nullptr;
            std::vector<const assets::SkillRow*> attacks;
            for (const std::uint32_t id : player_.skills) {
                const assets::SkillRow* s = tables_->skills.Find(id);
                if (s == nullptr) continue;
                if (game::IsImbue(*s)) {
                    if (imbue == nullptr || s->masteryLevels[0] > imbue->masteryLevels[0]) imbue = s;
                } else if (game::TraitsOf(*s).attack && s->targetRequired && s->targetEnemyMonster) {
                    attacks.push_back(s);
                }
            }
            std::sort(attacks.begin(), attacks.end(), [](const auto* a, const auto* b) { return a->masteryLevels[0] > b->masteryLevels[0]; });
            if (attacks.size() > 4) attacks.resize(4);
            const bool imbued = imbue != nullptr && std::any_of(player_.effects.begin(), player_.effects.end(), [&](const game::ActiveEffect& e) {
                const assets::SkillRow* s = e.kind == game::ActiveEffect::Kind::Skill ? tables_->skills.Find(e.id) : nullptr;
                return s != nullptr && s->group == imbue->group;
            });
            if (imbue != nullptr && !imbued && context.time - t.imbueTried > 5.0) {
                t.imbueTried = context.time;
                core::LogInfo("game: hunt: imbue {}", imbue->codename);
                return shared_->session->UseSkill(imbue->id, 0);
            }
            // Short of mana (under a fifth): plain attacks while the potions bring it back.
            const bool lowMana = player_.self.maxMp > 0 && player_.self.mp * 5 < player_.self.maxMp;
            if (attacks.empty() || lowMana) return shared_->session->Attack(target);
            const assets::SkillRow* s = attacks[t.rotation++ % attacks.size()];
            shared_->session->UseSkill(s->id, target);
        };
        if (fighting && !onTheWay && self != nullptr && player_.self.hp > 0 && !t.hunt.empty()) strike(t.attacking);
        // A hunt (the quests' tests) picks up what lies near between its fights, as a player would (G a drop at a time).
        bool picking = false;
        if (self != nullptr && !fighting && player_.self.hp > 0 && !t.hunt.empty()) {
            const proto::FlatPosition me = proto::ToFlat(self->position);
            for (const auto& [id, e] : view_.Entities()) {
                const proto::FlatPosition p = proto::ToFlat(e.position);
                if (e.kind == proto::EntityKind::GroundItem && std::hypot(p.x - me.x, p.z - me.z) <= 150.0 && t.pickTries[id]++ < 3) {
                    shared_->session->PickUp(id);
                    core::LogInfo("game: hunt: picks up {}", id);
                    picking = true;
                    t.attackAfter = 1.0f;
                    break;
                }
            }
        }
        if (self != nullptr && !fighting && !picking && player_.self.hp > 0) {
            const proto::FlatPosition me = proto::ToFlat(self->position);
            std::uint32_t best = 0;
            double bestDistance = 1e9;
            for (const auto& [id, e] : view_.Entities()) {
                if (e.kind != proto::EntityKind::Monster || killed_.contains(id) || t.unreachable.contains(id)) continue;
                if (!t.hunt.empty()) {
                    // hunt:* fights the normal ones only (a giant or a champion takes a minute, a quest's drops come as
                    // well from the normal ones); a hunt by name that monster whatever its class (a unique a quest waits for).
                    const assets::ObjectRow* row = tables_ && tables_->Usable() ? tables_->characters.Find(e.look.model) : nullptr;
                    if (row == nullptr || (t.hunt == "*" ? (e.rarity & 0x0F) != 0 : row->codename != t.hunt)) continue;
                }
                const proto::FlatPosition p = proto::ToFlat(e.position);
                const double d = std::hypot(p.x - me.x, p.z - me.z);
                if (d < bestDistance) {
                    best = id;
                    bestDistance = d;
                }
            }
            if (best != 0) {
                t.attacking = best;
                t.targetSince = context.time;
                core::LogInfo("game: --attack-nearest: monster {} at {:.0f}", best, bestDistance);
                // A hunt goes round what stands between (a straight way: the strike's own approach).
                if (t.hunt.empty() || !RouteTo(view_.Find(best)->position, best)) {
                    route_ = {};
                    strike(best);
                }
            }
        }
    }
    // --follow NAME: walks to that player whenever it is more than 60 away (stopping 40 short), the
    // camera turned to look at it from behind the walker (an observer's captures).
    if (!t.follow.empty()) {
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        for (const auto& [id, e] : view_.Entities()) {
            if (self == nullptr || id == view_.Self() || e.kind != proto::EntityKind::Character || e.look.name != t.follow) continue;
            const proto::FlatPosition me = proto::ToFlat(self->position), them = proto::ToFlat(e.position);
            const double d = std::hypot(them.x - me.x, them.z - me.z);
            if (d > 1.0) cameraYaw_ = float(std::atan2(them.x - me.x, them.z - me.z));
            cameraDistance_ = 130.0f;
            cameraPitch_ = 0.9f;
            if ((t.followWait -= float(context.dt)) < 0.0f && d > 60.0) {
                t.followWait = 1.0f;
                const double k = (d - 40.0) / d;
                shared_->session->MoveTo(proto::FromFlat({me.x + (them.x - me.x) * k, me.z + (them.z - me.z) * k}, e.position.y));
            }
            break;
        }
    }
    if (player_.self.hp != t.lastHp && player_.self.maxHp > 0) {
        if (t.lastHp != 0 && player_.self.hp < t.lastHp) ++t.hurt;
        t.lastHp = player_.self.hp;
    }
}

} // namespace sr::client
