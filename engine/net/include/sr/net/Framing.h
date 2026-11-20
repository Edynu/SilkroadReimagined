#pragma once

// Frames and lanes of our protocol, independent of the transport.
//
// A frame is a u32 length (of what follows it), a u16 message id and the payload, all
// little-endian. A receiver bounds the length before it allocates anything.
//
// Messages wait in an OutgoingQueue until the transport writes them. The reliable lane keeps
// every message in order; the latest lane keeps only the newest message per key while it
// waits (a position superseded before it was written is never sent), in the place of the
// first one, so the order between lanes stays the order of the first sends.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace sr::net {

using MessageId = std::uint16_t;

inline constexpr std::size_t kFrameHeaderSize = 6; // u32 length, u16 id

struct Message {
    MessageId id = 0;
    std::vector<std::byte> payload;
};

// Appends one frame to `out`.
void AppendFrame(std::vector<std::byte>& out, MessageId id, std::span<const std::byte> payload);

// Splits a received byte stream into messages.
class FrameParser {
public:
    // `maxFrame`: the largest id + payload this side accepts.
    explicit FrameParser(std::uint32_t maxFrame) : maxFrame_(maxFrame) {}

    // Adds received bytes and appends every complete message to `out`. Returns false on a
    // malformed frame (`error` says why); the stream is then unusable.
    bool Feed(std::span<const std::byte> bytes, std::vector<Message>& out, std::string& error);
    // Bytes of an incomplete frame, waiting for the rest.
    std::size_t Buffered() const { return buffer_.size() - offset_; }

private:
    std::uint32_t maxFrame_;
    std::vector<std::byte> buffer_;
    std::size_t offset_ = 0;
};

class OutgoingQueue {
public:
    // Reliable lane: always sent, in order.
    void Push(MessageId id, std::span<const std::byte> payload);
    // Latest lane: replaces a waiting message with the same key (keeping its place), or appends.
    void PushLatest(std::uint64_t key, MessageId id, std::span<const std::byte> payload);
    // Encodes every waiting message as frames into `frames` (replacing its content) and empties
    // the queue. False when nothing waits.
    bool Take(std::vector<std::byte>& frames);

    bool Empty() const { return entries_.empty(); }
    // Bytes the waiting messages take as frames.
    std::size_t PendingBytes() const { return pendingBytes_; }
    // Latest-lane messages dropped because a newer one replaced them (since construction).
    std::uint64_t Replaced() const { return replaced_; }

private:
    struct Entry {
        MessageId id = 0;
        std::vector<std::byte> payload;
    };
    std::vector<Entry> entries_;
    std::unordered_map<std::uint64_t, std::size_t> latest_; // key -> index in entries_
    std::size_t pendingBytes_ = 0;
    std::uint64_t replaced_ = 0;
};

} // namespace sr::net
