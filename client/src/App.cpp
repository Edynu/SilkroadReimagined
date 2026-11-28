#include "App.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio/GameAudio.h"
#include "bench/CrowdRenderer.h"
#include "frame/FrameRenderer.h"
#include "frame/FrameSnapshot.h"
#include "frame/FrameStats.h"
#include "frame/SceneRenderer.h"
#include "gui/UiWarmup.h"
#include "screens/AssetCheckScreen.h"
#include "screens/SceneBenchScreen.h"
#include "screens/CrowdScreen.h"
#include "screens/LogoScreen.h"
#include "screens/Screen.h"
#include "screens/ViewerScreen.h"
#include "screens/DungeonScreen.h"
#include "screens/GalleryScreen.h"
#include "screens/WorldScreen.h"
#include "play/PlayScreen.h"
#include "title/CharactersScreen.h"
#include "title/CreateScreen.h"
#include "title/TitleScreen.h"
#include "sr/assets/ExeCursors.h"
#include "sr/assets/FileSystem.h"
#include "sr/core/Config.h"
#include "sr/core/Jobs.h"
#include "sr/core/Log.h"
#include "sr/core/Options.h"
#include "sr/core/Profiler.h"
#include "sr/core/Trace.h"
#include "sr/devui/ImGuiLayer.h"
#include "sr/gfx/AssetStore.h"
#include "sr/platform/Window.h"
#include "sr/rhi/Rhi.h"

#ifdef SR_DEV_TOOLS
#include "devtools/EngineWindow.h"
#endif

namespace sr::client {

namespace {

bool ParseSize(const std::string& text, int& width, int& height) {
    return std::sscanf(text.c_str(), "%dx%d", &width, &height) == 2 && width > 0 && height > 0;
}

constexpr const char* kOptionsFile = "sro_options.cfg"; // shared with the prototype
constexpr const char* kUiWarmupFile = "sro_ui_warmup.txt"; // the interface's textures to warm up (gui/UiWarmupList.h)

} // namespace

// The game thread: window events, screens (simulation, presentation, UI),
// then one snapshot a frame for the render thread (4.4), which owns the device's frames.
int Run(const core::Config& config) {
    core::SetThreadName("game");
    core::RaiseThreadPriority();
    if (config.Has("log") && !core::OpenLogFile(config.GetString("log"))) {
        core::LogWarning("cannot write the log file {}", config.GetString("log"));
    }
    std::string error;

    core::JobSystem jobs(config.GetInt("workers", core::JobSystem::kDefaultWorkers));
    const bool renderThread = config.GetBool("render-thread", true);
    core::LogInfo("job system: {} workers{}; {}", jobs.WorkerCount(), jobs.WorkerCount() == 0 ? " (inline)" : "",
                  renderThread ? "game and render threads" : "one thread (--render-thread off)");

    assets::FileSystem files;
    {
        SR_ZONE("startup: mount archives");
        if (!assets::MountGameArchives(files, config.GetString("game"), error)) {
            core::LogError("{} (set --game or SR_GAME_DIR to the Silkroad 1.188 folder)", error);
            return 1;
        }
    }
    if (config.Has("override")) {
        files.Mount(std::make_unique<assets::DirectorySource>(config.GetString("override")),
                    assets::FileSystem::kOverridePriority);
    }
    // Files made while the game runs (the guilds' emblems as textures), under their own folder.
    auto memory = std::make_unique<assets::MemorySource>();
    assets::MemorySource* madeFiles = memory.get();
    files.Mount(std::move(memory), assets::FileSystem::kOverridePriority + 1);

    // The player's options (the original's Option window), then engine settings on top.
    core::Options options;
    core::Options::Load(kOptionsFile, options);
    // --look classic|modern-low|modern-high|modern-ultra: this run's look (not saved unless the
    // Option window keeps it).
    if (const std::string look = config.GetString("look", ""); !look.empty()) {
        if (const int q = core::VideoOptions::LookByName(look); q >= 0) {
            options.video.ApplyLook(q);
        } else {
            core::LogWarning("--look {}: classic, modern-low, modern-high or modern-ultra", look);
        }
    }
    // --video key=value,...: our rows one by one after the look (the keys of sro_options.cfg's
    // video.<key>, e.g. sunShadows=2,viewDistance=0).
    const std::string videoRows = config.GetString("video", "");
    for (std::string_view rest = videoRows; !rest.empty();) {
        const std::size_t comma = rest.find(',');
        const std::string_view pair = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        const std::size_t eq = pair.find('=');
        bool known = false;
        for (const core::VideoOptions::ExtraRow& row : core::VideoOptions::ExtraRows()) {
            if (eq == std::string_view::npos || pair.substr(0, eq) != row.key) continue;
            options.video.*row.field = std::clamp(std::atoi(std::string(pair.substr(eq + 1)).c_str()), 0, row.max);
            known = true;
        }
        // The original's rows by their slot (slotN=V, 0..15) on the active record, e.g. slot1=0 (no shadows), slot9=0 (low textures).
        if (!known && eq != std::string_view::npos && pair.starts_with("slot")) {
            const int slot = std::atoi(std::string(pair.substr(4, eq - 4)).c_str());
            if (slot >= 0 && slot < core::VideoOptions::kSlots) {
                options.video.records[std::size_t(options.video.active)][std::size_t(slot)] = std::atoi(std::string(pair.substr(eq + 1)).c_str());
                known = true;
            }
        }
        if (!known) core::LogWarning("--video {}: not one of the video rows", pair);
    }
    // --game-rows key=0|1,...: the Game page's rows for this run (the keys of sro_options.cfg's game.<key>, e.g. guide=1); kept
    // only when the Option window saves.
    const std::string gameRows = config.GetString("game-rows", "");
    for (std::string_view rest = gameRows; !rest.empty();) {
        const std::size_t comma = rest.find(',');
        const std::string_view pair = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        if (const std::size_t eq = pair.find('='); eq != std::string_view::npos) {
            options.game.Set(pair.substr(0, eq), pair.substr(eq + 1) != "0");
        } else {
            core::LogWarning("--game-rows {}: key=0 or key=1", pair);
        }
    }
    int frameLimit = config.GetInt("frame-limit", options.video.frameLimit); // 0: display rate, -1: none
    const bool vsyncAllowed = config.GetBool("vsync", true);

    // The sound (O3): the device, the player's levels, the tables on a job.
    GameAudio audio(jobs, files);
    std::string audioName;
    const bool audioOn = config.GetBool("audio", true);
    // The skill bench's videos take their sound from a loopback device: the mix of each frame's time (48 kHz).
    const int loopbackRate = config.Has("bench-video") ? 48'000 : 0;
    // Their levels are their own, not the player's (whose mutes would leave them silent): the skills' sounds whole, the
    // ambience at the default, no music over them.
    core::AudioOptions levels = options.audio;
    if (loopbackRate > 0) levels = core::AudioOptions{0, 100, 50, true, false, false};
    if (audioOn && audio.Open(levels, error, loopbackRate)) {
        audioName = audio.engine().device().DeviceName() + " (" + audio.engine().device().Implementation() + ")";
        core::LogInfo("audio: {}", audioName);
    } else if (!audioOn) {
        audioName = "off (--audio false)";
    } else {
        audioName = "none: " + error;
        core::LogWarning("audio: {}", error); // the game runs without sound
    }

    int width = 1600, height = 900;
    if (config.Has("size") && !ParseSize(config.GetString("size"), width, height)) {
        core::LogError("--size expects WxH, for example 1600x900");
        return 2;
    }
    platform::Window window;
    if (!window.Create("SroReimagined", width, height, error)) {
        core::LogError("{}", error);
        return 1;
    }
    // --window-mode 0..2 (windowed, borderless, fullscreen) for this run; otherwise the option's.
    int windowMode = std::clamp(config.GetInt("window-mode", options.video.windowMode), 0, 2);
    if (windowMode != 0) window.SetMode(static_cast<platform::Window::Mode>(windowMode));
    // The original's cursors, from its own executable (read for its resources, never run): the game
    // folder's, or --cursors <exe>; without one the system's cursors stay. Read on a job; the window
    // takes them on this thread once they are in.
    struct CursorLoad {
        std::atomic<bool> ready{false};
        std::vector<assets::CursorImage> images;
        std::string source; // where they came from (the log's line)
    };
    auto cursors = std::make_shared<CursorLoad>();
    bool cursorsApplied = false;
    core::JobCounter cursorsJob;
    jobs.Run(
        [cursors, exe = config.Has("cursors") ? std::filesystem::path(config.GetString("cursors"))
                                              : std::filesystem::path(config.GetString("game")) / "sro_client.exe",
         dir = std::filesystem::path(config.GetString("cursor-dir", "client/assets/cursors"))] {
            // Kept beside the code after the first start (git-ignored), else read from the executable and kept there.
            if (LoadCursorFolder(dir, cursors->images)) {
                cursors->source = dir.string();
                cursors->ready.store(true, std::memory_order_release);
                return;
            }
            std::vector<std::byte> bytes;
            if (std::FILE* f = std::fopen(exe.string().c_str(), "rb")) {
                std::error_code ec;
                bytes.resize(static_cast<std::size_t>(std::filesystem::file_size(exe, ec)));
                bytes.resize(std::fread(bytes.data(), 1, bytes.size(), f));
                std::fclose(f);
            }
            std::string cursorError;
            if (bytes.empty() || !assets::DecodeExeCursors(bytes, cursors->images, cursorError)) {
                core::LogWarning("cursors: {}", bytes.empty() ? exe.string() + " not found (the system's cursors stay; --cursors <sro_client.exe>)" : cursorError);
            } else {
                cursors->source = "sro_client.exe";
                if (SaveCursorFolder(dir, cursors->images)) cursors->source += ", kept in " + dir.string();
            }
            cursors->ready.store(true, std::memory_order_release);
        },
        core::JobPriority::Streaming, &cursorsJob);

    rhi::DeviceDesc deviceDesc;
    const auto native = window.Native();
    deviceDesc.window = {native.hinstance, native.hwnd};
    deviceDesc.width = window.PixelWidth();
    deviceDesc.height = window.PixelHeight();
    deviceDesc.vsync = vsyncAllowed && frameLimit == 0;
    deviceDesc.validation = config.GetBool("validation", false) || config.GetBool("sync-validation", false) || config.GetBool("gpu-validation", false);
    deviceDesc.syncValidation = config.GetBool("sync-validation", false);
    deviceDesc.gpuValidation = config.GetBool("gpu-validation", false);
    // Streaming memory, reserved now.
    deviceDesc.stagingBytes = static_cast<std::size_t>(std::max(8, config.GetInt("staging-mib", 64))) << 20;
    deviceDesc.textureReserveBytes = static_cast<std::size_t>(std::max(0, config.GetInt("texture-reserve-mib", 1024))) << 20;
    std::unique_ptr<rhi::Device> device = rhi::CreateVulkanDevice(deviceDesc, error);
    if (!device) {
        core::LogError("{}", error);
        return 1;
    }
    core::LogInfo("GPU: {}", device->AdapterName());

    devui::ImGuiLayer imgui;
    if (!imgui.Init(*device, window, error)) {
        core::LogError("{}", error);
        return 1;
    }
    const std::filesystem::path capturePath = config.GetString("capture");
    const int captureFrame = config.GetInt("capture-frame", 10);
    // A screen that is still loading holds the capture back, at most this many frames longer.
    const int captureWait = config.GetInt("capture-wait", 3000);
    bool captureSent = false;
    // A fixed layout for automated captures; otherwise the tools remember their windows (a player's build has none).
#ifdef SR_PLAYER_BUILD
    ImGui::GetIO().IniFilename = nullptr;
#else
    ImGui::GetIO().IniFilename = capturePath.empty() ? "sr_client.ini" : nullptr;
#endif

    CrowdRenderer crowdRenderer;
    if (!crowdRenderer.Init(*device, error)) {
        core::LogError("{}", error);
        return 1;
    }

    // Models, textures and animations stream through the asset store.
    gfx::AssetStore assets(*device, files, jobs);
    gfx::AssetStoreDesc assetDesc;
    assetDesc.geometryBytes = static_cast<std::size_t>(std::max(16, config.GetInt("geometry-mib", 512))) << 20;
    assetDesc.textureBudgetBytes = std::max<std::uint64_t>(device->GetStreamingInfo().textureReserve, 64ull << 20); // the reserve, no more
    if (!assets.Init(assetDesc, error)) {
        core::LogError("{}", error);
        return 1;
    }
    // The interface's textures the last runs drew, asked for now (gui/UiWarmup.h): in by the time the player is in the
    // game. --ui-warmup false: none (each picture loads when first shown). A run that only checks something (a capture,
    // a benchmark) reads the list but does not write it: what it drew is not what the player uses.
    std::optional<UiWarmup> uiWarmup;
    // --ui-warmup-save true|false: write it anyway, or never.
    const bool warmupSaves = config.GetBool("ui-warmup-save", capturePath.empty() && config.GetDouble("run-seconds", 0.0) <= 0.0 &&
                                                                  config.GetInt("screen-cycle", 0) <= 0 && !config.Has("asset-check"));
    if (config.GetBool("ui-warmup", true)) {
        uiWarmup.emplace(assets);
        uiWarmup->Load(kUiWarmupFile);
    }
    SceneRenderer sceneRenderer;
    // The frame's skinning arena (32 bytes a vertex): characters whose parts no longer fit are not
    // drawn that frame; thousands of dressed players on screen need more than the default.
    const std::uint64_t skinMib = static_cast<std::uint64_t>(std::clamp(config.GetInt("skin-arena-mib", 128), 16, 4096));
    if (!sceneRenderer.Init(*device, assets, static_cast<std::uint32_t>((skinMib << 20) / 32), error)) {
        core::LogError("{}", error);
        return 1;
    }

    gfx::UiPass uiPass; // the game UI (C3h)
    if (!uiPass.Init(*device, error)) {
        core::LogError("{}", error);
        return 1;
    }
    gfx::PortraitPass portraitPass; // the HUD's portraits
    if (!portraitPass.Init(*device, error)) {
        core::LogError("{}", error);
        return 1;
    }

    CameraLatch cameraLatch;
    FeedbackChannel feedback;
    FrameRecorder recorder;
    SnapshotQueue queue([&device](std::uint64_t frame) { device->WaitForFrame(frame); });
    FrameRenderer renderer(*device, imgui, crowdRenderer, sceneRenderer, uiPass, portraitPass, assets, jobs, cameraLatch, feedback, recorder);
    // Videos (the skill bench's --bench-video): the encoder, the frames' rate (the game's with --fixed-dt), the widest.
    renderer.SetVideo(config.GetString("ffmpeg", "ffmpeg"), std::clamp(config.GetInt("video-fps", 30), 1, 120),
                      std::uint32_t(std::max(0, config.GetInt("video-width", 1280))), audio.Loopback() ? loopbackRate : 0);

    std::thread render;
    if (renderThread) {
        render = std::thread([&] {
            core::SetThreadName("render");
            core::RaiseThreadPriority();
            while (const FrameSnapshot* snapshot = queue.BeginRead()) {
                renderer.Render(*snapshot);
                queue.EndRead(device->SubmittedFrame());
            }
        });
    }

    ScreenMachine screens;
    screens.Register("logo", [] { return std::make_unique<LogoScreen>(); });
    screens.Register("crowd", [] { return std::make_unique<CrowdScreen>(); });
    screens.Register("empty", [] { return std::make_unique<EmptyScreen>(); });
    screens.Register("viewer", [] { return std::make_unique<ViewerScreen>(); });
    screens.Register("assetcheck", [] { return std::make_unique<AssetCheckScreen>(); });
    screens.Register("scenebench", [] { return std::make_unique<SceneBenchScreen>(); });
    screens.Register("world", [] { return std::make_unique<WorldScreen>(); });
    screens.Register("dungeon", [] { return std::make_unique<DungeonScreen>(); });
    screens.Register("title", [] { return std::make_unique<TitleScreen>(); });
    screens.Register("characters", [] { return std::make_unique<CharactersScreen>(); });
    screens.Register("create", [] { return std::make_unique<CreateScreen>(); });
    screens.Register("game", [] { return std::make_unique<PlayScreen>(); });
    screens.Register("gallery", [] { return std::make_unique<GalleryScreen>(); });
    const std::vector<std::string> cycle = {"crowd", "empty", "logo"};

#ifdef SR_DEV_TOOLS
    bool toolsVisible = config.GetBool("tools", capturePath.empty());
#else
    // A build without the developer tools (the players'): no ImGui window at all, the screens' panels included, and F1
    // does nothing.
    bool toolsVisible = false;
#endif
#ifdef SR_DEV_TOOLS
    EngineWindow engineWindow({device->AdapterName(), audioName, &config, &files, &assets});
#endif
    ScreenContext context{jobs, *device, files, config, cameraLatch, assets};
    context.madeFiles = madeFiles;
    context.feedback = &feedback;
    if (audio.IsOpen()) context.audio = &audio;
    // The game UI's input: the window's events, translated.
    ui::Input uiInput;
    bool textInputOn = false;
    ui::Rect textArea{-1, -1, 0, 0};
    float textCaret = 0.0f;
    context.input = &uiInput;
    context.options = &options;
    if (uiWarmup) context.uiWarmup = &*uiWarmup;
    context.getClipboard = [&window] { return window.ClipboardText(); };
    context.setClipboard = [&window](const std::string& text) { window.SetClipboardText(text); };
    screens.Request(config.Has("asset-check") ? std::string("assetcheck") : config.GetString("screen", "logo"));

    // Automated runs: --run-seconds (benchmark summary), --screen-cycle (leak check).
    const double runSeconds = config.GetDouble("run-seconds", 0.0);
    const int screenCycles = config.GetInt("screen-cycle", 0);
    const double fixedDt = config.GetDouble("fixed-dt", 0.0);
    rhi::Device::ResourceCounts cycleBaseline{};
    int cyclesDone = 0;
    // --window-test: resize, borderless, windowed, minimise, restore; rendering must go on.
    const bool windowTest = config.GetBool("window-test", false);
    int windowStep = 0;
    std::uint64_t framesAtRestore = 0;

    int exitCode = 0;
    const auto startTime = std::chrono::steady_clock::now();
    double lastTime = 0.0, gameTime = 0.0;
    double loopbackDue = 0.0; // a loopback's frames of sound owed (the frames' times at its rate)
    float displayRate = 0.0f; // the window's display's refresh rate (pacing a window, below)
    for (std::uint64_t frame = 1;; ++frame) {
        core::Profiler::Get().NextFrame();
        bool quit = false;
        uiInput.BeginFrame(window.Time());
        if (!window.PumpEvents([&](const void* event) {
                imgui.ProcessEvent(event);
                ui::InputEvent translated;
                if (window.Translate(event, translated)) uiInput.Feed(translated);
            })) {
            quit = true;
        }
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
        // A video's frame that was not drawn (a minimised window) holds the game's clock: the same moment again.
        const double dt = renderer.VideoHeld() ? 0.0 : fixedDt > 0.0 ? fixedDt : std::clamp(now - lastTime, 0.0, 0.25);
        lastTime = now;
        gameTime += dt;
        if (renderer.CaptureDone() || quit || (runSeconds > 0.0 && now >= runSeconds)) break;
        if (context.quit) {
            if (exitCode == 0) exitCode = context.exitCode;
            break;
        }
        if (renderer.CaptureFailed()) exitCode = 1;

        FrameSnapshot* snapshot = queue.BeginWrite();
        if (snapshot == nullptr) break;
        const auto workStart = std::chrono::steady_clock::now();
        SR_ZONE("game: frame");

        // The developer tools change the cursor only while the mouse is theirs.
        if (toolsVisible && ImGui::GetIO().WantCaptureMouse) {
            ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        } else {
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        }
        imgui.NewFrame();
        const ImGuiIO& io = ImGui::GetIO();
        // F1 and F5 are the developer tools' (through ImGui); Alt+Enter is the game's own (SDL's events).
#ifdef SR_DEV_TOOLS
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) toolsVisible = !toolsVisible;
#endif
        if (uiInput.KeyPressed(ui::Key::Enter) && uiInput.KeyDown(ui::Key::Alt)) {
            // Alt+Enter: windowed <-> borderless, as the original's hot key.
            windowMode = windowMode == 0 ? 1 : 0;
            if (!window.SetMode(static_cast<platform::Window::Mode>(windowMode))) core::LogWarning("{}", window.LastError());
        }
#ifdef SR_DEV_TOOLS
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
            const auto it = std::find(cycle.begin(), cycle.end(), std::string(screens.Current()));
            screens.Request(it == cycle.end() || it + 1 == cycle.end() ? cycle.front() : *(it + 1));
        }
#endif
        if (screenCycles > 0 && frame % 10 == 0) {
            // The leak check: crowd -> empty -> logo -> crowd ..., comparing live resources at
            // the start of each round (once frames in flight have let go of the last round).
            if (cyclesDone == 1) cycleBaseline = device->GetResourceCounts();
            if (cyclesDone >= screenCycles * 3 + 1) {
                const auto counts = device->GetResourceCounts();
                core::LogInfo("screen cycle: {} switches; live at the start {} textures {} buffers {} pipelines; at the end {} {} {} ({} waiting for the GPU)",
                              screens.Switches(), cycleBaseline.textures, cycleBaseline.buffers, cycleBaseline.pipelines,
                              counts.textures, counts.buffers, counts.pipelines, counts.pendingDeletions);
                if (counts.textures != cycleBaseline.textures || counts.buffers != cycleBaseline.buffers ||
                    counts.pipelines != cycleBaseline.pipelines) {
                    core::LogError("screen cycle: resources leaked");
                    exitCode = 3;
                }
                break;
            }
            screens.Request(cycle[static_cast<std::size_t>(cyclesDone) % cycle.size()]);
            ++cyclesDone;
        }

        if (windowTest) {
            const double t = now;
            const auto rendered = feedback.Get().frames;
            if (windowStep == 0 && t > 0.5) { window.Resize(1280, 720); ++windowStep; }
            else if (windowStep == 1 && t > 1.0) { window.SetMode(platform::Window::Mode::Borderless); ++windowStep; }
            else if (windowStep == 2 && t > 1.5) { window.SetMode(platform::Window::Mode::Windowed); ++windowStep; }
            else if (windowStep == 3 && t > 2.0) { window.Resize(1600, 900); ++windowStep; }
            else if (windowStep == 4 && t > 2.5) { window.Minimize(); ++windowStep; }
            else if (windowStep == 5 && t > 3.5) { window.Restore(); framesAtRestore = rendered; ++windowStep; }
            else if (windowStep == 6 && t > 4.5) {
                const bool ok = rendered > framesAtRestore + 10;
                core::LogInfo("window test: {} frames rendered since the restore, {}x{} now: {}", rendered - framesAtRestore,
                              window.PixelWidth(), window.PixelHeight(), ok ? "passed" : "FAILED");
                if (!ok) exitCode = 4;
                break;
            }
        }

        context.dt = dt;
        context.time = gameTime;
        context.slot = queue.SlotIndex(snapshot);
        context.toolsVisible = toolsVisible;
        context.holdCapture = false;
        context.mouseBlocked = toolsVisible && io.WantCaptureMouse;
        context.keyboardBlocked = toolsVisible && io.WantCaptureKeyboard;
        context.textInput = false;
        context.cursor = 0x95;
        {
            SR_PROFILE("game: screen");
            screens.Update(context, *snapshot);
        }
        if (uiWarmup) uiWarmup->ListReady(); // follows its loads: each lets go of what is in (logged as it happens)
        audio.Update(dt);
        // A loopback's mix of this frame's time, carried with its frame (the frames held keep no time: no sound).
        if (audio.Loopback()) {
            loopbackDue += dt * double(loopbackRate);
            const auto frames = static_cast<std::size_t>(loopbackDue);
            loopbackDue -= double(frames);
            snapshot->audio.resize(frames * 2);
            audio.Render(snapshot->audio);
        }
        if (!cursorsApplied && cursors->ready.load(std::memory_order_acquire)) {
            for (const assets::CursorImage& c : cursors->images) window.AddCursor(c.id, c.rgba.data(), c.width, c.height, c.hotX, c.hotY);
            if (!cursors->images.empty()) core::LogInfo("cursors: {} from {}", cursors->images.size(), cursors->source);
            cursorsApplied = true;
        }
        if (context.warpMouse) {
            context.warpMouse = false;
            window.WarpMouse(context.warpTo.x, context.warpTo.y);
        }
        window.HideCursor(context.hideCursor);
        context.hideCursor = false;
        if (cursorsApplied) {
            if (toolsVisible && io.WantCaptureMouse) {
                window.ForgetCursor();
            } else {
                window.UseCursor(context.cursor);
            }
        }
        if (context.textInput != textInputOn) {
            // Typed characters (and the IME) only while a text field has the keyboard.
            window.SetTextInput(context.textInput);
            textInputOn = context.textInput;
        }
        if (context.textInput && (context.textArea.x != textArea.x || context.textArea.y != textArea.y || context.textCaret != textCaret)) {
            textArea = context.textArea;
            textCaret = context.textCaret;
            window.SetTextInputArea(textArea.x, textArea.y, textArea.w, textArea.h, textCaret);
        }

#ifdef SR_DEV_TOOLS
        engineWindow.AddFrameTime(dt);
        if (toolsVisible) engineWindow.Draw(jobs, feedback.Get(), screens.Current(), renderThread);
#endif
        snapshot->number = frame;
        snapshot->width = window.PixelWidth();
        snapshot->height = window.PixelHeight();
        // The Option window kept new options: saved, the window mode and the frame limit applied
        // (the renderer reads the video rows from the snapshot every frame).
        if (context.optionsChanged) {
            context.optionsChanged = false;
            if (!options.Save(kOptionsFile)) core::LogWarning("options: cannot write {}", kOptionsFile);
            frameLimit = options.video.frameLimit;
            if (audio.IsOpen()) audio.SetLevels(options.audio);
            if (const int mode = std::clamp(options.video.windowMode, 0, 2); mode != windowMode) {
                windowMode = mode;
                if (!window.SetMode(static_cast<platform::Window::Mode>(windowMode))) core::LogWarning("{}", window.LastError());
            }
        }
        snapshot->video = options.video;
        snapshot->vsync = vsyncAllowed && frameLimit == 0;
        snapshot->frameLimit = frameLimit;
        if (snapshot->vsync && windowMode == 0) {
            // In a window the desktop's compositor shows our frames at the display's rate anyway
            // (no tearing); its FIFO queue missed one vblank in eight there (165 Hz, AMD). Presented
            // at once and paced by our limiter at the display's rate it misses one in twenty-five;
            // borderless gets the display to itself and one in a hundred.
            if (frame % 60 == 1) displayRate = window.RefreshRate();
            if (displayRate >= 30.0f) {
                snapshot->vsync = false;
                snapshot->frameLimit = int(std::lround(displayRate));
            }
        }
        snapshot->drawImgui = snapshot->drawImgui || toolsVisible;
        if (!capturePath.empty() && !captureSent && frame >= static_cast<std::uint64_t>(captureFrame)) {
            const bool late = frame >= static_cast<std::uint64_t>(captureFrame + captureWait);
            if (!context.holdCapture || late) {
                if (context.holdCapture) core::LogWarning("capture: the screen is still loading after {} frames", frame);
                snapshot->capture = capturePath;
                captureSent = true;
            }
        }
        {
            SR_PROFILE("game: ui copy");
            imgui.EndFrame(snapshot->imgui);
        }
        snapshot->gameWorkMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - workStart).count();
        queue.Publish();
        SR_FRAME_MARK_NAMED("game");

        if (!renderThread) {
            // --render-thread off: the same render frame, on this thread, through the same snapshot.
            const FrameSnapshot* published = queue.BeginRead();
            renderer.Render(*published);
            queue.EndRead(device->SubmittedFrame());
        }
    }

    // Shut down: the render thread first, then what the screens own.
    queue.Stop();
    if (render.joinable()) render.join();
    renderer.FinishVideo();
    device->WaitIdle();
    {
        FrameSnapshot last;
        screens.Stop(context, last);
        for (rhi::BufferHandle b : last.releaseBuffers) device->DestroyBuffer(b);
        for (rhi::TextureHandle t : last.releaseTextures) device->DestroyTexture(t);
    }
    jobs.Wait(cursorsJob);
    audio.Shutdown();
    if (uiWarmup) {
        // What this run drew, for the next (the screens have noted everything by now), then the holds let go.
        if (warmupSaves && !uiWarmup->Save(kUiWarmupFile)) core::LogWarning("ui warm-up: cannot write {}", kUiWarmupFile);
        uiWarmup->ReleaseAll();
    }
    assets.Shutdown(); // waits for its jobs; the GPU side goes with the device's deferred deletions
    if (renderer.CaptureFailed() || renderer.VideoFailed()) exitCode = 1;

    if (runSeconds > 0.0 || config.Has("asset-check")) {
        const std::string summary = recorder.Summary();
        core::LogInfo("benchmark ({} workers, {}):\n{}\ngame thread waited for a free snapshot {:.2f} s in total", jobs.WorkerCount(),
                      renderThread ? "render thread" : "one thread", summary, queue.WriteWaitSeconds());
        core::LogInfo("{}", renderer.GpuSummary());
        if (config.Has("bench-out")) {
            std::ofstream out(config.GetString("bench-out"));
            out << summary << "\n";
        }
    }

    device->WaitIdle();
    uiPass.Shutdown();
    portraitPass.Shutdown();
    sceneRenderer.Shutdown();
    crowdRenderer.Shutdown();
    imgui.Shutdown();
    if (deviceDesc.validation) {
        core::LogInfo("validation messages: {}", device->ValidationMessageCount());
        if (device->ValidationMessageCount() > 0 && exitCode == 0) exitCode = 2;
    }
    return exitCode;
}

} // namespace sr::client
