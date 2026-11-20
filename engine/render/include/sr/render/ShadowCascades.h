#pragma once

// Cascaded shadow maps for one directional light: the camera's view split into slices by
// distance, each covered by an orthographic view from the light, in one square of a 2 x 2
// atlas. Each slice is fitted with a sphere (so its size does not change as the camera
// turns) and its centre snapped to whole texels (so shadows do not shimmer as it moves).

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace sr::render {

struct ShadowCascades {
    int count = 0;
    std::array<glm::mat4, 4> viewProj{};   // to render cascade i (into its atlas square)
    std::array<glm::mat4, 4> atlas{};      // world -> (atlas u, v, depth) of cascade i
    glm::vec4 splits{0.0f};                // view depth where cascade i ends
    glm::vec4 texels{0.0f};                // world size of one texel of cascade i
};

// `viewProj` / `cameraPosition`: the camera's; `toLight`: direction toward the light;
// `distance`: how far shadows reach; `atlasSize`: texels across the whole atlas;
// `behind`: how far past a slice toward the light casters are still caught.
inline ShadowCascades BuildShadowCascades(const glm::mat4& viewProj, const glm::vec3& cameraPosition, const glm::vec3& toLight,
                                          int count, float distance, int atlasSize, float behind = 3000.0f) {
    ShadowCascades out;
    out.count = std::clamp(count, 0, 4);
    if (out.count == 0 || atlasSize <= 0) return out;
    const glm::mat4 inverse = glm::inverse(viewProj);
    const auto unproject = [&](float x, float y, float z) {
        const glm::vec4 p = inverse * glm::vec4(x, y, z, 1.0f);
        return glm::vec3(p) / p.w;
    };
    std::array<glm::vec3, 4> nearCorner{}, farCorner{};
    const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int i = 0; i < 4; ++i) {
        nearCorner[static_cast<std::size_t>(i)] = unproject(corners[i][0], corners[i][1], 0.0f);
        farCorner[static_cast<std::size_t>(i)] = unproject(corners[i][0], corners[i][1], 1.0f);
    }
    const glm::vec3 forward = glm::normalize(unproject(0, 0, 1) - unproject(0, 0, 0));
    const float nearDepth = glm::dot(unproject(0, 0, 0) - cameraPosition, forward);
    const float farDepth = glm::dot(unproject(0, 0, 1) - cameraPosition, forward);
    const float reach = std::clamp(distance, nearDepth + 1.0f, farDepth);
    // Split depths: between even and logarithmic, so the near cascades stay small.
    std::array<float, 5> split{};
    split[0] = nearDepth;
    for (int i = 1; i <= out.count; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(out.count);
        const float logarithmic = nearDepth * std::pow(reach / nearDepth, f);
        const float even = nearDepth + (reach - nearDepth) * f;
        split[static_cast<std::size_t>(i)] = 0.75f * logarithmic + 0.25f * even;
    }
    const auto at = [&](std::size_t corner, float depth) {
        const float t = (depth - nearDepth) / std::max(farDepth - nearDepth, 1e-3f);
        return nearCorner[corner] + (farCorner[corner] - nearCorner[corner]) * t;
    };
    // The light's turn (no translation): it looks along the light's travel.
    const glm::vec3 travel = -glm::normalize(toLight);
    // An up axis far from the light's travel (the sun crosses the sky in x / y: z), so the
    // light's frame does not flip as the sun passes overhead.
    const glm::vec3 up = std::abs(travel.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const glm::mat4 lightView = glm::lookAt(glm::vec3(0.0f), travel, up);
    const int half = atlasSize / 2;
    for (int i = 0; i < out.count; ++i) {
        const float d0 = split[static_cast<std::size_t>(i)], d1 = split[static_cast<std::size_t>(i + 1)];
        std::array<glm::vec3, 8> slice{};
        glm::vec3 centre(0.0f);
        for (std::size_t c = 0; c < 4; ++c) {
            slice[c] = at(c, d0);
            slice[c + 4] = at(c, d1);
        }
        for (const glm::vec3& p : slice) centre += p / 8.0f;
        float radius = 0.0f;
        for (const glm::vec3& p : slice) radius = std::max(radius, glm::length(p - centre));
        radius = std::ceil(radius / 8.0f) * 8.0f; // steps, so tiny camera moves keep the size
        const float texel = 2.0f * radius / static_cast<float>(half);
        glm::vec3 c = glm::vec3(lightView * glm::vec4(centre, 1.0f));
        c.x = std::floor(c.x / texel) * texel;
        c.y = std::floor(c.y / texel) * texel;
        const glm::mat4 projection = glm::ortho(c.x - radius, c.x + radius, c.y - radius, c.y + radius, c.z - radius - behind, c.z + radius);
        const glm::mat4 vp = projection * lightView;
        out.viewProj[static_cast<std::size_t>(i)] = vp;
        // NDC x, y (-1..1) -> this cascade's quarter of the atlas (a viewport's y runs down
        // from NDC -1, as texture v does).
        const glm::vec2 quarter(static_cast<float>(i % 2) * 0.5f, static_cast<float>(i / 2) * 0.5f);
        const glm::mat4 bias = glm::translate(glm::mat4(1.0f), glm::vec3(quarter + 0.25f, 0.0f)) *
                               glm::scale(glm::mat4(1.0f), glm::vec3(0.25f, 0.25f, 1.0f));
        out.atlas[static_cast<std::size_t>(i)] = bias * vp;
        out.splits[i] = d1;
        out.texels[i] = texel;
    }
    return out;
}

} // namespace sr::render
