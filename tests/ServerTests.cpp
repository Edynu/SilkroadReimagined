#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sr/core/Jobs.h"
#include "sr/game/TitleProtocol.h"
#include "sr/net/Connection.h"
#include "sr/server/Database.h"
#include "sr/server/ClientGate.h"
#include "sr/server/GatewayServer.h"
#include "sr/server/LoginServer.h"
#include "sr/server/Service.h"
#include "sr/server/Store.h"
#include "ServerTestSupport.h"

using namespace std::chrono_literals;
using namespace sr;
namespace proto = sr::game::proto;

using namespace sr::test;

TEST_CASE("Server database: the original's layout migrates once, then stays; a second shard gets its own schema") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CHECK(server::SchemaVersion(t.Db()) == server::LatestSchemaVersion());
    CHECK(server::ShardSchemaVersion(t.Db()) == server::LatestShardSchemaVersion());
    std::string error;
    CHECK(server::MigrateSchema(t.Db(), error));
    CHECK(server::SchemaVersion(t.Db()) == server::LatestSchemaVersion());
    CHECK(server::LatestSchemaVersion() == 16);     // the original's account tables + ours; 3: sr_beta_keys; 4: the website's administration; 5: the session's character; 6: the site's players; 7: the shard's capacity; 8: the session's token; 9: silk (sk_silk and its logs, the starting silk); 10: the skill lab's tables; 11: its RE notes by side, notes by aspect, the files check; 12: sr_server_samples; 13: sr_lab_videos; 14: sr_lab_items; 15: the support tickets; 16: a ticket file kept in the database
    CHECK(server::LatestShardSchemaVersion() == 30); // the original's shard tables + ours; 30: sr_bargain_lock (out of _timedjob); 28: _openmarket (the Open Market); 27: sr_academy_matching (the Academy's matching list); 26: the Academy's tables (_trainingcamp...); 25: _timedjobforpet (the pets' timed jobs); 24: sr_event_entries, _flagworld_eventparticipants (the events); 22: _inventoryforlinkedstorage (the magic cubes); 20: _siegefortressobject and the forges' (the fortress war); 19: _charnicknamelist (the jobs' aliases); 18: sr_auto_potion's rows of ours; 17: _bindingoptionwithitem again (merged: both lines of work took 15); 16: _charcos, _invcos (the pets); 15: _bindingoptionwithitem; 14: sr_unique_events; 2: _charquest, buffs in _timedjob; 3: the guilds' tables; 4: sr_crests; 5: sr_auto_potion; 6: sr_guild_storage_lock; 7: _guildwar; 8: the fortress war's tables; 9: the guild window's; 10: friends, blocks, letters; 11: _rentiteminfo; 12: sr_quests; 13: the collection book, the instance entries
    CHECK(t.Db().Query("SELECT to_regclass('sro_vt_shard._charquest') IS NOT NULL").Bool(0, 0));
    CHECK(t.Db().Query("SELECT to_regclass('sro_vt_shard._charcos') IS NOT NULL").Bool(0, 0));
    CHECK_FALSE(t.Db().Query("SELECT to_regclass('sro_vt_shard.sr_char_buffs') IS NOT NULL").Bool(0, 0));
    for (const char* table : {"sro_vt_account.tb_user", "sro_vt_account._shard", "sro_vt_account._notice", "sro_vt_account._blockeduser",
                              "sro_vt_account._punishment", "sro_vt_account.sr_account_security", "sro_vt_account.sr_login_tickets",
                              "sro_vt_account.sr_online_sessions", "sro_vt_account.sr_shard_reports", "sro_vt_account.sr_beta_keys", "sro_vt_account.sr_web_admins", "sro_vt_account.sr_web_sessions", "sro_vt_account.sr_web_audit",
                              "sro_vt_account.sr_site_content", "sro_vt_account.sr_site_news", "sro_vt_account.sr_site_files", "sro_vt_account.sr_admin_commands", "sro_vt_shard._char", "sro_vt_shard._user",
                              "sro_vt_shard._items", "sro_vt_shard._inventory", "sro_vt_shard._itempool", "sro_vt_shard._chest", "sro_vt_shard._charskill",
                              "sro_vt_shard._charskillmastery", "sro_vt_shard._clientconfig", "sro_vt_shard.sr_char_state",
                              "sro_vt_shard._guild", "sro_vt_shard._guildmember", "sro_vt_shard._alliedclans", "sro_vt_shard._guildchest", "sro_vt_shard.sr_crests", "sro_vt_shard.sr_auto_potion", "sro_vt_shard.sr_guild_storage_lock", "sro_vt_shard._guildwar"}) {
        CAPTURE(table);
        CHECK(t.Db().Query("SELECT to_regclass($1) IS NOT NULL", {table}).Bool(0, 0));
    }
    // The original's placeholder rows: item 0 (an empty place points at it) and character 0.
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_shard._items WHERE id64 = 0").Int(0, 0) == 1);
    CHECK(t.Db().Query("SELECT charname16 FROM sro_vt_shard._char WHERE charid = 0").Text(0, 0) == "dummy");
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_shard._latestitemserial").Int(0, 0) == 1);
    // Another shard: the same tables in its own schema, the accounts shared.
    REQUIRE_MESSAGE(server::MigrateSchema(t.Db(), "sro_vt_shard2", error), error);
    CHECK(server::ShardSchemaVersion(t.Db(), "sro_vt_shard2") == server::LatestShardSchemaVersion());
    CHECK(t.Db().Query("SELECT to_regclass('sro_vt_shard2._char') IS NOT NULL").Bool(0, 0));
    CHECK_FALSE(server::MigrateSchema(t.Db(), "Robert'); DROP TABLE x; --", error));
}

TEST_CASE("Server database: accounts, wrong passwords and blocks") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    auto& db = t.Db();
    std::string error;
    CHECK(server::CreateAccount(db, "Tester", "secret1", error) == server::CreateAccountResult::Created);
    CHECK(server::CreateAccount(db, "tester", "secret2", error) == server::CreateAccountResult::NameTaken);
    CHECK(server::CreateAccount(db, "no", "secret1", error) == server::CreateAccountResult::InvalidName);
    CHECK(server::CreateAccount(db, "bad name", "secret1", error) == server::CreateAccountResult::InvalidName);
    CHECK(server::CreateAccount(db, "Other", "short", error) == server::CreateAccountResult::InvalidPassword);

    bool ok = false;
    auto account = server::FindAccount(db, "TESTER", ok);
    REQUIRE(ok);
    REQUIRE(account);
    CHECK(account->name == "Tester");
    CHECK_FALSE(account->blocked);
    CHECK(server::FindAccount(db, "nobody", ok) == std::nullopt);
    CHECK(ok);

    for (int i = 1; i < 3; ++i) CHECK(server::RecordWrongPassword(db, account->id, 3, 60, "too many") == i);
    CHECK(server::RecordWrongPassword(db, account->id, 3, 60, "too many") == 3);
    account = server::FindAccount(db, "tester", ok);
    CHECK(account->blocked);
    CHECK(account->failedLogins == 0);
    CHECK(account->blockReason == "too many");
    CHECK(account->blockedUntil > 0);

    CHECK(server::SetBlock(db, "tester", "", 0));
    CHECK_FALSE(server::FindAccount(db, "tester", ok)->blocked);
    CHECK(server::SetBlock(db, "tester", "cheating", 0));
    account = server::FindAccount(db, "tester", ok);
    CHECK(account->blocked);
    CHECK(account->blockedUntil == 0); // no end
    CHECK_FALSE(server::SetBlock(db, "nobody", "x", 0));
}

TEST_CASE("Server database: game server reports and tickets") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    auto& db = t.Db();
    std::string error;
    REQUIRE(server::ReportGameServer(db, TestServer(1), error));
    REQUIRE(server::ReportGameServer(db, TestServer(2), error));
    bool ok = false;
    auto servers = server::ListGameServers(db, 15, ok);
    REQUIRE(servers.size() == 2);
    CHECK(servers[0].reporting);
    CHECK(server::StopGameServer(db, 2));
    servers = server::ListGameServers(db, 15, ok);
    CHECK(servers[0].reporting);
    CHECK_FALSE(servers[1].reporting);
    // A report older than the limit is offline too.
    db.Query("UPDATE sro_vt_account.sr_shard_reports SET reported_at = now() - interval '1 minute' WHERE shard_id = 1");
    CHECK_FALSE(server::ListGameServers(db, 15, ok)[0].reporting);

    REQUIRE(server::CreateAccount(db, "ticketer", "secret1", error) == server::CreateAccountResult::Created);
    const std::int64_t account = server::FindAccount(db, "ticketer", ok)->id;
    // Once, for its server, within its time.
    auto ticket = server::IssueTicket(db, account, 1, 30, error);
    REQUIRE(ticket.size() == 32);
    CHECK(server::ConsumeTicket(db, ticket, 1) == account);
    CHECK(server::ConsumeTicket(db, ticket, 1) == std::nullopt);
    ticket = server::IssueTicket(db, account, 1, 30, error);
    CHECK(server::ConsumeTicket(db, ticket, 2) == std::nullopt); // the wrong server burns it
    CHECK(server::ConsumeTicket(db, ticket, 1) == std::nullopt);
    ticket = server::IssueTicket(db, account, 1, 0, error);
    CHECK(server::ConsumeTicket(db, ticket, 1) == std::nullopt); // expired
    CHECK(server::IssueTicket(db, account, 1, 0, error).size() == 32);
    CHECK(server::PurgeTickets(db) == 1);
    // Only the hash is stored.
    ticket = server::IssueTicket(db, account, 1, 30, error);
    CHECK(db.Query("SELECT count(*) FROM sro_vt_account.sr_login_tickets WHERE token_hash = $1", {server::Param::Bytes(ticket)}).Int(0, 0) == 0);
}

TEST_CASE("Servers: the gateway lists the game servers and where to log in") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    REQUIRE(server::ReportGameServer(t.Db(), TestServer(1), error));
    auto full = TestServer(2, 50, 50);
    full.schema = "sro_vt_shard2"; // another shard: its own row
    REQUIRE(server::ReportGameServer(t.Db(), full, error));
    REQUIRE(server::PostNotice(t.Db(), "Welcome", "First notice"));

    server::GatewayServer gateway;
    server::GatewayConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.loginPort = 15780;
    config.refresh = 50ms;
    REQUIRE_MESSAGE(gateway.Start(config, error), error);

    net::NetService net(1);
    Client client(net, gateway.Port(), [&] { gateway.Poll(2ms); });
    REQUIRE(client.Greet());
    client.Send(proto::ServerListRequest{});
    proto::ServerList list;
    REQUIRE(client.Expect(list));
    REQUIRE(list.servers.size() == 2);
    CHECK(list.servers[0].name == "Server 1");
    CHECK(list.servers[0].state == proto::ServerState::Open);
    CHECK(list.servers[0].online == 3);
    CHECK(list.servers[0].capacity == 100);
    CHECK(list.servers[1].state == proto::ServerState::Full);
    REQUIRE(list.groups.size() == 1);
    CHECK(list.groups[0].name == "Silkroad");
    CHECK(list.loginPort == 15780);

    client.Send(proto::NoticesRequest{});
    proto::Notices notices;
    REQUIRE(client.Expect(notices));
    REQUIRE(notices.notices.size() == 1);
    CHECK(notices.notices[0].title == "Welcome");

    // A stopped game server shows as offline after the next refresh.
    REQUIRE(server::StopGameServer(t.Db(), 1));
    bool offline = false;
    for (int i = 0; i < 100 && !offline; ++i) {
        std::this_thread::sleep_for(10ms);
        client.Send(proto::ServerListRequest{});
        REQUIRE(client.Expect(list));
        offline = list.servers[0].state == proto::ServerState::Offline;
    }
    CHECK(offline);
}

TEST_CASE("Servers: a client must greet first, with our protocol") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    server::GatewayServer gateway;
    server::GatewayConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.peers.helloTimeout = 200ms;
    REQUIRE_MESSAGE(gateway.Start(config, error), error);
    net::NetService net(1);
    const auto pump = [&] { gateway.Poll(2ms); };

    Client rude(net, gateway.Port(), pump);
    rude.Send(proto::ServerListRequest{});
    CHECK(rude.WaitClosed());

    Client old(net, gateway.Port(), pump);
    CHECK_FALSE(old.Greet(proto::kProtocolVersion + 1));
    CHECK(old.WaitClosed());

    Client silent(net, gateway.Port(), pump);
    CHECK(silent.WaitClosed(5s)); // the hello timeout
}

TEST_CASE("Servers: logging in") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    REQUIRE(server::CreateAccount(t.Db(), "player", "secret1", error) == server::CreateAccountResult::Created);
    REQUIRE(server::CreateAccount(t.Db(), "banned", "secret1", error) == server::CreateAccountResult::Created);
    REQUIRE(server::SetBlock(t.Db(), "banned", "cheating", 3600));
    // Three shards of one game server each (one shard's servers are chosen among: ShardTests.cpp).
    const auto shard = [](server::GameServerInfo s, const char* schema) {
        s.schema = schema;
        return s;
    };
    REQUIRE(server::ReportGameServer(t.Db(), TestServer(1), error));
    REQUIRE(server::ReportGameServer(t.Db(), shard(TestServer(2, 10, 10), "sro_vt_shard2"), error)); // full
    REQUIRE(server::ReportGameServer(t.Db(), shard(TestServer(3), "sro_vt_shard3"), error));
    REQUIRE(server::StopGameServer(t.Db(), 3)); // offline
    // A shard of two game servers with room each, but the shard's capacity (both together) reached.
    for (std::uint16_t id : {std::uint16_t(4), std::uint16_t(5)}) {
        server::GameServerInfo s = shard(TestServer(id, 5, 3000), "sro_vt_shard4");
        s.shardCapacity = 10;
        REQUIRE(server::ReportGameServer(t.Db(), s, error));
    }

    core::JobSystem jobs(2);
    server::LoginServer login;
    server::LoginConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.jobs = &jobs;
    config.attemptLimit = 3;
    config.addressAttempts = 100;
    REQUIRE_MESSAGE(login.Start(config, error), error);
    net::NetService net(1);
    const auto pump = [&] { login.Poll(2ms); };

    Client c(net, login.Port(), pump);
    REQUIRE(c.Greet());
    const auto attempt = [&](const std::string& account, const std::string& password, std::uint16_t server) {
        proto::LoginRequest request;
        request.account = account;
        request.password = password;
        request.server = server;
        c.Send(request);
        proto::LoginResult result;
        REQUIRE(c.Expect(result));
        return result;
    };

    // No such account: answered as a wrong password (which names exist is not told).
    proto::LoginResult r = attempt("nobody", "secret1", 1);
    CHECK(r.status == proto::LoginStatus::WrongPassword);
    CHECK(r.attempts == 1);
    CHECK(attempt("x", "secret1", 1).status == proto::LoginStatus::Invalid);
    r = attempt("player", "wrong!!", 1);
    CHECK(r.status == proto::LoginStatus::WrongPassword);
    CHECK(r.attempts == 1);
    CHECK(r.attemptLimit == 3);
    // A block is told only to whoever knows the password.
    CHECK(attempt("banned", "wrong!!", 1).status == proto::LoginStatus::WrongPassword);
    r = attempt("banned", "secret1", 1);
    CHECK(r.status == proto::LoginStatus::Blocked);
    CHECK(r.reason == "cheating");
    CHECK(r.blockedUntil > 0);
    CHECK(attempt("player", "secret1", 2).status == proto::LoginStatus::ServerFull);
    CHECK(attempt("player", "secret1", 4).status == proto::LoginStatus::ServerFull); // the shard is, its servers are not
    CHECK(attempt("player", "secret1", 3).status == proto::LoginStatus::ServerUnavailable);
    CHECK(attempt("player", "secret1", 9).status == proto::LoginStatus::ServerUnavailable);

    // The right password: the game server's address and a ticket it takes once.
    r = attempt("PLAYER", "secret1", 1);
    REQUIRE(r.status == proto::LoginStatus::Accepted);
    CHECK(r.host == "127.0.0.1");
    CHECK(r.port == 16001);
    REQUIRE(r.ticket.size() == proto::kTicketBytes);
    CHECK(c.WaitClosed()); // the login server is done with this client
    bool ok = false;
    const std::int64_t player = server::FindAccount(t.Db(), "player", ok)->id;
    CHECK(server::ConsumeTicket(t.Db(), r.ticket, 1) == player);
    CHECK(server::ConsumeTicket(t.Db(), r.ticket, 1) == std::nullopt);
    // The good login reset the count of wrong passwords.
    CHECK(server::FindAccount(t.Db(), "player", ok)->failedLogins == 0);

    // Three wrong passwords in a row make that address wait; the account is not blocked (nobody else can lock its
    // owner out), and that address cannot try it again, not even with the right password, until the wait is over.
    Client d(net, login.Port(), pump);
    REQUIRE(d.Greet());
    const auto wrong = [&] {
        proto::LoginRequest request;
        request.account = "player";
        request.password = "nope!!!";
        request.server = 1;
        d.Send(request);
        proto::LoginResult result;
        REQUIRE(d.Expect(result));
        return result;
    };
    CHECK(wrong().attempts == 1);
    CHECK(wrong().attempts == 2);
    CHECK(wrong().status == proto::LoginStatus::TooManyAttempts);
    CHECK(wrong().status == proto::LoginStatus::TooManyAttempts);
    CHECK_FALSE(server::FindAccount(t.Db(), "player", ok)->blocked);
    {
        proto::LoginRequest request;
        request.account = "Player";
        request.password = "secret1";
        request.server = 1;
        d.Send(request);
        proto::LoginResult result;
        REQUIRE(d.Expect(result));
        CHECK(result.status == proto::LoginStatus::TooManyAttempts);
    }
    const auto stats = login.GetStats();
    CHECK(stats.accepted == 1);
    login.Stop();
}

TEST_CASE("Servers: attempts per address are limited") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    core::JobSystem jobs(1);
    server::LoginServer login;
    server::LoginConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.jobs = &jobs;
    config.addressAttempts = 3;
    config.attemptLimit = 10; // the address's limit, not the name's
    REQUIRE_MESSAGE(login.Start(config, error), error);
    net::NetService net(1);
    Client c(net, login.Port(), [&] { login.Poll(2ms); });
    REQUIRE(c.Greet());
    proto::LoginRequest request;
    request.account = "nobody";
    request.password = "secret1";
    request.server = 1;
    for (int i = 0; i < 3; ++i) {
        c.Send(request);
        proto::LoginResult r;
        REQUIRE(c.Expect(r));
        CHECK(r.status == proto::LoginStatus::WrongPassword);
    }
    c.Send(request);
    proto::LoginResult r;
    REQUIRE(c.Expect(r));
    CHECK(r.status == proto::LoginStatus::TooManyAttempts);
}

TEST_CASE("Servers: a peer that floods is closed, one that throws is closed alone; others go on") {
    // No database: Peers over a bare listener (Service.h), as every server's loop uses it.
    net::NetService net(1);
    std::string error;
    const auto listener = net.Listen(Local(), error);
    REQUIRE_MESSAGE(listener, error);
    server::PeerLimits limits;
    limits.messagesPerSecond = 10.0;
    limits.messageBurst = 20.0;
    limits.budgetLoopback = true; // the test's peers are on loopback
    server::Peers peers("test", limits);
    int handled = 0;
    constexpr net::MessageId kWork = 0x7F01, kThrow = 0x7F02;
    const auto pump = [&] {
        net.WaitForActivity(2ms);
        peers.Pump(*listener, [&](server::Peer&, const net::Message& m) {
            if (m.id == kThrow) throw std::runtime_error("a handler's bug");
            ++handled;
        });
    };

    Client calm(net, listener->Port(), pump), flood(net, listener->Port(), pump), buggy(net, listener->Port(), pump);
    REQUIRE(calm.Greet());
    REQUIRE(flood.Greet());
    REQUIRE(buggy.Greet());

    // 200 at once: past the bucket of 20; closed, most of them never handled.
    for (int i = 0; i < 200; ++i) flood.Connection().Send(kWork, {});
    CHECK(flood.WaitClosed());
    CHECK(handled <= 21);
    const int afterFlood = handled;

    // A handler's exception: that peer closed, the loop goes on.
    buggy.Connection().Send(kThrow, {});
    CHECK(buggy.WaitClosed());

    // The calm one, a few: all handled, still open.
    for (int i = 0; i < 5; ++i) calm.Connection().Send(kWork, {});
    for (int i = 0; i < 200 && handled < afterFlood + 5; ++i) pump();
    CHECK(handled == afterFlood + 5);
    CHECK(calm.Connection().State() == net::ConnectionState::Open);
}

TEST_CASE("Servers: loopback addresses") {
    CHECK(net::IsLoopbackAddress("127.0.0.1"));
    CHECK(net::IsLoopbackAddress("127.5.0.9"));
    CHECK(net::IsLoopbackAddress("::1"));
    CHECK(net::IsLoopbackAddress("::ffff:127.0.0.1"));
    CHECK_FALSE(net::IsLoopbackAddress("10.0.0.1"));
    CHECK_FALSE(net::IsLoopbackAddress("1127.0.0.1"));
    CHECK_FALSE(net::IsLoopbackAddress("::ffff:10.0.0.1"));
}

TEST_CASE("Client versions: compared by their numbers, read from Hello's name and from SroDeploy's client.json") {
    using server::ClientVersionGate;
    CHECK(ClientVersionGate::Older("0.1", "0.1.3"));
    CHECK(ClientVersionGate::Older("0.1.2", "0.1.3"));
    CHECK(ClientVersionGate::Older("0.1.9", "0.1.10"));
    CHECK_FALSE(ClientVersionGate::Older("0.1.3", "0.1.3"));
    CHECK_FALSE(ClientVersionGate::Older("0.2", "0.1.9"));
    CHECK_FALSE(ClientVersionGate::Older("0.1.3.0", "0.1.3"));
    CHECK(ClientVersionGate::ClientVersion("SroReimagined 0.1.2") == "0.1.2");
    CHECK(ClientVersionGate::ClientVersion("SroReimagined 0.1") == "0.1"); // the clients before 0.1.3
    CHECK_FALSE(ClientVersionGate::ClientVersion("sr_tests"));
    CHECK_FALSE(ClientVersionGate::ClientVersion("SroBots"));
    CHECK(ClientVersionGate::PublishedVersion(R"({"version": "0.1.3", "file": "SroReimagined-0.1.3.zip"})") == "0.1.3");
    CHECK(ClientVersionGate::PublishedVersion(R"({"file":"a.zip","version":"0.2.0"})") == "0.2.0");
    CHECK(ClientVersionGate::PublishedVersion(R"({"file":"a.zip"})").empty());
    CHECK(ClientVersionGate::PublishedVersion(R"({"version":"<script>"})").empty());
}

TEST_CASE("Servers: a players' client older than the shard's floor is turned away at Hello, told where the new one is") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    // SroDeploy's published client: the floor follows it.
    const std::filesystem::path patch = std::filesystem::temp_directory_path() / std::format("sr_gate_{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(patch / "windows-x64");
    std::ofstream(patch / "windows-x64" / "client.json") << R"({"version": "0.1.4", "file": "SroReimagined-0.1.4.zip"})";
    core::Config settings;
    settings.Set("min-client-version", "0.1.3");
    settings.Set("client-version-from-patch", "true");
    settings.Set("web-patch-dir", patch.string());
    settings.Set("client-download-url", "https://example.com/download");
    auto gate = server::ClientVersionGate::FromConfig(settings);
    REQUIRE(gate);
    CHECK(gate->Required() == "0.1.4");

    server::GatewayServer gateway;
    server::GatewayConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.loginPort = 15780;
    config.refresh = 50ms;
    config.peers.clientGate = gate;
    REQUIRE_MESSAGE(gateway.Start(config, error), error);
    net::NetService net(1);
    const auto greet = [&](const std::string& name) {
        Client client(net, gateway.Port(), [&] { gateway.Poll(2ms); });
        return client.GreetAs(name);
    };
    // The clients of before (all "0.1") and an older one: a protocol mismatch, with the words and the address.
    for (const char* old : {"SroReimagined 0.1", "SroReimagined 0.1.3"}) {
        CAPTURE(old);
        const proto::Welcome w = greet(old);
        CHECK(w.result == proto::WelcomeResult::ProtocolMismatch);
        CHECK(w.reason.find("version 0.1.4 is out") != std::string::npos);
        CHECK(w.reason.find("https://example.com/download") != std::string::npos);
    }
    // The published one and newer, and the programs that are not the players' client, come in.
    CHECK(greet("SroReimagined 0.1.4").result == proto::WelcomeResult::Accepted);
    CHECK(greet("SroReimagined 0.2.0").result == proto::WelcomeResult::Accepted);
    CHECK(greet("sr_tests").result == proto::WelcomeResult::Accepted);
    gateway.Stop();
    std::error_code ec;
    std::filesystem::remove_all(patch, ec);
}
