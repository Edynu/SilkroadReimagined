// The Open Market on the game screen: the session's market events into the player's
// game::OpenMarketView (the CNIFOpenMarket window reads it), their system lines with the item's name, the window opened
// when the market opens at Juel and closed when it closes, and the test steps (of --guild and --fortress).

#include <cmath>
#include <cstdlib>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "play/PlayScreen.h"
#include "sr/core/Log.h"

namespace sr::client {

namespace proto = game::proto;

void PlayScreen::HandleOpenMarket(ScreenContext& context, const proto::OpenMarketEvent& event) {
    const bool wasOpen = player_.openMarket.Open();
    const auto lines = player_.openMarket.Apply(event, context.time);
    for (const game::OpenMarketView::Line& line : lines) {
        const auto post = [this, line](double time) {
            std::string text = gui_.data().Text(line.key);
            if (line.item != 0) {
                const assets::ItemRow* row = hudContext_.items != nullptr ? hudContext_.items->Find(line.item) : nullptr;
                text = FormatText(text, {row != nullptr ? gui_.data().Text(row->nameKey) : std::string()});
            }
            if (!text.empty()) player_.Post(game::GameMessage::Kind::General, text, time);
        };
        if (gui_.data().Ready()) post(context.time);
        else textLater_.push_back(post);
    }
    if (const auto* a = std::get_if<proto::OpenMarketAnswer>(&event)) {
        if (a->code != game::openmarket::code::kOk) core::LogInfo("game: open market: request {:#x} refused, code {:#x}", a->request, a->code);
        else if (a->request == proto::OpenMarketBuy::kId) OpenMarketSearchAgain(hudContext_); // the bought row off the list
    }
    if (player_.openMarket.TakeOpened()) {
        core::LogInfo("game: open market: open");
        ShowOpenMarket(hudContext_, true);
        // The player's listings with the window (the Register and Settle pages read them).
        if (!replay_) shared_->session->SendGame(proto::OpenMarketMine::kId, proto::Encode(proto::OpenMarketMine{}));
    }
    if (wasOpen && !player_.openMarket.Open()) ShowOpenMarket(hudContext_, false);
}

// The test steps: om-open (the nearest Juel's market, as its row), om-close, om-put:PLACE (a bag item on the Register page,
// its price box then answered by om-price:PRICE), om-page:search|register|settle, om-search:CLASS,TYPE[,DEGREE] (the stall
// network's combos' indices), om-choose:N, om-buy:N (then yes), om-register (then yes), om-cancel:N (then yes), om-settle.
bool PlayScreen::OpenMarketScriptStep(const std::string& verb, const std::string& arg) {
    if (!verb.starts_with("om-")) return false;
    if (verb == "om-open") {
        std::uint32_t npc = 0;
        double nearest = 1e18;
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        for (const auto& [id, e] : view_.Entities()) {
            if (self == nullptr || e.kind != proto::EntityKind::Character || e.look.model != game::openmarket::kNpcRefObjId) continue;
            const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(e.position);
            const double d = std::hypot(a.x - b.x, a.z - b.z);
            if (d < nearest) nearest = d, npc = id;
        }
        if (npc == 0) {
            core::LogWarning("game: om-open: no {} in sight", game::openmarket::kNpcCodename);
            return true;
        }
        if (!replay_) shared_->session->SendGame(proto::OpenMarketOpen::kId, proto::Encode(proto::OpenMarketOpen{npc}));
    } else if (verb == "om-close") {
        if (!replay_) shared_->session->SendGame(proto::OpenMarketClose::kId, proto::Encode(proto::OpenMarketClose{}));
    } else if (verb == "om-put") {
        if (!OpenMarketTakesItems(hudContext_)) core::LogWarning("game: om-put: the Register page is not shown");
        else OpenMarketPut(hudContext_, std::atoi(arg.c_str()));
    } else if (verb == "om-price") {
        if (!OpenMarketPrice(hudContext_, std::strtoull(arg.c_str(), nullptr, 10))) core::LogWarning("game: om-price: no price box shown");
    } else if (!OpenMarketScript(hudContext_, verb, arg)) {
        core::LogWarning("game: {}: the market's window is not open", verb);
    }
    return true;
}

} // namespace sr::client
