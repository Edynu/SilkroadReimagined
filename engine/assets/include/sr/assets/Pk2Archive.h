#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sr/core/MappedFile.h"

namespace sr::assets {

// Read-only access to a JoyMax PK2 container (Data.pk2, Map.pk2, ...).
//
// Format (little-endian, packed):
//   header  256 B : signature[30] version:u32 encrypted:u8 verify[16] reserved[205]
//   entry   128 B : type:u8 name[81] times:3*u64 position:i64 size:u32 nextChain:i64 pad[2]
//   block  2560 B : 20 entries; a directory is a chain of blocks linked through
//                   entry[19].nextChain. The root directory block sits at offset 256.
// Only directory blocks are Blowfish-encrypted; file payloads are stored as-is.
//
// Open() maps the archive and indexes every file up front, after which the
// object is immutable: Find() and Read() are safe from any number of threads.
class Pk2Archive {
public:
    struct File {
        std::string path;    // as stored, '\'-separated, original case
        std::uint64_t offset = 0;
        std::uint32_t size = 0;
    };

    // ASCII key shared by all official Silkroad Online clients.
    static constexpr std::string_view kDefaultKey = "169841";

    bool Open(const std::filesystem::path& path, std::string& error, std::string_view asciiKey = kDefaultKey);

    // Case- and separator-insensitive lookup; nullptr when absent.
    const File* Find(std::string_view path) const;

    // Zero-copy view into the mapped archive, valid while the archive lives.
    std::span<const std::byte> Read(const File& file) const;

    const std::vector<File>& Files() const { return files_; }
    std::size_t FolderCount() const { return folderCount_; }
    const std::filesystem::path& Path() const { return path_; }
    // True when the header's verify field matched the key (informational).
    bool KeyVerifiedByHeader() const { return keyVerifiedByHeader_; }

private:
    std::filesystem::path path_;
    core::MappedFile mapping_;
    std::vector<File> files_;
    std::unordered_map<std::string, std::uint32_t> index_; // normalized path -> files_ index
    std::size_t folderCount_ = 0;
    bool keyVerifiedByHeader_ = false;
};

} // namespace sr::assets
