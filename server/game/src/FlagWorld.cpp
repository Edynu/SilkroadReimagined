#include "sr/server/FlagWorld.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>
#include <unordered_map>

#include "sr/assets/GameData.h"
#include "sr/game/FlagView.h"
#include "sr/core/Log.h"
#include "sr/game/EventProtocol.h"
#include "sr/server/Database.h"
#include "sr/server/EventStore.h"
#include "sr/server/ShardBus.h"
#include "sr/server/World.h"

namespace sr::server {

namespace proto = game::proto;
namespace flag = game::flag;

namespace {

// The matches' layers: above the Forgotten World's (Instances.cpp's 1..500,000), so a layer number names one place.
constexpr std::uint32_t kFirstLayer = 500'001, kLastLayer = 599'999;
constexpr std::chrono::seconds kSelectWait{15};     // ours: a pole walked to is acted on when reached
constexpr std::chrono::milliseconds kDoorLine{1000}; // ours: FF 08 at most once a second
constexpr std::int64_t kLateLine = 30;               // ours: a round's lines older than this are not told (a server started late)

// What the shard's game servers tell each other about the flag world (sr_events_<shard>).
struct FlagBus {
    enum Kind : std::uint8_t { Dev = 1, Assign = 2, Cancelled = 3 };
    std::uint8_t kind = Dev;
    std::uint16_t from = 0, server = 0;
    std::uint8_t action = 0, team = 0;
    std::uint32_t character = 0, layer = 0;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("from", from);
        v("server", server);
        v("action", action);
        v("team", team);
        v("character", character);
        v("layer", layer);
    }
};

proto::WorldPosition At(const flag::Point& p, std::uint32_t layer) {
    proto::WorldPosition w;
    w.region = flag::kRegion;
    w.x = p.x;
    w.y = p.y;
    w.z = p.z;
    w.layer = layer;
    return w;
}

float Distance(const proto::WorldPosition& a, const proto::WorldPosition& b) {
    const proto::FlatPosition fa = proto::ToFlat(a), fb = proto::ToFlat(b);
    return float(std::hypot(fa.x - fb.x, fa.z - fb.z));
}

std::int64_t Ms(FlagWorlds::Clock::time_point t) { return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count(); }

} // namespace

struct FlagWorlds::State {
    FlagWorldHost& host;
    World& world;
    Monsters* monsters; // null: no monsters on this server (a test's world)
    const assets::CharacterTable* characters;
    FlagWorldConfig config;
    std::mt19937 random;

    struct Member {
        std::int64_t character = 0;
        std::string name;
        std::uint8_t team = 0;
        int level = 1;
        std::uint16_t server = 0;
        proto::WorldPosition home;
        std::uint32_t entity = 0; // on this server, in the match (0: not in it)
    };
    struct Match {
        std::uint32_t layer = 0;
        int group = 0;
        game::FlagMatch flag;
        std::vector<Member> members;
        std::uint32_t poles[3] = {};
        std::vector<std::uint32_t> keys; // lying
        std::uint32_t dropped = 0;
        bool doorOpen[2] = {};
        std::vector<std::size_t> nests;
        std::size_t keyNest[2] = {SIZE_MAX, SIZE_MAX};
        Clock::time_point started;
        Clock::time_point nextClock;
        bool spawned = false, over = false;
        std::unordered_map<std::uint32_t, Clock::time_point> revivals;
        Member* ByEntity(std::uint32_t entity) {
            for (Member& m : members)
                if (entity != 0 && m.entity == entity) return &m;
            return nullptr;
        }
        Member* ByCharacter(std::int64_t character) {
            for (Member& m : members)
                if (m.character == character) return &m;
            return nullptr;
        }
    };
    std::map<std::uint32_t, Match> matches;
    std::unordered_map<std::uint32_t, std::uint32_t> inside;    // entity -> its match's layer
    std::unordered_map<std::int64_t, std::uint32_t> arrivals;   // a character coming over -> its match's layer
    std::set<std::uint32_t> registered;                         // on this server's queue
    std::uint32_t nextLayer = kFirstLayer;
    struct PendingSelect {
        std::uint32_t target = 0;
        Clock::time_point until;
    };
    std::unordered_map<std::uint32_t, PendingSelect> selects;
    std::unordered_map<std::uint32_t, Clock::time_point> doorLines;

    // The round.
    std::optional<game::FlagCycle> cycle;
    struct Steps {
        bool open = false, standby5 = false, standby1 = false, start = false, end = false, stop = false;
    } steps;
    bool cancelled = false;

    State(FlagWorldHost& h, World& w, Monsters* m, const assets::CharacterTable* c, FlagWorldConfig cfg, std::uint32_t seed)
        : host(h), world(w), monsters(m), characters(c), config(std::move(cfg)), random(seed) {}

    bool IsHost() { return host.FlagHostServer() == host.FlagServerId(); }
    std::uint32_t Model(std::string_view codename) const {
        const assets::ObjectRow* row = characters != nullptr ? characters->Find(codename) : nullptr;
        return row != nullptr ? row->refObjId : 0u;
    }
    Match* Of(std::uint32_t layer) {
        const auto it = matches.find(layer);
        return it != matches.end() ? &it->second : nullptr;
    }
    Match* MatchOf(std::uint32_t entity) {
        const auto it = inside.find(entity);
        return it != inside.end() ? Of(it->second) : nullptr;
    }

    // --- Lines ----------------------------------------------------------------------------------------------

    static std::vector<std::byte> NoticeBytes(bool personal, std::uint8_t code, std::uint32_t value = 0, std::uint8_t team = 0, std::string name = {}) {
        return proto::Encode(proto::FlagWorldNotice{std::uint8_t(personal ? 1 : 0), code, value, team, std::move(name)});
    }
    void Tell(std::uint32_t entity, game::FlagCode code, std::uint32_t value = 0, std::uint8_t team = 0, std::string name = {}) {
        host.FlagSend(entity, proto::FlagWorldNotice::kId, NoticeBytes(true, std::uint8_t(code), value, team, std::move(name)));
    }
    void TellState(std::uint32_t entity, game::FlagState state, std::uint32_t value = 0) {
        host.FlagSend(entity, proto::FlagWorldNotice::kId, NoticeBytes(false, std::uint8_t(state), value));
    }
    void Everyone(game::FlagState state) { host.FlagSendAll(proto::FlagWorldNotice::kId, NoticeBytes(false, std::uint8_t(state))); }
    // Every member in the match's world.
    template <typename F>
    void ForInside(Match& m, F&& f) {
        for (Member& member : m.members) {
            const auto it = inside.find(member.entity);
            if (member.entity != 0 && it != inside.end() && it->second == m.layer) f(member);
        }
    }
    std::string NameOf(Match& m, std::uint32_t entity) {
        if (const Member* member = m.ByEntity(entity)) return member->name;
        const auto p = host.FlagPlayer(entity);
        return p ? p->name : std::string();
    }

    // --- The round ---------------------------------------------------------------------------------------------

    void SetCycle(const game::FlagCycle& c) {
        cycle = c;
        steps = {};
        cancelled = false;
    }
    void Tick(Clock::time_point now) {
        const std::int64_t unix = host.FlagUnixNow();
        if (config.enabled && (!cycle || steps.stop)) {
            const auto next = game::NextFlagCycle(unix, config.schedule);
            if (next && (!cycle || next->open != cycle->open)) SetCycle(*next);
        }
        if (cycle) {
            const game::FlagCycle c = *cycle;
            const auto due = [&](bool& done, std::int64_t at) {
                if (done || unix < at) return false;
                done = true;
                return true;
            };
            if (due(steps.open, c.open) && unix - c.open <= kLateLine) Everyone(game::FlagState::Open);
            if (due(steps.standby5, c.standby5) && c.standby5 > c.open && unix - c.standby5 <= kLateLine) Everyone(game::FlagState::Standby5);
            if (due(steps.standby1, c.standby1) && c.standby1 > c.standby5 && c.standby1 > c.open && unix - c.standby1 <= kLateLine) {
                Everyone(game::FlagState::Standby1);
            }
            if (due(steps.start, c.start)) Start(now);
            if (due(steps.end, c.end) && !cancelled) End(now);
            if (due(steps.stop, c.stop)) Stop();
        }
        for (auto& [layer, m] : matches) Step(m, now);
        // A pole walked to: acted on once reached.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> reached;
        for (auto it = selects.begin(); it != selects.end();) {
            const auto p = host.FlagPlayer(it->first);
            const auto there = world.Where(it->second.target);
            if (now > it->second.until || !p || !there) {
                it = selects.erase(it);
                continue;
            }
            if (Distance(p->at, there->position) <= flag::kReach + 1.0f && !world.Moving(it->first)) {
                reached.emplace_back(it->first, it->second.target);
                it = selects.erase(it);
                continue;
            }
            ++it;
        }
        for (const auto& [entity, target] : reached) Select(entity, target);
    }
    bool RegistrationOpen() const {
        if (!cycle || !steps.open || steps.start) return false;
        const std::int64_t unix = host.FlagUnixNow();
        return unix >= cycle->open && unix < cycle->start;
    }

    void Dev(std::uint8_t action) {
        const std::int64_t unix = host.FlagUnixNow();
        const bool running = cycle && steps.start && !steps.stop;
        switch (action) {
        case 1: // registration now
            if (running) return;
            SetCycle(game::FlagCycleFrom(unix, config.schedule));
            break;
        case 2: { // the play now
            if (running) return;
            if (!cycle || steps.stop || unix < cycle->open) SetCycle(game::FlagCycleFrom(unix, config.schedule));
            game::FlagCycle c = *cycle;
            c.start = std::min(c.start, unix);
            c.standby5 = std::min(c.standby5, c.start);
            c.standby1 = std::min(c.standby1, c.start);
            c.end = c.start + config.schedule.playSeconds;
            c.stop = c.end + config.schedule.winnersSeconds;
            cycle = c;
            steps.standby5 = steps.standby1 = true; // started early: no "in 5 / 1 minutes" lines
            break;
        }
        case 3: // the end now (the results)
            if (!running || steps.end) return;
            cycle->end = unix;
            cycle->stop = unix + config.schedule.winnersSeconds;
            break;
        case 4: // everything off: everyone out, no results
            if (!cycle) return;
            cancelled = true;
            cycle->end = std::min(cycle->end, unix);
            cycle->stop = unix;
            if (!steps.start) steps.start = steps.open = steps.standby5 = steps.standby1 = true; // nothing formed
            break;
        default: return;
        }
        core::LogInfo("flag world: the developer's switch {}", action);
    }

    // The play's start: the queue's registrations closed; the host forms the matches.
    void Start(Clock::time_point now) {
        registered.clear();
        Everyone(game::FlagState::Closed);
        Everyone(game::FlagState::Started);
        if (!IsHost()) return;
        host.FlagSubmit(
            [this](Database& db) {
                std::string error;
                taken.clear();
                if (!TakeEventEntries(db, EventKind::FlagWorld, taken, error)) core::LogWarning("flag world: the queue: {}", error);
            },
            [this, now] { Form(now); });
    }
    std::vector<EventEntryRow> taken; // the queue read at the start (the database's thread, then the loop's)

    void Form(Clock::time_point now) {
        std::vector<EventEntryRow> rows = std::move(taken);
        taken.clear();
        std::unordered_map<std::int64_t, const EventEntryRow*> byCharacter;
        std::vector<game::FlagEntry> entries;
        for (const EventEntryRow& r : rows) {
            byCharacter[r.character] = &r;
            entries.push_back({std::uint32_t(r.character), r.level, r.registered});
        }
        // The first team of each match a coin's (the shard manager's rand % 2, SM).
        const game::FlagPlan plan = game::PlanMatches(entries, config.minPlayers, config.teamMost, [] { return std::rand() & 1; });
        for (const std::uint32_t character : plan.cancelled) {
            const EventEntryRow& r = *byCharacter[character];
            if (r.server == host.FlagServerId()) {
                if (const std::uint32_t entity = host.FlagEntityOf(r.character)) Tell(entity, game::FlagCode::TooFew);
            } else {
                Publish({FlagBus::Cancelled, host.FlagServerId(), r.server, 0, 0, character, 0});
            }
        }
        for (const game::FlagMatchPlan& mp : plan.matches) {
            Match* m = Create(mp.group, now);
            if (m == nullptr) break;
            for (const auto& [character, team] : mp.players) {
                const EventEntryRow& r = *byCharacter[character];
                Member member;
                member.character = r.character;
                member.name = r.name;
                member.team = team;
                member.level = r.level;
                member.server = r.server;
                member.home.region = r.region;
                member.home.x = r.x;
                member.home.y = r.y;
                member.home.z = r.z;
                m->members.push_back(member);
            }
            core::LogInfo("flag world: a match of group {} in layer {} ({} players)", m->group, m->layer, m->members.size());
            for (const Member& member : std::vector<Member>(m->members)) {
                if (member.server == host.FlagServerId()) {
                    if (const std::uint32_t entity = host.FlagEntityOf(member.character)) Admit(entity, *m, member.character);
                } else {
                    arrivals[member.character] = m->layer;
                    Publish({FlagBus::Assign, host.FlagServerId(), member.server, 0, member.team, std::uint32_t(member.character), m->layer});
                }
            }
        }
    }

    std::uint32_t NextLayer() {
        for (std::uint32_t tries = 0; tries <= kLastLayer - kFirstLayer; ++tries) {
            const std::uint32_t layer = nextLayer;
            nextLayer = nextLayer >= kLastLayer ? kFirstLayer : nextLayer + 1;
            if (!matches.contains(layer)) return layer;
        }
        return 0;
    }

    Match* Create(int group, Clock::time_point now) {
        const std::uint32_t layer = NextLayer();
        if (layer == 0) return nullptr;
        Match& m = matches[layer];
        m.layer = layer;
        m.group = group;
        m.flag.Reset();
        m.started = now;
        m.nextClock = now + std::chrono::milliseconds(config.warmupMs + flag::kClockMs);
        for (int pole = 0; pole < 3; ++pole) m.poles[pole] = world.AddNpc(Model(m.flag.OnPole(pole) ? flag::kFlapCodenames[pole] : flag::kPoleCodenames[pole]),
                                                                          At(flag::kPoles[pole], layer), 0.0f);
        for (int team = 0; team < 2; ++team) world.SetNavZone(0, std::string(flag::kDoorZones[team]), true, layer);
        return &m;
    }

    void Close(Match& m) {
        if (monsters != nullptr)
            for (const std::size_t nest : m.nests) monsters->RemoveNest(nest);
        for (const std::uint32_t pole : m.poles)
            if (pole != 0) world.Leave(pole);
        for (const std::uint32_t key : m.keys) world.Leave(key);
        if (m.dropped != 0) world.Leave(m.dropped);
        for (int team = 0; team < 2; ++team) world.SetNavZone(0, std::string(flag::kDoorZones[team]), false, m.layer);
        core::LogInfo("flag world: the match in layer {} closes", m.layer);
    }

    // Into its match: at its camp in the match's layer, its team shown, untouchable a while, told the match's world.
    void Admit(std::uint32_t entity, Match& m, std::int64_t character) {
        Member* member = m.ByCharacter(character);
        if (member == nullptr || m.over) return;
        member->entity = entity;
        inside[entity] = m.layer;
        host.FlagLeaveParty(entity);
        host.FlagTravel(entity, At(flag::kCamps[member->team], m.layer));
        host.FlagSetTeam(entity, member->team);
        host.FlagUntouchable(entity, std::chrono::milliseconds(flag::kUntouchableMs));
        TellState(entity, game::FlagState::MatchWorld, flag::kWorld);
        const auto played = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - m.started).count() - config.warmupMs;
        if (played > 0) Tell(entity, game::FlagCode::Clock, ClockValue(played));
        core::LogInfo("flag world: {} joins the match in layer {} on team {}", member->name, m.layer, member->team);
    }

    // The play's clock as the client counts it: down from its own 20:00 (game::FlagView::kTimerMs, the original's play), so
    // a shorter play (ours: --ctf-cycle's) is told as further on, the timer ending with it.
    std::uint32_t ClockValue(std::int64_t played) const {
        const std::int64_t playMs = (cycle ? cycle->end - cycle->start : config.schedule.playSeconds) * 1000 - config.warmupMs;
        const std::int64_t ahead = std::max<std::int64_t>(0, game::FlagView::kTimerMs - playMs);
        return std::uint32_t(std::clamp<std::int64_t>(played + ahead, 0, game::FlagView::kTimerMs));
    }

    // Home: out of the match, alive, to where it registered from (another server's town: a move there).
    void SendHome(std::uint32_t entity) {
        Match* m = MatchOf(entity);
        if (m == nullptr) return;
        Member* member = m->ByEntity(entity);
        std::vector<game::FlagMatch::Event> events;
        m->flag.Gone(entity, events);
        Apply(*m, events);
        m->revivals.erase(entity);
        inside.erase(entity);
        selects.erase(entity);
        host.FlagSetTeam(entity, 0xFF);
        if (const auto p = host.FlagPlayer(entity); p && p->dead) host.FlagRevive(entity, p->at);
        if (member != nullptr) {
            member->entity = 0;
            host.FlagTravel(entity, member->home);
        }
    }

    void End(Clock::time_point) {
        Everyone(game::FlagState::Ended);
        for (auto& [layer, m] : matches) {
            if (m.over) continue;
            m.over = true;
            const std::optional<std::uint8_t> winner = m.flag.Winner();
            if (const std::uint32_t carrier = m.flag.Carrier()) host.FlagBuff(carrier, false);
            std::vector<std::uint32_t> home;
            ForInside(m, [&](Member& member) {
                host.FlagDeed(member.character, "event-matches"); // a match played (the beta quests' degree 8)
                if (winner && member.team == *winner) {
                    const bool given = host.FlagGive(member.entity, flag::kTrophy, 1); // a full bag: lost
                    core::LogInfo("flag world: {} wins{}", member.name, given ? " an Ice Trophy" : " (no room: the trophy is lost)");
                    Tell(member.entity, game::FlagCode::Won);
                } else {
                    Tell(member.entity, winner ? game::FlagCode::Lost : game::FlagCode::Draw);
                    home.push_back(member.entity);
                }
            });
            for (const std::uint32_t entity : home) SendHome(entity);
            core::LogInfo("flag world: the match in layer {} ends: {}", layer, winner ? (*winner == 0 ? "red wins" : "blue wins") : "a draw");
        }
    }

    void Stop() {
        Everyone(game::FlagState::Stopped);
        registered.clear();
        arrivals.clear();
        for (auto& [layer, m] : matches) {
            std::vector<std::uint32_t> left;
            ForInside(m, [&](Member& member) { left.push_back(member.entity); });
            for (const std::uint32_t entity : left) SendHome(entity);
            Close(m);
        }
        matches.clear();
        selects.clear();
    }

    // --- A match's play ------------------------------------------------------------------------------------------

    void Step(Match& m, Clock::time_point now) {
        if (m.over) return;
        if (!m.spawned && now >= m.started + std::chrono::milliseconds(config.warmupMs)) Spawn(m);
        std::vector<game::FlagMatch::Event> events;
        m.flag.Tick(Ms(now), events);
        Apply(m, events);
        if (now >= m.nextClock) {
            m.nextClock = now + std::chrono::milliseconds(flag::kClockMs);
            const auto played = std::chrono::duration_cast<std::chrono::milliseconds>(now - m.started).count() - config.warmupMs;
            ForInside(m, [&](Member& member) { Tell(member.entity, game::FlagCode::Clock, ClockValue(played)); });
        }
        for (auto it = m.revivals.begin(); it != m.revivals.end();) {
            if (now < it->second) {
                ++it;
                continue;
            }
            const std::uint32_t entity = it->first;
            it = m.revivals.erase(it);
            const Member* member = m.ByEntity(entity);
            const auto p = host.FlagPlayer(entity);
            if (member != nullptr && p && p->dead && inside.contains(entity)) host.FlagRevive(entity, At(flag::kCamps[member->team], m.layer));
        }
    }

    // Each room's ten monsters of the match's group around its anchor, the key's carrier one of them at random;
    // spawned once.
    void Spawn(Match& m) {
        m.spawned = true;
        const std::uint32_t model = Model(flag::MonsterOfGroup(m.group));
        if (model == 0 || monsters == nullptr) return;
        for (int team = 0; team < 2; ++team) {
            const int key = int(random() % flag::kMonstersPerRoom);
            for (int i = 0; i < flag::kMonstersPerRoom; ++i) {
                MonsterNest n;
                n.refObjId = model;
                n.position = At(flag::kAnchors[team], m.layer);
                n.spawnRadius = flag::kMonsterSpread;
                n.radius = flag::kMonsterSpread * 2.0f;
                n.maxCount = 1;
                n.respawn = false;
                const std::size_t index = monsters->AddNest(n);
                if (index == SIZE_MAX) continue;
                m.nests.push_back(index);
                if (i == key) m.keyNest[team] = index;
            }
        }
    }

    void SwapPole(Match& m, int pole) {
        if (m.poles[pole] != 0) world.Leave(m.poles[pole]);
        m.poles[pole] = world.AddNpc(Model(m.flag.OnPole(pole) ? flag::kFlapCodenames[pole] : flag::kPoleCodenames[pole]), At(flag::kPoles[pole], m.layer), 0.0f);
    }

    void Apply(Match& m, const std::vector<game::FlagMatch::Event>& events) {
        using K = game::FlagMatch::Event::Kind;
        for (const game::FlagMatch::Event& e : events) {
            const std::string name = NameOf(m, e.actor);
            switch (e.kind) {
            case K::Started:
                host.FlagSendAround(e.actor, proto::FlagWorldNotice::kId, NoticeBytes(false, std::uint8_t(game::FlagState::Action), e.actor));
                host.FlagHold(e.actor);
                break;
            case K::Carrier:
                if (e.pole >= 0) SwapPole(m, e.pole);
                host.FlagBuff(e.actor, true);
                ForInside(m, [&](Member& member) {
                    if (member.team == e.team) Tell(member.entity, game::FlagCode::OurCarrier, 0, e.team, name);
                    else Tell(member.entity, game::FlagCode::TheirCarrier);
                });
                core::LogInfo("flag world: {} carries the flag (layer {})", name, m.layer);
                break;
            case K::Placed:
                SwapPole(m, e.pole);
                host.FlagBuff(e.actor, false);
                ForInside(m, [&](Member& member) { Tell(member.entity, game::FlagCode::Placed, 0, e.team, name); });
                core::LogInfo("flag world: {} places the flag on pole {} (layer {})", name, e.pole, m.layer);
                break;
            case K::Dropped: {
                host.FlagBuff(e.actor, false);
                const auto where = world.Where(e.actor);
                proto::WorldPosition at = where ? where->position : At(flag::kPoles[0], m.layer);
                at.layer = m.layer;
                if (m.dropped != 0) world.Leave(m.dropped);
                m.dropped = world.AddNpc(Model(flag::kDroppedFlag), at, 0.0f);
                ForInside(m, [&](Member& member) { Tell(member.entity, game::FlagCode::Dropped); });
                core::LogInfo("flag world: the flag drops (layer {})", m.layer);
                break;
            }
            }
        }
    }

    // A pole, the key or the dropped flag talked to.
    bool Select(std::uint32_t entity, std::uint32_t target) {
        Match* m = MatchOf(entity);
        if (m == nullptr || target == 0) return false;
        int pole = -1;
        for (int i = 0; i < 3; ++i)
            if (m->poles[i] == target) pole = i;
        const auto key = std::find(m->keys.begin(), m->keys.end(), target);
        if (pole < 0 && key == m->keys.end() && target != m->dropped) return false;
        Member* member = m->ByEntity(entity);
        const auto p = host.FlagPlayer(entity);
        if (member == nullptr || !p || p->dead || m->over) return true;
        const std::int64_t now = Ms(Clock::now());
        std::vector<game::FlagMatch::Event> events;
        if (pole >= 0) {
            const proto::WorldPosition at = At(flag::kPoles[pole], m->layer);
            if (Distance(p->at, at) > flag::kReach) {
                // Walked there, 10 from the pole on his side; ours: acted on once reached.
                const float d = std::max(0.001f, Distance(p->at, at));
                proto::WorldPosition to = at;
                const proto::FlatPosition a = proto::ToFlat(p->at), b = proto::ToFlat(at);
                to.x = at.x + float((a.x - b.x) / d) * flag::kWalkTo;
                to.z = at.z + float((a.z - b.z) / d) * flag::kWalkTo;
                host.FlagWalkTo(entity, to);
                selects[entity] = {target, Clock::now() + kSelectWait};
                core::LogInfo("flag world: {} walks to pole {} ({:.0f} away)", member->name, pole, Distance(p->at, at));
                return true;
            }
            const auto code = m->flag.SelectPole(pole, entity, member->team, now, events);
            if (code && *code != game::FlagCode::Done) Tell(entity, *code);
            core::LogInfo("flag world: {} at pole {}: {}", member->name, pole, !code ? "nothing to do" : *code == game::FlagCode::Done ? "the action starts" : "busy");
            Apply(*m, events);
            return true;
        }
        if (key != m->keys.end()) {
            // The key: its taker's team's door open for the match, for everyone.
            world.Leave(*key);
            m->keys.erase(key);
            const std::uint8_t team = member->team;
            if (!m->doorOpen[team]) {
                m->doorOpen[team] = true;
                world.SetNavZone(0, std::string(flag::kDoorZones[team]), false, m->layer);
            }
            ForInside(*m, [&](Member& x) { Tell(x.entity, game::FlagCode::KeyTaken, flag::kDoorZoneIds[team], team, member->name); });
            core::LogInfo("flag world: {} takes a key (layer {}): the {} door opens", member->name, m->layer, team == 0 ? "red" : "blue");
            return true;
        }
        // The dropped flag: refused on a blocked way.
        const auto there = world.Where(m->dropped);
        if (!there || !host.FlagWayClear(p->at, there->position)) {
            Tell(entity, game::FlagCode::FlagBlocked);
            return true;
        }
        if (m->flag.PickUp(entity, member->team, now, events)) {
            world.Leave(m->dropped);
            m->dropped = 0;
        }
        Apply(*m, events);
        return true;
    }

    void Publish(const FlagBus& b) { host.FlagPublish(BusToHex(proto::Encode(b))); }
    void OnBus(const FlagBus& b) {
        if (b.from == host.FlagServerId()) return;
        if (b.kind == FlagBus::Dev) return Dev(b.action);
        if (b.server != host.FlagServerId()) return;
        const std::uint32_t entity = host.FlagEntityOf(b.character);
        if (b.kind == FlagBus::Cancelled) {
            if (entity != 0) Tell(entity, game::FlagCode::TooFew);
            return;
        }
        if (b.kind == FlagBus::Assign && entity != 0) {
            // To the host's camp (a move there; taken into its match on arrival).
            core::LogInfo("flag world: character {} goes to its match on game server {}", b.character, b.from);
            host.FlagTravel(entity, At(flag::kCamps[b.team > 1 ? 0 : b.team], 0));
        }
    }
};

FlagWorlds::FlagWorlds(FlagWorldHost& host, World& world, Monsters* monsters, const assets::CharacterTable* characters, FlagWorldConfig config,
                       std::uint32_t seed)
    : s_(std::make_unique<State>(host, world, monsters, characters, std::move(config), seed)) {}

FlagWorlds::~FlagWorlds() = default;

void FlagWorlds::Enter(std::uint32_t entity) {
    const auto p = s_->host.FlagPlayer(entity);
    if (!p) return;
    const auto it = s_->arrivals.find(p->character);
    if (it == s_->arrivals.end()) return;
    const std::uint32_t layer = it->second;
    s_->arrivals.erase(it);
    if (State::Match* m = s_->Of(layer)) s_->Admit(entity, *m, p->character);
}

void FlagWorlds::Leave(std::uint32_t entity) {
    if (s_->registered.erase(entity) != 0) {
        if (const auto p = s_->host.FlagPlayer(entity)) {
            s_->host.FlagSubmit(
                [character = p->character](Database& db) {
                    std::string error;
                    if (!DeleteEventEntry(db, EventKind::FlagWorld, character, error)) core::LogWarning("flag world: a cancel: {}", error);
                },
                {});
        }
    }
    s_->selects.erase(entity);
    State::Match* m = s_->MatchOf(entity);
    if (m == nullptr) return;
    std::vector<game::FlagMatch::Event> events;
    m->flag.Gone(entity, events); // his flag dropped where he stood, his action cancelled
    s_->Apply(*m, events);
    m->revivals.erase(entity);
    if (State::Member* member = m->ByEntity(entity)) member->entity = 0; // kept in the record: no reward, no way back in
    s_->inside.erase(entity);
}

bool FlagWorlds::Handle(std::uint32_t entity, proto::MessageId id, std::span<const std::byte> payload, bool devCommands) {
    State& s = *s_;
    switch (id) {
    case proto::FlagWorldRegister::kId: {
        const auto p = s.host.FlagPlayer(entity);
        if (!p) return true;
        if (s.registered.erase(entity) != 0) {
            // A cancel.
            s.host.FlagSubmit(
                [character = p->character](Database& db) {
                    std::string error;
                    if (!DeleteEventEntry(db, EventKind::FlagWorld, character, error)) core::LogWarning("flag world: a cancel: {}", error);
                },
                {});
            s.Tell(entity, game::FlagCode::Done);
            core::LogInfo("flag world: {} cancels the registration", p->name);
            return true;
        }
        const game::FlagCode refusal = game::RegistrationRefusal(p->entrant);
        if (refusal != game::FlagCode::Done) {
            s.Tell(entity, refusal);
            core::LogInfo("flag world: {} refused (code {})", p->name, int(refusal));
            return true;
        }
        if (!s.config.enabled && !s.cycle) {
            s.Tell(entity, game::FlagCode::NotNow);
            return true;
        }
        if (!s.RegistrationOpen()) {
            s.Tell(entity, game::FlagCode::NotNow);
            return true;
        }
        s.registered.insert(entity);
        EventEntryRow row;
        row.event = EventKind::FlagWorld;
        row.character = p->character;
        row.name = p->name;
        row.level = p->entrant.maxLevel;
        row.server = s.host.FlagServerId();
        row.region = p->at.region;
        row.x = p->at.x;
        row.y = p->at.y;
        row.z = p->at.z;
        row.registered = s.host.FlagUnixNow() * 1000 + std::int64_t(s.random() % 1000);
        s.host.FlagSubmit(
            [row](Database& db) {
                std::string error;
                if (!SaveEventEntry(db, row, error)) core::LogWarning("flag world: a registration: {}", error);
            },
            [this, entity] {
                if (s_->registered.contains(entity)) s_->Tell(entity, game::FlagCode::Done);
            });
        core::LogInfo("flag world: {} registers (level {})", p->name, p->entrant.maxLevel);
        return true;
    }
    case proto::EventTalk::kId: {
        proto::EventTalk talk;
        if (!proto::Decode(payload, talk)) return false;
        const game::SwapQuest* quest = game::SwapQuestById(talk.quest);
        if (quest == nullptr) return true;
        proto::EventSays says;
        says.npc = talk.npc;
        says.quest = talk.quest;
        if (talk.choice == 0xFF) {
            says.text = game::kSwapTalkA;
            says.rows = {std::string(quest == &game::SwapTickets() ? "SN_TALK_QEV_CH_EVENT_KISAENG_070910_B" : game::kSwapTalkB), std::string(game::kTalkExit)};
        } else if (talk.choice == 0) {
            // The swap (ours one order for both: the checks, the draw, the tokens taken, the prize given).
            if (s.host.FlagCount(entity, quest->token) < std::uint32_t(game::kSwapTokens)) {
                says.text = game::kSwapTalkE;
            } else if (!s.host.FlagRoom(entity)) {
                says.text = game::kSwapTalkD;
            } else {
                const float r = float(std::uniform_int_distribution<int>(0, 32767)(s.random)) / 32767.0f * 100.0f;
                const game::SwapPrize& prize = quest->prizes[game::DrawPrize(quest->prizes, r)];
                if (!s.host.FlagTake(entity, quest->token, game::kSwapTokens)) return true;
                if (!s.host.FlagGive(entity, prize.item, prize.count)) core::LogWarning("flag world: a swap's prize {} not given", prize.item);
                says.text = game::kSwapTalkC;
                core::LogInfo("flag world: entity {} swaps {} x10 for {} x{}", entity, quest->token, prize.item, prize.count);
            }
            says.rows = {std::string(game::kTalkExit)};
        } else {
            return true;
        }
        s.host.FlagSend(entity, proto::EventSays::kId, proto::Encode(says));
        return true;
    }
    case proto::FlagWorldDev::kId: {
        proto::FlagWorldDev dev;
        if (!proto::Decode(payload, dev)) return false;
        if (!devCommands) return true;
        Dev(dev.action);
        return true;
    }
    default: return false;
    }
}

void FlagWorlds::Dev(std::uint8_t action) {
    s_->Dev(action);
    s_->Publish({FlagBus::Dev, s_->host.FlagServerId(), 0, action, 0, 0, 0});
}

bool FlagWorlds::Select(std::uint32_t entity, std::uint32_t target) { return s_->Select(entity, target); }

void FlagWorlds::TellDesk(std::uint32_t entity, std::uint32_t npc, std::string_view codename) {
    if (codename != game::flag::kSoOk) return;
    proto::EventDesk desk;
    desk.npc = npc;
    desk.flagState = !s_->config.enabled && !s_->cycle ? 0 : s_->registered.contains(entity) ? 2 : 1;
    desk.quests.push_back({game::SwapIceTrophies().id, std::string(game::SwapIceTrophies().title)});
    s_->host.FlagSend(entity, proto::EventDesk::kId, proto::Encode(desk));
}

bool FlagWorlds::UseItem(std::uint32_t entity, std::uint8_t slot, const assets::ItemRow& row) {
    if (row.codename != game::flag::kCard) return false;
    const auto p = s_->host.FlagPlayer(entity);
    if (!p || !s_->host.FlagUseUp(entity, slot)) return true;
    // _queued, its answer never read; the box at once.
    s_->host.FlagSubmit(
        [account = p->account](Database& db) {
            std::string error;
            if (!AddFlagEventParticipant(db, account, error)) core::LogWarning("flag world: the card: {}", error);
        },
        {});
    s_->Tell(entity, game::FlagCode::Subscribed);
    core::LogInfo("flag world: {} uses a Subscription Card", p->name);
    return true;
}

void FlagWorlds::Killed(const Monsters::Damaged& killed, const proto::WorldPosition& at) {
    if (killed.nest < 0) return;
    for (auto& [layer, m] : s_->matches) {
        for (int team = 0; team < 2; ++team) {
            if (m.keyNest[team] != std::size_t(killed.nest)) continue;
            m.keyNest[team] = SIZE_MAX;
            proto::WorldPosition where = at;
            where.layer = layer;
            m.keys.push_back(s_->world.AddNpc(s_->Model(game::flag::kKey), where, 0.0f));
            core::LogInfo("flag world: a key lies in layer {}", layer);
            return;
        }
    }
}

void FlagWorlds::Died(std::uint32_t victim, std::uint32_t killer) {
    State& s = *s_;
    State::Match* m = s.MatchOf(victim);
    if (m == nullptr) return;
    std::vector<game::FlagMatch::Event> events;
    m->flag.Gone(victim, events);
    s.Apply(*m, events);
    s.selects.erase(victim);
    m->revivals[victim] = Clock::now() + std::chrono::milliseconds(game::flag::kReviveMs);
    s.Tell(victim, game::FlagCode::Dead);
    // The kill's trophy: the levels at most 9 apart, one Ice Trophy (a full bag: lost).
    if (killer == 0 || s.MatchOf(killer) != m || m->over) return;
    const auto a = s.host.FlagPlayer(killer), b = s.host.FlagPlayer(victim);
    if (!a || !b || std::abs(a->level - b->level) > game::flag::kTrophyLevelGap) return;
    const bool given = s.host.FlagGive(killer, game::flag::kTrophy, 1);
    core::LogInfo("flag world: {} kills {}{}", a->name, b->name, given ? ": an Ice Trophy" : " (no room for the trophy)");
}

bool FlagWorlds::Resurrect(std::uint32_t entity) {
    State::Match* m = s_->MatchOf(entity);
    if (m == nullptr) return false;
    const auto it = m->revivals.find(entity);
    if (it != m->revivals.end()) it->second = std::min(it->second, Clock::now() + std::chrono::milliseconds(game::flag::kCampButtonMs));
    return true;
}

void FlagWorlds::Walked(std::uint32_t entity, const proto::WorldPosition& from, const proto::WorldPosition& to) {
    State::Match* m = s_->MatchOf(entity);
    if (m == nullptr || from.region != game::flag::kRegion || to.region != game::flag::kRegion) return;
    for (int team = 0; team < 2; ++team) {
        if (m->doorOpen[team]) continue;
        const game::flag::Point& door = game::flag::kDoorCentres[team];
        // Ours: a walk across the doorway's line (its two cells, about 52 wide) while it is locked.
        const float a = from.z - door.z, b = to.z - door.z;
        if ((a > 0) == (b > 0) && std::abs(b) > 6.0f) continue;
        const float t = std::abs(a - b) < 0.001f ? 0.0f : a / (a - b);
        const float x = from.x + (to.x - from.x) * std::clamp(t, 0.0f, 1.0f);
        if (std::abs(x - door.x) > 30.0f) continue;
        const auto now = Clock::now();
        auto& last = s_->doorLines[entity];
        if (now - last < kDoorLine) return;
        last = now;
        s_->Tell(entity, game::FlagCode::DoorLocked);
        return;
    }
}

void FlagWorlds::Tick(Clock::time_point now) { s_->Tick(now); }

void FlagWorlds::BusMessage(const std::string& text) {
    const auto bytes = BusFromHex(text);
    FlagBus b;
    if (bytes && proto::Decode(*bytes, b)) s_->OnBus(b);
}

bool FlagWorlds::Hostile(std::uint32_t a, std::uint32_t b) const {
    const auto ia = s_->inside.find(a), ib = s_->inside.find(b);
    if (ia == s_->inside.end() || ib == s_->inside.end() || ia->second != ib->second) return false;
    State::Match* m = s_->Of(ia->second);
    if (m == nullptr || m->over) return false;
    const State::Member *ma = m->ByEntity(a), *mb = m->ByEntity(b);
    return ma != nullptr && mb != nullptr && ma->team != mb->team;
}

bool FlagWorlds::InMatch(std::uint32_t entity) const { return s_->inside.contains(entity); }

std::optional<proto::WorldPosition> FlagWorlds::ReturnPlace(std::uint32_t entity) const {
    State::Match* m = s_->MatchOf(entity);
    const State::Member* member = m != nullptr ? m->ByEntity(entity) : nullptr;
    if (member == nullptr) return std::nullopt;
    return member->home;
}

FlagWorlds::Phase FlagWorlds::CurrentPhase() const {
    const State& s = *s_;
    if (!s.cycle) return Phase::Off;
    const std::int64_t unix = s.host.FlagUnixNow();
    if (s.steps.stop || unix < s.cycle->open) return Phase::Waiting;
    if (!s.steps.start) return Phase::Registration;
    if (!s.steps.end) return Phase::Play;
    return Phase::Winners;
}

bool FlagWorlds::Registered(std::uint32_t entity) const { return s_->registered.contains(entity); }
std::size_t FlagWorlds::Matches() const { return s_->matches.size(); }
std::uint32_t FlagWorlds::LayerOf(std::uint32_t entity) const {
    const auto it = s_->inside.find(entity);
    return it != s_->inside.end() ? it->second : 0u;
}
std::uint8_t FlagWorlds::TeamOf(std::uint32_t entity) const {
    State::Match* m = s_->MatchOf(entity);
    const State::Member* member = m != nullptr ? m->ByEntity(entity) : nullptr;
    return member != nullptr ? member->team : std::uint8_t(0xFF);
}
std::vector<std::uint32_t> FlagWorlds::PolesOf(std::uint32_t layer) const {
    const State::Match* m = s_->Of(layer);
    return m != nullptr ? std::vector<std::uint32_t>{m->poles[0], m->poles[1], m->poles[2]} : std::vector<std::uint32_t>{};
}
std::uint32_t FlagWorlds::KeyOf(std::uint32_t layer) const {
    const State::Match* m = s_->Of(layer);
    return m != nullptr && !m->keys.empty() ? m->keys.front() : 0u;
}
std::uint32_t FlagWorlds::DroppedFlagOf(std::uint32_t layer) const {
    const State::Match* m = s_->Of(layer);
    return m != nullptr ? m->dropped : 0u;
}
std::vector<std::size_t> FlagWorlds::NestsOf(std::uint32_t layer) const {
    const State::Match* m = s_->Of(layer);
    return m != nullptr ? m->nests : std::vector<std::size_t>{};
}
bool FlagWorlds::DoorOpen(std::uint32_t layer, std::uint8_t team) const {
    const State::Match* m = s_->Of(layer);
    return m != nullptr && team < 2 && m->doorOpen[team];
}
const game::FlagMatch* FlagWorlds::MatchOf(std::uint32_t layer) const {
    const State::Match* m = s_->Of(layer);
    return m != nullptr ? &m->flag : nullptr;
}

} // namespace sr::server
