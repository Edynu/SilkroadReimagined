#pragma once

// sr::anim - skeletal animation on the poses and skinning palettes from
// .bsk skeletons and .ban animations. Started from the prototype's render/Pose (C2); the
// animation mixer of C8 grows here.

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace sr::assets {
struct SkeletonData;
struct AnimationData;
} // namespace sr::assets

namespace sr::anim {

// Evaluates a skeleton's pose (bind pose or a sampled animation) and the
// skinning palette (world * inverse bind) for the GPU.
class SkeletonPose {
public:
    void Init(const assets::SkeletonData& skeleton);
    // Maps the animation's tracks to bones (by name). Call when the animation changes.
    void SetAnimation(const assets::AnimationData* animation);

    // A clip of a few bones only (a hit's flinch: the original's chinaman_a_benormalhit moves 4 of 43)
    // played over the animation: its bones take its keys, the rest keep the animation's. Null: none.
    void SetOverlay(const assets::AnimationData* overlay);
    bool HasOverlay() const { return overlay_ != nullptr; }

    // timeMs is wrapped for looping animations and clamped otherwise (the overlay's time
    // `overlayMs` the same). inPlace keeps the root bone's horizontal position at the bind pose, so
    // walk/run cycles stay centred instead of moving away. `overlayWeight`: how much of the overlay's
    // bones take its keys (0..1; the rest the animation's): the flinch at half over a swing.
    void Evaluate(float timeMs, bool inPlace, float overlayMs = 0.0f, float overlayWeight = 1.0f);

    // Body volume: per-bone factors that thicken a bone's skin around its length
    // axis (its local Y and Z scale in the skinning palette only; children and the
    // pose are unaffected). Empty or 1.0 = unchanged.
    void SetRadialScale(std::vector<float> perBone) { radial_ = std::move(perBone); }

    // A turn put on one bone's own rotation after the animation (a bone controller: a skill's spine
    // aim); -1 none. It stays until changed.
    void SetBoneTurn(int bone, const glm::quat& turn) {
        turnBone_ = bone;
        boneTurn_ = turn;
    }

    // Cross-fade: the next evaluations ease from the pose last evaluated into the new
    // animation over `durationMs` (advanced by AdvanceBlend), so motions do not snap.
    void StartBlend(float durationMs);
    void AdvanceBlend(float deltaMs) { blendLeft_ = blendLeft_ > deltaMs ? blendLeft_ - deltaMs : 0.0f; }

    // --- The mixer: the original's two layers of clips ------------
    // A cyclic clip (stand, run, a stance, a cast's wait loop) under one-shots (a swing, a flinch, a fall). Each one-shot
    // keeps its own clock: its weight 0 to 1 over its blend-in (its first frame held when `holdFirst`), 1 while it plays,
    // 1 to 0 over its blend-out once it ends or is stopped (its last frame held). Every bone takes the weight-averaged
    // pose of the one-shots that animate it (two of weight 1 half and half), laid on with at most weight 1; the cyclic
    // clip fills the rest. A new cyclic clip cross-fades from the old over `fadeMs`. Times in ms on the caller's clock.
    void SetCycle(const assets::AnimationData* clip, double nowMs, float fadeMs);
    // A one-shot started now; `tag` the caller's name for it (FindOnce). Its id (never 0). `rate` its clip's speed
    // (0.5: twice as long; a slowed entity's actions).
    std::uint32_t PlayOnce(const assets::AnimationData* clip, double nowMs, float blendInMs, float blendOutMs, bool holdFirst, std::uint64_t tag,
                           float rate = 1.0f);
    // A one-shot blended out from now (it plays on through its blend-out); Resume takes a stop back.
    void StopOnce(std::uint32_t id, double nowMs, float blendOutMs);
    void ResumeOnce(std::uint32_t id);
    // The one-shot of that tag still playing or blending out (0: none).
    std::uint32_t FindOnce(std::uint64_t tag, double nowMs) const;
    void ClearOnces() { onces_.clear(); }
    // The cyclic clip and the time into it (ms, within its length) at `nowMs`; false with none.
    bool CycleTime(double nowMs, const assets::AnimationData*& clip, double& timeMs) const;
    void EvaluateMix(double nowMs, bool inPlace);
    // What the mixer plays at `nowMs` (the cycle, each one-shot's clip, time and weight): a line for logs.
    std::string MixSummary(double nowMs) const;

    const std::vector<glm::mat4>& World() const { return world_; }
    const std::vector<glm::mat4>& Palette() const { return palette_; }
    int MappedTracks() const { return mappedTracks_; }

private:
    const assets::SkeletonData* skeleton_ = nullptr;
    const assets::AnimationData* animation_ = nullptr;
    std::vector<int> trackBone_;        // animation track -> bone, -1 if unmatched
    const assets::AnimationData* overlay_ = nullptr;
    std::vector<int> overlayBone_;
    std::vector<glm::mat4> inverseBind_;
    std::vector<glm::mat4> world_;
    std::vector<glm::mat4> palette_;
    std::vector<float> radial_;
    int turnBone_ = -1;
    glm::quat boneTurn_{1.0f, 0.0f, 0.0f, 0.0f};
    int motionRoot_ = -1;
    // The local pose last evaluated, and the one being faded from.
    std::vector<glm::quat> rotations_, fromRotations_, overlayRotations_;
    std::vector<glm::vec3> translations_, fromTranslations_, overlayTranslations_;
    float blendLeft_ = 0.0f, blendTotal_ = 0.0f;
    int mappedTracks_ = 0;

    // The mixer's state.
    struct Clip {
        const assets::AnimationData* clip = nullptr;
        std::vector<int> bones; // track -> bone
        double startMs = 0;
    };
    struct Once {
        std::uint32_t id = 0;
        std::uint64_t tag = 0;
        Clip clip;
        float blendIn = 0, blendOut = 0;
        bool holdFirst = false;
        float rate = 1.0f;
        double stopMs = -1.0; // stopped at (its blend-out from then); -1: at its end
    };
    Clip cycle_, cycleFrom_;
    double fadeStart_ = 0;
    float fadeMs_ = 0;
    std::vector<Once> onces_;
    std::uint32_t nextOnce_ = 1;
    std::vector<glm::quat> layerRotations_, bindRotations_;
    std::vector<glm::vec3> layerTranslations_, bindTranslations_;
    std::vector<float> layerWeights_;
    Clip MapClip(const assets::AnimationData* clip, double startMs) const;
    // A one-shot's weight and its clip's time now (weight 0: over).
    static void OnceAt(const Once& o, double nowMs, float& weight, float& timeMs);
    void Finish(); // the bone turn, the world matrices and the palette from the local pose
};

} // namespace sr::anim
