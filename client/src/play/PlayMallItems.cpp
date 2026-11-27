// The item mall's items on the game screen: the uses that ask first (the expansions' box), the
// items used on another (the Extension Gear on a Devil's Spirit, the potion of growth on a growth pet's item), the
// premium's chat commands (/Return, /Reverse Return, /Resurrection), its booth's question, and the
// services' answers as the client's lines (its error table).

#include <array>
#include <format>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "play/PlayScreen.h"
#include "sr/core/Log.h"
#include "sr/game/Mall.h"
#include "sr/game/MallProtocol.h"
#include "sr/game/SpecialItems.h"
#include "sr/game/TimedServices.h"

namespace sr::client {

namespace proto = game::proto;

const char* MallItemErrorKey(std::uint16_t code) {
    switch (code) {
    case 0x1882: return "UIIT_MSG_STRGERR_CANT_MUTATE_EQUIPED_ITEM";
    case 0x1887: return "UIIT_MSG_STRGERR_ONLY_THE_DEAD_CAN_USE_RESURRECT_SCROLL";
    case 0x1889: return "UIIT_SKILL_USE_FAIL_CHAR_DEAD";
    case 0x1894: return "UIIT_SKILL_USE_FAIL_OVERLAP";
    case 0x18B8: return "UIIT_MSG_COSPETERR_CANT_USE_WRONGOBJECT"; // ours: the original's key for 0x18B8 is not traced
    case 0x18C6: return "UIIT_MSG_PREMIUM_USE_OVER";
    case 0x18CC: return "UIIT_MSG_PREMIUM_NOT_USE";
    case 0x18F8: return "UIIT_MSG_NASRUN_EXTENSION_ITEM_USE_AFTER_AWAKE";
    case 0x18F9: return "UIIT_MSG_NASRUN_NOT_AWAKE_WEAR_ERROR";
    case 0x18FA: return "UIIT_CTL_NASRUN_NOT_TRADE";
    case 0x18FB: return "UIIT_STT_GOLDTIME_PAUSE_ERROR";
    case 0x18FE: return "UIIT_STT_STORAGE_EXPANSION_USE_ERORR";
    default: return nullptr;
    }
}

bool PlayScreen::BerserkScrollRefused(std::uint8_t place, double time) {
    if (!tables_ || !tables_->Usable() || player_.self.berserkLit == 0) return false;
    const game::ItemInstance* item = player_.inventory.At(place);
    const assets::ItemRow* row = item != nullptr ? tables_->items.Find(item->refId) : nullptr;
    if (row == nullptr || game::BonusScrollOf(*row).key != game::kBonusBerserkTimeKey) return false;
    player_.Post(game::GameMessage::Kind::General, gui_.data().Text("UIIT_MSG_HWAN_FAIL_OVERLAP_1"), time);
    return true;
}

bool PlayScreen::UseMallItem(std::uint8_t place) {
    if (!tables_ || !tables_->Usable()) return false;
    const game::ItemInstance* item = player_.inventory.At(place);
    const assets::ItemRow* row = item != nullptr ? tables_->items.Find(item->refId) : nullptr;
    if (row == nullptr || row->typeId[0] != 3 || row->typeId[1] != 3) return false;
    // The bag's use asks first for the guild recall scroll (12 / 3: "Recall all guild members now?") and for a
    // Forgotten World's hole (12 / 7: "Will you create the gate to the Forgotten World?"), in a confirmation box; Yes
    // uses it.
    if (row->typeId[2] == 12 && (row->typeId[3] == 3 || row->typeId[3] == 7)) {
        if (replay_ || !shared_ || !shared_->session) return true;
        const char* key = row->typeId[3] == 3 ? "UIIT_MSG_GUILD_RECALL" : "UIIT_MSG_USE_MAGIC_HOLE_CREATE_GATE";
        hud_.Confirm("UIIT_STT_CONFIRM_BOX", {gui_.data().Text(key)}, [this, place](bool yes) {
            if (yes && !replay_ && shared_ && shared_->session) shared_->session->UseItem(place);
        });
        return true;
    }
    if (row->typeId[2] != 13) return false;
    const int kind = row->typeId[3];
    if (kind != 17) return false; // 15, 16: a target picked first (HudServices.cpp, the cursor mode 0xA6)
    if (replay_ || !shared_ || !shared_->session) return true;
    const UiData& d = gui_.data();
    // The expansions ask first (the bag's use routine): the bag's or the storage's line.
    const char* key = row->params[1] == 1 ? "UIIT_STT_STORAGE_EXPANSION_WAREHOUSE_ADDITION_ITEM_USE" : "UIIT_STT_STORAGE_EXPANSION_INVENTORY_ADDITION_ITEM_USE";
    hud_.Confirm("", {d.Text(key)}, [this, place](bool yes) {
        if (yes && !replay_ && shared_ && shared_->session) shared_->session->UseItem(place);
    });
    return true;
}

// The original's item use answer on a success, by the item's kind: the guild recall scroll (12 / 3) "Recall has been
// requested to all guild members." in a confirmation box; a hole (12 / 7) gacha_change_2.wav at the player.
void PlayScreen::OwnItemUsed(std::uint32_t item) {
    if (!tables_ || !tables_->Usable() || replay_) return;
    const assets::ItemRow* row = tables_->items.Find(item);
    if (row == nullptr || row->typeId[0] != 3 || row->typeId[1] != 3 || row->typeId[2] != 12) return;
    if (row->typeId[3] == 3) {
        hud_.Inform("UIIT_STT_CONFIRM_BOX", {gui_.data().Text("UIIT_MSG_GUILD_RECALL_CONFIRM")}, nullptr);
    } else if (row->typeId[3] == 7 && hudContext_.playAtPlayer) {
        hudContext_.playAtPlayer("prim\\snd\\ui\\gacha_change_2.wav");
    }
}

bool PlayScreen::PremiumCommand(const std::string& line) {
    if (!tables_ || !tables_->Usable()) return false;
    const UiData& d = gui_.data();
    const std::string word = line.substr(0, line.find(' ') == std::string::npos ? line.size() : line.size());
    const auto is = [&](const char* key) { return d.HasText(key) && d.Text(key) == line; };
    const char* item = nullptr;
    if (is("UIIT_STT_PREMIUM_COMMAND_RETURN_HIGH_SPEED")) item = "ITEM_MALL_RETURN_SCROLL_HIGH_SPEED";
    if (is("UIIT_STT_PREMIUM_COMMAND_REVERSE_RETURN")) item = "ITEM_MALL_REVERSE_RETURN_SCROLL";
    if (is("UIIT_STT_PREMIUM_COMMAND_RESURRECTION_100%")) item = "ITEM_MALL_RESURRECTION_100P_SCROLL";
    (void)word;
    if (item == nullptr) return false;
    const assets::ItemRow* row = tables_->items.Find(std::string_view(item));
    if (row != nullptr) UsePremium(row->refObjId);
    core::LogInfo("game: premium command {}", line);
    return true;
}

void PlayScreen::UsePremium(std::uint32_t item) {
    if (!tables_ || !tables_->Usable()) return;
    const UiData& d = gui_.data();
    const auto post = [&](const char* key) { player_.Post(game::GameMessage::Kind::General, d.Text(key), player_.now); };
    // The client's own checks: the premium running (0x18CC), /Resurrection only for the dead (0x1887).
    const bool premium = std::any_of(player_.timedServices.services.begin(), player_.timedServices.services.end(),
                                     [](const proto::TimedService& s) { return s.category == game::kTimedPremium; });
    if (!premium) {
        post(MallItemErrorKey(0x18CC));
        return;
    }
    const assets::ItemRow* row = tables_->items.Find(item);
    if (row == nullptr || replay_ || !shared_ || !shared_->session) return;
    const bool dead = player_.self.maxHp > 0 && player_.self.hp == 0;
    if (row->typeId == std::array<int, 4>{3, 3, 13, 6} && !dead) {
        post(MallItemErrorKey(0x1887));
        return;
    }
    const std::uint32_t id = row->refObjId;
    if (row->typeId == std::array<int, 4>{3, 3, 3, 3}) {
        // /Reverse Return: the reverse return chooser (box kind 0x24), then the use with the choice.
        hud_.ReverseReturn([this, id](int result) {
            if (replay_ || !shared_ || !shared_->session || result == 0) return;
            if (result == 3) {
                ChooseReturnPoint(hudContext_, [this, id](std::uint32_t point) {
                    if (!replay_ && shared_ && shared_->session) shared_->session->SendGuild(proto::PremiumUse{id, proto::kReverseToPoint, point});
                });
                return;
            }
            shared_->session->SendGuild(proto::PremiumUse{id, std::uint8_t(result == 1 ? proto::kReverseToRecall : proto::kReverseToDeath), 0});
        });
        return;
    }
    shared_->session->SendGuild(proto::PremiumUse{id, 0, 0});
}

void PlayScreen::HandleMallItems(ScreenContext& context, const proto::MallEvent& event) {
    const UiData& d = gui_.data();
    const auto post = [&](const std::string& text) {
        if (!text.empty()) player_.Post(game::GameMessage::Kind::General, text, context.time);
    };
    // An item's durability (the original's 0x3052; read here before the state takes the new value): a
    // rise is snd_revive from 0 and snd_repair else (worn or in the bag); a worn item's fall to 0 is snd_eqbreak and to 6
    // snd_eqdanger.
    if (const auto* dur = std::get_if<proto::DurabilityChanged>(&event)) {
        const game::ItemInstance* item = player_.inventory.At(dur->place);
        ui::Interface* ui = gui_.interface();
        if (item != nullptr && ui != nullptr && !replay_) {
            const std::uint32_t before = item->durability, after = dur->durability;
            const char* sound = nullptr;
            // effectsound.txt's UI rows: SND_REPAIR (three, one picked), SND_REVIVE, SND_EQDANGER, SND_EQBREAK.
            if (after > before) {
                sound = before == 0 ? "SND_REVIVE" : "SND_REPAIR";
                FlashItemSlot(hudContext_, dur->place, ui::Slot::Flash::Repair); // the sparks
            } else if (dur->place < game::slot::kWorn && after != before) {
                sound = after == 0 ? "SND_EQBREAK" : after == 6 ? "SND_EQDANGER" : nullptr;
            }
            if (sound != nullptr) ui->Sound(sound);
        }
        return;
    }
    if (const auto* a = std::get_if<proto::ServiceAnswer>(&event)) {
        if (a->code != 0) {
            const char* key = MallItemErrorKey(a->code);
            post(d.Text(key != nullptr ? key : "UIIT_MSG_COSPETERR_CANT_USEITEM"));
            core::LogInfo("game: mall service {} refused (0x{:X})", int(a->request), a->code);
            return;
        }
        switch (a->request) {
        case proto::ServiceRequest::Pause: post(d.Text(a->b != 0 ? "UIIT_MSG_GOLDTIME_PAUSE_START" : "UIIT_MSG_GOLDTIME_PAUSE_STOP")); break;
        case proto::ServiceRequest::UseOn:
            if (const assets::ItemRow* r = tables_ && tables_->Usable() ? tables_->items.Find(a->a) : nullptr; r != nullptr && game::IsDevilSpirit(*r)) {
                post(d.Text("UIIT_MSG_NASRUN_AWAKE_RENEW"));
            }
            break;
        default: break;
        }
        core::LogInfo("game: mall service {} done ({}, {})", int(a->request), a->a, a->b);
    } else if (const auto* t = std::get_if<proto::TimedServices>(&event)) {
        // The premium's booth, asked once its package runs and before it was chosen (ours: the original asks from its
        // premium list window): "Establish as an avatar stall?".
        const bool premium = std::any_of(t->services.begin(), t->services.end(), [](const proto::TimedService& s) { return s.category == game::kTimedPremium; });
        if (premium && !premiumBoothAsked_ && !replay_) {
            premiumBoothAsked_ = true;
            hud_.Confirm("", {d.Text("UIIT_MSG_PREMIUM_AVATAR_SHOP_USE")}, [this](bool yes) {
                if (yes && !replay_ && shared_ && shared_->session) shared_->session->SendGuild(proto::PremiumBooth{0});
            });
        }
        for (const proto::TimedService& s : t->services) {
            core::LogInfo("game: time service {} item {}: {} s left, bonus {}/{} s{}", int(s.category), s.item, s.secondsLeft, s.bonus >> 16, s.bonus & 0xFFFF,
                          s.paused ? " (paused)" : "");
        }
        // The Open Market ticket running: the consignment notification's side icon.
        const bool ticket = std::any_of(t->services.begin(), t->services.end(),
                                        [](const proto::TimedService& s) { return s.category == game::openmarket::kTicketCategory && s.secondsLeft > 0; });
        ShowMarketAlarmGuide(hudContext_, ticket);
    } else if (const auto* g = std::get_if<proto::StorageGrown>(&event)) {
        core::LogInfo("game: {} grown to {}", g->target == 1 ? "the bag" : "the storage", int(g->size));
    }
}

} // namespace sr::client
