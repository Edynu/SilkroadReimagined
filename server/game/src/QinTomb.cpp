#include "sr/server/QinTomb.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <map>
#include <random>
#include <string_view>
#include <vector>

#include "sr/assets/GameData.h"
#include "sr/core/Log.h"
#include "sr/server/Instances.h"
#include "sr/server/Monsters.h"

namespace sr::server {

namespace proto = game::proto;

namespace {

// The worlds (`_RefGame_World` 14 and 15: type 1, up to 5 copies of 500 players; no duration, no empty time of their own).
constexpr std::uint32_t kFloor5World = 14, kFloor6World = 15;
constexpr std::uint32_t kMaxUsers = 500;
constexpr int kScheduleSnakeGate = 2; // `_RefScheduleDefine` 2: `_Schedule` rows 8, 9 (10:59:12 and 13:59:12, 600 s)
// Their GAMEWORLD_DISAPPEARANCE_TIME rows (GROUP_QINSHI_FLOOR_5 / _6): the copies' last hour after the gate, its warnings at
// 60, 30, 10 and 5 minutes left (as the Pharaoh's tomb's rows are its warnings).
constexpr std::array<std::uint32_t, 4> kDisappearMs{3'600'000, 1'800'000, 600'000, 300'000};
// Until the gate shuts the copies have no end (ours: a day, moved to the last hour at the shut).
constexpr std::int64_t kUntilShutMs = 86'400'000;
// A portal's object (the runtime teleport INS_QUEST_TELEPORT, used as a gap; the original's RuntimePortal: inferred).
constexpr std::uint32_t kPortalObject = 19556;

struct Place {
    float x, y, z;
};

// The spawners of the original's instance triggers (names its own), each a wave of monsters (its MobShower entries: count
// each, at one place of the floor) and the spawner it fires when the wave is dead; the Flame Master's needs the four
// guardians. Their tactics (Tab_RefTactics of the floors' nests, hives 523..525, and of the crowd's monsters): every one
// aggressive with a trace of 500, a sight of 200 (the White Snake 350 and a trace of 350; the crowd 100).
struct Wave {
    std::string_view name;
    int floor; // 5 or 6
    std::vector<std::string_view> monsters;
    int count; // of each
    Place at;
    float sight, trace;
    std::string_view fires;
    std::vector<std::string_view> needs; // spawners cleared before it starts (else it starts when fired)
};
const std::vector<Wave>& Waves() {
    static const std::vector<Wave> waves{
        {"Chins_Floor5_East", 5, {"MOB_TQ_EASTGUARDIAN"}, 1, {16203, -75, -396}, 200, 500, "Chins_Floor5_FlameMaster", {}},
        {"Chins_Floor5_South", 5, {"MOB_TQ_SOUTHGUARDIAN"}, 1, {222, -38, -15928}, 200, 500, "Chins_Floor5_FlameMaster", {}},
        {"Chins_Floor5_West", 5, {"MOB_TQ_WESTGUARDIAN"}, 1, {-15163, -10, 1733}, 200, 500, "Chins_Floor5_FlameMaster", {}},
        {"Chins_Floor5_North", 5, {"MOB_TQ_NORTHGUARDIAN"}, 1, {267, -10, 15848}, 200, 500, "Chins_Floor5_FlameMaster", {}},
        {"Chins_Floor5_FlameMaster",
         5,
         {"MOB_TQ_FLAMEMASTER"},
         1,
         {-1210, -51, -932},
         200,
         500,
         "Chins_Floor5_Portal",
         {"Chins_Floor5_East", "Chins_Floor5_West", "Chins_Floor5_North", "Chins_Floor5_South"}},
        {"Chins_Floor6_East", 6, {"MOB_TQ_BLACKSNAKE"}, 1, {8207, 132, -10}, 200, 500, "Chins_Floor6_Portal1", {}},
        {"Chins_Floor6_South",
         6,
         {"MOB_TQ_SNAKEGENERAL", "MOB_TQ_SNAKEGENERAL_CLON", "MOB_TQ_SNAKEGENERAL_CLON2", "MOB_TQ_SNAKEGENERAL_CLON3"},
         1,
         {-17, 132, -8213},
         200,
         500,
         "Chins_Floor6_Portal2",
         {}},
        {"Chins_Floor6_West",
         6,
         {"MOB_TQ_TOMBBUGGHOST_CLON2", "MOB_TQ_SNAKESLAVE_CLON2", "MOB_TQ_TOMBBUGGHOST_CLON", "MOB_TQ_SNAKEWOMAN_CLON2", "MOB_TQ_TOMBBUGGHOST",
          "MOB_TQ_SNAKESLAVE_CLON", "MOB_TQ_SNAKEWOMAN_CLON", "MOB_TQ_SNAKEDEMON_CLON2", "MOB_TQ_SNAKESLAVE", "MOB_TQ_SNAKEDEMON_CLON",
          "MOB_TQ_SNAKEMAN_CLON2", "MOB_TQ_SNAKEWOMAN", "MOB_TQ_SNAKEMAN_CLON", "MOB_TQ_SNAKEDEMON", "MOB_TQ_SNAKEMAN"},
         5,
         {-8199, 132, 25},
         100,
         500,
         "Chins_Floor6_Portal3",
         {}},
        {"Chins_Floor6_North", 6, {"MOB_TQ_WHITESNAKE"}, 1, {37, 132, 8199}, 350, 350, "Chins_Floor6_Portal5", {}},
    };
    return waves;
}
// A wave's nests: where its monsters wander and how far they may be led (the floors' nests' nGenerateRadius and nRadius).
constexpr float kSpawnRadius = 100.0f, kLeash = 150.0f;

// The portal spawners: each puts up its portals (a place on its floor, a destination on `toFloor`) and fires the next when
// it stands (Portal1, 2, 3 and 5 raise Portal4).
struct PortalSpawner {
    std::string_view name;
    int floor, toFloor;
    std::vector<std::pair<Place, Place>> portals; // where it stands, where it leads
    std::string_view fires;
};
const std::vector<PortalSpawner>& PortalSpawners() {
    static const std::vector<PortalSpawner> portals{
        {"Chins_Floor6_Portal1", 6, 6, {{{4585, -49, 39}, {0, 1785, 59}}}, "Chins_Floor6_Portal4"},
        {"Chins_Floor6_Portal2", 6, 6, {{{37, -49, -4586}, {0, 1785, 59}}}, "Chins_Floor6_Portal4"},
        {"Chins_Floor6_Portal3", 6, 6, {{{-4587, -49, -40}, {0, 1785, 59}}}, "Chins_Floor6_Portal4"},
        {"Chins_Floor6_Portal5", 6, 6, {{{-42, -49, 4584}, {0, 1785, 59}}}, "Chins_Floor6_Portal4"},
        {"Chins_Floor6_Portal4", 6, 6, {{{-131, 1890, 2535}, {-73, 140, 7089}}}, ""},
    };
    return portals;
}
// Floor 5's (Chins_Floor5_Portal): three portals into floor 6, its i-th at the place the drawn order's i-th names, to the
// i-th destination (rand % 6 picks the order).
constexpr std::array<Place, 3> kFloor5PortalPlaces{{{1498, 59, 1650}, {1815, 68, -1334}, {-1349, 68, 1101}}};
constexpr std::array<Place, 3> kFloor5PortalTo{{{7086, 140, 73}, {69, 140, -7092}, {-7089, 140, -71}}};
constexpr std::array<std::array<int, 3>, 6> kOrders{{{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};
constexpr std::string_view kFloor5Portal = "Chins_Floor5_Portal";

} // namespace

struct QinTomb::State {
    QinTombHost& host;
    Instances& instances;
    Monsters& monsters;
    const assets::CharacterTable& characters;
    QinTombConfig config;
    std::mt19937 random;

    bool gateOpen = false;
    bool shut = false;           // the gate shut on these copies (their last hour runs)
    std::int64_t periodEnd = 0;  // the schedule's (Unix s; 0: a developer's opening, shut by hand)
    std::int64_t lastOpened = 0; // the period opened last (its start)
    std::uint32_t floor5 = 0, floor6 = 0;
    int order = 0; // floor 5's portals' order (kOrders)
    struct Spawner {
        bool started = false, cleared = false;
        std::vector<std::size_t> nests;
        std::vector<std::uint32_t> portals;
    };
    std::map<std::string, Spawner, std::less<>> spawners;

    State(QinTombHost& h, Instances& i, Monsters& m, const assets::CharacterTable& c, QinTombConfig cfg, std::uint32_t seed)
        : host(h), instances(i), monsters(m), characters(c), config(std::move(cfg)), random(seed) {}

    std::uint32_t LayerOf(int floor) const { return floor == 5 ? floor5 : floor6; }
    static std::uint16_t RegionOf(int floor) { return floor == 5 ? kFloor5 : kFloor6; }
    proto::WorldPosition On(int floor, const Place& p) const {
        proto::WorldPosition at{RegionOf(floor), p.x, p.y, p.z};
        at.layer = LayerOf(floor);
        return at;
    }

    GameWorldSpec Spec(std::uint32_t id, const char* codename) const {
        GameWorldSpec spec;
        spec.id = id;
        spec.codename = codename;
        spec.maxCount = 5;
        spec.maxUsers = kMaxUsers;
        spec.durationMs = std::uint32_t(kUntilShutMs);
        spec.emptyMs = spec.durationMs; // an empty copy ends with its pair once the gate shut (Tick)
        spec.gapMs = 0;
        spec.noJobSuit = false;
        // No start of its own (no _RefInstance_World_Start_Pos row): a resurrection in town, out of it.
        for (const std::uint32_t ms : kDisappearMs) spec.warnMs.push_back(std::uint32_t(double(ms) / std::max(0.001, config.timeScale)));
        return spec;
    }

    // --- The spawners --------------------------------------------------------------------------------------------

    void StartWave(const Wave& w) {
        Spawner& s = spawners[std::string(w.name)];
        if (s.started) return;
        s.started = true;
        for (const std::string_view code : w.monsters) {
            const assets::ObjectRow* row = characters.Find(code);
            if (row == nullptr) {
                core::LogWarning("qin tomb: characterdata has no {}", code);
                continue;
            }
            MonsterNest n;
            n.refObjId = row->refObjId;
            n.position = On(w.floor, w.at);
            n.maxCount = w.count;
            n.respawn = false;
            n.radius = kLeash;
            n.spawnRadius = kSpawnRadius;
            n.aggressive = true;
            n.sight = w.sight;
            n.trace = w.trace;
            n.traceBoundary = 1;
            n.partyMonsters = false; // PARTYMONSTER_SPAWN is not ON for these worlds
            const std::size_t index = monsters.AddNest(n);
            if (index != SIZE_MAX) s.nests.push_back(index);
        }
        core::LogInfo("qin tomb: {} ({} monsters)", w.name, s.nests.size() * std::size_t(w.count));
        // A wave with nothing to raise is cleared at once (its chain goes on).
        if (s.nests.empty()) s.cleared = true;
    }

    void PutPortals(std::string_view name, int floor, int toFloor, const std::vector<std::pair<Place, Place>>& list) {
        Spawner& s = spawners[std::string(name)];
        if (s.started) return;
        s.started = true;
        for (const auto& [at, to] : list) {
            const std::uint32_t portal = instances.AddPortal(LayerOf(floor), kPortalObject, On(floor, at), LayerOf(toFloor), On(toFloor, to));
            if (portal != 0) s.portals.push_back(portal);
        }
        core::LogInfo("qin tomb: {} ({} portals)", name, s.portals.size());
    }

    // A spawner fired: a wave starts once what it needs is cleared; a portal spawner stands and fires its own next.
    void Fire(std::string_view name) {
        if (name.empty()) return;
        for (const Wave& w : Waves()) {
            if (w.name != name) continue;
            for (const std::string_view need : w.needs) {
                const auto it = spawners.find(need);
                if (it == spawners.end() || !it->second.cleared) return;
            }
            return StartWave(w);
        }
        if (name == kFloor5Portal) {
            std::vector<std::pair<Place, Place>> list;
            for (std::size_t i = 0; i < kFloor5PortalTo.size(); ++i) {
                list.push_back({kFloor5PortalPlaces[std::size_t(kOrders[std::size_t(order)][i])], kFloor5PortalTo[i]});
            }
            return PutPortals(name, 5, 6, list);
        }
        for (const PortalSpawner& p : PortalSpawners()) {
            if (p.name != name) continue;
            const bool before = spawners[std::string(name)].started;
            PutPortals(p.name, p.floor, p.toFloor, p.portals);
            if (!before) Fire(p.fires);
            return;
        }
    }

    // --- The gate and the copies ---------------------------------------------------------------------------------

    bool OpenGate(Clock::time_point now, std::int64_t until) {
        if (gateOpen) return false;
        // One pair of copies at a time: an older one still standing ends first.
        if (floor5 != 0 || floor6 != 0) EndCopies("a new opening");
        floor5 = instances.OpenWorld(Spec(kFloor5World, "INS_QINSHI_FLOOR_5"), "the snake gate");
        floor6 = instances.OpenWorld(Spec(kFloor6World, "INS_QINSHI_FLOOR_6"), "the snake gate");
        if (floor5 == 0 || floor6 == 0) {
            core::LogWarning("qin tomb: no layer for the floors' copies");
            EndCopies("no layer");
            return false;
        }
        (void)now;
        gateOpen = true;
        shut = false;
        periodEnd = until;
        spawners.clear();
        order = int(random() % kOrders.size());
        // The four guardians of floor 5 and floor 6's four sides from the start.
        for (const Wave& w : Waves()) {
            if (w.needs.empty()) StartWave(w);
        }
        core::LogInfo("qin tomb: the snake gate is open (floor 5's copy layer {}, floor 6's layer {})", floor5, floor6);
        host.QinShardNotice("UIIT_TQ_F4_TELEPORT_OPEN");
        return true;
    }

    // The gate shut: no one else comes in; the copies' last hour begins.
    void ShutGate(Clock::time_point now) {
        if (!gateOpen) return;
        gateOpen = false;
        periodEnd = 0;
        shut = true;
        const auto ends = now + std::chrono::milliseconds(std::int64_t(double(kDisappearMs[0]) / std::max(0.001, config.timeScale)));
        if (floor5 != 0) instances.EndAt(floor5, ends);
        if (floor6 != 0) instances.EndAt(floor6, ends);
        core::LogInfo("qin tomb: the snake gate is shut (the copies end in {} s)", std::chrono::duration_cast<std::chrono::seconds>(ends - now).count());
        host.QinShardNotice("UIIT_TQ_F4_TELEPORT_CLOSE");
    }

    void RemoveNests() {
        for (auto& [name, s] : spawners) {
            for (const std::size_t n : s.nests) monsters.RemoveNest(n);
            s.nests.clear();
        }
    }

    void EndCopies(const char* why) {
        if (gateOpen) {
            gateOpen = false;
            periodEnd = 0;
            host.QinShardNotice("UIIT_TQ_F4_TELEPORT_CLOSE");
        }
        RemoveNests();
        spawners.clear();
        if (floor5 != 0) instances.CloseWorld(floor5);
        if (floor6 != 0) instances.CloseWorld(floor6);
        if (floor5 != 0 || floor6 != 0) core::LogInfo("qin tomb: the floors' copies end: {}", why);
        floor5 = floor6 = 0;
        shut = false;
    }

    void TickCopies(Clock::time_point now) {
        (void)now;
        if (floor5 == 0 && floor6 == 0) return;
        // Their time over (Instances ended them): the spawners' nests go too.
        if ((floor5 != 0 && !instances.Stands(floor5)) || (floor6 != 0 && !instances.Stands(floor6))) return EndCopies("their time is over");
        // The gate shut and nobody left in either: nobody can come back, both end.
        if (shut && instances.Inside(floor5) == 0 && instances.Inside(floor6) == 0) return EndCopies("nobody is left inside");
        // The waves: one whose monsters are all dead is cleared, and fires its next.
        for (const Wave& w : Waves()) {
            const auto it = spawners.find(w.name);
            if (it == spawners.end() || !it->second.started || it->second.cleared) continue;
            const bool dead = std::all_of(it->second.nests.begin(), it->second.nests.end(), [&](std::size_t n) { return monsters.NestCleared(n); });
            if (!dead) continue;
            it->second.cleared = true;
            core::LogInfo("qin tomb: {} cleared", w.name);
            Fire(w.fires);
        }
    }
};

QinTomb::QinTomb(QinTombHost& host, Instances& instances, Monsters& monsters, const assets::CharacterTable& characters, QinTombConfig config,
                 std::uint32_t seed)
    : s_(std::make_unique<State>(host, instances, monsters, characters, std::move(config), seed)) {}

QinTomb::~QinTomb() = default;

void QinTomb::Tick(Clock::time_point now) {
    State& s = *s_;
    if (s.config.enabled) {
        const std::int64_t unix = std::int64_t(std::time(nullptr));
        std::optional<game::Period> period;
        if (s.config.cycleOpen > 0 && s.config.cyclePeriod > 0) {
            const std::int64_t base = unix - unix % s.config.cyclePeriod;
            period = game::Period{base, base + s.config.cycleOpen};
        } else {
            period = game::SchedulePeriod(s.config.schedule, kScheduleSnakeGate, unix);
        }
        if (period && period->Contains(unix) && period->start != s.lastOpened && !s.gateOpen) {
            s.lastOpened = period->start;
            s.OpenGate(now, period->end);
        }
        if (s.gateOpen && s.periodEnd != 0 && unix >= s.periodEnd) s.ShutGate(now);
    }
    s.TickCopies(now);
}

bool QinTomb::Open(Clock::time_point now) { return s_->OpenGate(now, 0); }
void QinTomb::Shut(Clock::time_point now) { s_->ShutGate(now); }
void QinTomb::End() { s_->EndCopies("a developer's end"); }

proto::GateStatus QinTomb::Admit(std::uint32_t entity) {
    State& s = *s_;
    if (!s.gateOpen || s.floor5 == 0) return proto::GateStatus::Nowhere;
    if (s.instances.LayerOf(entity) != 0) return proto::GateStatus::InInstance;
    if (s.instances.Inside(s.floor5) >= kMaxUsers) return proto::GateStatus::InstanceFull;
    const auto at = s.host.QinPoint(kFloor5Gate);
    if (!at) return proto::GateStatus::Nowhere;
    proto::WorldPosition there = *at;
    there.layer = s.floor5;
    return s.instances.AdmitAt(entity, s.floor5, there) ? proto::GateStatus::Ok : proto::GateStatus::Nowhere;
}

bool QinTomb::GateOpen() const { return s_->gateOpen; }
std::uint32_t QinTomb::Floor5() const { return s_->floor5; }
std::uint32_t QinTomb::Floor6() const { return s_->floor6; }
bool QinTomb::Started(const std::string& spawner) const {
    const auto it = s_->spawners.find(spawner);
    return it != s_->spawners.end() && it->second.started;
}
bool QinTomb::Cleared(const std::string& spawner) const {
    const auto it = s_->spawners.find(spawner);
    return it != s_->spawners.end() && it->second.cleared;
}
std::vector<std::uint32_t> QinTomb::Portals(std::uint32_t layer) const {
    std::vector<std::uint32_t> out;
    if (layer == 0) return out;
    for (const auto& [name, s] : s_->spawners) {
        const int floor = name.starts_with("Chins_Floor5") ? 5 : 6;
        if (s_->LayerOf(floor) != layer) continue;
        out.insert(out.end(), s.portals.begin(), s.portals.end());
    }
    return out;
}
int QinTomb::Floor5Order() const { return s_->order; }

} // namespace sr::server
