#include "sr/core/RangeAllocator.h"

#include <iterator>

namespace sr::core {

void RangeAllocator::Reset(std::uint64_t capacity, std::uint64_t granularity) {
    granularity_ = granularity == 0 ? 1 : granularity;
    capacity_ = capacity / granularity_ * granularity_;
    used_ = 0;
    free_.clear();
    bySize_.clear();
    allocated_.clear();
    if (capacity_ > 0) InsertFree(0, capacity_);
}

void RangeAllocator::InsertFree(std::uint64_t offset, std::uint64_t size) {
    free_.emplace(offset, size);
    bySize_.emplace(size, offset);
}

void RangeAllocator::EraseFree(std::map<std::uint64_t, std::uint64_t>::iterator it) {
    bySize_.erase({it->second, it->first});
    free_.erase(it);
}

std::optional<std::uint64_t> RangeAllocator::Allocate(std::uint64_t size) {
    if (size == 0) size = 1;
    const std::uint64_t rounded = (size + granularity_ - 1) / granularity_ * granularity_;
    // Best fit: the smallest free range that holds it (lowest offset among equal sizes).
    const auto fit = bySize_.lower_bound({rounded, 0});
    if (fit == bySize_.end()) return std::nullopt;
    const std::uint64_t offset = fit->second;
    const std::uint64_t rangeSize = fit->first;
    EraseFree(free_.find(offset));
    if (rangeSize > rounded) InsertFree(offset + rounded, rangeSize - rounded);
    allocated_.emplace(offset, rounded);
    used_ += rounded;
    return offset;
}

bool RangeAllocator::Free(std::uint64_t offset) {
    const auto it = allocated_.find(offset);
    if (it == allocated_.end()) return false;
    std::uint64_t start = offset;
    std::uint64_t size = it->second;
    used_ -= size;
    allocated_.erase(it);
    // Merge with the free range after it, then the one before it.
    const auto next = free_.find(start + size);
    if (next != free_.end()) {
        size += next->second;
        EraseFree(next);
    }
    auto after = free_.lower_bound(start);
    if (after != free_.begin()) {
        const auto prev = std::prev(after);
        if (prev->first + prev->second == start) {
            start = prev->first;
            size += prev->second;
            EraseFree(prev);
        }
    }
    InsertFree(start, size);
    return true;
}

std::uint64_t RangeAllocator::SizeOf(std::uint64_t offset) const {
    const auto it = allocated_.find(offset);
    return it == allocated_.end() ? 0 : it->second;
}

} // namespace sr::core
