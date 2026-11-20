#pragma once

#include <string>
#include <string_view>

namespace sr::core {

// Asset paths are byte strings (game file names are ASCII or CP949), always
// backslash-separated, relative and case-insensitive, like `res\char\foo.bsr`.
//
// NormalizeAssetPath turns any spelling into the lookup key: '/' becomes '\',
// repeated separators collapse, leading/trailing separators and "." segments
// are dropped, and ASCII letters are lowercased. Bytes >= 0x80 are left alone
// so CP949 names are not mangled; the same rule is applied to stored names and
// queries, so lookups stay consistent either way.
std::string NormalizeAssetPath(std::string_view path);

} // namespace sr::core
