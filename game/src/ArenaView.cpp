#include "sr/game/ArenaView.h"

#include <algorithm>

namespace sr::game {

namespace {

using Line = ArenaView::Line;
using Arg = ArenaView::Arg;

Line Say(const char* key, std::vector<Arg> args = {}) { return {key, std::move(args), false}; }
Arg Key(std::string_view key) { return {std::string(key), true}; }
Arg Literal(std::string text) { return {std::move(text), false}; }

// A mode's name, or none past the four (the original leaves its %s empty then).
Arg ModeArg(std::uint8_t mode) { return mode < 4 ? Key(arena::ModeName(arena::Mode(mode))) : Literal({}); }

// The kinds' bits in the broadcasts' order: score (0x20), then flag (0x40).
template <typename F>
void ForKinds(std::uint16_t kinds, F&& f) {
    if ((kinds & arena::kScoreBit) != 0) f(arena::kScore);
    if ((kinds & arena::kFlagBit) != 0) f(arena::kFlag);
}

} // namespace

void ArenaView::Enter(std::uint8_t mode, std::uint8_t kind) {
    // The state with its windows made and hidden ((0)); a state already there keeps
    // its windows (the original asserts on a second score window and keeps the first).
    if (!InArena()) {
        team0_ = team1_ = 0;
        top_.clear();
        shown_ = rankShown_ = false;
    }
    mode_ = mode;
    kind_ = kind;
}

void ArenaView::End() {
    // The stop: the score, rank and result windows destroyed, the state cleared.
    mode_ = kind_ = 0xFF;
    shown_ = rankShown_ = false;
    team0_ = team1_ = 0;
    top_.clear();
    result_.reset();
}

ArenaView::Outcome ArenaView::Apply(const proto::ArenaNotice& n, double now) {
    Outcome out;
    if (n.personal == 0) {
        // A broadcast.
        switch (n.code) {
        case 0:
        case 1:
        case 2:
            // Registration has begun: a line for each kind offered, the big notice, the way to take part.
            ForKinds(n.kinds, [&](std::uint8_t kind) {
                out.lines.push_back(Say("UIIT_MGS_ARENA_JOIN_GUIDANCE_DISPLAY", {ModeArg(n.mode), Key(arena::KindName(kind))}));
            });
            out.lines.push_back({"UIIT_MGS_ARENA_JOIN_GUIDANCE_DISPLAY_NOTIFY", {}, true});
            out.lines.push_back(Say("UIIT_MGS_ARENA_JOIN_GUIDANCE"));
            break;
        case 3: out.lines.push_back(Say("UIIT_MGS_ARENA_JOIN_WINDOW_COMPLETION")); break;
        case 4: Enter(n.mode, n.kind); break;
        case 5:
            End();
            timerShown_ = false; // ours: the timer goes with the state (the original's runs out by itself)
            out.lines.push_back(Say("UIIT_MGS_ARENA_END"));
            break;
        case 8:
            // The play: the windows shown, the timer set to the length and started, its line.
            if (InArena()) shown_ = true;
            timerShown_ = true;
            timerEnd_ = now + double(n.value) / 1000.0;
            out.lines.push_back(Say("UIIT_MGS_ARENA_START"));
            break;
        case 0x0B:
        case 0x0C: out.lines.push_back(Say("UIIT_MGS_ARENA_JOIN_ING")); break;
        case 0x0D:
        case 0x0E:
            out.lines.push_back(Say("UIIT_MGS_ARENA_START_ALARM", {Literal(n.code == 0x0D ? "5" : "1")}));
            out.lines.push_back(Say("UIIT_MSG_ARENA_GAME_ENTERD_ERORR"));
            break;
        case 0x0F:
            ForKinds(n.kinds, [&](std::uint8_t kind) {
                out.lines.push_back(Say("UIIT_MGS_ARENA_JOIN_ING2", {ModeArg(n.mode), Key(arena::KindName(kind))}));
            });
            out.lines.push_back(Say("UIIT_MGS_ARENA_JOIN_GUIDANCE"));
            break;
        default: break; // 6, 7, 0x10 and the rest: nothing
        }
        return out;
    }
    // A code to this player (its FF form). 03: "You are not registered." (none of ours sends it).
    if (n.code == 3) {
        out.lines.push_back(Say("UIIT_MGS_ARENA_UNKNOWN_USER"));
        return out;
    }
    switch (ArenaCode(n.code)) {
    case ArenaCode::Done:
        // Action 2 the cancel, else the registration; the mode's name.
        out.lines.push_back(Say(n.value == 2 ? "UIIT_MGS_ARENA_CANCEL_DISPLAY" : "UIIT_MGS_ARENA_JOIN_WINDOW_COMPLETION_DISPLAY", {ModeArg(n.mode)}));
        break;
    case ArenaCode::Already: out.lines.push_back(Say("UIIT_MGS_ARENA_ALREADY_JOIN_ERR")); break;
    case ArenaCode::NotNow: out.lines.push_back(Say("UIIT_MGS_ARENA_CANT_PARTICIPATE_RIGHT_NOW")); break;
    case ArenaCode::Level: out.lines.push_back(Say("UIIT_MSG_FLAGWAR_GAME_JOIN_LEVEL_ERORR")); break;
    case ArenaCode::TooFew: out.lines.push_back(Say("UIIT_MSG_FLAGWAR_GAME_CANCEL")); break;
    case ArenaCode::EnterFailed: out.lines.push_back(Say("UIIT_MGS_ARENA_ENTRANCE_FAIL")); break;
    case ArenaCode::Conditions: out.lines.push_back(Say("UIIT_MSG_ARENA_GAME_ENTERD_ERORR")); break;
    case ArenaCode::NotLeader: out.lines.push_back(Say("UIIT_MGS_ARENA_CANT_PARTICIPATE_ENTRANCE_CONDITION_PARTY")); break;
    case ArenaCode::NotMaster: out.lines.push_back(Say("UIIT_MGS_ARENA_CANT_PARTICIPATE_ENTRANCE_CONDITION_GUILD")); break;
    case ArenaCode::JobSuit: out.lines.push_back(Say("UIIT_MGS_ARENA_CANT_PARTICIPATE_ENTRANCE_CONDITION")); break;
    case ArenaCode::Points:
        out.points = n.value;
        out.lines.push_back(Say("UIIT_MGS_ARENA_SCORE_ACQUISITION", {Literal(std::to_string(n.value))}));
        break;
    case ArenaCode::OurCarrier: out.lines.push_back(Say("UIIT_MSG_FLAGWAR_PICKUP_FLAG_OURTEAM", {Literal(n.name)})); break;
    case ArenaCode::TheirCarrier: out.lines.push_back(Say("UIIT_MSG_FLAGWAR_PICKUP_FLAG_OTHERTEAM")); break;
    case ArenaCode::Action: out.actionBar = n.value; break;
    case ArenaCode::Dropped: out.lines.push_back(Say("UIIT_MSG_FLAGWAR_FLAG_DROP")); break;
    case ArenaCode::Placed:
        // Team 0 "Red Team", any other "Blue Team"; then the placer.
        out.lines.push_back(Say("UIIT_MGS_ARENA_FLAGWAR_INSTALLED_DISPLAY",
                                {Key(n.team == 0 ? "UIIT_MGS_ARENA_FLAGWAR_RED" : "UIIT_MGS_ARENA_FLAGWAR_BLUE"), Literal(n.name)}));
        break;
    case ArenaCode::TakeBusy: out.lines.push_back(Say("UIIT_MSG_FLAGWAR_PLAYER_FLAG_PICKUP")); break;
    case ArenaCode::PlaceBusy: out.lines.push_back(Say("UIIT_MGS_ARENA_FLAGWAR_PLAYER_PUTDOWN_FLAG")); break;
    case ArenaCode::FlagBlocked: out.lines.push_back(Say("UIIT_MGS_ARENA_FLAGWAR_ERR_CANT_PICKING_FLAG_IN_SAFEZONE")); break;
    case ArenaCode::HasFlag: out.lines.push_back(Say("UIIT_MGS_ARENA_ERR_FLAG_PICKUP")); break;
    case ArenaCode::Clock:
        // The 10-s clock: a player without the state gets it (the windows made); the timer, the score and rank windows
        // shown; the timer set to length - elapsed.
        if (!InArena()) Enter(n.mode, n.kind);
        shown_ = true;
        timerShown_ = true;
        timerEnd_ = now + double(std::max<std::int64_t>(0, std::int64_t(n.value) - std::int64_t(n.value2))) / 1000.0;
        break;
    case ArenaCode::Revival: out.lines.push_back(Say("UIIT_MGS_ARENA_END_GUIDANCE", {Literal(std::to_string(kRevivalSeconds))})); break;
    case ArenaCode::BagFull: out.lines.push_back(Say("UIIT_MSG_ARENA_REWARD_MESSAGE_ERR")); break;
    default: break; // 01 (no line), 0x41 / 0x88 (ArenaScores), the rest: nothing
    }
    return out;
}

void ArenaView::Apply(const proto::ArenaScores& s) {
    // FF 41 / FF 88: the score window shown if hidden and set; FF 41's rows into the rank window, shown.
    if (!InArena()) return; // no windows to fill
    shown_ = true;
    team0_ = s.team0;
    team1_ = s.team1;
    if (kind_ != arena::kFlag) {
        top_.assign(s.top.begin(), s.top.begin() + std::ptrdiff_t(std::min<std::size_t>(s.top.size(), kTopRows)));
        rankShown_ = true;
    }
}

ArenaView::Outcome ArenaView::Apply(const proto::ArenaResult& r) {
    // Broadcast 9 (case 9): the result window made if none, its picture, the two scores, coins and SP.
    Outcome out;
    result_ = r;
    out.result = true;
    return out;
}

std::optional<ArenaView::DeskRow> ArenaView::DeskRowOf(std::uint16_t state) {
    if ((state & arena::kRegistered) != 0) return DeskRow{"UIIT_STT_ARENA_NPC_CANCEL", true, 4, 0};
    const std::uint8_t mode = std::uint8_t(state & 0x0F);
    if (mode > 3) return std::nullopt;
    return DeskRow{std::string(arena::ModeName(arena::Mode(mode))), false, mode, std::uint16_t(state & 0xF0)};
}

std::string_view ArenaView::ModeTalk(std::uint8_t mode) {
    switch (mode) {
    case 0: return "SN_NPC_BATTLE_ARENA_MANAGER_RANDOMMATCH_BS";
    case 1: return "SN_NPC_BATTLE_ARENA_MANAGER_PARTY_BS";
    case 2: return "SN_NPC_BATTLE_ARENA_MANAGER_GUILD_BS";
    case 3: return "SN_NPC_BATTLE_ARENA_MANAGER_JOB_BS";
    default: return {};
    }
}

std::vector<ArenaView::KindRow> ArenaView::KindRows(std::uint16_t kinds) {
    std::vector<KindRow> rows;
    if ((kinds & arena::kScoreBit) != 0) rows.push_back({arena::kScore, "UIIT_STT_ARENA_SCORE", "UIIT_MSG_ARENA_JOIN_WINDOW_DESCRIPTION_SCORE"});
    if ((kinds & arena::kFlagBit) != 0) rows.push_back({arena::kFlag, "UIIT_STT_ARENA_FLAG", "UIIT_MSG_ARENA_JOIN_WINDOW_DESCRIPTION_FLAG"});
    return rows;
}

ArenaView::Line ArenaView::Confirmation(std::uint8_t mode, std::uint8_t kind) {
    return Say("UIIT_MSG_ARENA_JOIN_WINDOW_CONFIRMATION", {ModeArg(mode), Key(arena::KindName(kind))});
}

proto::ArenaRegister ArenaView::Register(std::uint8_t mode, std::uint8_t kind, std::vector<std::uint32_t> members) {
    proto::ArenaRegister r;
    r.action = 1;
    r.mode = mode;
    r.kind = kind;
    if (mode == std::uint8_t(arena::Mode::Guild)) r.members = std::move(members);
    return r;
}

} // namespace sr::game
