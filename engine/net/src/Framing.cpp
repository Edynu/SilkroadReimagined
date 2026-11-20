#include "sr/net/Framing.h"

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
        Message& m = out.emplace_back();
        m.id = MessageId(std::uint16_t(p[4]) | std::uint16_t(std::uint16_t(p[5]) << 8));
        m.payload.assign(p + kFrameHeaderSize, p + 4 + length);
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

void OutgoingQueue::Push(MessageId id, std::span<const std::byte> payload) {
    entries_.push_back({id, {payload.begin(), payload.end()}});
    pendingBytes_ += kFrameHeaderSize + payload.size();
}

void OutgoingQueue::PushLatest(std::uint64_t key, MessageId id, std::span<const std::byte> payload) {
    if (const auto it = latest_.find(key); it != latest_.end()) {
        Entry& e = entries_[it->second];
        pendingBytes_ -= e.payload.size();
        pendingBytes_ += payload.size();
        e.id = id;
        e.payload.assign(payload.begin(), payload.end());
        ++replaced_;
        return;
    }
    latest_.emplace(key, entries_.size());
    Push(id, payload);
}

bool OutgoingQueue::Take(std::vector<std::byte>& frames) {
    frames.clear();
    if (entries_.empty()) return false;
    frames.reserve(pendingBytes_);
    for (const Entry& e : entries_) AppendFrame(frames, e.id, e.payload);
    entries_.clear();
    latest_.clear();
    pendingBytes_ = 0;
    return true;
}

} // namespace sr::net
