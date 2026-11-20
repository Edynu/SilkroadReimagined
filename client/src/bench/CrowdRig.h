#pragma once

// The crowd benchmark's character: a humanoid skeleton of 44 bones,
// the size of a Silkroad player body, with two keyframed animations (walk, run) of 30
// keys per bone, sampled and blended the way real animations will be (C8). Pose() runs on worker
// jobs and writes straight into mapped GPU memory.

#include <array>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace sr::client {

class CrowdRig {
public:
    static constexpr int kBones = 44;
    static constexpr int kKeys = 30;
    static constexpr std::size_t kBytesPerBone = 64; // 3 rows of a 3x4 world matrix, colour + size

    CrowdRig();

    struct Placement {
        float x = 0, z = 0, yaw = 0;
        float animTime = 0;
        float runBlend = 0;          // 0 walk .. 1 run
        float scale = 1;             // grows from 0 as the character fades in
        float r = 1, g = 1, b = 1;   // its colour
    };
    // One character's kBones bone transforms into `out` (kBones * kBytesPerBone bytes).
    void Pose(const Placement& placement, std::byte* out) const;

private:
    struct Key {
        glm::quat rotation;
        glm::vec3 translation;
    };
    std::array<int, kBones> parent_{};
    std::array<float, kBones> size_{}; // box size per bone
    std::vector<Key> walk_, run_;      // [key * kBones + bone]
};

} // namespace sr::client
