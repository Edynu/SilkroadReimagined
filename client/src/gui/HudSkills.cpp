// The Skills page (C7f): masteries by tab, the board of a
// mastery's skills, learning and raising, dragging a learned skill onto the quick slots.

#include <algorithm>
#include <format>
#include <map>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/game/Skills.h"

namespace sr::client {

namespace {

constexpr const char* kSkillLayout = "resinfo\\ifskill.txt";
// The board's list (its scroll manager, set up by the board): rows 55 apart (a row's
// bar is 60 tall, its top 3 lines clear, so each overlaps the one above by 5), 4 of them shown
// (4 x 55 + 5 = the list's 225), the bar's range the rows less 4, the bar 3 lower.
constexpr float kRowStep = 55.0f;
constexpr int kRowsShown = 4;

bool Inside(ui::Rect r, ui::Vec2 p) { return p.x >= r.x && p.y >= r.y && p.x <= r.Right() && p.y <= r.Bottom(); }

// The tables' icons: under the icon folder (skillmasterydata's are written with it).
std::string IconPath(const std::string& path) {
    if (path.empty()) return {};
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return lower.starts_with("icon\\") ? path : "icon\\" + path;
}

void AddSection(ui::Interface& ui, ui::Widget& window, const char* layout, const std::string& name) {
    const assets::UiLayout* l = ui.Layout(layout);
    if (l == nullptr) return;
    for (const auto& s : l->sections) {
        if (s.name != name) continue;
        for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) window.Add(ui.Make(*it));
    }
}

ui::Widget* MakeNode(ui::Interface& ui, ui::Widget& parent, const std::string& type, const std::string& name, int id, ui::Rect rect,
                     const std::string& ddj = {}, const std::string& text = {}) {
    assets::UiLayoutNode n;
    n.type = type;
    n.name = name;
    n.id = id;
    n.rect = {rect.x, rect.y, rect.w, rect.h};
    n.ddj = ddj;
    n.text = text;
    n.hAlign = 1;
    auto made = ui.Make(n);
    ui::Widget* w = made.get();
    parent.Add(std::move(made));
    return w;
}

} // namespace

// CIFSkill (73, page of the main window).
class SkillWindow : public ui::Widget {
public:
    SkillWindow(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;
    // The skill a widget of the board shows when it may be dragged (learned, not passive); 0 none.
    std::uint32_t SkillOf(const ui::Widget* w) const;

private:
    struct SlotView {
        ui::Widget* box = nullptr;
        ui::Slot* icon = nullptr;
        ui::Widget* button = nullptr;
        const assets::SkillRow* first = nullptr; // the group's level 1
        const assets::SkillRow* learned = nullptr;
        const assets::SkillRow* next = nullptr;  // what the button learns (null: nothing)
        const assets::SkillRow* tipped = nullptr; // the level its tooltip was made for
    };
    void Rebuild();          // tabs and mastery buttons from the server's mastery list (once)
    void MakeTabs();
    void SelectTab(int tab);
    std::vector<std::uint32_t> masteryOfButton_;
    void ShowMastery(std::uint32_t mastery);
    void Refresh();          // the numbers, the buttons, each slot's state
    game::SkillBook Book() const;
    int MasteryLevel(std::uint32_t mastery) const;
    void Ask(const assets::SkillRow& skill);

    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    std::uint64_t seen_ = ~0ull;
    std::vector<int> tabs_;                       // tab ids, in the list's order
    std::vector<std::uint32_t> masteriesShown_;   // the selected tab's masteries
    std::vector<ui::Widget*> tabButtons_, masteryButtons_; // made once (the list's masteries never change)
    // Each mastery's rows and slots, made the first time it is shown and kept (shown or hidden).
    struct Board {
        std::vector<ui::Widget*> rows;
        std::vector<SlotView> slots;
    };
    std::map<std::uint32_t, Board> boards_;
    Board current_;
    std::vector<std::uint32_t> listed_; // the mastery ids the tabs were made for
    int tab_ = 0;
    std::uint32_t mastery_ = 0;
    int firstRow_ = -1;
    // Inside the withdrawal window (CIFSkillWithdrawal, HudServices.cpp): its buttons withdraw a level.
    bool withdrawal_ = false;
};

std::uint32_t DraggedSkill(const ui::Widget* source) {
    for (const ui::Widget* w = source; w != nullptr; w = w->parent()) {
        if (const auto* window = dynamic_cast<const SkillWindow*>(w)) return window->SkillOf(source);
        // A socket's active skill from the equipment's skill window.
        if (const auto* sockets = dynamic_cast<const EquipSkillWindow*>(w)) return sockets->SkillOf(source);
    }
    return 0;
}

std::unique_ptr<ui::Widget> MakeSkillWindow(const assets::UiLayoutNode& node, HudContext& context) {
    return std::make_unique<SkillWindow>(node, context);
}

void SkillWindow::Created(ui::Interface& ui) {
    ui_ = &ui;
    // Type 1 (the Skills page): the skill points and the mastery total along the bottom.
    AddSection(ui, *this, kSkillLayout, "MainSkillWnd");
    if (Widget* board = Child(10)) {
        if (Widget* up = board->Child(14)) up->visible = false;
        if (Widget* list = board->Child(10)) {
            if (Widget* bar = list->Child(10)) bar->rect.y += 3.0f;
        }
    }
}

game::SkillBook SkillWindow::Book() const {
    game::SkillBook book;
    if (context_.state == nullptr || context_.skills == nullptr) return book;
    for (const auto& [mastery, level] : context_.state->masteries) {
        if (level > 0) book.masteries[mastery] = level;
    }
    for (const std::uint32_t id : context_.state->skills) {
        if (const assets::SkillRow* row = context_.skills->Find(id)) book.groups[row->group] = row->level;
    }
    return book;
}

int SkillWindow::MasteryLevel(std::uint32_t mastery) const {
    if (context_.state == nullptr) return 0;
    for (const auto& [id, level] : context_.state->masteries) {
        if (id == mastery) return level;
    }
    return 0;
}

void SkillWindow::Rebuild() {
    // One tab per tab id the listed masteries have, in the list's order; made
    // once for the character's list (a learned level does not change it).
    if (context_.state == nullptr || context_.skills == nullptr) return;
    std::vector<std::uint32_t> listed;
    for (const auto& [id, level] : context_.state->masteries) listed.push_back(id);
    if (listed != listed_ && tabButtons_.empty()) {
        listed_ = listed;
        MakeTabs();
    }
    SelectTab(tab_);
}

void SkillWindow::MakeTabs() {
    tabs_.clear();
    for (const auto& [id, level] : context_.state->masteries) {
        const assets::MasteryRow* m = context_.skills->Mastery(id);
        if (m != nullptr && std::find(tabs_.begin(), tabs_.end(), m->tab) == tabs_.end()) tabs_.push_back(m->tab);
    }
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        std::string key;
        for (const auto& [id, level] : context_.state->masteries) {
            const assets::MasteryRow* m = context_.skills->Mastery(id);
            if (m != nullptr && m->tab == tabs_[i]) {
                key = m->tabKey;
                break;
            }
        }
        ui::Widget* b = MakeNode(*ui_, *this, "CIFButton", "GDR_SKILL_TAB", 300 + int(i), {4.0f + 62.0f * float(i), -23.0f, 60.0f, 24.0f},
                                 "interface\\ifcommon\\com_tab_off.ddj", key);
        b->SetText(context_.Text(key));
        tabButtons_.push_back(b);
    }
    // Every mastery's button (68 x 28, 68 apart along the top), shown with its tab.
    std::map<int, int> perTab;
    for (const auto& [id, level] : context_.state->masteries) {
        const assets::MasteryRow* m = context_.skills->Mastery(id);
        if (m == nullptr) continue;
        const int n = perTab[m->tab]++;
        ui::Widget* b = MakeNode(*ui_, *this, "CIFButton", "GDR_SKILL_MASTERY", 400 + int(masteryButtons_.size()),
                                 {12.0f + 68.0f * float(n), 4.0f, 68.0f, 28.0f}, "interface\\skill\\skl_mastery_tab_off.ddj");
        b->SetText(context_.Text(m->nameKey));
        masteryButtons_.push_back(b);
        masteryOfButton_.push_back(id);
    }
}

void SkillWindow::SelectTab(int tab) {
    tab_ = std::clamp(tab, 0, std::max(0, int(tabs_.size()) - 1));
    masteriesShown_.clear();
    for (std::size_t i = 0; i < tabButtons_.size(); ++i) {
        if (auto* s = ui::As<ui::Static>(tabButtons_[i])) s->texture = int(i) == tab_ ? "interface\\ifcommon\\com_tab_on.ddj" : "";
    }
    for (std::size_t i = 0; i < masteryButtons_.size(); ++i) {
        const assets::MasteryRow* m = context_.skills->Mastery(masteryOfButton_[i]);
        const bool shown = m != nullptr && !tabs_.empty() && m->tab == tabs_[std::size_t(tab_)];
        masteryButtons_[i]->visible = shown;
        if (shown) masteriesShown_.push_back(masteryOfButton_[i]);
    }
    std::uint32_t pick = mastery_;
    if (std::find(masteriesShown_.begin(), masteriesShown_.end(), pick) == masteriesShown_.end()) pick = masteriesShown_.empty() ? 0 : masteriesShown_.front();
    ShowMastery(pick);
}

void SkillWindow::ShowMastery(std::uint32_t mastery) {
    for (std::size_t i = 0; i < masteryButtons_.size(); ++i) {
        if (auto* s = ui::As<ui::Static>(masteryButtons_[i])) s->texture = masteryOfButton_[i] == mastery ? "interface\\skill\\skl_mastery_tab_on.ddj" : "";
    }
    Widget* board = Child(10);
    Widget* rowsBox = board != nullptr ? board->Child(10) : nullptr;
    if (rowsBox == nullptr || context_.skills == nullptr) return;
    // The board shown so far goes back to its place, hidden; this one's comes out (made the first time).
    if (mastery == mastery_ && (!current_.rows.empty() || mastery == 0)) {
        Refresh();
        return;
    }
    if (mastery_ != 0) {
        for (ui::Widget* w : current_.rows) w->visible = false;
        boards_[mastery_] = current_;
    }
    mastery_ = mastery;
    current_ = {};
    if (const auto kept = boards_.find(mastery); kept != boards_.end()) {
        current_ = kept->second;
        if (const assets::MasteryRow* m = context_.skills->Mastery(mastery)) {
            if (auto* icon = ui::As<ui::Slot>(board->Child(11))) icon->icon = IconPath(m->iconPath);
            if (Widget* name = board->Child(12)) name->SetText(context_.Text(m->nameKey) + " " + context_.Text("PARAM_MASTERY"));
        }
        firstRow_ = -1;
        if (auto* scroll = ui::As<ui::ScrollBar>(rowsBox->Child(10))) {
            scroll->SetRange(0, std::max(0, int(current_.rows.size()) - kRowsShown));
            scroll->SetPosition(0);
        }
        Refresh();
        return;
    }
    if (mastery == 0) return;
    // The header: the mastery's icon and name.
    if (const assets::MasteryRow* m = context_.skills->Mastery(mastery)) {
        if (auto* icon = ui::As<ui::Slot>(board->Child(11))) icon->icon = IconPath(m->iconPath);
        if (Widget* name = board->Child(12)) name->SetText(context_.Text(m->nameKey) + " " + context_.Text("PARAM_MASTERY"));
    }
    // A row per board row (328 x 60, stacked kRowStep apart), each skill group at x 37 + 36 x its
    // column; the empty columns after the last used one shown as empty places.
    std::map<int, std::vector<const assets::SkillRow*>> byRow;
    for (const assets::SkillRow* s : context_.skills->Board(mastery)) byRow[s->boardRow].push_back(s);
    const int rowCount = byRow.empty() ? 0 : byRow.rbegin()->first + 1;
    for (int r = 0; r < rowCount; ++r) {
        ui::Widget* row = MakeNode(*ui_, *rowsBox, "CIFSkillGroup", "GDR_SKILL_GROUP", 100 + r, {0.0f, kRowStep * float(r), 328.0f, 60.0f},
                                   "interface\\skill\\skl_mastery_bar.ddj");
        current_.rows.push_back(row);
        if (auto* label = ui::As<ui::Slot>(row->Child(10))) {
            const assets::SkillGroupRow* g = context_.skills->BoardRowGroup(mastery, r);
            label->icon = g != nullptr && !g->iconPath.empty() ? IconPath(g->iconPath) : "";
            label->pickable = false;
            if (g != nullptr) label->tooltip = context_.Text(g->nameKey);
        }
        int last = -1;
        for (const assets::SkillRow* s : byRow[r]) last = std::max(last, s->boardColumn);
        for (int col = 0; col <= 7; ++col) {
            const auto it = std::find_if(byRow[r].begin(), byRow[r].end(), [&](const assets::SkillRow* s) { return s->boardColumn == col; });
            const ui::Rect at{37.0f + 36.0f * float(col), 3.0f, 36.0f, 56.0f};
            if (it == byRow[r].end()) {
                if (col > last) MakeNode(*ui_, *row, "CIFStatic", "GDR_SKILL_NOTHING", 200 + col, {at.x, at.y, 32.0f, 32.0f}, "interface\\skill\\skl_mastery_nothing.ddj");
                continue;
            }
            ui::Widget* box = MakeNode(*ui_, *row, "CIFSkillSlot", "GDR_SKILL_SLOT", 100 + col, at);
            SlotView v;
            v.box = box;
            v.icon = ui::As<ui::Slot>(box->Child(1));
            v.button = box->Child(2);
            v.first = *it;
            current_.slots.push_back(v);
        }
    }
    firstRow_ = -1; // placed on the next refresh
    rowsBox->clipChildren = true;
    // Under the first row's clear top lines the board shows through (its frame); every mastery has
    // more rows than the list shows, so its rows fill it to the bottom at every place of the bar.
    if (Widget* bar = rowsBox->Child(10)) {
        if (auto* scroll = ui::As<ui::ScrollBar>(bar)) {
            scroll->SetRange(0, std::max(0, rowCount - kRowsShown));
            scroll->SetPosition(0);
        }
    }
    Refresh();
}

void SkillWindow::Refresh() {
    if (context_.state == nullptr || context_.skills == nullptr) return;
    const game::CharacterStatus& self = context_.state->self;
    const game::SkillBook book = Book();
    const int country = self.race;
    // The bottom: skill points, the mastery total over the race's allowance.
    if (Widget* sp = Child(22)) sp->SetText(std::to_string(self.skillPoints));
    if (Widget* total = Child(24)) total->SetText(std::format("{}/{}", book.MasteryTotal(), game::MasteryAllowance(country, self.level)));
    // The potion's row (ifskill.txt 25 its icon, 26 its name, 27 how many: "%d" UIIT_STT_UNIT), shown in the withdrawal
    // window alone (its type 3).
    {
        const game::ItemInstance* potion = withdrawal_ && context_.withdrawPotion >= 0 && context_.items != nullptr
                                               ? context_.state->inventory.At(std::uint8_t(context_.withdrawPotion)) : nullptr;
        const assets::ItemRow* row = potion != nullptr ? context_.items->Find(potion->refId) : nullptr;
        int count = 0;
        if (row != nullptr) {
            for (const auto& [at, it] : context_.state->inventory.places) {
                if (at >= game::slot::kWorn && it.refId == row->refObjId) count += std::max<int>(1, it.count);
            }
        }
        for (const int id : {25, 26, 27}) {
            if (Widget* w = Child(id)) w->visible = row != nullptr;
        }
        if (row != nullptr) {
            if (auto* icon = ui::As<ui::Static>(Child(25))) icon->texture = row->iconPath.empty() ? std::string("icon\\icon_default.ddj") : "icon\\" + row->iconPath;
            if (Widget* name = Child(26)) name->SetText(context_.Text(row->nameKey));
            if (Widget* n = Child(27)) n->SetText(std::format("{} {}", count, context_.Text("UIIT_STT_UNIT")));
        }
    }
    Widget* board = Child(10);
    if (board == nullptr) return;
    // The header's level and its level-up button: at level 0, or with the next level within the
    // character's (and 120) and its skill points at hand.
    const int level = MasteryLevel(mastery_);
    if (Widget* lv = board->Child(13)) lv->SetText(FormatText(context_.Text("UIIT_STT_LV_LEVEL"), {std::to_string(level)}));
    if (Widget* up = board->Child(14)) {
        const std::uint32_t cost = context_.masteryCosts != nullptr ? game::MasteryCost(*context_.masteryCosts, level) : ~0u;
        up->visible = mastery_ != 0 && (level == 0 || (level + 1 <= self.level && level < 120 && cost <= self.skillPoints));
        if (withdrawal_) up->visible = mastery_ != 0 && level > 0; // ours: the original's withdrawal art not traced
    }
    // The rows shown from the scroll bar's place: the four from there.
    Widget* rowsBox = board->Child(10);
    int first = 0;
    if (auto* scroll = rowsBox != nullptr ? ui::As<ui::ScrollBar>(rowsBox->Child(10)) : nullptr) first = scroll->position();
    if (first != firstRow_) {
        firstRow_ = first;
        for (std::size_t r = 0; r < current_.rows.size(); ++r) {
            const int at = int(r) - first;
            current_.rows[r]->visible = at >= 0 && at < kRowsShown;
            current_.rows[r]->rect.y = kRowStep * float(at);
        }
    }
    // Each group: its learned level (the icon, the level digits, the up button while the next can
    // be learned, the max plate at the last), or its first level (half seen, or the disabled look
    // when the mastery is too low; the add button when it can be learned).
    std::map<std::uint32_t, const assets::SkillRow*> learned;
    for (const std::uint32_t id : context_.state->skills) {
        if (const assets::SkillRow* row = context_.skills->Find(id)) learned[row->group] = row;
    }
    for (SlotView& v : current_.slots) {
        const auto it = learned.find(v.first->group);
        v.learned = it != learned.end() ? it->second : nullptr;
        const assets::SkillRow* shown = v.learned != nullptr ? v.learned : v.first;
        v.next = v.learned != nullptr ? context_.skills->GroupLevel(v.first->group, v.learned->level + 1) : v.first;
        const bool canLearn = v.next != nullptr &&
                              game::CanLearnSkill(book, *context_.skills, *v.next, country, self.skillPoints, self.strength, self.intelligence) ==
                                  game::SkillCheck::Ok;
        if (v.icon != nullptr) {
            const bool reached = book.MasteryLevel(v.first->masteries[0]) >= v.first->masteryLevels[0];
            v.icon->icon = v.learned == nullptr && !reached ? "interface\\skill\\skl_mastery_disable.ddj" : "icon\\" + shown->iconPath;
            v.icon->alpha = v.learned == nullptr && reached ? 0.5f : 1.0f;
            // Not picked up in the withdrawal window: a click there asks to withdraw the skill.
            v.icon->pickable = !withdrawal_ && v.learned != nullptr && v.learned->activity != 0;
            // Its tooltip: the level shown (learned, or the first), all it does and asks.
            if (v.tipped != shown) {
                v.tipped = shown;
                v.icon->tooltip.clear();
                v.icon->tooltipSigned = false;
                v.icon->tooltipLines = [this, shown] { return SkillTooltip(*shown, context_); };
            }
            // A learned skill's wait, as on its quick slots.
            const HudContext::Cooling c = v.learned != nullptr && context_.skillCooling ? context_.skillCooling(v.learned->id) : HudContext::Cooling{};
            v.icon->cooling = c.cooling;
            v.icon->charge = c.charge;
        }
        if (v.button != nullptr) {
            auto* b = ui::As<ui::Static>(v.button);
            if (v.learned != nullptr && v.next == nullptr) {
                v.button->visible = true;
                v.button->enabled = false;
                if (b != nullptr) b->texture = "interface\\skill\\skl_level_max.ddj";
            } else {
                v.button->visible = canLearn;
                v.button->enabled = true;
                if (b != nullptr) b->texture = v.learned != nullptr ? "interface\\skill\\skl_button_up.ddj" : "interface\\skill\\skl_button_add.ddj";
            }
            // The withdrawal window: no learning buttons (the original's slot states 5..7 hide them); a
            // click on a learned skill asks to withdraw it.
            if (withdrawal_) v.button->visible = false;
        }
        // The level digits on their plate (one digit at 26,24; two at 24,24 and 29,24).
        const int lv = v.learned != nullptr ? v.learned->level : 0;
        if (Widget* plate = v.box->Child(3)) plate->visible = lv > 0;
        const std::string digits = lv > 0 ? std::to_string(lv) : std::string();
        for (int d = 0; d < 3; ++d) {
            auto* w = ui::As<ui::Static>(v.box->Child(4 + d));
            if (w == nullptr) continue;
            w->visible = d < int(digits.size());
            if (!w->visible) continue;
            w->texture = std::format("interface\\skill\\skl_lv_number_{}.ddj", digits[std::size_t(d)]);
            w->rect = {digits.size() == 1 ? 26.0f : 24.0f + 5.0f * float(d), 24.0f, 8.0f, 8.0f};
        }
    }
}

void SkillWindow::Update(ui::Frame& frame) {
    // The wheel over the board scrolls its rows.
    if (Widget* board = Child(10); board != nullptr && frame.input.wheel() != 0.0f && Inside(board->ScreenRect(), frame.pointer.position)) {
        if (auto* scroll = board->Child(10) != nullptr ? ui::As<ui::ScrollBar>(board->Child(10)->Child(10)) : nullptr) {
            scroll->SetPosition(scroll->position() + (frame.input.wheel() > 0.0f ? -1 : 1));
        }
    }
    if (context_.state == nullptr || context_.skills == nullptr) return;
    withdrawal_ = InWithdrawalWindow(this);
    if (context_.state->skillsVersion != seen_) {
        seen_ = context_.state->skillsVersion;
        Rebuild();
    }
    Refresh();
}

void SkillWindow::Ask(const assets::SkillRow& skill) {
    if (context_.hud == nullptr) return;
    // The practice box.
    const std::string name = context_.Text(skill.nameKey);
    SkillPracticeBox::Question q;
    q.icon = IconPath(skill.iconPath);
    q.name = name;
    q.level = skill.level;
    if (const assets::MasteryRow* m = context_.skills->Mastery(skill.masteries[0])) q.mastery = context_.Text(m->nameKey);
    q.description = context_.Text(skill.descKey);
    q.next = skill.level <= 1 ? FormatText(context_.Text("UIIT_STT_SKILL_LEARN"), {name})
                              : FormatText(context_.Text("UIIT_STT_SKILL_REINFORCEMENT_MSG"), {name, std::to_string(skill.level)});
    q.skillPoints = std::uint32_t(std::max(0, skill.spCost));
    Hud* hud = context_.hud;
    const std::uint32_t id = skill.id;
    hud->Practice(q, [hud, id] { hud->Request({HudRequest::Kind::LearnSkill, int(id)}); });
}

bool SkillWindow::OnNotice(const ui::Notice& notice) {
    // A right click on a learned skill's icon uses it, as its quick slot would (the user's word on the original,
    // 2026-11-13; the 1.188 client's handler not traced).
    if (notice.kind == ui::Notice::Kind::RightClicked && notice.source != nullptr && context_.hud != nullptr) {
        for (const SlotView& v : current_.slots) {
            if (notice.source != v.icon || v.learned == nullptr) continue;
            context_.hud->Request({HudRequest::Kind::UseSkill, int(v.learned->id)});
            return true;
        }
        return false;
    }
    if (notice.kind != ui::Notice::Kind::Clicked || notice.source == nullptr || context_.skills == nullptr) return false;
    if (notice.source->parent() == this && notice.control >= 300 && notice.control < 300 + int(tabButtons_.size())) {
        SelectTab(notice.control - 300);
        return true;
    }
    if (notice.source->parent() == this && notice.control >= 400 && notice.control < 400 + int(masteryButtons_.size())) {
        ShowMastery(masteryOfButton_[std::size_t(notice.control - 400)]);
        return true;
    }
    Widget* board = Child(10);
    if (withdrawal_ && board != nullptr && notice.source->parent() == board && notice.control == 14 && mastery_ != 0) {
        AskWithdraw(context_, nullptr, mastery_);
        return true;
    }
    if (withdrawal_) {
        for (const SlotView& v : current_.slots) {
            // A click on the learned skill (ours: the original's slot click in this mode not traced).
            if (notice.source == v.icon && v.learned != nullptr) {
                AskWithdraw(context_, v.learned, 0);
                return true;
            }
        }
    }
    if (board != nullptr && notice.source->parent() == board && notice.control == 14 && mastery_ != 0 && context_.hud != nullptr) {
        // The mastery's next level (its cost 0 from level 0).
        const assets::MasteryRow* m = context_.skills->Mastery(mastery_);
        const int level = MasteryLevel(mastery_);
        const std::uint32_t cost = context_.masteryCosts != nullptr ? game::MasteryCost(*context_.masteryCosts, level) : 0;
        SkillPracticeBox::Question q;
        const std::string name = m != nullptr ? context_.Text(m->nameKey) : std::string();
        q.icon = m != nullptr ? IconPath(m->iconPath) : std::string();
        q.name = name;
        q.mastery = name;
        q.isMastery = true;
        q.level = level + 1;
        q.description = m != nullptr ? context_.Text(m->descKey) : std::string();
        q.next = FormatText(context_.Text("UIIT_STT_SKILL_LEARN"), {name});
        q.skillPoints = cost;
        Hud* hud = context_.hud;
        const std::uint32_t id = mastery_;
        hud->Practice(q, [hud, id] { hud->Request({HudRequest::Kind::RaiseMastery, int(id)}); });
        return true;
    }
    for (const SlotView& v : current_.slots) {
        if (notice.source == v.button && notice.source->parent() == v.box && v.next != nullptr) {
            Ask(*v.next);
            return true;
        }
    }
    return false;
}

// --- The practice box -------------------------------------------------------------------------------

void SkillPracticeBox::Created(ui::Interface& ui) {
    ui::MainFrame::Created(ui);
    ui_ = &ui;
    // ginterface's 310 is short of its layout: the lower tile ends at 318, then the 16 of the frame's foot.
    rect.h = 334.0f;
    // The description's panel (GDR_SKLPB_FILL_COLOR, no texture of its own): black.
    if (ui::Widget* fill = Child(8)) fill->background = ui::Argb(0xFF000000);
    // The description (an edit in the layout): shown wrapped from its start, never typed in.
    if (auto* description = ui::As<ui::Edit>(Child(18))) {
        description->multiline = true;
        description->readOnly = true;
    }
}

void SkillPracticeBox::Ask(const Question& q, std::function<void()> confirmed) {
    confirmed_ = std::move(confirmed);
    // Its two looks and their titles: a skill's icon, name and level, or a
    // mastery's name in its box (which would cover the skill's name, under it in the layout).
    const auto show = [this](int id, bool on) {
        if (ui::Widget* w = Child(id)) w->visible = on;
    };
    for (int id : {11, 12, 13, 15}) show(id, !q.isMastery);
    for (int id : {16, 17}) show(id, q.isMastery);
    if (ui::Widget* title = Child(0)) {
        title->SetText(context_.Text(q.isMastery ? "UIIT_STT_CIRCULATION_PRACTICE_MASTERY_WND" : "UIIT_STT_CIRCULATION_PRACTICE_SKILL_WND"));
    }
    if (auto* slot = ui::As<ui::Slot>(Child(12))) {
        slot->icon = q.icon;
        slot->pickable = false;
    }
    if (ui::Widget* w = Child(13)) w->SetText(q.name);
    if (ui::Widget* w = Child(15)) w->SetText(std::to_string(q.level));
    if (ui::Widget* w = Child(17)) w->SetText(q.mastery);
    if (ui::Widget* w = Child(18)) w->SetText(q.description);
    if (ui::Widget* w = Child(21)) w->SetText(q.next);
    if (ui::Widget* w = Child(23)) w->SetText(std::to_string(q.skillPoints));
    // The skill points short: the box says so instead of sending (a message box of kind 9 in the original).
    if (ui::Widget* ok = Child(19)) ok->enabled = context_.state == nullptr || context_.state->self.skillPoints >= q.skillPoints;
}

void SkillPracticeBox::Answer(bool yes) {
    auto confirmed = std::move(confirmed_);
    confirmed_ = nullptr;
    const bool affordable = Child(19) == nullptr || Child(19)->enabled;
    // Hidden, not closed: the box is one of the HUD's windows made once at the start (window 31);
    // closed, it was gone and no skill could be raised again.
    if (context_.hud != nullptr) {
        context_.hud->Hide(hud::kPractice);
    } else if (ui_ != nullptr) {
        ui_->Close(this);
    }
    if (yes && affordable && confirmed) confirmed();
}

bool SkillPracticeBox::OnNotice(const ui::Notice& notice) {
    if (notice.kind != ui::Notice::Kind::Clicked || notice.source == nullptr || notice.source->parent() != this) return ui::MainFrame::OnNotice(notice);
    if (notice.control == 19) {
        Answer(true);
        return true;
    }
    if (notice.control == 20 || notice.control == 2) {
        Answer(false);
        return true;
    }
    return ui::MainFrame::OnNotice(notice);
}

std::uint32_t SkillWindow::SkillOf(const ui::Widget* w) const {
    for (const SlotView& v : current_.slots) {
        if (w == v.icon && v.learned != nullptr && v.learned->activity != 0) return v.learned->id;
    }
    return 0;
}

} // namespace sr::client
