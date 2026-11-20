#include "sr/core/Log.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#include "sr/core/Trace.h"

namespace sr::core {

namespace {

struct LogState {
    std::mutex mutex;
    std::FILE* file = nullptr;
    LogLevel minimum = LogLevel::Info;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

LogState& State() {
    static LogState state;
    return state;
}

const char* LevelTag(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warning: return "warning";
    case LogLevel::Error: return "error";
    }
    return "?";
}

} // namespace

namespace {
std::atomic<std::uint64_t> gErrors{0};
} // namespace

std::uint64_t LoggedErrors() { return gErrors.load(); }

void Log(LogLevel level, std::string_view message) {
    if (level == LogLevel::Error) ++gErrors;
    LogState& state = State();
    if (level < state.minimum) return;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start).count();
    const std::string line = std::format("[{:9.3f}] {}: {}\n", seconds, LevelTag(level), message);
    TraceMessage(line.data(), line.size() - 1);
    std::lock_guard lock(state.mutex);
    std::FILE* console = level >= LogLevel::Warning ? stderr : stdout;
    std::fwrite(line.data(), 1, line.size(), console);
    std::fflush(console);
    if (state.file != nullptr) {
        std::fwrite(line.data(), 1, line.size(), state.file);
        std::fflush(state.file);
    }
#ifdef _WIN32
    OutputDebugStringA(line.c_str());
#endif
}

bool OpenLogFile(const std::filesystem::path& path) {
    LogState& state = State();
    std::lock_guard lock(state.mutex);
    if (state.file != nullptr) std::fclose(state.file);
#ifdef _WIN32
    state.file = _wfopen(path.c_str(), L"wb");
#else
    state.file = std::fopen(path.c_str(), "wb");
#endif
    return state.file != nullptr;
}

void CloseLogFile() {
    LogState& state = State();
    std::lock_guard lock(state.mutex);
    if (state.file != nullptr) std::fclose(state.file);
    state.file = nullptr;
}

void SetLogLevel(LogLevel level) {
    State().minimum = level;
}

} // namespace sr::core
