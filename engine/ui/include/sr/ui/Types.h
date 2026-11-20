#pragma once

#include <cstdint>

namespace sr::ui {

// Screen positions are pixels from the window's top-left corner.
struct Vec2 {
    float x = 0.0f, y = 0.0f;
};

struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool Contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
    Rect Intersect(const Rect& o) const;
};

inline Rect Rect::Intersect(const Rect& o) const {
    const float x0 = x > o.x ? x : o.x, y0 = y > o.y ? y : o.y;
    const float x1 = Right() < o.Right() ? Right() : o.Right(), y1 = Bottom() < o.Bottom() ? Bottom() : o.Bottom();
    return {x0, y0, x1 > x0 ? x1 - x0 : 0.0f, y1 > y0 ? y1 - y0 : 0.0f};
}

// 0xAABBGGRR, as packed into vertices.
using Color = std::uint32_t;
constexpr Color Rgba(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a = 255) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}
constexpr Color kWhite = Rgba(255, 255, 255);
// From the original's 0xAARRGGBB.
constexpr Color Argb(std::uint32_t argb) { return Rgba((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, argb >> 24); }
constexpr Color WithAlpha(Color c, float alpha) {
    const float a = static_cast<float>(c >> 24) * (alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha);
    return (c & 0x00FFFFFFu) | (static_cast<std::uint32_t>(a + 0.5f) << 24);
}

// A texture the renderer can sample: its bindless index (0 = none).
// The scene's overlay (a window's 3D character, rendered by its own camera): the renderer puts its
// target in this texture's place (gfx::kSceneOverlayTexture is the same number).
constexpr std::uint32_t kSceneOverlayTexture = 0xFFFFFF00u;

struct TextureRef {
    std::uint32_t index = 0;
    bool premultipliedAlpha = false;
    bool opaque = false; // ignore the texture's alpha (a rendered scene)
    bool operator==(const TextureRef&) const = default;
};

} // namespace sr::ui
