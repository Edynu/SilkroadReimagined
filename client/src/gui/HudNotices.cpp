// The HUD's notices: the system messages, the three banners, the
// system menu.

#include <algorithm>
#include <cmath>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/core/Log.h"

namespace sr::client {

namespace {

bool Inside(ui::Rect r, ui::Vec2 p) { return p.x >= r.x && p.y >= r.y && p.x <= r.Right() && p.y <= r.Bottom(); }

} // namespace

// --- System messages -----------------------------------------------------------------------------

void SystemMessages::Created(ui::Interface&) {
    box_ = ui::As<ui::TextBox>(Child(19));
    bar_ = ui::As<ui::ScrollBar>(Child(24));
    board_ = Child(50);
    if (box_ != nullptr) {
        // 14-pixel rows, a hundred paragraphs, the newest at the bottom, wrapped rows indented.
        box_->lineHeight = 14.0f;
        box_->maxParagraphs = 100;
        box_->bottomAnchored = true;
        box_->hangingIndent = true;
        box_->bar = bar_;
    }
    // The filter board starts closed, its boxes as the filter (without a saved choice: everything
    // but Combat; observed).
    if (board_ != nullptr) {
        board_->visible = false;
        for (int i = 0; i < 5; ++i) {
            if (auto* box = ui::As<ui::CheckBox>(board_->Child(10 + i))) box->checked = filter[std::size_t(i)];
        }
    }
    // The frame is shown from the start only when the option does not hide it.
    ShowFrame(!autohide);
}

bool SystemMessages::Passes(game::GameMessage::Kind kind) const {
    // General and the guide's tips always; the others by their box, Game for the rest.
    using K = game::GameMessage::Kind;
    switch (kind) {
    case K::General:
    case K::Guide: return true;
    case K::Acquisition: return filter[0];
    case K::Combat: return filter[1];
    case K::Status: return filter[2];
    case K::Party: return filter[3];
    case K::Game: return filter[4];
    }
    return filter[4];
}

void SystemMessages::SetLevel(int level) {
    // Observed:. 3 lines a level; the bottom 90 above the screen's, the right edge about
    // 7 from it.
    level_ = std::clamp(level, 1, 6);
    const float n = float(level_);
    rect.x = placedFor_.x - 357.0f;
    rect.y = placedFor_.y - 375.0f + (6.0f - n) * 42.0f;
    rect.h = 33.0f + 42.0f * n;
    if (box_ != nullptr) {
        box_->rect = {10.0f, 15.0f, 316.0f, 42.0f * n};
        box_->visibleRows = 3 * level_;
        box_->ScrollToEnd();
    }
    if (bar_ != nullptr) {
        bar_->rect.h = bar_->track = std::max(0.0f, 42.0f * n - 57.0f);
        bar_->SetRange(0, 0);
    }
    if (Widget* mid = Child(6)) mid->rect.h = 42.0f * n + 25.0f;
    if (Widget* bottom = Child(7)) bottom->rect.y = 42.0f * n + 29.0f;
    if (Widget* size = Child(15)) size->rect.y = 42.0f * n + 13.0f;
    ShowFrame(frameShown_);
}

void SystemMessages::ShowFrame(bool shown) {
    // The background, the buttons and the scroll bar; the lines are always drawn.
    frameShown_ = shown;
    for (int id : {5, 6, 7, 13, 15}) {
        if (Widget* w = Child(id)) w->visible = shown;
    }
    if (bar_ != nullptr) bar_->visible = shown && level_ != 1;
    if (!shown) boardOpen_ = false; // the board closes with the frame
    if (board_ != nullptr) board_->visible = shown && boardOpen_;
}

void SystemMessages::Update(ui::Frame& frame) {
    const ui::Vec2 screen = frame.interface.screen();
    if (screen.x != placedFor_.x || screen.y != placedFor_.y) {
        placedFor_ = screen;
        SetLevel(level_);
    }
    // The lines posted since the last frame, through the filter: tan, the guide's tips light blue.
    if (const game::PlayerState* state = context_.state; state != nullptr && box_ != nullptr) {
        const std::uint64_t fresh = std::min<std::uint64_t>(state->messagesPosted - seen_, state->messages.size());
        for (std::size_t i = state->messages.size() - std::size_t(fresh); i < state->messages.size(); ++i) {
            const game::GameMessage& m = state->messages[i];
            if (!Passes(m.kind)) continue;
            box_->AddParagraph(m.text, m.kind == game::GameMessage::Kind::Guide ? ui::Argb(0xFFBACFF2) : ui::Argb(0xFFDBC99B), ui::kTextShadow);
        }
        seen_ = state->messagesPosted;
    }
    // The frame while the mouse is over the window (or the open board); always without the option (the Game page's
    // "System UI Autohide").
    if (context_.options != nullptr) autohide = context_.options->game.Get("hideSystemMessages");
    const ui::Vec2 mouse = frame.pointer.position;
    const bool over = Inside(ScreenRect(), mouse) || (boardOpen_ && board_ != nullptr && Inside(board_->ScreenRect(), mouse));
    const bool shown = !autohide || over;
    if (shown != frameShown_) ShowFrame(shown);
    // The wheel: one row a notch.
    if (over && box_ != nullptr && frame.input.wheel() != 0.0f) box_->ScrollBy(frame.input.wheel() > 0.0f ? -1 : 1);
    // The buttons' help texts.
    if (context_.text && frameShown_) {
        if (Widget* b = Child(13); b != nullptr && frame.pointer.hovered == b) {
            frame.interface.ShowTooltip(context_.text("UIIT_STT_MESSAGE_FILTERING"), b->ScreenRect());
        }
        if (Widget* b = Child(15); b != nullptr && frame.pointer.hovered == b) {
            frame.interface.ShowTooltip(context_.text("UIIT_STT_TOGGLE_CHATING_WINDOW_SETING"), b->ScreenRect());
        }
    }
}

bool SystemMessages::OnNotice(const ui::Notice& notice) {
    if (notice.kind != ui::Notice::Kind::Clicked || notice.source == nullptr) return false;
    if (notice.source->parent() == this && notice.control == 13) {
        boardOpen_ = !boardOpen_;
        if (board_ != nullptr) board_->visible = frameShown_ && boardOpen_;
        return true;
    }
    if (notice.source->parent() == this && notice.control == 15) {
        SetLevel(level_ % 6 + 1); // after 6 back to 1
        return true;
    }
    if (board_ != nullptr && notice.source->parent() == board_ && notice.control >= 10 && notice.control <= 14) {
        if (auto* box = ui::As<ui::CheckBox>(notice.source)) filter[std::size_t(notice.control - 10)] = box->checked;
        return true;
    }
    return false;
}

// --- Banners -------------------------------------------------------------------------------------

void NoticeBanner::Created(ui::Interface&) {
    // Observed: (the three made by the HUD),.
    switch (id()) {
    case 35:
        kind_ = game::Banner::Kind::Warning;
        art_ = "interface\\ifcommon\\com_warning_";
        colour_ = ui::Rgba(0, 52, 92);
        top_ = 130.0f;
        break;
    case 42:
        kind_ = game::Banner::Kind::Quest;
        art_ = "interface\\ifcommon\\com_quest_";
        colour_ = ui::Rgba(0, 91, 66);
        top_ = 160.0f;
        break;
    default:
        kind_ = game::Banner::Kind::Notice;
        art_ = "interface\\ifcommon\\com_notice_";
        colour_ = ui::Rgba(128, 45, 67);
        top_ = 100.0f;
        break;
    }
    if (auto* text = ui::As<ui::Static>(Child(0))) {
        text->textStyle |= ui::kTextBold;
        text->visible = false;
    }
    // Shown, but nothing to show: no size (so it takes no clicks) until a text comes.
    rect = {0, top_, 0, 0};
}

void NoticeBanner::Update(ui::Frame& frame) {
    if (context_.state == nullptr) return;
    const game::Banner& banner = context_.state->banners[std::size_t(kind_)];
    auto* text = ui::As<ui::Static>(Child(0));
    if (text == nullptr) return;
    if (banner.time >= 0 && banner.time != shownTime_) {
        // A new text: the window as large as it, centred on the screen.
        shownTime_ = banner.time;
        text->SetText(banner.text);
        float width = 0.0f;
        int lines = 1;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= banner.text.size(); ++i) {
            if (i == banner.text.size() || banner.text[i] == '\n') {
                width = std::max(width, frame.paint.TextWidth(std::string_view(banner.text).substr(start, i - start), 0) + 1.0f);
                if (i < banner.text.size()) ++lines;
                start = i + 1;
            }
        }
        const float height = ui::TextLineHeight(0) * float(lines);
        const ui::Vec2 screen = frame.interface.screen();
        rect = {std::floor(screen.x / 2.0f) - std::floor(width / 2.0f), top_, std::ceil(width), height};
        text->rect = {0, 0, rect.w, rect.h};
        text->visible = true;
    }
    // Seven seconds: five at full, two fading.
    const double age = context_.time - shownTime_;
    if (shownTime_ < 0 || age >= 7.0) {
        text->visible = false;
        rect.w = rect.h = 0.0f;
        alpha = 1.0f;
        return;
    }
    alpha = age > 5.0 ? float((7.0 - age) / 2.0) : 1.0f;
}

void NoticeBanner::Draw(ui::Frame& frame) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) return;
    // A band of the colour at half alpha under the text, with soft ends 40 pixels out and 8 above
    // and below: the frame's textures carry only alpha (measured).
    const float a = Opacity();
    const ui::Rect r = ScreenRect();
    const float fill = std::max(0.0f, float(0x80) - (255.0f - a * 255.0f) / 2.0f) / 255.0f;
    frame.paint.Fill(r, ui::WithAlpha(colour_, fill));
    const ui::Color tint = ui::WithAlpha(ui::kWhite, a);
    const std::string corner = art_ + "corner.ddj", side = art_ + "edge2.ddj", edge = art_ + "edge.ddj";
    // The pieces face the top left; the others are mirrored.
    frame.paint.Image(corner, {r.x - 40, r.y - 8, 40, 8}, {0, 0}, {1, 1}, tint);
    frame.paint.Image(corner, {r.Right(), r.y - 8, 40, 8}, {1, 0}, {0, 1}, tint);
    frame.paint.Image(corner, {r.x - 40, r.Bottom(), 40, 8}, {0, 1}, {1, 0}, tint);
    frame.paint.Image(corner, {r.Right(), r.Bottom(), 40, 8}, {1, 1}, {0, 0}, tint);
    frame.paint.Image(side, {r.x - 40, r.y, 40, r.h}, {0, 0}, {1, 1}, tint);
    frame.paint.Image(side, {r.Right(), r.y, 40, r.h}, {1, 0}, {0, 1}, tint);
    frame.paint.Image(edge, {r.x, r.y - 8, r.w, 8}, {0, 0}, {1, 1}, tint);
    frame.paint.Image(edge, {r.x, r.Bottom(), r.w, 8}, {0, 1}, {1, 0}, tint);
}

// --- System menu ---------------------------------------------------------------------------------

void SystemMenu::Created(ui::Interface& ui) {
    MainFrame::Created(ui);
    // The customer centre's button goes; the other four stand 34 apart from y 58, and the window
    // shrinks to them.
    if (Widget* help = Child(12)) {
        help->visible = false;
        help->enabled = false;
    }
    float y = 58.0f;
    for (int id : {10, 11, 13, 14}) {
        if (Widget* b = Child(id)) {
            b->rect.x = 31.0f;
            b->rect.y = y;
            y += 34.0f;
        }
    }
    constexpr float kButtons = 4.0f;
    if (Widget* tile = Child(5)) tile->rect.h = 28.0f * kButtons;
    if (Widget* border = Child(6)) border->rect.h = 15.0f + 34.0f * kButtons;
    rect.h = 60.0f + 38.0f * kButtons;
}

bool SystemMenu::OnNotice(const ui::Notice& notice) {
    if (notice.kind == ui::Notice::Kind::Clicked && notice.source != nullptr && notice.source->parent() == this && context_.hud != nullptr) {
        switch (notice.control) {
        case 10:
            // The Option window; the menu closes before the window opens.
            visible = false;
            if (context_.hud != nullptr) context_.hud->OpenOptions();
            return true;
        case 11:
            visible = false;
            context_.hud->Show(hud::kGameGuide);
            return true;
        case 13: // Restart: no question asked; the server counts down
            visible = false;
            context_.hud->Request({HudRequest::Kind::Restart});
            return true;
        case 14:
            visible = false;
            context_.hud->Request({HudRequest::Kind::Quit});
            return true;
        default: break;
        }
    }
    return MainFrame::OnNotice(notice);
}

} // namespace sr::client
