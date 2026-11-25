#include "sr/assets/WorldMap.h"

#include <cstdlib>
#include <format>

#include "sr/assets/FileSystem.h"
#include "sr/core/Text.h"

namespace sr::assets {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

int Int(std::string_view s) { return static_cast<int>(std::strtol(std::string(Trim(s)).c_str(), nullptr, 10)); }

bool Number(std::string_view s) {
    s = Trim(s);
    if (!s.empty() && s.front() == '-') s.remove_prefix(1);
    if (s.empty()) return false;
    for (const char ch : s) {
        if (ch < '0' || ch > '9') return false;
    }
    return true;
}

// The text's lines split at tabs, a byte order mark dropped.
template <typename F>
void Lines(std::string_view text, F&& f) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    for (std::size_t at = 0; at < text.size();) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        std::vector<std::string_view> c;
        for (std::size_t p = 0; p <= line.size();) {
            std::size_t tab = line.find('\t', p);
            if (tab == std::string_view::npos) tab = line.size();
            c.push_back(line.substr(p, tab - p));
            p = tab + 1;
        }
        f(c);
    }
}

std::string Text(std::string_view s) {
    s = Trim(s);
    return s == "xxx" ? std::string() : std::string(s);
}

} // namespace

std::string WorldMapRow::Tile(int i, int j) const {
    const int band = tileRegions > 0 ? tileRegions : 1;
    return std::format("{}{}x{}.ddj", picture, regionLeft + i * band, regionTop - j * band);
}

std::string DungeonMapRow::Label() const { return letter == 'F' ? std::format("{}F", floor) : std::format("{}{}", letter, floor); }

std::string DungeonMapRow::Tile(int i, int j) const { return std::format("{}{}x{}.ddj", prefix, regionLeft + i, regionTop - j); }

const WorldMapRow* WorldMapData::Map(int id) const {
    for (const WorldMapRow& m : maps) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

const DungeonMapRow* WorldMapData::Dungeon(int id) const {
    for (const DungeonMapRow& d : dungeons) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

std::vector<const DungeonMapRow*> WorldMapData::DungeonsOf(std::uint16_t region) const {
    std::vector<const DungeonMapRow*> out;
    for (const DungeonMapRow& d : dungeons) {
        if (d.region == region) out.push_back(&d);
    }
    return out;
}

const DungeonMapRow* WorldMapData::Floor(const DungeonMapRow& of, int floor) const {
    for (const DungeonMapRow& d : dungeons) {
        if (d.name == of.name && d.letter == of.letter && d.floors == of.floors && d.floor == floor) return &d;
    }
    return nullptr;
}

void DecodeWorldMapInfo(std::string_view text, WorldMapData& out) {
    out.maps.clear();
    out.dungeons.clear();
    int section = 0; // 1 WLocalmap, 2 Dungeonmap
    Lines(text, [&](const std::vector<std::string_view>& c) {
        if (c[0] == "#section" && c.size() > 1) {
            const std::string_view name = Trim(c[1]);
            section = name == "WLocalmap" ? 1 : name == "Dungeonmap" ? 2 : 0;
            return;
        }
        if (!Number(c[0])) return; // notes, comments, blank lines
        if (section == 1 && c.size() >= 21) {
            WorldMapRow m;
            m.id = Int(c[0]);
            m.tiled = Int(c[1]) == 0;
            m.picture = Text(c[3]);
            m.textureW = Int(c[4]);
            m.textureH = Int(c[5]);
            m.pictureW = Int(c[6]);
            m.pictureH = Int(c[7]);
            m.drawW = Int(c[8]);
            m.drawH = Int(c[9]);
            m.regionLeft = Int(c[10]);
            m.regionTop = Int(c[11]);
            m.regionRight = Int(c[12]);
            m.regionBottom = Int(c[13]);
            m.leftX = Int(c[14]);
            m.topY = Int(c[15]);
            m.rightX = Int(c[16]);
            m.bottomY = Int(c[17]);
            m.fortress = Text(c[18]);
            m.nameKey = Text(c[19]);
            m.place = Int(c[20]) != 0;
            if (c.size() > 21) {
                const std::string_view tiles = Trim(c[21]);
                if (const std::size_t x = tiles.find('x'); x != std::string_view::npos) m.tileRegions = Int(tiles.substr(0, x));
            }
            out.maps.push_back(std::move(m));
        } else if (section == 2 && c.size() >= 18) {
            DungeonMapRow d;
            d.id = Int(c[0]);
            d.name = std::string(Trim(c[1]));
            const std::string_view letter = Trim(c[2]);
            d.letter = letter.empty() ? 'F' : letter[0];
            d.floor = Int(c[3]);
            d.floors = Int(c[4]);
            d.region = static_cast<std::uint16_t>(Int(c[5]));
            d.tilesAcross = Int(c[6]);
            d.tilesDown = Int(c[7]);
            d.drawW = Int(c[8]);
            d.drawH = Int(c[9]);
            d.regionLeft = Int(c[10]);
            d.regionTop = Int(c[11]);
            d.regionRight = Int(c[12]);
            d.regionBottom = Int(c[13]);
            d.prefix = Text(c[15]);
            d.floorKey = Text(c[17]);
            out.dungeons.push_back(std::move(d));
        }
    });
}

void DecodeWorldMapLabels(std::string_view text, WorldMapData& out) {
    out.labels.clear();
    Lines(text, [&](const std::vector<std::string_view>& c) {
        if (c.size() < 21 || Trim(c[0]) != "1" || !Number(c[1])) return; // out of service, notes
        WorldMapLabel l;
        l.id = Int(c[1]);
        l.picture = Int(c[2]) == 2;
        l.what = Text(c[3]);
        if (l.what.empty() || !Number(c[8])) return; // the NPCs' name rows (90000..): no place
        l.opens = Int(c[7]);
        l.map = Int(c[8]);
        l.regionX = Int(c[9]);
        l.regionZ = Int(c[10]);
        l.x = Int(c[11]);
        l.y = Int(c[12]);
        l.width = Int(c[13]);
        l.height = Int(c[14]);
        l.red = static_cast<std::uint8_t>(Int(c[15]));
        l.green = static_cast<std::uint8_t>(Int(c[16]));
        l.blue = static_cast<std::uint8_t>(Int(c[17]));
        l.shadow = Int(c[18]) != 0;
        l.bold = Int(c[19]) != 0;
        l.blur = Int(c[20]) != 0;
        if (c.size() > 21) l.tooltipKey = Text(c[21]);
        out.labels.push_back(std::move(l));
    });
}

void DecodeReturnPoints(std::string_view text, WorldMapData& out) {
    out.returnPoints.clear();
    Lines(text, [&](const std::vector<std::string_view>& c) {
        if (c.size() < 13 || !Number(c[0]) || Int(c[0]) != 1 || !Number(c[1])) return;
        ReturnPointRow r;
        r.id = Int(c[1]);
        r.zoneKey = Text(c[3]);
        r.region = static_cast<std::uint16_t>(static_cast<std::int16_t>(Int(c[4])));
        r.x = static_cast<float>(std::strtod(std::string(Trim(c[5])).c_str(), nullptr));
        r.y = static_cast<float>(std::strtod(std::string(Trim(c[6])).c_str(), nullptr));
        r.z = static_cast<float>(std::strtod(std::string(Trim(c[7])).c_str(), nullptr));
        r.group = Int(c[9]);
        r.marker = Int(c[10]) == 1;
        r.levelMin = Int(c[11]);
        r.levelMax = Int(c[12]);
        out.returnPoints.push_back(std::move(r));
    });
}

void DecodeFortressMapPlaces(std::string_view text, WorldMapData& out) {
    out.fortresses.clear();
    Lines(text, [&](const std::vector<std::string_view>& c) {
        if (c.size() < 7 || !Number(c[0]) || Int(c[0]) != 1) return;
        FortressMapPlace p;
        p.fortress = Text(c[1]);
        p.town = Text(c[2]);
        p.regionX = Int(c[3]);
        p.regionZ = Int(c[4]);
        p.x = Int(c[5]);
        p.y = Int(c[6]);
        out.fortresses.push_back(std::move(p));
    });
}

void DecodeFortressTables(std::string_view blessings, std::string_view forges, WorldMapData& out) {
    out.blessings.clear();
    out.forges.clear();
    Lines(blessings, [&](const std::vector<std::string_view>& c) {
        if (c.size() < 6 || !Number(c[0]) || Int(c[0]) != 1) return;
        out.blessings.push_back({Int(c[1]), Int(c[2]), std::uint32_t(Int(c[3])), std::uint64_t(std::max(0, Int(c[4]))), std::uint32_t(std::max(0, Int(c[5])))});
    });
    Lines(forges, [&](const std::vector<std::string_view>& c) {
        if (c.size() < 6 || !Number(c[0]) || Int(c[0]) != 1) return;
        out.forges.push_back({Int(c[1]), std::uint32_t(Int(c[2])), std::uint64_t(std::max(0, Int(c[3]))), std::uint32_t(std::max(0, Int(c[4]))), Int(c[5])});
    });
}

void DecodeFortressDungeons(std::string_view text, WorldMapData& out) {
    out.caves.clear();
    Lines(text, [&](const std::vector<std::string_view>& c) {
        if (c.size() < 6 || !Number(c[0]) || Int(c[0]) != 1) return;
        out.caves.push_back({Int(c[1]), Int(c[2]), Int(c[3]), std::uint64_t(std::max(0, Int(c[4]))), std::uint32_t(std::max(0, Int(c[5])))});
    });
}

void DecodeFortressZones(std::string_view text, const std::vector<NamedSpot>& spots, WorldMapData& out) {
    out.zones.clear();
    Lines(text, [&](const std::vector<std::string_view>& c) {
        if (c.size() < 11 || !Number(c[0]) || Int(c[0]) != 1) return;
        FortressZoneRow z;
        z.id = std::uint32_t(Int(c[1]));
        z.name = Text(c[2]);
        z.event = Text(c[3]);
        if (z.event != "BARRICADE" && z.event != "HEADQUARTERS") return;
        z.structure = Text(c[9]);
        z.world = Text(c[10]);
        for (const NamedSpot& s : spots) {
            if (s.name != z.name) continue;
            z.placed = true;
            z.region = s.region;
            z.x = s.x;
            z.y = s.y;
            z.z = s.z;
            z.yaw = s.yaw;
        }
        out.zones.push_back(std::move(z));
    });
}

int WorldMapGuide::MapOfArea(const std::string& area) const {
    if (area == "field") return 0;
    for (const WorldMapGuideTab& t : tabs) {
        if (t.section != "AREA") continue;
        for (const WorldMapGuideRow& r : t.rows) {
            if (r.area == area && r.linkKind == 0 && r.link > 0) return r.link;
        }
    }
    return -1;
}

void DecodeWorldMapGuide(std::string_view text, std::string_view regions, WorldMapGuide& out) {
    out.tabs.clear();
    out.monsterTiles.clear();
    WorldMapGuideTab* tab = nullptr;
    Lines(text, [&](const std::vector<std::string_view>& c) {
        if (c.empty()) return;
        if (Trim(c[0]) == "#section") {
            // "#section  NAME  service  art  label" (MAP_MANAGER: the maps' pages, not a tab).
            tab = nullptr;
            if (c.size() < 5 || Trim(c[2]) != "1" || Trim(c[1]) == "MAP_MANAGER") return;
            out.tabs.push_back({std::string(Trim(c[1])), Text(c[3]), Text(c[4]), {}});
            tab = &out.tabs.back();
            return;
        }
        if (tab == nullptr || Trim(c[0]) != "1") return; // out of service, notes
        WorldMapGuideRow r;
        r.codename = Text(c[2]);
        if (tab->section == "AREA") {
            if (c.size() < 11) return;
            r.nameKey = Text(c[3]);
            r.area = Text(c[4]);
            r.heading = Trim(c[5]) == "main";
            for (int i = 0; i < 3; ++i) r.order[std::size_t(i)] = Int(c[6 + std::size_t(i)]);
            r.linkKind = Int(c[9]);
            r.link = Int(c[10]);
        } else if (tab->section == "MONSTER") {
            if (c.size() < 14) return;
            r.area = Text(c[3]);
            r.heading = Trim(c[4]) == "main";
            for (int i = 0; i < 3; ++i) r.order[std::size_t(i)] = Int(c[5 + std::size_t(i)]);
            r.nameKey = r.heading ? "UIIT_STT_" + r.codename : std::string();
            r.regionX = Int(c[10]);
            r.regionZ = Int(c[11]);
            r.placeX = Int(c[12]);
            r.placeY = Int(c[13]);
        } else {
            return;
        }
        if (r.codename.empty()) return;
        tab->rows.push_back(std::move(r));
    });
    Lines(regions, [&](const std::vector<std::string_view>& c) {
        if (c.size() < 2 || Text(c[0]).empty()) return;
        std::vector<std::pair<int, int>>& tiles = out.monsterTiles[Text(c[0])];
        std::string_view list = Trim(c[1]);
        for (std::size_t at = 0; at < list.size();) {
            std::size_t comma = list.find(',', at);
            if (comma == std::string_view::npos) comma = list.size();
            const std::string_view one = Trim(list.substr(at, comma - at));
            if (const std::size_t x = one.find('x'); x != std::string_view::npos) tiles.push_back({Int(one.substr(0, x)), Int(one.substr(x + 1))});
            at = comma + 1;
        }
    });
}

bool LoadWorldMapData(const FileSystem& fs, WorldMapData& out, std::string& error) {
    const auto info = fs.Read("server_dep\\silkroad\\textdata\\worldmap_mapinfo.txt");
    if (!info) {
        error = "worldmap_mapinfo.txt is missing";
        return false;
    }
    DecodeWorldMapInfo(core::DecodeTextFile(info->Bytes()), out);
    if (const auto labels = fs.Read("server_dep\\silkroad\\textdata\\worldmap_localinfo.txt")) DecodeWorldMapLabels(core::DecodeTextFile(labels->Bytes()), out);
    if (const auto points = fs.Read("server_dep\\silkroad\\textdata\\refoptionalteleport.txt")) DecodeReturnPoints(core::DecodeTextFile(points->Bytes()), out);
    if (const auto forts = fs.Read("server_dep\\silkroad\\textdata\\siegefortressreward.txt")) DecodeFortressMapPlaces(core::DecodeTextFile(forts->Bytes()), out);
    const auto bless = fs.Read("server_dep\\silkroad\\textdata\\refsiegeblessbuff.txt");
    const auto forge = fs.Read("server_dep\\silkroad\\textdata\\siegefortressitemforge.txt");
    DecodeFortressTables(bless ? core::DecodeTextFile(bless->Bytes()) : std::string(), forge ? core::DecodeTextFile(forge->Bytes()) : std::string(), out);
    if (const auto guide = fs.Read("server_dep\\silkroad\\textdata\\worldmapguidedata.txt")) {
        const auto regions = fs.Read("server_dep\\silkroad\\textdata\\worldmapguidedata_region.txt");
        DecodeWorldMapGuide(core::DecodeTextFile(guide->Bytes()), regions ? core::DecodeTextFile(regions->Bytes()) : std::string(), out.guide);
    }
    if (const auto caves = fs.Read("server_dep\\silkroad\\textdata\\refsiegedungeon.txt")) DecodeFortressDungeons(core::DecodeTextFile(caves->Bytes()), out);
    if (const auto zones = fs.Read("server_dep\\silkroad\\textdata\\eventzonedata.txt")) {
        std::vector<NamedSpot> spots;
        std::string why;
        if (const auto file = fs.Read("navmesh\\objectstring.ifo"); !file || !DecodeObjectStrings(file->Bytes(), spots, why)) spots.clear();
        DecodeFortressZones(core::DecodeTextFile(zones->Bytes()), spots, out);
    }
    return true;
}

} // namespace sr::assets
