// The underbar, the quick slots and the extra quick slots: 1.150's CIFUnderBar and
// 1.188's CNIFUnderMenuBar, which 1.188 shows in its place (section 2.6).

#include <algorithm>
#include <cmath>
#include <format>

#include "gui/Hud.h"
#include "sr/core/Log.h"
#include "gui/HudWindows.h"

namespace sr::client {

namespace {

constexpr const char* kUnderBarLayout = "resinfo\\ifunderbar.txt";
constexpr const char* kExtraLayout = "resinfo\\ifextquickslot.txt";
constexpr const char* kMenuBarLayout = "res_ui\\nifundermenubar.2dt";

// The quick slot a widget shows, whichever bar it is on (-1: none).
int QuickSlotOf(const ui::Widget* w, int page) {
    if (w == nullptr) return -1;
    const ui::Widget* root = const_cast<ui::Widget*>(w)->Root();
    if (dynamic_cast<const UnderBar*>(root) != nullptr) return UnderBar::SlotIndex(w->id(), page);
    if (dynamic_cast<const MenuBar*>(root) != nullptr) return MenuBar::SlotIndex(w->id(), page);
    if (dynamic_cast<const ExtraQuickSlots*>(root) != nullptr && w->id() >= 100 && w->id() < 110) {
        return game::QuickSlots::kExtraFirst + (w->id() - 100);
    }
    return -1;
}

// A section of a layout added to a window (its first node on top, after what it has).
void AddSection(ui::Interface& ui, ui::Widget& window, const char* layout, const std::string& name) {
    const assets::UiLayout* l = ui.Layout(layout);
    if (l == nullptr) return;
    for (const auto& s : l->sections) {
        if (s.name != name) continue;
        for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) window.Add(ui.Make(*it));
    }
}

void ShowSlot(ui::Slot* slot, const HudContext& context, const game::QuickSlot& q) {
    if (slot == nullptr) return;
    slot->icon = q.kind == game::QuickSlot::Kind::Empty || !context.slotIcon ? std::string() : context.slotIcon(q);
    // A skill's shortcut: the skill's tooltip (the original's slot tooltip serves the Skills page and
    // the underbar's skills alike).
    const assets::SkillRow* skill = q.kind == game::QuickSlot::Kind::Skill && context.skills != nullptr ? context.skills->Find(q.value) : nullptr;
    if (skill != nullptr) {
        const HudContext* c = &context;
        slot->tooltipSigned = false;
        slot->tooltipLines = [c, skill] { return SkillTooltip(*skill, *c); };
    } else {
        slot->tooltipSigned = true;
        slot->tooltipLines = nullptr;
    }
    // An action's shortcut: what its next use does.
    if (q.kind == game::QuickSlot::Kind::Action && context.slotText) slot->tooltip = context.slotText(q);
    // An item shortcut shows its stack's count (expendables).
    slot->count = 0;
    slot->countAlways = false;
    if (q.kind == game::QuickSlot::Kind::Item && context.state != nullptr && context.items != nullptr) {
        const game::ItemInstance* item = context.state->inventory.At(std::uint8_t(q.value));
        const assets::ItemRow* row = item != nullptr ? context.items->Find(item->refId) : nullptr;
        if (row != nullptr && game::KindOf(*row) == game::ItemKind::Expendable) {
            slot->count = item->count;
            slot->countAlways = true;
        }
    }
    // Its wait: the clock, the ready flash, the time left (underbar slots write it).
    const HudContext::Cooling c = context.slotCooling && q.kind != game::QuickSlot::Kind::Empty ? context.slotCooling(q) : HudContext::Cooling{};
    slot->cooling = c.cooling;
    slot->charge = c.charge;
    slot->seconds = c.seconds;
}

} // namespace

void LinkQuickSlotDrops(ui::Widget& window, HudContext& context) {
    HudContext* c = &context;
    // Dropped on a quick slot of either bar: the two swap (or the slot takes what came from
    // another window); dropped anywhere else on the bar: kept where it was.
    window.onDrop = [c](ui::Widget* source, ui::Widget* target) {
        if (c->state == nullptr) return false;
        game::QuickSlots& q = c->state->quickSlots;
        const int to = QuickSlotOf(target, q.page), from = QuickSlotOf(source, q.page);
        if (to < 0 || to == from) return true;
        if (from >= 0) {
            std::swap(q.slots[std::size_t(from)], q.slots[std::size_t(to)]);
            if (c->hud != nullptr) {
                c->hud->Request({HudRequest::Kind::QuickSlotChanged, from});
                c->hud->Request({HudRequest::Kind::QuickSlotChanged, to});
            }
        }
        // A learned skill from the Skills page: a shortcut to it (kind 73, its skilldata id).
        if (const std::uint32_t skill = DraggedSkill(source); skill != 0) {
            q.slots[std::size_t(to)] = {game::QuickSlot::Kind::Skill, skill};
            if (c->hud != nullptr) c->hud->Request({HudRequest::Kind::QuickSlotChanged, to});
            return true;
        }
        // An icon of the Action page: a shortcut to the action (kind 74, its actionwnddata id).
        if (const std::uint32_t action = DraggedAction(source); action != 0) {
            q.slots[std::size_t(to)] = {game::QuickSlot::Kind::Action, action};
            if (c->hud != nullptr) c->hud->Request({HudRequest::Kind::QuickSlotChanged, to});
            return true;
        }
        // An item from the inventory or the equipment: a shortcut to its place (kinds 70 and 71).
        if (const int place = InventoryPlaceOf(source); place >= 0) {
            q.slots[std::size_t(to)] = {place >= game::slot::kWorn ? game::QuickSlot::Kind::Item : game::QuickSlot::Kind::Equipped,
                                        std::uint32_t(place)};
            if (c->hud != nullptr) c->hud->Request({HudRequest::Kind::QuickSlotChanged, to});
        }
        return true;
    };
    // A quick slot let go anywhere but on a bar is cleared.
    window.onPickEnd = [c](ui::Widget* source, ui::Widget* taker) {
        if (c->state == nullptr) return;
        const bool kept = taker != nullptr && (dynamic_cast<UnderBar*>(taker->Root()) != nullptr || dynamic_cast<MenuBar*>(taker->Root()) != nullptr ||
                                               dynamic_cast<ExtraQuickSlots*>(taker->Root()) != nullptr);
        const int from = QuickSlotOf(source, c->state->quickSlots.page);
        if (kept || from < 0) return;
        c->state->quickSlots.slots[std::size_t(from)] = {};
        if (c->hud != nullptr) c->hud->Request({HudRequest::Kind::QuickSlotChanged, from});
    };
}

// --- The underbar ----------------------------------------------------------------------------------

int UnderBar::SlotIndex(int controlId, int) {
    // 19 the M slot (0); 20..59 the four pages (1..40); 60..69 the extra bar's (41..50).
    if (controlId == 19) return game::QuickSlots::kRightButton;
    if (controlId >= 20 && controlId < 60) return controlId - 19;
    if (controlId >= 60 && controlId < 70) return controlId - 19;
    return -1;
}

void UnderBar::Created(ui::Interface& ui) {
    // Its three sections, in order: the bar, the extra bar's ten slots, the key labels over the
    // slots.
    AddSection(ui, *this, kUnderBarLayout, "AdditionalQuickSlot");
    AddSection(ui, *this, kUnderBarLayout, "QuickSlotNumber");
    // The extra bar's slots never show here: the extra bar draws them.
    for (int id = 60; id < 70; ++id) {
        if (Widget* w = Child(id)) w->visible = false;
    }
    if (Widget* frame = Child(200)) frame->visible = false;
    if (auto* sp = ui::As<ui::Gauge>(Child(16))) sp->SetValue(0.0f, true);
    LinkQuickSlotDrops(*this, context_);
}

void UnderBar::Update(ui::Frame& frame) {
    game::PlayerState* state = context_.state;
    if (state == nullptr) return;
    game::QuickSlots& q = state->quickSlots;
    // The page's ten slots, the page as F1..F4.
    for (int id = 20; id < 60; ++id) {
        auto* slot = ui::As<ui::Slot>(Child(id));
        if (slot == nullptr) continue;
        slot->visible = (id - 20) / game::QuickSlots::kPerPage == q.page;
        if (slot->visible) ShowSlot(slot, context_, q.slots[std::size_t(id - 19)]);
    }
    ShowSlot(ui::As<ui::Slot>(Child(19)), context_, q.slots[game::QuickSlots::kRightButton]);
    if (Widget* page = Child(11); page != nullptr) {
        const std::string text = std::format("F{}", q.page + 1);
        if (page->text() != text) page->SetText(text);
    }
    // The percentage of the level (never 100), the skill experience gauge and the skill points.
    const game::CharacterStatus& s = state->self;
    const double percent = std::min(s.ExperienceRatio() * 100.0, 99.99);
    if (Widget* text = Child(18)) text->SetText(std::format("{:.2f} %", percent));
    if (auto* sp = ui::As<ui::Gauge>(Child(16))) sp->SetValue(float(s.skillExperience) / 400.0f);
    if (Widget* text = Child(15)) text->SetText(std::to_string(s.skillPoints));
    // The "used" frame over the column last used, fading over 4 s and then staying, unseen.
    if (Widget* used = Child(200)) {
        used->visible = q.usedColumn >= 0 && q.usedAt >= 0;
        if (used->visible) {
            used->rect.x = 289.0f + 36.0f * float(q.usedColumn) - 4.0f;
            used->rect.y = 8.0f;
            used->alpha = float(std::clamp(1.0 - (context_.time - q.usedAt) / 4.0, 0.0, 1.0));
        }
    }
    // The round buttons' help.
    static constexpr std::pair<int, const char*> kTips[] = {
        {4, "UIIT_STT_TOGGLE_MENU"}, {5, "UIIT_STT_TOGGLE_COMMUNITY"}, {6, "UIIT_PAG_SYSTEM"}, {7, "UIIT_STT_SILKMALL_SHORT_KEY"}};
    for (const auto& [id, key] : kTips) {
        if (Widget* b = Child(id); b != nullptr && frame.pointer.hovered == b) frame.interface.ShowTooltip(context_.Text(key), b->ScreenRect());
    }
}

void UnderBar::Draw(ui::Frame& frame) {
    Widget::Draw(frame);
    if (context_.state == nullptr) return;
    // Ten green cells, each a tenth of the level, the next one growing in thousandths with the
    // texture squeezed into it.
    const ui::Vec2 o = Origin();
    const ui::Color tint = ui::WithAlpha(ui::kWhite, Opacity());
    const double r = context_.state->self.ExperienceRatio();
    const int full = std::min(9, int(std::trunc(std::min(r * 10.0, 99.99))));
    for (int i = 0; i < full; ++i) frame.paint.Image("interface\\underbar\\ub_exp_bar.ddj", {o.x + 18.0f + 20.0f * float(i), o.y + 27.0f, 20, 20}, {0, 0}, {1, 1}, tint);
    const double p = std::min(r * 100.0, 99.99);
    const float w = std::trunc(float(int(std::trunc(p * 10.0)) % 100) / 100.0f * 20.0f);
    if (w > 0.0f) frame.paint.Image("interface\\underbar\\ub_exp_bar.ddj", {o.x + 18.0f + 20.0f * float(full), o.y + 27.0f, w, 20}, {0, 0}, {1, 1}, tint);
}

bool UnderBar::OnNotice(const ui::Notice& notice) {
    if (notice.source == nullptr || context_.hud == nullptr || context_.state == nullptr) return false;
    game::QuickSlots& q = context_.state->quickSlots;
    if (notice.kind == ui::Notice::Kind::RightClicked) {
        // A right click uses the slot; only page 1's show the frame (the original's quirk, observed).
        const int index = SlotIndex(notice.control, q.page);
        if (index < 0) return false;
        context_.hud->UseQuickSlot(index, context_.time, index >= 1 && index <= game::QuickSlots::kPerPage);
        return true;
    }
    if (notice.kind != ui::Notice::Kind::Clicked || notice.source->parent() != this) return false;
    switch (notice.control) {
    case 4: context_.hud->ToggleMain(); return true;
    case 5: ToggleCommunityPage(context_); return true;
    case 6: context_.hud->Toggle(hud::kSystemMenu); return true;
    case 7: AskItemMall(context_); return true;
    case 12: q.page = (q.page + game::QuickSlots::kPages - 1) % game::QuickSlots::kPages; return true;
    case 13: q.page = (q.page + 1) % game::QuickSlots::kPages; return true;
    default: return false;
    }
}

// --- 1.188's underbar (CNIFUnderMenuBar) --------------------------------------------------------------

namespace {

// The controls whose tooltip names the key bound to their window ("Party (%s)", ...); the
// Character, Inventory and Skill buttons' plain ones first are replaced by these.
struct KeyTip {
    int control;
    const char* key;
    ui::KeyAction action;
};
constexpr KeyTip kKeyTips[] = {
    {83, "UIIT_STT_TOGGLE_CONTENTS_MENU_PARTY", ui::KeyAction::Party},
    {93, "UIIT_STT_TOGGLE_CONTENTS_MENU_PARTYMATCH", ui::KeyAction::PartyMatching},
    {96, "UIIT_STT_TOGGLE_CONTENTS_MENU_GUILD", ui::KeyAction::Community},
    {99, "UIIT_STT_TOGGLE_CONTENTS_MENU_TRAININGCAMP", ui::KeyAction::Academy},
    {105, "UIIT_STT_TOGGLE_CONTENTS_MENU_ACTION", ui::KeyAction::Actions},
    {108, "UIIT_STT_TOGGLE_CONTENTS_MENU_COMMUNITY", ui::KeyAction::Community},
    {111, "UIIT_STT_TOGGLE_CONTENTS_MENU_QUEST", ui::KeyAction::Quests},
    {114, "UIIT_STT_TOGGLE_CONTENTS_MENU_ENCHANT", ui::KeyAction::Alchemy},
    {123, "UIIT_STT_TOGGLE_CONTENTS_MENU_AUTOPOTION", ui::KeyAction::AutoPotion},
    {6, "UIIT_STT_TOGGLE_CONTENTS_MENU_CHARACTER", ui::KeyAction::Character},
    {7, "UIIT_STT_TOGGLE_CONTENTS_MENU_INVENTORY", ui::KeyAction::Inventory},
    {8, "UIIT_STT_TOGGLE_CONTENTS_MENU_SKILL", ui::KeyAction::Skills},
};

// The menu's rows (CNIFMenuButton under the popup 59) and what each opens (1.188's table).
constexpr int kMenuRows[] = {83, 93, 96, 99, 102, 104, 105, 108, 111, 114, 117, 120, 123, 126};

void PutText(ui::Widget* w, const std::string& text) {
    if (w != nullptr && w->text() != text) w->SetText(text);
}

} // namespace

int MenuBar::SlotIndex(int controlId, int page) {
    if (controlId == 50) return game::QuickSlots::kRightButton;
    if (controlId >= 40 && controlId < 50) return game::QuickSlots::Index(page, controlId - 40);
    return -1;
}

void MenuBar::Created(ui::Interface& ui) {
    ui::Static::Created(ui);
    const auto* records = context_.layout2dt ? context_.layout2dt(kMenuBarLayout) : nullptr;
    if (records == nullptr || records->empty()) return;
    texture = records->front().art; // ub_new_mainbar2.ddj
    Build2dt(ui, *this, *records, [](const assets::Ui2dtRecord& r, assets::UiLayoutNode& n) {
        if (r.className == "CIFSlotWithHelpEx") n.type = "CIFSlotWithHelp";
        // The "used" mark (ub_slot_arrow.ddj) has no class in the layout: an image of its art (it was a bare
        // widget, drawn as nothing: the keys' press showed no mark; the user's report 2026-11-13).
        if (r.id == 103 && r.className.empty()) {
            n.type = "CIFStatic";
            n.ddj = r.art; // its kind keeps no art in the node
        }
    });
    // "EXP" by the bar, the menu hidden, the pets' strip hidden until a pet (ours: not yet), the "used"
    // mark hidden, the stall network's row hidden (the stall's in its place; the layout's note: gone from Legend 10).
    PutText(Child(14), context_.Text("UIO_CHARINFO_STT_EXP"));
    menu_ = Child(59);
    if (menu_ != nullptr) menu_->visible = false;
    for (int id : {16, 103}) {
        if (Widget* w = Child(id)) w->visible = false;
    }
    // The mark over the slots (the layout lists it before them: under them it was hidden), clicks through to the slot;
    // the menu's popup kept over both.
    if (Widget* used = Child(103)) {
        used->clickThrough = true;
        used->BringToFront();
    }
    if (menu_ != nullptr) menu_->BringToFront();
    if (menu_ != nullptr) {
        if (Widget* w = menu_->Child(120)) w->visible = false;
    }
    LinkQuickSlotDrops(*this, context_);
}

void MenuBar::FitMenu(ui::Painter& paint) {
    // the menu as wide as its longest label needs: the most a row's text overruns its label, plus 10,
    // added to the popup, its tile, every row and the three lines (ours: its right edge kept above the Menu button).
    if (menu_ == nullptr) return;
    float over = 0.0f;
    bool measured = false;
    for (int id : kMenuRows) {
        Widget* row = menu_->Child(id);
        Widget* label = row != nullptr ? row->Child(200) : nullptr;
        if (label == nullptr || label->text().empty()) continue;
        const float width = paint.TextWidth(label->text(), label->node().fontIndex);
        measured = measured || width > 0.0f;
        over = std::max(over, width - label->rect.w);
    }
    if (!measured) return; // the font is not in yet
    menuFitted_ = true;
    if (over <= 0.0f) return;
    const float add = std::ceil(over) + 10.0f;
    menu_->rect.x -= add;
    menu_->rect.w += add;
    for (int id : {62, 70, 79, 98}) {
        if (Widget* w = menu_->Child(id)) w->rect.w += add;
    }
    for (int id : kMenuRows) {
        Widget* row = menu_->Child(id);
        if (row == nullptr) continue;
        row->rect.w += add;
        if (Widget* label = row->Child(200)) label->rect.w += add;
    }
}

void MenuBar::ToggleMenu() {
    // the bar raised, the menu shown or hidden.
    if (menu_ == nullptr) return;
    if (context_.hud != nullptr && context_.hud->interface() != nullptr) context_.hud->interface()->Raise(this);
    menu_->visible = !menu_->visible;
}

void MenuBar::Update(ui::Frame& frame) {
    if (!menuFitted_) FitMenu(frame.paint);
    game::PlayerState* state = context_.state;
    if (state == nullptr) return;
    game::QuickSlots& q = state->quickSlots;
    // The page's ten slots and the mouse slot; the page as "F%d" (four pages, the arrows wrap).
    for (int i = 0; i < game::QuickSlots::kPerPage; ++i) ShowSlot(ui::As<ui::Slot>(Child(40 + i)), context_, q.slots[std::size_t(SlotIndex(40 + i, q.page))]);
    ShowSlot(ui::As<ui::Slot>(Child(50)), context_, q.slots[game::QuickSlots::kRightButton]);
    PutText(Child(57), std::format("F{}", q.page + 1));
    // The experience: the level's percentage (at most 99.99) by the level, its tens digit at the bar's left,
    // the ten cells a percent each within that tenth (the last one part-filled), and how far into the tenth in the
    // middle ("%d%%" of the tenth's remainder, +0.005, times 10); the skill experience (400 a point) and the points.
    const game::CharacterStatus& s = state->self;
    const float percent = std::min(float(s.ExperienceRatio() * 100.0), 99.99f);
    const int tens = int(percent) / 10;
    const float rest = percent - float(tens * 10) + 0.005f;
    const int units = int(rest);
    for (int i = 0; i < 10; ++i) {
        if (auto* cell = ui::As<ui::Gauge>(Child(20 + i))) cell->SetValue(i < units ? 1.0f : i == units ? rest - float(units) : 0.0f, true);
    }
    PutText(Child(17), std::format("{}%", int(rest * 10.0f)));
    PutText(Child(13), std::format("{}. {}", context_.Text("UIIT_STT_LEVEL_LV"), s.level));
    PutText(Child(15), std::format("{:.2f} %", percent));
    PutText(Child(10), std::to_string(tens));
    if (auto* sp = ui::As<ui::Gauge>(Child(5))) sp->SetValue(float(s.skillExperience) / 400.0f, true);
    PutText(Child(12), std::to_string(s.skillPoints));
    // The "used" mark over the column last used, 4 left and 3 up of its slot, fading over 4 s.
    if (Widget* used = Child(103)) {
        Widget* slot = q.usedColumn >= 0 ? Child(40 + q.usedColumn) : nullptr;
        used->visible = slot != nullptr && q.usedAt >= 0 && context_.time - q.usedAt < 4.0;
        if (used->visible) {
            used->rect.x = slot->rect.x - 4.0f;
            used->rect.y = slot->rect.y - 3.0f;
            used->alpha = float(std::clamp(1.0 - (context_.time - q.usedAt) / 4.0, 0.0, 1.0));
        }
    }
    // The tooltips: the Item Mall's plain, the others naming their key.
    const auto hovered = [&](Widget* w) { return w != nullptr && w->visible && frame.pointer.hovered == w; };
    if (Widget* mall = Child(9); hovered(mall)) frame.interface.ShowTooltip(context_.Text("UIIT_STT_SILKMALL_SHORT_KEY"), mall->ScreenRect());
    for (const KeyTip& tip : kKeyTips) {
        Widget* w = tip.control < 10 ? Child(tip.control) : menu_ != nullptr ? menu_->Child(tip.control) : nullptr;
        if (!hovered(w) || context_.hud == nullptr) continue;
        frame.interface.ShowTooltip(FormatText(context_.Text(tip.key), {BoundKeyName(context_.hud->bindings(), tip.action)}), w->ScreenRect());
    }
}

bool MenuBar::OnNotice(const ui::Notice& notice) {
    if (notice.source == nullptr || context_.hud == nullptr || context_.state == nullptr) return false;
    game::QuickSlots& q = context_.state->quickSlots;
    if (notice.kind == ui::Notice::Kind::RightClicked) {
        // A right click uses the slot; the "used" mark only for the first page's (marks keys 1..10).
        const int index = SlotIndex(notice.control, q.page);
        if (index < 0 || notice.source->parent() != this) return false;
        context_.hud->UseQuickSlot(index, context_.time, index >= 1 && index <= game::QuickSlots::kPerPage);
        return true;
    }
    if (notice.kind != ui::Notice::Kind::Clicked) return false;
    const bool row = menu_ != nullptr && notice.source->parent() == menu_;
    if (notice.source->parent() != this && !row) return false;
    switch (notice.control) {
    case 4: ToggleMenu(); return true;
    case 19: q.page = (q.page + 1) % game::QuickSlots::kPages; return true;
    case 18: q.page = (q.page + game::QuickSlots::kPages - 1) % game::QuickSlots::kPages; return true;
    case 6:
    case 7:
    case 8:
    case 9: Choose(notice.control); return true;
    default:
        if (row && std::find(std::begin(kMenuRows), std::end(kMenuRows), notice.control) != std::end(kMenuRows)) {
            Choose(notice.control);
            return true;
        }
        return false;
    }
}

void MenuBar::Choose(int control) {
    // A Menu row chosen, by the control pressed; then the menu closes. The windows the original opens with a "show" (its
    // calls' argument 1) are shown; the pages of the main window and the bar's three buttons toggle as their keys do.
    Hud& hud = *context_.hud;
    switch (control) {
    case 6: hud.TogglePage(hud::kPageCharacter); break;
    case 7: hud.TogglePage(hud::kPageInventory); break;
    case 8: hud.TogglePage(hud::kPageSkills); break;
    case 9: AskItemMall(context_); break;
    case 83: hud.TogglePage(hud::kPageParty); break;
    case 93: hud.Show(hud::kPartyMatch); break;
    case 96: ShowGuildSide(context_, false); break; // the guild window (176 in 1.188) on its guild page
    case 99: hud.TogglePage(hud::kPageAcademy); break;
    case 102: break; // Guardian Matching (CIFMentorMatch): ours, not built
    case 104: ShowCollectionBook(context_); break;
    case 105: hud.TogglePage(hud::kPageActions); break;
    case 108: ShowGuildSide(context_, true); break;
    case 111: hud.Request({HudRequest::Kind::ToggleQuests}); break;
    case 114: hud.Request({HudRequest::Kind::ToggleAlchemy}); break; //, case 0x72
    case 117: // "Stall": the Action page's stall (its action 1009)
        if (context_.useAction) context_.useAction(1009);
        break;
    case 120: hud.Show(hud::kStallNetwork); break;
    case 123: hud.Show(hud::kAutoPotion); break;
    case 126: hud.Show(hud::kSystemMenu); break;
    default: break;
    }
    if (menu_ != nullptr) menu_->visible = false;
}

// --- The extra quick slots ----------------------------------------------------------------------------

void ExtraQuickSlots::Created(ui::Interface& ui) {
    if (context_.options != nullptr) {
        const core::ExtraBarOptions& o = context_.options->extraBar;
        open_ = o.open, vertical_ = o.vertical, twoLines_ = o.twoLines, seeThrough_ = o.seeThrough, locked_ = o.locked, fixed_ = o.fixed;
    }
    Shape(ui);
    ShowOpen();
}

void ExtraQuickSlots::Save() {
    if (context_.options == nullptr) return;
    context_.options->extraBar = {open_, vertical_, twoLines_, seeThrough_, locked_, fixed_};
    if (context_.optionsChanged) context_.optionsChanged();
}

void ExtraQuickSlots::Apply(const Settings& s) {
    const bool relayout = s.twoLines != twoLines_;
    seeThrough_ = s.seeThrough, locked_ = s.locked, fixed_ = s.fixed, twoLines_ = s.twoLines;
    if (relayout) {
        reshape_ = true;
    } else if (context_.hud != nullptr && context_.hud->interface() != nullptr) {
        Shape(*context_.hud->interface());
        ShowOpen();
    }
    Save();
}

void ExtraQuickSlots::ToggleOptions() {
    ui::Interface* ui = context_.hud != nullptr ? context_.hud->interface() : nullptr;
    if (ui == nullptr) return;
    for (ui::Widget* w : ui->Windows()) {
        if (w->node().type == "CIFExtQuickSlotOption" && w->visible) {
            ui->Close(w);
            return;
        }
    }
    const assets::UiLayout* l = ui->Layout(kExtraLayout);
    if (l == nullptr) return;
    const assets::UiLayoutNode* node = nullptr;
    for (const auto& s : l->sections) {
        if (s.name == "Option" && !s.nodes.empty()) node = &s.nodes.front();
    }
    if (node == nullptr) return;
    ui::Widget* w = ui->Open(*node);
    if (w == nullptr) return;
    // Beside the bar, on the screen's wider side: a bar down the screen, right of it on the left half, else left
    // of it; a bar across, under it on the upper half, else over it.
    const ui::Vec2 screen = ui->screen();
    if (vertical_) {
        w->rect.x = rect.x < screen.x / 2.0f ? rect.x + rect.w : rect.x - w->rect.w;
        w->rect.y = rect.y;
    } else {
        w->rect.x = rect.x;
        w->rect.y = rect.y < screen.y / 2.0f ? rect.y + rect.h : rect.y - w->rect.h;
    }
}

void ExtraQuickSlots::Shape(ui::Interface& ui) {
    // Without saved options: open, vertical, two lines, see-through: the Type4 section. The window is its handle; the
    // tiles and slots hang below it.
    ClearChildren();
    const std::string section = vertical_ ? (twoLines_ ? "Type4" : "Type3") : (twoLines_ ? "Type2" : "Type1");
    AddSection(ui, *this, kExtraLayout, section);
    if (Widget* handle = Child(10)) {
        rect.w = handle->rect.w;
        rect.h = handle->rect.h;
    }
    // See-through: alpha 110 on the handle, its buttons, the tiles and the slots.
    const float a = seeThrough_ ? 110.0f / 255.0f : 1.0f;
    for (const auto& c : children()) c->alpha = a;
    LinkQuickSlotDrops(*this, context_);
    // Locked: the slots' shortcuts stay (none picked up); fixing the position: the handle moves nothing.
    for (int i = 0; i < 10; ++i) {
        if (auto* slot = ui::As<ui::Slot>(Child(100 + i))) slot->pickable = !locked_;
    }
    if (auto* handle = dynamic_cast<ui::DragArea*>(Child(10))) handle->fixed = fixed_;
}

void ExtraQuickSlots::Update(ui::Frame&) {
    if (reshape_ && context_.hud != nullptr && context_.hud->interface() != nullptr) {
        reshape_ = false;
        Shape(*context_.hud->interface());
        ShowOpen();
    }
    if (context_.state == nullptr) return;
    for (int i = 0; i < 10; ++i) {
        auto* slot = ui::As<ui::Slot>(Child(100 + i));
        if (slot == nullptr) continue;
        slot->visible = open_;
        ShowSlot(slot, context_, context_.state->quickSlots.slots[std::size_t(game::QuickSlots::kExtraFirst + i)]);
    }
}

bool ExtraQuickSlots::OnNotice(const ui::Notice& notice) {
    if (notice.source == nullptr || context_.hud == nullptr) return false;
    if (notice.kind == ui::Notice::Kind::RightClicked && notice.control >= 100 && notice.control < 110) {
        context_.hud->UseQuickSlot(game::QuickSlots::kExtraFirst + notice.control - 100, context_.time, false);
        return true;
    }
    if (notice.kind != ui::Notice::Kind::Clicked || notice.source->parent() != this) return false;
    if (notice.control == 14) {
        // Open / close: the tiles, the slots and the end piece.
        open_ = !open_;
        ShowOpen();
        Save();
        return true;
    }
    if (notice.control == 11) {
        ToggleOptions();
        return true;
    }
    if ((notice.control == 12 && vertical_) || (notice.control == 13 && !vertical_)) {
        // GDR_EXT_QUICKSLOT_HORI_BTN (12) / _VERT_BTN (13): the bar laid out across or down, its lines kept
        // (resinfo\ifextquickslot.txt's Type1..Type4); the user's report 2026-11-14: both did nothing.
        vertical_ = notice.control == 13;
        reshape_ = true; // next frame: the button pressed is one of the children Shape replaces
        Save();
        return true;
    }
    return notice.control >= 11 && notice.control <= 13;
}

void ExtraQuickSlots::ShowOpen() {
    for (const auto& c : children()) {
        if ((c->id() >= 30 && c->id() < 40) || (c->id() >= 100 && c->id() < 110) || c->id() == 20) c->visible = open_;
    }
    // Open, the button closes it (its close face); closed, its open face.
    if (auto* b = ui::As<ui::Button>(Child(14))) {
        const char* face = vertical_ ? (open_ ? "qsl_vclose_button.ddj" : "qsl_vopen_button.ddj") : (open_ ? "qsl_hclose_button.ddj" : "qsl_hopen_button.ddj");
        b->texture = std::string("interface\\quick_slot\\") + face;
    }
}

// --- The extra quick bar's options --------------------------------------------------------------------

namespace {

// CIFExtQuickSlotOption (the bar's Option section, 290 x 223, resinfo\\ifextquickslotoption.txt): Invisible (25), Lock (26),
// Fixing position (27), Two lines (28); Apply (36) sets them on the bar, Confirm (35) sets them and closes.
class ExtraBarOptionWindow : public ui::MainFrame {
public:
    explicit ExtraBarOptionWindow(const assets::UiLayoutNode& node) : MainFrame(node) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        ui_ = &ui;
        // Its controls are its class's own layout's (ifextquickslotoption.txt), made with it; the ticks as the bar has them.
        if (ExtraQuickSlots* bar = Bar()) {
            const ExtraQuickSlots::Settings s = bar->Current();
            const std::pair<int, bool> ticks[] = {{25, s.seeThrough}, {26, s.locked}, {27, s.fixed}, {28, s.twoLines}};
            for (const auto& [id, on] : ticks) {
                if (auto* box = ui::As<ui::CheckBox>(Child(id))) box->checked = on;
            }
        }
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr && n.source->parent() == this && (n.control == 35 || n.control == 36)) {
            const auto ticked = [&](int id) {
                const auto* box = ui::As<ui::CheckBox>(Child(id));
                return box != nullptr && box->checked;
            };
            if (ExtraQuickSlots* bar = Bar()) bar->Apply({ticked(25), ticked(26), ticked(27), ticked(28)});
            if (n.control == 35 && ui_ != nullptr) ui_->Close(this);
            return true;
        }
        return ui::MainFrame::OnNotice(n);
    }

private:
    ExtraQuickSlots* Bar() const { return ui_ != nullptr ? dynamic_cast<ExtraQuickSlots*>(ui_->Window(hud::kExtQuickSlots)) : nullptr; }
    ui::Interface* ui_ = nullptr;
};

} // namespace

void RegisterExtraBarOptions(ui::Interface& ui, HudContext&) {
    ui.Register("CIFExtQuickSlotOption", [](const assets::UiLayoutNode& n) { return std::make_unique<ExtraBarOptionWindow>(n); });
}

} // namespace sr::client
