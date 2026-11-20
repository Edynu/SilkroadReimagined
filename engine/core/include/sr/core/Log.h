#pragma once

// The log: thread-safe, one line per message, to stderr, an optional file, the debugger
// output and the profiler's timeline.
//
//   core::OpenLogFile("SroReimagined.log");
//   core::LogInfo("GPU: {}", name);
//   core::LogError("cannot open {}: {}", path, error);

#include <cstdint>
#include <filesystem>
#include <format>
#include <string_view>
#include <utility>

namespace sr::core {

enum class LogLevel { Debug, Info, Warning, Error };

void Log(LogLevel level, std::string_view message);
// Also writes to this file (truncated). Returns false if it cannot be created.
bool OpenLogFile(const std::filesystem::path& path);
void CloseLogFile();
// Messages below this level are dropped (default: Info).
void SetLogLevel(LogLevel level);
// Errors logged since the start, dropped or not (the tests: "nothing went wrong in between").
std::uint64_t LoggedErrors();

template <typename... Args>
void LogDebug(std::format_string<Args...> format, Args&&... args) {
    Log(LogLevel::Debug, std::format(format, std::forward<Args>(args)...));
}
template <typename... Args>
void LogInfo(std::format_string<Args...> format, Args&&... args) {
    Log(LogLevel::Info, std::format(format, std::forward<Args>(args)...));
}
template <typename... Args>
void LogWarning(std::format_string<Args...> format, Args&&... args) {
    Log(LogLevel::Warning, std::format(format, std::forward<Args>(args)...));
}
template <typename... Args>
void LogError(std::format_string<Args...> format, Args&&... args) {
    Log(LogLevel::Error, std::format(format, std::forward<Args>(args)...));
}

} // namespace sr::core
