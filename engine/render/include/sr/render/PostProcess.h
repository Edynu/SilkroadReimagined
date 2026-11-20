#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include <glm/glm.hpp>

#include "sr/rhi/Rhi.h"

namespace sr::render {

class TextureCache;

// Full-screen passes (one triangle, no vertex buffer, shaders/Post.hlsl).
//
// Ambient occlusion (SSAO, before the scene pass): from a sampled depth pre-pass, in world
// space (normals from the neighbours' positions), Low at half resolution with 8 samples,
// High at full resolution with 16, then a depth-aware blur across and down.
class AmbientOcclusion {
public:
    bool Init(rhi::Device& device, std::string& error);
    void Shutdown();
    // Makes the targets for a width x height scene at `quality` (1 low, 2 high); result() is
    // then the texture Render fills.
    void Ensure(std::uint32_t width, std::uint32_t height, int quality);
    // Renders the occlusion of `depth` (width x height) at `quality` (1 low, 2 high); the
    // result (r: 1 open, 0 occluded) is result(). The camera block is viewProj, its inverse,
    // the camera's position and forward direction; `nearPlane` / `farPlane` the projection's.
    void Render(rhi::CommandList& cmd, rhi::TextureHandle depth, std::uint32_t width, std::uint32_t height, int quality,
                const glm::mat4& viewProj, const glm::vec3& camera, const glm::vec3& forward, float nearPlane, float farPlane);
    rhi::TextureHandle result() const { return a_; }

private:
    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle ssao_, blur_;
    rhi::TextureHandle a_, b_;
    std::uint32_t width_ = 0, height_ = 0;
};

// Copies of a finished image or a sampled depth (modern water reads the scene behind it
// and needs a depth target it may write while it samples the pre-pass).
class SceneCopy {
public:
    bool Init(rhi::Device& device, rhi::Format colorFormat, std::string& error);
    void Shutdown();
    void Color(rhi::CommandList& cmd, rhi::TextureHandle source, rhi::TextureHandle target, std::uint32_t width,
               std::uint32_t height) const;
    // `target` a ShadowMap (D32) texture: cleared, then the source's depth written in.
    void Depth(rhi::CommandList& cmd, rhi::TextureHandle source, rhi::TextureHandle target, std::uint32_t width,
               std::uint32_t height) const;

private:
    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle color_, depth_;
};

// What the image after the scene goes through, from the Option window and the sky.
struct PostSettings {
    int toneMapping = 0; // 0 the original's (clamp), 1 ACES, 2 AgX
    int bloom = 0;       // 0 off, 1 the original's 512 x 512 glow, 2 modern (dual filter)
    int lensFlare = 0;   // 0 off, 1 the original's 30 sprites, 2 modern (+ light shafts)
    bool grading = false;
    bool fxaa = false;
    int volumetricFog = 0;     // 0 off, 1 low (8 steps), 2 high (24)
    bool volumetricClouds = false;
    float weatherPresence = 0.0f; // rain or snow (thicker fog and cloud cover)
    std::uint64_t frame = 0;   // the scene's FrameConstants this frame (volumetrics)
    float timeOfDay = 0.5f;
    float seconds = 0.0f; // a running clock (the clouds' wind)
    float deltaSeconds = 0.0f;
    // The camera and the depth pre-pass (flare visibility, shafts); null without one.
    glm::mat4 viewProj{1.0f};
    glm::vec3 camera{0.0f};
    rhi::TextureHandle depth;
    // Without a depth pre-pass: whether the scene blocks the line from the camera to a point
    // (the flare's probes); null: nothing does.
    std::function<bool(const glm::vec3&)> blocked;
    bool Any() const { return toneMapping != 0 || bloom != 0 || lensFlare != 0 || grading || fxaa || volumetricFog != 0 || volumetricClouds; }
};

// After the scene: bloom, light shafts, tone mapping and colour grading into an 8-bit image,
// the original's lens flare over it, then FXAA.
//
// The original's bloom: the scene into a 512 x 512
// target at 128/255, a blur across with a bright pass (each tap less 40/255; five taps 2.6
// texels apart, weights 80, 70, 50 /255), the same blur down without it, then the scene at
// 198/255 under the glow at 192/255.
// The original's lens flare: the sun on its arc ((time - 0.25) * 2 pi, 20000 out) seen 3500
// units out; 30 sprites (sun\lens1..8, 23 drawn) along the axis from the sun to 2.2 x the way
// to the screen centre, each sprite's alpha byte times the visibility; the visibility is five
// points (the sun and 10 * width / 1024 units to each side on X and Z, weights 0.3 and 0.175)
// tested against the depth, eased by min(dt * 10, 1) a frame; nothing while it is at most
// 0.1 or the sun is below 700 units, or off screen by more than a tenth.
class PostProcess {
public:
    bool Init(rhi::Device& device, rhi::Format displayFormat, std::string& error);
    void Shutdown();
    // Makes the targets for a width x height image and returns the texture Run will leave
    // the result in (null: nothing is on, show the scene). Call before the image is used.
    rhi::TextureHandle Prepare(std::uint32_t width, std::uint32_t height, const PostSettings& settings);
    // Runs the chain over `scene` (width x height, sampled) and returns what to show: the
    // scene itself when nothing is on.
    rhi::TextureHandle Run(rhi::CommandList& cmd, rhi::TextureHandle scene, std::uint32_t width, std::uint32_t height,
                           const PostSettings& settings, TextureCache& textures);
    // Only FXAA, as before the chain (tools).
    void Fxaa(rhi::CommandList& cmd, rhi::TextureHandle source, rhi::TextureHandle target, std::uint32_t width,
              std::uint32_t height) const;

private:
    void Ensure(std::uint32_t width, std::uint32_t height);
    // The sun on screen and the flare's visibility pass; then its sprites over the image.
    void FlareVisibility(rhi::CommandList& cmd, const PostSettings& settings, std::uint32_t width, std::uint32_t height);
    void FlareSprites(rhi::CommandList& cmd, TextureCache& textures, std::uint32_t width, std::uint32_t height);

    rhi::Device* device_ = nullptr;
    rhi::Format displayFormat_ = rhi::Format::RGBA8Unorm;
    rhi::PipelineHandle fxaa_, compose_, bloomDown_, bloomBlur_, dualDown_, dualUp_, shafts_, visibility_;
    rhi::PipelineHandle flareAdd_, flareOver_, fog_, clouds_;
    rhi::TextureHandle fogTarget_, cloudTarget_; // half resolution
    std::uint32_t width_ = 0, height_ = 0;
    rhi::TextureHandle display_, fxaaOut_, shaftsTarget_;
    rhi::TextureHandle glowA_, glowB_; // 512 x 512, 8-bit
    static constexpr int kDualLevels = 5;
    std::array<rhi::TextureHandle, kDualLevels> dual_{};
    std::array<std::uint32_t, kDualLevels> dualWidth_{}, dualHeight_{};
    std::array<rhi::TextureHandle, 2> visibilityTargets_{};
    int visibilityCurrent_ = 0;
    bool visibilityValid_ = false;
    glm::vec2 axis_{0.0f}; // the flare's axis vector, eased frame to frame
    // This frame's sun: in pixels and uv, whether the flare shows, how strong shafts are.
    glm::vec2 sunPixel_{0.0f}, sunUv_{0.5f};
    bool flareShown_ = false;
    float shaftStrength_ = 0.0f;
};

} // namespace sr::render
