#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sr/assets/Pk2Archive.h"

namespace sr::assets {

// Bytes of one file: either a view into a source that outlives it (PK2
// mapping) or an owned buffer (loose file read from disk). Move-only.
class FileData {
public:
    static FileData View(std::span<const std::byte> bytes);
    static FileData Owned(std::vector<std::byte> bytes);

    FileData(FileData&&) noexcept = default;
    FileData& operator=(FileData&&) noexcept = default;
    FileData(const FileData&) = delete;
    FileData& operator=(const FileData&) = delete;

    std::span<const std::byte> Bytes() const { return view_; }
    std::size_t Size() const { return view_.size(); }

private:
    FileData() = default;
    std::vector<std::byte> owned_; // moving a vector keeps its buffer, so view_ stays valid
    std::span<const std::byte> view_;
};

// Somewhere asset files come from. Paths passed in are already normalized
// (see core::NormalizeAssetPath).
class IFileSource {
public:
    virtual ~IFileSource() = default;
    virtual std::string_view Name() const = 0;
    virtual bool Contains(const std::string& normalizedPath) const = 0;
    virtual std::optional<FileData> Read(const std::string& normalizedPath) const = 0;
    // Appends the path of every file this source holds ('\'-separated).
    virtual void ListFiles(std::vector<std::string>& out) const = 0;
};

// Serves files out of an opened PK2 archive.
class Pk2Source final : public IFileSource {
public:
    explicit Pk2Source(Pk2Archive archive);
    std::string_view Name() const override { return name_; }
    bool Contains(const std::string& normalizedPath) const override;
    std::optional<FileData> Read(const std::string& normalizedPath) const override;
    void ListFiles(std::vector<std::string>& out) const override;
    const Pk2Archive& Archive() const { return archive_; }

private:
    Pk2Archive archive_;
    std::string name_;
};

// Serves loose files from a folder that mirrors the archive layout, e.g.
// <root>/res/char/foo.bsr. This is how remastered assets replace originals
// one file at a time.
class DirectorySource final : public IFileSource {
public:
    explicit DirectorySource(std::filesystem::path root);
    std::string_view Name() const override { return name_; }
    bool Contains(const std::string& normalizedPath) const override;
    std::optional<FileData> Read(const std::string& normalizedPath) const override;
    void ListFiles(std::vector<std::string>& out) const override;

private:
    std::filesystem::path Resolve(const std::string& normalizedPath) const;

    std::filesystem::path root_;
    std::string name_;
};

// Files the program makes while it runs (a guild's emblem as a texture):
// put from any thread, served like any other file. Mount it before handing the FileSystem out, keep
// a pointer to put into it.
class MemorySource final : public IFileSource {
public:
    std::string_view Name() const override { return "memory"; }
    bool Contains(const std::string& normalizedPath) const override;
    std::optional<FileData> Read(const std::string& normalizedPath) const override;
    void ListFiles(std::vector<std::string>& out) const override;
    // A file in (any spelling of the path; replaces one there).
    void Put(std::string_view path, std::vector<std::byte> bytes);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<std::byte>> files_;
};

// One merged, read-only view over every mounted source. Lookups try sources
// from highest to lowest priority; equal priorities keep mount order. Mount
// everything before handing the FileSystem to other threads.
class FileSystem {
public:
    static constexpr int kGamePriority = 0;
    static constexpr int kOverridePriority = 100;

    void Mount(std::unique_ptr<IFileSource> source, int priority = kGamePriority);

    bool Exists(std::string_view path) const;
    std::optional<FileData> Read(std::string_view path) const;
    // Which source would serve `path`, or nullptr. Useful for debugging overrides.
    const IFileSource* Resolve(std::string_view path) const;
    // Every file across all sources, each path once (first source's spelling),
    // sorted case-insensitively. The archives' and folders' files are listed once
    // and kept (they do not change after mounting); a MemorySource's are asked each time.
    std::vector<std::string> ListFiles() const;
    // The files under `folder` (any spelling; "server_dep\\silkroad\\textdata" or with a trailing
    // separator), at any depth, as normalized paths, sorted: a range of the kept list, not a walk
    // of every file. The table loaders read their few files this way.
    std::vector<std::string> ListFolder(std::string_view folder) const;

    std::span<const std::unique_ptr<IFileSource>> Sources() const { return sources_; }

private:
    // The archives' and folders' files, normalized and sorted, with each one's first spelling.
    struct Index {
        std::vector<std::string> keys, originals;
    };
    std::shared_ptr<const Index> GetIndex() const;
    // The MemorySources' files (normalized), sorted.
    std::vector<std::string> MemoryFiles() const;

    std::vector<std::unique_ptr<IFileSource>> sources_;
    std::vector<int> priorities_;
    mutable std::mutex indexMutex_;
    mutable std::shared_ptr<const Index> index_; // made on first use, dropped by Mount
};

// Mounts Media, Map, Data, Music and Particles from a Silkroad client folder,
// in that order (the order the original client opens them).
bool MountGameArchives(FileSystem& fs, const std::filesystem::path& gameDir, std::string& error);

} // namespace sr::assets
