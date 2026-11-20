#pragma once

// The game UI's pass (C3h): the game's draw lists (sr::ui, which gfx does not
// see: the client copies them into a UiFrame in its snapshot), drawn over the finished scene in the
// swapchain pass (before the developer tools). Textures are bindless; a font's atlas is created and
// updated by the game thread (Device::CreateTexture / UpdateTexture may be called from any
// thread). Draw: render thread.

#include <cstdint>
#include <string>
#include <vector>

#include "sr/rhi/Rhi.h"

namespace sr::gfx {

// sr::ui's vertex layout: pixels, uv, RGBA8 colour.
struct UiVertex {
    float x = 0, y = 0;
    float u = 0, v = 0;
    std::uint32_t color = 0xFFFFFFFFu;
};
static_assert(sizeof(UiVertex) == 20);

// A command with this texture draws the scene's overlay target (ui::kSceneOverlayTexture), given
// to Draw; without one it is skipped.
constexpr std::uint32_t kSceneOverlayTexture = 0xFFFFFF00u;

struct UiCommand {
    std::uint32_t texture = 0;  // bindless
    bool premultipliedAlpha = false;
    bool opaque = false;        // ignore the texture's alpha
    float clip[4]{};            // x, y, width, height in pixels
    std::uint32_t mask = 0;     // bindless; 0: no mask
    float maskRect[4]{};        // where the mask is stretched, pixels
    float depth = 0;            // a world point's depth: hidden where the scene's is nearer (0: none)
    bool additive = false;      // added onto the target (the additive pipeline), not blended
    std::uint32_t indexOffset = 0, indexCount = 0;
};

// One frame's UI: the draw lists' vertices in one buffer, their indices rebased onto it.
struct UiFrame {
    std::vector<UiVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<UiCommand> commands; // index offsets into `indices`
    void Clear() {
        vertices.clear(); // keeps the capacity: no allocation per frame
        indices.clear();
        commands.clear();
    }
    bool Empty() const { return indices.empty(); }
};

class UiPass {
public:
    // Drawn in the swapchain pass (its format).
    bool Init(rhi::Device& device, std::string& error);
    void Shutdown();
    // Inside the swapchain pass, `width` x `height` pixels. `sceneDepth`: the scene's depth target
    // (bindless, the swapchain's size) for commands with a depth; 0: they are drawn as the rest.
    void Draw(rhi::CommandList& cmd, const UiFrame& frame, float width, float height, std::uint32_t sceneOverlay = 0,
              std::uint32_t sceneDepth = 0) const;

private:
    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle pipeline_;
    rhi::PipelineHandle additivePipeline_; // the same, adding (effect sprites drawn on black)
};

} // namespace sr::gfx
