#include "sr/gfx/TerrainPass.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "kTerrainPixelShader.spv.h"
#include "kTerrainShadowPixelShader.spv.h"
#include "kTerrainShadowVertexShader.spv.h"
#include "kTerrainVertexShader.spv.h"
#include "kIceModernPixelShader.spv.h"
#include "kWaterModernPixelShader.spv.h"
#include "kWaterModernVertexShader.spv.h"
#include "kWaterPixelShader.spv.h"
#include "kWaterVertexShader.spv.h"
#include "sr/core/Trace.h"

namespace sr::gfx {

namespace {

template <std::size_t N>
std::span<const std::byte> Spirv(const unsigned char (&code)[N]) {
    return {reinterpret_cast<const std::byte*>(code), N};
}

// Ahead of the blocks in the frame's allocation (Terrain.hlsl reads both).
struct Constants {
    std::uint64_t pool = 0;
    std::uint64_t pad = 0;
};
static_assert(sizeof(Constants) == 16);

struct Push {
    glm::mat4 viewProj;
    std::uint64_t data = 0;  // Constants, then the blocks
    std::uint64_t frame = 0; // FrameConstants
    std::uint32_t lodBase = 0, lodShift = 0;
};
static_assert(sizeof(Push) <= 128);

// Ahead of the region table and the shadows in the frame's allocation (Terrain.hlsl, VSShadow).
struct ShadowConstants {
    std::uint64_t pool = 0;
    std::uint32_t count = 0;
    std::uint32_t texture = 0;
    float alpha = 0;
    std::uint32_t pad[3]{};
};
static_assert(sizeof(ShadowConstants) == 32);
constexpr std::uint32_t kVerticesPerShadow = 8 * 8 * 6; // 8 x 8 tiles
// The original's shadow colour, 0xA8000000: black at alpha 168.
constexpr float kShadowAlpha = 168.0f / 255.0f;

// Outside one of the frustum's planes (world space, depth 0..1).
bool Outside(const glm::mat4& m, const glm::vec3& lo, const glm::vec3& hi) {
    const auto row = [&](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
    const glm::vec4 planes[6] = {row(3) + row(0), row(3) - row(0), row(3) + row(1), row(3) - row(1), row(2), row(3) - row(2)};
    for (const glm::vec4& p : planes) {
        // The box's corner farthest along the plane's normal.
        const glm::vec3 far(p.x >= 0 ? hi.x : lo.x, p.y >= 0 ? hi.y : lo.y, p.z >= 0 ? hi.z : lo.z);
        if (glm::dot(glm::vec3(p), far) + p.w < 0) return true;
    }
    return false;
}

} // namespace

bool TerrainPass::Init(rhi::Device& device, rhi::Format color, rhi::Format depth, std::uint32_t maxBlocks, std::string& error) {
    device_ = &device;
    maxBlocks_ = maxBlocks;
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = Spirv(kTerrainVertexShader);
    desc.fragmentShader = Spirv(kTerrainPixelShader);
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = "PSMain";
    desc.depthTest = true;
    desc.depthWrite = true;
    desc.cull = rhi::CullMode::Back; // the original culls counter-clockwise
    desc.colorFormat = color;
    desc.depthFormat = depth;
    desc.debugName = "terrain";
    pipeline_ = device.CreateGraphicsPipeline(desc);
    rhi::GraphicsPipelineDesc shadow = desc;
    shadow.vertexShader = Spirv(kTerrainShadowVertexShader);
    shadow.fragmentShader = Spirv(kTerrainShadowPixelShader);
    shadow.vertexEntry = "VSShadow";
    shadow.fragmentEntry = "PSShadow";
    shadow.blend = rhi::BlendMode::AlphaBlend;
    shadow.depthWrite = false;
    // The shadow's vertices are the terrain's, computed alike; a nudge towards the camera covers
    // any difference in rounding between the two shaders.
    shadow.depthBiasConstant = -1.0f;
    shadow.depthBiasSlope = -1.0f;
    shadow.debugName = "terrain shadows";
    shadow_ = device.CreateGraphicsPipeline(shadow);
    // The original's water: blended by its alpha, both sides, depth written (so what draws after
    // it is hidden below the surface).
    rhi::GraphicsPipelineDesc water = desc;
    water.vertexShader = Spirv(kWaterVertexShader);
    water.fragmentShader = Spirv(kWaterPixelShader);
    water.vertexEntry = "VSWater";
    water.fragmentEntry = "PSWater";
    water.blend = rhi::BlendMode::AlphaBlend;
    water.cull = rhi::CullMode::None;
    water.debugName = "water";
    water_pipeline_ = device.CreateGraphicsPipeline(water);
    water.vertexShader = Spirv(kWaterModernVertexShader);
    water.vertexEntry = "VSWaterModern";
    water.fragmentShader = Spirv(kWaterModernPixelShader);
    water.fragmentEntry = "PSWaterModern";
    water.debugName = "water (modern)";
    waterModern_ = device.CreateGraphicsPipeline(water);
    water.fragmentShader = Spirv(kIceModernPixelShader);
    water.fragmentEntry = "PSIceModern";
    water.debugName = "ice (modern)";
    iceModern_ = device.CreateGraphicsPipeline(water);
    if (!pipeline_ || !shadow_ || !water_pipeline_ || !waterModern_ || !iceModern_) {
        error = "cannot create the terrain pipeline";
        return false;
    }
    blocks_.reserve(maxBlocks);
    water_.reserve(maxBlocks);
    return true;
}

void TerrainPass::Shutdown() {
    if (device_ == nullptr) return;
    device_->DestroyPipeline(pipeline_);
    device_->DestroyPipeline(shadow_);
    device_->DestroyPipeline(water_pipeline_);
    device_->DestroyPipeline(waterModern_);
    device_->DestroyPipeline(iceModern_);
    device_ = nullptr;
}

void TerrainPass::Prepare(std::span<const Region> regions, const glm::mat4& viewProj, const glm::vec3& eye,
                          const std::array<float, 3>& lodDistances) {
    // A block's detail by the flat distance of its centre (its neighbours' the same way: both sides
    // of an edge agree).
    const auto shiftAt = [&](const glm::vec3& corner) -> std::uint32_t {
        const glm::vec2 centre = glm::vec2(corner.x, corner.z) + assets::kBlockSize * 0.5f;
        const float d = glm::length(centre - glm::vec2(eye.x, eye.z));
        std::uint32_t shift = 0;
        for (float limit : lodDistances) shift += limit > 0.0f && d > limit ? 1u : 0u;
        return shift;
    };
    SR_ZONE("render: terrain blocks");
    blocks_.clear();
    lodShifts_.clear();
    water_.clear();
    waterLevel_ = -1e38f;
    float nearestWater = 0;
    regionTable_.fill(0);
    for (const Region& region : regions) {
        if (blocks_.size() == maxBlocks_) break;
        if (region.info == nullptr) continue;
        const int rx = static_cast<int>(std::floor(region.offset.x / assets::kRegionSize)) + kTableSide / 2;
        const int rz = static_cast<int>(std::floor(region.offset.z / assets::kRegionSize)) + kTableSide / 2;
        if (rx >= 0 && rz >= 0 && rx < kTableSide && rz < kTableSide) regionTable_[static_cast<std::size_t>(rz * kTableSide + rx)] = region.info->offset + 1;
        for (std::uint32_t b = 0; b < kTerrainBlocks; ++b) {
            const TerrainWater& w = region.info->water[b];
            float lo = region.info->blockHeights[b][0], hi = region.info->blockHeights[b][1];
            if (w.Visible()) {
                lo = std::min(lo, w.height);
                hi = std::max(hi, w.height);
            }
            const glm::vec3 corner = region.offset + glm::vec3(static_cast<float>(b % assets::kBlocksPerRegion) * assets::kBlockSize, 0.0f,
                                                               static_cast<float>(b / assets::kBlocksPerRegion) * assets::kBlockSize);
            if (Outside(viewProj, corner + glm::vec3(0, lo, 0), corner + glm::vec3(assets::kBlockSize, hi, assets::kBlockSize))) continue;
            if (blocks_.size() == maxBlocks_) break;
            if (w.Visible()) {
                WaterBlock wb;
                wb.region = region.info->offset;
                wb.block = b;
                wb.type = static_cast<std::uint32_t>(w.type);
                std::memcpy(wb.offset, &region.offset[0], sizeof(wb.offset));
                wb.height = w.height;
                water_.push_back(wb);
                // The water level: the nearest visible water's height (the original takes the
                // first of its visible list, which is ordered by distance).
                const glm::vec2 centre = glm::vec2(corner.x, corner.z) + assets::kBlockSize * 0.5f;
                const float distance = glm::length(centre - glm::vec2(eye.x, eye.z));
                if (waterLevel_ < -1e37f || distance < nearestWater) {
                    waterLevel_ = w.height;
                    nearestWater = distance;
                }
            }
            Block block;
            block.region = region.info->offset;
            block.block = b;
            block.lightmap = region.info->lightmap;
            std::memcpy(block.offset, &region.offset[0], sizeof(block.offset));
            const float size = assets::kBlockSize;
            block.neighbours = shiftAt(corner - glm::vec3(size, 0, 0)) | shiftAt(corner + glm::vec3(size, 0, 0)) << 8 |
                               shiftAt(corner - glm::vec3(0, 0, size)) << 16 | shiftAt(corner + glm::vec3(0, 0, size)) << 24;
            blocks_.push_back(block);
            lodShifts_.push_back(static_cast<std::uint8_t>(shiftAt(corner)));
        }
    }
    // The blocks by their detail, finest first (a draw each).
    lodCounts_ = {};
    for (std::uint8_t s : lodShifts_) ++lodCounts_[s];
    if (lodCounts_[0] != blocks_.size()) {
        lodOrder_.resize(blocks_.size());
        for (std::size_t i = 0; i < lodOrder_.size(); ++i) lodOrder_[i] = std::uint32_t(i);
        std::stable_sort(lodOrder_.begin(), lodOrder_.end(), [&](std::uint32_t a, std::uint32_t b) { return lodShifts_[a] < lodShifts_[b]; });
        lodSorted_.resize(blocks_.size());
        for (std::size_t i = 0; i < lodOrder_.size(); ++i) lodSorted_[i] = blocks_[lodOrder_[i]];
        blocks_.swap(lodSorted_);
    }
    // The water blocks before the ice (the modern water draws them with different shaders).
    iceFirst_ = static_cast<std::size_t>(std::stable_partition(water_.begin(), water_.end(), [](const WaterBlock& w) { return w.type != 1; }) -
                                         water_.begin());
}

void TerrainPass::Draw(rhi::CommandList& cmd, const glm::mat4& viewProj, const GeometryPool& pool, std::uint64_t frame) const {
    if (blocks_.empty()) return;
    SR_ZONE("render: terrain");
    const std::size_t bytes = sizeof(Constants) + blocks_.size() * sizeof(Block);
    const rhi::TransientAllocation alloc = device_->AllocateTransient(bytes, 16);
    if (alloc.cpu == nullptr) return;
    Constants constants;
    constants.pool = pool.Address();
    std::memcpy(alloc.cpu, &constants, sizeof(constants));
    std::memcpy(static_cast<std::byte*>(alloc.cpu) + sizeof(constants), blocks_.data(), blocks_.size() * sizeof(Block));

    Push push;
    push.viewProj = viewProj;
    push.data = alloc.gpuAddress;
    push.frame = frame;
    cmd.BindPipeline(pipeline_);
    // No vertex or index buffer: the vertex shader makes each block's tiles from its id; a draw for
    // each level of detail (16 x 16 tiles a block, 8 x 8, 4 x 4, 2 x 2).
    std::uint32_t first = 0;
    for (std::uint32_t shift = 0; shift < 4; ++shift) {
        const std::uint32_t count = lodCounts_[shift];
        if (count == 0) continue;
        push.lodBase = first;
        push.lodShift = shift;
        cmd.PushConstants(&push, sizeof(push));
        const std::uint32_t side = 16u >> shift;
        cmd.Draw(side * side * 6 * count, 0);
        first += count;
    }
}

void TerrainPass::DrawWater(rhi::CommandList& cmd, const glm::mat4& viewProj, std::uint64_t frame, const GeometryPool& pool,
                            std::uint32_t water, std::uint32_t ice, bool modern, const WaterReflection* reflection, int detail) const {
    if (water_.empty()) return;
    SR_ZONE("render: water");
    // The header the water shaders read (kWaterHeader): the pool, the water's frame and the ice, then the reflection, then
    // the Video page's Water Detail (0 low, 1 medium, 2 high).
    struct WaterConstants {
        std::uint64_t pool = 0;
        std::uint32_t water = 0, ice = 0;
        std::uint32_t reflection = 0, bump = 0;
        float width = 0, height = 0;
        std::uint32_t detail = 1, pad[3]{};
    } constants{pool.Address(), water, ice};
    constants.detail = std::uint32_t(std::clamp(detail, 0, 2));
    if (reflection != nullptr) {
        constants.reflection = reflection->texture;
        constants.bump = reflection->bump;
        constants.width = reflection->width;
        constants.height = reflection->height;
    }
    static_assert(sizeof(WaterConstants) == 48);
    const std::size_t bytes = sizeof(constants) + water_.size() * sizeof(WaterBlock);
    const rhi::TransientAllocation alloc = device_->AllocateTransient(bytes, 16);
    if (alloc.cpu == nullptr) return;
    std::memcpy(alloc.cpu, &constants, sizeof(constants));
    std::memcpy(static_cast<std::byte*>(alloc.cpu) + sizeof(constants), water_.data(), water_.size() * sizeof(WaterBlock));
    Push push;
    push.viewProj = viewProj;
    push.data = alloc.gpuAddress;
    push.frame = frame;
    if (!modern) {
        cmd.BindPipeline(water_pipeline_);
        cmd.PushConstants(&push, sizeof(push));
        cmd.Draw(6 * static_cast<std::uint32_t>(water_.size()), 0); // a quad per block
        return;
    }
    // Modern: the water blocks, then the ice blocks, each with its own shader (Prepare puts the
    // water first).
    if (iceFirst_ > 0) {
        cmd.BindPipeline(waterModern_);
        cmd.PushConstants(&push, sizeof(push));
        cmd.Draw(6 * static_cast<std::uint32_t>(iceFirst_), 0);
    }
    if (iceFirst_ < water_.size()) {
        cmd.BindPipeline(iceModern_);
        cmd.PushConstants(&push, sizeof(push));
        cmd.Draw(6 * static_cast<std::uint32_t>(water_.size() - iceFirst_), 6 * static_cast<std::uint32_t>(iceFirst_));
    }
}

void TerrainPass::DrawShadows(rhi::CommandList& cmd, const glm::mat4& viewProj, std::uint64_t frame, const GeometryPool& pool,
                              std::span<const Shadow> shadows, std::uint32_t texture) const {
    if (shadows.empty() || blocks_.empty()) return;
    SR_ZONE("render: blob shadows");
    const std::size_t tableBytes = sizeof(regionTable_);
    const std::size_t bytes = sizeof(ShadowConstants) + tableBytes + shadows.size() * sizeof(Shadow);
    const rhi::TransientAllocation alloc = device_->AllocateTransient(bytes, 16);
    if (alloc.cpu == nullptr) return;
    ShadowConstants constants;
    constants.pool = pool.Address();
    constants.count = static_cast<std::uint32_t>(shadows.size());
    constants.texture = texture;
    constants.alpha = kShadowAlpha;
    auto* out = static_cast<std::byte*>(alloc.cpu);
    std::memcpy(out, &constants, sizeof(constants));
    std::memcpy(out + sizeof(constants), regionTable_.data(), tableBytes);
    std::memcpy(out + sizeof(constants) + tableBytes, shadows.data(), shadows.size() * sizeof(Shadow));

    Push push;
    push.viewProj = viewProj;
    push.data = alloc.gpuAddress;
    push.frame = frame;
    cmd.BindPipeline(shadow_);
    cmd.PushConstants(&push, sizeof(push));
    cmd.Draw(kVerticesPerShadow * static_cast<std::uint32_t>(shadows.size()), 0);
}

} // namespace sr::gfx
