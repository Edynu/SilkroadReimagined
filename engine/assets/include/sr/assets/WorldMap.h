#pragma once

// The world map's tables (Media.pk2 server_dep\silkroad\textdata), measured over the 1.188
// files:
//
// worldmap_mapinfo.txt, section WLocalmap (a row per map; 0 the world, 1..999 towns and areas, 1001..1003 the fortresses):
//   0 id, 1 type (0 tiled, 1 one picture), 2 a Korean name, 3 the picture (tiled: a prefix completed by
//   "<regionX>x<regionY>.ddj", one tile a band of regions), 4/5 the texture's size, 6/7 the part of it drawn,
//   8/9 the drawn size (pixels), 10..13 its region box left, top, right, bottom, 14..17 the offsets of its top left
//   and bottom right corner in those regions (192 to a region), 18 its fortress's instance code (xxx none), 19 its
//   name key, 20 1 when it is a place of its own (0: the world, a fortress), 21 a tiled map's regions to a tile
//   ("4x4"; xxx one picture).
// Section Dungeonmap (a row per floor, ids from 2001): 0 id, 1 a Korean name, 2 the floor's letter (F above ground,
//   B below), 3 its number, 4 the dungeon's floors, 5 its region id (0x8000 | the dungeon), 6/7 its tiles across
//   and down, 8/9 the drawn size, 10..13 its region box left, top, right, bottom (a tile a region, named
//   "<prefix><regionX>x<regionY>.ddj"), 14 a scale, 15 the tiles' prefix, 16 an index, 17 the floor's text key,
//   18 the whole-map button's kind, 19..22 four numbers.
// worldmap_localinfo.txt (a row per label or icon on a map): 0 in service, 1 id, 2 kind (1 a text, 2 a picture),
//   3 the text key or the picture, 4..6 Korean notes, 7 the map a click opens (0 none), 8 the map it is on
//   (0 the world), 9/10 its region (on the world map: 32 pixels to a region), 11/12 its place (pixels: in that
//   region on the world map, else on the map), 13/14 a picture's size (a text's: two flags), 15..17 a text's
//   colour, 18..20 its shadow, bold and blur, 21 a tooltip key (xxx none).

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sr/assets/Terrain.h"

namespace sr::assets {

class FileSystem;

struct WorldMapRow {
    int id = 0;
    bool tiled = false;
    std::string picture; // tiled: the tiles' prefix
    int textureW = 0, textureH = 0, pictureW = 0, pictureH = 0, drawW = 0, drawH = 0;
    int regionLeft = 0, regionTop = 0, regionRight = 0, regionBottom = 0;
    int leftX = 0, topY = 0, rightX = 0, bottomY = 0; // the corners' offsets in their regions (192 to a region)
    std::string fortress, nameKey;
    bool place = false;
    int tileRegions = 0; // a tiled map: regions to a tile across and down (4 for "4x4")

    // Its edges in world units (192 to a region, counted from region 135 across and 92 down).
    int Left() const { return (regionLeft - 135) * 192 + leftX; }
    int Right() const { return (regionRight - 135) * 192 + rightX; }
    int Top() const { return (regionTop - 92) * 192 + topY; }
    int Bottom() const { return (regionBottom - 92) * 192 + bottomY; }
    // A tiled map's tiles across and down, and the file of tile (i, j) (i from the left, j from the top).
    int TilesAcross() const { return tiled && textureW > 0 ? drawW / textureW : 0; }
    int TilesDown() const { return tiled && textureH > 0 ? drawH / textureH : 0; }
    std::string Tile(int i, int j) const;
};

struct DungeonMapRow {
    int id = 0;
    std::string name; // the Korean name (one per dungeon: its floors share it)
    char letter = 'F';
    int floor = 1, floors = 1;
    std::uint16_t region = 0;
    int tilesAcross = 0, tilesDown = 0, drawW = 0, drawH = 0;
    int regionLeft = 0, regionTop = 0, regionRight = 0, regionBottom = 0;
    std::string prefix, floorKey;

    // "1F" above ground, "B1" below.
    std::string Label() const;
    // Its edges in world units: whole regions, the right and top ones included.
    int Left() const { return (regionLeft - 135) * 192; }
    int Right() const { return (regionRight - 134) * 192; }
    int Top() const { return (regionTop - 91) * 192; }
    int Bottom() const { return (regionBottom - 92) * 192; }
    std::string Tile(int i, int j) const; // i from the left, j from the top
};

struct WorldMapLabel {
    int id = 0;
    bool picture = false; // false: a text
    std::string what;     // the text key or the picture
    int opens = 0;        // the map a click opens (0 none)
    int map = 0;          // the map it is on (0 the world)
    int regionX = 0, regionZ = 0, x = 0, y = 0;
    int width = 0, height = 0;
    std::uint8_t red = 255, green = 255, blue = 255;
    bool shadow = false, bold = false, blur = false;
    std::string tooltipKey;
};

// refoptionalteleport.txt (the database's _RefOptionalTeleport): the places the
// reverse return scroll takes a player to from the world map. Columns: 0 service, 1 id, 2 name (Korean), 3 zone name
// key ("xxx" for a group's flag), 4 region (a dungeon's as a negative 16-bit number), 5..7 x y z, 8 world, 9 its
// group (-1 none), 10 a map marker of its own (1), 11 the lowest level, 12 the highest (0 none).
struct ReturnPointRow {
    int id = 0;
    std::string zoneKey; // empty: a group's flag
    std::uint16_t region = 0;
    float x = 0, y = 0, z = 0;
    int group = -1;
    bool marker = false;
    int levelMin = 0, levelMax = 0;
};

// siegefortressreward.txt (the 1.188 client's): a fortress's crest box on the world map
// at 32 pixels to a region (region x, region z, and pixels in from the region's top left) and the town whose entry
// banner names its guild. Columns: 0 service, 1 the fortress's codename, 2 the town's region name (xxx: none), 3..6.
struct FortressMapPlace {
    std::string fortress; // FORTRESS_JANGAN
    std::string town;     // RN_CH_JANGAN
    int regionX = 0, regionZ = 0, x = 0, y = 0;
};

// The client's own fortress tables (server_dep\silkroad\textdata): refsiegeblessbuff.txt (the manager's blessings: 0 service,
// 1 BlessID, 2 the fortress, 3 the buff's skill, 4 gold, 5 GP) and siegefortressitemforge.txt (the smith's and the trainer's
// list: 0 service, 1 the fortress, 2 the item, 3 gold, 4 GP, 5 minutes).
struct FortressBlessRow {
    int id = 0, fortress = 0;
    std::uint32_t skill = 0;
    std::uint64_t gold = 0;
    std::uint32_t guildPoints = 0;
};
// refsiegedungeon.txt (the Cave of Discipline: 0 service, 1 the fortress, 2 its world, 3 MaxCreateCount, 4 EntryGold, 5 EntryGP).
struct FortressDungeonRow {
    int fortress = 0, world = 0, maxCreate = 0;
    std::uint64_t gold = 0;
    std::uint32_t guildPoints = 0;
};
struct FortressForgeRow {
    int fortress = 0;
    std::uint32_t item = 0;
    std::uint64_t gold = 0;
    std::uint32_t guildPoints = 0;
    int minutes = 0;
};

// eventzonedata.txt (the 1.188 client's copy of `_RefEventZone`: 0 service, 1 ID, 2 ZoneName, 3 EventName, 4..8 Param1..5,
// 9 strParam1 the structure, 10 strParam2 the world, ...): the fortresses' barricade and war camp zones, which a manual's
// use marks, each placed at its ZoneName's spot in
// navmesh\objectstring.ifo (NamedSpot; not placed when the file has none).
struct FortressZoneRow {
    std::uint32_t id = 0;
    std::string name, event, structure, world; // STRUCTURE_POS_JA_BARRICADE_01, BARRICADE, STRUCTURE_BARRICADE_05, INS_FORT_JA
    bool placed = false;
    std::uint16_t region = 0;
    float x = 0, y = 0, z = 0, yaw = 0;
};

// worldmapguidedata.txt: the world map's guide (1.188's CNIFWorldMapGuide). Its tabs are its sections in service (AREA the
// places, MONSTER the monsters; QUEST is out of service), each with its tab's art and label key; their rows in service in
// the file's order, a `main` row a heading and the `sub` rows after it its entries, listed by the player's race's order
// column (0 Chinese, 1 European). AREA cols: 0 service, 1 a name, 2 codename, 3 its text key, 4 its area, 5 main / sub,
// 6..8 the orders, 9 the link's kind (0 a map id, 1 a worldmap_localinfo row), 10 the link. MONSTER cols: 0 service,
// 1 a name, 2 codename (a heading: an area's, its text UIIT_STT_<codename>; an entry: the monster), 3 its area, 4 main /
// sub, 5..7 the orders, 8 9 its pixel on the world map, 10 11 its region (-1: in a dungeon), 12 13 its place there (the
// region's offset; in a dungeon the map's pixel), 14 its areas' keys ("0x80000005,0x80000003": the 32-bit ARGB of its
// pixels in its tiles' pictures, interface\worldmap\monster\<area>_<x>x<z>.ddj). worldmapguidedata_region.txt: a
// monster's map tiles (field: each tile 4 x 4 regions, by its region "XxZ").
struct WorldMapGuideRow {
    std::string codename, nameKey, area;
    bool heading = false;
    std::array<int, 3> order{};
    int linkKind = 0, link = 0;                        // AREA
    int regionX = -1, regionZ = -1, placeX = 0, placeY = 0; // MONSTER
    std::vector<std::uint32_t> keys;                        // MONSTER: its areas' pixels in its tiles' pictures
    bool icon = false; // QUEST: a place (wmap_quest_icon, `codename` its tooltip key), else its monsters ("A,B" in `codename`)
};
struct WorldMapGuideTab {
    std::string section, art, labelKey;
    std::vector<WorldMapGuideRow> rows;
};
struct WorldMapGuide {
    std::vector<WorldMapGuideTab> tabs; // in service, in the file's order
    std::unordered_map<std::string, std::vector<std::pair<int, int>>> monsterTiles; // codename -> its tiles (x, z)
    // The QUEST section (no tab in 1.188; the quest list's map button reads it): a quest's codename -> its rows.
    std::unordered_map<std::string, std::vector<WorldMapGuideRow>> quests;
    // An area's map: the world for "field", else the AREA row of that area linking a map.
    int MapOfArea(const std::string& area) const;
};
void DecodeWorldMapGuide(std::string_view text, std::string_view regions, WorldMapGuide& out);

struct WorldMapData {
    WorldMapGuide guide;
    std::vector<WorldMapRow> maps;
    std::vector<DungeonMapRow> dungeons;
    std::vector<WorldMapLabel> labels; // in service, with a place
    std::vector<ReturnPointRow> returnPoints; // in service
    std::vector<FortressMapPlace> fortresses; // in service
    std::vector<FortressBlessRow> blessings;  // in service
    std::vector<FortressForgeRow> forges;     // in service
    std::vector<FortressDungeonRow> caves;    // in service
    std::vector<FortressZoneRow> zones;       // in service: BARRICADE and HEADQUARTERS

    const WorldMapRow* Map(int id) const;
    const DungeonMapRow* Dungeon(int id) const;
    // The floors of the dungeon with this region id (several floors may share one: Donhwang's cave).
    std::vector<const DungeonMapRow*> DungeonsOf(std::uint16_t region) const;
    // The floor of the same dungeon numbered `floor` (none: no such floor).
    const DungeonMapRow* Floor(const DungeonMapRow& of, int floor) const;
};

void DecodeWorldMapInfo(std::string_view text, WorldMapData& out);
void DecodeWorldMapLabels(std::string_view text, WorldMapData& out);
void DecodeReturnPoints(std::string_view text, WorldMapData& out);
void DecodeFortressMapPlaces(std::string_view text, WorldMapData& out);
void DecodeFortressTables(std::string_view blessings, std::string_view forges, WorldMapData& out);
void DecodeFortressDungeons(std::string_view text, WorldMapData& out);
// eventzonedata.txt's BARRICADE and HEADQUARTERS rows in service, placed by objectstring.ifo's spots of their names.
void DecodeFortressZones(std::string_view text, const std::vector<NamedSpot>& spots, WorldMapData& out);
// Both files of `fs` (their rows replace `out`'s); false when worldmap_mapinfo.txt is missing.
bool LoadWorldMapData(const FileSystem& fs, WorldMapData& out, std::string& error);

} // namespace sr::assets
