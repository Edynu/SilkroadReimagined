#include "sr/game/NavDungeon.h"

#include <algorithm>
#include <cmath>

#include "NavShared.h"

namespace sr::game {

using namespace detail;

namespace {

// Doorway edges of two blocks match when their ends meet (in either order) this closely; the
// client matches them by position, the tolerance is ours.
constexpr float kDoorTolerance = 1.0f;

bool Near(const float a[3], const float b[3]) {
    return std::fabs(a[0] - b[0]) <= kDoorTolerance && std::fabs(a[1] - b[1]) <= kDoorTolerance && std::fabs(a[2] - b[2]) <= kDoorTolerance;
}

} // namespace

void NavDungeon::Block::ToLocal(float wx, float wz, float& lx, float& lz) const {
    const float dx = wx - x, dz = wz - z;
    lx = dx * cos + dz * sin;
    lz = -dx * sin + dz * cos;
}

void NavDungeon::Block::ToWorld(float lx, float lz, float& wx, float& wz) const {
    wx = lx * cos - lz * sin + x;
    wz = lx * sin + lz * cos + z;
}

void NavDungeon::Link(const assets::DungeonData& data) {
    region_ = data.regionId;
    for (int i = 0; i < 3; ++i) {
        boundsMin_[i] = data.boundsMin[i];
        gridSize_[i] = data.gridSize[i];
    }
    for (const auto& cell : data.grid) grid_[cell.key] = cell.blocks;
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        Block& b = blocks_[i];
        const assets::DungeonData::Block& d = data.blocks[i];
        b.x = d.position[0];
        b.y = d.position[1];
        b.z = d.position[2];
        b.cos = std::cos(d.yaw);
        b.sin = std::sin(d.yaw);
        b.neighbours = d.neighbours;
        const std::uint32_t outline = b.mesh ? b.mesh->outlineCount : 0;
        b.doorBlock.assign(outline, -1);
        b.doorEdge.assign(outline, 0);
        if (b.mesh) {
            // The floor's box turned into dungeon space (its four corners).
            const NavObjectMesh& m = *b.mesh;
            for (int k = 0; k < 3; ++k) {
                b.min[k] = kInfinity;
                b.max[k] = -kInfinity;
            }
            for (const float cx : {m.min[0], m.max[0]}) {
                for (const float cz : {m.min[2], m.max[2]}) {
                    float wx = 0, wz = 0;
                    b.ToWorld(cx, cz, wx, wz);
                    b.min[0] = std::min(b.min[0], wx);
                    b.max[0] = std::max(b.max[0], wx);
                    b.min[2] = std::min(b.min[2], wz);
                    b.max[2] = std::max(b.max[2], wz);
                }
            }
            b.min[1] = m.min[1] + b.y;
            b.max[1] = m.max[1] + b.y;
        }
    }
    // A doorway edge's ends in dungeon space.
    const auto ends = [&](const Block& b, std::uint32_t edge, float a[3], float c[3]) {
        const NavObjectMesh::Edge& e = b.mesh->edges[edge];
        const NavObjectMesh::Vertex& va = b.mesh->vertices[e.a];
        const NavObjectMesh::Vertex& vb = b.mesh->vertices[e.b];
        b.ToWorld(va.x, va.z, a[0], a[2]);
        b.ToWorld(vb.x, vb.z, c[0], c[2]);
        a[1] = va.y + b.y;
        c[1] = vb.y + b.y;
    };
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        Block& b = blocks_[i];
        if (!b.mesh) continue;
        for (std::uint32_t e = 0; e < b.mesh->outlineCount; ++e) {
            if ((b.mesh->edges[e].flags & nav_edge::kLink) == 0) continue;
            float a[3], c[3];
            ends(b, e, a, c);
            for (const std::uint32_t n : b.neighbours) {
                if (n >= blocks_.size() || n == i || !blocks_[n].mesh) continue;
                const Block& other = blocks_[n];
                for (std::uint32_t f = 0; f < other.mesh->outlineCount && b.doorBlock[e] < 0; ++f) {
                    if ((other.mesh->edges[f].flags & nav_edge::kLink) == 0) continue;
                    float oa[3], oc[3];
                    ends(other, f, oa, oc);
                    if ((Near(a, oc) && Near(c, oa)) || (Near(a, oa) && Near(c, oc))) {
                        b.doorBlock[e] = static_cast<std::int32_t>(n);
                        b.doorEdge[e] = f;
                        ++doors_;
                    }
                }
                if (b.doorBlock[e] >= 0) break;
            }
        }
    }
}

NavPosition NavDungeon::Drop(float x, float y, float z) const {
    NavPosition out;
    out.region = region_;
    out.x = x;
    out.y = y;
    out.z = z;
    const float p[3] = {x, y, z};
    std::uint32_t index[3];
    for (int i = 0; i < 3; ++i) {
        const float cell = std::floor((p[i] - boundsMin_[i]) / assets::DungeonData::kGridCell);
        if (cell < 0.0f || cell >= static_cast<float>(gridSize_[i])) return out;
        index[i] = static_cast<std::uint32_t>(cell);
    }
    const auto it = grid_.find((index[1] << 20) | (index[2] << 10) | index[0]);
    if (it == grid_.end()) return out;
    float distance = kInfinity;
    for (const std::uint32_t b : it->second) {
        if (b >= blocks_.size() || !blocks_[b].mesh) continue;
        const Block& block = blocks_[b];
        float lx = 0, lz = 0, height = 0;
        block.ToLocal(x, z, lx, lz);
        const std::int32_t tri = TriangleUnder(*block.mesh, lx, lz, y - block.y, height);
        if (tri < 0 || std::fabs(height + block.y - y) >= distance) continue;
        distance = std::fabs(height + block.y - y);
        out.y = height + block.y;
        out.cell = tri;
        out.instance = static_cast<std::int32_t>(b);
    }
    return out;
}

std::uint32_t NavDungeon::Walk(const NavPosition& from, float x, float z, NavPosition& to, std::uint32_t maxCrossings,
                              std::vector<std::string>* zones, const std::unordered_set<std::string>* blocked) const {
    static const std::unordered_set<std::string> none;
    to = from;
    if (!from.Valid() || from.instance < 0 || static_cast<std::size_t>(from.instance) >= blocks_.size()) return nav_walk::kFailed;
    std::uint32_t index = static_cast<std::uint32_t>(from.instance);
    const Block* block = &blocks_[index];
    if (!block->mesh || static_cast<std::size_t>(from.cell) >= block->mesh->triangles.size()) return nav_walk::kFailed;
    std::uint32_t result = nav_walk::kReached, crossings = maxCrossings;
    float px = 0, pz = 0, qx = 0, qz = 0;
    block->ToLocal(from.x, from.z, px, pz);
    block->ToLocal(x, z, qx, qz);
    std::uint32_t tri = static_cast<std::uint32_t>(from.cell);
    std::int64_t cameFrom = -1;
    for (;;) {
        const NavObjectMesh& mesh = *block->mesh;
        const MeshStep step = WalkTriangles(mesh, tri, px, pz, qx, qz, cameFrom, crossings, blocked != nullptr ? *blocked : none, zones);
        tri = step.tri;
        const NavObjectMesh::Triangle& t = mesh.triangles[tri];
        to.instance = static_cast<std::int32_t>(index);
        to.cell = static_cast<std::int32_t>(tri);
        if (step.kind == MeshStep::Kind::Inside) {
            to.x = x;
            to.z = z;
            to.y = block->y + t.HeightAt(qx, qz);
            return result;
        }
        float cx = step.x, cz = step.z;
        const auto stop = [&](std::uint32_t bits) {
            PullTowards(cx, cz, t.cx, t.cz, kPullBack);
            block->ToWorld(cx, cz, to.x, to.z);
            to.y = block->y + t.HeightAt(cx, cz);
            return result | bits;
        };
        if (step.kind == MeshStep::Kind::Limit) return stop(nav_walk::kLimit);
        if (step.kind == MeshStep::Kind::Blocked) return stop(nav_walk::kBlocked);
        // An outline edge: through a doorway into the next block, or the floor's end.
        const std::int32_t next = mesh.IsOutline(step.edge) && step.edge < block->doorBlock.size() ? block->doorBlock[step.edge] : -1;
        if (next < 0) return stop(nav_walk::kBlocked);
        const std::uint32_t edge = block->doorEdge[step.edge];
        // A shut door at the doorway (its edge on either side, or the cell beyond) stops the walk.
        if (blocked != nullptr && !blocked->empty()) {
            const NavObjectMesh& beyond = *blocks_[static_cast<std::size_t>(next)].mesh;
            const std::uint8_t here = mesh.edges[step.edge].event, there = beyond.edges[edge].event;
            if (((here & 0xC0u) != 0 && CellBlocked(mesh, here, *blocked)) || ((there & 0xC0u) != 0 && CellBlocked(beyond, there, *blocked)) ||
                CellBlocked(beyond, beyond.triangles[beyond.edges[edge].cellA].event, *blocked)) {
                return stop(nav_walk::kBlocked);
            }
        }
        float wx = 0, wz = 0;
        block->ToWorld(cx, cz, wx, wz);
        index = static_cast<std::uint32_t>(next);
        block = &blocks_[index];
        block->ToLocal(wx, wz, px, pz);
        block->ToLocal(x, z, qx, qz);
        OntoEdge(*block->mesh, edge, px, pz);
        tri = block->mesh->edges[edge].cellA;
        if (zones != nullptr) NavZoneOfCell(*block->mesh, tri, *zones); // a doorway straight into a zone
        cameFrom = edge;
        result |= nav_walk::kLink;
    }
}

bool NavDungeon::Ray(const float origin[3], const float direction[3], float maxLength, NavHit& hit) const {
    float best = maxLength;
    bool found = false;
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        const Block& block = blocks_[i];
        if (!block.mesh) continue;
        float enter = 0;
        if (!RayBox(origin, direction, block.min, block.max, best, enter)) continue;
        // The ray in the block's space.
        float lox = 0, loz = 0;
        block.ToLocal(origin[0], origin[2], lox, loz);
        const float lo[3] = {lox, origin[1] - block.y, loz};
        const float ld[3] = {direction[0] * block.cos + direction[2] * block.sin, direction[1], -direction[0] * block.sin + direction[2] * block.cos};
        const NavObjectMesh& mesh = *block.mesh;
        for (const NavObjectMesh::Triangle& t : mesh.triangles) {
            const auto& a = mesh.vertices[t.v[0]];
            const auto& b = mesh.vertices[t.v[1]];
            const auto& c = mesh.vertices[t.v[2]];
            const float pa[3] = {a.x, a.y, a.z}, pb[3] = {b.x, b.y, b.z}, pc[3] = {c.x, c.y, c.z};
            float s = 0;
            if (!RayTriangle(lo, ld, pa, pb, pc, s) || s >= best) continue;
            best = s;
            found = true;
            hit.region = region_;
            hit.x = origin[0] + direction[0] * s;
            hit.y = origin[1] + direction[1] * s;
            hit.z = origin[2] + direction[2] * s;
            hit.distance = s;
            hit.instance = static_cast<std::int32_t>(i);
        }
    }
    return found;
}

} // namespace sr::game
