#include "sr/core/FramePacer.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <thread>

#include "sr/core/Trace.h"

namespace sr::core {

namespace {
// Below this the spin takes over: timer wake-ups are not that precise.
constexpr auto kSpin = std::chrono::microseconds(700);
} // namespace

FramePacer::FramePacer() {
#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer_ == nullptr) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS); // before Windows 10 1803
#endif
}

FramePacer::~FramePacer() {
#ifdef _WIN32
    if (timer_ != nullptr) CloseHandle(static_cast<HANDLE>(timer_));
#endif
}

void FramePacer::SetLimit(int framesPerSecond) {
    if (framesPerSecond == limit_) return;
    limit_ = framesPerSecond;
    interval_ = framesPerSecond > 0
                    ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / framesPerSecond))
                    : std::chrono::steady_clock::duration{};
    next_ = {};
}

void FramePacer::SleepUntil(std::chrono::steady_clock::time_point when) {
    const auto now = std::chrono::steady_clock::now();
    if (when - now > kSpin) {
#ifdef _WIN32
        if (timer_ != nullptr) {
            const auto ticks100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(when - now - kSpin).count() / 100;
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(ticks100ns); // negative: relative
            if (SetWaitableTimerEx(static_cast<HANDLE>(timer_), &due, 0, nullptr, nullptr, nullptr, 0)) {
                WaitForSingleObject(static_cast<HANDLE>(timer_), INFINITE);
            }
        } else {
            std::this_thread::sleep_for(when - now - kSpin);
        }
#else
        std::this_thread::sleep_for(when - now - kSpin);
#endif
    }
    while (std::chrono::steady_clock::now() < when) std::this_thread::yield();
}

double FramePacer::Wait() {
    if (limit_ <= 0) return 0.0;
    SR_ZONE("pacer: wait");
    const auto now = std::chrono::steady_clock::now();
    if (next_ == std::chrono::steady_clock::time_point{} || now - next_ > interval_) {
        next_ = now + interval_; // first frame, or more than a frame behind: restart the cadence
    } else {
        next_ += interval_;
    }
    SleepUntil(next_);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - now).count();
}

} // namespace sr::core
