#include "sr/gfx/ModelPass.h"

#include "kModelPixelShader.spv.h"
#include "kModelVertexShader.spv.h"

namespace sr::gfx {

namespace {

template <std::size_t N>
std::span<const std::byte> Spirv(const unsigned char (&code)[N]) {
    return {reinterpret_cast<const std::byte*>(code), N};
}

struct ModelPush {
    glm::mat4 viewProj;
    std::uint64_t geometry = 0;
    std::uint64_t instances = 0;
    std::uint64_t palette = 0;
    std::uint32_t vertexOffset = 0;
    std::uint32_t vertexFlags = 0;
    std::uint32_t instance = 0;
    std::uint32_t texture = 0;
    std::uint32_t material = 0;
    std::uint32_t pad = 0;
};
static_assert(sizeof(ModelPush) <= 128);

} // namespace

bool ModelPass::Init(rhi::Device& device, rhi::Format color, rhi::Format depth, std::string& error) {
    device_ = &device;
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = Spirv(kModelVertexShader);
    desc.fragmentShader = Spirv(kModelPixelShader);
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = "PSMain";
    desc.depthTest = true;
    desc.depthWrite = true;
    desc.colorFormat = color;
    desc.depthFormat = depth;
    desc.debugName = "streamed models";
    pipeline_ = device.CreateGraphicsPipeline(desc);
    if (!pipeline_) {
        error = "cannot create the model pipeline";
        return false;
    }
    return true;
}

void ModelPass::Shutdown() {
    if (device_ != nullptr && pipeline_) device_->DestroyPipeline(pipeline_);
    pipeline_ = {};
}

void ModelPass::Record(rhi::CommandList& cmd, const glm::mat4& viewProj, std::span<const Item> items,
                       const GeometryPool& pool, const GpuScene& scene, std::uint64_t palette) const {
    if (items.empty()) return;
    cmd.BindPipeline(pipeline_);
    cmd.BindIndexBuffer(pool.Buffer(), 0, false);
    ModelPush push;
    push.viewProj = viewProj;
    push.geometry = pool.Address();
    push.instances = scene.Address();
    push.palette = palette;
    for (const Item& item : items) {
        if (item.model == nullptr) continue;
        push.instance = item.instance;
        for (const ModelPartInfo& part : item.model->parts) {
            if (part.indexCount == 0) continue;
            push.vertexOffset = static_cast<std::uint32_t>(part.vertexOffset);
            // Without bones the vertices stay in their bind pose (the stride still counts the skin).
            push.vertexFlags = palette != 0 && item.skinned ? part.vertexFlags : part.vertexFlags | 4u;
            push.texture = part.textureBindless;
            push.material = part.materialFlags;
            cmd.PushConstants(&push, sizeof(push));
            cmd.DrawIndexed(part.indexCount, static_cast<std::uint32_t>(part.indexOffset / 2), 0);
        }
    }
}

} // namespace sr::gfx
