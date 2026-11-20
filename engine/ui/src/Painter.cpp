#include "sr/ui/Painter.h"

#include <algorithm>
#include <cmath>

#include "sr/ui/Font.h"

namespace sr::ui {

float FontPixels(int fontIndex) {
    // -MulDiv(points, 96, 72), rounded as MulDiv does.
    constexpr float kPixels[] = {12.0f, 11.0f, 16.0f, 15.0f, 20.0f};
    return kPixels[std::clamp(fontIndex, 0, 4)];
}

Resources::Texture Painter::Get(const std::string& path) {
    Resources::Texture t = resources_.GetTexture(path);
    if (!t.resident && !t.failed) ++pending_;
    return t;
}

Vec2 Painter::TextureSize(const std::string& path) {
    const Resources::Texture t = Get(path);
    return t.resident ? Vec2{t.width, t.height} : Vec2{};
}

bool Painter::HasTexture(const std::string& path) { return !Get(path).failed; }

void Painter::Image(const std::string& path, Rect r, Vec2 uv0, Vec2 uv1, Color tint) {
    if (path.empty() || r.w <= 0.0f || r.h <= 0.0f) return;
    const Resources::Texture t = Get(path);
    if (!t.resident) return;
    list_.Image(t.ref, ToScreen(r), uv0, uv1, tint);
}

void Painter::ImageOpaque(const std::string& path, Rect r, Vec2 uv0, Vec2 uv1) {
    if (path.empty() || r.w <= 0.0f || r.h <= 0.0f) return;
    Resources::Texture t = Get(path);
    if (!t.resident) return;
    t.ref.opaque = true;
    list_.Image(t.ref, ToScreen(r), uv0, uv1, kWhite);
}

void Painter::SceneOverlay(Rect r, Vec2 screen) {
    const Rect s = ToScreen(r);
    if (s.w <= 0.0f || s.h <= 0.0f || screen.x <= 0.0f || screen.y <= 0.0f) return;
    list_.Image(TextureRef{kSceneOverlayTexture, false, false}, s, {s.x / screen.x, s.y / screen.y},
                {(s.x + s.w) / screen.x, (s.y + s.h) / screen.y});
}

bool Painter::PushMask(const std::string& path, Rect r) {
    const Resources::Texture t = Get(path);
    if (!t.resident) return false;
    list_.PushMask(t.ref, ToScreen(r));
    return true;
}

void Painter::ImageQuad(const std::string& path, const Vec2 (&p)[4], const Vec2 (&uv)[4], Color tint) {
    if (path.empty()) return;
    const Resources::Texture t = Get(path);
    if (!t.resident) return;
    const Vec2 s[4] = {{p[0].x * scale_, p[0].y * scale_}, {p[1].x * scale_, p[1].y * scale_}, {p[2].x * scale_, p[2].y * scale_},
                       {p[3].x * scale_, p[3].y * scale_}};
    list_.Quad(t.ref, s, uv, tint);
}

void Painter::ImageCorners(const std::string& path, Rect r, const Vec2 (&uv)[4], Color tint) {
    if (path.empty() || r.w <= 0.0f || r.h <= 0.0f) return;
    const Resources::Texture t = Get(path);
    if (!t.resident) return;
    const Rect s = ToScreen(r);
    const Vec2 p[4] = {{s.x, s.y}, {s.Right(), s.y}, {s.Right(), s.Bottom()}, {s.x, s.Bottom()}};
    list_.Quad(t.ref, p, uv, tint);
}

void Painter::ImageTurned(const std::string& path, Rect r, Color tint) {
    if (path.empty() || r.w <= 0.0f || r.h <= 0.0f) return;
    const Resources::Texture t = Get(path);
    if (!t.resident) return;
    const Rect s = ToScreen(r);
    // The texture's left edge runs along the rect's top.
    const Vec2 p[4] = {{s.x, s.y}, {s.Right(), s.y}, {s.Right(), s.Bottom()}, {s.x, s.Bottom()}};
    const Vec2 uv[4] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
    list_.Quad(t.ref, p, uv, tint);
}

void Painter::Fill(Rect r, Color color) { list_.Fill(ToScreen(r), color); }

void Painter::FillQuad(const Vec2 (&p)[4], Color color) {
    const Vec2 s[4] = {{p[0].x * scale_, p[0].y * scale_}, {p[1].x * scale_, p[1].y * scale_}, {p[2].x * scale_, p[2].y * scale_},
                       {p[3].x * scale_, p[3].y * scale_}};
    list_.FillQuad(s, color);
}

void Painter::Outline(Rect r, Color color) { list_.Outline(ToScreen(r), color, std::max(1.0f, scale_)); }

float Painter::TextWidth(std::string_view text, int fontIndex, float sizeScale) {
    Font* font = resources_.GetFont();
    if (!font || text.empty()) return 0.0f;
    return font->Measure(text, FontPixels(fontIndex) * sizeScale * scale_).x / scale_;
}

float Painter::LineHeight(int fontIndex) {
    Font* font = resources_.GetFont();
    return font ? font->LineHeight(FontPixels(fontIndex) * scale_) / scale_ : FontPixels(fontIndex);
}

void Painter::Text(std::string_view text, Rect r, Color color, int fontIndex, int hAlign, int vAlign, float sizeScale) {
    Font* font = resources_.GetFont();
    if (text.empty() || !font) return;
    const float size = FontPixels(fontIndex) * sizeScale * scale_;
    const Vec2 extent = font->Measure(text, size);
    const Rect s = ToScreen(r);
    Vec2 pos{s.x, s.y};
    if (hAlign == 1) pos.x = s.x + std::floor((s.w - extent.x) * 0.5f);
    if (hAlign == 2) pos.x = s.Right() - extent.x;
    if (vAlign == 1) pos.y = s.y + std::floor((s.h - extent.y) * 0.5f);
    if (vAlign == 2) pos.y = s.Bottom() - extent.y;
    list_.PushClip(s);
    font->Draw(list_, text, pos, size, color);
    list_.PopClip();
}

} // namespace sr::ui
