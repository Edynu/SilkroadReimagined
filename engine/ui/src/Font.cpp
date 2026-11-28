#include "sr/ui/Font.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_DRIVER_H
#include FT_MODULE_H

namespace sr::ui {

struct Font::State {
    stbtt_fontinfo info{};
    int ascent = 0, descent = 0, lineGap = 0;
};

// FreeType with the classic TrueType interpreter (v35: the font's instructions applied in both directions, as
// Windows' rasteriser of 1.188's time did), the face and its fallback over the loaded bytes.
struct Font::Mono {
    FT_Library library = nullptr;
    FT_Face face = nullptr, fallback = nullptr;
    ~Mono() {
        if (fallback != nullptr) FT_Done_Face(fallback);
        if (face != nullptr) FT_Done_Face(face);
        if (library != nullptr) FT_Done_FreeType(library);
    }
};

Font::Font() { ResetAtlas(); }
Font::~Font() = default;

void Font::LoadMono() {
    mono_.reset();
    if (ttf_.empty()) return;
    auto m = std::make_unique<Mono>();
    if (FT_Init_FreeType(&m->library) != 0) return;
    FT_UInt version = TT_INTERPRETER_VERSION_35;
    FT_Property_Set(m->library, "truetype", "interpreter-version", &version);
    if (FT_New_Memory_Face(m->library, reinterpret_cast<const FT_Byte*>(ttf_.data()), FT_Long(ttf_.size()), 0, &m->face) != 0) return;
    if (!fallbackTtf_.empty()) {
        FT_New_Memory_Face(m->library, reinterpret_cast<const FT_Byte*>(fallbackTtf_.data()), FT_Long(fallbackTtf_.size()), 0, &m->fallback);
    }
    mono_ = std::move(m);
}

void Font::SetSmooth(bool smooth) {
    if (smooth == smooth_) return;
    smooth_ = smooth;
    ResetAtlas();
}

std::uint32_t NextCodepoint(std::string_view s, std::size_t& i) {
    const auto byte = [&](std::size_t at) { return static_cast<std::uint8_t>(s[at]); };
    const std::uint8_t c = byte(i);
    int length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (length == 0 || i + static_cast<std::size_t>(length) > s.size()) {
        ++i;
        return 0xFFFD;
    }
    std::uint32_t cp = length == 1 ? c : length == 2 ? (c & 0x1Fu) : length == 3 ? (c & 0x0Fu) : (c & 0x07u);
    for (int k = 1; k < length; ++k) {
        const std::uint8_t next = byte(i + static_cast<std::size_t>(k));
        if ((next & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (next & 0x3Fu);
    }
    i += static_cast<std::size_t>(length);
    return cp;
}

bool Font::Load(std::vector<std::byte> ttf, std::string& error) {
    ttf_ = std::move(ttf);
    auto state = std::make_unique<State>();
    const auto* data = reinterpret_cast<const unsigned char*>(ttf_.data());
    const int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&state->info, data, offset)) {
        error = "not a TrueType font";
        return false;
    }
    stbtt_GetFontVMetrics(&state->info, &state->ascent, &state->descent, &state->lineGap);
    font_ = std::move(state);
    LoadMono();
    glyphs_.clear();
    ResetAtlas();
    return true;
}

bool Font::LoadFallback(std::vector<std::byte> ttf, std::string& error) {
    fallbackTtf_ = std::move(ttf);
    auto state = std::make_unique<State>();
    const auto* data = reinterpret_cast<const unsigned char*>(fallbackTtf_.data());
    const int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&state->info, data, offset)) {
        error = "not a TrueType font";
        fallbackTtf_.clear();
        return false;
    }
    stbtt_GetFontVMetrics(&state->info, &state->ascent, &state->descent, &state->lineGap);
    fallback_ = std::move(state);
    LoadMono();
    glyphs_.clear();
    ResetAtlas();
    return true;
}

void Font::ResetAtlas() {
    atlas_.assign(static_cast<std::size_t>(atlasWidth_) * atlasHeight_ * 4, 0);
    // Texel (0, 0) is opaque white, for fills.
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) std::memset(&atlas_[(static_cast<std::size_t>(y) * atlasWidth_ + x) * 4], 255, 4);
    }
    penX_ = 4;
    penY_ = 0;
    rowHeight_ = 4;
    glyphs_.clear();
    dirty_ = true;
    dirtyTop_ = 0;
    dirtyBottom_ = atlasHeight_;
}

float Font::Ascent(float pixelSize) const {
    if (!font_) return pixelSize;
    return static_cast<float>(font_->ascent) * stbtt_ScaleForMappingEmToPixels(&font_->info, pixelSize);
}

float Font::LineHeight(float pixelSize) const {
    if (!font_) return pixelSize * 1.2f;
    const float scale = stbtt_ScaleForMappingEmToPixels(&font_->info, pixelSize);
    return std::ceil(static_cast<float>(font_->ascent - font_->descent + font_->lineGap) * scale);
}

const Font::Glyph& Font::GlyphFor(std::uint32_t codepoint, int pixelSize) {
    const std::uint64_t key = static_cast<std::uint64_t>(pixelSize) << 32 | codepoint;
    if (const auto it = glyphs_.find(key); it != glyphs_.end()) return it->second;
    Glyph glyph;
    // Where a glyph's pixels go in the atlas (a new row, or the atlas started over when full); false when it started over.
    const auto place = [&](int w, int h) {
        if (penX_ + w + 1 > atlasWidth_) {
            penX_ = 0;
            penY_ += rowHeight_ + 1;
            rowHeight_ = 0;
        }
        if (penY_ + h + 1 > atlasHeight_) {
            ResetAtlas();
            return false;
        }
        return true;
    };
    const auto placed = [&](int x0, int y0, int w, int h) {
        glyph.x0 = static_cast<float>(x0);
        glyph.y0 = static_cast<float>(y0);
        glyph.x1 = static_cast<float>(x0 + w);
        glyph.y1 = static_cast<float>(y0 + h);
        glyph.u0 = static_cast<float>(penX_) / atlasWidth_;
        glyph.v0 = static_cast<float>(penY_) / atlasHeight_;
        glyph.u1 = static_cast<float>(penX_ + w) / atlasWidth_;
        glyph.v1 = static_cast<float>(penY_ + h) / atlasHeight_;
        dirtyTop_ = std::min(dirtyTop_, penY_);
        dirtyBottom_ = std::max(dirtyBottom_, penY_ + h);
        penX_ += w + 1;
        rowHeight_ = std::max(rowHeight_, h);
        dirty_ = true;
    };
    // The original's letters: hinted at this size, rendered 1-bit (each set bit a full pixel), the hinted advance.
    if (!smooth_ && mono_ && mono_->face != nullptr) {
        FT_Face face = mono_->face;
        FT_UInt index = FT_Get_Char_Index(face, codepoint);
        if (index == 0 && codepoint != ' ' && mono_->fallback != nullptr) {
            if (const FT_UInt other = FT_Get_Char_Index(mono_->fallback, codepoint); other != 0) {
                face = mono_->fallback;
                index = other;
            }
        }
        if (index == 0 && codepoint != ' ') index = FT_Get_Char_Index(face, '?');
        if (FT_Set_Pixel_Sizes(face, 0, FT_UInt(pixelSize)) == 0 && FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO) == 0) {
            const FT_GlyphSlot slot = face->glyph;
            glyph.advance = static_cast<float>((slot->advance.x + 32) >> 6);
            const FT_Bitmap& b = slot->bitmap;
            const int w = int(b.width), h = int(b.rows);
            if (w > 0 && h > 0 && b.pixel_mode == FT_PIXEL_MODE_MONO) {
                if (!place(w, h)) return GlyphFor(codepoint, pixelSize);
                for (int y = 0; y < h; ++y) {
                    const unsigned char* row = b.buffer + static_cast<std::ptrdiff_t>(y) * b.pitch;
                    for (int x = 0; x < w; ++x) {
                        std::uint8_t* px = &atlas_[(static_cast<std::size_t>(penY_ + y) * atlasWidth_ + penX_ + x) * 4];
                        px[0] = px[1] = px[2] = 255;
                        px[3] = (row[x >> 3] & (0x80 >> (x & 7))) != 0 ? 255 : 0;
                    }
                }
                placed(slot->bitmap_left, -slot->bitmap_top, w, h);
            }
        }
        return glyphs_.emplace(key, glyph).first->second;
    }
    if (font_) {
        const stbtt_fontinfo* info = &font_->info;
        int index = stbtt_FindGlyphIndex(info, static_cast<int>(codepoint));
        if (index == 0 && codepoint != ' ' && fallback_) {
            if (const int other = stbtt_FindGlyphIndex(&fallback_->info, static_cast<int>(codepoint)); other != 0) {
                info = &fallback_->info;
                index = other;
            }
        }
        if (index == 0 && codepoint != ' ') index = stbtt_FindGlyphIndex(info, '?');
        const float scale = stbtt_ScaleForMappingEmToPixels(info, static_cast<float>(pixelSize));
        int advance = 0, bearing = 0;
        stbtt_GetGlyphHMetrics(info, index, &advance, &bearing);
        glyph.advance = std::round(static_cast<float>(advance) * scale);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(info, index, scale, scale, &x0, &y0, &x1, &y1);
        const int w = x1 - x0, h = y1 - y0;
        if (w > 0 && h > 0) {
            if (penX_ + w + 1 > atlasWidth_) {
                penX_ = 0;
                penY_ += rowHeight_ + 1;
                rowHeight_ = 0;
            }
            if (penY_ + h + 1 > atlasHeight_) {
                // Full: start over (the glyphs in use come back on demand).
                ResetAtlas();
                return GlyphFor(codepoint, pixelSize);
            }
            std::vector<unsigned char> coverage(static_cast<std::size_t>(w) * h);
            stbtt_MakeGlyphBitmap(info, coverage.data(), w, h, w, scale, scale, index);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    std::uint8_t* px = &atlas_[(static_cast<std::size_t>(penY_ + y) * atlasWidth_ + penX_ + x) * 4];
                    px[0] = px[1] = px[2] = 255;
                    px[3] = coverage[static_cast<std::size_t>(y) * w + x];
                }
            }
            glyph.x0 = static_cast<float>(x0);
            glyph.y0 = static_cast<float>(y0);
            glyph.x1 = static_cast<float>(x1);
            glyph.y1 = static_cast<float>(y1);
            glyph.u0 = static_cast<float>(penX_) / atlasWidth_;
            glyph.v0 = static_cast<float>(penY_) / atlasHeight_;
            glyph.u1 = static_cast<float>(penX_ + w) / atlasWidth_;
            glyph.v1 = static_cast<float>(penY_ + h) / atlasHeight_;
            dirtyTop_ = std::min(dirtyTop_, penY_);
            dirtyBottom_ = std::max(dirtyBottom_, penY_ + h);
            penX_ += w + 1;
            rowHeight_ = std::max(rowHeight_, h);
            dirty_ = true;
        }
    }
    return glyphs_.emplace(key, glyph).first->second;
}

Vec2 Font::Measure(std::string_view text, float pixelSize) {
    const int size = std::max(1, static_cast<int>(std::lround(pixelSize)));
    float width = 0, line = 0;
    int lines = text.empty() ? 0 : 1;
    for (std::size_t i = 0; i < text.size();) {
        const std::uint32_t cp = NextCodepoint(text, i);
        if (cp == '\n') {
            width = std::max(width, line);
            line = 0;
            ++lines;
            continue;
        }
        line += GlyphFor(cp, size).advance;
    }
    return {std::max(width, line), static_cast<float>(lines) * LineHeight(static_cast<float>(size))};
}

void Font::Draw(DrawList& list, std::string_view text, Vec2 pos, float pixelSize, Color color) {
    const int size = std::max(1, static_cast<int>(std::lround(pixelSize)));
    const float baseline = std::round(pos.y + Ascent(static_cast<float>(size)));
    float x = std::round(pos.x), y = baseline;
    for (std::size_t i = 0; i < text.size();) {
        const std::uint32_t cp = NextCodepoint(text, i);
        if (cp == '\n') {
            x = std::round(pos.x);
            y += LineHeight(static_cast<float>(size));
            continue;
        }
        const Glyph& g = GlyphFor(cp, size);
        if (g.x1 > g.x0) {
            list.Image(texture_, {x + g.x0, y + g.y0, g.x1 - g.x0, g.y1 - g.y0}, {g.u0, g.v0}, {g.u1, g.v1}, color);
        }
        x += g.advance;
    }
}

std::vector<std::string> Font::Wrap(std::string_view text, float pixelSize, float width) {
    std::vector<std::string> lines;
    std::string line, word;
    const auto flushWord = [&] {
        if (word.empty()) return;
        const std::string joined = line.empty() ? word : line + " " + word;
        if (line.empty() || Measure(joined, pixelSize).x <= width) {
            line = joined;
        } else {
            lines.push_back(line);
            line = word;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == '\n') {
            flushWord();
            lines.push_back(line);
            line.clear();
        } else if (c == ' ') {
            flushWord();
        } else {
            word += c;
        }
    }
    flushWord();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

} // namespace sr::ui
