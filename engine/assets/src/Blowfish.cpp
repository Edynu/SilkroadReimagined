#include "sr/assets/Blowfish.h"

#include <cassert>
#include <cstring>
#include <utility>

#include "BlowfishTables.inc"

namespace sr::assets {

Blowfish::Blowfish(std::span<const std::uint8_t> key) {
    assert(!key.empty() && key.size() <= 56);
    std::memcpy(p_, detail::kBlowfishP, sizeof(p_));
    std::memcpy(s_, detail::kBlowfishS, sizeof(s_));

    std::size_t j = 0;
    for (std::uint32_t& p : p_) {
        std::uint32_t word = 0;
        for (int k = 0; k < 4; ++k) {
            word = (word << 8) | key[j];
            j = (j + 1) % key.size();
        }
        p ^= word;
    }

    std::uint32_t left = 0;
    std::uint32_t right = 0;
    for (int i = 0; i < 18; i += 2) {
        EncryptWords(left, right);
        p_[i] = left;
        p_[i + 1] = right;
    }
    for (auto& box : s_) {
        for (int i = 0; i < 256; i += 2) {
            EncryptWords(left, right);
            box[i] = left;
            box[i + 1] = right;
        }
    }
}

std::uint32_t Blowfish::F(std::uint32_t x) const {
    return ((s_[0][x >> 24] + s_[1][(x >> 16) & 0xFF]) ^ s_[2][(x >> 8) & 0xFF]) + s_[3][x & 0xFF];
}

void Blowfish::EncryptWords(std::uint32_t& left, std::uint32_t& right) const {
    std::uint32_t l = left;
    std::uint32_t r = right;
    for (int i = 0; i < 16; ++i) {
        l ^= p_[i];
        r ^= F(l);
        std::swap(l, r);
    }
    std::swap(l, r);
    r ^= p_[16];
    l ^= p_[17];
    left = l;
    right = r;
}

void Blowfish::DecryptWords(std::uint32_t& left, std::uint32_t& right) const {
    std::uint32_t l = left;
    std::uint32_t r = right;
    for (int i = 17; i > 1; --i) {
        l ^= p_[i];
        r ^= F(l);
        std::swap(l, r);
    }
    std::swap(l, r);
    r ^= p_[1];
    l ^= p_[0];
    left = l;
    right = r;
}

namespace {

template <typename Op>
void ForEachBlock(std::span<std::byte> data, Op op) {
    assert(data.size() % 8 == 0);
    for (std::size_t off = 0; off + 8 <= data.size(); off += 8) {
        std::uint32_t words[2];
        std::memcpy(words, data.data() + off, 8); // little-endian host, see Pk2Archive.cpp
        op(words[0], words[1]);
        std::memcpy(data.data() + off, words, 8);
    }
}

} // namespace

void Blowfish::EncryptBlocks(std::span<std::byte> data) const {
    ForEachBlock(data, [this](std::uint32_t& l, std::uint32_t& r) { EncryptWords(l, r); });
}

void Blowfish::DecryptBlocks(std::span<std::byte> data) const {
    ForEachBlock(data, [this](std::uint32_t& l, std::uint32_t& r) { DecryptWords(l, r); });
}

} // namespace sr::assets
