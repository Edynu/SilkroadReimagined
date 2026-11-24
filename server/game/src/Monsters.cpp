#include "sr/server/Monsters.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <cctype>
#include <numbers>
#include <optional>
#include <random>
#include <span>
#include <unordered_map>
#include <utility>

#include "sr/assets/GameData.h"
#include "sr/core/Log.h"
#include "sr/core/Trace.h"
#include "sr/game/Jobs.h"
#include "sr/server/World.h"

namespace sr::server {

namespace proto = game::proto;

namespace {

constexpr float kPlayerRadius = 5.0f;            // a player's body in reaches (ours)
constexpr float kGiveUpDistance = 500.0f;        // from its target (TraceData's most common: a pending hit's reach, the bandits')
constexpr auto kRecoveryTick = std::chrono::seconds(4); // a monster's natural recovery, every 4 s while it stands
constexpr double kChaseLeg = 300.0;              // a chase's longest leg toward where its target runs (ours)
constexpr auto kCorpse = std::chrono::seconds(5); // a killed monster lies this long (ours)

float Distance(const proto::WorldPosition& a, const proto::WorldPosition& b) {
    const proto::FlatPosition p = proto::ToFlat(a), q = proto::ToFlat(b);
    return float(std::hypot(p.x - q.x, p.z - q.z));
}

} // namespace

std::optional<proto::WorldPosition> MonsterWayRound(
    const proto::WorldPosition& from, const proto::WorldPosition& to,
    const std::function<std::optional<proto::WorldPosition>(const proto::WorldPosition&, const proto::WorldPosition&)>& walkEnd) {
    const auto reached = [&](const proto::WorldPosition& goal) {
        const auto end = walkEnd(from, goal);
        return end && Distance(*end, goal) <= 1.0f;
    };
    if (reached(to)) return std::nullopt;
    const proto::FlatPosition a = proto::ToFlat(from), b = proto::ToFlat(to);
    const double dx = b.x - a.x, dz = b.z - a.z, length = std::hypot(dx, dz);
    if (length < 1.0) return std::nullopt;
    // Its bucket: the reversed step's angle from the x axis, 0..180 degrees in tens (AI_Vec3_AngleBucket10Deg: the step
    // times -1 against (1, 0, 0), so the side of z is lost); the directions tried, the table's (cos t, -sin t) at
    // t = 5 + 10 k degrees for k = bucket + 1 .. bucket + 35 (AI_BuildDirectionVectorTable), each for min(length, 15).
    const double degree = std::numbers::pi / 180.0, probe = std::min(length, 15.0);
    const double alpha = std::acos(std::clamp(-dx / length, -1.0, 1.0)) / degree;
    const int bucket = std::clamp(int(alpha / 10.0), 0, 35);
    for (int i = 1; i < 36; ++i) {
        const double t = (5.0 + 10.0 * double((bucket + i) % 36)) * degree;
        const double ux = std::cos(t), uz = -std::sin(t);
        proto::WorldPosition probeTo = proto::FromFlat({a.x + ux * probe, a.z + uz * probe}, from.y);
        probeTo.layer = from.layer;
        if (!reached(probeTo)) continue;
        proto::WorldPosition step = proto::FromFlat({a.x + ux * 20.0, a.z + uz * 20.0}, from.y);
        step.layer = from.layer;
        return step;
    }
    return std::nullopt;
}

struct Monsters::State {
    World& world;
    const assets::CharacterTable& characters;
    const assets::SkillTable* skills = nullptr;
    struct Nest {
        MonsterNest spec;
        const assets::ObjectRow* row = nullptr;
        std::vector<const assets::SkillRow*> attacks; // its skills that hit
        game::StateDefence defence;  // against states: its level and Resist columns
        bool canFall = false, canPush = false; // its Knockdown bits
        int recoverMs = 0;           // KO_RecoverTime
        bool active = false;
        std::vector<std::uint32_t> alive;
        Clock::time_point nextRefill;
        int left = 0;       // a nest that does not refill: the monsters it has still to spawn
        int bodies = 0;     // its monsters standing or lying (a temporary nest goes with its last)
        bool partyNext = false; // a party's kill marked it: its next hatchings may give a party monster
        int crowdHive = -1;      // its crowded hunting ground (State::crowds)
        Clock::time_point refillFrom; // when its refill's delay began (a crowd's new rate rolls it again from there)
        bool removed = false;
        // Its monsters' summons (their skills 'ssou': groups of four, the monster, its rarity, the fewest and the most
        // raised). A monster that only summons (a seal stone, a
        // Dimension Pillar) calls them when it is first hit; one that also fights uses each summon skill once, as its
        // health falls to the skill's band: the band its name
        // ends with (MSKILL_RM_TAHOMET_SUMMON_R1_80 .. _00), else (ours) its skills' bands evenly spaced (the Roc's three:
        // 75, 50, 25 %).
        struct Summon {
            std::uint32_t refObjId = 0;
            int rarity = 0;
            int fewest = 1, most = 1;
        };
        struct SummonSkill {
            std::vector<Summon> groups;
            int band = -1; // the health percent it is used at (-1: unnamed, spaced evenly)
        };
        std::vector<SummonSkill> summons;
        // Its death skills ('D'): one drawn by their chances when it dies with a killer, cast on him (ours: a summon's
        // monsters raised; another kind's effect not yet). `chance` its AI_AttackChance.
        struct DeathSkill {
            const assets::SkillRow* skill = nullptr;
            SummonSkill summon;
            int chance = 0;
        };
        std::vector<DeathSkill> deaths;
        // Its conditional skills (Monsters::SetConditionalSkills), in order.
        struct Conditional {
            const assets::SkillRow* skill = nullptr;
            SummonSkill summon;
            int kind = 0;
            std::uint32_t data = 0;
            int option = 0;
        };
        std::vector<Conditional> conditionals;
        // A unique's nest (MonsterNest::hiveMax): its hive, whether the hive's monster is here (in the world, or
        // waiting for the region to wake), and when it may hatch here.
        int hive = -1;
        bool held = false;
        Clock::time_point due{};
        // An ordinary nest of a capped hive (MonsterNest::hiveMax, not a unique's): the hive's cap over its nests' monsters
        // alive (ours: only those of awake nests are alive); `due` its next hatch's time once picked at a death.
        int capHive = -1;
    };
    // The world's unique hives: one state the whole time, the regions awake or not.
    struct Hive {
        int max = 1;
        std::vector<std::size_t> nests;  // by the original's nest id
        int alive = 0;                   // its monsters that exist
        std::optional<std::size_t> next; // where the next hatches (picked at a death; none: the first ready in order)
    };
    std::vector<Hive> hives;
    // The ordinary capped hives (Tab_RefHive dwOverwriteMaxTotalCount over nests of other monsters than uniques).
    struct CapHive {
        int max = 0;
        std::vector<std::size_t> nests;  // by the original's nest id
        std::optional<std::size_t> next; // where the next hatches (picked at a death)
    };
    std::vector<CapHive> capHives;
    std::vector<int> capFills; // the capped hives a wake brought nests of (filled at its end)
    // The crowded hunting grounds (MonsterNest::crowdPlayers): every 30 s the players in its nests' regions summed (a
    // player counted for each of its nests there, as the original's per nest), two samples' half-sum kept in turn in a
    // ring of two, their average the players; the rate round(players / made-for) x step, within 0 and its most, set on
    // its nests when it changes.
    struct Crowd {
        std::uint32_t hive = 0;
        std::vector<std::size_t> nests;
        int madeFor = 1, step = 0, most = 0;
        int sum = 0, samples = 0, slot = 0;
        std::array<int, 2> ring{};
        int players = 0;
        int rate = 0;
    };
    std::vector<Crowd> crowds;
    Clock::time_point nextCrowd{};
    void GroupCrowds() {
        std::map<std::uint32_t, std::size_t> byHive;
        for (std::size_t n = 0; n < nests.size(); ++n) {
            const MonsterNest& spec = nests[n].spec;
            if (spec.crowdPlayers <= 0 || spec.hive == 0) continue;
            auto [it, fresh] = byHive.try_emplace(spec.hive, crowds.size());
            if (fresh) crowds.push_back({spec.hive, {}, spec.crowdPlayers, spec.crowdStep, spec.crowdMax});
            crowds[it->second].nests.push_back(n);
            nests[n].crowdHive = int(it->second);
        }
        if (!crowds.empty()) {
            std::size_t over = 0;
            for (const Crowd& c : crowds) over += c.nests.size();
            core::LogInfo("monsters: {} crowded hunting grounds over {} nests", crowds.size(), over);
        }
    }
    // A sample of every crowd (Update's every 30 s): its players and its rate; a nest whose rate changed rolls its
    // refill's delay again from its start.
    void SampleCrowds(const std::vector<Monsters::Player>& players, Clock::time_point now) {
        if (crowds.empty()) return;
        std::unordered_map<std::uint16_t, int> perRegion;
        for (const Monsters::Player& p : players) ++perRegion[p.position.region];
        for (Crowd& c : crowds) {
            int count = 0;
            for (const std::size_t n : c.nests) {
                if (const auto it = perRegion.find(nests[n].spec.position.region); it != perRegion.end()) count += it->second;
            }
            ++c.samples;
            c.sum += count;
            if (c.samples > 1) {
                c.ring[std::size_t(c.slot)] = c.sum >> 1;
                c.slot ^= 1;
                c.sum = 0;
                c.players = (c.ring[0] + c.ring[1]) >> 1;
                c.samples = 0;
            }
            const int rate = std::clamp(int(std::nearbyint(double(c.players) / double(std::max(1, c.madeFor)))) * c.step, 0, std::max(0, c.most));
            if (rate == c.rate) continue;
            c.rate = rate;
            for (const std::size_t n : c.nests) {
                Nest& nest = nests[n];
                if (nest.active && nest.spec.respawn) nest.nextRefill = nest.refillFrom + Seconds(Delay(nest));
            }
        }
    }
    // A nest's delay before its next hatch, in seconds: rolled within its least and most, less its crowd's cut (the
    // original's in whole milliseconds).
    float Delay(const Nest& nest) {
        const float roll = Uniform(nest.spec.delayMin, nest.spec.delayMax);
        if (nest.crowdHive < 0) return roll;
        const double ms = std::trunc(double(roll) * 1000.0);
        const double cut = std::nearbyint(ms * double(crowds[std::size_t(nest.crowdHive)].rate) / 100.0);
        return float((ms - cut) / 1000.0);
    }
    // Its refill's delay begun now.
    void RollRefill(Nest& nest, Clock::time_point now) {
        nest.refillFrom = now;
        nest.nextRefill = now + Seconds(Delay(nest));
    }
    enum class Mode : std::uint8_t { Idle, Chase, Home, Dead };
    struct Monster {
        std::uint32_t entity = 0;
        std::size_t nest = 0;
        std::uint8_t rarity = 0;
        bool championRow = false; // a champion or a giant of the roll: it fights by its tactics' champion row
        std::uint32_t hp = 0;
        game::CombatStats stats;
        Mode mode = Mode::Idle;
        std::uint32_t target = 0;
        int slot = -1;                 // its place about its target (State::approach; -1: none, it goes for the target itself)
        std::uint32_t slotTarget = 0;  // whom that place is about
        proto::WorldPosition aim; // where its chase last headed
        Clock::time_point nextThink, nextSwing, goneAt;
        Clock::time_point swingEnds; // it stands through its swing's clip: no chase before then
        Clock::time_point nextLook; // an idle one's next look around for a player
        std::uint32_t summoned = 0; // its nest's summon skills used (a bit each)
        std::uint32_t body = 0;     // a god's linked loot body (its _DROP summon)
        Clock::time_point nextRecovery; // its natural recovery's next tick
        Clock::time_point lastStrike;   // its last swing (a chaser that has not struck for 5 s walks to a far target)
        // Its conditional skills' state: a health one used (once), an interval one's last time (none: not looked at yet).
        std::vector<bool> conditionalUsed;
        std::vector<std::optional<Clock::time_point>> conditionalAt;
        struct PendingHit {
            Clock::time_point at;
            std::uint32_t target = 0;
            game::AttackSpec attack;
            float reach = 0;
            const assets::SkillRow* skill = nullptr; // its swing's skill: the states it gives the player
        };
        std::vector<PendingHit> pending;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> damageBy; // since it last went home
        // Its memory of opponents: the current target's record and a second
        // one, each the damage dealt, the threat and when it last grew; wiped as it idles or goes home.
        struct Opponent {
            std::uint32_t entity = 0;
            std::uint64_t damage = 0;
            std::int64_t threat = 0;
            Clock::time_point tick;
        };
        Opponent foe, rival; // the current target's record, the second
        int waitMs = 1000; // its last swing's wait (a current target silent for twice it is dropped for the second)
        // States (K3): the set, its own chance history (a stun's one in four to break), its numbers
        // with the states' changes, its speed's percent; a knock and until when (down: it lies).
        game::StateSet states;
        struct Debuff {
            const assets::SkillRow* skill = nullptr;
            Clock::time_point until;
            std::vector<game::StatChange> changes;
        };
        std::vector<Debuff> debuffs; // buffs' changes on it (K4)
        game::ChanceRolls rolls;
        game::CombatStats current;
        int speedPercent = 100;
        bool knocked = false, down = false;
        Clock::time_point knockedUntil;
        // A bandit's: gone without dying then (its nest's leaveMs after its last hit given or taken).
        Clock::time_point leaveAt = Clock::time_point::max();
    };
    std::vector<Nest> nests;
    std::function<bool(std::uint16_t)> town; // a bandit entering one is gone
    std::function<bool(const proto::WorldPosition&)> townArea; // a town's safe area (regioninfo's, by position)
    std::unordered_map<std::string, proto::WorldPosition> summonPlaces; // a summon's own spot by its codename
    std::unordered_map<std::uint32_t, std::vector<Monsters::ConditionalSkill>> conditionalSkills; // by the monster's refObjId
    // A nest's conditional skills from the table (its monster's).
    void FillConditionals(Nest& nest) {
        nest.conditionals.clear();
        const auto it = conditionalSkills.find(nest.spec.refObjId);
        if (it == conditionalSkills.end() || skills == nullptr) return;
        for (const Monsters::ConditionalSkill& c : it->second) {
            const assets::SkillRow* skill = skills->Find(std::string_view(c.skill));
            if (skill == nullptr) continue;
            Nest::Conditional entry{skill, {}, c.kind, c.data, c.option};
            assets::ForEachSkillParam(*skill, [&](std::uint32_t tag, std::span<const std::uint32_t> args) {
                if (tag != assets::SkillTag("ssou")) return;
                for (std::size_t g = 0; g + 1 < args.size() && args[g] != 0; g += 4) {
                    const std::uint32_t fewest = g + 2 < args.size() ? args[g + 2] : 1;
                    const std::uint32_t most = g + 3 < args.size() ? std::max(args[g + 3], fewest) : fewest;
                    entry.summon.groups.push_back({args[g], int(args[g + 1] & 0x0F), int(std::clamp<std::uint32_t>(fewest, 1, 50)),
                                                   int(std::clamp<std::uint32_t>(most, 1, 50))});
                }
            });
            nest.conditionals.push_back(std::move(entry));
        }
    }
    // Its first conditional skill ready now (the original's in order): a health one once at its share or below, an
    // interval one every `data` ms (its first look only stamps the time); -1: none.
    int ReadyConditional(Monster& m, const Nest& nest, Clock::time_point now) {
        m.conditionalUsed.resize(nest.conditionals.size(), false);
        m.conditionalAt.resize(nest.conditionals.size());
        for (std::size_t i = 0; i < nest.conditionals.size(); ++i) {
            const Nest::Conditional& c = nest.conditionals[i];
            if (c.kind == 0) {
                if (m.conditionalUsed[i] || m.stats.maxHp == 0) continue;
                if (double(m.hp) * 100.0 / double(m.stats.maxHp) > double(c.data)) continue;
                m.conditionalUsed[i] = true;
                return int(i);
            }
            if (c.kind == 1) {
                if (!m.conditionalAt[i]) {
                    m.conditionalAt[i] = now;
                    continue;
                }
                if (now - *m.conditionalAt[i] < std::chrono::milliseconds(c.data)) continue;
                m.conditionalAt[i] = now;
                return int(i);
            }
        }
        return -1;
    }
    // Town ground: a town's region (`_RefRegion.IsBattleField` 0) or a town's safe area about it (the original's
    // regioninfo's rectangles of kind 2); never in an instance's layer.
    bool InTown(const proto::WorldPosition& at) const {
        if (at.layer != 0) return false;
        return (town && town(at.region)) || (townArea && townArea(at));
    }
    std::vector<std::size_t> freeNests; // removed nests' places, taken again by AddNest
    std::vector<MonsterNest> summons;   // raised by hits this update, added at the next
    std::unordered_map<std::uint16_t, std::vector<std::size_t>> byRegion; // the nests of each region
    std::unordered_map<std::uint32_t, Monster> monsters;
    // How many monsters fight each player now (worked out each think): at most the world's squad take one
    // (MAX_SQUAD_MEMBER_COUNT: 6 in the shared world, 30 in an instance's data-driven world; the original's AddMember).
    std::unordered_map<std::uint32_t, int> chasers;
    static int SquadLimit(const proto::WorldPosition& at) { return at.layer == 0 ? 6 : 30; }
    // The places about a target its attackers take, as the original's positioner keeps them: eight, one each 45 degrees
    // from 22, turning the other way round (clockwise as atan2 counts), each held by one monster; one comes to the place on
    // the side it comes from, else the nearest free one to either side; with all eight held it still goes for its own
    // preferred place, shared; a nearer newcomer takes over a held place; none on another plane or beyond the next
    // regions (upstream's review 2026-11-22). A place is held while its monster chases that target from it (an
    // entry left by one gone or turned to another is free: HeldBy).
    static constexpr int kApproachPlaces = 8;
    std::unordered_map<std::uint32_t, std::array<std::uint32_t, kApproachPlaces>> approach;
    bool HeldBy(std::uint32_t target, int place, std::uint32_t entity) const {
        const auto a = approach.find(target);
        if (a == approach.end() || a->second[std::size_t(place)] != entity || entity == 0) return false;
        const auto m = monsters.find(entity);
        return m != monsters.end() && m->second.mode == Mode::Chase && m->second.target == target && m->second.slot == place &&
               m->second.slotTarget == target;
    }
    // A place's angle in degrees as atan2 counts (place p at 22 + 45 p, the other way round).
    static double ApproachAngle(int place) { return -(22.0 + 45.0 * place); }
    // Its place about its target (taken now when it has none about this one); all eight held: its preferred one, shared.
    // -1: the two not on the same plane or in regions not next to each other (it goes for the target's own spot).
    int ApproachPlace(Monster& m, const proto::WorldPosition& self, const proto::WorldPosition& target) {
        const int rx = (self.region & 0xFF) - (target.region & 0xFF), rz = ((self.region >> 8) & 0x7F) - ((target.region >> 8) & 0x7F);
        const bool dungeon = ((self.region | target.region) & 0x8000) != 0;
        if (self.layer != target.layer || (dungeon && self.region != target.region) || std::abs(rx) > 1 || std::abs(rz) > 1) {
            m.slot = -1;
            m.slotTarget = 0;
            return -1;
        }
        if (m.slot >= 0 && m.slotTarget == m.target && HeldBy(m.target, m.slot, m.entity)) return m.slot;
        m.slot = -1;
        m.slotTarget = 0;
        const proto::FlatPosition a = proto::ToFlat(self), b = proto::ToFlat(target);
        const float mine = Distance(self, target);
        // The side it comes from (inferred: the place nearest its bearing from the target), counted the places' way round.
        const double bearing = -std::atan2(double(a.z - b.z), double(a.x - b.x)) * 180.0 / std::numbers::pi;
        const int preferred = int(std::floor(std::fmod(bearing + 0.5 + 720.0, 360.0) / 45.0)) % kApproachPlaces;
        auto& places = approach[m.target];
        for (int k = 0; k < kApproachPlaces; ++k) {
            // preferred, then one to each side, then two, ...
            const int step = (k + 1) / 2, place = ((k % 2 == 1 ? preferred + step : preferred - step) % kApproachPlaces + kApproachPlaces) % kApproachPlaces;
            if (const std::uint32_t holder = places[std::size_t(place)]; HeldBy(m.target, place, holder)) {
                // A held place taken over by a newcomer nearer the target (the original's, inferred:
                // the nearer wins); the one it had takes another next time (HeldBy fails for it).
                const auto at = world.Where(holder);
                if (!at || Distance(at->position, target) <= mine) continue;
            }
            places[std::size_t(place)] = m.entity;
            m.slot = place;
            m.slotTarget = m.target;
            return place;
        }
        return preferred;
    }
    // The point of place `place` about `target`, `distance` out (the original's: 0.8 of the attacker's reach).
    static proto::WorldPosition ApproachPoint(const proto::WorldPosition& target, int place, float distance) {
        const double angle = ApproachAngle(place) * std::numbers::pi / 180.0;
        const proto::FlatPosition b = proto::ToFlat(target);
        return proto::FromFlat({float(b.x + distance * std::cos(angle)), float(b.z + distance * std::sin(angle))}, target.y);
    }
    std::size_t activeNests = 0;
    std::mt19937 random;
    Clock::time_point nextWake, nextThink;
    // This update's players (a copy: kept until the next update), by entity, and by region (aggressive monsters look
    // around their own).
    std::vector<Player> playerList;
    std::unordered_map<std::uint32_t, const Player*> players;
    std::unordered_map<std::uint16_t, std::vector<const Player*>> playersByRegion;

    State(World& w, const assets::CharacterTable& c, const assets::SkillTable* k, std::uint32_t seed)
        : world(w), characters(c), skills(k), random(seed) {}

    float Uniform(float a, float b) { return std::uniform_real_distribution<float>(a, b)(random); }
    int Percent() { return std::uniform_int_distribution<int>(0, 99)(random); }
    int Roll101() { return std::uniform_int_distribution<int>(0, 100)(random); } // the original's rand % 101
    static Clock::duration Seconds(float s) {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(s));
    }

    // Nests whose region's navigation came or went: filled on waking, emptied on sleeping.
    void Wake(Clock::time_point now) {
        for (auto& [region, list] : byRegion) {
            const bool loaded = world.NavigationLoaded(region);
            for (std::size_t n : list) {
                Nest& nest = nests[n];
                if (nest.spec.fixed || nest.removed || nest.spec.waiting || loaded == nest.active) continue;
                nest.active = loaded;
                if (loaded) {
                    ++activeNests;
                    Fill(n, now);
                    RollRefill(nest, now);
                } else {
                    --activeNests;
                    if (!nest.spec.respawn) nest.left += int(nest.alive.size()); // they come back with the navigation
                    for (std::uint32_t entity : nest.alive) {
                        world.Leave(entity);
                        monsters.erase(entity);
                        --nest.bodies;
                    }
                    nest.alive.clear();
                }
            }
        }
        // Corpses of sleeping nests go with them.
        std::vector<std::size_t> buried;
        for (auto it = monsters.begin(); it != monsters.end();) {
            if (it->second.mode == Mode::Dead && !nests[it->second.nest].active) {
                world.Leave(it->first);
                buried.push_back(it->second.nest);
                it = monsters.erase(it);
            } else {
                ++it;
            }
        }
        for (const std::size_t n : buried) Buried(n);
        for (const int h : std::exchange(capFills, {})) FillCapped(capHives[std::size_t(h)], now);
    }

    // A nest's gaps filled at once (one that does not refill: as many as it has left).
    void Fill(std::size_t n, Clock::time_point now) {
        Nest& nest = nests[n];
        if (nest.hive >= 0) {
            if (nest.held && nest.alive.empty()) Spawn(n, now);
            return;
        }
        // A capped hive's: once every nest of it this wake brings is up (Wake), in the hive's order.
        if (nest.capHive >= 0) {
            if (std::find(capFills.begin(), capFills.end(), nest.capHive) == capFills.end()) capFills.push_back(nest.capHive);
            return;
        }
        for (int i = int(nest.alive.size()); i < nest.spec.maxCount && (nest.spec.respawn || nest.left > 0); ++i) Spawn(n, now);
    }
    // A body of the nest gone (it left the world): a temporary nest done with goes.
    void Buried(std::size_t n) {
        Nest& nest = nests[n];
        --nest.bodies;
        if (nest.spec.temporary && nest.bodies <= 0 && !nest.removed && Cleared(nest)) Remove(n);
    }
    static bool Cleared(const Nest& nest) { return !nest.spec.respawn && !nest.spec.waiting && nest.left <= 0 && nest.alive.empty(); }
    void Remove(std::size_t n) {
        Nest& nest = nests[n];
        if (nest.removed) return;
        for (auto it = monsters.begin(); it != monsters.end();) {
            if (it->second.nest == n) {
                world.Leave(it->first);
                it = monsters.erase(it);
            } else {
                ++it;
            }
        }
        if (nest.active) --activeNests;
        nest.alive.clear();
        nest.active = false;
        nest.removed = true;
        nest.bodies = 0;
        auto& list = byRegion[nest.spec.position.region];
        list.erase(std::remove(list.begin(), list.end(), n), list.end());
        freeNests.push_back(n);
    }
    // A nest's state from its spec (the constructor's, AddNest's); false when its monster is unknown.
    bool Make(MonsterNest spec, Nest& out) {
        const assets::ObjectRow* row = characters.Find(spec.refObjId);
        if (row == nullptr || spec.maxCount <= 0) return false;
        spec.delayMin = std::max(1.0f, spec.delayMin);
        spec.delayMax = std::max(spec.delayMin, spec.delayMax);
        out = Nest{spec, row};
        // Against states: its level and Resist columns (none known: no resistance), its Knockdown bits.
        std::array<std::uint8_t, 32> resist{};
        if (resistances != nullptr) {
            if (const auto r = resistances->find(spec.refObjId); r != resistances->end()) resist = r->second;
        }
        out.defence = MonsterStateDefence(row->level, resist);
        out.canFall = (row->knockdown & 1) != 0;
        out.canPush = (row->knockdown & 2) != 0;
        out.recoverMs = row->knockdownRecoverMs;
        out.left = spec.maxCount;
        if (skills != nullptr) {
            for (std::uint32_t id : row->skills) {
                const assets::SkillRow* skill = id != 0 ? skills->Find(id) : nullptr;
                if (skill == nullptr) continue;
                // An appearance skill ('S', on spawning: ours not cast yet) is never swung.
                if (skill->aiType == 'S') continue;
                Nest::SummonSkill summon;
                assets::ForEachSkillParam(*skill, [&](std::uint32_t tag, std::span<const std::uint32_t> args) {
                    if (tag != assets::SkillTag("ssou")) return;
                    for (std::size_t g = 0; g + 1 < args.size() && args[g] != 0; g += 4) {
                        const std::uint32_t fewest = g + 2 < args.size() ? args[g + 2] : 1;
                        const std::uint32_t most = g + 3 < args.size() ? std::max(args[g + 3], fewest) : fewest;
                        summon.groups.push_back({args[g], int(args[g + 1] & 0x0F), int(std::clamp<std::uint32_t>(fewest, 1, 50)),
                                                 int(std::clamp<std::uint32_t>(most, 1, 50))});
                    }
                });
                // A death skill ('D'): kept for the death, never swung.
                if (skill->aiType == 'D') {
                    out.deaths.push_back({skill, std::move(summon), skill->aiChance});
                    continue;
                }
                if (!summon.groups.empty()) {
                    // Its band from its name's end (_80 .. _00).
                    const std::string& code = skill->codename;
                    if (code.size() > 3 && code[code.size() - 3] == '_' && std::isdigit(static_cast<unsigned char>(code[code.size() - 2])) &&
                        std::isdigit(static_cast<unsigned char>(code.back()))) {
                        summon.band = std::atoi(code.c_str() + code.size() - 2);
                    }
                    if (out.summons.size() < 32) out.summons.push_back(std::move(summon));
                    continue; // a summon skill is not swung
                }
                if (skill->attack) out.attacks.push_back(skill);
            }
            FillConditionals(out);
            // The unnamed bands spaced evenly over its health.
            const int n = int(out.summons.size());
            for (int i = 0; i < n; ++i) {
                if (out.summons[std::size_t(i)].band < 0) out.summons[std::size_t(i)].band = 100 * (n - i) / (n + 1);
            }
        }
        return true;
    }
    std::shared_ptr<const MonsterResistances> resistances; // the monsters' Resist columns (null: none known)

    void Spawn(std::size_t n, Clock::time_point now) {
        Nest& nest = nests[n];
        std::uint8_t rarity = std::uint8_t(nest.row->rarity);
        bool championRow = false;
        if (nest.spec.rarity >= 0) {
            rarity = std::uint8_t(nest.spec.rarity);
        } else if (rarity != 1 && (rarity < 3 || rarity > 8)) {
            // An ordinary monster: a party monster at rand % 101 < 50 from a nest a party's kill marked (not a quest's
            // monster, TID4 4); then, where its tactics names a champion row, a champion or a giant (rand % 101 <= 14,
            // the original's start value) at the nest's rand % 101 < percent, fighting by that row.
            rarity = 0;
            if (nest.partyNext && nest.spec.partyMonsters && nest.row->typeId[3] != 4 && Roll101() < 50) rarity = 0x10;
            if (nest.spec.championPercent > 0 && Roll101() < nest.spec.championPercent) {
                rarity |= Roll101() <= 14 ? 4 : 1;
                championRow = true;
            }
        }
        World::MonsterLook look;
        look.model = nest.spec.refObjId;
        look.rarity = rarity;
        look.hp = nest.spec.labHp > 0 ? nest.spec.labHp : proto::MonsterMaxHp(nest.row->maxHp, rarity);
        look.walkSpeed = nest.row->walkSpeed;
        look.runSpeed = nest.row->runSpeed;
        // A thief or hunter monster's look, rand % 256 at each spawn.
        const auto& t = nest.row->typeId;
        if (t[0] == 1 && t[1] == 2 && t[2] == 1 && (t[3] == 2 || t[3] == 3)) look.appearance = std::uint8_t(std::uniform_int_distribution<int>(0, 255)(random));
        // A point in the spawn circle (even over its area); the nest's own point when a few miss the
        // floor.
        const proto::FlatPosition centre = proto::ToFlat(nest.spec.position);
        std::uint32_t entity = 0;
        if (nest.spec.fixed) entity = world.AddStructure(look, nest.spec.position, nest.spec.yaw);
        for (int attempt = 0; attempt < 4 && entity == 0 && !nest.spec.fixed; ++attempt) {
            const float r = nest.spec.spawnRadius * std::sqrt(Uniform(0.0f, 1.0f)), a = Uniform(0.0f, 2.0f * std::numbers::pi_v<float>);
            proto::WorldPosition at = attempt == 3 ? nest.spec.position
                                                   : proto::FromFlat({centre.x + r * std::sin(a), centre.z + r * std::cos(a)}, nest.spec.position.y);
            entity = world.AddMonster(look, at, Uniform(-std::numbers::pi_v<float>, std::numbers::pi_v<float>));
        }
        if (entity == 0) return;
        nest.alive.push_back(entity);
        ++nest.bodies;
        if (!nest.spec.respawn) --nest.left;
        Monster& m = monsters[entity];
        m.entity = entity;
        m.nest = n;
        m.rarity = rarity;
        m.championRow = championRow;
        if ((rarity >> 4) == 1) nest.partyNext = false; // its party monster came
        // A god's loot body: linked both ways (the god leads).
        if (nest.spec.linkTo != 0) {
            if (const auto god = monsters.find(nest.spec.linkTo); god != monsters.end()) god->second.body = entity;
        }
        m.stats = game::MonsterCombatStats(*nest.row, rarity);
        if (nest.spec.labHp > 0) m.stats.maxHp = nest.spec.labHp; // a lab monster's own health
        m.current = m.stats;
        m.hp = m.stats.maxHp;
        m.nextThink = now + Seconds(Uniform(0.5f, 6.0f));
        if (nest.spec.lab != proto::LabBehaviour::Normal) m.nextThink = now; // a lab monster flees at once (ours)
        m.nextLook = now + Seconds(Uniform(0.0f, 1.5f));
        if (nest.spec.leaveMs > 0) {
            m.leaveAt = now + std::chrono::milliseconds(nest.spec.leaveMs);
            m.nextLook = now; // it looks about at once (the transport is in sight)
        }
    }
    // A bandit's hit given or taken sets its time to leave again.
    void Rearm(Monster& m, Clock::time_point now) {
        if (const int ms = nests[m.nest].spec.leaveMs; ms > 0) m.leaveAt = now + std::chrono::milliseconds(ms);
    }
    // Gone without dying (life state 3: no killer, no drop, no experience; inferred): out of the world, its nest told.
    void Vanish(Monster& m) {
        world.Leave(m.entity);
        Nest& home = nests[m.nest];
        home.alive.erase(std::remove(home.alive.begin(), home.alive.end(), m.entity), home.alive.end());
    }

    // The unique hives' hatching: while a hive has fewer than its cap, its next hatches at the nest picked at the last
    // death once that nest's time has come, or, none
    // picked, at the first of its nests (by nest id) whose time has come. It exists from then on; it is in the
    // world while its region is awake.
    void HatchHives(Clock::time_point now, Events& out) {
        for (Hive& hive : hives) {
            while (hive.alive < hive.max) {
                std::optional<std::size_t> at;
                if (hive.next) {
                    if (nests[*hive.next].due <= now && !nests[*hive.next].held) at = hive.next;
                } else {
                    for (const std::size_t n : hive.nests) {
                        if (!nests[n].held && nests[n].due <= now) {
                            at = n;
                            break;
                        }
                    }
                }
                if (!at) break;
                Nest& nest = nests[*at];
                nest.held = true;
                ++hive.alive;
                hive.next.reset();
                out.uniques.push_back({nest.spec.refObjId, nest.spec.position});
                if (nest.active) Spawn(*at, now);
            }
        }
    }
    // A capped hive's monsters alive now (its awake nests').
    int CappedAlive(const CapHive& hive) const {
        int alive = 0;
        for (const std::size_t n : hive.nests) alive += int(nests[n].alive.size());
        return alive;
    }
    static bool Hatchable(const Nest& nest) { return nest.active && !nest.removed && int(nest.alive.size()) < nest.spec.maxCount; }
    // A capped hive's nests woken: filled in order (by nest id) up to the cap, each to its own count at most.
    void FillCapped(CapHive& hive, Clock::time_point now) {
        int alive = CappedAlive(hive);
        for (const std::size_t n : hive.nests) {
            while (alive < hive.max && Hatchable(nests[n])) {
                Spawn(n, now);
                ++alive;
            }
        }
    }
    // A capped hive's hatching (as the unique hives'): while below
    // its cap, at the nest picked at the last death once its time has come and it is below its own count; none picked,
    // at each of its nests in order whose time has come, until the cap. Each second, with the nests' refills.
    void HatchCapped(Clock::time_point now) {
        for (CapHive& hive : capHives) {
            int alive = CappedAlive(hive);
            if (alive >= hive.max) continue;
            if (hive.next) {
                Nest& nest = nests[*hive.next];
                if (nest.due > now) continue;
                if (Hatchable(nest)) {
                    Spawn(*hive.next, now);
                    ++alive;
                }
                hive.next.reset();
            }
            for (const std::size_t n : hive.nests) {
                while (alive < hive.max && nests[n].due <= now && Hatchable(nests[n])) {
                    Spawn(n, now);
                    ++alive;
                }
            }
        }
    }
    // A capped hive's monster killed: the next hatches at one of its nests picked at
    // random (rand() % (count + 1), the last when it falls past it), after that nest's delay.
    void CappedDeath(Nest& home, Clock::time_point now) {
        CapHive& hive = capHives[std::size_t(home.capHive)];
        const int count = int(hive.nests.size());
        int pick = std::uniform_int_distribution<int>(0, count)(random);
        if (pick >= count - 1) pick = count - 1;
        const std::size_t next = hive.nests[std::size_t(pick)];
        hive.next = next;
        nests[next].due = now + Seconds(Delay(nests[next])); // a crowded hunting ground's cut too
    }
    // A hive's monster killed: the next hatches at one of its nests picked at
    // random (rand() % (count + 1), the last when it falls past it, as the original), after that nest's delay.
    void HiveDeath(Nest& home, Clock::time_point now) {
        Hive& hive = hives[std::size_t(home.hive)];
        home.held = false;
        hive.alive = std::max(0, hive.alive - 1);
        const int count = int(hive.nests.size());
        int pick = std::uniform_int_distribution<int>(0, count)(random);
        if (pick >= count - 1) pick = count - 1;
        const std::size_t next = hive.nests[std::size_t(pick)];
        hive.next = next;
        nests[next].due = now + Seconds(Uniform(nests[next].spec.delayMin, nests[next].spec.delayMax));
    }
    // The world's unique nests grouped into their hives (the constructor's nests only: an instance's are its own).
    void GroupHives() {
        std::map<std::uint32_t, std::size_t> byId; // hive id -> index
        std::vector<std::size_t> unique;
        for (std::size_t n = 0; n < nests.size(); ++n) {
            const Nest& nest = nests[n];
            if (nest.row == nullptr || nest.row->rarity % 16 != 3 || nest.spec.fixed || nest.spec.temporary || nest.spec.waiting || !nest.spec.respawn)
                continue;
            unique.push_back(n);
        }
        std::sort(unique.begin(), unique.end(), [&](std::size_t a, std::size_t b) { return nests[a].spec.nestId < nests[b].spec.nestId; });
        for (const std::size_t n : unique) {
            Nest& nest = nests[n];
            const bool shared = nest.spec.hive != 0 && nest.spec.hiveMax > 0;
            std::size_t h = hives.size();
            if (const auto it = byId.find(nest.spec.hive); shared && it != byId.end()) {
                h = it->second;
            } else {
                hives.push_back({shared ? nest.spec.hiveMax : std::max(1, nest.spec.maxCount), {}, 0, std::nullopt});
                if (shared) byId[nest.spec.hive] = h;
            }
            hives[h].nests.push_back(n);
            nest.hive = int(h);
        }
        // The ordinary capped hives: a hive whose cap is below its nests' own counts together (the others change
        // nothing). Measured on the 1.188 export: 109 hives, their 1,180 nests 1,976 monsters, capped to 1,052.
        std::map<std::uint32_t, std::vector<std::size_t>> capped; // hive id -> its nests
        for (std::size_t n = 0; n < nests.size(); ++n) {
            const Nest& nest = nests[n];
            if (nest.hive >= 0 || nest.row == nullptr || nest.spec.hive == 0 || nest.spec.hiveMax <= 0 || nest.spec.fixed || nest.spec.temporary ||
                nest.spec.waiting || !nest.spec.respawn) {
                continue;
            }
            capped[nest.spec.hive].push_back(n);
        }
        for (auto& [id, list] : capped) {
            int total = 0;
            for (const std::size_t n : list) total += nests[n].spec.maxCount;
            const int max = nests[list.front()].spec.hiveMax;
            if (max >= total) continue;
            std::sort(list.begin(), list.end(), [&](std::size_t a, std::size_t b) { return nests[a].spec.nestId < nests[b].spec.nestId; });
            for (const std::size_t n : list) nests[n].capHive = int(capHives.size());
            capHives.push_back({max, list, std::nullopt});
        }
        if (!capHives.empty()) {
            std::size_t held = 0;
            int cap = 0;
            for (const CapHive& h : capHives) held += h.nests.size(), cap += h.max;
            core::LogInfo("monsters: {} capped hives over {} nests, {} monsters at most together", capHives.size(), held, cap);
        }
    }

    // Whether Harmony keeps the monster from starting a fight with the player: its grade's bit in the
    // protection's (grades 2, 4, 5 never: the original's table) and its level not above it.
    static bool Protected(const Monster& m, const Player& p) {
        if (p.protectedGrades == 0) return false;
        std::uint8_t bit = 0;
        switch (m.rarity & 0x0F) {
        case 0: bit = 1; break;
        case 1: bit = 2; break;
        case 3: bit = 4; break;
        case 6: bit = 8; break;
        default: return false;
        }
        return (p.protectedGrades & bit) != 0 && m.stats.level <= p.protectedLevel;
    }

    void Forget(Monster& m) {
        m.foe = {};
        m.rival = {};
    }
    // A record of an opponent's hit or threat: the current record's when it is the current
    // target (or none), else the second's when free or its own; a third opponent is not recorded. The
    // threat grows by its percent of itself and the message's, held to 0..2^31-1.
    bool Record(Monster& m, std::uint32_t source, std::uint64_t damage, std::int64_t threat, int percent, Clock::time_point now) {
        Monster::Opponent* r = nullptr;
        if (m.foe.entity == 0 || m.foe.entity == source) {
            r = &m.foe;
        } else if (m.rival.entity == 0 || m.rival.entity == source) {
            r = &m.rival;
        }
        if (r == nullptr) return false;
        r->entity = source;
        r->damage += damage;
        const std::int64_t grown = r->threat + std::int64_t(std::trunc(double(r->threat) * double(percent) / 100.0)) + threat;
        r->threat = std::clamp<std::int64_t>(grown, 0, 0x7FFFFFFF);
        r->tick = now;
        return true;
    }
    bool Valid(std::uint32_t entity) const {
        const auto it = players.find(entity);
        return it != players.end() && it->second->alive && !it->second->hidden;
    }
    // Attacked, or threatened (an attack message or a threat-only one): idle, it engages with the
    // threat x 1.15 when that comes above 0; in battle it records and turns as its tactics
    // say (btChangeTarget 2, the most rows:; ours: every monster's), its target's
    // threat at 0 sending it idle.
    void Attacked(Monster& m, std::uint32_t source, std::uint64_t damage, std::int64_t threat, int percent, Clock::time_point now) {
        if (source == 0) return;
        if (m.mode != Mode::Chase || m.target == 0) {
            Record(m, source, damage, std::int64_t(std::trunc(double(threat) * 1.15)), percent, now);
            if (m.foe.threat > 0) Engage(m, m.foe.entity, now);
            return;
        }
        const bool recorded = Record(m, source, damage, threat, percent, now);
        if (m.foe.threat <= 0) return DropFight(m, now);
        // A bandit's tactics row: 0 keeps its target, 1 turns to the
        // one that hit it at once; 2 as every other monster.
        if (const int policy = nests[m.nest].spec.changeTarget; policy == 0 && Valid(m.target)) {
            return;
        } else if (policy == 1 && source != m.target && Valid(source)) {
            if (m.foe.entity != source) std::swap(m.foe, m.rival);
            if (m.foe.entity != source) m.foe = {source, 0, 0, now};
            m.target = source;
            m.aim = {};
            return;
        }
        if (recorded && m.rival.entity == source) {
            if (!Valid(m.foe.entity)) {
                m.foe = m.rival;
                m.rival = {};
            } else if (m.rival.threat > m.foe.threat || now - m.foe.tick >= std::chrono::milliseconds(2 * m.waitMs)) {
                std::swap(m.foe, m.rival);
            } else {
                return;
            }
        } else if (!recorded) {
            // A third opponent: it takes the current place (no threat) when the current target is gone or
            // farther than it.
            const auto place = world.Where(m.entity);
            const auto them = players.find(source), now_ = players.find(m.foe.entity);
            const bool closer = place && them != players.end() && now_ != players.end() &&
                                Distance(them->second->position, place->position) < Distance(now_->second->position, place->position);
            if (Valid(m.foe.entity) && !closer) return;
            m.rival = m.foe;
            m.foe = {source, 0, 0, now};
        } else {
            return;
        }
        m.target = m.foe.entity;
        m.aim = {};
    }

    // A fortress war's guard or tablet monster goes for a player fighting in the war against its side (not of its guild
    // nor of its union).
    static bool WarFoe(const MonsterNest& spec, const Player& p) {
        if (!p.atWar || p.guild == spec.warGuild) return false;
        return spec.warAlliance == 0 || p.alliance != spec.warAlliance;
    }
    // An idle monster's look around, on a timer of its own, standing or wandering (1,010 to 1,500 ms; 200 ms only for
    // the tactics flags 4 and 0x80, which no aggressive row has). An aggressive
    // one takes the nearest player in sight.
    void LookAround(Monster& m, Clock::time_point now) {
        const Nest& nest = nests[m.nest];
        const auto place = world.Where(m.entity);
        if (!place) return;
        if (nest.spec.bandit != 0) return BanditLook(m, *place, now);
        // Aggressive by their nest, or by their kind: champions, giants and elites look for a fight.
        // A champion or giant of the roll by its tactics' champion row (every one aggressive, its own sight).
        const int grade = m.rarity & 0x0F;
        const bool aggressive = nest.spec.aggressive || m.championRow || grade == 1 || grade == 4 || grade == 6;
        if (aggressive && !nest.attacks.empty()) {
            const float own = m.championRow && nest.spec.championSight > 0 ? nest.spec.championSight : nest.spec.sight;
            const float sight = own > 0 ? own : 100.0f;
            const Player* best = nullptr;
            float bestDistance = sight;
            const int rx = place->position.region & 0xFF, rz = place->position.region >> 8;
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (rx + dx < 0 || rx + dx > 255 || rz + dz < 0 || rz + dz > 255) continue;
                    const auto it = playersByRegion.find(std::uint16_t(((rz + dz) << 8) | (rx + dx)));
                    if (it == playersByRegion.end()) continue;
                    for (const Player* p : it->second) {
                        const float d = Distance(p->position, place->position);
                        if (nest.spec.warGuild != 0 && !WarFoe(nest.spec, *p)) continue; // a guard: only the side's enemies in the war
                        if (nest.spec.warGuild == 0 && InTown(p->position)) continue; // town ground is safe (the user's report 2026-11-14)
                        if (const auto c = chasers.find(p->entity); c != chasers.end() && c->second >= SquadLimit(p->position)) continue; // its squad full
                        if (p->alive && !p->hidden && !p->banditsOnly && d < bestDistance && !m.states.Idles(p->entity) && !Protected(m, *p)) {
                            best = p;
                            bestDistance = d;
                        }
                    }
                }
            }
            if (best != nullptr) {
                Engage(m, best->entity, now);
                ++chasers[best->entity];
            }
        }
    }

    // A bandit's look about (selectors 3 and 4): the nearest in its sight of those its kind hunts
    // (a thief monster: the traders and hunters and their COSes; a hunter monster: the thieves and theirs), else a thief
    // monster's nearest player without a job; a COS its owner rides gives way to the owner.
    void BanditLook(Monster& m, const World::Place& place, Clock::time_point now) {
        const Nest& nest = nests[m.nest];
        if (nest.attacks.empty()) return;
        const auto kind = game::BanditKind(nest.spec.bandit);
        const Player* hunted = nullptr;
        const Player* fallback = nullptr;
        float hd = nest.spec.sight > 0 ? nest.spec.sight : game::kBanditSight, fd = hd;
        const int rx = place.position.region & 0xFF, rz = place.position.region >> 8;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (rx + dx < 0 || rx + dx > 255 || rz + dz < 0 || rz + dz > 255) continue;
                const auto it = playersByRegion.find(std::uint16_t(((rz + dz) << 8) | (rx + dx)));
                if (it == playersByRegion.end()) continue;
                for (const Player* p : it->second) {
                    if (!p->alive || p->hidden || m.states.Idles(p->entity)) continue;
                    if (InTown(p->position)) continue; // a town is safe ground (inferred)
                    const float d = Distance(p->position, place.position);
                    if (game::BanditHunts(kind, p->job) && d < hd) {
                        hunted = p;
                        hd = d;
                    } else if (!p->banditsOnly && p->partner == 0 && game::BanditFallback(kind, p->job) && d < fd) {
                        fallback = p;
                        fd = d;
                    }
                }
            }
        }
        const Player* pick = hunted != nullptr ? hunted : fallback;
        if (pick == nullptr) return;
        Engage(m, pick->entity, now); // a rider's transport (Engage)
    }
    // In a fight a bandit goes between a player and their transport: when the other is at least 15 nearer, it becomes
    // the target.
    void BanditSwitch(Monster& m, const World::Place& place) {
        const Nest& nest = nests[m.nest];
        const auto who = players.find(m.target);
        if (nest.spec.bandit == 0 || who == players.end() || who->second->partner == 0) return;
        const auto other = players.find(who->second->partner);
        if (other == players.end() || !other->second->alive || other->second->hidden) return;
        if (Struck(other->first) != other->first) return; // its owner on it: never struck
        if (!game::BanditHunts(game::BanditKind(nest.spec.bandit), other->second->job)) return;
        if (Distance(other->second->position, place.position) + game::kBanditSwitchCloser > Distance(who->second->position, place.position)) return;
        if (m.foe.entity != other->first) std::swap(m.foe, m.rival);
        if (m.foe.entity != other->first) m.foe = {other->first, 0, 0, Clock::now()};
        m.target = other->first;
        m.aim = {};
    }

    // One that never moves: its walk and run speeds both 0 (_RefObjCommon Speed1, Speed2; characterdata cols 46, 47: the
    // plants, the desert trunks, the traps, the seal stones, 79 in service). It stands where it was put, fights what
    // comes within its reach and follows nobody (the user's report 2026-11-13: the Blood Devils wandered).
    static bool Static(const Nest& nest) { return nest.row != nullptr && nest.row->walkSpeed <= 0.0f && nest.row->runSpeed <= 0.0f; }

    // An idle monster's next move: it stays a while, or walks a little within its nest's circle.
    void Idle(Monster& m, Clock::time_point now) {
        const Nest& nest = nests[m.nest];
        const auto place = world.Where(m.entity);
        if (!place) return;
        if (Static(nest)) {
            m.nextThink = now + Seconds(Uniform(2.0f, 6.0f));
            return;
        }
        if (world.Moving(m.entity)) {
            m.nextThink = now + Seconds(0.5f);
            return;
        }
        // Outside its home boundary (its nest's radius; 1.5 times it for homing kind 1; kind 2 none), it goes home (the
        // original's on entering idle and at each wander; ours: not for a point without its nest's record, radius 0).
        if (nest.spec.homing != 2 && nest.spec.radius > 0.0f && !m.states.Rooted()) {
            const float boundary = nest.spec.radius * (nest.spec.homing == 1 ? 1.5f : 1.0f);
            if (Distance(place->position, nest.spec.position) >= boundary) {
                Events none;
                return GoHome(m, now, none);
            }
        }
        m.nextThink = now + Seconds(Uniform(2.0f, 6.0f));
        if (Percent() < 25 || m.states.Rooted()) return;
        const proto::FlatPosition here = proto::ToFlat(place->position), centre = proto::ToFlat(nest.spec.position);
        const float a = Uniform(0.0f, 2.0f * std::numbers::pi_v<float>), d = Uniform(40.0f, 120.0f);
        proto::FlatPosition to{here.x + d * std::sin(a), here.z + d * std::cos(a)};
        const float keep = std::max(nest.spec.spawnRadius, 60.0f);
        if (std::hypot(to.x - centre.x, to.z - centre.z) > keep) {
            // Out of its circle: back toward the point, somewhere in the inner half.
            const float r = keep * 0.5f * std::sqrt(Uniform(0.0f, 1.0f)), b = Uniform(0.0f, 2.0f * std::numbers::pi_v<float>);
            to = {centre.x + r * std::sin(b), centre.z + r * std::cos(b)};
        }
        world.MoveTo(m.entity, proto::FromFlat(to, place->position.y), true);
        const float walk = nest.row->walkSpeed > 0 ? nest.row->walkSpeed : 16.0f;
        m.nextThink += Seconds(float(std::hypot(to.x - here.x, to.z - here.z)) / walk);
    }

    // What a fight with `entity` strikes: the transport it rides, else itself.
    std::uint32_t Struck(std::uint32_t entity) const {
        const auto it = players.find(entity);
        return it != players.end() && it->second->mount != 0 && Valid(it->second->mount) ? it->second->mount : entity;
    }
    void Engage(Monster& m, std::uint32_t target, Clock::time_point now) {
        if (nests[m.nest].attacks.empty()) return; // nothing to fight with
        target = Struck(target);
        if (m.states.In(game::StateKind::Confusion)) return; // its tactics keep it out of fights
        if (m.foe.entity != target) {
            m.rival = m.foe;
            m.foe = {target, 0, 0, now};
        }
        m.mode = Mode::Chase;
        m.target = target;
        m.aim = {};
        m.nextThink = now;
        m.lastStrike = now; // (ours: a fresh chase runs; the 5 s count from its start)
        if (m.nextSwing < now) m.nextSwing = now + Seconds(Uniform(0.2f, 0.6f));
    }

    // A summon skill's monsters raised about `m` (added at the next update): each on a ring as far as its body and theirs
    // and 30 more, at a random angle (ours: each its own angle, the original's one a group), or at its own spot (a god's
    // loot body: the INS_DEFAULT row named after it, linked to the god).
    void RaiseSummons(const Monster& m, const Nest& nest, const Nest::SummonSkill& skill) {
        const auto place = world.Where(m.entity);
        if (!place) return;
        for (const Nest::Summon& group : skill.groups) {
            const assets::ObjectRow* row = characters.Find(group.refObjId);
            const float ring = float(m.stats.bodyRadius) + (row != nullptr ? row->bodyRadius : 0.0f) + 30.0f;
            const int count = std::uniform_int_distribution<int>(group.fewest, group.most)(random);
            const auto spot = row != nullptr ? summonPlaces.find(row->codename) : summonPlaces.end();
            const bool lootBody = row != nullptr && row->codename == nest.row->codename + "_DROP";
            for (int k = 0; k < count; ++k) {
                const float angle = Uniform(0.0f, 2.0f * std::numbers::pi_v<float>);
                const proto::FlatPosition c = proto::ToFlat(place->position);
                proto::WorldPosition at = spot != summonPlaces.end()
                                              ? spot->second
                                              : proto::FromFlat({c.x + double(std::cos(angle) * ring), c.z + double(std::sin(angle) * ring)}, place->position.y);
                at.layer = place->position.layer;
                MonsterNest raised;
                raised.refObjId = group.refObjId;
                raised.rarity = group.rarity;
                raised.position = at;
                raised.spawnRadius = 0.0f;
                raised.radius = 300.0f;
                raised.maxCount = 1;
                raised.aggressive = true;
                raised.sight = 300.0f;
                raised.respawn = false;
                raised.temporary = true;
                if (lootBody) raised.linkTo = m.entity;
                summons.push_back(raised);
            }
        }
    }
    // The outdoor chase's way round an obstacle (the original's probe, at each step of a chase: MonsterWayRound); none
    // free (or in a dungeon, whose chase goes by the original's own route data: not read), the straight step all the
    // same.
    std::optional<proto::WorldPosition> WayRound(const proto::WorldPosition& from, const proto::WorldPosition& to) const {
        if ((from.region & 0x8000) != 0 || !world.HasNavigation()) return std::nullopt;
        return MonsterWayRound(from, to, [&](const proto::WorldPosition& a, const proto::WorldPosition& b) { return world.WalkEnd(a, b); });
    }
    // Its attack for this swing: drawn by weight, each its AI_AttackChance (0: never) and half of how far the target stands
    // inside its reach ((range + its body + the target's - distance) x 5 / 10, when positive); r = rand % (sum + 1) on the
    // running sums. None weighed (ours): one at random.
    const assets::SkillRow* PickAttack(const Nest& nest, const Monster& m, float distance) {
        std::vector<int> weights;
        int sum = 0;
        for (const assets::SkillRow* s : nest.attacks) {
            int w = 0;
            if (s->aiChance > 0) {
                const float inside = float(std::max(s->range, 10)) + m.stats.bodyRadius + kPlayerRadius - distance;
                w = s->aiChance + (inside > 0.0f ? int(inside) * 5 / 10 : 0);
            }
            weights.push_back(w);
            sum += w;
        }
        if (sum <= 0) return nest.attacks[std::size_t(std::uniform_int_distribution<int>(0, int(nest.attacks.size()) - 1)(random))];
        const int r = std::uniform_int_distribution<int>(0, sum)(random);
        int running = 0;
        for (std::size_t i = 0; i < weights.size(); ++i) {
            running += weights[i];
            if (weights[i] > 0 && r <= running) return nest.attacks[i];
        }
        return nest.attacks.back();
    }

    void GoHome(Monster& m, Clock::time_point now, Events& out) {
        m.mode = Mode::Home;
        m.target = 0;
        Forget(m);
        m.pending.clear(), m.swingEnds = {};
        m.damageBy.clear();
        (void)out; // its health comes back by its natural recovery, not at once (the original's AI never sets it)
        if (!Static(nests[m.nest])) world.MoveTo(m.entity, nests[m.nest].spec.position);
        m.nextThink = now + Seconds(0.5f);
    }

    // A fighting monster: run into reach, swing when its time has come; give up when led too far.
    void Fight(Monster& m, Clock::time_point now, Events& out) {
        m.nextThink = now + std::chrono::milliseconds(200);
        const auto place = world.Where(m.entity);
        // Its target boarded a transport since: the transport is fought instead.
        if (const std::uint32_t struck = Struck(m.target); struck != m.target) {
            if (m.foe.entity == m.target) m.foe.entity = struck;
            m.target = struck;
            m.aim = {};
        }
        const auto who = players.find(m.target);
        if (!place) return GoHome(m, now, out);
        // Its target dead, gone or hidden: it idles where it is and forgets both records (no
        // fall back to the second one).
        if (who == players.end() || !who->second->alive || who->second->hidden) return DropFight(m, now);
        const Nest& nest = nests[m.nest];
        if (nest.spec.bandit != 0) {
            // Its target gone into a town: it lets it be (inferred: the towns are safe ground; it never follows in).
            if (InTown(who->second->position)) return DropFight(m, now);
            const std::uint32_t before = m.target;
            BanditSwitch(m, *place);
            if (m.target != before) return; // at the other next time
        } else if (nest.spec.warGuild == 0 && InTown(who->second->position)) {
            // Its target on town ground: it goes back to its nest, never into the town after it (the user's report
            // 2026-11-14; one that steps in all the same is taken away below).
            return GoHome(m, now, out);
        }
        const proto::WorldPosition& there = who->second->position;
        const float distance = Distance(place->position, there);
        // The trace: its tactics' TraceData less however far it stands beyond 1.5 times its home radius; its target farther
        // and it drops the fight where it stands (idle; it walks back from outside its circle and recovers there). A still
        // one's trace is its reach; one whose boundary does not hold it (btTraceBoundary 0, 2) never gives up by distance.
        // A bandit: led beyond 500 or its leash it goes home (as before; its own branch).
        if (nest.spec.bandit != 0) {
            const float leash = std::max(nest.spec.radius * 1.5f, 300.0f);
            if (distance > kGiveUpDistance || Distance(place->position, nest.spec.position) > leash) return GoHome(m, now, out);
        } else if (nest.spec.traceBoundary == 1) {
            const float beyond = std::max(0.0f, Distance(place->position, nest.spec.position) - nest.spec.radius * 1.5f);
            const float stillReach = (Static(nest) || nest.spec.trace <= 0.0f) && !nest.attacks.empty()
                                         ? float(std::max(nest.attacks.front()->range, 10)) + m.stats.bodyRadius + kPlayerRadius
                                         : 0.0f;
            const float trace = (nest.spec.trace > 0.0f ? nest.spec.trace : stillReach) - beyond;
            if (distance > trace) return DropFight(m, now);
        }
        // Feared by its target, or confused: it drops the fight where it stands.
        if (m.states.Idles(m.target)) return DropFight(m, now);
        // The skill it uses next: drawn by its attacks' weights (PickAttack); myopia shortens its reach.
        const assets::SkillRow* skill = PickAttack(nest, m, distance);
        const float reach = (float(std::max(skill->range, 10)) + m.stats.bodyRadius + kPlayerRadius) * float(m.states.ReachPercent()) / 100.0f;
        // A swing begun is played out where it stands (as a player's, K10); then the chase goes on
        // (the user's report 2026-11-14: chasers broke off their swings to run, a stutter).
        if (now < m.swingEnds) return;
        // In reach, its next swing not due yet: it keeps up with a target on the move, so that swing goes out the moment
        // it is due (ours; the user's report 2026-11-14: a chaser stood in reach waiting, the target ran off, it ran
        // again, and never landed a hit); a target standing still, it stands too.
        const bool keepUp = distance <= reach && now < m.nextSwing && world.Moving(m.target);
        if (distance > reach || keepUp) {
            if (m.states.Rooted() || Static(nest)) return; // it cannot go after it
            // After it: toward where the target stands now, never where it is going (the original's pursuit,
            // every 100 ms, the target's present place, a walk to a reach
            // short of it; the user's clip 2026-11-14: aimed at the target's leg's end, thieves between a player and
            // his click ran on to the click and never fought). Turned again when the way is more than 15 degrees off
            // (0.2618 rad), or when the target moved 30 from the aim, so a chaser never stops at a stale spot.
            // Its place about the target (the original's eight): it heads for that point, 0.8 of its reach out from the
            // target on its side, so a crowd stands round its target instead of in one heap (the user's report
            // 2026-11-22); with all eight held, its preferred place all the same; the target far apart (another plane, a
            // region not next to its own), the target itself.
            const int spot = ApproachPlace(m, place->position, there);
            const proto::WorldPosition goal = spot >= 0 ? ApproachPoint(there, spot, reach * 0.8f) : there;
            m.nextThink = now + std::chrono::milliseconds(100);
            bool turn = !world.Moving(m.entity) || Distance(m.aim, goal) > 30.0f;
            if (!turn) {
                const proto::FlatPosition a = proto::ToFlat(place->position), was = proto::ToFlat(m.aim), is = proto::ToFlat(goal);
                const float ux = was.x - a.x, uz = was.z - a.z, vx = is.x - a.x, vz = is.z - a.z;
                const float lu = std::hypot(ux, uz), lv = std::hypot(vx, vz);
                turn = lu > 1.0f && lv > 1.0f && (ux * vx + uz * vz) / (lu * lv) < std::cos(0.2617994f);
            }
            if (turn) {
                const proto::FlatPosition a = proto::ToFlat(place->position), b = proto::ToFlat(goal);
                const double length = std::max(1.0, std::hypot(a.x - b.x, a.z - b.z));
                // Not struck for 5 s: it walks while its target is over 100 away (moving) or 150 (standing), runs nearer.
                const bool walk = now - m.lastStrike > std::chrono::seconds(5) && distance > (world.Moving(m.target) ? 100.0f : 150.0f);
                proto::WorldPosition step;
                if (length > kChaseLeg) { // a far end: a leg's worth toward it (one walk's reach, never refused)
                    step = proto::FromFlat({a.x + (b.x - a.x) / length * kChaseLeg, a.z + (b.z - a.z) / length * kChaseLeg}, goal.y);
                } else {
                    const double stop = spot >= 0 ? 0.0 : std::min(double(reach) * 0.7, length); // onto its place's point
                    step = proto::FromFlat({b.x + (a.x - b.x) / length * stop, b.z + (a.z - b.z) / length * stop}, goal.y);
                }
                step.layer = place->position.layer;
                // An obstacle in the way outdoors: 20 the first free way round (WayRound), the next step looking again.
                if (const auto round = WayRound(place->position, step)) step = *round;
                world.MoveTo(m.entity, step, walk);
                m.aim = goal;
            }
            return;
        }
        if (world.Moving(m.entity)) world.MoveTo(m.entity, place->position); // stop where it stands
        if (now < m.nextSwing) return;
        // A conditional skill ready (Tab_RefAISkill) goes first: a summon's monsters raised, an attack swung in this skill's
        // place (ours: another kind not yet).
        if (const int c = ReadyConditional(m, nest, now); c >= 0) {
            const Nest::Conditional& cond = nest.conditionals[std::size_t(c)];
            if (!cond.summon.groups.empty()) {
                RaiseSummons(m, nest, cond.summon);
                out.swings.push_back({m.entity, m.target, cond.skill->id});
                m.lastStrike = now;
                m.nextSwing = now + std::chrono::milliseconds(std::max(1000, cond.skill->durationMs));
                return;
            }
            if (cond.skill->attack) skill = cond.skill;
        }
        out.swings.push_back({m.entity, m.target, skill->id});
        m.lastStrike = now;
        const game::AttackSpec attack = game::AttackOf(*skill);
        const int duration = std::max(500, skill->durationMs);
        m.swingEnds = now + std::chrono::milliseconds(duration);
        for (int h = 0; h < attack.hits; ++h) {
            const float at = 0.3f + 0.4f * float(h) / float(std::max(1, attack.hits)); // ours: the clips' hit events are the client's
            // It lands on its target wherever it ran meanwhile (a player's swing does the same, K10), only not on one gone beyond the chase's limit (a teleport, a gate).
            m.pending.push_back({now + std::chrono::milliseconds(int(float(duration) * at)), m.target, attack, kGiveUpDistance, skill});
        }
        // Frostbite and slow stretch the time between its swings.
        const int wait = std::max(duration, skill->cooldownMs) * m.states.AttackTimePercent() / 100;
        m.waitMs = wait + std::uniform_int_distribution<int>(0, 500)(random);
        m.nextSwing = now + std::chrono::milliseconds(m.waitMs);
    }

    // A lab monster fleeing (proto::LabBehaviour::FleeWalk, FleeRun): straight away from its player, a leg at a time,
    // turned again when that player comes round more than 15 degrees; it stops when the player is gone.
    void Flee(Monster& m, Clock::time_point now) {
        m.nextThink = now + std::chrono::milliseconds(200);
        const Nest& nest = nests[m.nest];
        const auto place = world.Where(m.entity);
        const auto from = players.find(nest.spec.labFrom);
        if (!place || from == players.end() || m.states.Rooted() || Static(nest)) return Stop(m);
        const proto::FlatPosition a = proto::ToFlat(place->position), p = proto::ToFlat(from->second->position);
        float dx = a.x - p.x, dz = a.z - p.z;
        const float d = std::hypot(dx, dz);
        if (d < 0.5f) {
            dx = 1.0f, dz = 0.0f;
        } else {
            dx /= d, dz /= d;
        }
        constexpr float kLeg = 120.0f;
        bool turn = !world.Moving(m.entity);
        if (!turn) {
            const proto::FlatPosition was = proto::ToFlat(m.aim);
            const float ux = was.x - a.x, uz = was.z - a.z, lu = std::hypot(ux, uz);
            turn = lu < 20.0f || (ux * dx + uz * dz) / lu < std::cos(0.2617994f);
        }
        if (!turn) return;
        m.aim = proto::FromFlat({a.x + dx * kLeg, a.z + dz * kLeg}, place->position.y);
        world.MoveTo(m.entity, m.aim, nest.spec.lab == proto::LabBehaviour::FleeWalk);
    }

    // --- States (K3) -------------------------------------------------------

    void Stop(Monster& m) {
        if (!world.Moving(m.entity)) return;
        if (const auto place = world.Where(m.entity)) world.MoveTo(m.entity, place->position);
    }
    // Its numbers and its speed after its states changed.
    void Refresh(Monster& m) {
        // Its buffs' changes and its states' in one sheet (the keeper sums them).
        game::StatSheet sheet;
        for (const Monster::Debuff& d : m.debuffs) sheet.Add(d.skill != nullptr ? d.skill->id : 0u, d.changes);
        m.states.AddChanges(sheet, m.stats);
        m.current = m.stats;
        game::ApplySheet(m.current, sheet);
        const int speed = m.states.SpeedPercent();
        if (speed != m.speedPercent && !m.knocked) world.SetSpeedFactor(m.entity, float(speed) / 100.0f);
        m.speedPercent = speed;
    }
    // Feared or confused: it stops fighting where it stands and idles (its AI's IDLE state).
    void DropFight(Monster& m, Clock::time_point now) {
        m.mode = Mode::Idle;
        m.target = 0;
        Forget(m);
        m.pending.clear(), m.swingEnds = {};
        Stop(m);
        m.nextThink = now + Seconds(Uniform(2.0f, 6.0f));
    }
    // A state it just took: what it does at once (the callbacks' start).
    void Took(Monster& m, const game::ActiveState& state, Clock::time_point now) {
        switch (state.kind) {
        case game::StateKind::Freeze:
        case game::StateKind::Sleep:
        case game::StateKind::Stun:
        case game::StateKind::Stone:
            m.pending.clear(), m.swingEnds = {}; // its swing stops
            Stop(m);
            break;
        case game::StateKind::Root: Stop(m); break;
        case game::StateKind::Fear:
            if (m.mode == Mode::Chase && m.target == state.source) DropFight(m, now);
            break;
        case game::StateKind::Confusion:
            if (m.mode == Mode::Chase) DropFight(m, now);
            break;
        default: break;
        }
    }
    // Its health less `damage` from `attacker`; a kill ends it (its corpse lies, its nest refills).
    Damaged Hurt(Monster& m, std::uint32_t attacker, std::uint32_t damage, Clock::time_point now) {
        Damaged result;
        const Nest& nest = nests[m.nest];
        result.hit = true;
        result.refObjId = nest.spec.refObjId;
        result.rarity = m.rarity;
        result.nest = std::int64_t(m.nest);
        // Its summon skills due (Nest::summons): the summoned each raised on a ring about it at a random angle, as far as
        // its body and theirs and 30 more (ours: each its own angle, the original's one a group),
        // added at the next update.
        const std::uint32_t after = damage >= m.hp ? 0 : m.hp - damage;
        const bool onlySummons = nest.attacks.empty();
        for (std::size_t i = 0; i < nest.summons.size(); ++i) {
            if ((m.summoned & (1u << i)) != 0) continue;
            const Nest::SummonSkill& skill = nest.summons[i];
            if (!onlySummons && std::uint64_t(after) * 100 > std::uint64_t(m.stats.maxHp) * std::uint64_t(skill.band)) continue;
            m.summoned |= 1u << i;
            RaiseSummons(m, nest, skill);
        }
        const std::uint64_t taken = std::min<std::uint64_t>(damage, m.hp);
        auto by = std::find_if(m.damageBy.begin(), m.damageBy.end(), [&](const auto& d) { return d.first == attacker; });
        if (by == m.damageBy.end()) by = m.damageBy.insert(m.damageBy.end(), {attacker, 0});
        by->second += taken;
        m.hp = damage >= m.hp ? 0 : m.hp - damage;
        result.hp = m.hp;
        world.SetHp(m.entity, m.hp);
        if (m.hp != 0) return result;
        result.killed = true;
        // Killed by someone: one of its death skills drawn by the running sum of their chances against a roll of 0..100
        // (a chance of 0 at once), cast on the killer: a summon's monsters come now (ours: another kind's effect not yet).
        if (attacker != 0 && !nest.deaths.empty()) {
            const double roll = double(std::uniform_int_distribution<int>(0, 32767)(random)) / 32767.0 * 100.0;
            double running = 0.0;
            for (const Nest::DeathSkill& d : nest.deaths) {
                running += double(d.chance);
                if (d.chance != 0 && roll >= running) continue;
                if (!d.summon.groups.empty()) RaiseSummons(m, nest, d.summon);
                break;
            }
        }
        result.damageBy = std::move(m.damageBy);
        m.damageBy.clear();
        m.mode = Mode::Dead;
        m.pending.clear(), m.swingEnds = {};
        m.goneAt = nests[m.nest].spec.fixed ? Clock::time_point::max() : now + kCorpse; // a structure stays, as rubble
        if (m.states.Mask() != 0) {
            m.states.Clear();
            result.statesChanged = true;
        }
        Stop(m);
        result.killer = attacker;
        // Its nest refills the gap after its delay (a unique's hive: at a nest it picks).
        Nest& home = nests[m.nest];
        home.alive.erase(std::remove(home.alive.begin(), home.alive.end(), m.entity), home.alive.end());
        if (home.hive >= 0) {
            HiveDeath(home, now);
            result.unique = true;
        } else if (home.capHive >= 0) {
            CappedDeath(home, now);
        } else if (int(home.alive.size()) + 1 >= home.spec.maxCount) {
            RollRefill(home, now);
        }
        return result;
    }
    // A knock: pushed away from the attacker (along the floor; a wall stops it where it is), doing
    // nothing for its time; down, it lies.
    void Knock(Monster& m, const Affliction& a, Clock::time_point now, Damaged& result) {
        const auto place = world.Where(m.entity);
        if (!place) return;
        const proto::FlatPosition from = proto::ToFlat(a.from), here = proto::ToFlat(place->position);
        double dx = here.x - from.x, dz = here.z - from.z;
        const double length = std::hypot(dx, dz);
        if (length < 0.001) {
            dx = 0.0;
            dz = 1.0;
        } else {
            dx /= length;
            dz /= length;
        }
        const float distance = a.knock == game::Knock::Down ? game::kKnockDownDistance : a.distance;
        proto::WorldPosition to = proto::FromFlat({here.x + dx * distance, here.z + dz * distance}, place->position.y);
        // A knock-back slides at 250 a second (P5); a knock-down's fall takes 0.3 s (ours: the original's endpoint is the
        // hit's; its client slides there).
        const float run = std::max(1.0f, nests[m.nest].row->runSpeed);
        const float speed = a.knock == game::Knock::Down ? distance / 0.3f : game::kKnockBackSpeed;
        world.SetSpeedFactor(m.entity, std::max(1.0f, speed / run));
        if (world.MoveTo(m.entity, to) != World::MoveRefused::None) {
            to = place->position;
            Stop(m);
        }
        m.pending.clear(), m.swingEnds = {};
        m.knocked = true;
        m.down = a.knock == game::Knock::Down;
        m.knockedUntil = now + std::chrono::milliseconds(a.knockMs);
        result.knock = a.knock;
        result.knockTo = to;
        result.knockMs = a.knockMs;
    }
    // Every update: the knock's end, the states' ticks (their damage) and ends.
    void UpdateStates(Monster& m, Clock::time_point now, Events& out, std::vector<Tick>* ticks) {
        if (m.knocked && now >= m.knockedUntil) {
            m.knocked = m.down = false;
            world.SetSpeedFactor(m.entity, float(m.speedPercent) / 100.0f);
            m.nextThink = now;
        }
        if (std::erase_if(m.debuffs, [&](const Monster::Debuff& d) { return d.until <= now; }) != 0) Refresh(m);
        if (m.states.Mask() == 0) return;
        for (const game::StateTick& t : m.states.Ticks(now, m.hp, 0, m.current.magAbsorption)) {
            Damaged d = Hurt(m, t.source, t.hp, now);
            const bool killed = d.killed;
            if (ticks != nullptr) ticks->push_back({m.entity, t, std::move(d)});
            if (killed) {
                out.statesChanged.push_back(m.entity);
                return;
            }
        }
        // A time bomb's burst at its natural end (a cure never sets it off): its
        // damage, fixed, from the bomber, and as much threat.
        const std::optional<game::ActiveState> bomb =
            m.states.In(game::StateKind::TimeBomb) ? std::optional<game::ActiveState>(*m.states.Find(game::StateKind::TimeBomb)) : std::nullopt;
        const std::vector<game::StateKind> ended = m.states.Expire(now);
        if (ended.empty()) return;
        Refresh(m);
        out.statesChanged.push_back(m.entity);
        if (bomb && std::find(ended.begin(), ended.end(), game::StateKind::TimeBomb) != ended.end() && bomb->value > 0) {
            const std::uint32_t damage = std::uint32_t(bomb->value);
            Damaged d = Hurt(m, bomb->source, damage, now);
            const bool killed = d.killed;
            if (ticks != nullptr) ticks->push_back({m.entity, {game::StateKind::TimeBomb, bomb->source, damage, 0, bomb->skill}, std::move(d)});
            if (!killed) Attacked(m, bomb->source, damage, std::int64_t(damage), 0, now);
        }
    }

    void LandHits(Monster& m, Clock::time_point now, Events& out) {
        for (auto it = m.pending.begin(); it != m.pending.end();) {
            if (now < it->at) {
                ++it;
                continue;
            }
            const auto who = players.find(it->target);
            const auto place = world.Where(m.entity);
            if (who != players.end() && who->second->alive && who->second->stats != nullptr && place &&
                Distance(place->position, who->second->position) <= it->reach) {
                out.hits.push_back({m.entity, it->target, game::RollHit(m.current, *who->second->stats, it->attack, {false, true, nullptr}, random),
                                    it->skill, nests[m.nest].row != nullptr ? nests[m.nest].row->level : 1});
                Rearm(m, now);
            }
            it = m.pending.erase(it);
        }
    }
};

std::optional<Monsters::Corpse> Monsters::CorpseOf(std::uint32_t entity) const {
    const auto it = s_->monsters.find(entity);
    if (it == s_->monsters.end() || it->second.mode != State::Mode::Dead) return std::nullopt;
    const State::Nest& nest = s_->nests[it->second.nest];
    return Corpse{nest.spec.refObjId, nest.row != nullptr ? nest.row->level : 0, it->second.rarity, nest.row != nullptr ? nest.row->typeId[3] : 0};
}

void Monsters::PartyKilled(std::uint32_t entity) {
    const auto it = s_->monsters.find(entity);
    if (it == s_->monsters.end() || it->second.nest >= s_->nests.size()) return;
    s_->nests[it->second.nest].partyNext = true;
}

Monsters::Monsters(World& world, const assets::CharacterTable& characters, const assets::SkillTable* skills, std::vector<MonsterNest> nests,
                   std::uint32_t seed, std::shared_ptr<const MonsterResistances> resistances)
    : s_(std::make_unique<State>(world, characters, skills, seed)) {
    s_->resistances = std::move(resistances);
    for (const MonsterNest& spec : nests) {
        State::Nest nest;
        if (!s_->Make(spec, nest)) continue;
        s_->byRegion[spec.position.region].push_back(s_->nests.size());
        s_->nests.push_back(std::move(nest));
    }
    s_->GroupHives();
    s_->GroupCrowds();
}

bool Monsters::Homeward(std::uint32_t entity) const {
    const auto it = s_->monsters.find(entity);
    return it != s_->monsters.end() && it->second.mode == State::Mode::Home;
}

int Monsters::CrowdRate(std::uint32_t hive) const {
    for (const State::Crowd& c : s_->crowds) {
        if (c.hive == hive) return c.rate;
    }
    return 0;
}

void Monsters::SetTowns(std::function<bool(std::uint16_t)> town) { s_->town = std::move(town); }
void Monsters::SetTownAreas(std::function<bool(const proto::WorldPosition&)> area) { s_->townArea = std::move(area); }

void Monsters::SetSummonPlaces(std::unordered_map<std::string, proto::WorldPosition> places) { s_->summonPlaces = std::move(places); }

void Monsters::SetConditionalSkills(std::unordered_map<std::uint32_t, std::vector<ConditionalSkill>> skills) {
    s_->conditionalSkills = std::move(skills);
    for (auto& nest : s_->nests) s_->FillConditionals(nest);
}

std::uint32_t Monsters::LinkedBody(std::uint32_t god) const {
    const auto it = s_->monsters.find(god);
    if (it == s_->monsters.end() || it->second.body == 0) return 0;
    const auto body = s_->monsters.find(it->second.body);
    return body != s_->monsters.end() && body->second.hp > 0 ? it->second.body : 0u;
}

std::uint32_t Monsters::RefOf(std::uint32_t entity) const {
    const auto it = s_->monsters.find(entity);
    return it != s_->monsters.end() ? s_->nests[it->second.nest].spec.refObjId : 0u;
}

std::size_t Monsters::AddNest(const MonsterNest& spec) {
    State& s = *s_;
    State::Nest nest;
    if (!s.Make(spec, nest)) return SIZE_MAX;
    std::size_t n = s.nests.size();
    if (!s.freeNests.empty()) {
        n = s.freeNests.back();
        s.freeNests.pop_back();
        s.nests[n] = std::move(nest);
    } else {
        s.nests.push_back(std::move(nest));
    }
    s.byRegion[spec.position.region].push_back(n);
    // Awake at once when its navigation is in (a fresh instance's first nests; the summon by its caller).
    if (!spec.waiting && !spec.fixed && s.world.NavigationLoaded(spec.position.region)) {
        s.nests[n].active = true;
        ++s.activeNests;
        s.Fill(n, Clock::now());
    }
    return n;
}

void Monsters::RemoveNest(std::size_t nest) {
    if (nest < s_->nests.size()) s_->Remove(nest);
}

void Monsters::ActivateNest(std::size_t nest) {
    State& s = *s_;
    if (nest >= s.nests.size() || s.nests[nest].removed || !s.nests[nest].spec.waiting) return;
    s.nests[nest].spec.waiting = false;
    if (s.world.NavigationLoaded(s.nests[nest].spec.position.region)) {
        s.nests[nest].active = true;
        ++s.activeNests;
        s.Fill(nest, Clock::now());
    }
}

bool Monsters::NestCleared(std::size_t nest) const {
    return nest < s_->nests.size() && !s_->nests[nest].removed && State::Cleared(s_->nests[nest]);
}

std::pair<int, int> Monsters::NestCounts(std::size_t nest) const {
    if (nest >= s_->nests.size() || s_->nests[nest].removed) return {0, 0};
    return {int(s_->nests[nest].alive.size()), s_->nests[nest].left};
}

std::vector<std::uint32_t> Monsters::NestMonsters(std::size_t nest) const {
    if (nest >= s_->nests.size() || s_->nests[nest].removed) return {};
    return s_->nests[nest].alive;
}

const MonsterNest* Monsters::NestSpec(std::size_t nest) const {
    return nest < s_->nests.size() && !s_->nests[nest].removed ? &s_->nests[nest].spec : nullptr;
}

Monsters::~Monsters() = default;

void Monsters::Update(Clock::time_point now, const std::vector<Player>& players, Events& out, std::vector<Tick>* ticks) {
    State& s = *s_;
    SR_ZONE("monsters");
    // Summons raised by last update's hits.
    for (std::vector<MonsterNest> raised = std::exchange(s.summons, {}); const MonsterNest& spec : raised) AddNest(spec);
    // The crowded hunting grounds' players, every 30 s.
    if (s.nextCrowd == Clock::time_point{}) s.nextCrowd = now + std::chrono::seconds(30);
    if (now >= s.nextCrowd) {
        s.nextCrowd = now + std::chrono::seconds(30);
        s.SampleCrowds(players, now);
    }
    if (now >= s.nextWake) {
        s.nextWake = now + std::chrono::seconds(1);
        s.Wake(now);
        s.HatchHives(now, out);
        s.HatchCapped(now);
        // Gaps refill one at a time, each after the nest's delay.
        for (std::size_t n = 0; n < s.nests.size(); ++n) {
            State::Nest& nest = s.nests[n];
            if (nest.hive >= 0) {
                if (nest.active && nest.held && nest.alive.empty()) s.Spawn(n, now);
                continue;
            }
            if (nest.capHive >= 0) continue; // its hive's (HatchCapped)
            if (nest.spec.fixed || nest.removed || !nest.spec.respawn || !nest.active || int(nest.alive.size()) >= nest.spec.maxCount ||
                now < nest.nextRefill) {
                continue;
            }
            s.Spawn(n, now);
            s.RollRefill(nest, now);
        }
    }
    if (now < s.nextThink) return;
    s.nextThink = now + std::chrono::milliseconds(50);
    // Its own copy of the players: what it keeps of them (by entity, by region) is read again between updates, by a hit
    // (Damage -> Attacked: the attacker's place) and by AddThreat; the caller's list is gone by then (a use after free
    // found by 1,500 bots entering at once, 2026-11-15).
    s.playerList = players;
    s.players.clear();
    s.playersByRegion.clear();
    for (const Player& p : s.playerList) {
        s.players[p.entity] = &p;
        s.playersByRegion[p.position.region].push_back(&p);
    }
    s.chasers.clear();
    for (const auto& [entity, m] : s.monsters) {
        if (m.mode == State::Mode::Chase && m.target != 0) ++s.chasers[m.target];
    }
    std::vector<std::size_t> buried; // nests whose bodies went this update
    for (auto it = s.monsters.begin(); it != s.monsters.end();) {
        State::Monster& m = it->second;
        if (m.mode == State::Mode::Dead) {
            if (now >= m.goneAt) {
                s.world.Leave(m.entity);
                const std::size_t nest = m.nest;
                it = s.monsters.erase(it);
                buried.push_back(nest);
                continue;
            }
            ++it;
            continue;
        }
        if (s.nests[m.nest].spec.fixed) { // a structure does nothing
            ++it;
            continue;
        }
        // A step into a town is any monster's end: the original's monster, on its region changing, is taken away where
        // the region is no battlefield (`_RefRegion.IsBattleField` 0), not a bandit's only. One put in a town by its nest
        // stays (ours: else it would be taken and made again for ever). A bandit's end also: 20 s without a hit.
        {
            const auto place = s.world.Where(m.entity);
            const bool bandit = s.nests[m.nest].spec.leaveMs > 0;
            const bool intoTown = place && s.InTown(place->position) && (bandit || !s.InTown(s.nests[m.nest].spec.position));
            if (intoTown || (bandit && now >= m.leaveAt)) {
                const std::size_t nest = m.nest;
                s.Vanish(m);
                it = s.monsters.erase(it);
                buried.push_back(nest);
                continue;
            }
        }
        s.UpdateStates(m, now, out, ticks);
        if (m.mode == State::Mode::Dead) {
            ++it;
            continue;
        }
        s.LandHits(m, now, out);
        // Its natural recovery: every 4 s, alive and standing (not walking or running), its row's MaxContain, no more than
        // it lacks, cut by its recovery cut (panic); not a structure's nor a lab monster's.
        if (now >= m.nextRecovery) {
            m.nextRecovery = now + kRecoveryTick;
            const auto& nest = s.nests[m.nest];
            if (m.hp > 0 && m.hp < m.stats.maxHp && nest.row != nullptr && nest.row->maxContain > 0 && !nest.spec.fixed &&
                nest.spec.lab == proto::LabBehaviour::Normal && !s.world.Moving(m.entity)) {
                const std::uint32_t gain = game::RecoveryCut(m.states, std::min(nest.row->maxContain, m.stats.maxHp - m.hp), true);
                if (gain > 0) {
                    m.hp += gain;
                    s.world.SetHp(m.entity, m.hp);
                    out.healed.push_back(m.entity);
                }
            }
        }
        // Knocked, frozen, asleep or stunned: it does nothing.
        if (m.knocked || m.states.Locked()) {
            ++it;
            continue;
        }
        // The skill lab's: a still one does nothing of its own, a fleeing one only flees.
        if (const proto::LabBehaviour lab = s.nests[m.nest].spec.lab; lab != proto::LabBehaviour::Normal) {
            if ((lab == proto::LabBehaviour::FleeWalk || lab == proto::LabBehaviour::FleeRun) && now >= m.nextThink) s.Flee(m, now);
            ++it;
            continue;
        }
        if (m.mode == State::Mode::Idle && now >= m.nextLook) {
            m.nextLook = now + s.Seconds(s.Uniform(1.01f, 1.5f));
            s.LookAround(m, now);
        }
        if (now >= m.nextThink) {
            switch (m.mode) {
            case State::Mode::Idle: s.Idle(m, now); break;
            case State::Mode::Chase: s.Fight(m, now, out); break;
            case State::Mode::Home:
                if (!s.world.Moving(m.entity)) {
                    m.mode = State::Mode::Idle;
                    m.nextThink = now + s.Seconds(s.Uniform(2.0f, 6.0f));
                } else {
                    m.nextThink = now + s.Seconds(0.5f);
                }
                break;
            case State::Mode::Dead: break;
            }
        }
        ++it;
    }
    for (const std::size_t n : buried) s.Buried(n);
}

std::uint32_t Monsters::AddFixed(std::uint32_t refObjId, const proto::WorldPosition& at, float yaw, std::uint32_t hp, std::uint32_t maxHp, float bodyRadius,
                                 bool destroyed) {
    State& s = *s_;
    const assets::ObjectRow* row = s.characters.Find(refObjId);
    if (row == nullptr) return 0;
    MonsterNest spec;
    spec.refObjId = refObjId;
    spec.position = at;
    spec.yaw = yaw;
    spec.fixed = true;
    State::Nest nest{spec, row};
    const std::size_t n = s.nests.size();
    nest.active = true;
    s.byRegion[at.region].push_back(n);
    s.nests.push_back(std::move(nest));
    s.Spawn(n, Clock::now());
    if (s.nests[n].alive.empty()) return 0;
    const std::uint32_t entity = s.nests[n].alive.back();
    State::Monster& m = s.monsters[entity];
    if (maxHp != 0) {
        m.stats.maxHp = maxHp;
        m.current.maxHp = maxHp;
    }
    if (bodyRadius > 0.0f) {
        m.stats.bodyRadius = bodyRadius;
        m.current.bodyRadius = bodyRadius;
    }
    m.hp = destroyed ? 0u : std::min(hp != 0 ? hp : m.stats.maxHp, m.stats.maxHp);
    s.world.SetHp(entity, m.hp);
    if (destroyed) {
        m.mode = State::Mode::Dead;
        m.goneAt = Clock::time_point::max();
        std::vector<std::uint32_t>& alive = s.nests[n].alive;
        alive.erase(std::remove(alive.begin(), alive.end(), entity), alive.end());
    }
    return entity;
}

std::uint32_t Monsters::AddWarMonster(std::uint32_t refObjId, const proto::WorldPosition& at, float yaw, std::uint32_t warGuild, std::uint32_t warAlliance,
                                      std::uint32_t hp) {
    State& s = *s_;
    MonsterNest spec;
    spec.refObjId = refObjId;
    spec.position = at;
    spec.yaw = yaw;
    spec.aggressive = true;
    spec.sight = 200.0f; // ours: the guards' sight is not traced
    spec.radius = 500.0f;
    spec.respawn = false;
    spec.warGuild = warGuild;
    spec.warAlliance = warAlliance;
    State::Nest nest;
    if (!s.Make(spec, nest)) return 0;
    const std::size_t n = s.nests.size();
    nest.active = true;
    nest.left = 1;
    s.byRegion[at.region].push_back(n);
    s.nests.push_back(std::move(nest));
    s.Spawn(n, Clock::now());
    if (s.nests[n].alive.empty()) return 0;
    const std::uint32_t entity = s.nests[n].alive.back();
    State::Monster& m = s.monsters[entity];
    if (hp != 0) m.hp = std::min(hp, m.stats.maxHp);
    s.world.SetHp(entity, m.hp);
    return entity;
}

void Monsters::RemoveFixed(std::uint32_t entity) {

    State& s = *s_;
    const auto it = s.monsters.find(entity);
    if (it == s.monsters.end()) return;
    State::Nest& nest = s.nests[it->second.nest];
    nest.alive.erase(std::remove(nest.alive.begin(), nest.alive.end(), entity), nest.alive.end());
    --nest.bodies;
    s.monsters.erase(it);
    s.world.Leave(entity);
}

std::vector<std::uint32_t> Monsters::Near(const proto::WorldPosition& at, float radius) const {
    std::vector<std::uint32_t> out;
    const proto::FlatPosition c = proto::ToFlat(at);
    const int rx = at.region & 0xFF, rz = at.region >> 8;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (rx + dx < 0 || rx + dx > 255 || rz + dz < 0 || rz + dz > 255) continue;
            const auto nests = s_->byRegion.find(std::uint16_t((rz + dz) << 8 | (rx + dx)));
            if (nests == s_->byRegion.end()) continue;
            for (const std::size_t n : nests->second) {
                for (const std::uint32_t entity : s_->nests[n].alive) {
                    const auto m = s_->monsters.find(entity);
                    if (m == s_->monsters.end() || m->second.mode == State::Mode::Dead) continue;
                    const auto place = s_->world.Where(entity);
                    if (!place) continue;
                    const proto::FlatPosition p = proto::ToFlat(place->position);
                    if (std::hypot(p.x - c.x, p.z - c.z) <= radius) out.push_back(entity);
                }
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

Monsters::Damaged Monsters::Damage(std::uint32_t monster, std::uint32_t attacker, std::uint32_t damage, Clock::time_point now,
                                   const Affliction* affliction) {
    State& s = *s_;
    const auto it = s.monsters.find(monster);
    if (it == s.monsters.end() || it->second.mode == State::Mode::Dead) return {};
    State::Monster& m = it->second;
    // Magical damage breaks a root, before the hit lands.
    bool changed = affliction != nullptr && affliction->magical && m.states.End(game::StateKind::Root);
    Damaged result = s.Hurt(m, attacker, damage, now);
    if (result.killed || s.nests[m.nest].spec.fixed) return result;
    s.Rearm(m, now);
    // A hit with damage wakes it and breaks a stun one time in four; then it turns on the
    // attacker; then the hit's own states, then its knock.
    if (damage > 0 && !m.states.Damaged(m.rolls, s.random).empty()) changed = true;
    // Its memory: the hit's threat (its damage when none given), as its tactics take it.
    const std::int64_t threat = affliction != nullptr && affliction->threatGiven ? affliction->threat : std::int64_t(damage);
    s.Attacked(m, attacker, damage, threat, affliction != nullptr ? affliction->threatPercent : 0, now);
    if (affliction != nullptr) {
        for (const game::ActiveState& state : affliction->states) {
            if (!m.states.Apply(state)) continue;
            changed = true;
            s.Took(m, state, now);
        }
        if (affliction->knock != game::Knock::None) s.Knock(m, *affliction, now, result);
        if (affliction->debuff != nullptr) {
            std::vector<const assets::SkillRow*> running;
            for (const State::Monster::Debuff& d : m.debuffs) running.push_back(d.skill);
            const game::BuffAdmission admission = game::AdmitBuff(*affliction->debuff, running);
            if (admission.admitted) {
                std::vector<std::size_t> replaced = admission.replaces;
                std::sort(replaced.rbegin(), replaced.rend());
                for (const std::size_t i : replaced) m.debuffs.erase(m.debuffs.begin() + std::ptrdiff_t(i));
                m.debuffs.push_back({affliction->debuff, now + std::chrono::milliseconds(affliction->debuffMs), affliction->debuffChanges});
                changed = true;
            }
        }
    }
    if (changed) s.Refresh(m);
    result.statesChanged = changed;
    return result;
}

bool Monsters::Boost(std::uint32_t monster, const assets::SkillRow* skill, std::vector<game::StatChange> changes, std::chrono::milliseconds ms,
                     Clock::time_point now) {
    State& s = *s_;
    const auto it = s.monsters.find(monster);
    if (it == s.monsters.end() || it->second.mode == State::Mode::Dead) return false;
    State::Monster& m = it->second;
    std::erase_if(m.debuffs, [&](const State::Monster::Debuff& d) { return d.skill == skill; });
    m.debuffs.push_back({skill, now + ms, std::move(changes)});
    s.Refresh(m);
    return true;
}

void Monsters::AddThreat(std::uint32_t monster, std::uint32_t source, std::int64_t threat, Clock::time_point now) {
    State& s = *s_;
    const auto it = s.monsters.find(monster);
    if (it == s.monsters.end() || it->second.mode == State::Mode::Dead) return;
    s.Attacked(it->second, source, 0, threat, 0, now);
}

std::vector<std::uint32_t> Monsters::TargetingOf(std::uint32_t entity) const {
    std::vector<std::uint32_t> out;
    for (const auto& [id, m] : s_->monsters) {
        if (m.mode == State::Mode::Chase && m.target == entity) out.push_back(id);
    }
    return out;
}

std::pair<std::uint32_t, std::int64_t> Monsters::CurrentTarget(std::uint32_t monster) const {
    const auto it = s_->monsters.find(monster);
    if (it == s_->monsters.end() || it->second.mode != State::Mode::Chase) return {0, 0};
    return {it->second.foe.entity, it->second.foe.threat};
}

const game::CombatStats* Monsters::Stats(std::uint32_t entity) const {
    const auto it = s_->monsters.find(entity);
    return it == s_->monsters.end() || it->second.mode == State::Mode::Dead ? nullptr : &it->second.current;
}

std::optional<Monsters::Target> Monsters::TargetOf(std::uint32_t entity) const {
    const auto it = s_->monsters.find(entity);
    if (it == s_->monsters.end() || it->second.mode == State::Mode::Dead) return std::nullopt;
    const State::Monster& m = it->second;
    const State::Nest& nest = s_->nests[m.nest];
    Target t;
    t.defence = nest.defence;
    t.states = m.states.Mask();
    t.susceptibility = m.states.Susceptibility();
    t.down = m.knocked && m.down;
    t.canFall = nest.canFall && !t.down;
    t.canPush = nest.canPush;
    t.recoverMs = nest.recoverMs;
    return t;
}

std::vector<proto::StateRecord> Monsters::StatesOf(std::uint32_t entity, Clock::time_point now) const {
    std::vector<proto::StateRecord> out;
    const auto it = s_->monsters.find(entity);
    if (it == s_->monsters.end()) return out;
    for (const game::ActiveState& a : it->second.states.All()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(a.Ends() - now).count();
        out.push_back({std::uint8_t(a.kind), std::uint8_t(std::clamp(a.level, 0, 255)), std::uint32_t(std::max<long long>(0, left))});
    }
    return out;
}

game::StateDefence MonsterStateDefence(int level, const std::array<std::uint8_t, 32>& resist) {
    // The columns: ResistFrozen, Frostbite, Burn, EShock, Poison, Zombie, Sleep, Root, Slow, Fear,
    // Myopia, Blood, Stone, Dark, Stun, Disea, Chaos, CsePD, CseMD, CseSTR, CseINT, CseHP, CseMP,
    // Resist24, Bomb, ... The elements go by their bits (shock before burn); the buckets skip Stone and Resist24.
    game::StateDefence d;
    d.level = level;
    constexpr int kElements[] = {0, 1, 3, 2, 4, 5};
    for (std::size_t i = 0; i < 6; ++i) d.elementResist[i] = resist[std::size_t(kElements[i])];
    constexpr int kBuckets[game::kResistBuckets] = {6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 24};
    for (std::size_t b = 0; b < std::size(kBuckets); ++b) d.percent[b] = resist[std::size_t(kBuckets[b])];
    return d;
}

std::uint32_t Monsters::Hp(std::uint32_t entity) const {
    const auto it = s_->monsters.find(entity);
    return it == s_->monsters.end() || it->second.mode == State::Mode::Dead ? 0u : it->second.hp;
}

std::uint32_t Monsters::Heal(std::uint32_t entity, std::uint32_t amount) {
    const auto it = s_->monsters.find(entity);
    if (it == s_->monsters.end() || it->second.mode == State::Mode::Dead) return 0;
    State::Monster& m = it->second;
    m.hp = std::uint32_t(std::min<std::uint64_t>(std::uint64_t(m.hp) + amount, m.current.maxHp));
    s_->world.SetHp(entity, m.hp);
    return m.hp;
}

std::size_t Monsters::Alive() const { return s_->monsters.size(); }

std::size_t Monsters::Nests() const { return s_->nests.size(); }
std::size_t Monsters::ActiveNests() const { return s_->activeNests; }

} // namespace sr::server
