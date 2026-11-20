#include "sr/core/Crc32.h"

#include <array>

namespace sr::core {

namespace {

// Slicing-by-4 tables: hashing every file of the game (~2.4 GB) for the
// archive cross-check should take seconds, not minutes.
constexpr std::array<std::array<std::uint32_t, 256>, 4> MakeTables() {
    std::array<std::array<std::uint32_t, 256>, 4> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        t[0][i] = c;
    }
    for (std::uint32_t i = 0; i < 256; ++i) {
        for (int s = 1; s < 4; ++s) {
            t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
        }
    }
    return t;
}

constexpr auto kTables = MakeTables();

} // namespace

std::uint32_t Crc32(std::span<const std::byte> data, std::uint32_t crc) {
    crc = ~crc;
    const std::byte* p = data.data();
    std::size_t n = data.size();

    while (n >= 4) {
        crc ^= static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
               static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
        crc = kTables[3][crc & 0xFF] ^ kTables[2][(crc >> 8) & 0xFF] ^ kTables[1][(crc >> 16) & 0xFF] ^
              kTables[0][crc >> 24];
        p += 4;
        n -= 4;
    }
    while (n-- > 0) {
        crc = kTables[0][(crc ^ static_cast<std::uint32_t>(*p++)) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

} // namespace sr::core
