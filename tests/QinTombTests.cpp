// The Qin-Shi tomb's upper floors on the game files: the snake gate opened, a player through it into floor 5's copy, the
// four guardians and the Flame Master, floor 5's portals in the drawn order into floor 6's copy, the Black Snake and its
// portal up to the summit, then the gate shut, the copies' last hour told (run 1,000 times faster) and everyone back on the
// 4th floor at its end.

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <thread>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/InstanceProtocol.h"
#include "sr/server/Instances.h"
#include "sr/server/Monsters.h"
#include "sr/server/QinTomb.h"
#include "sr/server/World.h"

using namespace sr;
namespace proto = game::proto;

namespace {

struct QinRig : server::InstanceHost, server::QinTombHost {
    server::World* world = nullptr;
    std::vector<std::pair<proto::MessageId, std::vector<std::byte>>> sent;
    std::vector<std::string> shard;

    std::optional<InstanceHost::Player> InstancePlayer(std::uint32_t entity) override {
        InstanceHost::Player p;
        p.peer = entity;
        p.character = 1000 + entity;
        p.name = "P" + std::to_string(entity);
        p.level = 100;
        return p;
    }
    void InstanceSend(std::uint64_t, proto::MessageId id, const std::vector<std::byte>& payload) override { sent.emplace_back(id, payload); }
    void InstanceTeleport(std::uint32_t entity, const proto::WorldPosition& to) override { world->Teleport(entity, to); }
    std::uint32_t InstanceItemAt(std::uint32_t, std::uint8_t) override { return 0; }
    bool InstanceTakeOne(std::uint32_t, std::uint8_t) override { return false; }
    std::vector<server::InstanceEntry> entries; // the entry records saved, in turn
    void InstanceSaveEntry(std::int64_t, const server::InstanceEntry& e) override { entries.push_back(e); }
    void InstanceSaveBook(std::int64_t, const server::BookEntry&) override {}
    void InstanceSaveWait(std::int64_t, const server::ItemWait&) override {}
    void InstanceBookChanged(std::uint32_t) override {}
    std::vector<Member> InstancePartyMembers(std::uint32_t) override { return {}; }
    std::uint32_t InstanceEntityOf(std::uint32_t) override { return 0; }
    std::uint16_t InstanceServerId() override { return 1; }
    void InstancePublish(std::string) override {}
    int InstanceMonsterLevel(std::uint32_t) override { return 0; }

    void QinShardNotice(const std::string& key) override { shard.push_back(key); }
    std::optional<proto::WorldPosition> QinPoint(std::uint32_t point) override {
        if (point == server::QinTomb::kFloor5Gate) return proto::WorldPosition{server::QinTomb::kFloor5, -1335.0f, -42.0f, 642.0f}; // GATE_JINSI_05x01
        return std::nullopt;
    }
    template <typename T>
    std::vector<T> All() const {
        std::vector<T> out;
        for (const auto& [id, payload] : sent) {
            T m;
            if (id == T::kId && proto::Decode(payload, m)) out.push_back(m);
        }
        return out;
    }
};

double Flat(const proto::WorldPosition& a, float x, float z) { return std::hypot(double(a.x) - x, double(a.z) - z); }

} // namespace

TEST_CASE("Qin-Shi tomb: the snake gate, floors 5 and 6 and the copies' end (game files)") {
    const char* gameDir = std::getenv("SR_GAME_DIR");
    if (!gameDir) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, gameDir, error), error);
    assets::CharacterTable characters;
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    REQUIRE_MESSAGE(skills->Load(fs, error), error);

    core::JobSystem jobs(2);
    server::WorldConfig config;
    server::World world(config, jobs, &fs);
    server::Monsters monsters(world, characters, skills.get(), {}, 7);
    QinRig rig;
    rig.world = &world;
    server::Instances instances(rig, world, monsters, characters, std::make_shared<server::GameWorldCatalog>(), 11);
    server::QinTombConfig qinConfig;
    qinConfig.enabled = false; // no schedule: opened by hand
    qinConfig.timeScale = 1000.0;
    server::QinTomb qin(rig, instances, monsters, characters, qinConfig, 5);

    std::vector<server::WorldMessage> out;
    std::vector<server::Monsters::Player> fighters;
    const auto pump = [&](const std::function<bool()>& until, int rounds = 4000) {
        for (int i = 0; i < rounds; ++i) {
            const auto now = server::WorldClock::now();
            world.Update(now, out);
            server::Monsters::Events events;
            monsters.Update(now, fighters, events);
            instances.Tick(now);
            qin.Tick(now);
            out.clear();
            if (until()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    const auto inLayer = [](proto::WorldPosition p, std::uint32_t layer) {
        p.layer = layer;
        return p;
    };
    // The monster of a codename standing within `reach` of a place (0: none yet).
    const auto find = [&](const char* code, proto::WorldPosition at, float reach) -> std::uint32_t {
        const assets::ObjectRow* row = characters.Find(std::string_view(code));
        REQUIRE(row != nullptr);
        for (const std::uint32_t m : monsters.Near(at, reach)) {
            if (monsters.RefOf(m) == row->refObjId) return m;
        }
        return 0;
    };
    const auto kill = [&](std::uint32_t monster, std::uint32_t by) { monsters.Damage(monster, by, monsters.Hp(monster), server::WorldClock::now()); };

    // A player on the 4th floor by the snake gate.
    server::WorldPlayer p;
    p.peer = 1;
    p.character = 1001;
    p.position = {server::QinTomb::kFloor4, -655.0f, 7.0f, -172.0f};
    p.dropOnGround = true;
    const std::uint32_t me = world.Enter(p);
    instances.Enter(me, 1001, {}, {});
    REQUIRE(pump([&] { return world.Where(me).has_value(); }));

    // The gate shut: nobody goes through.
    CHECK(qin.Admit(me) == proto::GateStatus::Nowhere);
    // Opened: the shard told, a copy of each floor, floor 5's guardians and floor 6's four sides started.
    REQUIRE(qin.Open(server::WorldClock::now()));
    CHECK(qin.GateOpen());
    REQUIRE(qin.Floor5() != 0);
    REQUIRE(qin.Floor6() != 0);
    CHECK(qin.Floor5() != qin.Floor6());
    REQUIRE(rig.shard.size() == 1);
    CHECK(rig.shard[0] == "UIIT_TQ_F4_TELEPORT_OPEN");
    for (const char* s : {"Chins_Floor5_East", "Chins_Floor5_South", "Chins_Floor5_West", "Chins_Floor5_North", "Chins_Floor6_East", "Chins_Floor6_South",
                          "Chins_Floor6_West", "Chins_Floor6_North"}) {
        CHECK_MESSAGE(qin.Started(s), s);
    }
    CHECK_FALSE(qin.Started("Chins_Floor5_FlameMaster"));
    CHECK(qin.Open(server::WorldClock::now()) == false); // open already

    // Through the gate: in floor 5's copy at GATE_JINSI_05x01; a second time refused (already in an instance).
    REQUIRE(qin.Admit(me) == proto::GateStatus::Ok);
    REQUIRE(pump([&] {
        const auto at = world.Where(me);
        return at && at->position.layer == qin.Floor5();
    }));
    CHECK(world.Where(me)->position.region == server::QinTomb::kFloor5);
    CHECK(Flat(world.Where(me)->position, -1335.0f, 642.0f) < 50.0);
    CHECK(qin.Admit(me) == proto::GateStatus::InInstance);

    // The four guardians, each at its side; killed, the Flame Master comes.
    const std::array<std::pair<const char*, std::array<float, 3>>, 4> guardians{{{"MOB_TQ_EASTGUARDIAN", {16203, -75, -396}},
                                                                                  {"MOB_TQ_SOUTHGUARDIAN", {222, -38, -15928}},
                                                                                  {"MOB_TQ_WESTGUARDIAN", {-15163, -10, 1733}},
                                                                                  {"MOB_TQ_NORTHGUARDIAN", {267, -10, 15848}}}};
    for (const auto& [code, at] : guardians) {
        const proto::WorldPosition place = inLayer({server::QinTomb::kFloor5, at[0], at[1], at[2]}, qin.Floor5());
        std::uint32_t guardian = 0;
        REQUIRE_MESSAGE(pump([&] { return (guardian = find(code, place, 400.0f)) != 0; }), code);
        CHECK(world.Where(guardian)->position.layer == qin.Floor5());
        kill(guardian, me);
    }
    REQUIRE(pump([&] { return qin.Started("Chins_Floor5_FlameMaster"); }));
    for (const char* s : {"Chins_Floor5_East", "Chins_Floor5_South", "Chins_Floor5_West", "Chins_Floor5_North"}) CHECK(qin.Cleared(s));
    std::uint32_t flameMaster = 0;
    const proto::WorldPosition flamePlace = inLayer({server::QinTomb::kFloor5, -1210.0f, -51.0f, -932.0f}, qin.Floor5());
    REQUIRE(pump([&] { return (flameMaster = find("MOB_TQ_FLAMEMASTER", flamePlace, 400.0f)) != 0; }));
    CHECK(qin.Portals(qin.Floor5()).empty());
    kill(flameMaster, me);

    // Its death: three portals into floor 6, the i-th at the drawn order's i-th place.
    REQUIRE(pump([&] { return qin.Portals(qin.Floor5()).size() == 3; }));
    constexpr std::array<std::array<float, 2>, 3> places{{{1498, 1650}, {1815, -1334}, {-1349, 1101}}};
    constexpr std::array<std::array<int, 3>, 6> orders{{{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};
    REQUIRE(qin.Floor5Order() >= 0);
    REQUIRE(qin.Floor5Order() < 6);
    const std::vector<std::uint32_t> portals = qin.Portals(qin.Floor5());
    for (std::size_t i = 0; i < portals.size(); ++i) {
        const auto at = world.Where(portals[i]);
        REQUIRE(at.has_value());
        CHECK(at->position.layer == qin.Floor5());
        const auto& want = places[std::size_t(orders[std::size_t(qin.Floor5Order())][i])];
        CHECK(Flat(at->position, want[0], want[1]) < 5.0);
    }

    // The first portal used from beside it: into floor 6's copy at the first destination (the east side).
    world.Teleport(me, world.Where(portals[0])->position);
    REQUIRE(pump([&] { return world.Where(me) && Flat(world.Where(me)->position, world.Where(portals[0])->position.x, world.Where(portals[0])->position.z) < 30.0; }));
    REQUIRE(instances.Handle(me, proto::EnterGap::kId, proto::Encode(proto::EnterGap{portals[0]})));
    REQUIRE(pump([&] {
        const auto at = world.Where(me);
        return at && at->position.layer == qin.Floor6();
    }));
    CHECK(world.Where(me)->position.region == server::QinTomb::kFloor6);
    CHECK(Flat(world.Where(me)->position, 7086.0f, 73.0f) < 50.0);
    CHECK(instances.LayerOf(me) == qin.Floor6());
    CHECK(instances.Inside(qin.Floor5()) == 0);
    CHECK(instances.Inside(qin.Floor6()) == 1);

    // The Black Snake killed: its portal up (Portal1) and the summit's (Portal4) stand; Portal1 takes the player up.
    std::uint32_t blackSnake = 0;
    const proto::WorldPosition snakePlace = inLayer({server::QinTomb::kFloor6, 8207.0f, 132.0f, -10.0f}, qin.Floor6());
    REQUIRE(pump([&] { return (blackSnake = find("MOB_TQ_BLACKSNAKE", snakePlace, 400.0f)) != 0; }));
    kill(blackSnake, me);
    REQUIRE(pump([&] { return qin.Started("Chins_Floor6_Portal1"); }));
    CHECK(qin.Started("Chins_Floor6_Portal4"));
    CHECK_FALSE(qin.Started("Chins_Floor6_Portal2"));
    const std::vector<std::uint32_t> up = qin.Portals(qin.Floor6());
    REQUIRE(up.size() == 2);
    std::uint32_t portal1 = 0;
    for (const std::uint32_t e : up) {
        if (Flat(world.Where(e)->position, 4585.0f, 39.0f) < 5.0) portal1 = e;
    }
    REQUIRE(portal1 != 0);
    // Too far: nothing happens.
    REQUIRE(instances.Handle(me, proto::EnterGap::kId, proto::Encode(proto::EnterGap{portal1})));
    pump([] { return false; }, 20);
    CHECK(Flat(world.Where(me)->position, 7086.0f, 73.0f) < 50.0);
    world.Teleport(me, world.Where(portal1)->position);
    REQUIRE(pump([&] { return Flat(world.Where(me)->position, 4585.0f, 39.0f) < 30.0; }));
    REQUIRE(instances.Handle(me, proto::EnterGap::kId, proto::Encode(proto::EnterGap{portal1})));
    REQUIRE(pump([&] { return Flat(world.Where(me)->position, 0.0f, 59.0f) < 50.0; }));
    CHECK(world.Where(me)->position.layer == qin.Floor6());
    CHECK(world.Where(me)->position.y > 1500.0f); // the summit

    // The gate shut: the shard told; the copies' last hour (3.6 s here) told at 60, 30, 10 and 5 minutes left, then
    // everyone back on the 4th floor where it came in, the copies gone.
    qin.Shut(server::WorldClock::now());
    CHECK_FALSE(qin.GateOpen());
    REQUIRE(rig.shard.size() == 2);
    CHECK(rig.shard[1] == "UIIT_TQ_F4_TELEPORT_CLOSE");
    CHECK(qin.Admit(me) != proto::GateStatus::Ok);
    REQUIRE(pump([&] { return qin.Floor5() == 0 && qin.Floor6() == 0; }));
    const auto alerts = rig.All<proto::InstanceAlert>();
    REQUIRE(alerts.size() == 4);
    CHECK(alerts[0].seconds >= 3);
    CHECK(alerts[3].seconds == 0);
    REQUIRE(pump([&] {
        const auto at = world.Where(me);
        return at && at->position.layer == 0;
    }));
    CHECK(world.Where(me)->position.region == server::QinTomb::kFloor4);
    CHECK(instances.LayerOf(me) == 0);
    CHECK(instances.Count() == 0);

    // Opened again and shut with nobody inside: the copies end at once.
    REQUIRE(qin.Open(server::WorldClock::now()));
    REQUIRE(qin.Floor5() != 0);
    qin.Shut(server::WorldClock::now());
    REQUIRE(pump([&] { return qin.Floor5() == 0 && qin.Floor6() == 0; }, 50));
    CHECK(instances.Count() == 0);
}

TEST_CASE("Instances: out of the game inside an instance, back inside where it left while the instance stands (game files)") {
    const char* gameDir = std::getenv("SR_GAME_DIR");
    if (!gameDir) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, gameDir, error), error);
    assets::CharacterTable characters;
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    core::JobSystem jobs(2);
    server::WorldConfig config;
    server::World world(config, jobs, &fs);
    server::Monsters monsters(world, characters, skills.get(), {}, 7);
    QinRig rig;
    rig.world = &world;
    server::Instances instances(rig, world, monsters, characters, std::make_shared<server::GameWorldCatalog>(), 11);
    std::vector<server::WorldMessage> out;
    std::vector<server::Monsters::Player> fighters;
    const auto pump = [&](const std::function<bool()>& until, int rounds = 3000) {
        for (int i = 0; i < rounds; ++i) {
            const auto now = server::WorldClock::now();
            world.Update(now, out);
            server::Monsters::Events events;
            monsters.Update(now, fighters, events);
            instances.Tick(now);
            out.clear();
            if (until()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    // A world of its own code's (as the Roc's Peak), its inside on the Roc's mountain, entered from the mountain's gate.
    server::GameWorldSpec spec;
    spec.id = 99;
    spec.codename = "INS_T_LEFT";
    spec.maxUsers = 4;
    spec.durationMs = 3'600'000;
    spec.emptyMs = 3'600'000;
    spec.gapMs = 0;
    const proto::WorldPosition gate{23406, 1382.0f, 4056.0f, 1558.0f};
    const proto::WorldPosition inside{26045, 1027.0f, -41.0f, 822.0f};
    const auto enter = [&](std::vector<server::InstanceEntry> entries) {
        server::WorldPlayer p;
        p.peer = 1;
        p.character = 1001;
        p.position = gate; // kept where it entered from
        p.dropOnGround = true;
        const std::uint32_t e = world.Enter(p);
        instances.Enter(e, 1001, {}, std::move(entries));
        return e;
    };
    std::uint32_t me = enter({});
    REQUIRE(pump([&] { return world.Where(me).has_value(); }));
    const std::uint32_t layer = instances.OpenWorld(spec, "the test");
    REQUIRE(layer != 0);
    REQUIRE(instances.AdmitAt(me, layer, inside));
    REQUIRE(pump([&] { return world.Where(me) && world.Where(me)->position.layer == layer; }));
    REQUIRE_FALSE(rig.entries.empty());
    CHECK(rig.entries.back().active);

    // Out of the game inside, then back while it stands: inside again where it left, its entry still active.
    const proto::WorldPosition leftAt = world.Where(me)->position;
    instances.Leave(me);
    world.Leave(me);
    me = enter({rig.entries.back()});
    REQUIRE(pump([&] { return world.Where(me) && world.Where(me)->position.layer == layer; }));
    CHECK(world.Where(me)->position.region == leftAt.region);
    CHECK(std::hypot(world.Where(me)->position.x - leftAt.x, world.Where(me)->position.z - leftAt.z) < 30.0f);
    CHECK(instances.LayerOf(me) == layer);
    CHECK(instances.Inside(layer) == 1);
    CHECK(rig.entries.back().active);

    // Out again, the instance gone meanwhile: it stays where it entered from, the entry no longer active.
    instances.Leave(me);
    world.Leave(me);
    instances.CloseWorld(layer);
    me = enter({rig.entries.back()});
    REQUIRE(pump([&] { return world.Where(me).has_value(); }, 200));
    pump([] { return false; }, 50);
    CHECK(world.Where(me)->position.layer == 0);
    CHECK(world.Where(me)->position.region == gate.region);
    CHECK(instances.LayerOf(me) == 0);
    CHECK_FALSE(rig.entries.back().active);

    // A quest's own world: its quest ends when its player is out; no coming back.
    const std::uint32_t questLayer = instances.OpenQuestWorld("INS_QNO_T_LEFT", 98, "the test");
    REQUIRE(questLayer != 0);
    REQUIRE(instances.AdmitAt(me, questLayer, inside));
    REQUIRE(pump([&] { return world.Where(me) && world.Where(me)->position.layer == questLayer; }));
    instances.Leave(me);
    world.Leave(me);
    me = enter({rig.entries.back()});
    REQUIRE(pump([&] { return world.Where(me).has_value(); }, 200));
    pump([] { return false; }, 50);
    CHECK(world.Where(me)->position.layer == 0);
}
