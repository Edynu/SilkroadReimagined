#pragma once

// Numbers that flow back from the render thread (render -> game): the
// latest frame's statistics for the developer tools, and every frame's timings for benchmark
// summaries (--run-seconds).

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sr/rhi/Rhi.h"

namespace sr::client {

struct RenderFeedback {
    rhi::Device::FrameStats device;
    rhi::Device::ResourceCounts resources;
    double renderWorkMs = 0; // recording and submitting, without waiting for the GPU or pacing
    double gpuWaitMs = 0;    // BeginFrame waiting for the frame before last
    double streamingMs = 0;  // the asset store's update
    rhi::Device::StreamingInfo streaming;
    rhi::Device::MemoryBudget memory;
    std::uint64_t frames = 0;
    std::vector<rhi::Device::GpuTimer> gpuTimers; // the render graph's passes, a few frames back
};

// The scene's depth read back at the pixel the game asked for (SceneView::probeX / probeY), a few frames late.
struct DepthProbe {
    bool valid = false;
    float x = -1.0f, y = -1.0f; // the pixel asked for (swapchain pixels)
    float depth = 1.0f;         // the depth target's value there (0 near .. 1 far)
    std::uint64_t frame = 0;    // the rendered frame it was read in
};

// The sound occlusion's depth cube (ours, experimental): six square faces round the camera's eye, side by side in an atlas
// of 3 x 2 faces, each a 90-degree view (viewProj; GLM's left-handed 0..1 depth), the static world's depth in each texel
// (1: nothing within `farZ`). Read back a few frames late.
struct AudioCube {
    static constexpr int kFace = 64, kWidth = 3 * kFace, kHeight = 2 * kFace;
    glm::vec3 eye{0.0f};
    std::array<glm::mat4, 6> viewProj{};
    float nearZ = 2.0f, farZ = 1500.0f;
    std::vector<float> depth; // kWidth x kHeight, row by row
    std::uint64_t frame = 0;
};

class FeedbackChannel {
public:
    void Put(const RenderFeedback& feedback) {
        std::lock_guard lock(mutex_);
        value_ = feedback;
    }
    RenderFeedback Get() const {
        std::lock_guard lock(mutex_);
        return value_;
    }
    // The depth probe on its own (small, read every game frame).
    void PutProbe(const DepthProbe& probe) {
        std::lock_guard lock(mutex_);
        probe_ = probe;
    }
    DepthProbe Probe() const {
        std::lock_guard lock(mutex_);
        return probe_;
    }
    void PutAudioCube(std::shared_ptr<const AudioCube> cube) {
        std::lock_guard lock(mutex_);
        audioCube_ = std::move(cube);
    }
    std::shared_ptr<const AudioCube> LatestAudioCube() const {
        std::lock_guard lock(mutex_);
        return audioCube_;
    }

private:
    mutable std::mutex mutex_;
    RenderFeedback value_;
    DepthProbe probe_;
    std::shared_ptr<const AudioCube> audioCube_;
};

class FrameRecorder {
public:
    struct Sample {
        double intervalMs = 0; // between this frame's present and the last one's
        double renderMs = 0;   // render thread work
        double gameMs = 0;     // game thread work on the frame shown
        bool burst = false;    // a spawn burst was applied in the game frame shown
        double streamingMs = 0; // render thread: the asset store's update
    };
    void Add(const Sample& sample) {
        std::lock_guard lock(mutex_);
        samples_.push_back(sample);
    }
    std::size_t Count() const {
        std::lock_guard lock(mutex_);
        return samples_.size();
    }
    // Averages, percentiles and the worst frames; what checks.
    std::string Summary() const;

private:
    mutable std::mutex mutex_;
    std::vector<Sample> samples_;
};

} // namespace sr::client
