#include <doctest/doctest.h>

#include "sr/assets/GameData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <random>
#include <set>
#include <thread>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/Terrain.h"
#include "sr/core/Jobs.h"
#include "sr/game/Berserk.h"
#include "sr/game/Heals.h"
#include "sr/game/NavLoader.h"
#include "sr/game/NavMesh.h"
#include "sr/game/NavPath.h"
#include "sr/game/PlayerState.h"
#include "sr/game/WorldProtocol.h"
#include "sr/game/SupportProtocol.h"
#include "sr/game/SimulationClock.h"
#include "sr/game/WorldRecording.h"
#include "sr/game/WorldView.h"
#include "sr/core/Text.h"
#include "sr/server/Drops.h"
#include "sr/server/GameServer.h"
#include "sr/server/Store.h"
#include "sr/server/World.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
using Race = game::CreationRules::Race;

namespace {

proto::WorldPosition At(int rx, int rz, float x, float z, float y = 0.0f) { return {assets::RegionId(rx, rz), x, y, z}; }

double Distance(const proto::WorldPosition& a, const proto::WorldPosition& b) {
    const double ax = (a.region & 0xFF) * 1920.0 + a.x, az = (a.region >> 8) * 1920.0 + a.z;
    const double bx = (b.region & 0xFF) * 1920.0 + b.x, bz = (b.region >> 8) * 1920.0 + b.z;
    return std::hypot(ax - bx, az - bz);
}

// A world driven by a made-up clock, and what each player was told.
struct Harness {
    core::JobSystem jobs{2};
    server::World world;
    server::WorldClock::time_point now = server::WorldClock::now();
    std::map<std::uint64_t, std::vector<server::WorldMessage>> told;
    // What one player is told, kept as the client's recorder keeps it (C6d).
    game::WorldRecording* recording = nullptr;
    std::uint64_t recordPeer = 0;
    server::WorldClock::time_point recordStart = now;

    Harness(double tickRate, const assets::FileSystem* files = nullptr) : world(Config(tickRate), jobs, files) {}
    explicit Harness(const server::WorldConfig& config) : world(config, jobs, nullptr) {}
    static server::WorldConfig Config(double rate) {
        server::WorldConfig c;
        c.tickRate = rate;
        return c;
    }
    void Run(double seconds) {
        const auto end = now + std::chrono::duration_cast<server::WorldClock::duration>(std::chrono::duration<double>(seconds));
        const auto step = std::chrono::duration_cast<server::WorldClock::duration>(std::chrono::duration<double>(1.0 / world.TickRate()));
        do {
            std::vector<server::WorldMessage> out;
            world.Update(now, out);
            for (auto& m : out) {
                if (recording && m.peer == recordPeer)
                    recording->messages.push_back({std::chrono::duration<double>(now - recordStart).count(), m.id, {m.payload.Span().begin(), m.payload.Span().end()}});
                told[m.peer].push_back(std::move(m));
            }
            now += step;
        } while (now <= end);
    }
    template <typename T>
    std::vector<T> Of(std::uint64_t peer) const {
        std::vector<T> out;
        const auto it = told.find(peer);
        if (it == told.end()) return out;
        for (const server::WorldMessage& m : it->second) {
            if (m.id != T::kId) continue;
            T decoded;
            REQUIRE(proto::Decode(m.payload, decoded));
            out.push_back(decoded);
        }
        return out;
    }
    std::uint32_t Enter(std::uint64_t peer, const proto::WorldPosition& at, const char* name) {
        server::WorldPlayer p;
        p.peer = peer;
        p.character = std::int64_t(peer);
        p.summary.name = name;
        p.position = at;
        return world.Enter(p);
    }
};

std::shared_ptr<game::CharacterRules> WorldRules() {
    auto rules = std::make_shared<game::CharacterRules>();
    for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
    rules->creation.AddFigure(100, Race::China, true);
    rules->creation.AddArmourSet(Race::China, true, 1, 2, 3);
    rules->creation.AddWeapon(Race::China, 10);
    rules->creation.AddFigure(200, Race::Europe, true); // a European (the races' resurrections)
    rules->creation.AddArmourSet(Race::Europe, true, 4, 5, 6);
    rules->creation.AddWeapon(Race::Europe, 11);
    return rules;
}

std::filesystem::path GameDir() {
    const char* dir = std::getenv("SR_GAME_DIR");
    return dir ? std::filesystem::path(dir) : std::filesystem::path();
}

} // namespace

TEST_CASE("World: players see each other enter, walk and leave, at 10, 20 and 30 ticks a second") {
    for (const double rate : {10.0, 20.0, 30.0}) {
        CAPTURE(rate);
        Harness h(rate);
        const std::uint32_t a = h.Enter(1, At(100, 100, 500, 500), "Alpha");
        const std::uint32_t b = h.Enter(2, At(100, 100, 600, 500), "Beta");
        const std::uint32_t far = h.Enter(3, At(104, 100, 500, 500), "Far"); // 4 regions east: out of view
        h.Run(0.2);
        // Each is told of itself first, then of the other.
        REQUIRE(h.Of<proto::WorldEntered>(1).size() == 1);
        CHECK(h.Of<proto::WorldEntered>(1)[0].entity == a);
        CHECK(h.Of<proto::WorldEntered>(1)[0].runSpeed == doctest::Approx(50.0f));
        REQUIRE(h.Of<proto::EntityAppeared>(1).size() == 1);
        CHECK(h.Of<proto::EntityAppeared>(1)[0].entity == b);
        CHECK(h.Of<proto::EntityAppeared>(1)[0].character.name == "Beta");
        REQUIRE(h.Of<proto::EntityAppeared>(2).size() == 1);
        CHECK(h.Of<proto::EntityAppeared>(2)[0].entity == a);
        CHECK(h.Of<proto::EntityAppeared>(3).empty());

        // Alpha walks 300 units north: Beta hears of the leg; after 2 s Alpha is there.
        h.world.MoveTo(a, At(100, 100, 500, 800));
        h.Run(1.0);
        const auto legs = h.Of<proto::EntityMoved>(2);
        REQUIRE(!legs.empty());
        CHECK(legs.back().entity == a);
        CHECK(legs.back().speed == doctest::Approx(50.0f));
        CHECK(Distance(legs.back().to, At(100, 100, 500, 800)) < 0.01);
        // Real time, whatever the rate: about 50 units a second.
        const auto place = h.world.Where(a);
        REQUIRE(place);
        CHECK(Distance(place->position, At(100, 100, 500, 500)) == doctest::Approx(50.0).epsilon(0.1));
        CHECK(!h.Of<proto::EntityMoved>(1).empty()); // the walker hears of its own leg
        h.Run(6.0);
        CHECK(!h.world.Moving(a));
        CHECK(Distance(h.world.Where(a)->position, At(100, 100, 500, 800)) < 0.01);

        // A click farther than a click can reach is not taken.
        h.world.MoveTo(b, At(110, 100, 0, 0));
        h.Run(0.2);
        CHECK(!h.world.Moving(b));

        // Far walks into view (two clicks: one reaches 3,840 at most; it ends 2,820 from Beta, inside
        // the view's radius of 3,000): both sides are told; then it leaves the world: gone.
        h.world.MoveTo(far, At(102, 100, 900, 500));
        h.Run(70.0);
        h.world.MoveTo(far, At(101, 100, 1500, 500));
        h.Run(30.0);
        bool seen = false;
        for (const auto& m : h.Of<proto::EntityAppeared>(2)) seen = seen || m.entity == far;
        CHECK(seen);
        h.world.Leave(far);
        h.Run(0.2);
        bool gone = false;
        for (const auto& m : h.Of<proto::EntityGone>(2)) gone = gone || m.entity == far;
        CHECK(gone);
        CHECK(h.world.Entities() == 2);
    }
}

TEST_CASE("World: a teleport inside someone's view tells them where it stands now") {
    Harness h(20.0);
    const std::uint32_t a = h.Enter(1, At(100, 100, 500, 500), "Alpha");
    h.Enter(2, At(100, 100, 600, 500), "Beta");
    h.Run(0.2);
    REQUIRE(h.Of<proto::EntityAppeared>(2).size() == 1);
    // A short hop: Alpha never leaves Beta's view, so it does not appear anew; Beta must still hear of the place.
    REQUIRE(h.world.Teleport(a, At(100, 100, 650, 520)));
    h.Run(0.2);
    CHECK(h.Of<proto::EntityAppeared>(2).size() == 1);
    const auto legs = h.Of<proto::EntityMoved>(2);
    REQUIRE(!legs.empty());
    CHECK(legs.back().entity == a);
    CHECK(legs.back().speed == doctest::Approx(0.0f));
    CHECK(Distance(legs.back().from, At(100, 100, 650, 520)) < 0.01);
    CHECK(Distance(legs.back().to, At(100, 100, 650, 520)) < 0.01);
}

TEST_CASE("World: a view takes each kind up to its share, lends the unused places, ranks tiers and keeps what it sees") {
    server::WorldConfig config;
    config.tickRate = 20.0;
    config.viewRadius = 3000.0f;
    config.viewPlayers = 2;
    config.viewMonsters = 1; // none in this world: its place is lent
    config.viewItems = 1;
    config.viewTier1 = 1;
    config.viewTier2 = 1;
    config.viewKeep = 0.1f;
    Harness h(config);
    // A watcher, three players east of it at 100, 200 and 300, two items at 150 and 250, and a
    // player beyond the radius.
    const std::uint32_t a = h.Enter(1, At(100, 100, 500, 500), "Alpha");
    const std::uint32_t b = h.Enter(2, At(100, 100, 600, 500), "Beta");
    const std::uint32_t c = h.Enter(3, At(100, 100, 700, 500), "Gamma");
    const std::uint32_t d = h.Enter(4, At(100, 100, 800, 500), "Delta");
    const std::uint32_t i1 = h.world.AddGroundItem(62, At(100, 100, 650, 500), 0.0f, 1, 0);
    const std::uint32_t i2 = h.world.AddGroundItem(62, At(100, 100, 750, 500), 0.0f, 1, 0);
    const std::uint32_t beyond = h.Enter(5, At(102, 100, 0, 500), "Beyond"); // 3,340 away
    h.Run(0.2);
    std::set<std::uint32_t> seen;
    for (const auto& m : h.Of<proto::EntityAppeared>(1)) seen.insert(m.entity);
    // Players: Beta and Gamma (the share of 2); items: the nearer (the share of 1); the monsters'
    // unused place goes to the nearest left over, the farther item (250) before Delta (300).
    CHECK(seen == std::set<std::uint32_t>{b, c, i1, i2});
    CHECK(!seen.contains(d));
    CHECK(!seen.contains(beyond));
    // Tiers by rank: Beta (100) the nearest, the nearer item (150) next, Gamma (200) the rest.
    const auto hears = [&](std::uint32_t entity, int maxTier) {
        bool told = false;
        h.world.ForEachObserverWithin(entity, maxTier, [&](std::uint64_t peer) { told = told || peer == 1; });
        return told;
    };
    CHECK(hears(b, 0));
    CHECK(!hears(c, 1));
    CHECK(hears(c, 2));
    // Gamma's new leg reaches Alpha on a farther tier's tick (every 4th), not later.
    const std::size_t before = h.Of<proto::EntityMoved>(1).size();
    h.world.MoveTo(c, At(100, 100, 700, 520));
    h.Run(0.25);
    bool legOfGamma = false;
    const auto legs = h.Of<proto::EntityMoved>(1);
    for (std::size_t k = before; k < legs.size(); ++k) legOfGamma = legOfGamma || legs[k].entity == c;
    CHECK(legOfGamma);
    // Delta walks to just nearer than Gamma (200.0 against 201.0): Gamma, already seen, ranks 10 %
    // nearer and keeps its place in the players' share (no flicker at the edge of a share). Delta
    // takes the lent place instead, from the farther item (250), which goes.
    h.world.MoveTo(d, At(100, 100, 699, 520));
    h.Run(5.0);
    const auto appeared = [&](std::uint32_t entity) {
        bool found = false;
        for (const auto& m : h.Of<proto::EntityAppeared>(1)) found = found || m.entity == entity;
        return found;
    };
    const auto went = [&](std::uint32_t entity) {
        bool found = false;
        for (const auto& m : h.Of<proto::EntityGone>(1)) found = found || m.entity == entity;
        return found;
    };
    CHECK(!went(c));
    CHECK(appeared(d));
    CHECK(went(i2));
    // Gamma leaves the world: Delta takes its place in the players' share, and the lent place goes
    // back to the item.
    const std::size_t appearedBefore = h.Of<proto::EntityAppeared>(1).size();
    h.world.Leave(c);
    h.Run(0.2);
    bool itemBack = false;
    const auto now = h.Of<proto::EntityAppeared>(1);
    for (std::size_t k = appearedBefore; k < now.size(); ++k) itemBack = itemBack || now[k].entity == i2;
    CHECK(itemBack);
    CHECK(h.world.PeerOf(a) == 1);
    CHECK(h.world.PeerOf(i1) == 0);
}

TEST_CASE("World: a player lowers their own view (Online options), never above the server's shares") {
    server::WorldConfig config;
    config.tickRate = 20.0;
    config.viewRadius = 3000.0f;
    config.viewPlayers = 2;
    config.viewMonsters = 2; // none in this world: two places lent
    config.viewItems = 2;
    Harness h(config);
    const std::uint32_t a = h.Enter(1, At(100, 100, 500, 500), "Alpha");
    const std::uint32_t b = h.Enter(2, At(100, 100, 600, 500), "Beta");
    const std::uint32_t c = h.Enter(3, At(100, 100, 700, 500), "Gamma");
    const std::uint32_t d = h.Enter(4, At(100, 100, 800, 500), "Delta");
    const std::uint32_t i1 = h.world.AddGroundItem(62, At(100, 100, 650, 500), 0.0f, 1, 0);
    const std::uint32_t i2 = h.world.AddGroundItem(62, At(100, 100, 750, 500), 0.0f, 1, 0);
    // Alpha's view as the messages so far leave it.
    const auto view = [&] {
        std::set<std::uint32_t> seen;
        const auto it = h.told.find(1);
        if (it == h.told.end()) return seen;
        for (const server::WorldMessage& m : it->second) {
            if (m.id == proto::EntityAppeared::kId) {
                proto::EntityAppeared e;
                REQUIRE(proto::Decode(m.payload, e));
                seen.insert(e.entity);
            } else if (m.id == proto::EntityGone::kId) {
                proto::EntityGone e;
                REQUIRE(proto::Decode(m.payload, e));
                seen.erase(e.entity);
            }
        }
        return seen;
    };
    h.Run(0.2);
    // The server's shares: Beta and Gamma, Delta in a lent place (no monsters), both items.
    CHECK(view() == std::set<std::uint32_t>{b, c, d, i1, i2});

    // One player at most: only the nearest; the lowered kind takes no lent places (Delta goes too).
    auto applied = h.world.SetViewLimits(a, 1, 0);
    CHECK(applied.players == 1);
    CHECK(applied.monsters == 2);
    h.Run(0.5);
    CHECK(view() == std::set<std::uint32_t>{b, i1, i2});

    // More than the share is the share; 0 is the server's whole share (and its lending) again.
    applied = h.world.SetViewLimits(a, 500, 0);
    CHECK(applied.players == 2);
    h.Run(0.5);
    CHECK(view() == std::set<std::uint32_t>{b, c, d, i1, i2});
    applied = h.world.SetViewLimits(a, 0, 1);
    CHECK(applied.players == 2);
    CHECK(applied.monsters == 1);
    h.Run(0.5);
    CHECK(view() == std::set<std::uint32_t>{b, c, d, i1, i2});

    // Its own radius (the Online page's "how far"): what is beyond it goes; 0 is the server's again,
    // and asking for more than the server's reach is the server's.
    h.world.SetViewLimits(a, 0, 0, 120.0f);
    h.Run(0.5);
    CHECK(view() == std::set<std::uint32_t>{b});
    h.world.SetViewLimits(a, 0, 0, 90000.0f);
    h.Run(0.5);
    CHECK(view() == std::set<std::uint32_t>{b, c, d, i1, i2});
    h.world.SetViewLimits(a, 0, 0, 120.0f);
    h.Run(0.5);
    h.world.SetViewLimits(a, 0, 0);
    h.Run(0.5);
    CHECK(view() == std::set<std::uint32_t>{b, c, d, i1, i2});

    // Only a player's own view changes (Beta still sees everyone), and not for an item.
    CHECK(h.world.SetViewLimits(i1, 1, 1).players == 0);
    std::set<std::uint32_t> seenByBeta;
    for (const auto& m : h.Of<proto::EntityAppeared>(2)) seenByBeta.insert(m.entity);
    CHECK(seenByBeta.contains(a));
    CHECK(seenByBeta.contains(d));
}

TEST_CASE("World: a ground item is told as just fallen to those who see it fall, as lying there to those who come later") {
    // The message carries it both ways.
    proto::EntityAppeared sent;
    sent.entity = 7;
    sent.kind = proto::EntityKind::GroundItem;
    sent.character.model = 1;
    sent.amount = 500;
    sent.fresh = true;
    proto::EntityAppeared back;
    REQUIRE(proto::Decode(proto::Encode(sent), back));
    CHECK(back.fresh);
    CHECK(back.amount == 500);
    sent.fresh = false;
    REQUIRE(proto::Decode(proto::Encode(sent), back));
    CHECK(!back.fresh);

    Harness h(20.0);
    const std::uint32_t a = h.Enter(1, At(100, 100, 500, 500), "Alpha");
    h.Run(0.2);
    const auto told = [&](std::uint64_t peer, std::uint32_t entity) {
        std::vector<proto::EntityAppeared> out;
        for (const auto& m : h.Of<proto::EntityAppeared>(peer)) {
            if (m.entity == entity) out.push_back(m);
        }
        return out;
    };
    // Gold falls by Alpha: told as fallen, once.
    const std::uint32_t gold = h.world.AddGroundItem(1, At(100, 100, 520, 500), 0.0f, 500, 0);
    h.Run(0.5);
    REQUIRE(told(1, gold).size() == 1);
    CHECK(told(1, gold)[0].fresh);
    CHECK(told(1, gold)[0].kind == proto::EntityKind::GroundItem);
    // Beta comes later: it lies there for Beta. Neither player is ever "fresh".
    const std::uint32_t b = h.Enter(2, At(100, 100, 540, 500), "Beta");
    h.Run(0.2);
    REQUIRE(told(2, gold).size() == 1);
    CHECK(!told(2, gold)[0].fresh);
    REQUIRE(told(2, a).size() == 1);
    CHECK(!told(2, a)[0].fresh);
    REQUIRE(told(1, b).size() == 1);
    CHECK(!told(1, b)[0].fresh);
    // A second drop while both watch: both see it fall.
    const std::uint32_t sword = h.world.AddGroundItem(62, At(100, 100, 530, 510), 0.0f, 1, 0);
    h.Run(0.2);
    REQUIRE(told(1, sword).size() == 1);
    REQUIRE(told(2, sword).size() == 1);
    CHECK(told(1, sword)[0].fresh);
    CHECK(told(2, sword)[0].fresh);
    // The client's picture keeps it.
    game::WorldView view;
    view.Apply(game::TitleEvent{told(2, sword)[0]});
    view.Apply(game::TitleEvent{told(2, gold)[0]});
    REQUIRE(view.Find(sword) != nullptr);
    CHECK(view.Find(sword)->fresh);
    CHECK(!view.Find(gold)->fresh);
}

TEST_CASE("World: a walk ends where the navigation is blocked (game files)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    Harness h(20.0, &fs);
    // Constantinople, where a European character first enters.
    server::WorldPlayer p;
    p.peer = 1;
    p.position = At(79, 105, 335, 1460, 140);
    p.dropOnGround = true;
    const std::uint32_t walker = h.world.Enter(p);
    for (int i = 0; i < 200 && h.Of<proto::WorldEntered>(1).empty(); ++i) {
        h.Run(0.05);
        std::this_thread::sleep_for(10ms); // the navigation loads on the jobs
    }
    REQUIRE(h.Of<proto::WorldEntered>(1).size() == 1);
    const proto::WorldPosition start = h.Of<proto::WorldEntered>(1)[0].position;
    MESSAGE("entered at y " << start.y);

    // The same navigation, loaded apart, says where each walk ends.
    core::JobSystem jobs(2);
    game::NavLoader loader(fs, jobs);
    game::NavWorld nav;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) loader.Request(assets::RegionId(79 + dx, 105 + dz));
    }
    for (int i = 0; i < 500 && loader.GetStats().loading > 0; ++i) {
        std::this_thread::sleep_for(10ms);
        loader.Update(nav);
    }
    loader.Update(nav);
    int blocked = 0;
    for (int k = 0; k < 16; ++k) {
        const float angle = float(k) * 0.3927f;
        const proto::WorldPosition target = At(79, 105, start.x + 400.0f * std::sin(angle), start.z + 400.0f * std::cos(angle));
        h.world.MoveTo(walker, target);
        h.Run(0.05);
        const auto legs = h.Of<proto::EntityMoved>(1);
        REQUIRE(!legs.empty());
        const proto::EntityMoved& leg = legs.back();
        game::NavPosition from = nav.Drop(start.region, start.x, start.y, start.z), end;
        REQUIRE(from.Valid());
        nav.Walk(from, target.region, target.x, target.z, end);
        CHECK(Distance(leg.to, {end.region, end.x, end.y, end.z}) < 0.5);
        if (Distance(leg.to, target) > 1.0) ++blocked;
        // Back to the start for the next direction.
        h.Run(10.0);
        h.world.MoveTo(walker, start);
        h.Run(10.0);
        REQUIRE(Distance(h.world.Where(walker)->position, start) < 1.0);
        h.told.clear();
    }
    MESSAGE(blocked << " of 16 walks stopped short by the town's walls");
    CHECK(blocked > 0);

    // A click beyond one walk's reach (2,500 east, into the next region but one): not lost, a walk
    // toward it.
    REQUIRE(h.world.MoveTo(walker, At(79, 105, start.x + 2500.0f, start.z)) == server::World::MoveRefused::None);
    h.Run(0.05);
    const auto legs = h.Of<proto::EntityMoved>(1);
    REQUIRE(!legs.empty());
    const proto::FlatPosition a = proto::ToFlat(start), b = proto::ToFlat(legs.back().to);
    CHECK(b.x > a.x + 1.0);
    CHECK(std::hypot(b.x - a.x, b.z - a.z) < 1920.0);
}

TEST_CASE("World: a walk toward a heading (a click on the sky) goes on leg after leg until another walk (the user's report 2026-11-14)") {
    Harness h(20.0);
    server::WorldPlayer p;
    p.peer = 1;
    p.position = At(100, 100, 1000, 1000);
    const std::uint32_t walker = h.world.Enter(p);
    h.Run(0.1);
    REQUIRE(h.world.MoveToward(walker, At(100, 100, 1800, 1000)) == server::World::MoveRefused::None);
    CHECK(h.world.WalksToward(walker));
    // Three walks' reach at 50 a second: well past the first leg's end, on the same line.
    h.Run(3.0 * 1880.0 / 50.0);
    const proto::FlatPosition a = proto::ToFlat(At(100, 100, 1000, 1000)), b = proto::ToFlat(h.world.Where(walker)->position);
    CHECK(b.x - a.x > 2.0 * 1880.0);
    CHECK(std::abs(b.z - a.z) < 1.0);
    CHECK(h.world.WalksToward(walker));
    // Another click ends it.
    h.world.MoveTo(walker, h.world.Where(walker)->position);
    CHECK_FALSE(h.world.WalksToward(walker));
    h.Run(1.0);
    CHECK_FALSE(h.world.Moving(walker));
}

TEST_CASE("World: a walk toward a heading stops at the first wall (game files; the user's report 2026-11-14)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    Harness h(20.0, &fs);
    // Constantinople, inside its walls.
    server::WorldPlayer p;
    p.peer = 1;
    p.position = At(79, 105, 335, 1460, 140);
    p.dropOnGround = true;
    const std::uint32_t walker = h.world.Enter(p);
    for (int i = 0; i < 200 && h.Of<proto::WorldEntered>(1).empty(); ++i) {
        h.Run(0.05);
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(h.Of<proto::WorldEntered>(1).size() == 1);
    const proto::WorldPosition start = h.Of<proto::WorldEntered>(1)[0].position;
    core::JobSystem jobs(2);
    game::NavLoader loader(fs, jobs);
    game::NavWorld nav;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) loader.Request(assets::RegionId(79 + dx, 105 + dz));
    }
    for (int i = 0; i < 500 && loader.GetStats().loading > 0; ++i) {
        std::this_thread::sleep_for(10ms);
        loader.Update(nav);
    }
    loader.Update(nav);
    int checked = 0;
    for (int k = 0; k < 4; ++k) {
        // A walker of its own for each way, from the same start.
        std::uint32_t one = walker;
        if (k > 0) {
            server::WorldPlayer q = p;
            q.peer = std::uint64_t(10 + k);
            one = h.world.Enter(q);
            for (int i = 0; i < 200 && h.Of<proto::WorldEntered>(q.peer).empty(); ++i) {
                h.Run(0.05);
                std::this_thread::sleep_for(10ms);
            }
            REQUIRE(h.Of<proto::WorldEntered>(q.peer).size() == 1);
        }
        const float angle = float(k) * 1.5708f;
        const float hx = std::sin(angle), hz = std::cos(angle);
        if (const auto why = h.world.MoveToward(one, At(79, 105, start.x + 300.0f * hx, start.z + 300.0f * hz)); why != server::World::MoveRefused::None) {
            MESSAGE("way " << k << ": refused at the start (" << int(why) << "), as a click that way is");
            CHECK_FALSE(h.world.WalksToward(one));
            continue;
        }
        // Walked until it stops; then the navigation ahead given time to load (on a busy machine the walker can reach
        // a region not loaded yet and stop at its edge) and the same way asked again, until it truly goes no further.
        for (int again = 0; again < 8; ++again) {
            for (int i = 0; i < 4000 && h.world.WalksToward(one); ++i) {
                h.Run(0.05);
                if (i % 20 == 0) std::this_thread::sleep_for(2ms); // the navigation ahead loads on the jobs
            }
            h.Run(0.2);
            for (int i = 0; i < 30; ++i) {
                h.Run(0.05);
                std::this_thread::sleep_for(10ms);
            }
            const proto::WorldPosition here = h.world.Where(one)->position;
            const proto::FlatPosition f = proto::ToFlat(here);
            const proto::WorldPosition on = proto::FromFlat({f.x + 300.0 * hx, f.z + 300.0 * hz}, here.y);
            if (h.world.MoveToward(one, on) != server::World::MoveRefused::None || !h.world.WalksToward(one)) break;
            h.Run(0.1);
            const proto::WorldPosition moved = h.world.Where(one)->position;
            if (Distance(moved, here) < 1.0) {
                (void)h.world.MoveTo(one, moved); // stopped again where it stood: the wall
                h.Run(0.2);
                break;
            }
        }
        CHECK_FALSE(h.world.WalksToward(one));
        CHECK_FALSE(h.world.Moving(one));
        // It stands where the way on is blocked: a step of 10 more along it goes nowhere near as far.
        const proto::WorldPosition end = h.world.Where(one)->position;
        MESSAGE("way " << k << ": stood at " << (end.region & 0xFF) << "," << (end.region >> 8) << " " << end.x << ", " << end.z << ", "
                       << Distance(end, start) << " from the start");
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) loader.Request(assets::RegionId((end.region & 0xFF) + dx, (end.region >> 8) + dz));
        }
        for (int i = 0; i < 500 && loader.GetStats().loading > 0; ++i) {
            std::this_thread::sleep_for(10ms);
            loader.Update(nav);
        }
        loader.Update(nav);
        if (const game::NavPosition from = nav.Drop(end.region, end.x, end.y, end.z); from.Valid()) {
            const proto::FlatPosition e = proto::ToFlat(end);
            const proto::WorldPosition ahead = proto::FromFlat({e.x + 10.0 * hx, e.z + 10.0 * hz}, end.y);
            game::NavPosition stop;
            nav.Walk(from, ahead.region, ahead.x, ahead.z, stop);
            CHECK(Distance({stop.region, stop.x, stop.y, stop.z}, end) < 5.0);
            ++checked;
        }
    }
    MESSAGE(checked << " ways walked to a wall");
    CHECK(checked >= 2);
}

TEST_CASE("Game server: into the world, two players see each other walk, places kept") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    core::JobSystem jobs(2);
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.rules = WorldRules();
    config.reportInterval = 50ms;
    config.jobs = &jobs; // no navigation: walks go straight
    REQUIRE_MESSAGE(game.Start(config, error), error);
    net::NetService net(1);
    const auto pump = [&] { game.Poll(2ms); };

    // A player: account, ticket, lobby, a character selected.
    const auto player = [&](const std::string& account, const std::string& name) {
        REQUIRE(server::CreateAccount(t.Db(), account, "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        const std::int64_t id = server::FindAccount(t.Db(), account, ok)->id;
        auto c = std::make_unique<Client>(net, game.Port(), pump);
        REQUIRE(c->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
        c->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(c->Expect(entered));
        REQUIRE(entered.status == proto::LobbyEnterStatus::Accepted);
        proto::CharacterCreate m;
        m.name = name;
        m.model = 100;
        m.chest = 1;
        m.legs = 2;
        m.shoes = 3;
        m.weapon = 10;
        c->Send(m);
        proto::LobbyResult r;
        REQUIRE(c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        c->Send(proto::CharacterSelect{name});
        REQUIRE(c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        return c;
    };
    auto a = player("alpha", "Alpha");
    auto b = player("beta", "Beta");

    // Into the world: each is told of itself, in Jangan (a Chinese figure's town), then of the other.
    a->Send(proto::WorldEnter{});
    proto::WorldEntered ea;
    REQUIRE(a->Expect(ea));
    REQUIRE(ea.status == proto::WorldStatus::Ok);
    CHECK(ea.character.name == "Alpha");
    CHECK(ea.position.region == assets::RegionId(168, 98));
    b->Send(proto::WorldEnter{});
    proto::WorldEntered eb;
    REQUIRE(b->Expect(eb));
    proto::EntityAppeared seen;
    REQUIRE(a->Expect(seen));
    CHECK(seen.entity == eb.entity);
    CHECK(seen.character.name == "Beta");
    REQUIRE(b->Expect(seen));
    CHECK(seen.entity == ea.entity);

    // Alpha walks: Beta sees the leg.
    proto::MoveTo move;
    move.target = ea.position;
    move.target.x += 100.0f;
    a->Send(move);
    proto::EntityMoved leg;
    REQUIRE(b->Expect(leg));
    CHECK(leg.entity == ea.entity);
    CHECK(Distance(leg.to, move.target) < 0.05); // a leg's places go packed (to 1/32 of a unit)
    REQUIRE(a->Expect(leg)); // and so does Alpha

    // Alpha's own view (the Online options): lowered for players, the server's share for monsters
    // (0), never above the shares; the answer says what applies and what the server allows.
    a->Send(proto::ViewLimits{50, 0});
    proto::ViewLimitsAnswer view;
    REQUIRE(a->Expect(view));
    CHECK(view.players == 50);
    CHECK(view.monsters == config.world.viewMonsters);
    CHECK(view.maxPlayers == config.world.viewPlayers);
    CHECK(view.maxMonsters == config.world.viewMonsters);
    CHECK(view.radius == std::uint16_t(config.world.viewRadius / 10.0f));
    a->Send(proto::ViewLimits{60000, 1});
    REQUIRE(a->Expect(view));
    CHECK(view.players == config.world.viewPlayers);
    CHECK(view.monsters == 1);

    // Out of the world after the walk: the place is kept for the next entry.
    const auto until = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < until) pump();
    a->Send(proto::WorldLeave{});
    proto::WorldLeft left;
    REQUIRE(a->Expect(left));
    proto::EntityGone gone;
    REQUIRE(b->Expect(gone));
    CHECK(gone.entity == ea.entity);
    for (int i = 0; i < 50; ++i) pump(); // the save on a database thread
    a->Send(proto::WorldEnter{});
    proto::WorldEntered again;
    REQUIRE(a->Expect(again));
    CHECK(Distance(again.position, move.target) < 0.01);
    game.Stop();
}

TEST_CASE("Game server: a death's penalty, a resurrection offered, accepted, the experience given back (game files; K9)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    const assets::SkillRow* rise = skills->Find(std::string_view("SKILL_CH_WATER_RESURRECTION_A_01")); // resu 27 1, heal 0 10 0 10
    REQUIRE(rise != nullptr);
    core::JobSystem jobs(2);
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.rules = WorldRules();
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.devCommands = true;
    config.skills = skills;
    // Level 20: its experience 1,000,000, a monster of it 500: a death takes 19,999 (2 %).
    config.levels.assign(40, 2'000'000);
    config.levels[20] = 1'000'000;
    config.monsterExperience.assign(40, 500);
    REQUIRE_MESSAGE(game.Start(config, error), error);
    net::NetService net(1);
    const auto pump = [&] { game.Poll(2ms); };
    const auto player = [&](const std::string& account, const std::string& name, std::int64_t& character) {
        REQUIRE(server::CreateAccount(t.Db(), account, "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        const std::int64_t id = server::FindAccount(t.Db(), account, ok)->id;
        auto c = std::make_unique<Client>(net, game.Port(), pump);
        REQUIRE(c->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
        c->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(c->Expect(entered));
        proto::CharacterCreate m;
        m.name = name;
        m.model = 100;
        m.chest = 1;
        m.legs = 2;
        m.shoes = 3;
        m.weapon = 10;
        c->Send(m);
        proto::LobbyResult r;
        REQUIRE(c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        const std::vector<server::CharacterRow> list = server::ListCharacters(t.Db(), id, config.id, 0, ok);
        REQUIRE(list.size() == 1);
        character = list[0].id;
        return c;
    };
    std::int64_t healer = 0, fallen = 0;
    auto a = player("alpha", "Alpha", healer);
    auto b = player("beta", "Beta", fallen);
    // Alpha knows the water's first resurrection; Beta is of level 20 with 100,000 experience.
    REQUIRE(server::SaveMastery(t.Db(), healer, rise->masteries[0], 1));
    REQUIRE(server::SaveSkill(t.Db(), healer, 0, rise->id));
    REQUIRE(server::SaveProgress(t.Db(), {{fallen, 20, 100'000, 0, 0, 40, 40, 0}}));
    proto::LobbyResult r;
    for (auto* c : {a.get(), b.get()}) {
        c->Send(proto::CharacterSelect{c == a.get() ? "Alpha" : "Beta"});
        REQUIRE(c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
    }
    a->Send(proto::WorldEnter{});
    proto::WorldEntered ea, eb;
    REQUIRE(a->Expect(ea));
    b->Send(proto::WorldEnter{});
    REQUIRE(b->Expect(eb));
    REQUIRE(eb.character.level == 20);
    // The Progress Beta is told last (an entry's, then the death's, then the resurrection's).
    const auto progressOf = [&](std::uint64_t experience) {
        proto::Progress p;
        for (int i = 0; i < 6; ++i) {
            if (!b->Expect(p, 3s)) return false;
            if (p.experience == experience) return true;
        }
        return false;
    };
    // Beta dies (a developer's death, as a monster's hit would kill it): 19,999 experience off.
    b->Send(proto::DevDie{});
    REQUIRE(progressOf(100'000 - 19'999));
    // Alpha casts the resurrection on it: Beta is offered it, and accepts.
    a->Send(proto::UseSkill{rise->id, eb.entity});
    proto::SkillUseAnswer used;
    REQUIRE(a->Expect(used));
    CHECK(used.status == proto::SkillUseStatus::Ok);
    proto::RevivalOffered offer;
    REQUIRE(b->Expect(offer));
    CHECK(offer.caster == ea.entity);
    CHECK(offer.skill == rise->id);
    b->Send(proto::RevivalAnswer{1});
    // Up with a tenth of its most HP and MP, and 1 % of what the death took back (200, rounded up).
    // The rise's Vitals among those told (the entry's came before, the recovery's ticks may follow).
    proto::Vitals v;
    bool any = false;
    for (int i = 0; i < 12 && !any; ++i) {
        any = b->Expect(v, 1s) && v.hp > 0 && v.hp == game::RevivalFor(game::ReviveOf(*rise), v.maxHp, v.maxMp, 19'999).hp;
    }
    REQUIRE(any);
    const game::Revival expected = game::RevivalFor(game::ReviveOf(*rise), v.maxHp, v.maxMp, 19'999);
    CHECK(v.hp == expected.hp);
    CHECK(v.mp == expected.mp);
    CHECK(expected.experience == 200u);
    CHECK(progressOf(100'000 - 19'999 + 200));
    game.Stop();
}

TEST_CASE("Game server: a death's penalty larger than the level's experience takes the character a level down (game files; the user's report 2026-11-13)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    const assets::SkillRow* rise = skills->Find(std::string_view("SKILL_CH_WATER_RESURRECTION_A_01")); // resu 27 1, heal 0 10 0 10
    REQUIRE(rise != nullptr);
    core::JobSystem jobs(2);
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.rules = WorldRules();
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.devCommands = true;
    config.skills = skills;
    // Level 20: its experience 1,000,000, a monster of it 500: a death takes 19,999 (2 %).
    config.levels.assign(40, 2'000'000);
    config.levels[20] = 1'000'000;
    config.monsterExperience.assign(40, 500);
    REQUIRE_MESSAGE(game.Start(config, error), error);
    net::NetService net(1);
    const auto pump = [&] { game.Poll(2ms); };
    const auto player = [&](const std::string& account, const std::string& name, std::int64_t& character) {
        REQUIRE(server::CreateAccount(t.Db(), account, "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        const std::int64_t id = server::FindAccount(t.Db(), account, ok)->id;
        auto c = std::make_unique<Client>(net, game.Port(), pump);
        REQUIRE(c->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
        c->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(c->Expect(entered));
        proto::CharacterCreate m;
        m.name = name;
        m.model = 100;
        m.chest = 1;
        m.legs = 2;
        m.shoes = 3;
        m.weapon = 10;
        c->Send(m);
        proto::LobbyResult r;
        REQUIRE(c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        const std::vector<server::CharacterRow> list = server::ListCharacters(t.Db(), id, config.id, 0, ok);
        REQUIRE(list.size() == 1);
        character = list[0].id;
        return c;
    };
    std::int64_t healer = 0, fallen = 0;
    auto a = player("alpha", "Alpha", healer);
    auto b = player("beta", "Beta", fallen);
    // Alpha knows the water's first resurrection; Beta is of level 20 with 100,000 experience.
    REQUIRE(server::SaveMastery(t.Db(), healer, rise->masteries[0], 1));
    REQUIRE(server::SaveSkill(t.Db(), healer, 0, rise->id));
    REQUIRE(server::SaveProgress(t.Db(), {{fallen, 20, 5'000, 0, 0, 40, 40, 0}}));
    proto::LobbyResult r;
    for (auto* c : {a.get(), b.get()}) {
        c->Send(proto::CharacterSelect{c == a.get() ? "Alpha" : "Beta"});
        REQUIRE(c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
    }
    a->Send(proto::WorldEnter{});
    proto::WorldEntered ea, eb;
    REQUIRE(a->Expect(ea));
    b->Send(proto::WorldEnter{});
    REQUIRE(b->Expect(eb));
    REQUIRE(eb.character.level == 20);
    // The Progress Beta is told last (an entry's, then the death's, then the resurrection's).
    const auto progressOf = [&](std::uint64_t experience) {
        proto::Progress p;
        for (int i = 0; i < 6; ++i) {
            if (!b->Expect(p, 3s)) return false;
            if (p.experience == experience) return true;
        }
        return false;
    };
    // Beta dies with 5,000 in its level: the 19,999 taken goes past it, level 19 with 2,000,000 - 14,999.
    b->Send(proto::DevDie{});
    proto::Progress p;
    bool down = false;
    for (int i = 0; i < 6 && !down; ++i) {
        if (!b->Expect(p, 3s)) break;
        down = p.level == 19;
    }
    REQUIRE(down);
    CHECK(p.experience == 2'000'000u - 14'999u);
    game.Stop();
}

TEST_CASE("Game server: a resurrection across the races: a Chinese raises a dead European, a European a dead Chinese (game files; K9)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    core::JobSystem jobs(2);
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    // The Cleric's rebirth takes a rod in hand (weapon kind 15): the European starts with one.
    auto rules = WorldRules();
    auto items = std::make_shared<assets::ItemTable>();
    assets::ItemRow rod;
    rod.refObjId = 11;
    rod.codename = "ITEM_EU_TEST_ROD";
    rod.typeId = {3, 1, 6, 15};
    rod.country = 1;
    rod.inService = true;
    items->Add(rod);
    rules->items = items;
    config.rules = rules;
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.devCommands = true;
    config.skills = skills;
    config.levels.assign(40, 2'000'000);
    config.monsterExperience.assign(40, 500);
    REQUIRE_MESSAGE(game.Start(config, error), error);
    net::NetService net(1);
    const auto pump = [&] { game.Poll(2ms); };
    // A player of a figure (100 Chinese, 200 European) in the lobby, its character's id.
    const auto player = [&](const std::string& account, const std::string& name, std::uint32_t model, std::int64_t& character) {
        REQUIRE(server::CreateAccount(t.Db(), account, "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        const std::int64_t id = server::FindAccount(t.Db(), account, ok)->id;
        auto c = std::make_unique<Client>(net, game.Port(), pump);
        REQUIRE(c->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
        c->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(c->Expect(entered));
        proto::CharacterCreate m;
        m.name = name;
        m.model = model;
        m.chest = model == 100 ? 1 : 4;
        m.legs = model == 100 ? 2 : 5;
        m.shoes = model == 100 ? 3 : 6;
        m.weapon = model == 100 ? 10 : 11;
        c->Send(m);
        proto::LobbyResult r;
        REQUIRE(c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        const std::vector<server::CharacterRow> list = server::ListCharacters(t.Db(), id, config.id, 0, ok);
        REQUIRE(list.size() == 1);
        character = list[0].id;
        return c;
    };
    struct Pair {
        const char* skill;
        std::uint32_t healerModel, fallenModel;
        const char *healer, *fallen;
    };
    for (const Pair& pair : {Pair{"SKILL_CH_WATER_RESURRECTION_A_01", 100, 200, "Chinese", "European"},
                             Pair{"SKILL_EU_CLERIC_REBIRTHA_TARGET_A_01", 200, 100, "Cleric", "Blade"}}) {
        CAPTURE(pair.skill);
        const assets::SkillRow* rise = skills->Find(std::string_view(pair.skill));
        REQUIRE(rise != nullptr);
        std::int64_t healer = 0, fallen = 0;
        auto a = player(std::string(pair.healer) + "acct", pair.healer, pair.healerModel, healer);
        auto b = player(std::string(pair.fallen) + "acct", pair.fallen, pair.fallenModel, fallen);
        // The healer knows the resurrection; the fallen is of level 20 (within every first rank's 'resu').
        REQUIRE(server::SaveMastery(t.Db(), healer, rise->masteries[0], std::max(1, rise->masteryLevels[0])));
        REQUIRE(server::SaveSkill(t.Db(), healer, 0, rise->id));
        REQUIRE(server::SaveProgress(t.Db(), {{fallen, 20, 100'000, 0, 0, 40, 40, 0}}));
        // Side by side (the races start in their own towns: out of sight and reach of each other).
        server::CharacterRow::Place here;
        here.region = std::uint16_t(98 << 8 | 168);
        here.x = 900.0f;
        here.z = 900.0f;
        server::CharacterRow::Place beside = here;
        beside.x = 920.0f;
        REQUIRE(server::SavePlaces(t.Db(), {{healer, here}, {fallen, beside}}));
        proto::LobbyResult r;
        a->Send(proto::CharacterSelect{pair.healer});
        REQUIRE(a->Expect(r));
        b->Send(proto::CharacterSelect{pair.fallen});
        REQUIRE(b->Expect(r));
        a->Send(proto::WorldEnter{});
        proto::WorldEntered ea, eb;
        REQUIRE(a->Expect(ea));
        b->Send(proto::WorldEnter{});
        REQUIRE(b->Expect(eb));
        CHECK(ea.character.model == pair.healerModel);
        CHECK(eb.character.model == pair.fallenModel);
        // The healer sees the other (the world's next view), then it dies; the healer casts on it; it is
        // offered the resurrection and accepts.
        proto::EntityAppeared came;
        bool sees = false;
        for (int i = 0; i < 8 && !sees; ++i) sees = a->Expect(came, 1s) && came.entity == eb.entity;
        REQUIRE(sees);
        b->Send(proto::DevDie{});
        // The healer sees it fall (and later rise): the hits told around it.
        proto::EntityDamaged seen;
        bool dead = false;
        for (int i = 0; i < 8 && !dead; ++i) dead = a->Expect(seen, 1s) && seen.target == eb.entity && seen.hp == 0;
        REQUIRE(dead);
        a->Send(proto::UseSkill{rise->id, eb.entity});
        proto::SkillUseAnswer used;
        REQUIRE(a->Expect(used));
        CAPTURE(int(used.status));
        REQUIRE(used.status == proto::SkillUseStatus::Ok);
        proto::RevivalOffered offer;
        REQUIRE(b->Expect(offer));
        CHECK(offer.caster == ea.entity);
        CHECK(offer.skill == rise->id);
        b->Send(proto::RevivalAnswer{1});
        bool up = false;
        for (int i = 0; i < 8 && !up; ++i) up = a->Expect(seen, 1s) && seen.target == eb.entity && seen.hp > 0;
        CHECK(up);
        // Up where it lay: no teleport (that would enter it into the world again, at the return point).
        proto::WorldEntered again;
        CHECK_FALSE(b->Expect(again, 600ms));
        // Both leave for the next pair.
        a->Send(proto::WorldLeave{});
        b->Send(proto::WorldLeave{});
        for (int i = 0; i < 20; ++i) pump();
    }
    game.Stop();
}

TEST_CASE("Game server: a player's buffs are told to those who see it and to newcomers (game files; K10)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    const assets::SkillRow* guard = skills->Find(std::string_view("SKILL_CH_COLD_GANGGI_A_01")); // Weak Guard of Ice: its loop circles the holder
    REQUIRE(guard != nullptr);
    core::JobSystem jobs(2);
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.rules = WorldRules();
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.skills = skills;
    REQUIRE_MESSAGE(game.Start(config, error), error);
    net::NetService net(1);
    const auto pump = [&] { game.Poll(2ms); };
    const auto player = [&](const std::string& account, const std::string& name, std::int64_t& character) {
        REQUIRE(server::CreateAccount(t.Db(), account, "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        const std::int64_t id = server::FindAccount(t.Db(), account, ok)->id;
        auto c = std::make_unique<Client>(net, game.Port(), pump);
        REQUIRE(c->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
        c->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(c->Expect(entered));
        proto::CharacterCreate m;
        m.name = name;
        m.model = 100;
        m.chest = 1;
        m.legs = 2;
        m.shoes = 3;
        m.weapon = 10;
        c->Send(m);
        proto::LobbyResult r;
        REQUIRE(c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        const std::vector<server::CharacterRow> list = server::ListCharacters(t.Db(), id, config.id, 0, ok);
        REQUIRE(list.size() == 1);
        character = list[0].id;
        return c;
    };
    const auto enter = [&](Client& c, const std::string& name, proto::WorldEntered& entered) {
        proto::LobbyResult r;
        c.Send(proto::CharacterSelect{name});
        REQUIRE(c.Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        c.Send(proto::WorldEnter{});
        REQUIRE(c.Expect(entered));
    };
    // The buffs of `entity` as `c` is told them next (none told within the wait: false).
    const auto told = [&](Client& c, std::uint32_t entity, proto::EntityBuffs& out) {
        for (int i = 0; i < 8; ++i) {
            if (!c.Expect(out, 2s)) return false;
            if (out.entity == entity) return true;
        }
        return false;
    };
    std::int64_t caster = 0, watcher = 0, comer = 0;
    auto a = player("alpha", "Alpha", caster);
    auto b = player("beta", "Beta", watcher);
    auto g = player("gamma", "Gamma", comer);
    REQUIRE(server::SaveMastery(t.Db(), caster, guard->masteries[0], 10));
    REQUIRE(server::SaveSkill(t.Db(), caster, 0, guard->id));
    proto::WorldEntered ea, eb, eg;
    enter(*a, "Alpha", ea);
    enter(*b, "Beta", eb);
    // Alpha takes up the guard: Beta, who sees it, is told it holds it.
    a->Send(proto::UseSkill{guard->id, 0});
    proto::SkillUseAnswer used;
    REQUIRE(a->Expect(used));
    REQUIRE_MESSAGE(used.status == proto::SkillUseStatus::Ok, int(used.status));
    proto::BuffAdded added;
    REQUIRE(a->Expect(added, 5s));
    proto::EntityBuffs buffs;
    REQUIRE(told(*b, ea.entity, buffs));
    CHECK(buffs.skills == std::vector<std::uint32_t>{guard->id});
    // Gamma comes after: told it too, after Alpha's appearance.
    enter(*g, "Gamma", eg);
    REQUIRE(told(*g, ea.entity, buffs));
    CHECK(buffs.skills == std::vector<std::uint32_t>{guard->id});
    // Alpha ends it: those who see it are told it holds none.
    a->Send(proto::CancelBuff{added.token});
    REQUIRE(told(*b, ea.entity, buffs));
    CHECK(buffs.skills.empty());
    REQUIRE(told(*g, ea.entity, buffs));
    CHECK(buffs.skills.empty());
    game.Stop();
}

TEST_CASE("World: a recorded session replays to the same picture at every step rate") {
    Harness h(20.0);
    game::WorldRecording recording;
    h.recording = &recording;
    h.recordPeer = 1;
    const std::uint32_t a = h.Enter(1, At(100, 100, 500, 500), "Alpha");
    const std::uint32_t b = h.Enter(2, At(100, 100, 600, 500), "Beta");
    const std::uint32_t c = h.Enter(3, At(100, 100, 700, 700), "Gamma");
    h.Run(0.3);
    // Walks of every kind: one after the other, a new click mid-leg, into another region, leaving.
    h.world.MoveTo(a, At(100, 100, 500, 900));
    h.world.MoveTo(b, At(100, 100, 900, 520));
    h.Run(1.7);
    h.world.MoveTo(b, At(100, 100, 300, 300)); // mid-leg
    h.world.MoveTo(c, At(101, 100, 100, 700)); // over the region's edge
    h.Run(4.0);
    h.world.Leave(c);
    h.world.MoveTo(a, At(100, 100, 450, 450));
    h.Run(12.0);
    REQUIRE(recording.messages.size() > 6);
    CHECK(!h.world.Moving(a));
    CHECK(!h.world.Moving(b));

    // Saved and loaded unchanged.
    const auto file = std::filesystem::temp_directory_path() / "sr_world_recording_test.srrec";
    std::string error;
    REQUIRE_MESSAGE(recording.Save(file, error), error);
    game::WorldRecording loaded;
    REQUIRE_MESSAGE(loaded.Load(file, error), error);
    std::filesystem::remove(file);
    REQUIRE(loaded.messages.size() == recording.messages.size());
    for (std::size_t i = 0; i < loaded.messages.size(); ++i) {
        CHECK(loaded.messages[i].time == recording.messages[i].time);
        CHECK(loaded.messages[i].id == recording.messages[i].id);
        CHECK(loaded.messages[i].payload == recording.messages[i].payload);
    }

    // The same replay twice: the same hash; at every rate the walks end in the same picture, the
    // server's (once everyone has arrived).
    std::uint64_t first = 0;
    for (const double rate : {10.0, 20.0, 30.0, 60.0, 144.0}) {
        CAPTURE(rate);
        game::WorldView view, again;
        const std::uint64_t hash = game::ReplayHash(loaded, rate, 10.0, view);
        CHECK(game::ReplayHash(loaded, rate, 10.0, again) == hash);
        if (first == 0) first = hash;
        CHECK(hash == first);
        CHECK(view.Self() == a);
        REQUIRE(view.Find(a) != nullptr);
        REQUIRE(view.Find(b) != nullptr);
        CHECK(view.Find(c) == nullptr); // it left
        CHECK(Distance(view.Find(a)->position, h.world.Where(a)->position) < 0.01);
        CHECK(Distance(view.Find(b)->position, h.world.Where(b)->position) < 0.01);
    }
    // Driven in uneven frames, the replay splits the messages the same way.
    game::WorldView framed;
    game::WorldReplay replay(loaded);
    game::SimulationClock clock(30.0);
    double stepTime = 0.0;
    for (int frame = 0; !replay.Finished() || stepTime < loaded.Duration() + 10.0; ++frame) {
        const int steps = clock.Advance(frame % 3 == 0 ? 0.05 : 0.007);
        for (int i = 0; i < steps; ++i) {
            for (const game::TitleEvent& event : replay.TakeUntil(stepTime)) framed.Apply(event);
            framed.Step(clock.StepSeconds());
            stepTime += clock.StepSeconds();
        }
    }
    game::WorldView stepped;
    CHECK(framed.Hash() == game::ReplayHash(loaded, 30.0, 10.0, stepped));
    CHECK(replay.Malformed() == 0);
    CHECK(Distance(framed.Find(a)->position, h.world.Where(a)->position) < 0.01);
}

TEST_CASE("WorldView: a swing heard while walking turns the attacker to its target once it stands") {
    game::WorldView view;
    proto::WorldEntered entered;
    entered.entity = 1;
    entered.position = At(100, 100, 500, 500);
    view.Apply(game::TitleEvent{entered});
    proto::EntityAppeared monster;
    monster.entity = 2;
    monster.position = At(100, 100, 530, 520); // east and a little north
    view.Apply(game::TitleEvent{monster});
    // Walking north; the server stops it and swings, the swing first.
    view.Apply(game::TitleEvent{proto::EntityMoved{1, At(100, 100, 500, 500), At(100, 100, 500, 600), 50.0f}});
    view.Step(0.1);
    const float walking = view.Find(1)->yaw;
    CHECK(walking == doctest::Approx(0.0f));
    view.Apply(game::TitleEvent{proto::EntitySwing{1, 2, 0}});
    CHECK(view.Find(1)->yaw == doctest::Approx(walking)); // still walking: not yet
    const proto::WorldPosition here = view.Find(1)->position;
    view.Apply(game::TitleEvent{proto::EntityMoved{1, here, here, 0.0f}});
    const float toward = std::atan2(530.0f - here.x, 520.0f - here.z);
    CHECK(view.Find(1)->yaw == doctest::Approx(toward));
    // Standing, a swing turns it at once.
    proto::EntityAppeared other = monster;
    other.entity = 3;
    other.position = At(100, 100, here.x, here.z - 40.0f); // behind it
    view.Apply(game::TitleEvent{other});
    view.Apply(game::TitleEvent{proto::EntitySwing{1, 3, 0}});
    CHECK(std::abs(std::abs(view.Find(1)->yaw) - 3.14159265f) < 1e-3f);
}

TEST_CASE("WorldView: a walker re-aimed on its way goes on from where it is drawn; far off, the server's word (the user's report 2026-11-14)") {
    game::WorldView view;
    proto::WorldEntered entered;
    entered.entity = 1;
    entered.position = At(100, 100, 500, 500);
    view.Apply(game::TitleEvent{entered});
    proto::EntityAppeared monster;
    monster.entity = 2;
    monster.position = At(100, 100, 500, 700);
    view.Apply(game::TitleEvent{monster});
    // Chasing north at 50; half a second on it is 25 along.
    view.Apply(game::TitleEvent{proto::EntityMoved{2, At(100, 100, 500, 700), At(100, 100, 500, 900), 50.0f}});
    view.Step(0.5);
    const float drawn = view.Find(2)->position.z;
    CHECK(drawn == doctest::Approx(725.0f));
    // Re-aimed a little to the east, the leg starting 10 behind (sent before the client got there): no step back.
    view.Apply(game::TitleEvent{proto::EntityMoved{2, At(100, 100, 500, 715), At(100, 100, 540, 900), 50.0f}});
    CHECK(view.Find(2)->position.z == doctest::Approx(drawn));
    CHECK(view.Find(2)->moving);
    // Far from where it is drawn (beyond half a second of its walk): put where the server says.
    view.Apply(game::TitleEvent{proto::EntityMoved{2, At(100, 100, 500, 600), At(100, 100, 500, 900), 50.0f}});
    CHECK(view.Find(2)->position.z == doctest::Approx(600.0f));
}

TEST_CASE("WorldView: a look version is never given twice, across worlds and an id's new owner (the user's report 2026-11-13)") {
    // West: the player is 5, a monster 9 in sight.
    game::WorldView view;
    proto::WorldEntered west;
    west.entity = 5;
    west.position = At(100, 100, 500, 500);
    view.Apply(game::TitleEvent{west});
    proto::EntityAppeared monster;
    monster.entity = 9;
    monster.kind = proto::EntityKind::Monster;
    monster.character.model = 1954;
    monster.position = At(100, 100, 520, 500);
    view.Apply(game::TitleEvent{monster});
    const std::uint32_t monsterLook = view.Find(9)->lookVersion;
    const std::uint32_t westSelf = view.Find(5)->lookVersion;
    CHECK(monsterLook != westSelf);
    // A return scroll to East: another server, where the player is 9 (the monster's id in the old picture).
    proto::WorldEntered east;
    east.entity = 9;
    east.character.model = 14875;
    east.position = At(168, 97, 500, 500);
    view.Apply(game::TitleEvent{east});
    REQUIRE(view.Find(9) != nullptr);
    CHECK(view.Find(9)->look.model == 14875);
    CHECK(view.Find(9)->lookVersion != monsterLook); // a cache by id and version dresses it afresh
    CHECK(view.Find(9)->lookVersion != westSelf);
    // An id gone and seen again (another entity, or the same come back) is dressed afresh too.
    monster.entity = 12;
    view.Apply(game::TitleEvent{monster});
    const std::uint32_t first = view.Find(12)->lookVersion;
    view.Apply(game::TitleEvent{proto::EntityGone{12}});
    monster.character.model = 2027;
    monster.kind = proto::EntityKind::Character;
    view.Apply(game::TitleEvent{monster});
    CHECK(view.Find(12)->lookVersion != first);
}

TEST_CASE("World: each game server's entity ids from its own range; a range used up starts again past those in use") {
    // Two servers of a shard: their ids never meet (an id names one entity across the shard).
    server::WorldConfig east = Harness::Config(20.0), west = east;
    east.server = 1;
    west.server = 2;
    Harness a(east), b(west);
    const std::uint32_t first = a.Enter(1, At(100, 100, 500, 500), "Alpha");
    CHECK(first == 1u << 28);
    CHECK(b.Enter(1, At(100, 100, 500, 500), "Alpha") == 2u << 28);
    // A range of four: the counter comes round to its bottom and takes the first id not in use.
    server::WorldConfig small = east;
    small.idRange = 4;
    Harness h(small);
    const std::uint32_t watcher = h.Enter(1, At(100, 100, 500, 500), "Watcher");
    const std::uint32_t npc = h.world.AddNpc(2027, At(100, 100, 520, 500), 0.0f);
    const std::uint32_t other = h.world.AddNpc(2020, At(100, 100, 540, 500), 0.0f);
    CHECK(watcher == (1u << 28));
    CHECK(npc == watcher + 1);
    CHECK(other == watcher + 2);
    h.Run(0.2);
    REQUIRE(h.Of<proto::EntityAppeared>(1).size() == 2);
    h.world.Leave(npc);
    h.Run(0.2);
    REQUIRE(h.Of<proto::EntityGone>(1).size() == 1);
    CHECK(h.world.AddNpc(2021, At(100, 100, 560, 500), 0.0f) == watcher + 3);
    // Past the end: the bottom (the watcher's, in use), then the freed id; the watcher sees a newcomer there.
    const std::uint32_t again = h.world.AddNpc(2022, At(100, 100, 580, 500), 0.0f);
    CHECK(again == npc);
    h.Run(0.2);
    const auto appeared = h.Of<proto::EntityAppeared>(1);
    REQUIRE(appeared.size() == 4);
    const auto seen = std::find_if(appeared.begin() + 2, appeared.end(), [&](const proto::EntityAppeared& m) { return m.entity == again; });
    REQUIRE(seen != appeared.end());
    CHECK(seen->character.model == 2022);
}

TEST_CASE("World: a teleport takes a player out of view and in elsewhere") {
    Harness h(20.0);
    const std::uint32_t a = h.Enter(1, At(100, 100, 500, 500), "Alpha");
    h.Enter(2, At(100, 100, 600, 500), "Beta");     // beside Alpha's start
    const std::uint32_t c = h.Enter(3, At(140, 100, 500, 500), "Gamma"); // far away
    h.Run(0.3);
    REQUIRE(h.Of<proto::EntityAppeared>(2).size() == 1);
    CHECK(h.Of<proto::EntityAppeared>(3).empty());
    h.world.MoveTo(a, At(100, 100, 500, 900));
    h.Run(0.2);
    REQUIRE(h.world.Teleport(a, At(140, 100, 520, 500)));
    CHECK(!h.world.Where(a)); // between the places: nowhere
    h.Run(0.3);
    // Beta sees Alpha gone; Alpha is told of itself again, standing at the destination, then of
    // Gamma; Gamma sees Alpha arrive.
    REQUIRE(h.Of<proto::EntityGone>(2).size() == 1);
    CHECK(h.Of<proto::EntityGone>(2)[0].entity == a);
    const auto entered = h.Of<proto::WorldEntered>(1);
    REQUIRE(entered.size() == 2);
    CHECK(entered[1].entity == a);
    CHECK(Distance(entered[1].position, At(140, 100, 520, 500)) < 0.01);
    CHECK(!h.world.Moving(a)); // the walk it was on ended with the teleport
    const auto appeared = h.Of<proto::EntityAppeared>(1);
    REQUIRE(appeared.size() == 2); // Beta at the start, Gamma after
    CHECK(appeared[1].entity == c);
    REQUIRE(h.Of<proto::EntityAppeared>(3).size() == 1);
    CHECK(h.Of<proto::EntityAppeared>(3)[0].entity == a);
    CHECK(!h.world.Teleport(999, At(1, 1, 0, 0)));
}

TEST_CASE("berserk: a killing hit's chance and points, the hwan levels' modes") {
    using game::BerserkPointGain;
    CHECK(BerserkPointGain(20, 20, 0).chance == 800);   // near its level: 8 %
    CHECK(BerserkPointGain(20, 30, 0).chance == 800);   // above the player
    CHECK(BerserkPointGain(20, 17, 0).chance == 800);   // 3 below: still
    CHECK(BerserkPointGain(30, 20, 0).chance == 675);   // 10 below: 8 - 0.25 x 5
    CHECK(BerserkPointGain(80, 20, 0).chance == 25);    // far below: at least 0.25 %
    CHECK(BerserkPointGain(20, 20, 4).chance == 4000);  // a giant five times
    CHECK(BerserkPointGain(20, 20, 6).chance == 1600);  // an elite twice
    CHECK(BerserkPointGain(20, 20, 1).chance == 800);   // a champion as a general
    CHECK(BerserkPointGain(20, 20, 3).points == 5);     // a unique: all five, always
    CHECK(BerserkPointGain(20, 20, 3).chance >= 10000);
    CHECK(BerserkPointGain(20, 20, 0).points == 1);
    CHECK(BerserkPointGain(20, 20, 0, 50).chance == 400); // the stat 0xB8's percent
    const std::vector<game::HwanLevel> levels = {{0, 200, 0}, {1, 200, 10}, {6, 200, 35}};
    CHECK(game::HwanLevelOf(levels, 0).damageCut == 0);
    CHECK(game::HwanLevelOf(levels, 3).damageCut == 10);
    CHECK(game::HwanLevelOf(levels, 9).damageCut == 35);
    CHECK(game::HwanLevelOf({}, 4).speedPercent == 200);
    // The client's gauge.
    game::PlayerState s;
    s.Apply(game::TitleEvent{proto::BerserkPoints{3}});
    CHECK(s.self.berserk == 3);
    s.Apply(game::TitleEvent{proto::BerserkPoints{9}});
    CHECK(s.self.berserk == game::CharacterStatus::kBerserkBars);
    // Told just before the entry: kept through it.
    s.Apply(game::TitleEvent{proto::BerserkPoints{4}});
    proto::WorldEntered entered;
    entered.entity = 7;
    s.Apply(game::TitleEvent{entered});
    CHECK(s.self.berserk == 4);
}

TEST_CASE("Game server: berserk: a full gauge bought, the mode told to those who see it and to newcomers, over in its time; refused when not full") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    core::JobSystem jobs(2);
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.rules = WorldRules();
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.berserkMs = 1500;
    REQUIRE_MESSAGE(game.Start(config, error), error);
    net::NetService net(1);
    const auto pump = [&] { game.Poll(2ms); };
    const auto player = [&](const std::string& account, const std::string& name) {
        REQUIRE(server::CreateAccount(t.Db(), account, "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        const std::int64_t id = server::FindAccount(t.Db(), account, ok)->id;
        auto c = std::make_unique<Client>(net, game.Port(), pump);
        REQUIRE(c->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
        c->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(c->Expect(entered));
        proto::CharacterCreate m;
        m.name = name;
        m.model = 100;
        m.chest = 1;
        m.legs = 2;
        m.shoes = 3;
        m.weapon = 10;
        c->Send(m);
        proto::LobbyResult r;
        REQUIRE(c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        return c;
    };
    const auto enter = [&](Client& c, const std::string& name, proto::WorldEntered& entered, proto::BerserkPoints& points) {
        proto::LobbyResult r;
        c.Send(proto::CharacterSelect{name});
        REQUIRE(c.Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        c.Send(proto::WorldEnter{});
        REQUIRE(c.Expect(points)); // told with the quick slots, before the world's answer
        REQUIRE(c.Expect(entered));
    };
    const auto berserkOf = [&](Client& c, std::uint32_t entity, proto::EntityBerserk& out, std::chrono::milliseconds wait) {
        for (int i = 0; i < 8; ++i) {
            if (!c.Expect(out, wait)) return false;
            if (out.entity == entity) return true;
        }
        return false;
    };
    auto a = player("alpha", "Alpha");
    auto b = player("beta", "Beta");
    // Alpha's gauge is full (kept in _char.remainhwancount), its hwan level 5 (the blue mode's); Beta's empty.
    REQUIRE(t.Db().Query("UPDATE sro_vt_shard._char SET remainhwancount = 5, hwanlevel = 5 WHERE lower(charname16) = 'alpha'").Ok());
    proto::WorldEntered ea, eb;
    proto::BerserkPoints pa, pb;
    enter(*a, "Alpha", ea, pa);
    enter(*b, "Beta", eb, pb);
    CHECK(pa.points == 5);
    CHECK(pb.points == 0);
    // Beta's gauge is not full: refused (the original's 1).
    b->Send(proto::BerserkRequest{});
    proto::BerserkAnswer refused;
    REQUIRE(b->Expect(refused));
    CHECK(refused.status == proto::BerserkStatus::NotFull);
    // Alpha goes berserk: its five points go; it and Beta are told.
    a->Send(proto::BerserkRequest{});
    proto::BerserkAnswer answer;
    REQUIRE(a->Expect(answer));
    CHECK(answer.status == proto::BerserkStatus::Ok);
    proto::BerserkPoints spent;
    REQUIRE(a->Expect(spent));
    CHECK(spent.points == 0);
    proto::EntityBerserk mine, seen;
    REQUIRE(berserkOf(*a, ea.entity, mine, 2s));
    CHECK(mine.on == 1);
    CHECK(mine.ms == 1500u);
    CHECK(mine.variant == 1); // hwan level 5: the blue second mode (game::BerserkVariant)
    REQUIRE(berserkOf(*b, ea.entity, seen, 2s));
    CHECK(seen.on == 1);
    CHECK(seen.variant == 1);
    // Asked again in it: refused (3).
    a->Send(proto::BerserkRequest{});
    REQUIRE(a->Expect(answer));
    CHECK(answer.status == proto::BerserkStatus::Busy);
    // It runs twice as fast (the hwan level's SPDU 200: its run speed at 200 % of itself).
    proto::WorldPosition to = ea.position;
    to.x += 40.0f;
    a->Send(proto::MoveTo{to});
    proto::EntityMoved leg;
    bool walked = false;
    for (int i = 0; i < 8 && !walked; ++i) {
        if (!a->Expect(leg, 2s)) break;
        walked = leg.entity == ea.entity && leg.speed > 0.0f;
    }
    REQUIRE(walked);
    CHECK(leg.speed == doctest::Approx(2.0f * ea.runSpeed));
    // The gauge was saved empty.
    for (int i = 0; i < 50 && t.Db().Query("SELECT remainhwancount FROM sro_vt_shard._char WHERE lower(charname16) = 'alpha'").Int(0, 0) != 0; ++i) {
        pump();
    }
    CHECK(t.Db().Query("SELECT remainhwancount FROM sro_vt_shard._char WHERE lower(charname16) = 'alpha'").Int(0, 0) == 0);
    // Over in its time: both told.
    proto::EntityBerserk over;
    REQUIRE(berserkOf(*b, ea.entity, over, 3s));
    CHECK(over.on == 0);
}

TEST_CASE("Game server: a gate takes a player to the point it links to") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    core::JobSystem jobs(2);
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.rules = WorldRules();
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    // A gate where a Chinese character starts (Jangan, 168,98, as the server places one), one
    // far from it, and their points: the first leads to a place in region 170,98, the second
    // nowhere new.
    // The tables' rows: cells joined by tabs.
    const auto row = [](std::initializer_list<std::string> cells) {
        std::string out;
        for (const std::string& cell : cells) out += (out.empty() ? "" : "\t") + cell;
        return out + "\n";
    };
    const auto gateRow = [&](int id, int rx, int rz, float x, float z) {
        std::string out = "1\t" + std::to_string(id) + "\tSTORE_TEST_GATE";
        for (int col = 3; col < 58; ++col) {
            out += "\t";
            if (col == 41) out += std::to_string(assets::RegionId(rx, rz));
            else if (col == 43) out += std::to_string(int(x));
            else if (col == 45) out += std::to_string(int(z));
            else out += "0";
        }
        return out + "\n";
    };
    const auto linkRow = [&](int from, int to) {
        std::string out = "1\t" + std::to_string(from) + "\t" + std::to_string(to) + "\t5000";
        for (int col = 4; col < 21; ++col) out += "\t0";
        return out + "\n";
    };
    auto teleports = std::make_shared<assets::TeleportTable>();
    net::NetService net(1); // the gates are made once the start place is known, below
    const auto pump = [&] { game.Poll(2ms); };
    config.teleports = teleports;
    REQUIRE_MESSAGE(game.Start(config, error), error);
    REQUIRE(server::CreateAccount(t.Db(), "gamma", "secret1", error) == server::CreateAccountResult::Created);
    bool ok = false;
    const std::int64_t id = server::FindAccount(t.Db(), "gamma", ok)->id;
    Client c(net, game.Port(), pump);
    REQUIRE(c.Greet());
    proto::LobbyEnter e;
    e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
    c.Send(e);
    proto::LobbyEntered lobby;
    REQUIRE(c.Expect(lobby));
    proto::CharacterCreate m;
    m.name = "Gamma";
    m.model = 100;
    m.chest = 1;
    m.legs = 2;
    m.shoes = 3;
    m.weapon = 10;
    c.Send(m);
    proto::LobbyResult r;
    REQUIRE(c.Expect(r));
    REQUIRE(r.status == proto::LobbyStatus::Ok);
    c.Send(proto::CharacterSelect{"Gamma"});
    REQUIRE(c.Expect(r));
    c.Send(proto::WorldEnter{});
    proto::WorldEntered start;
    REQUIRE(c.Expect(start));
    REQUIRE(start.status == proto::WorldStatus::Ok);
    const int rx = start.position.region & 0xFF, rz = start.position.region >> 8;
    teleports->Decode(gateRow(9001, rx, rz, start.position.x + 20.0f, start.position.z) +
                          gateRow(9002, rx, rz, start.position.x + 500.0f, start.position.z),
                      row({"1", "1", "GATE_A", "9001", "SN_A", "0", "0", "0", "0", "0", "0", "0", "1"}) +
                          row({"1", "2", "GATE_B", "9002", "SN_B", std::to_string(assets::RegionId(170, 98)), "1000", "0", "1000", "0", "0", "0", "1"}) +
                          row({"1", "3", "GATE_C", "0", "SN_C", "0", "0", "0", "0", "0", "0", "0", "1"}),
                      linkRow(1, 2) + linkRow(2, 1) + linkRow(1, 3));
    proto::GateRefused refused;
    c.Send(proto::UseGate{4242, 2}); // no such gate
    REQUIRE(c.Expect(refused));
    CHECK(refused.status == proto::GateStatus::NoSuchGate);
    c.Send(proto::UseGate{9002, 1}); // a gate 500 away
    REQUIRE(c.Expect(refused));
    CHECK(refused.status == proto::GateStatus::TooFar);
    c.Send(proto::UseGate{9001, 7}); // not linked
    REQUIRE(c.Expect(refused));
    CHECK(refused.status == proto::GateStatus::NoSuchLink);
    c.Send(proto::UseGate{9001, 3}); // linked, but leads nowhere
    REQUIRE(c.Expect(refused));
    CHECK(refused.status == proto::GateStatus::Nowhere);
    c.Send(proto::UseGate{9001, 2}); // 5,000 gold; it has none
    REQUIRE(c.Expect(refused));
    CHECK(refused.status == proto::GateStatus::NotEnoughGold);
    // The gate's menu (C7i): it leads somewhere; its point (column 10 0) is no return point.
    proto::GateMenu menu;
    c.Send(proto::GateTalk{9001});
    REQUIRE(c.Expect(menu));
    CHECK(menu.gate == 9001);
    CHECK(menu.services == proto::kGateTeleport);
    c.Send(proto::GateTalk{9002});
    REQUIRE(c.Expect(refused));
    CHECK(refused.status == proto::GateStatus::TooFar);
    c.Send(proto::DesignateReturn{9001});
    REQUIRE(c.Expect(refused));
    CHECK(refused.status == proto::GateStatus::NoRecall);
    // With gold (the server keeps the gold of a character in the world: set while it is out).
    proto::InventoryList inventory;
    REQUIRE(c.Expect(inventory)); // the first entry's
    CHECK(inventory.gold == 0);
    c.Send(proto::WorldLeave{});
    proto::WorldLeft left;
    REQUIRE(c.Expect(left));
    REQUIRE(t.Db().Query("UPDATE sro_vt_shard._char SET remaingold = 7000 WHERE lower(charname16) = 'gamma'").Ok());
    c.Send(proto::CharacterSelect{"Gamma"});
    REQUIRE(c.Expect(r));
    c.Send(proto::WorldEnter{});
    REQUIRE(c.Expect(start));
    REQUIRE(c.Expect(inventory));
    CHECK(inventory.gold == 7000);
    c.Send(proto::UseGate{9001, 2});
    proto::GoldChanged gold;
    REQUIRE(c.Expect(gold));
    CHECK(gold.gold == 2000);
    proto::WorldEntered there;
    REQUIRE(c.Expect(there));
    CHECK(there.status == proto::WorldStatus::Ok);
    CHECK(there.entity == start.entity);
    CHECK(there.position.region == assets::RegionId(170, 98));
    CHECK(there.position.x == doctest::Approx(1000.0f));
    CHECK(there.position.z == doctest::Approx(1000.0f));
    game.Stop();
}

TEST_CASE("World: walkers stay on the floor they walk on, over stairs (game files)") {
    const auto dir = GameDir();
    if (dir.empty()) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    // The client's navigation, for its picture (game::WorldView).
    core::JobSystem jobs(2);
    game::NavLoader loader(fs, jobs);
    game::NavWorld nav;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) loader.Request(assets::RegionId(168 + dx, 98 + dz));
    }
    for (int i = 0; i < 400 && nav.Size() < 9; ++i) {
        loader.Update(nav);
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(nav.Size() == 9);
    // Jangan's palace front stairs (cj_pal_center_stair at 988, 279 of 168,98): down the middle,
    // and from the upper landing aslant towards the teleport gate (as a player walked when the
    // picture sank into them).
    struct Walk {
        proto::WorldPosition from, to;
    };
    for (const Walk& w : {Walk{At(168, 98, 988, 360, 20), At(168, 98, 988, 180)},                           Walk{At(168, 98, 1000, 310, 20), At(168, 98, 1060, 130)}, Walk{At(168, 98, 1020, 320, 20), At(168, 98, 1110, 140)},
                          Walk{At(168, 98, 1040, 300, 20), At(168, 98, 1080, 150)}, Walk{At(168, 98, 970, 320, 20), At(168, 98, 1040, 160)}, Walk{At(168, 98, 960, 300, 20), At(168, 98, 900, 160)}}) {
        CAPTURE(w.from.x);
        CAPTURE(w.to.x);
        Harness h(20.0, &fs);
        server::WorldPlayer p;
        p.peer = 1;
        p.position = w.from;
        p.dropOnGround = true;
        const std::uint32_t walker = h.world.Enter(p);
        for (int i = 0; i < 200 && h.Of<proto::WorldEntered>(1).empty(); ++i) {
            h.Run(0.05);
            std::this_thread::sleep_for(10ms); // the navigation loads on the jobs
        }
        REQUIRE(h.Of<proto::WorldEntered>(1).size() == 1);
        const proto::WorldPosition start = h.Of<proto::WorldEntered>(1)[0].position;
        h.world.MoveTo(walker, w.to);
        if (!h.world.Moving(walker)) {
            MESSAGE("refused: from " << start.x << "," << start.z << " y " << start.y);
            continue;
        }
        // Each tick (2.5 units at the run speed) the height changes no more than the steepest
        // sides of the stairs make it (a slope of about 2): no drop through a floor to the ground
        // under it (which the client's picture did, 10 to 20 units in a step, before C6e).
        float last = start.y, biggest = 0.0f;
        std::string profile;
        for (int i = 0; i < 200 && h.world.Moving(walker); ++i) {
            h.Run(0.05);
            const auto place = h.world.Where(walker);
            REQUIRE(place);
            biggest = std::max(biggest, std::abs(place->position.y - last));
            last = place->position.y;
            if (i % 4 == 0) profile += std::format(" ({:.0f},{:.0f}):{:.1f}", place->position.x, place->position.z, place->position.y);
        }
        MESSAGE("server" << profile << "; biggest step " << biggest);
        CHECK(biggest < 2.5f * 2.5f);

        game::WorldView view;
        view.SetNavigation(&nav);
        proto::WorldEntered entered;
        entered.entity = walker;
        entered.position = start;
        view.Apply(game::TitleEvent{entered});
        proto::EntityMoved leg;
        leg.entity = walker;
        leg.from = start;
        leg.to = h.world.Where(walker)->position;
        leg.speed = 50.0f;
        view.Apply(game::TitleEvent{leg});
        float drawnLast = start.y, drawnBiggest = 0.0f;
        std::string drawn;
        for (int i = 0; i < 600 && view.Find(walker)->moving; ++i) {
            view.Step(1.0 / 30.0);
            const proto::WorldPosition& at = view.Find(walker)->position;
            drawnBiggest = std::max(drawnBiggest, std::abs(at.y - drawnLast));
            drawnLast = at.y;
            if (i % 6 == 0) drawn += std::format(" ({:.0f},{:.0f}):{:.1f}", at.x, at.z, at.y);
        }
        MESSAGE("client" << drawn << "; biggest step " << drawnBiggest);
        CHECK(drawnBiggest < 2.5f * 50.0f / 30.0f);
        CHECK(Distance(view.Find(walker)->position, leg.to) < 0.01);
    }
}

TEST_CASE("World: an NPC standing where it appeared is put down on the floor (Jangan's Adventurer Flora; game files)") {
    const auto dir = GameDir();
    if (dir.empty()) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    core::JobSystem jobs(2);
    game::NavLoader loader(fs, jobs);
    game::NavWorld nav;
    loader.Request(assets::RegionId(168, 97));
    for (int i = 0; i < 600 && nav.Size() < 1; ++i) {
        loader.Update(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(nav.Size() == 1);
    // NPC_CH_EUROPE's spawn (Tab_RefNest): 11 above the market's paving, by its steps; it floated there.
    const proto::WorldPosition spawn{25000, 1671.0f, 11.0f, 261.0f};
    const game::NavPosition floor = nav.Drop(spawn.region, spawn.x, spawn.y, spawn.z);
    REQUIRE(floor.Valid());
    MESSAGE("Flora's floor: " << floor.y << (floor.instance >= 0 ? " (an object's)" : " (the terrain)"));
    game::WorldView view;
    view.SetNavigation(&nav);
    proto::EntityAppeared appeared;
    appeared.entity = 77;
    appeared.position = spawn;
    view.Apply(game::TitleEvent{appeared});
    CHECK(view.Find(77)->position.y == doctest::Approx(11.0f)); // as told, until a step
    view.Step(1.0 / 30.0);
    CHECK(view.Find(77)->position.y == doctest::Approx(floor.y));
    CHECK(view.Find(77)->position.y < 10.0f);
    CHECK(view.Find(77)->nav.Valid());
}

TEST_CASE("World: walks onto stairs end on them, on the server and in the client's picture (game files)") {
    const auto dir = GameDir();
    if (dir.empty()) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    // The client's navigation (its own, as the client loads it).
    core::JobSystem jobs(2);
    game::NavLoader loader(fs, jobs);
    game::NavWorld nav;
    for (int dz = -1; dz <= 2; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) loader.Request(assets::RegionId(168 + dx, 97 + dz));
    }
    for (int i = 0; i < 600 && nav.Size() < 12; ++i) {
        loader.Update(nav);
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(nav.Size() == 12);
    // The server's world with one player, put down at each start as a teleport puts it.
    Harness h(20.0, &fs);
    server::WorldPlayer p;
    p.peer = 1;
    p.position = At(168, 98, 988, 360, 20);
    p.dropOnGround = true;
    const std::uint32_t walker = h.world.Enter(p);
    std::size_t told = 0;
    const auto settle = [&] {
        for (int i = 0; i < 400 && h.Of<proto::WorldEntered>(1).size() <= told; ++i) {
            h.Run(0.05);
            std::this_thread::sleep_for(5ms); // the navigation loads on the jobs
        }
        REQUIRE(h.Of<proto::WorldEntered>(1).size() == told + 1);
        ++told;
    };
    settle();

    // Jangan: the palace's front stairs and the court below them (168,98), the ramp before the
    // inner palace where the gate's travellers arrive (168,97). Clicks straight down onto a
    // stair's surface, from the ground near it.
    std::mt19937 random(11);
    std::uniform_real_distribution<float> spread(-160.0f, 160.0f);
    struct Area {
        int rx, rz;
        float x, z;
    };
    int walks = 0, onStairs = 0, apart = 0;
    for (const Area area : {Area{168, 98, 988, 279}, Area{168, 98, 984, 521}, Area{168, 97, 969, 1500}}) {
        const std::uint16_t region = assets::RegionId(area.rx, area.rz);
        int areaWalks = 0;
        for (int n = 0; n < 400 && areaWalks < 60; ++n) {
            const float tx = area.x + spread(random), tz = area.z + spread(random);
            const float sx = tx + 0.5f * spread(random), sz = tz + 0.5f * spread(random);
            if (std::min({tx, tz, sx, sz}) < 1.0f || std::max({tx, tz, sx, sz}) > 1919.0f) continue;
            const float down[3] = {0, -1, 0}, aboveTarget[3] = {tx, 400.0f, tz}, aboveStart[3] = {sx, 400.0f, sz};
            game::NavHit target, ground;
            if (!nav.Ray(region, aboveTarget, down, 1000.0f, target) || target.instance < 0) continue; // on a stair
            if (!nav.Ray(region, aboveStart, down, 1000.0f, ground) || ground.instance >= 0) continue;  // from the ground
            // Put down there, then the click.
            REQUIRE(h.world.Teleport(walker, {region, sx, ground.y, sz}));
            const std::size_t before = h.told[1].size();
            settle();
            h.world.MoveTo(walker, {target.region, target.x, target.y, target.z});
            if (!h.world.Moving(walker)) continue;
            for (int i = 0; i < 400 && h.world.Moving(walker); ++i) h.Run(0.05);
            const proto::WorldPosition end = h.world.Where(walker)->position;
            ++walks;
            ++areaWalks;
            if (std::hypot(end.x - target.x, end.z - target.z) < 1.0f && std::abs(end.y - target.y) < 2.0f) ++onStairs;
            // The client's picture of the same: what the server told the player, stepped at 30 a second.
            game::WorldView view;
            view.SetNavigation(&nav);
            for (std::size_t m = before; m < h.told[1].size(); ++m) {
                game::TitleEvent event;
                if (game::DecodeWorldEvent(h.told[1][m].id, h.told[1][m].payload, event)) view.Apply(event);
            }
            REQUIRE(view.Find(walker) != nullptr);
            for (int i = 0; i < 3000 && view.Find(walker)->moving; ++i) view.Step(1.0 / 30.0);
            const proto::WorldPosition drawn = view.Find(walker)->position;
            if (std::abs(drawn.y - end.y) > 1.0f) {
                ++apart;
                MESSAGE("apart: from (" << sx << ", " << sz << ") to (" << target.x << ", " << target.z << "): the server at y " << end.y
                                        << ", the client at " << drawn.y);
            }
        }
    }
    MESSAGE(walks << " walks onto stairs, " << onStairs << " ended on the stair clicked, " << apart << " drawn on another floor");
    CHECK(walks >= 60);
    CHECK(onStairs * 10 >= walks * 8); // the rest stop at walls and sides, as clicks walk straight
    CHECK(apart == 0);
}

TEST_CASE("World: players are put down on the floor their height points to (game files)") {
    const auto dir = GameDir();
    if (dir.empty()) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    // The teleport table's arrivals (teleportdata): Hotan's on its plaza (a model 12
    // above the ground), Jangan's on its paving (a model 0.6 above the ground), Donwhang's on the
    // ground; and a point of Jangan's palace courtyard on the stairs' deck, 0.33 under its ground.
    struct Case {
        proto::WorldPosition at;
        float expected;
    };
    std::uint64_t peer = 1;
    for (const Case c : {Case{At(135, 92, 1138, 153, 250), 244.3f}, Case{At(168, 97, 969, 1369, 0), -32.6f},
                         Case{At(153, 102, 957, 1508, -80), -106.8f}, Case{At(168, 98, 999.8f, 321.3f, 20), 19.67f}}) {
        CAPTURE(c.expected);
        Harness h(20.0, &fs);
        server::WorldPlayer p;
        p.peer = peer;
        p.position = c.at;
        p.dropOnGround = true;
        h.world.Enter(p);
        for (int i = 0; i < 400 && h.Of<proto::WorldEntered>(peer).empty(); ++i) {
            h.Run(0.05);
            std::this_thread::sleep_for(5ms); // the navigation loads on the jobs
        }
        REQUIRE(h.Of<proto::WorldEntered>(peer).size() == 1);
        CHECK(h.Of<proto::WorldEntered>(peer)[0].position.y == doctest::Approx(c.expected).epsilon(0.001));
        ++peer;
    }
}

TEST_CASE("Game server: Restart counts down, a move stops it, then the character leaves the world") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    core::JobSystem jobs(2);
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.rules = WorldRules();
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.logoutCountdown = 1s;
    net::NetService net(1);
    const auto pump = [&] { game.Poll(2ms); };
    REQUIRE_MESSAGE(game.Start(config, error), error);
    REQUIRE(server::CreateAccount(t.Db(), "delta", "secret1", error) == server::CreateAccountResult::Created);
    bool ok = false;
    const std::int64_t id = server::FindAccount(t.Db(), "delta", ok)->id;
    Client c(net, game.Port(), pump);
    REQUIRE(c.Greet());
    proto::LobbyEnter e;
    e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
    c.Send(e);
    proto::LobbyEntered lobby;
    REQUIRE(c.Expect(lobby));
    proto::CharacterCreate m;
    m.name = "Delta";
    m.model = 100;
    m.chest = 1;
    m.legs = 2;
    m.shoes = 3;
    m.weapon = 10;
    c.Send(m);
    proto::LobbyResult r;
    REQUIRE(c.Expect(r));
    REQUIRE(r.status == proto::LobbyStatus::Ok);
    c.Send(proto::CharacterSelect{"Delta"});
    REQUIRE(c.Expect(r));
    c.Send(proto::WorldEnter{});
    proto::WorldEntered start;
    REQUIRE(c.Expect(start));
    REQUIRE(start.status == proto::WorldStatus::Ok);
    // Asked: the seconds come back; only the player's cancel stops the countdown (0x7006),
    // not a move (the client sends the cancel on its click).
    c.Send(proto::LogoutRequest{proto::LogoutKind::Restart});
    proto::LogoutAnswer answer;
    REQUIRE(c.Expect(answer));
    CHECK(answer.status == proto::LogoutStatus::Counting);
    CHECK(answer.kind == proto::LogoutKind::Restart);
    CHECK(answer.seconds == 1);
    c.Send(proto::LogoutCancel{});
    REQUIRE(c.Expect(answer));
    CHECK(answer.status == proto::LogoutStatus::Cancelled);
    // Asked again and left alone: out of the world when the time is up, not before.
    const auto asked = std::chrono::steady_clock::now();
    c.Send(proto::LogoutRequest{proto::LogoutKind::Exit});
    REQUIRE(c.Expect(answer));
    CHECK(answer.status == proto::LogoutStatus::Counting);
    CHECK(answer.kind == proto::LogoutKind::Exit);
    proto::WorldLeft left;
    REQUIRE(c.Expect(left));
    CHECK(std::chrono::steady_clock::now() - asked >= 900ms);
    game.Stop();
}

TEST_CASE("Game server: the quick slots are kept for the character and sent when it enters") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    core::JobSystem jobs(2);
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.rules = WorldRules();
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.logoutCountdown = 0s;
    config.databaseThreads = 1; // the saves before the next entry's load, in order
    net::NetService net(1);
    const auto pump = [&] { game.Poll(2ms); };
    REQUIRE_MESSAGE(game.Start(config, error), error);
    REQUIRE(server::CreateAccount(t.Db(), "epsilon", "secret1", error) == server::CreateAccountResult::Created);
    bool ok = false;
    const std::int64_t id = server::FindAccount(t.Db(), "epsilon", ok)->id;
    Client c(net, game.Port(), pump);
    REQUIRE(c.Greet());
    proto::LobbyEnter e;
    e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
    c.Send(e);
    proto::LobbyEntered lobby;
    REQUIRE(c.Expect(lobby));
    proto::CharacterCreate m;
    m.name = "Epsilon";
    m.model = 100;
    m.chest = 1;
    m.legs = 2;
    m.shoes = 3;
    m.weapon = 10;
    c.Send(m);
    proto::LobbyResult r;
    REQUIRE(c.Expect(r));
    REQUIRE(r.status == proto::LobbyStatus::Ok);
    c.Send(proto::CharacterSelect{"Epsilon"});
    REQUIRE(c.Expect(r));
    c.Send(proto::WorldEnter{});
    proto::QuickSlotList slots;
    REQUIRE(c.Expect(slots));
    CHECK(slots.slots.empty()); // a new character's bar is empty
    proto::WorldEntered start;
    REQUIRE(c.Expect(start));
    // Three set, one of them emptied again; a slot past 50 and an unknown kind are refused.
    c.Send(proto::QuickSlotSet{{1, 73, 3001}});
    c.Send(proto::QuickSlotSet{{2, 70, 13}});
    c.Send(proto::QuickSlotSet{{41, 74, 1000}});
    c.Send(proto::QuickSlotSet{{2, 0, 0}});
    c.Send(proto::QuickSlotSet{{51, 73, 1}});
    c.Send(proto::QuickSlotSet{{3, 99, 1}});
    // Out (to the lobby) and in again: the bar comes back.
    c.Send(proto::LogoutRequest{proto::LogoutKind::Restart});
    proto::LogoutAnswer answer;
    REQUIRE(c.Expect(answer));
    proto::WorldLeft left;
    REQUIRE(c.Expect(left));
    c.Send(proto::CharacterSelect{"Epsilon"});
    REQUIRE(c.Expect(r));
    c.Send(proto::WorldEnter{});
    REQUIRE(c.Expect(slots));
    REQUIRE(slots.slots.size() == 2);
    CHECK(slots.slots[0].slot == 1);
    CHECK(slots.slots[0].kind == 73);
    CHECK(slots.slots[0].value == 3001);
    CHECK(slots.slots[1].slot == 41);
    CHECK(slots.slots[1].kind == 74);
    CHECK(slots.slots[1].value == 1000);
    game.Stop();
}

TEST_CASE("Game server: chat to the people around, whispers by name, global lines, refusals and the flood rule") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    core::JobSystem jobs(2);
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.rules = WorldRules();
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.devCommands = true; // global lines without the item (until the inventory)
    net::NetService net(1);
    const auto pump = [&] { game.Poll(2ms); };
    REQUIRE_MESSAGE(game.Start(config, error), error);
    // Two players in the world, side by side in Jangan.
    const auto player = [&](const std::string& account, const std::string& name) {
        REQUIRE(server::CreateAccount(t.Db(), account, "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        const std::int64_t id = server::FindAccount(t.Db(), account, ok)->id;
        auto c = std::make_unique<Client>(net, game.Port(), pump);
        REQUIRE(c->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
        c->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(c->Expect(entered));
        proto::CharacterCreate m;
        m.name = name;
        m.model = 100;
        m.chest = 1;
        m.legs = 2;
        m.shoes = 3;
        m.weapon = 10;
        c->Send(m);
        proto::LobbyResult r;
        REQUIRE(c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        c->Send(proto::CharacterSelect{name});
        REQUIRE(c->Expect(r));
        c->Send(proto::WorldEnter{});
        proto::WorldEntered in;
        REQUIRE(c->Expect(in));
        REQUIRE(in.status == proto::WorldStatus::Ok);
        return std::make_pair(std::move(c), in.entity);
    };
    auto [a, alpha] = player("zeta", "Alpha");
    auto [b, beta] = player("eta", "Beta");
    proto::EntityAppeared seen;
    REQUIRE(a->Expect(seen)); // they see each other
    REQUIRE(b->Expect(seen));
    const auto send = [](Client& c, proto::ChatKind kind, std::uint8_t index, std::string text, std::string target = {}) {
        proto::ChatSend s;
        s.kind = kind;
        s.index = index;
        s.target = std::move(target);
        s.text = std::move(text);
        c.Send(s);
    };
    proto::ChatAnswer answer;
    proto::ChatLine line;

    // To the people around: Beta hears it with the speaker's entity; Alpha gets the answer only.
    send(*a, proto::ChatKind::All, 7, "hello there");
    REQUIRE(a->Expect(answer));
    CHECK(answer.result == proto::ChatResult::Ok);
    CHECK(answer.kind == proto::ChatKind::All);
    CHECK(answer.index == 7);
    REQUIRE(b->Expect(line));
    CHECK(line.kind == proto::ChatKind::All);
    CHECK(line.sender == alpha);
    CHECK(line.name == "Alpha");
    CHECK(line.text == "hello there");

    // A whisper finds its target by name, ignoring case.
    send(*a, proto::ChatKind::Whisper, 8, "psst", "beta");
    REQUIRE(a->Expect(answer));
    CHECK(answer.result == proto::ChatResult::Ok);
    CHECK(answer.index == 8);
    REQUIRE(b->Expect(line));
    CHECK(line.kind == proto::ChatKind::Whisper);
    CHECK(line.sender == 0);
    CHECK(line.name == "Alpha");
    CHECK(line.text == "psst");

    // Refused: nobody of that name, no party, no union, an empty or too long line.
    send(*a, proto::ChatKind::Whisper, 9, "anyone?", "Nobody");
    REQUIRE(a->Expect(answer));
    CHECK(answer.result == proto::ChatResult::TargetNotFound);
    CHECK(answer.index == 9);
    send(*a, proto::ChatKind::Party, 10, "go");
    REQUIRE(a->Expect(answer));
    CHECK(answer.result == proto::ChatResult::NotInParty);
    send(*a, proto::ChatKind::Union, 11, "allies");
    REQUIRE(a->Expect(answer));
    CHECK(answer.result == proto::ChatResult::NotInUnion); // the original's 0x200C: no union
    send(*a, proto::ChatKind::All, 12, "");
    REQUIRE(a->Expect(answer));
    CHECK(answer.result == proto::ChatResult::InvalidCommand);
    send(*a, proto::ChatKind::All, 13, std::string(101, 'x'));
    REQUIRE(a->Expect(answer));
    CHECK(answer.result == proto::ChatResult::InvalidCommand);

    // A global line needs its item (CrossServerTests has one that does).
    send(*b, proto::ChatKind::Global, 1, "to all");
    REQUIRE(b->Expect(answer));
    CHECK(answer.result == proto::ChatResult::InvalidCommand);

    // Four lines to the people around within 4 s: the fourth is refused, and so is any line for
    // the next 10 s.
    for (std::uint8_t i = 20; i < 23; ++i) {
        send(*b, proto::ChatKind::All, i, "spam");
        REQUIRE(b->Expect(answer));
        CHECK(answer.result == proto::ChatResult::Ok);
    }
    send(*b, proto::ChatKind::All, 23, "spam");
    REQUIRE(b->Expect(answer));
    CHECK(answer.result == proto::ChatResult::Restricted);
    CHECK(answer.seconds == 10);
    send(*b, proto::ChatKind::Whisper, 24, "still?", "Alpha");
    REQUIRE(b->Expect(answer));
    CHECK(answer.result == proto::ChatResult::Restricted);
    CHECK(answer.seconds >= 9);
    game.Stop();
}

TEST_CASE("Game server: the inventory on entering, items worn and taken off, seen by others, kept") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    core::JobSystem jobs(2);
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    auto rules = WorldRules();
    // The creation's items as itemdata rows: Chinese chest, legs, shoes (any sex) and a sword.
    auto table = std::make_shared<assets::ItemTable>();
    const auto row = [&](std::uint32_t id, const char* code, int tid3, int tid4, float durability) {
        assets::ItemRow r;
        r.refObjId = id;
        r.codename = code;
        r.typeId = {3, 1, tid3, tid4};
        r.country = 0;
        r.sex = 2;
        r.requirementTypes = {1, -1, -1, -1};
        r.requirementLevels = {1, 0, 0, 0};
        r.stats.assign(55, 0.0f);
        r.stats[0] = durability; // col 63
        table->Add(r);
    };
    row(1, "CHEST", 1, 3, 40);
    row(2, "LEGS", 1, 4, 40);
    row(3, "SHOES", 1, 6, 40);
    row(10, "SWORD", 6, 2, 60);
    {
        // The sword's physical attack (cols 95..98: 100 to 150, at its least and most): what the C page shows with it.
        assets::ItemRow sword = *table->Find(10u);
        sword.stats[95 - 63] = 100;
        sword.stats[96 - 63] = 100;
        sword.stats[97 - 63] = 150;
        sword.stats[98 - 63] = 150;
        table->Add(sword);
    }
    rules->items = table;
    config.rules = rules;
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.logoutCountdown = 0s;
    config.databaseThreads = 1; // saves before the next entry's load, in order
    net::NetService net(1);
    const auto pump = [&] { game.Poll(2ms); };
    REQUIRE_MESSAGE(game.Start(config, error), error);
    const auto player = [&](const std::string& account, const std::string& name) {
        REQUIRE(server::CreateAccount(t.Db(), account, "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        const std::int64_t id = server::FindAccount(t.Db(), account, ok)->id;
        auto c = std::make_unique<Client>(net, game.Port(), pump);
        REQUIRE(c->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
        c->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(c->Expect(entered));
        proto::CharacterCreate m;
        m.name = name;
        m.model = 100;
        m.chest = 1;
        m.legs = 2;
        m.shoes = 3;
        m.weapon = 10;
        c->Send(m);
        proto::LobbyResult r;
        REQUIRE(c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        c->Send(proto::CharacterSelect{name});
        REQUIRE(c->Expect(r));
        return c;
    };
    auto a = player("theta", "Alpha");
    auto b = player("iota", "Beta");
    a->Send(proto::WorldEnter{});
    proto::InventoryList inv;
    REQUIRE(a->Expect(inv));
    proto::WorldEntered ea;
    REQUIRE(a->Expect(ea));
    // The creation's items in the original's places, new: chest 1, legs 4, feet 5, weapon 6.
    CHECK(inv.size == 45);
    CHECK(inv.gold == 0);
    REQUIRE(inv.items.size() == 4);
    CHECK(inv.items[0].slot == 1);
    CHECK(inv.items[0].item == 1);
    CHECK(inv.items[0].durability == 40);
    CHECK(inv.items[1].slot == 4);
    CHECK(inv.items[2].slot == 5);
    CHECK(inv.items[3].slot == 6);
    CHECK(inv.items[3].item == 10);
    CHECK(inv.items[3].durability == 60);
    b->Send(proto::WorldEnter{});
    proto::WorldEntered eb;
    REQUIRE(b->Expect(eb));
    proto::EntityAppeared seen;
    REQUIRE(a->Expect(seen));
    REQUIRE(b->Expect(seen));
    CHECK(seen.character.equipment.size() == 4);

    // The sword off into the bag: the answer, and both see Alpha's weapon place empty; the stat panel's numbers told
    // again, its attack without the sword's (the C page kept the entry's; the user's report 2026-11-13).
    proto::Progress panel;
    std::uint32_t armed = 0;
    while (a->Expect(panel, 300ms)) armed = panel.phyAttackMax;
    a->Send(proto::ItemMove{6, 13, 0});
    proto::ItemMoved moved;
    REQUIRE(a->Expect(moved));
    CHECK(moved.status == proto::ItemStatus::Ok);
    REQUIRE(moved.changed.size() == 1);
    CHECK(moved.changed[0].slot == 13);
    CHECK(moved.changed[0].item == 10);
    REQUIRE(moved.emptied.size() == 1);
    CHECK(moved.emptied[0] == 6);
    proto::EquipmentChanged change;
    REQUIRE(b->Expect(change));
    CHECK(change.entity == ea.entity);
    CHECK(change.slot == 6);
    CHECK(change.item == 0);
    REQUIRE(a->Expect(change)); // the owner too
    REQUIRE(a->Expect(panel));
    MESSAGE("physical attack with the sword " << armed << ", without " << panel.phyAttackMax);
    CHECK(panel.phyAttackMax < armed);
    // The chest is no weapon.
    a->Send(proto::ItemMove{1, 6, 0});
    REQUIRE(a->Expect(moved));
    CHECK(moved.status == proto::ItemStatus::WrongSlot);
    // The sword back on: Beta sees it.
    a->Send(proto::ItemMove{13, 6, 0});
    REQUIRE(a->Expect(moved));
    CHECK(moved.status == proto::ItemStatus::Ok);
    REQUIRE(b->Expect(change));
    CHECK(change.slot == 6);
    CHECK(change.item == 10);
    // Into the bag again, out of the world and back: it is still in the bag.
    a->Send(proto::ItemMove{6, 20, 0});
    REQUIRE(a->Expect(moved));
    CHECK(moved.status == proto::ItemStatus::Ok);
    a->Send(proto::LogoutRequest{proto::LogoutKind::Restart});
    proto::LogoutAnswer answer;
    REQUIRE(a->Expect(answer));
    proto::WorldLeft left;
    REQUIRE(a->Expect(left));
    for (int i = 0; i < 50; ++i) pump();
    a->Send(proto::CharacterSelect{"Alpha"});
    proto::LobbyResult r;
    REQUIRE(a->Expect(r));
    a->Send(proto::WorldEnter{});
    REQUIRE(a->Expect(inv));
    REQUIRE(inv.items.size() == 4);
    CHECK(inv.items[3].slot == 20);
    CHECK(inv.items[3].item == 10);
    CHECK(inv.items[3].durability == 60);
    game.Stop();
}

TEST_CASE("monster drops roll from the generated tables: gold every kill, items now and then") {
    // Needs the game files and monster_drops.tsv (tools/scripts/monster_drops.py) beside the debug server.
    const char* dir = std::getenv("SR_GAME_DIR");
    const std::filesystem::path table = std::filesystem::path(__FILE__).parent_path().parent_path() / "build" / "clang" / "server" / "Debug" / "monster_drops.tsv";
    if (dir == nullptr || !std::filesystem::exists(table)) {
        MESSAGE("SR_GAME_DIR or monster_drops.tsv missing - skipped");
        return;
    }
    sr::assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::MountGameArchives(fs, dir, error), error);
    sr::assets::ItemTable items;
    sr::assets::CharacterTable characters;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> gold;
    const auto file = fs.Read("server_dep\\silkroad\\textdata\\levelgold.txt");
    REQUIRE(file.has_value());
    REQUIRE(sr::assets::DecodeLevelGold(sr::core::DecodeTextFile(file->Bytes()), gold, error));
    sr::server::DropTable drops;
    REQUIRE_MESSAGE(drops.Load(table.string(), items, error), error);
    drops.SetLevelGold(gold);
    const sr::assets::ObjectRow* mangnyang = characters.Find(std::string_view("MOB_CH_MANGNYANG"));
    REQUIRE(mangnyang != nullptr);
    std::mt19937 random(7);
    sr::server::GameRates rates;
    int golds = 0, things = 0, kills = 2000;
    std::uint64_t goldSum = 0;
    std::map<std::string, int> seen;
    for (int k = 0; k < kills; ++k) {
        for (const auto& d : drops.Roll(*mangnyang, 0, 1, rates, random)) {
            if (d.item.refId == 0) {
                ++golds;
                goldSum += d.gold;
                CHECK(d.gold >= gold[1].first);
                CHECK(d.gold <= gold[1].second);
            } else {
                ++things;
                const sr::assets::ItemRow* row = items.Find(d.item.refId);
                REQUIRE(row != nullptr);
                ++seen[row->codename];
            }
        }
    }
    MESSAGE(kills << " mangnyangs: " << golds << " gold heaps (" << goldSum / std::max(1, golds) << " each), " << things << " items");
    for (const auto& [code, n] : seen) MESSAGE("  " << code << " x" << n);
    CHECK(golds == kills); // one heap a pass, a general monster one pass
    CHECK(things > 0);
    // A unique drops a lot.
    const sr::assets::ObjectRow* tiger = characters.Find(std::string_view("MOB_CH_TIGERWOMAN"));
    REQUIRE(tiger != nullptr);
    std::size_t uniqueItems = 0;
    for (int k = 0; k < 200; ++k) uniqueItems += drops.Roll(*tiger, 3, 20, rates, random).size();
    MESSAGE("200 tiger girls: " << uniqueItems << " drops");
    CHECK(uniqueItems > 200 * 5);
}

TEST_CASE("monster drops as the original rolls them: one of ammunition or a scroll a pass, the exact level's row, tries until one gives, a unique's stages") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) {
        MESSAGE("SR_GAME_DIR missing - skipped");
        return;
    }
    sr::assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::MountGameArchives(fs, dir, error), error);
    sr::assets::ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    // Every roll certain at level 50; nothing at 51. The arrow is kept only now and then (its absolute 9 %),
    // so its tries go on until one is kept.
    const std::string records =
        "budget\t0\t8\t1\t20\n"
        "budget\t3\t60\t1\t1\n"
        "category\t0\tammo-scroll\tarrow,bolt|scroll\n"
        "consumable-class\tarrow\t50\t0\t1\n"
        "consumable-item\tarrow\t0\tITEM_ETC_AMMO_ARROW_01\t100\t8\t1\n"
        "consumable-class\tbolt\t50\t0\t1\n"
        "consumable-item\tbolt\t0\tITEM_ETC_AMMO_BOLT_01\t100\t99\t1\n"
        "consumable-class\tscroll\t50\t0\t1\n"
        "consumable-item\tscroll\t0\tITEM_ETC_SCROLL_RETURN_01\t100\t99\t1\n"
        "unique-count\tdefault\trecipe\t2\t2\n"
        "unique-count\tdefault\tscroll\t0\t0\n"
        "unique-count\tMOB_TK_BONELORD\trepairable\t0\t0\n"
        "unique-item\trecipe\tITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B\n"
        "unique-item\tscroll\tITEM_ETC_SCROLL_RETURN_02\n"
        // A profiled unique (its extras in place of the default's) and a body whose profile gives nothing.
        "profile\tMOB_TEST_PROFILED\t3\t1\t11\t2\t1\n"
        "profile\tMOB_TEST_PROFILED\t3\t1\t12\t1\t1\n"
        "profile\tMOB_TEST_PROFILED\t3\t2\t6\t3\t3\n"
        "profile\tMOB_TEST_PROFILED\t3\t2\t14\t4\t4\n"
        "profile\tMOB_TEST_EMPTY\t3\t1\t11\t0\t0\n"
        "profile\tMOB_TEST_EMPTY\t3\t1\t12\t0\t0\n";
    sr::server::DropTable drops;
    REQUIRE_MESSAGE(drops.LoadText(records, items, error), error);
    sr::assets::ObjectRow monster;
    monster.codename = "MOB_TEST";
    monster.level = 50;
    std::mt19937 random(11);
    const sr::server::GameRates rates;
    std::map<std::string, int> seen;
    int kills = 3000, arrows = 0;
    for (int k = 0; k < kills; ++k) {
        const auto d = drops.Roll(monster, 0, 50, rates, random);
        REQUIRE(d.size() <= 1); // one of them a pass (an arrow's 20 tries all lost now and then: 0.91^20)
        if (!d.empty()) ++seen[items.Find(d[0].item.refId)->codename];
    }
    arrows = seen["ITEM_ETC_AMMO_ARROW_01"];
    MESSAGE("arrows " << arrows << ", bolts " << seen["ITEM_ETC_AMMO_BOLT_01"] << ", scrolls " << seen["ITEM_ETC_SCROLL_RETURN_01"]);
    CHECK(seen["ITEM_ETC_SCROLL_RETURN_01"] > kills * 45 / 100); // a scroll half the time, ammunition the other half
    CHECK(seen["ITEM_ETC_SCROLL_RETURN_01"] < kills * 55 / 100);
    CHECK(arrows > kills * 18 / 100); // a quarter of the passes, an arrow kept within 20 tries in 85 % of them (one try: 2 %)
    // The exact level's row: nothing at 51 (no fallback to 50's).
    monster.level = 51;
    for (int k = 0; k < 200; ++k) CHECK(drops.Roll(monster, 0, 51, rates, random).empty());
    // A unique's stages: its own row changes only the stages it names (Bonelord keeps the default's elixirs).
    monster.codename = "MOB_TK_BONELORD";
    int recipes = 0;
    for (const auto& d : drops.Roll(monster, 3, 51, rates, random)) recipes += items.Find(d.item.refId)->codename == "ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B";
    CHECK(recipes == 2);
    // A profile's extras replace all of a unique's: its 3 elixirs (not the default's 2), its 4 special scrolls.
    monster.codename = "MOB_TEST_PROFILED";
    std::map<std::string, int> extras;
    for (const auto& d : drops.Roll(monster, 3, 51, rates, random)) ++extras[items.Find(d.item.refId)->codename];
    CHECK(extras["ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B"] == 3);
    CHECK(extras["ITEM_ETC_SCROLL_RETURN_02"] == 4);
    // The same monster of another kind has no profile: the default's extras.
    extras.clear();
    for (const auto& d : drops.Roll(monster, 8, 51, rates, random)) ++extras[items.Find(d.item.refId)->codename];
    CHECK(extras["ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B"] == 2);
    // A profile of no gold, no passes and no extras: nothing at all (Haroeris' body).
    monster.codename = "MOB_TEST_EMPTY";
    monster.level = 50;
    for (int k = 0; k < 50; ++k) CHECK(drops.Roll(monster, 3, 50, rates, random).empty());
}

TEST_CASE("World: a hidden player leaves the sight of those who do not detect it; detection of its mask and level shows it (K12)") {
    Harness h(20.0);
    const std::uint32_t a = h.Enter(1, At(100, 100, 500, 500), "Alpha");
    const std::uint32_t b = h.Enter(2, At(100, 100, 560, 500), "Beta");
    h.Run(0.2);
    const auto sees = [&](std::uint64_t peer, std::uint32_t entity) {
        // Its last word on the entity: appeared (seen) or gone (not).
        bool seen = false;
        for (const auto& m : h.Of<proto::EntityAppeared>(peer)) seen = seen || m.entity == entity;
        int appeared = 0, gone = 0;
        for (const auto& m : h.Of<proto::EntityAppeared>(peer)) appeared += m.entity == entity;
        for (const auto& m : h.Of<proto::EntityGone>(peer)) gone += m.entity == entity;
        return appeared > gone;
    };
    REQUIRE(sees(1, b));
    // Beta hides (mask 1, level 3): Alpha loses it; Beta still sees Alpha.
    h.world.SetHidden(b, 1, 3);
    h.Run(0.3);
    CHECK_FALSE(sees(1, b));
    CHECK(sees(2, a));
    // A detection of another mask, or of a lower level: still hidden.
    h.world.SetDetection(a, 2, 9);
    h.Run(0.3);
    CHECK_FALSE(sees(1, b));
    h.world.SetDetection(a, 1, 2);
    h.Run(0.3);
    CHECK_FALSE(sees(1, b));
    // Its mask at its level: seen again.
    h.world.SetDetection(a, 1, 3);
    h.Run(0.3);
    CHECK(sees(1, b));
    // Hidden again, no detection: its party member sees it, another party's not.
    h.world.SetDetection(a, 0, 0);
    h.Run(0.3);
    CHECK_FALSE(sees(1, b));
    h.world.SetParty(a, 4);
    h.world.SetParty(b, 4);
    h.Run(0.3);
    CHECK(sees(1, b));
    h.world.SetParty(a, 5);
    h.Run(0.3);
    CHECK_FALSE(sees(1, b));
    // Out of hiding: seen by all.
    h.world.SetDetection(a, 0, 0);
    h.world.SetHidden(b, 0, 0);
    h.Run(0.3);
    CHECK(sees(1, b));
}

TEST_CASE("Game server: avatars put on and taken off, their kinds, the dress's attachment, kept in _") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    core::JobSystem jobs(2);
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    auto rules = WorldRules();
    auto table = std::make_shared<assets::ItemTable>();
    const auto row = [&](std::uint32_t id, std::array<int, 4> type, float durability, int sex = 2, int childItems = 0) {
        assets::ItemRow r;
        r.refObjId = id;
        r.codename = "ITEM_" + std::to_string(id);
        r.typeId = type;
        r.country = 3;
        r.sex = sex;
        r.childItems = childItems;
        r.requirementTypes = {1, -1, -1, -1};
        r.requirementLevels = {1, 0, 0, 0};
        r.stats.assign(55, 0.0f);
        r.stats[0] = durability; // col 63
        table->Add(r);
    };
    // The creation's set, and the avatars: a hat, a dress that takes an attachment, an attachment, a flag for women, a
    // second hat, a Devil's Spirit.
    row(1, {3, 1, 1, 3}, 40);
    row(2, {3, 1, 1, 4}, 40);
    row(3, {3, 1, 1, 6}, 40);
    row(10, {3, 1, 6, 2}, 60);
    constexpr std::uint32_t kHat = 40, kDress = 41, kAttachment = 42, kWomensFlag = 43, kOtherHat = 44, kSpirit = 45;
    row(kHat, {3, 1, 13, 1}, 0);
    row(kDress, {3, 1, 13, 2}, 0, 2, 1);
    row(kAttachment, {3, 1, 13, 3}, 0);
    row(kWomensFlag, {3, 1, 13, 4}, 0, 0);
    row(kOtherHat, {3, 1, 13, 1}, 0);
    row(kSpirit, {3, 1, 14, 1}, 0);
    rules->items = table;
    config.rules = rules;
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.logoutCountdown = 0s;
    config.databaseThreads = 1; // saves before the next entry's load, in order
    net::NetService net(1);
    const auto pump = [&] { game.Poll(2ms); };
    REQUIRE_MESSAGE(game.Start(config, error), error);
    REQUIRE(server::CreateAccount(t.Db(), "dressy", "secret1", error) == server::CreateAccountResult::Created);
    bool ok = false;
    const std::int64_t account = server::FindAccount(t.Db(), "dressy", ok)->id;
    const auto connect = [&] {
        auto c = std::make_unique<Client>(net, game.Port(), pump);
        REQUIRE(c->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), account, config.id, 30, error);
        c->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(c->Expect(entered));
        return c;
    };
    auto a = connect();
    proto::CharacterCreate m;
    m.name = "Dressy";
    m.model = 100;
    m.chest = 1;
    m.legs = 2;
    m.shoes = 3;
    m.weapon = 10;
    a->Send(m);
    proto::LobbyResult r;
    REQUIRE(a->Expect(r));
    REQUIRE(r.status == proto::LobbyStatus::Ok);
    // The avatars into the bag while the account is away (13 .. 18 in this order).
    a->Connection().Close();
    a.reset();
    for (int i = 0; i < 2000 && t.Db().Query("SELECT 1 FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {account}).Rows() > 0; ++i) pump();
    for (std::uint32_t ref : {kHat, kDress, kAttachment, kWomensFlag, kOtherHat, kSpirit}) {
        server::StoredItem item;
        item.refId = ref;
        // The Devil's Spirit awake for a day (its awakening's end in its data word).
        if (ref == kSpirit) item.durability = std::uint32_t(std::time(nullptr) + 86400);
        REQUIRE(server::GiveItems(t.Db(), config.id, "Dressy", {item}, 0, error) == server::GiveResult::Done);
    }
    const auto enter = [&](proto::InventoryList& inv, proto::AvatarList& worn) {
        a = connect();
        a->Send(proto::CharacterSelect{"Dressy"});
        REQUIRE(a->Expect(r));
        a->Send(proto::WorldEnter{});
        REQUIRE(a->Expect(inv));
        REQUIRE(a->Expect(worn));
    };
    proto::InventoryList inv;
    proto::AvatarList worn;
    enter(inv, worn);
    CHECK(worn.items.empty());
    REQUIRE(inv.items.size() == 10);
    const auto move = [&](bool wear, std::uint8_t place) {
        a->Send(proto::AvatarMove{wear, place});
        proto::AvatarMoved moved;
        REQUIRE(a->Expect(moved));
        return moved;
    };

    // The hat on: into the hat's place (0), out of the bag's 13.
    proto::AvatarMoved moved = move(true, 13);
    REQUIRE(moved.status == proto::ItemStatus::Ok);
    CHECK(moved.bagEmptied == std::vector<std::uint8_t>{13});
    REQUIRE(moved.avatars.size() == 1);
    CHECK(moved.avatars[0].slot == game::avatar::kHat);
    CHECK(moved.avatars[0].item == kHat);
    // The attachment needs a dress first; a second hat its place free; the flag for women a woman; the chest is no
    // avatar; an empty place nothing.
    CHECK(move(true, 15).status == proto::ItemStatus::NeedsDress);
    CHECK(move(true, 17).status == proto::ItemStatus::PlaceTaken);
    CHECK(move(true, 16).status == proto::ItemStatus::Sex);
    CHECK(move(true, 1).status == proto::ItemStatus::BadSlot);
    CHECK(move(true, 30).status == proto::ItemStatus::NoItem);
    // The dress, then the attachment over it, then the Devil's Spirit (its own place, 4).
    moved = move(true, 14);
    REQUIRE(moved.status == proto::ItemStatus::Ok);
    CHECK(moved.avatars[0].slot == game::avatar::kDress);
    moved = move(true, 15);
    REQUIRE(moved.status == proto::ItemStatus::Ok);
    CHECK(moved.avatars[0].slot == game::avatar::kAttachment);
    moved = move(true, 18);
    REQUIRE(moved.status == proto::ItemStatus::Ok);
    CHECK(moved.avatars[0].slot == game::avatar::kSpirit);
    // The dress off takes the attachment with it: into the bag's first free places (13, then 14).
    moved = move(false, game::avatar::kDress);
    REQUIRE(moved.status == proto::ItemStatus::Ok);
    REQUIRE(moved.bag.size() == 2);
    CHECK(moved.bag[0].slot == 13);
    CHECK(moved.bag[0].item == kDress);
    CHECK(moved.bag[1].slot == 14);
    CHECK(moved.bag[1].item == kAttachment);
    CHECK(moved.avatarsEmptied == std::vector<std::uint8_t>{game::avatar::kDress, game::avatar::kAttachment});
    CHECK(move(false, game::avatar::kDress).status == proto::ItemStatus::NoItem);
    // The dress back on; out of the world and in again: the hat, the dress and the Devil's Spirit worn, the attachment
    // in the bag.
    REQUIRE(move(true, 13).status == proto::ItemStatus::Ok);
    a->Send(proto::LogoutRequest{proto::LogoutKind::Restart});
    proto::LogoutAnswer answer;
    REQUIRE(a->Expect(answer));
    proto::WorldLeft left;
    REQUIRE(a->Expect(left));
    // The save lands on a database thread: waited for by the clock (a count of pumps says nothing of its time).
    const auto avatarRows = [&] {
        return t.Db().Query("SELECT a.slot, it.refitemid FROM sro_vt_shard._inventoryforavatar a JOIN sro_vt_shard._items it ON it.id64 = a.itemid "
                            "JOIN sro_vt_shard._char c ON c.charid = a.charid WHERE c.charname16 = 'Dressy' AND a.itemid <> 0 ORDER BY a.slot");
    };
    for (const auto until = std::chrono::steady_clock::now() + 5s; std::chrono::steady_clock::now() < until;) {
        pump();
        if (const server::QueryResult r = avatarRows(); r.Ok() && r.Rows() == 3) break;
    }
    server::QueryResult kept = avatarRows();
    REQUIRE(kept.Ok());
    REQUIRE(kept.Rows() == 3);
    CHECK(kept.Int(0, 0) == game::avatar::kHat);
    CHECK(kept.Int(0, 1) == kHat);
    CHECK(kept.Int(1, 0) == game::avatar::kDress);
    CHECK(kept.Int(1, 1) == kDress);
    CHECK(kept.Int(2, 0) == game::avatar::kSpirit);
    CHECK(kept.Int(2, 1) == kSpirit);
    a->Send(proto::CharacterSelect{"Dressy"});
    REQUIRE(a->Expect(r));
    a->Send(proto::WorldEnter{});
    REQUIRE(a->Expect(inv));
    REQUIRE(a->Expect(worn));
    REQUIRE(worn.items.size() == 3);
    CHECK(worn.items[0].slot == game::avatar::kHat);
    CHECK(worn.items[0].item == kHat);
    CHECK(worn.items[1].slot == game::avatar::kDress);
    CHECK(worn.items[1].item == kDress);
    CHECK(worn.items[2].slot == game::avatar::kSpirit);
    const auto attachment = std::find_if(inv.items.begin(), inv.items.end(), [&](const proto::ItemRecord& i) { return i.item == kAttachment; });
    REQUIRE(attachment != inv.items.end());
    CHECK(attachment->slot == 14);
    CHECK(std::none_of(inv.items.begin(), inv.items.end(), [&](const proto::ItemRecord& i) { return i.item == kHat || i.item == kDress; }));
    game.Stop();
}

TEST_CASE("Hotan: the palace's guild hall reaches the town by the navigation (game files; the user's report 2026-11-12)") {
    const auto dir = GameDir();
    if (dir.empty()) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    core::JobSystem jobs(2);
    game::NavLoader loader(fs, jobs);
    game::NavWorld nav;
    std::size_t wanted = 0;
    for (int dz = -1; dz <= 3; ++dz) {
        for (int dx = -1; dx <= 2; ++dx) {
            loader.Request(assets::RegionId(135 + dx, 92 + dz));
            ++wanted;
        }
    }
    for (int i = 0; i < 2000 && nav.Size() < wanted; ++i) {
        loader.Update(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    MESSAGE(nav.Size() << " of " << wanted << " regions held");
    // NPC_KT_GUILD's spawn (Tab_RefNest 14819) and NPC_KT_WAREHOUSE's (the town's square).
    const game::NavPosition guild = nav.Drop(24199, 1145.56f, 267.67f, 585.75f);
    REQUIRE(guild.Valid());
    MESSAGE("the guild hall's floor: region " << guild.region << " y " << guild.y << (guild.instance >= 0 ? " (an object's)" : " (the terrain)"));
    const game::NavPosition square = nav.Drop(23687, 1134.28f, 243.75f, 610.79f);
    REQUIRE(square.Valid());
    MESSAGE("the square's floor: y " << square.y << (square.instance >= 0 ? " (an object's)" : " (the terrain)"));
    std::vector<game::NavPosition> path;
    game::NavPathOptions options;
    options.maxNodes = 400000;
    options.margin = 2500.0f;
    game::NavPathStats stats;
    const bool found = game::FindNavPath(nav, guild, 23687, 1134.28f, 610.79f, path, options, &stats);
    MESSAGE("way found: " << found << ", " << path.size() << " legs, " << stats.expanded << " points, " << stats.walks << " walks");
    REQUIRE(found);
    for (const game::NavPosition& p : path) {
        const std::string on = p.instance >= 0 ? "an object" : "the terrain";
        MESSAGE("  leg to region " << p.region << " (" << p.x << ", " << p.y << ", " << p.z << ") on " << on);
    }
    // The server's world: a player at the guild NPC, each leg's end clicked in turn (a click walks straight).
    Harness h(20.0, &fs);
    server::WorldPlayer p;
    p.peer = 1;
    p.position = {24199, 1145.56f, 267.67f, 585.75f};
    p.dropOnGround = true;
    const std::uint32_t walker = h.world.Enter(p);
    for (int i = 0; i < 600 && h.Of<proto::WorldEntered>(1).empty(); ++i) {
        h.Run(0.05);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_FALSE(h.Of<proto::WorldEntered>(1).empty());
    MESSAGE("entered at y " << h.world.Where(walker)->position.y);
    // The client's picture of it: what the server told the player, stepped at 30 a second on the client's navigation.
    game::WorldView view;
    view.SetNavigation(&nav);
    std::size_t applied = 0;
    const auto picture = [&] {
        for (; applied < h.told[1].size(); ++applied) {
            game::TitleEvent event;
            if (game::DecodeWorldEvent(h.told[1][applied].id, h.told[1][applied].payload, event)) view.Apply(event);
        }
        for (int i = 0; i < 3000 && view.Find(walker) != nullptr && view.Find(walker)->moving; ++i) view.Step(1.0 / 30.0);
    };
    picture();
    for (const game::NavPosition& leg : path) {
        const server::World::MoveRefused why = h.world.MoveTo(walker, {leg.region, leg.x, leg.y, leg.z});
        for (int i = 0; i < 2000 && h.world.Moving(walker); ++i) h.Run(0.05);
        const proto::WorldPosition end = h.world.Where(walker)->position;
        const proto::FlatPosition a = proto::ToFlat(end), b = proto::ToFlat({leg.region, leg.x, leg.y, leg.z});
        picture();
        REQUIRE(view.Find(walker) != nullptr);
        const proto::WorldPosition drawn = view.Find(walker)->position;
        const proto::FlatPosition c = proto::ToFlat(drawn);
        MESSAGE("  walked (refused " << int(why) << ") to region " << end.region << " (" << end.x << ", " << end.y << ", " << end.z << "), "
                                      << std::hypot(a.x - b.x, a.z - b.z) << " short; drawn at (" << drawn.x << ", " << drawn.y << ", " << drawn.z
                                      << "), " << std::hypot(c.x - a.x, c.z - a.z) << " apart");
        CHECK(std::hypot(a.x - b.x, a.z - b.z) < 2.0);
        CHECK(std::hypot(c.x - a.x, c.z - a.z) < 2.0);
        CHECK(std::abs(drawn.y - end.y) < 2.0f);
    }
}

TEST_CASE("Hotan: down the palace's south stairs from X 113, Y 400 (game files; the user's report 2026-11-13)") {
    const auto dir = GameDir();
    if (dir.empty()) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    core::JobSystem jobs(2);
    game::NavLoader loader(fs, jobs);
    game::NavWorld nav;
    std::size_t wanted = 0;
    for (int dz = -1; dz <= 3; ++dz) {
        for (int dx = -1; dx <= 2; ++dx) {
            loader.Request(assets::RegionId(135 + dx, 92 + dz));
            ++wanted;
        }
    }
    for (int i = 0; i < 2000 && nav.Size() < wanted; ++i) {
        loader.Update(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(nav.Size() == wanted);
    // X 113, Y 400 (region 24199, local 1130..1139, 160..169): the stairs' steps at 243.85, the terrain 1.2 under them,
    // closed round (a walker on it went nowhere). Put down at the terrain's height, a walker stands on the steps.
    const game::NavPosition onTerrain = nav.Drop(24199, 1135.0f, 242.659f, 165.0f);
    REQUIRE(onTerrain.Valid());
    CHECK(onTerrain.instance >= 0);
    CHECK(onTerrain.y == doctest::Approx(243.852f).epsilon(0.001));
    // The walks south from the steps, down the stairs, onto the walkway and through its door in the region below (it
    // failed there: the door is in that region's link list).
    const game::NavPosition from = nav.Drop(24199, 1135.0f, 244.0f, 165.0f);
    REQUIRE(from.instance >= 0);
    for (float length = 300.0f; length <= 1000.0f; length += 50.0f) {
        game::NavPosition end;
        const std::uint32_t bits = nav.Walk(from, 24199, 1135.0f, 165.0f - length, end);
        const proto::FlatPosition a = proto::ToFlat({24199, 1135.0f, 0.0f, 165.0f - length}), b = proto::ToFlat({end.region, end.x, end.y, end.z});
        CHECK_MESSAGE((bits & game::nav_walk::kFailed) == 0, "south " << length << ": bits " << bits);
        CHECK_MESSAGE(std::hypot(a.x - b.x, a.z - b.z) < 1.0, "south " << length << ": ended at region " << end.region << " (" << end.x << ", " << end.z << ")");
    }
    // The server's world: put down on the highest floor there, then clicks south (Y minus) of growing lengths.
    Harness h(20.0, &fs);
    server::WorldPlayer p;
    p.peer = 1;
    p.position = {24199, 1135.0f, 400.0f, 165.0f};
    p.dropOnGround = true;
    const std::uint32_t walker = h.world.Enter(p);
    for (int i = 0; i < 600 && h.Of<proto::WorldEntered>(1).empty(); ++i) {
        h.Run(0.05);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_FALSE(h.Of<proto::WorldEntered>(1).empty());
    const proto::WorldPosition start = h.world.Where(walker)->position;
    MESSAGE("entered at (" << start.x << ", " << start.y << ", " << start.z << ")");
    int stuck = 0;
    for (const float length : {50.0f, 150.0f, 300.0f, 600.0f, 1000.0f}) {
        REQUIRE(h.world.Teleport(walker, start));
        for (int i = 0; i < 20; ++i) h.Run(0.05);
        const proto::FlatPosition a = proto::ToFlat(h.world.Where(walker)->position);
        const proto::WorldPosition to = proto::FromFlat({a.x, a.z - length}, start.y);
        const server::World::MoveRefused why = h.world.MoveTo(walker, to);
        for (int i = 0; i < 2000 && h.world.Moving(walker); ++i) h.Run(0.05);
        const proto::WorldPosition end = h.world.Where(walker)->position;
        const proto::FlatPosition b = proto::ToFlat(end);
        const double went = std::hypot(b.x - a.x, b.z - a.z);
        MESSAGE("south " << length << ": refused " << int(why) << ", went " << went << " to region " << end.region << " (" << end.x << ", " << end.y
                         << ", " << end.z << ")");
        if (went < length - 2.0) ++stuck;
    }
    CHECK(stuck == 0);
}

TEST_CASE("Pathfinding over Hotan's town: the best of three searches finds every way one finds, never longer (game files)") {
    const auto dir = GameDir();
    if (dir.empty()) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    core::JobSystem jobs(4);
    game::NavLoader loader(fs, jobs);
    game::NavWorld nav;
    std::size_t wanted = 0;
    for (int dz = -1; dz <= 3; ++dz) {
        for (int dx = -1; dx <= 2; ++dx) {
            loader.Request(assets::RegionId(135 + dx, 92 + dz));
            ++wanted;
        }
    }
    for (int i = 0; i < 2000 && nav.Size() < wanted; ++i) {
        loader.Update(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(nav.Size() == wanted);
    // Points on the town's floors (the highest at each place: its platforms, the palace, the walkways), 300 to 1,500
    // apart, about Hotan's square and palace (regions 135..136, 92..94).
    std::mt19937 random(7);
    std::uniform_real_distribution<float> along(50.0f, 1870.0f);
    std::uniform_int_distribution<int> rx(135, 136), rz(92, 94);
    const auto point = [&]() {
        for (int tries = 0; tries < 50; ++tries) {
            const std::uint16_t region = assets::RegionId(rx(random), rz(random));
            const game::NavPosition p = nav.Drop(region, along(random), 400.0f, along(random));
            if (p.Valid() && p.y > 140.0f) return p;
        }
        return game::NavPosition{};
    };
    int pairs = 0, foundOne = 0, foundBest = 0, nearer = 0, shorter = 0, longer = 0;
    double lengthOne = 0, lengthBest = 0;
    for (int n = 0; n < 200 && pairs < 40; ++n) {
        const game::NavPosition a = point(), b = point();
        if (!a.Valid() || !b.Valid()) continue;
        const double ax = (a.region & 0xFF) * 1920.0 + a.x, az = (a.region >> 8) * 1920.0 + a.z;
        const double bx = (b.region & 0xFF) * 1920.0 + b.x, bz = (b.region >> 8) * 1920.0 + b.z;
        const double apart = std::hypot(bx - ax, bz - az);
        if (apart < 300.0 || apart > 1500.0) continue;
        ++pairs;
        std::vector<game::NavPosition> one, best;
        const bool okOne = game::FindNavPath(nav, a, b.region, b.x, b.z, one);
        game::NavPathOptions closest;
        closest.closest = true; // as a player's click: no way, the nearest place reached
        game::NavPathStats bestStats;
        const bool okBest = game::FindBestNavPath(nav, a, b.region, b.x, b.z, best, closest, &bestStats, &jobs) && !bestStats.partial;
        foundOne += okOne;
        foundBest += okBest;
        if (bestStats.partial) {
            // It ends nearer the goal than it started.
            REQUIRE_FALSE(best.empty());
            const double ex = (best.back().region & 0xFF) * 1920.0 + best.back().x, ez = (best.back().region >> 8) * 1920.0 + best.back().z;
            CHECK(std::hypot(bx - ex, bz - ez) < apart);
            ++nearer;
        }
        CHECK_MESSAGE((!okOne || okBest), "pair " << pairs << ": the best of three missed a way one search found");
        if (okOne && okBest) {
            const double l1 = game::NavPathLength(a, one), lb = game::NavPathLength(a, best);
            lengthOne += l1;
            lengthBest += lb;
            shorter += lb < l1 - 1.0;
            longer += lb > l1 + 1.0;
        }
        // Every leg of the best way is one straight walk that reaches its end (as the server walks a click: no limit on
        // the cells crossed), on the floor the way says.
        game::NavPosition at = a;
        int legNo = 0;
        for (const game::NavPosition& leg : best) {
            ++legNo;
            game::NavPosition end;
            const std::uint32_t bits = nav.Walk(at, leg.region, leg.x, leg.z, end, 100000);
            CHECK_MESSAGE((bits & (game::nav_walk::kBlocked | game::nav_walk::kFailed | game::nav_walk::kLimit)) == 0,
                          "pair " << pairs << " leg " << legNo << " of " << best.size() << ": bits " << bits);
            CHECK(end.instance == leg.instance);
            at = end;
        }
    }
    MESSAGE(pairs << " pairs: one search found " << foundOne << ", the best of three " << foundBest << " (and " << nearer
                  << " ways to the nearest place reached); where both did, the best was shorter "
                  << shorter << " times, longer " << longer << " (" << lengthOne << " against " << lengthBest << ")");
    CHECK(pairs >= 30);
    CHECK(foundBest >= foundOne);
    CHECK(longer == 0);
}

TEST_CASE("Pathfinding: a click's legs keep off the corners: Jangan's specialty trader to the field south of the wall (game files; the user's report 2026-11-14)") {
    const auto dir = GameDir();
    if (dir.empty()) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    core::JobSystem jobs(4);
    game::NavLoader loader(fs, jobs);
    game::NavWorld nav;
    std::size_t wanted = 0;
    for (int dz = -2; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            loader.Request(assets::RegionId(168 + dx, 97 + dz));
            ++wanted;
        }
    }
    for (int i = 0; i < 2000 && nav.Size() < wanted; ++i) {
        loader.Update(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(nav.Size() == wanted);
    // Where Hero stood at the specialty trader, and the click in the field (the run that stuck 19 short of a leg).
    const game::NavPosition from = nav.Drop(assets::RegionId(168, 97), 1772.32f, 100.0f, 480.22f);
    REQUIRE(from.Valid());
    const std::uint16_t field = assets::RegionId(168, 96);
    game::NavPathOptions options;
    options.closest = true;
    options.clearance = 8.0f;
    std::vector<game::NavPosition> path;
    REQUIRE(game::FindBestNavPath(nav, from, field, 780.0f, 1640.0f, path, options));
    REQUIRE(!path.empty());
    // Each leg one straight walk with 8 to spare on both sides (or a raw grid step where none had).
    game::NavPosition at = from;
    int clear = 0;
    for (const game::NavPosition& leg : path) {
        if (game::NavWalkClear(nav, at, leg.region, leg.x, leg.z, 8.0f)) ++clear;
        game::NavPosition end;
        CHECK((nav.Walk(at, leg.region, leg.x, leg.z, end) & (game::nav_walk::kBlocked | game::nav_walk::kFailed)) == 0);
        at = leg;
    }
    MESSAGE(path.size() << " legs, " << clear << " with the clearance");
    CHECK(clear >= int(path.size()) - 1); // the gate's own step through the wall has no room to spare
    // A walker a little off each leg's start (the server's stop, a step aside) still walks it.
    at = from;
    for (const game::NavPosition& leg : path) {
        for (const float off : {-4.0f, 4.0f}) {
            const game::NavPosition aside = nav.Drop(at.region, at.x + off, at.y + 20.0f, at.z + off);
            if (!aside.Valid()) continue;
            game::NavPosition end;
            const std::uint32_t walked = nav.Walk(aside, leg.region, leg.x, leg.z, end);
            CHECK_MESSAGE((walked & game::nav_walk::kBlocked) == 0, "a leg blocked 4 off its start");
        }
        at = leg;
    }
}

TEST_CASE("Pathfinding: the wall cost keeps a way off the walls: Jangan's specialty trader to the field south of the wall (game files; the user's report 2026-11-18)") {
    const auto dir = GameDir();
    if (dir.empty()) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    core::JobSystem jobs(4);
    game::NavLoader loader(fs, jobs);
    game::NavWorld nav;
    std::size_t wanted = 0;
    for (int dz = -2; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            loader.Request(assets::RegionId(168 + dx, 97 + dz));
            ++wanted;
        }
    }
    for (int i = 0; i < 2000 && nav.Size() < wanted; ++i) {
        loader.Update(nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(nav.Size() == wanted);
    const game::NavPosition from = nav.Drop(assets::RegionId(168, 97), 1772.32f, 100.0f, 480.22f);
    REQUIRE(from.Valid());
    const std::uint16_t field = assets::RegionId(168, 96);
    // The share of the way, a sample every 10, within 20 of a wall (a 20 walk in one of eight ways blocked).
    const auto nearWalls = [&](const std::vector<game::NavPosition>& path, int& samples) {
        int near = 0;
        samples = 0;
        game::NavPosition at = from;
        for (const game::NavPosition& leg : path) {
            const float ax = float(at.region % 256) * 1920.0f + at.x, az = float(at.region / 256) * 1920.0f + at.z;
            const float bx = float(leg.region % 256) * 1920.0f + leg.x, bz = float(leg.region / 256) * 1920.0f + leg.z;
            const float length = std::hypot(bx - ax, bz - az);
            for (float d = 0.0f; d < length; d += 10.0f) {
                const float wx = ax + (bx - ax) * d / length, wz = az + (bz - az) * d / length;
                const auto region = std::uint16_t(int(wz / 1920.0f) * 256 + int(wx / 1920.0f));
                const game::NavPosition p = nav.Drop(region, std::fmod(wx, 1920.0f), at.y + 20.0f, std::fmod(wz, 1920.0f));
                if (!p.Valid()) continue;
                ++samples;
                for (int k = 0; k < 8; ++k) {
                    const float angle = float(k) * std::numbers::pi_v<float> / 4.0f;
                    game::NavPosition end;
                    if ((nav.Walk(p, p.region, p.x + 20.0f * std::cos(angle), p.z + 20.0f * std::sin(angle), end) & game::nav_walk::kBlocked) != 0) {
                        ++near;
                        break;
                    }
                }
            }
            at = leg;
        }
        return near;
    };
    game::NavPathOptions options;
    options.closest = true;
    options.clearance = 15.0f;
    std::vector<game::NavPosition> plain, kept;
    REQUIRE(game::FindBestNavPath(nav, from, field, 780.0f, 1640.0f, plain, options));
    options.wallCost = 0.5f;
    REQUIRE(game::FindBestNavPath(nav, from, field, 780.0f, 1640.0f, kept, options));
    int plainSamples = 0, keptSamples = 0;
    const int plainNear = nearWalls(plain, plainSamples), keptNear = nearWalls(kept, keptSamples);
    MESSAGE("without the wall cost " << plainNear << " of " << plainSamples << " samples within 20 of a wall, " << game::NavPathLength(from, plain)
                                      << " long; with it " << keptNear << " of " << keptSamples << ", " << game::NavPathLength(from, kept) << " long");
    CHECK(keptNear * plainSamples <= plainNear * keptSamples); // a smaller share near the walls
    CHECK(game::NavPathLength(from, kept) <= game::NavPathLength(from, plain) * 1.15); // and not much longer
}

TEST_CASE("Game server: a bug report from the game becomes the website's support ticket, its screenshot in the database") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    core::JobSystem jobs(2);
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.rules = WorldRules();
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    net::NetService net(1);
    const auto pump = [&] { game.Poll(2ms); };
    REQUIRE_MESSAGE(game.Start(config, error), error);
    REQUIRE(server::CreateAccount(t.Db(), "reporter", "secret1", error) == server::CreateAccountResult::Created);
    bool ok = false;
    const std::int64_t account = server::FindAccount(t.Db(), "reporter", ok)->id;
    Client c(net, game.Port(), pump);
    REQUIRE(c.Greet());
    proto::LobbyEnter e;
    e.ticket = server::IssueTicket(t.Db(), account, config.id, 30, error);
    c.Send(e);
    proto::LobbyEntered entered;
    REQUIRE(c.Expect(entered));
    proto::CharacterCreate m;
    m.name = "Reporter";
    m.model = 100;
    m.chest = 1;
    m.legs = 2;
    m.shoes = 3;
    m.weapon = 10;
    c.Send(m);
    proto::LobbyResult r;
    REQUIRE(c.Expect(r));
    REQUIRE(r.status == proto::LobbyStatus::Ok);
    c.Send(proto::CharacterSelect{"Reporter"});
    REQUIRE(c.Expect(r));
    c.Send(proto::WorldEnter{});
    proto::WorldEntered in;
    REQUIRE(c.Expect(in));
    REQUIRE(in.status == proto::WorldStatus::Ok);

    // The report, then its screenshot (a JPEG's first bytes) in two parts.
    std::vector<std::uint8_t> image(70'000, 0x5A);
    image[0] = 0xFF, image[1] = 0xD8, image[2] = 0xFF;
    proto::BugReport report;
    report.category = 1; // graphics
    report.subject = "Arms through the sleeves";
    report.text = "The forearm shows through the garment's hands in streaks.";
    report.info = "client: 0.1.3\nGPU: test";
    report.imageBytes = std::uint32_t(image.size());
    c.Send(report);
    for (std::size_t at = 0; at < image.size(); at += proto::kBugPartBytes) {
        proto::BugReportPart part;
        part.bytes.assign(image.begin() + std::ptrdiff_t(at), image.begin() + std::ptrdiff_t(std::min(image.size(), at + proto::kBugPartBytes)));
        c.Send(part);
    }
    // Told its ticket's number in the chat.
    proto::ChatLine line;
    bool told = false;
    for (int i = 0; i < 20 && !told; ++i) {
        if (!c.Expect(line)) break;
        told = line.kind == proto::ChatKind::Notice && line.text.find("ticket #") != std::string::npos;
    }
    REQUIRE(told);
    const auto tickets = t.Db().Query("SELECT id, kind, category, source, jid, subject, client_info FROM sro_vt_account.sr_support_tickets");
    REQUIRE(tickets.Ok());
    REQUIRE(tickets.Rows() == 1);
    CHECK(tickets.Text(0, 1) == "bug");
    CHECK(tickets.Text(0, 2) == "graphics");
    CHECK(tickets.Text(0, 3) == "client");
    CHECK(tickets.Int(0, 4) == account);
    CHECK(tickets.Text(0, 5) == "Arms through the sleeves");
    CHECK(tickets.Text(0, 6).find("character: Reporter") != std::string::npos); // the server's own lines
    CHECK(tickets.Text(0, 6).find("client: 0.1.3") != std::string::npos);    // and the client's
    const auto files = t.Db().Query("SELECT mime, bytes, length(data), stored FROM sro_vt_account.sr_support_files WHERE ticket_id = $1", {tickets.Int(0, 0)});
    REQUIRE(files.Ok());
    REQUIRE(files.Rows() == 1);
    CHECK(files.Text(0, 0) == "image/jpeg");
    CHECK(files.Int(0, 1) == std::int64_t(image.size()));
    CHECK(files.Int(0, 2) == std::int64_t(image.size()));
    CHECK(files.Text(0, 3).starts_with("db:"));

    // Five an hour: four more without a screenshot are taken, the next is turned away.
    report.imageBytes = 0;
    int taken = 0;
    bool refused = false;
    for (int i = 0; i < 5; ++i) {
        c.Send(report);
        if (!c.Expect(line)) break;
        if (line.text.find("ticket #") != std::string::npos) ++taken;
        if (line.text.find("several bug reports") != std::string::npos) refused = true;
    }
    CHECK(taken == 4);
    CHECK(refused);
    game.Stop();
}
