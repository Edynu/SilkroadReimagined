#include "bench/CrowdRig.h"

#include <cmath>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>

namespace sr::client {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kWalkCycle = 1.0f; // seconds
constexpr float kRunCycle = 0.6f;

struct BoneSpec {
    int parent;
    glm::vec3 offset; // from the parent, in the bind pose
    float size;
};

// Pelvis, spine, head, two arms with three fingers, two legs, then hair, a cloak, a weapon, a
// shield and belt pieces: 44 bones, like a Silkroad player body.
const std::array<BoneSpec, CrowdRig::kBones> kSkeleton = {{
    {-1, {0, 9.0f, 0}, 1.6f},                                                        // 0 pelvis
    {0, {0, 1.6f, 0}, 1.4f}, {1, {0, 1.6f, 0}, 1.5f}, {2, {0, 1.6f, 0}, 1.6f}, {3, {0, 1.6f, 0}, 1.6f}, // 1-4 spine
    {4, {0, 1.2f, 0}, 0.8f}, {5, {0, 1.4f, 0}, 1.6f},                                 // 5 neck, 6 head
    {4, {1.0f, 0.8f, 0}, 0.8f}, {7, {1.4f, 0, 0}, 0.9f}, {8, {0, -3.0f, 0}, 0.8f}, {9, {0, -2.6f, 0}, 0.7f}, // 7-10 left arm
    {10, {0, -0.6f, 0}, 0.3f}, {11, {0, -0.5f, 0}, 0.3f}, {12, {0, -0.4f, 0}, 0.3f},          // 11-13 left fingers
    {4, {-1.0f, 0.8f, 0}, 0.8f}, {14, {-1.4f, 0, 0}, 0.9f}, {15, {0, -3.0f, 0}, 0.8f}, {16, {0, -2.6f, 0}, 0.7f}, // 14-17 right arm
    {17, {0, -0.6f, 0}, 0.3f}, {18, {0, -0.5f, 0}, 0.3f}, {19, {0, -0.4f, 0}, 0.3f},          // 18-20 right fingers
    {0, {1.0f, -0.6f, 0}, 1.1f}, {21, {0, -4.2f, 0}, 1.0f}, {22, {0, -4.0f, 0}, 0.8f}, {23, {0, -0.4f, 0.9f}, 0.5f}, // 21-24 left leg
    {0, {-1.0f, -0.6f, 0}, 1.1f}, {25, {0, -4.2f, 0}, 1.0f}, {26, {0, -4.0f, 0}, 0.8f}, {27, {0, -0.4f, 0.9f}, 0.5f}, // 25-28 right leg
    {6, {0, 0.6f, -0.5f}, 0.5f}, {29, {0, -0.4f, -0.4f}, 0.4f}, {30, {0, -0.4f, -0.3f}, 0.3f},   // 29-31 hair
    {2, {0, 0, -0.9f}, 0.6f}, {32, {0, -1.2f, -0.2f}, 0.6f}, {33, {0, -1.2f, -0.2f}, 0.6f},   // 32-37 cloak
    {34, {0, -1.2f, -0.2f}, 0.6f}, {35, {0, -1.2f, -0.2f}, 0.6f}, {36, {0, -1.2f, -0.2f}, 0.6f},
    {17, {0, -0.5f, 0.4f}, 0.4f}, {38, {0, 0, 1.8f}, 0.4f},                            // 38-39 weapon
    {10, {-0.4f, 0, 0}, 1.2f},                                                         // 40 shield
    {0, {0.8f, 0, 0.6f}, 0.4f}, {0, {-0.8f, 0, 0.6f}, 0.4f}, {0, {0, 0, -0.8f}, 0.4f},  // 41-43 belt
}};

// The local rotation of bone `bone` at `phase` (0..1 of the cycle) for a gait of `amplitude`.
glm::quat Gait(int bone, float phase, float amplitude) {
    const float s = std::sin(phase * 2.0f * kPi);
    const float c = std::cos(phase * 2.0f * kPi);
    const glm::vec3 x(1, 0, 0), y(0, 1, 0), z(0, 0, 1);
    switch (bone) {
    case 21: return glm::angleAxis(0.6f * amplitude * s, x);                       // left thigh
    case 25: return glm::angleAxis(-0.6f * amplitude * s, x);                      // right thigh
    case 22: return glm::angleAxis(0.5f * amplitude * (1.0f + s) * 0.5f, x);       // calves bend
    case 26: return glm::angleAxis(0.5f * amplitude * (1.0f - s) * 0.5f, x);
    case 8: return glm::angleAxis(-0.5f * amplitude * s, x) * glm::angleAxis(-0.3f, z); // arms swing
    case 15: return glm::angleAxis(0.5f * amplitude * s, x) * glm::angleAxis(0.3f, z);
    case 9: case 16: return glm::angleAxis(-0.3f * amplitude, x);
    case 1: return glm::angleAxis(0.08f * amplitude * c, y);                       // spine twist
    case 3: return glm::angleAxis(-0.06f * amplitude * c, y);
    case 6: return glm::angleAxis(0.05f * amplitude * s, x);                       // head bob
    default:
        if (bone >= 32 && bone <= 37) return glm::angleAxis(0.15f * amplitude * (1.0f + c), x); // cloak sways
        if (bone >= 29 && bone <= 31) return glm::angleAxis(0.1f * amplitude * c, x);
        if (bone >= 11 && bone <= 13) return glm::angleAxis(0.3f, x);
        if (bone >= 18 && bone <= 20) return glm::angleAxis(0.3f, x);
        return glm::quat(1, 0, 0, 0);
    }
}

} // namespace

CrowdRig::CrowdRig() {
    walk_.resize(static_cast<std::size_t>(kKeys) * kBones);
    run_.resize(static_cast<std::size_t>(kKeys) * kBones);
    for (int b = 0; b < kBones; ++b) {
        parent_[static_cast<std::size_t>(b)] = kSkeleton[static_cast<std::size_t>(b)].parent;
        size_[static_cast<std::size_t>(b)] = kSkeleton[static_cast<std::size_t>(b)].size;
    }
    for (int k = 0; k < kKeys; ++k) {
        const float phase = static_cast<float>(k) / static_cast<float>(kKeys - 1);
        for (int b = 0; b < kBones; ++b) {
            const glm::vec3 offset = kSkeleton[static_cast<std::size_t>(b)].offset;
            const std::size_t i = static_cast<std::size_t>(k) * kBones + static_cast<std::size_t>(b);
            walk_[i] = {Gait(b, phase, 1.0f), offset};
            // Running: wider swings and a lower stance.
            run_[i] = {Gait(b, phase, 1.8f), b == 0 ? offset - glm::vec3(0, 0.8f, 0) : offset};
        }
    }
}

void CrowdRig::Pose(const Placement& p, std::byte* out) const {
    const float walkPos = std::fmod(p.animTime / kWalkCycle, 1.0f) * static_cast<float>(kKeys - 1);
    const float runPos = std::fmod(p.animTime / kRunCycle, 1.0f) * static_cast<float>(kKeys - 1);
    const int walkKey = static_cast<int>(walkPos);
    const int runKey = static_cast<int>(runPos);
    const float walkFrac = walkPos - static_cast<float>(walkKey);
    const float runFrac = runPos - static_cast<float>(runKey);
    const int walkNext = walkKey + 1 < kKeys ? walkKey + 1 : walkKey;
    const int runNext = runKey + 1 < kKeys ? runKey + 1 : runKey;

    const glm::mat4 world = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(p.x, 0.0f, p.z)), p.yaw, glm::vec3(0, 1, 0)) *
                            glm::scale(glm::mat4(1.0f), glm::vec3(p.scale));
    std::array<glm::mat4, kBones> model;
    for (int b = 0; b < kBones; ++b) {
        const auto key = [&](const std::vector<Key>& keys, int k) -> const Key& {
            return keys[static_cast<std::size_t>(k) * kBones + static_cast<std::size_t>(b)];
        };
        // Sample both animations between their keys, then blend walk -> run.
        const glm::quat walkRot = glm::slerp(key(walk_, walkKey).rotation, key(walk_, walkNext).rotation, walkFrac);
        const glm::quat runRot = glm::slerp(key(run_, runKey).rotation, key(run_, runNext).rotation, runFrac);
        const glm::vec3 walkPosition = glm::mix(key(walk_, walkKey).translation, key(walk_, walkNext).translation, walkFrac);
        const glm::vec3 runPosition = glm::mix(key(run_, runKey).translation, key(run_, runNext).translation, runFrac);
        const glm::quat rotation = glm::slerp(walkRot, runRot, p.runBlend);
        const glm::vec3 translation = glm::mix(walkPosition, runPosition, p.runBlend);
        const glm::mat4 local = glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation);
        const int parent = parent_[static_cast<std::size_t>(b)];
        model[static_cast<std::size_t>(b)] = parent < 0 ? local : model[static_cast<std::size_t>(parent)] * local;

        // Rows of world * model, then colour and box size: 64 bytes, written once (the memory
        // is write-combined GPU memory, never read back).
        const glm::mat4 m = world * model[static_cast<std::size_t>(b)];
        const float rows[16] = {m[0][0], m[1][0], m[2][0], m[3][0], m[0][1], m[1][1], m[2][1], m[3][1],
                                m[0][2], m[1][2], m[2][2], m[3][2], p.r, p.g, p.b, size_[static_cast<std::size_t>(b)]};
        std::memcpy(out + static_cast<std::size_t>(b) * kBytesPerBone, rows, sizeof(rows));
    }
}

} // namespace sr::client
