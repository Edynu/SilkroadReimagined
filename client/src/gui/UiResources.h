#pragma once

// The interface's resources for sr::ui (its textures through the asset store, the text table and
// the font of UiData).

#include <string>

#include "gui/UiData.h"
#include "sr/ui/Painter.h"

namespace sr::client {

class UiResources final : public ui::Resources {
public:
    UiResources(UiTextures& textures, const UiData& data) : textures_(textures), data_(data) {}

    Texture GetTexture(const std::string& path) override {
        const UiTextures::Ref r = textures_.Get(path);
        Texture t;
        t.ref = r.texture;
        t.width = r.width;
        t.height = r.height;
        t.resident = r.resident;
        t.failed = !r.resident && !textures_.Exists(path);
        return t;
    }
    void Prefetch(const std::string& path) override { textures_.Prefetch(path); }
    const std::string& Text(const std::string& key) const override { return data_.Text(key); }
    ui::Font* GetFont() const override { return data_.Font(); }

private:
    UiTextures& textures_;
    const UiData& data_;
};

} // namespace sr::client
