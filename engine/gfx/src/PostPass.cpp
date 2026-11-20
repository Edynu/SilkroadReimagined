#include "sr/gfx/PostPass.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "kPostFlarePixelShader.spv.h"
#include "kPostFlareVertexShader.spv.h"
#include "kPostPixelShader.spv.h"
#include "kPostVertexShader.spv.h"
#include "sr/core/Trace.h"

namespace sr::gfx {

namespace {

template <std::size_t N>
std::span<const std::byte> Spirv(const unsigned char (&code)[N]) {
    return {reinterpret_cast<const std::byte*>(code), N};
}

// Post.hlsl's modes and constants.
constexpr std::uint32_t kModeDown = 0, kModeBlurX = 1, kModeBlurY = 2, kModeComposite = 3, kModeVisibility = 4;
constexpr std::uint32_t kBloomSize = 512;

struct PostConstants {
    std::uint32_t textures[4]{};
    glm::vec4 params{0.0f};
    glm::vec4 points[5]{}; // visibility: uv, depth, 1 when in front of the camera
};

struct Push {
    std::uint64_t constants = 0;
    std::uint32_t mode = 0;
    std::uint32_t pad = 0;
};

struct FlarePush {
    glm::vec4 rect{0.0f};       // centre x, y (pixels), half size (pixels), alpha base (0..255)
    glm::vec2 invViewport{0.0f};
    std::uint32_t texture = 0;
    std::uint32_t intensity = 0; // the 1 x 1 intensity target
};

// The original's 30 flare slots, those that draw: slot, sun\lens texture (3..8),
// half size in pixels, alpha base (x the intensity), alpha blended (else additive).
struct FlareSlot {
    int slot;
    int lens;
    float size;
    float alpha;
    bool blended;
};
constexpr FlareSlot kFlare[] = {
    {0, 7, 30, 10, false},  {1, 6, 40, 10, false},  {2, 3, 300, 255, true},  {3, 7, 30, 20, false},  {4, 6, 40, 20, false},
    {5, 7, 50, 20, false},  {6, 4, 300, 20, true},  {7, 7, 40, 20, false},   {8, 6, 30, 20, false},  {9, 7, 50, 20, false},
    {11, 6, 30, 19, false}, {12, 7, 40, 18, false}, {14, 6, 40, 16, false},  {15, 7, 30, 15, false}, {17, 6, 80, 13, false},
    {18, 7, 40, 12, false}, {21, 6, 30, 9, false},  {22, 8, 120, 8, true},   {24, 6, 40, 6, false},  {25, 7, 30, 5, false},
    {26, 7, 30, 4, false},  {27, 6, 80, 3, false},  {28, 7, 40, 2, false},
};

} // namespace

bool PostPass::Init(rhi::Device& device, rhi::Format sceneFormat, std::string& error) {
    device_ = &device;
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = Spirv(kPostVertexShader);
    desc.fragmentShader = Spirv(kPostPixelShader);
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = "PSMain";
    desc.colorFormat = rhi::Format::RGBA8Unorm;
    desc.debugName = "glow";
    down_ = device.CreateGraphicsPipeline(desc);
    blurX_ = device.CreateGraphicsPipeline(desc);
    blurY_ = device.CreateGraphicsPipeline(desc);
    desc.colorFormat = sceneFormat;
    desc.debugName = "post composite";
    composite_ = device.CreateGraphicsPipeline(desc);
    desc.colorFormat = rhi::Format::RGBA16Float;
    desc.debugName = "flare visibility";
    visibility_ = device.CreateGraphicsPipeline(desc);

    rhi::GraphicsPipelineDesc flare;
    flare.vertexShader = Spirv(kPostFlareVertexShader);
    flare.fragmentShader = Spirv(kPostFlarePixelShader);
    flare.vertexEntry = "VSFlare";
    flare.fragmentEntry = "PSFlare";
    flare.colorFormat = sceneFormat;
    flare.blend = rhi::BlendMode::Additive; // source x source alpha + destination
    flare.debugName = "lens flare";
    flareAdd_ = device.CreateGraphicsPipeline(flare);
    flare.blend = rhi::BlendMode::AlphaBlend;
    flareAlpha_ = device.CreateGraphicsPipeline(flare);

    rhi::TextureDesc target;
    target.width = target.height = kBloomSize;
    target.format = rhi::Format::RGBA8Unorm;
    target.usage = rhi::TextureUsage::RenderTarget;
    target.debugName = "glow";
    bloom_ = device.CreateTexture(target);
    target.debugName = "glow blur";
    blur_ = device.CreateTexture(target);
    target.width = target.height = 1;
    target.format = rhi::Format::RGBA16Float;
    target.debugName = "flare intensity";
    for (rhi::TextureHandle& t : intensity_) t = device.CreateTexture(target);
    if (!down_ || !blurX_ || !blurY_ || !composite_ || !visibility_ || !flareAdd_ || !flareAlpha_ || !bloom_ || !blur_ || !intensity_[0] ||
        !intensity_[1]) {
        error = "cannot create the glow and lens flare passes";
        return false;
    }
    return true;
}

void PostPass::Shutdown() {
    if (device_ == nullptr) return;
    for (rhi::PipelineHandle p : {down_, blurX_, blurY_, composite_, visibility_, flareAdd_, flareAlpha_}) device_->DestroyPipeline(p);
    device_->DestroyTexture(bloom_);
    device_->DestroyTexture(blur_);
    for (rhi::TextureHandle t : intensity_) device_->DestroyTexture(t);
    device_ = nullptr;
}

RenderGraph::Resource PostPass::AddPasses(RenderGraph& graph, RenderGraph::Resource sceneResource, const Frame& frame) {
    if (!frame.glow && !frame.flare) return sceneResource;
    rhi::Device& device = *device_;
    // A full-screen pass: `mode` over `target` with these constants.
    const auto fullscreen = [&device](rhi::CommandList& cmd, rhi::PipelineHandle pipeline, rhi::TextureHandle target, std::uint32_t mode,
                                      const PostConstants& constants) {
        const rhi::TransientAllocation alloc = device.AllocateTransient(sizeof(constants), 16);
        if (alloc.cpu == nullptr) return;
        std::memcpy(alloc.cpu, &constants, sizeof(constants));
        rhi::RenderPassDesc pass;
        pass.color = target;
        cmd.BeginRenderPass(pass);
        cmd.BindPipeline(pipeline);
        Push push;
        push.constants = alloc.gpuAddress;
        push.mode = mode;
        cmd.PushConstants(&push, sizeof(push));
        cmd.Draw(3, 0);
        cmd.EndPass();
    };

    const auto output = graph.ImportTexture(frame.output, "post");
    RenderGraph::Resource bloom = 0;
    if (frame.glow) {
        const auto first = graph.ImportTexture(bloom_, "glow");
        const auto blur = graph.ImportTexture(blur_, "glow blur");
        bloom = graph.ImportTexture(bloom_, "glow (blurred)");
        PostConstants down;
        down.textures[0] = device.BindlessIndex(frame.scene);
        graph.AddPass("glow: down", PassKind::Graphics,
                      [&](RenderGraph::Builder& b) { b.Read(sceneResource, Use::Sampled).Write(first, Use::ColorWrite); },
                      [this, fullscreen, down](rhi::CommandList& cmd, const RenderGraph&) { fullscreen(cmd, down_, bloom_, kModeDown, down); });
        PostConstants x;
        x.textures[0] = device.BindlessIndex(bloom_);
        graph.AddPass("glow: blur x", PassKind::Graphics,
                      [&](RenderGraph::Builder& b) { b.Read(first, Use::Sampled).Write(blur, Use::ColorWrite); },
                      [this, fullscreen, x](rhi::CommandList& cmd, const RenderGraph&) { fullscreen(cmd, blurX_, blur_, kModeBlurX, x); });
        PostConstants y;
        y.textures[0] = device.BindlessIndex(blur_);
        graph.AddPass("glow: blur y", PassKind::Graphics,
                      [&](RenderGraph::Builder& b) { b.Read(blur, Use::Sampled).Write(bloom, Use::ColorWrite); },
                      [this, fullscreen, y](rhi::CommandList& cmd, const RenderGraph&) { fullscreen(cmd, blurY_, bloom_, kModeBlurY, y); });
    }

    // The flare: the sun's point and four neighbours, then the sprites along the axis.
    bool flare = false;
    RenderGraph::Resource intensity = 0;
    if (frame.flare && frame.sunUp && frame.width > 0 && frame.height > 0) {
        const glm::vec3 sun = frame.eye + glm::normalize(frame.sunDirection) * frame.viewDistance;
        const glm::vec4 clip = frame.viewProj * glm::vec4(sun, 1.0f);
        if (clip.w > 0.0f && std::abs(clip.x / clip.w) <= 1.1f && std::abs(clip.y / clip.w) <= 1.1f) {
            flare = true;
            PostConstants v;
            const unsigned previous = current_;
            current_ ^= 1u;
            v.textures[0] = device.BindlessIndex(frame.depth);
            v.textures[1] = device.BindlessIndex(intensity_[previous]);
            v.params = glm::vec4(frame.seconds, 0, 0, 0);
            const float e = 10.0f * static_cast<float>(frame.width) / 1024.0f;
            const glm::vec3 offsets[5] = {{0, 0, 0}, {0, 0, e}, {0, 0, -e}, {e, 0, 0}, {-e, 0, 0}};
            for (int i = 0; i < 5; ++i) {
                const glm::vec4 p = frame.viewProj * glm::vec4(sun + offsets[i], 1.0f);
                if (p.w <= 0.0f) continue;
                const glm::vec3 ndc = glm::vec3(p) / p.w;
                v.points[i] = glm::vec4(ndc.x * 0.5f + 0.5f, ndc.y * 0.5f + 0.5f, ndc.z, 1.0f);
            }
            const auto previousResource = graph.ImportTexture(intensity_[previous], "flare intensity (last)");
            intensity = graph.ImportTexture(intensity_[current_], "flare intensity");
            graph.AddPass("flare: visibility", PassKind::Graphics,
                          [&](RenderGraph::Builder& b) {
                              b.Read(sceneResource, Use::Sampled).Read(previousResource, Use::Sampled).Write(intensity, Use::ColorWrite);
                          },
                          [this, fullscreen, v, target = intensity_[current_]](rhi::CommandList& cmd, const RenderGraph&) {
                              fullscreen(cmd, visibility_, target, kModeVisibility, v);
                          });
            // The axis follows 2.2 x (centre - sun) by half the way a frame (as the original, per frame).
            const glm::vec2 s((clip.x / clip.w + 1.0f) * 0.5f * static_cast<float>(frame.width),
                              (clip.y / clip.w + 1.0f) * 0.5f * static_cast<float>(frame.height));
            const glm::vec2 centre(static_cast<float>(frame.width) * 0.5f, static_cast<float>(frame.height) * 0.5f);
            if (!axisValid_) axis_ = glm::vec2(0.0f);
            axisValid_ = true;
            axis_ += 0.5f * (2.2f * (centre - s) - axis_);
            sunScreen_ = s;
        }
    }
    if (!flare) axisValid_ = false;

    PostConstants composite;
    composite.textures[0] = device.BindlessIndex(frame.scene);
    composite.textures[1] = frame.glow ? device.BindlessIndex(bloom_) : 0;
    composite.params = glm::vec4(frame.glow ? 1.0f : 0.0f, 0, 0, 0);
    graph.AddPass("scene: post", PassKind::Graphics,
                  [&](RenderGraph::Builder& b) {
                      b.Read(sceneResource, Use::Sampled).Write(output, Use::ColorWrite);
                      if (bloom != 0) b.Read(bloom, Use::Sampled);
                      if (intensity != 0) b.Read(intensity, Use::Sampled);
                  },
                  [this, fullscreen, composite, frame, flare](rhi::CommandList& cmd, const RenderGraph&) {
                      SR_ZONE("render: post");
                      const rhi::TransientAllocation alloc = device_->AllocateTransient(sizeof(composite), 16);
                      if (alloc.cpu == nullptr) return;
                      std::memcpy(alloc.cpu, &composite, sizeof(composite));
                      rhi::RenderPassDesc pass;
                      pass.color = frame.output;
                      cmd.BeginRenderPass(pass);
                      cmd.BindPipeline(composite_);
                      Push push;
                      push.constants = alloc.gpuAddress;
                      push.mode = kModeComposite;
                      cmd.PushConstants(&push, sizeof(push));
                      cmd.Draw(3, 0);
                      if (flare) DrawFlare(cmd, frame, device_->BindlessIndex(intensity_[current_]));
                      cmd.EndPass();
                      (void)fullscreen;
                  });
    return output;
}

void PostPass::DrawFlare(rhi::CommandList& cmd, const Frame& frame, std::uint32_t intensity) const {
    FlarePush push;
    push.invViewport = glm::vec2(1.0f / static_cast<float>(frame.width), 1.0f / static_cast<float>(frame.height));
    push.intensity = intensity;
    for (const FlareSlot& f : kFlare) {
        const std::uint32_t texture = frame.lens[static_cast<std::size_t>(f.lens - 3)];
        if (texture == 0) continue;
        const glm::vec2 at = sunScreen_ + axis_ * (static_cast<float>(f.slot - 2) / 19.0f);
        push.rect = glm::vec4(at, f.size, f.alpha);
        push.texture = texture;
        cmd.BindPipeline(f.blended ? flareAlpha_ : flareAdd_);
        cmd.PushConstants(&push, sizeof(push));
        cmd.Draw(6, 0);
    }
}

} // namespace sr::gfx
