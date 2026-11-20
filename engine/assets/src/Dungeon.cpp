#include "sr/assets/Dungeon.h"

#include <algorithm>
#include <charconv>
#include <string_view>

#include "sr/core/BinaryReader.h"

namespace sr::assets {

namespace {

constexpr std::size_t kSignatureSize = 12;
constexpr std::uint32_t kMaxList = 100000;

void ReadFloats(core::BinaryReader& r, float* out, int count) {
    for (int i = 0; i < count; ++i) out[i] = r.F32();
}

bool ReadIndices(core::BinaryReader& r, std::vector<std::uint32_t>& out) {
    const std::uint32_t n = r.U32();
    if (!r.Ok() || n > kMaxList || std::size_t{n} * 4 > r.Remaining()) return false;
    out.resize(n);
    for (std::uint32_t& v : out) v = r.U32();
    return r.Ok();
}

bool ReadBlock(core::BinaryReader& r, DungeonData::Block& b) {
    b.path = r.CountedString();
    b.name = r.CountedString();
    r.U32();
    ReadFloats(r, b.position, 3);
    b.yaw = r.F32();
    b.flags = r.U32();
    ReadFloats(r, b.boundsMin, 3);
    ReadFloats(r, b.boundsMax, 3);
    b.ambient = r.U32();
    b.fog.color = r.U32();
    b.fog.start = r.F32();
    b.fog.end = r.F32();
    b.fog.density = r.F32();
    b.fog.heightFog = r.U8() != 0;
    if (b.fog.heightFog) ReadFloats(r, b.fog.height, 4);
    b.motion = r.U8() != 0;
    if (b.motion) {
        ReadFloats(r, b.motionVector, 3);
        ReadFloats(r, b.motionAngles, 3);
        b.motionPeriod = r.U32();
    }
    r.CountedString();
    b.room = static_cast<std::int32_t>(r.U32());
    b.floor = static_cast<std::int32_t>(r.U32());
    if (!ReadIndices(r, b.neighbours) || !ReadIndices(r, b.visible)) return false;
    const std::uint32_t objects = r.U32();
    b.obstacles = r.U32();
    if (!r.Ok() || objects > kMaxList) return false;
    b.objects.resize(objects);
    for (DungeonData::Object& o : b.objects) {
        o.name = r.CountedString();
        o.path = r.CountedString();
        ReadFloats(r, o.position, 3);
        ReadFloats(r, o.rotation, 3);
        ReadFloats(r, o.scale, 3);
        o.flags = r.U32();
        o.index = r.U32();
        o.value = r.F32();
        if (o.flags & 4) o.color = r.U32();
        if (!r.Ok()) return false;
    }
    const std::uint32_t lights = r.U32();
    if (!r.Ok() || lights > kMaxList) return false;
    b.lights.resize(lights);
    for (DungeonData::Light& l : b.lights) {
        l.name = r.CountedString();
        ReadFloats(r, l.position, 3);
        ReadFloats(r, l.diffuse, 3);
        ReadFloats(r, l.ambient, 3);
        ReadFloats(r, l.specular, 3);
        ReadFloats(r, l.attenuation, 3);
    }
    return r.Ok();
}

} // namespace

bool DecodeDungeonInfo(std::span<const std::byte> file, std::vector<DungeonInfoEntry>& out, std::string& error) {
    out.clear();
    const std::string_view text(reinterpret_cast<const char*>(file.data()), file.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty() || line.starts_with("//")) continue;
        unsigned service = 0, id = 0;
        const char* const last = line.data() + line.size();
        const auto [serviceEnd, ec1] = std::from_chars(line.data(), last, service);
        const char* idStart = serviceEnd;
        while (idStart < last && (*idStart == '\t' || *idStart == ' ')) ++idStart;
        const auto [idEnd, ec2] = std::from_chars(idStart, last, id);
        const std::size_t open = line.find('"'), close = line.rfind('"');
        if (ec1 != std::errc{} || ec2 != std::errc{} || idStart == serviceEnd || service > 1 || id > 0x7FFF ||
            open == std::string_view::npos || close <= open || static_cast<std::size_t>(idEnd - line.data()) > open) {
            error = "bad line: " + std::string(line);
            return false;
        }
        out.push_back({service == 1, static_cast<std::uint16_t>(0x8000 | id), std::string(line.substr(open + 1, close - open - 1))});
    }
    return true;
}

// .dof (JMXVDOF 0101), little-endian; a string is u32 length + bytes (CP949):
//   char[12] signature
//   u32 offsets: blocks, links, grid, block groups, name lists (0 none), two unused, bounding box
//   u32 resource id, string name, u32, u32, u16 region id
//   bounding box: f32 min xyz, max xyz
//   blocks: u32 count, per block: string .bsr, string name, u32, f32 x, y, z, f32 yaw, u32 flags,
//     f32 bounds min xyz, max xyz, u32 ambient, u32 fog colour, f32 fog start, end, density,
//     u8 (1: f32 x 4 height fog), u8 (non-zero: f32 x 3 vector, f32 x 3 angles, u32 period ms),
//     string, i32 room, i32 floor, u32 n + n u32 neighbours, u32 n + n u32 visible,
//     u32 objects, u32 obstacles, per object: string name, string .bsr, f32 x 3 position,
//       f32 x 3 rotation, f32 x 3 scale, u32 flags, u32 light index, f32 value, (flags & 4) u32 colour,
//     u32 lights, per light: string name, f32 x 3 position, diffuse, ambient, specular, attenuation
//   links: u32 count (= blocks), per block u32 n + n u32
//   grid: u32 nx, ny, nz, u32 cells, per cell u32 key, u32 n + n u32 blocks
//   block groups: u32 count, per group string name, u32 flags, u32 n + n u32
//   name lists: two lists of u32 count + strings (room names, floor names)
bool DecodeDof(std::span<const std::byte> file, DungeonData& out, std::string& error) {
    out = DungeonData{};
    core::BinaryReader r(file);
    if (!r.Matches("JMXVDOF 0101")) {
        error = "not a JMXVDOF 0101 file";
        return false;
    }
    r.Seek(kSignatureSize);
    std::uint32_t offsets[8];
    for (std::uint32_t& o : offsets) o = r.U32();
    const std::uint32_t blocksAt = offsets[0], linksAt = offsets[1], gridAt = offsets[2], groupsAt = offsets[3],
                        namesAt = offsets[4], boxAt = offsets[7];
    r.U32(); // resource id
    r.CountedString();
    r.U32();
    r.U32();
    out.regionId = r.U16();
    if (!r.Ok()) {
        error = "truncated header";
        return false;
    }
    for (std::uint32_t o : offsets) {
        if (o > file.size()) {
            error = "section offset outside the file";
            return false;
        }
    }
    // A section ends where the next one starts (or at the end of the file).
    const auto sectionEnd = [&](std::uint32_t start) {
        std::size_t end = file.size();
        for (std::uint32_t o : offsets) {
            if (o > start && o < end) end = o;
        }
        return end;
    };
    const auto section = [&](std::uint32_t start) { return core::BinaryReader(file.first(sectionEnd(start)), start); };

    core::BinaryReader box(file, boxAt);
    ReadFloats(box, out.boundsMin, 3);
    ReadFloats(box, out.boundsMax, 3);
    if (boxAt == 0 || !box.Ok()) {
        error = "no bounding box";
        return false;
    }

    core::BinaryReader b = section(blocksAt);
    const std::uint32_t count = b.U32();
    if (blocksAt == 0 || !b.Ok() || count > 10000) {
        error = "bad block count";
        return false;
    }
    out.blocks.resize(count);
    for (DungeonData::Block& block : out.blocks) {
        if (!ReadBlock(b, block)) break;
    }
    if (!b.Ok() || b.Remaining() != 0) {
        error = "blocks not read exactly (an older revision?)";
        return false;
    }
    for (const DungeonData::Block& block : out.blocks) {
        for (const auto* list : {&block.neighbours, &block.visible}) {
            if (std::any_of(list->begin(), list->end(), [&](std::uint32_t i) { return i >= count; })) {
                error = "block list out of range";
                return false;
            }
        }
    }

    core::BinaryReader l = section(linksAt);
    if (linksAt == 0 || l.U32() != count) {
        error = "links do not match the blocks";
        return false;
    }
    out.links.resize(count);
    for (auto& list : out.links) {
        if (!ReadIndices(l, list)) break;
    }
    if (!l.Ok() || l.Remaining() != 0) {
        error = "links not read exactly";
        return false;
    }

    core::BinaryReader g = section(gridAt);
    for (std::uint32_t& n : out.gridSize) n = g.U32();
    const std::uint32_t cells = g.U32();
    if (gridAt == 0 || !g.Ok() || cells > kMaxList) {
        error = "bad grid";
        return false;
    }
    out.grid.resize(cells);
    for (DungeonData::GridCell& cell : out.grid) {
        cell.key = g.U32();
        if (!ReadIndices(g, cell.blocks)) break;
    }
    if (!g.Ok() || g.Remaining() != 0) {
        error = "grid not read exactly";
        return false;
    }

    if (groupsAt != 0) {
        core::BinaryReader s = section(groupsAt);
        const std::uint32_t groups = s.U32();
        if (!s.Ok() || groups > kMaxList) {
            error = "bad block groups";
            return false;
        }
        out.groups.resize(groups);
        for (DungeonData::Group& group : out.groups) {
            group.name = s.CountedString();
            group.flags = s.U32();
            if (!ReadIndices(s, group.blocks)) break;
        }
        if (!s.Ok() || s.Remaining() != 0) {
            error = "block groups not read exactly";
            return false;
        }
    }

    if (namesAt != 0) {
        core::BinaryReader s = section(namesAt);
        for (auto* list : {&out.roomNames, &out.floorNames}) {
            const std::uint32_t n = s.U32();
            if (!s.Ok() || n > kMaxList) break;
            for (std::uint32_t i = 0; i < n && s.Ok(); ++i) list->push_back(s.CountedString());
        }
        if (!s.Ok() || s.Remaining() != 0) {
            error = "name lists not read exactly";
            return false;
        }
    }
    return true;
}

} // namespace sr::assets
