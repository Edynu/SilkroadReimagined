#pragma once

// The GPU-driven scene benchmark (C3b): `--instances N` copies of a few streamed
// models on a grid, sent once as world commands; the render thread culls and draws them on the
// GPU. The camera circles the field so culling changes every frame. With `--run-seconds` the
// frame statistics show whether the render thread's time depends on N (it must not).
// `--models a.bsr,b.bsr` picks the models.

#include <cstdint>
#include <string>
#include <vector>

#include "screens/Screen.h"
#include "sr/gfx/AssetStore.h"

namespace sr::client {

class SceneBenchScreen final : public Screen {
public:
    const char* Name() const override { return "scenebench"; }
    void Enter(ScreenContext& context) override;
    void Exit(ScreenContext& context, FrameSnapshot& snapshot) override;
    void Update(ScreenContext& context, FrameSnapshot& snapshot) override;

private:
    std::vector<gfx::AssetId> models_;
    std::uint32_t count_ = 0;
    bool placed_ = false;
    float extent_ = 0; // the field's half size
    OrbitCamera camera_;
};

} // namespace sr::client
