#include "sr/server/JobStore.h"

#include <algorithm>
#include <ctime>

#include "sr/core/Log.h"

namespace sr::server {

namespace {

// The league's 7-day wait as the original keeps it (timed job category 2, job 2, 604,800 s).
constexpr int kLeagueWaitCategory = 2, kLeagueWaitJob = 2;
constexpr std::int64_t kAliasLock = 0x5352'414C'4941; // "SRALIA"

bool Exists(Database& db, const char* table) {
    const QueryResult r = db.Query("SELECT to_regclass($1) IS NOT NULL", {table});
    return r.Ok() && r.Rows() == 1 && r.Bool(0, 0);
}

} // namespace

bool LoadJob(Database& db, std::int64_t character, game::JobInfo& job, std::string& alias, std::int64_t& leftAt, std::string& error) {
    job = {};
    alias.clear();
    leftAt = 0;
    QueryResult r = db.Query("SELECT j.jobtype, j.level, j.exp, j.contribution, j.reward, c.nickname16 FROM _char c "
                             "LEFT JOIN _chartrijob j ON j.charid = c.charid WHERE c.charid = $1",
                             {character});
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    if (r.Rows() == 0) return true;
    if (!r.IsNull(0, 0)) {
        job.type = game::JobType(std::clamp<std::int64_t>(r.Int(0, 0), 0, 3));
        job.level = int(std::clamp<std::int64_t>(r.Int(0, 1), 1, game::kJobMaxLevel));
        job.exp = std::uint64_t(std::max<std::int64_t>(0, r.Int(0, 2)));
        job.contribution = std::uint32_t(std::max<std::int64_t>(0, r.Int(0, 3)));
        job.reward = std::uint32_t(std::max<std::int64_t>(0, r.Int(0, 4)));
    }
    alias = r.Text(0, 5);
    QueryResult wait = db.Query("SELECT coalesce(max(data1), 0) FROM _timedjob WHERE charid = $1 AND category = $2 AND jobid = $3",
                                {character, std::int64_t(kLeagueWaitCategory), std::int64_t(kLeagueWaitJob)});
    if (!wait.Ok()) {
        error = wait.Error();
        return false;
    }
    leftAt = wait.Int(0, 0);
    if (leftAt != 0 && std::int64_t(std::time(nullptr)) - leftAt >= game::kJobRejoinSeconds) leftAt = 0;
    return true;
}

bool JoinLeague(Database& db, std::int64_t character, game::JobType type, game::JobInfo& joined) {
    std::string error;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            joined = {};
            joined.type = type;
            static constexpr const char* kOld[] = {"", "joblvl_trader, trader_exp", "joblvl_robber, robber_exp", "joblvl_hunter, hunter_exp"};
            QueryResult old = tx.Query(std::string("SELECT ") + kOld[int(type)] + " FROM _oldtrijob WHERE charid = $1", {character});
            if (!old.Ok()) return false;
            if (old.Rows() == 1) {
                joined.level = int(std::clamp<std::int64_t>(old.Int(0, 0), 1, game::kJobMaxLevel));
                joined.exp = std::uint64_t(std::max<std::int64_t>(0, old.Int(0, 1)));
            }
            if (!tx.Query("INSERT INTO _chartrijob (charid, jobtype, level, exp, contribution, reward) VALUES ($1, $2, $3, $4, 0, 0) "
                          "ON CONFLICT (charid) DO UPDATE SET jobtype = EXCLUDED.jobtype, level = EXCLUDED.level, exp = EXCLUDED.exp, contribution = 0",
                          {character, std::int64_t(type), std::int64_t(joined.level), std::int64_t(std::min<std::uint64_t>(joined.exp, 0x7FFFFFFF))})
                     .Ok()) {
                return false;
            }
            if (type == game::JobType::Trader) {
                if (!tx.Query("DELETE FROM _chartrijobsafetrade WHERE charid = $1", {character}).Ok() ||
                    !tx.Query("INSERT INTO _chartrijobsafetrade (charid, ablecount, status, lastsafetrade) VALUES ($1, 0, 0, localtimestamp)", {character}).Ok()) {
                    return false;
                }
            }
            return tx.Query("DELETE FROM _oldtrijob WHERE charid = $1", {character}).Ok() &&
                   tx.Query("DELETE FROM _timedjob WHERE charid = $1 AND category = $2 AND jobid = $3",
                            {character, std::int64_t(kLeagueWaitCategory), std::int64_t(kLeagueWaitJob)})
                       .Ok();
        },
        error);
    if (!ok) core::LogWarning("joining a league ({}): {}", character, error);
    return ok;
}

bool LeaveLeague(Database& db, std::int64_t character, std::int64_t leftAt) {
    std::string error;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            // The week's contribution and the reward unclaimed go too (the original's leave zeroes its job record).
            return tx.Query("UPDATE _chartrijob SET jobtype = 0, level = 1, exp = 0, contribution = 0, reward = 0 WHERE charid = $1", {character}).Ok() &&
                   tx.Query("UPDATE _char SET nickname16 = '' WHERE charid = $1", {character}).Ok() &&
                   tx.Query("DELETE FROM _chartrijobsafetrade WHERE charid = $1", {character}).Ok() &&
                   tx.Query("DELETE FROM _timedjob WHERE charid = $1 AND category = $2 AND jobid = $3",
                            {character, std::int64_t(kLeagueWaitCategory), std::int64_t(kLeagueWaitJob)})
                       .Ok() &&
                   tx.Query("INSERT INTO _timedjob (charid, category, jobid, timetokeep, data1, data2, serial64, jid) "
                            "VALUES ($1, $2, $3, $4, $5, 0, 0, coalesce((SELECT userjid FROM _user WHERE charid = $1), 0))",
                            {character, std::int64_t(kLeagueWaitCategory), std::int64_t(kLeagueWaitJob), std::int64_t(game::kJobRejoinSeconds), leftAt})
                       .Ok();
        },
        error);
    if (!ok) core::LogWarning("leaving a league ({}): {}", character, error);
    return ok;
}

namespace {

int AliasFreeIn(Database& db, std::string_view alias) {
    const std::string name(alias);
    QueryResult r = db.Query("SELECT EXISTS (SELECT 1 FROM _charnamelist WHERE lower(charname16) = lower($1)) OR "
                             "EXISTS (SELECT 1 FROM _char WHERE lower(charname16) = lower($1)), "
                             "EXISTS (SELECT 1 FROM _charnicknamelist WHERE lower(nickname16) = lower($1))",
                             {name});
    if (!r.Ok() || r.Rows() != 1) return 0;
    if (r.Bool(0, 0)) return -1;
    if (r.Bool(0, 1)) return -2;
    return 1;
}

} // namespace

int AliasFree(Database& db, std::string_view alias) { return AliasFreeIn(db, alias); }

int AssignAlias(Database& db, std::int64_t character, std::string_view alias) {
    int answer = 0;
    std::string error;
    db.Transaction(
        [&](Database& tx) {
            // One assignment at a time across the shard's game servers (ours: the original's server is one process).
            if (!tx.Query("SELECT pg_advisory_xact_lock($1)", {kAliasLock}).Ok()) return false;
            answer = AliasFreeIn(tx, alias);
            if (answer != 1) return false;
            const std::string name(alias);
            if (!tx.Query("INSERT INTO _charnicknamelist (nickname16, charid) VALUES ($1, $2)", {name, character}).Ok() ||
                !tx.Query("UPDATE _char SET nickname16 = $2 WHERE charid = $1", {character, name}).Ok()) {
                answer = 0;
                return false;
            }
            return true;
        },
        error);
    if (answer == 0) core::LogWarning("an alias for {}: {}", character, error);
    return answer;
}

bool LoadOldJobs(Database& db, std::int64_t character, OldJobs& old) {
    QueryResult r = db.Query("SELECT joblvl_trader, trader_exp, joblvl_robber, robber_exp, joblvl_hunter, hunter_exp FROM _oldtrijob WHERE charid = $1",
                             {character});
    if (!r.Ok() || r.Rows() != 1) return false;
    const auto exp = [&](int c) { return std::uint32_t(std::max<std::int64_t>(0, r.Int(0, c))); };
    old.traderLevel = int(r.Int(0, 0));
    old.traderExp = exp(1);
    old.thiefLevel = int(r.Int(0, 2));
    old.thiefExp = exp(3);
    old.hunterLevel = int(r.Int(0, 4));
    old.hunterExp = exp(5);
    return true;
}

bool SaveJob(Database& db, std::int64_t character, const game::JobInfo& job) {
    // The original's columns are 32-bit (exp, contribution, reward): held to INT_MAX as its procedures hold them.
    const auto cap = [](std::uint64_t v) { return std::int64_t(std::min<std::uint64_t>(v, 0x7FFFFFFF)); };
    return db.Query("INSERT INTO _chartrijob (charid, jobtype, level, exp, contribution, reward) VALUES ($1, $2, $3, $4, $5, $6) "
                    "ON CONFLICT (charid) DO UPDATE SET jobtype = EXCLUDED.jobtype, level = EXCLUDED.level, exp = EXCLUDED.exp, "
                    "contribution = EXCLUDED.contribution, reward = EXCLUDED.reward",
                    {character, std::int64_t(job.type), std::int64_t(job.level), cap(job.exp), cap(job.contribution), cap(job.reward)})
        .Ok();
}

bool SaveAlias(Database& db, std::int64_t character, const std::string& alias) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            if (!tx.Query("UPDATE _char SET nickname16 = $2 WHERE charid = $1", {character, alias}).Ok()) return false;
            if (alias.empty()) return true;
            return tx.Query("INSERT INTO _charnicknamelist (nickname16, charid) VALUES ($1, $2) ON CONFLICT DO NOTHING", {alias, character}).Ok();
        },
        error);
}

bool LoadMarkets(Database& db, const std::vector<std::uint32_t>& npcs, std::vector<game::MarketRow>& rows, std::string& error) {
    rows.clear();
    if (npcs.empty() || !Exists(db, "_itemquotation")) return true;
    std::string ids = "{";
    for (const std::uint32_t npc : npcs) ids += (ids.size() > 1 ? "," : "") + std::to_string(npc);
    ids += "}";
    QueryResult r = db.Query("SELECT id, assocnpc, refitemid, round(basequot * 100), round(quot_lb * 100), round(quot_ub * 100), basestockamount, "
                             "fluctuateamount, curstockamount FROM _itemquotation WHERE service = 1 AND assocnpc = ANY($1::int[]) ORDER BY id",
                             {ids});
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    for (int k = 0; k < r.Rows(); ++k) {
        game::MarketRow m;
        m.id = std::uint32_t(r.Int(k, 0));
        m.npc = std::uint32_t(r.Int(k, 1));
        m.item = std::uint32_t(r.Int(k, 2));
        m.baseQuote = int(r.Int(k, 3));
        m.lowQuote = int(r.Int(k, 4));
        m.highQuote = int(r.Int(k, 5));
        m.baseStock = r.Int(k, 6);
        m.fluctuate = std::max<std::int64_t>(1, r.Int(k, 7));
        m.stock = r.Int(k, 8);
        rows.push_back(m);
    }
    return true;
}

bool SaveMarketStocks(Database& db, const std::vector<std::pair<std::uint32_t, std::int64_t>>& stocks) {
    bool ok = true;
    for (const auto& [id, stock] : stocks) {
        ok = db.Query("UPDATE _itemquotation SET curstockamount = $2 WHERE id = $1", {std::int64_t(id), std::int64_t(std::clamp<std::int64_t>(stock, 0, 0x7FFFFFFF))}).Ok() && ok;
    }
    return ok;
}

bool AddTrijobReward(Database& db, game::JobType type, std::uint64_t amount) {
    if (!Exists(db, "_trijobrewards")) return false;
    const std::int64_t add = std::int64_t(std::min<std::uint64_t>(amount, 0x7FFFFFFF));
    QueryResult r = db.Query("UPDATE _trijobrewards SET reward = least(2147483647, reward::bigint + $2) WHERE jobtype = $1", {std::int64_t(type), add});
    if (r.Ok() && r.Affected() == 0) r = db.Query("INSERT INTO _trijobrewards (jobtype, reward) VALUES ($1, $2)", {std::int64_t(type), add});
    return r.Ok();
}

namespace {
const char* JobTableWord(game::JobType type) {
    switch (type) {
    case game::JobType::Trader: return "trader";
    case game::JobType::Thief: return "robber";
    case game::JobType::Hunter: return "hunter";
    default: return nullptr;
    }
}
} // namespace

bool LoadBargainLock(Database& db, std::int64_t character, std::int64_t& until, std::string& error) {
    until = 0;
    const QueryResult r = db.Query("SELECT coalesce(max(until), 0) FROM sr_bargain_lock WHERE charid = $1", {character});
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    until = r.Int(0, 0);
    return true;
}

void SaveBargainLock(Database& db, std::int64_t character, std::int64_t until) {
    // Ours (the original keeps the bargain hours' lock-out in its own memory): sr_bargain_lock, never the original's _timedjob.
    const QueryResult r = db.Query("INSERT INTO sr_bargain_lock (charid, until) VALUES ($1, $2) ON CONFLICT (charid) DO UPDATE SET until = EXCLUDED.until",
                                   {character, until});
    if (!r.Ok()) core::LogWarning("the bargain goods' lock-out ({}): {}", character, r.Error());
}

bool LoadSafeTrade(Database& db, std::int64_t character, std::optional<SafeTradeRow>& out, std::string& error) {
    out.reset();
    if (!Exists(db, "_chartrijobsafetrade")) return true;
    const QueryResult r = db.Query("SELECT ablecount, status, to_char(lastsafetrade, 'YYYYMMDD')::integer FROM _chartrijobsafetrade WHERE charid = $1 LIMIT 1",
                                   {character});
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    if (r.Rows() == 0) return true;
    out = SafeTradeRow{int(r.Int(0, 0)), r.Int(0, 1) != 0, std::int32_t(r.Int(0, 2))};
    return true;
}

void SaveSafeTrade(Database& db, std::int64_t character, const SafeTradeRow& row) {
    db.Query("UPDATE _chartrijobsafetrade SET ablecount = $2, status = $3, lastsafetrade = to_timestamp($4::text, 'YYYYMMDD')::timestamp WHERE charid = $1",
             {character, std::int64_t(row.count), std::int64_t(row.active ? 1 : 0), std::to_string(row.day)});
}

bool RunJobWeek(Database& db, std::string& error) {
    return db.Transaction(
        [&](Database& tx) {
            const auto ok = [&](const QueryResult& r) {
                if (!r.Ok()) error = r.Error();
                return r.Ok();
            };
            // The thieves' and hunters' pools shared by contribution.
            for (const int job : {2, 3}) {
                const QueryResult totals =
                    tx.Query("SELECT coalesce(sum(contribution::bigint), 0), coalesce((SELECT min(reward) FROM _trijobrewards WHERE jobtype = $1), 0) "
                             "FROM _chartrijob WHERE jobtype = $1",
                             {std::int64_t(job)});
                if (!ok(totals)) return false;
                const std::int64_t total = totals.Int(0, 0), pool = totals.Int(0, 1);
                if (total == 0 && pool == 0) continue;
                if (total > 0 &&
                    !ok(tx.Query("UPDATE _chartrijob SET reward = least(2147483647, reward::bigint + ($2::bigint * contribution::bigint) / $3::bigint) "
                                 "WHERE jobtype = $1",
                                 {std::int64_t(job), pool, total}))) {
                    return false;
                }
                if (!ok(tx.Query("UPDATE _trijobrewards SET reward = 0 WHERE jobtype = $1", {std::int64_t(job)}))) return false;
            }
            for (const game::JobType type : {game::JobType::Trader, game::JobType::Thief, game::JobType::Hunter}) {
                const std::string word = JobTableWord(type);
                const std::int64_t job = std::int64_t(type);
                // Activity: the top 50 by level, then exp.
                if (!ok(tx.Query("DELETE FROM tab_refranking_" + word + "activity"))) return false;
                if (!ok(tx.Query("INSERT INTO tab_refranking_" + word +
                                     "activity SELECT row_number() OVER (ORDER BY t.level DESC, t.exp DESC), coalesce(c.nickname16, ''), t.level, t.exp, "
                                     "coalesce(o.country, 0) FROM _chartrijob t JOIN _char c ON c.charid = t.charid LEFT JOIN _refobjcommon o ON o.id = "
                                     "c.refobjid WHERE t.jobtype = $1 ORDER BY t.level DESC, t.exp DESC LIMIT 50",
                                 {job}))) {
                    return false;
                }
                // Contribution: only when any was made; then set to 0.
                const QueryResult any = tx.Query("SELECT 1 FROM _chartrijob WHERE jobtype = $1 AND contribution > 0 LIMIT 1", {job});
                if (!ok(any)) return false;
                if (any.Rows() == 0) continue;
                if (!ok(tx.Query("DELETE FROM tab_refranking_" + word + "contribution"))) return false;
                if (!ok(tx.Query("INSERT INTO tab_refranking_" + word +
                                     "contribution SELECT row_number() OVER (ORDER BY t.contribution DESC), coalesce(c.nickname16, ''), t.level, "
                                     "t.contribution FROM _chartrijob t JOIN _char c ON c.charid = t.charid WHERE t.jobtype = $1 "
                                     "ORDER BY t.contribution DESC LIMIT 50",
                                 {job}))) {
                    return false;
                }
                if (!ok(tx.Query("UPDATE _chartrijob SET contribution = 0 WHERE jobtype = $1", {job}))) return false;
            }
            return true;
        },
        error);
}

bool LoadJobRanking(Database& db, game::JobType type, int kind, std::vector<JobRankRow>& out, std::string& error) {
    out.clear();
    const char* word = JobTableWord(type);
    if (word == nullptr) return true;
    const std::string table = std::string("tab_refranking_") + word + (kind == 0 ? "activity" : "contribution");
    if (!Exists(db, table.c_str())) return true;
    const QueryResult r = kind == 0 ? db.Query("SELECT rank, nickname, joblevel, jobexp, country FROM " + table + " ORDER BY rank")
                                    : db.Query("SELECT rank, nickname, joblevel, contribution, 0 FROM " + table + " ORDER BY rank");
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    for (int k = 0; k < r.Rows(); ++k) {
        out.push_back({int(r.Int(k, 0)), r.Text(k, 1), int(r.Int(k, 2)), r.Int(k, 3), int(r.Int(k, 4))});
    }
    return true;
}

std::int64_t TakeJobReward(Database& db, std::int64_t character) {
    const QueryResult r = db.Query("UPDATE _chartrijob t SET reward = 0 FROM (SELECT charid, reward FROM _chartrijob WHERE charid = $1 FOR UPDATE) old "
                                   "WHERE t.charid = old.charid RETURNING old.reward",
                                   {character});
    return r.Ok() && r.Rows() > 0 ? r.Int(0, 0) : 0;
}

bool LoadBanditData(Database& db, game::BanditTable& table, std::unordered_map<std::uint16_t, RegionKind>& regions, std::string& error) {
    table = {};
    regions.clear();
    const bool typed = db.Query("SELECT 1 FROM information_schema.columns WHERE table_name = '_refobjcommon' AND column_name = 'typeid4'").Rows() > 0 &&
                       db.Query("SELECT 1 FROM information_schema.columns WHERE table_name = '_refobjchar' AND column_name = 'lvl'").Rows() > 0;
    if (typed && Exists(db, "_refobjcommon") && Exists(db, "_refobjchar")) {
        QueryResult r = db.Query("SELECT o.id, o.codename128, o.typeid4, c.lvl FROM _refobjcommon o JOIN _refobjchar c ON c.id = o.link "
                                 "WHERE o.service = 1 AND o.typeid1 = 1 AND o.typeid2 = 2 AND o.typeid3 = 1 AND o.typeid4 IN (2, 3)");
        if (!r.Ok()) {
            error = r.Error();
            return false;
        }
        for (int k = 0; k < r.Rows(); ++k) {
            const std::string code = r.Text(k, 1);
            const game::BanditKind kind = r.Int(k, 2) == 2 ? game::BanditKind::Thief : game::BanditKind::Hunter;
            const int zone = code.starts_with("MOB_EU_THIEF") || code.starts_with("MOB_EU_HUNTER") ? 1
                             : code.starts_with("MOB_THIEF") || code.starts_with("MOB_HUNTER")       ? 0
                                                                                                     : -1; // list 2: empty in the data
            table.Add(zone, kind, int(r.Int(k, 3)), std::uint32_t(r.Int(k, 0)));
        }
    }
    if (Exists(db, "_refregion")) {
        QueryResult r = db.Query("SELECT wregionid, isbattlefield, coalesce(continentname, '') FROM _refregion");
        if (!r.Ok()) {
            error = r.Error();
            return false;
        }
        for (int k = 0; k < r.Rows(); ++k) {
            RegionKind& kind = regions[std::uint16_t(std::int16_t(r.Int(k, 0)))];
            kind.town = r.Int(k, 1) == 0;
            kind.banditZone = std::int8_t(game::BanditZoneOf(r.Text(k, 2)));
        }
    }
    return true;
}

bool LoadJobRules(Database& db, game::JobRules& rules, std::string& error) {
    if (Exists(db, "_refshardcontentconfig")) {
        QueryResult r = db.Query("SELECT codename128, value FROM _refshardcontentconfig WHERE service = 1");
        if (!r.Ok()) {
            error = r.Error();
            return false;
        }
        for (int k = 0; k < r.Rows(); ++k) {
            const std::string name = r.Text(k, 0);
            const auto value = std::int64_t(std::stoll("0" + r.Text(k, 1)));
            for (int n = 1; n <= 5; ++n) {
                if (name == "TRADESCALE" + std::to_string(n) + "_AMOUNT_MAX") rules.scale[std::size_t(n - 1)] = std::uint32_t(value);
            }
            if (name == "TRADE_MON_FACTOR") rules.monsterFactor = std::uint32_t(std::max<std::int64_t>(1, value));
            if (name == "TRADE_SAFETY_OPTION") rules.safetyOption = int(value);
            if (name == "TRADE_SAFETY_NUM") rules.safetyCount = int(value);
        }
    }
    if (Exists(db, "_refdropgold")) {
        QueryResult r = db.Query("SELECT monlevel, goldmin FROM _refdropgold ORDER BY monlevel");
        if (!r.Ok()) {
            error = r.Error();
            return false;
        }
        rules.goldMin.clear();
        for (int k = 0; k < r.Rows(); ++k) {
            const auto level = std::size_t(std::max<std::int64_t>(0, r.Int(k, 0)));
            if (rules.goldMin.size() <= level) rules.goldMin.resize(level + 1, 0);
            rules.goldMin[level] = std::uint32_t(std::max<std::int64_t>(0, r.Int(k, 1)));
        }
    }
    if (Exists(db, "_reflevel")) {
        QueryResult r = db.Query("SELECT lvl, gust_mob_exp, jobexp_trader, jobexp_robber, jobexp_hunter FROM _reflevel ORDER BY lvl");
        if (!r.Ok()) {
            error = r.Error();
            return false;
        }
        rules.mobExp.clear();
        for (auto& t : rules.levelExp) t.clear();
        for (int k = 0; k < r.Rows(); ++k) {
            const auto level = std::size_t(std::max<std::int64_t>(0, r.Int(k, 0)));
            if (rules.mobExp.size() <= level) rules.mobExp.resize(level + 1, 0);
            rules.mobExp[level] = std::uint64_t(std::max<std::int64_t>(0, r.Int(k, 1)));
            if (level <= std::size_t(game::kJobMaxLevel)) {
                for (std::size_t j = 0; j < 3; ++j) {
                    if (rules.levelExp[j].size() <= level) rules.levelExp[j].resize(level + 1, 0);
                    rules.levelExp[j][level] = std::uint64_t(std::max<std::int64_t>(0, r.Int(k, 2 + int(j))));
                }
            }
        }
    }
    return true;
}

} // namespace sr::server
