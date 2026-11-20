#pragma once

// Skeletons (.bsk) and skeletal animations (.ban), decoded into engine types.
// Convention (game space, left-handed, Y up): a transform is a unit quaternion
// (x, y, z, w) plus a translation, relative to the parent bone. World = parent * local.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sr::assets {

struct BoneTransform {
    std::array<float, 4> rotation{0, 0, 0, 1}; // x, y, z, w
    std::array<float, 3> translation{};
};

struct Bone {
    std::string name;
    std::string parentName;
    int parent = -1;     // index into SkeletonData::bones, -1 for roots
    BoneTransform local; // bind pose relative to the parent
};

struct SkeletonData {
    std::vector<Bone> bones; // parents always come before their children
    int Find(const std::string& name) const;
};

struct AnimationTrack {
    std::string boneName;
    std::vector<BoneTransform> keys; // one per AnimationData::keyTimes entry
    bool translationValid = true;    // false: use the bone's bind translation
};

struct AnimationData {
    std::string name;
    std::uint32_t durationMs = 0;
    std::uint32_t fps = 0;
    bool loop = false;
    std::vector<std::uint32_t> keyTimesMs;
    std::vector<AnimationTrack> tracks;
};

bool DecodeBsk(std::span<const std::byte> file, SkeletonData& out, std::string& error);
bool DecodeBan(std::span<const std::byte> file, AnimationData& out, std::string& error);

} // namespace sr::assets
