#include "sr/gfx/GeometryPool.h"

namespace sr::gfx {

bool GeometryPool::Init(rhi::Device& device, std::size_t bytes, std::string& error) {
    device_ = &device;
    rhi::BufferDesc desc;
    desc.size = bytes / kGranularity * kGranularity;
    desc.usage = rhi::BufferUsage::Index | rhi::BufferUsage::Storage;
    desc.debugName = "geometry pool";
    desc.cpuVisible = false;
    buffer_ = device.CreateBuffer(desc);
    if (!buffer_) {
        error = "cannot reserve the geometry pool (" + std::to_string(bytes >> 20) + " MiB)";
        return false;
    }
    address_ = device.BufferAddress(buffer_);
    std::lock_guard lock(mutex_);
    ranges_.Reset(desc.size, kGranularity);
    pending_.clear();
    return true;
}

void GeometryPool::Shutdown() {
    if (device_ != nullptr && buffer_) device_->DestroyBuffer(buffer_);
    buffer_ = {};
    address_ = 0;
    std::lock_guard lock(mutex_);
    ranges_.Reset(0, kGranularity);
    pending_.clear();
}

std::optional<std::uint64_t> GeometryPool::Allocate(std::uint64_t bytes) {
    std::lock_guard lock(mutex_);
    return ranges_.Allocate(bytes);
}

void GeometryPool::Free(std::uint64_t offset) {
    // Any frame up to the one being recorded may still read it.
    const std::uint64_t frame = device_ != nullptr ? device_->RecordingFrame() : 0;
    std::lock_guard lock(mutex_);
    pending_.push_back({offset, frame});
}

void GeometryPool::Collect() {
    if (device_ == nullptr) return;
    const std::uint64_t done = device_->CompletedFrame();
    std::lock_guard lock(mutex_);
    std::size_t kept = 0;
    for (const PendingFree& f : pending_) {
        if (f.frame <= done) {
            ranges_.Free(f.offset);
        } else {
            pending_[kept++] = f;
        }
    }
    pending_.resize(kept);
}

GeometryPool::Stats GeometryPool::GetStats() const {
    std::lock_guard lock(mutex_);
    Stats s;
    s.capacity = ranges_.Capacity();
    s.used = ranges_.Used();
    s.largestFree = ranges_.LargestFree();
    s.allocations = ranges_.Allocations();
    s.pendingFrees = pending_.size();
    return s;
}

} // namespace sr::gfx
