#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sr/rhi/Rhi.h"

namespace sr::assets {
struct ModelData;
}

namespace sr::render {

class TextureCache;

struct GpuModelPart {
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    std::uint32_t indexCount = 0;
    std::uint32_t textureIndex = 0; // bindless; 0 = white
    bool textured = false;
    bool skinned = false; // vertices carry skeleton bone indices + weights
    bool alphaTest = true; // discard texels with alpha < 0.5
};

struct GpuModel {
    std::vector<GpuModelPart> parts; // same order as ModelData::parts
    int unmatchedBones = 0;          // mesh bones missing from the skeleton (bound to bone 0)
    // A box around every part's vertices in the model's space (the bind pose), for culling.
    glm::vec3 boundsMin{0.0f}, boundsMax{0.0f};
    // Foliage (trees, grass): the height it sways over in the wind; 0 it stands still.
    float swayHeight = 0.0f;
};

enum class ShadingMode : std::uint32_t { Textured = 0, Normals = 1, Untextured = 2, UVs = 3 };

// How one part looks right now under its .bsr modifiers: a colour, its blending and its
// texture window (flip-book frame or scroll).
struct PartLook {
    glm::vec4 tint{1.0f};
    glm::vec4 uvTransform{1.0f, 1.0f, 0.0f, 0.0f}; // uv * xy + zw
    int blend = 0; // 0 opaque, 1 alpha, 2 additive
    bool depthWrite = true; // a blended part writes depth unless its material says not to
    float alphaCutoff = -2.0f; // -2 the part's own rule, -1 none, else the material's reference (0..1)
    bool hidden = false;
};
// The looks of a model's parts at `timeMs` from its always-on ("ambient") modifier sets;
// empty when it has none that change materials.
// `scenery`: placed world objects write depth when blended (unless their material says not
// to); a blended character part (an effect material) does not.
std::vector<PartLook> AmbientLooks(const assets::ModelData& model, float timeMs, bool scenery = false);

// Draws decoded models (.bsr/.cpd/.bms), static or skinned, into a colour + depth target.
class ModelRenderer {
public:
    bool Init(rhi::Device& device, rhi::Format colorFormat, rhi::Format depthFormat, std::string& error);
    void Shutdown();

    // Skinned meshes are uploaded as skinned only when the model has a skeleton.
    // alphaFromMaterial: alpha-test only materials whose flags say the texture's
    // alpha is transparency (0x200 without 0x30000000); many character textures keep
    // specular data in alpha and must stay
    // opaque. Otherwise every textured part is alpha-tested (world objects for now).
    // `staticOnly`: skinned meshes are uploaded as static ones (scenery drawn in its bind
    // pose), so the whole model can be drawn instanced.
    GpuModel Upload(const assets::ModelData& model, TextureCache& textures, bool alphaFromMaterial = false,
                    bool staticOnly = false);
    void Release(GpuModel& model);

    // `palette`: skinning matrices (one per skeleton bone) for skinned parts;
    // empty draws skinned parts in the bind pose. `visible` (optional) hides parts.
    // `world` places the model (rotation + translation, no scale).
    // Call between BeginFrame and EndFrame (the palette goes to transient memory).
    void Draw(rhi::CommandList& cmd, const GpuModel& model, const glm::mat4& viewProj, ShadingMode mode,
              bool wireframe, std::span<const glm::mat4> palette = {},
              std::span<const std::uint8_t> visible = {}, const glm::mat4& world = glm::mat4(1.0f),
              std::span<const PartLook> looks = {}) const;
    // Draws `count` copies of a model, one draw call per part: `instances` is the GPU address
    // of `count` world matrices (glm::mat4, rotation + translation, e.g. AllocateTransient
    // memory). Skinned parts are skipped (upload such models with staticOnly).
    void DrawInstanced(rhi::CommandList& cmd, const GpuModel& model, const glm::mat4& viewProj, ShadingMode mode,
                       bool wireframe, std::uint64_t instances, std::uint32_t count,
                       std::span<const PartLook> looks = {}) const;
    // Into a shadow map (depth only, alpha tested; additive parts cast nothing): one model,
    // or `count` copies of a static one as DrawInstanced.
    void DrawShadow(rhi::CommandList& cmd, const GpuModel& model, const glm::mat4& viewProj,
                    std::span<const glm::mat4> palette = {}, const glm::mat4& world = glm::mat4(1.0f),
                    std::span<const PartLook> looks = {}) const;
    void DrawShadowInstanced(rhi::CommandList& cmd, const GpuModel& model, const glm::mat4& viewProj, std::uint64_t instances,
                             std::uint32_t count, std::span<const PartLook> looks = {}) const;
    // The frame's constants (render/FrameConstants.h, transient memory of this frame) for the
    // draws that follow; 0 uses the shader's defaults. Reset it to 0 when the frame's draws are done.
    void SetFrame(std::uint64_t address) const { frame_ = address; }
    // While on, the DrawShadow calls draw the camera's depth pre-pass (no slope bias).
    void SetPrepass(bool on) const { prepass_ = on; }

private:
    // `skinned`: draw the skinned parts too, with `palette` (world matrix, then the bones).
    void DrawParts(rhi::CommandList& cmd, const GpuModel& model, const glm::mat4& viewProj, ShadingMode mode,
                   bool wireframe, std::uint64_t instances, std::uint32_t count, std::span<const std::uint8_t> visible,
                   std::span<const PartLook> looks, bool skinned, std::uint64_t palette) const;
    mutable std::uint64_t frame_ = 0;

    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle staticSolid_, staticWire_, skinnedSolid_, skinnedWire_;
    rhi::PipelineHandle shadowStatic_, shadowSkinned_, prepassStatic_, prepassSkinned_;
    mutable bool prepass_ = false;
    mutable bool shadowPass_ = false; // DrawParts picks the shadow pipelines
    // Blended pipelines [skinned][additive][depth written]: depth is always tested; whether a
    // part writes it is up to its material (foliage does, glows do not).
    rhi::PipelineHandle blended_[2][2][2];
};

} // namespace sr::render
