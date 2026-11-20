#pragma once

// Numbers that flow back from the render thread (render -> game): the
// latest frame's statistics for the developer tools, and every frame's timings for benchmark
// summaries (--run-seconds).

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

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

private:
    mutable std::mutex mutex_;
    RenderFeedback value_;
    DepthProbe probe_;
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
