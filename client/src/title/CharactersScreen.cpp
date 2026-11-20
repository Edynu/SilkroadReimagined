#include "title/CharactersScreen.h"

#include <algorithm>
#include <format>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "sr/assets/Dress.h"
#include "world/StageEffects.h"
#include "sr/core/Log.h"
#include "title/CameraKeys.h"
#include "sr/core/Trace.h"

namespace sr::client {

namespace proto = game::proto;
using Race = game::CreationRules::Race;

namespace {

} // namespace

void CharactersScreen::Enter(ScreenContext& context) {
    shared_ = TitleShared::From(context);
    context.handoff.erase(TitleShared::kHandoff);
    if (!shared_) {
        core::LogWarning("characters: no title session handed over; back to the title");
        next = "title";
        return;
    }
    textures_ = std::make_unique<UiTextures>(context.assets, context.uiWarmup);
    kit_ = std::make_unique<UiKit>(ui_, *textures_, shared_->data);
    shared_->LoadCharacters(context.jobs, context.files);
    tables_ = shared_->characters;
    shared_->session->ListCharacters();
    busy_ = true;
    track_ = DockKeys();
    trackTime_ = 0.0f;
    // No glow on the stage (inferred from a screenshot of the original's select screen: its stone
    // does not bloom).
    world_ = std::make_unique<WorldScreen>(WorldScreen::Backdrop{kSelectRegionX, kSelectRegionY, 4, false});
    world_->Enter(context);
    for (const StageProp& p : kStageProps) world_->AddProp(p.model, p.x, p.y, p.z, p.yaw, 1.0f);
    // The command line's create and select happen once a run (not again back from the world).
    if (!std::exchange(shared_->scripted, true)) {
        autoCreate_ = context.config.GetString("create", "");
        autoSelect_ = context.config.GetString("select", "");
    } else if (shared_->restarted && context.config.GetBool("reselect", false) && !std::exchange(shared_->reselected, true)) {
        // --reselect: back from --logout-after's Restart, the same character again (a re-entry's tries).
        autoSelect_ = context.config.GetString("select", "");
    }
    exitOnSelect_ = context.config.GetBool("exit-on-select", false);
    autoPedestal_ = context.config.GetBool("pedestal", false);
    const std::string pick = context.config.GetString("pick", "");
    autoPick_ = pick == "europe" ? 0 : pick == "china" ? 1 : -1;
    europe_ = context.config.GetString("race", "china") == "europe";
    male_ = context.config.GetString("sex", "man") != "woman";
}

void CharactersScreen::Exit(ScreenContext& context, FrameSnapshot& snapshot) {
    if (shared_) context.jobs.Wait(shared_->charactersJob); // it reads the file system
    StopItemGlows(); // their effects go with the world
    if (world_) world_->Exit(context, snapshot);
    world_.reset();
    if (const rhi::TextureHandle t = atlas_.Release()) snapshot.releaseTextures.push_back(t);
    textures_.reset();
    kit_.reset();
    shared_.reset();
}

std::string CharactersScreen::StatusText(proto::LobbyStatus status) const { return LobbyStatusText(shared_->data, status); }

void CharactersScreen::Create(const std::string& name, Race race, bool male) {
    if (!tables_->Dressable()) {
        message_ = "The creation rules are not loaded.";
        return;
    }
    const game::CreationRules& c = tables_->rules.creation;
    const auto figures = c.FiguresOf(race, male);
    const auto armour = c.ArmourOf(race, male);
    const auto weapons = c.WeaponsOf(race);
    if (figures.empty() || armour.empty() || weapons.empty()) {
        message_ = "Nothing to create with.";
        return;
    }
    proto::CharacterCreate m;
    m.name = name;
    m.model = figures.front();
    // The first armour and weapon the rules take together (a European armour's class limits its
    // weapons: the first of each need not go).
    bool found = false;
    for (std::size_t a = 0; a < armour.size() && !found; ++a) {
        for (const std::uint32_t weapon : weapons) {
            if (c.Check(m.model, armour[a].chest, armour[a].legs, armour[a].shoes, weapon) != game::CreationCheck::Ok) continue;
            m.chest = armour[a].chest;
            m.legs = armour[a].legs;
            m.shoes = armour[a].shoes;
            m.weapon = weapon;
            found = true;
            break;
        }
    }
    if (!found) {
        message_ = "Nothing to create with.";
        return;
    }
    shared_->session->Create(m);
    busy_ = true;
    message_ = "Creating " + name + "...";
}

void CharactersScreen::HandleEvents(ScreenContext& context) {
    for (game::TitleEvent& event : shared_->session->TakeEvents()) {
        if (auto* list = std::get_if<proto::CharacterList>(&event)) {
            busy_ = false;
            list_ = std::move(*list);
            stageDirty_ = true;
            const bool still = std::any_of(list_->characters.begin(), list_->characters.end(), [&](const auto& c) { return c.name == selected_; });
            if (!selected_.empty() && !still) Choose({});
        } else if (auto* result = std::get_if<proto::LobbyResult>(&event)) {
            busy_ = false;
            message_ = StatusText(result->status);
            if (result->action == proto::LobbyAction::Select) {
                if (result->status == proto::LobbyStatus::Ok) {
                    message_.clear();
                    core::LogInfo("characters: {} selected", selected_);
                }
                if (exitOnSelect_) {
                    context.quit = true;
                    context.exitCode = result->status == proto::LobbyStatus::Ok ? 0 : 1;
                } else if (result->status == proto::LobbyStatus::Ok) {
                    // Into the world (C6c): black, then the game screen's loading screen.
                    busy_ = true;
                    leaving_ = "game";
                    screenFade_.Out(0.5f);
                }
            } else if (result->action != proto::LobbyAction::NameCheck) {
                if (result->status == proto::LobbyStatus::Ok && result->action == proto::LobbyAction::Create) {
                    stage_ = Stage::Dock;
                    race_ = -1;
                    fade_ = 0.0f;
                    track_ = DockKeys();
                    trackTime_ = 0.0f;
                }
                shared_->session->ListCharacters(); // the list after a change
                busy_ = true;
            }
        } else if (auto* error = std::get_if<game::TitleError>(&event)) {
            core::LogWarning("characters: {}", error->reason);
            if (message_.empty() || !kicked_) message_ = shared_->data.Text("UIO_MSG_ERROR_SEVER_CONNECT");
            busy_ = false;
        } else if (auto* bye = std::get_if<proto::Disconnected>(&event)) {
            // The server closes the connection and says why: kept over the lost connection's words.
            message_ = TitleShared::FarewellOf(*bye);
            kicked_ = true;
            core::LogWarning("characters: {}", message_);
        }
    }
    // Automated runs, one step at a time while nothing is pending: create (once the rules are
    // read, unless the character exists), then select.
    if (!list_ || busy_) return;
    if (!autoCreate_.empty()) {
        if (!tables_->DressReady()) return;
        const std::string name = std::exchange(autoCreate_, {});
        const bool has = std::any_of(list_->characters.begin(), list_->characters.end(), [&](const auto& c) { return c.name == name; });
        if (!has) {
            Create(name, europe_ ? Race::Europe : Race::China, male_);
            return;
        }
    }
    const bool arrived = trackTime_ >= track_.back().time;
    if (autoPedestal_ && stage_ == Stage::Dock && arrived) {
        autoPedestal_ = false;
        GoToPedestal();
        return;
    }
    if (autoPick_ >= 0 && stage_ == Stage::Pedestal && arrived) {
        PickRace(std::exchange(autoPick_, -1));
        return;
    }
    if (!autoSelect_.empty()) {
        const std::string name = std::exchange(autoSelect_, {});
        Choose(name);
        shared_->session->Select(name);
        busy_ = true;
    }
}

void CharactersScreen::Update(ScreenContext& context, FrameSnapshot& snapshot) {
    if (!shared_) {
        context.holdCapture = true;
        return;
    }
    SR_ZONE("characters: update");
    shared_->session->Update();
    HandleEvents(context);
    if (!next.empty()) {
        if (next == "create" || next == "game") context.handoff[TitleShared::kHandoff] = shared_; // the session goes along
        context.holdCapture = true; // leaving: the next screen draws the frame worth capturing
        return;
    }
    UpdateDock(context, snapshot);
    screenFade_.Update(float(context.dt));
    if (!leaving_.empty() && screenFade_.Done()) {
        next = leaving_;
        if (next == "game") context.handoff[TitleShared::kHandoff] = shared_;
        context.holdCapture = true;
        return;
    }
    const float width = float(context.device.SwapchainWidth()), height = float(context.device.SwapchainHeight());
    // The login can be quicker than the interface data the title started loading.
    if (!shared_->data.Ready()) {
        context.holdCapture = true;
        return;
    }
    ui::Font* font = shared_->data.Font();
    if (font) atlas_.Sync(context.device, *font);
    ui_.font = font;
    ui_.getClipboard = context.getClipboard;
    ui_.setClipboard = context.setClipboard;
    ui_.NewFrame(*context.input, {width, height}, context.mouseBlocked, context.keyboardBlocked);
    kit_->grid().FitAnchored(width, height);
    ui_.BeginLayer("characters", ui::LayerKind::Window);
    ui_.draw().SetWhite(font ? font->texture() : ui::TextureRef{}, font ? font->whiteUv() : ui::Vec2{0.5f, 0.5f});
    Draw(context, width, height);
    ui_.EndLayer();
    ui_.BeginLayer("characters: fade", ui::LayerKind::Overlay);
    ui_.draw().SetWhite(font ? font->texture() : ui::TextureRef{}, font ? font->whiteUv() : ui::Vec2{0.5f, 0.5f});
    screenFade_.Draw(*kit_, width, height);
    ui_.EndLayer();
    ui_.EndFrame();
    if (font) atlas_.Sync(context.device, *font);
    for (const ui::DrawList* list : ui_.DrawOrder()) AppendUi(snapshot.ui, *list);
    context.textInput = ui_.typing();
    // A capture waits for the list, the characters on their slots and the camera's arrival.
    if (!list_ || busy_ || !textures_->Settled() || stageDirty_ || trackTime_ < track_.back().time || autoPedestal_ || autoPick_ >= 0 ||
        stage_ == Stage::Picked || !screenFade_.Done()) {
        context.holdCapture = true;
    }
    // Leaving for the create screen this frame: the session goes along (the switch comes before
    // another update).
    if (next == "create") context.handoff[TitleShared::kHandoff] = shared_;
}

void CharactersScreen::UpdateDock(ScreenContext& context, FrameSnapshot& snapshot) {
    if (!world_) return;
    // The glow tables came in after the characters were dressed: those wearing an item with a plus (the plus glow is
    // on their models) dressed again; the item effects alone need no new dress (StartItemGlows).
    if (placed_ != 0 && !dressedWithGlows_ && tables_->Ready()) {
        dressedWithGlows_ = true;
        if (list_) {
            for (const proto::CharacterSummary& c : list_->characters) {
                if (std::any_of(c.equipment.begin(), c.equipment.end(), [](const proto::EquippedItem& e) { return e.plus > 0; })) stageDirty_ = true;
            }
        }
    }
    PlaceCharacters(context);
    StartItemGlows(context);
    if (enteredAt_ < 0.0) enteredAt_ = context.time;
    if (!shown_ && DockReady()) {
        core::LogInfo("characters: shown after {:.2f} s", context.time - enteredAt_);
        shown_ = true; // the wharf and every character in: the flight starts, out of black
        screenFade_.In(0.5f);
    }
    if (shown_) trackTime_ = std::min(track_.back().time, trackTime_ + float(context.dt));
    const bool arrived = trackTime_ >= track_.back().time;
    if (stage_ == Stage::ToPedestal && arrived) stage_ = Stage::Pedestal;
    if (stage_ == Stage::ToDock && arrived) stage_ = Stage::Dock;
    if (stage_ == Stage::Picked) {
        // The fade to black (GDR_FADE, 1 s); then the race's create screen (state 9).
        fade_ = std::min(1.0f, fade_ + float(context.dt));
        if (fade_ >= 1.0f && arrived && next.empty()) {
            shared_->race = race_;
            next = "create";
        }
    }
    ApplyCameraKey(*world_, assets::SampleCameraTrack(track_, trackTime_), kSelectRegionX, kSelectRegionY);
    world_->Update(context, snapshot);
}

bool CharactersScreen::DockReady() const {
    if (!world_ || !world_->Settled() || !list_ || stageDirty_ || !tables_->DressReady()) return false;
    for (std::size_t i = 0; i < 4; ++i) {
        if ((placed_ >> i & 1u) != 0 && !world_->StageReady(i)) return false;
    }
    return true;
}

void CharactersScreen::PlaceCharacters(ScreenContext& context) {
    if (!stageDirty_ || !list_ || !tables_->Dressable()) return;
    stageDirty_ = false;
    placed_ = 0;
    StopItemGlows(); // dressed anew: the old effects go, the new ones come when the models show
    // The plus glow's table is one of the later ones: read only once they are all in (not while it loads).
    dressedWithGlows_ = tables_->Ready();
    const assets::EquipGlowTable* equipGlows = dressedWithGlows_ ? &tables_->equipGlows : nullptr;
    const std::vector<DockSlot> slots = DockSlots(int(list_->characters.size()));
    for (std::size_t i = 0; i < 4; ++i) {
        if (i >= list_->characters.size() || i >= slots.size()) {
            world_->ClearStage(i);
            continue;
        }
        // Dressed in what it wears, at its height and volume.
        const proto::CharacterSummary& c = list_->characters[i];
        std::vector<std::uint32_t> worn;
        std::vector<std::uint8_t> plus;
        for (const proto::EquippedItem& e : c.equipment) {
            worn.push_back(e.item);
            plus.push_back(e.plus);
        }
        assets::DressedCharacter dressed;
        std::string error;
        if (!assets::Dress(context.files, tables_->characters, tables_->items, c.model, worn, c.height, dressed, error, plus, equipGlows,
                           tables_->visuals.Size() > 0 ? &tables_->visuals : nullptr)) {
            core::LogWarning("characters: {}: {}", c.name, error);
            world_->ClearStage(i);
            continue;
        }
        WorldScreen::StageLook look;
        look.model = dressed.Key();
        look.stance = dressed.stance;
        look.height = dressed.height;
        look.volume = dressed.volume;
        // A character waiting for deletion sits (animation state 14). The
        // original also draws it at alpha 0xCC (not yet: our instances are opaque).
        look.state = c.deleting ? 14 : 0;
        world_->SetStage(i, look, slots[i].x, slots[i].y, slots[i].z, 0.0f);
        placed_ |= 1u << i;
    }
}

void CharactersScreen::StartItemGlows(ScreenContext& context) {
    if (!list_ || !dressedWithGlows_ || stageDirty_) return;
    std::vector<std::uint32_t> worn;
    std::vector<std::uint8_t> plus;
    for (std::size_t i = 0; i < glows_.size() && i < list_->characters.size(); ++i) {
        const unsigned bit = 1u << i;
        if ((placed_ & bit) == 0 || (glowing_ & bit) != 0 || !world_->StageReady(i)) continue;
        glowing_ |= bit;
        worn.clear();
        plus.clear();
        for (const proto::EquippedItem& e : list_->characters[i].equipment) {
            worn.push_back(e.item);
            plus.push_back(e.plus);
        }
        glows_[i] = StageEffects::ItemGlows(*world_, i, context.files, tables_->items, tables_->itemGlows, worn, plus);
    }
}

void CharactersScreen::StopItemGlows() {
    for (auto& list : glows_) {
        if (world_) {
            for (const gfx::EffectSystem::Handle h : list) world_->Effects().Stop(h);
        }
        list.clear();
    }
    glowing_ = 0;
}

void CharactersScreen::GoToPedestal() {
    // Create (with room for a character): the track to the pedestal (5 s).
    Choose({});
    track_ = CreateKeys();
    trackTime_ = 0.0f;
    stage_ = Stage::ToPedestal;
    hoveredIdol_ = -1;
}

void CharactersScreen::GoToDock(float seconds) {
    // From the current view back to the dock (2 s from the pedestal).
    assets::CameraKey from = assets::SampleCameraTrack(track_, trackTime_);
    from.time = 0.0f;
    assets::CameraKey to = DockKeys().back();
    to.time = seconds;
    track_ = {from, to};
    trackTime_ = 0.0f;
    stage_ = Stage::ToDock;
    race_ = -1;
    fade_ = 0.0f;
}

void CharactersScreen::PickRace(int race) {
    assets::CameraKey from = assets::SampleCameraTrack(track_, trackTime_);
    from.time = 0.0f;
    track_ = {from, PickKey(1.0f, race)};
    trackTime_ = 0.0f;
    stage_ = Stage::Picked;
    race_ = race;
    fade_ = 0.0f;
}

void CharactersScreen::Choose(const std::string& name) {
    if (name == selected_) return;
    // From wherever the camera is now: to the character in 2 s, back to the dock in 1 s.
    assets::CameraKey from = assets::SampleCameraTrack(track_, trackTime_);
    from.time = 0.0f;
    std::optional<assets::CameraKey> to;
    if (!name.empty() && list_) {
        const std::vector<DockSlot> slots = DockSlots(int(list_->characters.size()));
        for (std::size_t i = 0; i < list_->characters.size() && i < slots.size(); ++i) {
            if (list_->characters[i].name == name) to = ChosenKey(2.0f, slots[i], list_->characters[i].deleting);
        }
    }
    if (!to) {
        to = DockKeys().back();
        to->time = 1.0f;
    }
    selected_ = to->time == 2.0f ? name : std::string();
    message_.clear();
    if (!selected_.empty()) {
        for (const auto& c : list_->characters) {
            if (c.name == selected_ && c.deleting) message_ = "UIO_STT_CHAR_DEL_WAITING";
        }
    }
    track_ = {from, *to};
    trackTime_ = 0.0f;
}

namespace {

// The select layout's own grid (its bars: GDR_STA_SCREENUP 0..172, GDR_STA_SCREENDOWN from 1030),
// scaled to the window's height.
constexpr float kLayoutHeight = 1200.0f;

} // namespace

void CharactersScreen::DrawPedestal(float width, float height, float T) {
    // The race windows over their idols (at the pedestal): each centred on its idol,
    // kept 5 from the left edge; a click on an idol picks its race. Ours stand above
    // the idol's box and show only for the idol under the mouse (inferred: the original projects a
    // point it derives from the idol's matrix and changes the windows' look on hover; the two
    // 440-wide windows would overlap).
    const assets::UiLayout* select = shared_->data.Layout(kSelectLayout);
    if (!select) return;
    struct Idol {
        const char* window;
        const char* section;
        const char* text;
        std::size_t prop; // in kStageProps
    };
    static constexpr Idol kIdols[] = {{"GDR_STA_EUROPE", "Europe", "GDR_TEXT_EUROPE", 1}, {"GDR_STA_CHINA", "China", "GDR_TEXT_CHINA", 2}};
    const core::DesignGrid& grid = kit_->grid();
    int hovered = -1;
    for (int i = 0; i < 2; ++i) {
        const Idol& idol = kIdols[i];
        const auto* frame = select->Find(idol.window);
        assets::Bounds box;
        if (!frame || !world_->PropBounds(idol.prop, box)) continue;
        // The idol's box on the screen.
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        bool seen = true;
        for (int corner = 0; corner < 8; ++corner) {
            ui::Vec2 s;
            seen = seen && Project(width, height, corner & 1 ? box.max[0] : box.min[0], corner & 2 ? box.max[1] : box.min[1],
                                   corner & 4 ? box.max[2] : box.min[2], s);
            const core::DesignPoint d = grid.ToDesign({s.x, s.y});
            x0 = std::min(x0, d.x), y0 = std::min(y0, d.y), x1 = std::max(x1, d.x), y1 = std::max(y1, d.y);
        }
        if (!seen) continue;
        const float w = frame->rect.w, h = frame->rect.h;
        const core::DesignRect window{std::max(5.0f, std::round((x0 + x1) / 2 - w / 2)), std::max(T + 5, std::round(y0 - h - 8)), w, h};
        ui_.PushId(idol.window);
        const auto onIdol = kit_->Item("##idol", {x0, y0, x1 - x0, y1 - y0}, !busy_);
        // Shown while its idol (or the window itself) is under the mouse.
        const auto onWindow = hoveredIdol_ == i ? kit_->Item("##window", window, !busy_) : ui::Context::ItemState{};
        ui_.PopId();
        if (onIdol.clicked || onWindow.clicked) PickRace(i);
        if (!onIdol.hovered && !onWindow.hovered) continue;
        hovered = i;
        const float alpha = 1.0f;
        kit_->Image(frame->ddj, window, alpha);
        for (const auto& section : select->sections) {
            if (section.name != idol.section) continue;
            for (const auto& n : section.nodes) {
                const core::DesignRect r = UiKit::Local(n, window);
                if (n.name == idol.text) {
                    const assets::UiLayoutColor c = n.fontColor;
                    kit_->TextBox(shared_->data.Text(n.text), r, ui::WithAlpha(ui::Rgba(c.r, c.g, c.b), alpha), n.fontIndex);
                } else if (!n.text.empty()) {
                    kit_->NodeText(n, shared_->data.Text(n.text), r, alpha);
                }
            }
        }
    }
    hoveredIdol_ = hovered;
}

bool CharactersScreen::Project(float width, float height, float x, float y, float z, ui::Vec2& out) const {
    const OrbitCamera& c = world_->Camera();
    const glm::vec3 eye(c.targetX, c.targetY, c.targetZ);
    const glm::vec3 dir(std::cos(c.pitch) * std::sin(c.yaw), std::sin(c.pitch), std::cos(c.pitch) * std::cos(c.yaw));
    // As the scene renderer's fly camera (left-handed, depth 0..1, y down).
    const glm::mat4 view = glm::lookAt(eye, eye + dir, glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(c.fovY, width / height, c.nearZ > 0 ? c.nearZ : 1.0f, c.farZ > 0 ? c.farZ : 6000.0f);
    proj[1][1] *= -1.0f;
    const glm::vec4 clip = proj * view * glm::vec4(x, y, z, 1.0f);
    if (clip.w <= 0.0f) return false;
    out = {(clip.x / clip.w * 0.5f + 0.5f) * width, (clip.y / clip.w * 0.5f + 0.5f) * height};
    return true;
}

void CharactersScreen::Draw(ScreenContext& context, float width, float height) {
    const UiData& data = shared_->data;
    const assets::UiLayout* select = data.Layout(kSelectLayout);
    const auto node = [&](const char* name) { return select ? select->Find(name) : nullptr; };
    const float px = height / kLayoutHeight; // layout units -> pixels
    const auto* up = node("GDR_STA_SCREENUP");
    const auto* down = node("GDR_STA_SCREENDOWN");
    const float top = std::round((up ? up->rect.y + up->rect.h : 172.0f) * px);
    const float bottom = std::round((down ? down->rect.y : 1030.0f) * px);
    if (!shown_) {
        kit_->FillScreen({0, 0, width, height}, ui::Rgba(0, 0, 0)); // the wharf is not in yet
    } else {
        kit_->FillScreen({0, 0, width, top}, ui::Rgba(0, 0, 0));
        kit_->FillScreen({0, bottom, width, height - bottom}, ui::Rgba(0, 0, 0));
    }
    if (up) kit_->ImageScreen(up->ddj, {0, 0, width, top});
    if (down) kit_->ImageScreen(down->ddj, {0, bottom, width, height - bottom});
    float region = stage_ == Stage::Dock ? 0.0f : 1.0f;
    if (stage_ == Stage::ToPedestal) region = std::min(1.0f, trackTime_ / track_.back().time);
    if (stage_ == Stage::ToDock) region = 1.0f - std::min(1.0f, trackTime_ / track_.back().time);
    for (const auto& [name, alpha] : {std::pair{"GDR_STA_TITLE", 1.0f - region}, std::pair{"GDR_STA_REGIONTITLE", region}}) {
        const auto* caption = node(name);
        if (caption && alpha > 0.0f) {
            kit_->ImageScreen(caption->ddj, {caption->rect.x * px, caption->rect.y * px, caption->rect.w * px, caption->rect.h * px}, alpha);
        }
    }

    // The rest stands on the window's edges, as the original places it on the screen's (from the right edge, the bottom
    // bar's top B and the top bar's bottom T).
    const core::DesignGrid& grid = kit_->grid();
    const float W = grid.width(), T = top / grid.scale(), B = bottom / grid.scale();

    // The characters: a click on one (its body or its label) chooses it; the label ("%s Lev:%d")
    // shows over the one under the mouse (inferred: the original's shows none otherwise).
    const proto::CharacterSummary* chosen = nullptr;
    const bool still = !track_.empty() && trackTime_ >= track_.back().time;
    if (list_ && world_ && shown_ && stage_ == Stage::Dock) {
        const std::vector<DockSlot> slots = DockSlots(int(list_->characters.size()));
        for (std::size_t i = 0; i < list_->characters.size() && i < slots.size() && i < 4; ++i) {
            const proto::CharacterSummary& c = list_->characters[i];
            if (c.name == selected_) chosen = &c;
            if (!selected_.empty() || !still) continue; // chosen: the info window instead
            ui::Vec2 head, feet;
            if (!Project(width, height, slots[i].x, slots[i].y + 19.0f, slots[i].z, head) ||
                !Project(width, height, slots[i].x, slots[i].y, slots[i].z, feet)) {
                continue;
            }
            const std::string label = std::format("{} Lev:{}", c.name, int(c.level));
            const core::DesignPoint at = grid.ToDesign({head.x, head.y});
            const core::DesignPoint foot = grid.ToDesign({feet.x, feet.y});
            const float w = kit_->TextWidth(label, 0) + 12;
            const core::DesignRect labelRect{at.x - w / 2, at.y - 18, w, 16};
            const core::DesignRect body{at.x - 20, at.y, 40, std::max(10.0f, foot.y - at.y)};
            ui_.PushId(c.name);
            // Both take part every frame (each keeps its own press state).
            const auto bodyItem = kit_->Item("##body", body, !busy_);
            const auto labelItem = kit_->Item("##label", labelRect, !busy_);
            ui_.PopId();
            if (bodyItem.clicked || labelItem.clicked) Choose(c.name);
            if (bodyItem.hovered || labelItem.hovered) {
                kit_->Fill(labelRect, ui::Rgba(0, 0, 0, 120));
                kit_->Text(label, labelRect, c.deleting ? ui::Rgba(255, 110, 100) : ui::Rgba(255, 230, 120), 0, 1);
            }
        }
    }

    // The chosen character's info window (GDR_STA_CHARINFO at W - 271, T + 41).
    if (chosen && select) {
        const float alpha = std::min(1.0f, trackTime_ / 0.5f);
        const core::DesignRect window{W - 271, T + 41, 228, 140};
        if (const auto* frame = node("GDR_STA_CHARINFO")) kit_->Image(frame->ddj, window, alpha);
        for (const auto& section : select->sections) {
            if (section.name != "Info") continue; // the other sections' STATICs are elsewhere
            for (const auto& n : section.nodes) {
                const core::DesignRect r = UiKit::Local(n, window);
                if (n.name == "GDR_STATIC1" || n.name == "GDR_STATIC2" || n.name == "GDR_STATIC3") kit_->NodeText(n, data.Text(n.text), r, alpha);
                if (n.name == "GDR_GAU_HP" || n.name == "GDR_GAU_MP") kit_->Image(n.ddj, r, alpha); // full: the lobby has no damage
                if (n.name == "GDR_STA_NAME") kit_->NodeText(n, chosen->name, r, alpha);
                if (n.name == "GDR_STA_LEVEL") kit_->NodeText(n, std::to_string(int(chosen->level)), r, alpha);
                if (n.name == "GDR_STA_SP") kit_->NodeText(n, std::to_string(chosen->statPoints), r, alpha);
                if (n.name == "GDR_STA_EXP") kit_->NodeText(n, std::to_string(chosen->experience), r, alpha);
            }
        }
        if (chosen->deleting) {
            // The deletion's window (GDR_STA_REMAINTIME, centred, B - 102; section Remain): its title,
            // the time left (UIO_MSG_CHAR_DEL_TIME, rich text; ours wraps to a second line), and a
            // gauge of the 7 days gone (1 - minutes / 10080, 0x73A2xx).
            const core::DesignRect box{std::round((W - 328) / 2), B - 102, 328, 92};
            if (const auto* frame = node("GDR_STA_REMAINTIME")) kit_->Image(frame->ddj, box, alpha);
            const std::uint32_t m = chosen->deleteMinutes;
            for (const auto& section : select->sections) {
                if (section.name != "Remain") continue;
                for (const auto& n : section.nodes) {
                    const core::DesignRect r = UiKit::Local(n, box);
                    if (n.name == "GDR_STA_WREMINTIME1") kit_->NodeText(n, data.Text(n.text), r, alpha);
                    if (n.name == "GDR_PML_WREMINTIME2") {
                        char text[512];
                        std::snprintf(text, sizeof(text), data.Text("UIO_MSG_CHAR_DEL_TIME").c_str(), int(m / 1440), int(m / 60 % 24), int(m % 60));
                        kit_->RichText(text, {r.x + 8, r.y - 4, r.w - 16, 28}, ui::WithAlpha(ui::kWhite, alpha), n.fontIndex, 1);
                    }
                    if (n.name == "GDR_REMAING") {
                        const float gone = std::clamp(1.0f - float(m) / 10080.0f, 0.0f, 1.0f);
                        kit_->Image(n.ddj, {r.x, r.y, r.w * gone, r.h}, alpha);
                    }
                }
                for (const auto& n : section.nodes) {
                    if (n.name == "GDR_REMAINGBOX") kit_->Image(n.ddj, UiKit::Local(n, box), alpha);
                }
            }
        }
    }

    // The buttons, 104 apart from the right edge, 27 under the bottom bar's top:
    // browsing, Create and Cancel (back to the title); a character chosen, Start,
    // Delete or Restore, and Cancel (back to browsing).
    const float by = B + 27;
    if (select && (stage_ == Stage::ToPedestal || stage_ == Stage::Pedestal)) {
        // At the pedestal: Cancel (GDR_BTN_CANCEL) goes back to the dock.
        if (const auto* cancel = node("GDR_BTN_CANCEL")) {
            if (kit_->Button("##cancel", *cancel, {W - 105, by, 92, 41}, !busy_ && stage_ == Stage::Pedestal)) GoToDock(2.0f);
        }
    } else if (select && stage_ == Stage::Dock) {
        if (!chosen) {
            if (const auto* create = node("GDR_BTN_CREATE")) {
                if (kit_->Button("##create", *create, {W - 209, by, 92, 41}, !busy_ && list_ && still)) {
                    if (list_->characters.size() < list_->maxCharacters) {
                        GoToPedestal();
                    } else {
                        message_ = "UIO_MSG_ERROR_CHARACTER_OVER";
                    }
                }
            }
            if (const auto* back = node("GDR_BTN_BACK")) {
                if (kit_->Button("##back", *back, {W - 105, by, 92, 41}, !busy_ && leaving_.empty())) {
                    shared_->session->Disconnect();
                    leaving_ = "title";
                    screenFade_.Out(0.5f);
                }
            }
        } else {
            if (const auto* start = node("GDR_BTN_START")) {
                if (kit_->Button("##start", *start, {W - 313, by, 92, 41}, !busy_ && !chosen->deleting)) {
                    shared_->session->Select(selected_);
                    busy_ = true;
                }
            }
            const bool deleting = chosen->deleting;
            if (const auto* remove = node(deleting ? "GDR_BTN_RESTORE" : "GDR_BTN_DELETE")) {
                if (kit_->Button("##delete", *remove, {W - 209, by, 92, 41}, !busy_)) {
                    if (deleting) {
                        shared_->session->Restore(selected_);
                    } else {
                        shared_->session->Delete(selected_);
                    }
                    busy_ = true;
                }
            }
            if (const auto* back = node("GDR_BTN_BACK2")) {
                if (kit_->Button("##back2", *back, {W - 105, by, 92, 41}, !busy_)) Choose({});
            }
        }
    }

    if (stage_ == Stage::Pedestal) DrawPedestal(width, height, T);
    // A race picked: the fade to black over everything (GDR_FADE), then the race's create screen.
    if (stage_ == Stage::Picked) {
        kit_->FillScreen({0, 0, width, height}, ui::Rgba(0, 0, 0, std::uint8_t(255.0f * fade_)));
    }
    // Messages in the bottom bar's text box (GDR_TEXT_MESSAGE at 26, B + 29).
    if (!message_.empty()) {
        const bool blink = std::fmod(context.time, 1.0) < 0.85;
        kit_->RichText(data.Text(message_), {26, B + 29, W - 26 - 330, 100}, blink ? ui::Rgba(255, 220, 140) : ui::Rgba(255, 220, 140, 160), 0, 0);
    }
}

} // namespace sr::client
