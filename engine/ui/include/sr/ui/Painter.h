#pragma once

// Drawing the original's interface: textures by their paths, text in its fonts, in interface
// pixels (the original's: nothing is scaled) turned into
// screen pixels by the interface's scale (1 = the original).

#include <string>
#include <string_view>

#include "sr/ui/DrawList.h"
#include "sr/ui/Types.h"

namespace sr::ui {

class Font;

// What the interface needs from the program: its textures, its text table, its font.
class Resources {
public:
    virtual ~Resources() = default;
    struct Texture {
        TextureRef ref;
        float width = 0.0f, height = 0.0f;
        bool resident = false; // ready to draw
        bool failed = false;   // the file is missing or broken
    };
    // Requested on first use; not resident until it has streamed in.
    virtual Texture GetTexture(const std::string& path) = 0;
    // Asked for ahead of being shown (a map's tiles just off its view): loading meanwhile, not yet drawn. By default
    // the same as GetTexture.
    virtual void Prefetch(const std::string& path) { GetTexture(path); }
    // The text of a key (textuisystem.txt's English column); the key itself when it has none.
    virtual const std::string& Text(const std::string& key) const = 0;
    virtual Font* GetFont() const = 0;
};

// The original's interface fonts: Arial at 9, 8, 12, 11 and 15 points for indexes 0 to 4, each
// regular or bold (weight 600), made by height -MulDiv(points, 96, 72). Em heights in interface pixels.
float FontPixels(int fontIndex);

class Painter {
public:
    Painter(DrawList& list, Resources& resources, float scale) : list_(list), resources_(resources), scale_(scale) {}

    float scale() const { return scale_; }
    DrawList& list() { return list_; }
    Resources& resources() { return resources_; }
    Rect ToScreen(Rect r) const { return {r.x * scale_, r.y * scale_, r.w * scale_, r.h * scale_}; }

    // A texture's size in its own pixels (= interface pixels); zero until it is resident.
    Vec2 TextureSize(const std::string& path);
    // The file exists (false once loading it failed; true while it is still loading).
    bool HasTexture(const std::string& path);
    // Every texture asked for is resident or failed (a prefetch is not counted: nothing waits for it).
    bool Settled() const { return pending_ == 0; }
    // Asks for a texture that will soon be shown, without drawing it (Resources::Prefetch).
    void Prefetch(const std::string& path) {
        if (!path.empty()) resources_.Prefetch(path);
    }

    void Image(const std::string& path, Rect r, Vec2 uv0 = {0, 0}, Vec2 uv1 = {1, 1}, Color tint = kWhite);
    // The texture with its alpha left out (a picture under a mask: the mask's alpha alone).
    void ImageOpaque(const std::string& path, Rect r, Vec2 uv0 = {0, 0}, Vec2 uv1 = {1, 1});
    // The texture over `r` with a UV for each corner (top-left, top-right, bottom-right, bottom-left).
    void ImageCorners(const std::string& path, Rect r, const Vec2 (&uv)[4], Color tint = kWhite);
    // The texture turned a quarter turn clockwise over `r`.
    void ImageTurned(const std::string& path, Rect r, Color tint = kWhite);
    // The scene's overlay over `r`, the part of it that falls there (`screen`: the screen in
    // pixels): a window's 3D character.
    void SceneOverlay(Rect r, Vec2 screen);
    void Fill(Rect r, Color color);
    // A filled quad of any shape, corners in interface pixels.
    void FillQuad(const Vec2 (&p)[4], Color color);
    void Outline(Rect r, Color color);
    void PushClip(Rect r) { list_.PushClip(ToScreen(r)); }
    void PopClip() { list_.PopClip(); }
    // Until PopMask, what is drawn takes its alpha also from the texture at `path` stretched over
    // `r` (the original's second texture stage: the minimap's disc). False, and nothing pushed,
    // until the texture is resident.
    bool PushMask(const std::string& path, Rect r);
    void PopMask() { list_.PopMask(); }
    // Until PopDepth, what is drawn stands at that depth of the scene (0..1, the world's projection):
    // hidden where the scene is nearer.
    void PushDepth(float depth) { list_.PushDepth(depth); }
    // Draws until PopAdditive are added onto what is behind (DrawList::PushAdditive).
    void PushAdditive() { list_.PushAdditive(); }
    void PopAdditive() { list_.PopAdditive(); }
    void PopDepth() { list_.PopDepth(); }
    // The texture on a quad of four points (interface pixels, clockwise from its top-left corner),
    // with their UVs: markers turned to a direction.
    void ImageQuad(const std::string& path, const Vec2 (&p)[4], const Vec2 (&uv)[4], Color tint = kWhite);

    // hAlign 0 left, 1 centre, 2 right; vAlign 0 top, 1 centre, 2 bottom. Clipped to `r`.
    // `sizeScale` enlarges the font (the area banner's name is drawn half again as large).
    void Text(std::string_view text, Rect r, Color color, int fontIndex = 0, int hAlign = 0, int vAlign = 1, float sizeScale = 1.0f);
    float TextWidth(std::string_view text, int fontIndex = 0, float sizeScale = 1.0f);
    float LineHeight(int fontIndex = 0);

private:
    Resources::Texture Get(const std::string& path);

    DrawList& list_;
    Resources& resources_;
    float scale_;
    int pending_ = 0;
};

} // namespace sr::ui
