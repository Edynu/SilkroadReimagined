#include "sr/audio/Decode.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_HEADER_ONLY
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <stb_vorbis.c>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace sr::audio {

namespace {

std::uint16_t U16(const std::byte* p) { return std::uint16_t(std::uint16_t(p[0]) | std::uint16_t(p[1]) << 8); }
std::uint32_t U32(const std::byte* p) { return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24; }
bool Tag(const std::byte* p, const char* tag) { return std::memcmp(p, tag, 4) == 0; }

stb_vorbis* Vorbis(void* p) { return static_cast<stb_vorbis*>(p); }

} // namespace

bool DecodeWav(std::span<const std::byte> bytes, Pcm& out, std::string& error) {
    if (bytes.size() < 12 || !Tag(bytes.data(), "RIFF") || !Tag(bytes.data() + 8, "WAVE")) {
        error = "not a RIFF WAVE file";
        return false;
    }
    int format = 0, channels = 0, rate = 0, bits = 0;
    std::span<const std::byte> data;
    bool hasData = false; // an empty one is a file of silence (prim\snd\player\black.wav)
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const std::byte* chunk = bytes.data() + at;
        const std::size_t size = U32(chunk + 4);
        const std::size_t body = at + 8;
        const std::size_t available = std::min(size, bytes.size() - body); // a cut last chunk is read as far as it goes
        if (Tag(chunk, "fmt ") && available >= 16) {
            format = U16(chunk + 8);
            channels = U16(chunk + 10);
            rate = int(U32(chunk + 12));
            bits = U16(chunk + 22);
            // WAVE_FORMAT_EXTENSIBLE: the sub-format's first two bytes are the format tag.
            if (format == 0xFFFE && available >= 26) format = U16(chunk + 8 + 24);
        } else if (Tag(chunk, "data")) {
            data = bytes.subspan(body, available);
            hasData = true;
        }
        at = body + size + (size & 1);
    }
    if (format != 1) {
        error = format == 0 ? "no fmt chunk" : "not PCM (format " + std::to_string(format) + ")";
        return false;
    }
    // 8 and 16 bits; 24 (1.188's winter-event sounds, four files) is cut to its top 16.
    if ((channels != 1 && channels != 2) || (bits != 8 && bits != 16 && bits != 24) || rate <= 0) {
        error = "unsupported PCM: " + std::to_string(channels) + " channels, " + std::to_string(bits) + " bits, " + std::to_string(rate) + " Hz";
        return false;
    }
    if (!hasData) {
        error = "no data chunk";
        return false;
    }
    out.channels = channels;
    out.rate = rate;
    const std::size_t blockBytes = std::size_t(channels) * std::size_t(bits / 8);
    const std::size_t frames = data.size() / blockBytes;
    out.samples.resize(frames * std::size_t(channels));
    if (bits == 16) {
        std::memcpy(out.samples.data(), data.data(), out.samples.size() * 2); // little-endian, as the machine
    } else if (bits == 24) {
        for (std::size_t i = 0; i < out.samples.size(); ++i) {
            out.samples[i] = std::int16_t(std::uint16_t(std::to_integer<unsigned>(data[3 * i + 1]) | std::to_integer<unsigned>(data[3 * i + 2]) << 8));
        }
    } else {
        for (std::size_t i = 0; i < out.samples.size(); ++i) out.samples[i] = std::int16_t((int(data[i]) - 128) * 256); // unsigned 8-bit
    }
    return true;
}

bool DecodeOgg(std::span<const std::byte> bytes, Pcm& out, std::string& error) {
    int channels = 0, rate = 0;
    short* samples = nullptr;
    const int frames = stb_vorbis_decode_memory(reinterpret_cast<const unsigned char*>(bytes.data()), int(bytes.size()), &channels, &rate, &samples);
    if (frames < 0 || samples == nullptr || channels <= 0) {
        std::free(samples);
        error = "not a readable Ogg Vorbis file";
        return false;
    }
    out.channels = channels;
    out.rate = rate;
    out.samples.assign(samples, samples + std::size_t(frames) * std::size_t(channels));
    std::free(samples);
    return true;
}

bool DecodeSound(std::span<const std::byte> bytes, Pcm& out, std::string& error) {
    if (bytes.size() >= 4 && Tag(bytes.data(), "OggS")) return DecodeOgg(bytes, out, error);
    return DecodeWav(bytes, out, error);
}

OggStream::~OggStream() {
    if (vorbis_ != nullptr) stb_vorbis_close(Vorbis(vorbis_));
}

bool OggStream::Open(std::vector<std::byte> bytes, std::string& error) {
    if (vorbis_ != nullptr) stb_vorbis_close(Vorbis(vorbis_));
    vorbis_ = nullptr;
    bytes_ = std::move(bytes);
    int code = 0;
    stb_vorbis* v = stb_vorbis_open_memory(reinterpret_cast<const unsigned char*>(bytes_.data()), int(bytes_.size()), &code, nullptr);
    if (v == nullptr) {
        error = "not a readable Ogg Vorbis file (stb_vorbis error " + std::to_string(code) + ")";
        return false;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    vorbis_ = v;
    channels_ = info.channels;
    rate_ = int(info.sample_rate);
    return true;
}

std::size_t OggStream::Read(std::int16_t* out, std::size_t frames, bool loop) {
    if (vorbis_ == nullptr) return 0;
    std::size_t done = 0;
    bool rewound = false;
    while (done < frames) {
        const int got = stb_vorbis_get_samples_short_interleaved(Vorbis(vorbis_), channels_, out + done * std::size_t(channels_),
                                                                  int((frames - done) * std::size_t(channels_)));
        if (got > 0) {
            done += std::size_t(got);
            rewound = false;
            continue;
        }
        // The end: from the start again (once in a row, so an empty file cannot spin).
        if (!loop || rewound || stb_vorbis_seek_start(Vorbis(vorbis_)) == 0) break;
        rewound = true;
    }
    return done;
}

} // namespace sr::audio
