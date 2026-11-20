#pragma once

// The bookkeeping of loaded assets: entries keyed by a normalized path,
// handed out as index + generation, reference counted. An entry whose last reference goes away is
// not removed (the original game frees it at once): it joins a least-recently-used
// keep list, from which the owner evicts when memory runs short. A new reference takes it back
// out. Thread-safe (one lock); the owner keeps each entry's payload in its own array, indexed by
// AssetHandle::index.

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sr::core {

struct AssetHandle {
    std::uint32_t index = 0; // 0 = null
    std::uint32_t generation = 0;
    explicit operator bool() const { return index != 0; }
    friend bool operator==(AssetHandle a, AssetHandle b) = default;
};

class AssetTable {
public:
    // Room for `entries` without growing: growing rehashes every key under the lock (milliseconds
    // at tens of thousands of assets).
    void Reserve(std::size_t entries);

    // The entry for `key` with one more reference; `created` tells whether it is new (the caller
    // then starts loading it). Keys are compared as given (normalize them first).
    AssetHandle Acquire(std::string_view key, bool& created);
    // Another reference to a live entry. False if the handle is stale.
    bool AddRef(AssetHandle handle);
    // One reference less; at zero the entry joins the keep list (newest last). False if stale.
    bool Release(AssetHandle handle);

    bool Valid(AssetHandle handle) const;
    std::uint32_t References(AssetHandle handle) const; // 0 for stale handles too
    std::string Key(AssetHandle handle) const;
    // The handle of a live entry by key (null if none); adds no reference.
    AssetHandle Find(std::string_view key) const;

    // The least recently released unreferenced entry, or null. `skip` of them are passed over
    // (an owner that cannot evict the oldest yet asks for the next).
    AssetHandle OldestUnreferenced(std::size_t skip = 0) const;
    // Removes an entry: its key is forgotten and its handle (and every copy) becomes stale. Only
    // unreferenced entries can be removed (false otherwise, or if stale).
    bool Remove(AssetHandle handle);

    std::size_t Live() const;
    std::size_t Unreferenced() const;
    // Slots ever created; indices are below this (for sizing the owner's payload array).
    std::uint32_t SlotCount() const;

private:
    static constexpr std::uint32_t kNone = 0;
    struct Entry {
        std::string key;
        std::uint32_t generation = 1;
        std::uint32_t references = 0;
        bool live = false;
        std::uint32_t prev = kNone, next = kNone; // keep list links (by index)
        bool kept = false;                       // in the keep list
    };
    const Entry* Get(AssetHandle handle) const;
    Entry* Get(AssetHandle handle);
    void Unlink(std::uint32_t index);
    void LinkBack(std::uint32_t index);

    mutable std::mutex mutex_;
    std::vector<Entry> entries_{1}; // index 0 unused: a zero handle is null
    std::vector<std::uint32_t> free_;
    std::unordered_map<std::string, std::uint32_t> byKey_;
    std::uint32_t head_ = kNone, tail_ = kNone; // keep list, oldest first
    std::size_t live_ = 0, kept_ = 0;
};

} // namespace sr::core
