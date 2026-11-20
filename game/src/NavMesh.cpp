#include "sr/game/NavMesh.h"

#include "sr/game/NavDungeon.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "NavShared.h"

namespace sr::game {

// Helpers the dungeons share (NavShared.h), and the outdoor world's own.
namespace detail {

float Cross2(float ax, float az, float bx, float bz) { return ax * bz - az * bx; }

// The segment p->q against the segment a->b: their crossing at p + t (q - p) = a + u (b - a).
bool SegmentCross(float px, float pz, float qx, float qz, float ax, float az, float bx, float bz, float& t, float& u) {
    const float rx = qx - px, rz = qz - pz, sx = bx - ax, sz = bz - az;
    const float denom = Cross2(rx, rz, sx, sz);
    if (std::fabs(denom) < 1e-12f) return false; // parallel
    const float wx = ax - px, wz = az - pz;
    t = Cross2(wx, wz, sx, sz) / denom;
    u = Cross2(wx, wz, rx, rz) / denom;
    return true;
}

// Moves (x, z) towards (cx, cz) by `distance` (to it when nearer).
void PullTowards(float& x, float& z, float cx, float cz, float distance) {
    const float dx = cx - x, dz = cz - z;
    const float length = std::sqrt(dx * dx + dz * dz);
    if (length <= distance) {
        x = cx;
        z = cz;
        return;
    }
    x += dx / length * distance;
    z += dz / length * distance;
}

void OntoEdge(const NavObjectMesh& mesh, std::uint32_t edge, float& x, float& z) {
    const NavObjectMesh::Vertex& a = mesh.vertices[mesh.edges[edge].a];
    const NavObjectMesh::Vertex& b = mesh.vertices[mesh.edges[edge].b];
    const float ex = b.x - a.x, ez = b.z - a.z, length2 = ex * ex + ez * ez;
    const float t = length2 > 0.0f ? std::clamp(((x - a.x) * ex + (z - a.z) * ez) / length2, 0.0f, 1.0f) : 0.0f;
    x = a.x + t * ex;
    z = a.z + t * ez;
}

bool RayTriangle(const float o[3], const float d[3], const float a[3], const float b[3], const float c[3], float& t) {
    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (std::fabs(det) < 1e-12f) return false;
    const float inv = 1.0f / det;
    const float s[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
    const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < -1e-5f || u > 1.0f + 1e-5f) return false;
    const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
    const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
    if (v < -1e-5f || u + v > 1.0f + 1e-5f) return false;
    t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    return t >= 0.0f;
}

// The ray's entry and exit of a box; false when it misses it within [0, maxT].
bool RayBox(const float o[3], const float d[3], const float lo[3], const float hi[3], float maxT, float& enter) {
    float t0 = 0.0f, t1 = maxT;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-12f) {
            if (o[i] < lo[i] || o[i] > hi[i]) return false;
            continue;
        }
        float a = (lo[i] - o[i]) / d[i], b = (hi[i] - o[i]) / d[i];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    enter = t0;
    return true;
}

bool PointInTriangle(float px, float pz, const NavObjectMesh::Vertex& a, const NavObjectMesh::Vertex& b,
                     const NavObjectMesh::Vertex& c) {
    const float d1 = Cross2(b.x - a.x, b.z - a.z, px - a.x, pz - a.z);
    const float d2 = Cross2(c.x - b.x, c.z - b.z, px - b.x, pz - b.z);
    const float d3 = Cross2(a.x - c.x, a.z - c.z, px - c.x, pz - c.z);
    constexpr float e = 1e-3f;
    const bool negative = d1 < -e || d2 < -e || d3 < -e;
    const bool positive = d1 > e || d2 > e || d3 > e;
    return !(negative && positive);
}

// The triangle under (x, z) whose height is nearest to y (object space); -1 if none.
std::int32_t TriangleUnder(const NavObjectMesh& mesh, float x, float z, float y, float& height) {
    std::int32_t best = -1;
    float bestDistance = kInfinity;
    for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
        const NavObjectMesh::Triangle& t = mesh.triangles[i];
        const auto& a = mesh.vertices[t.v[0]];
        const auto& b = mesh.vertices[t.v[1]];
        const auto& c = mesh.vertices[t.v[2]];
        if (x < std::min({a.x, b.x, c.x}) - 0.01f || x > std::max({a.x, b.x, c.x}) + 0.01f ||
            z < std::min({a.z, b.z, c.z}) - 0.01f || z > std::max({a.z, b.z, c.z}) + 0.01f) {
            continue;
        }
        if (!PointInTriangle(x, z, a, b, c)) continue;
        const float h = t.HeightAt(x, z);
        if (std::fabs(h - y) < bestDistance) {
            bestDistance = std::fabs(h - y);
            best = static_cast<std::int32_t>(i);
            height = h;
        }
    }
    return best;
}

// The region and offset (x, z) falls in, starting from `region`; false off the grid.
bool WrapRegion(std::uint16_t& region, float& x, float& z) {
    const float fx = std::floor(x / kNavRegionSize), fz = std::floor(z / kNavRegionSize);
    const int rx = NavRegionX(region) + static_cast<int>(fx), rz = NavRegionZ(region) + static_cast<int>(fz);
    if (rx < 0 || rx > 255 || rz < 0 || rz > 255) return false;
    x -= fx * kNavRegionSize;
    z -= fz * kNavRegionSize;
    // A value a hair below a multiple of 1920 can round to 1920 itself.
    x = std::min(x, std::nextafter(kNavRegionSize, 0.0f));
    z = std::min(z, std::nextafter(kNavRegionSize, 0.0f));
    region = NavRegionId(rx, rz);
    return true;
}

// The offset from region `from` to region `to`, in units.
void RegionOffset(std::uint16_t from, std::uint16_t to, float& dx, float& dz) {
    dx = static_cast<float>(NavRegionX(to) - NavRegionX(from)) * kNavRegionSize;
    dz = static_cast<float>(NavRegionZ(to) - NavRegionZ(from)) * kNavRegionSize;
}

std::int32_t InstanceByKey(const NavRegion& region, std::uint32_t key) {
    for (std::size_t i = 0; i < region.instances.size(); ++i) {
        if (region.instances[i].key == key && region.instances[i].Walkable()) return static_cast<std::int32_t>(i);
    }
    return -1;
}

// Whether a cell's event names a zone standing in the way ("zone_r90": the name and the object's turn).
bool CellBlocked(const NavObjectMesh& mesh, std::uint8_t event, const std::unordered_set<std::string>& blocked) {
    if (event == 0 || blocked.empty()) return false;
    const std::size_t index = event & 0x3Fu;
    if (index >= mesh.events.size()) return false;
    std::string name = mesh.events[index];
    if (const std::size_t turn = name.rfind("_r"); turn != std::string::npos) name.resize(turn);
    return blocked.contains(name);
}

MeshStep WalkTriangles(const NavObjectMesh& mesh, std::uint32_t tri, float px, float pz, float qx, float qz, std::int64_t cameFrom,
                       std::uint32_t& crossings, const std::unordered_set<std::string>& blockedZones, std::vector<std::string>* zones) {
    for (;;) {
        const NavObjectMesh::Triangle& t = mesh.triangles[tri];
        // The first edge the rest of the walk leaves this triangle through.
        float best = kInfinity;
        std::uint32_t bestEdge = 0;
        for (std::uint32_t k = 0; k < t.edgeCount; ++k) {
            const std::uint32_t edge = mesh.triangleEdges[t.firstEdge + k];
            if (static_cast<std::int64_t>(edge) == cameFrom) continue;
            const NavObjectMesh::Edge& e = mesh.edges[edge];
            const auto& a = mesh.vertices[e.a];
            const auto& b = mesh.vertices[e.b];
            float s = 0, u = 0;
            if (!SegmentCross(px, pz, qx, qz, a.x, a.z, b.x, b.z, s, u)) continue;
            // A crossing right at the start counts: the walk passed through a vertex onto this
            // edge, or the point sits a hair outside the triangle.
            if (s < -kSegmentEpsilon || s > 1.0f + kSegmentEpsilon || u < -kEdgeEpsilon || u > 1.0f + kEdgeEpsilon) continue;
            const float inside = Cross2(b.x - a.x, b.z - a.z, t.cx - a.x, t.cz - a.z);
            const float target = Cross2(b.x - a.x, b.z - a.z, qx - a.x, qz - a.z);
            if (inside * target >= 0.0f) continue; // the target is not beyond this edge
            if (s < best) {
                best = s;
                bestEdge = edge;
            }
        }
        MeshStep step;
        step.tri = tri;
        if (best == kInfinity) return step;
        step.x = px + best * (qx - px);
        step.z = pz + best * (qz - pz);
        step.edge = bestEdge;
        if (crossings == 0) {
            step.kind = MeshStep::Kind::Limit;
            return step;
        }
        --crossings;
        const NavObjectMesh::Edge& e = mesh.edges[bestEdge];
        if (mesh.IsOutline(bestEdge)) {
            step.kind = MeshStep::Kind::Outline;
            return step;
        }
        const bool fromA = e.cellA == tri;
        if ((fromA && (e.flags & nav_edge::kBlockAToB)) || (!fromA && (e.flags & nav_edge::kBlockBToA))) {
            step.kind = MeshStep::Kind::Blocked;
            return step;
        }
        if (CellBlocked(mesh, mesh.triangles[fromA ? e.cellB : e.cellA].event, blockedZones)) { // entering it: leaving one is free
            step.kind = MeshStep::Kind::Blocked;
            return step;
        }
        // An edge marked with a zone standing in the way (a Forgotten World door across a doorway).
        if ((e.event & 0xC0u) != 0 && CellBlocked(mesh, e.event, blockedZones)) {
            step.kind = MeshStep::Kind::Blocked;
            return step;
        }
        if (zones != nullptr) NavZonesEntered(mesh, bestEdge, tri, fromA ? e.cellB : e.cellA, *zones);
        tri = fromA ? e.cellB : e.cellA;
        px = step.x;
        pz = step.z;
        cameFrom = bestEdge;
    }
}

// Where the rest of a terrain walk first enters an object listed in its cell.
struct Entry {
    float t = kInfinity; // along the segment
    std::int32_t instance = -1;
    std::uint32_t edge = 0;
    float u = 0;             // along the edge
    float lx = 0, lz = 0;    // object space
    float wx = 0, wz = 0;    // region space
};

void TestEntry(const NavRegion::Instance& inst, std::int32_t index, float px, float pz, float qx, float qz,
               const assets::RegionNavData::Cell& cell, Entry& best) {
    if (std::max(px, qx) < inst.minX || std::min(px, qx) > inst.maxX || std::max(pz, qz) < inst.minZ ||
        std::min(pz, qz) > inst.maxZ) {
        return;
    }
    const NavObjectMesh& mesh = *inst.mesh;
    float lpx, lpz, lqx, lqz;
    inst.ToLocal(px, pz, lpx, lpz);
    inst.ToLocal(qx, qz, lqx, lqz);
    const auto test = [&](std::uint32_t k) {
        const NavObjectMesh::Edge& e = mesh.edges[k];
        if (e.flags & nav_edge::kLedge) return; // not an entry from outside
        const auto& a = mesh.vertices[e.a];
        const auto& b = mesh.vertices[e.b];
        float t = 0, u = 0;
        if (!SegmentCross(lpx, lpz, lqx, lqz, a.x, a.z, b.x, b.z, t, u)) return;
        if (t <= kSegmentEpsilon || t > 1.0f || u < -kEdgeEpsilon || u > 1.0f + kEdgeEpsilon || t >= best.t) return;
        // Only a crossing from outside in (the edge's triangle lies inside).
        const NavObjectMesh::Triangle& tri = mesh.triangles[e.cellA];
        const float inside = Cross2(b.x - a.x, b.z - a.z, tri.cx - a.x, tri.cz - a.z);
        const float start = Cross2(b.x - a.x, b.z - a.z, lpx - a.x, lpz - a.z);
        if (inside * start >= 0.0f) return;
        Entry entry;
        entry.t = t;
        entry.instance = index;
        entry.edge = k;
        entry.u = u;
        entry.lx = lpx + t * (lqx - lpx);
        entry.lz = lpz + t * (lqz - lpz);
        inst.ToWorld(entry.lx, entry.lz, entry.wx, entry.wz);
        // Entered from this cell (another cell that lists the object finds its own entries).
        constexpr float slack = 0.5f;
        if (entry.wx < cell.minX - slack || entry.wx > cell.maxX + slack || entry.wz < cell.minZ - slack ||
            entry.wz > cell.maxZ + slack) {
            return;
        }
        best = entry;
    };
    const assets::ObjectNavMesh::Grid& grid = mesh.grid;
    if (grid.width == 0 || grid.height == 0 || grid.start.size() != std::size_t{grid.width} * grid.height + 1) {
        for (std::uint32_t k = 0; k < mesh.outlineCount; ++k) test(k);
        return;
    }
    const auto tileOf = [](float v, float origin, std::uint32_t count) {
        const int i = static_cast<int>(std::floor((v - origin) / assets::ObjectNavMesh::kGridTile));
        return std::clamp(i, 0, static_cast<int>(count) - 1);
    };
    const int x0 = tileOf(std::min(lpx, lqx), grid.originX, grid.width), x1 = tileOf(std::max(lpx, lqx), grid.originX, grid.width);
    const int z0 = tileOf(std::min(lpz, lqz), grid.originZ, grid.height), z1 = tileOf(std::max(lpz, lqz), grid.originZ, grid.height);
    for (int gz = z0; gz <= z1; ++gz) {
        for (int gx = x0; gx <= x1; ++gx) {
            const std::size_t tile = static_cast<std::size_t>(gz) * grid.width + static_cast<std::size_t>(gx);
            for (std::uint32_t i = grid.start[tile]; i < grid.start[tile + 1]; ++i) test(grid.edges[i]);
        }
    }
}

// The terrain's surface under the ray within one tile; also the walkable water over it.
bool RayTile(const NavRegion& region, int tx, int tz, const float o[3], const float d[3], float maxT, float& t) {
    const auto& h = region.data.heights;
    const auto at = [&](int x, int z) { return h[static_cast<std::size_t>(z) * assets::kNavHeights + static_cast<std::size_t>(x)]; };
    const float x0 = static_cast<float>(tx) * kNavTileSize, z0 = static_cast<float>(tz) * kNavTileSize;
    const float p00[3] = {x0, at(tx, tz), z0}, p10[3] = {x0 + kNavTileSize, at(tx + 1, tz), z0};
    const float p01[3] = {x0, at(tx, tz + 1), z0 + kNavTileSize}, p11[3] = {x0 + kNavTileSize, at(tx + 1, tz + 1), z0 + kNavTileSize};
    const std::size_t block = static_cast<std::size_t>(tz / 16) * assets::kNavBlocks + static_cast<std::size_t>(tx / 16);
    const bool water = (region.data.blockFlags[block] & 2) != 0;
    const float waterY = region.data.waterHeights[block];
    bool found = false;
    t = maxT;
    const auto consider = [&](const float* a, const float* b, const float* c) {
        float hit = 0;
        if (!RayTriangle(o, d, a, b, c, hit) || hit > t) return;
        if (water && o[1] + d[1] * hit < waterY) return; // under the walkable water
        t = hit;
        found = true;
    };
    if (((tx ^ tz) & 1) == 0) {
        consider(p00, p01, p11);
        consider(p00, p11, p10);
    } else {
        consider(p00, p10, p01);
        consider(p10, p11, p01);
    }
    if (water && std::fabs(d[1]) > 1e-12f) {
        const float hit = (waterY - o[1]) / d[1];
        const float x = o[0] + d[0] * hit, z = o[2] + d[2] * hit;
        if (hit >= 0.0f && hit < t && x >= x0 && x <= x0 + kNavTileSize && z >= z0 && z <= z0 + kNavTileSize &&
            NavTerrainHeight(region, x, z) <= waterY + 1e-3f) {
            t = hit;
            found = true;
        }
    }
    return found;
}

// The terrain of one region against the ray (region-local), tile by tile along it.
bool RayTerrain(const NavRegion& region, const float o[3], const float d[3], float maxT, float& t) {
    // The part of the ray over the region.
    float t0 = 0.0f, t1 = maxT;
    for (int axis : {0, 2}) {
        if (std::fabs(d[axis]) < 1e-12f) {
            if (o[axis] < 0.0f || o[axis] > kNavRegionSize) return false;
            continue;
        }
        float a = (0.0f - o[axis]) / d[axis], b = (kNavRegionSize - o[axis]) / d[axis];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
    }
    if (t0 > t1) return false;
    const auto tileOf = [](float v) { return std::clamp(static_cast<int>(std::floor(v / kNavTileSize)), 0, assets::kNavTiles - 1); };
    int tx = tileOf(o[0] + d[0] * t0), tz = tileOf(o[2] + d[2] * t0);
    const int stepX = d[0] > 0 ? 1 : -1, stepZ = d[2] > 0 ? 1 : -1;
    const auto next = [&](int tile, int step, int axis) {
        if (std::fabs(d[axis]) < 1e-12f) return kInfinity;
        const float edge = static_cast<float>(tile + (step > 0 ? 1 : 0)) * kNavTileSize;
        return (edge - o[axis]) / d[axis];
    };
    float nextX = next(tx, stepX, 0), nextZ = next(tz, stepZ, 2);
    const float deltaX = std::fabs(d[0]) < 1e-12f ? kInfinity : kNavTileSize / std::fabs(d[0]);
    const float deltaZ = std::fabs(d[2]) < 1e-12f ? kInfinity : kNavTileSize / std::fabs(d[2]);
    float enter = t0;
    while (enter <= t1) {
        if (RayTile(region, tx, tz, o, d, maxT, t)) return true; // tiles come in order along the ray
        if (nextX < nextZ) {
            tx += stepX;
            enter = nextX;
            nextX += deltaX;
        } else {
            tz += stepZ;
            enter = nextZ;
            nextZ += deltaZ;
        }
        if (tx < 0 || tx >= assets::kNavTiles || tz < 0 || tz >= assets::kNavTiles) break;
    }
    return false;
}

} // namespace detail

using namespace detail;

float NavObjectMesh::Triangle::HeightAt(float x, float z) const {
    if (std::fabs(plane[1]) < 1e-6f) return cy; // a vertical triangle: no height of its own
    return -(plane[0] * x + plane[2] * z + plane[3]) / plane[1];
}

std::shared_ptr<const NavObjectMesh> NavObjectMesh::Build(const assets::ObjectNavMesh& mesh) {
    auto out = std::make_shared<NavObjectMesh>();
    out->vertices.resize(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        Vertex& v = out->vertices[i];
        v.x = mesh.vertices[i][0];
        v.y = mesh.vertices[i][1];
        v.z = mesh.vertices[i][2];
        const float angle = static_cast<float>(i < mesh.directions.size() ? mesh.directions[i] : 0) * (6.28318530718f / 256.0f);
        v.nx = std::cos(angle);
        v.nz = -std::sin(angle);
    }
    out->outlineCount = static_cast<std::uint32_t>(mesh.outline.size());
    out->edges.reserve(mesh.outline.size() + mesh.internal.size());
    for (const auto* list : {&mesh.outline, &mesh.internal}) {
        for (const assets::ObjectNavMesh::Edge& e : *list) out->edges.push_back({e.a, e.b, e.cell, e.neighbour, e.flags, e.event});
    }
    out->triangles.resize(mesh.cells.size());
    for (std::size_t i = 0; i < mesh.cells.size(); ++i) {
        Triangle& t = out->triangles[i];
        for (int k = 0; k < 3; ++k) t.v[k] = mesh.cells[i][static_cast<std::size_t>(k)];
        const Vertex& a = out->vertices[t.v[0]];
        const Vertex& b = out->vertices[t.v[1]];
        const Vertex& c = out->vertices[t.v[2]];
        const float e1[3] = {b.x - a.x, b.y - a.y, b.z - a.z}, e2[3] = {c.x - a.x, c.y - a.y, c.z - a.z};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (length > 0.0f) {
            for (float& v : n) v /= length;
        }
        t.plane[0] = n[0];
        t.plane[1] = n[1];
        t.plane[2] = n[2];
        t.plane[3] = -(n[0] * a.x + n[1] * a.y + n[2] * a.z);
        t.cx = (a.x + b.x + c.x) / 3.0f;
        t.cy = (a.y + b.y + c.y) / 3.0f;
        t.cz = (a.z + b.z + c.z) / 3.0f;
        t.event = i < mesh.cellEvents.size() ? mesh.cellEvents[i] : 0;
    }
    // Each triangle's edges.
    std::vector<std::uint32_t> counts(out->triangles.size() + 1, 0);
    for (const Edge& e : out->edges) {
        ++counts[e.cellA];
        if (e.cellB != 0xFFFF) ++counts[e.cellB];
    }
    std::uint32_t total = 0;
    for (std::size_t i = 0; i < out->triangles.size(); ++i) {
        out->triangles[i].firstEdge = total;
        out->triangles[i].edgeCount = static_cast<std::uint8_t>(std::min<std::uint32_t>(counts[i], 255));
        total += counts[i];
    }
    out->triangleEdges.resize(total);
    std::vector<std::uint32_t> fill(out->triangles.size(), 0);
    for (std::uint32_t k = 0; k < out->edges.size(); ++k) {
        const Edge& e = out->edges[k];
        for (const std::uint16_t cell : {e.cellA, e.cellB}) {
            if (cell == 0xFFFF) continue;
            out->triangleEdges[out->triangles[cell].firstEdge + fill[cell]++] = k;
        }
    }
    out->grid = mesh.grid;
    out->events = mesh.events;
    out->marker = mesh.IsEventMarker();
    for (int i = 0; i < 3; ++i) {
        out->min[i] = mesh.bounds.min[static_cast<std::size_t>(i)];
        out->max[i] = mesh.bounds.max[static_cast<std::size_t>(i)];
    }
    return out;
}

void NavRegion::Instance::ToLocal(float wx, float wz, float& lx, float& lz) const {
    const float dx = wx - x, dz = wz - z;
    lx = dx * cos + dz * sin;
    lz = -dx * sin + dz * cos;
}

void NavRegion::Instance::ToWorld(float lx, float lz, float& wx, float& wz) const {
    wx = lx * cos - lz * sin + x;
    wz = lx * sin + lz * cos + z;
}

const assets::RegionNavData::Tile& NavRegion::TileAt(float x, float z) const {
    const int tx = std::clamp(static_cast<int>(std::floor(x / kNavTileSize)), 0, assets::kNavTiles - 1);
    const int tz = std::clamp(static_cast<int>(std::floor(z / kNavTileSize)), 0, assets::kNavTiles - 1);
    return data.tiles[static_cast<std::size_t>(tz) * assets::kNavTiles + static_cast<std::size_t>(tx)];
}

void NavRegion::Link() {
    for (std::size_t i = 0; i < instances.size(); ++i) {
        Instance& inst = instances[i];
        const assets::RegionNavData::Object& o = data.objects[i];
        inst.x = o.position[0];
        inst.y = o.position[1];
        inst.z = o.position[2];
        inst.cos = std::cos(o.yaw);
        inst.sin = std::sin(o.yaw);
        inst.key = (std::uint32_t{o.ownerRegion} << 16) | o.uid;
        inst.minX = inst.maxX = inst.x;
        inst.minZ = inst.maxZ = inst.z;
        inst.minY = inst.maxY = inst.y;
        if (!inst.mesh) continue;
        const NavObjectMesh& mesh = *inst.mesh;
        inst.minX = inst.minZ = kInfinity;
        inst.maxX = inst.maxZ = -kInfinity;
        for (const float lx : {mesh.min[0], mesh.max[0]}) {
            for (const float lz : {mesh.min[2], mesh.max[2]}) {
                float wx = 0, wz = 0;
                inst.ToWorld(lx, lz, wx, wz);
                inst.minX = std::min(inst.minX, wx);
                inst.maxX = std::max(inst.maxX, wx);
                inst.minZ = std::min(inst.minZ, wz);
                inst.maxZ = std::max(inst.maxZ, wz);
            }
        }
        inst.minY = inst.y + mesh.min[1];
        inst.maxY = inst.y + mesh.max[1];
    }

    // The cells' side lists: an internal edge on its side of cell A and, when open, of cell B; a
    // border edge on its side of cell A.
    const std::size_t slots = data.cells.size() * 4;
    sideStart.assign(slots + 1, 0);
    const auto each = [&](auto&& fn) {
        for (std::uint32_t k = 0; k < data.internalEdges.size(); ++k) {
            const assets::RegionNavData::Edge& e = data.internalEdges[k];
            fn(std::size_t{e.cellA} * 4 + e.sideA, k);
            if ((e.flags & 3) == 0 && e.cellB != 0xFFFF && e.sideB <= 3) fn(std::size_t{e.cellB} * 4 + e.sideB, k);
        }
        for (std::uint32_t k = 0; k < data.borderEdges.size(); ++k) {
            const assets::RegionNavData::Edge& e = data.borderEdges[k];
            fn(std::size_t{e.cellA} * 4 + e.sideA, kBorderEdge | k);
        }
    };
    each([&](std::size_t slot, std::uint32_t) { ++sideStart[slot + 1]; });
    for (std::size_t i = 0; i < slots; ++i) sideStart[i + 1] += sideStart[i];
    sideEdges.resize(sideStart[slots]);
    std::vector<std::uint32_t> fill(slots, 0);
    each([&](std::size_t slot, std::uint32_t ref) { sideEdges[sideStart[slot] + fill[slot]++] = ref; });
}

float NavTerrainHeight(const NavRegion& region, float x, float z) {
    const int tx = std::clamp(static_cast<int>(std::floor(x / kNavTileSize)), 0, assets::kNavTiles - 1);
    const int tz = std::clamp(static_cast<int>(std::floor(z / kNavTileSize)), 0, assets::kNavTiles - 1);
    const float fx = std::clamp((x - static_cast<float>(tx) * kNavTileSize) / kNavTileSize, 0.0f, 1.0f);
    const float fz = std::clamp((z - static_cast<float>(tz) * kNavTileSize) / kNavTileSize, 0.0f, 1.0f);
    const auto& h = region.data.heights;
    const std::size_t row = static_cast<std::size_t>(tz) * assets::kNavHeights + static_cast<std::size_t>(tx);
    const float h00 = h[row], h10 = h[row + 1], h01 = h[row + assets::kNavHeights], h11 = h[row + assets::kNavHeights + 1];
    float y = 0;
    if (((tx ^ tz) & 1) == 0) {
        // Diagonal (0,0)-(1,1): the triangle on the z side of it, or the one on the x side.
        y = fz >= fx ? h00 + fz * (h01 - h00) + fx * (h11 - h01) : h00 + fx * (h10 - h00) + fz * (h11 - h10);
    } else {
        // Diagonal (1,0)-(0,1).
        y = fx + fz <= 1.0f ? h00 + fx * (h10 - h00) + fz * (h01 - h00)
                            : h11 + (1.0f - fx) * (h01 - h11) + (1.0f - fz) * (h10 - h11);
    }
    const std::size_t block = static_cast<std::size_t>(tz / 16) * assets::kNavBlocks + static_cast<std::size_t>(tx / 16);
    if ((region.data.blockFlags[block] & 2) != 0) y = std::max(y, region.data.waterHeights[block]);
    return y;
}

namespace {

void AddZone(const NavObjectMesh& mesh, std::uint8_t event, std::vector<std::string>& zones) {
    const std::size_t index = event & 0x3Fu;
    if (index >= mesh.events.size()) return;
    if (std::find(zones.begin(), zones.end(), mesh.events[index]) == zones.end()) zones.push_back(mesh.events[index]);
}

} // namespace

void NavZonesEntered(const NavObjectMesh& mesh, std::uint32_t edge, std::uint32_t from, std::uint32_t into, std::vector<std::string>& zones) {
    if (edge < mesh.edges.size() && (mesh.edges[edge].event & 0xC0u) != 0) AddZone(mesh, mesh.edges[edge].event, zones);
    if (into >= mesh.triangles.size()) return;
    const std::uint8_t event = mesh.triangles[into].event;
    if ((event & 0xC0u) == 0) return;
    if (from < mesh.triangles.size() && mesh.triangles[from].event == event) return; // inside the zone already
    AddZone(mesh, event, zones);
}

void NavZoneOfCell(const NavObjectMesh& mesh, std::uint32_t cell, std::vector<std::string>& zones) {
    if (cell < mesh.triangles.size() && (mesh.triangles[cell].event & 0xC0u) != 0) AddZone(mesh, mesh.triangles[cell].event, zones);
}

void NavWorld::Add(std::shared_ptr<const NavRegion> region) {
    if (region) regions_[region->id] = std::move(region);
}

void NavWorld::AddDungeon(std::shared_ptr<const NavDungeon> dungeon) {
    if (dungeon) dungeons_[dungeon->Region()] = std::move(dungeon);
}

void NavWorld::Remove(std::uint16_t region) {
    regions_.erase(region);
    dungeons_.erase(region);
}

const NavRegion* NavWorld::Find(std::uint16_t region) const {
    const auto it = regions_.find(region);
    return it == regions_.end() ? nullptr : it->second.get();
}

const NavDungeon* NavWorld::FindDungeon(std::uint16_t region) const {
    const auto it = dungeons_.find(region);
    return it == dungeons_.end() ? nullptr : it->second.get();
}

bool NavWorld::TerrainHeight(std::uint16_t region, float x, float z, float& y) const {
    if (NavIsDungeon(region)) return false; // no terrain
    if (!WrapRegion(region, x, z)) return false;
    const NavRegion* nav = Find(region);
    if (nav == nullptr) return false;
    y = NavTerrainHeight(*nav, x, z);
    return true;
}

NavPosition NavWorld::Drop(std::uint16_t region, float x, float y, float z) const {
    NavPosition out;
    if (NavIsDungeon(region)) {
        const NavDungeon* dungeon = FindDungeon(region);
        return dungeon != nullptr ? dungeon->Drop(x, y, z) : out;
    }
    if (!WrapRegion(region, x, z)) return out;
    const NavRegion* nav = Find(region);
    if (nav == nullptr) return out;
    out.region = region;
    out.x = x;
    out.z = z;
    const assets::RegionNavData::Tile& tile = nav->TileAt(x, z);
    out.y = NavTerrainHeight(*nav, x, z);
    std::int32_t cell = tile.cell, instance = -1;
    float distance = std::fabs(out.y - y);
    const assets::RegionNavData::Cell& c = nav->data.cells[tile.cell];
    for (std::uint32_t k = 0; k < c.objectCount; ++k) {
        const std::uint16_t i = nav->data.cellObjects[c.firstObject + k];
        const NavRegion::Instance& inst = nav->instances[i];
        if (!inst.Walkable() || inst.mesh->marker) continue;
        if (x < inst.minX || x > inst.maxX || z < inst.minZ || z > inst.maxZ) continue;
        float lx = 0, lz = 0, height = 0;
        inst.ToLocal(x, z, lx, lz);
        const std::int32_t tri = TriangleUnder(*inst.mesh, lx, lz, y - inst.y, height);
        if (tri < 0 || std::fabs(height + inst.y - y) >= distance) continue;
        distance = std::fabs(height + inst.y - y);
        out.y = height + inst.y;
        cell = tri;
        instance = i;
    }
    // An object's floor lying on the terrain (a stair's step, a deck, no more than 3 above the ground there) is what a
    // walker there stands on, not the terrain under it: one put on that terrain could not walk off it (Hotan's palace
    // stairs, 2026-11-13: the terrain 1.2 under the steps, closed round; ours).
    if (instance < 0) {
        constexpr float kOnTerrain = 3.0f;
        const float ground = NavTerrainHeight(*nav, x, z);
        for (std::uint32_t k = 0; k < c.objectCount; ++k) {
            const std::uint16_t i = nav->data.cellObjects[c.firstObject + k];
            const NavRegion::Instance& inst = nav->instances[i];
            if (!inst.Walkable() || inst.mesh->marker) continue;
            if (x < inst.minX || x > inst.maxX || z < inst.minZ || z > inst.maxZ) continue;
            float lx = 0, lz = 0, height = 0;
            inst.ToLocal(x, z, lx, lz);
            const std::int32_t tri = TriangleUnder(*inst.mesh, lx, lz, ground + kOnTerrain - inst.y, height);
            if (tri < 0 || height + inst.y < ground - 0.01f || height + inst.y > ground + kOnTerrain) continue;
            out.y = height + inst.y;
            cell = tri;
            instance = i;
            break;
        }
    }
    if (instance < 0 && (tile.flags & 1) != 0) return out; // the terrain wins on a blocked tile
    out.cell = cell;
    out.instance = instance;
    return out;
}

bool NavWorld::Settle(NavPosition& at, float& tx, float& tz) const {
    std::uint16_t region = at.region;
    float x = at.x, z = at.z;
    if (!WrapRegion(region, x, z) || Find(region) == nullptr) return false;
    float dx = 0, dz = 0;
    RegionOffset(at.region, region, dx, dz);
    tx -= dx;
    tz -= dz;
    at.region = region;
    at.x = x;
    at.z = z;
    return true;
}

const std::unordered_set<std::string>& NavWorld::NamesBlocking(std::uint32_t layer) const {
    static const std::unordered_set<std::string> none;
    const auto it = blockingNames_.find(layer);
    return it != blockingNames_.end() ? it->second : none;
}

std::uint32_t NavWorld::Walk(const NavPosition& from, std::uint16_t toRegion, float x, float z, NavPosition& to,
                             std::uint32_t maxCrossings, std::vector<std::string>* zones, std::uint32_t layer) const {
    to = from;
    if (NavIsDungeon(from.region) || NavIsDungeon(toRegion)) {
        const NavDungeon* dungeon = FindDungeon(from.region);
        if (dungeon == nullptr || toRegion != from.region) return nav_walk::kFailed;
        return dungeon->Walk(from, x, z, to, maxCrossings, zones, &NamesBlocking(layer));
    }
    if (!from.Valid() || Find(from.region) == nullptr) return nav_walk::kFailed;
    float dx = 0, dz = 0;
    RegionOffset(from.region, toRegion, dx, dz);
    float tx = x + dx, tz = z + dz;
    if (std::fabs(tx - from.x) > kNavRegionSize || std::fabs(tz - from.z) > kNavRegionSize) return nav_walk::kFailed;
    NavPosition at = from;
    std::uint32_t crossings = maxCrossings, result = nav_walk::kReached;
    std::int32_t ignore = -1;
    for (int leg = 0;; ++leg) {
        if (leg == kMaxLegs) {
            result |= nav_walk::kFailed;
            break;
        }
        const float startDistance = std::hypot(tx - at.x, tz - at.z);
        std::int32_t left = -1;
        result = at.instance < 0 ? TerrainLeg(at, tx, tz, ignore, crossings, zones, layer) : ObjectLeg(at, tx, tz, crossings, left, zones, layer);
        if ((result & (nav_walk::kFailed | nav_walk::kBlocked | nav_walk::kLimit)) != 0) break;
        if ((result & (nav_walk::kOnObject | nav_walk::kLeftObject)) == 0) break;
        if (startDistance < kNearTarget) break;
        ignore = left;
    }
    to = at;
    return result;
}

std::uint32_t NavWorld::TerrainLeg(NavPosition& at, float& tx, float& tz, std::int32_t ignore, std::uint32_t& crossings,
                                   std::vector<std::string>* zones, std::uint32_t layer) const {
    const NavRegion* nav = Find(at.region);
    if (nav == nullptr || at.cell < 0 || static_cast<std::size_t>(at.cell) >= nav->data.cells.size()) return nav_walk::kFailed;
    for (;;) {
        const assets::RegionNavData::Cell& cell = nav->data.cells[static_cast<std::size_t>(at.cell)];
        const float centreX = 0.5f * (cell.minX + cell.maxX), centreZ = 0.5f * (cell.minZ + cell.maxZ);

        // 1. Objects first: the nearest entry into one listed in this cell.
        Entry entry;
        for (std::uint32_t k = 0; k < cell.objectCount; ++k) {
            const std::uint16_t i = nav->data.cellObjects[cell.firstObject + k];
            const NavRegion::Instance& inst = nav->instances[i];
            if (i == ignore || !inst.Walkable() || (inst.mesh->marker && !ZoneBlocks(inst.key, layer))) continue;
            TestEntry(inst, i, at.x, at.z, tx, tz, cell, entry);
        }
        if (entry.instance >= 0) {
            const NavRegion::Instance& inst = nav->instances[static_cast<std::size_t>(entry.instance)];
            const NavObjectMesh& mesh = *inst.mesh;
            const NavObjectMesh::Edge& e = mesh.edges[entry.edge];
            if ((e.flags & nav_edge::kBlockBToA) || mesh.marker) {
                // A wall (or a fortress's gate or tower standing): stay outside, pushed out along the outline normal of its nearer end.
                const NavObjectMesh::Vertex& end = mesh.vertices[entry.u < 0.5f ? e.a : e.b];
                float wx = 0, wz = 0;
                inst.ToWorld(entry.lx + end.nx * kWallPush, entry.lz + end.nz * kWallPush, wx, wz);
                at.x = std::clamp(wx, cell.minX, cell.maxX);
                at.z = std::clamp(wz, cell.minZ, cell.maxZ);
                at.y = NavTerrainHeight(*nav, at.x, at.z);
                return nav_walk::kBlocked;
            }
            at.instance = entry.instance;
            at.cell = e.cellA;
            at.x = entry.wx;
            at.z = entry.wz;
            at.y = inst.y + mesh.triangles[e.cellA].HeightAt(entry.lx, entry.lz);
            if (zones != nullptr) NavZoneOfCell(mesh, e.cellA, *zones); // stepping onto the object into a zone
            return nav_walk::kOnObject;
        }

        // 2. The target in this cell.
        if (tx >= cell.minX && tx <= cell.maxX && tz >= cell.minZ && tz <= cell.maxZ) {
            at.x = tx;
            at.z = tz;
            at.y = NavTerrainHeight(*nav, tx, tz);
            return nav_walk::kReached;
        }

        // 3. Where the walk leaves the cell's rectangle, and the edge there.
        const float dx = tx - at.x, dz = tz - at.z;
        if (dx == 0.0f && dz == 0.0f) {
            at.y = NavTerrainHeight(*nav, at.x, at.z); // there already (a hair outside the rectangle)
            return nav_walk::kReached;
        }
        const float leaveX = dx > 0 ? (cell.maxX - at.x) / dx : dx < 0 ? (cell.minX - at.x) / dx : kInfinity;
        const float leaveZ = dz > 0 ? (cell.maxZ - at.z) / dz : dz < 0 ? (cell.minZ - at.z) / dz : kInfinity;
        const float t = std::clamp(std::min(leaveX, leaveZ), 0.0f, 1.0f);
        float ex = at.x + dx * t, ez = at.z + dz * t;
        const int sideX = dx > 0 ? 1 : 3, sideZ = dz > 0 ? 0 : 2;
        int sides[2] = {leaveX <= leaveZ ? sideX : sideZ, -1};
        if (std::fabs(leaveX - leaveZ) * std::hypot(dx, dz) < kSideEpsilon) sides[1] = sides[0] == sideX ? sideZ : sideX; // a corner
        std::uint32_t crossed = 0;
        int side = -1;
        for (const int s : sides) {
            if (s < 0) continue;
            const bool alongX = s == 0 || s == 2; // the side runs along x
            const float along = alongX ? ex : ez;
            const std::size_t slot = static_cast<std::size_t>(at.cell) * 4 + static_cast<std::size_t>(s);
            for (std::uint32_t i = nav->sideStart[slot]; i < nav->sideStart[slot + 1]; ++i) {
                const assets::RegionNavData::Edge& e = nav->EdgeAt(nav->sideEdges[i]);
                const float lo = alongX ? e.ax : e.az, hi = alongX ? e.bx : e.bz;
                if (along >= lo - kSideEpsilon && along <= hi + kSideEpsilon) {
                    crossed = nav->sideEdges[i];
                    side = s;
                    break;
                }
            }
            if (side >= 0) break;
        }
        // Exactly on the side's line.
        if (side == 1) ex = cell.maxX;
        if (side == 3) ex = cell.minX;
        if (side == 0) ez = cell.maxZ;
        if (side == 2) ez = cell.minZ;
        const auto stop = [&](std::uint32_t bits) {
            PullTowards(ex, ez, centreX, centreZ, kPullBack);
            at.x = ex;
            at.z = ez;
            at.y = NavTerrainHeight(*nav, ex, ez);
            return bits;
        };
        if (side < 0) return stop(nav_walk::kBlocked); // no edge there: a side nothing joins
        if (crossings == 0) return stop(nav_walk::kLimit);
        --crossings;
        const assets::RegionNavData::Edge& e = nav->EdgeAt(crossed);
        if ((e.flags & 3) != 0) return stop(nav_walk::kBlocked);

        if ((crossed & NavRegion::kBorderEdge) != 0) {
            // Into the neighbouring region (not held: blocked).
            const NavRegion* next = Find(e.regionB);
            if (next == nullptr || e.cellB >= next->data.cells.size()) return stop(nav_walk::kBlocked);
            float ox = 0, oz = 0;
            RegionOffset(at.region, e.regionB, ox, oz);
            ex -= ox;
            ez -= oz;
            tx -= ox;
            tz -= oz;
            nav = next;
            at.region = e.regionB;
            at.cell = e.cellB;
        } else {
            const std::uint16_t other = e.cellA == at.cell ? e.cellB : e.cellA;
            if (other >= nav->data.cells.size()) return stop(nav_walk::kBlocked);
            at.cell = other;
        }
        const assets::RegionNavData::Cell& next = nav->data.cells[static_cast<std::size_t>(at.cell)];
        PullTowards(ex, ez, 0.5f * (next.minX + next.maxX), 0.5f * (next.minZ + next.maxZ), kPullBack);
        at.x = ex;
        at.z = ez;
    }
}

std::uint32_t NavWorld::ObjectLeg(NavPosition& at, float& tx, float& tz, std::uint32_t& crossings, std::int32_t& left,
                                  std::vector<std::string>* zones, std::uint32_t layer) const {
    const NavRegion* nav = Find(at.region);
    if (nav == nullptr || at.instance < 0 || static_cast<std::size_t>(at.instance) >= nav->instances.size()) return nav_walk::kFailed;
    std::int32_t index = at.instance;
    const NavRegion::Instance* inst = &nav->instances[static_cast<std::size_t>(index)];
    if (!inst->Walkable() || at.cell < 0 || static_cast<std::size_t>(at.cell) >= inst->mesh->triangles.size()) return nav_walk::kFailed;
    std::uint32_t result = nav_walk::kReached;
    float px = 0, pz = 0, qx = 0, qz = 0;
    inst->ToLocal(at.x, at.z, px, pz);
    inst->ToLocal(tx, tz, qx, qz);
    std::uint32_t tri = static_cast<std::uint32_t>(at.cell);
    std::int64_t cameFrom = -1;

    // After every stop on an object: if the object reaches into a neighbouring region and the
    // walker is past the border, it carries on as the neighbour's instance of the same object.
    const auto rehome = [&]() {
        std::uint16_t region = at.region;
        float x = at.x, z = at.z;
        if (!WrapRegion(region, x, z) || region == at.region) return;
        const NavRegion* next = Find(region);
        if (next == nullptr) return;
        const std::int32_t j = InstanceByKey(*next, nav->instances[static_cast<std::size_t>(at.instance)].key);
        if (j < 0) return;
        float ox = 0, oz = 0;
        RegionOffset(at.region, region, ox, oz);
        tx -= ox;
        tz -= oz;
        at.region = region;
        at.x = x;
        at.z = z;
        at.instance = j;
    };

    for (;;) {
        const NavObjectMesh& mesh = *inst->mesh;
        const MeshStep step = WalkTriangles(mesh, tri, px, pz, qx, qz, cameFrom, crossings, NamesBlocking(layer), zones);
        tri = step.tri;
        const NavObjectMesh::Triangle& t = mesh.triangles[tri];
        if (step.kind == MeshStep::Kind::Inside) {
            // The target is on this triangle.
            at.x = tx;
            at.z = tz;
            at.cell = static_cast<std::int32_t>(tri);
            at.y = inst->y + t.HeightAt(qx, qz);
            rehome();
            return result;
        }
        float cx = step.x, cz = step.z;
        const auto stop = [&](std::uint32_t bits) {
            PullTowards(cx, cz, t.cx, t.cz, kPullBack);
            inst->ToWorld(cx, cz, at.x, at.z);
            at.instance = index;
            at.cell = static_cast<std::int32_t>(tri);
            at.y = inst->y + t.HeightAt(cx, cz);
            rehome();
            return result | bits;
        };
        if (step.kind == MeshStep::Kind::Limit) return stop(nav_walk::kLimit);
        if (step.kind == MeshStep::Kind::Blocked) return stop(nav_walk::kBlocked);
        const std::uint32_t bestEdge = step.edge;
        const NavObjectMesh::Edge& e = mesh.edges[bestEdge];

        if (e.flags & nav_edge::kLink) {
            // A door: the region's link list says which object and edge it opens onto.
            const auto findLink = [&](const NavRegion& r, std::int32_t i) -> const assets::RegionNavData::Link* {
                const assets::RegionNavData::Link* found = nullptr;
                const assets::RegionNavData::Object& object = r.data.objects[static_cast<std::size_t>(i)];
                for (std::uint32_t k = 0; k < object.linkCount; ++k) {
                    const assets::RegionNavData::Link& l = r.data.links[object.firstLink + k];
                    if (l.ownEdge == bestEdge) found = &l;
                }
                return found;
            };
            const assets::RegionNavData::Link* link = findLink(*nav, index);
            // An object reaching into a neighbouring region has the doors past the border in that region's list (Hotan's
            // palace walkway, 2026-11-13: a walk down its stairs failed at the next door): there the walk carries on as
            // the neighbour's instance of the same object (the mesh's own coordinates stay; the target moves with the
            // region), its link list read.
            if (link == nullptr) {
                float wx = 0, wz = 0;
                inst->ToWorld(cx, cz, wx, wz);
                std::uint16_t region = at.region;
                if (WrapRegion(region, wx, wz) && region != at.region) {
                    if (const NavRegion* next = Find(region)) {
                        const std::int32_t j = InstanceByKey(*next, inst->key);
                        if (j >= 0 && next->instances[static_cast<std::size_t>(j)].Walkable()) {
                            float ox = 0, oz = 0;
                            RegionOffset(at.region, region, ox, oz);
                            tx -= ox;
                            tz -= oz;
                            at.region = region;
                            nav = next;
                            index = j;
                            inst = &nav->instances[static_cast<std::size_t>(j)];
                            at.instance = index;
                            link = findLink(*nav, index);
                        }
                    }
                }
            }
            if (link == nullptr || link->object >= nav->instances.size() || !nav->instances[link->object].Walkable() ||
                link->objectEdge >= nav->instances[link->object].mesh->outlineCount) {
                return stop(nav_walk::kFailed);
            }
            float wx = 0, wz = 0;
            inst->ToWorld(cx, cz, wx, wz);
            index = link->object;
            inst = &nav->instances[link->object];
            inst->ToLocal(wx, wz, px, pz);
            inst->ToLocal(tx, tz, qx, qz);
            OntoEdge(*inst->mesh, link->objectEdge, px, pz);
            tri = inst->mesh->edges[link->objectEdge].cellA;
            if (zones != nullptr) NavZoneOfCell(*inst->mesh, tri, *zones);
            cameFrom = link->objectEdge;
            at.instance = index;
            result |= nav_walk::kLink;
            continue;
        }
        if (e.flags & (nav_edge::kBlockAToB | nav_edge::kLedge)) return stop(nav_walk::kBlocked);

        // Off the object: just outside the edge, onto the terrain.
        const auto& a = mesh.vertices[e.a];
        const auto& b = mesh.vertices[e.b];
        float nx = b.z - a.z, nz = -(b.x - a.x);
        const float length = std::hypot(nx, nz);
        if (length > 0.0f) {
            nx /= length;
            nz /= length;
        }
        if (nx * (cx - t.cx) + nz * (cz - t.cz) < 0.0f) {
            nx = -nx;
            nz = -nz;
        }
        NavPosition out = at;
        inst->ToWorld(cx + nx * kLeaveOffset, cz + nz * kLeaveOffset, out.x, out.z);
        float ttx = tx, ttz = tz;
        if (!Settle(out, ttx, ttz)) return stop(nav_walk::kBlocked);
        const NavRegion* onto = Find(out.region);
        const assets::RegionNavData::Tile& tile = onto->TileAt(out.x, out.z);
        if ((tile.flags & 1) != 0) return stop(nav_walk::kBlocked);
        out.cell = tile.cell;
        out.instance = -1;
        out.y = NavTerrainHeight(*onto, out.x, out.z);
        left = out.region == at.region ? index : InstanceByKey(*onto, inst->key);
        at = out;
        tx = ttx;
        tz = ttz;
        return result | nav_walk::kLeftObject;
    }
}

bool NavWorld::Ray(std::uint16_t region, const float origin[3], const float direction[3], float maxLength, NavHit& hit) const {
    if (NavIsDungeon(region)) {
        const NavDungeon* dungeon = FindDungeon(region);
        return dungeon != nullptr && dungeon->Ray(origin, direction, maxLength, hit);
    }
    float best = maxLength;
    bool found = false;
    const int rx = NavRegionX(region), rz = NavRegionZ(region);
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (rx + dx < 0 || rx + dx > 255 || rz + dz < 0 || rz + dz > 255) continue;
            const std::uint16_t id = NavRegionId(rx + dx, rz + dz);
            const NavRegion* nav = Find(id);
            if (nav == nullptr) continue;
            const float o[3] = {origin[0] - static_cast<float>(dx) * kNavRegionSize, origin[1],
                                origin[2] - static_cast<float>(dz) * kNavRegionSize};
            const auto record = [&](float t, std::int32_t instance) {
                best = t;
                found = true;
                hit.region = id;
                hit.x = o[0] + direction[0] * t;
                hit.y = o[1] + direction[1] * t;
                hit.z = o[2] + direction[2] * t;
                hit.distance = t;
                hit.instance = instance;
            };
            float t = 0;
            if (RayTerrain(*nav, o, direction, best, t) && t < best) record(t, -1);
            for (std::size_t i = 0; i < nav->instances.size(); ++i) {
                const NavRegion::Instance& inst = nav->instances[i];
                if (!inst.Walkable() || inst.mesh->marker) continue;
                const float lo[3] = {inst.minX, inst.minY, inst.minZ}, hi[3] = {inst.maxX, inst.maxY, inst.maxZ};
                float enter = 0;
                if (!RayBox(o, direction, lo, hi, best, enter)) continue;
                // The ray in object space.
                float lox = 0, loz = 0;
                inst.ToLocal(o[0], o[2], lox, loz);
                const float lo3[3] = {lox, o[1] - inst.y, loz};
                const float ld[3] = {direction[0] * inst.cos + direction[2] * inst.sin, direction[1],
                                     -direction[0] * inst.sin + direction[2] * inst.cos};
                const NavObjectMesh& mesh = *inst.mesh;
                for (const NavObjectMesh::Triangle& tri : mesh.triangles) {
                    const auto& a = mesh.vertices[tri.v[0]];
                    const auto& b = mesh.vertices[tri.v[1]];
                    const auto& c = mesh.vertices[tri.v[2]];
                    const float pa[3] = {a.x, a.y, a.z}, pb[3] = {b.x, b.y, b.z}, pc[3] = {c.x, c.y, c.z};
                    float s = 0;
                    if (RayTriangle(lo3, ld, pa, pb, pc, s) && s < best) record(s, static_cast<std::int32_t>(i));
                }
            }
        }
    }
    if (found) {
        // In the region the hit lies in.
        std::uint16_t id = hit.region;
        float x = hit.x, z = hit.z;
        if (WrapRegion(id, x, z) && Find(id) != nullptr) {
            if (id != hit.region && hit.instance >= 0) {
                hit.instance = InstanceByKey(*Find(id), Find(hit.region)->instances[static_cast<std::size_t>(hit.instance)].key);
            }
            hit.region = id;
            hit.x = x;
            hit.z = z;
        }
    }
    return found;
}

} // namespace sr::game
