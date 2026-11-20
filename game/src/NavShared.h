#pragma once

// Pieces of the navigation queries shared by the outdoor world and dungeons (NavMesh.cpp,
// NavDungeon.cpp). Internal to sr::game.

#include <string>
#include <unordered_set>
#include <cstdint>
#include <limits>

#include "sr/game/NavMesh.h"

namespace sr::game::detail {

// The client's distances: a stop or a cell change keeps the walker this far inside
// the cell; a walker bumping into an object's wall is pushed out this far; leaving an object puts
// it this far outside the edge; a leg starting this near the target ends the walk; a walk has at
// most this many legs.
constexpr float kPullBack = 0.2f;
constexpr float kWallPush = 0.01f;
constexpr float kLeaveOffset = 0.2f;
constexpr float kNearTarget = 5.0f;
constexpr int kMaxLegs = 6;

constexpr float kSegmentEpsilon = 1e-5f; // along a segment, as a fraction
constexpr float kEdgeEpsilon = 1e-4f;    // along an edge, as a fraction
constexpr float kSideEpsilon = 1e-3f;    // on a cell side, in units
constexpr float kInfinity = std::numeric_limits<float>::infinity();

// A walk across one object navmesh in its own space, triangle by triangle through the edges the
// segment p -> q crosses, from triangle `tri` (entered through `cameFrom`, -1 none). It ends on the
// triangle holding the target, at an internal edge that blocks the way (or the crossing limit), or
// at the first outline edge it crosses: the caller decides what that edge does.
struct MeshStep {
    enum class Kind : std::uint8_t { Inside, Blocked, Limit, Outline };
    Kind kind = Kind::Inside;
    std::uint32_t tri = 0;
    float x = 0, z = 0;    // Blocked, Limit, Outline: the crossing (object space)
    std::uint32_t edge = 0; // the edge crossed
};

// `zones`: the event zones entered on the way (NavZonesEntered).
// Whether an event (a cell's or an edge's) names a zone standing in the way ("zone_r90": the zone and its turn).
bool CellBlocked(const NavObjectMesh& mesh, std::uint8_t event, const std::unordered_set<std::string>& blocked);
MeshStep WalkTriangles(const NavObjectMesh& mesh, std::uint32_t tri, float px, float pz, float qx, float qz, std::int64_t cameFrom,
                       std::uint32_t& crossings, const std::unordered_set<std::string>& blockedZones = {},
                       std::vector<std::string>* zones = nullptr);
// The triangle under (x, z) whose height is nearest to y (object space); -1 if none.
std::int32_t TriangleUnder(const NavObjectMesh& mesh, float x, float z, float y, float& height);
// Moves (x, z) towards (cx, cz) by `distance` (to it when nearer).
void PullTowards(float& x, float& z, float cx, float cz, float distance);
// Moves (x, z) onto the edge (the nearest point of it): a walker handed through a door starts on
// the far side's edge exactly, whatever the gap between the two edges.
void OntoEdge(const NavObjectMesh& mesh, std::uint32_t edge, float& x, float& z);
bool RayTriangle(const float o[3], const float d[3], const float a[3], const float b[3], const float c[3], float& t);
// The ray's entry into a box; false when it misses it within [0, maxT].
bool RayBox(const float o[3], const float d[3], const float lo[3], const float hi[3], float maxT, float& enter);

} // namespace sr::game::detail
