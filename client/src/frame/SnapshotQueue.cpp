#include <chrono>

#include "frame/FrameSnapshot.h"
#include "sr/core/Trace.h"

namespace sr::client {

SnapshotQueue::SnapshotQueue(WaitForGpu waitForGpu) : waitForGpu_(std::move(waitForGpu)) {
    states_.fill(State::Free);
}

FrameSnapshot* SnapshotQueue::BeginWrite() {
    SR_ZONE("snapshot: wait for a slot");
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex_);
    // Back-pressure: the render thread must have taken the last published frame.
    changed_.wait(lock, [&] { return stopping_ || ready_ < 0; });
    if (stopping_) return nullptr;
    int slot = -1;
    std::uint64_t oldest = UINT64_MAX;
    for (unsigned i = 0; i < kSlots; ++i) {
        if (states_[i] == State::Free && gpuFrames_[i] < oldest) {
            oldest = gpuFrames_[i];
            slot = static_cast<int>(i);
        }
    }
    // With three slots one is always free here: one may be read, none is ready, none written.
    states_[static_cast<std::size_t>(slot)] = State::Writing;
    writing_ = slot;
    const std::uint64_t gpuFrame = gpuFrames_[static_cast<std::size_t>(slot)];
    lock.unlock();
    // The GPU may still read this slot's buffers (frame `gpuFrame`): wait for it.
    if (gpuFrame != 0 && waitForGpu_) waitForGpu_(gpuFrame);
    {
        std::lock_guard relock(mutex_);
        writeWait_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    FrameSnapshot* snapshot = &slots_[static_cast<std::size_t>(slot)];
    snapshot->Reset();
    return snapshot;
}

void SnapshotQueue::Publish() {
    std::lock_guard lock(mutex_);
    if (writing_ < 0) return;
    states_[static_cast<std::size_t>(writing_)] = State::Ready;
    ready_ = writing_;
    writing_ = -1;
    changed_.notify_all();
}

FrameSnapshot* SnapshotQueue::BeginRead() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return stopping_ || ready_ >= 0; });
    if (ready_ < 0) return nullptr;
    reading_ = ready_;
    ready_ = -1;
    states_[static_cast<std::size_t>(reading_)] = State::Reading;
    changed_.notify_all(); // the game thread may start its next frame
    return &slots_[static_cast<std::size_t>(reading_)];
}

void SnapshotQueue::EndRead(std::uint64_t gpuFrame) {
    std::lock_guard lock(mutex_);
    if (reading_ < 0) return;
    states_[static_cast<std::size_t>(reading_)] = State::Free;
    gpuFrames_[static_cast<std::size_t>(reading_)] = gpuFrame;
    reading_ = -1;
    changed_.notify_all();
}

void SnapshotQueue::Stop() {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    changed_.notify_all();
}

unsigned SnapshotQueue::SlotIndex(const FrameSnapshot* snapshot) const {
    return static_cast<unsigned>(snapshot - slots_.data());
}

double SnapshotQueue::WriteWaitSeconds() const {
    std::lock_guard lock(mutex_);
    return writeWait_;
}

} // namespace sr::client
