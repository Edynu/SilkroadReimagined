#include "sr/gfx/PortraitPass.h"

#include <algorithm>
#include <cstring>

#include "kPortraitPixelShader.spv.h"
#include "kPortraitVertexShader.spv.h"
#include "sr/core/Trace.h"
#include "sr/gfx/GpuScene.h"

namespace sr::gfx {

namespace {

template <std::size_t N>
std::span<const std::byte> Spirv(const unsigned char (&code)[N]) {
    return {reinterpret_cast<const std::byte*>(code), N};
}

struct PortraitPush {
    glm::mat4 viewProj;
    std::uint64_t geometry = 0;
    std::uint64_t instance = 0;
    std::uint64_t palette = 0;
    std::uint32_t vertexOffset = 0;
    std::uint32_t vertexFlags = 0;
    std::uint32_t texture = 0;
    std::uint32_t material = 0;
};
static_assert(sizeof(PortraitPush) <= 128);

} // namespace

bool PortraitPass::Init(rhi::Device& device, std::string& error) {
    device_ = &device;
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = Spirv(kPortraitVertexShader);
    desc.fragmentShader = Spirv(kPortraitPixelShader);
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = "PSMain";
    desc.depthTest = true;
    desc.depthWrite = true;
    desc.colorFormat = rhi::Format::RGBA8Unorm;
    desc.depthFormat = rhi::Format::D32Float;
    desc.debugName = "portraits";
    pipeline_ = device.CreateGraphicsPipeline(desc);
    if (!pipeline_) {
        error = "cannot create the portrait pipeline";
        return false;
    }
    return true;
}

void PortraitPass::Shutdown() {
    if (device_ != nullptr && pipeline_) device_->DestroyPipeline(pipeline_);
    pipeline_ = {};
    kept_.clear();
    frame_.clear();
    device_ = nullptr;
}

void PortraitPass::Keep(std::span<const PortraitDraw> draws) {
    for (const PortraitDraw& d : draws) {
        std::erase_if(kept_, [&](const PortraitDraw& k) { return k.color == d.color; });
        kept_.push_back(d);
    }
}

std::span<const RenderGraph::Resource> PortraitPass::AddPasses(RenderGraph& graph, std::span<const PortraitDraw> draws, AssetStore& assets) {
    results_.clear();
    frame_.clear();
    // The kept ones first, unless this frame draws the same target again.
    for (const PortraitDraw& k : kept_) {
        if (std::none_of(draws.begin(), draws.end(), [&](const PortraitDraw& d) { return d.color == k.color; })) frame_.push_back(k);
    }
    kept_.clear();
    frame_.insert(frame_.end(), draws.begin(), draws.end());
    for (const PortraitDraw& draw : frame_) {
        const RenderGraph::Resource color = graph.ImportTexture(draw.color, "portrait");
        const RenderGraph::Resource depth = graph.ImportTexture(draw.depth, "portrait depth");
        graph.AddPass("portrait", PassKind::Graphics,
                      [&](RenderGraph::Builder& b) { b.Write(color, Use::ColorWrite).Write(depth, Use::DepthWrite); },
                      [this, &draw, &assets](rhi::CommandList& cmd, const RenderGraph&) { Draw(cmd, draw, assets); });
        results_.push_back(color);
    }
    return results_;
}

void PortraitPass::Draw(rhi::CommandList& cmd, const PortraitDraw& draw, AssetStore& assets) const {
    SR_ZONE("render: portrait");
    rhi::RenderPassDesc pass;
    pass.color = draw.color;
    pass.depth = draw.depth;
    // Opaque black behind the head, the depth at its far end.
    const float clear[4] = {0, 0, 0, 1};
    std::memcpy(pass.clearColor, clear, sizeof(clear));
    cmd.BeginRenderPass(pass);
    const std::shared_ptr<const ModelInfo> model = assets.Model(draw.model);
    const std::size_t paletteBytes = draw.palette.size() * sizeof(glm::mat4);
    const rhi::TransientAllocation alloc = model ? device_->AllocateTransient(sizeof(InstanceRecord) + paletteBytes, 64) : rhi::TransientAllocation{};
    if (model && alloc.cpu != nullptr) {
        // Its instance record (the world rows, its palette from the start), then the palette.
        InstanceRecord record;
        std::memcpy(record.world, draw.world.data(), sizeof(record.world));
        record.paletteOffset = 0;
        record.flags = draw.palette.empty() ? 0u : kInstanceSkinned;
        std::memcpy(alloc.cpu, &record, sizeof(record));
        if (paletteBytes > 0) std::memcpy(static_cast<std::byte*>(alloc.cpu) + sizeof(record), draw.palette.data(), paletteBytes);
        GeometryPool& pool = assets.Geometry();
        cmd.BindPipeline(pipeline_);
        cmd.BindIndexBuffer(pool.Buffer(), 0, false);
        PortraitPush push;
        push.viewProj = draw.viewProj;
        push.geometry = pool.Address();
        push.instance = alloc.gpuAddress;
        push.palette = alloc.gpuAddress + sizeof(record);
        for (const ModelPartInfo& part : model->parts) {
            if (part.indexCount == 0) continue;
            push.vertexOffset = static_cast<std::uint32_t>(part.vertexOffset);
            // Without a palette the vertices stay in their bind pose.
            push.vertexFlags = draw.palette.empty() ? part.vertexFlags | 4u : part.vertexFlags;
            push.texture = part.textureBindless;
            push.material = part.materialFlags;
            cmd.PushConstants(&push, sizeof(push));
            cmd.DrawIndexed(part.indexCount, static_cast<std::uint32_t>(part.indexOffset / 2), 0);
        }
    }
    cmd.EndPass();
}

} // namespace sr::gfx
