#pragma once

// Draws streamed models (AssetStore) from the GPU scene: one indexed draw per part, vertices pulled
// from the geometry pool. The simple lit pass of C2's viewer and asset check; C3's renderer
// replaces it with GPU-driven passes over the same data.

#include <cstdint>
#include <span>
#include <string>

#include <glm/glm.hpp>

#include "sr/gfx/AssetStore.h"
#include "sr/gfx/GeometryPool.h"
#include "sr/gfx/GpuScene.h"
#include "sr/rhi/Rhi.h"

namespace sr::gfx {

class ModelPass {
public:
    // Pipelines for passes with these colour and depth formats.
    bool Init(rhi::Device& device, rhi::Format color, rhi::Format depth, std::string& error);
    void Shutdown();

    struct Item {
        const ModelInfo* model = nullptr; // Resident
        std::uint32_t instance = 0;       // GpuScene record (its palette offset for skinned parts)
        bool skinned = false;             // its record's palette offset is valid: skin the parts
    };
    // Into an open pass. `palette`: the address of the frame's skinning matrices (0 if none).
    void Record(rhi::CommandList& cmd, const glm::mat4& viewProj, std::span<const Item> items, const GeometryPool& pool,
                const GpuScene& scene, std::uint64_t palette) const;

private:
    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle pipeline_;
};

} // namespace sr::gfx
