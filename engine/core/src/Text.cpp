#include "sr/core/Text.h"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sr::core {

std::string Cp949ToUtf8(std::string_view text) {
    const bool ascii = std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    if (ascii || text.empty()) {
        return std::string(text);
    }
#ifdef _WIN32
    const int wideLen = ::MultiByteToWideChar(949, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wideLen <= 0) return std::string(text);
    std::wstring wide(static_cast<std::size_t>(wideLen), L'\0');
    ::MultiByteToWideChar(949, 0, text.data(), static_cast<int>(text.size()), wide.data(), wideLen);
    const int utf8Len = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(utf8Len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, out.data(), utf8Len, nullptr, nullptr);
    return out;
#else
    return std::string(text);
#endif
}

void AppendUtf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

std::string DecodeTextFile(std::span<const std::byte> bytes) {
    const auto at = [&](std::size_t i) { return static_cast<unsigned char>(bytes[i]); };
    if (bytes.size() >= 2 && at(0) == 0xFF && at(1) == 0xFE) {
        std::string out;
        out.reserve(bytes.size() / 2);
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
            char32_t unit = static_cast<char32_t>(at(i) | at(i + 1) << 8);
            if (unit >= 0xD800 && unit < 0xDC00 && i + 3 < bytes.size()) {
                const char32_t low = static_cast<char32_t>(at(i + 2) | at(i + 3) << 8);
                if (low >= 0xDC00 && low < 0xE000) {
                    unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                }
            }
            AppendUtf8(out, unit);
        }
        return out;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (bytes.size() >= 3 && at(0) == 0xEF && at(1) == 0xBB && at(2) == 0xBF) return std::string(text.substr(3));
    return Cp949ToUtf8(text);
}

} // namespace sr::core
