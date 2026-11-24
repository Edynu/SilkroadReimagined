#include "sr/server/World.h"

#include <algorithm>
#include <deque>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "sr/assets/FileSystem.h"
#include "sr/assets/Terrain.h"
#include "sr/core/Jobs.h"
#include "sr/core/Log.h"
#include "sr/core/Trace.h"
#include "sr/game/NavLoader.h"
#include "sr/game/NavMesh.h"

namespace sr::server {

namespace proto = game::proto;

namespace {

// World-wide coordinates (x east, z north): the protocol's.
using Flat = proto::FlatPosition;
using proto::FromFlat;
using proto::ToFlat;

// How far one walk goes toward a click (NavWorld::Walk takes a target at most a region away).
constexpr double kWalkReach = game::kNavRegionSize - 40.0;

int RegionX(std::uint16_t region) { return region & 0xFF; }
int RegionZ(std::uint16_t region) { return region >> 8; }

template <typename T>
WorldMessage Make(std::uint64_t peer, const T& message, std::uint64_t latestKey = 0) {
    return {peer, T::kId, proto::Encode(message), latestKey};
}

} // namespace

struct World::State {
    struct Entity;
    // Its summary as shown: its own, or a transform's look over it.
    static proto::CharacterSummary Shown(const Entity& e);
    struct Entity {
        std::uint32_t id = 0;
        std::uint64_t peer = 0;
        std::int64_t character = 0;
        proto::CharacterSummary summary;
        proto::WorldPosition position;
        float yaw = 0;
        bool placed = false, dropOnGround = false;
        // The leg it walks.
        bool moving = false;
        // A walk toward a heading (MoveToward: a click on the sky): its way (a unit vector on the ground), kept
        // while each leg walks its full length; the leg that stopped short (a wall) is the last.
        bool heading = false, headingLast = false;
        double headingX = 0, headingZ = 0;
        proto::WorldPosition legFrom, legTo;
        double length = 0, travelled = 0;
        float speed = 0;
        float speedFactor = 1.0f; // buffs
        bool legWalk = false;     // the leg under way is a walk (MoveTo's `walk`), not a run
        bool walkMode = false;    // SetWalking
        float walkSpeed = 0, runSpeed = 0; // its own (monsters); 0: the configured player speeds
        bool watcher = false;      // a player: sees the others, holds the navigation around it
        bool npc = false;          // stands, watches nobody (AddNpc, AddGroundItem)
        proto::EntityKind kind = proto::EntityKind::Character;
        std::uint32_t amount = 0;  // a ground item's count or gold
        std::uint8_t plus = 0;
        std::uint8_t rarity = 0;   // a monster's kind
        bool fresh = false;        // a ground item just put down: told as fallen on the next pass only
        std::uint32_t maxHp = 0, maxMp = 0; // a player's (WorldEntered)
        SharedBytes appearBytes; // an NPC's EntityAppeared, encoded once (shared by everyone told)
        std::vector<proto::StateRecord> states; // its states (SetStates), their time left at statesAt
        std::vector<std::uint32_t> buffs;        // the skills of its buffs (SetBuffs)
        std::uint8_t hideMask = 0, hideLevel = 0;     // SetHidden
        std::uint8_t detectMask = 0, detectLevel = 0; // SetDetection
        std::uint32_t party = 0;                      // SetParty
        std::uint32_t mount = 0;                      // Carry: what carries it (its legs are copied to it)
        std::optional<proto::CharacterSummary> shown; // SetShownLook: a transform's look
        // What newcomers hear after its appearance (SetExtra): one message of each id.
        std::vector<std::pair<proto::MessageId, SharedBytes>> extras;
        WorldClock::time_point statesAt;
        game::NavPosition nav;      // where it stands on the navigation (its cell and object)
        game::NavPosition legStart; // the leg's start and end on the navigation (the walk MoveTo checked)
        game::NavPosition legEnd;
        bool legChanged = false; // its observers hear of it after this tick
        int zone = 0;
        // What a player sees, sorted by id: the entity, its tier (0 the nearest), and a new leg
        // held for that tier's next tick.
        struct Seen {
            std::uint32_t id = 0;
            std::uint8_t tier = 0;
            bool pending = false;
            std::uint32_t dense = 0; // the entity's dense number (its "seen before" mark)
        };
        std::vector<Seen> seen;
        std::vector<std::uint32_t> observers; // the players whose view holds it (their entity ids)
        // A player's view's bookkeeping (Interest): worked out again at once (new, back from a teleport, its limits,
        // party or detection changed); a leg in it held for a farther tier's tick; when it was last worked out, in
        // which cell, and the sum of the versions of the cells around it then.
        bool viewDirty = true, viewPending = false;
        std::uint64_t viewTick = 0, viewCell = 0, viewSignature = 0;
        // The grid cell it stood in on the last pass (its key), while it is in the grid.
        std::uint64_t gridCell = 0;
        bool inGrid = false;
        // A player's own limits (ViewLimits; at most the shares): a kind it lowered takes no lent places.
        std::size_t viewPlayers = SIZE_MAX, viewMonsters = SIZE_MAX;
        double viewRadius = 0.0; // how far it sees, when below the server's (0: the server's)
        std::size_t slot = 0;            // its index among the placed, on an interest pass
        std::uint32_t dense = 0;         // a small number of its own while it exists (NewId): the views' marks
        std::vector<std::uint16_t> navRegions;  // the navigation it holds (3 x 3 around it; a dungeon: itself)
        std::uint16_t navCentre = 0xFFFF;       // the region they are around
        // A player's event zones (TakeZonesEntered): those its leg entered so far, and the new ones
        // for the owner (written by its zone's step).
        std::vector<std::string> legZones, zonesPending;
    };

    WorldConfig config;
    core::JobSystem& jobs;
    const assets::FileSystem* files = nullptr;
    std::unique_ptr<game::NavLoader> loader;
    game::NavWorld nav;
    std::unordered_map<std::uint16_t, int> navUsers; // region -> entities holding it

    std::unordered_map<std::uint32_t, Entity> entities;
    // Each grid cell's version: raised whenever an entity comes into it or leaves it (it walks across, appears,
    // leaves the world, hides, changes party): a view whose cells' versions did not change has nobody new to see.
    std::unordered_map<std::uint64_t, std::uint64_t> cellVersion;
    // Something about `e` changed who may see it: the views around it are worked out again on their turn.
    void Touch(Entity& e) {
        if (e.inGrid) ++cellVersion[e.gridCell];
    }
    // Entity ids: this server's range [idFirst, idEnd), the next to try; past the end the range starts again from its
    // bottom, skipping those still in use (one is given again only after the whole range came round).
    std::uint32_t idFirst = 1, idEnd = 0, nextId = 1;
    // The dense numbers the views' "seen before" marks are indexed by (bounded by the entities alive, not by every id
    // ever given): one freed is given again only after kDenseQuarantine others were freed, long after the views it
    // was in dropped it (a view forgets a gone entity on its next pass).
    static constexpr std::size_t kDenseQuarantine = 65536;
    std::uint32_t nextDense = 0;
    std::deque<std::uint32_t> freedDense;
    std::pair<std::uint32_t, std::uint32_t> NewId() {
        std::uint32_t id = 0;
        std::uint64_t tries = 0;
        do {
            // Every id of the range in use at once (2^28 entities: never on a working server).
            if (++tries > std::uint64_t(idEnd - idFirst) + 1) {
                core::LogError("world: every entity id of {}..{} is in use", idFirst, idEnd);
                std::abort();
            }
            id = nextId;
            nextId = nextId + 1 >= idEnd || nextId + 1 < idFirst ? idFirst : nextId + 1;
        } while (id == 0 || entities.contains(id));
        std::uint32_t dense = 0;
        if (freedDense.size() > kDenseQuarantine) {
            dense = freedDense.front();
            freedDense.pop_front();
        } else {
            dense = nextDense++;
        }
        return {id, dense};
    }
    std::unordered_map<int, std::vector<std::uint32_t>> zones;

    bool started = false;
    WorldClock::time_point nextTick;
    WorldClock::time_point now; // the update's (newcomers' states count their time left from it)
    std::uint64_t ticks = 0;
    std::size_t activeZones = 0;
    double lastStepMs = 0;
    WorldStats stats;

    State(const WorldConfig& c, core::JobSystem& j, const assets::FileSystem* f) : config(c), jobs(j), files(f) {
        if (files != nullptr) loader = std::make_unique<game::NavLoader>(*files, jobs);
        constexpr std::uint32_t kRange = 1u << 28;
        const std::uint32_t lane = config.server % 16u;
        idFirst = std::max<std::uint32_t>(1, lane * kRange);
        idEnd = lane == 15 ? 0xFFFFFFFFu : (lane + 1) * kRange;
        if (config.idRange > 0 && config.idRange < kRange) idEnd = lane * kRange + config.idRange;
        nextId = idFirst;
    }

    double Step() const { return 1.0 / std::max(1.0, config.tickRate); }

    int ZoneOf(std::uint16_t region) const {
        if (game::NavIsDungeon(region)) return 0x40000000 | region; // a dungeon is a zone of its own
        const int n = std::max(1, config.zoneRegions);
        return (RegionX(region) / n) | ((RegionZ(region) / n) << 16);
    }

    // The navigation around a walker: the 3 x 3 regions about its own, held while it is there.
    void HoldNavigation(Entity& e) {
        if (!loader) return;
        std::vector<std::uint16_t> want;
        if (game::NavIsDungeon(e.position.region)) {
            want.push_back(e.position.region); // a dungeon has no neighbours
        } else {
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int x = RegionX(e.position.region) + dx, z = RegionZ(e.position.region) + dz;
                    if (x >= 0 && x <= 255 && z >= 0 && z <= 255) want.push_back(assets::RegionId(x, z));
                }
            }
        }
        for (std::uint16_t r : want) {
            if (std::find(e.navRegions.begin(), e.navRegions.end(), r) == e.navRegions.end() && navUsers[r]++ == 0) loader->Request(r);
        }
        for (std::uint16_t r : e.navRegions) {
            if (std::find(want.begin(), want.end(), r) == want.end() && --navUsers[r] == 0) {
                navUsers.erase(r);
                loader->Release(r, nav);
            }
        }
        e.navRegions = std::move(want);
        e.navCentre = e.position.region;
    }
    void DropNavigation(Entity& e) {
        if (!loader) return;
        for (std::uint16_t r : e.navRegions) {
            if (--navUsers[r] == 0) {
                navUsers.erase(r);
                loader->Release(r, nav);
            }
        }
        e.navRegions.clear();
    }
    // Its region's navigation is in (or there is none to wait for).
    bool NavigationReady(std::uint16_t region) const { return !loader || !loader->Loading(region); }

    // The height of the ground at (x, z) near y, from the navigation; y itself when there is none.
    float Ground(const proto::WorldPosition& p) const {
        if (!loader || !nav.Holds(p.region)) return p.y;
        const game::NavPosition at = nav.Drop(p.region, p.x, p.y, p.z);
        return at.Valid() ? at.y : p.y;
    }

    // Putting a player down (an entry, a teleport's arrival): the surface nearest the height it
    // is given, as the original client drops a position it is given (the
    // teleport table's arrival heights pick Hotan's plaza 12 above the ground, Jangan's paving 0.6
    // above it). Where that is the ground, an object's floor up to kBuriedFloor under it counts as
    // the floor (ours: the decks that run just under the ground they meet, as at Jangan's palace
    // stairs; a player put down on the ground there would never step onto them, section 5).
    static constexpr float kBuriedFloor = 1.0f;
    float PutDown(const proto::WorldPosition& p) const {
        if (!loader || !nav.Holds(p.region)) return p.y;
        const game::NavPosition at = nav.Drop(p.region, p.x, p.y, p.z);
        if (!at.Valid()) {
            float y = p.y;
            return nav.TerrainHeight(p.region, p.x, p.z, y) ? y : p.y;
        }
        if (at.instance < 0) {
            const game::NavPosition buried = nav.Drop(p.region, p.x, at.y - kBuriedFloor, p.z);
            if (buried.Valid() && buried.instance >= 0 && buried.y >= at.y - kBuriedFloor) return buried.y;
        }
        return at.y;
    }

    // Where a walker stands on the navigation: its kept position, or found from its place.
    game::NavPosition Standing(const Entity& e) const {
        if (e.nav.Valid() && nav.Holds(e.nav.region)) return e.nav;
        return nav.Drop(e.position.region, e.position.x, e.position.y, e.position.z);
    }

    // The leg walked from its start to `to`'s x and z (a point on it): the floor reached (over a
    // bridge, the bridge, not the ground under it), the same as the whole leg's walk passes there; `to.y` when the
    // navigation is not known there.
    float Stepped(Entity& e, const proto::WorldPosition& to) {
        if (!loader || !e.legStart.Valid() || !nav.Holds(e.legStart.region) || !nav.Holds(to.region)) {
            e.nav = {};
            return to.y;
        }
        game::NavPosition stop;
        std::vector<std::string> zones;
        nav.Walk(e.legStart, to.region, to.x, to.z, stop, 100000, e.watcher ? &zones : nullptr, e.position.layer);
        NoteZones(e, zones);
        if (stop.Valid() && stop.region == to.region && std::hypot(stop.x - to.x, stop.z - to.z) < 0.5f) {
            e.nav = stop;
        } else {
            e.nav = nav.Drop(to.region, to.x, stop.Valid() ? stop.y : e.position.y, to.z);
        }
        return e.nav.Valid() ? e.nav.y : Ground(to);
    }

    // A rider's mount (Carry) put where the rider is, on the same leg: the two walk it together.
    void CopyLeg(const Entity& rider) {
        if (rider.mount == 0) return;
        const auto it = entities.find(rider.mount);
        if (it == entities.end()) return;
        Entity& m = it->second;
        m.position = rider.position;
        m.nav = rider.nav;
        m.yaw = rider.yaw;
        m.moving = rider.moving;
        m.legFrom = rider.legFrom;
        m.legTo = rider.legTo;
        m.legStart = rider.legStart;
        m.legEnd = rider.legEnd;
        m.length = rider.length;
        m.travelled = rider.travelled;
        m.speed = rider.speed;
        m.legChanged = true;
    }

    // The zones a player's leg entered that it had not entered before on this leg.
    static void NoteZones(Entity& e, const std::vector<std::string>& zones) {
        for (const std::string& z : zones) {
            if (std::find(e.legZones.begin(), e.legZones.end(), z) != e.legZones.end()) continue;
            e.legZones.push_back(z);
            e.zonesPending.push_back(z);
        }
    }
    std::vector<ZoneEntered> zonesEntered; // for TakeZonesEntered

    // A tick: every zone walks its walkers (in parallel), then walkers that left their zone move.
    void StepZones(double dt) {
        std::vector<std::vector<std::uint32_t>*> active;
        for (auto& [key, members] : zones) {
            if (!members.empty()) active.push_back(&members);
        }
        activeZones = active.size();
        std::vector<double> zoneMs(active.size(), 0.0);
        jobs.ParallelFor(active.size(), 1, [&](std::size_t begin, std::size_t end) {
            SR_ZONE("world: zone");
            for (std::size_t z = begin; z < end; ++z) {
                const auto start = WorldClock::now();
                for (std::uint32_t id : *active[z]) Walk(entities.at(id), dt);
                zoneMs[z] = std::chrono::duration<double, std::milli>(WorldClock::now() - start).count();
            }
        });
        for (const double ms : zoneMs) stats.zoneMsMax = std::max(stats.zoneMsMax, ms);
        stats.zonesMax = std::max(stats.zonesMax, active.size());
        for (auto& [id, e] : entities) {
            const int zone = ZoneOf(e.position.region);
            if (zone == e.zone) continue;
            auto& from = zones[e.zone];
            from.erase(std::remove(from.begin(), from.end(), id), from.end());
            zones[zone].push_back(id);
            e.zone = zone;
            ++stats.zoneChanges;
        }
    }

    void Walk(Entity& e, double dt) {
        if (!e.moving) return;
        e.travelled += double(e.speed) * dt;
        if (e.travelled >= e.length) {
            if (e.watcher && loader && e.legStart.Valid() && nav.Holds(e.legStart.region)) {
                // The rest of the leg's zones (those between the last tick's point and its end).
                game::NavPosition stop;
                std::vector<std::string> zones;
                nav.Walk(e.legStart, e.legTo.region, e.legTo.x, e.legTo.z, stop, 100000, &zones, e.position.layer);
                NoteZones(e, zones);
            }
            e.position = e.legTo; // where the leg's walk ended, on its floor
            if (e.legEnd.Valid()) e.nav = e.legEnd;
            e.moving = false;
            return;
        }
        const Flat a = ToFlat(e.legFrom), b = ToFlat(e.legTo);
        const double t = e.travelled / e.length;
        const float y = e.legFrom.y + float(t) * (e.legTo.y - e.legFrom.y);
        proto::WorldPosition p = FromFlat({a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t}, y);
        p.y = Stepped(e, p);
        e.position = p;
    }

    static bool Sees(const std::vector<Entity::Seen>& seen, std::uint32_t id, const Entity::Seen** found = nullptr) {
        const auto it = std::lower_bound(seen.begin(), seen.end(), id, [](const Entity::Seen& s, std::uint32_t v) { return s.id < v; });
        if (it == seen.end() || it->id != id) return false;
        if (found != nullptr) *found = &*it;
        return true;
    }

    // Who sees whom, and what each player is told of it (the rules in World.h). A view's members (who is in it) are
    // worked out again only when they may have changed:
    // - at once when the view is new or was dropped (an entry, a teleport) or what decides it changed (the player's
    //   limits, party or detection: viewDirty);
    // - else on the player's turn, every kViewTurn ticks (the players spread over the ticks by their dense number),
    //   when it walks, stands in another cell than last time, the cells around it changed (their versions: raised
    //   whenever an entity comes into or leaves one), or kViewRefresh ticks went by (what walked across the radius
    //   within the same cells).
    // A view worked out again: on the jobs, in parallel (each reads every walker's place and writes only its own list
    // and messages), then on this thread each entity's observers follow the views' changes. Every other view stands
    // and is only told of what it holds: the new legs of the walkers in it (found from their observers lists) and the
    // legs held for a farther tier's tick. Each walker's leg is encoded once and the bytes shared by everyone told.
    static constexpr std::uint64_t kViewTurn = 4, kViewRefresh = 20;

    void Interest(std::vector<WorldMessage>& out) {
        const double cell = std::max(64.0, double(config.gridCell));
        const auto cellOf = [cell](double v) { return std::int64_t(std::floor(v / cell)); };
        const auto key = [](std::int64_t cx, std::int64_t cz) {
            return (std::uint64_t(std::uint32_t(cx)) << 32) | std::uint64_t(std::uint32_t(cz));
        };
        // The placed with their places; the cells' versions follow what came into a cell or left it.
        std::vector<Entity*> placed;
        std::vector<double> fx, fz;
        std::vector<std::uint32_t> watchers, changed; // their slots: the players; those with a new leg
        std::unordered_set<std::uint64_t> freshCells;   // the cells of ground items just put down (seen falling now)
        placed.reserve(entities.size());
        fx.reserve(entities.size());
        fz.reserve(entities.size());
        for (auto& [id, e] : entities) {
            if (!e.placed) {
                if (e.inGrid) {
                    ++cellVersion[e.gridCell];
                    e.inGrid = false;
                }
                continue;
            }
            const Flat f = ToFlat(e.position);
            const std::uint64_t k = key(cellOf(f.x), cellOf(f.z));
            if (!e.inGrid || k != e.gridCell) {
                if (e.inGrid) ++cellVersion[e.gridCell];
                ++cellVersion[k];
                e.gridCell = k;
                e.inGrid = true;
            }
            e.slot = placed.size();
            if (e.watcher) watchers.push_back(std::uint32_t(e.slot));
            if (e.legChanged) changed.push_back(std::uint32_t(e.slot));
            if (e.fresh) freshCells.insert(k);
            placed.push_back(&e);
            fx.push_back(f.x);
            fz.push_back(f.z);
        }
        // Nobody to tell (a server without players): no views to work out, only the pass's end.
        if (watchers.empty()) {
            EndInterestPass();
            return;
        }
        enum Class : std::uint8_t { kPlayer, kMonster, kItem, kNpc };
        std::vector<std::uint8_t> classOf(placed.size());
        for (std::size_t i = 0; i < placed.size(); ++i) {
            const Entity& e = *placed[i];
            classOf[i] = e.kind == proto::EntityKind::GroundItem ? kItem : e.npc ? kNpc : e.kind == proto::EntityKind::Monster ? kMonster : kPlayer;
        }
        // The grid: the placed by cell; the cells the players stand in, each with the sum of the versions of the cells
        // within reach of its whole square (a crowd stands in a few cells: worked out once for all of them).
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> grid;
        grid.reserve(placed.size());
        for (std::size_t i = 0; i < placed.size(); ++i) grid[placed[i]->gridCell].push_back(std::uint32_t(i));
        const double reachOut = std::max(1.0, double(config.viewRadius)) * (1.0 + std::clamp(double(config.viewKeep), 0.0, 0.9));
        std::unordered_map<std::uint64_t, std::uint32_t> watcherCell; // a cell key -> its index among the players' cells
        std::vector<std::pair<std::int64_t, std::int64_t>> watcherCells;
        std::vector<std::uint32_t> cellOfWatcher(placed.size(), 0);
        for (const std::uint32_t i : watchers) {
            const std::int64_t cx = cellOf(fx[i]), cz = cellOf(fz[i]);
            const auto [it, added] = watcherCell.try_emplace(key(cx, cz), std::uint32_t(watcherCells.size()));
            if (added) watcherCells.push_back({cx, cz});
            cellOfWatcher[i] = it->second;
        }
        const auto forReach = [&](std::size_t w, auto&& visit) {
            const auto [cx, cz] = watcherCells[w];
            const double x0 = double(cx) * cell, z0 = double(cz) * cell;
            for (std::int64_t gx = cellOf(x0 - reachOut), gx1 = cellOf(x0 + cell + reachOut); gx <= gx1; ++gx) {
                for (std::int64_t gz = cellOf(z0 - reachOut), gz1 = cellOf(z0 + cell + reachOut); gz <= gz1; ++gz) visit(key(gx, gz));
            }
        };
        // A ground item just put down is told as falling on this pass only: the views around it are worked out now.
        std::vector<std::uint64_t> signature(watcherCells.size(), 0);
        std::vector<char> urgent(watcherCells.size(), 0);
        jobs.ParallelFor(watcherCells.size(), 16, [&](std::size_t begin, std::size_t end) {
            for (std::size_t w = begin; w < end; ++w) {
                forReach(w, [&](std::uint64_t k) {
                    if (const auto it = cellVersion.find(k); it != cellVersion.end()) signature[w] += it->second;
                    if (!freshCells.empty() && freshCells.contains(k)) urgent[w] = 1;
                });
            }
        });
        // Which views are worked out again on this tick (`full`); the others stand (`kept`).
        const std::uint64_t tick = ticks;
        std::vector<std::uint32_t> full, kept;
        std::vector<char> cellNeeded(watcherCells.size(), 0);
        for (const std::uint32_t i : watchers) {
            Entity& e = *placed[i];
            const std::uint32_t c = cellOfWatcher[i];
            bool again = e.viewDirty || urgent[c];
            if (!again && (tick + e.dense) % kViewTurn == 0) {
                again = e.moving || e.viewCell != e.gridCell || e.viewSignature != signature[c] || tick - e.viewTick >= kViewRefresh;
            }
            if (again) {
                full.push_back(i);
                cellNeeded[c] = 1;
                e.viewDirty = false;
                e.viewTick = tick;
                e.viewCell = e.gridCell;
                e.viewSignature = signature[c];
            } else {
                kept.push_back(i);
            }
        }
        // The candidates of the cells a view worked out again stands in.
        std::vector<std::vector<std::uint32_t>> candidates(watcherCells.size());
        jobs.ParallelFor(watcherCells.size(), 1, [&](std::size_t begin, std::size_t end) {
            for (std::size_t w = begin; w < end; ++w) {
                if (!cellNeeded[w]) continue;
                forReach(w, [&](std::uint64_t k) {
                    if (const auto it = grid.find(k); it != grid.end()) candidates[w].insert(candidates[w].end(), it->second.begin(), it->second.end());
                });
            }
        });
        // New legs encoded once each; an appearance only when someone newly sees it (encoded by the
        // job that tells it: most walkers are seen by nobody new on a tick).
        std::vector<SharedBytes> legs(placed.size());
        jobs.ParallelFor(changed.size(), 64, [&](std::size_t begin, std::size_t end) {
            for (std::size_t k = begin; k < end; ++k) legs[changed[k]] = proto::Encode(Leg(*placed[changed[k]]));
        });
        const double radius = std::max(1.0, double(config.viewRadius));
        const double keep = std::clamp(double(config.viewKeep), 0.0, 0.9);
        const double nearer = (1.0 - keep) * (1.0 - keep); // a seen one's rank: its squared distance times this
        const auto due = [tick](std::uint8_t tier) { return tier == 0 || tick % (tier == 1 ? 2u : 4u) == 0; };
        struct Change {
            std::uint32_t watcher = 0, other = 0;
            bool added = false;
        };
        struct Tally {
            std::uint64_t watchers = 0, seen = 0, players = 0, monsters = 0, items = 0, legsSent = 0, legsDeferred = 0;
            std::size_t seenMax = 0;
        };
        // The views worked out again, in chunks of players.
        const std::size_t chunks = std::clamp<std::size_t>(full.size(), 1, 256);
        std::vector<std::vector<WorldMessage>> told(chunks);
        std::vector<std::vector<Change>> changes(chunks);
        std::vector<Tally> tallies(chunks);
        jobs.ParallelFor(full.empty() ? 0 : chunks, 1, [&](std::size_t chunkBegin, std::size_t chunkEnd) {
            SR_ZONE("world: interest");
            using Candidate = std::pair<double, const Entity*>; // its rank (squared distance, nearer when seen), the entity
            const auto byRank = [](const Candidate& a, const Candidate& b) { return a.first < b.first; };
            std::vector<Candidate> players, monsters, items, npcs, rest, chosen;
            std::vector<std::pair<Entity::Seen, const Entity*>> now;
            std::vector<Entity::Seen> next;
            // What a view held, marked by entity id with the view's epoch: "seen before" in O(1).
            static thread_local std::vector<std::uint32_t> marks;
            static thread_local std::uint32_t epoch = 0;
            if (marks.size() <= nextDense) marks.resize(std::size_t(nextDense) + 1024, 0u);
            for (std::size_t c = chunkBegin; c < chunkEnd; ++c) {
                std::vector<WorldMessage>& mine = told[c];
                Tally& tally = tallies[c];
                for (std::size_t k = full.size() * c / chunks, last = full.size() * (c + 1) / chunks; k < last; ++k) {
                    const std::size_t i = full[k];
                    Entity& e = *placed[i];
                    players.clear();
                    monsters.clear();
                    items.clear();
                    npcs.clear();
                    if (++epoch == 0) { // wrapped: clear the old marks
                        std::fill(marks.begin(), marks.end(), 0u);
                        epoch = 1;
                    }
                    for (const Entity::Seen& seen : e.seen) {
                        if (seen.dense < marks.size()) marks[seen.dense] = epoch;
                    }
                    const double mx = fx[i], mz = fz[i];
                    // Its own radius when it lowered it (the Online page): what is beyond is not told.
                    const double ownRadius = e.viewRadius > 0.0 ? std::min(e.viewRadius, radius) : radius;
                    const double ownNear2 = ownRadius * ownRadius, ownFar2 = ownNear2 * (1.0 + keep) * (1.0 + keep);
                    for (const std::uint32_t o : candidates[cellOfWatcher[i]]) {
                        if (o == i) continue;
                        const double dx = fx[o] - mx, dz = fz[o] - mz, d2 = dx * dx + dz * dz;
                        if (d2 > ownFar2) continue;
                        const Entity* other = placed[o];
                        // Hidden from it unless it detects the hide (its mask, its level).
                        if (other->hideMask != 0 && (other->party == 0 || other->party != e.party) &&
                            ((e.detectMask & other->hideMask) == 0 || other->hideLevel > e.detectLevel)) {
                            continue;
                        }
                        const bool had = marks[other->dense] == epoch;
                        if (!had && d2 > ownNear2) continue; // beyond the radius: only what it sees stays a little farther
                        const Candidate candidate{had ? d2 * nearer : d2, other};
                        switch (classOf[o]) {
                        case kItem: items.push_back(candidate); break;
                        case kNpc: npcs.push_back(candidate); break;
                        case kMonster: monsters.push_back(candidate); break;
                        default: players.push_back(candidate); break;
                        }
                    }
                    // Each kind up to its share, the nearest first; the places a kind leaves unused go to
                    // the nearest of what the others have left over.
                    chosen.clear();
                    rest.clear();
                    std::size_t spare = 0;
                    // `lends`: its leftovers may take lent places (not a kind the player lowered itself).
                    const auto take = [&](std::vector<Candidate>& list, std::size_t share, bool lends) {
                        if (list.size() > share) {
                            std::nth_element(list.begin(), list.begin() + std::ptrdiff_t(share), list.end(), byRank);
                            if (lends) rest.insert(rest.end(), list.begin() + std::ptrdiff_t(share), list.end());
                            list.resize(share);
                        } else {
                            spare += share - list.size();
                        }
                        chosen.insert(chosen.end(), list.begin(), list.end());
                    };
                    take(players, std::min(config.viewPlayers, e.viewPlayers), e.viewPlayers >= config.viewPlayers);
                    take(monsters, std::min(config.viewMonsters, e.viewMonsters), e.viewMonsters >= config.viewMonsters);
                    take(items, config.viewItems, true);
                    if (spare > 0 && !rest.empty()) {
                        if (rest.size() > spare) {
                            std::nth_element(rest.begin(), rest.begin() + std::ptrdiff_t(spare), rest.end(), byRank);
                            rest.resize(spare);
                        }
                        chosen.insert(chosen.end(), rest.begin(), rest.end());
                    }
                    // Tiers by rank: the nearest viewTier1, the next viewTier2, the rest. NPCs stand
                    // (they never walk): the farthest tier.
                    const std::size_t t1 = std::min(chosen.size(), config.viewTier1);
                    const std::size_t t2 = std::min(chosen.size(), t1 + config.viewTier2);
                    if (chosen.size() > t1) std::nth_element(chosen.begin(), chosen.begin() + std::ptrdiff_t(t1), chosen.end(), byRank);
                    if (chosen.size() > t2) std::nth_element(chosen.begin() + std::ptrdiff_t(t1), chosen.begin() + std::ptrdiff_t(t2), chosen.end(), byRank);
                    now.clear();
                    for (std::size_t k2 = 0; k2 < chosen.size(); ++k2) {
                        const Entity* o = chosen[k2].second;
                        now.push_back({{o->id, std::uint8_t(k2 < t1 ? 0 : k2 < t2 ? 1 : 2), false, o->dense}, o});
                        if (o->kind == proto::EntityKind::GroundItem) ++tally.items;
                        else if (o->kind == proto::EntityKind::Monster) ++tally.monsters;
                        else ++tally.players;
                    }
                    for (const Candidate& n : npcs) now.push_back({{n.second->id, 2, false, n.second->dense}, n.second});
                    std::sort(now.begin(), now.end(), [](const auto& a, const auto& b) { return a.first.id < b.first.id; });
                    // Against what it saw (both sorted by id): gone, come into view, or a new leg (now,
                    // or held for its tier's tick).
                    next.clear();
                    std::size_t s = 0;
                    bool held = false;
                    const auto gone = [&](std::uint32_t id) {
                        mine.push_back(Make(e.peer, proto::EntityGone{id}));
                        changes[c].push_back({e.id, id, false});
                    };
                    for (auto& [view, o] : now) {
                        for (; s < e.seen.size() && e.seen[s].id < view.id; ++s) gone(e.seen[s].id);
                        if (s < e.seen.size() && e.seen[s].id == view.id) {
                            const bool pending = e.seen[s].pending || o->legChanged;
                            ++s;
                            if (pending && due(view.tier)) {
                                mine.push_back({e.peer, proto::EntityMoved::kId, o->legChanged ? legs[o->slot] : SharedBytes(proto::Encode(Leg(*o))), LegKey(view.id)});
                                ++tally.legsSent;
                            } else if (pending) {
                                view.pending = true;
                                held = true;
                                if (o->legChanged) ++tally.legsDeferred;
                            }
                        } else {
                            mine.push_back({e.peer, proto::EntityAppeared::kId, o->npc ? o->appearBytes : SharedBytes(proto::Encode(Appeared(*o))), 0});
                            if (!o->states.empty()) mine.push_back(Make(e.peer, StatesNow(*o)));
                            if (!o->buffs.empty()) mine.push_back(Make(e.peer, proto::EntityBuffs{o->id, o->buffs}));
                            for (const auto& [id, bytes] : o->extras) mine.push_back({e.peer, id, bytes, 0});
                            changes[c].push_back({e.id, view.id, true});
                        }
                        next.push_back(view);
                    }
                    for (; s < e.seen.size(); ++s) gone(e.seen[s].id);
                    e.seen.swap(next);
                    e.viewPending = held;
                    ++tally.watchers;
                    tally.seen += e.seen.size();
                    tally.seenMax = std::max(tally.seenMax, e.seen.size());
                }
            }
        });
        for (std::vector<WorldMessage>& mine : told) {
            out.insert(out.end(), std::make_move_iterator(mine.begin()), std::make_move_iterator(mine.end()));
        }
        // Each entity's observers follow the views (on this thread: many views write one list).
        for (const std::vector<Change>& list : changes) {
            for (const Change& ch : list) {
                const auto it = entities.find(ch.other);
                if (it == entities.end()) continue; // it left the world
                std::vector<std::uint32_t>& observers = it->second.observers;
                if (ch.added) {
                    observers.push_back(ch.watcher);
                } else if (const auto w = std::find(observers.begin(), observers.end(), ch.watcher); w != observers.end()) {
                    *w = observers.back();
                    observers.pop_back();
                }
            }
        }
        // The views kept as they are: each hears of the new legs of what it holds (from the walkers' observers lists)
        // and, on a farther tier's tick, the legs held for it.
        std::vector<std::uint32_t> keptIndex(placed.size(), UINT32_MAX);
        for (std::size_t k = 0; k < kept.size(); ++k) keptIndex[kept[k]] = std::uint32_t(k);
        std::vector<std::vector<std::uint32_t>> inbox(kept.size()); // the slots of the walkers with a new leg it holds
        for (const std::uint32_t o : changed) {
            for (const std::uint32_t watcher : placed[o]->observers) {
                const auto it = entities.find(watcher);
                if (it == entities.end() || !it->second.placed) continue;
                const std::size_t slot = it->second.slot;
                if (slot >= placed.size() || placed[slot] != &it->second || keptIndex[slot] == UINT32_MAX) continue;
                inbox[keptIndex[slot]].push_back(o);
            }
        }
        const bool heldDue = tick % 2 == 0; // a farther tier's tick (tier 1 every 2nd, tier 2 every 4th)
        std::vector<std::uint32_t> busy; // the kept views with something to tell
        Tally keptTally;
        for (std::size_t k = 0; k < kept.size(); ++k) {
            const Entity& e = *placed[kept[k]];
            if (!inbox[k].empty() || (heldDue && e.viewPending)) busy.push_back(std::uint32_t(k));
            ++keptTally.watchers;
            keptTally.seen += e.seen.size();
            keptTally.seenMax = std::max(keptTally.seenMax, e.seen.size());
        }
        const std::size_t keptChunks = std::clamp<std::size_t>(busy.size(), 1, 256);
        std::vector<std::vector<WorldMessage>> keptTold(keptChunks);
        std::vector<Tally> keptTallies(keptChunks);
        jobs.ParallelFor(busy.empty() ? 0 : keptChunks, 1, [&](std::size_t chunkBegin, std::size_t chunkEnd) {
            SR_ZONE("world: interest (kept views)");
            for (std::size_t c = chunkBegin; c < chunkEnd; ++c) {
                std::vector<WorldMessage>& mine = keptTold[c];
                Tally& tally = keptTallies[c];
                for (std::size_t b = busy.size() * c / keptChunks, last = busy.size() * (c + 1) / keptChunks; b < last; ++b) {
                    const std::uint32_t k = busy[b];
                    Entity& e = *placed[kept[k]];
                    for (const std::uint32_t o : inbox[k]) {
                        const Entity& other = *placed[o];
                        const auto it = std::lower_bound(e.seen.begin(), e.seen.end(), other.id,
                                                         [](const Entity::Seen& s, std::uint32_t v) { return s.id < v; });
                        if (it == e.seen.end() || it->id != other.id) continue;
                        if (due(it->tier)) {
                            mine.push_back({e.peer, proto::EntityMoved::kId, legs[o], LegKey(other.id)});
                            it->pending = false;
                            ++tally.legsSent;
                        } else {
                            it->pending = true;
                            e.viewPending = true;
                            ++tally.legsDeferred;
                        }
                    }
                    if (heldDue && e.viewPending) {
                        bool held = false;
                        for (Entity::Seen& s : e.seen) {
                            if (!s.pending) continue;
                            if (!due(s.tier)) {
                                held = true;
                                continue;
                            }
                            s.pending = false;
                            const auto it = entities.find(s.id);
                            if (it == entities.end() || !it->second.placed) continue; // gone: told so when the view is worked out again
                            mine.push_back({e.peer, proto::EntityMoved::kId, proto::Encode(Leg(it->second)), LegKey(s.id)});
                            ++tally.legsSent;
                        }
                        e.viewPending = held;
                    }
                }
            }
        });
        for (std::vector<WorldMessage>& mine : keptTold) {
            out.insert(out.end(), std::make_move_iterator(mine.begin()), std::make_move_iterator(mine.end()));
        }
        tallies.push_back(keptTally);
        tallies.insert(tallies.end(), keptTallies.begin(), keptTallies.end());
        for (const Tally& t : tallies) {
            stats.watcherTicks += t.watchers;
            stats.seen += t.seen;
            stats.seenPlayers += t.players;
            stats.seenMonsters += t.monsters;
            stats.seenItems += t.items;
            stats.seenMax = std::max(stats.seenMax, t.seenMax);
            stats.legsSent += t.legsSent;
            stats.legsDeferred += t.legsDeferred;
        }
        stats.viewsWorkedOut += full.size();
        // A walker hears of its own new leg too (the server's word on where it goes).
        for (const std::uint32_t w : watchers) {
            const Entity* e = placed[w];
            if (e->legChanged) out.push_back({e->peer, proto::EntityMoved::kId, legs[w], LegKey(e->id)});
        }
        EndInterestPass();
    }

    // The end of an interest pass: every new leg told (or held for its tier), the ground items seen falling are lying.
    void EndInterestPass() {
        for (auto& [id, e] : entities) {
            e.legChanged = false;
            // A ground item seen falling on this pass: lying there for anyone who comes later.
            if (e.fresh) {
                e.fresh = false;
                e.appearBytes = proto::Encode(Appeared(e));
            }
        }
    }

    // A player no longer in anyone's lists as an observer (it leaves, or starts its view afresh).
    void ForgetViews(Entity& watcher) {
        for (const Entity::Seen& s : watcher.seen) {
            const auto it = entities.find(s.id);
            if (it == entities.end()) continue;
            std::vector<std::uint32_t>& observers = it->second.observers;
            if (const auto w = std::find(observers.begin(), observers.end(), watcher.id); w != observers.end()) {
                *w = observers.back();
                observers.pop_back();
            }
        }
        watcher.seen.clear();
        watcher.viewDirty = true;
        watcher.viewPending = false;
    }

    // Its states with the time each has left now (those over left out).
    proto::EntityStates StatesNow(const Entity& o) const {
        proto::EntityStates m{o.id, {}};
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(now - o.statesAt).count();
        for (const proto::StateRecord& r : o.states) {
            if (spent < 0 || std::uint64_t(spent) < r.ms) m.states.push_back({r.kind, r.level, std::uint32_t(r.ms - std::uint64_t(std::max<long long>(0, spent)))});
        }
        return m;
    }

    static proto::EntityAppeared Appeared(const Entity& o) {
        proto::EntityAppeared m;
        m.entity = o.id;
        m.character = Shown(o);
        m.position = o.position;
        m.yaw = o.yaw;
        m.moving = o.moving;
        m.to = o.moving ? o.legTo : o.position;
        m.speed = o.moving ? o.speed : 0.0f;
        m.kind = o.kind;
        m.amount = o.amount;
        m.plus = o.plus;
        m.rarity = o.rarity;
        m.fresh = o.fresh;
        return m;
    }

    static std::uint64_t LegKey(std::uint32_t entity) { return (std::uint64_t{1} << 32) | entity; }
    static proto::EntityMoved Leg(const Entity& e) {
        proto::EntityMoved m;
        m.entity = e.id;
        m.from = e.position; // where it is now: the rest of the leg
        m.to = e.moving ? e.legTo : e.position;
        m.speed = e.moving ? e.speed : 0.0f;
        return m;
    }

    // Players whose navigation came in: placed on the ground, told of themselves.
    void Place(std::vector<WorldMessage>& out) {
        for (auto& [id, e] : entities) {
            if (e.placed || !NavigationReady(e.position.region)) continue;
            if (e.dropOnGround) e.position.y = PutDown(e.position);
            e.nav = {};
            e.placed = true;
            e.viewDirty = true; // its view worked out on the next pass
            proto::WorldEntered m;
            m.entity = id;
            m.character = Shown(e);
            m.position = e.position;
            m.yaw = e.yaw;
            m.walkSpeed = config.walkSpeed;
            m.runSpeed = config.runSpeed;
            m.tickRate = std::uint16_t(std::lround(config.tickRate));
            m.maxHp = e.maxHp;
            m.maxMp = e.maxMp;
            out.push_back(Make(e.peer, m));
        }
    }
};

World::World(const WorldConfig& config, core::JobSystem& jobs, const assets::FileSystem* files)
    : s_(std::make_unique<State>(config, jobs, files)) {}

World::~World() = default;

std::uint32_t World::Enter(const WorldPlayer& player) {
    const std::pair<std::uint32_t, std::uint32_t> made = s_->NewId();
    const std::uint32_t id = made.first;
    State::Entity& e = s_->entities[id];
    e.id = id;
    e.dense = made.second;
    e.peer = player.peer;
    e.character = player.character;
    e.summary = player.summary;
    e.position = player.position;
    e.yaw = player.yaw;
    e.dropOnGround = player.dropOnGround;
    e.watcher = true;
    e.zone = s_->ZoneOf(e.position.region);
    s_->zones[e.zone].push_back(id);
    s_->HoldNavigation(e);
    return id;
}

std::uint32_t World::AddNpc(std::uint32_t model, const proto::WorldPosition& position, float yaw, std::string_view name) {
    const std::pair<std::uint32_t, std::uint32_t> made = s_->NewId();
    const std::uint32_t id = made.first;
    State::Entity& e = s_->entities[id];
    e.id = id;
    e.dense = made.second;
    e.npc = true;
    e.summary.model = model;
    e.summary.name = std::string(name);
    e.position = position;
    e.yaw = yaw;
    e.placed = true;
    e.zone = s_->ZoneOf(e.position.region);
    s_->zones[e.zone].push_back(id);
    e.appearBytes = proto::Encode(State::Appeared(e));
    return id;
}

std::uint32_t World::AddGroundItem(std::uint32_t item, const proto::WorldPosition& position, float yaw, std::uint32_t amount,
                                   std::uint8_t plus, std::uint8_t state, std::string_view name) {
    const std::uint32_t id = AddNpc(item, position, yaw, name);
    State::Entity& e = s_->entities[id];
    e.kind = proto::EntityKind::GroundItem;
    e.fresh = true; // it falls now: the next pass tells it so (Interest)
    e.amount = amount;
    e.plus = plus;
    e.rarity = state;
    e.appearBytes = proto::Encode(State::Appeared(e));
    return id;
}

std::uint32_t World::AddSkillObject(std::uint32_t skill, std::uint32_t owner, const proto::WorldPosition& position, float yaw) {
    const std::uint32_t id = AddNpc(skill, position, yaw);
    State::Entity& e = s_->entities[id];
    e.kind = proto::EntityKind::SkillObject;
    e.amount = owner;
    e.appearBytes = proto::Encode(State::Appeared(e));
    return id;
}

std::uint32_t World::AddMonster(const MonsterLook& look, const proto::WorldPosition& position, float yaw) {
    State& s = *s_;
    if (!NavigationLoaded(position.region)) return 0;
    const game::NavPosition at = s.nav.Drop(position.region, position.x, position.y, position.z);
    if (!at.Valid()) return 0;
    const std::pair<std::uint32_t, std::uint32_t> made = s.NewId();
    const std::uint32_t id = made.first;
    State::Entity& e = s.entities[id];
    e.id = id;
    e.dense = made.second;
    e.kind = proto::EntityKind::Monster;
    e.summary.model = look.model;
    e.summary.hp = look.hp;
    e.rarity = look.rarity;
    e.amount = look.appearance;
    e.walkSpeed = look.walkSpeed;
    e.runSpeed = look.runSpeed;
    e.position = {at.region, at.x, at.y, at.z, position.layer};
    e.nav = at;
    e.yaw = yaw;
    e.placed = true;
    e.zone = s.ZoneOf(e.position.region);
    s.zones[e.zone].push_back(id);
    return id;
}

std::optional<proto::WorldPosition> World::WalkEnd(const proto::WorldPosition& from, const proto::WorldPosition& to) const {
    if (!s_->loader) return to;
    if (!s_->nav.Holds(from.region)) return std::nullopt;
    const game::NavPosition at = s_->nav.Drop(from.region, from.x, from.y, from.z);
    if (!at.Valid()) return std::nullopt;
    game::NavPosition stop;
    const std::uint32_t result = s_->nav.Walk(at, to.region, to.x, to.z, stop, 100000, nullptr, from.layer);
    if ((result & game::nav_walk::kFailed) != 0 || !stop.Valid()) return std::nullopt;
    return proto::WorldPosition{stop.region, stop.x, stop.y, stop.z, from.layer};
}

std::uint32_t World::AddStructure(const MonsterLook& look, const proto::WorldPosition& position, float yaw) {
    State& s = *s_;
    const std::pair<std::uint32_t, std::uint32_t> made = s.NewId();
    const std::uint32_t id = made.first;
    State::Entity& e = s.entities[id];
    e.id = id;
    e.dense = made.second;
    e.kind = proto::EntityKind::Monster;
    e.summary.model = look.model;
    e.summary.hp = look.hp;
    e.rarity = look.rarity;
    e.position = position;
    e.yaw = yaw;
    e.placed = true;
    e.zone = s.ZoneOf(e.position.region);
    s.zones[e.zone].push_back(id);
    return id;
}

std::uint32_t World::AddCos(const MonsterLook& look, std::uint32_t owner, const proto::WorldPosition& position, float yaw) {
    State& s = *s_;
    proto::WorldPosition at = position;
    game::NavPosition nav;
    if (s.loader) {
        if (!NavigationLoaded(position.region)) return 0;
        nav = s.nav.Drop(position.region, position.x, position.y, position.z);
        if (!nav.Valid()) return 0;
        at = {nav.region, nav.x, nav.y, nav.z, position.layer};
    }
    const std::pair<std::uint32_t, std::uint32_t> made = s.NewId();
    const std::uint32_t id = made.first;
    State::Entity& e = s.entities[id];
    e.id = id;
    e.dense = made.second;
    e.kind = proto::EntityKind::Cos;
    e.amount = owner;
    e.summary.model = look.model;
    e.summary.hp = look.hp;
    e.walkSpeed = look.walkSpeed;
    e.runSpeed = look.runSpeed;
    e.position = at;
    e.nav = nav;
    e.yaw = yaw;
    e.placed = true;
    e.zone = s.ZoneOf(e.position.region);
    s.zones[e.zone].push_back(id);
    return id;
}

void World::SetModel(std::uint32_t entity, std::uint32_t model) {
    if (const auto it = s_->entities.find(entity); it != s_->entities.end()) it->second.summary.model = model;
}

void World::SetName(std::uint32_t entity, std::string name) {
    if (const auto it = s_->entities.find(entity); it != s_->entities.end()) it->second.summary.name = std::move(name);
}

void World::SetSpeeds(std::uint32_t entity, float walkSpeed, float runSpeed) {
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end() || (it->second.walkSpeed == walkSpeed && it->second.runSpeed == runSpeed)) return;
    it->second.walkSpeed = walkSpeed;
    it->second.runSpeed = runSpeed;
    Resume(entity);
}

void World::Carry(std::uint32_t rider, std::uint32_t mount) {
    const auto it = s_->entities.find(rider);
    if (it == s_->entities.end()) return;
    it->second.mount = mount;
    if (mount != 0) s_->CopyLeg(it->second);
}

bool World::NavigationLoaded(std::uint16_t region) const {
    return s_->loader && !s_->loader->Loading(region) && s_->nav.Holds(region);
}

bool World::HasNavigation() const { return s_->loader != nullptr; }

void World::SetHp(std::uint32_t entity, std::uint32_t hp) {
    if (const auto it = s_->entities.find(entity); it != s_->entities.end()) it->second.summary.hp = hp;
}

void World::SetStates(std::uint32_t entity, std::vector<proto::StateRecord> states, WorldClock::time_point at) {
    if (const auto it = s_->entities.find(entity); it != s_->entities.end()) {
        it->second.states = std::move(states);
        it->second.statesAt = at;
    }
}

void World::SetExtra(std::uint32_t entity, proto::MessageId id, std::vector<std::byte> payload) {
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end()) return;
    auto& extras = it->second.extras;
    std::erase_if(extras, [&](const auto& e) { return e.first == id; });
    if (!payload.empty()) extras.push_back({id, std::move(payload)});
}

void World::SetBuffs(std::uint32_t entity, std::vector<std::uint32_t> skills) {
    if (const auto it = s_->entities.find(entity); it != s_->entities.end()) it->second.buffs = std::move(skills);
}

void World::SetVitals(std::uint32_t entity, std::uint32_t hp, std::uint32_t maxHp, std::uint32_t mp, std::uint32_t maxMp) {
    if (const auto it = s_->entities.find(entity); it != s_->entities.end()) {
        it->second.summary.hp = hp;
        it->second.summary.mp = mp;
        it->second.maxHp = maxHp;
        it->second.maxMp = maxMp;
    }
}

void World::Leave(std::uint32_t entity) {
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end()) return;
    auto& members = s_->zones[it->second.zone];
    members.erase(std::remove(members.begin(), members.end(), entity), members.end());
    s_->DropNavigation(it->second);
    s_->ForgetViews(it->second);
    s_->Touch(it->second); // its cell lost it: the views around it see it gone when worked out again
    s_->freedDense.push_back(it->second.dense);
    s_->entities.erase(it);
}

void World::SetHidden(std::uint32_t entity, std::uint8_t mask, std::uint8_t level) {
    if (const auto it = s_->entities.find(entity); it != s_->entities.end()) {
        if (it->second.hideMask == mask && it->second.hideLevel == level) return;
        it->second.hideMask = mask;
        it->second.hideLevel = level;
        s_->Touch(it->second); // who may see it changed: the views around it again
    }
}

void World::SetParty(std::uint32_t entity, std::uint32_t party) {
    // Called for every player often (the server's loop): only a change counts.
    if (const auto it = s_->entities.find(entity); it != s_->entities.end() && it->second.party != party) {
        it->second.party = party;
        it->second.viewDirty = true; // a hidden party member: seen now, or no longer
        s_->Touch(it->second);
    }
}

void World::SetDetection(std::uint32_t entity, std::uint8_t mask, std::uint8_t level) {
    if (const auto it = s_->entities.find(entity); it != s_->entities.end()) {
        if (it->second.detectMask == mask && it->second.detectLevel == level) return;
        it->second.detectMask = mask;
        it->second.detectLevel = level;
        it->second.viewDirty = true; // what it detects changed
    }
}

std::optional<World::Pace> World::PaceOf(std::uint32_t entity) const {
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end()) return std::nullopt;
    const State::Entity& e = it->second;
    Pace p;
    p.walk = (e.walkSpeed > 0 ? e.walkSpeed : s_->config.walkSpeed) * e.speedFactor;
    p.run = (e.runSpeed > 0 ? e.runSpeed : s_->config.runSpeed) * e.speedFactor;
    p.now = e.moving ? e.speed : 0.0f;
    p.walking = e.walkMode;
    return p;
}

void World::SetSpeedFactor(std::uint32_t entity, float factor) {
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end() || it->second.speedFactor == factor) return;
    it->second.speedFactor = factor;
    Resume(entity);
}

void World::Resume(std::uint32_t entity) {
    // A walk under way goes on at the new speed from where it stands (the original's speed change shows at once,
    // not from the next click): its leg again to the same end, told as any new leg.
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end() || !it->second.placed || !it->second.moving) return;
    State::Entity& e = it->second;
    if (e.heading) {
        WalkHeading(entity);
        return;
    }
    const proto::WorldPosition to = e.legTo;
    MoveTo(entity, to, e.legWalk);
}

World::MoveRefused World::MoveTo(std::uint32_t entity, const proto::WorldPosition& targetIn, bool walk) {
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end() || !it->second.placed) return MoveRefused::Unknown;
    State::Entity& e = it->second;
    e.heading = false; // any other walk ends a walk toward a heading (MoveToward sets it again)
    proto::WorldPosition target = targetIn;
    target.layer = e.position.layer; // a walk stays in its layer (a client's click has none)
    const Flat from = ToFlat(e.position), wanted = ToFlat(target);
    const double distance = std::hypot(wanted.x - from.x, wanted.z - from.z);
    if (distance > s_->config.maxMove) { // farther than a click reaches
        ++s_->stats.refused;
        ++s_->stats.refusedFar;
        return MoveRefused::Far;
    }
    // A click beyond one walk's reach on the navigation (held a region about the walker, a walk
    // spans at most a region; a far one on the sky or a long view): toward it, as far as that goes.
    proto::WorldPosition aim = target;
    if (s_->loader && distance > kWalkReach) {
        const double k = kWalkReach / distance;
        aim = FromFlat({from.x + (wanted.x - from.x) * k, from.z + (wanted.z - from.z) * k}, target.y);
    }
    proto::WorldPosition end = aim;
    e.legStart = e.legEnd = {};
    if (s_->loader) {
        // The walk on the server's own navigation: it ends where the way is blocked, or at the edge
        // of the regions held (one still loading included: the click is not lost).
        if (!s_->NavigationReady(e.position.region) || !s_->nav.Holds(e.position.region)) {
            ++s_->stats.refused;
            ++s_->stats.refusedNavigation;
            return MoveRefused::Navigation;
        }
        const game::NavPosition at = s_->Standing(e);
        if (!at.Valid()) {
            ++s_->stats.refused;
            ++s_->stats.refusedFloor;
            return MoveRefused::Floor;
        }
        game::NavPosition stop;
        const std::uint32_t result = s_->nav.Walk(at, aim.region, aim.x, aim.z, stop, 100000, nullptr, e.position.layer);
        if ((result & game::nav_walk::kFailed) != 0 || !stop.Valid()) {
            ++s_->stats.refused;
            ++s_->stats.refusedWalk;
            return MoveRefused::Walk;
        }
        end = {stop.region, stop.x, stop.y, stop.z, e.position.layer};
        e.legStart = at;
        e.legEnd = stop;
    }
    const Flat to = ToFlat(end);
    e.legFrom = e.position;
    e.legTo = end;
    e.legZones.clear(); // a new leg enters its zones afresh
    e.length = std::hypot(to.x - from.x, to.z - from.z);
    e.travelled = 0;
    const float run = e.runSpeed > 0 ? e.runSpeed : s_->config.runSpeed;
    const float walkSpeed = e.walkSpeed > 0 ? e.walkSpeed : s_->config.walkSpeed;
    e.speed = (walk || e.walkMode ? walkSpeed : run) * e.speedFactor;
    e.legWalk = walk;
    e.moving = e.length > 0.01;
    if (e.moving) e.yaw = float(std::atan2(to.x - from.x, to.z - from.z));
    e.legChanged = true;
    ++s_->stats.legs;
    s_->CopyLeg(e);
    return MoveRefused::None;
}

World::MoveRefused World::MoveToward(std::uint32_t entity, const proto::WorldPosition& toward) {
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end() || !it->second.placed) return MoveRefused::Unknown;
    State::Entity& e = it->second;
    const Flat from = ToFlat(e.position), to = ToFlat(toward);
    const double dx = to.x - from.x, dz = to.z - from.z, length = std::hypot(dx, dz);
    if (length < 1.0) return MoveTo(entity, toward); // no way to speak of: a plain click
    e.headingX = dx / length;
    e.headingZ = dz / length;
    return WalkHeading(entity);
}

World::MoveRefused World::WalkHeading(std::uint32_t entity) {
    State::Entity& e = s_->entities.at(entity);
    const double hx = e.headingX, hz = e.headingZ;
    const Flat from = ToFlat(e.position);
    // One walk's reach on (a walk spans at most a region about the walker); without a navigation, as far.
    const double reach = kWalkReach;
    const proto::WorldPosition aim = FromFlat({from.x + hx * reach, from.z + hz * reach}, e.position.y);
    const MoveRefused why = MoveTo(entity, aim); // ends the heading; set again below
    e.headingX = hx;
    e.headingZ = hz;
    if (why == MoveRefused::Navigation) {
        // The navigation where it stands not in yet (a region still loading): the same way again next tick.
        e.heading = true;
        e.headingLast = false;
        return why;
    }
    if (why != MoveRefused::None) return why;
    e.heading = true;
    // A leg shorter than its aim stopped at something (a wall, a cliff, the edge of the navigation): the last.
    e.headingLast = e.length < reach - 1.0;
    return why;
}

bool World::WalksToward(std::uint32_t entity) const {
    const auto it = s_->entities.find(entity);
    return it != s_->entities.end() && it->second.heading;
}

std::optional<proto::WorldPosition> World::Blink(std::uint32_t entity, const proto::WorldPosition& toIn, float most) {
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end() || !it->second.placed) return std::nullopt;
    State::Entity& e = it->second;
    proto::WorldPosition to = toIn;
    to.layer = e.position.layer;
    const Flat from = ToFlat(e.position), wanted = ToFlat(to);
    // The most distance in three dimensions, the height's too.
    const double dy = double(to.y) - double(e.position.y);
    const double distance = std::sqrt((wanted.x - from.x) * (wanted.x - from.x) + (wanted.z - from.z) * (wanted.z - from.z) + dy * dy);
    proto::WorldPosition aim = to;
    if (distance > double(most) && distance > 0.0) {
        const double k = double(most) / distance;
        aim = FromFlat({from.x + (wanted.x - from.x) * k, from.z + (wanted.z - from.z) * k}, float(double(e.position.y) + dy * k));
    }
    proto::WorldPosition end = aim;
    if (s_->loader) {
        // Along the navigation, as a walk would go (no blink through a wall), stopping where it is blocked.
        if (!s_->NavigationReady(e.position.region) || !s_->nav.Holds(e.position.region)) return std::nullopt;
        const game::NavPosition at = s_->Standing(e);
        if (!at.Valid()) return std::nullopt;
        game::NavPosition stop;
        const std::uint32_t result = s_->nav.Walk(at, aim.region, aim.x, aim.z, stop, 100000, nullptr, e.position.layer);
        if ((result & game::nav_walk::kFailed) != 0 || !stop.Valid()) return std::nullopt;
        end = {stop.region, stop.x, stop.y, stop.z, e.position.layer};
        e.nav = stop;
    }
    const Flat a = ToFlat(e.position), b = ToFlat(end);
    if (std::hypot(b.x - a.x, b.z - a.z) > 0.01) e.yaw = float(std::atan2(b.x - a.x, b.z - a.z));
    e.position = end;
    e.legFrom = e.legTo = end;
    e.legStart = e.legEnd = {};
    e.length = e.travelled = 0;
    e.moving = false;
    e.heading = false;
    e.legChanged = true; // its observers hear it stands there (its zone follows on the next tick)
    ++s_->stats.legs;
    s_->CopyLeg(e);
    return end;
}

void World::SetNavZone(std::uint32_t key, const std::string& name, bool blocks, std::uint32_t layer) {
    s_->nav.SetZoneBlocks(key, name, blocks, layer);
}

void World::SetWalking(std::uint32_t entity, bool walking) {
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end() || it->second.walkMode == walking) return;
    it->second.walkMode = walking;
    Resume(entity);
}

std::vector<World::ZoneEntered> World::TakeZonesEntered() {
    std::vector<ZoneEntered> out;
    out.swap(s_->zonesEntered);
    return out;
}

bool World::Teleport(std::uint32_t entity, const proto::WorldPosition& to) {
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end()) return false;
    State::Entity& e = it->second;
    e.moving = false;
    e.heading = false;
    e.legZones.clear();
    e.zonesPending.clear();
    e.legChanged = true; // those who keep it in view hear where it stands now (it is not "new" to them)
    e.position = to;
    e.placed = false; // out of everyone's view until the navigation there is in
    e.dropOnGround = true;
    s_->ForgetViews(e); // told of everyone around it afresh
    auto& members = s_->zones[e.zone];
    members.erase(std::remove(members.begin(), members.end(), entity), members.end());
    e.zone = s_->ZoneOf(to.region);
    s_->zones[e.zone].push_back(entity);
    s_->HoldNavigation(e);
    return true;
}

void World::Update(WorldClock::time_point now, std::vector<WorldMessage>& out) {
    State& s = *s_;
    SR_ZONE("world: update");
    s.now = now;
    if (s.loader) s.loader->Update(s.nav);
    s.Place(out);
    const auto step = std::chrono::duration_cast<WorldClock::duration>(std::chrono::duration<double>(s.Step()));
    if (!s.started) {
        s.started = true;
        s.nextTick = now;
    }
    int ran = 0;
    const auto begin = WorldClock::now();
    while (now >= s.nextTick && ran < s.config.maxCatchUp) {
        s.StepZones(s.Step());
        std::vector<std::uint32_t> headingOn;
        for (auto& [id, e] : s.entities) {
            for (std::string& z : e.zonesPending) s.zonesEntered.push_back({id, std::move(z)});
            e.zonesPending.clear();
            if (e.heading && !e.moving) {
                if (e.headingLast || !e.placed) {
                    e.heading = false; // it met a wall: it stands there
                } else {
                    headingOn.push_back(id);
                }
            }
        }
        // A walk toward a heading whose leg ended unblocked: the next leg on the same way (here, on this thread: a
        // leg's walk on the navigation and its copy to a mount).
        for (const std::uint32_t id : headingOn) WalkHeading(id);
        s.nextTick += step;
        ++s.ticks;
        ++ran;
    }
    if (now >= s.nextTick) {
        // Too far behind: the rest is dropped.
        s.stats.dropped += std::uint64_t((now - s.nextTick) / step) + 1;
        s.nextTick = now + step;
    }
    if (ran == 0) return;
    s.lastStepMs = std::chrono::duration<double, std::milli>(WorldClock::now() - begin).count() / ran;
    // Walkers that changed region hold the navigation around their new one.
    for (auto& [id, e] : s.entities) {
        if (e.watcher && e.navCentre != e.position.region) s.HoldNavigation(e);
    }
    const auto interest = WorldClock::now();
    const std::size_t told = out.size();
    s.Interest(out);
    const auto end = WorldClock::now();
    const double tickMs = std::chrono::duration<double, std::milli>(end - begin).count() / ran;
    s.stats.ticks += std::uint64_t(ran);
    s.stats.tickMsSum += tickMs * ran;
    s.stats.tickMsMax = std::max(s.stats.tickMsMax, tickMs);
    if (tickMs > 1000.0 * s.Step()) s.stats.overBudget += std::uint64_t(ran);
    s.stats.interestMsMax = std::max(s.stats.interestMsMax, std::chrono::duration<double, std::milli>(end - interest).count());
    s.stats.messages += out.size() - told;
    s.stats.entities = s.entities.size();
}

WorldStats World::TakeStats() {
    WorldStats taken = s_->stats;
    taken.entities = s_->entities.size();
    s_->stats = {};
    return taken;
}

WorldClock::time_point World::NextTick() const { return s_->started ? s_->nextTick : WorldClock::now(); }

std::optional<World::Place> World::Where(std::uint32_t entity) const {
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end() || !it->second.placed) return std::nullopt;
    return Place{it->second.position, it->second.yaw};
}

std::uint32_t World::ModelOf(std::uint32_t entity) const {
    const auto it = s_->entities.find(entity);
    return it != s_->entities.end() ? it->second.summary.model : 0u;
}

bool World::Moving(std::uint32_t entity) const {
    const auto it = s_->entities.find(entity);
    return it != s_->entities.end() && it->second.moving;
}

std::optional<proto::WorldPosition> World::Heading(std::uint32_t entity) const {
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end() || !it->second.moving) return std::nullopt;
    return it->second.legTo;
}

void World::ForEachPlayer(const std::function<void(std::int64_t, const Place&)>& visit) const {
    for (const auto& [id, e] : s_->entities) {
        if (e.placed && e.watcher) visit(e.character, Place{e.position, e.yaw});
    }
}

proto::CharacterSummary World::State::Shown(const Entity& e) {
    proto::CharacterSummary s = e.summary;
    if (e.shown) {
        s.model = e.shown->model;
        s.height = e.shown->height;
        s.equipment = e.shown->equipment;
        s.avatars = e.shown->avatars;
    }
    return s;
}

void World::SetShownLook(std::uint32_t entity, std::optional<proto::CharacterSummary> look) {
    if (const auto it = s_->entities.find(entity); it != s_->entities.end()) it->second.shown = std::move(look);
}

std::optional<proto::CharacterSummary> World::OwnLook(std::uint32_t entity) const {
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end()) return std::nullopt;
    return it->second.summary;
}

void World::SetModelLook(std::uint32_t entity, std::uint32_t model, std::uint8_t height) {
    if (const auto it = s_->entities.find(entity); it != s_->entities.end()) {
        it->second.summary.model = model;
        it->second.summary.height = height;
    }
}

void World::SetEquipment(std::uint32_t entity, std::vector<proto::EquippedItem> equipment) {
    if (const auto it = s_->entities.find(entity); it != s_->entities.end()) it->second.summary.equipment = std::move(equipment);
}

void World::SetAvatars(std::uint32_t entity, std::vector<proto::EquippedItem> avatars) {
    if (const auto it = s_->entities.find(entity); it != s_->entities.end()) it->second.summary.avatars = std::move(avatars);
}

void World::ForEachObserver(std::uint32_t entity, const std::function<void(std::uint64_t)>& visit) const {
    ForEachObserverWithin(entity, 2, visit);
}

void World::ForEachObserverWithin(std::uint32_t entity, int maxTier, const std::function<void(std::uint64_t)>& visit) const {
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end()) return;
    if (it->second.watcher) visit(it->second.peer);
    for (const std::uint32_t watcher : it->second.observers) {
        const auto w = s_->entities.find(watcher);
        if (w == s_->entities.end()) continue;
        if (maxTier < 2) {
            const State::Entity::Seen* seen = nullptr;
            if (!State::Sees(w->second.seen, entity, &seen) || seen->tier > maxTier) continue;
        }
        visit(w->second.peer);
    }
}

World::ViewLimitsApplied World::SetViewLimits(std::uint32_t entity, std::size_t players, std::size_t monsters, float radius) {
    const auto it = s_->entities.find(entity);
    if (it == s_->entities.end() || !it->second.watcher) return {};
    State::Entity& e = it->second;
    e.viewPlayers = players == 0 ? SIZE_MAX : std::min(players, s_->config.viewPlayers);
    e.viewMonsters = monsters == 0 ? SIZE_MAX : std::min(monsters, s_->config.viewMonsters);
    e.viewRadius = radius > 0.0f ? std::min(double(radius), double(s_->config.viewRadius)) : 0.0;
    e.viewDirty = true;
    return {std::min(e.viewPlayers, s_->config.viewPlayers), std::min(e.viewMonsters, s_->config.viewMonsters)};
}

const WorldConfig& World::Config() const { return s_->config; }

std::uint64_t World::PeerOf(std::uint32_t entity) const {
    const auto it = s_->entities.find(entity);
    return it != s_->entities.end() && it->second.watcher ? it->second.peer : 0;
}

std::size_t World::Entities() const { return s_->entities.size(); }
std::uint64_t World::Ticks() const { return s_->ticks; }
double World::TickRate() const { return s_->config.tickRate; }
std::size_t World::ActiveZones() const { return s_->activeZones; }
double World::LastStepMs() const { return s_->lastStepMs; }

} // namespace sr::server
