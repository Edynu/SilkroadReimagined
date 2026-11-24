// The Roc: the summon skill's groups of four, and the Peak's play on the game files: the gate opened,
// a player through it, the seal stones and their followers, the entrance scene, the Roc with its countdowns, spirits and
// berserk, its death's exit, everyone sent off the Peak at the end. The Peak's times run 1,000 times faster.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <set>
#include <thread>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/InstanceProtocol.h"
#include "sr/server/Instances.h"
#include "sr/server/Monsters.h"
#include "sr/server/Roc.h"
#include "sr/server/World.h"

using namespace sr;
namespace proto = game::proto;

namespace {

struct RocRig : server::InstanceHost, server::RocHost {
    server::World* world = nullptr;
    std::vector<std::pair<proto::MessageId, std::vector<std::byte>>> sent;
    std::vector<std::string> shard;

    std::optional<InstanceHost::Player> InstancePlayer(std::uint32_t entity) override {
        InstanceHost::Player p;
        p.peer = entity;
        p.character = 1000 + entity;
        p.name = "P" + std::to_string(entity);
        p.level = 90;
        return p;
    }
    void InstanceSend(std::uint64_t, proto::MessageId id, const std::vector<std::byte>& payload) override { sent.emplace_back(id, payload); }
    void InstanceTeleport(std::uint32_t entity, const proto::WorldPosition& to) override { world->Teleport(entity, to); }
    std::uint32_t InstanceItemAt(std::uint32_t, std::uint8_t) override { return 0; }
    bool InstanceTakeOne(std::uint32_t, std::uint8_t) override { return false; }
    void InstanceSaveEntry(std::int64_t, const server::InstanceEntry&) override {}
    void InstanceSaveBook(std::int64_t, const server::BookEntry&) override {}
    void InstanceSaveWait(std::int64_t, const server::ItemWait&) override {}
    void InstanceBookChanged(std::uint32_t) override {}
    std::vector<Member> InstancePartyMembers(std::uint32_t) override { return {}; }
    std::uint32_t InstanceEntityOf(std::uint32_t) override { return 0; }
    std::uint16_t InstanceServerId() override { return 1; }
    void InstancePublish(std::string) override {}
    int InstanceMonsterLevel(std::uint32_t) override { return 0; }

    void RocShardNotice(const std::string& key) override { shard.push_back(key); }
    void RocSend(std::uint32_t, proto::MessageId id, const std::vector<std::byte>& payload) override { sent.emplace_back(id, payload); }
    std::optional<proto::WorldPosition> RocPoint(std::uint32_t point) override {
        if (point == server::Roc::kMountainGate) return proto::WorldPosition{23406, 1382.0f, 4056.0f, 1558.0f};
        if (point == server::Roc::kPeakGate) return proto::WorldPosition{26045, 1027.0f, -41.0f, 822.0f};
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

} // namespace

TEST_CASE("Roc: the Peak's play, from the gate to the end (game files)") {
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
    RocRig rig;
    rig.world = &world;
    server::Instances instances(rig, world, monsters, characters, std::make_shared<server::GameWorldCatalog>(), 11);
    server::RocConfig rocConfig;
    rocConfig.enabled = false; // no schedule: opened by hand
    rocConfig.timeScale = 1000.0;
    rocConfig.realSceneTime = false;
    server::Roc roc(rig, instances, monsters, characters, skills.get(), rocConfig, 5);

    std::vector<server::WorldMessage> out;
    std::vector<server::Monsters::Player> fighters;
    const auto pump = [&](const std::function<bool()>& until, int rounds = 4000) {
        for (int i = 0; i < rounds; ++i) {
            const auto now = server::WorldClock::now();
            world.Update(now, out);
            server::Monsters::Events events;
            monsters.Update(now, fighters, events);
            instances.Tick(now);
            roc.Tick(now);
            out.clear();
            if (until()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };

    // A player at the mountain's gate.
    server::WorldPlayer p;
    p.peer = 1;
    p.character = 1001;
    p.position = {23406, 1382.0f, 4056.0f, 1558.0f};
    p.dropOnGround = true;
    const std::uint32_t me = world.Enter(p);
    instances.Enter(me, 1001, {}, {});
    REQUIRE(pump([&] { return world.Where(me).has_value(); }));

    // The gate shut: nobody goes up.
    CHECK_FALSE(roc.Admit(me));
    // Opened: the shard told, a Peak with its 8 stones; the player through it, in the Peak's layer at its arrival.
    REQUIRE(roc.Open(server::WorldClock::now()));
    CHECK(roc.GateOpen());
    REQUIRE(roc.Layer() != 0);
    REQUIRE(rig.shard.size() == 1);
    CHECK(rig.shard[0] == "UIIT_MSG_QUEST_ROC_APPEAR");
    CHECK(roc.Step() == 0);
    CHECK(roc.FollowersLeft() == 8);
    REQUIRE(roc.Admit(me));
    REQUIRE(pump([&] {
        const auto at = world.Where(me);
        return at && at->position.layer == roc.Layer();
    }));
    CHECK(world.Where(me)->position.region == 26045);
    CHECK(instances.LayerOf(me) == roc.Layer());

    // The stones stand; one broken: its follower called by its death skill's summon (MSKILL_RM_SEALSTONE_*_ATTACK01 is a
    // 'D' skill: its group one Wing Tribe Knight or Devil Shaitan, rarity 8).
    std::uint32_t stone = 0;
    REQUIRE(pump([&] {
        proto::WorldPosition arrival{26045, 1027.0f, -41.0f, 822.0f};
        arrival.layer = roc.Layer();
        for (const std::uint32_t m : monsters.Near(arrival, 2000.0f)) {
            if (monsters.RefOf(m) >= 24278 && monsters.RefOf(m) <= 24284) stone = m;
        }
        return stone != 0;
    }));
    const auto summonedBefore = monsters.Alive();
    const proto::WorldPosition stoneAt = world.Where(stone)->position;
    monsters.Damage(stone, me, 1, server::WorldClock::now());
    CHECK(monsters.Alive() == summonedBefore); // a hit calls nobody
    monsters.Damage(stone, me, monsters.Hp(stone), server::WorldClock::now());
    REQUIRE(pump([&] { return monsters.Alive() >= summonedBefore; }, 1500));
    bool knight = false;
    for (int i = 0; i < 1500 && !knight; ++i) {
        for (const std::uint32_t m : monsters.Near(stoneAt, 200.0f)) knight |= monsters.RefOf(m) == 24285;
        if (!knight) pump([] { return false; }, 1);
    }
    for (const std::uint32_t m : monsters.Near(stoneAt, 200.0f)) {
        knight |= monsters.RefOf(m) == 24285;
    }
    CHECK(knight);

    // The 8 followers dead: the stones broken (told on the Peak), the Roc after 10 minutes and its 42 s entrance.
    proto::WorldPosition peak{26045, 1000.0f, 0.0f, 1000.0f};
    peak.layer = roc.Layer();
    for (int i = 0; i < 8; ++i) {
        server::Monsters::Damaged d;
        d.killed = true;
        d.refObjId = i == 0 ? 24260 : 24285;
        roc.Killed(d, peak, server::WorldClock::now());
    }
    CHECK(roc.FollowersLeft() == 0);
    CHECK(roc.Step() == 1);
    const auto notices = rig.All<proto::InstanceNotice>();
    REQUIRE_FALSE(notices.empty());
    CHECK(notices.back().text == "UIIT_MSG_QUEST_ROC_BREAK_STONE");
    REQUIRE(pump([&] { return roc.Step() == 3 && roc.RocEntity() != 0; }));
    const auto scenes = rig.All<proto::QuestScene>();
    REQUIRE(scenes.size() == 1);
    CHECK(scenes[0].name == "roc_entrance");
    CHECK_FALSE(roc.GateOpen()); // the entrance shut it
    CHECK(rig.shard.back() == "UIIT_MSG_QUEST_ROC_DISAPPEAR");
    const std::uint32_t rocEntity = roc.RocEntity();
    CHECK(monsters.RefOf(rocEntity) == 3877);
    const double defenceBefore = monsters.Stats(rocEntity)->phyDefence, hitBefore = monsters.Stats(rocEntity)->hitRate;

    // Its summons by its health: a first hit calls nobody; below 75 % its first summon skill, 20 wing
    // tribe dukes, each on the ring about it, not all on one spot.
    const auto dukes = [&] {
        std::vector<proto::WorldPosition> at;
        for (const std::uint32_t m : monsters.Near(world.Where(rocEntity)->position, 600.0f)) {
            if (monsters.RefOf(m) == 24155) at.push_back(world.Where(m)->position);
        }
        return at;
    };
    monsters.Damage(rocEntity, me, 1, server::WorldClock::now());
    pump([] { return false; }, 300);
    CHECK(dukes().empty());
    const std::uint32_t rocHp = monsters.Hp(rocEntity);
    monsters.Damage(rocEntity, me, rocHp / 3, server::WorldClock::now());
    REQUIRE(pump([&] { return dukes().size() == 20; }, 1500));
    {
        const auto placed = dukes();
        std::set<std::pair<int, int>> spots;
        for (const auto& p : placed) spots.insert({int(p.x), int(p.z)});
        CHECK(spots.size() > 10);
    }

    // Its spirits topped up to three kinds; its berserk countdown, then its berserk an hour after it came.
    REQUIRE(pump([&] { return roc.SpiritKinds() == 3; }));
    // (A monster's attack is its skills'; its defence and hit rate show the berserk's 10 %.)
    REQUIRE(defenceBefore > 0);
    REQUIRE(pump([&] {
        const auto* stats = monsters.Stats(rocEntity);
        return stats != nullptr && stats->phyDefence > defenceBefore;
    }));
    CHECK(monsters.Stats(rocEntity)->phyDefence == doctest::Approx(defenceBefore * 1.1).epsilon(0.01));
    CHECK(monsters.Stats(rocEntity)->hitRate == doctest::Approx(hitBefore * 1.1).epsilon(0.01));
    const auto berserk = rig.All<proto::RocNotice>();
    REQUIRE(berserk.size() >= 7);
    CHECK(berserk[0].kind == proto::RocNotice::kBerserk);
    CHECK(berserk[0].minutes == 30);
    CHECK(berserk[5].minutes == 1);
    CHECK(berserk[6].minutes == proto::RocNotice::kStarted);

    // Killed: an exit where it fell; everyone off the Peak 30 minutes later, to the mountain's gate, the Peak gone.
    server::Monsters::Damaged dead;
    dead.killed = true;
    dead.refObjId = 3877;
    roc.Killed(dead, peak, server::WorldClock::now());
    CHECK(roc.Step() == 5);
    CHECK(instances.ExitsOf(roc.Layer()).size() == 1);
    REQUIRE(pump([&] { return roc.Layer() == 0; }));
    REQUIRE(pump([&] {
        const auto at = world.Where(me);
        return at && at->position.layer == 0;
    }));
    CHECK(world.Where(me)->position.region == 23406);
    CHECK(instances.LayerOf(me) == 0);
    CHECK(instances.Count() == 0);
}
