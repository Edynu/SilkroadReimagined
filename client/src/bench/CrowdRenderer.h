#pragma once

// Render-thread side of the crowd benchmark: draws one box per bone, instanced, from the bone
// transforms the game thread's pose jobs wrote into GPU memory. The pass is recorded in parallel
// chunks on worker jobs into the crowd's own colour and depth
// targets, then copied into the swapchain pass.

#include <cstdint>
#include <string>

#include "frame/FrameSnapshot.h"
#include "sr/core/Jobs.h"
#include "sr/gfx/RenderGraph.h"
#include "sr/rhi/Rhi.h"

namespace sr::client {

class CrowdRenderer {
public:
    bool Init(rhi::Device& device, std::string& error);
    void Shutdown();

    // Before the swapchain pass: the crowd into its targets (sized width x height).
    void Render(rhi::CommandList& cmd, const CrowdView& view, const OrbitCamera& camera, core::JobSystem& jobs,
                std::uint32_t width, std::uint32_t height);
    // Inside the swapchain pass: copies the last rendered crowd image over the screen.
    void Composite(rhi::CommandList& cmd);
    // The crowd pass in the frame's graph; returns its colour result, which
    // the swapchain pass reads (Composite). `view` and `camera` must live until the graph ran.
    gfx::RenderGraph::Resource AddPasses(gfx::RenderGraph& graph, const CrowdView& view, const OrbitCamera& camera,
                                         core::JobSystem& jobs, std::uint32_t width, std::uint32_t height);

private:
    void EnsureTargets(std::uint32_t width, std::uint32_t height);

    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle crowd_;
    rhi::PipelineHandle blit_;
    rhi::BufferHandle cube_; // 24 vertices (position, normal) then 36 16-bit indices
    std::size_t indexOffset_ = 0;
    rhi::TextureHandle color_, depth_;
    std::uint32_t width_ = 0, height_ = 0;
    bool rendered_ = false;
};

} // namespace sr::client
