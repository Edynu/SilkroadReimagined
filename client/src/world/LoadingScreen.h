#pragma once

// The original's loading screen: a picture and a
// gauge, shown from the moment a place starts loading until it is settled, then closed at once (no
// minimum time, no fade); the world is neither drawn nor updated meanwhile. Layout in the
// original's 1600 x 1200 design units: the picture fills the screen's height at 4:3, centred; the
// gauge's frame, its bar and the "now loading" caption are stretched per axis.

#include <string>

#include "sr/gfx/AssetStore.h"
#include "sr/ui/DrawList.h"

namespace sr::client {

class LoadingScreen {
public:
    // Shows `picture` (falls back to the China picture if the archive lacks it).
    void Begin(gfx::AssetStore& assets, const std::string& picture);
    void End(gfx::AssetStore& assets);
    bool Active() const { return active_; }
    // 0..1; the bar never goes back.
    void SetProgress(float progress);
    float Progress() const { return progress_; }
    // Its quads, over a black screen of `width` x `height` pixels.
    void Draw(gfx::AssetStore& assets, ui::DrawList& list, float width, float height);
    // Everything it shows resident (or failed): a capture waits for it.
    bool Ready(const gfx::AssetStore& assets) const;

private:
    bool active_ = false;
    float progress_ = 0.0f;
    gfx::AssetId picture_, fallback_, form_, gauge_, caption_;
};

} // namespace sr::client
