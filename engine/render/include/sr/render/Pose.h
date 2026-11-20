#pragma once

#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace sr::assets {
struct SkeletonData;
struct AnimationData;
} // namespace sr::assets

namespace sr::render {

// Evaluates a skeleton's pose (bind pose or a sampled animation) and the
// skinning palette (world * inverse bind) for the GPU.
class SkeletonPose {
public:
    void Init(const assets::SkeletonData& skeleton);
    // Maps the animation's tracks to bones (by name). Call when the animation changes.
    void SetAnimation(const assets::AnimationData* animation);

    // timeMs is wrapped for looping animations and clamped otherwise.
    // inPlace keeps the root bone's horizontal position at the bind pose, so
    // walk/run cycles stay centred instead of moving away.
    void Evaluate(float timeMs, bool inPlace);

    // Body volume: per-bone factors that thicken a bone's skin around its length
    // axis (its local Y and Z scale in the skinning palette only; children and the
    // pose are unaffected). Empty or 1.0 = unchanged.
    void SetRadialScale(std::vector<float> perBone) { radial_ = std::move(perBone); }

    // Cross-fade: the next evaluations ease from the pose last evaluated into the new
    // animation over `durationMs` (advanced by AdvanceBlend), so motions do not snap.
    void StartBlend(float durationMs);
    void AdvanceBlend(float deltaMs) { blendLeft_ = blendLeft_ > deltaMs ? blendLeft_ - deltaMs : 0.0f; }

    const std::vector<glm::mat4>& World() const { return world_; }
    const std::vector<glm::mat4>& Palette() const { return palette_; }
    int MappedTracks() const { return mappedTracks_; }

private:
    const assets::SkeletonData* skeleton_ = nullptr;
    const assets::AnimationData* animation_ = nullptr;
    std::vector<int> trackBone_;        // animation track -> bone, -1 if unmatched
    std::vector<glm::mat4> inverseBind_;
    std::vector<glm::mat4> world_;
    std::vector<glm::mat4> palette_;
    std::vector<float> radial_;
    int motionRoot_ = -1;
    // The local pose last evaluated, and the one being faded from.
    std::vector<glm::quat> rotations_, fromRotations_;
    std::vector<glm::vec3> translations_, fromTranslations_;
    float blendLeft_ = 0.0f, blendTotal_ = 0.0f;
    int mappedTracks_ = 0;
};

} // namespace sr::render
