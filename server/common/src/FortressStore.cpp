#include "sr/server/FortressStore.h"

#include <algorithm>
#include <map>

#include "sr/server/Database.h"

namespace sr::server {

namespace {

bool Fail(const QueryResult& r, std::string& error) {
    if (r.Ok()) return false;
    error = r.Error();
    return true;
}

bool TableExists(Database& db, const char* name) {
    const QueryResult r = db.Query("SELECT to_regclass($1) IS NOT NULL", {name});
    return r.Ok() && r.Rows() == 1 && r.Bool(0, 0);
}

} // namespace

bool LoadStructureReferences(Database& db, std::vector<StructureReference>& out, std::string& error) {
    out.clear();
    const QueryResult rows = db.Query(
        "SELECT o.id, o.codename128, coalesce(o.reqlevel1, 0), coalesce(c.except_1, 0), coalesce(c.except_2, 0), coalesce(c.except_3, 0), "
        "coalesce(c.maxhp, 0), coalesce(o.costrepair, 0), coalesce(o.canrevive, 0), coalesce(o.costrevive, 0) "
        "FROM _refobjcommon o JOIN _refobjchar c ON c.id = o.link WHERE o.codename128 LIKE 'STRUCTURE\\_%' ORDER BY o.id");
    if (!rows.Ok()) {
        error = rows.Error();
        return false;
    }
    std::unordered_map<std::string, std::size_t> byName;
    for (int i = 0; i < rows.Rows(); ++i) {
        StructureReference r;
        r.refObjId = std::uint32_t(rows.Int(i, 0));
        r.codename = rows.Text(i, 1);
        while (!r.codename.empty() && r.codename.back() == ' ') r.codename.pop_back();
        r.reqLevel = int(rows.Int(i, 2));
        r.gold = std::uint32_t(std::max<std::int64_t>(0, rows.Int(i, 3)));
        r.guildPoints = std::uint32_t(std::max<std::int64_t>(0, rows.Int(i, 4)));
        r.minutes = std::uint32_t(std::max<std::int64_t>(0, rows.Int(i, 5)));
        r.maxHp = std::uint32_t(std::max<std::int64_t>(0, rows.Int(i, 6)));
        r.costRepair = std::uint32_t(std::max<std::int64_t>(0, rows.Int(i, 7)));
        r.canRevive = rows.Int(i, 8) != 0;
        r.costRevive = std::uint32_t(std::max<std::int64_t>(0, rows.Int(i, 9)));
        byName[r.codename] = out.size();
        out.push_back(std::move(r));
    }
    // Every row for the levels down (`_SiegeFortressResetTempStruct_NoTx` reads them all); the upgrades offered only
    // from the rows in service.
    const QueryResult upgrades = db.Query("SELECT structname, basestructcodename, upgradestructname1, upgradestructname2, upgradestructname3, "
                                          "upgradestructname4, service FROM _refsiegestructupgrade");
    if (!upgrades.Ok()) return true; // no upgrade table: nothing upgrades
    const auto text = [&](int row, int column) {
        std::string t = upgrades.Text(row, column);
        while (!t.empty() && t.back() == ' ') t.pop_back();
        return t;
    };
    for (int i = 0; i < upgrades.Rows(); ++i) {
        const std::string name = text(i, 0);
        const auto it = byName.find(name);
        if (it == byName.end()) continue;
        if (const std::string base = text(i, 1); byName.contains(base)) out[it->second].base = base;
        if (const auto first = byName.find(text(i, 2)); first != byName.end()) {
            std::string& below = out[first->second].below;
            if (below.empty() || name < below) below = name;
        }
        if (upgrades.Int(i, 6) == 0) continue;
        for (int k = 0; k < 4; ++k) {
            const std::string next = text(i, 2 + k);
            if (next != "xxx" && byName.contains(next)) out[it->second].upgrades[std::size_t(k)] = next;
        }
    }
    return true;
}

bool LoadBattleRanks(Database& db, std::vector<BattleRankRow>& out, std::string& error) {
    out.clear();
    const QueryResult rows = db.Query("SELECT ranklvl, reqpkcount, bindedskillid FROM _refsiegefortressbattlerank WHERE service = 1 ORDER BY ranklvl");
    if (!rows.Ok()) {
        error = rows.Error();
        return false;
    }
    for (int i = 0; i < rows.Rows(); ++i) {
        out.push_back({int(rows.Int(i, 0)), std::uint32_t(std::max<std::int64_t>(0, rows.Int(i, 1))), std::uint32_t(std::max<std::int64_t>(0, rows.Int(i, 2)))});
    }
    return true;
}

bool SaveBattleRecord(Database& db, std::uint32_t fortress, std::int64_t character, int kills, int killed, int rank) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            return tx.Query("DELETE FROM _siegefortressbattlerecord WHERE fortressid = $1 AND charid = $2", {std::int64_t(fortress), character}).Ok() &&
                   tx.Query("INSERT INTO _siegefortressbattlerecord (fortressid, charid, killcount, killedcount, rankupdate, currank) "
                            "VALUES ($1, $2, $3, $4, localtimestamp, $5)",
                            {std::int64_t(fortress), character, kills, killed, rank})
                       .Ok();
        },
        error);
}

bool LoadFortressState(Database& db, FortressState& out, std::string& error) {
    out = {};
    const QueryResult fortresses = db.Query("SELECT fortressid, guildid, tempguildid, taxratio, tax, coalesce(npchired, 0), coalesce(introduction, ''), "
                                            "coalesce(extract(epoch from createddungeontime)::bigint, 0), coalesce(createddungeoncount, 0), "
                                            "coalesce(introductionmodificationpermission, 1) "
                                            "FROM _siegefortress ORDER BY fortressid");
    const QueryResult requests = db.Query("SELECT fortressid, guildid, requesttype FROM _siegefortressrequest ORDER BY fortressid, guildid");
    const QueryResult structures =
        db.Query("SELECT fortressid, ownerguildid, refeventstructid, refobjid, hp, state, coalesce(extract(epoch from makedate)::bigint, 0) "
                 "FROM _siegefortressstruct ORDER BY fortressid, refeventstructid");
    const QueryResult stones = db.Query("SELECT fortressid, guildid, accumulatedamage FROM _siegefortressstonestate ORDER BY fortressid, guildid");
    for (const QueryResult* r : {&fortresses, &requests, &structures, &stones}) {
        if (Fail(*r, error)) return false;
    }
    for (int i = 0; i < fortresses.Rows(); ++i) {
        out.fortresses.push_back({std::uint32_t(fortresses.Int(i, 0)), std::uint32_t(fortresses.Int(i, 1)), std::uint32_t(fortresses.Int(i, 2)),
                                  int(fortresses.Int(i, 3)), fortresses.Int(i, 4), std::uint8_t(fortresses.Int(i, 5)), fortresses.Text(i, 6),
                                  fortresses.Int(i, 7), int(fortresses.Int(i, 8)), fortresses.Int(i, 9) != 0});
    }
    for (int i = 0; i < requests.Rows(); ++i) {
        out.requests.push_back({std::uint32_t(requests.Int(i, 0)), std::uint32_t(requests.Int(i, 1)), std::uint8_t(requests.Int(i, 2))});
    }
    for (int i = 0; i < structures.Rows(); ++i) {
        out.structures.push_back({std::uint32_t(structures.Int(i, 0)), std::uint32_t(structures.Int(i, 1)), std::uint32_t(structures.Int(i, 2)),
                                  std::uint32_t(structures.Int(i, 3)), structures.Int(i, 4), int(structures.Int(i, 5)), structures.Int(i, 6)});
    }
    for (int i = 0; i < stones.Rows(); ++i) {
        out.stones.push_back({std::uint32_t(stones.Int(i, 0)), std::uint32_t(stones.Int(i, 1)), stones.Int(i, 2)});
    }
    if (TableExists(db, "_siegefortressobject")) {
        const QueryResult objects =
            db.Query("SELECT id, fortressid, ownerguildid, refobjid, hp, region, posx, posy, posz, direction, ownerlevel FROM _siegefortressobject ORDER BY id");
        if (Fail(objects, error)) return false;
        for (int i = 0; i < objects.Rows(); ++i) {
            FortressObjectRow o;
            o.id = objects.Int(i, 0);
            o.fortress = std::uint32_t(objects.Int(i, 1));
            o.owner = std::uint32_t(objects.Int(i, 2));
            o.refObjId = std::uint32_t(objects.Int(i, 3));
            o.hp = objects.Int(i, 4);
            o.region = std::uint16_t(std::int16_t(objects.Int(i, 5)));
            o.x = float(objects.Double(i, 6));
            o.y = float(objects.Double(i, 7));
            o.z = float(objects.Double(i, 8));
            o.direction = float(objects.Double(i, 9));
            o.ownerLevel = int(objects.Int(i, 10));
            out.objects.push_back(o);
        }
    }
    if (TableExists(db, "_siegefortressitemforge")) {
        const QueryResult forges = db.Query("SELECT fortressid, itemrefid, amount, finished, extract(epoch from startdate)::bigint, extract(epoch from finishdate)::bigint "
                                            "FROM _siegefortressitemforge ORDER BY fortressid, itemrefid");
        if (Fail(forges, error)) return false;
        for (int i = 0; i < forges.Rows(); ++i) {
            out.forges.push_back({std::uint32_t(forges.Int(i, 0)), std::uint32_t(forges.Int(i, 1)), int(forges.Int(i, 2)), forges.Int(i, 3) != 0, forges.Int(i, 4),
                                  forges.Int(i, 5)});
        }
    }
    const QueryResult records = db.Query("SELECT fortressid, charid, killcount, killedcount, currank FROM _siegefortressbattlerecord ORDER BY fortressid, charid");
    if (Fail(records, error)) return false;
    for (int i = 0; i < records.Rows(); ++i) {
        out.records.push_back({std::uint32_t(records.Int(i, 0)), records.Int(i, 1), int(records.Int(i, 2)), int(records.Int(i, 3)), int(records.Int(i, 4))});
    }
    return true;
}

bool SaveFortressHired(Database& db, std::uint32_t fortress, std::uint8_t hired) {
    return db.Query("UPDATE _siegefortress SET npchired = $2 WHERE fortressid = $1", {std::int64_t(fortress), int(hired)}).Ok();
}

bool SaveFortressIntroduction(Database& db, std::uint32_t fortress, const std::string& text) {
    return db.Query("UPDATE _siegefortress SET introduction = $2 WHERE fortressid = $1", {std::int64_t(fortress), text}).Ok();
}

bool SaveFortressDungeon(Database& db, std::uint32_t fortress, int count, std::int64_t createdAt) {
    return db.Query("UPDATE _siegefortress SET createddungeoncount = $2, createddungeontime = CASE WHEN $3::bigint = 0 THEN createddungeontime "
                    "ELSE to_timestamp($3::bigint) AT TIME ZONE 'UTC' END WHERE fortressid = $1",
                    {std::int64_t(fortress), count, createdAt})
        .Ok();
}

bool LoadBlessBuffs(Database& db, std::vector<BlessRow>& out, std::string& error) {
    out.clear();
    if (!TableExists(db, "_refsiegeblessbuff")) return true;
    const QueryResult r = db.Query("SELECT blessid, fortressid, refblessbuffid, needgold, needgp FROM _refsiegeblessbuff WHERE service <> 0 ORDER BY blessid");
    if (Fail(r, error)) return false;
    for (int i = 0; i < r.Rows(); ++i) {
        out.push_back({std::uint32_t(r.Int(i, 0)), std::uint32_t(r.Int(i, 1)), std::uint32_t(r.Int(i, 2)), std::uint64_t(std::max<std::int64_t>(0, r.Int(i, 3))),
                       std::uint32_t(std::max<std::int64_t>(0, r.Int(i, 4)))});
    }
    return true;
}

bool InsertFortressForge(Database& db, const FortressForgeRow& r) {
    return db.Query("INSERT INTO _siegefortressitemforge (fortressid, itemrefid, amount, finished, startdate, finishdate) "
                    "VALUES ($1, $2, $3, $4, to_timestamp($5::bigint) AT TIME ZONE 'UTC', to_timestamp($6::bigint) AT TIME ZONE 'UTC')",
                    {std::int64_t(r.fortress), std::int64_t(r.item), r.amount, r.finished ? 1 : 0, r.start, r.finish})
        .Ok();
}

bool SaveFortressForge(Database& db, const FortressForgeRow& r) {
    return db.Query("UPDATE _siegefortressitemforge SET amount = $3, finished = $4 WHERE fortressid = $1 AND itemrefid = $2",
                    {std::int64_t(r.fortress), std::int64_t(r.item), r.amount, r.finished ? 1 : 0})
        .Ok();
}

bool DeleteFortressForge(Database& db, std::uint32_t fortress, std::uint32_t item) {
    return db.Query("DELETE FROM _siegefortressitemforge WHERE fortressid = $1 AND itemrefid = $2", {std::int64_t(fortress), std::int64_t(item)}).Ok();
}

bool LoadForgeReference(Database& db, std::vector<ForgeReferenceRow>& out, std::string& error) {
    out.clear();
    if (!TableExists(db, "_refsiegefortressitemforge")) return true;
    const QueryResult r = db.Query("SELECT f.fortressid, f.refitemid, f.reqgold, f.reqgp, f.forgetimemin, "
                                   "(c.typeid1 = 3 AND c.typeid2 = 3 AND c.typeid3 = 3 AND c.typeid4 = 2) "
                                   "FROM _refsiegefortressitemforge f JOIN _refobjcommon c ON c.id = f.refitemid WHERE f.service = 1 ORDER BY f.fortressid, f.refitemid");
    if (Fail(r, error)) return false;
    for (int i = 0; i < r.Rows(); ++i) {
        out.push_back({std::uint32_t(r.Int(i, 0)), std::uint32_t(r.Int(i, 1)), std::uint64_t(std::max<std::int64_t>(0, r.Int(i, 2))),
                       std::uint32_t(std::max<std::int64_t>(0, r.Int(i, 3))), int(r.Int(i, 4)), r.Bool(i, 5)});
    }
    return true;
}

bool LoadSiegeDungeons(Database& db, std::vector<SiegeDungeonRow>& out, std::string& error) {
    out.clear();
    if (!TableExists(db, "_refsiegedungeon")) return true;
    const QueryResult r = db.Query("SELECT fortressid, worldid, maxcreatecount, entrygold, entrygp FROM _refsiegedungeon WHERE service = 1 ORDER BY fortressid");
    if (Fail(r, error)) return false;
    for (int i = 0; i < r.Rows(); ++i) {
        out.push_back({std::uint32_t(r.Int(i, 0)), std::uint32_t(r.Int(i, 1)), int(r.Int(i, 2)), std::uint64_t(std::max<std::int64_t>(0, r.Int(i, 3))),
                       std::uint32_t(std::max<std::int64_t>(0, r.Int(i, 4)))});
    }
    return true;
}

bool LoadSiegeQuests(Database& db, std::vector<SiegeQuestRow>& quests, std::vector<SiegeRewardRow>& rewards, std::string& error) {
    quests.clear();
    rewards.clear();
    if (TableExists(db, "_refsiegequest")) {
        const QueryResult q = db.Query("SELECT questid, questtype, rewardconditiontargetcount, isaccumulation FROM _refsiegequest WHERE service = 1 ORDER BY questid");
        if (Fail(q, error)) return false;
        for (int i = 0; i < q.Rows(); ++i) quests.push_back({int(q.Int(i, 0)), int(q.Int(i, 1)), int(q.Int(i, 2)), q.Int(i, 3) != 0, {}});
        if (TableExists(db, "_refsiegequestreward")) {
            const QueryResult r = db.Query("SELECT questid, rewardtype, rewardrefid, rewardvalue FROM _refsiegequestreward WHERE service = 1 ORDER BY questid, rewardtype");
            if (Fail(r, error)) return false;
            for (int i = 0; i < r.Rows(); ++i) {
                for (SiegeQuestRow& quest : quests) {
                    if (quest.id == int(r.Int(i, 0))) {
                        quest.rewards.push_back({int(r.Int(i, 1)), std::uint32_t(r.Int(i, 2)), std::uint32_t(std::max<std::int64_t>(0, r.Int(i, 3)))});
                    }
                }
            }
        }
    }
    if (TableExists(db, "_refsiegefortressrewards")) {
        const QueryResult r = db.Query("SELECT fortressid, rewardtypeid, rewardvalue, rewardcount FROM _refsiegefortressrewards WHERE service = 1 ORDER BY fortressid, rewardtypeid");
        if (Fail(r, error)) return false;
        for (int i = 0; i < r.Rows(); ++i) {
            rewards.push_back({std::uint32_t(r.Int(i, 0)), int(r.Int(i, 1)), std::uint32_t(std::max<std::int64_t>(0, r.Int(i, 2))),
                               std::uint32_t(std::max<std::int64_t>(0, r.Int(i, 3)))});
        }
    }
    return true;
}

std::int64_t AddFortressObject(Database& db, const FortressObjectRow& o) {

    const QueryResult r = db.Query("INSERT INTO _siegefortressobject (fortressid, ownerguildid, refobjid, hp, region, posx, posy, posz, direction, ownerlevel) "
                                   "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10) RETURNING id",
                                   {std::int64_t(o.fortress), std::int64_t(o.owner), std::int64_t(o.refObjId), o.hp, int(std::int16_t(o.region)), double(o.x),
                                    double(o.y), double(o.z), double(o.direction), o.ownerLevel});
    return r.Ok() && r.Rows() == 1 ? r.Int(0, 0) : 0;
}

bool DeleteFortressObject(Database& db, std::int64_t id) { return db.Query("DELETE FROM _siegefortressobject WHERE id = $1", {id}).Ok(); }

bool InsertFortress(Database& db, std::uint32_t fortress, const std::vector<FortressStructRow>& structures, std::string& error) {
    return db.Transaction(
        [&](Database& tx) {
            const QueryResult f = tx.Query("INSERT INTO _siegefortress (fortressid) SELECT $1 WHERE NOT EXISTS "
                                           "(SELECT 1 FROM _siegefortress WHERE fortressid = $1)",
                                           {std::int64_t(fortress)});
            if (!f.Ok()) return false;
            for (const FortressStructRow& s : structures) {
                const QueryResult r = tx.Query("INSERT INTO _siegefortressstruct (fortressid, ownerguildid, refeventstructid, refobjid, hp, makedate, state) "
                                               "SELECT $1, $2, $3, $4, $5, localtimestamp, $6 WHERE NOT EXISTS "
                                               "(SELECT 1 FROM _siegefortressstruct WHERE refeventstructid = $3)",
                                               {std::int64_t(s.fortress), std::int64_t(s.owner), std::int64_t(s.eventId), std::int64_t(s.refObjId), s.hp, s.state});
                if (!r.Ok()) return false;
            }
            return true;
        },
        error);
}

bool InsertFortressRequest(Database& db, const FortressRequestRow& request) {
    return db.Query("INSERT INTO _siegefortressrequest (fortressid, guildid, requesttype) VALUES ($1, $2, $3)",
                    {std::int64_t(request.fortress), std::int64_t(request.guild), int(request.type)})
        .Ok();
}

bool DeleteFortressRequest(Database& db, std::uint32_t fortress, std::uint32_t guild) {
    return db.Query("DELETE FROM _siegefortressrequest WHERE fortressid = $1 AND guildid = $2", {std::int64_t(fortress), std::int64_t(guild)}).Ok();
}

bool SaveFortressStruct(Database& db, const FortressStructRow& row) {
    return db
        .Query("UPDATE _siegefortressstruct SET ownerguildid = $2, refobjid = $3, hp = $4, state = $5, "
               "makedate = CASE WHEN refobjid IS DISTINCT FROM $3 THEN localtimestamp ELSE makedate END "
               "WHERE refeventstructid = $1",
               {std::int64_t(row.eventId), std::int64_t(row.owner), std::int64_t(row.refObjId), row.hp, row.state})
        .Ok();
}

bool SaveFortressStone(Database& db, const FortressStoneRow& row) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            if (!tx.Query("DELETE FROM _siegefortressstonestate WHERE fortressid = $1 AND guildid = $2", {std::int64_t(row.fortress), std::int64_t(row.guild)})
                     .Ok()) {
                return false;
            }
            return tx.Query("INSERT INTO _siegefortressstonestate (fortressid, guildid, accumulatedamage) VALUES ($1, $2, $3)",
                            {std::int64_t(row.fortress), std::int64_t(row.guild), std::min<std::int64_t>(row.damage, 0x7FFFFFFF)})
                .Ok();
        },
        error);
}

bool SetFortressHolder(Database& db, std::uint32_t fortress, std::uint32_t holder) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            return tx.Query("UPDATE _siegefortress SET tempguildid = $2, npchired = 0 WHERE fortressid = $1", {std::int64_t(fortress), std::int64_t(holder)})
                       .Ok() &&
                   tx.Query("DELETE FROM _siegefortressstonestate WHERE fortressid = $1", {std::int64_t(fortress)}).Ok() &&
                   // "the objects added in the war are removed" (`_SiegeFortressTempFinished`)
                   (!TableExists(tx, "_siegefortressobject") || tx.Query("DELETE FROM _siegefortressobject WHERE fortressid = $1", {std::int64_t(fortress)}).Ok());
        },
        error);
}

bool FinishFortress(Database& db, std::uint32_t fortress, std::uint32_t owner, std::uint32_t holder, std::string& error) {
    return db.Transaction(
        [&](Database& tx) {
            const std::int64_t f = fortress;
            for (const char* sql : {"DELETE FROM _siegefortressbattlerecord WHERE fortressid = $1", "DELETE FROM _siegefortressrequest WHERE fortressid = $1",
                                    "DELETE FROM _siegefortressstonestate WHERE fortressid = $1"}) {
                if (!tx.Query(sql, {f}).Ok()) return false;
            }
            const bool objects = TableExists(tx, "_siegefortressobject") && TableExists(tx, "_refobjcommon");
            if (holder == 0 || holder == owner) {
                // Kept: the objects added in the war (flags TID4 2, summoned monsters 3) go, the guards stay.
                return tx.Query("UPDATE _siegefortress SET tempguildid = 0 WHERE fortressid = $1", {f}).Ok() &&
                       (!objects || tx.Query("DELETE FROM _siegefortressobject WHERE fortressid = $1 AND refobjid IN (SELECT id FROM _refobjcommon WHERE "
                                             "typeid1 = 1 AND typeid2 = 2 AND typeid3 = 4 AND typeid4 IN (2, 3))",
                                             {f})
                                         .Ok());
            }
            // Taken: every object of the fortress goes (guards, flags).
            if (objects && !tx.Query("DELETE FROM _siegefortressobject WHERE fortressid = $1", {f}).Ok()) return false;
            if (!tx.Query("UPDATE _siegefortress SET guildid = $2, npchired = 0, tempguildid = 0 WHERE fortressid = $1", {f, std::int64_t(holder)}).Ok()) {
                return false;
            }
            return owner == 0 ||
                   tx.Query("UPDATE _guildmember SET siegeauthority = 0 WHERE siegeauthority >= 4 AND guildid = $1", {std::int64_t(owner)}).Ok();
        },
        error);
}

bool AddFortressTax(Database& db, std::uint32_t fortress, std::int64_t delta) {
    return db.Query("UPDATE _siegefortress SET tax = greatest(0, tax + $2) WHERE fortressid = $1", {std::int64_t(fortress), delta}).Ok();
}

bool LoadTaxedObjects(Database& db, std::vector<std::pair<std::uint32_t, std::uint32_t>>& out, std::string& error) {
    QueryResult r = db.Query("SELECT o.id, f.fortressid FROM _refobjcommon o JOIN _refsiegefortress f ON f.codename128 = o.assocfile1_128 "
                             "WHERE o.service = 1 AND f.service = 1 ORDER BY o.id");
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    for (int k = 0; k < r.Rows(); ++k) out.push_back({std::uint32_t(r.Int(k, 0)), std::uint32_t(r.Int(k, 1))});
    return true;
}

bool SaveFortressTaxRatio(Database& db, std::uint32_t fortress, int ratio) {
    return db.Query("UPDATE _siegefortress SET taxratio = $2 WHERE fortressid = $1", {std::int64_t(fortress), ratio}).Ok();
}

bool LoadWorldRegions(Database& db, std::vector<std::pair<std::uint16_t, std::string>>& out, std::string& error) {
    out.clear();
    if (!TableExists(db, "_refgame_world") || !TableExists(db, "_refinstance_world_region")) return true;
    const QueryResult q = db.Query("SELECT r.regionid, w.worldcodename128 FROM _refinstance_world_region r JOIN _refgame_world w ON w.id = r.worldid");
    if (Fail(q, error)) return false;
    for (int i = 0; i < q.Rows(); ++i) out.emplace_back(std::uint16_t(std::int16_t(q.Int(i, 0))), q.Text(i, 1));
    return true;
}

bool LoadFortressItems(Database& db, std::vector<FortressItemRow>& out, std::string& error) {
    out.clear();
    if (!TableExists(db, "_refobjitem")) return true;
    const QueryResult r = db.Query(
        "SELECT o.id, o.codename128, o.typeid1, o.typeid2, o.typeid3, o.typeid4, coalesce(i.desc1_128, ''), i.param2, i.param3, i.param4, i.param5, i.range, "
        "coalesce(i.desc4_128, '') "
        "FROM _refobjcommon o JOIN _refobjitem i ON i.id = o.link WHERE o.service = 1 AND o.typeid1 = 3 AND o.typeid2 = 3 AND "
        "((o.typeid3 = 3 AND o.typeid4 IN (6, 7, 8, 9, 11)) OR (o.typeid3 = 1 AND o.typeid4 = 10) OR (o.typeid3 = 6 AND o.typeid4 = 2)) ORDER BY o.id");
    if (Fail(r, error)) return false;
    for (int k = 0; k < r.Rows(); ++k) {
        FortressItemRow row;
        row.refObjId = std::uint32_t(r.Int(k, 0));
        row.codename = r.Text(k, 1);
        for (int t = 0; t < 4; ++t) row.typeId[std::size_t(t)] = int(r.Int(k, 2 + t));
        row.target = r.Text(k, 6);
        row.place = std::max(0, int(r.Int(k, 7)));
        row.time = std::max(0, int(r.Int(k, 8)));
        row.reuseMs = std::max(0, int(r.Int(k, 9)));
        row.mask = std::max(0, int(r.Int(k, 10)));
        row.range = std::max(0, int(r.Int(k, 11)));
        if (const std::string cool = r.Text(k, 12); cool.starts_with("COOLTIME:")) {
            row.coolGroup = std::uint32_t(std::strtoul(cool.c_str() + 9, nullptr, 16));
        }
        out.push_back(std::move(row));
    }
    return true;
}

namespace {
bool LoadVariants(Database& db, const char* table, const char* sql, std::vector<SiegeVariantRow>& out, std::string& error) {
    out.clear();
    if (!TableExists(db, table) || !TableExists(db, "_refobjchar")) return true;
    const QueryResult r = db.Query(sql);
    if (Fail(r, error)) return false;
    for (int i = 0; i < r.Rows(); ++i) {
        std::string group = r.Text(i, 2);
        while (!group.empty() && group.back() == ' ') group.pop_back();
        out.push_back({std::uint32_t(r.Int(i, 0)), std::uint32_t(r.Int(i, 1)), std::move(group), int(r.Int(i, 3))});
    }
    return true;
}
} // namespace

bool LoadFortressGuards(Database& db, std::vector<SiegeVariantRow>& out, std::string& error) {
    return LoadVariants(db, "_refsiegefortressguard",
                        "SELECT g.fortressid, g.guardrefobjid, c.orgobjcodename128, h.lvl FROM _refsiegefortressguard g JOIN _refobjcommon c ON c.id = g.guardrefobjid "
                        "JOIN _refobjchar h ON h.id = c.link WHERE g.service = 1 ORDER BY g.fortressid, g.guardrefobjid",
                        out, error);
}

bool LoadSiegeSummons(Database& db, std::vector<SiegeVariantRow>& out, std::string& error) {
    return LoadVariants(db, "_refsiegelvlsummonmonster",
                        "SELECT 0, s.refobjid, o.codename128, h.lvl FROM _refsiegelvlsummonmonster s JOIN _refobjcommon c ON c.id = s.refobjid "
                        "JOIN _refobjcommon o ON o.id = s.reforgobjid JOIN _refobjchar h ON h.id = c.link WHERE s.service = 1 ORDER BY s.refobjid",
                        out, error);
}

bool LoadFortressReference(Database& db, std::vector<FortressReferenceRow>& out, std::string& error) {

    out.clear();
    for (const char* table : {"_refsiegefortress", "_refgame_world", "_refgame_world_config", "_refeventzone", "_refregion"}) {
        if (!TableExists(db, table)) return true; // no import: no fortresses
    }
    const QueryResult forts = db.Query(
        "SELECT f.fortressid, f.codename128, f.nameid128, f.requestnpcname128, f.requestfee, "
        "  (SELECT min(w.worldcodename128) FROM _refgame_world w WHERE w.configgroupcodename128 = c.groupcodename128), "
        "  (SELECT min(a.value) FROM _refgame_world_config a WHERE a.groupcodename128 = c.groupcodename128 "
        "     AND a.valuecodename128 = 'REF_REGION_BIND_ASSOC_SERVER'), "
        "  coalesce(f.linkedteleportcodename, ''), coalesce(f.taxtargets, 0), coalesce(f.maxadmission, 0), coalesce(f.maxguard, 0), "
        "  coalesce(f.maxbarricade, 0) "
        "FROM _refsiegefortress f LEFT JOIN _refgame_world_config c ON c.valuecodename128 = 'REF_SIEGE_FORTRESS' AND c.value = f.codename128 "
        "WHERE f.service = 1 ORDER BY f.fortressid");
    if (Fail(forts, error)) return false;
    for (int i = 0; i < forts.Rows(); ++i) {
        FortressReferenceRow f;
        f.id = std::uint32_t(forts.Int(i, 0));
        f.codename = forts.Text(i, 1);
        f.nameKey = forts.Text(i, 2);
        f.official = forts.Text(i, 3);
        f.fee = std::uint32_t(std::max<std::int64_t>(0, forts.Int(i, 4)));
        f.world = forts.Text(i, 5);
        f.linkedTeleport = forts.Text(i, 7);
        f.taxTargets = std::uint16_t(forts.Int(i, 8));
        f.maxAdmission = int(forts.Int(i, 9));
        f.maxGuard = int(forts.Int(i, 10));
        f.maxBarricade = int(forts.Int(i, 11));
        const std::string area = forts.Text(i, 6);
        const QueryResult regions = db.Query("SELECT wregionid FROM _refregion WHERE continentname = $1 ORDER BY wregionid", {area});
        const QueryResult zones = db.Query("SELECT id, zonename, eventname, coalesce(strparam1, '') FROM _refeventzone WHERE service = 1 AND strparam2 = $1 "
                                           "ORDER BY id",
                                           {f.world});
        if (Fail(regions, error) || Fail(zones, error)) return false;
        for (int r = 0; r < regions.Rows(); ++r) f.regions.push_back(std::uint16_t(regions.Int(r, 0)));
        for (int z = 0; z < zones.Rows(); ++z) {
            f.structures.push_back({std::uint32_t(zones.Int(z, 0)), zones.Text(z, 1), zones.Text(z, 2), zones.Text(z, 3)});
        }
        out.push_back(std::move(f));
    }
    return true;
}

} // namespace sr::server
