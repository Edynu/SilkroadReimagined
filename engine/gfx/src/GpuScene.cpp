#include "sr/gfx/GpuScene.h"

#include <algorithm>
#include <span>

namespace sr::gfx {

bool GpuScene::Init(rhi::Device& device, std::uint32_t capacity, std::string& error) {
    device_ = &device;
    rhi::BufferDesc desc;
    desc.size = std::size_t{capacity} * sizeof(InstanceRecord);
    desc.usage = rhi::BufferUsage::Storage;
    desc.debugName = "instances";
    desc.cpuVisible = false;
    buffer_ = device.CreateBuffer(desc);
    if (!buffer_) {
        error = "cannot create the instance buffer";
        return false;
    }
    address_ = device.BufferAddress(buffer_);
    records_.assign(capacity, InstanceRecord{});
    free_.clear();
    next_ = live_ = 0;
    dirtyBegin_ = kInvalid;
    dirtyEnd_ = 0;
    return true;
}

void GpuScene::Shutdown() {
    if (device_ != nullptr && buffer_) device_->DestroyBuffer(buffer_);
    buffer_ = {};
    address_ = 0;
    records_.clear();
    free_.clear();
    next_ = live_ = 0;
}

std::uint32_t GpuScene::Add(const InstanceRecord& record) {
    std::uint32_t index;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
    } else if (next_ < records_.size()) {
        index = next_++;
    } else {
        return kInvalid;
    }
    ++live_;
    Set(index, record);
    return index;
}

void GpuScene::Set(std::uint32_t instance, const InstanceRecord& record) {
    if (instance >= records_.size()) return;
    records_[instance] = record;
    dirtyBegin_ = std::min(dirtyBegin_, instance);
    dirtyEnd_ = std::max(dirtyEnd_, instance + 1);
}

void GpuScene::Remove(std::uint32_t instance) {
    if (instance >= records_.size()) return;
    Set(instance, InstanceRecord{}); // no parts: the culling pass skips the slot
    free_.push_back(instance);
    --live_;
}

std::uint32_t GpuScene::High() const { return next_; }

void GpuScene::Upload(rhi::CommandList& cmd) {
    if (dirtyBegin_ >= dirtyEnd_) return;
    const auto bytes = std::as_bytes(std::span(records_).subspan(dirtyBegin_, dirtyEnd_ - dirtyBegin_));
    cmd.UpdateBuffer(buffer_, std::size_t{dirtyBegin_} * sizeof(InstanceRecord), bytes);
    dirtyBegin_ = kInvalid;
    dirtyEnd_ = 0;
}

} // namespace sr::gfx
