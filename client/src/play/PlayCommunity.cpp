// Friends, the block list and letters on the game screen: the session's
// community events into game::CommunityView, their system lines, the friend proposal's yes/no box
// and the test steps (of --guild) that drive the requests.

#include <cstdlib>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "play/PlayScreen.h"
#include "sr/core/Log.h"

namespace sr::client {

namespace proto = game::proto;

void PlayScreen::HandleCommunity(ScreenContext& context, const proto::CommunityEvent& event) {
    for (const game::CommunityView::Line& line : community_.Apply(event)) {
        const std::string shown = CommunityLineText(hudContext_, line);
        if (!shown.empty()) player_.Post(game::GameMessage::Kind::General, shown, context.time);
    }
    // The envelope stands while letters wait unread.
    if (community_.Unread() > 0) {
        if (!hud_.Shown(hud::kLetterAlarm)) hud_.Show(hud::kLetterAlarm);
    } else if (hud_.Shown(hud::kLetterAlarm)) {
        hud_.Hide(hud::kLetterAlarm);
    }
    if (std::holds_alternative<proto::LetterArrived>(event) && community_.LettersListed()) {
        // The box is on the screen's mind: shown fresh.
        shared_->session->SendGame(proto::LettersAsk::kId, proto::Encode(proto::LettersAsk{}));
    }
    auto proposal = community_.TakeProposal();
    if (!proposal) return;
    const auto send = [this, request = proposal->request](bool yes) {
        shared_->session->SendGame(proto::FriendReply::kId, proto::Encode(proto::FriendReply{request, std::uint8_t(yes ? 1 : 0)}));
    };
    // Refused without asking (/BlockFriendInvitation, kept on the PC).
    if (player_.friendInvitesBlocked) return send(false);
    const auto text = [&](const char* key) { return gui_.data().Text(key); };
    const auto answered = [this, send, time = context.time, text](bool yes) {
        if (!friendAskOpen_) return;
        friendAskOpen_ = false;
        send(yes);
        player_.Post(game::GameMessage::Kind::General, text(yes ? "UIIT_MSG_FRIEND_CONCLUSION" : "UIIT_MSG_FRIEND_REFUSAL"), time);
    };
    friendAskOpen_ = true;
    // The original's has no time to answer (proto::kFriendAnswerSeconds 0): the box stays until answered.
    friendAskUntil_ = proto::kFriendAnswerSeconds > 0 ? context.time + proto::kFriendAnswerSeconds : 1e300;
    if (guildAnswer_ != 0) return answered(guildAnswer_ > 0);
    hud_.Confirm("UIIT_STT_CONFIRM_BOX", {proposal->name + " " + text("UIIT_MSG_FRIEND_REQUEST_01"), text("UIIT_MSG_FRIEND_REQUEST_02")}, answered);
}

void PlayScreen::CommunityTick(double now) {
    if (!friendAskOpen_ || now < friendAskUntil_) return;
    // Not answered in time: the question goes away, the asker has been told by now.
    friendAskOpen_ = false;
    hud_.AnswerConfirm(false);
    player_.Post(game::GameMessage::Kind::General, gui_.data().Text("UIIT_MSG_FRIEND_SYSCANCEL"), now);
}

// The test steps: friend:NAME, friend-yes, friend-no, friend-delete:NAME, block:NAME, unblock:NAME,
// letter:TO|TEXT, letters, letter-read:N, letter-open:N (the window's Read), letter-delete:N, community[:TAB].
bool PlayScreen::CommunityScriptStep(const std::string& verb, const std::string& arg, const std::string& step) {
    const auto send = [&](const auto& message) {
        using T = std::decay_t<decltype(message)>;
        shared_->session->SendGame(T::kId, proto::Encode(message));
    };
    const auto letterAt = [&]() -> const proto::LetterRecord* {
        const std::size_t index = std::size_t(std::strtoul(arg.c_str(), nullptr, 10));
        if (index >= community_.Letters().size()) {
            core::LogWarning("game: --guild {}: no such letter ({} listed)", step, community_.Letters().size());
            return nullptr;
        }
        return &community_.Letters()[index];
    };
    if (verb == "friend") {
        send(proto::FriendAsk{arg});
    } else if (verb == "friend-yes" || verb == "friend-no") {
        if (!hud_.AnswerConfirm(verb == "friend-yes")) core::LogWarning("game: --guild {}: no question open", step);
    } else if (verb == "friend-delete") {
        for (const proto::FriendRecord& f : community_.Friends()) {
            if (f.name == arg) send(proto::FriendDelete{f.character});
        }
    } else if (verb == "block") {
        send(proto::WhisperBlock{1, arg});
    } else if (verb == "unblock") {
        send(proto::WhisperBlock{2, arg});
    } else if (verb == "letter") {
        const std::size_t bar = arg.find('|');
        send(proto::LetterWrite{arg.substr(0, bar), bar == std::string::npos ? std::string() : arg.substr(bar + 1)});
    } else if (verb == "letters") {
        send(proto::LettersAsk{});
    } else if (verb == "letter-read") {
        if (const auto* r = letterAt()) send(proto::LetterOpen{r->id});
    } else if (verb == "letter-open") {
        CommunityReadLetter(hudContext_, std::atoi(arg.c_str()));
    } else if (verb == "letter-delete") {
        if (const auto* r = letterAt()) send(proto::LetterDelete{r->id});
    } else if (verb == "community") {
        ToggleCommunityPage(hudContext_, arg.empty() ? 0 : std::atoi(arg.c_str()));
    } else {
        return false;
    }
    return true;
}

} // namespace sr::client
