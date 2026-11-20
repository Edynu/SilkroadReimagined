#pragma once

// Guilds and unions: the original's rules and numbers, shared by the game
// server and the client, and the client's picture of its guild and union (GuildView), fed by the
// session's guild events.
//
// Its rules follow the original's game server, the original's database for what its shard manager writes, and the
// 1.188 client for what it shows.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sr/game/GuildProtocol.h"

namespace sr::game {

// --- The rules -----------------------------------------------------------------------------------

constexpr int kGuildCreateLevel = 20;
constexpr std::uint64_t kGuildCreateGold = 500'000;   // (0x7A120), taken on success
constexpr std::size_t kGuildNameMin = 2, kGuildNameMax = 12; // bytes
constexpr std::size_t kGuildNicknameMax = 12;         // bytes
constexpr int kGuildNicknameLevel = 4;                // titles from guild level 4
constexpr int kGuildMaxLevel = 5;
constexpr int kUnionMinLevel = 2;                     // both guilds
constexpr std::size_t kUnionMaxGuilds = 8;            // _AlliedClans Ally1..Ally8
constexpr int kUnionChatHolders = 12;                 // per guild, the master included
constexpr std::uint32_t kGuildPenaltySeconds = 259'200; // 3 days: timed job category 2, job 1
constexpr std::uint32_t kProposalSeconds = 30;        // a proposal's answer
constexpr float kGuildInviteReach = 600.0f;           // a player's reach
constexpr float kGuildNpcReach = 300.0f;              // an NPC's reach

// The NPCs that keep guilds, one in each town: Jangan's general merchant, the others' guild managers
// (function 15, by a part of the codename).
inline bool IsGuildNpcCodename(std::string_view code) {
    for (const std::string_view keeper : {std::string_view("NPC_CH_GENARAL_SP"), std::string_view("NPC_WC_GUILD"), std::string_view("NPC_KT_GUILD"),
                                          std::string_view("NPC_EU_GUILD"), std::string_view("NPC_CA_GUILD"), std::string_view("NPC_SD_M_AREA_GUILD"),
                                          std::string_view("NPC_SD_T_AREA_GUILD2")}) {
        if (code.find(keeper) != std::string_view::npos) return true;
    }
    return false;
}

// The most members by guild level (index: the level; the client's table).
constexpr std::array<int, 6> kGuildMembers = {0, 15, 20, 25, 35, 50};
inline int GuildMemberCap(int level) { return kGuildMembers[std::size_t(level < 1 ? 1 : level > kGuildMaxLevel ? kGuildMaxLevel : level)]; }
// What the next level costs from each level (index: the level now; 0 at the top): guild points
// (the client's) and gold.
constexpr std::array<std::uint32_t, 6> kGuildLevelUpPoints = {0, 5'400, 50'400, 135'000, 378'000, 0};
constexpr std::array<std::uint64_t, 6> kGuildLevelUpGold = {0, 3'000'000, 9'000'000, 15'000'000, 21'000'000, 0};

// A member's authority (_GuildMember.Permission). The master
// holds them all (0xFFFFFFFF); a new member none.
constexpr std::uint32_t kGuildPermInvite = 0x01, kGuildPermKick = 0x02, kGuildPermUnionChat = 0x04, kGuildPermStorage = 0x08,
                        kGuildPermNotice = 0x10;
constexpr std::uint32_t kGuildPermAll = 0xFFFFFFFFu;
// The guild storage's places by the guild's level: a page of 30 a level from 2 (the original's shard manager sends the
// size).
constexpr int GuildStorageSize(int level) { return level < 2 ? 0 : level >= 5 ? 120 : (level - 1) * 30; }
// A member's place in the fortress war (_GuildMember.SiegeAuthority): the master is its commander.
// How many of each a guild may have, the three administrators only with a fortress.
constexpr std::uint8_t kPositionNone = 0, kPositionCommander = 1, kPositionDeputy = 2, kPositionWar = 4, kPositionProduction = 8, kPositionTraining = 0x10, kPositionEngineer = 0x20;
constexpr int GuildPositionPlaces(std::uint8_t position) { return position == kPositionDeputy ? 4 : position == kPositionEngineer ? 10 : 1; }
constexpr bool GuildPositionNeedsFortress(std::uint8_t position) { return position == kPositionWar || position == kPositionProduction || position == kPositionTraining; }
// Its name in the text tables (null: none).
inline const char* GuildPositionKey(std::uint8_t position) {
    switch (position) {
    case kPositionCommander: return "UIIT_STT_FORT_GUILD_COMMANDER";
    case kPositionDeputy: return "UIIT_STT_FORT_GUILD_SUBCOMMANDER";
    case kPositionWar: return "UIIT_STT_FORT_GUILD_BATTLEMANAGER";
    case kPositionProduction: return "UIIT_STT_FORT_GUILD_PRODUCTMANAGER";
    case kPositionTraining: return "UIIT_STT_FORT_GUILD_TRAINERMANAGER";
    case kPositionEngineer: return "UIIT_STT_FORT_GUILD_ENGINEER";
    default: return nullptr;
    }
}
// Ranks (_GuildMember.MemberClass): the client also names 5 "Vice Master", which the server never gives.
constexpr std::uint8_t kGuildMaster = 0, kGuildViceMaster = 5, kGuildMember = 10;

// Why a request was refused: the original's codes (the client has a text for each).
enum class GuildError : std::uint16_t {
    Ok = 0,
    Failed = 3,                // the general refusal: not in reach, not found, dead target
    TargetBusy = 0x4C06,
    Dead = 0x4C07,
    CreatorLevel = 0x4C0A,
    NoGold = 0x4C0C,
    NoGuild = 0x4C0D,
    InviterGone = 0x4C0E,
    TimedOut = 0x4C10,
    ExistingMember = 0x4C12,
    Full = 0x4C13,
    JoinRefused = 0x4C16,
    InAnotherGuild = 0x4C17,
    NameLength = 0x4C18,
    NameNotAllowed = 0x4C19,
    NameTaken = 0x4C1A,
    CannotCreate = 0x4C1B,
    MemberNotFound = 0x4C1D,
    Denied = 0x4C1E,
    MasterNotExpellable = 0x4C1F,
    LevelMax = 0x4C21,
    NoticeTitle = 0x4C22,
    NoticeBody = 0x4C23,
    TargetDenied = 0x4C24,
    NoTargetGuild = 0x4C25,
    OwnGuild = 0x4C26,
    AlreadyAllied = 0x4C27,
    TargetHasUnion = 0x4C28,
    UnionFull = 0x4C29,
    UnionLevel = 0x4C2A,
    UnionTargetLevel = 0x4C2B,
    UnionRefused = 0x4C2E,
    NotInUnion = 0x4C2F,
    NotInThisUnion = 0x4C30,
    LevelUpGold = 0x4C31,
    LevelUpPoints = 0x4C32,
    KickSelf = 0x4C36,
    CreatePenalty = 0x4C3C,
    JoinPenalty = 0x4C3D,
    // A job suit worn: the invitee and the creator; the inviter's
    // own suit is not checked.
    JoinInSuit = 0x4C56,
    CreateInSuit = 0x4C57,
    LevelTooLow = 0x4C4A,      // the storage (level 2) and the emblems (4, a union's 5)
    UnionChatFull = 0x4C4B,
    NicknameLetters = 0x4C4D,
    CrestExists = 0x4C4E,      // an emblem is registered: delete it first
    NoCrest = 0x4C4F,
    NicknameLevel = 0x4C51,
    NoElection = 0x4C33,       // "Elections cannot be called during this period." (the master not away long enough)
    ElectionOpen = 0x4C37,     // an election runs already
    ElectionNoLeave = 0x4C38,  // voters and candidates stay through it
    ElectionNoKick = 0x4C39,
    NoVote = 0x5006,           // no election runs
    NoVotingRight = 0x5008,    // not on within the last three days
    // Guild wars: the other master not in the game or not within 300 ("The
    // object is too far."), already enemies, 50 hostile guilds on either side, not at war, an ally,
    // the master's gold short of the stake, nothing to collect, the stake over 500,000,000.
    WarFar = 0x4C60,
    AlreadyEnemies = 0x4C3A,
    HostileFull = 0x4C3E,
    TargetHostileFull = 0x4C3F,
    NotEnemies = 0x4C40,
    AllyWar = 0x4C41,
    WarGold = 0x4C43,
    NoWarReward = 0x4C45,
    WagerLimit = 0x4C5E,
    DisbandWar = 0x4C46, // "Cannot disband the guild during a war"
    DisbandFortress = 0x4C70, // a guild holding a fortress: "A guild owning a fortress is not allowed to disassemble."
    DisbandSiege = 0x4C74,    // a guild in a fortress's war, or applied for one
    // Fortress positions.
    PositionMaster = 0x4C58,     // the commander's own (the master's)
    PositionFull = 0x4C59,       // no place left
    PositionHeld = 0x4C5A,       // that member has one
    PositionNoFortress = 0x4C5B, // the administrators need a fortress
    PositionNone = 0x4C5C,       // none to take away
    PositionInWar = 0x4C5D,      // the guild is in the fortress war now
};

// Why the guild's points were spent (_GPHistory.Reason).
constexpr std::uint8_t kGpReasonGrowth = 5, kGpReasonGuildItems = 6;
// The fortress's: its buff, structures, NPC employment, guards, item production, the Cave.
constexpr std::uint8_t kGpReasonFortressBuff = 0, kGpReasonStructure = 1, kGpReasonNpc = 2, kGpReasonGuard = 3, kGpReasonForge = 4, kGpReasonDungeon = 7;

// --- Guild wars --------------------------------------------------------

// The victory points to choose from (the 1.188 client's request window offers them): 0 the period decides.
constexpr std::array<std::uint32_t, 8> kGuildWarVictoryPoints{0, 5'000, 10'000, 50'000, 100'000, 150'000, 250'000, 300'000};
// A war's kill's points (the game server's): d = the killer's level - the
// victim's; d <= -6: 1, d > 10: 251, else 15 d + 100 (the killer a pet's: the pet's level).
constexpr std::uint32_t GuildWarKillPoints(int killerLevel, int victimLevel) {
    const int d = killerLevel - victimLevel;
    if (d <= -6) return 1;
    if (d > 10) return 251;
    return std::uint32_t(d * 15 + 100);
}
constexpr std::uint32_t kGuildWarMaxWager = 500'000'000;
constexpr int kGuildWarMaxHostile = 50;
constexpr double kGuildWarReach = 300.0;                  // the other master this near
constexpr int kGuildWarEndSeconds = 60;                   // a surrender: "is to close in a minute" (the client's)
// The terms as the original checks them: the victory's index under 8; a period of at most
// 30 days, 23 hours and 50 minutes, or none; ours: some period or a victory to end it.
bool GuildWarTermsValid(const proto::GuildWarTerms& terms);
// The period's seconds (0: none).
std::int64_t GuildWarSeconds(const proto::GuildWarTerms& terms);

// --- The master's election -------------------------------------------

// Called once the master has been away 45 days (`_Guild_Master_Election_Available`);
// the candidates the members on within 7 days (`_Guild_Is_Master_Candidators`); the voters those on
// within 3 days, and the vote 10 minutes (the 1.188 client's texts; the vote itself ran in the
// shard manager, not ours).
constexpr int kElectionAbsenceDays = 45, kElectionCandidateDays = 7, kElectionVoterDays = 3;
constexpr int kElectionSeconds = 600;
// A candidate wins at the ballot that brings it to this share of the candidates (ballots x 100 / candidates; the shard
// manager's 60.0); the time up without it, the election is cancelled.
constexpr double kElectionWinPercent = 60.0;

// --- Emblems -------------------------------------------------------------

// A guild's emblem from guild level 4, a union's from its leading guild's level 5, by its master
// alone, one at a time.
constexpr int kGuildCrestLevel = 4, kUnionCrestLevel = 5;
constexpr int kCrestSide = 16;
// The items that register and delete them (itemdata 9268-9271, TID 3/3/12/2).
struct CrestItem {
    std::uint8_t kind = proto::kCrestGuild;
    bool remove = false;
};
std::optional<CrestItem> CrestItemOf(std::string_view codename);
// guildmark.bmp / unionmark.bmp as the 1.188 client reads it: "BM", 24 bits a pixel,
// 16 x 16 (bottom-up rows), else Format; the pixels top row first, blue, green, red.
enum class CrestFile : std::uint8_t { Ok = 0, Missing = 1, Format = 2 };
CrestFile ReadCrestBitmap(std::span<const std::uint8_t> file, std::vector<std::uint8_t>& pixels);

// A guild name the server takes: 2-12 bytes (the name rules' letters and filter are the caller's).
inline bool GuildNameLengthOk(std::string_view name) { return name.size() >= kGuildNameMin && name.size() <= kGuildNameMax; }
// Characters of UTF-8 text (no check of its form).
std::size_t Utf8Length(std::string_view text);

// --- The client's picture --------------------------------------------------------------------------

// The guild and union the player belongs to, and the guilds of the players it sees (their
// over-head line), as the server told them.
class GuildView {
public:
    // A guild event; returns the system line's text key and its argument when the event makes one
    // (empty key: none). `self` is the player's character id (its own removal ends its guild).
    struct Line {
        std::string key;            // a textuisystem key (UIIT_MSG_GUILD_...)
        std::string argument;       // its %s
        std::string second;         // its second %s (a title granted: the member, then the title)
        std::string third;          // its third (a war's kill: the victim's guild)
        std::string secondKey;      // its second %s as a text key to look up (a position's name)
    };
    std::vector<Line> Apply(const proto::GuildEvent& event);
    // The world was entered afresh (a teleport too): the others' tags go with their entities.
    void ClearEntities() { entities_.clear(); }
    void Forget(std::uint32_t entity) { entities_.erase(entity); }

    bool InGuild() const { return guild_.id != 0; }
    const proto::GuildInfo& Guild() const { return guild_; }
    const proto::UnionInfo& Union() const { return union_; }
    const proto::GuildMemberRecord* Member(std::uint32_t character) const;
    const proto::GuildMemberRecord* Master() const;
    // The player's own entry (by its name) and authority.
    void SetSelf(std::string name) { selfName_ = std::move(name); }
    const proto::GuildMemberRecord* Self() const;
    bool May(std::uint32_t permission) const;
    // The members online.
    int Online() const;
    // The guild a player in view belongs to (null: none told).
    const proto::EntityGuild* EntityTag(std::uint32_t entity) const;
    // Emblems: the pixels of a guild's (proto::kCrestGuild) or a union's emblem at that revision, once
    // the server sent them (else null; the request is queued when the revision is first seen).
    const std::vector<std::uint8_t>* Crest(std::uint8_t kind, std::uint32_t id, std::uint32_t revision) const;
    std::vector<proto::GuildCrestRequest> TakeCrestRequests() { return std::exchange(crestRequests_, {}); }
    // An emblem wanted for another reason (a fortress's holder on the world map): asked once at that revision.
    void AskCrest(std::uint8_t kind, std::uint32_t id, std::uint32_t revision) { WantCrest(kind, id, revision); }
    // The master's election as last told; the server asked the window open (taken once).
    const proto::GuildElection& Election() const { return election_; }
    bool TakeElectionShow() { return std::exchange(electionShow_, false); }
    // Wars: the player's guild's, a war declared on it (to answer), the reward told.
    const proto::GuildWars& Wars() const { return wars_; }
    bool AtWarWith(std::uint32_t guild) const;
    const proto::GuildWarProposal* WarProposal() const { return warProposal_ ? &*warProposal_ : nullptr; }
    void ClearWarProposal() { warProposal_.reset(); }
    std::optional<std::uint64_t> TakeWarReward() { return std::exchange(warReward_, std::nullopt); }
    // The guild points' history, told when asked for at an NPC (taken once).
    std::optional<proto::GuildGpHistory> TakeGpHistory() { return std::exchange(gpHistory_, std::nullopt); }
    // The master's recall asked of the player (taken once): answered with
    // proto::GuildRecallAnswer within its seconds.
    std::optional<proto::GuildRecallAsk> TakeRecallAsk() { return std::exchange(recallAsk_, std::nullopt); }
    // The over-head line of a guild member: [guild] or [guild * nickname].
    static std::string HeadLine(std::string_view guild, std::string_view nickname);
    // Its colour: the player's own guild orange, its union's light green, others
    // light blue.
    std::uint32_t HeadColour(std::uint32_t guild, std::uint32_t unionId) const;
    // The last proposal to the player not yet answered (kind 0: none), and its answer sent.
    const proto::GuildProposal* Proposal() const { return proposal_.kind != 0 ? &proposal_ : nullptr; }
    void ClearProposal() { proposal_ = {0, 0, {}, {}}; }
    // Answers since the last call (what the HUD reports).
    std::vector<proto::GuildAnswer> TakeAnswers() { return std::exchange(answers_, {}); }
    // A change worth redrawing the guild window for (a counter).
    std::uint64_t Revision() const { return revision_; }

private:
    proto::GuildInfo guild_;
    proto::UnionInfo union_;
    proto::GuildProposal proposal_{0, 0, {}, {}};
    std::unordered_map<std::uint32_t, proto::EntityGuild> entities_;
    struct CrestPixels {
        std::uint32_t revision = 0;
        std::vector<std::uint8_t> pixels;
    };
    std::map<std::pair<std::uint8_t, std::uint32_t>, CrestPixels> crests_;
    std::set<std::tuple<std::uint8_t, std::uint32_t, std::uint32_t>> crestsAsked_;
    std::vector<proto::GuildCrestRequest> crestRequests_;
    proto::GuildElection election_;
    bool electionShow_ = false;
    proto::GuildWars wars_;
    std::optional<proto::GuildWarProposal> warProposal_;
    std::optional<std::uint64_t> warReward_;
    std::optional<proto::GuildGpHistory> gpHistory_;
    std::optional<proto::GuildRecallAsk> recallAsk_;
    void WantCrest(std::uint8_t kind, std::uint32_t id, std::uint32_t revision);
    std::vector<proto::GuildAnswer> answers_;
    std::string selfName_;
    std::uint64_t revision_ = 0;
};

} // namespace sr::game
