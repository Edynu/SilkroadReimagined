#include "sr/assets/Pk2Archive.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <unordered_set>
#include <utility>

#include "sr/assets/Blowfish.h"
#include "sr/core/Path.h"

static_assert(std::endian::native == std::endian::little, "PK2 parsing assumes a little-endian host");

namespace sr::assets {

namespace {

constexpr std::size_t kHeaderSize = 256;
constexpr std::size_t kEntrySize = 128;
constexpr std::size_t kEntriesPerBlock = 20;
constexpr std::size_t kBlockSize = kEntrySize * kEntriesPerBlock;
constexpr std::uint64_t kRootOffset = kHeaderSize;
constexpr std::uint32_t kVersion = 0x01000002;

constexpr std::uint8_t kTypeEmpty = 0;
constexpr std::uint8_t kTypeFolder = 1;
constexpr std::uint8_t kTypeFile = 2;

// The ASCII key is XORed with this before it becomes the Blowfish key.
constexpr std::array<std::uint8_t, 10> kKeyMask = {0x03, 0xF8, 0xE4, 0x44, 0x88, 0x99, 0x3F, 0x64, 0xFE, 0x35};

// header.verify holds this string encrypted with the archive key. Only the
// first three bytes are reliable in shipped archives.
constexpr char kVerifyPlain[16] = "Joymax Pak File";
constexpr std::size_t kVerifyBytes = 3;

template <typename T>
T Load(const std::byte* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

struct RawEntry {
    std::uint8_t type;
    std::string_view name;
    std::int64_t position;
    std::uint32_t size;
    std::int64_t nextChain;
};

// Parses one 128-byte entry. Returns nullopt (with `error` set) when the bytes
// cannot be a valid entry, which is also how a wrong key shows up.
std::optional<RawEntry> ParseEntry(const std::byte* p, std::string& error) {
    RawEntry e{};
    e.type = static_cast<std::uint8_t>(p[0]);
    e.position = Load<std::int64_t>(p + 106);
    e.size = Load<std::uint32_t>(p + 114);
    e.nextChain = Load<std::int64_t>(p + 118);
    if (e.type == kTypeEmpty) {
        return e;
    }
    if (e.type != kTypeFolder && e.type != kTypeFile) {
        error = "unknown entry type " + std::to_string(e.type);
        return std::nullopt;
    }
    if (p[126] != std::byte{0} || p[127] != std::byte{0}) {
        error = "entry padding is not zero (wrong key or corrupt block)";
        return std::nullopt;
    }
    const char* name = reinterpret_cast<const char*>(p + 1);
    const std::size_t len = strnlen(name, 81);
    if (len == 0 || len == 81) {
        error = "entry name is empty or unterminated";
        return std::nullopt;
    }
    e.name = std::string_view(name, len);
    for (unsigned char c : e.name) {
        if (c < 0x20) {
            error = "entry name contains control characters";
            return std::nullopt;
        }
    }
    if (e.position < 0) {
        error = "negative entry position";
        return std::nullopt;
    }
    return e;
}

} // namespace

bool Pk2Archive::Open(const std::filesystem::path& path, std::string& error, std::string_view asciiKey) {
    *this = Pk2Archive{};
    path_ = path;

    if (!mapping_.Open(path, error)) {
        return false;
    }
    const std::span<const std::byte> bytes = mapping_.Data();
    if (bytes.size() < kHeaderSize + kBlockSize) {
        error = "file is too small to be a PK2 archive";
        return false;
    }

    const std::byte* header = bytes.data();
    const std::uint32_t version = Load<std::uint32_t>(header + 30);
    if (version != kVersion) {
        error = "unsupported PK2 version";
        return false;
    }
    const bool encrypted = header[34] != std::byte{0};

    std::optional<Blowfish> cipher;
    if (encrypted) {
        if (asciiKey.empty() || asciiKey.size() > kKeyMask.size()) {
            error = "PK2 key must be 1..10 characters";
            return false;
        }
        std::array<std::uint8_t, kKeyMask.size()> key{};
        for (std::size_t i = 0; i < asciiKey.size(); ++i) {
            key[i] = static_cast<std::uint8_t>(asciiKey[i]) ^ kKeyMask[i];
        }
        cipher.emplace(std::span<const std::uint8_t>(key.data(), asciiKey.size()));

        std::array<std::byte, sizeof(kVerifyPlain)> verify{};
        std::memcpy(verify.data(), kVerifyPlain, verify.size());
        cipher->EncryptBlocks(verify);
        // A mismatch is not fatal on its own (some clients ship a dirty verify
        // field); a wrong key is still caught by the root block's padding check.
        keyVerifiedByHeader_ = std::memcmp(verify.data(), header + 35, kVerifyBytes) == 0;
    }

    // Walk the directory tree. Every block belongs to exactly one directory,
    // so a block seen twice means the chain is cyclic or corrupt.
    struct Pending {
        std::uint64_t block;
        std::string prefix;
    };
    std::vector<Pending> stack{{kRootOffset, {}}};
    std::unordered_set<std::uint64_t> visited;
    std::array<std::byte, kBlockSize> block{};

    while (!stack.empty()) {
        Pending dir = std::move(stack.back());
        stack.pop_back();

        for (std::uint64_t offset = dir.block; offset != 0;) {
            if (offset > bytes.size() - kBlockSize) {
                error = "directory block offset is out of range";
                return false;
            }
            if (!visited.insert(offset).second) {
                error = "directory chain is cyclic";
                return false;
            }
            std::memcpy(block.data(), bytes.data() + offset, kBlockSize);
            if (cipher) {
                cipher->DecryptBlocks(block);
            }

            std::int64_t nextChain = 0;
            for (std::size_t i = 0; i < kEntriesPerBlock; ++i) {
                std::string entryError;
                const auto entry = ParseEntry(block.data() + i * kEntrySize, entryError);
                if (!entry) {
                    error = entryError + (encrypted ? " - is the key right?" : "");
                    return false;
                }
                if (i + 1 == kEntriesPerBlock) {
                    nextChain = entry->nextChain;
                }
                if (entry->type == kTypeEmpty || entry->name == "." || entry->name == "..") {
                    continue;
                }

                std::string fullPath = dir.prefix.empty() ? std::string(entry->name)
                                                          : dir.prefix + '\\' + std::string(entry->name);
                if (entry->type == kTypeFolder) {
                    ++folderCount_;
                    stack.push_back({static_cast<std::uint64_t>(entry->position), std::move(fullPath)});
                    continue;
                }

                const auto position = static_cast<std::uint64_t>(entry->position);
                if (position > bytes.size() || entry->size > bytes.size() - position) {
                    error = "file data lies outside the archive: " + fullPath;
                    return false;
                }
                const auto [it, inserted] =
                    index_.try_emplace(core::NormalizeAssetPath(fullPath), static_cast<std::uint32_t>(files_.size()));
                if (inserted) {
                    files_.push_back({std::move(fullPath), position, entry->size});
                }
            }
            if (nextChain < 0) {
                error = "negative directory chain offset";
                return false;
            }
            offset = static_cast<std::uint64_t>(nextChain);
        }
    }

    return true;
}

const Pk2Archive::File* Pk2Archive::Find(std::string_view path) const {
    const auto it = index_.find(core::NormalizeAssetPath(path));
    return it == index_.end() ? nullptr : &files_[it->second];
}

std::span<const std::byte> Pk2Archive::Read(const File& file) const {
    return mapping_.Data().subspan(static_cast<std::size_t>(file.offset), file.size);
}

} // namespace sr::assets
