#include "sr/server/Temples.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <string>
#include <tuple>

#include "sr/core/Log.h"
#include "sr/game/InstanceProtocol.h"
#include "sr/game/Jobs.h"
#include "sr/server/Instances.h"
#include "sr/server/World.h"

namespace sr::server {

namespace proto = game::proto;

namespace {

// The temples' pairs by their schedule (`_RefScheduleDefine` 9..11).
struct Pair {
    int define;
    const char* first;
    const char* second; // none: Haroeris's alone (Seth's opens on his death)
};
constexpr std::array<Pair, 3> kPairs{{
    {9, "INS_TEMPLE_SELK", "INS_TEMPLE_NEIT"},
    {10, "INS_TEMPLE_ANUB", "INS_TEMPLE_ISIS"},
    {11, "INS_TEMPLE_HARO", nullptr},
}};
// The countdown from the period's start (SM): warnings 30, 10 and 5 minutes before, the opening 1800 s in.
constexpr std::array<int, 3> kWarnAt{0, 1200, 1500};
constexpr std::array<std::uint8_t, 3> kWarnMinutes{30, 10, 5};
constexpr int kOpenAt = 1800;
// The doors (teleportbuilding: alex_raid_portal.bsr) and Haroeris.
constexpr std::uint32_t kTempleDoor = 27760, kSethDoor = 27759, kHaroeris = 26736;
constexpr proto::WorldPosition kHaroerisAt{0x800E, 9919.0f, 100.0f, 24.0f};
// Where the reputations are kept (in the original's order).
constexpr int kAnubisTraderHunter = 0, kAnubisThief = 1, kHaroerisTraderHunter = 2, kHaroerisThief = 3;

float Number(const std::map<std::string, std::string>& cfg, const char* key, float otherwise = 0.0f) {
    const auto it = cfg.find(key);
    return it != cfg.end() ? float(std::atof(it->second.c_str())) : otherwise;
}
proto::WorldPosition Place(const std::map<std::string, std::string>& cfg, const std::string& prefix) {
    return {std::uint16_t(int(Number(cfg, (prefix + "REGION").c_str()))), Number(cfg, (prefix + "X").c_str()), Number(cfg, (prefix + "Y").c_str()),
            Number(cfg, (prefix + "Z").c_str())};
}
bool TraderOrHunter(std::uint8_t job) { return job == 1 || job == 3; }

} // namespace

struct Temples::State {
    TempleHost& host;
    Instances& instances;
    Monsters& monsters;
    World& world;
    std::shared_ptr<const GameWorldCatalog> catalog;
    TempleConfig config;
    std::array<int, 4> reputation{};

    struct Temple {
        const GameWorldSpec* spec = nullptr;
        proto::WorldPosition door, inside, leave, back;
        std::uint32_t layer = 0;
        std::uint32_t doorEntity = 0;
        std::uint8_t allowed = proto::TempleNotice::kEveryone;
        bool equal = false;
        std::size_t haroeris = SIZE_MAX; // Haroeris's temple: his nest
        std::int64_t decided = 0;        // when its reputation was last decided (Unix seconds)
    };
    std::map<std::string, Temple> temples; // by codename
    struct Period {
        std::int64_t start = 0, end = 0;
        std::size_t warned = 0;
        bool opened = false, closed = false;
    };
    std::array<Period, 3> periods{};
    std::map<std::uint32_t, std::string> doors; // door entity -> temple

    State(TempleHost& h, Instances& i, Monsters& m, World& w, std::shared_ptr<const GameWorldCatalog> c, TempleConfig cfg, std::array<int, 4> rep)
        : host(h), instances(i), monsters(m), world(w), catalog(std::move(c)), config(std::move(cfg)), reputation(rep) {
        for (const GameWorldSpec& spec : catalog->temples) {
            Temple t;
            t.spec = &spec;
            t.door = Place(spec.config, "ENTER_GATE_");
            t.inside = Place(spec.config, "ENTER_GATE_TARGAET_");
            t.leave = Place(spec.config, "LEAVE_GATE_");
            t.back = Place(spec.config, "LEAVE_GATE_TARGAET_");
            temples[spec.codename] = t;
        }
    }

    int Rule(const char* key, int otherwise) const {
        const auto it = catalog->templeArea.find(key);
        return it != catalog->templeArea.end() ? std::atoi(it->second.c_str()) : otherwise;
    }

    template <typename T>
    void Tell(const T& message) {
        host.TempleTell(T::kId, proto::Encode(message));
    }

    // The alliance's share once 30 are inside: 0x15.
    bool RatioRefuses(std::uint8_t job) {
        if (job == 4) return false;
        const auto [traderHunter, thief] = host.TempleAllianceCounts();
        const int total = traderHunter + thief;
        if (total < Rule("ENTER_LIMIT_CONDITION_TEMPLE_ASSOCIATION_MINIMUM_NUM", 30)) return false;
        if (TraderOrHunter(job)) return traderHunter * 100 / total >= Rule("ENTER_LIMIT_CONDITION_TEMPLE_ASSOCIATION_RATE_MERCHANT_HUNTER", 70);
        return thief * 100 / total >= Rule("ENTER_LIMIT_CONDITION_TEMPLE_ASSOCIATION_RATE_THIEF", 70);
    }

    // The checks of the area and of a temple, in the order of the world's admission: the
    // level (0x14), the job suit (0x1B), the ride (0x18 / 0x19), free battle
    // (0x1C), then the alliances' share (0x15); their codes' texts.
    proto::GateStatus Refusal(std::uint32_t entity, bool levelChecked) {
        const auto f = host.TempleFacts(entity);
        if (!f) return proto::GateStatus::Nowhere;
        if (!levelChecked && f->level < Rule("ENTER_LIMIT_CONDITION_LEVEL_MINIMUM_NUM", 105)) return proto::GateStatus::ShrineLevel;
        if (f->jobState == 4) return proto::GateStatus::ShrineJobSuit;
        if (f->riding) return proto::GateStatus::Riding;
        if (f->freePvp) return proto::GateStatus::JobSuit; // 0x1C -> 0x1C2B
        if (RatioRefuses(f->jobState)) return proto::GateStatus::ShrineRatio;
        return proto::GateStatus::Ok;
    }

    // Who may enter Anubis's or Haroeris's (SM), their reputations then back to 0 and written.
    void Decide(Temple& t) {
        t.allowed = proto::TempleNotice::kEveryone;
        t.equal = false;
        const bool anubis = t.spec->codename == "INS_TEMPLE_ANUB", haroeris = t.spec->codename == "INS_TEMPLE_HARO";
        if (!anubis && !haroeris) return;
        const int th = anubis ? kAnubisTraderHunter : kHaroerisTraderHunter, thief = anubis ? kAnubisThief : kHaroerisThief;
        if (reputation[th] > reputation[thief]) {
            t.allowed = proto::TempleNotice::kTraderHunter;
        } else if (reputation[th] < reputation[thief]) {
            t.allowed = proto::TempleNotice::kThief;
        } else {
            t.equal = true;
            t.allowed = anubis ? proto::TempleNotice::kEveryone : proto::TempleNotice::kNobody;
        }
        core::LogInfo("temples: {}: reputations {} / {}: {} may enter", t.spec->codename, reputation[th], reputation[thief], int(t.allowed));
        t.decided = std::int64_t(std::time(nullptr));
        reputation[th] = reputation[thief] = 0;
        host.TempleSaveReputation(reputation);
    }

    void OpenTemple(Temple& t) {
        if (t.layer != 0) return;
        Decide(t);
        if (t.allowed == proto::TempleNotice::kNobody) {
            // Nobody may enter (Haroeris's on equal reputations): the original's opening stops at its decision's switch
            // (case 0): no instance, no door; the notice says why.
            Tell(proto::TempleNotice{proto::TempleNotice::kOpened, t.inside.region, t.allowed, t.equal, 0});
            core::LogInfo("temples: {} stays shut: nobody may enter", t.spec->codename);
            return;
        }
        t.layer = instances.OpenWorld(*t.spec, "the temples' schedule");
        if (t.layer == 0) {
            core::LogWarning("temples: no instance for {}", t.spec->codename);
            return;
        }
        instances.AddExit(t.layer, kTempleDoor, t.leave);
        if (t.spec->codename != "INS_TEMPLE_SETH") {
            t.doorEntity = world.AddNpc(kTempleDoor, t.door, 0.0f);
            doors[t.doorEntity] = t.spec->codename;
        }
        if (t.spec->codename == "INS_TEMPLE_HARO") {
            MonsterNest n;
            n.refObjId = kHaroeris;
            n.position = kHaroerisAt;
            n.position.layer = t.layer;
            n.maxCount = 1;
            n.respawn = false;
            n.radius = 500.0f;
            n.aggressive = true;
            n.sight = 300.0f;
            t.haroeris = monsters.AddNest(n);
        }
        Tell(proto::TempleNotice{proto::TempleNotice::kOpened, t.inside.region, t.allowed, t.equal, 0});
        core::LogInfo("temples: {} open (layer {})", t.spec->codename, t.layer);
    }

    void CloseTemple(Temple& t) {
        if (t.layer == 0) return;
        if (t.doorEntity != 0) {
            world.Leave(t.doorEntity);
            doors.erase(t.doorEntity);
            t.doorEntity = 0;
        }
        if (t.haroeris != SIZE_MAX) monsters.RemoveNest(t.haroeris);
        t.haroeris = SIZE_MAX;
        instances.CloseWorld(t.layer);
        t.layer = 0;
        Tell(proto::TempleNotice{proto::TempleNotice::kClosed, t.inside.region, 0, false, 0});
        core::LogInfo("temples: {} closed", t.spec->codename);
    }

    std::vector<Temple*> PairOf(int define) {
        std::vector<Temple*> out;
        for (const Pair& p : kPairs) {
            if (p.define != define) continue;
            for (const char* name : {p.first, p.second}) {
                if (name == nullptr) continue;
                if (auto it = temples.find(name); it != temples.end()) out.push_back(&it->second);
            }
            if (define == 11) { // Seth's goes with Haroeris's
                if (auto it = temples.find("INS_TEMPLE_SETH"); it != temples.end()) out.push_back(&it->second);
            }
        }
        return out;
    }

    void OpenPair(int define) {
        for (Temple* t : PairOf(define)) {
            if (t->spec->codename != "INS_TEMPLE_SETH") OpenTemple(*t);
        }
    }
    void ClosePair(int define) {
        for (Temple* t : PairOf(define)) CloseTemple(*t);
    }

    std::optional<game::Period> PeriodOf(int define, std::int64_t unix) const {
        if (config.cyclePeriod > 0) {
            const std::int64_t base = unix - unix % config.cyclePeriod;
            return game::Period{base, base + config.cyclePeriod};
        }
        return game::SchedulePeriod(config.schedule, define, unix);
    }

    void TickSchedule() {
        const std::int64_t unix = std::int64_t(std::time(nullptr));
        for (std::size_t k = 0; k < kPairs.size(); ++k) {
            const int define = kPairs[k].define;
            Period& run = periods[k];
            const auto p = PeriodOf(define, unix);
            if (p && p->Contains(unix) && p->start != run.start) {
                if (!run.closed && run.opened) ClosePair(define);
                run = {p->start, p->end, 0, false, false};
            }
            if (run.start == 0 || run.closed) continue;
            // In a cycle, the opening a quarter in and the warnings scaled; the schedule's are the original's.
            const double scale = config.cyclePeriod > 0 ? double(config.cyclePeriod) / 4.0 / double(kOpenAt) : 1.0;
            const std::int64_t openAt = run.start + std::int64_t(double(kOpenAt) * scale);
            while (!run.opened && run.warned < kWarnAt.size() && unix >= run.start + std::int64_t(double(kWarnAt[run.warned]) * scale) && unix < openAt) {
                for (Temple* t : PairOf(define)) {
                    if (t->spec->codename == "INS_TEMPLE_SETH") continue;
                    Tell(proto::TempleNotice{proto::TempleNotice::kBefore, t->inside.region, 0, false, kWarnMinutes[run.warned]});
                }
                ++run.warned;
            }
            if (!run.opened && unix >= openAt) {
                run.opened = true;
                OpenPair(define);
            }
            if (run.opened && unix >= run.end) {
                run.closed = true;
                ClosePair(define);
            }
        }
    }
};

Temples::Temples(TempleHost& host, Instances& instances, Monsters& monsters, World& world, std::shared_ptr<const GameWorldCatalog> catalog,
                 TempleConfig config, std::array<int, 4> reputation)
    : s_(std::make_unique<State>(host, instances, monsters, world, std::move(catalog), std::move(config), reputation)) {}

Temples::~Temples() = default;

void Temples::Tick(Clock::time_point) {
    State& s = *s_;
    if (s.config.enabled) s.TickSchedule();
    // An instance ended by its own time: its door down.
    for (auto& [name, t] : s.temples) {
        if (t.layer != 0 && !s.instances.Stands(t.layer)) {
            if (t.doorEntity != 0) {
                s.world.Leave(t.doorEntity);
                s.doors.erase(t.doorEntity);
                t.doorEntity = 0;
            }
            t.layer = 0;
        }
    }
}

proto::GateStatus Temples::AreaRefusal(std::uint32_t entity, std::uint32_t gate) {
    const auto f = s_->host.TempleFacts(entity);
    if (!f) return proto::GateStatus::Nowhere;
    const bool fits = gate == kAreaGateThief ? f->jobState == 2 : TraderOrHunter(f->jobState);
    if (!fits) return proto::GateStatus::ShrineNoRight;
    return s_->Refusal(entity, true);
}

std::optional<proto::WorldPosition> Temples::RefusedPlace(std::uint32_t entity) {
    State& s = *s_;
    const auto& area = s.catalog->templeArea;
    if (const auto on = area.find("FAILED_ENTER_WORLD_TO_SPECIAL_TARGET_SWITCH"); on == area.end() || on->second != "ON") return std::nullopt;
    if (s.Refusal(entity, true) == proto::GateStatus::Ok) return std::nullopt;
    const auto f = s.host.TempleFacts(entity);
    if (!f || f->league == 0) return std::nullopt;
    const char* who = f->league == 2 ? "THIEF" : "MERCHANT_HUNTER";
    const auto value = [&](const char* what) {
        const auto it = area.find(std::string("FAILED_ENTER_WORLD_TO_SPECIAL_TARGET_") + what + "_" + who);
        return it != area.end() ? float(std::atof(it->second.c_str())) : 0.0f;
    };
    const proto::WorldPosition to{std::uint16_t(int(value("REGION"))), value("POSX"), value("POSY"), value("POSZ")};
    if (to.region == 0) return std::nullopt;
    return to;
}

bool Temples::IsDoor(std::uint32_t entity) const { return s_->doors.contains(entity); }

proto::GateStatus Temples::UseDoor(std::uint32_t entity, std::uint32_t door) {
    State& s = *s_;
    const auto d = s.doors.find(door);
    if (d == s.doors.end()) return proto::GateStatus::NoSuchGate;
    State::Temple& t = s.temples.at(d->second);
    const auto me = s.world.Where(entity), there = s.world.Where(door);
    if (!me || !there) return proto::GateStatus::Nowhere;
    const proto::FlatPosition a = proto::ToFlat(me->position), b = proto::ToFlat(there->position);
    if (std::hypot(a.x - b.x, a.z - b.z) > proto::kGapReach) return proto::GateStatus::TooFar;
    if (t.layer == 0) return proto::GateStatus::ShrineNoRight;
    // The decision travels with the door: the opening writes it as the door's use condition (5 the traders
    // and hunters, 3 the thieves, 1 everyone), tested at its use by the league joined (the link's condition
    // kind 5 the trader or the hunter league, 3 the thief league, 1 anyone not excluded), before the world's
    // admission (Refusal); failed: 0x1C2F "no right".
    const auto f = s.host.TempleFacts(entity);
    const bool th = f && (f->league == 1 || f->league == 3), thief = f && f->league == 2;
    if ((t.allowed == proto::TempleNotice::kTraderHunter && !th) || (t.allowed == proto::TempleNotice::kThief && !thief)) {
        return proto::GateStatus::ShrineNoRight;
    }
    if (const proto::GateStatus status = s.Refusal(entity, false); status != proto::GateStatus::Ok) return status;
    if (s.instances.Inside(t.layer) >= t.spec->maxUsers) return proto::GateStatus::InstanceFull;
    proto::WorldPosition inside = t.inside;
    inside.layer = t.layer;
    const bool ok = s.instances.LayerOf(entity) != 0 ? s.instances.Transfer(entity, t.layer, inside) : s.instances.AdmitAt(entity, t.layer, inside);
    if (!ok) return proto::GateStatus::Nowhere;
    core::LogInfo("temples: entity {} into {}", entity, t.spec->codename);
    return proto::GateStatus::Ok;
}

std::optional<proto::GapInfo> Temples::DoorInfo(std::uint32_t door) const {
    const State& s = *s_;
    const auto d = s.doors.find(door);
    if (d == s.doors.end()) return std::nullopt;
    const State::Temple& t = s.temples.at(d->second);
    proto::GapInfo info;
    info.entity = door;
    info.world = t.spec->id;
    info.temple = true;
    // only Seth's door says its temple's players (its instance's) and its world's WorldMaxUserCount.
    if (t.spec->codename == "INS_TEMPLE_SETH") {
        info.inside = std::uint8_t(std::min<std::size_t>(255, t.layer != 0 ? s.instances.Inside(t.layer) : 0));
        info.capacity = std::uint8_t(std::min<std::uint32_t>(255, t.spec->maxUsers));
    }
    return info;
}

void Temples::Killed(const Monsters::Damaged& killed, const proto::WorldPosition& at) {
    State& s = *s_;
    if (killed.refObjId != kHaroeris) return;
    auto haro = s.temples.find("INS_TEMPLE_HARO");
    auto seth = s.temples.find("INS_TEMPLE_SETH");
    if (haro == s.temples.end() || seth == s.temples.end() || haro->second.layer == 0 || at.layer != haro->second.layer) return;
    // his wave's trigger tells the shard manager, which opens Seth's (SM); its door inside Haroeris's.
    State::Temple& t = seth->second;
    if (t.layer != 0) return;
    s.OpenTemple(t);
    if (t.layer == 0) return;
    proto::WorldPosition door = t.door;
    door.layer = haro->second.layer;
    t.doorEntity = s.world.AddNpc(kSethDoor, door, 0.0f);
    s.doors[t.doorEntity] = t.spec->codename;
    core::LogInfo("temples: Haroeris killed: Seth's temple open");
}

void Temples::AddReputation(int temple, int alliance, int points) {
    State& s = *s_;
    if ((temple != 0 && temple != 1) || (alliance != 1 && alliance != 2)) return;
    const int index = temple == 0 ? (alliance == 1 ? kAnubisTraderHunter : kAnubisThief) : (alliance == 1 ? kHaroerisTraderHunter : kHaroerisThief);
    // Never below 0 (a developer's removal; the shard manager's save skips a negative value, SM).
    s.reputation[std::size_t(index)] = std::max(0, s.reputation[std::size_t(index)] + points);
    s.host.TempleSaveReputation(s.reputation);
}

bool Temples::Open(int define) {
    State& s = *s_;
    for (std::size_t k = 0; k < kPairs.size(); ++k) {
        if (kPairs[k].define != define) continue;
        const std::int64_t unix = std::int64_t(std::time(nullptr));
        s.periods[k] = {unix - kOpenAt, unix + 5400, kWarnAt.size(), true, false};
        s.OpenPair(define);
        return true;
    }
    return false;
}

void Temples::Close(int define) {
    State& s = *s_;
    for (std::size_t k = 0; k < kPairs.size(); ++k) {
        if (kPairs[k].define != define) continue;
        s.periods[k].closed = true;
        s.ClosePair(define);
    }
}

std::vector<proto::TempleReputationRow> Temples::Board() const {
    const State& s = *s_;
    const std::int64_t unix = std::int64_t(std::time(nullptr));
    std::vector<proto::TempleReputationRow> rows;
    for (const auto& [define, name, th, thief] : {std::tuple{10, "INS_TEMPLE_ANUB", kAnubisTraderHunter, kAnubisThief},
                                                  std::tuple{11, "INS_TEMPLE_HARO", kHaroerisTraderHunter, kHaroerisThief}}) {
        proto::TempleReputationRow row;
        if (const auto it = s.temples.find(name); it != s.temples.end()) {
            row.allowed = it->second.allowed;
            row.since = it->second.decided;
        }
        // The next opening: this period's if it is still to come, else the next period's (30 minutes into it).
        const double scale = s.config.cyclePeriod > 0 ? double(s.config.cyclePeriod) / 4.0 / double(kOpenAt) : 1.0;
        for (std::int64_t t = unix; t < unix + 8 * 86400 && row.nextOpening == 0;) {
            const auto p = s.PeriodOf(define, t);
            if (!p) break;
            const std::int64_t open = p->start + std::int64_t(double(kOpenAt) * scale);
            if (open > unix) row.nextOpening = open;
            t = std::max(p->end, t + 1);
        }
        row.traderHunter = std::uint32_t(std::max(0, s.reputation[std::size_t(th)]));
        row.thief = std::uint32_t(std::max(0, s.reputation[std::size_t(thief)]));
        rows.push_back(row);
    }
    return rows;
}

proto::TemplePoints Temples::PointsOf(const std::vector<proto::TempleReputationRow>& board) {
    proto::TemplePoints p;
    if (board.size() >= 1) {
        p.anubisTraderHunter = board[0].traderHunter;
        p.anubisThief = board[0].thief;
    }
    if (board.size() >= 2) {
        p.haroerisTraderHunter = board[1].traderHunter;
        p.haroerisThief = board[1].thief;
    }
    return p;
}

bool Temples::OffersBoard(const std::string& codename) {
    return codename == "NPC_SD_M_AREA_MERCHANT" || codename == "NPC_SD_M_AREA_HUNTER" || codename == "NPC_SD_T_AREA_THIEF";
}

std::array<int, 4> Temples::Reputation() const { return s_->reputation; }

std::uint32_t Temples::LayerOf(std::uint32_t worldId) const {
    for (const auto& [name, t] : s_->temples) {
        if (t.spec->id == worldId) return t.layer;
    }
    return 0;
}

std::uint32_t Temples::DoorOf(std::uint32_t worldId) const {
    for (const auto& [name, t] : s_->temples) {
        if (t.spec->id == worldId) return t.doorEntity;
    }
    return 0;
}

std::uint8_t Temples::AllowedOf(std::uint32_t worldId) const {
    for (const auto& [name, t] : s_->temples) {
        if (t.spec->id == worldId) return t.allowed;
    }
    return proto::TempleNotice::kNobody;
}

} // namespace sr::server
