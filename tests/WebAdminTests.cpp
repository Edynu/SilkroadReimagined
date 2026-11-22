#include <doctest/doctest.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
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

TEST_CASE("Web admin: authenticator codes are RFC 6238's, Base32 is RFC 4648's") {
    const std::string seed = "12345678901234567890";
    const std::span<const std::uint8_t> key(reinterpret_cast<const std::uint8_t*>(seed.data()), seed.size());
    CHECK(server::TotpCode(key, 59, 8) == "94287082");
    CHECK(server::TotpCode(key, 1111111109, 8) == "07081804");
    CHECK(server::TotpCode(key, 1234567890, 8) == "89005924");
    CHECK(server::TotpCode(key, 2000000000, 8) == "69279037");
    const std::string foobar = "foobar";
    CHECK(server::Base32({reinterpret_cast<const std::uint8_t*>(foobar.data()), foobar.size()}) == "MZXW6YTBOI");
    const auto back = server::FromBase32("mzxw 6ytb-oi");
    CHECK(std::string(back.begin(), back.end()) == "foobar");
    CHECK(server::FromBase32("not base32!").empty());
    std::int64_t step = 0;
    CHECK(server::VerifyTotp(key, server::TotpCode(key, 1000), 1000 + 30, step)); // a step late is still good
    CHECK(step == 1000 / 30);
    CHECK_FALSE(server::VerifyTotp(key, server::TotpCode(key, 1000), 1000 + 90, step));
}

namespace {

struct Admin {
    std::string name, password;
    std::vector<std::uint8_t> secret;
};

std::string Code(const Admin& a) { return server::TotpCode(a.secret, std::int64_t(std::time(nullptr))); }

// A logged-in browser: its cookie and second token.
struct Browser {
    httplib::Client* client = nullptr;
    std::string cookie, csrf;
    httplib::Headers Headers(bool write = true) const {
        httplib::Headers h{{"Cookie", cookie}};
        if (write) h.emplace("X-CSRF-Token", csrf);
        return h;
    }
    httplib::Result Send(const char* method, const std::string& path, const json& body = json::object(), bool write = true) const {
        const std::string text = body.dump();
        if (std::string_view(method) == "GET") return client->Get(path, Headers(false));
        if (std::string_view(method) == "PUT") return client->Put(path, Headers(write), text, "application/json");
        if (std::string_view(method) == "DELETE") return client->Delete(path, Headers(write), text, "application/json");
        return client->Post(path, Headers(write), text, "application/json");
    }
};

std::pair<int, json> Login(httplib::Client& client, const Admin& a, const std::string& code, Browser* into = nullptr) {
    const auto r = client.Post("/api/admin/login", json{{"name", a.name}, {"password", a.password}, {"code", code}}.dump(), "application/json");
    REQUIRE(r);
    const json body = json::parse(r->body, nullptr, false);
    if (into != nullptr && r->status == 200) {
        const std::string set = r->get_header_value("Set-Cookie");
        CHECK(set.find("HttpOnly") != std::string::npos);
        CHECK(set.find("SameSite=Strict") != std::string::npos);
        CHECK(set.find("Path=/api/admin") != std::string::npos);
        into->client = &client;
        into->cookie = set.substr(0, set.find(';'));
        into->csrf = body["csrf"];
    }
    return {r->status, body};
}

} // namespace

TEST_CASE("Web admin: logins with a password and a code, sessions, the page's token, roles, and what each may change") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    server::Database& db = t.Db();
    REQUIRE_MESSAGE(server::MigrateSchema(db, error), error);
    const auto make = [&](const char* name, const char* role) {
        Admin a{name, std::string(name) + "-long-password", {}};
        std::string secret;
        REQUIRE_MESSAGE(server::MakeAdmin(db, a.name, role, a.password, secret, error), error);
        a.secret = server::FromBase32(secret);
        REQUIRE(a.secret.size() == 20);
        return a;
    };
    const Admin boss = make("boss", "admin"), mod = make("helper", "moderator"), victim = make("victim", "admin");
    std::string unused;
    CHECK_FALSE(server::MakeAdmin(db, "short", "admin", "tooshort", unused, error)); // a short password
    CHECK(server::AuthenticatorUri("boss", "ABC").starts_with("otpauth://totp/SroReimagined:boss?secret=ABC"));

    const std::filesystem::path files = std::filesystem::temp_directory_path() / ("sr-web-files-" + std::to_string(std::time(nullptr)));
    server::WebConfig c;
    c.databaseUrl = t.Url();
    c.port = 0;
    c.filesDir = files.string();
    c.secureCookie = false;
    c.adminLoginAttempts = 100; // this test logs in often from one address (the limit has its own test)
    server::WebServer web;
    REQUIRE_MESSAGE(web.Start(c, error), error);
    httplib::Client client("127.0.0.1", web.Port());
    client.set_read_timeout(30s);

    // Without a session: nothing.
    REQUIRE(client.Get("/api/admin/me"));
    CHECK(client.Get("/api/admin/me")->status == 401);
    // A wrong code, a wrong password: the same answer.
    CHECK(Login(client, boss, "000000").second["error"] == "wrong");
    CHECK(Login(client, Admin{boss.name, "wrong-password-x", boss.secret}, Code(boss)).first == 401);
    // Right: a session.
    Browser b;
    const std::string code = Code(boss);
    REQUIRE(Login(client, boss, code, &b).first == 200);
    CHECK(Login(client, boss, code).first == 401); // a code is good once
    auto me = b.Send("GET", "/api/admin/me");
    REQUIRE(me);
    CHECK(json::parse(me->body)["role"] == "admin");
    CHECK(db.Query("SELECT count(*) FROM sro_vt_account.sr_web_sessions").Int(0, 0) == 1);
    CHECK(db.Query("SELECT count(*) FROM sro_vt_account.sr_web_sessions WHERE token_hash = $1", {server::Param::Bytes(std::vector<std::uint8_t>(32))}).Int(0, 0) == 0);

    // A change needs the page's token and this site's origin.
    const json texts = {{"links", {{"discord", "https://discord.gg/example"}}}, {"hero", {{"title", "Silkroad <script>alert(1)</script>"}}}};
    CHECK(client.Put("/api/admin/site", {{"Cookie", b.cookie}}, json{{"content", texts}}.dump(), "application/json")->status == 403);
    CHECK(client.Put("/api/admin/site", {{"Cookie", b.cookie}, {"X-CSRF-Token", b.csrf}, {"Origin", "https://evil.example"}}, json{{"content", texts}}.dump(),
                     "application/json")
              ->status == 403);
    CHECK(b.Send("PUT", "/api/admin/site", {{"content", texts}})->status == 200);
    CHECK(b.Send("PUT", "/api/admin/site", {{"content", {{"links", {{"discord", "javascript:alert(1)"}}}}}})->status == 400);
    // The public site shows them (as data: the page renders text, never HTML).
    json site;
    for (int i = 0; i < 100; ++i) {
        site = json::parse(client.Get("/api/site")->body);
        if (site["content"].contains("hero")) break;
        std::this_thread::sleep_for(20ms);
    }
    CHECK(site["content"]["links"]["discord"] == "https://discord.gg/example");

    // News: added, edited, hidden, deleted; only the visible on the site.
    auto added = b.Send("POST", "/api/admin/news", {{"date", "2026-11-12"}, {"tag", "Update"}, {"title", "The beta opens"}, {"text", "Come in."}});
    REQUIRE(added);
    REQUIRE(added->status == 201);
    const std::int64_t news = json::parse(added->body)["id"];
    CHECK(b.Send("POST", "/api/admin/news", {{"date", "2026-13-40"}, {"tag", "x"}, {"title", "x"}, {"text", ""}})->status == 400);
    CHECK(b.Send("PUT", "/api/admin/news/" + std::to_string(news), {{"date", "2026-11-12"}, {"tag", "Update"}, {"title", "Hidden"}, {"text", "."}, {"visible", false}})->status == 200);
    for (int i = 0; i < 100; ++i) {
        site = json::parse(client.Get("/api/site")->body);
        if (site["news"].empty()) break;
        std::this_thread::sleep_for(20ms);
    }
    CHECK(site["news"].empty());
    CHECK(json::parse(b.Send("GET", "/api/admin/news")->body)["news"].size() == 1);
    CHECK(b.Send("DELETE", "/api/admin/news/" + std::to_string(news))->status == 200);

    // Uploads: images only, by their first bytes; stored under random names, served from /files. (The client is SroDeploy's.)
    const std::string png = std::string("\x89PNG\r\n\x1A\n", 8) + std::string(100, 'x');
    auto upload = client.Post("/api/admin/files?kind=image&name=hero.png&label=Jangan", b.Headers(), png, "application/octet-stream");
    REQUIRE(upload);
    REQUIRE(upload->status == 201);
    const std::string imageUrl = json::parse(upload->body)["url"];
    CHECK(imageUrl.ends_with(".png"));
    CHECK(imageUrl.find("hero") == std::string::npos);
    const auto served = client.Get(imageUrl);
    REQUIRE(served);
    CHECK(served->body == png);
    CHECK(client.Post("/api/admin/files?kind=image&name=evil.png", b.Headers(), "<svg onload=alert(1)>", "application/octet-stream")->status == 400);
    CHECK(client.Post("/api/admin/files?kind=client&name=c.zip", b.Headers(), "PK\x03\x04", "application/octet-stream")->status == 400);
    CHECK(client.Post("/api/admin/files?kind=image&name=h.png", Browser{&client, b.cookie, "wrong"}.Headers(), png, "application/octet-stream")->status == 403);
    for (int i = 0; i < 100; ++i) {
        site = json::parse(client.Get("/api/site")->body);
        if (site["images"].size() == 1) break;
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(site["images"].size() == 1);
    CHECK(site["images"][0]["label"] == "Jangan");
    CHECK(site["client"].is_null()); // no web-patch-dir

    // Players: a search, a ban (with the game servers told to put it out), its lifting, a new password.
    REQUIRE(server::CreateAccount(db, "player1", "secret1", error) == server::CreateAccountResult::Created);
    auto found = json::parse(b.Send("GET", "/api/admin/players?q=play")->body);
    REQUIRE(found["players"].size() == 1);
    CHECK(found["players"][0]["blocked"] == false);
    auto bus = server::Database::Connect(server::WithSearchPath(t.Url(), "sro_vt_shard, sro_vt_account"), error);
    REQUIRE(bus);
    REQUIRE(bus->Listen("sr_admin_sro_vt_shard"));
    CHECK(b.Send("POST", "/api/admin/players/ban", {{"account", "player1"}, {"reason", "botting"}, {"days", 7}})->status == 200);
    bool ok = false;
    CHECK(server::FindAccount(db, "player1", ok)->blocked);
    CHECK(db.Query("SELECT count(*) FROM sro_vt_account.sr_admin_commands WHERE kind = 'kick' AND text = 'player1'").Int(0, 0) == 1);
    CHECK(b.Send("POST", "/api/admin/players/unban", {{"account", "player1"}})->status == 200);
    CHECK_FALSE(server::FindAccount(db, "player1", ok)->blocked);
    CHECK(b.Send("POST", "/api/admin/players/password", {{"account", "player1"}, {"password", "brand-new-1"}})->status == 200);
    CHECK(server::VerifyPassword("brand-new-1", server::FindAccount(db, "player1", ok)->passwordHash));
    CHECK_FALSE(server::VerifyPassword("secret1", server::FindAccount(db, "player1", ok)->passwordHash));

    // The moderator: players and announcements, not the site, not the console, not passwords.
    Browser m;
    REQUIRE(Login(client, mod, Code(mod), &m).first == 200);
    CHECK(m.Send("PUT", "/api/admin/site", {{"content", texts}})->status == 403);
    CHECK(m.Send("POST", "/api/admin/console", {{"text", "/notice hi"}})->status == 403);
    CHECK(m.Send("POST", "/api/admin/players/password", {{"account", "player1"}, {"password", "another-1"}})->status == 403);
    // Online: player1 in the world as its character (the game servers set the session's character).
    REQUIRE(db.Query("INSERT INTO sro_vt_shard._char (charname16, scale, posx, posy, posz, lastlogout) VALUES ('Walker', 0, 0, 0, 0, now())").Ok());
    REQUIRE(db.Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id, character_id) SELECT u.jid, 2, c.charid FROM sro_vt_account.tb_user u, "
                     "sro_vt_shard._char c WHERE u.struserid = 'player1' AND c.charname16 = 'Walker'")
                .Ok());
    const json online = json::parse(m.Send("GET", "/api/admin/players/online")->body)["online"];
    REQUIRE(online.size() == 1);
    CHECK(online[0]["character"] == "Walker");
    CHECK(online[0]["server"] == 2);
    auto announced = m.Send("POST", "/api/admin/announce", {{"text", "The server restarts in 10 minutes."}, {"server", 0}});
    REQUIRE(announced);
    CHECK(announced->status == 200);
    CHECK(b.Send("POST", "/api/admin/console", {{"text", "/item ITEM_ETC_HP_POTION_01 10"}, {"server", 2}})->status == 200);
    // The game servers hear each on the bus (sr_admin_<schema>: the command's id) and read it from sr_admin_commands.
    std::vector<server::Database::Notification> heard;
    for (int i = 0; i < 100 && heard.size() < 3; ++i) {
        for (auto& n : bus->TakeNotifications()) heard.push_back(std::move(n));
        std::this_thread::sleep_for(20ms);
    }
    CHECK(heard.size() == 3); // the kick, the announcement, the command
    const json commands = json::parse(b.Send("GET", "/api/admin/commands")->body)["commands"];
    REQUIRE(commands.size() == 3);
    CHECK(commands[0]["kind"] == "console");
    CHECK(commands[0]["server"] == 2);
    CHECK(commands[1]["kind"] == "announce");
    CHECK(commands[1]["by"] == "helper");

    // What was done, by whom.
    const json audit = json::parse(b.Send("GET", "/api/admin/audit")->body)["audit"];
    std::set<std::string> actions;
    for (const json& a : audit) actions.insert(a["action"].get<std::string>());
    for (const char* a : {"login", "login-refused", "site-texts", "news-add", "news-edit", "news-delete", "upload", "ban", "unban", "player-password", "announce", "console"})
        CHECK_MESSAGE(actions.contains(a), a);
    CHECK(m.Send("GET", "/api/admin/audit")->status == 403);

    // Five wrong in a row lock an account: even the right password and code are refused then.
    for (int i = 0; i < 5; ++i) Login(client, Admin{victim.name, "wrong-password-x", victim.secret}, Code(victim));
    CHECK(Login(client, victim, Code(victim)).first == 401);
    CHECK(db.Query("SELECT locked_until > now() FROM sro_vt_account.sr_web_admins WHERE name = 'victim'").Bool(0, 0));

    // Logged out: the session is gone.
    CHECK(b.Send("POST", "/api/admin/logout")->status == 200);
    CHECK(b.Send("GET", "/api/admin/me")->status == 401);
    // A large body anywhere but an upload is refused before it is read.
    CHECK(client.Post("/api/register", std::string(100 * 1024, 'x'), "application/json")->status == 413);

    web.Stop();
    std::error_code ec;
    std::filesystem::remove_all(files, ec);
}

TEST_CASE("Web admin: the allowed addresses: exact, ranges, IPv6, this machine however it is reached") {
    CHECK(server::AddressAllowed({}, "198.51.100.7"));
    CHECK(server::AddressAllowed({"127.0.0.1"}, "127.0.0.1"));
    CHECK(server::AddressAllowed({"127.0.0.1"}, "::1"));            // a browser on the server opening localhost
    CHECK(server::AddressAllowed({"127.0.0.1"}, "::ffff:127.0.0.1"));
    CHECK(server::AddressAllowed({"::1"}, "127.0.0.1"));
    CHECK(server::AddressAllowed({"localhost"}, "::1"));
    CHECK_FALSE(server::AddressAllowed({"127.0.0.1"}, "198.51.100.7"));
    CHECK(server::AddressAllowed({"127.0.0.1", "198.51.100.7"}, "198.51.100.7"));
    CHECK(server::AddressAllowed({"10.0.0.0/8"}, "::ffff:10.20.30.40"));
    CHECK_FALSE(server::AddressAllowed({"10.0.0.0/8"}, "11.0.0.1"));
    CHECK(server::AddressAllowed({"198.51.100.0/24"}, "198.51.100.250"));
    CHECK(server::AddressAllowed({"2a02:8070:1234::/48"}, "2a02:8070:1234:5:6:7:8:9"));
    CHECK_FALSE(server::AddressAllowed({"2a02:8070:1234::/48"}, "2a02:8070:1235::1"));
    CHECK(server::AddressAllowed({"2001:db8::7"}, "2001:0db8:0000::0007")); // the same address written another way
    CHECK_FALSE(server::AddressAllowed({"2001:db8::7"}, "198.51.100.7"));
    CHECK_FALSE(server::AddressAllowed({"bogus", "10.0.0.0/99"}, "10.1.1.1"));
}

TEST_CASE("Web admin: the client's download is SroDeploy's zip, its VirusTotal link automatic or set") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error, secret;
    REQUIRE_MESSAGE(server::MigrateSchema(t.Db(), error), error);
    REQUIRE(server::MakeAdmin(t.Db(), "boss", "admin", "boss-long-password", secret, error));
    const Admin boss{"boss", "boss-long-password", server::FromBase32(secret)};
    const std::filesystem::path patch = std::filesystem::temp_directory_path() / "sr-web-patch";
    std::filesystem::remove_all(patch);
    std::filesystem::create_directories(patch / "windows-x64");
    const std::string sha(64, 'a');
    {
        std::ofstream out(patch / "windows-x64" / "client.json");
        out << json{{"version", "0.1.2"}, {"channel", "windows-x64"}, {"file", "SroReimagined-0.1.2.zip"}, {"size", 8434674},
                    {"unpacked", 21753678}, {"files", 12}, {"sha256", sha}, {"created", "2026-11-22T02:43:21Z"}}
                   .dump();
    }
    server::WebConfig c;
    c.databaseUrl = t.Url();
    c.port = 0;
    c.filesDir = (std::filesystem::temp_directory_path() / "sr-web-files-client").string();
    c.secureCookie = false;
    c.patchDir = patch.string();
    server::WebServer web;
    REQUIRE_MESSAGE(web.Start(c, error), error);
    httplib::Client client("127.0.0.1", web.Port());
    json site = json::parse(client.Get("/api/site")->body);
    REQUIRE(site["client"].is_object());
    CHECK(site["client"]["version"] == "0.1.2");
    CHECK(site["client"]["url"] == "/patch/windows-x64/SroReimagined-0.1.2.zip");
    CHECK(site["client"]["size"] == 8434674);
    CHECK(site["client"]["sha256"] == sha);
    CHECK(site["client"]["virustotal"] == "https://www.virustotal.com/gui/file/" + sha);
    CHECK(site["client"]["virustotalAuto"] == true);

    Browser b;
    REQUIRE(Login(client, boss, Code(boss), &b).first == 200);
    CHECK(b.Send("PUT", "/api/admin/client", {{"virustotal", "https://evil.example/scan"}})->status == 400);
    const std::string link = "https://www.virustotal.com/gui/file-analysis/abc";
    CHECK(b.Send("PUT", "/api/admin/client", {{"virustotal", link}})->status == 200);
    for (int i = 0; i < 100; ++i) {
        site = json::parse(client.Get("/api/site")->body);
        if (site["client"]["virustotal"] == link) break;
        std::this_thread::sleep_for(20ms);
    }
    CHECK(site["client"]["virustotal"] == link);
    CHECK(site["client"]["virustotalAuto"] == false);
    CHECK(json::parse(b.Send("GET", "/api/admin/client")->body)["client"]["virustotal"] == link);

    // A new version: the link set by hand was for the file before, so the new one's is automatic again.
    {
        std::ofstream out(patch / "windows-x64" / "client.json");
        out << json{{"version", "0.1.3"}, {"file", "SroReimagined-0.1.3.zip"}, {"size", 1}, {"sha256", std::string(64, 'b')}}.dump();
    }
    CHECK(json::parse(b.Send("GET", "/api/admin/client")->body)["client"]["virustotalAuto"] == true);
    // A name that is not a plain file's: no download rather than a link elsewhere.
    {
        std::ofstream out(patch / "windows-x64" / "client.json");
        out << json{{"version", "0.1.4"}, {"file", "../../secret.zip"}, {"size", 1}, {"sha256", sha}}.dump();
    }
    CHECK(json::parse(b.Send("GET", "/api/admin/client")->body)["client"].is_null());
    web.Stop();
    std::filesystem::remove_all(patch);
}

TEST_CASE("Web admin: the API answers only the listed addresses; the address limit on logins") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error, secret;
    REQUIRE_MESSAGE(server::MigrateSchema(t.Db(), error), error);
    REQUIRE(server::MakeAdmin(t.Db(), "boss", "admin", "boss-long-password", secret, error));
    const Admin boss{"boss", "boss-long-password", server::FromBase32(secret)};
    server::WebConfig c;
    c.databaseUrl = t.Url();
    c.port = 0;
    c.filesDir = (std::filesystem::temp_directory_path() / "sr-web-files-allow").string();
    c.secureCookie = false;
    SUBCASE("not listed") {
        c.adminAllow = {"10.0.0.0/8", "192.168.1.5"};
        server::WebServer web;
        REQUIRE_MESSAGE(web.Start(c, error), error);
        httplib::Client client("127.0.0.1", web.Port());
        CHECK(Login(client, boss, Code(boss)).first == 403);
    }
    SUBCASE("listed") {
        c.adminAllow = {"127.0.0.0/8"};
        server::WebServer web;
        REQUIRE_MESSAGE(web.Start(c, error), error);
        httplib::Client client("127.0.0.1", web.Port());
        // Ten tries from one address in 15 minutes, then none (even the right one).
        for (int i = 0; i < 10; ++i) CHECK(Login(client, boss, "000000").first == 401);
        CHECK(Login(client, boss, Code(boss)).first == 429);
    }
}
