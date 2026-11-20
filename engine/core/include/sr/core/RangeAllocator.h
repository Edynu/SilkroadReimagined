#pragma once

// Sub-allocates ranges of one big block (a GPU buffer reserved at start):
// best fit, sizes rounded up to the granularity, free neighbours merged. Not thread-safe; the
// owner locks.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace sr::core {

class RangeAllocator {
public:
    RangeAllocator() = default;
    RangeAllocator(std::uint64_t capacity, std::uint64_t granularity) { Reset(capacity, granularity); }

    // Forgets every allocation: one free range of `capacity` (rounded down to the granularity).
    void Reset(std::uint64_t capacity, std::uint64_t granularity);

    // The offset of a free range of at least `size` bytes, or nothing if no free range is big
    // enough. The allocation's size is `size` rounded up to the granularity.
    std::optional<std::uint64_t> Allocate(std::uint64_t size);
    // Frees what Allocate returned at `offset` (its size was recorded). False if `offset` is not
    // the start of an allocation.
    bool Free(std::uint64_t offset);

    std::uint64_t Capacity() const { return capacity_; }
    std::uint64_t Used() const { return used_; }
    std::uint64_t LargestFree() const { return bySize_.empty() ? 0 : bySize_.rbegin()->first; }
    std::size_t FreeRanges() const { return free_.size(); }
    std::size_t Allocations() const { return allocated_.size(); }
    // The size of the allocation at `offset` (0 if there is none).
    std::uint64_t SizeOf(std::uint64_t offset) const;

private:
    void InsertFree(std::uint64_t offset, std::uint64_t size);
    void EraseFree(std::map<std::uint64_t, std::uint64_t>::iterator it);

    std::uint64_t capacity_ = 0;
    std::uint64_t granularity_ = 1;
    std::uint64_t used_ = 0;
    std::map<std::uint64_t, std::uint64_t> free_;                // offset -> size
    std::set<std::pair<std::uint64_t, std::uint64_t>> bySize_;   // (size, offset), for best fit
    std::map<std::uint64_t, std::uint64_t> allocated_;           // offset -> size
};

} // namespace sr::core
