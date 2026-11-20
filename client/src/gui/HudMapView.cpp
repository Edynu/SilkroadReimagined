#include "gui/HudMapView.h"

#include <algorithm>
#include <cmath>

#include "sr/ui/Controls.h"

namespace sr::client {

bool MapView::Show(const assets::WorldMapData& data, int id) {
    const assets::WorldMapRow* map = id < 2001 ? data.Map(id) : nullptr;
    const assets::DungeonMapRow* dungeon = id >= 2001 ? data.Dungeon(id) : nullptr;
    if (map == nullptr && dungeon == nullptr) return false;
    data_ = &data;
    map_ = map;
    dungeon_ = dungeon;
    id_ = id;
    scroll_ = {0, 0}; // a map is laid from the view's top left
    last_.reset();
    return true;
}

void MapView::Clear() {
    map_ = nullptr;
    dungeon_ = nullptr;
    id_ = -1;
    last_.reset();
}

float MapView::Width() const { return map_ != nullptr ? float(map_->drawW) : dungeon_ != nullptr ? float(dungeon_->drawW) : 0.0f; }
float MapView::Height() const { return map_ != nullptr ? float(map_->drawH) : dungeon_ != nullptr ? float(dungeon_->drawH) : 0.0f; }

std::optional<ui::Vec2> MapView::Pixel(std::uint16_t region, float x, float z) const {
    if (!Shown()) return std::nullopt;
    const bool inDungeon = (region & 0x8000) != 0;
    if (inDungeon != (dungeon_ != nullptr)) return std::nullopt;
    // A dungeon's own space stands at region 128 across and down.
    const int rx = inDungeon ? 128 : (region & 0xFF), rz = inDungeon ? 128 : ((region >> 8) & 0x7F);
    const double wx = x / 10.0 + (rx - 135) * 192.0, wz = z / 10.0 + (rz - 92) * 192.0;
    const double left = map_ != nullptr ? map_->Left() : dungeon_->Left();
    const double right = map_ != nullptr ? map_->Right() : dungeon_->Right();
    const double top = map_ != nullptr ? map_->Top() : dungeon_->Top();
    const double bottom = map_ != nullptr ? map_->Bottom() : dungeon_->Bottom();
    const double sx = std::fabs(right - left) / Width(), sz = std::fabs(top - bottom) / Height();
    if (sx <= 0.0 || sz <= 0.0) return std::nullopt;
    return ui::Vec2{float((wx - left) / sx), float(Height() - (wz - bottom) / sz)};
}

ui::Vec2 MapView::LabelPixel(const assets::WorldMapLabel& label) const {
    if (map_ != nullptr && label.map == 0) {
        return {float(label.x + (label.regionX - map_->regionLeft) * 32), float(label.y + (map_->regionTop - label.regionZ) * 32)};
    }
    return {float(label.x), float(label.y)};
}

void MapView::SetViewSize(ui::Vec2 size) {
    view_ = size;
    ScrollBy({0, 0});
}

void MapView::ScrollBy(ui::Vec2 delta) {
    scroll_.x = std::clamp(scroll_.x + delta.x, 0.0f, std::max(0.0f, Width() - view_.x));
    scroll_.y = std::clamp(scroll_.y + delta.y, 0.0f, std::max(0.0f, Height() - view_.y));
}

void MapView::CentreOn(ui::Vec2 pixel) {
    scroll_ = {std::floor(pixel.x - view_.x / 2.0f), std::floor(pixel.y - view_.y / 2.0f)};
    ScrollBy({0, 0});
}

void MapView::Draw(ui::Painter& paint, const ui::Rect& view, bool corners) const {
    paint.Fill(view, ui::Rgba(0, 0, 0));
    if (Shown()) {
        paint.PushClip(view);
        // Tiles a little way off the view are asked for too, not drawn (ours): dragged in, they are mostly in already
        // instead of black until they load. The margin: half the view or a tile, whichever is more, each side.
        const auto tile = [&](const std::string& path, float px, float py, float w, float h, ui::Vec2 uv1) {
            const ui::Vec2 at = ToScreen({px, py}, view);
            if (at.x + w < view.x || at.y + h < view.y || at.x > view.x + view.w || at.y > view.y + view.h) {
                const float mx = std::max(w, view.w / 2.0f), my = std::max(h, view.h / 2.0f);
                const bool near = at.x + w >= view.x - mx && at.y + h >= view.y - my && at.x <= view.x + view.w + mx &&
                                  at.y <= view.y + view.h + my;
                if (near) paint.Prefetch(path); // loaded meanwhile (nothing waits for it)
                return;
            }
            paint.Image(path, {at.x, at.y, w, h}, {0, 0}, uv1);
        };
        if (map_ != nullptr && map_->tiled) {
            const float w = float(map_->textureW), h = float(map_->textureH);
            for (int i = 0; i < map_->TilesAcross(); ++i) {
                for (int j = 0; j < map_->TilesDown(); ++j) tile(map_->Tile(i, j), i * w, j * h, w, h, {1, 1});
            }
        } else if (map_ != nullptr) {
            // One picture: its drawn part of the texture over the drawn size.
            tile(map_->picture, 0, 0, float(map_->drawW), float(map_->drawH),
                 {float(map_->pictureW) / float(std::max(1, map_->textureW)), float(map_->pictureH) / float(std::max(1, map_->textureH))});
        } else {
            const float w = float(dungeon_->drawW) / float(std::max(1, dungeon_->tilesAcross));
            const float h = float(dungeon_->drawH) / float(std::max(1, dungeon_->tilesDown));
            for (int i = 0; i < dungeon_->tilesAcross; ++i) {
                for (int j = 0; j < dungeon_->tilesDown; ++j) tile(dungeon_->Tile(i, j), i * w, j * h, w, h, {1, 1});
            }
        }
        paint.PopClip();
    }
    if (corners) {
        paint.Image("interface\\worldmap\\wmap_window_edge_1.ddj", {view.x, view.y, 4, 4});
        paint.Image("interface\\worldmap\\wmap_window_edge_2.ddj", {view.x + view.w - 4, view.y, 4, 4});
        paint.Image("interface\\worldmap\\wmap_window_edge_3.ddj", {view.x + view.w - 4, view.y + view.h - 4, 4, 4});
        paint.Image("interface\\worldmap\\wmap_window_edge_4.ddj", {view.x, view.y + view.h - 4, 4, 4});
    }
}

void MapView::DrawLabels(ui::Painter& paint, const ui::Rect& view, const std::function<std::string(const std::string&)>& text) const {
    if (!Shown() || data_ == nullptr) return;
    paint.PushClip(view);
    // The pictures first, then the texts over them: a town's picture has a blank plate where its name
    // falls (Jangan's: SN_ZONE_22001 lands on city_jangan.ddj's plate; inferred from the art, the
    // table lists the name first).
    for (const assets::WorldMapLabel& l : data_->labels) {
        if (l.map != id_ || !l.picture) continue;
        const ui::Vec2 at = ToScreen(LabelPixel(l), view);
        const float w = float(l.width), h = float(l.height);
        if (w <= 0 || h <= 0 || at.x + w < view.x || at.y + h < view.y || at.x > view.x + view.w || at.y > view.y + view.h) continue;
        paint.Image(l.what, {at.x, at.y, w, h});
    }
    for (const assets::WorldMapLabel& l : data_->labels) {
        if (l.map != id_ || l.picture) continue;
        const ui::Vec2 at = ToScreen(LabelPixel(l), view);
        const std::string s = text ? text(l.what) : l.what;
        if (s.empty() || s == l.what) continue; // a key without a text
        const float w = std::ceil(paint.TextWidth(s, 0)) + 2.0f;
        const ui::Rect r{std::floor(at.x - w / 2.0f), at.y, w, 14.0f};
        if (r.x + r.w < view.x || r.y + r.h < view.y || r.x > view.x + view.w || r.y > view.y + view.h) continue;
        const int style = (l.shadow ? ui::kTextShadow : 0) | (l.bold ? ui::kTextBold : 0);
        ui::DrawLabel(paint, s, r, ui::Rgba(l.red, l.green, l.blue), 0, 1, 0, style == 0 ? ui::kTextShadow : style);
    }
    paint.PopClip();
}

const assets::WorldMapLabel* MapView::LinkAt(ui::Vec2 at, const ui::Rect& view) const {
    if (!Shown() || data_ == nullptr) return nullptr;
    if (at.x < view.x || at.y < view.y || at.x >= view.x + view.w || at.y >= view.y + view.h) return nullptr;
    for (const assets::WorldMapLabel& l : data_->labels) {
        if (l.map != id_ || !l.picture || l.opens <= 0 || l.opens > 1000) continue;
        const ui::Vec2 p = ToScreen(LabelPixel(l), view);
        if (at.x > p.x && at.x < p.x + float(l.width) && at.y > p.y && at.y < p.y + float(l.height)) return &l;
    }
    return nullptr;
}

bool MapView::Drag(const ui::Frame& frame, const ui::Widget& owner, const ui::Rect& view, bool* pressed) {
    if (pressed != nullptr) *pressed = false;
    const ui::Vec2 mouse = frame.pointer.position;
    const bool inside = mouse.x >= view.x && mouse.y >= view.y && mouse.x < view.x + view.w && mouse.y < view.y + view.h;
    if (frame.input.Pressed(ui::MouseButton::Left) && frame.pointer.hovered == &owner && inside) {
        last_ = mouse;
        if (pressed != nullptr) *pressed = true;
        return true;
    }
    if (!last_) return false;
    // Held until every button is up (the interface's hold, as the original's capture).
    if (frame.pointer.held != &owner || frame.pointer.heldButton != ui::MouseButton::Left) {
        last_.reset();
        return false;
    }
    // The map follows the mouse: dragged right, the map moves right (its left part comes into the view).
    ScrollBy({last_->x - mouse.x, last_->y - mouse.y});
    last_ = mouse;
    return true;
}

} // namespace sr::client
