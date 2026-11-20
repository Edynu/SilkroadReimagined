#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "sr/rhi/Rhi.h"

namespace sr::assets {
class FileSystem;
struct TextureData;
} // namespace sr::assets

namespace sr::render {

// Loads .ddj/.dds textures from the file system onto the GPU once per path.
class TextureCache {
public:
    struct Entry {
        rhi::TextureHandle handle; // null when loading failed
        bool premultipliedAlpha = false; // DXT2/DXT4
        std::uint32_t width = 0, height = 0;
        std::string error;
    };

    TextureCache(rhi::Device& device, const assets::FileSystem& fs) : device_(device), fs_(fs) {}
    ~TextureCache() { Clear(); }
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    const Entry& Get(const std::string& path);
    // A texture decoded elsewhere (a worker thread): uploaded unless the path is cached.
    void Put(const std::string& path, const assets::TextureData& texture);
    bool Has(const std::string& path) const;
    void Clear();
    std::size_t Size() const { return entries_.size(); }

private:
    rhi::Device& device_;
    const assets::FileSystem& fs_;
    std::unordered_map<std::string, Entry> entries_; // key: normalized path
};

// Uploads a decoded texture with all its mips; returns a null handle on failure.
rhi::TextureHandle UploadTexture(rhi::Device& device, const assets::TextureData& texture, const std::string& debugName);

} // namespace sr::render
