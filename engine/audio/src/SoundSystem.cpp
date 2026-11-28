#include "sr/audio/SoundSystem.h"

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/efx.h>

#include <algorithm>
#include <array>
#include <atomic>

#include "sr/assets/FileSystem.h"
#include "sr/audio/Decode.h"
#include "sr/core/Jobs.h"
#include "sr/core/Log.h"
#include "sr/core/Path.h"
#include "sr/core/Trace.h"

namespace sr::audio {

namespace {

// The music's stream: kStreamBuffers pieces of kStreamFrames frames queued ahead (about 2 s at
// 44,100 Hz), one decoding job at a time refilling the ones played. Our jobs run at Normal
// priority, ahead of the world's Streaming decodes: a click must not wait behind a region loading.
constexpr int kStreamBuffers = 4;
constexpr std::size_t kStreamFrames = 22050;

ALenum Format(int channels) { return channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16; }

ALuint Id(std::uint32_t v) { return static_cast<ALuint>(v); }

// The EFX filter calls, found on the device (ALC_EXT_EFX).
LPALGENFILTERS genFilters = nullptr;
LPALDELETEFILTERS deleteFilters = nullptr;
LPALFILTERI filteri = nullptr;
LPALFILTERF filterf = nullptr;

// The game's left-handed coordinates to OpenAL's right-handed ones.
void Flip(const Vec3& v, ALfloat out[3]) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = -v.z;
}

} // namespace

// A file decoded whole: the job fills `pcm` and sets `done`; the game thread hands it to OpenAL.
struct SoundSystem::Clip {
    struct Task {
        std::atomic<bool> done{false};
        bool ok = false;
        Pcm pcm;
        std::string error;
    };
    enum class State : std::uint8_t { Loading, Ready, Failed } state = State::Loading;
    std::string path;
    std::shared_ptr<Task> task;
    std::uint32_t buffer = 0;
    std::size_t bytes = 0;
    double used = 0.0;
};

struct SoundSystem::Voice {
    std::uint32_t source = 0;
    std::uint32_t filter = 0; // its low-pass filter (EFX; 0: none)
    bool busy = false;
    Channel channel = Channel::Effects;
    float volume = 1.0f;
    std::uint32_t loop = 0; // its loop's id; 0: a one-shot
    double started = 0.0;
    const Clip* clip = nullptr;
};

struct SoundSystem::Pending {
    std::string key;
    Channel channel = Channel::Effects;
    float volume = 1.0f;
    bool positional = false;
    Vec3 position;
    float muffle = 0.0f; // 0 .. 1 (occluded)
    std::uint32_t loop = 0;
    double asked = 0.0;
};

// One music track: its file read and pieces decoded on jobs (one at a time, `busy` while one
// runs), queued on its own source.
struct SoundSystem::Stream {
    struct Task {
        std::atomic<bool> busy{false};
        bool opened = false, failed = false;
        std::string error;
        OggStream ogg;
        int channels = 2, rate = 44100;
        int want = 0;                                   // pieces the next job decodes
        std::vector<std::vector<std::int16_t>> pieces;  // decoded, waiting for a free buffer
    };
    std::string path;
    std::shared_ptr<Task> task;
    std::uint32_t source = 0;
    std::array<std::uint32_t, kStreamBuffers> buffers{};
    std::vector<std::uint32_t> free; // buffers not queued
    float fade = 0.0f, target = 1.0f, speed = 1.0f; // the fade's gain now, where it goes, per second
    bool ended = false;                             // failed, or faded out
    bool waiting = false;                           // starts once every track before it has faded out (silent till then)
};

SoundSystem::SoundSystem(core::JobSystem& jobs, const assets::FileSystem& files)
    : jobs_(jobs), files_(files), work_(std::make_unique<core::JobCounter>()) {}

SoundSystem::~SoundSystem() { Shutdown(); }

bool SoundSystem::Open(std::string& error) { return OpenLoopback(0, error); }

bool SoundSystem::OpenLoopback(int rate, std::string& error) {
    if (!(rate > 0 ? device_.OpenLoopback(rate, error) : device_.Open(error))) return false;
    voices_.resize(kVoices);
    for (Voice& v : voices_) {
        ALuint source = 0;
        alGenSources(1, &source);
        if (alGetError() != AL_NO_ERROR) {
            error = "cannot make the OpenAL sources";
            Shutdown();
            return false;
        }
        v.source = source;
    }
    // The low-pass filters (ours: the muffled sounds behind walls), when the device has EFX.
    efx_ = false;
    if (ALCdevice* dev = alcGetContextsDevice(alcGetCurrentContext()); dev != nullptr && alcIsExtensionPresent(dev, "ALC_EXT_EFX") == ALC_TRUE) {
        genFilters = reinterpret_cast<LPALGENFILTERS>(alGetProcAddress("alGenFilters"));
        deleteFilters = reinterpret_cast<LPALDELETEFILTERS>(alGetProcAddress("alDeleteFilters"));
        filteri = reinterpret_cast<LPALFILTERI>(alGetProcAddress("alFilteri"));
        filterf = reinterpret_cast<LPALFILTERF>(alGetProcAddress("alFilterf"));
        efx_ = genFilters != nullptr && deleteFilters != nullptr && filteri != nullptr && filterf != nullptr;
        for (Voice& v : voices_) {
            if (!efx_) break;
            ALuint filter = 0;
            genFilters(1, &filter);
            filteri(filter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
            filterf(filter, AL_LOWPASS_GAIN, kMuffledGain);
            filterf(filter, AL_LOWPASS_GAINHF, kMuffledGainHf);
            v.filter = filter;
        }
        alGetError();
    }
    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    SetDistances(reference_, farthest_);
    return true;
}

void SoundSystem::Shutdown() {
    jobs_.Wait(*work_); // the jobs read through files_ and must be done before it goes
    if (!device_.IsOpen()) return;
    for (Voice& v : voices_) {
        ALuint source = Id(v.source);
        alSourceStop(source);
        alDeleteSources(1, &source);
        if (v.filter != 0 && deleteFilters != nullptr) {
            ALuint filter = Id(v.filter);
            deleteFilters(1, &filter);
        }
    }
    voices_.clear();
    for (auto& s : streams_) {
        ALuint source = Id(s->source);
        alSourceStop(source);
        alSourcei(source, AL_BUFFER, 0);
        alDeleteSources(1, &source);
        for (const std::uint32_t b : s->buffers) {
            ALuint buffer = Id(b);
            alDeleteBuffers(1, &buffer);
        }
    }
    streams_.clear();
    for (auto& [key, clip] : clips_) {
        ALuint buffer = Id(clip->buffer);
        if (buffer != 0) alDeleteBuffers(1, &buffer);
    }
    clips_.clear();
    pending_.clear();
    musicPath_.clear();
    device_.Close();
}

void SoundSystem::SetGain(Channel channel, float gain) {
    gains_[std::size_t(channel)] = std::clamp(gain, 0.0f, 1.0f);
    if (!device_.IsOpen()) return;
    for (const Voice& v : voices_) {
        if (v.busy && v.channel == channel) alSourcef(Id(v.source), AL_GAIN, VoiceGain(v));
    }
    // The music's sources follow in UpdateStreams.
}

void SoundSystem::SetListener(Vec3 position, Vec3 forward, Vec3 up) {
    if (!device_.IsOpen()) return;
    ALfloat p[3], o[6];
    Flip(position, p);
    Flip(forward, o);
    Flip(up, o + 3);
    alListenerfv(AL_POSITION, p);
    alListenerfv(AL_ORIENTATION, o);
    listener_ = position;
}

void SoundSystem::SetDistances(float reference, float farthest) {
    reference_ = std::max(1.0f, reference);
    farthest_ = std::max(reference_, farthest);
    for (const Voice& v : voices_) {
        alSourcef(Id(v.source), AL_REFERENCE_DISTANCE, reference_);
        alSourcef(Id(v.source), AL_MAX_DISTANCE, farthest_);
    }
}

SoundSystem::Clip& SoundSystem::Request(std::string_view path) {
    std::string key = core::NormalizeAssetPath(path);
    auto it = clips_.find(key);
    if (it == clips_.end()) {
        auto clip = std::make_unique<Clip>();
        clip->path = key;
        clip->task = std::make_shared<Clip::Task>();
        jobs_.Run(
            [task = clip->task, files = &files_, path = key] {
                SR_ZONE("audio: decode");
                if (std::optional<assets::FileData> data = files->Read(path)) {
                    task->ok = DecodeSound(data->Bytes(), task->pcm, task->error);
                } else {
                    task->error = "not in the game files";
                }
                task->done.store(true, std::memory_order_release);
            },
            core::JobPriority::Normal, work_.get());
        it = clips_.emplace(std::move(key), std::move(clip)).first;
    }
    it->second->used = now_;
    return *it->second;
}

void SoundSystem::Preload(std::string_view path) {
    if (device_.IsOpen() && !path.empty()) Request(path);
}

void SoundSystem::Play(std::string_view path, Channel channel, float volume) {
    if (!device_.IsOpen() || path.empty()) return;
    Clip& clip = Request(path);
    Pending p{clip.path, channel, volume, false, {}, 0.0f, 0, now_};
    if (clip.state == Clip::State::Ready) Start(p, clip);
    else if (clip.state == Clip::State::Loading) pending_.push_back(std::move(p));
}

void SoundSystem::PlayAt(std::string_view path, Channel channel, Vec3 position, float volume) {
    if (!device_.IsOpen() || path.empty()) return;
    Clip& clip = Request(path);
    Pending p{clip.path, channel, volume, true, position, 0.0f, 0, now_};
    ++positionalPlayed;
    if (occluded) p.muffle = std::clamp(occluded(listener_, position), 0.0f, 1.0f);
    if (p.muffle > 0.0f) ++muffled;
    if (clip.state == Clip::State::Ready) Start(p, clip);
    else if (clip.state == Clip::State::Loading) pending_.push_back(std::move(p));
}

std::uint32_t SoundSystem::StartLoop(std::string_view path, Channel channel, float volume) {
    if (!device_.IsOpen() || path.empty()) return 0;
    Clip& clip = Request(path);
    const std::uint32_t id = nextLoop_++;
    Pending p{clip.path, channel, volume, false, {}, 0.0f, id, now_};
    if (clip.state == Clip::State::Ready) Start(p, clip);
    else if (clip.state == Clip::State::Loading) pending_.push_back(std::move(p));
    return id;
}

void SoundSystem::StopLoop(std::uint32_t loop) {
    if (loop == 0) return;
    std::erase_if(pending_, [&](const Pending& p) { return p.loop == loop; });
    for (Voice& v : voices_) {
        if (v.busy && v.loop == loop) Release(v);
    }
}

float SoundSystem::VoiceGain(const Voice& v) const { return v.volume * gains_[std::size_t(v.channel)]; }

SoundSystem::Voice* SoundSystem::FreeVoice() {
    Voice* oldest = nullptr;
    for (Voice& v : voices_) {
        if (!v.busy) return &v;
        if (v.loop == 0 && (oldest == nullptr || v.started < oldest->started)) oldest = &v;
    }
    if (oldest != nullptr) Release(*oldest); // every voice busy: the oldest one-shot is cut
    return oldest;
}

void SoundSystem::Release(Voice& v) {
    const ALuint source = Id(v.source);
    alSourceStop(source);
    alSourcei(source, AL_BUFFER, 0);
    v.busy = false;
    v.loop = 0;
    v.clip = nullptr;
}

void SoundSystem::Start(const Pending& p, Clip& clip) {
    Voice* v = FreeVoice();
    if (v == nullptr) return;
    v->busy = true;
    v->channel = p.channel;
    v->volume = std::clamp(p.volume, 0.0f, 1.0f);
    v->loop = p.loop;
    v->started = now_;
    v->clip = &clip;
    clip.used = now_;
    const ALuint source = Id(v->source);
    alSourcei(source, AL_BUFFER, ALint(clip.buffer));
    alSourcei(source, AL_LOOPING, p.loop != 0 ? AL_TRUE : AL_FALSE);
    alSourcef(source, AL_GAIN, VoiceGain(*v));
    if (efx_) {
        if (p.positional && p.muffle > 0.0f && v->filter != 0) {
            filterf(v->filter, AL_LOWPASS_GAIN, 1.0f + (kMuffledGain - 1.0f) * p.muffle);
            filterf(v->filter, AL_LOWPASS_GAINHF, 1.0f + (kMuffledGainHf - 1.0f) * p.muffle);
            alSourcei(source, AL_DIRECT_FILTER, ALint(v->filter));
        } else {
            alSourcei(source, AL_DIRECT_FILTER, AL_FILTER_NULL);
        }
    }
    if (p.positional) {
        ALfloat at[3];
        Flip(p.position, at);
        alSourcei(source, AL_SOURCE_RELATIVE, AL_FALSE);
        alSourcefv(source, AL_POSITION, at);
    } else {
        // At the listener, whatever it does.
        const ALfloat zero[3] = {0, 0, 0};
        alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSourcefv(source, AL_POSITION, zero);
    }
    alSourcePlay(source);
}

void SoundSystem::Update(double dt) {
    if (!device_.IsOpen()) return;
    SR_ZONE("audio: update");
    now_ += std::max(0.0, dt);

    // Clips decoded since the last frame.
    for (auto& [key, clip] : clips_) {
        if (clip->state != Clip::State::Loading || !clip->task->done.load(std::memory_order_acquire)) continue;
        Clip::Task& task = *clip->task;
        if (task.ok && !task.pcm.samples.empty()) {
            ALuint buffer = 0;
            alGenBuffers(1, &buffer);
            alBufferData(buffer, Format(task.pcm.channels), task.pcm.samples.data(), ALsizei(task.pcm.samples.size() * 2), task.pcm.rate);
            if (alGetError() == AL_NO_ERROR) {
                clip->buffer = buffer;
                clip->bytes = task.pcm.samples.size() * 2;
                clip->state = Clip::State::Ready;
            } else {
                alDeleteBuffers(1, &buffer);
                clip->state = Clip::State::Failed;
                core::LogWarning("audio: {}: OpenAL refused its samples", key);
            }
        } else {
            clip->state = Clip::State::Failed; // a file of no samples plays nothing, quietly
            if (!task.ok) core::LogWarning("audio: {}: {}", key, task.error);
        }
        clip->task.reset(); // the samples are OpenAL's now
    }

    // Sounds waiting for their clip: started, or dropped when too late (a loop waits).
    std::erase_if(pending_, [&](const Pending& p) {
        const auto it = clips_.find(p.key);
        if (it == clips_.end() || it->second->state == Clip::State::Failed) return true;
        if (it->second->state == Clip::State::Ready) {
            Start(p, *it->second);
            return true;
        }
        return p.loop == 0 && now_ - p.asked > kLateSeconds;
    });

    // Finished one-shots free their voice.
    for (Voice& v : voices_) {
        if (!v.busy || v.loop != 0) continue;
        ALint state = AL_STOPPED;
        alGetSourcei(Id(v.source), AL_SOURCE_STATE, &state);
        if (state == AL_STOPPED) Release(v);
    }

    UpdateStreams(dt);
    TrimClips();
}

void SoundSystem::TrimClips() {
    std::size_t total = 0;
    for (const auto& [key, clip] : clips_) total += clip->bytes;
    while (total > kClipBudgetBytes) {
        // The least recently used clip no voice plays.
        Clip* victim = nullptr;
        for (auto& [key, clip] : clips_) {
            if (clip->state != Clip::State::Ready) continue;
            const bool playing = std::any_of(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.busy && v.clip == clip.get(); });
            if (!playing && (victim == nullptr || clip->used < victim->used)) victim = clip.get();
        }
        if (victim == nullptr) return;
        total -= victim->bytes;
        ALuint buffer = Id(victim->buffer);
        alDeleteBuffers(1, &buffer);
        const std::string key = victim->path;
        clips_.erase(key);
    }
}

void SoundSystem::PlayMusic(std::string_view path, float fadeSeconds, bool afterFade) {
    if (!device_.IsOpen()) return;
    const std::string key = core::NormalizeAssetPath(path);
    if (key == musicPath_) return;
    musicPath_ = key;
    const float speed = 1.0f / std::max(0.05f, fadeSeconds);
    for (auto& s : streams_) {
        s->target = 0.0f;
        s->speed = speed;
    }
    if (key.empty()) return;

    auto stream = std::make_unique<Stream>();
    stream->path = key;
    stream->speed = speed;
    stream->fade = streams_.empty() ? 1.0f : 0.0f; // the first track starts at once, later ones cross-fade
    // Or, after the fade: silent until the tracks before it are gone, then at full volume.
    if (afterFade && !streams_.empty()) {
        stream->waiting = true;
        stream->fade = 1.0f;
    }
    ALuint source = 0;
    alGenSources(1, &source);
    ALuint buffers[kStreamBuffers] = {};
    alGenBuffers(kStreamBuffers, buffers);
    if (alGetError() != AL_NO_ERROR) {
        alDeleteSources(1, &source);
        alDeleteBuffers(kStreamBuffers, buffers);
        core::LogWarning("audio: cannot make the music's source");
        return;
    }
    stream->source = source;
    for (int i = 0; i < kStreamBuffers; ++i) {
        stream->buffers[std::size_t(i)] = buffers[i];
        stream->free.push_back(buffers[i]);
    }
    alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
    alSourcef(source, AL_GAIN, 0.0f);

    // The file read and the first pieces decoded on a job.
    stream->task = std::make_shared<Stream::Task>();
    stream->task->want = kStreamBuffers;
    stream->task->busy.store(true, std::memory_order_relaxed);
    jobs_.Run(
        [task = stream->task, files = &files_, path = key] {
            SR_ZONE("audio: music open");
            std::optional<assets::FileData> data = files->Read(path);
            if (!data) {
                task->failed = true;
                task->error = "not in the game files";
            } else if (!task->ogg.Open(std::vector<std::byte>(data->Bytes().begin(), data->Bytes().end()), task->error)) {
                task->failed = true;
            } else {
                task->opened = true;
                task->channels = task->ogg.Channels();
                task->rate = task->ogg.Rate();
                for (int i = 0; i < task->want; ++i) {
                    std::vector<std::int16_t> piece(kStreamFrames * std::size_t(task->channels));
                    piece.resize(task->ogg.Read(piece.data(), kStreamFrames, true) * std::size_t(task->channels));
                    if (piece.empty()) break;
                    task->pieces.push_back(std::move(piece));
                }
            }
            task->busy.store(false, std::memory_order_release);
        },
        core::JobPriority::Normal, work_.get());
    streams_.push_back(std::move(stream));
}

void SoundSystem::UpdateStreams(double dt) {
    const float music = gains_[std::size_t(Channel::Music)];
    for (auto& s : streams_) {
        const ALuint source = Id(s->source);
        // A track waiting for the ones before it to fade out: silent, not started, until they are gone.
        if (s->waiting) {
            const bool before = std::any_of(streams_.begin(), streams_.end(), [&](const auto& o) { return o.get() != s.get() && !o->ended && !o->waiting; });
            if (before) {
                alSourcef(source, AL_GAIN, 0.0f);
                continue;
            }
            s->waiting = false;
        }
        // The fade.
        const float step = s->speed * float(dt);
        s->fade = s->fade < s->target ? std::min(s->target, s->fade + step) : std::max(s->target, s->fade - step);
        alSourcef(source, AL_GAIN, s->fade * music);
        if (s->target == 0.0f && s->fade == 0.0f) {
            s->ended = true;
            continue;
        }
        // Played pieces back to the free list.
        ALint processed = 0;
        alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
        while (processed-- > 0) {
            ALuint buffer = 0;
            alSourceUnqueueBuffers(source, 1, &buffer);
            s->free.push_back(buffer);
        }
        Stream::Task& task = *s->task;
        if (task.busy.load(std::memory_order_acquire)) continue;
        if (task.failed) {
            core::LogWarning("audio: music {}: {}", s->path, task.error);
            s->ended = true;
            continue;
        }
        // Decoded pieces into free buffers, queued.
        while (!task.pieces.empty() && !s->free.empty()) {
            const std::vector<std::int16_t>& piece = task.pieces.front();
            const ALuint buffer = s->free.back();
            alBufferData(buffer, Format(task.channels), piece.data(), ALsizei(piece.size() * 2), task.rate);
            alSourceQueueBuffers(source, 1, &buffer);
            s->free.pop_back();
            task.pieces.erase(task.pieces.begin());
        }
        // Playing (again, after running dry).
        ALint state = AL_STOPPED, queued = 0;
        alGetSourcei(source, AL_SOURCE_STATE, &state);
        alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
        if (state != AL_PLAYING && queued > 0) alSourcePlay(source);
        // The next pieces for the buffers played.
        if (!s->free.empty() && task.opened) {
            task.want = int(s->free.size());
            task.busy.store(true, std::memory_order_relaxed);
            jobs_.Run(
                [t = s->task] {
                    SR_ZONE("audio: music decode");
                    for (int i = 0; i < t->want; ++i) {
                        std::vector<std::int16_t> piece(kStreamFrames * std::size_t(t->channels));
                        piece.resize(t->ogg.Read(piece.data(), kStreamFrames, true) * std::size_t(t->channels));
                        if (piece.empty()) break;
                        t->pieces.push_back(std::move(piece));
                    }
                    t->busy.store(false, std::memory_order_release);
                },
                core::JobPriority::Normal, work_.get());
        }
    }
    // Tracks faded out (or failed) go; a job still decoding for one keeps only its task alive.
    std::erase_if(streams_, [](const std::unique_ptr<Stream>& s) {
        if (!s->ended) return false;
        ALuint source = Id(s->source);
        alSourceStop(source);
        alSourcei(source, AL_BUFFER, 0);
        alDeleteSources(1, &source);
        for (const std::uint32_t b : s->buffers) {
            ALuint buffer = Id(b);
            alDeleteBuffers(1, &buffer);
        }
        return true;
    });
}

SoundSystem::Stats SoundSystem::GetStats() const {
    Stats s;
    for (const Voice& v : voices_) s.voicesBusy += v.busy ? 1 : 0;
    for (const auto& [key, clip] : clips_) {
        ++s.clips;
        s.clipBytes += clip->bytes;
        s.decoding += clip->state == Clip::State::Loading ? 1 : 0;
    }
    s.musicStreams = int(streams_.size());
    return s;
}

} // namespace sr::audio
