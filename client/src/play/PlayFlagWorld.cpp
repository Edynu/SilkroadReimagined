// Capture the Flag and Event So-Ok on the game screen: the notices' lines on the green banner and in the system
// messages (game::FlagView reads them as the 1.188
// client's handler does), the event timer, the Subscription Card's box, the action bar over a pole's actor, the players'
// flag teams (in a match an enemy is another team), So-Ok's desk and her coded quests' talk in the NPC window, the death
// box's camp button and its countdown, and the test steps (of --guild and --fortress) that drive them. The teams' look
// (the default-wear slot 3 / 4, a cape inferred) is not known yet and not shown.

#include <cmath>
#include <cstdio>
#include <format>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "play/PlayScreen.h"
#include "sr/core/Log.h"

namespace sr::client {

namespace proto = game::proto;

namespace {
constexpr double kFlagScriptWait = 15.0; // ours: how long a script step waits for So-Ok's window
}

void PlayScreen::HandleEvent(ScreenContext& context, const proto::EventEvent& event) {
    if (HandleArena(context, event)) return; // the Battle Arena's (PlayArena.cpp)
    if (const auto* n = std::get_if<proto::FlagWorldNotice>(&event)) {
        const game::FlagView::Outcome out = flag_.Apply(*n, view_.Self(), context.time);
        if (out.line) FlagLine(*out.line, context.time);
        if (out.cardBox && !replay_) ShowSubscribedBox(hudContext_);
        // State 0x0A: the actor's bar, 5 s, the one a job suit's change
        // shows (jobChanges_: the same two fields in the original).
        if (out.actionBar != 0) jobChanges_[out.actionBar] = {context.time, game::FlagView::kActionSeconds};
        core::LogInfo("game: flag world: {} {:#x} (value {}, team {}{}{})", n->personal != 0 ? "code" : "state", int(n->code), n->value, int(n->team),
                      n->name.empty() ? "" : ", ", n->name);
    } else if (const auto* desk = std::get_if<proto::EventDesk>(&event)) {
        flag_.Apply(*desk);
        if (auto* npc = dynamic_cast<NpcWindow*>(hud_.interface() != nullptr ? hud_.interface()->Window(hud::kNpc) : nullptr); npc != nullptr && npc->visible) {
            npc->ShowEventDesk();
        }
        core::LogInfo("game: So-Ok's desk: state {}, {} quests", int(desk->flagState), desk->quests.size());
    } else if (const auto* says = std::get_if<proto::EventSays>(&event)) {
        auto* npc = dynamic_cast<NpcWindow*>(hud_.interface() != nullptr ? hud_.interface()->Window(hud::kNpc) : nullptr);
        if (npc != nullptr && npc->visible) {
            npc->ShowEventSays(*says);
            // sook-swap: her talk A's first row (the swap), as a click on it.
            if (flagScript_ == FlagScript::SwapRow && !says->rows.empty() && says->rows.front() != game::kTalkExit) {
                flagScript_ = FlagScript::None;
                npc->Choose(1);
            }
        }
        core::LogInfo("game: So-Ok says {} ({} rows)", says->text, says->rows.size());
    } else if (const auto* team = std::get_if<proto::EntityFlagTeam>(&event)) {
        flag_.Apply(*team);
        if (team->entity == view_.Self()) core::LogInfo("game: flag world: the player's team {}", int(team->team));
    }
}

void PlayScreen::FlagLine(const game::FlagView::Line& line, double time) {
    if (!gui_.data().Ready()) {
        textLater_.push_back([this, line](double at) { FlagLine(line, at); }); // its text once the texts are in
        return;
    }
    std::string text = gui_.data().Text(line.key);
    if (!line.name.empty()) text = FormatText(text, {line.name});
    if (line.banner) {
        // The green banner (CIFNotify 42, the quest notice's band): its "\n" escapes as spaces.
        std::string band = text;
        for (std::size_t at = band.find("\\n"); at != std::string::npos; at = band.find("\\n", at)) band.replace(at, 2, " ");
        std::replace(band.begin(), band.end(), '\n', ' ');
        player_.Notify(game::Banner::Kind::Quest, band, time);
    }
    // A system line of category 0, always shown.
    player_.Post(game::GameMessage::Kind::General, text, time);
}

bool PlayScreen::FlagFoe(std::uint32_t other) const { return flag_.Enemy(view_.Self(), other); }

bool PlayScreen::ActionBarRuns(double now) const {
    const auto it = jobChanges_.find(view_.Self());
    return it != jobChanges_.end() && now - it->second.started < double(it->second.seconds);
}

bool PlayScreen::ActionBarRefuses(double now) {
    if (!ActionBarRuns(now)) return false;
    // 0x1834 as a system line (the exchange request, a stall).
    player_.Post(game::GameMessage::Kind::General, gui_.data().Text("UIIT_MSG_STRGERR_STORAGE_OPERATION_BLOCKED"), now);
    return true;
}

void PlayScreen::FlagDeath(double now) {
    (void)now;
    // The death box's first button, pressed in a match: the ordinary answer (Resurrect kind 1), then the camp countdown when
    // none is open.
    if (flag_.InMatch(view_.Self())) OpenCampCountdown(hudContext_);
}

void PlayScreen::FlagFrame(ScreenContext& context) {
    // The event timer (window 410): shown and hidden by the view's state.
    const bool timer = flag_.TimerShown(context.time) || arena_.TimerShown(context.time); // the arena's too (PlayArena.cpp)
    if (timer != flagTimerShown_) {
        flagTimerShown_ = timer;
        if (timer) hud_.Show(hud::kTimer);
        else hud_.Hide(hud::kTimer);
    }
    ArenaFrame(context);
    if (flagScript_ == FlagScript::None || replay_) return;
    if (context.time > flagScriptUntil_) {
        core::LogWarning("game: flag world: So-Ok's window did not come");
        flagScript_ = FlagScript::None;
        return;
    }
    auto* npc = dynamic_cast<NpcWindow*>(hud_.interface() != nullptr ? hud_.interface()->Window(hud::kNpc) : nullptr);
    const proto::EventDesk& desk = flag_.Desk();
    if (npc == nullptr || !npc->visible || desk.npc == 0 || npc->npc() != desk.npc) return;
    if (flagScript_ == FlagScript::Register) {
        flagScript_ = FlagScript::None;
        const std::string_view key = game::FlagView::DeskRowKey(desk.flagState);
        if (key.empty() || !npc->ChooseText(gui_.data().Text(std::string(key)))) core::LogWarning("game: ctf-register: no registration row (state {})", int(desk.flagState));
    } else if (flagScript_ == FlagScript::Swap) {
        // Her talk, then the swap's quest row (its EventTalk 0xFF); talk A's row 1 when it comes.
        npc->ChooseText(gui_.data().Text("UIIT_STT_NPC_CHATTING_WND_TALKSTART"));
        flagScript_ = FlagScript::None;
        for (const proto::EventDesk::Row& row : desk.quests) {
            if (row.quest != game::SwapIceTrophies().id) continue;
            if (npc->ChooseText(gui_.data().Text(row.title))) flagScript_ = FlagScript::SwapRow;
        }
        if (flagScript_ != FlagScript::SwapRow) core::LogWarning("game: sook-swap: no swap row at So-Ok");
    }
}

std::uint32_t PlayScreen::NearestByCodename(const std::function<bool(std::string_view)>& match) const {
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    if (self == nullptr || !tables_ || !tables_->Usable()) return 0;
    std::uint32_t best = 0;
    double nearest = 1e18;
    for (const auto& [id, e] : view_.Entities()) {
        const assets::ObjectRow* row = e.kind == proto::EntityKind::Character ? tables_->characters.Find(e.look.model) : nullptr;
        if (row == nullptr || !match(row->codename)) continue;
        const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(e.position);
        if (const double d = std::hypot(a.x - b.x, a.z - b.z); d < nearest) {
            nearest = d;
            best = id;
        }
    }
    return best;
}

// The test steps (--guild and --fortress): ctf-register (So-Ok's registration row, or its cancel; her window opened first
// when it is not), ctf-dev:N (the developer's switch: 1 open, 2 start, 3 end, 4 cancel), ctf-key, ctf-pole:N (0 white,
// 1 red, 2 blue), ctf-flag (the dropped flag), ctf-camp (the death box's camp button), ctf-ok (the card's box's Confirm),
// sook-swap (her swap's quest, then its row 1). The poles, the key and the flag are NPCs: talking to one is its select
// (the server walks the player up to a pole).
bool PlayScreen::FlagScriptStep(ScreenContext& context, const std::string& verb, const std::string& arg) {
    if (verb == "snap") { // any script's picture of the screen now (--snap-dir DIR), the run going on
        snapName_ = arg.empty() ? "snap" : arg;
        return true;
    }
    if (ArenaScriptStep(context, verb, arg)) return true; // arena-* (PlayArena.cpp)
    if (!verb.starts_with("ctf-") && verb != "sook-swap") return false;
    if (replay_ || !shared_ || !shared_->session) return true;
    auto& session = *shared_->session;
    const auto talk = [&](std::uint32_t npc, const char* what) {
        if (npc == 0) core::LogWarning("game: {}: no {} in sight", verb, what);
        else session.TalkToNpc(npc);
    };
    if (verb == "ctf-register" || verb == "sook-swap") {
        flagScript_ = verb == "ctf-register" ? FlagScript::Register : FlagScript::Swap;
        flagScriptUntil_ = context.time + kFlagScriptWait;
        auto* npc = dynamic_cast<NpcWindow*>(hud_.interface() != nullptr ? hud_.interface()->Window(hud::kNpc) : nullptr);
        if (npc != nullptr && npc->visible && flag_.Desk().npc != 0 && npc->npc() == flag_.Desk().npc) return true; // FlagFrame presses it
        // Her window first, as a click on her: walked up to when far, talked to when near (PlayScreen::Npcs).
        const std::uint32_t soOk = NearestByCodename([](std::string_view c) { return c == game::flag::kSoOk; });
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        const game::WorldView::Entity* her = view_.Find(soOk);
        if (soOk == 0 || self == nullptr || her == nullptr) {
            core::LogWarning("game: {}: no Event So-Ok in sight", verb);
            flagScript_ = FlagScript::None;
            return true;
        }
        pendingNpc_ = soOk;
        pendingUntil_ = context.time + 10.0;
        pendingGate_ = 0;
        const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(her->position);
        const double length = std::max(1.0, std::hypot(a.x - b.x, a.z - b.z));
        if (length > 200.0) session.MoveTo(proto::FromFlat({b.x + (a.x - b.x) / length * 60.0, b.z + (a.z - b.z) / length * 60.0}, her->position.y));
    } else if (verb == "ctf-dev") {
        session.SendGame(proto::FlagWorldDev::kId, proto::Encode(proto::FlagWorldDev{std::uint8_t(std::atoi(arg.c_str()))}));
    } else if (verb == "ctf-key") {
        talk(NearestByCodename([](std::string_view c) { return c == game::flag::kKey; }), "key");
    } else if (verb == "ctf-flag") {
        talk(NearestByCodename([](std::string_view c) { return c == game::flag::kDroppedFlag; }), "dropped flag");
    } else if (verb == "ctf-pole") {
        const int pole = std::clamp(std::atoi(arg.c_str()), 0, 2);
        talk(NearestByCodename([pole](std::string_view c) { return c == game::flag::kPoleCodenames[pole] || c == game::flag::kFlapCodenames[pole]; }), "such pole");
    } else if (verb == "ctf-walk") {
        // ctf-walk:X,Z (ours, for tries): a straight walk to X,Z of the region it stands in, the step again each second until
        // within 20 (40 times at most): the event map's corridors by their waypoints.
        float x = 0, z = 0;
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        if (self != nullptr && std::sscanf(arg.c_str(), "%f,%f", &x, &z) == 2) {
            const double d = std::hypot(double(self->position.x) - x, double(self->position.z) - z);
            core::LogInfo("game: ctf-walk to {:.0f},{:.0f}: {:.0f} from ({:.0f}, {:.0f})", x, z, d, self->position.x, self->position.z);
            if (d > 20.0 && ++flagWalkTries_ < 40) {
                proto::WorldPosition to = self->position;
                to.x = x;
                to.z = z;
                session.MoveTo(to);
                guildScript_.insert(guildScript_.begin(), "ctf-walk:" + arg);
                guildNext_ = context.time + 1.0;
            } else {
                flagWalkTries_ = 0;
            }
        }
    } else if (verb == "ctf-camp") {
        if (!hud_.PressRebirth(200)) core::LogWarning("game: ctf-camp: no death box");
    } else if (verb == "ctf-ok") {
        if (!AnswerInformBox(hudContext_)) core::LogWarning("game: ctf-ok: no box");
    } else {
        core::LogWarning("game: {}: not a step", verb);
    }
    return true;
}

} // namespace sr::client
