#include "sr/server/EventStore.h"

namespace sr::server {

namespace {

constexpr const char* kColumns = "event, charid, charname, level, server, region, posx, posy, posz, registered, mode, kind, groupid";

EventEntryRow RowOf(const QueryResult& r, int i) {
    EventEntryRow e;
    e.event = EventKind(r.Int(i, 0));
    e.character = r.Int(i, 1);
    e.name = r.Text(i, 2);
    e.level = int(r.Int(i, 3));
    e.server = std::uint16_t(r.Int(i, 4));
    e.region = std::uint16_t(r.Int(i, 5));
    e.x = float(r.Double(i, 6));
    e.y = float(r.Double(i, 7));
    e.z = float(r.Double(i, 8));
    e.registered = r.Int(i, 9);
    e.mode = std::int32_t(r.Int(i, 10));
    e.kind = std::int32_t(r.Int(i, 11));
    e.group = r.Int(i, 12);
    return e;
}

bool Load(Database& db, EventKind event, std::vector<EventEntryRow>& out, std::string& error) {
    const QueryResult r = db.Query(std::string("SELECT ") + kColumns + " FROM sr_event_entries WHERE event = $1 ORDER BY registered", {int(event)});
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    for (int i = 0; i < r.Rows(); ++i) out.push_back(RowOf(r, i));
    return true;
}

} // namespace

bool SaveEventEntry(Database& db, const EventEntryRow& e, std::string& error) {
    const QueryResult r = db.Query(std::string("INSERT INTO sr_event_entries (") + kColumns +
                                       ") VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13) "
                                       "ON CONFLICT (event, charid) DO UPDATE SET charname = EXCLUDED.charname, level = EXCLUDED.level, "
                                       "server = EXCLUDED.server, region = EXCLUDED.region, posx = EXCLUDED.posx, posy = EXCLUDED.posy, "
                                       "posz = EXCLUDED.posz, registered = EXCLUDED.registered, mode = EXCLUDED.mode, kind = EXCLUDED.kind, "
                                       "groupid = EXCLUDED.groupid",
                                   {int(e.event), e.character, e.name, e.level, int(e.server), int(std::int16_t(e.region)), double(e.x), double(e.y),
                                    double(e.z), e.registered, e.mode, e.kind, e.group});
    if (!r.Ok()) error = r.Error();
    return r.Ok();
}

bool DeleteEventEntry(Database& db, EventKind event, std::int64_t character, std::string& error) {
    const QueryResult r = db.Query("DELETE FROM sr_event_entries WHERE event = $1 AND charid = $2", {int(event), character});
    if (!r.Ok()) error = r.Error();
    return r.Ok();
}

bool LoadEventEntries(Database& db, EventKind event, std::vector<EventEntryRow>& out, std::string& error) { return Load(db, event, out, error); }

bool TakeEventEntries(Database& db, EventKind event, std::vector<EventEntryRow>& out, std::string& error) {
    return db.Transaction(
        [&](Database& tx) {
            std::vector<EventEntryRow> rows;
            if (!Load(tx, event, rows, error)) return false;
            const QueryResult d = tx.Query("DELETE FROM sr_event_entries WHERE event = $1", {int(event)});
            if (!d.Ok()) {
                error = d.Error();
                return false;
            }
            out = std::move(rows);
            return true;
        },
        error);
}

bool AddFlagEventParticipant(Database& db, std::int64_t jid, std::string& error) {
    return db.Transaction(
        [&](Database& tx) {
            const QueryResult u = tx.Query("UPDATE _flagworld_eventparticipants SET count = count + 1, latestattempt = now() WHERE jid = $1", {jid});
            if (!u.Ok()) {
                error = u.Error();
                return false;
            }
            if (u.Affected() > 0) return true;
            const QueryResult i = tx.Query("INSERT INTO _flagworld_eventparticipants (jid, latestattempt, count) VALUES ($1, now(), 1)", {jid});
            if (!i.Ok()) error = i.Error();
            return i.Ok();
        },
        error);
}

int FlagEventParticipantCount(Database& db, std::int64_t jid) {
    const QueryResult r = db.Query("SELECT coalesce(sum(count), 0) FROM _flagworld_eventparticipants WHERE jid = $1", {jid});
    return r.Ok() && r.Rows() == 1 ? int(r.Int(0, 0)) : 0;
}

} // namespace sr::server
