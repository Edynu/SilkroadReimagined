#include "sr/assets/ExeCursors.h"

#include <cstring>
#include <map>

namespace sr::assets {

// PE layout used here: e_lfanew at 0x3C; "PE\0\0"; the COFF header (sections count at
// +2, optional header size at +16); the optional header (magic 0x10B / 0x20B, the data
// directories at +96 / +112, the resource directory is entry 2); the section table
// (40 bytes each: virtual size +8, RVA +12, raw size +16, raw offset +20).
// Resource tree: a directory (16 bytes, named / id entry counts at +12 / +14), then 8-byte
// entries (id, offset; the high bit of the offset marks a subdirectory); type -> id ->
// language -> a data entry (RVA, size). RT_CURSOR = 1, RT_GROUP_CURSOR = 12.
// A group: u16 0, u16 2, u16 count, then 14-byte entries (..., u16 resource id at +12).
// A cursor: u16 hotspot x, u16 hotspot y, then a DIB: BITMAPINFOHEADER (40 bytes; the
// height covers the colour and the mask), a palette for <= 8 bits, the colour rows
// bottom-up (padded to 4 bytes), then the 1-bit AND mask rows.

namespace {
struct Reader {
    std::span<const std::byte> data;
    bool ok = true;
    template <typename T>
    T At(std::size_t offset) {
        T v{};
        if (offset + sizeof(T) > data.size()) {
            ok = false;
            return v;
        }
        std::memcpy(&v, data.data() + offset, sizeof(T));
        return v;
    }
};

struct Section {
    std::uint32_t rva, size, raw, rawSize;
};

bool DecodeDib(std::span<const std::byte> res, CursorImage& image) {
    Reader r{res};
    image.hotX = r.At<std::uint16_t>(0);
    image.hotY = r.At<std::uint16_t>(2);
    const std::size_t dib = 4;
    const std::uint32_t headerSize = r.At<std::uint32_t>(dib);
    const std::int32_t width = r.At<std::int32_t>(dib + 4);
    const std::int32_t height = r.At<std::int32_t>(dib + 8) / 2;
    const std::uint16_t bits = r.At<std::uint16_t>(dib + 14);
    std::uint32_t colours = r.At<std::uint32_t>(dib + 32);
    if (!r.ok || width <= 0 || height <= 0 || width > 256 || height > 256) return false;
    if (bits <= 8 && colours == 0) colours = 1u << bits;
    const std::size_t palette = dib + headerSize;
    const std::size_t pixels = palette + (bits <= 8 ? colours * 4 : 0);
    const std::size_t colourStride = ((static_cast<std::size_t>(width) * bits + 31) / 32) * 4;
    const std::size_t maskStride = ((static_cast<std::size_t>(width) + 31) / 32) * 4;
    const std::size_t mask = pixels + colourStride * static_cast<std::size_t>(height);
    if (mask + maskStride * static_cast<std::size_t>(height) > res.size()) return false;
    image.width = width;
    image.height = height;
    image.rgba.assign(static_cast<std::size_t>(width * height * 4), 0);
    bool anyAlpha = false;
    for (int y = 0; y < height; ++y) {
        const std::size_t row = pixels + colourStride * static_cast<std::size_t>(height - 1 - y);
        const std::size_t maskRow = mask + maskStride * static_cast<std::size_t>(height - 1 - y);
        for (int x = 0; x < width; ++x) {
            std::uint8_t red = 0, green = 0, blue = 0, alpha = 255;
            if (bits == 32) {
                blue = r.At<std::uint8_t>(row + x * 4);
                green = r.At<std::uint8_t>(row + x * 4 + 1);
                red = r.At<std::uint8_t>(row + x * 4 + 2);
                alpha = r.At<std::uint8_t>(row + x * 4 + 3);
                anyAlpha |= alpha != 0;
            } else if (bits == 24) {
                blue = r.At<std::uint8_t>(row + x * 3);
                green = r.At<std::uint8_t>(row + x * 3 + 1);
                red = r.At<std::uint8_t>(row + x * 3 + 2);
            } else if (bits <= 8) {
                const std::uint8_t byte = r.At<std::uint8_t>(row + (static_cast<std::size_t>(x) * bits) / 8);
                const int shift = 8 - bits - static_cast<int>((static_cast<std::size_t>(x) * bits) % 8);
                const std::uint32_t index = (byte >> shift) & ((1u << bits) - 1);
                blue = r.At<std::uint8_t>(palette + index * 4);
                green = r.At<std::uint8_t>(palette + index * 4 + 1);
                red = r.At<std::uint8_t>(palette + index * 4 + 2);
            } else {
                return false;
            }
            const bool transparent = (r.At<std::uint8_t>(maskRow + x / 8) >> (7 - x % 8)) & 1;
            std::uint8_t* out = &image.rgba[static_cast<std::size_t>((y * width + x) * 4)];
            out[0] = red;
            out[1] = green;
            out[2] = blue;
            // The AND mask decides, unless 32-bit pixels carry their own alpha. Masked pixels
            // are see-through (an "invert" pixel cannot be drawn by a colour cursor).
            if (bits == 32) {
                out[3] = alpha;
            } else {
                out[3] = transparent ? 0 : 255;
                if (transparent) out[0] = out[1] = out[2] = 0;
            }
        }
    }
    if (bits == 32 && !anyAlpha) {
        // No alpha in the pixels: use the mask.
        for (int y = 0; y < height; ++y) {
            const std::size_t maskRow = mask + maskStride * static_cast<std::size_t>(height - 1 - y);
            for (int x = 0; x < width; ++x) {
                const bool transparent = (r.At<std::uint8_t>(maskRow + x / 8) >> (7 - x % 8)) & 1;
                image.rgba[static_cast<std::size_t>((y * width + x) * 4 + 3)] = transparent ? 0 : 255;
            }
        }
    }
    return r.ok;
}
} // namespace

bool DecodeExeCursors(std::span<const std::byte> exe, std::vector<CursorImage>& out, std::string& error) {
    out.clear();
    Reader r{exe};
    if (r.At<std::uint16_t>(0) != 0x5A4D) {
        error = "not an executable";
        return false;
    }
    const std::uint32_t pe = r.At<std::uint32_t>(0x3C);
    if (r.At<std::uint32_t>(pe) != 0x00004550) {
        error = "no PE header";
        return false;
    }
    const std::size_t coff = pe + 4;
    const std::uint16_t sectionCount = r.At<std::uint16_t>(coff + 2);
    const std::uint16_t optionalSize = r.At<std::uint16_t>(coff + 16);
    const std::size_t optional = coff + 20;
    const std::uint16_t magic = r.At<std::uint16_t>(optional);
    const std::size_t directories = optional + (magic == 0x20B ? 112 : 96);
    const std::uint32_t resourceRva = r.At<std::uint32_t>(directories + 2 * 8);
    std::vector<Section> sections;
    for (std::size_t i = 0; i < sectionCount; ++i) {
        const std::size_t s = optional + optionalSize + i * 40;
        sections.push_back({r.At<std::uint32_t>(s + 12), r.At<std::uint32_t>(s + 8), r.At<std::uint32_t>(s + 20), r.At<std::uint32_t>(s + 16)});
    }
    if (!r.ok || resourceRva == 0) {
        error = "no resources";
        return false;
    }
    const auto toFile = [&](std::uint32_t rva) -> std::size_t {
        for (const Section& s : sections) {
            if (rva >= s.rva && rva < s.rva + std::max(s.size, s.rawSize)) return s.raw + (rva - s.rva);
        }
        return 0;
    };
    const std::size_t root = toFile(resourceRva);
    if (root == 0) {
        error = "resources outside every section";
        return false;
    }
    // id -> data entry (file offset, size), for one resource type.
    const auto entries = [&](std::uint32_t type) {
        std::map<std::uint16_t, std::pair<std::size_t, std::uint32_t>> found;
        const auto children = [&](std::size_t dir, auto&& visit) {
            const std::uint16_t named = r.At<std::uint16_t>(dir + 12), ids = r.At<std::uint16_t>(dir + 14);
            for (std::size_t i = 0; i < static_cast<std::size_t>(named + ids) && r.ok; ++i) {
                const std::size_t e = dir + 16 + i * 8;
                visit(r.At<std::uint32_t>(e), r.At<std::uint32_t>(e + 4));
            }
        };
        children(root, [&](std::uint32_t id, std::uint32_t offset) {
            if (id != type || !(offset & 0x80000000u)) return;
            children(root + (offset & 0x7FFFFFFFu), [&](std::uint32_t resId, std::uint32_t langs) {
                if (!(langs & 0x80000000u) || (resId & 0x80000000u)) return;
                children(root + (langs & 0x7FFFFFFFu), [&](std::uint32_t, std::uint32_t data) {
                    if (data & 0x80000000u) return;
                    const std::size_t entry = root + data;
                    found.emplace(static_cast<std::uint16_t>(resId), std::make_pair(toFile(r.At<std::uint32_t>(entry)), r.At<std::uint32_t>(entry + 4)));
                });
            });
        });
        return found;
    };
    const auto cursors = entries(1);
    for (const auto& [groupId, group] : entries(12)) {
        const auto [offset, size] = group;
        if (offset == 0 || r.At<std::uint16_t>(offset + 4) == 0) continue;
        // The first image of the group.
        const std::uint16_t cursorId = r.At<std::uint16_t>(offset + 6 + 12);
        const auto it = cursors.find(cursorId);
        if (it == cursors.end() || it->second.first == 0 || it->second.first + it->second.second > exe.size()) continue;
        CursorImage image;
        image.id = groupId;
        if (DecodeDib(exe.subspan(it->second.first, it->second.second), image)) out.push_back(std::move(image));
        (void)size;
    }
    if (out.empty()) {
        error = "no cursors";
        return false;
    }
    return true;
}

} // namespace sr::assets
