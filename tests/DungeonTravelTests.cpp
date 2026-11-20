// Into the dungeons and between their floors: the navigation's
// teleport zones and the game server taking players through them (set SR_GAME_DIR, and SR_DB_URL for
// the server's case; otherwise they are skipped).

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <thread>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/FlagWorld.h"
#include "sr/game/NavDungeon.h"
#include "sr/game/NavPath.h"
#include "sr/game/NavLoader.h"
#include "sr/game/NavMesh.h"
#include "sr/server/GameServer.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;

namespace {

std::filesystem::path TravelGameDir() {
    const char* dir = std::getenv("SR_GAME_DIR");
    return dir ? std::filesystem::path(dir) : std::filesystem::path();
}

// Loads `regions` into `nav` (outdoor regions or dungeons) and waits for them.
void LoadNavigation(game::NavLoader& loader, game::NavWorld& nav, const std::vector<std::uint16_t>& regions) {
    for (const std::uint16_t r : regions) loader.Request(r);
    for (int i = 0; i < 1000; ++i) {
        loader.Update(nav);
        bool done = true;
        for (const std::uint16_t r : regions) done &= !loader.Loading(r);
        if (done) return;
        std::this_thread::sleep_for(10ms);
    }
}

// A zone's place: the centre of the first cell of that zone (a cell byte 0xC0 | its name), or of the
// far cell of the first edge marked with it (0x40 | its name), in the region's or dungeon's space.
struct ZonePlace {
    bool found = false;
    std::uint16_t region = 0;
    float x = 0, y = 0, z = 0;
};

ZonePlace ZoneCentre(const game::NavObjectMesh& mesh, const std::string& zone, const auto& toWorld, float baseY) {
    ZonePlace out;
    std::size_t index = mesh.events.size();
    for (std::size_t i = 0; i < mesh.events.size(); ++i) {
        if (mesh.events[i] == zone) index = i;
    }
    if (index == mesh.events.size()) return out;
    for (const auto& t : mesh.triangles) {
        if ((t.event & 0xC0) != 0 && (t.event & 0x3F) == index) {
            toWorld(t.cx, t.cz, out.x, out.z);
            out.y = baseY + t.cy;
            out.found = true;
            return out;
        }
    }
    return out;
}

ZonePlace FindZone(const game::NavWorld& nav, const std::vector<std::uint16_t>& regions, const std::string& zone) {
    for (const std::uint16_t r : regions) {
        if (const game::NavDungeon* d = nav.FindDungeon(r)) {
            for (const auto& block : d->Blocks()) {
                if (!block.mesh) continue;
                ZonePlace p = ZoneCentre(*block.mesh, zone, [&](float lx, float lz, float& wx, float& wz) { block.ToWorld(lx, lz, wx, wz); }, block.y);
                if (p.found) {
                    p.region = r;
                    return p;
                }
            }
        } else if (const game::NavRegion* region = nav.Find(r)) {
            for (const auto& inst : region->instances) {
                if (!inst.mesh) continue;
                ZonePlace p = ZoneCentre(*inst.mesh, zone, [&](float lx, float lz, float& wx, float& wz) { inst.ToWorld(lx, lz, wx, wz); }, inst.y);
                if (p.found) {
                    p.region = r;
                    return p;
                }
            }
        }
    }
    return {};
}

} // namespace

TEST_CASE("Navigation: walks report the teleport zones they enter: the tomb's door and floors, Donhwang's cave mouth (game files)") {
    const auto dir = TravelGameDir();
    if (dir.empty()) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    assets::TeleportTable teleports;
    REQUIRE_MESSAGE(teleports.Load(fs, error), error);
    core::JobSystem jobs(2);
    game::NavLoader loader(fs, jobs);
    game::NavWorld nav;

    // From a teleport point's arrival to the centre of a zone, on the navigation: the zone is reported.
    const auto walkInto = [&](const std::string& point, const std::string& zone, const std::vector<std::uint16_t>& regions) {
        CAPTURE(point);
        CAPTURE(zone);
        const assets::TeleportPoint* p = teleports.PointNamed(point);
        REQUIRE(p != nullptr);
        LoadNavigation(loader, nav, regions);
        const ZonePlace target = FindZone(nav, regions, zone);
        REQUIRE(target.found);
        const game::NavPosition from = nav.Drop(p->region, p->x, p->y + 5.0f, p->z);
        REQUIRE(from.Valid());
        MESSAGE(point << " arrives at " << p->x << "," << p->y << "," << p->z << "; " << zone << " at " << target.x << "," << target.y << "," << target.z
                      << " (" << std::hypot(target.x - p->x, target.z - p->z) << " away)");
        std::vector<std::string> zones;
        game::NavPosition to;
        nav.Walk(from, target.region, target.x, target.z, to, 100000, &zones);
        CHECK(std::find(zones.begin(), zones.end(), zone) != zones.end());
        // Standing in it and walking on inside it: not entered again.
        std::vector<std::string> again;
        game::NavPosition still;
        nav.Walk(to, target.region, target.x + 0.5f, target.z + 0.5f, still, 100000, &again);
        CHECK(std::find(again.begin(), again.end(), zone) == again.end());
        return to;
    };
    // The tomb's door outside Jangan (cells of the zone on the door's object), and B1's two zones (a
    // dungeon's floor): the way down to B2 and the way out.
    const std::uint16_t door = teleports.PointNamed("GATE_JINSI_OUT")->region;
    std::vector<std::uint16_t> around;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) around.push_back(assets::RegionId((door & 0xFF) + dx, (door >> 8) + dz));
    }
    walkInto("GATE_JINSI_OUT", "gate_jinsi_out", around);
    walkInto("GATE_JINSI_IN", "gate_jinsi_in", {0x8007});
    walkInto("GATE_JINSI_01x01_12", "gate_jinsi_01x01_12", {0x8007});
    walkInto("GATE_JINSI_02x01_09", "gate_jinsi_02x01_09", {0x8006});

    // Donhwang's cave mouth: its zone is edges of the mouth's object (0x40 | the name), crossed.
    const std::uint16_t mouth = teleports.PointNamed("GATE_DUNGEON_DH_IN")->region;
    std::vector<std::uint16_t> nearMouth;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) nearMouth.push_back(assets::RegionId((mouth & 0xFF) + dx, (mouth >> 8) + dz));
    }
    LoadNavigation(loader, nav, nearMouth);
    bool crossed = false;
    for (const std::uint16_t r : nearMouth) {
        const game::NavRegion* region = nav.Find(r);
        if (region == nullptr) continue;
        for (const auto& inst : region->instances) {
            if (!inst.mesh || std::find(inst.mesh->events.begin(), inst.mesh->events.end(), "gate_dungeon_dh_in") == inst.mesh->events.end()) continue;
            // Through each marked edge from its one cell to the other.
            for (const auto& e : inst.mesh->edges) {
                if ((e.event & 0xC0) == 0 || e.cellB == 0xFFFF) continue;
                const auto& a = inst.mesh->triangles[e.cellA];
                const auto& b = inst.mesh->triangles[e.cellB];
                float ax = 0, az = 0, bx = 0, bz = 0;
                inst.ToWorld(a.cx, a.cz, ax, az);
                inst.ToWorld(b.cx, b.cz, bx, bz);
                const game::NavPosition from = nav.Drop(r, ax, inst.y + a.cy + 2.0f, az);
                if (!from.Valid() || from.instance < 0) continue;
                std::vector<std::string> zones;
                game::NavPosition to;
                nav.Walk(from, r, bx, bz, to, 100000, &zones);
                crossed |= std::find(zones.begin(), zones.end(), "gate_dungeon_dh_in") != zones.end();
            }
        }
    }
    CHECK(crossed);
}

TEST_CASE("Game server: into the tomb of Qin Shi through its door, down to B2 and back, out again; refused below the link's level (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = TravelGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto teleports = std::make_shared<assets::TeleportTable>();
    REQUIRE_MESSAGE(teleports->Load(fs, error), error);
    const assets::TeleportPoint* outside = teleports->PointNamed("GATE_JINSI_OUT");
    const assets::TeleportPoint* inside = teleports->PointNamed("GATE_JINSI_IN");
    const assets::TeleportPoint* down = teleports->PointNamed("GATE_JINSI_01x01_12");
    const assets::TeleportPoint* b2 = teleports->PointNamed("GATE_JINSI_02x01_09");
    REQUIRE(outside != nullptr);
    REQUIRE(inside != nullptr);
    REQUIRE(down != nullptr);
    REQUIRE(b2 != nullptr);
    // The way in asks for level 70 (`_RefTeleLink` 55 -> 56: Restrict1 1, 70..999).
    const auto in = teleports->LinksFrom(outside->id);
    REQUIRE(in.size() == 1);
    CHECK(in.front()->to == inside->id);
    CHECK(in.front()->restrictions[0].kind == 1);
    CHECK(in.front()->restrictions[0].a == 70);
    CHECK(in.front()->restrictions[1].kind == 2);

    // The zones' places, from the navigation the client would walk.
    core::JobSystem navJobs(2);
    game::NavLoader loader(fs, navJobs);
    game::NavWorld nav;
    std::vector<std::uint16_t> around;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) around.push_back(assets::RegionId((outside->region & 0xFF) + dx, (outside->region >> 8) + dz));
    }
    LoadNavigation(loader, nav, around);
    LoadNavigation(loader, nav, {0x8007, 0x8006});
    const ZonePlace doorZone = FindZone(nav, around, "gate_jinsi_out");
    const ZonePlace outZone = FindZone(nav, {0x8007}, "gate_jinsi_in");
    const ZonePlace downZone = FindZone(nav, {0x8007}, "gate_jinsi_01x01_12");
    const ZonePlace upZone = FindZone(nav, {0x8006}, "gate_jinsi_02x01_09");
    REQUIRE(doorZone.found);
    REQUIRE(outZone.found);
    REQUIRE(downZone.found);
    REQUIRE(upZone.found);

    core::JobSystem jobs(2);
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    auto rules = std::make_shared<game::CharacterRules>();
    for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
    rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
    rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
    rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
    config.rules = rules;
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.devCommands = true;
    config.databaseThreads = 1;
    config.files = &fs;
    config.teleports = teleports;
    config.teleportZones = {{"gate_jinsi_out", "GATE_JINSI_OUT"},
                            {"gate_jinsi_in", "GATE_JINSI_IN"},
                            {"gate_jinsi_01x01_12", "GATE_JINSI_01x01_12"},
                            {"gate_jinsi_02x01_09", "GATE_JINSI_02x01_09"}};
    REQUIRE_MESSAGE(game.Start(config, error), error);
    net::NetService net(1);
    const auto pump = [&] { game.Poll(2ms); };

    // A character of `level` standing at a teleport point's arrival (in the world: placed, then entered).
    const auto make = [&](const std::string& name, int level, const assets::TeleportPoint& at) {
        REQUIRE(server::CreateAccount(t.Db(), name + "acct", "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        const std::int64_t account = server::FindAccount(t.Db(), name + "acct", ok)->id;
        auto c = std::make_unique<Client>(net, game.Port(), pump);
        REQUIRE(c->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), account, config.id, 30, error);
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
        const std::vector<server::CharacterRow> list = server::ListCharacters(t.Db(), account, config.id, 0, ok);
        REQUIRE(list.size() == 1);
        REQUIRE(server::SaveProgress(t.Db(), {{list[0].id, std::uint8_t(level), 0, 0, 0, 40, 40, 0}}));
        server::CharacterRow::Place here;
        here.region = at.region;
        here.x = at.x;
        here.y = at.y;
        here.z = at.z;
        REQUIRE(server::SavePlaces(t.Db(), {{list[0].id, here}}));
        c->Send(proto::CharacterSelect{name});
        REQUIRE(c->Expect(r));
        c->Send(proto::WorldEnter{});
        proto::WorldEntered start;
        REQUIRE(c->Expect(start));
        REQUIRE(start.status == proto::WorldStatus::Ok);
        return std::make_pair(std::move(c), start);
    };
    // Walks into a zone (clicks towards its centre until the server's answer comes).
    const auto walkInto = [&](Client& c, const ZonePlace& zone) {
        c.Send(proto::MoveTo{{zone.region, zone.x, zone.y, zone.z}});
    };

    // Below level 70: the door's zone refuses (the original's 0x1C15).
    {
        auto [c, start] = make("Low", 50, *outside);
        CHECK(start.position.region == outside->region);
        walkInto(*c, doorZone);
        proto::GateRefused refused;
        REQUIRE(c->Expect(refused, 15s));
        CHECK(refused.status == proto::GateStatus::WrongLevel);
    }
    // Level 75: in through the door to B1, down to B2, up again, out of the tomb.
    auto [c, start] = make("Deep", 75, *outside);
    walkInto(*c, doorZone);
    proto::WorldEntered there;
    REQUIRE(c->Expect(there, 15s));
    CHECK(there.position.region == 0x8007);
    CHECK(std::hypot(there.position.x - inside->x, there.position.z - inside->z) < 1.0f);
    // On B1's floor (the server holds the dungeon's navigation and put it down there).
    const game::NavPosition b1 = nav.Drop(0x8007, there.position.x, there.position.y + 5.0f, there.position.z);
    REQUIRE(b1.Valid());
    CHECK(std::fabs(b1.y - there.position.y) < 2.0f);

    // B1's way down is far along the floor: a developer's teleport to its arrival from B2, then into it.
    c->Send(proto::DevGoto{down->id});
    REQUIRE(c->Expect(there, 15s));
    CHECK(there.position.region == 0x8007);
    walkInto(*c, downZone);
    REQUIRE(c->Expect(there, 15s));
    CHECK(there.position.region == 0x8006);
    CHECK(std::hypot(there.position.x - b2->x, there.position.z - b2->z) < 1.0f);
    // And back up: B2's zone leads to B1's arrival beside the way down.
    walkInto(*c, upZone);
    REQUIRE(c->Expect(there, 15s));
    CHECK(there.position.region == 0x8007);
    CHECK(std::hypot(there.position.x - down->x, there.position.z - down->z) < 1.0f);
    // Out: B1's way out leads to the door outside.
    c->Send(proto::DevGoto{inside->id});
    REQUIRE(c->Expect(there, 15s));
    walkInto(*c, outZone);
    REQUIRE(c->Expect(there, 15s));
    CHECK(there.position.region == outside->region);
    CHECK(std::hypot(there.position.x - outside->x, there.position.z - outside->z) < 1.0f);
    game.Stop();
}

TEST_CASE("Navigation: Capture the Flag's event map (region 0x8009): each camp reaches the white pole through its room's doorway (game files)") {
    const auto dir = TravelGameDir();
    if (dir.empty()) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    core::JobSystem jobs(2);
    game::NavLoader loader(fs, jobs);
    game::NavWorld nav;
    LoadNavigation(loader, nav, {game::flag::kRegion});
    for (int team = 0; team < 2; ++team) {
        CAPTURE(team);
        const game::flag::Point& camp = game::flag::kCamps[team];
        const game::NavPosition from = nav.Drop(game::flag::kRegion, camp.x, camp.y + 5.0f, camp.z);
        REQUIRE(from.Valid());
        std::vector<game::NavPosition> path;
        game::NavPathOptions options;
        options.maxNodes = 200000;
        REQUIRE(game::FindBestNavPath(nav, from, game::flag::kRegion, game::flag::kPoles[0].x, game::flag::kPoles[0].z, path, options, nullptr, &jobs));
        std::string legs;
        for (const game::NavPosition& p : path) {
            legs += std::format(" ({:.0f}, {:.0f})", p.x, p.z);
        }
        MESSAGE("team " << team << ":" << legs);
        CHECK(path.size() >= 2);
    }
}