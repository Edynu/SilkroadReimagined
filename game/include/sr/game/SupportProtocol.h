#pragma once

// A player's bug report from the game (ours, 0x1480-0x149F; the build's SR_BUG_REPORTS): the report, then its screenshot
// in parts, to the game server, which keeps it as one of the website's support tickets (sr_support_tickets, source
// "client"; the screenshot in sr_support_files). The server answers in the chat (a notice line): the ticket's number,
// or why it was not taken. Requests only, from the client.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sr/game/Protocol.h"

namespace sr::game::proto {

// The report's limits: the subject and text as the website's form takes them (characters), the details the client adds,
// the screenshot (a JPEG) and the parts it goes in (within a frame's 64 KB).
constexpr std::size_t kBugSubjectChars = 120;
constexpr std::size_t kBugTextChars = 5000;
constexpr std::size_t kBugInfoBytes = 2000;
constexpr std::uint32_t kBugImageMax = 4u << 20;
constexpr std::size_t kBugPartBytes = 48 * 1024;
// The client's pace for the parts (the servers' message budget is 50 a second, PeerLimits).
constexpr int kBugPartsPerSecond = 20;

// The website's bug categories (SroWeb's SupportCategories("bug")), the window's choice by index.
inline constexpr std::string_view kBugCategories[] = {"gameplay", "graphics", "crash", "connection", "other"};

struct BugReport {
    static constexpr MessageId kId = 0x1480;
    std::uint8_t category = 0; // kBugCategories
    std::string subject;
    std::string text;
    std::string info;            // the client's version, system and settings (one per line)
    std::uint32_t imageBytes = 0; // the screenshot's size (0: none); its parts follow
    template <typename V>
    void Visit(V& v) {
        v("category", category);
        v("subject", subject, kBugSubjectChars * 4);
        v("text", text, kBugTextChars * 4);
        v("info", info, kBugInfoBytes);
        v("imageBytes", imageBytes);
    }
};

// The next part of the report's screenshot, in order; the report is taken once its last part is in.
struct BugReportPart {
    static constexpr MessageId kId = 0x1481;
    std::vector<std::uint8_t> bytes;
    template <typename V>
    void Visit(V& v) {
        v("bytes", bytes, kBugPartBytes);
    }
};

// The bug reports' range (0x1480-0x149F).
inline bool IsSupportMessage(MessageId id) { return id >= 0x1480 && id < 0x14A0; }

} // namespace sr::game::proto
