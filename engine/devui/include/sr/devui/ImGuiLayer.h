#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <imgui.h>

#include "sr/platform/Window.h"
#include "sr/rhi/Rhi.h"

namespace sr::devui {

// How ImGui::Image() should show a texture.
enum class ChannelView : std::uint8_t { RGBA = 0, RGB = 1, Alpha = 2, Red = 3, Green = 4, Blue = 5 };

struct TextureView {
    std::uint32_t bindlessIndex = 0;
    rhi::SamplerIndex sampler = rhi::SamplerIndex::LinearClamp;
    ChannelView channel = ChannelView::RGBA;
    int mip = -1; // -1 = automatic
    bool premultipliedAlpha = false;
};

// Packs a TextureView into an ImTextureID for ImGui::Image().
ImTextureID MakeTextureId(const TextureView& view);

// A finished ImGui frame copied out of ImGui, so another thread (the render thread) can draw
// it while ImGui builds the next one.
struct ImGuiFrame {
    struct Command {
        ImVec4 clip;                   // in ImGui's coordinates
        ImTextureID texture = 0;
        std::uint32_t elemCount = 0;
        std::uint32_t firstIndex = 0;  // into `indices`
        std::int32_t vertexOffset = 0; // into `vertices`
        bool resetState = false;       // ImDrawCallback_ResetRenderState
    };
    std::vector<ImDrawVert> vertices;
    std::vector<ImDrawIdx> indices;
    std::vector<Command> commands;
    ImVec2 displayPos{0, 0};
    ImVec2 displaySize{0, 0};
    ImVec2 framebufferScale{1, 1};
    void Clear() {
        vertices.clear();
        indices.clear();
        commands.clear();
    }
};

// Dear ImGui on top of sr::rhi: SDL3 for input, our own renderer for drawing
// (font atlas and images are bindless textures).
class ImGuiLayer {
public:
    bool Init(rhi::Device& device, platform::Window& window, std::string& error);
    void Shutdown();

    void ProcessEvent(const void* sdlEvent);
    void NewFrame();
    // Finishes the ImGui frame and draws it. Call inside a render pass (single-threaded use).
    void Render(rhi::CommandList& cmd);
    // Finishes the ImGui frame: creates and updates its textures and copies the draw data into
    // `out` (the thread that runs ImGui).
    void EndFrame(ImGuiFrame& out);
    // Draws a copied frame inside a render pass (any one thread).
    void Draw(rhi::CommandList& cmd, const ImGuiFrame& frame);

private:
    void UpdateTexture(ImTextureData* tex);
    void SetupRenderState(rhi::CommandList& cmd, const ImGuiFrame& frame, std::size_t vertexOffset,
                          std::size_t indexOffset, rhi::BufferHandle buffer);

    rhi::Device* device_ = nullptr;
    ImGuiFrame singleThreaded_; // Render()'s copy
    rhi::PipelineHandle pipeline_;
    bool initialized_ = false;
};

} // namespace sr::devui
