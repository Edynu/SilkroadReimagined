#include "sr/game/Guild.h"
#include "sr/server/Fortress.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <random>
#include <map>
#include <set>
#include <unordered_map>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/assets/Terrain.h"
#include "sr/core/Log.h"
#include "sr/game/SpecialItems.h"
#include "sr/server/Database.h"
#include "sr/server/GuildStore.h"
#include "sr/server/ShardBus.h"

namespace sr::server {

namespace proto = game::proto;
using game::FortressError;
using game::StructureKind;

namespace {

// A structure's State (`_SiegeFortressStruct.State`): 0 standing, 2 destroyed (ours: the battle aide's
// three states "Normal", "Repair required", "Destroyed" read as 0, 1, 2; inferred).
constexpr int kStructStanding = 0, kStructDestroyed = 2;
// Ours, for the zones built on by an item (barricades, war camps): empty, and built (an item's structure stands there).
constexpr int kStructEmpty = 4, kStructBuilt = 5;
bool BuiltByItem(StructureKind k) { return k == StructureKind::Barricade || k == StructureKind::Headquarters; }

// The structures that stand in the world from the start (ours: the defensive positions and war camps
// are foundations the owner builds on, the barricades come from their items; not built yet).
bool StandsFromTheStart(StructureKind k) { return k == StructureKind::Heart || k == StructureKind::GuardTower || k == StructureKind::Gate; }

template <typename T>
std::vector<std::byte> Bytes(const T& message) {
    return proto::Encode(message);
}

// --- The shard's bus (channel sr_fortress_<shard>) ----------------------------------
// What the original's shard manager relays between its game servers, in our words: the periods from the server that
// keeps them, each fortress's row from the server that runs it, the applications from where they were made, the notices
// every player hears, a guild's messages to its members elsewhere.
enum : std::uint8_t {
    kBusHello = 1,   // a server has started: the keeper tells it the periods, each runner its fortresses' rows
    kBusPeriods = 2, // the periods now (the original's 0x33 / 0x34, 2, 6, 0x31 / 0x32), from the keeper
    kBusNotice = 3,  // a notice for every player (the warnings, the captures: the original's 1, 3-5, 9, 8)
    kBusRow = 4,     // a fortress's owner, holder, rate and war (the original's 7, 8), from its runner
    kBusRequest = 5, // an application made or taken back (the original's 0x0C, 0x0D)
    kBusGuild = 6,   // a message to a guild's members and its union's on every server
    kBusDev = 7,     // a developer's period switch, to the keeper
};

struct BusEnvelope {
    std::uint16_t from = 0;
    std::uint8_t kind = 0;
    std::vector<std::uint8_t> payload;
    template <typename V>
    void Visit(V& v) {
        v("from", from);
        v("kind", kind);
        v("payload", payload, 3900);
    }
};
struct PeriodsWire {
    bool requests = false, war = false, tax = false;
    std::int64_t requestStart = 0, requestEnd = 0, warStart = 0, warEnd = 0;
    template <typename V>
    void Visit(V& v) {
        v("requests", requests);
        v("war", war);
        v("tax", tax);
        v("requestStart", requestStart);
        v("requestEnd", requestEnd);
        v("warStart", warStart);
        v("warEnd", warEnd);
    }
};
struct RowWire {
    std::uint32_t fortress = 0, guild = 0, tempGuild = 0;
    std::int32_t rate = 0;
    bool atWar = false;
    std::uint8_t hired = 0;   // NPCHired (the original's kind 0x12)
    std::string introduction; // the original's kind 0x1A
    template <typename V>
    void Visit(V& v) {
        v("fortress", fortress);
        v("guild", guild);
        v("tempGuild", tempGuild);
        v("rate", rate);
        v("atWar", atWar);
        v("hired", hired);
        v("introduction", introduction, 255);
    }
};
struct RequestWire {
    std::uint32_t fortress = 0, guild = 0;
    std::uint8_t type = 0;
    bool add = false;
    template <typename V>
    void Visit(V& v) {
        v("fortress", fortress);
        v("guild", guild);
        v("type", type);
        v("add", add);
    }
};
struct GuildWire {
    std::uint32_t guild = 0;
    std::uint16_t id = 0;
    std::vector<std::uint8_t> payload;
    template <typename V>
    void Visit(V& v) {
        v("guild", guild);
        v("id", id);
        v("payload", payload, 3000);
    }
};
struct DevWire {
    std::uint8_t action = 0;
    template <typename V>
    void Visit(V& v) {
        v("action", action);
    }
};
template <typename T>
std::optional<T> BusDecode(const std::vector<std::uint8_t>& payload) {
    T out;
    if (!proto::Decode(std::as_bytes(std::span(payload)), out)) return std::nullopt;
    return out;
}
std::vector<std::uint8_t> AsU8(const std::vector<std::byte>& b) {
    std::vector<std::uint8_t> out(b.size());
    if (!b.empty()) std::memcpy(out.data(), b.data(), b.size());
    return out;
}
std::vector<std::byte> AsBytes(const std::vector<std::uint8_t>& b) {
    std::vector<std::byte> out(b.size());
    if (!b.empty()) std::memcpy(out.data(), b.data(), b.size());
    return out;
}
// The notices every server tells its own players when the server that made them says so (each server makes its own
// period notices from the keeper's periods).
bool ShardWideNotice(proto::FortressNoticeKind k) {
    using K = proto::FortressNoticeKind;
    return k == K::WarSoon || k == K::WarEnd30 || k == K::WarEnd20 || k == K::WarEnd10 || k == K::WarEnd1 || k == K::Conquer || k == K::TaxRate;
}

} // namespace

struct Fortresses::State {
    FortressHost& host;
    FortressConfig config;
    State(FortressHost& h, FortressConfig c) : host(h), config(std::move(c)) {}

    struct Structure {
        std::size_t spot = 0;       // its index in the spec's structures
        std::uint32_t refObjId = 0;
        std::uint32_t owner = 0;
        std::int64_t hp = 0;
        int state = kStructStanding;
        std::int64_t madeAt = 0;    // when it was built or last upgraded (Unix seconds; MakeDate)
        std::uint32_t entity = 0;   // standing in the world (0: not)
        bool dirty = false;         // its health to write
        bool open = false;          // a gate opened by its pulley (the state word's bit 1)
        bool paying = false;        // a repair or an upgrade paid for, its transaction under way
        Clock::time_point nextSkill{}; // its own skill's next time (the war's; FortressStructureSkill)
    };
    struct Fortress {
        FortressSpec spec;
        FortressRow row;
        std::vector<FortressRequestRow> requests;
        std::vector<Structure> structures;
        std::map<std::uint32_t, std::uint64_t> stone; // guild -> the heart's damage (the original's map, by guild id)
        std::set<std::uint32_t> stoneDirty;
        bool atWar = false;
        std::optional<Clock::time_point> heartOpenAt; // the seal lifts then (none: sealed, or open)
        std::optional<Clock::time_point> evacuateAt;  // those who may not be here go then (5 s into the war)
        std::optional<Clock::time_point> openAgainAt; // a capture's five minutes: its attackers enter again then
        bool heartOpenTold = false;
        struct Record {
            int kills = 0, deaths = 0, rank = 0;
        };
        std::map<std::int64_t, Record> records; // character -> this war's kills, deaths and battle rank
        // The war's objects standing for a guild at a point of their own (flags, guards, summoned monsters; `_SiegeFortressObject`),
        // by entity.
        struct Object {
            std::shared_ptr<std::int64_t> row = std::make_shared<std::int64_t>(0); // its ID once written
            std::uint32_t refObjId = 0, owner = 0;
            StructureKind kind = StructureKind::None;
            std::int64_t hp = 0;
            std::uint32_t maxHp = 0;
            proto::WorldPosition at;
            float yaw = 0;
            int ownerLevel = 0;
            std::string nameKey;
            std::optional<Clock::time_point> until; // gone then (flags: 120 s)
            Clock::time_point nextPulse{};          // a flag's aura's next pulse
            int pulseMs = 0;
            bool dirty = false; // its health to write
        };
        std::map<std::uint32_t, Object> objects;
        // The smith's and the trainer's work (`_SiegeFortressItemForge`, one a side).
        std::vector<FortressForgeRow> forges;
        // Its Cave of Discipline open (the instance's layer; 0: none).
        std::uint32_t dungeonLayer = 0;
        std::uint32_t Controller() const { return row.tempGuild != 0 ? row.tempGuild : row.guild; }
        const FortressSpec::Structure& Spot(const Structure& s) const { return spec.structures[s.spot]; }
        bool InArea(std::uint16_t region) const { return std::find(spec.regions.begin(), spec.regions.end(), region) != spec.regions.end(); }
    };
    // A guild of the holder's side (the holder's guild, or one of its union; none while nobody holds the fortress).
    bool HolderSide(const Fortress& f, const std::optional<FortressGuild>& g) const {
        const std::uint32_t holder = f.Controller();
        if (!g || holder == 0) return false;
        if (g->id == holder) return true;
        const std::uint32_t holderUnion = AllianceOf(holder);
        return holderUnion != 0 && g->alliance == holderUnion;
    }
    std::vector<Fortress> forts;
    std::minstd_rand random{std::random_device{}()};
    std::unordered_map<std::uint32_t, std::pair<std::size_t, std::size_t>> byEntity; // entity -> fortress, structure
    std::unordered_map<std::uint32_t, std::size_t> objectFort;                       // a war object's entity -> its fortress
    // A repair kit's pulses on a structure (ours: its skill's 'puls' and 'dura' from the use).
    struct KitRepair {
        std::uint32_t structure = 0;
        std::uint32_t amount = 0; // health each pulse
        int pulsesLeft = 0;
        int pulseMs = 0;
        Clock::time_point next{};
    };
    std::vector<KitRepair> kitRepairs;
    bool requestsOpen = false, warOn = false;
    bool manual = false;       // the periods follow Dev() alone
    std::optional<bool> taxForced; // the tax period switched by hand (/SIEGE STARTTAX / ENDTAX), else the schedule's
    bool fromBus = false;            // applying another server's word: told to this server's players only
    std::optional<PeriodsWire> told; // the keeper's periods (on a server that keeps none)
    Clock::time_point nextPeriods{}; // the keeper's next word on the periods to the others
    std::optional<bool> taxSaid;     // the tax period as last told to this server's players
    // The tax period's notices (the original's 0x31 / 0x32: the client's state bit, no line) when it changed.
    void TellTax() {
        const bool tax = TaxPeriod();
        if (taxSaid && *taxSaid == tax) return;
        const bool first = !taxSaid;
        taxSaid = tax;
        if (first) return; // the list on entering has it
        Notice(tax ? proto::FortressNoticeKind::TaxOpen : proto::FortressNoticeKind::TaxClosed);
        BroadcastList();
        PublishPeriods();
    }
    std::int64_t cycleStart = 0;
    bool restored = false; // the start's unfinished wars looked at (Tick)
    Clock::time_point now{};
    std::int64_t unixNow = 0;
    Clock::time_point nextFlush{};
    std::int64_t warnedFor = 0, warnedEndFor = 0; // the war start / end the warnings were for
    std::set<int> warned, warnedEnd;

    // Writes, one at a time in the order made.
    std::deque<std::function<void(Database&)>> writes;
    bool writing = false;
    void Write(std::function<void(Database&)> task) {
        writes.push_back(std::move(task));
        Pump();
    }
    void Pump() {
        if (writing || writes.empty()) return;
        writing = true;
        auto task = std::move(writes.front());
        writes.pop_front();
        host.FortressSubmit(std::move(task), [this] {
            writing = false;
            Pump();
        });
    }

    Fortress* Find(std::uint32_t id) {
        for (Fortress& f : forts) {
            if (f.spec.id == id) return &f;
        }
        return nullptr;
    }
    const Fortress* Find(std::uint32_t id) const { return const_cast<State*>(this)->Find(id); }

    std::string GuildName(std::uint32_t guild) const {
        if (guild == 0) return {};
        const std::optional<FortressGuild> g = host.FortressGuildById(guild);
        return g ? g->name : std::string();
    }
    std::uint32_t AllianceOf(std::uint32_t guild) const {
        if (guild == 0) return 0;
        const std::optional<FortressGuild> g = host.FortressGuildById(guild);
        return g ? g->alliance : 0;
    }

    // --- Who takes part ---------------------------------------------------------------------------

    // The guilds of a fortress's war: its owner, the guild holding it, and those applied.
    bool Participant(const Fortress& f, std::uint32_t guild) const {
        if (guild == 0) return false;
        if (guild == f.row.guild || guild == f.row.tempGuild) return true;
        return std::any_of(f.requests.begin(), f.requests.end(), [&](const FortressRequestRow& r) { return r.guild == guild; });
    }
    const FortressRequestRow* RequestOf(std::uint32_t guild) const {
        for (const Fortress& f : forts) {
            for (const FortressRequestRow& r : f.requests) {
                if (r.guild == guild) return &r;
            }
        }
        return nullptr;
    }

    // --- Messages ---------------------------------------------------------------------------------

    proto::FortressList List() const {
        proto::FortressList list;
        list.requestsOpen = requestsOpen;
        list.warOn = warOn;
        list.taxPeriod = TaxPeriod();
        for (const Fortress& f : forts) {
            proto::FortressRecord r;
            r.id = f.spec.id;
            r.nameKey = f.spec.nameKey;
            r.owner = f.row.guild;
            r.ownerName = GuildName(f.row.guild);
            r.holder = f.row.tempGuild;
            r.holderName = GuildName(f.row.tempGuild);
            r.taxRate = std::uint16_t(std::clamp(f.row.taxRatio, 0, 0xFFFF));
            r.atWar = f.atWar;
            r.regions = f.spec.regions;
            if (const std::optional<FortressGuild> g = f.Controller() != 0 ? host.FortressGuildById(f.Controller()) : std::nullopt) {
                r.masterName = g->masterName;
                r.crest = g->crest;
            }
            r.introduction = f.row.introduction;
            list.fortresses.push_back(std::move(r));
        }
        return list;
    }
    void BroadcastList() { host.FortressBroadcast(proto::FortressList::kId, Bytes(List())); }
    void Notice(proto::FortressNoticeKind kind, std::uint32_t fortress = 0, std::uint32_t value = 0, std::string guild = {}, std::string nameKey = {}) {
        const std::vector<std::byte> bytes = Bytes(proto::FortressNotice{kind, fortress, value, std::move(guild), std::move(nameKey)});
        host.FortressBroadcast(proto::FortressNotice::kId, bytes);
        if (!fromBus && ShardWideNotice(kind)) Publish(kBusNotice, bytes);
    }

    // --- The shard's bus --------------------------------------------------------------------------

    void Publish(std::uint8_t kind, const std::vector<std::byte>& payload) {
        if (!host.FortressOnBus()) return;
        BusEnvelope e;
        e.from = host.FortressServerId();
        e.kind = kind;
        e.payload = AsU8(payload);
        host.FortressPublish(BusToHex(proto::Encode(e)));
    }
    PeriodsWire PeriodsNow() const {
        if (!config.periodKeeper && told) return *told;
        PeriodsWire w;
        w.requests = requestsOpen;
        w.war = warOn;
        w.tax = TaxPeriod();
        const Periods p = PeriodsAt(unixNow);
        if (p.requestPeriod) {
            w.requestStart = p.requestPeriod->start;
            w.requestEnd = p.requestPeriod->end;
        }
        if (p.warPeriod) {
            w.warStart = p.warBegin;
            w.warEnd = p.warPeriod->end;
        }
        return w;
    }
    void PublishPeriods() {
        if (!config.periodKeeper || fromBus) return;
        Publish(kBusPeriods, Bytes(PeriodsNow()));
    }
    void PublishRow(const Fortress& f) {
        if (!f.spec.local) return;
        Publish(kBusRow, Bytes(RowWire{f.spec.id, f.row.guild, f.row.tempGuild, f.row.taxRatio, f.atWar, f.row.npcHired, f.row.introduction}));
    }
    // A message to a guild's members and its union's, here and on the shard's other servers.
    void SendGuild(std::uint32_t guild, proto::MessageId id, const std::vector<std::byte>& payload) {
        host.FortressSendGuild(guild, id, payload);
        Publish(kBusGuild, Bytes(GuildWire{guild, id, AsU8(payload)}));
    }

    // --- The structures ---------------------------------------------------------------------------

    const BattleRankRow* Rank(int rank) const {
        for (const BattleRankRow& r : config.battleRanks) {
            if (r.rank == rank) return &r;
        }
        return nullptr;
    }
    std::uint32_t MaxHp(const FortressSpec::Structure& spot, std::uint32_t refObjId = 0) const {
        const StructureReference* ref = refObjId != 0 ? Reference(refObjId) : nullptr;
        const std::uint64_t hp = config.structureHp != 0 ? config.structureHp : ref != nullptr && ref->maxHp > 0 ? ref->maxHp : spot.maxHp;
        return std::uint32_t(std::clamp<std::uint64_t>(hp, 1, 0x7FFFFFFF));
    }
    const StructureReference* Reference(std::uint32_t refObjId) const {
        for (const StructureReference& r : config.structureReferences) {
            if (r.refObjId == refObjId) return &r;
        }
        return nullptr;
    }
    const StructureReference* ReferenceNamed(const std::string& codename) const {
        for (const StructureReference& r : config.structureReferences) {
            if (r.codename == codename) return &r;
        }
        return nullptr;
    }
    void Stand(std::size_t fi, std::size_t si) {
        Fortress& f = forts[fi];
        Structure& s = f.structures[si];
        const bool destroyed = s.state == kStructDestroyed || s.hp <= 0;
        if (!f.spec.local || s.entity != 0) return;
        const StructureKind kind = f.Spot(s).kind;
        if (!StandsFromTheStart(kind) && !(BuiltByItem(kind) && s.state == kStructBuilt) && !(kind == StructureKind::Defensive && !Foundation(s))) return;
        const std::uint32_t maxHp = MaxHp(f.Spot(s), s.refObjId);
        s.hp = destroyed ? 0 : std::min<std::int64_t>(s.hp, maxHp);
        s.entity = host.SpawnStructure(f.Spot(s), s.refObjId, std::uint32_t(s.hp), maxHp, destroyed);
        if (s.entity == 0) return;
        byEntity[s.entity] = {fi, si};
        TellStructure(f, s);
    }
    // What those around are told of a structure: its kind, owner, size and state word (bit 0 destroyed, the rubble).
    void TellStructure(const Fortress& f, const Structure& s) {
        const StructureShape shape = host.StructureShapeOf(s.refObjId);
        const std::uint16_t state = std::uint16_t((s.state == kStructDestroyed ? 1 : 0) | (s.open ? 2 : 0));
        host.StructureAppearance(s.entity, proto::EntityStructure::kId,
                                 Bytes(proto::EntityStructure{s.entity, f.spec.id, std::uint8_t(f.Spot(s).kind), s.owner, MaxHp(f.Spot(s), s.refObjId), shape.pickRadius, shape.pickHeight, state}));
    }
    // Whether its place stands in the way of walks (`_RefEventZone.Param1`): a gate
    // while closed, a barricade while one stands, the others always, fallen or not.
    void BlockZone(const Fortress& f, const Structure& s) {
        if (!f.spec.local) return;
        const FortressSpec::Structure& spot = f.Spot(s);
        const bool blocks = spot.kind == StructureKind::Gate ? s.state == kStructStanding && !s.open : spot.kind == StructureKind::Barricade ? s.entity != 0 : true;
        host.FortressZoneBlocks(spot.navKey, spot.navName, blocks);
    }
    // The fortress's area emptied of those who may not be in it, to
    // its linked gate: as a war begins, all but the guild holding it; as one ends, all but its side (with its union).
    void Evacuate(const Fortress& f, bool warEnded) {
        const std::uint32_t holder = f.Controller();
        const std::uint32_t holderUnion = holder != 0 ? AllianceOf(holder) : 0;
        host.FortressEvacuate(f.spec.regions, f.spec.linkedTeleport, [this, holder, holderUnion, warEnded](std::int64_t character) {
            const std::optional<FortressGuild> g = host.FortressGuildOf(character);
            if (!g || holder == 0) return false;
            if (g->id == holder) return true;
            return warEnded && holderUnion != 0 && g->alliance == holderUnion;
        });
        core::LogInfo("fortress war: {} is cleared ({})", f.spec.codename, warEnded ? "the war is over" : "the war begins");
    }
    // A barricade's or a war camp's zone emptied (a barricade gone, a camp back to its foundation).
    void Clear(Fortress& f, Structure& s) {
        Fell(s);
        s.refObjId = f.Spot(s).refObjId;
        s.owner = 0;
        s.hp = 0;
        s.state = kStructEmpty;
        s.dirty = false;
        SaveStructure(f, s);
        BlockZone(f, s);
    }
    // --- The war's objects (flags, guards, summoned monsters; `_SiegeFortressObject`) ------------------------------

    // One stands in the world for its guild (a fixed monster; told as a structure of its kind); 0: it could not.
    std::uint32_t StandObject(std::size_t fi, Fortress::Object o) {
        Fortress& f = forts[fi];
        FortressSpec::Structure spot;
        spot.refObjId = o.refObjId;
        spot.kind = o.kind;
        spot.nameKey = o.nameKey;
        spot.maxHp = o.maxHp;
        spot.position = o.at;
        spot.yaw = o.yaw;
        // A guard or a tablet's monster moves and fights for its side; a flag stands as a structure.
        const bool fights = o.kind == StructureKind::Guard || o.kind == StructureKind::Summoned;
        const std::uint32_t entity = fights ? host.FortressSpawnWarMonster(o.refObjId, o.at, o.yaw, o.owner, AllianceOf(o.owner), std::uint32_t(std::max<std::int64_t>(0, o.hp)))
                                            : host.SpawnStructure(spot, o.refObjId, std::uint32_t(std::max<std::int64_t>(1, o.hp)), o.maxHp, false);
        if (entity == 0) return 0;
        objectFort[entity] = fi;
        const StructureShape shape = host.StructureShapeOf(o.refObjId);
        host.StructureAppearance(entity, proto::EntityStructure::kId,
                                 Bytes(proto::EntityStructure{entity, f.spec.id, std::uint8_t(o.kind), o.owner, o.maxHp, shape.pickRadius, shape.pickHeight, 0}));
        f.objects.emplace(entity, std::move(o));
        return entity;
    }
    // One gone (fallen, its time over, dismissed, the war's end): out of the world and its row deleted.
    void RemoveObject(std::uint32_t entity) {
        const auto fit = objectFort.find(entity);
        if (fit == objectFort.end()) return;
        Fortress& f = forts[fit->second];
        objectFort.erase(fit);
        const auto it = f.objects.find(entity);
        if (it == f.objects.end()) return;
        const std::shared_ptr<std::int64_t> row = it->second.row;
        f.objects.erase(it);
        host.RemoveStructure(entity);
        Write([row](Database& db) {
            if (*row != 0) DeleteFortressObject(db, *row);
        });
    }
    // Its row written (`_AddSiegeFortressObject`), its ID kept for the delete.
    void SaveNewObject(const Fortress& f, const Fortress::Object& o) {
        const FortressObjectRow r{0, f.spec.id, o.owner, o.refObjId, o.hp, o.at.region, o.at.x, o.at.y, o.at.z, o.yaw, o.ownerLevel};
        Write([r, row = o.row](Database& db) { *row = AddFortressObject(db, r); });
    }
    // The objects taken away (`_SiegeFortressFinished`: a fortress kept loses its flags and summoned monsters, a fortress
    // taken all of them; `_SiegeFortressTempFinished`: all of them).
    void RemoveObjects(Fortress& f, bool flagsAndSummonedOnly) {
        std::vector<std::uint32_t> gone;
        for (const auto& [entity, o] : f.objects) {
            if (!flagsAndSummonedOnly || o.kind == StructureKind::Flag || o.kind == StructureKind::Summoned) gone.push_back(entity);
        }
        for (const std::uint32_t e : gone) RemoveObject(e);
    }
    Fortress::Object* ObjectOf(std::uint32_t entity, Fortress** fort = nullptr) {
        const auto fit = objectFort.find(entity);
        if (fit == objectFort.end()) return nullptr;
        Fortress& f = forts[fit->second];
        const auto it = f.objects.find(entity);
        if (it == f.objects.end()) return nullptr;
        if (fort != nullptr) *fort = &f;
        return &it->second;
    }

    // The war's rewards (`_RefSiegeFortressRewards`, in the finish's answer): type 1 the item to the holder's master if
    // he is here, told 0x385F 0x13; type 5 `value` x each one's level's `GUST_Mob_Exp` as SP experience to the holder's players in
    // the fortress's area.
    void Reward(const Fortress& f) {
        const std::uint32_t holder = f.row.guild;
        if (holder == 0) return;
        for (const SiegeRewardRow& r : config.rewards) {
            if (r.fortress != f.spec.id) continue;
            if (r.type == 1) {
                const std::int64_t master = host.FortressMasterOf(holder);
                if (master != 0 && host.FortressGiveItemTo(master, r.value, std::max<std::uint32_t>(1, r.count))) {
                    host.FortressSendCharacter(master, proto::FortressNotice::kId, Bytes(proto::FortressNotice{proto::FortressNoticeKind::RewardMaster, f.spec.id}));
                    core::LogInfo("fortress war: {}'s reward {} to the master of guild {}", f.spec.codename, r.value, holder);
                }
            }
            if (r.type == 5) {
                const int n = host.FortressAreaSp(f.spec.regions, holder, r.value);
                core::LogInfo("fortress war: {}'s reward: {} x the level's experience as SP to {} of guild {}", f.spec.codename, r.value, n, holder);
            }
        }
    }
    // A war's object felled by a player: its siege quest (a flag 4, a guard 6).
    void ObjectFelled(std::uint32_t entity, std::int64_t killer) {
        const Fortress::Object* o = ObjectOf(entity);
        if (o == nullptr || killer == 0) return;
        if (o->kind == StructureKind::Flag) QuestCredit(killer, 4);
        if (o->kind == StructureKind::Guard) QuestCredit(killer, 6);
    }
    // One more kill to a player's war record, its rank climbed: at the next rank's kills, its buff in place of
    // the old one's, the player told; the last rank told to the guild and its union too; the record written, the score told.
    void Climb(Fortress& f, std::int64_t character) {
        Fortress::Record& k = f.records[character];
        ++k.kills;
        int rank = k.rank;
        for (const BattleRankRow& r : config.battleRanks) {
            if (r.rank > rank && std::uint32_t(k.kills) >= r.kills) rank = r.rank;
        }
        if (rank != k.rank) {
            const BattleRankRow* old = Rank(k.rank);
            const BattleRankRow* next = Rank(rank);
            k.rank = rank;
            const auto [name, buff] = host.FortressRankBuff(character, old != nullptr ? old->skill : 0u, next != nullptr ? next->skill : 0u);
            const std::vector<std::byte> notice = Bytes(proto::FortressNotice{proto::FortressNoticeKind::RankUp, f.spec.id, std::uint32_t(rank), name, buff});
            // Ranks 1..5 to the player; the last (6) once, to his union if he has one, else his guild (he among them).
            if (const std::optional<FortressGuild> g = host.FortressGuildOf(character); rank >= 6 && g) {
                SendGuild(g->id, proto::FortressNotice::kId, notice);
            } else {
                host.FortressSendCharacter(character, proto::FortressNotice::kId, notice);
            }
            core::LogInfo("fortress war: {} reaches battle rank {} ({} kills)", name, rank, k.kills);
        }
        const std::uint32_t id = f.spec.id;
        Write([id, character, r = k](Database& db) { SaveBattleRecord(db, id, character, r.kills, r.deaths, r.rank); });
        host.FortressSendCharacter(character, proto::FortressScore::kId, Bytes(proto::FortressScore{id, std::uint32_t(k.kills), std::uint32_t(k.deaths), std::uint8_t(k.rank)}));
    }
    // The siege quests (`_RefSiegeQuest`): the doer's count of the target's type up by one; each quest of that
    // type whose count it reaches gives its rewards (an item, SP experience); a type that does not accumulate (1..5) starts
    // again after its award.
    std::map<std::int64_t, std::map<int, int>> questCounts; // character -> type -> count (this war's)
    void QuestCredit(std::int64_t character, int type) {
        if (character == 0) return;
        int& count = questCounts[character][type];
        ++count;
        bool awarded = false, accumulates = true;
        for (const SiegeQuestRow& q : config.quests) {
            if (q.type != type) continue;
            accumulates = q.accumulates;
            if (count != q.count) continue;
            awarded = true;
            for (const SiegeQuestRow::Reward& r : q.rewards) {
                if (r.type == 1) host.FortressGiveItemTo(character, r.ref, std::max<std::uint32_t>(1, r.value));
                if (r.type == 2) host.FortressQuestSp(character, r.value);
            }
            core::LogInfo("fortress war: character {} completes the siege quest {} ({} of type {})", character, q.id, count, type);
        }
        if (awarded && !accumulates) count = 0;
    }
    // A defensive position at its level 0 (`STRUCTURE_*_DEFENSIVE_POSITION_00`): a foundation the battle aide's Build turns into
    // its first level (the upgrade to `_01`: 5,000 gold, 500 GP); nothing stands there until then.
    bool Foundation(const Structure& s) const {
        const StructureReference* r = Reference(s.refObjId);
        return r != nullptr && r->codename.ends_with("_00");
    }
    void Fell(Structure& s) {
        if (s.entity == 0) return;

        byEntity.erase(s.entity);
        host.RemoveStructure(s.entity);
        s.entity = 0;
    }
    void SaveStructure(const Fortress& f, const Structure& s) {
        const FortressStructRow row{f.spec.id, s.owner, f.Spot(s).eventId, s.refObjId, s.hp, s.state};
        Write([row](Database& db) { SaveFortressStruct(db, row); });
    }
    // The level a structure falls to when the heart is broken: the one it was upgraded from, else its line's first.
    std::uint32_t LevelBelow(std::uint32_t refObjId) const {
        const StructureReference* r = Reference(refObjId);
        if (r == nullptr) return refObjId;
        if (const StructureReference* below = ReferenceNamed(!r->below.empty() ? r->below : r->base)) return below->refObjId;
        return refObjId;
    }
    // The upgradeable structures (the heart, the towers, the gates, the defensive positions) handed to
    // `owner` at full health, standing again: one level down when the heart is broken
    // (`_SiegeFortressResetTempStructAll_NoTx`), at their first level when a war ends in a new owner
    // (`_SiegeFortressResetStructUpgradeLevel_NoTx`: each to its OrgObjCodeName128, the spot's own).
    void Rebuild(std::size_t fi, std::uint32_t owner, bool levelDown) {
        Fortress& f = forts[fi];
        for (std::size_t si = 0; si < f.structures.size(); ++si) {
            Structure& s = f.structures[si];
            const FortressSpec::Structure& spot = f.Spot(s);
            if (spot.kind != StructureKind::Heart && spot.kind != StructureKind::GuardTower && spot.kind != StructureKind::Gate &&
                spot.kind != StructureKind::Defensive) {
                continue;
            }
            Fell(s);
            s.owner = owner;
            s.refObjId = levelDown && s.refObjId != 0 ? LevelBelow(s.refObjId) : spot.refObjId;
            s.hp = MaxHp(spot, s.refObjId);
            s.state = kStructStanding;
            s.open = false;
            s.dirty = false;
            SaveStructure(f, s);
            Stand(fi, si);
            BlockZone(f, s);
        }
    }
    int TowersStanding(const Fortress& f) const {
        int n = 0;
        for (const Structure& s : f.structures) {
            if (f.Spot(s).kind == StructureKind::GuardTower && s.state != kStructDestroyed && s.entity != 0) ++n;
        }
        return n;
    }

    // --- The periods ------------------------------------------------------------------------------

    void OpenRequests() {
        if (requestsOpen) return;
        requestsOpen = true;
        core::LogInfo("fortress war: the application period begins");
        Notice(proto::FortressNoticeKind::RequestsOpen);
        BroadcastList();
        PublishPeriods();
    }
    void CloseRequests() {
        if (!requestsOpen) return;
        requestsOpen = false;
        core::LogInfo("fortress war: the application period ends");
        Notice(proto::FortressNoticeKind::RequestsClosed);
        BroadcastList();
        PublishPeriods();
    }
    void BeginWar() {
        if (warOn) return;
        warOn = true;
        dungeonAllowed = false;
        lastWarStart = unixNow;
        // Every fortress's war begins: its sides from the applications, the heart sealed while a
        // guard tower stands.
        for (Fortress& f : forts) {
            f.atWar = true;
            if (!f.spec.local) continue; // its own server begins it
            // The records are this war's: a war's end clears them (`_SiegeFortressFinished`), a restart in it reads them back.

            f.heartOpenTold = false;
            f.heartOpenAt = TowersStanding(f) == 0 ? std::optional<Clock::time_point>(now) : std::nullopt;
            f.evacuateAt = now + std::chrono::seconds(5);
            core::LogInfo("fortress war: {} begins: owner {}, {} applications", f.spec.codename, f.row.guild, f.requests.size());
        }
        Notice(proto::FortressNoticeKind::WarBegin);
        BroadcastList();
        PublishPeriods();
    }
    void EndWar() {
        if (!warOn) return;
        warOn = false;
        dungeonAllowed = true; // kind 6 sets it again
        questCounts.clear();
        for (std::size_t fi = 0; fi < forts.size(); ++fi) Finish(fi);
        Notice(proto::FortressNoticeKind::WarEnd);
        BroadcastList();
        PublishPeriods();
    }
    // A fortress's war over (`_SiegeFortressFinished`): the
    // guild holding it owns it; the applications, the heart's damage and the records gone.
    void Finish(std::size_t fi) {
        Fortress& f = forts[fi];
        if (!f.atWar) return;
        f.atWar = false;
        const std::uint32_t owner = f.row.guild, holder = f.row.tempGuild;
        const bool taken = holder != 0 && holder != owner;
        if (taken) f.row.guild = holder;
        f.row.tempGuild = 0;
        f.requests.clear();
        f.stone.clear();
        f.stoneDirty.clear();
        if (!f.spec.local) {
            // A mirror: its own server finishes it, writes it and tells its row (the same outcome).
            f.records.clear();
            return;
        }
        // The ranks' buffs end with the war.
        for (const auto& [character, record] : f.records) {
            if (const BattleRankRow* rank = Rank(record.rank)) host.FortressRankBuff(character, rank->skill, 0);
            host.FortressDeed(character, "fortress-war"); // fought in it (a battle record): the beta quests' degree 11
        }
        f.records.clear();
        f.heartOpenAt.reset();
        f.evacuateAt.reset();
        f.openAgainAt.reset();
        // The barricades go, the war camps are foundations again (`_SiegeFortressFinished`); the flags and the summoned
        // monsters go, and with a new owner the guards too.
        for (Structure& s : f.structures) {
            if (BuiltByItem(f.Spot(s).kind) && s.state == kStructBuilt) Clear(f, s);
        }
        RemoveObjects(f, !taken);
        kitRepairs.clear();
        const std::uint32_t id = f.spec.id;
        Write([id, owner, holder](Database& db) {
            std::string error;
            if (!FinishFortress(db, id, owner, holder, error)) core::LogWarning("fortress war: finishing fortress {}: {}", id, error);
        });
        // A new owner's structures at their first level and full health (`_SiegeFortressResetStructUpgradeLevel_NoTx`);
        // a fortress kept keeps its damage and its fallen structures, for the battle aide's repairs.
        if (taken) Rebuild(fi, f.row.guild, false);
        if (taken && owner != 0) host.FortressLost(owner);
        Reward(f);
        Evacuate(f, true);
        PublishRow(f);
        core::LogInfo("fortress war: {} ends: owner {} ({})", f.spec.codename, f.row.guild, taken ? "taken" : "kept");
    }
    // The heart broken: the guild that dealt it the most
    // damage holds the fortress for now (`_SiegeFortress.TempGuildID`); the structures are handed to it
    // (`_SiegeFortressTempFinished`), the towers seal the heart again, the damage is forgotten.
    void Capture(std::size_t fi, std::int64_t killer) {
        Fortress& f = forts[fi];
        std::vector<std::pair<std::uint32_t, std::uint64_t>> damage(f.stone.begin(), f.stone.end());
        std::uint32_t holder = game::TopDamager(damage);
        if (holder == 0) {
            const std::optional<FortressGuild> g = host.FortressGuildOf(killer);
            holder = g ? g->id : 0;
        }
        if (holder == 0) holder = f.Controller(); // nobody's: it stays
        f.row.tempGuild = holder;
        f.stone.clear();
        f.stoneDirty.clear();
        const std::uint32_t id = f.spec.id;
        Write([id, holder](Database& db) { SetFortressHolder(db, id, holder); });
        RemoveObjects(f, false); // `_SiegeFortressTempFinished`: every object of the fortress goes
        Rebuild(fi, holder, true);

        f.heartOpenTold = false;
        f.heartOpenAt = TowersStanding(f) == 0 ? std::optional<Clock::time_point>(now) : std::nullopt;
        // For five minutes only the new holder's side comes in (EntryRefusal); every attacker in its area, the old owner's
        // guild among them, is sent to its first inner gate (the original's teleport of the attackers' set at the capture).
        f.openAgainAt = now + std::chrono::seconds(game::kCaptureEntrySeconds);
        if (const auto gate = std::find_if(f.spec.insideGates.begin(), f.spec.insideGates.end(), [&](const std::string& g) { return host.FortressHasPoint(g); });
            gate != f.spec.insideGates.end()) {
            host.FortressEvacuate(f.spec.regions, *gate, [this, &f](std::int64_t character) {
                const std::optional<FortressGuild> g = host.FortressGuildOf(character);
                return !g || !Participant(f, g->id) || HolderSide(f, g);
            });
        }
        core::LogInfo("fortress war: {} held by guild {} for now", f.spec.codename, holder);
        // The original tells its holder's change as kind 8, the client's only "occupied" line.
        Notice(proto::FortressNoticeKind::Conquer, id, 0, GuildName(holder));
        BroadcastList();
        PublishRow(f);
    }

    // The schedule's (or the cycle's) periods now: applications, war; and the next war's times.
    // The war's period holds its lead first (the original's kind 1 at its start, "30 minutes before"), then the war.
    struct Periods {
        bool requests = false, war = false;
        std::optional<game::Period> requestPeriod, warPeriod;
        std::int64_t warBegin = 0; // the war itself from then to the period's end
    };
    Periods PeriodsAt(std::int64_t t) const {
        Periods p;
        const int cycle = config.requestSeconds + config.gapSeconds + config.warSeconds;
        if (config.warSeconds > 0 && cycle > 0) {
            const std::int64_t base = cycleStart + (t - cycleStart) / cycle * cycle;
            const std::int64_t into = t - base;
            p.requestPeriod = game::Period{base, base + config.requestSeconds};
            // Ours: the cycle's gap is the war period's lead.
            p.warPeriod = game::Period{base + config.requestSeconds, base + cycle};
            p.warBegin = base + config.requestSeconds + config.gapSeconds;
            if (into >= config.requestSeconds + config.gapSeconds + config.warSeconds) {
                p.requestPeriod = game::Period{base + cycle, base + cycle + config.requestSeconds};
            }
            p.requests = p.requestPeriod->Contains(t);
            p.war = t >= p.warBegin && t < p.warPeriod->end;
            return p;
        }
        p.requestPeriod = game::SchedulePeriod(config.schedule, game::kScheduleSiegeRequest, t);
        p.warPeriod = game::SchedulePeriod(config.schedule, game::kScheduleSiegeWar, t);
        p.requests = p.requestPeriod && p.requestPeriod->Contains(t);
        if (p.warPeriod) {
            // The war 30 minutes into its period (define 6: "30 minutes or more").
            p.warBegin = p.warPeriod->start + std::min<std::int64_t>(game::kWarLeadSeconds, p.warPeriod->end - p.warPeriod->start);
            p.war = t >= p.warBegin && t < p.warPeriod->end;
        }
        return p;
    }

    void Warnings(const Periods& p) {
        if (manual || !p.warPeriod) return;
        const game::Period& war = *p.warPeriod;
        if (config.warningMinutes.empty()) return;
        // The period's start: the war's coming (the original's kind 1; the client prints 30).
        if (!warOn && war.Contains(unixNow) && unixNow < p.warBegin && warnedFor != war.start) {
            warnedFor = war.start;
            Notice(proto::FortressNoticeKind::WarSoon, 0, std::uint32_t((p.warBegin - unixNow + 59) / 60));
            dungeonAllowed = false; // kind 1 clears the manager's dungeon flag
        }
        // Before its end: 30, 20, 10 and 1 minutes (kinds 3, 4, 5, 9).
        if (warOn && war.Contains(unixNow)) {
            if (warnedEndFor != war.end) {
                warnedEndFor = war.end;
                warnedEnd.clear();
            }
            for (int m : config.warningMinutes) {
                if (war.end - unixNow <= std::int64_t(m) * 60 && !warnedEnd.contains(m)) {
                    warnedEnd.insert(m);
                    // Only the nearest due warning is said (the others passed while the server was down).
                    if (war.end - unixNow > std::int64_t(m - 1) * 60) Notice(proto::WarEndWarning(m), 0, std::uint32_t(m));
                }
            }
        }
    }

    void Flush() {
        for (Fortress& f : forts) {
            for (Structure& s : f.structures) {
                if (!s.dirty) continue;
                s.dirty = false;
                SaveStructure(f, s);
            }
            for (std::uint32_t guild : f.stoneDirty) {
                const FortressStoneRow row{f.spec.id, guild, std::int64_t(std::min<std::uint64_t>(f.stone[guild], 0x7FFFFFFF))};
                Write([row](Database& db) { SaveFortressStone(db, row); });
            }
            f.stoneDirty.clear();
        }
    }

    // --- Requests at the official -----------------------------------------------------------------

    bool AtOfficial(const FortressActor& a, std::uint32_t npc, const Fortress* f) const {
        const std::string code = host.FortressNpc(npc);
        if (code.empty()) return false;
        const double d = host.FortressDistance(a.entity, npc);
        if (d < 0 || d > game::kFortressNpcReach) return false;
        return f == nullptr ? true : code == f->spec.official;
    }

    void ApplyInfo(const FortressActor& a, std::uint32_t npc) {
        proto::FortressApplyInfo info;
        info.npc = npc;
        const std::string code = host.FortressNpc(npc);
        for (const Fortress& f : forts) {
            if (f.spec.official != code) continue;
            info.rows.push_back({f.spec.id, f.spec.nameKey, GuildName(f.row.guild), f.spec.fee, false});
        }
        const PeriodsWire p = PeriodsNow();
        info.warStart = p.warStart;
        info.warEnd = p.warEnd;
        info.requestStart = p.requestStart;
        info.requestEnd = p.requestEnd;
        info.requestsOpen = requestsOpen;
        if (const std::optional<FortressGuild> g = host.FortressGuildOf(a.character)) {
            if (const FortressRequestRow* r = RequestOf(g->id)) {
                info.applied = r->fortress;
                info.side = proto::FortressSide(r->type);
            }
            for (proto::FortressApplyRow& row : info.rows) {
                const Fortress* f = Find(row.fortress);
                const std::uint32_t ownerAlliance = f != nullptr ? AllianceOf(f->row.guild) : 0;
                row.defend = ownerAlliance != 0 && g->alliance == ownerAlliance && g->id != f->row.guild;
            }
        }
        host.FortressSend(a.peer, proto::FortressApplyInfo::kId, Bytes(info));
    }

    // --- The fortress manager's tax (NPC function 0x17) -----------------------------

    // The tax period (`_Schedule` define 8, SCHEDULE_DEF_SIEGE_TAX_PERIOD), or the dev switch's.
    bool TaxPeriod() const {
        if (!config.periodKeeper) return told && told->tax;
        if (taxForced) return *taxForced;
        const auto p = game::SchedulePeriod(config.schedule, game::kScheduleSiegeTax, unixNow);
        return p && p->Contains(unixNow);
    }
    // The manager in reach and the fortress whose area holds it (none: refused).
    Fortress* AtManager(const FortressActor& a, std::uint32_t npc) {
        const std::string code = host.FortressNpc(npc);
        if (code.find("_FORTRESS_MANAGER") == std::string::npos) return nullptr;
        const double d = host.FortressDistance(a.entity, npc);
        if (d < 0 || d > game::kFortressNpcReach) return nullptr;
        const auto where = host.FortressEntityPosition(npc);
        if (!where) return nullptr;
        for (Fortress& f : forts) {
            if (f.InArea(where->region)) return &f;
        }
        return nullptr;
    }
    // Query (action 0): the rate and the tax gathered (the database's: every game server adds to it).
    void TaxQuery(const FortressActor& a, std::uint32_t npc) {
        Fortress* f = AtManager(a, npc);
        if (f == nullptr) return Answer(a, proto::FortressAction::TaxQuery, FortressError::NoFortress, 0, proto::FortressSide::Attack);
        const std::uint32_t id = f->spec.id;
        const std::uint16_t targets = f->spec.taxTargets;
        auto tax = std::make_shared<std::int64_t>(0);
        host.FortressSubmit([id, tax](Database& db) { *tax = db.Query("SELECT tax FROM _siegefortress WHERE fortressid = $1", {std::int64_t(id)}).Int(0, 0); },
                            [this, a, npc, id, tax, targets] {
                                const Fortress* now = Find(id);
                                const std::int16_t rate = now != nullptr ? std::int16_t(now->row.taxRatio) : 0;
                                host.FortressSend(a.peer, proto::FortressTaxInfo::kId, Bytes(proto::FortressTaxInfo{npc, id, rate, *tax, TaxPeriod(), targets}));
                            });
    }
    // The holder's master only: the codes in the original's order.
    FortressError TaxRefusal(const FortressActor& a, const Fortress* f) const {
        if (!TaxPeriod()) return FortressError::NotTaxPeriod;
        if (f == nullptr) return FortressError::NoFortress;
        const auto guild = host.FortressGuildOf(a.character);
        if (!guild || guild->id != f->Controller()) return FortressError::NotHolder;
        if (!guild->master) return FortressError::OnlyMaster;
        return FortressError::None;
    }
    // Rate (action 1): -20..+20, in the tax period, by the holder's master, not the rate it has.
    void TaxRate(const FortressActor& a, std::uint32_t npc, int rate) {
        Fortress* f = AtManager(a, npc);
        const auto refuse = [&](FortressError e) { Answer(a, proto::FortressAction::TaxRate, e, f != nullptr ? f->spec.id : 0, proto::FortressSide::Attack); };
        if (rate < game::kTaxRateMin || rate > game::kTaxRateMax) return refuse(FortressError::RateRange);
        if (!TaxPeriod()) return refuse(FortressError::NotTaxPeriod);
        if (f == nullptr) return refuse(FortressError::NoFortress);
        if (rate == f->row.taxRatio) return refuse(FortressError::SameRate);
        if (const FortressError e = TaxRefusal(a, f); e != FortressError::None) return refuse(e);
        const std::uint32_t id = f->spec.id, guild = f->row.guild;
        f->row.taxRatio = rate;
        host.FortressSubmit([id, rate](Database& db) { SaveFortressTaxRatio(db, id, rate); }, [] {});
        host.FortressTaxRateChanged(id, guild, rate);
        PublishRow(*f);
        // Every player is told the new rate (the original's kind 7, ``: an open NPC or tax window shows it).
        Notice(proto::FortressNoticeKind::TaxRate, id, std::uint32_t(std::uint16_t(std::int16_t(rate))));
        core::LogInfo("fortress war: {} sets fortress {}'s tax to {} %", a.name, id, rate);
        host.FortressSend(a.peer, proto::FortressAnswer::kId,
                          Bytes(proto::FortressAnswer{proto::FortressAction::TaxRate, true, 0, id, proto::FortressSide::Attack, 0, rate}));
        TaxQuery(a, npc);
    }
    // Levy (action 2): the tax gathered, up to `amount`, to the holder's master.
    void TaxLevy(const FortressActor& a, std::uint32_t npc, std::int64_t amount) {
        Fortress* f = AtManager(a, npc);
        const auto refuse = [&](FortressError e) { Answer(a, proto::FortressAction::TaxLevy, e, f != nullptr ? f->spec.id : 0, proto::FortressSide::Attack); };
        if (const FortressError e = TaxRefusal(a, f); e != FortressError::None) return refuse(e);
        if (amount <= 0) return refuse(FortressError::Failed);
        const std::uint32_t id = f->spec.id;
        // Out of the fortress's row and into the master's gold in one transaction (the original's tax collection is one
        // procedure).
        host.FortressPay(
            a.character, 0, 0, 0,
            [id, amount](Database& tx) -> std::optional<std::uint64_t> {
                if (tx.Query("UPDATE _siegefortress SET tax = tax - $2 WHERE fortressid = $1 AND tax >= $2 RETURNING tax", {std::int64_t(id), amount}).Rows() != 1) {
                    return std::nullopt;
                }
                return std::uint64_t(amount);
            },
            [this, a, npc, id, amount](bool ok, std::uint64_t) {
                if (!ok) return Answer(a, proto::FortressAction::TaxLevy, FortressError::Failed, id, proto::FortressSide::Attack);
                core::LogInfo("fortress war: {} levies {} tax from fortress {}", a.name, amount, id);
                host.FortressSend(a.peer, proto::FortressAnswer::kId,
                                  Bytes(proto::FortressAnswer{proto::FortressAction::TaxLevy, true, 0, id, proto::FortressSide::Attack, 0, amount}));
                TaxQuery(a, npc);
            });
    }

    void Answer(const FortressActor& a, proto::FortressAction action, FortressError error, std::uint32_t fortress, proto::FortressSide side,
                std::uint32_t zone = 0, std::int64_t amount = 0) {
        host.FortressSend(a.peer, proto::FortressAnswer::kId,
                          Bytes(proto::FortressAnswer{action, error == FortressError::None, std::uint16_t(error), fortress, side, zone, amount}));
    }

    // Return to fortress (0x705D): a guild's way to the Gate of Resurrection of the fortress
    // it holds; ten minutes between uses. Not limited to the war.
    void Return(const FortressActor& a, std::uint32_t fortress) {
        const Fortress* f = Find(fortress);
        if (f == nullptr) return; // silent, as the original's 1
        const std::optional<FortressGuild> g = host.FortressGuildOf(a.character);
        if (!g || g->id != f->Controller()) return Answer(a, proto::FortressAction::Return, FortressError::ReturnNotHolder, fortress, proto::FortressSide::Attack);
        // Not in a job suit (0x7C06), not in a Battle Arena match (0x7C09), in the original's order.
        if (host.FortressJobSuit(a.peer)) return Answer(a, proto::FortressAction::Return, FortressError::ReturnUniform, fortress, proto::FortressSide::Attack);
        if (host.FortressInArena(a.peer)) return Answer(a, proto::FortressAction::Return, FortressError::ReturnArena, fortress, proto::FortressSide::Attack);
        const std::shared_ptr<bool> taken = std::make_shared<bool>(false);
        host.FortressSubmit([taken, character = a.character](Database& db) { *taken = TakeFortressReturn(db, character, 600); },
                            [this, a, fortress, taken, gate = f->spec.revivalGate] {
                                if (!*taken) return Answer(a, proto::FortressAction::Return, FortressError::ReturnWaiting, fortress, proto::FortressSide::Attack);
                                if (!host.FortressSendTo(a.peer, gate)) Answer(a, proto::FortressAction::Return, FortressError::Failed, fortress, proto::FortressSide::Attack);
                            });
    }

    // --- The battle aide (NPC function 0x19) ------------------------------

    // The aide in reach and the fortress whose area holds it (none: refused).
    Fortress* AtAide(const FortressActor& a, std::uint32_t npc) {
        const std::string code = host.FortressNpc(npc);
        if (code.find("_FORTRESS_BATTLEAIDE") == std::string::npos) return nullptr;
        const double d = host.FortressDistance(a.entity, npc);
        if (d < 0 || d > game::kFortressNpcReach) return nullptr;
        const auto where = host.FortressEntityPosition(npc);
        if (!where) return nullptr;
        for (Fortress& f : forts) {
            if (f.InArea(where->region)) return &f;
        }
        return nullptr;
    }
    Structure* AtZone(Fortress& f, std::uint32_t zone) {
        for (Structure& s : f.structures) {
            if (f.Spot(s).eventId == zone) return &s;
        }
        return nullptr;
    }
    // A structure's repair price: the missing health at max(1, CostRepair / MaxHP)
    // gold each (1 in every row), the revival's price when destroyed, at most 100,000,000.
    std::uint64_t RepairCost(const Fortress& f, const Structure& s) const {
        const std::uint32_t maxHp = MaxHp(f.Spot(s), s.refObjId);
        const std::uint64_t missing = s.hp >= maxHp ? 0 : maxHp - std::uint64_t(std::max<std::int64_t>(0, s.hp));
        const StructureReference* ref = Reference(s.refObjId);
        const double price = std::max(1.0, ref != nullptr && ref->maxHp > 0 ? double(ref->costRepair) / double(ref->maxHp) : 1.0);
        std::uint64_t cost = std::uint64_t(std::trunc(price * double(missing)));
        if (s.state == kStructDestroyed && ref != nullptr) cost += ref->costRevive;
        return std::min<std::uint64_t>(cost, 100'000'000);
    }
    proto::FortressStructureRecord Record(const Fortress& f, const Structure& s) const {
        proto::FortressStructureRecord r;
        r.zone = f.Spot(s).eventId;
        r.region = f.Spot(s).position.region;
        r.x = f.Spot(s).position.x;
        r.z = f.Spot(s).position.z;
        r.refObjId = s.refObjId;
        r.kind = std::uint8_t(f.Spot(s).kind);
        r.maxHp = MaxHp(f.Spot(s), s.refObjId);
        r.hp = std::uint32_t(std::clamp<std::int64_t>(s.hp, 0, r.maxHp));
        r.state = std::uint16_t((s.state == kStructDestroyed ? 1 : 0) | (s.open ? 2 : 0));
        r.repairCost = std::uint32_t(RepairCost(f, s));
        if (const StructureReference* ref = Reference(s.refObjId)) {
            r.upgradeGold = ref->gold;
            r.upgradePoints = ref->guildPoints;
            const std::int64_t ready = s.madeAt + std::int64_t(ref->minutes) * 60;
            r.waitMinutes = ready > unixNow ? std::uint32_t((ready - unixNow + 59) / 60) : 0u;
            for (const std::string& next : ref->upgrades) {
                const StructureReference* n = next.empty() ? nullptr : ReferenceNamed(next);
                r.upgrades.push_back(n != nullptr ? n->refObjId : 0u);
                if (n != nullptr && r.upgradeLevel == 0) r.upgradeLevel = std::uint8_t(std::clamp(n->reqLevel, 0, 255));
            }
        }
        return r;
    }
    void StructuresInfo(const FortressActor& a, const proto::FortressStructuresQuery& m) {
        Fortress* f = AtAide(a, m.npc);
        if (f == nullptr) return Answer(a, proto::FortressAction::Structures, FortressError::WrongNpc, m.fortress, proto::FortressSide::Attack);
        proto::FortressStructureList list;
        list.npc = m.npc;
        list.fortress = f->spec.id;
        for (const Structure& s : f->structures) {
            if (Reference(s.refObjId) != nullptr) list.structures.push_back(Record(*f, s));
        }
        host.FortressSend(a.peer, proto::FortressStructureList::kId, Bytes(list));
    }
    // Repair: not during a war; whole again, standing; no guild or role asked.
    void Repair(const FortressActor& a, const proto::FortressRepair& m) {
        const auto refuse = [&](FortressError e) { Answer(a, proto::FortressAction::Repair, e, m.fortress, proto::FortressSide::Attack); };
        Fortress* f = AtAide(a, m.npc);
        if (f == nullptr) return refuse(FortressError::WrongNpc);
        if (warOn && f->atWar) return refuse(FortressError::InWar);
        Structure* s = AtZone(*f, m.zone);
        if (s == nullptr || Reference(s->refObjId) == nullptr) return refuse(FortressError::NoStructure);
        if (m.fortress != 0 && m.fortress != f->spec.id) return refuse(FortressError::WrongFortress);
        const std::uint32_t maxHp = MaxHp(f->Spot(*s), s->refObjId);
        if (s->state != kStructDestroyed && s->hp >= std::int64_t(maxHp)) return refuse(FortressError::RepairFull);
        if (s->state == kStructDestroyed && !Reference(s->refObjId)->canRevive) return refuse(FortressError::NoRevive);
        const std::uint64_t cost = RepairCost(*f, *s);
        if (a.gold < cost || s->paying) return refuse(FortressError::RepairGold);
        const std::size_t fi = std::size_t(f - forts.data()), si = std::size_t(s - f->structures.data());
        // The gold and the structure's row in one transaction; the structure whole again once it stands.
        s->paying = true;
        const FortressStructRow row{f->spec.id, s->owner, f->Spot(*s).eventId, s->refObjId, std::int64_t(maxHp), kStructStanding};
        host.FortressPay(
            a.character, cost, 0, 0,
            [row](Database& tx) -> std::optional<std::uint64_t> {
                if (!SaveFortressStruct(tx, row)) return std::nullopt;
                return 0;
            },
            [this, a, fi, si, maxHp, cost, refObjId = row.refObjId, npc = m.npc, zone = m.zone](bool ok, std::uint64_t) {
                if (fi >= forts.size() || si >= forts[fi].structures.size()) return;
                Fortress& f = forts[fi];
                Structure& s = f.structures[si];
                s.paying = false;
                if (!ok || s.refObjId != refObjId) return Answer(a, proto::FortressAction::Repair, FortressError::RepairGold, f.spec.id, proto::FortressSide::Attack, zone);
                Fell(s);
                s.hp = maxHp;
                s.state = kStructStanding;
                s.open = false; // a repair closes it (the state word to 0)
                Stand(fi, si);
                BlockZone(f, s);
                core::LogInfo("fortress war: {} repairs {}'s structure {} for {} gold", a.name, f.spec.codename, zone, cost);
                Answer(a, proto::FortressAction::Repair, FortressError::None, f.spec.id, proto::FortressSide::Attack, zone);
                StructuresInfo(a, {npc, f.spec.id});
            });
    }
    // A barricade built at the battle aide (its database answer in
    // kind 0x13), in the original's order: a
    // battle aide (the dispatcher's NPC check, 3), the zone (3), its structure's reference (0x281E), a barricade (0x281F:
    // TID 1/2/5/6; ours by the zone's kind), the zone of the fortress asked (0x2820); then written for the
    // asker's guild (`_SiegeFortressStructInsert`: a zone already built is the procedure's -1501, which answers nothing).
    // No period, guild, position, gold or MaxBarricade is asked.
    void Build(const FortressActor& a, const proto::FortressBuild& m) {
        const auto refuse = [&](FortressError e) { Answer(a, proto::FortressAction::Build, e, m.fortress, proto::FortressSide::Attack); };
        if (AtAide(a, m.npc) == nullptr) return refuse(FortressError::NoFortress);
        Fortress* f = nullptr;
        Structure* s = nullptr;
        for (Fortress& fort : forts) {
            if (Structure* at = AtZone(fort, m.zone)) {
                f = &fort;
                s = at;
            }
        }
        if (s == nullptr || !f->spec.local) return refuse(FortressError::NoFortress);
        const FortressSpec::Structure& spot = f->Spot(*s);
        if (spot.refObjId == 0) return refuse(FortressError::BuildNoReference);
        if (spot.kind != StructureKind::Barricade) return refuse(FortressError::BuildNotBarricade);
        if (m.fortress != f->spec.id) return refuse(FortressError::BuildWrongFortress);
        if (s->state == kStructBuilt) {
            core::LogInfo("fortress war: {}'s barricade at {}'s zone {} refused: built already (the database's -1501, no answer)", a.name,
                          f->spec.codename, m.zone);
            return;
        }
        const std::optional<FortressGuild> g = host.FortressGuildOf(a.character);
        const std::size_t fi = std::size_t(f - forts.data()), si = std::size_t(s - f->structures.data());
        Fell(*s);
        s->refObjId = spot.refObjId;
        s->owner = g ? g->id : 0u;
        s->hp = MaxHp(spot, s->refObjId);
        s->state = kStructBuilt;
        s->madeAt = unixNow;
        SaveStructure(*f, *s);
        Stand(fi, si);
        BlockZone(*f, *s);
        core::LogInfo("fortress war: {} builds the barricade of {}'s zone {} at the battle aide", a.name, f->spec.codename, m.zone);
        Answer(a, proto::FortressAction::Build, FortressError::None, f->spec.id, proto::FortressSide::Attack, m.zone, spot.refObjId);
    }
    // A gate's pulley: the pulley ties
    // to its gate by name (STRUCTURE_GATE_PULLEY_JA_01 to the place STRUCTURE_POS_JA_GATE_01; the original registers them,
    // inferred to be by name); another fortress's 0x2829, a destroyed gate 0x283C; no guild or position asked.
    void Pulley(const FortressActor& a, const proto::FortressPulley& m) {
        const auto refuse = [&](FortressError e) { Answer(a, proto::FortressAction::Pulley, e, m.fortress, proto::FortressSide::Attack); };
        const std::string code = host.FortressNpc(m.npc);
        const std::size_t at = code.find("_GATE_PULLEY_");
        const double d = host.FortressDistance(a.entity, m.npc);
        if (at == std::string::npos || d < 0 || d > game::kFortressNpcReach) return refuse(FortressError::NoFortress);
        std::string place = "structure_pos_" + code.substr(at + 13, 2) + "_gate_" + code.substr(code.size() - 2);
        std::transform(place.begin(), place.end(), place.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        Fortress* f = nullptr;
        Structure* gate = nullptr;
        for (Fortress& fort : forts) {
            for (Structure& s : fort.structures) {
                if (fort.Spot(s).kind == StructureKind::Gate && fort.Spot(s).navName == place) {
                    f = &fort;
                    gate = &s;
                }
            }
        }
        if (f == nullptr || !f->spec.local) return refuse(FortressError::NoFortress);
        if (m.fortress != f->spec.id) return refuse(FortressError::PulleyFortress);
        if (gate->state == kStructDestroyed || gate->entity == 0) return refuse(FortressError::GateDestroyed);
        gate->open = m.op != 0;
        TellStructure(*f, *gate);
        BlockZone(*f, *gate);
        const std::uint16_t state = std::uint16_t(gate->open ? 2 : 0);
        core::LogInfo("fortress war: {} {} the gate {} of {}", a.name, gate->open ? "opens" : "closes", f->Spot(*gate).eventId, f->spec.codename);
        host.FortressSend(a.peer, proto::FortressAnswer::kId,
                          Bytes(proto::FortressAnswer{proto::FortressAction::Pulley, true, 0, f->spec.id, proto::FortressSide::Attack, f->Spot(*gate).eventId, state}));
    }

    // --- The fortress items -----------------------------------------------------
    std::map<std::pair<std::int64_t, std::uint32_t>, std::int64_t> itemReady; // (character, its COOLTIME key) -> when it may be used again (Unix ms)
    const FortressItemRow* ItemRow(std::uint32_t refObjId) const {
        for (const FortressItemRow& r : config.items) {
            if (r.refObjId == refObjId) return &r;
        }
        return nullptr;
    }
    // Whether the guild is of the fortress's defenders: the guild controlling it, or of its union.
    bool Defender(const Fortress& f, const FortressGuild& g) const {
        const std::uint32_t c = f.Controller();
        if (c == 0) return false;
        const std::uint32_t u = AllianceOf(c);
        return g.id == c || (u != 0 && g.alliance == u);
    }
    void UseItem(const FortressActor& a, const proto::FortressUseItem& m) {
        const auto refuse = [&](FortressError e) { Answer(a, proto::FortressAction::UseItem, e, m.fortress, proto::FortressSide::Attack); };
        const std::optional<std::uint32_t> held = host.FortressItemAt(a.peer, m.slot);
        const FortressItemRow* row = held ? ItemRow(*held) : nullptr;
        if (row == nullptr) return refuse(FortressError::ItemNotUsable);
        Fortress* f = Find(m.fortress);
        const auto where = host.FortressEntityPosition(a.entity);
        if (f == nullptr || !f->spec.local || !where) return refuse(FortressError::ItemWrongZone);
        // The item's place and time (Param2, Param3; the original's server reads them only for the scrolls, the client for
        // all: the callbacks hold the same periods).
        const bool inside = f->InArea(where->region);
        if (row->place == 1 && !inside) return refuse(FortressError::ItemNotInFortress);
        if (row->place == 2 && inside) return refuse(FortressError::ItemInFortress);
        if (row->time == 1 && !(warOn && f->atWar)) return refuse(FortressError::ItemNotInWar);
        if (row->time == 2 && warOn) return refuse(FortressError::ItemInWar);
        if (row->mask != 0 && (host.FortressPositionOf(a.character) & row->mask) == 0) return refuse(FortressError::ItemPosition);
        const std::int64_t nowMs = unixNow * 1000;
        // Its COOLTIME: by its group (game::CoolTimeKey), every item of it waiting with the one used.
        const std::uint32_t coolKey = game::CoolTimeKey(row->refObjId, row->coolGroup);
        if (const auto it = itemReady.find({a.character, coolKey}); it != itemReady.end() && nowMs < it->second) return refuse(FortressError::ItemNotUsable);
        const std::optional<FortressGuild> g = host.FortressGuildOf(a.character);
        FortressError e = FortressError::ItemNotUsable;
        if (row->typeId == std::array<int, 4>{3, 3, 3, 8}) e = Manual(a, *f, *row, g, m.zone, *where);
        if (row->typeId == std::array<int, 4>{3, 3, 3, 9}) e = PlantFlag(a, *f, *row, g, m, *where);
        if (row->typeId == std::array<int, 4>{3, 3, 1, 10}) e = RepairKit(a, *f, *row, g, m.target, *where);
        if (row->typeId == std::array<int, 4>{3, 3, 6, 2}) e = ShockBomb(a, *row, m.target, *where);
        if (row->typeId == std::array<int, 4>{3, 3, 3, 6} || row->typeId == std::array<int, 4>{3, 3, 3, 7}) e = PlaceGuard(a, *f, *row, g, m, *where);
        if (row->typeId == std::array<int, 4>{3, 3, 3, 11}) e = ReadScroll(a, *f, *row, *where);
        if (e != FortressError::None) return refuse(e);
        host.FortressTakeItem(a.peer, m.slot);
        itemReady[{a.character, coolKey}] = nowMs + row->reuseMs;
        if (row->reuseMs > 0) {
            host.FortressSend(a.peer, proto::Cooldown::kId, Bytes(proto::Cooldown{proto::CooldownKind::Item, row->refObjId, std::uint32_t(row->reuseMs)}));
        }
        host.FortressSend(a.peer, proto::FortressAnswer::kId,
                          Bytes(proto::FortressAnswer{proto::FortressAction::UseItem, true, 0, f->spec.id, proto::FortressSide::Attack, m.zone, row->typeId[3]}));

    }
    // A manual (TID4 8): a barricade by the holder on a free barricade zone, under the fortress's
    // `MaxBarricade`; a war camp by an attacker on its foundation, one a guild.
    FortressError Manual(const FortressActor& a, Fortress& f, const FortressItemRow& row, const std::optional<FortressGuild>& g, std::uint32_t zone,
                         const proto::WorldPosition& at) {
        const bool camp = row.target.find("HEADQUARTERS") != std::string::npos;
        const StructureKind want = camp ? StructureKind::Headquarters : StructureKind::Barricade;
        const proto::FlatPosition me = proto::ToFlat(at);
        const auto distance = [&](const Structure& s) {
            const proto::FlatPosition p = proto::ToFlat(f.Spot(s).position);
            return std::hypot(p.x - me.x, p.z - me.z);
        };
        Structure* s = nullptr;
        if (zone != 0) {
            s = AtZone(f, zone);
            if (s == nullptr) return FortressError::ItemWrongZone;
            if (f.Spot(*s).kind != want) return FortressError::ItemNotUsable;
        } else {
            // Ours: the nearest free zone of the manual's kind (the stock client marks them and the player clicks one).
            double best = 1e18;
            for (Structure& c : f.structures) {
                if (f.Spot(c).kind != want || c.state == kStructBuilt) continue;
                if (const double d = distance(c); d < best) {
                    best = d;
                    s = &c;
                }
            }
            if (s == nullptr) return FortressError::ItemTooFar;
        }
        if (distance(*s) > double(row.range)) return FortressError::ItemTooFar;
        if (!g) return FortressError::ItemPermission;
        if (!camp) {
            if (g->id != f.Controller()) return FortressError::ItemNotHolder;
            const auto built = std::count_if(f.structures.begin(), f.structures.end(),
                                             [&](const Structure& c) { return f.Spot(c).kind == StructureKind::Barricade && c.state == kStructBuilt; });
            if (f.spec.maxBarricade > 0 && built >= f.spec.maxBarricade) return FortressError::ItemLimit;
            if (s->state == kStructBuilt) return FortressError::ItemZoneTaken;
        } else {
            const bool holdsAny = std::any_of(forts.begin(), forts.end(), [&](const Fortress& o) { return o.row.guild == g->id; });
            if (holdsAny || Defender(f, *g)) return FortressError::ItemDefender;
            const bool hasCamp = std::any_of(f.structures.begin(), f.structures.end(), [&](const Structure& c) {
                return f.Spot(c).kind == StructureKind::Headquarters && c.state == kStructBuilt && c.owner == g->id;
            });
            if (hasCamp) return FortressError::ItemCampBuilt;
            if (s->state == kStructBuilt) return FortressError::ItemAlreadyBuilt;
        }
        // A barricade is the zone's own (its `_RefEventZone.strParam1`; the manual's Param1
        // only names the kind of zone it takes); a camp the manual's (STRUCTURE_HEADQUARTERS_01, the foundation's next level).
        const auto character = host.FortressCharacter(row.target);
        if (!character) return FortressError::ItemNotUsable;
        const std::size_t fi = std::size_t(&f - forts.data()), si = std::size_t(s - f.structures.data());
        Fell(*s);
        s->refObjId = !camp && f.Spot(*s).refObjId != 0 ? f.Spot(*s).refObjId : character->first;
        s->owner = g->id;
        s->hp = MaxHp(f.Spot(*s), s->refObjId);
        s->state = kStructBuilt;
        s->madeAt = unixNow;
        SaveStructure(f, *s);
        Stand(fi, si);
        BlockZone(f, *s);
        core::LogInfo("fortress war: {} builds {} at {}'s zone {}", a.name, row.target, f.spec.codename, f.Spot(*s).eventId);
        return FortressError::None;
    }
    static std::string Trimmed(std::string t) {
        while (!t.empty() && (t.back() == ' ' || t.back() == '\0')) t.pop_back();
        return t;
    }
    // A flag (TID4 9): its guild's, at a point within the item's
    // Range of the player (3D, 0x18D8) that a straight walk reaches (0x18D9); the structure of its Param1 standing there for
    // 120 s with its DefaultSkill_1 as an aura over the guild's players near it.
    FortressError PlantFlag(const FortressActor& a, Fortress& f, const FortressItemRow& row, const std::optional<FortressGuild>& g, const proto::FortressUseItem& m,
                            const proto::WorldPosition& me) {
        if (!g) return FortressError::ItemPermission;
        const proto::WorldPosition at{m.region, m.x, m.y, m.z};
        const proto::FlatPosition p = proto::ToFlat(me), q = proto::ToFlat(at);
        const double dy = double(me.y) - double(at.y);
        const double d = std::sqrt((p.x - q.x) * (p.x - q.x) + (p.z - q.z) * (p.z - q.z) + dy * dy);
        if (m.region == 0 || d > double(row.range)) return FortressError::ItemTooFar;
        if (!f.InArea(m.region)) return FortressError::ItemWrongZone;
        if (!host.FortressWalkClear(me, at)) return FortressError::ItemNotWalkable;
        const std::string code = Trimmed(row.target);
        const auto character = host.FortressCharacter(code);
        if (!character) return FortressError::ItemNotUsable;
        Fortress::Object o;
        o.refObjId = character->first;
        o.owner = g->id;
        o.kind = StructureKind::Flag;
        o.maxHp = config.structureHp != 0 ? config.structureHp : std::max<std::uint32_t>(1, character->second);
        o.hp = o.maxHp;
        o.at = at;
        o.ownerLevel = a.level;
        o.until = now + std::chrono::seconds(config.flagSeconds);

        o.pulseMs = host.FortressFlagPulseMs(o.refObjId);
        o.nextPulse = now + std::chrono::milliseconds(o.pulseMs > 0 ? o.pulseMs : 1000);
        const std::size_t fi = std::size_t(&f - forts.data());
        SaveNewObject(f, o);
        const std::uint32_t entity = StandObject(fi, o);
        if (entity == 0) return FortressError::ItemNotUsable;
        if (o.pulseMs == 0) host.FortressFlagAura(entity, o.refObjId, o.owner, false);
        core::LogInfo("fortress war: {} plants {} at {}'s ({:.0f}, {:.0f})", a.name, code, f.spec.codename, m.x, m.z);
        return FortressError::None;
    }
    // A repair kit (TID 3,3,1,10) on a structure of the fortress: the holder's (0x18E5), a structure (0x18EC),
    // neither destroyed nor whole (0x18ED), within the item's Range (0x18EA); its skill's heal (n % of the most health) every
    // 'puls' for its 'dura' (the war, the place and the position are the item's checks above).
    FortressError RepairKit(const FortressActor& a, Fortress& f, const FortressItemRow& row, const std::optional<FortressGuild>& g, std::uint32_t target,
                            const proto::WorldPosition& me) {
        if (!g || g->id != f.Controller()) return FortressError::ItemNotHolder;
        const auto it = byEntity.find(target);
        if (it == byEntity.end() || &forts[it->second.first] != &f) return FortressError::ItemWrongTarget;
        Structure& s = f.structures[it->second.second];
        const std::uint32_t maxHp = MaxHp(f.Spot(s), s.refObjId);
        if (s.state == kStructDestroyed || s.hp <= 0 || s.hp >= std::int64_t(maxHp)) return FortressError::ItemTargetState;
        const auto there = host.FortressEntityPosition(target);
        if (!there) return FortressError::ItemWrongTarget;
        const proto::FlatPosition p = proto::ToFlat(me), q = proto::ToFlat(*there);
        if (std::hypot(p.x - q.x, p.z - q.z) > double(row.range)) return FortressError::ItemTargetFar;
        const auto kit = host.FortressKitSkill(Trimmed(row.target));
        if (!kit || kit->percent <= 0) return FortressError::ItemNotUsable;
        const int pulseMs = kit->pulseMs > 0 ? kit->pulseMs : game::kRepairKitPulseMs;
        const int durationMs = kit->durationMs > 0 ? kit->durationMs : game::kRepairKitMs;
        KitRepair r;
        r.structure = target;
        r.amount = std::max<std::uint32_t>(1, std::uint32_t(std::uint64_t(maxHp) * std::uint64_t(kit->percent) / 100));
        r.pulseMs = pulseMs;
        r.pulsesLeft = std::max(1, durationMs / pulseMs);
        r.next = now + std::chrono::milliseconds(pulseMs);
        kitRepairs.push_back(r);
        core::LogInfo("fortress war: {} repairs {}'s structure {} with {} ({} x {})", a.name, f.spec.codename, f.Spot(s).eventId, Trimmed(row.target),
                      r.pulsesLeft, r.amount);
        return FortressError::None;
    }
    // The shock bomb (TID 3,3,6,2): its skill (Param1, SKILL_FORT_SHOCK_BOMB: a knock back and a stun) cast on an enemy within
    // the item's Range, as its rows make it (the skill unit's own checks and timing).
    FortressError ShockBomb(const FortressActor& a, const FortressItemRow& row, std::uint32_t target, const proto::WorldPosition& me) {
        const auto there = target != 0 ? host.FortressEntityPosition(target) : std::nullopt;
        if (!there) return FortressError::ItemWrongTarget;
        const proto::FlatPosition p = proto::ToFlat(me), q = proto::ToFlat(*there);
        if (std::hypot(p.x - q.x, p.z - q.z) > double(row.range)) return FortressError::ItemTargetFar;
        if (!host.FortressCastItemSkill(a.peer, Trimmed(row.target), target)) return FortressError::ItemWrongTarget;
        core::LogInfo("fortress war: {} throws {} at {}", a.name, Trimmed(row.target), target);
        return FortressError::None;
    }
    // A guard (TID4 7) or a tablet's monster (6) placed (in its order after the item's period and reuse): the
    // point within the item's Range in 3D (0x18D8) and a clear walk to it (0x18D9); the group (Param1) allowed at the fortress
    // (`_RefSiegeFortressGuard`, 0x18E7) at the placer's level (0x1811); a guard under the fortress's `MaxGuard` (0x18EF).
    // No holder or position check in the server (the client's: the guards' mask 5). Kept in `_SiegeFortressObject` with
    // `OwnerLevel`; a guard stays until felled, dismissed or the fortress changes hands, a tablet's monster 20 minutes
    // (inferred).
    FortressError PlaceGuard(const FortressActor& a, Fortress& f, const FortressItemRow& row, const std::optional<FortressGuild>& g, const proto::FortressUseItem& m,
                             const proto::WorldPosition& me) {
        const bool guard = row.typeId[3] == 7;
        const proto::WorldPosition at{m.region, m.x, m.y, m.z};
        const proto::FlatPosition p = proto::ToFlat(me), q = proto::ToFlat(at);
        const double dy = double(me.y) - double(at.y);
        if (m.region == 0 || std::sqrt((p.x - q.x) * (p.x - q.x) + (p.z - q.z) * (p.z - q.z) + dy * dy) > double(row.range)) return FortressError::ItemTooFar;
        if (!host.FortressWalkClear(me, at)) return FortressError::ItemNotWalkable;
        const std::string group = Trimmed(row.target);
        const SiegeVariantRow* variant = nullptr;
        bool allowed = false;
        for (const SiegeVariantRow& v : config.guards) {
            if (v.fortress != f.spec.id || v.group != group) continue;
            allowed = true;
            if (v.level == a.level) variant = &v;
        }
        if (!allowed) return FortressError::ItemGroup;
        if (variant == nullptr) return FortressError::NoVariant;
        if (!f.InArea(m.region)) return FortressError::ItemWrongZone;
        if (guard) {
            const auto standing = std::count_if(f.objects.begin(), f.objects.end(), [](const auto& o) { return o.second.kind == StructureKind::Guard; });
            if (f.spec.maxGuard > 0 && standing >= f.spec.maxGuard) return FortressError::ItemLimit;
        }
        if (!g) return FortressError::ItemPermission;
        const auto character = host.FortressCharacterRow(variant->refObjId);
        if (!character) return FortressError::NoVariant;
        Fortress::Object o;
        o.refObjId = variant->refObjId;
        o.owner = g->id;
        o.kind = guard ? StructureKind::Guard : StructureKind::Summoned;
        o.maxHp = character->maxHp;
        o.hp = character->maxHp;
        o.at = at;
        o.ownerLevel = a.level;
        if (!guard) o.until = now + std::chrono::seconds(game::kSummonedSeconds);
        SaveNewObject(f, o);
        const std::uint32_t entity = StandObject(std::size_t(&f - forts.data()), o);
        if (entity == 0) return FortressError::ItemNotUsable;
        core::LogInfo("fortress war: {} places {} (level {}) at {}'s ({:.0f}, {:.0f})", a.name, character->codename, variant->level, f.spec.codename, m.x, m.z);
        return FortressError::None;
    }
    // A summon scroll (TID4 11): the unique of its group (Param1) at the reader's level (`_RefSiegeLvlSummonMonster`;
    // none: 5) at the reader's place, an ordinary unique with no owner (inferred: the spawn passes none);
    // its period and place are the item's (Param3 2 outside the war, Param2 1 in a fortress's world).
    FortressError ReadScroll(const FortressActor& a, Fortress& f, const FortressItemRow& row, const proto::WorldPosition& me) {
        const std::string group = Trimmed(row.target);
        const SiegeVariantRow* variant = nullptr;
        for (const SiegeVariantRow& v : config.summons) {
            if (v.group == group && v.level == a.level) variant = &v;
        }
        if (variant == nullptr) return FortressError::NoSummon;
        if (host.FortressSpawnUnique(variant->refObjId, me) == 0) return FortressError::NoSummon;
        core::LogInfo("fortress war: {} reads {}: its unique {} at {}", a.name, row.codename, variant->refObjId, f.spec.codename);
        return FortressError::None;
    }
    // The objects' and the kits' time (each tick).
    void TickObjects() {
        for (KitRepair& r : kitRepairs) {
            while (r.pulsesLeft > 0 && now >= r.next) {
                --r.pulsesLeft;
                r.next += std::chrono::milliseconds(r.pulseMs);
                const auto it = byEntity.find(r.structure);
                if (it == byEntity.end()) {
                    r.pulsesLeft = 0;
                    break;
                }
                Fortress& f = forts[it->second.first];
                Structure& s = f.structures[it->second.second];
                if (s.state == kStructDestroyed) {
                    r.pulsesLeft = 0;
                    break;
                }
                if (const std::uint32_t hp = host.FortressHealStructure(r.structure, r.amount); hp > 0) {
                    s.hp = hp;
                    s.dirty = true;
                }
            }
        }
        std::erase_if(kitRepairs, [](const KitRepair& r) { return r.pulsesLeft <= 0; });
        // The structures' own skills while the war is on (their 'S' skills, cast as they stand in the original): each
        // standing one's at its period, for the holder's side.
        for (Fortress& f : forts) {
            if (!f.atWar) continue;
            for (Structure& s : f.structures) {
                if (s.entity == 0 || s.state != kStructStanding || s.hp <= 0 || now < s.nextSkill) continue;
                const int ms = host.FortressStructureSkillMs(s.refObjId);
                s.nextSkill = now + std::chrono::milliseconds(ms > 0 ? ms : 60'000);
                if (ms > 0) host.FortressStructureSkill(s.entity, s.refObjId, f.Controller());
            }
        }
        std::vector<std::uint32_t> expired;
        for (Fortress& f : forts) {
            for (auto& [entity, o] : f.objects) {
                if (o.until && now >= *o.until) {
                    expired.push_back(entity);
                    continue;
                }
                if (o.kind == StructureKind::Flag && now >= o.nextPulse) {
                    o.nextPulse = now + std::chrono::milliseconds(o.pulseMs > 0 ? o.pulseMs : 1000);
                    host.FortressFlagAura(entity, o.refObjId, o.owner, o.pulseMs > 0);
                }
            }
        }
        for (const std::uint32_t e : expired) {
            core::LogInfo("fortress war: the object {} is gone (its time is over)", e);
            RemoveObject(e);
        }
    }

    // --- The manager's other rows ------------------------------------------------

    std::int64_t lastWarStart = 0; // the last war's beginning here (Unix seconds; 0: none since the start)
    std::map<std::pair<std::int64_t, std::uint32_t>, std::int64_t> blessReady; // (character, skill) -> its reuse's end (Unix ms)
    void ManageAnswer(const FortressActor& a, proto::FortressAction action, FortressError e, std::uint32_t fortress, std::int64_t amount = 0) {
        host.FortressSend(a.peer, proto::FortressAnswer::kId,
                          Bytes(proto::FortressAnswer{action, e == FortressError::None, std::uint16_t(e), fortress, proto::FortressSide::Attack, 0, amount}));
    }
    void Manage(const FortressActor& a, const proto::FortressManage& m) {
        using A = proto::FortressAction;
        switch (m.action) {
        case A::Hired:
        case A::Hire:
        case A::Schedule:
        case A::Introduction:
        case A::Introduce:
        case A::GpHistory:
        case A::Bless: break;
        default: return ManageMore(a, m);
        }
        Fortress* f = AtManager(a, m.npc);
        if (f == nullptr) return ManageAnswer(a, m.action, FortressError::NoFortress, m.fortress);
        const std::optional<FortressGuild> g = host.FortressGuildOf(a.character);
        switch (m.action) {
        case A::Hired: return ManageAnswer(a, A::Hired, FortressError::None, f->spec.id, f->row.npcHired);
        case A::Hire: return Hire(a, *f, g, m.value);
        case A::Schedule: return Schedule(a, *f, g);
        case A::Introduction:
            // The holder's members only (0x2806).
            if (!g || g->id != f->Controller()) return ManageAnswer(a, A::Introduction, FortressError::NotHolder, f->spec.id);
            host.FortressSend(a.peer, proto::FortressIntroduction::kId, Bytes(proto::FortressIntroduction{f->spec.id, f->row.introduction}));
            return;
        case A::Introduce: return Introduce(a, *f, g, m.text);
        case A::GpHistory:
            if (!g || g->id != f->Controller()) return ManageAnswer(a, A::GpHistory, FortressError::NotHolder, f->spec.id);
            host.FortressGpHistoryTo(a.peer, g->id);
            return;
        case A::Bless: return Bless(a, *f, g, m.value);
        default: return;
        }
    }
    // A hire (action 4): not in the war (0x2818); the holder's guild (0x2806), its master (0x2807); not hired yet
    // (0x2809); 30,000 gold (0x280F) and 3,000 GP (0x280E); `_SiegeFortressHireNPC`. Hiring changes nothing but the flags
    // (re/38 9.3): the holder's members are told them (0x385F 0x12).
    static constexpr std::uint64_t kHireGold = 30'000;
    static constexpr std::uint32_t kHirePoints = 3'000;
    void Hire(const FortressActor& a, Fortress& f, const std::optional<FortressGuild>& g, std::uint32_t bit) {
        const auto refuse = [&](FortressError e) { ManageAnswer(a, proto::FortressAction::Hire, e, f.spec.id); };
        if (warOn) return refuse(FortressError::InWar);
        if (bit != 1 && bit != 2 && bit != 4) return refuse(FortressError::NoFortress);
        if (!g || g->id != f.row.guild) return refuse(FortressError::NotHolder);
        if (!g->master) return refuse(FortressError::OnlyMaster);
        if ((f.row.npcHired & bit) != 0) return refuse(FortressError::HireDone);
        if (a.gold < kHireGold) return refuse(FortressError::Gold);
        if (g->points < kHirePoints) return refuse(FortressError::GuildPoints);
        // Hired now (a second hire waits), with the gold, the guild's points, their history and the flags in one
        // transaction; undone if it does not stand.
        f.row.npcHired = std::uint8_t(f.row.npcHired | bit);
        const std::uint32_t id = f.spec.id, guild = g->id;
        const std::uint8_t hired = f.row.npcHired;
        host.FortressPay(
            a.character, kHireGold, guild, kHirePoints,
            [id, hired, guild, name = a.name](Database& tx) -> std::optional<std::uint64_t> {
                if (!SaveFortressHired(tx, id, hired) || !AddGpHistory(tx, guild, name, kHirePoints, game::kGpReasonNpc)) return std::nullopt;
                return 0;
            },
            [this, a, id, bit, guild](bool ok, std::uint64_t) {
                Fortress* f = Find(id);
                if (f == nullptr) return;
                if (!ok) {
                    f->row.npcHired = std::uint8_t(f->row.npcHired & ~bit);
                    return ManageAnswer(a, proto::FortressAction::Hire, FortressError::Gold, id);
                }
                SendGuild(guild, proto::FortressNotice::kId, Bytes(proto::FortressNotice{proto::FortressNoticeKind::Hired, id, f->row.npcHired}));
                PublishRow(*f);
                core::LogInfo("fortress war: {} hires the NPC {} at {} (flags {})", a.name, bit, f->spec.codename, int(f->row.npcHired));
                ManageAnswer(a, proto::FortressAction::Hire, FortressError::None, id, f->row.npcHired);
            });
    }
    // The war's schedule (action 5): the last and the next war's month, day, weekday, hour and minute, and whether
    // any guild applied to attack this fortress.
    static std::array<std::uint8_t, 5> DateOf(std::int64_t t) {
        if (t <= 0) return {};
        std::time_t tt = std::time_t(t);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &tt);
#else
        localtime_r(&tt, &local);
#endif
        return {std::uint8_t(local.tm_mon + 1), std::uint8_t(local.tm_mday), std::uint8_t(local.tm_wday), std::uint8_t(local.tm_hour), std::uint8_t(local.tm_min)};
    }
    std::int64_t NextWarStart() const {
        std::int64_t t = unixNow;
        for (int i = 0; i < 4; ++i) {
            const Periods p = PeriodsAt(t);
            if (!p.warPeriod) return 0;
            if (p.warBegin > unixNow) return p.warBegin;
            t = p.warPeriod->end + 1;
        }
        return 0;
    }
    void Schedule(const FortressActor& a, const Fortress& f, const std::optional<FortressGuild>& g) {
        proto::FortressSchedule s;
        s.fortress = f.spec.id;
        s.last = DateOf(lastWarStart);
        s.next = DateOf(manual ? 0 : NextWarStart());
        (void)g;
        s.applied = std::any_of(f.requests.begin(), f.requests.end(), [](const FortressRequestRow& r) { return r.type == std::uint8_t(proto::FortressSide::Attack); });
        host.FortressSend(a.peer, proto::FortressSchedule::kId, Bytes(s));
    }
    // The introduction written (action 0x1A): the holder's master (0x2806, 0x2807), allowed
    // (`IntroductionModificationPermission`, 0x284C), at most 120 letters (0x283F); kept, in the list's third string.
    static constexpr std::size_t kIntroductionMost = 120;
    void Introduce(const FortressActor& a, Fortress& f, const std::optional<FortressGuild>& g, const std::string& text) {
        const auto refuse = [&](FortressError e) { ManageAnswer(a, proto::FortressAction::Introduce, e, f.spec.id); };
        if (!g || g->id != f.Controller()) return refuse(FortressError::NotHolder);
        if (!g->master) return refuse(FortressError::OnlyMaster);
        if (!f.row.introPermission) return refuse(FortressError::IntroPermission);
        if (text.size() > kIntroductionMost) return refuse(FortressError::IntroLength);
        f.row.introduction = text;
        const std::uint32_t id = f.spec.id;
        Write([id, text](Database& db) { SaveFortressIntroduction(db, id, text); });
        BroadcastList();
        PublishRow(f);
        core::LogInfo("fortress war: {} writes {}'s introduction", a.name, f.spec.codename);
        ManageAnswer(a, proto::FortressAction::Introduce, FortressError::None, id);
    }
    // A blessing (action 0x1C, in its order): the holder's guild or its union (0x283D: the two union ids compared with no
    // test for none, as the original's, so a guild of no union passes when the holder has none), the fortress's row
    // (0x2840), no fortress blessing on the player (0x300C), its reuse (0x284A), a buff that may stand (0x300C), the
    // gold (0x280F), a guild (0x280B), the GP (0x280E);
    // `_SiegeFortressEngageBlessBuff` takes both. The buff goes on the one who asked (inferred: the replacement and reuse
    // checks are his; the answer's delivery was not traced).
    void Bless(const FortressActor& a, Fortress& f, const std::optional<FortressGuild>& g, std::uint32_t blessId) {
        const auto refuse = [&](FortressError e) { ManageAnswer(a, proto::FortressAction::Bless, e, f.spec.id); };
        const std::uint32_t holder = f.Controller();
        if (holder == 0) return refuse(FortressError::NotHolder);
        const std::uint32_t holderUnion = AllianceOf(holder);
        if ((g ? g->id : 0u) != holder && (g ? g->alliance : 0u) != holderUnion) return refuse(FortressError::NotUnion);
        const BlessRow* row = nullptr;
        for (const BlessRow& b : config.blesses) {
            if (b.id == blessId && b.fortress == f.spec.id) row = &b;
        }
        if (row == nullptr) return refuse(FortressError::NoBless);
        const FortressHost::BlessCheck check = host.FortressBlessCheck(a.peer, row->skill);
        if (!check.known) return refuse(FortressError::NoBless);
        // One fortress blessing at a time: any `_RefSiegeBlessBuff` skill among the player's buffs.
        for (const std::uint32_t skill : check.running) {
            if (std::any_of(config.blesses.begin(), config.blesses.end(), [&](const BlessRow& b) { return b.skill == skill; })) return refuse(FortressError::BlessOverlap);
        }
        const std::int64_t nowMs = unixNow * 1000;
        if (const auto it = blessReady.find({a.character, row->skill}); it != blessReady.end() && nowMs < it->second) return refuse(FortressError::BlessWait);
        if (!check.admitted) return refuse(FortressError::BlessOverlap);
        if (a.gold < row->gold) return refuse(FortressError::Gold);
        if (!g) return refuse(FortressError::NotInGuild);
        if (g->points < row->guildPoints) return refuse(FortressError::GuildPoints);
        // Its reuse begun now (a second ask waits), the gold, the guild's points and their history in one transaction; the
        // blessing once it stands (its reuse forgotten if it does not).
        const std::pair<std::int64_t, std::uint32_t> key{a.character, row->skill};
        const auto before = blessReady.find(key);
        const std::optional<std::int64_t> was = before != blessReady.end() ? std::optional(before->second) : std::nullopt;
        blessReady[key] = nowMs + check.reuseMs;
        host.FortressPay(
            a.character, row->gold, g->id, row->guildPoints,
            [guild = g->id, name = a.name, points = row->guildPoints](Database& tx) -> std::optional<std::uint64_t> {
                if (!AddGpHistory(tx, guild, name, points, game::kGpReasonFortressBuff)) return std::nullopt;
                return 0;
            },
            [this, a, key, was, skill = row->skill, id = f.spec.id, blessId](bool ok, std::uint64_t) {
                if (!ok) {
                    if (was) {
                        blessReady[key] = *was;
                    } else {
                        blessReady.erase(key);
                    }
                    return ManageAnswer(a, proto::FortressAction::Bless, FortressError::BlessFailed, id);
                }
                host.FortressBlessGive(a.peer, skill);
                core::LogInfo("fortress war: {} takes fortress {}'s blessing {} (skill {})", a.name, id, blessId, skill);
                ManageAnswer(a, proto::FortressAction::Bless, FortressError::None, id, skill);
            });
    }
    // The smith's, the trainer's and the Cave's actions.
    void ManageMore(const FortressActor& a, const proto::FortressManage& m) {
        using A = proto::FortressAction;
        if (m.action >= A::ForgeQuery && m.action <= A::TrainCollect) return Forge(a, m);
        DungeonAction(a, m);
    }

    // --- The smith and the trainer ----------------------------------
    // The forge's NPC in reach (the smith's _FORTRESS_SMITH, the trainer's _FORTRESS_TRAINER) and its fortress (none: refused).
    Fortress* AtForge(const FortressActor& a, std::uint32_t npc, bool trainer) {
        const std::string code = host.FortressNpc(npc);
        if (code.find(trainer ? "_FORTRESS_TRAINER" : "_FORTRESS_SMITH") == std::string::npos) return nullptr;
        const double d = host.FortressDistance(a.entity, npc);
        if (d < 0 || d > game::kFortressNpcReach) return nullptr;
        const auto where = host.FortressEntityPosition(npc);
        if (!where) return nullptr;
        for (Fortress& f : forts) {
            if (f.InArea(where->region)) return &f;
        }
        return nullptr;
    }
    const ForgeReferenceRow* ForgeRow(std::uint32_t fortress, std::uint32_t item) const {
        for (const ForgeReferenceRow& r : config.forges) {
            if (r.fortress == fortress && r.item == item) return &r;
        }
        return nullptr;
    }
    bool TrainerItem(std::uint32_t fortress, std::uint32_t item) const {
        const ForgeReferenceRow* r = ForgeRow(fortress, item);
        return r != nullptr && r->trainer;
    }
    FortressForgeRow* Running(Fortress& f, bool trainer) {
        for (FortressForgeRow& r : f.forges) {
            if (TrainerItem(f.spec.id, r.item) == trainer) return &r;
        }
        return nullptr;
    }
    static constexpr int kForgeMost = 20;
    void Forge(const FortressActor& a, const proto::FortressManage& m) {
        using A = proto::FortressAction;
        const bool trainer = m.action >= A::TrainQuery;
        const int op = int(m.action) - int(trainer ? A::TrainQuery : A::ForgeQuery); // 0 query, 1 start, 2 cancel, 3 collect
        Fortress* f = AtForge(a, m.npc, trainer);
        const auto refuse = [&](FortressError e) { ManageAnswer(a, m.action, e, f != nullptr ? f->spec.id : m.fortress); };
        // The query answers anyone at the NPC; the others are not for the war (0x2818), then the fortress.
        if (op != 0 && warOn) return refuse(FortressError::InWar);
        if (f == nullptr) return refuse(FortressError::NoFortress);
        const std::optional<FortressGuild> g = host.FortressGuildOf(a.character);
        const std::uint8_t position = trainer ? game::kPositionTraining : game::kPositionProduction;
        // The holder's guild (0x2806), its master or the side's administrator (0x2817).
        const auto holderRefusal = [&]() -> FortressError {
            if (!g || g->id != f->Controller()) return FortressError::NotHolder;
            if (!g->master && (host.FortressPositionOf(a.character) & position) == 0) return FortressError::ForgePosition;
            return FortressError::None;
        };
        FortressForgeRow* running = Running(*f, trainer);
        const auto tell = [&] {
            proto::FortressForge s{m.npc, std::uint8_t(trainer ? 1 : 0), f->spec.id};
            if (FortressForgeRow* r = Running(*f, trainer)) {
                s.has = true;
                s.item = r->item;
                s.count = std::uint16_t(r->amount);
                s.done = r->finished;
                s.finishes = r->finish;
            }
            host.FortressSend(a.peer, proto::FortressForge::kId, Bytes(s));
        };
        if (op == 0) return tell();
        const std::uint32_t id = f->spec.id;
        if (op == 1) {
            // Start: not in the war (0x2818), none running (0x280A), the holder's guild (0x2806), the master or the side's
            // position (Production 8, Training 0x10; 0x2817), the side's item (3), at most 20 (0x2816; ours: none refused
            // too, the original's free empty work), the gold (0x280F) and GP (0x280E); cost and time times the count, times
            // 0.85 when a member holds the side's position.
            if (running != nullptr) return refuse(FortressError::ForgeBusy);
            if (const FortressError e = holderRefusal(); e != FortressError::None) return refuse(e);
            const ForgeReferenceRow* row = ForgeRow(id, m.value);
            if (row == nullptr || row->trainer != trainer) return refuse(FortressError::NoFortress);
            const int count = int(m.value2);
            if (count <= 0 || count > kForgeMost) return refuse(FortressError::ForgeCount);
            const double factor = host.FortressGuildHasPosition(g->id, position) ? 0.85 : 1.0;
            const std::uint64_t gold = std::uint64_t(std::floor(double(row->gold) * count * factor));
            const std::uint32_t points = std::uint32_t(std::floor(double(row->guildPoints) * count * factor));
            const std::int64_t seconds = std::int64_t(std::floor(double(row->minutes) * count * config.forgeSecondsPerMinute * factor));
            if (a.gold < gold) return refuse(FortressError::Gold);
            if (g->points < points) return refuse(FortressError::GuildPoints);
            // Begun now (the forge busy meanwhile), its row with the gold, the guild's points and their history in one
            // transaction; taken back if it does not stand.
            const FortressForgeRow r{id, row->item, count, false, unixNow, unixNow + seconds};
            f->forges.push_back(r);
            host.FortressPay(
                a.character, gold, g->id, points,
                [r, guild = g->id, name = a.name, points](Database& tx) -> std::optional<std::uint64_t> {
                    if (!InsertFortressForge(tx, r) || !AddGpHistory(tx, guild, name, points, game::kGpReasonForge)) return std::nullopt;
                    return 0;
                },
                [this, a, r, action = m.action, npc = m.npc, trainer, gold, points, seconds](bool ok, std::uint64_t) {
                    Fortress* f = Find(r.fortress);
                    if (f == nullptr) return;
                    if (!ok) {
                        std::erase_if(f->forges, [&](const FortressForgeRow& x) { return x.item == r.item && x.start == r.start; });
                        return ManageAnswer(a, action, FortressError::Gold, r.fortress);
                    }
                    core::LogInfo("fortress war: {} starts {} x {} at {}'s {} ({} gold, {} GP, {} s)", a.name, r.amount, r.item, f->spec.codename,
                                  trainer ? "trainer" : "smith", gold, points, seconds);
                    ManageAnswer(a, action, FortressError::None, r.fortress, r.finish);
                    // Its forge as it stands now (the query's answer).
                    proto::FortressForge told{npc, std::uint8_t(trainer ? 1 : 0), f->spec.id};
                    if (FortressForgeRow* x = Running(*f, trainer)) {
                        told.has = true;
                        told.item = x->item;
                        told.count = std::uint16_t(x->amount);
                        told.done = x->finished;
                        told.finishes = x->finish;
                    }
                    host.FortressSend(a.peer, proto::FortressForge::kId, Bytes(told));
                });
            return;
        }
        // Cancel and collect: the holder and the position, a work of the side (0x2819), the work's item named (3).
        if (const FortressError e = holderRefusal(); e != FortressError::None) return refuse(e);
        if (running == nullptr) return refuse(FortressError::ForgeNone);
        if (running->item != m.value) return refuse(FortressError::NoFortress);
        const FortressForgeRow r = *running;
        if (op == 2) {
            // Cancel: nothing given back.
            std::erase_if(f->forges, [&](const FortressForgeRow& x) { return x.item == r.item; });
            Write([r](Database& db) { DeleteFortressForge(db, r.fortress, r.item); });
            ManageAnswer(a, m.action, FortressError::None, id, r.item);
            return tell();
        }
        // Collect (`_SiegeFortressCollectForgedItem`'s rules): done (0x281A), not more than is left nor than one stack
        // (MaxStack; 0x281B; ours: none refused too), one stack into an empty bag place (0x281C).
        if (!running->finished) return refuse(FortressError::ForgeNotDone);
        const int count = int(m.value2);
        const std::uint32_t stack = host.FortressItemMaxStack(running->item);
        if (count <= 0 || count > running->amount || (stack != 0 && std::uint32_t(count) > stack)) return refuse(FortressError::ForgeTooMany);
        if (!host.FortressGiveStack(a.peer, running->item, std::uint32_t(count))) return refuse(FortressError::ForgeBagFull);
        running->amount -= count;
        const FortressForgeRow left = *running;
        if (left.amount <= 0) {
            std::erase_if(f->forges, [&](const FortressForgeRow& x) { return x.item == left.item; });
            Write([left](Database& db) { DeleteFortressForge(db, left.fortress, left.item); });
        } else {
            Write([left](Database& db) { SaveFortressForge(db, left); });
        }
        core::LogInfo("fortress war: {} collects {} x {} at {}", a.name, count, left.item, f->spec.codename);
        ManageAnswer(a, m.action, FortressError::None, id, count);
        tell();
    }
    // The forges' clock: a work done at its time, the holder's guild told "Production of [%s] is completed.".
    void TickForges() {
        for (Fortress& f : forts) {
            if (!f.spec.local) continue;
            for (FortressForgeRow& r : f.forges) {
                if (r.finished || unixNow < r.finish) continue;
                r.finished = true;
                const FortressForgeRow done = r;
                Write([done](Database& db) { SaveFortressForge(db, done); });
                if (const std::uint32_t holder = f.Controller(); holder != 0) {
                    SendGuild(holder, proto::FortressNotice::kId, Bytes(proto::FortressNotice{proto::FortressNoticeKind::ForgeDone, f.spec.id, r.item}));
                }
            }
        }
    }
    // --- The Cave of Discipline -------------------------------------------------------------
    bool dungeonAllowed = true; // the manager's flag: cleared 30 minutes before a war (kind 1), set again at its end (kind 6)
    const SiegeDungeonRow* DungeonRow(std::uint32_t fortress) const {
        for (const SiegeDungeonRow& r : config.dungeons) {
            if (r.fortress == fortress) return &r;
        }
        return nullptr;
    }
    // Local wall time as Unix seconds (`CreatedDungeonTime` is a local date), and its day.
    static std::int64_t LocalWall(std::int64_t t) {
        std::time_t tt = std::time_t(t);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &tt);
        return std::int64_t(_mkgmtime(&local));
#else
        localtime_r(&tt, &local);
        return std::int64_t(timegm(&local));
#endif
    }
    // The day's creations start again on the first query or creation of a new local day.
    void DungeonDay(Fortress& f) {
        const SiegeDungeonRow* row = DungeonRow(f.spec.id);
        if (row == nullptr || f.row.dungeonCount > row->maxCreate) return;
        if (f.row.dungeonTime != 0 && f.row.dungeonTime / 86400 == LocalWall(unixNow) / 86400) return;
        if (f.row.dungeonCount == 0) return;
        f.row.dungeonCount = 0;
        const std::uint32_t id = f.spec.id;
        Write([id](Database& db) { SaveFortressDungeon(db, id, 0, 0); });
    }
    void TellDungeon(const FortressActor& a, std::uint32_t npc, const Fortress& f) {
        proto::FortressDungeon d{npc, f.spec.id, std::uint8_t(std::clamp(f.row.dungeonCount, 0, 255)), f.dungeonLayer != 0};
        d.players = std::uint8_t(std::min<std::size_t>(255, f.dungeonLayer != 0 ? host.FortressDungeonInside(f.dungeonLayer) : 0));
        d.admitted = f.dungeonLayer != 0 && host.FortressDungeonReadmits(a.peer, f.dungeonLayer);
        host.FortressSend(a.peer, proto::FortressDungeon::kId, Bytes(d));
    }
    void DungeonAction(const FortressActor& a, const proto::FortressManage& m) {
        using A = proto::FortressAction;
        const auto refuse = [&](FortressError e) { ManageAnswer(a, m.action, e, m.fortress); };
        if (m.action < A::DungeonQuery || m.action > A::DungeonLeave) return refuse(FortressError::NoFortress);
        const std::optional<FortressGuild> g = host.FortressGuildOf(a.character);
        if (m.action == A::DungeonLeave) {
            // Leave (0x20): the Cave's teleporter in reach (3), a living player, the holder's guild or union (0x283D: the
            // union ids compared with no test for none, as the original's), in its dungeon; out to the character's recall
            // point (`_Char.TelRegion`, set to the entry's place when it entered; none: code 5).
            if (const std::string code = host.FortressNpc(m.npc); code.find("SIEGE_DUNGEON_TELEPORT") == std::string::npos) return refuse(FortressError::NoFortress);
            if (const double d = host.FortressDistance(a.entity, m.npc); d < 0 || d > game::kFortressNpcReach) return refuse(FortressError::NoFortress);
            if (a.dead) return refuse(FortressError::Failed);
            const std::uint32_t layer = host.FortressDungeonOf(a.peer);
            for (Fortress& f : forts) {
                if (f.dungeonLayer == 0 || f.dungeonLayer != layer) continue;
                const std::uint32_t holder = f.Controller(), holderUnion = AllianceOf(holder);
                if ((g ? g->id : 0u) != holder && (g ? g->alliance : 0u) != holderUnion) return refuse(FortressError::NotUnion);
                if (!host.FortressDungeonExit(a.peer)) return refuse(FortressError::NoRecallPoint);
                return ManageAnswer(a, m.action, FortressError::None, f.spec.id);
            }
            return refuse(FortressError::DungeonClosed);
        }
        Fortress* f = AtManager(a, m.npc);
        if (f == nullptr) return refuse(FortressError::NoFortress);
        if (!g) return refuse(FortressError::NotInGuild);
        const std::uint32_t holder = f->Controller(), holderUnion = AllianceOf(holder);
        const bool ownGuild = g->id == holder;
        const bool ownSide = ownGuild || (holderUnion != 0 && g->alliance == holderUnion);
        const SiegeDungeonRow* row = DungeonRow(f->spec.id);
        if (m.action == A::DungeonQuery) {
            // Query (0x1D): the holder's guild or union (0x283D), a dungeon row (0x2842).
            if (!ownSide) return refuse(FortressError::NotUnion);
            if (row == nullptr) return refuse(FortressError::NoDungeon);
            DungeonDay(*f);
            return TellDungeon(a, m.npc, *f);
        }
        if (m.action == A::DungeonCreate) {
            // Create (0x1E): the holder's guild (0x2806), position 1, 2 or 4 (0x283E), a row and world (0x2842), none
            // open (0x2844), under MaxCreateCount (0x2843), the manager's flag (0x284B); free.
            if (!ownGuild) return refuse(FortressError::NotHolder);
            if ((host.FortressPositionOf(a.character) & (game::kPositionCommander | game::kPositionDeputy | game::kPositionWar)) == 0) {
                return refuse(FortressError::DungeonPosition);
            }
            if (row == nullptr) return refuse(FortressError::NoDungeon);
            if (f->dungeonLayer != 0) return refuse(FortressError::DungeonOpen);
            DungeonDay(*f);
            if (f->row.dungeonCount >= row->maxCreate) return refuse(FortressError::DungeonCount);
            if (!dungeonAllowed) return refuse(FortressError::DungeonTime);
            const std::uint32_t layer = host.FortressDungeonOpen(a.name);
            if (layer == 0) return refuse(FortressError::DungeonCreate);
            f->dungeonLayer = layer;
            ++f->row.dungeonCount;
            f->row.dungeonTime = LocalWall(unixNow);
            const std::uint32_t id = f->spec.id;
            const int count = f->row.dungeonCount;
            const std::int64_t at = f->row.dungeonTime;
            Write([id, count, at](Database& db) { SaveFortressDungeon(db, id, count, at); });
            SendGuild(holder, proto::FortressNotice::kId, Bytes(proto::FortressNotice{proto::FortressNoticeKind::DungeonCreated, id}));
            core::LogInfo("fortress war: {} opens {}'s Cave of Discipline (layer {}, {} today)", a.name, f->spec.codename, layer, count);
            ManageAnswer(a, m.action, FortressError::None, id);
            return TellDungeon(a, m.npc, *f);
        }
        // Enter (0x1F): the holder's guild or union (0x283D), one open (0x2847), room (0x2849), the flag (0x284B); a
        // first entry pays the gold (0x280F) and the guild's GP (0x280E; `_SiegeFortressEntryDungeon`); free again when
        // their latest instance entry is this Cave's layer.
        if (!ownSide) return refuse(FortressError::NotUnion);
        if (f->dungeonLayer == 0 || !host.FortressDungeonStands(f->dungeonLayer)) return refuse(FortressError::DungeonClosed);
        if (host.FortressDungeonInside(f->dungeonLayer) >= host.FortressDungeonMaxUsers()) return refuse(FortressError::DungeonFull);
        if (!dungeonAllowed) return refuse(FortressError::DungeonTime);
        // The world's entry conditions (a job suit, the free-PvP cape, a ride or transport, a stall): refused before any price.
        if (!host.FortressDungeonFree(a.peer)) return refuse(FortressError::DungeonPay);
        if (!host.FortressDungeonReadmits(a.peer, f->dungeonLayer)) {
            const std::uint64_t gold = row != nullptr ? row->gold : 0;
            const std::uint32_t points = row != nullptr ? row->guildPoints : 0;
            if (a.gold < gold) return refuse(FortressError::Gold);
            if (g->points < points) return refuse(FortressError::GuildPoints);
            // The gold, the guild's points and their history in one transaction; in once it stands.
            host.FortressPay(
                a.character, gold, g->id, points,
                [guild = g->id, name = a.name, points](Database& tx) -> std::optional<std::uint64_t> {
                    if (!AddGpHistory(tx, guild, name, points, game::kGpReasonDungeon)) return std::nullopt;
                    return 0;
                },
                [this, a, action = m.action, id = f->spec.id](bool ok, std::uint64_t) {
                    Fortress* f = Find(id);
                    if (f == nullptr) return;
                    if (!ok || f->dungeonLayer == 0 || !host.FortressDungeonAdmit(a.peer, f->dungeonLayer)) return ManageAnswer(a, action, FortressError::DungeonPay, id);
                    core::LogInfo("fortress war: {} enters {}'s Cave of Discipline", a.name, f->spec.codename);
                    ManageAnswer(a, action, FortressError::None, id);
                });
            return;
        }
        if (!host.FortressDungeonAdmit(a.peer, f->dungeonLayer)) return refuse(FortressError::DungeonPay);
        core::LogInfo("fortress war: {} enters {}'s Cave of Discipline", a.name, f->spec.codename);
        ManageAnswer(a, m.action, FortressError::None, f->spec.id);
    }
    // A Cave ended by its time or its emptiness: 0x385F 0x16 to the holder's union.
    void TickDungeons() {
        for (Fortress& f : forts) {
            if (f.dungeonLayer == 0 || host.FortressDungeonStands(f.dungeonLayer)) continue;
            core::LogInfo("fortress war: {}'s Cave of Discipline closes", f.spec.codename);
            f.dungeonLayer = 0;
            if (const std::uint32_t holder = f.Controller(); holder != 0) {
                SendGuild(holder, proto::FortressNotice::kId, Bytes(proto::FortressNotice{proto::FortressNoticeKind::DungeonClosed, f.spec.id}));
            }
        }
    }

    // A barricade taken down by its guild (0x17, a barricade 0x282B, the own guild's 0x282C, position 1, 2 or
    // 4 0x282F); guards and monsters (0x16) come with them.
    void Remove(const FortressActor& a, const proto::FortressRemove& m) {
        // A guard or a summoned monster dismissed (0x16): a guard (0x282A), the own guild's (0x282C), position 1
        // or 4 (0x282E); the answer has no body (`COMMON_DISMISS_COMPLETE`).
        Fortress* of = nullptr;
        if (const Fortress::Object* o = ObjectOf(m.object, &of); o != nullptr && o->kind != StructureKind::Flag) {
            const auto dismiss = [&](FortressError e) { Answer(a, proto::FortressAction::DismissObject, e, m.fortress, proto::FortressSide::Attack); };
            const std::optional<FortressGuild> g = host.FortressGuildOf(a.character);
            if (!g || g->id != o->owner) return dismiss(FortressError::NotOwnObject);
            if ((host.FortressPositionOf(a.character) & (game::kPositionCommander | game::kPositionWar)) == 0) return dismiss(FortressError::DismissRole);
            core::LogInfo("fortress war: {} dismisses the {} {}", a.name, o->kind == StructureKind::Guard ? "guard" : "monster", m.object);
            const std::uint32_t fortress = of->spec.id;
            RemoveObject(m.object);
            return Answer(a, proto::FortressAction::DismissObject, FortressError::None, fortress, proto::FortressSide::Attack);
        }
        const auto it = byEntity.find(m.object);
        const auto refuse = [&](FortressError e) { Answer(a, proto::FortressAction::RemoveBarricade, e, m.fortress, proto::FortressSide::Attack); };
        if (it == byEntity.end()) return refuse(FortressError::NotBarricade);
        Fortress& f = forts[it->second.first];
        Structure& s = f.structures[it->second.second];
        if (f.Spot(s).kind != StructureKind::Barricade) return refuse(FortressError::NotBarricade);
        const std::optional<FortressGuild> g = host.FortressGuildOf(a.character);
        if (!g || g->id != s.owner) return refuse(FortressError::NotOwnObject);
        if ((host.FortressPositionOf(a.character) & (game::kPositionCommander | game::kPositionDeputy | game::kPositionWar)) == 0) {
            return refuse(FortressError::RemoveRole);
        }
        const std::uint32_t zone = f.Spot(s).eventId;
        Clear(f, s);
        core::LogInfo("fortress war: {} takes down the barricade at {}'s zone {}", a.name, f.spec.codename, zone);
        Answer(a, proto::FortressAction::RemoveBarricade, FortressError::None, f.spec.id, proto::FortressSide::Attack, zone);
    }

    // Upgrade, in the original's order.
    void Upgrade(const FortressActor& a, const proto::FortressUpgrade& m) {
        const auto refuse = [&](FortressError e) { Answer(a, proto::FortressAction::Upgrade, e, m.fortress, proto::FortressSide::Attack); };
        Fortress* f = AtAide(a, m.npc);
        if (f == nullptr) return refuse(FortressError::WrongNpc);
        Structure* s = AtZone(*f, m.zone);
        const StructureReference* ref = s != nullptr ? Reference(s->refObjId) : nullptr;
        if (s == nullptr) return refuse(FortressError::NoFortress);
        if (ref == nullptr) return refuse(FortressError::NoStructure);
        const StructureKind kind = f->Spot(*s).kind;
        if (kind == StructureKind::Barricade) return refuse(FortressError::NoStructure);
        if (warOn && f->atWar && kind != StructureKind::Headquarters) return refuse(FortressError::InWar);
        if (m.branch > 3) return refuse(FortressError::UpgradeBranch);
        if (m.fortress != 0 && m.fortress != f->spec.id) return refuse(FortressError::WrongFortress);
        const bool building = kind == StructureKind::Defensive && Foundation(*s); // Build: a foundation is never "whole"
        if (!building && (s->state == kStructDestroyed || s->hp < std::int64_t(MaxHp(f->Spot(*s), s->refObjId)))) return refuse(FortressError::UpgradeNotWhole);

        const StructureReference* next = ref->upgrades[m.branch].empty() ? nullptr : ReferenceNamed(ref->upgrades[m.branch]);
        if (next == nullptr) return refuse(FortressError::NoFortress);
        const std::optional<FortressGuild> g = host.FortressGuildOf(a.character);
        const std::uint8_t position = host.FortressPositionOf(a.character);
        // A member with position 1 or 4 (0x2833), of any guild: the original asks nothing of the holder (its client
        // offers the button to the holder's members only).
        if (!g || (position & (game::kPositionCommander | game::kPositionWar)) == 0) return refuse(FortressError::UpgradeRole);
        if (a.level < next->reqLevel) return refuse(FortressError::UpgradeLevel);
        if (a.gold < ref->gold) return refuse(FortressError::UpgradeGold);
        if (std::optional<FortressGuild> held = host.FortressGuildById(g->id); !held) return refuse(FortressError::UpgradePoints);
        if (unixNow < s->madeAt + std::int64_t(ref->minutes) * 60) return refuse(FortressError::UpgradeWait);
        if (g->points < ref->guildPoints) return refuse(FortressError::UpgradePoints);
        if (s->paying) return refuse(FortressError::UpgradeGold);
        const std::size_t fi = std::size_t(f - forts.data()), si = std::size_t(s - f->structures.data());
        // The gold, the guild's points, their history and the structure's new row in one transaction; raised once it stands.
        s->paying = true;
        const std::uint32_t nextId = next->refObjId, from = s->refObjId;
        const std::int64_t nextHp = MaxHp(f->Spot(*s), nextId);
        const FortressStructRow row{f->spec.id, s->owner, f->Spot(*s).eventId, nextId, nextHp, kStructStanding};
        host.FortressPay(
            a.character, ref->gold, g->id, ref->guildPoints,
            [row, guild = g->id, name = a.name, points = ref->guildPoints](Database& tx) -> std::optional<std::uint64_t> {
                if (!SaveFortressStruct(tx, row)) return std::nullopt;
                if (points > 0 && !AddGpHistory(tx, guild, name, points, game::kGpReasonStructure)) return std::nullopt;
                return 0;
            },
            [this, a, fi, si, from, nextId, nextHp, npc = m.npc, zone = m.zone, made = unixNow](bool ok, std::uint64_t) {
                if (fi >= forts.size() || si >= forts[fi].structures.size()) return;
                Fortress& f = forts[fi];
                Structure& s = f.structures[si];
                s.paying = false;
                if (!ok || s.refObjId != from) return Answer(a, proto::FortressAction::Upgrade, FortressError::UpgradeGold, f.spec.id, proto::FortressSide::Attack, zone);
                Fell(s);
                s.refObjId = nextId;
                s.hp = nextHp;
                s.state = kStructStanding;
                s.madeAt = made;
                Stand(fi, si);
                BlockZone(f, s);
                const StructureReference* to = Reference(nextId);
                core::LogInfo("fortress war: {} upgrades {}'s structure {} to {}", a.name, f.spec.codename, zone, to != nullptr ? to->codename : std::string());
                Answer(a, proto::FortressAction::Upgrade, FortressError::None, f.spec.id, proto::FortressSide::Attack, zone);
                StructuresInfo(a, {npc, f.spec.id});
            });
    }

    void Apply(const FortressActor& a, const proto::FortressApply& m) {
        Fortress* f = Find(m.fortress);
        const std::optional<FortressGuild> g = host.FortressGuildOf(a.character);
        game::FortressApplyFacts facts;
        facts.requestsOpen = requestsOpen;
        facts.fortressKnown = f != nullptr;
        facts.atOfficial = f != nullptr && AtOfficial(a, m.npc, f);
        facts.guild = g ? g->id : 0;
        facts.side = m.side;
        if (g) {
            facts.guildHoldsFortress = std::any_of(forts.begin(), forts.end(), [&](const Fortress& o) { return o.row.guild == g->id; });
            facts.ownerOfThis = f != nullptr && f->row.guild == g->id;
            const std::uint32_t ownerAlliance = f != nullptr ? AllianceOf(f->row.guild) : 0;
            facts.alliedWithOwner = ownerAlliance != 0 && g->alliance == ownerAlliance;
            facts.master = g->master;
            facts.guildLevel = g->level;
            facts.members = g->members;
            facts.applied = RequestOf(g->id) != nullptr;
        }
        facts.gold = a.gold;
        facts.fee = f != nullptr ? f->spec.fee : 0;
        FortressError error = game::ApplyRefusal(facts);
        if (error != FortressError::None) return Answer(a, proto::FortressAction::Apply, error, m.fortress, m.side);
        // The application stands now (a second waits), its row and the fee (an attacker's) in one transaction, as
        // `_SiegeFortressRequestSiege` takes them (-1003 without the gold); taken back if it does not stand.
        const FortressRequestRow row{f->spec.id, g->id, std::uint8_t(m.side)};
        f->requests.push_back(row);
        const std::uint64_t fee = m.side == proto::FortressSide::Attack ? facts.fee : 0;
        host.FortressPay(
            a.character, fee, 0, 0,
            [row](Database& tx) -> std::optional<std::uint64_t> {
                if (!InsertFortressRequest(tx, row)) return std::nullopt;
                return 0;
            },
            [this, a, row, side = m.side, name = g->name](bool ok, std::uint64_t) {
                Fortress* f = Find(row.fortress);
                if (f == nullptr) return;
                if (!ok) {
                    std::erase_if(f->requests, [&](const FortressRequestRow& r) { return r.guild == row.guild && r.fortress == row.fortress; });
                    return Answer(a, proto::FortressAction::Apply, FortressError::Gold, row.fortress, side);
                }
                Publish(kBusRequest, Bytes(RequestWire{row.fortress, row.guild, row.type, true}));
                core::LogInfo("fortress war: guild {} applies to {} {} ({})", name, side == proto::FortressSide::Attack ? "take" : "defend",
                              f->spec.codename, a.name);
                Answer(a, proto::FortressAction::Apply, FortressError::None, row.fortress, side);
            });
    }

    void Cancel(const FortressActor& a, const proto::FortressCancel& m) {
        Fortress* f = Find(m.fortress);
        const std::optional<FortressGuild> g = host.FortressGuildOf(a.character);
        game::FortressCancelFacts facts;
        facts.requestsOpen = requestsOpen;
        facts.fortressKnown = f != nullptr;
        facts.atOfficial = f != nullptr && AtOfficial(a, m.npc, f);
        facts.guild = g ? g->id : 0;
        facts.fortress = m.fortress;
        proto::FortressSide side = proto::FortressSide::Attack;
        if (g) {
            facts.master = g->master;
            facts.ownerOfThis = f != nullptr && f->row.guild == g->id;
            if (const FortressRequestRow* r = RequestOf(g->id)) {
                facts.appliedFortress = r->fortress;
                side = proto::FortressSide(r->type);
            }
        }
        const FortressError error = game::CancelRefusal(facts);
        if (error == FortressError::None) {
            std::erase_if(f->requests, [&](const FortressRequestRow& r) { return r.guild == g->id; });
            const std::uint32_t fortress = f->spec.id, guild = g->id;
            Publish(kBusRequest, Bytes(RequestWire{fortress, guild, std::uint8_t(side), false}));
            // In the master's order of saves, as the application's insert (FortressPay): a cancel and a new application
            // right after it are written in that order.
            host.FortressPay(
                a.character, 0, 0, 0,
                [fortress, guild](Database& tx) -> std::optional<std::uint64_t> {
                    if (!DeleteFortressRequest(tx, fortress, guild)) return std::nullopt;
                    return 0;
                },
                [](bool, std::uint64_t) {});
            core::LogInfo("fortress war: guild {} cancels its application to {}", g->name, f->spec.codename);
        }
        Answer(a, proto::FortressAction::Cancel, error, m.fortress, side);
    }
};

Fortresses::Fortresses(FortressHost& host, std::vector<FortressSpec> specs, FortressConfig config) : s_(std::make_unique<State>(host, std::move(config))) {
    for (FortressSpec& spec : specs) {
        State::Fortress f;
        f.spec = std::move(spec);
        f.row.id = f.spec.id;
        s_->forts.push_back(std::move(f));
    }
}

Fortresses::~Fortresses() = default;

void Fortresses::Load(FortressState state) {
    State& s = *s_;
    for (std::size_t fi = 0; fi < s.forts.size(); ++fi) {
        State::Fortress& f = s.forts[fi];
        const auto row = std::find_if(state.fortresses.begin(), state.fortresses.end(), [&](const FortressRow& r) { return r.id == f.spec.id; });
        if (row != state.fortresses.end()) f.row = *row;
        for (const FortressRequestRow& r : state.requests) {
            if (r.fortress == f.spec.id) f.requests.push_back(r);
        }
        for (const FortressStoneRow& r : state.stones) {
            if (r.fortress == f.spec.id && r.damage > 0) f.stone[r.guild] = std::uint64_t(r.damage);
        }
        // Each structure's row, or a new one (owner none, full health, standing).
        std::vector<FortressStructRow> missing;
        for (std::size_t si = 0; si < f.spec.structures.size(); ++si) {
            const FortressSpec::Structure& spot = f.spec.structures[si];
            State::Structure st;
            st.spot = si;
            const auto it = std::find_if(state.structures.begin(), state.structures.end(), [&](const FortressStructRow& r) { return r.eventId == spot.eventId; });
            if (it != state.structures.end()) {
                st.refObjId = it->refObjId != 0 ? it->refObjId : spot.refObjId;
                st.owner = it->owner;
                st.hp = it->hp;
                st.state = it->state;
                st.madeAt = it->madeAt;
                if (BuiltByItem(spot.kind) && st.state != kStructBuilt) {
                    // An empty zone (or a row made before the items: ours, never built).
                    st.refObjId = spot.refObjId;
                    st.owner = 0;
                    st.hp = 0;
                    st.state = kStructEmpty;
                }
            } else if (BuiltByItem(spot.kind)) {
                st.refObjId = spot.refObjId;
                st.state = kStructEmpty;
                missing.push_back({f.spec.id, 0, spot.eventId, spot.refObjId, 0, kStructEmpty});
            } else {
                st.refObjId = spot.refObjId;
                st.owner = f.row.guild;
                st.hp = spot.maxHp;
                missing.push_back({f.spec.id, st.owner, spot.eventId, spot.refObjId, std::int64_t(spot.maxHp), kStructStanding});
            }
            f.structures.push_back(st);
        }
        if (f.spec.local && (row == state.fortresses.end() || !missing.empty())) {
            const std::uint32_t id = f.spec.id;
            s.Write([id, missing](Database& db) {
                std::string error;
                if (!InsertFortress(db, id, missing, error)) core::LogWarning("fortress war: fortress {}'s rows: {}", id, error);
            });
        }
        for (std::size_t si = 0; si < f.structures.size(); ++si) {
            s.Stand(fi, si);
            s.BlockZone(f, f.structures[si]);
        }
        // The war's objects (`_SiegeFortressObject`, read at the start): the guards stand again; a flag's or a
        // summoned monster's time is not kept (ours: they are gone, their rows deleted).
        for (const FortressObjectRow& r : state.objects) {
            if (r.fortress != f.spec.id || !f.spec.local) continue;
            const auto character = s.host.FortressCharacterRow(r.refObjId);
            const StructureKind kind = character ? character->kind : StructureKind::None;
            if (kind != StructureKind::Guard) {
                const std::int64_t id = r.id;
                s.Write([id](Database& db) { DeleteFortressObject(db, id); });
                continue;
            }
            State::Fortress::Object o;
            *o.row = r.id;
            o.refObjId = r.refObjId;
            o.owner = r.owner;
            o.kind = kind;
            o.maxHp = s.config.structureHp != 0 ? s.config.structureHp : std::max<std::uint32_t>(1, character->maxHp);
            o.hp = std::clamp<std::int64_t>(r.hp, 1, o.maxHp);
            o.at = {r.region, r.x, r.y, r.z};
            o.yaw = r.direction;
            o.ownerLevel = r.ownerLevel;
            s.StandObject(fi, std::move(o));
        }
        for (const FortressForgeRow& r : state.forges) {
            if (r.fortress == f.spec.id) f.forges.push_back(r);
        }
        // The battle records (`_SiegeFortressBattleRecord`): a restart in a war keeps the kills and the ranks.
        for (const FortressBattleRecordRow& r : state.records) {
            if (r.fortress == f.spec.id) f.records[r.character] = {r.kills, r.deaths, r.rank};
        }
        core::LogInfo("fortress war: {} ({}, owner {}, {} applications, {} structures standing)", f.spec.codename,
                      f.spec.local ? "run here" : "another server's", f.row.guild, f.requests.size(),
                      std::count_if(f.structures.begin(), f.structures.end(), [](const State::Structure& x) { return x.entity != 0; }));
    }
}

void Fortresses::Entered(const FortressActor& actor) {
    State& s = *s_;
    s.host.FortressSend(actor.peer, proto::FortressList::kId, Bytes(s.List()));
    // The holder's members are told its hired NPCs (0x385F 0x12 on entering).
    if (const std::optional<FortressGuild> g = s.host.FortressGuildOf(actor.character)) {
        for (const State::Fortress& f : s.forts) {
            if (f.row.guild == g->id) {
                s.host.FortressSend(actor.peer, proto::FortressNotice::kId, Bytes(proto::FortressNotice{proto::FortressNoticeKind::Hired, f.spec.id, f.row.npcHired}));
            }
        }
    }
    // In a war: his score so far (the original sends 0x11 on entering).
    for (const State::Fortress& f : s.forts) {
        const auto r = f.records.find(actor.character);
        if (!f.atWar || r == f.records.end()) continue;
        // His rank's buff again (a restart or a new login in the war keeps the rank: the records are read back).
        if (const BattleRankRow* rank = f.spec.local ? s.Rank(r->second.rank) : nullptr) s.host.FortressRankBuff(actor.character, 0, rank->skill);
        s.host.FortressSend(actor.peer, proto::FortressScore::kId,

                            Bytes(proto::FortressScore{f.spec.id, std::uint32_t(r->second.kills), std::uint32_t(r->second.deaths), std::uint8_t(r->second.rank)}));
    }
}

bool Fortresses::Handle(const FortressActor& actor, proto::MessageId id, std::span<const std::byte> payload, bool devCommands) {
    State& s = *s_;
    switch (id) {
    case proto::FortressApplyOpen::kId: {
        proto::FortressApplyOpen m;
        if (!proto::Decode(payload, m)) return false;
        if (!s.AtOfficial(actor, m.npc, nullptr)) {
            s.Answer(actor, proto::FortressAction::Query, FortressError::WrongNpc, 0, proto::FortressSide::Attack);
            return true;
        }
        s.ApplyInfo(actor, m.npc);
        return true;
    }
    case proto::FortressApply::kId: {
        proto::FortressApply m;
        if (!proto::Decode(payload, m)) return false;
        s.Apply(actor, m);
        return true;
    }
    case proto::FortressCancel::kId: {
        proto::FortressCancel m;
        if (!proto::Decode(payload, m)) return false;
        s.Cancel(actor, m);
        return true;
    }
    case proto::FortressTaxQuery::kId: {
        proto::FortressTaxQuery m;
        if (!proto::Decode(payload, m)) return false;
        s.TaxQuery(actor, m.npc);
        return true;
    }
    case proto::FortressTaxRate::kId: {
        proto::FortressTaxRate m;
        if (!proto::Decode(payload, m)) return false;
        s.TaxRate(actor, m.npc, m.rate);
        return true;
    }
    case proto::FortressTaxLevy::kId: {
        proto::FortressTaxLevy m;
        if (!proto::Decode(payload, m)) return false;
        s.TaxLevy(actor, m.npc, m.amount);
        return true;
    }
    case proto::FortressStructuresQuery::kId: {
        proto::FortressStructuresQuery m;
        if (!proto::Decode(payload, m)) return false;
        s.StructuresInfo(actor, m);
        return true;
    }
    case proto::FortressRepair::kId: {
        proto::FortressRepair m;
        if (!proto::Decode(payload, m)) return false;
        s.Repair(actor, m);
        return true;
    }
    case proto::FortressBuild::kId: {
        proto::FortressBuild m;
        if (!proto::Decode(payload, m)) return false;
        s.Build(actor, m);
        return true;
    }
    case proto::FortressUpgrade::kId: {
        proto::FortressUpgrade m;
        if (!proto::Decode(payload, m)) return false;
        s.Upgrade(actor, m);
        return true;
    }
    case proto::FortressUseItem::kId: {
        proto::FortressUseItem m;
        if (!proto::Decode(payload, m)) return false;
        s.UseItem(actor, m);
        return true;
    }
    case proto::FortressRemove::kId: {
        proto::FortressRemove m;
        if (!proto::Decode(payload, m)) return false;
        s.Remove(actor, m);
        return true;
    }
    case proto::FortressManage::kId: {
        proto::FortressManage m;
        if (!proto::Decode(payload, m)) return false;
        s.Manage(actor, m);
        return true;
    }
    case proto::FortressPulley::kId: {
        proto::FortressPulley m;
        if (!proto::Decode(payload, m)) return false;
        s.Pulley(actor, m);
        return true;
    }
    case proto::FortressReturn::kId: {
        proto::FortressReturn m;
        if (!proto::Decode(payload, m)) return false;
        s.Return(actor, m.fortress);
        return true;
    }
    case proto::FortressDev::kId: {
        proto::FortressDev m;
        if (!proto::Decode(payload, m)) return false;
        if (!devCommands) {
            core::LogInfo("fortress war: {} asked for a period switch; not a development server", actor.name);
            return true;
        }
        if (m.action == proto::FortressDevAction::GoTo) {
            // Beside a player, or the heart or the structure, or (entity ~0) the fortress's battle aide, (~1) its manager
            // (ours, for tries).
            const auto standing = s.byEntity.find(m.entity);
            if (m.entity == 0xFFFFFFFFu || m.entity == 0xFFFFFFFEu) {
                const State::Fortress* f = s.Find(m.fortress);
                const char* kind = m.entity == 0xFFFFFFFFu ? "_FORTRESS_BATTLEAIDE" : "_FORTRESS_MANAGER";
                const std::uint32_t aide = f != nullptr ? s.host.FortressNpcIn(kind, f->spec.regions) : 0u;
                if (const auto where = aide != 0 ? s.host.FortressEntityPosition(aide) : std::nullopt) {
                    const proto::FlatPosition at = proto::ToFlat(*where);
                    s.host.FortressTeleport(actor.entity, proto::FromFlat({at.x + 20.0, at.z}, where->y));
                }
                return true;
            }
            if (standing == s.byEntity.end() && m.entity != 0) {
                if (const auto where = s.host.FortressEntityPosition(m.entity)) {
                    const proto::FlatPosition at = proto::ToFlat(*where);
                    s.host.FortressTeleport(actor.entity, proto::FromFlat({at.x + 12.0, at.z}, where->y));
                }
                return true;
            }
            const State::Fortress* f = standing != s.byEntity.end() ? &s.forts[standing->second.first] : s.Find(m.fortress);
            const auto heart = f == nullptr ? std::vector<FortressSpec::Structure>::const_iterator{}
                               : standing != s.byEntity.end()
                                   ? f->spec.structures.begin() + std::ptrdiff_t(f->structures[standing->second.second].spot)
                               : m.zone != 0 ? std::find_if(f->spec.structures.begin(), f->spec.structures.end(),
                                                            [&](const FortressSpec::Structure& st) { return st.eventId == m.zone; })
                                             : std::find_if(f->spec.structures.begin(), f->spec.structures.end(),
                                                            [](const FortressSpec::Structure& st) { return st.kind == StructureKind::Heart; });
            if (f != nullptr && heart != f->spec.structures.end()) {
                const proto::FlatPosition at = proto::ToFlat(heart->position);
                // In front of it, 30 clear of its footprint: a long one (a gate in its wall) across its short side, on the side the
                // player stands; the others on the player's side (south of the heart when none is asked for).
                const StructureShape shape = s.host.StructureShapeOf(heart->refObjId);
                const double cs = std::cos(double(heart->yaw)), sn = std::sin(double(heart->yaw));
                double lx = 0.0, lz = -1.0; // model space
                if (standing != s.byEntity.end()) {
                    if (const auto from = s.host.FortressEntityPosition(actor.entity)) {
                        const proto::FlatPosition a = proto::ToFlat(*from);
                        const double dx = a.x - at.x, dz = a.z - at.z;
                        lx = cs * dx + sn * dz;
                        lz = -sn * dx + cs * dz;
                    }
                }
                const double halfX = std::max(std::fabs(double(shape.minX)), std::fabs(double(shape.maxX)));
                const double halfZ = std::max(std::fabs(double(shape.minZ)), std::fabs(double(shape.maxZ)));
                double px = 0.0, pz = 0.0;
                if (halfX > 2.0 * halfZ) {
                    pz = (lz >= 0.0 ? 1.0 : -1.0) * (halfZ + 30.0);
                } else if (halfZ > 2.0 * halfX) {
                    px = (lx >= 0.0 ? 1.0 : -1.0) * (halfX + 30.0);
                } else {
                    const double length = std::max(1e-6, std::hypot(lx, lz));
                    const double clear = std::max(90.0, std::max(halfX, halfZ) + 30.0);
                    px = lx / length * clear;
                    pz = lz / length * clear;
                }
                s.host.FortressTeleport(actor.entity, proto::FromFlat({at.x + cs * px - sn * pz, at.z + sn * px + cs * pz}, heart->position.y));
                core::LogInfo("fortress war: {} goes to {} (developer)", actor.name, f->spec.codename);
            }
            return true;
        }
        core::LogInfo("fortress war: {} switches the periods ({}, developer)", actor.name, int(m.action));
        Dev(m.action);
        return true;
    }
    default: return false;
    }
}

void Fortresses::Tick(Clock::time_point now, std::int64_t unixNow) {
    State& s = *s_;
    s.now = now;
    s.unixNow = unixNow;
    if (s.cycleStart == 0) s.cycleStart = unixNow;
    // The periods are the keeper's; the others follow its word (BusMessage).
    if (!s.manual && s.config.periodKeeper) {
        const State::Periods p = s.PeriodsAt(unixNow);
        if (p.requests) s.OpenRequests();
        else s.CloseRequests();
        if (p.war) s.BeginWar();
        else s.EndWar();
        s.Warnings(p);
    }
    // The first time the periods are known (the keeper's own, or the keeper's word): outside a war, a fortress of this
    // server still held for now (a war cut by a stop) is finished with its holder as the owner (the original's
    // RestoreOccupation at the start).
    if (!s.restored && (s.config.periodKeeper || s.told)) {
        s.restored = true;
        for (std::size_t fi = 0; fi < s.forts.size() && !s.warOn; ++fi) {
            State::Fortress& f = s.forts[fi];
            if (!f.spec.local || f.row.tempGuild == 0) continue;
            core::LogInfo("fortress war: {} was held for now by guild {} with no war on: its war finished", f.spec.codename, f.row.tempGuild);
            f.atWar = true;
            s.Finish(fi);
            s.BroadcastList();
        }
    }
    s.TellTax();
    if (s.config.periodKeeper && now >= s.nextPeriods) {
        // Once a minute the others hear the periods again (a lost word is made good).
        s.nextPeriods = now + std::chrono::seconds(60);
        s.PublishPeriods();
    }
    for (State::Fortress& f : s.forts) {
        if (!f.spec.local) continue;
        if (f.atWar && f.evacuateAt && now >= *f.evacuateAt) {
            f.evacuateAt.reset();
            s.Evacuate(f, false);
        }
        if (f.atWar && f.heartOpenAt && now >= *f.heartOpenAt && !f.heartOpenTold) {
            f.heartOpenTold = true;
            s.Notice(proto::FortressNoticeKind::HeartOpen, f.spec.id);
        }
    }
    s.TickObjects();
    s.TickForges();
    s.TickDungeons();
    if (now >= s.nextFlush) {
        s.nextFlush = now + std::chrono::seconds(2);
        s.Flush();
    }
}

void Fortresses::Dev(proto::FortressDevAction action) {
    State& s = *s_;
    if (!s.config.periodKeeper && action != proto::FortressDevAction::GoTo) {
        // The periods are another server's: it switches them and tells everyone.
        s.Publish(kBusDev, Bytes(DevWire{std::uint8_t(action)}));
        return;
    }
    s.manual = true;
    switch (action) {
    case proto::FortressDevAction::StartRequests: s.OpenRequests(); break;
    case proto::FortressDevAction::EndRequests: s.CloseRequests(); break;
    case proto::FortressDevAction::StartWar: s.BeginWar(); break;
    case proto::FortressDevAction::EndWar: s.EndWar(); break;
    case proto::FortressDevAction::GoTo: break;
    case proto::FortressDevAction::StartTax:
        s.taxForced = true;
        s.PublishPeriods();
        break;
    case proto::FortressDevAction::EndTax:
        s.taxForced = false;
        s.PublishPeriods();
        break;
    }
}

bool Fortresses::TaxPeriod() const { return s_->TaxPeriod(); }

void Fortresses::Hello() { s_->Publish(kBusHello, {}); }

void Fortresses::BusMessage(const std::string& text) {
    State& s = *s_;
    const auto bytes = BusFromHex(text);
    if (!bytes) return;
    BusEnvelope e;
    if (!proto::Decode(*bytes, e) || e.from == s.host.FortressServerId()) return;
    s.fromBus = true;
    switch (e.kind) {
    case kBusHello:
        // A server has started: what this one keeps, as it is now.
        if (s.config.periodKeeper) s.Publish(kBusPeriods, Bytes(s.PeriodsNow()));
        for (const State::Fortress& f : s.forts) s.PublishRow(f);
        break;
    case kBusPeriods:
        if (const auto w = BusDecode<PeriodsWire>(e.payload); w && !s.config.periodKeeper) {
            s.told = *w;
            // The keeper's word, as each original game server takes its shard manager's.
            if (w->requests) s.OpenRequests();
            else s.CloseRequests();
            if (w->war) s.BeginWar();
            else s.EndWar();
            s.TellTax();
        }
        break;
    case kBusNotice:
        s.host.FortressBroadcast(proto::FortressNotice::kId, AsBytes(e.payload));
        break;
    case kBusRow:
        if (const auto w = BusDecode<RowWire>(e.payload)) {
            if (State::Fortress* f = s.Find(w->fortress); f != nullptr && !f->spec.local) {
                const bool changed = f->row.guild != w->guild || f->row.tempGuild != w->tempGuild || f->row.taxRatio != w->rate ||
                                     f->row.introduction != w->introduction;
                f->row.npcHired = w->hired;
                f->row.introduction = w->introduction;
                f->row.guild = w->guild;
                f->row.tempGuild = w->tempGuild;
                if (f->row.taxRatio != w->rate) s.host.FortressTaxRateChanged(f->spec.id, w->guild, w->rate);
                f->row.taxRatio = w->rate;
                if (changed) s.BroadcastList();
            }
        }
        break;
    case kBusRequest:
        if (const auto w = BusDecode<RequestWire>(e.payload)) {
            if (State::Fortress* f = s.Find(w->fortress)) {
                std::erase_if(f->requests, [&](const FortressRequestRow& r) { return r.guild == w->guild; });
                if (w->add) f->requests.push_back({w->fortress, w->guild, w->type});
            }
        }
        break;
    case kBusGuild:
        if (const auto w = BusDecode<GuildWire>(e.payload)) s.host.FortressSendGuild(w->guild, w->id, AsBytes(w->payload));
        break;
    case kBusDev:
        if (const auto w = BusDecode<DevWire>(e.payload); w && s.config.periodKeeper) {
            s.fromBus = false; // the keeper's own switch: told to everyone
            Dev(proto::FortressDevAction(w->action));
        }
        break;
    default: break;
    }
    s.fromBus = false;
}

bool Fortresses::IsStructure(std::uint32_t entity) const {
    if (s_->byEntity.contains(entity)) return true;
    const State::Fortress::Object* o = const_cast<State&>(*s_).ObjectOf(entity);
    return o != nullptr && o->kind == StructureKind::Flag; // a flag is hit as a structure; guards and monsters as monsters
}
bool Fortresses::IsWarObject(std::uint32_t entity) const { return s_->objectFort.contains(entity); }


std::array<double, 3> OutlineContact(const std::vector<std::array<float, 6>>& outline, double px, double pz) {
    double best = -1.0, bx = 0, bz = 0, by = 0;
    for (const auto& e : outline) {
        const double ex = double(e[3]) - e[0], ez = double(e[5]) - e[2];
        const double len2 = ex * ex + ez * ez;
        if (len2 <= 0.0) continue;
        const double t = ((px - e[0]) * ex + (pz - e[2]) * ez) / len2;
        if (t < 0.0 || t > 1.0) continue;
        const double fx = e[0] + t * ex, fz = e[2] + t * ez;
        const double d = std::hypot(px - fx, pz - fz);
        if (best < 0.0 || d < best) {
            best = d;
            bx = fx;
            bz = fz;
            by = e[1] + t * (double(e[4]) - e[1]);
        }
    }
    if (best >= 0.0) return {bx, by, bz};
    for (const auto& e : outline) {
        for (int k = 0; k < 2; ++k) {
            const double d = std::hypot(px - e[k * 3], pz - e[k * 3 + 2]);
            if (best < 0.0 || d < best) {
                best = d;
                bx = e[k * 3];
                by = e[k * 3 + 1];
                bz = e[k * 3 + 2];
            }
        }
    }
    return {bx, by, bz};
}

std::optional<proto::WorldPosition> Fortresses::NearestPoint(std::uint32_t entity, double x, double z) const {
    const State& s = *s_;
    const auto it = s.byEntity.find(entity);
    if (it == s.byEntity.end()) return std::nullopt;
    const State::Fortress& f = s.forts[it->second.first];
    const State::Structure& st = f.structures[it->second.second];
    const FortressSpec::Structure& spot = f.Spot(st);
    const StructureShape shape = s.host.StructureShapeOf(st.refObjId);
    // World to model space and back (x' = x cos - z sin, z' = x sin + z cos).
    const proto::FlatPosition c = proto::ToFlat(spot.position);
    const double cs = std::cos(double(spot.yaw)), sn = std::sin(double(spot.yaw));
    const double dx = x - c.x, dz = z - c.z;
    const double px = cs * dx + sn * dz, pz = -sn * dx + cs * dz;
    const auto back = [&](double lx, double lz, double ly) {
        return proto::FromFlat({c.x + cs * lx - sn * lz, c.z + sn * lx + cs * lz}, float(double(spot.position.y) + ly));
    };
    if (!shape.outline.empty()) {
        const std::array<double, 3> p = OutlineContact(shape.outline, px, pz);
        return back(p[0], p[2], p[1]);
    }
    if (shape.maxX <= shape.minX || shape.maxZ <= shape.minZ) return std::nullopt;
    return back(std::clamp(px, double(shape.minX), double(shape.maxX)), std::clamp(pz, double(shape.minZ), double(shape.maxZ)), 0.0);
}

bool Fortresses::FortressArea(std::uint16_t region) const {
    return std::any_of(s_->forts.begin(), s_->forts.end(), [&](const State::Fortress& f) { return f.InArea(region); });
}

bool Fortresses::WarArea(std::uint16_t region) const {
    return std::any_of(s_->forts.begin(), s_->forts.end(), [&](const State::Fortress& f) { return f.atWar && f.InArea(region); });
}

std::optional<proto::WorldPosition> Fortresses::RevivalPlace(std::int64_t character, std::uint16_t region) {
    State& s = *s_;
    const std::optional<FortressGuild> g = s.host.FortressGuildOf(character);
    if (!g) return std::nullopt;
    for (const State::Fortress& f : s.forts) {
        if (!f.atWar || !f.InArea(region)) continue;
        for (const State::Structure& st : f.structures) {
            if (f.Spot(st).kind != StructureKind::Headquarters || st.state != kStructBuilt || st.owner != g->id) continue;
            const proto::FlatPosition c = proto::ToFlat(f.Spot(st).position);
            std::uniform_real_distribution<double> angle(0.0, 6.283185307179586), reach(0.0, 250.0);
            const double t = angle(s.random), r = reach(s.random);
            return proto::FromFlat({c.x + std::cos(t) * r, c.z + std::sin(t) * r}, f.Spot(st).position.y);
        }
    }
    return std::nullopt;
}

std::string Fortresses::RevivalGate(std::int64_t character, std::uint16_t region) {
    State& s = *s_;
    for (const State::Fortress& f : s.forts) {
        if (!f.atWar || !f.InArea(region)) continue;
        const auto pick = [&](const std::vector<std::string>& gates) {
            // Only the gates that exist (three or four, by fortress).
            std::vector<std::string> real;
            for (const std::string& g : gates) {
                if (s.host.FortressHasPoint(g)) real.push_back(g);
            }
            return real.empty() ? std::string() : real[s.random() % real.size()];
        };
        const std::optional<FortressGuild> g = s.host.FortressGuildOf(character);
        const std::uint32_t holder = f.Controller();
        if (g && g->id == holder) return f.spec.revivalGate;
        const std::uint32_t holderUnion = holder != 0 ? s.AllianceOf(holder) : 0;
        if (g && holderUnion != 0 && g->alliance == holderUnion) return pick(f.spec.insideGates);
        return pick(f.spec.outsideGates);
    }
    return {};
}

FortressError Fortresses::MayHit(std::int64_t attacker, std::uint32_t structure) const {
    const State& s = *s_;
    const auto it = s.byEntity.find(structure);
    if (it == s.byEntity.end()) {
        // A war's object (a flag, a guard, a summoned monster): its guild's enemies in the war hit it.
        State::Fortress* of = nullptr;
        const State::Fortress::Object* o = const_cast<State&>(s).ObjectOf(structure, &of);
        if (o == nullptr) return FortressError::None;
        if (!s.warOn || !of->atWar) return FortressError::StructureOutOfWar;
        const std::optional<FortressGuild> g = s.host.FortressGuildOf(attacker);
        const game::WarSide me{g ? g->id : 0, g ? g->alliance : 0};
        if (const FortressError e = game::StructureRefusal(true, game::WarSide{o->owner, s.AllianceOf(o->owner)}, me); e != FortressError::None) return e;
        return s.Participant(*of, me.guild) ? FortressError::None : FortressError::NotParticipant;
    }
    const State::Fortress& f = s.forts[it->second.first];
    const State::Structure& st = f.structures[it->second.second];
    if (!s.warOn || !f.atWar) return FortressError::StructureOutOfWar;
    if (st.open) return FortressError::GateOpen; // an open gate is not hit
    const std::optional<FortressGuild> g = s.host.FortressGuildOf(attacker);
    const game::WarSide me{g ? g->id : 0, g ? g->alliance : 0};
    const game::WarSide owner{st.owner, s.AllianceOf(st.owner)};
    if (const FortressError e = game::StructureRefusal(true, owner, me); e != FortressError::None) return e;
    if (!s.Participant(f, me.guild)) return FortressError::NotParticipant;
    if (f.Spot(st).kind == StructureKind::Heart) {
        if (s.TowersStanding(f) > 0) return FortressError::HeartSealed;
        if (!f.heartOpenAt || s.now < *f.heartOpenAt) return FortressError::HeartReleasing;
    }
    return FortressError::None;
}

void Fortresses::StructureHit(std::uint32_t structure, std::int64_t attacker, std::uint64_t damage, std::uint32_t hpLeft) {
    State& s = *s_;
    const auto it = s.byEntity.find(structure);
    if (it == s.byEntity.end()) {
        if (State::Fortress::Object* o = s.ObjectOf(structure)) {
            o->hp = hpLeft;
            o->dirty = true;
        }
        return;
    }
    State::Fortress& f = s.forts[it->second.first];
    State::Structure& st = f.structures[it->second.second];
    st.hp = hpLeft;
    st.dirty = true;
    if (f.Spot(st).kind != StructureKind::Heart || damage == 0) return;
    // The heart's damage by guild (`_SiegeFortressStoneState`).
    const std::optional<FortressGuild> g = s.host.FortressGuildOf(attacker);
    if (!g) return;
    f.stone[g->id] += damage;
    f.stoneDirty.insert(g->id);
}

void Fortresses::StructureDestroyed(std::uint32_t structure, std::int64_t killer) {
    State& s = *s_;
    const auto it = s.byEntity.find(structure);
    if (it == s.byEntity.end()) {
        // A war's object felled: gone (its kill counts for the siege quests: step 9).
        if (State::Fortress::Object* o = s.ObjectOf(structure)) {
            core::LogInfo("fortress war: the {} {} of guild {} is destroyed", o->kind == StructureKind::Flag ? "flag" : "object", structure, o->owner);
            s.ObjectFelled(structure, killer);
            s.RemoveObject(structure);
        }
        return;
    }
    const std::size_t fi = it->second.first;
    State::Fortress& f = s.forts[fi];
    State::Structure& st = f.structures[it->second.second];
    const FortressSpec::Structure& spot = f.Spot(st);
    st.hp = 0;
    if (spot.kind == StructureKind::Heart) {
        s.QuestCredit(killer, 3); // the siege quests: 1 a tower, 2 a gate, 3 the heart, 5 a war camp
        s.Capture(fi, killer);
        return;
    }
    if (spot.kind == StructureKind::GuardTower) s.QuestCredit(killer, 1);
    if (spot.kind == StructureKind::Gate) s.QuestCredit(killer, 2);
    if (spot.kind == StructureKind::Headquarters) s.QuestCredit(killer, 5);
    if (BuiltByItem(spot.kind)) {
        // A barricade goes, a war camp is a foundation again; told as the original's 0x0B.
        const std::string owner = s.GuildName(st.owner);
        s.Clear(f, st);
        s.Notice(proto::FortressNoticeKind::StructureDestroyed, f.spec.id, 0, owner, spot.nameKey);
        return;
    }
    st.state = kStructDestroyed;
    st.dirty = false;
    s.SaveStructure(f, st);
    s.BlockZone(f, st);
    s.TellStructure(f, st); // it stays, as rubble, until repaired or rebuilt
    s.Notice(proto::FortressNoticeKind::StructureDestroyed, f.spec.id, 0, s.GuildName(st.owner), spot.nameKey);
    core::LogInfo("fortress war: {}'s structure {} destroyed", f.spec.codename, spot.eventId);
    // The last guard tower: the heart's seal lifts after a while (the fortress's world is told; the client: "You can
    // attack Fortress Heart in a minute").
    if (spot.kind == StructureKind::GuardTower && s.TowersStanding(f) == 0) {
        f.heartOpenAt = s.now + std::chrono::seconds(s.config.heartSealSeconds);
        f.heartOpenTold = false;
        s.Notice(proto::FortressNoticeKind::SealReleasing, f.spec.id, std::uint32_t(s.config.heartSealSeconds));
    }
}

bool Fortresses::Hostile(std::int64_t a, std::uint16_t regionA, std::int64_t b, std::uint16_t regionB) const {
    const State& s = *s_;
    if (!s.warOn || a == b) return false;
    for (const State::Fortress& f : s.forts) {
        if (!f.atWar || !f.InArea(regionA) || !f.InArea(regionB)) continue;
        const std::optional<FortressGuild> ga = s.host.FortressGuildOf(a), gb = s.host.FortressGuildOf(b);
        const game::WarSide sa{ga ? ga->id : 0, ga ? ga->alliance : 0}, sb{gb ? gb->id : 0, gb ? gb->alliance : 0};
        // Ours: only the war's guilds fight in its area (the original lets only them in).
        if (!s.Participant(f, sa.guild) || !s.Participant(f, sb.guild)) return false;
        return game::PlayersHostileInWar(true, sa, sb);
    }
    return false;
}

bool Fortresses::Fighting(std::int64_t character, std::uint16_t region) const {
    const State& s = *s_;
    if (!s.warOn) return false;
    const std::optional<FortressGuild> g = s.host.FortressGuildOf(character);
    if (!g) return false;
    for (const State::Fortress& f : s.forts) {
        if (f.atWar && f.InArea(region) && s.Participant(f, g->id)) return true;
    }
    return false;
}

bool Fortresses::MayEnter(std::int64_t character, std::uint16_t region) const {
    const State& s = *s_;
    if (!s.warOn) return true;
    const std::optional<FortressGuild> g = s.host.FortressGuildOf(character);
    for (const State::Fortress& f : s.forts) {
        if (f.atWar && f.InArea(region)) return g && s.Participant(f, g->id);
    }
    return true;
}

FortressError Fortresses::EntryRefusal(std::int64_t character, std::uint16_t region, bool fortressGate, bool jobSuit) const {
    const State& s = *s_;
    const auto it = std::find_if(s.forts.begin(), s.forts.end(), [&](const State::Fortress& f) { return f.InArea(region); });
    if (it == s.forts.end()) return FortressError::None;
    const State::Fortress& f = *it;
    const std::optional<FortressGuild> g = s.host.FortressGuildOf(character);
    // Any other gate into its world (the revival, charge and glory gates): the holder's side only.
    if (!fortressGate) return s.HolderSide(f, g) ? FortressError::None : FortressError::EntryNotMember;
    game::FortressEntryFacts facts;
    facts.jobSuit = jobSuit;
    facts.warOn = s.warOn && f.atWar;
    facts.guild = g ? g->id : 0;
    facts.participant = g && s.Participant(f, g->id);
    facts.holderSide = s.HolderSide(f, g);
    facts.held = f.Controller() != 0;
    facts.closed = f.openAgainAt && s.now < *f.openAgainAt;
    facts.maxAdmission = f.spec.maxAdmission;
    if (facts.warOn && facts.participant && f.spec.maxAdmission > 0) {
        // Each side inside (the original's world counts of its attackers and defenders).
        const auto side = [&](bool defenders) {
            return s.host.FortressCountIn(f.spec.regions, [&](std::int64_t c) {
                const std::optional<FortressGuild> o = s.host.FortressGuildOf(c);
                return o && s.Participant(f, o->id) && s.HolderSide(f, o) == defenders;
            });
        };
        facts.defendersInside = facts.holderSide ? side(true) : 0;
        facts.attackersInside = facts.holderSide ? 0 : side(false);
    }
    return game::EntryRefusal(facts);
}

void Fortresses::SendOut(const FortressActor& actor, std::uint16_t region) {
    for (const State::Fortress& f : s_->forts) {
        if (!f.atWar || !f.InArea(region)) continue;
        core::LogInfo("fortress war: {} logs in inside {} at war: sent out", actor.name, f.spec.codename);
        s_->host.FortressSendTo(actor.peer, f.spec.linkedTeleport);
        return;
    }
}

void Fortresses::PlayerKilled(std::int64_t killer, std::int64_t victim, std::uint16_t region) {
    State& s = *s_;
    if (!s.warOn) return;
    const std::optional<FortressGuild> gk = s.host.FortressGuildOf(killer), gv = s.host.FortressGuildOf(victim);
    for (State::Fortress& f : s.forts) {
        // The fortress of the victim's place (the original's: of his world), its war's two guilds.
        if (!f.atWar || !f.InArea(region) || !s.Participant(f, gk ? gk->id : 0) || !s.Participant(f, gv ? gv->id : 0)) continue;
        State::Fortress::Record& v = f.records[victim];
        ++v.deaths;
        // The kill counts for the killer and their living party members within 1,000, each climbing the ranks.
        std::vector<std::int64_t> credited{killer};
        for (const std::int64_t m : s.host.FortressPartyNear(killer, 1000.0)) {
            const std::optional<FortressGuild> gm = s.host.FortressGuildOf(m);
            if (m != victim && gm && s.Participant(f, gm->id)) credited.push_back(m);
        }
        for (const std::int64_t c : credited) s.Climb(f, c);
        const std::uint32_t id = f.spec.id;
        s.Write([id, victim, r = v](Database& db) { SaveBattleRecord(db, id, victim, r.kills, r.deaths, r.rank); });
        s.host.FortressSendCharacter(victim, proto::FortressScore::kId, Bytes(proto::FortressScore{id, std::uint32_t(v.kills), std::uint32_t(v.deaths), std::uint8_t(v.rank)}));
        // The siege quest of a player's kill (type 7) for the killer.
        s.QuestCredit(killer, 7);
        return;
    }
}

bool Fortresses::GuildHoldsFortress(std::uint32_t guild) const {
    return guild != 0 && std::any_of(s_->forts.begin(), s_->forts.end(), [&](const State::Fortress& f) { return f.row.guild == guild; });
}

bool Fortresses::GuildInWar(std::uint32_t guild) const {
    return std::any_of(s_->forts.begin(), s_->forts.end(), [&](const State::Fortress& f) { return s_->Participant(f, guild); });
}

bool Fortresses::RequestsOpen() const { return s_->requestsOpen; }
bool Fortresses::WarOn() const { return s_->warOn; }
std::uint32_t Fortresses::Owner(std::uint32_t fortress) const {
    const State::Fortress* f = s_->Find(fortress);
    return f != nullptr ? f->row.guild : 0;
}
std::uint32_t Fortresses::Holder(std::uint32_t fortress) const {
    const State::Fortress* f = s_->Find(fortress);
    return f != nullptr ? f->row.tempGuild : 0;
}
std::vector<std::uint32_t> Fortresses::Structures(std::uint32_t fortress, StructureKind kind) const {
    std::vector<std::uint32_t> out;
    const State::Fortress* f = s_->Find(fortress);
    if (f == nullptr) return out;
    for (const State::Structure& st : f->structures) {
        if (f->Spot(st).kind == kind) out.push_back(st.entity);
    }
    return out;
}
std::vector<std::uint32_t> Fortresses::Objects(std::uint32_t fortress, StructureKind kind) const {
    std::vector<std::uint32_t> out;
    if (const State::Fortress* f = s_->Find(fortress)) {
        for (const auto& [entity, o] : f->objects) {
            if (o.kind == kind) out.push_back(entity);
        }
    }
    return out;
}
std::size_t Fortresses::Count() const { return s_->forts.size(); }
bool Fortresses::Writing() const { return s_->writing || !s_->writes.empty(); }

bool LoadFortressSpecs(Database& db, const assets::FileSystem& files, const assets::CharacterTable& characters, std::vector<FortressSpec>& out,
                       std::string& error) {
    out.clear();
    std::vector<FortressReferenceRow> reference;
    if (!LoadFortressReference(db, reference, error)) return false;
    if (reference.empty()) return true;
    // The structures' places: navmesh\objectstring.ifo's named spots (STRUCTURE_POS_JA_GATE_01, ...).
    std::unordered_map<std::string, assets::NamedSpot> spots;
    if (const auto file = files.Read("navmesh\\objectstring.ifo")) {
        std::vector<assets::NamedSpot> list;
        if (!assets::DecodeObjectStrings(file->Bytes(), list, error)) return false;
        for (assets::NamedSpot& spot : list) spots[spot.name] = std::move(spot);
    } else {
        error = "navmesh\\objectstring.ifo is missing";
        return false;
    }
    const auto trim = [](std::string text) {
        while (!text.empty() && text.back() == ' ') text.pop_back();
        return text;
    };
    for (const FortressReferenceRow& r : reference) {
        FortressSpec f;
        f.id = r.id;
        f.codename = trim(r.codename);
        f.nameKey = trim(r.nameKey);
        f.official = trim(r.official);
        f.fee = r.fee;
        f.taxTargets = r.taxTargets;
        f.maxAdmission = r.maxAdmission;
        f.maxGuard = r.maxGuard;
        f.maxBarricade = r.maxBarricade;
        f.regions = r.regions;
        f.linkedTeleport = trim(r.linkedTeleport);
        // INS_FORT_JA -> GATE_JA_REVIVAL_GATE (`_RefTeleport`'s Gate of Resurrection, TID 4/1/1/2).
        if (const std::string world = trim(r.world); world.starts_with("INS_FORT_")) {
            const std::string code = world.substr(9);
            f.revivalGate = "GATE_" + code + "_REVIVAL_GATE";
            // Its gates by `_RefTeleport`'s names: inside in its own world, outside in world 1 (Jangan's are named for China,
            // Hotan's for Khotan, Bijeokdan's BD).
            const std::string outside = code == "JA" ? "CH" : code == "HT" ? "KT" : code == "BJ" ? "BD" : code;
            for (int n = 1; n <= 4; ++n) {
                f.insideGates.push_back("GATE_" + code + "_FORT_GATE" + std::to_string(n));
                f.outsideGates.push_back("GATE_" + outside + "_FORT_GATE" + std::to_string(n));
            }
        }
        std::size_t missing = 0;
        for (const FortressReferenceRow::Structure& z : r.structures) {
            const auto spot = spots.find(trim(z.zone));
            const assets::ObjectRow* row = characters.Find(trim(z.codename));
            const StructureKind kind = row != nullptr ? game::StructureKindOf(row->typeId[0], row->typeId[1], row->typeId[2], row->typeId[3]) : StructureKind::None;
            if (spot == spots.end() || row == nullptr || kind == StructureKind::None) {
                ++missing;
                continue;
            }
            FortressSpec::Structure st;
            st.eventId = z.eventId;
            st.refObjId = row->refObjId;
            st.kind = kind;
            st.nameKey = row->nameKey;
            st.maxHp = row->maxHp;
            st.position = {spot->second.region, spot->second.x, spot->second.y, spot->second.z};
            st.yaw = spot->second.yaw;
            st.navKey = spot->second.id;
            st.navName = trim(z.zone);
            std::transform(st.navName.begin(), st.navName.end(), st.navName.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            f.structures.push_back(st);
        }
        if (missing > 0) core::LogWarning("fortress war: {}: {} structures without their place or character, left out", f.codename, missing);
        out.push_back(std::move(f));
    }
    return true;
}

bool LoadFortressSchedule(Database& db, std::vector<game::ScheduleRow>& out, std::string& error) {
    return LoadSchedules(db, {game::kScheduleSiegeRequest, game::kScheduleSiegeWar, game::kScheduleSiegeTax}, out, error);
}

bool LoadSchedules(Database& db, const std::vector<int>& defines, std::vector<game::ScheduleRow>& out, std::string& error) {
    out.clear();
    const QueryResult exists = db.Query("SELECT to_regclass('_schedule') IS NOT NULL");
    if (!exists.Ok() || exists.Rows() == 0 || !exists.Bool(0, 0)) return true;
    std::string ids = "{";
    for (const int d : defines) ids += (ids.size() > 1 ? "," : "") + std::to_string(d);
    ids += "}";
    const QueryResult r = db.Query(
        "SELECT scheduledefineidx, to_char(datestart, 'YYYY-MM-DD HH24:MI:SS'), to_char(dateend, 'YYYY-MM-DD HH24:MI:SS'), maininterval_type, "
        "coalesce(subinterval_dayofweek, 0), coalesce(subinterval_starttimehour, 0), coalesce(subinterval_starttimeminute, 0), "
        "coalesce(subinterval_starttimesecond, 0), coalesce(subinterval_durationsecond, 0), coalesce(subinterval_weeks, 0), "
        "coalesce(maininterval_typedate, 1) FROM _schedule "
        "WHERE scheduledefineidx = ANY($1::int[]) "
        "ORDER BY scheduleidx",
        {ids});
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    for (int i = 0; i < r.Rows(); ++i) {
        game::ScheduleRow row;
        row.define = int(r.Int(i, 0));
        row.dateStart = game::LocalSeconds(r.Text(i, 1));
        row.dateEnd = game::LocalSeconds(r.Text(i, 2));
        row.mainType = int(r.Int(i, 3));
        row.dayOfWeek = int(r.Int(i, 4));
        row.hour = int(r.Int(i, 5));
        row.minute = int(r.Int(i, 6));
        row.second = int(r.Int(i, 7));
        row.durationSeconds = int(r.Int(i, 8));
        row.weeks = int(r.Int(i, 9));
        row.typeDate = int(r.Int(i, 10));
        out.push_back(row);
    }
    return true;
}

} // namespace sr::server
