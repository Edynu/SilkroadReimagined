#pragma once

// The original's glow and lens flare ("Glow and lens flare (C3g)"), after the scene:
// - glow: the scene downsampled into 512 x 512 at x 0.502, a bright pass (minus 0.157) fused into
//   a 5-tap horizontal blur, a 5-tap vertical blur (taps 0, +-2.6, +-5.2 texels; weights 0.314,
//   0.275, 0.196), then composited as 0.776 x scene + 0.753 x bloom;
// - lens flare: the sun's point and four neighbours tested against the depth buffer (a 1 x 1
//   target keeps the intensity, smoothed toward the score by min(10 dt, 1)), then 23 sprites
//   along the line from the sun through the screen centre, in pixels.
// Render thread only.

#include <array>
#include <cstdint>
#include <string>

#include <glm/glm.hpp>

#include "sr/gfx/RenderGraph.h"
#include "sr/rhi/Rhi.h"

namespace sr::gfx {

class PostPass {
public:
    // `sceneFormat`: the scene colour's (the composite and the flare write it).
    bool Init(rhi::Device& device, rhi::Format sceneFormat, std::string& error);
    void Shutdown();

    struct Frame {
        bool glow = false;
        bool flare = false;
        rhi::TextureHandle scene;   // the scene's colour (RGBA8, sampled after its pass)
        rhi::TextureHandle depth;   // the scene's depth (TextureUsage::ShadowMap)
        rhi::TextureHandle output;  // a target of the scene's size and format
        glm::mat4 viewProj{1.0f};
        glm::vec3 eye{0.0f};
        glm::vec3 sunDirection{0, 1, 0}; // towards the sun
        bool sunUp = false;              // the sun above the horizon (the original: y >= 700 of 20000)
        float viewDistance = 5500.0f;    // the probe's distance (the original's view distance)
        float seconds = 0.0f;            // since the last frame
        std::uint32_t width = 0, height = 0;
        std::array<std::uint32_t, 6> lens{}; // bindless sun\lens3 .. lens8
    };
    // Adds the passes after the scene's (which wrote `sceneResource`); returns the result to
    // composite, or `sceneResource` when neither glow nor flare is on.
    RenderGraph::Resource AddPasses(RenderGraph& graph, RenderGraph::Resource sceneResource, const Frame& frame);

private:
    void DrawFlare(rhi::CommandList& cmd, const Frame& frame, std::uint32_t intensity) const;

    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle down_, blurX_, blurY_, composite_, visibility_, flareAdd_, flareAlpha_;
    rhi::TextureHandle bloom_, blur_;                // 512 x 512
    std::array<rhi::TextureHandle, 2> intensity_{};   // 1 x 1, last frame's and this frame's
    unsigned current_ = 0;
    glm::vec2 axis_{0.0f};  // the smoothed sun-to-centre offset of the flare
    glm::vec2 sunScreen_{0.0f}; // the sun's point on screen, in pixels
    bool axisValid_ = false;
};

} // namespace sr::gfx
