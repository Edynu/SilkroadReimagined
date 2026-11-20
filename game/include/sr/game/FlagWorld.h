#pragma once

// Capture the Flag (the original's FlagWorld) and Event So-Ok's desk and prize swap: the rules both sides share.
// Headless (the game server keeps the matches, the client reads the codes). The numbers and places are the original's;
// what the original's shard manager decided (the level groups, the team split, the minimum, the schedule) is ours and
// marked so.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "sr/game/Places.h"

namespace sr::game {

namespace flag {

inline constexpr std::uint16_t kRegion = 0x8009;   // dungeon 9, Dungeon\wchina\event.dof
inline constexpr std::uint16_t kWorld = 29;        // _RefGame_World INS_EVENT_RANDOMMATCH (state 8's world id)
inline constexpr int kMinLevel = 20;               // the highest level reached (cmp al, 0x14)
inline constexpr int kTeamMost = 8;                // 16 a match, 8 a side (the shard manager's, SM)
inline constexpr int kMinPlayers = 0;              // 0: the shard manager's minimum by group (MinPlayersOfGroup); else ours
inline constexpr std::int64_t kWarmupMs = 10'000;  // the monsters at start + 10 s
inline constexpr std::int64_t kPlayMs = 1'200'000; // the match's 20 minutes after the warm-up (0x127690)
inline constexpr std::int64_t kActionMs = 5'000;   // a pole's take or place (GetTickCount + 0x1388)
inline constexpr std::int64_t kCarryMs = 60'000;   // the carrier's time
inline constexpr std::int64_t kReviveMs = 30'000;  // a death's automatic revival at the camp (0x7530)
inline constexpr std::int64_t kCampButtonMs = 10'000; // ours: the death box's camp button revives after the client's 10 s countdown
inline constexpr std::int64_t kUntouchableMs = 6'000; // body mode 2 on entering the match world
inline constexpr std::int64_t kClockMs = 10'000;   // FF 0E every 10 s of play
inline constexpr std::int64_t kAttackAfterMs = 10'000; // kills count as the event's relation from start + 10 s
inline constexpr float kReach = 15.0f;             // a pole farther than this: walked there
inline constexpr float kWalkTo = 10.0f;            // to this far from it
inline constexpr float kMonsterSpread = 300.0f;    // the monsters within this of their anchor
inline constexpr int kMonstersPerRoom = 10;
inline constexpr int kTrophyLevelGap = 9;          // a kill's trophy: the levels at most this apart
inline constexpr int kGroups = 7;

inline constexpr std::string_view kTrophy = "ITEM_ETC_E080723_ICETROPHY";
inline constexpr std::string_view kCarrierSkill = "SKILL_EVENT_FLAG_GET_01";
inline constexpr std::string_view kSoOk = "NPC_CH_EVENT_KISAENG1";
inline constexpr std::string_view kCard = "ITEM_ETC_E070919_TICKET";
inline constexpr std::string_view kKey = "NPC_EVENT_FLAGWAR_MASTERKEY";
inline constexpr std::string_view kDroppedFlag = "NPC_EVENT_FLAGWAR_FLAG";

struct Point {
    float x = 0, y = 0, z = 0;
};
// The poles: 0 white (the centre), 1 red (team 0's), 2 blue (team 1's) (floats).
inline constexpr Point kPoles[3] = {{992.0f, 0.0f, -340.0f}, {1578.0f, 0.0f, 202.0f}, {339.0f, 0.0f, 198.0f}};
inline constexpr std::string_view kPoleCodenames[3] = {"NPC_EVENT_FLAGWAR_FLAGPOLE_WHITE", "NPC_EVENT_FLAGWAR_FLAGPOLE_RED",
                                                       "NPC_EVENT_FLAGWAR_FLAGPOLE_BLUE"};
inline constexpr std::string_view kFlapCodenames[3] = {"NPC_EVENT_FLAGWAR_FLAP_FLAGPOLE_WHITE", "NPC_EVENT_FLAGWAR_FLAP_FLAGPOLE_RED",
                                                       "NPC_EVENT_FLAGWAR_FLAP_FLAGPOLE_BLUE"};
// The camps (the revival rooms) by team.
inline constexpr Point kCamps[2] = {{2104.0f, 0.0f, -8.0f}, {-183.0f, 0.0f, 5.0f}};
// The monsters' anchors by team: team 0's in the red room, team 1's in the blue.
inline constexpr Point kAnchors[2] = {{1314.0f, 0.0f, 255.0f}, {607.0f, 0.0f, 200.0f}};
// The doors by team (the key a team takes opens its own room's door, for everyone): their event zones
// (_RefEventZone 233 / 232, the navmesh's names in lower case) and the doorways' middles.
inline constexpr std::string_view kDoorZones[2] = {"structure_pos_event_gate_red", "structure_pos_event_gate_blue"};
inline constexpr std::uint32_t kDoorZoneIds[2] = {233, 232};
inline constexpr Point kDoorCentres[2] = {{1300.0f, 0.0f, -102.0f}, {620.0f, 0.0f, -102.0f}};

// The team's own pole (where it places): team 0 red (1), team 1 blue (2).
inline int OwnPole(std::uint8_t team) { return team == 0 ? 1 : 2; }
// The monsters of a level group.
std::string_view MonsterOfGroup(int group);
// The shard manager's level groups by the player's highest level: 0 (19 and below: refused) 1 (20..35),
// 2 (36..50), 3 (51..65), 4 (66..80), 5 (81..90), 6 (91 and above); the old note follows: its
// monsters' level: 0 20-29, 1 30-39, 2 40-54, 3 55-69, 4 70-79, 5 80-89, 6 90 and over.
int GroupOfLevel(int level);

} // namespace flag

// The original's 0x34B1 to the client (and "The client's side"): a state broadcast, or a code to one
// player (its FF form). Our FlagWorldNotice carries the same numbers.
enum class FlagState : std::uint8_t {
    Open = 2,       // participation opened: UIIT_MSG_FLAGWAR_STANDBY_10
    Closed = 3,     // participation closed (nothing shown)
    Started = 4,    // the play: UIIT_MSG_FLAGWAR_GAME_START, the doors closed again
    Stopped = 5,    // everyone out, the timer hidden
    MatchWorld = 8, // value = the world id: _GAME_START_MISSION and the 20:00 timer for those in it
    Ended = 9,      // _GAME_END, the timer hidden
    Action = 0x0A,  // value = the actor's entity: the 5 s bar over his head
    Standby5 = 0x0D,
    Standby1 = 0x0E,
};
enum class FlagCode : std::uint8_t {
    Done = 0,          // a registration or its cancel accepted (the client picks the line by the row it drew)
    MoveFailed = 1,    // (no line)
    NotNow = 4,        // _GAME_START_NOT_ENTER
    Level = 5,         // _GAME_JOIN_LEVEL_ERORR
    TooFew = 6,        // _GAME_CANCEL
    DoorLocked = 8,    // _FLAGZONE_CANNOT_ENTER
    OurCarrier = 9,    // name: _PICKUP_FLAG_OURTEAM
    TheirCarrier = 0x0A,
    KeyTaken = 0x0B,   // value = the door's zone id, team, name: the door open; _PICKUP_MASTERKEY to that team
    Subscribed = 0x0C, // the card's box
    Dead = 0x0D,       // (no line)
    Clock = 0x0E,      // value = the elapsed ms
    Dropped = 0x0F,
    Placed = 0x10,     // team, name: _FLAG_MISSION_CLEAR / _FAIL
    Won = 0x11,
    TakeBusy = 0x12,
    PlaceBusy = 0x13,
    Conditions = 0x15, // _GAME_ENTERD_ERORR (a system line only)
    Lost = 0x16,
    Draw = 0x17,
    FlagBlocked = 0x18, // (no line)
};

// --- Registration ------------------------------------------------------------------------------------------

// A player at So-Ok as the checks read them.
struct FlagEntrant {
    int maxLevel = 1;         // the highest level reached
    bool alive = true;
    bool interacting = false; // at a stall (owner or visitor), the stall network, a specialty trade NPC
    bool jobSuit = false;
    bool freePvp = false;
    bool guildWar = false;    // his guild has a hostile guild
    bool inTown = true;       // the region's battlefield byte 0
    bool questBlock = false;  // a quest's teleport block (bit 0x40000; ours has none yet)
};
// Done, Level, or Conditions.
FlagCode RegistrationRefusal(const FlagEntrant& e);

// --- Forming the matches (the shard manager's) --------------------------------------------
// A match's fewest players by its group: 4 for groups 1 and 2, 8 above.
namespace flag {
int MinPlayersOfGroup(int group);
} // namespace flag

struct FlagEntry {
    std::uint32_t character = 0;
    int level = 1;        // the highest level reached
    std::int64_t order = 0; // when it registered (earlier first)
};
struct FlagMatchPlan {
    int group = 0;
    std::vector<std::pair<std::uint32_t, std::uint8_t>> players; // character, team
};
struct FlagPlan {
    std::vector<FlagMatchPlan> matches;
    std::vector<std::uint32_t> cancelled; // in a group too small
};
// The shard manager's: the players by level, the lowest first (then by when they registered); those of group 0
// refused; every `teamMost` x 2 in a row a match (the group its highest player's); a last match of an odd count drops
// its last player; a match below its group's fewest (`minPlayers` instead when above 0) cancelled whole; each match's
// teams in turn from a first team `coin` draws (0 or 1; none: 0): t, !t, !t, t, t, !t, ... Parties are left on entry.
FlagPlan PlanMatches(std::vector<FlagEntry> entries, int minPlayers = flag::kMinPlayers, int teamMost = flag::kTeamMost,
                     const std::function<int()>& coin = {});

// --- The flag -----------------------------------------------------------------------

class FlagMatch {
public:
    struct Event {
        enum class Kind : std::uint8_t { Started, Carrier, Placed, Dropped } kind = Kind::Started;
        std::uint32_t actor = 0;
        std::uint8_t team = 0;
        int pole = -1;
    };
    struct Action {
        std::uint32_t actor = 0; // 0: free
        std::uint8_t team = 0;
        bool place = false;      // else a take
        std::int64_t until = 0;
    };
    FlagMatch() { Reset(); }
    // The play's start: the flag on the white pole, nothing else.
    void Reset();
    // A pole selected (in reach; take, place): Done with a Started event, a busy code, or none
    // (nothing to do there).
    std::optional<FlagCode> SelectPole(int pole, std::uint32_t actor, std::uint8_t team, std::int64_t now, std::vector<Event>& out);
    // The dropped flag selected (instant): he carries it.
    bool PickUp(std::uint32_t actor, std::uint8_t team, std::int64_t now, std::vector<Event>& out);
    // His death or leaving: his action cancelled, the flag dropped if he carries it.
    void Gone(std::uint32_t actor, std::vector<Event>& out);
    // The actions done, the carrier's 60 s.
    void Tick(std::int64_t now, std::vector<Event>& out);
    // The winner: 0 the flag on red, 1 on blue, none a draw.
    std::optional<std::uint8_t> Winner() const;

    bool OnPole(int pole) const { return flapped_[pole]; }
    bool Dropped() const { return dropped_; }
    std::uint32_t Carrier() const { return carrier_; }
    std::uint8_t CarrierTeam() const { return carrierTeam_; }
    const Action& ActionAt(int pole) const { return actions_[pole]; }

private:
    void Drop(std::vector<Event>& out);
    bool flapped_[3] = {};
    Action actions_[3];
    std::uint32_t carrier_ = 0;
    std::uint8_t carrierTeam_ = 0;
    std::int64_t carrierSince_ = 0;
    bool dropped_ = false;
};

// --- The schedule (ours: the original's is the shard manager's) ------------------------------------------------

// One round: participation opens (10 minutes before the play, define 12's description), the standby lines at 5 and
// 1 minutes before (states 0x0D, 0x0E), the play (10 s of warm-up and 20 minutes), the end with its results, the
// stop (the winners out). Unix seconds.
struct FlagCycle {
    std::int64_t open = 0, standby5 = 0, standby1 = 0, start = 0, end = 0, stop = 0;
};
struct FlagSchedule {
    std::vector<int> times;      // the plays' starts, minutes of the server's local day
    std::int64_t registerSeconds = 600, playSeconds = (flag::kWarmupMs + flag::kPlayMs) / 1000, winnersSeconds = 60;
    bool cycle = false;          // a cycle from the Unix epoch (tries): registration, play, winners, again
};
// The round running at `now` or the next one (none: no times).
std::optional<FlagCycle> NextFlagCycle(std::int64_t now, const FlagSchedule& s);
// A round started at once (the developer's switch): registration for `registerSeconds` from now.
FlagCycle FlagCycleFrom(std::int64_t open, const FlagSchedule& s);

// --- So-Ok's prize swap ----------------------------------------------------------------

struct SwapPrize {
    std::string_view item;
    std::uint16_t count = 1;
    float weight = 0;
};
struct SwapQuest {
    std::uint16_t id = 0;
    std::string_view codename, title, token;
    std::span<const SwapPrize> prizes;
};
inline constexpr int kSwapTokens = 10;
// QEV_CH_EVENT_KISAENG_081201 and _070919.
const SwapQuest& SwapIceTrophies();
const SwapQuest& SwapTickets();
const SwapQuest* SwapQuestById(std::uint16_t id);
// The prize of a draw `r` in [0, 100] (the original: rand() / 32767.0 x 100): the first row whose running sum of the
// float weights is at least r, else the first.
std::size_t DrawPrize(std::span<const SwapPrize> prizes, float r);
// The talk's keys (A greeting, B the swap's row, C given, D bag full, E too few).
inline constexpr std::string_view kSwapTalkA = "SN_TALK_QEV_CH_EVENT_KISAENG_070919_A", kSwapTalkB = "SN_TALK_QEV_CH_EVENT_KISAENG_070919_B",
                                  kSwapTalkC = "SN_TALK_QEV_CH_EVENT_KISAENG_070919_C", kSwapTalkD = "SN_TALK_QEV_CH_EVENT_KISAENG_070919_D",
                                  kSwapTalkE = "SN_TALK_QEV_CH_EVENT_KISAENG_070919_E", kTalkExit = "SN_TALK_COMMON_EXIT";

} // namespace sr::game
