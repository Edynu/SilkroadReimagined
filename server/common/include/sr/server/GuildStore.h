#pragma once

// Guilds and unions in the original's tables: _Guild,
// _GuildMember, _AlliedClans, _GuildChest, _Char.GuildID and the leave penalty in _TimedJob. Our
// own queries, written from what the original's procedures do (_Guild_Create, _Guild_FnAddMember,
// _Guild_DelMember_NoTX, _Guild_DeleteGuild_NoTX, _Guild_Delegate_Master, _AllianceCreate,
// _AllianceRemove); none of them is called.

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sr::server {

class Database;

struct GuildMemberRow {
    std::int64_t character = 0;
    std::string name;
    int memberClass = 10;
    int level = 1;
    std::uint32_t donation = 0;
    std::uint32_t permission = 0;
    std::string nickname;
    std::uint32_t model = 0;
    std::int64_t awaySeconds = 0; // since _Char.LastLogout, as loaded (the election's rules)
    int position = 0;             // _GuildMember.SiegeAuthority
    std::uint32_t contribution = 0; // _GuildMember.Contribution
};

// One line of _GPHistory.
struct GpHistoryRow {
    std::string name;
    std::uint32_t points = 0;
    int reason = 0;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
};

struct GuildRow {
    std::uint32_t id = 0;
    std::string name;
    int level = 1;
    std::uint32_t points = 0; // GatheredSP
    std::string noticeTitle, notice;
    std::uint32_t alliance = 0;
    std::uint32_t lastCrest = 0, crest = 0; // LastCrestRev, CurCrestRev (0: no emblem)
    std::uint32_t booty = 0;                // war rewards to collect (_Guild.Booty)
    std::vector<GuildMemberRow> members;
};

// A guild war (_GuildWar): the challenger (Guild1) and the acceptor, both
// stakes (LodgedGold), the victory's index, the end (none: unlimited), the points; ours in its spare
// columns: Data1 the period asked (days << 16 | hours << 8 | minutes, 1 << 24 unlimited), Data2 each
// stake.
struct GuildWarRow {
    std::uint32_t id = 0;
    std::uint32_t first = 0, second = 0;
    std::uint8_t victory = 0;
    std::uint32_t lodged = 0, wager = 0;
    std::int64_t secondsLeft = -1; // -1: no end
    std::uint32_t period = 0;
    std::uint32_t points1 = 0, points2 = 0;
};

// A union: its guilds by slot (0 the leading guild; 0 an empty slot).
struct AllianceRow {
    std::uint32_t id = 0;
    std::array<std::uint32_t, 8> guilds{};
    std::uint32_t lastCrest = 0, crest = 0; // LastCrestRev, CurCrestRev
};

// An emblem's pixels (ours, sr_crests: the original kept them on its crest web site): a guild's
// (kind 0) or a union's (1), the current revision only.
struct CrestRow {
    std::uint8_t kind = 0;
    std::uint32_t id = 0;
    std::uint32_t revision = 0;
    std::vector<std::uint8_t> pixels;
};

// A character's leave penalty: the seconds it has left now.
struct GuildPenaltyRow {
    std::int64_t character = 0;
    std::int64_t secondsLeft = 0;
};

struct GuildData {
    std::vector<GuildRow> guilds;
    std::vector<AllianceRow> alliances;
    std::vector<GuildPenaltyRow> penalties;
    std::vector<CrestRow> crests;
    std::vector<GuildWarRow> wars;
};

// Every guild (the placeholder 0 left out) with its members, every union, every running penalty.
bool LoadGuilds(Database& db, GuildData& out, std::string& error);
// Only these guilds and unions, as they are now (a guild or union not found is gone), and their
// emblems; no penalties (a shard's other game server changed them).
bool LoadSomeGuilds(Database& db, const std::vector<std::uint32_t>& guilds, const std::vector<std::uint32_t>& alliances, GuildData& out,
                    std::string& error);

// Guild wars, written as the original's procedures do (_GuildWar_Begin, _GuildWar_AccountCombatResult,
// _GuildWar_EndOfWar): a war begun (its id, 0 on failure); a kill's points to the killer's side, its
// kill and contribution to the killer, its death to the victim; the war over, each guild's reward added
// to its _Guild.Booty and the war's row gone; a guild's booty collected (what it was; 0 when none).
std::uint32_t BeginGuildWar(Database& db, const GuildWarRow& war);
bool CountGuildWarKill(Database& db, std::uint32_t war, bool firstScores, std::uint32_t points, std::uint32_t killerGuild, std::int64_t killer,
                       std::uint32_t victimGuild, std::int64_t victim);
bool EndGuildWar(Database& db, std::uint32_t war, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& rewards);
std::uint32_t TakeGuildBooty(Database& db, std::uint32_t guild);
// Every war (the shard's other servers load them again after a change).
bool LoadGuildWars(Database& db, std::vector<GuildWarRow>& out, std::string& error);

// The guild storage's lock across a shard's game servers (sr_guild_storage_lock: one member at a time): taken by this
// server for `character` when free or already its own; else
// the holder's name. Released on closing, and all of a server's when it starts.
std::string TakeGuildStorageLock(Database& db, std::uint32_t guild, std::uint16_t server, std::int64_t character, bool& taken);
bool ReleaseGuildStorageLock(Database& db, std::uint32_t guild, std::uint16_t server);
bool ReleaseServerGuildStorageLocks(Database& db, std::uint16_t server);

// A guild of level 1 with its creator as master (class 0, every permission, siege authority 1),
// the creator's _Char.GuildID, and its storage's places (one per _RefDummySlot row): the new id, or
// 0 (`nameTaken` when a guild of that name exists, ignoring case).
std::uint32_t CreateGuild(Database& db, const std::string& name, std::int64_t creator, bool& nameTaken, std::string& error);
// A member in (its name, level and model from _Char) and its _Char.GuildID.
bool AddGuildMember(Database& db, std::uint32_t guild, std::int64_t character, int memberClass, std::uint32_t permission);
// A member out (not the master) and its _Char.GuildID cleared.
bool RemoveGuildMember(Database& db, std::uint32_t guild, std::int64_t character);
// The leave penalty: _TimedJob category 2, job 1, its seconds in TimeToKeep; ours: Data1 holds the
// time it was written (Unix seconds), so it keeps running while the character is away.
bool AddGuildPenalty(Database& db, std::int64_t character, std::uint32_t seconds);
// The wait of Return to fortress (the original's timed job 2, 5): false while one runs, else it starts.
bool TakeFortressReturn(Database& db, std::int64_t character, std::uint32_t seconds);
// A character's wait kept as the original's timed job `category` / `job` (_TimedJob; ours: Data1 the time it was written,
// Unix seconds, so it runs on while the character is away): false while one runs, else it starts for `seconds` (the guild
// recall's 2 / 3).
bool TakeTimedWait(Database& db, std::int64_t character, std::uint8_t category, std::uint32_t job, std::uint32_t seconds);
// The guild gone: its members' _Char.GuildID cleared, its members, storage places and row deleted.
bool DisbandGuild(Database& db, std::uint32_t guild);
// The master's place to another member (the old master becomes a member with no permissions).
bool DelegateGuildMaster(Database& db, std::uint32_t guild, std::int64_t oldMaster, std::int64_t newMaster);
bool SaveGuildPermissions(Database& db, std::uint32_t guild, const std::vector<std::pair<std::int64_t, std::uint32_t>>& members);
bool SaveGuildNotice(Database& db, std::uint32_t guild, const std::string& title, const std::string& body);
bool SaveGuildNickname(Database& db, std::uint32_t guild, std::int64_t character, const std::string& nickname);
bool SaveGuildMemberLevel(Database& db, std::int64_t character, int level);
// The guild's level and points, and (character not 0) a member's donation total.
bool SaveGuildPosition(Database& db, std::uint32_t guild, std::int64_t character, int position);
bool AddGpHistory(Database& db, std::uint32_t guild, const std::string& name, std::uint32_t points, int reason);
// The newest first, at most 200.
std::vector<GpHistoryRow> LoadGpHistory(Database& db, std::uint32_t guild);
bool SaveGuildPoints(Database& db, std::uint32_t guild, int level, std::uint32_t points, std::int64_t character, std::uint32_t donation);
// A union of two guilds (the first leads): its id, 0 on failure.
std::uint32_t CreateAlliance(Database& db, std::uint32_t leader, std::uint32_t first);
// A union's slots as given, and each guild's _Guild.Alliance (those no longer in it cleared).
bool SaveAlliance(Database& db, const AllianceRow& alliance);
// The union gone (its guilds' _Guild.Alliance cleared, its emblem with it).
bool RemoveAlliance(Database& db, std::uint32_t alliance);
// An emblem registered (`pixels`, at revision `revision` = `last`) or deleted (`revision` 0, no
// pixels): the guild's or the union's LastCrestRev / CurCrestRev and its row of sr_crests.
bool SaveCrest(Database& db, std::uint8_t kind, std::uint32_t id, std::uint32_t last, std::uint32_t revision, const std::vector<std::uint8_t>& pixels);

} // namespace sr::server
