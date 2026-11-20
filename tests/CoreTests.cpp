#include <doctest/doctest.h>

#include <cstring>

#include "sr/core/Crc32.h"
#include "sr/core/DesignGrid.h"
#include "sr/core/Path.h"

using sr::core::NormalizeAssetPath;

TEST_CASE("NormalizeAssetPath folds case and separators") {
    CHECK(NormalizeAssetPath("Res\\Char\\Foo.BSR") == "res\\char\\foo.bsr");
    CHECK(NormalizeAssetPath("res/char/foo.bsr") == "res\\char\\foo.bsr");
    CHECK(NormalizeAssetPath("\\\\res//char\\.\\foo.bsr\\") == "res\\char\\foo.bsr");
    CHECK(NormalizeAssetPath("") == "");
    CHECK(NormalizeAssetPath("\\") == "");
}

TEST_CASE("NormalizeAssetPath leaves CP949 bytes alone") {
    const std::string korean = "fonts\\\xB1\xE2\xBA\xBB.TTF";
    CHECK(NormalizeAssetPath(korean) == "fonts\\\xB1\xE2\xBA\xBB.ttf");
}

TEST_CASE("Crc32 matches the standard check value") {
    const char* text = "123456789";
    const auto bytes = std::as_bytes(std::span(text, std::strlen(text)));
    CHECK(sr::core::Crc32(bytes) == 0xCBF43926u);
    // Incremental hashing gives the same result.
    CHECK(sr::core::Crc32(bytes.subspan(4), sr::core::Crc32(bytes.first(4))) == 0xCBF43926u);
    CHECK(sr::core::Crc32({}) == 0u);
}

TEST_CASE("DesignGrid scales the 1024x768 grid uniformly and centres it") {
    sr::core::DesignGrid grid;
    grid.Fit(1024.0f, 768.0f);
    CHECK(grid.scale() == 1.0f);
    CHECK(grid.ToScreen(sr::core::DesignPoint{10.0f, 20.0f}).x == 10.0f);

    grid.Fit(2048.0f, 1080.0f); // wider than 4:3: height decides, bars left and right
    CHECK(grid.scale() == doctest::Approx(1080.0f / 768.0f));
    CHECK(grid.ToScreen(sr::core::DesignPoint{0.0f, 0.0f}).x == doctest::Approx((2048.0f - 1440.0f) * 0.5f));
    CHECK(grid.ToScreen(sr::core::DesignPoint{0.0f, 0.0f}).y == 0.0f);

    grid.Fit(800.0f, 1200.0f); // taller than 4:3: width decides, bars top and bottom
    CHECK(grid.scale() == doctest::Approx(800.0f / 1024.0f));
    CHECK(grid.ToScreen(sr::core::DesignPoint{0.0f, 0.0f}).x == 0.0f);
    CHECK(grid.ToScreen(sr::core::DesignPoint{0.0f, 0.0f}).y == doctest::Approx((1200.0f - 600.0f) * 0.5f));
}

TEST_CASE("DesignGrid maps a mouse position back to design space") {
    sr::core::DesignGrid grid;
    grid.Fit(1920.0f, 1080.0f);
    const sr::core::DesignRect button{100.0f, 200.0f, 50.0f, 20.0f};
    const sr::core::DesignRect onScreen = grid.ToScreen(button);
    const sr::core::DesignPoint centre{onScreen.x + onScreen.w * 0.5f, onScreen.y + onScreen.h * 0.5f};
    CHECK(button.Contains(grid.ToDesign(centre)));
    CHECK_FALSE(button.Contains(grid.ToDesign({onScreen.x - 2.0f, centre.y})));
    grid.Fit(0.0f, 0.0f);
    CHECK(grid.scale() == 1.0f);
}

TEST_CASE("DesignGrid anchored mode grows with the window and spans all of it") {
    sr::core::DesignGrid grid;
    grid.FitAnchored(1920.0f, 1080.0f);
    CHECK(grid.scale() == doctest::Approx(1080.0f / 768.0f));
    CHECK(grid.height() == doctest::Approx(768.0f));
    CHECK(grid.width() == doctest::Approx(1920.0f * 768.0f / 1080.0f));
    // No letterbox: the virtual screen's right edge is the window's right edge.
    CHECK(grid.ToScreen(sr::core::DesignPoint{grid.width(), 0.0f}).x == doctest::Approx(1920.0f));
    CHECK(grid.ToScreen(sr::core::DesignPoint{0.0f, 0.0f}).x == 0.0f);
    grid.Fit(1920.0f, 1080.0f);
    CHECK(grid.width() == 1024.0f);
}

#include "sr/core/Profiler.h"

TEST_CASE("profiler keeps a frame's timings and counts, in first-seen order") {
    sr::core::Profiler& p = sr::core::Profiler::Get();
    p.NextFrame();
    p.Count("test: drawn", 3);
    p.AddTime("test: cull", 1.5);
    p.Count("test: drawn", 2);
    p.NextFrame();
    const auto find = [&](const char* name) -> const sr::core::Profiler::Entry* {
        for (const auto& e : p.Last()) {
            if (std::string_view(e.name) == name) return &e;
        }
        return nullptr;
    };
    REQUIRE(find("test: drawn") != nullptr);
    CHECK(find("test: drawn")->value == 5.0);
    CHECK_FALSE(find("test: drawn")->time);
    REQUIRE(find("test: cull") != nullptr);
    CHECK(find("test: cull")->value == 1.5);
    p.NextFrame(); // a quiet frame lists the names at zero
    REQUIRE(find("test: drawn") != nullptr);
    CHECK(find("test: drawn")->value == 0.0);
}
