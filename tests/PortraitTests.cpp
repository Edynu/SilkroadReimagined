// The HUD's portraits: a picture under a mask draws with the mask's alpha
// only; the player's bar shows its portrait under pmi_face.ddj, the quick party bars and the party
// page the members' (and the far ones darkened); and, over the game's own files, every player figure
// has what a portrait is posed by (ANI_POSE in its empty-handed set) and framed on (its Bip01 Head
// bone).

#include <doctest/doctest.h>

#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "gui/PortraitTexture.h"
#include "sr/assets/Animation.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/assets/Model.h"
#include "sr/assets/UiLayout.h"
#include "sr/core/Path.h"
#include "sr/core/Text.h"
#include "sr/game/PlayerState.h"
#include "sr/game/WorldView.h"
#include "sr/ui/Context.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

using namespace sr;

namespace {

// Every texture resident at 16 x 16 under its own index (the mask's and the picture's told apart).
class IndexedResources final : public ui::Resources {
public:
    Texture GetTexture(const std::string& path) override {
        auto [it, added] = indexes_.try_emplace(path, std::uint32_t(indexes_.size() + 1));
        return {ui::TextureRef{it->second}, 16.0f, 16.0f, true, false};
    }
    std::uint32_t Index(const std::string& path) { return GetTexture(path).ref.index; }
    const std::string& Text(const std::string& key) const override { return key; }
    ui::Font* GetFont() const override { return nullptr; }

private:
    std::map<std::string, std::uint32_t> indexes_;
};

const char* GameDirVar() { return std::getenv("SR_GAME_DIR"); }

// The HUD built from the game's own layouts, headless, on a player standing at Jangan.
struct GameHud {
    assets::FileSystem fs;
    IndexedResources resources;
    std::map<std::string, assets::UiLayout> layouts;
    std::unique_ptr<ui::Interface> interface;
    game::PlayerState state;
    game::WorldView world;
    client::Hud hud;
    client::HudContext context;
    ui::Context ui;
    ui::Input input;
    double time = 0.0;
    static constexpr std::uint16_t kRegion = (97 << 8) | 168;

    bool Load(const char* dir, std::string& error) {
        if (!assets::MountGameArchives(fs, dir, error)) return false;
        assets::UiDefines defines;
        if (const auto file = fs.Read("config\\define.txt")) defines = assets::DecodeUiDefines(core::DecodeTextFile(file->Bytes()));
        for (const std::string& path : fs.ListFiles()) {
            const std::string key = core::NormalizeAssetPath(path);
            if (!key.starts_with("resinfo\\") || !key.ends_with(".txt")) continue;
            assets::UiLayout layout;
            if (const auto file = fs.Read(key); file && assets::DecodeUiLayout(core::DecodeTextFile(file->Bytes()), defines, layout, error)) {
                layouts[key] = std::move(layout);
            }
        }
        interface = std::make_unique<ui::Interface>(resources, [this](const std::string& path) {
            const auto it = layouts.find(path);
            return it == layouts.end() ? nullptr : &it->second;
        });
        ui::RegisterControls(*interface);
        game::proto::WorldEntered entered;
        entered.entity = 1;
        entered.character.name = "Altay";
        entered.character.level = 7;
        entered.position = {kRegion, 960.0f, 0.0f, 960.0f};
        world.Apply(game::TitleEvent{entered});
        state.Apply(game::TitleEvent{entered});
        context.state = &state;
        context.world = &world;
        context.hud = &hud;
        client::RegisterHudWindows(*interface, context);
        hud.Bind(&state);
        hud.Create(*interface, {1600, 900});
        return true;
    }
    void Frame(std::initializer_list<ui::InputEvent> events = {}) {
        time += 1.0 / 60.0;
        context.time = time;
        input.BeginFrame(time);
        for (const ui::InputEvent& e : events) input.Feed(e);
        ui.NewFrame(input, {1600, 900}, false, false);
        interface->Frame(ui, 1.0f, time, 1.0f / 60.0f);
        hud.Sync();
        ui.EndFrame();
    }
    // A window by its id.
    ui::Widget* Window(int id) {
        for (ui::Widget* w : interface->Windows()) {
            if (w->id() == id) return w;
        }
        return nullptr;
    }
    // A page of a window (nested in it), by its id.
    ui::Widget* Page(int window, int id) {
        std::function<ui::Widget*(ui::Widget&)> search = [&](ui::Widget& w) -> ui::Widget* {
            if (w.id() == id) return &w;
            for (const auto& c : w.children()) {
                if (ui::Widget* found = search(*c)) return found;
            }
            return nullptr;
        };
        ui::Widget* w = Window(window);
        return w != nullptr ? search(*w) : nullptr;
    }
};

} // namespace

TEST_CASE("portraits: a picture under a mask draws opaque, its alpha the mask's") {
    IndexedResources resources;
    std::map<std::string, assets::UiLayout> layouts;
    ui::Interface interface{resources, [&](const std::string& path) {
                                const auto it = layouts.find(path);
                                return it == layouts.end() ? nullptr : &it->second;
                            }};
    ui::RegisterControls(interface);
    assets::UiLayoutSection section;
    section.name = "Create";
    assets::UiLayoutNode window;
    window.type = "CIFWnd";
    window.name = "W";
    window.id = 1;
    window.rect = {10, 10, 100, 100};
    section.nodes.push_back(window);
    assets::UiLayoutNode face;
    face.type = "CIFStaticWithPictureClip";
    face.name = "FACE";
    face.id = 12;
    face.rect = {15, 7, 48, 48};
    section.nodes.push_back(face);
    assets::UiLayout layout;
    layout.sections.push_back(section);
    layouts["resinfo\\face.txt"] = layout;
    ui::Widget* w = interface.Open("resinfo\\face.txt", "Create", 0, {});
    REQUIRE(w != nullptr);
    auto* picture = ui::As<ui::PictureClip>(w->Child(12));
    REQUIRE(picture != nullptr);
    picture->texture = client::PortraitTexture(client::kSelfPortrait);
    picture->mask = "interface\\playerminiinfo\\pmi_face.ddj";

    ui::Context context;
    ui::Input input;
    input.BeginFrame(0.0);
    context.NewFrame(input, {1024, 768}, false, false);
    interface.Frame(context, 1.0f, 0.0, 1.0f / 60.0f);
    context.EndFrame();
    // One command draws the picture: its texture opaque, the mask over its rect.
    int found = 0;
    for (const ui::DrawList* list : context.DrawOrder()) {
        for (const ui::DrawCommand& c : list->commands()) {
            if (c.texture.index != resources.Index(picture->texture)) continue;
            ++found;
            CHECK(c.texture.opaque);
            CHECK(c.mask.index == resources.Index(picture->mask));
            CHECK(c.maskRect.w == doctest::Approx(48.0f));
            CHECK(c.maskRect.h == doctest::Approx(48.0f));
        }
    }
    CHECK(found == 1);
}

TEST_CASE("portraits: the player's bar shows the player's portrait under pmi_face.ddj (game files)") {
    const char* dir = GameDirVar();
    if (dir == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipping");
        return;
    }
    GameHud h;
    std::string error;
    REQUIRE_MESSAGE(h.Load(dir, error), error);
    ui::Widget* bar = h.Window(client::hud::kPlayerInfo);
    REQUIRE(bar != nullptr);
    // Control 12, GDR_PMI_PICTURE (15, 7, 48 x 48), the 1.188 client's picture with a mask.
    auto* face = ui::As<ui::PictureClip>(bar->Child(12));
    REQUIRE(face != nullptr);
    CHECK(face->rect.w == doctest::Approx(48.0f));
    CHECK(face->rect.h == doctest::Approx(48.0f));
    CHECK(face->texture == client::PortraitTexture(client::kSelfPortrait));
    CHECK(face->mask == "interface\\playerminiinfo\\pmi_face.ddj");
    CHECK(static_cast<bool>(h.fs.Read("interface\\playerminiinfo\\pmi_face.ddj")));
}

TEST_CASE("portraits: the quick party bars and the party page show the members' faces (game files)") {
    const char* dir = GameDirVar();
    if (dir == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipping");
        return;
    }
    GameHud h;
    std::string error;
    REQUIRE_MESSAGE(h.Load(dir, error), error);
    // Ann leads (in sight, near), the player, and Kira out of sight 750 away (the second grade).
    game::proto::EntityAppeared ann;
    ann.entity = 10;
    ann.character.name = "Ann";
    ann.position = {GameHud::kRegion, 1000.0f, 0.0f, 960.0f};
    h.world.Apply(game::TitleEvent{ann});
    game::proto::PartyInfo info;
    info.party = 7;
    info.leader = 101;
    const auto member = [](std::uint32_t id, std::uint32_t entity, const char* name, float x) {
        game::proto::PartyMemberRecord m;
        m.id = id;
        m.entity = entity;
        m.name = name;
        m.model = 1907;
        m.position = {GameHud::kRegion, x, 0.0f, 960.0f};
        return m;
    };
    info.members = {member(101, 10, "Ann", 1000.0f), member(102, 1, "Altay", 960.0f), member(103, 0, "Kira", 960.0f - 750.0f)};
    // Ann's two highest masteries (the bar's icons); Kira has none.
    info.members[0].mastery1 = 257;
    info.members[0].mastery2 = 275;
    assets::SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(h.fs, error), error);
    h.context.skills = &skills;
    h.state.Apply(game::TitleEvent{info});
    REQUIRE(h.state.party.InParty());
    h.hud.OpenPage(client::hud::kPageParty);
    h.Frame();
    h.Frame();

    // The quick party bars: the other members in turn, each face its portrait under qpt_face.ddj.
    ui::Widget* quick = h.Window(client::hud::kQuickParty);
    REQUIRE(quick != nullptr);
    const std::pair<std::uint32_t, int> bars[] = {{101, 0}, {103, 2}}; // member, distance grade
    for (int i = 0; i < 2; ++i) {
        ui::Widget* slot = quick->Child(1 + i);
        REQUIRE(slot != nullptr);
        auto* face = ui::As<ui::PictureClip>(slot->Child(1));
        REQUIRE(face != nullptr);
        CHECK(face->texture == client::PortraitTexture(bars[i].first));
        CHECK(face->mask == "interface\\quickparty\\qpt_face.ddj");
        ui::Widget* far = slot->Child(5);
        REQUIRE(far != nullptr);
        CHECK(far->visible == (bars[i].second > 0));
        if (bars[i].second == 2) CHECK(ui::As<ui::Static>(far)->texture == "interface\\quickparty\\qpt_face_faraway_70.ddj");
        // The masteries under the face, 17 x 17: each one's icon, none hidden.
        auto* first = ui::As<ui::Static>(slot->Child(0x65));
        auto* second = ui::As<ui::Static>(slot->Child(0x66));
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        CHECK(first->visible == (i == 0));
        CHECK(second->visible == (i == 0));
        if (i == 0) {
            CHECK(first->texture == skills.Mastery(257)->iconPath);
            CHECK(second->texture == skills.Mastery(275)->iconPath);
            CHECK(static_cast<bool>(h.fs.Read(first->texture)));
            CHECK(first->rect.w == doctest::Approx(17.0f));
            CHECK(first->rect.y == doctest::Approx(39.0f));
        }
        // The slots 73 apart (the party buffs option on).
        CHECK(slot->rect.y == doctest::Approx(73.0f * float(i)));
    }
    // A click on a member's bar selects it (in sight), as for a heal on it: no click in the world needed.
    {
        h.hud.TakeRequests();
        const ui::Rect bar = quick->Child(1)->ScreenRect();
        ui::InputEvent move;
        move.type = ui::InputEvent::Type::MouseMove;
        move.position = {bar.x + 60.0f, bar.y + 10.0f};
        h.Frame({move});
        ui::InputEvent down = move;
        down.type = ui::InputEvent::Type::MouseDown;
        h.Frame({down});
        ui::InputEvent up = move;
        up.type = ui::InputEvent::Type::MouseUp;
        h.Frame({up});
        bool selected = false;
        for (const client::HudRequest& r : h.hud.TakeRequests()) selected = selected || (r.kind == client::HudRequest::Kind::SelectEntity && r.value == 10);
        CHECK(selected);
    }
    // The party page: the master's row shows Ann's, the player's row the player's own.
    ui::Widget* page = h.Page(client::hud::kMainPopup, client::hud::kPageParty);
    REQUIRE(page != nullptr);
    auto* master = ui::As<ui::PictureClip>(page->Child(41));
    REQUIRE(master != nullptr);
    CHECK(master->texture == client::PortraitTexture(101));
    CHECK(master->mask == "interface\\party\\pt_face.ddj");
    ui::Widget* first = page->Child(30);
    REQUIRE(first != nullptr);
    auto* own = ui::As<ui::PictureClip>(first->Child(9));
    REQUIRE(own != nullptr);
    CHECK(own->texture == client::PortraitTexture(client::kSelfPortrait));
    CHECK(own->mask == "interface\\party\\pt_face.ddj");
    for (const char* path : {"interface\\quickparty\\qpt_face.ddj", "interface\\party\\pt_face.ddj", "interface\\quickparty\\qpt_face_faraway_70.ddj"}) {
        CHECK_MESSAGE(static_cast<bool>(h.fs.Read(path)), path);
    }
}

TEST_CASE("portraits: every player figure has its pose clip and its head bone (game files)") {
    const char* dir = GameDirVar();
    if (dir == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipping");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    // The player figures (TID 1 / 1: a character, a player) in service, as characterdata lists them.
    int figures = 0, noModel = 0;
    for (const auto& [code, row] : characters.rows()) {
        if (row.typeId[0] != 1 || row.typeId[1] != 1 || !row.inService) continue;
        const std::string path = assets::CharacterTable::ResolveModel(fs, row);
        if (path.empty()) {
            ++noModel;
            MESSAGE("no model: " << code);
            continue;
        }
        assets::ModelData model;
        REQUIRE_MESSAGE(assets::LoadModel(fs, path, model, error), code << ": " << error);
        ++figures;
        CHECK_MESSAGE(model.skeleton.Find("Bip01 Head") >= 0, code << ": no Bip01 Head");
        // ANI_POSE (state 60) in the empty-handed set, a clip that decodes.
        assets::ResourceData sets;
        sets.animationPaths = model.animationPaths;
        sets.animationSets = model.animationSets;
        const std::string pose = sets.StateAnimation("default", 60);
        CHECK_MESSAGE(!pose.empty(), code << ": no ANI_POSE");
        if (pose.empty()) continue;
        const auto file = fs.Read(pose);
        REQUIRE_MESSAGE(static_cast<bool>(file), pose);
        assets::AnimationData clip;
        CHECK_MESSAGE(assets::DecodeBan(file->Bytes(), clip, error), pose << ": " << error);
    }
    MESSAGE(figures << " player figures, " << noModel << " without a model");
    CHECK(figures > 0);
}
