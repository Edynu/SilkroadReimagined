#include "sr/core/MappedFile.h"

#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

namespace sr::core {

MappedFile::~MappedFile() { Close(); }

MappedFile::MappedFile(MappedFile&& other) noexcept { *this = std::move(other); }

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        Close();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        opened_ = std::exchange(other.opened_, false);
#ifdef _WIN32
        fileHandle_ = std::exchange(other.fileHandle_, nullptr);
        mappingHandle_ = std::exchange(other.mappingHandle_, nullptr);
#endif
    }
    return *this;
}

#ifdef _WIN32

static std::string LastErrorMessage(const char* what) {
    return std::string(what) + " failed (Win32 error " + std::to_string(::GetLastError()) + ")";
}

bool MappedFile::Open(const std::filesystem::path& path, std::string& error) {
    Close();

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = LastErrorMessage("CreateFileW");
        return false;
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size)) {
        error = LastErrorMessage("GetFileSizeEx");
        ::CloseHandle(file);
        return false;
    }

    fileHandle_ = file;
    size_ = static_cast<std::size_t>(size.QuadPart);
    opened_ = true;
    if (size_ == 0) {
        return true; // CreateFileMapping rejects empty files; an empty view is fine.
    }

    HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        error = LastErrorMessage("CreateFileMappingW");
        Close();
        return false;
    }
    mappingHandle_ = mapping;

    void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        error = LastErrorMessage("MapViewOfFile");
        Close();
        return false;
    }
    data_ = static_cast<const std::byte*>(view);
    return true;
}

void MappedFile::Close() {
    if (data_ != nullptr) {
        ::UnmapViewOfFile(data_);
    }
    if (mappingHandle_ != nullptr) {
        ::CloseHandle(mappingHandle_);
    }
    if (fileHandle_ != nullptr) {
        ::CloseHandle(fileHandle_);
    }
    data_ = nullptr;
    size_ = 0;
    opened_ = false;
    mappingHandle_ = nullptr;
    fileHandle_ = nullptr;
}

#else

bool MappedFile::Open(const std::filesystem::path& path, std::string& error) {
    Close();

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        error = std::string("open failed: ") + std::strerror(errno);
        return false;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        error = std::string("fstat failed: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    size_ = static_cast<std::size_t>(st.st_size);
    opened_ = true;
    if (size_ > 0) {
        void* view = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        if (view == MAP_FAILED) {
            error = std::string("mmap failed: ") + std::strerror(errno);
            ::close(fd);
            size_ = 0;
            opened_ = false;
            return false;
        }
        data_ = static_cast<const std::byte*>(view);
    }
    ::close(fd); // The mapping keeps its own reference to the file.
    return true;
}

void MappedFile::Close() {
    if (data_ != nullptr) {
        ::munmap(const_cast<std::byte*>(data_), size_);
    }
    data_ = nullptr;
    size_ = 0;
    opened_ = false;
}

#endif

} // namespace sr::core
