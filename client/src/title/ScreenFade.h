#pragma once

// A black over the whole screen fading in or out (the title screens' GDR_SCREENFADE / GDR_FADE:
// the create screens fade in from black over 0.5 s). A screen leaves once
// its fade out is done.

#include <algorithm>
#include <cstdint>

#include "title/UiKit.h"

namespace sr::client {

struct ScreenFade {
    float black = 1.0f;  // 0 clear .. 1 black
    float target = 1.0f;
    float seconds = 0.5f;

    void In(float s = 0.5f) {
        target = 0.0f;
        seconds = s;
    }
    void Out(float s = 0.5f) {
        target = 1.0f;
        seconds = s;
    }
    void Update(float dt) {
        const float step = seconds > 0.0f ? dt / seconds : 1.0f;
        black = target > black ? std::min(target, black + step) : std::max(target, black - step);
    }
    bool Done() const { return black == target; }
    // Over everything the screen drew before it (pixels).
    void Draw(UiKit& kit, float width, float height) const {
        if (black > 0.0f) kit.FillScreen({0, 0, width, height}, ui::Rgba(0, 0, 0, static_cast<std::uint8_t>(255.0f * black)));
    }
};

} // namespace sr::client
