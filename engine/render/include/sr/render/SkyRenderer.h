#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <glm/glm.hpp>

#include "sr/rhi/Rhi.h"

namespace sr::render {

class TextureCache;

// The original's sky (shaders/Sky.hlsl), drawn first in the scene pass without depth:
//  - a dome of radius 20000 (an octahedron split four times; its upper half) coloured per
//    vertex from the horizon to the zenith, with the glow around the sun;
//  - 3000 stars on the upper dome, the first 1000 twinkling in ten groups of 100;
//  - the sun (sun\lens2.ddj, tinted) from 6:00 to 21:00, on an east-west arc;
//  - the moon (sun\moon01..29.ddj by the lunar day) on its own, faster arc at night;
//  - a cloud plane (skybox\cloud1.ddj) scrolling over a 500 s cycle, into the fog far off;
//  - a fog-coloured skirt below the horizon.
class SkyRenderer {
public:
    struct State {
        float timeOfDay = 0.5f;
        double seconds = 0.0;
        int lunarDay = 14;      // 0..29; 29 has no moon
        float starAlpha = 0.0f; // the environment's star curve (-1..1)
        float cloudAlpha = 0.0f;
    };

    bool Init(rhi::Device& device, rhi::Format colorFormat, rhi::Format depthFormat, std::string& error);
    void Shutdown();
    // Loads the sun, moon and cloud textures (once) and runs the stars' twinkle (10 times a second).
    void Update(TextureCache& textures, double seconds);
    // `viewProj` and `cameraPosition` as the scene's; `frame` the FrameConstants address.
    void Draw(rhi::CommandList& cmd, const glm::mat4& viewProj, const glm::vec3& cameraPosition, std::uint64_t frame,
              const State& state, float width, float height) const;

private:
    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle vertices_, indices_;
    struct Range {
        std::uint32_t first = 0, count = 0;
    };
    std::array<Range, 7> ranges_{}; // by kind (1..6)
    bool texturesLoaded_ = false;
    std::uint32_t sunTexture_ = 0, cloudTexture_ = 0;
    std::array<std::uint32_t, 29> moonTextures_{};
    // Twinkle: the bytes of the ten groups, the generator, the last tick.
    std::array<std::uint8_t, 10> flicker_{};
    std::uint32_t random_ = 1;
    double flickerTick_ = 0.0;
};

} // namespace sr::render
