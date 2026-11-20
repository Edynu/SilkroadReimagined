#include "sr/render/TerrainRenderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <unordered_map>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "kTerrainPixelShader.spv.h"
#include "kTerrainShadowShader.spv.h"
#include "kTerrainVertexShader.spv.h"
#include "sr/assets/Texture.h"
#include "sr/render/Frustum.h"
#include "sr/render/TextureCache.h"

namespace sr::render {

namespace {

using assets::kBlocksPerRegion;
using assets::kTilesPerBlock;
using assets::kTileSize;

constexpr int kGrid = kBlocksPerRegion * kTilesPerBlock + 1; // 97 samples per axis

using TerrainVertex = TerrainMesh::Vertex;

struct PushData {
    glm::mat4 viewProj; // with the region's offset
    glm::vec4 offset;
    std::uint64_t frame;
    std::uint32_t mode;
    float timeSeconds;
    std::uint32_t lightmap;
    std::uint32_t pad[3];
};
static_assert(sizeof(PushData) == 112);

// Calls draw(first, count) for each run of consecutive items that pass `visible`.
template <typename Visible, typename DrawRun>
void ForEachRun(std::size_t items, Visible&& visible, DrawRun&& draw) {
    std::size_t start = 0;
    bool open = false;
    for (std::size_t i = 0; i <= items; ++i) {
        const bool in = i < items && visible(i);
        if (in && !open) {
            start = i;
            open = true;
        } else if (!in && open) {
            draw(start, i - start);
            open = false;
        }
    }
}

} // namespace

bool TerrainRenderer::Init(rhi::Device& device, rhi::Format colorFormat, rhi::Format depthFormat, std::string& error) {
    device_ = &device;
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = std::as_bytes(std::span(kTerrainVertexShader));
    desc.fragmentShader = std::as_bytes(std::span(kTerrainPixelShader));
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = "PSMain";
    desc.vertexStride = sizeof(TerrainVertex);
    desc.attributes = {
        {0, rhi::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(TerrainVertex, position))},
        {1, rhi::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(TerrainVertex, normal))},
        {2, rhi::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(TerrainVertex, barycentric))},
        {3, rhi::VertexFormat::UInt4, static_cast<std::uint32_t>(offsetof(TerrainVertex, textures))},
    };
    desc.depthTest = true;
    desc.depthWrite = true;
    desc.colorFormat = colorFormat;
    desc.depthFormat = depthFormat;
    desc.debugName = "terrain";
    solid_ = device.CreateGraphicsPipeline(desc);
    desc.wireframe = true;
    wire_ = device.CreateGraphicsPipeline(desc);
    desc.wireframe = false;
    desc.depthWrite = false;
    desc.blend = rhi::BlendMode::AlphaBlend;
    desc.debugName = "water";
    water_ = device.CreateGraphicsPipeline(desc);
    // Modern water draws its final colour (the scene behind is read, not blended) and its depth.
    desc.blend = rhi::BlendMode::Opaque;
    desc.depthWrite = true;
    desc.debugName = "water (modern)";
    waterModern_ = device.CreateGraphicsPipeline(desc);
    // Shadow maps: depth only, biased against acne.
    desc.blend = rhi::BlendMode::Opaque;
    desc.depthWrite = true;
    desc.depthOnly = true;
    desc.colorFormat = rhi::Format::Unknown;
    desc.depthFormat = rhi::Format::D32Float;
    desc.fragmentShader = std::as_bytes(std::span(kTerrainShadowShader));
    desc.fragmentEntry = "PSShadow";
    desc.depthBiasConstant = 1.0f;
    desc.depthBiasSlope = 2.0f;
    desc.debugName = "terrain shadow";
    shadow_ = device.CreateGraphicsPipeline(desc);
    desc.depthBiasConstant = 4.0f;
    desc.depthBiasSlope = 0.0f;
    desc.debugName = "terrain pre-pass";
    prepass_ = device.CreateGraphicsPipeline(desc);
    if (!solid_ || !wire_ || !water_ || !waterModern_ || !shadow_ || !prepass_) {
        error = "could not create the terrain pipelines";
        return false;
    }
    return true;
}

void TerrainRenderer::Shutdown() {
    if (device_ == nullptr) return;
    for (rhi::PipelineHandle p : {solid_, wire_, water_, waterModern_, shadow_, prepass_}) device_->DestroyPipeline(p);
    device_ = nullptr;
}

TerrainMesh TerrainRenderer::Build(const assets::TerrainRegion& region, const assets::AssetIndex& tiles,
                                   const std::string& waterTexture) {
    TerrainMesh mesh;

    // Region-wide sample grid: neighbouring blocks share their edge samples.
    std::vector<float> heights(kGrid * kGrid, 0.0f);
    std::vector<std::uint16_t> words(kGrid * kGrid, 0);
    for (int bz = 0; bz < kBlocksPerRegion; ++bz) {
        for (int bx = 0; bx < kBlocksPerRegion; ++bx) {
            const auto& block = region.blocks[static_cast<std::size_t>(bz * kBlocksPerRegion + bx)];
            for (int z = 0; z <= kTilesPerBlock; ++z) {
                for (int x = 0; x <= kTilesPerBlock; ++x) {
                    const std::size_t g = static_cast<std::size_t>((bz * kTilesPerBlock + z) * kGrid + bx * kTilesPerBlock + x);
                    const std::size_t v = static_cast<std::size_t>(z * assets::kVerticesPerBlock + x);
                    heights[g] = block.heights[v];
                    words[g] = block.textures[v];
                }
            }
        }
    }
    const auto height = [&](int x, int z) {
        x = std::clamp(x, 0, kGrid - 1);
        z = std::clamp(z, 0, kGrid - 1);
        return heights[static_cast<std::size_t>(z * kGrid + x)];
    };
    const auto normal = [&](int x, int z) {
        const float dx = height(x + 1, z) - height(x - 1, z);
        const float dz = height(x, z + 1) - height(x, z - 1);
        return glm::normalize(glm::vec3(-dx, 2.0f * kTileSize, -dz));
    };

    // Ground texture id -> packed slot reference (slot | scale << 24).
    std::unordered_map<std::uint16_t, std::uint32_t> slots;
    const auto texture = [&](std::uint16_t word) {
        const std::uint16_t id = assets::TerrainBlock::TextureId(word);
        auto it = slots.find(id);
        if (it == slots.end()) {
            const auto entry = tiles.find(id);
            if (entry == tiles.end()) ++mesh.unknownTextures;
            mesh.texturePaths.push_back(entry != tiles.end() ? entry->second.path : std::string());
            it = slots.emplace(id, static_cast<std::uint32_t>(mesh.texturePaths.size() - 1)).first;
        }
        return it->second | static_cast<std::uint32_t>(assets::TerrainBlock::TextureScale(word)) << 24;
    };

    mesh.terrain.reserve(static_cast<std::size_t>(kRegionBlocks) * TerrainMesh::kBlockVertices);
    const auto emitTriangle = [&](const std::array<glm::ivec2, 3>& corners) {
        std::uint32_t packed[3];
        for (int i = 0; i < 3; ++i) packed[i] = texture(words[static_cast<std::size_t>(corners[i].y * kGrid + corners[i].x)]);
        for (int i = 0; i < 3; ++i) {
            const glm::ivec2 c = corners[static_cast<std::size_t>(i)];
            const glm::vec3 n = normal(c.x, c.y);
            TerrainVertex v{};
            v.position[0] = static_cast<float>(c.x) * kTileSize;
            v.position[1] = height(c.x, c.y);
            v.position[2] = static_cast<float>(c.y) * kTileSize;
            v.normal[0] = n.x;
            v.normal[1] = n.y;
            v.normal[2] = n.z;
            v.barycentric[i] = 1.0f;
            v.textures[0] = packed[0];
            v.textures[1] = packed[1];
            v.textures[2] = packed[2];
            mesh.terrain.push_back(v);
        }
    };
    for (int bz = 0; bz < kBlocksPerRegion; ++bz) {
        for (int bx = 0; bx < kBlocksPerRegion; ++bx) {
            glm::vec2 range(height(bx * kTilesPerBlock, bz * kTilesPerBlock));
            range.y = range.x;
            for (int tz = 0; tz < kTilesPerBlock; ++tz) {
                for (int tx = 0; tx < kTilesPerBlock; ++tx) {
                    const int x = bx * kTilesPerBlock + tx, z = bz * kTilesPerBlock + tz;
                    const glm::ivec2 c00(x, z), c10(x + 1, z), c01(x, z + 1), c11(x + 1, z + 1);
                    for (const glm::ivec2 c : {c00, c10, c01, c11}) {
                        range.x = std::min(range.x, height(c.x, c.y));
                        range.y = std::max(range.y, height(c.x, c.y));
                    }
                    // The split diagonal alternates like a checkerboard.
                    if ((x & 1) == (z & 1)) {
                        emitTriangle({c00, c01, c11});
                        emitTriangle({c00, c11, c10});
                    } else {
                        emitTriangle({c10, c00, c01});
                        emitTriangle({c10, c01, c11});
                    }
                }
            }
            mesh.blockHeights[static_cast<std::size_t>(bz * kBlocksPerRegion + bx)] = range;
        }
    }

    // Water as the original builds it: a block of water type 0 is a 17 x 17 grid at its water
    // level, each vertex as opaque as the water is deep there (clamp(trunc((water - ground) /
    // 2), 0, 15) / 15; a block with no wet vertex has none); a type 1 block with waves is one
    // quad of water\water201, opaque. The texture repeats 4 times a block. In each vertex:
    // barycentric (1, alpha, 1 for the type 1 quad); textures the slot of its texture.
    std::uint32_t waterSlot = 0, waveSlot = 0;
    for (int bz = 0; bz < kBlocksPerRegion; ++bz) {
        for (int bx = 0; bx < kBlocksPerRegion; ++bx) {
            const int index = bz * kBlocksPerRegion + bx;
            const auto& block = region.blocks[static_cast<std::size_t>(index)];
            if (block.waterType < 0) continue;
            const bool quad = block.waterType == 1 && block.waterCoverage != 0;
            const float y = block.waterHeight;
            const auto depthAt = [&](int x, int z) { return y - height(bx * kTilesPerBlock + x, bz * kTilesPerBlock + z); };
            const auto alphaAt = [&](int x, int z) {
                return static_cast<float>(std::clamp(static_cast<int>(depthAt(x, z) * 0.5f), 0, 15)) / 15.0f;
            };
            // Cells wholly above the water are left out; shallow ones stay (the original draws
            // them clear; modern water needs its whole surface up to the shore).
            if (!quad) {
                bool wet = false;
                for (int z = 0; z <= kTilesPerBlock && !wet; ++z) {
                    for (int x = 0; x <= kTilesPerBlock && !wet; ++x) wet = depthAt(x, z) > 0.0f;
                }
                if (!wet) continue;
            }
            std::uint32_t& slot = quad ? waveSlot : waterSlot;
            if (slot == 0) {
                mesh.texturePaths.push_back(quad ? std::string("water\\water201.ddj") : waterTexture);
                slot = static_cast<std::uint32_t>(mesh.texturePaths.size());
            }
            TerrainMesh::WaterRange range;
            range.block = static_cast<std::uint8_t>(index);
            range.first = static_cast<std::uint32_t>(mesh.water.size());
            const float x0 = static_cast<float>(bx) * assets::kBlockSize, z0 = static_cast<float>(bz) * assets::kBlockSize;
            const auto vertex = [&](int x, int z, float alpha) {
                TerrainVertex v{};
                v.position[0] = x0 + static_cast<float>(x) * kTileSize;
                v.position[1] = y;
                v.position[2] = z0 + static_cast<float>(z) * kTileSize;
                v.normal[1] = 1.0f;
                v.barycentric[0] = 1.0f;
                v.barycentric[1] = alpha;
                v.barycentric[2] = quad ? 1.0f : 0.0f;
                v.textures[0] = v.textures[1] = v.textures[2] = slot - 1;
                mesh.water.push_back(v);
            };
            const int cells = quad ? 1 : kTilesPerBlock;
            const int step = quad ? kTilesPerBlock : 1;
            for (int z = 0; z < cells; ++z) {
                for (int x = 0; x < cells; ++x) {
                    const int ax = x * step, az = z * step, bx1 = ax + step, bz1 = az + step;
                    const float a00 = quad ? 1.0f : alphaAt(ax, az), a01 = quad ? 1.0f : alphaAt(ax, bz1);
                    const float a11 = quad ? 1.0f : alphaAt(bx1, bz1), a10 = quad ? 1.0f : alphaAt(bx1, az);
                    if (!quad && depthAt(ax, az) <= 0.0f && depthAt(ax, bz1) <= 0.0f && depthAt(bx1, bz1) <= 0.0f && depthAt(bx1, az) <= 0.0f) {
                        continue;
                    }
                    vertex(ax, az, a00);
                    vertex(ax, bz1, a01);
                    vertex(bx1, bz1, a11);
                    vertex(ax, az, a00);
                    vertex(bx1, bz1, a11);
                    vertex(bx1, az, a10);
                }
            }
            range.count = static_cast<std::uint32_t>(mesh.water.size()) - range.first;
            if (range.count == 0) continue;
            mesh.waterBlocks.push_back(range);
            glm::vec2& heights = mesh.blockHeights[static_cast<std::size_t>(index)];
            heights = glm::vec2(std::min(heights.x, y), std::max(heights.y, y));
        }
    }
    return mesh;
}

GpuTerrain TerrainRenderer::Upload(TerrainMesh mesh, TextureCache& textures, const assets::TextureData* lightmap) {
    GpuTerrain gpu;
    if (lightmap != nullptr) {
        gpu.lightmap = UploadTexture(*device_, *lightmap, "lightmap");
        gpu.lightmapIndex = device_->BindlessIndex(gpu.lightmap);
    }
    gpu.missingTextures = mesh.unknownTextures;
    std::vector<std::uint32_t> bindless(mesh.texturePaths.size(), 0);
    for (std::size_t i = 0; i < mesh.texturePaths.size(); ++i) {
        if (mesh.texturePaths[i].empty()) continue;
        bindless[i] = device_->BindlessIndex(textures.Get(mesh.texturePaths[i]).handle);
        if (bindless[i] == 0) ++gpu.missingTextures;
    }
    const auto resolve = [&](std::vector<TerrainVertex>& vertices) {
        for (TerrainVertex& v : vertices) {
            for (int i = 0; i < 3; ++i) {
                const std::uint32_t slot = v.textures[i] & 0xFFFFFFu;
                v.textures[i] = (slot < bindless.size() ? bindless[slot] : 0u) | (v.textures[i] & 0xFF000000u);
            }
        }
    };
    resolve(mesh.terrain);
    resolve(mesh.water);
    if (!mesh.terrain.empty()) {
        gpu.terrain = device_->CreateBuffer(
            {mesh.terrain.size() * sizeof(TerrainVertex), rhi::BufferUsage::Vertex, "terrain", false},
            std::as_bytes(std::span(mesh.terrain)));
        gpu.terrainVertices = static_cast<std::uint32_t>(mesh.terrain.size());
    }
    if (!mesh.water.empty()) {
        gpu.water = device_->CreateBuffer({mesh.water.size() * sizeof(TerrainVertex), rhi::BufferUsage::Vertex, "water", false},
                                          std::as_bytes(std::span(mesh.water)));
        gpu.waterVertices = static_cast<std::uint32_t>(mesh.water.size());
    }
    gpu.waterBlocks = std::move(mesh.waterBlocks);
    gpu.blockHeights = mesh.blockHeights;
    return gpu;
}

void TerrainRenderer::Release(GpuTerrain& terrain) {
    device_->DestroyBuffer(terrain.terrain);
    device_->DestroyBuffer(terrain.water);
    if (terrain.lightmap) device_->DestroyTexture(terrain.lightmap);
    terrain = {};
}

void TerrainRenderer::DrawBlocks(rhi::CommandList& cmd, const GpuTerrain& terrain, const glm::vec3& offset,
                                 const Frustum* frustum) const {
    cmd.BindVertexBuffer(terrain.terrain, 0);
    if (frustum == nullptr || terrain.terrainVertices != kRegionBlocks * TerrainMesh::kBlockVertices) {
        cmd.Draw(terrain.terrainVertices, 0);
        return;
    }
    const auto visible = [&](std::size_t b) {
        const float x = offset.x + static_cast<float>(b % kBlocksPerRegion) * assets::kBlockSize;
        const float z = offset.z + static_cast<float>(b / kBlocksPerRegion) * assets::kBlockSize;
        const glm::vec2 h = terrain.blockHeights[b];
        return frustum->Box({x, offset.y + h.x, z}, {x + assets::kBlockSize, offset.y + h.y, z + assets::kBlockSize});
    };
    ForEachRun(kRegionBlocks, visible, [&](std::size_t first, std::size_t count) {
        cmd.Draw(static_cast<std::uint32_t>(count) * TerrainMesh::kBlockVertices,
                 static_cast<std::uint32_t>(first) * TerrainMesh::kBlockVertices);
    });
}

void TerrainRenderer::DrawTerrain(rhi::CommandList& cmd, const GpuTerrain& terrain, const glm::mat4& viewProj,
                                  const glm::vec3& offset, bool textured, bool wireframe, const Frustum* frustum) const {
    if (terrain.terrainVertices == 0) return;
    PushData push{};
    push.viewProj = viewProj * glm::translate(glm::mat4(1.0f), offset);
    push.offset = glm::vec4(offset, 0.0f);
    push.frame = frame_;
    push.mode = textured && !wireframe ? 0 : 2;
    push.lightmap = terrain.lightmapIndex;
    cmd.BindPipeline(wireframe ? wire_ : solid_);
    cmd.PushConstants(&push, sizeof(push));
    DrawBlocks(cmd, terrain, offset, frustum);
}

void TerrainRenderer::DrawShadow(rhi::CommandList& cmd, const GpuTerrain& terrain, const glm::mat4& viewProj,
                                 const glm::vec3& offset, const Frustum& frustum) const {
    if (terrain.terrainVertices == 0 || !shadow_) return;
    PushData push{};
    push.viewProj = viewProj * glm::translate(glm::mat4(1.0f), offset);
    push.offset = glm::vec4(offset, 0.0f);
    cmd.BindPipeline(prepassMode_ ? prepass_ : shadow_);
    cmd.PushConstants(&push, sizeof(push));
    DrawBlocks(cmd, terrain, offset, &frustum);
}

void TerrainRenderer::DrawWater(rhi::CommandList& cmd, const GpuTerrain& terrain, const glm::mat4& viewProj,
                                const glm::vec3& offset, float timeSeconds, const Frustum* frustum, bool modern,
                                std::uint32_t frameTexture) const {
    if (terrain.waterVertices == 0) return;
    PushData push{};
    push.viewProj = viewProj * glm::translate(glm::mat4(1.0f), offset);
    push.offset = glm::vec4(offset, 0.0f);
    push.frame = frame_;
    push.mode = modern ? 3 : 1;
    push.timeSeconds = timeSeconds;
    push.lightmap = frameTexture; // the water's animation frame
    cmd.BindPipeline(modern ? waterModern_ : water_);
    cmd.PushConstants(&push, sizeof(push));
    cmd.BindVertexBuffer(terrain.water, 0);
    if (frustum == nullptr) {
        cmd.Draw(terrain.waterVertices, 0);
        return;
    }
    // The blocks in view; neighbours in the buffer go in one draw.
    std::uint32_t runFirst = 0, runCount = 0;
    for (const TerrainMesh::WaterRange& r : terrain.waterBlocks) {
        const float x = offset.x + static_cast<float>(r.block % kBlocksPerRegion) * assets::kBlockSize;
        const float z = offset.z + static_cast<float>(r.block / kBlocksPerRegion) * assets::kBlockSize;
        const glm::vec2 h = terrain.blockHeights[r.block];
        if (!frustum->Box({x, offset.y + h.x, z}, {x + assets::kBlockSize, offset.y + h.y, z + assets::kBlockSize})) continue;
        if (runCount > 0 && runFirst + runCount == r.first) {
            runCount += r.count;
            continue;
        }
        if (runCount > 0) cmd.Draw(runCount, runFirst);
        runFirst = r.first;
        runCount = r.count;
    }
    if (runCount > 0) cmd.Draw(runCount, runFirst);
}

} // namespace sr::render
