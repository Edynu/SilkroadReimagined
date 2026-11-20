// The interface's warm-up list (client/src/gui/UiWarmupList.h): what a run drew, aged run by run, read back in order.

#include <doctest/doctest.h>

#include <string>

#include "gui/UiWarmupList.h"

using sr::client::UiWarmupList;

TEST_CASE("the ui warm-up list: drawn this run first, the rest aged, forgotten after a few runs") {
    UiWarmupList list;
    list.Decode("0\tinterface\\ifcommon\\com_button.ddj\r\n2\tminimap\\100x90.ddj\n" // CRLF and LF both read
                "4\tinterface\\old.ddj\nnot a line\n-1\tinterface\\bad.ddj\n\t\n3\t\n");
    REQUIRE(list.size() == 3);
    // Read back most recent first.
    const auto paths = list.Paths();
    REQUIRE(paths.size() == 3);
    CHECK(paths[0] == "interface\\ifcommon\\com_button.ddj");
    CHECK(paths[1] == "minimap\\100x90.ddj");
    CHECK(paths[2] == "interface\\old.ddj");

    // This run draws the minimap tile (another spelling of it) and a new picture.
    list.Note("Minimap/100x90.ddj");
    list.Note("interface\\worldmap\\map\\world_0_0.ddj");
    list.Note("interface\\worldmap\\map\\world_0_0.ddj"); // once is enough
    CHECK(list.drawnThisRun() == 2);
    const std::string next = list.Encode();
    // Drawn now: 0; the button not drawn: a run older; the old one past kForgetAfter: gone.
    CHECK(next.find("0\tminimap\\100x90.ddj\n") != std::string::npos);
    CHECK(next.find("0\tinterface\\worldmap\\map\\world_0_0.ddj\n") != std::string::npos);
    CHECK(next.find("1\tinterface\\ifcommon\\com_button.ddj\n") != std::string::npos);
    CHECK(next.find("old.ddj") == std::string::npos);

    // Read again by the next run: the same three, the ones drawn last first.
    UiWarmupList again;
    again.Decode(next);
    REQUIRE(again.size() == 3);
    CHECK(again.Paths().back() == "interface\\ifcommon\\com_button.ddj");
}

TEST_CASE("the ui warm-up list keeps at most kMaxEntries, the most recently drawn") {
    UiWarmupList list;
    std::string text;
    for (std::size_t i = 0; i < UiWarmupList::kMaxEntries + 10; ++i) text += "1\tinterface\\x" + std::to_string(i) + ".ddj\n";
    list.Decode(text);
    list.Note("interface\\fresh.ddj");
    UiWarmupList next;
    next.Decode(list.Encode());
    CHECK(next.size() == UiWarmupList::kMaxEntries);
    CHECK(next.Paths().front() == "interface\\fresh.ddj"); // drawn this run: kept, and first
}
