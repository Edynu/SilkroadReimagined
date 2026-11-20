#include "sr/render/ModelRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "kMeshPixelShader.spv.h"
#include "kMeshShadowShader.spv.h"
#include "kMeshVertexShader.spv.h"
#include "kSkinnedVertexShader.spv.h"
#include "sr/assets/Model.h"
#include "sr/render/TextureCache.h"

namespace sr::render {

namespace {

struct PushData {
    glm::mat4 viewProj;
    std::uint64_t frame; // FrameConstants address (0: the shader's defaults)
    float swayHeight;    // foliage: the model's height (0: no sway)
    float pad0;
    std::uint32_t textureIndex;
    std::uint32_t mode;
    float alphaCutoff;
    std::uint32_t tint; // RGBA8
    std::uint64_t dataAddress; // static: instance world matrices; skinned: the bone palette
    float pad[2];
    glm::vec4 uvTransform;
};
static_assert(sizeof(PushData) <= 128);
static_assert(offsetof(PushData, dataAddress) == 96);
static_assert(offsetof(PushData, uvTransform) == 112);

std::uint32_t PackTint(const glm::vec4& c) {
    const auto b = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return b(c.r) | b(c.g) << 8 | b(c.b) << 16 | b(c.a) << 24;
}

struct SkinnedVertex {
    float position[3];
    float normal[3];
    float uv[2];
    std::uint8_t bones[4]; // skeleton bone indices; only the first two are used
    float weights[2];
};

constexpr float kAlphaCutoff = 128.0f / 255.0f; // the original's constant reference (GREATEREQUAL)

} // namespace

bool ModelRenderer::Init(rhi::Device& device, rhi::Format colorFormat, rhi::Format depthFormat, std::string& error) {
    device_ = &device;
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = std::as_bytes(std::span(kMeshVertexShader));
    desc.fragmentShader = std::as_bytes(std::span(kMeshPixelShader));
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = "PSMain";
    desc.vertexStride = sizeof(assets::MeshVertex);
    desc.attributes = {
        {0, rhi::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(assets::MeshVertex, position))},
        {1, rhi::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(assets::MeshVertex, normal))},
        {2, rhi::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(assets::MeshVertex, uv))},
    };
    // Culling stays off until the original winding/two-sided rules are known.
    desc.cull = rhi::CullMode::None;
    desc.depthTest = true;
    desc.depthWrite = true;
    desc.colorFormat = colorFormat;
    desc.depthFormat = depthFormat;
    desc.debugName = "mesh static";
    staticSolid_ = device.CreateGraphicsPipeline(desc);
    desc.wireframe = true;
    staticWire_ = device.CreateGraphicsPipeline(desc);

    desc.vertexShader = std::as_bytes(std::span(kSkinnedVertexShader));
    desc.vertexEntry = "VSSkinned";
    desc.vertexStride = sizeof(SkinnedVertex);
    desc.attributes = {
        {0, rhi::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(SkinnedVertex, position))},
        {1, rhi::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(SkinnedVertex, normal))},
        {2, rhi::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(SkinnedVertex, uv))},
        {3, rhi::VertexFormat::UByte4, static_cast<std::uint32_t>(offsetof(SkinnedVertex, bones))},
        {4, rhi::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(SkinnedVertex, weights))},
    };
    desc.debugName = "mesh skinned";
    desc.wireframe = false;
    skinnedSolid_ = device.CreateGraphicsPipeline(desc);
    desc.wireframe = true;
    skinnedWire_ = device.CreateGraphicsPipeline(desc);

    // Blended parts (material modifiers that switch blending on): skinned and static, each
    // with and without depth writes.
    desc.wireframe = false;
    for (int skinned = 1; skinned >= 0; --skinned) {
        if (skinned == 0) {
            desc.vertexShader = std::as_bytes(std::span(kMeshVertexShader));
            desc.vertexEntry = "VSMain";
            desc.vertexStride = sizeof(assets::MeshVertex);
            desc.attributes = {
                {0, rhi::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(assets::MeshVertex, position))},
                {1, rhi::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(assets::MeshVertex, normal))},
                {2, rhi::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(assets::MeshVertex, uv))},
            };
        }
        for (int additive = 0; additive < 2; ++additive) {
            for (int write = 0; write < 2; ++write) {
                desc.blend = additive ? rhi::BlendMode::Additive : rhi::BlendMode::AlphaBlend;
                desc.depthWrite = write != 0;
                desc.debugName = std::string("mesh ") + (skinned ? "skinned " : "static ") + (additive ? "additive" : "alpha") +
                                 (write ? " +depth" : "");
                blended_[skinned][additive][write] = device.CreateGraphicsPipeline(desc);
            }
        }
    }

    // Shadow maps: depth only, alpha tested, biased against acne.
    {
        rhi::GraphicsPipelineDesc shadow;
        shadow.fragmentShader = std::as_bytes(std::span(kMeshShadowShader));
        shadow.fragmentEntry = "PSShadow";
        shadow.depthTest = true;
        shadow.depthWrite = true;
        shadow.depthOnly = true;
        shadow.depthFormat = rhi::Format::D32Float;
        shadow.depthBiasConstant = 1.0f;
        shadow.depthBiasSlope = 2.0f;
        shadow.vertexShader = std::as_bytes(std::span(kMeshVertexShader));
        shadow.vertexEntry = "VSMain";
        shadow.vertexStride = sizeof(assets::MeshVertex);
        shadow.attributes = {
            {0, rhi::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(assets::MeshVertex, position))},
            {1, rhi::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(assets::MeshVertex, normal))},
            {2, rhi::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(assets::MeshVertex, uv))},
        };
        shadow.debugName = "mesh static shadow";
        shadowStatic_ = device.CreateGraphicsPipeline(shadow);
        // The camera's depth pre-pass: a hair of constant bias only (the scene pass tests
        // against it; a slope bias would let what lies just behind a grazing floor through).
        shadow.depthBiasConstant = 4.0f;
        shadow.depthBiasSlope = 0.0f;
        shadow.debugName = "mesh static pre-pass";
        prepassStatic_ = device.CreateGraphicsPipeline(shadow);
        shadow.depthBiasConstant = 1.0f;
        shadow.depthBiasSlope = 2.0f;
        shadow.vertexShader = std::as_bytes(std::span(kSkinnedVertexShader));
        shadow.vertexEntry = "VSSkinned";
        shadow.vertexStride = sizeof(SkinnedVertex);
        shadow.attributes = {
            {0, rhi::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(SkinnedVertex, position))},
            {1, rhi::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(SkinnedVertex, normal))},
            {2, rhi::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(SkinnedVertex, uv))},
            {3, rhi::VertexFormat::UByte4, static_cast<std::uint32_t>(offsetof(SkinnedVertex, bones))},
            {4, rhi::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(SkinnedVertex, weights))},
        };
        shadow.debugName = "mesh skinned shadow";
        shadowSkinned_ = device.CreateGraphicsPipeline(shadow);
        shadow.depthBiasConstant = 4.0f;
        shadow.depthBiasSlope = 0.0f;
        shadow.debugName = "mesh skinned pre-pass";
        prepassSkinned_ = device.CreateGraphicsPipeline(shadow);
    }

    bool blendedOk = true;
    for (auto& a : blended_) {
        for (auto& b : a) {
            for (const rhi::PipelineHandle& p : b) blendedOk = blendedOk && static_cast<bool>(p);
        }
    }
    if (!staticSolid_ || !staticWire_ || !skinnedSolid_ || !skinnedWire_ || !shadowStatic_ || !shadowSkinned_ || !prepassStatic_ ||
        !prepassSkinned_ || !blendedOk) {
        error = "could not create the mesh pipelines";
        return false;
    }
    return true;
}

void ModelRenderer::Shutdown() {
    if (device_ == nullptr) return;
    for (rhi::PipelineHandle p : {staticSolid_, staticWire_, skinnedSolid_, skinnedWire_, shadowStatic_, shadowSkinned_, prepassStatic_,
                                  prepassSkinned_}) {
        if (p) device_->DestroyPipeline(p);
    }
    for (auto& a : blended_) {
        for (auto& b : a) {
            for (rhi::PipelineHandle& p : b) {
                if (p) device_->DestroyPipeline(p);
            }
        }
    }
    device_ = nullptr;
}

GpuModel ModelRenderer::Upload(const assets::ModelData& model, TextureCache& textures, bool alphaFromMaterial,
                               bool staticOnly) {
    GpuModel gpu;
    const bool hasSkeleton = !staticOnly && !model.skeleton.bones.empty() && model.skeleton.bones.size() <= 256;
    bool anyVertex = false;
    for (const assets::ModelPart& part : model.parts) {
        GpuModelPart out;
        const auto& mesh = part.mesh;
        for (const assets::MeshVertex& v : mesh.vertices) {
            const glm::vec3 p(v.position[0], v.position[1], v.position[2]);
            gpu.boundsMin = anyVertex ? glm::min(gpu.boundsMin, p) : p;
            gpu.boundsMax = anyVertex ? glm::max(gpu.boundsMax, p) : p;
            anyVertex = true;
        }
        if (hasSkeleton && mesh.IsSkinned()) {
            // Mesh bone indices refer to the mesh's own bone list; rewrite them as skeleton indices.
            std::vector<std::uint8_t> remap(mesh.boneNames.size(), 0);
            for (std::size_t b = 0; b < mesh.boneNames.size(); ++b) {
                const int bone = model.skeleton.Find(mesh.boneNames[b]);
                if (bone < 0) ++gpu.unmatchedBones;
                remap[b] = static_cast<std::uint8_t>(bone < 0 ? 0 : bone);
            }
            std::vector<SkinnedVertex> vertices(mesh.vertices.size());
            for (std::size_t v = 0; v < vertices.size(); ++v) {
                SkinnedVertex& sv = vertices[v];
                std::memcpy(sv.position, mesh.vertices[v].position, sizeof(sv.position));
                std::memcpy(sv.normal, mesh.vertices[v].normal, sizeof(sv.normal));
                std::memcpy(sv.uv, mesh.vertices[v].uv, sizeof(sv.uv));
                for (int i = 0; i < 2; ++i) {
                    const std::uint8_t local = mesh.boneIndices[v][static_cast<std::size_t>(i)];
                    sv.bones[i] = local < remap.size() ? remap[local] : 0;
                    sv.weights[i] = mesh.boneWeights[v][static_cast<std::size_t>(i)];
                }
                sv.bones[2] = sv.bones[3] = 0;
            }
            out.vertices = device_->CreateBuffer(
                {vertices.size() * sizeof(SkinnedVertex), rhi::BufferUsage::Vertex, part.meshPath, false},
                std::as_bytes(std::span(vertices)));
            out.skinned = true;
        } else {
            out.vertices = device_->CreateBuffer(
                {mesh.vertices.size() * sizeof(assets::MeshVertex), rhi::BufferUsage::Vertex, part.meshPath, false},
                std::as_bytes(std::span(mesh.vertices)));
        }
        out.indices = device_->CreateBuffer(
            {mesh.indices.size() * sizeof(std::uint16_t), rhi::BufferUsage::Index, part.meshPath, false},
            std::as_bytes(std::span(mesh.indices)));
        out.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
        if (part.hasMaterial && !part.material.texturePath.empty()) {
            const TextureCache::Entry& texture = textures.Get(part.material.texturePath);
            out.textureIndex = device_->BindlessIndex(texture.handle);
            out.textured = static_cast<bool>(texture.handle);
        }
        if (alphaFromMaterial) {
            const std::uint32_t flags = part.hasMaterial ? part.material.flags : 0;
            out.alphaTest = part.allowAlphaTest && (flags & 0x200u) != 0 && (flags & 0x30000000u) == 0;
        }
        gpu.parts.push_back(out);
    }
    return gpu;
}

void ModelRenderer::Release(GpuModel& model) {
    for (GpuModelPart& part : model.parts) {
        device_->DestroyBuffer(part.vertices);
        device_->DestroyBuffer(part.indices);
    }
    model.parts.clear();
}

void ModelRenderer::Draw(rhi::CommandList& cmd, const GpuModel& model, const glm::mat4& viewProj, ShadingMode mode,
                         bool wireframe, std::span<const glm::mat4> palette, std::span<const std::uint8_t> visible,
                         const glm::mat4& world, std::span<const PartLook> looks) const {
    bool anyStatic = false, anySkinned = false;
    for (const GpuModelPart& part : model.parts) (part.skinned ? anySkinned : anyStatic) = true;
    // Static parts: one instance whose world matrix is in transient memory.
    std::uint64_t instance = 0;
    if (anyStatic) {
        const rhi::TransientAllocation alloc = device_->AllocateTransient(sizeof(glm::mat4), 64);
        if (alloc.cpu == nullptr) return;
        std::memcpy(alloc.cpu, &world, sizeof(glm::mat4));
        instance = alloc.gpuAddress;
    }
    // Skinned parts: the world matrix, then the bone palette (without one, the bind pose).
    std::uint64_t paletteAddress = 0;
    if (anySkinned) {
        const std::size_t bones = palette.empty() ? 256 : palette.size();
        const rhi::TransientAllocation alloc = device_->AllocateTransient((bones + 1) * sizeof(glm::mat4), 64);
        if (alloc.cpu == nullptr) return;
        auto* out = static_cast<glm::mat4*>(alloc.cpu);
        out[0] = world;
        if (palette.empty()) {
            std::fill(out + 1, out + 1 + bones, glm::mat4(1.0f));
        } else {
            std::memcpy(out + 1, palette.data(), palette.size_bytes());
        }
        paletteAddress = alloc.gpuAddress;
    }
    DrawParts(cmd, model, viewProj, mode, wireframe, instance, 1, visible, looks, anySkinned, paletteAddress);
}

void ModelRenderer::DrawInstanced(rhi::CommandList& cmd, const GpuModel& model, const glm::mat4& viewProj, ShadingMode mode,
                                  bool wireframe, std::uint64_t instances, std::uint32_t count,
                                  std::span<const PartLook> looks) const {
    if (count == 0 || instances == 0) return;
    DrawParts(cmd, model, viewProj, mode, wireframe, instances, count, {}, looks, false, 0);
}

void ModelRenderer::DrawShadow(rhi::CommandList& cmd, const GpuModel& model, const glm::mat4& viewProj,
                               std::span<const glm::mat4> palette, const glm::mat4& world, std::span<const PartLook> looks) const {
    shadowPass_ = true;
    Draw(cmd, model, viewProj, ShadingMode::Textured, false, palette, {}, world, looks);
    shadowPass_ = false;
}

void ModelRenderer::DrawShadowInstanced(rhi::CommandList& cmd, const GpuModel& model, const glm::mat4& viewProj,
                                        std::uint64_t instances, std::uint32_t count, std::span<const PartLook> looks) const {
    shadowPass_ = true;
    DrawInstanced(cmd, model, viewProj, ShadingMode::Textured, false, instances, count, looks);
    shadowPass_ = false;
}

void ModelRenderer::DrawParts(rhi::CommandList& cmd, const GpuModel& model, const glm::mat4& viewProj, ShadingMode mode,
                              bool wireframe, std::uint64_t instances, std::uint32_t count,
                              std::span<const std::uint8_t> visible, std::span<const PartLook> looks, bool skinned,
                              std::uint64_t palette) const {
    PushData push{};
    push.viewProj = viewProj;
    push.frame = frame_;
    push.swayHeight = model.swayHeight;
    rhi::PipelineHandle bound;
    for (std::size_t i = 0; i < model.parts.size(); ++i) {
        const GpuModelPart& part = model.parts[i];
        if ((!visible.empty() && i < visible.size() && visible[i] == 0) || part.indexCount == 0) continue;
        if (part.skinned && !skinned) continue;
        const PartLook* look = i < looks.size() ? &looks[i] : nullptr;
        if (look != nullptr && look->hidden) continue;
        const int blend = wireframe || look == nullptr ? 0 : look->blend;
        if (shadowPass_ && blend == 2) continue; // glows cast no shadow
        // The pre-pass holds only what the scene pass writes depth for.
        if (shadowPass_ && prepass_ && blend != 0 && !look->depthWrite) continue;
        const rhi::PipelineHandle pipeline =
            shadowPass_ ? (prepass_ ? (part.skinned ? prepassSkinned_ : prepassStatic_) : (part.skinned ? shadowSkinned_ : shadowStatic_))
            : blend != 0 ? blended_[part.skinned ? 1 : 0][blend == 2 ? 1 : 0][look->depthWrite ? 1 : 0]
            : part.skinned ? (wireframe ? skinnedWire_ : skinnedSolid_)
                           : (wireframe ? staticWire_ : staticSolid_);
        if (look != nullptr) {
            glm::vec4 tint = look->tint;
            if (blend == 0) tint.a = 1.0f;
            else tint.a = std::min(tint.a, 254.0f / 255.0f); // below 255: the shader keeps the alpha
            push.tint = PackTint(tint);
            push.uvTransform = look->uvTransform;
        } else {
            push.tint = 0xFFFFFFFFu;
            push.uvTransform = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
        }
        if (pipeline != bound) {
            cmd.BindPipeline(pipeline);
            bound = pipeline;
        }
        push.dataAddress = part.skinned ? palette : instances;
        // Parts without a usable texture fall back to flat shading.
        const ShadingMode partMode = mode == ShadingMode::Textured && !part.textured ? ShadingMode::Untextured : mode;
        push.mode = static_cast<std::uint32_t>(partMode) | (blend == 2 ? 0x100u : 0u);
        push.textureIndex = part.textureIndex;
        push.alphaCutoff = wireframe || !part.alphaTest ? -1.0f : kAlphaCutoff;
        // The material's own alpha test (a modifier's reference), where it has one.
        if (look != nullptr && !wireframe && look->alphaCutoff > -1.5f) push.alphaCutoff = look->alphaCutoff;
        // A blended part (foliage) casts where it is mostly opaque.
        if (shadowPass_ && blend == 1 && push.alphaCutoff < 0.0f) push.alphaCutoff = 0.5f;
        cmd.PushConstants(&push, sizeof(push));
        cmd.BindVertexBuffer(part.vertices, 0);
        cmd.BindIndexBuffer(part.indices, 0, false);
        if (part.skinned || count == 1) {
            cmd.DrawIndexed(part.indexCount, 0, 0);
        } else {
            cmd.DrawIndexedInstanced(part.indexCount, count, 0, 0);
        }
    }
}

namespace {

// A modifier's clock: loop (0), back and forth (1) or hold at the end (2).
float Clock(float timeMs, float durationMs, std::uint32_t mode) {
    if (durationMs <= 0.0f) return 0.0f;
    if (mode == 2) return std::min(timeMs, durationMs);
    if (mode == 1) {
        const float t = std::fmod(timeMs, 2.0f * durationMs);
        return t <= durationMs ? t : 2.0f * durationMs - t;
    }
    return std::fmod(timeMs, durationMs);
}

} // namespace

std::vector<PartLook> AmbientLooks(const assets::ModelData& model, float timeMs, bool scenery) {
    std::vector<PartLook> looks;
    for (const auto& set : model.resource.modifierSets) {
        if (set.kind != 2) continue; // only the always-on sets here
        for (const auto& m : set.materials) {
            if (looks.empty()) looks.resize(model.parts.size());
            glm::vec4 colour(1.0f);
            if (!m.keys.empty() && (m.colourFlags & 3) != 0) {
                const float t = Clock(timeMs, static_cast<float>(m.durationMs), m.mode);
                std::size_t k = 0;
                while (k + 1 < m.keys.size() && static_cast<float>(m.keys[k + 1].ms) <= t) ++k;
                const auto& a = m.keys[k];
                const auto& b = m.keys[std::min(k + 1, m.keys.size() - 1)];
                const float span = static_cast<float>(b.ms) - static_cast<float>(a.ms);
                const float w = span > 0.0f ? std::clamp((t - static_cast<float>(a.ms)) / span, 0.0f, 1.0f) : 0.0f;
                for (int c = 0; c < 4; ++c) colour[c] = a.rgba[c] + (b.rgba[c] - a.rgba[c]) * w;
            }
            const bool blended = m.enables[1] != 0;
            const int blend = !blended ? 0 : m.states[1] == 2 ? 2 : 1; // destination ONE: additive
            if (m.enables[3] != 0 && m.pulseRate > 0.0f && m.states[11] > m.states[10]) {
                // The texture factor's alpha going up and down between its two bounds.
                const float low = m.states[10], high = m.states[11];
                const float period = 2.0f * (high - low) / (m.pulseRate * 0.01f);
                const float t = std::fmod(timeMs, period);
                const float v = low + (t <= period * 0.5f ? t : period - t) * m.pulseRate * 0.01f;
                colour.a *= v / 255.0f;
            }
            // A placed object's glow pulses but never goes out (as the original shows them): at
            // least 0.3 of its brightest.
            if (scenery && blended) {
                float peak = 1.0f;
                if (!m.keys.empty() && (m.colourFlags & 3) != 0) {
                    peak = 0.0f;
                    for (const auto& key : m.keys) peak = std::max(peak, key.rgba[3]);
                }
                if (m.enables[3] != 0 && m.pulseRate > 0.0f && m.states[11] > m.states[10]) peak *= m.states[11] / 255.0f;
                colour.a = std::max(colour.a, 0.3f * peak);
            }
            // Alpha test (enables[0]: reference in states[8]) and depth writes (off when enables[2]).
            const float cutoff = m.enables[0] != 0 ? static_cast<float>(m.states[8]) / 255.0f : -1.0f;
            for (std::size_t i = 0; i < model.parts.size(); ++i) {
                if (m.material != 0xFFFFFFFFu && static_cast<std::uint32_t>(model.parts[i].materialIndex) != m.material) continue;
                looks[i].tint = glm::vec4(glm::vec3(colour), blend == 0 ? 1.0f : colour.a);
                looks[i].blend = blend;
                looks[i].depthWrite = m.enables[2] == 0 && (scenery || blend == 0);
                looks[i].alphaCutoff = cutoff;
            }
        }
        for (const auto& tm : set.textures) {
            if (looks.empty()) looks.resize(model.parts.size());
            glm::vec4 uv(1.0f, 1.0f, 0.0f, 0.0f);
            if (tm.words[0] == 0 && tm.words[1] == 0 && tm.words[2] > 0 && tm.words[3] > 0 && tm.words[4] > 0) {
                // A flip-book: frames start..end of a rows x columns sheet at fps.
                const int columns = static_cast<int>(tm.words[4]), rows = static_cast<int>(tm.words[3]);
                const int first = tm.start, last = std::max<int>(tm.start, tm.end);
                const int count = last - first + 1;
                const int step = std::max(1, static_cast<int>(1000 / tm.words[2]));
                int n = static_cast<int>(timeMs) / step;
                if (tm.bookFlags & 4) {
                    n %= std::max(1, 2 * count - 2);
                    if (n >= count) n = 2 * count - 2 - n;
                } else if (tm.bookFlags & 2) {
                    n %= count;
                } else {
                    n = std::min(n, count - 1);
                }
                const int frame = first + n;
                uv = glm::vec4(1.0f / columns, 1.0f / rows, static_cast<float>(frame % columns) / columns,
                               static_cast<float>(frame / columns) / rows);
            } else if (tm.words[1] == 1) {
                // Scrolling: u / v per second.
                const float s = timeMs / 1000.0f;
                uv = glm::vec4(1.0f, 1.0f, std::fmod(tm.matrix[8] * s, 1.0f), std::fmod(tm.matrix[9] * s, 1.0f));
            } else {
                continue;
            }
            for (std::size_t i = 0; i < model.parts.size(); ++i) {
                if (tm.material != 0xFFFFFFFFu && static_cast<std::uint32_t>(model.parts[i].materialIndex) != tm.material) continue;
                looks[i].uvTransform = uv;
            }
        }
    }
    return looks;
}

} // namespace sr::render
