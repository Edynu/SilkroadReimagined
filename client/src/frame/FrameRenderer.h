#pragma once

// The render thread's frame: turns one snapshot into one presented frame.
// Owns everything about the device's frames: resizes, vsync, pacing, captures and the releases
// the snapshot carries. With `--render-thread off` the game thread calls it directly, through the
// same snapshot, so both modes run the same code.

#include <atomic>
#include <chrono>
#include <filesystem>

#include "bench/CrowdRenderer.h"
#include "frame/FrameSnapshot.h"
#include "frame/FrameStats.h"
#include "frame/VideoWriter.h"
#include "frame/SceneRenderer.h"
#include "screens/Screen.h"
#include "sr/gfx/AssetStore.h"
#include "sr/gfx/PortraitPass.h"
#include "sr/gfx/RenderGraph.h"
#include "sr/core/FramePacer.h"
#include "sr/core/Jobs.h"
#include "sr/devui/ImGuiLayer.h"
#include "sr/rhi/Rhi.h"

namespace sr::client {

class FrameRenderer {
public:
    FrameRenderer(rhi::Device& device, devui::ImGuiLayer& imgui, CrowdRenderer& crowd, SceneRenderer& scene, gfx::UiPass& ui,
                  gfx::PortraitPass& portraits, gfx::AssetStore& assets, core::JobSystem& jobs, const CameraLatch& camera,
                  FeedbackChannel& feedback, FrameRecorder& recorder);

    void Render(const FrameSnapshot& snapshot);

    // A requested capture was written (or failed): the app may exit.
    bool CaptureDone() const { return captureDone_.load(std::memory_order_acquire); }
    bool CaptureFailed() const { return captureFailed_.load(std::memory_order_acquire); }
    // The passes' GPU time over the run, averaged per frame, most expensive first (after the
    // render thread stopped).
    std::string GpuSummary() const;
    // The videos' encoder (a path or a name on PATH), their rate and their widest (FrameSnapshot::record).
    // `audioRate` > 0: the snapshots carry their sound (FrameSnapshot::audio), put into the videos.
    void SetVideo(std::string ffmpeg, int fps, std::uint32_t maxWidth, int audioRate = 0) {
        ffmpeg_ = std::move(ffmpeg);
        videoFps_ = fps;
        videoMaxWidth_ = maxWidth;
        videoAudioRate_ = audioRate;
    }
    // The video being written finished (the app's end).
    void FinishVideo();
    bool VideoFailed() const { return videoFailed_.load(std::memory_order_acquire); }
    // A frame meant for a video was not drawn (the window minimised: no swapchain): the app holds the game's clock until
    // frames come again, so the video misses nothing.
    bool VideoHeld() const { return videoHeld_.load(std::memory_order_acquire); }

private:
    void Release(const FrameSnapshot& snapshot);

    rhi::Device& device_;
    devui::ImGuiLayer& imgui_;
    CrowdRenderer& crowd_;
    SceneRenderer& scene_;
    gfx::UiPass& ui_;
    gfx::PortraitPass& portraits_;
    gfx::AssetStore& assets_;
    core::JobSystem& jobs_;
    const CameraLatch& camera_;
    FeedbackChannel& feedback_;
    FrameRecorder& recorder_;
    core::FramePacer pacer_;
    gfx::RenderGraph graph_; // the frame's passes, rebuilt every frame
    std::uint32_t width_ = 0, height_ = 0;
    std::chrono::steady_clock::time_point lastPresent_{};
    std::uint64_t frames_ = 0;
    std::vector<std::pair<std::string, double>> gpuTotals_; // by pass name: summed ms
    std::uint64_t gpuFrames_ = 0;
    std::atomic<bool> captureDone_{false}, captureFailed_{false};
    VideoWriter video_;
    std::string ffmpeg_ = "ffmpeg";
    int videoFps_ = 30;
    std::uint32_t videoMaxWidth_ = 1280;
    int videoAudioRate_ = 0;
    std::atomic<bool> videoFailed_{false}, videoHeld_{false};
};

} // namespace sr::client
