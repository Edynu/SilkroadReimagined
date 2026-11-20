// Values: gauges, scroll bars, the scroll manager, spinners.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

namespace sr::ui {

namespace {

// The original eases on its update tick; taken as 60 a second (its rate was not traced).
constexpr float kTicksPerSecond = 60.0f;

} // namespace

// --- Gauge ---------------------------------------------------------------------------------------

void Gauge::SetValue(float value, bool instant) {
    target_ = std::min(value, 1.0f); // the original clamps the top only
    if (instant) shown_ = target_;
}

void Gauge::Update(Frame& frame) {
    if (shown_ == target_) return;
    // Each tick the shown value moves 10 x speed of the gap (10% at the default 0.01), snapping
    // when it reaches or passes the target.
    const float ticks = std::max(1.0f, frame.dt * kTicksPerSecond);
    const float keep = std::pow(std::max(0.0f, 1.0f - 10.0f * speed), ticks);
    const float next = target_ + (shown_ - target_) * keep;
    shown_ = std::abs(next - target_) < 0.001f || (shown_ < target_) != (next < target_) ? target_ : next;
}

void Gauge::Draw(Frame& frame) {
    const std::string& path = texture.empty() ? node().ddj : texture;
    const Rect r = ScreenRect();
    const float* uv = node().uv;
    const float opacity = Opacity();
    // Left to right, the texture cropped with the value (not squeezed).
    const auto fill = [&](float ratio, float alpha) {
        ratio = std::clamp(ratio, 0.0f, 1.0f);
        if (ratio <= 0.0f) return;
        frame.paint.Image(path, {r.x, r.y, r.w * ratio, r.h}, {uv[0], uv[1]}, {uv[0] + (uv[2] - uv[0]) * ratio, uv[3]}, WithAlpha(kWhite, alpha * opacity));
    };
    if (!rest.empty() && shown_ < 1.0f) {
        const float from = std::clamp(shown_, 0.0f, 1.0f);
        frame.paint.Image(rest, {r.x + r.w * from, r.y, r.w * (1.0f - from), r.h}, {uv[0] + (uv[2] - uv[0]) * from, uv[1]}, {uv[2], uv[3]},
                          WithAlpha(kWhite, opacity));
    }
    if (backgroundValue >= 0.0f) fill(backgroundValue, 0x40 / 255.0f);
    if (shown_ != target_) {
        fill(std::max(shown_, target_), 0x60 / 255.0f);
        fill(std::min(shown_, target_), 1.0f);
    } else {
        fill(shown_, 1.0f);
    }
    DrawText(frame, textColor);
}

// --- Scroll bar ----------------------------------------------------------------------------------

void ScrollBar::Created(Interface&) {
    // Its own layout gave the up arrow (0), the down arrow (1) and the thumb (2); the track is
    // com_scroll_bar, the thumb's travel the bar's height. The arrows repeat while held; the thumb
    // follows the mouse while held.
    if (auto* up = As<Button>(Child(0))) up->behaviour = Button::kRepeat;
    if (auto* down = As<Button>(Child(1))) down->behaviour = Button::kRepeat;
    if (auto* thumb = As<Button>(Child(2))) thumb->behaviour = Button::kDragNotify;
    track = rect.h;
    SetRange(0, 100, 1);
}

void ScrollBar::SetRange(int min, int max, int step) {
    min_ = min;
    max_ = std::max(min, max);
    step_ = std::max(1, step);
    position_ = min_; // a new range starts at its beginning
    PlaceChildren();
}

void ScrollBar::SetPosition(int position) {
    position_ = std::clamp(position, min_, max_);
    PlaceChildren();
    Notify(Notice::Kind::ScrollChanged, position_); // on every call, as the original
}

void ScrollBar::PlaceChildren() {
    const int range = max_ - min_;
    if (Widget* thumb = Child(2)) thumb->rect.y = range > 0 ? float((position_ - min_) * int(track) / range) : 0.0f;
    if (Widget* down = Child(1)) down->rect.y = track + 16.0f;
    if (Widget* up = Child(0)) up->rect.y = -16.0f;
}

bool ScrollBar::OnNotice(const Notice& notice) {
    if (notice.source == this || notice.kind != Notice::Kind::Clicked) return false;
    if (notice.source->parent() != this) return false;
    if (notice.control == 0) SetPosition(position_ - step_);
    if (notice.control == 1) SetPosition(position_ + step_);
    return notice.control <= 2;
}

void ScrollBar::Update(Frame& frame) {
    // Dragging the thumb: the value under the mouse, the thumb snapping to its place.
    Widget* thumb = Child(2);
    if (thumb == nullptr || frame.pointer.held != thumb || frame.input.mouseDelta().y == 0.0f) return;
    const int range = max_ - min_;
    const float h = rect.h;
    if (range <= 0 || h <= 0.0f) return;
    const float y = frame.pointer.position.y - Origin().y - 10.0f + h / float(2 * range);
    SetPosition(min_ + int(std::trunc(y * float(range) / h)));
}

void ScrollBar::Draw(Frame& frame) {
    // The track stretched from the top of the bar over its travel and the thumb's height.
    const Rect r = ScreenRect();
    frame.paint.Image("interface\\ifcommon\\com_scroll_bar.ddj", {r.x, r.y, 16.0f, track + 16.0f}, {0, 0}, {1, 1},
                      WithAlpha(kWhite, Opacity()));
}

// --- Spinner -------------------------------------------------------------------------------------

void Spin::Draw(Frame& frame) {
    // Its own art, stretched over it.
    frame.paint.Image("interface\\store\\str_slot_02.ddj", ScreenRect(), {0, 0}, {1, 1}, WithAlpha(kWhite, Opacity()));
}

bool Spin::OnNotice(const Notice& notice) {
    if (notice.source == this || notice.kind != Notice::Kind::Clicked || notice.source->parent() != this) return false;
    if (notice.control == 1 && value > 0) --value;
    if (notice.control == 2 && value < max) ++value;
    if (notice.control == 1 || notice.control == 2) Notify(Notice::Kind::Clicked);
    return notice.control == 1 || notice.control == 2;
}

// --- The option bar -----------------------------------------------------------------------------

void HSlider::Created(Interface&) {
    interactive = true;
    // Its layout (ifhscroll_option.txt) gives the left arrow (0) at -21, the thumb (2) and the right
    // arrow (1) at 186, for a narrower bar: the right arrow goes to the bar's end, where the board
    // under it (opt_volume.ddj) has its arrow, as the left one sits on the board's left arrow.
    if (auto* left = As<Button>(Child(0))) left->behaviour = Button::kRepeat;
    if (auto* right = As<Button>(Child(1))) {
        right->behaviour = Button::kRepeat;
        right->rect.x = rect.w - 3.0f;
    }
}

void HSlider::SetValue(int v) { value = std::clamp(v, min, max); }

void HSlider::Update(Frame&) {
    // The layout's thumb shows the value (the bar takes the drag: the thumb lets the mouse through).
    auto* thumb = As<Static>(Child(2));
    if (thumb == nullptr) return;
    thumb->interactive = false;
    const float range = float(std::max(1, max - min));
    thumb->rect = {std::round((rect.w - 16.0f) * float(value - min) / range), std::floor((rect.h - 16.0f) / 2.0f), 16.0f, 16.0f};
    thumb->texture = enabled ? std::string() : std::string("interface\\ifcommon\\com_scroll_bar_disable.ddj");
}

bool HSlider::OnNotice(const Notice& notice) {
    if (notice.source == this || notice.kind != Notice::Kind::Clicked || notice.source->parent() != this) return false;
    if (notice.control != 0 && notice.control != 1) return false;
    const int v = std::clamp(value + (notice.control == 0 ? -1 : 1), min, max);
    if (v != value) {
        value = v;
        Notify(Notice::Kind::ValueChanged, value);
    }
    return true;
}

void HSlider::Draw(Frame& frame) {
    if (Child(2) != nullptr) return; // the layout's thumb shows it
    // Without its layout: the thumb (16 x 16) on the bar at its value (the common scroll button).
    const Rect r = ScreenRect();
    const float range = float(std::max(1, max - min));
    const float x = r.x + (r.w - 16.0f) * float(value - min) / range;
    frame.paint.Image(enabled ? "interface\\ifcommon\\com_scroll_button.ddj" : "interface\\ifcommon\\com_scroll_bar_disable.ddj",
                      {x, r.y + (r.h - 16.0f) / 2.0f, 16.0f, 16.0f}, {0, 0}, {1, 1}, WithAlpha(kWhite, Opacity()));
}

void HSlider::Mouse(Frame& frame) {
    if (!enabled) return;
    const Input& in = frame.input;
    if (in.Pressed(MouseButton::Left) && frame.pointer.hovered == this) dragging_ = true;
    if (!in.Down(MouseButton::Left)) dragging_ = false;
    if (!dragging_) return;
    const Rect r = ScreenRect();
    const float t = std::clamp((frame.pointer.position.x - r.x - 8.0f) / std::max(1.0f, r.w - 16.0f), 0.0f, 1.0f);
    const int v = min + int(std::lround(t * float(max - min)));
    if (v != value) {
        value = v;
        Notify(Notice::Kind::ValueChanged, value);
    }
}

void Spin::Update(Frame&) {
    if (Widget* page = Child(0)) page->SetText(std::to_string(value + 1));
}

// --- The slider ---------------------------------------------------------------------------------

void Slider::Created(Interface& interface) {
    Static::Created(interface);
    interactive = true;
    for (int id : {1, 2}) {
        if (auto* b = As<Button>(Child(id))) b->behaviour = Button::kRepeat;
    }
    if (Widget* thumb = Child(3)) thumb->interactive = false; // the track takes the drag
}

void Slider::SetValue(int v) { value = std::clamp(v, min, std::max(min, max)); }

void Slider::Update(Frame&) {
    Widget* back = Child(1);
    Widget* forward = Child(2);
    Widget* thumb = Child(3);
    if (placeForward && back != nullptr && forward != nullptr && forward->rect.w > 0) forward->rect.x = rect.w - back->rect.x - forward->rect.w;
    if (thumb == nullptr) return;
    // The track takes the press and the drag, the thumb's too: a thumb made as a button turns itself interactive as
    // it is made, after Created (the auto potion's: its drag went to the button and moved nothing; 2026-11-13).
    thumb->interactive = false;
    const float left = back != nullptr ? back->rect.x + back->rect.w : 0.0f, right = forward != nullptr ? forward->rect.x : rect.w;
    const float travel = std::max(0.0f, right - left - thumb->rect.w);
    const float t = max > min ? float(value - min) / float(max - min) : 0.0f;
    thumb->rect.x = std::round(left + t * travel);
}

bool Slider::OnNotice(const Notice& notice) {
    if (notice.source == this || notice.kind != Notice::Kind::Clicked || notice.source->parent() != this) return false;
    if (notice.control != 1 && notice.control != 2) return false;
    const int v = std::clamp(value + (notice.control == 1 ? -step : step), min, std::max(min, max));
    if (v != value) {
        value = v;
        Notify(Notice::Kind::ValueChanged, value);
    }
    return true;
}

void Slider::Mouse(Frame& frame) {
    if (!enabled) return;
    const Input& in = frame.input;
    if (in.Pressed(MouseButton::Left) && frame.pointer.hovered == this) dragging_ = true;
    if (!in.Down(MouseButton::Left)) dragging_ = false;
    if (!dragging_) return;
    const Widget* back = Child(1);
    const Widget* forward = Child(2);
    const Widget* thumb = Child(3);
    const float thumbW = thumb != nullptr ? thumb->rect.w : 0.0f;
    const float left = back != nullptr ? back->rect.x + back->rect.w : 0.0f, right = forward != nullptr ? forward->rect.x : rect.w;
    const float travel = std::max(1.0f, right - left - thumbW);
    const Rect r = ScreenRect();
    const float t = std::clamp((frame.pointer.position.x - r.x - left - thumbW * 0.5f) / travel, 0.0f, 1.0f);
    const int v = min + int(std::lround(t * float(max - min)));
    if (v != value) {
        value = v;
        Notify(Notice::Kind::ValueChanged, value);
    }
}

// --- The vertical spinner -------------------------------------------------------------------------

void VerticalSpin::Created(Interface& interface) {
    const auto make = [&](const char* type, int id, Rect r, const char* ddj) {
        assets::UiLayoutNode node;
        node.type = type;
        node.id = id;
        node.rect = {r.x, r.y, r.w, r.h};
        node.ddj = ddj;
        node.hAlign = 1;
        node.vAlign = 1;
        return interface.Make(node);
    };
    // The box (black inside), the number over it, the arrows at the right.
    const float boxW = std::max(0.0f, rect.w - 20.0f);
    auto box = make("CIFStretchWnd", 0, {0, 0, boxW, rect.h}, "interface\\ifcommon\\com_blacksquare_");
    box->background = Argb(0xFF000000);
    Add(std::move(box));
    Add(make("CIFStatic", 3, {0, 0, boxW, rect.h}, ""));
    auto up = make("CIFButton", 1, {boxW, 0, 20, 12}, "interface\\underbar\\ub_up_arrow.ddj");
    auto down = make("CIFButton", 2, {boxW, rect.h - 12.0f, 20, 12}, "interface\\underbar\\ub_down_arrow.ddj");
    if (auto* b = As<Button>(up.get())) b->behaviour = Button::kRepeat;
    if (auto* b = As<Button>(down.get())) b->behaviour = Button::kRepeat;
    Add(std::move(up));
    Add(std::move(down));
}

void VerticalSpin::SetValue(int v) { value = std::clamp(v, min, std::max(min, max)); }

void VerticalSpin::Update(Frame&) {
    Widget* number = Child(3);
    if (number == nullptr) return;
    char text[32];
    std::snprintf(text, sizeof text, "%.*f", std::clamp(decimals, 0, 4), double(value) / double(divider != 0.0f ? divider : 1.0f));
    if (number->text() != text) number->SetText(text);
}

bool VerticalSpin::OnNotice(const Notice& notice) {
    if (notice.source == this || notice.kind != Notice::Kind::Clicked || notice.source->parent() != this) return false;
    if (notice.control != 1 && notice.control != 2) return false;
    const int v = std::clamp(value + (notice.control == 1 ? -step : step), min, std::max(min, max));
    if (v != value) {
        value = v;
        Notify(Notice::Kind::ValueChanged, value);
    }
    return true;
}

} // namespace sr::ui
