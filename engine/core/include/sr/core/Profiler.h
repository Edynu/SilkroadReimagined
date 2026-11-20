#pragma once

// Per-frame CPU timings and counters for the developer tools' Performance panel. Names are
// string literals; entries keep the order they were first seen in. Main thread only (jobs
// and other threads use SR_ZONE from Trace.h).
//
//   SR_PROFILE("world: cull");            // times the rest of the scope (and a Tracy zone)
//   core::Profiler::Get().Count("objects drawn", n);

#include <chrono>
#include <string_view>
#include <vector>

#include "sr/core/Trace.h"

namespace sr::core {

class Profiler {
public:
    struct Entry {
        const char* name = "";
        double value = 0.0; // ms for timings
        bool time = true;
    };

    static Profiler& Get();
    // Ends the current frame (its entries become Last()) and starts the next one.
    void NextFrame();
    void AddTime(const char* name, double ms) { Slot(name, true).value += ms; }
    void Count(const char* name, double value) { Slot(name, false).value += value; }
    // The last finished frame.
    const std::vector<Entry>& Last() const { return last_; }

private:
    Entry& Slot(const char* name, bool time);
    std::vector<Entry> current_, last_;
};

class ScopedTimer {
public:
    explicit ScopedTimer(const char* name) : name_(name), start_(std::chrono::steady_clock::now()) {}
    ~ScopedTimer() {
        Profiler::Get().AddTime(name_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count());
    }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    const char* name_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace sr::core

#define SR_PROFILE_CONCAT2(a, b) a##b
#define SR_PROFILE_CONCAT(a, b) SR_PROFILE_CONCAT2(a, b)
#define SR_PROFILE(name)                                                    \
    ::sr::core::ScopedTimer SR_PROFILE_CONCAT(srProfile_, __LINE__)(name);  \
    SR_ZONE(name)
