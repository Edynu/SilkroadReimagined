#include "sr/ui/DrawList.h"

namespace sr::ui {

namespace {
constexpr Rect kNoClip{-1e6f, -1e6f, 2e6f, 2e6f};
}

void DrawList::Clear() {
    vertices_.clear();
    indices_.clear();
    commands_.clear();
    clips_.clear();
    masks_.clear();
    additive_ = 0;
}

Rect DrawList::clip() const { return clips_.empty() ? kNoClip : clips_.back(); }

void DrawList::PushClip(Rect r) { clips_.push_back(clips_.empty() ? r : clips_.back().Intersect(r)); }

void DrawList::PopClip() {
    if (!clips_.empty()) clips_.pop_back();
}

void DrawList::PushMask(TextureRef mask, Rect rect) { masks_.emplace_back(mask, rect); }

void DrawList::PopMask() {
    if (!masks_.empty()) masks_.pop_back();
}

void DrawList::Begin(TextureRef texture) {
    const Rect c = clip();
    const TextureRef mask = masks_.empty() ? TextureRef{} : masks_.back().first;
    const Rect m = masks_.empty() ? Rect{} : masks_.back().second;
    const float depth = depths_.empty() ? 0.0f : depths_.back();
    const bool additive = additive_ > 0;
    if (!commands_.empty()) {
        const DrawCommand& last = commands_.back();
        if (last.texture == texture && last.clip.x == c.x && last.clip.y == c.y && last.clip.w == c.w && last.clip.h == c.h &&
            last.mask == mask && last.maskRect.x == m.x && last.maskRect.y == m.y && last.maskRect.w == m.w && last.maskRect.h == m.h &&
            last.depth == depth && last.additive == additive) {
            return;
        }
    }
    DrawCommand command;
    command.texture = texture;
    command.clip = c;
    command.mask = mask;
    command.maskRect = m;
    command.depth = depth;
    command.additive = additive;
    command.indexOffset = static_cast<std::uint32_t>(indices_.size());
    commands_.push_back(command);
}

void DrawList::Quad(TextureRef texture, const Vec2 (&p)[4], const Vec2 (&uv)[4], Color tint) {
    if (texture.index == 0 || (tint >> 24) == 0) return;
    Begin(texture);
    const auto base = static_cast<std::uint32_t>(vertices_.size());
    for (int i = 0; i < 4; ++i) vertices_.push_back({p[i].x, p[i].y, uv[i].x, uv[i].y, tint});
    for (std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) indices_.push_back(base + i);
    commands_.back().indexCount += 6;
}

void DrawList::Image(TextureRef texture, Rect r, Vec2 uv0, Vec2 uv1, Color tint) {
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    const Vec2 p[4] = {{r.x, r.y}, {r.Right(), r.y}, {r.Right(), r.Bottom()}, {r.x, r.Bottom()}};
    const Vec2 uv[4] = {{uv0.x, uv0.y}, {uv1.x, uv0.y}, {uv1.x, uv1.y}, {uv0.x, uv1.y}};
    Quad(texture, p, uv, tint);
}

void DrawList::Fill(Rect r, Color color) { Image(white_, r, whiteUv_, whiteUv_, color); }

void DrawList::FillQuad(const Vec2 (&p)[4], Color color) {
    const Vec2 uv[4] = {whiteUv_, whiteUv_, whiteUv_, whiteUv_};
    Quad(white_, p, uv, color);
}

void DrawList::Outline(Rect r, Color color, float t) {
    Fill({r.x, r.y, r.w, t}, color);
    Fill({r.x, r.Bottom() - t, r.w, t}, color);
    Fill({r.x, r.y + t, t, r.h - 2 * t}, color);
    Fill({r.Right() - t, r.y + t, t, r.h - 2 * t}, color);
}

} // namespace sr::ui
