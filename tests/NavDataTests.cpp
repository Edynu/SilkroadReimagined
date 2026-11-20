// Navigation over the real 1.188 data (C4b; set SR_GAME_DIR, otherwise skipped): every .nvm and
// object navmesh decodes and holds the rules the client relies on, and the queries
// give consistent answers over every region of the world.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <random>
#include <string>

#include "sr/assets/FileSystem.h"
#include "sr/assets/Model.h"
#include "sr/assets/NavMesh.h"
#include "sr/assets/Terrain.h"
#include "sr/core/Path.h"
#include "sr/game/NavMesh.h"

using namespace sr;
using namespace sr::game;

namespace {

std::filesystem::path GameDir() {
    const char* dir = std::getenv("SR_GAME_DIR");
    return dir == nullptr ? std::filesystem::path{} : std::filesystem::path(dir);
}

struct NavData {
    assets::FileSystem fs;
    assets::NavMapInfo map;
    assets::AssetIndex objects;
    std::map<std::uint32_t, std::shared_ptr<const NavObjectMesh>> meshes; // by object id
};

// The indexes and every object's navmesh (object.ifo -> .bsr or a .cpd's collision .bsr -> the .bms).
bool LoadNavData(NavData& d) {
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(d.fs, GameDir(), error), error);
    const auto map = d.fs.Read("navmesh\\mapinfo.mfo");
    const auto objects = d.fs.Read("navmesh\\object.ifo");
    REQUIRE(map.has_value());
    REQUIRE(objects.has_value());
    REQUIRE_MESSAGE(assets::DecodeNavMapInfo(map->Bytes(), d.map, error), error);
    REQUIRE_MESSAGE(assets::DecodeObjectIndex(objects->Bytes(), d.objects, error), error);
    for (const auto& [id, entry] : d.objects) {
        assets::ObjectNavMesh nav;
        if (assets::LoadObjectNavMesh(d.fs, entry.path, nav, error) && !nav.empty()) d.meshes[id] = NavObjectMesh::Build(nav);
    }
    return true;
}

std::shared_ptr<const NavRegion> LoadRegion(NavData& d, std::uint16_t id, std::string& error) {
    const auto file = d.fs.Read(assets::RegionNavMeshFile(id));
    assets::RegionNavData data;
    if (!file) {
        error = "missing";
        return nullptr;
    }
    if (!assets::DecodeRegionNavMesh(file->Bytes(), data, error)) return nullptr;
    return NavRegion::Build(id, std::move(data), [&](std::uint32_t objectId) {
        const auto it = d.meshes.find(objectId);
        return it == d.meshes.end() ? nullptr : it->second;
    });
}

// Where a query left a walker is a place it can stand: a terrain cell holding the point (not
// blocked, at the terrain's height) or an object triangle under it at the triangle's height.
bool Standable(const NavWorld& world, const NavPosition& p, std::string& why) {
    const NavRegion* region = world.Find(p.region);
    if (region == nullptr || !p.Valid()) {
        why = "invalid";
        return false;
    }
    if (p.instance < 0) {
        const auto& cell = region->data.cells[static_cast<std::size_t>(p.cell)];
        constexpr float slack = 0.25f;
        if (p.x < cell.minX - slack || p.x > cell.maxX + slack || p.z < cell.minZ - slack || p.z > cell.maxZ + slack) {
            why = "outside its cell";
            return false;
        }
        if (static_cast<std::uint32_t>(p.cell) >= region->data.openCells) {
            why = "in a blocked cell";
            return false;
        }
        if (std::fabs(p.y - NavTerrainHeight(*region, p.x, p.z)) > 0.01f) {
            why = "off the terrain height";
            return false;
        }
        return true;
    }
    const NavRegion::Instance& inst = region->instances[static_cast<std::size_t>(p.instance)];
    if (!inst.Walkable() || static_cast<std::size_t>(p.cell) >= inst.mesh->triangles.size()) {
        why = "bad object cell";
        return false;
    }
    float lx = 0, lz = 0;
    inst.ToLocal(p.x, p.z, lx, lz);
    const NavObjectMesh::Triangle& t = inst.mesh->triangles[static_cast<std::size_t>(p.cell)];
    if (std::fabs(p.y - (inst.y + t.HeightAt(lx, lz))) > 0.05f) {
        why = "off its triangle's height";
        return false;
    }
    // Inside the triangle (2D, with some slack for the 0.2 pull-backs and float error).
    const auto& a = inst.mesh->vertices[t.v[0]];
    const auto& b = inst.mesh->vertices[t.v[1]];
    const auto& c = inst.mesh->vertices[t.v[2]];
    const auto distanceOutside = [&](const NavObjectMesh::Vertex& u, const NavObjectMesh::Vertex& v) {
        const float ex = v.x - u.x, ez = v.z - u.z, length = std::hypot(ex, ez);
        if (length == 0.0f) return 0.0f;
        const float side = (ex * (lz - u.z) - ez * (lx - u.x)) / length;
        const float inside = ex * (t.cz - u.z) - ez * (t.cx - u.x);
        return inside >= 0 ? -side : side;
    };
    if (std::max({distanceOutside(a, b), distanceOutside(b, c), distanceOutside(c, a)}) > 0.3f) {
        why = "outside its triangle";
        return false;
    }
    return true;
}

} // namespace

TEST_CASE("every navmesh decodes and keeps the client's rules") {
    if (GameDir().empty()) return;
    NavData d;
    LoadNavData(d);
    CHECK(d.map.Count() == 3300);
    CHECK(d.objects.size() == 2767);
    MESSAGE(d.meshes.size() << " objects with a navmesh");

    std::size_t files = 0, decoded = 0, older = 0, olderEnabled = 0, enabledMissing = 0;
    std::size_t instances = 0, unresolved = 0, links = 0, badLinks = 0, centroids = 0, centroidMisses = 0;
    std::size_t heightMismatches = 0, waterMismatches = 0;
    std::string error;
    std::map<std::string, int> errors;
    for (const std::string& path : d.fs.ListFiles()) {
        const std::string key = core::NormalizeAssetPath(path);
        if (!key.starts_with("navmesh\\nv_") || !key.ends_with(".nvm")) continue;
        ++files;
        const auto id = static_cast<std::uint16_t>(std::stoul(key.substr(11, 4), nullptr, 16));
        const auto file = d.fs.Read(path);
        assets::RegionNavData data;
        if (!assets::DecodeRegionNavMesh(file->Bytes(), data, error)) {
            ++older;
            olderEnabled += d.map.Enabled(id) ? 1 : 0;
            ++errors[error];
            continue;
        }
        ++decoded;
        if (!d.map.Enabled(id)) continue;
        CAPTURE(path);

        // Tiles: blocked exactly when their cell is beyond the open ones; blocked cells have no edges.
        bool tilesOk = true;
        for (const auto& t : data.tiles) tilesOk &= ((t.flags & 1) != 0) == (t.cell >= data.openCells);
        CHECK(tilesOk);
        bool edgesOk = true;
        for (const auto& e : data.internalEdges) edgesOk &= e.cellA < data.openCells && (e.cellB == 0xFFFF || e.cellB < data.openCells);
        for (const auto& e : data.borderEdges) {
            edgesOk &= e.cellA < data.openCells && e.regionA == id;
            const int dx = e.sideA == 1 ? 1 : e.sideA == 3 ? -1 : 0, dz = e.sideA == 0 ? 1 : e.sideA == 2 ? -1 : 0;
            edgesOk &= e.regionB == NavRegionId(NavRegionX(id) + dx, NavRegionZ(id) + dz);
        }
        CHECK(edgesOk);

        // The heights and water are the .m's.
        const auto mesh = d.fs.Read(assets::RegionFile(id, ".m"));
        REQUIRE(mesh.has_value());
        assets::TerrainRegion terrain;
        REQUIRE_MESSAGE(assets::DecodeMapMesh(mesh->Bytes(), terrain, error), error);
        for (int z = 0; z < assets::kNavHeights; ++z) {
            for (int x = 0; x < assets::kNavHeights; ++x) {
                const int bx = std::min(x / 16, 5), bz = std::min(z / 16, 5);
                const auto& block = terrain.blocks[static_cast<std::size_t>(bz * 6 + bx)];
                const float m = block.heights[static_cast<std::size_t>((z - bz * 16) * 17 + (x - bx * 16))];
                heightMismatches += std::fabs(m - data.heights[static_cast<std::size_t>(z * 97 + x)]) > 0.01f ? 1 : 0;
            }
        }
        for (std::size_t b = 0; b < 36; ++b) {
            const bool walkable = (data.blockFlags[b] & 2) != 0;
            waterMismatches += walkable != (terrain.blocks[b].waterType == 1) || data.waterHeights[b] != terrain.blocks[b].waterHeight;
        }

        // Every instance has its object's navmesh; links are in range.
        const auto region = NavRegion::Build(id, std::move(data), [&](std::uint32_t objectId) {
            const auto it = d.meshes.find(objectId);
            return it == d.meshes.end() ? nullptr : it->second;
        });
        for (std::size_t i = 0; i < region->instances.size(); ++i) {
            const NavRegion::Instance& inst = region->instances[i];
            ++instances;
            if (!inst.mesh) {
                const std::uint32_t objectId = region->data.objects[i].objectId;
                if (++unresolved <= 8) {
                    const auto entry = d.objects.find(objectId);
                    MESSAGE("region " << id << ": object " << objectId << " has no navmesh ("
                                      << (entry == d.objects.end() ? std::string("not in object.ifo") : entry->second.path) << ")");
                }
                continue;
            }
            const auto& object = region->data.objects[i];
            for (std::uint32_t k = 0; k < object.linkCount; ++k) {
                const auto& link = region->data.links[object.firstLink + k];
                if (link.object == 0xFFFF) continue;
                ++links;
                const bool bad = link.ownEdge >= inst.mesh->outlineCount || !region->instances[link.object].mesh ||
                                 link.objectEdge >= region->instances[link.object].mesh->outlineCount;
                if (bad && ++badLinks <= 4) {
                    MESSAGE("region " << id << ": object " << object.objectId << " edge " << link.ownEdge << " links object "
                                      << region->data.objects[link.object].objectId << " edge " << link.objectEdge);
                }
            }
            // Its triangles lie in terrain cells that list it (the placement's rotation sense).
            if (object.ownerRegion != id) continue;
            for (const NavObjectMesh::Triangle& t : inst.mesh->triangles) {
                float wx = 0, wz = 0;
                inst.ToWorld(t.cx, t.cz, wx, wz);
                if (wx < 0 || wz < 0 || wx >= kNavRegionSize || wz >= kNavRegionSize) continue;
                ++centroids;
                const auto& cell = region->data.cells[region->TileAt(wx, wz).cell];
                bool listed = false;
                for (std::uint32_t k = 0; k < cell.objectCount; ++k) listed |= region->data.cellObjects[cell.firstObject + k] == i;
                centroidMisses += listed ? 0 : 1;
            }
        }
    }
    for (std::uint32_t id = 0; id < 0x10000; ++id) {
        const auto region = static_cast<std::uint16_t>(id);
        enabledMissing += d.map.Enabled(region) && !d.fs.Exists(assets::RegionNavMeshFile(region)) ? 1 : 0;
    }
    CHECK(files == 5040);
    CHECK(decoded == 4939);
    CHECK(older == 101);
    CHECK(errors["older layout with 4-byte tiles"] == 101);
    CHECK(olderEnabled == 0);
    CHECK(enabledMissing == 0);
    CHECK(unresolved == 0);
    CHECK(badLinks == 0);
    CHECK(heightMismatches <= 4); // four vertices of the whole world differ (by up to 13.9)
    CHECK(waterMismatches == 0);
    CHECK(centroidMisses == 0);
    MESSAGE(instances << " object instances, " << links << " links, " << centroids << " object triangles placed, "
                      << heightMismatches << " height samples differ from the .m");
}

TEST_CASE("navigation queries over every region: heights, drops, rays and walks") {
    if (GameDir().empty()) return;
    NavData d;
    LoadNavData(d);
    std::mt19937 random(150);
    std::uniform_real_distribution<float> anywhere(1.0f, kNavRegionSize - 1.0f), offset(-300.0f, 300.0f);

    std::size_t regions = 0, drops = 0, valid = 0, onObjects = 0, rays = 0, rayMismatches = 0, walks = 0, blocked = 0,
                badWalks = 0, objectWalks = 0;
    std::string error, why;
    for (int z = 0; z < 256; ++z) {
        for (int x = 0; x < 256; ++x) {
            const std::uint16_t id = NavRegionId(x, z);
            if (!d.map.Enabled(id)) continue;
            NavWorld world; // the region alone: walks stop at its borders
            const auto region = LoadRegion(d, id, error);
            REQUIRE_MESSAGE(region, error);
            world.Add(region);
            ++regions;

            // Heights at the grid's vertices, with the walkable water's floor.
            bool heightsOk = true;
            for (int vz = 0; vz < assets::kNavHeights; vz += 8) {
                for (int vx = 0; vx < assets::kNavHeights; vx += 8) {
                    float h = region->data.heights[static_cast<std::size_t>(vz * 97 + vx)];
                    const std::size_t block = static_cast<std::size_t>(std::min(vz / 16, 5) * 6 + std::min(vx / 16, 5));
                    if (region->data.blockFlags[block] & 2) h = std::max(h, region->data.waterHeights[block]);
                    heightsOk &= std::fabs(NavTerrainHeight(*region, vx * 20.0f, vz * 20.0f) - h) < 0.01f;
                }
            }
            CHECK(heightsOk);

            for (int i = 0; i < 24; ++i) {
                const float px = anywhere(random), pz = anywhere(random);
                // A drop from the ground's height lands somewhere standable or nowhere (blocked).
                const float ground = NavTerrainHeight(*region, px, pz);
                const NavPosition p = world.Drop(id, px, ground, pz);
                ++drops;
                if (p.Valid()) {
                    ++valid;
                    onObjects += p.instance >= 0 ? 1 : 0;
                    if (!Standable(world, p, why)) FAIL_CHECK(why << " after a drop at " << px << ", " << pz << " in " << id);
                }

                // A ray straight down from high above meets a surface the drop finds again.
                const float origin[3] = {px, 10000.0f, pz}, down[3] = {0, -1, 0};
                NavHit hit;
                if (world.Ray(id, origin, down, 40000.0f, hit)) {
                    ++rays;
                    if (hit.instance < 0) {
                        rayMismatches += std::fabs(hit.y - NavTerrainHeight(*region, hit.x, hit.z)) > 0.01f;
                    } else {
                        const NavPosition again = world.Drop(id, hit.x, hit.y, hit.z);
                        rayMismatches += !again.Valid() || std::fabs(again.y - hit.y) > 0.05f;
                    }
                }

                // Walks from valid points towards points around them end somewhere standable.
                if (!p.Valid()) continue;
                NavPosition at = p;
                for (int step = 0; step < 4; ++step) {
                    NavPosition to;
                    const std::uint32_t result = world.Walk(at, id, std::clamp(at.x + offset(random), 0.0f, 1919.0f),
                                                            std::clamp(at.z + offset(random), 0.0f, 1919.0f), to);
                    ++walks;
                    blocked += (result & nav_walk::kBlocked) ? 1 : 0;
                    objectWalks += to.instance >= 0 ? 1 : 0;
                    if (!Standable(world, to, why)) {
                        if (++badWalks <= 10) MESSAGE(why << ": walk " << result << " in region " << id << " from " << at.x << ", " << at.z);
                        break;
                    }
                    at = to;
                }
            }
        }
    }
    CHECK(regions == 3300);
    CHECK(badWalks == 0);
    CHECK(rayMismatches * 100 <= rays); // object triangles at the edge of the cells that list them
    MESSAGE(regions << " regions: " << valid << " of " << drops << " drops valid (" << onObjects << " on objects), " << rays
                    << " rays (" << rayMismatches << " not found again by a drop), " << walks << " walks (" << blocked
                    << " blocked, " << objectWalks << " ending on objects)");
}

TEST_CASE("the fortresses' structure places are event objects of the navigation (objectstring.ifo's ids are their keys)") {
    if (GameDir().empty()) return;
    NavData d;
    LoadNavData(d);
    const auto file = d.fs.Read("navmesh\\objectstring.ifo");
    REQUIRE(file.has_value());
    std::vector<assets::NamedSpot> spots;
    std::string error;
    REQUIRE_MESSAGE(assets::DecodeObjectStrings(file->Bytes(), spots, error), error);
    // The gates, towers and hearts must be found in the navigation: objects of their own (Jangan's, by key),
    // or event cells of a fortress's big object (by the zone's name, the object's turn after it).
    std::map<std::uint16_t, std::shared_ptr<const NavRegion>> regions;
    const auto regionAt = [&](int x, int z) -> const NavRegion* {
        const std::uint16_t id = NavRegionId(x, z);
        auto& held = regions[id];
        if (!held && d.map.Enabled(id)) held = LoadRegion(d, id, error);
        return held.get();
    };
    int checked = 0, byKey = 0, byName = 0;
    for (const assets::NamedSpot& spot : spots) {
        if (!spot.name.starts_with("STRUCTURE_POS_")) continue;
        if (spot.name.find("FORT_STONE") == std::string::npos && spot.name.find("GUARD_TOWER") == std::string::npos && spot.name.find("_GATE_") == std::string::npos) continue;
        ++checked;
        std::string lower = spot.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        bool key = false, name = false;
        for (int dz = -2; dz <= 2; ++dz) {
            for (int dx = -2; dx <= 2; ++dx) {
                const NavRegion* region = regionAt(NavRegionX(spot.region) + dx, NavRegionZ(spot.region) + dz);
                if (region == nullptr) continue;
                for (const NavRegion::Instance& i : region->instances) {
                    if (i.key == spot.id && i.mesh && i.mesh->marker) {
                        key = true;
                    }
                    if (!i.mesh) continue;
                    for (const std::string& n : i.mesh->events) name = name || n.starts_with(lower + "_r");
                }
            }
        }
        byKey += key ? 1 : 0;
        byName += name ? 1 : 0;
        CAPTURE(spot.name);
        CHECK((key || name));
    }
    MESSAGE(checked << " gates, towers and hearts: " << byKey << " objects of their own, " << byName << " event cells");
    CHECK(checked == 19);
}

TEST_CASE("walks cross region borders around Constantinople") {
    if (GameDir().empty()) return;
    NavData d;
    LoadNavData(d);
    NavWorld world;
    std::string error;
    for (int z = 103; z <= 107; ++z) {
        for (int x = 76; x <= 80; ++x) {
            const std::uint16_t id = NavRegionId(x, z);
            if (!d.map.Enabled(id)) continue;
            const auto region = LoadRegion(d, id, error);
            REQUIRE_MESSAGE(region, error);
            world.Add(region);
        }
    }
    REQUIRE(world.Size() >= 9);
    std::mt19937 random(78105);
    std::uniform_real_distribution<float> anywhere(0.0f, kNavRegionSize), offset(-600.0f, 600.0f);
    std::size_t walks = 0, crossed = 0, bad = 0;
    std::string why;
    for (int i = 0; i < 3000; ++i) {
        const std::uint16_t id = NavRegionId(77 + static_cast<int>(random() % 3), 104 + static_cast<int>(random() % 3));
        NavPosition at = world.Drop(id, anywhere(random), 0, anywhere(random));
        if (!at.Valid()) continue;
        at = world.Drop(at.region, at.x, NavTerrainHeight(*world.Find(at.region), at.x, at.z), at.z);
        if (!at.Valid()) continue;
        NavPosition to;
        world.Walk(at, at.region, at.x + offset(random), at.z + offset(random), to);
        ++walks;
        crossed += to.region != at.region ? 1 : 0;
        if (!Standable(world, to, why) && ++bad <= 10) MESSAGE(why);
    }
    CHECK(bad == 0);
    CHECK(crossed > 0);
    MESSAGE(world.Size() << " regions, " << walks << " walks, " << crossed << " into another region");
}
