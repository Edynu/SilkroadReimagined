#pragma once

// What the game thread hands the render thread each frame: a
// self-contained description of the frame. Once published it is immutable; the render thread
// never sees game state. Three slots: one being written, one being rendered, one waiting for the
// GPU frame that used it.

#include <array>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

#include <glm/glm.hpp>

#include "sr/core/Options.h"
#include "sr/devui/ImGuiLayer.h"
#include "sr/gfx/AssetStore.h"
#include "sr/gfx/Effects.h"
#include "sr/gfx/Environment.h"
#include "sr/gfx/PortraitPass.h"
#include "sr/gfx/TerrainPass.h"
#include "sr/gfx/UiPass.h"
#include "sr/gfx/WeatherPass.h"
#include "sr/rhi/Rhi.h"

namespace sr::client {

// A camera's state; the render thread re-reads yaw and pitch from the newest input (late latch)
// before it draws. An orbit camera looks at the target from `distance` away; a fly camera
// (`fly`) stands at the target and looks along (cos pitch sin yaw, sin pitch, cos pitch cos yaw).
struct OrbitCamera {
    float targetX = 0, targetY = 0, targetZ = 0;
    float yaw = 0.6f, pitch = 0.55f, distance = 700.0f;
    float fovY = 0.9f;
    bool fly = false;
    float nearZ = 0, farZ = 0; // 0: from the orbit distance
};

// The crowd benchmark's view (bench/): bone transforms already in GPU memory.
struct CrowdView {
    bool active = false;
    rhi::BufferHandle palettes;         // this slot's palette buffer, written by the pose jobs
    std::uint32_t bones = 0;            // bones (instances) in it
    std::uint32_t bonesPerCharacter = 0;
    std::uint32_t chunks = 8;           // parallel recording chunks
    OrbitCamera camera;
};

// Streamed models to draw: asset ids the screen holds references on, resolved
// by the render thread (an id evicted meanwhile is skipped).
struct SceneItem {
    static constexpr std::uint32_t kNoPalette = 0xFFFFFFFFu;
    gfx::AssetId model;
    std::array<float, 12> world{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; // rows of a 3x4 matrix
    std::uint32_t paletteOffset = kNoPalette; // first matrix in SceneView::palette (skinned models)
};

// A region's terrain to draw (C3c): the screen holds a reference on it.
struct TerrainItem {
    gfx::AssetId region;
    std::array<float, 3> offset{}; // its south-west corner in world space
};

struct SceneView {
    bool active = false;
    OrbitCamera camera;
    std::array<float, 4> clearColor{0.18f, 0.2f, 0.24f, 1.0f};
    std::vector<SceneItem> items;
    std::vector<glm::mat4> palette; // this frame's skinning matrices
    std::vector<TerrainItem> terrain;
    // The world's skinned instances (C3d): their matrices (glm::mat4 each, at the instances'
    // palette offsets), in a buffer the game wrote for this snapshot slot; none: drawn unposed.
    rhi::BufferHandle worldPalettes;
    std::vector<gfx::TerrainPass::Shadow> shadows; // blob shadows on the terrain
    gfx::SceneLighting lighting;                    // the environment now (C3e)
    std::vector<gfx::PointLight> lights;            // point lights instances take (dungeons, C4d)
    // The material tints instances take (WorldCommand::tint; rgb, a skill's SCT_MAT now; at most 127).
    std::vector<glm::vec4> tints;
    gfx::WeatherView weather;                       // rain and snow (C4c)
    // The effects (V1): sprites and meshes drawn after the scene, and the lights they give
    // (effects.lights, for V2's lighting).
    gfx::EffectFrame effects;
    gfx::SkyState sky;                              // drawn when active
    // The original's glow and lens flare (C3g); the flare needs an active sky.
    // The overlay through a camera of its own (column-major view-projection: a window's character,
    // the equipment's), drawn by the UI where a window asks (ui::kSceneOverlayTexture) instead of
    // over the whole screen.
    bool overlayOwnCamera = false;
    std::array<float, 16> overlayViewProj{};
    bool glow = false;
    bool lensFlare = false;
    float viewDistance = 5500.0f; // the original's view distance (the flare's probe)
    // The pixel (swapchain pixels; x < 0: none) whose scene depth the renderer reads back (FeedbackChannel::Probe): the
    // mouse's, for the clicks' occlusion.
    float probeX = -1.0f, probeY = -1.0f;
    // The sound occlusion's depth cube round the player's head (FeedbackChannel::LatestAudioCube; SR_AUDIO_OCCLUSION):
    // drawn and read back while set, centred on `audioEye` (the scene's coordinates).
    bool audioCube = false;
    std::array<float, 3> audioEye{};
    float frameSeconds = 0.0f;    // since the last game frame
    double seconds = 0;                             // game time, for animated textures
};

// Changes to the persistent world: the render thread keeps every instance in
// its GPU scene and draws them through GPU culling, so a frame carries only what changed. Handles
// are the game's; the model must stay referenced by the game while an instance shows it.
struct WorldCommand {
    static constexpr std::uint32_t kNoPalette = 0xFFFFFFFFu;
    enum class Kind : std::uint8_t { Add, Move, Remove };
    Kind kind = Kind::Add;
    std::uint32_t handle = 0;
    gfx::AssetId model;                // Add
    std::array<float, 12> world{};     // Add, Move: rows of a 3x4 matrix
    // Add: a skinned instance's first matrix in SceneView::worldPalettes (kept for its lifetime).
    std::uint32_t paletteOffset = kNoPalette;
    // Add: 1 + the point light (SceneView::lights) the instance takes; 0 none (kept for its lifetime).
    std::uint16_t light = 0;
    // Add: drawn in the scene's overlay (its own target, composited over gfx::UiFrame `uiUnder`
    // and under the UI): the original's character windows over the screen's bars.
    bool overlay = false;
    // Add, Move: how far it has faded out (0 whole, 255 gone; gfx::kInstanceFadeShift).
    std::uint8_t fade = 0;
    // Add, Move: 1 + its material tint (SceneView::tints); 0 none.
    std::uint8_t tint = 0;
    // Add, Move: parts left off the end of its model's (a dressed character's weapon and shield, last
    // in its parts, hidden while a skill hides them).
    std::uint8_t hiddenTail = 0;
};

struct FrameSnapshot {
    std::uint64_t number = 0; // the game frame that built it (1-based)
    std::uint32_t width = 0, height = 0; // the window's drawable size
    bool vsync = true;
    int frameLimit = 0;       // frames a second when vsync is off; 0 or less: none
    // The player's video options this frame (the renderer reads its switches: Classic or the
    // Modern rows).
    core::VideoOptions video;
    std::array<float, 4> clearColor{0.06f, 0.07f, 0.09f, 1.0f};

    CrowdView crowd;
    SceneView scene;
    std::vector<WorldCommand> worldCommands; // applied in order, every frame (even when the scene is not drawn)
    gfx::UiFrame uiUnder;     // the game UI between the scene and its overlay (bars, backdrops)
    gfx::UiFrame ui;          // the game UI, over the scene
    // Portraits to draw into their targets before the UI samples them (client/src/frame/Portraits.h).
    std::vector<gfx::PortraitDraw> portraits;
    devui::ImGuiFrame imgui;
    bool drawImgui = false;

    std::filesystem::path capture;                // save this frame as PNG (then the app exits)
    bool captureGoesOn = false;                   // ... and the app goes on (the skill bench's pictures)
    std::filesystem::path record;                 // this frame goes into that video (frame/VideoWriter.h; a new file
                                                  // starts one, none ends the one written)
    std::vector<std::int16_t> audio;              // the sound of this frame's time (stereo 16-bit, a loopback's mix)
    std::vector<rhi::BufferHandle> releaseBuffers; // destroyed once this frame is recorded
    std::vector<rhi::TextureHandle> releaseTextures;

    double gameWorkMs = 0; // the game thread's work on this frame, for the statistics
    bool burst = false;    // a burst of spawns was applied this frame (statistics)

    void Reset() {
        number = 0;
        crowd = {};
        scene.active = false;
        scene.items.clear(); // keeps the capacity: no allocation per frame
        scene.palette.clear();
        scene.terrain.clear();
        scene.worldPalettes = {};
        scene.shadows.clear();
        scene.effects.Clear();
        // Set by a screen each frame it wants them: a slot used by the game screen's character window last must not hand its
        // own camera to the next screen's overlay (the create screen's character went there after a Restart, unseen).
        scene.overlayOwnCamera = false;
        scene.probeX = scene.probeY = -1.0f;
        scene.audioCube = false;
        worldCommands.clear();
        portraits.clear();
        uiUnder.Clear();
        ui.Clear();
        imgui.Clear();
        drawImgui = false;
        capture.clear();
        captureGoesOn = false;
        record.clear();
        audio.clear();
        releaseBuffers.clear();
        releaseTextures.clear();
        gameWorkMs = 0;
        burst = false;
    }
};

// The game -> render hand-off with back-pressure: the game thread starts frame N+2 only after the
// render thread took N+1, so the pipeline stays one frame deep; a slot is reused only after the
// GPU finished the frame that rendered it (palettes in GPU memory are written in place).
class SnapshotQueue {
public:
    static constexpr unsigned kSlots = 3;
    using WaitForGpu = std::function<void(std::uint64_t gpuFrame)>;

    explicit SnapshotQueue(WaitForGpu waitForGpu);

    // Game thread: a free slot to fill (index via SlotIndex); waits while the render thread has
    // not taken the last published one. Returns nullptr after Stop().
    FrameSnapshot* BeginWrite();
    void Publish();
    // Render thread: the published snapshot, waiting for one; nullptr after Stop().
    FrameSnapshot* BeginRead();
    // Render thread: done with it; `gpuFrame` is the device frame that renders it.
    void EndRead(std::uint64_t gpuFrame);
    void Stop();
    unsigned SlotIndex(const FrameSnapshot* snapshot) const;
    // Seconds the game thread waited in BeginWrite in total (back-pressure, for statistics).
    double WriteWaitSeconds() const;

private:
    enum class State { Free, Writing, Ready, Reading };
    std::array<FrameSnapshot, kSlots> slots_;
    std::array<State, kSlots> states_{};
    std::array<std::uint64_t, kSlots> gpuFrames_{}; // the device frame that last rendered the slot
    int writing_ = -1, ready_ = -1, reading_ = -1;
    bool stopping_ = false;
    double writeWait_ = 0;
    WaitForGpu waitForGpu_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
};

} // namespace sr::client
