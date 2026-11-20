#include "sr/assets/Terrain.h"

#include <bit>
#include <charconv>
#include <cstring>

#include "sr/assets/Texture.h"
#include "sr/core/BinaryReader.h"

namespace sr::assets {

namespace {

constexpr std::size_t kSignatureSize = 12;
constexpr int kLodGroups = 4;

// Splits "00012 0x0000000a "a" "b" {..}" into its numeric and quoted fields.
struct IndexLine {
    std::uint32_t id = 0;
    std::uint32_t flags = 0;
    std::vector<std::string> quoted;
};

bool ParseIndexLine(std::string_view line, IndexLine& out) {
    out = IndexLine{};
    std::size_t pos = 0;
    const auto skipSpace = [&] {
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    };
    const auto number = [&](std::uint32_t& value) {
        skipSpace();
        int base = 10;
        if (line.substr(pos, 2) == "0x" || line.substr(pos, 2) == "0X") {
            pos += 2;
            base = 16;
        }
        const auto [end, ec] = std::from_chars(line.data() + pos, line.data() + line.size(), value, base);
        if (ec != std::errc{}) return false;
        pos = static_cast<std::size_t>(end - line.data());
        return true;
    };
    if (!number(out.id) || !number(out.flags)) return false;
    for (;;) {
        skipSpace();
        if (pos >= line.size() || line[pos] != '"') break;
        const std::size_t close = line.find('"', pos + 1);
        if (close == std::string_view::npos) return false;
        out.quoted.emplace_back(line.substr(pos + 1, close - pos - 1));
        pos = close + 1;
    }
    return !out.quoted.empty();
}

// Shared reader for the two text indexes: signature line, count line, rows.
template <typename RowFn>
bool ParseTextIndex(std::span<const std::byte> file, std::string_view signature, AssetIndex& out, std::string& error,
                    RowFn row) {
    out.clear();
    const std::string_view text(reinterpret_cast<const char*>(file.data()), file.size());
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (!line.empty()) lines.push_back(line);
        start = end + 1;
    }
    if (lines.size() < 2 || lines[0] != signature) {
        error = "missing " + std::string(signature) + " header";
        return false;
    }
    std::uint32_t declared = 0;
    std::from_chars(lines[1].data(), lines[1].data() + lines[1].size(), declared);
    for (std::size_t i = 2; i < lines.size(); ++i) {
        IndexLine parsed;
        if (!ParseIndexLine(lines[i], parsed)) {
            error = "bad row " + std::to_string(i + 1) + ": " + std::string(lines[i]);
            return false;
        }
        out[parsed.id] = row(parsed);
    }
    if (out.size() != declared) {
        error = "declared " + std::to_string(declared) + " rows, found " + std::to_string(out.size());
        return false;
    }
    return true;
}

} // namespace

std::string RegionFile(std::uint16_t regionId, std::string_view extension) {
    return std::to_string(regionId >> 8) + '\\' + std::to_string(regionId & 0xFF) + std::string(extension);
}

// .m (JMXVMAPM1000): 36 blocks of 2575 bytes, z-major:
//   u32 flags, u16 environmentId,
//   289 x { f32 height, u16 texture, u8 brightness },
//   i8 waterType, u8 waterCoverage, f32 waterHeight,
//   256 x u16 tileFlags, f32 heightMax, f32 heightMin, 20 reserved bytes
bool DecodeMapMesh(std::span<const std::byte> file, TerrainRegion& out, std::string& error) {
    constexpr std::size_t kBlockBytes = 2575;
    core::BinaryReader r(file);
    if (!r.Matches("JMXVMAPM1000")) {
        error = "not a JMXVMAPM1000 file";
        return false;
    }
    if (file.size() != kSignatureSize + out.blocks.size() * kBlockBytes) {
        error = "unexpected .m size";
        return false;
    }
    r.Seek(kSignatureSize);
    for (TerrainBlock& block : out.blocks) {
        block.flags = r.U32();
        block.environmentId = r.U16();
        for (std::size_t v = 0; v < block.heights.size(); ++v) {
            block.heights[v] = r.F32();
            block.textures[v] = r.U16();
            block.brightness[v] = r.U8();
        }
        block.waterType = static_cast<std::int8_t>(r.U8());
        block.waterCoverage = r.U8();
        block.waterHeight = r.F32();
        for (auto& f : block.tileFlags) f = r.U16();
        block.heightMax = r.F32();
        block.heightMin = r.F32();
        r.Skip(20);
    }
    if (!r.Ok() || r.Remaining() != 0) {
        error = "truncated .m";
        return false;
    }
    return true;
}

// .o2 (JMXVMAPO1001): for each of the 36 blocks (z-major), 4 LOD groups of
//   u16 count, count x { u32 objectId, f32 x, f32 y, f32 z, i16 isStatic, f32 yaw,
//   i16 uid, i16 unknown, u8 isBig, u8 isStruct, u16 regionId }   (30 bytes)
// An object is listed in every block it covers, in its own region's file and its neighbours' (a
// big building some 300 times); regionId and uid name it: v1.150's 2,123 files hold 111,272
// listings of 43,119 objects, no two of them under one name.
bool DecodeMapObjects(std::span<const std::byte> file, std::vector<ObjectPlacement>& out, std::string& error) {
    out.clear();
    core::BinaryReader r(file);
    if (!r.Matches("JMXVMAPO1001")) {
        error = "not a JMXVMAPO1001 file";
        return false;
    }
    r.Seek(kSignatureSize);
    for (int block = 0; block < kBlocksPerRegion * kBlocksPerRegion; ++block) {
        for (int lod = 0; lod < kLodGroups; ++lod) {
            const std::uint16_t count = r.U16();
            for (std::uint16_t i = 0; i < count && r.Ok(); ++i) {
                ObjectPlacement p;
                p.objectId = r.U32();
                for (float& v : p.position) v = r.F32();
                p.isStatic = static_cast<std::int16_t>(r.U16());
                p.yaw = r.F32();
                p.uid = static_cast<std::int16_t>(r.U16());
                r.U16();
                p.isBig = r.U8() != 0;
                p.isStruct = r.U8() != 0;
                p.regionId = r.U16();
                p.block = static_cast<std::uint8_t>(block);
                p.lodGroup = static_cast<std::uint8_t>(lod);
                out.push_back(p);
            }
        }
    }
    if (!r.Ok() || r.Remaining() != 0) {
        error = "object records do not end at the end of the file";
        return false;
    }
    return true;
}

// object.ifo rows: id flags "path"
bool DecodeObjectIndex(std::span<const std::byte> file, AssetIndex& out, std::string& error) {
    return ParseTextIndex(file, "JMXVOBJI1000", out, error,
                          [](const IndexLine& l) { return IndexEntry{l.flags, {}, l.quoted[0]}; });
}

// tile2d.ifo rows: id flags "category" "file.ddj" [{x,y} ...]; files live in Map.pk2\tile2d\.
bool DecodeTileIndex(std::span<const std::byte> file, AssetIndex& out, std::string& error) {
    return ParseTextIndex(file, "JMXV2DTI1001", out, error, [](const IndexLine& l) {
        return IndexEntry{l.flags, l.quoted[0], l.quoted.size() > 1 ? "tile2d\\" + l.quoted[1] : std::string()};
    });
}

bool DecodeObjectStrings(std::span<const std::byte> file, std::vector<NamedSpot>& out, std::string& error) {
    out.clear();
    const std::string_view text(reinterpret_cast<const char*>(file.data()), file.size());
    std::vector<std::string_view> lines;
    for (std::size_t start = 0; start < text.size();) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (!line.empty()) lines.push_back(line);
        start = end + 1;
    }
    if (lines.size() < 2 || lines[0] != "JMXVOBJI1000") {
        error = "missing JMXVOBJI1000 header";
        return false;
    }
    std::uint32_t declared = 0;
    std::from_chars(lines[1].data(), lines[1].data() + lines[1].size(), declared);
    for (std::size_t i = 2; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        std::size_t pos = 0;
        const auto number = [&](std::uint32_t& value) {
            while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
            int base = 10;
            if (line.substr(pos, 2) == "0x" || line.substr(pos, 2) == "0X") {
                pos += 2;
                base = 16;
            }
            const auto [end, ec] = std::from_chars(line.data() + pos, line.data() + line.size(), value, base);
            if (ec != std::errc{}) return false;
            pos = static_cast<std::size_t>(end - line.data());
            return true;
        };
        NamedSpot spot;
        std::uint32_t rx = 0, rz = 0, bits[4] = {};
        bool ok = number(spot.id) && number(spot.flags) && number(rx) && number(rz) && rx < 256 && rz < 256;
        for (std::uint32_t& b : bits) ok = ok && number(b);
        const std::size_t open = line.find('"', pos);
        const std::size_t close = open == std::string_view::npos ? open : line.find('"', open + 1);
        if (!ok || close == std::string_view::npos) {
            error = "bad row " + std::to_string(i + 1) + ": " + std::string(line);
            return false;
        }
        spot.region = RegionId(int(rx), int(rz));
        spot.x = std::bit_cast<float>(bits[0]);
        spot.y = std::bit_cast<float>(bits[1]);
        spot.z = std::bit_cast<float>(bits[2]);
        spot.yaw = std::bit_cast<float>(bits[3]);
        spot.name = std::string(line.substr(open + 1, close - open - 1));
        out.push_back(std::move(spot));
    }
    if (out.size() != declared) {
        error = "declared " + std::to_string(declared) + " rows, found " + std::to_string(out.size());
        return false;
    }
    return true;
}

bool DecodeMapLightmap(std::span<const std::byte> file, TextureData& out, std::string& error) {
    core::BinaryReader r(file);
    if (!r.Matches("JMXVMAPT1001")) {
        error = "not a JMXVMAPT1001 file";
        return false;
    }
    r.Skip(kSignatureSize + 36 * 256);
    r.U32(); // the record's length; not trusted: the image is the rest of the file
    r.U32(); // type
    if (!r.Ok() || r.Remaining() == 0) {
        error = "lightmap record is truncated";
        return false;
    }
    return DecodeDds(r.Bytes(r.Remaining()), out, error);
}

} // namespace sr::assets
