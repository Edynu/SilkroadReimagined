#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

namespace sr::core {

// Read-only memory mapping of a whole file. Move-only; the mapping lives as
// long as the object. Views handed out by data() stay valid until then, and
// reading them from several threads at once is safe.
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();

    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // Maps `path`. On failure returns false and leaves a message in `error`.
    bool Open(const std::filesystem::path& path, std::string& error);
    void Close();

    bool IsOpen() const { return opened_; }
    std::span<const std::byte> Data() const { return {data_, size_}; }
    std::size_t Size() const { return size_; }

private:
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    bool opened_ = false;
#ifdef _WIN32
    void* fileHandle_ = nullptr;
    void* mappingHandle_ = nullptr;
#endif
};

} // namespace sr::core
