// Borders and backgrounds.
// Every part is one quad at its texture's own size or tiled at it; a part whose texture did not
// load is left out; nothing here draws an interior.

#include <algorithm>
#include <cmath>

#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

namespace sr::ui {

namespace {

// A texture repeated from `from` along x (or y) over `length` pixels at its own size, the last
// copy cropped (its first pixels shown), `thickness` across.
void TileAlong(Painter& paint, const std::string& path, Vec2 from, float length, bool vertical, Color tint) {
    const Vec2 s = paint.TextureSize(path);
    const float step = vertical ? s.y : s.x;
    if (step <= 0.0f || length <= 0.0f) return;
    for (float at = 0.0f; at < length; at += step) {
        const float part = std::min(step, length - at);
        if (vertical) {
            paint.Image(path, {from.x, from.y + at, s.x, part}, {0, 0}, {1, part / step}, tint);
        } else {
            paint.Image(path, {from.x + at, from.y, part, s.y}, {0, 0}, {part / step, 1}, tint);
        }
    }
}

std::unique_ptr<Widget> MakeStatic(Interface& interface, int id, Rect r, std::string text, int fontIndex, int hAlign, int vAlign) {
    assets::UiLayoutNode node;
    node.type = "CIFStatic";
    node.id = id;
    node.rect = {r.x, r.y, r.w, r.h};
    node.fontIndex = fontIndex;
    node.hAlign = hAlign;
    node.vAlign = vAlign;
    auto w = interface.Make(node);
    w->SetText(std::move(text));
    return w;
}

} // namespace

void FrameBorder::Draw(Frame& frame) {
    // A background colour its owner set. The node's DDJ is the parts' prefix, not a texture: drawn as
    // one, it was a missing file (a warning) behind every frame; a whole texture is drawn as a window's.
    const std::string p = prefix();
    if (p.empty() || p.ends_with(".ddj")) {
        Widget::Draw(frame);
        return;
    }
    if ((background >> 24) != 0) frame.paint.Fill(ScreenRect(), WithAlpha(background, float(background >> 24) / 255.0f * Opacity()));
    Painter& paint = frame.paint;
    const Color tint = WithAlpha(kWhite, Opacity());
    const std::string lu = p + "left_up.ddj", ru = p + "right_up.ddj", rd = p + "right_down.ddj", ld = p + "left_down.ddj",
                      ls = p + "left_side.ddj", mu = p + "mid_up.ddj", rs = p + "right_side.ddj", md = p + "mid_down.ddj";
    const Vec2 a = paint.TextureSize(lu), b = paint.TextureSize(ru), c = paint.TextureSize(rd), d = paint.TextureSize(ld);
    const Vec2 bottom = paint.TextureSize(md), right = paint.TextureSize(rs);
    const Rect r = ScreenRect();
    paint.Image(lu, {r.x, r.y, a.x, a.y}, {0, 0}, {1, 1}, tint);
    paint.Image(ru, {r.Right() - b.x, r.y, b.x, b.y}, {0, 0}, {1, 1}, tint);
    paint.Image(rd, {r.Right() - c.x, r.Bottom() - c.y, c.x, c.y}, {0, 0}, {1, 1}, tint);
    paint.Image(ld, {r.x, r.Bottom() - d.y, d.x, d.y}, {0, 0}, {1, 1}, tint);
    TileAlong(paint, mu, {r.x + a.x, r.y}, r.w - a.x - b.x, false, tint);
    TileAlong(paint, md, {r.x + d.x, r.Bottom() - bottom.y}, r.w - d.x - c.x, false, tint);
    TileAlong(paint, ls, {r.x, r.y + a.y}, r.h - a.y - d.y, true, tint);
    // The original starts the right edge below the left-top corner (the same height as the
    // right-top one in every shipped frame).
    TileAlong(paint, rs, {r.Right() - right.x, r.y + a.y}, r.h - b.y - c.y, true, tint);
}

void SubFrame::Created(Interface& interface) {
    // The caption: a static at (10, 7), 20 narrower, 12 tall, font 0, centred at the top.
    Add(MakeStatic(interface, 0, {10, 7, rect.w - 20, 12}, text(), 0, 1, 0));
    SetText({});
}

void MainFrame::Created(Interface& interface) {
    // Title at (10, 12), the strip to drag by over the top 34 pixels, the close button 26 from
    // the right.
    Add(MakeStatic(interface, 0, {10, 12, rect.w - 21, 12}, text(), 0, 1, 0));
    SetText({});
    assets::UiLayoutNode drag;
    drag.type = "CIFDragableArea";
    drag.id = 1;
    drag.rect = {10, 0, rect.w - 21, 34};
    Add(interface.Make(drag));
    assets::UiLayoutNode close;
    close.type = "CIFCloseButton";
    close.id = 2;
    close.rect = {rect.w - 26, 10, 16, 16};
    Add(interface.Make(close));
    interface_ = &interface;
}

bool MainFrame::OnNotice(const Notice& notice) {
    if (notice.kind == Notice::Kind::Clicked && notice.control == 2 && notice.source->parent() == this) {
        if (onNotice) onNotice(notice); // the window's behaviour hears of it first
        Root()->visible = false;
        if (interface_ != nullptr) interface_->Sound("SND_WINDOW_CLOSE");
        return true;
    }
    return FrameBorder::OnNotice(notice);
}

void ScrollManager::Created(Interface&) {
    // Margins (0, 0): the bar's travel is the height less its arrows and thumb, at the right edge
    // 16 below the top.
    if (Widget* bar = Child(10)) {
        bar->rect = {rect.w - 16.0f, 16.0f, 16.0f, rect.h - 48.0f};
    }
}

void TileFill::Draw(Frame& frame) {
    Widget::Draw(frame);
    const std::string& path = node().ddj;
    const Vec2 s = frame.paint.TextureSize(path);
    if (s.x <= 0.0f || s.y <= 0.0f) return;
    const Rect r = ScreenRect();
    const Color tint = WithAlpha(kWhite, Opacity());
    for (float y = 0; y < r.h; y += s.y) {
        for (float x = 0; x < r.w; x += s.x) {
            const float w = std::min(s.x, r.w - x), h = std::min(s.y, r.h - y);
            frame.paint.Image(path, {r.x + x, r.y + y, w, h}, {0, 0}, {w / s.x, h / s.y}, tint);
        }
    }
}

void StretchBox::Draw(Frame& frame) {
    // The base look first (a background colour), then the eight parts.
    if ((background >> 24) != 0) frame.paint.Fill(ScreenRect(), WithAlpha(background, float(background >> 24) / 255.0f * Opacity()));
    const std::string& p = prefix.empty() ? node().ddj : prefix;
    if (p.empty()) return;
    Painter& paint = frame.paint;
    const Color tint = WithAlpha(kWhite, Opacity());
    const std::string lu = p + "left_up.ddj", ru = p + "right_up.ddj", rd = p + "right_down.ddj", ld = p + "left_down.ddj",
                      ls = p + "left_side.ddj", rs = p + "right_side.ddj";
    const Vec2 a = paint.TextureSize(lu), b = paint.TextureSize(ru), c = paint.TextureSize(rd), d = paint.TextureSize(ld);
    const Vec2 left = paint.TextureSize(ls), right = paint.TextureSize(rs);
    const Rect r = ScreenRect();
    paint.Image(lu, {r.x, r.y, a.x, a.y}, {0, 0}, {1, 1}, tint);
    paint.Image(ru, {r.Right() - b.x, r.y, b.x, b.y}, {0, 0}, {1, 1}, tint);
    paint.Image(rd, {r.Right() - c.x, r.Bottom() - c.y, c.x, c.y}, {0, 0}, {1, 1}, tint);
    paint.Image(ld, {r.x, r.Bottom() - d.y, d.x, d.y}, {0, 0}, {1, 1}, tint);
    paint.Image(ls, {r.x, r.y + a.y, left.x, r.h - a.y - d.y}, {0, 0}, {1, 1}, tint);
    paint.ImageTurned(ls, {r.x + a.x, r.y, r.w - a.x - b.x, left.x}, tint);
    paint.Image(rs, {r.Right() - right.x, r.y + b.y, right.x, r.h - b.y - c.y}, {0, 0}, {1, 1}, tint);
    paint.ImageTurned(rs, {r.x + d.x, r.Bottom() - right.x, r.w - d.x - c.x, right.x}, tint);
}

void Bar::Created(Interface& interface) {
    // The caption: a static over the whole bar, font 0, centred both ways.
    Add(MakeStatic(interface, 0, {0, 0, rect.w, rect.h}, text(), 0, 1, 1));
    SetText({});
}

void Bar::Draw(Frame& frame) {
    // Its DDJ is the pieces' prefix, not a texture of its own: only the background under them.
    if ((background >> 24) != 0) frame.paint.Fill(ScreenRect(), WithAlpha(background, float(background >> 24) / 255.0f * Opacity()));
    const std::string& p = pieces.empty() ? node().ddj : pieces;
    if (p.empty()) return;
    Painter& paint = frame.paint;
    const Color tint = WithAlpha(kWhite, Opacity());
    const std::string left = p + "left.ddj", mid = p + "mid.ddj", right = p + "right.ddj";
    const Vec2 l = paint.TextureSize(left), m = paint.TextureSize(mid), rt = paint.TextureSize(right);
    const Rect r = ScreenRect();
    // Every piece spans the bar's height.
    paint.Image(left, {r.x, r.y, l.x, r.h}, {0, 0}, {1, 1}, tint);
    if (m.x > 0.0f) {
        const float end = r.w - rt.x;
        for (float x = l.x; x < end; x += m.x) {
            const float w = std::min(m.x, end - x);
            paint.Image(mid, {r.x + x, r.y, w, r.h}, {0, 0}, {w / m.x, 1}, tint);
        }
    }
    paint.Image(right, {r.Right() - rt.x, r.y, rt.x, r.h}, {0, 0}, {1, 1}, tint);
}

void Lattice::Draw(Frame& frame) {
    // Its DDJ is the cells' prefix, not a texture of its own: only the background under them.
    if ((background >> 24) != 0) frame.paint.Fill(ScreenRect(), WithAlpha(background, float(background >> 24) / 255.0f * Opacity()));
    const std::string& p = node().ddj;
    if (p.empty()) return;
    constexpr float kCell = 36.0f;
    const Rect r = ScreenRect();
    const int columns = int(r.w / kCell), rows = int(r.h / kCell);
    const Color tint = WithAlpha(kWhite, Opacity());
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const bool lastColumn = column == columns - 1, lastRow = row == rows - 1;
            const char* part = lastColumn && lastRow ? "right_down" : lastRow ? "left_down" : lastColumn ? "right_up" : "left_up";
            frame.paint.Image(p + part + ".ddj", {r.x + float(column) * kCell, r.y + float(row) * kCell, kCell, kCell}, {0, 0}, {1, 1}, tint);
        }
    }
}

void Fade::Draw(Frame& frame) {
    Rect r = ScreenRect();
    if (node().hasClientRect) {
        const auto& c = node().clientRect;
        r = {r.x + c.x, r.y + c.y, r.w - c.x - c.w, r.h - c.y - c.h};
    }
    frame.paint.Fill(r, WithAlpha(color, float(color >> 24) / 255.0f * Opacity()));
}

} // namespace sr::ui
