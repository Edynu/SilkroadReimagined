#include "sr/game/FlagView.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace sr::game {

namespace {

FlagView::Line Shown(const char* key, std::string name = {}) { return {key, std::move(name), true}; }

} // namespace

FlagView::Outcome FlagView::Apply(const proto::FlagWorldNotice& n, std::uint32_t self, double now) {
    Outcome out;
    if (n.personal == 0) {
        // A state broadcast (the byte table / jump table).
        switch (FlagState(n.code)) {
        case FlagState::Open: out.line = Shown("UIIT_MSG_FLAGWAR_STANDBY_10"); break;
        case FlagState::Standby5: out.line = Shown("UIIT_MSG_FLAGWAR_STANDBY_5"); break;
        case FlagState::Standby1: out.line = Shown("UIIT_MSG_FLAGWAR_STANDBY_1"); break;
        case FlagState::Started:
            doors_.clear(); // every door closed again
            out.line = Shown("UIIT_MSG_FLAGWAR_GAME_START");
            break;
        case FlagState::Stopped: timerShown_ = false; break;
        case FlagState::MatchWorld:
            // Only for the match world's players (the original compares the player's world; ours sends it
            // only to them, with the match world's id): the line and the timer at 20:00.
            if (n.value != flag::kWorld) break;
            out.line = Shown("UIIT_MSG_FLAGWAR_GAME_START_MISSION");
            timerShown_ = true;
            timerEnd_ = now + double(kTimerMs) / 1000.0;
            break;
        case FlagState::Ended:
            out.line = Shown("UIIT_MSG_FLAGWAR_GAME_END");
            timerShown_ = false;
            break;
        case FlagState::Action: out.actionBar = n.value; break;
        default: break; // 3 (participation closed), 6, 7, 0x0B, 0x0C, 0x0F..: nothing
        }
        return out;
    }
    // A code to this player (its FF form, codes 0..0x17).
    switch (FlagCode(n.code)) {
    case FlagCode::Done:
        // Which one is the client's own flag, set when So-Ok's row was drawn (nothing in the message says).
        out.line = Shown(registering_ ? "UIIT_MSG_FLAGWAR_GAME_JOIN_SUCCES" : "UIIT_MSG_FLAGWAR_GAME_JOIN_CANCEL");
        break;
    case FlagCode::NotNow: out.line = Shown("UIIT_MSG_FLAGWAR_GAME_START_NOT_ENTER"); break;
    case FlagCode::Level: out.line = Shown("UIIT_MSG_FLAGWAR_GAME_JOIN_LEVEL_ERORR"); break;
    case FlagCode::TooFew: out.line = Shown("UIIT_MSG_FLAGWAR_GAME_CANCEL"); break;
    case FlagCode::DoorLocked: out.line = Shown("UIIT_MSG_FLAGWAR_FLAGZONE_CANNOT_ENTER"); break;
    case FlagCode::OurCarrier: out.line = Shown("UIIT_MSG_FLAGWAR_PICKUP_FLAG_OURTEAM", n.name); break;
    case FlagCode::TheirCarrier: out.line = Shown("UIIT_MSG_FLAGWAR_PICKUP_FLAG_OTHERTEAM"); break;
    case FlagCode::KeyTaken:
        // The door open for every receiver; the line only to the taker's team.
        doors_[n.value] = true;
        if (n.team == TeamOf(self)) out.line = Shown("UIIT_MSG_FLAGWAR_PICKUP_MASTERKEY", n.name);
        break;
    case FlagCode::Subscribed: out.cardBox = true; break;
    case FlagCode::Clock:
        // The 10-s resynchronisation: shown if hidden, set to 20:00 less the time played, run.
        timerShown_ = true;
        timerEnd_ = now + double(std::max<std::int64_t>(0, kTimerMs - std::int64_t(n.value))) / 1000.0;
        break;
    case FlagCode::Dropped: out.line = Shown("UIIT_MSG_FLAGWAR_FLAG_DROP"); break;
    case FlagCode::Placed:
        out.line = n.team == TeamOf(self) ? Shown("UIIT_MSG_FLAGWAR_FLAG_MISSION_CLEAR", n.name) : Shown("UIIT_MSG_FLAGWAR_FLAG_MISSION_FAIL");
        break;
    case FlagCode::Won: out.line = Shown("UIIT_MSG_FLAGWAR_GAME_WIN"); break;
    case FlagCode::TakeBusy:
    case FlagCode::PlaceBusy: out.line = Shown("UIIT_MSG_FLAGWAR_PLAYER_FLAG_PICKUP"); break;
    case FlagCode(0x14):
    case FlagCode::Conditions: out.line = Line{"UIIT_MSG_FLAGWAR_GAME_ENTERD_ERORR", {}, false}; break; // the system line alone
    case FlagCode::Lost: out.line = Shown("UIIT_MSG_FLAGWAR_GAME_LOSE"); break;
    case FlagCode::Draw: out.line = Shown("UIIT_MSG_FLAGWAR_GAME_DRAW"); break;
    default: break; // 01, 02, 03, 07, 0D (the revival), 0x18 and above: nothing
    }
    return out;
}

void FlagView::Apply(const proto::EntityFlagTeam& t) {
    if (t.team == kNoTeam) teams_.erase(t.entity);
    else teams_[t.entity] = t.team;
}

void FlagView::Entered(std::uint32_t self) {
    const std::uint8_t own = TeamOf(self);
    teams_.clear();
    if (own != kNoTeam) teams_[self] = own;
    else timerShown_ = false;
}

std::uint8_t FlagView::TeamOf(std::uint32_t entity) const {
    const auto it = teams_.find(entity);
    return it != teams_.end() ? it->second : kNoTeam;
}

bool FlagView::Enemy(std::uint32_t self, std::uint32_t other) const {
    const std::uint8_t a = TeamOf(self), b = TeamOf(other);
    return other != self && a != kNoTeam && b != kNoTeam && a != b;
}

bool FlagView::DoorOpen(std::uint32_t zone) const {
    const auto it = doors_.find(zone);
    return it != doors_.end() && it->second;
}

std::string FlagView::TimerText(double secondsLeft) {
    const int s = std::max(0, int(std::ceil(secondsLeft - 1e-6)));
    return std::format("{:02}:{:02}", s / 60, s % 60);
}

std::string_view FlagView::DeskRowKey(std::uint8_t flagState) {
    switch (flagState) {
    case 1: return "UIIT_MSG_FLAGWAR_GAME_JOIN_REQ";
    case 2: return "UIIT_MSG_FLAGWAR_GAME_JOIN_REQ_CANCEL";
    default: return {};
    }
}

std::optional<FlagView::Line> FlagView::RowRefusal(int level) {
    if (level < flag::kMinLevel) return Shown("UIIT_MSG_FLAGWAR_GAME_JOIN_LEVEL_ERORR");
    return std::nullopt;
}

} // namespace sr::game
