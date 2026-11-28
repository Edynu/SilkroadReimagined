#include "sr/ui/Interface.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "sr/ui/ClassLayouts.h"
#include "sr/ui/Controls.h"

namespace sr::ui {

namespace {

// Nested windows (a window class inside another's layout) go this deep at most (the deepest the
// original nests: the main window, the Skills page, its board, the board's list, the list's bar).
constexpr int kMaxDepth = 6;

const assets::UiLayoutSection* FindSection(const assets::UiLayout& layout, const std::string& name) {
    for (const auto& s : layout.sections) {
        if (name.empty() || s.name == name) return &s;
    }
    return nullptr;
}

// A section without a size takes the box around its controls.
Vec2 SectionSize(const assets::UiLayoutSection& section) {
    if (section.rect.w > 0 && section.rect.h > 0) return {section.rect.w, section.rect.h};
    float w = 0, h = 0;
    for (const auto& n : section.nodes) {
        w = std::max(w, n.rect.x + n.rect.w);
        h = std::max(h, n.rect.y + n.rect.h);
    }
    return {w, h};
}

} // namespace

Interface::Interface(Resources& resources, LayoutLookup layouts) : resources_(resources), layouts_(std::move(layouts)) {}

Interface::~Interface() {
    // The windows go while the interface is whole (a control's destructor may disown its pop-up).
    std::vector<Top> windows = std::move(windows_);
    windows_.clear();
    windows.clear();
}

void Interface::Register(std::string type, Factory factory) { factories_[std::move(type)] = std::move(factory); }

std::string Interface::TextOf(const assets::UiLayoutNode& node) const {
    if (node.text.empty()) return {};
    return resources_.Text(node.text);
}

std::unique_ptr<Widget> Interface::Build(const assets::UiLayoutNode& node, int depth) {
    std::unique_ptr<Widget> w;
    if (const auto it = factories_.find(node.type); it != factories_.end()) w = it->second(node);
    if (!w) w = std::make_unique<Widget>(node);
    w->SetText(TextOf(node));
    // A window class with a layout of its own builds its controls from it (its first section).
    if (depth < kMaxDepth) {
        const std::string_view own = LayoutOfClass(w->node().type); // a factory that made another class (a 1.188 CNIF window) opts out
        if (!own.empty()) {
            if (const assets::UiLayout* layout = layouts_(std::string(own))) {
                if (const assets::UiLayoutSection* section = FindSection(*layout, {})) {
                    if (w->rect.w <= 0 || w->rect.h <= 0) {
                        const Vec2 size = SectionSize(*section);
                        w->rect.w = size.x;
                        w->rect.h = size.y;
                    }
                    AddChildren(*w, *section, depth + 1);
                }
            }
        }
    }
    return w;
}

void Interface::AddChildren(Widget& parent, const assets::UiLayoutSection& section, int depth) {
    // The first node of a section is the topmost: the texts of the player's bar sit before its
    // gauges and art in the file, the video options' combo boxes before the opaque board under
    // them (observed in the data; the parser at appends in the order it walks its parsed
    // tree, inferred to be reversed there). Children are kept in paint order, so last node first.
    for (auto it = section.nodes.rbegin(); it != section.nodes.rend(); ++it) parent.Add(Build(*it, depth));
}

std::unique_ptr<Widget> Interface::Make(const assets::UiLayoutNode& node) {
    auto w = Build(node, 0);
    CallCreated(*w);
    return w;
}

void Interface::CallCreated(Widget& w) {
    // Children first made from the layouts; what Created adds is made through Make (and created
    // there). A window may also replace children in its Created (another section of its layout):
    // those are gone and skipped.
    std::vector<Widget*> built;
    std::unordered_set<const Widget*> alive;
    const std::function<void(Widget&)> collect = [&](Widget& x) {
        alive.insert(&x);
        for (const auto& c : x.children()) collect(*c);
    };
    const std::function<void(Widget&)> order = [&](Widget& x) {
        built.push_back(&x);
        for (const auto& c : x.children()) order(*c);
    };
    order(w);
    collect(w);
    for (Widget* x : built) {
        if (!alive.contains(x) || x->createdDone) continue;
        x->createdDone = true;
        x->Created(*this);
        alive.clear();
        collect(w);
    }
}

Widget* Interface::AddWindow(std::unique_ptr<Widget> window) {
    Top top{std::move(window), "interface " + std::to_string(nextLayer_++)};
    windows_.push_back(std::move(top));
    Widget* added = windows_.back().window.get();
    RaiseOnTop();
    return added;
}

void Interface::KeepOnTop(std::vector<int> ids) {
    onTop_ = std::move(ids);
    RaiseOnTop();
}

void Interface::RaiseOnTop() {
    for (int id : onTop_) {
        const auto it = std::find_if(windows_.begin(), windows_.end(), [&](const Top& t) { return t.window->id() == id; });
        if (it == windows_.end() || it + 1 == windows_.end()) continue;
        Top top = std::move(*it);
        windows_.erase(it);
        windows_.push_back(std::move(top));
    }
    // A pop-up is in front of them all.
    const auto it = std::find_if(windows_.begin(), windows_.end(), [&](const Top& t) { return t.window.get() == popup_; });
    if (popup_ != nullptr && it != windows_.end() && it + 1 != windows_.end()) {
        Top top = std::move(*it);
        windows_.erase(it);
        windows_.push_back(std::move(top));
    }
}

Widget* Interface::Open(const std::string& layoutPath, const std::string& sectionName, int id, Vec2 place) {
    const assets::UiLayout* layout = layouts_(layoutPath);
    if (layout == nullptr) return nullptr;
    // A window class's own layout opens as that class, with ginterface.txt's node for it when the
    // master list has one (its size, chrome and title), as the game makes it.
    const std::string_view owner = sectionName.empty() ? ClassOfLayout(layoutPath) : std::string_view{};
    if (!owner.empty()) {
        assets::UiLayoutNode node;
        bool listed = false;
        if (const assets::UiLayout* master = layouts_("resinfo\\ginterface.txt")) {
            for (const auto& s : master->sections) {
                for (const auto& n : s.nodes) {
                    if (!listed && n.type == owner) {
                        node = n;
                        listed = true;
                    }
                }
            }
        }
        if (!listed) {
            node.type = std::string(owner);
            node.name = std::string(owner);
        }
        node.id = id != 0 ? id : node.id;
        node.rect.x = place.x;
        node.rect.y = place.y;
        return AddWindow(Make(node));
    }
    const assets::UiLayoutSection* section = FindSection(*layout, sectionName);
    if (section == nullptr) return nullptr;
    assets::UiLayoutNode root;
    root.name = section->name;
    root.type = "CIFWnd";
    root.id = id;
    const Vec2 size = SectionSize(*section);
    root.rect = {place.x, place.y, size.x, size.y};
    auto window = std::make_unique<Widget>(root);
    AddChildren(*window, *section, 0);
    CallCreated(*window);
    return AddWindow(std::move(window));
}

Widget* Interface::Open(const assets::UiLayoutNode& node) { return AddWindow(Make(node)); }

Widget* Interface::OpenPopup(Widget& owner, std::unique_ptr<Widget> popup) {
    ClosePopup();
    CallCreated(*popup);
    Widget* window = popup.get();
    popup_ = window;
    popupOwner_ = &owner;
    AddWindow(std::move(popup));
    return window;
}

void Interface::ClosePopup() {
    if (popup_ != nullptr) Close(popup_);
    popup_ = nullptr;
    popupOwner_ = nullptr;
}

void Interface::Close(Widget* window) {
    // Hidden now, deleted at the start of the next frame (the original defers it too: a window may
    // close itself while handling its own click). Its place is kept. A control inside a window
    // (a section opened as a plain window holding it) closes that whole window: closing only
    // itself left the holder on screen, empty and taking the mouse.
    if (window == nullptr) return;
    // Called from a handler that may have just rebuilt controls: the kept pointers below are looked up only if alive.
    if (Widget::Removals() != removalsSeen_) ForgetRemoved();
    window = window->Root();
    if (window == popup_) {
        popup_ = nullptr;
        popupOwner_ = nullptr;
    }
    Remember(*window);
    window->visible = false;
    if (pointer_.held != nullptr && pointer_.held->Root() == window) pointer_.held = nullptr;
    if (focus_ != nullptr && focus_->Root() == window) focus_ = nullptr;
    if (picked_ != nullptr && picked_->Root() == window) picked_ = nullptr;
    pointer_.hovered = nullptr;
    closing_.push_back(window);
}

void Interface::CloseAll() {
    std::vector<Top> windows = std::move(windows_);
    windows_.clear();
    windows.clear();
    closing_.clear();
    popup_ = popupOwner_ = nullptr;
    pointer_ = {};
    focus_ = nullptr;
    picked_ = nullptr;
}

void Interface::ForgetRemoved() {
    removalsSeen_ = Widget::Removals();
    // Which of the kept pointers are still in a window's tree (only those that are set are looked for).
    Widget** kept[] = {&pointer_.hovered, &pointer_.held, &under_, &focus_, &picked_, &popupOwner_};
    std::vector<Widget*> wanted;
    for (Widget** p : kept) {
        if (*p != nullptr) wanted.push_back(*p);
    }
    if (wanted.empty()) return;
    std::vector<const Widget*> found;
    std::vector<const Widget*> stack;
    for (const Top& t : windows_) stack.push_back(t.window.get());
    while (!stack.empty() && found.size() < wanted.size()) {
        const Widget* w = stack.back();
        stack.pop_back();
        if (std::find(wanted.begin(), wanted.end(), w) != wanted.end()) found.push_back(w);
        for (const auto& child : w->children()) stack.push_back(child.get());
    }
    for (Widget** p : kept) {
        if (*p != nullptr && std::find(found.begin(), found.end(), *p) == found.end()) *p = nullptr;
    }
}

void Interface::Raise(Widget* window) {
    const auto it = std::find_if(windows_.begin(), windows_.end(), [&](const Top& t) { return t.window.get() == window; });
    if (it == windows_.end() || it + 1 == windows_.end()) return;
    Top top = std::move(*it);
    windows_.erase(it);
    windows_.push_back(std::move(top));
    RaiseOnTop();
}

std::vector<Widget*> Interface::Windows() const {
    std::vector<Widget*> out;
    for (const Top& t : windows_) out.push_back(t.window.get());
    return out;
}

Widget* Interface::Window(int id) const {
    for (const Top& t : windows_) {
        if (t.window->id() == id && t.window->visible) return t.window.get();
    }
    return nullptr;
}

void Interface::Remember(const Widget& window) {
    if (window.id() > 0) places_[window.id()] = {window.rect.x, window.rect.y};
}

void Interface::Place(Widget& window, Vec2 screen) {
    const auto it = places_.find(window.id());
    if (it == places_.end()) {
        Centre(window, screen);
        return;
    }
    // Back where it was, pulled inside the screen if it would stick out.
    window.rect.x = std::min(it->second.x, screen.x - window.rect.w);
    window.rect.y = std::min(it->second.y, screen.y - window.rect.h);
}

void Interface::Centre(Widget& window, Vec2 screen) {
    // Whole pixels: half the screen less half the window.
    window.rect.x = std::floor(screen.x / 2.0f) - std::floor(window.rect.w / 2.0f);
    window.rect.y = std::floor(screen.y / 2.0f) - std::floor(window.rect.h / 2.0f);
}

void Interface::PullOntoScreen(Vec2 screen) {
    for (const Top& t : windows_) {
        Widget& w = *t.window;
        if (w.rect.x >= screen.x - 24.0f) w.rect.x = screen.x - w.rect.w;
        if (w.rect.y >= screen.y - 24.0f) w.rect.y = screen.y - w.rect.h;
        // Lost off the left or the top (no more than 24 of it on the screen): back at the edge.
        if (w.rect.x + w.rect.w <= 24.0f) w.rect.x = 0.0f;
        if (w.rect.y + w.rect.h <= 24.0f) w.rect.y = 0.0f;
    }
}

std::string Interface::SavePlaces(Vec2 screen) const {
    std::string out = std::to_string(int(screen.x)) + " " + std::to_string(int(screen.y)) + "\n";
    std::vector<std::pair<int, Vec2>> sorted(places_.begin(), places_.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [id, at] : sorted) out += std::to_string(id) + " " + std::to_string(int(at.x)) + " " + std::to_string(int(at.y)) + "\n";
    return out;
}

void Interface::LoadPlaces(std::string_view text, Vec2 screen) {
    std::istringstream in{std::string(text)};
    int w = 0, h = 0;
    if (!(in >> w >> h) || w != int(screen.x) || h != int(screen.y)) return; // another screen size
    int id = 0, x = 0, y = 0;
    while (in >> id >> x >> y) places_[id] = {float(x), float(y)};
}

void Interface::ShowTooltip(std::string text, Rect owner) {
    tooltip_.clear();
    if (!text.empty()) tooltip_.push_back({std::move(text), kWhite, kTextShadow});
    tooltipSign_.clear();
    tooltipOwner_ = owner;
}

void Interface::ShowTooltip(std::vector<TooltipLine> lines, Rect owner, std::string sign) {
    tooltip_ = std::move(lines);
    tooltipSign_ = std::move(sign);
    tooltipOwner_ = owner;
}

Widget* Interface::HitTest(Widget& w, Vec2 p, bool interactiveOnly) {
    // Hidden or disabled: the whole subtree refuses; so does one the mouse goes through.
    if (!w.visible || !w.enabled || w.clickThrough) return nullptr;
    // Children drawn later are in front (a clipping widget's only inside it).
    const Rect r = w.ScreenRect();
    const bool inside = p.x >= r.x && p.y >= r.y && p.x <= r.x + r.w && p.y <= r.y + r.h;
    for (auto it = w.children().rbegin(); it != w.children().rend() && (!w.clipChildren || inside); ++it) {
        if (Widget* hit = HitTest(**it, p, interactiveOnly)) return hit;
    }
    if (interactiveOnly && !w.interactive) return nullptr;
    return w.Hit(p) ? &w : nullptr;
}

void Interface::UpdateTree(Widget& w, ui::Frame& frame) {
    if (!w.visible) return;
    w.Update(frame);
    for (std::size_t i = 0; i < w.children().size(); ++i) UpdateTree(*w.children()[i], frame);
}

void Interface::DrawTree(Widget& w, ui::Frame& frame) {
    if (!w.visible) return;
    w.Draw(frame);
    if (w.clipChildren) frame.paint.PushClip(w.ScreenRect());
    for (const auto& c : w.children()) DrawTree(*c, frame);
    if (w.clipChildren) frame.paint.PopClip();
}

void Interface::Frame(Context& context, float scale, double time, float dt) {
    frameContext_ = &context;
    // Windows closed last frame go now.
    // (A window going may close a pop-up it owned: taken out of the list first.)
    const std::vector<Widget*> closing = std::exchange(closing_, {});
    for (Widget* w : closing) std::erase_if(windows_, [&](const Top& t) { return t.window.get() == w; });
    // Controls removed since the last frame (outside it): nothing points at them from here on.
    if (Widget::Removals() != removalsSeen_) ForgetRemoved();
    // A pop-up whose owner is gone or no longer shown goes with it.
    if (popup_ != nullptr && (popupOwner_ == nullptr || !popupOwner_->ShownOnScreen())) ClosePopup();

    const Input& input = context.input();
    screen_ = {context.screen().x / scale, context.screen().y / scale};
    pointer_.position = {input.mouse().x / scale, input.mouse().y / scale};
    // The window under the mouse is the one the UI context found (last frame's solid parts, in
    // our order); in it, the front-most interactive widget, and the deepest widget of any kind
    // (where a held item would be dropped).
    pointer_.hovered = nullptr;
    Widget* under = nullptr;
    under_ = nullptr;
    pickEnded_ = false;
    const Id over = context.hoveredLayer();
    for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
        if (HashId(it->layer) != over) continue;
        pointer_.hovered = HitTest(*it->window, pointer_.position, true);
        under = HitTest(*it->window, pointer_.position, false);
        under_ = under != nullptr ? under : it->window.get();
        break;
    }
    tooltip_.clear();
    sideTooltip_.clear();

    // An open pop-up holds the mouse: a press outside it and its owner only closes it.
    bool swallowPress = false;
    if (popup_ != nullptr && (input.Pressed(MouseButton::Left) || input.Pressed(MouseButton::Right))) {
        bool inside = under != nullptr && under->Root() == popup_;
        for (Widget* w = under; w != nullptr && !inside; w = w->parent()) inside = w == popupOwner_;
        if (!inside) {
            ClosePopup();
            swallowPress = true;
        }
    }

    // A held item (picked up by a click on its slot): the next left press only reaches buttons and
    // tabs (another page of the window, the item still held),
    // the next release drops it on the widget under the mouse or the first of its parents that
    // takes it; a right press puts it back.
    if (picked_ != nullptr && !swallowPress) {
        if (input.Pressed(MouseButton::Right)) {
            picked_ = nullptr;
            pickEnded_ = true;
            swallowPress = true;
        } else if (input.Pressed(MouseButton::Left) && dynamic_cast<const Button*>(pointer_.hovered) == nullptr &&
                   dynamic_cast<const SelectableArea*>(pointer_.hovered) == nullptr) {
            swallowPress = true;
        } else if (input.Released(MouseButton::Left) && pointer_.held == nullptr) {
            Widget* taker = nullptr;
            for (Widget* w = under; w != nullptr; w = w->parent()) {
                if (w->onDrop && w->onDrop(picked_, under)) {
                    taker = w;
                    break;
                }
            }
            Widget* source = picked_;
            picked_ = nullptr;
            pickEnded_ = true;
            if (Widget* home = source->Root(); home->onPickEnd) home->onPickEnd(source, taker);
            swallowPress = true;
        }
    }

    constexpr MouseButton kButtons[] = {MouseButton::Left, MouseButton::Right, MouseButton::Middle};
    if (pointer_.held == nullptr && !swallowPress) {
        for (MouseButton b : kButtons) {
            if (input.Pressed(b) && pointer_.hovered != nullptr) {
                pointer_.held = pointer_.hovered;
                pointer_.heldButton = b;
                pointer_.pressPosition = pointer_.position;
                break;
            }
        }
    }

    Painter measure(context.draw(), resources_, scale); // no layer is open: measuring only
    ui::Frame frame{*this, measure, input, pointer_, time, dt};
    for (std::size_t i = 0; i < windows_.size(); ++i) UpdateTree(*windows_[i].window, frame);
    // A window's update may have rebuilt its controls (ClearChildren) under the mouse: the widget about to take the
    // mouse below may be gone.
    if (Widget::Removals() != removalsSeen_) ForgetRemoved();
    // A press away from the focused widget takes the keyboard from it.
    if (focus_ != nullptr && input.Pressed(MouseButton::Left) && pointer_.hovered != focus_) focus_ = nullptr;
    if (!swallowPress) {
        if (Widget* target = pointer_.held != nullptr ? pointer_.held : pointer_.hovered) target->Mouse(frame);
    }
    // The click itself may have rebuilt a window's controls, the clicked one among them (the item mall's menu row
    // rebuilds its panel): nothing may point at them after it, the screen asks under() before the next frame.
    if (Widget::Removals() != removalsSeen_) ForgetRemoved();
    if (pointer_.held != nullptr && !input.Down(pointer_.heldButton)) pointer_.held = nullptr;
    if (focus_ != nullptr && !focus_->ShownOnScreen()) focus_ = nullptr;
    Widget* typedIn = nullptr;
    if (focus_ != nullptr && !context.keyboardBlocked()) {
        typedIn = focus_;
        focus_->Keyboard(frame);
        if (Widget::Removals() != removalsSeen_) ForgetRemoved(); // an Enter that rebuilt controls (a search)
    }
    // The game sees no keys while a text field has the keyboard, nor in the frame a field let it
    // go (the Enter or Escape that ended the typing is the field's).
    if (focus_ != nullptr || typedIn != nullptr) {
        context.SetFocus(reinterpret_cast<Id>(focus_ != nullptr ? focus_ : typedIn));
    } else if (context.typing()) {
        context.ClearFocus();
    }
    if (auto* edit = dynamic_cast<Edit*>(focus_)) {
        const Rect r = edit->ScreenRect();
        context.SetTextArea({r.x * scale, r.y * scale, r.w * scale, r.h * scale}, edit->caretX() * scale);
    }

    settled_ = true;
    int z = 1;
    for (Top& t : windows_) {
        if (!t.window->visible) {
            t.shownAt = -1.0;
            t.revealed = false;
            continue;
        }
        if (t.shownAt < 0.0) t.shownAt = time;
        context.BeginLayer(t.layer, LayerKind::Window);
        context.SetZ(z++);
        Painter paint(context.draw(), resources_, scale);
        ui::Frame drawing{*this, paint, input, pointer_, time, dt};
        DrawTree(*t.window, drawing);
        // A window just shown whose pictures are not all in yet stays out of sight, and lets the mouse through, until
        // they are (kRevealHold at most): it appears whole, never piece by piece. Asked for by this draw, they load
        // meanwhile; one shown again later has them in at once.
        if (!t.revealed) {
            if (paint.Settled() || time - t.shownAt >= kRevealHold) {
                t.revealed = true;
            } else {
                context.draw().Clear();
                context.EndLayer();
                settled_ = false;
                continue;
            }
        }
        // Every shown control takes the mouse over its rect (the original's windows swallow the
        // click; only a bar lets it through), so the world sees clicks only where nothing is.
        const std::function<void(Widget&)> solid = [&](Widget& w) {
            if (!w.visible || w.clickThrough) return;
            if (w.rect.w > 0 && w.rect.h > 0 && w.type() != "CIFBarWnd") context.Solid(paint.ToScreen(w.ScreenRect()));
            for (const auto& c : w.children()) solid(*c);
        };
        solid(*t.window);
        context.EndLayer();
        settled_ = settled_ && paint.Settled();
    }
    // The tooltip, over the windows (a held one of the world's when no window shows its own).
    if (tooltip_.empty() && !heldTooltip_.empty()) {
        tooltip_ = std::move(heldTooltip_);
        tooltipOwner_ = heldOwner_;
        tooltipSign_.clear();
    }
    heldTooltip_.clear();
    if (!tooltip_.empty()) {
        context.BeginLayer("interface tooltip", LayerKind::Overlay);
        context.SetZ(1);
        Painter paint(context.draw(), resources_, scale);
        const Vec2 screen{context.screen().x / scale, context.screen().y / scale};
        const Rect box = DrawTooltip(paint, tooltip_, tooltipOwner_, screen, tooltipSign_);
        // The side box beside it (its frame 8 out of each): its top level with the first's.
        if (!sideTooltip_.empty() && box.w > 0.0f) DrawTooltip(paint, sideTooltip_, {box.x - 8.0f, box.y - 8.0f, box.w + 16.0f, box.h + 16.0f}, screen);
        context.EndLayer();
    }
    // The held item over everything, the tooltip too: its slot drawn once more, centred on the
    // mouse (the original's frame does it after the windows).
    if (picked_ != nullptr) {
        context.BeginLayer("interface held item", LayerKind::Overlay);
        context.SetZ(2);
        Painter paint(context.draw(), resources_, scale);
        ui::Frame drawing{*this, paint, input, pointer_, time, dt};
        const Rect saved = picked_->rect;
        const Vec2 origin = picked_->Origin();
        picked_->rect.x += std::floor(pointer_.position.x - std::floor(saved.w / 2.0f)) - origin.x;
        picked_->rect.y += std::floor(pointer_.position.y - std::floor(saved.h / 2.0f)) - origin.y;
        const bool shown = picked_->visible;
        picked_->visible = true;
        DrawTree(*picked_, drawing);
        picked_->visible = shown;
        picked_->rect = saved;
        context.EndLayer();
    }
}

} // namespace sr::ui
