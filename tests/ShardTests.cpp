// A shard split between game servers: its settings, its one row in the server
// list, and which game server a login is sent to.

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <thread>

#include "ServerTestSupport.h"
#include "sr/core/Config.h"
#include "sr/server/GatewayServer.h"
#include "sr/server/ShardLayout.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;

namespace {

server::ShardLayout Parse(const char* text, std::string* problem = nullptr) {
    core::Config config;
    std::string error;
    REQUIRE(config.ParseText(text, error));
    server::ShardLayout layout;
    const bool ok = server::ShardLayout::Read(config, layout, error);
    if (problem != nullptr) {
        *problem = error;
    } else {
        REQUIRE_MESSAGE(ok, error);
    }
    return layout;
}

const std::vector<std::string> kAreas = {"CHINA", "West_China", "Thief Village", "TQ", "Eu", "GOD_FLAME", "GOD_TOGUI", "ARENA_FLAG"};

const char* kSplit =
    "shard-name = Silkroad\n"
    "server.1.name = East\n"
    "server.1.public-host = east.example.com\n"
    "server.1.port = 15781\n"
    "server.1.capacity = 800\n"
    "server.1.regions = CHINA, West_China, Thief Village, TQ\n"
    "server.2.name = West\n"
    "server.2.regions = Eu, GOD_*, ARENA_*\n";

server::GameServerInfo Report(std::uint16_t id, std::uint32_t online, std::uint32_t capacity, const char* schema = "sro_vt_shard") {
    server::GameServerInfo s;
    s.id = id;
    s.name = "Silkroad";
    s.group = 1;
    s.groupName = "Silkroad";
    s.host = "127.0.0.1";
    s.port = std::uint16_t(16000 + id);
    s.capacity = capacity;
    s.online = online;
    s.schema = schema;
    s.reporting = true;
    return s;
}

} // namespace

TEST_CASE("A shard's settings: each area has one owner, by name, by prefix, or as the rest") {
    server::ShardLayout layout = Parse(kSplit);
    REQUIRE(layout.Servers().size() == 2);
    CHECK(layout.Name() == "Silkroad");
    REQUIRE(layout.Server(1) != nullptr);
    CHECK(layout.Server(1)->name == "East");
    CHECK(layout.Server(1)->publicHost == "east.example.com");
    CHECK(layout.Server(1)->port == 15781);
    CHECK(layout.Server(1)->capacity == 800);
    // The shard's capacity (all its servers together) is 3000 unless written, and a server not saying carries all of it.
    CHECK(layout.Capacity() == server::ShardLayout::kDefaultShardCapacity);
    CHECK(layout.Server(2)->capacity == 3000);
    CHECK(layout.Server(3) == nullptr);
    CHECK(layout.OwnerOf("CHINA") == 1);
    CHECK(layout.OwnerOf("china") == 1); // names are not case-sensitive
    CHECK(layout.OwnerOf("Thief Village") == 1); // a name with a space
    CHECK(layout.OwnerOf("GOD_FLAME") == 2);
    CHECK(layout.OwnerOf("ARENA_FLAG") == 2);
    CHECK(layout.OwnerOf("Atlantis") == 0);
    std::string error;
    CHECK_MESSAGE(layout.Check(kAreas, error), error);

    // One server with *: the whole world in one process.
    server::ShardLayout one = Parse("server.1.regions = *\n");
    CHECK(one.OwnerOf("CHINA") == 1);
    CHECK(one.Check(kAreas, error));

    // * takes only what the others leave; a name beats a prefix, a prefix beats *.
    server::ShardLayout rest = Parse("server.1.regions = *\nserver.2.regions = GOD_*\nserver.3.regions = GOD_FLAME\n");
    CHECK(rest.OwnerOf("CHINA") == 1);
    CHECK(rest.OwnerOf("GOD_TOGUI") == 2);
    CHECK(rest.OwnerOf("GOD_FLAME") == 3);
    CHECK(rest.Check(kAreas, error));

    // No servers named: an empty layout (a game server alone).
    CHECK(Parse("shard-name = Solo\n").Empty());
}

TEST_CASE("A shard's settings: mistakes stop the server, and say what is wrong") {
    std::string problem;
    std::string error;
    // An area nobody runs.
    CHECK_FALSE(Parse("server.1.regions = CHINA\n").Check(kAreas, error));
    CHECK(error.find("West_China") != std::string::npos);
    // A name the game does not have (a typo).
    CHECK_FALSE(Parse("server.1.regions = *\nserver.2.regions = Chna\n").Check(kAreas, error));
    CHECK(error.find("Chna") != std::string::npos);
    // Claimed twice.
    CHECK_FALSE(Parse("server.1.regions = *, CHINA\nserver.2.regions = CHINA\n").Check(kAreas, error));
    CHECK(error.find("CHINA") != std::string::npos);
    CHECK_FALSE(Parse("server.1.regions = GOD_*, CHINA, West_China, Thief Village, TQ, Eu, ARENA_FLAG\nserver.2.regions = GOD_*\n").Check(kAreas, error));
    // * twice; a server with no regions; a bad number; an unknown setting.
    Parse("server.1.regions = *\nserver.2.regions = *\n", &problem);
    CHECK(problem.find("more than one") != std::string::npos);
    Parse("server.1.name = East\n", &problem);
    CHECK(problem.find("server.1.regions") != std::string::npos);
    Parse("server.x.regions = *\n", &problem);
    CHECK(problem.find("number") != std::string::npos);
    Parse("server.1.regions = *\nserver.1.port = many\n", &problem);
    CHECK(problem.find("many") != std::string::npos);
    Parse("server.1.regions = *\nserver.1.colour = red\n", &problem);
    CHECK(problem.find("colour") != std::string::npos);
    Parse("shard-capacity = 0\nserver.1.regions = *\n", &problem);
    CHECK(problem.find("shard-capacity") != std::string::npos);
}

TEST_CASE("A shard's capacity is all its game servers' together, and each carries the whole of it") {
    server::ShardLayout layout = Parse((std::string("shard-capacity = 2500\n") + kSplit).c_str());
    CHECK(layout.Capacity() == 2500);
    CHECK(layout.Server(1)->capacity == 800);  // written: a smaller machine
    CHECK(layout.Server(2)->capacity == 2500); // not: the shard's, since the players gather where they like

    // Reported beside each server's own, the shard's capacity is the row's (not the servers' added up).
    std::vector<server::GameServerInfo> all = {Report(1, 1200, 3000), Report(2, 900, 3000)};
    for (auto& s : all) s.shardCapacity = 3000;
    auto shards = server::ShardsOf(all);
    REQUIRE(shards.size() == 1);
    CHECK(shards[0].online == 2100);
    CHECK(shards[0].capacity == 3000);
    // One server down: the other still carries the whole shard.
    all[1].reporting = false;
    all[1].online = 0;
    CHECK(server::ShardsOf(all)[0].capacity == 3000);
}

TEST_CASE("A shard is one row: its game servers added up, up while any reports") {
    std::vector<server::GameServerInfo> all = {Report(1, 10, 100), Report(2, 30, 200), Report(5, 7, 50, "sro_vt_shard2")};
    auto shards = server::ShardsOf(all);
    REQUIRE(shards.size() == 2);
    CHECK(shards[0].id == 1);
    CHECK(shards[0].online == 40);
    CHECK(shards[0].capacity == 300);
    CHECK(shards[0].reporting);
    CHECK(shards[1].id == 5);
    all[0].reporting = false;
    CHECK(server::ShardsOf(all)[0].reporting); // one left
    all[1].reporting = false;
    CHECK_FALSE(server::ShardsOf(all)[0].reporting); // none
}

TEST_CASE("A login goes to the shard's game server with the most room, or the one a developer names") {
    std::vector<server::GameServerInfo> all = {Report(1, 90, 100), Report(2, 40, 200), Report(5, 0, 50, "sro_vt_shard2")};
    auto chosen = server::ChooseLobbyServer(all, 1, 0);
    REQUIRE(chosen.has_value());
    CHECK(chosen->id == 2); // by share: 20% against 90%; never the other shard's empty server
    CHECK(server::ChooseLobbyServer(all, 2, 0)->id == 2); // any row id of the shard will do
    CHECK(server::ChooseLobbyServer(all, 1, 1)->id == 1); // named
    CHECK(server::ChooseLobbyServer(all, 1, 5)->id == 2); // another shard's is not a choice
    CHECK_FALSE(server::ChooseLobbyServer(all, 9, 0).has_value()); // no such row
    all[1].reporting = false;
    CHECK(server::ChooseLobbyServer(all, 1, 2)->id == 1); // the named one is down
    all[0].reporting = false;
    CHECK_FALSE(server::ChooseLobbyServer(all, 1, 0).has_value()); // the shard is down
}

TEST_CASE("Servers: the gateway lists a shard of two game servers once; one stopping leaves it up, both take it down") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    REQUIRE(server::ReportGameServer(t.Db(), Report(1, 3, 100), error));
    REQUIRE(server::ReportGameServer(t.Db(), Report(2, 4, 100), error));

    server::GatewayServer gateway;
    server::GatewayConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.loginPort = 15780;
    config.refresh = 10ms;
    REQUIRE_MESSAGE(gateway.Start(config, error), error);
    net::NetService net(1);
    Client client(net, gateway.Port(), [&] { gateway.Poll(2ms); });
    REQUIRE(client.Greet());
    const auto list = [&] {
        client.Send(proto::ServerListRequest{});
        proto::ServerList l;
        REQUIRE(client.Expect(l));
        return l;
    };
    const auto waitFor = [&](proto::ServerState state) {
        for (int i = 0; i < 200; ++i) {
            const proto::ServerList l = list();
            REQUIRE(l.servers.size() == 1);
            if (l.servers[0].state == state) return l;
            std::this_thread::sleep_for(10ms);
        }
        FAIL("the list never changed");
        return proto::ServerList{};
    };
    const proto::ServerList both = list();
    REQUIRE(both.servers.size() == 1);
    CHECK(both.servers[0].state == proto::ServerState::Open);
    CHECK(both.servers[0].online == 7);
    CHECK(both.servers[0].capacity == 200);

    REQUIRE(server::StopGameServer(t.Db(), 1));
    std::this_thread::sleep_for(100ms);
    CHECK(waitFor(proto::ServerState::Open).servers[0].state == proto::ServerState::Open);
    REQUIRE(server::StopGameServer(t.Db(), 2));
    CHECK(waitFor(proto::ServerState::Offline).servers[0].state == proto::ServerState::Offline);
}
