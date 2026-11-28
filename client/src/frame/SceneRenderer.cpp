#include "frame/SceneRenderer.h"

#include <algorithm>
#include <bit>
#include <span>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "kBlitPixelShader.spv.h"
#include "kBlitVertexShader.spv.h"
#include "kDepthProbeComputeShader.spv.h"
#include "sr/core/Log.h"
#ifdef SR_AUDIO_OCCLUSION
#include "kAudioCubeComputeShader.spv.h"
#endif
#include "sr/core/Trace.h"

namespace sr::client {

namespace {

template <std::size_t N>
std::span<const std::byte> Spirv(const unsigned char (&code)[N]) {
    return {reinterpret_cast<const std::byte*>(code), N};
}

struct BlitPush {
    std::uint32_t textureIndex = 0;
    std::uint32_t samplerIndex = 0;
};

glm::mat4 ViewProj(const OrbitCamera& c, float aspect, glm::vec3& eye, glm::mat4& sky, glm::mat4* viewOut = nullptr, glm::mat4* projOut = nullptr,
                   glm::mat4* skyProjOut = nullptr) {
    const glm::vec3 target(c.targetX, c.targetY, c.targetZ);
    const glm::vec3 dir(std::cos(c.pitch) * std::sin(c.yaw), std::sin(c.pitch), std::cos(c.pitch) * std::cos(c.yaw));
    eye = c.fly ? target : target + dir * c.distance;
    const glm::mat4 view = c.fly ? glm::lookAt(target, target + dir, glm::vec3(0, 1, 0))
                                 : glm::lookAt(target + dir * c.distance, target, glm::vec3(0, 1, 0));
    // Unless the camera sets them, near and far follow the orbit distance, so a ring and a
    // fortress both fit the depth range.
    const float nearZ = c.nearZ > 0 ? c.nearZ : std::max(0.05f, c.distance * 0.01f);
    const float farZ = c.farZ > 0 ? c.farZ : c.distance * 20.0f + 1000.0f;
    glm::mat4 proj = glm::perspective(c.fovY, aspect, nearZ, farZ);
    proj[1][1] *= -1.0f; // Vulkan's y points down
    // The sky: centred on the camera, out to 5,000,000 (the original's far plane for it).
    glm::mat4 skyProj = glm::perspective(c.fovY, aspect, 10.0f, 5000000.0f);
    skyProj[1][1] *= -1.0f;
    sky = skyProj * glm::mat4(glm::mat3(view));
    if (viewOut != nullptr) *viewOut = view;
    if (projOut != nullptr) *projOut = proj;
    if (skyProjOut != nullptr) *skyProjOut = skyProj;
    return proj * view;
}

// The reflection's camera: the view mirrored about the water level (y = level), its near plane turned onto the water's
// plane (an oblique projection: what lies under the water is clipped, the original's user clip plane), and x mirrored
// once more so the triangles keep their winding (the water samples it at 1 - u).
void ReflectionViewProj(const glm::mat4& view, glm::mat4 proj, const glm::mat4& skyProj, float level, bool underWater, glm::mat4& viewProj,
                        glm::mat4& sky) {
    glm::mat4 mirror(1.0f);
    mirror[1][1] = -1.0f;
    mirror[3][1] = 2.0f * level;
    const glm::mat4 viewR = view * mirror;
    // The plane in the mirrored camera's space: the side the original keeps (above the water; below it from under it).
    const glm::vec4 plane = underWater ? glm::vec4(0.0f, -1.0f, 0.0f, level) : glm::vec4(0.0f, 1.0f, 0.0f, -level);
    const glm::vec4 c = glm::transpose(glm::inverse(viewR)) * plane;
    // Lengyel's oblique near plane, for a depth of 0..1: the projection's third row becomes the plane, scaled so the far
    // corner of the frustum keeps its depth.
    const glm::vec4 q = glm::inverse(proj) * glm::vec4(c.x > 0.0f ? 1.0f : (c.x < 0.0f ? -1.0f : 0.0f), c.y > 0.0f ? 1.0f : (c.y < 0.0f ? -1.0f : 0.0f),
                                                       1.0f, 1.0f);
    const float d = glm::dot(c, q);
    if (std::abs(d) > 1e-6f) {
        const glm::vec4 row = c * (1.0f / d);
        proj[0][2] = row.x;
        proj[1][2] = row.y;
        proj[2][2] = row.z;
        proj[3][2] = row.w;
    }
    glm::mat4 flipX(1.0f);
    flipX[0][0] = -1.0f;
    viewProj = flipX * proj * viewR;
    sky = flipX * skyProj * glm::mat4(glm::mat3(viewR));
}

constexpr std::uint32_t kInstances = 65536;
// The persistent world: instances, and parts drawn per bucket and frame.
constexpr std::uint32_t kWorldInstances = 1u << 18;
constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;
// The overlay: a few characters.
constexpr std::uint32_t kOverlayInstances = 64;
// The scene's colour: HDR, so the Modern look's bloom, shafts and tone mapping see light above 1
// (the Classic look clamps it in the last step, as the original's 8-bit target did).
constexpr rhi::Format kSceneColor = rhi::Format::RGBA16Float;

// Terrain: regions in a snapshot, and visible blocks a frame (36 a region).
constexpr std::size_t kTerrainRegions = 256;
constexpr std::uint32_t kTerrainBlocks = 4096;

} // namespace

bool SceneRenderer::Init(rhi::Device& device, gfx::AssetStore& assets, std::uint32_t skinVertices, std::string& error) {
    device_ = &device;
    assets_ = &assets;
    if (!scene_.Init(device, kInstances, error)) return false;
    if (!pass_.Init(device, kSceneColor, rhi::Format::D32Float, error)) return false;
    if (!world_.Init(device, kWorldInstances, error)) return false;
    gfx::WorldPass::Limits worldLimits;
    worldLimits.arenaVertices = skinVertices;
    worldLimits.maxSkinJobs = std::max(1u << 16, skinVertices / 64); // a dressed player has many small parts
    if (!worldPass_.Init(device, kSceneColor, rhi::Format::D32Float, worldLimits, error)) return false;
    if (!overlay_.Init(device, kOverlayInstances, error)) return false;
    gfx::WorldPass::Limits overlayLimits;
    overlayLimits.maxDraws = 1u << 10;
    overlayLimits.maxSkinJobs = 1u << 8;
    overlayLimits.arenaVertices = 1u << 18;
    if (!overlayPass_.Init(device, rhi::Format::RGBA8Unorm, rhi::Format::D32Float, overlayLimits, error)) return false;
    gfx::WorldPass::Limits reflectionLimits = worldLimits;
    reflectionLimits.maxDraws = 1u << 15;
    if (!reflectionPass_.Init(device, kSceneColor, rhi::Format::D32Float, reflectionLimits, error)) return false;
    bumpTexture_ = assets.RequestTexture("skybox\\waterbump3.ddj");
    sphereGray_ = assets.RequestTexture("prim\\mtrl\\etc\\spheremap_gray.ddj");
    sphereHighlight_ = assets.RequestTexture("prim\\mtrl\\etc\\spheremap_highlight.ddj");
    gfx::WorldPass::Limits shadowLimits;
    shadowLimits.maxDraws = 1u << 16;
    shadowLimits.arenaVertices = skinVertices / 2;
    shadowLimits.maxSkinJobs = std::max(1u << 14, shadowLimits.arenaVertices / 64);
    shadowLimits.depthOnly = true;
    shadowLimits.views = 4; // a view per cascade
    if (!shadowPass_.Init(device, rhi::Format::Unknown, rhi::Format::D32Float, shadowLimits, error)) return false;
    if (!staticShadowPass_.Init(device, rhi::Format::Unknown, rhi::Format::D32Float, shadowLimits, error)) return false;
    shadowTexture_ = assets.RequestTexture("skybox\\shadowsphere.ddj");
    for (int i = 1; i <= 30; ++i) {
        char path[32];
        std::snprintf(path, sizeof(path), "water\\water1%02d.ddj", i);
        waterFrames_.push_back(assets.RequestTexture(path));
    }
    iceTexture_ = assets.RequestTexture("water\\water201.ddj");
    if (!terrainPass_.Init(device, kSceneColor, rhi::Format::D32Float, kTerrainBlocks, error)) return false;
    if (!skyPass_.Init(device, kSceneColor, rhi::Format::D32Float, error)) return false;
    if (!weatherPass_.Init(device, kSceneColor, rhi::Format::D32Float, error)) return false;
    const char* weather[4] = {"weather\\rain1.ddj", "weather\\rain2.ddj", "weather\\snow1.ddj", "weather\\snow2.ddj"};
    for (int i = 0; i < 4; ++i) weatherTextures_[i] = assets.RequestTexture(weather[i]);
    if (!postPass_.Init(device, kSceneColor, error)) return false;
    if (!modernPass_.Init(device, error)) return false;
    if (!effectPass_.Init(device, kSceneColor, rhi::Format::D32Float, error)) return false;
    for (int i = 3; i <= 8; ++i) {
        char path[32];
        std::snprintf(path, sizeof(path), "sun\\lens%d.ddj", i);
        lensTextures_.push_back(assets.RequestTexture(path));
    }
    cloudTexture_ = assets.RequestTexture("skybox\\cloud1.ddj");
    sunTexture_ = assets.RequestTexture("sun\\lens2.ddj");
    for (int i = 1; i <= 29; ++i) {
        char path[32];
        std::snprintf(path, sizeof(path), "sun\\moon%02d.ddj", i);
        moonTextures_.push_back(assets.RequestTexture(path));
    }
    terrainInfos_.reserve(kTerrainRegions);
    terrainRegions_.reserve(kTerrainRegions);
    rhi::GraphicsPipelineDesc blit;
    blit.vertexShader = Spirv(kBlitVertexShader);
    blit.fragmentShader = Spirv(kBlitPixelShader);
    blit.vertexEntry = "VSMain";
    blit.fragmentEntry = "PSMain";
    blit.debugName = "scene blit";
    blit_ = device.CreateGraphicsPipeline(blit);
    blit.blend = rhi::BlendMode::AlphaBlend;
    blit.debugName = "scene overlay blit";
    blitOver_ = device.CreateGraphicsPipeline(blit);
    if (!blit_ || !blitOver_) {
        error = "cannot create the scene blit pipelines";
        return false;
    }
    // The depth probe (the clicks' occlusion): its pipeline and its slots (16 bytes each), read by the CPU.
    rhi::ComputePipelineDesc probe;
    probe.shader = Spirv(kDepthProbeComputeShader);
    probe.entry = "CSMain";
    probe.debugName = "depth probe";
    probePipeline_ = device.CreateComputePipeline(probe);
    rhi::BufferDesc slots;
    slots.size = kProbeSlots * 16;
    slots.usage = rhi::BufferUsage::Storage;
    slots.debugName = "depth probe slots";
    slots.cpuVisible = true;
    probeBuffer_ = device.CreateBuffer(slots);
    if (!probePipeline_ || !probeBuffer_) {
        error = "cannot create the depth probe";
        return false;
    }
    // Targets now, at the window's size: creating them on the first scene frame stalls it.
    EnsureTargets(device.SwapchainWidth(), device.SwapchainHeight());
    return true;
}

void SceneRenderer::Shutdown() {
    if (device_ == nullptr) return;
    scene_.Shutdown();
    pass_.Shutdown();
    world_.Shutdown();
    worldPass_.Shutdown();
    overlay_.Shutdown();
    overlayPass_.Shutdown();
    shadowPass_.Shutdown();
    staticShadowPass_.Shutdown();
    if (audioReady_) {
        audioPass_[0].Shutdown();
        audioPass_[1].Shutdown();
        device_->DestroyTexture(audioAtlas_);
        device_->DestroyBuffer(audioBuffer_);
        device_->DestroyPipeline(audioReadPipeline_);
        audioReady_ = false;
    }
    device_->DestroyTexture(shadowAtlas_);
    device_->DestroyTexture(staticAtlas_);
    device_->DestroyPipeline(blitOver_);
    device_->DestroyTexture(overlayColor_);
    device_->DestroyTexture(overlayDepth_);
    terrainPass_.Shutdown();
    terrainInfos_.clear();
    terrainRegions_.clear();
    assets_->Release(shadowTexture_);
    shadowTexture_ = {};
    for (gfx::AssetId id : waterFrames_) assets_->Release(id);
    waterFrames_.clear();
    assets_->Release(iceTexture_);
    iceTexture_ = {};
    skyPass_.Shutdown();
    weatherPass_.Shutdown();
    for (gfx::AssetId& id : weatherTextures_) {
        assets_->Release(id);
        id = {};
    }
    postPass_.Shutdown();
    modernPass_.Shutdown();
    effectPass_.Shutdown();
    for (gfx::AssetId id : lensTextures_) assets_->Release(id);
    lensTextures_.clear();
    assets_->Release(cloudTexture_);
    assets_->Release(sunTexture_);
    for (gfx::AssetId id : moonTextures_) assets_->Release(id);
    moonTextures_.clear();
    device_->DestroyTexture(metalProbe_);
    device_->DestroyTexture(metalProbeDepth_);
    if (metalProbeMade_) metalProbePass_.Shutdown();
    metalProbeMade_ = false;
    device_->DestroyPipeline(blit_);
    device_->DestroyPipeline(probePipeline_);
    device_->DestroyBuffer(probeBuffer_);
    device_->DestroyTexture(color_);
    device_->DestroyTexture(depth_);
    device_->DestroyTexture(post_);
    device_->DestroyTexture(picture_);
    device_->DestroyTexture(waterCopy_);
    device_->DestroyTexture(reflection_);
    device_->DestroyTexture(reflectionDepth_);
    reflectionPass_.Shutdown();
    assets_->Release(bumpTexture_);
    bumpTexture_ = {};
    assets_->Release(sphereGray_);
    assets_->Release(sphereHighlight_);
    sphereGray_ = sphereHighlight_ = {};
    resolved_.clear();
    device_ = nullptr;
}

void SceneRenderer::EnsureTargets(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0 || (width == width_ && height == height_ && color_)) return;
    device_->DestroyTexture(color_);
    device_->DestroyTexture(depth_);
    device_->DestroyTexture(post_);
    device_->DestroyTexture(picture_);
    device_->DestroyTexture(waterCopy_);
    device_->DestroyTexture(overlayColor_);
    device_->DestroyTexture(overlayDepth_);
    rhi::TextureDesc color;
    color.width = width;
    color.height = height;
    color.format = kSceneColor;
    color.usage = rhi::TextureUsage::RenderTarget;
    color.debugName = "scene colour";
    color_ = device_->CreateTexture(color);
    color.debugName = "scene post";
    post_ = device_->CreateTexture(color);
    color.debugName = "scene before the water";
    waterCopy_ = device_->CreateTexture(color);
    color.format = rhi::Format::RGBA8Unorm;
    color.debugName = "scene picture";
    picture_ = device_->CreateTexture(color);
    color.debugName = "scene overlay";
    overlayColor_ = device_->CreateTexture(color);
    rhi::TextureDesc depth = color;
    depth.format = rhi::Format::D32Float;
    // Sampled after the scene (the lens flare's visibility).
    depth.usage = rhi::TextureUsage::ShadowMap;
    depth.debugName = "scene depth";
    depth_ = device_->CreateTexture(depth);
    depth.debugName = "scene overlay depth";
    overlayDepth_ = device_->CreateTexture(depth);
    width_ = width;
    height_ = height;
}

void SceneRenderer::ApplyWorldCommands(std::span<const WorldCommand> commands) {
    for (const WorldCommand& c : commands) {
        if (c.handle >= handleSlots_.size()) {
            handleSlots_.resize(std::max<std::size_t>(c.handle + 1, handleSlots_.size() * 2), kNoSlot);
            handleOverlay_.resize(handleSlots_.size(), 0);
        }
        std::uint32_t& slot = handleSlots_[c.handle];
        std::uint8_t& inOverlay = handleOverlay_[c.handle];
        gfx::GpuScene& scene = inOverlay ? overlay_ : world_;
        gfx::InstanceRecord record;
        std::memcpy(record.world, c.world.data(), sizeof(record.world));
        switch (c.kind) {
        case WorldCommand::Kind::Add:
            if (c.paletteOffset != WorldCommand::kNoPalette) {
                record.paletteOffset = c.paletteOffset;
                record.flags |= gfx::kInstanceSkinned;
            }
            record.flags |= std::uint32_t{c.light} << 16;
            record.flags |= std::uint32_t{c.fade} << gfx::kInstanceFadeShift;
            record.flags |= (std::uint32_t{c.tint} << gfx::kInstanceTintShift) & gfx::kInstanceTintMask;
            if (slot != kNoSlot) scene.Remove(slot);
            {
                // Drawn once its model is resident (ResolveWorld fills the parts).
                inOverlay = c.overlay ? 1 : 0;
                gfx::GpuScene& target = c.overlay ? overlay_ : world_;
                slot = target.Add(record);
                if (slot != gfx::GpuScene::kInvalid) unresolved_.push_back({slot, c.model, c.overlay, c.hiddenTail});
                else slot = kNoSlot;
            }
            break;
        case WorldCommand::Kind::Move:
            if (slot != kNoSlot) {
                const gfx::InstanceRecord& old = scene.Get(slot);
                if (!inOverlay && (old.flags & gfx::kInstanceSkinned) == 0) ++staticGeneration_; // the cached shadows change
                record.meshBase = old.meshBase;
                record.meshCount = old.meshCount;
                if (const auto full = fullParts_.find((std::uint64_t(inOverlay) << 32) | slot); full != fullParts_.end() && old.meshCount != 0) {
                    record.meshCount = full->second - std::min<std::uint32_t>(c.hiddenTail, full->second);
                }
                record.paletteOffset = old.paletteOffset;
                record.flags = (old.flags & ~(gfx::kInstanceFadeMask | gfx::kInstanceTintMask)) | (std::uint32_t{c.fade} << gfx::kInstanceFadeShift) |
                               ((std::uint32_t{c.tint} << gfx::kInstanceTintShift) & gfx::kInstanceTintMask);
                scene.Set(slot, record);
            }
            break;
        case WorldCommand::Kind::Remove:
            if (slot != kNoSlot && !inOverlay) {
                nightLights_.erase(slot);
                if ((scene.Get(slot).flags & gfx::kInstanceSkinned) == 0) ++staticGeneration_;
            }
            if (slot != kNoSlot) scene.Remove(slot);
            std::erase_if(unresolved_, [s = slot, o = inOverlay != 0](const Pending& p) { return p.slot == s && p.overlay == o; });
            slot = kNoSlot;
            break;
        }
    }
}

void SceneRenderer::ResolveWorld() {
    std::size_t kept = 0;
    for (const Pending& p : unresolved_) {
        const std::shared_ptr<const gfx::ModelInfo> model = assets_->Model(p.model);
        if (!model) {
            unresolved_[kept++] = p;
            continue;
        }
        gfx::GpuScene& scene = p.overlay ? overlay_ : world_;
        gfx::InstanceRecord record = scene.Get(p.slot);
        record.meshBase = model->meshBase;
        record.meshCount = model->meshCount - std::min<std::uint32_t>(p.hiddenTail, model->meshCount);
        fullParts_[(std::uint64_t(p.overlay ? 1 : 0) << 32) | p.slot] = model->meshCount;
        scene.Set(p.slot, record);
        if (p.overlay) continue;
        // A building now drawn where the shadows reach changes the cached cascades.
        if ((record.flags & gfx::kInstanceSkinned) == 0) {
            const glm::vec2 d(record.world[3] - eye_.x, record.world[11] - eye_.z);
            if (glm::dot(d, d) < 7000.0f * 7000.0f) ++staticGeneration_;
        }
        // Its windows: a light at each night-light part, as wide as the part (60 .. 260 units).
        std::vector<gfx::EffectLight> windows;
        for (const gfx::ModelPartInfo& part : model->parts) {
            if ((part.materialFlags & gfx::kMaterialNightLight) == 0) continue;
            const glm::vec3 c(part.sphere[0], part.sphere[1], part.sphere[2]);
            const auto row = [&](int r) { return glm::vec4(record.world[r * 4], record.world[r * 4 + 1], record.world[r * 4 + 2], record.world[r * 4 + 3]); };
            const glm::vec3 at(glm::dot(glm::vec3(row(0)), c) + row(0).w, glm::dot(glm::vec3(row(1)), c) + row(1).w,
                               glm::dot(glm::vec3(row(2)), c) + row(2).w);
            const float scale = glm::length(glm::vec3(row(0).x, row(1).x, row(2).x));
            gfx::EffectLight light;
            light.position = at;
            light.radius = std::clamp(part.sphere[3] * scale * 1.6f, 60.0f, 260.0f);
            light.colour = glm::vec3(1.0f, 0.72f, 0.42f);
            light.intensity = 0.55f;
            windows.push_back(light);
        }
        if (!windows.empty()) nightLights_[p.slot] = std::move(windows);
    }
    unresolved_.resize(kept);
}

void SceneRenderer::ResolveTerrain(const SceneView& view) {
    terrainInfos_.clear();
    terrainRegions_.clear();
    for (const TerrainItem& item : view.terrain) {
        if (terrainInfos_.size() == kTerrainRegions) break;
        std::shared_ptr<const gfx::TerrainInfo> info = assets_->Terrain(item.region);
        if (!info) continue; // not resident yet (or evicted since the snapshot was built)
        terrainRegions_.push_back({info.get(), glm::vec3(item.offset[0], item.offset[1], item.offset[2])});
        terrainInfos_.push_back(std::move(info));
    }
}

gfx::RenderGraph::Resource SceneRenderer::AddPasses(gfx::RenderGraph& graph, const SceneView& view, const OrbitCamera& camera,
                                                    std::uint32_t width, std::uint32_t height) {
    drawn_ = 0;
    if (width == 0 || height == 0) return 0;
    EnsureTargets(width, height); // made once per size, imported into the graph
    Resolve(view);
    ResolveWorld();
    glm::mat4 cameraView(1.0f), cameraProj(1.0f), cameraSkyProj(1.0f);
    viewProj_ = ViewProj(camera, static_cast<float>(width_) / static_cast<float>(height_), eye_, skyViewProj_, &cameraView, &cameraProj, &cameraSkyProj);
    cameraView_ = cameraView;
    ResolveTerrain(view);
    // Levels of detail (ours): the terrain halves its detail past these distances and small objects
    // leave once a few pixels across, both further out with the detail row and the view distance.
    static constexpr float kDetailScale[4] = {0.6f, 1.0f, 1.6f, 2.4f};
    static constexpr float kMinAngle[4] = {0.0027f, 0.0018f, 0.0011f, 0.0006f};
    const int detail = std::clamp(video_.detailDistance, 0, 3);
    const float lodScale = kDetailScale[detail] * float(1 + std::clamp(video_.viewDistance, 0, 3));
    const std::array<float, 3> lodDistances = {700.0f * lodScale, 1400.0f * lodScale, 2800.0f * lodScale};
    terrainPass_.Prepare(terrainRegions_, viewProj_, eye_, lodDistances);
    worldPass_.SetSizeCulling(eye_, kMinAngle[detail] / std::sqrt(float(1 + std::clamp(video_.viewDistance, 0, 3))));
    const auto instances = graph.ImportBuffer(scene_.Buffer(), "instances");
    const auto worldInstances = graph.ImportBuffer(world_.Buffer(), "world instances");
    const auto meshes = graph.ImportBuffer(assets_->Meshes().Buffer(), "mesh table");
    const auto geometry = graph.ImportBuffer(assets_->Geometry().Buffer(), "geometry pool");
    const auto color = graph.ImportTexture(color_, "scene colour");
    const bool withOverlay = overlay_.Live() > 0;
    const auto overlayInstances = withOverlay ? graph.ImportBuffer(overlay_.Buffer(), "overlay instances") : 0;
    graph.AddPass("scene: records", gfx::PassKind::Transfer,
                  [&](gfx::RenderGraph::Builder& b) {
                      b.Write(instances, gfx::Use::TransferWrite).Write(worldInstances, gfx::Use::TransferWrite).Write(meshes, gfx::Use::TransferWrite);
                      if (withOverlay) b.Write(overlayInstances, gfx::Use::TransferWrite);
                  },
                  [this, withOverlay](rhi::CommandList& cmd, const gfx::RenderGraph&) {
                      scene_.Upload(cmd);
                      world_.Upload(cmd);
                      if (withOverlay) overlay_.Upload(cmd);
                      assets_->UploadMeshes(cmd);
                  });
    const bool skinning = static_cast<bool>(view.worldPalettes);
    worldPass_.SetWaterBlended(video_.water != 0);
    lists_ = worldPass_.AddCulling(graph, worldInstances, meshes, world_, assets_->Meshes(), viewProj_, skinning,
                                   terrainPass_.WaterLevel());
    const auto palettes = skinning ? graph.ImportBuffer(view.worldPalettes, "world palettes") : 0;
    if (skinning) {
        worldPass_.AddSkinning(graph, lists_, worldInstances, meshes, geometry, palettes, view.worldPalettes, assets_->Geometry(), world_,
                               assets_->Meshes());
    }
    // The sun's cascaded shadows (G1, the Modern rows): one culling over every cascade, skinned once
    // more for it, then each cascade drawn into its square of the atlas.
    shadows_ = {};
    if (const int quality = std::clamp(video_.sunShadows, 0, 4); quality > 0 && view.sky.active) PrepareShadows(view, quality);
    if (shadows_.count == 0) drawnShadows_.count = 0; // the atlas is stale when they come back
    gfx::RenderGraph::Resource shadowAtlas = 0;
    if (shadows_.count > 0) {
        // Each cascade drawn this frame: its view, the specks it leaves out (parts under a few of its
        // texels across, by the casters row) and whether characters cast into it.
        static constexpr float kSpecks[3] = {0.0f, 1.5f, 3.0f};
        const float speck = kSpecks[std::clamp(video_.shadowCulling, 0, 2)];
        // The static casters of the cascades whose view moved (into the cached atlas), and the
        // characters of the cascades that take them (every frame, over a copy of it).
        std::array<glm::mat4, 4> redrawn{}, live{};
        std::array<float, 4> specks{}, liveSpecks{};
        for (std::size_t k = 0; k < shadowRedraw_.size(); ++k) {
            const int i = shadowRedraw_[k];
            redrawn[k] = drawnShadows_.viewProj[std::size_t(i)];
            specks[k] = speck * drawnShadows_.texels[i];
        }
        const std::size_t liveCount = std::size_t(std::min(shadowCharacterCascades_, shadows_.count));
        for (std::size_t i = 0; i < liveCount; ++i) {
            live[i] = drawnShadows_.viewProj[i];
            liveSpecks[i] = speck * drawnShadows_.texels[int(i)];
        }
        // The swaying plants (foliage parts) of the near cascades are drawn every frame with the characters: in the
        // cached atlas they would stand still until the cascade is drawn again (the sway at the sun's few redraws a
        // second). The far cascades keep them cached (a sway there is under a texel).
        const int liveFoliage = int(std::min<std::size_t>(kLiveFoliageCascades, liveCount));
        std::uint32_t cachedFoliage = 0;
        for (std::size_t k = 0; k < shadowRedraw_.size(); ++k) {
            if (shadowRedraw_[k] >= liveFoliage) cachedFoliage |= 1u << k;
        }
        staticShadowLists_ = {};
        if (!shadowRedraw_.empty()) {
            staticShadowLists_ = staticShadowPass_.AddCulling(graph, worldInstances, meshes, world_, assets_->Meshes(),
                                                              std::span<const glm::mat4>(redrawn.data(), shadowRedraw_.size()), false,
                                                              -1.0e30f, std::span<const float>(specks.data(), shadowRedraw_.size()), 0, 4,
                                                              cachedFoliage);
        }
        shadowLists_ = shadowPass_.AddCulling(graph, worldInstances, meshes, world_, assets_->Meshes(),
                                              std::span<const glm::mat4>(live.data(), std::max<std::size_t>(liveCount, 1)), skinning, -1.0e30f,
                                              std::span<const float>(liveSpecks.data(), std::max<std::size_t>(liveCount, 1)),
                                              std::uint32_t(liveCount), 0, (1u << liveFoliage) - 1u);
        if (skinning) {
            shadowPass_.AddSkinning(graph, shadowLists_, worldInstances, meshes, geometry, palettes, view.worldPalettes, assets_->Geometry(),
                                    world_, assets_->Meshes());
        }
        shadowAtlas = graph.ImportTexture(shadowAtlas_, "sun shadows");
        const auto staticAtlas = graph.ImportTexture(staticAtlas_, "sun shadows (static)");
        graph.AddPass("sun shadows", gfx::PassKind::Graphics,
                      [&](gfx::RenderGraph::Builder& b) {
                          b.Write(staticAtlas, gfx::Use::DepthWrite);
                          if (!shadowRedraw_.empty()) {
                              b.Read(staticShadowLists_.commands, gfx::Use::IndirectRead)
                                  .Read(staticShadowLists_.counts, gfx::Use::IndirectRead)
                                  .Read(staticShadowLists_.draws, gfx::Use::VertexRead);
                          }
                          b.Read(worldInstances, gfx::Use::VertexRead)
                              .Read(meshes, gfx::Use::VertexRead)
                              .Read(shadowLists_.commands, gfx::Use::IndirectRead)
                              .Read(shadowLists_.counts, gfx::Use::IndirectRead)
                              .Read(shadowLists_.draws, gfx::Use::VertexRead)
                              .Read(shadowLists_.arena, gfx::Use::VertexRead)
                              .Read(geometry, gfx::Use::VertexRead)
                              .Read(geometry, gfx::Use::FragmentRead)
                              .Read(geometry, gfx::Use::IndexRead)
                              .Write(shadowAtlas, gfx::Use::DepthWrite);
                      },
                      [this, &view](rhi::CommandList& cmd, const gfx::RenderGraph&) { DrawShadows(cmd, view); });
    }
    AddAudioCube(graph, worldInstances, meshes, geometry, view);
    // The overlay (the character windows' characters): culled, skinned and drawn on its own, into
    // its own target, under the same camera and light.
    overlayResult_ = 0;
    if (withOverlay) {
        overlayViewProj_ = view.overlayOwnCamera ? glm::make_mat4(view.overlayViewProj.data()) : viewProj_;
        overlayLists_ = overlayPass_.AddCulling(graph, overlayInstances, meshes, overlay_, assets_->Meshes(), overlayViewProj_, skinning,
                                                terrainPass_.WaterLevel());
        if (skinning) {
            overlayPass_.AddSkinning(graph, overlayLists_, overlayInstances, meshes, geometry, palettes, view.worldPalettes, assets_->Geometry(),
                                     overlay_, assets_->Meshes());
        }
        overlayResult_ = graph.ImportTexture(overlayColor_, "scene overlay");
        graph.AddPass("scene overlay", gfx::PassKind::Graphics,
                      [&](gfx::RenderGraph::Builder& b) {
                          b.Read(overlayInstances, gfx::Use::VertexRead)
                              .Read(meshes, gfx::Use::VertexRead)
                              .Read(overlayLists_.commands, gfx::Use::IndirectRead)
                              .Read(overlayLists_.counts, gfx::Use::IndirectRead)
                              .Read(overlayLists_.draws, gfx::Use::VertexRead)
                              .Read(overlayLists_.arena, gfx::Use::VertexRead)
                              .Read(geometry, gfx::Use::VertexRead)
                              .Read(geometry, gfx::Use::FragmentRead)
                              .Read(geometry, gfx::Use::IndexRead)
                              .Write(overlayResult_, gfx::Use::ColorWrite);
                      },
                      [this, &view](rhi::CommandList& cmd, const gfx::RenderGraph&) { DrawOverlay(cmd, view); });
    }
    // The original's water reflection (its row 4; the Original look's water only, outdoors, with water in sight): the
    // scene mirrored about the water level into its own 512 x 512 target before the scene.
    const float waterLevel = terrainPass_.WaterLevel();
    reflecting_ = video_.water == 0 && video_.current()[4] != 0 && view.sky.active && terrainPass_.VisibleWater() != 0 && waterLevel > -1e30f;
    gfx::RenderGraph::Resource reflectionTarget = 0;
    if (reflecting_) {
        if (!reflection_) {
            rhi::TextureDesc desc;
            desc.width = desc.height = 512;
            desc.format = kSceneColor;
            desc.usage = rhi::TextureUsage::RenderTarget;
            desc.debugName = "water reflection";
            reflection_ = device_->CreateTexture(desc);
            desc.format = rhi::Format::D32Float;
            desc.usage = rhi::TextureUsage::DepthStencil;
            desc.debugName = "water reflection depth";
            reflectionDepth_ = device_->CreateTexture(desc);
        }
        ReflectionViewProj(cameraView, cameraProj, cameraSkyProj, waterLevel, eye_.y < waterLevel, reflectViewProj_, reflectSkyViewProj_);
        reflectionPass_.SetWaterBlended(false);
        reflectionLists_ = reflectionPass_.AddCulling(graph, worldInstances, meshes, world_, assets_->Meshes(), reflectViewProj_, skinning, waterLevel);
        if (skinning) {
            reflectionPass_.AddSkinning(graph, reflectionLists_, worldInstances, meshes, geometry, palettes, view.worldPalettes, assets_->Geometry(),
                                        world_, assets_->Meshes());
        }
        reflectionTarget = graph.ImportTexture(reflection_, "water reflection");
        graph.AddPass("scene: water reflection", gfx::PassKind::Graphics,
                      [&](gfx::RenderGraph::Builder& b) {
                          b.Read(worldInstances, gfx::Use::VertexRead)
                              .Read(meshes, gfx::Use::VertexRead)
                              .Read(reflectionLists_.commands, gfx::Use::IndirectRead)
                              .Read(reflectionLists_.counts, gfx::Use::IndirectRead)
                              .Read(reflectionLists_.draws, gfx::Use::VertexRead)
                              .Read(reflectionLists_.arena, gfx::Use::VertexRead)
                              .Read(geometry, gfx::Use::VertexRead)
                              .Read(geometry, gfx::Use::FragmentRead)
                              .Read(geometry, gfx::Use::IndexRead)
                              .Write(reflectionTarget, gfx::Use::ColorWrite);
                      },
                      [this, &view](rhi::CommandList& cmd, const gfx::RenderGraph&) { DrawReflection(cmd, view); });
    }
    // The Modern metal sheen's probe (ours): with its row on and the Modern look of it, one face of the cube round the
    // player a frame (90 degrees each, near 14 so the player's own body stays out of it).
    metalProbeDrawing_ = false;
    gfx::RenderGraph::Resource metalProbeTarget = 0;
    if (video_.current()[6] != 0 && video_.metalSheen != 0 && world_.Live() > 0) {
        if (!metalProbeMade_) {
            std::string error;
            gfx::WorldPass::Limits limits;
            limits.maxDraws = 1u << 14;
            limits.arenaVertices = 1u << 18;
            limits.maxSkinJobs = 256;
            if (metalProbePass_.Init(*device_, kSceneColor, rhi::Format::D32Float, limits, error)) {
                rhi::TextureDesc desc;
                desc.width = kMetalFace * 3;
                desc.height = kMetalFace * 2;
                desc.format = kSceneColor;
                desc.usage = rhi::TextureUsage::RenderTarget;
                desc.debugName = "metal probe";
                metalProbe_ = device_->CreateTexture(desc);
                desc.format = rhi::Format::D32Float;
                desc.usage = rhi::TextureUsage::DepthStencil;
                desc.debugName = "metal probe depth";
                metalProbeDepth_ = device_->CreateTexture(desc);
                metalProbeMade_ = true;
                metalProbeFaces_ = 0;
            } else {
                core::LogWarning("render: the metal probe: {}", error);
            }
        }
        if (metalProbeMade_) {
            static const glm::vec3 kDirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            const std::uint32_t face = metalProbeFace_ % 6;
            const glm::vec3 centre(camera.targetX, camera.targetY, camera.targetZ);
            const glm::vec3 up = (face == 2 || face == 3) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
            const glm::mat4 look = glm::lookAt(centre, centre + kDirs[face], up);
            glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, 14.0f, 4000.0f);
            projection[1][1] *= -1.0f;
            metalProbeViewProj_ = projection * look;
            glm::mat4 skyProjection = glm::perspective(glm::radians(90.0f), 1.0f, 10.0f, 5000000.0f);
            skyProjection[1][1] *= -1.0f;
            metalProbeSkyViewProj_ = skyProjection * glm::mat4(glm::mat3(look));
            metalProbePass_.SetWaterBlended(false);
            metalProbeLists_ = metalProbePass_.AddCulling(graph, worldInstances, meshes, world_, assets_->Meshes(), metalProbeViewProj_, skinning,
                                                          terrainPass_.WaterLevel());
            if (skinning) {
                metalProbePass_.AddSkinning(graph, metalProbeLists_, worldInstances, meshes, geometry, palettes, view.worldPalettes, assets_->Geometry(),
                                            world_, assets_->Meshes());
            }
            metalProbeTarget = graph.ImportTexture(metalProbe_, "metal probe");
            metalProbeDrawing_ = true;
            graph.AddPass("scene: metal probe", gfx::PassKind::Graphics,
                          [&](gfx::RenderGraph::Builder& b) {
                              b.Read(worldInstances, gfx::Use::VertexRead)
                                  .Read(meshes, gfx::Use::VertexRead)
                                  .Read(metalProbeLists_.commands, gfx::Use::IndirectRead)
                                  .Read(metalProbeLists_.counts, gfx::Use::IndirectRead)
                                  .Read(metalProbeLists_.draws, gfx::Use::VertexRead)
                                  .Read(metalProbeLists_.arena, gfx::Use::VertexRead)
                                  .Read(geometry, gfx::Use::VertexRead)
                                  .Read(geometry, gfx::Use::FragmentRead)
                                  .Read(geometry, gfx::Use::IndexRead)
                                  .Write(metalProbeTarget, gfx::Use::ColorWrite);
                          },
                          [this, &view](rhi::CommandList& cmd, const gfx::RenderGraph&) { DrawMetalProbe(cmd, view); });
        }
    }
    graph.AddPass("scene", gfx::PassKind::Graphics,
                  [&](gfx::RenderGraph::Builder& b) {
                      if (reflectionTarget != 0) b.Read(reflectionTarget, gfx::Use::Sampled);
                      if (metalProbeTarget != 0) b.Read(metalProbeTarget, gfx::Use::Sampled);
                      b.Read(instances, gfx::Use::VertexRead)
                          .Read(worldInstances, gfx::Use::VertexRead)
                          .Read(meshes, gfx::Use::VertexRead)
                          .Read(lists_.commands, gfx::Use::IndirectRead)
                          .Read(lists_.counts, gfx::Use::IndirectRead)
                          .Read(lists_.draws, gfx::Use::VertexRead)
                          .Read(lists_.arena, gfx::Use::VertexRead)
                          .Read(geometry, gfx::Use::VertexRead)
                          .Read(geometry, gfx::Use::FragmentRead)
                          .Read(geometry, gfx::Use::IndexRead)
                          .Write(color, gfx::Use::ColorWrite);
                      if (shadowAtlas != 0) b.Read(shadowAtlas, gfx::Use::Sampled);
                  },
                  [this, &view, &camera](rhi::CommandList& cmd, const gfx::RenderGraph&) { Draw(cmd, view, camera); });

    // The effects (V1), over the scene's colour and depth.
    effectPass_.AddPasses(graph, color, geometry, assets_->Geometry(), {&view.effects, color_, depth_, viewProj_});

    // The glow and the lens flare (C3g): the original's rows (lens flare 10, bloom 11) and ours
    // widening them (1 the original's, 2 the Modern one).
    const auto& record = video_.current();
    const bool glowRow = record[11] != 0, flareRow = record[10] != 0;
    gfx::PostPass::Frame post;
    post.glow = view.glow && glowRow && video_.bloom == 1;
    effectPass_.SetGlow(float(std::clamp(video_.effectGlow, 0, 10)) / 10.0f);
    post.flare = view.lensFlare && view.sky.active && flareRow && video_.lensFlare >= 1;
    post.scene = color_;
    post.depth = depth_;
    post.output = post_;
    post.viewProj = viewProj_;
    post.eye = eye_;
    post.sunDirection = view.sky.sunPosition;
    post.sunUp = view.sky.sunVisible && view.sky.sunPosition.y >= 700.0f;
    post.viewDistance = view.viewDistance;
    post.seconds = view.frameSeconds;
    post.width = width_;
    post.height = height_;
    for (std::size_t i = 0; i < lensTextures_.size() && i < post.lens.size(); ++i) {
        const auto texture = assets_->Texture(lensTextures_[i]);
        post.lens[i] = texture ? texture->bindless : 0;
    }
    const auto result = postPass_.AddPasses(graph, color, post);

    // The last steps into the 8-bit picture: Classic clamps and takes the brightness; the Modern
    // rows add occlusion, sun shafts, bloom, tone mapping, grading and FXAA (gfx::ModernPass).
    gfx::ModernPass::Frame modern;
    gfx::ModernPass::Settings& m = modern.settings;
    const bool modernLook = video_.toneMapping != 0;
    m.toneMapping = std::clamp(video_.toneMapping, 0, 2);
    m.gamma = video_.Gamma();
    m.grading = video_.colourGrading != 0 && view.sky.active;
    m.occlusion = std::clamp(video_.ambientOcclusion, 0, 2);
    m.bloom = video_.bloom == 2; // ours: on whatever the original's row says
    m.shafts = video_.lensFlare == 2 && view.lensFlare && view.sky.active;
    m.fxaa = video_.antiAliasing == 1;
    m.vignette = modernLook;
    m.heightFog = std::clamp(video_.volumetricFog, 0, 2);
    modern.fogColour = view.lighting.fogColor;
    modern.scene = result == color ? color_ : post_;
    modern.depth = depth_;
    modern.output = picture_;
    modern.viewProj = viewProj_;
    modern.eye = eye_;
    modern.sunDirection = view.sky.sunPosition;
    modern.sunColour = view.sky.sunColor;
    // The sun's weight in the Modern effects (its shafts, its light in the fog): fading with its height, not on or off.
    {
        const float height = glm::length(view.sky.sunPosition) > 0.0f ? glm::normalize(view.sky.sunPosition).y : -1.0f;
        modern.sunWeight = view.sky.sunVisible ? glm::smoothstep(-0.02f, 0.12f, height) : 0.0f;
    }
    modern.timeOfDay = view.lighting.timeOfDay;
    modern.width = width_;
    modern.height = height_;
    const auto depthResource = graph.ImportTexture(depth_, "scene depth (sampled)");
    const auto picture = modernPass_.AddPasses(graph, result, depthResource, modern);
    AddProbe(graph, depthResource, view);
    result_ = picture_;
    return picture;
}

void SceneRenderer::AddProbe(gfx::RenderGraph& graph, gfx::RenderGraph::Resource depth, const SceneView& view) {
    // The slots whose frames have finished: the newest answer kept.
    const auto* mapped = static_cast<const std::byte*>(device_->MappedMemory(probeBuffer_));
    if (mapped == nullptr) return;
    const std::uint64_t completed = device_->CompletedFrame();
    for (std::uint32_t s = 0; s < kProbeSlots; ++s) {
        ProbeAsk& ask = probeAsks_[s];
        if (ask.frame == 0 || ask.frame > completed) continue;
        float value = 1.0f;
        std::memcpy(&value, mapped + s * 16, sizeof(value));
        if (ask.frame >= probe_.frame) probe_ = {true, ask.x, ask.y, value, ask.frame};
        ask.frame = 0;
    }
    // This frame's: the asked pixel's texel of the depth target (the swapchain's pixels scaled to it).
    if (view.probeX < 0.0f || !depth_ || width_ == 0 || height_ == 0) return;
    const std::uint64_t frame = device_->RecordingFrame();
    const std::uint32_t slot = std::uint32_t(frame % kProbeSlots);
    if (probeAsks_[slot].frame != 0) return; // its last answer not in yet: skip a frame
    const float sw = float(std::max(1u, device_->SwapchainWidth())), sh = float(std::max(1u, device_->SwapchainHeight()));
    const std::uint32_t tx = std::min(width_ - 1, std::uint32_t(std::clamp(view.probeX / sw, 0.0f, 1.0f) * float(width_)));
    const std::uint32_t ty = std::min(height_ - 1, std::uint32_t(std::clamp(view.probeY / sh, 0.0f, 1.0f) * float(height_)));
    probeAsks_[slot] = {frame, view.probeX, view.probeY};
    struct Push {
        std::uint64_t output;
        std::uint32_t depth, x, y, frame;
    };
    const Push push{device_->BufferAddress(probeBuffer_) + slot * 16, device_->BindlessIndex(depth_), tx, ty, std::uint32_t(frame)};
    // (Its output is a mapped buffer the graph does not know: kept as a side effect, not culled.)
    graph.AddPass("scene: depth probe", gfx::PassKind::Compute, [&](gfx::RenderGraph::Builder& b) { b.Read(depth, gfx::Use::Sampled).SideEffect(); },
                  [this, push](rhi::CommandList& cmd, const gfx::RenderGraph&) {
                      cmd.BindComputePipeline(probePipeline_);
                      cmd.PushConstants(&push, sizeof(push));
                      cmd.Dispatch(1, 1, 1);
                  });
}

void SceneRenderer::AddAudioCube(gfx::RenderGraph& graph, gfx::RenderGraph::Resource instances, gfx::RenderGraph::Resource meshes,
                                 gfx::RenderGraph::Resource geometry, const SceneView& view) {
#ifdef SR_AUDIO_OCCLUSION
    constexpr std::uint32_t W = AudioCube::kWidth, H = AudioCube::kHeight;
    constexpr float kNear = 2.0f, kFar = 1500.0f;
    // The slots whose frames have finished: the newest handed back.
    if (audioReady_) {
        const auto* mapped = static_cast<const std::byte*>(device_->MappedMemory(audioBuffer_));
        const std::uint64_t completed = device_->CompletedFrame();
        std::uint64_t newest = 0;
        for (std::uint32_t s = 0; s < kAudioSlots && mapped != nullptr; ++s) {
            AudioAsk& ask = audioAsks_[s];
            if (ask.frame == 0 || ask.frame > completed) continue;
            if (ask.frame > newest) {
                newest = ask.frame;
                auto cube = std::make_shared<AudioCube>();
                cube->eye = ask.eye;
                cube->viewProj = ask.viewProj;
                cube->nearZ = kNear;
                cube->farZ = kFar;
                cube->frame = ask.frame;
                cube->depth.resize(std::size_t(W) * H);
                std::memcpy(cube->depth.data(), mapped + std::size_t(s) * W * H * 4, cube->depth.size() * 4);
                audioCubeReady_ = std::move(cube);
            }
            ask.frame = 0;
        }
    }
    if (!view.audioCube || world_.Live() == 0) return;
    if (!audioReady_) {
        // Made on first use: two depth-only world passes of three views each, the atlas, the copy's pipeline and slots.
        std::string error;
        gfx::WorldPass::Limits limits;
        limits.maxDraws = 1u << 13;
        limits.arenaVertices = 1024;
        limits.maxSkinJobs = 64;
        limits.depthOnly = true;
        limits.views = 3;
        rhi::TextureDesc desc;
        desc.width = W;
        desc.height = H;
        desc.format = rhi::Format::D32Float;
        desc.usage = rhi::TextureUsage::ShadowMap;
        desc.debugName = "audio cube";
        rhi::ComputePipelineDesc read;
        read.shader = Spirv(kAudioCubeComputeShader);
        read.entry = "CSMain";
        read.debugName = "audio cube readback";
        rhi::BufferDesc slots;
        slots.size = std::size_t(kAudioSlots) * W * H * 4;
        slots.usage = rhi::BufferUsage::Storage;
        slots.cpuVisible = true;
        slots.debugName = "audio cube slots";
        if (!audioPass_[0].Init(*device_, rhi::Format::Unknown, rhi::Format::D32Float, limits, error) ||
            !audioPass_[1].Init(*device_, rhi::Format::Unknown, rhi::Format::D32Float, limits, error)) {
            core::LogWarning("render: the audio cube: {}", error);
            return;
        }
        audioAtlas_ = device_->CreateTexture(desc);
        audioReadPipeline_ = device_->CreateComputePipeline(read);
        audioBuffer_ = device_->CreateBuffer(slots);
        audioReady_ = true;
        core::LogInfo("render: the audio cube made ({} x {}, faces of {})", W, H, AudioCube::kFace);
    }
    const std::uint64_t frame = device_->RecordingFrame();
    const std::uint32_t slot = std::uint32_t(frame % kAudioSlots);
    if (audioAsks_[slot].frame != 0) return; // its last copy not read yet: skip a frame
    // The six faces round the player's head: +x, -x, +y, -y, +z, -z, each 90 degrees square, y flipped as the camera's
    // (Vulkan's y points down: the triangles keep their winding for the back-face culling).
    static const glm::vec3 kDirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, kNear, kFar);
    projection[1][1] *= -1.0f;
    const glm::vec3 eye(view.audioEye[0], view.audioEye[1], view.audioEye[2]);
    for (int i = 0; i < 6; ++i) {
        const glm::vec3 up = (i == 2 || i == 3) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        audioViewProj_[std::size_t(i)] = projection * glm::lookAt(eye, eye + kDirs[i], up);
    }
    // What stops a sound: the buildings, walls and rocks; not what is a few units across, nor the plants' swaying parts.
    const std::array<float, 3> minRadius = {6.0f, 6.0f, 6.0f};
    for (int k = 0; k < 2; ++k) {
        audioLists_[k] = audioPass_[k].AddCulling(graph, instances, meshes, world_, assets_->Meshes(),
                                                  std::span<const glm::mat4>(audioViewProj_.data() + 3 * k, 3), false, -1.0e30f,
                                                  std::span<const float>(minRadius.data(), 3), 0, 3, 0);
    }
    const auto atlas = graph.ImportTexture(audioAtlas_, "audio cube");
    graph.AddPass("audio cube", gfx::PassKind::Graphics,
                  [&](gfx::RenderGraph::Builder& b) {
                      for (int k = 0; k < 2; ++k) {
                          b.Read(audioLists_[k].commands, gfx::Use::IndirectRead)
                              .Read(audioLists_[k].counts, gfx::Use::IndirectRead)
                              .Read(audioLists_[k].draws, gfx::Use::VertexRead);
                      }
                      b.Read(instances, gfx::Use::VertexRead)
                          .Read(meshes, gfx::Use::VertexRead)
                          .Read(geometry, gfx::Use::VertexRead)
                          .Read(geometry, gfx::Use::FragmentRead)
                          .Read(geometry, gfx::Use::IndexRead)
                          .Write(atlas, gfx::Use::DepthWrite);
                  },
                  [this](rhi::CommandList& cmd, const gfx::RenderGraph&) { DrawAudioCube(cmd); });
    struct Push {
        std::uint64_t output;
        std::uint32_t depth, width, height;
    };
    const Push push{device_->BufferAddress(audioBuffer_) + std::uint64_t(slot) * W * H * 4, device_->BindlessIndex(audioAtlas_), W, H};
    // (Its output is a mapped buffer the graph does not know: kept as a side effect, not culled.)
    graph.AddPass("audio cube readback", gfx::PassKind::Compute, [&](gfx::RenderGraph::Builder& b) { b.Read(atlas, gfx::Use::Sampled).SideEffect(); },
                  [this, push](rhi::CommandList& cmd, const gfx::RenderGraph&) {
                      cmd.BindComputePipeline(audioReadPipeline_);
                      cmd.PushConstants(&push, sizeof(push));
                      cmd.Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                  });
    audioAsks_[slot] = {frame, eye, audioViewProj_};
#else
    (void)graph, (void)instances, (void)meshes, (void)geometry, (void)view;
#endif
}

void SceneRenderer::DrawAudioCube(rhi::CommandList& cmd) {
    SR_ZONE("render: audio cube");
    rhi::RenderPassDesc pass;
    pass.depth = audioAtlas_; // cleared to 1
    cmd.BeginRenderPass(pass);
    constexpr std::uint32_t F = AudioCube::kFace;
    for (std::uint32_t i = 0; i < 6; ++i) {
        const std::uint32_t x = (i % 3) * F, y = (i / 3) * F;
        cmd.SetViewport({float(x), float(y), float(F), float(F)});
        cmd.SetScissor({std::int32_t(x), std::int32_t(y), F, F});
        audioPass_[i / 3].DrawDepth(cmd, audioViewProj_[i], assets_->Geometry(), world_, assets_->Meshes(), 0, i % 3);
    }
    cmd.EndPass();
}

void SceneRenderer::PrepareShadows(const SceneView& view, int quality) {
    // The light, at every hour (there are shadows all night): the sun first, as long as it stands above the horizon
    // (a little under it, its last light), its shadows fading as it sets; then the moon, its shadows fading in once the
    // sun is down. At the hand-over both are at nothing, so the shadows never jump from one side of the sky to the
    // other; at dawn the same backwards. (The moon took over as soon as it stood higher than the sun, at the sun's full
    // strength minus a short fade: the shadows flipped while the sun was still up; 2026-11-21, the user.)
    const auto unit = [](const glm::vec3& p) { return glm::length(p) > 0.0f ? glm::normalize(p) : glm::vec3(0.0f, -1.0f, 0.0f); };
    const glm::vec3 sun = unit(view.sky.sunPosition), moon = unit(view.sky.moonPosition);
    constexpr float kSunGone = -0.03f, kNight = -0.10f; // the sun's height: its last light, full night
    const bool bySun = sun.y > kSunGone;
    glm::vec3 toLight = bySun ? sun : moon;
    float strength = 0.0f;
    if (bySun) {
        strength = glm::smoothstep(kSunGone, 0.25f, sun.y);
    } else {
        const float night = 1.0f - glm::smoothstep(kNight, kSunGone, sun.y); // 0 as the sun goes, 1 once it is gone
        strength = 0.45f * std::max(glm::smoothstep(0.02f, 0.25f, moon.y), 0.35f) * night;
    }
    // Never lower than 25 degrees: long dawn shadows would cross whole regions.
    constexpr float kLowest = 0.4226f;
    if (toLight.y < kLowest) {
        const glm::vec2 across = glm::normalize(glm::vec2(toLight.x, toLight.z)) * std::sqrt(1.0f - kLowest * kLowest);
        toLight = glm::vec3(across.x, kLowest, across.y);
    }
    // Overcast under rain and snow.
    if (!view.weather.rain.empty() || !view.weather.snow[0].empty() || !view.weather.snow[1].empty()) strength *= 0.35f;

    // Low: 2 cascades in a 2048 atlas; Medium 3 in 4096; High 4 in 8192; Ultra as High, its splits
    // nearer the camera (more of the atlas on what is close); as far as the distance row.
    static constexpr int kCount[5] = {0, 2, 3, 4, 4};
    static constexpr float kReach[4] = {1800.0f, 2800.0f, 4000.0f, 6000.0f};
    const float reach = kReach[std::clamp(video_.shadowDistance, 0, 3)];
    // The light held between steps of a tenth of a degree (half a degree jumped visibly as the time went on; 2026-11-13):
    // the cascades' views stay put while it creeps, and a step redraws them all at once.
    if (drawnShadows_.count > 0 && glm::dot(toLight, shadowLight_) > 0.9999985f) toLight = shadowLight_;
    static constexpr std::uint32_t kAtlas[5] = {0, 2048, 4096, 8192, 8192};
    const std::uint32_t size = kAtlas[quality];
    if (size != shadowSize_) {
        device_->DestroyTexture(shadowAtlas_);
        device_->DestroyTexture(staticAtlas_);
        rhi::TextureDesc desc;
        desc.width = desc.height = size;
        desc.format = rhi::Format::D32Float;
        desc.usage = rhi::TextureUsage::ShadowMap;
        desc.debugName = "sun shadows";
        shadowAtlas_ = device_->CreateTexture(desc);
        desc.debugName = "sun shadows (static)";
        staticAtlas_ = device_->CreateTexture(desc);
        shadowSize_ = shadowAtlas_ && staticAtlas_ ? size : 0;
        drawnShadows_.count = 0; // the new static atlas holds nothing yet
    }
    if (!shadowAtlas_ || !staticAtlas_) return;
    // Snapped in steps of an eighth of each cascade: the cached static shadows last while the
    // camera moves inside a step.
    const float logarithmic = quality >= 4 ? 0.88f : 0.75f;
    const gfx::ShadowCascades fresh = gfx::BuildShadowCascades(viewProj_, eye_, toLight, kCount[quality], reach, int(size), 3000.0f, 8, logarithmic);
    // The static casters of a cascade are drawn again (into the cached atlas) only when its view
    // moved (a step of the camera) or the world's buildings changed, one far cascade a frame at
    // most; all of them when the light, the count or the atlas changed. The characters are drawn
    // every frame into the cascades that take them (the casters row).
    static constexpr int kCharacterCascades[3] = {4, 3, 2};
    shadowCharacterCascades_ = kCharacterCascades[std::clamp(video_.shadowCulling, 0, 2)];
    const bool all = fresh.count != drawnShadows_.count || size != drawnSize_ || toLight != shadowLight_;
    ++shadowFrame_;
    shadowRedraw_.clear();
    shadows_ = fresh;
    for (int i = 0; i < fresh.count; ++i) {
        const bool turn = i < 2 || (i % 2) == int(shadowFrame_ % 2);
        const bool moved = std::memcmp(&fresh.viewProj[std::size_t(i)], &drawnShadows_.viewProj[std::size_t(i)], sizeof(glm::mat4)) != 0 ||
                           drawnGeneration_[std::size_t(i)] != staticGeneration_;
        const bool now = all || (turn && moved);
        if (now) drawnGeneration_[std::size_t(i)] = staticGeneration_;
        if (now) {
            shadowRedraw_.push_back(i);
            drawnShadows_.viewProj[std::size_t(i)] = fresh.viewProj[std::size_t(i)];
            drawnShadows_.atlas[std::size_t(i)] = fresh.atlas[std::size_t(i)];
            drawnShadows_.texels[i] = fresh.texels[i];
        }
        // What the atlas holds for this cascade (drawn now or before).
        shadows_.atlas[std::size_t(i)] = drawnShadows_.atlas[std::size_t(i)];
        shadows_.texels[i] = drawnShadows_.texels[i];
    }
    drawnShadows_.count = fresh.count;
    drawnSize_ = size;
    shadowLight_ = toLight;
    shadowStrength_ = strength;
    shadowFilter_ = quality - 1;
}

void SceneRenderer::DrawShadows(rhi::CommandList& cmd, const SceneView& view) {
    SR_ZONE("render: sun shadows");
    const std::uint64_t frame = FrameConstants(view, true);
    const std::uint32_t half = shadowSize_ / 2;
    const auto square = [&](rhi::CommandList& c, int i) {
        const std::uint32_t x = std::uint32_t(i % 2) * half, y = std::uint32_t(i / 2) * half;
        c.SetViewport({float(x), float(y), float(half), float(half)});
        c.SetScissor({std::int32_t(x), std::int32_t(y), half, half});
        return rhi::Rect{std::int32_t(x), std::int32_t(y), half, half};
    };
    // The static casters of the cascades whose view moved, into the cached atlas.
    if (!shadowRedraw_.empty()) {
        rhi::RenderPassDesc cached;
        cached.depth = staticAtlas_;
        cached.loadDepth = true; // the other cascades keep what they hold
        cmd.BeginRenderPass(cached);
        for (std::size_t k = 0; k < shadowRedraw_.size(); ++k) {
            const int i = shadowRedraw_[k];
            cmd.ClearDepth(square(cmd, i), 1.0f);
            staticShadowPass_.DrawDepth(cmd, drawnShadows_.viewProj[std::size_t(i)], assets_->Geometry(), world_, assets_->Meshes(), frame,
                                        std::uint32_t(k));
        }
        cmd.EndPass();
    }
    // The live atlas: the cached one copied, then the characters.
    rhi::RenderPassDesc pass;
    pass.depth = shadowAtlas_; // cleared to 1
    cmd.BeginRenderPass(pass);
    modernPass_.CopyDepth(cmd, staticAtlas_);
    const int liveCount = std::min(shadowCharacterCascades_, shadows_.count);
    for (int i = 0; i < liveCount; ++i) {
        square(cmd, i);
        shadowPass_.DrawDepth(cmd, drawnShadows_.viewProj[std::size_t(i)], assets_->Geometry(), world_, assets_->Meshes(), frame,
                              std::uint32_t(i));
    }
    cmd.EndPass();
}

void SceneRenderer::Resolve(const SceneView& view) {
    // The models (an id evicted since the snapshot was built is skipped) and their instance records.
    resolved_.clear();
    items_.clear();
    for (const SceneItem& item : view.items) {
        std::shared_ptr<const gfx::ModelInfo> model = assets_->Model(item.model);
        if (!model) continue;
        const auto instance = static_cast<std::uint32_t>(items_.size());
        if (instance >= scene_.Capacity()) break;
        gfx::InstanceRecord record;
        std::memcpy(record.world, item.world.data(), sizeof(record.world));
        const bool skinned = item.paletteOffset != SceneItem::kNoPalette;
        record.paletteOffset = skinned ? item.paletteOffset : 0;
        scene_.Set(instance, record);
        items_.push_back({model.get(), instance, skinned});
        resolved_.push_back(std::move(model));
    }
}

void SceneRenderer::Draw(rhi::CommandList& cmd, const SceneView& view, const OrbitCamera& camera) {
    SR_ZONE("render: scene");
    std::uint64_t palette = 0;
    if (!view.palette.empty()) {
        const std::size_t bytes = view.palette.size() * sizeof(glm::mat4);
        const rhi::TransientAllocation alloc = device_->AllocateTransient(bytes, 64);
        if (alloc.cpu != nullptr) {
            std::memcpy(alloc.cpu, view.palette.data(), bytes);
            palette = alloc.gpuAddress;
        }
    }

    // The frame's constants, read by the world passes.
    const std::uint64_t frame = FrameConstants(view, true);

    rhi::RenderPassDesc pass;
    pass.color = color_;
    pass.depth = depth_;
    std::memcpy(pass.clearColor, view.clearColor.data(), sizeof(pass.clearColor));
    cmd.BeginRenderPass(pass);
    if (view.sky.active) {
        const auto bindless = [&](gfx::AssetId id) -> std::uint32_t {
            const auto texture = assets_->Texture(id);
            return texture ? texture->bindless : 0;
        };
        gfx::SkyPass::Textures textures;
        textures.clouds = bindless(cloudTexture_);
        textures.sun = bindless(sunTexture_);
        textures.moon = bindless(moonTextures_[static_cast<std::size_t>(std::clamp(view.sky.moonPhase, 0, 28))]);
        skyPass_.Draw(cmd, skyViewProj_, view.sky, textures, view.seconds, width_, height_);
    }
    if (frame != 0) terrainPass_.Draw(cmd, viewProj_, assets_->Geometry(), frame);
    // The original's circle shadows (its shadow detail row: 0 none; 1 the circles; 2 its projected shadows, ours the circles
    // still), unless the sun's cascaded shadows are drawn.
    if (!view.shadows.empty() && frame != 0 && shadows_.count == 0 && video_.current()[1] != 0) {
        if (const auto texture = assets_->Texture(shadowTexture_)) {
            terrainPass_.DrawShadows(cmd, viewProj_, frame, assets_->Geometry(), view.shadows, texture->bindless);
        }
    }
    // The Modern water reflects and refracts the scene: everything solid first, then a copy of the
    // colour and view depth, then the water over it (opaque, depth tested) in a second pass, then
    // the translucent parts (waterfalls), which write no depth the water could test against.
    if (frame != 0 && video_.water != 0) {
        using Parts = gfx::WorldPass::Parts;
        worldPass_.Draw(cmd, viewProj_, assets_->Geometry(), world_, assets_->Meshes(), frame, gfx::WorldPass::Phase::BeforeWater, Parts::Solid);
        worldPass_.Draw(cmd, viewProj_, assets_->Geometry(), world_, assets_->Meshes(), frame, gfx::WorldPass::Phase::AfterWater, Parts::Solid);
        cmd.EndPass();
        modernPass_.CopyForWater(cmd, color_, depth_, waterCopy_, viewProj_);
        rhi::RenderPassDesc again;
        again.color = color_;
        again.depth = depth_;
        again.loadColor = true;
        again.loadDepth = true;
        cmd.BeginRenderPass(again);
        const auto index = static_cast<std::size_t>(std::fmod(std::max(view.seconds, 0.0) * 10.0, 30.0));
        const auto water = assets_->Texture(waterFrames_[std::min<std::size_t>(index, waterFrames_.size() - 1)]);
        const auto ice = assets_->Texture(iceTexture_);
        const std::uint64_t waterFrame = FrameConstants(view, true, true);
        terrainPass_.DrawWater(cmd, viewProj_, waterFrame, assets_->Geometry(), water ? water->bindless : 0, ice ? ice->bindless : 0, true, nullptr,
                               video_.current()[5]);
        // The pools (water parts) and the translucent parts over the copy.
        worldPass_.Draw(cmd, viewProj_, assets_->Geometry(), world_, assets_->Meshes(), waterFrame, gfx::WorldPass::Phase::BeforeWater,
                        Parts::Translucent);
        worldPass_.Draw(cmd, viewProj_, assets_->Geometry(), world_, assets_->Meshes(), waterFrame, gfx::WorldPass::Phase::AfterWater,
                        Parts::Translucent);
    } else if (frame != 0) {
        // The original's order ("Water"): what stands below the water level, the water,
        // then the rest (the entities before it, seen through it under the surface).
        worldPass_.Draw(cmd, viewProj_, assets_->Geometry(), world_, assets_->Meshes(), frame, gfx::WorldPass::Phase::BeforeWater);
        if (terrainPass_.VisibleWater() != 0) {
            // A frame every 100 ms, as the original's (it counts wall-clock milliseconds).
            // Water Detail low: the water stands still (its first frame).
            const auto index = video_.current()[5] == 0 ? std::size_t{0} : static_cast<std::size_t>(std::fmod(std::max(view.seconds, 0.0) * 10.0, 30.0));
            const auto water = assets_->Texture(waterFrames_[std::min<std::size_t>(index, waterFrames_.size() - 1)]);
            const auto ice = assets_->Texture(iceTexture_);
            gfx::TerrainPass::WaterReflection reflection;
            if (reflecting_) {
                const auto bump = assets_->Texture(bumpTexture_);
                reflection.texture = device_->BindlessIndex(reflection_);
                reflection.bump = bump ? bump->bindless : 0u;
                reflection.width = float(width_);
                reflection.height = float(height_);
            }
            terrainPass_.DrawWater(cmd, viewProj_, frame, assets_->Geometry(), water ? water->bindless : 0, ice ? ice->bindless : 0,
                                   video_.water != 0, &reflection, video_.current()[5]);
        }
        worldPass_.Draw(cmd, viewProj_, assets_->Geometry(), world_, assets_->Meshes(), frame, gfx::WorldPass::Phase::AfterWater);
    }
    // Rain and snow (C4c), after the world; none with the weather row off.
    if (!view.weather.Empty() && video_.weather != 0) {
        const auto bindless = [&](gfx::AssetId id) -> std::uint32_t {
            const auto texture = assets_->Texture(id);
            return texture ? texture->bindless : 0;
        };
        gfx::WeatherPass::Textures textures;
        textures.rain = bindless(weatherTextures_[0]);
        textures.ripple = bindless(weatherTextures_[1]);
        textures.snow[0] = bindless(weatherTextures_[2]);
        textures.snow[1] = bindless(weatherTextures_[3]);
        // The camera's axes, as ViewProj builds its view (left-handed: right = up x forward).
        const glm::vec3 dir(std::cos(camera.pitch) * std::sin(camera.yaw), std::sin(camera.pitch), std::cos(camera.pitch) * std::cos(camera.yaw));
        const glm::vec3 forward = camera.fly ? dir : -dir;
        const glm::vec3 level = glm::normalize(glm::cross(glm::vec3(0, 1, 0), forward));
        weatherPass_.Draw(cmd, viewProj_, view.weather, textures, level, glm::normalize(glm::cross(forward, level)), level);
    }
    pass_.Record(cmd, viewProj_, items_, assets_->Geometry(), scene_, palette);
    cmd.EndPass();
    drawn_ = static_cast<std::uint32_t>(items_.size());
    rendered_ = true;
}

// 128 lights in a tile's mask (four words); the shader walks them (Frame.hlsli FrameEffectLight).
constexpr std::uint32_t kLightMaskWords = 4;
constexpr std::uint32_t kLightTile = 64;

void SceneRenderer::SelectLights(const SceneView& view, float lightReach, std::uint32_t tilesX, std::uint32_t tilesY) const {
    // The effects' lights (V2, the Modern row). In a crowd hundreds compete (the user's Hotan test, 2026-11-13: lights
    // "fighting for a spot"): the kept ones are the most telling (strength x reach / distance), up to 128 (High) or 48
    // (Medium); one kept last frame keeps its place against newcomers (x1.5), and a newcomer fades in over 0.2 s, so the
    // set changes a little at a time rather than trading places every frame.
    const double dt = lightClock_ < 0.0 ? 0.0 : std::clamp(view.seconds - lightClock_, 0.0, 0.25);
    const std::size_t capacity = video_.lightEffects >= 2 ? 128 : 48;
    const int perTile = video_.lightEffects >= 2 ? 48 : 24; // lights a screen tile keeps
    lightCandidates_.clear();
    for (const gfx::EffectLight& l : view.effects.lights) {
        const glm::vec3 d = l.position - eye_;
        if (glm::dot(d, d) < lightReach * lightReach) lightCandidates_.push_back(l);
    }
    const float t = view.lighting.timeOfDay;
    if (view.sky.active && (t < 0.25f || t > 0.75f)) {
        for (const auto& [slot, lights] : nightLights_) {
            for (const gfx::EffectLight& l : lights) {
                const glm::vec3 d = l.position - eye_;
                if (glm::dot(d, d) < lightReach * lightReach) lightCandidates_.push_back(l);
            }
        }
    }
    lightScores_.resize(lightCandidates_.size());
    effectOrder_.resize(lightCandidates_.size());
    for (std::size_t i = 0; i < lightCandidates_.size(); ++i) {
        const gfx::EffectLight& l = lightCandidates_[i];
        float score = l.intensity * std::max(l.radius, 1.0f) / std::max(glm::length(l.position - eye_), 100.0f);
        if (l.id != 0) {
            if (const auto m = lightMemory_.find(l.id); m != lightMemory_.end() && m->second.kept) score *= 1.5f;
        } else {
            score *= 1.5f; // a window: always the same
        }
        lightScores_[i] = score;
        effectOrder_[i] = static_cast<std::uint32_t>(i);
    }
    std::sort(effectOrder_.begin(), effectOrder_.end(), [&](std::uint32_t a, std::uint32_t b) { return lightScores_[a] > lightScores_[b]; });
    for (auto& [id, m] : lightMemory_) m.kept = false;
    lightGlows_.clear();
    tileMasks_.assign(std::size_t{tilesX} * tilesY * kLightMaskWords, 0u);
    std::uint32_t* masks = tileMasks_.data();
    for (const std::uint32_t at : effectOrder_) {
        if (lightGlows_.size() / 2 >= capacity) break;
        const gfx::EffectLight& l = lightCandidates_[at];
        const float radius = std::max(l.radius, 1.0f);
        // Its screen rectangle: the sphere's eight box corners projected (conservative).
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        bool behind = false;
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 p = l.position + radius * glm::vec3(c & 1 ? 1.0f : -1.0f, c & 2 ? 1.0f : -1.0f, c & 4 ? 1.0f : -1.0f);
            const glm::vec4 clip = viewProj_ * glm::vec4(p, 1.0f);
            if (clip.w <= 1.0f) {
                behind = true;
                break;
            }
            const float sx = (clip.x / clip.w * 0.5f + 0.5f) * float(width_), sy = (clip.y / clip.w * 0.5f + 0.5f) * float(height_);
            x0 = std::min(x0, sx), y0 = std::min(y0, sy), x1 = std::max(x1, sx), y1 = std::max(y1, sy);
        }
        if (behind) x0 = y0 = 0.0f, x1 = float(width_), y1 = float(height_);
        if (x1 < 0.0f || y1 < 0.0f || x0 >= float(width_) || y0 >= float(height_)) continue;
        const int tx0 = std::max(0, int(x0) / int(kLightTile)), ty0 = std::max(0, int(y0) / int(kLightTile));
        const int tx1 = std::min(int(tilesX) - 1, int(x1) / int(kLightTile)), ty1 = std::min(int(tilesY) - 1, int(y1) / int(kLightTile));
        // A light goes into all its tiles or none (kept in some and dropped from others, it ended at tile edges in
        // squares of light); a full tile turns it away (a crowd's casts would otherwise cost every pixel dozens).
        bool room = true;
        for (int ty = ty0; ty <= ty1 && room; ++ty) {
            for (int tx = tx0; tx <= tx1 && room; ++tx) {
                const std::uint32_t* tile = masks + (std::size_t(ty) * tilesX + std::size_t(tx)) * kLightMaskWords;
                int used = 0;
                for (std::uint32_t w = 0; w < kLightMaskWords; ++w) used += std::popcount(tile[w]);
                room = used < perTile;
            }
        }
        if (!room) continue;
        const std::size_t slot = lightGlows_.size() / 2;
        for (int ty = ty0; ty <= ty1; ++ty) {
            for (int tx = tx0; tx <= tx1; ++tx) {
                std::uint32_t* tile = masks + (std::size_t(ty) * tilesX + std::size_t(tx)) * kLightMaskWords;
                tile[slot / 32] |= 1u << (slot % 32);
            }
        }
        float weight = 1.0f;
        if (l.id != 0) {
            LightMemory& m = lightMemory_[l.id];
            m.kept = true;
            m.weight = std::min(1.0f, m.weight + static_cast<float>(dt) * 5.0f);
            m.seen = view.seconds;
            weight = m.weight;
        }
        // Faded over the last quarter of the reach from the eye rather than cut there.
        const float reachFade = std::clamp((lightReach - glm::length(l.position - eye_)) / (lightReach * 0.25f), 0.0f, 1.0f);
        lightGlows_.push_back(glm::vec4(l.position, radius));
        lightGlows_.push_back(glm::vec4(l.colour * l.intensity * reachFade * weight, 0.0f));
    }
    // One not kept starts again from dark when it comes back; long gone ones are forgotten.
    for (auto it = lightMemory_.begin(); it != lightMemory_.end();) {
        if (!it->second.kept) it->second.weight = 0.0f;
        it = view.seconds - it->second.seen > 3.0 ? lightMemory_.erase(it) : std::next(it);
    }
}

std::uint64_t SceneRenderer::FrameConstants(const SceneView& view, bool shadows, bool waterCopy) const {
    gfx::FrameConstants constants = gfx::MakeFrameConstants(viewProj_, eye_, view.lighting, view.seconds, view.lights.size());
    // The Metallic Sheen: the Video page's row 6 (the original's engine option 12) on or off, our row its look; its colour
    // the environment's diffuse (the original's texture factor, set every frame from the world's colour).
    {
        const auto gray = assets_->Texture(sphereGray_), highlight = assets_->Texture(sphereHighlight_);
        const float mode = video_.current()[6] == 0 ? 0.0f : video_.metalSheen != 0 ? 2.0f : 1.0f;
        // w: the Modern look's probe (bindless), once each of its faces has been drawn; the main scene's passes only (`shadows`:
        // the probe's own pass writes it, the reflection's and the overlay's do not declare it).
        const float probe = shadows && metalProbeMade_ && metalProbeFaces_ >= 6 && mode > 1.5f ? float(device_->BindlessIndex(metalProbe_)) : 0.0f;
        constants.metal = glm::vec4(gray ? float(gray->bindless) : 0.0f, highlight ? float(highlight->bindless) : 0.0f, gray ? mode : 0.0f, probe);
        constants.metalColour = glm::vec4(view.lighting.sunDiffuse, 1.0f);
        // GLM's left-handed look-at: its rows are the camera's right, up and forward.
        constants.cameraRight = glm::vec4(cameraView_[0][0], cameraView_[1][0], cameraView_[2][0], 0.0f);
        constants.cameraUp = glm::vec4(cameraView_[0][1], cameraView_[1][1], cameraView_[2][1], 0.0f);
    }
    static constexpr float kLightReach[3] = {800.0f, 1500.0f, 3000.0f};
    const float lightReach = kLightReach[std::clamp(video_.lightReach, 0, 2)];
    const std::uint32_t tilesX = (width_ + kLightTile - 1) / kLightTile, tilesY = (height_ + kLightTile - 1) / kLightTile;
    // The effects' lights: chosen once a frame (the water pass's constants reuse them).
    std::size_t effectLights = 0;
    if (shadows && video_.lightEffects > 0) {
        if (!lightsSelected_ || view.seconds != lightClock_) {
            SelectLights(view, lightReach, tilesX, tilesY);
            lightClock_ = view.seconds;
            lightsSelected_ = true;
        }
        effectLights = lightGlows_.size() / 2;
        constants.waterCopy.z = static_cast<float>(effectLights);
    }
    if (shadows && shadows_.count > 0) {
        for (int i = 0; i < 4; ++i) constants.shadowAtlas[i] = shadows_.atlas[std::size_t(i)];
        constants.shadowSplits = shadows_.splits;
        constants.shadowTexels = shadows_.texels;
        constants.shadowParams = glm::vec4(float(shadows_.count), float(device_->BindlessIndex(shadowAtlas_)), shadowStrength_, float(shadowFilter_));
        constants.shadowLight = glm::vec4(shadowLight_, 1.0f);
    }
    // The Modern rows the world shaders read (not the overlay's own camera: its characters keep
    // the original's light).
    if (shadows) {
        constants.modern = glm::vec4(video_.characterLighting != 0 ? 1.0f : 0.0f, video_.foliageSway != 0 ? 1.0f : 0.0f,
                                     video_.weather == 2 ? view.weather.ground.x : 0.0f, video_.lightEffects > 0 ? 1.0f : 0.0f);
        // The Modern weather's ground (wet, snow lying): none with the Original's or the weather off.
        if (video_.weather == 2) {
            constants.weather = view.weather.ground;
            // A shower's mist: the linear fog's far end comes in by up to a quarter with the rain falling.
            const float rain = view.weather.ground.z;
            if (rain > 0.0f && constants.fogRange.y > constants.fogRange.x) {
                const float end = constants.fogRange.x + (constants.fogRange.y - constants.fogRange.x) * (1.0f - 0.25f * rain);
                constants.fogRange.y = end;
                constants.fogRange.z = 1.0f / std::max(end - constants.fogRange.x, 1.0f);
            }
        }
    }
    if (waterCopy) {
        constants.waterCopy.x = float(device_->BindlessIndex(waterCopy_));
        constants.waterCopy.y = 1.0f;
    }
    // w: the Modern water row is on (objects' water surfaces, a fountain's pool, shade like water).
    if (shadows) constants.waterCopy.w = video_.water != 0 ? 1.0f : 0.0f;
    // The sky and the sun (the modern water reflects and glints them).
    if (shadows && view.sky.active) {
        constants.skyZenith = glm::vec4(view.sky.zenith, 1.0f);
        constants.skyHorizon = glm::vec4(view.sky.horizon, 1.0f);
        const float sunLength = glm::length(view.sky.sunPosition);
        const glm::vec3 toSun = sunLength > 0.0f ? view.sky.sunPosition / sunLength : glm::vec3(0, 1, 0);
        const float up = view.sky.sunVisible ? glm::smoothstep(0.0f, 0.15f, toSun.y) : 0.0f;
        constants.sun = glm::vec4(toSun, up);
        constants.sunColour = glm::vec4(view.sky.sunColor, 1.0f);
    }
    // The effects' lights binned by 64-pixel screen tiles (SelectLights): kLightMaskWords words a tile.
    const std::size_t tileOffset = sizeof(constants) + view.lights.size() * sizeof(gfx::PointLightGpu) + effectLights * 2 * sizeof(glm::vec4);
    const std::size_t tileBytes = effectLights > 0 ? std::size_t{tilesX} * tilesY * kLightMaskWords * 4 : 0;
    constants.lightTiles = glm::vec4(float(tilesX), float(kLightTile), float(tileOffset), float(tilesY));
    // The material tints after the tiles (16-byte aligned): kFrameLights y their offset, z their count.
    const std::size_t tintOffset = (tileOffset + tileBytes + 15) & ~std::size_t{15};
    const std::size_t tints = std::min<std::size_t>(view.tints.size(), 127);
    constants.lights.y = float(tintOffset);
    constants.lights.z = float(tints);
    const std::size_t bytes = tintOffset + tints * sizeof(glm::vec4);
    const rhi::TransientAllocation alloc = device_->AllocateTransient(bytes, 16);
    if (alloc.cpu == nullptr) return 0;
    std::memcpy(alloc.cpu, &constants, sizeof(constants));
    auto* lights = reinterpret_cast<gfx::PointLightGpu*>(static_cast<std::byte*>(alloc.cpu) + sizeof(constants));
    for (std::size_t i = 0; i < view.lights.size(); ++i) lights[i] = gfx::MakePointLight(view.lights[i]);
    // Written whole: the transient memory is write-combined (reading it back, as |= does, costs milliseconds).
    if (effectLights > 0) std::memcpy(lights + view.lights.size(), lightGlows_.data(), effectLights * 2 * sizeof(glm::vec4));
    if (tileBytes > 0) std::memcpy(static_cast<std::byte*>(alloc.cpu) + tileOffset, tileMasks_.data(), tileBytes);
    if (tints > 0) std::memcpy(static_cast<std::byte*>(alloc.cpu) + tintOffset, view.tints.data(), tints * sizeof(glm::vec4));
    return alloc.gpuAddress;
}

// The original's reflection render: its depth cleared, its colour kept (as the original's: where nothing draws, the last
// frame's shows), the sky, the terrain and the world from the mirrored camera.
void SceneRenderer::DrawReflection(rhi::CommandList& cmd, const SceneView& view) {
    SR_ZONE("render: water reflection");
    const std::uint64_t frame = FrameConstants(view, false);
    rhi::RenderPassDesc pass;
    pass.color = reflection_;
    pass.depth = reflectionDepth_;
    pass.loadColor = true;
    cmd.BeginRenderPass(pass);
    if (view.sky.active) {
        const auto bindless = [&](gfx::AssetId id) -> std::uint32_t {
            const auto texture = assets_->Texture(id);
            return texture ? texture->bindless : 0;
        };
        gfx::SkyPass::Textures textures;
        textures.clouds = bindless(cloudTexture_);
        textures.sun = bindless(sunTexture_);
        textures.moon = bindless(moonTextures_[static_cast<std::size_t>(std::clamp(view.sky.moonPhase, 0, 28))]);
        skyPass_.Draw(cmd, reflectSkyViewProj_, view.sky, textures, view.seconds, 512, 512);
    }
    if (frame != 0) {
        terrainPass_.Draw(cmd, reflectViewProj_, assets_->Geometry(), frame);
        reflectionPass_.Draw(cmd, reflectViewProj_, assets_->Geometry(), world_, assets_->Meshes(), frame, gfx::WorldPass::Phase::BeforeWater);
        reflectionPass_.Draw(cmd, reflectViewProj_, assets_->Geometry(), world_, assets_->Meshes(), frame, gfx::WorldPass::Phase::AfterWater);
    }
    cmd.EndPass();
}

void SceneRenderer::DrawMetalProbe(rhi::CommandList& cmd, const SceneView& view) {
    SR_ZONE("render: metal probe");
    const std::uint64_t frame = FrameConstants(view, false);
    const std::uint32_t face = metalProbeFace_ % 6;
    rhi::RenderPassDesc pass;
    pass.color = metalProbe_;
    pass.depth = metalProbeDepth_;
    pass.loadColor = true; // the other faces stay
    cmd.BeginRenderPass(pass);
    const std::uint32_t x = (face % 3) * kMetalFace, y = (face / 3) * kMetalFace;
    cmd.SetViewport({float(x), float(y), float(kMetalFace), float(kMetalFace)});
    cmd.SetScissor({std::int32_t(x), std::int32_t(y), kMetalFace, kMetalFace});
    if (view.sky.active) {
        const auto bindless = [&](gfx::AssetId id) -> std::uint32_t {
            const auto texture = assets_->Texture(id);
            return texture ? texture->bindless : 0;
        };
        gfx::SkyPass::Textures textures;
        textures.clouds = bindless(cloudTexture_);
        textures.sun = bindless(sunTexture_);
        textures.moon = bindless(moonTextures_[static_cast<std::size_t>(std::clamp(view.sky.moonPhase, 0, 28))]);
        skyPass_.Draw(cmd, metalProbeSkyViewProj_, view.sky, textures, view.seconds, kMetalFace, kMetalFace);
    }
    if (frame != 0) {
        terrainPass_.Draw(cmd, metalProbeViewProj_, assets_->Geometry(), frame);
        metalProbePass_.Draw(cmd, metalProbeViewProj_, assets_->Geometry(), world_, assets_->Meshes(), frame, gfx::WorldPass::Phase::BeforeWater);
        metalProbePass_.Draw(cmd, metalProbeViewProj_, assets_->Geometry(), world_, assets_->Meshes(), frame, gfx::WorldPass::Phase::AfterWater);
    }
    cmd.EndPass();
    ++metalProbeFace_;
    metalProbeFaces_ = std::min<std::uint32_t>(metalProbeFaces_ + 1, 6);
}

void SceneRenderer::DrawOverlay(rhi::CommandList& cmd, const SceneView& view) {
    SR_ZONE("render: scene overlay");
    const std::uint64_t frame = FrameConstants(view, false);
    rhi::RenderPassDesc pass;
    pass.color = overlayColor_;
    pass.depth = overlayDepth_;
    const float clear[4] = {0, 0, 0, 0}; // what it does not cover shows what is under it
    std::memcpy(pass.clearColor, clear, sizeof(clear));
    cmd.BeginRenderPass(pass);
    if (frame != 0) {
        overlayPass_.Draw(cmd, overlayViewProj_, assets_->Geometry(), overlay_, assets_->Meshes(), frame, gfx::WorldPass::Phase::BeforeWater);
        overlayPass_.Draw(cmd, overlayViewProj_, assets_->Geometry(), overlay_, assets_->Meshes(), frame, gfx::WorldPass::Phase::AfterWater);
    }
    cmd.EndPass();
    overlayRendered_ = true;
}

std::uint32_t SceneRenderer::DepthTexture() const { return depth_ ? device_->BindlessIndex(depth_) : 0u; }

std::uint32_t SceneRenderer::TakeOverlayTexture() {
    if (!overlayRendered_) return 0;
    overlayRendered_ = false;
    return device_->BindlessIndex(overlayColor_);
}

void SceneRenderer::CompositeOverlay(rhi::CommandList& cmd) {
    if (!overlayRendered_) return;
    BlitPush push;
    push.textureIndex = device_->BindlessIndex(overlayColor_);
    push.samplerIndex = static_cast<std::uint32_t>(rhi::SamplerIndex::PointClamp);
    cmd.BindPipeline(blitOver_);
    cmd.PushConstants(&push, sizeof(push));
    cmd.Draw(3, 0);
    overlayRendered_ = false;
}

void SceneRenderer::Composite(rhi::CommandList& cmd) {
    if (!rendered_) return;
    BlitPush push;
    push.textureIndex = device_->BindlessIndex(result_ ? result_ : color_);
    push.samplerIndex = static_cast<std::uint32_t>(rhi::SamplerIndex::PointClamp);
    cmd.BindPipeline(blit_);
    cmd.PushConstants(&push, sizeof(push));
    cmd.Draw(3, 0);
    rendered_ = false;
}

} // namespace sr::client
