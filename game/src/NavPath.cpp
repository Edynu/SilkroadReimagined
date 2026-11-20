#include "sr/game/NavPath.h"

#include "sr/core/Jobs.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <queue>
#include <unordered_map>
#include <utility>

namespace sr::game {
namespace {

bool IsDungeon(std::uint16_t region) { return (region & 0x8000) != 0; }

struct Flat {
    double x = 0, z = 0;
};

double Distance(Flat a, Flat b) { return std::hypot(a.x - b.x, a.z - b.z); }

// World-wide x/z of a region-local point; in a dungeon (one region) its own coordinates.
Flat ToFlat(std::uint16_t region, float x, float z) {
    if (IsDungeon(region)) return {x, z};
    return {double(region & 0xFF) * kNavRegionSize + x, double(region >> 8) * kNavRegionSize + z};
}
Flat ToFlat(const NavPosition& p) { return ToFlat(p.region, p.x, p.z); }

// The region a world-wide point falls in and its local x/z (`dungeon`: that dungeon, the coordinates its own).
std::uint16_t FromFlat(Flat f, std::uint16_t dungeon, float& x, float& z) {
    if (dungeon != 0) {
        x = float(f.x);
        z = float(f.z);
        return dungeon;
    }
    const int rx = std::clamp(int(std::floor(f.x / kNavRegionSize)), 0, 255), rz = std::clamp(int(std::floor(f.z / kNavRegionSize)), 0, 255);
    x = float(f.x - rx * double(kNavRegionSize));
    z = float(f.z - rz * double(kNavRegionSize));
    return std::uint16_t((rz << 8) | rx);
}

constexpr std::uint32_t kStopped = nav_walk::kBlocked | nav_walk::kFailed | nav_walk::kLimit;
constexpr std::uint32_t kStepCrossings = 64;     // a grid step crosses a few cells at most
constexpr std::uint32_t kLegCrossings = 100000;  // a leg of the result: as a click's walk
constexpr double kArrived = 1.0;                 // a walk ending this near its target reached it
constexpr std::size_t kLookAhead = 64;           // points of the way a leg may skip

struct Node {
    NavPosition at;
    std::int32_t ix = 0, iz = 0;
    float g = 0;
    std::int32_t parent = -1;
    bool closed = false;
};

// A grid point and what it is walked on (the terrain, or which object: a bridge's deck and the ground under it are
// two places).
std::uint64_t Key(std::int32_t ix, std::int32_t iz, std::int32_t instance) {
    return (std::uint64_t(std::uint32_t(ix) & 0xFFFFFu) << 44) | (std::uint64_t(std::uint32_t(iz) & 0xFFFFFu) << 24) |
           (std::uint64_t(std::uint32_t(instance + 1)) & 0xFFFFFFu);
}

} // namespace

bool NavWalkClear(const NavWorld& nav, const NavPosition& from, std::uint16_t toRegion, float x, float z, float clearance, std::uint32_t layer) {
    const std::uint16_t dungeon = IsDungeon(from.region) ? from.region : 0;
    const Flat a = ToFlat(from), b = ToFlat(toRegion, x, z);
    const auto walks = [&](const NavPosition& start, Flat target) {
        float lx = 0, lz = 0;
        const std::uint16_t region = FromFlat(target, dungeon, lx, lz);
        NavPosition to;
        if ((nav.Walk(start, region, lx, lz, to, kLegCrossings, nullptr, layer) & kStopped) != 0) return false;
        return Distance(ToFlat(to), target) < kArrived;
    };
    if (!walks(from, b)) return false;
    const double length = Distance(a, b);
    if (clearance <= 0.0f || length < 1.0) return true;
    // The walks beside it: their starts `clearance` to either side, dropped onto the ground there (none: a wall).
    const double nx = -(b.z - a.z) / length * clearance, nz = (b.x - a.x) / length * clearance;
    for (const double side : {1.0, -1.0}) {
        const Flat s{a.x + side * nx, a.z + side * nz}, t{b.x + side * nx, b.z + side * nz};
        float lx = 0, lz = 0;
        const std::uint16_t region = FromFlat(s, dungeon, lx, lz);
        const NavPosition start = nav.Drop(region, lx, from.y, lz);
        if (!start.Valid() || std::abs(start.y - from.y) > 30.0f || !walks(start, t)) return false;
    }
    return true;
}

bool FindNavPath(const NavWorld& nav, const NavPosition& from, std::uint16_t toRegion, float x, float z,
                 std::vector<NavPosition>& path, const NavPathOptions& options, NavPathStats* stats) {
    path.clear();
    NavPathStats ownStats;
    NavPathStats& st = stats != nullptr ? *stats : ownStats;
    st = {};
    const std::uint16_t dungeon = IsDungeon(from.region) ? from.region : 0;
    if ((dungeon != 0 || IsDungeon(toRegion)) && toRegion != from.region) return false;
    const Flat start = ToFlat(from), goal = ToFlat(toRegion, x, z);
    if (Distance(start, goal) < kArrived) return true;

    // The circles kept out of (in a dungeon its own coordinates, as the walk's).
    struct Circle {
        Flat centre;
        double radius = 0;
    };
    std::vector<Circle> circles;
    for (const NavAvoid& a : options.avoid) circles.push_back({ToFlat(a.region, a.x, a.z), a.radius});
    const auto clear = [&](Flat a, Flat b) {
        for (const Circle& c : circles) {
            const double dx = b.x - a.x, dz = b.z - a.z, length2 = dx * dx + dz * dz;
            const double t = length2 > 0 ? std::clamp(((c.centre.x - a.x) * dx + (c.centre.z - a.z) * dz) / length2, 0.0, 1.0) : 0.0;
            if (Distance({a.x + t * dx, a.z + t * dz}, c.centre) < c.radius) return false;
        }
        return true;
    };

    // Whether a straight walk from `a` reaches `target` (and keeps out of the circles); `to` where it ends.
    const auto reaches = [&](const NavPosition& a, Flat target, NavPosition& to, std::uint32_t crossings) {
        if (!clear(ToFlat(a), target)) return false;
        float lx = 0, lz = 0;
        const std::uint16_t region = FromFlat(target, dungeon, lx, lz);
        ++st.walks;
        if ((nav.Walk(a, region, lx, lz, to, crossings, nullptr, options.layer) & kStopped) != 0) return false;
        return Distance(ToFlat(to), target) < kArrived;
    };

    NavPosition end;
    if (Distance(start, goal) <= options.legLength && reaches(from, goal, end, kLegCrossings)) {
        path.push_back(end);
        return true;
    }

    // The grid: its point (0, 0) is the start; the search stays in the box of the start and the goal, widened.
    const double step = std::max(1.0f, options.step);
    const auto lowIndex = [&](double a, double b, double origin) { return std::int32_t(std::floor((std::min(a, b) - options.margin - origin) / step)); };
    const auto highIndex = [&](double a, double b, double origin) { return std::int32_t(std::ceil((std::max(a, b) + options.margin - origin) / step)); };
    const std::int32_t minX = lowIndex(start.x, goal.x, start.x), maxX = highIndex(start.x, goal.x, start.x);
    const std::int32_t minZ = lowIndex(start.z, goal.z, start.z), maxZ = highIndex(start.z, goal.z, start.z);
    const auto pointOf = [&](std::int32_t ix, std::int32_t iz) { return Flat{start.x + ix * step, start.z + iz * step}; };
    const auto estimate = [&](Flat p) { return float(Distance(p, goal)); };

    std::vector<Node> nodes;
    std::unordered_map<std::uint64_t, std::int32_t> index;
    using Entry = std::pair<float, std::int32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    nodes.push_back({from, 0, 0, 0.0f, -1, false});
    index.emplace(Key(0, 0, from.instance), 0);
    open.push({estimate(start), 0});

    constexpr std::int32_t kSteps[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    const double tryGoal = step * 3.0;
    std::int32_t found = -1;
    std::int32_t nearest = 0; // the place reached nearest the goal (NavPathOptions::closest)
    double nearestDistance = Distance(start, goal);
    while (!open.empty() && st.expanded < options.maxNodes) {
        const std::int32_t i = open.top().second;
        open.pop();
        if (nodes[std::size_t(i)].closed) continue;
        nodes[std::size_t(i)].closed = true;
        ++st.expanded;
        const NavPosition at = nodes[std::size_t(i)].at;
        const std::int32_t ix = nodes[std::size_t(i)].ix, iz = nodes[std::size_t(i)].iz;
        const float g = nodes[std::size_t(i)].g;
        if (const double d = Distance(ToFlat(at), goal); i != 0 && d < nearestDistance) {
            nearest = i;
            nearestDistance = d;
        }
        if (Distance(pointOf(ix, iz), goal) <= tryGoal && reaches(at, goal, end, kStepCrossings * 4)) {
            found = i;
            break;
        }
        if (i != 0 && options.arrive > 0.0f && Distance(pointOf(ix, iz), goal) <= options.arrive) {
            found = i;
            end = at;
            break;
        }
        // The eight neighbours walked to first: those not reached (a wall, an edge) make every step out of this point
        // dearer (NavPathOptions::wallCost), so the way keeps off walls and corners where it has room.
        struct Neighbour {
            std::int32_t nx = 0, nz = 0;
            bool diagonal = false, reached = false;
            NavPosition to;
        };
        Neighbour around[8];
        int blocked = 0;
        for (std::size_t k = 0; k < 8; ++k) {
            Neighbour& n = around[k];
            n.nx = ix + kSteps[k][0];
            n.nz = iz + kSteps[k][1];
            n.diagonal = kSteps[k][0] != 0 && kSteps[k][1] != 0;
            if (n.nx < minX || n.nx > maxX || n.nz < minZ || n.nz > maxZ) continue;
            n.reached = reaches(at, pointOf(n.nx, n.nz), n.to, kStepCrossings);
            if (!n.reached) ++blocked;
        }
        const float wall = i == 0 ? 0.0f : float(step) * options.wallCost * float(blocked);
        for (const Neighbour& n : around) {
            if (!n.reached) continue;
            const std::int32_t nx = n.nx, nz = n.nz;
            const Flat q = pointOf(nx, nz);
            const NavPosition& to = n.to;
            const float cost = g + wall + float(n.diagonal ? step * std::numbers::sqrt2 : step);
            const std::uint64_t k = Key(nx, nz, to.instance);
            const auto it = index.find(k);
            if (it == index.end()) {
                const auto n = std::int32_t(nodes.size());
                nodes.push_back({to, nx, nz, cost, i, false});
                index.emplace(k, n);
                open.push({cost + estimate(q), n});
            } else if (Node& known = nodes[std::size_t(it->second)]; !known.closed && cost < known.g) {
                known.g = cost;
                known.parent = i;
                known.at = to;
                open.push({cost + estimate(q), it->second});
            }
        }
    }
    if (found < 0 && options.closest && nearest > 0 && nearestDistance + kArrived < Distance(start, goal)) {
        found = nearest;
        end = nodes[std::size_t(nearest)].at;
        st.partial = true;
    }
    if (found < 0) return false;

    // The way found, point by point, then pulled tight: from where the walker stands, the farthest point ahead that
    // one straight walk reaches.
    std::vector<NavPosition> way;
    for (std::int32_t i = found; i > 0; i = nodes[std::size_t(i)].parent) way.push_back(nodes[std::size_t(i)].at);
    std::reverse(way.begin(), way.end());
    if (way.empty() || Distance(ToFlat(way.back()), ToFlat(end)) >= kArrived) way.push_back(end);
    NavPosition at = from;
    for (std::size_t i = 0; i < way.size();) {
        std::size_t next = i;
        NavPosition reached = way[i];
        const Flat a = ToFlat(at);
        for (std::size_t j = std::min(way.size() - 1, i + kLookAhead); j > i; --j) {
            const Flat b = ToFlat(way[j]);
            NavPosition to;
            // On the floor the search found there too (a straight walk may end under a walkway, on the ground, where the
            // next point is not walked to: a leg the server stops short of; Hotan, 2026-11-13).
            if (Distance(a, b) <= options.legLength && reaches(at, b, to, kLegCrossings) && to.instance == way[j].instance &&
                (options.clearance <= 0.0f || NavWalkClear(nav, at, way[j].region, way[j].x, way[j].z, options.clearance, options.layer))) {
                next = j;
                reached = to;
                break;
            }
        }
        if (next == i) {
            // The next point itself (reached from the one before it when the way was found).
            NavPosition to;
            if (reaches(at, ToFlat(way[i]), to, kLegCrossings)) reached = to;
        }
        path.push_back(reached);
        at = reached;
        i = next + 1;
    }
    return true;
}

double NavPathLength(const NavPosition& from, const std::vector<NavPosition>& path) {
    double length = 0;
    Flat at = ToFlat(from);
    for (const NavPosition& p : path) {
        const Flat b = ToFlat(p);
        length += Distance(at, b);
        at = b;
    }
    return length;
}

bool FindBestNavPath(const NavWorld& nav, const NavPosition& from, std::uint16_t toRegion, float x, float z,
                     std::vector<NavPosition>& path, const NavPathOptions& options, NavPathStats* stats, core::JobSystem* jobs) {
    // The variants: the options' own, a finer grid (its box narrower, more points), a coarser one (its box wider).
    constexpr std::size_t kVariants = 3;
    NavPathOptions variants[kVariants] = {options, options, options};
    variants[1].step = std::min(options.step, 15.0f);
    variants[1].margin = options.margin * 0.6f;
    variants[1].maxNodes = options.maxNodes + options.maxNodes / 2;
    variants[2].step = std::max(options.step, 40.0f);
    variants[2].margin = options.margin * 1.6f;
    std::vector<NavPosition> found[kVariants];
    NavPathStats searched[kVariants];
    bool ok[kVariants] = {};
    const auto search = [&](std::size_t begin, std::size_t end) {
        for (std::size_t v = begin; v < end; ++v) ok[v] = FindNavPath(nav, from, toRegion, x, z, found[v], variants[v], &searched[v]);
    };
    if (jobs != nullptr) jobs->ParallelFor(kVariants, 1, search);
    else search(0, kVariants);
    // The shortest way to the goal; none: the way to the nearest place reached, the one ending nearest the goal.
    const Flat goal = ToFlat(toRegion, x, z);
    const auto score = [&](std::size_t v) {
        if (!searched[v].partial) return NavPathLength(from, found[v]);
        const Flat e = found[v].empty() ? ToFlat(from) : ToFlat(found[v].back());
        return 1e9 + Distance(e, goal);
    };
    std::size_t best = kVariants;
    NavPathStats total;
    for (std::size_t v = 0; v < kVariants; ++v) {
        total.expanded += searched[v].expanded;
        total.walks += searched[v].walks;
        if (ok[v] && (best == kVariants || score(v) < score(best) - 1e-3)) best = v;
    }
    total.partial = best != kVariants && searched[best].partial;
    if (stats != nullptr) *stats = total;
    path.clear();
    if (best == kVariants) return false;
    path = std::move(found[best]);
    return true;
}

} // namespace sr::game
