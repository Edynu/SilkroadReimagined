#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "sr/ui/Context.h"
#include "sr/ui/DrawList.h"
#include "sr/ui/Font.h"
#include "sr/ui/Hotkeys.h"
#include "sr/ui/Input.h"

using namespace sr::ui;

namespace {

InputEvent Mouse(InputEvent::Type type, float x, float y) {
    InputEvent e;
    e.type = type;
    e.position = {x, y};
    return e;
}

// One frame: a HUD layer with a button at (10,10,50,20) and a window layer over (40,0,100,100).
struct Frame {
    Context::ItemState hudButton, windowButton;
};
Frame Run(Context& ui, Input& input, bool windowOpen) {
    Frame f;
    ui.NewFrame(input, {800, 600}, false, false);
    ui.BeginLayer("hud", LayerKind::Hud);
    ui.Solid({10, 10, 50, 20});
    f.hudButton = ui.Item("button", {10, 10, 50, 20});
    ui.EndLayer();
    if (windowOpen) {
        ui.BeginLayer("window", LayerKind::Window);
        ui.Solid({40, 0, 100, 100});
        f.windowButton = ui.Item("close", {40, 0, 100, 100});
        ui.EndLayer();
    }
    ui.EndFrame();
    return f;
}

} // namespace

TEST_CASE("draw lists batch by texture and clip") {
    DrawList list;
    list.SetWhite({7}, {0.5f, 0.5f});
    list.Image({1}, {0, 0, 10, 10});
    list.Image({1}, {10, 0, 10, 10});
    list.Fill({0, 0, 5, 5}, Rgba(255, 0, 0));
    list.PushClip({0, 0, 4, 4});
    list.Fill({0, 0, 5, 5}, Rgba(255, 0, 0));
    list.PopClip();
    list.Image({0}, {0, 0, 10, 10}); // no texture: skipped
    REQUIRE(list.commands().size() == 3);
    CHECK(list.commands()[0].indexCount == 12);
    CHECK(list.commands()[1].texture.index == 7);
    CHECK(list.commands()[2].clip.w == 4.0f);
    CHECK(list.vertices().size() == 16);
}

TEST_CASE("input keeps presses for one frame") {
    Input input;
    input.BeginFrame(0.0);
    input.Feed(Mouse(InputEvent::Type::MouseDown, 5, 6));
    InputEvent key;
    key.type = InputEvent::Type::KeyDown;
    key.key = Key::I;
    input.Feed(key);
    CHECK(input.Pressed(MouseButton::Left));
    CHECK(input.Down(MouseButton::Left));
    CHECK(input.KeyPressed(Key::I));
    input.BeginFrame(0.1);
    CHECK_FALSE(input.Pressed(MouseButton::Left));
    CHECK(input.Down(MouseButton::Left));
    CHECK_FALSE(input.KeyPressed(Key::I));
    CHECK(input.KeyDown(Key::I));
}

TEST_CASE("the front layer takes the mouse; clicks complete on release over the item") {
    Context ui;
    Input input;
    input.BeginFrame(0);
    input.Feed(Mouse(InputEvent::Type::MouseMove, 45, 15)); // over both the HUD button and the window
    Run(ui, input, true); // first frame: hover is resolved from the previous frame's solids
    input.BeginFrame(0.1);
    Frame f = Run(ui, input, true);
    CHECK_FALSE(f.hudButton.hovered);
    CHECK(f.windowButton.hovered);
    CHECK(ui.mouseOverUi());

    input.BeginFrame(0.2);
    input.Feed(Mouse(InputEvent::Type::MouseDown, 45, 15));
    f = Run(ui, input, true);
    CHECK(f.windowButton.pressed);
    input.BeginFrame(0.3);
    input.Feed(Mouse(InputEvent::Type::MouseUp, 45, 15));
    f = Run(ui, input, true);
    CHECK(f.windowButton.clicked);

    // Without the window, the HUD button gets it; outside every solid, the world does.
    input.BeginFrame(0.4);
    Run(ui, input, false);
    input.BeginFrame(0.5);
    f = Run(ui, input, false);
    CHECK(f.hudButton.hovered);
    input.BeginFrame(0.6);
    input.Feed(Mouse(InputEvent::Type::MouseMove, 300, 300));
    Run(ui, input, false);
    CHECK_FALSE(ui.mouseOverUi());
    CHECK(ui.mouseFree());
}

TEST_CASE("text fields take the keyboard from the game") {
    Context ui;
    Input input;
    input.BeginFrame(0);
    InputEvent key;
    key.type = InputEvent::Type::KeyDown;
    key.key = Key::I;
    input.Feed(key);
    ui.NewFrame(input, {800, 600}, false, false);
    CHECK(ui.KeyPressed(Key::I));
    ui.SetFocus(42);
    CHECK_FALSE(ui.KeyPressed(Key::I));
    CHECK(ui.input().KeyPressed(Key::I)); // the field itself still reads it
    ui.EndFrame();
}

TEST_CASE("UTF-8 decoding") {
    const std::string text = "A\xC3\xA9\xE2\x82\xAC";
    std::size_t i = 0;
    CHECK(NextCodepoint(text, i) == 'A');
    CHECK(NextCodepoint(text, i) == 0xE9);
    CHECK(NextCodepoint(text, i) == 0x20AC);
    CHECK(i == text.size());
    const std::string broken = "\xC3";
    i = 0;
    CHECK(NextCodepoint(broken, i) == 0xFFFD);
}

TEST_CASE("fonts rasterise into the atlas and measure text") {
    const char* candidates[] = {"C:\\Windows\\Fonts\\arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                "/usr/share/fonts/truetype/freefont/FreeSans.ttf"};
    std::filesystem::path path;
    for (const char* c : candidates) {
        if (std::filesystem::exists(c)) {
            path = c;
            break;
        }
    }
    if (path.empty()) {
        MESSAGE("no TrueType font found - skipped");
        return;
    }
    std::ifstream file(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::memcpy(bytes.data(), raw.data(), raw.size());
    Font font;
    std::string error;
    REQUIRE_MESSAGE(font.Load(std::move(bytes), error), error);
    const Vec2 one = font.Measure("Hello", 12);
    const Vec2 two = font.Measure("Hello Hello", 12);
    CHECK(one.x > 10);
    CHECK(two.x > one.x * 2);
    CHECK(font.Measure("a\nb", 12).y == doctest::Approx(2 * font.LineHeight(12)));
    CHECK(font.Measure("Hello", 24).x > one.x * 1.7f);
    CHECK(font.dirty());
    font.MarkUploaded({3});
    DrawList list;
    font.Draw(list, "Hi", {0, 0}, 12, kWhite);
    REQUIRE(list.commands().size() == 1);
    CHECK(list.commands()[0].texture.index == 3);
    CHECK(list.commands()[0].indexCount == 12);
    // A glyph's texels carry coverage in alpha.
    bool covered = false;
    for (std::size_t i = 3; i < font.atlasPixels().size(); i += 4) covered = covered || font.atlasPixels()[i] > 128;
    CHECK(covered);
    const auto lines = font.Wrap("one two three four", 12, font.Measure("one two", 12).x + 1);
    REQUIRE(lines.size() == 3); // "three four" is wider than "one two"
    CHECK(lines[0] == "one two");
    CHECK(lines[1] == "three");
}

TEST_CASE("the font atlas reports the rows new glyphs went to") {
    const char* candidates[] = {"C:\\Windows\\Fonts\\arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    std::filesystem::path path;
    for (const char* c : candidates) {
        if (std::filesystem::exists(c)) path = c;
    }
    if (path.empty()) return;
    std::ifstream file(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::memcpy(bytes.data(), raw.data(), raw.size());
    Font font;
    std::string error;
    REQUIRE(font.Load(std::move(bytes), error));
    font.MarkUploaded({1});
    CHECK_FALSE(font.dirty());
    font.Measure("A", 14);
    CHECK(font.dirty());
    CHECK(font.dirtyTop() == 0);
    CHECK(font.dirtyBottom() > 5);
    CHECK(font.dirtyBottom() < 40);
}

TEST_CASE("double clicks need two presses close in time and place") {
    Input input;
    input.BeginFrame(1.0);
    input.Feed(Mouse(InputEvent::Type::MouseDown, 10, 10));
    CHECK_FALSE(input.DoubleClicked(MouseButton::Left));
    input.Feed(Mouse(InputEvent::Type::MouseUp, 10, 10));
    input.BeginFrame(1.2);
    input.Feed(Mouse(InputEvent::Type::MouseDown, 11, 10));
    CHECK(input.DoubleClicked(MouseButton::Left));
    input.Feed(Mouse(InputEvent::Type::MouseUp, 11, 10));
    input.BeginFrame(1.3);
    input.Feed(Mouse(InputEvent::Type::MouseDown, 11, 10)); // a third press starts over
    CHECK_FALSE(input.DoubleClicked(MouseButton::Left));
    input.BeginFrame(2.5);
    input.Feed(Mouse(InputEvent::Type::MouseDown, 11, 10)); // too late
    CHECK_FALSE(input.DoubleClicked(MouseButton::Left));
}

#include <atomic>

#include "sr/core/ThreadPool.h"

TEST_CASE("the thread pool runs every job and hands back results") {
    sr::core::ThreadPool pool(3);
    std::atomic<int> sum{0};
    std::vector<std::future<int>> results;
    for (int i = 1; i <= 100; ++i) {
        results.push_back(pool.Submit([i, &sum] {
            sum += i;
            return i * 2;
        }));
    }
    int doubled = 0;
    for (auto& r : results) doubled += r.get();
    CHECK(sum == 5050);
    CHECK(doubled == 10100);
    CHECK(pool.size() == 3);
}

TEST_CASE("the original's keys: defaults, rebinding, fixed keys") {
    KeyBindings keys;
    CHECK(keys.Find(Key::C) == KeyAction::Character);
    CHECK(keys.Find(Key::I) == KeyAction::Inventory);
    CHECK(keys.Find(Key::Tab) == KeyAction::Berserk);
    CHECK(keys.Find(Key::PageDown) == KeyAction::PetBehaviour);
    CHECK(keys.Find(Key::L) == KeyAction::Academy);
    CHECK(keys.Get(KeyAction::FaceTarget) == Key::None);
    CHECK(KeyActionLabel(KeyAction::AutoPotion) == "UIIT_PAG_MACROPOTION_TITLE");
    // A key given to one action is taken from the other.
    CHECK(keys.Set(KeyAction::FaceTarget, Key::C));
    CHECK(keys.Find(Key::C) == KeyAction::FaceTarget);
    CHECK(keys.Get(KeyAction::Character) == Key::None);
    // Escape, digits, arrows and F-keys keep their jobs.
    CHECK_FALSE(keys.Set(KeyAction::Character, Key::Escape));
    CHECK_FALSE(keys.Set(KeyAction::Character, Key::Num1));
    CHECK_FALSE(keys.Set(KeyAction::Character, Key::F9));
    CHECK_FALSE(keys.Set(KeyAction::Character, Key::Left));
    keys.Reset();
    CHECK(keys.Find(Key::C) == KeyAction::Character);
    CHECK_FALSE(keys.Find(Key::Num1).has_value());
}
