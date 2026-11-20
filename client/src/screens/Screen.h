#pragma once

// Screens (logo, crowd benchmark, later login, character select, the game): one is active at a
// time on the game thread. A screen updates its world and fills the frame's snapshot; it never
// touches the device's frames. Resources it no longer needs go into the
// snapshot's release lists, so frames still in flight keep them until they are drawn.

#include <atomic>
#include <bit>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "frame/FrameSnapshot.h"
#include "sr/assets/FileSystem.h"
#include "sr/core/Config.h"
#include "sr/core/Options.h"
#include "sr/core/Jobs.h"
#include "sr/gfx/AssetStore.h"
#include "sr/rhi/Rhi.h"
#include "sr/ui/Input.h"

namespace sr::client {

class GameAudio;
class UiWarmup;
class FeedbackChannel;

// The newest camera rotation, published by the game thread as input arrives and read by the
// render thread just before it draws (late latch).
class CameraLatch {
public:
    void Store(float yaw, float pitch) {
        value_.store(std::uint64_t{std::bit_cast<std::uint32_t>(yaw)} | std::uint64_t{std::bit_cast<std::uint32_t>(pitch)} << 32,
                     std::memory_order_release);
        valid_.store(true, std::memory_order_release);
    }
    // Overwrites the camera's yaw and pitch with the newest ones (if any were stored).
    void Apply(OrbitCamera& camera) const {
        if (!valid_.load(std::memory_order_acquire)) return;
        const std::uint64_t v = value_.load(std::memory_order_acquire);
        camera.yaw = std::bit_cast<float>(static_cast<std::uint32_t>(v));
        camera.pitch = std::bit_cast<float>(static_cast<std::uint32_t>(v >> 32));
    }
    void Clear() { valid_.store(false, std::memory_order_release); }

private:
    std::atomic<std::uint64_t> value_{0};
    std::atomic<bool> valid_{false};
};

struct ScreenContext {
    core::JobSystem& jobs;
    rhi::Device& device;
    const assets::FileSystem& files;
    const core::Config& config;
    CameraLatch& camera;
    gfx::AssetStore& assets; // streamed models, textures, animations
    assets::MemorySource* madeFiles = nullptr; // files made while running, read through `files` (null: none)
    GameAudio* audio = nullptr; // the game's sound (null: none); game thread only
    // What the render thread hands back (the scene's depth under the mouse: the clicks' occlusion; null: none).
    const FeedbackChannel* feedback = nullptr;
    double dt = 0;           // seconds since the last game frame (fixed with --fixed-dt)
    double time = 0;         // seconds since the start
    unsigned slot = 0;       // the snapshot slot being written: index for per-slot GPU memory
    bool toolsVisible = false;
    // Set by a screen to end the program after this frame (automated checks), with its exit code.
    bool quit = false;
    // Set by a screen each frame while it is still loading: --capture waits for it (reset every frame).
    bool holdCapture = false;
    int exitCode = 0;
    // The game UI's input this frame (the window's events, unless the developer tools hold the
    // mouse or the keyboard). A screen sets textInput while one of its text fields has the keyboard.
    const ui::Input* input = nullptr;
    bool mouseBlocked = false, keyboardBlocked = false;
    bool textInput = false;
    // The text field's rect and caret x (pixels) while textInput, for the IME's candidate list.
    ui::Rect textArea;
    float textCaret = 0.0f;
    // The mouse cursor the screen wants (the original's cursor resource ids,
    // section 7); reset to the normal arrow (0x95) every frame.
    int cursor = 0x95;
    // The player's options (sro_options.cfg; the Option window edits them): a screen sets
    // optionsChanged after changing them, the app then saves and applies them (the window mode,
    // the frame limit, the renderer's settings each frame through the snapshot).
    core::Options* options = nullptr;
    bool optionsChanged = false;
    std::function<std::string()> getClipboard;
    std::function<void(const std::string&)> setClipboard;
    // Named screen to screen data (a screen leaving its result for the next), e.g. the title's
    // session handed to character select.
    std::map<std::string, std::shared_ptr<void>> handoff;
    // The interface's textures warmed up for the run (gui/UiWarmup.h): every screen's UiTextures notes what it draws
    // there, the game screen waits for it before it shows (null: none, e.g. a tool's run).
    UiWarmup* uiWarmup = nullptr;
};

class Screen {
public:
    virtual ~Screen() = default;
    virtual const char* Name() const = 0;
    virtual void Enter(ScreenContext&) {}
    // Releases what the screen owns: GPU resources into `snapshot`'s release lists.
    virtual void Exit(ScreenContext&, FrameSnapshot& snapshot) { (void)snapshot; }
    virtual void Update(ScreenContext& context, FrameSnapshot& snapshot) = 0;

    std::string next; // set by Update to switch screens at the end of the frame
};

class ScreenMachine {
public:
    using Factory = std::function<std::unique_ptr<Screen>()>;
    void Register(const std::string& name, Factory factory) { factories_[name] = std::move(factory); }
    bool Has(const std::string& name) const { return factories_.contains(name); }

    // Runs the active screen, then switches if it (or Request) asked to.
    void Update(ScreenContext& context, FrameSnapshot& snapshot);
    void Request(const std::string& name) { requested_ = name; }
    // Ends the active screen (at shutdown).
    void Stop(ScreenContext& context, FrameSnapshot& snapshot);

    const char* Current() const { return screen_ ? screen_->Name() : ""; }
    std::uint64_t Switches() const { return switches_; }

private:
    std::map<std::string, Factory> factories_;
    std::unique_ptr<Screen> screen_;
    std::string requested_;
    std::uint64_t switches_ = 0;
};

} // namespace sr::client
