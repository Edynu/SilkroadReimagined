#include "sr/render/ParticleRenderer.h"

#include <cstddef>
#include <algorithm>
#include <cstring>

#include "kParticlePixelShader.spv.h"
#include "kParticleVertexShader.spv.h"

namespace sr::render {

namespace {

struct PushData {
    glm::mat4 viewProj;
    std::uint32_t textureIndex;
    std::uint32_t flags;
    std::uint32_t depthIndex;
    float softness;
    float rcpSize[2];
    float nearPlane;
    float farPlane;
};
static_assert(sizeof(PushData) <= 128);

} // namespace

bool ParticleRenderer::Init(rhi::Device& device, rhi::Format colorFormat, rhi::Format depthFormat, std::string& error) {
    device_ = &device;
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = std::as_bytes(std::span(kParticleVertexShader));
    desc.fragmentShader = std::as_bytes(std::span(kParticlePixelShader));
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = "PSMain";
    desc.vertexStride = sizeof(ParticleVertex);
    desc.attributes = {
        {0, rhi::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(ParticleVertex, position))},
        {1, rhi::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(ParticleVertex, uv))},
        {2, rhi::VertexFormat::RGBA8Unorm, static_cast<std::uint32_t>(offsetof(ParticleVertex, color))},
    };
    desc.cull = rhi::CullMode::None;
    desc.depthTest = true;
    desc.depthWrite = false;
    desc.colorFormat = colorFormat;
    desc.depthFormat = depthFormat;
    desc.blend = rhi::BlendMode::AlphaBlend;
    desc.debugName = "particles alpha";
    alpha_ = device.CreateGraphicsPipeline(desc);
    desc.blend = rhi::BlendMode::Additive;
    desc.debugName = "particles additive";
    additive_ = device.CreateGraphicsPipeline(desc);
    if (!alpha_ || !additive_) {
        error = "particle pipelines failed";
        return false;
    }
    return true;
}

void ParticleRenderer::Shutdown() {
    if (device_ == nullptr) return;
    if (alpha_) device_->DestroyPipeline(alpha_);
    if (additive_) device_->DestroyPipeline(additive_);
    alpha_ = additive_ = {};
}

void ParticleRenderer::Draw(rhi::CommandList& cmd, const glm::mat4& viewProj, std::span<const ParticleVertex> vertices,
                            std::span<const ParticleBatch> batches, const SoftDepth* soft) const {
    if (device_ == nullptr || vertices.empty() || batches.empty() || !alpha_) return;
    const std::size_t bytes = vertices.size_bytes();
    const rhi::TransientAllocation alloc = device_->AllocateTransient(bytes, 16);
    if (alloc.cpu == nullptr) return;
    std::memcpy(alloc.cpu, vertices.data(), bytes);
    cmd.BindVertexBuffer(alloc.buffer, alloc.offset);
    rhi::PipelineHandle bound{};
    for (const ParticleBatch& b : batches) {
        if (b.vertexCount == 0) continue;
        const rhi::PipelineHandle pipeline = b.additive ? additive_ : alpha_;
        if (!(pipeline == bound)) {
            cmd.BindPipeline(pipeline);
            cmd.BindVertexBuffer(alloc.buffer, alloc.offset);
            bound = pipeline;
        }
        PushData push{};
        push.viewProj = viewProj;
        push.textureIndex = b.textureIndex;
        push.flags = (b.sourceOne ? 1u : 0u) | (b.premultiplied ? 0x10000u : 0u);
        if (soft != nullptr && soft->depthIndex != 0 && !b.hard) {
            push.depthIndex = soft->depthIndex;
            push.softness = std::max(soft->softness, 0.1f);
            push.rcpSize[0] = 1.0f / std::max(soft->width, 1.0f);
            push.rcpSize[1] = 1.0f / std::max(soft->height, 1.0f);
            push.nearPlane = soft->nearPlane;
            push.farPlane = soft->farPlane;
        }
        cmd.PushConstants(&push, sizeof(push));
        cmd.Draw(b.vertexCount, b.firstVertex);
    }
}

} // namespace sr::render
