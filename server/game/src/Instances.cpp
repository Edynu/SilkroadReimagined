#include "sr/server/Instances.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <deque>

#include "sr/assets/GameData.h"
#include "sr/core/Log.h"
#include "sr/server/ShardBus.h"
#include "sr/server/World.h"

namespace sr::server {

namespace proto = game::proto;

namespace {

std::int64_t UnixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
float Distance(const proto::WorldPosition& a, const proto::WorldPosition& b) {
    const proto::FlatPosition p = proto::ToFlat(a), q = proto::ToFlat(b);
    return float(std::hypot(p.x - q.x, p.z - q.z));
}
int Number(const std::string& text) { return std::atoi(text.c_str()); }
std::uint32_t BookKey(std::uint32_t theme, std::uint8_t slot) { return theme << 8 | slot; }

constexpr std::uint32_t kMaxLayer = 500'000; // layers are reused below this (the flat coordinates stay exact)
constexpr auto kSummonAnswer = std::chrono::seconds(20);  //'s box: 20,000 ms
constexpr auto kSummonTravel = std::chrono::seconds(60);  // ours: a member summoned from the other server on its way
constexpr auto kSummonPause = std::chrono::seconds(5);    // UIIT_STT_PARTY_SUMMON_BUTTON: 5 s between summons

// A summon between the shard's game servers: asked of the member's server, its coming, its refusal.
struct SummonBus {
    enum Kind : std::uint8_t { Ask = 1, Coming = 2, Refused = 3 };
    std::uint8_t kind = Ask;
    std::uint16_t from = 0, to = 0;
    std::uint32_t character = 0; // the member
    std::uint32_t layer = 0, key = 0;
    std::int32_t minLevel = 0, maxLevel = 0;
    std::string master, text, name;
    proto::WorldPosition place; // Ask: where the member goes on the master's server; Coming: where it comes back to
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("from", from);
        v("to", to);
        v("character", character);
        v("layer", layer);
        v("key", key);
        v("minLevel", minLevel);
        v("maxLevel", maxLevel);
        v("master", master, 64);
        v("text", text, 128);
        v("name", name, 64);
        v("place", place);
    }
};

// Why the player may not be summoned (0: it may; the reasons of UIIT_STT_PARTY_SUMMON_IMPOSSILBE_<n>).
std::uint8_t Ineligible(const InstanceHost::Player& p, int minLevel, int maxLevel) {
    if (p.dead) return 8;
    if (!p.inTown) return 1;
    if (p.level < minLevel || (maxLevel > 0 && p.level > maxLevel)) return 3;
    if (p.jobSuit) return 4;
    if (p.dealing) return 5;
    if (p.riding) return 6;
    if (p.atWar) return 7;
    if (p.freePvp) return 9;
    return 0;
}

} // namespace

struct Instances::State {
    InstanceHost& host;
    World& world;
    Monsters& monsters;
    const assets::CharacterTable& characters;
    std::shared_ptr<const GameWorldCatalog> catalog;
    std::mt19937 random;

    struct Player {
        std::int64_t character = 0;
        std::set<std::uint32_t> book; // BookKey
        std::vector<InstanceEntry> entries;
        std::uint32_t layer = 0;      // the instance it is in (0: none)
        proto::WorldPosition back;    // where it entered from
        bool arriving = false;        // teleported in, not placed yet
        std::map<std::uint32_t, std::int64_t> waits; // an item group -> when its wait began (Unix seconds)
    };
    std::unordered_map<std::uint32_t, Player> players;

    struct Delayed {
        Clock::time_point at;
        std::size_t trigger = 0, action = 0;
        std::uint32_t user = 0;
    };
    struct Event {
        std::string kind; // EVENT_MONSTER_KILL, EVENT_WORLD_ENTRANCE
        std::uint32_t user = 0; // the event's player
        // A kill's
        std::uint32_t monster = 0; // its RefObjID
        std::uint8_t rarity = 0;
        std::int64_t nest = -1;
        proto::WorldPosition at;
    };
    struct Instance {
        std::uint32_t layer = 0, key = 0;
        const GameWorldSpec* spec = nullptr;
        HoleSpec hole;
        std::uint32_t party = 0;
        std::string master;
        std::uint32_t masterEntity = 0;
        std::uint32_t gap = 0;
        Clock::time_point gapEnds, ends, emptySince;
        std::int64_t opened = 0;
        std::set<std::uint32_t> inside;
        std::unordered_map<std::uint32_t, std::size_t> nestById;              // the original's nest -> Monsters' index
        std::unordered_map<std::size_t, std::uint32_t> hiveOf;                // Monsters' index -> hive
        std::unordered_map<std::uint32_t, std::vector<std::size_t>> hives;    // hive -> Monsters' indices
        std::vector<std::size_t> extra;                                       // nests its triggers raised
        struct DoorState {
            std::uint32_t entity = 0;
            bool open = false;
        };
        std::vector<DoorState> doors;
        std::vector<std::uint32_t> npcs; // entities
        std::vector<std::uint32_t> exits;
        std::vector<std::uint32_t> pillars; // the party recall pillars
        std::vector<bool> fired;
        std::vector<Delayed> delayed;
        std::size_t warned = 0; // its end's warnings told (GameWorldSpec::warnMs)
    };
    std::map<std::uint32_t, Instance> instances; // by layer
    // Summons: asked by this server's masters (by the member's character), asked of this server's
    // players (by entity), members summoned from the other server on their way here (by character).
    struct Summon {
        std::uint32_t layer = 0, key = 0, master = 0;
        std::string name; // the member's
        Clock::time_point expires;
    };
    std::unordered_map<std::uint32_t, Summon> summons;
    struct Asked {
        bool remote = false;
        std::uint16_t from = 0;
        std::uint32_t character = 0, layer = 0, key = 0;
        std::int32_t minLevel = 0, maxLevel = 0;
        std::string master;
        proto::WorldPosition to;
        Clock::time_point expires;
    };
    std::unordered_map<std::uint32_t, Asked> asked;
    struct Arrival {
        std::uint32_t layer = 0, key = 0;
        proto::WorldPosition back;
        Clock::time_point expires;
    };
    std::unordered_map<std::uint32_t, Arrival> arrivals;
    std::unordered_map<std::uint32_t, Clock::time_point> summonPause; // a master's next summon
    std::uint32_t nextLayer = 1;
    std::deque<GameWorldSpec> questWorlds; // the quests' worlds opened so far (OpenQuestWorld), kept for their instances
    std::deque<GameWorldSpec> playedWorlds; // the worlds their own code plays (OpenWorld), kept for their instances
    std::vector<bool> sharedFired;
    std::vector<Delayed> sharedDelayed;

    State(InstanceHost& h, World& w, Monsters& m, const assets::CharacterTable& c, std::shared_ptr<const GameWorldCatalog> cat, std::uint32_t seed)
        : host(h), world(w), monsters(m), characters(c), catalog(std::move(cat)), random(seed) {
        sharedFired.assign(catalog->sharedTriggers.size(), false);
    }

    template <typename T>
    void Send(std::uint32_t entity, const T& message) {
        if (const auto p = host.InstancePlayer(entity)) host.InstanceSend(p->peer, T::kId, proto::Encode(message));
    }
    void Refuse(std::uint32_t entity, const char* text, const char* why) {
        if (const auto p = host.InstancePlayer(entity)) core::LogInfo("instances: {} refused: {}", p->name, why);
        Send(entity, proto::InstanceRefused{text});
    }
    Instance* Of(std::uint32_t layer) {
        const auto it = instances.find(layer);
        return it != instances.end() ? &it->second : nullptr;
    }
    Instance* ByGap(std::uint32_t gap) {
        for (auto& [layer, inst] : instances) {
            if (gap != 0 && inst.gap == gap) return &inst;
        }
        return nullptr;
    }
    std::uint32_t NextLayer() {
        for (std::uint32_t tries = 0; tries < kMaxLayer; ++tries) {
            const std::uint32_t layer = nextLayer;
            nextLayer = nextLayer >= kMaxLayer ? 1 : nextLayer + 1;
            if (!instances.contains(layer)) return layer;
        }
        return 0;
    }
    std::size_t CountOf(std::uint32_t world) const {
        return std::size_t(std::count_if(instances.begin(), instances.end(), [&](const auto& kv) { return kv.second.spec->id == world; }));
    }
    static proto::WorldPosition InLayer(proto::WorldPosition p, std::uint32_t layer) {
        p.layer = layer;
        return p;
    }

    // --- An instance's life --------------------------------------------------------------------------------

    Instance* Open(const GameWorldSpec& spec, const HoleSpec& hole, const InstanceHost::Player& master, std::uint32_t masterEntity,
                   const proto::WorldPosition& gapAt, float yaw, Clock::time_point now) {
        const std::uint32_t layer = NextLayer();
        if (layer == 0) return nullptr;
        Instance& inst = instances[layer];
        inst.layer = layer;
        inst.key = std::uniform_int_distribution<std::uint32_t>(1, 0x7FFFFFFF)(random);
        inst.spec = &spec;
        inst.hole = hole;
        inst.party = master.party;
        inst.master = master.name;
        inst.masterEntity = masterEntity;
        inst.opened = UnixNow();
        inst.gapEnds = now + std::chrono::milliseconds(spec.gapMs);
        inst.ends = now + std::chrono::milliseconds(spec.durationMs);
        inst.emptySince = now;
        inst.fired.assign(spec.triggers.size(), false);
        // Its monsters: every nest (the waiting ones wait for their trigger), in its layer.
        for (const MonsterNest& n : spec.nests) {
            MonsterNest nest = n;
            nest.position.layer = layer;
            const std::size_t index = monsters.AddNest(nest);
            if (index == SIZE_MAX) continue;
            inst.nestById[n.nestId] = index;
            inst.hiveOf[index] = n.hive;
            inst.hives[n.hive].push_back(index);
        }
        for (const GameWorldSpec::Npc& npc : spec.npcs) {
            const std::uint32_t entity = world.AddNpc(npc.refObjId, InLayer(npc.position, layer), npc.yaw);
            inst.npcs.push_back(entity);
            if (npc.codename == "NPC_INS_EXIT_TELEPORT") inst.exits.push_back(entity);
            if (npc.codename == "NPC_FWORLD_RECALL_PARTY") inst.pillars.push_back(entity);
        }
        // Its doors shut: standing, and in the way of its walks.
        for (const GameWorldSpec::Door& d : spec.doors) {
            const assets::ObjectRow* row = characters.Find(d.refObjId);
            World::MonsterLook look;
            look.model = d.refObjId;
            look.hp = row != nullptr ? std::uint32_t(row->maxHp) : 1u;
            inst.doors.push_back({world.AddStructure(look, InLayer(d.position, layer), d.yaw), false});
            world.SetNavZone(d.navKey, d.navName, true, layer);
        }
        if (hole.gapModel != 0) inst.gap = world.AddNpc(hole.gapModel, gapAt, yaw);
        core::LogInfo("instances: {} opens {} (layer {}, {} nests, {} NPCs, {} doors, {} triggers)", master.name, spec.codename, layer, inst.nestById.size(),
                      inst.npcs.size(), inst.doors.size(), spec.triggers.size());
        return &inst;
    }

    void Close(std::uint32_t layer) {
        Instance* inst = Of(layer);
        if (inst == nullptr) return;
        for (const std::uint32_t entity : std::set<std::uint32_t>(inst->inside)) Exit(entity, false);
        for (const auto& [id, index] : inst->nestById) monsters.RemoveNest(index);
        for (const std::size_t index : inst->extra) monsters.RemoveNest(index);
        for (const std::uint32_t npc : inst->npcs) world.Leave(npc);
        for (std::size_t d = 0; d < inst->doors.size(); ++d) {
            if (inst->doors[d].entity != 0) world.Leave(inst->doors[d].entity);
            world.SetNavZone(inst->spec->doors[d].navKey, inst->spec->doors[d].navName, false, layer);
        }
        if (inst->gap != 0) world.Leave(inst->gap);
        core::LogInfo("instances: {} (layer {}) ends", inst->spec->codename, layer);
        instances.erase(layer);
    }

    // Into the instance: its entry kept, taken to the start.
    void Admit(std::uint32_t entity, Player& p, Instance& inst, const proto::WorldPosition& from, const proto::WorldPosition* to = nullptr,
               bool counted = true) {
        p.layer = inst.layer;
        p.back = from;
        p.back.layer = 0;
        p.arriving = true;
        inst.inside.insert(entity);
        InstanceEntry e;
        for (const InstanceEntry& old : p.entries) {
            if (old.world == inst.spec->id) e = old;
        }
        e.key = inst.key;
        e.world = inst.spec->id;
        e.layer = inst.layer;
        e.opened = inst.opened;
        e.region = p.back.region;
        e.x = std::int32_t(std::lround(p.back.x));
        e.y = std::int32_t(std::lround(p.back.y));
        e.z = std::int32_t(std::lround(p.back.z));
        e.active = true;
        if (counted) {
            // The day's count (the tomb's): an entry of another day starts it again.
            if (inst.spec->entriesPerDay > 0 && !SameDay(e.last, UnixNow())) e.count = 0;
            e.count += 1;
            e.last = UnixNow();
        }
        std::erase_if(p.entries, [&](const InstanceEntry& old) { return old.world == e.world; });
        p.entries.insert(p.entries.begin(), e);
        host.InstanceSaveEntry(p.character, e);
        host.InstanceTeleport(entity, InLayer(to != nullptr ? *to : inst.spec->start, inst.layer));
    }

    // Two Unix times on one local day (the original's GameDateTime: the server's local date).
    static bool SameDay(std::int64_t a, std::int64_t b) {
        if (a <= 0 || b <= 0) return false;
        std::time_t ta = std::time_t(a), tb = std::time_t(b);
        std::tm la{}, lb{};
#ifdef _WIN32
        localtime_s(&la, &ta);
        localtime_s(&lb, &tb);
#else
        localtime_r(&ta, &la);
        localtime_r(&tb, &lb);
#endif
        return la.tm_year == lb.tm_year && la.tm_yday == lb.tm_yday;
    }

    // The Pharaoh's tomb's admission.
    proto::GateStatus EnterTomb(std::uint32_t entity, const GameWorldSpec& world) {
        const auto me = players.find(entity);
        const auto p = host.InstancePlayer(entity);
        const auto place = this->world.Where(entity);
        if (me == players.end() || !p || !place) return proto::GateStatus::Nowhere;
        Player& pl = me->second;
        const auto refuse = [&](proto::GateStatus status, const char* why) {
            core::LogInfo("instances: {} at {}: refused ({})", p->name, world.codename, why);
            return status;
        };
        if (pl.layer != 0) return refuse(proto::GateStatus::InInstance, "in an instance");
        // A free re-entry: its latest entry this world's, that instance standing with its key.
        const InstanceEntry* latest = nullptr;
        for (const InstanceEntry& e : pl.entries) {
            if (latest == nullptr || e.last > latest->last) latest = &e;
        }
        if (latest != nullptr && latest->world == world.id) {
            if (Instance* inst = Of(latest->layer); inst != nullptr && inst->key == latest->key && inst->spec->id == world.id) {
                core::LogInfo("instances: {} goes back into {} (layer {})", p->name, world.codename, inst->layer);
                Admit(entity, pl, *inst, place->position, nullptr, false);
                return proto::GateStatus::Ok;
            }
        }
        if (p->level < world.minLevel || (world.maxLevel > 0 && p->level > world.maxLevel)) return refuse(proto::GateStatus::WrongLevel, "level");
        if (p->jobSuit && world.noJobSuit) return refuse(proto::GateStatus::JobSuit, "job suit");
        if (p->riding) return refuse(proto::GateStatus::Riding, "riding");
        if (p->freePvp) return refuse(proto::GateStatus::JobSuit, "free PvP");
        if (world.entriesPerDay > 0) {
            for (const InstanceEntry& e : pl.entries) {
                if (e.world == world.id && SameDay(e.last, UnixNow()) && int(e.count) >= world.entriesPerDay) {
                    return refuse(proto::GateStatus::TombEntries, "the day's entries");
                }
            }
        }
        if (world.party) {
            if (p->party == 0 || host.InstancePartyMembers(entity).size() < 2) return refuse(proto::GateStatus::TombParty, "a party of two");
            if (!p->partyMaster) {
                // A member: into his master's instance, of this difficulty.
                for (auto& [layer, inst] : instances) {
                    if (inst.party != p->party || !inst.spec->party) continue;
                    if (inst.spec->id != world.id) return refuse(proto::GateStatus::TombMaster, "the master's other difficulty");
                    if (inst.inside.size() >= inst.spec->maxUsers) return refuse(proto::GateStatus::InstanceFull, "full");
                    Admit(entity, pl, inst, place->position);
                    core::LogInfo("instances: {} joins its party's {} (layer {})", p->name, world.codename, layer);
                    return proto::GateStatus::Ok;
                }
                return refuse(proto::GateStatus::TombMaster, "the master has not gone in");
            }
            // The master: his party's older one ends (one at a time a party, ours), a new one opens.
            for (auto& [layer, inst] : instances) {
                if (inst.party == p->party && inst.spec->party && inst.inside.empty()) inst.ends = Clock::now();
            }
        }
        if (CountOf(world.id) >= world.maxCount) return refuse(proto::GateStatus::InstanceNone, "no instance free");
        Instance* inst = Open(world, HoleSpec{}, *p, entity, {}, 0.0f, Clock::now());
        if (inst == nullptr) return refuse(proto::GateStatus::InstanceNone, "no layer");
        inst->gapEnds = Clock::now();
        Admit(entity, pl, *inst, place->position);
        // His party told the difficulty (0x300C 0x0C1B; UIIT_MSG_EGYPT_TOMB_ENTRANCE_CONDITION_ERROR_04 "Party master chooses %s
        // difficulty.").
        if (world.party) {
            proto::InstanceNotice told{"UIIT_MSG_EGYPT_TOMB_ENTRANCE_CONDITION_ERROR_04"};
            told.system = true;
            told.notify = true;
            told.arg = world.nameKey;
            for (const InstanceHost::Member& m : host.InstancePartyMembers(entity)) {
                if (m.entity != 0 && m.entity != entity) Send(m.entity, told);
            }
        }
        return proto::GateStatus::Ok;
    }

    // The state told: its instance (none: 0) and the re-entry limit's timed job left after its latest entry.
    proto::InstanceState StateFor(const Player& p, std::uint32_t world, std::uint32_t secondsLeft) const {
        proto::InstanceState s{world, secondsLeft};
        std::int64_t last = 0;
        for (const InstanceEntry& e : p.entries) last = std::max(last, e.last);
        const std::int64_t left = last + std::int64_t(catalog->jobSeconds) - UnixNow();
        if (last != 0 && left > 0) {
            s.jobSeconds = std::uint32_t(left);
            s.jobIcon = catalog->jobIcon;
            s.jobSubject = catalog->jobSubject;
            s.jobText = catalog->jobText;
        }
        return s;
    }

    // Out of its instance: back where it came from (`teleport`), its entry no longer active.
    void Exit(std::uint32_t entity, bool alreadyOut) {
        const auto it = players.find(entity);
        if (it == players.end() || it->second.layer == 0) return;
        Player& p = it->second;
        if (Instance* inst = Of(p.layer)) {
            inst->inside.erase(entity);
            if (inst->inside.empty()) inst->emptySince = Clock::now();
        }
        p.layer = 0;
        p.arriving = false;
        for (InstanceEntry& e : p.entries) {
            if (!e.active) continue;
            e.active = false;
            host.InstanceSaveEntry(p.character, e);
        }
        if (!alreadyOut) host.InstanceTeleport(entity, p.back);
        Send(entity, StateFor(p, 0, 0));
    }

    // --- The triggers ----------------------------------------------------------------------------------------

    bool Compare(const std::map<std::string, std::string>& params, int value) const {
        const auto sign = params.find("SIGN_SELECT");
        const auto num = params.find("NUM_INPUT");
        if (num == params.end()) return false;
        const int n = Number(num->second);
        if (sign == params.end()) return value == n;
        if (sign->second == "SIGN_LEFT_BIG_EQUAL") return value >= n;
        if (sign->second == "SIGN_RIGHT_BIG_EQUAL") return value <= n;
        if (sign->second == "SIGN_LEFT_BIG") return value > n;
        if (sign->second == "SIGN_RIGHT_BIG") return value < n;
        return value == n;
    }
    bool Holds(const TriggerSpec::Condition& c, const Event& e, Instance* inst) {
        const auto param = [&](const char* name) {
            const auto it = c.params.find(name);
            return it != c.params.end() ? it->second : std::string();
        };
        if (c.kind == "CONDITION_MONSTER_HIVE_KILL") {
            if (inst == nullptr || e.kind != "EVENT_MONSTER_KILL" || e.nest < 0) return false;
            const std::uint32_t hive = std::uint32_t(Number(param("HIVE_MONSTER_SELECT")));
            const auto mine = inst->hiveOf.find(std::size_t(e.nest));
            if (mine == inst->hiveOf.end() || mine->second != hive) return false;
            const auto& list = inst->hives[hive];
            return std::all_of(list.begin(), list.end(), [&](std::size_t n) { return monsters.NestCleared(n); });
        }
        if (c.kind == "CONDITION_MONSTER_KILL") {
            const assets::ObjectRow* row = characters.Find(e.monster);
            return e.kind == "EVENT_MONSTER_KILL" && row != nullptr && row->codename == param("MONSTER_SELECT");
        }
        if (c.kind == "CONDITION_MONSTER_RARITY_KILL") return e.kind == "EVENT_MONSTER_KILL" && int(e.rarity) == Number(param("NUM_INPUT"));
        if (c.kind == "CONDITION_MONSTER_LEVEL_KILL" || c.kind == "CONDITION_MONSTER_LEVEL") {
            return e.kind == "EVENT_MONSTER_KILL" && Compare(c.params, host.InstanceMonsterLevel(e.monster));
        }
        if (c.kind == "CONDITION_USER_LEVEL") {
            const auto p = e.user != 0 ? host.InstancePlayer(e.user) : std::nullopt;
            return p && Compare(c.params, p->level);
        }
        return false; // a kind these worlds do not use
    }
    // Whether its conditions let it fire (none: it fires).
    bool Passes(const TriggerSpec& t, const Event& e, Instance* inst) {
        for (std::size_t i = 0; i < t.conditions.size(); ++i) {
            const TriggerSpec::Condition& c = t.conditions[i];
            const std::string& next = Holds(c, e, inst) ? c.onTrue : c.onFalse;
            if (next == "RETURN_TRUE") return true;
            if (next == "RETURN_FALSE") return false;
        }
        return true;
    }
    void Raise(const std::vector<TriggerSpec>& triggers, std::vector<bool>& fired, std::vector<Delayed>& delayed, const Event& e, Instance* inst,
               Clock::time_point now) {
        for (std::size_t i = 0; i < triggers.size(); ++i) {
            const TriggerSpec& t = triggers[i];
            if (!t.active || (!t.repeat && fired[i])) continue;
            if (std::find(t.events.begin(), t.events.end(), e.kind) == t.events.end()) continue;
            if (!Passes(t, e, inst)) continue;
            fired[i] = true;
            for (std::size_t a = 0; a < t.actions.size(); ++a) {
                if (t.actions[a].delayMs == 0) {
                    Act(t, t.actions[a], e.user, inst);
                } else {
                    delayed.push_back({now + std::chrono::milliseconds(t.actions[a].delayMs), i, a, e.user});
                }
            }
        }
    }
    // The players an action names: the instance's, its event's, or a random one of the instance's.
    std::vector<std::uint32_t> Targets(const std::string& which, std::uint32_t user, Instance* inst) {
        std::vector<std::uint32_t> out;
        if (which == "TARGET_PLAYER_ALL" && inst != nullptr) {
            out.assign(inst->inside.begin(), inst->inside.end());
        } else if (which == "TARGET_PLAYER_RANDOM" && inst != nullptr && !inst->inside.empty()) {
            auto it = inst->inside.begin();
            std::advance(it, std::uniform_int_distribution<std::size_t>(0, inst->inside.size() - 1)(random));
            out.push_back(*it);
        } else if (user != 0) {
            out.push_back(user);
        }
        return out;
    }
    void Act(const TriggerSpec& t, const TriggerSpec::Action& a, std::uint32_t user, Instance* inst) {
        const std::string target = t.Param(a, "TARGET_PLAYER_INS_SELECT");
        if (a.kind == "ACTION_MONSTER_ACTIVE_NEST") {
            if (inst == nullptr) return;
            const auto it = inst->nestById.find(std::uint32_t(Number(t.Param(a, "NEST_MONSTER_SELECT"))));
            if (it != inst->nestById.end()) monsters.ActivateNest(it->second);
        } else if (a.kind == "ACTION_SWITCH_EVENTZONE") {
            if (inst == nullptr) return;
            const std::string zone = t.Param(a, "EVENT_ZONE_SELECT");
            const bool open = t.Param(a, "ON_OFF_SWITCH") == "ON";
            for (std::size_t d = 0; d < inst->spec->doors.size(); ++d) {
                const GameWorldSpec::Door& door = inst->spec->doors[d];
                Instance::DoorState& state = inst->doors[d];
                if (door.zone != zone || state.open == open) continue;
                state.open = open;
                world.SetNavZone(door.navKey, door.navName, !open, inst->layer);
                if (open && state.entity != 0) {
                    world.Leave(state.entity); // ours: the door goes (its opening's look not traced)
                    state.entity = 0;
                } else if (!open && state.entity == 0) {
                    World::MonsterLook look;
                    look.model = door.refObjId;
                    look.hp = 1;
                    state.entity = world.AddStructure(look, InLayer(door.position, inst->layer), door.yaw);
                }
            }
        } else if (a.kind == "ACTION_MESSAGE_NOTIFY_WINDOW" || a.kind == "ACTION_MESSAGE_SYSTEM_WINDOW") {
            const bool notify = a.kind == "ACTION_MESSAGE_NOTIFY_WINDOW";
            for (const std::uint32_t p : Targets(target, user, inst)) Send(p, proto::InstanceNotice{t.Param(a, "STRING_INPUT"), notify, !notify});
        } else if (a.kind == "ACTION_MONSTER_LOAD_MONSTER") {
            const double chance = std::atof(t.Param(a, "FLOAT_INPUT").c_str());
            if (chance < 1.0 && std::uniform_real_distribution<double>(0.0, 1.0)(random) >= chance) return;
            const assets::ObjectRow* row = characters.Find(t.Param(a, "MONSTER_SELECT"));
            if (row == nullptr) return;
            const auto who = Targets(target, user, inst);
            if (who.empty()) return;
            const auto place = world.Where(who.front());
            if (!place) return;
            MonsterNest nest;
            nest.refObjId = row->refObjId;
            nest.position = place->position; // its layer with it
            nest.spawnRadius = 40.0f;
            nest.radius = 400.0f;
            nest.maxCount = std::max(1, Number(t.Param(a, "NUM_INPUT_2")));
            nest.rarity = Number(t.Param(a, "NUM_INPUT_1"));
            nest.aggressive = true;
            nest.sight = 300.0f;
            nest.respawn = false;
            nest.temporary = true;
            const std::size_t index = monsters.AddNest(nest);
            if (index != SIZE_MAX && inst != nullptr) inst->extra.push_back(index);
            core::LogInfo("instances: {} raises {} x{}", t.codename, row->codename, nest.maxCount);
        }
    }
    void RunDelayed(const std::vector<TriggerSpec>& triggers, std::vector<Delayed>& delayed, Instance* inst, Clock::time_point now) {
        std::vector<Delayed> due;
        std::erase_if(delayed, [&](const Delayed& d) {
            if (d.at > now) return false;
            due.push_back(d);
            return true;
        });
        for (const Delayed& d : due) Act(triggers[d.trigger], triggers[d.trigger].actions[d.action], d.user, inst);
    }

    // --- The collection book ---------------------------------------------------------------------------------

    void Register(std::uint32_t entity, std::uint8_t slot) {
        const auto it = players.find(entity);
        if (it == players.end()) return;
        const std::uint32_t item = host.InstanceItemAt(entity, slot);
        const auto t = catalog->talismans.find(item);
        if (t == catalog->talismans.end()) return Send(entity, proto::TalismanRegistered{false, 0, 0, ""});
        proto::TalismanRegistered answer{false, t->second.theme, t->second.slot, "UIIT_MSG_COLLECTION_WINDOW_RECORD_ERR"};
        if (it->second.book.contains(BookKey(t->second.theme, t->second.slot)) || !host.InstanceTakeOne(entity, slot)) return Send(entity, answer);
        it->second.book.insert(BookKey(t->second.theme, t->second.slot));
        host.InstanceSaveBook(it->second.character, {t->second.theme, t->second.slot});
        answer.ok = true;
        answer.text = "UIIT_MSG_COLLECTION_WINDOW_RECORD_FINISH";
        Send(entity, answer);
        host.InstanceBookChanged(entity);
    }

    // --- The hole and the gap --------------------------------------------------------------------------------

    void UseHole(std::uint32_t entity, std::uint8_t slot, const HoleSpec& hole) {
        const auto p = host.InstancePlayer(entity);
        const auto me = players.find(entity);
        const GameWorldSpec* spec = catalog->World(hole.world);
        const auto place = world.Where(entity);
        if (!p || me == players.end() || spec == nullptr || !place) return;
        if (p->dead) return Refuse(entity, "UIIT_MGS_ERR_GOD_DEADSTATE", "dead");
        if (me->second.layer != 0 || place->position.layer != 0) return Refuse(entity, "UIIT_MGS_ERR_GOD_INS_WORLD", "in an instance");
        if (!p->inTown) return Refuse(entity, "UIIT_MGS_ERR_GOD_TELEPORT_HOLE_NOT_TOWN", "not in a town");
        if (p->party == 0 || !p->partyMaster) return Refuse(entity, "UIIT_MGS_ERR_GOD_TELEPORT_HOLE_NOT_PARTY", "not a party's master");
        if (p->level < hole.minLevel || (hole.maxLevel > 0 && p->level > hole.maxLevel)) return Refuse(entity, "UIIT_MGS_ERR_GOD_LEVEL", "level");
        // Ours: one gap at a time for a master (the original's rule not traced).
        for (const auto& [layer, inst] : instances) {
            if (inst.masterEntity == entity && inst.gap != 0) return Refuse(entity, "UIIT_MGS_ERR_GOD_INS_WORLD", "its gap still stands");
        }
        if (CountOf(spec->id) >= spec->maxCount) return Refuse(entity, "UIIT_MGS_ERR_GOD_PERSONNEL_MAX_COUNT", "the world's instances are all taken");
        // Its group's wait (COOLTIME: 5 hours after any hole was used; the client greys the holes, so silently).
        const std::int64_t now = UnixNow();
        if (const auto w = me->second.waits.find(hole.waitGroup);
            hole.waitGroup != 0 && w != me->second.waits.end() && now < w->second + std::int64_t(hole.waitMs / 1000)) {
            core::LogInfo("instances: {} refused: the holes' wait ({} s left)", p->name, w->second + std::int64_t(hole.waitMs / 1000) - now);
            return;
        }
        if (host.InstanceItemAt(entity, slot) != hole.item || !host.InstanceTakeOne(entity, slot)) return;
        if (Open(*spec, hole, *p, entity, place->position, place->yaw, Clock::now()) != nullptr && hole.waitGroup != 0 && hole.waitMs != 0) {
            me->second.waits[hole.waitGroup] = now;
            host.InstanceSaveWait(me->second.character, {hole.waitGroup, now});
            Send(entity, proto::Cooldown{proto::CooldownKind::Item, hole.item, hole.waitMs});
        }
    }

    void SelectGap(std::uint32_t entity, std::uint32_t gap) {
        Instance* inst = ByGap(gap);
        if (inst == nullptr) return;
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(inst->gapEnds - Clock::now()).count();
        const auto p = host.InstancePlayer(entity);
        Send(entity, proto::GapInfo{gap, inst->spec->id, inst->hole.theme, inst->hole.grade, std::uint16_t(inst->hole.minLevel),
                                    std::uint16_t(inst->hole.maxLevel), inst->master, std::uint32_t(std::max<std::int64_t>(0, left)),
                                    p && p->party != 0 && p->party == inst->party});
    }

    void EnterGap(std::uint32_t entity, std::uint32_t gap) {
        Instance* inst = ByGap(gap);
        // A gap's model put in an instance as its exit (the Roc's INS_QUEST_TELEPORT): the way out.
        if (inst == nullptr) {
            if (const auto me = players.find(entity); me != players.end() && me->second.layer != 0) {
                if (Instance* in = Of(me->second.layer); in != nullptr && std::find(in->exits.begin(), in->exits.end(), gap) != in->exits.end()) {
                    return LeaveBy(entity, gap);
                }
            }
        }
        const auto me = players.find(entity);
        const auto p = host.InstancePlayer(entity);
        const auto place = world.Where(entity), there = world.Where(gap);
        if (inst == nullptr || me == players.end() || !p || !place || !there) return;
        if (Distance(place->position, there->position) > proto::kGapReach) return Refuse(entity, "UIIT_MGS_ERR_GOD_TELEPORT_HOLE_NOT_PARTY", "too far");
        if (p->party == 0 || p->party != inst->party) return Refuse(entity, "UIIT_MGS_ERR_GOD_TELEPORT_HOLE_NOT_PARTY", "not of the party");
        if (p->dead) return Refuse(entity, "UIIT_MGS_ERR_GOD_DEADSTATE", "dead");
        if (p->riding) return Refuse(entity, "UIIT_MGS_ERR_GOD_CART_STATE", "riding");
        if (p->jobSuit && inst->spec->noJobSuit) return Refuse(entity, "UIIT_MGS_ERR_GOD_JOB_STATE", "job clothes");
        if (p->dealing) return Refuse(entity, "UIIT_MGS_ERR_GOD_TRADE_STATE", "dealing");
        if (p->atWar) return Refuse(entity, "UIIT_MGS_ERR_GOD_GUILD", "at war");
        if (me->second.layer != 0) return Refuse(entity, "UIIT_MGS_ERR_GOD_INS_WORLD", "in an instance");
        if (p->level < inst->spec->minLevel || (inst->spec->maxLevel > 0 && p->level > inst->spec->maxLevel)) {
            return Refuse(entity, "UIIT_MGS_ERR_GOD_LEVEL", "level");
        }
        if (inst->inside.size() >= inst->spec->maxUsers) return Refuse(entity, "UIIT_MGS_ERR_GOD_PERSONNEL_MAX_COUNT", "full");
        // The re-entry limit: another instance entered too lately.
        const std::int64_t now = UnixNow();
        for (const InstanceEntry& e : me->second.entries) {
            if (e.key != inst->key && now - e.last < std::int64_t(catalog->reentrySeconds)) {
                return Refuse(entity, "UIIT_MGS_ERR_GOD_LIMIT_TIME", "the re-entry limit");
            }
        }
        core::LogInfo("instances: {} enters {} (layer {})", p->name, inst->spec->codename, inst->layer);
        Admit(entity, me->second, *inst, place->position);
    }

    // --- The party recall pillar ----------------------------------------------------------------

    void Publish(const SummonBus& m) { host.InstancePublish(BusToHex(proto::Encode(m))); }
    void TellMaster(std::uint32_t master, const char* text, const std::string& name) { Send(master, proto::SummonResult{text, name}); }

    // The master at its instance's pillar (null: not so).
    Instance* AtPillar(std::uint32_t entity, std::uint32_t pillar) {
        const auto me = players.find(entity);
        Instance* inst = me != players.end() ? Of(me->second.layer) : nullptr;
        if (inst == nullptr || std::find(inst->pillars.begin(), inst->pillars.end(), pillar) == inst->pillars.end()) return nullptr;
        const auto place = world.Where(entity), there = world.Where(pillar);
        if (!place || !there || Distance(place->position, there->position) > proto::kGapReach) return nullptr;
        return inst;
    }

    void OpenSummon(std::uint32_t entity, std::uint32_t pillar) {
        Instance* inst = AtPillar(entity, pillar);
        const auto p = host.InstancePlayer(entity);
        if (inst == nullptr || !p) return;
        if (!p->partyMaster) return TellMaster(entity, "UIIT_MGS_ERR_PARTY_SUMMON_NOT", "");
        Send(entity, ListFor(entity, *inst));
    }

    proto::SummonList ListFor(std::uint32_t entity, const Instance& inst) {
        proto::SummonList list;
        list.maxUsers = std::uint8_t(inst.spec->maxUsers);
        const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(inst.ends - Clock::now()).count();
        const bool full = inst.inside.size() >= inst.spec->maxUsers;
        for (const InstanceHost::Member& m : host.InstancePartyMembers(entity)) {
            if (m.server == host.InstanceServerId() && m.entity == entity) continue;
            proto::SummonList::Row row;
            row.character = m.character;
            row.name = m.name;
            row.level = std::uint8_t(std::clamp(m.level, 0, 255));
            row.minutes = std::uint16_t(std::max<std::int64_t>(0, minutes));
            const bool here = m.server == host.InstanceServerId();
            const auto them = here ? players.find(m.entity) : players.end();
            if (them != players.end() && them->second.layer == inst.layer) {
                row.state = 1; // inside already
            } else if (full) {
                row.state = 3;
                row.reason = 2;
            } else if (here) {
                const auto who = host.InstancePlayer(m.entity);
                row.reason = who ? Ineligible(*who, inst.spec->minLevel, inst.spec->maxLevel) : 1;
                row.state = row.reason == 0 ? 2 : 3;
            } // on the other server: its server decides when asked
            list.rows.push_back(row);
        }
        return list;
    }

    void SummonMember(std::uint32_t entity, std::uint32_t character) {
        const auto me = players.find(entity);
        Instance* inst = me != players.end() ? Of(me->second.layer) : nullptr;
        const auto p = host.InstancePlayer(entity);
        if (inst == nullptr || !p) return;
        if (!p->partyMaster) return TellMaster(entity, "UIIT_MGS_ERR_PARTY_SUMMON_NOT", "");
        const auto now = Clock::now();
        if (const auto pause = summonPause.find(entity); pause != summonPause.end() && now < pause->second) return; // the button's 5 s
        const std::vector<InstanceHost::Member> members = host.InstancePartyMembers(entity);
        const auto m = std::find_if(members.begin(), members.end(), [&](const InstanceHost::Member& x) { return x.character == character; });
        if (m == members.end() || (m->server == host.InstanceServerId() && m->entity == entity)) return;
        if (inst->inside.size() >= inst->spec->maxUsers) return TellMaster(entity, "UIIT_STT_PARTY_SUMMON_IMPOSSILBE_2", m->name);
        if (const auto pending = summons.find(character); pending != summons.end() && now < pending->second.expires) {
            return TellMaster(entity, "UIIT_STT_PARTY_SUMMON_WAIT_ERR", m->name);
        }
        summonPause[entity] = now + kSummonPause;
        summons[character] = {inst->layer, inst->key, entity, m->name, now + kSummonAnswer + (m->server == host.InstanceServerId() ? std::chrono::seconds(0) : kSummonTravel)};
        if (m->server == host.InstanceServerId()) {
            const auto who = host.InstancePlayer(m->entity);
            const auto them = players.find(m->entity);
            if (!who || them == players.end() || them->second.layer != 0 || Ineligible(*who, inst->spec->minLevel, inst->spec->maxLevel) != 0) {
                summons.erase(character);
                return TellMaster(entity, "UIIT_STT_GNGWC_PARTY_MEMBER_CALL_CANNOT", m->name);
            }
            asked[m->entity] = {false, host.InstanceServerId(), character, inst->layer, inst->key, inst->spec->minLevel, inst->spec->maxLevel, p->name, {},
                                now + kSummonAnswer};
            Send(m->entity, proto::SummonAsked{p->name, std::uint32_t(kSummonAnswer.count())});
            core::LogInfo("instances: {} summons {} into {} (layer {})", p->name, m->name, inst->spec->codename, inst->layer);
            return;
        }
        // On the other server: asked there; it comes to the place its master entered from (this server's town).
        SummonBus ask;
        ask.kind = SummonBus::Ask;
        ask.from = host.InstanceServerId();
        ask.to = m->server;
        ask.character = character;
        ask.layer = inst->layer;
        ask.key = inst->key;
        ask.minLevel = inst->spec->minLevel;
        ask.maxLevel = inst->spec->maxLevel;
        ask.master = p->name;
        ask.name = m->name;
        ask.place = me->second.back;
        Publish(ask);
        core::LogInfo("instances: {} summons {} from game server {} into {} (layer {})", p->name, m->name, m->server, inst->spec->codename, inst->layer);
    }

    void SummonReply(std::uint32_t entity, bool yes) {
        const auto it = asked.find(entity);
        if (it == asked.end()) return;
        const Asked a = it->second;
        asked.erase(it);
        const auto me = players.find(entity);
        const auto p = host.InstancePlayer(entity);
        if (Clock::now() > a.expires || me == players.end() || !p) return;
        const bool ready = me->second.layer == 0 && Ineligible(*p, a.minLevel, a.maxLevel) == 0;
        if (!a.remote) {
            const auto s = summons.find(a.character);
            Instance* inst = Of(a.layer);
            if (s == summons.end() || inst == nullptr || inst->key != a.key) return;
            const std::uint32_t master = s->second.master;
            summons.erase(s);
            if (!yes) return TellMaster(master, "UIIT_STT_GNGWC_PARTY_MEMBER_CALL_REFUSAL", p->name);
            if (!ready || inst->inside.size() >= inst->spec->maxUsers) return TellMaster(master, "UIIT_STT_GNGWC_PARTY_MEMBER_CALL_CANNOT", p->name);
            const auto place = world.Where(entity);
            if (!place) return;
            core::LogInfo("instances: {} answers the summon into {} (layer {})", p->name, inst->spec->codename, inst->layer);
            Admit(entity, me->second, *inst, place->position);
            return;
        }
        SummonBus answer;
        answer.from = host.InstanceServerId();
        answer.to = a.from;
        answer.character = a.character;
        answer.layer = a.layer;
        answer.key = a.key;
        answer.name = p->name;
        if (!yes || !ready) {
            answer.kind = SummonBus::Refused;
            answer.text = yes ? "UIIT_STT_GNGWC_PARTY_MEMBER_CALL_CANNOT" : "UIIT_STT_GNGWC_PARTY_MEMBER_CALL_REFUSAL";
            Publish(answer);
            return;
        }
        const auto place = world.Where(entity);
        if (!place) return;
        // Coming: the master's server takes it in on its arrival; it travels there (a move to that server's town).
        answer.kind = SummonBus::Coming;
        answer.place = place->position;
        answer.place.layer = 0;
        Publish(answer);
        core::LogInfo("instances: {} answers the summon of game server {}: on its way", p->name, a.from);
        host.InstanceTeleport(entity, a.to);
    }

    void OnBus(const SummonBus& m) {
        if (m.to != host.InstanceServerId()) return;
        const auto now = Clock::now();
        if (m.kind == SummonBus::Ask) {
            const std::uint32_t entity = host.InstanceEntityOf(m.character);
            const auto p = entity != 0 ? host.InstancePlayer(entity) : std::nullopt;
            const auto me = players.find(entity);
            SummonBus refused;
            refused.kind = SummonBus::Refused;
            refused.from = host.InstanceServerId();
            refused.to = m.from;
            refused.character = m.character;
            refused.name = m.name;
            refused.text = "UIIT_STT_GNGWC_PARTY_MEMBER_CALL_CANNOT";
            if (!p || me == players.end() || me->second.layer != 0 || Ineligible(*p, m.minLevel, m.maxLevel) != 0) return Publish(refused);
            asked[entity] = {true, m.from, m.character, m.layer, m.key, m.minLevel, m.maxLevel, m.master, m.place, now + kSummonAnswer};
            Send(entity, proto::SummonAsked{m.master, std::uint32_t(kSummonAnswer.count())});
        } else if (m.kind == SummonBus::Coming) {
            arrivals[m.character] = {m.layer, m.key, m.place, now + kSummonTravel};
            summons.erase(m.character);
        } else if (m.kind == SummonBus::Refused) {
            const auto s = summons.find(m.character);
            if (s == summons.end()) return;
            const std::uint32_t master = s->second.master;
            summons.erase(s);
            TellMaster(master, m.text.c_str(), m.name);
        }
    }

    // A summoned member who came from the other server: into its instance (where it entered from: its own town there).
    bool TakeArrival(std::uint32_t entity, Player& p) {
        const auto it = arrivals.find(std::uint32_t(p.character));
        if (it == arrivals.end()) return false;
        const Arrival a = it->second;
        arrivals.erase(it);
        Instance* inst = Of(a.layer);
        if (inst == nullptr || inst->key != a.key || inst->inside.size() >= inst->spec->maxUsers || Clock::now() > a.expires) return false;
        Admit(entity, p, *inst, a.back);
        return true;
    }

    void LeaveBy(std::uint32_t entity, std::uint32_t exit) {
        const auto me = players.find(entity);
        if (me == players.end() || me->second.layer == 0) return;
        Instance* inst = Of(me->second.layer);
        const auto place = world.Where(entity), there = world.Where(exit);
        if (inst == nullptr || !place || !there || std::find(inst->exits.begin(), inst->exits.end(), exit) == inst->exits.end()) return;
        if (Distance(place->position, there->position) > proto::kGapReach) return;
        Exit(entity, false);
    }
};

Instances::Instances(InstanceHost& host, World& world, Monsters& monsters, const assets::CharacterTable& characters,
                     std::shared_ptr<const GameWorldCatalog> catalog, std::uint32_t seed)
    : s_(std::make_unique<State>(host, world, monsters, characters, std::move(catalog), seed)) {}

Instances::~Instances() = default;

void Instances::Enter(std::uint32_t entity, std::int64_t character, std::vector<BookEntry> book, std::vector<InstanceEntry> entries,
                      std::vector<ItemWait> waits) {
    State::Player& p = s_->players[entity];
    p = {};
    p.character = character;
    // The holes' wait left, told as a hole's (the client greys every item of the group).
    const std::int64_t now = UnixNow();
    for (const ItemWait& w : waits) {
        p.waits[w.group] = w.started;
        for (const auto& [item, hole] : s_->catalog->holes) {
            if (hole.waitGroup != w.group || hole.waitMs == 0) continue;
            const std::int64_t left = w.started + std::int64_t(hole.waitMs / 1000) - now;
            if (left > 0) s_->Send(entity, proto::Cooldown{proto::CooldownKind::Item, item, std::uint32_t(left * 1000)});
            break;
        }
    }
    for (const BookEntry& b : book) p.book.insert(BookKey(b.theme, b.slot));
    p.entries = std::move(entries);
    // Left inside last time (it comes back where it entered from): no longer inside.
    for (InstanceEntry& e : p.entries) {
        if (!e.active) continue;
        e.active = false;
        s_->host.InstanceSaveEntry(character, e);
    }
    proto::CollectionBook message;
    for (const BookEntry& b : book) message.entries.push_back({b.theme, b.slot});
    s_->Send(entity, message);
    if (const proto::InstanceState state = s_->StateFor(p, 0, 0); state.jobSeconds > 0) s_->Send(entity, state); // the re-entry limit still runs
    s_->TakeArrival(entity, p);
}

void Instances::Leave(std::uint32_t entity) {
    State& s = *s_;
    const auto it = s.players.find(entity);
    if (it == s.players.end()) return;
    if (it->second.layer != 0) {
        if (State::Instance* inst = s.Of(it->second.layer)) {
            inst->inside.erase(entity);
            if (inst->inside.empty()) inst->emptySince = Clock::now();
        }
        for (InstanceEntry& e : it->second.entries) {
            if (!e.active) continue;
            e.active = false;
            s.host.InstanceSaveEntry(it->second.character, e);
        }
    }
    for (auto& [layer, inst] : s.instances) {
        if (inst.masterEntity == entity) inst.masterEntity = 0;
    }
    s.asked.erase(entity);
    s.summonPause.erase(entity);
    s.players.erase(it);
}

bool Instances::Handle(std::uint32_t entity, proto::MessageId id, std::span<const std::byte> payload) {
    State& s = *s_;
    switch (id) {
    case proto::SelectGap::kId:
        if (proto::SelectGap m; proto::Decode(payload, m)) {
            s.SelectGap(entity, m.entity);
            return true;
        }
        return false;
    case proto::EnterGap::kId:
        if (proto::EnterGap m; proto::Decode(payload, m)) {
            s.EnterGap(entity, m.entity);
            return true;
        }
        return false;
    case proto::RegisterTalisman::kId:
        if (proto::RegisterTalisman m; proto::Decode(payload, m)) {
            s.Register(entity, m.slot);
            return true;
        }
        return false;
    case proto::LeaveInstance::kId:
        if (proto::LeaveInstance m; proto::Decode(payload, m)) {
            s.LeaveBy(entity, m.exit);
            return true;
        }
        return false;
    case proto::OpenSummon::kId:
        if (proto::OpenSummon m; proto::Decode(payload, m)) {
            s.OpenSummon(entity, m.pillar);
            return true;
        }
        return false;
    case proto::SummonMember::kId:
        if (proto::SummonMember m; proto::Decode(payload, m)) {
            s.SummonMember(entity, m.character);
            return true;
        }
        return false;
    case proto::SummonReply::kId:
        if (proto::SummonReply m; proto::Decode(payload, m)) {
            s.SummonReply(entity, m.yes);
            return true;
        }
        return false;
    default: return false;
    }
}

bool Instances::UseItem(std::uint32_t entity, std::uint8_t slot, const assets::ItemRow& row) {
    State& s = *s_;
    if (const auto hole = s.catalog->holes.find(row.refObjId); hole != s.catalog->holes.end()) {
        s.UseHole(entity, slot, hole->second);
        return true;
    }
    if (s.catalog->talismans.contains(row.refObjId)) {
        s.Register(entity, slot);
        return true;
    }
    return false;
}

void Instances::Killed(std::uint32_t killer, const Monsters::Damaged& killed, const proto::WorldPosition& at) {
    State& s = *s_;
    State::Event e;
    e.kind = "EVENT_MONSTER_KILL";
    e.user = killer;
    e.monster = killed.refObjId;
    e.rarity = killed.rarity;
    e.nest = killed.nest;
    e.at = at;
    const auto now = Clock::now();
    if (at.layer == 0) {
        s.Raise(s.catalog->sharedTriggers, s.sharedFired, s.sharedDelayed, e, nullptr, now);
        return;
    }
    if (State::Instance* inst = s.Of(at.layer)) s.Raise(inst->spec->triggers, inst->fired, inst->delayed, e, inst, now);
}

void Instances::Arrived(std::uint32_t entity) {
    State& s = *s_;
    const auto it = s.players.find(entity);
    if (it == s.players.end() || it->second.layer == 0) return;
    State::Instance* inst = s.Of(it->second.layer);
    if (inst == nullptr) return;
    it->second.arriving = false;
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(inst->ends - Clock::now()).count();
    s.Send(entity, s.StateFor(it->second, inst->spec->id, std::uint32_t(std::max<std::int64_t>(0, left))));
    State::Event e;
    e.kind = "EVENT_WORLD_ENTRANCE";
    e.user = entity;
    s.Raise(inst->spec->triggers, inst->fired, inst->delayed, e, inst, Clock::now());
}

std::uint32_t Instances::OpenSiege(const std::string& opener, std::uint32_t seconds) {
    State& s = *s_;
    if (!s.catalog->siege) return 0;
    InstanceHost::Player master;
    master.name = opener;
    State::Instance* inst = s.Open(*s.catalog->siege, HoleSpec{}, master, 0, {}, 0.0f, Clock::now());
    if (inst == nullptr) return 0;
    if (seconds != 0) inst->ends = Clock::now() + std::chrono::seconds(seconds);
    inst->gapEnds = Clock::now(); // no gap: its empty time counts from the start
    return inst->layer;
}

bool Instances::AdmitSiege(std::uint32_t entity, std::uint32_t layer) {
    State& s = *s_;
    State::Instance* inst = s.Of(layer);
    const auto it = s.players.find(entity);
    const auto place = s.world.Where(entity);
    if (inst == nullptr || it == s.players.end() || !place || it->second.layer != 0) return false;
    s.Admit(entity, it->second, *inst, place->position);
    return true;
}

bool Instances::ReadmitsSiege(std::uint32_t entity, std::uint32_t layer) const {
    const State& s = *s_;
    const auto it = s.players.find(entity);
    const auto inst = s.instances.find(layer);
    if (it == s.players.end() || inst == s.instances.end() || it->second.entries.empty()) return false;
    // Its latest entry (by the time of entering).
    const InstanceEntry* latest = &it->second.entries.front();
    for (const InstanceEntry& e : it->second.entries) {
        if (e.last > latest->last) latest = &e;
    }
    const InstanceEntry& last = *latest;
    return last.world == inst->second.spec->id && last.layer == layer && last.key == inst->second.key;
}

void Instances::ExitSiege(std::uint32_t entity, const proto::WorldPosition* to) {
    if (to == nullptr) return s_->Exit(entity, false);
    s_->Exit(entity, true);
    s_->host.InstanceTeleport(entity, *to);
}

bool Instances::Stands(std::uint32_t layer) const { return s_->instances.contains(layer); }

std::size_t Instances::Inside(std::uint32_t layer) const {
    const auto it = s_->instances.find(layer);
    return it != s_->instances.end() ? it->second.inside.size() : 0;
}

std::uint32_t Instances::SiegeMaxUsers() const { return s_->catalog->siege ? s_->catalog->siege->maxUsers : 0; }

std::uint32_t Instances::OpenQuestWorld(const std::string& codename, std::uint32_t worldId, const std::string& opener) {
    State& s = *s_;
    const GameWorldSpec* spec = nullptr;
    for (const GameWorldSpec& w : s.questWorlds) {
        if (w.codename == codename) spec = &w;
    }
    if (spec == nullptr) {
        // _RefGame_World's quest rows: 100 instances, 8 players, no duration nor empty time of their own (the quest ends it).
        GameWorldSpec w;
        w.id = worldId;
        w.codename = codename;
        w.maxCount = 100;
        w.maxUsers = 8;
        w.durationMs = 7'200'000;
        w.emptyMs = 0;
        w.gapMs = 0;
        s.questWorlds.push_back(std::move(w));
        spec = &s.questWorlds.back();
    }
    InstanceHost::Player master;
    master.name = opener;
    State::Instance* inst = s.Open(*spec, HoleSpec{}, master, 0, {}, 0.0f, Clock::now());
    if (inst == nullptr) return 0;
    inst->gapEnds = Clock::now();
    return inst->layer;
}

proto::GateStatus Instances::EnterTomb(std::uint32_t entity, const GameWorldSpec& world) { return s_->EnterTomb(entity, world); }
const GameWorldSpec* Instances::TombOfPoint(std::uint32_t point) const { return s_->catalog->TombOfPoint(point); }

std::uint32_t Instances::OpenWorld(const GameWorldSpec& spec, const std::string& opener) {
    State& s = *s_;
    const GameWorldSpec* kept = nullptr;
    for (GameWorldSpec& w : s.playedWorlds) {
        if (w.id == spec.id) {
            w = spec; // its instances read it: the same world's spec, refreshed in place
            kept = &w;
        }
    }
    if (kept == nullptr) {
        s.playedWorlds.push_back(spec);
        kept = &s.playedWorlds.back();
    }
    InstanceHost::Player master;
    master.name = opener;
    State::Instance* inst = s.Open(*kept, HoleSpec{}, master, 0, {}, 0.0f, Clock::now());
    if (inst == nullptr) return 0;
    inst->gapEnds = Clock::now();
    return inst->layer;
}

void Instances::CloseWorld(std::uint32_t layer) { s_->Close(layer); }

std::vector<std::uint32_t> Instances::PlayersIn(std::uint32_t layer) const {
    const auto it = s_->instances.find(layer);
    return it != s_->instances.end() ? std::vector<std::uint32_t>(it->second.inside.begin(), it->second.inside.end()) : std::vector<std::uint32_t>{};
}

std::uint32_t Instances::AddExit(std::uint32_t layer, std::uint32_t refObjId, const proto::WorldPosition& at, float yaw) {
    State::Instance* inst = s_->Of(layer);
    if (inst == nullptr) return 0;
    const std::uint32_t entity = s_->world.AddNpc(refObjId, State::InLayer(at, layer), yaw);
    if (entity == 0) return 0;
    inst->npcs.push_back(entity);
    inst->exits.push_back(entity);
    return entity;
}

bool Instances::Transfer(std::uint32_t entity, std::uint32_t layer, const proto::WorldPosition& at) {
    State& s = *s_;
    const auto it = s.players.find(entity);
    State::Instance* to = s.Of(layer);
    if (it == s.players.end() || it->second.layer == 0 || to == nullptr || it->second.layer == layer) return false;
    if (State::Instance* from = s.Of(it->second.layer)) {
        from->inside.erase(entity);
        if (from->inside.empty()) from->emptySince = Clock::now();
    }
    const proto::WorldPosition back = it->second.back;
    s.Admit(entity, it->second, *to, back, &at);
    return true;
}

bool Instances::AdmitAt(std::uint32_t entity, std::uint32_t layer, const proto::WorldPosition& at) {
    State& s = *s_;
    State::Instance* inst = s.Of(layer);
    const auto it = s.players.find(entity);
    const auto place = s.world.Where(entity);
    if (inst == nullptr || it == s.players.end() || !place || it->second.layer != 0) return false;
    s.Admit(entity, it->second, *inst, place->position, &at);
    return true;
}

void Instances::Tick(Clock::time_point now) {
    State& s = *s_;
    // Players inside: landed (their entrance), or out by another way (a return scroll, a resurrection in town).
    for (auto& [entity, p] : s.players) {
        if (p.layer == 0) continue;
        const auto place = s.world.Where(entity);
        if (!place) continue; // on its way
        if (place->position.layer != p.layer) {
            if (!p.arriving) s.Exit(entity, true);
            continue;
        }
        if (p.arriving) Arrived(entity);
    }
    std::vector<std::uint32_t> over;
    for (auto& [layer, inst] : s.instances) {
        if (inst.gap != 0 && now >= inst.gapEnds) {
            s.world.Leave(inst.gap);
            inst.gap = 0;
        }
        s.RunDelayed(inst.spec->triggers, inst.delayed, &inst, now);
        // Its end's warnings (the tomb's GAMEWORLD_DISAPPEARANCE_TIME: 30, 10 and 5 minutes left).
        while (inst.warned < inst.spec->warnMs.size() && now >= inst.ends - std::chrono::milliseconds(inst.spec->warnMs[inst.warned])) {
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(inst.ends - now).count();
            for (const std::uint32_t entity : inst.inside) s.Send(entity, proto::InstanceAlert{std::uint32_t(std::max<std::int64_t>(0, left))});
            ++inst.warned;
        }
        if (now >= inst.ends) {
            over.push_back(layer);
        } else if (inst.inside.empty() && inst.gap == 0 && now - inst.emptySince >= std::chrono::milliseconds(inst.spec->emptyMs)) {
            over.push_back(layer);
        }
    }
    for (const std::uint32_t layer : over) s.Close(layer);
    s.RunDelayed(s.catalog->sharedTriggers, s.sharedDelayed, nullptr, now);
    std::erase_if(s.summons, [&](const auto& kv) { return now > kv.second.expires; });
    std::erase_if(s.asked, [&](const auto& kv) { return now > kv.second.expires; });
    std::erase_if(s.arrivals, [&](const auto& kv) { return now > kv.second.expires; });
}

std::optional<proto::WorldPosition> Instances::ReturnPlace(std::uint32_t entity) const {
    const auto it = s_->players.find(entity);
    if (it == s_->players.end() || it->second.layer == 0) return std::nullopt;
    return it->second.back;
}

std::optional<proto::WorldPosition> Instances::StartOf(std::uint32_t entity) const {
    const auto it = s_->players.find(entity);
    if (it == s_->players.end() || it->second.layer == 0) return std::nullopt;
    const auto inst = s_->instances.find(it->second.layer);
    if (inst == s_->instances.end()) return std::nullopt;
    return State::InLayer(inst->second.spec->start, inst->first);
}

bool Instances::ThemeComplete(std::uint32_t entity, std::string_view theme) const {
    const auto it = s_->players.find(entity);
    const CollectionTheme* t = s_->catalog->Theme(theme);
    if (it == s_->players.end() || t == nullptr) return false;
    std::size_t have = 0;
    for (std::uint8_t slot = 1; slot <= t->complete; ++slot) have += it->second.book.contains(BookKey(t->id, slot)) ? 1 : 0;
    return have >= t->complete;
}

std::uint32_t Instances::WorldOf(std::uint32_t entity) const {
    const auto it = s_->players.find(entity);
    if (it == s_->players.end() || it->second.layer == 0) return 0;
    const auto inst = s_->instances.find(it->second.layer);
    return inst != s_->instances.end() ? inst->second.spec->id : 0;
}

std::size_t Instances::Count() const { return s_->instances.size(); }

std::uint32_t Instances::GapOf(std::uint32_t master) const {
    for (const auto& [layer, inst] : s_->instances) {
        if (inst.masterEntity == master && inst.gap != 0) return inst.gap;
    }
    return 0;
}

std::uint32_t Instances::LayerOf(std::uint32_t entity) const {
    const auto it = s_->players.find(entity);
    return it != s_->players.end() ? it->second.layer : 0;
}

bool Instances::DoorOpen(std::uint32_t layer, const std::string& zone) const {
    const auto it = s_->instances.find(layer);
    if (it == s_->instances.end()) return false;
    for (std::size_t d = 0; d < it->second.spec->doors.size(); ++d) {
        if (it->second.spec->doors[d].zone == zone) return it->second.doors[d].open;
    }
    return false;
}

void Instances::BusMessage(const std::string& text) {
    const auto bytes = BusFromHex(text);
    SummonBus m;
    if (bytes && proto::Decode(*bytes, m)) s_->OnBus(m);
}

bool Instances::AtPillar(std::uint32_t entity, std::uint32_t pillar) { return s_->AtPillar(entity, pillar) != nullptr; }

std::vector<std::uint32_t> Instances::PillarsOf(std::uint32_t layer) const {
    const auto it = s_->instances.find(layer);
    return it != s_->instances.end() ? it->second.pillars : std::vector<std::uint32_t>{};
}

std::vector<std::uint32_t> Instances::ExitsOf(std::uint32_t layer) const {
    const auto it = s_->instances.find(layer);
    return it != s_->instances.end() ? it->second.exits : std::vector<std::uint32_t>{};
}

std::size_t Instances::NestOf(std::uint32_t layer, std::uint32_t nestId) const {
    const auto it = s_->instances.find(layer);
    if (it == s_->instances.end()) return SIZE_MAX;
    const auto n = it->second.nestById.find(nestId);
    return n != it->second.nestById.end() ? n->second : SIZE_MAX;
}

} // namespace sr::server
