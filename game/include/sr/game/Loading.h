#pragma once

// The loading screen's picture: the original
// picks it by what is loading: the first entry into the world shows one of two Europe pictures at
// random, the load after the player's own death the rebirth picture, and a teleport the picture
// of its destination region (a fixed table of towns, ferries and dungeons; China otherwise).

#include <cstdint>
#include <string>

namespace sr::game {

enum class LoadingKind : std::uint8_t { FirstEntry, Rebirth, Teleport };

// "interface\loading\<name>.ddj". `random` picks among the first entry's pictures. Two names the
// original uses are missing from v1.150's Media.pk2 (loading_royalmausoleum for dungeons
// 0x8002-0x8007, custumize_china_loading): the caller falls back to kLoadingFallback.
std::string LoadingPicture(LoadingKind kind, std::uint16_t destination, unsigned random);
inline constexpr const char* kLoadingFallback = "interface\\loading\\loading_china_1.ddj";

} // namespace sr::game
