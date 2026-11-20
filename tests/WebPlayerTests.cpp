#include <doctest/doctest.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "sr/server/Crypto.h"
#include "sr/server/Database.h"
#include "sr/server/Store.h"
#include "sr/server/WebAdmin.h"
#include "sr/server/WebServer.h"
#include "ServerTestSupport.h"

using namespace std::chrono_literals;
using namespace sr;
using namespace sr::test;
using json = nlohmann::json;

namespace {

// A player's character (its account's).
std::int64_t MakeCharacter(server::Database& db, const char* account, const char* name, int level) {
    bool ok = false;
    const std::int64_t jid = server::FindAccount(db, account, ok)->id;
    const server::QueryResult r = db.Query("INSERT INTO sro_vt_shard._char (charname16, curlevel, scale, posx, posy, posz, lastlogout) "
                                           "VALUES ($1, $2, 0, 0, 0, 0, now()) RETURNING charid",
                                           {name, level});
    REQUIRE_MESSAGE(r.Ok(), r.Error());
    REQUIRE(db.Query("INSERT INTO sro_vt_shard._user (userjid, charid) VALUES ($1, $2)", {jid, r.Int(0, 0)}).Ok());
    return r.Int(0, 0);
}

struct Player {
    httplib::Client* client = nullptr;
    std::string cookie, csrf;
    httplib::Headers Headers(bool token = true) const {
        httplib::Headers h{{"Cookie", cookie}};
        if (token) h.emplace("X-CSRF-Token", csrf);
        return h;
    }
};

std::pair<int, json> SignIn(httplib::Client& client, const char* name, const char* password, Player* into = nullptr) {
    const auto r = client.Post("/api/account/login", json{{"name", name}, {"password", password}}.dump(), "application/json");
    REQUIRE(r);
    const json body = json::parse(r->body, nullptr, false);
    if (into != nullptr && r->status == 200) {
        const std::string set = r->get_header_value("Set-Cookie");
        CHECK(set.find("HttpOnly") != std::string::npos);
        CHECK(set.find("Path=/api") != std::string::npos);
        into->client = &client;
        into->cookie = set.substr(0, set.find(';'));
        into->csrf = body["csrf"];
    }
    return {r->status, body};
}

} // namespace

TEST_CASE("Web players: a game account signs in; comments on the news as one's character; the admin removes them; uptime, stats, unique kills") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    server::Database& db = t.Db();
    REQUIRE_MESSAGE(server::MigrateSchema(db, error), error);
    REQUIRE(server::CreateAccount(db, "walker", "walker-pass", error) == server::CreateAccountResult::Created);
    REQUIRE(server::CreateAccount(db, "other", "other-pass", error) == server::CreateAccountResult::Created);
    REQUIRE(server::CreateAccount(db, "banned", "banned-pass", error) == server::CreateAccountResult::Created);
    REQUIRE(server::SetBlock(db, "banned", "botting", 3600));
    MakeCharacter(db, "walker", "Astra", 110);
    MakeCharacter(db, "walker", "Nyx", 40);
    MakeCharacter(db, "other", "Kaizen", 90);
    const std::int64_t news = db.Query("INSERT INTO sro_vt_account.sr_site_news (day, tag, title, body) VALUES ('2026-11-12', 'Update', 'Beta', 'Soon.') RETURNING id").Int(0, 0);
    const std::int64_t hidden = db.Query("INSERT INTO sro_vt_account.sr_site_news (day, tag, title, body, visible) VALUES ('2026-11-12', 'x', 'Hidden', '.', false) RETURNING id").Int(0, 0);

    server::WebConfig c;
    c.databaseUrl = t.Url();
    c.port = 0;
    c.filesDir = (std::filesystem::temp_directory_path() / "sr-web-files-players").string();
    c.secureCookie = false;
    c.statusAge = 0s;
    c.rankingAge = 0s;
    server::WebServer web;
    REQUIRE_MESSAGE(web.Start(c, error), error);
    httplib::Client client("127.0.0.1", web.Port());
    client.set_read_timeout(30s);

    // Signing in: the game's password; a wrong one, an unknown account, a blocked one refused (and nothing blocked by it).
    CHECK(SignIn(client, "walker", "wrong-pass").first == 401);
    CHECK(SignIn(client, "nobody", "whatever").first == 401);
    const auto blocked = SignIn(client, "banned", "banned-pass");
    CHECK(blocked.first == 403);
    CHECK(blocked.second["message"].get<std::string>().find("botting") != std::string::npos);
    bool ok = false;
    CHECK_FALSE(server::FindAccount(db, "walker", ok)->blocked);
    Player w;
    const auto in = SignIn(client, "walker", "walker-pass", &w);
    REQUIRE(in.first == 200);
    REQUIRE(in.second["characters"].size() == 2);
    CHECK(in.second["characters"][0]["name"] == "Astra"); // the highest first
    CHECK(json::parse(client.Get("/api/account/me", w.Headers(false))->body)["name"] == "walker");

    // Comments: as one's own character only, with the page's token, one every 20 seconds.
    const std::string path = "/api/news/" + std::to_string(news) + "/comments";
    CHECK(client.Post(path, w.Headers(false), json{{"text", "Hi"}, {"character", "Astra"}}.dump(), "application/json")->status == 403);
    CHECK(client.Post(path, w.Headers(), json{{"text", "Hi"}, {"character", "Kaizen"}}.dump(), "application/json")->status == 400); // not its
    CHECK(client.Post(path, w.Headers(), json{{"text", ""}, {"character", "Astra"}}.dump(), "application/json")->status == 400);
    CHECK(client.Post("/api/news/" + std::to_string(hidden) + "/comments", w.Headers(), json{{"text", "Hi"}, {"character", "Astra"}}.dump(), "application/json")->status == 404);
    auto posted = client.Post(path, w.Headers(), json{{"text", "Can't wait <b>!</b>"}, {"character", "astra"}}.dump(), "application/json");
    REQUIRE(posted);
    REQUIRE(posted->status == 201);
    CHECK(client.Post(path, w.Headers(), json{{"text", "Again"}, {"character", "Nyx"}}.dump(), "application/json")->status == 429);
    // Another player's, after the gap (written as if earlier).
    REQUIRE(db.Query("UPDATE sro_vt_account.sr_site_comments SET created_at = now() - interval '1 minute'").Ok());
    Player o;
    REQUIRE(SignIn(client, "other", "other-pass", &o).first == 200);
    REQUIRE(client.Post(path, o.Headers(), json{{"text", "Me too"}, {"character", "Kaizen"}}.dump(), "application/json")->status == 201);
    // Anyone reads them; each sees which are its own; the account's name is never shown.
    json list = json::parse(client.Get(path, w.Headers(false))->body)["comments"];
    REQUIRE(list.size() == 2);
    CHECK(list[0]["author"] == "Astra");
    CHECK(list[0]["text"] == "Can't wait <b>!</b>"); // as written: the page shows it as text
    CHECK(list[0]["mine"] == true);
    CHECK(list[1]["mine"] == false);
    CHECK(list.dump().find("walker") == std::string::npos);
    // One's own removed; another's not.
    const std::int64_t mine = list[0]["id"], theirs = list[1]["id"];
    CHECK(client.Delete("/api/comments/" + std::to_string(theirs), w.Headers())->status == 404);
    CHECK(client.Delete("/api/comments/" + std::to_string(mine), w.Headers())->status == 200);
    CHECK(json::parse(client.Get(path)->body)["comments"].size() == 1);

    // The admin removes another's; the news counts what is left.
    std::string secret;
    REQUIRE(server::MakeAdmin(db, "boss", "moderator", "boss-long-password", secret, error));
    const auto login = client.Post("/api/admin/login",
                                   json{{"name", "boss"}, {"password", "boss-long-password"}, {"code", server::TotpCode(server::FromBase32(secret), std::int64_t(std::time(nullptr)))}}.dump(),
                                   "application/json");
    REQUIRE(login);
    REQUIRE(login->status == 200);
    const httplib::Headers admin{{"Cookie", login->get_header_value("Set-Cookie").substr(0, login->get_header_value("Set-Cookie").find(';'))},
                                 {"X-CSRF-Token", json::parse(login->body)["csrf"].get<std::string>()}};
    const json all = json::parse(client.Get("/api/admin/comments", admin)->body)["comments"];
    REQUIRE(all.size() == 2);
    CHECK(all[0]["account"] == "other");
    CHECK(client.Delete("/api/admin/comments/" + std::to_string(theirs), admin)->status == 200);
    CHECK(json::parse(client.Get(path)->body)["comments"].empty());
    json site;
    for (int i = 0; i < 100; ++i) {
        site = json::parse(client.Get("/api/site")->body);
        if (!site["news"].empty() && site["news"][0]["comments"] == 0) break;
        std::this_thread::sleep_for(20ms);
    }
    CHECK(site["news"][0]["comments"] == 0);

    // Signing out ends the session.
    CHECK(client.Post("/api/account/logout", w.Headers(), "{}", "application/json")->status == 200);
    CHECK(client.Get("/api/account/me", w.Headers(false))->status == 401);

    // A game server's uptime: its run's start, kept while it reports, new after a stop.
    server::GameServerInfo s;
    s.id = 1;
    s.host = "127.0.0.1";
    s.port = 16001;
    s.capacity = 100;
    REQUIRE(server::ReportGameServer(db, s, error));
    REQUIRE(db.Query("UPDATE sro_vt_account.sr_shard_reports SET started_at = now() - interval '2 hours'").Ok());
    REQUIRE(server::ReportGameServer(db, s, error)); // still the same run
    bool listed = false;
    std::int64_t started = server::ListGameServers(db, 15, listed).front().startedAt;
    CHECK(std::int64_t(std::time(nullptr)) - started >= 7190);
    REQUIRE(server::StopGameServer(db, 1));
    REQUIRE(server::ReportGameServer(db, s, error)); // a new run
    started = server::ListGameServers(db, 15, listed).front().startedAt;
    CHECK(std::int64_t(std::time(nullptr)) - started < 60);
    json status;
    for (int i = 0; i < 100; ++i) {
        status = json::parse(client.Get("/api/status")->body);
        if (status.contains("servers") && !status["servers"].empty() && status["servers"][0]["up"] == true) break;
        std::this_thread::sleep_for(20ms);
    }
    CHECK(status["servers"][0]["startedAt"].get<std::int64_t>() == started);

    // Unique kills (the killing hit's player) and the numbers.
    for (const char* killer : {"Astra", "Astra", "Kaizen"}) {
        server::UniqueEvent e;
        e.kind = server::UniqueEvent::Kind::Killed;
        e.refObjId = 1954;
        e.codename = "MOB_CH_TIGERWOMAN";
        e.name = "Tiger Girl";
        e.server = 1;
        e.killerName = killer;
        e.at = double(std::time(nullptr));
        REQUIRE(server::RecordUniqueEvent(db, e));
    }
    json ranks;
    for (int i = 0; i < 100; ++i) {
        ranks = json::parse(client.Get("/api/rankings")->body);
        if (ranks.contains("uniqueKills") && ranks["uniqueKills"].size() == 2) break;
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(ranks["uniqueKills"].size() == 2);
    CHECK(ranks["uniqueKills"][0]["name"] == "Astra");
    CHECK(ranks["uniqueKills"][0]["value"] == 2);
    CHECK(ranks["uniqueKills"][0]["detail"] == "Tiger Girl");
    bool statsOk = false;
    const server::ShardStats stats = server::LoadShardStats(db, statsOk);
    REQUIRE(statsOk);
    CHECK(stats.accounts == 3 + 1 - 1); // the three players (the admin is not a game account)
    CHECK(stats.characters == 3);
    CHECK(stats.uniquesKilled == 3);
    CHECK(stats.uniquesKilledToday == 3);
    CHECK(client.Get("/api/stats")->status == 200);
}
