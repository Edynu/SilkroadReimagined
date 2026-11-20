#pragma once

// The jobs in the original's tables: a character's league (`_CharTrijob`), its alias
// (`_Char.NickName16`, every alias ever given kept in `_CharNickNameList`), and the database's constants the rules
// read (game::JobRules: `_RefShardContentConfig`, `_RefDropGold`, `_RefLevel`).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sr/game/Jobs.h"
#include "sr/server/Database.h"
#include "sr/server/Store.h"

namespace sr::server {

// A character's league and alias (none: JobType::None at level 1, an empty alias) and when it last left a league
// (`leftAt`, Unix seconds; 0: never, or its 7 days are over). False: the query failed.
bool LoadJob(Database& db, std::int64_t character, game::JobInfo& job, std::string& alias, std::int64_t& leftAt, std::string& error);
// Its league as it stands now (the row made when missing).
bool SaveJob(Database& db, std::int64_t character, const game::JobInfo& job);
// Its alias (an empty one clears it); a new one is also kept in `_CharNickNameList`.
bool SaveAlias(Database& db, std::int64_t character, const std::string& alias);

// Joining the league of `type`: its level and experience from the character's `_OldTrijob`
// row (1 / 0 without one), which then goes (`_QueryOldTrijobInfo`, `_DeleteOldTrijobData`); a trader's safe-trade row
// made (`_InsertCharTrijobSafeTradeData` 0, 0); the wait's timed job gone. `joined` the league as it now stands.
bool JoinLeague(Database& db, std::int64_t character, game::JobType type, game::JobInfo& joined);
// Leaving it: the type, level and experience cleared, the alias with them (the 1.188 client empties it on the
// answer; it stays in `_CharNickNameList`, never given again), the safe-trade row deleted, the 7-day wait kept as the
// original's timed job (`_TimedJob` category 2, job 2; ours: Data1 the time it left, `leftAt`).
bool LeaveLeague(Database& db, std::int64_t character, std::int64_t leftAt);

// Whether an alias is free (`_CanAssignThisNickName`): 1 free, -1 a character's name, -2 another alias, 0 the query
// failed. Names compare ignoring case, as the original's collation does.
int AliasFree(Database& db, std::string_view alias);
// Gives the character the alias if it is free (`_AssignNickName`, then `_Char.NickName16`), in one transaction that
// holds the shard's alias lock: AliasFree's answer.
int AssignAlias(Database& db, std::int64_t character, std::string_view alias);

// The levels and experience before the leagues (`_GetOldTrijobData`); false: none (or the query failed).
struct OldJobs {
    int traderLevel = 1, thiefLevel = 1, hunterLevel = 1;
    std::uint32_t traderExp = 0, thiefExp = 0, hunterExp = 0;
};
bool LoadOldJobs(Database& db, std::int64_t character, OldJobs& old);

// The specialty NPCs' markets (`_ItemQuotation`, in service) of the NPCs `npcs` (their RefObjIDs), the quotes in
// hundredths. False: the query failed.
bool LoadMarkets(Database& db, const std::vector<std::uint32_t>& npcs, std::vector<game::MarketRow>& rows, std::string& error);
// Their stocks as they are now (`CurStockAmount`), by row id.
bool SaveMarketStocks(Database& db, const std::vector<std::pair<std::uint32_t, std::int64_t>>& stocks);

// The rules' constants from the reference tables; a table missing keeps the defaults of game::JobRules (its scale,
// its factor) or leaves its list empty. False only when a query of a table that exists fails.
bool LoadJobRules(Database& db, game::JobRules& rules, std::string& error);
// The week's pool of a job (`_TrijobRewards`, the original's `_AddTrijobReward`): `amount` added (its row made when missing).
bool AddTrijobReward(Database& db, game::JobType type, std::uint64_t amount);

// The bargain goods' lock-out (`_TimedJob` 100/7: ours, the original's category 2 id 7 is the academy's founding wait, its end in Data1 as Unix seconds; 0 none), and its save.
bool LoadBargainLock(Database& db, std::int64_t character, std::int64_t& until, std::string& error);
void SaveBargainLock(Database& db, std::int64_t character, std::int64_t until);
// A trader's safe trade row (none: not a trader league member), and its save.
bool LoadSafeTrade(Database& db, std::int64_t character, std::optional<SafeTradeRow>& out, std::string& error);
void SaveSafeTrade(Database& db, std::int64_t character, const SafeTradeRow& row);

// The week (the original's `_UpdateTriangleJobWeekly`, an operator's job: SroDb --job-week), in
// one transaction: each thief and hunter gets floor(pool x its contribution / the job's total) added to its Reward
// (capped at INT_MAX) and the pools are emptied; the top 50 of each job by level and exp go to
// `Tab_RefRanking_<Trader|Robber|Hunter>Activity`, and, where any contribution was made, the top 50 by contribution to
// `Tab_RefRanking_<...>Contribution`, those contributions then set to 0.
bool RunJobWeek(Database& db, std::string& error);

// A job's ranking as the league NPC shows it (`Tab_RefRanking_*`, written by the week): kind 0 activity (exp in
// `value`), 1 the week's contribution or donation; in rank order.
struct JobRankRow {
    int rank = 0;
    std::string alias;
    int level = 0;
    std::int64_t value = 0;
    int country = 0; // activity's only (the player's race's, `_RefObjCommon.Country`)
};
bool LoadJobRanking(Database& db, game::JobType type, int kind, std::vector<JobRankRow>& out, std::string& error);
// A thief's or hunter's outcome of the weeks (`_CharTrijob.Reward`), and its collection: the reward read and set to 0
// in one statement (none: 0).
std::int64_t TakeJobReward(Database& db, std::int64_t character);

// The bandits' data: the thief and hunter monsters in service by zone and level (`_RefObjCommon`
// TID 1/2/1/2 and 1/2/1/3 with their `_RefObjChar.Lvl`; zone 0 the codenames `MOB_THIEF*` / `MOB_HUNTER*`, 1
// `MOB_EU_THIEF*` / `MOB_EU_HUNTER*`), and each region's kind (`_RefRegion`: IsBattleField 0 a town, its continent's
// bandit zone, game::BanditZoneOf).
struct RegionKind {
    bool town = false;
    std::int8_t banditZone = -1;
};
bool LoadBanditData(Database& db, game::BanditTable& table, std::unordered_map<std::uint16_t, RegionKind>& regions, std::string& error);

} // namespace sr::server
