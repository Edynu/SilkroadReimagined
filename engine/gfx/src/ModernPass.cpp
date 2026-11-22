#include "sr/gfx/ModernPass.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "kDepthCopyPixelShader.spv.h"
#include "kModernPixelShader.spv.h"
#include "kModernVertexShader.spv.h"
#include "sr/core/Trace.h"

namespace sr::gfx {

namespace {

template <std::size_t N>
std::span<const std::byte> Spirv(const unsigned char (&code)[N]) {
    return {reinterpret_cast<const std::byte*>(code), N};
}

// Modern.hlsl's modes and constants.
constexpr std::uint32_t kModeOcclusion = 0, kModeOcclusionBlur = 1, kModeShafts = 2, kModeBloomFirst = 3, kModeBloomDown = 4,
                        kModeBloomUp = 5, kModeFinal = 6, kModeFxaa = 7, kModeWaterCopy = 8;

struct ModernConstants {
    std::uint32_t textures[8]{};
    glm::vec4 params[8]{};
    glm::mat4 invViewProj{1.0f};
    glm::mat4 viewProj{1.0f};
};
static_assert(sizeof(ModernConstants) == 288);

struct Push {
    std::uint64_t constants = 0;
    std::uint32_t mode = 0;
    std::uint32_t pad = 0;
};

} // namespace

bool ModernPass::Init(rhi::Device& device, std::string& error) {
    device_ = &device;
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = Spirv(kModernVertexShader);
    desc.fragmentShader = Spirv(kModernPixelShader);
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = "PSMain";
    desc.colorFormat = rhi::Format::RGBA16Float;
    desc.debugName = "modern (hdr)";
    hdr_ = device.CreateGraphicsPipeline(desc);
    desc.blend = rhi::BlendMode::Additive;
    desc.debugName = "modern (bloom up)";
    additive_ = device.CreateGraphicsPipeline(desc);
    desc.blend = rhi::BlendMode::Opaque;
    desc.colorFormat = rhi::Format::RGBA8Unorm;
    desc.debugName = "modern (8-bit)";
    ldr_ = device.CreateGraphicsPipeline(desc);
    rhi::GraphicsPipelineDesc copy;
    copy.vertexShader = Spirv(kModernVertexShader);
    copy.fragmentShader = Spirv(kDepthCopyPixelShader);
    copy.vertexEntry = "VSMain";
    copy.fragmentEntry = "PSDepthCopy";
    copy.depthOnly = true;
    copy.depthFormat = rhi::Format::D32Float;
    copy.depthTest = true;
    copy.depthWrite = true;
    copy.debugName = "depth copy";
    depthCopy_ = device.CreateGraphicsPipeline(copy);
    if (!hdr_ || !additive_ || !ldr_ || !depthCopy_) {
        error = "cannot create the modern passes";
        return false;
    }
    return true;
}

void ModernPass::Shutdown() {
    if (device_ == nullptr) return;
    for (rhi::PipelineHandle p : {hdr_, additive_, ldr_, depthCopy_}) device_->DestroyPipeline(p);
    for (rhi::TextureHandle t : {occlusion_, occlusionBlur_, shafts_, fxaaSource_}) device_->DestroyTexture(t);
    for (rhi::TextureHandle t : bloom_) device_->DestroyTexture(t);
    device_ = nullptr;
}

void ModernPass::EnsureTargets(std::uint32_t width, std::uint32_t height) {
    if (width == width_ && height == height_ && occlusion_) return;
    for (rhi::TextureHandle t : {occlusion_, occlusionBlur_, shafts_, fxaaSource_}) device_->DestroyTexture(t);
    for (rhi::TextureHandle t : bloom_) device_->DestroyTexture(t);
    rhi::TextureDesc d;
    d.usage = rhi::TextureUsage::RenderTarget;
    d.format = rhi::Format::RGBA8Unorm;
    d.width = std::max(1u, width / 2);
    d.height = std::max(1u, height / 2);
    d.debugName = "modern: occlusion";
    occlusion_ = device_->CreateTexture(d);
    d.debugName = "modern: occlusion blurred";
    occlusionBlur_ = device_->CreateTexture(d);
    d.width = width;
    d.height = height;
    d.debugName = "modern: before FXAA";
    fxaaSource_ = device_->CreateTexture(d);
    d.format = rhi::Format::RGBA16Float;
    d.width = std::max(1u, width / 4);
    d.height = std::max(1u, height / 4);
    d.debugName = "modern: sun shafts";
    shafts_ = device_->CreateTexture(d);
    for (int i = 0; i < kBloomLevels; ++i) {
        d.width = std::max(1u, width >> (i + 1));
        d.height = std::max(1u, height >> (i + 1));
        d.debugName = "modern: bloom";
        bloom_[std::size_t(i)] = device_->CreateTexture(d);
    }
    width_ = width;
    height_ = height;
}

RenderGraph::Resource ModernPass::AddPasses(RenderGraph& graph, RenderGraph::Resource sceneResource, RenderGraph::Resource depthResource,
                                            const Frame& frame) {
    rhi::Device& device = *device_;
    EnsureTargets(frame.width, frame.height);
    const Settings& s = frame.settings;
    // A full-screen pass of `mode` into `target` (its size) with these constants.
    const auto fullscreen = [&device](rhi::CommandList& cmd, rhi::PipelineHandle pipeline, rhi::TextureHandle target, std::uint32_t mode,
                                      const ModernConstants& constants, bool load) {
        const rhi::TransientAllocation alloc = device.AllocateTransient(sizeof(constants), 16);
        if (alloc.cpu == nullptr) return;
        std::memcpy(alloc.cpu, &constants, sizeof(constants));
        rhi::RenderPassDesc pass;
        pass.color = target;
        pass.loadColor = load;
        cmd.BeginRenderPass(pass);
        cmd.BindPipeline(pipeline);
        Push push;
        push.constants = alloc.gpuAddress;
        push.mode = mode;
        cmd.PushConstants(&push, sizeof(push));
        cmd.Draw(3, 0);
        cmd.EndPass();
    };
    ModernConstants base;
    base.textures[0] = device.BindlessIndex(frame.scene);
    base.textures[1] = device.BindlessIndex(frame.depth);
    base.viewProj = frame.viewProj;
    base.invViewProj = glm::inverse(frame.viewProj);
    base.params[5] = glm::vec4(frame.eye, 0.0f);

    // Ambient occlusion: half resolution, then blurred across what is one surface.
    RenderGraph::Resource occlusion = 0;
    if (s.occlusion > 0) {
        const auto raw = graph.ImportTexture(occlusion_, "modern: occlusion");
        occlusion = graph.ImportTexture(occlusionBlur_, "modern: occlusion blurred");
        ModernConstants c = base;
        c.params[0] = glm::vec4(s.occlusion >= 2 ? 9.0f : 6.0f, s.occlusion >= 2 ? 1.4f : 1.1f, 2.0f / float(frame.width), 2.0f / float(frame.height));
        graph.AddPass("modern: occlusion", PassKind::Graphics, [&](RenderGraph::Builder& b) { b.Read(depthResource, Use::Sampled).Write(raw, Use::ColorWrite); },
                      [this, fullscreen, c](rhi::CommandList& cmd, const RenderGraph&) { fullscreen(cmd, ldr_, occlusion_, kModeOcclusion, c, false); });
        ModernConstants blur = c;
        blur.textures[2] = device.BindlessIndex(occlusion_);
        graph.AddPass("modern: occlusion blur", PassKind::Graphics,
                      [&](RenderGraph::Builder& b) { b.Read(raw, Use::Sampled).Read(depthResource, Use::Sampled).Write(occlusion, Use::ColorWrite); },
                      [this, fullscreen, blur](rhi::CommandList& cmd, const RenderGraph&) {
                          fullscreen(cmd, ldr_, occlusionBlur_, kModeOcclusionBlur, blur, false);
                      });
    }

    // Sun shafts: only while the sun is above the horizon and near the screen.
    RenderGraph::Resource shafts = 0;
    if (s.shafts && frame.sunWeight > 0.0f) {
        const glm::vec4 clip = frame.viewProj * glm::vec4(frame.eye + glm::normalize(frame.sunDirection) * 10000.0f, 1.0f);
        if (clip.w > 0.0f) {
            const glm::vec2 uv = glm::vec2(clip.x, clip.y) / clip.w * 0.5f + 0.5f;
            const float off = std::max({0.0f, -uv.x, uv.x - 1.0f, -uv.y, uv.y - 1.0f});
            if (off < 0.6f) {
                shafts = graph.ImportTexture(shafts_, "modern: sun shafts");
                ModernConstants c = base;
                c.params[1] = glm::vec4(uv, (1.0f - off / 0.6f) * frame.sunWeight, float(frame.width) / float(std::max(1u, frame.height)));
                c.params[2] = glm::vec4(frame.sunColour, 0.0f);
                graph.AddPass("modern: sun shafts", PassKind::Graphics,
                              [&](RenderGraph::Builder& b) { b.Read(sceneResource, Use::Sampled).Read(depthResource, Use::Sampled).Write(shafts, Use::ColorWrite); },
                              [this, fullscreen, c](rhi::CommandList& cmd, const RenderGraph&) { fullscreen(cmd, hdr_, shafts_, kModeShafts, c, false); });
            }
        }
    }

    // Bloom: five halvings, then back up, each level adding the smaller one.
    RenderGraph::Resource bloom = 0;
    if (s.bloom) {
        std::array<RenderGraph::Resource, kBloomLevels> down{}, up{};
        for (int i = 0; i < kBloomLevels; ++i) down[std::size_t(i)] = graph.ImportTexture(bloom_[std::size_t(i)], "modern: bloom down");
        for (int i = 0; i < kBloomLevels; ++i) {
            ModernConstants c = base;
            const rhi::TextureHandle source = i == 0 ? frame.scene : bloom_[std::size_t(i - 1)];
            const std::uint32_t sw = i == 0 ? frame.width : std::max(1u, frame.width >> i), sh = i == 0 ? frame.height : std::max(1u, frame.height >> i);
            c.textures[0] = device.BindlessIndex(source);
            c.params[0] = glm::vec4(0, 0, 1.0f / float(sw), 1.0f / float(sh));
            c.params[1] = glm::vec4(0, 0, 0, 1.0f); // the threshold: only light above white (windows, effects)
            const RenderGraph::Resource from = i == 0 ? sceneResource : down[std::size_t(i - 1)];
            graph.AddPass("modern: bloom down", PassKind::Graphics,
                          [&, from, i](RenderGraph::Builder& b) { b.Read(from, Use::Sampled).Write(down[std::size_t(i)], Use::ColorWrite); },
                          [this, fullscreen, c, i](rhi::CommandList& cmd, const RenderGraph&) {
                              fullscreen(cmd, hdr_, bloom_[std::size_t(i)], i == 0 ? kModeBloomFirst : kModeBloomDown, c, false);
                          });
        }
        RenderGraph::Resource last = down[kBloomLevels - 1];
        for (int i = kBloomLevels - 2; i >= 0; --i) {
            up[std::size_t(i)] = graph.ImportTexture(bloom_[std::size_t(i)], "modern: bloom up");
            ModernConstants c = base;
            c.textures[0] = device.BindlessIndex(bloom_[std::size_t(i + 1)]);
            c.params[0] = glm::vec4(0, 0, 1.0f / float(std::max(1u, frame.width >> (i + 2))), 1.0f / float(std::max(1u, frame.height >> (i + 2))));
            const RenderGraph::Resource from = last;
            graph.AddPass("modern: bloom up", PassKind::Graphics,
                          [&, from, i](RenderGraph::Builder& b) {
                              b.Read(from, Use::Sampled).Read(down[std::size_t(i)], Use::Sampled).Write(up[std::size_t(i)], Use::ColorWrite);
                          },
                          [this, fullscreen, c, i](rhi::CommandList& cmd, const RenderGraph&) {
                              fullscreen(cmd, additive_, bloom_[std::size_t(i)], kModeBloomUp, c, true);
                          });
            last = up[std::size_t(i)];
        }
        bloom = last;
    }

    // The last step: everything into the 8-bit picture (through FXAA when it is on).
    const auto output = graph.ImportTexture(frame.output, "modern: picture");
    const auto before = s.fxaa ? graph.ImportTexture(fxaaSource_, "modern: before FXAA") : output;
    ModernConstants c = base;
    c.textures[2] = occlusion != 0 ? device.BindlessIndex(occlusionBlur_) : 0;
    c.textures[3] = bloom != 0 ? device.BindlessIndex(bloom_[0]) : 0;
    c.textures[4] = shafts != 0 ? device.BindlessIndex(shafts_) : 0;
    c.params[3] = glm::vec4(float(s.toneMapping), 1.0f / std::max(0.1f, s.gamma), s.grading ? 0.8f : 0.0f, s.vignette ? 0.18f : 0.0f);
    c.params[4] = glm::vec4(frame.timeOfDay, occlusion != 0 ? 1.0f : 0.0f, bloom != 0 ? 0.08f : 0.0f, shafts != 0 ? 1.0f : 0.0f);
    if (s.heightFog > 0) {
        // Density at the ground, then the height it halves over; the ground about the camera's
        // foot (60 below the eye, as the follow camera stands).
        c.params[6] = glm::vec4(frame.fogColour, s.heightFog >= 2 ? 0.00035f : 0.0002f);
        // The sun's light in the fog toward it, weighed by its height (fading with the sunset, not switched off).
        const glm::vec3 toSun = frame.sunWeight > 0.0f ? glm::normalize(frame.sunDirection) : glm::vec3(0.0f);
        c.params[7] = glm::vec4(toSun, frame.eye.y - 60.0f);
        c.params[2] = glm::vec4(frame.sunColour * frame.sunWeight, 0.0f);
        c.textures[1] = device.BindlessIndex(frame.depth);
    }
    const rhi::TextureHandle finalTarget = s.fxaa ? fxaaSource_ : frame.output;
    graph.AddPass("modern: final", PassKind::Graphics,
                  [&](RenderGraph::Builder& b) {
                      b.Read(sceneResource, Use::Sampled).Write(before, Use::ColorWrite);
                      if (occlusion != 0) b.Read(occlusion, Use::Sampled);
                      if (bloom != 0) b.Read(bloom, Use::Sampled);
                      if (shafts != 0) b.Read(shafts, Use::Sampled);
                      if (s.heightFog > 0) b.Read(depthResource, Use::Sampled);
                  },
                  [this, fullscreen, c, finalTarget](rhi::CommandList& cmd, const RenderGraph&) {
                      SR_ZONE("render: final");
                      fullscreen(cmd, ldr_, finalTarget, kModeFinal, c, false);
                  });
    if (s.fxaa) {
        ModernConstants f = base;
        f.textures[0] = device.BindlessIndex(fxaaSource_);
        f.params[0] = glm::vec4(0, 0, 1.0f / float(frame.width), 1.0f / float(frame.height));
        graph.AddPass("modern: FXAA", PassKind::Graphics, [&](RenderGraph::Builder& b) { b.Read(before, Use::Sampled).Write(output, Use::ColorWrite); },
                      [this, fullscreen, f, out = frame.output](rhi::CommandList& cmd, const RenderGraph&) { fullscreen(cmd, ldr_, out, kModeFxaa, f, false); });
    }
    return output;
}

void ModernPass::CopyForWater(rhi::CommandList& cmd, rhi::TextureHandle scene, rhi::TextureHandle depth, rhi::TextureHandle target,
                              const glm::mat4& viewProj) const {
    ModernConstants c;
    c.textures[0] = device_->BindlessIndex(scene);
    c.textures[1] = device_->BindlessIndex(depth);
    c.viewProj = viewProj;
    c.invViewProj = glm::inverse(viewProj);
    const rhi::TransientAllocation alloc = device_->AllocateTransient(sizeof(c), 16);
    if (alloc.cpu == nullptr) return;
    std::memcpy(alloc.cpu, &c, sizeof(c));
    rhi::RenderPassDesc pass;
    pass.color = target;
    cmd.BeginRenderPass(pass);
    cmd.BindPipeline(hdr_);
    Push push;
    push.constants = alloc.gpuAddress;
    push.mode = kModeWaterCopy;
    cmd.PushConstants(&push, sizeof(push));
    cmd.Draw(3, 0);
    cmd.EndPass();
}

void ModernPass::CopyDepth(rhi::CommandList& cmd, rhi::TextureHandle source) const {
    ModernConstants c;
    c.textures[0] = device_->BindlessIndex(source);
    const rhi::TransientAllocation alloc = device_->AllocateTransient(sizeof(c), 16);
    if (alloc.cpu == nullptr) return;
    std::memcpy(alloc.cpu, &c, sizeof(c));
    cmd.BindPipeline(depthCopy_);
    Push push;
    push.constants = alloc.gpuAddress;
    cmd.PushConstants(&push, sizeof(push));
    cmd.Draw(3, 0);
}

} // namespace sr::gfx
