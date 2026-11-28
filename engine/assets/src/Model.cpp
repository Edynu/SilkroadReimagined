#include "sr/assets/Model.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "sr/assets/FileSystem.h"
#include "sr/core/BinaryReader.h"
#include "sr/core/Path.h"

namespace sr::assets {

namespace {

constexpr std::size_t kSignatureSize = 12;
constexpr std::uint32_t kMaxVertices = 1'000'000;
constexpr std::uint32_t kMaxTriangles = 1'000'000;
constexpr std::uint32_t kMaxBones = 256;
constexpr std::uint32_t kMaxMaterials = 1024;
constexpr std::uint32_t kMaxEntries = 512;
constexpr std::uint32_t kFlagLightmapped = 0x400;

void ReadFloats(core::BinaryReader& r, float* out, int count) {
    for (int i = 0; i < count; ++i) out[i] = r.F32();
}

std::string FolderOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

void Merge(Bounds& into, const Bounds& b, bool& first) {
    for (int i = 0; i < 3; ++i) {
        into.min[i] = first ? b.min[i] : std::min(into.min[i], b.min[i]);
        into.max[i] = first ? b.max[i] : std::max(into.max[i], b.max[i]);
    }
    first = false;
}

Bounds BoundsOf(const std::vector<MeshVertex>& vertices) {
    Bounds b;
    bool first = true;
    for (const MeshVertex& v : vertices) {
        Bounds p;
        for (int i = 0; i < 3; ++i) p.min[i] = p.max[i] = v.position[i];
        Merge(b, p, first);
    }
    return b;
}

} // namespace

// .bms layout (0109/0110):
//   0x00 "JMXVBMS 0110"
//   0x0C u32 section offsets[12]: [0] vertices, [1] skin, [2] triangles, [5] bounds
//   0x3C u32 unknown, u32 flags, u32 unknown, string meshName, string materialName, u32 sortKey
//   [0]  u32 vertexCount, vertices: pos f32x3, normal f32x3, uv f32x2,
//        (flags & 0x400: lightmap uv f32x2), u32x3 unknown;
//        lightmapped meshes then store string lightmapPath
//   [1]  u32 boneCount, strings boneNames; if bones, per vertex (0110: 6 B)
//        u8 bone0, u16 weight0, u8 bone1 (0xFF = none), u16 weight1
//   [2]  u32 triangleCount, u16 indices[3 * triangleCount]
//   [5]  bounds min f32x3, max f32x3
//   [7]  object navmesh (DecodeBmsNavMesh); [11] is not an offset but the navmesh flags
bool DecodeBms(std::span<const std::byte> file, MeshData& out, std::string& error) {
    out = MeshData{};
    core::BinaryReader r(file);
    if (r.Matches("JMXVBMS 0110")) {
        out.version = 110;
    } else if (r.Matches("JMXVBMS 0109")) {
        out.version = 109;
    } else {
        error = "not a JMXVBMS 0109/0110 file";
        return false;
    }
    r.Seek(kSignatureSize);
    std::uint32_t sections[12];
    for (auto& s : sections) s = r.U32();
    r.U32();
    out.flags = r.U32();
    r.U32();
    out.name = r.CountedString();
    out.materialName = r.CountedString();
    out.sortKey = r.U32();
    if (!r.Ok()) {
        error = "truncated header";
        return false;
    }

    // Vertices
    r.Seek(sections[0]);
    const std::uint32_t vertexCount = r.U32();
    if (vertexCount > kMaxVertices) {
        error = "implausible vertex count";
        return false;
    }
    const bool lightmapped = (out.flags & kFlagLightmapped) != 0;
    out.vertices.resize(vertexCount);
    if (lightmapped) out.lightmapUvs.resize(vertexCount);
    for (std::uint32_t i = 0; i < vertexCount; ++i) {
        MeshVertex& v = out.vertices[i];
        ReadFloats(r, v.position, 3);
        ReadFloats(r, v.normal, 3);
        ReadFloats(r, v.uv, 2);
        if (lightmapped) ReadFloats(r, out.lightmapUvs[i].data(), 2);
        r.Skip(12);
    }
    if (lightmapped) out.lightmapPath = r.CountedString();
    if (!r.Ok() || r.Offset() != sections[1]) {
        error = "vertex section does not end at the skin section";
        return false;
    }

    // Skin
    const std::uint32_t boneCount = r.U32();
    if (boneCount > kMaxBones) {
        error = "implausible bone count";
        return false;
    }
    std::vector<std::string> boneNames(boneCount);
    for (auto& name : boneNames) name = r.CountedString();
    if (boneCount > 0 && out.version == 110) {
        out.boneNames = std::move(boneNames);
        out.boneIndices.resize(vertexCount);
        out.boneWeights.resize(vertexCount);
        for (std::uint32_t i = 0; i < vertexCount; ++i) {
            const std::uint8_t b0 = r.U8();
            const float w0 = r.U16();
            const std::uint8_t b1 = r.U8();
            const float w1 = b1 == 0xFF ? 0.0f : static_cast<float>(r.U16());
            if (b1 == 0xFF) r.Skip(2);
            const float sum = w0 + w1 > 0.0f ? w0 + w1 : 1.0f;
            out.boneIndices[i] = {b0, b1 == 0xFF ? b0 : b1};
            out.boneWeights[i] = {w0 / sum, w1 / sum};
        }
    }
    // 0109 skin records (12 B per vertex) are not decoded yet; the section is skipped.

    // Triangles
    r.Seek(sections[2]);
    const std::uint32_t triangleCount = r.U32();
    if (triangleCount > kMaxTriangles) {
        error = "implausible triangle count";
        return false;
    }
    out.indices.resize(std::size_t{triangleCount} * 3);
    for (auto& index : out.indices) {
        index = r.U16();
        if (index >= vertexCount) {
            error = "triangle index out of range";
            return false;
        }
    }
    if (!r.Ok()) {
        error = "truncated triangle section";
        return false;
    }

    // Bounds (fall back to the vertices when the section is missing)
    if (sections[5] != 0 && sections[5] + 24 <= file.size()) {
        r.Seek(sections[5]);
        ReadFloats(r, out.bounds.min.data(), 3);
        ReadFloats(r, out.bounds.max.data(), 3);
    }
    if (sections[5] == 0 || out.bounds.max[0] < out.bounds.min[0]) {
        out.bounds = BoundsOf(out.vertices);
    }
    // The navmesh is optional data for the mesh itself: a bad one is left out
    // (the game-data test checks that every shipped one decodes).
    std::string navError;
    if (!DecodeBmsNavMesh(file, out.navMesh, navError)) out.navMesh = {};
    return true;
}

// .bms section 7, the object navmesh, in the order the client reads it. Header word
// [11] holds its flags: 1 = one more byte per edge, 2 = one more byte per cell, 4 = event names;
// 8 (1.188's fortress walls, barricades and gate pulley, 17 files) changes nothing in the layout:
// all of them read exactly without it. Its meaning is not known yet; it is kept in `flags`.
//   u32 vertexCount, { f32 x, y, z; u8 outline normal direction }
//   u32 cellCount, { u16 v0, v1, v2; u16 (always 0); (flags & 2) u8 event }
//   u32 outlineCount, { u16 vA, vB; u16 cell, neighbour (0xFFFF none); u8 flags; (flags & 1) u8 event }
//   u32 internalCount, same records
//   (flags & 4) u32 count, strings
//   edge grid: f32 originX, originZ, u32 width, height, tileCount (= width * height),
//              per tile u32 count, u16 outline edge indices
// The section runs to the next header offset (or the end of the file) and is read exactly.
bool DecodeBmsNavMesh(std::span<const std::byte> file, ObjectNavMesh& out, std::string& error) {
    out = ObjectNavMesh{};
    core::BinaryReader header(file);
    if (!header.Matches("JMXVBMS 0110") && !header.Matches("JMXVBMS 0109")) {
        error = "not a JMXVBMS 0109/0110 file";
        return false;
    }
    header.Seek(kSignatureSize);
    std::uint32_t sections[12];
    for (auto& s : sections) s = header.U32();
    if (!header.Ok()) {
        error = "truncated header";
        return false;
    }
    const std::uint32_t start = sections[7], flags = sections[11];
    if (start == 0) return true;
    if ((flags & ~15u) != 0) {
        error = "unknown navmesh flags";
        return false;
    }
    out.flags = flags;
    std::size_t end = file.size();
    for (int i = 0; i < 12; ++i) {
        if (i != 11 && sections[i] > start && sections[i] < end) end = sections[i];
    }
    if (start >= end) {
        error = "navmesh offset outside the file";
        return false;
    }
    core::BinaryReader r(file.first(end), start);

    const std::uint32_t vertexCount = r.U32();
    if (vertexCount > 0x10000 || std::size_t{vertexCount} * 13 > r.Remaining()) {
        error = "implausible navmesh vertex count";
        return false;
    }
    out.vertices.resize(vertexCount);
    out.directions.resize(vertexCount);
    for (std::uint32_t i = 0; i < vertexCount; ++i) {
        ReadFloats(r, out.vertices[i].data(), 3);
        out.directions[i] = r.U8();
    }
    const std::uint32_t cellCount = r.U32();
    const std::size_t cellSize = (flags & 2) ? 9 : 8;
    if (cellCount > 0x10000 || cellCount * cellSize > r.Remaining()) {
        error = "implausible navmesh cell count";
        return false;
    }
    out.cells.resize(cellCount);
    if (flags & 2) out.cellEvents.resize(cellCount);
    for (std::uint32_t c = 0; c < cellCount; ++c) {
        for (auto& index : out.cells[c]) {
            index = r.U16();
            if (index >= vertexCount) {
                error = "navmesh cell vertex out of range";
                return false;
            }
        }
        r.U16();
        if (flags & 2) out.cellEvents[c] = r.U8();
    }
    const auto readEdges = [&](std::vector<ObjectNavMesh::Edge>& edges) {
        const std::uint32_t count = r.U32();
        const std::size_t edgeSize = (flags & 1) ? 10 : 9;
        if (count > cellCount * 3 || count * edgeSize > r.Remaining()) return false;
        edges.resize(count);
        for (auto& edge : edges) {
            edge.a = r.U16();
            edge.b = r.U16();
            edge.cell = r.U16();
            edge.neighbour = r.U16();
            edge.flags = r.U8();
            if (flags & 1) edge.event = r.U8();
            if (edge.a >= vertexCount || edge.b >= vertexCount || edge.cell >= cellCount ||
                (edge.neighbour != 0xFFFF && edge.neighbour >= cellCount)) {
                return false;
            }
        }
        return r.Ok();
    };
    if (!readEdges(out.outline) || !readEdges(out.internal)) {
        error = "bad navmesh edges";
        return false;
    }
    if (flags & 4) {
        const std::uint32_t names = r.U32();
        for (std::uint32_t i = 0; i < names && r.Ok(); ++i) out.events.push_back(r.CountedString());
    }
    ObjectNavMesh::Grid& grid = out.grid;
    grid.originX = r.F32();
    grid.originZ = r.F32();
    grid.width = r.U32();
    grid.height = r.U32();
    const std::uint32_t tiles = r.U32();
    if (!r.Ok() || std::uint64_t{grid.width} * grid.height != tiles || tiles > r.Remaining() / 4) {
        error = "bad navmesh edge grid";
        return false;
    }
    grid.start.reserve(std::size_t{tiles} + 1);
    for (std::uint32_t t = 0; t < tiles && r.Ok(); ++t) {
        grid.start.push_back(static_cast<std::uint32_t>(grid.edges.size()));
        const std::uint32_t count = r.U32();
        if (std::size_t{count} * 2 > r.Remaining()) break;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint16_t edge = r.U16();
            if (edge >= out.outline.size()) {
                error = "navmesh grid edge out of range";
                return false;
            }
            grid.edges.push_back(edge);
        }
    }
    grid.start.push_back(static_cast<std::uint32_t>(grid.edges.size()));
    if (!r.Ok() || grid.start.size() != std::size_t{tiles} + 1 || r.Offset() != end) {
        error = "navmesh section not read exactly";
        return false;
    }
    if (!out.vertices.empty()) {
        for (int i = 0; i < 3; ++i) out.bounds.min[i] = out.bounds.max[i] = out.vertices[0][i];
        for (const auto& v : out.vertices) {
            for (int i = 0; i < 3; ++i) {
                out.bounds.min[i] = std::min(out.bounds.min[i], v[i]);
                out.bounds.max[i] = std::max(out.bounds.max[i], v[i]);
            }
        }
    }
    return true;
}

bool ObjectNavMesh::IsEventMarker() const {
    if (events.size() != 1 || cells.size() != 2) return false;
    const std::string_view name = events[0];
    return name.starts_with("event") || name.starts_with("EVENT");
}

// .bmt layout (0102): "JMXVBMT 0102", u32 count, then per material:
//   string name, f32x4 ambient, diffuse, specular, emissive, f32 power, u32 flags,
//   string texture (relative to the .bmt folder unless it contains a '\'),
//   f32 textureScale, u8 x3
bool DecodeBmt(std::span<const std::byte> file, const std::string& bmtPath, std::vector<MaterialData>& out,
               std::string& error) {
    out.clear();
    core::BinaryReader r(file);
    if (!r.Matches("JMXVBMT 0102")) {
        error = "not a JMXVBMT 0102 file";
        return false;
    }
    r.Seek(kSignatureSize);
    const std::uint32_t count = r.U32();
    if (count > kMaxMaterials) {
        error = "implausible material count";
        return false;
    }
    const std::string folder = FolderOf(bmtPath);
    out.resize(count);
    for (MaterialData& m : out) {
        m.name = r.CountedString();
        ReadFloats(r, m.ambient, 4);
        ReadFloats(r, m.diffuse, 4);
        ReadFloats(r, m.specular, 4);
        ReadFloats(r, m.emissive, 4);
        m.power = r.F32();
        m.flags = r.U32();
        const std::string texture = r.CountedString();
        if (!texture.empty()) {
            m.texturePath = texture.find('\\') != std::string::npos || folder.empty() ? texture : folder + '\\' + texture;
        }
        m.textureScale = r.F32();
        for (auto& b : m.extra) b = r.U8();
    }
    if (!r.Ok() || r.Remaining() != 0) {
        error = "material records do not end at the end of the file";
        return false;
    }
    return true;
}

namespace {

bool DecodeModifierSets(core::BinaryReader& r, std::vector<ResourceData::ModifierSet>& out) {
    const std::uint32_t count = r.U32();
    if (!r.Ok() || count > 256) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        ResourceData::ModifierSet set;
        set.kind = r.U32();
        set.state = static_cast<std::int32_t>(r.U32());
        set.animationSet = r.CountedString();
        const std::uint32_t modifiers = r.U32();
        if (!r.Ok() || modifiers > 256) return false;
        for (std::uint32_t k = 0; k < modifiers; ++k) {
            const std::uint32_t type = r.U32();
            std::uint32_t base[6];
            for (auto& w : base) w = r.U32();
            std::uint8_t bytes[4];
            for (auto& b : bytes) b = r.U8();
            switch (type) {
            case 0x00000: {
                ResourceData::MaterialModifier mm;
                mm.flags = base[2];
                mm.material = base[3];
                mm.durationMs = r.U32();
                mm.colourFlags = r.U32();
                mm.mode = r.U32();
                const std::uint32_t keys = r.U32();
                if (keys > 4096) return false;
                for (std::uint32_t j = 0; j < keys && r.Ok(); ++j) {
                    ResourceData::MaterialModifier::Key key;
                    key.ms = r.U32();
                    for (float& c : key.rgba) c = r.F32();
                    mm.keys.push_back(key);
                }
                if (mm.colourFlags & 4) {
                    const std::uint32_t scalars = r.U32();
                    if (scalars > 4096) return false;
                    r.Skip(8u * scalars);
                }
                for (auto& e : mm.enables) e = r.U32();
                for (auto& b : mm.states) b = r.U8();
                mm.pulseRate = r.F32();
                mm.twoSided = r.U32() == 1;
                set.materials.push_back(std::move(mm));
                break;
            }
            case 0x10000: {
                ResourceData::TextureModifier tm;
                tm.flags = base[2];
                tm.material = base[3];
                for (auto& w : tm.words) w = r.U32();
                for (float& f : tm.matrix) f = r.F32();
                tm.start = bytes[0];
                tm.end = bytes[1];
                tm.bookFlags = bytes[2];
                set.textures.push_back(tm);
                break;
            }
            case 0x10001:
            case 0x10002:
                r.U32();
                r.CountedString();
                r.U32();
                break;
            case 0x30000: {
                const std::uint32_t n = r.U32();
                if (n > 256) return false;
                for (std::uint32_t j = 0; j < n && r.Ok(); ++j) {
                    ResourceData::ParticleModifier pm;
                    pm.atRoot = r.U32() != 0;
                    pm.effect = r.CountedString();
                    pm.bone = r.CountedString();
                    for (float& f : pm.offset) f = r.F32();
                    pm.startMs = r.U32();
                    for (auto& b : pm.flags) b = r.U8();
                    pm.turned = r.U8() != 0;
                    if (pm.turned) {
                        for (float& f : pm.turn) f = r.F32();
                    }
                    set.particles.push_back(std::move(pm));
                }
                break;
            }
            case 0x40000: {
                ResourceData::EnvMapModifier em;
                em.flags = base[2];
                em.material = base[3];
                em.alphaFlags = base[5];
                for (auto& w : em.words) w = r.U32();
                set.envMaps.push_back(em);
                break;
            }
            case 0x40001: {
                r.Skip(40);
                const std::uint32_t n = r.U32();
                if (n > 4096) return false;
                for (std::uint32_t j = 0; j < n && r.Ok(); ++j) {
                    if (r.U8() == 1) r.CountedString();
                }
                break;
            }
            case 0x50000: {
                const std::uint32_t n = r.U32();
                if (n > 4096) return false;
                if (n > 0) r.Skip(44);
                for (std::uint32_t j = 0; j < n && r.Ok(); ++j) {
                    ResourceData::SoundTrack track;
                    track.group = r.CountedString();
                    const std::uint32_t tracks = r.U32();
                    if (tracks > 4096) return false;
                    for (std::uint32_t t = 0; t < tracks && r.Ok(); ++t) {
                        if (r.U32() == 1) {
                            ResourceData::SoundEvent event;
                            event.wave = r.CountedString();
                            event.ms = r.U32();
                            event.handle = r.CountedString();
                            track.events.push_back(std::move(event));
                        }
                    }
                    set.sounds.push_back(std::move(track));
                }
                break;
            }
            case 0x60000:
            case 0x60001:
            case 0x60002:
            case 0x70000: break;
            default: return false;
            }
            if (!r.Ok()) return false;
        }
        out.push_back(std::move(set));
    }
    return r.Ok();
}

} // namespace

// .bsr layout (0109):
//   0x0C u32 section offsets[13]: [0] material sets, [1] meshes
//   0x40 u16 kind0, u16 kind1, string name
//   [0]  u32 count, per set: u32 id, string .bmt path
//   [1]  u32 count, per mesh: string .bms path; some resources put a u32 before
//        every path except the first, recognisable because the plain read is not a .bms path
bool DecodeBsr(std::span<const std::byte> file, ResourceData& out, std::string& error) {
    out = ResourceData{};
    core::BinaryReader r(file);
    if (!r.Matches("JMXVRES 0109")) {
        error = "not a JMXVRES 0109 file";
        return false;
    }
    r.Seek(kSignatureSize);
    std::uint32_t sections[13];
    for (auto& s : sections) s = r.U32();
    r.Seek(0x40);
    out.kind0 = r.U16();
    out.kind1 = r.U16();
    out.name = r.CountedString();

    r.Seek(sections[0]);
    const std::uint32_t setCount = r.U32();
    if (setCount > kMaxEntries) {
        error = "implausible material set count";
        return false;
    }
    out.materialSets.resize(setCount);
    for (auto& set : out.materialSets) {
        set.id = r.U32();
        set.path = r.CountedString();
    }

    r.Seek(sections[1]);
    const std::uint32_t meshCount = r.U32();
    if (meshCount > kMaxEntries) {
        error = "implausible mesh count";
        return false;
    }
    const auto isMesh = [](const std::string& p) { return core::NormalizeAssetPath(p).ends_with(".bms"); };
    for (std::uint32_t i = 0; i < meshCount && r.Ok(); ++i) {
        const std::size_t at = r.Offset();
        std::string path = r.CountedString();
        if (!isMesh(path)) {
            r = core::BinaryReader(file, at);
            r.U32();
            path = r.CountedString();
            if (!isMesh(path)) {
                error = "mesh entry is not a .bms path";
                return false;
            }
        }
        out.meshPaths.push_back(std::move(path));
    }

    // [2] u32 count, per skeleton: string .bsk path, string attach bone (only the first is used)
    if (sections[2] != 0) {
        r.Seek(sections[2]);
        const std::uint32_t skeletons = r.U32();
        for (std::uint32_t i = 0; i < skeletons && i < kMaxEntries && r.Ok(); ++i) {
            std::string path = r.CountedString();
            std::string bone = r.CountedString();
            if (i == 0) {
                out.skeletonPath = std::move(path);
                out.skeletonAttachBone = std::move(bone);
            }
        }
    }
    // [3] u32, u32, u32 count, strings .ban paths
    if (sections[3] != 0) {
        r.Seek(sections[3]);
        r.U32();
        r.U32();
        const std::uint32_t animations = r.U32();
        if (animations > 4096) {
            error = "implausible animation count";
            return false;
        }
        for (std::uint32_t i = 0; i < animations && r.Ok(); ++i) out.animationPaths.push_back(r.CountedString());
    }
    if (!r.Ok()) {
        error = "truncated resource";
        return false;
    }
    // [7] string: the .bms whose object navmesh the object walks on (often a floor mesh made for
    // it); 0 for objects without one.
    if (sections[7] != 0 && sections[7] < file.size()) {
        core::BinaryReader n(file, sections[7]);
        std::string path = n.CountedString();
        if (n.Ok() && isMesh(path)) out.navMeshPath = std::move(path);
    }
    // [5] animation sets (optional; a malformed one is dropped, not an error):
    //   u32 sets, per set: string name, u32 states, per state:
    //     u32 id, i32 animation, u32 events, events x { i32 cursor ms, i32 code, i32, i32 }
    //     (code 1 events time the skill effects: a stage's "start event" N is the Nth),
    //     u32 warp points, if any: f32 scale + points x 8 bytes
    if (sections[5] != 0 && sections[5] < file.size()) {
        const std::size_t end = sections[6] != 0 && sections[6] <= file.size() ? sections[6] : file.size();
        core::BinaryReader s(file.first(end), sections[5]);
        const std::uint32_t sets = s.U32();
        std::vector<ResourceData::AnimationSet> parsed;
        for (std::uint32_t i = 0; i < sets && i < 256 && s.Ok(); ++i) {
            ResourceData::AnimationSet set;
            set.name = s.CountedString();
            const std::uint32_t states = s.U32();
            for (std::uint32_t k = 0; k < states && k < 512 && s.Ok(); ++k) {
                ResourceData::AnimationState state;
                state.id = s.U32();
                state.animation = static_cast<std::int32_t>(s.U32());
                const std::uint32_t events = s.U32();
                if (events > 4096) break;
                for (std::uint32_t e = 0; e < events && s.Ok(); ++e) {
                    ResourceData::AnimationEvent event;
                    event.timeMs = static_cast<std::int32_t>(s.U32());
                    event.code = static_cast<std::int32_t>(s.U32());
                    event.a = static_cast<std::int32_t>(s.U32());
                    event.b = static_cast<std::int32_t>(s.U32());
                    state.events.push_back(event);
                }
                const std::uint32_t warp = s.U32();
                if (warp > 4096) break;
                if (warp > 0) s.Skip(4u + 8u * warp);
                set.states.push_back(state);
            }
            parsed.push_back(std::move(set));
        }
        if (s.Ok()) out.animationSets = std::move(parsed);
    }
    // [6] modifiers (optional; one this reader does not know drops them all).
    if (sections[6] != 0 && sections[6] < file.size()) {
        core::BinaryReader m(file, sections[6]);
        std::vector<ResourceData::ModifierSet> sets;
        if (DecodeModifierSets(m, sets) && DecodeModifierSets(m, sets)) {
            out.modifierSets = std::move(sets);
        } else {
            out.modifiersUnread = true;
        }
    }
    return true;
}

std::vector<int> ResourceData::StateEvents(std::string_view set, std::uint32_t state, int code) const {
    std::vector<int> out;
    for (const AnimationEvent& e : StateEventRecords(set, state, code)) out.push_back(e.timeMs);
    return out;
}

std::vector<ResourceData::AnimationEvent> ResourceData::StateEventRecords(std::string_view set, std::uint32_t state, int code) const {
    std::vector<AnimationEvent> out;
    for (const AnimationSet& s : animationSets) {
        if (s.name.size() != set.size() || !std::equal(s.name.begin(), s.name.end(), set.begin(), [](char a, char b) {
                return (a >= 'A' && a <= 'Z' ? a - 'A' + 'a' : a) == (b >= 'A' && b <= 'Z' ? b - 'A' + 'a' : b);
            })) {
            continue;
        }
        for (const AnimationState& st : s.states) {
            if (st.id != state) continue;
            for (const AnimationEvent& e : st.events) {
                if (e.code == code) out.push_back(e);
            }
            std::stable_sort(out.begin(), out.end(), [](const AnimationEvent& x, const AnimationEvent& y) { return x.timeMs < y.timeMs; });
            return out;
        }
    }
    return out;
}

std::vector<ResourceData::SoundEvent> ResourceData::StateSounds(std::string_view set, std::uint32_t state) const {
    const auto same = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return (x >= 'A' && x <= 'Z' ? x - 'A' + 'a' : x) == (y >= 'A' && y <= 'Z' ? y - 'A' + 'a' : y);
               });
    };
    std::vector<SoundEvent> out;
    const auto take = [&](const ModifierSet& m) {
        for (const SoundTrack& track : m.sounds) out.insert(out.end(), track.events.begin(), track.events.end());
    };
    for (const ModifierSet& m : modifierSets) {
        if (m.kind == 1 && m.state == std::int32_t(state) && same(m.animationSet, set)) take(m);
    }
    if (out.empty()) {
        std::string clip = StateAnimation(set, state);
        if (const auto slash = clip.find_last_of("\\/"); slash != std::string::npos) clip = clip.substr(slash + 1);
        if (const auto dot = clip.rfind('.'); dot != std::string::npos) clip.resize(dot);
        for (const ModifierSet& m : modifierSets) {
            if (m.kind == 0 && !clip.empty() && same(m.animationSet, clip)) take(m);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const SoundEvent& a, const SoundEvent& b) { return a.ms < b.ms; });
    return out;
}

std::string ResourceData::StateAnimation(std::string_view set, std::uint32_t state) const {
    const auto lower = [](std::string_view text) {
        std::string out(text);
        for (char& c : out) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        return out;
    };
    const std::string wanted = lower(set);
    for (const AnimationSet& s : animationSets) {
        if (lower(s.name) != wanted) continue;
        for (const AnimationState& st : s.states) {
            if (st.id == state && st.animation >= 0 && st.animation < static_cast<int>(animationPaths.size())) {
                return animationPaths[static_cast<std::size_t>(st.animation)];
            }
        }
    }
    return {};
}

AmbientLook PartAmbientLook(const ResourceData& resource, int materialIndex) {
    AmbientLook look;
    if (materialIndex < 0) return look;
    // A modifier names one material by its place in the .bmt, or all of them (0xFFFFFFFF).
    const auto mine = [&](std::uint32_t material) {
        return material == 0xFFFFFFFFu || material == static_cast<std::uint32_t>(materialIndex);
    };
    for (const ResourceData::ModifierSet& set : resource.modifierSets) {
        if (set.kind != 2) continue;
        for (const ResourceData::TextureModifier& t : set.textures) {
            if (!mine(t.material) || t.words[1] != 1 || look.scrolls) continue;
            look.scrolls = true;
            const int at[6] = {0, 1, 4, 5, 8, 9};
            for (int i = 0; i < 6; ++i) look.uvRate[i] = t.matrix[at[i]];
        }
        for (const ResourceData::EnvMapModifier& e : set.envMaps) {
            if (!mine(e.material)) continue;
            look.envMap = true;
            look.envAlphaZero = look.envAlphaZero || (e.alphaFlags & 2u) != 0;
        }
        for (const ResourceData::MaterialModifier& m : set.materials) {
            if (!mine(m.material) || m.enables[1] == 0) continue;
            const bool alphaTest = m.enables[0] != 0, noDepthWrites = m.enables[2] != 0;
            if (alphaTest && !noDepthWrites) continue; // the edges blended, the rest solid: drawn as cut out
            look.blended = true;
            look.alphaTest = alphaTest;
            look.sourceBlend = m.states[0];
            look.destinationBlend = m.states[1];
            look.twoSided = m.twoSided;
            // The texture stage's colour: texture x the lit material colour (the first key's, grey),
            // doubled by "modulate 2x" (5), four times by "modulate 4x" (6).
            float key = 1.0f;
            if (!m.keys.empty() && (m.colourFlags & 3u) != 0) key = (m.keys[0].rgba[0] + m.keys[0].rgba[1] + m.keys[0].rgba[2]) / 3.0f;
            const std::uint8_t op = m.states[2];
            look.tint = key * (op == 5 ? 2.0f : op == 6 ? 4.0f : 1.0f);
        }
    }
    return look;
}

// .cpd (0101): "JMXVCPD 0101", u32 collisionOffset, u32 partsOffset (absolute),
//   u32 x5, u32 flags, string name; at collisionOffset: string collision .bsr (its navmesh is the
//   compound's: every structure compound of 1.188's object.ifo names one, the effects none);
//   at partsOffset: u32 count, strings .bsr paths
bool DecodeCpd(std::span<const std::byte> file, CompoundData& out, std::string& error) {
    out = CompoundData{};
    core::BinaryReader r(file);
    if (!r.Matches("JMXVCPD 0101")) {
        error = "not a JMXVCPD 0101 file";
        return false;
    }
    r.Seek(kSignatureSize);
    const std::uint32_t collisionOffset = r.U32();
    const std::uint32_t partsOffset = r.U32();
    r.Seek(44);
    out.name = r.CountedString();
    r.Seek(collisionOffset);
    out.collisionPath = r.CountedString();
    r.Seek(partsOffset);
    const std::uint32_t count = r.U32();
    if (count > kMaxEntries) {
        error = "implausible part count";
        return false;
    }
    for (std::uint32_t i = 0; i < count && r.Ok(); ++i) out.partPaths.push_back(r.CountedString());
    if (!r.Ok()) {
        error = "truncated compound";
        return false;
    }
    return true;
}

bool LoadObjectNavMesh(const FileSystem& fs, const std::string& objectPath, ObjectNavMesh& out, std::string& error) {
    out = ObjectNavMesh{};
    std::string resourcePath = objectPath;
    if (core::NormalizeAssetPath(objectPath).ends_with(".cpd")) {
        const auto file = fs.Read(objectPath);
        CompoundData compound;
        if (!file) {
            error = objectPath + " not found";
            return false;
        }
        if (!DecodeCpd(file->Bytes(), compound, error)) return false;
        if (compound.collisionPath.empty()) return true;
        resourcePath = compound.collisionPath;
    }
    if (!core::NormalizeAssetPath(resourcePath).ends_with(".bsr")) return true;
    const auto bsr = fs.Read(resourcePath);
    if (!bsr) {
        error = resourcePath + " not found";
        return false;
    }
    ResourceData resource;
    if (!DecodeBsr(bsr->Bytes(), resource, error)) return false;
    if (resource.navMeshPath.empty()) return true;
    const auto bms = fs.Read(resource.navMeshPath);
    if (!bms) {
        error = resource.navMeshPath + " not found";
        return false;
    }
    return DecodeBmsNavMesh(bms->Bytes(), out, error);
}

namespace {

// The original matches material names without regard to case (a mesh says "Yeoha", its
// .bmt "yeoha"); an unnamed mesh material is "default". ASCII folding only.
bool SameName(const std::string& a, const std::string& b) {
    const std::string& x = a.empty() ? std::string("default") : a;
    const std::string& y = b.empty() ? std::string("default") : b;
    if (x.size() != y.size()) return false;
    for (std::size_t i = 0; i < x.size(); ++i) {
        char p = x[i], q = y[i];
        if (p >= 'A' && p <= 'Z') p = static_cast<char>(p + 32);
        if (q >= 'A' && q <= 'Z') q = static_cast<char>(q + 32);
        if (p != q) return false;
    }
    return true;
}

// Appends one mesh with the first material of that name found in `materialSets`.
void AppendMesh(const FileSystem& fs, const std::string& meshPath,
                const std::vector<std::vector<MaterialData>>& materialSets, ModelData& out, bool& firstBounds,
                int meshIndex = -1) {
    ModelPart part;
    part.meshPath = meshPath;
    part.meshIndex = meshIndex;
    auto file = fs.Read(meshPath);
    std::string meshError;
    if (!file) {
        out.warnings.push_back("missing mesh " + meshPath);
        return;
    }
    if (!DecodeBms(file->Bytes(), part.mesh, meshError)) {
        out.warnings.push_back(meshPath + ": " + meshError);
        return;
    }
    for (const auto& set : materialSets) {
        const auto it = std::find_if(set.begin(), set.end(),
                                     [&](const MaterialData& m) { return SameName(m.name, part.mesh.materialName); });
        if (it != set.end()) {
            part.material = *it;
            part.hasMaterial = true;
            part.materialIndex = static_cast<int>(it - set.begin());
            break;
        }
    }
    if (!part.hasMaterial && !materialSets.empty()) {
        out.warnings.push_back("no material '" + part.mesh.materialName + "' for " + meshPath);
    }
    Merge(out.bounds, part.mesh.bounds, firstBounds);
    out.parts.push_back(std::move(part));
}

// Appends every mesh of a .bsr; adopts its skeleton if the model has none yet.
bool AppendResource(const FileSystem& fs, const std::string& path, ModelData& out, bool& firstBounds,
                    ResourceData* resourceOut, std::string& error) {
    auto file = fs.Read(path);
    if (!file) {
        error = "missing resource " + path;
        return false;
    }
    ResourceData resource;
    if (!DecodeBsr(file->Bytes(), resource, error)) {
        error = path + ": " + error;
        return false;
    }
    // Set 0 is the default look; later sets are variants (clone, champion, ...).
    // The default look is the lowest set id (the file may list a variant first).
    std::vector<ResourceData::MaterialSet> ordered = resource.materialSets;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const ResourceData::MaterialSet& a, const ResourceData::MaterialSet& b) { return a.id < b.id; });
    std::vector<std::vector<MaterialData>> materialSets;
    for (const auto& set : ordered) {
        std::vector<MaterialData> materials;
        std::string setError;
        auto bmt = fs.Read(set.path);
        if (!bmt) {
            out.warnings.push_back("missing material set " + set.path);
        } else if (!DecodeBmt(bmt->Bytes(), set.path, materials, setError)) {
            out.warnings.push_back(set.path + ": " + setError);
        }
        materialSets.push_back(std::move(materials));
    }
    for (std::size_t i = 0; i < resource.meshPaths.size(); ++i) {
        const std::size_t before = out.parts.size();
        AppendMesh(fs, resource.meshPaths[i], materialSets, out, firstBounds, static_cast<int>(i));
        if (out.parts.size() > before && !resource.modifierSets.empty()) {
            out.parts.back().ambient = PartAmbientLook(resource, out.parts.back().materialIndex);
        }
    }
    if (out.skeleton.bones.empty() && !resource.skeletonPath.empty()) {
        auto bsk = fs.Read(resource.skeletonPath);
        std::string skeletonError;
        if (!bsk) {
            out.warnings.push_back("missing skeleton " + resource.skeletonPath);
        } else if (!DecodeBsk(bsk->Bytes(), out.skeleton, skeletonError)) {
            out.warnings.push_back(resource.skeletonPath + ": " + skeletonError);
        } else {
            out.animationPaths = resource.animationPaths;
            out.animationSets = resource.animationSets;
        }
    }
    if (resourceOut != nullptr) *resourceOut = std::move(resource);
    return true;
}

} // namespace

bool UseMaterialSet(const FileSystem& fs, ModelData& model, std::uint32_t setId) {
    const auto set = std::find_if(model.resource.materialSets.begin(), model.resource.materialSets.end(),
                                  [&](const ResourceData::MaterialSet& m) { return m.id == setId; });
    if (set == model.resource.materialSets.end()) return false;
    auto bmt = fs.Read(set->path);
    std::vector<MaterialData> materials;
    std::string error;
    if (!bmt || !DecodeBmt(bmt->Bytes(), set->path, materials, error)) return false;
    for (ModelPart& part : model.parts) {
        const auto it = std::find_if(materials.begin(), materials.end(),
                                     [&](const MaterialData& m) { return SameName(m.name, part.mesh.materialName); });
        if (it == materials.end()) continue;
        part.material = *it;
        part.hasMaterial = true;
    }
    return true;
}

bool FindPartLink(std::span<const std::byte> bsr, bool character, std::size_t meshCount, PartLink& out) {
    const auto u32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        std::memcpy(&v, bsr.data() + at, 4);
        return v;
    };
    constexpr std::size_t kMinOffset = 0x40; // past the header and section pointers
    constexpr std::uint32_t kMaxPairs = 512;
    // Characters end with one more u32 (nComboNum, 0); some items do too, so items
    // try both endings (the exact one first).
    const std::size_t endings[2] = {character ? 4u : 0u, 4u};
    for (const std::size_t trailing : endings) {
        if (bsr.size() < kMinOffset + 16 + trailing) continue;
        if (trailing == 4 && u32(bsr.size() - 4) != 0) continue;
        for (std::uint32_t n = kMaxPairs + 1; n-- > 0;) {
            const std::size_t tail = 16 + 8 * static_cast<std::size_t>(n) + trailing;
            if (tail + kMinOffset > bsr.size()) continue;
            const std::size_t p = bsr.size() - tail;
            if (u32(p + 12) != n) continue;
            PartLink link{u32(p), u32(p + 4), u32(p + 8), {}};
            // An item's: a 1 or 2, its part up to 16 (the job suits'), its mode 1 or 2, its keys under 32 with values 0
            // or 1 (the client reads only the keys: a thief's suit's values are 0 and 1).
            bool valid = character || (link.a >= 1 && link.a <= 2 && (link.c == 1 || link.c == 2) && link.b <= 16);
            for (std::uint32_t i = 0; valid && i < n; ++i) {
                const std::uint32_t key = u32(p + 16 + 8 * i), value = u32(p + 20 + 8 * i);
                valid = character ? value < meshCount : key < 32 && value <= 1;
                link.pairs.push_back({key, value});
            }
            if (!valid) continue;
            out = std::move(link);
            return true;
        }
        if (character) break; // characters always carry the extra u32
    }
    return false;
}

bool LoadModel(const FileSystem& fs, const std::string& path, ModelData& out, std::string& error) {
    out = ModelData{};
    out.sourcePath = path;
    const std::string key = core::NormalizeAssetPath(path);
    bool firstBounds = true;

    if (key.ends_with(".bms")) {
        AppendMesh(fs, path, {}, out, firstBounds);
    } else if (key.ends_with(".cpd")) {
        auto file = fs.Read(path);
        CompoundData compound;
        if (!file) {
            error = "file not found";
            return false;
        }
        if (!DecodeCpd(file->Bytes(), compound, error)) {
            return false;
        }
        for (std::size_t i = 0; i < compound.partPaths.size(); ++i) {
            std::string partError;
            if (!AppendResource(fs, compound.partPaths[i], out, firstBounds, i == 0 ? &out.resource : nullptr,
                                partError)) {
                out.warnings.push_back(partError);
            }
        }
    } else if (!AppendResource(fs, path, out, firstBounds, &out.resource, error)) {
        if (error.starts_with("missing resource")) error = "file not found";
        return false;
    }

    if (out.parts.empty()) {
        error = out.warnings.empty() ? "the resource has no meshes" : out.warnings.front();
        return false;
    }
    return true;
}

} // namespace sr::assets
