#include "sr/gfx/WorldPass.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "kCullComputeShader.spv.h"
#include "kSkinComputeShader.spv.h"
#include "kWorldBlendedPixelShader.spv.h"
#include "kWorldDepthPixelShader.spv.h"
#include "kWorldDepthOnlyVertexShader.spv.h"
#include "kWorldDepthVertexShader.spv.h"
#include "kWorldPixelShader.spv.h"
#include "kWorldVertexShader.spv.h"
#include "sr/core/Trace.h"

namespace sr::gfx {

namespace {

template <std::size_t N>
std::span<const std::byte> Spirv(const unsigned char (&code)[N]) {
    return {reinterpret_cast<const std::byte*>(code), N};
}

// Layouts in Cull.hlsl and Skin.hlsl.
struct CullConstants {
    float planes[6][4];
    std::uint32_t instanceCount = 0;
    std::uint32_t maxDraws = 0;
    std::uint32_t counts = 0; // storage index
    std::uint32_t maxJobs = 0;
    std::uint64_t instances = 0, meshes = 0, commands = 0, draws = 0, jobs = 0;
    std::uint32_t arenaVertices = 0;
    float waterLevel = 0;
    std::uint32_t views = 1;
    std::uint32_t pad[3]{};
    float morePlanes[3][6][4]{}; // views 1..3
    float minRadius[4]{};
    std::uint32_t skinnedViews = 4;
    std::uint32_t cutoutBucket = 0;
    std::uint32_t waterBlended = 0; // modern water: water parts into the translucent list
    std::uint32_t staticViews = 4;
    float eye[3]{};
    float minAngle = 0.0f;
};
static_assert(offsetof(CullConstants, cutoutBucket) == 484);
static_assert(offsetof(CullConstants, eye) == 496 && offsetof(CullConstants, minAngle) == 508);
static_assert(offsetof(CullConstants, minRadius) == 464);
static_assert(offsetof(CullConstants, skinnedViews) == 480);
static_assert(offsetof(CullConstants, views) == 160);
static_assert(offsetof(CullConstants, morePlanes) == 176);
static_assert(offsetof(CullConstants, instanceCount) == 96);
static_assert(offsetof(CullConstants, instances) == 112);
static_assert(offsetof(CullConstants, draws) == 136);
static_assert(offsetof(CullConstants, arenaVertices) == 152);

struct SkinConstants {
    std::uint64_t jobs = 0, meshes = 0, instances = 0, geometry = 0, palette = 0, arena = 0;
    std::uint32_t maxJobs = 0;
    std::uint32_t pad = 0;
};
static_assert(offsetof(SkinConstants, maxJobs) == 48);

struct DrawPush {
    glm::mat4 viewProj;
    std::uint64_t geometry = 0, instances = 0, meshes = 0, draws = 0, arena = 0, frame = 0;
};
static_assert(sizeof(DrawPush) <= 128);

// The counts buffer: a uint per list at 0, the arena's vertices at 32, the skinning dispatch's
// group counts (jobs, 1, 1) at 48. Lists: bucket + phase * kBucketCount (Cull.hlsl).
constexpr std::size_t kSkinDispatch = 48;
constexpr std::uint32_t kLists = kBucketCount * 2; // a view's
// Where a view's list counts (Cull.hlsl CountOffset): view 0 at 0, views 1..3 from 64.
std::size_t CountOffset(std::uint32_t view, std::uint32_t list) { return view == 0 ? list * 4 : 64 + ((view - 1) * kLists + list) * 4; }
constexpr std::uint32_t kDrawRecordBytes = 16;
constexpr std::uint32_t kJobBytes = 16;
constexpr std::uint32_t kArenaVertexBytes = 32;

// World-space frustum planes from a view-projection (depth 0..1): inside where dot(n, p) + d >= 0.
void FrustumPlanes(const glm::mat4& m, float out[6][4]) {
    const auto row = [&](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
    const glm::vec4 planes[6] = {row(3) + row(0), row(3) - row(0), row(3) + row(1), row(3) - row(1), row(2), row(3) - row(2)};
    for (int p = 0; p < 6; ++p) {
        const float length = glm::length(glm::vec3(planes[p]));
        const glm::vec4 n = length > 0 ? planes[p] / length : planes[p];
        std::memcpy(out[p], &n[0], sizeof(float) * 4);
    }
}

} // namespace

bool WorldPass::Init(rhi::Device& device, rhi::Format color, rhi::Format depth, const Limits& limits, std::string& error) {
    device_ = &device;
    limits_ = limits;
    const std::uint32_t maxDraws = limits.maxDraws;
    rhi::ComputePipelineDesc cull;
    cull.shader = Spirv(kCullComputeShader);
    cull.entry = "CSMain";
    cull.debugName = "cull";
    cull_ = device.CreateComputePipeline(cull);
    rhi::ComputePipelineDesc skin;
    skin.shader = Spirv(kSkinComputeShader);
    skin.entry = "CSMain";
    skin.debugName = "skin";
    skin_ = device.CreateComputePipeline(skin);

    rhi::GraphicsPipelineDesc draw;
    draw.vertexShader = Spirv(kWorldVertexShader);
    draw.fragmentShader = Spirv(kWorldPixelShader);
    draw.vertexEntry = "VSMain";
    draw.fragmentEntry = "PSMain";
    draw.depthTest = true;
    draw.depthWrite = true;
    draw.colorFormat = color;
    draw.depthFormat = depth;
    draw.debugName = "world";
    draw.cull = rhi::CullMode::Back; // as the terrain: the original culls counter-clockwise
    if (limits.depthOnly) {
        // Shadow casters: both sides (thin walls and roofs leave no light leaks), biased by slope.
        // Solid parts (bucket 0) without a fragment shader; cut-out and fading ones (bucket 1)
        // with the one that leaves their holes.
        draw.vertexShader = Spirv(kWorldDepthOnlyVertexShader);
        draw.vertexEntry = "VSDepthOnly";
        draw.fragmentShader = {};
        draw.depthOnly = true;
        draw.colorFormat = rhi::Format::Unknown;
        draw.cull = rhi::CullMode::None;
        draw.depthBiasConstant = 1.0f;
        draw.depthBiasSlope = 1.75f;
        draw.debugName = "world (solid shadow casters)";
        draw_ = device.CreateGraphicsPipeline(draw);
        draw.vertexShader = Spirv(kWorldDepthVertexShader);
        draw.vertexEntry = "VSDepth";
        draw.fragmentShader = Spirv(kWorldDepthPixelShader);
        draw.fragmentEntry = "PSDepth";
        draw.debugName = "world (cut-out shadow casters)";
        drawTwoSided_ = device.CreateGraphicsPipeline(draw);
    } else {
        draw_ = device.CreateGraphicsPipeline(draw);
        draw.cull = rhi::CullMode::None;
        draw.debugName = "world, two-sided";
        drawTwoSided_ = device.CreateGraphicsPipeline(draw);
        // Translucent parts: the shader writes premultiplied colour (each part's own blend factors
        // folded in) and drops the back faces of one-sided ones.
        draw.fragmentShader = Spirv(kWorldBlendedPixelShader);
        draw.fragmentEntry = "PSBlended";
        draw.blend = rhi::BlendMode::PremultipliedAlpha;
        draw.depthWrite = false;
        draw.debugName = "world, translucent";
        drawBlended_ = device.CreateGraphicsPipeline(draw);
    }

    rhi::BufferDesc desc;
    desc.cpuVisible = false;
    desc.usage = rhi::BufferUsage::Indirect | rhi::BufferUsage::Storage;
    const std::size_t views = std::clamp<std::uint32_t>(limits.views, 1, kMaxViews);
    desc.size = views * kLists * maxDraws * sizeof(rhi::DrawIndexedIndirectCommand);
    desc.debugName = "draw commands";
    commands_ = device.CreateBuffer(desc);
    desc.size = 256;
    desc.debugName = "draw counts";
    counts_ = device.CreateBuffer(desc);
    desc.usage = rhi::BufferUsage::Storage;
    desc.size = views * kLists * maxDraws * kDrawRecordBytes;
    desc.debugName = "draw records";
    draws_ = device.CreateBuffer(desc);
    desc.size = std::size_t{limits.maxSkinJobs} * kJobBytes;
    desc.debugName = "skinning jobs";
    jobs_ = device.CreateBuffer(desc);
    desc.size = std::size_t{limits.arenaVertices} * kArenaVertexBytes;
    desc.debugName = "skinned vertices";
    arena_ = device.CreateBuffer(desc);
    if (!cull_ || !skin_ || !draw_ || !drawTwoSided_ || (!limits.depthOnly && !drawBlended_) || !commands_ || !counts_ || !draws_ || !jobs_ || !arena_ || device.StorageIndex(counts_) == 0) {
        error = "cannot create the GPU-driven world pass";
        return false;
    }
    return true;
}

void WorldPass::Shutdown() {
    if (device_ == nullptr) return;
    device_->DestroyPipeline(cull_);
    device_->DestroyPipeline(skin_);
    device_->DestroyPipeline(draw_);
    device_->DestroyPipeline(drawTwoSided_);
    if (drawBlended_) device_->DestroyPipeline(drawBlended_);
    device_->DestroyBuffer(commands_);
    device_->DestroyBuffer(counts_);
    device_->DestroyBuffer(draws_);
    device_->DestroyBuffer(jobs_);
    device_->DestroyBuffer(arena_);
    device_ = nullptr;
}

WorldPass::Lists WorldPass::AddCulling(RenderGraph& graph, RenderGraph::Resource instances, RenderGraph::Resource meshes,
                                       const GpuScene& scene, const MeshTable& table, const glm::mat4& viewProj, bool skinning,
                                       float waterLevel) {
    return AddCulling(graph, instances, meshes, scene, table, std::span<const glm::mat4>(&viewProj, 1), skinning, waterLevel);
}

WorldPass::Lists WorldPass::AddCulling(RenderGraph& graph, RenderGraph::Resource instances, RenderGraph::Resource meshes,
                                       const GpuScene& scene, const MeshTable& table, std::span<const glm::mat4> views, bool skinning,
                                       float waterLevel, std::span<const float> minRadius, std::uint32_t skinnedViews,
                                       std::uint32_t staticViews) {
    Lists lists;
    lists.commands = graph.ImportBuffer(commands_, "draw commands");
    lists.counts = graph.ImportBuffer(counts_, "draw counts");
    lists.draws = graph.ImportBuffer(draws_, "draw records");
    lists.jobs = graph.ImportBuffer(jobs_, "skinning jobs");
    lists.arena = graph.ImportBuffer(arena_, "skinned vertices");
    graph.AddPass("world: clear lists", PassKind::Transfer,
                  [&](RenderGraph::Builder& b) { b.Write(lists.counts, Use::TransferWrite); },
                  [this](rhi::CommandList& cmd, const RenderGraph&) {
                      // Counters to 0, the skinning dispatch to (0, 1, 1); disjoint ranges.
                      cmd.FillBuffer(counts_, 0, kSkinDispatch + 4, 0);
                      cmd.FillBuffer(counts_, kSkinDispatch + 4, 8, 1);
                      cmd.FillBuffer(counts_, 64, std::size_t{3} * kLists * 4, 0); // views 1..3
                  });
    CullConstants constants;
    constants.views = static_cast<std::uint32_t>(std::clamp<std::size_t>(views.size(), 1, std::min<std::size_t>(limits_.views, kMaxViews)));
    FrustumPlanes(views[0], constants.planes);
    for (std::uint32_t v = 1; v < constants.views; ++v) FrustumPlanes(views[v], constants.morePlanes[v - 1]);
    for (std::size_t v = 0; v < minRadius.size() && v < 4; ++v) constants.minRadius[v] = minRadius[v];
    constants.skinnedViews = skinnedViews;
    constants.staticViews = staticViews;
    std::memcpy(constants.eye, &sizeCullEye_[0], sizeof(constants.eye));
    constants.minAngle = sizeCullAngle_;
    constants.cutoutBucket = limits_.depthOnly ? 1u : 0u;
    constants.waterBlended = waterBlended_ && !limits_.depthOnly ? 1u : 0u;
    constants.instanceCount = scene.High();
    constants.maxDraws = limits_.maxDraws;
    constants.counts = device_->StorageIndex(counts_);
    constants.maxJobs = skinning ? limits_.maxSkinJobs : 0;
    constants.instances = scene.Address();
    constants.meshes = table.Address();
    constants.commands = device_->BufferAddress(commands_);
    constants.draws = device_->BufferAddress(draws_);
    constants.jobs = device_->BufferAddress(jobs_);
    constants.arenaVertices = limits_.arenaVertices;
    constants.waterLevel = waterLevel;
    graph.AddPass("world: cull", PassKind::Compute,
                  [&](RenderGraph::Builder& b) {
                      b.Read(instances, Use::ComputeRead)
                          .Read(meshes, Use::ComputeRead)
                          .Write(lists.counts, Use::ComputeWrite)
                          .Write(lists.commands, Use::ComputeWrite)
                          .Write(lists.draws, Use::ComputeWrite)
                          .Write(lists.jobs, Use::ComputeWrite);
                  },
                  [this, constants](rhi::CommandList& cmd, const RenderGraph&) {
                      SR_ZONE("render: cull");
                      if (constants.instanceCount == 0) return;
                      const rhi::TransientAllocation alloc = device_->AllocateTransient(sizeof(CullConstants), 16);
                      if (alloc.cpu == nullptr) return;
                      std::memcpy(alloc.cpu, &constants, sizeof(constants));
                      cmd.BindComputePipeline(cull_);
                      const std::uint64_t address = alloc.gpuAddress;
                      cmd.PushConstants(&address, sizeof(address));
                      cmd.Dispatch((constants.instanceCount + 63) / 64, 1, 1);
                  });
    return lists;
}

void WorldPass::AddSkinning(RenderGraph& graph, const Lists& lists, RenderGraph::Resource instances, RenderGraph::Resource meshes,
                            RenderGraph::Resource geometry, RenderGraph::Resource paletteResource, rhi::BufferHandle palette,
                            const GeometryPool& pool, const GpuScene& scene, const MeshTable& table) {
    SkinConstants constants;
    constants.jobs = device_->BufferAddress(jobs_);
    constants.meshes = table.Address();
    constants.instances = scene.Address();
    constants.geometry = pool.Address();
    constants.palette = device_->BufferAddress(palette);
    constants.arena = device_->BufferAddress(arena_);
    constants.maxJobs = limits_.maxSkinJobs;
    graph.AddPass("world: skin", PassKind::Compute,
                  [&](RenderGraph::Builder& b) {
                      b.Read(lists.counts, Use::IndirectRead)
                          .Read(lists.jobs, Use::ComputeRead)
                          .Read(instances, Use::ComputeRead)
                          .Read(meshes, Use::ComputeRead)
                          .Read(geometry, Use::ComputeRead)
                          .Read(paletteResource, Use::ComputeRead)
                          .Write(lists.arena, Use::ComputeWrite);
                  },
                  [this, constants](rhi::CommandList& cmd, const RenderGraph&) {
                      SR_ZONE("render: skin");
                      if (constants.palette == 0) return;
                      const rhi::TransientAllocation alloc = device_->AllocateTransient(sizeof(SkinConstants), 16);
                      if (alloc.cpu == nullptr) return;
                      std::memcpy(alloc.cpu, &constants, sizeof(constants));
                      cmd.BindComputePipeline(skin_);
                      const std::uint64_t address = alloc.gpuAddress;
                      cmd.PushConstants(&address, sizeof(address));
                      cmd.DispatchIndirect(counts_, kSkinDispatch);
                  });
}

void WorldPass::Draw(rhi::CommandList& cmd, const glm::mat4& viewProj, const GeometryPool& pool, const GpuScene& scene,
                     const MeshTable& table, std::uint64_t frame, Phase phase, Parts parts) const {
    if (scene.High() == 0) return;
    cmd.BindIndexBuffer(pool.Buffer(), 0, false);
    DrawPush push;
    push.viewProj = viewProj;
    push.geometry = pool.Address();
    push.instances = scene.Address();
    push.meshes = table.Address();
    push.draws = device_->BufferAddress(draws_);
    push.arena = device_->BufferAddress(arena_);
    push.frame = frame;
    const std::uint32_t maxDraws = limits_.maxDraws;
    for (std::uint32_t bucket = 0; bucket < kBucketCount; ++bucket) {
        if (bucket == kBucketBlended ? parts == Parts::Solid : parts == Parts::Translucent) continue;
        if (bucket == kBucketBlended && !drawBlended_) continue;
        cmd.BindPipeline(bucket == kBucketBlended ? drawBlended_ : bucket == kBucketTwoSided ? drawTwoSided_ : draw_);
        cmd.PushConstants(&push, sizeof(push));
        const std::uint32_t list = bucket + (phase == Phase::AfterWater ? kBucketCount : 0);
        cmd.DrawIndexedIndirectCount(commands_, std::size_t{list} * maxDraws * sizeof(rhi::DrawIndexedIndirectCommand), counts_,
                                     std::size_t{list} * 4, maxDraws);
    }
}

void WorldPass::DrawDepth(rhi::CommandList& cmd, const glm::mat4& viewProj, const GeometryPool& pool, const GpuScene& scene,
                          const MeshTable& table, std::uint64_t frame, std::uint32_t view) const {
    if (scene.High() == 0) return;
    cmd.BindIndexBuffer(pool.Buffer(), 0, false);
    DrawPush push;
    push.viewProj = viewProj;
    push.geometry = pool.Address();
    push.instances = scene.Address();
    push.meshes = table.Address();
    push.draws = device_->BufferAddress(draws_);
    push.arena = device_->BufferAddress(arena_);
    push.frame = frame;
    const std::uint32_t maxDraws = limits_.maxDraws;
    for (std::uint32_t list = 0; list < kLists; ++list) {
        if (list % kBucketCount == kBucketBlended) continue; // translucent parts cast no shadow (Cull.hlsl)
        cmd.BindPipeline(list % kBucketCount == kBucketTwoSided ? drawTwoSided_ : draw_);
        cmd.PushConstants(&push, sizeof(push));
        const std::size_t at = std::size_t{view} * kLists + list;
        cmd.DrawIndexedIndirectCount(commands_, at * maxDraws * sizeof(rhi::DrawIndexedIndirectCommand), counts_, CountOffset(view, list),
                                     maxDraws);
    }
}

} // namespace sr::gfx
