// COS on the game screen: our own rides and transports (CosSummoned / CosDismissed), who rides
// what (CosRiding, ours and others'), the riders drawn on their mounts, the commands and the test steps.
//
// A rider sits on its mount's "saddle" bone in the "cart" set's state 0 (the prototype's finding on the original's
// files; prototype/src/game/Mount.cpp), facing the mount's way; the mount walks the rider's legs (the server gives it
// the same ones), so it runs and stands as its own legs say. The mount may come into sight after the word that it is
// ridden: the rider is seated once both are staged.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>

#include <glm/glm.hpp>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "play/PlayScreen.h"
#include "audio/GameAudio.h"
#include "sr/core/Log.h"
#include "sr/game/Cos.h"
#include "sr/game/Inventory.h"

namespace sr::client {

namespace proto = game::proto;

void PlayScreen::HandleCos(ScreenContext& context, const proto::CosEvent& event) {
    if (const auto* summoned = std::get_if<proto::CosSummoned>(&event)) {
        // A growth or pick pet just summoned: SND_COS_SUMMON (the original's CICCos::vf_068 for a fresh one; ours the
        // owner's alone, the world shown: not the pets already out when the player enters).
        const bool fresh = std::none_of(ownCoses_.begin(), ownCoses_.end(), [&](const OwnCos& c) { return c.entity == summoned->entity; });
        const auto kind = game::CosKind(summoned->kind);
        if (fresh && shown_ && !replay_ && context.audio != nullptr && (kind == game::CosKind::AttackPet || kind == game::CosKind::GrabPet)) {
            context.audio->PlayUi("SND_COS_SUMMON");
        }
        std::erase_if(ownCoses_, [&](const OwnCos& c) { return c.entity == summoned->entity; });
        OwnCos own{summoned->entity, summoned->model, summoned->item, summoned->kind, summoned->hp, summoned->maxHp, summoned->name,
                   summoned->level, summoned->hgp, summoned->option};
        own.experience = summoned->experience;
        own.aggressive = summoned->aggressive;
        if (summoned->rentSeconds != 0) {
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            own.rentUntil = std::int64_t(now) + std::int64_t(summoned->rentSeconds);
        }
        ownCoses_.push_back(std::move(own));
        core::LogInfo("game: our COS {} (model {}, kind {})", summoned->entity, summoned->model, int(summoned->kind));
    } else if (const auto* dismissed = std::get_if<proto::CosDismissed>(&event)) {
        std::erase_if(ownCoses_, [&](const OwnCos& c) { return c.entity == dismissed->entity; });
        core::LogInfo("game: our COS {} is gone", dismissed->entity);
    } else if (const auto* riding = std::get_if<proto::CosRiding>(&event)) {
        if (riding->riding) {
            riders_[riding->rider] = riding->mount;
        } else {
            riders_.erase(riding->rider);
        }
        if (riding->rider == view_.Self()) core::LogInfo("game: {} {}", riding->riding ? "riding" : "on foot, off", riding->mount);
    } else if (const auto* bag = std::get_if<proto::CosBag>(&event)) {
        for (OwnCos& c : ownCoses_) {
            if (c.entity != bag->entity) continue;
            c.bagSize = bag->size;
            c.bag.clear();
            for (const proto::ItemRecord& r : bag->items) c.bag[r.slot] = game::Inventory::FromRecord(r);
            ++c.bagVersion;
        }
    } else if (const auto* option = std::get_if<proto::CosOption>(&event)) {
        for (OwnCos& c : ownCoses_) {
            if (c.entity == option->entity) c.option = option->option;
        }
    } else if (const auto* items = std::get_if<proto::CosPetItems>(&event)) {
        // The pets of the bag's summoning items: their tooltips and slots (redrawn).
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        for (const proto::CosPetItem& p : items->pets) {
            hudContext_.petItems[p.id] = {p.model, p.state, p.name, p.level, p.rentSeconds != 0 ? std::int64_t(now) + std::int64_t(p.rentSeconds) : 0};
        }
        ++player_.inventoryVersion;
    } else if (const auto* exp = std::get_if<proto::CosExperience>(&event)) {
        for (OwnCos& c : ownCoses_) {
            if (c.entity == exp->entity) c.experience = exp->experience;
        }
        // "You gained [%d] pet experience." (the 1.188 client's, on the original's 0x30C9 type 3)
        // "You have lost [%d]amount of pet experience." (the same, a negative change: its death's)
        if (!replay_ && exp->lost > 0) {
            player_.Post(game::GameMessage::Kind::General, FormatText(gui_.data().Text("UIIT_MSG_COSPET_LOST_EXP"), {std::to_string(exp->lost)}), context.time);
        }
        if (!replay_ && exp->gained > 0) {
            player_.Post(game::GameMessage::Kind::General, FormatText(gui_.data().Text("UIIT_MSG_COSPET_GAIN_EXP"), {std::to_string(exp->gained)}), context.time);
        }
    } else if (const auto* fed = std::get_if<proto::CosHgp>(&event)) {
        for (OwnCos& c : ownCoses_) {
            if (c.entity != fed->entity) continue;
            // Hungry from now on: "All stats are lowered by 50% when the pet's HGP is lower than 30%."
            if (!replay_ && c.hgp > game::kCosHungry && fed->hgp <= game::kCosHungry) {
                player_.Post(game::GameMessage::Kind::General, gui_.data().Text("UIIT_MSG_COSPETERR_HGP_30LOW"), context.time);
            }
            c.hgp = fed->hgp;
        }
    } else if (const auto* stance = std::get_if<proto::CosStance>(&event)) {
        for (OwnCos& c : ownCoses_) {
            if (c.entity == stance->entity) c.aggressive = stance->aggressive;
        }
    } else if (const auto* renamed = std::get_if<proto::CosRenamed>(&event)) {
        for (OwnCos& c : ownCoses_) {
            if (c.entity == renamed->entity) c.name = renamed->name;
        }
        core::LogInfo("game: our pet {} is named {}", renamed->entity, renamed->name);
    } else if (const auto* refused = std::get_if<proto::CosRefused>(&event)) {
        const std::string text = gui_.data().Text(refused->text.c_str());
        // A murderer's ride (0x1876): a banner too (category 5).
        if (!replay_ && refused->text == "UIIT_MSG_STRGERR_MURDERER_CANT_SUMMON_VEHICLE") player_.Notify(game::Banner::Kind::Warning, text, context.time);
        if (!replay_) player_.Post(game::GameMessage::Kind::General, refused->arg.empty() ? text : FormatText(text, {refused->arg}), context.time);
        core::LogInfo("game: cos: refused ({})", refused->text);
    }
    SyncCosHud();
}

void PlayScreen::SyncCosHud() {
    hudContext_.coses.clear();
    const std::uint32_t mount = OwnMount();
    for (const OwnCos& c : ownCoses_) {
        hudContext_.coses.push_back({c.entity, c.model, c.kind, c.hp, c.maxHp, c.entity == mount, c.name, c.level, c.hgp, c.option, c.bagSize, c.bag,
                                     c.bagVersion, c.experience, c.rentUntil, c.aggressive});
    }
    if (!hud_.Created()) return;
    if (hudContext_.coses.empty()) {
        hud_.Hide(hud::kPets);
    } else if (!hud_.ShownOrWaiting(hud::kPets)) {
        hud_.ShowWithInterface(hud::kPets); // F12's hidden interface keeps it hidden (the user's report 2026-11-14)
    }
}

void PlayScreen::CosCommand(int command, std::uint32_t entity) {
    if (replay_) return;
    const auto it = std::find_if(ownCoses_.begin(), ownCoses_.end(), [&](const OwnCos& c) { return c.entity == entity; });
    // The one asked for, else (a hotkey with none picked yet) the first.
    const OwnCos* cos = it != ownCoses_.end() ? &*it : ownCoses_.empty() ? nullptr : &ownCoses_.front();
    if (cos == nullptr) return;
    const bool carrier = cos->kind == std::uint8_t(game::CosKind::Ride) || cos->kind == std::uint8_t(game::CosKind::Transport);
    const bool pet = cos->kind == std::uint8_t(game::CosKind::AttackPet) || cos->kind == std::uint8_t(game::CosKind::GrabPet);
    const std::uint32_t id = cos->entity;
    const auto dismiss = [this, id] { shared_->session->SendGame(proto::CosDismiss::kId, proto::Encode(proto::CosDismiss{id})); };
    const auto ask = [this, dismiss](std::vector<std::string> lines) {
        hud_.Confirm("UIIT_STT_CONFIRM_BOX", std::move(lines), [dismiss](bool yes) {
            if (yes) dismiss();
        });
    };
    switch (command) {
    case kCosRide: // Board / Dismount (Home): a ride's dismount is its end (the server's rule)
        if (carrier) shared_->session->SendGame(proto::CosRide::kId, proto::Encode(proto::CosRide{cos->entity, OwnMount() != cos->entity}));
        break;
    case kCosTerminate: // PgUp: a pet's Unsummon asks first ("Cancel pet summoning?"); a transport's Terminate too, its
                        // goods would fall (UIIT_MSG_COS_CLEAN_CONFIRM1, 2); a ride's goes at once
        if (pet) {
            ask({gui_.data().Text("UIIT_MSG_COSPET_PETRETURN")});
        } else if (cos->kind == std::uint8_t(game::CosKind::Transport)) {
            ask({gui_.data().Text("UIIT_MSG_COS_CLEAN_CONFIRM1"), gui_.data().Text("UIIT_MSG_COS_CLEAN_CONFIRM2")});
        } else {
            dismiss();
        }
        break;
    case kCosFollow: // Follow (Del): a pet back to its owner
        if (pet) shared_->session->SendGame(proto::CosCommand::kId, proto::Encode(proto::CosCommand{id, proto::CosOrder::Follow}));
        break;
    case kCosAttack: // Attack: an attack pet on the monster the player has selected
        if (cos->kind == std::uint8_t(game::CosKind::AttackPet) && player_.target.entity != 0 && player_.target.kind == game::TargetStatus::Kind::Monster) {
            shared_->session->SendGame(proto::CosCommand::kId, proto::Encode(proto::CosCommand{id, proto::CosOrder::Attack, player_.target.entity}));
        }
        break;
    case kCosStance: // PgDn: aggressive / defensive in turn
        if (cos->kind == std::uint8_t(game::CosKind::AttackPet)) {
            shared_->session->SendGame(proto::CosCommand::kId, proto::Encode(proto::CosCommand{id, proto::CosOrder::Stance}));
        }
        break;
    case kCosInfo: // Info (Insert): the COS window, for any COS out (gives it to every one); its Basic
                   // Info by the kind, a grab pet's bag and settings beside it
        hudContext_.cosSelected = cos->entity;
        hud_.Toggle(hud::kPetWindow);
        break;
    default: break;
    }
}

void PlayScreen::CosHit(const proto::EntityDamaged& hit) {
    for (OwnCos& c : ownCoses_) {
        if (c.entity != hit.target) continue;
        c.hp = hit.hp;
        SyncCosHud();
        return;
    }
}

void PlayScreen::CosForget(std::uint32_t entity) {
    riders_.erase(entity); // a rider out of sight; a mount out of sight leaves its rider standing until told
}

std::uint32_t PlayScreen::OwnMount() const {
    const auto it = riders_.find(view_.Self());
    return it != riders_.end() ? it->second : 0;
}

bool PlayScreen::SeatOf(std::uint32_t rider, glm::vec3& at, float& yaw) const {
    if (!world_) return false;
    const auto it = riders_.find(rider);
    if (it == riders_.end()) return false;
    const auto m = slots_.find(it->second);
    const game::WorldView::Entity* mount = view_.Find(it->second);
    if (m == slots_.end() || mount == nullptr) return false;
    glm::mat4 saddle(1.0f);
    for (const char* bone : {"saddle", "Saddle", "SADDLE"}) {
        if (world_->StageBone(m->second, bone, saddle)) {
            at = glm::vec3(saddle[3]);
            yaw = mount->yaw;
            return true;
        }
    }
    return false; // its model is not in yet: on its feet meanwhile
}

void PlayScreen::CosRide(bool ride) {
    if (replay_) return;
    for (const OwnCos& c : ownCoses_) {
        if (c.kind != std::uint8_t(game::CosKind::Ride) && c.kind != std::uint8_t(game::CosKind::Transport)) continue;
        shared_->session->SendGame(proto::CosRide::kId, proto::Encode(proto::CosRide{c.entity, ride}));
        return;
    }
}

void PlayScreen::CosDismiss() {
    if (replay_) return;
    for (const OwnCos& c : ownCoses_) {
        if (c.kind != std::uint8_t(game::CosKind::Ride) && c.kind != std::uint8_t(game::CosKind::Transport)) continue;
        shared_->session->SendGame(proto::CosDismiss::kId, proto::Encode(proto::CosDismiss{c.entity}));
        return;
    }
}

// The test steps (--guild / --fortress): cos-ride (board our ride or transport), cos-off (get down), cos-dismiss (send
// it away), pet-unsummon / pet-follow (our first pet, unasked); a scroll or a pet's item is used with use-item:PLACE.
bool PlayScreen::CosScriptStep(const std::string& verb, const std::string& arg) {
    const auto firstPet = [this]() -> const OwnCos* {
        for (const OwnCos& c : ownCoses_) {
            if (c.kind == std::uint8_t(game::CosKind::AttackPet) || c.kind == std::uint8_t(game::CosKind::GrabPet)) return &c;
        }
        return nullptr;
    };
    if (verb == "cos-ride") {
        CosRide(true);
    } else if (verb == "cos-off") {
        CosRide(false);
    } else if (verb == "cos-dismiss") {
        CosDismiss();
    } else if (verb == "pet-info" || verb == "attack-pet-info") {
        // pet-info: the first pet's window; attack-pet-info: the attack pet's.
        const OwnCos* pet = firstPet();
        if (verb == "attack-pet-info") {
            pet = nullptr;
            for (const OwnCos& c : ownCoses_) {
                if (c.kind == std::uint8_t(game::CosKind::AttackPet)) pet = &c;
            }
        }
        if (pet != nullptr) CosCommand(kCosInfo, pet->entity);
    } else if (verb == "pet-name") {
        // pet-name:NAME (the first pet's, as the window's Create name and its "Confirm pet name?" would send it).
        if (const OwnCos* pet = firstPet(); pet != nullptr && !replay_) {
            shared_->session->SendGame(proto::CosRename::kId, proto::Encode(proto::CosRename{pet->entity, arg}));
        }
    } else if (verb == "pet-use") {
        // pet-use:PLACE,TARGET (an item of the bag used on a pet's item, as dropping it there does).
        if (const auto comma = arg.find(','); comma != std::string::npos && !replay_) {
            const auto from = std::uint8_t(std::strtoul(arg.substr(0, comma).c_str(), nullptr, 10));
            const auto to = std::uint8_t(std::strtoul(arg.substr(comma + 1).c_str(), nullptr, 10));
            shared_->session->SendGame(proto::CosUseOnPet::kId, proto::Encode(proto::CosUseOnPet{from, to}));
        }
    } else if (verb == "goto-mob") {
        // A developer's teleport to a nest of that monster (SroGame --dev-commands), for a pet's fight in --fortress too.
        if (!replay_) shared_->session->SendGame(proto::DevGotoMonster::kId, proto::Encode(proto::DevGotoMonster{arg}));
    } else if (verb == "pet-attack" || verb == "pet-stance") {
        // pet-attack (the attack pet on the selected monster, else the nearest), pet-stance (aggressive / defensive).
        const OwnCos* pet = nullptr;
        for (const OwnCos& c : ownCoses_) {
            if (c.kind == std::uint8_t(game::CosKind::AttackPet)) pet = &c;
        }
        if (pet != nullptr && !replay_) {
            if (verb == "pet-stance") {
                shared_->session->SendGame(proto::CosCommand::kId, proto::Encode(proto::CosCommand{pet->entity, proto::CosOrder::Stance}));
            } else {
                std::uint32_t target = player_.target.kind == game::TargetStatus::Kind::Monster ? player_.target.entity : 0;
                if (target == 0) {
                    const game::WorldView::Entity* self = view_.Find(view_.Self());
                    float best = 1e9f;
                    for (const auto& [id, e] : view_.Entities()) {
                        if (e.kind != proto::EntityKind::Monster || e.look.hp == 0 || self == nullptr) continue;
                        const proto::FlatPosition a = proto::ToFlat(e.position), b = proto::ToFlat(self->position);
                        const float d = std::hypot(a.x - b.x, a.z - b.z);
                        if (d < best) {
                            best = d;
                            target = id;
                        }
                    }
                }
                if (target != 0) shared_->session->SendGame(proto::CosCommand::kId, proto::Encode(proto::CosCommand{pet->entity, proto::CosOrder::Attack, target}));
            }
        }
    } else if (verb == "pet-tab") {
        hudContext_.cosTab = std::atoi(arg.c_str()); // the open COS window's page (0 Basic Info, 1 Inventory, 2 Setting)
    } else if (verb == "pet-option" || verb == "pet-take") {
        // pet-option:N (a grab pet's settings, proto::kGrab* bits), pet-take:BAG,PLACE (from its bag to the inventory).
        const OwnCos* pet = nullptr;
        for (const OwnCos& c : ownCoses_) {
            if (c.kind == std::uint8_t(game::CosKind::GrabPet)) pet = &c;
        }
        if (pet != nullptr && !replay_) {
            if (verb == "pet-option") {
                const auto option = std::uint32_t(std::strtoul(arg.c_str(), nullptr, 0));
                shared_->session->SendGame(proto::CosSetOption::kId, proto::Encode(proto::CosSetOption{pet->entity, option}));
            } else if (const auto comma = arg.find(','); comma != std::string::npos) {
                const auto from = std::uint8_t(std::strtoul(arg.substr(0, comma).c_str(), nullptr, 10));
                const auto to = std::uint8_t(std::strtoul(arg.substr(comma + 1).c_str(), nullptr, 10));
                shared_->session->SendGame(proto::CosBagMove::kId, proto::Encode(proto::CosBagMove{pet->entity, from, to, true, false}));
            }
        }
    } else if (verb == "pet-unsummon" || verb == "pet-follow") {
        if (const OwnCos* pet = firstPet(); pet != nullptr && !replay_) {
            if (verb == "pet-unsummon") {
                shared_->session->SendGame(proto::CosDismiss::kId, proto::Encode(proto::CosDismiss{pet->entity}));
            } else {
                shared_->session->SendGame(proto::CosCommand::kId, proto::Encode(proto::CosCommand{pet->entity, proto::CosOrder::Follow}));
            }
        }
    } else {
        return false;
    }
    return true;
}

} // namespace sr::client
