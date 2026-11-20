#include "sr/assets/FileSystem.h"

#include <algorithm>
#include <fstream>
#include <utility>

#include "sr/core/Path.h"

namespace sr::assets {

// ---------------------------------------------------------------- FileData

FileData FileData::View(std::span<const std::byte> bytes) {
    FileData data;
    data.view_ = bytes;
    return data;
}

FileData FileData::Owned(std::vector<std::byte> bytes) {
    FileData data;
    data.owned_ = std::move(bytes);
    data.view_ = data.owned_;
    return data;
}

// ---------------------------------------------------------------- Pk2Source

Pk2Source::Pk2Source(Pk2Archive archive)
    : archive_(std::move(archive)), name_(archive_.Path().filename().string()) {}

bool Pk2Source::Contains(const std::string& normalizedPath) const {
    return archive_.Find(normalizedPath) != nullptr;
}

std::optional<FileData> Pk2Source::Read(const std::string& normalizedPath) const {
    const Pk2Archive::File* file = archive_.Find(normalizedPath);
    if (file == nullptr) {
        return std::nullopt;
    }
    return FileData::View(archive_.Read(*file));
}

void Pk2Source::ListFiles(std::vector<std::string>& out) const {
    for (const auto& file : archive_.Files()) {
        out.push_back(file.path);
    }
}

// ---------------------------------------------------------------- DirectorySource

DirectorySource::DirectorySource(std::filesystem::path root)
    : root_(std::move(root)), name_(root_.string()) {}

std::filesystem::path DirectorySource::Resolve(const std::string& normalizedPath) const {
    // Normalized paths are relative and contain no "." segments; ".." is
    // rejected so a crafted path cannot escape the override folder.
    std::filesystem::path out = root_;
    std::size_t start = 0;
    while (start <= normalizedPath.size()) {
        std::size_t end = normalizedPath.find('\\', start);
        if (end == std::string::npos) {
            end = normalizedPath.size();
        }
        const std::string_view segment(normalizedPath.data() + start, end - start);
        if (segment == "..") {
            return {};
        }
        out /= std::filesystem::path(segment);
        start = end + 1;
    }
    return out;
}

bool DirectorySource::Contains(const std::string& normalizedPath) const {
    const std::filesystem::path full = Resolve(normalizedPath);
    std::error_code ec;
    return !full.empty() && std::filesystem::is_regular_file(full, ec);
}

std::optional<FileData> DirectorySource::Read(const std::string& normalizedPath) const {
    const std::filesystem::path full = Resolve(normalizedPath);
    if (full.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(full, ec);
    if (ec) {
        return std::nullopt;
    }
    std::ifstream in(full, std::ios::binary);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return std::nullopt;
    }
    return FileData::Owned(std::move(bytes));
}

bool MemorySource::Contains(const std::string& normalizedPath) const {
    std::lock_guard lock(mutex_);
    return files_.contains(normalizedPath);
}

std::optional<FileData> MemorySource::Read(const std::string& normalizedPath) const {
    std::lock_guard lock(mutex_);
    const auto it = files_.find(normalizedPath);
    if (it == files_.end()) return std::nullopt;
    return FileData::Owned(it->second);
}

void MemorySource::ListFiles(std::vector<std::string>& out) const {
    std::lock_guard lock(mutex_);
    for (const auto& [path, bytes] : files_) out.push_back(path);
}

void MemorySource::Put(std::string_view path, std::vector<std::byte> bytes) {
    std::string key = core::NormalizeAssetPath(path);
    std::lock_guard lock(mutex_);
    files_[std::move(key)] = std::move(bytes);
}

void DirectorySource::ListFiles(std::vector<std::string>& out) const {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root_, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        std::string relative = it->path().lexically_relative(root_).generic_string();
        std::replace(relative.begin(), relative.end(), '/', '\\');
        out.push_back(std::move(relative));
    }
}

// ---------------------------------------------------------------- FileSystem

std::shared_ptr<const FileSystem::Index> FileSystem::GetIndex() const {
    std::lock_guard lock(indexMutex_);
    if (index_) return index_;
    std::vector<std::pair<std::string, std::string>> keyed; // normalized, original
    std::vector<std::string> paths;
    for (const auto& source : sources_) {
        if (dynamic_cast<const MemorySource*>(source.get()) != nullptr) continue; // changes while running
        paths.clear();
        source->ListFiles(paths);
        keyed.reserve(keyed.size() + paths.size());
        for (auto& path : paths) {
            keyed.emplace_back(core::NormalizeAssetPath(path), std::move(path));
        }
    }
    // Stable: for duplicate paths the higher-priority source (listed first) wins.
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    keyed.erase(std::unique(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first == b.first; }),
                keyed.end());
    auto index = std::make_shared<Index>();
    index->keys.reserve(keyed.size());
    index->originals.reserve(keyed.size());
    for (auto& [key, original] : keyed) {
        index->keys.push_back(std::move(key));
        index->originals.push_back(std::move(original));
    }
    index_ = std::move(index);
    return index_;
}

std::vector<std::string> FileSystem::MemoryFiles() const {
    std::vector<std::string> out;
    for (const auto& source : sources_) {
        if (dynamic_cast<const MemorySource*>(source.get()) != nullptr) source->ListFiles(out);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> FileSystem::ListFiles() const {
    const std::shared_ptr<const Index> index = GetIndex();
    const std::vector<std::string> memory = MemoryFiles();
    if (memory.empty()) return index->originals;
    // Merged by key; a file in both is listed once (the kept list's spelling).
    std::vector<std::string> out;
    out.reserve(index->keys.size() + memory.size());
    std::size_t m = 0;
    for (std::size_t i = 0; i < index->keys.size(); ++i) {
        for (; m < memory.size() && memory[m] < index->keys[i]; ++m) out.push_back(memory[m]);
        if (m < memory.size() && memory[m] == index->keys[i]) ++m;
        out.push_back(index->originals[i]);
    }
    out.insert(out.end(), memory.begin() + static_cast<std::ptrdiff_t>(m), memory.end());
    return out;
}

std::vector<std::string> FileSystem::ListFolder(std::string_view folder) const {
    std::string prefix = core::NormalizeAssetPath(folder);
    if (!prefix.empty()) prefix += '\\';
    const std::shared_ptr<const Index> index = GetIndex();
    std::vector<std::string> out;
    for (auto it = std::lower_bound(index->keys.begin(), index->keys.end(), prefix); it != index->keys.end() && it->starts_with(prefix); ++it) {
        out.push_back(*it);
    }
    bool added = false;
    for (std::string& path : MemoryFiles()) {
        if (!path.starts_with(prefix) || std::binary_search(out.begin(), out.end(), path)) continue;
        out.push_back(std::move(path));
        added = true;
    }
    if (added) std::sort(out.begin(), out.end());
    return out;
}

void FileSystem::Mount(std::unique_ptr<IFileSource> source, int priority) {
    {
        std::lock_guard lock(indexMutex_);
        index_.reset(); // listed again on next use
    }
    // Insert after every source with priority >= this one: highest first,
    // mount order among equals.
    std::size_t at = 0;
    while (at < priorities_.size() && priorities_[at] >= priority) {
        ++at;
    }
    sources_.insert(sources_.begin() + static_cast<std::ptrdiff_t>(at), std::move(source));
    priorities_.insert(priorities_.begin() + static_cast<std::ptrdiff_t>(at), priority);
}

const IFileSource* FileSystem::Resolve(std::string_view path) const {
    const std::string key = core::NormalizeAssetPath(path);
    for (const auto& source : sources_) {
        if (source->Contains(key)) {
            return source.get();
        }
    }
    return nullptr;
}

bool FileSystem::Exists(std::string_view path) const { return Resolve(path) != nullptr; }

std::optional<FileData> FileSystem::Read(std::string_view path) const {
    const std::string key = core::NormalizeAssetPath(path);
    for (const auto& source : sources_) {
        if (auto data = source->Read(key)) {
            return data;
        }
    }
    return std::nullopt;
}

bool MountGameArchives(FileSystem& fs, const std::filesystem::path& gameDir, std::string& error) {
    for (const char* name : {"Media.pk2", "Map.pk2", "Data.pk2", "Music.pk2", "Particles.pk2"}) {
        Pk2Archive archive;
        if (!archive.Open(gameDir / name, error)) {
            error = std::string(name) + ": " + error;
            return false;
        }
        fs.Mount(std::make_unique<Pk2Source>(std::move(archive)), FileSystem::kGamePriority);
    }
    return true;
}

} // namespace sr::assets
