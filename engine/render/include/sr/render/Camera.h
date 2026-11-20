#pragma once

#include <glm/glm.hpp>

namespace sr::render {

// Orbits a target point. World space is the game's: left-handed, Y up.
struct OrbitCamera {
    glm::vec3 target{0.0f};
    float yaw = 0.7f;   // radians around Y
    float pitch = 0.4f; // radians above the horizon
    float distance = 10.0f;
    float fovY = glm::radians(45.0f);

    glm::vec3 Position() const;
    glm::mat4 View() const;
    // Perspective for Vulkan clip space (Y down, depth 0..1); near/far follow the distance.
    glm::mat4 Projection(float aspect) const;

    void Orbit(float deltaYaw, float deltaPitch);
    // Moves the target in the view plane; deltas in pixels of a viewport `viewportHeight` tall.
    void Pan(float dxPixels, float dyPixels, float viewportHeight);
    void Zoom(float wheelSteps);
    // Fits the box into view.
    void Frame(const glm::vec3& boundsMin, const glm::vec3& boundsMax);
};

// Third-person follow camera, like the original game's: orbits a point above the
// character's feet, right-drag turns it, the wheel zooms within fixed limits, and
// the target eases toward the character so movement does not jerk the view.
struct FollowCamera {
    OrbitCamera orbit;
    float targetHeight = 1.6f;   // look at this far above the character's position
    float minDistance = 2.0f;
    float maxDistance = 30.0f;
    float minPitch = -0.2f;      // keep the camera above the ground plane's horizon
    float maxPitch = 1.4f;
    float followSpeed = 12.0f;   // 1/s; higher follows tighter

    FollowCamera();
    // Puts the camera on the character immediately (spawn, teleport).
    void Snap(const glm::vec3& characterPosition);
    // Eases the target toward the character; `dt` in seconds.
    void Update(const glm::vec3& characterPosition, float dt);
    void Turn(float deltaYaw, float deltaPitch);
    void Zoom(float wheelSteps);
    // Heading the character should walk for "forward" input (horizontal, unit length).
    glm::vec3 ForwardOnGround() const;
};

// Free-flying camera (WASD + mouse look) for world inspection.
struct FlyCamera {
    glm::vec3 position{0.0f};
    float yaw = 0.0f;   // radians around Y; 0 looks along +Z (north)
    float pitch = 0.0f; // radians, positive looks up
    float fovY = glm::radians(60.0f);
    float nearZ = 1.0f;
    float farZ = 8000.0f;

    glm::vec3 Forward() const;
    glm::vec3 Right() const;
    glm::mat4 View() const;
    glm::mat4 Projection(float aspect) const; // Vulkan clip space
    void Look(float deltaYaw, float deltaPitch);
    // Moves in camera space: x right, y world up, z forward.
    void Move(const glm::vec3& local);
};

} // namespace sr::render
