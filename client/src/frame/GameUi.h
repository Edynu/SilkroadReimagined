#pragma once

// The game thread's side of the game UI pass (gfx::UiPass): sr::ui draw
// lists copied into the snapshot's UiFrame, and a font's atlas kept on the GPU (the game thread
// creates and updates the texture; the device takes that from any thread).

#include <string>

#include "sr/gfx/UiPass.h"
#include "sr/rhi/Rhi.h"
#include "sr/ui/DrawList.h"
#include "sr/ui/Font.h"

namespace sr::client {

// Appends a draw list to the frame (drawn after what is already there).
inline void AppendUi(gfx::UiFrame& frame, const ui::DrawList& list) {
    const auto vertexBase = static_cast<std::uint32_t>(frame.vertices.size());
    const auto indexBase = static_cast<std::uint32_t>(frame.indices.size());
    for (const ui::Vertex& v : list.vertices()) frame.vertices.push_back({v.x, v.y, v.u, v.v, v.color});
    for (std::uint32_t i : list.indices()) frame.indices.push_back(i + vertexBase);
    for (const ui::DrawCommand& c : list.commands()) {
        gfx::UiCommand out;
        out.texture = c.texture.index;
        out.premultipliedAlpha = c.texture.premultipliedAlpha;
        out.opaque = c.texture.opaque;
        out.clip[0] = c.clip.x;
        out.clip[1] = c.clip.y;
        out.clip[2] = c.clip.w;
        out.clip[3] = c.clip.h;
        out.mask = c.mask.index;
        out.maskRect[0] = c.maskRect.x;
        out.maskRect[1] = c.maskRect.y;
        out.maskRect[2] = c.maskRect.w;
        out.maskRect[3] = c.maskRect.h;
        out.depth = c.depth;
        out.additive = c.additive;
        out.indexOffset = c.indexOffset + indexBase;
        out.indexCount = c.indexCount;
        frame.commands.push_back(out);
    }
}

// A font's atlas on the GPU: made at the first Sync, then the rows new glyphs changed. `smooth`: the Video page's
// "Interface text" (the original's 1-bit letters, or smooth ones); a change draws the glyphs again.
class FontAtlas {
public:
    void Sync(rhi::Device& device, ui::Font& font, bool smooth = false) {
        font.SetSmooth(smooth);
        const auto width = static_cast<std::uint32_t>(font.atlasWidth()), height = static_cast<std::uint32_t>(font.atlasHeight());
        const auto pixels = std::as_bytes(std::span(font.atlasPixels()));
        if (!texture_) {
            const std::span<const std::byte> mip0 = pixels;
            const rhi::TextureInitData init{std::span(&mip0, 1)};
            texture_ = device.CreateTexture({width, height, 1, rhi::Format::RGBA8Unorm, "ui font atlas"}, &init);
            font.MarkUploaded({device.BindlessIndex(texture_), false});
            return;
        }
        if (!font.dirty()) return;
        const int top = std::max(0, font.dirtyTop()), bottom = std::min(font.atlasHeight(), font.dirtyBottom());
        if (bottom > top) {
            const std::size_t rowBytes = std::size_t{width} * 4;
            device.UpdateTexture(texture_, {0, top, width, static_cast<std::uint32_t>(bottom - top)},
                                 pixels.subspan(static_cast<std::size_t>(top) * rowBytes, static_cast<std::size_t>(bottom - top) * rowBytes),
                                 width * 4);
        }
        font.MarkUploaded({device.BindlessIndex(texture_), false});
    }
    // Gives the texture back through the snapshot (destroyed once the frame is recorded).
    rhi::TextureHandle Release() {
        const rhi::TextureHandle t = texture_;
        texture_ = {};
        return t;
    }

private:
    rhi::TextureHandle texture_;
};

} // namespace sr::client
