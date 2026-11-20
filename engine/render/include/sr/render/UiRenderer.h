#pragma once

#include <string>
#include <vector>

#include "sr/rhi/Rhi.h"
#include "sr/ui/DrawList.h"

namespace sr::ui {
class Font;
}

namespace sr::render {

// Draws sr::ui draw lists (the game UI) into the current render pass, and keeps
// fonts' atlases on the GPU.
class UiRenderer {
public:
    bool Init(rhi::Device& device, std::string& error);
    void Shutdown();

    // Creates the font's atlas texture (call once before drawing with the font) and
    // uploads it again whenever new glyphs were added (call before Render).
    void SyncFont(ui::Font& font);
    // Back to front, in a pass of `width` x `height` pixels.
    void Render(rhi::CommandList& cmd, const std::vector<const ui::DrawList*>& lists, float width, float height);

private:
    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle pipeline_;
    struct FontTexture {
        const ui::Font* font = nullptr;
        rhi::TextureHandle texture;
    };
    std::vector<FontTexture> fonts_;
};

} // namespace sr::render
