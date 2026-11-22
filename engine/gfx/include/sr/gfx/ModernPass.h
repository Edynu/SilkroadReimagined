#pragma once

// The frame's last steps over the HDR scene (G0 / G3 / G4): the
// Classic look's is a clamp and the brightness into the 8-bit picture; the Modern rows add, each at
// its own cost and resolution:
// - ambient occlusion from the depth (half resolution, 8 taps, a depth-aware blur; darkens the scene);
// - sun shafts: the sky's light along the line to the sun (quarter resolution, 32 taps), only while
//   the sun is near the screen;
// - bloom: a chain of five halvings (13-tap filter, a soft threshold) and back up (a tent);
// - tone mapping (ACES, AgX), grading by the time of day, a vignette, then FXAA.
// Render thread only.

#include <array>
#include <cstdint>
#include <string>

#include <glm/glm.hpp>

#include "sr/gfx/RenderGraph.h"
#include "sr/rhi/Rhi.h"

namespace sr::gfx {

class ModernPass {
public:
    bool Init(rhi::Device& device, std::string& error);
    void Shutdown();

    struct Settings {
        int toneMapping = 0;      // 0 clamp (Classic), 1 ACES, 2 AgX
        float gamma = 1.0f;       // the brightness (1 normal)
        bool grading = false;
        int occlusion = 0;        // 0 off, 1 low, 2 high
        bool bloom = false;       // the modern bloom (the original's glow is PostPass's)
        bool shafts = false;
        bool fxaa = false;
        bool vignette = false;
        int heightFog = 0;        // 0 off, 1 low, 2 high: fog thicker near the ground, lit by the sun
    };
    struct Frame {
        Settings settings;
        rhi::TextureHandle scene;  // HDR colour (sampled after its passes)
        rhi::TextureHandle depth;  // the scene's depth (TextureUsage::ShadowMap)
        rhi::TextureHandle output; // the 8-bit picture (RGBA8, the scene's size)
        glm::mat4 viewProj{1.0f};
        glm::vec3 eye{0.0f};
        glm::vec3 sunDirection{0, 1, 0}; // towards the sun
        glm::vec3 sunColour{1.0f};
        glm::vec3 fogColour{0.6f, 0.7f, 0.8f}; // the environment's (the height fog's)
        float sunWeight = 0.0f; // the sun's part in the shafts and the fog's light: 0 under the horizon, 1 well above it
        float timeOfDay = 0.5f;
        std::uint32_t width = 0, height = 0;
    };
    // Adds its passes after the scene's (`sceneResource` the HDR colour, `depthResource` its depth);
    // returns the 8-bit picture.
    RenderGraph::Resource AddPasses(RenderGraph& graph, RenderGraph::Resource sceneResource, RenderGraph::Resource depthResource,
                                    const Frame& frame);

    // Inside a recorded pass (outside a render pass): the scene's colour and its view depth into
    // `target` (RGBA16F, the scene's size) for the modern water.
    void CopyForWater(rhi::CommandList& cmd, rhi::TextureHandle scene, rhi::TextureHandle depth, rhi::TextureHandle target,
                      const glm::mat4& viewProj) const;

    // Inside a depth-only pass whose target was cleared to 1: `source` (a depth texture of the
    // target's size, sampled) written into the target's depth.
    void CopyDepth(rhi::CommandList& cmd, rhi::TextureHandle source) const;

private:
    void EnsureTargets(std::uint32_t width, std::uint32_t height);
    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle hdr_, ldr_, additive_, depthCopy_;
    rhi::TextureHandle occlusion_, occlusionBlur_, shafts_, fxaaSource_;
    static constexpr int kBloomLevels = 5;
    std::array<rhi::TextureHandle, kBloomLevels> bloom_{};
    std::uint32_t width_ = 0, height_ = 0;
};

} // namespace sr::gfx
