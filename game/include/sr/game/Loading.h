#pragma once

// The loading screen's picture: the original
// picks it by what is loading: the first entry into the world shows one of two Europe pictures at
// random, the load after the player's own death the rebirth picture, and a teleport the picture
// of its destination region (a fixed table of towns, ferries and dungeons; China otherwise).

#include <cstdint>
#include <string>

namespace sr::game {

enum class LoadingKind : std::uint8_t { FirstEntry, Rebirth, Teleport };

// "interface\loading\<name>.ddj" by the 1.188 client's table. `random` picks among the first entry's pictures. A name
// missing from the archive (1.188's custumize_china_loading, its character select's): the caller falls back to
// kLoadingFallback.
std::string LoadingPicture(LoadingKind kind, std::uint16_t destination, unsigned random);
inline constexpr const char* kLoadingFallback = "interface\\loading\\loading_china_1.ddj";

} // namespace sr::game
