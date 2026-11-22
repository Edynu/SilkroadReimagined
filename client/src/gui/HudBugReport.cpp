// The bug report window (ours; HudWindows.h BugReportWindow, the build's SR_BUG_REPORTS): opened by the button at the
// performance panel's left. The kind of bug (the website's bug categories), a subject, what happened, and the
// screenshot: taken on the frame after Send, with or without the interface, never with this window in it. Send hands it
// to HudContext::bugReport (the game screen: PlayBugReport.cpp); the server's answer comes as a notice in the chat.

#include <algorithm>
#include <cmath>
#include <string>

#include "Hud.h"
#include "HudWindows.h"
#include "sr/game/SupportProtocol.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

namespace sr::client {
namespace {

constexpr ui::Color kBack = ui::Rgba(10, 8, 6, 225);
constexpr ui::Color kEdge = ui::Rgba(150, 128, 82, 230); // the gold of the game's frames
constexpr ui::Color kField = ui::Rgba(0, 0, 0, 170);
constexpr ui::Color kLabel = ui::Rgba(239, 218, 163);   // the option pages' labels
constexpr ui::Color kTitle = ui::Rgba(255, 228, 160);
constexpr ui::Color kHint = ui::Rgba(170, 160, 135);
constexpr ui::Color kProblem = ui::Rgba(255, 120, 90);

// The controls' ids.
enum : int { kCategory = 10, kSubject = 11, kText = 12, kShot = 13, kWithUi = 14, kSend = 20, kCancel = 21 };

// The fields' places (the window's own pixels).
constexpr ui::Rect kSubjectBox{16.0f, 84.0f, 388.0f, 22.0f};
constexpr ui::Rect kTextBox{16.0f, 132.0f, 388.0f, 140.0f};

// The website's words for the kinds, as the window shows them.
constexpr const char* kCategoryNames[] = {"Gameplay", "Graphics", "Crash", "Connection", "Other"};
static_assert(std::size(kCategoryNames) == std::size(game::proto::kBugCategories));

ui::Widget* Make(ui::Interface& ui, ui::Widget& parent, const char* type, int id, ui::Rect rect, const std::string& ddj = {}, const std::string& text = {},
                 int hAlign = 0) {
    assets::UiLayoutNode n;
    n.type = type;
    n.name = "SR_BUGREPORT_MADE";
    n.id = id;
    n.rect = {rect.x, rect.y, rect.w, rect.h};
    n.ddj = ddj;
    n.text = text;
    n.hAlign = hAlign;
    auto made = ui.Make(n);
    ui::Widget* w = made.get();
    parent.Add(std::move(made));
    return w;
}

// Characters, not bytes (the server counts them so).
std::size_t Characters(const std::string& s) {
    return std::size_t(std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

std::string Trimmed(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

} // namespace

void BugReportWindow::Created(ui::Interface& ui) {
    ui_ = &ui;
    rect.w = kWidth;
    rect.h = kHeight;
    // The kind.
    if (auto* combo = ui::As<ui::ComboBox>(Make(ui, *this, "CIFComboBox", kCategory, {110.0f, 36.0f, 170.0f, 20.0f}))) {
        combo->items.assign(std::begin(kCategoryNames), std::end(kCategoryNames));
        combo->Select(category_);
    }
    // The subject and what happened (the website's limits).
    if (auto* subject = ui::As<ui::Edit>(Make(ui, *this, "CIFEdit", kSubject, {kSubjectBox.x + 4.0f, kSubjectBox.y + 4.0f, kSubjectBox.w - 8.0f, 14.0f}))) {
        subject->limit = game::proto::kBugSubjectChars;
    }
    if (auto* text = ui::As<ui::Edit>(Make(ui, *this, "CIFEdit", kText, {kTextBox.x + 4.0f, kTextBox.y + 4.0f, kTextBox.w - 8.0f, kTextBox.h - 8.0f}))) {
        text->multiline = true;
        text->limit = game::proto::kBugTextChars;
    }
    // The screenshot: taken or not, the interface in it or not.
    for (const auto& [id, y, label] : {std::tuple{kShot, 284.0f, "Attach a screenshot"}, std::tuple{kWithUi, 306.0f, "Show the interface in the screenshot"}}) {
        Make(ui, *this, "CIFCheckBox", id, {16.0f, y, 16.0f, 16.0f}, "interface\\ifcommon\\com_checkbutton_off.ddj");
        Make(ui, *this, "CIFStatic", id + 100, {38.0f, y + 2.0f, 300.0f, 14.0f})->SetText(label);
    }
    Make(ui, *this, "CIFButton", kSend, {kWidth - 196.0f, kHeight - 36.0f, 88.0f, 24.0f}, "interface\\ifcommon\\com_button.ddj", "Send", 1);
    Make(ui, *this, "CIFButton", kCancel, {kWidth - 102.0f, kHeight - 36.0f, 88.0f, 24.0f}, "interface\\ifcommon\\com_button.ddj", "Cancel", 1);
}

void BugReportWindow::Draw(ui::Frame& frame) {
    now_ = frame.time;
    const ui::Rect r = ScreenRect();
    const float a = Opacity();
    frame.paint.Fill(r, ui::WithAlpha(kBack, a));
    frame.paint.Outline(r, ui::WithAlpha(kEdge, a));
    frame.paint.Outline({r.x + 2.0f, r.y + 2.0f, r.w - 4.0f, r.h - 4.0f}, ui::WithAlpha(ui::Rgba(90, 74, 44, 200), a));
    const auto label = [&](std::string_view text, float x, float y, float w, ui::Color colour, int hAlign = 0) {
        ui::DrawLabel(frame.paint, text, {r.x + x, r.y + y, w, 14.0f}, ui::WithAlpha(colour, a), 0, hAlign, 1, ui::kTextShadow);
    };
    label("Report a bug", 0.0f, 10.0f, r.w, kTitle, 1);
    frame.paint.Fill({r.x + 16.0f, r.y + 28.0f, r.w - 32.0f, 1.0f}, ui::WithAlpha(kEdge, a * 0.6f));
    label("Kind of bug", 16.0f, 39.0f, 90.0f, kLabel);
    label("Subject", 16.0f, 66.0f, 200.0f, kLabel);
    label("What happened? What did you do before it?", 16.0f, 114.0f, 388.0f, kLabel);
    // The fields' wells behind their edits.
    for (const ui::Rect& box : {kSubjectBox, kTextBox}) {
        const ui::Rect s{r.x + box.x, r.y + box.y, box.w, box.h};
        frame.paint.Fill(s, ui::WithAlpha(kField, a));
        frame.paint.Outline(s, ui::WithAlpha(ui::Rgba(110, 94, 60, 220), a));
    }
    // What the counts allow, under the text.
    if (const auto* text = ui::As<ui::Edit>(Child(kText))) {
        label(std::to_string(Characters(text->text())) + " / " + std::to_string(game::proto::kBugTextChars), 16.0f, 274.0f - 0.0f, 388.0f, kHint, 2);
    }
    label("The report window itself is never in the screenshot.", 38.0f, 324.0f, 340.0f, kHint);
    if (!problem_.empty() && now_ - problemAt_ < 6.0) label(problem_, 16.0f, kHeight - 31.0f, kWidth - 220.0f, kProblem);
}

void BugReportWindow::Mouse(ui::Frame& frame) {
    // A press on the window's own face (not a control) takes the focus from the fields: nothing more.
    (void)frame;
}

bool BugReportWindow::OnNotice(const ui::Notice& notice) {
    if (notice.kind == ui::Notice::Kind::ComboPicked && notice.control == kCategory) {
        category_ = std::clamp(notice.value, 0, int(std::size(kCategoryNames)) - 1);
        return true;
    }
    if (notice.kind == ui::Notice::Kind::Clicked && notice.control == kShot) {
        // Without a screenshot, the interface's box means nothing.
        const auto* shot = ui::As<ui::CheckBox>(Child(kShot));
        if (ui::Widget* withUi = Child(kWithUi)) withUi->enabled = shot == nullptr || shot->checked;
        return true;
    }
    if (notice.kind != ui::Notice::Kind::Clicked) return false;
    if (notice.control == kSend) {
        Send();
        return true;
    }
    if (notice.control == kCancel) {
        if (ui_ != nullptr) ui_->Close(this);
        return true;
    }
    return false;
}

void BugReportWindow::Send() {
    const auto* subject = ui::As<ui::Edit>(Child(kSubject));
    const auto* text = ui::As<ui::Edit>(Child(kText));
    BugReportForm form;
    form.category = category_;
    form.subject = subject != nullptr ? Trimmed(subject->text()) : std::string();
    form.text = text != nullptr ? Trimmed(text->text()) : std::string();
    if (const auto* shot = ui::As<ui::CheckBox>(Child(kShot))) form.screenshot = shot->checked;
    if (const auto* withUi = ui::As<ui::CheckBox>(Child(kWithUi))) form.withInterface = withUi->checked;
    // The website's limits, said here rather than by the server's refusal.
    const std::size_t s = Characters(form.subject), t = Characters(form.text);
    if (s < 3) {
        problem_ = "Give it a subject (3 characters at least).";
    } else if (t < 10) {
        problem_ = "Say what happened (10 characters at least).";
    } else if (!context_.bugReport) {
        problem_ = "Bug reports cannot be sent now.";
    } else {
        problem_.clear();
        // Closed first: the screenshot is taken on a later frame, without it.
        auto send = context_.bugReport;
        if (ui_ != nullptr) ui_->Close(this);
        send(std::move(form));
        return;
    }
    problemAt_ = now_;
}

void ToggleBugReport(ui::Interface& ui, HudContext& context) {
    if (ui::Widget* open = ui.Window(hud::kBugReport); open != nullptr && open->visible) {
        ui.Close(open);
        return;
    }
    (void)context;
    assets::UiLayoutNode node;
    node.type = "SRBugReport";
    node.name = "SRBugReport";
    node.id = hud::kBugReport;
    node.rect = {0.0f, 0.0f, BugReportWindow::kWidth, BugReportWindow::kHeight};
    if (ui::Widget* w = ui.Open(node)) ui.Centre(*w, ui.screen());
}

} // namespace sr::client
