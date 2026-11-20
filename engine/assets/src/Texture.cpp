#include "sr/assets/Texture.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace sr::assets {

namespace {

constexpr std::size_t kDdsHeaderSize = 128; // "DDS " + 124-byte header
constexpr std::uint32_t kDdsdMipMapCount = 0x20000;
constexpr std::uint32_t kDdpfAlphaPixels = 0x1;
constexpr std::uint32_t kDdpfFourCC = 0x4;
constexpr std::uint32_t kDdpfRgb = 0x40;

std::uint32_t U32(const std::byte* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

constexpr std::uint32_t FourCC(const char (&s)[5]) {
    return static_cast<std::uint32_t>(s[0]) | static_cast<std::uint32_t>(s[1]) << 8 |
           static_cast<std::uint32_t>(s[2]) << 16 | static_cast<std::uint32_t>(s[3]) << 24;
}

// Extracts one channel selected by `mask` and rescales it to 0..255.
struct Channel {
    std::uint32_t mask = 0;
    int shift = 0;
    std::uint32_t max = 0;

    explicit Channel(std::uint32_t m) : mask(m) {
        if (m != 0) {
            shift = std::countr_zero(m);
            max = m >> shift;
        }
    }
    std::uint8_t Get(std::uint32_t pixel, std::uint8_t fallback) const {
        if (mask == 0) return fallback;
        const std::uint32_t v = (pixel & mask) >> shift;
        return static_cast<std::uint8_t>((v * 255 + max / 2) / max);
    }
};

// Name for an uncompressed layout, D3D style (A1R5G5B5, X8R8G8B8, ...).
std::string MaskFormatName(std::uint32_t bits, std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a,
                           bool hasAlpha) {
    const auto width = [](std::uint32_t m) { return std::popcount(m); };
    std::string name;
    const int restBits = static_cast<int>(bits) - width(r) - width(g) - width(b);
    if (hasAlpha && a != 0) {
        name += "A" + std::to_string(width(a));
    } else if (restBits > 0) {
        name += "X" + std::to_string(restBits);
    }
    name += "R" + std::to_string(width(r)) + "G" + std::to_string(width(g)) + "B" + std::to_string(width(b));
    return name;
}

} // namespace

const char* ToString(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8: return "RGBA8";
    case TextureFormat::BC1: return "BC1";
    case TextureFormat::BC2: return "BC2";
    case TextureFormat::BC3: return "BC3";
    }
    return "?";
}

std::size_t TextureMipSize(TextureFormat format, std::uint32_t width, std::uint32_t height) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    const std::size_t blocks = std::size_t{(width + 3) / 4} * ((height + 3) / 4);
    switch (format) {
    case TextureFormat::RGBA8: return std::size_t{width} * height * 4;
    case TextureFormat::BC1: return blocks * 8;
    case TextureFormat::BC2:
    case TextureFormat::BC3: return blocks * 16;
    }
    return 0;
}

bool DecodeDds(std::span<const std::byte> file, TextureData& out, std::string& error) {
    out = TextureData{};
    if (file.size() < kDdsHeaderSize || std::memcmp(file.data(), "DDS ", 4) != 0) {
        error = "not a DDS file";
        return false;
    }
    const std::byte* h = file.data();
    const std::uint32_t flags = U32(h + 8);
    const std::uint32_t height = U32(h + 12);
    const std::uint32_t width = U32(h + 16);
    const std::uint32_t mipCount = U32(h + 28);
    const std::uint32_t pfFlags = U32(h + 80);
    const std::uint32_t fourCC = U32(h + 84);
    const std::uint32_t bitCount = U32(h + 88);
    const std::uint32_t rMask = U32(h + 92), gMask = U32(h + 96), bMask = U32(h + 100), aMask = U32(h + 104);
    const std::uint32_t caps2 = U32(h + 112);

    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        error = "bad DDS dimensions";
        return false;
    }
    if (caps2 & 0xFE00) {
        error = "cube maps and volume textures are not supported";
        return false;
    }

    bool uncompressed = false;
    if (pfFlags & kDdpfFourCC) {
        if (fourCC == FourCC("DXT1")) {
            out.format = TextureFormat::BC1;
        } else if (fourCC == FourCC("DXT2") || fourCC == FourCC("DXT3")) {
            out.format = TextureFormat::BC2;
            out.premultipliedAlpha = fourCC == FourCC("DXT2");
        } else if (fourCC == FourCC("DXT4") || fourCC == FourCC("DXT5")) {
            out.format = TextureFormat::BC3;
            out.premultipliedAlpha = fourCC == FourCC("DXT4");
        } else {
            error = "unsupported DDS FourCC";
            return false;
        }
        out.sourceFormat.assign(reinterpret_cast<const char*>(h + 84), 4);
    } else if ((pfFlags & kDdpfRgb) && (bitCount == 16 || bitCount == 24 || bitCount == 32)) {
        uncompressed = true;
        out.format = TextureFormat::RGBA8;
        out.sourceFormat = MaskFormatName(bitCount, rMask, gMask, bMask, aMask, (pfFlags & kDdpfAlphaPixels) != 0);
    } else {
        error = "unsupported DDS pixel format";
        return false;
    }

    // Mip count: only trusted when flagged, clamped to what the size allows
    // and to what the file actually contains.
    const std::uint32_t maxLevels = static_cast<std::uint32_t>(std::bit_width(std::max(width, height)));
    std::uint32_t levels = (flags & kDdsdMipMapCount) && mipCount > 0 ? std::min(mipCount, maxLevels) : 1;

    const std::size_t srcBpp = bitCount / 8;
    std::size_t available = file.size() - kDdsHeaderSize;
    std::size_t srcTotal = 0;
    std::uint32_t usable = 0;
    for (std::uint32_t m = 0; m < levels; ++m) {
        const std::uint32_t w = std::max(width >> m, 1u), hgt = std::max(height >> m, 1u);
        const std::size_t srcSize = uncompressed ? std::size_t{w} * hgt * srcBpp : TextureMipSize(out.format, w, hgt);
        if (srcTotal + srcSize > available) break;
        srcTotal += srcSize;
        ++usable;
    }
    if (usable == 0) {
        error = "DDS file is truncated";
        return false;
    }
    levels = usable;

    out.width = width;
    out.height = height;
    std::size_t dstTotal = 0;
    for (std::uint32_t m = 0; m < levels; ++m) {
        const std::uint32_t w = std::max(width >> m, 1u), hgt = std::max(height >> m, 1u);
        const std::size_t size = TextureMipSize(out.format, w, hgt);
        out.mips.push_back({w, hgt, dstTotal, size});
        dstTotal += size;
    }
    out.pixels.resize(dstTotal);

    const std::byte* src = file.data() + kDdsHeaderSize;
    if (!uncompressed) {
        std::memcpy(out.pixels.data(), src, dstTotal);
        return true;
    }

    const Channel r(rMask), g(gMask), b(bMask);
    const Channel a((pfFlags & kDdpfAlphaPixels) ? aMask : 0);
    std::byte* dst = out.pixels.data();
    for (const TextureData::Mip& mip : out.mips) {
        const std::size_t count = std::size_t{mip.width} * mip.height;
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t px = 0;
            std::memcpy(&px, src, srcBpp);
            src += srcBpp;
            *dst++ = std::byte{r.Get(px, 0)};
            *dst++ = std::byte{g.Get(px, 0)};
            *dst++ = std::byte{b.Get(px, 0)};
            *dst++ = std::byte{a.Get(px, 255)};
        }
    }
    return true;
}

bool DecodeDdj(std::span<const std::byte> file, TextureData& out, std::string& error) {
    constexpr std::size_t kDdjHeaderSize = 20;
    if (file.size() < kDdjHeaderSize || std::memcmp(file.data(), "JMXVDDJ 1000", 12) != 0) {
        error = "not a JMXVDDJ 1000 file";
        return false;
    }
    return DecodeDds(file.subspan(kDdjHeaderSize), out, error);
}

namespace {

// One BC colour block (8 bytes) into 16 RGBA pixels; `bc1` allows the 3-colour mode with a
// transparent black.
void ColourBlock(const std::uint8_t* b, bool bc1, std::uint8_t out[16][4]) {
    const std::uint16_t c0 = static_cast<std::uint16_t>(b[0] | b[1] << 8), c1 = static_cast<std::uint16_t>(b[2] | b[3] << 8);
    const auto expand = [](std::uint16_t c, std::uint8_t rgb[3]) {
        rgb[0] = static_cast<std::uint8_t>((c >> 11 & 31) * 255 / 31);
        rgb[1] = static_cast<std::uint8_t>((c >> 5 & 63) * 255 / 63);
        rgb[2] = static_cast<std::uint8_t>((c & 31) * 255 / 31);
    };
    std::uint8_t palette[4][4] = {};
    expand(c0, palette[0]);
    expand(c1, palette[1]);
    palette[0][3] = palette[1][3] = palette[2][3] = palette[3][3] = 255;
    for (int k = 0; k < 3; ++k) {
        if (c0 > c1 || !bc1) {
            palette[2][k] = static_cast<std::uint8_t>((2 * palette[0][k] + palette[1][k]) / 3);
            palette[3][k] = static_cast<std::uint8_t>((palette[0][k] + 2 * palette[1][k]) / 3);
        } else {
            palette[2][k] = static_cast<std::uint8_t>((palette[0][k] + palette[1][k]) / 2);
            palette[3][k] = 0;
        }
    }
    if (bc1 && c0 <= c1) palette[3][3] = 0;
    const std::uint32_t bits = static_cast<std::uint32_t>(b[4] | b[5] << 8 | b[6] << 16) | static_cast<std::uint32_t>(b[7]) << 24;
    for (int i = 0; i < 16; ++i) {
        const std::uint8_t* p = palette[bits >> (2 * i) & 3];
        for (int k = 0; k < 4; ++k) out[i][k] = p[k];
    }
}

} // namespace

std::vector<std::uint8_t> DecodeToRgba8(const TextureData& texture, std::size_t level) {
    const TextureData::Mip& mip = texture.mips[level];
    const std::uint32_t w = std::max(mip.width, 1u), h = std::max(mip.height, 1u);
    std::vector<std::uint8_t> rgba(std::size_t{w} * h * 4);
    const auto* src = reinterpret_cast<const std::uint8_t*>(texture.pixels.data() + mip.offset);
    if (texture.format == TextureFormat::RGBA8) {
        std::memcpy(rgba.data(), src, std::min(rgba.size(), mip.size));
        return rgba;
    }
    const std::size_t blockBytes = texture.format == TextureFormat::BC1 ? 8 : 16;
    const std::uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (std::uint32_t by = 0; by < bh; ++by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            const std::uint8_t* block = src + (std::size_t{by} * bw + bx) * blockBytes;
            if ((std::size_t{by} * bw + bx + 1) * blockBytes > mip.size) return rgba;
            std::uint8_t px[16][4];
            ColourBlock(texture.format == TextureFormat::BC1 ? block : block + 8, texture.format == TextureFormat::BC1, px);
            if (texture.format == TextureFormat::BC2) {
                for (int i = 0; i < 16; ++i) px[i][3] = static_cast<std::uint8_t>(((block[i / 2] >> (4 * (i % 2))) & 15) * 17);
            } else if (texture.format == TextureFormat::BC3) {
                const std::uint8_t a0 = block[0], a1 = block[1];
                std::uint8_t alphas[8] = {a0, a1};
                for (int k = 0; k < 6; ++k) {
                    alphas[2 + k] = a0 > a1 ? static_cast<std::uint8_t>(((6 - k) * a0 + (1 + k) * a1) / 7)
                                    : k < 4 ? static_cast<std::uint8_t>(((4 - k) * a0 + (1 + k) * a1) / 5)
                                    : static_cast<std::uint8_t>(k == 4 ? 0 : 255);
                }
                std::uint64_t bits = 0;
                for (int k = 0; k < 6; ++k) bits |= std::uint64_t{block[2 + k]} << (8 * k);
                for (int i = 0; i < 16; ++i) px[i][3] = alphas[bits >> (3 * i) & 7];
            }
            for (int i = 0; i < 16; ++i) {
                const std::uint32_t x = bx * 4 + static_cast<std::uint32_t>(i % 4), y = by * 4 + static_cast<std::uint32_t>(i / 4);
                if (x >= w || y >= h) continue;
                std::memcpy(&rgba[(std::size_t{y} * w + x) * 4], px[i], 4);
            }
        }
    }
    return rgba;
}

namespace {

// The next level down: each pixel the rounded mean of the 2 x 2 above it (the last row or column
// repeated at an odd edge).
std::vector<std::uint8_t> HalfLevel(const std::vector<std::uint8_t>& level, std::uint32_t w, std::uint32_t h, std::uint32_t nw, std::uint32_t nh) {
    std::vector<std::uint8_t> next(std::size_t{nw} * nh * 4);
    const std::uint8_t* src = level.data();
    std::uint8_t* dst = next.data();
    for (std::uint32_t y = 0; y < nh; ++y) {
        const std::uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
        for (std::uint32_t x = 0; x < nw; ++x) {
            const std::uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
            const std::uint8_t* a = src + (std::size_t{y0} * w + x0) * 4;
            const std::uint8_t* b = src + (std::size_t{y0} * w + x1) * 4;
            const std::uint8_t* c = src + (std::size_t{y1} * w + x0) * 4;
            const std::uint8_t* d = src + (std::size_t{y1} * w + x1) * 4;
            for (int k = 0; k < 4; ++k) *dst++ = static_cast<std::uint8_t>((unsigned{a[k]} + b[k] + c[k] + d[k] + 2) / 4);
        }
    }
    return next;
}

// --- The block encoder: the generated levels in the texture's own BC format, read back by the
// same palette rules as ColourBlock (so an encoded block decodes to the colours it chose).

std::uint16_t Pack565(const float c[3]) {
    const auto q = [](float v, int max) { return static_cast<std::uint16_t>(std::clamp(static_cast<int>(v * float(max) / 255.0f + 0.5f), 0, max)); };
    return static_cast<std::uint16_t>(q(c[0], 31) << 11 | q(c[1], 63) << 5 | q(c[2], 31));
}

// 16 pixels (RGBA) into a colour block. `bc1`: a pixel under half alpha is transparent (the
// 3-colour mode's fourth entry, as the original's alpha-tested DXT1 textures use it).
void EncodeColourBlock(const std::uint8_t px[16][4], bool bc1, std::uint8_t out[8]) {
    bool transparent[16] = {};
    bool anyTransparent = false;
    float mean[3] = {0, 0, 0};
    int opaque = 0;
    for (int i = 0; i < 16; ++i) {
        transparent[i] = bc1 && px[i][3] < 128;
        anyTransparent = anyTransparent || transparent[i];
        if (transparent[i]) continue;
        for (int k = 0; k < 3; ++k) mean[k] += px[i][k];
        ++opaque;
    }
    if (opaque == 0) {
        // All transparent: equal endpoints (the 3-colour mode) and every index the transparent one.
        std::memset(out, 0, 4);
        std::memset(out + 4, 0xFF, 4);
        return;
    }
    for (float& m : mean) m /= float(opaque);
    // The colours' main direction (the covariance's first eigenvector, by power iteration).
    float cov[6] = {};
    for (int i = 0; i < 16; ++i) {
        if (transparent[i]) continue;
        const float d[3] = {px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2]};
        cov[0] += d[0] * d[0], cov[1] += d[0] * d[1], cov[2] += d[0] * d[2];
        cov[3] += d[1] * d[1], cov[4] += d[1] * d[2], cov[5] += d[2] * d[2];
    }
    float axis[3] = {1, 1, 1};
    for (int it = 0; it < 8; ++it) {
        const float n[3] = {cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2], cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                            cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
        const float len = std::max({std::abs(n[0]), std::abs(n[1]), std::abs(n[2])});
        if (len < 1e-6f) break; // one colour: any axis does
        for (int k = 0; k < 3; ++k) axis[k] = n[k] / len;
    }
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < 16; ++i) {
        if (transparent[i]) continue;
        const float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
        lo = std::min(lo, t), hi = std::max(hi, t);
    }
    const float norm = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
    float e0[3], e1[3];
    for (int k = 0; k < 3; ++k) {
        e0[k] = mean[k] + axis[k] * hi / std::max(norm, 1e-6f);
        e1[k] = mean[k] + axis[k] * lo / std::max(norm, 1e-6f);
    }
    std::uint16_t c0 = Pack565(e0), c1 = Pack565(e1);
    // The mode is the endpoints' order: c0 > c1 four colours, c0 <= c1 three and transparent.
    if (anyTransparent ? c0 > c1 : c0 < c1) std::swap(c0, c1);
    out[0] = static_cast<std::uint8_t>(c0), out[1] = static_cast<std::uint8_t>(c0 >> 8);
    out[2] = static_cast<std::uint8_t>(c1), out[3] = static_cast<std::uint8_t>(c1 >> 8);
    // The palette as ColourBlock decodes it, then each pixel's nearest entry.
    std::uint8_t probe[8] = {out[0], out[1], out[2], out[3], 0, 0, 0, 0};
    std::uint8_t entries[16][4];
    std::uint8_t palette[4][4];
    for (std::uint32_t e = 0; e < 4; ++e) {
        const std::uint32_t bits = e * 0x55555555u; // every index e
        std::memcpy(probe + 4, &bits, 4);
        ColourBlock(probe, bc1, entries);
        std::memcpy(palette[e], entries[0], 4);
    }
    const bool threeColour = bc1 && c0 <= c1;
    std::uint32_t indices = 0;
    for (int i = 0; i < 16; ++i) {
        std::uint32_t best = 0;
        if (transparent[i]) {
            best = 3;
        } else {
            int bestDistance = 1 << 30;
            for (std::uint32_t e = 0; e < (threeColour ? 3u : 4u); ++e) {
                int d = 0;
                for (int k = 0; k < 3; ++k) d += (int(px[i][k]) - palette[e][k]) * (int(px[i][k]) - palette[e][k]);
                if (d < bestDistance) bestDistance = d, best = e;
            }
        }
        indices |= best << (2 * i);
    }
    std::memcpy(out + 4, &indices, 4);
}

// BC3's alpha half: the block's highest and lowest alpha as its ends (the 8-value mode), each
// pixel's nearest of the eight.
void EncodeAlphaBlock(const std::uint8_t px[16][4], std::uint8_t out[8]) {
    std::uint8_t a0 = 0, a1 = 255;
    for (int i = 0; i < 16; ++i) a0 = std::max(a0, px[i][3]), a1 = std::min(a1, px[i][3]);
    out[0] = a0, out[1] = a1;
    std::uint8_t alphas[8] = {a0, a1};
    for (int k = 0; k < 6; ++k) alphas[2 + k] = static_cast<std::uint8_t>(((6 - k) * a0 + (1 + k) * a1) / 7);
    std::uint64_t bits = 0;
    if (a0 > a1) {
        for (int i = 0; i < 16; ++i) {
            std::uint64_t best = 0;
            int bestDistance = 256;
            for (std::uint64_t e = 0; e < 8; ++e) {
                const int d = std::abs(int(px[i][3]) - alphas[e]);
                if (d < bestDistance) bestDistance = d, best = e;
            }
            bits |= best << (3 * i);
        }
    }
    for (int k = 0; k < 6; ++k) out[2 + k] = static_cast<std::uint8_t>(bits >> (8 * k));
}

// RGBA8 pixels into BC1, BC2 or BC3 blocks (a level under 4 x 4 repeats its edge pixels).
std::vector<std::uint8_t> EncodeBlocks(const std::vector<std::uint8_t>& rgba, std::uint32_t w, std::uint32_t h, TextureFormat format) {
    const std::uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    const std::size_t blockBytes = format == TextureFormat::BC1 ? 8 : 16;
    std::vector<std::uint8_t> out(std::size_t{bw} * bh * blockBytes);
    std::uint8_t px[16][4];
    for (std::uint32_t by = 0; by < bh; ++by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            for (int i = 0; i < 16; ++i) {
                const std::uint32_t x = std::min(bx * 4 + static_cast<std::uint32_t>(i % 4), w - 1);
                const std::uint32_t y = std::min(by * 4 + static_cast<std::uint32_t>(i / 4), h - 1);
                std::memcpy(px[i], &rgba[(std::size_t{y} * w + x) * 4], 4);
            }
            std::uint8_t* block = out.data() + (std::size_t{by} * bw + bx) * blockBytes;
            if (format == TextureFormat::BC1) {
                EncodeColourBlock(px, true, block);
                continue;
            }
            if (format == TextureFormat::BC2) {
                // Four bits a pixel, the even pixel in the low half (as the decoder reads them).
                for (int i = 0; i < 16; i += 2) {
                    const auto q = [&](int p) { return static_cast<std::uint8_t>((px[p][3] * 15 + 127) / 255); };
                    block[i / 2] = static_cast<std::uint8_t>(q(i) | q(i + 1) << 4);
                }
            } else {
                EncodeAlphaBlock(px, block);
            }
            EncodeColourBlock(px, false, block + 8);
        }
    }
    return out;
}

} // namespace

void CompleteMips(TextureData& texture) {
    if (texture.mips.size() != 1 || (texture.width <= 1 && texture.height <= 1)) return;
    std::vector<std::uint8_t> level = DecodeToRgba8(texture, 0);
    std::uint32_t w = std::max(texture.width, 1u), h = std::max(texture.height, 1u);
    // A compressed texture stays in its format: the top level as it came, the others encoded from
    // the box-filtered pixels (D3DX's way; decoded to RGBA8 they took 4 to 8 times the memory).
    const TextureFormat format = texture.format;
    const bool compressed = format != TextureFormat::RGBA8;
    std::vector<std::byte> pixels;
    std::vector<TextureData::Mip> mips;
    const auto append = [&](const std::uint8_t* data, std::size_t size, std::uint32_t mw, std::uint32_t mh) {
        mips.push_back({mw, mh, pixels.size(), size});
        const auto* b = reinterpret_cast<const std::byte*>(data);
        pixels.insert(pixels.end(), b, b + size);
    };
    if (compressed) {
        const TextureData::Mip& top = texture.mips[0];
        append(reinterpret_cast<const std::uint8_t*>(texture.pixels.data() + top.offset), top.size, w, h);
    } else {
        append(level.data(), level.size(), w, h);
    }
    while (w > 1 || h > 1) {
        const std::uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
        level = HalfLevel(level, w, h, nw, nh);
        if (compressed) {
            const std::vector<std::uint8_t> blocks = EncodeBlocks(level, nw, nh, format);
            append(blocks.data(), blocks.size(), nw, nh);
        } else {
            append(level.data(), level.size(), nw, nh);
        }
        w = nw;
        h = nh;
    }
    texture.pixels = std::move(pixels);
    texture.mips = std::move(mips);
}

} // namespace sr::assets
