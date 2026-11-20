#pragma once

#include <cstdint>
#include <span>
#include <string>

#include <glm/glm.hpp>

#include "sr/rhi/Rhi.h"

namespace sr::render {

// Effect geometry built on the CPU each frame (plates facing the camera, small meshes):
// world-space triangles, drawn in batches that share a texture and a blend.
struct ParticleVertex {
    float position[3];
    float uv[2];
    std::uint32_t color; // RGBA8 (r in the low byte)
};

struct ParticleBatch {
    std::uint32_t textureIndex = 0; // bindless; 0 = white
    bool premultiplied = false;
    bool additive = false;  // destination blend ONE
    bool sourceOne = false; // source blend ONE (alpha ignored)
    std::uint32_t firstVertex = 0, vertexCount = 0; // a triangle list
    bool hard = false;      // never soft (a marker lying on the ground)
};

class ParticleRenderer {
public:
    bool Init(rhi::Device& device, rhi::Format colorFormat, rhi::Format depthFormat, std::string& error);
    void Shutdown();
    // Soft particles: the scene's depth (a sampled pre-pass of the same size as the target).
    struct SoftDepth {
        std::uint32_t depthIndex = 0; // bindless; 0 = hard edges
        float width = 1, height = 1;
        float nearPlane = 1, farPlane = 10000;
        float softness = 12.0f; // world units over which an effect fades into what it meets
    };
    // Inside a pass with depth (after the opaque world): depth tested, not written.
    void Draw(rhi::CommandList& cmd, const glm::mat4& viewProj, std::span<const ParticleVertex> vertices,
              std::span<const ParticleBatch> batches, const SoftDepth* soft = nullptr) const;

private:
    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle alpha_, additive_;
};

} // namespace sr::render
