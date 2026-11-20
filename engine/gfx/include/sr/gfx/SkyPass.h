#pragma once

// The original's sky ("Time of day, sky and fog (C3e)"), drawn first, camera-centred,
// without depth: the dome (an octahedron subdivided 4 times, radius 20000; vertex colours from the
// horizon to the zenith by the gradient, turning to the sun glow near the sun), the stars at night,
// the sun and the moon, the clouds (a flat plane 20000 above, scrolling, fogged out far away), then
// the horizon band below the horizon in the fog colour. Render thread only.

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

#include "sr/gfx/Environment.h"
#include "sr/rhi/Rhi.h"

namespace sr::gfx {

class SkyPass {
public:
    bool Init(rhi::Device& device, rhi::Format color, rhi::Format depth, std::string& error);
    void Shutdown();

    struct Textures {
        std::uint32_t clouds = 0, sun = 0, moon = 0; // bindless (0: that part is not drawn)
    };
    // Inside the scene pass, before everything else. `viewProj` without the camera's position
    // and a far plane beyond the sky (5,000,000); `seconds` the wall clock for the clouds' scroll.
    void Draw(rhi::CommandList& cmd, const glm::mat4& viewProj, const SkyState& sky, const Textures& textures, double seconds,
              std::uint32_t width, std::uint32_t height) const;

private:
    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle dome_, blended_; // the dome; the stars, sprites, clouds and band (alpha blended)
    rhi::BufferHandle geometry_;          // dome then band vertices (float3, unit sphere), then stars
    std::uint32_t domeVertices_ = 0, bandVertices_ = 0, stars_ = 0;
    std::uint64_t starOffset_ = 0;
};

} // namespace sr::gfx
