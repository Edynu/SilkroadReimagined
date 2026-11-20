#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "sr/assets/Blowfish.h"

using sr::assets::Blowfish;

namespace {

void CheckVector(std::array<std::uint8_t, 8> key, std::uint32_t l, std::uint32_t r, std::uint32_t cl,
                 std::uint32_t cr) {
    const Blowfish bf(key);
    std::uint32_t a = l;
    std::uint32_t b = r;
    bf.EncryptWords(a, b);
    CHECK(a == cl);
    CHECK(b == cr);
    bf.DecryptWords(a, b);
    CHECK(a == l);
    CHECK(b == r);
}

} // namespace

TEST_CASE("Blowfish matches Eric Young's reference vectors") {
    CheckVector({0, 0, 0, 0, 0, 0, 0, 0}, 0x00000000, 0x00000000, 0x4EF99745, 0x6198DD78);
    CheckVector({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 0xFFFFFFFF, 0xFFFFFFFF, 0x51866FD5, 0xB85ECB8A);
    CheckVector({0x30, 0, 0, 0, 0, 0, 0, 0}, 0x10000000, 0x00000001, 0x7D856F9A, 0x613063F2);
}

TEST_CASE("Blowfish with the Silkroad PK2 key reproduces the archive verify bytes") {
    // "169841" XOR the PK2 key mask.
    const std::array<std::uint8_t, 6> key = {0x32, 0xCE, 0xDD, 0x7C, 0xBC, 0xA8};
    const Blowfish bf(key);

    std::array<std::byte, 16> block{};
    std::memcpy(block.data(), "Joymax Pak File", 16);
    bf.EncryptBlocks(block);
    // Shipped archives store d8 da 30 as the first verify bytes.
    CHECK(block[0] == std::byte{0xD8});
    CHECK(block[1] == std::byte{0xDA});
    CHECK(block[2] == std::byte{0x30});

    bf.DecryptBlocks(block);
    CHECK(std::memcmp(block.data(), "Joymax Pak File", 16) == 0);
}
