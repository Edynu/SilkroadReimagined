#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "sr/ui/Types.h"

namespace sr::ui {

struct Vertex {
    float x, y;
    float u, v;
    Color color;
};

// A run of triangles with one texture and one clip rect.
struct DrawCommand {
    TextureRef texture;
    Rect clip;
    TextureRef mask; // its alpha multiplies the texture's (index 0: none)
    Rect maskRect;   // where the mask is stretched (pixels)
    float depth = 0; // a point of the world's depth (0..1): hidden where the scene is nearer (0: none)
    bool additive = false; // added onto what is behind (an effect sprite's black is nothing), not blended
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
};

// Textured, tinted quads in pixels, batched by texture and clip rect.
class DrawList {
public:
    void Clear();
    // A white texel, for untextured fills (the font atlas carries one).
    void SetWhite(TextureRef texture, Vec2 uv) {
        white_ = texture;
        whiteUv_ = uv;
    }

    void PushClip(Rect clip);
    void PopClip();
    Rect clip() const;
    // Draws until PopMask take their alpha also from `mask` stretched over `rect` (pixels).
    void PushMask(TextureRef mask, Rect rect);
    void PopMask();
    // Draws until PopDepth stand at that depth of the scene (names over heads: hidden behind walls).
    void PushDepth(float depth) { depths_.push_back(depth); }
    // Draws until PopAdditive are added onto what is behind (the effects' sprites drawn on black).
    void PushAdditive() { ++additive_; }
    void PopAdditive() {
        if (additive_ > 0) --additive_;
    }
    void PopDepth() {
        if (!depths_.empty()) depths_.pop_back();
    }

    void Image(TextureRef texture, Rect rect, Vec2 uv0 = {0, 0}, Vec2 uv1 = {1, 1}, Color tint = kWhite);
    // Four corners clockwise from the top-left, with their UVs.
    void Quad(TextureRef texture, const Vec2 (&p)[4], const Vec2 (&uv)[4], Color tint = kWhite);
    void Fill(Rect rect, Color color);
    // A filled quad of any shape (corners in order).
    void FillQuad(const Vec2 (&p)[4], Color color);
    void Outline(Rect rect, Color color, float thickness = 1.0f);

    const std::vector<Vertex>& vertices() const { return vertices_; }
    const std::vector<std::uint32_t>& indices() const { return indices_; }
    const std::vector<DrawCommand>& commands() const { return commands_; }
    bool empty() const { return indices_.empty(); }

private:
    void Begin(TextureRef texture);

    std::vector<Vertex> vertices_;
    std::vector<std::uint32_t> indices_;
    std::vector<DrawCommand> commands_;
    std::vector<Rect> clips_;
    std::vector<std::pair<TextureRef, Rect>> masks_;
    std::vector<float> depths_;
    int additive_ = 0;
    TextureRef white_;
    Vec2 whiteUv_;
};

} // namespace sr::ui
