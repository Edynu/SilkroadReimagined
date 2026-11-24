// The fortress manager's and the forges' windows on the client, each from the 1.188
// client's own window code:
// - CIFFortressBusiness (ginterface.txt 302 "Jobs for Fortress War", iffortressbusiness.txt): Create builds its layout's
//   Create section only (its ApplyGuildSolt section, a list of applied guilds, is never made), the fill
//    writes the last war's date into 520 and the next war's into 521 ("%02d-%02d %s %02d:%02d",
//   UIIT_STT_SCHEDULE_WEEKDAY: month, day, the weekday's name, hour, minute), 510 whether an attacker applied
//   (UIIT_STT_FORT_MANAGER_WAR_APPLYGUILD_EXIST / _NONEXIST) and 513 its mark (gil_application_on / _off.ddj).
// - CNIFGuildIntroWnd (window 0xB9, res_ui\guild_intro.2dt): the guild's introduction (create,
//   fill, the count): its title UIIT_PAG_GUILD_INTRODUCE, the tip 1214, the edit 1215 (at most 120), the count
//   1216 ("%d/%d", UIIT_STT_GUILD_INTRODUCE_LIMIT), Confirm 1228; the edit typed in and Confirm enabled only for the
//   commander (the member flag); Confirm sends the introduction (0x705E 0x1A).
// - CIFFortressMakeItemWnd (ginterface.txt 304, iffortressmakeitemwnd.txt + Slot): create, open,
// the work's part, its clock, the tabs, a row. Its title the row's
//   (SN_FORTRESS_SMITH_PRODUCT, UIIT_STT_FORT_ETC_TRAINING); three tabs (ids 300.., 72 x 24 at 25 + 75 i, 55: the smith's
//   UIIT_STT_CAS_CUSTOMER_CENTER_ETC, _FORT_ETC_TRAP, _FORT_ETC_SIEGEWEAPON, the trainer's UIIT_STT_FORT_ETC_FORTRESSWAR,
//   SN_TAB_CART, SN_TAB_VEHICLE), every item of the side listed under the first (adds them all to tab 0); rows
//   500.. of 340 x 32 on gil_bar02_deselect.ddj, seven at least, each its item's icon (11), name (12) and Make (13; 14 and
//   15 hidden). The work: its icon (212), name (213), count (238), the gauge (240, 0% .. 100% under it), the
//   time left (231: UIIT_STT_REMAIN_TIME with days, hours, minutes or seconds), Complete (214) when done or Cancel (215)
//   while it runs, the Make buttons off while there is one; all off unless the player is the Commander or holds the side's
//   position (8 the smith's, 0x10 the trainer's). Its time: count x the row's minutes x 60, x 0.85 when a
//   member holds that position.
// And the questions the manager's, the smith's and the Cave's rows ask (with the NPC window's boxes; the smith's two
// message-box sections MsgBoxMakeItem, MsgBoxMakeItemCancel of ifmessagebox.txt).

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <format>
#include <string>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/assets/UiLayout2dt.h"
#include "sr/game/Fortress.h"
#include "sr/game/Guild.h"
#include "sr/game/Items.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr int kBusinessWindow = 302, kMakeItemWindow = 304, kIntroWindow = 185;

void AddSection(ui::Interface& ui, ui::Widget& to, const char* layout, const char* section) {
    const assets::UiLayout* l = ui.Layout(layout);
    if (l == nullptr) return;
    for (const auto& s : l->sections) {
        if (s.name != section) continue;
        for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) to.Add(ui.Make(*it));
    }
}

// The player's own guild record's fortress position (0: none).
std::uint8_t OwnPosition(const HudContext& c) {
    if (c.guild == nullptr || !c.guild->InGuild()) return 0;
    const proto::GuildMemberRecord* self = c.guild->Self();
    return self != nullptr ? self->position : 0;
}

// The fortress the player's guild holds (0: none).
std::uint32_t HeldFortress(const HudContext& c) {
    if (c.fortress == nullptr || c.guild == nullptr || !c.guild->InGuild()) return 0;
    for (const proto::FortressRecord& f : c.fortress->Fortresses()) {
        if ((f.holder != 0 ? f.holder : f.owner) == c.guild->Guild().id) return f.id;
    }
    return 0;
}

// --- CIFFortressBusiness ---------------------------------------------------------------------------------------------

class BusinessWindow : public ui::MainFrame {
public:
    BusinessWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui); // its layout's Create section
        for (int id : {kLast, kNext}) {
            if (auto* edit = ui::As<ui::Edit>(Child(id))) edit->readOnly = true;
        }
    }

    void Fill(const proto::FortressSchedule& s) {
        if (auto* edit = ui::As<ui::Edit>(Child(kLast))) edit->SetValue(Date(s.last));
        if (auto* edit = ui::As<ui::Edit>(Child(kNext))) edit->SetValue(Date(s.next));
        if (ui::Widget* w = Child(kApplied)) w->SetText(c_.Text(s.applied ? "UIIT_STT_FORT_MANAGER_WAR_APPLYGUILD_EXIST" : "UIIT_STT_FORT_MANAGER_WAR_APPLYGUILD_NONEXIST"));
        if (auto* mark = ui::As<ui::Static>(Child(kMark))) mark->texture = s.applied ? "interface\\guild\\gil_application_on.ddj" : "interface\\guild\\gil_application_off.ddj";
    }

private:
    // The date as writes it: UIIT_STT_SCHEDULE_WEEKDAY with the month, day, weekday's name (0 Sunday; none
    // outside 0..6), hour and minute.
    std::string Date(const std::array<std::uint8_t, 5>& d) const {
        static constexpr const char* kDays[] = {"SUN", "MON", "TUS", "WED", "THU", "FRI", "SAT"};
        const std::string day = d[2] < 7 ? c_.Text(std::string("UIIT_STT_FORT_MANAGER_WAR_SCHEDULE_") + kDays[d[2]]) : std::string();
        const auto two = [](int v) { return std::format("{:02}", v); }; // its "%02d-%02d %s %02d:%02d"
        return FormatText(c_.Text("UIIT_STT_SCHEDULE_WEEKDAY"), {two(d[0]), two(d[1]), day, two(d[3]), two(d[4])});
    }

    static constexpr int kLast = 520, kNext = 521, kApplied = 510, kMark = 513;
    HudContext& c_;
};

// --- CNIFGuildIntroWnd -------------------------------------------------------------------------------------------------

class IntroWindow : public ui::MainFrame {
public:
    IntroWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        SetText(c_.Text("UIIT_PAG_GUILD_INTRODUCE"));
        MainFrame::Created(ui);
        ui_ = &ui;
        const auto* layout = c_.layout2dt ? c_.layout2dt("res_ui\\guild_intro.2dt") : nullptr;
        if (layout == nullptr || layout->empty()) return;
        BuildGuild2dt(ui, *layout, *this, layout->front().rect.x, layout->front().rect.y);
        if (auto* edit = ui::As<ui::Edit>(Child(kEdit))) {
            edit->limit = kLimit;
            edit->multiline = true; // its flag 0x40 (inferred: the notice's multi-line edit)
        }
        if (ui::Widget* tip = Child(kTip)) tip->SetText(c_.Text("UIIT_STT_GUILD_INTRODUCE_TIP"));
        if (ui::Widget* ok = Child(kConfirm)) ok->SetText(c_.Text("UIIS_CTL_CONFIRM"));
    }

    void Fill(std::uint32_t fortress, std::uint32_t npc, const std::string& text) {
        fortress_ = fortress;
        npc_ = npc;
        // The commander types and confirms; anyone else reads.
        const bool commander = OwnPosition(c_) == game::kPositionCommander;
        if (auto* edit = ui::As<ui::Edit>(Child(kEdit))) {
            edit->SetValue(text);
            edit->readOnly = !commander;
        }
        if (ui::Widget* ok = Child(kConfirm)) ok->enabled = commander;
        Count();
    }

    void Update(ui::Frame& frame) override {
        MainFrame::Update(frame);
        Count();
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.control == kConfirm) {
            Confirm();
            return true;
        }
        return MainFrame::OnNotice(n);
    }

    // Confirm (the commander's): the introduction sent (0x705E 0x1A {fortress, text}); the window stays (inferred).
    void Confirm() {
        const auto* edit = ui::As<ui::Edit>(Child(kEdit));
        if (edit == nullptr || edit->readOnly || !c_.fortressRequest) return;
        proto::FortressManage m{npc_, proto::FortressAction::Introduce, fortress_};
        m.text = edit->text();
        c_.fortressRequest(proto::FortressManage::kId, proto::Encode(m));
    }
    void Type(const std::string& text) {
        if (auto* edit = ui::As<ui::Edit>(Child(kEdit)); edit != nullptr && !edit->readOnly) edit->SetValue(text.substr(0, kLimit));
    }

private:
    // "%d/%d": the letters typed and the 120.
    void Count() {
        const auto* edit = ui::As<ui::Edit>(Child(kEdit));
        ui::Widget* w = Child(kCount);
        if (edit == nullptr || w == nullptr) return;
        const std::string shown = FormatText(c_.Text("UIIT_STT_GUILD_INTRODUCE_LIMIT"), {std::to_string(Utf8Length(edit->text())), std::to_string(kLimit)});
        if (shown != w->text()) w->SetText(shown);
    }
    static std::size_t Utf8Length(const std::string& s) {
        return std::size_t(std::count_if(s.begin(), s.end(), [](char ch) { return (static_cast<unsigned char>(ch) & 0xC0) != 0x80; }));
    }

    static constexpr int kTip = 1214, kEdit = 1215, kCount = 1216, kConfirm = 1228;
    static constexpr std::size_t kLimit = 120;
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::uint32_t fortress_ = 0, npc_ = 0;
};

// --- The smith's boxes (MsgBoxMakeItem, MsgBoxMakeItemCancel) ---------------------------------------------------------

class MakeItemBox : public ui::MainFrame {
public:
    MakeItemBox(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
        // The message box's own background (ifmessagebox.txt's Create section), sized to the box, then the section.
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
        if (auto* edit = ui::As<ui::Edit>(Child(Cancelling() ? kCancelCount : kCount))) {
            edit->numeric = true;
            edit->limit = 2;
            edit->passKeys = true;
            edit->readOnly = Cancelling(); // the cancel box shows the work's count (inferred)
        }
    }

    void Ask(const std::string& name, int count, std::function<void(int count)> yes) {
        yes_ = std::move(yes);
        const bool cancel = Cancelling();
        if (ui::Widget* w = Child(cancel ? kCancelMain : kMain)) {
            w->SetText(FormatText(c_.Text(cancel ? "UIIT_MSG_FORT_SMITH_PRODUCT_CANCEL_WINDOW" : "UIIT_MSG_FORT_SMITH_PRODUCT_WINDOW"), {name}));
        }
        if (auto* edit = ui::As<ui::Edit>(Child(cancel ? kCancelCount : kCount))) {
            edit->SetValue(std::to_string(count));
            if (!cancel && ui_ != nullptr) ui_->Focus(edit);
        }
    }

    bool OnNotice(const ui::Notice& n) override {
        const bool mine = n.source != nullptr && n.source->parent() == this;
        const bool confirm = mine && ((n.kind == ui::Notice::Kind::Clicked && (n.control == kConfirm || n.control == kCancelConfirm)) ||
                                      n.kind == ui::Notice::Kind::Submitted);
        const bool cancel = mine && ((n.kind == ui::Notice::Kind::Clicked && (n.control == kCancel || n.control == kCancelCancel || n.control == 2)) ||
                                     n.kind == ui::Notice::Kind::Cancelled);
        if (!confirm && !cancel) return MainFrame::OnNotice(n);
        if (confirm) Answer();
        if (ui_ != nullptr) ui_->Close(this);
        return true;
    }
    void Answer() {
        const auto* edit = ui::As<ui::Edit>(Child(Cancelling() ? kCancelCount : kCount));
        const int count = edit != nullptr ? std::atoi(edit->text().c_str()) : 0;
        if (yes_) yes_(count);
    }

private:
    bool Cancelling() const { return node().subSection == "MsgBoxMakeItemCancel"; }
    static constexpr int kMain = 114, kCount = 116, kConfirm = 111, kCancel = 110;
    static constexpr int kCancelMain = 124, kCancelCount = 126, kCancelConfirm = 121, kCancelCancel = 120;
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::function<void(int)> yes_;
};

MakeItemBox* OpenMakeItemBox(HudContext& c, const char* section) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return nullptr;
    for (ui::Widget* w : ui->Windows()) {
        if (dynamic_cast<MakeItemBox*>(w) != nullptr && w->visible) ui->Close(w); // one at a time
    }
    assets::UiLayoutNode node;
    node.type = "CIFMakeItemBox";
    node.name = "GDR_MSGBOX_MAKEITEM";
    node.ddj = "interface\\messagebox\\msgbox2_window_";
    node.text = "UIIT_STT_CONFIRM_BOX";
    node.subSection = section;
    node.rect = {0.0f, 0.0f, 285.0f, 160.0f}; // ours: its section's buttons at y 120 with the frame's margins
    auto* box = dynamic_cast<MakeItemBox*>(ui->Open(node));
    if (box != nullptr) ui->Centre(*box, c.hud->Screen());
    return box;
}

// --- CIFFortressMakeItemWnd ----------------------------------------------------------------------------------------------

class MakeItemWindow : public ui::MainFrame {
public:
    MakeItemWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui); // the Create section
        ui_ = &ui;
        for (int i = 0; i < 5; ++i) {
            if (ui::Widget* w = Child(kPercentFirst + i)) w->SetText(std::to_string(i * 25) + "%");
        }
        if (auto* gauge = ui::As<ui::Gauge>(Child(kGauge))) gauge->SetValue(0.0f, true);
        // The three tabs: buttons 300.. at 25 + 75 i, 55, 72 x 24.
        for (int i = 0; i < 3; ++i) {
            assets::UiLayoutNode n;
            n.type = "CIFButton";
            n.id = kTabFirst + i;
            n.ddj = "interface\\ifcommon\\com_tab_off.ddj";
            n.rect = {25.0f + 75.0f * float(i), 55.0f, 72.0f, 24.0f};
            Add(ui.Make(n));
        }
        if (ui::Widget* list = Child(kList)) {
            for (int i = 0; i < kVisibleRows; ++i) {
                assets::UiLayoutNode n;
                n.type = "CIFStatic";
                n.id = kRowFirst + i;
                n.ddj = "interface\\guild\\gil_bar02_deselect.ddj";
                n.rect = {0.0f, float(i) * 32.0f, 340.0f, 32.0f};
                auto made = ui.Make(n);
                ui::Widget* row = made.get();
                list->Add(std::move(made));
                AddSection(ui, *row, "resinfo\\iffortressmakeitemwndslot.txt", "Create");
                for (int id : {kRowMake, kRowComplete, kRowCancel}) {
                    if (ui::Widget* b = row->Child(id)) b->visible = false; // the slot's Create hides all three
                }
                if (auto* slot = ui::As<ui::Slot>(row->Child(kRowIcon))) slot->pickable = false;
            }
            if (auto* bar = ui::As<ui::ScrollBar>(list->Child(10))) bar->SetRange(0, 0);
        }
        if (auto* slot = ui::As<ui::Slot>(Child(kWorkIcon))) slot->pickable = false;
    }

    // Opened for a side (0 the smith, 1 the trainer) by its row's answer.
    void Open(const proto::FortressForge& work) {
        side_ = work.side;
        npc_ = work.npc;
        fortress_ = work.fortress != 0 ? work.fortress : HeldFortress(c_);
        // Its title: the frame's title static (id 0, as the layout's first frame is: told apart by its class).
        for (const auto& child : children()) {
            if (child->id() == 0 && child->node().type == "CIFStatic") child->SetText(c_.Text(side_ == 0 ? "SN_FORTRESS_SMITH_PRODUCT" : "UIIT_STT_FORT_ETC_TRAINING"));
        }
        if (ui::Widget* heading = Child(kHeading)) heading->SetText(c_.Text(side_ == 0 ? "SN_FORTRESS_SMITH_PRODUCT" : "UIIT_STT_FORT_ETC_TRAINING"));
        const char* tabs[2][3] = {{"UIIT_STT_CAS_CUSTOMER_CENTER_ETC", "UIIT_STT_FORT_ETC_TRAP", "UIIT_STT_FORT_ETC_SIEGEWEAPON"},
                                  {"UIIT_STT_FORT_ETC_FORTRESSWAR", "SN_TAB_CART", "SN_TAB_VEHICLE"}};
        for (int i = 0; i < 3; ++i) {
            if (ui::Widget* t = Child(kTabFirst + i)) t->SetText(c_.Text(tabs[side_ == 0 ? 0 : 1][i]));
        }
        // The side's items of the fortress (siegefortressitemforge.txt; the trainer's TID 3,3,3,2), all under the first tab;
        // in the order of their ids (inferred: the client sorts by a field of the item's row).
        items_.clear();
        if (c_.worldMap != nullptr) {
            for (const assets::FortressForgeRow& r : c_.worldMap->forges) {
                if (std::uint32_t(r.fortress) != fortress_) continue;
                const assets::ItemRow* row = c_.items != nullptr ? c_.items->Find(r.item) : nullptr;
                const bool transport = row != nullptr && row->typeId == std::array<int, 4>{3, 3, 3, 2};
                if (transport == (side_ == 1)) items_.push_back(r);
            }
        }
        std::sort(items_.begin(), items_.end(), [](const auto& a, const auto& b) { return a.item < b.item; });
        Tab(0);
        Work(work);
    }

    void Tab(int tab) {
        tab_ = tab;
        first_ = 0;
        for (int i = 0; i < 3; ++i) {
            if (auto* t = ui::As<ui::Button>(Child(kTabFirst + i))) t->texture = i == tab ? "interface\\ifcommon\\com_tab_on.ddj" : "interface\\ifcommon\\com_tab_off.ddj";
        }
        if (ui::Widget* list = Child(kList)) {
            if (auto* bar = ui::As<ui::ScrollBar>(list->Child(10))) {
                bar->SetRange(0, std::max(0, int(Shown().size()) - kVisibleRows));
                bar->SetPosition(0);
            }
        }
        Rows();
    }

    // The work's part from the server's answer.
    void Work(const proto::FortressForge& work) {
        work_ = work;
        const bool has = work.has && work.count > 0;
        const assets::ItemRow* row = has && c_.items != nullptr ? c_.items->Find(work.item) : nullptr;
        if (auto* slot = ui::As<ui::Slot>(Child(kWorkIcon))) slot->icon = row == nullptr ? std::string() : row->iconPath.empty() ? "icon\\icon_default.ddj" : "icon\\" + row->iconPath;
        if (ui::Widget* w = Child(kWorkName)) w->SetText(row != nullptr ? c_.Text(row->nameKey) : std::string());
        if (ui::Widget* w = Child(kWorkCount)) w->SetText(has ? std::to_string(work.count) : std::string());
        if (ui::Widget* w = Child(kComplete)) w->visible = has && work.done;
        if (ui::Widget* w = Child(kCancel)) w->visible = has && !work.done;
        // The work's whole time: count x the row's minutes x 60, x 0.85 with a member in the side's position.
        total_ = 0;
        if (has && c_.worldMap != nullptr) {
            for (const assets::FortressForgeRow& r : c_.worldMap->forges) {
                if (std::uint32_t(r.fortress) == fortress_ && r.item == work.item) total_ = std::int64_t(work.count) * r.minutes * 60;
            }
            if (MemberHolds(side_ == 0 ? game::kPositionProduction : game::kPositionTraining)) total_ = std::int64_t(double(total_) * 0.85 + 0.5);
        }
        Permit();
        Clock();
    }

    void Update(ui::Frame& frame) override {
        MainFrame::Update(frame);
        // The clock: each second (its timer 10).
        const std::int64_t now = std::int64_t(std::time(nullptr));
        if (now != shownAt_) Clock();
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked) {
            if (n.control >= kTabFirst && n.control < kTabFirst + 3 && n.source != nullptr && n.source->parent() == this) {
                Tab(n.control - kTabFirst);
                return true;
            }
            if (n.control == kRowMake && n.source != nullptr && n.source->parent() != nullptr) {
                Make(first_ + n.source->parent()->id() - kRowFirst);
                return true;
            }
            if (n.control == kComplete) {
                Collect();
                return true;
            }
            if (n.control == kCancel) {
                AskCancel();
                return true;
            }
        }
        if (n.kind == ui::Notice::Kind::ScrollChanged && n.source != nullptr && n.source->id() == 10) {
            first_ = std::max(0, n.value);
            Rows();
            return true;
        }
        return MainFrame::OnNotice(n);
    }

    // A row's Make: "Do you want to produce [%s]?" with the count (MsgBoxMakeItem), then the start {item, count}.
    void Make(int index) {
        const std::vector<const assets::FortressForgeRow*> shown = Shown();
        if (index < 0 || std::size_t(index) >= shown.size() || !Permitted() || (work_.has && work_.count > 0)) return;
        const assets::FortressForgeRow r = *shown[std::size_t(index)];
        HudContext* c = &c_;
        const std::uint32_t npc = npc_, fortress = fortress_;
        const proto::FortressAction start = side_ == 0 ? proto::FortressAction::ForgeStart : proto::FortressAction::TrainStart;
        if (auto* box = OpenMakeItemBox(c_, "MsgBoxMakeItem")) {
            box->Ask(Name(r.item), 1, [c, npc, fortress, start, item = r.item](int count) {
                if (count <= 0 || !c->fortressRequest) return;
                c->fortressRequest(proto::FortressManage::kId, proto::Encode(proto::FortressManage{npc, start, fortress, item, std::uint32_t(count)}));
            });
        }
    }
    // Cancel: "Do you want to cancel production of [%s]?" (MsgBoxMakeItemCancel), then the cancel.
    void AskCancel() {
        if (!work_.has || work_.done || !Permitted()) return;
        HudContext* c = &c_;
        const std::uint32_t npc = npc_, fortress = fortress_;
        const proto::FortressAction cancel = side_ == 0 ? proto::FortressAction::ForgeCancel : proto::FortressAction::TrainCancel;
        if (auto* box = OpenMakeItemBox(c_, "MsgBoxMakeItemCancel")) {
            box->Ask(Name(work_.item), work_.count, [c, npc, fortress, cancel, item = work_.item](int) {
                if (c->fortressRequest) c->fortressRequest(proto::FortressManage::kId, proto::Encode(proto::FortressManage{npc, cancel, fortress, item}));
            });
        }
    }
    // Complete: the work collected into the bag, a stack at a time (inferred: no box asks a count; the server takes no more
    // than the item's MaxStack into one empty place, 0x281B).
    void Collect() {
        if (!work_.has || !work_.done || !Permitted() || !c_.fortressRequest) return;
        const proto::FortressAction collect = side_ == 0 ? proto::FortressAction::ForgeCollect : proto::FortressAction::TrainCollect;
        const assets::ItemRow* row = c_.items != nullptr ? c_.items->Find(work_.item) : nullptr;
        const std::uint32_t stack = row != nullptr ? std::uint32_t(std::max(1, game::MaxStack(*row))) : std::uint32_t(work_.count);
        c_.fortressRequest(proto::FortressManage::kId, proto::Encode(proto::FortressManage{npc_, collect, fortress_, work_.item, std::min<std::uint32_t>(work_.count, stack)}));
    }
    bool AnswerBox(bool yes, int count) {
        if (ui_ == nullptr) return false;
        for (ui::Widget* w : ui_->Windows()) {
            auto* box = dynamic_cast<MakeItemBox*>(w);
            if (box == nullptr || !w->visible) continue;
            if (count > 0) {
                if (auto* edit = ui::As<ui::Edit>(box->Child(116))) edit->SetValue(std::to_string(count));
            }
            if (yes) box->Answer();
            ui_->Close(box);
            return true;
        }
        return false;
    }
    int RowOf(std::uint32_t item) const {
        const std::vector<const assets::FortressForgeRow*> shown = Shown();
        for (std::size_t i = 0; i < shown.size(); ++i) {
            if (shown[i]->item == item) return int(i);
        }
        return -1;
    }

private:
    std::vector<const assets::FortressForgeRow*> Shown() const {
        std::vector<const assets::FortressForgeRow*> out;
        if (tab_ != 0) return out; // the other two tabs stay empty (adds every item to the first)
        for (const auto& r : items_) out.push_back(&r);
        return out;
    }
    std::string Name(std::uint32_t item) const {
        const assets::ItemRow* row = c_.items != nullptr ? c_.items->Find(item) : nullptr;
        return row != nullptr ? c_.Text(row->nameKey) : std::to_string(item);
    }
    void Rows() {
        ui::Widget* list = Child(kList);
        if (list == nullptr) return;
        const std::vector<const assets::FortressForgeRow*> shown = Shown();
        for (int i = 0; i < kVisibleRows; ++i) {
            ui::Widget* row = list->Child(kRowFirst + i);
            if (row == nullptr) continue;
            const std::size_t k = std::size_t(first_ + i);
            const assets::ItemRow* item = k < shown.size() && c_.items != nullptr ? c_.items->Find(shown[k]->item) : nullptr;
            if (auto* slot = ui::As<ui::Slot>(row->Child(kRowIcon))) slot->icon = item == nullptr ? std::string() : item->iconPath.empty() ? "icon\\icon_default.ddj" : "icon\\" + item->iconPath;
            if (ui::Widget* name = row->Child(kRowName)) name->SetText(item != nullptr ? c_.Text(item->nameKey) : std::string());
            if (ui::Widget* make = row->Child(kRowMake)) make->visible = item != nullptr; // shown with its item
        }
        Permit();
    }
    // Whether the player may work it: the Commander, or the side's position.
    bool Permitted() const {
        const std::uint8_t p = OwnPosition(c_);
        return p == game::kPositionCommander || p == (side_ == 0 ? game::kPositionProduction : game::kPositionTraining);
    }
    bool MemberHolds(std::uint8_t position) const {
        if (c_.guild == nullptr) return false;
        for (const proto::GuildMemberRecord& m : c_.guild->Guild().members) {
            if (m.position == position) return true;
        }
        return false;
    }
    // The buttons' state (with's arguments): no work, the Make buttons; done, Complete; running,
    // Cancel; nothing without the permission.
    void Permit() {
        const bool may = Permitted();
        const bool has = work_.has && work_.count > 0;
        if (ui::Widget* list = Child(kList)) {
            for (int i = 0; i < kVisibleRows; ++i) {
                if (ui::Widget* row = list->Child(kRowFirst + i)) {
                    if (ui::Widget* make = row->Child(kRowMake)) make->enabled = may && !has;
                }
            }
        }
        if (ui::Widget* w = Child(kComplete)) w->enabled = may && has && work_.done;
        if (ui::Widget* w = Child(kCancel)) w->enabled = may && has && !work_.done;
    }
    // The gauge and the time left: "%s: %d%s %d%s %d%s" with days, hours, minutes; under an hour the
    // minutes + 1; under a minute the seconds.
    void Clock() {
        const std::int64_t now = std::int64_t(std::time(nullptr));
        shownAt_ = now;
        auto* gauge = ui::As<ui::Gauge>(Child(kGauge));
        ui::Widget* left = Child(kLeft);
        const bool has = work_.has && work_.count > 0;
        if (!has) {
            if (gauge != nullptr) gauge->SetValue(0.0f, true);
            if (left != nullptr) left->SetText({});
            return;
        }
        if (work_.done || work_.finishes <= now) {
            if (gauge != nullptr) gauge->SetValue(1.0f, true);
            if (left != nullptr) left->SetText({});
            return;
        }
        const std::int64_t remaining = work_.finishes - now;
        if (gauge != nullptr && total_ > 0) gauge->SetValue(std::clamp(float(double(total_ - remaining) / double(total_)), 0.0f, 1.0f), true);
        if (left == nullptr) return;
        const std::int64_t minutes = remaining / 60, hours = minutes / 60, days = hours / 24;
        const std::string head = c_.Text("UIIT_STT_REMAIN_TIME");
        std::string text;
        if (days > 0) {
            text = head + " : " + std::to_string(days) + c_.Text("PARAM_DAY") + " " + std::to_string(hours % 24) + c_.Text("UIIT_STT_HOUR") + " " +
                   std::to_string(minutes % 60) + c_.Text("UIIT_STT_MINUTE") + " ";
        } else if (hours > 0) {
            text = head + " : " + std::to_string(hours) + c_.Text("UIIT_STT_HOUR") + " " + std::to_string(minutes % 60) + c_.Text("UIIT_STT_MINUTE") + " ";
        } else if (minutes > 0) {
            text = head + " : " + std::to_string(minutes + 1) + " " + c_.Text("UIIT_STT_MINUTE");
        } else {
            text = head + " : " + std::to_string(remaining) + " " + c_.Text("PARAM_SECOND");
        }
        left->SetText(text);
    }

    static constexpr int kGauge = 240, kPercentFirst = 232, kHeading = 230, kLeft = 231, kList = 210;
    static constexpr int kWorkIcon = 212, kWorkName = 213, kComplete = 214, kCancel = 215, kWorkCount = 238;
    static constexpr int kTabFirst = 300, kRowFirst = 500, kVisibleRows = 7;
    static constexpr int kRowIcon = 11, kRowName = 12, kRowMake = 13, kRowComplete = 14, kRowCancel = 15;
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::uint8_t side_ = 0;
    std::uint32_t npc_ = 0, fortress_ = 0;
    int tab_ = 0, first_ = 0;
    std::vector<assets::FortressForgeRow> items_;
    proto::FortressForge work_;
    std::int64_t total_ = 0, shownAt_ = 0;
};

} // namespace

void RegisterFortressWorkWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CIFFortressBusiness", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<BusinessWindow>(n, *c); });
    ui.Register("CIFFortressMakeItemWnd", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<MakeItemWindow>(n, *c); });
    ui.Register("CNIFGuildIntroWnd", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<IntroWindow>(n, *c); });
    ui.Register("CIFMakeItemBox", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<MakeItemBox>(n, *c); });
}

void OpenFortressBusiness(HudContext& c, const proto::FortressSchedule& schedule) {
    if (auto* window = c.hud != nullptr ? dynamic_cast<BusinessWindow*>(c.hud->Show(kBusinessWindow)) : nullptr) window->Fill(schedule);
}

void OpenGuildIntroduction(HudContext& c, const proto::FortressIntroduction& intro, std::uint32_t npc) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr || !c.layout2dt) return;
    IntroWindow* window = nullptr;
    for (ui::Widget* w : ui->Windows()) {
        if (auto* open = dynamic_cast<IntroWindow*>(w)) window = open;
    }
    if (window == nullptr) {
        const auto* layout = c.layout2dt("res_ui\\guild_intro.2dt");
        if (layout == nullptr || layout->empty()) return;
        assets::UiLayoutNode node;
        node.type = "CNIFGuildIntroWnd";
        node.id = kIntroWindow;
        node.ddj = layout->front().frame;
        node.rect = {0.0f, 0.0f, layout->front().rect.w, layout->front().rect.h};
        window = dynamic_cast<IntroWindow*>(ui->Open(node));
        if (window == nullptr) return;
        ui->Centre(*window, ui->screen());
    }
    window->visible = true;
    window->Fill(intro.fortress, npc, intro.text);
}

bool TypeGuildIntroduction(HudContext& c, const std::string& text, bool confirm) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return false;
    for (ui::Widget* w : ui->Windows()) {
        auto* window = dynamic_cast<IntroWindow*>(w);
        if (window == nullptr || !w->visible) continue;
        window->Type(text);
        if (confirm) window->Confirm();
        return true;
    }
    return false;
}

void OpenFortressMakeItem(HudContext& c, const proto::FortressForge& work, bool open) {
    if (c.hud == nullptr) return;
    // The query's answer opens it; a later answer (a start, a cancel, a collect) refills an open one.
    if (!open && !c.hud->Shown(kMakeItemWindow)) return;
    auto* window = dynamic_cast<MakeItemWindow*>(c.hud->Show(kMakeItemWindow));
    if (window == nullptr) return;
    if (open) {
        window->Open(work);
    } else {
        window->Work(work);
    }
}

bool PressFortressMakeItem(HudContext& c, const std::string& action, std::uint32_t item, int count) {
    auto* window = c.hud != nullptr && c.hud->Shown(kMakeItemWindow) ? dynamic_cast<MakeItemWindow*>(c.hud->Show(kMakeItemWindow)) : nullptr;
    if (window == nullptr) return false;
    if (action == "make") {
        window->Make(window->RowOf(item));
        return window->AnswerBox(true, count);
    }
    if (action == "cancel") {
        window->AskCancel();
        return window->AnswerBox(true, 0);
    }
    if (action == "complete") {
        window->Collect();
        return true;
    }
    // The gallery's: the boxes asked and left open.
    if (action == "ask-make") {
        window->Make(window->RowOf(item));
        return true;
    }
    if (action == "ask-cancel") {
        window->AskCancel();
        return true;
    }
    if (action == "tab") {
        window->Tab(int(item));
        return true;
    }
    return false;
}

} // namespace sr::client
