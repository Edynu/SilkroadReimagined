#include "sr/ui/Context.h"

#include <algorithm>
#include <cmath>

#include "sr/ui/Font.h"

namespace sr::ui {

Id HashId(std::string_view text, Id seed) {
    Id h = seed;
    for (char c : text) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

void Context::NewFrame(const Input& input, Vec2 screenSize, bool mouseBlocked, bool keyboardBlocked) {
    input_ = &input;
    screen_ = screenSize;
    mouseBlocked_ = mouseBlocked;
    keyboardBlocked_ = keyboardBlocked;
    ++frame_;
    current_ = nullptr;
    idStack_.clear();
    activeSeen_ = false;

    // The layer under the mouse: the front-most one drawn last frame whose solid parts contain it.
    hoveredLayer_ = 0;
    hoveredKind_ = LayerKind::Background;
    if (!mouseBlocked_) {
        const Vec2 mouse = input.mouse();
        const Layer* best = nullptr;
        for (const auto& [id, layer] : layers_) {
            if (layer.frame + 1 != frame_) continue;
            const bool over = std::any_of(layer.solid.begin(), layer.solid.end(), [&](const Rect& r) { return r.Contains(mouse); });
            if (!over) continue;
            if (best == nullptr || layer.kind > best->kind || (layer.kind == best->kind && layer.z > best->z)) best = &layer;
        }
        if (best != nullptr) {
            hoveredLayer_ = best->id;
            hoveredKind_ = best->kind;
        }
    }
    // A click raises the window under it.
    if (input.Pressed(MouseButton::Left) && hoveredKind_ == LayerKind::Window) {
        if (auto it = layers_.find(hoveredLayer_); it != layers_.end()) it->second.z = nextZ_++;
    }
}

void Context::EndFrame() {
    if (active_ != 0 && (!activeSeen_ || !input_->Down(MouseButton::Left))) active_ = 0;
    for (auto& [id, layer] : layers_) {
        if (layer.frame != frame_) continue;
        layer.solid = std::move(layer.nextSolid);
        layer.nextSolid.clear();
    }
    // Layers not drawn for a while are forgotten.
    std::erase_if(layers_, [&](const auto& entry) { return entry.second.frame + 600 < frame_; });
    current_ = nullptr;
}

void Context::BeginLayer(std::string_view name, LayerKind kind) {
    const Id id = HashId(name);
    auto [it, inserted] = layers_.try_emplace(id);
    Layer& layer = it->second;
    if (inserted || layer.frame + 1 < frame_) {
        // New (or reopened): on top of its kind.
        layer.z = nextZ_++;
    }
    layer.id = id;
    layer.kind = kind;
    layer.frame = frame_;
    layer.list.Clear();
    layer.nextSolid.clear();
    if (font != nullptr) layer.list.SetWhite(font->texture(), font->whiteUv());
    current_ = &layer;
    idStack_.assign(1, id);
}

void Context::EndLayer() {
    current_ = nullptr;
    idStack_.clear();
}

void Context::Solid(Rect r) {
    if (current_ != nullptr && r.w > 0 && r.h > 0) current_->nextSolid.push_back(r);
}

DrawList& Context::draw() {
    static DrawList orphan; // drawing outside a layer goes nowhere
    if (current_ == nullptr) {
        orphan.Clear();
        return orphan;
    }
    return current_->list;
}

void Context::Raise() {
    if (current_ != nullptr) current_->z = nextZ_++;
}

void Context::SetZ(int z) {
    if (current_ != nullptr) current_->z = z;
}

std::vector<const DrawList*> Context::DrawOrder() const {
    std::vector<const Layer*> drawn;
    for (const auto& [id, layer] : layers_) {
        if (layer.frame == frame_) drawn.push_back(&layer);
    }
    std::sort(drawn.begin(), drawn.end(), [](const Layer* a, const Layer* b) {
        return a->kind != b->kind ? a->kind < b->kind : a->z < b->z;
    });
    std::vector<const DrawList*> out;
    for (const Layer* layer : drawn) out.push_back(&layer->list);
    return out;
}

void Context::PushId(std::string_view id) { idStack_.push_back(HashId(id, idStack_.empty() ? 0 : idStack_.back())); }

void Context::PushId(int id) { PushId(std::to_string(id)); }

void Context::PopId() {
    if (idStack_.size() > 1) idStack_.pop_back();
}

Id Context::GetId(std::string_view label) const { return HashId(label, idStack_.empty() ? 0 : idStack_.back()); }

Context::ItemState Context::Item(Id id, Rect r, bool enabled) {
    ItemState s;
    if (input_ == nullptr || current_ == nullptr) return s;
    const Input& in = *input_;
    const bool layerHovered = current_->id == hoveredLayer_;
    const bool inside = r.Contains(in.mouse());
    if (active_ == id) activeSeen_ = true;
    s.hovered = enabled && layerHovered && inside && (active_ == 0 || active_ == id);
    if (s.hovered && in.Pressed(MouseButton::Left)) {
        active_ = id;
        activeSeen_ = true;
        s.pressed = true;
    }
    if (active_ == id) {
        s.held = in.Down(MouseButton::Left);
        s.clicked = enabled && in.Released(MouseButton::Left) && inside && layerHovered;
        const Vec2 from = in.PressPosition(MouseButton::Left), now = in.mouse();
        s.dragging = s.held && std::hypot(now.x - from.x, now.y - from.y) > 3.0f;
    }
    return s;
}

bool Context::mouseOverUi() const {
    return (hoveredLayer_ != 0 && hoveredKind_ != LayerKind::Background) || active_ != 0;
}

bool Context::KeyPressed(Key key, bool repeat) const {
    if (input_ == nullptr || keyboardBlocked_ || typing()) return false;
    return input_->KeyPressed(key, repeat);
}

} // namespace sr::ui
