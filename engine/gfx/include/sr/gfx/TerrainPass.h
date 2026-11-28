#pragma once

// The outdoor terrain (C3c; drawn as the original's "Terrain"):
// every visible block of every resident region in one draw. Vertices are pulled from the
// region's range of the geometry pool (Terrain.h); the pixel shader blends the tile's textures in
// the original's order, each by its alpha bilinear between the tile's corners, lit by the vertex
// colour times two, then multiplies by the region's lightmap plus a lift. Render thread only.
//
// Lighting and fog come from the frame's constants (FrameConstants.h): the sun light's direction,
// diffuse and ambient, and the lightmap's lift. The far blocks' flat colour comes with C3e.
//
// Blob shadows (C3d): the original's decal. For each shadow the
// terrain's tiles under a square of its size are drawn again, at the terrain's own vertices, with
// the shadow texture across the square (clamped), black at the original's alpha, blended over.
//
// Water (C3f): the original's plain water. One quad per visible block with
// water, at its height: the animated frame times the environment's water colour, its alpha the
// depth below the surface in 2-unit steps (full at 30 units) interpolated over the block; ice an
// untinted texture at a constant alpha.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sr/gfx/FrameConstants.h"
#include "sr/gfx/GeometryPool.h"
#include "sr/gfx/Terrain.h"
#include "sr/rhi/Rhi.h"

namespace sr::gfx {

class TerrainPass {
public:
    // Room for `maxBlocks` visible blocks a frame (36 a region).
    bool Init(rhi::Device& device, rhi::Format color, rhi::Format depth, std::uint32_t maxBlocks, std::string& error);
    void Shutdown();

    struct Region {
        const TerrainInfo* info = nullptr; // kept alive by the caller until the frame is recorded
        glm::vec3 offset{0.0f};            // the region's south-west corner in world space
    };
    // Picks the blocks inside the frustum (their boxes from the blocks' height ranges and water)
    // and the water level: the height of the visible water nearest to `eye` (-1e38: none).
    // Regions must lie within 16 regions of the world origin (the shadows' lookup table).
    // `lodDistances`: the view distances (flat) past which a block drops to half, a quarter and an
    // eighth of its detail (ours; all zero: full detail everywhere).
    void Prepare(std::span<const Region> regions, const glm::mat4& viewProj, const glm::vec3& eye,
                 const std::array<float, 3>& lodDistances = {});
    float WaterLevel() const { return waterLevel_; }
    // Inside the graphics pass that reads the geometry pool; `frame` the FrameConstants' address.
    void Draw(rhi::CommandList& cmd, const glm::mat4& viewProj, const GeometryPool& pool, std::uint64_t frame) const;

    // A blob shadow: centre (on the ground) and the square's side, in world units.
    struct Shadow {
        float x = 0, y = 0, z = 0, size = 0;
    };
    static constexpr float kMaxShadowSize = 140.0f; // 8 tiles across, the pass's fixed grid
    // After Draw, in the same pass: `texture` is the shadow's bindless index.
    void DrawShadows(rhi::CommandList& cmd, const glm::mat4& viewProj, std::uint64_t frame, const GeometryPool& pool,
                     std::span<const Shadow> shadows, std::uint32_t texture) const;

    // After the objects below the water level: the water. `water` the bindless index of this
    // moment's frame of the animation, `ice` the ice texture's.
    // The original's reflective water (its reflection row): the mirrored scene's target and the bump map
    // (skybox\waterbump3.ddj) its samples are bent by; the screen's size. texture 0: the plain water.
    struct WaterReflection {
        std::uint32_t texture = 0, bump = 0;
        float width = 0, height = 0;
    };
    void DrawWater(rhi::CommandList& cmd, const glm::mat4& viewProj, std::uint64_t frame, const GeometryPool& pool,
                   std::uint32_t water, std::uint32_t ice, bool modern = false, const WaterReflection* reflection = nullptr,
                   int detail = 1) const; // detail: the Video page's Water Detail (0 low, 1 medium, 2 high)

    std::uint32_t VisibleBlocks() const { return static_cast<std::uint32_t>(blocks_.size()); }
    std::uint32_t VisibleWater() const { return static_cast<std::uint32_t>(water_.size()); }

private:
    struct Block {
        std::uint64_t region = 0; // offset of the region's range in the pool
        std::uint32_t block = 0;
        std::uint32_t lightmap = 0;
        float offset[3]{};
        std::uint32_t neighbours = 0; // the detail shifts of its west, east, south, north neighbours
    };
    static_assert(sizeof(Block) == 32);
    std::array<std::uint32_t, 4> lodCounts_{}; // blocks_ sorted by detail: how many of each
    std::vector<std::uint8_t> lodShifts_;      // each block's detail, while preparing
    std::vector<std::uint32_t> lodOrder_;
    std::vector<Block> lodSorted_;

    struct WaterBlock {
        std::uint64_t region = 0; // offset of the region's range in the pool
        std::uint32_t block = 0;
        std::uint32_t type = 0;   // 0 water, 1 ice
        float offset[3]{};
        float height = 0;
    };
    static_assert(sizeof(WaterBlock) == 32);

    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle pipeline_, shadow_, water_pipeline_;
    rhi::PipelineHandle waterModern_; // the Modern look's water (G2)
    rhi::PipelineHandle iceModern_;   // and its ice
    std::size_t iceFirst_ = 0;        // water_'s first ice block
    std::uint32_t maxBlocks_ = 0;
    std::vector<Block> blocks_;      // this frame's visible blocks (capacity reserved at Init)
    std::vector<WaterBlock> water_;  // and those with water
    float waterLevel_ = -1e38f;
    // Regions by position for the shadows: (x + 16) + (z + 16) * 32 in regions from the world
    // origin -> offset in the pool + 1 (0: none).
    static constexpr int kTableSide = 32;
    std::array<std::uint64_t, kTableSide * kTableSide> regionTable_{};
};

} // namespace sr::gfx
