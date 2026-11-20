#include "sr/audio/Device.h"

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

namespace sr::audio {

Device::~Device() {
    Close();
}

bool Device::Open(std::string& error) {
    Close();
    ALCdevice* device = alcOpenDevice(nullptr);
    if (device == nullptr) {
        error = "no audio output device";
        return false;
    }
    ALCcontext* context = alcCreateContext(device, nullptr);
    if (context == nullptr || alcMakeContextCurrent(context) != ALC_TRUE) {
        if (context != nullptr) alcDestroyContext(context);
        alcCloseDevice(device);
        error = "cannot create an OpenAL context";
        return false;
    }
    device_ = device;
    context_ = context;

    const ALCenum nameQuery =
        alcIsExtensionPresent(device, "ALC_ENUMERATE_ALL_EXT") == ALC_TRUE ? ALC_ALL_DEVICES_SPECIFIER : ALC_DEVICE_SPECIFIER;
    if (const ALCchar* name = alcGetString(device, nameQuery)) deviceName_ = name;
    const ALchar* renderer = alGetString(AL_RENDERER);
    const ALchar* version = alGetString(AL_VERSION);
    implementation_ = std::string(renderer != nullptr ? renderer : "OpenAL") + " " + (version != nullptr ? version : "");
    return true;
}

bool Device::OpenLoopback(int rate, std::string& error) {
    Close();
    if (alcIsExtensionPresent(nullptr, "ALC_SOFT_loopback") != ALC_TRUE) {
        error = "OpenAL has no loopback (ALC_SOFT_loopback)";
        return false;
    }
    const auto open = reinterpret_cast<LPALCLOOPBACKOPENDEVICESOFT>(alcGetProcAddress(nullptr, "alcLoopbackOpenDeviceSOFT"));
    const auto render = reinterpret_cast<LPALCRENDERSAMPLESSOFT>(alcGetProcAddress(nullptr, "alcRenderSamplesSOFT"));
    ALCdevice* device = open != nullptr ? open(nullptr) : nullptr;
    if (device == nullptr || render == nullptr) {
        error = "cannot open an OpenAL loopback device";
        return false;
    }
    const ALCint attributes[] = {ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT, ALC_FORMAT_TYPE_SOFT, ALC_SHORT_SOFT, ALC_FREQUENCY, rate, 0};
    ALCcontext* context = alcCreateContext(device, attributes);
    if (context == nullptr || alcMakeContextCurrent(context) != ALC_TRUE) {
        if (context != nullptr) alcDestroyContext(context);
        alcCloseDevice(device);
        error = "cannot create an OpenAL loopback context";
        return false;
    }
    device_ = device;
    context_ = context;
    render_ = reinterpret_cast<void*>(render);
    loopback_ = true;
    rate_ = rate;
    deviceName_ = "loopback";
    const ALchar* renderer = alGetString(AL_RENDERER);
    const ALchar* version = alGetString(AL_VERSION);
    implementation_ = std::string(renderer != nullptr ? renderer : "OpenAL") + " " + (version != nullptr ? version : "");
    return true;
}

void Device::Render(std::span<std::int16_t> out) {
    if (!loopback_ || render_ == nullptr || out.size() < 2) return;
    reinterpret_cast<LPALCRENDERSAMPLESSOFT>(render_)(static_cast<ALCdevice*>(device_), out.data(), static_cast<ALCsizei>(out.size() / 2));
}

void Device::Close() {
    if (context_ != nullptr) {
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(static_cast<ALCcontext*>(context_));
        context_ = nullptr;
    }
    if (device_ != nullptr) {
        alcCloseDevice(static_cast<ALCdevice*>(device_));
        device_ = nullptr;
    }
    deviceName_.clear();
    implementation_.clear();
    render_ = nullptr;
    loopback_ = false;
    rate_ = 0;
}

} // namespace sr::audio
