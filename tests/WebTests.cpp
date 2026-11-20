#include <doctest/doctest.h>

#include <cctype>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "sr/server/Crypto.h"
#include "sr/server/Database.h"
#include "sr/server/Store.h"
#include "sr/server/WebServer.h"
#include "ServerTestSupport.h"

using namespace std::chrono_literals;
using namespace sr;
using namespace sr::test;
using json = nlohmann::json;

namespace {

server::WebConfig TestConfig(const TestDatabase& t) {
    server::WebConfig c;
    c.databaseUrl = t.Url();
    c.port = 0; // any free port
    c.shardName = "Reimagined";
    c.rateExp = 10.0;
    c.statusAge = 0s; // every request asks again (the answer comes a moment later)
    c.rankingAge = 0s;
    return c;
}

struct Site {
    server::WebServer server;
    std::unique_ptr<httplib::Client> client;
    explicit Site(const server::WebConfig& config) {
        std::string error;
        REQUIRE_MESSAGE(server.Start(config, error), error);
        client = std::make_unique<httplib::Client>("127.0.0.1", server.Port());
        client->set_read_timeout(30s);
    }
    json Get(const char* path, int expect = 200) {
        const auto r = client->Get(path);
        REQUIRE(r);
        CHECK(r->status == expect);
        return json::parse(r->body, nullptr, false);
    }
    std::pair<int, json> Register(const json& body) {
        const auto r = client->Post("/api/register", body.dump(), "application/json");
        REQUIRE(r);
        return {r->status, json::parse(r->body, nullptr, false)};
    }
    // Asks until `done` holds (the caches answer with the last result while they ask again).
    json GetUntil(const char* path, const std::function<bool(const json&)>& done) {
        json j;
        for (int i = 0; i < 200; ++i) {
            j = Get(path);
            if (done(j)) return j;
            std::this_thread::sleep_for(20ms);
        }
        FAIL("never: " << j.dump());
        return j;
    }
};

server::GameServerInfo Report(std::uint16_t id, std::uint32_t online, std::uint32_t capacity) {
    server::GameServerInfo s;
    s.id = id;
    s.name = "Silkroad";
    s.group = 1;
    s.groupName = "Silkroad";
    s.host = "127.0.0.1";
    s.port = std::uint16_t(16000 + id);
    s.capacity = capacity;
    s.online = online;
    s.reporting = true;
    return s;
}

} // namespace

TEST_CASE("Web: registration makes an account the login server takes; names, passwords and e-mails are checked") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Site site(TestConfig(t));

    const json info = site.Get("/api/server");
    CHECK(info["name"] == "Reimagined");
    CHECK(info["levelCap"] == 110);
    CHECK(info["rates"]["exp"] == 10.0);
    CHECK(info["registration"]["open"] == true);
    CHECK(info["registration"]["betaKey"] == false);

    auto [made, body] = site.Register({{"name", "Walker"}, {"password", "secret123"}, {"email", "walker@example.com"}});
    CHECK(made == 201);
    CHECK(body["ok"] == true);
    bool ok = false;
    const auto account = server::FindAccount(t.Db(), "walker", ok);
    REQUIRE(account);
    CHECK(server::VerifyPassword("secret123", account->passwordHash));
    CHECK(t.Db().Query("SELECT email, reg_ip FROM sro_vt_account.tb_user WHERE jid = $1", {account->id}).Text(0, 0) == "walker@example.com");
    CHECK(t.Db().Query("SELECT reg_ip FROM sro_vt_account.tb_user WHERE jid = $1", {account->id}).Text(0, 0) == "127.0.0.1");

    CHECK(site.Register({{"name", "WALKER"}, {"password", "secret123"}}).second["error"] == "name-taken");
    CHECK(site.Register({{"name", "no"}, {"password", "secret123"}}).second["error"] == "invalid-name");
    CHECK(site.Register({{"name", "bad name"}, {"password", "secret123"}}).second["error"] == "invalid-name");
    CHECK(site.Register({{"name", "Other"}, {"password", "short"}}).second["error"] == "invalid-password");
    CHECK(site.Register({{"name", "Other"}, {"password", "secret123"}, {"email", "not-mail"}}).second["error"] == "invalid-email");
    // Only JSON (a form on another site cannot post it unasked).
    const auto form = site.client->Post("/api/register", "name=Other&password=secret123", "application/x-www-form-urlencoded");
    REQUIRE(form);
    CHECK(form->status == 415);
    CHECK_FALSE(server::FindAccount(t.Db(), "Other", ok));
}

TEST_CASE("Web: attempts are limited per address; beta keys are good once") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    REQUIRE_MESSAGE(server::MigrateSchema(t.Db(), error), error);

    SUBCASE("the limit") {
        server::WebConfig c = TestConfig(t);
        c.registerAttempts = 2;
        Site site(c);
        CHECK(site.Register({{"name", "First"}, {"password", "secret123"}}).first == 201);
        CHECK(site.Register({{"name", "Second"}, {"password", "secret123"}}).first == 201);
        const auto third = site.Register({{"name", "Third"}, {"password", "secret123"}});
        CHECK(third.first == 429);
        CHECK(third.second["error"] == "too-many");
        bool ok = false;
        CHECK_FALSE(server::FindAccount(t.Db(), "Third", ok));
    }
    SUBCASE("beta keys") {
        const auto keys = server::MakeBetaKeys(t.Db(), 2, "test", error);
        REQUIRE_MESSAGE(keys.size() == 2, error);
        CHECK(keys[0].size() == 19); // XXXX-XXXX-XXXX-XXXX
        CHECK(keys[0] != keys[1]);
        server::WebConfig c = TestConfig(t);
        c.betaKeys = true;
        Site site(c);
        CHECK(site.Get("/api/server")["registration"]["betaKey"] == true);
        CHECK(site.Register({{"name", "NoKey"}, {"password", "secret123"}}).second["error"] == "key-required");
        CHECK(site.Register({{"name", "BadKey"}, {"password", "secret123"}, {"key", "AAAA-BBBB-CCCC-DDDD"}}).second["error"] == "key-refused");
        // Typed loosely: lower case, without the dashes.
        std::string loose;
        for (const char ch : keys[0])
            if (ch != '-') loose += char(std::tolower(static_cast<unsigned char>(ch)));
        CHECK(site.Register({{"name", "Keyed"}, {"password", "secret123"}, {"key", loose}}).first == 201);
        CHECK(site.Register({{"name", "Again"}, {"password", "secret123"}, {"key", keys[0]}}).second["error"] == "key-refused");
        bool ok = false;
        CHECK(server::FindAccount(t.Db(), "Keyed", ok));
        CHECK_FALSE(server::FindAccount(t.Db(), "BadKey", ok)); // the account went with the refused key
        CHECK_FALSE(server::FindAccount(t.Db(), "Again", ok));
        const auto keyed = server::FindAccount(t.Db(), "Keyed", ok);
        CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_account.sr_beta_keys WHERE used_by = $1", {keyed->id}).Int(0, 0) == 1);
    }
}

TEST_CASE("Web: the status counts a shard's two game servers; one stopping leaves it up, both take it down") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    REQUIRE(server::ReportGameServer(t.Db(), Report(1, 3, 100), error));
    REQUIRE(server::ReportGameServer(t.Db(), Report(2, 4, 100), error));
    Site site(TestConfig(t));

    json s = site.GetUntil("/api/status", [](const json& j) { return j.contains("players") && j["players"] == 7; });
    CHECK(s["online"] == true);
    CHECK(s["capacity"] == 200);
    CHECK(s["gameServers"] == 2);
    REQUIRE(s["shards"].size() == 1);
    CHECK(s["shards"][0]["name"] == "Reimagined");
    CHECK(s["shards"][0]["gameServersTotal"] == 2);

    REQUIRE(server::StopGameServer(t.Db(), 1));
    s = site.GetUntil("/api/status", [](const json& j) { return j["gameServers"] == 1; });
    CHECK(s["online"] == true);
    CHECK(s["players"] == 4); // the stopped one's players no longer count
    CHECK(s["capacity"] == 100);
    REQUIRE(server::StopGameServer(t.Db(), 2));
    s = site.GetUntil("/api/status", [](const json& j) { return j["online"] == false; });
    CHECK(s["players"] == 0);
    CHECK(s["shards"][0]["up"] == false);
}

TEST_CASE("Web: a shard's capacity is the one its game servers report (both together), with one of them or both up") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    const auto report = [](std::uint16_t id, std::uint32_t online) {
        server::GameServerInfo s = Report(id, online, 3000); // each carries the whole shard
        s.shardCapacity = 3000;
        return s;
    };
    REQUIRE(server::ReportGameServer(t.Db(), report(1, 5), error));
    REQUIRE(server::ReportGameServer(t.Db(), report(2, 6), error));
    Site site(TestConfig(t));

    json s = site.GetUntil("/api/status", [](const json& j) { return j.contains("players") && j["players"] == 11; });
    CHECK(s["capacity"] == 3000);
    REQUIRE(s["shards"].size() == 1);
    CHECK(s["shards"][0]["capacity"] == 3000);
    REQUIRE(server::StopGameServer(t.Db(), 1));
    s = site.GetUntil("/api/status", [](const json& j) { return j["gameServers"] == 1; });
    CHECK(s["capacity"] == 3000);
}

TEST_CASE("Web: the rankings: players by level and experience, guilds, jobs; game masters, bots and the deleted left out") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    server::Database& db = t.Db();
    // An account with one character: level, experience, model (1907 Chinese, 14875 European), deleted.
    const auto character = [&](const char* account, const char* name, int level, std::int64_t exp, int model, int deleted = 0) {
        bool ok = false;
        if (!server::FindAccount(db, account, ok)) REQUIRE(server::CreateAccount(db, account, "secret1", error) == server::CreateAccountResult::Created);
        const std::int64_t jid = server::FindAccount(db, account, ok)->id;
        const server::QueryResult r = db.Query("INSERT INTO sro_vt_shard._char (charname16, curlevel, expoffset, refobjid, deleted, scale, posx, posy, posz, lastlogout) "
                                               "VALUES ($1, $2, $3, $4, $5, 0, 0, 0, 0, now()) RETURNING charid",
                                               {name, level, exp, model, deleted});
        REQUIRE_MESSAGE(r.Ok(), r.Error());
        const std::int64_t id = r.Int(0, 0);
        REQUIRE(db.Query("INSERT INTO sro_vt_shard._user (userjid, charid) VALUES ($1, $2)", {jid, id}).Ok());
        return id;
    };
    const std::int64_t astra = character("astra", "Astra", 110, 500, 1907);
    const std::int64_t kaizen = character("kaizen", "Kaizen", 110, 900, 14875);
    character("nyx", "Nyx", 98, 0, 1907);
    character("gamemaster", "TheGM", 140, 0, 1907);
    REQUIRE(db.Query("UPDATE sro_vt_account.tb_user SET sec_primary = 1 WHERE struserid = 'gamemaster'").Ok());
    character("admin", "[GM]Admin", 130, 0, 1907); // an account not marked, a GM's name
    character("bot17", "Bot17", 120, 0, 1907);
    character("gone", "Gone", 130, 0, 1907, 1);

    const server::QueryResult guild = db.Query("INSERT INTO sro_vt_shard._guild (name, lvl, gatheredsp, foundationdate) VALUES ('Nightsong', 4, 10, now()) RETURNING id");
    REQUIRE_MESSAGE(guild.Ok(), guild.Error());
    REQUIRE(db.Query("INSERT INTO sro_vt_shard._guildmember (guildid, charid, charname, memberclass, charlevel, gp_donation, joindate) "
                     "VALUES ($1, $2, 'Astra', 0, 110, 0, now())",
                     {guild.Int(0, 0), astra})
                .Ok());
    REQUIRE(db.Query("INSERT INTO sro_vt_shard._guild (name, lvl, gatheredsp, foundationdate) VALUES ('Vanguard', 4, 50, now())").Ok());
    REQUIRE(db.Query("INSERT INTO sro_vt_shard._chartrijob (charid, jobtype, level, exp) VALUES ($1, 3, 5, 10), ($2, 1, 6, 0)", {astra, kaizen}).Ok());

    Site site(TestConfig(t));
    const json r = site.GetUntil("/api/rankings", [](const json& j) { return j.contains("players") && !j["players"].empty(); });
    REQUIRE(r["players"].size() == 3);
    CHECK(r["players"][0]["name"] == "Kaizen"); // the same level, more experience
    CHECK(r["players"][0]["detail"] == "European");
    CHECK(r["players"][1]["name"] == "Astra");
    CHECK(r["players"][1]["detail"] == "Nightsong");
    CHECK(r["players"][1]["value"] == 110);
    CHECK(r["players"][2]["name"] == "Nyx");
    REQUIRE(r["guilds"].size() == 2);
    CHECK(r["guilds"][0]["name"] == "Vanguard"); // the same level, more gathered SP
    CHECK(r["guilds"][1]["detail"] == "Astra");  // its master
    REQUIRE(r["jobs"].size() == 2);
    CHECK(r["jobs"][0]["name"] == "Kaizen");
    CHECK(r["jobs"][0]["detail"] == "Trader");
    CHECK(r["jobs"][1]["detail"] == "Hunter");
}
