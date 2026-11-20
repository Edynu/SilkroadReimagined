#pragma once

// Character portraits: a posed model drawn into a small target of its
// own, cleared to opaque black, under the original's portrait light; the game UI then shows the
// target through a round mask (the player's bar, the party's bars). The original draws one only
// when it is asked to (a new look), so a portrait is a pass on the frames that ask for it, and the
// target keeps its picture in between. Render thread, in the frame's graph before the UI.

#include <array>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sr/gfx/AssetStore.h"
#include "sr/gfx/RenderGraph.h"
#include "sr/rhi/Rhi.h"

namespace sr::gfx {

// One portrait to draw: its targets (a RenderTarget colour, RGBA8, and a DepthStencil depth, D32,
// of the same size), the model (resident: the game holds a reference), its world transform (rows
// of a 3 x 4) and its skeleton's posed palette (glm, column-major), and the camera.
struct PortraitDraw {
    rhi::TextureHandle color, depth;
    AssetId model;
    std::array<float, 12> world{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::vector<glm::mat4> palette;
    glm::mat4 viewProj{1.0f};
};

class PortraitPass {
public:
    bool Init(rhi::Device& device, std::string& error);
    void Shutdown();

    // A pass a portrait, each writing its colour target; returns those targets, for the pass that
    // samples them (the UI) to read. A target is always cleared, so a model gone meanwhile leaves it
    // black, never undefined. `draws` and `assets` must live until the graph ran.
    std::span<const RenderGraph::Resource> AddPasses(RenderGraph& graph, std::span<const PortraitDraw> draws, AssetStore& assets);
    // Draws of a frame that was not drawn (a minimised window): they go with the next frame's, so a
    // target the interface shows is never left unrendered.
    void Keep(std::span<const PortraitDraw> draws);

private:
    void Draw(rhi::CommandList& cmd, const PortraitDraw& draw, AssetStore& assets) const;

    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle pipeline_;
    std::vector<RenderGraph::Resource> results_;
    std::vector<PortraitDraw> kept_, frame_;
};

} // namespace sr::gfx
