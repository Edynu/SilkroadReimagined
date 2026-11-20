#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "sr/game/Protocol.h"

using namespace sr::game::proto;

namespace {

enum class Colour : std::uint8_t { Red = 1, Blue = 2 };

struct Point {
    float x = 0, y = 0;
    template <typename V>
    void Visit(V& v) {
        v("x", x);
        v("y", y);
    }
    bool operator==(const Point&) const = default;
};

// Every kind of field once.
struct Sample {
    bool flag = false;
    std::int8_t i8 = 0;
    std::uint16_t u16 = 0;
    std::int32_t i32 = 0;
    std::uint64_t u64 = 0;
    double f64 = 0;
    Colour colour = Colour::Red;
    std::string name;
    Point at;
    std::vector<Point> path;
    std::vector<std::string> tags;
    template <typename V>
    void Visit(V& v) {
        v("flag", flag);
        v("i8", i8);
        v("u16", u16);
        v("i32", i32);
        v("u64", u64);
        v("f64", f64);
        v("colour", colour);
        v("name", name, 16);
        v("at", at);
        v("path", path, 8);
        v("tags", tags);
    }
    bool operator==(const Sample&) const = default;
};

Sample Filled() {
    Sample s;
    s.flag = true;
    s.i8 = -5;
    s.u16 = 0xBEEF;
    s.i32 = -123456789;
    s.u64 = 0x0123456789ABCDEFull;
    s.f64 = 2.5;
    s.colour = Colour::Blue;
    s.name = "Jangan \xEC\x9E\xA5"; // UTF-8 kept as bytes
    s.at = {1.5f, -2.0f};
    s.path = {{1, 2}, {3, 4}};
    s.tags = {"a", "", "ccc"};
    return s;
}

} // namespace

TEST_CASE("Protocol: every field kind round-trips, little-endian") {
    const Sample s = Filled();
    const std::vector<std::byte> bytes = Encode(s);
    Sample back;
    REQUIRE(Decode(bytes, back));
    CHECK(back == s);

    // The layout: bool, i8, then u16 0xBEEF little-endian.
    REQUIRE(bytes.size() > 4);
    CHECK(bytes[0] == std::byte{1});
    CHECK(bytes[1] == std::byte{0xFB});
    CHECK(bytes[2] == std::byte{0xEF});
    CHECK(bytes[3] == std::byte{0xBE});

    CHECK(Describe("Sample", s).starts_with("Sample{flag=true, i8=-5, u16=48879,"));
    CHECK(Describe("Sample", s).find("at={x=1.5, y=-2}") != std::string::npos);
}

TEST_CASE("Protocol: Decode refuses anything but exactly one message") {
    const std::vector<std::byte> bytes = Encode(Filled());
    Sample out;
    // Every truncation fails.
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        CHECK_FALSE(Decode(std::span(bytes.data(), n), out));
    }
    // Trailing bytes fail.
    std::vector<std::byte> longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(Decode(longer, out));
}

TEST_CASE("Protocol: limits, bools and floats are checked") {
    Sample out;
    // The layout before the name: bool, i8, u16, i32, u64, f64, colour = 25 bytes; then its u16 size.
    constexpr std::size_t kNameAt = 25;
    SUBCASE("a string over its limit") {
        Sample s;
        s.name = std::string(16, 'x');
        std::vector<std::byte> bytes = Encode(s);
        CHECK(Decode(bytes, out));
        // Its size made 17, one byte more behind it (our encoder never sends that: a peer that does is refused).
        bytes[kNameAt] = std::byte{17};
        bytes.insert(bytes.begin() + kNameAt + 2, std::byte{'x'});
        CHECK_FALSE(Decode(bytes, out));
    }
    SUBCASE("a list over its limit") {
        Sample s;
        s.path.assign(8, Point{});
        std::vector<std::byte> bytes = Encode(s);
        CHECK(Decode(bytes, out));
        // An empty name (2 bytes), the point (8), then the path's count: made 9, a ninth point behind the eighth.
        const std::size_t countAt = kNameAt + 2 + 8;
        bytes[countAt] = std::byte{9};
        bytes.insert(bytes.begin() + countAt + 2 + 8 * 8, 8, std::byte{0});
        CHECK_FALSE(Decode(bytes, out));
    }
    SUBCASE("the encoder keeps to the limits: a string or list cut to fit, whole characters") {
        Sample s;
        s.name = std::string(15, 'x') + "\xEC\x9E\xA5"; // 18 bytes: the last character would not fit whole
        s.path.assign(9, Point{});
        REQUIRE(Decode(Encode(s), out));
        CHECK(out.name == std::string(15, 'x'));
        CHECK(out.path.size() == 8);
    }
    SUBCASE("a list count the bytes cannot hold is refused before allocating") {
        Sample s;
        std::vector<std::byte> bytes = Encode(s);
        // The tags count is the last u16: claim 4,000 strings with no bytes behind them.
        bytes[bytes.size() - 2] = std::byte{0xA0};
        bytes[bytes.size() - 1] = std::byte{0x0F};
        CHECK_FALSE(Decode(bytes, out));
    }
    SUBCASE("a bool other than 0 or 1") {
        std::vector<std::byte> bytes = Encode(Sample{});
        bytes[0] = std::byte{2};
        CHECK_FALSE(Decode(bytes, out));
    }
    SUBCASE("non-finite floats") {
        Sample s;
        s.f64 = std::numeric_limits<double>::infinity();
        CHECK_FALSE(Decode(Encode(s), out));
        s.f64 = std::numeric_limits<double>::quiet_NaN();
        CHECK_FALSE(Decode(Encode(s), out));
        s.f64 = 0;
        s.at.x = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(Decode(Encode(s), out));
    }
}

TEST_CASE("Protocol: every string is well-formed UTF-8") {
    CHECK(ValidUtf8("Jangan \xEC\x9E\xA5 \xF0\x9F\x99\x82"));
    CHECK_FALSE(ValidUtf8("\xFF"));
    CHECK_FALSE(ValidUtf8("\xEC\x9E"));         // cut short
    CHECK_FALSE(ValidUtf8("\xC0\xAF"));         // overlong '/'
    CHECK_FALSE(ValidUtf8("\xE0\x80\xAF"));     // overlong
    CHECK_FALSE(ValidUtf8("\xED\xA0\x80"));     // a surrogate
    CHECK_FALSE(ValidUtf8("\xF4\x90\x80\x80")); // past U+10FFFF
    CHECK(ToValidUtf8("a\xFF" "b\xEC\x9E", 100) == "a?b??");

    // A peer's broken string: the message is refused.
    Sample s;
    s.name = "ok";
    std::vector<std::byte> bytes = Encode(s);
    Sample out;
    REQUIRE(Decode(bytes, out));
    bytes[25 + 2] = std::byte{0xFF}; // the name's first byte (after its u16 size)
    CHECK_FALSE(Decode(bytes, out));
    // Ours is mended before it goes: the other side always takes it.
    s.name = "a\xFF";
    REQUIRE(Decode(Encode(s), out));
    CHECK(out.name == "a?");
}

TEST_CASE("Protocol: the version check") {
    Hello hello;
    hello.client = "SroReimagined 0.1";
    Hello back;
    REQUIRE(Decode(Encode(hello), back));
    CHECK(back.protocol == kProtocolVersion);
    CHECK(back.client == "SroReimagined 0.1");

    CHECK(AnswerHello(hello, "test").result == WelcomeResult::Accepted);
    hello.protocol = kProtocolVersion + 1;
    const Welcome refused = AnswerHello(hello, "test");
    CHECK(refused.result == WelcomeResult::ProtocolMismatch);
    CHECK(refused.protocol == kProtocolVersion);
    CHECK_FALSE(refused.reason.empty());
    Welcome welcomeBack;
    REQUIRE(Decode(Encode(refused), welcomeBack));
    CHECK(welcomeBack.result == WelcomeResult::ProtocolMismatch);
    CHECK(welcomeBack.reason == refused.reason);
}
