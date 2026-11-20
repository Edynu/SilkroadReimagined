#pragma once

// Resolution-independent UI layout. Screens are authored on the original game's
// 1024 x 768 design grid; DesignGrid maps that grid onto any window with one
// uniform scale, centred (letterboxed), so the layout never stretches.

#include <algorithm>

namespace sr::core {

struct DesignPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct DesignRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool Contains(DesignPoint p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
};

class DesignGrid {
public:
    static constexpr float kWidth = 1024.0f;
    static constexpr float kHeight = 768.0f;

    // `windowWidth` / `windowHeight` in pixels. Zero sizes leave scale 1, offset 0.
    void Fit(float windowWidth, float windowHeight) {
        if (windowWidth <= 0.0f || windowHeight <= 0.0f) {
            scale_ = 1.0f;
            offsetX_ = offsetY_ = 0.0f;
            return;
        }
        scale_ = std::min(windowWidth / kWidth, windowHeight / kHeight);
        offsetX_ = (windowWidth - kWidth * scale_) * 0.5f;
        offsetY_ = (windowHeight - kHeight * scale_) * 0.5f;
        width_ = kWidth;
        height_ = kHeight;
    }
    // The in-game interface: the same scale (so it grows with the window, as it
    // looks at 1024 x 768), but the whole window is the virtual screen - wider or
    // taller than 1024 x 768 - and windows anchor to its edges.
    void FitAnchored(float windowWidth, float windowHeight) {
        Fit(windowWidth, windowHeight);
        offsetX_ = offsetY_ = 0.0f;
        if (windowWidth > 0.0f && windowHeight > 0.0f) {
            width_ = windowWidth / scale_;
            height_ = windowHeight / scale_;
        }
    }
    // The virtual screen in design units (1024 x 768 after Fit).
    float width() const { return width_; }
    float height() const { return height_; }

    float scale() const { return scale_; }
    DesignPoint ToScreen(DesignPoint p) const { return {offsetX_ + p.x * scale_, offsetY_ + p.y * scale_}; }
    DesignRect ToScreen(DesignRect r) const {
        const DesignPoint o = ToScreen(DesignPoint{r.x, r.y});
        return {o.x, o.y, r.w * scale_, r.h * scale_};
    }
    DesignPoint ToDesign(DesignPoint screen) const { return {(screen.x - offsetX_) / scale_, (screen.y - offsetY_) / scale_}; }

private:
    float scale_ = 1.0f;
    float offsetX_ = 0.0f;
    float offsetY_ = 0.0f;
    float width_ = kWidth;
    float height_ = kHeight;
};

} // namespace sr::core
