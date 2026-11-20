#pragma once

// Outdoor world data from Map.pk2. The world is a grid of regions, 1920 x 1920
// units each, stored as Map.pk2\<regionY>\<regionX>.{m,o2,t}. A region has
// 6 x 6 blocks of 16 x 16 tiles (20 units each), i.e. 17 x 17 height samples
// per block. X grows east, Z north, Y is up.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sr::assets {

constexpr float kRegionSize = 1920.0f;
constexpr int kBlocksPerRegion = 6;
constexpr int kTilesPerBlock = 16;
constexpr int kVerticesPerBlock = 17;
constexpr float kBlockSize = kRegionSize / kBlocksPerRegion; // 320
constexpr float kTileSize = kBlockSize / kTilesPerBlock;     // 20

struct TerrainBlock {
    std::uint32_t flags = 0;
    std::uint16_t environmentId = 0;
    // Per height sample (index z * 17 + x).
    std::array<float, kVerticesPerBlock * kVerticesPerBlock> heights{};
    // Bits 0-9: texture id (tile2d.ifo), bits 13-15: texture scale code.
    std::array<std::uint16_t, kVerticesPerBlock * kVerticesPerBlock> textures{};
    std::array<std::uint8_t, kVerticesPerBlock * kVerticesPerBlock> brightness{};
    std::int8_t waterType = -1;     // -1 none, 0 water, 1 ice ("Water")
    std::uint8_t waterCoverage = 0; // 0 no visible water, 1 all under, 2 shore (shallow), 3 shore (some deep)
    float waterHeight = 0.0f;       // world y of the surface
    std::array<std::uint16_t, kTilesPerBlock * kTilesPerBlock> tileFlags{};
    float heightMax = 0.0f;
    float heightMin = 0.0f;

    static std::uint16_t TextureId(std::uint16_t word) { return word & 0x03FF; }
    static std::uint8_t TextureScale(std::uint16_t word) { return static_cast<std::uint8_t>(word >> 13); }
};

// One region's height mesh (.m). Blocks are stored z-major: index bz * 6 + bx.
struct TerrainRegion {
    std::array<TerrainBlock, kBlocksPerRegion * kBlocksPerRegion> blocks;
};

struct ObjectPlacement {
    std::uint32_t objectId = 0; // object.ifo id
    float position[3]{};        // relative to the south-west corner of `regionId`
    float yaw = 0.0f;           // radians around Y; positive turns +X toward +Z
    std::uint16_t regionId = 0; // (y << 8) | x
    std::int16_t uid = 0;
    std::int16_t isStatic = 0;
    bool isBig = false;
    bool isStruct = false;
    std::uint8_t block = 0;    // bz * 6 + bx of the block that lists it
    std::uint8_t lodGroup = 0; // 0..3
};

// object.ifo / tile2d.ifo: id -> file.
struct IndexEntry {
    std::uint32_t flags = 0;
    std::string category; // tile2d.ifo only
    std::string path;
};
using AssetIndex = std::unordered_map<std::uint32_t, IndexEntry>;

constexpr std::uint16_t RegionId(int x, int y) { return static_cast<std::uint16_t>((y << 8) | x); }
std::string RegionFile(std::uint16_t regionId, std::string_view extension); // e.g. "105\78.m"

bool DecodeMapMesh(std::span<const std::byte> file, TerrainRegion& out, std::string& error);       // .m
bool DecodeMapObjects(std::span<const std::byte> file, std::vector<ObjectPlacement>& out, std::string& error); // .o2
struct TextureData;
// .t (JMXVMAPT1001): the region's lightmap, which the original multiplies over its terrain
// (baked light and shadows; it covers the region, u along x, v along z).
//   char[12] signature, 36 blocks x 256 tile light bytes (not used here),
//   u32 record length, u32 type, then a DDS image (to the end of the file)
bool DecodeMapLightmap(std::span<const std::byte> file, TextureData& out, std::string& error);
// object.ifo ("JMXVOBJI1000") and tile2d.ifo ("JMXV2DTI1001").
bool DecodeObjectIndex(std::span<const std::byte> file, AssetIndex& out, std::string& error);
bool DecodeTileIndex(std::span<const std::byte> file, AssetIndex& out, std::string& error);

// navmesh\objectstring.ifo ("JMXVOBJI1000", Data.pk2): named spots in the world (the fortresses'
// structure places STRUCTURE_POS_*, event gates). After the signature and
// the count, a line each:
//   0xID 0xFLAGS regionX regionZ 0xX 0xY 0xZ 0xYAW "NAME"
// the four floats written as their bits in hex (the place in the region, its height, the yaw in
// radians); ID's high word is the region when the spot is in the world (0xFFFF otherwise), FLAGS 0
// in every row of the 1.188 file.
struct NamedSpot {
    std::uint32_t id = 0, flags = 0;
    std::uint16_t region = 0; // RegionId(regionX, regionZ)
    float x = 0, y = 0, z = 0, yaw = 0;
    std::string name;
};
bool DecodeObjectStrings(std::span<const std::byte> file, std::vector<NamedSpot>& out, std::string& error);

} // namespace sr::assets
