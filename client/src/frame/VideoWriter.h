#pragma once

// A video written from the frames (the skill bench's videos, L11): each frame's pixels, RGBA as
// the capture reads them back, piped into an ffmpeg process that encodes them (H.264 in MP4, at a fixed rate: run the
// client with --fixed-dt so the game's time is the video's). The render thread owns it; nothing here is threaded.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

namespace sr::client {

class VideoWriter {
public:
    VideoWriter() = default;
    VideoWriter(const VideoWriter&) = delete;
    VideoWriter& operator=(const VideoWriter&) = delete;
    ~VideoWriter() { Close(); }

    // Starts `ffmpeg` (a path, or a name on PATH) writing `file`: frames of width x height at `fps`, scaled to at most
    // `maxWidth` wide (0: as they are).
    // `audioRate` > 0: the frames come with their sound (AddAudio), kept beside the video as WAV and put into it as AAC
    // when it closes.
    bool Open(const std::filesystem::path& file, const std::string& ffmpeg, std::uint32_t width, std::uint32_t height, int fps, std::uint32_t maxWidth,
              std::string& error, int audioRate = 0);
    // The sound of a frame's time: stereo 16-bit samples.
    void AddAudio(std::span<const std::int16_t> samples);
    // One frame of width x height x 4 bytes (false: the encoder is gone).
    bool Write(std::span<const std::uint8_t> rgba);
    // The input closed and the encoder waited for (its file finished); true when it exited cleanly.
    bool Close();

    bool IsOpen() const { return process_ != nullptr; }
    const std::filesystem::path& File() const { return file_; }
    std::uint32_t Width() const { return width_; }
    std::uint32_t Height() const { return height_; }
    std::uint64_t Frames() const { return frames_; }

private:
    void* process_ = nullptr; // the encoder's process handle (Windows)
    void* input_ = nullptr;   // the write end of its standard input
    std::filesystem::path file_;
    std::string ffmpeg_;
    std::ofstream wav_;
    std::uint64_t audioBytes_ = 0;
    int audioRate_ = 0;
    bool Mux(); // the WAV into the video (the picture copied, the sound AAC)
    std::uint32_t width_ = 0, height_ = 0;
    std::uint64_t frames_ = 0;
};

} // namespace sr::client
