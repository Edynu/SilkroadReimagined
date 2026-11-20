// The auto potion window (CIFAutoPotion, ginterface.txt 135, the T key):
// the 1.188 client's layout (ifautopotion.txt: the description, the HP and MP rows of
// ifautopotionslot.txt, the abnormal state's row, the delay's row, Confirm / Cancel), each row with
// its check box, and the HP and MP rows with their percent box and slider (1..100).
// Ours: in place of a row's two combo boxes naming a quick slot (the bar's page and key), a slot an
// item of the inventory is dropped in, its icon and how many of it the bag holds, on an inventory
// cell, "Potion" (or "Pills") beside it; the slot empty when the bag holds none. An item that does not
// fit the row is refused (game::AutoPotionFits); the item picked up from the slot and let go outside
// the window leaves the row empty. Confirm hands the settings to the game (the server keeps them and
// uses the potions); Cancel and the close button keep nothing.
// Ours beyond the original (2026-11-13, the user's ask): three tabs under the description (shortened for
// them), Character, Vehicle and Pet; the Vehicle and Pet tabs show the HP row alone, for the vehicle's
// (a ride or a transport) and the attack pet's health, its slot taking the pets' HP recovery kit
// (game::AutoPotion::kVehicle, kAttackPet). The delay is the window's, shared by every row.

#include <algorithm>
#include <array>
#include <utility>
#include <string>

#include "gui/HudWindows.h"
#include "sr/game/Inventory.h"

namespace sr::client {

namespace {

constexpr const char* kLayout = "resinfo\\ifautopotion.txt";
// The two CIFAutoPotionSlot rows of the window, and in each (ifautopotionslot.txt): the check box,
// the name, the percent and its unit, the bar's and the key's labels and combo boxes, the slider,
// its three numbers under it.
constexpr int kHpRowId = 10, kMpRowId = 11;
constexpr int kRowCheck = 1, kRowName = 2, kRowValue = 6, kRowUnit = 7, kRowBeltLabel = 10, kRowBelt = 11, kRowKeyLabel = 12, kRowKey = 13,
              kRowSlider = 15, kRowMin = 17, kRowCenter = 18, kRowMax = 19;
// The window's own: the abnormal row (AbnormalSlot) and the delay (PotionDelaySlot), the buttons.
constexpr int kAbnormalCheck = 15, kAbnormalBeltLabel = 17, kAbnormalBelt = 18, kAbnormalKeyLabel = 19, kAbnormalKey = 20;
constexpr int kDelayCheck = 25, kDelaySpin = 27, kConfirm = 30, kCancel = 31;
// Ours, in the window: each row's cell (outline and lattice) and its slot, by row; the tabs.
constexpr int kOutlineId = 60, kCellId = 70, kSlotId = 80, kTabBase = 90;
// The description's box and its text (ifautopotion.txt 5, 7), shortened for the tabs; the abnormal row's name.
constexpr int kDescFrame = 5, kDescText = 7, kAbnormalName = 16;
constexpr float kTabsRoom = 30.0f;

class AutoPotionWindow : public ui::MainFrame {
public:
    AutoPotionWindow(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        ui_ = &ui;
        for (const char* section : {"AbnormalSlot", "PotionDelaySlot"}) {
            if (const assets::UiLayout* l = ui.Layout(kLayout)) {
                for (const auto& s : l->sections) {
                    if (s.name != section) continue;
                    for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) Add(ui.Make(*it));
                }
            }
        }
        // The HP and MP rows (names them, fills their numbers).
        for (const auto& [rowId, nameKey] : {std::pair{kHpRowId, "UIIT_STT_MACROPOTION_HP"}, std::pair{kMpRowId, "UIIT_STT_MACROPOTION_MP"}}) {
            ui::Widget* row = Child(rowId);
            if (row == nullptr) continue;
            if (ui::Widget* name = row->Child(kRowName)) name->SetText(context_.Text(nameKey));
            if (ui::Widget* unit = row->Child(kRowUnit)) unit->SetText("%");
            if (auto* slider = ui::As<ui::Slider>(row->Child(kRowSlider))) {
                slider->min = 1;
                slider->max = 100;
                slider->step = 1;
                // Its arrows and thumb as the row sets them: the quest window's arrows at the bar's ends
                // over its sockets, the common thumb 2 down (0 and 312, 20 wide).
                slider->placeForward = false;
                if (auto* back = ui::As<ui::Button>(slider->Child(1))) {
                    back->texture = "interface\\ifcommon\\com_qst_lefttarrow_button.ddj";
                    back->rect = {0, 0, 20, 20};
                }
                if (auto* forward = ui::As<ui::Button>(slider->Child(2))) {
                    forward->texture = "interface\\ifcommon\\com_qst_rightarrow_button.ddj";
                    forward->rect = {slider->rect.w - 22.0f, 0, 20, 20};
                }
                if (ui::Widget* thumb = slider->Child(3)) thumb->rect = {20, 2, 16, 16};
            }
            if (ui::Widget* w = row->Child(kRowMin)) w->SetText("1");
            if (ui::Widget* w = row->Child(kRowCenter)) w->SetText("50");
            if (ui::Widget* w = row->Child(kRowMax)) w->SetText("100");
            // Ours: the quick slot's choice gone, our slot's label in the key label's place.
            for (int id : {kRowBeltLabel, kRowBelt, kRowKey}) {
                if (ui::Widget* w = row->Child(id)) w->visible = false;
            }
            if (ui::Widget* label = row->Child(kRowKeyLabel)) label->SetText(context_.Text("UIIT_STT_SILKMALL_POTION"));
        }
        for (int id : {kAbnormalBeltLabel, kAbnormalBelt, kAbnormalKey}) {
            if (ui::Widget* w = Child(id)) w->visible = false;
        }
        if (ui::Widget* label = Child(kAbnormalKeyLabel)) label->SetText(context_.Text("UIIT_CTL_WK_ARTICLES_CURE"));
        // The tabs (ours): the description's box shortened, the three in the room it leaves above the HP row.
        if (ui::Widget* w = Child(kDescFrame)) w->rect.h -= kTabsRoom;
        if (ui::Widget* w = Child(kDescText)) w->rect.h -= kTabsRoom - 4.0f;
        {
            static constexpr const char* kTabs[] = {"UIIT_STT_CHARACTER", "UIIT_CTL_WK_COS_VEHICLE", "UIIT_STT_SILKMALL_PET"};
            const ui::Widget* desc = Child(kDescFrame);
            const float y = desc != nullptr ? desc->rect.y + desc->rect.h + 4.0f : 155.0f;
            for (int i = 0; i < 3; ++i) {
                assets::UiLayoutNode n;
                n.type = "CIFButton";
                n.id = kTabBase + i;
                n.rect = {26.0f + 80.0f * float(i), y, 76.0f, 24.0f};
                n.ddj = "interface\\ifcommon\\com_long_tab_off.ddj";
                n.hAlign = 1;
                tabs_[std::size_t(i)] = Add(ui.Make(n));
                if (tabs_[std::size_t(i)] != nullptr) tabs_[std::size_t(i)]->SetText(context_.Text(kTabs[i]));
            }
        }
        if (auto* spin = ui::As<ui::VerticalSpin>(Child(kDelaySpin))) {
            // 0.5 .. 9.5 s by 0.5, shown in seconds with one place.
            spin->min = game::kDelayMinMs;
            spin->max = game::kDelayMaxMs;
            spin->step = game::kDelayStepMs;
            spin->divider = 1000.0f;
            spin->decimals = 1;
        }
        // Each row's slot on an inventory cell (its outline, as the inventory's lattice has), right of
        // the label, within the window (window pixels).
        for (int r = 0; r < game::AutoPotion::kRows; ++r) {
            const ui::Vec2 at = CellAt(r);
            const auto node = [&](const char* type, int id, ui::Rect rect, const char* ddj) {
                assets::UiLayoutNode n;
                n.type = type;
                n.id = id;
                n.rect = {rect.x, rect.y, rect.w, rect.h};
                n.ddj = ddj;
                return n;
            };
            Add(ui.Make(node("CIFStretchWnd", kOutlineId + r, {at.x - 3, at.y - 3, 38, 38}, "interface\\ifcommon\\lattice_window\\com_lattice_outline_")));
            Add(ui.Make(node("CIFLattice", kCellId + r, {at.x, at.y, 36, 36}, "interface\\ifcommon\\lattice_window\\com_lattice_")));
            Add(ui.Make(node("CIFSlotWithHelp", kSlotId + r, {at.x + 2, at.y + 2, 32, 32}, "")));
        }
        ShowTab();
        onDrop = [this](ui::Widget* source, ui::Widget* target) {
            const int row = RowOfSlot(target);
            if (row >= 0) Take(row, InventoryPlaceOf(source));
            return true; // kept where it came from, whatever it was
        };
        onPickEnd = [this](ui::Widget* source, ui::Widget* taker) {
            const int row = RowOfSlot(source);
            if (row >= 0 && (taker == nullptr || taker->Root() != this)) draft_.rows[std::size_t(row)].item = 0;
        };
        Load();
    }

    void Update(ui::Frame&) override {
        const game::PlayerState* state = context_.state;
        if (state == nullptr) return;
        if (state->autoPotionVersion != seen_) Load(); // the server's word came while it is open
        for (int r = 0; r < game::AutoPotion::kRows; ++r) ShowSlot(r);
        for (int rowId : {kHpRowId, kMpRowId}) {
            ui::Widget* row = Child(rowId);
            const auto* slider = row != nullptr ? ui::As<ui::Slider>(row->Child(kRowSlider)) : nullptr;
            if (slider == nullptr) continue;
            if (ui::Widget* value = row->Child(kRowValue)) {
                const std::string text = std::to_string(slider->value);
                if (value->text() != text) value->SetText(text);
            }
        }
    }

    bool OnNotice(const ui::Notice& notice) override {
        const bool mine = notice.kind == ui::Notice::Kind::Clicked && notice.source != nullptr && notice.source->parent() == this;
        if (mine && notice.control >= kTabBase && notice.control < kTabBase + 3) {
            SetTab(notice.control - kTabBase);
            return true;
        }
        if (mine && notice.control == kConfirm) {
            Confirm();
            return true;
        }
        if (mine && notice.control == kCancel) {
            if (ui_ != nullptr) ui_->Close(this);
            return true;
        }
        return ui::MainFrame::OnNotice(notice);
    }

    // The settings as the window shows them.
    game::AutoPotion Settings() const {
        game::AutoPotion s = draft_;
        const std::pair<int, int> shown[2] = {{kHpRowId, HpRowShown()}, {kMpRowId, int(game::AutoPotion::kMp)}};
        for (const auto& [rowId, r] : shown) {
            const ui::Widget* row = Child(rowId);
            if (row == nullptr) continue;
            if (const auto* c = ui::As<ui::CheckBox>(row->Child(kRowCheck))) s.rows[std::size_t(r)].enabled = c->checked;
            if (const auto* slider = ui::As<ui::Slider>(row->Child(kRowSlider))) s.rows[std::size_t(r)].percent = std::uint8_t(slider->value);
        }
        if (const auto* c = ui::As<ui::CheckBox>(Child(kAbnormalCheck))) s.rows[game::AutoPotion::kAbnormal].enabled = c->checked;
        if (const auto* c = ui::As<ui::CheckBox>(Child(kDelayCheck))) s.delayEnabled = c->checked;
        if (const auto* spin = ui::As<ui::VerticalSpin>(Child(kDelaySpin))) s.delayMs = std::uint16_t(spin->value);
        s.Clamp();
        return s;
    }

    void Confirm() {
        const game::AutoPotion s = Settings();
        if (context_.autoPotionSave) {
            context_.autoPotionSave(s);
        } else if (context_.state != nullptr) {
            context_.state->autoPotion = s;
            ++context_.state->autoPotionVersion;
        }
        if (ui_ != nullptr) ui_->Close(this);
    }

    // An inventory place's item into a row, when it fits.
    bool Take(int row, int place) {
        const game::PlayerState* state = context_.state;
        const game::ItemInstance* item = state != nullptr && place >= game::slot::kWorn ? state->inventory.At(std::uint8_t(place)) : nullptr;
        const assets::ItemRow* ref = item != nullptr && context_.items != nullptr ? context_.items->Find(item->refId) : nullptr;
        if (ref == nullptr || !game::AutoPotionFits(row, *ref)) return false;
        draft_.rows[std::size_t(row)].item = ref->refObjId;
        return true;
    }

    ui::Slot* SlotOf(int row) { return ui::As<ui::Slot>(Child(kSlotId + row)); }

    void SetRow(int row, bool enabled, int percent) {
        if ((row == game::AutoPotion::kHp || row >= game::AutoPotion::kVehicle) && row != HpRowShown()) {
            // A row its tab does not show now: kept for it.
            draft_.rows[std::size_t(row)].enabled = enabled;
            draft_.rows[std::size_t(row)].percent = std::uint8_t(std::clamp(percent, 1, 100));
            return;
        }
        ui::Widget* r = row == game::AutoPotion::kAbnormal ? this : Child(row == game::AutoPotion::kMp ? kMpRowId : kHpRowId);
        if (r == nullptr) return;
        if (auto* c = ui::As<ui::CheckBox>(r->Child(row == game::AutoPotion::kAbnormal ? kAbnormalCheck : kRowCheck))) c->checked = enabled;
        if (row != game::AutoPotion::kAbnormal) {
            if (auto* slider = ui::As<ui::Slider>(r->Child(kRowSlider))) slider->SetValue(percent);
        }
    }
    void SetDelay(bool enabled, int ms) {
        if (auto* c = ui::As<ui::CheckBox>(Child(kDelayCheck))) c->checked = enabled;
        if (auto* spin = ui::As<ui::VerticalSpin>(Child(kDelaySpin))) spin->SetValue(ms);
    }

    // A tab (0 Character, 1 Vehicle, 2 Pet): the HP row's widgets kept for the row they showed, then
    // showing the tab's.
    void SetTab(int tab) {
        tab = std::clamp(tab, 0, 2);
        if (tab == tab_) return;
        KeepHpRow();
        tab_ = tab;
        ShowHpRow();
        ShowTab();
    }

private:
    // The row the HP row's widgets show: the character's HP, the vehicle's or the attack pet's.
    int HpRowShown() const { return tab_ == 1 ? game::AutoPotion::kVehicle : tab_ == 2 ? game::AutoPotion::kAttackPet : game::AutoPotion::kHp; }

    void KeepHpRow() {
        const ui::Widget* row = Child(kHpRowId);
        if (row == nullptr) return;
        game::AutoPotion::Entry& e = draft_.rows[std::size_t(HpRowShown())];
        if (const auto* c = ui::As<ui::CheckBox>(row->Child(kRowCheck))) e.enabled = c->checked;
        if (const auto* slider = ui::As<ui::Slider>(row->Child(kRowSlider))) e.percent = std::uint8_t(slider->value);
    }
    void ShowHpRow() {
        ui::Widget* row = Child(kHpRowId);
        if (row == nullptr) return;
        const game::AutoPotion::Entry& e = draft_.rows[std::size_t(HpRowShown())];
        if (auto* c = ui::As<ui::CheckBox>(row->Child(kRowCheck))) c->checked = e.enabled;
        if (auto* slider = ui::As<ui::Slider>(row->Child(kRowSlider))) slider->SetValue(e.percent);
    }
    // What the tab shows: on Character the original's rows; on the others the HP row alone, its slot the
    // tab's row's (the pets' kit), "Cure the transport" beside it on the Vehicle tab.
    void ShowTab() {
        const bool character = tab_ == 0;
        for (int id : {kMpRowId, kAbnormalCheck, kAbnormalName, kAbnormalKeyLabel}) {
            if (ui::Widget* w = Child(id)) w->visible = character;
        }
        for (int r = 0; r < game::AutoPotion::kRows; ++r) {
            const bool shown = r == HpRowShown() || (character && (r == game::AutoPotion::kMp || r == game::AutoPotion::kAbnormal));
            for (int id : {kOutlineId + r, kCellId + r, kSlotId + r}) {
                if (ui::Widget* w = Child(id)) w->visible = shown;
            }
        }
        if (ui::Widget* row = Child(kHpRowId)) {
            if (ui::Widget* label = row->Child(kRowKeyLabel)) label->SetText(context_.Text(tab_ == 1 ? "UIIT_STT_COS_HEAL" : "UIIT_STT_SILKMALL_POTION"));
        }
        for (int i = 0; i < 3; ++i) {
            if (auto* tab = ui::As<ui::Static>(tabs_[std::size_t(i)])) {
                tab->texture = i == tab_ ? "interface\\ifcommon\\com_long_tab_on.ddj" : "interface\\ifcommon\\com_long_tab_off.ddj";
            }
        }
    }

    // Window pixels: right of the rows' labels; the HP and MP rows' cells over their combo boxes'
    // line (the rows' tops less 6), the abnormal row's beside its label; ours on the HP row's.
    ui::Vec2 CellAt(int row) const {
        if (row >= game::AutoPotion::kVehicle) row = game::AutoPotion::kHp;
        const ui::Widget* r = row == game::AutoPotion::kAbnormal ? nullptr : Child(row == 0 ? kHpRowId : kMpRowId);
        if (r != nullptr) return {r->rect.x + 300.0f, r->rect.y - 6.0f};
        const ui::Widget* label = Child(kAbnormalKeyLabel);
        return {323.0f, label != nullptr ? label->rect.y - 12.0f : 367.0f};
    }

    int RowOfSlot(const ui::Widget* w) const {
        for (int r = 0; r < game::AutoPotion::kRows; ++r) {
            const ui::Widget* slot = Child(kSlotId + r);
            const ui::Widget* cell = Child(kCellId + r);
            if (w != nullptr && (w == slot || w == cell)) return r;
        }
        return -1;
    }

    void Load() {
        const game::PlayerState* state = context_.state;
        if (state == nullptr) return;
        seen_ = state->autoPotionVersion;
        draft_ = state->autoPotion;
        const std::pair<int, int> shown[2] = {{kHpRowId, HpRowShown()}, {kMpRowId, int(game::AutoPotion::kMp)}};
        for (const auto& [rowId, r] : shown) {
            ui::Widget* row = Child(rowId);
            if (row == nullptr) continue;
            if (auto* c = ui::As<ui::CheckBox>(row->Child(kRowCheck))) c->checked = draft_.rows[std::size_t(r)].enabled;
            if (auto* slider = ui::As<ui::Slider>(row->Child(kRowSlider))) slider->SetValue(draft_.rows[std::size_t(r)].percent);
        }
        if (auto* c = ui::As<ui::CheckBox>(Child(kAbnormalCheck))) c->checked = draft_.rows[game::AutoPotion::kAbnormal].enabled;
        if (auto* c = ui::As<ui::CheckBox>(Child(kDelayCheck))) c->checked = draft_.delayEnabled;
        if (auto* spin = ui::As<ui::VerticalSpin>(Child(kDelaySpin))) spin->SetValue(draft_.delayMs);
    }

    // A row's slot: its item's icon and how many the bag holds; empty when none (the row keeps it).
    void ShowSlot(int row) {
        ui::Slot* slot = SlotOf(row);
        if (slot == nullptr || context_.state == nullptr) return;
        const std::uint32_t ref = draft_.rows[std::size_t(row)].item;
        const assets::ItemRow* item = ref != 0 && context_.items != nullptr ? context_.items->Find(ref) : nullptr;
        int count = 0;
        for (const auto& [place, it] : context_.state->inventory.places) {
            if (place >= game::slot::kWorn && it.refId == ref) count += it.count;
        }
        if (item == nullptr || count == 0) {
            slot->icon.clear();
            slot->count = 0;
            slot->tooltipLines = nullptr;
            slot->tooltip = item != nullptr ? context_.Text(item->nameKey) + " (0)" : context_.Text("UIIT_STT_SRO_MACRO_AUTO_POTION_ON_FAIL");
            slot->cooling = slot->charge = 0.0f;
            return;
        }
        slot->icon = item->iconPath.empty() ? std::string("icon\\icon_default.ddj") : "icon\\" + item->iconPath;
        slot->count = count;
        slot->countAlways = true;
        slot->pickable = true;
        slot->tooltip.clear();
        game::ItemInstance shown;
        shown.refId = ref;
        shown.count = std::uint16_t(std::min(count, 0xFFFF));
        HudContext* c = &context_;
        const assets::ItemRow* rowPtr = item;
        slot->tooltipLines = [c, rowPtr, shown] { return ItemTooltip(*rowPtr, shown, *c); };
        const HudContext::Cooling cooling = context_.itemCooling ? context_.itemCooling(ref) : HudContext::Cooling{};
        slot->cooling = cooling.cooling;
        slot->charge = cooling.charge;
    }

    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    game::AutoPotion draft_;
    std::uint64_t seen_ = ~0ull;
    std::array<ui::Widget*, 3> tabs_{};
    int tab_ = 0;
};

} // namespace

std::unique_ptr<ui::Widget> MakeAutoPotionWindow(const assets::UiLayoutNode& node, HudContext& context) {
    return std::make_unique<AutoPotionWindow>(node, context);
}

ui::Slot* AutoPotionSlot(ui::Widget* window, int row) {
    auto* w = dynamic_cast<AutoPotionWindow*>(window);
    return w != nullptr ? w->SlotOf(row) : nullptr;
}

bool AutoPotionDrop(ui::Widget* window, int row, int place) {
    auto* w = dynamic_cast<AutoPotionWindow*>(window);
    return w != nullptr && w->Take(row, place);
}

void AutoPotionSetRow(ui::Widget* window, int row, bool enabled, int percent) {
    if (auto* w = dynamic_cast<AutoPotionWindow*>(window)) w->SetRow(row, enabled, percent);
}

void AutoPotionSetDelay(ui::Widget* window, bool enabled, int ms) {
    if (auto* w = dynamic_cast<AutoPotionWindow*>(window)) w->SetDelay(enabled, ms);
}

void AutoPotionTab(ui::Widget* window, int tab) {
    if (auto* w = dynamic_cast<AutoPotionWindow*>(window)) w->SetTab(tab);
}

void AutoPotionConfirm(ui::Widget* window) {
    if (auto* w = dynamic_cast<AutoPotionWindow*>(window)) w->Confirm();
}

} // namespace sr::client
