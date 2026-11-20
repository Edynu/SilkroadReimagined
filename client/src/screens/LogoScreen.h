#pragma once

// The first screen: the original's logo (interface\outer\logo-big.ddj), faded in and out, then
// the next screen; drawn by the game's own UI pass (no ImGui). The texture is read and decoded on
// a Streaming job and created from there (the device allows that), so the game thread never waits
// for it.

#include <atomic>
#include <memory>

#include "screens/Screen.h"
#include "sr/ui/DrawList.h"

namespace sr::client {

class LogoScreen final : public Screen {
public:
    const char* Name() const override { return "logo"; }
    void Enter(ScreenContext& context) override;
    void Exit(ScreenContext& context, FrameSnapshot& snapshot) override;
    void Update(ScreenContext& context, FrameSnapshot& snapshot) override;

private:
    // Shared with the loading job, which may finish after the screen is gone.
    struct Load {
        // Loading -> Loaded (by the job) or Loading -> Abandoned (by Exit): whichever side loses
        // the exchange frees the texture, so it is freed exactly once.
        enum State : int { Loading, Loaded, Abandoned };
        std::atomic<int> state{Loading};
        rhi::TextureHandle texture;
        std::uint32_t width = 0, height = 0;
    };
    std::shared_ptr<Load> load_;
    double shown_ = 0; // seconds since the logo appeared
    ui::DrawList list_; // the logo, for the game's UI pass
};

class EmptyScreen final : public Screen {
public:
    const char* Name() const override { return "empty"; }
    void Update(ScreenContext&, FrameSnapshot&) override {}
};

} // namespace sr::client
