#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sr::assets {

// GPU-ready pixel formats. Every original texture maps to one of these; the
// block-compressed ones upload as-is, everything else is expanded to RGBA8.
enum class TextureFormat : std::uint8_t {
    RGBA8, // 8 bits per channel, R G B A byte order
    BC1,   // DXT1
    BC2,   // DXT2 / DXT3
    BC3,   // DXT4 / DXT5
};

const char* ToString(TextureFormat format);

// A decoded 2D texture with its mip chain, independent of the file format it
// came from (DDJ today; KTX2/PNG remaster textures later).
struct TextureData {
    struct Mip {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::size_t offset = 0; // into `pixels`
        std::size_t size = 0;
    };

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureFormat format = TextureFormat::RGBA8;
    bool premultipliedAlpha = false; // DXT2/DXT4: colour already multiplied by alpha
    std::string sourceFormat;        // e.g. "DXT1", "A1R5G5B5", for tools
    std::vector<Mip> mips;           // largest first
    std::vector<std::byte> pixels;

    std::span<const std::byte> MipBytes(std::size_t level) const {
        return std::span(pixels).subspan(mips[level].offset, mips[level].size);
    }
};

// Bytes in one mip level of `format` (tightly packed).
std::size_t TextureMipSize(TextureFormat format, std::uint32_t width, std::uint32_t height);

// A texture stored with one level gets its whole mip chain, as the original's D3DX loader
// makes one (box filtered). A block-compressed texture keeps its format: its top level as it
// came, the smaller ones encoded again in BC1 / BC2 / BC3 (decoded to RGBA8, as they once were,
// the game's textures took about three times the memory). Without the chain, far and tilted
// surfaces (water, ground) shimmer and moire.
void CompleteMips(TextureData& texture);
// The RGBA8 pixels of one block-compressed (BC1..BC3) or RGBA8 level.
std::vector<std::uint8_t> DecodeToRgba8(const TextureData& texture, std::size_t level);

// Decodes a DirectDraw Surface (.dds) file.
bool DecodeDds(std::span<const std::byte> file, TextureData& out, std::string& error);

// Decodes a Silkroad .ddj file: "JMXVDDJ 1000", u32 size, u32 3, then a DDS file.
bool DecodeDdj(std::span<const std::byte> file, TextureData& out, std::string& error);

} // namespace sr::assets
