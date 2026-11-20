// The guild master's two windows, from the 1.188 client's layouts:
// the leader transfer window (CIFGuildMasterLeave, ginterface.txt 58, ifguildmasterleave.txt) and
// the election's (CIFGuildMasterElection, 57, ifguildmasterelection.txt). Each lists seven rows at
// a time in its ListWnd section's bars, each row its slot layout (ifguildmasterleaveslot.txt: a check
// box and the member's name; ifguildmasterelectionslot.txt: the box, the candidate and its votes),
// scrolled by the list's bar; one row checked at a time; Confirm and Cancel.
//
// The transfer: the members but the master; Confirm asks "Transfer leadership to the guild member",
// then the hand-over goes with the guild NPC (the guild NPC's row "Transfer leadership"). The
// election: the candidates and their ballots, the time left; Confirm casts the ballot at the NPC.
//
// And the guild's promotion (CIFGuildLevelUp, ginterface.txt 49, ifguildlevelup.txt; the NPC's row
// "Promote the guild."): the guild's mark, name and level, what the next level asks, the question.

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/game/Guild.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr int kConfirm = 11, kCancel = 12, kList = 41, kListBar = 10, kFirstBar = 31, kBars = 7;
constexpr int kSlotCheck = 10, kSlotName = 11, kSlotVotes = 12, kTimeLeft = 14;
constexpr int kTransferWindow = 58, kElectionWindow = 57, kLevelUpWindow = 49;

void AddSection(ui::Interface& ui, ui::Widget& to, const char* layout, const char* section) {
    const assets::UiLayout* l = ui.Layout(layout);
    if (l == nullptr) return;
    for (const auto& s : l->sections) {
        if (s.name != section) continue;
        for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) to.Add(ui.Make(*it));
    }
}

class MasterListWindow : public ui::MainFrame {
public:
    struct Row {
        std::uint32_t character = 0;
        std::string name;
        std::string votes; // the election's column (empty in the transfer)
    };

    MasterListWindow(const assets::UiLayoutNode& node, HudContext& c, const char* layout, const char* slotLayout, const char* title)
        : MainFrame(node), c_(c), layout_(layout), slotLayout_(slotLayout), title_(title) {}

    void Created(ui::Interface& ui) override {
        SetText(c_.text ? c_.text(title_) : std::string(title_));
        MainFrame::Created(ui);
        AddSection(ui, *this, layout_, "ListWnd");
        for (int i = 0; i < kBars; ++i) {
            ui::Widget* bar = Child(kFirstBar + i);
            if (bar == nullptr) continue;
            AddSection(ui, *bar, slotLayout_, "Create");
            if (auto* box = ui::As<ui::CheckBox>(bar->Child(kSlotCheck))) box->checked = false;
            bars_.push_back(bar);
        }
    }

    void Update(ui::Frame& frame) override {
        MainFrame::Update(frame);
        rows_ = Rows();
        const int most = std::max(0, int(rows_.size()) - int(bars_.size()));
        scroll_ = std::clamp(scroll_, 0, most);
        if (ui::Widget* list = Child(kList)) {
            if (auto* bar = dynamic_cast<ui::ScrollBar*>(list->Child(kListBar))) bar->SetRange(0, most);
        }
        for (std::size_t i = 0; i < bars_.size(); ++i) {
            const std::size_t at = std::size_t(scroll_) + i;
            const Row* row = at < rows_.size() ? &rows_[at] : nullptr;
            for (int id : {kSlotCheck, kSlotName, kSlotVotes}) {
                if (ui::Widget* w = bars_[i]->Child(id)) w->visible = row != nullptr;
            }
            if (row == nullptr) continue;
            if (ui::Widget* name = bars_[i]->Child(kSlotName); name != nullptr && name->text() != row->name) name->SetText(row->name);
            if (ui::Widget* votes = bars_[i]->Child(kSlotVotes); votes != nullptr && votes->text() != row->votes) votes->SetText(row->votes);
            if (auto* box = ui::As<ui::CheckBox>(bars_[i]->Child(kSlotCheck))) box->checked = row->character == selected_;
        }
        Refresh();
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::ScrollChanged) {
            scroll_ = n.value;
            return true;
        }
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr) return MainFrame::OnNotice(n);
        for (std::size_t i = 0; i < bars_.size(); ++i) {
            if (n.source->parent() != bars_[i]) continue;
            const std::size_t at = std::size_t(scroll_) + i;
            if (at < rows_.size()) selected_ = rows_[at].character;
            return true;
        }
        if (n.source->parent() == this && n.control == kConfirm) {
            Confirm();
            return true;
        }
        if (n.source->parent() == this && n.control == kCancel) {
            visible = false;
            return true;
        }
        return MainFrame::OnNotice(n);
    }

    // The test options' way: the row of that name checked.
    bool Pick(const std::string& name) {
        for (const Row& r : Rows()) {
            if (r.name == name) {
                selected_ = r.character;
                return true;
            }
        }
        return false;
    }
    virtual void Confirm() = 0;

protected:
    virtual std::vector<Row> Rows() const = 0;
    virtual void Refresh() {}
    HudContext& c_;
    std::uint32_t selected_ = 0;

private:
    const char* layout_;
    const char* slotLayout_;
    const char* title_;
    std::vector<ui::Widget*> bars_;
    std::vector<Row> rows_;
    int scroll_ = 0;
};

class TransferWindow : public MasterListWindow {
public:
    TransferWindow(const assets::UiLayoutNode& node, HudContext& c)
        : MasterListWindow(node, c, "resinfo\\ifguildmasterleave.txt", "resinfo\\ifguildmasterleaveslot.txt", "UIIT_STT_MLEAVE_WINDOWS") {}

    void Confirm() override {
        if (selected_ == 0 || c_.hud == nullptr) return;
        const game::proto::GuildMemberRecord* m = c_.guild != nullptr ? c_.guild->Member(selected_) : nullptr;
        if (m == nullptr) return;
        HudContext* ctx = &c_;
        // "Transfer leadership to the guild member", then the hand-over at the NPC.
        c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {c_.text("UIIT_MSG_MLEAVE_CONFIRM"), m->name}, [ctx, to = selected_](bool yes) {
            if (yes && ctx->guildRequest) ctx->guildRequest(proto::GuildMasterTransfer::kId, proto::Encode(proto::GuildMasterTransfer{ctx->guildNpc, to}));
        });
        visible = false;
    }

private:
    std::vector<Row> Rows() const override {
        std::vector<Row> rows;
        if (c_.guild == nullptr) return rows;
        for (const auto& m : c_.guild->Guild().members) {
            if (m.memberClass != game::kGuildMaster) rows.push_back({m.character, m.name, {}});
        }
        return rows;
    }
};

class ElectionWindow : public MasterListWindow {
public:
    ElectionWindow(const assets::UiLayoutNode& node, HudContext& c)
        : MasterListWindow(node, c, "resinfo\\ifguildmasterelection.txt", "resinfo\\ifguildmasterelectionslot.txt", "UIIT_STT_MRELEASE_VOTESTATE") {}

    void Confirm() override {
        if (selected_ == 0 || !c_.guildRequest) return;
        c_.guildRequest(proto::GuildElectionVote::kId, proto::Encode(proto::GuildElectionVote{c_.guildNpc, selected_}));
    }

private:
    std::vector<Row> Rows() const override {
        std::vector<Row> rows;
        if (c_.guild == nullptr) return rows;
        for (const auto& candidate : c_.guild->Election().candidates) {
            rows.push_back({candidate.character, candidate.name, std::to_string(candidate.votes)});
        }
        return rows;
    }
    void Refresh() override {
        if (c_.guild == nullptr) return;
        const proto::GuildElection& e = c_.guild->Election();
        if (c_.guild->Revision() != seen_) {
            seen_ = c_.guild->Revision();
            told_ = c_.time;
            if (e.ballot != 0) selected_ = e.ballot;
        }
        if (e.active == 0) {
            visible = false;
            return;
        }
        // The time left as told, counted down here: minutes and seconds.
        const int left = std::max(0, int(e.secondsLeft) - int(c_.time - told_));
        if (ui::Widget* time = Child(kTimeLeft)) {
            const std::string text = std::format("{:02}:{:02}", left / 60, left % 60);
            if (time->text() != text) time->SetText(text);
        }
    }
    std::uint64_t seen_ = ~0ull;
    double told_ = 0.0;
};

// filled from the guild when shown; OK asks the promotion at the NPC, the server checks.
class LevelUpWindow : public ui::FrameBorder {
public:
    LevelUpWindow(const assets::UiLayoutNode& node, HudContext& c) : FrameBorder(node), c_(c) {}

    void Update(ui::Frame& frame) override {
        FrameBorder::Update(frame);
        if (c_.guild == nullptr || !c_.text) return;
        const auto& g = c_.guild->Guild();
        const int level = std::clamp<int>(g.level, 0, game::kGuildMaxLevel);
        const int next = std::min(level + 1, game::kGuildMaxLevel); // never past the last
        Put(kName, g.name);
        Put(kLevel, FormatText(c_.text("UIIT_STT_LV_LEVEL"), {std::to_string(level)}));
        Put(kCondition, FormatText(c_.text("UIIT_MSG_GUILD_LEVEL_UP_CONDITION"), {std::to_string(next)}));
        Put(kNeedPoints, FormatText("%s :  %d GP", {c_.text("UIIT_STT_NEED_GP"), std::to_string(game::kGuildLevelUpPoints[std::size_t(level)])}));
        Put(kNeedGold, FormatText("%s :  %d %s", {c_.text("UIIT_STT_CIRCULATION_NEEDMONEY"), std::to_string(game::kGuildLevelUpGold[std::size_t(level)]), c_.text("UIIT_STT_GOLD")}));
        Put(kQuestion, FormatText(c_.text("UIIT_MSG_QUESTION_GUILD_LEVEL_UP"), {g.name, std::to_string(next)}));
        // The mark left of the centred name, the level right of it.
        ui::Widget* name = Child(kName);
        if (name == nullptr) return;
        const float half = frame.paint.TextWidth(g.name, name->node().fontIndex) / 2.0f;
        const float centre = name->rect.x + name->rect.w / 2.0f;
        if (auto* mark = dynamic_cast<ui::Static*>(Child(kMark))) {
            mark->texture = "interface\\guild\\gil_mark01.ddj";
            mark->rect.x = centre - half - 18.0f;
            mark->rect.y = name->rect.y - 2.0f;
        }
        if (ui::Widget* lv = Child(kLevel)) lv->rect.x = centre + half + 20.0f;
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr && n.source->parent() == this && (n.control == kOk || n.control == kCancel)) {
            Press(n.control == kOk);
            return true;
        }
        return FrameBorder::OnNotice(n);
    }

    void Press(bool ok) {
        visible = false;
        if (ok && c_.guildRequest) c_.guildRequest(proto::GuildLevelUp::kId, proto::Encode(proto::GuildLevelUp{c_.guildNpc}));
    }

private:
    static constexpr int kOk = 2, kCancel = 3, kMark = 4, kName = 5, kLevel = 6, kCondition = 7, kNeedPoints = 8, kNeedGold = 9, kQuestion = 10;

    void Put(int id, const std::string& text) {
        if (ui::Widget* w = Child(id); w != nullptr && w->text() != text) w->SetText(text);
    }

    HudContext& c_;
};

MasterListWindow* Find(HudContext& c, int id) {
    return c.hud != nullptr ? dynamic_cast<MasterListWindow*>(c.hud->Show(id)) : nullptr;
}

} // namespace

void RegisterGuildMasterWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CIFGuildMasterLeave", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<TransferWindow>(n, *c); });
    ui.Register("CIFGuildMasterElection", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<ElectionWindow>(n, *c); });
    ui.Register("CIFGuildLevelUp", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<LevelUpWindow>(n, *c); });
}

void OpenGuildMasterTransfer(HudContext& c) { Find(c, kTransferWindow); }
void OpenGuildElection(HudContext& c) { Find(c, kElectionWindow); }
void OpenGuildLevelUp(HudContext& c) {
    if (c.hud != nullptr) c.hud->Show(kLevelUpWindow);
}

bool PressGuildLevelUp(HudContext& c, bool ok) {
    if (c.hud == nullptr || !c.hud->Shown(kLevelUpWindow)) return false;
    auto* window = dynamic_cast<LevelUpWindow*>(c.hud->Show(kLevelUpWindow));
    if (window != nullptr) window->Press(ok);
    return window != nullptr;
}

bool PickGuildMasterRow(HudContext& c, const std::string& name, bool confirm) {
    if (c.hud == nullptr) return false;
    for (int id : {kTransferWindow, kElectionWindow}) {
        if (!c.hud->Shown(id)) continue;
        auto* window = dynamic_cast<MasterListWindow*>(c.hud->Show(id));
        if (window == nullptr || !window->Pick(name)) continue;
        if (confirm) window->Confirm();
        return true;
    }
    return false;
}

} // namespace sr::client
