#pragma once

// The original's rain and snow (C4c), drawn after the world inside the scene
// pass: each record is a cluster of 9 drops or flakes on a 50-unit grid around it, expanded on the
// GPU into camera-facing triangles (rain streaks turn about the vertical only), alpha blended, depth
// tested but not written, without fog; then the rain's ripples on water and the lightning's
// screen flash. Render thread only.

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sr/rhi/Rhi.h"

namespace sr::gfx {

struct WeatherView {
    std::vector<glm::vec4> rain;    // cluster centres (xyz)
    std::vector<glm::vec4> snow[2]; // cluster centres, weather\snow1 and snow2
    std::vector<glm::vec4> ripples; // xyz, w: age in seconds (1.2 s each)
    glm::vec4 flash{1.0f, 1.0f, 1.0f, 0.0f}; // the flash's colour and alpha (none at (1, 1, 1, 0))
    bool Empty() const {
        return rain.empty() && snow[0].empty() && snow[1].empty() && ripples.empty() && !Flashing();
    }
    bool Flashing() const { return flash.a > 0.0f || flash.r < 1.0f || flash.g < 1.0f || flash.b < 1.0f; }
};

class WeatherPass {
public:
    bool Init(rhi::Device& device, rhi::Format color, rhi::Format depth, std::string& error);
    void Shutdown();

    struct Textures {
        std::uint32_t rain = 0, ripple = 0; // weather\rain1, rain2
        std::uint32_t snow[2]{};            // weather\snow1, snow2
    };
    // Inside the scene pass, after the world. `right` and `up`: the camera's (unit length);
    // `level` its right vector kept horizontal (the streaks' facing).
    void Draw(rhi::CommandList& cmd, const glm::mat4& viewProj, const WeatherView& view, const Textures& textures,
              const glm::vec3& right, const glm::vec3& up, const glm::vec3& level) const;

private:
    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle particles_, flash_;
};

} // namespace sr::gfx
