#pragma once

// Local lights (effects, torches) for clustered forward shading: the view is cut into
// kTilesX x kTilesY screen tiles and kSlices depth slices (logarithmic, so near slices are
// thin); each cluster lists the lights whose sphere may reach into it. Shaders find their
// pixel's cluster and shade only those lights. Binned on the CPU: a few hundred lights at
// most, a few thousand clusters.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace sr::render {

struct PointLight {
    glm::vec3 position{0.0f}; // origin-relative world
    float radius = 100.0f;    // no light past it
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
};

struct LightClusters {
    static constexpr int kTilesX = 16, kTilesY = 9, kSlices = 24;
    static constexpr int kClusters = kTilesX * kTilesY * kSlices;
    static constexpr std::size_t kMaxIndices = 32768;
    float nearDepth = 10.0f, farDepth = 3000.0f;
    // Per light: position + radius, colour + intensity (two float4s, as the shaders read them).
    std::vector<glm::vec4> lights;
    // Per cluster (x fastest, then y, then slice): first index, count.
    std::vector<std::uint32_t> clusters;
    std::vector<std::uint32_t> indices;

    int Slice(float depth) const {
        if (depth <= nearDepth) return 0;
        const float s = std::log(depth / nearDepth) / std::log(farDepth / nearDepth) * static_cast<float>(kSlices);
        return std::clamp(static_cast<int>(s), 0, kSlices - 1);
    }

    // `viewProj` / `cameraPosition` / `forward`: the camera's (viewProj maps to a viewport
    // whose y runs down from NDC -1). Lights past `farDepth` are dropped; at most `maxLights`
    // are kept, the strongest near the camera first.
    void Build(std::span<const PointLight> input, const glm::mat4& viewProj, const glm::vec3& cameraPosition,
               const glm::vec3& forward, std::size_t maxLights) {
        lights.clear();
        clusters.assign(static_cast<std::size_t>(kClusters) * 2, 0u);
        std::vector<const PointLight*> kept;
        for (const PointLight& l : input) {
            const float depth = glm::dot(l.position - cameraPosition, forward);
            if (l.radius <= 0.0f || l.intensity <= 0.0f || depth + l.radius < 0.0f || depth - l.radius > farDepth) continue;
            kept.push_back(&l);
        }
        const auto weight = [&](const PointLight* l) {
            const float d = glm::length(l->position - cameraPosition);
            return l->intensity * l->radius / std::max(d, 1.0f);
        };
        if (kept.size() > maxLights) {
            std::partial_sort(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(maxLights), kept.end(),
                              [&](const PointLight* a, const PointLight* b) { return weight(a) > weight(b); });
            kept.resize(maxLights);
        }
        // Each light's cluster box, then two passes over flat arrays: count per cluster,
        // then fill (no per-cluster lists, so the cost follows the lights, not the grid).
        boxes_.clear();
        for (std::size_t i = 0; i < kept.size(); ++i) {
            const PointLight& l = *kept[i];
            lights.emplace_back(l.position, l.radius);
            lights.emplace_back(l.color, l.intensity);
            const float depth = glm::dot(l.position - cameraPosition, forward);
            const int s0 = Slice(depth - l.radius), s1 = Slice(depth + l.radius);
            // The sphere's box on screen; the whole screen when it reaches behind the camera.
            glm::vec2 low(-1.0f), high(1.0f);
            if (depth - l.radius > 0.0f) {
                low = glm::vec2(1e9f);
                high = glm::vec2(-1e9f);
                bool behind = false;
                for (int c = 0; c < 8; ++c) {
                    const glm::vec3 corner = l.position + glm::vec3(c & 1 ? l.radius : -l.radius, c & 2 ? l.radius : -l.radius,
                                                                    c & 4 ? l.radius : -l.radius);
                    const glm::vec4 clip = viewProj * glm::vec4(corner, 1.0f);
                    if (clip.w <= 1e-3f) {
                        behind = true;
                        break;
                    }
                    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
                    low = glm::min(low, ndc);
                    high = glm::max(high, ndc);
                }
                if (behind) {
                    low = glm::vec2(-1.0f);
                    high = glm::vec2(1.0f);
                }
            }
            if (high.x < -1.0f || high.y < -1.0f || low.x > 1.0f || low.y > 1.0f) {
                boxes_.push_back({0, -1, 0, -1, 0, -1});
                continue;
            }
            const auto tile = [](float ndc, int tiles) {
                return std::clamp(static_cast<int>(std::floor((ndc * 0.5f + 0.5f) * static_cast<float>(tiles))), 0, tiles - 1);
            };
            boxes_.push_back({tile(low.x, kTilesX), tile(high.x, kTilesX), tile(low.y, kTilesY), tile(high.y, kTilesY), s0, s1});
        }
        const auto forEach = [&](auto&& fn) {
            for (std::size_t i = 0; i < boxes_.size(); ++i) {
                const auto& b = boxes_[i];
                for (int s = b.s0; s <= b.s1; ++s) {
                    for (int y = b.y0; y <= b.y1; ++y) {
                        for (int x = b.x0; x <= b.x1; ++x) fn(static_cast<std::size_t>((s * kTilesY + y) * kTilesX + x), i);
                    }
                }
            }
        };
        forEach([&](std::size_t c, std::size_t) { ++clusters[c * 2 + 1]; });
        std::size_t total = 0;
        for (std::size_t c = 0; c < static_cast<std::size_t>(kClusters); ++c) {
            const std::size_t room = kMaxIndices - std::min(kMaxIndices, total);
            const std::uint32_t count = static_cast<std::uint32_t>(std::min<std::size_t>(clusters[c * 2 + 1], room));
            clusters[c * 2] = static_cast<std::uint32_t>(total);
            clusters[c * 2 + 1] = count;
            total += count;
        }
        indices.assign(total, 0u);
        fill_.assign(static_cast<std::size_t>(kClusters), 0u);
        forEach([&](std::size_t c, std::size_t i) {
            if (fill_[c] < clusters[c * 2 + 1]) indices[clusters[c * 2] + fill_[c]++] = static_cast<std::uint32_t>(i);
        });
    }
    std::size_t count() const { return lights.size() / 2; }

private:
    struct ClusterBox {
        int x0, x1, y0, y1, s0, s1;
    };
    std::vector<ClusterBox> boxes_;
    std::vector<std::uint32_t> fill_;
};

} // namespace sr::render
