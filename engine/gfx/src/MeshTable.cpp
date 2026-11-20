#include "sr/gfx/MeshTable.h"

#include <algorithm>
#include <cstring>

namespace sr::gfx {

bool MeshTable::Init(rhi::Device& device, std::uint32_t capacity, std::string& error) {
    device_ = &device;
    rhi::BufferDesc desc;
    desc.size = std::size_t{capacity} * sizeof(MeshRecord);
    desc.usage = rhi::BufferUsage::Storage;
    desc.debugName = "mesh table";
    desc.cpuVisible = false;
    buffer_ = device.CreateBuffer(desc);
    if (!buffer_) {
        error = "cannot create the mesh table";
        return false;
    }
    address_ = device.BufferAddress(buffer_);
    records_.assign(capacity, MeshRecord{});
    ranges_.Reset(capacity, 1);
    pending_.clear();
    return true;
}

void MeshTable::Shutdown() {
    if (device_ != nullptr && buffer_) device_->DestroyBuffer(buffer_);
    buffer_ = {};
    address_ = 0;
    records_.clear();
    ranges_.Reset(0, 1);
}

std::optional<std::uint32_t> MeshTable::Add(std::span<const MeshRecord> records) {
    if (records.empty()) return std::nullopt;
    const auto base = ranges_.Allocate(records.size());
    if (!base) return std::nullopt;
    const auto first = static_cast<std::uint32_t>(*base);
    std::copy(records.begin(), records.end(), records_.begin() + first);
    dirtyBegin_ = std::min(dirtyBegin_, first);
    dirtyEnd_ = std::max(dirtyEnd_, first + static_cast<std::uint32_t>(records.size()));
    return first;
}

void MeshTable::Free(std::uint32_t base) {
    pending_.push_back({base, device_ != nullptr ? device_->RecordingFrame() : 0});
}

void MeshTable::Collect() {
    if (device_ == nullptr) return;
    const std::uint64_t done = device_->CompletedFrame();
    std::size_t kept = 0;
    for (const PendingFree& f : pending_) {
        if (f.frame <= done) {
            ranges_.Free(f.base);
        } else {
            pending_[kept++] = f;
        }
    }
    pending_.resize(kept);
}

void MeshTable::Upload(rhi::CommandList& cmd) {
    if (dirtyBegin_ >= dirtyEnd_) return;
    const auto bytes = std::as_bytes(std::span(records_).subspan(dirtyBegin_, dirtyEnd_ - dirtyBegin_));
    cmd.UpdateBuffer(buffer_, std::size_t{dirtyBegin_} * sizeof(MeshRecord), bytes);
    dirtyBegin_ = 0xFFFFFFFFu;
    dirtyEnd_ = 0;
}

} // namespace sr::gfx
