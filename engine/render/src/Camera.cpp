#include "sr/render/Camera.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace sr::render {

namespace {
constexpr float kMaxPitch = 1.55f; // just short of straight up/down
}

glm::vec3 OrbitCamera::Position() const {
    const glm::vec3 offset(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
    return target + offset * distance;
}

glm::mat4 OrbitCamera::View() const { return glm::lookAt(Position(), target, glm::vec3(0.0f, 1.0f, 0.0f)); }

glm::mat4 OrbitCamera::Projection(float aspect) const {
    const float nearZ = std::max(distance * 0.01f, 0.01f);
    const float farZ = distance * 100.0f + 1000.0f;
    glm::mat4 proj = glm::perspective(fovY, std::max(aspect, 0.01f), nearZ, farZ);
    proj[1][1] *= -1.0f; // Vulkan clip space has Y pointing down
    return proj;
}

void OrbitCamera::Orbit(float deltaYaw, float deltaPitch) {
    yaw += deltaYaw;
    pitch = std::clamp(pitch + deltaPitch, -kMaxPitch, kMaxPitch);
}

void OrbitCamera::Pan(float dxPixels, float dyPixels, float viewportHeight) {
    const glm::mat4 view = View();
    // Rows of the view matrix are the camera's right and up axes in world space.
    const glm::vec3 right(view[0][0], view[1][0], view[2][0]);
    const glm::vec3 up(view[0][1], view[1][1], view[2][1]);
    const float worldPerPixel = 2.0f * distance * std::tan(fovY * 0.5f) / std::max(viewportHeight, 1.0f);
    target += (-right * dxPixels + up * dyPixels) * worldPerPixel;
}

void OrbitCamera::Zoom(float wheelSteps) { distance = std::clamp(distance * std::pow(0.85f, wheelSteps), 0.05f, 50000.0f); }

void OrbitCamera::Frame(const glm::vec3& boundsMin, const glm::vec3& boundsMax) {
    target = (boundsMin + boundsMax) * 0.5f;
    const float radius = std::max(glm::length(boundsMax - boundsMin) * 0.5f, 0.1f);
    distance = radius / std::sin(fovY * 0.5f) * 1.05f;
}

} // namespace sr::render

namespace sr::render {

FollowCamera::FollowCamera() {
    orbit.distance = 10.0f;
    orbit.pitch = 0.35f;
}

void FollowCamera::Snap(const glm::vec3& characterPosition) {
    orbit.target = characterPosition + glm::vec3(0.0f, targetHeight, 0.0f);
}

void FollowCamera::Update(const glm::vec3& characterPosition, float dt) {
    const glm::vec3 goal = characterPosition + glm::vec3(0.0f, targetHeight, 0.0f);
    // Frame-rate independent exponential easing.
    const float t = 1.0f - std::exp(-followSpeed * std::max(dt, 0.0f));
    orbit.target += (goal - orbit.target) * t;
}

void FollowCamera::Turn(float deltaYaw, float deltaPitch) {
    orbit.yaw += deltaYaw;
    orbit.pitch = std::clamp(orbit.pitch + deltaPitch, minPitch, maxPitch);
}

void FollowCamera::Zoom(float wheelSteps) {
    orbit.distance = std::clamp(orbit.distance * std::pow(0.85f, wheelSteps), minDistance, maxDistance);
}

// The orbit places the camera at +sin(yaw), +cos(yaw) from the target, so it looks along the opposite way.
glm::vec3 FollowCamera::ForwardOnGround() const { return glm::vec3(-std::sin(orbit.yaw), 0.0f, -std::cos(orbit.yaw)); }

glm::vec3 FlyCamera::Forward() const {
    return glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
}

// Left-handed: with Y up and Z forward, +X is to the right.
glm::vec3 FlyCamera::Right() const { return glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), Forward())); }

glm::mat4 FlyCamera::View() const { return glm::lookAt(position, position + Forward(), glm::vec3(0.0f, 1.0f, 0.0f)); }

glm::mat4 FlyCamera::Projection(float aspect) const {
    glm::mat4 proj = glm::perspective(fovY, std::max(aspect, 0.01f), nearZ, farZ);
    proj[1][1] *= -1.0f;
    return proj;
}

void FlyCamera::Look(float deltaYaw, float deltaPitch) {
    yaw += deltaYaw;
    pitch = std::clamp(pitch + deltaPitch, -kMaxPitch, kMaxPitch);
}

void FlyCamera::Move(const glm::vec3& local) {
    position += Right() * local.x + glm::vec3(0.0f, local.y, 0.0f) + Forward() * local.z;
}

} // namespace sr::render
