// The exchange on the game screen: the Action page's Exchange (1006) and the
// target menu's, the request's yes / no box, the window shown while it is open, its Confirm (confirm,
// then approve), the system lines, and the test steps (of --guild and --fortress) that
// drive it.

#include <cstdlib>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "play/PlayScreen.h"
#include "sr/core/Log.h"

namespace sr::client {

namespace proto = game::proto;

void PlayScreen::AskExchange(ScreenContext& context) {
    // The selected player (a player, not the player itself), told "Applying for a trade to [%s]."
    const std::uint32_t target = player_.target.entity;
    if (replay_ || target == 0 || target == view_.Self() || player_.target.kind != game::TargetStatus::Kind::Player) return;
    if (ActionBarRefuses(context.time)) return; // a job suit's change, a flag pole's action
    const game::WorldView::Entity* e = view_.Find(target);
    const std::string name = e != nullptr ? e->look.name : std::string();
    player_.Post(game::GameMessage::Kind::General, FormatText(gui_.data().Text("UIIT_MSG_DEAL_ASKING"), {name}), context.time);
    shared_->session->SendGame(proto::ExchangeAsk::kId, proto::Encode(proto::ExchangeAsk{target}));
}

void PlayScreen::HandleExchange(ScreenContext& context, const proto::ExchangeEvent& event) {
    const auto text = [this](const char* key) { return gui_.data().Text(key); };
    for (const game::ExchangeView::Line& line : exchange_.Apply(event)) {
        using Kind = game::ExchangeView::Line::Kind;
        switch (line.kind) {
        case Kind::Opened:
            // The window, beside the bag (the original opens both).
            hud_.Show(hud::kExchange);
            hud_.OpenPage(hud::kPageInventory);
            player_.Post(game::GameMessage::Kind::General, FormatText(text("UIIT_MSG_EXCHANGE_START"), {line.name}), context.time);
            core::LogInfo("game: exchange with {} open", line.name);
            break;
        case Kind::Done:
            hud_.Hide(hud::kExchange);
            player_.Post(game::GameMessage::Kind::General, text("UIIT_MSG_STGRERR_ITEM_EXCHANGE_SUCCEED"), context.time);
            player_.Post(game::GameMessage::Kind::General, FormatText(text("UIIT_MSG_EXCHANGE_END"), {line.name}), context.time);
            core::LogInfo("game: exchange with {} done", line.name);
            break;
        case Kind::Ended:
        case Kind::Refused: {
            if (line.kind == Kind::Ended) hud_.Hide(hud::kExchange);
            const char* key = ExchangeCodeKey(line.code);
            if (key != nullptr) player_.Post(game::GameMessage::Kind::General, text(key), context.time);
            core::LogInfo("game: exchange {} (0x{:X})", line.kind == Kind::Ended ? "ended" : "refused", std::uint16_t(line.code));
            break;
        }
        }
    }
    // A request: the yes / no box ("[name]", "Applied for the exchange. Will you accept it?"); gone with the request
    // (the asker gave up, its time ran out).
    if (exchange_.TakeRequestGone() && exchangeAsking_) {
        exchangeAskDropped_ = true;
        hud_.AnswerConfirm(false);
    }
    if (auto request = exchange_.TakeRequest()) {
        const auto answer = [this](bool yes) {
            exchangeAsking_ = false;
            if (exchangeAskDropped_ || replay_) return;
            core::LogInfo("game: exchange request {}", yes ? "accepted" : "declined");
            shared_->session->SendGame(proto::ExchangeReply::kId, proto::Encode(proto::ExchangeReply{std::uint8_t(yes ? 1 : 0)}));
        };
        exchangeAsking_ = true;
        exchangeAskDropped_ = false;
        // The Option window's "Exchange Request" unticked (/BlockTradeInvitation too): refused without asking
        // (the asker hears "The request for a private trade has been cancelled.").
        if (context.options != nullptr && !context.options->game.Get("exchangeRequests")) {
            core::LogInfo("game: exchange request from {} refused (exchange requests are off)", request->name);
            return answer(false);
        }
        if (guildAnswer_ != 0) return answer(guildAnswer_ > 0);
        hud_.Confirm("UIIT_STT_CONFIRM_BOX", {request->name, text("UIIT_MSG_DEAL_ASK")}, answer);
    }
}

// The window's Confirm: nothing offered on either side says so; the player's offer
// open, it is confirmed ("check the other's offer" while the other has not); both confirmed, it is the
// approval ("waiting for the other's").
void PlayScreen::ExchangeConfirm() {
    if (!exchange_.Open() || replay_) return;
    const game::ExchangeView::Side& mine = exchange_.Mine();
    const game::ExchangeView::Side& theirs = exchange_.Theirs();
    const auto empty = [](const game::ExchangeView::Side& s) {
        for (const auto& i : s.items) {
            if (i) return false;
        }
        return s.gold == 0;
    };
    ui::Interface* ui = hud_.interface();
    const auto note = [&](const char* key) {
        if (ui != nullptr) ExchangeNote(*ui, gui_.data().Text(key));
    };
    if (empty(mine) && empty(theirs)) return note("UIIT_MSG_STRGERR_DEAL_FAIL_NON_ITEM");
    using Step = proto::ExchangeStep;
    if (mine.step == Step::Open) {
        if (theirs.step == Step::Open) note("UIIT_MSG_EXCHANGE_DECEPTION_REAFFIRM");
        shared_->session->SendGame(proto::ExchangeConfirm::kId, proto::Encode(proto::ExchangeConfirm{}));
    } else if (mine.step == Step::Confirmed && theirs.step != Step::Open) {
        if (theirs.step != Step::Approved) note("UIIT_MSG_EXCHANGE_AGREEMENT_WAIT");
        shared_->session->SendGame(proto::ExchangeApprove::kId, proto::Encode(proto::ExchangeApprove{}));
    }
}

// The test steps: exchange:NAME (asks that player, selected), put:PLACE, take:SLOT, gold:N, confirm (the
// window's button), exchange-cancel; a request is answered by --guild-accept / --guild-decline.
bool PlayScreen::ExchangeScriptStep(const std::string& verb, const std::string& arg) {
    const auto send = [&](const auto& message) {
        using T = std::decay_t<decltype(message)>;
        if (!replay_) shared_->session->SendGame(T::kId, proto::Encode(message));
    };
    if (verb == "exchange") {
        for (const auto& [id, e] : view_.Entities()) {
            if (e.kind == proto::EntityKind::Character && e.look.name == arg && id != view_.Self()) {
                SelectPlayer(id);
                if (!replay_) shared_->session->SendGame(proto::ExchangeAsk::kId, proto::Encode(proto::ExchangeAsk{id}));
                return true;
            }
        }
        core::LogWarning("game: exchange: {} is not in sight", arg);
    } else if (verb == "put") {
        send(proto::ExchangePut{std::uint8_t(std::strtoul(arg.c_str(), nullptr, 10))});
    } else if (verb == "take") {
        send(proto::ExchangeTake{std::uint8_t(std::strtoul(arg.c_str(), nullptr, 10))});
    } else if (verb == "gold") {
        send(proto::ExchangeGold{std::strtoull(arg.c_str(), nullptr, 10)});
    } else if (verb == "confirm") {
        ExchangeConfirm();
    } else if (verb == "exchange-cancel") {
        send(proto::ExchangeCancel{});
    } else {
        return false;
    }
    return true;
}

} // namespace sr::client
