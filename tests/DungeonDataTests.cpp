// Dungeons (C4d): dungeoninfo.txt, and every .dof of the 1.188 data (set SR_GAME_DIR, otherwise
// the game-data case is skipped).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>

#include "sr/assets/Dungeon.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/Model.h"
#include "sr/core/Path.h"
#include "sr/game/NavDungeon.h"

using namespace sr;
using namespace sr::assets;

namespace {

std::span<const std::byte> Bytes(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// (x, z) inside the triangle, or at most `slack` outside an edge.
bool TriangleHolds(const sr::game::NavObjectMesh& mesh, std::uint32_t tri, float x, float z, float slack) {
    const auto& t = mesh.triangles[tri];
    for (int k = 0; k < 3; ++k) {
        const auto& u = mesh.vertices[t.v[k]];
        const auto& v = mesh.vertices[t.v[(k + 1) % 3]];
        const float ex = v.x - u.x, ez = v.z - u.z, length = std::hypot(ex, ez);
        if (length == 0.0f) continue;
        const float side = (ex * (z - u.z) - ez * (x - u.x)) / length;
        const float inside = ex * (t.cz - u.z) - ez * (t.cx - u.x);
        if ((inside >= 0 ? -side : side) > slack) return false;
    }
    return true;
}

} // namespace

TEST_CASE("dungeoninfo.txt: a service flag, a region id (| 0x8000) and a .dof per line, comments skipped") {
    const std::string text =
        "//service\t//region id\tfile\r\n1\t1\t\"Dungeon\\wchina\\Dunhwang_Cv.dof\"\r\n\r\n0\t9\t\"Dungeon\\wchina\\event.dof\"\r\n";
    std::vector<DungeonInfoEntry> entries;
    std::string error;
    REQUIRE_MESSAGE(DecodeDungeonInfo(Bytes(text), entries, error), error);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].inService);
    CHECK(entries[0].regionId == 0x8001);
    CHECK(entries[0].path == "Dungeon\\wchina\\Dunhwang_Cv.dof");
    CHECK_FALSE(entries[1].inService);
    CHECK(entries[1].regionId == 0x8009);
    CHECK_FALSE(DecodeDungeonInfo(Bytes("x \"a.dof\"\n"), entries, error));
    CHECK_FALSE(DecodeDungeonInfo(Bytes("1\t\"a.dof\"\n"), entries, error)); // v1.150's two columns
}

TEST_CASE("every .dof decodes; the listed dungeons' blocks and objects resolve") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) return;
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);

    const auto infoFile = fs.Read("dungeon\\dungeoninfo.txt");
    REQUIRE(infoFile.has_value());
    std::vector<DungeonInfoEntry> info;
    REQUIRE_MESSAGE(DecodeDungeonInfo(infoFile->Bytes(), info, error), error);
    REQUIRE(info.size() == 24);
    CHECK(info[0].regionId == 0x8001);
    CHECK(info[8].regionId == 0x8009);
    CHECK(std::count_if(info.begin(), info.end(), [](const DungeonInfoEntry& e) { return e.inService; }) == 17);

    std::size_t files = 0, decoded = 0, blocks = 0, objects = 0, lights = 0, water = 0;
    std::vector<std::string> failures;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = core::NormalizeAssetPath(path);
        if (!key.ends_with(".dof")) continue;
        ++files;
        const auto file = fs.Read(path);
        DungeonData dof;
        if (!DecodeDof(file->Bytes(), dof, error)) {
            failures.push_back(key + ": " + error);
            continue;
        }
        ++decoded;
        CAPTURE(key);
        blocks += dof.blocks.size();
        // Links are symmetric and hold every block's neighbours; the grid covers the box.
        bool linksOk = dof.links.size() == dof.blocks.size();
        int oneWay = 0, unlisted = 0;
        for (std::size_t b = 0; b < dof.blocks.size() && dof.links.size() == dof.blocks.size(); ++b) {
            for (std::uint32_t other : dof.links[b]) {
                const auto& back = dof.links[other];
                if (std::find(back.begin(), back.end(), static_cast<std::uint32_t>(b)) == back.end() && ++oneWay <= 3) {
                    MESSAGE("one-way link: block " << b << " (" << dof.blocks[b].name << ") -> " << other << " (" << dof.blocks[other].name << ")");
                }
            }
            for (std::uint32_t n : dof.blocks[b].neighbours) {
                if (std::find(dof.links[b].begin(), dof.links[b].end(), n) == dof.links[b].end() && ++unlisted <= 3) {
                    MESSAGE("neighbour not linked: block " << b << " (" << dof.blocks[b].name << ") -> " << n);
                }
            }
        }
        CHECK(linksOk);
        CHECK(oneWay == 0);
        // The fortress dungeon's passages 05 and 06 name each other as neighbours but are not in each
        // other's link lists (the one such pair in 1.188's data).
        CHECK(unlisted == (key == "dungeon\\wchina\\fortress_dungeon.dof" ? 2 : 0));
        for (int axis = 0; axis < 3; ++axis) {
            const float extent = dof.boundsMax[axis] - dof.boundsMin[axis];
            CHECK(dof.gridSize[axis] == static_cast<std::uint32_t>(std::ceil(extent / DungeonData::kGridCell)));
        }
        bool gridOk = true;
        for (const auto& cell : dof.grid) {
            gridOk &= (cell.key & 0x3FF) < dof.gridSize[0] && ((cell.key >> 10) & 0x3FF) < dof.gridSize[2] &&
                      (cell.key >> 20) < dof.gridSize[1];
            for (std::uint32_t b : cell.blocks) gridOk &= b < dof.blocks.size();
        }
        CHECK(gridOk);
        for (const auto& block : dof.blocks) {
            // Water objects are one quad each.
            for (const auto& o : block.objects) {
                if ((o.flags & 4) == 0) continue;
                ++water;
                ModelData model;
                REQUIRE_MESSAGE(LoadModel(fs, o.path, model, error), error);
                REQUIRE(!model.parts.empty());
                CHECK(model.parts[0].mesh.vertices.size() == 4);
                CHECK(model.parts[0].mesh.indices.size() == 6);
            }
            objects += block.objects.size();
            lights += block.lights.size();
            std::uint32_t obstacles = 0;
            for (const auto& o : block.objects) obstacles += (o.flags & 2) ? 1 : 0;
            CHECK(obstacles == block.obstacles);
        }
    }
    CHECK(files == 21);
    CHECK(decoded == 20);
    REQUIRE(failures.size() == 1);
    CHECK(failures[0].starts_with("dungeon\\wchina\\dunhwang_cv1.dof"));
    CHECK(water == 50); // all in the jinsi floors (1.188 lists them; v1.150 did not)
    MESSAGE(decoded << " .dof decode: " << blocks << " blocks, " << objects << " objects, " << lights << " lights");

    // The dungeons in service: every block's .bsr names a floor mesh with a navmesh, every object's
    // .bsr is there.
    std::size_t navBlocks = 0, listedBlocks = 0, missingObjects = 0, listedWater = 0;
    for (const DungeonInfoEntry& entry : info) {
        if (!entry.inService) continue;
        CAPTURE(entry.path);
        const auto file = fs.Read(entry.path);
        REQUIRE(file.has_value());
        DungeonData dof;
        REQUIRE_MESSAGE(DecodeDof(file->Bytes(), dof, error), error);
        CHECK(dof.regionId == entry.regionId);
        for (const auto& block : dof.blocks) {
            ++listedBlocks;
            ResourceData resource;
            const auto bsr = fs.Read(block.path);
            REQUIRE_MESSAGE(bsr.has_value(), block.path);
            REQUIRE_MESSAGE(DecodeBsr(bsr->Bytes(), resource, error), error);
            if (!resource.navMeshPath.empty()) {
                ObjectNavMesh nav;
                const auto bms = fs.Read(resource.navMeshPath);
                if (bms && DecodeBmsNavMesh(bms->Bytes(), nav, error) && !nav.empty()) ++navBlocks;
            }
            for (const auto& object : block.objects) {
                missingObjects += fs.Exists(object.path) ? 0 : 1;
                listedWater += (object.flags & 4) ? 1 : 0;
            }
        }
    }
    CHECK(listedBlocks == 504);
    CHECK(navBlocks == 504);
    CHECK(missingObjects == 0);
    CHECK(listedWater == 50);
}

TEST_CASE("the Donwhang cave's navigation: every block has a floor, walks go through its doorways") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) return;
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    for (const char* path : {"dungeon\\wchina\\dunhwang_cv.dof", "dungeon\\wchina\\event.dof"}) {
        CAPTURE(path);
        const auto file = fs.Read(path);
        REQUIRE(file.has_value());
        DungeonData data;
        REQUIRE_MESSAGE(DecodeDof(file->Bytes(), data, error), error);
        const auto dungeon = sr::game::NavDungeon::Build(data, [&](std::uint32_t b) -> std::shared_ptr<const sr::game::NavObjectMesh> {
            ResourceData resource;
            const auto bsr = fs.Read(data.blocks[b].path);
            if (!bsr || !DecodeBsr(bsr->Bytes(), resource, error) || resource.navMeshPath.empty()) return nullptr;
            const auto bms = fs.Read(resource.navMeshPath);
            ObjectNavMesh nav;
            if (!bms || !DecodeBmsNavMesh(bms->Bytes(), nav, error) || nav.empty()) return nullptr;
            return sr::game::NavObjectMesh::Build(nav);
        });

        // A drop at every floor triangle's centre finds a floor at that height.
        std::size_t triangles = 0, found = 0, sameBlock = 0;
        std::vector<sr::game::NavPosition> starts;
        for (std::size_t b = 0; b < dungeon->Blocks().size(); ++b) {
            const auto& block = dungeon->Blocks()[b];
            REQUIRE(block.mesh);
            for (const auto& t : block.mesh->triangles) {
                ++triangles;
                float wx = 0, wz = 0;
                block.ToWorld(t.cx, t.cz, wx, wz);
                const sr::game::NavPosition p = dungeon->Drop(wx, t.cy + block.y, wz);
                if (!p.Valid() || std::fabs(p.y - (t.cy + block.y)) > 0.1f) continue;
                ++found;
                sameBlock += p.instance == static_cast<std::int32_t>(b) ? 1 : 0;
                if (triangles % 7 == 0) starts.push_back(p);
            }
        }
        CHECK(found * 100 >= triangles * 99);

        // Walks from there end on a floor triangle that holds them, at its height; some change block.
        std::mt19937 random(8001);
        std::uniform_real_distribution<float> offset(-400.0f, 400.0f);
        std::size_t walks = 0, blocked = 0, crossed = 0, bad = 0;
        for (const sr::game::NavPosition& start : starts) {
            sr::game::NavPosition to;
            const std::uint32_t result = dungeon->Walk(start, start.x + offset(random), start.z + offset(random), to);
            ++walks;
            blocked += (result & sr::game::nav_walk::kBlocked) ? 1 : 0;
            crossed += to.instance != start.instance ? 1 : 0;
            const auto& block = dungeon->Blocks()[static_cast<std::size_t>(to.instance)];
            float lx = 0, lz = 0;
            block.ToLocal(to.x, to.z, lx, lz);
            const auto& t = block.mesh->triangles[static_cast<std::size_t>(to.cell)];
            const bool onIt = std::fabs(to.y - (block.y + t.HeightAt(lx, lz))) < 0.05f &&
                              TriangleHolds(*block.mesh, static_cast<std::uint32_t>(to.cell), lx, lz, 0.3f);
            if (!onIt && ++bad <= 3) {
                MESSAGE("walk " << result << " from block " << start.instance << " (" << start.x << ", " << start.z << ") cell "
                                << start.cell << " to block " << to.instance << " cell " << to.cell << " at (" << to.x << ", " << to.y << ", "
                                << to.z << "), triangle height " << block.y + t.HeightAt(lx, lz) << ", local " << lx << ", " << lz);
            }
        }
        CHECK(bad == 0);
        CHECK(crossed > 0);
        MESSAGE(dungeon->Blocks().size() << " blocks, " << dungeon->Doors() << " doorway edges matched; " << found << " of " << triangles
                                         << " floor centres dropped (" << sameBlock << " onto their own block); " << walks << " walks ("
                                         << blocked << " blocked, " << crossed << " into another block)");
    }
}
