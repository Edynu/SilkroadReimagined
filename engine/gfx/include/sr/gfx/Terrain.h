#pragma once

// A region's terrain on the GPU (C3c), built the way the original builds its
// blocks ("Terrain"): every height sample uses one texture; a tile shows, in
// ascending texture order (id, then scale), every texture whose alpha is non-zero at one of its
// corners, each blended over the ones before by its alpha bilinearly between the corners. A
// texture's alpha is 1 at a sample that uses it, and at the corners of every tile it claims (the
// lowest texture touching a tile claims it, so every tile starts opaque).
//
// One range of the geometry pool per region, read by address:
//   palette  256 x { u32 bindless texture, u32 scale code }          at 0
//   samples  36 blocks x 289 x { f32 height, u32 normal }           at kTerrainSampleOffset
//            (blocks z-major, samples z * 17 + x; the normal octahedral, snorm16 x 2)
//   tiles    36 blocks x 256 x { u32 count, 8 x u32 layer }          at kTerrainTileOffset
//            (layer: palette index in bits 0-7, corner alpha bits 8-11: (x, z), (x+1, z),
//            (x, z+1), (x+1, z+1); in drawing order)

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sr/assets/Terrain.h"
#include "sr/core/AssetTable.h"

namespace sr::gfx {

constexpr std::uint32_t kTerrainPaletteEntries = 256;
constexpr std::uint32_t kTerrainSampleStride = 8;
constexpr std::uint32_t kTerrainTileLayers = 8;
constexpr std::uint32_t kTerrainTileStride = 4 + kTerrainTileLayers * 4;
constexpr std::uint32_t kTerrainSamplesPerBlock = assets::kVerticesPerBlock * assets::kVerticesPerBlock;
constexpr std::uint32_t kTerrainTilesPerBlock = assets::kTilesPerBlock * assets::kTilesPerBlock;
constexpr std::uint32_t kTerrainBlocks = assets::kBlocksPerRegion * assets::kBlocksPerRegion;
constexpr std::uint64_t kTerrainSampleOffset = kTerrainPaletteEntries * 8;
constexpr std::uint64_t kTerrainTileOffset = kTerrainSampleOffset + std::uint64_t{kTerrainBlocks} * kTerrainSamplesPerBlock * kTerrainSampleStride;
constexpr std::uint64_t kTerrainBytes = kTerrainTileOffset + std::uint64_t{kTerrainBlocks} * kTerrainTilesPerBlock * kTerrainTileStride;

// A texture of the region: tile2d.ifo id and scale code (the sample's texture word, bits 10-12
// dropped, as the original keys it).
struct TerrainTexture {
    std::uint16_t id = 0;
    std::uint8_t scale = 0;
    friend auto operator<=>(const TerrainTexture&, const TerrainTexture&) = default;
};

// The CPU side of building a region: everything but the palette's texture indices.
constexpr int kTerrainGrid = assets::kBlocksPerRegion * assets::kTilesPerBlock + 1; // 97 samples across

struct TerrainBuild {
    std::vector<TerrainTexture> palette;         // ascending (the drawing order)
    std::vector<std::byte> samplesAndTiles;      // what goes at kTerrainSampleOffset
    std::array<std::array<float, 2>, kTerrainBlocks> blockHeights{}; // min, max per block
    std::vector<float> heights;                  // kTerrainGrid x kTerrainGrid, z-major
    std::uint32_t crowdedTiles = 0;              // tiles with more than kTerrainTileLayers textures
};
bool BuildTerrain(const assets::TerrainRegion& region, TerrainBuild& out, std::string& error);

// A block's water ("Water"): type -1 none, 0 water, 1 ice; coverage 0 = nothing shows.
struct TerrainWater {
    std::int8_t type = -1;
    std::uint8_t coverage = 0;
    float height = 0.0f;
    bool Visible() const { return type >= 0 && coverage != 0; }
};

// A resident region, published by the asset store.
struct TerrainInfo {
    std::uint16_t regionId = 0;                  // (y << 8) | x
    std::uint64_t offset = 0;                    // its range in the geometry pool
    std::array<std::array<float, 2>, kTerrainBlocks> blockHeights{};
    std::uint32_t lightmap = 0;                  // bindless index of the .t lightmap (0: none)
    std::vector<assets::ObjectPlacement> placements; // the region's objects (.o2)
    std::vector<float> heights;                  // kTerrainGrid x kTerrainGrid, z-major
    std::array<std::uint16_t, kTerrainBlocks> environments{}; // per block: its environment.ifo entry
    std::array<TerrainWater, kTerrainBlocks> water{};
    std::uint32_t crowdedTiles = 0;
};

// The terrain's height at (x, z) in the region (0..1920), on the triangles the terrain pass draws
// (the original's diagonal per tile); clamped to the region. Until the navmesh (C4) places things.
float TerrainHeight(const TerrainInfo& info, float x, float z);

} // namespace sr::gfx
