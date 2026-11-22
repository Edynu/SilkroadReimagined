#include "WebPlayers.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "sr/core/Log.h"
#include "sr/server/Crypto.h"
#include "sr/server/Service.h"
#include "sr/server/SilkStore.h"
#include "sr/server/Store.h"
#include "sr/server/WebServer.h"
#include "WebCommon.h"
#include "WebSupport.h"

namespace sr::server {

namespace {

using namespace web;

constexpr const char* kCookie = "sro_player";
constexpr int kSessionDays = 30;
constexpr std::size_t kCommentMax = 1000;
constexpr int kCommentGapSeconds = 20;

struct Character {
    std::string name;
    int level = 0;
    std::string race;
};

std::vector<Character> CharactersOf(Database& db, std::int64_t jid) {
    std::vector<Character> out;
    const QueryResult r = db.Query("SELECT c.charname16, c.curlevel, c.refobjid FROM _user u JOIN _char c ON c.charid = u.charid "
                                   "WHERE u.userjid = $1 AND c.deleted = 0 AND c.charid <> 0 ORDER BY c.curlevel DESC, c.charid",
                                   {jid});
    for (int i = 0; r.Ok() && i < r.Rows(); ++i)
        out.push_back({r.Text(i, 0), int(r.Int(i, 1)), r.Int(i, 2) >= 14875 ? "European" : "Chinese"}); // characterdata 1907-1932, 14875-14900
    return out;
}

// The player's own page (/account): its account and each character in more detail. Race and sex from the character's
// model (characterdata: 1907-1919 Chinese men, 1920-1932 women; 14875-14887 European men, 14888-14900 women), the job
// from _chartrijob (1 trader, 2 thief, 3 hunter), "online" from the game servers' sessions.
json ProfileJson(Database& db, std::int64_t jid) {
    json account = json::object();
    const QueryResult u = db.Query("SELECT struserid, coalesce(email, ''), coalesce(floor(extract(epoch FROM regtime))::bigint, 0) "
                                   "FROM sro_vt_account.tb_user WHERE jid = $1",
                                   {jid});
    if (!u.Ok() || u.Rows() != 1) return nullptr;
    const std::string email = u.Text(0, 1);
    // The address shown masked (the page is the player's, but a screen may be shared): j***@example.com.
    std::string masked;
    if (const auto at = email.find('@'); at != std::string::npos && at > 0) masked = email.substr(0, 1) + "***" + email.substr(at);
    account = {{"name", u.Text(0, 0)}, {"email", masked}, {"hasEmail", !email.empty()}, {"created", u.Int(0, 2)}};
    SilkBalance silk;
    std::string error;
    if (LoadSilk(db, jid, silk, error)) account["silk"] = {{"own", silk.own}, {"gift", silk.gift}, {"point", silk.point}};

    json characters = json::array();
    const QueryResult c = db.Query(
        "SELECT c.charname16, c.curlevel, c.refobjid, c.remaingold, c.remainskillpoint, c.strength, c.intellect, c.hp, c.mp, "
        "       coalesce(g.name, ''), coalesce(j.jobtype, 0), coalesce(j.level, 0), floor(extract(epoch FROM c.lastlogout))::bigint, "
        "       EXISTS (SELECT 1 FROM sro_vt_account.sr_online_sessions o WHERE o.character_id = c.charid), c.nickname16, c.totalpk, "
        "       c.remainstatpoint, c.maxlevel "
        "FROM _user u JOIN _char c ON c.charid = u.charid "
        "LEFT JOIN _guild g ON g.id = c.guildid AND c.guildid <> 0 "
        "LEFT JOIN _chartrijob j ON j.charid = c.charid "
        "WHERE u.userjid = $1 AND c.deleted = 0 AND c.charid <> 0 ORDER BY c.curlevel DESC, c.charid",
        {jid});
    static const char* jobs[] = {"", "Trader", "Thief", "Hunter"};
    for (int i = 0; c.Ok() && i < c.Rows(); ++i) {
        const std::int64_t model = c.Int(i, 2);
        const bool european = model >= 14875;
        const bool woman = european ? model >= 14888 : model >= 1920;
        const std::int64_t job = c.Int(i, 10);
        characters.push_back({{"name", c.Text(i, 0)},
                              {"level", c.Int(i, 1)},
                              {"maxLevel", c.Int(i, 17)},
                              {"race", european ? "European" : "Chinese"},
                              {"sex", woman ? "female" : "male"},
                              {"gold", c.Int(i, 3)},
                              {"skillPoints", c.Int(i, 4)},
                              {"statPoints", c.Int(i, 16)},
                              {"strength", c.Int(i, 5)},
                              {"intellect", c.Int(i, 6)},
                              {"hp", c.Int(i, 7)},
                              {"mp", c.Int(i, 8)},
                              {"guild", c.Text(i, 9)},
                              {"job", job >= 0 && job <= 3 ? jobs[job] : ""},
                              {"jobLevel", c.Int(i, 11)},
                              {"jobAlias", c.Text(i, 14)},
                              {"pk", c.Int(i, 15)},
                              {"lastPlayed", c.Int(i, 12)},
                              {"online", c.Bool(i, 13)}});
    }
    return {{"account", account}, {"characters", characters}};
}

json CharactersJson(const std::vector<Character>& list) {
    json a = json::array();
    for (const Character& c : list) a.push_back({{"name", c.name}, {"level", c.level}, {"race", c.race}});
    return a;
}

std::string BlockedWords(const AccountRow& a) {
    std::string words = "The account is blocked";
    if (a.blockedUntil > 0) {
        const std::time_t t = std::time_t(a.blockedUntil);
        char when[64] = {};
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M UTC", std::gmtime(&t));
        words += std::string(" until ") + when;
    }
    return words + (a.blockReason.empty() ? std::string(".") : ": " + a.blockReason + ".");
}

} // namespace

class WebPlayers {
public:
    explicit WebPlayers(const AdminHost& host)
        : host_(host), addressLimiter_(10, std::chrono::minutes(15)), accountLimiter_(10, std::chrono::minutes(15)),
          supportLimiter_(std::max(1, host.config->supportAttempts), std::chrono::hours(1)) {}

    struct Session {
        std::int64_t jid = 0;
        std::string account, csrf;
    };

    void Register(httplib::Server& http);

private:
    const WebConfig& Config() const { return *host_.config; }
    DatabasePool& Pool() const { return *host_.pool; }

    // The request's player, or none (no answer written: the caller decides).
    std::optional<Session> Find(const httplib::Request& req) {
        const std::vector<std::uint8_t> token = FromHex(CookieValue(req, kCookie));
        if (token.size() != 32) return std::nullopt;
        const auto found = OnPool<std::optional<Session>>(Pool(), [hash = Sha256(token)](Database& db) -> std::optional<Session> {
            const QueryResult r = db.Query("UPDATE sro_vt_account.sr_player_sessions s SET seen_at = now() FROM sro_vt_account.tb_user u "
                                           "WHERE s.token_hash = $1 AND u.jid = s.jid AND s.expires_at > now() RETURNING u.jid, u.struserid, s.csrf",
                                           {Param::Bytes(hash)});
            if (!r.Ok() || r.Rows() != 1) return std::nullopt;
            return Session{r.Int(0, 0), r.Text(0, 1), r.Text(0, 2)};
        });
        return found ? *found : std::nullopt;
    }
    // The player of a change (its origin and second token checked), else the answer refusing it.
    std::optional<Session> Guard(const httplib::Request& req, httplib::Response& res) {
        auto who = Find(req);
        if (!who) {
            Refuse(res, 401, "login", "Please log in.");
            return std::nullopt;
        }
        if (!SameOrigin(req, Config().origins) || !ConstantTimeEqual(req.get_header_value("X-CSRF-Token"), who->csrf)) {
            Refuse(res, 403, "csrf", "The page's session token is missing; reload the page.");
            return std::nullopt;
        }
        return who;
    }
    std::string CookieHeader(const std::string& value, int maxAge) const {
        return std::format("{}={}; Path=/api; HttpOnly; SameSite=Lax; Max-Age={}{}", kCookie, value, maxAge, Config().secureCookie ? "; Secure" : "");
    }

    void Login(const httplib::Request& req, httplib::Response& res);
    // The account's current password, for a change (the login's limits apply); false: the refusal is written.
    bool CheckCurrent(const httplib::Request& req, httplib::Response& res, const Session& who, const std::string& current);

    AdminHost host_;
    std::mutex limiterMutex_;
    AttemptLimiter addressLimiter_, accountLimiter_;
    AttemptLimiter supportLimiter_;

    void SupportTicket(const httplib::Request& req, httplib::Response& res);
    void SupportFile(const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader);
};

void WebPlayers::SupportTicket(const httplib::Request& req, httplib::Response& res) {
    const auto body = Body(req, res);
    if (!body) return;
    const std::string address = host_.visitor(req);
    // A field people never see: a robot fills it in, and is thanked without a ticket.
    if (!Field(*body, "website").empty()) return Answer(res, 201, {{"id", 0}, {"uploadToken", ""}});
    NewTicket t;
    t.kind = Field(*body, "kind");
    t.category = Field(*body, "category");
    t.subject = Field(*body, "subject");
    t.body = Field(*body, "text");
    t.name = Field(*body, "name");
    t.email = Field(*body, "email");
    t.clientInfo = Field(*body, "clientInfo");
    t.source = Field(*body, "source").empty() ? std::string("site") : Field(*body, "source");
    t.address = address;
    if (const auto who = Find(req)) {
        t.jid = who->jid;
        if (t.name.empty()) t.name = who->account;
    }
    if (const auto why = CheckTicket(t)) return Refuse(res, 400, "invalid", *why);
    if (Config().supportAttempts > 0) {
        const std::lock_guard lock(limiterMutex_);
        if (!supportLimiter_.Allow(address)) return Refuse(res, 429, "too-many", "Too many messages from here; try again in an hour.");
    }
    const auto made = OnPool<std::optional<std::pair<std::int64_t, std::string>>>(Pool(), [t](Database& db) { return CreateTicket(db, t); });
    if (!made || !*made) return Refuse(res, 503, "busy", "The message could not be saved; try again.");
    core::LogInfo("SroWeb: support ticket #{} ({}, {}) from {}", (*made)->first, t.kind, t.category, address);
    Answer(res, 201, {{"id", (*made)->first}, {"uploadToken", (*made)->second}});
}

void WebPlayers::SupportFile(const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) {
    namespace fs = std::filesystem;
    const std::int64_t id = std::atoll(req.matches[1].str().c_str());
    const std::vector<std::uint8_t> token = FromHex(req.get_param_value("token"));
    std::string original = fs::path(req.get_param_value("name")).filename().string().substr(0, 200);
    if (original.empty()) original = "file";
    const std::string lengthHeader = req.get_header_value("Content-Length");
    const std::int64_t length = lengthHeader.empty() ? -1 : std::atoll(lengthHeader.c_str());
    if (length <= 0) return Refuse(res, 411, "length", "The upload must say its size.");
    if (length > kSupportVideoMax) return Refuse(res, 413, "too-big", std::format("A file is up to {} MB.", kSupportVideoMax >> 20));
    if (token.size() != 32) return Refuse(res, 403, "token", "This upload is not allowed.");
    // The ticket's token, still good, and room for one more.
    const auto room = OnPool<int>(Pool(), [id, hash = Sha256(token)](Database& db) {
        const QueryResult r = db.Query(std::format("SELECT (SELECT count(*) FROM sro_vt_account.sr_support_files f WHERE f.ticket_id = t.id) "
                                                   "FROM sro_vt_account.sr_support_tickets t WHERE t.id = $1 AND t.upload_token = $2 "
                                                   "AND t.created_at > now() - make_interval(hours => {})",
                                                   kSupportUploadHours),
                                       {id, Param::Bytes(hash)});
        if (!r.Ok() || r.Rows() != 1) return -1;
        return kSupportFilesMax - int(r.Int(0, 0));
    });
    if (!room) return Refuse(res, 503, "busy", "The server is busy; try again.");
    if (*room < 0) return Refuse(res, 403, "token", "This upload is not allowed (or its time is over).");
    if (*room == 0) return Refuse(res, 409, "full", std::format("A ticket takes {} files.", kSupportFilesMax));

    std::error_code ec;
    const fs::path dir = fs::absolute(Config().supportDir) / std::to_string(id);
    fs::create_directories(dir, ec);
    const std::string random = Hex(RandomBytes(16));
    const fs::path part = dir / (random + ".part");
    std::ofstream out(part, std::ios::binary);
    if (!out) return Refuse(res, 500, "disk", "The upload folder cannot be written.");
    Sha256Stream hash;
    std::int64_t got = 0;
    std::string head;
    const bool read = reader([&](const char* data, std::size_t size) {
        got += std::int64_t(size);
        if (got > length) return false;
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
    const std::string mime = SupportFileType(head);
    if (mime.empty()) return fail(400, "type", "Screenshots (JPEG, PNG, WebP) and videos (MP4, WebM) only.");
    if (mime.starts_with("image/") && got > kSupportImageMax) return fail(413, "too-big", std::format("A screenshot is up to {} MB.", kSupportImageMax >> 20));
    const std::string stored = std::to_string(id) + "/" + random + SupportFileExtension(mime);
    fs::rename(part, fs::absolute(Config().supportDir) / stored, ec);
    if (ec) return fail(500, "disk", "The file could not be stored.");
    const std::string sha = Hex(hash.Finish());
    const auto row = OnPool<std::int64_t>(Pool(), [id, stored, original, mime, got, sha](Database& db) -> std::int64_t {
        // Counted again inside the insert: two uploads at once cannot pass the limit.
        const QueryResult r = db.Query(std::format("INSERT INTO sro_vt_account.sr_support_files (ticket_id, stored, original, mime, bytes, sha256) "
                                                   "SELECT $1, $2, $3, $4, $5, $6 WHERE (SELECT count(*) FROM sro_vt_account.sr_support_files "
                                                   "WHERE ticket_id = $1) < {} RETURNING id",
                                                   kSupportFilesMax),
                                       {id, stored, original, mime, got, sha});
        return r.Ok() && r.Rows() == 1 ? r.Int(0, 0) : 0;
    });
    if (!row || *row == 0) {
        fs::remove(fs::absolute(Config().supportDir) / stored, ec);
        return Refuse(res, 409, "full", std::format("A ticket takes {} files.", kSupportFilesMax));
    }
    Answer(res, 201, {{"id", *row}, {"mime", mime}, {"bytes", got}});
}

bool WebPlayers::CheckCurrent(const httplib::Request& req, httplib::Response& res, const Session& who, const std::string& current) {
    {
        const std::lock_guard lock(limiterMutex_);
        if (!addressLimiter_.Allow(host_.visitor(req)) || !accountLimiter_.Allow(Lower(who.account))) {
            Refuse(res, 429, "too-many", "Too many tries; wait 15 minutes.");
            return false;
        }
    }
    const auto right = OnPool<bool>(Pool(), [name = who.account, current](Database& db) {
        bool ok = false;
        const auto account = FindAccount(db, name, ok);
        if (!account || current.empty() || current.size() > 64) return false;
        if (!account->passwordHash.empty()) return VerifyPassword(current, account->passwordHash);
        return !account->legacyHash.empty() && VerifyLegacyPassword(current, account->legacyHash);
    });
    if (!right) {
        Refuse(res, 503, "busy", "The server is busy; try again.");
        return false;
    }
    if (!*right) {
        Refuse(res, 403, "wrong", "The current password is wrong.");
        return false;
    }
    return true;
}

void WebPlayers::Login(const httplib::Request& req, httplib::Response& res) {
    const auto body = Body(req, res);
    if (!body) return;
    const std::string name = Field(*body, "name"), password = Field(*body, "password");
    const std::string address = host_.visitor(req);
    if (!ValidAccountName(name) || password.empty() || password.size() > 64) return Refuse(res, 401, "wrong", "The account name or password is wrong.");
    {
        const std::lock_guard lock(limiterMutex_);
        if (!addressLimiter_.Allow(address) || !accountLimiter_.Allow(Lower(name)))
            return Refuse(res, 429, "too-many", "Too many tries; wait 15 minutes.");
    }
    struct Outcome {
        int status = 401;
        std::string error = "wrong", message = "The account name or password is wrong.";
        std::string account, token, csrf;
        std::vector<Character> characters;
    };
    const auto outcome = OnPool<Outcome>(Pool(), [name, password, address, legacy = Config().legacyPasswords](Database& db) {
        Outcome o;
        bool ok = false;
        const auto account = FindAccount(db, name, ok);
        if (!account) {
            VerifyPassword(password, "$argon2id$v=19$m=19456,t=2,p=1$c29tZXNhbHRzb21lc2FsdA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"); // the same time
            return o;
        }
        // As the login server: ours, else the original's (then ours is stored for it).
        bool right = !account->passwordHash.empty() ? VerifyPassword(password, account->passwordHash) : false;
        if (!right && account->passwordHash.empty() && !account->legacyHash.empty() && VerifyLegacyPassword(password, account->legacyHash)) {
            right = true;
            StorePasswordHash(db, account->id, HashPassword(password), legacy);
        }
        if (!right) return o;
        if (account->blocked) {
            o.status = 403;
            o.error = "blocked";
            o.message = BlockedWords(*account);
            return o;
        }
        const std::vector<std::uint8_t> token = RandomBytes(32), csrf = RandomBytes(32);
        db.Query("DELETE FROM sro_vt_account.sr_player_sessions WHERE expires_at < now()");
        const QueryResult s = db.Query(std::format("INSERT INTO sro_vt_account.sr_player_sessions (token_hash, jid, csrf, address, expires_at) "
                                                   "VALUES ($1, $2, $3, $4, now() + make_interval(days => {}))",
                                                   kSessionDays),
                                       {Param::Bytes(Sha256(token)), account->id, Hex(csrf), address});
        if (!s.Ok() || token.size() != 32) {
            o.status = 500;
            o.error = "server";
            o.message = "Something went wrong; try again later.";
            return o;
        }
        o.status = 200;
        o.account = account->name;
        o.token = Hex(token);
        o.csrf = Hex(csrf);
        o.characters = CharactersOf(db, account->id);
        return o;
    });
    if (!outcome) return Refuse(res, 503, "busy", "The server is busy; try again.");
    if (outcome->status != 200) return Refuse(res, outcome->status, outcome->error.c_str(), outcome->message);
    core::LogInfo("SroWeb: player {} logged in from {}", outcome->account, address);
    res.set_header("Set-Cookie", CookieHeader(outcome->token, kSessionDays * 86400));
    Answer(res, 200, {{"name", outcome->account}, {"csrf", outcome->csrf}, {"characters", CharactersJson(outcome->characters)}});
}

void WebPlayers::Register(httplib::Server& http) {
    http.Post("/api/account/login", [this](const httplib::Request& req, httplib::Response& res) { Login(req, res); });
    http.Get("/api/account/me", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Find(req);
        if (!who) return Refuse(res, 401, "login", "Please log in.");
        const auto characters = OnPool<std::vector<Character>>(Pool(), [jid = who->jid](Database& db) { return CharactersOf(db, jid); });
        Answer(res, 200, {{"name", who->account}, {"csrf", who->csrf}, {"characters", CharactersJson(characters ? *characters : std::vector<Character>{})}});
    });
    // The player's own page: the account and its characters.
    http.Get("/api/account/profile", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Find(req);
        if (!who) return Refuse(res, 401, "login", "Please log in.");
        const auto profile = OnPool<json>(Pool(), [jid = who->jid](Database& db) { return ProfileJson(db, jid); });
        if (!profile || profile->is_null()) return Refuse(res, 503, "busy", "The server is busy; try again.");
        res.set_header("Cache-Control", "no-store");
        Answer(res, 200, *profile);
    });
    // A new password: the current one first (as a login: the same limits). Every other session of the account ends.
    http.Post("/api/account/password", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res);
        if (!who) return;
        const auto body = Body(req, res);
        if (!body) return;
        const std::string current = Field(*body, "current"), password = Field(*body, "password");
        if (!ValidPassword(password)) return Refuse(res, 400, "password", "Passwords are 6 to 64 characters.");
        if (!CheckCurrent(req, res, *who, current)) return;
        const auto ok = OnPool<bool>(Pool(), [jid = who->jid, password, legacy = Config().legacyPasswords, keep = Sha256(FromHex(CookieValue(req, kCookie)))](Database& db) {
            if (!StorePasswordHash(db, jid, HashPassword(password), LegacyPasswords::Clear)) return false;
            if (legacy == LegacyPasswords::Keep) db.Query("UPDATE sro_vt_account.tb_user SET password = $2 WHERE jid = $1", {jid, LegacyPasswordHash(password)});
            db.Query("DELETE FROM sro_vt_account.sr_player_sessions WHERE jid = $1 AND token_hash <> $2", {jid, Param::Bytes(keep)});
            return true;
        });
        if (!ok || !*ok) return Refuse(res, 503, "busy", "The server is busy; try again.");
        core::LogInfo("SroWeb: player {} changed the password from {}", who->account, host_.visitor(req));
        Answer(res, 200, {{"ok", true}});
    });
    // A new e-mail address (or none): the password first.
    http.Post("/api/account/email", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res);
        if (!who) return;
        const auto body = Body(req, res);
        if (!body) return;
        const std::string current = Field(*body, "current"), email = Field(*body, "email");
        if (!email.empty() && !ValidEmail(email)) return Refuse(res, 400, "email", "That e-mail address does not look right.");
        if (!CheckCurrent(req, res, *who, current)) return;
        const auto ok = OnPool<bool>(Pool(), [jid = who->jid, email](Database& db) {
            return db.Query("UPDATE sro_vt_account.tb_user SET email = nullif($2, '') WHERE jid = $1", {jid, email}).Ok();
        });
        if (!ok || !*ok) return Refuse(res, 503, "busy", "The server is busy; try again.");
        core::LogInfo("SroWeb: player {} changed the e-mail address", who->account);
        Answer(res, 200, {{"ok", true}});
    });
    // Support: a ticket, then its files (WebSupport.h); a signed-in player's own tickets and its answers.
    http.Post("/api/support", [this](const httplib::Request& req, httplib::Response& res) { SupportTicket(req, res); });
    http.Post(R"(/api/support/(\d+)/files)", [this](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) {
        SupportFile(req, res, reader);
    });
    http.Get("/api/account/tickets", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Find(req);
        if (!who) return Refuse(res, 401, "login", "Please log in.");
        const auto list = OnPool<json>(Pool(), [jid = who->jid](Database& db) { return PlayerTickets(db, jid); });
        if (!list) return Refuse(res, 503, "busy", "The server is busy; try again.");
        res.set_header("Cache-Control", "no-store");
        Answer(res, 200, {{"tickets", *list}});
    });
    http.Post("/api/account/tickets/:id/reply", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res);
        if (!who) return;
        const auto body = Body(req, res);
        if (!body) return;
        std::string text = Field(*body, "text");
        text.erase(0, text.find_first_not_of(" \t\r\n"));
        text.erase(text.find_last_not_of(" \t\r\n") + 1);
        if (text.empty() || text.size() > 5000) return Refuse(res, 400, "text", "The answer is 1 to 5000 characters.");
        const std::int64_t id = std::atoll(req.path_params.at("id").c_str());
        const auto ok = OnPool<bool>(Pool(), [jid = who->jid, id, name = who->account, text](Database& db) { return PlayerReply(db, jid, id, name, text); });
        if (!ok) return Refuse(res, 503, "busy", "The server is busy; try again.");
        if (!*ok) return Refuse(res, 404, "ticket", "No such open ticket of yours.");
        Answer(res, 200, {{"ok", true}});
    });
    http.Post("/api/account/logout", [this](const httplib::Request& req, httplib::Response& res) {
        if (!Guard(req, res)) return;
        OnPool<bool>(Pool(), [hash = Sha256(FromHex(CookieValue(req, kCookie)))](Database& db) {
            return db.Query("DELETE FROM sro_vt_account.sr_player_sessions WHERE token_hash = $1", {Param::Bytes(hash)}).Ok();
        });
        res.set_header("Set-Cookie", CookieHeader("", 0));
        Answer(res, 200, {{"ok", true}});
    });

    // A news' comments: anyone reads them; a player writes, signed as one of its characters.
    http.Get("/api/news/:id/comments", [this](const httplib::Request& req, httplib::Response& res) {
        const std::int64_t news = std::atoll(req.path_params.at("id").c_str());
        const auto who = Find(req);
        const auto list = OnPool<json>(Pool(), [news, jid = who ? who->jid : 0](Database& db) {
            json a = json::array();
            const QueryResult r = db.Query("SELECT id, author, body, floor(extract(epoch FROM created_at))::bigint, jid FROM sro_vt_account.sr_site_comments "
                                           "WHERE news_id = $1 AND NOT removed ORDER BY id LIMIT 500",
                                           {news});
            for (int i = 0; r.Ok() && i < r.Rows(); ++i)
                a.push_back({{"id", r.Int(i, 0)}, {"author", r.Text(i, 1)}, {"text", r.Text(i, 2)}, {"at", r.Int(i, 3)}, {"mine", jid != 0 && r.Int(i, 4) == jid}});
            return a;
        });
        if (!list) return Refuse(res, 503, "busy", "The server is busy; try again.");
        res.set_header("Cache-Control", "no-cache");
        Answer(res, 200, {{"comments", *list}});
    });
    http.Post("/api/news/:id/comments", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res);
        if (!who) return;
        const auto body = Body(req, res);
        if (!body) return;
        const std::int64_t news = std::atoll(req.path_params.at("id").c_str());
        std::string text = Field(*body, "text");
        const std::string character = Field(*body, "character");
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
        text.erase(0, text.find_first_not_of(" \t\r\n"));
        if (text.empty() || text.size() > kCommentMax) return Refuse(res, 400, "text", "A comment is 1 to 1000 characters.");
        struct Outcome {
            int status = 500;
            std::string error = "server", message = "Something went wrong.";
            std::int64_t id = 0;
        };
        const auto outcome = OnPool<Outcome>(Pool(), [who = *who, news, text, character, address = host_.visitor(req)](Database& db) {
            Outcome o;
            bool ok = false;
            const auto account = FindAccount(db, who.account, ok);
            if (!account || account->blocked) return Outcome{403, "blocked", account ? BlockedWords(*account) : "No such account.", 0};
            const QueryResult n = db.Query("SELECT 1 FROM sro_vt_account.sr_site_news WHERE id = $1 AND visible", {news});
            if (!n.Ok() || n.Rows() != 1) return Outcome{404, "news", "No such news.", 0};
            std::string author;
            for (const Character& c : CharactersOf(db, who.jid))
                if (Lower(c.name) == Lower(character)) author = c.name;
            if (author.empty()) return Outcome{400, "character", "Sign it as one of your characters.", 0};
            const QueryResult last = db.Query(std::format("SELECT 1 FROM sro_vt_account.sr_site_comments WHERE jid = $1 AND created_at > now() - interval '{} seconds'",
                                                          kCommentGapSeconds),
                                              {who.jid});
            if (last.Ok() && last.Rows() > 0) return Outcome{429, "too-fast", "One comment every 20 seconds.", 0};
            const QueryResult r = db.Query("INSERT INTO sro_vt_account.sr_site_comments (news_id, jid, author, body, address) VALUES ($1, $2, $3, $4, $5) RETURNING id",
                                           {news, who.jid, author, text, address});
            if (!r.Ok()) return o;
            return Outcome{201, "", "", r.Int(0, 0)};
        });
        if (!outcome) return Refuse(res, 503, "busy", "The server is busy; try again.");
        if (outcome->status != 201) return Refuse(res, outcome->status, outcome->error.c_str(), outcome->message);
        Answer(res, 201, {{"id", outcome->id}});
    });
    http.Delete("/api/comments/:id", [this](const httplib::Request& req, httplib::Response& res) {
        const auto who = Guard(req, res);
        if (!who) return;
        const std::int64_t id = std::atoll(req.path_params.at("id").c_str());
        const auto ok = OnPool<bool>(Pool(), [id, jid = who->jid](Database& db) {
            const QueryResult r = db.Query("UPDATE sro_vt_account.sr_site_comments SET removed = true, removed_by = 'its author' WHERE id = $1 AND jid = $2 AND NOT removed",
                                           {id, jid});
            return r.Ok() && r.Affected() == 1;
        });
        if (!ok || !*ok) return Refuse(res, 404, "comment", "No such comment of yours.");
        Answer(res, 200, {{"ok", true}});
    });
}

std::shared_ptr<WebPlayers> RegisterPlayers(httplib::Server& http, const AdminHost& host) {
    auto players = std::make_shared<WebPlayers>(host);
    players->Register(http);
    return players;
}

} // namespace sr::server
