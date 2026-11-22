#pragma once

// GPU-driven drawing of the world's instances: a compute pass culls every
// instance's parts against the frustum and builds one draw list per bucket; the draw pass issues
// one indirect-count draw per bucket. The CPU's cost does not depend on how many instances there
// are. Skinned instances (C3d): culling also lists a skinning job per visible part with bones and
// gives it room in a vertex arena; a compute pass skins those vertices with the frame's palette
// before the draws, which then read them like static vertices. Render thread only.

#include <cstdint>
#include <span>
#include <string>

#include <glm/glm.hpp>

#include "sr/gfx/GeometryPool.h"
#include "sr/gfx/GpuScene.h"
#include "sr/gfx/MeshTable.h"
#include "sr/gfx/RenderGraph.h"
#include "sr/rhi/Rhi.h"

namespace sr::gfx {

class WorldPass {
public:
    struct Limits {
        std::uint32_t maxDraws = 1u << 18;       // parts per bucket and frame
        std::uint32_t maxSkinJobs = 1u << 16;    // skinned parts per frame
        std::uint32_t arenaVertices = 4u << 20;  // skinned vertices per frame (32 bytes each)
        // The draws write depth only (the sun's shadow maps, G1): `color` is ignored, DrawDepth draws.
        bool depthOnly = false;
        // Views culled at once (the shadow cascades): lists for each, a skinned part skinned once.
        std::uint32_t views = 1;
    };
    static constexpr std::uint32_t kMaxViews = 4;
    // Draw lists and the skinning arena, for passes with these targets' formats.
    bool Init(rhi::Device& device, rhi::Format color, rhi::Format depth, const Limits& limits, std::string& error);
    void Shutdown();

    // The draw lists as graph resources: the draw pass reads `commands` and `counts` as indirect
    // arguments, `draws` and `arena` from the vertex shader.
    struct Lists {
        RenderGraph::Resource commands = 0, counts = 0, draws = 0, jobs = 0, arena = 0;
    };
    // Adds the clearing and the culling of `scene`'s instances. `instances` and `meshes` are the
    // graph's imports of the scene's and the mesh table's buffers (written before, if at all).
    // Without `skinning` (no palette this frame) skinned instances draw in their bind pose.
    // Instances whose origin lies below `waterLevel` go to the lists drawn before the water;
    // the rest, and every skinned one (the entities), after it ("Water").
    Lists AddCulling(RenderGraph& graph, RenderGraph::Resource instances, RenderGraph::Resource meshes,
                     const GpuScene& scene, const MeshTable& table, const glm::mat4& viewProj, bool skinning,
                     float waterLevel);
    // Several views at once (Limits::views of them at most): each its own lists. `minRadius`: per
    // view, parts smaller than it are left out; skinned instances go to the first `skinnedViews`, static ones to the
    // first `staticViews`, except their swaying parts (kMaterialFoliage), which go to the views of `foliageViews`' bits.
    Lists AddCulling(RenderGraph& graph, RenderGraph::Resource instances, RenderGraph::Resource meshes, const GpuScene& scene,
                     const MeshTable& table, std::span<const glm::mat4> views, bool skinning, float waterLevel,
                     std::span<const float> minRadius = {}, std::uint32_t skinnedViews = 4, std::uint32_t staticViews = 4,
                     std::uint32_t foliageViews = 0xFFFFFFFFu);
    // Adds the skinning of what the culling listed, with the matrices in `palette` (a buffer the
    // game wrote this frame; `paletteResource` its import).
    void AddSkinning(RenderGraph& graph, const Lists& lists, RenderGraph::Resource instances, RenderGraph::Resource meshes,
                     RenderGraph::Resource geometry, RenderGraph::Resource paletteResource, rhi::BufferHandle palette,
                     const GeometryPool& pool, const GpuScene& scene, const MeshTable& table);
    // Inside the graphics pass that read the lists: one indirect draw per bucket of `phase`.
    // `frame`: the FrameConstants' address. `parts`: the solid buckets, the translucent one
    // (kBucketBlended, last), or both; the Modern water draws the translucent parts after itself.
    enum class Phase { BeforeWater, AfterWater };
    enum class Parts { All, Solid, Translucent };
    void Draw(rhi::CommandList& cmd, const glm::mat4& viewProj, const GeometryPool& pool, const GpuScene& scene,
              const MeshTable& table, std::uint64_t frame, Phase phase, Parts parts = Parts::All) const;

    // A depth-only pass (Limits::depthOnly): `view`'s lists, both phases, from `viewProj`. `frame`:
    // the FrameConstants' address (the foliage's sway), 0 none.
    void DrawDepth(rhi::CommandList& cmd, const glm::mat4& viewProj, const GeometryPool& pool, const GpuScene& scene,
                   const MeshTable& table, std::uint64_t frame, std::uint32_t view = 0) const;

    // The modern water: parts of water material (kMaterialWater) culled into the translucent list,
    // drawn after the scene's copy so they reflect and refract it.
    void SetWaterBlended(bool on) { waterBlended_ = on; }
    // Levels of detail (ours): static parts smaller on screen than `minAngle` (radius / distance
    // from `eye`) are left out of the next cullings; 0: none.
    void SetSizeCulling(const glm::vec3& eye, float minAngle) {
        sizeCullEye_ = eye;
        sizeCullAngle_ = minAngle;
    }

    const Limits& GetLimits() const { return limits_; }

private:
    rhi::Device* device_ = nullptr;
    // The draws: back faces culled, two-sided, translucent (blended, no depth writes; none in a
    // depth-only pass, where translucent parts cast no shadow).
    rhi::PipelineHandle cull_, skin_, draw_, drawTwoSided_, drawBlended_;
    rhi::BufferHandle commands_, counts_, draws_, jobs_, arena_;
    Limits limits_;
    bool waterBlended_ = false;
    glm::vec3 sizeCullEye_{0.0f};
    float sizeCullAngle_ = 0.0f;
};

} // namespace sr::gfx
