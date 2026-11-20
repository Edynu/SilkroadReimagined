#include <algorithm>
#include <cmath>
#include <cstring>

#include "sr/gfx/Terrain.h"

namespace sr::gfx {

namespace {

constexpr int kSide = assets::kVerticesPerBlock; // 17
constexpr int kTiles = assets::kTilesPerBlock;   // 16
constexpr int kGrid = kTerrainGrid; // 97 samples across a region

// Octahedral encoding of a unit normal into two snorm16.
std::uint32_t PackNormal(float x, float y, float z) {
    const float l1 = std::abs(x) + std::abs(y) + std::abs(z);
    float u = x / l1, v = z / l1;
    if (y < 0.0f) {
        const float ou = (1.0f - std::abs(v)) * (u >= 0.0f ? 1.0f : -1.0f);
        const float ov = (1.0f - std::abs(u)) * (v >= 0.0f ? 1.0f : -1.0f);
        u = ou;
        v = ov;
    }
    const auto snorm = [](float f) {
        return static_cast<std::uint32_t>(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(std::clamp(f, -1.0f, 1.0f) * 32767.0f))));
    };
    return snorm(u) | (snorm(v) << 16);
}

TerrainTexture TextureOf(std::uint16_t word) {
    return {assets::TerrainBlock::TextureId(word), assets::TerrainBlock::TextureScale(word)};
}

} // namespace

bool BuildTerrain(const assets::TerrainRegion& region, TerrainBuild& out, std::string& error) {
    // The palette: every texture of the region, in the original's order (id, then scale).
    out.palette.clear();
    for (const assets::TerrainBlock& block : region.blocks) {
        for (std::uint16_t word : block.textures) out.palette.push_back(TextureOf(word));
    }
    std::sort(out.palette.begin(), out.palette.end());
    out.palette.erase(std::unique(out.palette.begin(), out.palette.end()), out.palette.end());
    if (out.palette.size() > kTerrainPaletteEntries) {
        error = "more than 256 terrain textures in one region";
        return false;
    }
    const auto paletteIndex = [&](std::uint16_t word) {
        return static_cast<std::uint8_t>(std::lower_bound(out.palette.begin(), out.palette.end(), TextureOf(word)) - out.palette.begin());
    };

    // Heights over the whole region (neighbouring blocks share their edge samples), for normals
    // that do not crease at block edges.
    std::vector<float> heights(kGrid * kGrid, 0.0f);
    for (int bz = 0; bz < assets::kBlocksPerRegion; ++bz) {
        for (int bx = 0; bx < assets::kBlocksPerRegion; ++bx) {
            const assets::TerrainBlock& block = region.blocks[static_cast<std::size_t>(bz * assets::kBlocksPerRegion + bx)];
            for (int z = 0; z < kSide; ++z) {
                for (int x = 0; x < kSide; ++x) {
                    heights[static_cast<std::size_t>((bz * kTiles + z) * kGrid + bx * kTiles + x)] =
                        block.heights[static_cast<std::size_t>(z * kSide + x)];
                }
            }
        }
    }
    const auto height = [&](int gx, int gz) {
        gx = std::clamp(gx, 0, kGrid - 1);
        gz = std::clamp(gz, 0, kGrid - 1);
        return heights[static_cast<std::size_t>(gz * kGrid + gx)];
    };

    out.heights = heights;
    out.samplesAndTiles.assign(static_cast<std::size_t>(kTerrainBytes - kTerrainSampleOffset), std::byte{0});
    std::byte* samples = out.samplesAndTiles.data();
    std::byte* tiles = samples + (kTerrainTileOffset - kTerrainSampleOffset);
    out.crowdedTiles = 0;

    for (int bz = 0; bz < assets::kBlocksPerRegion; ++bz) {
        for (int bx = 0; bx < assets::kBlocksPerRegion; ++bx) {
            const int b = bz * assets::kBlocksPerRegion + bx;
            const assets::TerrainBlock& block = region.blocks[static_cast<std::size_t>(b)];
            float lo = block.heights[0], hi = block.heights[0];

            std::array<std::uint8_t, kTerrainSamplesPerBlock> texture{};
            for (int s = 0; s < static_cast<int>(kTerrainSamplesPerBlock); ++s) {
                const int x = s % kSide, z = s / kSide;
                const int gx = bx * kTiles + x, gz = bz * kTiles + z;
                const float h = block.heights[static_cast<std::size_t>(s)];
                lo = std::min(lo, h);
                hi = std::max(hi, h);
                // Central differences; tiles are 20 units.
                const float nx = height(gx - 1, gz) - height(gx + 1, gz);
                const float nz = height(gx, gz - 1) - height(gx, gz + 1);
                const float ny = 2.0f * assets::kTileSize;
                const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                const std::uint32_t normal = PackNormal(nx / length, ny / length, nz / length);
                std::byte* out8 = samples + (static_cast<std::size_t>(b) * kTerrainSamplesPerBlock + static_cast<std::size_t>(s)) * kTerrainSampleStride;
                std::memcpy(out8, &h, 4);
                std::memcpy(out8 + 4, &normal, 4);
                texture[static_cast<std::size_t>(s)] = paletteIndex(block.textures[static_cast<std::size_t>(s)]);
            }
            out.blockHeights[static_cast<std::size_t>(b)] = {lo, hi};

            // Each tile is claimed by the lowest texture at its corners.
            const auto corner = [&](int tx, int tz, int c) {
                return texture[static_cast<std::size_t>((tz + (c >> 1)) * kSide + tx + (c & 1))];
            };
            std::array<std::uint8_t, kTerrainTilesPerBlock> claim{};
            for (int t = 0; t < static_cast<int>(kTerrainTilesPerBlock); ++t) {
                const int tx = t % kTiles, tz = t / kTiles;
                claim[static_cast<std::size_t>(t)] = std::min({corner(tx, tz, 0), corner(tx, tz, 1), corner(tx, tz, 2), corner(tx, tz, 3)});
            }
            // A texture's alpha at a sample: its own texture, and the claims of the tiles around it.
            const auto opaque = [&](int x, int z, std::uint8_t layer) {
                if (texture[static_cast<std::size_t>(z * kSide + x)] == layer) return true;
                for (int dz = -1; dz <= 0; ++dz) {
                    for (int dx = -1; dx <= 0; ++dx) {
                        const int tx = x + dx, tz = z + dz;
                        if (tx < 0 || tz < 0 || tx >= kTiles || tz >= kTiles) continue;
                        if (claim[static_cast<std::size_t>(tz * kTiles + tx)] == layer) return true;
                    }
                }
                return false;
            };

            for (int t = 0; t < static_cast<int>(kTerrainTilesPerBlock); ++t) {
                const int tx = t % kTiles, tz = t / kTiles;
                // Candidates: what is opaque at one of the corners (the corners' own textures, and
                // the claims of the tiles touching them).
                std::array<std::uint8_t, 40> candidates{};
                std::size_t count = 0;
                const auto add = [&](std::uint8_t layer) {
                    if (std::find(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(count), layer) ==
                        candidates.begin() + static_cast<std::ptrdiff_t>(count)) {
                        candidates[count++] = layer;
                    }
                };
                for (int c = 0; c < 4; ++c) add(corner(tx, tz, c));
                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = tx + dx, nz = tz + dz;
                        if (nx < 0 || nz < 0 || nx >= kTiles || nz >= kTiles) continue;
                        add(claim[static_cast<std::size_t>(nz * kTiles + nx)]);
                    }
                }
                std::sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(count));
                std::array<std::uint32_t, 40> layers{};
                std::uint32_t used = 0;
                for (std::size_t i = 0; i < count; ++i) {
                    std::uint32_t mask = 0;
                    for (int c = 0; c < 4; ++c) {
                        if (opaque(tx + (c & 1), tz + (c >> 1), candidates[i])) mask |= 1u << c;
                    }
                    if (mask == 0) continue;
                    // A layer opaque at all four corners covers the whole tile: what lies under it
                    // never shows (the tile's claim is such a layer, so every tile starts with one).
                    if (mask == 0xF) used = 0;
                    layers[used++] = candidates[i] | (mask << 8);
                }
                if (used > kTerrainTileLayers) {
                    ++out.crowdedTiles; // the topmost layers are dropped
                    used = kTerrainTileLayers;
                }
                std::byte* tile = tiles + (static_cast<std::size_t>(b) * kTerrainTilesPerBlock + static_cast<std::size_t>(t)) * kTerrainTileStride;
                std::memcpy(tile, &used, 4);
                std::memcpy(tile + 4, layers.data(), used * 4);
            }
        }
    }
    return true;
}

float TerrainHeight(const TerrainInfo& info, float x, float z) {
    if (info.heights.size() != static_cast<std::size_t>(kGrid * kGrid)) return 0.0f;
    const float gx = std::clamp(x / assets::kTileSize, 0.0f, static_cast<float>(kGrid - 1));
    const float gz = std::clamp(z / assets::kTileSize, 0.0f, static_cast<float>(kGrid - 1));
    const int tx = std::min(static_cast<int>(gx), kGrid - 2), tz = std::min(static_cast<int>(gz), kGrid - 2);
    const float fx = gx - static_cast<float>(tx), fz = gz - static_cast<float>(tz);
    const auto h = [&](int dx, int dz) { return info.heights[static_cast<std::size_t>((tz + dz) * kGrid + tx + dx)]; };
    const float h00 = h(0, 0), h10 = h(1, 0), h01 = h(0, 1), h11 = h(1, 1);
    if ((tx & 1) == (tz & 1)) {
        // Diagonal (x, z) - (x+1, z+1).
        return fz >= fx ? h00 + (h11 - h01) * fx + (h01 - h00) * fz : h00 + (h10 - h00) * fx + (h11 - h10) * fz;
    }
    // Diagonal (x, z+1) - (x+1, z).
    return fx + fz <= 1.0f ? h00 + (h10 - h00) * fx + (h01 - h00) * fz
                           : h11 + (h01 - h11) * (1.0f - fx) + (h10 - h11) * (1.0f - fz);
}

} // namespace sr::gfx
