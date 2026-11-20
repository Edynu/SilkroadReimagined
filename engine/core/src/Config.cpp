#include "sr/core/Config.h"

#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>

namespace sr::core {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

bool Config::LoadFile(const std::filesystem::path& path, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot read " + path.string();
        return false;
    }
    std::ostringstream text;
    text << file.rdbuf();
    if (!ParseText(text.str(), error)) {
        error = path.string() + ": " + error;
        return false;
    }
    return true;
}

bool Config::ParseText(std::string_view text, std::string& error) {
    int lineNumber = 0;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        ++lineNumber;
        line = Trim(line);
        if (line.empty() || line.front() == '#') continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            error = "line " + std::to_string(lineNumber) + ": expected key = value";
            return false;
        }
        const std::string_view key = Trim(line.substr(0, equals));
        if (key.empty()) {
            error = "line " + std::to_string(lineNumber) + ": empty key";
            return false;
        }
        Set(std::string(key), std::string(Trim(line.substr(equals + 1))));
    }
    return true;
}

bool Config::ParseCommandLine(int argc, const char* const* argv, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.size() < 3 || arg.substr(0, 2) != "--") {
            error = "unexpected argument: " + std::string(arg);
            return false;
        }
        std::string key(arg.substr(2));
        const bool hasValue = i + 1 < argc && std::string_view(argv[i + 1]).substr(0, 2) != "--";
        Set(std::move(key), hasValue ? std::string(argv[++i]) : std::string("true"));
    }
    return true;
}

std::string Config::GetString(std::string_view key, std::string_view fallback) const {
    const auto it = values_.find(key);
    return it != values_.end() ? it->second : std::string(fallback);
}

int Config::GetInt(std::string_view key, int fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    int value = 0;
    const std::string& s = it->second;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    return ec == std::errc() && ptr == s.data() + s.size() ? value : fallback;
}

double Config::GetDouble(std::string_view key, double fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    double value = 0.0;
    const std::string& s = it->second;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    return ec == std::errc() && ptr == s.data() + s.size() ? value : fallback;
}

bool Config::GetBool(std::string_view key, bool fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    const std::string v = Lower(it->second);
    if (v == "true" || v == "yes" || v == "on" || v == "1") return true;
    if (v == "false" || v == "no" || v == "off" || v == "0") return false;
    return fallback;
}

} // namespace sr::core
