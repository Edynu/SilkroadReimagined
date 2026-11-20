// The Battle Arena's windows of the 1.188 client, from their res_ui layouts:
// - CNIFBattleArenaRuleWnd (162, arena_rule_select.2dt, "Arena Registration"): the kinds offered, one row each (score
//   first), a row clicked turns to com_bar01select_ and shows its description;
//   Register asks "Do you want to enter [%s-%s]?" and Yes sends the request; Cancel and the frame's
//   close close it, and the guild window with it when it came from there.
// - CNIFBattleArenaGuildWnd (160, arena_guilduser_select.2dt, "Formation for participant"): the guild's members on the
//   left, the chosen on the right, 8 rows each; the arrows move the selected row across (at most 16 chosen); the counts
//   over the lists; Confirm hides it and opens the rule window in the guild mode.
// - CNIFBattleArenaScoreWnd (200, arena_game_score.2dt): team 1's score left of the red dragon, team 0's right of the
//   blue tiger (CIFNumberWnd digits gil_small_count_N, 12 x 24: team 1's leftwards from its control, team 0's
//   rightwards); at (W - 262, 258).
// - CNIFBattleArenaRankWnd (163, arena_game_rank.2dt, gil_list_frame): the top five's rows that have data: "%d.", the
//   team's icon (team 1 the dragon, else the tiger), the name, the points; at (W - 262, 288).
// - CNIFBattleArenaResultWnd (181, arena_game_result.2dt, "Battle Arena Result"): centred: the dragon's and
//   the tiger's team pictures, the outcome (gil_win / gil_lose / gil_draw), the two scores in gil_count_N
//   (21 x 31; team 1 left, team 0 right), "Arena Coin: %d" and "Skill exp: %d".

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <optional>

#include "gui/Hud.h"
#include "gui/HudWindows.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr const char* kRuleLayout = "res_ui\\arena_rule_select.2dt";
constexpr const char* kGuildLayout = "res_ui\\arena_guilduser_select.2dt";
constexpr const char* kScoreLayout = "res_ui\\arena_game_score.2dt";
constexpr const char* kRankLayout = "res_ui\\arena_game_rank.2dt";
constexpr const char* kResultLayout = "res_ui\\arena_game_result.2dt";
constexpr int kRuleWindow = 162, kGuildWindow = 160, kScoreWindow = 200, kRankWindow = 163, kResultWindow = 181;
constexpr const char* kSelectedBar = "interface\\ifcommon\\com_bar01select_";
constexpr const char* kDragon = "interface\\guild\\gil_red_dregon.ddj";
constexpr const char* kTiger = "interface\\guild\\gil_blue_tiger.ddj";

ui::Interface* Ui(const HudContext& c) { return c.hud != nullptr ? c.hud->interface() : nullptr; }

// A window by its id, shown or hidden (Interface::Window finds shown ones only).
ui::Widget* Any(const ui::Interface& ui, int id) {
    for (ui::Widget* w : ui.Windows()) {
        if (w->id() == id) return w;
    }
    return nullptr;
}

// A layout's records under the window: the list rows' bars (kind 15 in a slot) rows that take a click, the headers bars
// with their captions, the text boxes (kind 7) text boxes, the black squares (kind 3: com_blacksquare_'s corners and
// sides, no top or bottom pieces) stretched boxes black inside.
std::vector<ui::Widget*> BuildArena(ui::Interface& ui, ui::Widget& window, const std::vector<assets::Ui2dtRecord>& records) {
    std::map<int, std::uint32_t> kinds;
    for (const assets::Ui2dtRecord& r : records) kinds[r.id] = r.kind;
    std::vector<ui::Widget*> made = Build2dt(ui, window, records, [&](const assets::Ui2dtRecord& r, assets::UiLayoutNode& n) {
        if (r.kind == 15) {
            n.type = kinds[r.parents[0]] == 8 ? "CNIFGuildRow" : "CIFBarWnd";
            // The art names one of the bar's pieces (com_bar02_right.ddj, com_bar01_mid.ddj): their prefix.
            for (std::string_view piece : {"left.ddj", "mid.ddj", "right.ddj"}) {
                if (const std::size_t at = n.ddj.rfind(piece); at != std::string::npos && at + piece.size() == n.ddj.size()) n.ddj.resize(at);
            }
        }
        if (r.kind == 7) n.type = "CIFTextBox";
        if (r.kind == 3) n.type = "CIFStretchWnd";
    });
    for (std::size_t i = 1; i < records.size() && i < made.size(); ++i) {
        if (records[i].kind == 3 && made[i] != nullptr) made[i]->background = ui::Argb(0xFF000000);
    }
    return made;
}

std::string LineText(const HudContext& c, const game::ArenaView::Line& line) {
    std::vector<std::string> args;
    for (const auto& a : line.args) args.push_back(a.key ? c.Text(a.text) : a.text);
    std::string text = c.Text(line.key);
    const std::string_view f = text;
    std::string out;
    std::size_t next = 0;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%' || i + 1 >= f.size()) {
            out += f[i];
            continue;
        }
        if (f[i + 1] == '%') {
            out += '%';
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        while (j < f.size() && std::string_view("-+ #0123456789.lhI").find(f[j]) != std::string_view::npos) ++j;
        if (j >= f.size()) break;
        if (next < args.size()) out += args[next++];
        i = j;
    }
    return out;
}

// A number in digit pictures `art`N.ddj of w x h; leftwards (the
// units ending at the control's left edge) or rightwards (the first digit at it).
class ArenaNumber : public ui::Widget {
public:
    ArenaNumber(const assets::UiLayoutNode& node, std::string art, float w, float h, bool rightwards)
        : Widget(node), art_(std::move(art)), w_(w), h_(h), rightwards_(rightwards) {
        clickThrough = true;
    }
    std::uint32_t value = 0;
    void Draw(ui::Frame& frame) override {
        const std::string digits = std::to_string(value);
        const ui::Vec2 o = Origin();
        const std::size_t n = digits.size();
        for (std::size_t i = 0; i < n; ++i) {
            // Leftwards: picture i is the i-th digit from the end at x - (i + 1) w; rightwards: the i-th from the start.
            const char d = rightwards_ ? digits[i] : digits[n - 1 - i];
            const float x = rightwards_ ? o.x + w_ * float(i) : o.x - w_ * float(i + 1);
            frame.paint.Image(std::format("{}{}.ddj", art_, d), {x, o.y, w_, h_}, {0, 0}, {1, 1}, ui::WithAlpha(ui::kWhite, Opacity()));
        }
    }

private:
    std::string art_;
    float w_, h_;
    bool rightwards_;
};

ArenaNumber* AddNumber(ui::Widget* control, const char* art, float w, float h, bool rightwards) {
    if (control == nullptr) return nullptr;
    assets::UiLayoutNode n;
    n.type = "ArenaNumber";
    n.id = 1000 + control->id();
    auto number = std::make_unique<ArenaNumber>(n, art, w, h, rightwards);
    number->rect = {0.0f, 0.0f, 0.0f, 0.0f};
    return static_cast<ArenaNumber*>(control->Add(std::move(number)));
}

void SetPicture(ui::Widget* w, const std::string& path) {
    if (auto* s = dynamic_cast<ui::Static*>(w)) s->texture = path;
}

// A list row (a slot with its bar and texts): its bar's pieces by its state.
void Highlight(ui::Widget* slot, int barId, bool selected) {
    if (slot == nullptr) return;
    if (auto* bar = dynamic_cast<ui::Bar*>(slot->Child(barId))) bar->pieces = selected ? kSelectedBar : std::string();
}

// --- The rule window ---------------------------------------------------------------------------------------------

class RuleWindow : public ui::MainFrame {
public:
    RuleWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        ui_ = &ui;
        const auto* records = c_.layout2dt ? c_.layout2dt(kRuleLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        if (ui::Widget* title = Child(0)) title->SetText(c_.Text(records->front().textKey));
        const std::vector<ui::Widget*> made = BuildArena(ui, *this, *records);
        for (std::size_t i = 0; i < records->size(); ++i) {
            const int id = (*records)[i].id;
            if ((*records)[i].kind == 8 && id >= 10 && id <= 16) slots_[std::size_t(id - 10)] = made[i];
            if (id == 30) description_ = dynamic_cast<ui::TextBox*>(made[i]);
            if (id == 7 && description_ != nullptr) description_->bar = ui::As<ui::ScrollBar>(made[i]);
        }
    }

    // The kinds from their bits (nothing selected, the description empty), for `mode`; the guild's chosen members with it.
    void Set(std::uint8_t mode, std::uint16_t kinds, std::vector<std::uint32_t> members) {
        mode_ = mode;
        members_ = std::move(members);
        rows_ = game::ArenaView::KindRows(kinds);
        Select(-1);
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i] == nullptr) continue;
            if (ui::Widget* name = slots_[i]->Child(29)) name->SetText(i < rows_.size() ? c_.Text(std::string(rows_[i].name)) : std::string());
        }
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr) {
            // A row's bar: its slot's place in the list.
            if (ui::Widget* slot = n.source->parent(); slot != nullptr && slot != this) {
                for (std::size_t i = 0; i < slots_.size(); ++i) {
                    if (slots_[i] == slot && i < rows_.size()) {
                        Select(int(i));
                        return true;
                    }
                }
            }
            if (n.source->parent() == this && n.control == 8) {
                Register();
                return true;
            }
            if (n.source->parent() == this && n.control == 9) {
                Close();
                return true;
            }
        }
        // The frame's close: the guild window with it.
        const bool taken = ui::MainFrame::OnNotice(n);
        if (!visible) Close();
        return taken;
    }

    bool PickKind(std::uint8_t kind) {
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].kind == kind) {
                Select(int(i));
                return true;
            }
        }
        return false;
    }
    // Register with a row selected: the confirm box; its Yes sends.
    bool Register() {
        if (selected_ < 0 || std::size_t(selected_) >= rows_.size() || c_.hud == nullptr) return false;
        const std::uint8_t kind = rows_[std::size_t(selected_)].kind;
        const std::uint8_t mode = mode_;
        const std::vector<std::uint32_t> members = members_;
        HudContext* c = &c_;
        c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {LineText(c_, game::ArenaView::Confirmation(mode, kind))}, [c, mode, kind, members](bool yes) {
            if (!yes) return;
            if (c->eventRequest) c->eventRequest(proto::ArenaRegister::kId, proto::Encode(game::ArenaView::Register(mode, kind, members)));
            CloseArenaRegistration(*c);
        });
        return true;
    }
    void Close() { CloseArenaRegistration(c_); }

private:
    void Select(int i) {
        selected_ = i;
        for (std::size_t k = 0; k < slots_.size(); ++k) Highlight(slots_[k], 27, int(k) == i);
        if (description_ == nullptr) return;
        description_->ClearParagraphs();
        if (i >= 0 && std::size_t(i) < rows_.size()) {
            std::string text = c_.Text(std::string(rows_[std::size_t(i)].description));
            for (std::size_t at = text.find("\\n"); at != std::string::npos; at = text.find("\\n", at)) text.replace(at, 2, "\n");
            description_->AddParagraph(text, ui::kWhite);
        }
    }
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::array<ui::Widget*, 7> slots_{};
    ui::TextBox* description_ = nullptr;
    std::vector<game::ArenaView::KindRow> rows_;
    std::vector<std::uint32_t> members_;
    std::uint8_t mode_ = 0;
    int selected_ = -1;
};

// --- The guild window --------------------------------------------------------------------------------------------

class GuildPickWindow : public ui::MainFrame {
public:
    GuildPickWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        const auto* records = c_.layout2dt ? c_.layout2dt(kGuildLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        if (ui::Widget* title = Child(0)) title->SetText(c_.Text(records->front().textKey));
        const std::vector<ui::Widget*> made = BuildArena(ui, *this, *records);
        for (std::size_t i = 0; i < records->size(); ++i) {
            const int id = (*records)[i].id;
            if ((*records)[i].kind != 8) continue;
            if (id >= 100 && id < 108) left_[std::size_t(id - 100)] = made[i];
            if (id >= 200 && id < 208) right_[std::size_t(id - 200)] = made[i];
        }
        leftBar_ = ui::As<ui::ScrollBar>(Child(6));
        rightBar_ = ui::As<ui::ScrollBar>(Child(7));
    }

    // Every member of the player's guild on the left (level, name), none chosen (no filter).
    void Fill(std::uint16_t kinds) {
        kinds_ = kinds;
        members_.clear();
        chosen_.clear();
        if (c_.guild != nullptr) {
            for (const proto::GuildMemberRecord& m : c_.guild->Guild().members) members_.push_back({m.character, m.level, m.name});
        }
        pickedLeft_ = pickedRight_ = -1;
        Refresh();
    }

    void Update(ui::Frame& frame) override {
        ui::MainFrame::Update(frame);
        const int left = leftBar_ != nullptr ? leftBar_->position() : 0, right = rightBar_ != nullptr ? rightBar_->position() : 0;
        if (left != leftTop_ || right != rightTop_) {
            leftTop_ = left;
            rightTop_ = right;
            Refresh();
        }
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr) {
            ui::Widget* slot = n.source->parent();
            for (std::size_t i = 0; i < 8; ++i) {
                if (slot != nullptr && slot == left_[i]) return Pick(pickedLeft_, leftTop_ + int(i), members_.size());
                if (slot != nullptr && slot == right_[i]) return Pick(pickedRight_, rightTop_ + int(i), chosen_.size());
            }
            if (n.source->parent() == this) {
                switch (n.control) {
                case 10: MoveRight(); return true;
                case 11: MoveLeft(); return true;
                case 12: Confirm(); return true;
                case 13: CloseArenaRegistration(c_); return true;
                default: break;
                }
            }
        }
        return ui::MainFrame::OnNotice(n);
    }

    // Script steps: a member by name moved right; Confirm.
    bool ChooseName(const std::string& name) {
        for (std::size_t i = 0; i < members_.size(); ++i) {
            if (members_[i].name != name) continue;
            pickedLeft_ = int(i);
            return MoveRight();
        }
        return false;
    }
    void Confirm() {
        // Hidden, the rule window opened with the kinds' bits and the guild mode; no minimum checked.
        std::vector<std::uint32_t> ids;
        for (const Member& m : chosen_) ids.push_back(m.character);
        visible = false;
        OpenArenaRule(c_, std::uint8_t(game::arena::Mode::Guild), kinds_, std::move(ids));
    }
    std::size_t Chosen() const { return chosen_.size(); }

private:
    struct Member {
        std::uint32_t character = 0;
        std::uint8_t level = 0;
        std::string name;
    };
    bool Pick(int& picked, int index, std::size_t count) {
        if (index < 0 || std::size_t(index) >= count) return true;
        picked = index;
        Refresh();
        return true;
    }
    bool MoveRight() {
        // Refused with 16 chosen already (over 15).
        if (pickedLeft_ < 0 || std::size_t(pickedLeft_) >= members_.size() || chosen_.size() >= std::size_t(game::arena::kGuildMost)) return false;
        chosen_.push_back(members_[std::size_t(pickedLeft_)]);
        members_.erase(members_.begin() + pickedLeft_);
        pickedLeft_ = -1;
        Refresh();
        return true;
    }
    bool MoveLeft() {
        if (pickedRight_ < 0 || std::size_t(pickedRight_) >= chosen_.size()) return false;
        members_.push_back(chosen_[std::size_t(pickedRight_)]);
        chosen_.erase(chosen_.begin() + pickedRight_);
        pickedRight_ = -1;
        Refresh();
        return true;
    }
    void Refresh() {
        // The scroll bars' ranges (count - 8), the rows from their tops, the counts over the lists.
        if (leftBar_ != nullptr) leftBar_->SetRange(0, std::max(0, int(members_.size()) - 8));
        if (rightBar_ != nullptr) rightBar_->SetRange(0, std::max(0, int(chosen_.size()) - 8));
        leftTop_ = std::clamp(leftTop_, 0, std::max(0, int(members_.size()) - 8));
        rightTop_ = std::clamp(rightTop_, 0, std::max(0, int(chosen_.size()) - 8));
        const auto fill = [&](const std::array<ui::Widget*, 8>& slots, const std::vector<Member>& list, int top, int picked) {
            for (std::size_t i = 0; i < slots.size(); ++i) {
                ui::Widget* slot = slots[i];
                if (slot == nullptr) continue;
                const std::size_t at = std::size_t(top) + i;
                const Member* m = at < list.size() ? &list[at] : nullptr;
                if (ui::Widget* level = slot->Child(11)) level->SetText(m != nullptr ? std::to_string(m->level) : std::string());
                if (ui::Widget* name = slot->Child(12)) name->SetText(m != nullptr ? m->name : std::string());
                Highlight(slot, 10, m != nullptr && int(at) == picked);
            }
        };
        fill(left_, members_, leftTop_, pickedLeft_);
        fill(right_, chosen_, rightTop_, pickedRight_);
        const std::size_t all = members_.size() + chosen_.size();
        if (ui::Widget* w = Child(20)) w->SetText(FormatText(c_.Text("UIIT_STT_ARENA_CONSTITUTION_WINDOW_GUILDSMAN"), {std::to_string(members_.size()), std::to_string(all)}));
        if (ui::Widget* w = Child(21)) {
            w->SetText(FormatText(c_.Text("UIIT_STT_ARENA_CONSTITUTION_WINDOW_ARENA_ENTRY"), {std::to_string(chosen_.size()), std::to_string(game::arena::kGuildMost)}));
        }
    }
    HudContext& c_;
    std::array<ui::Widget*, 8> left_{}, right_{};
    ui::ScrollBar *leftBar_ = nullptr, *rightBar_ = nullptr;
    std::vector<Member> members_, chosen_;
    int pickedLeft_ = -1, pickedRight_ = -1, leftTop_ = 0, rightTop_ = 0;
    std::uint16_t kinds_ = 0;
};

// --- The score, rank and result windows ----------------------------------------------------------------------------

class ScoreWindow : public ui::Widget {
public:
    ScoreWindow(const assets::UiLayoutNode& node, HudContext& c) : Widget(node), c_(c) {}
    void Created(ui::Interface& ui) override {
        clickThrough = true;
        const auto* records = c_.layout2dt ? c_.layout2dt(kScoreLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        BuildArena(ui, *this, *records);
        ui::Widget* slot = Child(9);
        if (slot == nullptr) return;
        SetPicture(slot->Child(11), kDragon);
        SetPicture(slot->Child(12), "interface\\guild\\gil_small_count.ddj");
        SetPicture(slot->Child(13), kTiger);
        team1_ = AddNumber(slot->Child(10), "interface\\guild\\gil_small_count_", 12.0f, 24.0f, false);
        team0_ = AddNumber(slot->Child(14), "interface\\guild\\gil_small_count_", 12.0f, 24.0f, true);
    }
    void Update(ui::Frame&) override {
        if (c_.arena == nullptr) return;
        if (team0_ != nullptr) team0_->value = c_.arena->Score(0);
        if (team1_ != nullptr) team1_->value = c_.arena->Score(1);
    }

private:
    HudContext& c_;
    ArenaNumber *team0_ = nullptr, *team1_ = nullptr;
};

class RankWindow : public ui::Widget {
public:
    RankWindow(const assets::UiLayoutNode& node, HudContext& c) : Widget(node), c_(c) {}
    void Created(ui::Interface& ui) override {
        clickThrough = true;
        const auto* records = c_.layout2dt ? c_.layout2dt(kRankLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        art_ = records->front().art;
        BuildArena(ui, *this, *records);
    }
    void Draw(ui::Frame& frame) override {
        if (!art_.empty()) frame.paint.Image(art_, ScreenRect(), {0, 0}, {1, 1}, ui::WithAlpha(ui::kWhite, Opacity()));
    }
    void Update(ui::Frame&) override {
        if (c_.arena == nullptr) return;
        const auto& top = c_.arena->Top();
        for (int i = 0; i < game::ArenaView::kTopRows; ++i) {
            ui::Widget* row = Child(30 + i);
            if (row == nullptr) continue;
            const proto::ArenaScores::Row* r = std::size_t(i) < top.size() ? &top[std::size_t(i)] : nullptr;
            row->visible = r != nullptr; // the rows that have data
            if (r == nullptr) continue;
            if (ui::Widget* w = row->Child(100)) w->SetText(std::format("{}.", i + 1));
            SetPicture(row->Child(101), r->team == 1 ? kDragon : kTiger);
            if (ui::Widget* w = row->Child(102)) w->SetText(r->name);
            if (ui::Widget* w = row->Child(103)) w->SetText(std::to_string(r->points));
        }
    }

private:
    HudContext& c_;
    std::string art_;
};

class ResultWindow : public ui::MainFrame {
public:
    ResultWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}
    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        const auto* records = c_.layout2dt ? c_.layout2dt(kResultLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        if (ui::Widget* title = Child(0)) title->SetText(c_.Text(records->front().textKey));
        BuildArena(ui, *this, *records);
        SetPicture(Child(10), "interface\\guild\\gil_arena_dragon_team.ddj");
        SetPicture(Child(15), "interface\\guild\\gil_arena_tiger_team.ddj");
        SetPicture(Child(20), "interface\\guild\\gil_draw.ddj"); // at creation
        SetPicture(Child(60), "interface\\guild\\gil_count.ddj");
        team1_ = AddNumber(Child(40), "interface\\guild\\gil_count_", 21.0f, 31.0f, false);
        team0_ = AddNumber(Child(50), "interface\\guild\\gil_count_", 21.0f, 31.0f, true);
    }
    void Update(ui::Frame& frame) override {
        ui::MainFrame::Update(frame);
        if (c_.arena == nullptr || !c_.arena->Result()) return;
        const proto::ArenaResult& r = *c_.arena->Result();
        static constexpr const char* kOutcome[] = {"interface\\guild\\gil_win.ddj", "interface\\guild\\gil_lose.ddj", "interface\\guild\\gil_draw.ddj"};
        if (r.outcome >= 1 && r.outcome <= 3) SetPicture(Child(20), kOutcome[r.outcome - 1]);
        if (team0_ != nullptr) team0_->value = r.team0;
        if (team1_ != nullptr) team1_->value = r.team1;
        if (ui::Widget* w = Child(21)) w->SetText(FormatText(c_.Text("UIIT_MSG_ARENA_RESULT_WINDOW_HONORMEDAL"), {std::to_string(r.coins)}));
        if (ui::Widget* w = Child(22)) w->SetText(FormatText(c_.Text("UIIT_MSG_ARENA_RESULT_WINDOW_SKILLPOINT"), {std::to_string(r.sp)}));
    }
    bool OnNotice(const ui::Notice& n) override {
        // The frame's close: the window gone, the view's result with it.
        const bool closed = ui::MainFrame::OnNotice(n);
        if (!visible && c_.arena != nullptr) c_.arena->CloseResult();
        return closed;
    }

private:
    HudContext& c_;
    ArenaNumber *team0_ = nullptr, *team1_ = nullptr;
};

ui::Widget* OpenAt(HudContext& c, const char* layout, const char* type, int id, std::optional<ui::Vec2> at) {
    ui::Interface* ui = Ui(c);
    if (ui == nullptr) return nullptr;
    if (ui::Widget* w = Any(*ui, id)) {
        w->visible = true;
        return w;
    }
    auto node = RootNode2dt(c, layout, type, id);
    if (!node) return nullptr;
    if (at) {
        node->rect.x = at->x;
        node->rect.y = at->y;
    }
    return ui->Open(*node);
}

void CloseWindow(HudContext& c, int id) {
    ui::Interface* ui = Ui(c);
    if (ui == nullptr) return;
    if (ui::Widget* w = Any(*ui, id)) ui->Close(w);
}

} // namespace

std::string ArenaLineText(const HudContext& c, const game::ArenaView::Line& line) { return LineText(c, line); }

void RegisterArenaWindows(ui::Interface& ui, HudContext& context) {
    HudContext* c = &context;
    ui.Register("CNIFBattleArenaRuleWnd", [c](const assets::UiLayoutNode& n) { return std::make_unique<RuleWindow>(n, *c); });
    ui.Register("CNIFBattleArenaGuildWnd", [c](const assets::UiLayoutNode& n) { return std::make_unique<GuildPickWindow>(n, *c); });
    ui.Register("CNIFBattleArenaScoreWnd", [c](const assets::UiLayoutNode& n) { return std::make_unique<ScoreWindow>(n, *c); });
    ui.Register("CNIFBattleArenaRankWnd", [c](const assets::UiLayoutNode& n) { return std::make_unique<RankWindow>(n, *c); });
    ui.Register("CNIFBattleArenaResultWnd", [c](const assets::UiLayoutNode& n) { return std::make_unique<ResultWindow>(n, *c); });
}

void OpenArenaRule(HudContext& c, std::uint8_t mode, std::uint16_t kinds, std::vector<std::uint32_t> members) {
    if (auto* w = dynamic_cast<RuleWindow*>(OpenAt(c, kRuleLayout, "CNIFBattleArenaRuleWnd", kRuleWindow, std::nullopt))) {
        w->Set(mode, kinds, std::move(members));
        if (ui::Interface* ui = Ui(c)) ui->Raise(w);
    }
}

void OpenArenaGuild(HudContext& c, std::uint16_t kinds) {
    if (auto* w = dynamic_cast<GuildPickWindow*>(OpenAt(c, kGuildLayout, "CNIFBattleArenaGuildWnd", kGuildWindow, std::nullopt))) {
        w->Fill(kinds);
        if (ui::Interface* ui = Ui(c)) ui->Raise(w);
    }
}

void CloseArenaRegistration(HudContext& c) {
    CloseWindow(c, kRuleWindow);
    CloseWindow(c, kGuildWindow);
}

bool ArenaRuleShown(const HudContext& c) {
    ui::Interface* ui = Ui(c);
    ui::Widget* w = ui != nullptr ? ui->Window(kRuleWindow) : nullptr;
    return w != nullptr && w->visible;
}

bool ArenaRegisterKind(HudContext& c, std::uint8_t kind) {
    ui::Interface* ui = Ui(c);
    auto* w = ui != nullptr ? dynamic_cast<RuleWindow*>(ui->Window(kRuleWindow)) : nullptr;
    return w != nullptr && w->visible && w->PickKind(kind) && w->Register();
}

bool ArenaPickKind(HudContext& c, std::uint8_t kind) {
    ui::Interface* ui = Ui(c);
    auto* w = ui != nullptr ? dynamic_cast<RuleWindow*>(ui->Window(kRuleWindow)) : nullptr;
    return w != nullptr && w->PickKind(kind);
}

bool ArenaGuildChoose(HudContext& c, const std::vector<std::string>& names, bool confirm) {
    ui::Interface* ui = Ui(c);
    auto* w = ui != nullptr ? dynamic_cast<GuildPickWindow*>(ui->Window(kGuildWindow)) : nullptr;
    if (w == nullptr || !w->visible) return false;
    bool all = true;
    for (const std::string& name : names) all = w->ChooseName(name) && all;
    if (confirm) w->Confirm();
    return all;
}

void ArenaWindows(HudContext& c) {
    ui::Interface* ui = Ui(c);
    if (ui == nullptr || c.arena == nullptr || c.hud == nullptr) return;
    const game::ArenaView& v = *c.arena;
    const ui::Vec2 screen = c.hud->Screen();
    // The score and rank windows live with the arena state (made by 4 or a late clock, destroyed by 5), hidden until
    // shown; at the screen's right.
    const auto keep = [&](bool made, bool shown, const char* layout, const char* type, int id, ui::Vec2 at) {
        ui::Widget* w = Any(*ui, id);
        if (!made) {
            if (w != nullptr) ui->Close(w);
            return;
        }
        if (w == nullptr) w = OpenAt(c, layout, type, id, at);
        if (w == nullptr) return;
        w->rect.x = at.x;
        w->rect.y = at.y;
        w->visible = shown;
    };
    keep(v.InArena(), v.ScoreShown(), kScoreLayout, "CNIFBattleArenaScoreWnd", kScoreWindow, {screen.x - 262.0f, 258.0f});
    keep(v.RankMade(), v.RankShown(), kRankLayout, "CNIFBattleArenaRankWnd", kRankWindow, {screen.x - 262.0f, 288.0f});
    // The result: centred (x = W / 2 - 260, y = H / 2 - 133), until closed or the stop.
    ui::Widget* result = Any(*ui, kResultWindow);
    if (v.Result()) {
        if (result == nullptr) {
            result = OpenAt(c, kResultLayout, "CNIFBattleArenaResultWnd", kResultWindow,
                            ui::Vec2{std::floor(screen.x / 2.0f) - 260.0f, std::floor(screen.y / 2.0f) - 133.0f});
            if (result != nullptr) ui->Raise(result);
        }
    } else if (result != nullptr) {
        ui->Close(result);
    }
}

bool CloseArenaResult(HudContext& c) {
    if (c.arena == nullptr || !c.arena->Result()) return false;
    c.arena->CloseResult();
    CloseWindow(c, kResultWindow);
    return true;
}

} // namespace sr::client
