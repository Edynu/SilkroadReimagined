#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "App.h"

namespace sr::client {

bool SavePng(const std::filesystem::path& path, int width, int height, const std::uint8_t* rgba) {
    return stbi_write_png(path.string().c_str(), width, height, 4, rgba, width * 4) != 0;
}

bool SaveJpeg(const std::filesystem::path& path, int width, int height, const std::uint8_t* rgba, int quality) {
    return stbi_write_jpg(path.string().c_str(), width, height, 4, rgba, quality) != 0;
}

} // namespace sr::client
