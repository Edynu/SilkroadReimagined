// Navigation (C4b) on hand-made regions and objects: the .nvm decoder, terrain heights, dropping a
// point, walking (cells, region borders, objects entered, crossed and left, doors) and rays.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

#include "sr/assets/NavMesh.h"
#include "sr/game/NavDungeon.h"
#include "sr/game/NavMesh.h"
#include "sr/game/NavPath.h"

using namespace sr;
using namespace sr::game;
using assets::RegionNavData;

namespace {

class Writer {
public:
    std::vector<std::byte> bytes;
    void Raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        bytes.insert(bytes.end(), b, b + n);
    }
    void U32(std::uint32_t v) { Raw(&v, 4); }
    void U16(std::uint16_t v) { Raw(&v, 2); }
    void U8(std::uint8_t v) { Raw(&v, 1); }
    void F32(float v) { Raw(&v, 4); }
};

// The .nvm layout (assets/src/NavMesh.cpp), written from the decoded form.
std::vector<std::byte> EncodeNvm(const RegionNavData& d) {
    Writer w;
    w.Raw("JMXVNVM 1000", 12);
    w.U16(static_cast<std::uint16_t>(d.objects.size()));
    for (const RegionNavData::Object& o : d.objects) {
        w.U32(o.objectId);
        for (float v : o.position) w.F32(v);
        w.U16(o.field10);
        w.F32(o.yaw);
        w.U16(o.uid);
        w.U16(o.field18);
        w.U16(o.field1A);
        w.U16(o.ownerRegion);
        w.U16(o.linkCount);
        for (std::uint32_t i = 0; i < o.linkCount; ++i) {
            const RegionNavData::Link& l = d.links[o.firstLink + i];
            w.U16(l.object);
            w.U16(l.objectEdge);
            w.U16(l.ownEdge);
        }
    }
    w.U32(static_cast<std::uint32_t>(d.cells.size()));
    w.U32(d.openCells);
    for (const RegionNavData::Cell& c : d.cells) {
        w.F32(c.minX);
        w.F32(c.minZ);
        w.F32(c.maxX);
        w.F32(c.maxZ);
        w.U8(c.objectCount);
        for (std::uint32_t i = 0; i < c.objectCount; ++i) w.U16(d.cellObjects[c.firstObject + i]);
    }
    const auto edges = [&](const std::vector<RegionNavData::Edge>& list, bool border) {
        w.U32(static_cast<std::uint32_t>(list.size()));
        for (const RegionNavData::Edge& e : list) {
            w.F32(e.ax);
            w.F32(e.az);
            w.F32(e.bx);
            w.F32(e.bz);
            w.U8(e.flags);
            w.U8(e.sideA);
            w.U8(e.sideB);
            w.U16(e.cellA);
            w.U16(e.cellB);
            if (border) {
                w.U16(e.regionA);
                w.U16(e.regionB);
            }
        }
    };
    edges(d.borderEdges, true);
    edges(d.internalEdges, false);
    for (const RegionNavData::Tile& t : d.tiles) {
        w.U16(t.cell);
        w.U16(0);
        w.U16(t.flags);
        w.U16(t.tile2d);
    }
    for (float h : d.heights) w.F32(h);
    for (std::uint8_t b : d.blockFlags) w.U8(b);
    for (float h : d.waterHeights) w.F32(h);
    return w.bytes;
}

RegionNavData::Edge Side(float ax, float az, float bx, float bz, std::uint8_t flags, std::uint8_t sideA, std::uint16_t cellA,
                         std::uint8_t sideB = 0xFF, std::uint16_t cellB = 0xFFFF) {
    RegionNavData::Edge e;
    e.ax = ax;
    e.az = az;
    e.bx = bx;
    e.bz = bz;
    e.flags = flags;
    e.sideA = sideA;
    e.cellA = cellA;
    e.sideB = sideB;
    e.cellB = cellB;
    return e;
}

RegionNavData EmptyRegion() {
    RegionNavData d;
    d.tiles.resize(std::size_t{assets::kNavTiles} * assets::kNavTiles);
    d.heights.assign(std::size_t{assets::kNavHeights} * assets::kNavHeights, 0.0f);
    return d;
}

// Two open cells side by side in the south half (0 west, 1 east, joined by an open edge), a blocked
// cell over the north half; closed sides all round except the east border, which opens into
// region `east` (its cell 0).
RegionNavData SplitRegion(std::uint16_t id, std::uint16_t east) {
    RegionNavData d = EmptyRegion();
    d.cells = {{0, 0, 960, 960}, {960, 0, 1920, 960}, {0, 960, 1920, 1920}};
    d.openCells = 2;
    d.internalEdges = {
        Side(960, 0, 960, 960, 4, 1, 0, 3, 1),   // open, west cell's max x / east cell's min x
        Side(0, 960, 960, 960, 2, 0, 0),        // closed sides
        Side(960, 960, 1920, 960, 2, 0, 1),
        Side(0, 0, 960, 0, 2, 2, 0),
        Side(960, 0, 1920, 0, 2, 2, 1),
        Side(0, 0, 0, 960, 2, 3, 0),
    };
    RegionNavData::Edge border = Side(1920, 0, 1920, 960, 8, 1, 1, 3, 0);
    border.regionA = id;
    border.regionB = east;
    d.borderEdges = {border};
    for (int tz = 0; tz < assets::kNavTiles; ++tz) {
        for (int tx = 0; tx < assets::kNavTiles; ++tx) {
            RegionNavData::Tile& t = d.tiles[static_cast<std::size_t>(tz * assets::kNavTiles + tx)];
            t.cell = tz >= 48 ? 2 : tx < 48 ? 0 : 1;
            t.flags = tz >= 48 ? 1 : 0;
        }
    }
    return d;
}

std::shared_ptr<const NavRegion> Build(std::uint16_t id, RegionNavData data,
                                       const std::vector<std::shared_ptr<const NavObjectMesh>>& meshes = {}) {
    // Through the file format, so the decoder is part of every test.
    RegionNavData decoded;
    std::string error;
    const std::vector<std::byte> file = EncodeNvm(data);
    REQUIRE_MESSAGE(assets::DecodeRegionNavMesh(file, decoded, error), error);
    return NavRegion::Build(id, std::move(decoded), [&](std::uint32_t objectId) {
        return objectId < meshes.size() ? meshes[objectId] : nullptr;
    });
}

// A flat rectangle of two triangles at height y, from (x0, z0) to (x1, z1) in object space.
// Outline edges: 0 south (z0), 1 east (x1), 2 north (z1), 3 west (x0), with the given flags;
// the diagonal from the south-west to the north-east corner is internal (triangle 0 south-east of
// it, 1 north-west).
assets::ObjectNavMesh Rectangle(float x0, float z0, float x1, float z1, float y, const std::uint8_t flags[4],
                                std::uint8_t diagonalFlags = 4) {
    assets::ObjectNavMesh m;
    m.vertices = {{x0, y, z0}, {x1, y, z0}, {x1, y, z1}, {x0, y, z1}};
    // Outward corner normals as the file's angle indices: (cos t, -sin t), t = i 2 pi / 256.
    const float normals[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (const auto& n : normals) {
        double t = std::atan2(-n[1], n[0]);
        if (t < 0) t += 2 * std::numbers::pi;
        m.directions.push_back(static_cast<std::uint8_t>(std::lround(t / (2 * std::numbers::pi) * 256) & 0xFF));
    }
    m.cells = {{0, 1, 2}, {0, 2, 3}};
    m.outline = {{0, 1, 0, 0xFFFF, flags[0], 0}, {1, 2, 0, 0xFFFF, flags[1], 0}, {2, 3, 1, 0xFFFF, flags[2], 0},
                 {3, 0, 1, 0xFFFF, flags[3], 0}};
    m.internal = {{0, 2, 0, 1, diagonalFlags, 0}};
    m.grid.originX = x0;
    m.grid.originZ = z0;
    m.grid.width = static_cast<std::uint32_t>(std::ceil((x1 - x0) / assets::ObjectNavMesh::kGridTile));
    m.grid.height = static_cast<std::uint32_t>(std::ceil((z1 - z0) / assets::ObjectNavMesh::kGridTile));
    for (std::uint32_t i = 0; i < m.grid.width * m.grid.height; ++i) {
        m.grid.start.push_back(static_cast<std::uint32_t>(m.grid.edges.size()));
        for (std::uint16_t e = 0; e < 4; ++e) m.grid.edges.push_back(e); // every edge in every tile: enough here
    }
    m.grid.start.push_back(static_cast<std::uint32_t>(m.grid.edges.size()));
    m.bounds.min = {x0, y, z0};
    m.bounds.max = {x1, y, z1};
    return m;
}

// One open cell over the whole region, closed all round, flat at height 0, with `objects` on it.
RegionNavData OpenRegion(const std::vector<RegionNavData::Object>& objects) {
    RegionNavData d = EmptyRegion();
    d.cells = {{0, 0, 1920, 1920}};
    d.openCells = 1;
    d.internalEdges = {Side(0, 1920, 1920, 1920, 2, 0, 0), Side(1920, 0, 1920, 1920, 2, 1, 0), Side(0, 0, 1920, 0, 2, 2, 0),
                       Side(0, 0, 0, 1920, 2, 3, 0)};
    d.objects = objects;
    d.cells[0].objectCount = static_cast<std::uint8_t>(objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) d.cellObjects.push_back(static_cast<std::uint16_t>(i));
    return d;
}

RegionNavData::Object Place(std::uint32_t objectId, float x, float y, float z, float yaw, std::uint16_t uid, std::uint16_t region) {
    RegionNavData::Object o;
    o.objectId = objectId;
    o.position[0] = x;
    o.position[1] = y;
    o.position[2] = z;
    o.yaw = yaw;
    o.uid = uid;
    o.ownerRegion = region;
    return o;
}

constexpr std::uint16_t kRegion = NavRegionId(10, 10);
constexpr std::uint8_t kOpen = 0, kLedge = nav_edge::kLedge, kWall = nav_edge::kBlockBToA | nav_edge::kBlockAToB;

} // namespace

TEST_CASE("the .nvm decoder reads what the layout writes") {
    RegionNavData d = SplitRegion(kRegion, NavRegionId(11, 10));
    d.objects = {Place(7, 100, 2, 300, 0.5f, 42, kRegion)};
    d.links = {{0xFFFF, 1, 2}};
    d.objects[0].linkCount = 1;
    d.cells[1].objectCount = 1;
    d.cells[1].firstObject = 0;
    d.cellObjects = {0};
    d.blockFlags[3] = 2;
    d.waterHeights[3] = 12.5f;
    const std::vector<std::byte> file = EncodeNvm(d);
    RegionNavData out;
    std::string error;
    REQUIRE_MESSAGE(assets::DecodeRegionNavMesh(file, out, error), error);
    CHECK(out.objects.size() == 1);
    CHECK(out.objects[0].uid == 42);
    CHECK(out.objects[0].position[2] == 300.0f);
    REQUIRE(out.links.size() == 1);
    CHECK(out.links[0].ownEdge == 2);
    CHECK(out.openCells == 2);
    CHECK(out.cells[1].objectCount == 1);
    CHECK(out.borderEdges[0].regionB == NavRegionId(11, 10));
    CHECK(out.internalEdges[0].cellB == 1);
    CHECK(out.tiles[95 * 96].flags == 1);
    CHECK(out.waterHeights[3] == 12.5f);

    // The older 4-byte-tile files and anything not read exactly are refused.
    std::vector<std::byte> shorter(file.begin(), file.end() - static_cast<std::ptrdiff_t>(96 * 96 * 4));
    CHECK_FALSE(assets::DecodeRegionNavMesh(shorter, out, error));
    CHECK(error == "older layout with 4-byte tiles");
    std::vector<std::byte> longer = file;
    longer.push_back(std::byte{0});
    CHECK_FALSE(assets::DecodeRegionNavMesh(longer, out, error));
    CHECK(assets::RegionNavMeshFile(NavRegionId(0xA8, 0x62)) == "navmesh\\nv_62a8.nvm");
}

TEST_CASE("terrain height: two triangles a tile, the diagonal alternating, raised to walkable water") {
    RegionNavData d = SplitRegion(kRegion, 0);
    for (int z = 0; z < assets::kNavHeights; ++z) {
        for (int x = 0; x < assets::kNavHeights; ++x) {
            d.heights[static_cast<std::size_t>(z * assets::kNavHeights + x)] = static_cast<float>((x * 3 + z * 7) % 11);
        }
    }
    d.blockFlags[1] = 2; // block (1, 0): walkable water at 8
    d.waterHeights[1] = 8.0f;
    const auto region = Build(kRegion, d);
    const auto h = [&](int x, int z) { return d.heights[static_cast<std::size_t>(z * assets::kNavHeights + x)]; };

    // Every grid vertex outside the water block.
    for (int z = 0; z < assets::kNavHeights; z += 5) {
        for (int x = 0; x < 16; ++x) CHECK(NavTerrainHeight(*region, x * 20.0f, z * 20.0f) == doctest::Approx(h(x, z)));
    }
    // Tile (0, 0), same parity: the diagonal (0,0)-(1,1); at (0.75, 0.25) the x-side triangle.
    CHECK(NavTerrainHeight(*region, 15, 5) == doctest::Approx(h(0, 0) + 0.75f * (h(1, 0) - h(0, 0)) + 0.25f * (h(1, 1) - h(1, 0))));
    CHECK(NavTerrainHeight(*region, 5, 15) == doctest::Approx(h(0, 0) + 0.75f * (h(0, 1) - h(0, 0)) + 0.25f * (h(1, 1) - h(0, 1))));
    // Tile (1, 0), other parity: the diagonal (1,0)-(0,1).
    CHECK(NavTerrainHeight(*region, 25, 5) == doctest::Approx(h(1, 0) + 0.25f * (h(2, 0) - h(1, 0)) + 0.25f * (h(1, 1) - h(1, 0))));
    CHECK(NavTerrainHeight(*region, 35, 15) ==
          doctest::Approx(h(2, 1) + 0.25f * (h(1, 1) - h(2, 1)) + 0.25f * (h(2, 0) - h(2, 1))));
    // Planar across a triangle: the midpoint of two points of the same triangle.
    const float a = NavTerrainHeight(*region, 21, 2), b = NavTerrainHeight(*region, 29, 6);
    CHECK(NavTerrainHeight(*region, 25, 4) == doctest::Approx(0.5f * (a + b)));
    // The water block: never below its surface.
    for (float x = 320; x < 640; x += 13) CHECK(NavTerrainHeight(*region, x, 100) >= 8.0f);
    CHECK(NavTerrainHeight(*region, 400, 100) == 8.0f);
}

TEST_CASE("drop: a valid cell on open ground, nothing on a blocked tile or where no region is held") {
    NavWorld world;
    world.Add(Build(kRegion, SplitRegion(kRegion, 0)));
    NavPosition p = world.Drop(kRegion, 480, 0, 480);
    CHECK(p.Valid());
    CHECK(p.cell == 0);
    CHECK(p.instance == -1);
    CHECK(world.Drop(kRegion, 1500, 0, 100).cell == 1);
    CHECK_FALSE(world.Drop(kRegion, 480, 0, 1500).Valid()); // the blocked north half
    CHECK_FALSE(world.Drop(kRegion, 2500, 0, 100).Valid()); // the east neighbour is not held
    // Coordinates wrap into the region they fall in.
    world.Add(Build(NavRegionId(11, 10), SplitRegion(NavRegionId(11, 10), 0)));
    p = world.Drop(kRegion, 2500, 0, 100);
    CHECK(p.Valid());
    CHECK(p.region == NavRegionId(11, 10));
    CHECK(p.x == doctest::Approx(580.0f));
}

TEST_CASE("walk on the terrain: across an open edge, stopped at a closed side, into the next region") {
    const std::uint16_t east = NavRegionId(11, 10);
    NavWorld world;
    world.Add(Build(kRegion, SplitRegion(kRegion, east)));
    NavPosition to;

    // West cell to east cell.
    const NavPosition from = world.Drop(kRegion, 480, 0, 480);
    CHECK(world.Walk(from, kRegion, 1440, 480, to) == nav_walk::kReached);
    CHECK(to.cell == 1);
    CHECK(to.x == 1440.0f);

    // North into the blocked half: stopped 0.2 inside, towards the cell's centre.
    CHECK(world.Walk(from, kRegion, 480, 1440, to) == nav_walk::kBlocked);
    CHECK(to.cell == 0);
    CHECK(to.z == doctest::Approx(959.8f));
    CHECK(to.x == doctest::Approx(480.0f));

    // East across the border: blocked while the neighbour is not held, through once it is.
    const NavPosition edge = world.Drop(kRegion, 1800, 0, 480);
    CHECK(world.Walk(edge, east, 100, 480, to) == nav_walk::kBlocked);
    CHECK(to.region == kRegion);
    CHECK(to.x == doctest::Approx(1919.8f));
    world.Add(Build(east, SplitRegion(east, 0)));
    CHECK(world.Walk(edge, east, 100, 480, to) == nav_walk::kReached);
    CHECK(to.region == east);
    CHECK(to.cell == 0);
    CHECK(to.x == doctest::Approx(100.0f));

    // A crossing limit and a target too far.
    CHECK(world.Walk(from, kRegion, 1440, 480, to, 0) == nav_walk::kLimit);
    CHECK(world.Walk(from, kRegion, 4000, 480, to) == nav_walk::kFailed);
    CHECK(world.Walk(NavPosition{}, kRegion, 10, 10, to) == nav_walk::kFailed);
}

// A wall across the middle with a gap at its west end: the south strip (cell 0), the gap (1, west of x 640), the
// north strip (2); the block east of the gap (3) closed.
RegionNavData WallRegion() {
    RegionNavData d = EmptyRegion();
    d.cells = {{0, 0, 1920, 640}, {0, 640, 640, 1280}, {0, 1280, 1920, 1920}, {640, 640, 1920, 1280}};
    d.openCells = 3;
    d.internalEdges = {
        Side(0, 640, 640, 640, 4, 0, 0, 2, 1),     // open: the south strip's north side / the gap's south side
        Side(0, 1280, 640, 1280, 4, 0, 1, 2, 2),   // open: the gap's north side / the north strip's south side
        Side(640, 640, 1920, 640, 2, 0, 0),        // the wall's sides
        Side(640, 1280, 1920, 1280, 2, 2, 2),
        Side(640, 640, 640, 1280, 2, 1, 1),
        Side(0, 0, 1920, 0, 2, 2, 0),              // the region's edges
        Side(0, 0, 0, 640, 2, 3, 0),
        Side(1920, 0, 1920, 640, 2, 1, 0),
        Side(0, 640, 0, 1280, 2, 3, 1),
        Side(0, 1280, 0, 1920, 2, 3, 2),
        Side(1920, 1280, 1920, 1920, 2, 1, 2),
        Side(0, 1920, 1920, 1920, 2, 0, 2),
    };
    for (int tz = 0; tz < assets::kNavTiles; ++tz) {
        for (int tx = 0; tx < assets::kNavTiles; ++tx) {
            RegionNavData::Tile& t = d.tiles[static_cast<std::size_t>(tz * assets::kNavTiles + tx)];
            t.cell = tz < 32 ? 0 : tz >= 64 ? 2 : tx < 32 ? 1 : 3;
            t.flags = t.cell == 3 ? 1 : 0;
        }
    }
    return d;
}

TEST_CASE("a way round a wall: every leg one straight walk, through the gap") {
    NavWorld world;
    world.Add(Build(kRegion, WallRegion()));
    const NavPosition from = world.Drop(kRegion, 1500, 0, 300);
    REQUIRE(from.Valid());
    NavPosition to;
    CHECK(world.Walk(from, kRegion, 1500, 1600, to) == nav_walk::kBlocked); // straight north: the wall

    std::vector<NavPosition> path;
    NavPathStats stats;
    REQUIRE(FindNavPath(world, from, kRegion, 1500, 1600, path, {}, &stats));
    REQUIRE(path.size() >= 2);
    CHECK(path.back().x == doctest::Approx(1500.0f).epsilon(0.001));
    CHECK(path.back().z == doctest::Approx(1600.0f).epsilon(0.001));
    NavPosition at = from;
    bool throughGap = false;
    for (const NavPosition& leg : path) {
        CHECK(world.Walk(at, leg.region, leg.x, leg.z, to) == nav_walk::kReached);
        at = to;
        throughGap = throughGap || (leg.x < 640.0f && leg.z > 600.0f && leg.z < 1320.0f);
    }
    CHECK(throughGap);
    CHECK(path.size() <= 4); // pulled tight: to the gap's corners and on, not grid point by grid point
    MESSAGE("legs ", path.size(), ", points expanded ", stats.expanded, ", walks ", stats.walks);

    // A straight way (within a leg's length) is the goal alone; the start itself none.
    REQUIRE(FindNavPath(world, from, kRegion, 500, 200, path));
    CHECK(path.size() == 1);
    REQUIRE(FindNavPath(world, from, kRegion, 1500, 300, path));
    CHECK(path.empty());

    // A place kept out of (where a walker stood stuck): the way goes round it; one over the gap closes the north.
    NavPathOptions avoiding;
    avoiding.avoid = {{kRegion, 1000, 300, 100}};
    REQUIRE(FindNavPath(world, from, kRegion, 500, 300, path, avoiding));
    CHECK(path.size() >= 2);
    at = from;
    for (const NavPosition& leg : path) {
        const float dx = leg.x - at.x, dz = leg.z - at.z, length2 = dx * dx + dz * dz;
        const float t = std::clamp(((1000.0f - at.x) * dx + (300.0f - at.z) * dz) / length2, 0.0f, 1.0f);
        CHECK(std::hypot(at.x + t * dx - 1000.0f, at.z + t * dz - 300.0f) >= 100.0f);
        at = leg;
    }
    avoiding.avoid = {{kRegion, 320, 960, 400}};
    CHECK_FALSE(FindNavPath(world, from, kRegion, 1500, 1600, path, avoiding));

    // No way: the goal inside the wall, or the search out of room.
    CHECK_FALSE(FindNavPath(world, from, kRegion, 1500, 900, path));
    // Near it will do (an NPC behind a counter): the way ends at the wall's near side, within reach.
    NavPathOptions near;
    near.arrive = 300.0f;
    REQUIRE(FindNavPath(world, from, kRegion, 1500, 900, path, near));
    REQUIRE_FALSE(path.empty());
    CHECK(std::hypot(path.back().x - 1500.0f, path.back().z - 900.0f) <= 300.0f);
    CHECK(path.back().z < 640.0f);
    NavPathOptions small;
    small.maxNodes = 50;
    CHECK_FALSE(FindNavPath(world, from, kRegion, 1500, 1600, path, small));
}

TEST_CASE("walk onto, under, across and off an object") {
    // A deck 100 x 40 at height 50 centred on (500, 500): open ends east and west, ledges north and
    // south. A wall block 40 x 40 at (1000, 500).
    const std::uint8_t deckFlags[4] = {kLedge, kOpen, kLedge, kOpen};
    const std::uint8_t wallFlags[4] = {kWall, kWall, kWall, kWall};
    const std::vector<std::shared_ptr<const NavObjectMesh>> meshes = {
        NavObjectMesh::Build(Rectangle(-50, -20, 50, 20, 0, deckFlags)),
        NavObjectMesh::Build(Rectangle(-20, -20, 20, 20, 0, wallFlags)),
    };
    NavWorld world;
    world.Add(Build(kRegion, OpenRegion({Place(0, 500, 50, 500, 0, 1, kRegion), Place(1, 1000, 0, 500, 0, 2, kRegion)}), meshes));
    NavPosition to;

    // In from the west end, then along the deck.
    const NavPosition west = world.Drop(kRegion, 400, 0, 500);
    REQUIRE(west.Valid());
    CHECK(world.Walk(west, kRegion, 520, 500, to) == nav_walk::kReached);
    CHECK(to.instance == 0);
    CHECK(to.y == doctest::Approx(50.0f));
    CHECK(to.x == doctest::Approx(520.0f));
    const NavPosition onDeck = to;

    // Under it from the south: its sides are ledges, not entries.
    const NavPosition south = world.Drop(kRegion, 520, 0, 450);
    CHECK(world.Walk(south, kRegion, 520, 550, to) == nav_walk::kReached);
    CHECK(to.instance == -1);
    CHECK(to.y == 0.0f);

    // On it, a ledge stops the walker; the far end lets it down onto the ground.
    CHECK(world.Walk(onDeck, kRegion, 520, 560, to) == nav_walk::kBlocked);
    CHECK(to.instance == 0);
    CHECK(to.z < 520.0f);
    CHECK(to.z > 519.0f);
    CHECK(world.Walk(onDeck, kRegion, 600, 500, to) == nav_walk::kReached);
    CHECK(to.instance == -1);
    CHECK(to.x == doctest::Approx(600.0f));
    CHECK(to.y == 0.0f);
    // A short step off the end stops just outside it (the walk's last leg starts within 5 units).
    NavPosition nearEnd = onDeck;
    CHECK(world.Walk(onDeck, kRegion, 549, 500, nearEnd) == nav_walk::kReached);
    CHECK(world.Walk(nearEnd, kRegion, 551, 500, to) == nav_walk::kLeftObject);
    CHECK(to.instance == -1);
    CHECK(to.x == doctest::Approx(550.2f));

    // The wall: stopped just outside, on the ground.
    const NavPosition beforeWall = world.Drop(kRegion, 900, 0, 500);
    CHECK(world.Walk(beforeWall, kRegion, 1100, 500, to) == nav_walk::kBlocked);
    CHECK(to.instance == -1);
    CHECK(to.x < 980.0f);
    CHECK(to.x > 979.98f);

    // Dropping picks the surface nearest in height.
    NavPosition p = world.Drop(kRegion, 520, 0, 500);
    CHECK(p.instance == -1);
    p = world.Drop(kRegion, 520, 45, 500);
    CHECK(p.instance == 0);
    CHECK(p.y == doctest::Approx(50.0f));
}

TEST_CASE("walk: a fortress's event object stands in the way only while it blocks") {
    const std::uint8_t open[4] = {0, 0, 0, 0};
    assets::ObjectNavMesh mesh = Rectangle(100, 400, 200, 600, 0, open);
    mesh.events = {"event_gate"};
    REQUIRE(mesh.IsEventMarker());
    RegionNavData::Object placed;
    placed.objectId = 7;
    placed.position[0] = 300.0f;
    placed.uid = 5;
    placed.ownerRegion = kRegion;
    const auto built = NavObjectMesh::Build(mesh);
    NavWorld world;
    world.Add(NavRegion::Build(kRegion, OpenRegion({placed}), [&](std::uint32_t id) { return id == 7 ? built : nullptr; }));
    const std::uint32_t key = (std::uint32_t{kRegion} << 16) | 5u;
    const NavPosition from = world.Drop(kRegion, 350, 0, 500);
    REQUIRE(from.Valid());
    NavPosition to;
    // A marker is walked as terrain by default...
    CHECK(world.Walk(from, kRegion, 650, 500, to) == nav_walk::kReached);
    CHECK(to.x == doctest::Approx(650.0f));
    // ...stops the walk at its edge while it blocks, the walker staying outside...
    world.SetZoneBlocks(key, "", true);
    CHECK(world.Walk(from, kRegion, 650, 500, to) == nav_walk::kBlocked);
    CHECK(to.x < 405.0f);
    CHECK(to.x > 380.0f);
    CHECK(to.instance == -1);
    // ...and lets it through once it stops blocking (a gate down).
    world.SetZoneBlocks(key, "", false);
    CHECK(world.Walk(from, kRegion, 650, 500, to) == nav_walk::kReached);
}

TEST_CASE("walk: an event cell inside a big object stops the walk while its zone blocks") {
    const std::uint8_t open[4] = {0, 0, 0, 0};
    assets::ObjectNavMesh mesh = Rectangle(100, 400, 400, 700, 10, open);
    mesh.events = {"structure_pos_ht_gate_01_r90"};
    mesh.cellEvents = {0, 0xC0}; // the north-west triangle is the zone's
    RegionNavData::Object placed;
    placed.objectId = 7;
    placed.uid = 5;
    placed.ownerRegion = kRegion;
    const auto built = NavObjectMesh::Build(mesh);
    NavWorld world;
    world.Add(NavRegion::Build(kRegion, OpenRegion({placed}), [&](std::uint32_t id) { return id == 7 ? built : nullptr; }));
    // On the south-east triangle, walking to the north-west one.
    const NavPosition from = world.Drop(kRegion, 350, 10, 450);
    REQUIRE(from.instance == 0);
    NavPosition to;
    CHECK(world.Walk(from, kRegion, 150, 650, to) == nav_walk::kReached);
    world.SetZoneBlocks(0, "structure_pos_ht_gate_01", true);
    CHECK(world.Walk(from, kRegion, 150, 650, to) == nav_walk::kBlocked);
    CHECK(to.instance == 0);
    CHECK(to.cell == from.cell);
    // Once inside, one may leave.
    world.SetZoneBlocks(0, "structure_pos_ht_gate_01", false);
    const NavPosition inside = world.Drop(kRegion, 150, 10, 650);
    REQUIRE(inside.instance == 0);
    world.SetZoneBlocks(0, "structure_pos_ht_gate_01", true);
    CHECK(world.Walk(inside, kRegion, 350, 450, to) == nav_walk::kReached);
}

TEST_CASE("walk across a turned object, through one-way edges and doors") {
    // The deck turned a quarter (its length along z), and its diagonal crossable one way only:
    // from triangle 1 (north-west in object space) to 0, not back.
    const std::uint8_t deckFlags[4] = {kLedge, kOpen, kLedge, kOpen};
    const std::vector<std::shared_ptr<const NavObjectMesh>> oneWay = {
        NavObjectMesh::Build(Rectangle(-50, -20, 50, 20, 0, deckFlags, nav_edge::kInternal | nav_edge::kBlockAToB)),
    };
    const float quarter = std::numbers::pi_v<float> / 2;
    NavWorld world;
    world.Add(Build(kRegion, OpenRegion({Place(0, 500, 10, 500, quarter, 1, kRegion)}), oneWay));
    NavPosition to;
    // Object x runs along world z: its west end (x = -50) is at world z 450.
    const NavPosition south = world.Drop(kRegion, 500, 0, 400);
    CHECK(world.Walk(south, kRegion, 500, 470, to) == nav_walk::kReached);
    CHECK(to.instance == 0);
    CHECK(to.y == doctest::Approx(10.0f));
    // Object x' = x cos - z sin: (10, -10) lies in triangle 0, (-10, 10) in triangle 1; crossing
    // from 0 (cell A) to 1 is blocked, from 1 to 0 is not.
    const NavRegion::Instance& deck = world.Find(kRegion)->instances[0];
    float ax = 0, az = 0, bx = 0, bz = 0;
    deck.ToWorld(10, -10, ax, az);
    deck.ToWorld(-10, 10, bx, bz);
    CHECK(ax == doctest::Approx(510.0f)); // a quarter turn: object (x, z) at world (-z, x) from the origin
    CHECK(az == doctest::Approx(510.0f));
    const NavPosition inA = world.Drop(kRegion, ax, 20, az), inB = world.Drop(kRegion, bx, 20, bz);
    REQUIRE(inA.cell == 0);
    REQUIRE(inB.cell == 1);
    CHECK(world.Walk(inA, kRegion, bx, bz, to) == nav_walk::kBlocked);
    CHECK(to.cell == 0);
    CHECK(world.Walk(inB, kRegion, ax, az, to) == nav_walk::kReached);
    CHECK(to.cell == 0);

    // Doors: deck A's east end (a link edge) opens onto deck B's west end, 30 higher.
    const std::uint8_t aFlags[4] = {kLedge, nav_edge::kLink, kLedge, kOpen};
    const std::uint8_t bFlags[4] = {kLedge, kOpen, kLedge, nav_edge::kLink};
    const std::vector<std::shared_ptr<const NavObjectMesh>> decks = {
        NavObjectMesh::Build(Rectangle(-50, -20, 50, 20, 0, aFlags)),
        NavObjectMesh::Build(Rectangle(-50, -20, 50, 20, 0, bFlags)),
    };
    RegionNavData linked = OpenRegion({Place(0, 500, 50, 500, 0, 1, kRegion), Place(1, 600, 80, 500, 0, 2, kRegion)});
    linked.links = {{1, 3, 1}}; // A's edge 1 (east) -> B's edge 3 (west)
    linked.objects[0].linkCount = 1;
    NavWorld doors;
    doors.Add(Build(kRegion, linked, decks));
    const NavPosition onA = doors.Drop(kRegion, 520, 60, 500);
    REQUIRE(onA.instance == 0);
    CHECK(doors.Walk(onA, kRegion, 600, 500, to) == nav_walk::kLink);
    CHECK(to.instance == 1);
    CHECK(to.y == doctest::Approx(80.0f));
    // Without the link entry the door fails.
    linked.objects[0].linkCount = 0;
    NavWorld broken;
    broken.Add(Build(kRegion, linked, decks));
    CHECK((broken.Walk(onA, kRegion, 600, 500, to) & nav_walk::kFailed) != 0);
    CHECK(to.instance == 0);
}

TEST_CASE("rays meet the nearest of the terrain and the objects") {
    const std::uint8_t deckFlags[4] = {kLedge, kOpen, kLedge, kOpen};
    const std::vector<std::shared_ptr<const NavObjectMesh>> meshes = {NavObjectMesh::Build(Rectangle(-50, -20, 50, 20, 0, deckFlags))};
    RegionNavData d = OpenRegion({Place(0, 500, 50, 500, 0, 1, kRegion)});
    d.blockFlags[2] = 2; // walkable water at 5 over block (2, 0)
    d.waterHeights[2] = 5.0f;
    NavWorld world;
    world.Add(Build(kRegion, d, meshes));
    NavHit hit;

    const float down[3] = {0, -1, 0};
    const float above[3] = {520, 200, 500};
    REQUIRE(world.Ray(kRegion, above, down, 1000, hit));
    CHECK(hit.instance == 0);
    CHECK(hit.y == doctest::Approx(50.0f));
    CHECK(hit.distance == doctest::Approx(150.0f));

    // Slanting down under the deck's height: the ground beyond.
    const float s = 1.0f / std::sqrt(1.0f + 0.05f * 0.05f);
    const float slant[3] = {s, -0.05f * s, 0};
    const float low[3] = {300, 30, 500};
    REQUIRE(world.Ray(kRegion, low, slant, 1000, hit));
    CHECK(hit.instance == -1);
    CHECK(hit.x == doctest::Approx(900.0f).epsilon(0.001));
    CHECK(hit.y == doctest::Approx(0.0f).scale(1.0));
    // Out of reach.
    CHECK_FALSE(world.Ray(kRegion, low, slant, 500, hit));

    // Diagonally down onto the deck.
    const float r = std::sqrt(0.5f);
    const float diagonal[3] = {r, -r, 0};
    const float high[3] = {420, 100, 500};
    REQUIRE(world.Ray(kRegion, high, diagonal, 1000, hit));
    CHECK(hit.instance == 0);
    CHECK(hit.x == doctest::Approx(470.0f));

    // The walkable water is a surface.
    const float overWater[3] = {700, 100, 100};
    REQUIRE(world.Ray(kRegion, overWater, down, 1000, hit));
    CHECK(hit.y == doctest::Approx(5.0f));

    // From a neighbouring region's coordinates: the hit comes back in the region it lies in.
    const float fromWest[3] = {520 + 1920, 200, 500};
    REQUIRE(world.Ray(NavRegionId(9, 10), fromWest, down, 1000, hit));
    CHECK(hit.region == kRegion);
    CHECK(hit.x == doctest::Approx(520.0f));
}

TEST_CASE("dungeon navigation: floors found through the grid, walks through doorways between blocks") {
    // Two floors 100 x 40 side by side, walled but for a doorway: block 0 centred on the origin
    // (doorway east), block 1 on x = 100 (doorway west).
    const std::uint8_t westFlags[4] = {kWall, nav_edge::kLink, kWall, kWall}; // doorway east
    const std::uint8_t eastFlags[4] = {kWall, kWall, kWall, nav_edge::kLink}; // doorway west
    const std::vector<std::shared_ptr<const NavObjectMesh>> floors = {
        NavObjectMesh::Build(Rectangle(-50, -20, 50, 20, 0, westFlags)),
        NavObjectMesh::Build(Rectangle(-50, -20, 50, 20, 0, eastFlags)),
    };
    assets::DungeonData data;
    data.regionId = 0x8001;
    data.boundsMin[0] = -100;
    data.boundsMin[1] = -50;
    data.boundsMin[2] = -100;
    data.boundsMax[0] = 300;
    data.boundsMax[1] = 150;
    data.boundsMax[2] = 100;
    data.gridSize[0] = 2;
    data.gridSize[1] = 1;
    data.gridSize[2] = 1;
    data.grid = {{0, {0, 1}}, {1, {1}}}; // x cells 0 (-100..100) and 1 (100..300)
    data.blocks.resize(2);
    data.blocks[0].neighbours = {1};
    data.blocks[1].position[0] = 100;
    data.blocks[1].neighbours = {0};
    auto build = [&] { return NavDungeon::Build(data, [&](std::uint32_t b) { return floors[b]; }); };
    auto dungeon = build();
    CHECK(dungeon->Doors() == 2);

    NavPosition a = dungeon->Drop(0, 5, 0);
    REQUIRE(a.Valid());
    CHECK(a.instance == 0);
    CHECK(a.region == 0x8001);
    CHECK(dungeon->Drop(120, 5, 0).instance == 1);
    CHECK_FALSE(dungeon->Drop(400, 5, 0).Valid()); // outside the grid
    CHECK_FALSE(dungeon->Drop(0, 5, 60).Valid());  // no floor there

    NavPosition to;
    CHECK(dungeon->Walk(a, 120, 0, to) == nav_walk::kLink);
    CHECK(to.instance == 1);
    CHECK(to.x == doctest::Approx(120.0f));
    CHECK(dungeon->Walk(a, 0, 40, to) == nav_walk::kBlocked); // a wall
    CHECK(to.z < 20.0f);
    CHECK(to.z > 19.0f);

    // Not neighbours: no doorway, the floor ends.
    data.blocks[0].neighbours.clear();
    data.blocks[1].neighbours.clear();
    dungeon = build();
    CHECK(dungeon->Doors() == 0);
    CHECK(dungeon->Walk(a, 120, 0, to) == nav_walk::kBlocked);
    CHECK(to.instance == 0);
    CHECK(to.x == doctest::Approx(49.8f).epsilon(0.001));
}
