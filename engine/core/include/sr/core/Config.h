#pragma once

// Engine settings (not the player's game options, see Options.h): `key = value` lines from a
// file, then the command line on top (`--key value`, or `--flag` alone for true). Keys are
// case-sensitive; '#' starts a comment line. Unknown keys are kept, so tools can read them.
//
//   game = E:\Silkroad\VSRO_Client
//   workers = 0
//   simulation-rate = 30

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace sr::core {

class Config {
public:
    // A missing file is not an error (returns true, nothing set).
    bool LoadFile(const std::filesystem::path& path, std::string& error);
    // Parses argv[1..]: "--key value" or "--flag" (true when the next argument is another
    // option or there is none). A bare argument without "--" is an error.
    bool ParseCommandLine(int argc, const char* const* argv, std::string& error);
    // Parses `key = value` lines (what LoadFile reads).
    bool ParseText(std::string_view text, std::string& error);

    void Set(std::string key, std::string value) { values_[std::move(key)] = std::move(value); }
    bool Has(std::string_view key) const { return values_.find(key) != values_.end(); }

    std::string GetString(std::string_view key, std::string_view fallback = {}) const;
    // A missing or unparsable value gives the fallback.
    int GetInt(std::string_view key, int fallback) const;
    double GetDouble(std::string_view key, double fallback) const;
    // true / yes / on / 1 and false / no / off / 0 (any case).
    bool GetBool(std::string_view key, bool fallback) const;

    const std::map<std::string, std::string, std::less<>>& Values() const { return values_; }

private:
    std::map<std::string, std::string, std::less<>> values_;
};

} // namespace sr::core
