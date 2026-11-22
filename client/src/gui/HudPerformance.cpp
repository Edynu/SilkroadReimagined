// The performance panel (ours; HudWindows.h PerformancePanel): frames a second, the frame times and the game
// connection's latency and traffic, in a small box drawn with the game's own font. One line collapsed, its header
// clicked to expand or collapse it.

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "HudWindows.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

namespace sr::client {
namespace {

constexpr ui::Color kBack = ui::Rgba(0, 0, 0, 160);
constexpr ui::Color kEdge = ui::Rgba(150, 128, 82, 220); // the gold of the game's frames
constexpr ui::Color kLabel = ui::Rgba(200, 190, 160);
constexpr ui::Color kGood = ui::Rgba(120, 230, 110), kFair = ui::Rgba(255, 220, 90), kPoor = ui::Rgba(255, 110, 90);

// Frames a second: green from 55, yellow from 30, red below.
ui::Color FpsColour(double fps) { return fps >= 55.0 ? kGood : fps >= 30.0 ? kFair : kPoor; }
// A round trip: green to 80 ms, yellow to 180 ms, red above.
ui::Color LatencyColour(double ms) { return ms < 0.0 ? kLabel : ms <= 80.0 ? kGood : ms <= 180.0 ? kFair : kPoor; }

std::string Rate(double bytes) {
    return bytes >= 1024.0 * 1024.0 ? std::format("{:.1f} MB/s", bytes / (1024.0 * 1024.0)) : std::format("{:.1f} KB/s", bytes / 1024.0);
}
std::string Total(std::uint64_t bytes) {
    return bytes >= 1024ull * 1024ull ? std::format("{:.1f} MB", double(bytes) / (1024.0 * 1024.0)) : std::format("{:.0f} KB", double(bytes) / 1024.0);
}

} // namespace

float PerformancePanel::RightGap(std::size_t coses) {
    constexpr float kMinimap = 129.0f, kGap = 6.0f;
    if (coses == 0) return kMinimap + kGap;
    // The leftmost status icon's left edge, its outline included (HudCos.cpp kStatusRight, kStatusStep).
    return std::max(kMinimap, 172.0f + 48.0f * float(coses - 1) + 11.0f) + kGap;
}

void PerformancePanel::Update(ui::Frame& frame) {
    const float button = HasButton() ? kButton + kButtonGap : 0.0f;
    rect.w = button + kWidth;
    rect.h = kHeader + (expanded ? float(kExpandedLines) * kLine + 4.0f : 0.0f);
    // At the top, left of the minimap and of the COS's status icons out now (a pet's would hide it).
    const ui::Vec2 screen = frame.interface.screen();
    rect.x = std::floor(screen.x - RightGap(context_.coses.size()) - rect.w);
    rect.y = 4.0f;
}

ui::Rect PerformancePanel::ButtonRect() const {
    const ui::Rect r = ScreenRect();
    return {r.x, r.y - 1.0f, kButton, kButton};
}

void PerformancePanel::Mouse(ui::Frame& frame) {
    if (frame.pointer.hovered != this || !frame.input.Pressed(ui::MouseButton::Left)) return;
    const ui::Vec2 p = frame.pointer.position;
    if (HasButton()) {
        const ui::Rect b = ButtonRect();
        if (p.x >= b.x && p.x <= b.Right() && p.y >= b.y && p.y <= b.Bottom()) {
#ifdef SR_BUG_REPORTS
            if (ui_ != nullptr) ToggleBugReport(*ui_, context_);
#endif
            return;
        }
        if (p.x < b.Right() + kButtonGap) return;
    }
    // A press on the header (the top line) expands or collapses it.
    const ui::Rect r = ScreenRect();
    if (p.y <= r.y + kHeader) expanded = !expanded;
}

void PerformancePanel::Draw(ui::Frame& frame) {
    const float a = Opacity();
    ui::Rect r = ScreenRect();
    if (HasButton()) {
        // The bug report's button: the game's bug icon in a box like the panel's, lit under the mouse.
        const ui::Rect b = ButtonRect();
        const ui::Vec2 m = frame.pointer.position;
        const bool over = frame.pointer.hovered == this && m.x >= b.x && m.x <= b.Right() && m.y >= b.y && m.y <= b.Bottom();
        frame.paint.Fill(b, ui::WithAlpha(over ? ui::Rgba(60, 48, 24, 220) : kBack, a));
        frame.paint.Outline(b, ui::WithAlpha(over ? ui::Rgba(240, 210, 130) : kEdge, a));
        frame.paint.Image("icon\\item\\etc\\etc_bug_earwig.ddj", {b.x + 1.0f, b.y + 1.0f, b.w - 2.0f, b.h - 2.0f}, {0, 0}, {1, 1},
                          ui::WithAlpha(ui::kWhite, a));
        if (over) frame.interface.ShowTooltip("Report a bug", b);
        r.x += kButton + kButtonGap;
        r.w -= kButton + kButtonGap;
    }
    frame.paint.Fill(r, ui::WithAlpha(kBack, a));
    frame.paint.Outline(r, ui::WithAlpha(kEdge, a));

    const HudPerformance* p = context_.performance;
    const auto label = [&](std::string_view text, float x, float y, float w, ui::Color colour, int hAlign = 0) {
        ui::DrawLabel(frame.paint, text, {x, y, w, kLine}, ui::WithAlpha(colour, a), 0, hAlign, 1, ui::kTextShadow);
    };

    // The header: the frames a second and the latency, the sign that expands or collapses it.
    const float x = r.x + 6.0f, w = r.w - 12.0f;
    float y = r.y + 1.0f;
    if (p == nullptr) {
        label("FPS -", x, y, w, kLabel);
    } else {
        const std::string fps = std::format("{:.0f} FPS", p->fps);
        label(fps, x, y, 70.0f, FpsColour(p->fps));
        const std::string latency = !p->connected ? std::string("offline") : p->latencyMs < 0.0 ? std::string("- ms")
                                                                                                : std::format("{:.0f} ms", p->latencyMs);
        label(latency, x + 70.0f, y, 80.0f, p->connected ? LatencyColour(p->latencyMs) : kPoor);
    }
    label(expanded ? "-" : "+", x, y, w, kLabel, 2);
    if (!expanded || p == nullptr) return;

    // The details, a label and its value on each line.
    y = r.y + kHeader + 2.0f;
    const auto line = [&](std::string_view name, const std::string& value, ui::Color colour) {
        label(name, x, y, 60.0f, kLabel);
        label(value, x + 60.0f, y, w - 60.0f, colour);
        y += kLine;
    };
    line("Frame", std::format("{:.1f} ms", p->frameMs), FpsColour(p->fps));
    line("Worst", std::format("{:.1f} ms", p->worstMs), p->worstMs <= 33.4 ? kGood : p->worstMs <= 66.7 ? kFair : kPoor);
    line("Latency", p->latencyMs < 0.0 ? std::string("walk to measure") : std::format("{:.0f} ms (a walk's round trip)", p->latencyMs),
         LatencyColour(p->latencyMs));
    line("In", std::format("{}  {:.0f} msg/s", Rate(p->bytesIn), p->messagesIn), ui::kWhite);
    line("Out", std::format("{}  {:.0f} msg/s", Rate(p->bytesOut), p->messagesOut), ui::kWhite);
    line("Total", std::format("{} in, {} out", Total(p->totalIn), Total(p->totalOut)), ui::kWhite);
}

} // namespace sr::client
