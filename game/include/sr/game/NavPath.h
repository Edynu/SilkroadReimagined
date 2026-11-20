#pragma once

// A way over the navigation between two places, for walkers that must get round what stands in the way: the bots
// and the tests' scripted players (ours; the original's players walk straight where they click and stop at the
// first wall, and so do ours).
//
// A* over a grid of points `step` apart around the start and the goal: a step from one point to its neighbour is
// open when NavWorld::Walk from the one reaches the other, so terrain cells, objects (bridges, walls, the levels of
// a building), region borders, a dungeon's blocks and the doors shut in the walker's layer all count as they do
// for the server, which checks every move with the same walk. The points found are then pulled tight: each leg of
// the result is one straight walk, as a click is.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sr/game/NavMesh.h"

namespace sr::core {
class JobSystem;
}

namespace sr::game {

// A circle the way must keep out of (region-local centre): a place a walker found it could not pass, which the
// navigation does not know (a door shut in an instance, as the server keeps it).
struct NavAvoid {
    std::uint16_t region = 0;
    float x = 0, z = 0, radius = 0;
};

struct NavPathOptions {
    float step = 25.0f;           // the grid's spacing
    float margin = 1500.0f;       // how far past the box of the start and the goal the search may go
    std::size_t maxNodes = 40000; // points taken from the open list at most (no way found within them: none)
    float legLength = 1200.0f;    // the longest leg of the result (a walk reaches at most a region's size)
    std::uint32_t layer = 0;      // the instance walked in (its shut doors), 0 the shared world
    std::vector<NavAvoid> avoid;  // no step or leg enters these
    // A point this near the goal will do (0: the goal itself): for a goal no walk reaches, an NPC behind its
    // counter, whom a player talks to from the counter's side.
    float arrive = 0.0f;
    // No way to the goal (walled in, a roof, too far): the way to the nearest place the search reached instead, if
    // nearer than the start (true then, NavPathStats::partial set; a player's click goes as near as it can).
    bool closest = false;
    // The legs pulled tight keep this far from what stands beside them (NavWalkClear; 0: they may graze a corner).
    float clearance = 0.0f;
    // A step out of a point beside a wall (one of its eight neighbours not reached) costs this many steps more for
    // each neighbour not reached (ours, the user's report 2026-11-18: the shortest way runs along walls and round
    // corners at their edge; this keeps it a point or two off them where there is room). 0: none (the bots').
    float wallCost = 0.0f;
};

// A player's way should not graze corners (ours, the user's report 2026-11-14: a leg that touches a wall's corner is
// stopped there by the smallest difference in where the walker stands). Whether a straight walk from `from` to
// (x, z) of `toRegion` gets through with `clearance` to spare: it does, and so do the walks beside it, `clearance` to
// either side (their starts on the ground there).
bool NavWalkClear(const NavWorld& nav, const NavPosition& from, std::uint16_t toRegion, float x, float z, float clearance, std::uint32_t layer = 0);

struct NavPathStats {
    std::size_t expanded = 0; // points taken from the open list
    std::size_t walks = 0;    // walks tried
    bool partial = false;     // the way leads to the nearest place reached, not the goal (NavPathOptions::closest)
};

// The legs from `from` to (x, z) of region `toRegion`: each element is reached from the one before (the first from
// `from`) by a straight walk, the last is the goal. Empty and true when `from` is already there. False when no way
// was found within the options' limits (the goal walled in, off the held regions, too far).
bool FindNavPath(const NavWorld& nav, const NavPosition& from, std::uint16_t toRegion, float x, float z,
                 std::vector<NavPosition>& path, const NavPathOptions& options = {}, NavPathStats* stats = nullptr);

// The best of several searches (ours; a player's Pathfinding, the Option window's Beta page): the same way searched on
// grids of different steps at once (the options' step, a finer one of 15 for the narrow gaps a coarse grid steps over,
// a coarser one of 40 with a wider box for the long ways a fine grid runs out of points on), on the job system when one
// is given, the shortest way found taken. Its stats add up the searches'.
bool FindBestNavPath(const NavWorld& nav, const NavPosition& from, std::uint16_t toRegion, float x, float z,
                     std::vector<NavPosition>& path, const NavPathOptions& options = {}, NavPathStats* stats = nullptr,
                     core::JobSystem* jobs = nullptr);

// A way's length on the flat: from `from` through each leg.
double NavPathLength(const NavPosition& from, const std::vector<NavPosition>& path);

} // namespace sr::game
