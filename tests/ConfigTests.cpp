#include <doctest/doctest.h>

#include <iterator>
#include <string>

#include "sr/core/Config.h"

using sr::core::Config;

TEST_CASE("Config: file text, then the command line on top") {
    Config config;
    std::string error;
    REQUIRE(config.ParseText("# engine settings\n"
                             "game = C:\\Games\\v1150\n"
                             "workers=6\r\n"
                             "\n"
                             "simulation-rate = 30.5\n"
                             "vsync = off\n",
                             error));
    const char* argv[] = {"SroReimagined", "--workers", "3", "--validation", "--size", "1280x720", "--offset", "-4"};
    REQUIRE(config.ParseCommandLine(static_cast<int>(std::size(argv)), argv, error));

    CHECK(config.GetString("game") == "C:\\Games\\v1150");
    CHECK(config.GetInt("workers", 0) == 3);  // the command line wins
    CHECK(config.GetDouble("simulation-rate", 0.0) == doctest::Approx(30.5));
    CHECK(config.GetBool("vsync", true) == false);
    CHECK(config.GetBool("validation", false) == true); // a flag alone
    CHECK(config.GetString("size") == "1280x720");
    CHECK(config.GetInt("offset", 0) == -4);           // a negative value is a value, not an option
    CHECK(config.GetInt("missing", 17) == 17);
    CHECK(config.GetInt("game", 5) == 5);              // not a number: the fallback
    CHECK(config.GetBool("size", true) == true);       // not a boolean: the fallback
}

TEST_CASE("Config: errors") {
    Config config;
    std::string error;
    CHECK_FALSE(config.ParseText("just words\n", error));
    CHECK_FALSE(config.ParseText(" = value\n", error));
    const char* argv[] = {"SroReimagined", "stray"};
    CHECK_FALSE(config.ParseCommandLine(2, argv, error));
    CHECK(config.LoadFile("this file does not exist.cfg", error)); // missing file: nothing set, no error
}
