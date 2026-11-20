// The player's COS on the HUD: 1.188's CIFCOSManager (ginterface.txt GDR_COS_MANAGER, id 39, in
// the Create section; no rect of its own) shows a status icon for each COS out and the command bar of the picked one.
//
// - A status icon (CIFCOSStatus, resinfo\ifcosstatus.txt: GDR_COSSTAT_SLOT_WND am_window 44 x 56, the COS's
//   characterdata icon in GDR_COSSTAT_ICON at (6, 7, 32 x 32), the HP gauge GDR_COSSTAT_HP am_hp at (4, 47); an attack
//   pet's frame, am_cos_window 44 x 68, adds the HGP gauge GDR_COSSTAT_SGAUGE at (4, 56)) hangs from the top right: the
//   first at the screen's width - 172, each further one 48 more to the left, 2 down. With more than one
//   out, the picked one is outlined (GDR_COSSTAT_SELECT, icon\etc\cos_outline_2 / _1 at (-11, -11)); a
//   click picks one; its tooltip says its name and HP.
// - The command bar (CIFCOSCommand; the constants below): a cell for each command with its icon (icon\action\cos_cmd_*,
//   a _disable look when it cannot be given); nothing else (1.188 hides ifcoscommand.txt's board and open / close
//   button). The sets by kind are Build's; the keys Insert (Info), Home (Ride), PgUp (Unsummon / Terminate),
//   Del (Follow), PgDn (the stance): the Pet hotkeys.
// The screen gives the COS (HudContext::coses) and does the commands (HudContext::cosCommand).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <initializer_list>
#include <memory>
#include <string>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/game/Jobs.h"
#include "sr/assets/GameData.h"
#include "sr/core/Log.h"
#include "sr/game/CharacterRules.h"
#include "sr/game/Cos.h"
#include "sr/game/CosProtocol.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr const char* kAnimal = "interface\\animal\\";
constexpr const char* kStatusLayout = "resinfo\\ifcosstatus.txt";
constexpr float kStatusRight = 172.0f, kStatusStep = 48.0f, kStatusTop = 2.0f;
// The command bar (CIFCOSCommand, makes a command's cell, lays them out, picks the
// commands by the COS's kind): a cell's piece and its icon (28 x 28) by its place in the row, and how far the next starts:
//   first: am_ctrl_window_end 36 x 36, icon at (4, 5), next 33 on;
//   middle: am_ctrl_window_middle 32 x 36, icon at (2, 5), next 31 on;
//   last: am_ctrl_window_middle drawn 34 wide, icon at (2, 5), next 34 on.
// The row starts at the menu bar's place (window 175, CNIFUnderMenuBar) + 653 - 31 per cell across and 28 above it.
// first hides the layout's board (am_ctrl_tab, id 2) and open / close button (id 3), then places them after the
// last cell all the same: the 1.188 bar shows its cells alone (the user's screenshot of the 1.188 client agrees).
struct CellArt {
    const char* piece;
    float width, iconX, advance;
};
constexpr CellArt kFirstCell{"am_ctrl_window_end.ddj", 36.0f, 4.0f, 33.0f};
constexpr CellArt kMiddleCell{"am_ctrl_window_middle.ddj", 32.0f, 2.0f, 31.0f};
constexpr CellArt kLastCell{"am_ctrl_window_middle.ddj", 34.0f, 2.0f, 34.0f};
constexpr float kCellHeight = 36.0f, kIconY = 5.0f, kIcon = 28.0f;
constexpr float kRowFromMenuBar = 653.0f, kRowPerCell = 31.0f, kRowAboveMenuBar = 28.0f;

// A node's rect from a layout, else the given one (a layout not loaded).
ui::Rect NodeRect(const assets::UiLayout* layout, const char* name, ui::Rect fallback) {
    const assets::UiLayoutNode* n = layout != nullptr ? layout->Find(name) : nullptr;
    return n != nullptr ? ui::Rect{n->rect.x, n->rect.y, n->rect.w, n->rect.h} : fallback;
}

class StatusIcon : public ui::Widget {
public:
    StatusIcon(const assets::UiLayoutNode& node, HudContext& c, const assets::UiLayout* layout, std::uint32_t entity)
        : Widget(node), c_(c), entity_(entity) {
        icon_ = NodeRect(layout, "GDR_COSSTAT_ICON", {6, 7, 32, 32});
        hp_ = NodeRect(layout, "GDR_COSSTAT_HP", {4, 47, 0, 0});
        hgp_ = NodeRect(layout, "GDR_COSSTAT_SGAUGE", {4, 56, 0, 0});
        select_ = NodeRect(layout, "GDR_COSSTAT_SELECT", {-11, -11, 0, 0});
        interactive = true;
    }
    std::uint32_t entity() const { return entity_; }

    void Draw(ui::Frame& frame) override {
        const HudContext::CosShown* cos = Find();
        if (cos == nullptr) return;
        const ui::Rect r = ScreenRect();
        const bool growth = cos->kind == std::uint8_t(game::CosKind::AttackPet);
        if (entity_ == c_.cosSelected && c_.coses.size() > 1) {
            const std::string outline = growth ? "icon\\etc\\cos_outline_1.ddj" : "icon\\etc\\cos_outline_2.ddj";
            const ui::Vec2 size = frame.paint.TextureSize(outline);
            if (size.x > 0) frame.paint.Image(outline, {r.x + select_.x, r.y + select_.y, size.x, size.y});
        }
        frame.paint.Image(std::string(kAnimal) + (growth ? "am_cos_window.ddj" : "am_window.ddj"), r);
        const assets::ObjectRow* row = c_.characters != nullptr ? c_.characters->Find(cos->model) : nullptr;
        if (row != nullptr && !row->iconPath.empty()) frame.paint.Image("icon\\" + row->iconPath, {r.x + icon_.x, r.y + icon_.y, icon_.w, icon_.h});
        const auto gauge = [&](const char* art, ui::Rect at, float ratio) {
            const std::string texture = std::string(kAnimal) + art;
            const ui::Vec2 size = frame.paint.TextureSize(texture);
            ratio = std::clamp(ratio, 0.0f, 1.0f);
            if (size.x > 0 && ratio > 0) frame.paint.Image(texture, {r.x + at.x, r.y + at.y, size.x * ratio, size.y}, {0, 0}, {ratio, 1});
        };
        if (cos->maxHp > 0) gauge("am_hp.ddj", hp_, float(cos->hp) / float(cos->maxHp));
        if (growth) gauge("am_hgp.ddj", hgp_, float(cos->hgp) / float(game::kCosHgpFull));
    }

    void Mouse(ui::Frame& frame) override {
        const HudContext::CosShown* cos = Find();
        if (cos == nullptr) return;
        if (frame.pointer.hovered == this) {
            const assets::ObjectRow* row = c_.characters != nullptr ? c_.characters->Find(cos->model) : nullptr;
            std::string tip = !cos->name.empty() ? cos->name : row != nullptr && !row->nameKey.empty() ? c_.Text(row->nameKey) : c_.Text("UIIT_STT_COSNEWUI_TITLE");
            if (cos->level > 0 && cos->kind == std::uint8_t(game::CosKind::AttackPet)) tip += "  Lv " + std::to_string(cos->level);
            if (cos->maxHp > 0) tip += "\nHP " + std::to_string(cos->hp) + " / " + std::to_string(cos->maxHp);
            if (cos->kind == std::uint8_t(game::CosKind::AttackPet)) tip += "\nHGP " + std::to_string(cos->hgp / 100) + "%";
            frame.interface.ShowTooltip(tip, ScreenRect());
        }
        if (frame.input.Released(ui::MouseButton::Left) && frame.pointer.hovered == this) c_.cosSelected = entity_;
    }

private:
    const HudContext::CosShown* Find() const {
        for (const HudContext::CosShown& c : c_.coses) {
            if (c.entity == entity_) return &c;
        }
        return nullptr;
    }
    HudContext& c_;
    std::uint32_t entity_ = 0;
    ui::Rect icon_, hp_, hgp_, select_;
};

// A command's icon in its cell; a click (when it can be given) hands the command to the bar.
class CommandButton : public ui::Widget {
public:
    // Made in code (no Created call): it takes the mouse from the start.
    explicit CommandButton(const assets::UiLayoutNode& node) : Widget(node) { interactive = true; }
    int command = 0;
    std::string icon;       // icon\action\cos_cmd_*.ddj (its _disable look when it cannot be given)
    std::string tip;        // its name
    void Draw(ui::Frame& frame) override {
        std::string path = icon;
        if (!enabled) path.insert(path.size() - 4, "_disable");
        frame.paint.Image(path, ScreenRect());
    }
    void Mouse(ui::Frame& frame) override {
        if (frame.pointer.hovered == this && !tip.empty()) frame.interface.ShowTooltip(tip, ScreenRect());
        if (enabled && frame.input.Released(ui::MouseButton::Left) && frame.pointer.hovered == this) Notify(ui::Notice::Kind::Clicked, command);
    }
};

class CosManager : public ui::Widget {
public:
    CosManager(const assets::UiLayoutNode& node, HudContext& c) : Widget(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        status_ = ui.Layout(kStatusLayout);
    }

    void Update(ui::Frame& frame) override {
        Widget::Update(frame);
        if (c_.coses.empty()) {
            c_.cosSelected = 0;
        } else if (std::none_of(c_.coses.begin(), c_.coses.end(), [&](const HudContext::CosShown& s) { return s.entity == c_.cosSelected; })) {
            c_.cosSelected = c_.coses.front().entity;
        }
        // Rebuilt when what is out, or the picked one's commands, change.
        std::string key = std::to_string(c_.cosSelected);
        for (const HudContext::CosShown& s : c_.coses) key += "|" + std::to_string(s.entity) + ":" + std::to_string(s.kind) + (s.ridden ? "r" : "") + (s.aggressive ? "a" : "");
        // The menu bar's place (window 175; where the HUD puts it, when it is not made).
        const ui::Widget* menu = frame.interface.Window(hud::kMenuBar);
        const ui::Vec2 screen = frame.interface.screen();
        const ui::Rect bar = menu != nullptr ? menu->ScreenRect() : ui::Rect{std::floor((screen.x - 800.0f) / 2.0f), screen.y - 68.0f, 800.0f, 68.0f};
        key += "@" + std::to_string(int(bar.x)) + "," + std::to_string(int(bar.y)) + "," + std::to_string(int(screen.x));
        if (key == built_) return;
        // Not while one of its controls is held: the interface keeps a pointer to it until the button is up.
        if (frame.pointer.held != nullptr && frame.pointer.held->parent() == this) return;
        built_ = key;
        Build(frame, bar);
    }

    void Draw(ui::Frame& frame) override {
        if (c_.coses.empty()) return;
        // The cells' pieces under their icons.
        for (const auto& [rect, piece] : cells_) frame.paint.Image(std::string(kAnimal) + piece, rect);
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind != ui::Notice::Kind::Clicked) return false;
        core::LogInfo("hud: cos command {} on {}", n.value, c_.cosSelected);
        if (c_.cosCommand && c_.cosSelected != 0) c_.cosCommand(n.value, c_.cosSelected);
        return true;
    }

private:
    void Build(ui::Frame& frame, ui::Rect menuBar) {
        ClearChildren();
        cells_.clear();
        const ui::Vec2 screen = frame.interface.screen();
        // The status icons.
        for (std::size_t i = 0; i < c_.coses.size(); ++i) {
            const HudContext::CosShown& s = c_.coses[i];
            const bool growth = s.kind == std::uint8_t(game::CosKind::AttackPet);
            assets::UiLayoutNode n;
            n.type = "CIFCOSStatus";
            n.id = 1000 + int(i);
            n.rect = {screen.x - kStatusRight - kStatusStep * float(i), kStatusTop, 44.0f, growth ? 68.0f : 56.0f};
            auto icon = std::make_unique<StatusIcon>(n, c_, status_, s.entity);
            icon->rect = {n.rect.x, n.rect.y, n.rect.w, n.rect.h};
            Add(std::move(icon));
        }
        const HudContext::CosShown* picked = nullptr;
        for (const HudContext::CosShown& s : c_.coses) {
            if (s.entity == c_.cosSelected) picked = &s;
        }
        if (picked == nullptr) return;
        // The picked one's commands, the 1.188 client's sets by kind (their looks): a ride's and a
        // transport's Info, Ride, Terminate; a grab pet's Info, Ride (not its own), Unsummon, Follow; an attack pet's the
        // same, then Attack (the skill page) and the stance.
        struct Cmd {
            int command;
            std::string icon, tipKey;
            bool enabled;
        };
        const bool carrier = picked->kind == std::uint8_t(game::CosKind::Ride) || picked->kind == std::uint8_t(game::CosKind::Transport);
        const bool attackPet = picked->kind == std::uint8_t(game::CosKind::AttackPet);
        std::vector<Cmd> cmds;
        cmds.push_back({kCosInfo, "icon\\action\\cos_cmd_coswindow.ddj", "UIIT_STT_COSNEWUI_INFO", true}); // any COS out
        cmds.push_back({kCosRide, picked->ridden ? "icon\\action\\cos_cmd_disembark.ddj" : "icon\\action\\cos_cmd_embark.ddj",
                        picked->ridden ? "UIIT_STT_COS_DISEMBARK" : "UIIT_STT_COS_RIDE", carrier});
        if (carrier) {
            cmds.push_back({kCosTerminate, "icon\\action\\cos_cmd_ai_destruction.ddj", "UIIT_STT_COS_CLEAN", true});
        } else {
            cmds.push_back({kCosTerminate, "icon\\action\\cos_cmd_unsummon.ddj", "UIIT_STT_COSNEWUI_SUMMONCANCEL", true});
            cmds.push_back({kCosFollow, "icon\\action\\cos_cmd_follower.ddj", "UIIT_STT_COSNEWUI_FOLLOW", true});
            if (attackPet) {
                // Attack (the selected monster) and the stance, its icon by the stance it is in.
                cmds.push_back({kCosAttack, "icon\\action\\cos_cmd_skill_page.ddj", "UIIT_STT_COS_ATTACK", true});
                cmds.push_back({kCosStance, picked->aggressive ? "icon\\action\\cos_cmd_aggressive.ddj" : "icon\\action\\cos_cmd_defensive.ddj",
                                picked->aggressive ? "UIIT_STT_COS_AGGRESSIVE" : "UIIT_STT_COS_DEFENSIVE", true});
            }
        }
        // The row from the menu bar's place.
        const std::size_t shown = cmds.size();
        float x = menuBar.x + kRowFromMenuBar - kRowPerCell * float(shown);
        const float y0 = menuBar.y - kRowAboveMenuBar;
        for (std::size_t i = 0; i < shown; ++i) {
            const CellArt& art = shown == 1 || i == 0 ? kFirstCell : i + 1 == shown ? kLastCell : kMiddleCell;
            cells_.push_back({{x, y0, art.width, kCellHeight}, art.piece});
            assets::UiLayoutNode n;
            n.type = "CIFButton";
            n.id = 2000 + int(i);
            auto b = std::make_unique<CommandButton>(n);
            b->rect = {x + art.iconX, y0 + kIconY, kIcon, kIcon};
            x += art.advance;
            b->command = cmds[i].command;
            b->icon = cmds[i].icon;
            b->tip = c_.Text(cmds[i].tipKey);
            b->enabled = cmds[i].enabled;
            Add(std::move(b));
        }
    }

    HudContext& c_;
    const assets::UiLayout* status_ = nullptr;
    std::string built_;
    std::vector<std::pair<ui::Rect, const char*>> cells_; // the row's cells and their pieces
};

// CIFCOS (ginterface.txt's COSWnd section, GDR_COS_WND id 120, 355 x 390; resinfo\ifcos.txt): the picked COS's window,
// opened by Info (Insert). Its pages under three tabs (com_long_tab, 72 x 24 at (18 + 78 i, 44); ours, as the v1.150
// client's, the 1.188 places not traced): Basic Info (ifcos.txt's COSInfoWnd section, CIFCOSInfo laid out by
// resinfo\ifcosinfo.txt; ShowInfo below fills it as do), Inventory
// (COSInventoryWnd, CIFCOSInventory: the lattice 7 x 4 at (41, 59), 36 apart, the page spinner 17, the name 6) and
// Setting (COSSetupWnd, CIFCOSSetup: grabbing on / off 20, its scope 22, gold / equipment / the rest 25..27, Confirm 40,
// Cancel 41); the last two a grab pet's only. Items are dragged between the bag and the inventory, or within the bag.
class CosWindow : public ui::MainFrame {
public:
    CosWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    static constexpr int kTabBase = 900, kSlotBase = 300, kProxy = 399;
    enum Page { kInfo = 0, kBag = 1, kSetup = 2 };

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        ui_ = &ui;
        static constexpr const char* kTabs[] = {"UIIT_STT_COSNEWUI_TABMENU_BASICINFO", "UIIT_STT_COS_INVENTORY", "UIIT_STT_COSNEWUI_TABMENU_TECHNOLOGY"};
        for (int i = 0; i < 3; ++i) {
            assets::UiLayoutNode n;
            n.type = "CIFButton";
            n.id = kTabBase + i;
            n.rect = {18.0f + 78.0f * float(i), 44.0f, 72.0f, 24.0f};
            n.ddj = "interface\\ifcommon\\com_long_tab_off.ddj";
            n.hAlign = 1;
            tabs_[i] = Add(ui.Make(n));
            if (tabs_[i] != nullptr) tabs_[i]->SetText(c_.Text(kTabs[i]));
        }
        // The pages, from ifcos.txt's sections (each one node of its own class, laid out by its own file).
        if (const assets::UiLayout* layout = ui.Layout("resinfo\\ifcos.txt")) {
            static constexpr const char* kSections[] = {"COSInfoWnd", "COSInventoryWnd", "COSSetupWnd"};
            for (int k = 0; k < 3; ++k) {
                for (const auto& section : layout->sections) {
                    if (section.name != kSections[k] || section.nodes.empty()) continue;
                    pages_[k] = Add(ui.Make(section.nodes.front()));
                }
            }
        }
        if (ui::Widget* bag = pages_[kBag]) {
            for (int i = 0; i < proto::kCosBagPage; ++i) {
                assets::UiLayoutNode n;
                n.type = "CIFSlotWithHelp";
                n.id = kSlotBase + i;
                n.rect = {43.0f + 36.0f * float(i % 7), 61.0f + 36.0f * float(i / 7), 32.0f, 32.0f};
                auto* slot = ui::As<ui::Slot>(bag->Add(ui.Make(n)));
                if (slot == nullptr) continue;
                slot->pickable = false; // a click picks up the proxy (OnNotice)
                slot->tooltipLines = [this, i] {
                    const HudContext::CosShown* cos = Picked();
                    const game::ItemInstance* item = nullptr;
                    if (cos != nullptr) {
                        if (const auto it = cos->bag.find(std::uint8_t(page_ * proto::kCosBagPage + i)); it != cos->bag.end()) item = &it->second;
                    }
                    const assets::ItemRow* row = item != nullptr && c_.items != nullptr ? c_.items->Find(item->refId) : nullptr;
                    return row != nullptr ? ItemTooltip(*row, *item, c_) : std::vector<ui::TooltipLine>{};
                };
                slots_[i] = slot;
            }
        }
        if (ui::Widget* setup = pages_[kSetup]) {
            if (auto* onOff = ui::As<ui::RadioButton>(setup->Find(20))) {
                onOff->origin = {0.0f, 0.0f};
                onOff->spacing = 130.0f;
                onOff->SetUnits(ui, {c_.Text("UIIT_STT_COSNEWUI_PICKUP_SWITCH_ON"), c_.Text("UIIT_STT_COSNEWUI_PICKUP_SWITCH_OFF")}, true);
            }
            if (auto* scope = ui::As<ui::RadioButton>(setup->Find(22))) {
                scope->origin = {0.0f, 0.0f};
                scope->spacing = 130.0f;
                scope->SetUnits(ui, {c_.Text("UIIT_STT_COSNEWUI_PICKUPITEM_ASSORTMENT_SELF"), c_.Text("UIIT_STT_COSNEWUI_PICKUPITEM_ASSORTMENT_ALL")}, true);
            }
        }
        // What is held: a hidden slot carrying a copy of the item and its place (the storage window's way).
        assets::UiLayoutNode held;
        held.type = "CIFSlotWithHelp";
        held.id = kProxy;
        held.rect = {0.0f, 0.0f, 32.0f, 32.0f};
        proxy_ = ui::As<ui::Slot>(Add(ui.Make(held)));
        if (proxy_ != nullptr) proxy_->visible = false;
        // Dropped on a place of the bag: from the inventory's bag, or from another place of this bag; on a transport's,
        // a specialty good of the shop (the purchase's window).
        onDrop = [this](ui::Widget* source, ui::Widget* target) {
            const int to = CosBagPlaceOf(target);
            const HudContext::CosShown* cos = Picked();
            if (to < 0 || cos == nullptr || !c_.cosRequest) return false;
            if (cos->kind == std::uint8_t(game::CosKind::Transport)) {
                int tab = 0;
                if (const assets::ShopGood* good = StoreWindow::GoodOf(source, &tab)) {
                    if (c_.tradeNpc == 0 || !BuyIntoCart(c_, *good, tab)) {
                        if (c_.state != nullptr) c_.state->Post(game::GameMessage::Kind::General, c_.Text("UIIT_MSG_STRGERR_CANNOT_DEAL_AT_SHOP"), c_.time);
                    }
                    return true;
                }
                return true; // nothing else goes into a transport's bag (0x1843)
            }
            if (const int from = CosBagPlaceOf(source); from >= 0) {
                if (from != to) Move(std::uint8_t(from), std::uint8_t(to), true, true);
                return true;
            }
            if (const int place = InventoryPlaceOf(source); place >= game::slot::kWorn) {
                Move(std::uint8_t(place), std::uint8_t(to), false, true);
                return true;
            }
            return false;
        };
    }

    // A slot of this window: the bag place it stands for (the held proxy: the place it was picked from).
    int PlaceOf(const ui::Widget* slot) const {
        if (slot == nullptr) return -1;
        if (slot == proxy_) return proxyPlace_;
        if (slot->parent() != pages_[kBag] || slot->id() < kSlotBase || slot->id() >= kSlotBase + proto::kCosBagPage) return -1;
        return page_ * proto::kCosBagPage + slot->id() - kSlotBase;
    }

    void Update(ui::Frame& frame) override {
        ui::MainFrame::Update(frame);
        const HudContext::CosShown* cos = Picked();
        if (cos == nullptr) { // its COS gone: so is the window
            if (ui_ != nullptr) ui_->Close(this);
            return;
        }
        const bool transport = cos->kind == std::uint8_t(game::CosKind::Transport) && cos->bagSize > 0;
        const bool grab = cos->kind == std::uint8_t(game::CosKind::GrabPet) || transport; // a bag page
        if (transport) {
            ShowTrade(*cos);
        } else {
            ShowTradeRows(false);
        }
        if (c_.cosTab >= 0) { // a script's page
            tab_ = std::clamp(c_.cosTab, 0, 2);
            c_.cosTab = -1;
        }
        if (!grab && tab_ != kInfo) tab_ = kInfo;
        if (transport && tab_ == kSetup) tab_ = kBag;
        ShowInfo(*cos);
        for (int i = 0; i < 3; ++i) {
            if (pages_[i] != nullptr) pages_[i]->visible = i == tab_;
            if (auto* tab = ui::As<ui::Static>(tabs_[i])) {
                tab->visible = i == kInfo || (grab && !(transport && i == kSetup));
                tab->texture = i == tab_ ? "interface\\ifcommon\\com_long_tab_on.ddj" : "interface\\ifcommon\\com_long_tab_off.ddj";
            }
        }
        const assets::ObjectRow* row = c_.characters != nullptr ? c_.characters->Find(cos->model) : nullptr;
        const std::string name = !cos->name.empty() ? cos->name : row != nullptr && !row->nameKey.empty() ? c_.Text(row->nameKey) : c_.Text("UIIT_STT_COSNEWUI_TITLE");
        if (pages_[kBag] != nullptr) {
            if (ui::Widget* label = pages_[kBag]->Find(6)) label->SetText(name);
            if (auto* spin = ui::As<ui::Spin>(pages_[kBag]->Find(17))) {
                spin->max = std::max(0, (int(cos->bagSize) + proto::kCosBagPage - 1) / proto::kCosBagPage - 1);
                spin->value = std::clamp(spin->value, 0, spin->max);
                if (spin->value != page_) {
                    page_ = spin->value;
                    seen_ = ~0ull;
                }
            }
        }
        if (cos->bagVersion != seen_ || cos->entity != shownEntity_) {
            seen_ = cos->bagVersion;
            for (int i = 0; i < proto::kCosBagPage; ++i) {
                if (slots_[i] == nullptr) continue;
                const int place = page_ * proto::kCosBagPage + i;
                const auto it = cos->bag.find(std::uint8_t(place));
                ShowItem(*slots_[i], it != cos->bag.end() ? &it->second : nullptr, c_);
                // Beyond the bag's size: blocked, as the inventory's (the page's first
                // size - page x 28 places open, the rest pt_block.ddj and disabled).
                slots_[i]->enabled = place < cos->bagSize;
                if (place >= cos->bagSize) {
                    slots_[i]->icon = "interface\\pet\\pt_block.ddj";
                    slots_[i]->count = 0;
                }
            }
        }
        // The setup page shows the pet's settings (again when another pet is picked, or the server changed them).
        if (cos->entity != shownEntity_ || cos->option != shownOption_) {
            shownEntity_ = cos->entity;
            shownOption_ = cos->option;
            ShowOption(cos->option);
        }
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr && n.control >= kTabBase && n.control < kTabBase + 3 && n.source->parent() == this) {
            tab_ = n.control - kTabBase;
            return true;
        }
        const HudContext::CosShown* cos = Picked();
        // Sell All (35, at a specialty NPC with goods aboard): asked first, then every good sold (1.188's box
        // UIIT_MSG_SPECIALTY_ALLSELL_WINDOW; ours one request for them all).
        if (n.kind == ui::Notice::Kind::Clicked && n.control == 35 && n.source != nullptr && pages_[kBag] != nullptr && n.source->parent() == pages_[kBag]) {
            if (c_.hud != nullptr && c_.tradeNpc != 0) {
                HudContext* ctx = &c_;
                c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {c_.Text("UIIT_MSG_SPECIALTY_ALLSELL_WINDOW")}, [ctx](bool yes) {
                    if (yes && ctx->tradeRequest) ctx->tradeRequest(proto::TradeSellAll::kId, proto::Encode(proto::TradeSellAll{ctx->tradeNpc}));
                });
            }
            return true;
        }
        // Create name: the name asked ("Name your pet."), then "Confirm pet name?" with "You cannot
        // change the pet's name afterwards."; yes sends it (the original's 0x7117).
        if (n.kind == ui::Notice::Kind::Clicked && n.control == 26 && n.source != nullptr && pages_[kInfo] != nullptr && n.source->parent() == pages_[kInfo] &&
            cos != nullptr && cos->name.empty() && c_.cosRequest) {
            const std::uint32_t entity = cos->entity;
            AskGuildText(c_, c_.Text("UIIT_STT_COSPET_MAKENAME"), game::NameRules::kMaxLength, [this, entity](std::string name) {
                if (c_.hud == nullptr) return;
                c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {c_.Text("UIIT_MSG_COSPET_CHOOSE_NAME"), c_.Text("UIIT_MSG_COSPET_NOTREVISION_NAME")},
                                [this, entity, name](bool yes) {
                                    if (yes && c_.cosRequest) c_.cosRequest(proto::CosRename::kId, proto::Encode(proto::CosRename{entity, name}));
                                });
            });
            return true;
        }
        // A click on an item of the bag: the proxy picked up in its stead.
        if (n.kind == ui::Notice::Kind::Clicked && n.value == 0 && n.source != nullptr && pages_[kBag] != nullptr && n.source->parent() == pages_[kBag] &&
            n.control >= kSlotBase && n.control < kSlotBase + proto::kCosBagPage && proxy_ != nullptr && c_.hud != nullptr && c_.hud->interface() != nullptr) {
            const auto* slot = ui::As<ui::Slot>(n.source);
            if (slot != nullptr && !slot->icon.empty()) {
                proxy_->icon = slot->icon;
                proxy_->count = slot->count;
                proxy_->countAlways = slot->countAlways;
                proxy_->rare = slot->rare;
                proxyPlace_ = page_ * proto::kCosBagPage + n.control - kSlotBase;
                c_.hud->interface()->Pick(proxy_);
            }
            return true;
        }
        // The setup page's Confirm: the settings to the server; Cancel: back to the kept ones.
        if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr && pages_[kSetup] != nullptr && n.source->parent() == pages_[kSetup] && cos != nullptr) {
            if (n.control == 40 && c_.cosRequest) {
                c_.cosRequest(proto::CosSetOption::kId, proto::Encode(proto::CosSetOption{cos->entity, OptionShown()}));
                return true;
            }
            if (n.control == 41) {
                ShowOption(cos->option);
                return true;
            }
        }
        return ui::MainFrame::OnNotice(n);
    }

private:
    // A transport's trade (ifcosinventory.txt's TradeInfo section): the goods' total at their Price (23),
    // the stars (26..30, tinted by their count), Sell All (35) at a specialty NPC with goods aboard.
    void ShowTrade(const HudContext::CosShown& cos) {
        ui::Widget* bag = pages_[kBag];
        if (bag == nullptr || ui_ == nullptr) return;
        if (tradeRows_.empty()) {
            if (const assets::UiLayout* l = ui_->Layout("resinfo\\ifcosinventory.txt")) {
                for (const auto& s : l->sections) {
                    if (s.name != "TradeInfo") continue;
                    for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) {
                        if (ui::Widget* w = bag->Add(ui_->Make(*it))) tradeRows_.push_back(w);
                    }
                }
            }
        }
        ShowTradeRows(true);
        std::uint64_t total = 0;
        for (const auto& [place, item] : cos.bag) total += std::uint64_t(item.count) * game::kGoodsPrice;
        if (ui::Widget* w = bag->Find(23)) w->SetText(GoldText(total));
        static constexpr std::uint32_t kTint[] = {0xFFFFFFFF, 0xFF3C9BE8, 0xFF34C97C, 0xFFFFFFFF, 0xFFFF7E00, 0xFFFA4848};
        const int stars = std::clamp(int(c_.tradeStars), 0, 5);
        for (int i = 0; i < 5; ++i) {
            if (auto* star = dynamic_cast<ui::Static*>(bag->Find(26 + i))) {
                star->visible = i < stars;
                star->tint = ui::Argb(kTint[stars]);
            }
        }
        if (ui::Widget* sell = bag->Find(35)) sell->enabled = c_.tradeNpc != 0 && !cos.bag.empty();
    }

    // The trade rows (the TradeInfo section: the total, the trade difficulty's stars, Sell All) are a transport's only;
    // the window is one for every COS, so another's bag hides them (the user's report 2026-11-14: a grab pet's bag
    // showed them after a transport's). The stars keep their own visibility (ShowTrade) while shown.
    void ShowTradeRows(bool on) {
        for (ui::Widget* w : tradeRows_) {
            const int id = w->id();
            if (!on || id < 26 || id > 30) w->visible = on;
        }
    }

    const HudContext::CosShown* Picked() const {
        for (const HudContext::CosShown& s : c_.coses) {
            if (s.entity == c_.cosSelected) return &s;
        }
        return nullptr;
    }
    // The Basic Info page (ifcosinfo.txt's ids) for the COS's kind:
    // - HP (30 frame, 35 gauge, 40 label, 41 "hp/max (n%)"): every COS but a grab pet;
    // - Rental period (50, 51, 52 "d day h hour m minute"): a grab pet's;
    // - HGP (31, 36, 42, 43 "n% (hgp)") and EXP (32, 37, 44, 45 "exp/need (n.nn%)"): an attack pet's;
    // - the numbers (labels 60..63, 71..73; values 65..68, 76..78): an attack pet's, else the "<Stat does not exist>"
    //   line (100) alone; a hungry pet's values grey;
    // - its name (27): the window's title when it has none; Create name (26) for an unnamed pet.
    void ShowInfo(const HudContext::CosShown& cos) {
        ui::Widget* page = pages_[kInfo];
        if (page == nullptr) return;
        const bool attack = cos.kind == std::uint8_t(game::CosKind::AttackPet);
        const bool grab = cos.kind == std::uint8_t(game::CosKind::GrabPet);
        const auto show = [page](std::initializer_list<int> ids, bool on) {
            for (const int id : ids) {
                if (ui::Widget* w = page->Find(id)) w->visible = on;
            }
        };
        show({30, 35, 40, 41}, !grab);
        show({50, 51, 52}, grab);
        show({31, 36, 42, 43, 32, 37, 44, 45}, attack);
        show({60, 61, 62, 63, 65, 66, 67, 68, 71, 72, 73, 76, 77, 78}, attack);
        show({100}, !attack);
        const auto text = [page](int id, const std::string& s, ui::Color color = ui::kWhite) {
            if (ui::Widget* w = page->Find(id)) {
                w->SetText(s);
                if (auto* label = ui::As<ui::Static>(w)) label->textColor = color;
            }
        };
        const auto gauge = [page](int id, double ratio) {
            if (auto* g = ui::As<ui::Gauge>(page->Find(id))) g->SetValue(float(std::clamp(ratio, 0.0, 1.0)), true);
        };
        text(27, !cos.name.empty() ? cos.name : c_.Text("UIIT_STT_COSNEWUI_TITLE"));
        if (ui::Widget* button = page->Find(26)) button->enabled = cos.name.empty() && (attack || grab) && c_.cosRequest;
        if (!grab) {
            const std::uint32_t hp = std::min(cos.hp, cos.maxHp);
            gauge(35, cos.maxHp > 0 ? double(hp) / double(cos.maxHp) : 0.0);
            text(41, std::format("{}/{} ({}%)", hp, cos.maxHp, cos.maxHp > 0 ? hp * 100 / cos.maxHp : 0));
        } else {
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            const std::int64_t left = cos.rentUntil > now ? cos.rentUntil - std::int64_t(now) : 0;
            text(52, std::format("{}{} {}{} {}{}", left / 86400, c_.Text("PARAM_DAY"), left % 86400 / 3600, c_.Text("PARAM_HOUR"), left % 3600 / 60,
                                 c_.Text("PARAM_MINUTE")));
        }
        if (!attack) return;
        gauge(36, double(cos.hgp) / double(game::kCosHgpFull));
        text(43, std::format("{}% ({})", cos.hgp / 100, cos.hgp));
        const std::uint64_t need = c_.state != nullptr ? c_.state->ExperienceToNext(cos.level) : 0;
        if (need > 0) {
            gauge(37, double(cos.experience) / double(need));
            text(45, std::format("{}/{} ({:.2f}%)", cos.experience, need, double(cos.experience) * 100.0 / double(need)));
        } else {
            gauge(37, 1.0);
            text(45, "100%");
        }
        // The numbers, from its characterdata row and its skills (game::PetStatsOf; the client's own).
        const assets::ObjectRow* row = c_.characters != nullptr ? c_.characters->Find(cos.model) : nullptr;
        if (row == nullptr) return;
        std::array<const assets::SkillRow*, 10> skills{};
        for (std::size_t i = 0; i < skills.size(); ++i) skills[i] = c_.skills != nullptr && row->skills[i] != 0 ? c_.skills->Find(row->skills[i]) : nullptr;
        const game::PetStats s = game::PetStatsOf(*row, skills, cos.hgp);
        const ui::Color value = s.hungry ? ui::Argb(0xFF999999) : ui::kWhite;
        const auto range = [](int lo, int hi) { return hi == 0 ? std::string("0") : std::format("{} ~ {}", lo, hi); };
        text(65, std::to_string(cos.level));
        text(66, std::to_string(s.hitRate), value);
        text(67, range(s.phyMin, s.phyMax), value);
        text(68, std::to_string(s.phyDefence), value);
        text(76, std::to_string(s.parry), value);
        text(77, range(s.magMin, s.magMax), value);
        text(78, std::to_string(s.magDefence), value);
    }
    void Move(std::uint8_t from, std::uint8_t to, bool fromPet, bool toPet) {
        const HudContext::CosShown* cos = Picked();
        if (cos != nullptr && c_.cosRequest) c_.cosRequest(proto::CosBagMove::kId, proto::Encode(proto::CosBagMove{cos->entity, from, to, fromPet, toPet}));
    }
    void ShowOption(std::uint32_t option) {
        ui::Widget* setup = pages_[kSetup];
        if (setup == nullptr) return;
        if (auto* onOff = ui::As<ui::RadioButton>(setup->Find(20))) onOff->selected = (option & proto::kGrabOn) != 0 ? 0 : 1;
        if (auto* scope = ui::As<ui::RadioButton>(setup->Find(22))) scope->selected = (option & proto::kGrabAnyone) != 0 ? 1 : 0;
        static constexpr std::pair<int, std::uint32_t> kBoxes[] = {{25, proto::kGrabGold}, {26, proto::kGrabEquipment}, {27, proto::kGrabOther}};
        for (const auto& [id, bit] : kBoxes) {
            if (auto* box = ui::As<ui::CheckBox>(setup->Find(id))) box->checked = (option & bit) != 0;
        }
    }
    std::uint32_t OptionShown() const {
        ui::Widget* setup = pages_[kSetup];
        std::uint32_t option = 0;
        if (setup == nullptr) return option;
        if (auto* onOff = ui::As<ui::RadioButton>(setup->Find(20)); onOff != nullptr && onOff->selected == 0) option |= proto::kGrabOn;
        if (auto* scope = ui::As<ui::RadioButton>(setup->Find(22)); scope != nullptr && scope->selected == 1) option |= proto::kGrabAnyone;
        static constexpr std::pair<int, std::uint32_t> kBoxes[] = {{25, proto::kGrabGold}, {26, proto::kGrabEquipment}, {27, proto::kGrabOther}};
        for (const auto& [id, bit] : kBoxes) {
            if (auto* box = ui::As<ui::CheckBox>(setup->Find(id)); box != nullptr && box->checked) option |= bit;
        }
        return option;
    }

    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::array<ui::Widget*, 3> tabs_{}, pages_{};
    std::vector<ui::Widget*> tradeRows_; // the TradeInfo section's, made at a transport's first show
    std::array<ui::Slot*, proto::kCosBagPage> slots_{};
    ui::Slot* proxy_ = nullptr;
    int proxyPlace_ = -1;
    int tab_ = kInfo, page_ = 0;
    std::uint64_t seen_ = ~0ull;
    std::uint32_t shownEntity_ = 0, shownOption_ = ~0u;
};

} // namespace

int CosBagPlaceOf(const ui::Widget* slot) {
    for (const ui::Widget* w = slot; w != nullptr; w = w->parent()) {
        if (const auto* window = dynamic_cast<const CosWindow*>(w)) return window->PlaceOf(slot);
    }
    return -1;
}

void RegisterCosWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CIFCOSManager", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<CosManager>(n, *c); });
    ui.Register("CIFCOS", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<CosWindow>(n, *c); });
}

} // namespace sr::client
