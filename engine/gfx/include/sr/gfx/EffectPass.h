#pragma once

// Draws the frame's effects (gfx/Effects.h) over the finished scene:
// a pass of its own after the scene's (the opaque world, the water, the weather) and before the post
// pass, keeping the scene's colour and depth; depth tested, never written, no culling. Per blend
// (alpha, premultiplied, additive, in that order): the meshes, one instanced draw per mesh part,
// then its ribbons in one draw (their vertices built on the game thread, six a segment), then every sprite
// of the blend in one draw, expanded from its record in the vertex shader. So a frame costs at most 3
// sprite and 3 ribbon draws and one draw per mesh part in use, whatever the number of effects; textures
// are bindless. Render thread only.

#include <array>
#include <string>

#include <glm/glm.hpp>

#include "sr/gfx/Effects.h"
#include "sr/gfx/RenderGraph.h"
#include "sr/rhi/Rhi.h"

namespace sr::gfx {

class GeometryPool;

class EffectPass {
public:
    bool Init(rhi::Device& device, rhi::Format color, rhi::Format depth, std::string& error);
    void Shutdown();
    // The effects' brightness (ours, the option effectGlow: 1 the original's).
    void SetGlow(float glow) { glow_ = glow; }

    struct Frame {
        const EffectFrame* effects = nullptr; // must live until the graph ran
        rhi::TextureHandle color, depth;      // the scene's (depth a sampled ShadowMap target)
        glm::mat4 viewProj{1.0f};
    };
    // Adds "scene: effects" writing `color` (the scene's colour as the graph knows it), reading the
    // geometry pool (`geometry`); nothing when there is nothing to draw.
    void AddPasses(RenderGraph& graph, RenderGraph::Resource color, RenderGraph::Resource geometry, const GeometryPool& pool,
                   const Frame& frame);

private:
    void Draw(rhi::CommandList& cmd);

    rhi::Device* device_ = nullptr;
    std::array<rhi::PipelineHandle, kEffectBlends> sprites_{}, meshes_{}, ribbons_{};
    const GeometryPool* pool_ = nullptr;
    Frame frame_;
    float glow_ = 1.0f;
};

} // namespace sr::gfx
