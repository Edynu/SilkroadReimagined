#include "sr/server/SupportStore.h"

#include <algorithm>

#include "sr/server/Crypto.h"
#include "sr/server/Store.h"

namespace sr::server {

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

} // namespace

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

bool AddTicketFileData(Database& db, std::int64_t ticket, const std::string& original, const std::string& mime, std::span<const std::uint8_t> data) {
    const std::string sha = Hex(Sha256(data));
    const QueryResult r = db.Query("INSERT INTO sro_vt_account.sr_support_files (ticket_id, stored, original, mime, bytes, sha256, data) "
                                   "VALUES ($1, $2, $3, $4, $5, $6, $7)",
                                   {ticket, "db:" + sha + ":" + std::to_string(ticket), original, mime, std::int64_t(data.size()), sha,
                                    Param::Bytes(std::vector<std::uint8_t>(data.begin(), data.end()))});
    return r.Ok();
}

} // namespace sr::server
