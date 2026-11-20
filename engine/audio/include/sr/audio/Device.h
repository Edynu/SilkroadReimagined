#pragma once

// The audio output, through OpenAL Soft. OpenAL Soft mixes on its
// own thread; the rest of the engine only queues buffers and moves sources. Only this module
// sees OpenAL.

#include <cstdint>
#include <span>
#include <string>

namespace sr::audio {

class Device {
public:
    Device() = default;
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // Opens the system's default output and makes its context current.
    bool Open(std::string& error);
    // Opens a loopback device instead (ALC_SOFT_loopback): nothing plays; the mix is asked for with Render, stereo
    // 16-bit at `rate` (the skill bench's videos: their sound in step with their frames).
    bool OpenLoopback(int rate, std::string& error);
    bool Loopback() const { return loopback_; }
    int Rate() const { return rate_; }
    // The next `frames` stereo frames of the mix into `out` (2 x frames samples); loopback only.
    void Render(std::span<std::int16_t> out);
    void Close();
    bool IsOpen() const { return device_ != nullptr; }

    // The output device's name and the OpenAL implementation ("OpenAL Soft 1.25.2" ...).
    const std::string& DeviceName() const { return deviceName_; }
    const std::string& Implementation() const { return implementation_; }

private:
    void* device_ = nullptr;  // ALCdevice*
    void* context_ = nullptr; // ALCcontext*
    void* render_ = nullptr;  // alcRenderSamplesSOFT
    bool loopback_ = false;
    int rate_ = 0;
    std::string deviceName_;
    std::string implementation_;
};

} // namespace sr::audio
