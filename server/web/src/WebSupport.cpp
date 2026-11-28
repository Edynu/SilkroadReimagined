#include "WebSupport.h"

#include <algorithm>
#include <format>

#include "sr/server/Crypto.h"
#include "sr/server/Store.h"

namespace sr::server::web {

using json = nlohmann::json;

namespace {

json MessagesOf(Database& db, std::int64_t ticket, bool withNotes) {
    json a = json::array();
    const QueryResult r = db.Query(std::format("SELECT id, author, name, body, floor(extract(epoch FROM created_at))::bigint FROM sro_vt_account.sr_support_messages "
                                               "WHERE ticket_id = $1{} ORDER BY created_at, id",
                                               withNotes ? "" : " AND author <> 'note'"),
                                   {ticket});
    for (int i = 0; r.Ok() && i < r.Rows(); ++i)
        a.push_back({{"id", r.Int(i, 0)}, {"author", r.Text(i, 1)}, {"name", r.Text(i, 2)}, {"text", r.Text(i, 3)}, {"at", r.Int(i, 4)}});
    return a;
}

json FilesOf(Database& db, std::int64_t ticket) {
    json a = json::array();
    const QueryResult r = db.Query("SELECT id, original, mime, bytes FROM sro_vt_account.sr_support_files WHERE ticket_id = $1 ORDER BY id", {ticket});
    for (int i = 0; r.Ok() && i < r.Rows(); ++i)
        a.push_back({{"id", r.Int(i, 0)}, {"name", r.Text(i, 1)}, {"mime", r.Text(i, 2)}, {"bytes", r.Int(i, 3)}});
    return a;
}

constexpr const char* kTicketColumns =
    "t.id, t.kind, t.category, t.status, t.subject, t.body, t.name, t.email, coalesce(u.struserid, ''), t.source, t.client_info, "
    "floor(extract(epoch FROM t.created_at))::bigint, floor(extract(epoch FROM t.updated_at))::bigint, t.address, "
    "(SELECT count(*) FROM sro_vt_account.sr_support_files f WHERE f.ticket_id = t.id), "
    "(SELECT count(*) FROM sro_vt_account.sr_support_messages m WHERE m.ticket_id = t.id AND m.author <> 'note') ";

json TicketRow(const QueryResult& r, int i, bool staff) {
    json t = {{"id", r.Int(i, 0)},       {"kind", r.Text(i, 1)},   {"category", r.Text(i, 2)}, {"status", r.Text(i, 3)},
              {"subject", r.Text(i, 4)}, {"text", r.Text(i, 5)},   {"source", r.Text(i, 9)},   {"created", r.Int(i, 11)},
              {"updated", r.Int(i, 12)}, {"files", r.Int(i, 14)}, {"messages", r.Int(i, 15)}};
    if (staff) {
        t["name"] = r.Text(i, 6);
        t["email"] = r.Text(i, 7);
        t["account"] = r.Text(i, 8);
        t["clientInfo"] = r.Text(i, 10);
        t["address"] = r.Text(i, 13);
    }
    return t;
}

} // namespace

bool SupportUploadPath(const std::string& method, const std::string& path) {
    if (method != "POST" || !path.starts_with("/api/support/") || !path.ends_with("/files")) return false;
    const std::string_view id = std::string_view(path).substr(13, path.size() - 13 - 6);
    return !id.empty() && id.size() <= 18 && std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
}

json PlayerTickets(Database& db, std::int64_t jid) {
    json a = json::array();
    const QueryResult r = db.Query(std::string("SELECT ") + kTicketColumns +
                                       "FROM sro_vt_account.sr_support_tickets t LEFT JOIN sro_vt_account.tb_user u ON u.jid = t.jid "
                                       "WHERE t.jid = $1 AND t.kind <> 'help' ORDER BY t.updated_at DESC LIMIT 50", // help requests: the admin's alone
                                   {jid});
    for (int i = 0; r.Ok() && i < r.Rows(); ++i) {
        json t = TicketRow(r, i, false);
        t["conversation"] = MessagesOf(db, r.Int(i, 0), false);
        json names = json::array();
        for (const json& f : FilesOf(db, r.Int(i, 0))) names.push_back(f["name"]);
        t["fileNames"] = names;
        a.push_back(std::move(t));
    }
    return a;
}

bool PlayerReply(Database& db, std::int64_t jid, std::int64_t ticket, const std::string& name, const std::string& text) {
    const QueryResult r = db.Query("UPDATE sro_vt_account.sr_support_tickets SET status = 'open', updated_at = now() WHERE id = $1 AND jid = $2 AND status <> 'closed'",
                                   {ticket, jid});
    if (!r.Ok() || r.Affected() != 1) return false;
    return db.Query("INSERT INTO sro_vt_account.sr_support_messages (ticket_id, author, name, body) VALUES ($1, 'player', $2, $3)", {ticket, name, text}).Ok();
}

json TicketList(Database& db, const std::string& status, const std::string& kind, const std::string& search) {
    std::string where = "true";
    std::vector<Param> params;
    if (status == "open" || status == "answered" || status == "closed") {
        params.emplace_back(status);
        where += std::format(" AND t.status = ${}", params.size());
    }
    if (kind == "contact" || kind == "bug" || kind == "help") {
        params.emplace_back(kind);
        where += std::format(" AND t.kind = ${}", params.size());
    }
    if (!search.empty()) {
        params.emplace_back("%" + search + "%");
        where += std::format(" AND (t.subject ILIKE ${0} OR t.body ILIKE ${0} OR t.email ILIKE ${0} OR u.struserid ILIKE ${0} OR t.id::text = ${1})", params.size(),
                             params.size() + 1);
        params.emplace_back(search);
    }
    json a = json::array();
    const QueryResult r = db.Query(std::string("SELECT ") + kTicketColumns +
                                       "FROM sro_vt_account.sr_support_tickets t LEFT JOIN sro_vt_account.tb_user u ON u.jid = t.jid WHERE " + where +
                                       " ORDER BY (t.status = 'open') DESC, t.updated_at DESC LIMIT 200",
                                   params);
    for (int i = 0; r.Ok() && i < r.Rows(); ++i) {
        json t = TicketRow(r, i, true);
        t.erase("text");
        a.push_back(std::move(t));
    }
    return {{"tickets", a}, {"counts", SupportCounts(db)}};
}

json TicketDetail(Database& db, std::int64_t id) {
    const QueryResult r = db.Query(std::string("SELECT ") + kTicketColumns +
                                       "FROM sro_vt_account.sr_support_tickets t LEFT JOIN sro_vt_account.tb_user u ON u.jid = t.jid WHERE t.id = $1",
                                   {id});
    if (!r.Ok() || r.Rows() != 1) return nullptr;
    json t = TicketRow(r, 0, true);
    t["conversation"] = MessagesOf(db, id, true);
    t["attachments"] = FilesOf(db, id);
    return t;
}

bool StaffMessage(Database& db, std::int64_t id, const std::string& name, const std::string& text, bool note) {
    const QueryResult r = db.Query(note ? "UPDATE sro_vt_account.sr_support_tickets SET updated_at = now() WHERE id = $1"
                                        : "UPDATE sro_vt_account.sr_support_tickets SET status = 'answered', updated_at = now() WHERE id = $1",
                                   {id});
    if (!r.Ok() || r.Affected() != 1) return false;
    return db.Query("INSERT INTO sro_vt_account.sr_support_messages (ticket_id, author, name, body) VALUES ($1, $2, $3, $4)",
                    {id, note ? "note" : "staff", name, text})
        .Ok();
}

bool SetTicketStatus(Database& db, std::int64_t id, const std::string& status) {
    if (status != "open" && status != "answered" && status != "closed") return false;
    const QueryResult r = db.Query("UPDATE sro_vt_account.sr_support_tickets SET status = $2, updated_at = now() WHERE id = $1", {id, status});
    return r.Ok() && r.Affected() == 1;
}

std::optional<std::vector<std::string>> DeleteTicket(Database& db, std::int64_t id) {
    std::vector<std::string> stored;
    const QueryResult f = db.Query("SELECT stored FROM sro_vt_account.sr_support_files WHERE ticket_id = $1", {id});
    for (int i = 0; f.Ok() && i < f.Rows(); ++i) stored.push_back(f.Text(i, 0));
    const QueryResult r = db.Query("DELETE FROM sro_vt_account.sr_support_tickets WHERE id = $1", {id});
    if (!r.Ok() || r.Affected() != 1) return std::nullopt;
    return stored;
}

json SupportCounts(Database& db) {
    json counts = {{"contact", {{"open", 0}, {"answered", 0}, {"closed", 0}}},
                   {"bug", {{"open", 0}, {"answered", 0}, {"closed", 0}}},
                   {"help", {{"open", 0}, {"answered", 0}, {"closed", 0}}}};
    const QueryResult r = db.Query("SELECT kind, status, count(*) FROM sro_vt_account.sr_support_tickets GROUP BY kind, status");
    for (int i = 0; r.Ok() && i < r.Rows(); ++i)
        if (counts.contains(r.Text(i, 0))) counts[r.Text(i, 0)][r.Text(i, 1)] = r.Int(i, 2);
    return counts;
}

} // namespace sr::server::web
