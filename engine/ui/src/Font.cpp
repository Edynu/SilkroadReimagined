#include "sr/ui/Font.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

namespace sr::ui {

struct Font::State {
    stbtt_fontinfo info{};
    int ascent = 0, descent = 0, lineGap = 0;
};

Font::Font() { ResetAtlas(); }
Font::~Font() = default;

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
