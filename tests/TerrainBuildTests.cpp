// The terrain build (gfx/Terrain.h): every region of the game builds, every
// tile starts with an opaque layer and draws its layers in the original's order, and no tile
// needs more layers than the GPU layout holds. Needs SR_GAME_DIR.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>

#include "sr/assets/FileSystem.h"
#include "sr/assets/Terrain.h"
#include "sr/core/Path.h"
#include "sr/gfx/Terrain.h"

using namespace sr;

TEST_CASE("every region's terrain builds into the GPU layout") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, std::filesystem::path(dir), error), error);

    std::size_t regions = 0, failed = 0, badFirst = 0, unordered = 0, crowded = 0;
    std::size_t maxPalette = 0, maxBlockSet = 0;
    std::uint32_t maxLayers = 0;
    gfx::TerrainBuild build;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = core::NormalizeAssetPath(path);
        // Rows 0 and 1 are off-map developer regions in an older layout (see GameDataTests).
        if (!key.ends_with(".m") || key.starts_with("0\\") || key.starts_with("1\\")) continue;
        const auto data = fs.Read(path);
        assets::TerrainRegion region;
        if (!data || !assets::DecodeMapMesh(data->Bytes(), region, error) || !gfx::BuildTerrain(region, build, error)) {
            if (++failed <= 5) MESSAGE(path << ": " << error);
            continue;
        }
        ++regions;
        crowded += build.crowdedTiles;
        maxPalette = std::max(maxPalette, build.palette.size());
        for (const assets::TerrainBlock& block : region.blocks) {
            std::set<std::pair<int, int>> set; // the original's per-block set: at most 49
            for (std::uint16_t word : block.textures) set.insert({assets::TerrainBlock::TextureId(word), assets::TerrainBlock::TextureScale(word)});
            maxBlockSet = std::max(maxBlockSet, set.size());
        }
        const std::byte* tiles = build.samplesAndTiles.data() + (gfx::kTerrainTileOffset - gfx::kTerrainSampleOffset);
        for (std::uint32_t t = 0; t < gfx::kTerrainBlocks * gfx::kTerrainTilesPerBlock; ++t) {
            std::uint32_t record[1 + gfx::kTerrainTileLayers];
            std::memcpy(record, tiles + std::size_t{t} * gfx::kTerrainTileStride, sizeof(record));
            maxLayers = std::max(maxLayers, record[0]);
            if (record[0] == 0 || ((record[1] >> 8) & 0xF) != 0xF) ++badFirst;
            for (std::uint32_t i = 1; i < record[0]; ++i) {
                if ((record[i + 1] & 0xFF) <= (record[i] & 0xFF)) ++unordered;
            }
        }
    }
    MESSAGE(regions << " regions; palettes up to " << maxPalette << " textures, block sets up to " << maxBlockSet
                    << ", tiles up to " << maxLayers << " layers");
    CHECK(regions == 3437);
    CHECK(failed == 0);
    CHECK(badFirst == 0);  // every tile starts covered
    CHECK(unordered == 0); // ascending texture order, as the original draws
    CHECK(crowded == 0);   // the layout's 8 layers are enough for the whole game
    CHECK(maxBlockSet <= 49);
}

TEST_CASE("terrain heights follow the drawn triangles") {
    gfx::TerrainInfo info;
    constexpr int kGrid = gfx::kTerrainGrid;
    info.heights.assign(static_cast<std::size_t>(kGrid * kGrid), 0.0f);
    // A bump in one corner of tile (2, 2) (even parity: diagonal (x, z) - (x+1, z+1)) and of tile
    // (3, 2) (odd: diagonal (x, z+1) - (x+1, z)).
    const auto at = [&](int x, int z) -> float& { return info.heights[static_cast<std::size_t>(z * kGrid + x)]; };
    at(3, 2) = 10.0f; // corner (x+1, z) of tile (2, 2), corner (x, z) of tile (3, 2)
    const float t = assets::kTileSize;

    // At samples: the sample's height.
    CHECK(gfx::TerrainHeight(info, 3 * t, 2 * t) == doctest::Approx(10.0f));
    CHECK(gfx::TerrainHeight(info, 2 * t, 2 * t) == doctest::Approx(0.0f));
    // Tile (2, 2): the bump's corner (x+1, z) is off the diagonal, so the centre of the diagonal
    // stays 0, and the triangle holding (x+1, z) rises toward it.
    CHECK(gfx::TerrainHeight(info, 2.5f * t, 2.5f * t) == doctest::Approx(0.0f));
    CHECK(gfx::TerrainHeight(info, 2.75f * t, 2.25f * t) == doctest::Approx(5.0f));
    CHECK(gfx::TerrainHeight(info, 2.25f * t, 2.75f * t) == doctest::Approx(0.0f));
    // Tile (3, 2): diagonal (x, z+1) - (x+1, z); the bump at (x, z) lifts the triangle below it.
    CHECK(gfx::TerrainHeight(info, 3.5f * t, 2.5f * t) == doctest::Approx(0.0f));
    CHECK(gfx::TerrainHeight(info, 3.25f * t, 2.25f * t) == doctest::Approx(5.0f));
    // Continuous across the shared edge x = 3 tiles.
    CHECK(gfx::TerrainHeight(info, 3 * t - 0.001f, 2.4f * t) == doctest::Approx(gfx::TerrainHeight(info, 3 * t + 0.001f, 2.4f * t)).epsilon(0.001));
    // Clamped outside the region.
    CHECK(gfx::TerrainHeight(info, -100.0f, -100.0f) == doctest::Approx(0.0f));
    CHECK(gfx::TerrainHeight(info, 5000.0f, 5000.0f) == doctest::Approx(0.0f));
}
