#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sr/assets/Terrain.h"
#include "sr/rhi/Rhi.h"

namespace sr::assets {
struct TextureData;
}

namespace sr::render {

class Frustum;
class TextureCache;

constexpr int kRegionBlocks = assets::kBlocksPerRegion * assets::kBlocksPerRegion; // 36

// A region's terrain and water built on the CPU (safe on a worker thread): vertices block by
// block (so each block can be culled), the ground textures as slots of `texturePaths` until
// TerrainRenderer::Upload turns them into bindless indices.
struct TerrainMesh {
    struct Vertex {
        float position[3];
        float normal[3];
        float barycentric[3];
        std::uint32_t textures[4]; // texture (slot, then bindless index) | scale code << 24, for the 3 corners
    };
    std::vector<Vertex> terrain;           // kBlockVertices per block, blocks z-major
    std::vector<Vertex> water;             // one quad (6 vertices) per block with water
    // Each block's water: its vertices [first, first + count).
    struct WaterRange {
        std::uint8_t block = 0;
        std::uint32_t first = 0, count = 0;
    };
    std::vector<WaterRange> waterBlocks;
    std::vector<std::string> texturePaths; // slot -> path ("" = not in tile2d.ifo)
    std::array<glm::vec2, kRegionBlocks> blockHeights{}; // lowest, highest terrain (or water) per block
    int unknownTextures = 0; // tile ids tile2d.ifo does not list
    static constexpr std::uint32_t kBlockVertices = assets::kTilesPerBlock * assets::kTilesPerBlock * 6;
};

struct GpuTerrain {
    rhi::BufferHandle terrain;
    std::uint32_t terrainVertices = 0;
    rhi::BufferHandle water;
    std::uint32_t waterVertices = 0;
    std::vector<TerrainMesh::WaterRange> waterBlocks;
    std::array<glm::vec2, kRegionBlocks> blockHeights{};
    rhi::TextureHandle lightmap; // the region's own (.t), released with it
    std::uint32_t lightmapIndex = 0;
    int missingTextures = 0; // tile ids whose texture could not be loaded
};

// Builds and draws one region's terrain and water. Geometry is in region-local
// coordinates (0..1920 on X and Z); pass the region's world offset to Draw().
class TerrainRenderer {
public:
    bool Init(rhi::Device& device, rhi::Format colorFormat, rhi::Format depthFormat, std::string& error);
    void Shutdown();

    static TerrainMesh Build(const assets::TerrainRegion& region, const assets::AssetIndex& tiles,
                             const std::string& waterTexture);
    // Loads the textures it names (through the cache: put them there first to keep this fast);
    // `lightmap` (optional) is the region's .t image.
    GpuTerrain Upload(TerrainMesh mesh, TextureCache& textures, const assets::TextureData* lightmap = nullptr);
    GpuTerrain Upload(const assets::TerrainRegion& region, const assets::AssetIndex& tiles, TextureCache& textures,
                      const std::string& waterTexture) {
        return Upload(Build(region, tiles, waterTexture), textures);
    }
    void Release(GpuTerrain& terrain);

    // `frustum` (optional, origin-relative like the camera) skips the blocks outside it.
    void DrawTerrain(rhi::CommandList& cmd, const GpuTerrain& terrain, const glm::mat4& viewProj,
                     const glm::vec3& offset, bool textured, bool wireframe, const Frustum* frustum = nullptr) const;
    // Draw after all opaque geometry (alpha blended, no depth writes).
    // `modern`: in its own pass after the scene, reading the scene behind it and the depth
    // pre-pass through the frame constants (refraction, depth colour, caustics, Fresnel, foam).
    // `frameTexture`: the original's animated water (water\\water1NN at 10 a second; bindless).
    void DrawWater(rhi::CommandList& cmd, const GpuTerrain& terrain, const glm::mat4& viewProj,
                   const glm::vec3& offset, float timeSeconds, const Frustum* frustum = nullptr, bool modern = false,
                   std::uint32_t frameTexture = 0) const;
    // Into a shadow map (depth only): the blocks inside `frustum` (the light's view).
    void DrawShadow(rhi::CommandList& cmd, const GpuTerrain& terrain, const glm::mat4& viewProj, const glm::vec3& offset,
                    const Frustum& frustum) const;

    // The frame's constants for the draws that follow (see ModelRenderer::SetFrame).
    void SetFrame(std::uint64_t address) const { frame_ = address; }

private:
    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle solid_, wire_, water_, waterModern_, shadow_, prepass_;
    mutable bool prepassMode_ = false;
public:
    // While on, DrawShadow draws the camera's depth pre-pass (no slope bias).
    void SetPrepass(bool on) const { prepassMode_ = on; }
private:
    void DrawBlocks(rhi::CommandList& cmd, const GpuTerrain& terrain, const glm::vec3& offset, const Frustum* frustum) const;
    mutable std::uint64_t frame_ = 0;
};

} // namespace sr::render
