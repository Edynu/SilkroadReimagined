#include "sr/server/GuildStore.h"

#include <chrono>
#include <format>
#include <unordered_map>

#include "sr/server/Database.h"

namespace sr::server {

namespace {

// _GuildMember.Permission is an int: the master's 0xFFFFFFFF is stored as -1.
std::int64_t Signed(std::uint32_t permission) { return std::int64_t(std::int32_t(permission)); }

std::int64_t UnixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

namespace {

// An int[] parameter's text ("{1,2,3}").
std::string IdArray(const std::vector<std::uint32_t>& ids) {
    std::string out = "{";
    for (std::uint32_t id : ids) out += (out.size() > 1 ? "," : "") + std::to_string(id);
    return out + "}";
}

// Every guild, union and emblem, or (`some`) only those of the guilds and unions named, and then no
// penalties.
bool LoadGuildsWhere(Database& db, GuildData& out, std::string& error, const std::vector<std::uint32_t>* someGuilds,
                     const std::vector<std::uint32_t>* someAlliances) {
    out = {};
    const bool some = someGuilds != nullptr;
    const std::vector<Param> ids = some ? std::vector<Param>{IdArray(*someGuilds), IdArray(*someAlliances)} : std::vector<Param>{};
    const std::string guildWhere = some ? " AND id = ANY($1::int[])" : "", memberWhere = some ? " AND m.guildid = ANY($1::int[])" : "",
                      allianceWhere = some ? " AND id = ANY($2::int[])" : "",
                      crestWhere = some ? " WHERE (kind = 0 AND id = ANY($1::int[])) OR (kind = 1 AND id = ANY($2::int[]))" : "";
    // Each query takes both arrays when filtered (the ones it does not use too: their types are given).
    const std::string both = some ? " AND $2::int[] IS NOT NULL" : "";
    const QueryResult guilds = db.Query("SELECT id, name, lvl, gatheredsp, coalesce(mastercommenttitle, ''), coalesce(mastercomment, ''), "
                                        "coalesce(alliance, 0), lastcrestrev, curcrestrev, coalesce(booty, 0), coalesce(mercenaryattr, 0) FROM _guild WHERE id <> 0" + guildWhere + both +
                                            " ORDER BY id",
                                        ids);
    const QueryResult members = db.Query(
        "SELECT m.guildid, m.charid, m.charname, m.memberclass, coalesce(c.curlevel, m.charlevel), m.gp_donation, coalesce(m.permission, 0), "
        "coalesce(m.nickname, ''), coalesce(nullif(m.refobjid, 0), c.refobjid, 0), "
        "coalesce(floor(extract(epoch from (localtimestamp - c.lastlogout)))::bigint, 0), coalesce(m.siegeauthority, 0), coalesce(m.contribution, 0) "
        "FROM _guildmember m LEFT JOIN _char c ON c.charid = m.charid WHERE m.guildid <> 0 AND m.charid <> 0" + memberWhere + both +
            " ORDER BY m.guildid, m.joindate, m.charid",
        ids);
    const QueryResult alliances = db.Query("SELECT id, coalesce(ally1, 0), coalesce(ally2, 0), coalesce(ally3, 0), coalesce(ally4, 0), "
                                           "coalesce(ally5, 0), coalesce(ally6, 0), coalesce(ally7, 0), coalesce(ally8, 0), lastcrestrev, curcrestrev "
                                           "FROM _alliedclans WHERE id <> 0" + allianceWhere + (some ? " AND $1::int[] IS NOT NULL" : "") + " ORDER BY id",
                                           ids);
    const QueryResult penalties = some ? db.Query("SELECT 1 WHERE false")
                                       : db.Query("SELECT charid, timetokeep, coalesce(data1, 0) FROM _timedjob WHERE category = 2 AND jobid = 1");
    const QueryResult crests = db.Query("SELECT kind, id, revision, pixels FROM sr_crests" + crestWhere, ids);
    for (const QueryResult* r : {&guilds, &members, &alliances, &penalties, &crests}) {
        if (!r->Ok()) {
            error = r->Error();
            return false;
        }
    }
    std::unordered_map<std::uint32_t, std::size_t> index;
    for (int i = 0; i < guilds.Rows(); ++i) {
        GuildRow g;
        g.id = std::uint32_t(guilds.Int(i, 0));
        g.name = guilds.Text(i, 1);
        g.level = int(guilds.Int(i, 2));
        g.points = std::uint32_t(std::max<std::int64_t>(0, guilds.Int(i, 3)));
        g.noticeTitle = guilds.Text(i, 4);
        g.notice = guilds.Text(i, 5);
        g.alliance = std::uint32_t(guilds.Int(i, 6));
        g.lastCrest = std::uint32_t(std::max<std::int64_t>(0, guilds.Int(i, 7)));
        g.crest = std::uint32_t(std::max<std::int64_t>(0, guilds.Int(i, 8)));
        g.booty = std::uint32_t(std::max<std::int64_t>(0, guilds.Int(i, 9)));
        g.mercenaryAttr = std::uint8_t(guilds.Int(i, 10) & 0xFF);
        index[g.id] = out.guilds.size();
        out.guilds.push_back(std::move(g));
    }
    for (int i = 0; i < members.Rows(); ++i) {
        const auto it = index.find(std::uint32_t(members.Int(i, 0)));
        if (it == index.end()) continue;
        GuildMemberRow m;
        m.character = members.Int(i, 1);
        m.name = members.Text(i, 2);
        m.memberClass = int(members.Int(i, 3));
        m.level = int(members.Int(i, 4));
        m.donation = std::uint32_t(std::max<std::int64_t>(0, members.Int(i, 5)));
        m.permission = std::uint32_t(members.Int(i, 6));
        m.nickname = members.Text(i, 7);
        m.model = std::uint32_t(members.Int(i, 8));
        m.awaySeconds = std::max<std::int64_t>(0, members.Int(i, 9));
        m.position = int(members.Int(i, 10));
        m.contribution = std::uint32_t(std::max<std::int64_t>(0, members.Int(i, 11)));
        out.guilds[it->second].members.push_back(std::move(m));
    }
    for (int i = 0; i < alliances.Rows(); ++i) {
        AllianceRow a;
        a.id = std::uint32_t(alliances.Int(i, 0));
        for (int k = 0; k < 8; ++k) a.guilds[std::size_t(k)] = std::uint32_t(alliances.Int(i, 1 + k));
        a.lastCrest = std::uint32_t(std::max<std::int64_t>(0, alliances.Int(i, 9)));
        a.crest = std::uint32_t(std::max<std::int64_t>(0, alliances.Int(i, 10)));
        out.alliances.push_back(a);
    }
    for (int i = 0; i < crests.Rows(); ++i) {
        CrestRow c;
        c.kind = std::uint8_t(crests.Int(i, 0));
        c.id = std::uint32_t(crests.Int(i, 1));
        c.revision = std::uint32_t(crests.Int(i, 2));
        c.pixels = crests.Bytes(i, 3);
        out.crests.push_back(std::move(c));
    }
    const std::int64_t now = UnixNow();
    for (int i = 0; i < penalties.Rows(); ++i) {
        const std::int64_t written = penalties.Int(i, 2);
        const std::int64_t left = penalties.Int(i, 1) - (written > 0 && written <= now ? now - written : 0);
        if (left > 0) out.penalties.push_back({penalties.Int(i, 0), left});
    }
    return some || LoadGuildWars(db, out.wars, error);
}

} // namespace

bool LoadGuildWars(Database& db, std::vector<GuildWarRow>& out, std::string& error) {
    out.clear();
    const QueryResult r = db.Query("SELECT id, guild1, guild2, victorypointindex, lodgedgold, data2, "
                                   "coalesce(floor(extract(epoch from (warendtime - localtimestamp)))::bigint, -1), data1, pointgain1, pointgain2 "
                                   "FROM _guildwar ORDER BY id");
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    for (int i = 0; i < r.Rows(); ++i) {
        GuildWarRow w;
        w.id = std::uint32_t(r.Int(i, 0));
        w.first = std::uint32_t(r.Int(i, 1));
        w.second = std::uint32_t(r.Int(i, 2));
        w.victory = std::uint8_t(r.Int(i, 3));
        w.lodged = std::uint32_t(std::max<std::int64_t>(0, r.Int(i, 4)));
        w.wager = std::uint32_t(std::max<std::int64_t>(0, r.Int(i, 5)));
        w.secondsLeft = r.IsNull(i, 6) ? -1 : r.Int(i, 6);
        if (w.secondsLeft < -1) w.secondsLeft = 0; // past its end: over at the next turn
        w.period = std::uint32_t(r.Int(i, 7));
        w.points1 = std::uint32_t(std::max<std::int64_t>(0, r.Int(i, 8)));
        w.points2 = std::uint32_t(std::max<std::int64_t>(0, r.Int(i, 9)));
        out.push_back(w);
    }
    return true;
}

std::uint32_t BeginGuildWar(Database& db, const GuildWarRow& w) {
    // As _GuildWar_Begin: one row a pair (refused when it stands), the end as a time (none: no end).
    const QueryResult r = db.Query(
        "INSERT INTO _guildwar (wartype, victorypointindex, lodgedgold, warendtime, guild1, guild2, pointgain1, pointgain2, data1, data2) "
        "SELECT 0, $1, $2, CASE WHEN $3::bigint < 0 THEN NULL ELSE localtimestamp + make_interval(secs => $3::bigint) END, $4, $5, 0, 0, $6, $7 "
        "WHERE NOT EXISTS (SELECT 1 FROM _guildwar WHERE (guild1 = $4 AND guild2 = $5) OR (guild1 = $5 AND guild2 = $4)) RETURNING id",
        {int(w.victory), std::int64_t(w.lodged), w.secondsLeft, std::int64_t(w.first), std::int64_t(w.second), std::int64_t(w.period), std::int64_t(w.wager)});
    return r.Ok() && r.Rows() == 1 ? std::uint32_t(r.Int(0, 0)) : 0u;
}

bool CountGuildWarKill(Database& db, std::uint32_t war, bool firstScores, std::uint32_t points, std::uint32_t killerGuild, std::int64_t killer,
                       std::uint32_t victimGuild, std::int64_t victim) {
    std::string error;
    const std::int64_t id = war;
    return db.Transaction(
        [&](Database& tx) {
            const char* side = firstScores ? "pointgain1" : "pointgain2";
            if (!tx.Query(std::format("UPDATE _guildwar SET {0} = {0} + $2 WHERE id = $1", side), {id, std::int64_t(points)}).Ok()) return false;
            if (!tx.Query("UPDATE _guildmember SET guildwarkill = coalesce(guildwarkill, 0) + 1, contribution = coalesce(contribution, 0) + $3 "
                          "WHERE guildid = $1 AND charid = $2",
                          {std::int64_t(killerGuild), killer, std::int64_t(points)})
                     .Ok()) {
                return false;
            }
            return tx.Query("UPDATE _guildmember SET guildwarkilled = coalesce(guildwarkilled, 0) + 1 WHERE guildid = $1 AND charid = $2",
                            {std::int64_t(victimGuild), victim})
                .Ok();
        },
        error);
}

bool EndGuildWar(Database& db, std::uint32_t war, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& rewards) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            for (const auto& [guild, booty] : rewards) {
                if (booty == 0) continue;
                if (!tx.Query("UPDATE _guild SET booty = coalesce(booty, 0) + $2 WHERE id = $1", {std::int64_t(guild), std::int64_t(booty)}).Ok()) return false;
            }
            return tx.Query("DELETE FROM _guildwar WHERE id = $1", {std::int64_t(war)}).Ok();
        },
        error);
}

std::uint32_t TakeGuildBooty(Database& db, std::uint32_t guild) {
    const QueryResult r = db.Query("UPDATE _guild g SET booty = 0 FROM (SELECT id, coalesce(booty, 0) AS was FROM _guild WHERE id = $1 FOR UPDATE) old "
                                   "WHERE g.id = old.id RETURNING old.was",
                                   {std::int64_t(guild)});
    return r.Ok() && r.Rows() == 1 ? std::uint32_t(std::max<std::int64_t>(0, r.Int(0, 0))) : 0u;
}

bool LoadGuilds(Database& db, GuildData& out, std::string& error) { return LoadGuildsWhere(db, out, error, nullptr, nullptr); }

bool LoadSomeGuilds(Database& db, const std::vector<std::uint32_t>& guilds, const std::vector<std::uint32_t>& alliances, GuildData& out,
                    std::string& error) {
    return LoadGuildsWhere(db, out, error, &guilds, &alliances);
}

std::string TakeGuildStorageLock(Database& db, std::uint32_t guild, std::uint16_t server, std::int64_t character, bool& taken) {
    // Taken when free or already this server's (its holder there changes); else the holder's name.
    taken = false;
    const QueryResult r = db.Query("INSERT INTO sr_guild_storage_lock (guildid, server, charid) VALUES ($1, $2, $3) "
                                   "ON CONFLICT (guildid) DO UPDATE SET charid = EXCLUDED.charid, since = localtimestamp "
                                   "WHERE sr_guild_storage_lock.server = EXCLUDED.server RETURNING 1",
                                   {std::int64_t(guild), int(server), character});
    if (!r.Ok()) return {};
    if (r.Rows() > 0) {
        taken = true;
        return {};
    }
    const QueryResult holder = db.Query("SELECT coalesce(c.charname16, '') FROM sr_guild_storage_lock l LEFT JOIN _char c ON c.charid = l.charid "
                                        "WHERE l.guildid = $1",
                                        {std::int64_t(guild)});
    return holder.Ok() && holder.Rows() > 0 ? holder.Text(0, 0) : std::string();
}

bool ReleaseGuildStorageLock(Database& db, std::uint32_t guild, std::uint16_t server) {
    return db.Query("DELETE FROM sr_guild_storage_lock WHERE guildid = $1 AND server = $2", {std::int64_t(guild), int(server)}).Ok();
}

bool ReleaseServerGuildStorageLocks(Database& db, std::uint16_t server) {
    return db.Query("DELETE FROM sr_guild_storage_lock WHERE server = $1", {int(server)}).Ok();
}

std::uint32_t CreateGuild(Database& db, const std::string& name, std::int64_t creator, bool& nameTaken, std::string& error) {
    nameTaken = false;
    std::uint32_t id = 0;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            // As _Guild_Create: refused when the name exists or the creator is in a guild already.
            const QueryResult taken = tx.Query("SELECT 1 FROM _guild WHERE lower(name) = lower($1)", {name});
            if (!taken.Ok()) return false;
            if (taken.Rows() > 0) {
                nameTaken = true;
                return false;
            }
            const QueryResult member = tx.Query("SELECT 1 FROM _guildmember WHERE charid = $1 AND guildid <> 0", {creator});
            if (!member.Ok() || member.Rows() > 0) return false;
            const QueryResult made = tx.Query("INSERT INTO _guild (name, lvl, gatheredsp, foundationdate, alliance, booty) "
                                              "VALUES ($1, 1, 0, localtimestamp(0), 0, 0) RETURNING id",
                                              {name});
            if (!made.Ok() || made.Rows() != 1) return false;
            id = std::uint32_t(made.Int(0, 0));
            if (!AddGuildMember(tx, id, creator, 0, 0xFFFFFFFFu)) return false;
            return tx.Query("INSERT INTO _guildchest (guildid, slot) SELECT $1, cnt FROM _refdummyslot", {std::int64_t(id)}).Ok();
        },
        error);
    return ok ? id : 0;
}

bool AddGuildMember(Database& db, std::uint32_t guild, std::int64_t character, int memberClass, std::uint32_t permission) {
    // As _Guild_FnAddMember: the name, level and model from _Char; the master gets the siege's
    // commander authority (1), the others 0.
    const QueryResult added = db.Query(
        "INSERT INTO _guildmember (guildid, charid, charname, memberclass, charlevel, gp_donation, joindate, permission, contribution, "
        "guildwarkill, guildwarkilled, nickname, refobjid, siegeauthority) "
        "SELECT $1, charid, charname16, $3, curlevel, 0, localtimestamp(0), $4, 0, 0, 0, NULL, refobjid, $5 FROM _char WHERE charid = $2",
        {std::int64_t(guild), character, memberClass, Signed(permission), memberClass == 0 ? 1 : 0});
    if (!added.Ok() || added.Affected() != 1) return false;
    return db.Query("UPDATE _char SET guildid = $2 WHERE charid = $1", {character, std::int64_t(guild)}).Ok();
}

bool RemoveGuildMember(Database& db, std::uint32_t guild, std::int64_t character) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            return tx.Query("UPDATE _char SET guildid = 0 WHERE charid = $1", {character}).Ok() &&
                   tx.Query("DELETE FROM _guildmember WHERE guildid = $1 AND charid = $2 AND memberclass <> 0", {std::int64_t(guild), character}).Ok();
        },
        error);
}

bool TakeFortressReturn(Database& db, std::int64_t character, std::uint32_t seconds) {
    bool taken = false;
    std::string error;
    db.Transaction(
        [&](Database& tx) {
            const QueryResult running = tx.Query("SELECT data1 + timetokeep FROM _timedjob WHERE charid = $1 AND category = 2 AND jobid = 5", {character});
            if (!running.Ok()) return false;
            if (running.Rows() > 0 && running.Int(0, 0) > UnixNow()) return true;
            taken = tx.Query("DELETE FROM _timedjob WHERE charid = $1 AND category = 2 AND jobid = 5", {character}).Ok() &&
                    tx.Query("INSERT INTO _timedjob (charid, category, jobid, timetokeep, data1, data2, serial64, jid) "
                             "VALUES ($1, 2, 5, $2, $3, 0, 0, coalesce((SELECT userjid FROM _user WHERE charid = $1), 0))",
                             {character, std::int64_t(seconds), UnixNow()})
                        .Ok();
            return taken;
        },
        error);
    return taken;
}

bool TakeTimedWait(Database& db, std::int64_t character, std::uint8_t category, std::uint32_t job, std::uint32_t seconds) {
    bool taken = false;
    std::string error;
    const std::int64_t kind = category, id = job;
    db.Transaction(
        [&](Database& tx) {
            // One at a time a character (its row locked through the transaction).
            if (!tx.Query("SELECT 1 FROM _char WHERE charid = $1 FOR UPDATE", {character}).Ok()) return false;
            const QueryResult running = tx.Query("SELECT data1 + timetokeep FROM _timedjob WHERE charid = $1 AND category = $2 AND jobid = $3", {character, kind, id});
            if (!running.Ok()) return false;
            if (running.Rows() > 0 && running.Int(0, 0) > UnixNow()) return true;
            taken = tx.Query("DELETE FROM _timedjob WHERE charid = $1 AND category = $2 AND jobid = $3", {character, kind, id}).Ok() &&
                    tx.Query("INSERT INTO _timedjob (charid, category, jobid, timetokeep, data1, data2, serial64, jid) "
                             "VALUES ($1, $2, $3, $4, $5, 0, 0, coalesce((SELECT userjid FROM _user WHERE charid = $1), 0))",
                             {character, kind, id, std::int64_t(seconds), UnixNow()})
                        .Ok();
            return taken;
        },
        error);
    return taken;
}

bool AddGuildPenalty(Database& db, std::int64_t character, std::uint32_t seconds) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            return tx.Query("DELETE FROM _timedjob WHERE charid = $1 AND category = 2 AND jobid = 1", {character}).Ok() &&
                   tx.Query("INSERT INTO _timedjob (charid, category, jobid, timetokeep, data1, data2, serial64, jid) "
                            "VALUES ($1, 2, 1, $2, $3, 0, 0, coalesce((SELECT userjid FROM _user WHERE charid = $1), 0))",
                            {character, std::int64_t(seconds), UnixNow()})
                       .Ok();
        },
        error);
}

bool DisbandGuild(Database& db, std::uint32_t guild) {
    std::string error;
    const std::int64_t id = guild;
    return db.Transaction(
        [&](Database& tx) {
            return tx.Query("UPDATE _char SET guildid = 0 WHERE charid IN (SELECT charid FROM _guildmember WHERE guildid = $1 AND charid <> 0)", {id})
                       .Ok() &&
                   tx.Query("DELETE FROM _guildmember WHERE guildid = $1", {id}).Ok() &&
                   tx.Query("DELETE FROM _guildchest WHERE guildid = $1", {id}).Ok() && tx.Query("DELETE FROM sr_crests WHERE kind = 0 AND id = $1", {id}).Ok() &&
                   tx.Query("DELETE FROM _guild WHERE id = $1", {id}).Ok();
        },
        error);
}

bool SaveGuildPosition(Database& db, std::uint32_t guild, std::int64_t character, int position) {
    return db.Query("UPDATE _guildmember SET siegeauthority = $3 WHERE guildid = $1 AND charid = $2", {std::int64_t(guild), character, position}).Ok();
}

bool AddGpHistory(Database& db, std::uint32_t guild, const std::string& name, std::uint32_t points, int reason) {
    // A row, then the guild's history kept to its newest 100 (the original's _AddGPHistory, then _DeleteGPHistory).
    if (!db.Query("INSERT INTO _gphistory (guildid, usedtime, charname, usedgp, reason) VALUES ($1, localtimestamp(0), $2, $3, $4)",
                  {std::int64_t(guild), name, std::int64_t(points), reason})
             .Ok()) {
        return false;
    }
    return db.Query("DELETE FROM _gphistory WHERE guildid = $1 AND id NOT IN (SELECT id FROM _gphistory WHERE guildid = $1 ORDER BY id DESC LIMIT 100)",
                    {std::int64_t(guild)})
        .Ok();
}

std::vector<GpHistoryRow> LoadGpHistory(Database& db, std::uint32_t guild) {
    std::vector<GpHistoryRow> out;
    const QueryResult r = db.Query("SELECT charname, usedgp, reason, extract(year from usedtime)::int, extract(month from usedtime)::int, "
                                   "extract(day from usedtime)::int, extract(hour from usedtime)::int, extract(minute from usedtime)::int "
                                   // The shard manager's (SM): the last 30 days, at most 100, newest first.
                                   "FROM _gphistory WHERE guildid = $1 AND usedtime >= localtimestamp - interval '30 days' ORDER BY id DESC LIMIT 100",
                                   {std::int64_t(guild)});
    if (!r.Ok()) return out;
    for (int i = 0; i < r.Rows(); ++i) {
        out.push_back({r.Text(i, 0), std::uint32_t(std::max<std::int64_t>(0, r.Int(i, 1))), int(r.Int(i, 2)), int(r.Int(i, 3)), int(r.Int(i, 4)), int(r.Int(i, 5)), int(r.Int(i, 6)), int(r.Int(i, 7))});
    }
    return out;
}

bool DelegateGuildMaster(Database& db, std::uint32_t guild, std::int64_t oldMaster, std::int64_t newMaster) {
    std::string error;
    const std::int64_t id = guild;
    return db.Transaction(
        [&](Database& tx) {
            const QueryResult a = tx.Query("UPDATE _guildmember SET memberclass = 10, permission = 0, siegeauthority = 0 "
                                           "WHERE guildid = $1 AND charid = $2 AND memberclass = 0",
                                           {id, oldMaster});
            const QueryResult b = tx.Query("UPDATE _guildmember SET memberclass = 0, permission = -1, siegeauthority = 1 "
                                           "WHERE guildid = $1 AND charid = $2",
                                           {id, newMaster});
            return a.Ok() && b.Ok() && a.Affected() == 1 && b.Affected() == 1;
        },
        error);
}

bool SaveGuildPermissions(Database& db, std::uint32_t guild, const std::vector<std::pair<std::int64_t, std::uint32_t>>& members) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            for (const auto& [character, permission] : members) {
                if (!tx.Query("UPDATE _guildmember SET permission = $3 WHERE guildid = $1 AND charid = $2", {std::int64_t(guild), character, Signed(permission)})
                         .Ok()) {
                    return false;
                }
            }
            return true;
        },
        error);
}

bool SaveGuildNotice(Database& db, std::uint32_t guild, const std::string& title, const std::string& body) {
    return db.Query("UPDATE _guild SET mastercommenttitle = $2, mastercomment = $3 WHERE id = $1", {std::int64_t(guild), title, body}).Ok();
}

bool SaveGuildNickname(Database& db, std::uint32_t guild, std::int64_t character, const std::string& nickname) {
    return db.Query("UPDATE _guildmember SET nickname = nullif($3, '') WHERE guildid = $1 AND charid = $2", {std::int64_t(guild), character, nickname}).Ok();
}

bool SaveGuildMemberLevel(Database& db, std::int64_t character, int level) {
    // As _Guild_RefreshMemberStatus's level target.
    return db.Query("UPDATE _guildmember SET charlevel = $2 WHERE charid = $1", {character, level}).Ok();
}

bool SaveGuildPoints(Database& db, std::uint32_t guild, int level, std::uint32_t points, std::int64_t character, std::uint32_t donation) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            if (!tx.Query("UPDATE _guild SET lvl = $2, gatheredsp = $3 WHERE id = $1", {std::int64_t(guild), level, std::int64_t(points)}).Ok()) return false;
            return character == 0 ||
                   tx.Query("UPDATE _guildmember SET gp_donation = $3 WHERE guildid = $1 AND charid = $2", {std::int64_t(guild), character, std::int64_t(donation)})
                       .Ok();
        },
        error);
}

std::uint32_t CreateAlliance(Database& db, std::uint32_t leader, std::uint32_t first) {
    // As _AllianceCreate: the leader in Ally1, the first ally in Ally2, both guilds' Alliance set.
    std::uint32_t id = 0;
    std::string error;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            const QueryResult made = tx.Query("INSERT INTO _alliedclans (ally1, ally2, ally3, ally4, ally5, ally6, ally7, ally8, foundationdate) "
                                              "VALUES ($1, $2, 0, 0, 0, 0, 0, 0, localtimestamp(0)) RETURNING id",
                                              {std::int64_t(leader), std::int64_t(first)});
            if (!made.Ok() || made.Rows() != 1) return false;
            id = std::uint32_t(made.Int(0, 0));
            const QueryResult set = tx.Query("UPDATE _guild SET alliance = $3 WHERE id IN ($1, $2)", {std::int64_t(leader), std::int64_t(first), std::int64_t(id)});
            return set.Ok() && set.Affected() == 2;
        },
        error);
    return ok ? id : 0;
}

bool SaveAlliance(Database& db, const AllianceRow& alliance) {
    std::string error;
    const std::int64_t id = alliance.id;
    return db.Transaction(
        [&](Database& tx) {
            std::vector<Param> slots{id};
            std::string list;
            for (std::size_t k = 0; k < 8; ++k) {
                slots.push_back(std::int64_t(alliance.guilds[k]));
                if (alliance.guilds[k] != 0) list += (list.empty() ? "" : ",") + std::to_string(alliance.guilds[k]);
            }
            if (!tx.Query("UPDATE _alliedclans SET ally1 = $2, ally2 = $3, ally3 = $4, ally4 = $5, ally5 = $6, ally6 = $7, ally7 = $8, ally8 = $9 "
                          "WHERE id = $1",
                          slots)
                     .Ok()) {
                return false;
            }
            const std::string out = list.empty() ? "UPDATE _guild SET alliance = 0 WHERE alliance = $1"
                                                 : std::format("UPDATE _guild SET alliance = 0 WHERE alliance = $1 AND id NOT IN ({})", list);
            if (!tx.Query(out, {id}).Ok()) return false;
            return list.empty() || tx.Query(std::format("UPDATE _guild SET alliance = $1 WHERE id IN ({})", list), {id}).Ok();
        },
        error);
}

bool RemoveAlliance(Database& db, std::uint32_t alliance) {
    // As _AllianceRemove: every guild out, then the union's row.
    std::string error;
    const std::int64_t id = alliance;
    return db.Transaction(
        [&](Database& tx) {
            return tx.Query("UPDATE _guild SET alliance = 0 WHERE alliance = $1", {id}).Ok() && tx.Query("DELETE FROM sr_crests WHERE kind = 1 AND id = $1", {id}).Ok() &&
                   tx.Query("DELETE FROM _alliedclans WHERE id = $1", {id}).Ok();
        },
        error);
}

bool SaveCrest(Database& db, std::uint8_t kind, std::uint32_t id, std::uint32_t last, std::uint32_t revision, const std::vector<std::uint8_t>& pixels) {
    std::string error;
    const std::int64_t key = id;
    const char* table = kind == 0 ? "_guild" : "_alliedclans";
    return db.Transaction(
        [&](Database& tx) {
            if (!tx.Query(std::format("UPDATE {} SET lastcrestrev = $2, curcrestrev = $3 WHERE id = $1", table), {key, std::int64_t(last), std::int64_t(revision)}).Ok()) {
                return false;
            }
            if (!tx.Query("DELETE FROM sr_crests WHERE kind = $1 AND id = $2", {int(kind), key}).Ok()) return false;
            return revision == 0 || pixels.empty() ||
                   tx.Query("INSERT INTO sr_crests (kind, id, revision, pixels) VALUES ($1, $2, $3, $4)",
                            {int(kind), key, std::int64_t(revision), Param::Bytes(pixels)})
                       .Ok();
        },
        error);
}

} // namespace sr::server
