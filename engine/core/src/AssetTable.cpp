#include "sr/core/AssetTable.h"

namespace sr::core {

const AssetTable::Entry* AssetTable::Get(AssetHandle handle) const {
    if (handle.index == 0 || handle.index >= entries_.size()) return nullptr;
    const Entry& e = entries_[handle.index];
    return e.live && e.generation == handle.generation ? &e : nullptr;
}

AssetTable::Entry* AssetTable::Get(AssetHandle handle) {
    return const_cast<Entry*>(static_cast<const AssetTable*>(this)->Get(handle));
}

void AssetTable::Unlink(std::uint32_t index) {
    Entry& e = entries_[index];
    if (!e.kept) return;
    if (e.prev != kNone) entries_[e.prev].next = e.next; else head_ = e.next;
    if (e.next != kNone) entries_[e.next].prev = e.prev; else tail_ = e.prev;
    e.prev = e.next = kNone;
    e.kept = false;
    --kept_;
}

void AssetTable::LinkBack(std::uint32_t index) {
    Entry& e = entries_[index];
    e.prev = tail_;
    e.next = kNone;
    if (tail_ != kNone) entries_[tail_].next = index; else head_ = index;
    tail_ = index;
    e.kept = true;
    ++kept_;
}

void AssetTable::Reserve(std::size_t entries) {
    std::lock_guard lock(mutex_);
    byKey_.reserve(entries);
    entries_.reserve(entries + 1);
    free_.reserve(entries);
}

AssetHandle AssetTable::Acquire(std::string_view key, bool& created) {
    std::lock_guard lock(mutex_);
    const std::string k(key);
    if (const auto it = byKey_.find(k); it != byKey_.end()) {
        Entry& e = entries_[it->second];
        if (e.references++ == 0) Unlink(it->second);
        created = false;
        return {it->second, e.generation};
    }
    std::uint32_t index;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
    } else {
        index = static_cast<std::uint32_t>(entries_.size());
        entries_.emplace_back();
    }
    Entry& e = entries_[index];
    e.key = k;
    e.references = 1;
    e.live = true;
    byKey_.emplace(k, index);
    ++live_;
    created = true;
    return {index, e.generation};
}

bool AssetTable::AddRef(AssetHandle handle) {
    std::lock_guard lock(mutex_);
    Entry* e = Get(handle);
    if (e == nullptr) return false;
    if (e->references++ == 0) Unlink(handle.index);
    return true;
}

bool AssetTable::Release(AssetHandle handle) {
    std::lock_guard lock(mutex_);
    Entry* e = Get(handle);
    if (e == nullptr || e->references == 0) return false;
    if (--e->references == 0) LinkBack(handle.index);
    return true;
}

bool AssetTable::Valid(AssetHandle handle) const {
    std::lock_guard lock(mutex_);
    return Get(handle) != nullptr;
}

std::uint32_t AssetTable::References(AssetHandle handle) const {
    std::lock_guard lock(mutex_);
    const Entry* e = Get(handle);
    return e != nullptr ? e->references : 0;
}

std::string AssetTable::Key(AssetHandle handle) const {
    std::lock_guard lock(mutex_);
    const Entry* e = Get(handle);
    return e != nullptr ? e->key : std::string();
}

AssetHandle AssetTable::Find(std::string_view key) const {
    std::lock_guard lock(mutex_);
    const auto it = byKey_.find(std::string(key));
    if (it == byKey_.end()) return {};
    return {it->second, entries_[it->second].generation};
}

AssetHandle AssetTable::OldestUnreferenced(std::size_t skip) const {
    std::lock_guard lock(mutex_);
    std::uint32_t index = head_;
    while (index != kNone && skip > 0) {
        index = entries_[index].next;
        --skip;
    }
    return index == kNone ? AssetHandle{} : AssetHandle{index, entries_[index].generation};
}

bool AssetTable::Remove(AssetHandle handle) {
    std::lock_guard lock(mutex_);
    Entry* e = Get(handle);
    if (e == nullptr || e->references != 0) return false;
    Unlink(handle.index);
    byKey_.erase(e->key);
    e->key.clear();
    e->live = false;
    ++e->generation;
    free_.push_back(handle.index);
    --live_;
    return true;
}

std::size_t AssetTable::Live() const {
    std::lock_guard lock(mutex_);
    return live_;
}

std::size_t AssetTable::Unreferenced() const {
    std::lock_guard lock(mutex_);
    return kept_;
}

std::uint32_t AssetTable::SlotCount() const {
    std::lock_guard lock(mutex_);
    return static_cast<std::uint32_t>(entries_.size());
}

} // namespace sr::core
