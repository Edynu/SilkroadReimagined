#pragma once

// Navigation: where the ground is and
// how a walker moves over it. The outdoor world is a grid of 1920 x 1920 regions; each has a
// navmesh of axis-aligned terrain cells joined by edges (its .nvm), a height grid and the object
// instances standing on it (bridges, decks, stairs, floors), each walking on its object's
// navmesh of triangles. Positions are region-local: a region id ((z << 8) | x) and x, z in
// 0..1920, y up.
//
// Everything here is immutable once built: a region's navigation is replaced, never edited, so
// the queries run from any thread while the world is not being changed.

#include <unordered_set>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sr/assets/Model.h"
#include "sr/assets/NavMesh.h"

namespace sr::game {

class NavDungeon;

constexpr float kNavRegionSize = 1920.0f;
constexpr float kNavTileSize = 20.0f;

constexpr int NavRegionX(std::uint16_t region) { return region & 0xFF; }
constexpr int NavRegionZ(std::uint16_t region) { return region >> 8; }
constexpr std::uint16_t NavRegionId(int x, int z) { return static_cast<std::uint16_t>((z << 8) | x); }
// A dungeon's region (bit 15 set): its own space, no region grid.
constexpr bool NavIsDungeon(std::uint16_t region) { return (region & 0x8000) != 0; }

// An object's walkable surface in its own space, shared by every instance of the object.
struct NavObjectMesh {
    struct Vertex {
        float x = 0, y = 0, z = 0;
        float nx = 0, nz = 0; // outline vertices: the outward 2D normal
    };
    struct Triangle {
        std::uint16_t v[3]{};
        float plane[4]{};        // a x + b y + c z + d = 0, (a, b, c) of unit length
        float cx = 0, cy = 0, cz = 0; // centroid
        std::uint32_t firstEdge = 0;  // into triangleEdges
        std::uint8_t edgeCount = 0;
        std::uint8_t event = 0;
        float HeightAt(float x, float z) const;
    };
    // Outline edges come first ([0, outlineCount)), then the internal ones.
    struct Edge {
        std::uint16_t a = 0, b = 0;
        std::uint16_t cellA = 0, cellB = 0xFFFF;
        std::uint8_t flags = 0;
        std::uint8_t event = 0;
    };
    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;
    std::vector<Edge> edges;
    std::uint32_t outlineCount = 0;
    std::vector<std::uint32_t> triangleEdges;
    assets::ObjectNavMesh::Grid grid;
    std::vector<std::string> events;
    bool marker = false; // an event zone walked as terrain (ObjectNavMesh::IsEventMarker)
    float min[3]{}, max[3]{};

    bool IsOutline(std::uint32_t edge) const { return edge < outlineCount; }
    static std::shared_ptr<const NavObjectMesh> Build(const assets::ObjectNavMesh& mesh);
};

// Edge flag bits (assets::ObjectNavMesh::Edge for objects, assets::RegionNavData::Edge for terrain).
namespace nav_edge {
constexpr std::uint8_t kBlockBToA = 0x01; // internal; on the outline: a wall from outside
constexpr std::uint8_t kBlockAToB = 0x02; // internal; on the outline: blocks leaving; terrain: closed side
constexpr std::uint8_t kInternal = 0x04;
constexpr std::uint8_t kLink = 0x08;      // outline: a door to another object; terrain: region border
constexpr std::uint8_t kLedge = 0x10;     // outline: not an entry from outside, blocks leaving
} // namespace nav_edge

// One region's navigation.
struct NavRegion {
    struct Instance {
        std::shared_ptr<const NavObjectMesh> mesh; // null when the object has no navmesh
        float x = 0, y = 0, z = 0;                 // region-local
        float cos = 1, sin = 0;                    // of the yaw
        std::uint32_t key = 0;                     // owner region << 16 | uid
        float minX = 0, minZ = 0, maxX = 0, maxZ = 0, minY = 0, maxY = 0; // region-local bounds
        // Object space <-> region space.
        void ToLocal(float wx, float wz, float& lx, float& lz) const;
        void ToWorld(float lx, float lz, float& wx, float& wz) const;
        bool Walkable() const { return mesh && !mesh->triangles.empty(); }
    };
    std::uint16_t id = 0;
    assets::RegionNavData data;
    std::vector<Instance> instances; // parallel to data.objects
    // Per cell and side (cell * 4 + side): the edges on that side, internal edges as their index,
    // border edges as kBorderEdge | index.
    static constexpr std::uint32_t kBorderEdge = 0x80000000u;
    std::vector<std::uint32_t> sideStart; // cells * 4 + 1
    std::vector<std::uint32_t> sideEdges;

    const assets::RegionNavData::Edge& EdgeAt(std::uint32_t ref) const {
        return (ref & kBorderEdge) ? data.borderEdges[ref & ~kBorderEdge] : data.internalEdges[ref];
    }
    const assets::RegionNavData::Tile& TileAt(float x, float z) const;
    // `meshFor(objectId)` gives each instance its object's navmesh (or null).
    template <typename MeshFor>
    static std::shared_ptr<const NavRegion> Build(std::uint16_t id, assets::RegionNavData&& data, MeshFor&& meshFor) {
        auto region = std::make_shared<NavRegion>();
        region->id = id;
        region->data = std::move(data);
        region->instances.resize(region->data.objects.size());
        for (std::size_t i = 0; i < region->instances.size(); ++i) {
            region->instances[i].mesh = meshFor(region->data.objects[i].objectId);
        }
        region->Link();
        return region;
    }

private:
    void Link(); // instances' transforms and bounds, the cells' side lists
};

// The terrain height under (x, z) (0..1920): the height grid split into two triangles per tile
// with the diagonal alternating like a checkerboard, raised to the water on walkable water.
float NavTerrainHeight(const NavRegion& region, float x, float z);

struct NavPosition {
    std::uint16_t region = 0;
    float x = 0, y = 0, z = 0;
    std::int32_t cell = -1;     // terrain cell, or the object triangle when on an instance; -1 invalid
    std::int32_t instance = -1; // the instance (in region's list) walked on; -1 the terrain
    bool Valid() const { return cell >= 0; }
};

// Bits of a walk's result (the client's own).
namespace nav_walk {
constexpr std::uint32_t kReached = 0;
constexpr std::uint32_t kBlocked = 0x1;     // stopped at a blocked edge (or a missing region)
constexpr std::uint32_t kLink = 0x2;        // went through a door to another object
constexpr std::uint32_t kOnObject = 0x4;    // ended on an object entered from the terrain
constexpr std::uint32_t kLeftObject = 0x10; // left an object onto the terrain
constexpr std::uint32_t kLimit = 0x20;      // crossed as many cells as allowed
constexpr std::uint32_t kFailed = 0x10000000;
} // namespace nav_walk

// Event zones a walk enters: an object's or a dungeon floor's
// cells whose cell byte has bits 6-7 set (0xC0 | the name's index: the tomb's gates, its entrance door),
// entered from a cell outside that zone, and internal edges whose edge byte has them set (0x40 | the
// name: Donhwang's cave mouth and its entrance room), crossed. The name is the mesh's, as written there
// (lower case in the files); each zone once per walk.
void NavZonesEntered(const NavObjectMesh& mesh, std::uint32_t edge, std::uint32_t from, std::uint32_t into, std::vector<std::string>& zones);
void NavZoneOfCell(const NavObjectMesh& mesh, std::uint32_t cell, std::vector<std::string>& zones);

struct NavHit {
    std::uint16_t region = 0;
    float x = 0, y = 0, z = 0; // region-local
    float distance = 0;
    std::int32_t instance = -1; // -1 the terrain
};

// The regions the game holds and the queries over them. Changed only by the thread that owns it
// (the game thread, between phases); the queries are const and lock-free.
class NavWorld {
public:
    void Add(std::shared_ptr<const NavRegion> region);
    // A dungeon: its region's queries go to it (Drop, Walk, Ray; it has no terrain).
    void AddDungeon(std::shared_ptr<const NavDungeon> dungeon);
    void Remove(std::uint16_t region); // a region or a dungeon
    void Clear() {
        regions_.clear();
        dungeons_.clear();
    }
    const NavRegion* Find(std::uint16_t region) const;
    const NavDungeon* FindDungeon(std::uint16_t region) const;
    // Whether that region's navigation is held (an outdoor region or a dungeon).
    bool Holds(std::uint16_t region) const { return Find(region) != nullptr || FindDungeon(region) != nullptr; }
    std::size_t Size() const { return regions_.size() + dungeons_.size(); }

    // The terrain height (x, z wrap into the neighbouring regions); false if that region is not held.
    bool TerrainHeight(std::uint16_t region, float x, float z, float& y) const;
    // The surface under (x, z) nearest in height to y: the terrain or an object's triangle there.
    // Invalid when no region is held there, or the terrain wins on a blocked tile.
    NavPosition Drop(std::uint16_t region, float x, float y, float z) const;
    // Walks from `from` towards (x, z) of region `toRegion`: over terrain cells through their open
    // edges, onto objects through their outline and across them triangle by triangle, into
    // neighbouring regions. `to` is where the walker ends (where it was stopped when blocked).
    // In a dungeon the target must be in the same dungeon. `zones`: the event zones the walk enters, in
    // order (NavZonesEntered). `layer`: the instance the walker is in (its doors; 0 the shared world).
    std::uint32_t Walk(const NavPosition& from, std::uint16_t toRegion, float x, float z, NavPosition& to,
                       std::uint32_t maxCrossings = 100000, std::vector<std::string>* zones = nullptr, std::uint32_t layer = 0) const;
    // An event zone in the way (a fortress's gate, tower, heart): the walk
    // stops before entering it while it blocks. Jangan's are objects of their own, known by their key
    // (objectstring.ifo's id); the others' are cells of the fortress's big object, known by the zone's name in
    // lower case. Only between walks (the world's thread, never during a tick's jobs). `layer`: the instance whose
    // walks it stops (a Forgotten World door; 0 the shared world's).
    void SetZoneBlocks(std::uint32_t key, const std::string& name, bool blocks, std::uint32_t layer = 0) {
        const std::uint64_t k = (std::uint64_t(layer) << 32) | key;
        if (blocks) {
            blockingZones_.insert(k);
            blockingNames_[layer].insert(name);
        } else {
            blockingZones_.erase(k);
            if (const auto it = blockingNames_.find(layer); it != blockingNames_.end()) {
                it->second.erase(name);
                if (it->second.empty()) blockingNames_.erase(it);
            }
        }
    }
    bool ZoneBlocks(std::uint32_t key, std::uint32_t layer = 0) const { return blockingZones_.contains((std::uint64_t(layer) << 32) | key); }
    // The nearest surface the ray meets within `maxLength` (the client picks with 1000), among the
    // 3 x 3 regions around `region`: the terrain as walked (with walkable water) and the instances'
    // triangles. `origin` is relative to `region`, `direction` of unit length. In a dungeon: its floors.
    bool Ray(std::uint16_t region, const float origin[3], const float direction[3], float maxLength, NavHit& hit) const;

private:
    std::unordered_set<std::uint64_t> blockingZones_; // the placed objects standing in the way (SetZoneBlocks): layer << 32 | key
    std::unordered_map<std::uint32_t, std::unordered_set<std::string>> blockingNames_; // the event cells' zones standing in the way, by layer
    const std::unordered_set<std::string>& NamesBlocking(std::uint32_t layer) const;
    std::uint32_t TerrainLeg(NavPosition& at, float& tx, float& tz, std::int32_t ignore, std::uint32_t& crossings,
                             std::vector<std::string>* zones, std::uint32_t layer) const;
    std::uint32_t ObjectLeg(NavPosition& at, float& tx, float& tz, std::uint32_t& crossings, std::int32_t& left,
                            std::vector<std::string>* zones, std::uint32_t layer) const;
    bool Settle(NavPosition& at, float& tx, float& tz) const; // wraps x/z into the region, rebasing the target

    std::unordered_map<std::uint16_t, std::shared_ptr<const NavRegion>> regions_;
    std::unordered_map<std::uint16_t, std::shared_ptr<const NavDungeon>> dungeons_;
};

} // namespace sr::game
