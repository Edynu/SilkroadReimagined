#include "title/CreateScreen.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "audio/GameAudio.h"
#include "sr/assets/Dress.h"
#include "sr/core/Log.h"
#include "sr/core/Trace.h"
#include "title/CameraKeys.h"

namespace sr::client {

namespace proto = game::proto;
using Race = game::CreationRules::Race;

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kLayoutHeight = 1200.0f;

// The scenes: the town's region and the point the world camera turns on.
struct Scene {
    int regionX, regionY;
    float x, y, z;
};
constexpr Scene kEuropeScene{79, 105, 335.0f, 140.0f, 1460.0f};
constexpr Scene kChinaScene{168, 98, 960.42f, 50.0f, 458.26f};

// The character window (CIFCharacterWnd): the character stands at (3, 0.5, 0) in its own space,
// seen by its own camera key; the zoom moves that key in 1 s.
constexpr float kCharacterX = 3.0f, kCharacterY = 0.5f, kCharacterZ = 0.0f;

assets::CameraKey ViewKey(float time, bool zoomed, bool male, float height) {
    if (!zoomed) return MakeKey(time, 0, 0, 0.5f, 9.8f, 0.0f, 0.2f, 0.0f, 28.5f);
    return MakeKey(time, 0, 0, 2.0f, (male ? 15.0f : 14.0f) * height, 0.0f, 0.2f, 0.0f, 11.0f);
}

// A key's view as the scene renderer's fly camera has it (ApplyCameraKey's eye behind the point).
glm::mat4 KeyView(const assets::CameraKey& k) {
    const float p = k.rotation[0], yaw = k.rotation[1];
    const glm::vec3 forward(std::cos(p) * std::sin(yaw), -std::sin(p), std::cos(p) * std::cos(yaw));
    const glm::vec3 eye = glm::vec3(k.position[0], k.position[1], k.position[2]) - forward * k.distance;
    return glm::lookAt(eye, eye + forward, glm::vec3(0, 1, 0));
}

// The figures' text names, in the figures' id order.
constexpr const char* kEuropeWomen[] = {"NOBLE",  "WITCH",      "SUMMONER",  "MERCHANT", "ORACLE", "CRUSADER", "AMAZONESS",
                                        "KNIGHT", "ADVENTURER", "GLADIATOR", "ANGEL",    "DEVIL",  "SUCCUBUS"};
constexpr const char* kEuropeMen[] = {"NOBLE",     "EXORCIST",  "NECROMANCER", "MERCHANT", "PRIEST", "KNIGHT",  "WARRIOR",
                                      "GLADIATOR", "BARBARIAN", "ADVENTURER",  "ANGEL",    "DEVIL",  "WEREWOLF"};
constexpr const char* kChinaWomen[] = {"ADVENTURER",   "ASSASSIN",     "BOGY",      "FIGHTER", "FOX",    "KANGSI", "KISAENG",
                                       "MERCHANT",     "NECROMANCERB", "NECROMANCERW", "NOBLEGIRL", "SCHOLAR", "WARRIOR"};
constexpr const char* kChinaMen[] = {"ADVENTURER", "BOGY",      "FIGHTER", "MERCHANT", "MONK",   "MONKEY", "NECROMANCER",
                                     "NOBLEBOY",   "PERFORMER", "PRIEST",  "SCHOLAR",  "TATTOO", "WARRIOR"};

// A weapon's text name by its class (the fourth type id).
const char* WeaponText(int weaponClass) {
    switch (weaponClass) {
    case 2: return "SWORD";
    case 3: return "BLADE";
    case 4: return "SPEAR";
    case 5: return "TBLADE";
    case 6: return "BOW";
    case 7: return "ONEHANDSWORD";
    case 8: return "TWOHANDSWORD";
    case 9: return "DUELAXE";
    case 10: return "DARKSTAFF";
    case 11: return "TWOHANDSTAFF";
    case 12: return "CROSSBOW";
    case 13: return "DAGGER";
    case 14: return "HARP";
    case 15: return "ONEHANDSTAFF";
    default: return nullptr;
    }
}

std::array<float, 12> Rows(const glm::mat4& m) {
    std::array<float, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) out[static_cast<std::size_t>(4 * r + c)] = m[c][r];
    }
    return out;
}

} // namespace

void CreateScreen::Enter(ScreenContext& context) {
    shared_ = TitleShared::From(context);
    context.handoff.erase(TitleShared::kHandoff);
    if (!shared_) {
        core::LogWarning("create: no title session handed over; back to the title");
        next = "title";
        return;
    }
    shared_->LoadCharacters(context.jobs, context.files);
    tables_ = shared_->characters;
    europe_ = shared_->race == 0;
    textures_ = std::make_unique<UiTextures>(context.assets, context.uiWarmup);
    kit_ = std::make_unique<UiKit>(ui_, *textures_, shared_->data);
    const Scene& scene = europe_ ? kEuropeScene : kChinaScene;
    WorldScreen::Backdrop backdrop{scene.regionX, scene.regionY, 1};
    backdrop.fovY = 45.0f * kPi / 180.0f; // the character window's 45 (inferred: its field of view)
    world_ = std::make_unique<WorldScreen>(backdrop);
    world_->Enter(context);
    loading_.Begin(context.assets, europe_ ? "interface\\loading\\loading_charactercustom_europe.ddj" : "interface\\loading\\loading_charactercustom.ddj");

    // The setup's choices: a man, the first figure, height and volume in the
    // middle, no weapon; China's armour starts at its first kind, Europe's at none.
    male_ = context.config.GetString("sex", "man") != "woman";
    value_[kArmour] = europe_ ? 0 : 1;
    if (const std::string look = context.config.GetString("look", ""); !look.empty()) {
        int i = 0;
        for (std::size_t start = 0; start <= look.size() && i < kSliders; ++i) {
            const std::size_t comma = std::min(look.find(',', start), look.size());
            value_[i] = std::atoi(look.substr(start, comma - start).c_str());
            start = comma + 1;
        }
    }
    view_ = {ViewKey(0.0f, false, male_, 1.0f)};
    autoName_ = context.config.GetString("create-name", "");
    exitOnCreate_ = context.config.GetBool("exit-on-create", false);
    clock_ = context.config.GetDouble("create-clock", 0.0); // seconds into the town's turn (captures)
}

void CreateScreen::Exit(ScreenContext& context, FrameSnapshot& snapshot) {
    if (shared_) context.jobs.Wait(shared_->charactersJob); // it reads the file system
    loading_.End(context.assets);
    if (world_) world_->Exit(context, snapshot);
    world_.reset();
    if (const rhi::TextureHandle t = atlas_.Release()) snapshot.releaseTextures.push_back(t);
    textures_.reset();
    kit_.reset();
    shared_.reset();
}

void CreateScreen::Leave() {
    // To black, then the character select (it fades in from black).
    if (leaving_) return;
    leaving_ = true;
    screenFade_.Out(0.5f);
}

std::pair<int, int> CreateScreen::Range(Slider which) const {
    if (!tables_->Dressable()) return {0, 0};
    const game::CreationRules& c = tables_->rules.creation;
    const Race race = europe_ ? Race::Europe : Race::China;
    switch (which) {
    case kFigure: return {1, std::max(1, int(c.FiguresOf(race, male_).size()))};
    case kHeight:
    case kVolume: return {0, 4};
    case kWeapon: return {0, int(c.WeaponsOf(race).size())};
    case kArmour:
        if (!europe_) return {1, int(c.ArmourOf(race, male_).size())};
        // Europe's by the weapon's class.
        switch (WeaponClass()) {
        case 7: case 8: case 9: case 15: return {0, 2};
        case 10: case 11: case 12: case 13: case 14: return {0, 1};
        default: return {0, 0};
        }
    default: return {0, 0};
    }
}

int CreateScreen::WeaponClass() const {
    if (!tables_->Dressable() || value_[kWeapon] <= 0) return 0;
    const auto weapons = tables_->rules.creation.WeaponsOf(europe_ ? Race::Europe : Race::China);
    if (value_[kWeapon] > int(weapons.size())) return 0;
    const assets::ItemRow* row = tables_->items.Find(weapons[std::size_t(value_[kWeapon] - 1)]);
    return row ? row->typeId[3] : 0;
}

int CreateScreen::ArmourSet() const {
    const int v = value_[kArmour];
    if (!europe_) return v;
    // Europe: the armour slider's value per weapon class; 1 heavy, 2 light, 3 clothes.
    switch (WeaponClass()) {
    case 7: case 8: case 9: return v;
    case 10: case 11: case 14: return v == 1 ? 3 : 0;
    case 12: case 13: return v == 1 ? 2 : 0;
    case 15: return v == 1 ? 2 : v == 2 ? 3 : 0;
    default: return 0;
    }
}

CreateScreen::Choice CreateScreen::Chosen() const {
    Choice out;
    if (!tables_->Dressable()) return out;
    const game::CreationRules& c = tables_->rules.creation;
    const Race race = europe_ ? Race::Europe : Race::China;
    const auto figures = c.FiguresOf(race, male_);
    if (value_[kFigure] >= 1 && value_[kFigure] <= int(figures.size())) out.model = figures[std::size_t(value_[kFigure] - 1)];
    const auto armour = c.ArmourOf(race, male_);
    if (const int set = ArmourSet(); set >= 1 && set <= int(armour.size())) {
        const auto& a = armour[std::size_t(set - 1)];
        out.chest = a.chest;
        out.legs = a.legs;
        out.shoes = a.shoes;
    }
    const auto weapons = c.WeaponsOf(race);
    if (value_[kWeapon] >= 1 && value_[kWeapon] <= int(weapons.size())) {
        out.weapon = weapons[std::size_t(value_[kWeapon] - 1)];
        out.shield = c.ShieldFor(out.weapon);
    }
    return out;
}

void CreateScreen::SendCreate() {
    const Choice choice = Chosen();
    proto::CharacterCreate m;
    m.name = name_;
    m.model = choice.model;
    m.height = assets::PackShape(value_[kHeight], value_[kVolume]);
    m.chest = choice.chest;
    m.legs = choice.legs;
    m.shoes = choice.shoes;
    m.weapon = choice.weapon;
    shared_->session->Create(m);
    busy_ = true;
}

void CreateScreen::Dress(ScreenContext& context) {
    if (!lookDirty_ || !tables_->Dressable()) return;
    lookDirty_ = false;
    for (int i = 0; i < kSliders; ++i) {
        const auto [min, max] = Range(Slider(i));
        value_[i] = std::clamp(value_[i], min, max);
    }
    const Choice choice = Chosen();
    if (choice.model == 0) return;
    std::vector<std::uint32_t> worn;
    for (const std::uint32_t item : {choice.chest, choice.legs, choice.shoes, choice.weapon, choice.shield}) {
        if (item != 0) worn.push_back(item);
    }
    assets::DressedCharacter dressed;
    std::string error;
    if (!assets::Dress(context.files, tables_->characters, tables_->items, choice.model, worn,
                       assets::PackShape(value_[kHeight], value_[kVolume]), dressed, error)) {
        core::LogWarning("create: {}", error);
        return;
    }
    WorldScreen::StageLook look;
    look.model = dressed.Key();
    look.stance = dressed.stance;
    look.volume = dressed.volume;
    look.shadow = false; // the character window has no ground
    look.overlay = true; // CIFCharacterWnd: over the bars and the back image, under the windows
    height_ = dressed.height;
    world_->SetStage(0, look, 0, 0, 0, 0);
}

void CreateScreen::HandleEvents(ScreenContext& context) {
    for (game::TitleEvent& event : shared_->session->TakeEvents()) {
        if (auto* result = std::get_if<proto::LobbyResult>(&event)) {
            busy_ = false;
            const bool ok = result->status == proto::LobbyStatus::Ok;
            if (result->action == proto::LobbyAction::NameCheck) {
                message_ = ok ? "UIO_MSG_ERROR_ADMISSON" : LobbyStatusText(shared_->data, result->status);
            } else if (result->action == proto::LobbyAction::Create) {
                if (ok) core::LogInfo("create: {} created", name_);
                if (exitOnCreate_) {
                    context.quit = true;
                    context.exitCode = ok ? 0 : 1;
                }
                if (ok) {
                    Leave(); // back to the character select
                } else {
                    message_ = LobbyStatusText(shared_->data, result->status);
                    warning_ = false;
                }
            }
        } else if (auto* error = std::get_if<game::TitleError>(&event)) {
            core::LogWarning("create: {}", error->reason);
            message_ = "UIO_MSG_ERROR_SEVER_CONNECT";
            busy_ = false;
        }
    }
}

void CreateScreen::UpdateScene(ScreenContext& context, FrameSnapshot& snapshot) {
    if (!world_) return;
    Dress(context);
    const Scene& scene = europe_ ? kEuropeScene : kChinaScene;
    const float dt = float(context.dt);
    // The town turns slowly: 1 radian in 30 s.
    const assets::CameraKey town = MakeKey(0.0f, scene.regionX, scene.regionY, scene.x, scene.y, scene.z, 0.0f,
                                           0.390955f + float(context.time + clock_) * 1000.0f / 30000.0f, 0.1f);
    ApplyCameraKey(*world_, town, scene.regionX, scene.regionY);

    // The character's spin slows down: it loses dt x speed / 2 a frame, stopping below 30.
    float slow = dt * spin_ * 0.5f;
    if (std::abs(spin_) < std::abs(slow)) slow = spin_;
    angle_ += (spin_ + spin_ - slow) * dt * 0.5f;
    spin_ -= slow;
    if (std::abs(spin_) < 30.0f) spin_ = 0.0f;
    viewTime_ = std::min(view_.back().time, viewTime_ + dt);

    // The character window's picture, made in the world: the character placed so that the town's
    // camera sees it as the window's camera would (W = town^-1 x window x model). It faces its camera
    // (inferred: the window's yaw 0 would show its back in our convention).
    const float yaw = kPi + angle_ / 180.0f * kPi;
    const float c = -std::cos(yaw) * height_, s = std::sin(yaw) * height_;
    glm::mat4 model(1.0f);
    model[0] = glm::vec4(c, 0, s, 0);
    model[1] = glm::vec4(0, height_, 0, 0);
    model[2] = glm::vec4(-s, 0, c, 0);
    model[3] = glm::vec4(kCharacterX, kCharacterY, kCharacterZ, 1);
    const assets::CameraKey window = assets::SampleCameraTrack(view_, viewTime_);
    const assets::CameraKey townView = MakeKey(0.0f, 0, 0, scene.x, scene.y, scene.z, town.rotation[0], town.rotation[1], town.distance);
    world_->SetStageTransform(0, Rows(glm::inverse(KeyView(townView)) * KeyView(window) * model));
    world_->Update(context, snapshot);
}

void CreateScreen::Update(ScreenContext& context, FrameSnapshot& snapshot) {
    if (!shared_) {
        context.holdCapture = true;
        return;
    }
    SR_ZONE("create: update");
    shared_->session->Update();
    HandleEvents(context);
    if (!next.empty()) {
        context.handoff[TitleShared::kHandoff] = shared_; // the session lives on in the lobby
        context.holdCapture = true;
        return;
    }
    UpdateScene(context, snapshot);
    screenFade_.Update(float(context.dt));
    if (leaving_ && screenFade_.Done()) {
        next = "characters";
        context.handoff[TitleShared::kHandoff] = shared_;
        context.holdCapture = true;
        return;
    }
    const float width = float(context.device.SwapchainWidth()), height = float(context.device.SwapchainHeight());
    if (!shared_->data.Ready()) {
        context.holdCapture = true;
        return;
    }
    layout_ = shared_->data.Layout(europe_ ? kCreateEuropeLayout : kCreateChinaLayout);
    ui::Font* font = shared_->data.Font();
    if (font) atlas_.Sync(context.device, *font);
    ui_.font = font;
    ui_.getClipboard = context.getClipboard;
    ui_.setClipboard = context.setClipboard;
    ui_.NewFrame(*context.input, {width, height}, context.mouseBlocked, context.keyboardBlocked);
    kit_->grid().FitAnchored(width, height);
    // Two layers, as the original stacks them: the bars, caption and back image under the character
    // window (snapshot.uiUnder), the windows over it.
    ui_.BeginLayer("create: under", ui::LayerKind::Background);
    ui_.draw().SetWhite(font ? font->texture() : ui::TextureRef{}, font ? font->whiteUv() : ui::Vec2{0.5f, 0.5f});
    const ui::DrawList* under = &ui_.draw();
    DrawUnder(width, height);
    ui_.EndLayer();
    ui_.BeginLayer("create", ui::LayerKind::Window);
    ui_.draw().SetWhite(font ? font->texture() : ui::TextureRef{}, font ? font->whiteUv() : ui::Vec2{0.5f, 0.5f});
    Draw(context, height);
    ui_.EndLayer();
    ui_.BeginLayer("create: fade", ui::LayerKind::Overlay);
    ui_.draw().SetWhite(font ? font->texture() : ui::TextureRef{}, font ? font->whiteUv() : ui::Vec2{0.5f, 0.5f});
    if (!shown_ && world_ && world_->Settled() && world_->StageReady(0)) {
        shown_ = true;
        screenFade_.In(0.5f);
        loading_.End(context.assets);
    }
    screenFade_.Draw(*kit_, width, height);
    ui_.EndLayer();
    ui_.EndFrame();
    if (font) atlas_.Sync(context.device, *font);
    for (const ui::DrawList* list : ui_.DrawOrder()) AppendUi(list == under ? snapshot.uiUnder : snapshot.ui, *list);
    // Not in yet: the race's loading picture over it all, its gauge by the town's streaming.
    if (!shown_ && world_) {
        loading_.SetProgress(world_->StreamProgress(context.assets));
        loadingUi_.Clear();
        loading_.Draw(context.assets, loadingUi_, width, height);
        AppendUi(snapshot.ui, loadingUi_);
        // --capture-loading: the loading picture itself, once its pictures are in.
        // (the town's own wait is let go: this frame is the one wanted)
        if (context.config.GetBool("capture-loading", false) && loading_.Ready(context.assets) && loading_.Progress() > 0.0f) {
            context.holdCapture = false;
            return;
        }
    }
    context.textInput = ui_.typing();

    // Automated runs: once dressed, type the name, confirm and create.
    const bool dressed = tables_->Dressable() && world_->StageReady(0);
    if (!autoName_.empty() && dressed && !busy_ && !lookDirty_) {
        name_ = std::exchange(autoName_, {});
        warning_ = true;
        autoConfirm_ = true;
    }
    if (!tables_->Dressable() || !world_->Settled() || !dressed || !textures_->Settled() || viewTime_ < view_.back().time || busy_ ||
        !autoName_.empty() || autoConfirm_ || !screenFade_.Done()) {
        context.holdCapture = true;
    }
    if (!next.empty()) context.handoff[TitleShared::kHandoff] = shared_; // back to the lobby, the session with it
}

int CreateScreen::DrawSlider(Slider which, const assets::UiLayoutNode& node, float wx, float wy, int value, int min, int max,
                             bool enabled) {
    // A CIFSliderCtrl (section Slider): arrows at its ends, the thumb between them.
    const auto part = [&](const char* name) -> const assets::UiLayoutNode* {
        for (const auto& section : layout_->sections) {
            if (section.name != "Slider") continue;
            for (const auto& n : section.nodes) {
                if (n.name == name) return &n;
            }
        }
        return nullptr;
    };
    const assets::UiLayoutNode* prev = part("GDR_BTN_PREV");
    const assets::UiLayoutNode* nextArrow = part("GDR_BTN_NEXT");
    const assets::UiLayoutNode* thumb = part("GDR_BTN_THUMB");
    if (!prev || !nextArrow || !thumb) return value;
    const core::DesignRect s = UiKit::Local(node, {wx, wy, 0, 0});
    enabled = enabled && max > min && !busy_;
    ui_.PushId(int(which));
    bool hovered = false;
    const auto prevState = kit_->ButtonState("##prev", *prev, UiKit::Local(*prev, s), enabled && value > min);
    const auto nextState = kit_->ButtonState("##next", *nextArrow, UiKit::Local(*nextArrow, s), enabled && value < max);
    if (prevState.clicked) --value;
    if (nextState.clicked) ++value;
    // The track between the arrows: a press or drag puts the thumb under the mouse.
    const float left = s.x + prev->rect.x + prev->rect.w, right = s.x + nextArrow->rect.x;
    const float travel = std::max(1.0f, right - left - thumb->rect.w);
    const auto track = kit_->Item("##track", {left, s.y, right - left, node.rect.h}, enabled);
    if (track.held && enabled) {
        const core::DesignPoint mouse = kit_->grid().ToDesign({ui_.input().mouse().x, ui_.input().mouse().y});
        const float t = std::clamp((mouse.x - left - thumb->rect.w * 0.5f) / travel, 0.0f, 1.0f);
        value = min + int(std::lround(t * float(max - min)));
    }
    hovered = prevState.hovered || nextState.hovered || track.hovered;
    const float t = max > min ? float(value - min) / float(max - min) : 0.0f;
    kit_->Image(thumb->ddj, {std::round(left + t * travel), s.y + thumb->rect.y, thumb->rect.w, thumb->rect.h}, enabled ? 1.0f : 0.4f);
    ui_.PopId();
    if (hovered) topic_ = which;
    return std::clamp(value, min, max);
}

std::string CreateScreen::ExplainName() const {
    const bool female = !male_;
    switch (topic_) {
    case kFigure: {
        const int i = std::clamp(value_[kFigure], 1, 13) - 1;
        const char* name = europe_ ? (female ? kEuropeWomen[i] : kEuropeMen[i]) : (female ? kChinaWomen[i] : kChinaMen[i]);
        return std::string(europe_ ? "UIO_NEWCHAR_EU_" : "UIO_NEWCHAR_") + (female ? "FEMALE_" : "MAN_") + name;
    }
    case kHeight: return "UIO_NEWCHAR_STT_HEIGHT";
    case kVolume: return "UIO_NEWCHAR_STT_VOLUME";
    case kWeapon: {
        const char* weapon = WeaponText(WeaponClass());
        if (!weapon) return "UIO_NEWCHAR_STT_WEAPON";
        return std::string(europe_ ? "UIO_NEWCHAR_STT_EU_" : "UIO_NEWCHAR_STT_") + weapon;
    }
    case kArmour:
        switch (ArmourSet()) {
        case 1: return europe_ ? "UIO_NEWCHAR_STT_EU_HEAVY_ARMOR" : "UIO_NEWCHAR_STT_HEAVY_ARMOR";
        case 2: return europe_ ? "UIO_NEWCHAR_STT_EU_LIGHT_ARMOR" : "UIO_NEWCHAR_STT_LIGHT_ARMOR";
        case 3: return europe_ ? "UIO_NEWCHAR_STT_EU_ROBE" : "UIO_NEWCHAR_STT_CLOTHES";
        default: return "UIO_NEWCHAR_STT_PROTECTOR";
        }
    default: return {};
    }
}

std::string CreateScreen::ExplainText() const {
    switch (topic_) {
    case kFigure: return ExplainName() + "_EXPLANATION";
    case kHeight: return "UIO_NEWCHAR_EXPLANATION_HEIGHT";
    case kVolume: return "UIO_NEWCHAR_EXPLANATION_VOLUME";
    case kWeapon: {
        const char* weapon = WeaponText(WeaponClass());
        return weapon ? std::string("UIO_NEWCHAR_EXPLANATION_") + weapon : "UIO_NEWCHAR_EXPLANATION_ARMY_SELECT";
    }
    case kArmour:
        switch (ArmourSet()) {
        case 1: return europe_ ? "UIO_NEWCHAR_EXPLANATION_EU_HEAVY_ARMOR" : "UIO_NEWCHAR_EXPLANATION_HEAVY_ARMOR";
        case 2: return europe_ ? "UIO_NEWCHAR_EXPLANATION_EU_LIGHT_ARMOR" : "UIO_NEWCHAR_EXPLANATION_LIGHT_ARMOR";
        case 3: return europe_ ? "UIO_NEWCHAR_EXPLANATION_EU_ROBE" : "UIO_NEWCHAR_EXPLANATION_CLOTHES";
        default: return "UIO_NEWCHAR_EXPLANATION_ARMOR_SELECT";
        }
    default: return {};
    }
}

void CreateScreen::DrawCustom(float x, float y) {
    const UiData& data = shared_->data;
    const assets::UiLayoutNode* frame = layout_->Find("GDR_STA_CUSTOM");
    if (!frame) return;
    const core::DesignRect window{x, y, frame->rect.w, frame->rect.h};
    kit_->Image(frame->ddj, window);
    const Slider sliders[] = {kFigure, kHeight, kVolume, kWeapon, kArmour};
    const char* sliderNames[] = {"GDR_SLI_FIGURE", "GDR_SLI_HEIGHT", "GDR_SLI_VOLUME", "GDR_SLI_WEAPON", "GDR_SLI_PROTECTOR"};
    for (const auto& section : layout_->sections) {
        if (section.name != "Custom") continue;
        for (const auto& n : section.nodes) {
            const core::DesignRect r = UiKit::Local(n, window);
            if (n.name.starts_with("GDR_STATIC")) {
                kit_->NodeText(n, data.Text(n.text), r);
            } else if (n.name == "GDR_EDIT_NAME") {
                kit_->Edit("##name", n, r, name_, 12, false, false);
            } else if (n.name == "GDR_BTN_CHECK") {
                // Always live, as the original's; a name of the wrong length is refused here.
                if (kit_->Button("##check", n, r, !busy_)) {
                    if (tables_->Dressable() && tables_->rules.names.Check(name_) == game::NameCheck::BadLength) {
                        message_ = "UIO_MSG_ERROR_CHARACTER_NAME_STRING";
                    } else {
                        shared_->session->CheckName(name_);
                        busy_ = true;
                    }
                }
            } else if (n.name == "GDR_BTN_MALE" || n.name == "GDR_BTN_FEMALE") {
                // The sex buttons show "_on" for the chosen one.
                const bool man = n.name == "GDR_BTN_MALE";
                assets::UiLayoutNode face = n;
                face.ddj = std::string("interface\\outer\\") + (man ? "man" : "woman") + ((man == male_) ? "_on.ddj" : "_off.ddj");
                if (kit_->Button(man ? "##male" : "##female", face, r, !busy_) && male_ != man) {
                    male_ = man;
                    lookDirty_ = true;
                    view_ = {ViewKey(0.0f, zoomed_, male_, height_)};
                    viewTime_ = 0.0f;
                }
            } else {
                for (int i = 0; i < kSliders; ++i) {
                    if (n.name != sliderNames[i]) continue;
                    const auto [min, max] = Range(sliders[i]);
                    const int before = value_[i];
                    const int after = DrawSlider(sliders[i], n, window.x, window.y, before, min, max, true);
                    if (after != before) {
                        value_[i] = after;
                        topic_ = i;
                        // A new European weapon starts its armour at none.
                        if (i == kWeapon && europe_) value_[kArmour] = 0;
                        lookDirty_ = true;
                    }
                }
            }
        }
    }
}

void CreateScreen::DrawExplain(float x, float y) {
    const assets::UiLayoutNode* frame = layout_->Find("GDR_STA_EXPLAIN");
    if (!frame) return;
    const core::DesignRect window{x, y, frame->rect.w, frame->rect.h};
    kit_->Image(frame->ddj, window);
    for (const auto& section : layout_->sections) {
        if (section.name != "Explain") continue;
        for (const auto& n : section.nodes) {
            const core::DesignRect r = UiKit::Local(n, window);
            const assets::UiLayoutColor c = n.fontColor;
            if (n.name == "GDR_STA_EXPLAINNAME") kit_->Text(shared_->data.Text(ExplainName()), r, ui::Rgba(c.r, c.g, c.b), n.fontIndex, 1);
            if (n.name == "GDR_TEXT_EXPLAIN") kit_->TextBox(shared_->data.Text(ExplainText()), r, ui::Rgba(c.r, c.g, c.b), n.fontIndex);
        }
    }
}

void CreateScreen::DrawUnder(float width, float height) {
    if (!layout_) return;
    const float px = height / kLayoutHeight;
    const auto* up = layout_->Find("GDR_STA_SCREENUP");
    const auto* down = layout_->Find("GDR_STA_SCREENDOWN");
    const float top = std::round((up ? up->rect.y + up->rect.h : 172.0f) * px);
    const float bottom = std::round((down ? down->rect.y : 1030.0f) * px);
    if (!shown_) {
        kit_->FillScreen({0, 0, width, height}, ui::Rgba(0, 0, 0)); // the town and the character are not in yet
    } else {
        kit_->FillScreen({0, 0, width, top}, ui::Rgba(0, 0, 0));
        kit_->FillScreen({0, bottom, width, height - bottom}, ui::Rgba(0, 0, 0));
    }
    // The bars, then the back image and the caption on them.
    if (up) kit_->ImageScreen(up->ddj, {0, 0, width, top});
    if (down) kit_->ImageScreen(down->ddj, {0, bottom, width, height - bottom});
    if (const auto* back = layout_->Find("GDR_STA_BACKIMAGE"); back && !back->ddj.empty()) {
        kit_->ImageScreen(back->ddj, {back->rect.x * px, back->rect.y * px, back->rect.w * px, back->rect.h * px});
    }
    // The caption at its layout rect as interface pixels, not on the 1600 x 1200 grid (inferred from
    // screenshots of the original at 1024 x 768 and 1920 x 1200: 428 wide in both).
    if (const auto* caption = layout_->Find("GDR_STA_TITLE")) kit_->Image(caption->ddj, {caption->rect.x, caption->rect.y, caption->rect.w, caption->rect.h});
}

void CreateScreen::Draw(ScreenContext& context, float height) {
    if (!layout_) return;
    const UiData& data = shared_->data;
    const float px = height / kLayoutHeight;
    const auto* up = layout_->Find("GDR_STA_SCREENUP");
    const auto* down = layout_->Find("GDR_STA_SCREENDOWN");
    const float top = std::round((up ? up->rect.y + up->rect.h : 172.0f) * px);
    const float bottom = std::round((down ? down->rect.y : 1030.0f) * px);

    // The windows stand on the window's edges as the setup places them.
    const core::DesignGrid& grid = kit_->grid();
    const float W = grid.width(), H = grid.height(), T = top / grid.scale(), B = bottom / grid.scale();
    DrawCustom(75, std::round(T + (B - T) / 2) - 155);
    const float rx = W - 165, ry = B - 69;
    DrawExplain(europe_ ? rx - 69 : rx - 59, europe_ ? ry - 270 : ry - 219);
    if (const auto* frame = layout_->Find("GDR_STA_ROTATE")) {
        const core::DesignRect window{rx, ry, frame->rect.w, frame->rect.h};
        kit_->Image(frame->ddj, window);
        for (const auto& section : layout_->sections) {
            if (section.name != "Rotate") continue;
            for (const auto& n : section.nodes) {
                const core::DesignRect r = UiKit::Local(n, window);
                if (n.name == "GDR_BTN_LROTATE" || n.name == "GDR_BTN_RROTATE") {
                    // Held, the character turns (inferred: 180 degrees a second); let go, it slows down.
                    const bool left = n.name == "GDR_BTN_LROTATE";
                    if (kit_->ButtonState(left ? "##left" : "##right", n, r).held) spin_ = left ? -180.0f : 180.0f;
                } else if (n.name == "GDR_BTN_ZOOM") {
                    assets::UiLayoutNode face = n;
                    face.ddj = zoomed_ ? "interface\\outer\\zoomout.ddj" : "interface\\outer\\zoomin.ddj";
                    if (kit_->Button("##zoom", face, r)) {
                        assets::CameraKey from = assets::SampleCameraTrack(view_, viewTime_);
                        from.time = 0.0f;
                        zoomed_ = !zoomed_;
                        view_ = {from, ViewKey(1.0f, zoomed_, male_, height_)};
                        viewTime_ = 0.0f;
                    }
                }
            }
        }
    }

    // Confirm opens the warning; Cancel goes back. The original's order: Europe's armour, the weapon, then the name;
    // a refusal sounds snd_error.
    const float by = B + 27;
    if (const auto* ok = layout_->Find("GDR_BTN_OK")) {
        if (kit_->Button("##ok", *ok, {W - 209, by, 92, 41}, !busy_ && !warning_)) {
            const game::NameCheck check = tables_->Dressable() ? tables_->rules.names.Check(name_) : game::NameCheck::BadLength;
            if (europe_ && value_[kArmour] == 0) {
                message_ = "UIO_MSG_ERROR_CHARACTER_SELECTARMOR";
            } else if (value_[kWeapon] == 0) {
                message_ = "UIO_MSG_ERROR_CHARACTER_SELECTWEAPON";
            } else if (check == game::NameCheck::BadLength) {
                message_ = "UIO_MSG_ERROR_CHARACTER_NAME_STRING";
            } else if (check != game::NameCheck::Ok) {
                message_ = "UIO_MSG_ERROR_CHARACTER_WRONGSTRING";
            } else {
                warning_ = true;
            }
            if (!warning_ && context.audio != nullptr) context.audio->PlayUi("SND_ERROR");
        }
    }
    if (const auto* back = layout_->Find("GDR_BTN_BACK")) {
        if (kit_->Button("##back", *back, {W - 105, by, 92, 41}, !busy_)) Leave();
    }

    if (warning_) {
        const auto* frame = layout_->Find("GDR_STA_WCREATE");
        const core::DesignRect window{std::round((W - 248) / 2), std::round((H - 128) / 2), 248, 128};
        if (frame) kit_->Image(frame->ddj, window);
        for (const auto& section : layout_->sections) {
            if (section.name != "WCreate") continue;
            for (const auto& n : section.nodes) {
                const core::DesignRect r = UiKit::Local(n, window);
                if (n.name == "GDR_STA_WNAME") kit_->NodeText(n, name_, r);
                if (n.name == "GDR_STATIC1") kit_->NodeText(n, data.Text(n.text), r);
                if (n.name == "GDR_BTN_WCANCEL" && kit_->Button("##wcancel", n, r, !busy_)) warning_ = false;
                if (n.name == "GDR_BTN_WCREATE" && (kit_->Button("##wcreate", n, r, !busy_) || (autoConfirm_ && !busy_))) {
                    autoConfirm_ = false;
                    SendCreate();
                }
            }
        }
    }
    if (!message_.empty()) {
        const bool blink = std::fmod(context.time, 1.0) < 0.85;
        kit_->RichText(data.Text(message_), {26, B + 29, W - 26 - 330, 100}, blink ? ui::Rgba(255, 220, 140) : ui::Rgba(255, 220, 140, 160), 0, 0);
    }
}

} // namespace sr::client
