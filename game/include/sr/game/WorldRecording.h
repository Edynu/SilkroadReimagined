#pragma once

// The session recorder (C6d): the world's messages as they arrived (the time
// since the recording began, the id, the bytes), saved to a file and played back without a
// server. A replay hands the messages over by simulation time, so the same recording stepped at
// the same rate always gives the same picture (WorldView::Hash), however the frames fall.
//
// The file: "SRWREC01", a u32 count, then per message an f64 time, a u16 id, a u32 size and the
// payload (little-endian).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "sr/game/TitleSession.h"

namespace sr::game {

class WorldView;

struct RecordedMessage {
    double time = 0; // seconds since the recording began
    proto::MessageId id = 0;
    std::vector<std::byte> payload;
};

struct WorldRecording {
    std::vector<RecordedMessage> messages;
    double Duration() const { return messages.empty() ? 0.0 : messages.back().time; }
    bool Save(const std::filesystem::path& path, std::string& error) const;
    bool Load(const std::filesystem::path& path, std::string& error);
};

// A world message (the ids of WorldProtocol.h) as the session's event; false for another id or
// a malformed payload.
bool IsWorldMessage(proto::MessageId id);
// A world message's name for tools (the F1 Network panel); null for others.
const char* WorldMessageName(proto::MessageId id);
bool DecodeWorldEvent(proto::MessageId id, std::span<const std::byte> payload, TitleEvent& out);

class WorldReplay {
public:
    explicit WorldReplay(const WorldRecording& recording) : recording_(&recording) {}
    // The messages that arrived before `time` and were not taken yet, decoded (malformed ones
    // are skipped and counted).
    std::vector<TitleEvent> TakeUntil(double time);
    bool Finished() const { return next_ >= recording_->messages.size(); }
    int Malformed() const { return malformed_; }

private:
    const WorldRecording* recording_;
    std::size_t next_ = 0;
    int malformed_ = 0;
};

// Plays the whole recording into `view` (cleared first) at `rate` steps a second, `settle`
// seconds past its last message; returns the view's hash.
std::uint64_t ReplayHash(const WorldRecording& recording, double rate, double settle, WorldView& view);

} // namespace sr::game
