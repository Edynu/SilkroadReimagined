#pragma once

// Guilds and unions over the network: our own messages (0x1100-0x117F) for
// what the original's carry (its requests 0x70F0-0x7105, its proposals 0x3080 kinds 5 and 6, its
// guild data 0x34B3 / 0x38F5 / 0x3102 and a member's guild to those around, 0x30FF). The rules and
// the refusals' numbers are the original game server's; the error codes keep its numbers, which the client turns into
// its texts.

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "sr/game/Protocol.h"

namespace sr::game::proto {

// --- Requests (0x1100) ------------------------------------------------------------------------

// A new guild, at a guild NPC (its entity; the original's 0x70F0).
struct GuildCreate {
    static constexpr MessageId kId = 0x1100;
    std::uint32_t npc = 0;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("name", name, 64);
    }
};

// A player in view (its entity) asked to join the player's guild (0x70F3).
struct GuildInvite {
    static constexpr MessageId kId = 0x1101;
    std::uint32_t target = 0;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
    }
};

// What the invited player answered (the original's 0x3080 answer): `kind` the proposal's
// (GuildProposal), `accept` 1 yes, 0 no.
struct GuildProposalAnswer {
    static constexpr MessageId kId = 0x1102;
    std::uint8_t kind = 0;
    std::uint8_t accept = 0;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("accept", accept);
    }
};

// Out of the guild, at an NPC (0x70F2: any NPC within its reach; the master cannot leave).
struct GuildLeave {
    static constexpr MessageId kId = 0x1103;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};

// A member put out of the guild, by name (0x70F4).
struct GuildKick {
    static constexpr MessageId kId = 0x1104;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("name", name, 64);
    }
};

// The guild disbanded by its master, at a guild NPC (0x70F1).
struct GuildDisband {
    static constexpr MessageId kId = 0x1105;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};

// The guild's notice (0x70F9): a title of 1-128 characters, a body of 1-2048.
constexpr std::size_t kGuildNoticeTitle = 128, kGuildNoticeBody = 2048;
struct GuildNoticeEdit {
    static constexpr MessageId kId = 0x1106;
    std::string title, body;
    template <typename V>
    void Visit(V& v) {
        v("title", title, kGuildNoticeTitle * 4);
        v("body", body, kGuildNoticeBody * 4);
    }
};

// Members' authority (0x7104, the master only): each listed member's new permission bits.
struct GuildPermissionRecord {
    std::uint32_t character = 0;
    std::uint32_t permission = 0;
    template <typename V>
    void Visit(V& v) {
        v("character", character);
        v("permission", permission);
    }
};
struct GuildPermissions {
    static constexpr MessageId kId = 0x1107;
    std::vector<GuildPermissionRecord> members;
    template <typename V>
    void Visit(V& v) {
        v("members", members, 64);
    }
};

// The master's place handed to a member, at a guild NPC (0x7103).
struct GuildMasterTransfer {
    static constexpr MessageId kId = 0x1108;
    std::uint32_t npc = 0;
    std::uint32_t character = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("character", character);
    }
};

// A member's title (the original's grant name, 0x7256: the master, a guild of level 4 or more;
// empty clears it).
struct GuildNickname {
    static constexpr MessageId kId = 0x1109;
    std::uint32_t character = 0;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("character", character);
        v("name", name, 64);
    }
};

// The guild's next level, at a guild NPC (0x70FA, the master only).
struct GuildLevelUp {
    static constexpr MessageId kId = 0x110A;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};

// Skill points given to the guild as guild points (0x7258, one for one).
struct GuildDonate {
    static constexpr MessageId kId = 0x110B;
    std::uint32_t points = 0;
    template <typename V>
    void Visit(V& v) {
        v("points", points);
    }
};

// Another guild's master (a player in view) asked into the player's union (0x70FB); without a
// union, its acceptance makes one, the player's guild leading.
struct UnionInvite {
    static constexpr MessageId kId = 0x1110;
    std::uint32_t target = 0;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
    }
};

// The player's guild out of its union (0x70FC, its master).
struct UnionLeave {
    static constexpr MessageId kId = 0x1111;
    template <typename V>
    void Visit(V&) {}
};

// A guild put out of the union by the leading guild's master (0x70FD).
struct UnionKick {
    static constexpr MessageId kId = 0x1112;
    std::uint32_t guild = 0;
    template <typename V>
    void Visit(V& v) {
        v("guild", guild);
    }
};

// The master's election: a member asks for it at a guild NPC once the
// master has been away 45 days (the original's 0x7105); the vote's window asked (0x7106); a ballot
// cast at the NPC for a candidate (0x7107).
struct GuildElectionStart {
    static constexpr MessageId kId = 0x1113;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};
struct GuildElectionOpen {
    static constexpr MessageId kId = 0x1114;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};
struct GuildElectionVote {
    static constexpr MessageId kId = 0x1115;
    std::uint32_t npc = 0;
    std::uint32_t candidate = 0; // its character
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("candidate", candidate);
    }
};

// Guild wars: a master declares war on a guild by its name with its terms
// (the original's 0x7110: the victory points' choice, the period, the stake each master pays); the
// other master answers the proposal (GuildProposal kind 10); a master ends a war by giving it up (0x7112:
// a minute later, the other guild wins); the winner's reward asked and collected at a guild NPC (0x7114,
// 0x7113).
struct GuildWarTerms {
    std::uint8_t victory = 0;  // game::kGuildWarVictoryPoints' index (0: none, the period decides)
    std::uint8_t days = 0, hours = 0, minutes = 0;
    std::uint8_t unlimited = 0; // no end but the victory or a surrender
    std::uint32_t wager = 0;    // gold each master stakes; the winner's guild gets both
    template <typename V>
    void Visit(V& v) {
        v("victory", victory);
        v("days", days);
        v("hours", hours);
        v("minutes", minutes);
        v("unlimited", unlimited);
        v("wager", wager);
    }
};
struct GuildWarDeclare {
    static constexpr MessageId kId = 0x1116;
    std::string target; // the guild's name
    GuildWarTerms terms;
    template <typename V>
    void Visit(V& v) {
        v("target", target, 64);
        v("terms", terms);
    }
};
struct GuildWarEnd {
    static constexpr MessageId kId = 0x1117;
    std::uint32_t guild = 0; // the enemy
    template <typename V>
    void Visit(V& v) {
        v("guild", guild);
    }
};
struct GuildWarRewardQuery {
    static constexpr MessageId kId = 0x1118;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};
struct GuildWarRewardClaim {
    static constexpr MessageId kId = 0x1119;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};

// A member's fortress position (0x70FF, the master; game/Guild.h kPosition*, 0: none any more).
struct GuildPositionGrant {
    static constexpr MessageId kId = 0x111A;
    std::uint32_t character = 0;
    std::uint8_t position = 0;
    template <typename V>
    void Visit(V& v) {
        v("character", character);
        v("position", position);
    }
};

// What the guild's points went on, asked at a guild NPC (0x7501); the answer is GuildGpHistory.
struct GuildGpHistoryOpen {
    static constexpr MessageId kId = 0x111B;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};

// An ability for the guild's mercenaries bought at the guild's NPC (the original's 0x7259 {npc, the ability}; 0 releases
// them all; game/GuildMercenary.h): answered GuildAnswer{MercenaryAbility}, its members told GuildInfo again. Only on a
// shard with mercenaries (GuildInfo::mercenaries), else refused 3.
struct GuildMercenaryAbility {
    static constexpr MessageId kId = 0x111D;
    std::uint32_t npc = 0;
    std::uint8_t ability = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("ability", ability);
    }
};

// A member's answer to its master's recall (GuildRecallAsk; the original's 0x7031):
// 1 come, 0 stay. A yes is answered GuildAnswer{RecallAnswer}: 0 on its way, else 0x18C1.
struct GuildRecallAnswer {
    static constexpr MessageId kId = 0x111C;
    std::uint8_t accept = 0;
    template <typename V>
    void Visit(V& v) {
        v("accept", accept);
    }
};

// Crests: a guild's or a union's emblem, 16 x 16 pixels, 3 bytes each
// (blue, green, red), the top row first. The original's client read guildmark.bmp / unionmark.bmp,
// matched its pixels to a palette of 256 colours and the emblem went through a web site; ours keeps
// every colour and goes through the game server.
constexpr std::size_t kCrestBytes = 16 * 16 * 3;
constexpr std::uint8_t kCrestGuild = 0, kCrestUnion = 1;

// An emblem registered with its item in an inventory place (ITEM_ETC_GUILD_CREST_USER or
// ITEM_ETC_UNION_CREST_USER, used up on success; the original's item use 0x704C).
struct GuildCrestRegister {
    static constexpr MessageId kId = 0x110C;
    std::uint8_t kind = kCrestGuild;
    std::uint8_t slot = 0;
    std::vector<std::uint8_t> pixels; // kCrestBytes
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("slot", slot);
        v("pixels", pixels, kCrestBytes);
    }
};

// The emblem deleted with its item (ITEM_ETC_GUILD_CREST_DELETE / ITEM_ETC_UNION_CREST_DELETE).
struct GuildCrestDelete {
    static constexpr MessageId kId = 0x110D;
    std::uint8_t kind = kCrestGuild;
    std::uint8_t slot = 0;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("slot", slot);
    }
};

// An emblem the client has not got (seen over a head or in its own guild's data): its pixels are
// sent back (GuildCrest) while it is the current one.
struct GuildCrestRequest {
    static constexpr MessageId kId = 0x110E;
    std::uint8_t kind = kCrestGuild;
    std::uint32_t id = 0; // the guild or the union
    std::uint32_t revision = 0;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("id", id);
        v("revision", revision);
    }
};

// --- What the server tells (0x1140) -------------------------------------------------------------

// Which request an answer is for.
enum class GuildAction : std::uint8_t {
    Create = 1,
    Invite = 2,
    Join = 3, // the invited player's answer
    Leave = 4,
    Kick = 5,
    Disband = 6,
    Notice = 7,
    Permissions = 8,
    MasterTransfer = 9,
    Nickname = 10,
    LevelUp = 11,
    Donate = 12,
    CrestRegister = 13,
    CrestDelete = 14,
    UnionInvite = 16,
    UnionJoin = 17, // the invited master's answer
    UnionLeave = 18,
    UnionKick = 19,
    UnionCrestRegister = 20,
    UnionCrestDelete = 21,
    ElectionStart = 22,
    ElectionOpen = 23,
    ElectionVote = 24,
    WarDeclare = 25,
    WarAnswer = 26, // the other master's answer
    WarEnd = 27,
    WarReward = 28,
    WarClaim = 29,
    Position = 30,
    GpHistory = 31,
    RecallAnswer = 32, // a member's yes to its master's recall (0x18C1: refused)
    MercenaryAbility = 33, // an ability bought for the guild's mercenaries (GuildMercenaryAbility)
};

// A request's outcome: `error` 0 done, else the original's code (game/Guild.h GuildError); `value`
// what the code needs (the penalty's seconds left, ...).
struct GuildAnswer {
    static constexpr MessageId kId = 0x1140;
    GuildAction action = GuildAction::Create;
    std::uint16_t error = 0;
    std::uint32_t value = 0;
    template <typename V>
    void Visit(V& v) {
        v("action", action);
        v("error", error);
        v("value", value);
    }
};

// A proposal to the player (the original's 0x3080): `kind` 5 to join a guild, 6 to join a union;
// who asked (its entity and name) and its guild's name. Answered with GuildProposalAnswer within
// 30 s.
constexpr std::uint8_t kProposalGuild = 5, kProposalUnion = 6, kProposalWar = 10;
struct GuildProposal {
    static constexpr MessageId kId = 0x1141;
    std::uint8_t kind = kProposalGuild;
    std::uint32_t from = 0;
    std::string fromName;
    std::string guildName;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("from", from);
        v("fromName", fromName, 64);
        v("guildName", guildName, 64);
    }
};

// A member as the guild window lists it (the original's member entry).
struct GuildMemberRecord {
    std::uint32_t character = 0;
    std::string name;
    std::uint8_t memberClass = 10; // 0 the master, 10 a member
    std::uint8_t level = 1;
    std::uint32_t donation = 0;    // guild points given
    std::uint32_t permission = 0;  // game/Guild.h kGuildPerm*
    std::string nickname;
    std::uint32_t model = 0;       // characterdata RefObjID (its race's mark)
    std::uint8_t online = 0;       // 1 in the game
    std::uint8_t position = 0;     // in the fortress war (game/Guild.h kPosition*)
    std::uint32_t contribution = 0; // points won in guild wars (_GuildMember.Contribution)
    template <typename V>
    void Visit(V& v) {
        v("character", character);
        v("name", name, 64);
        v("memberClass", memberClass);
        v("level", level);
        v("donation", donation);
        v("permission", permission);
        v("nickname", nickname, 64);
        v("model", model);
        v("online", online);
        v("position", position);
        v("contribution", contribution);
    }
};

// The player's guild whole: on entering the world, on creating or joining one; `id` 0: the player
// has none (after leaving, being put out, a disband).
struct GuildInfo {
    static constexpr MessageId kId = 0x1142;
    std::uint32_t id = 0;
    std::string name;
    std::uint8_t level = 1;
    std::uint32_t points = 0; // guild points gathered
    std::string noticeTitle, notice;
    std::uint32_t unionId = 0;
    std::vector<GuildMemberRecord> members;
    std::uint32_t crest = 0; // the emblem's revision (0: none)
    std::uint8_t mercenaryAttr = 0; // the mercenaries' abilities bought (game::kMerc* bits)
    bool mercenaries = false;       // the shard runs the guild's mercenaries (its switch guild-mercenaries: the NPC's rows)
    template <typename V>
    void Visit(V& v) {
        v("id", id);
        v("name", name, 64);
        v("level", level);
        v("points", points);
        v("noticeTitle", noticeTitle, kGuildNoticeTitle * 4);
        v("notice", notice, kGuildNoticeBody * 4);
        v("unionId", unionId);
        v("members", members, 64);
        v("crest", crest);
        v("mercenaryAttr", mercenaryAttr);
        v("mercenaries", mercenaries);
    }
};

struct GuildMemberJoined {
    static constexpr MessageId kId = 0x1143;
    GuildMemberRecord member;
    template <typename V>
    void Visit(V& v) {
        v("member", member);
    }
};

// A member gone from the guild: `reason` 1 left, 2 put out.
struct GuildMemberRemoved {
    static constexpr MessageId kId = 0x1144;
    std::uint32_t character = 0;
    std::uint8_t reason = 1;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("character", character);
        v("reason", reason);
        v("name", name, 64);
    }
};

// A member's entry as it is now (its level, being online, rank, authority, title).
struct GuildMemberChanged {
    static constexpr MessageId kId = 0x1145;
    GuildMemberRecord member;
    template <typename V>
    void Visit(V& v) {
        v("member", member);
    }
};

struct GuildDisbanded {
    static constexpr MessageId kId = 0x1146;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("name", name, 64);
    }
};

struct GuildNoticeChanged {
    static constexpr MessageId kId = 0x1147;
    std::string title, body;
    template <typename V>
    void Visit(V& v) {
        v("title", title, kGuildNoticeTitle * 4);
        v("body", body, kGuildNoticeBody * 4);
    }
};

// A guild of the union as the union page lists it.
struct UnionGuildRecord {
    std::uint32_t id = 0;
    std::string name;
    std::uint8_t level = 1;
    std::string master;
    std::uint32_t masterModel = 0;
    std::uint16_t members = 0;
    template <typename V>
    void Visit(V& v) {
        v("id", id);
        v("name", name, 64);
        v("level", level);
        v("master", master, 64);
        v("masterModel", masterModel);
        v("members", members);
    }
};

// The player's union whole (on entering, on any change); `id` 0: none. The first guild leads.
struct UnionInfo {
    static constexpr MessageId kId = 0x1148;
    std::uint32_t id = 0;
    std::vector<UnionGuildRecord> guilds;
    std::uint32_t crest = 0; // the union's emblem's revision (0: none)
    template <typename V>
    void Visit(V& v) {
        v("id", id);
        v("guilds", guilds, 8);
        v("crest", crest);
    }
};

// What happened to the union (its system line): 1 formed or a guild joined, 2 a guild left, 3 a
// guild put out, 4 dissolved; the guild it was about.
struct UnionChanged {
    static constexpr MessageId kId = 0x1149;
    std::uint8_t event = 1;
    std::string guild;
    template <typename V>
    void Visit(V& v) {
        v("event", event);
        v("guild", guild, 64);
    }
};

// Guild points and the level now (a donation, a level up).
struct GuildPoints {
    static constexpr MessageId kId = 0x114A;
    std::uint8_t level = 1;
    std::uint32_t points = 0;
    template <typename V>
    void Visit(V& v) {
        v("level", level);
        v("points", points);
    }
};

// The election as a member sees it: to every member online when it starts, at each ballot and when
// it ends (`active` 0); `show` 1 in the answer to GuildElectionOpen (its window opens). The
// candidates with their votes, the seconds left, this member's ballot (a candidate, 0: none) and
// whether it may vote (on within the last three days).
struct GuildElectionCandidate {
    std::uint32_t character = 0;
    std::string name;
    std::uint16_t votes = 0;
    template <typename V>
    void Visit(V& v) {
        v("character", character);
        v("name", name, 64);
        v("votes", votes);
    }
};
struct GuildElection {
    static constexpr MessageId kId = 0x114C;
    std::uint8_t active = 0;
    std::uint8_t show = 0;
    std::uint32_t secondsLeft = 0;
    std::vector<GuildElectionCandidate> candidates;
    std::uint32_t ballot = 0;
    std::uint8_t mayVote = 0;
    template <typename V>
    void Visit(V& v) {
        v("active", active);
        v("show", show);
        v("secondsLeft", secondsLeft);
        v("candidates", candidates, 64);
        v("ballot", ballot);
        v("mayVote", mayVote);
    }
};

// The election over: the guild and its new master ("[%s] Guild's new Guildmaster is now [%s]"), or
// no winner (`winner` empty: "cancelled due to inability to satisfy requirements").
struct GuildElectionEnded {
    static constexpr MessageId kId = 0x114D;
    std::string guild;
    std::string winner;
    template <typename V>
    void Visit(V& v) {
        v("guild", guild, 64);
        v("winner", winner, 64);
    }
};

// A war declared on the player's guild, to its master (the agreement box shows the terms).
struct GuildWarProposal {
    static constexpr MessageId kId = 0x114E;
    std::string fromName, guildName;
    GuildWarTerms terms;
    template <typename V>
    void Visit(V& v) {
        v("fromName", fromName, 64);
        v("guildName", guildName, 64);
        v("terms", terms);
    }
};

// The player's guild's wars (on entering, on any change): each enemy, the points, the terms, the time
// left (0xFFFFFFFF: none), whether it is being given up (`ending`: its seconds left).
struct GuildWarRecord {
    std::uint32_t guild = 0;
    std::string name, master;
    std::uint8_t level = 1;
    std::uint16_t members = 0;
    std::uint32_t ours = 0, theirs = 0;
    GuildWarTerms terms;
    std::uint32_t secondsLeft = 0xFFFFFFFFu;
    std::uint32_t ending = 0;
    template <typename V>
    void Visit(V& v) {
        v("guild", guild);
        v("name", name, 64);
        v("master", master, 64);
        v("level", level);
        v("members", members);
        v("ours", ours);
        v("theirs", theirs);
        v("terms", terms);
        v("secondsLeft", secondsLeft);
        v("ending", ending);
    }
};
struct GuildWars {
    static constexpr MessageId kId = 0x114F;
    std::vector<GuildWarRecord> wars;
    template <typename V>
    void Visit(V& v) {
        v("wars", wars, 64);
    }
};

// A war's news for its guilds' members (their system lines): 1 begun (a, b the guilds), 2 refused (a
// the guild that refused), 3 a kill (a the killer, b the victim, c the victim's guild), 4 ending in a
// minute (a, b), 5 over (a, b), 6 won against a, 7 lost against a.
struct GuildWarNews {
    static constexpr MessageId kId = 0x1151;
    std::uint8_t kind = 0;
    std::string a, b, c;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("a", a, 64);
        v("b", b, 64);
        v("c", c, 64);
    }
};

// The guild's war reward waiting to be collected (the answer to GuildWarRewardQuery).
struct GuildWarReward {
    static constexpr MessageId kId = 0x1152;
    std::uint64_t gold = 0;
    template <typename V>
    void Visit(V& v) {
        v("gold", gold);
    }
};

// One spending of the guild's points (_GPHistory: who, how many, why, when; 0xB501).
struct GuildGpRecord {
    std::string name;
    std::uint32_t points = 0;
    std::uint8_t reason = 0; // game/Guild.h kGpReason*
    std::uint16_t year = 0;
    std::uint8_t month = 0, day = 0, hour = 0, minute = 0;
    template <typename V>
    void Visit(V& v) {
        v("name", name, 64);
        v("points", points);
        v("reason", reason);
        v("year", year);
        v("month", month);
        v("day", day);
        v("hour", hour);
        v("minute", minute);
    }
};
struct GuildGpHistory {
    static constexpr MessageId kId = 0x1153;
    std::vector<GuildGpRecord> rows;
    template <typename V>
    void Visit(V& v) {
        v("rows", rows, 200);
    }
};

// The guild's master recalls its members (the original's 0x7030 relayed to each member online;
// 10): "Guild master has recalled guild members. Agree to recall?" (UIIT_MSG_GUILD_RECALL_ANSWER), answered with
// GuildRecallAnswer within `seconds` (then "Response time has passed, so recall has been cancelled.").
struct GuildRecallAsk {
    static constexpr MessageId kId = 0x1154;
    std::string master;
    std::uint32_t seconds = 0;
    template <typename V>
    void Visit(V& v) {
        v("master", master, 64);
        v("seconds", seconds);
    }
};

// An emblem's pixels (the answer to GuildCrestRequest).
struct GuildCrest {
    static constexpr MessageId kId = 0x114B;
    std::uint8_t kind = kCrestGuild;
    std::uint32_t id = 0;
    std::uint32_t revision = 0;
    std::vector<std::uint8_t> pixels; // kCrestBytes
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("id", id);
        v("revision", revision);
        v("pixels", pixels, kCrestBytes);
    }
};

// A player's guild for those who see it (the original's 0x30FF; `guild` 0: none any more, its
// 0x3100): drawn over its head as [guild] or [guild * nickname], the guild's and the union's
// emblems left of it (their revisions; 0: none).
struct EntityGuild {
    static constexpr MessageId kId = 0x1150;
    std::uint32_t entity = 0;
    std::uint32_t guild = 0;
    std::string name;
    std::string nickname;
    std::uint32_t unionId = 0;
    std::uint32_t crest = 0;
    std::uint32_t unionCrest = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("guild", guild);
        v("name", name, 64);
        v("nickname", nickname, 64);
        v("unionId", unionId);
        v("crest", crest);
        v("unionCrest", unionCrest);
    }
};

// Every message the server sends of guilds, as one session event.
using GuildEvent = std::variant<GuildAnswer, GuildProposal, GuildInfo, GuildMemberJoined, GuildMemberRemoved, GuildMemberChanged,
                                GuildDisbanded, GuildNoticeChanged, UnionInfo, UnionChanged, GuildPoints, EntityGuild, GuildCrest,
                                GuildElection, GuildElectionEnded, GuildWarProposal, GuildWars, GuildWarNews, GuildWarReward, GuildGpHistory,
                                GuildRecallAsk>;

// The guild messages' range (0x1100-0x117F); the server's are from 0x1140.
inline bool IsGuildMessage(MessageId id) { return id >= 0x1100 && id < 0x1180; }

// A server's guild message as the session's event; false for another id or a malformed payload.
inline bool DecodeGuildEvent(MessageId id, std::span<const std::byte> payload, GuildEvent& out) {
    const auto as = [&]<typename T>(T message) {
        if (!Decode(payload, message)) return false;
        out = std::move(message);
        return true;
    };
    switch (id) {
    case GuildAnswer::kId: return as(GuildAnswer{});
    case GuildProposal::kId: return as(GuildProposal{});
    case GuildInfo::kId: return as(GuildInfo{});
    case GuildMemberJoined::kId: return as(GuildMemberJoined{});
    case GuildMemberRemoved::kId: return as(GuildMemberRemoved{});
    case GuildMemberChanged::kId: return as(GuildMemberChanged{});
    case GuildDisbanded::kId: return as(GuildDisbanded{});
    case GuildNoticeChanged::kId: return as(GuildNoticeChanged{});
    case UnionInfo::kId: return as(UnionInfo{});
    case UnionChanged::kId: return as(UnionChanged{});
    case GuildPoints::kId: return as(GuildPoints{});
    case EntityGuild::kId: return as(EntityGuild{});
    case GuildCrest::kId: return as(GuildCrest{});
    case GuildElection::kId: return as(GuildElection{});
    case GuildElectionEnded::kId: return as(GuildElectionEnded{});
    case GuildWarProposal::kId: return as(GuildWarProposal{});
    case GuildWars::kId: return as(GuildWars{});
    case GuildWarNews::kId: return as(GuildWarNews{});
    case GuildWarReward::kId: return as(GuildWarReward{});
    case GuildGpHistory::kId: return as(GuildGpHistory{});
    case GuildRecallAsk::kId: return as(GuildRecallAsk{});
    default: return false;
    }
}

inline const char* GuildMessageName(MessageId id) {
    switch (id) {
    case GuildAnswer::kId: return "GuildAnswer";
    case GuildProposal::kId: return "GuildProposal";
    case GuildInfo::kId: return "GuildInfo";
    case GuildMemberJoined::kId: return "GuildMemberJoined";
    case GuildMemberRemoved::kId: return "GuildMemberRemoved";
    case GuildMemberChanged::kId: return "GuildMemberChanged";
    case GuildDisbanded::kId: return "GuildDisbanded";
    case GuildNoticeChanged::kId: return "GuildNoticeChanged";
    case UnionInfo::kId: return "UnionInfo";
    case UnionChanged::kId: return "UnionChanged";
    case GuildPoints::kId: return "GuildPoints";
    case EntityGuild::kId: return "EntityGuild";
    case GuildCrest::kId: return "GuildCrest";
    case GuildElection::kId: return "GuildElection";
    case GuildElectionEnded::kId: return "GuildElectionEnded";
    case GuildWarProposal::kId: return "GuildWarProposal";
    case GuildWars::kId: return "GuildWars";
    case GuildWarNews::kId: return "GuildWarNews";
    case GuildWarReward::kId: return "GuildWarReward";
    case GuildGpHistory::kId: return "GuildGpHistory";
    case GuildRecallAsk::kId: return "GuildRecallAsk";
    default: return nullptr;
    }
}

} // namespace sr::game::proto
