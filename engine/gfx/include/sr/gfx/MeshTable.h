#pragma once

// Every resident model's parts as GPU records: what the culling pass tests
// and the draw pass reads, by index. A model owns a contiguous range (ModelInfo::meshBase,
// meshCount), written when it becomes Resident and freed when it is evicted (reused once the
// frames that could read it are done). One buffer reserved at start; render thread only.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "sr/core/RangeAllocator.h"
#include "sr/rhi/Rhi.h"

namespace sr::gfx {

// 64 bytes, read by shaders at meshes + index * 64.
struct MeshRecord {
    std::uint32_t firstIndex = 0;   // into the geometry pool's 16-bit indices
    std::uint32_t indexCount = 0;
    std::uint32_t vertexOffset = 0; // bytes into the geometry pool
    std::uint32_t vertexFlags = 0;  // kVertex*
    std::uint32_t texture = 0;      // bindless index (0: white)
    std::uint32_t materialFlags = 0; // kMaterial*
    std::uint32_t bucket = 0;       // the draw list it goes to (kBucket*)
    std::uint32_t vertexCount = 0;  // (the skinning pass walks them)
    float sphere[4]{};              // bounding sphere in model space: centre, radius
    std::uint32_t lightmap = 0;     // bindless index of the part's lightmap (0: none)
    // kMaterialScrolls (AssetStore.h): the texture matrix's rates per second as half floats,
    // (offset u, v), (m11, m12), (m21, m22). kMaterialEquipGlow: `lightmap` is the glow's texture and these are the
    // offset's growth per second (u, v as half floats), the colour's first end (RGB, A: the period / 20 ms) and its
    // other end (RGB, A: 1 modulate, 2 modulate 2x).
    std::uint32_t uvRate[3]{};
};
static_assert(sizeof(MeshRecord) == 64);

// Draw lists (one pipeline each): the original draws objects culling counter-clockwise faces, except for two-sided
// materials (flag 1). A model's front and
// back faces often lie on each other (sails, banners): drawn both, they fight. Translucent parts
// (kMaterialBlended: waterfalls, glows) draw after the solid ones, blended, without depth writes.
constexpr std::uint32_t kBucketCulled = 0;
constexpr std::uint32_t kBucketTwoSided = 1;
constexpr std::uint32_t kBucketBlended = 2;
constexpr std::uint32_t kBucketCount = 3;

class MeshTable {
public:
    bool Init(rhi::Device& device, std::uint32_t capacity, std::string& error);
    void Shutdown();

    // A range for `records` (the base index), or nothing when the table is full.
    std::optional<std::uint32_t> Add(std::span<const MeshRecord> records);
    // Reusable once the frames recorded until now are done.
    void Free(std::uint32_t base);
    // Once a frame: deferred frees whose frames finished.
    void Collect();
    // Outside passes: copies the records changed since the last upload.
    void Upload(rhi::CommandList& cmd);

    rhi::BufferHandle Buffer() const { return buffer_; }
    std::uint64_t Address() const { return address_; }
    std::uint32_t Used() const { return static_cast<std::uint32_t>(ranges_.Used()); }
    std::uint32_t Capacity() const { return static_cast<std::uint32_t>(records_.size()); }

private:
    rhi::Device* device_ = nullptr;
    rhi::BufferHandle buffer_;
    std::uint64_t address_ = 0;
    std::vector<MeshRecord> records_;
    core::RangeAllocator ranges_;
    struct PendingFree {
        std::uint32_t base = 0;
        std::uint64_t frame = 0;
    };
    std::vector<PendingFree> pending_;
    std::uint32_t dirtyBegin_ = 0xFFFFFFFFu, dirtyEnd_ = 0;
};

} // namespace sr::gfx
