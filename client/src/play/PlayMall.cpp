// The item mall on the game screen (HudMall.cpp the window): its catalogue read once, the
// window's requests sent, the server's answers to the window, the preview's figure dressed with the avatars tried on,
// and the script's steps:
//   mall                          the window (as Yes in "Enter Item Mall")
//   mall-shop:CODE|HOT|BASKET     a menu row (MALL_CONSUME, MALL_AVATAR, ...; the hot store; the basket)
//   mall-tab:N                    the open shop's tab N
//   mall-search:TEXT              the search's text and Find
//   mall-popup:CODE               a good's right click (CODE: its item's codename or its package's code)
//   mall-buy:CODE[,COUNT]         the good's confirm window with that count; mall-yes its Buy (or an open box's yes)
//   mall-wear:CODE, mall-original, mall-buy-worn    the try-on, Original, the worn set's Buy (its box; mall-yes)
//   mall-reserve:CODE, mall-basket-buy              the Zzim box (mall-yes adds it), the basket's lump buy (mall-yes)
//   mall-history, mall-inquire:N  the Buying list, its period N (0 a day .. 3 three months) inquired
//   mall-close                    the window closed
//   mall-dress:CODENAME           the bag's first of it worn on the dress side

#include "play/PlayScreen.h"

#include "sr/core/Log.h"

namespace sr::client {

namespace proto = game::proto;

void PlayScreen::SetupMall(ScreenContext& context) {
    if (!mallRead_) {
        mallRead_ = true;
        std::string why;
        if (!mallTable_.Load(context.files, why)) core::LogWarning("game: the item mall: {}", why);
        mall_.catalog = &mallTable_;
    }
    hudContext_.mall = &mall_;
    hudContext_.mallRequest = [this](proto::MessageId id, std::vector<std::byte> payload) {
        if (!replay_) shared_->session->SendGame(id, std::move(payload));
    };
    hud_.onItemMall = [this] { AskItemMall(hudContext_); };
}

void PlayScreen::HandleMall(ScreenContext&, const proto::MallEvent& event) {
    if (!replay_) ItemMallAnswer(hudContext_, event);
}

bool PlayScreen::MallScriptStep(const std::string& verb, const std::string& arg) {
    if (!verb.starts_with("mall") || verb == "mall-use") return false;
    if (verb == "mall-dress") {
        // mall-dress:CODENAME: the bag's first of it put on the dress side (as a drop on its avatar place).
        for (const auto& [place, item] : player_.inventory.places) {
            const assets::ItemRow* row = tables_ && tables_->Usable() ? tables_->items.Find(item.refId) : nullptr;
            if (row == nullptr || row->codename != arg || place < game::slot::kWorn) continue;
            hud_.Request({HudRequest::Kind::WearAvatar, place});
            return true;
        }
        core::LogWarning("game: --guild mall-dress:{}: none in the bag", arg);
        return true;
    }
    const std::string step = arg.empty() ? verb : verb + ":" + arg;
    if (!ItemMallStep(hudContext_, step)) core::LogWarning("game: --guild {}: not done (no mall window, or no such good)", step);
    return true;
}

// The preview's figure (the mall's CIFRenderStatic 123): the character with what it wears, the avatars tried on in
// place of their kind's.
void PlayScreen::MallPreview(ScreenContext& context) {
    if (!hud_.Created() || !hud_.Shown(hud::kItemMall) || !tables_ || !tables_->Usable()) return;
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    if (self == nullptr) return;
    std::vector<std::uint32_t> worn;
    for (const proto::EquippedItem& item : self->look.equipment) worn.push_back(item.item);
    std::map<int, std::uint32_t> avatars;
    for (const proto::EquippedItem& item : self->look.avatars) {
        if (const assets::ItemRow* row = tables_->items.Find(item.item)) avatars[game::MallAvatarKind(*row)] = item.item;
    }
    for (const auto& [kind, good] : mall_.worn) {
        const assets::MallGood* g = mallTable_.Find(good.first, good.second);
        const assets::ItemRow* row = g != nullptr ? tables_->items.Find(std::string_view(g->itemCodename)) : nullptr;
        if (row != nullptr) avatars[kind] = row->refObjId;
    }
    for (const auto& [kind, item] : avatars) worn.push_back(item);
    PortraitLook look;
    std::string error;
    if (DressPortrait(context.files, tables_->characters, tables_->items, self->look.model, worn, self->look.height, look, error,
                      tables_->visuals.Size() > 0 ? &tables_->visuals : nullptr)) {
        portraits_.ShowFigure(kMallPreview, look, ItemMallPreviewYaw(hudContext_));
    }
}

} // namespace sr::client
