// The game screen's side of parties and party matching: the party's lines
// into the system messages, the request box, the matching boxes, the HUD's party requests, a right
// click on a player, and the scripted tries (src/main.cpp lists the options).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gui/HudParty.h"
#include "play/PlayScreen.h"
#include "sr/core/Log.h"
#include "sr/game/Party.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

// "exp,item,anyone" -> the settings bits.
std::uint8_t SettingsOf(const std::string& list) {
    std::uint8_t s = 0;
    if (list.find("exp") != std::string::npos) s |= game::kPartyExpShare;
    if (list.find("item") != std::string::npos) s |= game::kPartyItemShare;
    if (list.find("anyone") != std::string::npos) s |= game::kPartyAnyoneInvites;
    return s;
}

} // namespace

void PlayScreen::PartyEnter(ScreenContext& context) {
    const core::Config& c = context.config;
    PartyTries& t = partyTries_;
    t.invite = c.GetString("party-invite", "");
    t.inviteSettings = SettingsOf(c.GetString("party-settings", "exp,item"));
    if (c.GetBool("party-accept", false)) t.answer = 1;
    if (c.GetBool("party-decline", false)) t.answer = 0;
    t.answerAfter = float(c.GetDouble("party-answer-after", 1.0));
    t.kick = c.GetString("party-kick", "");
    t.kickAfter = float(c.GetDouble("party-kick-after", t.kick.empty() ? -1.0 : 3.0));
    t.leaveAfter = float(c.GetDouble("party-leave-after", -1.0));
    if (const std::string set = c.GetString("party-set", ""); !set.empty()) {
        t.setSettings = SettingsOf(set);
        t.setAfter = float(c.GetDouble("party-set-after", 3.0));
    }
    t.page = c.GetBool("party-page", false);
    t.match = c.GetBool("party-match", false);
    if (const std::string title = c.GetString("match-register", ""); !title.empty()) {
        t.registerListing = true;
        t.form.title = title;
        t.form.purpose = std::uint8_t(std::clamp(c.GetInt("match-purpose", 0), 0, game::kPartyPurposes - 1));
        int low = 1, high = 120;
        if (const std::string levels = c.GetString("match-levels", ""); !levels.empty()) std::sscanf(levels.c_str(), "%d,%d", &low, &high);
        t.form.levelMin = std::uint8_t(std::clamp(low, 1, 255));
        t.form.levelMax = std::uint8_t(std::clamp(high, low, 255));
        t.form.settings = SettingsOf(c.GetString("match-settings", "exp,item"));
    }
    t.matchAfter = float(c.GetDouble("match-after", 1.0));
    t.matchJoin = c.GetString("match-join", "");
    if (c.GetBool("match-accept", false)) t.matchAnswer = 1;
    if (c.GetBool("match-refuse", false)) t.matchAnswer = 0;
    t.targetMenu = c.GetString("target-menu", "");
}

void PlayScreen::PartyFrame(ScreenContext& context) {
    player_.now = context.time;
    game::PartyView& party = player_.party;
    ui::Interface* ui = gui_.interface();
    // The party's lines: their texts with the names (an item's name from its refId) put in.
    for (const game::PartyView::Notice& n : party.notices) {
        if (n.key.empty()) continue;
        std::vector<std::string> args;
        for (const std::string& a : n.args) {
            if (a.rfind("#item:", 0) == 0) {
                const std::uint32_t ref = std::uint32_t(std::strtoul(a.c_str() + 6, nullptr, 10));
                const assets::ItemRow* row = tables_ && tables_->Usable() ? tables_->items.Find(ref) : nullptr;
                args.push_back(row != nullptr ? gui_.data().Text(row->nameKey) : a.substr(6));
            } else {
                args.push_back(a);
            }
        }
        std::string text = gui_.data().Text(n.key);
        if (args.size() == 1) text = FormatText(text, {args[0]});
        else if (args.size() == 2) text = FormatText(text, {args[0], args[1]});
        else if (args.size() >= 3) text = FormatText(text, {args[0], args[1], args[2]});
        core::LogInfo("game: party: {}", text);
        player_.Post(game::GameMessage::Kind::Party, text, context.time);
    }
    party.notices.clear();
    if (ui == nullptr || !hud_.Created()) return;
    // A request to party: the yes / no box (who asks, the party's settings, "Accept
    // party invitation?"); gone with the request (its time ran out).
    // The Option window's "Party Invitation" unticked: refused without asking, as the original's.
    if (party.invitation && !partyAsking_ && !replay_ && context.options != nullptr && !context.options->game.Get("partyInvites")) {
        core::LogInfo("game: party request from {} refused (party invitations are off)", party.invitation->name);
        party.invitation.reset();
        shared_->session->AnswerPartyInvite(false);
    }
    if (party.invitation && !partyAsking_) {
        const proto::PartyInvited& asked = *party.invitation;
        UiData& d = gui_.data();
        std::vector<std::string> lines{FormatText(d.Text("UIIT_STT_PARTY_SOMEUSER"), {asked.name}),
                                       PartyExpText(hudContext_, asked.settings) + ", " + PartyItemText(hudContext_, asked.settings)};
        if ((asked.settings & game::kPartyAnyoneInvites) != 0) lines.push_back(d.Text("UIIT_STT_PARTY_JOIN_ANYONE"));
        lines.push_back(d.Text("UIIT_STT_PARTY_PROPOSAL_ASK"));
        partyAsking_ = true;
        partyAskDropped_ = false;
        hud_.Confirm("UIIT_STT_CONFIRM_BOX", lines, [this](bool yes) {
            partyAsking_ = false;
            if (partyAskDropped_ || replay_) return;
            player_.party.invitation.reset();
            core::LogInfo("game: party request {}", yes ? "accepted" : "declined");
            shared_->session->AnswerPartyInvite(yes);
        });
        if (partyTries_.answer >= 0) partyTries_.answerAt = context.time + partyTries_.answerAfter;
    } else if (!party.invitation && partyAsking_) {
        partyAskDropped_ = true; // timed out: the box goes, nothing sent
        hud_.AnswerConfirm(false);
    }
    SyncPartyMatchBoxes(*ui, hudContext_);
    SyncAcademyProgress(*ui, hudContext_);
    if (party.joinRequest && partyTries_.matchAnswer >= 0 && partyTries_.matchAnswerAt < 0) partyTries_.matchAnswerAt = context.time + 1.0;
    // The listings again when the player's own listing or party changed (the matching window open).
    if ((party.ownEntry != partyOwnSeen_ || party.party != partySeen_) && hud_.Shown(hud::kPartyMatch) && !replay_) {
        shared_->session->ListPartyMatches({party.listing.page, 0xFF, 0, 0, ""});
    }
    partyOwnSeen_ = party.ownEntry;
    partySeen_ = party.party;
    PartyScript(context);
}

bool PlayScreen::PartyRequest(const HudRequest& r) {
    using K = HudRequest::Kind;
    const bool live = !replay_ && shared_ && shared_->session;
    game::TitleSession* s = live ? shared_->session.get() : nullptr;
    switch (r.kind) {
    case K::PartyInvite:
        core::LogInfo("game: party: asking {} (settings {})", r.value, r.extra);
        // "[%s] is requesting to party." with the asked one's name, a Party line, as the original's client says it.
        if (const game::WorldView::Entity* asked = view_.Find(std::uint32_t(r.value)); asked != nullptr && gui_.data().Ready()) {
            player_.Post(game::GameMessage::Kind::Party, FormatText(gui_.data().Text("UIIT_STT_PARTY_PROPOSAL_PENDING"), {asked->look.name}), hudContext_.time);
        }
        if (s) s->PartyInvite(std::uint32_t(r.value), std::uint8_t(r.extra));
        return true;
    case K::PartyAnswer:
        if (s) s->AnswerPartyInvite(r.value != 0);
        return true;
    case K::PartyLeave:
        if (s) s->LeaveParty();
        return true;
    case K::PartyKick:
        if (s) s->KickPartyMember(std::uint32_t(r.value));
        return true;
    case K::PartySettings:
        if (s && player_.party.InParty()) s->SetPartySettings(std::uint8_t(r.value));
        return true;
    case K::PartyMatchList:
        if (s) {
            s->ListPartyMatches({std::uint8_t(std::clamp(r.value, 0, 255)), std::uint8_t(r.extra & 0xFF), std::uint8_t((r.extra >> 8) & 0xFF),
                                 std::uint8_t((r.extra >> 16) & 0xFF), r.text});
        }
        return true;
    case K::PartyMatchRegister:
        if (s) {
            s->RegisterPartyMatch({std::uint8_t(r.value & 0xFF), std::uint8_t((r.value >> 8) & 0xFF), std::uint8_t(r.extra & 0xFF),
                                   std::uint8_t((r.extra >> 8) & 0xFF), r.text});
        }
        return true;
    case K::PartyMatchDelete:
        if (s) s->DeletePartyMatch();
        return true;
    case K::PartyMatchJoin:
        if (s) s->JoinPartyMatch(std::uint32_t(r.value));
        return true;
    case K::PartyMatchAnswer:
        if (s) s->AnswerPartyMatchJoin(r.value != 0);
        return true;
    case K::SelectEntity:
        if (view_.Find(std::uint32_t(r.value)) != nullptr) SelectPlayer(std::uint32_t(r.value));
        return true;
    case K::TargetPartyMember: {
        // Ctrl + 2..8: the other members in the party window's order, when in sight.
        const std::vector<const proto::PartyMemberRecord*> others = player_.party.Others();
        if (r.value >= 1 && std::size_t(r.value) <= others.size()) {
            const std::uint32_t entity = others[std::size_t(r.value - 1)]->entity;
            if (view_.Find(entity) != nullptr) SelectPlayer(entity);
        }
        return true;
    }
    default: return false;
    }
}

void PlayScreen::PartyRightClick(ScreenContext& context, const ui::Input& in) {
    // With the Game page's "Activate action short cut" on, a right click on another player: selected, its menu at the
    // mouse (the 1.188 client's CNIFTargetMenu). While the right button turns the camera, camera method 1 cancels it when
    // the mouse moved over 5 pixels either way; method 2 (the cursor held) takes the press's place, unless a frame
    // pushed it over 5 pixels (CameraInput). While the wheel button turns, any right click.
    if (!in.Released(ui::MouseButton::Right) || gui_.interface() == nullptr || replay_) return;
    const bool armed = targetMenuArmed_;
    targetMenuArmed_ = false;
    if (!armed || context.options == nullptr) return;
    const bool rightTurns = context.options->input.mouseMode == 0;
    const ui::Vec2 from = in.PressPosition(ui::MouseButton::Right);
    ui::Vec2 at = in.mouse();
    if (rightTurns && context.options->game.Get("cameraMethod2")) {
        at = from;
    } else if (rightTurns && (std::abs(at.x - from.x) > 5.0f || std::abs(at.y - from.y) > 5.0f)) {
        return;
    }
    const float width = float(context.device.SwapchainWidth()), height = float(context.device.SwapchainHeight());
    const std::uint32_t other = PlayerUnder(at.x, at.y, width, height);
    const game::WorldView::Entity* e = other != 0 ? view_.Find(other) : nullptr;
    if (e == nullptr) return;
    SelectPlayer(other);
    OpenTargetMenu(*gui_.interface(), hudContext_, other, e->look.name, at);
}

void PlayScreen::PartyScript(ScreenContext& context) {
    PartyTries& t = partyTries_;
    ui::Interface* ui = gui_.interface();
    if (replay_ || ui == nullptr || !shared_ || !shared_->session || view_.Find(view_.Self()) == nullptr) return;
    game::TitleSession& s = *shared_->session;
    game::PartyView& party = player_.party;
    const float dt = float(context.dt);
    // A player of a name in sight (0: none).
    const auto named = [&](const std::string& name) -> std::uint32_t {
        for (const auto& [id, e] : view_.Entities()) {
            if (id != view_.Self() && e.kind == proto::EntityKind::Character && e.look.name == name) return id;
        }
        return 0;
    };
    if (t.page) {
        hud_.OpenPage(hud::kPageParty);
        t.page = false;
    }
    if (t.match && !hud_.Shown(hud::kPartyMatch)) {
        hud_.Show(hud::kPartyMatch);
        t.match = false;
    }
    // --party-invite NAME: once it is in sight (again when a request's 30 s ran out with no party).
    if (!t.invite.empty() && !party.InParty() && context.time >= t.nextTry) {
        t.nextTry = context.time + 3.0;
        if (const std::uint32_t target = named(t.invite)) {
            t.nextTry = context.time + game::kPartyProposalSeconds + 2.0;
            core::LogInfo("game: --party-invite {} (settings {})", t.invite, t.inviteSettings);
            party.preferred = t.inviteSettings;
            s.PartyInvite(target, t.inviteSettings);
        }
    }
    if (!t.invite.empty() && party.members.size() >= 2) t.invite.clear();
    // --party-accept / --party-decline: the request box answered after a moment (a capture shows it).
    if (t.answerAt >= 0 && context.time >= t.answerAt) {
        t.answerAt = -1;
        core::LogInfo("game: --party-{}", t.answer == 1 ? "accept" : "decline");
        hud_.AnswerConfirm(t.answer == 1);
    }
    // --party-kick NAME after S seconds in a party; --party-leave-after S; --party-set LIST after S.
    if (!t.kick.empty() && party.InParty() && (t.kickAfter -= dt) < 0.0f) {
        for (const proto::PartyMemberRecord& m : party.members) {
            if (m.name == t.kick) {
                core::LogInfo("game: --party-kick {}", t.kick);
                s.KickPartyMember(m.id);
            }
        }
        t.kick.clear();
    }
    if (t.leaveAfter >= 0.0f && party.InParty() && (t.leaveAfter -= dt) < 0.0f) {
        core::LogInfo("game: --party-leave-after");
        s.LeaveParty();
    }
    if (t.setAfter >= 0.0f && party.InParty() && (t.setAfter -= dt) < 0.0f) {
        core::LogInfo("game: --party-set {}", t.setSettings);
        s.SetPartySettings(t.setSettings);
    }
    // --match-register TITLE: the listing made after a moment; --match-join NAME: the listings asked
    // for every 2 s until one of that leader shows, then a request to join it.
    if (t.registerListing && (t.matchAfter -= dt) < 0.0f) {
        t.registerListing = false;
        core::LogInfo("game: --match-register '{}'", t.form.title);
        party.ownForm = t.form;
        s.RegisterPartyMatch(t.form);
    }
    if (!t.matchJoin.empty() && !party.InParty() && party.joining == 0 && context.time >= t.nextTry) {
        t.nextTry = context.time + 2.0;
        bool asked = false;
        for (const proto::PartyMatchEntry& e : party.listing.entries) {
            if (e.name != t.matchJoin) continue;
            core::LogInfo("game: --match-join {} (listing {})", t.matchJoin, e.entry);
            s.JoinPartyMatch(e.entry);
            asked = true;
            t.matchJoin.clear();
            break;
        }
        if (!asked) s.ListPartyMatches({0, 0xFF, 0, 0, ""});
    }
    // --match-accept / --match-refuse: the leader's request box answered after a moment.
    if (t.matchAnswerAt >= 0 && context.time >= t.matchAnswerAt && party.joinRequest) {
        t.matchAnswerAt = -1;
        core::LogInfo("game: --match-{}", t.matchAnswer == 1 ? "accept" : "refuse");
        s.AnswerPartyMatchJoin(t.matchAnswer == 1);
        party.joinRequest.reset();
    }
    // --target-menu NAME: that player's menu, at the screen's middle (captures).
    if (!t.targetMenu.empty()) {
        if (const std::uint32_t target = named(t.targetMenu)) {
            SelectPlayer(target);
            const ui::Vec2 screen = ui->screen();
            OpenTargetMenu(*ui, hudContext_, target, t.targetMenu, {screen.x * 0.5f, screen.y * 0.5f});
            t.targetMenu.clear();
        }
    }
    // Captures wait while a scripted try is still to come.
    if (!t.invite.empty() || t.answerAt >= 0 || !t.matchJoin.empty() || t.registerListing || t.matchAnswerAt >= 0) context.holdCapture = true;
}

} // namespace sr::client
