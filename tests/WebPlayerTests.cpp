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

TEST_CASE("Web players: the player's own page: the account, its characters; a new password and e-mail with the current password") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    server::Database& db = t.Db();
    REQUIRE_MESSAGE(server::MigrateSchema(db, error), error);
    REQUIRE(server::CreateAccount(db, "walker", "walker-pass", error, server::LegacyPasswords::Keep, {"walker@example.com", "", {}}) == server::CreateAccountResult::Created);
    REQUIRE(server::CreateAccount(db, "other", "other-pass", error) == server::CreateAccountResult::Created);
    const std::int64_t astra = MakeCharacter(db, "walker", "Astra", 110);
    MakeCharacter(db, "walker", "Nyx", 40);
    MakeCharacter(db, "other", "Kaizen", 90);
    REQUIRE(db.Query("UPDATE sro_vt_shard._char SET refobjid = 14890, remaingold = 1234567 WHERE charid = $1", {astra}).Ok());
    REQUIRE(db.Query("INSERT INTO sro_vt_shard._guild (name, lvl, gatheredsp, foundationdate) VALUES ('Dragons', 1, 0, now())").Ok());
    REQUIRE(db.Query("UPDATE sro_vt_shard._char SET guildid = (SELECT id FROM sro_vt_shard._guild WHERE name = 'Dragons') WHERE charid = $1", {astra}).Ok());
    REQUIRE(db.Query("INSERT INTO sro_vt_shard._chartrijob (charid, jobtype, level) VALUES ($1, 3, 4)", {astra}).Ok());

    server::WebConfig c;
    c.databaseUrl = t.Url();
    c.port = 0;
    c.filesDir = (std::filesystem::temp_directory_path() / "sr-web-files-profile").string();
    c.secureCookie = false;
    server::WebServer web;
    REQUIRE_MESSAGE(web.Start(c, error), error);
    httplib::Client client("127.0.0.1", web.Port());
    client.set_read_timeout(30s);

    CHECK(client.Get("/api/account/profile")->status == 401);
    Player w;
    REQUIRE(SignIn(client, "walker", "walker-pass", &w).first == 200);
    const json p = json::parse(client.Get("/api/account/profile", w.Headers(false))->body);
    CHECK(p["account"]["name"] == "walker");
    CHECK(p["account"]["email"] == "w***@example.com"); // masked
    REQUIRE(p["characters"].size() == 2);                 // its own only
    const json& a = p["characters"][0];
    CHECK(a["name"] == "Astra");
    CHECK(a["race"] == "European");
    CHECK(a["sex"] == "female");
    CHECK(a["gold"] == 1234567);
    CHECK(a["guild"] == "Dragons");
    CHECK(a["job"] == "Hunter");
    CHECK(a["jobLevel"] == 4);
    CHECK(a["online"] == false);
    CHECK(p["characters"][1]["guild"] == "");
    CHECK(p.dump().find("Kaizen") == std::string::npos);

    // A second sign-in (another browser): the password's change ends it, not this one.
    Player w2;
    REQUIRE(SignIn(client, "walker", "walker-pass", &w2).first == 200);
    const auto post = [&](const Player& who, const char* path, const json& body, bool token = true) {
        return client.Post(path, who.Headers(token), body.dump(), "application/json")->status;
    };
    CHECK(post(w, "/api/account/password", {{"current", "walker-pass"}, {"password", "new-pass-1"}}, false) == 403); // no page token
    CHECK(post(w, "/api/account/password", {{"current", "wrong-pass"}, {"password", "new-pass-1"}}) == 403);
    CHECK(post(w, "/api/account/password", {{"current", "walker-pass"}, {"password", "x"}}) == 400);
    CHECK(post(w, "/api/account/password", {{"current", "walker-pass"}, {"password", "new-pass-1"}}) == 200);
    CHECK(SignIn(client, "walker", "walker-pass").first == 401);
    CHECK(client.Get("/api/account/me", w.Headers(false))->status == 200);
    CHECK(client.Get("/api/account/me", w2.Headers(false))->status == 401);

    CHECK(post(w, "/api/account/email", {{"current", "new-pass-1"}, {"email", "not-an-address"}}) == 400);
    CHECK(post(w, "/api/account/email", {{"current", "walker-pass"}, {"email", "new@example.org"}}) == 403);
    CHECK(post(w, "/api/account/email", {{"current", "new-pass-1"}, {"email", "new@example.org"}}) == 200);
    CHECK(json::parse(client.Get("/api/account/profile", w.Headers(false))->body)["account"]["email"] == "n***@example.org");
    CHECK(post(w, "/api/account/email", {{"current", "new-pass-1"}, {"email", ""}}) == 200);
    CHECK(json::parse(client.Get("/api/account/profile", w.Headers(false))->body)["account"]["hasEmail"] == false);
    web.Stop();
}

TEST_CASE("Web support: a message and a bug report with a screenshot; the staff's answer and note; the player's view; the limits") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error, secret;
    server::Database& db = t.Db();
    REQUIRE_MESSAGE(server::MigrateSchema(db, error), error);
    REQUIRE(server::CreateAccount(db, "walker", "walker-pass", error) == server::CreateAccountResult::Created);
    REQUIRE(server::MakeAdmin(db, "boss", "moderator", "boss-long-password", secret, error));

    server::WebConfig c;
    c.databaseUrl = t.Url();
    c.port = 0;
    c.filesDir = (std::filesystem::temp_directory_path() / "sr-web-files-support").string();
    c.supportDir = (std::filesystem::temp_directory_path() / "sr-web-support").string();
    std::filesystem::remove_all(c.supportDir);
    c.secureCookie = false;
    c.supportAttempts = 3;
    server::WebServer web;
    REQUIRE_MESSAGE(web.Start(c, error), error);
    httplib::Client client("127.0.0.1", web.Port());
    client.set_read_timeout(30s);
    const auto send = [&](const json& body, const httplib::Headers& h = {}) { return client.Post("/api/support", h, body.dump(), "application/json")->status; };
    const auto upload = [&](const std::string& path, const std::string& bytes) { return client.Post(path, bytes, "application/octet-stream"); };

    // Anonymous: an e-mail address is needed; the text's length; the kind's own categories. None of these count.
    json contact = {{"kind", "contact"}, {"category", "account"}, {"subject", "Lost my password"}, {"text", "I cannot log in any more, please help."}};
    CHECK(send(contact) == 400);
    contact["email"] = "someone@example.com";
    CHECK(send({{"kind", "contact"}, {"category", "graphics"}, {"subject", "Hello there"}, {"text", "0123456789 0123"}, {"email", "a@example.com"}}) == 400);
    CHECK(send({{"kind", "contact"}, {"category", "other"}, {"subject", "Hi there"}, {"text", "short"}, {"email", "a@example.com"}}) == 400);
    // A robot (the hidden field filled in): thanked, nothing kept.
    json robot = contact;
    robot["website"] = "http://spam.example";
    CHECK(send(robot) == 201);
    REQUIRE(send(contact) == 201);
    const std::int64_t first = db.Query("SELECT max(id) FROM sro_vt_account.sr_support_tickets").Int(0, 0);
    CHECK(db.Query("SELECT count(*) FROM sro_vt_account.sr_support_tickets").Int(0, 0) == 1);

    // Signed in: the bug report is the player's; screenshots and a video join it with the token; nothing else does.
    Player w;
    REQUIRE(SignIn(client, "walker", "walker-pass", &w).first == 200);
    const auto made = client.Post("/api/support", w.Headers(false),
                                  json{{"kind", "bug"}, {"category", "graphics"}, {"subject", "Water hides my legs"}, {"text", "Standing in the river, the legs vanish."}}.dump(),
                                  "application/json");
    REQUIRE(made->status == 201);
    const json bug = json::parse(made->body);
    const std::int64_t id = bug["id"];
    const std::string token = bug["uploadToken"];
    const std::string base = "/api/support/" + std::to_string(id) + "/files?name=shot.png&token=";
    const std::string png = std::string("\x89PNG\r\n\x1A\n", 8) + std::string(200, 'p');
    CHECK(upload(base + std::string(64, '0'), png)->status == 403);
    CHECK(upload(base + token, "MZ this is a program")->status == 400);
    CHECK(upload(base + token, png)->status == 201);
    const std::string mp4 = std::string("\0\0\0\x18" "ftypmp42", 12) + std::string(500, 'v');
    CHECK(json::parse(upload(base + token, mp4)->body)["mime"] == "video/mp4");
    for (int i = 0; i < 3; ++i) CHECK(upload(base + token, png)->status == 201);
    CHECK(upload(base + token, png)->status == 409); // five a ticket
    // The attachments are not under the public files.
    CHECK(client.Get("/files/" + std::to_string(id))->status != 200);

    // The caps: a ticket's files together (200 MB), the folder's whole (web-support-max-mb).
    CHECK(client.Post("/api/support/" + std::to_string(id) + "/files?name=big.mp4&token=" + token,
                      std::string("\0\0\0\x18" "ftypmp42", 12) + std::string(300, 'v'), "application/octet-stream")
              ->status == 409); // (five already: the count comes first)

    // The limit per address: three tickets an hour here (two sent).
    CHECK(send(contact) == 201);
    CHECK(send(contact) == 429);

    // The staff: the list, the ticket with its files, a note and an answer; the player sees the answer, not the note.
    const auto login = client.Post("/api/admin/login",
                                   json{{"name", "boss"}, {"password", "boss-long-password"}, {"code", server::TotpCode(server::FromBase32(secret), std::int64_t(std::time(nullptr)))}}.dump(),
                                   "application/json");
    REQUIRE(login->status == 200);
    Player staff{&client, login->get_header_value("Set-Cookie").substr(0, login->get_header_value("Set-Cookie").find(';')), json::parse(login->body)["csrf"]};
    const auto admin = [&](const char* method, const std::string& path, const json& body = json::object()) {
        const std::string url = "/api/admin" + path;
        if (std::string_view(method) == "GET") return client.Get(url, staff.Headers(false));
        if (std::string_view(method) == "PUT") return client.Put(url, staff.Headers(), body.dump(), "application/json");
        if (std::string_view(method) == "DELETE") return client.Delete(url, staff.Headers(), body.dump(), "application/json");
        return client.Post(url, staff.Headers(), body.dump(), "application/json");
    };
    const json list = json::parse(admin("GET", "/tickets?status=open")->body);
    CHECK(list["tickets"].size() == 3);
    CHECK(list["counts"]["bug"]["open"] == 1);
    CHECK(json::parse(admin("GET", "/tickets?q=legs")->body)["tickets"].size() == 1);
    const std::string ticket = "/tickets/" + std::to_string(id);
    const json one = json::parse(admin("GET", ticket)->body)["ticket"];
    CHECK(one["account"] == "walker");
    REQUIRE(one["attachments"].size() == 5);
    const std::string fileUrl = "/api/admin" + ticket + "/files/" + std::to_string(one["attachments"][0]["id"].get<std::int64_t>());
    const auto file = client.Get(fileUrl, staff.Headers(false));
    REQUIRE(file);
    CHECK(file->status == 200);
    CHECK(file->body == png);
    CHECK(file->get_header_value("Content-Type") == "image/png");
    CHECK(client.Get(fileUrl)->status == 401);
    CHECK(admin("POST", ticket + "/messages", {{"text", "Looks like the water's depth. Checking."}, {"note", true}})->status == 200);
    CHECK(admin("POST", ticket + "/messages", {{"text", "Thanks, fixed in 0.1.3."}})->status == 200);
    CHECK(json::parse(admin("GET", ticket)->body)["ticket"]["status"] == "answered");
    CHECK(admin("DELETE", "/tickets/" + std::to_string(first))->status == 403); // a moderator does not delete
    const json board = json::parse(admin("GET", "/dashboard")->body);
    CHECK(board["support"]["contact"]["open"] == 2);
    CHECK(board["accounts"]["total"] == 1);
    CHECK(board["registrations"].size() == 30);

    json mine = json::parse(client.Get("/api/account/tickets", w.Headers(false))->body)["tickets"];
    REQUIRE(mine.size() == 1);
    REQUIRE(mine[0]["conversation"].size() == 1);
    CHECK(mine[0]["conversation"][0]["text"] == "Thanks, fixed in 0.1.3.");
    CHECK(mine[0]["fileNames"].size() == 5);
    CHECK(mine.dump().find("depth") == std::string::npos);
    // The player answers: the ticket opens again; another's is not its; a closed one takes no more.
    const auto reply = [&](std::int64_t which, const char* text) {
        return client.Post("/api/account/tickets/" + std::to_string(which) + "/reply", w.Headers(), json{{"text", text}}.dump(), "application/json")->status;
    };
    CHECK(reply(first, "?") == 404);
    CHECK(reply(id, "Still there in caves.") == 200);
    CHECK(json::parse(admin("GET", ticket)->body)["ticket"]["status"] == "open");
    CHECK(admin("PUT", ticket, {{"status", "closed"}})->status == 200);
    CHECK(reply(id, "Again") == 404);
    web.Stop();
    std::filesystem::remove_all(c.supportDir);
}

TEST_CASE("Web: the API's limit per address a minute; the support folder's cap") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    server::Database& db = t.Db();
    REQUIRE_MESSAGE(server::MigrateSchema(db, error), error);
    server::WebConfig c;
    c.databaseUrl = t.Url();
    c.port = 0;
    c.filesDir = (std::filesystem::temp_directory_path() / "sr-web-files-limits").string();
    c.supportDir = (std::filesystem::temp_directory_path() / "sr-web-support-limits").string();
    std::filesystem::remove_all(c.supportDir);
    c.secureCookie = false;
    c.apiPerMinute = 6;
    c.supportMaxBytes = 1000; // the whole folder: 1000 bytes
    server::WebServer web;
    REQUIRE_MESSAGE(web.Start(c, error), error);
    httplib::Client client("127.0.0.1", web.Port());
    client.set_read_timeout(30s);

    // A ticket, then a screenshot larger than the folder's room: refused, nothing kept.
    const auto made = client.Post("/api/support", json{{"kind", "bug"}, {"category", "other"}, {"subject", "Something odd"}, {"text", "It happened twice today."},
                                                       {"email", "a@example.com"}}.dump(), "application/json");
    REQUIRE(made->status == 201);
    const json ticket = json::parse(made->body);
    const std::string png = std::string("\x89PNG\r\n\x1A\n", 8) + std::string(2000, 'p');
    const auto big = client.Post("/api/support/" + std::to_string(ticket["id"].get<std::int64_t>()) + "/files?name=a.png&token=" + ticket["uploadToken"].get<std::string>(),
                                 png, "application/octet-stream");
    CHECK(big->status == 507);
    CHECK(db.Query("SELECT count(*) FROM sro_vt_account.sr_support_files").Int(0, 0) == 0);

    // Six requests to /api a minute from this address (two above), then 429 with Retry-After; the pages are not counted.
    int ok = 0, refused = 0;
    for (int i = 0; i < 8; ++i) {
        const auto r = client.Get("/api/server");
        if (r->status == 200) ++ok;
        if (r->status == 429) {
            ++refused;
            CHECK(r->get_header_value("Retry-After") == "60");
        }
    }
    CHECK(ok == 4);
    CHECK(refused == 4);
    web.Stop();
    std::filesystem::remove_all(c.supportDir);
}
