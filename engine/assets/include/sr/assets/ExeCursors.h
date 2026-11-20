#pragma once

// The game's mouse cursors. The original client keeps them as Windows cursor resources in
// sro_client.exe (cursor groups 149..166), not in the archives; we
// read them from the player's own client at start:
//   0x95 arrow (the normal one), 0x96 repair hammer, 0x97 attack, 0x98 talk (NPC),
//   0x99 / 0x9A pick up (the left button held), 0xA0 attack a player, 0xA1 teleport gate,
//   0xA3 a stall, 0xA6 pick a target item.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sr::assets {

struct CursorImage {
    std::uint16_t id = 0; // the group resource id
    int width = 0, height = 0;
    int hotX = 0, hotY = 0;
    std::vector<std::uint8_t> rgba; // top-down
};

// Every cursor group of a PE file (the first image of each group), as RGBA.
bool DecodeExeCursors(std::span<const std::byte> exe, std::vector<CursorImage>& out, std::string& error);

} // namespace sr::assets
