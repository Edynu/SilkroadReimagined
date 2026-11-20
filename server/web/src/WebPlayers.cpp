#include "WebPlayers.h"

#include <format>
#include <mutex>
#include <unordered_map>

#include "sr/core/Log.h"
#include "sr/server/Crypto.h"
#include "sr/server/Service.h"
#include "sr/server/Store.h"
#include "sr/server/WebServer.h"
#include "WebCommon.h"

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
        : host_(host), addressLimiter_(10, std::chrono::minutes(15)), accountLimiter_(10, std::chrono::minutes(15)) {}

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

    AdminHost host_;
    std::mutex limiterMutex_;
    AttemptLimiter addressLimiter_, accountLimiter_;
};

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
