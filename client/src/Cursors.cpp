// The mouse cursors kept beside the code: the cursor groups of the player's sro_client.exe (assets::DecodeExeCursors), written
// once as PNG files with an index, so later starts read them there instead of the executable. The folder is git-ignored:
// they are the original's art, never committed.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <format>
#include <fstream>
#include <sstream>

#include "App.h"

namespace sr::client {

bool LoadCursorFolder(const std::filesystem::path& dir, std::vector<assets::CursorImage>& out) {
    std::ifstream index(dir / "cursors.txt");
    if (!index) return false;
    std::vector<assets::CursorImage> images;
    std::string line;
    while (std::getline(index, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        unsigned id = 0;
        int hotX = 0, hotY = 0;
        std::string file;
        if (!(in >> std::hex >> id >> std::dec >> hotX >> hotY >> file)) continue;
        int w = 0, h = 0, n = 0;
        stbi_uc* pixels = stbi_load((dir / file).string().c_str(), &w, &h, &n, 4);
        if (pixels == nullptr) return false; // a file missing: the executable again
        assets::CursorImage c;
        c.id = std::uint16_t(id);
        c.width = w;
        c.height = h;
        c.hotX = hotX;
        c.hotY = hotY;
        c.rgba.assign(pixels, pixels + std::size_t(w) * std::size_t(h) * 4);
        stbi_image_free(pixels);
        images.push_back(std::move(c));
    }
    if (images.empty()) return false;
    out = std::move(images);
    return true;
}

bool SaveCursorFolder(const std::filesystem::path& dir, const std::vector<assets::CursorImage>& images) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;
    std::ofstream index(dir / "cursors.txt");
    if (!index) return false;
    index << "# The game's mouse cursors, from the player's sro_client.exe (its cursor groups; never committed).\n"
             "# id (hex, the group resource id) hotX hotY file\n";
    for (const assets::CursorImage& c : images) {
        const std::string file = std::format("cursor_{:x}.png", c.id);
        if (!SavePng(dir / file, c.width, c.height, c.rgba.data())) return false;
        index << std::format("{:x} {} {} {}\n", c.id, c.hotX, c.hotY, file);
    }
    return bool(index);
}

} // namespace sr::client
