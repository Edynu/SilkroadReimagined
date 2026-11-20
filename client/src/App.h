#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "sr/assets/ExeCursors.h"

namespace sr::core {
class Config;
}

namespace sr::client {

// Runs the client until the window closes. Returns the process exit code (2 when --validation
// is on and the Vulkan validation layers reported anything).
int Run(const core::Config& config);

// Writes RGBA8 pixels as a PNG file.
bool SavePng(const std::filesystem::path& path, int width, int height, const std::uint8_t* rgba);
// The mouse cursors kept as files beside the code (Cursors.cpp): read, false when the folder is not there yet; written from
// the executable's once.
bool LoadCursorFolder(const std::filesystem::path& dir, std::vector<assets::CursorImage>& out);
bool SaveCursorFolder(const std::filesystem::path& dir, const std::vector<assets::CursorImage>& images);

} // namespace sr::client
