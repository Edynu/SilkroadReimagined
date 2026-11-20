#pragma once

// The six planes of a view-projection (GLM left-handed, depth 0..1, as every camera here),
// for culling bounding spheres and boxes on the CPU. Planes point inwards; a point is inside
// when dot(plane.xyz, p) + plane.w >= 0 for all six.

#include <array>

#include <glm/glm.hpp>

namespace sr::render {

class Frustum {
public:
    Frustum() = default;
    explicit Frustum(const glm::mat4& viewProj) {
        const glm::mat4 m = glm::transpose(viewProj); // rows of viewProj as columns
        planes_[0] = m[3] + m[0]; // left
        planes_[1] = m[3] - m[0]; // right
        planes_[2] = m[3] + m[1]; // bottom
        planes_[3] = m[3] - m[1]; // top
        planes_[4] = m[2];        // near (depth 0)
        planes_[5] = m[3] - m[2]; // far (depth 1)
        for (glm::vec4& p : planes_) {
            const float length = glm::length(glm::vec3(p));
            if (length > 0.0f) p /= length;
        }
    }

    bool Sphere(const glm::vec3& centre, float radius) const {
        for (const glm::vec4& p : planes_) {
            if (glm::dot(glm::vec3(p), centre) + p.w < -radius) return false;
        }
        return true;
    }
    // An axis-aligned box: outside when its corner furthest along a plane's normal is behind it.
    bool Box(const glm::vec3& min, const glm::vec3& max) const {
        for (const glm::vec4& p : planes_) {
            const glm::vec3 far(p.x >= 0.0f ? max.x : min.x, p.y >= 0.0f ? max.y : min.y, p.z >= 0.0f ? max.z : min.z);
            if (glm::dot(glm::vec3(p), far) + p.w < 0.0f) return false;
        }
        return true;
    }
    const std::array<glm::vec4, 6>& planes() const { return planes_; }

private:
    // Default: no planes cull anything.
    std::array<glm::vec4, 6> planes_{glm::vec4(0, 0, 0, 1), glm::vec4(0, 0, 0, 1), glm::vec4(0, 0, 0, 1),
                                     glm::vec4(0, 0, 0, 1), glm::vec4(0, 0, 0, 1), glm::vec4(0, 0, 0, 1)};
};

} // namespace sr::render
