#include "frame/FrameRenderer.h"

#include <thread>
#include <vector>

#include "App.h"
#include "sr/core/Log.h"
#include "sr/core/Trace.h"

namespace sr::client {

namespace {
double Ms(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}
} // namespace

FrameRenderer::FrameRenderer(rhi::Device& device, devui::ImGuiLayer& imgui, CrowdRenderer& crowd, SceneRenderer& scene, gfx::UiPass& ui,
                             gfx::PortraitPass& portraits, gfx::AssetStore& assets, core::JobSystem& jobs, const CameraLatch& camera,
                             FeedbackChannel& feedback, FrameRecorder& recorder)
    : device_(device), imgui_(imgui), crowd_(crowd), scene_(scene), ui_(ui), portraits_(portraits), assets_(assets), jobs_(jobs),
      camera_(camera), feedback_(feedback), recorder_(recorder), graph_(&device) {}

void FrameRenderer::Release(const FrameSnapshot& snapshot) {
    // The device defers each until every frame that may use it is done.
    for (rhi::BufferHandle b : snapshot.releaseBuffers) device_.DestroyBuffer(b);
    for (rhi::TextureHandle t : snapshot.releaseTextures) device_.DestroyTexture(t);
}

void FrameRenderer::Render(const FrameSnapshot& snapshot) {
    SR_ZONE("render: frame");
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();

    // Window and presentation requests, in the order the game made them.
    if (snapshot.width != width_ || snapshot.height != height_) {
        width_ = snapshot.width;
        height_ = snapshot.height;
        device_.Resize(width_, height_);
    }
    device_.SetVsync(snapshot.vsync);
    pacer_.SetLimit(snapshot.vsync ? 0 : snapshot.frameLimit);
    // Texture filtering: the original's row (8) on, at our row's level (waits for the GPU only when
    // it changes).
    device_.SetAnisotropy(snapshot.video.current()[8] != 0 ? snapshot.video.AnisotropyLevel() : 1.0f);

    rhi::CommandList* cmd = nullptr;
    {
        SR_ZONE("render: wait for GPU");
        cmd = device_.BeginFrame();
    }
    const auto begun = Clock::now();
    if (cmd == nullptr) {
        // Minimised: nothing to draw into. Still release, and do not spin; the portraits asked for go
        // with the next frame drawn (the interface shows them from now on).
        portraits_.Keep(snapshot.portraits);
        Release(snapshot);
        videoHeld_.store(!snapshot.record.empty(), std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return;
    }

    // Streaming: finished uploads became visible in BeginFrame; the store promotes, evicts and
    // starts new decode jobs.
    const auto streamingStart = Clock::now();
    assets_.Update();
    const double streamingMs = Ms(Clock::now() - streamingStart);

    // The frame as a graph: each view adds its passes, the swapchain pass reads
    // their results; the graph orders them and places the barriers.
    scene_.ApplyWorldCommands(snapshot.worldCommands); // every snapshot's, drawn or not
    {
        SR_ZONE("render: graph");
        graph_.Reset();
        const std::uint32_t width = device_.SwapchainWidth(), height = device_.SwapchainHeight();
        // Late latch: the newest rotation, not the one the snapshot was built with.
        OrbitCamera crowdCamera = snapshot.crowd.camera, sceneCamera = snapshot.scene.camera;
        camera_.Apply(crowdCamera);
        camera_.Apply(sceneCamera);
        const auto crowd = snapshot.crowd.active ? crowd_.AddPasses(graph_, snapshot.crowd, crowdCamera, jobs_, width, height) : 0;
        scene_.SetVideo(snapshot.video);
        const auto scene = snapshot.scene.active ? scene_.AddPasses(graph_, snapshot.scene, sceneCamera, width, height) : 0;
        // The portraits asked for this frame, each into its own target (the UI shows them).
        const auto portraits = portraits_.AddPasses(graph_, snapshot.portraits, assets_);
        graph_.AddPass("swapchain", gfx::PassKind::Graphics,
                       [&](gfx::RenderGraph::Builder& b) {
                           b.Read(crowd, gfx::Use::Sampled).Read(scene, gfx::Use::Sampled).SideEffect();
                           for (const gfx::RenderGraph::Resource portrait : portraits) b.Read(portrait, gfx::Use::Sampled);
                           if (scene != 0 && scene_.OverlayResult() != 0) b.Read(scene_.OverlayResult(), gfx::Use::Sampled);
                       },
                       [&](rhi::CommandList& list, const gfx::RenderGraph&) {
                           list.BeginSwapchainPass(snapshot.clearColor.data());
                           if (crowd != 0) crowd_.Composite(list);
                           if (scene != 0) scene_.Composite(list);
                           // The UI under the scene's overlay, then the overlay (character windows).
                           ui_.SetSharp(snapshot.video.sharpInterface != 0);
                           ui_.Draw(list, snapshot.uiUnder, static_cast<float>(width), static_cast<float>(height));
                           // An overlay with a camera of its own is drawn by the window that shows it.
                           const bool overlayInUi = scene != 0 && snapshot.scene.overlayOwnCamera;
                           if (scene != 0 && !overlayInUi) scene_.CompositeOverlay(list);
                           // The game UI over the scene (C3h), the developer tools over everything.
                           ui_.Draw(list, snapshot.ui, static_cast<float>(width), static_cast<float>(height),
                                    overlayInUi ? scene_.TakeOverlayTexture() : 0u, scene != 0 ? scene_.DepthTexture() : 0u);
                           if (snapshot.drawImgui) imgui_.Draw(list, snapshot.imgui);
                           list.EndPass();
                       });
        graph_.Compile();
        graph_.Run(*cmd);
    }

    std::vector<std::uint8_t> pixels;
    const bool capture = !snapshot.capture.empty(), record = !snapshot.record.empty();
    {
        SR_ZONE("render: submit + present");
        device_.EndFrame(capture || record ? &pixels : nullptr);
    }
    const auto submitted = Clock::now();
    SR_FRAME_MARK();
    Release(snapshot);

    RenderFeedback feedback;
    feedback.device = device_.LastFrameStats();
    feedback.resources = device_.GetResourceCounts();
    feedback.gpuWaitMs = Ms(begun - start);
    feedback.renderWorkMs = Ms(submitted - begun); // recording + submit + present, without the GPU wait
    feedback.streamingMs = streamingMs;
    feedback.streaming = device_.GetStreamingInfo();
    feedback.memory = device_.GetMemoryBudget();
    feedback.frames = ++frames_;
    feedback.gpuTimers = device_.LastGpuTimers();
    if (!feedback.gpuTimers.empty()) {
        ++gpuFrames_;
        for (const rhi::Device::GpuTimer& t : feedback.gpuTimers) {
            auto it = std::find_if(gpuTotals_.begin(), gpuTotals_.end(), [&](const auto& e) { return e.first == t.name; });
            if (it == gpuTotals_.end()) it = gpuTotals_.insert(gpuTotals_.end(), {t.name, 0.0});
            it->second += t.ms;
        }
    }
    feedback_.Put(feedback);
    feedback_.PutProbe(scene_.Probe()); // the scene's depth under the mouse (the clicks' occlusion)
    if (std::shared_ptr<const AudioCube> cube = scene_.TakeAudioCube()) feedback_.PutAudioCube(std::move(cube));

    pacer_.Wait();
    const auto presented = Clock::now();
    if (lastPresent_ != Clock::time_point{}) {
        recorder_.Add({Ms(presented - lastPresent_), feedback.renderWorkMs, snapshot.gameWorkMs, snapshot.burst, streamingMs});
    }
    lastPresent_ = presented;

    if (capture) {
        const int w = static_cast<int>(device_.SwapchainWidth()), h = static_cast<int>(device_.SwapchainHeight());
        bool ok = false;
        if (!pixels.empty() && snapshot.capture.extension() == ".jpg") {
            // A JPEG goes in under another name and is then renamed: whoever waits for it never reads it half written.
            std::filesystem::path part = snapshot.capture;
            part += ".part";
            std::error_code ec;
            ok = SaveJpeg(part, w, h, pixels.data(), 85);
            if (ok) std::filesystem::rename(part, snapshot.capture, ec);
            ok = ok && !ec;
        } else if (!pixels.empty()) {
            ok = SavePng(snapshot.capture, w, h, pixels.data());
        }
        if (ok) {
            core::LogInfo("captured {}", snapshot.capture.string());
        } else {
            core::LogError("could not capture the frame");
            captureFailed_.store(true, std::memory_order_release);
        }
        if (!snapshot.captureGoesOn) captureDone_.store(true, std::memory_order_release);
    }
    // A video's frame: a new file (or size) closes the one before and starts it. One not read back holds the game.
    videoHeld_.store(record && pixels.empty(), std::memory_order_release);
    const std::uint32_t w = device_.SwapchainWidth(), h = device_.SwapchainHeight();
    if (record && !pixels.empty()) {
        if (video_.IsOpen() && (video_.File() != snapshot.record || video_.Width() != w || video_.Height() != h)) FinishVideo();
        if (!video_.IsOpen()) {
            std::string error;
            if (!video_.Open(snapshot.record, ffmpeg_, w, h, videoFps_, videoMaxWidth_, error, videoAudioRate_)) {
                core::LogError("video: {}", error);
                videoFailed_.store(true, std::memory_order_release);
            }
        }
        if (video_.IsOpen()) video_.AddAudio(snapshot.audio);
        if (video_.IsOpen() && !video_.Write(pixels)) {
            core::LogError("video: the encoder took no more frames ({})", snapshot.record.string());
            videoFailed_.store(true, std::memory_order_release);
            video_.Close();
        }
    } else if (!record && video_.IsOpen()) {
        FinishVideo();
    }
}

void FrameRenderer::FinishVideo() {
    if (!video_.IsOpen()) return;
    const std::uint64_t frames = video_.Frames();
    const std::filesystem::path file = video_.File();
    if (video_.Close()) {
        core::LogInfo("video: {} ({} frames)", file.string(), frames);
    } else {
        videoFailed_.store(true, std::memory_order_release);
    }
}

std::string FrameRenderer::GpuSummary() const {
    if (gpuFrames_ == 0) return "no GPU timers";
    auto sorted = gpuTotals_;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string out = "GPU ms a frame by pass:";
    for (const auto& [name, total] : sorted) out += std::format("\n  {:7.3f}  {}", total / double(gpuFrames_), name);
    return out;
}

} // namespace sr::client
