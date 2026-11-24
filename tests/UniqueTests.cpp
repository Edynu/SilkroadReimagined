#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <set>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/server/Monsters.h"
#include "sr/server/Store.h"
#include "sr/server/WebServer.h"
#include "sr/server/World.h"
#include "ServerTestSupport.h"

using namespace std::chrono_literals;
using namespace sr;
using namespace sr::test;
namespace proto = sr::game::proto;

// The unique monsters: the original's hives and what the game servers tell the website.

TEST_CASE("Uniques: a hive holds one over its nests, kept while its region sleeps; after a kill the next comes at a nest it picks") {
    const char* gameDir = std::getenv("SR_GAME_DIR");
    if (gameDir == nullptr) {
        MESSAGE("SR_GAME_DIR not set: skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, gameDir, error), error);
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    const assets::ObjectRow* tiger = characters.Find(std::string_view("MOB_CH_TIGERWOMAN"));
    const assets::ObjectRow* plain = characters.Find(std::string_view("MOB_CH_TIGER"));
    REQUIRE(tiger != nullptr);
    REQUIRE(plain != nullptr);
    CHECK(tiger->rarity == 3);

    // Tiger Girl's hive (Tab_RefHive 193: one over its nests) as three nests in Jangan's region, ids out of order;
    // a unique nest of its own; an ordinary nest.
    const std::uint16_t region = assets::RegionId(168, 97);
    const auto nest = [&](const assets::ObjectRow& row, float x, float z, std::uint32_t id, std::uint32_t hive, int hiveMax) {
        server::MonsterNest n;
        n.refObjId = row.refObjId;
        n.position = {region, x, 0.0f, z};
        n.spawnRadius = 5;
        n.radius = 50;
        n.delayMin = n.delayMax = 1; // the original's are 3 to 6 hours
        n.maxCount = 1;
        n.nestId = id;
        n.hive = hive;
        n.hiveMax = hiveMax;
        return n;
    };
    std::vector<server::MonsterNest> nests = {nest(*tiger, 900, 1300, 30, 193, 1), nest(*tiger, 1000, 1300, 10, 193, 1), nest(*tiger, 950, 1400, 20, 193, 1),
                                              nest(*tiger, 800, 1200, 40, 0, 0), nest(*plain, 1100, 1400, 50, 0, 0)};

    core::JobSystem jobs(2);
    server::WorldConfig config;
    server::World world(config, jobs, &fs);
    server::Monsters monsters(world, characters, nullptr, nests, 7);
    std::vector<server::WorldMessage> out;
    std::vector<server::Monsters::Events::Unique> came;
    const auto pump = [&](const std::function<bool()>& until, int rounds = 2000) {
        for (int i = 0; i < rounds; ++i) {
            const auto now = server::WorldClock::now();
            world.Update(now, out);
            server::Monsters::Events events;
            monsters.Update(now, {}, events);
            came.insert(came.end(), events.uniques.begin(), events.uniques.end());
            out.clear();
            if (until()) return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    };
    // The unique nests holding a monster in the world (they stand 100 or more apart; the ordinary one is farther).
    const auto tigersNear = [&] {
        int n = 0;
        for (const auto& [x, z] : {std::pair{900.0f, 1300.0f}, {1000.0f, 1300.0f}, {950.0f, 1400.0f}, {800.0f, 1200.0f}})
            n += monsters.Near({region, x, 0, z}, 30).empty() ? 0 : 1;
        return n;
    };

    // Nobody near: the region sleeps, yet the hive's unique and the lone one come into being, each told once.
    REQUIRE(pump([&] { return came.size() >= 2; }));
    pump([] { return false; }, 300); // more updates: nothing more comes
    REQUIRE(came.size() == 2);
    std::set<std::pair<float, float>> places;
    for (const auto& u : came) places.insert({u.position.x, u.position.z});
    CHECK(places.contains({1000.0f, 1300.0f})); // the hive's first by nest id (10)
    CHECK(places.contains({800.0f, 1200.0f}));  // the lone one
    CHECK(monsters.Alive() == 0);               // nothing in the world while its region sleeps

    // A player comes: the region wakes and the two stand there (with the ordinary one), nothing new told.
    server::WorldPlayer player;
    player.peer = 1;
    player.character = 77;
    player.position = {region, 950, 0, 1300};
    player.dropOnGround = true;
    const std::uint32_t me = world.Enter(player);
    REQUIRE(pump([&] { return tigersNear() == 2; }));
    CHECK(came.size() == 2);

    // The hive's unique killed: the kill says so and names the killing hit; the next comes after the delay at one of
    // the hive's three nests, told as a coming.
    const std::vector<std::uint32_t> atFirst = monsters.Near({region, 1000, 0, 1300}, 30);
    REQUIRE(atFirst.size() == 1);
    const std::uint32_t hiveTiger = atFirst.front();
    const server::Monsters::Damaged d = monsters.Damage(hiveTiger, me, 100'000'000, std::chrono::steady_clock::now());
    CHECK(d.killed);
    CHECK(d.unique);
    CHECK(d.killer == me);
    REQUIRE(pump([&] { return came.size() == 3; }, 1000));
    const auto next = came.back().position;
    CHECK(((next.x == 900.0f && next.z == 1300.0f) || (next.x == 1000.0f && next.z == 1300.0f) || (next.x == 950.0f && next.z == 1400.0f)));
    REQUIRE(pump([&] { return tigersNear() == 2; }));
    CHECK(came.size() == 3);

    // The player leaves and comes back: the region sleeps and wakes, its uniques are the same ones (no new coming).
    world.Leave(me);
    pump([&] { return monsters.Alive() == 0; });
    CHECK(monsters.Alive() == 0);
    const std::uint32_t again = world.Enter(player);
    REQUIRE(again != 0);
    REQUIRE(pump([&] { return tigersNear() == 2; }));
    CHECK(came.size() == 3);
}

TEST_CASE("Uniques: the game servers' events, a server's going, the website's list across a shard's two servers, the bus, the pruning") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    server::Database& db = t.Db();
    const auto event = [](server::UniqueEvent::Kind kind, std::uint32_t model, const char* name, std::uint16_t server, double at,
                          const char* killer = "") {
        server::UniqueEvent e;
        e.kind = kind;
        e.refObjId = model;
        e.codename = std::string("MOB_") + name;
        e.name = name;
        e.server = server;
        e.region = 25000;
        e.x = 100;
        e.z = 200;
        e.killerId = *killer != 0 ? 42 : 0;
        e.killerName = killer;
        e.at = at;
        return e;
    };
    using Kind = server::UniqueEvent::Kind;
    // Server 1 (East): Tiger Girl came, was killed by Astra, came again. Server 2 (West): Kerberos came and was killed
    // by Kaizen; Uruchi came. Written out of order: the game server's times decide.
    REQUIRE(server::RecordUniqueEvent(db, event(Kind::Appeared, 1954, "Tiger Girl", 1, 1000)));
    REQUIRE(server::RecordUniqueEvent(db, event(Kind::Appeared, 1954, "Tiger Girl", 1, 3000)));
    REQUIRE(server::RecordUniqueEvent(db, event(Kind::Killed, 1954, "Tiger Girl", 1, 2000, "Astra")));
    REQUIRE(server::RecordUniqueEvent(db, event(Kind::Appeared, 5871, "Kerberos", 2, 1500)));
    REQUIRE(server::RecordUniqueEvent(db, event(Kind::Killed, 5871, "Kerberos", 2, 2500, "Kaizen")));
    REQUIRE(server::RecordUniqueEvent(db, event(Kind::Appeared, 1982, "Uruchi", 2, 2600)));

    // Both game servers report (a unique of a server that does not is not alive, whatever it last said).
    server::GameServerInfo report;
    report.host = "127.0.0.1";
    report.capacity = 100;
    std::string error0;
    for (std::uint16_t id : {1, 2}) {
        report.id = id;
        report.port = std::uint16_t(16000 + id);
        REQUIRE(server::ReportGameServer(db, report, error0));
    }

    server::WebConfig c;
    c.databaseUrl = t.Url();
    c.port = 0;
    c.statusAge = 3600s; // only the bus makes it read the list again
    server::WebServer web;
    std::string error;
    REQUIRE_MESSAGE(web.Start(c, error), error);
    httplib::Client client("127.0.0.1", web.Port());
    const auto list = [&](const std::function<bool(const nlohmann::json&)>& done) {
        nlohmann::json j;
        for (int i = 0; i < 200; ++i) {
            web.Poll();
            const auto r = client.Get("/api/uniques");
            REQUIRE(r);
            REQUIRE(r->status == 200);
            j = nlohmann::json::parse(r->body);
            if (done(j)) return j;
            std::this_thread::sleep_for(20ms);
        }
        FAIL("never: " << j.dump());
        return j;
    };
    nlohmann::json u = list([](const nlohmann::json& j) { return j["uniques"].size() == 3; });
    // The living first (the latest first), then the dead.
    CHECK(u["uniques"][0]["name"] == "Tiger Girl");
    CHECK(u["uniques"][0]["alive"] == true);
    CHECK(u["uniques"][0]["since"] == 3000);
    CHECK(u["uniques"][0]["killer"] == "Astra");
    CHECK(u["uniques"][0]["killedAt"] == 2000);
    CHECK(u["uniques"][1]["name"] == "Uruchi");
    CHECK(u["uniques"][1]["alive"] == true);
    CHECK(u["uniques"][1]["killer"] == "");
    CHECK(u["uniques"][2]["name"] == "Kerberos");
    CHECK(u["uniques"][2]["alive"] == false);
    CHECK(u["uniques"][2]["killer"] == "Kaizen");

    // Server 2 stops (or starts again after a crash): its living uniques went with it; server 1's stay. The site hears
    // it on the shard's bus (its cache would keep the old list for an hour).
    REQUIRE(server::ForgetServerUniques(db, 2, 4000));
    REQUIRE(server::Notify(db, "sr_uniques_sro_vt_shard", "3"));
    u = list([](const nlohmann::json& j) { return j["uniques"][1]["alive"] == false; });
    CHECK(u["uniques"][0]["name"] == "Tiger Girl");
    CHECK(u["uniques"][0]["alive"] == true);
    for (int i = 1; i < 3; ++i) CHECK(u["uniques"][i]["alive"] == false);
    CHECK(db.Query("SELECT count(*) FROM sr_unique_events WHERE event = 3").Int(0, 0) == 1); // only Uruchi was alive there

    // The record pruned (all of it is older than a month): each unique's latest and last kill stay, so the list is the same.
    CHECK(server::PruneUniqueEvents(db, 30) == 3); // Tiger Girl's first coming, Kerberos's coming, Uruchi's coming
    CHECK(db.Query("SELECT count(*) FROM sr_unique_events").Int(0, 0) == 4);
    REQUIRE(server::Notify(db, "sr_uniques_sro_vt_shard", "pruned"));
    const nlohmann::json after = list([](const nlohmann::json& j) { return j["updated"].is_number(); });
    CHECK(after["uniques"] == u["uniques"]);
    CHECK(server::PruneUniqueEvents(db, 30) == 0);

    // Server 1 stops without a word: its Tiger Girl is not alive on the site any more.
    REQUIRE(server::StopGameServer(db, 1));
    REQUIRE(server::Notify(db, "sr_uniques_sro_vt_shard", "stopped"));
    const nlohmann::json down = list([](const nlohmann::json& j) { return j["uniques"][0]["alive"] == false; });
    for (const auto& x : down["uniques"]) CHECK(x["alive"] == false);
}

TEST_CASE("Hives: an ordinary hive's cap holds its nests' monsters together, filled in nest order; a kill's gap refilled under it") {
    const char* gameDir = std::getenv("SR_GAME_DIR");
    if (gameDir == nullptr) {
        MESSAGE("SR_GAME_DIR not set: skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, gameDir, error), error);
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    const assets::ObjectRow* tiger = characters.Find(std::string_view("MOB_CH_TIGER"));
    REQUIRE(tiger != nullptr);

    // A hive of four nests of three (12) capped at 6, its nest ids out of order; a hive whose cap (10) is above its
    // nests' own (3): no cap at all.
    const std::uint16_t region = assets::RegionId(168, 97);
    const auto nest = [&](float x, std::uint32_t id, std::uint32_t hive, int hiveMax) {
        server::MonsterNest n;
        n.refObjId = tiger->refObjId;
        n.position = {region, x, 0.0f, 1300.0f};
        n.spawnRadius = 5;
        n.radius = 50;
        n.delayMin = n.delayMax = 1;
        n.maxCount = 3;
        n.nestId = id;
        n.hive = hive;
        n.hiveMax = hiveMax;
        return n;
    };
    std::vector<server::MonsterNest> nests = {nest(400, 4, 700, 6), nest(700, 2, 700, 6), nest(1000, 3, 700, 6), nest(1300, 1, 700, 6),
                                              nest(1600, 5, 701, 10)};
    core::JobSystem jobs(2);
    server::WorldConfig config;
    server::World world(config, jobs, &fs);
    server::Monsters monsters(world, characters, nullptr, nests, 7);
    std::vector<server::WorldMessage> out;
    const auto pump = [&](const std::function<bool()>& until, int rounds = 2000) {
        for (int i = 0; i < rounds; ++i) {
            const auto now = server::WorldClock::now();
            world.Update(now, out);
            server::Monsters::Events events;
            monsters.Update(now, {}, events);
            out.clear();
            if (until()) return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    };
    const auto at = [&](float x) { return int(monsters.Near({region, x, 0, 1300}, 100).size()); }; // they roam 50 about it
    const auto hive = [&] { return at(400) + at(700) + at(1000) + at(1300); };

    server::WorldPlayer player;
    player.peer = 1;
    player.character = 77;
    player.position = {region, 1000, 0, 1300};
    player.dropOnGround = true;
    const std::uint32_t me = world.Enter(player);
    REQUIRE(pump([&] { return hive() >= 6 && at(1600) == 3; }));
    pump([] { return false; }, 300);
    // Six over the four: nest 1 (x 1300) and nest 2 (x 700) full, in nest id order; the uncapped hive's nest full.
    CHECK(hive() == 6);
    CHECK(at(1300) == 3);
    CHECK(at(700) == 3);
    CHECK(at(1600) == 3);

    // One killed: one comes back (at the nest picked, once its second is over), never above six.
    const std::uint32_t victim = monsters.Near({region, 700, 0, 1300}, 100).front();
    CHECK(monsters.Damage(victim, me, 100'000'000, std::chrono::steady_clock::now()).killed);
    REQUIRE(pump([&] { return hive() == 5; }));
    REQUIRE(pump([&] { return hive() == 6; }));
    pump([] { return false; }, 400);
    CHECK(hive() == 6);
}

TEST_CASE("Party monsters: a nest an exp-shared party's kill marked hatches one at 50 % until it comes; champions only by a champion row") {
    // The kinds' health: a party monster's base 10, then its grade's.
    CHECK(proto::MonsterMaxHp(100, 0x00) == 100);
    CHECK(proto::MonsterMaxHp(100, 0x10) == 1'000);
    CHECK(proto::MonsterMaxHp(100, 0x11) == 2'000);
    CHECK(proto::MonsterMaxHp(100, 0x14) == 20'000);
    CHECK(proto::MonsterMaxHp(100, 0x07) == 3'000);
    CHECK(proto::MonsterMaxHp(100, 0x06) == 400);

    const char* gameDir = std::getenv("SR_GAME_DIR");
    if (gameDir == nullptr) {
        MESSAGE("SR_GAME_DIR not set: skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, gameDir, error), error);
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    const assets::ObjectRow* tiger = characters.Find(std::string_view("MOB_CH_TIGER"));
    REQUIRE(tiger != nullptr);

    // Three nests of one: a plain one with no champion row (champion% 0), the same where party monsters are off (an
    // instance world's), one whose every hatch is a champion or a giant.
    const std::uint16_t region = assets::RegionId(168, 97);
    const auto nest = [&](float x, int champion, bool party) {
        server::MonsterNest n;
        n.refObjId = tiger->refObjId;
        n.position = {region, x, 0.0f, 1300.0f};
        n.spawnRadius = 5;
        n.radius = 50;
        n.delayMin = n.delayMax = 0.05f;
        n.maxCount = 1;
        n.championPercent = champion;
        n.partyMonsters = party;
        return n;
    };
    core::JobSystem jobs(2);
    server::WorldConfig config;
    server::World world(config, jobs, &fs);
    server::Monsters monsters(world, characters, nullptr, {nest(400, 0, true), nest(800, 0, false), nest(1200, 101, true)}, 11);
    std::vector<server::WorldMessage> out;
    const auto pump = [&](const std::function<bool()>& until, int rounds = 2000) {
        for (int i = 0; i < rounds; ++i) {
            const auto now = server::WorldClock::now();
            world.Update(now, out);
            server::Monsters::Events events;
            monsters.Update(now, {}, events);
            out.clear();
            if (until()) return true;
            std::this_thread::sleep_for(2ms);
        }
        return false;
    };
    const auto at = [&](float x) { // the living one there
        for (const std::uint32_t e : monsters.Near({region, x, 0, 1300}, 100)) {
            if (monsters.Hp(e) > 0) return e;
        }
        return 0u;
    };
    server::WorldPlayer player;
    player.peer = 1;
    player.character = 77;
    player.position = {region, 800, 0, 1300};
    player.dropOnGround = true;
    const std::uint32_t me = world.Enter(player);
    // Kills the one at x, marked by a party's kill or not; its rarity.
    const auto kill = [&](float x, bool party) {
        std::uint32_t victim = 0;
        REQUIRE(pump([&] { return (victim = at(x)) != 0; }));
        const std::uint32_t hp = monsters.Hp(victim);
        const server::Monsters::Damaged d = monsters.Damage(victim, me, 2'000'000'000, std::chrono::steady_clock::now());
        REQUIRE(d.killed);
        CHECK(hp == proto::MonsterMaxHp(tiger->maxHp, d.rarity));
        if (party) monsters.PartyKilled(victim);
        return d.rarity;
    };

    // Unmarked: plain. Marked: a party monster within a few hatchings (each 50 %), then plain again.
    CHECK(kill(400, true) == 0);
    int tries = 0;
    std::uint8_t r = 0;
    while (tries < 40 && (r = kill(400, false)) == 0) ++tries;
    CHECK(r == 0x10);
    for (int i = 0; i < 8; ++i) CHECK(kill(400, false) == 0);
    // Where party monsters are off: never one.
    for (int i = 0; i < 8; ++i) CHECK(kill(800, true) == 0);
    // A champion row and 101 %: every one a champion or a giant (a party one once marked).
    bool party = false;
    for (int i = 0; i < 12; ++i) {
        const std::uint8_t c = kill(1200, true);
        CHECK(((c & 0x0F) == 1 || (c & 0x0F) == 4));
        party = party || (c >> 4) == 1;
    }
    CHECK(party);
}

TEST_CASE("Crowded hunting grounds: every 30 s the players in its nests' regions, their two minutes' average, cut its nests' delays") {
    const char* gameDir = std::getenv("SR_GAME_DIR");
    if (gameDir == nullptr) {
        MESSAGE("SR_GAME_DIR not set: skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, gameDir, error), error);
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    const assets::ObjectRow* tiger = characters.Find(std::string_view("MOB_CH_TIGER"));
    REQUIRE(tiger != nullptr);

    // A hive made for one player (its step 30 %, its most 80 %) of one nest of one, 10 s between hatchings.
    const std::uint16_t region = assets::RegionId(168, 97);
    server::MonsterNest n;
    n.refObjId = tiger->refObjId;
    n.position = {region, 1000.0f, 0.0f, 1300.0f};
    n.spawnRadius = 5;
    n.radius = 50;
    n.delayMin = n.delayMax = 10;
    n.maxCount = 1;
    n.hive = 900;
    n.crowdPlayers = 1;
    n.crowdStep = 30;
    n.crowdMax = 80;
    core::JobSystem jobs(2);
    server::WorldConfig config;
    server::World world(config, jobs, &fs);
    server::Monsters monsters(world, characters, nullptr, {n}, 5);
    std::vector<server::WorldMessage> out;
    std::vector<server::Monsters::Player> none;
    const auto pump = [&](const std::function<bool()>& until, int rounds = 2000) {
        for (int i = 0; i < rounds; ++i) {
            const auto now = server::WorldClock::now();
            world.Update(now, out);
            server::Monsters::Events events;
            monsters.Update(now, none, events);
            out.clear();
            if (until()) return true;
            std::this_thread::sleep_for(2ms);
        }
        return false;
    };
    const auto alive = [&] {
        for (const std::uint32_t e : monsters.Near({region, 1000, 0, 1300}, 100)) {
            if (monsters.Hp(e) > 0) return e;
        }
        return 0u;
    };
    server::WorldPlayer player;
    player.peer = 1;
    player.character = 77;
    player.position = {region, 1000, 0, 1300};
    player.dropOnGround = true;
    const std::uint32_t me = world.Enter(player);
    REQUIRE(pump([&] { return alive() != 0; }));
    const auto start = server::WorldClock::now(); // its first sample is due 30 s after its first update, before this

    // The clock moved on by hand: a sample every 30 s from the first update.
    std::vector<server::Monsters::Player> three(3);
    for (std::size_t i = 0; i < three.size(); ++i) {
        three[i].entity = 9000 + std::uint32_t(i);
        three[i].position = {region, 900.0f + 10.0f * float(i), 0.0f, 1300.0f};
    }
    const auto at = [&](double seconds, const std::vector<server::Monsters::Player>& players) {
        server::Monsters::Events events;
        monsters.Update(start + std::chrono::milliseconds(std::int64_t(seconds * 1000.0)), players, events);
    };
    CHECK(monsters.CrowdRate(900) == 0);
    at(30.5, three);
    CHECK(monsters.CrowdRate(900) == 0); // one sample: no average yet
    at(61, three);
    CHECK(monsters.CrowdRate(900) == 30); // (3 + 0) / 2 = 1 player: 1 step
    at(91.5, three);
    at(122, three);
    CHECK(monsters.CrowdRate(900) == 80); // 3 players: 90 %, held to 80

    // Killed now: back after 10 s less 80 %, 2 s.
    const std::uint32_t victim = alive();
    REQUIRE(victim != 0);
    const auto killedAt = start + std::chrono::milliseconds(123'000);
    CHECK(monsters.Damage(victim, me, 2'000'000'000, killedAt).killed);
    at(124.5, three);
    CHECK(alive() == 0);
    at(125.5, three);
    CHECK(alive() != 0);

    // Nobody for two minutes: the rate falls back.
    at(152.5, none);
    at(183, none);
    CHECK(monsters.CrowdRate(900) == 30); // (3 + 0) / 2
    at(213.5, none);
    at(244, none);
    CHECK(monsters.CrowdRate(900) == 0);
}
