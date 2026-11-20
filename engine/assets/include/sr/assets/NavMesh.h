#pragma once

// Navigation data of the outdoor world (Data.pk2 navmesh\):
//   navmesh\nv_xxxx.nvm  a region's walkable terrain and the object instances standing on it
//                        (xxxx = the region id (z << 8) | x in lower-case hex)
//   navmesh\mapinfo.mfo  which regions exist (one bit per region id)
//   navmesh\object.ifo   object id -> .bsr; the .bsr names the .bms whose object navmesh it walks on
// The object navmesh itself is decoded with the mesh (Model.h, DecodeBmsNavMesh).

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sr::assets {

constexpr int kNavTiles = 96;     // tiles per region side (20 units each)
constexpr int kNavHeights = 97;   // height samples per region side
constexpr int kNavBlocks = 6;     // water blocks per region side (320 units each)

// One region's .nvm, region-local coordinates (x and z in 0..1920).
struct RegionNavData {
    // An object instance on this region: the .o2 record (the same 30 bytes) and its links.
    // Neighbours' objects that overlap the region are repeated here with positions relative to
    // this region; (ownerRegion, uid) names an object across regions.
    struct Object {
        std::uint32_t objectId = 0; // navmesh\object.ifo
        float position[3]{};
        std::uint16_t field10 = 0;  // 0 or 0xFFFF; use not known
        float yaw = 0.0f;           // radians about +y: x' = x cos - z sin, z' = x sin + z cos
        std::uint16_t uid = 0;
        std::uint16_t field18 = 0, field1A = 0; // use not known
        std::uint16_t ownerRegion = 0;
        std::uint32_t firstLink = 0;
        std::uint16_t linkCount = 0;
    };
    // A door from one object to another: leaving `ownEdge` (an outline edge of the object that
    // lists the link) enters `object` (an index into `objects`, 0xFFFF none) through its outline
    // edge `objectEdge`.
    struct Link {
        std::uint16_t object = 0xFFFF;
        std::uint16_t objectEdge = 0;
        std::uint16_t ownEdge = 0;
    };
    // An axis-aligned rectangle of terrain; cells [0, openCells) are walkable, the rest blocked.
    struct Cell {
        float minX = 0, minZ = 0, maxX = 0, maxZ = 0;
        std::uint32_t firstObject = 0; // into cellObjects: the objects whose navmesh overlaps the cell
        std::uint8_t objectCount = 0;
    };
    // A piece of a cell's side. Sides: 0 = max z, 1 = max x, 2 = min z, 3 = min x.
    // Flags: 2 closed side (cell A only), 4 open internal edge, 8 open region border; the client
    // treats (flags & 3) as blocked.
    struct Edge {
        float ax = 0, az = 0, bx = 0, bz = 0; // axis-aligned, a < b
        std::uint8_t flags = 0;
        std::uint8_t sideA = 0, sideB = 0xFF;
        std::uint16_t cellA = 0, cellB = 0xFFFF; // border edges: cellB is in regionB
        std::uint16_t regionA = 0, regionB = 0;  // border edges only
    };
    struct Tile {
        std::uint16_t cell = 0;   // the cell holding the tile
        std::uint16_t flags = 0;  // bit 0: blocked (its cell is >= openCells); other bits unknown
        std::uint16_t tile2d = 0; // tile2d.ifo index
    };

    std::vector<Object> objects;
    std::vector<Link> links;
    std::vector<Cell> cells;
    std::vector<std::uint16_t> cellObjects;
    std::uint32_t openCells = 0;
    std::vector<Edge> borderEdges;
    std::vector<Edge> internalEdges;
    std::vector<Tile> tiles;      // kNavTiles^2, index tz * 96 + tx
    std::vector<float> heights;   // kNavHeights^2, index z * 97 + x (the .m heights)
    std::array<std::uint8_t, kNavBlocks * kNavBlocks> blockFlags{}; // bit 1: walkable water (.m water type 1)
    std::array<float, kNavBlocks * kNavBlocks> waterHeights{};      // the .m water heights
};

// "navmesh\nv_xxxx.nvm"
std::string RegionNavMeshFile(std::uint16_t regionId);
bool DecodeRegionNavMesh(std::span<const std::byte> file, RegionNavData& out, std::string& error);

// navmesh\mapinfo.mfo: "JMXVMFO 1000", u16 256, u16 128, 8 bytes, then 256 x 256 bits (one per
// region id, most significant bit first).
struct NavMapInfo {
    std::vector<std::uint8_t> bits; // 8192 bytes
    bool Enabled(std::uint16_t regionId) const {
        return regionId / 8u < bits.size() && ((bits[regionId / 8u] >> (7 - (regionId & 7))) & 1) != 0;
    }
    std::size_t Count() const;
};
bool DecodeNavMapInfo(std::span<const std::byte> file, NavMapInfo& out, std::string& error);

} // namespace sr::assets
