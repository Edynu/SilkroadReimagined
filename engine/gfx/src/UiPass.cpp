#include "sr/gfx/UiPass.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "kGameUiPixelShader.spv.h"
#include "kGameUiVertexShader.spv.h"
#include "sr/core/Trace.h"

namespace sr::gfx {

namespace {

template <std::size_t N>
std::span<const std::byte> Spirv(const unsigned char (&code)[N]) {
    return {reinterpret_cast<const std::byte*>(code), N};
}

struct Push {
    float scale[2];
    float translate[2];
    std::uint32_t texture = 0;
    std::uint32_t flags = 0; // bits 0-3 sampler, 16 premultiplied alpha, 17 ignore alpha
    std::uint32_t mask = 0;  // a mask texture (0: none) over maskRect
    std::uint32_t depthTexture = 0; // the scene's depth (0: no test)
    float maskRect[4]{};
    float depth = 0; // the command's world depth (0: no test)
    float pad[3]{};
};
static_assert(sizeof(Push) == 64);

} // namespace

bool UiPass::Init(rhi::Device& device, std::string& error) {
    device_ = &device;
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = Spirv(kGameUiVertexShader);
    desc.fragmentShader = Spirv(kGameUiPixelShader);
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = "PSMain";
    desc.vertexStride = sizeof(UiVertex);
    desc.attributes = {
        {0, rhi::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(UiVertex, x))},
        {1, rhi::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(UiVertex, u))},
        {2, rhi::VertexFormat::RGBA8Unorm, static_cast<std::uint32_t>(offsetof(UiVertex, color))},
    };
    desc.blend = rhi::BlendMode::AlphaBlend;
    desc.debugName = "game ui";
    pipeline_ = device.CreateGraphicsPipeline(desc);
    desc.blend = rhi::BlendMode::Additive;
    desc.debugName = "game ui (additive)";
    additivePipeline_ = device.CreateGraphicsPipeline(desc);
    if (!pipeline_ || !additivePipeline_) {
        error = "cannot create the game UI pipeline";
        return false;
    }
    return true;
}

void UiPass::Shutdown() {
    if (device_ == nullptr) return;
    device_->DestroyPipeline(pipeline_);
    device_->DestroyPipeline(additivePipeline_);
    device_ = nullptr;
}

void UiPass::Draw(rhi::CommandList& cmd, const UiFrame& frame, float width, float height, std::uint32_t sceneOverlay,
                  std::uint32_t sceneDepth) const {
    if (frame.Empty() || width <= 0 || height <= 0) return;
    SR_ZONE("render: game ui");
    const std::size_t vertexBytes = frame.vertices.size() * sizeof(UiVertex);
    const std::size_t indexStart = (vertexBytes + 15) & ~std::size_t{15};
    const rhi::TransientAllocation alloc = device_->AllocateTransient(indexStart + frame.indices.size() * sizeof(std::uint32_t), 16);
    if (alloc.cpu == nullptr) return;
    std::memcpy(alloc.cpu, frame.vertices.data(), vertexBytes);
    std::memcpy(static_cast<std::byte*>(alloc.cpu) + indexStart, frame.indices.data(), frame.indices.size() * sizeof(std::uint32_t));
    cmd.BindPipeline(pipeline_);
    cmd.BindVertexBuffer(alloc.buffer, alloc.offset);
    cmd.BindIndexBuffer(alloc.buffer, alloc.offset + indexStart, true);
    Push push{};
    push.scale[0] = 2.0f / width;
    push.scale[1] = 2.0f / height;
    push.translate[0] = -1.0f;
    push.translate[1] = -1.0f;
    bool additive = false;
    for (const UiCommand& dc : frame.commands) {
        const float x0 = std::max(dc.clip[0], 0.0f), y0 = std::max(dc.clip[1], 0.0f);
        const float x1 = std::min(dc.clip[0] + dc.clip[2], width), y1 = std::min(dc.clip[1] + dc.clip[3], height);
        if (x1 <= x0 || y1 <= y0 || dc.indexCount == 0) continue;
        std::uint32_t texture = dc.texture;
        if (texture == kSceneOverlayTexture) {
            if (sceneOverlay == 0) continue;
            texture = sceneOverlay;
        }
        if (dc.additive != additive) {
            additive = dc.additive;
            cmd.BindPipeline(additive ? additivePipeline_ : pipeline_);
        }
        cmd.SetScissor({static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0), static_cast<std::uint32_t>(x1 - x0),
                        static_cast<std::uint32_t>(y1 - y0)});
        push.texture = texture;
        push.flags = static_cast<std::uint32_t>(rhi::SamplerIndex::LinearClamp) | (dc.premultipliedAlpha ? 0x10000u : 0u) |
                     (dc.opaque ? 0x20000u : 0u);
        push.mask = dc.mask;
        push.depth = dc.depth;
        push.depthTexture = dc.depth > 0.0f ? sceneDepth : 0u;
        std::copy(std::begin(dc.maskRect), std::end(dc.maskRect), push.maskRect);
        cmd.PushConstants(&push, sizeof(push));
        cmd.DrawIndexed(dc.indexCount, dc.indexOffset, 0);
    }
    // The next draws in the pass get the whole target again.
    cmd.SetScissor({0, 0, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)});
}

} // namespace sr::gfx
