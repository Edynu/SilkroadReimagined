#include "sr/assets/Animation.h"

#include <cmath>

#include "sr/core/BinaryReader.h"

namespace sr::assets {

namespace {

constexpr std::uint32_t kMaxBones = 1024;
constexpr std::uint32_t kMaxKeys = 100'000;

BoneTransform ReadTransform(core::BinaryReader& r) {
    BoneTransform t;
    for (float& v : t.rotation) v = r.F32();
    for (float& v : t.translation) v = r.F32();
    return t;
}

} // namespace

int SkeletonData::Find(const std::string& name) const {
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

// .bsk (0101): "JMXVBSK 0101", u32 boneCount, then per bone:
//   u8 type, string name, string parentName, 3 x (quat f32x4 + translation f32x3)
//   [local, world bind, inverse world bind], u32 childCount, strings childNames
// followed by two u32 zeros. Only the local transform is kept: the world and
// inverse bind are recomputed from it (the stored inverse is not always exact).
bool DecodeBsk(std::span<const std::byte> file, SkeletonData& out, std::string& error) {
    out = SkeletonData{};
    core::BinaryReader r(file);
    if (!r.Matches("JMXVBSK 0101")) {
        error = "not a JMXVBSK 0101 file";
        return false;
    }
    r.Seek(12);
    const std::uint32_t count = r.U32();
    if (count > kMaxBones) {
        error = "implausible bone count";
        return false;
    }
    std::vector<Bone> fileOrder(count);
    for (Bone& bone : fileOrder) {
        r.U8();
        bone.name = r.CountedString();
        bone.parentName = r.CountedString();
        bone.local = ReadTransform(r);
        r.Skip(56); // world bind and inverse world bind
        const std::uint32_t children = r.U32();
        for (std::uint32_t c = 0; c < children && r.Ok(); ++c) r.CountedString();
    }
    if (!r.Ok()) {
        error = "truncated skeleton";
        return false;
    }

    // Order bones so every parent precedes its children (pose evaluation relies on it).
    std::vector<bool> placed(count, false);
    while (out.bones.size() < count) {
        const std::size_t before = out.bones.size();
        for (std::uint32_t i = 0; i < count; ++i) {
            if (placed[i]) continue;
            const Bone& bone = fileOrder[i];
            int parent = -1;
            if (!bone.parentName.empty()) {
                parent = out.Find(bone.parentName);
                if (parent < 0) {
                    bool parentExists = false;
                    for (const Bone& other : fileOrder) parentExists |= other.name == bone.parentName;
                    if (parentExists) continue; // parent not placed yet
                }
            }
            Bone copy = bone;
            copy.parent = parent;
            out.bones.push_back(std::move(copy));
            placed[i] = true;
        }
        if (out.bones.size() == before) {
            error = "skeleton has a parent cycle";
            return false;
        }
    }
    return true;
}

// .ban 0102: "JMXVBAN 0102", u32 0, u32 0, string name, u32 durationMs, u32 fps,
//   u32 loop, u32 keyCount, u32 keyTimesMs[keyCount], u32 trackCount, per track:
//   string boneName, u32 keyCount, keys (quat f32x4 + translation f32x3)
// .ban 0101 (two legacy files): no reserved u32s and no time table; each key is
//   u32 time, u32 time, quat, translation.
bool DecodeBan(std::span<const std::byte> file, AnimationData& out, std::string& error) {
    out = AnimationData{};
    core::BinaryReader r(file);
    const bool legacy = r.Matches("JMXVBAN 0101");
    if (!legacy && !r.Matches("JMXVBAN 0102")) {
        error = "not a JMXVBAN 0101/0102 file";
        return false;
    }
    r.Seek(legacy ? 12 : 20);
    out.name = r.CountedString();
    out.durationMs = r.U32();
    out.fps = r.U32();
    out.loop = r.U32() != 0;
    if (!legacy) {
        const std::uint32_t keyCount = r.U32();
        if (keyCount > kMaxKeys) {
            error = "implausible key count";
            return false;
        }
        out.keyTimesMs.resize(keyCount);
        for (auto& t : out.keyTimesMs) t = r.U32();
    }
    const std::uint32_t trackCount = r.U32();
    if (trackCount > kMaxBones) {
        error = "implausible track count";
        return false;
    }
    out.tracks.resize(trackCount);
    for (AnimationTrack& track : out.tracks) {
        track.boneName = r.CountedString();
        const std::uint32_t keyCount = r.U32();
        if (keyCount > kMaxKeys || (!legacy && keyCount != out.keyTimesMs.size())) {
            error = "track key count does not match the time table";
            return false;
        }
        track.keys.resize(keyCount);
        for (std::uint32_t k = 0; k < keyCount; ++k) {
            if (legacy) {
                const std::uint32_t time = r.U32();
                r.U32();
                if (&track == &out.tracks.front()) out.keyTimesMs.push_back(time);
            }
            track.keys[k] = ReadTransform(r);
        }
    }
    if (!r.Ok() || r.Remaining() != 0) {
        error = "animation does not end at the end of the file";
        return false;
    }
    // A few shipped tracks contain non-finite values. Translations: the whole
    // track falls back to the bind translation. Rotations: identity for that key.
    for (AnimationTrack& track : out.tracks) {
        for (BoneTransform& key : track.keys) {
            for (float v : key.translation) track.translationValid &= std::isfinite(v);
            bool finite = true;
            for (float v : key.rotation) finite &= std::isfinite(v);
            if (!finite) key.rotation = {0, 0, 0, 1};
        }
    }
    return true;
}

} // namespace sr::assets
