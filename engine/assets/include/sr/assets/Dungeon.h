#pragma once

// Dungeons: `dungeon\dungeoninfo.txt` names the dungeon of each dungeon
// region (region id = the listed id | 0x8000), and a `.dof` (JMXVDOF 0101) lays it out as blocks:
// rooms and passages, each a `.bsr` placed in the dungeon's own space (no region grid inside a
// dungeon) with its fog, ambient colour, the blocks drawn with it, its objects and point lights.
// A block's walkable surface is the object navmesh of its floor mesh (the .bsr's navmesh path).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sr::assets {

struct DungeonInfoEntry {
    bool inService = false;     // 1.188's first column (0: listed but not in use, the Jupiter temple's among them)
    std::uint16_t regionId = 0; // bit 15 set
    std::string path;           // the .dof
};
// CP949 text: comment lines ("//"), then per dungeon the service flag, a decimal id and a quoted
// path, separated by tabs (1.188; v1.150 had no service column).
bool DecodeDungeonInfo(std::span<const std::byte> file, std::vector<DungeonInfoEntry>& out, std::string& error);

struct DungeonData {
    struct Fog {
        std::uint32_t color = 0; // ARGB
        float start = 0, end = 0;
        float density = 0;       // the lit models' exponential fog
        bool heightFog = false;  // the four values below follow (only in files v1.150 does not use)
        float height[4]{};
    };
    struct Object {
        std::string name, path;  // path: a .bsr
        float position[3]{};     // dungeon space
        float rotation[3]{};     // Euler angles, radians (y = yaw)
        float scale[3]{1, 1, 1};
        std::uint32_t flags = 0; // 2: an obstacle (value = radius squared); 4: a water quad (colour follows)
        std::uint32_t index = 0; // the block light lighting it
        float value = 0;
        std::uint32_t color = 0; // ARGB, flags & 4
    };
    struct Light {
        std::string name;
        float position[3]{};
        float diffuse[3]{}, ambient[3]{}, specular[3]{};
        float attenuation[3]{}; // constant, linear, quadratic
    };
    struct Block {
        std::string path, name;  // path: a .bsr
        float position[3]{};     // dungeon space
        float yaw = 0;           // radians about +y
        std::uint32_t flags = 0; // bit 0: an entrance (sees the outdoor world)
        float boundsMin[3]{}, boundsMax[3]{}; // block-local
        std::uint32_t ambient = 0; // ARGB: the lit models' ambient light
        Fog fog;
        bool motion = false;     // a sway record (28 bytes) followed: vector, angles, period in ms
        float motionVector[3]{}, motionAngles[3]{};
        std::uint32_t motionPeriod = 0;
        std::int32_t room = -1, floor = -1; // into roomNames / floorNames
        std::vector<std::uint32_t> neighbours; // blocks joined by a walkable doorway
        std::vector<std::uint32_t> visible;    // blocks drawn with this one (never itself)
        std::vector<Object> objects;
        std::uint32_t obstacles = 0; // objects with flag 2
        std::vector<Light> lights;
    };
    struct GridCell {
        std::uint32_t key = 0; // (y << 20) | (z << 10) | x, 200-unit cells from the box minimum
        std::vector<std::uint32_t> blocks;
    };
    struct Group {
        std::string name;
        std::uint32_t flags = 0;
        std::vector<std::uint32_t> blocks;
    };
    static constexpr float kGridCell = 200.0f;

    std::uint16_t regionId = 0;
    float boundsMin[3]{}, boundsMax[3]{};
    std::vector<Block> blocks;
    std::vector<std::vector<std::uint32_t>> links; // per block: the blocks touching it
    std::uint32_t gridSize[3]{};                   // x, y, z
    std::vector<GridCell> grid;
    std::vector<std::string> roomNames, floorNames;
    std::vector<Group> groups;
};

// JMXVDOF 0101; every section read to its exact end. An older revision (one file in the archive,
// without the height-fog byte) is refused, as by the client.
bool DecodeDof(std::span<const std::byte> file, DungeonData& out, std::string& error);

} // namespace sr::assets
