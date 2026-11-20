#pragma once

// The Battle Arena: the rules both sides share. Headless.
// The numbers are the original's; what its shard manager decided (the teams, the level groups, the play's length, the
// way home) is ours and marked so.

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "sr/game/FlagWorld.h"

namespace sr::game {

namespace arena {

// The modes (the byte every message carries) and the map kinds (1 score, 2 flag; 0 occupy, a stub in 1.188: not built).
enum class Mode : std::uint8_t { Random = 0, Party = 1, Guild = 2, Job = 3, None = 4 };
inline constexpr std::uint8_t kScore = 1, kFlag = 2;
inline constexpr std::uint16_t kScoreBit = 0x20, kFlagBit = 0x40; // the kinds offered (the NPC's state, the rule window)
inline constexpr std::uint16_t kRegistered = 0x8000;            // the manager's state for a registered player
inline constexpr int kMinLevel = 20;                             // the highest level reached
inline constexpr int kGuildMost = 16;                            // the guild window's chosen (CL)
inline constexpr std::int64_t kPlayDelayMs = 15'000;             // the clock from "start in 10 sec" + 15 s
inline constexpr std::int64_t kReviveMs = 10'000;                // a death: up at the gate 10 s later
inline constexpr std::int64_t kClockMs = 10'000;                 // FF F0 every 10 s
inline constexpr std::int64_t kScoreEveryMs = 3'000;             // the scores at most every 3 s (0xBB8)
inline constexpr std::int64_t kUntouchableMs = 6'000;            // body mode 2 on entering
inline constexpr std::uint32_t kPlayerKillPoints = 6;            // score: a player's kill
inline constexpr std::uint32_t kFlagPoints = 1000;               // flag: a flag on a team's pole
inline constexpr std::uint32_t kFlagKillPoints = 1;              // flag: a death gives the other team 1
inline constexpr float kGateRadius = 10.0f;                      // _RefTeleport GenAreaRadius of the gates
inline constexpr std::string_view kCoin = "ITEM_ETC_ARENA_COIN";
inline constexpr std::string_view kCarrierSkill = "SKILL_BATTLE_ARENA_FLAG_GET_01";
inline constexpr std::string_view kManager = "NPC_BATTLE_ARENA_MANAGER";
inline constexpr std::string_view kDroppedFlag = "NPC_BATTLE_ARENA_FLAGWAR_FLAG";
// The arena's poles by FlagMatch's index (0 white, 1 red: team 0's, 2 blue: team 1's) and their flagged twins.
inline constexpr std::string_view kPoleCodenames[3] = {"NPC_BATTLE_ARENA_FLAGWAR_FLAGPOLE_WHITE", "NPC_BATTLE_ARENA_FLAGWAR_FLAGPOLE_RED",
                                                       "NPC_BATTLE_ARENA_FLAGWAR_FLAGPOLE_BLUE"};
inline constexpr std::string_view kFlapCodenames[3] = {"NPC_BATTLE_ARENA_FLAGWAR_FLAP_FLAGPOLE_WHITE", "NPC_BATTLE_ARENA_FLAGWAR_FLAP_FLAGPOLE_RED",
                                                       "NPC_BATTLE_ARENA_FLAGWAR_FLAP_FLAGPOLE_BLUE"};
// The schedules (`_RefScheduleDefine`): a mode's registration 16 + 2 x mode, its match 17 + 2 x mode.
inline int ParticipationDefine(Mode m) { return 16 + 2 * int(m); }
inline int MatchDefine(Mode m) { return 17 + 2 * int(m); }
// Ours: the play begins 5 minutes into the match schedule (its 5-minute warning), the 1-minute warning a
// minute before, "start in 10 sec" 10 s before; the results 30 s before the schedule's end, the stop (home) at its end.
inline constexpr std::int64_t kWarningSeconds = 300, kResultLeadSeconds = 30;

// Arena Coins by mode: a win 5 (random, party) or 7 (guild, job); a loss or a draw 1 or 2.
int CoinsFor(Mode mode, std::uint8_t outcome);
// The result's outcome (1 won, 2 lost, 3 draw) of a team for a winner (none: a draw).
inline std::uint8_t OutcomeOf(std::uint8_t team, std::optional<std::uint8_t> winner) { return !winner ? 3 : *winner == team ? 1 : 2; }
// The winner by the totals: the greater, equal a draw.
inline std::optional<std::uint8_t> WinnerOf(std::uint32_t team0, std::uint32_t team1) {
    if (team0 == team1) return std::nullopt;
    return std::uint8_t(team0 > team1 ? 0 : 1);
}
// The names' keys (the rule window, the lines).
std::string_view ModeName(Mode m);
inline std::string_view KindName(std::uint8_t kind) { return kind == kFlag ? "UIIT_STT_ARENA_FLAG" : "UIIT_STT_ARENA_SCORE"; }

} // namespace arena

// The original's 0x34D2 to the client: a broadcast, or a code to one player (its FF form).
enum class ArenaState : std::uint8_t {
    Open = 2,       // {mode, kinds}: "Battle Arena [%s-%s] registration has begun."
    Closed = 3,     // {mode}
    Soon = 4,       // {mode, kind}: "start in 10 sec" (the arena state: the windows made, hidden)
    Stopped = 5,    // {mode}: the arena state ended, the windows destroyed
    Started = 8,    // {length}: the windows shown, the timer
    Result = 9,     // {mode, outcome, coins, sp}: the result window
    Warning5 = 0x0D,
    Warning1 = 0x0E,
    Reminder = 0x0F,
};
enum class ArenaCode : std::uint8_t {
    Done = 0,         // {action, mode}
    NotRegistered = 1,
    Already = 2,
    NotNow = 4,
    Level = 5,
    TooFew = 6,
    EnterFailed = 8,
    Conditions = 9,
    NotLeader = 0x0B,
    NotMaster = 0x0C,
    JobSuit = 0x0D,
    Points = 0x40,    // {points}: "Earn [%d]points."
    Board = 0x41,     // the score board (ArenaScores)
    OurCarrier = 0x80, TheirCarrier = 0x81, Action = 0x82, Dropped = 0x83, Placed = 0x84, TakeBusy = 0x85, PlaceBusy = 0x86,
    FlagBlocked = 0x87, FlagScores = 0x88, HasFlag = 0x89,
    Clock = 0xF0,     // {length, elapsed, mode, kind}
    Revival = 0xF1,   // "moved to the starting point in [10] sec(s)"
    BagFull = 0xF2,
};

// A player at the manager as the checks read them.
struct ArenaEntrant {
    int maxLevel = 1;
    bool registered = false;
    bool open = false;           // that mode's registration open
    bool partyLeader = false, guildMaster = false;
    std::uint8_t jobState = 4;   // 1..3 a suit worn, 4 none
    FlagEntrant conditions;      // alive, in town, no free PvP, no war, not dealing (the same list as the flag world's)
    bool riding = false;
};
ArenaCode RegistrationRefusal(arena::Mode mode, const ArenaEntrant& e);

// One side of a match being formed (ours): a lone player (random, job) or a party's or guild's members together.
struct ArenaUnit {
    std::vector<std::uint32_t> characters;
    int level = 1;        // its highest member's (the level group)
    std::int64_t order = 0;
    std::uint8_t job = 0; // job mode: 1 trader, 2 thief, 3 hunter
    // Party and guild: the registrant's level (the order the shard manager pairs them in) and its members' levels
    // together (the match's group by the average); 0: `level`, and `level` a member.
    int leaderLevel = 0;
    int levelSum = 0;
};
struct ArenaMatchPlan {
    int group = 0;
    std::uint8_t size = 8; // the map's side: 8, 16 or 32
    std::vector<std::pair<std::uint32_t, std::uint8_t>> players;
};
struct ArenaPlan {
    std::vector<ArenaMatchPlan> matches;
    std::vector<std::uint32_t> cancelled;
};
// The shard manager's. Random (SM): all the lone players by level, the lowest first; those
// below 20 refused; every 16 in a row a match on the 8 map, a last one of an odd count losing its last player; its
// group the group of its players' average level (SM), below that group's fewest (4 for groups 1 and 2, else 8;
// `minPlayers` when above 0) cancelled whole; the teams in turn from a first `coin` draws. Party (SM) and
// guild (SM): the units by their registrants' levels, the lowest first, paired in that order, a last one
// alone cancelled; a coin's side for the first of each pair; the party's map 8, the guild's 16 when both together pass
// 16, else 8 (each side cut to the map's); the group by all their members' average level. Job (SM): those
// below 20 refused; traders and hunters, and thieves, each by level; a side of 32, while both have that many a match
// of the lowest of each (the map 32, 16 or 8 by the side), else the side halved, down to 2, and at 2 only while the
// next trader or hunter is 50 or below; the rest cancelled; a coin's team for the traders' side; the group by the
// first pair's higher level.
ArenaPlan PlanArena(arena::Mode mode, std::vector<ArenaUnit> units, int minPlayers = 0, const std::function<int()>& coin = {});

} // namespace sr::game
