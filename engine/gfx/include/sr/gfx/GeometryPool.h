#pragma once

// All streamed vertices and indices live in one device-local buffer reserved at start: shaders read vertices by
// address (vertex pulling) and the buffer is
// bound once as the index buffer. Ranges come from a best-fit allocator; a freed range is reused
// only once the GPU finished every frame that could still read it. Thread-safe.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "sr/core/RangeAllocator.h"
#include "sr/rhi/Rhi.h"

namespace sr::gfx {

class GeometryPool {
public:
    static constexpr std::uint64_t kGranularity = 256;

    bool Init(rhi::Device& device, std::size_t bytes, std::string& error);
    // Gives the buffer back (deferred by the device). Every range must be free by then.
    void Shutdown();

    // The offset of `bytes` bytes, or nothing when no free range is big enough.
    std::optional<std::uint64_t> Allocate(std::uint64_t bytes);
    // The range at `offset` becomes reusable once the frames recorded until now are done.
    void Free(std::uint64_t offset);
    // Render thread, once a frame: returns deferred frees whose frames finished.
    void Collect();

    rhi::BufferHandle Buffer() const { return buffer_; }
    std::uint64_t Address() const { return address_; }

    struct Stats {
        std::uint64_t capacity = 0, used = 0, largestFree = 0;
        std::size_t allocations = 0, pendingFrees = 0;
    };
    Stats GetStats() const;

private:
    rhi::Device* device_ = nullptr;
    rhi::BufferHandle buffer_;
    std::uint64_t address_ = 0;
    mutable std::mutex mutex_;
    core::RangeAllocator ranges_;
    struct PendingFree {
        std::uint64_t offset = 0;
        std::uint64_t frame = 0; // reusable once the GPU finished this frame
    };
    std::vector<PendingFree> pending_;
};

} // namespace sr::gfx
