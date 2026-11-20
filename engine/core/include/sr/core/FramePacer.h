#pragma once

// Holds a frame rate limit precisely (pacing happens on the render
// thread). Wait() sleeps on a high-resolution timer until shortly before the next frame is due,
// then spins the last fraction of a millisecond, so 144 FPS means 6.94 ms frames, not 7 or 8.

#include <chrono>
#include <cstddef>

namespace sr::core {

class FramePacer {
public:
    FramePacer();
    ~FramePacer();
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    // Frames per second; 0 or less: no limit (Wait returns at once).
    void SetLimit(int framesPerSecond);
    int Limit() const { return limit_; }
    // Call once per frame, after presenting. Returns the seconds it slept.
    double Wait();

private:
    void SleepUntil(std::chrono::steady_clock::time_point when);

    int limit_ = 0;
    std::chrono::steady_clock::duration interval_{};
    std::chrono::steady_clock::time_point next_{};
    void* timer_ = nullptr; // a high-resolution waitable timer (Windows)
};

// 64-bit FNV-1a, for state hashes (determinism checks).
class Hash64 {
public:
    void Bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) value_ = (value_ ^ p[i]) * 0x100000001b3ull;
    }
    template <typename T>
    void Value(const T& v) {
        Bytes(&v, sizeof(T));
    }
    unsigned long long Result() const { return value_; }

private:
    unsigned long long value_ = 0xcbf29ce484222325ull;
};

} // namespace sr::core
