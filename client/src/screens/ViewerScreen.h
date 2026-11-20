#pragma once

// The asset viewer on the new stack: any model (.bsr, .cpd, .bms) or
// texture (.ddj, .dds) of the archives, loaded through the asset store. Models are drawn by the
// render thread's scene renderer, skinned and animated; textures are shown with their mips; an
// effect (.efp, V1) plays at the origin through the effect system, kept 3 s, again each time it ends.
// `--open <path>` selects one at the start, `--animation N` plays a model's N-th animation.
// Right-drag turns the camera, the wheel zooms.

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "screens/Screen.h"
#include "sr/anim/Pose.h"
#include "sr/gfx/AssetStore.h"
#include "sr/gfx/Effects.h"

namespace sr::client {

class ViewerScreen final : public Screen {
public:
    const char* Name() const override { return "viewer"; }
    void Enter(ScreenContext& context) override;
    void Exit(ScreenContext& context, FrameSnapshot& snapshot) override;
    void Update(ScreenContext& context, FrameSnapshot& snapshot) override;

private:
    void Open(ScreenContext& context, const std::string& path);
    void Close(ScreenContext& context);
    void DrawBrowser(ScreenContext& context);
    void DrawDetails(ScreenContext& context);
    void Present(ScreenContext& context, FrameSnapshot& snapshot);

    // The archive's file list, built on a job (it walks every archive).
    struct Listing {
        std::atomic<bool> ready{false};
        std::vector<std::string> models, textures;
    };
    std::shared_ptr<Listing> listing_;
    bool showTextures_ = false;
    char filter_[128] = {};
    std::vector<int> filtered_; // indices into the shown list
    std::string filteredFor_;   // the filter + tab the list was built for

    std::string path_;
    gfx::AssetId asset_;
    bool isTexture_ = false;
    bool isEffect_ = false;
    gfx::EffectSystem effects_;
    gfx::EffectSystem::Handle effect_ = 0;
    double effectShown_ = 0; // seconds it has been drawn (a capture waits for some)
    void PresentEffect(ScreenContext& context, FrameSnapshot& snapshot);
    bool framed_ = false; // the camera was fitted to the model
    OrbitCamera camera_;
    int mip_ = -1;
    int channel_ = 0;
    float zoom_ = 1.0f;

    // Animation of a skinned model.
    std::shared_ptr<const gfx::ModelInfo> model_;
    anim::SkeletonPose pose_;
    bool poseReady_ = false;
    int animationIndex_ = -1;
    gfx::AssetId animation_;
    std::shared_ptr<const assets::AnimationData> playing_;
    double animationTime_ = 0;
    bool paused_ = false;
};

} // namespace sr::client
