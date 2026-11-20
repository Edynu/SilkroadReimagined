#include "sr/ui/Widget.h"

#include <algorithm>

#include "sr/ui/Interface.h"

namespace sr::ui {

Widget::Widget(const assets::UiLayoutNode& node) : node_(node) {
    rect = {node.rect.x, node.rect.y, node.rect.w, node.rect.h};
}

Widget::~Widget() = default;

Widget* Widget::Add(std::unique_ptr<Widget> child) {
    child->parent_ = this;
    children_.push_back(std::move(child));
    return children_.back().get();
}

void Widget::SendToBack() {
    if (parent_ == nullptr) return;
    auto& siblings = parent_->children_;
    const auto it = std::find_if(siblings.begin(), siblings.end(), [&](const auto& c) { return c.get() == this; });
    if (it == siblings.end() || it == siblings.begin()) return;
    std::rotate(siblings.begin(), it, it + 1);
}

void Widget::BringToFront() {
    if (parent_ == nullptr) return;
    auto& siblings = parent_->children_;
    const auto it = std::find_if(siblings.begin(), siblings.end(), [&](const auto& c) { return c.get() == this; });
    if (it == siblings.end()) return;
    std::rotate(it, it + 1, siblings.end());
}

Widget* Widget::Child(int id) const {
    for (const auto& c : children_) {
        if (c->id() == id) return c.get();
    }
    return nullptr;
}

Widget* Widget::Find(int id) {
    if (node_.id == id) return this;
    for (const auto& c : children_) {
        if (Widget* w = c->Find(id)) return w;
    }
    return nullptr;
}

Widget* Widget::Find(std::string_view name) {
    if (node_.name == name) return this;
    for (const auto& c : children_) {
        if (Widget* w = c->Find(name)) return w;
    }
    return nullptr;
}

Widget* Widget::Root() {
    Widget* w = this;
    while (w->parent_ != nullptr) w = w->parent_;
    return w;
}

Vec2 Widget::Origin() const {
    Vec2 o{rect.x, rect.y};
    for (const Widget* p = parent_; p != nullptr; p = p->parent_) {
        o.x += p->rect.x;
        o.y += p->rect.y;
    }
    return o;
}

bool Widget::ShownOnScreen() const {
    for (const Widget* w = this; w != nullptr; w = w->parent_) {
        if (!w->visible) return false;
    }
    return true;
}

void Widget::Notify(Notice::Kind kind, int value) {
    const Notice notice{kind, node_.id, this, value};
    for (Widget* w = this; w != nullptr; w = w->parent_) {
        if (w->OnNotice(notice)) return;
    }
}

float Widget::Opacity() const {
    float a = 1.0f;
    for (const Widget* w = this; w != nullptr; w = w->parent_) a *= w->alpha;
    return a;
}

void Widget::Draw(Frame& frame) {
    if ((background >> 24) != 0) frame.paint.Fill(ScreenRect(), WithAlpha(background, float(background >> 24) / 255.0f * Opacity()));
    DrawImage(frame, WithAlpha(kWhite, Opacity()));
    // A plain window draws no text (only statics do).
}

void Widget::FitToTexture(Painter& paint, const std::string& path) {
    if ((rect.w > 0.0f && rect.h > 0.0f) || path.empty()) return;
    const Vec2 s = paint.TextureSize(path);
    if (s.x <= 0.0f || s.y <= 0.0f) return;
    if (rect.w <= 0.0f) rect.w = s.x;
    if (rect.h <= 0.0f) rect.h = s.y;
}

void Widget::DrawImage(Frame& frame, Color tint) {
    if (node_.ddj.empty()) return;
    FitToTexture(frame.paint, node_.ddj);
    frame.paint.Image(node_.ddj, ScreenRect(), {node_.uv[0], node_.uv[1]}, {node_.uv[2], node_.uv[3]}, tint);
}

void Widget::DrawText(Frame& frame, Color color) {
    if (text_.empty()) return;
    Rect r = ScreenRect();
    if (node_.hasClientRect) {
        // ClientRect holds insets: left, top, right, bottom.
        r = {r.x + node_.clientRect.x, r.y + node_.clientRect.y, r.w - node_.clientRect.x - node_.clientRect.w,
             r.h - node_.clientRect.y - node_.clientRect.h};
    }
    frame.paint.Text(text_, r, color, node_.fontIndex, node_.hAlign, node_.vAlign);
}

} // namespace sr::ui
