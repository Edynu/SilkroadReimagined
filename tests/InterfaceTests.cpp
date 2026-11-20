// The original's interface rebuilt from its layouts (sr::ui::Interface, the controls): the window
// system on hand-made layouts, and every layout of the game data built into a window when
// SR_GAME_DIR is set.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "sr/assets/FileSystem.h"
#include "sr/assets/UiLayout.h"
#include "sr/core/Path.h"
#include "sr/core/Text.h"
#include "sr/ui/ClassLayouts.h"
#include "sr/ui/Context.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

using namespace sr::ui;
namespace assets = sr::assets;

namespace {

// Every texture resident at 16 x 16; text keys stay as they are; no font.
class FakeResources final : public Resources {
public:
    std::set<std::string> loading; // these not in yet
    Texture GetTexture(const std::string& path) override { return {TextureRef{1}, 16.0f, 16.0f, !loading.contains(path), false}; }
    const std::string& Text(const std::string& key) const override { return key; }
    Font* GetFont() const override { return nullptr; }
};

assets::UiLayoutNode Node(std::string type, int id, Rect r, std::string ddj = "x.ddj") {
    assets::UiLayoutNode n;
    n.type = std::move(type);
    n.name = "N" + std::to_string(id);
    n.id = id;
    n.rect = {r.x, r.y, r.w, r.h};
    n.ddj = std::move(ddj);
    return n;
}

// Drives frames of the interface with input events.
struct Harness {
    FakeResources resources;
    std::map<std::string, assets::UiLayout> layouts;
    Interface interface{resources, [this](const std::string& path) {
                            const auto it = layouts.find(path);
                            return it == layouts.end() ? nullptr : &it->second;
                        }};
    Context context;
    Input input;
    double time = 0.0;

    Harness() { RegisterControls(interface); }

    void Frame(std::initializer_list<InputEvent> events = {}) {
        time += 1.0 / 60.0;
        input.BeginFrame(time);
        for (const InputEvent& e : events) input.Feed(e);
        context.NewFrame(input, {1024, 768}, false, false);
        interface.Frame(context, 1.0f, time, 1.0f / 60.0f);
        context.EndFrame();
    }
    void Move(float x, float y) { Frame({Event(InputEvent::Type::MouseMove, x, y)}); }
    void Press(float x, float y, MouseButton b = MouseButton::Left) {
        Move(x, y);
        InputEvent e = Event(InputEvent::Type::MouseDown, x, y);
        e.button = b;
        Frame({e});
    }
    void Release(float x, float y, MouseButton b = MouseButton::Left) {
        InputEvent e = Event(InputEvent::Type::MouseUp, x, y);
        e.button = b;
        Frame({e});
    }
    void Click(float x, float y, MouseButton b = MouseButton::Left) {
        Press(x, y, b);
        Release(x, y, b);
    }
    static InputEvent Event(InputEvent::Type type, float x, float y) {
        InputEvent e;
        e.type = type;
        e.position = {x, y};
        return e;
    }
};

} // namespace

TEST_CASE("a click on a button reaches its window; the first node of a section is on top") {
    Harness h;
    assets::UiLayout layout;
    assets::UiLayoutSection section;
    section.name = "Create";
    section.rect = {0, 0, 200, 100};
    section.nodes.push_back(Node("CIFButton", 1, {10, 10, 40, 20}));
    section.nodes.push_back(Node("CIFButton", 2, {20, 15, 40, 20})); // under button 1 where they overlap
    section.nodes.push_back(Node("CIFStatic", 3, {0, 0, 200, 100}));  // the background, under both
    layout.sections.push_back(section);
    h.layouts["resinfo\\test.txt"] = layout;
    Widget* window = h.interface.Open("resinfo\\test.txt", "Create", 7, {100, 100});
    REQUIRE(window != nullptr);
    std::vector<int> clicked;
    window->onNotice = [&](const Notice& n) {
        if (n.kind == Notice::Kind::Clicked) clicked.push_back(n.control);
    };
    h.Frame();
    h.Click(125, 120); // over both buttons
    REQUIRE(clicked.size() == 1);
    CHECK(clicked[0] == 1);
    h.Click(155, 132); // only button 2
    REQUIRE(clicked.size() == 2);
    CHECK(clicked[1] == 2);
    // Pressed on a button and released off it: no click.
    h.Press(125, 120);
    h.Release(300, 300);
    CHECK(clicked.size() == 2);
    // The background static takes no clicks, but nothing falls through the window to the world.
    h.Move(290, 190);
    CHECK(h.context.mouseOverLayer());
    CHECK(h.interface.pointer().hovered == nullptr);
}

namespace {
// A control that counts the frames the mouse reaches it, and a window that rebuilds its one control during its update
// when asked (as the HUD's COS bar does when a horse is sent away: HudCos::Build, ClearChildren).
struct Probe final : Widget {
    using Widget::Widget;
    int* mice = nullptr;
    void Mouse(Frame&) override { ++*mice; }
};
struct Rebuilder final : Widget {
    using Widget::Widget;
    bool rebuild = false;
    int mice = 0;
    void Update(Frame&) override {
        if (!rebuild && !children().empty()) return;
        rebuild = false;
        // The new control made before the old goes: never at the old one's address (which would hide a stale pointer).
        auto probe = std::make_unique<Probe>(Node("Probe", 1, {10, 10, 40, 20}));
        probe->rect = {10, 10, 40, 20};
        probe->interactive = true;
        probe->mice = &mice;
        ClearChildren();
        Add(std::move(probe));
    }
};
bool InTree(const Widget& root, const Widget* w) {
    if (&root == w) return true;
    for (const auto& c : root.children()) {
        if (InTree(*c, w)) return true;
    }
    return false;
}
} // namespace

TEST_CASE("a window rebuilding its controls under the mouse: the interface lets go of the removed ones") {
    Harness h;
    h.interface.Register("Rebuilder", [](const assets::UiLayoutNode& n) { return std::make_unique<Rebuilder>(n); });
    auto* window = static_cast<Rebuilder*>(h.interface.Open(Node("Rebuilder", 50, {100, 100, 200, 100})));
    REQUIRE(window != nullptr);
    h.Frame();
    h.Move(125, 120); // over the control
    h.Frame();
    REQUIRE(h.interface.pointer().hovered != nullptr);
    const int before = window->mice;
    CHECK(before > 0);
    // Rebuilt while hovered, then while held: the mouse goes to what is there now, never to the one removed.
    window->rebuild = true;
    h.Frame();
    const Widget* hovered = h.interface.pointer().hovered;
    CHECK((hovered == nullptr || InTree(*window, hovered)));
    h.Press(125, 120);
    window->rebuild = true;
    h.Frame();
    CHECK((h.interface.pointer().held == nullptr || InTree(*window, h.interface.pointer().held)));
    h.Release(125, 120);
    // And removed between frames (outside any update).
    h.Move(125, 120);
    window->ClearChildren();
    h.Frame();
    CHECK((h.interface.pointer().hovered == nullptr || InTree(*window, h.interface.pointer().hovered)));
}

namespace {
// A row that reports its press, and a window whose notice rebuilds the rows, the pressed one among them (the item mall's
// menu: HudMall's MenuRow and MallWindow::Rebuild, the user's crash 2026-11-14).
struct PressRow final : Widget {
    explicit PressRow(const assets::UiLayoutNode& n) : Widget(n) { interactive = true; }
    void Mouse(Frame& frame) override {
        if (frame.input.Pressed(MouseButton::Left) && frame.pointer.hovered == this) Notify(Notice::Kind::Clicked);
    }
};
struct RowsWindow final : Widget {
    using Widget::Widget;
    Interface* ui = nullptr;
    Widget* panel = nullptr;
    Widget* other = nullptr; // a window the click closes as well
    int built = 0;
    void Build() {
        // The new row made before the old ones go: never at the old one's address (which would hide a stale pointer).
        auto row = std::make_unique<PressRow>(Node("Row", 1, {0, 0, 40, 20}));
        row->rect = {0, 0, 40, 20};
        panel->ClearChildren();
        panel->Add(std::move(row));
        ++built;
    }
    bool OnNotice(const Notice& n) override {
        if (n.kind != Notice::Kind::Clicked) return false;
        Build();
        if (other != nullptr) ui->Close(other);
        return true;
    }
};
} // namespace

TEST_CASE("a click that rebuilds its own row's panel: the interface keeps no pointer at the row it removed") {
    Harness h;
    h.interface.Register("RowsWindow", [](const assets::UiLayoutNode& n) { return std::make_unique<RowsWindow>(n); });
    auto* window = static_cast<RowsWindow*>(h.interface.Open(Node("RowsWindow", 60, {100, 100, 200, 100})));
    REQUIRE(window != nullptr);
    window->ui = &h.interface;
    auto panel = std::make_unique<Widget>(Node("CIFStatic", 2, {10, 10, 100, 50}));
    panel->rect = {10, 10, 100, 50};
    window->panel = window->Add(std::move(panel));
    window->Build();
    Widget* other = h.interface.Open(Node("CIFStatic", 61, {400, 400, 50, 50}));
    REQUIRE(other != nullptr);
    window->other = other;
    h.Frame();
    h.Move(115, 115); // over the row
    REQUIRE(h.interface.pointer().hovered != nullptr);
    // The press: the row's notice rebuilds the panel and closes the other window in the same frame.
    InputEvent down = Harness::Event(InputEvent::Type::MouseDown, 115, 115);
    down.button = MouseButton::Left;
    h.Frame({down});
    CHECK(window->built == 2);
    // What the screen asks right after the frame (PlayScreen::HandleInput logs under()'s window): alive or nothing.
    const auto alive = [&](const Widget* w) { return w == nullptr || InTree(*window, w); };
    CHECK(alive(h.interface.under()));
    CHECK(alive(h.interface.pointer().held));
    CHECK(alive(h.interface.pointer().hovered));
    if (Widget* w = h.interface.under()) CHECK(w->Root() == window);
    h.Release(115, 115);
    h.Frame();
}

TEST_CASE("a text box whose row's click refills it with fewer rows: drawn without the rows that went") {
    // The NPC talk (HudNpc.cpp): a row's click shows another page in the same box at once (the user's crash at Event
    // So-Ok, 2026-11-14: the hovered row's number outlived the rows).
    Harness h;
    auto* box = As<TextBox>(h.interface.Open(Node("CIFTextBox", 70, {100, 100, 300, 200}, "")));
    REQUIRE(box != nullptr);
    box->hoverTracking = true;
    for (int i = 0; i < 5; ++i) box->AddParagraph("row " + std::to_string(i), kWhite);
    int picked = -1;
    box->onNotice = [&](const Notice& n) {
        if (n.kind != Notice::Kind::RowPicked) return;
        picked = n.value;
        box->ClearParagraphs();
        box->AddParagraph("the next page", kWhite);
    };
    h.Frame();
    h.Frame();
    // The last row (the box's line height from its layout): pressed and released on it.
    const float y = 100.0f + box->lineHeight * 4.5f;
    h.Click(150, y);
    h.Frame();
    CHECK(picked == 4);
    CHECK(box->paragraphs().size() == 1);
}

TEST_CASE("a window shows whole or not at all: unseen while its pictures load, kRevealHold at most") {
    Harness h;
    const auto open = [&](int id, const std::string& picture, Vec2 at) {
        assets::UiLayout layout;
        assets::UiLayoutSection section;
        section.name = "W";
        section.rect = {0, 0, 100, 100};
        section.nodes.push_back(Node("CIFStatic", 1, {0, 0, 100, 100}, picture));
        layout.sections.push_back(section);
        h.layouts["resinfo\\w" + std::to_string(id) + ".txt"] = layout;
        return h.interface.Open("resinfo\\w" + std::to_string(id) + ".txt", "W", id, at);
    };
    h.resources.loading = {"slow.ddj"};
    REQUIRE(open(1, "slow.ddj", {100, 100}) != nullptr);
    h.Move(150, 150);
    h.Frame();
    // Its picture not in: nothing of it shows, the mouse goes through to the world.
    CHECK_FALSE(h.context.mouseOverLayer());
    // In: it shows (and takes the mouse) from the next frame.
    h.resources.loading.clear();
    h.Frame();
    h.Frame();
    CHECK(h.context.mouseOverLayer());

    // A picture that never comes: shown anyway once kRevealHold has passed.
    h.resources.loading = {"never.ddj"};
    REQUIRE(open(2, "never.ddj", {400, 100}) != nullptr);
    h.Move(450, 150);
    h.Frame();
    CHECK_FALSE(h.context.mouseOverLayer());
    for (int i = 0; i < 12; ++i) h.Frame(); // 0.2 s at 60 frames a second
    CHECK(h.context.mouseOverLayer());
    // Already revealed: it stays shown while a picture it shows later loads (only a window just shown waits).
    h.resources.loading = {"slow.ddj"};
    h.Move(150, 150);
    h.Frame();
    h.Frame();
    CHECK(h.context.mouseOverLayer());
}

TEST_CASE("a main frame window: its close button hides it, its title strip drags it") {
    Harness h;
    assets::UiLayoutNode node = Node("CIFSystemWnd", 98, {300, 200, 214, 245}, "interface\\frame\\mframe_wnd_");
    Widget* window = h.interface.Open(node);
    REQUIRE(window != nullptr);
    REQUIRE(window->Child(2) != nullptr); // the close button
    REQUIRE(window->Child(1) != nullptr); // the drag strip
    h.Frame();
    // Drag by the strip (10, 0, w - 21, 34): the window follows the mouse.
    h.Press(350, 210);
    h.Frame({Harness::Event(InputEvent::Type::MouseMove, 380, 240)});
    h.Release(380, 240);
    CHECK(window->rect.x == doctest::Approx(330));
    CHECK(window->rect.y == doctest::Approx(230));
    // The close button sits 26 from the right, 10 down.
    h.Click(330 + 214 - 26 + 8, 230 + 10 + 8);
    CHECK_FALSE(window->visible);
}

TEST_CASE("a control closing itself closes the window holding it; nothing of it keeps the mouse") {
    // A section opened as a plain window holding the real one (the Option window): its Confirm
    // closes the control; the holder must go too, or it stays on screen empty, taking clicks.
    Harness h;
    assets::UiLayout layout;
    assets::UiLayoutSection section;
    section.name = "Holder";
    section.rect = {0, 0, 300, 200};
    section.nodes.push_back(Node("CIFStatic", 1, {0, 0, 300, 200}, ""));
    layout.sections.push_back(section);
    h.layouts["resinfo\\holder.txt"] = layout;
    Widget* window = h.interface.Open("resinfo\\holder.txt", "Holder", 8, {100, 100});
    REQUIRE(window != nullptr);
    Widget* inner = window->Child(1);
    REQUIRE(inner != nullptr);
    h.Frame();
    h.Move(250, 200);
    CHECK(h.context.mouseOverLayer());
    h.interface.Close(inner);
    CHECK_FALSE(window->visible);
    h.Frame();
    h.Move(250, 200);
    CHECK_FALSE(h.context.mouseOverLayer());
    CHECK(h.interface.Windows().empty());
}

TEST_CASE("a combo box's list: opened and closed by its arrow or field, in front, holding the mouse") {
    Harness h;
    assets::UiLayout layout;
    assets::UiLayoutSection section;
    section.name = "Create";
    section.rect = {0, 0, 300, 100};
    section.nodes.push_back(Node("CIFComboBox", 1, {10, 10, 156, 20}, ""));
    section.nodes.push_back(Node("CIFButton", 2, {10, 40, 100, 20}));  // under the open list
    section.nodes.push_back(Node("CIFButton", 3, {170, 40, 60, 20}));  // beside it
    layout.sections.push_back(section);
    h.layouts["resinfo\\test.txt"] = layout;
    Widget* window = h.interface.Open("resinfo\\test.txt", "Create", 7, {100, 100});
    REQUIRE(window != nullptr);
    auto* combo = As<ComboBox>(window->Child(1));
    REQUIRE(combo != nullptr);
    combo->items = {"a", "b", "c"};
    std::vector<int> clicked, picked;
    window->onNotice = [&](const Notice& n) {
        if (n.kind == Notice::Kind::Clicked) clicked.push_back(n.control);
        if (n.kind == Notice::Kind::ComboPicked) picked.push_back(n.value);
    };
    h.Frame();
    // The arrow (the right 20 pixels) opens the list, in front of every window; again closes it.
    h.Click(256, 120);
    REQUIRE(combo->open());
    CHECK(h.interface.Windows().back() == h.interface.popup());
    h.Click(256, 120);
    CHECK_FALSE(combo->open());
    // The field opens it too. A row picked over the button under the list: the row, not the button.
    h.Click(150, 120);
    REQUIRE(combo->open());
    h.Click(150, 134 + 13 + 6); // the second row
    CHECK_FALSE(combo->open());
    REQUIRE(picked.size() == 1);
    CHECK(picked[0] == 1);
    CHECK(combo->selected == 1);
    CHECK(clicked.empty());
    // A press outside only closes it; the next click reaches the button.
    h.Click(256, 120);
    REQUIRE(combo->open());
    h.Click(300, 150);
    CHECK_FALSE(combo->open());
    CHECK(clicked.empty());
    h.Click(300, 150);
    REQUIRE(clicked.size() == 1);
    CHECK(clicked[0] == 3);
    // Its window closed: the list goes with it.
    h.Click(256, 120);
    REQUIRE(combo->open());
    h.interface.Close(window);
    h.Frame();
    CHECK(h.interface.popup() == nullptr);
    h.Frame(); // the window deleted: the combo forgets the interface's pop-up safely
    CHECK(h.interface.popupOwner() == nullptr);
}

TEST_CASE("an item picked up from a slot is dropped on another; a right click uses a slot") {
    Harness h;
    assets::UiLayout layout;
    assets::UiLayoutSection section;
    section.name = "Create";
    section.rect = {0, 0, 200, 60};
    section.nodes.push_back(Node("CIFSlotWithHelp", 10, {10, 10, 32, 32}, ""));
    section.nodes.push_back(Node("CIFSlotWithHelp", 11, {46, 10, 32, 32}, ""));
    layout.sections.push_back(section);
    h.layouts["resinfo\\slots.txt"] = layout;
    Widget* window = h.interface.Open("resinfo\\slots.txt", "Create", 0, {0, 0});
    REQUIRE(window != nullptr);
    auto* a = As<Slot>(window->Find(10));
    auto* b = As<Slot>(window->Find(11));
    REQUIRE((a != nullptr && b != nullptr));
    a->icon = "a.ddj";
    a->count = 3;
    int used = -1;
    window->onNotice = [&](const Notice& n) {
        if (n.kind == Notice::Kind::RightClicked) used = n.control;
    };
    window->onDrop = [](Widget* source, Widget* target) {
        auto* from = As<Slot>(source);
        auto* to = As<Slot>(target);
        if (from == nullptr || to == nullptr || from == to) return false;
        std::swap(from->icon, to->icon);
        std::swap(from->count, to->count);
        return true;
    };
    h.Frame();
    h.Click(25, 25, MouseButton::Right);
    CHECK(used == 10);
    // A left release picks it up; the next left release on another slot drops it there.
    h.Click(25, 25);
    CHECK(h.interface.picked() == a);
    h.Click(60, 25);
    CHECK(h.interface.picked() == nullptr);
    CHECK(b->icon == "a.ddj");
    CHECK(b->count == 3);
    CHECK(a->icon.empty());
    // The slot's hit area reaches 2 pixels past its icon.
    h.Click(80, 25, MouseButton::Right);
    CHECK(used == 11);
}

TEST_CASE("windows keep their places by id, inside the screen; without one they open centred") {
    Harness h;
    assets::UiLayoutNode node = Node("CIFSystemWnd", 98, {0, 0, 214, 245}, "interface\\frame\\mframe_wnd_");
    Widget* window = h.interface.Open(node);
    REQUIRE(window != nullptr);
    h.interface.Place(*window, {1024, 768});
    CHECK(window->rect.x == 512 - 107);
    CHECK(window->rect.y == 384 - 122);
    window->rect.x = 900;
    window->rect.y = 700;
    h.interface.Close(window);
    window = h.interface.Open(node);
    h.interface.Place(*window, {1024, 768});
    CHECK(window->rect.x == 1024 - 214);
    CHECK(window->rect.y == 768 - 245);
    // Saved and read back for the same screen size only.
    const std::string saved = h.interface.SavePlaces({1024, 768});
    Harness other;
    other.interface.LoadPlaces(saved, {1280, 1024});
    Widget* elsewhere = other.interface.Open(node);
    other.interface.Place(*elsewhere, {1280, 1024});
    CHECK(elsewhere->rect.x == 640 - 107);
    Harness same;
    same.interface.LoadPlaces(saved, {1024, 768});
    Widget* kept = same.interface.Open(node);
    same.interface.Place(*kept, {1024, 768});
    CHECK(kept->rect.x == 1024 - 214); // kept at 900, pulled inside the screen
}

TEST_CASE("an edit takes the keyboard from the game while it has the focus") {
    Harness h;
    assets::UiLayout layout;
    assets::UiLayoutSection section;
    section.name = "Create";
    section.rect = {0, 0, 200, 40};
    section.nodes.push_back(Node("CIFEdit", 4, {10, 10, 120, 20}, ""));
    layout.sections.push_back(section);
    h.layouts["resinfo\\edit.txt"] = layout;
    Widget* window = h.interface.Open("resinfo\\edit.txt", "Create", 0, {0, 0});
    REQUIRE(window != nullptr);
    auto* edit = As<Edit>(window->Find(4));
    REQUIRE(edit != nullptr);
    h.Frame();
    h.Click(30, 20);
    CHECK(h.interface.focus() == edit);
    InputEvent text;
    text.type = InputEvent::Type::Text;
    text.text = "abc";
    h.Frame({text});
    InputEvent back;
    back.type = InputEvent::Type::KeyDown;
    back.key = Key::Backspace;
    h.Frame({back});
    CHECK(edit->text() == "ab");
    CHECK(h.context.typing());
    // A click elsewhere gives the keyboard back.
    h.Click(500, 500);
    CHECK(h.interface.focus() == nullptr);
}

TEST_CASE("every interface layout of the game builds into a window") {
    const char* dirVar = std::getenv("SR_GAME_DIR");
    if (dirVar == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipping");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirVar, error), error);
    assets::UiDefines defines;
    if (const auto file = fs.Read("config\\define.txt")) defines = assets::DecodeUiDefines(sr::core::DecodeTextFile(file->Bytes()));
    Harness h;
    std::vector<std::string> paths;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = sr::core::NormalizeAssetPath(path);
        if (!key.starts_with("resinfo\\") || !key.ends_with(".txt")) continue;
        assets::UiLayout layout;
        const auto file = fs.Read(key);
        if (file && assets::DecodeUiLayout(sr::core::DecodeTextFile(file->Bytes()), defines, layout, error) && !layout.sections.empty()) {
            h.layouts[key] = std::move(layout);
            paths.push_back(key);
        }
    }
    CHECK(paths.size() == 236); // 1.188 (v1.150: 220); the 16 new layouts have no class in ClassLayouts.h yet
    CHECK(ClassLayoutCount() == 201); // 199: the slider's own (ifsliderctrl.txt, the auto potion); 201: the price board's two (iftradeinfo*.txt)
    std::size_t widgets = 0;
    std::map<std::string, int> plain; // node classes made as plain widgets
    for (const std::string& path : paths) {
        Widget* window = h.interface.Open(path);
        REQUIRE_MESSAGE(window != nullptr, path);
        const std::function<void(Widget&)> count = [&](Widget& w) {
            ++widgets;
            if (typeid(w) == typeid(Widget) && !w.type().empty() && LayoutOfClass(w.type()).empty()) ++plain[w.type()];
            for (const auto& c : w.children()) count(*c);
        };
        count(*window);
    }
    h.Frame();
    MESSAGE("widgets built: " << widgets);
    std::string plainList;
    for (const auto& [type, n] : plain) plainList += type + " ";
    MESSAGE("plain: " << plainList);
    CHECK(widgets > 3347); // every layout control, plus the nested layouts and what controls make
    // The classes left plain are windows of their own (behaviour in their batches), not controls.
    for (const auto& [type, n] : plain) {
        CHECK_MESSAGE(type.find("Static") == std::string::npos, type);
        CHECK_MESSAGE(type.find("Button") == std::string::npos, type);
    }
}
