#include "sr/game/Loading.h"

namespace sr::game {

namespace {

// Teleport destinations (region id (z << 8) | x; each the region of a gate in teleportdata.txt).
struct Destination {
    std::uint16_t region;
    const char* picture;
};
constexpr Destination kDestinations[] = {
    {0x61A8, "loading_zangan"},          // Jangan
    {0x6699, "loading_dunwhang"},        // Donwhang
    {0x5C87, "loading_hotan"},           // Hotan
    {0x6A6C, "loading_samarkand"},       // Samarkand
    {0x694F, "loading_constantinople"},  // Constantinople
    {0x60B6, "loading_thief"},           // the thieves' town
    {0x8001, "loading_dungeons_donwhang"}, // the Donwhang cave
    // The ferries of Hotan, Donwhang and Jangan, then the Europe and Asia Minor ferries.
    {0x5A8C, "loading_river"}, {0x5A8F, "loading_river"}, {0x5B8C, "loading_river"}, {0x5B8F, "loading_river"},
    {0x609E, "loading_river"}, {0x61A1, "loading_river"}, {0x629C, "loading_river"}, {0x64A1, "loading_river"},
    {0x624B, "loading_port2"}, {0x6559, "loading_port2"}, {0x6759, "loading_port2"},
};

std::string Path(const char* name) { return std::string("interface\\loading\\") + name + ".ddj"; }

} // namespace

std::string LoadingPicture(LoadingKind kind, std::uint16_t destination, unsigned random) {
    switch (kind) {
    case LoadingKind::FirstEntry: return Path(random % 2 == 0 ? "loading_europe_1" : "loading_europe_2");
    case LoadingKind::Rebirth: return Path("loading_rebirth");
    case LoadingKind::Teleport: break;
    }
    for (const Destination& d : kDestinations) {
        if (d.region == destination) return Path(d.picture);
    }
    if (destination >= 0x8002 && destination <= 0x8007) return Path("loading_royalmausoleum");
    return Path("loading_china_1");
}

} // namespace sr::game
