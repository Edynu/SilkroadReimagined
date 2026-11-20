#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace sr::core {

// Converts CP949 (Korean, the original game's code page) to UTF-8. ASCII
// passes through unchanged. On non-Windows builds the input is returned as-is.
std::string Cp949ToUtf8(std::string_view text);

// Appends one Unicode code point as UTF-8.
void AppendUtf8(std::string& out, char32_t codePoint);

// Decodes a game text file to UTF-8 by its byte-order mark: FF FE = UTF-16LE
// (most textdata and resinfo files), EF BB BF = UTF-8, none = CP949.
std::string DecodeTextFile(std::span<const std::byte> bytes);

} // namespace sr::core
