#include "sr/server/WebServer.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <format>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "sr/core/Log.h"
#include "sr/server/Database.h"
#include "sr/server/Service.h"
#include "sr/server/WebAdmin.h"
#include "WebPlayers.h"

namespace sr::server {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

std::int64_t UnixNow() { return std::int64_t(std::time(nullptr)); }

// A JSON answer kept for `age`: the first request that finds it older asks the database pool for a new
// one and is answered with the old one meanwhile; only the very first waits (a little) for an answer.
// Refresh asks at once (the bus said it changed): once more after the question on its way, if there is one.
class Cached {
public:
    using Make = std::function<std::optional<std::string>(Database&)>;
    Cached(DatabasePool& pool, std::chrono::seconds age, Make make) : pool_(pool), age_(age), make_(std::move(make)) {}

    std::string Get(std::chrono::milliseconds wait) {
        std::unique_lock lock(mutex_);
        const auto old = Clock::now() - at_;
        if (!asking_ && (body_.empty() || old >= age_)) Ask();
        // The first answer, or one long out of date (nobody asked for a while): worth a moment's wait.
        const bool stale = !body_.empty() && old >= age_ * 3 + std::chrono::seconds(2);
        if ((body_.empty() || stale) && wait.count() > 0) changed_.wait_for(lock, wait, [&] { return !asking_; });
        return body_;
    }
    void Refresh() {
        const std::lock_guard lock(mutex_);
        if (asking_) {
            again_ = true; // what is being read may predate the change
        } else {
            Ask();
        }
    }

private:
    void Ask() { // under mutex_
        asking_ = true;
        pool_.Submit([this](Database& db) {
            std::optional<std::string> made = make_(db);
            {
                const std::lock_guard lock(mutex_);
                asking_ = false;
                if (made) {
                    body_ = std::move(*made);
                    at_ = Clock::now();
                } else {
                    core::LogWarning("SroWeb: a query for the site failed; the last answer stays");
                }
                if (again_) {
                    again_ = false;
                    Ask();
                }
            }
            changed_.notify_all();
        });
    }

    DatabasePool& pool_;
    std::chrono::seconds age_;
    Make make_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string body_;
    Clock::time_point at_{};
    bool asking_ = false, again_ = false;
};

// The shards as the site shows them: only game servers that report count, so one that stopped without
// saying so drops out of the numbers after `staleSeconds`.
std::optional<std::string> StatusJson(Database& db, const WebConfig& config) {
    bool ok = false;
    const std::vector<GameServerInfo> servers = ListGameServers(db, config.staleSeconds, ok);
    if (!ok) return std::nullopt;
    json shards = json::array();
    std::uint32_t players = 0, capacity = 0;
    int up = 0;
    std::vector<std::string> schemas;
    for (const GameServerInfo& s : servers)
        if (std::find(schemas.begin(), schemas.end(), s.schema) == schemas.end()) schemas.push_back(s.schema);
    for (const std::string& schema : schemas) {
        std::uint32_t shardPlayers = 0, shardCapacity = 0, reportedCapacity = 0;
        int shardUp = 0, total = 0;
        std::string name;
        for (const GameServerInfo& s : servers) {
            if (s.schema != schema) continue;
            if (name.empty()) name = s.name; // its lowest-numbered server's, as the server list's (ShardsOf)
            ++total;
            if (!s.reporting) continue;
            ++shardUp;
            shardPlayers += s.online;
            shardCapacity += s.capacity;
            reportedCapacity = std::max(reportedCapacity, s.shardCapacity);
        }
        // The shard's own capacity (all its game servers together: each carries the whole shard), where they report it.
        if (reportedCapacity > 0) shardCapacity = reportedCapacity;
        if (schemas.size() == 1 && !config.shardName.empty()) name = config.shardName;
        // Its fortresses' owners and tax rates (the original fed its site with `_SiegeFortressStatusUpdateForWeb`).
        json fortresses = json::array();
        if (db.Query("SELECT to_regclass($1) IS NOT NULL", {schema + "._siegefortress"}).Bool(0, 0)) {
            const QueryResult f = db.Query(std::format("SELECT f.fortressid, coalesce(r.codename128, ''), coalesce(g.name, ''), f.taxratio FROM {0}._siegefortress f "
                                                       "LEFT JOIN {0}._guild g ON g.id = f.guildid AND f.guildid <> 0 "
                                                       "LEFT JOIN {0}._refsiegefortress r ON r.fortressid = f.fortressid ORDER BY f.fortressid",
                                                       schema));
            for (int i = 0; f.Ok() && i < f.Rows(); ++i) {
                std::string code = f.Text(i, 1);
                while (!code.empty() && code.back() == ' ') code.pop_back();
                fortresses.push_back({{"id", f.Int(i, 0)}, {"codename", code}, {"owner", f.Text(i, 2)}, {"taxRate", f.Int(i, 3)}});
            }
        }
        shards.push_back({{"name", name}, {"up", shardUp > 0}, {"players", shardPlayers}, {"capacity", shardCapacity},
                          {"gameServers", shardUp}, {"gameServersTotal", total}, {"fortresses", fortresses}});
        players += shardPlayers;
        capacity += shardCapacity;
        up += shardUp;
    }
    json each = json::array();
    for (const GameServerInfo& s : servers)
        each.push_back({{"id", s.id}, {"up", s.reporting}, {"players", s.reporting ? s.online : 0u}, {"capacity", s.capacity}, {"startedAt", s.reporting ? s.startedAt : 0}});
    const json out = {{"online", up > 0}, {"players", players}, {"capacity", capacity}, {"gameServers", up}, {"shards", shards}, {"servers", each}, {"updated", UnixNow()}};
    return out.dump();
}

std::optional<std::string> RankingsJson(Database& db, int count) {
    bool ok = false;
    const Rankings r = LoadRankings(db, count, ok);
    if (!ok) return std::nullopt;
    const auto rows = [](const std::vector<RankingRow>& list) {
        json a = json::array();
        for (const RankingRow& row : list) a.push_back({{"name", row.name}, {"value", row.value}, {"detail", row.detail}});
        return a;
    };
    const json out = {{"players", rows(r.players)}, {"guilds", rows(r.guilds)}, {"jobs", rows(r.jobs)}, {"uniqueKills", rows(r.uniqueKills)}, {"updated", UnixNow()}};
    return out.dump();
}

std::optional<std::string> StatsJson(Database& db) {
    bool ok = false;
    const ShardStats s = LoadShardStats(db, ok);
    if (!ok) return std::nullopt;
    const json out = {{"accounts", s.accounts}, {"characters", s.characters}, {"guilds", s.guilds}, {"uniquesKilled", s.uniquesKilled},
                      {"uniquesKilledToday", s.uniquesKilledToday}, {"updated", UnixNow()}};
    return out.dump();
}

std::optional<std::string> UniquesJson(Database& db) {
    bool ok = false;
    const std::vector<UniqueState> list = LoadUniques(db, ok);
    if (!ok) return std::nullopt;
    json a = json::array();
    for (const UniqueState& u : list)
        a.push_back({{"name", u.name}, {"alive", u.alive}, {"since", u.since}, {"killer", u.killer}, {"killedAt", u.killedAt}});
    const json out = {{"uniques", a}, {"updated", UnixNow()}};
    return out.dump();
}

std::string ServerJson(const WebConfig& c) {
    const json out = {{"name", c.shardName},
                      {"levelCap", c.levelCap},
                      {"rates", {{"exp", c.rateExp}, {"sp", c.rateSp}, {"gold", c.rateGold}, {"drop", c.rateDrop}}},
                      {"registration", {{"open", c.registration}, {"betaKey", c.betaKeys}}}};
    return out.dump();
}

bool IsLoopback(const std::string& address) { return address == "127.0.0.1" || address == "::1" || address == "::ffff:127.0.0.1"; }

void Answer(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void Refuse(httplib::Response& res, int status, const char* code, const char* message) {
    Answer(res, status, {{"error", code}, {"message", message}});
}

} // namespace

// The game servers' load (sr_server_samples, every 30 s each): the last hour by 30 s, the last 6 hours by 3 minutes, the
// last day by 10 minutes; each bucket's average tick and slowest tick (ms), the ticks over the budget, the players online
// and the monsters alive (their averages), by game server.
std::optional<std::string> LoadJson(Database& db) {
    json ranges = json::object();
    double budget = 0;
    for (const auto& [name, step, span] : {std::tuple{"1h", 30, "1 hour"}, {"6h", 180, "6 hours"}, {"24h", 600, "24 hours"}}) {
        const QueryResult r = db.Query(
            "SELECT shard_id, extract(epoch FROM date_bin(make_interval(secs => $1), at, TIMESTAMPTZ '2000-01-01'))::bigint AS t, "
            "avg(tick_avg_ms), max(tick_max_ms), sum(over_budget), round(avg(players)), round(avg(monsters)), max(budget_ms) "
            "FROM sro_vt_account.sr_server_samples WHERE at > now() - $2::interval GROUP BY 1, 2 ORDER BY 1, 2",
            {step, span});
        if (!r.Ok()) return std::nullopt;
        json servers = json::object();
        for (int i = 0; i < r.Rows(); ++i) {
            const std::string id = std::to_string(r.Int(i, 0));
            if (!servers.contains(id)) servers[id] = json::array();
            const auto round2 = [](double v) { return std::round(v * 100.0) / 100.0; };
            servers[id].push_back({r.Int(i, 1), round2(r.Double(i, 2)), round2(r.Double(i, 3)), r.Int(i, 4), r.Int(i, 5), r.Int(i, 6)});
            budget = std::max(budget, r.Double(i, 7));
        }
        ranges[name] = {{"stepSeconds", step}, {"servers", std::move(servers)}};
    }
    return json{{"budgetMs", budget}, {"now", UnixNow()}, {"columns", {"t", "tickAvgMs", "tickMaxMs", "overBudget", "players", "monsters"}}, {"ranges", ranges}}
        .dump();
}

struct WebServer::State {
    explicit State(const WebConfig& c)
        : config(c), limiter(std::max(1, c.registerAttempts), c.registerWindow),
          status(pool, c.statusAge, [this](Database& db) { return StatusJson(db, config); }),
          rankings(pool, c.rankingAge, [this](Database& db) { return RankingsJson(db, config.rankingCount); }),
          uniques(pool, c.statusAge, [](Database& db) { return UniquesJson(db); }),
          site(pool, std::chrono::seconds(60), [](Database& db) { return SiteJson(db); }),
          stats(pool, std::chrono::seconds(60), [](Database& db) { return StatsJson(db); }),
          load(pool, std::chrono::seconds(30), [](Database& db) { return LoadJson(db); }) {}

    WebConfig config;
    std::string serverJson;
    DatabasePool pool;
    httplib::Server http;
    std::thread listener;
    std::uint16_t port = 0;
    std::mutex limiterMutex;
    AttemptLimiter limiter;
    Cached status, rankings, uniques;
    Cached site; // the texts, news, images and downloads (read again at once when the administration changes them)
    Cached stats; // accounts, characters, guilds, uniques killed
    Cached load;  // the game servers' ticks, players and monsters over time (the server page's chart)
    std::shared_ptr<WebAdmin> admin;
    std::shared_ptr<WebPlayers> players;
    // The shard's bus: its game servers say when a unique came or fell (sr_uniques_<schema>).
    std::unique_ptr<Database> bus;

    // The visitor's address: the peer's, or behind a trusted local proxy the last X-Forwarded-For entry
    // (the address the proxy itself saw).
    std::string Visitor(const httplib::Request& req) const {
        if (config.trustProxy && IsLoopback(req.remote_addr)) {
            const std::string forwarded = req.get_header_value("X-Forwarded-For");
            if (!forwarded.empty()) {
                std::string last = forwarded.substr(forwarded.rfind(',') == std::string::npos ? 0 : forwarded.rfind(',') + 1);
                last.erase(0, last.find_first_not_of(' '));
                last.erase(last.find_last_not_of(' ') + 1);
                if (!last.empty()) return last;
            }
        }
        return req.remote_addr;
    }

    void Register(const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        if (!config.registration) return Refuse(res, 403, "closed", "Registration is closed.");
        // Only JSON: a form on another site cannot send it without the browser asking first.
        if (req.get_header_value("Content-Type").rfind("application/json", 0) != 0)
            return Refuse(res, 415, "bad-request", "The request must be JSON.");
        const json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) return Refuse(res, 400, "bad-request", "The request is not valid JSON.");
        const auto field = [&](const char* key) {
            const auto it = body.find(key);
            return it != body.end() && it->is_string() ? it->get<std::string>() : std::string();
        };
        const std::string name = field("name"), password = field("password"), email = field("email"), key = field("key");
        if (!ValidAccountName(name)) return Refuse(res, 400, "invalid-name", "Account names are 3 to 25 letters, digits or _.");
        if (!ValidPassword(password)) return Refuse(res, 400, "invalid-password", "Passwords are 6 to 64 characters.");
        if (!email.empty() && !ValidEmail(email)) return Refuse(res, 400, "invalid-email", "That e-mail address does not look right.");
        if (config.betaKeys && key.empty()) return Refuse(res, 403, "key-required", "Registration needs a beta key.");
        const std::string address = Visitor(req);
        if (config.registerAttempts > 0) {
            const std::lock_guard lock(limiterMutex);
            if (!limiter.Allow(address)) return Refuse(res, 429, "too-many", "Too many accounts from this address; try again later.");
        }

        NewAccountDetails details;
        details.email = email;
        details.address = address;
        if (config.betaKeys) details.betaKeyHash = BetaKeyHash(key);
        struct Outcome {
            CreateAccountResult result = CreateAccountResult::Failed;
            std::string error;
        };
        auto promise = std::make_shared<std::promise<Outcome>>();
        std::future<Outcome> future = promise->get_future();
        pool.Submit([promise, name, password, details, legacy = config.legacyPasswords](Database& db) {
            Outcome o;
            o.result = CreateAccount(db, name, password, o.error, legacy, details);
            promise->set_value(std::move(o));
        });
        if (future.wait_for(20s) != std::future_status::ready) return Refuse(res, 503, "busy", "The server is busy; try again in a moment.");
        const Outcome o = future.get();
        switch (o.result) {
        case CreateAccountResult::Created:
            core::LogInfo("SroWeb: account {} made from {}", name, address);
            return Answer(res, 201, {{"ok", true}, {"name", name}});
        case CreateAccountResult::NameTaken: return Refuse(res, 409, "name-taken", "That account name is taken.");
        case CreateAccountResult::InvalidName: return Refuse(res, 400, "invalid-name", "Account names are 3 to 25 letters, digits or _.");
        case CreateAccountResult::InvalidPassword: return Refuse(res, 400, "invalid-password", "Passwords are 6 to 64 characters.");
        case CreateAccountResult::KeyRefused: return Refuse(res, 403, "key-refused", "That beta key is not valid or was used already.");
        case CreateAccountResult::Failed: break;
        }
        core::LogError("SroWeb: registration of {} failed: {}", name, o.error);
        return Refuse(res, 500, "server", "Something went wrong; try again later.");
    }
};

WebServer::WebServer() = default;
WebServer::~WebServer() { Stop(); }

bool WebServer::Start(const WebConfig& config, std::string& error) {
    Stop();
    s_ = std::make_unique<State>(config);
    State& s = *s_;
    s.serverJson = ServerJson(config);
    // Its shard's tables first in the search path, as its game servers', and its bus.
    const std::string url = WithSearchPath(config.databaseUrl, config.shardSchema + ", " + kAccountSchema);
    if (!s.pool.Start(url, 2, error)) {
        s_.reset();
        return false;
    }
    s.bus = Database::Connect(url, error);
    if (!s.bus || !s.bus->Listen("sr_uniques_" + config.shardSchema)) {
        error = "the shard's bus: " + error;
        s.pool.Stop();
        s_.reset();
        return false;
    }

    httplib::Server& http = s.http;
    const std::size_t threads = std::size_t(std::max(1, config.threads));
    http.new_task_queue = [threads] { return new httplib::ThreadPool(threads); };
    // Only an admin's upload may be large (WebAdmin's pre-routing keeps every other body under 64 KB).
    http.set_payload_max_length(config.admin ? std::size_t(std::max(config.uploadMaxBytes, config.imageMaxBytes)) : 16 * 1024);
    http.set_read_timeout(10s);
    http.set_write_timeout(10s);
    // The page loads only its own scripts, styles, fonts and images, and is never framed.
    http.set_default_headers({{"X-Content-Type-Options", "nosniff"},
                              {"Referrer-Policy", "same-origin"},
                              {"X-Frame-Options", "DENY"},
                              {"Content-Security-Policy", "default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; script-src 'self'; "
                                                          "connect-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri 'self'; form-action 'self'"}});
    http.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
        Refuse(res, 500, "server", "Something went wrong.");
    });

    const auto cached = [](Cached& cache) {
        return [&cache](const httplib::Request&, httplib::Response& res) {
            const std::string body = cache.Get(3s);
            if (body.empty()) return Refuse(res, 503, "unavailable", "The game's data cannot be read right now.");
            res.set_header("Cache-Control", "no-cache");
            res.set_content(body, "application/json");
        };
    };
    http.Get("/api/status", cached(s.status));
    http.Get("/api/rankings", cached(s.rankings));
    http.Get("/api/uniques", cached(s.uniques));
    http.Get("/api/server", [&s](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_content(s.serverJson, "application/json");
    });
    http.Post("/api/register", [&s](const httplib::Request& req, httplib::Response& res) { s.Register(req, res); });
    http.Get("/api/site", cached(s.site));
    http.Get("/api/stats", cached(s.stats));
    http.Get("/api/load", cached(s.load));
    if (config.admin) {
        AdminHost host;
        host.config = &s.config;
        host.pool = &s.pool;
        host.visitor = [&s](const httplib::Request& req) { return s.Visitor(req); };
        host.siteChanged = [&s] { s.site.Refresh(); };
        s.admin = RegisterAdmin(http, host);
    }
    {
        AdminHost host;
        host.config = &s.config;
        host.pool = &s.pool;
        host.visitor = [&s](const httplib::Request& req) { return s.Visitor(req); };
        host.siteChanged = [&s] { s.site.Refresh(); };
        s.players = RegisterPlayers(http, host);
    }
    // The uploads (images, the client), the client offered as a download.
    {
        std::error_code ec;
        const std::filesystem::path files = std::filesystem::absolute(config.filesDir);
        std::filesystem::create_directories(files / "images", ec);
        std::filesystem::create_directories(files / "client", ec);
        if (!http.set_mount_point("/files", files.string())) {
            error = "the files' folder " + files.string() + " cannot be made";
            s.pool.Stop();
            s_.reset();
            return false;
        }
        http.set_file_request_handler([](const httplib::Request& req, httplib::Response& res) {
            if (req.path.starts_with("/files/client/")) res.set_header("Content-Disposition", "attachment");
        });
    }
    if (!config.siteRoot.empty()) {
        if (!http.set_mount_point("/", config.siteRoot)) {
            error = "the site's folder " + config.siteRoot + " is not there (build it: cd website, npm run build)";
            s.pool.Stop();
            s_.reset();
            return false;
        }
        // The page's own paths are the page (it shows the download page or the administration itself).
        const std::string index = (std::filesystem::path(config.siteRoot) / "index.html").string();
        const auto page = [index](const httplib::Request&, httplib::Response& res) {
            std::ifstream in(index, std::ios::binary);
            std::stringstream all;
            all << in.rdbuf();
            res.set_content(all.str(), "text/html; charset=utf-8");
        };
        for (const char* path : {"/download", "/admin", "/dev", "/news", "/about", "/server", "/rankings", R"(/news/\d+)"}) http.Get(path, page);
    }

    if (config.port == 0) {
        const int port = http.bind_to_any_port(config.host);
        s.port = port > 0 ? std::uint16_t(port) : 0;
    } else if (http.bind_to_port(config.host, config.port)) {
        s.port = config.port;
    }
    if (s.port == 0) {
        error = std::format("cannot listen on {}:{}", config.host, config.port);
        s.pool.Stop();
        s_.reset();
        return false;
    }
    s.listener = std::thread([&http] { http.listen_after_bind(); });
    http.wait_until_ready();
    // The first answers on their way before the first visitor.
    s.status.Get(0ms);
    s.rankings.Get(0ms);
    s.uniques.Get(0ms);
    s.site.Get(0ms);
    return true;
}

void WebServer::Poll() {
    if (!s_ || !s_->bus) return;
    if (!s_->bus->TakeNotifications().empty()) s_->uniques.Refresh();
}

void WebServer::Stop() {
    if (!s_) return;
    s_->http.stop();
    if (s_->listener.joinable()) s_->listener.join();
    s_->pool.Stop();
    s_.reset();
}

std::uint16_t WebServer::Port() const { return s_ ? s_->port : 0; }

} // namespace sr::server
