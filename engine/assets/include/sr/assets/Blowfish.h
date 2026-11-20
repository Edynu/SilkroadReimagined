#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sr::assets {

// Standard Blowfish (Schneier, 16 rounds) operating on 8-byte blocks whose two
// 32-bit halves are stored little-endian, which is how PK2 archives store them.
class Blowfish {
public:
    // `key` must be 1..56 bytes.
    explicit Blowfish(std::span<const std::uint8_t> key);

    void EncryptWords(std::uint32_t& left, std::uint32_t& right) const;
    void DecryptWords(std::uint32_t& left, std::uint32_t& right) const;

    // In-place over whole blocks; `data.size()` must be a multiple of 8.
    void EncryptBlocks(std::span<std::byte> data) const;
    void DecryptBlocks(std::span<std::byte> data) const;

private:
    std::uint32_t F(std::uint32_t x) const;

    std::uint32_t p_[18];
    std::uint32_t s_[4][256];
};

} // namespace sr::assets
