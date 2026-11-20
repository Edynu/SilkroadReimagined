#include "sr/core/Path.h"

namespace sr::core {

std::string NormalizeAssetPath(std::string_view path) {
    std::string out;
    out.reserve(path.size());

    std::size_t segmentStart = 0;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        const bool atEnd = i == path.size();
        const char c = atEnd ? '\\' : path[i];
        if (c != '\\' && c != '/') {
            continue;
        }

        std::string_view segment = path.substr(segmentStart, i - segmentStart);
        segmentStart = i + 1;
        if (segment.empty() || segment == ".") {
            continue;
        }

        if (!out.empty()) {
            out.push_back('\\');
        }
        for (char ch : segment) {
            out.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
        }
    }
    return out;
}

} // namespace sr::core
