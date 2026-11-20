#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "sr/assets/Texture.h"

using namespace sr::assets;

namespace {

// Minimal DDS header writer for tests.
std::vector<std::byte> MakeDds(std::uint32_t w, std::uint32_t h, std::uint32_t mipCount, const char* fourCC,
                               std::uint32_t bits, std::uint32_t r, std::uint32_t g, std::uint32_t b,
                               std::uint32_t a, std::uint32_t pfFlags) {
    std::vector<std::byte> f(128);
    auto put = [&](std::size_t at, std::uint32_t v) { std::memcpy(f.data() + at, &v, 4); };
    std::memcpy(f.data(), "DDS ", 4);
    put(4, 124);
    put(8, 0x1007 | (mipCount > 1 ? 0x20000u : 0u));
    put(12, h);
    put(16, w);
    put(28, mipCount);
    put(76, 32);
    put(80, pfFlags);
    if (fourCC != nullptr) std::memcpy(f.data() + 84, fourCC, 4);
    put(88, bits);
    put(92, r);
    put(96, g);
    put(100, b);
    put(104, a);
    return f;
}

void Append(std::vector<std::byte>& f, std::initializer_list<std::uint8_t> bytes) {
    for (auto v : bytes) f.push_back(std::byte{v});
}

} // namespace

TEST_CASE("DecodeDds expands A1R5G5B5 to RGBA8") {
    auto f = MakeDds(2, 1, 1, nullptr, 16, 0x7C00, 0x03E0, 0x001F, 0x8000, 0x41);
    Append(f, {0x00, 0xFC});  // A=1, R=31, G=0, B=0 -> opaque red
    Append(f, {0xE0, 0x03});  // A=0, G=31         -> transparent green
    TextureData tex;
    std::string error;
    REQUIRE_MESSAGE(DecodeDds(f, tex, error), error);
    CHECK(tex.format == TextureFormat::RGBA8);
    CHECK(tex.sourceFormat == "A1R5G5B5");
    REQUIRE(tex.pixels.size() == 8);
    const auto* p = reinterpret_cast<const std::uint8_t*>(tex.pixels.data());
    CHECK(p[0] == 255); CHECK(p[1] == 0); CHECK(p[2] == 0); CHECK(p[3] == 255);
    CHECK(p[4] == 0); CHECK(p[5] == 255); CHECK(p[6] == 0); CHECK(p[7] == 0);
}

TEST_CASE("DecodeDds names masks without alpha as X formats and makes them opaque") {
    auto f = MakeDds(1, 1, 1, nullptr, 32, 0xFF0000, 0xFF00, 0xFF, 0, 0x40);
    Append(f, {0x30, 0x20, 0x10, 0x00}); // B=0x30 G=0x20 R=0x10, junk X
    TextureData tex;
    std::string error;
    REQUIRE_MESSAGE(DecodeDds(f, tex, error), error);
    CHECK(tex.sourceFormat == "X8R8G8B8");
    const auto* p = reinterpret_cast<const std::uint8_t*>(tex.pixels.data());
    CHECK(p[0] == 0x10); CHECK(p[1] == 0x20); CHECK(p[2] == 0x30); CHECK(p[3] == 255);
}

TEST_CASE("DecodeDds keeps DXT blocks and drops mips the file does not contain") {
    // 8x8 DXT1 claiming 4 mips but holding only the first two (32 + 8 bytes).
    auto f = MakeDds(8, 8, 4, "DXT1", 0, 0, 0, 0, 0, 0x4);
    for (int i = 0; i < 40; ++i) f.push_back(std::byte{static_cast<std::uint8_t>(i)});
    TextureData tex;
    std::string error;
    REQUIRE_MESSAGE(DecodeDds(f, tex, error), error);
    CHECK(tex.format == TextureFormat::BC1);
    CHECK(tex.sourceFormat == "DXT1");
    REQUIRE(tex.mips.size() == 2);
    CHECK(tex.mips[0].size == 32);
    CHECK(tex.mips[1].width == 4);
    CHECK(tex.mips[1].size == 8);
    CHECK(tex.MipBytes(1)[0] == std::byte{32});
}

TEST_CASE("DecodeDds marks DXT2 as premultiplied BC2") {
    auto f = MakeDds(4, 4, 1, "DXT2", 0, 0, 0, 0, 0, 0x4);
    f.resize(f.size() + 16);
    TextureData tex;
    std::string error;
    REQUIRE_MESSAGE(DecodeDds(f, tex, error), error);
    CHECK(tex.format == TextureFormat::BC2);
    CHECK(tex.premultipliedAlpha);
}

TEST_CASE("DecodeDdj skips the 20-byte JMXV header and rejects other files") {
    auto dds = MakeDds(4, 4, 1, "DXT5", 0, 0, 0, 0, 0, 0x4);
    dds.resize(dds.size() + 16);
    std::vector<std::byte> ddj(20);
    std::memcpy(ddj.data(), "JMXVDDJ 1000", 12);
    ddj.insert(ddj.end(), dds.begin(), dds.end());

    TextureData tex;
    std::string error;
    REQUIRE_MESSAGE(DecodeDdj(ddj, tex, error), error);
    CHECK(tex.format == TextureFormat::BC3);
    CHECK_FALSE(DecodeDdj(dds, tex, error)); // a bare DDS is not a DDJ
}

TEST_CASE("CompleteMips keeps a one-level BC1 texture in BC1 and builds its chain") {
    TextureData tex;
    tex.width = 8;
    tex.height = 4;
    tex.format = TextureFormat::BC1;
    // Two blocks: all colour 0 = pure red (0xF800) / then all white (0xFFFF).
    const std::uint8_t blocks[16] = {0x00, 0xF8, 0x00, 0x00, 0, 0, 0, 0, 0xFF, 0xFF, 0x00, 0x00, 0, 0, 0, 0};
    tex.pixels.resize(16);
    std::memcpy(tex.pixels.data(), blocks, 16);
    tex.mips.push_back({8, 4, 0, 16});
    CompleteMips(tex);
    CHECK(tex.format == TextureFormat::BC1);
    REQUIRE(tex.mips.size() == 4); // 8x4, 4x2, 2x1, 1x1
    CHECK(tex.mips[3].width == 1);
    std::size_t offset = 0;
    for (const TextureData::Mip& m : tex.mips) {
        CHECK(m.offset == offset);
        CHECK(m.size == TextureMipSize(TextureFormat::BC1, m.width, m.height));
        offset += m.size;
    }
    CHECK(tex.pixels.size() == offset);
    CHECK(std::memcmp(tex.pixels.data(), blocks, 16) == 0); // the top level as it came
    const std::vector<std::uint8_t> top = DecodeToRgba8(tex, 0);
    CHECK(top[0] == 255); // red
    CHECK(top[1] == 0);
    CHECK(top[3] == 255); // opaque
    CHECK(top[4 * 4 + 1] == 255); // the second block: white
    const std::vector<std::uint8_t> last = DecodeToRgba8(tex, 3);
    CHECK(last[0] >= 247);       // red averaged with white, within 565's steps
    CHECK(last[1] > 100);
    CHECK(last[1] < 160);
    CHECK(last[3] == 255);
}

namespace {

// The largest difference of the first `channels` channels between two RGBA8 images.
int MaxError(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, int channels) {
    int worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (int(i % 4) >= channels) continue;
        worst = std::max(worst, std::abs(int(a[i]) - int(b[i])));
    }
    return worst;
}

} // namespace

TEST_CASE("CompleteMips' generated levels keep colours, cut-outs and alpha in BC1, BC2 and BC3") {
    struct Case {
        TextureFormat format;
        std::array<std::uint8_t, 4> colours[4];
    };
    const Case cases[] = {
        {TextureFormat::BC1, {{{255, 0, 0, 255}}, {{0, 255, 0, 255}}, {{0, 0, 255, 255}}, {{255, 255, 255, 255}}}},
        {TextureFormat::BC1, {{{200, 120, 40, 255}}, {{0, 0, 0, 0}}, {{200, 120, 40, 255}}, {{0, 0, 0, 0}}}}, // a cut-out
        {TextureFormat::BC2, {{{255, 0, 0, 255}}, {{255, 0, 0, 136}}, {{0, 0, 255, 0}}, {{0, 0, 255, 255}}}},
        {TextureFormat::BC3, {{{90, 90, 90, 255}}, {{90, 90, 90, 128}}, {{90, 90, 90, 10}}, {{90, 90, 90, 200}}}},
    };
    for (const Case& c : cases) {
        CAPTURE(int(c.format));
        // A one-level 16 x 16 texture in the format, one solid colour per 8 x 8 quadrant: its
        // level 1 (8 x 8) is known exactly, and the encoder must give those colours back.
        TextureData tex;
        tex.width = 16;
        tex.height = 16;
        tex.format = c.format;
        const std::size_t blockBytes = c.format == TextureFormat::BC1 ? 8 : 16;
        tex.pixels.assign(TextureMipSize(c.format, 16, 16), std::byte{0});
        tex.mips.push_back({16, 16, 0, tex.pixels.size()});
        // Each 4 x 4 block one solid colour: colour 0 = that colour (565), every index 0; BC1's
        // transparent ones as the 3-colour mode's index 3; BC2 / BC3 alpha set to the colour's.
        for (std::uint32_t by = 0; by < 4; ++by) {
            for (std::uint32_t bx = 0; bx < 4; ++bx) {
                const auto& col = c.colours[(by / 2) * 2 + bx / 2];
                auto* b = reinterpret_cast<std::uint8_t*>(tex.pixels.data()) + (std::size_t{by} * 4 + bx) * blockBytes;
                std::uint8_t* colour = c.format == TextureFormat::BC1 ? b : b + 8;
                const std::uint16_t v = static_cast<std::uint16_t>((col[0] >> 3) << 11 | (col[1] >> 2) << 5 | (col[2] >> 3));
                colour[0] = static_cast<std::uint8_t>(v), colour[1] = static_cast<std::uint8_t>(v >> 8);
                colour[2] = colour[0], colour[3] = colour[1];
                const bool cut = c.format == TextureFormat::BC1 && col[3] < 128;
                std::memset(colour + 4, cut ? 0xFF : 0x00, 4);
                if (c.format == TextureFormat::BC2) {
                    const std::uint8_t a = static_cast<std::uint8_t>(col[3] * 15 / 255);
                    for (int i = 0; i < 8; ++i) b[i] = static_cast<std::uint8_t>(a | a << 4);
                } else if (c.format == TextureFormat::BC3) {
                    b[0] = col[3], b[1] = col[3];
                    std::memset(b + 2, 0, 6);
                }
            }
        }
        const std::vector<std::uint8_t> top = DecodeToRgba8(tex, 0);
        CompleteMips(tex);
        REQUIRE(tex.mips.size() == 5); // 16, 8, 4, 2, 1
        CHECK(tex.format == c.format);
        for (const TextureData::Mip& m : tex.mips) CHECK(m.size == TextureMipSize(c.format, m.width, m.height));
        // Level 1 (8 x 8) is the top level's quadrants, each solid: the encoder's colours back
        // within a 565 step, alpha within BC2's 4-bit step or exactly for BC3's ends.
        const std::vector<std::uint8_t> level = DecodeToRgba8(tex, 1);
        std::vector<std::uint8_t> expect(level.size());
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) std::memcpy(&expect[(std::size_t{y} * 8 + x) * 4], &top[(std::size_t{y * 2} * 16 + x * 2) * 4], 4);
        }
        for (std::size_t i = 0; i < level.size(); i += 4) {
            if (expect[i + 3] == 0 && c.format == TextureFormat::BC1) {
                CHECK(level[i + 3] == 0); // a cut-out stays cut out
                continue;
            }
            for (int k = 0; k < 3; ++k) CHECK(std::abs(int(level[i + k]) - int(expect[i + k])) <= 8);
            CHECK(std::abs(int(level[i + 3]) - int(expect[i + 3])) <= (c.format == TextureFormat::BC2 ? 17 : 0));
        }
    }
}

TEST_CASE("The BC encoder holds a smooth gradient within a few steps") {
    // A one-level BC1 texture can not carry a gradient into level 1 exactly; the encoder's error on
    // the 4 x 4 blocks of a gradient is the measure (two ends and two thirds per block).
    TextureData tex;
    tex.width = 64;
    tex.height = 64;
    tex.format = TextureFormat::BC1;
    tex.pixels.assign(TextureMipSize(TextureFormat::BC1, 64, 64), std::byte{0});
    tex.mips.push_back({64, 64, 0, tex.pixels.size()});
    // Top level: each block's colour 0 = a gradient's value (one colour per block, index 0).
    for (std::uint32_t by = 0; by < 16; ++by) {
        for (std::uint32_t bx = 0; bx < 16; ++bx) {
            auto* b = reinterpret_cast<std::uint8_t*>(tex.pixels.data()) + (std::size_t{by} * 16 + bx) * 8;
            const std::uint16_t v = static_cast<std::uint16_t>((bx * 2) << 11 | (by * 4) << 5 | 16);
            b[0] = static_cast<std::uint8_t>(v), b[1] = static_cast<std::uint8_t>(v >> 8);
            b[2] = 0, b[3] = 0; // c0 > c1: four colours, index 0 = c0
        }
    }
    const std::vector<std::uint8_t> top = DecodeToRgba8(tex, 0);
    CompleteMips(tex);
    REQUIRE(tex.mips.size() == 7);
    // Level 1: each pixel the mean of a 2 x 2 of one block's colour, so the block's colour; its
    // 4 x 4 blocks span 2 x 2 of the top's blocks (four colours a block: a gradient of four).
    const std::vector<std::uint8_t> level = DecodeToRgba8(tex, 1);
    std::vector<std::uint8_t> expect(level.size());
    for (std::uint32_t y = 0; y < 32; ++y) {
        for (std::uint32_t x = 0; x < 32; ++x) std::memcpy(&expect[(std::size_t{y} * 32 + x) * 4], &top[(std::size_t{y * 2} * 64 + x * 2) * 4], 4);
    }
    CHECK(MaxError(level, expect, 3) <= 24);
}
