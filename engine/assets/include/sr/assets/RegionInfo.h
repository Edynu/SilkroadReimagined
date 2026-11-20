#pragma once

// regioninfo.txt (Media.pk2 `server_dep\silkroad\textdata\`; 1.188 moved it there from resinfo\): the named
// areas, `#TOWN name` / `#FIELD name [dungeon]`, each followed by its regions: `x y ALL` (the whole region) or
// `x y RECT x0 z0 x1 z1` (a part, in the region's own units 0..1920). The area names are effectenvsnd.txt's
// (the music), the towns are where the stall network may be used. Text already decoded
// to UTF-8 (core::DecodeTextFile).

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sr::assets {

class FileSystem;

class RegionAreaTable {
public:
    struct Area {
        std::string name;
        bool town = false;
        std::string dungeon; // the third field of a dungeon's line (donwhang, jinsi); empty otherwise
    };
    bool Parse(std::string_view text, std::string& error);
    // Reads and parses the game's file.
    bool Load(const FileSystem& files, std::string& error);
    // The area a point of a region is in (region = x | y << 8, as proto::WorldPosition; x, z the
    // region's own units): a RECT holding the point first, then the region's ALL (the first such
    // line); null when the region is in no area. Ours: the RECT before ALL order (towns are cut out
    // of the field around them) is inferred from the file, not traced.
    const Area* Find(std::uint16_t region, float x, float z) const;
    // The town a point is in: its index among Areas() (-1: none).
    int TownAt(std::uint16_t region, float x, float z) const;
    const std::vector<Area>& Areas() const { return areas_; }

private:
    struct Part {
        std::uint32_t area = 0;
        bool all = true;
        float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    };
    std::vector<Area> areas_;
    std::unordered_map<std::uint16_t, std::vector<Part>> regions_;
};

} // namespace sr::assets
