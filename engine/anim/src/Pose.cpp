#include "sr/anim/Pose.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>

#include "sr/assets/Animation.h"

namespace sr::anim {

namespace {

glm::quat ToQuat(const std::array<float, 4>& q) { return glm::quat(q[3], q[0], q[1], q[2]); } // (w, x, y, z)
glm::vec3 ToVec(const std::array<float, 3>& v) { return glm::vec3(v[0], v[1], v[2]); }

// A clip's keys at a time into the local pose of the bones its tracks map to.
void Sample(const assets::AnimationData& anim, const std::vector<int>& trackBone, float timeMs, std::vector<glm::quat>& rotations,
            std::vector<glm::vec3>& translations) {
    if (anim.keyTimesMs.empty()) return;
    const float duration = static_cast<float>(std::max<std::uint32_t>(anim.keyTimesMs.back(), 1));
    const float t = anim.loop ? std::fmod(std::max(timeMs, 0.0f), duration) : std::clamp(timeMs, 0.0f, duration);
    // Key interval [k0, k1] around t.
    const auto& times = anim.keyTimesMs;
    const auto upper = std::upper_bound(times.begin(), times.end(), static_cast<std::uint32_t>(t));
    std::size_t k1 = static_cast<std::size_t>(std::distance(times.begin(), upper));
    k1 = std::min(k1, times.size() - 1);
    const std::size_t k0 = k1 > 0 ? k1 - 1 : 0;
    const float span = static_cast<float>(times[k1]) - static_cast<float>(times[k0]);
    const float alpha = span > 0.0f ? std::clamp((t - static_cast<float>(times[k0])) / span, 0.0f, 1.0f) : 0.0f;
    for (std::size_t tr = 0; tr < anim.tracks.size() && tr < trackBone.size(); ++tr) {
        const int bone = trackBone[tr];
        const assets::AnimationTrack& track = anim.tracks[tr];
        if (bone < 0 || track.keys.size() <= k1) continue;
        const auto& a = track.keys[k0];
        const auto& b = track.keys[k1];
        rotations[static_cast<std::size_t>(bone)] = glm::slerp(ToQuat(a.rotation), ToQuat(b.rotation), alpha);
        if (track.translationValid) translations[static_cast<std::size_t>(bone)] = glm::mix(ToVec(a.translation), ToVec(b.translation), alpha);
    }
}

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
    overlay_ = nullptr;
    overlayBone_.clear();
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

void SkeletonPose::SetOverlay(const assets::AnimationData* overlay) {
    overlay_ = overlay;
    overlayBone_.clear();
    if (overlay == nullptr || skeleton_ == nullptr) return;
    for (const assets::AnimationTrack& track : overlay->tracks) overlayBone_.push_back(skeleton_->Find(track.boneName));
}

void SkeletonPose::StartBlend(float durationMs) {
    if (rotations_.empty() || durationMs <= 0.0f) return;
    fromRotations_ = rotations_;
    fromTranslations_ = translations_;
    blendLeft_ = blendTotal_ = durationMs;
}

void SkeletonPose::Evaluate(float timeMs, bool inPlace, float overlayMs, float overlayWeight) {
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

    if (animation_ != nullptr) Sample(*animation_, trackBone_, timeMs, rotations, translations);
    if (overlay_ != nullptr && overlayWeight >= 1.0f) {
        Sample(*overlay_, overlayBone_, overlayMs, rotations, translations);
    } else if (overlay_ != nullptr && overlayWeight > 0.0f) {
        // Its bones part of the way from the animation's pose to the overlay's.
        overlayRotations_ = rotations;
        overlayTranslations_ = translations;
        Sample(*overlay_, overlayBone_, overlayMs, overlayRotations_, overlayTranslations_);
        for (const int b : overlayBone_) {
            if (b < 0 || std::size_t(b) >= count) continue;
            rotations[std::size_t(b)] = glm::slerp(rotations[std::size_t(b)], overlayRotations_[std::size_t(b)], overlayWeight);
            translations[std::size_t(b)] = glm::mix(translations[std::size_t(b)], overlayTranslations_[std::size_t(b)], overlayWeight);
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

    Finish();
}

void SkeletonPose::Finish() {
    const std::size_t count = skeleton_->bones.size();
    std::vector<glm::quat>& rotations = rotations_;
    std::vector<glm::vec3>& translations = translations_;
    if (turnBone_ >= 0 && static_cast<std::size_t>(turnBone_) < count) {
        rotations[static_cast<std::size_t>(turnBone_)] = rotations[static_cast<std::size_t>(turnBone_)] * boneTurn_;
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

// --- The mixer -----------------------------------------------------------------------------------------------------------

SkeletonPose::Clip SkeletonPose::MapClip(const assets::AnimationData* clip, double startMs) const {
    Clip c;
    c.clip = clip;
    c.startMs = startMs;
    if (clip != nullptr && skeleton_ != nullptr) {
        for (const assets::AnimationTrack& track : clip->tracks) c.bones.push_back(skeleton_->Find(track.boneName));
    }
    return c;
}

void SkeletonPose::SetCycle(const assets::AnimationData* clip, double nowMs, float fadeMs) {
    if (clip == cycle_.clip) return; // the same loop goes on, its clock its own
    if (cycle_.clip != nullptr && fadeMs > 0.0f) {
        cycleFrom_ = std::move(cycle_);
        fadeStart_ = nowMs;
        fadeMs_ = fadeMs;
    } else {
        cycleFrom_ = {};
        fadeMs_ = 0.0f;
    }
    cycle_ = MapClip(clip, nowMs);
}

std::uint32_t SkeletonPose::PlayOnce(const assets::AnimationData* clip, double nowMs, float blendInMs, float blendOutMs, bool holdFirst, std::uint64_t tag) {
    if (clip == nullptr) return 0;
    // At most eight at once: the oldest gives way (a flood of swings).
    if (onces_.size() >= 8) onces_.erase(onces_.begin());
    Once o;
    o.id = nextOnce_++;
    if (nextOnce_ == 0) nextOnce_ = 1;
    o.tag = tag;
    o.clip = MapClip(clip, nowMs);
    o.blendIn = std::max(0.0f, blendInMs);
    o.blendOut = std::max(0.0f, blendOutMs);
    o.holdFirst = holdFirst;
    onces_.push_back(std::move(o));
    return onces_.back().id;
}

void SkeletonPose::StopOnce(std::uint32_t id, double nowMs, float blendOutMs) {
    for (Once& o : onces_) {
        if (o.id != id) continue;
        if (o.stopMs < 0.0 || o.stopMs > nowMs) {
            o.stopMs = nowMs;
            o.blendOut = std::max(0.0f, blendOutMs);
        }
    }
}

void SkeletonPose::ResumeOnce(std::uint32_t id) {
    for (Once& o : onces_) {
        if (o.id == id) o.stopMs = -1.0;
    }
}

std::uint32_t SkeletonPose::FindOnce(std::uint64_t tag, double nowMs) const {
    for (auto it = onces_.rbegin(); it != onces_.rend(); ++it) {
        if (it->tag != tag) continue;
        float w = 0, t = 0;
        OnceAt(*it, nowMs, w, t);
        if (w > 0.0f || nowMs < it->clip.startMs + it->blendIn) return it->id;
    }
    return 0;
}

void SkeletonPose::OnceAt(const Once& o, double nowMs, float& weight, float& timeMs) {
    const assets::AnimationData* clip = o.clip.clip;
    const double duration = clip != nullptr && !clip->keyTimesMs.empty() ? double(std::max<std::uint32_t>(clip->keyTimesMs.back(), 1)) : 1.0;
    const double lead = o.holdFirst ? double(o.blendIn) : 0.0; // its first frame held through its blend-in
    const double natural = o.clip.startMs + lead + duration;
    const double end = o.stopMs >= 0.0 ? o.stopMs : (clip != nullptr && clip->loop ? 1e300 : natural);
    const double at = std::min(nowMs, end);
    timeMs = float(std::max(0.0, at - o.clip.startMs - lead)); // its last frame held through its blend-out
    const double local = nowMs - o.clip.startMs;
    const double in = o.blendIn > 0.0f ? std::clamp(local / double(o.blendIn), 0.0, 1.0) : 1.0;
    const double out = nowMs <= end ? 1.0 : (o.blendOut > 0.0f ? 1.0 - (nowMs - end) / double(o.blendOut) : 0.0);
    weight = float(std::clamp(in * out, 0.0, 1.0));
}

std::string SkeletonPose::MixSummary(double nowMs) const {
    std::string out = "cycle " + (cycle_.clip != nullptr ? cycle_.clip->name : std::string("-"));
    if (cycleFrom_.clip != nullptr && fadeMs_ > 0.0f && nowMs < fadeStart_ + fadeMs_) out += " (from " + cycleFrom_.clip->name + ")";
    for (const Once& o : onces_) {
        float w = 0, t = 0;
        OnceAt(o, nowMs, w, t);
        out += " | " + (o.clip.clip != nullptr ? o.clip.clip->name : std::string("-")) + " t " + std::to_string(int(t)) + " w " + std::to_string(int(w * 100.0f));
    }
    return out;
}

void SkeletonPose::EvaluateMix(double nowMs, bool inPlace) {
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
    bindRotations_ = rotations;
    bindTranslations_ = translations;
    // The cyclic layer, cross-faded from the one before over its fade.
    if (cycle_.clip != nullptr) Sample(*cycle_.clip, cycle_.bones, float(nowMs - cycle_.startMs), rotations, translations);
    if (cycleFrom_.clip != nullptr && fadeMs_ > 0.0f && nowMs < fadeStart_ + fadeMs_) {
        overlayRotations_ = bindRotations_;
        overlayTranslations_ = bindTranslations_;
        Sample(*cycleFrom_.clip, cycleFrom_.bones, float(nowMs - cycleFrom_.startMs), overlayRotations_, overlayTranslations_);
        const float x = std::clamp(float((nowMs - fadeStart_) / fadeMs_), 0.0f, 1.0f);
        const float w = x * x * (3.0f - 2.0f * x);
        for (std::size_t i = 0; i < count; ++i) {
            rotations[i] = glm::slerp(overlayRotations_[i], rotations[i], w);
            translations[i] = glm::mix(overlayTranslations_[i], translations[i], w);
        }
    } else if (cycleFrom_.clip != nullptr) {
        cycleFrom_ = {};
    }
    // The one-shots: each bone the weight-averaged pose of those that animate it.
    layerWeights_.assign(count, 0.0f);
    layerRotations_.resize(count);
    layerTranslations_.resize(count);
    for (auto it = onces_.begin(); it != onces_.end();) {
        float w = 0, t = 0;
        OnceAt(*it, nowMs, w, t);
        const bool started = nowMs >= it->clip.startMs + it->blendIn;
        if (w <= 0.0f && started) {
            it = onces_.erase(it); // over
            continue;
        }
        if (w > 0.0f && it->clip.clip != nullptr) {
            overlayRotations_ = bindRotations_;
            overlayTranslations_ = bindTranslations_;
            Sample(*it->clip.clip, it->clip.bones, t, overlayRotations_, overlayTranslations_);
            for (const int bone : it->clip.bones) {
                if (bone < 0 || std::size_t(bone) >= count) continue;
                const std::size_t b = std::size_t(bone);
                const float total = layerWeights_[b] + w;
                if (layerWeights_[b] <= 0.0f) {
                    layerRotations_[b] = overlayRotations_[b];
                    layerTranslations_[b] = overlayTranslations_[b];
                } else {
                    const float k = w / total;
                    layerRotations_[b] = glm::slerp(layerRotations_[b], overlayRotations_[b], k);
                    layerTranslations_[b] = glm::mix(layerTranslations_[b], overlayTranslations_[b], k);
                }
                layerWeights_[b] = total;
            }
        }
        ++it;
    }
    for (std::size_t b = 0; b < count; ++b) {
        if (layerWeights_[b] <= 0.0f) continue;
        const float l = std::min(1.0f, layerWeights_[b]);
        rotations[b] = glm::slerp(rotations[b], layerRotations_[b], l);
        translations[b] = glm::mix(translations[b], layerTranslations_[b], l);
    }
    if (inPlace && motionRoot_ >= 0) {
        const auto& bind = skeleton_->bones[static_cast<std::size_t>(motionRoot_)].local.translation;
        translations[static_cast<std::size_t>(motionRoot_)].x = bind[0];
        translations[static_cast<std::size_t>(motionRoot_)].z = bind[2];
    }
    Finish();
}

} // namespace sr::anim
