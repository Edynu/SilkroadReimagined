// The guild window's side windows: the notice's editor
// (CIFGuildNotifyWrite, ginterface.txt 47, ifguildnotifywrite.txt), the window that turns skill points
// into guild points (CIFGuildPointUp, window 1500, ifguildpointup.txt), a member's fortress position
// (CIFGuildPositionGrant, window 1600, ifguildpositiongrant.txt) and the guild points' history
// (CNIFGPHistoryWnd, res_ui\guild_gp.2dt).

#include <algorithm>
#include <cstdlib>
#include <string>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/assets/UiLayout2dt.h"
#include "sr/game/Academy.h"
#include "sr/game/Guild.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr int kNoticeWindow = 47;

// at most 1023 characters of body and 127 of title; Confirm's own checks.
class NoticeWriteWindow : public ui::FrameBorder {
public:
    NoticeWriteWindow(const assets::UiLayoutNode& node, HudContext& c) : FrameBorder(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        FrameBorder::Created(ui);
        ui_ = &ui;
    }

    // Filled with the notice as it stands, the title first to type in; `academy`: the academy's notice (the same window,
    // its Confirm sending the academy's 0x7477).
    void Begin(bool academy = false) {
        academy_ = academy;
        std::string noticeTitle, notice;
        if (academy) {
            if (c_.state == nullptr) return;
            noticeTitle = c_.state->academy.Info().noticeTitle;
            notice = c_.state->academy.Info().notice;
        } else {
            if (c_.guild == nullptr) return;
            noticeTitle = c_.guild->Guild().noticeTitle;
            notice = c_.guild->Guild().notice;
        }
        auto* title = ui::As<ui::Edit>(Child(kTitle));
        auto* body = ui::As<ui::Edit>(Child(kBody));
        if (title != nullptr) {
            title->limit = academy ? int(game::academy::kNoticeTitleMax) : int(kTitleMost); // the academy's 1..128, 1..2048 (its server's)
            title->SetValue(noticeTitle);
            if (ui_ != nullptr) ui_->Focus(title);
        }
        if (body != nullptr) {
            body->multiline = true;
            body->limit = academy ? int(game::academy::kNoticeTextMax) : int(kBodyMost);
            body->SetValue(notice);
        }
    }

    void Update(ui::Frame& frame) override {
        FrameBorder::Update(frame);
        // "Please enter the contents of the note.(%d/Max.%d)"
        const auto* body = ui::As<ui::Edit>(Child(kBody));
        ui::Widget* counter = Child(kCounter);
        if (body == nullptr || counter == nullptr || !c_.text) return;
        const std::string text = FormatText(c_.text("UIIT_MSG_GUILD_COMMON_KNOW_REMIND"), {std::to_string(game::Utf8Length(body->text())), std::to_string(body->limit)});
        if (counter->text() != text) counter->SetText(text);
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr || n.source->parent() != this) return FrameBorder::OnNotice(n);
        if (n.control == kCancel) {
            visible = false;
            return true;
        }
        if (n.control == kConfirm) Confirm();
        return true;
    }

    void Confirm() {
        const auto* title = ui::As<ui::Edit>(Child(kTitle));
        const auto* body = ui::As<ui::Edit>(Child(kBody));
        if (title == nullptr || body == nullptr) return;
        const auto post = [&](std::uint16_t code) {
            if (c_.state != nullptr) c_.state->Post(game::GameMessage::Kind::General, GuildErrorText(c_, code, 0), c_.time);
        };
        if (title->text().empty()) return post(std::uint16_t(game::GuildError::NoticeTitle));
        if (body->text().empty()) return post(std::uint16_t(game::GuildError::NoticeBody));
        if (academy_) {
            if (c_.academyRequest) c_.academyRequest(proto::AcademyNoticeWrite::kId, proto::Encode(proto::AcademyNoticeWrite{title->text(), body->text()}));
        } else if (c_.guildRequest) {
            c_.guildRequest(proto::GuildNoticeEdit::kId, proto::Encode(proto::GuildNoticeEdit{title->text(), body->text()}));
        }
        visible = false;
    }

    void Fill(const std::string& title, const std::string& body) {
        if (auto* t = ui::As<ui::Edit>(Child(kTitle))) t->SetValue(title);
        if (auto* b = ui::As<ui::Edit>(Child(kBody))) b->SetValue(body);
    }

private:
    static constexpr int kCounter = 2, kTitle = 10, kBody = 11, kConfirm = 12, kCancel = 13;
    static constexpr std::size_t kTitleMost = 127, kBodyMost = 1023;
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    bool academy_ = false;
};

// the amount typed goes to the server, which holds it to the skill points there are.
class DonateWindow : public ui::FrameBorder {
public:
    DonateWindow(const assets::UiLayoutNode& node, HudContext& c) : FrameBorder(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        FrameBorder::Created(ui);
        ui_ = &ui;
    }

    void Begin() {
        if (ui::Widget* name = Child(kGuildName); name != nullptr && c_.guild != nullptr) name->SetText(c_.guild->Guild().name);
        if (auto* edit = ui::As<ui::Edit>(Child(kAmount))) {
            edit->numeric = true;
            edit->limit = 9;
            edit->SetValue("0");
            if (ui_ != nullptr) ui_->Focus(edit);
        }
    }

    void Update(ui::Frame& frame) override {
        FrameBorder::Update(frame);
        if (!laidOut_ && c_.text) LayOut(frame.paint);
        ui::Widget* points = Child(kPoints);
        if (points == nullptr || c_.state == nullptr) return;
        const std::string text = std::to_string(c_.state->self.skillPoints);
        if (points->text() != text) points->SetText(text);
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr || n.source->parent() != this) return FrameBorder::OnNotice(n);
        if (n.control == kOk) Press(true);
        if (n.control == kCancel) Press(false);
        return true;
    }

    void Press(bool ok) {
        const auto* edit = ui::As<ui::Edit>(Child(kAmount));
        const std::uint32_t amount = edit != nullptr ? std::uint32_t(std::strtoul(edit->text().c_str(), nullptr, 10)) : 0;
        if (ok && amount != 0 && c_.guildRequest) c_.guildRequest(proto::GuildDonate::kId, proto::Encode(proto::GuildDonate{amount}));
        if (ui_ != nullptr) ui_->Close(this);
    }

    void Fill(std::uint32_t amount) {
        if (auto* edit = ui::As<ui::Edit>(Child(kAmount))) edit->SetValue(std::to_string(amount));
    }

private:
    // Ours: the English lines are wider than the layout's single rows (the widest 490 pixels in a
    // window of 360), so the help's texts go wrapped in the rows they share, and the label clear of its number.
    void LayOut(ui::Painter& paint) {
        laidOut_ = true;
        if (ui::Widget* first = Child(612)) first->rect.x += 8.0f;
        if (ui::Widget* label = Child(607)) {
            label->rect.x = 30.0f;
            label->rect.w = 200.0f;
        }
        const float width = 324.0f;
        std::vector<std::string> rows;
        for (const char* key : {"UIIT_MSG_GUILD_GP_SUBSCRIPION_LIMIT2", "UIIT_MSG_GUILD_GP_SUBSCRIPION_LIMIT3", "UIIT_MSG_GUILD_GP_SUBSCRIPION_LIMIT4"}) {
            std::string row;
            for (std::size_t at = 0; at < c_.text(key).size();) {
                const std::string text = c_.text(key);
                std::size_t end = text.find(' ', at);
                end = end == std::string::npos ? text.size() : end;
                const std::string word = text.substr(at, end - at);
                const std::string longer = row.empty() ? word : row + " " + word;
                if (!row.empty() && paint.TextWidth(longer, 0) > width) {
                    rows.push_back(row);
                    row = word;
                } else {
                    row = longer;
                }
                at = end + 1;
            }
            if (!row.empty()) rows.push_back(row);
        }
        if (Child(615) != nullptr && ui_ != nullptr && rows.size() > 3) {
            assets::UiLayoutNode extra = Child(615)->node();
            extra.id = 616;
            extra.text.clear();
            Add(ui_->Make(extra));
        }
        int id = 613;
        for (std::size_t i = 0; i < rows.size() && i < 4; ++i, ++id) {
            ui::Widget* row = Child(id);
            if (row == nullptr) continue;
            row->SetText(rows[i]);
            row->rect = {18.0f, 71.0f + float(i) * 14.0f, width, 14.0f};
        }
    }

    static constexpr int kOk = 604, kCancel = 605, kPoints = 608, kAmount = 609, kGuildName = 611;
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    bool laidOut_ = false;
};

// The six choices: the five positions with the places left, and none.
constexpr std::uint8_t kPositionChoices[] = {game::kPositionDeputy, game::kPositionWar, game::kPositionProduction, game::kPositionTraining, game::kPositionEngineer, game::kPositionNone};

class PositionWindow : public ui::FrameBorder {
public:
    PositionWindow(const assets::UiLayoutNode& node, HudContext& c) : FrameBorder(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        FrameBorder::Created(ui);
        ui_ = &ui;
    }

    void Begin(std::uint32_t character) {
        character_ = character;
        picked_ = -1;
        if (ui_ == nullptr || !c_.text || c_.guild == nullptr) return;
        if (marks_.empty()) {
            assets::UiLayoutNode title;
            title.type = "CIFStatic";
            title.rect = {20.0f, 12.0f, rect.w - 40.0f, 14.0f};
            title.hAlign = 1;
            title.text = "UIIT_CTL_GUILD_POSITION_GRANT";
            Add(ui_->Make(title));
            // A box's "on" look over it (the layout gives only its off art).
            for (int i = 0; i < 6; ++i) {
                ui::Widget* box = Child(kFirstBox + i);
                if (box == nullptr) continue;
                assets::UiLayoutNode mark;
                mark.type = "CIFStatic";
                mark.ddj = "interface\\ifcommon\\com_radiobutton_on.ddj";
                mark.rect = {box->rect.x, box->rect.y, box->rect.w, box->rect.h};
                marks_.push_back(Add(ui_->Make(mark)));
            }
            // Ours: the longest English label runs past the black panel from where the layout starts it.
            for (int i = 0; i < 6; ++i) {
                for (ui::Widget* w : {Child(kFirstBox + i), Child(kFirstLabel + i)}) {
                    if (w != nullptr) w->rect.x -= kShiftLeft;
                }
                if (std::size_t(i) < marks_.size()) marks_[std::size_t(i)]->rect.x -= kShiftLeft;
            }
        }
        const game::GuildView& view = *c_.guild;
        bool fortress = false;
        if (c_.fortress != nullptr) {
            for (const proto::FortressRecord& f : c_.fortress->Fortresses()) fortress = fortress || f.owner == view.Guild().id;
        }
        for (int i = 0; i < 6; ++i) {
            const std::uint8_t position = kPositionChoices[i];
            if (ui::Widget* box = Child(kFirstBox + i)) box->enabled = !game::GuildPositionNeedsFortress(position) || fortress;
            ui::Widget* label = Child(kFirstLabel + i);
            if (label == nullptr) continue;
            if (position == game::kPositionNone) {
                label->SetText(c_.text("UIIT_STT_FORT_GUILD_POSITION_NOT"));
                continue;
            }
            int held = 0;
            for (const auto& m : view.Guild().members) held += m.position == position ? 1 : 0;
            label->SetText(FormatText("%s (%d)", {c_.text(game::GuildPositionKey(position)), std::to_string(game::GuildPositionPlaces(position) - held)}));
        }
        Show();
    }

    void Update(ui::Frame& frame) override {
        FrameBorder::Update(frame);
        // A position's own help over its box and its name (the original's UIIT_MSG_TP_FORT_GUILD_*, its name's key's
        // help form); "no position" has none.
        if (!c_.text) return;
        // The mouse on this window: on it or one of its parts, or on a decoration of it (nothing hovered).
        bool over = frame.pointer.hovered == nullptr && ScreenRect().Contains(frame.input.mouse());
        for (const ui::Widget* p = frame.pointer.hovered; p != nullptr && !over; p = p->parent()) over = p == this;
        for (int i = 0; i < 6; ++i) {
            const std::uint8_t position = kPositionChoices[i];
            if (position == game::kPositionNone) continue;
            for (ui::Widget* w : {Child(kFirstBox + i), Child(kFirstLabel + i)}) {
                // The name is a decoration (never the hovered control): the mouse over its place.
                if (w == nullptr || (frame.pointer.hovered != w && !(over && w->ScreenRect().Contains(frame.input.mouse())))) continue;
                std::string key = game::GuildPositionKey(position);
                if (key.starts_with("UIIT_STT_")) key = "UIIT_MSG_TP_" + key.substr(9);
                frame.interface.ShowTooltip(c_.text(key), w->ScreenRect());
            }
        }
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr || n.source->parent() != this) return FrameBorder::OnNotice(n);
        if (n.control >= kFirstBox && n.control < kFirstBox + 6) {
            picked_ = n.control - kFirstBox;
            Show();
        }
        if (n.control == kConfirm) Confirm();
        if (n.control == kCancel && ui_ != nullptr) ui_->Close(this);
        return true;
    }

    void Pick(std::uint8_t position) {
        for (int i = 0; i < 6; ++i) {
            if (kPositionChoices[i] == position) picked_ = i;
        }
        Show();
    }

    void Confirm() {
        // Nothing is sent without a choice.
        if (picked_ < 0) return;
        if (c_.guildRequest) c_.guildRequest(proto::GuildPositionGrant::kId, proto::Encode(proto::GuildPositionGrant{character_, kPositionChoices[picked_]}));
        if (ui_ != nullptr) ui_->Close(this);
    }

private:
    void Show() {
        for (std::size_t i = 0; i < marks_.size(); ++i) marks_[i]->visible = int(i) == picked_;
        // The boxes' own ticks stay off: the marks say which one is chosen.
        for (int i = 0; i < 6; ++i) {
            if (auto* box = ui::As<ui::CheckBox>(Child(kFirstBox + i))) box->checked = false;
        }
    }

    static constexpr int kConfirm = 704, kCancel = 705, kFirstBox = 706, kFirstLabel = 712;
    static constexpr float kShiftLeft = 36.0f;
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::uint32_t character_ = 0;
    int picked_ = -1;
    std::vector<ui::Widget*> marks_;
};

// What each reason of a spending is called (the Reason of _GPHistory).
constexpr const char* kGpReasons[] = {
    "UIIT_STT_FORTRESS_BUFF",
    "UIIT_STT_FORTRESS_GP_HISTORY_STRUCTURE",
    "UIIT_STT_FORTRESS_GP_HISTORY_NPC_EMPLOYMENT",
    "UIIT_STT_FORTRESS_GP_HISTORY_GUARD_EMPLOYMENT",
    "UIIT_STT_FORTRESS_GP_HISTORY_FORTRESS_ITEM_CREATE",
    "UIIT_STT_FORTRESS_GP_HISTORY_GROWTH",
    "UIIT_STT_FORTRESS_GP_HISTORY_GUILD_ITEM_BUY",
    "UIIT_STT_FORTRESS_GP_HISTORY_DUNGEON",
    "UIIT_STT_FORTRESS_GP_HISTORY_ETC",
};

class GpHistoryWindow : public ui::MainFrame {
public:
    GpHistoryWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        SetText(c_.text ? c_.text("UIIT_STT_GUILD_GP_HISTORY") : std::string());
        MainFrame::Created(ui);
        ui_ = &ui;
        const auto* layout = c_.layout2dt ? c_.layout2dt("res_ui\\guild_gp.2dt") : nullptr;
        if (layout == nullptr || layout->empty()) return;
        BuildGuild2dt(ui, *layout, *this, layout->front().rect.x, layout->front().rect.y);
        if (auto* box = ui::As<ui::TextBox>(Child(kText))) {
            box->lineHeight = 16.0f;
            box->bar = dynamic_cast<ui::ScrollBar*>(Child(kScroll));
        }
        if (ui::Widget* ok = Child(kOk)) ok->SetText(c_.text("UIIT_CTL_CONFIRM"));
    }

    // "[%s] has spent [%d] GP for [%s]. (%d/%d/%d %d:%d)", a line each.
    void Fill(const proto::GuildGpHistory& history) {
        auto* box = ui::As<ui::TextBox>(Child(kText));
        if (box == nullptr || !c_.text) return;
        box->ClearParagraphs();
        const std::size_t reasons = std::size(kGpReasons);
        for (const proto::GuildGpRecord& r : history.rows) {
            const std::string why = c_.text(kGpReasons[std::min<std::size_t>(r.reason, reasons - 1)]);
            box->AddParagraph(FormatText(c_.text("UIIT_MSG_GUILD_GP_HISTORY"), {r.name, std::to_string(r.points), why, std::to_string(r.year), std::to_string(r.month),
                                                                                 std::to_string(r.day), std::to_string(r.hour), std::to_string(r.minute)}),
                              ui::kWhite);
        }
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.control == kOk && ui_ != nullptr) {
            ui_->Close(this);
            return true;
        }
        return MainFrame::OnNotice(n);
    }

private:
    static constexpr int kScroll = 953, kOk = 954, kText = 955;
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
};

template <typename T>
T* OpenOf(ui::Interface& ui) {
    for (ui::Widget* w : ui.Windows()) {
        if (auto* open = dynamic_cast<T*>(w)) return open;
    }
    return nullptr;
}

} // namespace

void RegisterGuildBoxWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CIFGuildNotifyWrite", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<NoticeWriteWindow>(n, *c); });
    ui.Register("CIFGuildPointUp", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<DonateWindow>(n, *c); });
    ui.Register("CIFGuildPositionGrant", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<PositionWindow>(n, *c); });
    ui.Register("CNIFGPHistoryWnd", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<GpHistoryWindow>(n, *c); });
}

void OpenGuildNoticeWrite(HudContext& c) {
    if (c.hud == nullptr) return;
    // The pencil again closes it.
    if (c.hud->Shown(kNoticeWindow)) {
        if (ui::Widget* open = c.hud->Show(kNoticeWindow)) open->visible = false;
        return;
    }
    if (auto* window = dynamic_cast<NoticeWriteWindow*>(c.hud->Show(kNoticeWindow))) window->Begin();
}

void OpenAcademyNoticeWrite(HudContext& c) {
    if (c.hud == nullptr) return;
    if (c.hud->Shown(kNoticeWindow)) {
        if (ui::Widget* open = c.hud->Show(kNoticeWindow)) open->visible = false;
        return;
    }
    if (auto* window = dynamic_cast<NoticeWriteWindow*>(c.hud->Show(kNoticeWindow))) window->Begin(true);
}

void OpenGuildDonate(HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr || OpenOf<DonateWindow>(*ui) != nullptr) return;
    assets::UiLayoutNode node;
    node.type = "CIFGuildPointUp";
    node.id = 1500;
    node.ddj = "interface\\messagebox\\msgbox2_window_";
    node.rect = {0.0f, 0.0f, 360.0f, 240.0f}; // ours: its layout's tile with the frame's margins
    auto* window = dynamic_cast<DonateWindow*>(ui->Open(node));
    if (window == nullptr) return;
    ui->Centre(*window, ui->screen());
    window->Begin();
}

void OpenGuildPosition(HudContext& c, std::uint32_t character) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    auto* window = OpenOf<PositionWindow>(*ui);
    if (window == nullptr) {
        assets::UiLayoutNode node;
        node.type = "CIFGuildPositionGrant";
        node.id = 1600;
        node.ddj = "interface\\messagebox\\msgbox2_window_";
        node.rect = {0.0f, 0.0f, 292.0f, 310.0f}; // ours: its layout's tile with the frame's margins
        window = dynamic_cast<PositionWindow*>(ui->Open(node));
        if (window == nullptr) return;
        ui->Centre(*window, ui->screen());
    }
    window->Begin(character);
}

bool PickGuildPosition(HudContext& c, std::uint8_t position, bool confirm) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    auto* window = ui != nullptr ? OpenOf<PositionWindow>(*ui) : nullptr;
    if (window == nullptr) return false;
    window->Pick(position);
    if (confirm) window->Confirm();
    return true;
}

void OpenGuildGpHistory(HudContext& c, const proto::GuildGpHistory& history) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr || !c.layout2dt) return;
    auto* window = OpenOf<GpHistoryWindow>(*ui);
    if (window == nullptr) {
        const auto* layout = c.layout2dt("res_ui\\guild_gp.2dt");
        if (layout == nullptr || layout->empty()) return;
        assets::UiLayoutNode node;
        node.type = "CNIFGPHistoryWnd";
        node.id = 186;
        node.ddj = layout->front().frame;
        node.rect = {0.0f, 0.0f, layout->front().rect.w, layout->front().rect.h};
        window = dynamic_cast<GpHistoryWindow*>(ui->Open(node));
        if (window == nullptr) return;
        ui->Centre(*window, ui->screen());
    }
    window->Fill(history);
}

bool FillGuildNotice(HudContext& c, const std::string& title, const std::string& body) {
    if (c.hud == nullptr || !c.hud->Shown(kNoticeWindow)) return false;
    auto* window = dynamic_cast<NoticeWriteWindow*>(c.hud->Show(kNoticeWindow));
    if (window == nullptr) return false;
    window->Fill(title, body);
    window->Confirm();
    return true;
}

bool FillGuildDonate(HudContext& c, std::uint32_t amount) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    auto* window = ui != nullptr ? OpenOf<DonateWindow>(*ui) : nullptr;
    if (window == nullptr) return false;
    window->Fill(amount);
    window->Press(true);
    return true;
}

} // namespace sr::client
