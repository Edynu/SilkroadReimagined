#pragma once

// What a shard's game servers tell each other: our encoding in hexadecimal in
// a NOTIFY's payload, which holds at most 7,999 bytes. One channel per shard and feature.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sr::server {

constexpr std::size_t kBusTextLimit = 7990;

inline std::string BusToHex(std::span<const std::byte> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::byte b : bytes) {
        out.push_back(kDigits[std::to_integer<unsigned>(b) >> 4]);
        out.push_back(kDigits[std::to_integer<unsigned>(b) & 15]);
    }
    return out;
}

inline std::optional<std::vector<std::byte>> BusFromHex(std::string_view text) {
    if (text.size() % 2 != 0) return std::nullopt;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::vector<std::byte> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = digit(text[i]), low = digit(text[i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        out.push_back(std::byte((high << 4) | low));
    }
    return out;
}

} // namespace sr::server
