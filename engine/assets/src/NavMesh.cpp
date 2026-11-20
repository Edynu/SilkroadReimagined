#include "sr/assets/NavMesh.h"

#include <bit>
#include <cstdio>

#include "sr/core/BinaryReader.h"

namespace sr::assets {

namespace {

constexpr std::size_t kSignatureSize = 12;
constexpr std::size_t kFixedTail = std::size_t{kNavTiles} * kNavTiles * 8 + std::size_t{kNavHeights} * kNavHeights * 4 +
                                   kNavBlocks * kNavBlocks * 5;
constexpr std::size_t kOldFixedTail = kFixedTail - std::size_t{kNavTiles} * kNavTiles * 4;

void ReadEdge(core::BinaryReader& r, RegionNavData::Edge& e, bool border) {
    e.ax = r.F32();
    e.az = r.F32();
    e.bx = r.F32();
    e.bz = r.F32();
    e.flags = r.U8();
    e.sideA = r.U8();
    e.sideB = r.U8();
    e.cellA = r.U16();
    e.cellB = r.U16();
    if (border) {
        e.regionA = r.U16();
        e.regionB = r.U16();
    }
}

} // namespace

std::string RegionNavMeshFile(std::uint16_t regionId) {
    char name[32];
    std::snprintf(name, sizeof(name), "navmesh\\nv_%04x.nvm", static_cast<unsigned>(regionId));
    return name;
}

// .nvm (JMXVNVM 1000), little-endian, read in this order (the client's loader):
//   char[12] signature
//   u16 objectCount, per object: u32 id, f32 x, y, z, u16, f32 yaw, u16 uid, u16, u16, u16 owner region,
//       u16 linkCount, per link: u16 object, u16 its outline edge, u16 own outline edge
//   u32 cellCount, u32 openCells, per cell: f32 minX, minZ, maxX, maxZ, u8 n, n x u16 object index
//   u32 borderCount, per edge: f32 ax, az, bx, bz, u8 flags, u8 sideA, u8 sideB, u16 cellA, u16 cellB,
//       u16 regionA, u16 regionB
//   u32 internalCount, per edge: the same without the two regions
//   96 x 96 tiles: u16 cell, u16 (0), u16 flags, u16 tile2d index
//   97 x 97 f32 heights, 6 x 6 u8 block flags, 6 x 6 f32 water heights
// 101 files of the archive (none of them a region mapinfo.mfo enables) are an older layout with
// 4-byte tiles (60 of them also without the water blocks); they are rejected.
bool DecodeRegionNavMesh(std::span<const std::byte> file, RegionNavData& out, std::string& error) {
    out = RegionNavData{};
    core::BinaryReader r(file);
    if (!r.Matches("JMXVNVM 1000")) {
        error = "not a JMXVNVM 1000 file";
        return false;
    }
    r.Seek(kSignatureSize);

    const std::uint16_t objectCount = r.U16();
    out.objects.resize(objectCount);
    for (RegionNavData::Object& o : out.objects) {
        o.objectId = r.U32();
        for (float& v : o.position) v = r.F32();
        o.field10 = r.U16();
        o.yaw = r.F32();
        o.uid = r.U16();
        o.field18 = r.U16();
        o.field1A = r.U16();
        o.ownerRegion = r.U16();
        o.linkCount = r.U16();
        o.firstLink = static_cast<std::uint32_t>(out.links.size());
        if (!r.Ok() || std::size_t{o.linkCount} * 6 > r.Remaining()) {
            error = "truncated object records";
            return false;
        }
        for (std::uint16_t i = 0; i < o.linkCount; ++i) {
            RegionNavData::Link link;
            link.object = r.U16();
            link.objectEdge = r.U16();
            link.ownEdge = r.U16();
            if (link.object != 0xFFFF && link.object >= objectCount) {
                error = "link to an object out of range";
                return false;
            }
            out.links.push_back(link);
        }
    }

    const std::uint32_t cellCount = r.U32();
    out.openCells = r.U32();
    if (!r.Ok() || cellCount == 0 || cellCount > 0xFFFF || out.openCells > cellCount ||
        std::size_t{cellCount} * 17 > r.Remaining()) {
        error = "implausible cell counts";
        return false;
    }
    out.cells.resize(cellCount);
    for (RegionNavData::Cell& c : out.cells) {
        c.minX = r.F32();
        c.minZ = r.F32();
        c.maxX = r.F32();
        c.maxZ = r.F32();
        c.objectCount = r.U8();
        c.firstObject = static_cast<std::uint32_t>(out.cellObjects.size());
        for (std::uint8_t i = 0; i < c.objectCount && r.Ok(); ++i) {
            const std::uint16_t object = r.U16();
            if (object >= objectCount) {
                error = "cell lists an object out of range";
                return false;
            }
            out.cellObjects.push_back(object);
        }
    }

    const auto readEdges = [&](std::vector<RegionNavData::Edge>& edges, bool border) {
        const std::uint32_t count = r.U32();
        const std::size_t size = border ? 27 : 23;
        if (!r.Ok() || std::size_t{count} * size > r.Remaining()) return false;
        edges.resize(count);
        for (RegionNavData::Edge& e : edges) {
            ReadEdge(r, e, border);
            if (e.cellA >= cellCount || e.sideA > 3) return false;
            if (!border && e.cellB != 0xFFFF && (e.cellB >= cellCount || e.sideB > 3)) return false;
        }
        return r.Ok();
    };
    if (!readEdges(out.borderEdges, true) || !readEdges(out.internalEdges, false)) {
        error = "bad edges";
        return false;
    }

    if (r.Remaining() == kOldFixedTail || r.Remaining() == kOldFixedTail - kNavBlocks * kNavBlocks * 5) {
        error = "older layout with 4-byte tiles";
        return false;
    }
    if (r.Remaining() != kFixedTail) {
        error = "the fixed blocks do not end at the end of the file";
        return false;
    }
    out.tiles.resize(std::size_t{kNavTiles} * kNavTiles);
    for (RegionNavData::Tile& t : out.tiles) {
        t.cell = r.U16();
        r.U16();
        t.flags = r.U16();
        t.tile2d = r.U16();
        if (t.cell >= cellCount) {
            error = "tile cell out of range";
            return false;
        }
    }
    out.heights.resize(std::size_t{kNavHeights} * kNavHeights);
    for (float& h : out.heights) h = r.F32();
    for (std::uint8_t& b : out.blockFlags) b = r.U8();
    for (float& h : out.waterHeights) h = r.F32();
    if (!r.Ok() || r.Remaining() != 0) {
        error = "truncated fixed blocks";
        return false;
    }
    return true;
}

std::size_t NavMapInfo::Count() const {
    std::size_t n = 0;
    for (std::uint8_t b : bits) n += static_cast<std::size_t>(std::popcount(b));
    return n;
}

bool DecodeNavMapInfo(std::span<const std::byte> file, NavMapInfo& out, std::string& error) {
    out = NavMapInfo{};
    core::BinaryReader r(file);
    if (!r.Matches("JMXVMFO 1000")) {
        error = "not a JMXVMFO 1000 file";
        return false;
    }
    r.Seek(kSignatureSize);
    const std::uint16_t width = r.U16(), height = r.U16();
    r.Skip(8);
    if (!r.Ok() || width != 256 || height != 128 || r.Remaining() < 8192) {
        error = "unexpected mapinfo header";
        return false;
    }
    const auto bits = r.Bytes(8192);
    out.bits.resize(bits.size());
    for (std::size_t i = 0; i < bits.size(); ++i) out.bits[i] = static_cast<std::uint8_t>(bits[i]);
    return true;
}

} // namespace sr::assets
