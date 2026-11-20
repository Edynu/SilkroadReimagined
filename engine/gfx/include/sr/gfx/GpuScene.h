#pragma once

// Persistent instance records in a device-local buffer: the render thread
// adds, changes and removes them from the snapshot; the changed range is copied at the start of
// the frame. C3's culling and indirect draws read the same buffer. Render thread only.

#include <cstdint>
#include <string>
#include <vector>

#include "sr/rhi/Rhi.h"

namespace sr::gfx {

// InstanceRecord::flags.
// Skinned by the frame's palette (world instances: WorldPass's skinning pass; parts without bones
// draw as they are).
constexpr std::uint32_t kInstanceSkinned = 1;
// Bits 1..7: 1 + the frame's material tint the instance takes (0 none): its lit colour times it (a
// skill's SCT_MAT, the 1.188 model's material diffuse and ambient; World.hlsl, kFrameLights.yz).
constexpr std::uint32_t kInstanceTintShift = 1, kInstanceTintMask = 0xFE;
// Bits 8..15 of the flags: how far the instance has faded out (0 whole, 255 gone), drawn by leaving
// out pixels in an ordered pattern (no blending: it stays in the opaque draws).
constexpr std::uint32_t kInstanceFadeShift = 8, kInstanceFadeMask = 0xFF00;

// 64 bytes, read by shaders at instances + index * 64.
struct InstanceRecord {
    float world[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; // rows of a 3x4 world matrix
    std::uint32_t paletteOffset = 0; // first skinning matrix of the instance (in the frame's palette)
    std::uint32_t flags = 0;         // kInstance*; bits 16-31: 1 + the frame's point light it takes (0 none)
    // The model's parts in the mesh table (ModelInfo::meshBase, meshCount); 0 parts: not drawn
    // (a free slot, or a model not resident yet).
    std::uint32_t meshBase = 0;
    std::uint32_t meshCount = 0;
};
static_assert(sizeof(InstanceRecord) == 64);

class GpuScene {
public:
    static constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;

    bool Init(rhi::Device& device, std::uint32_t capacity, std::string& error);
    void Shutdown();

    // kInvalid when full.
    std::uint32_t Add(const InstanceRecord& record);
    void Set(std::uint32_t instance, const InstanceRecord& record);
    void Remove(std::uint32_t instance);
    const InstanceRecord& Get(std::uint32_t instance) const { return records_[instance]; }

    // Outside passes, before the frame's draws: copies the records changed since the last upload.
    void Upload(rhi::CommandList& cmd);

    std::uint64_t Address() const { return address_; }
    rhi::BufferHandle Buffer() const { return buffer_; }
    std::uint32_t Live() const { return live_; }
    // One past the highest slot ever used: the culling pass walks [0, High()).
    std::uint32_t High() const;
    std::uint32_t Capacity() const { return static_cast<std::uint32_t>(records_.size()); }

private:
    rhi::Device* device_ = nullptr;
    rhi::BufferHandle buffer_;
    std::uint64_t address_ = 0;
    std::vector<InstanceRecord> records_;
    std::vector<std::uint32_t> free_;
    std::uint32_t next_ = 0, live_ = 0;
    std::uint32_t dirtyBegin_ = kInvalid, dirtyEnd_ = 0; // [begin, end)
};

} // namespace sr::gfx
