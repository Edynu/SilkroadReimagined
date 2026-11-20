// The Academy on the game screen: the session's academy events into the player's
// game::AcademyView (the Academy page reads it), their system lines, an invitation's yes/no box
// (UIIT_STT_TC_JOIN_REQUEST) and the test steps (of --guild) that drive the requests.

#include <cstdlib>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "play/PlayScreen.h"
#include "sr/core/Log.h"

namespace sr::client {

namespace proto = game::proto;

void PlayScreen::HandleAcademy(ScreenContext& context, const proto::AcademyEvent& event) {
    // An invitation not answered in time: its box goes away (the server told both sides).
    if (const auto* news = std::get_if<proto::AcademyNews>(&event); news != nullptr && news->kind == proto::AcademyNewsKind::NoReply && academyAskOpen_) {
        academyAskOpen_ = false;
        hud_.AnswerConfirm(false);
    }
    const auto lines = player_.academy.Apply(event, player_.self.name, context.time);
    if (std::holds_alternative<proto::AcademyAccumulation>(event) || std::holds_alternative<proto::AcademyReserve>(event) ||
        std::holds_alternative<proto::AcademyInfo>(event)) {
        player_.ShowAcademyBuffs();
    }
    for (const game::AcademyView::Line& line : lines) {
        // The screen's own texts, once they are in (lines arrive as the world is entered).
        const auto post = [this, line](double time) {
            const std::string shown = AcademyLineText([this](const std::string& key) { return gui_.data().Text(key); }, line);
            if (!shown.empty()) player_.Post(game::GameMessage::Kind::General, shown, time);
        };
        if (gui_.data().Ready()) post(context.time);
        else textLater_.push_back(post);
    }
    if (player_.academy.TakeRankingArrived()) OpenAcademyRanking(hudContext_);
    const auto ask = player_.academy.TakeAsk();
    if (!ask) return;
    // An invitation's answer, or the guardian's to a join request from the matching list.
    const bool join = ask->join != 0;
    const auto answered = [this, join](bool yes) {
        if (!academyAskOpen_) return;
        academyAskOpen_ = false;
        if (replay_) return;
        if (join) shared_->session->SendGame(proto::AcademyMatchReply::kId, proto::Encode(proto::AcademyMatchReply{std::uint8_t(yes ? 1 : 0)}));
        else shared_->session->SendGame(proto::AcademyAnswer::kId, proto::Encode(proto::AcademyAnswer{std::uint8_t(yes ? 1 : 0)}));
    };
    academyAskOpen_ = true;
    if (guildAnswer_ != 0) return answered(guildAnswer_ > 0);
    hud_.Confirm("UIIT_STT_CONFIRM_BOX", {FormatText(gui_.data().Text("UIIT_STT_TC_JOIN_REQUEST"), {ask->from})}, answered);
}

// The test steps: academy (the L page), academy-found (the request; the storage keeper's row is
// talk:NPC then npc-row:UIIT_STT_TC_OPEN, then yes), academy-invite:NAME, academy-yes, academy-no,
// academy-leave, academy-ban:NAME, academy-disband; academy-graduate[:RATING 0..4], academy-force:NAME,
// academy-notice:TITLE|TEXT, academy-rank (the nearest NPC's ranking: stand by a storage keeper),
// academy-match (the list's window), academy-register:KIND,TITLE / academy-modify:KIND,TITLE (1 apprentices,
// 2 assistants), academy-unlist, academy-join:GUARDIAN (its entry on the list's page shown), academy-pass (the accumulated
// points passed to the guardian).
bool PlayScreen::AcademyScriptStep(const std::string& verb, const std::string& arg) {
    if (!verb.starts_with("academy")) return false;
    const auto send = [&](const auto& message) {
        using T = std::decay_t<decltype(message)>;
        if (!replay_) shared_->session->SendGame(T::kId, proto::Encode(message));
    };
    if (verb == "academy") {
        hud_.OpenPage(hud::kPageAcademy);
    } else if (verb == "academy-found") {
        send(proto::AcademyFound{});
    } else if (verb == "academy-invite") {
        for (const auto& [id, e] : view_.Entities()) {
            if (e.kind == proto::EntityKind::Character && e.look.name == arg && id != view_.Self()) {
                SelectPlayer(id);
                send(proto::AcademyInvite{id});
                return true;
            }
        }
        core::LogWarning("game: academy-invite: {} is not in sight", arg);
    } else if (verb == "academy-yes" || verb == "academy-no") {
        if (!academyAskOpen_ || !hud_.AnswerConfirm(verb == "academy-yes")) core::LogWarning("game: {}: no invitation open", verb);
    } else if (verb == "academy-leave") {
        send(proto::AcademyLeave{});
    } else if (verb == "academy-ban") {
        send(proto::AcademyBan{arg});
    } else if (verb == "academy-disband") {
        send(proto::AcademyDisband{});
    } else if (verb == "academy-graduate") {
        send(proto::AcademyGraduate{std::uint8_t(arg.empty() ? 4 : std::strtoul(arg.c_str(), nullptr, 10))});
    } else if (verb == "academy-force") {
        send(proto::AcademyGraduateForce{arg});
    } else if (verb == "academy-notice") {
        const std::size_t bar = arg.find('|');
        send(proto::AcademyNoticeWrite{arg.substr(0, bar), bar == std::string::npos ? std::string() : arg.substr(bar + 1)});
    } else if (verb == "academy-rank") {
        // The nearest NPC (the server checks its reach).
        send(proto::AcademyRankingAsk{NearestNpc(false)});
    } else if (verb == "academy-match") {
        hud_.Toggle(153);
    } else if (verb == "academy-register" || verb == "academy-modify") {
        const std::size_t comma = arg.find(',');
        const auto kind = std::uint8_t(std::strtoul(arg.c_str(), nullptr, 10));
        const std::string title = comma == std::string::npos ? std::string() : arg.substr(comma + 1);
        if (verb == "academy-register") send(proto::AcademyMatchRegister{kind, title});
        else send(proto::AcademyMatchModify{kind, title});
    } else if (verb == "academy-unlist") {
        send(proto::AcademyMatchDelete{});
    } else if (verb == "academy-list") {
        send(proto::AcademyMatchListAsk{});
    } else if (verb == "academy-pass") {
        // The accumulation's pass, as its board icon's right click and the box's yes would send it.
        send(proto::AcademyPassBuff{});
    } else if (verb == "academy-join") {
        for (const proto::AcademyMatchRow& row : player_.academy.Matches().rows) {
            if (row.guardian != arg) continue;
            player_.academy.joiningGuardian = row.guardian;
            player_.academy.joiningAt = hudContext_.time;
            send(proto::AcademyMatchJoin{row.entry});
            return true;
        }
        core::LogWarning("game: academy-join: {} is not on the list's page", arg);
    } else {
        return false;
    }
    return true;
}

} // namespace sr::client
