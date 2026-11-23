#include "sr/server/InstanceStore.h"

namespace sr::server {

bool LoadItemWaits(Database& db, std::int64_t character, std::vector<ItemWait>& out, std::string& error) {
    out.clear();
    QueryResult r = db.Query("SELECT data1, data2 FROM _timedjob WHERE charid = $1 AND category = 6 AND jobid = 201", {character});
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    for (int i = 0; i < r.Rows(); ++i) out.push_back({std::uint32_t(r.Int(i, 0)), r.Int(i, 1)});
    return true;
}

bool SaveItemWait(Database& db, std::int64_t character, const ItemWait& wait, std::string& error) {
    return db.Transaction(
        [&](Database& tx) {
            return tx.Query("DELETE FROM _timedjob WHERE charid = $1 AND category = 6 AND jobid = 201 AND data1 = $2", {character, std::int64_t(wait.group)}).Ok() &&
                   tx.Query("INSERT INTO _timedjob (charid, category, jobid, timetokeep, data1, data2, serial64, jid) "
                            "VALUES ($1, 6, 201, 1, $2, $3, 0, coalesce((SELECT userjid FROM _user WHERE charid = $1), 0))",
                            {character, std::int64_t(wait.group), wait.started})
                       .Ok();
        },
        error);
}

bool LoadCollectionBook(Database& db, std::int64_t character, std::vector<BookEntry>& out, std::string& error) {
    out.clear();
    QueryResult r = db.Query("SELECT themeid, slotindex FROM _charcollectionbook WHERE charid = $1 ORDER BY themeid, slotindex", {character});
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    for (int i = 0; i < r.Rows(); ++i) out.push_back({std::uint32_t(r.Int(i, 0)), std::uint8_t(r.Int(i, 1))});
    return true;
}

bool AddCollectionBookEntry(Database& db, std::int64_t character, const BookEntry& entry, std::string& error) {
    QueryResult r = db.Query("INSERT INTO _charcollectionbook (charid, themeid, slotindex, regdate) VALUES ($1, $2, $3, localtimestamp) "
                             "ON CONFLICT DO NOTHING",
                             {character, std::int64_t(entry.theme), std::int64_t(entry.slot)});
    if (!r.Ok()) error = r.Error();
    return r.Ok();
}

bool LoadInstanceEntries(Database& db, std::int64_t character, std::vector<InstanceEntry>& out, std::string& error) {
    out.clear();
    QueryResult r = db.Query("SELECT dungeonkeyid, worldid, layerid, extract(epoch FROM openedtime AT TIME ZONE 'UTC')::bigint, regionid, posx, posy, posz, "
                             "isactivated, entercount, extract(epoch FROM lastentertime AT TIME ZONE 'UTC')::bigint "
                             "FROM _charinstanceworlddata WHERE charid = $1 ORDER BY lastentertime DESC",
                             {character});
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    for (int i = 0; i < r.Rows(); ++i) {
        InstanceEntry e;
        e.key = std::uint32_t(r.Int(i, 0));
        e.world = std::uint32_t(r.Int(i, 1));
        e.layer = std::uint32_t(r.Int(i, 2));
        e.opened = r.Int(i, 3);
        e.region = std::uint16_t(r.Int(i, 4));
        e.x = std::int32_t(r.Int(i, 5));
        e.y = std::int32_t(r.Int(i, 6));
        e.z = std::int32_t(r.Int(i, 7));
        e.active = r.Int(i, 8) != 0;
        e.count = std::uint32_t(r.Int(i, 9));
        e.last = r.Int(i, 10);
        out.push_back(e);
    }
    return true;
}

bool SaveInstanceEntry(Database& db, std::int64_t character, const InstanceEntry& e, std::string& error) {
    // One transaction, as the original's _CharInstanceWorldDataUpdate: an active entry makes the character's others
    // inactive, then this world's row is written (at most one active entry a character).
    return db.Transaction(
        [&](Database& tx) {
            if (e.active && !tx.Query("UPDATE _charinstanceworlddata SET isactivated = 0 WHERE charid = $1 AND worldid <> $2 AND isactivated <> 0",
                                      {character, std::int64_t(e.world)})
                                 .Ok()) {
                return false;
            }
            QueryResult r = tx.Query(
                "INSERT INTO _charinstanceworlddata (charid, dungeonkeyid, worldid, layerid, openedtime, regionid, posx, posy, posz, isactivated, entercount, "
                "lastentertime) VALUES ($1, $2, $3, $4, to_timestamp($5) AT TIME ZONE 'UTC', $6, $7, $8, $9, $10, $11, to_timestamp($12) AT TIME ZONE 'UTC') "
                "ON CONFLICT (charid, worldid) DO UPDATE SET dungeonkeyid = EXCLUDED.dungeonkeyid, layerid = EXCLUDED.layerid, openedtime = EXCLUDED.openedtime, "
                "regionid = EXCLUDED.regionid, posx = EXCLUDED.posx, posy = EXCLUDED.posy, posz = EXCLUDED.posz, isactivated = EXCLUDED.isactivated, "
                "entercount = EXCLUDED.entercount, lastentertime = EXCLUDED.lastentertime",
                {character, std::int64_t(e.key), std::int64_t(e.world), std::int64_t(e.layer), e.opened, std::int64_t(std::int16_t(e.region)), std::int64_t(e.x),
                 std::int64_t(e.y), std::int64_t(e.z), std::int64_t(e.active ? 1 : 0), std::int64_t(e.count), e.last});
            if (!r.Ok()) error = r.Error();
            return r.Ok();
        },
        error);
}

} // namespace sr::server
