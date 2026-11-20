#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "sr/assets/Model.h"

using namespace sr::assets;

namespace {

class Writer {
public:
    std::vector<std::byte> bytes;
    void Raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        bytes.insert(bytes.end(), b, b + n);
    }
    void U32(std::uint32_t v) { Raw(&v, 4); }
    void U16(std::uint16_t v) { Raw(&v, 2); }
    void U8(std::uint8_t v) { Raw(&v, 1); }
    void F32(float v) { Raw(&v, 4); }
    void Str(const std::string& s) {
        U32(static_cast<std::uint32_t>(s.size()));
        Raw(s.data(), s.size());
    }
    void PatchU32(std::size_t at, std::uint32_t v) { std::memcpy(bytes.data() + at, &v, 4); }
};

// One triangle; optionally skinned (0110) or lightmapped.
std::vector<std::byte> MakeBms(bool skinned, bool lightmapped) {
    Writer w;
    w.Raw("JMXVBMS 0110", 12);
    for (int i = 0; i < 12; ++i) w.U32(0); // section offsets, patched below
    w.U32(0);
    w.U32(lightmapped ? 0x400 : 0);
    w.U32(0);
    w.Str("tri");
    w.Str("mat_a");
    w.U32(7);

    w.PatchU32(12 + 0 * 4, static_cast<std::uint32_t>(w.bytes.size()));
    w.U32(3);
    for (int v = 0; v < 3; ++v) {
        w.F32(static_cast<float>(v)); w.F32(0); w.F32(0);  // position
        w.F32(0); w.F32(1); w.F32(0);                      // normal
        w.F32(0.5f); w.F32(0.25f);                         // uv
        if (lightmapped) { w.F32(0.1f); w.F32(0.2f); }
        w.U32(0); w.U32(0); w.U32(0);
    }
    if (lightmapped) w.Str("prim\\lightmap\\x.ddj");

    w.PatchU32(12 + 1 * 4, static_cast<std::uint32_t>(w.bytes.size()));
    if (skinned) {
        w.U32(2);
        w.Str("Bip01");
        w.Str("Bip01 Spine");
        for (int v = 0; v < 3; ++v) {
            w.U8(0); w.U16(300); w.U8(v == 2 ? 0xFF : 1); w.U16(100);
        }
    } else {
        w.U32(0);
    }

    w.PatchU32(12 + 2 * 4, static_cast<std::uint32_t>(w.bytes.size()));
    w.U32(1);
    w.U16(0); w.U16(1); w.U16(2);
    return w.bytes;
}

} // namespace

TEST_CASE("DecodeBms reads a static mesh") {
    MeshData mesh;
    std::string error;
    REQUIRE_MESSAGE(DecodeBms(MakeBms(false, false), mesh, error), error);
    CHECK(mesh.version == 110);
    CHECK(mesh.name == "tri");
    CHECK(mesh.materialName == "mat_a");
    CHECK(mesh.sortKey == 7);
    REQUIRE(mesh.vertices.size() == 3);
    CHECK(mesh.vertices[2].position[0] == 2.0f);
    CHECK(mesh.vertices[1].uv[1] == 0.25f);
    CHECK(mesh.indices == std::vector<std::uint16_t>{0, 1, 2});
    CHECK_FALSE(mesh.IsSkinned());
    // No bounds section: computed from the vertices.
    CHECK(mesh.bounds.min[0] == 0.0f);
    CHECK(mesh.bounds.max[0] == 2.0f);
}

TEST_CASE("DecodeBms reads lightmapped vertices and the lightmap path") {
    MeshData mesh;
    std::string error;
    REQUIRE_MESSAGE(DecodeBms(MakeBms(false, true), mesh, error), error);
    REQUIRE(mesh.lightmapUvs.size() == 3);
    CHECK(mesh.lightmapUvs[0][1] == doctest::Approx(0.2f));
    CHECK(mesh.lightmapPath == "prim\\lightmap\\x.ddj");
}

TEST_CASE("DecodeBms reads two-bone skinning and normalizes the weights") {
    MeshData mesh;
    std::string error;
    REQUIRE_MESSAGE(DecodeBms(MakeBms(true, false), mesh, error), error);
    REQUIRE(mesh.IsSkinned());
    CHECK(mesh.boneNames[1] == "Bip01 Spine");
    CHECK(mesh.boneWeights[0][0] == doctest::Approx(0.75f));
    CHECK(mesh.boneIndices[0][1] == 1);
    // Second bone 0xFF means "none": all weight on the first bone.
    CHECK(mesh.boneWeights[2][0] == doctest::Approx(1.0f));
    CHECK(mesh.boneIndices[2][1] == 0);
}

TEST_CASE("DecodeBms rejects out-of-range indices and other files") {
    auto bytes = MakeBms(false, false);
    bytes[bytes.size() - 2] = std::byte{9}; // last index -> 9
    MeshData mesh;
    std::string error;
    CHECK_FALSE(DecodeBms(bytes, mesh, error));
    CHECK_FALSE(DecodeBms(std::vector<std::byte>(64), mesh, error));
}

namespace {

// MakeBms plus a two-cell navmesh (a 10 x 10 square at height 5) in section 7.
std::vector<std::byte> MakeBmsWithNavMesh() {
    Writer w;
    w.bytes = MakeBms(false, false);
    w.PatchU32(12 + 7 * 4, static_cast<std::uint32_t>(w.bytes.size()));
    const float corners[4][2] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    w.U32(4);
    std::uint8_t direction = 0;
    for (const auto& c : corners) {
        w.F32(c[0]); w.F32(5.0f); w.F32(c[1]); w.U8(direction);
        direction = static_cast<std::uint8_t>(direction + 64);
    }
    w.U32(2);
    w.U16(0); w.U16(1); w.U16(2); w.U16(0);
    w.U16(0); w.U16(2); w.U16(3); w.U16(0);
    w.U32(4); // outline
    const std::uint16_t outline[4][3] = {{0, 1, 0}, {1, 2, 0}, {2, 3, 1}, {3, 0, 1}};
    for (const auto& e : outline) {
        w.U16(e[0]); w.U16(e[1]); w.U16(e[2]); w.U16(0xFFFF); w.U8(0);
    }
    w.U32(1); // internal
    w.U16(0); w.U16(2); w.U16(0); w.U16(1); w.U8(0);
    w.F32(0); w.F32(0); w.U32(1); w.U32(1); w.U32(1); // edge grid: one tile
    w.U32(2); w.U16(0); w.U16(1);
    return w.bytes;
}

} // namespace

TEST_CASE("DecodeBmsNavMesh reads the object navmesh of section 7") {
    std::string error;
    ObjectNavMesh nav;
    REQUIRE_MESSAGE(DecodeBmsNavMesh(MakeBmsWithNavMesh(), nav, error), error);
    REQUIRE(nav.vertices.size() == 4);
    REQUIRE(nav.cells.size() == 2);
    CHECK(nav.cells[1][2] == 3);
    CHECK(nav.outline.size() == 4);
    REQUIRE(nav.internal.size() == 1);
    CHECK(nav.internal[0].neighbour == 1);
    CHECK(nav.bounds.max[0] == 10.0f);
    CHECK(nav.bounds.min[1] == 5.0f);
    // The outline normals' angle indices and the edge grid are kept.
    REQUIRE(nav.directions.size() == 4);
    CHECK(nav.directions[3] == 192);
    CHECK(nav.grid.width == 1);
    CHECK(nav.grid.start == std::vector<std::uint32_t>{0, 2});
    CHECK(nav.grid.edges == std::vector<std::uint16_t>{0, 1});
    CHECK_FALSE(nav.IsEventMarker());

    // DecodeBms keeps it with the mesh; meshes without section 7 have none.
    MeshData mesh;
    REQUIRE_MESSAGE(DecodeBms(MakeBmsWithNavMesh(), mesh, error), error);
    CHECK(mesh.navMesh.cells.size() == 2);
    REQUIRE(DecodeBms(MakeBms(false, false), mesh, error));
    CHECK(mesh.navMesh.empty());
    CHECK(DecodeBmsNavMesh(MakeBms(false, false), nav, error));
    CHECK(nav.empty());

    // The section must be read exactly.
    auto longer = MakeBmsWithNavMesh();
    longer.push_back(std::byte{0});
    CHECK_FALSE(DecodeBmsNavMesh(longer, nav, error));
    auto badCell = MakeBmsWithNavMesh();
    // first cell's v2 -> 9 (vertex out of range): offset of section 7 + 4 + 4*13 + 4 + 4
    std::uint32_t navStart = 0;
    std::memcpy(&navStart, badCell.data() + 12 + 7 * 4, 4);
    badCell[navStart + 4 + 4 * 13 + 4 + 4] = std::byte{9};
    CHECK_FALSE(DecodeBmsNavMesh(badCell, nav, error));
}

TEST_CASE("DecodeBmt resolves texture names against the material folder") {
    Writer w;
    w.Raw("JMXVBMT 0102", 12);
    w.U32(2);
    for (const char* tex : {"bark.ddj", "prim\\mtrl\\shared\\leaf.ddj"}) {
        w.Str(std::string("m_") + tex[0]);
        for (int i = 0; i < 16; ++i) w.F32(1.0f);
        w.F32(0.5f);
        w.U32(0x40);
        w.Str(tex);
        w.F32(1.0f);
        w.U8(1); w.U8(2); w.U8(3);
    }
    std::vector<MaterialData> materials;
    std::string error;
    REQUIRE_MESSAGE(DecodeBmt(w.bytes, "prim\\mtrl\\tree\\a.bmt", materials, error), error);
    REQUIRE(materials.size() == 2);
    CHECK(materials[0].texturePath == "prim\\mtrl\\tree\\bark.ddj");
    CHECK(materials[1].texturePath == "prim\\mtrl\\shared\\leaf.ddj");
    CHECK(materials[0].flags == 0x40);
    CHECK(materials[1].extra[2] == 3);

    w.U8(0); // trailing garbage must be rejected
    CHECK_FALSE(DecodeBmt(w.bytes, "a.bmt", materials, error));
}
