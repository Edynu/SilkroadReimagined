#include "frame/VideoWriter.h"

#include <algorithm>
#include <format>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "sr/core/Log.h"

namespace sr::client {

#ifdef _WIN32

namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

} // namespace

namespace {

void PutU32(std::ofstream& out, std::uint32_t v) {
    const char b[4] = {char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF), char(v >> 24)};
    out.write(b, 4);
}
void PutU16(std::ofstream& out, std::uint16_t v) {
    const char b[2] = {char(v & 0xFF), char(v >> 8)};
    out.write(b, 2);
}
// A 16-bit stereo WAV header for `bytes` of samples.
void WavHeader(std::ofstream& out, int rate, std::uint64_t bytes) {
    const std::uint32_t data = std::uint32_t(std::min<std::uint64_t>(bytes, 0xFFFFFFF0u));
    out.write("RIFF", 4);
    PutU32(out, 36 + data);
    out.write("WAVEfmt ", 8);
    PutU32(out, 16);
    PutU16(out, 1); // PCM
    PutU16(out, 2);
    PutU32(out, std::uint32_t(rate));
    PutU32(out, std::uint32_t(rate) * 4);
    PutU16(out, 4);
    PutU16(out, 16);
    out.write("data", 4);
    PutU32(out, data);
}

// Runs a command line and waits for it (true: it exited 0).
bool RunAndWait(const std::string& command) {
    std::wstring line = Widen(command);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, 300'000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code == 0;
}

} // namespace

bool VideoWriter::Open(const std::filesystem::path& file, const std::string& ffmpeg, std::uint32_t width, std::uint32_t height, int fps,
                       std::uint32_t maxWidth, std::string& error, int audioRate) {
    Close();
    if (width == 0 || height == 0 || fps <= 0) {
        error = "the video needs a size and a rate";
        return false;
    }
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    // Raw RGBA frames in; H.264 out (yuv420p for browsers, the moov atom first so a page plays it while it loads).
    std::string scale;
    if (maxWidth > 0 && width > maxWidth) scale = std::format(" -vf scale={}:-2:flags=bicubic", maxWidth);
    else if (width % 2 != 0 || height % 2 != 0) scale = " -vf scale=trunc(iw/2)*2:trunc(ih/2)*2";
    const std::string command = std::format("\"{}\" -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s {}x{} -r {} -i - "
                                            "-an -c:v libx264 -preset veryfast -crf 24 -pix_fmt yuv420p -movflags +faststart{} \"{}\"",
                                            ffmpeg, width, height, fps, scale, file.string());
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 1 << 22)) {
        error = std::format("no pipe for the encoder ({})", GetLastError());
        return false;
    }
    SetHandleInformation(write, HANDLE_FLAG_INHERIT, 0); // ours alone: the encoder sees its input end when we close it
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = read;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring line = Widen(command);
    const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(read);
    if (!started) {
        error = std::format("ffmpeg did not start ({}): {}", GetLastError(), ffmpeg);
        CloseHandle(write);
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    input_ = write;
    file_ = file;
    ffmpeg_ = ffmpeg;
    width_ = width;
    height_ = height;
    frames_ = 0;
    audioRate_ = audioRate;
    audioBytes_ = 0;
    if (audioRate_ > 0) {
        std::filesystem::path wav = file;
        wav += ".wav";
        wav_.open(wav, std::ios::binary | std::ios::trunc);
        if (wav_) WavHeader(wav_, audioRate_, 0);
    }
    return true;
}

void VideoWriter::AddAudio(std::span<const std::int16_t> samples) {
    if (!wav_ || samples.empty()) return;
    wav_.write(reinterpret_cast<const char*>(samples.data()), std::streamsize(samples.size_bytes()));
    audioBytes_ += samples.size_bytes();
}

bool VideoWriter::Mux() {
    std::filesystem::path wav = file_, muxed = file_;
    wav += ".wav";
    muxed += ".mux.mp4";
    std::error_code ec;
    const std::string command = std::format("\"{}\" -hide_banner -loglevel error -y -i \"{}\" -i \"{}\" -map 0:v:0 -map 1:a:0 -c:v copy -c:a aac -b:a 160k "
                                            "-shortest -movflags +faststart \"{}\"",
                                            ffmpeg_, file_.string(), wav.string(), muxed.string());
    const bool ok = RunAndWait(command);
    if (ok) {
        std::filesystem::rename(muxed, file_, ec);
    } else {
        std::filesystem::remove(muxed, ec);
        core::LogWarning("video: its sound was not put into {}", file_.string());
    }
    std::filesystem::remove(wav, ec);
    return ok && !ec;
}

bool VideoWriter::Write(std::span<const std::uint8_t> rgba) {
    if (input_ == nullptr || rgba.size() != std::size_t(width_) * height_ * 4) return false;
    std::size_t done = 0;
    while (done < rgba.size()) {
        DWORD wrote = 0;
        const DWORD chunk = DWORD(std::min<std::size_t>(rgba.size() - done, 1u << 24));
        if (!WriteFile(static_cast<HANDLE>(input_), rgba.data() + done, chunk, &wrote, nullptr) || wrote == 0) return false;
        done += wrote;
    }
    ++frames_;
    return true;
}

bool VideoWriter::Close() {
    if (process_ == nullptr) return false;
    CloseHandle(static_cast<HANDLE>(input_));
    input_ = nullptr;
    WaitForSingleObject(static_cast<HANDLE>(process_), 120'000);
    DWORD code = 1;
    GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
    if (code != 0) core::LogWarning("video: ffmpeg ended with {} on {}", code, file_.string());
    // Its sound: the WAV's header made whole, then put into the video.
    if (wav_.is_open()) {
        wav_.seekp(0);
        WavHeader(wav_, audioRate_, audioBytes_);
        wav_.close();
        if (code == 0 && audioBytes_ > 0) {
            Mux();
        } else {
            std::filesystem::path wav = file_;
            wav += ".wav";
            std::error_code ec;
            std::filesystem::remove(wav, ec);
        }
    }
    return code == 0;
}

#else

bool VideoWriter::Open(const std::filesystem::path&, const std::string&, std::uint32_t, std::uint32_t, int, std::uint32_t, std::string& error, int) {
    error = "videos are written on Windows only";
    return false;
}
bool VideoWriter::Write(std::span<const std::uint8_t>) { return false; }
void VideoWriter::AddAudio(std::span<const std::int16_t>) {}
bool VideoWriter::Mux() { return false; }
bool VideoWriter::Close() { return false; }

#endif

} // namespace sr::client
