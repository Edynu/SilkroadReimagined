// The performance panel (ours; HudWindows.h PerformancePanel): frames a second, the frame times and the game
// connection's latency and traffic, in a small box drawn with the game's own font. One line collapsed, its header
// clicked to expand or collapse it.

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

void PerformancePanel::Update(ui::Frame&) {
    rect.w = kWidth;
    rect.h = kHeader + (expanded ? float(kExpandedLines) * kLine + 4.0f : 0.0f);
}

void PerformancePanel::Mouse(ui::Frame& frame) {
    // A press on the header (the top line) expands or collapses it.
    if (frame.pointer.hovered != this || !frame.input.Pressed(ui::MouseButton::Left)) return;
    const ui::Rect r = ScreenRect();
    if (frame.pointer.position.y <= r.y + kHeader) expanded = !expanded;
}

void PerformancePanel::Draw(ui::Frame& frame) {
    const ui::Rect r = ScreenRect();
    const float a = Opacity();
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
