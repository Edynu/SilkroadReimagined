#pragma once

// The Battle Arena as the client sees it: what the 1.188 client's 0x34D2 handlers
// (the broadcasts, the FF codes) do with each notice, as data, and what its own
// CBattleArenaMgr keeps: the arena state (its mode and kind, from "start in 10 sec" or a late clock),
// the score and rank windows made and shown, the scores and the top five, the result, the event timer, and the
// manager's desk (the NPC's state after its menu). Headless: the game screen shows the lines, the windows and the bar.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sr/game/Arena.h"
#include "sr/game/EventProtocol.h"

namespace sr::game {

class ArenaView {
public:
    static constexpr int kActionSeconds = 5; // FF 82's gauge ((5))
    static constexpr int kRevivalSeconds = 10; // FF F1's "[10] sec(s)"
    static constexpr int kTopRows = 5;

    // A part of a line: a text key to look up (a mode's or a kind's name, a team's) or a literal (a name, a number).
    struct Arg {
        std::string text;
        bool key = false;
        bool operator==(const Arg&) const = default;
    };
    // A line: a system line of category 0, or the big notice (registration open).
    struct Line {
        std::string key;
        std::vector<Arg> args;
        bool notice = false;
        bool operator==(const Line&) const = default;
    };
    // What a notice asks of the screen.
    struct Outcome {
        std::vector<Line> lines;
        std::uint32_t actionBar = 0; // FF 82: that player's 5 s gauge
        std::uint32_t points = 0;    // FF 40: the floating number over the player
        bool result = false;         // broadcast 9: the result window opened
    };

    // A notice (ArenaNotice: the original's 0x34D2), at `now` (the screen's clock, seconds).
    Outcome Apply(const proto::ArenaNotice& n, double now);
    // The scores (FF 41 with the top five in a score map, FF 88 without in a flag map): the score window shown, its two
    // numbers; FF 41 fills the rank window's rows and shows it.
    void Apply(const proto::ArenaScores& s);
    // The result (broadcast 9): the result window's outcome, scores, coins and skill experience.
    Outcome Apply(const proto::ArenaResult& r);
    // The manager's desk after its NpcMenu; cleared by any NpcMenu (the next one's comes after it).
    void Apply(const proto::ArenaDesk& d) { desk_ = d; }
    void ClearDesk() { desk_ = {}; }

    // The arena state (1.188's CBattleArenaMgr +8 / +9; 0xFF none): made by "start in 10 sec" (4) or a clock (FF F0) with
    // none, ended by the stop (5).
    bool InArena() const { return mode_ != 0xFF; }
    std::uint8_t Mode() const { return mode_; }
    std::uint8_t Kind() const { return kind_; }
    // The windows: the score window (200) made with the state, the rank window (163) too unless the kind is flag;
    // shown by the play (8), a clock or a score (FF 41 / 88), hidden at first.
    bool ScoreShown() const { return InArena() && shown_; }
    bool RankMade() const { return InArena() && kind_ != arena::kFlag; }
    bool RankShown() const { return RankMade() && (shown_ || rankShown_); }
    std::uint32_t Score(std::uint8_t team) const { return team == 0 ? team0_ : team1_; }
    const std::vector<proto::ArenaScores::Row>& Top() const { return top_; }
    // The result window (181), open from broadcast 9 until closed or the stop.
    const std::optional<proto::ArenaResult>& Result() const { return result_; }
    void CloseResult() { result_.reset(); }

    // The event timer (CIFTimerWnd, set to the length by 8, to length - elapsed by FF F0).
    bool TimerShown(double now) const { return timerShown_ && now < timerEnd_ + 1.0; }
    double TimerLeft(double now) const { return timerEnd_ > now ? timerEnd_ - now : 0.0; }

    // The manager's rows: registered (0x8000) the cancel row; else the open mode's name (0..3); else none.
    struct DeskRow {
        std::string key;      // UIIT_STT_ARENA_NPC_CANCEL or the mode's name
        bool cancel = false;
        std::uint8_t mode = 4;
        std::uint16_t kinds = 0; // the kinds offered (0x20 score, 0x40 flag)
    };
    const proto::ArenaDesk& Desk() const { return desk_; }
    static std::optional<DeskRow> DeskRowOf(std::uint16_t state);
    // The open mode's talk (SN_NPC_BATTLE_ARENA_MANAGER_*_BS), its one row "Arena Registration".
    static std::string_view ModeTalk(std::uint8_t mode);
    // The rule window's rows from the kinds' bits: score first, then flag; each its name, description
    // and kind.
    struct KindRow {
        std::uint8_t kind = arena::kScore;
        std::string_view name, description;
    };
    static std::vector<KindRow> KindRows(std::uint16_t kinds);
    // The confirm box's text (UIIT_MSG_ARENA_JOIN_WINDOW_CONFIRMATION "[%s-%s]": the mode's name, the kind's).
    static Line Confirmation(std::uint8_t mode, std::uint8_t kind);
    // The register request (0x74D3 {1, mode, kind} or the guild's {1, 2, kind, members...}).
    static proto::ArenaRegister Register(std::uint8_t mode, std::uint8_t kind, std::vector<std::uint32_t> members = {});

private:
    void Enter(std::uint8_t mode, std::uint8_t kind);
    void End();
    std::uint8_t mode_ = 0xFF, kind_ = 0xFF;
    bool shown_ = false, rankShown_ = false;
    std::uint32_t team0_ = 0, team1_ = 0;
    std::vector<proto::ArenaScores::Row> top_;
    std::optional<proto::ArenaResult> result_;
    bool timerShown_ = false;
    double timerEnd_ = 0.0;
    proto::ArenaDesk desk_;
};

} // namespace sr::game
