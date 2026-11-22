// A bug report from the game (ours; the build's SR_BUG_REPORTS; HudBugReport.cpp is its window): the report window's Send
// gives the form here. Its screenshot is taken on the frame after (the window already closed; the interface left out
// when the player asked so), written by the render thread as a JPEG into the temporary folder, read back once there,
// then the report goes to the game server with the screenshot in parts at the servers' pace (SupportProtocol.h). The
// server keeps it as a support ticket on the website and answers with a notice in the chat.

#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

#include "play/PlayScreen.h"
#include "sr/core/Log.h"
#include "sr/game/SupportProtocol.h"

#ifndef SR_CLIENT_VERSION
#define SR_CLIENT_VERSION "unknown"
#endif

namespace sr::client {
namespace proto = game::proto;

namespace {

constexpr double kShotWaitSeconds = 10.0; // the render thread's file, at most this long

// A line in the game's messages (the server's answer comes in the chat).
void Notice(game::PlayerState& player, std::string text, double time) { player.Post(game::GameMessage::Kind::General, std::move(text), time); }

} // namespace

void PlayScreen::StartBugReport(BugReportForm form) {
    if (bugReport_) return;
    BugReportOut out;
    out.form = std::move(form);
    out.stage = out.form.screenshot ? BugReportOut::Stage::Capture : BugReportOut::Stage::Sending;
    bugReport_ = std::move(out);
    Notice(player_, "Sending your bug report...", now_);
}

void PlayScreen::BugReportFrame(ScreenContext& context, FrameSnapshot& snapshot) {
    if (!bugReport_) return;
    BugReportOut& r = *bugReport_;
    switch (r.stage) {
    case BugReportOut::Stage::Capture: {
        // This frame is the screenshot (the window closed on the frame before): the interface out unless asked for.
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "SroReimagined";
        std::filesystem::create_directories(dir, ec);
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        r.shot = dir / std::format("bugreport-{}.jpg", stamp);
        snapshot.capture = r.shot;
        snapshot.captureGoesOn = true;
        if (!r.form.withInterface) {
            snapshot.ui.Clear();
            snapshot.uiUnder.Clear();
            snapshot.drawImgui = false;
        }
        r.since = context.time;
        r.stage = BugReportOut::Stage::Waiting;
        return;
    }
    case BugReportOut::Stage::Waiting: {
        std::error_code ec;
        if (std::filesystem::exists(r.shot, ec)) {
            std::ifstream in(r.shot, std::ios::binary);
            r.image.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            in.close();
            std::filesystem::remove(r.shot, ec);
            if (r.image.size() > proto::kBugImageMax) {
                core::LogWarning("bug report: the screenshot is {} KB, over the {} KB the server takes: sent without it", r.image.size() / 1024,
                                 proto::kBugImageMax / 1024);
                Notice(player_, "The screenshot was too large; the report goes without it.", context.time);
                r.image.clear();
            }
            r.stage = BugReportOut::Stage::Sending;
        } else if (context.time - r.since > kShotWaitSeconds) {
            core::LogWarning("bug report: no screenshot after {} s: sent without it", kShotWaitSeconds);
            Notice(player_, "The screenshot could not be taken; the report goes without it.", context.time);
            r.stage = BugReportOut::Stage::Sending;
        }
        return;
    }
    case BugReportOut::Stage::Sending: break;
    }
    if (r.sent == 0 && r.nextPart == 0.0) {
        // The report first: what the player wrote, and what the client knows of itself.
        proto::BugReport report;
        report.category = std::uint8_t(std::clamp(r.form.category, 0, int(std::size(proto::kBugCategories)) - 1));
        report.subject = r.form.subject;
        report.text = r.form.text;
#if defined(_WIN32)
        constexpr const char* kSystem = "Windows";
#elif defined(__linux__)
        constexpr const char* kSystem = "Linux";
#else
        constexpr const char* kSystem = "other";
#endif
        report.info = std::format("client: {}\nsystem: {}\nGPU: {}\nscreen: {} x {}\nframes: {:.0f} a second (worst {:.1f} ms)\nlatency: {}\nscreenshot: {}",
                                  SR_CLIENT_VERSION, kSystem, context.device.AdapterName(), context.device.SwapchainWidth(),
                                  context.device.SwapchainHeight(), performance_.fps, performance_.worstMs,
                                  performance_.latencyMs < 0.0 ? std::string("not measured") : std::format("{:.0f} ms", performance_.latencyMs),
                                  !r.form.screenshot ? "none" : r.image.empty() ? "failed" : r.form.withInterface ? "with the interface" : "the scene alone");
        report.imageBytes = std::uint32_t(r.image.size());
        shared_->session->SendGame(proto::BugReport::kId, proto::Encode(report));
        r.nextPart = context.time;
    }
    // Its parts at the servers' pace.
    while (r.sent < r.image.size() && context.time >= r.nextPart) {
        proto::BugReportPart part;
        const std::size_t n = std::min(proto::kBugPartBytes, r.image.size() - r.sent);
        part.bytes.assign(r.image.begin() + std::ptrdiff_t(r.sent), r.image.begin() + std::ptrdiff_t(r.sent + n));
        shared_->session->SendGame(proto::BugReportPart::kId, proto::Encode(part));
        r.sent += n;
        r.nextPart += 1.0 / proto::kBugPartsPerSecond;
    }
    if (r.sent >= r.image.size()) {
        core::LogInfo("bug report: sent ({} KB of screenshot)", r.image.size() / 1024);
        bugReport_.reset();
    }
}

} // namespace sr::client
