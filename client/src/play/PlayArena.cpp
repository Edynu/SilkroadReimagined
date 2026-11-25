// The Battle Arena on the game screen: the notices' lines in the system messages
// and the big notice (game::ArenaView reads them as the 1.188 client's handlers do), the score, rank and result windows
// kept as the view has them, the event timer (CIFTimerWnd, shared with Capture the Flag), the 5-second gauge of a pole's
// actor, the points' floating number, the manager's desk in the NPC window, and the test steps (of --guild and
// --fortress) that drive them. The arena's teams are the flag teams (EntityFlagTeam): in a match an enemy is another team.

#include <cmath>
#include <format>
#include <sstream>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "play/PlayScreen.h"
#include "sr/core/Log.h"

namespace sr::client {

namespace proto = game::proto;

namespace {
constexpr double kArenaScriptWait = 15.0; // ours: how long a script step waits for the manager's window
}

bool PlayScreen::HandleArena(ScreenContext& context, const proto::EventEvent& event) {
    if (const auto* n = std::get_if<proto::ArenaNotice>(&event)) {
        const game::ArenaView::Outcome out = arena_.Apply(*n, context.time);
        for (const game::ArenaView::Line& line : out.lines) ArenaLine(line, context.time);
        // FF 82: the actor's 5-second gauge ((5); the original's own character, the actor being the one
        // told; ours tells those around and draws it over the actor), the bar a job suit's change shows.
        if (out.actionBar != 0) jobChanges_[out.actionBar] = {context.time, game::ArenaView::kActionSeconds};
        // FF 40: a floating number over the player (through the hits' number), blue (the original's).
        if (out.points != 0) {
            if (const game::WorldView::Entity* self = view_.Find(view_.Self())) {
                damageTexts_.push_back({self->position, out.points, false, ui::Argb(0xFF3A3AFF), context.time});
            }
        }
        core::LogInfo("game: arena: {} {:#x} (mode {}, kind {}, kinds {:#x}, value {}, {}{}{})", n->personal != 0 ? "code" : "state", int(n->code), int(n->mode),
                      int(n->kind), n->kinds, n->value, n->value2, n->name.empty() ? "" : ", ", n->name);
        return true;
    }
    if (const auto* desk = std::get_if<proto::ArenaDesk>(&event)) {
        arena_.Apply(*desk);
        if (auto* npc = dynamic_cast<NpcWindow*>(hud_.interface() != nullptr ? hud_.interface()->Window(hud::kNpc) : nullptr); npc != nullptr && npc->visible) {
            npc->ShowArenaDesk();
        }
        core::LogInfo("game: arena manager's desk: state {:#x}", desk->state);
        return true;
    }
    if (const auto* scores = std::get_if<proto::ArenaScores>(&event)) {
        arena_.Apply(*scores);
        core::LogInfo("game: arena scores {} : {} ({} in the top)", scores->team0, scores->team1, scores->top.size());
        return true;
    }
    if (const auto* result = std::get_if<proto::ArenaResult>(&event)) {
        arena_.Apply(*result);
        core::LogInfo("game: arena result: outcome {}, {} coins, {} skill experience ({} : {})", int(result->outcome), int(result->coins), result->sp,
                      result->team0, result->team1);
        return true;
    }
    return false;
}

void PlayScreen::ArenaLine(const game::ArenaView::Line& line, double time) {
    const std::string text = ArenaLineText(hudContext_, line);
    if (line.notice) {
        // The big notice, as a notice's band.
        player_.Notify(game::Banner::Kind::Notice, text, time);
        return;
    }
    // A system line of category 0; the table's "\n" escapes break it into lines.
    std::size_t start = 0;
    for (std::size_t at = text.find("\\n"); ; at = text.find("\\n", start)) {
        const std::string part = text.substr(start, at == std::string::npos ? std::string::npos : at - start);
        if (!part.empty()) player_.Post(game::GameMessage::Kind::General, part, time);
        if (at == std::string::npos) break;
        start = at + 2;
    }
}

bool PlayScreen::TalkToNearest(ScreenContext& context, std::string_view codename, const std::string& verb) {
    // Its window first, as a click on it: walked up to when far, talked to when near (PlayScreen::Npcs).
    const std::uint32_t npc = NearestByCodename([codename](std::string_view c) { return c == codename; });
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    const game::WorldView::Entity* it = view_.Find(npc);
    if (npc == 0 || self == nullptr || it == nullptr) {
        core::LogWarning("game: {}: no {} in sight", verb, codename);
        return false;
    }
    pendingNpc_ = npc;
    pendingUntil_ = context.time + 10.0;
    pendingGate_ = 0;
    const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(it->position);
    const double length = std::max(1.0, std::hypot(a.x - b.x, a.z - b.z));
    if (length > 200.0 && shared_ && shared_->session) {
        shared_->session->MoveTo(proto::FromFlat({b.x + (a.x - b.x) / length * 60.0, b.z + (a.z - b.z) / length * 60.0}, it->position.y));
    }
    return true;
}

void PlayScreen::ArenaFrame(ScreenContext& context) {
    ArenaWindows(hudContext_); // the score, rank and result windows as the view has them (HudArena.cpp)
    if (arenaScript_ == ArenaScript::None || replay_) return;
    if (context.time > arenaScriptUntil_) {
        core::LogWarning("game: arena: the manager's window did not come");
        arenaScript_ = ArenaScript::None;
        return;
    }
    auto* npc = dynamic_cast<NpcWindow*>(hud_.interface() != nullptr ? hud_.interface()->Window(hud::kNpc) : nullptr);
    const proto::ArenaDesk& desk = arena_.Desk();
    if (npc == nullptr || !npc->visible || desk.npc == 0 || npc->npc() != desk.npc) return;
    const ArenaScript script = std::exchange(arenaScript_, ArenaScript::None);
    const auto row = game::ArenaView::DeskRowOf(desk.state);
    if (!row) {
        core::LogWarning("game: arena: the manager offers nothing (state {:#x})", desk.state);
        return;
    }
    if (script == ArenaScript::Cancel) {
        if (!row->cancel) core::LogWarning("game: arena-cancel: not registered (state {:#x})", desk.state);
        else npc->ChooseText(gui_.data().Text(row->key));
        return;
    }
    if (row->cancel) {
        core::LogWarning("game: arena-register: registered already");
        return;
    }
    // The mode's row, its talk's "Arena Registration", then the rule window's kind, Register and Yes (the guild mode: its
    // guild window open, for arena-guild).
    if (!npc->ChooseText(gui_.data().Text(row->key)) || !npc->ChooseText(gui_.data().Text("UIIT_STT_ARENA_NPC_JOIN"))) {
        core::LogWarning("game: arena-register: no registration row");
        return;
    }
    if (row->mode == std::uint8_t(game::arena::Mode::Guild)) return;
    if (!ArenaRegisterKind(hudContext_, arenaScriptKind_)) core::LogWarning("game: arena-register: no such kind offered ({:#x})", row->kinds);
    else hud_.AnswerConfirm(true);
}

// The test steps (--guild and --fortress): arena-dev:ACTION[,MODE] (the developer's switch: 1 open the registration, 2
// start the play, 3 end it, 4 cancel everything), arena-register[:KIND] (the nearest NPC_BATTLE_ARENA_MANAGER's mode row,
// its "Arena Registration", the rule window's kind (1 score, the default; 2 flag), Register, Yes; with the rule window open
// already (after arena-guild), its kind, Register, Yes), arena-cancel (its "Cancel Registration"), arena-guild:NAME,NAME
// (the guild window: those moved right, Confirm), arena-ok (the result window closed).
bool PlayScreen::ArenaScriptStep(ScreenContext& context, const std::string& verb, const std::string& arg) {
    if (!verb.starts_with("arena-")) return false;
    if (replay_ || !shared_ || !shared_->session) return true;
    auto& session = *shared_->session;
    if (verb == "arena-dev") {
        proto::ArenaDev dev;
        dev.action = std::uint8_t(std::atoi(arg.c_str()));
        if (const std::size_t comma = arg.find(','); comma != std::string::npos) dev.mode = std::uint8_t(std::atoi(arg.c_str() + comma + 1));
        session.SendGame(proto::ArenaDev::kId, proto::Encode(dev));
    } else if (verb == "arena-register" || verb == "arena-cancel") {
        const std::uint8_t kind = arg.empty() ? game::arena::kScore : std::uint8_t(std::atoi(arg.c_str()));
        if (verb == "arena-register" && ArenaRuleShown(hudContext_)) {
            if (!ArenaRegisterKind(hudContext_, kind)) core::LogWarning("game: arena-register: no such kind offered");
            else hud_.AnswerConfirm(true);
            return true;
        }
        arenaScript_ = verb == "arena-register" ? ArenaScript::Register : ArenaScript::Cancel;
        arenaScriptKind_ = kind;
        arenaScriptUntil_ = context.time + kArenaScriptWait;
        auto* npc = dynamic_cast<NpcWindow*>(hud_.interface() != nullptr ? hud_.interface()->Window(hud::kNpc) : nullptr);
        if (npc != nullptr && npc->visible && arena_.Desk().npc != 0 && npc->npc() == arena_.Desk().npc) return true; // ArenaFrame goes on
        if (!TalkToNearest(context, game::arena::kManager, verb)) arenaScript_ = ArenaScript::None;
    } else if (verb == "arena-guild") {
        std::vector<std::string> names;
        std::stringstream list(arg);
        for (std::string name; std::getline(list, name, ',');) {
            if (!name.empty()) names.push_back(name);
        }
        if (!ArenaGuildChoose(hudContext_, names, true)) core::LogWarning("game: arena-guild: no guild window, or not every member found");
    } else if (verb == "arena-ok") {
        if (!CloseArenaResult(hudContext_)) core::LogWarning("game: arena-ok: no result window");
    } else {
        core::LogWarning("game: {}: not a step", verb);
    }
    return true;
}

} // namespace sr::client
