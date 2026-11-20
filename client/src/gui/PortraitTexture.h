#pragma once

// The interface's side of the HUD's portraits (frame/Portraits.h draws them):
// the texture a window draws for a character's portrait, an external texture of the screen's
// UiTextures (its path starts with '@': no game file does). Headless, as the HUD's windows are.

#include <cstdint>
#include <string>

namespace sr::client {

// The player's own portrait's key; a party member's is its character's id (never 0).
constexpr std::uint32_t kSelfPortrait = 0;
// The item mall's preview of the player's whole figure (Portraits::ShowFigure).
constexpr std::uint32_t kMallPreview = 0xFFFF0001u;

inline std::string PortraitTexture(std::uint32_t key) {
    return "@portrait\\" + std::to_string(key);
}

} // namespace sr::client
