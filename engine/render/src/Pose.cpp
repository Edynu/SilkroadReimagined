#include "sr/render/Pose.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>

#include "sr/assets/Animation.h"

namespace sr::render {

namespace {

glm::quat ToQuat(const std::array<float, 4>& q) { return glm::quat(q[3], q[0], q[1], q[2]); } // (w, x, y, z)
glm::vec3 ToVec(const std::array<float, 3>& v) { return glm::vec3(v[0], v[1], v[2]); }

glm::mat4 Compose(const glm::quat& rotation, const glm::vec3& translation) {
    glm::mat4 m = glm::mat4_cast(glm::normalize(rotation));
    m[3] = glm::vec4(translation, 1.0f);
    return m;
}

} // namespace

void SkeletonPose::Init(const assets::SkeletonData& skeleton) {
    skeleton_ = &skeleton;
    animation_ = nullptr;
    trackBone_.clear();
    mappedTracks_ = 0;
    const std::size_t count = skeleton.bones.size();
    world_.assign(count, glm::mat4(1.0f));
    palette_.assign(count, glm::mat4(1.0f));
    inverseBind_.assign(count, glm::mat4(1.0f));

    // The bind pose comes from the local transforms (forward kinematics); bones
    // are stored parents-first, so one pass suffices.
    for (std::size_t i = 0; i < count; ++i) {
        const assets::Bone& bone = skeleton.bones[i];
        const glm::mat4 local = Compose(ToQuat(bone.local.rotation), ToVec(bone.local.translation));
        world_[i] = bone.parent >= 0 ? world_[static_cast<std::size_t>(bone.parent)] * local : local;
        inverseBind_[i] = glm::inverse(world_[i]);
    }

    // The bone that carries locomotion: the Biped root if present, else the first root.
    motionRoot_ = skeleton.Find("Bip01");
    for (std::size_t i = 0; motionRoot_ < 0 && i < count; ++i) {
        if (skeleton.bones[i].parent < 0) motionRoot_ = static_cast<int>(i);
    }
}

void SkeletonPose::SetAnimation(const assets::AnimationData* animation) {
    animation_ = animation;
    trackBone_.clear();
    mappedTracks_ = 0;
    if (animation == nullptr || skeleton_ == nullptr) return;
    for (const assets::AnimationTrack& track : animation->tracks) {
        const int bone = skeleton_->Find(track.boneName);
        trackBone_.push_back(bone);
        mappedTracks_ += bone >= 0 ? 1 : 0;
    }
}

void SkeletonPose::StartBlend(float durationMs) {
    if (rotations_.empty() || durationMs <= 0.0f) return;
    fromRotations_ = rotations_;
    fromTranslations_ = translations_;
    blendLeft_ = blendTotal_ = durationMs;
}

void SkeletonPose::Evaluate(float timeMs, bool inPlace) {
    if (skeleton_ == nullptr) return;
    const std::size_t count = skeleton_->bones.size();

    std::vector<glm::quat>& rotations = rotations_;
    std::vector<glm::vec3>& translations = translations_;
    rotations.resize(count);
    translations.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        rotations[i] = ToQuat(skeleton_->bones[i].local.rotation);
        translations[i] = ToVec(skeleton_->bones[i].local.translation);
    }

    const assets::AnimationData* anim = animation_;
    if (anim != nullptr && !anim->keyTimesMs.empty()) {
        const float duration = static_cast<float>(std::max<std::uint32_t>(anim->keyTimesMs.back(), 1));
        float t = anim->loop ? std::fmod(std::max(timeMs, 0.0f), duration) : std::clamp(timeMs, 0.0f, duration);
        // Key interval [k0, k1] around t.
        const auto& times = anim->keyTimesMs;
        const auto upper = std::upper_bound(times.begin(), times.end(), static_cast<std::uint32_t>(t));
        std::size_t k1 = static_cast<std::size_t>(std::distance(times.begin(), upper));
        k1 = std::min(k1, times.size() - 1);
        const std::size_t k0 = k1 > 0 ? k1 - 1 : 0;
        const float span = static_cast<float>(times[k1]) - static_cast<float>(times[k0]);
        const float alpha = span > 0.0f ? std::clamp((t - static_cast<float>(times[k0])) / span, 0.0f, 1.0f) : 0.0f;

        for (std::size_t tr = 0; tr < anim->tracks.size() && tr < trackBone_.size(); ++tr) {
            const int bone = trackBone_[tr];
            const assets::AnimationTrack& track = anim->tracks[tr];
            if (bone < 0 || track.keys.size() <= k1) continue;
            const auto& a = track.keys[k0];
            const auto& b = track.keys[k1];
            rotations[static_cast<std::size_t>(bone)] = glm::slerp(ToQuat(a.rotation), ToQuat(b.rotation), alpha);
            if (track.translationValid) {
                translations[static_cast<std::size_t>(bone)] =
                    glm::mix(ToVec(a.translation), ToVec(b.translation), alpha);
            }
        }
    }
    if (inPlace && motionRoot_ >= 0) {
        const auto& bind = skeleton_->bones[static_cast<std::size_t>(motionRoot_)].local.translation;
        translations[static_cast<std::size_t>(motionRoot_)].x = bind[0];
        translations[static_cast<std::size_t>(motionRoot_)].z = bind[2];
    }
    if (blendLeft_ > 0.0f && fromRotations_.size() == count) {
        // Ease out of the previous pose (smoothstep weight of the new one).
        const float x = 1.0f - blendLeft_ / blendTotal_;
        const float w = x * x * (3.0f - 2.0f * x);
        for (std::size_t i = 0; i < count; ++i) {
            rotations[i] = glm::slerp(fromRotations_[i], rotations[i], w);
            translations[i] = glm::mix(fromTranslations_[i], translations[i], w);
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        const glm::mat4 local = Compose(rotations[i], translations[i]);
        const int parent = skeleton_->bones[i].parent;
        world_[i] = parent >= 0 ? world_[static_cast<std::size_t>(parent)] * local : local;
        glm::mat4 skin = world_[i];
        if (i < radial_.size() && radial_[i] != 1.0f) {
            skin[1] = glm::vec4(glm::vec3(skin[1]) * radial_[i], skin[1].w);
            skin[2] = glm::vec4(glm::vec3(skin[2]) * radial_[i], skin[2].w);
        }
        palette_[i] = skin * inverseBind_[i];
    }
}

} // namespace sr::render
