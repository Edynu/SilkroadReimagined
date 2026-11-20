#pragma once

// The original's interface as a tree of controls built from its layouts:
// each node of a resinfo layout becomes a widget of its class (CIFStatic, CIFButton, ...), placed
// relative to its parent in interface pixels; a node whose class is a window with a layout of its
// own nests that window. Widgets draw themselves through a Painter and tell their window what
// happened to them (a click, a change) as notices; what a window does with them is its behaviour.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sr/assets/UiLayout.h"
#include "sr/ui/Input.h"
#include "sr/ui/Painter.h"
#include "sr/ui/Types.h"

namespace sr::ui {

class Interface;
class Widget;

// What a control tells its window. The original sends (id, code) up the tree: the sender's own
// handler first, then its parent's, until one takes it (1/0 clicked, 2/1 scroll
// position, 2/0x806B value, 2/0x806D row picked, 2/0x806E row double-clicked, 2/0x806F combo picked,
// 1/2 text changed).
struct Notice {
    enum class Kind : std::uint8_t {
        Clicked,          // 1/0
        RightClicked,
        DoubleClicked,
        TextChanged,      // 1/2
        Submitted,        // Enter in an edit that passes it on
        Cancelled,        // Escape in an edit that passes it on
        ScrollChanged,    // 2/1, value = the position
        ValueChanged,     // 2/0x806B, value = the value
        RowPicked,        // 2/0x806D, value = the row
        RowDoubleClicked, // 2/0x806E
        ComboPicked,      // 2/0x806F, value = the row
    };
    Kind kind = Kind::Clicked;
    int control = -1; // the sender's layout id
    Widget* source = nullptr;
    int value = 0;
};

// The mouse as the tree sees it this frame: which widget is under it (the front-most), which one
// holds it (pressed on it, until every button is up).
struct Pointer {
    Vec2 position;         // interface pixels
    Widget* hovered = nullptr;
    Widget* held = nullptr;
    MouseButton heldButton = MouseButton::Left;
    Vec2 pressPosition;
};

// One frame of work over the tree.
struct Frame {
    Interface& interface;
    Painter& paint;
    const Input& input;
    const Pointer& pointer;
    double time = 0.0;
    float dt = 0.0f;
};

class Widget {
public:
    explicit Widget(const assets::UiLayoutNode& node);
    virtual ~Widget();
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    const assets::UiLayoutNode& node() const { return node_; }
    int id() const { return node_.id; }
    const std::string& name() const { return node_.name; }
    const std::string& type() const { return node_.type; }

    // Relative to the parent, in interface pixels.
    Rect rect;
    bool visible = true;
    bool enabled = true;
    // The widget takes the mouse (a decoration does not, so the control under it does).
    bool interactive = false;
    // It and its children let the mouse through to what is under them, windows and the world
    // (the area banner).
    bool clickThrough = false;
    // Its children drawn and hit only inside its rect (a scrolled list's rows cut at its edge).
    bool clipChildren = false;

    Widget* parent() const { return parent_; }
    const std::vector<std::unique_ptr<Widget>>& children() const { return children_; }
    Widget* Add(std::unique_ptr<Widget> child);
    // Every child gone (a window that builds another section of its layout in their place). Counted: the interface
    // then lets go of whatever it pointed at among them (the control under the mouse, the one held, the focus).
    void ClearChildren() {
        children_.clear();
        ++removals_;
    }
    // How many times children were removed so far (any widget's).
    static std::uint64_t Removals() { return removals_; }
    // A direct child by its layout id.
    Widget* Child(int id) const;
    // Depth-first, this widget included.
    Widget* Find(int id);
    Widget* Find(std::string_view name);
    // The top-level window this widget belongs to (itself for a window).
    Widget* Root();
    // Where the widget is on the interface (its parents' places added up).
    Vec2 Origin() const;
    Rect ScreenRect() const {
        const Vec2 o = Origin();
        return {o.x, o.y, rect.w, rect.h};
    }
    bool ShownOnScreen() const; // it and all its parents visible

    // The text shown (the node's text key resolved, or set by code).
    const std::string& text() const { return text_; }
    void SetText(std::string text) { text_ = std::move(text); }

    // Once, after the tree is built (the original's create: textures, children made in code).
    virtual void Created(Interface&) {}
    bool createdDone = false; // Created has run (the interface sets it)
    virtual void Update(Frame&) {}
    // Its own look, under its children.
    virtual void Draw(Frame& frame);
    // The mouse over it or holding it, once per frame (only for interactive widgets).
    virtual void Mouse(Frame&) {}
    // The keyboard, once per frame while it has the focus (Interface::Focus).
    virtual void Keyboard(Frame&) {}

    // Passes a notice up the tree until a widget takes it (OnNotice returns true).
    void Notify(Notice::Kind kind, int value = 0);
    // A notice from itself or a child; true when handled. By default the onNotice behaviour.
    virtual bool OnNotice(const Notice& notice) {
        if (!onNotice) return false;
        onNotice(notice);
        return true;
    }
    // A window's behaviour: what it does with its controls' notices.
    std::function<void(const Notice&)> onNotice;
    // A held item dropped on it or a child (`target`, the widget under the mouse); true when taken.
    std::function<bool(Widget* source, Widget* target)> onDrop;
    // On the window an item was picked up from: it was let go (dropped, `taker` the widget whose
    // onDrop took it, or null when nothing did); not called when the right button put it back.
    std::function<void(Widget* source, Widget* taker)> onPickEnd;
    // The point is on it (its rect, edges included; a slot's is larger).
    virtual bool Hit(Vec2 p) const {
        const Rect r = ScreenRect();
        return p.x >= r.x && p.y >= r.y && p.x <= r.Right() && p.y <= r.Bottom();
    }
    // To the front of its parent's children list: drawn first, offered clicks last.
    void SendToBack();
    // To the end of its parent's children list: drawn last (over its siblings), offered clicks first.
    void BringToFront();

    // A plain colour under the widget's look (the original's window background; 0 = none).
    Color background = 0;
    // The widget's opacity, multiplied down the tree (the original's window alpha).
    float alpha = 1.0f;
    float Opacity() const;

    // A rect without a width or height takes the texture's (the original sizes such controls to
    // their art when it loads them); once the texture is in.
    void FitToTexture(Painter& paint, const std::string& path);

protected:
    // The node's texture over the widget's rect (its UV_LT..UV_RB part) and its text.
    void DrawImage(Frame& frame, Color tint = kWhite);
    void DrawText(Frame& frame, Color color);

private:
    assets::UiLayoutNode node_;
    std::string text_;
    Widget* parent_ = nullptr;
    std::vector<std::unique_ptr<Widget>> children_;
    static inline std::uint64_t removals_ = 0; // the interface's thread alone builds and changes widgets
};

} // namespace sr::ui
