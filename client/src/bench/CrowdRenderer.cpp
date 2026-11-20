#include "bench/CrowdRenderer.h"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "bench/CrowdRig.h"
#include "kBlitPixelShader.spv.h"
#include "kBlitVertexShader.spv.h"
#include "kCrowdPixelShader.spv.h"
#include "kCrowdVertexShader.spv.h"
#include "sr/core/Trace.h"

namespace sr::client {

namespace {

template <std::size_t N>
std::span<const std::byte> Spirv(const unsigned char (&code)[N]) {
    return {reinterpret_cast<const std::byte*>(code), N};
}

struct CrowdPush {
    glm::mat4 viewProj;
    std::uint64_t palette = 0;
    std::uint32_t instanceBase = 0;
    std::uint32_t pad = 0;
};
static_assert(sizeof(CrowdPush) <= 128);

struct BlitPush {
    std::uint32_t textureIndex = 0;
    std::uint32_t samplerIndex = 0;
};

glm::mat4 ViewProj(const OrbitCamera& c, float aspect) {
    const glm::vec3 target(c.targetX, c.targetY, c.targetZ);
    const glm::vec3 dir(std::cos(c.pitch) * std::sin(c.yaw), std::sin(c.pitch), std::cos(c.pitch) * std::cos(c.yaw));
    const glm::mat4 view = glm::lookAt(target + dir * c.distance, target, glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(c.fovY, aspect, 1.0f, 20000.0f);
    proj[1][1] *= -1.0f; // Vulkan's y points down
    return proj * view;
}

} // namespace

bool CrowdRenderer::Init(rhi::Device& device, std::string& error) {
    device_ = &device;
    rhi::GraphicsPipelineDesc crowd;
    crowd.vertexShader = Spirv(kCrowdVertexShader);
    crowd.fragmentShader = Spirv(kCrowdPixelShader);
    crowd.vertexEntry = "VSMain";
    crowd.fragmentEntry = "PSMain";
    crowd.vertexStride = 24;
    crowd.attributes = {{0, rhi::VertexFormat::Float3, 0}, {1, rhi::VertexFormat::Float3, 12}};
    crowd.depthTest = true;
    crowd.depthWrite = true;
    crowd.colorFormat = rhi::Format::RGBA8Unorm;
    crowd.depthFormat = rhi::Format::D32Float;
    crowd.debugName = "crowd";
    crowd_ = device.CreateGraphicsPipeline(crowd);

    rhi::GraphicsPipelineDesc blit;
    blit.vertexShader = Spirv(kBlitVertexShader);
    blit.fragmentShader = Spirv(kBlitPixelShader);
    blit.vertexEntry = "VSMain";
    blit.fragmentEntry = "PSMain";
    blit.debugName = "blit";
    blit_ = device.CreateGraphicsPipeline(blit);
    if (!crowd_ || !blit_) {
        error = "cannot create the crowd pipelines";
        return false;
    }

    // A unit box: 4 vertices per face (flat normals), 2 triangles per face.
    std::vector<float> vertices;
    std::vector<std::uint16_t> indices;
    const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const glm::vec3& n : normals) {
        const glm::vec3 u = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 v = glm::cross(n, u);
        const auto base = static_cast<std::uint16_t>(vertices.size() / 6);
        for (const glm::vec2 corner : {glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, 1)}) {
            const glm::vec3 p = 0.5f * (n + corner.x * u + corner.y * v);
            vertices.insert(vertices.end(), {p.x, p.y, p.z, n.x, n.y, n.z});
        }
        indices.insert(indices.end(), {base, static_cast<std::uint16_t>(base + 1), static_cast<std::uint16_t>(base + 2), base,
                                       static_cast<std::uint16_t>(base + 2), static_cast<std::uint16_t>(base + 3)});
    }
    std::vector<std::byte> bytes(vertices.size() * sizeof(float) + indices.size() * sizeof(std::uint16_t));
    std::memcpy(bytes.data(), vertices.data(), vertices.size() * sizeof(float));
    indexOffset_ = vertices.size() * sizeof(float);
    std::memcpy(bytes.data() + indexOffset_, indices.data(), indices.size() * sizeof(std::uint16_t));
    rhi::BufferDesc desc;
    desc.size = bytes.size();
    desc.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::Index;
    desc.debugName = "crowd box";
    desc.cpuVisible = false;
    cube_ = device.CreateBuffer(desc, bytes);
    return static_cast<bool>(cube_);
}

void CrowdRenderer::Shutdown() {
    if (device_ == nullptr) return;
    device_->DestroyPipeline(crowd_);
    device_->DestroyPipeline(blit_);
    device_->DestroyBuffer(cube_);
    device_->DestroyTexture(color_);
    device_->DestroyTexture(depth_);
    device_ = nullptr;
}

void CrowdRenderer::EnsureTargets(std::uint32_t width, std::uint32_t height) {
    if (width == width_ && height == height_ && color_) return;
    device_->DestroyTexture(color_);
    device_->DestroyTexture(depth_);
    rhi::TextureDesc color;
    color.width = width;
    color.height = height;
    color.format = rhi::Format::RGBA8Unorm;
    color.usage = rhi::TextureUsage::RenderTarget;
    color.debugName = "crowd colour";
    color_ = device_->CreateTexture(color);
    rhi::TextureDesc depth = color;
    depth.format = rhi::Format::D32Float;
    depth.usage = rhi::TextureUsage::DepthStencil;
    depth.debugName = "crowd depth";
    depth_ = device_->CreateTexture(depth);
    width_ = width;
    height_ = height;
}

void CrowdRenderer::Render(rhi::CommandList& cmd, const CrowdView& view, const OrbitCamera& camera, core::JobSystem& jobs,
                           std::uint32_t width, std::uint32_t height) {
    SR_ZONE("render: crowd");
    if (width == 0 || height == 0) return;
    EnsureTargets(width, height);

    rhi::RenderPassDesc pass;
    pass.color = color_;
    pass.depth = depth_;
    pass.clearColor[0] = 0.10f;
    pass.clearColor[1] = 0.12f;
    pass.clearColor[2] = 0.15f;
    pass.parallel = true;
    cmd.BeginRenderPass(pass);

    const std::uint32_t perCharacter = view.bonesPerCharacter == 0 ? 1 : view.bonesPerCharacter;
    const std::uint32_t characters = view.bones / perCharacter;
    const std::uint32_t chunks = std::max(1u, std::min(view.chunks, std::max(1u, characters)));
    const std::uint32_t perChunk = (characters + chunks - 1) / chunks;
    CrowdPush push;
    push.viewProj = ViewProj(camera, static_cast<float>(width) / static_cast<float>(height));
    push.palette = device_->BufferAddress(view.palettes);

    const std::span<rhi::CommandList* const> lists = cmd.BeginParallel(chunks);
    jobs.ParallelFor(lists.size(), 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            SR_ZONE("render: crowd chunk");
            rhi::CommandList& list = *lists[i];
            const std::uint32_t first = static_cast<std::uint32_t>(i) * perChunk;
            if (first >= characters || push.palette == 0) continue;
            const std::uint32_t count = std::min(perChunk, characters - first);
            CrowdPush chunkPush = push;
            chunkPush.instanceBase = first * perCharacter;
            list.BindPipeline(crowd_);
            list.BindVertexBuffer(cube_, 0);
            list.BindIndexBuffer(cube_, indexOffset_, false);
            list.PushConstants(&chunkPush, sizeof(chunkPush));
            list.DrawIndexedInstanced(36, count * perCharacter, 0, 0);
        }
    });
    cmd.EndParallel();
    cmd.EndPass();
    rendered_ = true;
}

gfx::RenderGraph::Resource CrowdRenderer::AddPasses(gfx::RenderGraph& graph, const CrowdView& view, const OrbitCamera& camera,
                                                     core::JobSystem& jobs, std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return 0;
    EnsureTargets(width, height); // its own targets (made once per size), imported into the graph
    const auto color = graph.ImportTexture(color_, "crowd colour");
    // The palettes are written by the CPU (host memory): nothing on the GPU to wait for.
    const auto palettes = graph.ImportBuffer(view.palettes, "crowd palettes");
    graph.AddPass("crowd", gfx::PassKind::Graphics,
                  [&](gfx::RenderGraph::Builder& b) { b.Read(palettes, gfx::Use::VertexRead).Write(color, gfx::Use::ColorWrite); },
                  [this, &view, &camera, &jobs, width, height](rhi::CommandList& cmd, const gfx::RenderGraph&) {
                      Render(cmd, view, camera, jobs, width, height);
                  });
    return color;
}

void CrowdRenderer::Composite(rhi::CommandList& cmd) {
    if (!rendered_) return;
    BlitPush push;
    push.textureIndex = device_->BindlessIndex(color_);
    push.samplerIndex = static_cast<std::uint32_t>(rhi::SamplerIndex::PointClamp);
    cmd.BindPipeline(blit_);
    cmd.PushConstants(&push, sizeof(push));
    cmd.Draw(3, 0);
    rendered_ = false;
}

} // namespace sr::client
