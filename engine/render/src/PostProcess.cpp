#include "sr/render/PostProcess.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>

#include "kAoBlurPixelShader.spv.h"
#include "kBloomBlurPixelShader.spv.h"
#include "kBloomDownPixelShader.spv.h"
#include "kCloudsPixelShader.spv.h"
#include "kComposePixelShader.spv.h"
#include "kAtmosphereVertexShader.spv.h"
#include "kFogPixelShader.spv.h"
#include "kCopyPixelShader.spv.h"
#include "kDepthCopyPixelShader.spv.h"
#include "kDualDownPixelShader.spv.h"
#include "kDualUpPixelShader.spv.h"
#include "kFlarePixelShader.spv.h"
#include "kFlareVertexShader.spv.h"
#include "kFlareVisibilityPixelShader.spv.h"
#include "kFullscreenVertexShader.spv.h"
#include "kFxaaPixelShader.spv.h"
#include "kShaftsPixelShader.spv.h"
#include "kSsaoPixelShader.spv.h"
#include "sr/render/TextureCache.h"

namespace sr::render {

namespace {

constexpr float kPi = 3.14159265358979f;

struct PushData {
    std::uint32_t tex[4]{};
    glm::vec4 p0{0.0f}, p1{0.0f}, p2{0.0f}, p3{0.0f};
    std::uint64_t data = 0;
    std::uint32_t pad[2]{};
};
static_assert(sizeof(PushData) == 96);

struct FlarePush {
    float centre[2];
    float halfSize;
    float alpha;
    float rcpViewport[2];
    std::uint32_t textureIndex;
    std::uint32_t visibilityIndex;
};
static_assert(sizeof(FlarePush) == 32);

template <typename T, std::size_t N>
rhi::PipelineHandle FullScreenPipeline(rhi::Device& device, const T (&pixel)[N], const char* entry, rhi::Format format,
                                       rhi::BlendMode blend, const char* name) {
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = std::as_bytes(std::span(kFullscreenVertexShader));
    desc.fragmentShader = std::as_bytes(std::span(pixel));
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = entry;
    desc.colorFormat = format;
    desc.blend = blend;
    desc.debugName = name;
    return device.CreateGraphicsPipeline(desc);
}

void FullScreen(rhi::CommandList& cmd, rhi::PipelineHandle pipeline, rhi::TextureHandle target, std::uint32_t width,
                std::uint32_t height, const PushData& push, bool load = false) {
    rhi::RenderPassDesc pass;
    pass.color = target;
    pass.loadColor = load;
    cmd.BeginRenderPass(pass);
    cmd.SetViewport({0, 0, static_cast<float>(width), static_cast<float>(height)});
    cmd.SetScissor({0, 0, width, height});
    cmd.BindPipeline(pipeline);
    cmd.PushConstants(&push, sizeof(push));
    cmd.Draw(3, 0);
    cmd.EndPass();
}

void Replace(rhi::Device& device, rhi::TextureHandle& texture, std::uint32_t width, std::uint32_t height, rhi::Format format,
             const char* name) {
    if (texture) device.DestroyTexture(texture);
    texture = device.CreateTexture({std::max(width, 1u), std::max(height, 1u), 1, format, name, rhi::TextureUsage::RenderTarget});
}

// The original's flare: texture (sun\lens<n + 1>), half size in pixels, alpha byte; size 0
// is not drawn. Entry 2 (the sun's glow) is drawn twice the size; it and textures 3 and 7
// blend over the image, the rest add to it.
struct FlareSprite {
    int texture;
    float halfSize;
    float alpha;
};
constexpr FlareSprite kFlare[30] = {
    {6, 30, 10}, {5, 40, 10}, {2, 300, 255}, {6, 30, 20}, {5, 40, 20}, {6, 50, 20}, {3, 300, 20}, {6, 40, 20},
    {5, 30, 20}, {6, 50, 20}, {0, 0, 20},    {5, 30, 19}, {6, 40, 18}, {0, 0, 17},  {5, 40, 16}, {6, 30, 15},
    {0, 0, 14},  {5, 80, 13}, {6, 40, 12},   {0, 0, 11},  {0, 0, 10},  {5, 30, 9},  {7, 120, 8}, {0, 0, 7},
    {5, 40, 6},  {6, 30, 5},  {6, 30, 4},    {5, 80, 3},  {6, 40, 2},  {0, 0, 1},
};

// Colour grading by the time of day (0.5 noon): gain, saturation, lift, contrast.
struct Grade {
    glm::vec3 gain;
    float saturation;
    glm::vec3 lift;
    float contrast;
};
Grade GradeAt(float t) {
    static constexpr Grade kNight{{0.86f, 0.92f, 1.10f}, 0.82f, {0.0f, 0.006f, 0.02f}, 1.05f};
    static constexpr Grade kDusk{{1.10f, 0.97f, 0.86f}, 1.10f, {0.02f, 0.008f, 0.0f}, 1.06f};
    static constexpr Grade kDay{{1.02f, 1.0f, 0.98f}, 1.12f, {0.0f, 0.0f, 0.0f}, 1.08f};
    const auto mix = [](const Grade& a, const Grade& b, float w) {
        return Grade{glm::mix(a.gain, b.gain, w), a.saturation + (b.saturation - a.saturation) * w, glm::mix(a.lift, b.lift, w),
                     a.contrast + (b.contrast - a.contrast) * w};
    };
    const auto smooth = [](float e0, float e1, float x) {
        const float k = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return k * k * (3.0f - 2.0f * k);
    };
    // Night -> dawn (0.2..0.27) -> day (0.27..0.35); day -> dusk (0.72..0.8) -> night (0.8..0.88).
    if (t < 0.5f) {
        if (t < 0.27f) return mix(kNight, kDusk, smooth(0.2f, 0.27f, t));
        return mix(kDusk, kDay, smooth(0.27f, 0.35f, t));
    }
    if (t < 0.8f) return mix(kDay, kDusk, smooth(0.72f, 0.8f, t));
    return mix(kDusk, kNight, smooth(0.8f, 0.88f, t));
}

} // namespace

// ---- AmbientOcclusion ------------------------------------------------------------------

bool AmbientOcclusion::Init(rhi::Device& device, std::string& error) {
    device_ = &device;
    ssao_ = FullScreenPipeline(device, kSsaoPixelShader, "PSSsao", rhi::Format::RGBA8Unorm, rhi::BlendMode::Opaque, "ssao");
    blur_ = FullScreenPipeline(device, kAoBlurPixelShader, "PSAoBlur", rhi::Format::RGBA8Unorm, rhi::BlendMode::Opaque, "ao blur");
    if (!ssao_ || !blur_) {
        error = "ambient occlusion pipelines failed";
        return false;
    }
    return true;
}

void AmbientOcclusion::Shutdown() {
    if (device_ == nullptr) return;
    for (rhi::PipelineHandle* p : {&ssao_, &blur_}) {
        if (*p) device_->DestroyPipeline(*p);
        *p = {};
    }
    for (rhi::TextureHandle* t : {&a_, &b_}) {
        if (*t) device_->DestroyTexture(*t);
        *t = {};
    }
}

void AmbientOcclusion::Ensure(std::uint32_t width, std::uint32_t height, int quality) {
    if (device_ == nullptr) return;
    const bool half = quality < 2;
    const std::uint32_t w = half ? (width + 1) / 2 : width, h = half ? (height + 1) / 2 : height;
    if (w == width_ && h == height_ && a_) return;
    Replace(*device_, a_, w, h, rhi::Format::RGBA8Unorm, "ambient occlusion");
    Replace(*device_, b_, w, h, rhi::Format::RGBA8Unorm, "ambient occlusion blur");
    width_ = w;
    height_ = h;
}

void AmbientOcclusion::Render(rhi::CommandList& cmd, rhi::TextureHandle depth, std::uint32_t width, std::uint32_t height, int quality,
                              const glm::mat4& viewProj, const glm::vec3& camera, const glm::vec3& forward, float nearPlane,
                              float farPlane) {
    if (device_ == nullptr || !ssao_ || !depth || width == 0 || height == 0) return;
    Ensure(width, height, quality);
    const bool half = quality < 2;
    const std::uint32_t w = width_, h = height_;
    const rhi::TransientAllocation alloc = device_->AllocateTransient(160, 16);
    if (alloc.cpu == nullptr) return;
    auto* block = static_cast<float*>(alloc.cpu);
    const glm::mat4 inverse = glm::inverse(viewProj);
    std::memcpy(block, &viewProj, 64);
    std::memcpy(block + 16, &inverse, 64);
    const glm::vec4 eye(camera, 1.0f), ahead(forward, 0.0f);
    std::memcpy(block + 32, &eye, 16);
    std::memcpy(block + 36, &ahead, 16);

    cmd.BeginTimer("ambient occlusion");
    PushData push;
    push.tex[0] = device_->BindlessIndex(depth);
    push.p0 = glm::vec4(9.0f, 1.3f, half ? 8.0f : 16.0f, 1800.0f);
    push.p1 = glm::vec4(1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height), 0.0f, 0.0f);
    push.data = alloc.gpuAddress;
    FullScreen(cmd, ssao_, a_, w, h, push);
    PushData blur;
    blur.tex[0] = device_->BindlessIndex(a_);
    blur.tex[1] = device_->BindlessIndex(depth);
    blur.p0 = glm::vec4(1.0f / static_cast<float>(w), 0.0f, nearPlane, farPlane);
    FullScreen(cmd, blur_, b_, w, h, blur);
    blur.tex[0] = device_->BindlessIndex(b_);
    blur.p0 = glm::vec4(0.0f, 1.0f / static_cast<float>(h), nearPlane, farPlane);
    FullScreen(cmd, blur_, a_, w, h, blur);
    cmd.EndTimer();
}

// ---- SceneCopy --------------------------------------------------------------------------

bool SceneCopy::Init(rhi::Device& device, rhi::Format colorFormat, std::string& error) {
    device_ = &device;
    color_ = FullScreenPipeline(device, kCopyPixelShader, "PSCopy", colorFormat, rhi::BlendMode::Opaque, "copy");
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = std::as_bytes(std::span(kFullscreenVertexShader));
    desc.fragmentShader = std::as_bytes(std::span(kDepthCopyPixelShader));
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = "PSDepthCopy";
    desc.depthOnly = true;
    desc.depthFormat = rhi::Format::D32Float;
    desc.depthTest = true;
    desc.depthWrite = true;
    desc.debugName = "depth copy";
    depth_ = device.CreateGraphicsPipeline(desc);
    if (!color_ || !depth_) {
        error = "copy pipelines failed";
        return false;
    }
    return true;
}

void SceneCopy::Shutdown() {
    if (device_ == nullptr) return;
    for (rhi::PipelineHandle* p : {&color_, &depth_}) {
        if (*p) device_->DestroyPipeline(*p);
        *p = {};
    }
}

void SceneCopy::Color(rhi::CommandList& cmd, rhi::TextureHandle source, rhi::TextureHandle target, std::uint32_t width,
                      std::uint32_t height) const {
    if (!color_ || !source || !target) return;
    PushData push;
    push.tex[0] = device_->BindlessIndex(source);
    FullScreen(cmd, color_, target, width, height, push);
}

void SceneCopy::Depth(rhi::CommandList& cmd, rhi::TextureHandle source, rhi::TextureHandle target, std::uint32_t width,
                      std::uint32_t height) const {
    if (!depth_ || !source || !target) return;
    rhi::RenderPassDesc pass;
    pass.depth = target;
    cmd.BeginRenderPass(pass);
    cmd.SetViewport({0, 0, static_cast<float>(width), static_cast<float>(height)});
    cmd.SetScissor({0, 0, width, height});
    cmd.BindPipeline(depth_);
    PushData push;
    push.tex[0] = device_->BindlessIndex(source);
    cmd.PushConstants(&push, sizeof(push));
    cmd.Draw(3, 0);
    cmd.EndPass();
}

// ---- PostProcess ------------------------------------------------------------------------

bool PostProcess::Init(rhi::Device& device, rhi::Format displayFormat, std::string& error) {
    device_ = &device;
    displayFormat_ = displayFormat;
    using rhi::BlendMode;
    using rhi::Format;
    fxaa_ = FullScreenPipeline(device, kFxaaPixelShader, "PSFxaa", displayFormat, BlendMode::Opaque, "fxaa");
    compose_ = FullScreenPipeline(device, kComposePixelShader, "PSCompose", displayFormat, BlendMode::Opaque, "compose");
    bloomDown_ = FullScreenPipeline(device, kBloomDownPixelShader, "PSBloomDown", Format::RGBA8Unorm, BlendMode::Opaque, "bloom down");
    bloomBlur_ = FullScreenPipeline(device, kBloomBlurPixelShader, "PSBloomBlur", Format::RGBA8Unorm, BlendMode::Opaque, "bloom blur");
    dualDown_ = FullScreenPipeline(device, kDualDownPixelShader, "PSDualDown", Format::RGBA16Float, BlendMode::Opaque, "bloom dual down");
    dualUp_ = FullScreenPipeline(device, kDualUpPixelShader, "PSDualUp", Format::RGBA16Float, BlendMode::Additive, "bloom dual up");
    shafts_ = FullScreenPipeline(device, kShaftsPixelShader, "PSShafts", Format::RGBA16Float, BlendMode::Opaque, "light shafts");
    visibility_ = FullScreenPipeline(device, kFlareVisibilityPixelShader, "PSFlareVisibility", Format::RGBA16Float, BlendMode::Opaque,
                                     "flare visibility");
    rhi::GraphicsPipelineDesc flare;
    flare.vertexShader = std::as_bytes(std::span(kFlareVertexShader));
    flare.fragmentShader = std::as_bytes(std::span(kFlarePixelShader));
    flare.vertexEntry = "VSMain";
    flare.fragmentEntry = "PSMain";
    flare.colorFormat = displayFormat;
    flare.blend = BlendMode::Additive;
    flare.debugName = "lens flare (add)";
    flareAdd_ = device.CreateGraphicsPipeline(flare);
    flare.blend = BlendMode::AlphaBlend;
    flare.debugName = "lens flare (over)";
    flareOver_ = device.CreateGraphicsPipeline(flare);
    rhi::GraphicsPipelineDesc atmosphere;
    atmosphere.vertexShader = std::as_bytes(std::span(kAtmosphereVertexShader));
    atmosphere.vertexEntry = "VSMain";
    atmosphere.colorFormat = Format::RGBA16Float;
    atmosphere.fragmentShader = std::as_bytes(std::span(kFogPixelShader));
    atmosphere.fragmentEntry = "PSFog";
    atmosphere.debugName = "volumetric fog";
    fog_ = device.CreateGraphicsPipeline(atmosphere);
    atmosphere.fragmentShader = std::as_bytes(std::span(kCloudsPixelShader));
    atmosphere.fragmentEntry = "PSClouds";
    atmosphere.debugName = "volumetric clouds";
    clouds_ = device.CreateGraphicsPipeline(atmosphere);
    if (!fxaa_ || !compose_ || !bloomDown_ || !bloomBlur_ || !dualDown_ || !dualUp_ || !shafts_ || !visibility_ || !flareAdd_ ||
        !flareOver_ || !fog_ || !clouds_) {
        error = "post-processing pipelines failed";
        return false;
    }
    return true;
}

void PostProcess::Shutdown() {
    if (device_ == nullptr) return;
    for (rhi::PipelineHandle* p : {&fxaa_, &compose_, &bloomDown_, &bloomBlur_, &dualDown_, &dualUp_, &shafts_, &visibility_,
                                   &flareAdd_, &flareOver_, &fog_, &clouds_}) {
        if (*p) device_->DestroyPipeline(*p);
        *p = {};
    }
    for (rhi::TextureHandle* t : {&display_, &fxaaOut_, &shaftsTarget_, &glowA_, &glowB_, &fogTarget_, &cloudTarget_}) {
        if (*t) device_->DestroyTexture(*t);
        *t = {};
    }
    for (rhi::TextureHandle& t : dual_) {
        if (t) device_->DestroyTexture(t);
        t = {};
    }
    for (rhi::TextureHandle& t : visibilityTargets_) {
        if (t) device_->DestroyTexture(t);
        t = {};
    }
    width_ = height_ = 0;
}

void PostProcess::Ensure(std::uint32_t width, std::uint32_t height) {
    if (!glowA_) {
        Replace(*device_, glowA_, 512, 512, rhi::Format::RGBA8Unorm, "bloom 512 a");
        Replace(*device_, glowB_, 512, 512, rhi::Format::RGBA8Unorm, "bloom 512 b");
        for (rhi::TextureHandle& t : visibilityTargets_) Replace(*device_, t, 1, 1, rhi::Format::RGBA16Float, "flare visibility");
    }
    if (width == width_ && height == height_ && display_) return;
    width_ = width;
    height_ = height;
    Replace(*device_, display_, width, height, displayFormat_, "post display");
    Replace(*device_, fxaaOut_, width, height, displayFormat_, "post fxaa");
    Replace(*device_, shaftsTarget_, (width + 1) / 2, (height + 1) / 2, rhi::Format::RGBA16Float, "light shafts");
    Replace(*device_, fogTarget_, (width + 1) / 2, (height + 1) / 2, rhi::Format::RGBA16Float, "volumetric fog");
    Replace(*device_, cloudTarget_, width, height, rhi::Format::RGBA16Float, "volumetric clouds");
    std::uint32_t w = width, h = height;
    for (int i = 0; i < kDualLevels; ++i) {
        w = std::max(1u, (w + 1) / 2);
        h = std::max(1u, (h + 1) / 2);
        dualWidth_[static_cast<std::size_t>(i)] = w;
        dualHeight_[static_cast<std::size_t>(i)] = h;
        Replace(*device_, dual_[static_cast<std::size_t>(i)], w, h, rhi::Format::RGBA16Float, "bloom level");
    }
}

void PostProcess::Fxaa(rhi::CommandList& cmd, rhi::TextureHandle source, rhi::TextureHandle target, std::uint32_t width,
                       std::uint32_t height) const {
    if (!fxaa_ || !source || !target || width == 0 || height == 0) return;
    PushData push;
    push.tex[0] = device_->BindlessIndex(source);
    push.p0 = glm::vec4(1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height), 0.0f, 0.0f);
    FullScreen(cmd, fxaa_, target, width, height, push);
}

void PostProcess::FlareVisibility(rhi::CommandList& cmd, const PostSettings& settings, std::uint32_t width, std::uint32_t height) {
    flareShown_ = false;
    shaftStrength_ = 0.0f;
    // The sun on its arc, seen 3500 units out from the camera.
    const float angle = (settings.timeOfDay - 0.25f) * 2.0f * kPi;
    const glm::vec3 toSun(std::cos(angle), std::sin(angle), 0.0f);
    const float sunHeight = toSun.y * 20000.0f;
    const glm::vec3 sun = settings.camera + toSun * 3500.0f;
    const glm::vec4 clip = settings.viewProj * glm::vec4(sun, 1.0f);
    const glm::vec2 ndc = clip.w > 0.0f ? glm::vec2(clip) / clip.w : glm::vec2(10.0f);
    sunUv_ = ndc * 0.5f + 0.5f;
    sunPixel_ = sunUv_ * glm::vec2(static_cast<float>(width), static_cast<float>(height));
    const bool inView = clip.w > 0.0f && std::abs(ndc.x) <= 1.1f && std::abs(ndc.y) <= 1.1f;
    flareShown_ = sunHeight >= 700.0f && inView;
    // Shafts: strong with the sun up, fading as it leaves the screen.
    if (clip.w > 0.0f) {
        const float up = std::clamp((toSun.y - 0.035f) / 0.2f, 0.0f, 1.0f);
        const float edge = std::clamp(1.6f - std::max(std::abs(ndc.x), std::abs(ndc.y)), 0.0f, 1.0f);
        shaftStrength_ = up * edge;
    }
    // Five points around the sun, tested against the depth.
    const float offset = static_cast<float>(width) / 1024.0f * 10.0f;
    const glm::vec3 points[5] = {sun, sun + glm::vec3(0, 0, offset), sun - glm::vec3(0, 0, offset), sun + glm::vec3(offset, 0, 0),
                                 sun - glm::vec3(offset, 0, 0)};
    const float weights[5] = {0.3f, 0.175f, 0.175f, 0.175f, 0.175f};
    const rhi::TransientAllocation alloc = device_->AllocateTransient(5 * sizeof(glm::vec4), 16);
    if (alloc.cpu == nullptr) return;
    auto* probes = static_cast<glm::vec4*>(alloc.cpu);
    for (int i = 0; i < 5; ++i) {
        const glm::vec4 c = settings.viewProj * glm::vec4(points[i], 1.0f);
        glm::vec4 probe(0.0f);
        if (flareShown_ && c.w > 0.0f) {
            const glm::vec3 n = glm::vec3(c) / c.w;
            const glm::vec2 uv = glm::vec2(n) * 0.5f + 0.5f;
            const bool hidden = !settings.depth && settings.blocked && settings.blocked(points[i]);
            if (uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f && !hidden) probe = glm::vec4(uv, n.z, weights[i]);
        }
        probes[i] = probe;
    }
    PushData push;
    push.tex[0] = settings.depth ? device_->BindlessIndex(settings.depth) : 0; // 0: white, nothing hides the sun
    push.tex[1] = visibilityValid_ ? device_->BindlessIndex(visibilityTargets_[static_cast<std::size_t>(visibilityCurrent_)]) : 0;
    push.p0.x = std::min(settings.deltaSeconds * 10.0f, 1.0f);
    push.data = alloc.gpuAddress;
    visibilityCurrent_ ^= 1;
    FullScreen(cmd, visibility_, visibilityTargets_[static_cast<std::size_t>(visibilityCurrent_)], 1, 1, push);
    visibilityValid_ = true;
    // The axis eases halfway to 2.2 x the way to the centre each (60th of a second) frame.
    const glm::vec2 centre(static_cast<float>(width) * 0.5f, static_cast<float>(height) * 0.5f);
    const float ease = 1.0f - std::pow(0.5f, std::max(settings.deltaSeconds, 0.0f) * 60.0f);
    axis_ += (2.2f * (centre - sunPixel_) - axis_) * ease;
}

void PostProcess::FlareSprites(rhi::CommandList& cmd, TextureCache& textures, std::uint32_t width, std::uint32_t height) {
    if (!flareShown_ || !visibilityValid_) return;
    rhi::RenderPassDesc pass;
    pass.color = display_;
    pass.loadColor = true;
    cmd.BeginRenderPass(pass);
    cmd.SetViewport({0, 0, static_cast<float>(width), static_cast<float>(height)});
    cmd.SetScissor({0, 0, width, height});
    const std::uint32_t visibility = device_->BindlessIndex(visibilityTargets_[static_cast<std::size_t>(visibilityCurrent_)]);
    rhi::PipelineHandle bound;
    for (int i = 0; i < 30; ++i) {
        const FlareSprite& s = kFlare[i];
        if (s.halfSize <= 0.0f) continue;
        char path[32];
        std::snprintf(path, sizeof path, "sun\\lens%d.ddj", s.texture + 1);
        const TextureCache::Entry& texture = textures.Get(path);
        if (!texture.handle) continue;
        const bool over = i == 2 || s.texture == 3 || s.texture == 7;
        const rhi::PipelineHandle pipeline = over ? flareOver_ : flareAdd_;
        if (!(pipeline == bound)) {
            cmd.BindPipeline(pipeline);
            bound = pipeline;
        }
        const glm::vec2 at = sunPixel_ + axis_ * (static_cast<float>(i - 2) / 19.0f);
        FlarePush push{{at.x, at.y}, s.halfSize, s.alpha, {1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height)},
                       device_->BindlessIndex(texture.handle), visibility};
        cmd.PushConstants(&push, sizeof(push));
        cmd.Draw(6, 0);
    }
    cmd.EndPass();
}

rhi::TextureHandle PostProcess::Prepare(std::uint32_t width, std::uint32_t height, const PostSettings& settings) {
    if (device_ == nullptr || !compose_ || width == 0 || height == 0 || !settings.Any()) return {};
    Ensure(width, height);
    return settings.fxaa ? fxaaOut_ : display_;
}

rhi::TextureHandle PostProcess::Run(rhi::CommandList& cmd, rhi::TextureHandle scene, std::uint32_t width, std::uint32_t height,
                                    const PostSettings& settings, TextureCache& textures) {
    if (device_ == nullptr || !compose_ || !scene || width == 0 || height == 0 || !settings.Any()) return scene;
    Ensure(width, height);
    const std::uint32_t sceneIndex = device_->BindlessIndex(scene);
    cmd.BeginTimer("post");

    // Bloom.
    std::uint32_t bloom = 0;
    if (settings.bloom == 1) {
        PushData push;
        push.tex[0] = sceneIndex;
        FullScreen(cmd, bloomDown_, glowA_, 512, 512, push);
        push.tex[0] = device_->BindlessIndex(glowA_);
        push.p0 = glm::vec4(1.0f / 512.0f, 0.0f, 40.0f / 255.0f, 0.0f);
        FullScreen(cmd, bloomBlur_, glowB_, 512, 512, push);
        push.tex[0] = device_->BindlessIndex(glowB_);
        push.p0 = glm::vec4(0.0f, 1.0f / 512.0f, 0.0f, 0.0f);
        FullScreen(cmd, bloomBlur_, glowA_, 512, 512, push);
        bloom = device_->BindlessIndex(glowA_);
    } else if (settings.bloom == 2) {
        PushData push;
        push.tex[0] = sceneIndex;
        push.p0 = glm::vec4(1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height), 0.8f, 0.4f);
        FullScreen(cmd, dualDown_, dual_[0], dualWidth_[0], dualHeight_[0], push);
        for (std::size_t i = 1; i < kDualLevels; ++i) {
            push.tex[0] = device_->BindlessIndex(dual_[i - 1]);
            push.p0 = glm::vec4(1.0f / static_cast<float>(dualWidth_[i - 1]), 1.0f / static_cast<float>(dualHeight_[i - 1]), 0.0f, 0.0f);
            FullScreen(cmd, dualDown_, dual_[i], dualWidth_[i], dualHeight_[i], push);
        }
        for (std::size_t i = kDualLevels - 1; i-- > 0;) {
            push.tex[0] = device_->BindlessIndex(dual_[i + 1]);
            push.p0 = glm::vec4(1.0f / static_cast<float>(dualWidth_[i + 1]), 1.0f / static_cast<float>(dualHeight_[i + 1]), 0.0f, 0.0f);
            FullScreen(cmd, dualUp_, dual_[i], dualWidth_[i], dualHeight_[i], push, true);
        }
        bloom = device_->BindlessIndex(dual_[0]);
    }

    // The flare's visibility, and the shafts toward the sun (modern).
    if (settings.lensFlare > 0) FlareVisibility(cmd, settings, width, height);
    std::uint32_t shafts = 0;
    if (settings.lensFlare == 2 && settings.depth && shaftStrength_ > 0.0f) {
        PushData push;
        push.tex[0] = sceneIndex;
        push.tex[1] = device_->BindlessIndex(settings.depth);
        push.p0 = glm::vec4(sunUv_, 48.0f, 0.975f);
        FullScreen(cmd, shafts_, shaftsTarget_, (width + 1) / 2, (height + 1) / 2, push);
        shafts = device_->BindlessIndex(shaftsTarget_);
    }

    // Volumetric clouds and fog, at half resolution.
    std::uint32_t fog = 0, clouds = 0;
    if ((settings.volumetricFog > 0 || settings.volumetricClouds) && settings.depth && settings.frame != 0) {
        struct AtmospherePush {
            std::uint64_t frame;
            std::uint64_t camera;
            std::uint32_t depthIndex;
            float steps;
            float density;
            float time;
            glm::vec4 params;
        };
        static_assert(sizeof(AtmospherePush) == 48);
        const rhi::TransientAllocation alloc = device_->AllocateTransient(128, 16);
        if (alloc.cpu != nullptr) {
            const glm::mat4 inverse = glm::inverse(settings.viewProj);
            std::memcpy(alloc.cpu, &settings.viewProj, 64);
            std::memcpy(static_cast<char*>(alloc.cpu) + 64, &inverse, 64);
            const std::uint32_t w = (width + 1) / 2, h = (height + 1) / 2;
            const auto march = [&](rhi::PipelineHandle pipeline, rhi::TextureHandle target, const AtmospherePush& push) {
                // The fog at half resolution; the clouds whole (their edges meet the scene's).
                const std::uint32_t tw = pipeline == fog_ ? w : width, th = pipeline == fog_ ? h : height;
                rhi::RenderPassDesc pass;
                pass.color = target;
                pass.clearColor[3] = pipeline == fog_ ? 1.0f : 0.0f;
                cmd.BeginRenderPass(pass);
                cmd.SetViewport({0, 0, static_cast<float>(tw), static_cast<float>(th)});
                cmd.SetScissor({0, 0, tw, th});
                cmd.BindPipeline(pipeline);
                cmd.PushConstants(&push, sizeof(push));
                cmd.Draw(3, 0);
                cmd.EndPass();
            };
            const float seconds = settings.seconds;
            if (settings.volumetricClouds) {
                const float coverage = 0.45f + 0.4f * settings.weatherPresence;
                march(clouds_, cloudTarget_,
                      {settings.frame, alloc.gpuAddress, device_->BindlessIndex(settings.depth), 0.0f, coverage, seconds,
                       glm::vec4(settings.camera.y + 2500.0f, 0.0f, 0.0f, 0.0f)});
                clouds = device_->BindlessIndex(cloudTarget_);
            }
            if (settings.volumetricFog > 0) {
                // Thicker at dawn and in rain or snow; hugging the ground around the camera.
                const float dawn = std::exp(-std::pow((settings.timeOfDay - 0.27f) / 0.05f, 2.0f));
                const float density = 0.00022f * (1.0f + 2.0f * dawn + 3.0f * settings.weatherPresence);
                march(fog_, fogTarget_,
                      {settings.frame, alloc.gpuAddress, device_->BindlessIndex(settings.depth), settings.volumetricFog == 2 ? 24.0f : 8.0f,
                       density, seconds, glm::vec4(settings.camera.y - 140.0f, 220.0f, 0.0f, 0.0f)});
                fog = device_->BindlessIndex(fogTarget_);
            }
        }
    }

    // Compose: bloom and shafts in, tone mapping, grading.
    {
        PushData push;
        push.tex[0] = sceneIndex;
        push.tex[1] = bloom;
        push.tex[2] = shafts;
        push.tex[3] = fog;
        const float exposure = settings.toneMapping == 2 ? 1.25f : 1.0f;
        push.p0 = glm::vec4(static_cast<float>(settings.toneMapping), static_cast<float>(settings.bloom), exposure, 1.6f * shaftStrength_);
        const Grade grade = GradeAt(settings.timeOfDay);
        push.p1 = glm::vec4(grade.gain, grade.saturation);
        push.p2 = glm::vec4(grade.lift, grade.contrast);
        push.p3 = glm::vec4(0.25f, 0.05f, settings.grading ? 1.0f : 0.0f, static_cast<float>(clouds));
        FullScreen(cmd, compose_, display_, width, height, push);
    }
    if (settings.lensFlare > 0) FlareSprites(cmd, textures, width, height);

    rhi::TextureHandle result = display_;
    if (settings.fxaa) {
        Fxaa(cmd, display_, fxaaOut_, width, height);
        result = fxaaOut_;
    }
    cmd.EndTimer();
    return result;
}

} // namespace sr::render
