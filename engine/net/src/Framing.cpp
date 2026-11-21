#include "sr/net/Framing.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace sr::net {

namespace {

void PutU32(std::byte* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = std::byte((v >> (8 * i)) & 0xFF);
}

std::uint32_t GetU32(const std::byte* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= std::uint32_t(p[i]) << (8 * i);
    return v;
}

std::size_t LebSize(std::size_t v) {
    std::size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

void PutLeb(std::vector<std::byte>& out, std::size_t v) {
    while (v >= 0x80) {
        out.push_back(std::byte((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(std::byte(v));
}

// A packed frame's messages (Framing.h) appended to `out`; false when the payload is not one.
bool Unpack(std::span<const std::byte> payload, std::vector<Message>& out, std::string& error) {
    if (payload.size() < 2) {
        error = "a packed frame without its message id";
        return false;
    }
    const MessageId id = MessageId(std::uint16_t(payload[0]) | std::uint16_t(std::uint16_t(payload[1]) << 8));
    if (id == kPackedId) {
        error = "a packed frame inside a packed frame";
        return false;
    }
    std::size_t at = 2;
    while (at < payload.size()) {
        std::size_t length = 0;
        int shift = 0;
        for (;;) {
            if (at >= payload.size() || shift > 28) {
                error = "a packed frame's length runs past its end";
                return false;
            }
            const std::uint8_t b = std::uint8_t(payload[at++]);
            length |= std::size_t(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
        }
        if (length > payload.size() - at) {
            error = std::format("a packed message of {} bytes runs past its frame", length);
            return false;
        }
        Message& m = out.emplace_back();
        m.id = id;
        m.payload.assign(payload.data() + at, payload.data() + at + length);
        at += length;
    }
    return true;
}

} // namespace

void AppendFrame(std::vector<std::byte>& out, MessageId id, std::span<const std::byte> payload) {
    const std::size_t at = out.size();
    out.resize(at + kFrameHeaderSize + payload.size());
    PutU32(out.data() + at, std::uint32_t(2 + payload.size()));
    out[at + 4] = std::byte(id & 0xFF);
    out[at + 5] = std::byte(id >> 8);
    if (!payload.empty()) std::memcpy(out.data() + at + kFrameHeaderSize, payload.data(), payload.size());
}

bool FrameParser::Feed(std::span<const std::byte> bytes, std::vector<Message>& out, std::string& error) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    while (buffer_.size() - offset_ >= 4) {
        const std::byte* p = buffer_.data() + offset_;
        const std::uint32_t length = GetU32(p);
        if (length < 2) {
            error = std::format("frame length {} is shorter than a message id", length);
            return false;
        }
        if (length > maxFrame_) {
            error = std::format("frame length {} exceeds the limit {}", length, maxFrame_);
            return false;
        }
        if (buffer_.size() - offset_ < 4 + std::size_t(length)) break;
        const MessageId id = MessageId(std::uint16_t(p[4]) | std::uint16_t(std::uint16_t(p[5]) << 8));
        if (id == kPackedId) {
            if (!Unpack(std::span(p + kFrameHeaderSize, std::size_t(length) - 2), out, error)) return false;
        } else {
            Message& m = out.emplace_back();
            m.id = id;
            m.payload.assign(p + kFrameHeaderSize, p + 4 + length);
        }
        offset_ += 4 + std::size_t(length);
    }
    // Drop what was parsed once it is most of the buffer (amortised; no shift per frame).
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    } else if (offset_ > buffer_.size() / 2) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + std::ptrdiff_t(offset_));
        offset_ = 0;
    }
    return true;
}

void OutgoingQueue::Store(Entry& e, std::span<const std::byte> payload) {
    // A queue that keeps being replaced into while nothing is taken (a slow reader) would grow with unused payloads:
    // compacted once they are most of the buffer.
    if (data_.size() > 64 * 1024 && data_.size() > 2 * liveBytes_ + payload.size()) {
        std::vector<std::byte> kept;
        kept.reserve(liveBytes_ + payload.size());
        for (Entry& other : entries_) {
            if (&other == &e) continue; // re-stored below
            const std::uint32_t at = std::uint32_t(kept.size());
            kept.insert(kept.end(), data_.begin() + other.offset, data_.begin() + other.offset + other.size);
            other.offset = at;
        }
        data_.swap(kept);
    }
    e.offset = std::uint32_t(data_.size());
    e.size = std::uint32_t(payload.size());
    data_.insert(data_.end(), payload.begin(), payload.end());
}

void OutgoingQueue::Push(MessageId id, std::span<const std::byte> payload) {
    Entry& e = entries_.emplace_back();
    e.id = id;
    Store(e, payload);
    liveBytes_ += payload.size();
    pendingBytes_ += kFrameHeaderSize + payload.size();
}

void OutgoingQueue::PushLatest(std::uint64_t key, MessageId id, std::span<const std::byte> payload) {
    if (std::uint32_t* index = FindLatest(key, false)) {
        Entry& e = entries_[*index];
        liveBytes_ -= e.size;
        pendingBytes_ -= e.size;
        pendingBytes_ += payload.size();
        e.id = id;
        Store(e, payload);
        liveBytes_ += payload.size();
        ++replaced_;
        return;
    }
    *FindLatest(key, true) = std::uint32_t(entries_.size());
    Push(id, payload);
    entries_.back().latest = true;
}

std::uint32_t* OutgoingQueue::FindLatest(std::uint64_t key, bool insert) {
    if (insert && (slots_.empty() || (slotsUsed_ + 1) * 2 > slots_.size())) GrowLatest();
    if (slots_.empty()) return nullptr;
    const std::size_t mask = slots_.size() - 1;
    for (std::size_t i = std::size_t((key * 0x9E3779B97F4A7C15ull) >> 32) & mask;; i = (i + 1) & mask) {
        Slot& s = slots_[i];
        if (s.generation != generation_) {
            if (!insert) return nullptr;
            s.key = key;
            s.generation = generation_;
            ++slotsUsed_;
            return &s.entry;
        }
        if (s.key == key) return &s.entry;
    }
}

void OutgoingQueue::GrowLatest() {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(old.empty() ? 64 : old.size() * 2, Slot{});
    const std::uint32_t was = generation_;
    generation_ = 1;
    slotsUsed_ = 0;
    for (const Slot& s : old) {
        if (s.generation == was) *FindLatest(s.key, true) = s.entry;
    }
}

bool OutgoingQueue::Take(std::vector<std::byte>& frames) {
    frames.clear();
    if (entries_.empty()) return false;
    frames.reserve(pendingBytes_);
    const auto payload = [&](const Entry& e) { return std::span<const std::byte>(data_.data() + e.offset, e.size); };
    for (std::size_t i = 0; i < entries_.size();) {
        const Entry& first = entries_[i];
        // A run of latest-lane messages of one id: packed, as many to a frame as kPackedMaxPayload holds.
        std::size_t end = i + 1;
        if (first.latest) {
            while (end < entries_.size() && entries_[end].latest && entries_[end].id == first.id) ++end;
        }
        if (end - i < 2) {
            AppendFrame(frames, first.id, payload(first));
            i = end;
            continue;
        }
        while (i < end) {
            const std::size_t start = frames.size();
            frames.resize(start + kFrameHeaderSize + 2);
            frames[start + 4] = std::byte(kPackedId & 0xFF);
            frames[start + 5] = std::byte(kPackedId >> 8);
            frames[start + 6] = std::byte(first.id & 0xFF);
            frames[start + 7] = std::byte(first.id >> 8);
            std::size_t packed = 2;
            do {
                const Entry& e = entries_[i];
                const std::size_t need = LebSize(e.size) + e.size;
                if (packed > 2 && packed + need > kPackedMaxPayload) break;
                PutLeb(frames, e.size);
                frames.insert(frames.end(), data_.begin() + e.offset, data_.begin() + e.offset + e.size);
                packed += need;
                ++i;
            } while (i < end);
            PutU32(frames.data() + start, std::uint32_t(2 + packed));
        }
    }
    entries_.clear();
    data_.clear();
    liveBytes_ = 0;
    if (++generation_ == 0) { // wrapped: every slot's old generation could match again
        std::fill(slots_.begin(), slots_.end(), Slot{});
        generation_ = 1;
    }
    slotsUsed_ = 0;
    pendingBytes_ = 0;
    return true;
}

} // namespace sr::net
