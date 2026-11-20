#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sr/ui/DrawList.h"
#include "sr/ui/Input.h"

namespace sr::ui {

class Font;

using Id = std::uint64_t;
// FNV-1a; `seed` chains ids (a control inside a window).
Id HashId(std::string_view text, Id seed = 1469598103934665603ull);

// Back to front: the world's background, the fixed HUD, opened windows (the last
// clicked on top), overlays (tooltips, the dragged item, fades).
enum class LayerKind : std::uint8_t { Background = 0, Hud = 1, Window = 2, Overlay = 3 };

// The game UI's immediate-mode state: layers (one per window) with their draw
// lists and z-order, which layer the mouse is over, the control being pressed
// (it keeps the mouse until release), the control with the keyboard (a text field).
class Context {
public:
    // Call before drawing. `mouseBlocked` / `keyboardBlocked`: something above the game
    // UI (the developer tools) has them this frame.
    void NewFrame(const Input& input, Vec2 screenSize, bool mouseBlocked, bool keyboardBlocked);
    // Call after drawing; then DrawOrder() lists what to render.
    void EndFrame();

    // A layer: everything drawn until EndLayer() goes into its draw list. Names are unique.
    void BeginLayer(std::string_view name, LayerKind kind);
    void EndLayer();
    // Parts of the current layer that take the mouse; a layer without any never does,
    // so the world sees clicks on its empty corners. Rects are pixels.
    void Solid(Rect rect);
    DrawList& draw();
    // The current layer to the front of its kind (a window on click).
    void Raise();
    // The current layer's place among its kind, set by an owner that keeps its own order (the
    // interface's windows); larger is further front.
    void SetZ(int z);
    // The layer under the mouse (from last frame's solid parts); 0 for none.
    Id hoveredLayer() const { return hoveredLayer_; }
    // The layers drawn this frame, back to front.
    std::vector<const DrawList*> DrawOrder() const;

    // Control ids are scoped by the layer and the pushed ids.
    void PushId(std::string_view id);
    void PushId(int id);
    void PopId();
    Id GetId(std::string_view label) const;

    struct ItemState {
        bool hovered = false;  // the mouse is over it (and nothing else holds the mouse)
        bool pressed = false;  // the left button went down on it this frame
        bool held = false;     // pressed earlier and still down
        bool clicked = false;  // released over it after pressing it
        bool dragging = false; // held and moved more than a few pixels
    };
    // A mouse-interactive rect (pixels).
    ItemState Item(Id id, Rect rect, bool enabled = true);
    ItemState Item(std::string_view label, Rect rect, bool enabled = true) { return Item(GetId(label), rect, enabled); }
    Id activeId() const { return active_; }

    // The mouse is over a UI layer (not the background) or a control holds it.
    bool mouseOverUi() const;
    // The mouse is over a UI layer's solid part (not counting a control that holds it).
    bool mouseOverLayer() const { return hoveredLayer_ != 0 && hoveredKind_ != LayerKind::Background; }
    // The world may use the mouse: not blocked, not over the UI, no control holding it.
    bool mouseFree() const { return !mouseBlocked_ && !mouseOverUi(); }
    const Input& input() const { return *input_; }
    // A key for the game (false while a text field has the keyboard or it is blocked).
    bool KeyPressed(Key key, bool repeat = false) const;
    Vec2 screen() const { return screen_; }

    // Keyboard focus, for text fields.
    void SetFocus(Id id) { focus_ = id; }
    void ClearFocus() { focus_ = 0; }
    bool HasFocus(Id id) const { return focus_ == id && focus_ != 0; }
    bool typing() const { return focus_ != 0; }
    // The focused text field's rect and caret x (pixels), for the IME's candidate list (it opens
    // under the typed text, as the original's).
    void SetTextArea(Rect area, float caretX) {
        textArea_ = area;
        textCaret_ = caretX;
    }
    Rect textArea() const { return textArea_; }
    float textCaret() const { return textCaret_; }
    bool keyboardBlocked() const { return keyboardBlocked_; }

    // Set by the application.
    Font* font = nullptr;
    std::function<std::string()> getClipboard;
    std::function<void(const std::string&)> setClipboard;

private:
    struct Layer {
        Id id = 0;
        LayerKind kind = LayerKind::Hud;
        int z = 0;
        std::vector<Rect> solid;     // last frame's, for this frame's hover test
        std::vector<Rect> nextSolid; // this frame's
        DrawList list;
        std::uint64_t frame = 0;     // last frame it was drawn
    };

    const Input* input_ = nullptr;
    Input empty_;
    Vec2 screen_;
    bool mouseBlocked_ = false, keyboardBlocked_ = false;
    Rect textArea_;
    float textCaret_ = 0.0f;
    std::uint64_t frame_ = 0;
    std::unordered_map<Id, Layer> layers_;
    Layer* current_ = nullptr;
    std::vector<Id> idStack_;
    Id hoveredLayer_ = 0;
    LayerKind hoveredKind_ = LayerKind::Background;
    Id active_ = 0;
    bool activeSeen_ = false;
    Id focus_ = 0;
    int nextZ_ = 1;
};

} // namespace sr::ui
