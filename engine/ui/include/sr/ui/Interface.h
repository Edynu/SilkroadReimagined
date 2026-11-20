#pragma once

// The window manager of the original's interface (its CGInterface): the
// top-level windows built from layout sections, front to back; which widget the mouse is over
// and which one holds it; every window drawn as a layer of the UI context (so the world below and
// the developer tools above see the mouse correctly).

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sr/assets/UiLayout.h"
#include "sr/ui/Context.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Painter.h"
#include "sr/ui/Widget.h"

namespace sr::ui {

class Interface {
public:
    // A layout by its path ("resinfo\\ifunderbar.txt"); null when it is not loaded.
    using LayoutLookup = std::function<const assets::UiLayout*(const std::string& path)>;
    using Factory = std::function<std::unique_ptr<Widget>(const assets::UiLayoutNode&)>;

    Interface(Resources& resources, LayoutLookup layouts);
    ~Interface();

    // The class a layout node names (CIFButton, ...) and how to make it; unknown classes become
    // plain widgets (their texture and text).
    void Register(std::string type, Factory factory);
    // A widget of the node's class with its whole subtree (nested layouts, the children its class
    // makes in code); for controls that make controls.
    std::unique_ptr<Widget> Make(const assets::UiLayoutNode& node);

    // A top-level window from a layout section (empty: the layout's first), at `place` (interface
    // pixels; the section's own rect when it has a size). Null when the layout is missing.
    Widget* Open(const std::string& layoutPath, const std::string& section = {}, int id = 0, Vec2 place = {});
    // A top-level window from a node of another layout (as ginterface.txt's nodes make the HUD).
    Widget* Open(const assets::UiLayoutNode& node);
    // Hides it now and deletes it next frame. Opening puts a window in front; so does dragging it.
    void Close(Widget* window);
    void CloseAll();
    void Raise(Widget* window);
    // Windows raised again, in this order, after every open and every raise: always in front of
    // the others (the HUD manager's set: the system menu, the game guide, the tooltip window, the
    // notices...).
    void KeepOnTop(std::vector<int> ids);
    // A control's pop-up (a combo box's list): a top-level window in front of every other one (its
    // own layer: not clipped by the owner's window), one at a time. It closes when another opens,
    // when its owner is no longer shown (its page hidden, scrolled away, its window closed) or gone
    // (the owner calls Disown when destroyed), and on a press outside both it and its owner; that
    // press reaches nothing else (the original's list holds the mouse until a row is picked or the
    // mouse is let go outside it).
    Widget* OpenPopup(Widget& owner, std::unique_ptr<Widget> popup);
    void ClosePopup();
    Widget* popup() const { return popup_; }
    Widget* popupOwner() const { return popupOwner_; }
    void Disown(const Widget* owner) {
        if (owner == popupOwner_) popupOwner_ = nullptr;
    }
    // Back to front.
    std::vector<Widget*> Windows() const;
    // The top-level window with a layout id, if open.
    Widget* Window(int id) const;

    // Places kept by window id: a window closed remembers where
    // it was; Place puts it back there, pulled inside the screen, or centres it when it has none.
    void Remember(const Widget& window);
    void Place(Widget& window, Vec2 screen);
    void Centre(Widget& window, Vec2 screen);
    // Every window whose left edge is within 24 pixels of the right (or top of the bottom) is
    // pulled back to end at that edge, as the original does when the HUD is placed.
    void PullOntoScreen(Vec2 screen);
    // The kept places as text ("width height" then "id x y" lines), and back; read only for the
    // same screen size, as the original's Setting\wndpos.dat.
    std::string SavePlaces(Vec2 screen) const;
    void LoadPlaces(std::string_view text, Vec2 screen);

    // One frame: the mouse, the updates and the drawing; each window draws into its own layer.
    void Frame(Context& context, float scale, double time, float dt);
    // Every texture the interface asked for has streamed in (or failed).
    bool Settled() const { return settled_; }
    // The screen in interface pixels (as of the last frame; windows that place themselves by it).
    Vec2 screen() const { return screen_; }

    Resources& resources() { return resources_; }
    // The context of the frame running (its clipboard; null outside a frame).
    Context* context() const { return frameContext_; }
    const assets::UiLayout* Layout(const std::string& path) const { return layouts_(path); }
    // The node's text key resolved through the text table (a literal stays as it is).
    std::string TextOf(const assets::UiLayoutNode& node) const;
    const Pointer& pointer() const { return pointer_; }
    // The widget the keyboard goes to (an edit); a press elsewhere takes it away.
    void Focus(Widget* widget) { focus_ = widget; }
    Widget* focus() const { return focus_; }
    // The item picked up from a slot (the original's "focus"): dropped on the next left release.
    void Pick(Widget* source) { picked_ = source; }
    Widget* picked() const { return picked_; }
    // The deepest widget of any kind under the mouse this frame (null: the world, no window there):
    // where a held item was let go.
    Widget* under() const { return under_; }
    // A held item was let go (or put back) this frame: that release is not a click on the world.
    bool pickEnded() const { return pickEnded_; }
    // The tooltip for this frame, beside `owner` (interface pixels); call while hovering.
    void ShowTooltip(std::string text, Rect owner);
    void ShowTooltip(std::vector<TooltipLine> lines, Rect owner, std::string sign);
    // The tooltip the last frame showed (empty: none).
    const std::vector<TooltipLine>& tooltip() const { return tooltip_; }
    // A tooltip of something outside the windows (an object of the world under the mouse): set before the frame, shown
    // in it when no window shows one of its own, then dropped.
    void HoldTooltip(std::vector<TooltipLine> lines, Rect owner) {
        heldTooltip_ = std::move(lines);
        heldOwner_ = owner;
    }

    // The interface's sounds by their effectsound.txt handle (SND_BUTTON_CLICK, SND_WINDOW_OPEN,
    // SND_WINDOW_CLOSE): who plays them (the client's audio)
    // listens here; nobody by default.
    std::function<void(std::string_view handle)> onSound;
    void Sound(std::string_view handle) const {
        if (onSound) onSound(handle);
    }

private:
    struct Top {
        std::unique_ptr<Widget> window;
        std::string layer; // the UI context's layer name
        // Shown whole or not at all (ours): since when it is shown (-1: hidden), and whether it has been drawn with
        // every picture in (or kRevealHold passed) since.
        double shownAt = -1.0;
        bool revealed = false;
    };
    // The longest a window just shown waits, unseen, for its pictures (then it shows with what is in): under the
    // ~150 ms a click's answer starts to feel slow.
    static constexpr double kRevealHold = 0.12;
    std::unique_ptr<Widget> Build(const assets::UiLayoutNode& node, int depth);
    void CallCreated(Widget& w);
    Widget* AddWindow(std::unique_ptr<Widget> window);
    void RaiseOnTop();
    void AddChildren(Widget& parent, const assets::UiLayoutSection& section, int depth);
    Widget* HitTest(Widget& w, Vec2 p, bool interactiveOnly);
    void UpdateTree(Widget& w, ui::Frame& frame);
    void DrawTree(Widget& w, ui::Frame& frame);
    // After widgets were removed (Widget::Removals moved on): every pointer kept at a widget that is no longer in a
    // window let go (a window rebuilding its controls under the mouse: the COS bar as a horse is sent away).
    void ForgetRemoved();
    std::uint64_t removalsSeen_ = 0;

    Resources& resources_;
    LayoutLookup layouts_;
    std::unordered_map<std::string, Factory> factories_;
    std::vector<Top> windows_; // back to front
    std::vector<int> onTop_;
    Pointer pointer_;
    Widget* under_ = nullptr;
    bool pickEnded_ = false;
    Widget* focus_ = nullptr;
    Widget* picked_ = nullptr;
    Widget* popup_ = nullptr;
    Widget* popupOwner_ = nullptr;
    std::vector<Widget*> closing_;
    std::vector<TooltipLine> tooltip_;
    std::vector<TooltipLine> heldTooltip_;
    Rect heldOwner_;
    std::string tooltipSign_;
    Rect tooltipOwner_;
    std::unordered_map<int, Vec2> places_;
    bool settled_ = false;
    int nextLayer_ = 0;
    Vec2 screen_{1024, 768};
    Context* frameContext_ = nullptr;
};

} // namespace sr::ui
