#include "sr/render/TextureCache.h"

#include <vector>

#include "sr/assets/FileSystem.h"
#include "sr/assets/Texture.h"
#include "sr/core/Path.h"

namespace sr::render {

namespace {

rhi::Format ToRhi(assets::TextureFormat format) {
    switch (format) {
    case assets::TextureFormat::RGBA8: return rhi::Format::RGBA8Unorm;
    case assets::TextureFormat::BC1: return rhi::Format::BC1Unorm;
    case assets::TextureFormat::BC2: return rhi::Format::BC2Unorm;
    case assets::TextureFormat::BC3: return rhi::Format::BC3Unorm;
    }
    return rhi::Format::Unknown;
}

} // namespace

rhi::TextureHandle UploadTexture(rhi::Device& device, const assets::TextureData& texture, const std::string& debugName) {
    std::vector<std::span<const std::byte>> mips;
    mips.reserve(texture.mips.size());
    for (std::size_t i = 0; i < texture.mips.size(); ++i) {
        mips.push_back(texture.MipBytes(i));
    }
    const rhi::TextureInitData init{mips};
    return device.CreateTexture({texture.width, texture.height, static_cast<std::uint32_t>(mips.size()),
                                 ToRhi(texture.format), debugName},
                                &init);
}

const TextureCache::Entry& TextureCache::Get(const std::string& path) {
    const std::string key = core::NormalizeAssetPath(path);
    if (const auto it = entries_.find(key); it != entries_.end()) {
        return it->second;
    }
    Entry entry;
    auto data = fs_.Read(path);
    assets::TextureData texture;
    if (!data) {
        entry.error = "file not found";
    } else if (const bool dds = key.ends_with(".dds");
               !(dds ? assets::DecodeDds(data->Bytes(), texture, entry.error)
                     : assets::DecodeDdj(data->Bytes(), texture, entry.error))) {
        // error already set
    } else {
        // The world's textures get their whole mip chain (the interface is drawn 1:1).
        if (!key.starts_with("interface\\")) assets::CompleteMips(texture);
        entry.handle = UploadTexture(device_, texture, path);
        entry.premultipliedAlpha = texture.premultipliedAlpha;
        entry.width = texture.width;
        entry.height = texture.height;
        if (!entry.handle) entry.error = "GPU upload failed";
    }
    return entries_.emplace(key, std::move(entry)).first->second;
}

void TextureCache::Put(const std::string& path, const assets::TextureData& texture) {
    const std::string key = core::NormalizeAssetPath(path);
    if (entries_.contains(key)) return;
    Entry entry;
    entry.handle = UploadTexture(device_, texture, path);
    entry.premultipliedAlpha = texture.premultipliedAlpha;
    entry.width = texture.width;
    entry.height = texture.height;
    if (!entry.handle) entry.error = "GPU upload failed";
    entries_.emplace(key, std::move(entry));
}

bool TextureCache::Has(const std::string& path) const { return entries_.contains(core::NormalizeAssetPath(path)); }

void TextureCache::Clear() {
    for (auto& [key, entry] : entries_) {
        if (entry.handle) device_.DestroyTexture(entry.handle);
    }
    entries_.clear();
}

} // namespace sr::render
