// Capture the Flag's windows: the event has no window of its own in
// the 1.188 client; it uses the event timer (CIFTimerWnd, window 410), one message box (the Subscription Card's, kind
// 0x2A), the camp countdown (CIFTimeDrivenWnd) after the death box's camp button, and the action bar under a head.

#include <cmath>
#include <format>
#include <memory>

#include "gui/Hud.h"
#include "gui/HudWindows.h"

namespace sr::client {

namespace {

constexpr int kCampCountdown = 0x7538; // its window id
constexpr int kInformText = 301;       // ours: MsgBoxINIF's first static (id 1) in the box

// CIFTimerWnd (iftimerwnd.txt, ginterface.txt's window 410): MM:SS in the digit pictures interface\timer\timer_N.ddj
// (statics 1, 2 the minutes, 4, 5 the seconds) with timer.ddj (3) between, refreshed each second. The
// CTF only sets its time and starts it: its other mode (the last minute in red with a sound) and
// its count-in pictures (11..13, interface\gngwc\, absent from 1.188's Media.pk2) are other events'. Run out, it shows
// its "end" picture for a second (absent: nothing).
class EventTimer : public ui::Widget {
public:
    EventTimer(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}
    void Created(ui::Interface&) override {
        clickThrough = true;
        for (int id : {10, 11, 12, 13}) {
            if (ui::Widget* w = Child(id)) w->visible = false;
        }
        // The layout's statics carry the text "0" (a placeholder the original's pictures cover; ours would print it).
        for (int id : {1, 2, 3, 4, 5}) {
            if (ui::Widget* w = Child(id)) w->SetText({});
        }
    }
    void Update(ui::Frame&) override {
        // The Battle Arena's timer when it runs (the same CIFTimerWnd), else Capture the Flag's.
        const bool arena = context_.arena != nullptr && context_.arena->TimerShown(context_.time);
        if (!arena && context_.flag == nullptr) return;
        const double left = arena ? context_.arena->TimerLeft(context_.time) : context_.flag->TimerLeft(context_.time);
        const std::string text = game::FlagView::TimerText(left); // "MM:SS"
        constexpr int kDigits[4] = {1, 2, 4, 5};
        constexpr std::size_t kAt[4] = {0, 1, 3, 4};
        for (int i = 0; i < 4; ++i) {
            if (auto* digit = ui::As<ui::Static>(Child(kDigits[i]))) {
                digit->texture = std::format("interface\\timer\\timer_{}.ddj", text[kAt[i]]);
                digit->visible = left > 0.0;
            }
        }
        if (ui::Widget* colon = Child(3)) colon->visible = left > 0.0;
    }

private:
    HudContext& context_;
};

// The camp countdown: a CIFTimeDrivenWnd (iftimedrivenwnd.txt, the frame
// interface\frame\mframe_wnd_, the title UIIT_STT_EVENTGUIDE "Alarm"), its text each second "%d%s%s" of the seconds left,
// PARAM_SECOND and UIIT_MSG_FLAGWAR_GO_TO_ZONE run together, from 10 down; no buttons (its Yes / Cancel
// need flags it is not given); at 0 it closes itself. Ours: its size and place (sized to its text, not traced to the
// pixel): the text with 32 on each side, under the title; above the death box's place.
class CampCountdown : public ui::FrameBorder {
public:
    CampCountdown(const assets::UiLayoutNode& node, HudContext& context) : FrameBorder(node), context_(context) {}
    void Created(ui::Interface& ui) override {
        ui_ = &ui;
        started_ = context_.time;
        for (int id : {11, 13, 14, 15}) {
            if (ui::Widget* w = Child(id)) w->visible = false; // the layout's text, buttons and title: ours below
        }
        for (const auto& [id, key] : {std::pair{115, "UIIT_STT_EVENTGUIDE"}, std::pair{111, ""}}) {
            assets::UiLayoutNode n;
            n.type = "CIFStatic";
            n.id = id;
            n.text = key;
            n.hAlign = 1;
            n.vAlign = id == 111 ? 1 : 0;
            Add(ui.Make(n));
        }
        Arrange(rect.w);
    }
    void Update(ui::Frame& frame) override {
        const int left = game::FlagView::kCampSeconds - int(std::floor(context_.time - started_));
        if (left <= 0) {
            if (ui_ != nullptr) ui_->Close(this);
            return;
        }
        const std::string text = std::format("{}{}{}", left, context_.Text("PARAM_SECOND"), context_.Text("UIIT_MSG_FLAGWAR_GO_TO_ZONE"));
        if (ui::Widget* message = Child(111)) message->SetText(text);
        const float width = std::max(240.0f, std::ceil(frame.paint.TextWidth(text, 0)) + 64.0f);
        if (width != rect.w) {
            rect.x += std::floor((rect.w - width) / 2.0f);
            Arrange(width);
        }
    }

private:
    void Arrange(float width) {
        rect.w = width;
        if (ui::Widget* w = Child(16)) w->rect = {10.0f, 0.0f, width - 21.0f, 34.0f}; // the strip to drag it by
        if (ui::Widget* w = Child(12)) w->rect = {16.0f, 40.0f, width - 32.0f, rect.h - 56.0f};
        if (ui::Widget* w = Child(115)) w->rect = {10.0f, 12.0f, width - 21.0f, 12.0f};
        if (ui::Widget* w = Child(111)) w->rect = {16.0f, 40.0f, width - 32.0f, rect.h - 56.0f};
    }
    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    double started_ = 0.0;
};

} // namespace

void InformBox::Created(ui::Interface& ui) {
    ui::MainFrame::Created(ui);
    ui_ = &ui;
    // The box's own dark tile within its size (the layout's is laid out for 316 x 178).
    for (const auto& c : children()) {
        if (c->node().type == "CIFNormalTile") c->rect = {16.0f, 40.0f, rect.w - 32.0f, rect.h - 56.0f};
    }
    if (const assets::UiLayout* layout = ui.Layout("resinfo\\ifmessagebox.txt")) {
        for (const auto& s : layout->sections) {
            if (s.name != "MsgBoxINIF") continue;
            for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) {
                assets::UiLayoutNode n = *it;
                if (n.id == 1) {
                    n.id = kInformText; // the frame's own drag strip is 1
                    n.hAlign = 1;       // the text, centred
                }
                Add(ui.Make(n));
            }
        }
    }
    for (int id : {200, 201}) {
        if (ui::Widget* b = Child(id)) b->visible = false; // kind 0x2A: Confirm alone
    }
}

void InformBox::Say(const std::string& text) {
    if (ui::Widget* line = Child(kInformText)) line->SetText(text);
}

bool InformBox::OnNotice(const ui::Notice& notice) {
    const bool mine = notice.kind == ui::Notice::Kind::Clicked && notice.source != nullptr && notice.source->parent() == this;
    if (!mine || (notice.control != 202 && notice.control != 2)) return ui::MainFrame::OnNotice(notice);
    if (ui_ != nullptr) ui_->Close(this);
    return true;
}

void RegisterFlagWorldWindows(ui::Interface& ui, HudContext& context) {
    HudContext* c = &context;
    ui.Register("CIFTimerWnd", [c](const assets::UiLayoutNode& n) { return std::make_unique<EventTimer>(n, *c); });
    ui.Register("CIFTimeDrivenWnd", [c](const assets::UiLayoutNode& n) { return std::make_unique<CampCountdown>(n, *c); });
}

bool CampCountdownShown(const HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return false;
    for (ui::Widget* w : ui->Windows()) {
        if (dynamic_cast<CampCountdown*>(w) != nullptr && w->visible) return true;
    }
    return false;
}

void OpenCampCountdown(HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr || CampCountdownShown(c)) return; // one at a time (1.188's global)
    const ui::Vec2 screen = c.hud->Screen();
    constexpr float kWidth = 240.0f, kHeight = 96.0f;
    assets::UiLayoutNode node;
    node.type = "CIFTimeDrivenWnd";
    node.name = "GDR_FLAGWAR_CAMP_COUNTDOWN";
    node.id = kCampCountdown;
    node.ddj = "interface\\frame\\mframe_wnd_";
    node.rect = {std::floor((screen.x - kWidth) / 2.0f), std::floor(screen.y / 2.0f) - 74.0f - kHeight - 16.0f, kWidth, kHeight};
    ui->Open(node);
}

void ShowSubscribedBox(HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    // Made in code at the screen's centre (x = W/2 - 150, y = H/2 - 72), 308 x 144, its section MsgBoxINIF
    // (kind 0x2A): "Confirmation window", the text, Confirm.
    const ui::Vec2 screen = c.hud->Screen();
    assets::UiLayoutNode node;
    node.type = "CIFMessageBox";
    node.name = "GDR_MSGBOX_FLAGWAR_EVENT_JOIN";
    node.ddj = "interface\\messagebox\\msgbox2_window_";
    node.text = "UIIT_STT_CONFIRM_BOX";
    node.subSection = "MsgBoxINIF";
    node.rect = {std::floor(screen.x / 2.0f) - 150.0f, std::floor(screen.y / 2.0f) - 72.0f, 308.0f, 144.0f};
    if (auto* box = dynamic_cast<InformBox*>(ui->Open(node))) box->Say(c.Text("UIIT_MSG_FLAGWAR_EVENT_JOIN"));
}

bool AnswerInformBox(HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return false;
    for (ui::Widget* w : ui->Windows()) {
        if (dynamic_cast<InformBox*>(w) != nullptr && w->visible) {
            ui->Close(w);
            return true;
        }
    }
    return false;
}

void DrawActionBar(ui::Painter& paint, float x, float y, double elapsed, int seconds) {
    // From x - 24 to x - 24 + seconds x 4.8 - int(tenths x 0.48), y + 10 to y + 12, opaque yellow.
    const int tenths = int(elapsed * 10.0);
    const float width = float(seconds) * 4.8f - float(int(float(tenths) * 0.48f));
    if (width > 0.0f) paint.Fill({std::floor(x - 24.0f), std::floor(y + 10.0f), width, 2.0f}, ui::Argb(0xFFFFEE1F));
}

} // namespace sr::client
