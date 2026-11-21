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
//
// Packed frames: a run of latest-lane messages of one id, one after the other in the queue (a
// tick's moves for one player), is written as a single frame of id kPackedId: a u16 message id,
// then each message as a LEB128 length and its payload. The parser hands them out as the
// messages they were; a frame carries at most kPackedMaxPayload bytes, a longer run is split.
// No message of the protocol may use kPackedId.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace sr::net {

using MessageId = std::uint16_t;

inline constexpr std::size_t kFrameHeaderSize = 6; // u32 length, u16 id
inline constexpr MessageId kPackedId = 0xFFFE;     // a packed frame (above); reserved
inline constexpr std::size_t kPackedMaxPayload = 32 * 1024;

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
    // Bytes the waiting messages take as frames (each as its own frame: at most what is written).
    std::size_t PendingBytes() const { return pendingBytes_; }
    // Latest-lane messages dropped because a newer one replaced them (since construction).
    std::uint64_t Replaced() const { return replaced_; }

private:
    // The waiting messages: their payloads back to back in `data_` (a replaced latest-lane payload stays there, unused,
    // until the next Take), so a message costs no allocation of its own; the buffers keep their room from one Take to the
    // next.
    struct Entry {
        MessageId id = 0;
        bool latest = false;
        std::uint32_t offset = 0, size = 0;
    };
    // The latest lane's keys -> their entries: open addressing, emptied by moving to the next generation.
    struct Slot {
        std::uint64_t key = 0;
        std::uint32_t entry = 0;
        std::uint32_t generation = 0;
    };
    std::uint32_t* FindLatest(std::uint64_t key, bool insert);
    void GrowLatest();
    void Store(Entry& e, std::span<const std::byte> payload);

    std::vector<Entry> entries_;
    std::vector<std::byte> data_;
    std::size_t liveBytes_ = 0; // payload bytes still in use in data_
    std::vector<Slot> slots_;
    std::size_t slotsUsed_ = 0;
    std::uint32_t generation_ = 1;
    std::size_t pendingBytes_ = 0;
    std::uint64_t replaced_ = 0;
};

} // namespace sr::net
