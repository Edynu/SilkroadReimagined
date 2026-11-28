#pragma once

// The render thread's side of the scene:
// - the snapshot's terrain regions, drawn block by block (TerrainPass);
// - the persistent world: instances added, moved and removed by the snapshots' world commands,
//   kept in a GPU scene and drawn through GPU culling and indirect draws (WorldPass);
// - the snapshot's immediate models (the viewer, the asset check), drawn one by one (ModelPass);
// both into an offscreen colour + depth target, composited into the swapchain pass.

#include <array>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

#include "frame/FrameSnapshot.h"
#include "frame/FrameStats.h"
#include "sr/core/Options.h"
#include "sr/gfx/AssetStore.h"
#include "sr/gfx/EffectPass.h"
#include "sr/gfx/GpuScene.h"
#include "sr/gfx/ModelPass.h"
#include "sr/gfx/ModernPass.h"
#include "sr/gfx/PostPass.h"
#include "sr/gfx/RenderGraph.h"
#include "sr/gfx/ShadowCascades.h"
#include "sr/gfx/SkyPass.h"
#include "sr/gfx/TerrainPass.h"
#include "sr/gfx/WeatherPass.h"
#include "sr/gfx/WorldPass.h"
#include "sr/rhi/Rhi.h"

namespace sr::client {

class SceneRenderer {
public:
    // `skinVertices`: the frame's skinned vertices in the world view (32 bytes each; parts past it
    // are not drawn that frame), and a skinned part for every 64 of them; the shadow views get half.
    bool Init(rhi::Device& device, gfx::AssetStore& assets, std::uint32_t skinVertices, std::string& error);
    void Shutdown();

    // Every frame, before AddPasses: the snapshot's changes to the persistent world.
    void ApplyWorldCommands(std::span<const WorldCommand> commands);

    // The scene's passes in the frame's graph: instance and mesh records (a transfer), the world's
    // culling, then the world and the immediate models into the scene's colour and depth. Returns
    // the colour result, which the swapchain pass reads (Composite). `view` and `camera`
    // (late-latched) must live until the graph ran.
    gfx::RenderGraph::Resource AddPasses(gfx::RenderGraph& graph, const SceneView& view, const OrbitCamera& camera,
                                         std::uint32_t width, std::uint32_t height);
    // The player's video options (the snapshot's), before AddPasses.
    // The video options; the texture detail row (slot 9: 0 low .. 2 high) skips 2 - it of the textures' largest mips.
    void SetVideo(const core::VideoOptions& video) {
        video_ = video;
        if (assets_ != nullptr) assets_->SetMipSkip(2 - std::clamp(video.current()[9], 0, 2));
    }
    // Inside the swapchain pass.
    void Composite(rhi::CommandList& cmd);
    // The overlay's colour (0 when it has nothing), and its composite (blended, inside the swapchain
    // pass, after the UI under it).
    gfx::RenderGraph::Resource OverlayResult() const { return overlayResult_; }
    // The overlay's target for the UI to draw (SceneView::overlayOwnCamera; 0 when not rendered).
    std::uint32_t TakeOverlayTexture();
    // The scene's depth target (bindless; sampled by the UI's world-placed labels), 0 when none.
    std::uint32_t DepthTexture() const;
    void CompositeOverlay(rhi::CommandList& cmd);

    // The scene's depth last read back at the pixel a snapshot asked for (SceneView::probeX / probeY).
    const DepthProbe& Probe() const { return probe_; }
    // The sound occlusion's depth cube last read back (SceneView::audioCube; SR_AUDIO_OCCLUSION), once.
    std::shared_ptr<const AudioCube> TakeAudioCube() { return std::move(audioCubeReady_); }

    std::uint32_t DrawnModels() const { return drawn_; }
    std::uint32_t WorldInstances() const { return world_.Live(); }
    std::uint32_t TerrainBlocks() const { return terrainPass_.VisibleBlocks(); }

private:
    void EnsureTargets(std::uint32_t width, std::uint32_t height);
    void Resolve(const SceneView& view); // the snapshot's models -> resolved_, items_, instance records
    void ResolveWorld();                 // world instances whose model just became resident
    void ResolveTerrain(const SceneView& view); // the snapshot's resident regions -> terrainRegions_
    void Draw(rhi::CommandList& cmd, const SceneView& view, const OrbitCamera& camera);
    // The sun's cascaded shadows (G1): the light, the cascades and the atlas for this frame.
    void PrepareShadows(const SceneView& view, int quality);
    void DrawShadows(rhi::CommandList& cmd, const SceneView& view);
    // The sound occlusion's depth cube (ours, experimental; SR_AUDIO_OCCLUSION): the static world's objects culled for six
    // faces round the eye (two depth-only passes of three views; no characters, no swaying parts, nothing under a few
    // units), drawn into the 3 x 2 atlas, copied into a mapped slot and handed back once its frame is done.
    void AddAudioCube(gfx::RenderGraph& graph, gfx::RenderGraph::Resource instances, gfx::RenderGraph::Resource meshes,
                      gfx::RenderGraph::Resource geometry, const SceneView& view);
    void DrawAudioCube(rhi::CommandList& cmd);
    static constexpr std::uint32_t kAudioSlots = 3;
    struct AudioAsk {
        std::uint64_t frame = 0;
        glm::vec3 eye{0.0f};
        std::array<glm::mat4, 6> viewProj{};
    };
    bool audioReady_ = false; // its passes and targets made (on first use)
    gfx::WorldPass audioPass_[2];
    gfx::WorldPass::Lists audioLists_[2];
    rhi::TextureHandle audioAtlas_;
    rhi::PipelineHandle audioReadPipeline_;
    rhi::BufferHandle audioBuffer_;
    std::array<AudioAsk, kAudioSlots> audioAsks_{};
    std::array<glm::mat4, 6> audioViewProj_{};
    std::shared_ptr<const AudioCube> audioCubeReady_;

    rhi::Device* device_ = nullptr;
    gfx::AssetStore* assets_ = nullptr;
    gfx::GpuScene scene_; // immediate models, rewritten every frame
    gfx::ModelPass pass_;
    // The persistent world.
    gfx::GpuScene world_;
    gfx::WorldPass worldPass_;
    std::vector<std::uint32_t> handleSlots_; // the game's handle -> world_ / overlay_ slot (kNoSlot: none)
    std::vector<std::uint8_t> handleOverlay_; // the handle's instance is in overlay_
    struct Pending {
        std::uint32_t slot = 0;
        gfx::AssetId model;
        bool overlay = false;
        std::uint8_t hiddenTail = 0;
    };
    // A resolved instance's model's whole part count, by scene (0 world, 1 overlay) and slot: a
    // hidden tail is taken off it.
    std::unordered_map<std::uint64_t, std::uint32_t> fullParts_;
    // The overlay: instances drawn by their own world pass into their own target, cleared clear.
    gfx::GpuScene overlay_;
    gfx::WorldPass overlayPass_;
    gfx::WorldPass::Lists overlayLists_;
    rhi::TextureHandle overlayColor_, overlayDepth_;
    // The original's water reflection (its row 4 on, the Original look's water): the scene drawn mirrored about the water
    // level into a 512 x 512 target (only its depth cleared, as the original's), its culling of its own;
    // skybox\waterbump3.ddj bends where the water samples it.
    gfx::WorldPass reflectionPass_;
    gfx::WorldPass::Lists reflectionLists_;
    rhi::TextureHandle reflection_, reflectionDepth_;
    gfx::AssetId bumpTexture_;
    // The Metallic Sheen's sphere maps (the original's object renderer's, prim\mtrl\etc\), and the camera's view
    // (its right and up turn a normal into the sphere map's texel).
    gfx::AssetId sphereGray_, sphereHighlight_;
    glm::mat4 cameraView_{1.0f};
    // The Modern metal sheen's reflection probe (ours): the scene round the player (the camera's target) in six faces of
    // kMetalFace pixels (a 3 x 2 atlas), one face drawn a frame, sampled by the shining parts' reflection; ready once each
    // face has been drawn.
    static constexpr std::uint32_t kMetalFace = 128;
    gfx::WorldPass metalProbePass_;
    gfx::WorldPass::Lists metalProbeLists_;
    rhi::TextureHandle metalProbe_, metalProbeDepth_;
    bool metalProbeMade_ = false, metalProbeDrawing_ = false;
    std::uint32_t metalProbeFace_ = 0, metalProbeFaces_ = 0;
    glm::mat4 metalProbeViewProj_{1.0f}, metalProbeSkyViewProj_{1.0f};
    void DrawMetalProbe(rhi::CommandList& cmd, const SceneView& view);
    glm::mat4 reflectViewProj_{1.0f}, reflectSkyViewProj_{1.0f};
    bool reflecting_ = false;
    void DrawReflection(rhi::CommandList& cmd, const SceneView& view);
    rhi::PipelineHandle blitOver_;
    gfx::RenderGraph::Resource overlayResult_ = 0;
    bool overlayRendered_ = false;
    void DrawOverlay(rhi::CommandList& cmd, const SceneView& view);
    // `shadows`: with this frame's sun shadows (the scene; not the overlay's own camera).
    // `waterCopy`: with the scene's copy for the modern water (waterCopy_, made before it).
    std::uint64_t FrameConstants(const SceneView& view, bool shadows, bool waterCopy = false) const;
    // The effects' lights of this frame (once a frame; FrameConstants' later calls reuse them): lightGlows_ (two vec4
    // a light) and tileMasks_ (kLightMaskWords a tile).
    void SelectLights(const SceneView& view, float lightReach, std::uint32_t tilesX, std::uint32_t tilesY) const;
    std::vector<Pending> unresolved_; // instances whose model is not resident yet
    gfx::WorldPass::Lists lists_;
    // The sun's shadows: their casters culled for the whole reach, skinned and drawn into each
    // cascade's square of the atlas.
    gfx::WorldPass shadowPass_;        // the characters, every frame
    gfx::WorldPass::Lists shadowLists_;
    gfx::WorldPass staticShadowPass_;  // the static casters, when a cascade's view moved
    gfx::WorldPass::Lists staticShadowLists_;
    rhi::TextureHandle staticAtlas_;   // the static casters' shadows, kept between frames
    rhi::TextureHandle shadowAtlas_;
    std::uint32_t shadowSize_ = 0;
    gfx::ShadowCascades shadows_;      // count 0: none this frame (atlas: what the atlas holds)
    gfx::ShadowCascades drawnShadows_; // each cascade as last drawn into the atlas
    std::vector<int> shadowRedraw_;    // the cascades drawn this frame
    std::uint64_t shadowFrame_ = 0;
    std::uint32_t drawnSize_ = 0;
    int shadowCharacterCascades_ = 2;              // cascades characters cast into (the casters row)
    // The near cascades the swaying plants are drawn into every frame (not cached), at most the characters' cascades.
    static constexpr std::size_t kLiveFoliageCascades = 2;
    std::uint64_t staticGeneration_ = 0;           // counts changes of the world's static instances
    std::array<std::uint64_t, 4> drawnGeneration_{}; // each cascade's at its last drawing
    glm::vec3 shadowLight_{0, 1, 0};   // towards the light
    float shadowStrength_ = 0.0f;
    int shadowFilter_ = 0;
    // Terrain: the regions drawn this frame (kept alive while recording).
    gfx::TerrainPass terrainPass_;
    gfx::AssetId shadowTexture_; // skybox\shadowsphere.ddj, held while the renderer lives
    std::vector<gfx::AssetId> waterFrames_; // water\water101..130.ddj (10 a second)
    gfx::SkyPass skyPass_;
    gfx::WeatherPass weatherPass_;
    gfx::AssetId weatherTextures_[4]; // weather\rain1, rain2, snow1, snow2
    gfx::AssetId cloudTexture_, sunTexture_;  // skybox\cloud1.ddj, sun\lens2.ddj
    std::vector<gfx::AssetId> moonTextures_;  // sun\moon01..29.ddj
    gfx::PostPass postPass_;
    gfx::ModernPass modernPass_;
    core::VideoOptions video_ = core::VideoOptions::Defaults();
    gfx::EffectPass effectPass_; // the effects over the scene, before the post pass (V1)
    std::vector<gfx::AssetId> lensTextures_;  // sun\lens3..8.ddj (the flare)
    gfx::AssetId iceTexture_;               // water\water201.ddj
    std::vector<std::shared_ptr<const gfx::TerrainInfo>> terrainInfos_;
    std::vector<gfx::TerrainPass::Region> terrainRegions_;
    glm::mat4 viewProj_{1.0f};
    glm::mat4 overlayViewProj_{1.0f}; // the overlay's (its own camera, or the scene's)
    glm::mat4 skyViewProj_{1.0f}; // the camera's turn only, far beyond the sky
    glm::vec3 eye_{0.0f};

    rhi::PipelineHandle blit_;
    rhi::TextureHandle color_, depth_;
    rhi::TextureHandle post_;    // the scene after glow and flare (HDR)
    rhi::TextureHandle picture_; // the 8-bit picture after the last steps (gfx::ModernPass)
    rhi::TextureHandle waterCopy_; // the scene before the modern water: colour, view depth in alpha
    mutable std::vector<std::uint32_t> tileMasks_;   // the effects' lights by screen tile, before the copy
    mutable std::vector<std::uint32_t> effectOrder_; // the effects' lights, the most telling first (SelectLights)
    mutable std::vector<float> lightScores_;
    mutable std::vector<glm::vec4> lightGlows_;      // the lights kept this frame: position and radius, colour
    // Each light's place in the kept set from frame to frame (by its effect's handle): kept last frame (it keeps its
    // place against newcomers), its fade in (0..1), when last seen.
    struct LightMemory {
        bool kept = false;
        float weight = 0.0f;
        double seen = 0.0;
    };
    mutable std::unordered_map<std::uint32_t, LightMemory> lightMemory_;
    mutable double lightClock_ = -1.0;
    mutable bool lightsSelected_ = false;
    mutable std::vector<gfx::EffectLight> lightCandidates_; // the effects' and, at night, the windows'
    // The windows' light at night (Modern, with the lights from effects): a warm light at each
    // night-light part of the world's buildings, by their world slot.
    std::unordered_map<std::uint32_t, std::vector<gfx::EffectLight>> nightLights_;
    // The depth probe: a texel of the scene's depth copied by a compute pass into a slot of a mapped buffer, read once
    // its frame has finished (a ring, so no frame waits).
    static constexpr std::uint32_t kProbeSlots = 4;
    rhi::PipelineHandle probePipeline_;
    rhi::BufferHandle probeBuffer_;
    struct ProbeAsk {
        std::uint64_t frame = 0; // the frame that copies it (0: the slot is free)
        float x = 0, y = 0;      // the pixel asked for
    };
    std::array<ProbeAsk, kProbeSlots> probeAsks_{};
    DepthProbe probe_;
    void AddProbe(gfx::RenderGraph& graph, gfx::RenderGraph::Resource depth, const SceneView& view);
    rhi::TextureHandle result_;  // what Composite shows
    std::uint32_t width_ = 0, height_ = 0;
    std::vector<std::shared_ptr<const gfx::ModelInfo>> resolved_; // kept alive while recording
    std::vector<gfx::ModelPass::Item> items_;
    std::uint32_t drawn_ = 0;
    bool rendered_ = false;
};

} // namespace sr::client
