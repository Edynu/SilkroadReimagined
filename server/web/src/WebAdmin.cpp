#include "sr/server/WebAdmin.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <mutex>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "sr/core/Log.h"
#include "sr/server/Crypto.h"
#include "sr/server/Service.h"
#include "sr/server/WebServer.h"
#include "WebCommon.h"
#include "WebDev.h"

namespace sr::server {

namespace {

using namespace web;
using json = nlohmann::json;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr const char* kCookie = "sro_admin";
constexpr int kSessionHours = 12, kIdleMinutes = 60;
constexpr int kWrongInARow = 5, kLockMinutes = 15;
constexpr std::size_t kAdminPasswordMin = 12;
constexpr std::size_t kSiteMax = 512 * 1024;
constexpr std::size_t kBodyMax = 64 * 1024; // any request but an upload

// A link the site may show: its own path, or http(s).
bool SafeLink(std::string_view s) {
    return s.empty() || (s.starts_with("/") && !s.starts_with("//")) || s.starts_with("https://") || s.starts_with("http://");
}
bool LinkKey(std::string key) {
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* k : {"url", "href", "link", "image", "src", "discord", "github", "docs"})
        if (key.find(k) != std::string::npos) return true;
    return false;
}
// The site's texts: an object of objects, arrays and strings (numbers and booleans too), links safe, strings short.
bool ValidContent(const json& j, const std::string& key, std::string& why, int depth = 0) {
    if (depth > 8) return (why = "nested too deep"), false;
    if (j.is_string()) {
        const std::string& s = j.get_ref<const std::string&>();
        if (s.size() > 20000) return (why = key + " is too long"), false;
        if (LinkKey(key) && !SafeLink(s)) return (why = key + ": a link must start with https://, http:// or /"), false;
        return true;
    }
    if (j.is_number() || j.is_boolean() || j.is_null()) return true;
    if (j.is_array()) {
        if (j.size() > 200) return (why = key + " has too many entries"), false;
        for (const json& v : j)
            if (!ValidContent(v, key, why, depth + 1)) return false;
        return true;
    }
    if (j.is_object()) {
        for (const auto& [k, v] : j.items())
            if (k.size() > 64 || !ValidContent(v, k, why, depth + 1)) return why.empty() ? (why = "a key is too long", false) : false;
        return true;
    }
    return (why = "unknown value"), false;
}

// IPv4 ranges and exact addresses (anything else compared as text).
bool Allowed(const std::vector<std::string>& allow, const std::string& address) {
    if (allow.empty()) return true;
    const auto v4 = [](std::string_view s, std::uint32_t& out) {
        unsigned a, b, c, d;
        char tail = 0;
        if (std::sscanf(std::string(s).c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
        out = (a << 24) | (b << 16) | (c << 8) | d;
        return true;
    };
    std::string addr = address.starts_with("::ffff:") ? address.substr(7) : address;
    std::uint32_t ip = 0;
    const bool isV4 = v4(addr, ip);
    for (const std::string& rule : allow) {
        const std::size_t slash = rule.find('/');
        if (slash == std::string::npos) {
            if (rule == addr || (rule == "localhost" && (addr == "127.0.0.1" || addr == "::1"))) return true;
            continue;
        }
        std::uint32_t net = 0;
        const int bits = std::atoi(rule.c_str() + slash + 1);
        if (!isV4 || !v4(std::string_view(rule).substr(0, slash), net) || bits < 0 || bits > 32) continue;
        const std::uint32_t mask = bits == 0 ? 0 : 0xFFFFFFFFu << (32 - bits);
        if ((ip & mask) == (net & mask)) return true;
    }
    return false;
}

} // namespace

// --- The command line's -----------------------------------------------------------------------------

bool ValidAdminName(std::string_view name) {
    return name.size() >= 3 && name.size() <= 32 && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

bool MakeAdmin(Database& db, std::string_view name, std::string_view role, std::string_view password, std::string& secretBase32, std::string& error) {
    if (!ValidAdminName(name)) return (error = "admin names are 3 to 32 letters, digits or _"), false;
    if (role != "admin" && role != "moderator" && role != "tester") return (error = "the role is admin, moderator or tester"), false;
    if (password.size() < kAdminPasswordMin || password.size() > 128) return (error = "an admin's password is 12 to 128 characters"), false;
    const std::string hash = HashPassword(password);
    const std::vector<std::uint8_t> secret = RandomBytes(20);
    if (hash.empty() || secret.size() != 20) return (error = "hashing failed"), false;
    const QueryResult r = db.Query("INSERT INTO sro_vt_account.sr_web_admins (name, password_hash, totp_secret, role) VALUES ($1, $2, $3, $4)",
                                   {name, hash, Param::Bytes(secret), role});
    if (!r.Ok()) return (error = r.State() == "23505" ? "that admin exists already" : r.Error()), false;
    secretBase32 = Base32(secret);
    return true;
}

bool ResetAdminSecret(Database& db, std::string_view name, std::string& secretBase32, std::string& error) {
    const std::vector<std::uint8_t> secret = RandomBytes(20);
    const QueryResult r = db.Query("UPDATE sro_vt_account.sr_web_admins SET totp_secret = $2, totp_step = 0 WHERE lower(name) = lower($1)",
                                   {name, Param::Bytes(secret)});
    if (!r.Ok() || r.Affected() != 1) return (error = r.Ok() ? "no such admin" : r.Error()), false;
    db.Query("DELETE FROM sro_vt_account.sr_web_sessions s USING sro_vt_account.sr_web_admins a WHERE a.id = s.admin_id AND lower(a.name) = lower($1)",
             {name});
    secretBase32 = Base32(secret);
    return true;
}

bool SetAdminPassword(Database& db, std::string_view name, std::string_view password, std::string& error) {
    if (password.size() < kAdminPasswordMin || password.size() > 128) return (error = "an admin's password is 12 to 128 characters"), false;
    const std::string hash = HashPassword(password);
    const QueryResult r = db.Query("UPDATE sro_vt_account.sr_web_admins SET password_hash = $2, failed_logins = 0, locked_until = NULL "
                                   "WHERE lower(name) = lower($1)",
                                   {name, hash});
    if (!r.Ok() || r.Affected() != 1) return (error = r.Ok() ? "no such admin" : r.Error()), false;
    db.Query("DELETE FROM sro_vt_account.sr_web_sessions s USING sro_vt_account.sr_web_admins a WHERE a.id = s.admin_id AND lower(a.name) = lower($1)",
             {name});
    return true;
}

bool SetAdminDisabled(Database& db, std::string_view name, bool disabled, std::string& error) {
    const QueryResult r = db.Query("UPDATE sro_vt_account.sr_web_admins SET disabled = $2 WHERE lower(name) = lower($1)", {name, disabled ? "true" : "false"});
    if (!r.Ok() || r.Affected() != 1) return (error = r.Ok() ? "no such admin" : r.Error()), false;
    if (disabled)
        db.Query("DELETE FROM sro_vt_account.sr_web_sessions s USING sro_vt_account.sr_web_admins a WHERE a.id = s.admin_id AND lower(a.name) = lower($1)",
                 {name});
    return true;
}

std::string AuthenticatorUri(std::string_view name, std::string_view secretBase32) {
    return std::format("otpauth://totp/SroReimagined:{}?secret={}&issuer=SroReimagined&digits=6&period=30", name, secretBase32);
}

// --- The public site's data --------------------------------------------------------------------------

std::optional<std::string> SiteJson(Database& db) {
    json out = {{"content", json::object()}, {"news", json::array()}, {"images", json::array()}, {"downloads", json::array()}};
    const QueryResult content = db.Query("SELECT value FROM sro_vt_account.sr_site_content WHERE key = 'site'");
    if (!content.Ok()) return std::nullopt;
    if (content.Rows() == 1) {
        json c = json::parse(content.Text(0, 0), nullptr, false);
        if (c.is_object()) out["content"] = std::move(c);
    }
    const QueryResult news = db.Query("SELECT n.id, n.day::text, n.tag, n.title, n.body, "
                                      "       (SELECT count(*) FROM sro_vt_account.sr_site_comments c WHERE c.news_id = n.id AND NOT c.removed) "
                                      "FROM sro_vt_account.sr_site_news n WHERE n.visible ORDER BY n.day DESC, n.id DESC LIMIT 30");
    if (!news.Ok()) return std::nullopt;
    for (int i = 0; i < news.Rows(); ++i)
        out["news"].push_back({{"id", news.Int(i, 0)}, {"date", news.Text(i, 1)}, {"tag", news.Text(i, 2)}, {"title", news.Text(i, 3)}, {"text", news.Text(i, 4)},
                               {"comments", news.Int(i, 5)}});
    const QueryResult files = db.Query("SELECT kind, stored, original, label, bytes, sha256, floor(extract(epoch FROM uploaded_at))::bigint "
                                       "FROM sro_vt_account.sr_site_files WHERE visible ORDER BY uploaded_at DESC, id DESC");
    if (!files.Ok()) return std::nullopt;
    for (int i = 0; i < files.Rows(); ++i) {
        const std::string kind = files.Text(i, 0);
        if (kind == "image") {
            out["images"].push_back({{"url", "/files/images/" + files.Text(i, 1)}, {"label", files.Text(i, 3)}});
        } else {
            out["downloads"].push_back({{"url", "/files/client/" + files.Text(i, 1)},
                                        {"name", files.Text(i, 2)},
                                        {"label", files.Text(i, 3)},
                                        {"bytes", files.Int(i, 4)},
                                        {"sha256", files.Text(i, 5)},
                                        {"uploaded", files.Int(i, 6)}});
        }
    }
    return out.dump();
}

// --- The administration ----------------------------------------------------------------------------

class WebAdmin {
public:
    explicit WebAdmin(const AdminHost& host) : host_(host), loginLimiter_(std::max(1, host.config->adminLoginAttempts), std::chrono::minutes(15)) {}

    struct Session {
        int id = 0;
        std::string name, role, csrf;
    };

    void Register(httplib::Server& http);

private:
    const WebConfig& Config() const { return *host_.config; }
    DatabasePool& Pool() const { return *host_.pool; }

    void Audit(const Session* who, const std::string& name, const std::string& address, std::string action, std::string detail) {
        core::LogInfo("SroWeb admin: {} ({}) {} {}", who != nullptr ? who->name : name, address, action, detail.substr(0, 200));
        Pool().Submit([id = who != nullptr ? who->id : 0, name = who != nullptr ? who->name : name, address, action = std::move(action),
                       detail = std::move(detail)](Database& db) {
            db.Query("INSERT INTO sro_vt_account.sr_web_audit (admin_id, name, address, action, detail) VALUES ($1, $2, $3, $4, $5)",
                     {id != 0 ? Param(id) : Param(nullptr), name, address, action, detail.substr(0, 2000)});
        });
    }

    // A request from this machine itself: the peer a loopback address and no proxy's forwarding header (a reverse proxy on
    // the same machine connects from there too, and adds them).
    static bool LocalDeveloper(const httplib::Request& req) {
        const std::string& a = req.remote_addr;
        if (a != "127.0.0.1" && a != "::1" && a != "::ffff:127.0.0.1") return false;
        for (const char* h : {"X-Forwarded-For", "X-Forwarded-Host", "X-Forwarded-Proto", "X-Real-IP", "Forwarded"}) {
            if (req.has_header(h)) return false;
        }
        return true;
    }
    // The developer "local" (web-dev-local): the tester role (the developer pages alone); its second token one per run of
    // the server, told by /api/admin/me, so another site in the browser still cannot make its changes.
    std::optional<Session> LocalSession(const httplib::Request& req, httplib::Response& res, bool write) {
        static const std::string csrf = [] {
            std::string s;
            for (const std::uint8_t b : RandomBytes(32)) s += std::format("{:02x}", b);
            return s;
        }();
        if (write) {
            if (!SameOrigin(req)) {
                Refuse(res, 403, "origin", "That request did not come from this site.");
                return std::nullopt;
            }
            if (!ConstantTimeEqual(req.get_header_value("X-CSRF-Token"), csrf)) {
                Refuse(res, 403, "csrf", "The page's session token is missing; reload the page.");
                return std::nullopt;
            }
        }
        return Session{0, "local", "tester", csrf};
    }

    // The request's admin, else the answer refusing it. `write`: a change (its origin and second token checked);
    // `adminOnly`: the admin role's; `tester`: the tester role may too (its own account's routes and /dev's).
    std::optional<Session> Guard(const httplib::Request& req, httplib::Response& res, bool write, bool adminOnly, bool tester = false) {
        const std::string address = host_.visitor(req);
        if (!Allowed(Config().adminAllow, address)) {
            Refuse(res, 403, "forbidden", "Not from this address.");
            return std::nullopt;
        }
        const std::vector<std::uint8_t> token = FromHex(CookieValue(req, kCookie));
        // This machine's developer on a shard that allows it (web-dev-local): no login for the developer pages; a session's
        // cookie, when there is a live one, still wins (its own name on the notes). Changes keep the origin and token checks.
        const bool local = tester && Config().devLocal && LocalDeveloper(req);
        if (token.size() != 32) {
            if (local) return LocalSession(req, res, write);
            Refuse(res, 401, "login", "Please log in.");
            return std::nullopt;
        }
        const std::vector<std::uint8_t> hash = Sha256(token);
        const auto found = OnPool<std::optional<Session>>(Pool(), [hash](Database& db) -> std::optional<Session> {
            const QueryResult r = db.Query(std::format("UPDATE sro_vt_account.sr_web_sessions s SET seen_at = now() FROM sro_vt_account.sr_web_admins a "
                                                       "WHERE s.token_hash = $1 AND a.id = s.admin_id AND s.expires_at > now() "
                                                       "AND s.seen_at > now() - make_interval(mins => {}) AND NOT a.disabled "
                                                       "RETURNING a.id, a.name, a.role, s.csrf",
                                                       kIdleMinutes),
                                           {Param::Bytes(hash)});
            if (!r.Ok() || r.Rows() != 1) return std::nullopt;
            return Session{int(r.Int(0, 0)), r.Text(0, 1), r.Text(0, 2), r.Text(0, 3)};
        });
        if (!found) {
            Refuse(res, 503, "busy", "The server is busy; try again.");
            return std::nullopt;
        }
        if (!*found) {
            if (local) return LocalSession(req, res, write);
            Refuse(res, 401, "login", "Your session has ended; please log in.");
            return std::nullopt;
        }
        if (write) {
            if (!SameOrigin(req)) {
                Refuse(res, 403, "origin", "That request did not come from this site.");
                return std::nullopt;
            }
            if (!ConstantTimeEqual(req.get_header_value("X-CSRF-Token"), (*found)->csrf)) {
                Refuse(res, 403, "csrf", "The page's session token is missing; reload the page.");
                return std::nullopt;
            }
        }
        if (adminOnly && (*found)->role != "admin") {
            Refuse(res, 403, "role", "Your role cannot do this.");
            return std::nullopt;
        }
        if (!tester && (*found)->role == "tester") {
            Refuse(res, 403, "role", "This account is for the developer pages (/dev).");
            return std::nullopt;
        }
        return *found;
    }

    // A change's Origin (browsers send it): this site's (its Host) or one listed.
    bool SameOrigin(const httplib::Request& req) const {
        const std::string origin = req.get_header_value("Origin");
        if (origin.empty()) return true; // not a browser's cross-site request (those carry it); the second token still applies
        const std::string host = req.get_header_value("Host");
        if (origin == "http://" + host || origin == "https://" + host) return true;
        return std::find(Config().origins.begin(), Config().origins.end(), origin) != Config().origins.end();
    }

    // A JSON body, or the answer refusing it.
    static std::optional<json> Body(const httplib::Request& req, httplib::Response& res) {
        if (req.get_header_value("Content-Type").rfind("application/json", 0) != 0) {
            Refuse(res, 415, "bad-request", "The request must be JSON.");
            return std::nullopt;
        }
        json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            Refuse(res, 400, "bad-request", "The request is not valid JSON.");
            return std::nullopt;
        }
        return body;
    }

    void Login(const httplib::Request& req, httplib::Response& res);
    void Upload(const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader);
    void Command(const httplib::Request& req, httplib::Response& res, const char* kind);

    AdminHost host_;
    std::shared_ptr<void> dev_; // the developer pages' routes (WebDev.h)
    std::mutex limiterMutex_;
    AttemptLimiter loginLimiter_;
};

void WebAdmin::Login(const httplib::Request& req, httplib::Response& res) {
    const std::string address = host_.visitor(req);
    if (!Allowed(Config().adminAllow, address)) return Refuse(res, 403, "forbidden", "Not from this address.");
    const auto body = Body(req, res);
    if (!body) return;
    const std::string name = Field(*body, "name"), password = Field(*body, "password"), code = Field(*body, "code");
    {
        const std::lock_guard lock(limiterMutex_);
        if (!loginLimiter_.Allow(address)) return Refuse(res, 429, "too-many", "Too many tries from this address; wait 15 minutes.");
    }
    if (!ValidAdminName(name) || password.empty() || password.size() > 128 || code.size() != 6) {
        Audit(nullptr, name.substr(0, 32), address, "login-refused", "malformed");
        return Refuse(res, 401, "wrong", "The name, password or code is wrong.");
    }
    struct Outcome {
        bool ok = false;
        int id = 0;
        std::string name, role, csrf, token;
        std::string why;
    };
    const auto outcome = OnPool<Outcome>(Pool(), [name, password, code, address](Database& db) {
        Outcome o;
        const QueryResult a = db.Query("SELECT id, name, password_hash, totp_secret, totp_step, role, failed_logins, "
                                       "       coalesce(locked_until > now(), false), disabled "
                                       "FROM sro_vt_account.sr_web_admins WHERE lower(name) = lower($1)",
                                       {name});
        if (!a.Ok()) return (o.why = "database"), o;
        if (a.Rows() != 1) {
            VerifyPassword(password, "$argon2id$v=19$m=19456,t=2,p=1$c29tZXNhbHRzb21lc2FsdA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"); // the same time
            return (o.why = "no such admin"), o;
        }
        o.id = int(a.Int(0, 0));
        o.name = a.Text(0, 1);
        o.role = a.Text(0, 5);
        if (a.Bool(0, 8)) return (o.why = "disabled"), o;
        if (a.Bool(0, 7)) return (o.why = "locked"), o;
        std::int64_t step = 0;
        const bool passwordRight = VerifyPassword(password, a.Text(0, 2));
        const bool codeRight = VerifyTotp(a.Bytes(0, 3), code, UnixNow(), step) && step > a.Int(0, 4);
        if (!passwordRight || !codeRight) {
            db.Query(std::format("UPDATE sro_vt_account.sr_web_admins SET failed_logins = CASE WHEN failed_logins + 1 >= {0} THEN 0 ELSE failed_logins + 1 END, "
                                 "locked_until = CASE WHEN failed_logins + 1 >= {0} THEN now() + make_interval(mins => {1}) ELSE locked_until END "
                                 "WHERE id = $1",
                                 kWrongInARow, kLockMinutes),
                     {o.id});
            return (o.why = !passwordRight ? "wrong password" : "wrong code"), o;
        }
        const std::vector<std::uint8_t> token = RandomBytes(32), csrf = RandomBytes(32);
        if (token.size() != 32 || csrf.size() != 32) return (o.why = "random"), o;
        db.Query("DELETE FROM sro_vt_account.sr_web_sessions WHERE expires_at < now()");
        const QueryResult s = db.Query(std::format("INSERT INTO sro_vt_account.sr_web_sessions (token_hash, admin_id, csrf, address, expires_at) "
                                                   "VALUES ($1, $2, $3, $4, now() + make_interval(hours => {}))",
                                                   kSessionHours),
                                       {Param::Bytes(Sha256(token)), o.id, Hex(csrf), address});
        if (!s.Ok()) return (o.why = s.Error()), o;
        db.Query("UPDATE sro_vt_account.sr_web_admins SET failed_logins = 0, totp_step = $2, last_login_at = now() WHERE id = $1", {o.id, step});
        o.ok = true;
        o.token = Hex(token);
        o.csrf = Hex(csrf);
        return o;
    });
    if (!outcome) return Refuse(res, 503, "busy", "The server is busy; try again.");
    if (!outcome->ok) {
        Audit(nullptr, outcome->name.empty() ? name : outcome->name, address, "login-refused", outcome->why);
        // The same answer whatever was wrong (a locked account too): nothing tells which part to try again.
        return Refuse(res, 401, "wrong", "The name, password or code is wrong.");
    }
    Session who{outcome->id, outcome->name, outcome->role, outcome->csrf};
    Audit(&who, who.name, address, "login", "");
    res.set_header("Set-Cookie", std::format("{}={}; Path=/api/admin; HttpOnly; SameSite=Strict; Max-Age={}{}", kCookie, outcome->token,
                                             kSessionHours * 3600, Config().secureCookie ? "; Secure" : ""));
    Answer(res, 200, {{"name", who.name}, {"role", who.role}, {"csrf", who.csrf}});
}

void WebAdmin::Command(const httplib::Request& req, httplib::Response& res, const char* kind) {
    const bool console = std::string_view(kind) == "console";
    const auto who = Guard(req, res, true, console);
    if (!who) return;
    const auto body = Body(req, res);
    if (!body) return;
    std::string text = Field(*body, "text");
    const int server = body->contains("server") && (*body)["server"].is_number_integer() ? (*body)["server"].get<int>() : 0;
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    if (text.empty() || text.size() > (console ? 200u : 500u)) return Refuse(res, 400, "text", console ? "A command is 1 to 200 characters." : "An announcement is 1 to 500 characters.");
    if (server < 0 || server > 999) return Refuse(res, 400, "server", "No such game server.");
    const std::string shard = Config().shardSchema, by = who->name;
    const auto id = OnPool<std::int64_t>(Pool(), [shard, server, kind = std::string(kind), text, by](Database& db) -> std::int64_t {
        const QueryResult r = db.Query("INSERT INTO sro_vt_account.sr_admin_commands (shard, server_id, kind, text, created_by) VALUES ($1, $2, $3, $4, $5) RETURNING id",
                                       {shard, server, kind, text, by});
        if (!r.Ok()) return 0;
        Notify(db, "sr_admin_" + shard, std::to_string(r.Int(0, 0)));
        return r.Int(0, 0);
    });
    if (!id || *id == 0) return Refuse(res, 500, "server", "It could not be sent.");
    Audit(&*who, who->name, host_.visitor(req), kind, std::format("#{} server {}: {}", *id, server, text));
    Answer(res, 200, {{"id", *id}});
}

void WebAdmin::Upload(const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) {
    const auto who = Guard(req, res, true, true);
    if (!who) return;
    const std::string kind = req.get_param_value("kind");
    if (kind != "image" && kind != "client") return Refuse(res, 400, "kind", "Upload an image or a client.");
    std::string original = fs::path(req.get_param_value("name")).filename().string();
    const std::string label = req.get_param_value("label").substr(0, 200);
    if (original.empty() || original.size() > 200) return Refuse(res, 400, "name", "The file needs its name.");
    const std::string ext = Lower(fs::path(original).extension().string());
    if (kind == "client" && ext != ".zip" && ext != ".7z" && ext != ".rar" && ext != ".exe" && ext != ".msi")
        return Refuse(res, 400, "type", "A client is a .zip, .7z, .rar, .exe or .msi file.");
    const std::int64_t max = kind == "image" ? Config().imageMaxBytes : Config().uploadMaxBytes;
    const std::string lengthHeader = req.get_header_value("Content-Length");
    const std::int64_t length = lengthHeader.empty() ? -1 : std::atoll(lengthHeader.c_str());
    if (length <= 0) return Refuse(res, 411, "length", "The upload must say its size.");
    if (length > max) return Refuse(res, 413, "too-big", std::format("That file is larger than {} MB.", max >> 20));

    std::error_code ec;
    const fs::path root = fs::absolute(Config().filesDir), temp = root.parent_path() / (root.filename().string() + ".upload");
    fs::create_directories(temp, ec);
    const std::string id = Hex(RandomBytes(16));
    const fs::path part = temp / (id + ".part");
    std::ofstream out(part, std::ios::binary);
    if (!out) return Refuse(res, 500, "disk", "The upload folder cannot be written.");
    Sha256Stream hash;
    std::int64_t got = 0;
    std::string head;
    const bool read = reader([&](const char* data, std::size_t size) {
        got += std::int64_t(size);
        if (got > max) return false;
        if (head.size() < 16) head.append(data, std::min<std::size_t>(size, 16 - head.size()));
        out.write(data, std::streamsize(size));
        hash.Add({reinterpret_cast<const std::uint8_t*>(data), size});
        return bool(out);
    });
    out.close();
    const auto fail = [&](int status, const char* code, const std::string& message) {
        fs::remove(part, ec);
        Refuse(res, status, code, message);
    };
    if (!read || got != length) return fail(400, "incomplete", "The upload did not arrive whole.");
    std::string storedExt = ext;
    if (kind == "image") {
        const auto starts = [&](std::string_view magic, std::size_t at = 0) { return head.size() >= at + magic.size() && head.compare(at, magic.size(), magic) == 0; };
        if (starts("\xFF\xD8\xFF")) storedExt = ".jpg";
        else if (starts("\x89PNG\r\n\x1A\n")) storedExt = ".png";
        else if (starts("RIFF") && starts("WEBP", 8)) storedExt = ".webp";
        else return fail(400, "type", "Images are JPEG, PNG or WebP.");
    }
    const std::string stored = id + storedExt;
    const fs::path dir = root / (kind == "image" ? "images" : "client");
    fs::create_directories(dir, ec);
    fs::rename(part, dir / stored, ec);
    if (ec) return fail(500, "disk", "The file could not be stored.");
    const std::string sha = Hex(hash.Finish());
    const std::string by = who->name;
    const auto row = OnPool<std::int64_t>(Pool(), [kind, stored, original, label, got, sha, by](Database& db) -> std::int64_t {
        const QueryResult r = db.Query("INSERT INTO sro_vt_account.sr_site_files (kind, stored, original, label, bytes, sha256, uploaded_by) "
                                       "VALUES ($1, $2, $3, $4, $5, $6, $7) RETURNING id",
                                       {kind, stored, original, label, got, sha, by});
        return r.Ok() ? r.Int(0, 0) : 0;
    });
    if (!row || *row == 0) {
        fs::remove(dir / stored, ec);
        return Refuse(res, 500, "server", "The file could not be recorded.");
    }
    Audit(&*who, who->name, host_.visitor(req), "upload", std::format("{} {} ({} bytes, sha256 {})", kind, original, got, sha));
    host_.siteChanged();
    Answer(res, 201, {{"id", *row}, {"url", std::string("/files/") + (kind == "image" ? "images/" : "client/") + stored}, {"sha256", sha}, {"bytes", got}});
}

void WebAdmin::Register(httplib::Server& http) {
    // Bodies stay small except an upload's (the admin's, checked before a byte is read).
    http.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        if (req.path == "/api/admin/files" && req.method == "POST") return httplib::Server::HandlerResponse::Unhandled;
        if (web::DevUploadPath(req.method, req.path)) return httplib::Server::HandlerResponse::Unhandled;
        const std::string length = req.get_header_value("Content-Length");
        if ((!length.empty() && std::atoll(length.c_str()) > std::int64_t(kBodyMax)) || req.has_header("Transfer-Encoding")) {
            Refuse(res, 413, "too-big", "The request is too large.");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    http.Post("/api/admin/login", [this](const httplib::Request& req, httplib::Response& res) { Login(req, res); });
    // The developer pages (the skill lab): any role's session, or a key.
    web::DevAccess dev;
    dev.pool = host_.pool;
    dev.visitor = host_.visitor;
    dev.session = [this](const httplib::Request& req, httplib::Response& res, bool write) -> std::optional<web::DevWho> {
        const auto who = Guard(req, res, write, false, true);
        if (!who) return std::nullopt;
        return web::DevWho{who->id, who->name, who->role, false};
    };
    dev.audit = [this](const web::DevWho& who, const std::string& address, std::string action, std::string detail) {
        const Session s{who.adminId, who.key ? "key:" + who.name : who.name, who.role, {}};
        Audit(&s, s.name, address, std::move(action), std::move(detail));
    };
    dev.videos = fs::absolute(Config().labDir) / "videos";
    dev_ = web::RegisterDev(http, std::move(dev));
    http.Post("/api/admin/logout", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res, true, false, true);
        if (!who) return;
        const std::vector<std::uint8_t> token = FromHex(CookieValue(req, kCookie));
        OnPool<bool>(Pool(), [hash = Sha256(token)](Database& db) {
            return db.Query("DELETE FROM sro_vt_account.sr_web_sessions WHERE token_hash = $1", {Param::Bytes(hash)}).Ok();
        });
        Audit(&*who, who->name, host_.visitor(req), "logout", "");
        res.set_header("Set-Cookie", std::format("{}=; Path=/api/admin; HttpOnly; SameSite=Strict; Max-Age=0{}", kCookie, Config().secureCookie ? "; Secure" : ""));
        Answer(res, 200, {{"ok", true}});
    });
    // Who is logged in, and the second token (a page reloaded asks for it; only this site can read the answer).
    http.Get("/api/admin/me", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res, false, false, true);
        if (who) Answer(res, 200, {{"name", who->name}, {"role", who->role}, {"csrf", who->csrf}});
    });
    http.Post("/api/admin/password", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res, true, false, true);
        if (!who) return;
        const auto body = Body(req, res);
        if (!body) return;
        const std::string current = Field(*body, "current"), next = Field(*body, "password");
        if (next.size() < kAdminPasswordMin || next.size() > 128) return Refuse(res, 400, "password", "A new password is 12 to 128 characters.");
        const auto ok = OnPool<bool>(Pool(), [id = who->id, current, next](Database& db) {
            const QueryResult r = db.Query("SELECT password_hash FROM sro_vt_account.sr_web_admins WHERE id = $1", {id});
            if (!r.Ok() || r.Rows() != 1 || !VerifyPassword(current, r.Text(0, 0))) return false;
            return db.Query("UPDATE sro_vt_account.sr_web_admins SET password_hash = $2 WHERE id = $1", {id, HashPassword(next)}).Ok();
        });
        if (!ok || !*ok) return Refuse(res, 400, "wrong", "The current password is wrong.");
        Audit(&*who, who->name, host_.visitor(req), "password-own", "");
        Answer(res, 200, {{"ok", true}});
    });

    // The site's texts (one document: what site.ts's defaults may be replaced with).
    http.Get("/api/admin/site", [this](const httplib::Request& req, httplib::Response& res) {
        if (!Guard(req, res, false, true)) return;
        const auto value = OnPool<std::string>(Pool(), [](Database& db) {
            const QueryResult r = db.Query("SELECT value FROM sro_vt_account.sr_site_content WHERE key = 'site'");
            return r.Ok() && r.Rows() == 1 ? r.Text(0, 0) : std::string("{}");
        });
        if (!value) return Refuse(res, 503, "busy", "The server is busy; try again.");
        Answer(res, 200, {{"content", json::parse(*value, nullptr, false)}});
    });
    http.Put("/api/admin/site", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res, true, true);
        if (!who) return;
        const auto body = Body(req, res);
        if (!body) return;
        const auto it = body->find("content");
        std::string why;
        if (it == body->end() || !it->is_object()) return Refuse(res, 400, "content", "The texts must be an object.");
        const std::string text = it->dump();
        if (text.size() > kSiteMax) return Refuse(res, 413, "too-big", "The texts are too long.");
        if (!ValidContent(*it, "", why)) return Refuse(res, 400, "content", why);
        const auto ok = OnPool<bool>(Pool(), [text, by = who->name](Database& db) {
            return db.Query("INSERT INTO sro_vt_account.sr_site_content (key, value, updated_by) VALUES ('site', $1, $2) "
                            "ON CONFLICT (key) DO UPDATE SET value = $1, updated_by = $2, updated_at = now()",
                            {text, by})
                .Ok();
        });
        if (!ok || !*ok) return Refuse(res, 500, "server", "The texts could not be saved.");
        Audit(&*who, who->name, host_.visitor(req), "site-texts", std::format("{} bytes", text.size()));
        host_.siteChanged();
        Answer(res, 200, {{"ok", true}});
    });

    // News.
    const auto newsFields = [](const json& body, std::string& day, std::string& tag, std::string& title, std::string& text, bool& visible, std::string& why) {
        day = Field(body, "date");
        tag = Field(body, "tag");
        title = Field(body, "title");
        text = Field(body, "text");
        visible = !body.contains("visible") || !body["visible"].is_boolean() || body["visible"].get<bool>();
        unsigned y = 0, m = 0, d = 0;
        if (day.size() != 10 || std::sscanf(day.c_str(), "%4u-%2u-%2u", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return (why = "The date is YYYY-MM-DD."), false;
        if (tag.empty() || tag.size() > 32) return (why = "The tag is 1 to 32 characters."), false;
        if (title.empty() || title.size() > 200) return (why = "The title is 1 to 200 characters."), false;
        if (text.size() > 20000) return (why = "The text is too long."), false;
        return true;
    };
    http.Get("/api/admin/news", [this](const httplib::Request& req, httplib::Response& res) {
        if (!Guard(req, res, false, true)) return;
        const auto list = OnPool<json>(Pool(), [](Database& db) {
            json a = json::array();
            const QueryResult r = db.Query("SELECT id, day::text, tag, title, body, visible FROM sro_vt_account.sr_site_news ORDER BY day DESC, id DESC");
            for (int i = 0; r.Ok() && i < r.Rows(); ++i)
                a.push_back({{"id", r.Int(i, 0)}, {"date", r.Text(i, 1)}, {"tag", r.Text(i, 2)}, {"title", r.Text(i, 3)}, {"text", r.Text(i, 4)}, {"visible", r.Bool(i, 5)}});
            return a;
        });
        if (!list) return Refuse(res, 503, "busy", "The server is busy; try again.");
        Answer(res, 200, {{"news", *list}});
    });
    http.Post("/api/admin/news", [this, newsFields](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res, true, true);
        if (!who) return;
        const auto body = Body(req, res);
        if (!body) return;
        std::string day, tag, title, text, why;
        bool visible = true;
        if (!newsFields(*body, day, tag, title, text, visible, why)) return Refuse(res, 400, "news", why);
        const auto id = OnPool<std::int64_t>(Pool(), [day, tag, title, text, visible](Database& db) -> std::int64_t {
            const QueryResult r = db.Query("INSERT INTO sro_vt_account.sr_site_news (day, tag, title, body, visible) VALUES ($1::date, $2, $3, $4, $5) RETURNING id",
                                           {day, tag, title, text, visible ? "true" : "false"});
            return r.Ok() ? r.Int(0, 0) : 0;
        });
        if (!id || *id == 0) return Refuse(res, 400, "news", "The news could not be saved (is the date real?).");
        Audit(&*who, who->name, host_.visitor(req), "news-add", std::format("#{} {}", *id, title));
        host_.siteChanged();
        Answer(res, 201, {{"id", *id}});
    });
    http.Put("/api/admin/news/:id", [this, newsFields](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res, true, true);
        if (!who) return;
        const auto body = Body(req, res);
        if (!body) return;
        const std::int64_t id = std::atoll(req.path_params.at("id").c_str());
        std::string day, tag, title, text, why;
        bool visible = true;
        if (!newsFields(*body, day, tag, title, text, visible, why)) return Refuse(res, 400, "news", why);
        const auto ok = OnPool<bool>(Pool(), [id, day, tag, title, text, visible](Database& db) {
            const QueryResult r = db.Query("UPDATE sro_vt_account.sr_site_news SET day = $2::date, tag = $3, title = $4, body = $5, visible = $6, updated_at = now() WHERE id = $1",
                                           {id, day, tag, title, text, visible ? "true" : "false"});
            return r.Ok() && r.Affected() == 1;
        });
        if (!ok || !*ok) return Refuse(res, 404, "news", "No such news.");
        Audit(&*who, who->name, host_.visitor(req), "news-edit", std::format("#{} {}", id, title));
        host_.siteChanged();
        Answer(res, 200, {{"ok", true}});
    });
    http.Delete("/api/admin/news/:id", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res, true, true);
        if (!who) return;
        const std::int64_t id = std::atoll(req.path_params.at("id").c_str());
        const auto ok = OnPool<bool>(Pool(), [id](Database& db) {
            const QueryResult r = db.Query("DELETE FROM sro_vt_account.sr_site_news WHERE id = $1", {id});
            return r.Ok() && r.Affected() == 1;
        });
        if (!ok || !*ok) return Refuse(res, 404, "news", "No such news.");
        Audit(&*who, who->name, host_.visitor(req), "news-delete", std::format("#{}", id));
        host_.siteChanged();
        Answer(res, 200, {{"ok", true}});
    });

    // Files: the slider's images and the client.
    http.Get("/api/admin/files", [this](const httplib::Request& req, httplib::Response& res) {
        if (!Guard(req, res, false, true)) return;
        const auto list = OnPool<json>(Pool(), [](Database& db) {
            json a = json::array();
            const QueryResult r = db.Query("SELECT id, kind, stored, original, label, bytes, sha256, visible, floor(extract(epoch FROM uploaded_at))::bigint, uploaded_by "
                                           "FROM sro_vt_account.sr_site_files ORDER BY uploaded_at DESC, id DESC");
            for (int i = 0; r.Ok() && i < r.Rows(); ++i) {
                const std::string kind = r.Text(i, 1);
                a.push_back({{"id", r.Int(i, 0)}, {"kind", kind}, {"url", std::string("/files/") + (kind == "image" ? "images/" : "client/") + r.Text(i, 2)},
                             {"name", r.Text(i, 3)}, {"label", r.Text(i, 4)}, {"bytes", r.Int(i, 5)}, {"sha256", r.Text(i, 6)}, {"visible", r.Bool(i, 7)},
                             {"uploaded", r.Int(i, 8)}, {"by", r.Text(i, 9)}});
            }
            return a;
        });
        if (!list) return Refuse(res, 503, "busy", "The server is busy; try again.");
        Answer(res, 200, {{"files", *list}});
    });
    http.Post("/api/admin/files", [this](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) { Upload(req, res, reader); });
    http.Put("/api/admin/files/:id", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res, true, true);
        if (!who) return;
        const auto body = Body(req, res);
        if (!body) return;
        const std::int64_t id = std::atoll(req.path_params.at("id").c_str());
        const std::string label = Field(*body, "label").substr(0, 200);
        const bool visible = !body->contains("visible") || !(*body)["visible"].is_boolean() || (*body)["visible"].get<bool>();
        const auto ok = OnPool<bool>(Pool(), [id, label, visible](Database& db) {
            const QueryResult r = db.Query("UPDATE sro_vt_account.sr_site_files SET label = $2, visible = $3 WHERE id = $1", {id, label, visible ? "true" : "false"});
            return r.Ok() && r.Affected() == 1;
        });
        if (!ok || !*ok) return Refuse(res, 404, "file", "No such file.");
        Audit(&*who, who->name, host_.visitor(req), "file-edit", std::format("#{} {} {}", id, label, visible ? "shown" : "hidden"));
        host_.siteChanged();
        Answer(res, 200, {{"ok", true}});
    });
    http.Delete("/api/admin/files/:id", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res, true, true);
        if (!who) return;
        const std::int64_t id = std::atoll(req.path_params.at("id").c_str());
        const auto gone = OnPool<std::optional<std::pair<std::string, std::string>>>(Pool(), [id](Database& db) -> std::optional<std::pair<std::string, std::string>> {
            const QueryResult r = db.Query("DELETE FROM sro_vt_account.sr_site_files WHERE id = $1 RETURNING kind, stored", {id});
            if (!r.Ok() || r.Rows() != 1) return std::nullopt;
            return std::pair{r.Text(0, 0), r.Text(0, 1)};
        });
        if (!gone || !*gone) return Refuse(res, 404, "file", "No such file.");
        std::error_code ec;
        fs::remove(fs::absolute(Config().filesDir) / ((*gone)->first == "image" ? "images" : "client") / (*gone)->second, ec);
        Audit(&*who, who->name, host_.visitor(req), "file-delete", std::format("#{} {}", id, (*gone)->second));
        host_.siteChanged();
        Answer(res, 200, {{"ok", true}});
    });

    // Players: who is online, a search, bans and passwords.
    http.Get("/api/admin/players/online", [this](const httplib::Request& req, httplib::Response& res) {
        if (!Guard(req, res, false, false)) return;
        const auto list = OnPool<json>(Pool(), [](Database& db) {
            json a = json::array();
            // The character in play (account migration 5: the game servers set it; none in the lobby).
            const QueryResult r = db.Query("SELECT u.struserid, s.shard_id, floor(extract(epoch FROM s.since))::bigint, coalesce(c.charname16, '') "
                                           "FROM sro_vt_account.sr_online_sessions s JOIN sro_vt_account.tb_user u ON u.jid = s.jid "
                                           "LEFT JOIN _char c ON c.charid = s.character_id ORDER BY s.since");
            for (int i = 0; r.Ok() && i < r.Rows(); ++i)
                a.push_back({{"account", r.Text(i, 0)}, {"server", r.Int(i, 1)}, {"since", r.Int(i, 2)}, {"character", r.Text(i, 3)}});
            return a;
        });
        if (!list) return Refuse(res, 503, "busy", "The server is busy; try again.");
        Answer(res, 200, {{"online", *list}});
    });
    http.Get("/api/admin/players", [this](const httplib::Request& req, httplib::Response& res) {
        if (!Guard(req, res, false, false)) return;
        const std::string q = req.get_param_value("q").substr(0, 32);
        if (q.size() < 2) return Refuse(res, 400, "query", "Type at least two letters of an account or character name.");
        const auto list = OnPool<json>(Pool(), [q](Database& db) {
            json a = json::array();
            const QueryResult r = db.Query(
                "SELECT u.jid, u.struserid, coalesce(u.email, ''), coalesce(floor(extract(epoch FROM u.regtime))::bigint, 0), "
                "       (SELECT o.shard_id FROM sro_vt_account.sr_online_sessions o WHERE o.jid = u.jid), "
                "       coalesce((SELECT string_agg(c.charname16 || ' (' || c.curlevel || ')', ', ' ORDER BY c.curlevel DESC) "
                "                 FROM _user us JOIN _char c ON c.charid = us.charid AND c.deleted = 0 WHERE us.userjid = u.jid), '') "
                "FROM sro_vt_account.tb_user u "
                "WHERE starts_with(lower(u.struserid), lower($1)) OR u.jid IN (SELECT us.userjid FROM _user us JOIN _char c ON c.charid = us.charid "
                "                                                              WHERE starts_with(lower(c.charname16), lower($1))) "
                "ORDER BY u.struserid LIMIT 25",
                {q});
            for (int i = 0; r.Ok() && i < r.Rows(); ++i) {
                bool ok = false;
                const auto account = FindAccount(db, r.Text(i, 1), ok);
                a.push_back({{"account", r.Text(i, 1)},
                             {"email", r.Text(i, 2)},
                             {"registered", r.Int(i, 3)},
                             {"online", r.IsNull(i, 4) ? json(nullptr) : json(r.Int(i, 4))},
                             {"characters", r.Text(i, 5)},
                             {"blocked", account && account->blocked},
                             {"blockedUntil", account ? account->blockedUntil : 0},
                             {"reason", account ? account->blockReason : ""}});
            }
            return a;
        });
        if (!list) return Refuse(res, 503, "busy", "The server is busy; try again.");
        Answer(res, 200, {{"players", *list}});
    });
    http.Post("/api/admin/players/ban", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res, true, false);
        if (!who) return;
        const auto body = Body(req, res);
        if (!body) return;
        const std::string account = Field(*body, "account"), reason = Field(*body, "reason");
        const int days = (*body).contains("days") && (*body)["days"].is_number_integer() ? (*body)["days"].get<int>() : -1;
        if (!ValidAccountName(account)) return Refuse(res, 400, "account", "No such account.");
        if (reason.empty() || reason.size() > 200) return Refuse(res, 400, "reason", "A ban needs a reason (up to 200 characters).");
        if (days < 0 || days > 3650) return Refuse(res, 400, "days", "Days: 0 (for good) to 3650.");
        const std::string shard = Config().shardSchema, by = who->name;
        const auto ok = OnPool<bool>(Pool(), [account, reason, days, shard, by](Database& db) {
            bool found = false;
            if (!FindAccount(db, account, found) || !SetBlock(db, account, reason, std::int64_t(days) * 86400)) return false;
            // The game servers put it out if it is in (their side: sr_admin_commands kind "kick").
            const QueryResult r = db.Query("INSERT INTO sro_vt_account.sr_admin_commands (shard, kind, text, created_by) VALUES ($1, 'kick', $2, $3) RETURNING id",
                                           {shard, account, by});
            if (r.Ok()) Notify(db, "sr_admin_" + shard, std::to_string(r.Int(0, 0)));
            return true;
        });
        if (!ok || !*ok) return Refuse(res, 404, "account", "No such account.");
        Audit(&*who, who->name, host_.visitor(req), "ban", std::format("{} for {} ({})", account, days == 0 ? std::string("good") : std::format("{} days", days), reason));
        Answer(res, 200, {{"ok", true}});
    });
    http.Post("/api/admin/players/unban", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res, true, false);
        if (!who) return;
        const auto body = Body(req, res);
        if (!body) return;
        const std::string account = Field(*body, "account");
        if (!ValidAccountName(account)) return Refuse(res, 400, "account", "No such account.");
        const auto ok = OnPool<bool>(Pool(), [account](Database& db) { return SetBlock(db, account, "", 0); });
        if (!ok || !*ok) return Refuse(res, 404, "account", "No such account.");
        Audit(&*who, who->name, host_.visitor(req), "unban", account);
        Answer(res, 200, {{"ok", true}});
    });
    http.Post("/api/admin/players/password", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res, true, true);
        if (!who) return;
        const auto body = Body(req, res);
        if (!body) return;
        const std::string account = Field(*body, "account"), password = Field(*body, "password");
        if (!ValidAccountName(account)) return Refuse(res, 400, "account", "No such account.");
        if (!ValidPassword(password)) return Refuse(res, 400, "password", "Passwords are 6 to 64 characters.");
        const auto ok = OnPool<bool>(Pool(), [account, password, legacy = Config().legacyPasswords](Database& db) {
            bool found = false;
            const auto row = FindAccount(db, account, found);
            if (!row) return false;
            // Ours, and the original's form of the new one (or none): the old password works nowhere after this.
            if (!StorePasswordHash(db, row->id, HashPassword(password), LegacyPasswords::Clear)) return false;
            if (legacy == LegacyPasswords::Keep)
                db.Query("UPDATE sro_vt_account.tb_user SET password = $2 WHERE jid = $1", {row->id, LegacyPasswordHash(password)});
            return true;
        });
        if (!ok || !*ok) return Refuse(res, 404, "account", "No such account.");
        Audit(&*who, who->name, host_.visitor(req), "player-password", account);
        Answer(res, 200, {{"ok", true}});
    });

    // The game servers: announcements (moderators too) and the console (admins), carried out by them (sr_admin_commands).
    http.Post("/api/admin/announce", [this](const httplib::Request& req, httplib::Response& res) { Command(req, res, "announce"); });
    http.Post("/api/admin/console", [this](const httplib::Request& req, httplib::Response& res) { Command(req, res, "console"); });
    http.Get("/api/admin/commands", [this](const httplib::Request& req, httplib::Response& res) {
        if (!Guard(req, res, false, false)) return;
        const auto list = OnPool<json>(Pool(), [shard = Config().shardSchema](Database& db) {
            json a = json::array();
            const QueryResult r = db.Query("SELECT id, server_id, kind, text, created_by, floor(extract(epoch FROM created_at))::bigint, "
                                           "       coalesce(floor(extract(epoch FROM done_at))::bigint, 0), coalesce(done_by, 0), coalesce(result, '') "
                                           "FROM sro_vt_account.sr_admin_commands WHERE shard = $1 ORDER BY id DESC LIMIT 50",
                                           {shard});
            for (int i = 0; r.Ok() && i < r.Rows(); ++i)
                a.push_back({{"id", r.Int(i, 0)}, {"server", r.Int(i, 1)}, {"kind", r.Text(i, 2)}, {"text", r.Text(i, 3)}, {"by", r.Text(i, 4)},
                             {"at", r.Int(i, 5)}, {"doneAt", r.Int(i, 6)}, {"doneBy", r.Int(i, 7)}, {"result", r.Text(i, 8)}});
            return a;
        });
        if (!list) return Refuse(res, 503, "busy", "The server is busy; try again.");
        Answer(res, 200, {{"commands", *list}});
    });

    // The players' comments on the news: the latest, and their removal (moderators too).
    http.Get("/api/admin/comments", [this](const httplib::Request& req, httplib::Response& res) {
        if (!Guard(req, res, false, false)) return;
        const auto list = OnPool<json>(Pool(), [](Database& db) {
            json a = json::array();
            const QueryResult r = db.Query("SELECT c.id, c.news_id, coalesce(n.title, ''), c.author, u.struserid, c.body, floor(extract(epoch FROM c.created_at))::bigint, "
                                           "       c.removed, coalesce(c.removed_by, ''), c.address "
                                           "FROM sro_vt_account.sr_site_comments c LEFT JOIN sro_vt_account.sr_site_news n ON n.id = c.news_id "
                                           "LEFT JOIN sro_vt_account.tb_user u ON u.jid = c.jid ORDER BY c.id DESC LIMIT 100");
            for (int i = 0; r.Ok() && i < r.Rows(); ++i)
                a.push_back({{"id", r.Int(i, 0)}, {"news", r.Int(i, 1)}, {"title", r.Text(i, 2)}, {"author", r.Text(i, 3)}, {"account", r.Text(i, 4)},
                             {"text", r.Text(i, 5)}, {"at", r.Int(i, 6)}, {"removed", r.Bool(i, 7)}, {"removedBy", r.Text(i, 8)}, {"address", r.Text(i, 9)}});
            return a;
        });
        if (!list) return Refuse(res, 503, "busy", "The server is busy; try again.");
        Answer(res, 200, {{"comments", *list}});
    });
    http.Delete("/api/admin/comments/:id", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res, true, false);
        if (!who) return;
        const std::int64_t id = std::atoll(req.path_params.at("id").c_str());
        const auto ok = OnPool<bool>(Pool(), [id, by = who->name](Database& db) {
            const QueryResult r = db.Query("UPDATE sro_vt_account.sr_site_comments SET removed = true, removed_by = $2 WHERE id = $1 AND NOT removed", {id, by});
            return r.Ok() && r.Affected() == 1;
        });
        if (!ok || !*ok) return Refuse(res, 404, "comment", "No such comment.");
        Audit(&*who, who->name, host_.visitor(req), "comment-remove", std::format("#{}", id));
        host_.siteChanged();
        Answer(res, 200, {{"ok", true}});
    });

    http.Get("/api/admin/audit", [this](const httplib::Request& req, httplib::Response& res) {
        if (!Guard(req, res, false, true)) return;
        const auto list = OnPool<json>(Pool(), [](Database& db) {
            json a = json::array();
            const QueryResult r = db.Query("SELECT floor(extract(epoch FROM at))::bigint, name, address, action, detail FROM sro_vt_account.sr_web_audit ORDER BY id DESC LIMIT 200");
            for (int i = 0; r.Ok() && i < r.Rows(); ++i)
                a.push_back({{"at", r.Int(i, 0)}, {"name", r.Text(i, 1)}, {"address", r.Text(i, 2)}, {"action", r.Text(i, 3)}, {"detail", r.Text(i, 4)}});
            return a;
        });
        if (!list) return Refuse(res, 503, "busy", "The server is busy; try again.");
        Answer(res, 200, {{"audit", *list}});
    });
}

std::shared_ptr<WebAdmin> RegisterAdmin(httplib::Server& http, const AdminHost& host) {
    auto admin = std::make_shared<WebAdmin>(host);
    admin->Register(http);
    return admin;
}

} // namespace sr::server
