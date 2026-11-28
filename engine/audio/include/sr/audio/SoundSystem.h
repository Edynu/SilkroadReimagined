#pragma once

// The sound engine over OpenAL Soft. One thread drives it
// (the game thread: every call and Update); OpenAL Soft mixes on its own thread. Files are read and
// decoded on the job system, never on the calling thread:
//
//  - clips: a sound file decoded whole the first time it is asked for, kept by path in one OpenAL
//    buffer (the least recently used ones go when the cache passes its budget);
//  - voices: a fixed pool of sources; a sound asked for while its clip is still decoding starts when
//    it is in, or not at all after kLateSeconds (a click heard late is worse than none); with every
//    voice busy, the oldest one-shot is cut;
//  - three channels with live gains (music, effects, environment: the Options window's levels);
//  - 2D sounds (at the listener) and 3D sounds at a place (inverse distance, clamped);
//  - music: one track at a time streamed from its Ogg file (decoded a piece ahead on jobs, looping),
//    cross-faded with the one before.
//
// Positions are the game's (left-handed: x east, y up, z north); OpenAL's are right-handed, so z
// flips on the way in.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sr/audio/Device.h"

namespace sr::core {
class JobSystem;
class JobCounter;
} // namespace sr::core
namespace sr::assets {
class FileSystem;
}

namespace sr::audio {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

enum class Channel : std::uint8_t { Music = 0, Effects = 1, Environment = 2 };
inline constexpr int kChannelCount = 3;

class SoundSystem {
public:
    static constexpr int kVoices = 48;
    static constexpr double kLateSeconds = 0.5;
    static constexpr std::size_t kClipBudgetBytes = 96u << 20;

    SoundSystem(core::JobSystem& jobs, const assets::FileSystem& files);
    ~SoundSystem();
    SoundSystem(const SoundSystem&) = delete;
    SoundSystem& operator=(const SoundSystem&) = delete;

    // The system's default output and the voices. False: no sound (every call is then ignored).
    bool Open(std::string& error);
    // The same on a loopback device at `rate` (Device::OpenLoopback): RenderLoopback gives its mix.
    bool OpenLoopback(int rate, std::string& error);
    void RenderLoopback(std::span<std::int16_t> out) { device_.Render(out); }
    // Waits for its jobs, stops everything and closes the device.
    void Shutdown();
    bool IsOpen() const { return device_.IsOpen(); }
    const Device& device() const { return device_; }

    // A channel's gain, 0..1 (applied at once to what plays on it).
    void SetGain(Channel channel, float gain);
    float Gain(Channel channel) const { return gains_[std::size_t(channel)]; }

    // The ear: where it is, where it looks and which way is up (the camera).
    void SetListener(Vec3 position, Vec3 forward, Vec3 up);
    // 3D sounds: full volume within `reference` of the listener, falling off as reference / distance beyond it (the
    // original's DirectSound inverse falloff; its buffers never set their distances, so they keep falling: our
    // `farthest` far away), constant past `farthest`. The reference is ours (the original's unit not found).
    void SetDistances(float reference, float farthest);
    // Ours, experimental: asked when a positional sound starts (listener, sound): how much the world stands between them,
    // 0 nothing .. 1 wholly; such a sound plays muffled that much (a low-pass filter towards kMuffledGain,
    // kMuffledGainHf). Null: never.
    std::function<float(Vec3 listener, Vec3 sound)> occluded;
    static constexpr float kMuffledGain = 0.6f, kMuffledGainHf = 0.12f;
    // The positional sounds playing are asked again this often (their muffle eased toward the answer: a long sound opens
    // and closes as the listener walks round a wall).
    static constexpr double kOcclusionRefresh = 0.1;
    // Ours, experimental: the room's echo, one reverb every positional sound is sent to. `amount` 0 (none: the open) .. 1,
    // `decay` its length in seconds; eased toward over a quarter second.
    void SetReverb(float amount, float decay);
    // Sounds muffled and played since the start (the tests' and the log's).
    int muffled = 0, positionalPlayed = 0;

    // A file (a path in the file system) once, at the listener or at a place; `volume` 0..1 on top of
    // the channel's gain.
    void Play(std::string_view path, Channel channel, float volume = 1.0f);
    void PlayAt(std::string_view path, Channel channel, Vec3 position, float volume = 1.0f);
    // A file looping at the listener until stopped (ambience); 0 when the device is closed.
    std::uint32_t StartLoop(std::string_view path, Channel channel, float volume = 1.0f);
    void StopLoop(std::uint32_t loop);
    // The file decoded now (on a job), so a Play later starts at once.
    void Preload(std::string_view path);

    // The music: `path` streamed and looping, faded in over `fadeSeconds` while the track before
    // fades out over the same time; the same path again does nothing; empty: fade to silence. `afterFade`: the track
    // before fades out first and the new one starts after it at full volume (the original's way).
    void PlayMusic(std::string_view path, float fadeSeconds = 2.0f, bool afterFade = false);
    const std::string& Music() const { return musicPath_; }

    // Once a frame: clips decoded since are handed to OpenAL, waiting sounds start, finished voices
    // free, the music is refilled and faded, the cache trimmed.
    void Update(double dt);

    struct Stats {
        int voicesBusy = 0;
        int clips = 0;
        int decoding = 0;
        std::size_t clipBytes = 0;
        int musicStreams = 0;
    };
    Stats GetStats() const;

private:
    struct Clip;
    struct Voice;
    struct Pending;
    struct Stream;

    Clip& Request(std::string_view path);
    void Start(const Pending& p, Clip& clip);
    Voice* FreeVoice();
    void Release(Voice& v);
    float VoiceGain(const Voice& v) const;
    void UpdateStreams(double dt);
    void TrimClips();

    core::JobSystem& jobs_;
    const assets::FileSystem& files_;
    std::unique_ptr<core::JobCounter> work_; // every job of ours, waited for at Shutdown
    Device device_;
    double now_ = 0.0;
    float gains_[kChannelCount] = {1.0f, 1.0f, 1.0f};
    float reference_ = 60.0f, farthest_ = 1.0e6f;
    Vec3 listener_{};
    bool efx_ = false; // the device has ALC_EXT_EFX: each voice has a low-pass filter
    double occlusionAt_ = 0.0;
    std::uint32_t reverbEffect_ = 0, reverbSlot_ = 0; // the room's echo (EFX; 0: none)
    float reverbAmount_ = 0.0f, reverbDecay_ = 1.0f;   // as applied
    float reverbWantAmount_ = 0.0f, reverbWantDecay_ = 1.0f;
    double reverbAt_ = 0.0;
    void ApplyReverb();
    void Muffle(Voice& v, float muffle);
    std::unordered_map<std::string, std::unique_ptr<Clip>> clips_;
    std::vector<Voice> voices_;
    std::vector<Pending> pending_;
    std::uint32_t nextLoop_ = 1;
    std::vector<std::unique_ptr<Stream>> streams_; // the last one is the current track
    std::string musicPath_;
};

} // namespace sr::audio
