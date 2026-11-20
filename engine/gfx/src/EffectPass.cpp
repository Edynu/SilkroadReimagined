#include "sr/gfx/EffectPass.h"

#include <cstring>

#include "kEffectMeshVertexShader.spv.h"
#include "kEffectPixelShader.spv.h"
#include "kEffectRibbonPixelShader.spv.h"
#include "kEffectRibbonVertexShader.spv.h"
#include "kEffectSpriteVertexShader.spv.h"
#include "sr/core/Trace.h"
#include "sr/gfx/GeometryPool.h"

namespace sr::gfx {

namespace {

template <std::size_t N>
std::span<const std::byte> Spirv(const unsigned char (&code)[N]) {
    return {reinterpret_cast<const std::byte*>(code), N};
}

// Effect.hlsl's push constants.
struct Push {
    glm::mat4 viewProj{1.0f};
    std::uint64_t records = 0;
    std::uint64_t vertices = 0;
    std::uint32_t vertexFlags = 0;
    float glow = 1.0f; // the effects' brightness (ours; 1 the original's)
    std::uint32_t pad[2]{};
};
static_assert(sizeof(Push) == 96);

rhi::BlendMode Blending(EffectBlend blend) {
    switch (blend) {
    case EffectBlend::Premultiplied: return rhi::BlendMode::PremultipliedAlpha;
    case EffectBlend::Additive: return rhi::BlendMode::Additive;
    default: return rhi::BlendMode::AlphaBlend;
    }
}

} // namespace

bool EffectPass::Init(rhi::Device& device, rhi::Format color, rhi::Format depth, std::string& error) {
    device_ = &device;
    rhi::GraphicsPipelineDesc desc;
    desc.fragmentShader = Spirv(kEffectPixelShader);
    desc.fragmentEntry = "PSMain";
    desc.colorFormat = color;
    desc.depthFormat = depth;
    desc.depthTest = true;
    desc.depthWrite = false;
    desc.cull = rhi::CullMode::None;
    for (std::size_t b = 0; b < kEffectBlends; ++b) {
        desc.blend = Blending(static_cast<EffectBlend>(b));
        desc.vertexShader = Spirv(kEffectSpriteVertexShader);
        desc.vertexEntry = "VSSprite";
        desc.debugName = "effect sprites";
        sprites_[b] = device.CreateGraphicsPipeline(desc);
        desc.vertexShader = Spirv(kEffectMeshVertexShader);
        desc.vertexEntry = "VSMesh";
        desc.debugName = "effect meshes";
        meshes_[b] = device.CreateGraphicsPipeline(desc);
        rhi::GraphicsPipelineDesc ribbon = desc;
        ribbon.vertexShader = Spirv(kEffectRibbonVertexShader);
        ribbon.vertexEntry = "VSRibbon";
        ribbon.fragmentShader = Spirv(kEffectRibbonPixelShader);
        ribbon.fragmentEntry = "PSRibbon";
        ribbon.debugName = "effect ribbons";
        ribbons_[b] = device.CreateGraphicsPipeline(ribbon);
        if (!sprites_[b] || !meshes_[b] || !ribbons_[b]) {
            error = "effect pipelines could not be created";
            return false;
        }
    }
    return true;
}

void EffectPass::Shutdown() {
    if (device_ == nullptr) return;
    for (std::size_t b = 0; b < kEffectBlends; ++b) {
        if (sprites_[b]) device_->DestroyPipeline(sprites_[b]);
        if (meshes_[b]) device_->DestroyPipeline(meshes_[b]);
        if (ribbons_[b]) device_->DestroyPipeline(ribbons_[b]);
        sprites_[b] = meshes_[b] = ribbons_[b] = {};
    }
    device_ = nullptr;
}

void EffectPass::AddPasses(RenderGraph& graph, RenderGraph::Resource color, RenderGraph::Resource geometry, const GeometryPool& pool,
                           const Frame& frame) {
    if (device_ == nullptr || frame.effects == nullptr || frame.effects->Empty() || !frame.color || !frame.depth) return;
    frame_ = frame;
    pool_ = &pool;
    graph.AddPass("scene: effects", PassKind::Graphics,
                  [&](RenderGraph::Builder& b) {
                      b.Read(geometry, Use::VertexRead).Read(geometry, Use::IndexRead).Write(color, Use::ColorWrite);
                  },
                  [this](rhi::CommandList& cmd, const RenderGraph&) { Draw(cmd); });
}

void EffectPass::Draw(rhi::CommandList& cmd) {
    SR_ZONE("render: effects");
    const EffectFrame& effects = *frame_.effects;
    // The frame's records into transient memory, read by address.
    const auto upload = [&](const void* data, std::size_t bytes) -> std::uint64_t {
        if (bytes == 0) return 0;
        const rhi::TransientAllocation alloc = device_->AllocateTransient(bytes, 16);
        if (alloc.cpu == nullptr) return 0;
        std::memcpy(alloc.cpu, data, bytes);
        return alloc.gpuAddress;
    };
    const std::uint64_t sprites = upload(effects.sprites.data(), effects.sprites.size() * sizeof(EffectSprite));
    const std::uint64_t instances = upload(effects.meshInstances.data(), effects.meshInstances.size() * sizeof(EffectMeshInstance));
    const std::uint64_t ribbons = upload(effects.ribbons.data(), effects.ribbons.size() * sizeof(EffectRibbonVertex));

    rhi::RenderPassDesc pass;
    pass.color = frame_.color;
    pass.depth = frame_.depth;
    pass.loadColor = true;
    pass.loadDepth = true;
    cmd.BeginRenderPass(pass);
    Push push;
    push.viewProj = frame_.viewProj;
    push.glow = glow_;
    std::uint32_t firstSprite = 0, firstRibbon = 0;
    std::size_t draw = 0;
    bool indexed = false;
    for (std::size_t b = 0; b < kEffectBlends; ++b) {
        // The blend's meshes, a draw per part, then its sprites in one draw.
        bool bound = false;
        for (; draw < effects.meshDraws.size() && static_cast<std::size_t>(effects.meshDraws[draw].blend) == b; ++draw) {
            const EffectMeshDraw& d = effects.meshDraws[draw];
            if (instances == 0 || d.instanceCount == 0) continue;
            if (!bound) cmd.BindPipeline(meshes_[b]);
            bound = true;
            if (!indexed) cmd.BindIndexBuffer(pool_->Buffer(), 0, false);
            indexed = true;
            push.records = instances + std::uint64_t{d.firstInstance} * sizeof(EffectMeshInstance);
            push.vertices = pool_->Address() + d.vertexOffset;
            push.vertexFlags = d.vertexFlags;
            cmd.PushConstants(&push, sizeof(push));
            cmd.DrawIndexedInstanced(d.indexCount, d.instanceCount, d.firstIndex, 0);
        }
        // Its ribbons: their vertices as they are.
        if (const std::uint32_t vertices = effects.ribbonCounts[b]; vertices > 0 && ribbons != 0) {
            cmd.BindPipeline(ribbons_[b]);
            push.records = ribbons + std::uint64_t{firstRibbon} * sizeof(EffectRibbonVertex);
            push.vertices = 0;
            push.vertexFlags = 0;
            cmd.PushConstants(&push, sizeof(push));
            cmd.Draw(vertices, 0);
        }
        firstRibbon += effects.ribbonCounts[b];
        const std::uint32_t count = effects.spriteCounts[b];
        if (count > 0 && sprites != 0) {
            cmd.BindPipeline(sprites_[b]);
            push.records = sprites + std::uint64_t{firstSprite} * sizeof(EffectSprite);
            push.vertices = 0;
            push.vertexFlags = 0;
            cmd.PushConstants(&push, sizeof(push));
            cmd.Draw(count * 6, 0);
        }
        firstSprite += count;
    }
    cmd.EndPass();
}

} // namespace sr::gfx
