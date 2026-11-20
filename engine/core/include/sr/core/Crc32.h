#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sr::core {

// Standard CRC-32 (IEEE 802.3, reflected 0xEDB88320), identical to zlib's
// crc32(). Pass the previous result as `crc` to hash data in pieces.
std::uint32_t Crc32(std::span<const std::byte> data, std::uint32_t crc = 0);

} // namespace sr::core
