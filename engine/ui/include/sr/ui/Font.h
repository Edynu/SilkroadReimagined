#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sr/ui/DrawList.h"

namespace sr::ui {

// A TrueType font rasterised on demand into an RGBA atlas: white glyphs with coverage in alpha, plus one white texel
// for fills. Sizes are em heights in pixels, like GDI's negative lfHeight the original uses. Two ways to draw the
// letters: the original's (the default; 1.188's GetGlyphOutline GGO_BITMAP: hinted by the font's own TrueType
// instructions, every pixel on or off; FreeType's classic interpreter in mono mode) or smooth (ours, stb_truetype:
// unhinted, anti-aliased). The renderer uploads the atlas when it changed and tells the font its texture.
class Font {
public:
    Font();
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    bool Load(std::vector<std::byte> ttf, std::string& error);
    // A second face for the code points the first lacks (as Windows' font linking draws them for the original:
    // the Forgotten World's grade stars with Arial); its glyphs at the same pixel size.
    bool LoadFallback(std::vector<std::byte> ttf, std::string& error);
    bool loaded() const { return font_ != nullptr; }
    // Smooth letters (ours) or the original's 1-bit ones; a change draws every glyph again.
    void SetSmooth(bool smooth);
    bool smooth() const { return smooth_; }

    // Width and height of UTF-8 text (one line per '\n').
    Vec2 Measure(std::string_view text, float pixelSize);
    float LineHeight(float pixelSize) const;
    float Ascent(float pixelSize) const;
    // Text with its top-left at `pos`, inside the draw list's current clip.
    void Draw(DrawList& list, std::string_view text, Vec2 pos, float pixelSize, Color color);
    // Breaks text into lines no wider than `width` (at spaces; long words are cut).
    std::vector<std::string> Wrap(std::string_view text, float pixelSize, float width);

    // The atlas (RGBA8, row-major). dirty() until the renderer has uploaded it.
    int atlasWidth() const { return atlasWidth_; }
    int atlasHeight() const { return atlasHeight_; }
    const std::vector<std::uint8_t>& atlasPixels() const { return atlas_; }
    bool dirty() const { return dirty_; }
    // The atlas rows changed since the last upload: [dirtyTop, dirtyBottom).
    int dirtyTop() const { return dirtyTop_; }
    int dirtyBottom() const { return dirtyBottom_; }
    void MarkUploaded(TextureRef texture) {
        texture_ = texture;
        dirty_ = false;
        dirtyTop_ = atlasHeight_;
        dirtyBottom_ = 0;
    }
    TextureRef texture() const { return texture_; }
    Vec2 whiteUv() const { return {0.5f / static_cast<float>(atlasWidth_), 0.5f / static_cast<float>(atlasHeight_)}; }

private:
    struct Glyph {
        float advance = 0;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // quad relative to the pen on the baseline
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    };
    const Glyph& GlyphFor(std::uint32_t codepoint, int pixelSize);
    void ResetAtlas();

    struct State;
    struct Mono;
    std::unique_ptr<State> font_; // stb_truetype font info
    bool smooth_ = false;
    void LoadMono();
    std::vector<std::byte> ttf_;
    std::unique_ptr<State> fallback_;
    std::vector<std::byte> fallbackTtf_;
    std::unique_ptr<Mono> mono_; // FreeType's faces over the bytes above (the original's letters), gone before them
    std::unordered_map<std::uint64_t, Glyph> glyphs_; // (size << 32) | codepoint
    std::vector<std::uint8_t> atlas_;
    int atlasWidth_ = 1024, atlasHeight_ = 1024;
    int penX_ = 0, penY_ = 0, rowHeight_ = 0;
    bool dirty_ = true;
    int dirtyTop_ = 0, dirtyBottom_ = 1024;
    TextureRef texture_;
};

// Decodes one UTF-8 code point at `i`, advancing it (invalid bytes give U+FFFD).
std::uint32_t NextCodepoint(std::string_view text, std::size_t& i);

} // namespace sr::ui
