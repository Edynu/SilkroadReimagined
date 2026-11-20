#pragma once

// Builds small, valid, encrypted PK2 archives for tests, so the reader can be
// exercised without the game's files.

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "sr/assets/Blowfish.h"

namespace sr::test {

class TestPk2Writer {
public:
    struct Entry {
        std::uint8_t type; // 1 folder, 2 file
        std::string name;
        std::uint64_t position = 0;
        std::uint32_t size = 0;
    };

    explicit TestPk2Writer(std::string asciiKey = "169841") : asciiKey_(std::move(asciiKey)) {
        bytes_.resize(256);
        std::memcpy(bytes_.data(), "JoyMax File Manager!\n", 21);
        Put<std::uint32_t>(30, 0x01000002);
        bytes_[34] = 1; // encrypted
        std::array<std::byte, 16> verify{};
        std::memcpy(verify.data(), "Joymax Pak File", 16);
        Cipher().EncryptBlocks(verify);
        std::memcpy(bytes_.data() + 35, verify.data(), 3);
    }

    // Reserves an empty, zeroed directory block and returns its offset.
    std::uint64_t AddBlock() {
        const std::uint64_t at = bytes_.size();
        bytes_.resize(bytes_.size() + 2560);
        blocks_.push_back(at);
        return at;
    }

    // Appends raw file data and returns its offset.
    std::uint64_t AddData(const std::string& data) {
        const std::uint64_t at = bytes_.size();
        bytes_.insert(bytes_.end(), data.begin(), data.end());
        return at;
    }

    void SetEntry(std::uint64_t block, int slot, const Entry& e) {
        const std::size_t base = static_cast<std::size_t>(block) + static_cast<std::size_t>(slot) * 128;
        bytes_[base] = e.type;
        std::memcpy(bytes_.data() + base + 1, e.name.data(), e.name.size());
        Put<std::uint64_t>(base + 106, e.position);
        Put<std::uint32_t>(base + 114, e.size);
    }

    void SetNextChain(std::uint64_t block, std::uint64_t next) {
        Put<std::uint64_t>(static_cast<std::size_t>(block) + 19 * 128 + 118, next);
    }

    void Write(const std::filesystem::path& path) {
        std::vector<std::uint8_t> out = bytes_;
        const sr::assets::Blowfish cipher = Cipher();
        for (std::uint64_t block : blocks_) {
            cipher.EncryptBlocks(std::span(reinterpret_cast<std::byte*>(out.data() + block), 2560));
        }
        std::ofstream os(path, std::ios::binary);
        os.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    }

private:
    template <typename T>
    void Put(std::size_t at, T value) {
        std::memcpy(bytes_.data() + at, &value, sizeof(T));
    }

    sr::assets::Blowfish Cipher() const {
        static constexpr std::uint8_t kMask[10] = {0x03, 0xF8, 0xE4, 0x44, 0x88, 0x99, 0x3F, 0x64, 0xFE, 0x35};
        std::vector<std::uint8_t> key(asciiKey_.size());
        for (std::size_t i = 0; i < key.size(); ++i) {
            key[i] = static_cast<std::uint8_t>(asciiKey_[i]) ^ kMask[i];
        }
        return sr::assets::Blowfish(key);
    }

    std::string asciiKey_;
    std::vector<std::uint8_t> bytes_;
    std::vector<std::uint64_t> blocks_;
};

// A small archive used by several tests:
//   Readme.txt          "hello"
//   Res\Char\Man.BSR    "man-bsr"
//   Res\extra.dat       "extra"     (in a second, chained root block)
inline void WriteSampleArchive(const std::filesystem::path& path, const std::string& key = "169841") {
    TestPk2Writer w(key);
    const auto root = w.AddBlock();
    const auto rootMore = w.AddBlock();
    const auto res = w.AddBlock();
    const auto chr = w.AddBlock();

    w.SetEntry(root, 0, {1, ".", root});
    w.SetEntry(root, 1, {1, "Res", res});
    w.SetEntry(root, 2, {2, "Readme.txt", w.AddData("hello"), 5});
    w.SetNextChain(root, rootMore);

    w.SetEntry(rootMore, 0, {2, "Root2.txt", w.AddData("root2"), 5});

    w.SetEntry(res, 0, {1, ".", res});
    w.SetEntry(res, 1, {1, "..", root});
    w.SetEntry(res, 2, {1, "Char", chr});
    w.SetEntry(res, 3, {2, "extra.dat", w.AddData("extra"), 5});

    w.SetEntry(chr, 0, {1, ".", chr});
    w.SetEntry(chr, 1, {1, "..", res});
    w.SetEntry(chr, 2, {2, "Man.BSR", w.AddData("man-bsr"), 7});

    w.Write(path);
}

inline std::filesystem::path TempPath(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / "sr_tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

} // namespace sr::test
