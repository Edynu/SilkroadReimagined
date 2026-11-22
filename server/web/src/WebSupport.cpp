#include "WebSupport.h"

#include <algorithm>
#include <format>

#include "sr/server/Crypto.h"
#include "sr/server/Store.h"

namespace sr::server::web {

using json = nlohmann::json;

namespace {

std::string Trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    s.erase(0, first);
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

// Characters a person types: no control characters but the line breaks and tabs of a message.
bool PlainText(std::string_view s, bool lines) {
    return std::none_of(s.begin(), s.end(), [lines](char c) {
        const auto u = static_cast<unsigned char>(c);
        return (u < 32 && !(lines && (c == '\n' || c == '\r' || c == '\t'))) || u == 127;
    });
}

// UTF-8 characters, not bytes.
std::size_t Length(std::string_view s) {
    return std::size_t(std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

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

const std::vector<std::string>& SupportCategories(const std::string& kind) {
    static const std::vector<std::string> contact = {"account", "report", "suggestion", "other"};
    static const std::vector<std::string> bug = {"gameplay", "graphics", "crash", "connection", "website", "other"};
    static const std::vector<std::string> none;
    return kind == "contact" ? contact : kind == "bug" ? bug : none;
}

std::optional<std::string> CheckTicket(NewTicket& t) {
    t.subject = Trim(t.subject);
    t.body = Trim(t.body);
    t.name = Trim(t.name);
    t.email = Trim(t.email);
    t.clientInfo = Trim(t.clientInfo);
    if (t.kind != "contact" && t.kind != "bug") return "Choose a message or a bug report.";
    const auto& categories = SupportCategories(t.kind);
    if (std::find(categories.begin(), categories.end(), t.category) == categories.end()) return "Choose what it is about.";
    if (t.source != "site" && t.source != "client") return "An unknown source.";
    if (Length(t.subject) < 3 || Length(t.subject) > 120 || !PlainText(t.subject, false)) return "The subject is 3 to 120 characters.";
    if (Length(t.body) < 10 || Length(t.body) > 5000 || !PlainText(t.body, true)) return "The message is 10 to 5000 characters.";
    if (Length(t.name) > 40 || !PlainText(t.name, false)) return "The name is up to 40 characters.";
    if (!t.email.empty() && !ValidEmail(t.email)) return "That e-mail address does not look right.";
    if (t.jid == 0 && t.email.empty()) return "Give an e-mail address, so we can answer (or log in: the answer then shows on your account page).";
    if (t.clientInfo.size() > 2000 || !PlainText(t.clientInfo, true)) return "The client's details are too long.";
    return std::nullopt;
}

std::optional<std::pair<std::int64_t, std::string>> CreateTicket(Database& db, const NewTicket& t) {
    const std::vector<std::uint8_t> token = RandomBytes(32);
    if (token.size() != 32) return std::nullopt;
    const QueryResult r = db.Query("INSERT INTO sro_vt_account.sr_support_tickets (kind, category, subject, body, name, email, source, client_info, jid, address, upload_token) "
                                   "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, nullif($9, 0), $10, $11) RETURNING id",
                                   {t.kind, t.category, t.subject, t.body, t.name, t.email, t.source, t.clientInfo, t.jid, t.address, Param::Bytes(Sha256(token))});
    if (!r.Ok() || r.Rows() != 1) return std::nullopt;
    return std::pair{r.Int(0, 0), Hex(token)};
}

std::string SupportFileType(std::string_view head) {
    const auto starts = [&](std::string_view magic, std::size_t at = 0) { return head.size() >= at + magic.size() && head.substr(at, magic.size()) == magic; };
    if (starts("\xFF\xD8\xFF")) return "image/jpeg";
    if (starts("\x89PNG\r\n\x1A\n")) return "image/png";
    if (starts("RIFF") && starts("WEBP", 8)) return "image/webp";
    if (starts("ftyp", 4)) return "video/mp4";
    if (starts("\x1A\x45\xDF\xA3")) return "video/webm";
    return {};
}

std::string SupportFileExtension(const std::string& mime) {
    if (mime == "image/jpeg") return ".jpg";
    if (mime == "image/png") return ".png";
    if (mime == "image/webp") return ".webp";
    if (mime == "video/mp4") return ".mp4";
    if (mime == "video/webm") return ".webm";
    return ".bin";
}

json PlayerTickets(Database& db, std::int64_t jid) {
    json a = json::array();
    const QueryResult r = db.Query(std::string("SELECT ") + kTicketColumns +
                                       "FROM sro_vt_account.sr_support_tickets t LEFT JOIN sro_vt_account.tb_user u ON u.jid = t.jid "
                                       "WHERE t.jid = $1 ORDER BY t.updated_at DESC LIMIT 50",
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
    if (kind == "contact" || kind == "bug") {
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
    json counts = {{"contact", {{"open", 0}, {"answered", 0}, {"closed", 0}}}, {"bug", {{"open", 0}, {"answered", 0}, {"closed", 0}}}};
    const QueryResult r = db.Query("SELECT kind, status, count(*) FROM sro_vt_account.sr_support_tickets GROUP BY kind, status");
    for (int i = 0; r.Ok() && i < r.Rows(); ++i)
        if (counts.contains(r.Text(i, 0))) counts[r.Text(i, 0)][r.Text(i, 1)] = r.Int(i, 2);
    return counts;
}

} // namespace sr::server::web
