// The HUD (C7c): the player's state the windows show (game::PlayerState) and the HUD manager
// (client/src/gui/Hud.h): the start windows and their places by the screen's size, the keys'
// actions, Escape's order, F12 and the always-on-top set, on a hand-made ginterface.txt.

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>

#include "gui/ErrorTexts.h"
#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/assets/UiLayout.h"
#include "sr/core/Path.h"
#include "sr/core/Text.h"
#include "sr/game/Alchemy.h"
#include "sr/game/JobProtocol.h"
#include "sr/game/PlayerState.h"
#include "sr/ui/Context.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

using namespace sr;
using sr::client::Hud;
namespace hud = sr::client::hud;

namespace {

class FakeResources final : public ui::Resources {
public:
    Texture GetTexture(const std::string&) override { return {ui::TextureRef{1}, 16.0f, 16.0f, true, false}; }
    const std::string& Text(const std::string& key) const override { return key; }
    ui::Font* GetFont() const override { return nullptr; }
};

assets::UiLayoutNode Node(std::string type, int id, ui::Rect r) {
    assets::UiLayoutNode n;
    n.type = std::move(type);
    n.name = "N" + std::to_string(id);
    n.id = id;
    n.rect = {r.x, r.y, r.w, r.h};
    return n;
}

// The HUD manager over a small master list, at 1600 x 900.
struct HudHarness {
    FakeResources resources;
    std::map<std::string, assets::UiLayout> layouts;
    ui::Interface interface{resources, [this](const std::string& path) {
                                const auto it = layouts.find(path);
                                return it == layouts.end() ? nullptr : &it->second;
                            }};
    ui::Context context;
    ui::Input input;
    Hud hud;
    game::PlayerState state;
    double time = 0.0;
    static constexpr ui::Vec2 kScreen{1600, 900};

    HudHarness() {
        ui::RegisterControls(interface);
        assets::UiLayout master;
        assets::UiLayoutSection create;
        create.name = "Create";
        create.nodes.push_back(Node("CIFSystemWnd", hud::kSystemMenu, {300, 200, 214, 245}));
        create.nodes.push_back(Node("CIFWnd", hud::kMainPopup, {595, 262, 388, 408}));
        create.nodes.push_back(Node("CIFWnd", hud::kCommunity, {0, 0, 477, 393}));
        create.nodes.push_back(Node("CIFWnd", hud::kWorldMap, {100, 100, 652, 424}));
        create.nodes.push_back(Node("CIFWnd", hud::kPlayerInfo, {4, 7, 212, 70}));
        create.nodes.push_back(Node("CIFWnd", hud::kUnderBar, {112, 684, 800, 52}));
        create.nodes.push_back(Node("CIFWnd", hud::kMinimap, {892, 6, 140, 184}));
        master.sections.push_back(create);
        assets::UiLayoutSection alchemy;
        alchemy.name = "AlchemyBox";
        alchemy.nodes.push_back(Node("CIFWnd", hud::kAlchemy, {0, 0, 300, 200}));
        master.sections.push_back(alchemy);
        layouts["resinfo\\ginterface.txt"] = master;
        // The main window's pages, nested in it as the original's layout nests them.
        hud.Bind(&state);
        hud.Create(interface, kScreen);
        if (ui::Widget* main = Window(hud::kMainPopup)) {
            for (int page = hud::kPageInventory; page <= hud::kPageAcademy; ++page) {
                main->Add(interface.Make(Node("CIFWnd", page, {0, 0, 100, 100})));
            }
        }
        Frame();
    }

    ui::Widget* Window(int id) {
        for (ui::Widget* w : interface.Windows()) {
            if (w->id() == id) return w;
        }
        return nullptr;
    }
    void Frame(std::initializer_list<ui::InputEvent> events = {}) {
        time += 1.0 / 60.0;
        input.BeginFrame(time);
        for (const ui::InputEvent& e : events) input.Feed(e);
        context.NewFrame(input, kScreen, false, false);
        interface.Frame(context, 1.0f, time, 1.0f / 60.0f);
        hud.Keys(context, time);
        hud.Sync();
        context.EndFrame();
    }
    void Key(ui::Key key, bool ctrl = false) {
        ui::InputEvent down;
        down.type = ui::InputEvent::Type::KeyDown;
        down.key = key;
        if (ctrl) {
            ui::InputEvent c = down;
            c.key = ui::Key::Ctrl;
            Frame({c, down});
        } else {
            Frame({down});
        }
        ui::InputEvent up = down;
        up.type = ui::InputEvent::Type::KeyUp;
        if (ctrl) {
            ui::InputEvent c = up;
            c.key = ui::Key::Ctrl;
            Frame({up, c});
        } else {
            Frame({up});
        }
    }
};

} // namespace

TEST_CASE("the original's refusal codes find their words (the 1.188 client's one table)") {
    using sr::client::OriginalErrorKey;
    CHECK(std::string(OriginalErrorKey(0x185B)) == "UIIT_MSG_STRGERR_WAIT_FOR_REUSE_DELAY");
    CHECK(std::string(OriginalErrorKey(0x1C07)) == "UIIT_MSG_INTERACTION_FAIL_NOT_ENOUGH_MONEY");
    CHECK(std::string(OriginalErrorKey(0x3029)) == "UIIT_STT_SKILL_USE_FAIL_OVERLAP_COUNT");
    CHECK(std::string(OriginalErrorKey(0x0002)) == "UIIT_MSG_PARTYERR_UNKNOWN_ERROR");
    CHECK(OriginalErrorKey(0x1807) == nullptr); // none: the windows' own words
    CHECK(OriginalErrorKey(0xFFFF) == nullptr);
}

TEST_CASE("the HUD's start windows stand where the screen's size puts them") {
    HudHarness h;
    // Shown at the start: the player's bar, 1.188's underbar (CNIFUnderMenuBar; ginterface.txt's CIFUnderBar made but
    // hidden), the minimap; the rest wait hidden.
    CHECK(h.hud.Shown(hud::kPlayerInfo));
    CHECK(h.hud.Shown(hud::kMenuBar));
    CHECK_FALSE(h.hud.Shown(hud::kUnderBar));
    CHECK(h.hud.Shown(hud::kMinimap));
    CHECK_FALSE(h.hud.Shown(hud::kMainPopup));
    CHECK_FALSE(h.hud.Shown(hud::kSystemMenu));
    // Minimap at W - 129, 0; the underbar centred on the bottom, 68 high; the main window 44 from the
    // right and 200 above the bottom; the system menu centred; the player's bar where the file says.
    CHECK(h.Window(hud::kMinimap)->rect.x == doctest::Approx(1600 - 129));
    CHECK(h.Window(hud::kMinimap)->rect.y == doctest::Approx(0));
    CHECK(h.Window(hud::kMenuBar)->rect.x == doctest::Approx(400));
    CHECK(h.Window(hud::kMenuBar)->rect.y == doctest::Approx(900 - 68));
    CHECK(h.Window(hud::kMainPopup)->rect.x == doctest::Approx(1600 - 44 - 388));
    CHECK(h.Window(hud::kMainPopup)->rect.y == doctest::Approx(900 - 200 - 408));
    CHECK(h.Window(hud::kSystemMenu)->rect.x == doctest::Approx(800 - 107));
    CHECK(h.Window(hud::kPlayerInfo)->rect.x == doctest::Approx(4));
    // The quick quest window (shown while a quest runs) under the minimap: 324 from the right edge, its layout's y
    // kept, on any screen, not the layout's 702 of 1024.
    REQUIRE(h.interface.Open(Node("CIFWnd", hud::kQuestQuick, {702, 267, 220, 160})) != nullptr);
    h.hud.Place(HudHarness::kScreen);
    CHECK(h.Window(hud::kQuestQuick)->rect.x == doctest::Approx(1600 - 324));
    CHECK(h.Window(hud::kQuestQuick)->rect.y == doctest::Approx(267));
}

TEST_CASE("the HUD's keys open the main window's pages, Escape closes one thing at a time") {
    HudHarness h;
    ui::Widget* main = h.Window(hud::kMainPopup);
    REQUIRE(main != nullptr);
    // C: the main window on its Character page; C again: hidden; I: on Inventory, with the equipment.
    h.Key(ui::Key::C);
    CHECK(h.hud.Shown(hud::kMainPopup));
    CHECK(main->Child(hud::kPageCharacter)->visible);
    CHECK_FALSE(main->Child(hud::kPageInventory)->visible);
    h.Key(ui::Key::C);
    CHECK_FALSE(h.hud.Shown(hud::kMainPopup));
    h.Key(ui::Key::I);
    CHECK(h.hud.Shown(hud::kMainPopup));
    CHECK(main->Child(hud::kPageInventory)->visible);
    CHECK(main->Child(hud::kPageEquipment)->visible);
    CHECK_FALSE(main->Child(hud::kPageCharacter)->visible);
    // S while on Inventory: the page changes, the window stays.
    h.Key(ui::Key::S);
    CHECK(h.hud.Shown(hud::kMainPopup));
    CHECK(main->Child(hud::kPageSkills)->visible);
    // M: the world map. Escape closes the world map first (it comes before the main window).
    h.Key(ui::Key::M);
    CHECK(h.hud.Shown(hud::kWorldMap));
    h.Key(ui::Key::Escape);
    CHECK_FALSE(h.hud.Shown(hud::kWorldMap));
    CHECK(h.hud.Shown(hud::kMainPopup));
    h.Key(ui::Key::Escape);
    CHECK_FALSE(h.hud.Shown(hud::kMainPopup));
    // Nothing left to close: Escape opens the system menu, and the next closes it.
    h.Key(ui::Key::Escape);
    CHECK(h.hud.Shown(hud::kSystemMenu));
    h.Key(ui::Key::Escape);
    CHECK_FALSE(h.hud.Shown(hud::kSystemMenu));
}

TEST_CASE("Y asks the game screen for the alchemy window (1.188's own CNIFEnchantWnd)") {
    HudHarness h;
    // 1.188's alchemy window is its own (res_ui\nifenchantwnd.2dt): the game screen opens it on the request.
    h.hud.TakeRequests();
    h.Key(ui::Key::Y);
    bool asked = false;
    for (const auto& r : h.hud.TakeRequests()) asked = asked || r.kind == sr::client::HudRequest::Kind::ToggleAlchemy;
    CHECK(asked);
}

TEST_CASE("the system menu stays in front; F12 hides every window and shows them again") {
    HudHarness h;
    h.Key(ui::Key::Escape); // the system menu
    h.Key(ui::Key::M);      // the world map opens after it
    const auto windows = h.interface.Windows();
    CHECK(windows.back()->id() == hud::kSystemMenu);
    h.Key(ui::Key::F12);
    CHECK_FALSE(h.hud.Shown(hud::kMenuBar));
    CHECK_FALSE(h.hud.Shown(hud::kWorldMap));
    h.Key(ui::Key::F12);
    CHECK(h.hud.Shown(hud::kMenuBar));
    CHECK(h.hud.Shown(hud::kWorldMap));
    CHECK(h.hud.Shown(hud::kSystemMenu));
}

TEST_CASE("the HUD's keys use the quick slots of the underbar's page; the rest come out as requests") {
    HudHarness h;
    using K = sr::client::HudRequest::Kind;
    // 3 and 0 on page 1: quick slots 3 and 10, the used frame on their columns.
    h.Key(ui::Key::Num3);
    CHECK(h.state.quickSlots.usedColumn == 2);
    h.Key(ui::Key::Num0);
    CHECK(h.state.quickSlots.usedColumn == 9);
    // F2: page 2, where 1 is quick slot 11.
    h.Key(ui::Key::F2);
    CHECK(h.state.quickSlots.page == 1);
    h.Key(ui::Key::Num1);
    // X: the right button's slot; Ctrl + 2: the first party member.
    h.Key(ui::Key::X);
    h.Key(ui::Key::Num2, true);
    const auto requests = h.hud.TakeRequests();
    REQUIRE(requests.size() == 5);
    CHECK(requests[0].kind == K::UseQuickSlot);
    CHECK(requests[0].value == 3);
    CHECK(requests[1].value == 10);
    CHECK(requests[2].value == 11);
    CHECK(requests[3].kind == K::UseQuickSlot);
    CHECK(requests[3].value == 0);
    CHECK(requests[4].kind == K::TargetPartyMember);
    CHECK(requests[4].value == 1);
}

TEST_CASE("the HUD shows the target window for a new target only") {
    HudHarness h;
    CHECK_FALSE(h.hud.Shown(hud::kMainPopup));
    // No target window in this master list: Sync asks for it by id and finds none; the state
    // still drives it without failing.
    h.state.target.entity = 5;
    h.Frame();
    h.state.target.entity = 0;
    h.Frame();
    CHECK_FALSE(h.hud.Shown(hud::kTarget));
}

TEST_CASE("the player's state starts from the character the world sends") {
    game::PlayerState state;
    state.SetLevels({0, 35, 95, 195});
    game::proto::WorldEntered entered;
    entered.entity = 42;
    entered.character.name = "Hero";
    entered.character.model = 1907;
    entered.character.level = 2;
    entered.character.experience = 19;
    entered.character.hp = 200;
    entered.character.mp = 180;
    state.target.entity = 7;
    state.Apply(game::TitleEvent{entered});
    CHECK(state.self.entity == 42);
    CHECK(state.self.name == "Hero");
    CHECK(state.self.level == 2);
    CHECK(state.self.experienceToNext == 95);
    CHECK(state.self.ExperienceRatio() == doctest::Approx(0.2));
    CHECK(state.self.maxHp == 200);
    CHECK(state.self.maxMp == 180);
    CHECK_FALSE(state.target.Selected()); // a new world: nothing selected
    // The target goes when its entity does.
    state.target.entity = 9;
    game::proto::EntityGone other;
    other.entity = 8;
    state.Apply(game::TitleEvent{other});
    CHECK(state.target.Selected());
    game::proto::EntityGone gone;
    gone.entity = 9;
    state.Apply(game::TitleEvent{gone});
    CHECK_FALSE(state.target.Selected());
    // The messages keep the newest hundred.
    for (int i = 0; i < 120; ++i) state.Post(game::GameMessage::Kind::Game, std::to_string(i), 0.0);
    CHECK(state.messages.size() == game::PlayerState::kKeptMessages);
    CHECK(state.messages.front().text == "20");
}

TEST_CASE("the HUD's windows from the game's layouts show the player's state (game files)") {
    const char* dirVar = std::getenv("SR_GAME_DIR");
    if (dirVar == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipping");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirVar, error), error);
    assets::UiDefines defines;
    if (const auto file = fs.Read("config\\define.txt")) defines = assets::DecodeUiDefines(core::DecodeTextFile(file->Bytes()));
    FakeResources resources;
    std::map<std::string, assets::UiLayout> layouts;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = core::NormalizeAssetPath(path);
        if (!key.starts_with("resinfo\\") || !key.ends_with(".txt")) continue;
        assets::UiLayout layout;
        if (const auto file = fs.Read(key); file && assets::DecodeUiLayout(core::DecodeTextFile(file->Bytes()), defines, layout, error)) {
            layouts[key] = std::move(layout);
        }
    }
    ui::Interface interface{resources, [&](const std::string& path) {
                                const auto it = layouts.find(path);
                                return it == layouts.end() ? nullptr : &it->second;
                            }};
    ui::RegisterControls(interface);
    // A level-7 character at Jangan's centre with a monster selected and two quick slots filled.
    game::PlayerState state;
    game::WorldView world;
    game::proto::WorldEntered entered;
    entered.entity = 1;
    entered.character.name = "Altay";
    entered.character.level = 7;
    entered.character.hp = 495;
    entered.character.mp = 292;
    entered.position = {std::uint16_t((97 << 8) | 168), 960.0f, 0.0f, 960.0f};
    world.Apply(game::TitleEvent{entered});
    state.Apply(game::TitleEvent{entered});
    state.SetLevels({0, 118, 274, 498, 910, 1650, 2865, 4805, 7735});
    state.self.hp = 126;
    state.self.experience = 1906; // of 4,805: 39.667 %
    state.self.skillPoints = 24;
    state.target = {};
    state.target.entity = 2;
    state.target.kind = game::TargetStatus::Kind::Monster;
    state.target.name = "Barus Wolf";
    state.target.level = 6;
    state.target.rarity = 1;
    state.target.party = true;
    state.target.hp = 50;
    state.target.maxHp = 100;
    state.quickSlots.slots[1] = {game::QuickSlot::Kind::Skill, 7};
    state.quickSlots.slots[2] = {game::QuickSlot::Kind::Item, 13};
    Hud hud;
    sr::client::HudContext context;
    context.state = &state;
    context.world = &world;
    context.hud = &hud;
    context.text = [](const std::string& key) {
        if (key == "UIIT_STT_LV_LEVEL") return std::string("Lv %d");
        if (key == "UIIT_STT_MOB_CHAMPION") return std::string("Champion");
        if (key == "UIIT_STT_PARTYMOB_TARGET") return std::string("(Party)");
        return key;
    };
    context.slotIcon = [](const game::QuickSlot& q) { return "icon\\" + std::to_string(q.value) + ".ddj"; };
    // The 1.188 layouts the underbar and the collection book are made from (res_ui\*.2dt).
    std::map<std::string, std::vector<assets::Ui2dtRecord>> layouts2dt;
    for (const char* path : {"res_ui\\nifundermenubar.2dt", "res_ui\\nifcollectionwnd.2dt"}) {
        std::vector<assets::Ui2dtRecord> records;
        if (const auto file = fs.Read(path); file && assets::DecodeUi2dt(file->Bytes(), records, error)) layouts2dt[path] = std::move(records);
    }
    REQUIRE(layouts2dt.size() == 2);
    context.layout2dt = [&](const std::string& path) -> const std::vector<assets::Ui2dtRecord>* {
        const auto it = layouts2dt.find(path);
        return it == layouts2dt.end() ? nullptr : &it->second;
    };
    sr::client::RegisterHudWindows(interface, context);
    hud.Bind(&state);
    hud.Create(interface, HudHarness::kScreen);
    ui::Context ui;
    ui::Input input;
    double time = 0.0;
    const auto frame = [&](std::initializer_list<ui::InputEvent> events) {
        time += 1.0 / 60.0;
        context.time = time;
        input.BeginFrame(time);
        for (const ui::InputEvent& e : events) input.Feed(e);
        ui.NewFrame(input, HudHarness::kScreen, false, false);
        interface.Frame(ui, 1.0f, time, 1.0f / 60.0f);
        hud.Keys(ui, time);
        hud.Sync();
        ui.EndFrame();
    };
    frame({});
    frame({});
    const auto window = [&](int id) -> ui::Widget* {
        for (ui::Widget* w : interface.Windows()) {
            if (w->id() == id) return w;
        }
        return nullptr;
    };
    // The player's bar: the level, HP and MP as the original writes them.
    ui::Widget* bar = window(hud::kPlayerInfo);
    REQUIRE(bar != nullptr);
    CHECK(bar->Child(3)->text() == "Altay");
    CHECK(bar->Child(4)->text() == "Lv 7");
    CHECK(bar->Child(5)->text() == "126 / 495");
    CHECK(bar->Child(6)->text() == "292 / 292");
    // The underbar (1.188's CNIFUnderMenuBar): the percentage with two decimals, "Lv. 7", its tens
    // digit, the ten cells a percent each of the tenth (39.667: nine full, the tenth at .672), the tenth's progress
    // ("96%"), the skill points, the page, the slots' icons.
    ui::Widget* under = window(hud::kMenuBar);
    REQUIRE(under != nullptr);
    CHECK(under->visible);
    CHECK(under->rect.y == doctest::Approx(900 - 68));
    CHECK(under->Child(15)->text() == "39.67 %");
    CHECK(under->Child(13)->text() == "UIIT_STT_LEVEL_LV. 7");
    CHECK(under->Child(10)->text() == "3");
    CHECK(under->Child(17)->text() == "96%");
    CHECK(under->Child(12)->text() == "24");
    CHECK(under->Child(57)->text() == "F1");
    REQUIRE(ui::As<ui::Gauge>(under->Child(28)) != nullptr);
    CHECK(ui::As<ui::Gauge>(under->Child(28))->value() == doctest::Approx(1.0f));
    CHECK(ui::As<ui::Gauge>(under->Child(29))->value() == doctest::Approx(0.672f).epsilon(0.01));
    auto* first = ui::As<ui::Slot>(under->Child(40));
    auto* second = ui::As<ui::Slot>(under->Child(41));
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first->icon == "icon\\7.ddj");
    CHECK(second->icon == "icon\\13.ddj");
    // The target window: the monster's look centred at the top, 7 down, "Champion(Party)".
    ui::Widget* target = window(hud::kTarget);
    REQUIRE(target != nullptr);
    CHECK(target->visible);
    CHECK(target->rect.x == doctest::Approx((1600 - 236) / 2)); // 1.188's layout is 236 wide (v1.150's 196)
    CHECK(target->rect.y == doctest::Approx(7));
    REQUIRE(target->Child(2) != nullptr);
    CHECK(target->Child(2)->visible);
    CHECK(target->Child(2)->Child(2)->text() == "Champion(Party)");
    CHECK(target->Child(2)->Child(4)->text() == "Lv 6");
    // The minimap: Jangan's centre is X:6432 Y:1056.
    ui::Widget* minimap = window(hud::kMinimap);
    REQUIRE(minimap != nullptr);
    CHECK(minimap->Child(3)->text() == "X:6432");
    CHECK(minimap->Child(4)->text() == "Y:1056");
    CHECK(minimap->rect.x == doctest::Approx(1600 - 129));
    // A quick slot dropped on another swaps with it; one let go on the world is cleared.
    const auto click = [&](ui::Vec2 at) {
        ui::InputEvent move;
        move.type = ui::InputEvent::Type::MouseMove;
        move.position = at;
        frame({move});
        ui::InputEvent down = move;
        down.type = ui::InputEvent::Type::MouseDown;
        frame({down});
        ui::InputEvent up = move;
        up.type = ui::InputEvent::Type::MouseUp;
        frame({up});
    };
    const ui::Rect a = first->ScreenRect(), b = second->ScreenRect();
    click({a.x + 16, a.y + 16}); // picked up
    CHECK(interface.picked() == first);
    click({b.x + 16, b.y + 16}); // dropped on the second
    CHECK(state.quickSlots.slots[1].value == 13);
    CHECK(state.quickSlots.slots[2].value == 7);
    click({a.x + 16, a.y + 16});
    click({800, 450}); // the world
    CHECK(state.quickSlots.slots[1].kind == game::QuickSlot::Kind::Empty);
    bool told = false;
    for (const auto& r : hud.TakeRequests()) told = told || (r.kind == sr::client::HudRequest::Kind::QuickSlotChanged && r.value == 1);
    CHECK(told);
    // The arrow up shows the next page in the same ten slots, "F2".
    const ui::Rect upArrow = under->Child(19)->ScreenRect();
    click({upArrow.x + upArrow.w / 2, upArrow.y + upArrow.h / 2});
    CHECK(state.quickSlots.page == 1);
    frame({}); // the label follows on the bar's next update
    CHECK(under->Child(57)->text() == "F2");
    // The Menu button shows its menu; the menu's Collection book row opens the book and closes the menu.
    ui::Widget* menu = under->Child(59);
    REQUIRE(menu != nullptr);
    CHECK_FALSE(menu->visible);
    const ui::Rect menuButton = under->Child(4)->ScreenRect();
    click({menuButton.x + menuButton.w / 2, menuButton.y + menuButton.h / 2});
    CHECK(menu->visible);
    CHECK_FALSE(menu->Child(120)->visible); // the stall network's row (the stall's in its place)
    ui::Widget* row = menu->Child(104);
    REQUIRE(row != nullptr);
    const ui::Rect rowRect = row->ScreenRect();
    click({rowRect.x + 20, rowRect.y + rowRect.h / 2});
    CHECK_FALSE(menu->visible);
    ui::Widget* book = window(hud::kCollection);
    REQUIRE(book != nullptr);
    CHECK(book->visible);
}

TEST_CASE("the Skills page: the race's masteries as tabs, the board's slots, learning asks first (game files)") {
    const char* dirVar = std::getenv("SR_GAME_DIR");
    if (dirVar == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipping");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirVar, error), error);
    assets::UiDefines defines;
    if (const auto file = fs.Read("config\\define.txt")) defines = assets::DecodeUiDefines(core::DecodeTextFile(file->Bytes()));
    FakeResources resources;
    std::map<std::string, assets::UiLayout> layouts;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = core::NormalizeAssetPath(path);
        if (!key.starts_with("resinfo\\") || !key.ends_with(".txt")) continue;
        assets::UiLayout layout;
        if (const auto file = fs.Read(key); file && assets::DecodeUiLayout(core::DecodeTextFile(file->Bytes()), defines, layout, error)) {
            layouts[key] = std::move(layout);
        }
    }
    assets::SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    std::vector<std::uint32_t> masteryCosts;
    if (const auto file = fs.Read("server_dep\\silkroad\\textdata\\leveldata.txt")) {
        REQUIRE(assets::DecodeMasteryCosts(core::DecodeTextFile(file->Bytes()), masteryCosts, error));
    }
    ui::Interface interface{resources, [&](const std::string& path) {
                                const auto it = layouts.find(path);
                                return it == layouts.end() ? nullptr : &it->second;
                            }};
    ui::RegisterControls(interface);
    // A level-20 Chinese character with Bicheon at 10, Smash at level 1, 30 skill points.
    game::PlayerState state;
    game::proto::WorldEntered entered;
    entered.entity = 1;
    entered.character.name = "Altay";
    entered.character.level = 20;
    entered.character.strength = 40;
    entered.character.intelligence = 40;
    state.Apply(game::TitleEvent{entered});
    state.self.race = 0;
    state.self.skillPoints = 30;
    game::proto::SkillBookList book;
    for (std::uint32_t id : {257u, 258u, 259u, 273u, 274u, 275u, 276u}) book.masteries.push_back({id, std::uint8_t(id == 257 ? 10 : 0)});
    const assets::SkillRow* smash = skills.Find(std::string_view("SKILL_CH_SWORD_SMASH_A_01"));
    REQUIRE(smash != nullptr);
    book.skills.push_back(smash->id);
    state.Apply(game::TitleEvent{book});
    Hud hud;
    sr::client::HudContext context;
    context.state = &state;
    context.hud = &hud;
    context.skills = &skills;
    context.masteryCosts = &masteryCosts;
    context.text = [](const std::string& key) {
        if (key == "UIIT_STT_LV_LEVEL") return std::string("Lv %d");
        return key;
    };
    sr::client::RegisterHudWindows(interface, context);
    hud.Bind(&state);
    hud.Create(interface, HudHarness::kScreen);
    ui::Context ui;
    ui::Input input;
    double time = 0.0;
    const auto frame = [&](std::initializer_list<ui::InputEvent> events) {
        time += 1.0 / 60.0;
        context.time = time;
        input.BeginFrame(time);
        for (const ui::InputEvent& e : events) input.Feed(e);
        ui.NewFrame(input, HudHarness::kScreen, false, false);
        interface.Frame(ui, 1.0f, time, 1.0f / 60.0f);
        hud.Keys(ui, time);
        hud.Sync();
        ui.EndFrame();
    };
    frame({});
    hud.OpenPage(hud::kPageSkills);
    frame({});
    frame({});
    ui::Widget* main = interface.Window(hud::kMainPopup);
    REQUIRE(main != nullptr);
    ui::Widget* page = main->Find(hud::kPageSkills);
    REQUIRE(page != nullptr);
    CHECK(page->ShownOnScreen());
    // Two tabs (Weapon, Force), the bottom's numbers, Bicheon's board first.
    int tabs = 0;
    for (const auto& c : page->children()) tabs += c->node().name == "GDR_SKILL_TAB" ? 1 : 0;
    CHECK(tabs == 2);
    CHECK(page->Child(22)->text() == "30");
    CHECK(page->Child(24)->text() == "10/330"); // game::MasteryAllowance: a Chinese character's 330
    ui::Widget* board = page->Child(10);
    REQUIRE(board != nullptr);
    CHECK(board->Child(13)->text() == "Lv 10");
    CHECK(board->Child(14)->visible); // 10 + 1 <= 20, its cost within 30
    // Smash shows learned at level 1 and may be dragged; its next level can be learned.
    std::uint32_t dragged = 0;
    int learnable = 0;
    std::function<void(ui::Widget&)> walk = [&](ui::Widget& w) {
        if (auto* slot = dynamic_cast<ui::Slot*>(&w); slot != nullptr && w.ShownOnScreen()) {
            if (const std::uint32_t id = sr::client::DraggedSkill(slot)) dragged = id;
        }
        if (w.node().name == "GDR_SKILL_SLOT" && w.ShownOnScreen() && w.Child(2) != nullptr && w.Child(2)->visible && w.Child(2)->enabled) ++learnable;
        for (const auto& c : w.children()) walk(*c);
    };
    walk(*page);
    // The list's bar has its arrows and thumb (nested five deep).
    REQUIRE(board->Child(10) != nullptr);
    REQUIRE(board->Child(10)->Child(10) != nullptr);
    CHECK(board->Child(10)->Child(10)->children().size() >= 3);
    CHECK(dragged == smash->id);
    CHECK(learnable > 0);
    MESSAGE(learnable << " skills learnable on Bicheon's first rows");
    // A right click on the learned Smash's icon uses it (HudRequest::UseSkill, the play screen as its quick slot's).
    {
        ui::Slot* smashSlot = nullptr;
        std::function<void(ui::Widget&)> find = [&](ui::Widget& w) {
            if (auto* slot = dynamic_cast<ui::Slot*>(&w); slot != nullptr && w.ShownOnScreen() && sr::client::DraggedSkill(slot) == smash->id) smashSlot = slot;
            for (const auto& c : w.children()) find(*c);
        };
        find(*page);
        REQUIRE(smashSlot != nullptr);
        const ui::Rect r = smashSlot->ScreenRect();
        const ui::Vec2 at{r.x + r.w / 2.0f, r.y + r.h / 2.0f};
        hud.TakeRequests();
        ui::InputEvent move;
        move.type = ui::InputEvent::Type::MouseMove;
        move.position = at;
        frame({move});
        frame({});
        for (const ui::InputEvent::Type t : {ui::InputEvent::Type::MouseDown, ui::InputEvent::Type::MouseUp}) {
            ui::InputEvent e;
            e.type = t;
            e.position = at;
            e.button = ui::MouseButton::Right;
            frame({e});
        }
        const std::vector<sr::client::HudRequest> asked = hud.TakeRequests();
        CHECK(std::any_of(asked.begin(), asked.end(), [&](const sr::client::HudRequest& q) {
            return q.kind == sr::client::HudRequest::Kind::UseSkill && q.value == int(smash->id);
        }));
    }

    // The list: rows 55 apart, four shown, the bar's range the rows less four.
    ui::Widget* list = board->Child(10);
    int rows = 0;
    for (const auto& c : list->children()) rows += c->node().name == "GDR_SKILL_GROUP" ? 1 : 0;
    REQUIRE(rows > 4);
    auto* bar = ui::As<ui::ScrollBar>(list->Child(10));
    REQUIRE(bar != nullptr);
    bar->SetPosition(1000);
    frame({});
    const int top = bar->position();
    CHECK(top == rows - 4);
    int shown = 0;
    for (const auto& c : list->children()) {
        if (c->node().name != "GDR_SKILL_GROUP" || !c->visible) continue;
        ++shown;
        CHECK(c->rect.y == doctest::Approx(55.0f * float(c->id() - 100 - top)));
    }
    CHECK(shown == 4);
    // The board is a frame (its int_window_ parts close the list's bottom).
    CHECK(dynamic_cast<ui::FrameBorder*>(board) != nullptr);

    // The skill's tooltip (texts are their keys here): the name and level, then a blank, active,
    // what it costs, the description, what it does; Smash is learned, so its next level's conditions.
    const std::vector<ui::TooltipLine> tip = sr::client::SkillTooltip(*smash, context);
    REQUIRE(tip.size() > 5);
    CHECK(tip[0].text == smash->codename + " UIIT_STT_LEVEL_LV 1");
    CHECK(tip[1].text.empty());
    CHECK(tip[2].text == "PARAM_ACTIVE_SKILL");
    const auto has = [&](std::string_view text) {
        return std::any_of(tip.begin(), tip.end(), [&](const ui::TooltipLine& l) { return l.text.starts_with(text); });
    };
    CHECK(has("PARAM_REQ_WEAPON_TYPE: "));
    CHECK(has("PARAM_PA "));
    CHECK(has("PARAM_CONDITION_OF_NEXT_LEVEL"));
    CHECK_FALSE(has("PARAM_CONDITION_OF_LEARN"));
    // A skill not learned yet asks for its acquisition; a condition not met is red.
    const assets::SkillRow* far = nullptr;
    for (const assets::SkillRow* s : skills.Board(257)) {
        if (s->masteryLevels[0] > 10 && (far == nullptr || s->masteryLevels[0] < far->masteryLevels[0])) far = s;
    }
    REQUIRE(far != nullptr);
    const std::vector<ui::TooltipLine> learn = sr::client::SkillTooltip(*far, context);
    const auto header = std::find_if(learn.begin(), learn.end(), [](const ui::TooltipLine& l) { return l.text == "PARAM_CONDITION_OF_LEARN"; });
    REQUIRE(header != learn.end());
    const auto mastery = std::find_if(header, learn.end(), [](const ui::TooltipLine& l) { return l.text.starts_with("PARAM_MASTERY_LEVEL : "); });
    REQUIRE(mastery != learn.end());
    CHECK(mastery->color == ui::Argb(0xFFFF4A4A)); // Bicheon is at 10, the skill asks for more
}

TEST_CASE("the auto potion window: the original's rows, an item dropped in from the bag, Confirm (game files)") {
    const char* dirVar = std::getenv("SR_GAME_DIR");
    if (dirVar == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipping");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirVar, error), error);
    assets::UiDefines defines;
    if (const auto file = fs.Read("config\\define.txt")) defines = assets::DecodeUiDefines(core::DecodeTextFile(file->Bytes()));
    FakeResources resources;
    std::map<std::string, assets::UiLayout> layouts;
    for (const char* path : {"resinfo\\ginterface.txt", "resinfo\\ifautopotion.txt", "resinfo\\ifautopotionslot.txt", "resinfo\\ifsliderctrl.txt"}) {
        assets::UiLayout layout;
        const auto file = fs.Read(path);
        REQUIRE(file);
        REQUIRE_MESSAGE(assets::DecodeUiLayout(core::DecodeTextFile(file->Bytes()), defines, layout, error), error);
        layouts[path] = std::move(layout);
    }
    ui::Interface interface{resources, [&](const std::string& path) {
                                const auto it = layouts.find(path);
                                return it == layouts.end() ? nullptr : &it->second;
                            }};
    ui::RegisterControls(interface);
    // Two stacks of an HP potion and one of an MP potion in the bag.
    assets::ItemTable items;
    const auto potion = [&](std::uint32_t id, const char* code, int tid4, int p1, int p3) {
        assets::ItemRow r;
        r.refObjId = id;
        r.codename = code;
        r.typeId = {3, 3, 1, tid4};
        r.maxStack = 50;
        r.params[0] = p1;
        r.params[2] = p3;
        r.iconPath = std::string("item\\etc\\") + code + ".ddj";
        items.Add(r);
    };
    potion(4, "hp_potion_01", 1, 120, 0);
    potion(11, "mp_potion_01", 2, 0, 120);
    game::PlayerState state;
    game::proto::InventoryList bag;
    bag.size = 45;
    bag.items = {{13, 4, 0, 0, 0, 37, {}}, {14, 4, 0, 0, 0, 50, {}}, {15, 11, 0, 0, 0, 20, {}}};
    state.Apply(game::TitleEvent{bag});
    state.Apply(game::TitleEvent{game::ToMessage(game::AutoPotion{})});
    Hud hud;
    sr::client::HudContext context;
    context.state = &state;
    context.hud = &hud;
    context.items = &items;
    context.text = [](const std::string& key) { return key; };
    game::AutoPotion saved;
    int saves = 0;
    context.autoPotionSave = [&](const game::AutoPotion& s) {
        saved = s;
        ++saves;
    };
    sr::client::RegisterHudWindows(interface, context);
    hud.Bind(&state);
    hud.Create(interface, HudHarness::kScreen);
    ui::Context ui;
    ui::Input input;
    double time = 0.0;
    const auto frame = [&] {
        time += 1.0 / 60.0;
        context.time = time;
        input.BeginFrame(time);
        ui.NewFrame(input, HudHarness::kScreen, false, false);
        interface.Frame(ui, 1.0f, time, 1.0f / 60.0f);
        hud.Keys(ui, time);
        hud.Sync();
        ui.EndFrame();
    };
    frame();
    ui::Widget* w = hud.Show(hud::kAutoPotion);
    REQUIRE(w != nullptr);
    frame();
    // The original's rows: HP and MP named, the slider 1..100 at the defaults' 50,
    // the percent beside it; the quick slot's combo boxes gone, our slots in their place.
    ui::Widget* hpRow = w->Child(10);
    REQUIRE(hpRow != nullptr);
    CHECK(hpRow->Child(2)->text() == "UIIT_STT_MACROPOTION_HP");
    CHECK(w->Child(11)->Child(2)->text() == "UIIT_STT_MACROPOTION_MP");
    auto* slider = ui::As<ui::Slider>(hpRow->Child(15));
    REQUIRE(slider != nullptr);
    CHECK(slider->min == 1);
    CHECK(slider->max == 100);
    CHECK(slider->value == 50);
    CHECK(hpRow->Child(6)->text() == "50");
    CHECK_FALSE(hpRow->Child(13)->visible);
    for (int row = 0; row < game::AutoPotion::kRows; ++row) REQUIRE(sr::client::AutoPotionSlot(w, row) != nullptr);
    CHECK(sr::client::AutoPotionSlot(w, 0)->icon.empty());
    // Its thumb pressed and dragged with the mouse held: the value follows (the user's report 2026-11-13).
    {
        const ui::Rect thumb = slider->Child(3)->ScreenRect();
        const ui::Rect track = slider->ScreenRect();
        const auto feed = [&](ui::InputEvent::Type type, ui::Vec2 at) {
            ui::InputEvent e;
            e.type = type;
            e.position = at;
            e.button = ui::MouseButton::Left;
            time += 1.0 / 60.0;
            context.time = time;
            input.BeginFrame(time);
            input.Feed(e);
            ui.NewFrame(input, HudHarness::kScreen, false, false);
            interface.Frame(ui, 1.0f, time, 1.0f / 60.0f);
            ui.EndFrame();
        };
        const ui::Vec2 from{thumb.x + thumb.w / 2.0f, thumb.y + thumb.h / 2.0f};
        feed(ui::InputEvent::Type::MouseMove, from);
        feed(ui::InputEvent::Type::MouseDown, from);
        for (int i = 1; i <= 8; ++i) feed(ui::InputEvent::Type::MouseMove, {from.x + (track.x + track.w * 0.8f - from.x) * float(i) / 8.0f, from.y});
        feed(ui::InputEvent::Type::MouseUp, {track.x + track.w * 0.8f, from.y});
        CHECK(slider->value > 60);
        slider->SetValue(50);
        frame();
    }
    // Dropped in from the bag: an HP potion fits the HP row, not the MP row nor the abnormal one; the
    // slot shows how many the bag holds of it (both stacks).
    CHECK_FALSE(sr::client::AutoPotionDrop(w, game::AutoPotion::kMp, 13));
    CHECK_FALSE(sr::client::AutoPotionDrop(w, game::AutoPotion::kAbnormal, 13));
    CHECK(sr::client::AutoPotionDrop(w, game::AutoPotion::kHp, 13));
    CHECK(sr::client::AutoPotionDrop(w, game::AutoPotion::kMp, 15));
    frame();
    CHECK(sr::client::AutoPotionSlot(w, 0)->icon == "icon\\item\\etc\\hp_potion_01.ddj");
    CHECK(sr::client::AutoPotionSlot(w, 0)->count == 87);
    CHECK(sr::client::AutoPotionSlot(w, 1)->count == 20);
    // The rows ticked and the percents set, the delay at 2.5 s; Confirm hands them over and closes.
    sr::client::AutoPotionSetRow(w, game::AutoPotion::kHp, true, 70);
    sr::client::AutoPotionSetRow(w, game::AutoPotion::kMp, true, 40);
    sr::client::AutoPotionSetDelay(w, true, 2500);
    frame();
    CHECK(hpRow->Child(6)->text() == "70");
    sr::client::AutoPotionConfirm(w);
    CHECK(saves == 1);
    CHECK(saved.rows[game::AutoPotion::kHp] == game::AutoPotion::Entry{true, 70, 4});
    CHECK(saved.rows[game::AutoPotion::kMp] == game::AutoPotion::Entry{true, 40, 11});
    CHECK(saved.rows[game::AutoPotion::kAbnormal].item == 0);
    CHECK(saved.delayEnabled);
    CHECK(saved.delayMs == 2500);
    frame();
    CHECK_FALSE(hud.Shown(hud::kAutoPotion));
    // Opened again on what the server kept; when the bag holds none of the row's item any more, the
    // slot shows empty (the row keeps the item).
    state.Apply(game::TitleEvent{game::ToMessage(saved)});
    game::proto::ItemMoved used;
    used.emptied = {13, 14};
    state.Apply(game::TitleEvent{used});
    w = hud.Show(hud::kAutoPotion);
    REQUIRE(w != nullptr);
    frame();
    CHECK(ui::As<ui::Slider>(w->Child(10)->Child(15))->value == 70);
    CHECK(sr::client::AutoPotionSlot(w, 0)->icon.empty());
    CHECK(sr::client::AutoPotionSlot(w, 1)->count == 20);
    sr::client::AutoPotionConfirm(w);
    CHECK(saved.rows[game::AutoPotion::kHp].item == 4);
}

TEST_CASE("the alchemy window: 1.188's CNIFEnchantWnd opened, an item put in by its kind, Strengthen sends, the answer shown (game files)") {
    const char* dirVar = std::getenv("SR_GAME_DIR");
    if (dirVar == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipping");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirVar, error), error);
    assets::UiDefines defines;
    if (const auto file = fs.Read("config\\define.txt")) defines = assets::DecodeUiDefines(core::DecodeTextFile(file->Bytes()));
    FakeResources resources;
    std::map<std::string, assets::UiLayout> layouts;
    for (const char* path : {"resinfo\\ginterface.txt"}) {
        assets::UiLayout layout;
        const auto file = fs.Read(path);
        REQUIRE(file);
        REQUIRE_MESSAGE(assets::DecodeUiLayout(core::DecodeTextFile(file->Bytes()), defines, layout, error), error);
        layouts[path] = std::move(layout);
    }
    ui::Interface interface{resources, [&](const std::string& path) {
                                const auto it = layouts.find(path);
                                return it == layouts.end() ? nullptr : &it->second;
                            }};
    ui::RegisterControls(interface);
    std::map<std::string, std::vector<assets::Ui2dtRecord>> layouts2dt;
    for (const char* path : {"res_ui\\nifenchantwnd.2dt", "res_ui\\nifenchantalchemymsgbox.2dt"}) {
        std::vector<assets::Ui2dtRecord> records;
        if (const auto file = fs.Read(path); file && assets::DecodeUi2dt(file->Bytes(), records, error)) layouts2dt[path] = std::move(records);
    }
    REQUIRE(layouts2dt.size() == 2);
    assets::ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    const auto id = [&](const char* code) { return items.Find(std::string_view(code))->refObjId; };
    game::PlayerState state;
    game::proto::InventoryList bag;
    bag.size = 45;
    bag.items = {{13, id("ITEM_CH_SWORD_01_A"), 0, 0, 30, 1, {}},
                 {14, id("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B"), 0, 0, 0, 5, {}},
                 {15, id("ITEM_ETC_ARCHEMY_REINFORCE_PROB_UP_A_01"), 0, 0, 0, 3, {}},
                 {16, id("ITEM_ETC_ARCHEMY_REINFORCE_PROB_UP_A_02"), 0, 0, 0, 3, {}},
                 {17, id("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_SHIELD_B"), 0, 0, 0, 1, {}}};
    for (std::size_t i = 0; i < bag.items.size(); ++i) bag.items[i].item = bag.items[i].item; // (ids as the wire carries them)
    state.Apply(game::TitleEvent{bag});
    for (auto& [place, item] : state.inventory.places) item.serial = 100 + place;
    Hud hud;
    sr::client::HudContext context;
    context.state = &state;
    context.hud = &hud;
    context.items = &items;
    context.text = [](const std::string& key) { return key; };
    context.layout2dt = [&](const std::string& path) -> const std::vector<assets::Ui2dtRecord>* {
        const auto it = layouts2dt.find(path);
        return it == layouts2dt.end() ? nullptr : &it->second;
    };
    std::vector<std::string> lines;
    context.systemLine = [&](const std::string& line) { lines.push_back(line); };
    std::vector<std::pair<game::proto::MessageId, std::vector<std::byte>>> sent;
    context.alchemyRequest = [&](game::proto::MessageId m, std::vector<std::byte> payload) { sent.push_back({m, std::move(payload)}); };
    sr::client::RegisterHudWindows(interface, context);
    hud.Bind(&state);
    hud.Create(interface, HudHarness::kScreen);
    ui::Context ui;
    ui::Input input;
    double time = 0.0;
    const auto frame = [&] {
        time += 1.0 / 60.0;
        context.time = time;
        input.BeginFrame(time);
        ui.NewFrame(input, HudHarness::kScreen, false, false);
        interface.Frame(ui, 1.0f, time, 1.0f / 60.0f);
        hud.Keys(ui, time);
        hud.Sync();
        ui.EndFrame();
    };
    frame();
    sr::client::ToggleAlchemyWindow(context);
    frame();
    REQUIRE(sr::client::AlchemyShown(context));
    ui::Widget* w = interface.Window(hud::kEnchant);
    REQUIRE(w != nullptr);
    // Strengthen at first (the Type1 page 12 shown, the Type2 page 28 hidden), the main window on its inventory.
    ui::Widget* page = w->Child(5);
    REQUIRE(page != nullptr);
    ui::Widget* type1 = nullptr;
    for (const auto& c : page->children()) {
        if (c->id() == 12) type1 = c.get();
    }
    REQUIRE(type1 != nullptr);
    CHECK(type1->visible);
    CHECK(hud.Shown(hud::kMainPopup));
    // A powder before the equipment is refused (code 8), the wrong elixir too (10); the sword, its elixir and the
    // powder of its degree go to keys 0, 1, 2.
    sr::client::AlchemyPut(context, 15);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "UIIT_MSG_ENCHANT_LOAD_EQUIP_FIRST");
    sr::client::AlchemyPut(context, 13);
    sr::client::AlchemyPut(context, 17);
    CHECK(lines.back() == "UIIT_MSG_REINFORCERR_RECIPE_MISMATCH");
    sr::client::AlchemyPut(context, 14);
    sr::client::AlchemyPut(context, 16);
    CHECK(lines.back() == "UIIT_MSG_ENCHANT_NOT_LOAD_INGREDIENT2");
    sr::client::AlchemyPut(context, 15);
    frame();
    auto* main = ui::As<ui::Slot>(type1->Child(17));
    auto* elixir = ui::As<ui::Slot>(type1->Child(26));
    auto* powder = ui::As<ui::Slot>(type1->Child(27));
    REQUIRE(main != nullptr);
    REQUIRE(elixir != nullptr);
    REQUIRE(powder != nullptr);
    CHECK_FALSE(main->icon.empty());
    CHECK(elixir->count == 5);
    CHECK(powder->count == 3);
    // Strengthen: the request with the three places; the button turns into Cancel.
    sr::client::AlchemyPress(context);
    REQUIRE(sent.size() == 1);
    game::proto::AlchemyStrengthen m;
    REQUIRE(sent[0].first == game::proto::AlchemyStrengthen::kId);
    REQUIRE(game::proto::Decode(sent[0].second, m));
    CHECK(m.target == 13);
    CHECK(m.material == 14);
    CHECK(m.powder == 15);
    // The answer: +1, a line saying so; the equipment stays, the elixir and powder slots empty.
    game::proto::AlchemyResult r;
    r.outcome = game::proto::AlchemyOutcome::Success;
    r.place = 13;
    r.plusAfter = 1;
    sr::client::AlchemyResultArrived(context, r);
    frame();
    CHECK(lines.back() == "UIIT_MSG_REINFORCERR_SUCCESS");
    CHECK_FALSE(main->icon.empty());
    CHECK(elixir->count == 0);
    // Closed and opened again.
    sr::client::ToggleAlchemyWindow(context);
    frame();
    CHECK_FALSE(sr::client::AlchemyShown(context));
}


TEST_CASE("a magic option's tooltip line as 1.188 writes it: its keys and format, (+X%) at the item's degree, red penalties (game files)") {
    const char* dirVar = std::getenv("SR_GAME_DIR");
    if (dirVar == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipping");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirVar, error), error);
    assets::ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    std::vector<assets::MagicOptionRow> options;
    const auto file = fs.Read("server_dep\\silkroad\\textdata\\magicoption.txt");
    REQUIRE(file);
    REQUIRE(assets::DecodeMagicOptions(core::DecodeTextFile(file->Bytes()), options, error));
    sr::client::HudContext c;
    c.text = [](const std::string& key) { return key; };
    c.magicOptions = &options;
    const assets::ItemRow& sword = *items.Find(std::string_view("ITEM_CH_SWORD_01_A"));
    const auto id = [&](const char* name, int degree) { return game::alchemy::OptionRow(options, name, degree)->paramId; };
    sr::client::MagicOptionText t = sr::client::MagicOptionLine(c, sword, {id("MATTR_STR", 1), 3});
    CHECK(t.text == "PARAM_STR 3 PARAM_INCREASE (+100%)");
    CHECK_FALSE(t.negative);
    CHECK(sr::client::MagicOptionLine(c, sword, {id("MATTR_STR", 1), 1}).text == "PARAM_STR 1 PARAM_INCREASE (+0%)");
    CHECK(sr::client::MagicOptionLine(c, sword, {id("MATTR_ATHANASIA", 1), 2}).text == "PARAM_ATHANASIA(2UIIT_STT_COUNT)");
    t = sr::client::MagicOptionLine(c, sword, {id("MATTR_DEC_MAXDUR", 3), 40});
    CHECK(t.text == "PARAM_MAX_DURABILITY 40% PARAM_DECREASE");
    CHECK(t.negative);
    CHECK(sr::client::MagicOptionLine(c, sword, {65, 400}).text == "PARAM_NOT_REPAIRABLE (PARAM_MAX_DURABILITY 400% PARAM_INCREASE)");
    // An option whose row at the item's degree is missing gets no line (STR of degree 2 on a degree-1 sword reads
    // the degree-1 row instead).
    CHECK(sr::client::MagicOptionLine(c, sword, {id("MATTR_STR", 2), 2}).text == "PARAM_STR 2 PARAM_INCREASE (+50%)");
    // The stone lines' names.
    CHECK(sr::client::MagicOptionAlchemyNames(c, "MATTR_STR", 3).append == "PARAM_STR 3 PARAM_INCREASE");
    CHECK(sr::client::MagicOptionAlchemyNames(c, "MATTR_DUR", 40).percent);
}

namespace {

// The widget of a type somewhere under `w` (depth first).
template <typename T>
T* FindUnder(ui::Widget* w) {
    if (w == nullptr) return nullptr;
    if (auto* t = dynamic_cast<T*>(w)) return t;
    for (const auto& c : w->children()) {
        if (T* t = FindUnder<T>(c.get())) return t;
    }
    return nullptr;
}

} // namespace

TEST_CASE("the equipment: every worn place shows its item's tooltip on hover; the dress side's avatars (game files)") {
    const char* dirVar = std::getenv("SR_GAME_DIR");
    if (dirVar == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipping");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirVar, error), error);
    assets::UiDefines defines;
    if (const auto file = fs.Read("config\\define.txt")) defines = assets::DecodeUiDefines(core::DecodeTextFile(file->Bytes()));
    FakeResources resources;
    std::map<std::string, assets::UiLayout> layouts;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = core::NormalizeAssetPath(path);
        if (!key.starts_with("resinfo\\") || !key.ends_with(".txt")) continue;
        assets::UiLayout layout;
        if (const auto file = fs.Read(key); file && assets::DecodeUiLayout(core::DecodeTextFile(file->Bytes()), defines, layout, error)) {
            layouts[key] = std::move(layout);
        }
    }
    ui::Interface interface{resources, [&](const std::string& path) {
                                const auto it = layouts.find(path);
                                return it == layouts.end() ? nullptr : &it->second;
                            }};
    ui::RegisterControls(interface);
    // An item of every worn kind (head .. right ring) and of every avatar kind, by hand: each named by its own text key
    // (the tooltip's first line here).
    assets::ItemTable items;
    const auto row = [&](std::uint32_t id, std::array<int, 4> type, int childItems = 0) {
        assets::ItemRow r;
        r.refObjId = id;
        r.codename = "ITEM_" + std::to_string(id);
        r.nameKey = "SN_ITEM_" + std::to_string(id);
        r.typeId = type;
        r.country = 3;
        r.childItems = childItems;
        r.iconPath = "item\\" + std::to_string(id) + ".ddj";
        items.Add(r);
    };
    // The worn places 0..12: head, chest, shoulders, hands, legs (pants), feet, weapon, shield, job suit, earring, necklace,
    // the rings.
    const std::array<std::array<int, 4>, 13> worn = {{{3, 1, 1, 1}, {3, 1, 1, 3}, {3, 1, 1, 2}, {3, 1, 1, 5}, {3, 1, 1, 4}, {3, 1, 1, 6}, {3, 1, 6, 2},
                                                       {3, 1, 4, 1}, {3, 1, 7, 1}, {3, 1, 5, 1}, {3, 1, 5, 2}, {3, 1, 5, 3}, {3, 1, 5, 3}}};
    for (std::uint32_t place = 0; place < 13; ++place) row(100 + place, worn[place]);
    row(200, {3, 1, 13, 1});    // a hat
    row(201, {3, 1, 13, 2}, 1); // a dress that takes an attachment
    row(202, {3, 1, 13, 3});    // an attachment
    row(203, {3, 1, 13, 4});    // a flag
    row(204, {3, 1, 14, 1});    // a Devil's Spirit
    row(205, {3, 1, 13, 2}, 0); // a dress that takes none
    row(206, {3, 1, 13, 1});    // another hat
    game::PlayerState state;
    game::proto::InventoryList bag;
    bag.size = 45;
    for (std::uint8_t place = 0; place < 13; ++place) bag.items.push_back({place, 100u + place, 0, 0, 30, 1, {}});
    bag.items.push_back({13, 206, 0, 0, 0, 1, {}}); // another hat in the bag
    bag.items.push_back({14, 205, 0, 0, 0, 1, {}}); // the plain dress in the bag
    state.Apply(game::TitleEvent{bag});
    game::proto::AvatarList avatars;
    for (std::uint8_t place = 0; place < 5; ++place) avatars.items.push_back({place, 200u + place, 0, 0, 0, 1, {}});
    state.Apply(game::TitleEvent{avatars});
    Hud hud;
    sr::client::HudContext context;
    context.state = &state;
    context.hud = &hud;
    context.items = &items;
    context.text = [](const std::string& key) { return key; };
    sr::client::RegisterHudWindows(interface, context);
    hud.Bind(&state);
    hud.Create(interface, HudHarness::kScreen);
    ui::Context ui;
    ui::Input input;
    double time = 0.0;
    const auto frame = [&](std::initializer_list<ui::InputEvent> events) {
        time += 1.0 / 60.0;
        context.time = time;
        input.BeginFrame(time);
        for (const ui::InputEvent& e : events) input.Feed(e);
        ui.NewFrame(input, HudHarness::kScreen, false, false);
        interface.Frame(ui, 1.0f, time, 1.0f / 60.0f);
        hud.Keys(ui, time);
        hud.Sync();
        ui.EndFrame();
    };
    const auto moveTo = [&](ui::Vec2 at) {
        ui::InputEvent move;
        move.type = ui::InputEvent::Type::MouseMove;
        move.position = at;
        frame({move});
        frame({}); // the window under the mouse is the one the last frame found
    };
    const auto click = [&](ui::Vec2 at) {
        moveTo(at);
        ui::InputEvent down;
        down.type = ui::InputEvent::Type::MouseDown;
        down.position = at;
        frame({down});
        ui::InputEvent up = down;
        up.type = ui::InputEvent::Type::MouseUp;
        frame({up});
    };
    const auto centre = [](const ui::Widget* w) {
        const ui::Rect r = w->ScreenRect();
        return ui::Vec2{r.x + r.w / 2.0f, r.y + r.h / 2.0f};
    };
    frame({});
    hud.OpenPage(hud::kPageInventory);
    frame({});
    frame({});
    auto* equipment = FindUnder<sr::client::EquipmentWindow>(interface.Window(hud::kMainPopup));
    auto* inventory = FindUnder<sr::client::InventoryWindow>(interface.Window(hud::kMainPopup));
    REQUIRE(equipment != nullptr);
    REQUIRE(inventory != nullptr);
    REQUIRE(equipment->ShownOnScreen());
    CHECK_FALSE(equipment->AvatarView());

    // Every worn place, hovered: the slot under the mouse, its item's tooltip. The legs (pants) were taken by the
    // dress side's fifth slot (117, the Devil's Spirit's, at the legs' 5,172 and in front of them) while it showed.
    for (int place = 0; place < 13; ++place) {
        CAPTURE(place);
        ui::Slot* slot = equipment->SlotOf(place);
        REQUIRE(slot != nullptr);
        CHECK(slot->visible);
        moveTo(centre(slot));
        CHECK(interface.pointer().hovered == slot);
        REQUIRE_FALSE(interface.tooltip().empty());
        CHECK(interface.tooltip().front().text == "SN_ITEM_" + std::to_string(100 + place));
    }
    for (int kind = 0; kind < 5; ++kind) {
        REQUIRE(equipment->AvatarSlotOf(kind) != nullptr);
        CHECK_FALSE(equipment->AvatarSlotOf(kind)->visible);
    }

    // The dress side (button 13): the five avatar slots and the job suit's, button 14 to go back.
    ui::Widget* toAvatars = equipment->Child(13);
    REQUIRE(toAvatars != nullptr);
    CHECK(toAvatars->visible);
    click(centre(toAvatars));
    CHECK(equipment->AvatarView());
    CHECK_FALSE(toAvatars->visible);
    CHECK(equipment->Child(14)->visible);
    for (int place = 0; place < 13; ++place) CHECK(equipment->SlotOf(place)->visible == (place == game::slot::kJobSuit));
    for (int kind = 0; kind < 5; ++kind) {
        CAPTURE(kind);
        ui::Slot* slot = equipment->AvatarSlotOf(kind);
        CHECK(slot->visible);
        CHECK(slot->icon == "icon\\item\\" + std::to_string(200 + kind) + ".ddj");
        moveTo(centre(slot));
        CHECK(interface.pointer().hovered == slot);
        REQUIRE_FALSE(interface.tooltip().empty());
        CHECK(interface.tooltip().front().text == "SN_ITEM_" + std::to_string(200 + kind));
    }
    // The job suit's place shows there too.
    moveTo(centre(equipment->SlotOf(game::slot::kJobSuit)));
    REQUIRE_FALSE(interface.tooltip().empty());
    CHECK(interface.tooltip().front().text == "SN_ITEM_108");

    // Another hat from the bag dropped on the window: its kind is worn (the client's own check).
    (void)hud.TakeRequests();
    click(centre(inventory->SlotOf(13)));
    REQUIRE(interface.picked() == inventory->SlotOf(13));
    click(centre(equipment->AvatarSlotOf(game::avatar::kFlag)));
    CHECK(interface.picked() == nullptr);
    CHECK(hud.TakeRequests().empty());
    REQUIRE_FALSE(state.messages.empty());
    CHECK(state.messages.back().text == "UIIT_MSG_STRGERR_EQUIPITEM");
    // The hat taken to the bag: off (the server picks the bag's place).
    click(centre(equipment->AvatarSlotOf(game::avatar::kHat)));
    REQUIRE(interface.picked() == equipment->AvatarSlotOf(game::avatar::kHat));
    click(centre(inventory->SlotOf(20)));
    std::vector<sr::client::HudRequest> asked = hud.TakeRequests();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].kind == sr::client::HudRequest::Kind::TakeOffAvatar);
    CHECK(asked[0].value == game::avatar::kHat);
    // As the server answers: the hat in the bag's first free place, its avatar place empty; then the other hat goes on.
    game::proto::AvatarMoved off;
    off.bag = {{15, 200, 0, 0, 0, 1, {}}};
    off.avatarsEmptied = {game::avatar::kHat};
    state.Apply(game::TitleEvent{off});
    frame({});
    CHECK(equipment->AvatarSlotOf(game::avatar::kHat)->icon.empty());
    click(centre(inventory->SlotOf(13)));
    click(centre(equipment->AvatarSlotOf(game::avatar::kFlag))); // anywhere on the window
    asked = hud.TakeRequests();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].kind == sr::client::HudRequest::Kind::WearAvatar);
    CHECK(asked[0].value == 13);

    // A dress that takes no attachment: the attachment's slot disabled under the white veil.
    game::proto::AvatarMoved swap;
    swap.avatars = {{game::avatar::kDress, 205, 0, 0, 0, 1, {}}};
    swap.avatarsEmptied = {game::avatar::kAttachment};
    state.Apply(game::TitleEvent{swap});
    frame({});
    ui::Slot* attachment = equipment->AvatarSlotOf(game::avatar::kAttachment);
    CHECK_FALSE(attachment->enabled);
    CHECK(attachment->ghost);
    CHECK(attachment->icon.empty());

    // Back to the worn places (button 14): the avatar slots hidden again, the legs hovered show the pants.
    click(centre(equipment->Child(14)));
    CHECK_FALSE(equipment->AvatarView());
    for (int kind = 0; kind < 5; ++kind) CHECK_FALSE(equipment->AvatarSlotOf(kind)->visible);
    moveTo(centre(equipment->SlotOf(game::slot::kLegs)));
    CHECK(interface.pointer().hovered == equipment->SlotOf(game::slot::kLegs));
    REQUIRE_FALSE(interface.tooltip().empty());
    CHECK(interface.tooltip().front().text == "SN_ITEM_104");
}

TEST_CASE("the Socket page: an item put in, its sockets shown, Make and Remove by pick mode, a stone dropped and asked, the answers; the tooltips' socket lines; window 500 (game files)") {
    const char* dirVar = std::getenv("SR_GAME_DIR");
    if (dirVar == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipping");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirVar, error), error);
    assets::UiDefines defines;
    if (const auto file = fs.Read("config\\define.txt")) defines = assets::DecodeUiDefines(core::DecodeTextFile(file->Bytes()));
    FakeResources resources;
    std::map<std::string, assets::UiLayout> layouts;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = core::NormalizeAssetPath(path);
        if (!key.starts_with("resinfo\\") || !key.ends_with(".txt")) continue;
        assets::UiLayout layout;
        if (const auto file = fs.Read(key); file && assets::DecodeUiLayout(core::DecodeTextFile(file->Bytes()), defines, layout, error)) {
            layouts[key] = std::move(layout);
        }
    }
    ui::Interface interface{resources, [&](const std::string& path) {
                                const auto it = layouts.find(path);
                                return it == layouts.end() ? nullptr : &it->second;
                            }};
    ui::RegisterControls(interface);
    std::map<std::string, std::vector<assets::Ui2dtRecord>> layouts2dt;
    for (const char* path : {"res_ui\\nifenchantwnd.2dt", "res_ui\\nifenchantalchemymsgbox.2dt"}) {
        std::vector<assets::Ui2dtRecord> records;
        if (const auto file = fs.Read(path); file && assets::DecodeUi2dt(file->Bytes(), records, error)) layouts2dt[path] = std::move(records);
    }
    REQUIRE(layouts2dt.size() == 2);
    assets::ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    assets::SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    std::vector<assets::MagicOptGroupRow> groups;
    const auto groupFile = fs.Read("server_dep\\silkroad\\textdata\\refmagicoptgroup.txt");
    REQUIRE(groupFile);
    REQUIRE(assets::DecodeMagicOptGroups(core::DecodeTextFile(groupFile->Bytes()), groups, error));
    const auto id = [&](const char* code) { return items.Find(std::string_view(code))->refObjId; };
    const std::uint32_t gold = id("ITEM_ETC_SOCKET_STONE_GOLD_E"), sp = id("ITEM_ETC_SOCKET_STONE_SP_E"), heal = id("ITEM_ETC_SOCKET_STONE_HEAL_E");
    game::PlayerState state;
    game::proto::InventoryList bag;
    bag.size = 45;
    // Worn: a chest with GOLD_E at 3 and ER_E at 1. The bag: a chest (socket 0 GOLD_E at 2, socket 1 empty, 2 not
    // made), stones, the maker, Rondo 03, a sword of degree 1, a potion.
    game::proto::ItemRecord worn{1, id("ITEM_CH_M_CLOTHES_11_BA_A"), 0, 0, 50, 1, {}};
    worn.sockets = {{0, 31208, 3, gold}, {1, 31217, 1, id("ITEM_ETC_SOCKET_STONE_ER_E")}};
    game::proto::ItemRecord chest{13, id("ITEM_CH_M_CLOTHES_11_BA_A_RARE"), 0, 0, 50, 1, {}};
    chest.sockets = {{0, 31207, 2, gold}, {1, 0, 0, 0}};
    bag.items = {worn,
                 chest,
                 {14, heal, 0, 0, 0, 1, {}},
                 {15, id("ITEM_ETC_ARCHEMY_ETC_02"), 0, 0, 0, 2, {}},
                 {16, id("ITEM_ETC_ARCHEMY_RONDO_03"), 0, 0, 0, 3, {}},
                 {17, sp, 0, 0, 0, 1, {}},
                 {18, id("ITEM_CH_SWORD_01_A"), 0, 0, 30, 1, {}},
                 {19, id("ITEM_ETC_HP_POTION_01"), 0, 0, 0, 5, {}}};
    state.Apply(game::TitleEvent{bag});
    for (auto& [place, item] : state.inventory.places) item.serial = 100 + place;
    Hud hud;
    sr::client::HudContext context;
    context.state = &state;
    context.hud = &hud;
    context.items = &items;
    context.skills = &skills;
    context.magicOptGroups = &groups;
    context.text = [](const std::string& key) { return key; };
    context.layout2dt = [&](const std::string& path) -> const std::vector<assets::Ui2dtRecord>* {
        const auto it = layouts2dt.find(path);
        return it == layouts2dt.end() ? nullptr : &it->second;
    };
    std::vector<std::string> lines;
    context.systemLine = [&](const std::string& line) { lines.push_back(line); };
    std::vector<std::pair<game::proto::MessageId, std::vector<std::byte>>> sent;
    context.alchemyRequest = [&](game::proto::MessageId m, std::vector<std::byte> payload) { sent.push_back({m, std::move(payload)}); };
    sr::client::RegisterHudWindows(interface, context);
    hud.Bind(&state);
    hud.Create(interface, HudHarness::kScreen);
    ui::Context ui;
    ui::Input input;
    double time = 0.0;
    const auto frame = [&] {
        time += 1.0 / 60.0;
        context.time = time;
        input.BeginFrame(time);
        ui.NewFrame(input, HudHarness::kScreen, false, false);
        interface.Frame(ui, 1.0f, time, 1.0f / 60.0f);
        hud.Keys(ui, time);
        hud.Sync();
        ui.EndFrame();
    };
    const auto frames = [&](double seconds) {
        for (double t = 0.0; t < seconds; t += 1.0 / 60.0) frame();
    };
    frame();
    sr::client::ToggleAlchemyWindow(context);
    frame();
    ui::Widget* w = interface.Window(hud::kEnchant);
    REQUIRE(w != nullptr);
    sr::client::AlchemyShowSocket(context, true);
    frame();
    ui::Widget* page = nullptr;
    for (const auto& c : w->children()) {
        if (c->id() == 72) page = c.get();
    }
    REQUIRE(page != nullptr);
    REQUIRE(page->visible);
    CHECK_FALSE(w->Child(5)->visible);
    CHECK(page->Child(354)->text() == "UIIT_STT_SOCKET_HELP_SOCKET_DEFAULT");
    // Refused: a degree-1 sword, a potion.
    sr::client::AlchemyPut(context, 18);
    CHECK(lines.back() == "UIIT_MSG_SOCKET_LEVEL_MISMATCH");
    sr::client::AlchemyPut(context, 19);
    CHECK(lines.back() == "UIIT_STT_SOCKETERR_ITS_NOT_SOCKET_STUFF");
    // The chest: its name in the SoX gold, the closed mark over its unmade socket only, its stone in socket 0.
    sr::client::AlchemyPut(context, 13);
    frame();
    auto* target = ui::As<ui::Slot>(page->Child(70));
    REQUIRE(target != nullptr);
    CHECK_FALSE(target->icon.empty());
    CHECK(page->Child(68)->text() == "SN_ITEM_CH_CLOTHES_11_BA_A_RARE");
    CHECK(ui::As<ui::Static>(page->Child(68))->textColor == ui::Argb(0xFFFFD953));
    CHECK_FALSE(page->Child(73)->visible);
    CHECK_FALSE(page->Child(79)->visible);
    CHECK(page->Child(80)->visible);
    CHECK_FALSE(ui::As<ui::Slot>(page->Child(83))->icon.empty());
    CHECK(ui::As<ui::Slot>(page->Child(84))->icon.empty());
    CHECK(ui::As<ui::Button>(page->Child(56))->enabled);
    CHECK(ui::As<ui::Button>(page->Child(57))->enabled);
    CHECK(page->Child(354)->text() == "UIIT_STT_SOCKET_HELP_SOCKET_REMOVE");
    // Make: pick mode 1 (cursor 174) until an unmade socket is clicked; a made one does nothing.
    sr::client::AlchemySocketMake(context, 0);
    frame();
    CHECK(sr::client::AlchemySocketPickMode(context) == 1);
    CHECK(context.cursor == 174);
    CHECK(page->Child(354)->text() == "UIIT_STT_SOCKET_HELP_SOCKET_ADD");
    CHECK(sent.empty());
    sr::client::AlchemySocketMake(context, 2);
    frame();
    CHECK(sr::client::AlchemySocketPickMode(context) == 0);
    CHECK(context.cursor == 0);
    CHECK(sent.empty()); // the 400 ms gauge first
    frames(0.5);
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].first == game::proto::SocketMake::kId);
    game::proto::SocketMake make;
    REQUIRE(game::proto::Decode(sent[0].second, make));
    CHECK(make.target == 13);
    CHECK(make.index == 2);
    // The answer (the bag first): socket 2 made.
    state.inventory.places[13].sockets[2].made = true;
    ++state.inventoryVersion;
    game::proto::AlchemyResult r;
    r.operation = game::proto::AlchemyOperation::SocketMake;
    r.outcome = game::proto::AlchemyOutcome::Success;
    r.place = 13;
    r.socket = 2;
    sr::client::AlchemyResultArrived(context, r);
    frame();
    CHECK_FALSE(page->Child(80)->visible);
    CHECK_FALSE(ui::As<ui::Button>(page->Child(56))->enabled); // all three made
    // A weapon's stone on the chest: refused by the client; an armour stone on socket 1: asked, then sent at once.
    sent.clear();
    sr::client::AlchemySocketInsert(context, 14, 1);
    CHECK(lines.back() == "UIIT_STT_SOCKETERR_THE_ITEM_ISNT_PARING_THIS_SOCKET_STONE");
    sr::client::AlchemySocketInsert(context, 17, 0);
    CHECK(lines.back() == "UIIT_STT_SOCKETERR_THIS_HOLE_USED_ALREADY");
    sr::client::AlchemySocketInsert(context, 17, 1);
    frame();
    CHECK(sent.empty());
    REQUIRE(sr::client::AlchemyAnswer(context, true));
    frame();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].first == game::proto::SocketInsert::kId);
    game::proto::SocketInsert insert;
    REQUIRE(game::proto::Decode(sent[0].second, insert));
    CHECK(insert.target == 13);
    CHECK(insert.stone == 17);
    CHECK(insert.index == 1);
    // While it runs Remove is its Cancel.
    CHECK(page->Child(57)->text() == "UIIT_CTL_ALCHEMYBOX_BUTTON_DISMANTLING_CANCEL");
    CHECK(ui::As<ui::Button>(page->Child(57))->enabled);
    // Its answer on the 4th tick: the stone's skill at level 2; the line.
    state.inventory.places[13].sockets[1] = {true, 31204, 2, sp};
    state.inventory.places.erase(17);
    ++state.inventoryVersion;
    r = {};
    r.operation = game::proto::AlchemyOperation::SocketInsert;
    r.outcome = game::proto::AlchemyOutcome::Success;
    r.place = 13;
    r.socket = 1;
    r.option = 31204;
    r.value = 2;
    r.stone = sp;
    sr::client::AlchemyResultArrived(context, r);
    frame();
    CHECK(lines.back() == "UIIT_MSG_SOCKET_ALCHEMY_SUCCESS");
    CHECK(page->Child(57)->text() == "UIIT_CTL_SOCKET_BUTTON_REMOVE");
    CHECK_FALSE(ui::As<ui::Slot>(page->Child(84))->icon.empty());
    // Remove: pick mode 2 (cursor 175), a filled socket asked, the 400 ms gauge, then the request.
    sent.clear();
    sr::client::AlchemySocketErase(context, 2); // empty: still picking
    frame();
    CHECK(sr::client::AlchemySocketPickMode(context) == 2);
    CHECK(context.cursor == 175);
    CHECK(page->Child(354)->text() == "UIIT_STT_SOCKET_HELP_SOCKET_MAKE");
    sr::client::AlchemySocketErase(context, 0);
    REQUIRE(sr::client::AlchemyAnswer(context, true));
    frames(0.5);
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].first == game::proto::SocketErase::kId);
    game::proto::SocketErase erase;
    REQUIRE(game::proto::Decode(sent[0].second, erase));
    CHECK(erase.index == 0);
    // A refusal's text by its code.
    r = {};
    r.operation = game::proto::AlchemyOperation::SocketErase;
    r.outcome = game::proto::AlchemyOutcome::Refused;
    r.code = game::alchemy::code::kSocketNoMaterial;
    sr::client::AlchemyResultArrived(context, r);
    CHECK(lines.back() == "UIIT_STT_SOCKETERR_NOT_EXIST_RECIPE");
    // Closed: no pick mode, the screen's cursor.
    sr::client::ToggleAlchemyWindow(context);
    frame();
    CHECK(context.cursor == 0);

    // The item's tooltip: after its other lines each made socket, then the help.
    const game::ItemInstance& bagChest = state.inventory.places[13];
    const auto texts = [](const std::vector<ui::TooltipLine>& tl) {
        std::vector<std::string> out;
        for (const ui::TooltipLine& l : tl) out.push_back(l.text);
        return out;
    };
    const std::vector<std::string> tip = texts(sr::client::ItemTooltip(*items.Find(bagChest.refId), bagChest, context));
    const auto has = [&](const std::vector<std::string>& in, const std::string& line) { return std::find(in.begin(), in.end(), line) != in.end(); };
    CHECK(has(tip, "SN_ITEM_ETC_SOCKET_STONE_GOLD_E UIIT_STT_LEVEL_LV2"));
    CHECK(has(tip, "SN_ITEM_ETC_SOCKET_STONE_SP_E UIIT_STT_LEVEL_LV2"));
    CHECK(has(tip, "UIIT_STT_SOCKET_EMPTY_SLOT")); // socket 2, made and empty
    CHECK(has(tip, "UIIT_STT_SOCKET_HELP_TIP"));
    // A stone's: its part, each level's effect, the roll's note.
    const std::vector<std::string> stoneTip = texts(sr::client::ItemTooltip(*items.Find(gold), game::ItemInstance{0, gold}, context));
    CHECK(has(stoneTip, "UIIT_STT_ARMOR_POSITION : UIIT_STT_ARMOR_POSITION_BREAST"));
    CHECK(has(stoneTip, "UIIT_STT_SOCKET_SELECT_LEVEL"));
    int levels = 0;
    for (const std::string& l : stoneTip) levels += l.starts_with("UIIT_STT_LEVEL_LV") && l.find(" : ") != std::string::npos && l.size() > 22 ? 1 : 0;
    CHECK(levels == 3);
    const std::vector<std::string> healTip = texts(sr::client::ItemTooltip(*items.Find(heal), game::ItemInstance{0, heal}, context));
    CHECK(has(healTip, "UIIT_STT_ARMOR_POSITION : SN_TAB_WEAPON"));
    MESSAGE("GOLD_E's levels: " << stoneTip.size() << " lines");

    // Window 500: a right click on the worn chest shows its sockets' skills; the active one dragged gives its id, the
    // passive none; again hides it.
    hud.OpenPage(hud::kPageEquipment);
    frame();
    frame();
    REQUIRE(sr::client::ShowWornSocketSkills(context, 1));
    CHECK(sr::client::WornSocketSkill(context, 0, false) == 31208);
    CHECK(sr::client::WornSocketSkill(context, 1, false) == 31217);
    CHECK(sr::client::WornSocketSkill(context, 0, true) == 0);
    CHECK(sr::client::WornSocketSkill(context, 1, true) == 31217);
    auto* equipment = FindUnder<sr::client::EquipmentWindow>(interface.Window(hud::kMainPopup));
    REQUIRE(equipment != nullptr);
    sr::client::EquipSkillWindow* window500 = equipment->SkillWindow();
    REQUIRE(window500 != nullptr);
    CHECK(window500->visible);
    // A real right click on the worn chest's slot toggles it.
    ui::Slot* chestSlot = equipment->SlotOf(1);
    REQUIRE(chestSlot != nullptr);
    const ui::Rect cr = chestSlot->ScreenRect();
    const ui::Vec2 at{cr.x + cr.w / 2.0f, cr.y + cr.h / 2.0f};
    const auto rightClick = [&] {
        ui::InputEvent move;
        move.type = ui::InputEvent::Type::MouseMove;
        move.position = at;
        input.BeginFrame(time);
        for (const ui::InputEvent& e : {move}) input.Feed(e);
        frame();
        frame();
        for (const ui::InputEvent::Type t : {ui::InputEvent::Type::MouseDown, ui::InputEvent::Type::MouseUp}) {
            ui::InputEvent e;
            e.type = t;
            e.position = at;
            e.button = ui::MouseButton::Right;
            time += 1.0 / 60.0;
            context.time = time;
            input.BeginFrame(time);
            input.Feed(e);
            ui.NewFrame(input, HudHarness::kScreen, false, false);
            interface.Frame(ui, 1.0f, time, 1.0f / 60.0f);
            hud.Sync();
            ui.EndFrame();
        }
    };
    rightClick();
    CHECK_FALSE(window500->visible);
    rightClick();
    CHECK(window500->visible);
}


TEST_CASE("a league keeper's rows by the player's league, never its suit; a job's experience line") {
    using sr::client::JobNpcRow;
    game::PlayerState state;
    sr::client::HudContext context;
    context.state = &state;
    context.text = [](const std::string& key) { return key; };
    const auto rows = [&](std::uint32_t services) {
        std::vector<JobNpcRow> out;
        for (const auto& r : sr::client::JobNpcRows(context, services)) out.push_back(r.row);
        return out;
    };
    const std::uint32_t trader = game::proto::kNpcJobTrader, thief = game::proto::kNpcJobThief, hunter = game::proto::kNpcJobHunter;
    // In no league: join, the weekly ranking, the activity ranking, the previous information.
    CHECK(rows(trader) == std::vector{JobNpcRow::Join, JobNpcRow::DonationRank, JobNpcRow::ActivityRank, JobNpcRow::Previous});
    CHECK(rows(0x2).empty()); // no keeper
    // A trader at its own keeper: leave, create the alias; once made, modify it. No outcome at the trader's.
    state.job.type = game::JobType::Trader;
    CHECK(rows(trader) == std::vector{JobNpcRow::Leave, JobNpcRow::AliasCreate, JobNpcRow::DonationRank, JobNpcRow::ActivityRank, JobNpcRow::Previous});
    state.jobAlias = "Seller";
    CHECK(rows(trader)[1] == JobNpcRow::AliasModify);
    // At another league's keeper: the rankings and the previous information only.
    CHECK(rows(hunter) == std::vector{JobNpcRow::ContributionRank, JobNpcRow::ActivityRank, JobNpcRow::Previous});
    // A thief at its own: the outcome too, with the hunter's words.
    state.job.type = game::JobType::Thief;
    const auto thiefRows = sr::client::JobNpcRows(context, thief);
    REQUIRE(thiefRows.size() == 6);
    CHECK(thiefRows[0].text == "UIIT_STT_NPC_CHATTING_THIEFMENU_WITHD");
    CHECK(thiefRows[2].text == "UIIT_STT_NPC_CHATTING_HUNTERMENU_OUTCOME");
    CHECK(thiefRows[3].text == "UIIT_STT_NPC_CHATTING_HUNTERMENU_CONTRIBUTERANK");
    // The experience line against the job level's requirement (leveldata.txt cols 6..8).
    const std::vector<std::array<std::int64_t, 3>> table = {{0, 0, 0}, {70875, 70875, 70875}, {2388750, 2388750, 2388750}, {-1, -1, -1}};
    context.jobExp = &table;
    float fill = 0.0f;
    CHECK(sr::client::JobExpLine(context, 1, 1, 26224, fill) == "37% (26224)");
    CHECK(fill == doctest::Approx(26224.0 / 70875.0));
    CHECK(sr::client::JobExpLine(context, 2, 3, 5, fill) == "100%");
    CHECK(fill == 1.0f);
    CHECK(sr::client::JobClassName(context, 3, 7, 1) == "UIIT_STT_CLASS_EU_HUNTER_7");
    CHECK(sr::client::JobClassName(context, 1, 2, 0) == "UIIT_STT_CLASS_MERCHANT_2");
}

TEST_CASE("Event So-Ok's registration row and her coded quest's talk in the NPC window; the card's box (55)") {
    namespace proto = game::proto;
    FakeResources resources;
    std::map<std::string, assets::UiLayout> layouts;
    const auto layoutOf = [&](const char* path, const char* section, std::vector<assets::UiLayoutNode> nodes) {
        assets::UiLayoutSection s;
        s.name = section;
        s.nodes = std::move(nodes);
        layouts[path].sections.push_back(std::move(s));
    };
    layoutOf("resinfo\\if_npcwindow.txt", "Create", {Node("CIF_NPCTalk", 10, {10, 40, 380, 400})});
    layoutOf("resinfo\\if_npctalk.txt", "Create", {Node("CIFTextBox", 10, {10, 10, 360, 380})});
    layoutOf("resinfo\\ifmessagebox.txt", "Create", {Node("CIFNormalTile", 0, {16, 40, 284, 122})});
    layoutOf("resinfo\\ifmessagebox.txt", "MsgBoxINIF",
             {Node("CIFButton", 202, {112, 99, 76, 24}), Node("CIFButton", 201, {152, 99, 76, 24}), Node("CIFButton", 200, {72, 99, 76, 24}),
              Node("CIFStatic", 1, {0, 52, 300, 12})});
    ui::Interface interface{resources, [&](const std::string& path) {
                                const auto it = layouts.find(path);
                                return it == layouts.end() ? nullptr : &it->second;
                            }};
    ui::RegisterControls(interface);
    game::PlayerState state;
    game::FlagView flag;
    sr::client::HudContext context;
    context.state = &state;
    context.flag = &flag;
    context.text = [](const std::string& key) { return key; };
    context.hasText = [](const std::string& key) { return key != "SN_TALK_QEV_CH_EVENT_KISAENG_070910_B"; };
    std::vector<std::pair<proto::MessageId, std::vector<std::byte>>> sent;
    context.eventRequest = [&](proto::MessageId id, std::vector<std::byte> payload) { sent.emplace_back(id, std::move(payload)); };
    std::vector<std::string> lines;
    context.flagLine = [&](const game::FlagView::Line& line) { lines.push_back(line.key); };
    sr::client::RegisterHudWindows(interface, context);

    auto* npc = dynamic_cast<sr::client::NpcWindow*>(interface.Open(Node("CIF_NPCWindow", hud::kNpc, {0, 0, 400, 450})));
    REQUIRE(npc != nullptr);
    REQUIRE(npc->Child(10) != nullptr);
    auto* box = ui::As<ui::TextBox>(npc->Child(10)->Child(10));
    REQUIRE(box != nullptr);
    const auto rows = [&] {
        std::vector<std::string> out;
        for (const auto& p : box->paragraphs()) {
            if (!p.text.empty()) out.push_back(p.text);
        }
        return out;
    };
    constexpr std::uint32_t kSoOk = 9;
    proto::EventDesk desk;
    desk.npc = kSoOk;
    desk.flagState = 1;
    desk.quests.push_back({10145, "SN_QEV_CH_EVENT_KISAENG_081201"});
    flag.Apply(desk);
    // The registration row after her ordinary rows (a first menu has no end row, the original's); drawing it expects the "accepted" answer.
    flag.RowDrawn(2);
    npc->ShowNpc(kSoOk, "NPC_CH_EVENT_KISAENG1", "SN_NPC_CH_EVENT_KISAENG1", proto::kNpcTalk);
    CHECK(rows() == std::vector<std::string>{"1. UIIT_STT_NPC_CHATTING_WND_TALKSTART", "2. UIIT_MSG_FLAGWAR_GAME_JOIN_REQ"});
    CHECK(flag.Registering());
    // Under level 20: its line, nothing sent; at 20: the request.
    state.self.level = 19;
    CHECK(npc->Choose(2));
    CHECK(lines == std::vector<std::string>{"UIIT_MSG_FLAGWAR_GAME_JOIN_LEVEL_ERORR"});
    CHECK(sent.empty());
    state.self.level = 20;
    npc->ShowNpc(kSoOk, "NPC_CH_EVENT_KISAENG1", "SN_NPC_CH_EVENT_KISAENG1", proto::kNpcTalk);
    CHECK(npc->Choose(2));
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].first == proto::FlagWorldRegister::kId);
    // Registered: the cancel row, the "cancelled" answer expected.
    desk.flagState = 2;
    flag.Apply(desk);
    npc->ShowEventDesk();
    CHECK(rows()[1] == "2. UIIT_MSG_FLAGWAR_GAME_JOIN_REQ_CANCEL");
    CHECK_FALSE(flag.Registering());
    // Another NPC: no row.
    npc->ShowNpc(kSoOk + 1, "NPC_CH_SMITH", "SN_NPC_CH_SMITH", proto::kNpcTalk);
    CHECK(rows().size() == 1);

    // Her talk: the swap's title in her list; picked: EventTalk 0xFF.
    sent.clear();
    npc->ShowNpc(kSoOk, "NPC_CH_EVENT_KISAENG1", "SN_NPC_CH_EVENT_KISAENG1", proto::kNpcTalk);
    CHECK(npc->Choose(1));
    CHECK(rows() == std::vector<std::string>{"1. SN_QEV_CH_EVENT_KISAENG_081201", "2. UIIT_STT_NPC_CHATTING_WND_TALKEND"});
    CHECK(npc->Choose(1));
    REQUIRE(sent.size() == 1);
    proto::EventTalk talk;
    REQUIRE(proto::Decode(sent[0].second, talk));
    CHECK((talk.npc == kSoOk && talk.quest == 10145 && talk.choice == 0xFF));
    // Talk A: its text and two numbered rows; row 1 sends choice 0, the Exit row choice 1.
    npc->ShowEventSays({kSoOk, 10145, std::string(game::kSwapTalkA), {std::string(game::kSwapTalkB), std::string(game::kTalkExit)}});
    CHECK(rows() == std::vector<std::string>{std::string(game::kSwapTalkA), "1. " + std::string(game::kSwapTalkB), "2. SN_TALK_COMMON_EXIT"});
    CHECK(npc->Choose(1));
    REQUIRE(proto::Decode(sent.back().second, talk));
    CHECK(talk.choice == 0);
    npc->ShowEventSays({kSoOk, 10145, std::string(game::kSwapTalkA), {std::string(game::kSwapTalkB), std::string(game::kTalkExit)}});
    CHECK(npc->Choose(2));
    REQUIRE(proto::Decode(sent.back().second, talk));
    CHECK(talk.choice == 1);
    // A key the text tables lack shows an empty row (070919's B); another NPC's talk is not shown.
    npc->ShowEventSays({kSoOk, 10077, std::string(game::kSwapTalkA), {"SN_TALK_QEV_CH_EVENT_KISAENG_070910_B", std::string(game::kTalkExit)}});
    CHECK(rows()[1] == "1. ");
    npc->ShowEventSays({kSoOk + 1, 10077, "OTHER", {}});
    CHECK(rows()[0] == std::string(game::kSwapTalkA));

    // The card's box: MsgBoxINIF with Confirm alone; its text; Confirm closes it.
    assets::UiLayoutNode node = Node("CIFMessageBox", 0, {0, 0, 308, 144});
    node.subSection = "MsgBoxINIF";
    auto* inform = dynamic_cast<sr::client::InformBox*>(interface.Open(node));
    REQUIRE(inform != nullptr);
    inform->Say("UIIT_MSG_FLAGWAR_EVENT_JOIN");
    CHECK_FALSE(inform->Child(200)->visible);
    CHECK_FALSE(inform->Child(201)->visible);
    REQUIRE(inform->Child(202) != nullptr);
    CHECK(inform->Child(202)->visible);
    CHECK(inform->Child(301)->text() == "UIIT_MSG_FLAGWAR_EVENT_JOIN");
    CHECK(inform->OnNotice({ui::Notice::Kind::Clicked, 202, inform->Child(202), 0}));
    CHECK_FALSE(inform->visible);
}

TEST_CASE("the Battle Arena manager's rows in the NPC window: the open mode's row and its talk, the cancel") {
    namespace proto = game::proto;
    FakeResources resources;
    std::map<std::string, assets::UiLayout> layouts;
    assets::UiLayoutSection window, talk;
    window.name = talk.name = "Create";
    window.nodes = {Node("CIF_NPCTalk", 10, {10, 40, 380, 400})};
    talk.nodes = {Node("CIFTextBox", 10, {10, 10, 360, 380})};
    layouts["resinfo\\if_npcwindow.txt"].sections.push_back(window);
    layouts["resinfo\\if_npctalk.txt"].sections.push_back(talk);
    ui::Interface interface{resources, [&](const std::string& path) {
                                const auto it = layouts.find(path);
                                return it == layouts.end() ? nullptr : &it->second;
                            }};
    ui::RegisterControls(interface);
    game::PlayerState state;
    game::ArenaView arena;
    sr::client::HudContext context;
    context.state = &state;
    context.arena = &arena;
    context.text = [](const std::string& key) { return key; };
    context.hasText = [](const std::string&) { return true; };
    std::vector<std::pair<proto::MessageId, std::vector<std::byte>>> sent;
    context.eventRequest = [&](proto::MessageId id, std::vector<std::byte> payload) { sent.emplace_back(id, std::move(payload)); };
    sr::client::RegisterHudWindows(interface, context);
    auto* npc = dynamic_cast<sr::client::NpcWindow*>(interface.Open(Node("CIF_NPCWindow", hud::kNpc, {0, 0, 400, 450})));
    REQUIRE(npc != nullptr);
    auto* box = ui::As<ui::TextBox>(npc->Child(10)->Child(10));
    REQUIRE(box != nullptr);
    const auto rows = [&] {
        std::vector<std::string> out;
        for (const auto& p : box->paragraphs()) {
            if (!p.text.empty()) out.push_back(p.text);
        }
        return out;
    };
    constexpr std::uint32_t kManager = 12;
    // No desk yet: the ordinary rows only.
    npc->ShowNpc(kManager, "NPC_BATTLE_ARENA_MANAGER", "SN_NPC_BATTLE_ARENA_MANAGER", proto::kNpcTalk);
    CHECK(rows().size() == 1);
    // The random mode open with both kinds: its name's row; its talk with "Arena Registration".
    arena.Apply(proto::ArenaDesk{kManager, 0x60});
    npc->ShowArenaDesk();
    CHECK(rows() == std::vector<std::string>{"1. UIIT_STT_NPC_CHATTING_WND_TALKSTART", "2. UIIT_STT_ARENA_RANDOMMATCH"});
    CHECK(npc->Choose(2));
    CHECK(rows() == std::vector<std::string>{"SN_NPC_BATTLE_ARENA_MANAGER_RANDOMMATCH_BS", "1. UIIT_STT_ARENA_NPC_JOIN", "2. UIIT_STT_NPC_CHATTING_WND_TALKEND"});
    CHECK(sent.empty());
    // The guild mode's row and talk.
    arena.Apply(proto::ArenaDesk{kManager, 0x22});
    npc->ShowArenaDesk();
    CHECK(rows()[1] == "2. UIIT_STT_ARENA_GUILD");
    CHECK(npc->Choose(2));
    CHECK(rows()[0] == "SN_NPC_BATTLE_ARENA_MANAGER_GUILD_BS");
    // Registered: "Cancel Registration" sends the cancel at once.
    arena.Apply(proto::ArenaDesk{kManager, game::arena::kRegistered});
    npc->ShowArenaDesk();
    CHECK(rows()[1] == "2. UIIT_STT_ARENA_NPC_CANCEL");
    CHECK(npc->Choose(2));
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].first == proto::ArenaRegister::kId);
    proto::ArenaRegister cancel;
    REQUIRE(proto::Decode(sent[0].second, cancel));
    CHECK(cancel.action == 2);
    // No mode open, or another NPC: no row.
    arena.Apply(proto::ArenaDesk{kManager, 4});
    npc->ShowNpc(kManager, "NPC_BATTLE_ARENA_MANAGER", "SN_NPC_BATTLE_ARENA_MANAGER", proto::kNpcTalk);
    CHECK(rows().size() == 1);
    arena.Apply(proto::ArenaDesk{kManager, 0x20});
    npc->ShowNpc(kManager + 1, "NPC_CH_SMITH", "SN_NPC_CH_SMITH", proto::kNpcTalk);
    CHECK(rows().size() == 1);
}

TEST_CASE("the Battle Arena's windows: the rule window's kinds and the request, the guild window's 16, the score, rank and result windows (game files)") {
    namespace proto = game::proto;
    const char* dirVar = std::getenv("SR_GAME_DIR");
    if (dirVar == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipping");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirVar, error), error);
    FakeResources resources;
    std::map<std::string, assets::UiLayout> layouts;
    ui::Interface interface{resources, [&](const std::string& path) {
                                const auto it = layouts.find(path);
                                return it == layouts.end() ? nullptr : &it->second;
                            }};
    ui::RegisterControls(interface);
    std::map<std::string, std::vector<assets::Ui2dtRecord>> layouts2dt;
    for (const char* path : {"res_ui\\arena_rule_select.2dt", "res_ui\\arena_guilduser_select.2dt", "res_ui\\arena_game_score.2dt", "res_ui\\arena_game_rank.2dt",
                             "res_ui\\arena_game_result.2dt"}) {
        std::vector<assets::Ui2dtRecord> records;
        if (const auto file = fs.Read(path); file && assets::DecodeUi2dt(file->Bytes(), records, error)) layouts2dt[path] = std::move(records);
    }
    REQUIRE(layouts2dt.size() == 5);
    game::PlayerState state;
    game::ArenaView arena;
    game::GuildView guild;
    proto::GuildInfo info;
    info.id = 7;
    info.name = "Dragons";
    for (int i = 0; i < 18; ++i) {
        proto::GuildMemberRecord m;
        m.character = std::uint32_t(100 + i);
        m.name = "M" + std::to_string(i);
        m.memberClass = i == 0 ? 0 : 10;
        m.level = std::uint8_t(40 + i);
        info.members.push_back(m);
    }
    guild.Apply(proto::GuildEvent{info});
    Hud hud;
    sr::client::HudContext context;
    context.state = &state;
    context.hud = &hud;
    context.arena = &arena;
    context.guild = &guild;
    context.text = [](const std::string& key) { return key; };
    context.layout2dt = [&](const std::string& path) -> const std::vector<assets::Ui2dtRecord>* {
        const auto it = layouts2dt.find(path);
        return it == layouts2dt.end() ? nullptr : &it->second;
    };
    std::vector<std::pair<proto::MessageId, std::vector<std::byte>>> sent;
    context.eventRequest = [&](proto::MessageId m, std::vector<std::byte> payload) { sent.push_back({m, std::move(payload)}); };
    sr::client::RegisterHudWindows(interface, context);
    hud.Bind(&state);
    hud.Create(interface, HudHarness::kScreen);
    ui::Context ui;
    ui::Input input;
    double time = 0.0;
    const auto frame = [&] {
        time += 1.0 / 60.0;
        context.time = time;
        input.BeginFrame(time);
        ui.NewFrame(input, HudHarness::kScreen, false, false);
        interface.Frame(ui, 1.0f, time, 1.0f / 60.0f);
        hud.Sync();
        ui.EndFrame();
    };
    frame();

    // The rule window (162) with both kinds: score then flag; nothing selected, the description empty.
    sr::client::OpenArenaRule(context, 0, game::arena::kScoreBit | game::arena::kFlagBit);
    frame();
    ui::Widget* rule = interface.Window(162);
    REQUIRE(rule != nullptr);
    CHECK(rule->visible);
    CHECK(rule->Child(0)->text() == "UIIT_STT_ARENA_JOIN_WINDOW");
    REQUIRE(rule->Child(10) != nullptr);
    CHECK(rule->Child(10)->Child(29)->text() == "UIIT_STT_ARENA_SCORE");
    CHECK(rule->Child(11)->Child(29)->text() == "UIIT_STT_ARENA_FLAG");
    CHECK(rule->Child(12)->Child(29)->text().empty());
    auto* description = ui::As<ui::TextBox>(rule->Child(30));
    REQUIRE(description != nullptr);
    CHECK(description->paragraphs().empty());
    CHECK_FALSE(hud.AnswerConfirm(true));
    // The flag row picked: selected, its description; Register asks, Yes sends {1, 0, 2} and closes.
    CHECK(sr::client::ArenaPickKind(context, game::arena::kFlag));
    REQUIRE(description->paragraphs().size() == 1);
    CHECK(description->paragraphs()[0].text == "UIIT_MSG_ARENA_JOIN_WINDOW_DESCRIPTION_FLAG");
    CHECK(dynamic_cast<ui::Bar*>(rule->Child(11)->Child(27))->pieces == "interface\\ifcommon\\com_bar01select_");
    CHECK(dynamic_cast<ui::Bar*>(rule->Child(10)->Child(27))->pieces.empty());
    CHECK(sr::client::ArenaRegisterKind(context, game::arena::kFlag));
    CHECK(sent.empty());
    CHECK(hud.AnswerConfirm(true));
    REQUIRE(sent.size() == 1);
    proto::ArenaRegister r;
    REQUIRE(proto::Decode(sent[0].second, r));
    CHECK((r.action == 1 && r.mode == 0 && r.kind == game::arena::kFlag && r.members.empty()));
    frame();
    CHECK_FALSE(sr::client::ArenaRuleShown(context));

    // The guild window (160): all 18 members on the left; at most 16 moved right; Confirm opens the rule window in the
    // guild mode, whose request carries the chosen.
    sr::client::OpenArenaGuild(context, game::arena::kScoreBit);
    frame();
    ui::Widget* pick = interface.Window(160);
    REQUIRE(pick != nullptr);
    CHECK(pick->Child(20)->text() == "UIIT_STT_ARENA_CONSTITUTION_WINDOW_GUILDSMAN");
    std::vector<std::string> names;
    for (int i = 0; i < 17; ++i) names.push_back("M" + std::to_string(i));
    CHECK_FALSE(sr::client::ArenaGuildChoose(context, names, false)); // the 17th refused
    CHECK(pick->Child(100)->Child(12)->text() == "M16");
    CHECK(pick->Child(100)->Child(11)->text() == "56");
    CHECK(pick->Child(200)->Child(12)->text() == "M0");
    CHECK(sr::client::ArenaGuildChoose(context, {}, true));
    frame();
    CHECK_FALSE(pick->visible);
    REQUIRE(sr::client::ArenaRuleShown(context));
    CHECK(sr::client::ArenaRegisterKind(context, game::arena::kScore));
    CHECK(hud.AnswerConfirm(true));
    REQUIRE(sent.size() == 2);
    REQUIRE(proto::Decode(sent[1].second, r));
    CHECK((r.mode == 2 && r.kind == game::arena::kScore && r.members.size() == 16 && r.members[0] == 100 && r.members[15] == 115));

    // The score (200) and rank (163) windows with the state, hidden until shown, at the screen's right.
    sr::client::ArenaWindows(context);
    CHECK(interface.Window(200) == nullptr);
    proto::ArenaNotice soon;
    soon.code = 4;
    soon.kind = game::arena::kScore;
    arena.Apply(soon, time);
    sr::client::ArenaWindows(context);
    const auto count = [&](int id) {
        int n = 0;
        for (ui::Widget* w : interface.Windows()) n += w->id() == id ? 1 : 0;
        return n;
    };
    sr::client::ArenaWindows(context); // made once, kept hidden
    CHECK(count(200) == 1);
    CHECK(count(163) == 1);
    CHECK(interface.Window(200) == nullptr);
    arena.Apply(proto::ArenaScores{12, 30, {{1, "Walker", 18}, {0, "Hero", 12}}});
    sr::client::ArenaWindows(context);
    frame();
    REQUIRE(interface.Window(200) != nullptr);
    REQUIRE(interface.Window(163) != nullptr);
    CHECK(interface.Window(200)->rect.x == hud.Screen().x - 262.0f);
    CHECK(interface.Window(200)->rect.y == 258.0f);
    CHECK(interface.Window(163)->rect.y == 288.0f);
    ui::Widget* rank = interface.Window(163);
    CHECK(rank->visible);
    CHECK(rank->Child(30)->visible);
    CHECK(rank->Child(30)->Child(100)->text() == "1.");
    CHECK(rank->Child(30)->Child(102)->text() == "Walker");
    CHECK(rank->Child(30)->Child(103)->text() == "18");
    CHECK(dynamic_cast<ui::Static*>(rank->Child(30)->Child(101))->texture == "interface\\guild\\gil_red_dregon.ddj");
    CHECK(dynamic_cast<ui::Static*>(rank->Child(31)->Child(101))->texture == "interface\\guild\\gil_blue_tiger.ddj");
    CHECK_FALSE(rank->Child(32)->visible);

    // The result (181), centred; closed; the stop takes everything.
    arena.Apply(proto::ArenaResult{0, 2, 1, 2800, 12, 30});
    sr::client::ArenaWindows(context);
    frame();
    ui::Widget* result = interface.Window(181);
    REQUIRE(result != nullptr);
    CHECK(result->rect.x == std::floor(hud.Screen().x / 2.0f) - 260.0f);
    CHECK(result->rect.y == std::floor(hud.Screen().y / 2.0f) - 133.0f);
    CHECK(dynamic_cast<ui::Static*>(result->Child(20))->texture == "interface\\guild\\gil_lose.ddj");
    CHECK(result->Child(21)->text() == "UIIT_MSG_ARENA_RESULT_WINDOW_HONORMEDAL");
    CHECK(sr::client::CloseArenaResult(context));
    CHECK_FALSE(arena.Result());
    CHECK(interface.Window(181) == nullptr);
    proto::ArenaNotice stop;
    stop.code = 5;
    arena.Apply(stop, time);
    sr::client::ArenaWindows(context);
    frame(); // closed windows go at the next frame
    CHECK(count(200) == 0);
    CHECK(count(163) == 0);
}
