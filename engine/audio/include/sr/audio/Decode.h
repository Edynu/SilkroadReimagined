#pragma once

// Sound files to samples (no OpenAL here; the tests decode the whole game with it). Measured over
// v1.150: every sound effect is a RIFF WAVE of PCM (Data.pk2 prim\snd\..., 1,543 files: mono 16-bit at
// 22,050, 24,000 or 44,100 Hz, two 8-bit, one stereo), the music Ogg Vorbis (Music.pk2, 29 files at
// its root, stereo). Both come out as interleaved 16-bit samples.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sr::audio {

struct Pcm {
    int channels = 1;
    int rate = 22050;
    std::vector<std::int16_t> samples; // interleaved
    std::size_t Frames() const { return channels > 0 ? samples.size() / std::size_t(channels) : 0; }
};

// RIFF WAVE: the "fmt " chunk (PCM, or WAVE_FORMAT_EXTENSIBLE holding PCM; 8 or 16 bits, 1 or 2
// channels) and the "data" chunk; other chunks are skipped (each padded to an even size).
bool DecodeWav(std::span<const std::byte> bytes, Pcm& out, std::string& error);
// A whole Ogg Vorbis file (stb_vorbis).
bool DecodeOgg(std::span<const std::byte> bytes, Pcm& out, std::string& error);
// Either, by the bytes' signature ("RIFF" / "OggS").
bool DecodeSound(std::span<const std::byte> bytes, Pcm& out, std::string& error);

// An Ogg Vorbis file decoded a piece at a time (a music track is minutes long: tens of MB as
// samples). It keeps the file's bytes. One thread at a time (a job, then the next job).
class OggStream {
public:
    OggStream() = default;
    ~OggStream();
    OggStream(const OggStream&) = delete;
    OggStream& operator=(const OggStream&) = delete;

    bool Open(std::vector<std::byte> bytes, std::string& error);
    bool IsOpen() const { return vorbis_ != nullptr; }
    int Channels() const { return channels_; }
    int Rate() const { return rate_; }
    // Up to `frames` frames into `out` (interleaved, room for frames x channels); with `loop` it
    // goes on from the start at the end. The frames written (fewer only at the end without loop).
    std::size_t Read(std::int16_t* out, std::size_t frames, bool loop);

private:
    std::vector<std::byte> bytes_;
    void* vorbis_ = nullptr; // stb_vorbis*
    int channels_ = 0, rate_ = 0;
};

} // namespace sr::audio
