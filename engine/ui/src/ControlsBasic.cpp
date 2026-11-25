// Images, text, buttons, check boxes, radio groups, the drag strip.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

namespace sr::ui {

namespace {

// The interface fonts' point sizes (indexes 0 to 4).
float FontPoints(int fontIndex) {
    constexpr float kPoints[] = {9.0f, 8.0f, 12.0f, 11.0f, 15.0f};
    return kPoints[std::clamp(fontIndex, 0, 4)];
}

std::vector<std::string_view> Lines(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            out.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::string Variant(const std::string& path, const char* suffix) {
    const std::size_t dot = path.rfind('.');
    return dot == std::string::npos ? path + suffix : path.substr(0, dot) + suffix + path.substr(dot);
}

} // namespace

float TextLineHeight(int fontIndex) { return FontPoints(fontIndex) + 5.0f; }

void DrawLabel(Painter& paint, std::string_view text, Rect r, Color color, int fontIndex, int hAlign, int vAlign, int style) {
    if (text.empty()) return;
    // The original's text block: lines at point size + 5 apart, as wide as the widest, placed in
    // the rect by the alignment (truncated to whole pixels); not clipped, never cut short.
    const std::vector<std::string_view> lines = Lines(text);
    const float lineHeight = TextLineHeight(fontIndex);
    float width = 0.0f;
    for (std::string_view line : lines) width = std::max(width, paint.TextWidth(line, fontIndex));
    const float height = lineHeight * float(lines.size());
    float x = r.x, y = r.y;
    if (hAlign == 1) x = r.x + (r.w - width) * 0.5f;
    if (hAlign == 2) x = r.Right() - width;
    if (vAlign == 1) y = r.y + (r.h - height) * 0.5f;
    if (vAlign == 2) y = r.Bottom() - height;
    x = std::trunc(x);
    y = std::trunc(y);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const float lineWidth = paint.TextWidth(lines[i], fontIndex);
        float lx = x;
        if (hAlign == 1) lx = std::trunc(x + (width - lineWidth) * 0.5f);
        if (hAlign == 2) lx = x + width - lineWidth;
        const Rect at{lx, y + lineHeight * float(i), lineWidth + 2.0f, lineHeight};
        // Bold (the original's weight 600) is drawn as the glyphs twice, a pixel apart (ours: the
        // font atlas has one weight).
        const int passes = (style & kTextBold) ? 2 : 1;
        for (int pass = 0; pass < passes; ++pass) {
            const float dx = float(pass);
            if (style & kTextShadow) paint.Text(lines[i], {at.x + dx + 1, at.y + 1, at.w, at.h}, Rgba(0, 0, 0, color >> 24), fontIndex, 0, 0);
        }
        for (int pass = 0; pass < passes; ++pass) paint.Text(lines[i], {at.x + float(pass), at.y, at.w, at.h}, color, fontIndex, 0, 0);
    }
}

// --- Static --------------------------------------------------------------------------------------

void Static::Created(Interface&) {
    const auto& c = node().fontColor;
    textColor = node().hasFontColor ? Rgba(c.r, c.g, c.b) : kWhite;
    if (node().style & 0x40) visible = false; // made hidden
}

void Static::Draw(Frame& frame) {
    const float opacity = Opacity();
    if ((node().style & 0x20) == 0) {
        if ((background >> 24) != 0) frame.paint.Fill(ScreenRect(), WithAlpha(background, float(background >> 24) / 255.0f * opacity));
        const std::string& path = texture.empty() ? node().ddj : texture;
        if (!path.empty() && !noArt) {
            FitToTexture(frame.paint, path);
            const Vec2 a = customUv ? uv0 : Vec2{node().uv[0], node().uv[1]}, b = customUv ? uv1 : Vec2{node().uv[2], node().uv[3]};
            frame.paint.Image(path, ScreenRect(), a, b, WithAlpha(tint, opacity));
        }
    }
    // The text takes the window's alpha in place of its colour's; grey when disabled.
    const Color color = enabled ? textColor : Rgba(180, 180, 180);
    Rect r = ScreenRect();
    if (node().hasClientRect) {
        const auto& c = node().clientRect;
        r = {r.x + c.x, r.y + c.y, r.w - c.x - c.w, r.h - c.y - c.h};
    }
    DrawLabel(frame.paint, text(), r, WithAlpha(color, opacity), node().fontIndex, node().hAlign, node().vAlign, textStyle);
}

// --- Picture with a mask -------------------------------------------------------------------------

void PictureClip::Draw(Frame& frame) {
    if (mask.empty()) {
        Static::Draw(frame);
        return;
    }
    const std::string& path = texture.empty() ? node().ddj : texture;
    if (path.empty() || (node().style & 0x20) != 0) return;
    // The picture's colour, the mask's alpha (observed 1.150).
    if (!frame.paint.PushMask(mask, ScreenRect())) return;
    const Vec2 a = customUv ? uv0 : Vec2{node().uv[0], node().uv[1]}, b = customUv ? uv1 : Vec2{node().uv[2], node().uv[3]};
    frame.paint.ImageOpaque(path, ScreenRect(), a, b);
    frame.paint.PopMask();
}

// --- Selectable area -----------------------------------------------------------------------------

void SelectableArea::Created(Interface& interface) {
    Static::Created(interface);
    interactive = true;
}

void SelectableArea::Draw(Frame& frame) {
    const int style = textStyle;
    if (selected) textStyle |= kTextBold;
    Static::Draw(frame);
    textStyle = style;
}

void SelectableArea::Mouse(Frame& frame) {
    if (frame.input.Pressed(MouseButton::Left) && frame.pointer.hovered == this) Notify(Notice::Kind::Clicked);
}

// --- Button --------------------------------------------------------------------------------------

void Button::Created(Interface& interface) {
    Static::Created(interface);
    interactive = true;
}

std::string Button::Face(Frame& frame) const {
    const std::string& base = texture.empty() ? node().ddj : texture;
    if (base.empty() || (behaviour & kNoFaces)) return base;
    if (!enabled) {
        const std::string disabled = Variant(base, "_disable");
        return frame.paint.HasTexture(disabled) ? disabled : base;
    }
    if (down_) return Variant(base, "_press"); // the original takes it without checking
    if (frame.pointer.hovered == this && frame.pointer.held == nullptr) {
        const std::string focus = Variant(base, "_focus");
        if (frame.paint.HasTexture(focus)) return focus;
    }
    return base;
}

void Button::Update(Frame& frame) {
    if (!enabled || repeat_ < 0.0f || !down_) return;
    repeat_ -= frame.dt;
    if (repeat_ <= 0.0f) {
        repeat_ += 0.05f; // then every 50 ms
        Notify(Notice::Kind::Clicked);
    }
}

void Button::Mouse(Frame& frame) {
    const Input& in = frame.input;
    const bool over = frame.pointer.hovered == this;
    const bool held = frame.pointer.held == this && frame.pointer.heldButton == MouseButton::Left;
    if (!enabled) return;
    if (held && in.Pressed(MouseButton::Left)) {
        down_ = true;
        if (behaviour & kRepeat) {
            Notify(Notice::Kind::Clicked); // one at once, then after a second
            repeat_ = 1.0f;
        }
        return;
    }
    if (held && in.Down(MouseButton::Left)) {
        // Off the button the press looks undone (and a repeat pauses); back on, pressed again.
        down_ = over;
        if ((behaviour & kDragNotify) && in.mouseDelta().x != 0.0f) Notify(Notice::Kind::Clicked);
        else if ((behaviour & kDragNotify) && in.mouseDelta().y != 0.0f) Notify(Notice::Kind::Clicked);
        return;
    }
    if (held && in.Released(MouseButton::Left)) {
        down_ = false;
        const bool repeating = (behaviour & kRepeat) != 0;
        repeat_ = -1.0f;
        if (over && !repeating) {
            Notify(Notice::Kind::Clicked); // the command first, then the sound
            frame.interface.Sound("SND_BUTTON_CLICK");
        }
    }
}

void Button::Draw(Frame& frame) {
    const float opacity = Opacity();
    if ((background >> 24) != 0) frame.paint.Fill(ScreenRect(), WithAlpha(background, float(background >> 24) / 255.0f * opacity));
    FitToTexture(frame.paint, texture.empty() ? node().ddj : texture);
    const std::string face = Face(frame);
    if (!face.empty()) {
        frame.paint.Image(face, ScreenRect(), {node().uv[0], node().uv[1]}, {node().uv[2], node().uv[3]}, WithAlpha(kWhite, opacity));
    }
    // The caption stays put when pressed; a disabled one is light grey.
    const Color color = enabled ? textColor : Rgba(230, 230, 230);
    Rect r = ScreenRect();
    if (node().hasClientRect) {
        const auto& c = node().clientRect;
        r = {r.x + c.x, r.y + c.y, r.w - c.x - c.w, r.h - c.y - c.h};
    }
    DrawLabel(frame.paint, text(), r, WithAlpha(color, opacity), node().fontIndex, node().hAlign, node().vAlign, textStyle);
}

// --- Close button --------------------------------------------------------------------------------

void CloseButton::Created(Interface& interface) {
    Button::Created(interface);
    if (node().ddj.empty()) texture = "interface\\ifcommon\\com_windowclose.ddj";
}

void CloseButton::Mouse(Frame& frame) {
    Button::Mouse(frame); // the window's frame closes it (its click, id 2)
}

// --- Check box -----------------------------------------------------------------------------------

void CheckBox::Created(Interface&) {
    // Its own layout's static (id 0) is the tick; checked = the tick shown. Boxes start checked.
    interactive = true;
    if (node().style & 0x40) visible = false;
}

void CheckBox::Draw(Frame& frame) {
    if (Widget* tick = Child(0)) tick->visible = checked;
    // Disabled boxes are half transparent.
    alpha = enabled ? 1.0f : 128.0f / 255.0f;
    // The box: the layout's art, else the one the check box loads itself.
    std::string box = node().ddj.empty() ? std::string("interface\\ifcommon\\com_checkbutton_off.ddj") : node().ddj;
    // A res_ui (.2dt) box's art is the checked face itself ("..._on.ddj"); unchecked, its "_off" twin (the class's own tick
    // child, when it made one, follows `checked` above).
    if (!checked) {
        if (const std::size_t at = box.rfind("_on.ddj"); at != std::string::npos && at + 7 == box.size()) box.replace(at, 7, "_off.ddj");
    }
    FitToTexture(frame.paint, box);
    frame.paint.Image(box, ScreenRect(), {node().uv[0], node().uv[1]}, {node().uv[2], node().uv[3]}, WithAlpha(kWhite, Opacity()));
}

void CheckBox::Mouse(Frame& frame) {
    if (!enabled) return;
    if (frame.input.Released(MouseButton::Left) && frame.pointer.hovered == this) {
        checked = !checked;
        Notify(Notice::Kind::Clicked);
    }
}

// --- Radio group ---------------------------------------------------------------------------------

void RadioButton::Created(Interface&) {
    // A group: its units (16 x 16 buttons, ids 100 + i) and their labels (ids 200 + i) are made by
    // the window's code with SetCount; the layout gives only the group.
    if (node().style & 0x40) visible = false;
}

void RadioButton::Draw(Frame& frame) {
    for (const auto& c : children()) {
        if (c->id() >= 100 && c->id() < 110) {
            if (auto* b = As<Button>(c.get())) {
                b->texture = c->id() - 100 == selected ? "interface\\ifcommon\\com_radiobutton_on.ddj"
                                                       : "interface\\ifcommon\\com_radiobutton_off.ddj";
            }
        }
    }
    Widget::Draw(frame);
}

void RadioButton::Mouse(Frame&) {}

void RadioButton::SetUnits(Interface& interface, const std::vector<std::string>& labels, bool row) {
    const std::size_t n = std::min<std::size_t>(labels.size(), 10);
    for (std::size_t i = 0; i < n; ++i) {
        const float x = origin.x + (row ? spacing * float(i) : 0.0f), y = origin.y + (row ? 0.0f : spacing * float(i));
        assets::UiLayoutNode unit;
        unit.type = "CIFButton";
        unit.id = 100 + int(i);
        unit.rect = {x, y, 16, 16};
        auto button = interface.Make(unit);
        if (auto* b = As<Button>(button.get())) b->behaviour = Button::kNoFaces;
        Add(std::move(button));
        assets::UiLayoutNode label;
        label.type = "CIFStatic";
        label.id = 200 + int(i);
        label.rect = {x + labelOffset, y + 3, labelWidth, 12};
        auto text = interface.Make(label);
        text->SetText(labels[i]);
        Add(std::move(text));
    }
}

bool RadioButton::OnNotice(const Notice& notice) {
    // A unit's click selects it and tells the window, with the group's own id.
    if (notice.kind != Notice::Kind::Clicked || notice.source == this || notice.control < 100 || notice.control >= 110) return false;
    selected = notice.control - 100;
    Notify(Notice::Kind::Clicked, selected);
    return true;
}

// --- Drag strip ----------------------------------------------------------------------------------

void DragArea::Created(Interface&) {
    interactive = true;
    SendToBack();
}

void DragArea::Mouse(Frame& frame) {
    // Held: the window follows the mouse by the distance it moved.
    if (frame.pointer.held != this || frame.pointer.heldButton != MouseButton::Left) return;
    Widget* window = Root();
    if (frame.input.Pressed(MouseButton::Left)) {
        grab_ = {frame.pointer.position.x - window->rect.x, frame.pointer.position.y - window->rect.y};
        frame.interface.Raise(window);
        return;
    }
    if (frame.input.Down(MouseButton::Left) && !fixed) {
        window->rect.x = std::round(frame.pointer.position.x - grab_.x);
        window->rect.y = std::round(frame.pointer.position.y - grab_.y);
    }
}

} // namespace sr::ui
