// The fortress war's official window on the client, from the 1.188
// client's layouts: CIFFortressWarApplyWnd (ginterface.txt 303, "Fortress clerk - Fortress war",
// iffortresswarapplywnd.txt): the application period and the war's date (statics 504 and 505, "%s :
// %s" with UIIT_STT_SCHEDULE_PERIOD), the heads "Fortress name", "Occupying guild",
// "Application", and a row per fortress the official serves (iffortresswarapplywndslot.txt: the
// fortress's name, its occupying guild, the button). The button by the player's guild:
// its application there: "Cancellation of siege" / "Cancellation of defending"; else, in the owner's
// union, "Application for defending"; else "Application for siege". Each asks first
// (UIIT_MSG_FORT_OFFICIAL_*_WINDOW, the fee in the siege's), then the request goes with the official.
// The rows' spacing is ours (the scroll manager's rows are made in code, not traced).

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <format>
#include <string>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/game/Fortress.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr int kApplyWindow = 303;
constexpr const char* kLayout = "resinfo\\iffortresswarapplywnd.txt";
constexpr const char* kSlotLayout = "resinfo\\iffortresswarapplywndslot.txt";
constexpr int kApplyPeriod = 504, kWarDate = 505;
constexpr int kRowName = 600, kRowGuild = 601, kRowButton = 602;
constexpr int kRowBase = 700; // ours: the rows' ids
constexpr float kRowsTop = 183.0f, kRowHeight = 34.0f;

void AddSection(ui::Interface& ui, ui::Widget& to, const char* layout, const char* section) {
    const assets::UiLayout* l = ui.Layout(layout);
    if (l == nullptr) return;
    for (const auto& s : l->sections) {
        if (s.name != section) continue;
        for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) to.Add(ui.Make(*it));
    }
}

// "MM-DD hh:mm ~ MM-DD hh:mm" (UIIT_STT_SCHEDULE_PERIOD) of two Unix times in local time; empty for none.
std::string PeriodText(const HudContext& c, std::int64_t start, std::int64_t end) {
    if (start == 0) return "-";
    const auto local = [](std::int64_t t) {
        const std::time_t tt = std::time_t(t);
        std::tm out{};
#ifdef _WIN32
        localtime_s(&out, &tt);
#else
        localtime_r(&tt, &out);
#endif
        return out;
    };
    const std::tm a = local(start), b = local(end);
    const auto two = [](int v) { return std::format("{:02}", v); };
    return FormatText(c.Text("UIIT_STT_SCHEDULE_PERIOD"),
                      {two(a.tm_mon + 1), two(a.tm_mday), two(a.tm_hour), two(a.tm_min), two(b.tm_mon + 1), two(b.tm_mday), two(b.tm_hour), two(b.tm_min)});
}

std::string GoldText(std::uint32_t gold) {
    std::string digits = std::to_string(gold), out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

// The original's help over a control (set by its window's code, 0x653DC0) while the mouse is on its place: a control that
// takes the mouse or a decoration of this window (nothing hovered then).
void ShowHelp(ui::Frame& frame, ui::Widget& window, std::initializer_list<std::pair<int, const char*>> helps, const HudContext& c) {
    bool over = frame.pointer.hovered == nullptr && window.ScreenRect().Contains(frame.input.mouse());
    for (const ui::Widget* p = frame.pointer.hovered; p != nullptr && !over; p = p->parent()) over = p == &window;
    if (!over) return;
    for (const auto& [id, key] : helps) {
        ui::Widget* w = window.Child(id);
        if (w != nullptr && w->visible && w->ScreenRect().Contains(frame.input.mouse())) {
            frame.interface.ShowTooltip(c.Text(key), w->ScreenRect());
            return;
        }
    }
}

class ApplyWindow : public ui::MainFrame {
public:
    ApplyWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
        // Its layout's first section is built with it (the class's own layout); the heads and the
        // rows' place are the second.
        AddSection(ui, *this, kLayout, "FortressInfoSolt");
        if (ui::Widget* shape = Child(514)) shape->SetText({}); // its layout's "0" is the code's to fill (not traced)
    }

    void Update(ui::Frame& frame) override {
        // The heads' and the information's help (the original's UIIT_MSG_TP_FORT_WARAPPLY_*).
        ShowHelp(frame, *this, {{0x1FF, "UIIT_MSG_TP_FORT_WARAPPLY_FORTRESS"}, {0x200, "UIIT_MSG_TP_FORT_WARAPPLY_GUILD"},
                                {0x201, "UIIT_MSG_TP_FORT_WARAPPLY_APPLICATION"}, {0x1F6, "UIIT_MSG_TP_FORT_WARAPPLY_INFO"}}, c_);
        if (c_.fortress == nullptr || c_.fortress->Revision() == seen_) return;
        seen_ = c_.fortress->Revision();
        Fill();
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr && n.control == kRowButton && n.source->parent() != nullptr) {
            const int row = n.source->parent()->id() - kRowBase;
            if (row >= 0 && c_.fortress != nullptr && std::size_t(row) < c_.fortress->ApplyInfo().rows.size()) {
                Press(std::size_t(row));
                return true;
            }
        }
        return MainFrame::OnNotice(n);
    }

    // A row's button pressed (the test options' way too): its question, then the request.
    void Press(std::size_t row) {
        if (c_.fortress == nullptr || c_.hud == nullptr) return;
        const proto::FortressApplyInfo& info = c_.fortress->ApplyInfo();
        if (row >= info.rows.size()) return;
        const proto::FortressApplyRow r = info.rows[row];
        const std::string name = c_.Text(r.nameKey);
        const std::uint32_t npc = info.npc;
        HudContext* ctx = &c_;
        if (info.applied == r.fortress) {
            const bool defend = info.side == proto::FortressSide::Defend;
            const std::string key = defend ? "UIIT_MSG_FORT_OFFICIAL_UNIONAPPLY_CANCEL_WINDOW" : "UIIT_MSG_FORT_OFFICIAL_WARAPPLY_CANCEL_WINDOW";
            c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {FormatText(c_.Text(key), {name})}, [ctx, npc, r](bool yes) {
                if (yes && ctx->fortressRequest) ctx->fortressRequest(proto::FortressCancel::kId, proto::Encode(proto::FortressCancel{npc, r.fortress}));
            });
            return;
        }
        const proto::FortressSide side = r.defend ? proto::FortressSide::Defend : proto::FortressSide::Attack;
        const std::string question = r.defend ? FormatText(c_.Text("UIIT_MSG_FORT_OFFICIAL_UNIONAPPLY_WINDOW"), {name})
                                              : FormatText(c_.Text("UIIT_MSG_FORT_OFFICIAL_WARAPPLY_WINDOW"), {name, GoldText(r.fee)});
        c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {question}, [ctx, npc, r, side](bool yes) {
            if (yes && ctx->fortressRequest) ctx->fortressRequest(proto::FortressApply::kId, proto::Encode(proto::FortressApply{npc, r.fortress, side}));
        });
    }

private:
    void Fill() {
        const proto::FortressApplyInfo& info = c_.fortress->ApplyInfo();
        if (ui::Widget* w = Child(kApplyPeriod)) {
            w->SetText(c_.Text("UIIT_STT_FORT_OFFICAL_TEXT2") + " : " + PeriodText(c_, info.requestStart, info.requestEnd));
        }
        if (ui::Widget* w = Child(kWarDate)) w->SetText(c_.Text("UIIT_STT_FORT_OFFICAL_TEXT1") + " : " + PeriodText(c_, info.warStart, info.warEnd));
        for (std::size_t i = 0; i < 8; ++i) {
            ui::Widget* row = Child(kRowBase + int(i));
            if (i >= info.rows.size()) {
                if (row != nullptr) row->visible = false;
                continue;
            }
            if (row == nullptr && ui_ != nullptr) row = MakeRow(int(i));
            if (row == nullptr) continue;
            row->visible = true;
            const proto::FortressApplyRow& r = info.rows[i];
            if (ui::Widget* w = row->Child(kRowName)) w->SetText(c_.Text(r.nameKey));
            if (ui::Widget* w = row->Child(kRowGuild)) w->SetText(r.ownerName);
            const char* key = info.applied == r.fortress
                                  ? (info.side == proto::FortressSide::Defend ? "UIIT_CTL_FORT_OFFICAL_UNIONAPPLY_CANCEL" : "UIIT_CTL_FORT_OFFICAL_OCCUPYAPPLY_CANCEL")
                                  : (r.defend ? "UIIT_CTL_FORT_OFFICAL_UNIONAPPLY" : "UIIT_CTL_FORT_OFFICAL_OCCUPYAPPLY");
            if (ui::Widget* w = row->Child(kRowButton)) w->SetText(c_.Text(key));
        }
    }
    ui::Widget* MakeRow(int i) {
        assets::UiLayoutNode node;
        node.type = "CIFStatic";
        node.id = kRowBase + i;
        node.rect = {26.0f, kRowsTop + float(i) * kRowHeight, 397.0f, kRowHeight};
        auto made = ui_->Make(node);
        ui::Widget* row = made.get();
        AddSection(*ui_, *row, kSlotLayout, "Create");
        // Ours: the button as wide as its words (the layout gives it the texture's size).
        if (ui::Widget* button = row->Child(kRowButton)) button->rect = {276.0f, 5.0f, 104.0f, 24.0f};
        Add(std::move(made));
        return row;
    }

    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::uint64_t seen_ = ~0ull;
};

// --- The fortress manager's tax -----------------------------------------------------

constexpr int kTaxWindow = 300; // ginterface.txt TaxManagement: CIFTaxManagement (iftaxmanagement.txt), "Tax management"
constexpr const char* kTaxLayout = "resinfo\\iftaxmanagement.txt";
constexpr int kTaxFortressName = 27, kTaxTargetFirst = 30, kTaxSlider = 80, kTaxRate = 52, kTaxPercent = 53, kTaxMarkFirst = 54;
constexpr int kTaxGathered = 67, kTaxChange = 70, kTaxLevy = 71;
// MsgBoxTaxModify: 104 the question, 105 the current rate, 106 the new one, 101 Confirm, 100 Cancel; MsgBoxTaxLevy: 94 the
// amount, 96 Confirm, 95 Cancel (ifmessagebox.txt).
constexpr int kModifyMain = 104, kModifyCurrent = 105, kModifyNew = 106, kModifyConfirm = 101, kModifyCancel = 100;
constexpr int kLevyEdit = 94, kLevyConfirm = 96, kLevyCancel = 95;

std::string FortressNameOf(const HudContext& c, std::uint32_t fortress) {
    const proto::FortressRecord* f = c.fortress != nullptr ? c.fortress->Fortress(fortress) : nullptr;
    return f != nullptr ? c.Text(f->nameKey) : std::string();
}

void PostLine(HudContext& c, const std::string& text) {
    if (c.state != nullptr && !text.empty()) c.state->Post(game::GameMessage::Kind::General, text, c.time);
}

// A tax box (MsgBoxTaxModify, MsgBoxTaxLevy) over the message box's frame: its section's controls, Confirm and Cancel.
class TaxBox : public ui::MainFrame {
public:
    TaxBox(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
        // The message box's own background first (ifmessagebox.txt's Create section, com_bg_tile_b.ddj), sized to
        // this box as the money box's is, then the section's parts over it.
        if (const assets::UiLayout* layout = ui.Layout("resinfo\\ifmessagebox.txt")) {
            for (const auto& section : layout->sections) {
                if (section.name != "Create") continue;
                for (auto it = section.nodes.rbegin(); it != section.nodes.rend(); ++it) {
                    assets::UiLayoutNode n = *it;
                    n.rect = {16.0f, 40.0f, rect.w - 32.0f, rect.h - 56.0f};
                    Add(ui.Make(n));
                }
            }
        }
        AddSection(ui, *this, "resinfo\\ifmessagebox.txt", node().subSection.c_str());
        if (auto* edit = ui::As<ui::Edit>(Child(kLevyEdit))) {
            edit->numeric = true;
            edit->limit = 19;
            edit->passKeys = true; // Enter: Confirm; Escape: Cancel
        }
    }

    // The new rate's question: "Do you want to adjust tax rate of [%s]?", the current and the new rate.
    void AskRate(std::uint32_t npc, std::uint32_t fortress, int current, int rate) {
        npc_ = npc;
        fortress_ = fortress;
        current_ = current;
        rate_ = rate;
        if (ui::Widget* w = Child(kModifyMain)) w->SetText(FormatText(c_.Text("UIIT_MSG_FORT_MANAGER_TAXCHANGE_WINDOW1"), {FortressNameOf(c_, fortress)}));
        if (ui::Widget* w = Child(kModifyCurrent)) w->SetText(FormatText(c_.Text("UIIT_MSG_FORT_MANAGER_TAXCHANGE_WINDOW2"), {std::to_string(current)}));
        if (ui::Widget* w = Child(kModifyNew)) w->SetText(FormatText(c_.Text("UIIT_MSG_FORT_MANAGER_TAXCHANGE_WINDOW3"), {std::to_string(rate)}));
    }
    // The levy: the number box holding the whole tax gathered, at most that (inferred limit).
    void AskLevy(std::uint32_t npc, std::uint32_t fortress, std::int64_t gathered) {
        npc_ = npc;
        fortress_ = fortress;
        gathered_ = gathered;
        if (auto* edit = ui::As<ui::Edit>(Child(kLevyEdit))) {
            edit->SetValue(std::to_string(gathered));
            if (ui_ != nullptr) ui_->Focus(edit);
        }
    }

    bool OnNotice(const ui::Notice& n) override {
        const bool mine = n.source != nullptr && n.source->parent() == this;
        const bool confirm = mine && ((n.kind == ui::Notice::Kind::Clicked && (n.control == kModifyConfirm || n.control == kLevyConfirm)) ||
                                      n.kind == ui::Notice::Kind::Submitted);
        const bool cancel = mine && ((n.kind == ui::Notice::Kind::Clicked && (n.control == kModifyCancel || n.control == kLevyCancel || n.control == 2)) ||
                                     n.kind == ui::Notice::Kind::Cancelled);
        if (!confirm && !cancel) return MainFrame::OnNotice(n);
        if (confirm) Confirm();
        if (ui_ != nullptr) ui_->Close(this);
        return true;
    }

    void Confirm() {
        if (!c_.fortressRequest) return;
        if (node().subSection == "MsgBoxTaxModify") {
            // The same rate: the client's own refusal, nothing sent.
            if (rate_ == current_) return PostLine(c_, c_.Text("UIIT_MSG_FORT_MANAGER_TAX_ERROR_03"));
            c_.fortressRequest(proto::FortressTaxRate::kId, proto::Encode(proto::FortressTaxRate{npc_, std::int16_t(rate_)}));
            return;
        }
        const auto* edit = ui::As<ui::Edit>(Child(kLevyEdit));
        const std::int64_t amount = edit != nullptr ? std::clamp<std::int64_t>(std::strtoll(edit->text().c_str(), nullptr, 10), 0, gathered_) : 0;
        if (amount <= 0) return; // nothing sent for 0
        c_.fortressRequest(proto::FortressTaxLevy::kId, proto::Encode(proto::FortressTaxLevy{npc_, amount}));
    }

private:
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::uint32_t npc_ = 0, fortress_ = 0;
    int current_ = 0, rate_ = 0;
    std::int64_t gathered_ = 0;
};

// CIFTaxManagement (its map): the fortress (27), the NPCs it taxes as
// disabled check boxes (30..36: smith, armour, grocer, druggist, gate, specialty, warp), the rate's slider (80, 0..40 =
// -20..+20) with its number (52, 53 "%") and marks (54..58), the tax gathered (67), "Change" (70) and "Levying" (71),
// both for the guild's master only. The period's statics (62, 63, 65) stay empty, as the original leaves them.
class TaxWindow : public ui::MainFrame {
public:
    TaxWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
        // Its layout's Create section comes with it (the frames, the background); the three parts' sections next.
        for (const char* section : {"TexListSlot", "TexChangeSlot", "TaxLevySlot"}) AddSection(ui, *this, kTaxLayout, section);
        if (auto* slider = ui::As<ui::Slider>(Child(kTaxSlider))) {
            slider->min = 0;
            slider->max = game::kTaxRateMax - game::kTaxRateMin;
            slider->step = 1;
        }
        const char* marks[] = {"-20%", "-10%", "0%", "10%", "20%"};
        for (int i = 0; i < 5; ++i) {
            if (ui::Widget* w = Child(kTaxMarkFirst + i)) w->SetText(marks[i]);
        }
        if (ui::Widget* w = Child(kTaxPercent)) w->SetText("%");
        for (int i = 0; i < 7; ++i) {
            if (auto* box = ui::As<ui::CheckBox>(Child(kTaxTargetFirst + i))) box->enabled = false;
        }
    }

    void Update(ui::Frame& frame) override {
        // The three parts' help (the original's UIIT_MSG_TP_FORT_TAX_*).
        ShowHelp(frame, *this, {{0x13, "UIIT_MSG_TP_FORT_TAX_TARGET"}, {0x14, "UIIT_MSG_TP_FORT_TAX_CHANGE"}, {0x15, "UIIT_MSG_TP_FORT_TAX_LEVY"}}, c_);
        // (It closes with the NPC's window, Hud::Sync.)
        if (c_.fortress == nullptr || c_.fortress->Revision() == seen_) return;
        seen_ = c_.fortress->Revision();
        Fill();
    }

    bool OnNotice(const ui::Notice& n) override {
        const bool mine = n.source != nullptr && n.source->parent() == this;
        if (mine && n.kind == ui::Notice::Kind::ValueChanged && n.control == kTaxSlider) {
            ShowRate();
            return true;
        }
        if (mine && n.kind == ui::Notice::Kind::Clicked && n.control == kTaxChange) {
            AskRate();
            return true;
        }
        if (mine && n.kind == ui::Notice::Kind::Clicked && n.control == kTaxLevy) {
            AskLevy();
            return true;
        }
        return MainFrame::OnNotice(n);
    }

    // The slider put at `rate` (the test options' way).
    void SetRate(int rate) {
        if (auto* slider = ui::As<ui::Slider>(Child(kTaxSlider))) slider->SetValue(std::clamp(rate, game::kTaxRateMin, game::kTaxRateMax) - game::kTaxRateMin);
        ShowRate();
    }
    // The open box answered (the test options' way).
    bool AnswerBox(bool yes, std::int64_t amount) {
        if (ui_ == nullptr) return false;
        for (ui::Widget* w : ui_->Windows()) {
            auto* box = dynamic_cast<TaxBox*>(w);
            if (box == nullptr || !w->visible) continue;
            if (amount >= 0) {
                if (auto* edit = ui::As<ui::Edit>(box->Child(kLevyEdit))) edit->SetValue(std::to_string(amount));
            }
            if (yes) box->Confirm();
            ui_->Close(box);
            return true;
        }
        return false;
    }
    int Rate() const {
        const auto* slider = ui::As<ui::Slider>(Child(kTaxSlider));
        return slider != nullptr ? slider->value + game::kTaxRateMin : 0;
    }
    void AskRate() {
        if (!Master()) return;
        const proto::FortressTaxInfo& t = c_.fortress->TaxInfo();
        if (auto* box = OpenBox("MsgBoxTaxModify", 293.0f, 185.0f)) box->AskRate(t.npc, t.fortress, t.rate, Rate());
    }
    void AskLevy() {
        if (!Master()) return;
        const proto::FortressTaxInfo& t = c_.fortress->TaxInfo();
        if (auto* box = OpenBox("MsgBoxTaxLevy", 290.0f, 155.0f)) box->AskLevy(t.npc, t.fortress, t.tax);
    }

private:
    // The guild's master (a member flag; inferred the master's).
    bool Master() const {
        if (c_.guild == nullptr || c_.fortress == nullptr) return false;
        const proto::GuildMemberRecord* self = c_.guild->Self();
        const proto::GuildMemberRecord* master = c_.guild->Master();
        return self != nullptr && master != nullptr && self == master;
    }
    TaxBox* OpenBox(const char* section, float w, float h) {
        if (ui_ == nullptr || c_.hud == nullptr) return nullptr;
        for (ui::Widget* other : ui_->Windows()) {
            if (dynamic_cast<TaxBox*>(other) != nullptr && other->visible) ui_->Close(other); // one at a time
        }
        assets::UiLayoutNode node;
        node.type = "CIFTaxBox";
        node.name = "GDR_MSGBOX_TAX";
        node.ddj = "interface\\messagebox\\msgbox2_window_";
        node.text = "UIIT_STT_CONFIRM_BOX";
        node.subSection = section;
        node.rect = {0.0f, 0.0f, w, h}; // ours: the boxes' sizes are set in code, not traced
        auto* box = dynamic_cast<TaxBox*>(ui_->Open(node));
        if (box != nullptr) ui_->Centre(*box, c_.hud->Screen());
        return box;
    }
    void ShowRate() {
        if (ui::Widget* w = Child(kTaxRate)) w->SetText(std::to_string(Rate()));
    }
    void Fill() {
        const proto::FortressTaxInfo& t = c_.fortress->TaxInfo();
        if (ui::Widget* w = Child(kTaxFortressName)) w->SetText(FortressNameOf(c_, t.fortress));
        for (int i = 0; i < 7; ++i) {
            if (auto* box = ui::As<ui::CheckBox>(Child(kTaxTargetFirst + i))) box->checked = (t.targets >> i & 1) != 0;
        }
        if (fortress_ != t.fortress || rate_ != t.rate) SetRate(t.rate); // the slider follows a new rate told
        fortress_ = t.fortress;
        rate_ = t.rate;
        if (ui::Widget* w = Child(kTaxGathered)) w->SetText(std::to_string(t.tax));
        const bool master = Master();
        for (int id : {kTaxChange, kTaxLevy}) {
            if (ui::Widget* w = Child(id)) w->enabled = master;
        }
    }

    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::uint64_t seen_ = ~0ull;
    std::uint32_t fortress_ = ~0u;
    int rate_ = 0;
};

} // namespace

void RegisterFortressWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CIFFortressWarApplyWnd", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<ApplyWindow>(n, *c); });
    ui.Register("CIFTaxManagement", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<TaxWindow>(n, *c); });
    ui.Register("CIFTaxBox", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<TaxBox>(n, *c); });
}

void OpenTaxManagement(HudContext& c) {
    if (c.hud != nullptr) c.hud->Show(kTaxWindow);
}

bool PressTaxManagement(HudContext& c, const std::string& action, std::int64_t value) {
    auto* window = c.hud != nullptr ? dynamic_cast<TaxWindow*>(c.hud->Show(kTaxWindow)) : nullptr;
    if (window == nullptr) return false;
    if (action == "rate") {
        window->SetRate(int(value));
        window->AskRate();
        return true;
    }
    if (action == "levy") {
        window->AskLevy();
        return true;
    }
    return false;
}

bool AnswerTaxBox(HudContext& c, bool yes, std::int64_t amount) {
    auto* window = c.hud != nullptr && c.hud->Shown(kTaxWindow) ? dynamic_cast<TaxWindow*>(c.hud->Show(kTaxWindow)) : nullptr;
    return window != nullptr && window->AnswerBox(yes, amount);
}

void OpenFortressApply(HudContext& c) {
    if (c.hud != nullptr) c.hud->Show(kApplyWindow);
}

bool PressFortressApply(HudContext& c, std::uint32_t fortress) {
    auto* window = c.hud != nullptr ? dynamic_cast<ApplyWindow*>(c.hud->Show(kApplyWindow)) : nullptr;
    if (window == nullptr || c.fortress == nullptr) return false;
    const auto& rows = c.fortress->ApplyInfo().rows;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].fortress != fortress) continue;
        window->Press(i);
        return true;
    }
    return false;
}

std::string FortressNoticeText(const HudContext& c, const game::FortressView::Line& line) {
    const std::string text = c.Text(line.key);
    if (line.number >= 0) return FormatText(text, {std::to_string(line.number)});
    const std::string fortress = line.fortressKey.empty() ? std::string() : c.Text(line.fortressKey);
    if (!line.value.empty()) return line.fortressKey.empty() ? FormatText(text, {line.value}) : FormatText(text, {fortress, line.value});
    if (line.itemRef != 0) {
        const assets::ItemRow* row = c.items != nullptr ? c.items->Find(line.itemRef) : nullptr;
        return FormatText(text, {row != nullptr ? c.Text(row->nameKey) : std::to_string(line.itemRef)});
    }
    if (line.structureRef != 0) {
        const assets::ObjectRow* row = c.characters != nullptr ? c.characters->Find(line.structureRef) : nullptr;
        return FormatText(text, {row != nullptr ? c.Text(row->nameKey) : std::string()});
    }
    const std::string structure = line.structureKey.empty() ? std::string() : c.Text(line.structureKey);
    if (!line.structureKey.empty()) return FormatText(text, {structure, line.guild});
    if (!line.guild.empty()) return FormatText(text, {line.guild, fortress});
    return FormatText(text, {fortress});
}

} // namespace sr::client
