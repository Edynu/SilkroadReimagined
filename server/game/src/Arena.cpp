#include "sr/server/Arena.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>
#include <unordered_map>

#include "sr/assets/GameData.h"
#include "sr/core/Log.h"
#include "sr/game/EventProtocol.h"
#include "sr/server/Database.h"
#include "sr/server/EventStore.h"
#include "sr/server/ShardBus.h"
#include "sr/server/World.h"

namespace sr::server {

namespace proto = game::proto;
namespace ar = game::arena;
using game::ArenaCode;
using game::ArenaState;

namespace {

// The arena's layers: beyond the Forgotten World's and the flag world's.
constexpr std::uint32_t kFirstLayer = 600'001, kLastLayer = 699'999;
constexpr std::int64_t kLateLine = 30;

struct ArenaBus {
    enum Kind : std::uint8_t { Dev = 1, Assign = 2, Cancelled = 3 };
    std::uint8_t kind = Dev;
    std::uint16_t from = 0, server = 0; // server 0: whichever holds the character
    std::uint8_t action = 0, mode = 0, team = 0;
    std::uint32_t character = 0, layer = 0;
    proto::WorldPosition to;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("from", from);
        v("server", server);
        v("action", action);
        v("mode", mode);
        v("team", team);
        v("character", character);
        v("layer", layer);
        v("to", to);
    }
};

std::int64_t Ms(Arenas::Clock::time_point t) { return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count(); }

float Distance(const proto::WorldPosition& a, const proto::WorldPosition& b) {
    const proto::FlatPosition fa = proto::ToFlat(a), fb = proto::ToFlat(b);
    return float(std::hypot(fa.x - fb.x, fa.z - fb.z));
}

int PoleIndex(std::string_view codename) {
    for (int i = 0; i < 3; ++i)
        if (codename == ar::kPoleCodenames[i] || codename == ar::kFlapCodenames[i]) return i;
    return -1;
}

} // namespace

bool LoadArenaWorlds(Database& db, const assets::CharacterTable* characters, std::vector<ArenaWorldSpec>& out, std::string& error) {
    const QueryResult worlds = db.Query(
        "SELECT w.id, w.worldcodename128, 0, w.configgroupcodename128 FROM _refgame_world w "
        "WHERE w.worldcodename128 LIKE 'INS_ARENA_SCORE_%' OR w.worldcodename128 = 'INS_ARENA_FLAG_8' ORDER BY w.id");
    if (!worlds.Ok()) {
        error = worlds.Error();
        return false;
    }
    for (int i = 0; i < worlds.Rows(); ++i) {
        ArenaWorldSpec w;
        w.id = std::uint16_t(worlds.Int(i, 0));
        w.codename = worlds.Text(i, 1);
        w.kind = w.codename.find("FLAG") != std::string::npos ? ar::kFlag : ar::kScore;
        w.size = w.codename.ends_with("_32") ? 32 : w.codename.ends_with("_16") ? 16 : 8;
        const std::string group = worlds.Text(i, 3);
        const QueryResult config = db.Query("SELECT valuecodename128, value FROM _refgame_world_config WHERE groupcodename128 = $1", {group});
        bool gates[2] = {};
        for (int k = 0; config.Ok() && k < config.Rows(); ++k) {
            const std::string key = config.Text(k, 0), value = config.Text(k, 1);
            const int team = key == "REF_TELEPORT_BATTLE_ARENA_A_GATE" ? 0 : key == "REF_TELEPORT_BATTLE_ARENA_B_GATE" ? 1 : -1;
            if (team >= 0) {
                const QueryResult t = db.Query("SELECT genregionid, genpos_x, genpos_y, genpos_z FROM _refteleport WHERE codename128 = $1", {value});
                if (t.Ok() && t.Rows() == 1) {
                    proto::WorldPosition& g = w.gates[team];
                    g.region = std::uint16_t(t.Int(0, 0));
                    g.x = float(t.Double(0, 1));
                    g.y = float(t.Double(0, 2));
                    g.z = float(t.Double(0, 3));
                    gates[team] = true;
                }
            }
            if (key == "BATTLE_ARENA_REWARD_WIN") w.reward[0] = std::strtof(value.c_str(), nullptr);
            if (key == "BATTLE_ARENA_REWARD_LOSE") w.reward[1] = std::strtof(value.c_str(), nullptr);
            if (key == "BATTLE_ARENA_REWARD_DRAW") w.reward[2] = std::strtof(value.c_str(), nullptr);
        }
        if (!gates[0] || !gates[1]) continue;
        const QueryResult npcs = db.Query(
            "SELECT npccodename128, regionid, posx, posy, posz, param1, param2, param3, param4, param5, param6 FROM _refgameworldnpc "
            "WHERE worldcodename128 = $1 ORDER BY id",
            {w.codename});
        for (int k = 0; npcs.Ok() && k < npcs.Rows(); ++k) {
            ArenaWorldSpec::Spawn s;
            s.codename = npcs.Text(k, 0);
            const assets::ObjectRow* row = characters != nullptr ? characters->Find(std::string_view(s.codename)) : nullptr;
            s.refObjId = row != nullptr ? row->refObjId : 0u;
            s.at.region = std::uint16_t(npcs.Int(k, 1));
            s.at.x = float(npcs.Int(k, 2));
            s.at.y = float(npcs.Int(k, 3));
            s.at.z = float(npcs.Int(k, 4));
            s.group = int(npcs.Int(k, 5));
            s.points = int(npcs.Int(k, 6));
            s.count = std::max(1, int(npcs.Int(k, 7)));
            s.respawn = int(npcs.Int(k, 8));
            s.rarity = int(npcs.Int(k, 9));
            s.radius = int(npcs.Int(k, 10));
            w.spawns.push_back(std::move(s));
        }
        out.push_back(std::move(w));
    }
    return true;
}

struct Arenas::State {
    FlagWorldHost& host;
    ArenaHost& arena;
    World& world;
    Monsters* monsters;
    const assets::CharacterTable* characters;
    ArenaConfig config;
    std::mt19937 random;

    struct Member {
        std::int64_t character = 0;
        std::string name;
        std::uint8_t team = 0;
        int level = 1;
        proto::WorldPosition home; // region 0: its return point
        std::uint32_t entity = 0;
        std::uint32_t points = 0;
    };
    struct Match {
        std::uint32_t layer = 0;
        const ArenaWorldSpec* spec = nullptr;
        ar::Mode mode = ar::Mode::Random;
        int group = 0;
        std::vector<Member> members;
        std::uint32_t scores[2] = {}, kills[2] = {};
        game::FlagMatch flag;
        std::uint32_t poles[3] = {};
        proto::WorldPosition poleAt[3];
        std::uint32_t dropped = 0;
        std::unordered_map<std::size_t, std::uint32_t> nestPoints;
        std::unordered_map<std::uint32_t, Clock::time_point> revivals;
        bool playing = false, ended = false, scoreDirty = false;
        Clock::time_point playStart, nextClock, nextScore;
        std::uint32_t lengthMs = 0;
        Member* ByEntity(std::uint32_t e) {
            for (Member& m : members)
                if (e != 0 && m.entity == e) return &m;
            return nullptr;
        }
        Member* ByCharacter(std::int64_t c) {
            for (Member& m : members)
                if (m.character == c) return &m;
            return nullptr;
        }
    };
    std::map<std::uint32_t, Match> matches;
    std::unordered_map<std::uint32_t, std::uint32_t> inside;
    std::unordered_map<std::int64_t, std::pair<std::uint32_t, std::uint8_t>> arrivals; // character -> layer, team
    std::unordered_map<std::uint32_t, std::pair<std::int64_t, std::uint32_t>> registered; // entity -> character, group
    std::uint32_t nextLayer = kFirstLayer;

    struct Round {
        ar::Mode mode = ar::Mode::Random;
        std::uint16_t kinds = ar::kScoreBit | ar::kFlagBit;
        std::int64_t open = 0, close = 0, warn5 = 0, warn1 = 0, form = 0, soon = 0, play = 0, results = 0, stop = 0;
    };
    std::optional<Round> round;
    struct Steps {
        bool open = false, close = false, warn5 = false, warn1 = false, form = false, soon = false, play = false, results = false, stop = false;
    } steps;
    bool cancelled = false;
    std::vector<EventEntryRow> taken;

    State(FlagWorldHost& h, ArenaHost& a, World& w, Monsters* m, const assets::CharacterTable* c, ArenaConfig cfg, std::uint32_t seed)
        : host(h), arena(a), world(w), monsters(m), characters(c), config(std::move(cfg)), random(seed) {}

    bool IsHost() const {
        for (const ArenaWorldSpec& w : config.worlds) return arena.ArenaOwner(w.gates[0].region) == host.FlagServerId();
        return false;
    }
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
    std::uint8_t KindOf(const Match& m) const { return m.spec->kind; }

    // --- Lines -------------------------------------------------------------------------------------------------

    template <typename T>
    void Send(std::uint32_t entity, const T& message) {
        host.FlagSend(entity, T::kId, proto::Encode(message));
    }
    void Tell(std::uint32_t entity, ArenaCode code, std::uint32_t value = 0, std::uint8_t team = 0, std::string name = {}, std::uint32_t value2 = 0,
              std::uint8_t kind = 0) {
        proto::ArenaNotice n;
        n.personal = 1;
        n.code = std::uint8_t(code);
        n.kind = kind; // FF F0's kind: a client without the arena state makes its windows by it
        n.value = value;
        n.value2 = value2;
        n.team = team;
        n.name = std::move(name);
        if (round) n.mode = std::uint8_t(round->mode);
        Send(entity, n);
    }
    proto::ArenaNotice Broadcast(ArenaState state) const {
        proto::ArenaNotice n;
        n.code = std::uint8_t(state);
        if (round) {
            n.mode = std::uint8_t(round->mode);
            n.kinds = round->kinds;
        }
        return n;
    }
    void Everyone(ArenaState state) { host.FlagSendAll(proto::ArenaNotice::kId, proto::Encode(Broadcast(state))); }
    template <typename F>
    void ForInside(Match& m, F&& f) {
        for (Member& member : m.members) {
            const auto it = inside.find(member.entity);
            if (member.entity != 0 && it != inside.end() && it->second == m.layer) f(member);
        }
    }

    // --- The rounds --------------------------------------------------------------------------------------------

    Round RoundFrom(ar::Mode mode, std::int64_t open, std::int64_t registerSeconds, std::int64_t playSeconds) const {
        Round r;
        r.mode = mode;
        r.open = open;
        r.close = open + registerSeconds;
        r.warn5 = r.close;
        r.play = r.close + 20;
        r.warn1 = r.close;
        r.form = r.play - 15;
        r.soon = r.play - 10;
        r.results = r.play + playSeconds;
        r.stop = r.results + ar::kResultLeadSeconds;
        return r;
    }
    std::optional<Round> NextRound(std::int64_t now) const {
        if (config.cycleRegister > 0 && config.cyclePlay > 0) {
            const std::int64_t period = config.cycleRegister + 20 + config.cyclePlay + ar::kResultLeadSeconds + 5;
            const std::int64_t base = now - ((now % period) + period) % period;
            return RoundFrom(ar::Mode::Random, base, config.cycleRegister, config.cyclePlay);
        }
        std::optional<Round> best;
        for (int m = 0; m < 4; ++m) {
            const auto mode = ar::Mode(m);
            const auto p = game::SchedulePeriod(config.schedule, ar::ParticipationDefine(mode), now);
            if (!p) continue;
            const auto match = game::SchedulePeriod(config.schedule, ar::MatchDefine(mode), p->end);
            if (!match) continue;
            Round r;
            r.mode = mode;
            r.open = p->start;
            r.close = p->end;
            r.warn5 = match->start;
            r.warn1 = match->start + ar::kWarningSeconds - 60;
            r.play = match->start + ar::kWarningSeconds;
            r.form = r.play - 30;
            r.soon = r.play - 10;
            r.stop = match->end;
            r.results = std::max(r.play + 60, match->end - ar::kResultLeadSeconds);
            if (r.stop <= now) continue;
            if (!best || r.open < best->open) best = r;
        }
        return best;
    }
    void SetRound(const Round& r) {
        round = r;
        steps = {};
        cancelled = false;
    }
    bool RegistrationOpen() const {
        if (!round || !steps.open || steps.close) return false;
        return true;
    }

    void Tick(Clock::time_point now) {
        const std::int64_t unix = host.FlagUnixNow();
        if (config.enabled && (!round || steps.stop)) {
            if (const auto next = NextRound(unix); next && (!round || next->open != round->open)) SetRound(*next);
        }
        if (round) {
            const Round r = *round;
            const auto due = [&](bool& done, std::int64_t at) {
                if (done || unix < at) return false;
                done = true;
                return true;
            };
            if (due(steps.open, r.open) && unix - r.open <= kLateLine) Everyone(ArenaState::Open);
            if (due(steps.close, r.close)) {
                registered.clear();
                Everyone(ArenaState::Closed);
            }
            if (due(steps.warn5, r.warn5) && r.warn5 + 60 < r.play && unix - r.warn5 <= kLateLine) Everyone(ArenaState::Warning5);
            if (due(steps.warn1, r.warn1) && r.warn1 > r.warn5 && unix - r.warn1 <= kLateLine) Everyone(ArenaState::Warning1);
            if (due(steps.form, r.form) && !cancelled && IsHost()) Form(r);
            if (due(steps.soon, r.soon) && !cancelled) {
                for (auto& [layer, m] : matches) {
                    proto::ArenaNotice n = Broadcast(ArenaState::Soon);
                    n.kind = KindOf(m);
                    ForInside(m, [&](Member& x) { Send(x.entity, n); });
                }
            }
            if (due(steps.play, r.play) && !cancelled) {
                for (auto& [layer, m] : matches) Begin(m, now, std::uint32_t(std::max<std::int64_t>(0, r.results - r.play) * 1000));
            }
            if (due(steps.results, r.results) && !cancelled) Results();
            if (due(steps.stop, r.stop)) Stop();
        }
        for (auto& [layer, m] : matches) Step(m, now);
    }

    void Dev(std::uint8_t action, std::uint8_t mode) {
        const std::int64_t unix = host.FlagUnixNow();
        const bool running = round && steps.close && !steps.stop;
        switch (action) {
        case 1:
            if (running) return;
            SetRound(RoundFrom(ar::Mode(std::min<int>(mode, 3)), unix, config.devRegister, config.devPlay));
            break;
        case 2: {
            if (running && steps.play) return;
            if (!round || steps.stop) SetRound(RoundFrom(ar::Mode(std::min<int>(mode, 3)), unix, 0, config.devPlay));
            Round r = *round;
            r.close = std::min(r.close, unix);
            r.warn5 = r.warn1 = r.close;
            r.play = unix + 12;
            r.form = unix + 2;
            r.soon = unix + 2;
            r.results = r.play + config.devPlay;
            r.stop = r.results + ar::kResultLeadSeconds;
            round = r;
            steps.warn5 = steps.warn1 = true;
            break;
        }
        case 3:
            if (!running || steps.results || !steps.play) return;
            round->results = unix;
            round->stop = unix + ar::kResultLeadSeconds;
            break;
        case 4:
            if (!round) return;
            cancelled = true;
            round->stop = unix;
            break;
        default: return;
        }
        core::LogInfo("arena: the developer's switch {} (mode {})", action, mode);
    }

    // --- Registration ------------------------------------------------------------------------------------------

    void Register(std::uint32_t entity, const proto::ArenaRegister& r) {
        const auto p = host.FlagPlayer(entity);
        if (!p) return;
        if (r.action == 2) {
            const auto it = registered.find(entity);
            if (it == registered.end()) return Tell(entity, ArenaCode::NotRegistered);
            const auto [character, group] = it->second;
            registered.erase(it);
            host.FlagSubmit(
                [character, group](Database& db) {
                    std::string error;
                    DeleteEventEntry(db, EventKind::Arena, character, error);
                    if (group != 0) db.Query("DELETE FROM sr_event_entries WHERE event = $1 AND groupid = $2", {int(EventKind::Arena), std::int64_t(group)});
                },
                {});
            Tell(entity, ArenaCode::Done, 2);
            core::LogInfo("arena: {} cancels", p->name);
            return;
        }
        const ar::Mode mode = ar::Mode(std::min<int>(r.mode, 4));
        const ArenaHost::Facts facts = arena.ArenaFactsOf(entity);
        game::ArenaEntrant e;
        e.maxLevel = p->entrant.maxLevel;
        e.registered = registered.contains(entity);
        e.open = RegistrationOpen() && round->mode == mode;
        e.partyLeader = facts.partyLeader;
        e.guildMaster = facts.guildMaster;
        e.jobState = facts.jobState;
        e.conditions = p->entrant;
        e.riding = facts.riding;
        const ArenaCode refusal = game::RegistrationRefusal(mode, e);
        if (refusal != ArenaCode::Done) {
            core::LogInfo("arena: {} refused (code 0x{:02X})", p->name, std::uint8_t(refusal));
            return Tell(entity, refusal);
        }
        const std::uint8_t kind = r.kind == ar::kFlag && (round->kinds & ar::kFlagBit) ? ar::kFlag : ar::kScore;
        std::vector<std::uint32_t> who{std::uint32_t(p->character)};
        std::uint32_t group = 0;
        if (mode == ar::Mode::Party) {
            group = facts.party;
            for (const std::uint32_t c : facts.partyMembers)
                if (c != std::uint32_t(p->character)) who.push_back(c);
        } else if (mode == ar::Mode::Guild) {
            if (r.members.empty()) return Tell(entity, ArenaCode::NotRegistered); // FF 01: none chosen
            group = facts.guild;
            who.clear();
            for (std::size_t i = 0; i < r.members.size() && i < std::size_t(ar::kGuildMost); ++i) who.push_back(r.members[i]);
            if (std::find(who.begin(), who.end(), std::uint32_t(p->character)) == who.end()) who.insert(who.begin(), std::uint32_t(p->character));
        } else if (mode == ar::Mode::Job) {
            group = facts.jobState; // the side (thieves against the others)
        }
        std::vector<EventEntryRow> rows;
        const std::int64_t when = host.FlagUnixNow() * 1000 + std::int64_t(random() % 1000);
        for (const std::uint32_t c : who) {
            EventEntryRow row;
            row.event = EventKind::Arena;
            row.character = c;
            row.mode = std::int32_t(mode);
            row.kind = kind;
            row.group = mode == ar::Mode::Job ? std::int64_t(facts.jobState) : std::int64_t(group);
            row.registered = when;
            if (const std::uint32_t there = host.FlagEntityOf(c)) {
                if (const auto q = host.FlagPlayer(there)) {
                    row.name = q->name;
                    row.level = q->entrant.maxLevel;
                    row.server = host.FlagServerId();
                    row.region = q->at.region;
                    row.x = q->at.x;
                    row.y = q->at.y;
                    row.z = q->at.z;
                }
            } else {
                row.name = "#" + std::to_string(c);
                row.level = p->entrant.maxLevel;
            }
            rows.push_back(row);
        }
        registered[entity] = {p->character, group};
        host.FlagSubmit(
            [rows](Database& db) {
                std::string error;
                for (const EventEntryRow& row : rows)
                    if (!SaveEventEntry(db, row, error)) core::LogWarning("arena: a registration: {}", error);
            },
            [this, entity, mode] {
                if (registered.contains(entity)) Tell(entity, ArenaCode::Done, 1, 0, {}, std::uint32_t(mode));
            });
        core::LogInfo("arena: {} registers ({} mode {}, {} kind {})", p->name, rows.size() > 1 ? "with " + std::to_string(rows.size()) : "alone", int(mode),
                      kind == ar::kFlag ? "flag" : "score", kind);
    }

    // --- The matches -------------------------------------------------------------------------------------------

    const ArenaWorldSpec* WorldFor(std::uint8_t kind, std::uint8_t size) const {
        const ArenaWorldSpec* best = nullptr;
        for (const ArenaWorldSpec& w : config.worlds) {
            if (w.kind != kind) continue;
            if (w.size >= size && (best == nullptr || w.size < best->size)) best = &w;
        }
        if (best == nullptr)
            for (const ArenaWorldSpec& w : config.worlds)
                if (w.kind == kind && (best == nullptr || w.size > best->size)) best = &w;
        return best;
    }

    void Form(const Round& r) {
        host.FlagSubmit(
            [this](Database& db) {
                std::string error;
                taken.clear();
                if (!TakeEventEntries(db, EventKind::Arena, taken, error)) core::LogWarning("arena: the queue: {}", error);
            },
            [this, mode = r.mode] { Formed(mode); });
    }
    void Formed(ar::Mode mode) {
        std::vector<EventEntryRow> rows = std::move(taken);
        taken.clear();
        std::unordered_map<std::int64_t, EventEntryRow> byCharacter;
        for (const EventEntryRow& row : rows) byCharacter[row.character] = row;
        for (const std::uint8_t kind : {ar::kScore, ar::kFlag}) {
            std::vector<game::ArenaUnit> units;
            std::map<std::int64_t, std::size_t> unitOfGroup;
            for (const EventEntryRow& row : rows) {
                if (row.kind != kind || row.mode != std::int32_t(mode)) continue;
                const bool together = (mode == ar::Mode::Party || mode == ar::Mode::Guild) && row.group != 0;
                if (together && unitOfGroup.contains(row.group)) {
                    game::ArenaUnit& u = units[unitOfGroup[row.group]];
                    u.characters.push_back(std::uint32_t(row.character));
                    u.level = std::max(u.level, row.level);
                    u.levelSum += row.level;
                    continue;
                }
                game::ArenaUnit u;
                u.characters.push_back(std::uint32_t(row.character));
                u.level = row.level;
                u.leaderLevel = row.level; // the first registered: the party leader's or guild master's row
                u.levelSum = row.level;
                u.order = row.registered;
                u.job = std::uint8_t(row.group);
                if (together) unitOfGroup[row.group] = units.size();
                units.push_back(std::move(u));
            }
            if (units.empty()) continue;
            const game::ArenaPlan plan = game::PlanArena(mode, std::move(units), config.minPlayers, [this] { return int(random() & 1); });
            for (const std::uint32_t c : plan.cancelled) {
                const EventEntryRow& row = byCharacter[c];
                if (row.server == host.FlagServerId()) {
                    if (const std::uint32_t e = host.FlagEntityOf(c)) Tell(e, ArenaCode::TooFew);
                } else {
                    ArenaBus b;
                    b.kind = ArenaBus::Cancelled;
                    b.from = host.FlagServerId();
                    b.server = row.server;
                    b.character = c;
                    Publish(b);
                }
            }
            for (const game::ArenaMatchPlan& mp : plan.matches) {
                const ArenaWorldSpec* spec = WorldFor(kind, mp.size);
                if (spec == nullptr) continue;
                Match* m = Create(*spec, mode, mp.group);
                if (m == nullptr) break;
                for (const auto& [c, team] : mp.players) {
                    const EventEntryRow& row = byCharacter[c];
                    Member member;
                    member.character = c;
                    member.name = row.name;
                    member.team = team;
                    member.level = row.level;
                    member.home.region = row.region;
                    member.home.x = row.x;
                    member.home.y = row.y;
                    member.home.z = row.z;
                    m->members.push_back(member);
                }
                core::LogInfo("arena: a match in {} (layer {}, group {}, {} players)", spec->codename, m->layer, m->group, m->members.size());
                for (const Member& member : std::vector<Member>(m->members)) {
                    if (const std::uint32_t e = host.FlagEntityOf(member.character)) {
                        Admit(e, *m, member.character);
                        continue;
                    }
                    arrivals[member.character] = {m->layer, member.team};
                    ArenaBus b;
                    b.kind = ArenaBus::Assign;
                    b.from = host.FlagServerId();
                    b.server = 0; // whichever holds it
                    b.character = std::uint32_t(member.character);
                    b.layer = m->layer;
                    b.team = member.team;
                    b.to = m->spec->gates[member.team];
                    Publish(b);
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

    Match* Create(const ArenaWorldSpec& spec, ar::Mode mode, int group) {
        const std::uint32_t layer = NextLayer();
        if (layer == 0) return nullptr;
        Match& m = matches[layer];
        m.layer = layer;
        m.spec = &spec;
        m.mode = mode;
        m.group = group;
        if (spec.kind == ar::kFlag) {
            m.flag.Reset();
            for (const ArenaWorldSpec::Spawn& s : spec.spawns) {
                const int pole = PoleIndex(s.codename);
                if (pole < 0 || m.poles[pole] != 0) continue;
                m.poleAt[pole] = s.at;
                m.poleAt[pole].layer = layer;
                m.poles[pole] = world.AddNpc(Model(m.flag.OnPole(pole) ? ar::kFlapCodenames[pole] : ar::kPoleCodenames[pole]), m.poleAt[pole], 0.0f);
            }
        } else if (monsters != nullptr) {
            // The group's monsters (ours: group 0, the lowest levels, takes group 1's).
            const int wanted = std::max(1, group);
            for (const ArenaWorldSpec::Spawn& s : spec.spawns) {
                if (s.group != wanted || s.refObjId == 0) continue;
                MonsterNest n;
                n.refObjId = s.refObjId;
                n.position = s.at;
                n.position.layer = layer;
                n.spawnRadius = float(std::max(10, s.radius));
                n.radius = n.spawnRadius * 2.0f;
                n.maxCount = s.count;
                n.respawn = true;
                n.delayMin = n.delayMax = float(std::max(1, s.respawn));
                n.rarity = s.rarity;
                const std::size_t index = monsters->AddNest(n);
                if (index != SIZE_MAX) m.nestPoints[index] = std::uint32_t(s.points);
            }
        }
        return &m;
    }

    void Close(Match& m) {
        if (monsters != nullptr)
            for (const auto& [nest, points] : m.nestPoints) monsters->RemoveNest(nest);
        for (const std::uint32_t pole : m.poles)
            if (pole != 0) world.Leave(pole);
        if (m.dropped != 0) world.Leave(m.dropped);
    }

    proto::WorldPosition GateOf(const Match& m, std::uint8_t team) {
        proto::WorldPosition at = m.spec->gates[team > 1 ? 0 : team];
        std::uniform_real_distribution<float> d(-ar::kGateRadius, ar::kGateRadius);
        at.x += d(random);
        at.z += d(random);
        at.layer = m.layer;
        return at;
    }

    void Admit(std::uint32_t entity, Match& m, std::int64_t character) {
        Member* member = m.ByCharacter(character);
        if (member == nullptr || m.ended) return;
        member->entity = entity;
        if (const auto p = host.FlagPlayer(entity); p && member->home.region == 0) {
            member->home = p->at;
            member->home.layer = 0;
        }
        inside[entity] = m.layer;
        registered.erase(entity);
        host.FlagTravel(entity, GateOf(m, member->team));
        host.FlagSetTeam(entity, member->team);
        host.FlagUntouchable(entity, std::chrono::milliseconds(ar::kUntouchableMs));
        if (m.playing) {
            proto::ArenaNotice n = Broadcast(ArenaState::Started);
            n.kind = KindOf(m);
            n.value = m.lengthMs;
            Send(entity, n);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - m.playStart).count();
            Tell(entity, ArenaCode::Clock, m.lengthMs, 0, {}, std::uint32_t(std::max<std::int64_t>(0, elapsed)), KindOf(m));
            SendScores(m, entity);
        }
        core::LogInfo("arena: {} joins {} (layer {}) on team {}", member->name, m.spec->codename, m.layer, member->team);
    }

    void SendHome(std::uint32_t entity) {
        Match* m = MatchOf(entity);
        if (m == nullptr) return;
        Member* member = m->ByEntity(entity);
        std::vector<game::FlagMatch::Event> events;
        m->flag.Gone(entity, events);
        Apply(*m, events);
        m->revivals.erase(entity);
        inside.erase(entity);
        host.FlagSetTeam(entity, 0xFF);
        if (const auto p = host.FlagPlayer(entity); p && p->dead) host.FlagRevive(entity, p->at);
        if (member == nullptr) return;
        member->entity = 0;
        if (member->home.region != 0) host.FlagTravel(entity, member->home);
        else arena.ArenaReturnPoint(entity);
    }

    void Begin(Match& m, Clock::time_point now, std::uint32_t lengthMs) {
        m.playing = true;
        m.playStart = now;
        m.lengthMs = lengthMs;
        m.nextClock = now + std::chrono::milliseconds(ar::kClockMs);
        proto::ArenaNotice n = Broadcast(ArenaState::Started);
        n.kind = KindOf(m);
        n.value = lengthMs;
        ForInside(m, [&](Member& x) {
            Send(x.entity, n);
            SendScores(m, x.entity);
        });
    }

    void Totals(Match& m) {
        if (KindOf(m) == ar::kFlag) {
            m.scores[0] = ar::kFlagPoints * (m.flag.OnPole(1) ? 1u : 0u) + m.kills[0];
            m.scores[1] = ar::kFlagPoints * (m.flag.OnPole(2) ? 1u : 0u) + m.kills[1];
        }
    }
    proto::ArenaScores ScoresMessage(Match& m) {
        Totals(m);
        proto::ArenaScores s;
        s.team0 = m.scores[0];
        s.team1 = m.scores[1];
        if (KindOf(m) == ar::kScore) {
            std::vector<const Member*> order;
            for (const Member& x : m.members)
                if (x.points > 0) order.push_back(&x);
            std::stable_sort(order.begin(), order.end(), [](const Member* a, const Member* b) { return a->points > b->points; });
            for (std::size_t i = 0; i < order.size() && i < 5; ++i) s.top.push_back({order[i]->team, order[i]->name, order[i]->points});
        }
        return s;
    }
    void SendScores(Match& m, std::uint32_t only = 0) {
        const proto::ArenaScores s = ScoresMessage(m);
        if (only != 0) return Send(only, s);
        ForInside(m, [&](Member& x) { Send(x.entity, s); });
        m.scoreDirty = false;
        m.nextScore = Clock::now() + std::chrono::milliseconds(ar::kScoreEveryMs);
    }

    void Step(Match& m, Clock::time_point now) {
        if (m.ended) return;
        std::vector<game::FlagMatch::Event> events;
        if (KindOf(m) == ar::kFlag) m.flag.Tick(Ms(now), events);
        Apply(m, events);
        if (m.playing && now >= m.nextClock) {
            m.nextClock = now + std::chrono::milliseconds(ar::kClockMs);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m.playStart).count();
            ForInside(m, [&](Member& x) { Tell(x.entity, ArenaCode::Clock, m.lengthMs, 0, {}, std::uint32_t(elapsed), KindOf(m)); });
        }
        if (m.scoreDirty && now >= m.nextScore) SendScores(m);
        for (auto it = m.revivals.begin(); it != m.revivals.end();) {
            if (now < it->second) {
                ++it;
                continue;
            }
            const std::uint32_t entity = it->first;
            it = m.revivals.erase(it);
            const Member* member = m.ByEntity(entity);
            const auto p = host.FlagPlayer(entity);
            if (member != nullptr && p && p->dead && inside.contains(entity)) host.FlagRevive(entity, GateOf(m, member->team));
        }
    }

    void AddPoints(Match& m, Member& member, std::uint32_t points) {
        if (m.ended || !m.playing) return;
        member.points += points;
        m.scores[member.team] += points;
        Tell(member.entity, ArenaCode::Points, points);
        m.scoreDirty = true;
    }

    void SwapPole(Match& m, int pole) {
        if (m.poles[pole] != 0) world.Leave(m.poles[pole]);
        m.poles[pole] = world.AddNpc(Model(m.flag.OnPole(pole) ? ar::kFlapCodenames[pole] : ar::kPoleCodenames[pole]), m.poleAt[pole], 0.0f);
    }

    void Apply(Match& m, const std::vector<game::FlagMatch::Event>& events) {
        using K = game::FlagMatch::Event::Kind;
        for (const game::FlagMatch::Event& e : events) {
            const Member* actor = m.ByEntity(e.actor);
            const std::string name = actor != nullptr ? actor->name : std::string();
            switch (e.kind) {
            case K::Started: {
                proto::ArenaNotice n;
                n.personal = 1;
                n.code = std::uint8_t(ArenaCode::Action);
                n.value = e.actor;
                host.FlagSendAround(e.actor, proto::ArenaNotice::kId, proto::Encode(n));
                host.FlagHold(e.actor);
                break;
            }
            case K::Carrier:
                if (e.pole >= 0) SwapPole(m, e.pole);
                arena.ArenaBuff(e.actor, true);
                ForInside(m, [&](Member& x) {
                    if (x.team == e.team) Tell(x.entity, ArenaCode::OurCarrier, 0, e.team, name);
                    else Tell(x.entity, ArenaCode::TheirCarrier);
                });
                m.scoreDirty = true;
                break;
            case K::Placed:
                SwapPole(m, e.pole);
                arena.ArenaBuff(e.actor, false);
                ForInside(m, [&](Member& x) { Tell(x.entity, ArenaCode::Placed, 0, e.team, name); });
                m.scoreDirty = true;
                break;
            case K::Dropped: {
                arena.ArenaBuff(e.actor, false);
                const auto where = world.Where(e.actor);
                proto::WorldPosition at = where ? where->position : m.poleAt[0];
                at.layer = m.layer;
                if (m.dropped != 0) world.Leave(m.dropped);
                m.dropped = world.AddNpc(Model(ar::kDroppedFlag), at, 0.0f);
                ForInside(m, [&](Member& x) { Tell(x.entity, ArenaCode::Dropped); });
                break;
            }
            }
        }
    }

    bool Select(std::uint32_t entity, std::uint32_t target) {
        Match* m = MatchOf(entity);
        if (m == nullptr || target == 0 || KindOf(*m) != ar::kFlag) return false;
        int pole = -1;
        for (int i = 0; i < 3; ++i)
            if (m->poles[i] == target) pole = i;
        if (pole < 0 && target != m->dropped) return false;
        Member* member = m->ByEntity(entity);
        const auto p = host.FlagPlayer(entity);
        if (member == nullptr || !p || p->dead || m->ended) return true;
        std::vector<game::FlagMatch::Event> events;
        const std::int64_t now = Ms(Clock::now());
        if (pole >= 0) {
            if (m->flag.Carrier() == entity && m->flag.OnPole(pole)) {
                Tell(entity, ArenaCode::HasFlag);
                return true;
            }
            if (Distance(p->at, m->poleAt[pole]) > game::flag::kReach) {
                host.FlagWalkTo(entity, m->poleAt[pole]);
                return true;
            }
            const auto code = m->flag.SelectPole(pole, entity, member->team, now, events);
            if (code == game::FlagCode::TakeBusy) Tell(entity, ArenaCode::TakeBusy);
            if (code == game::FlagCode::PlaceBusy) Tell(entity, ArenaCode::PlaceBusy);
            Apply(*m, events);
            return true;
        }
        if (m->flag.Carrier() == entity) {
            Tell(entity, ArenaCode::HasFlag);
            return true;
        }
        const auto there = world.Where(m->dropped);
        if (!there || !host.FlagWayClear(p->at, there->position)) {
            Tell(entity, ArenaCode::FlagBlocked);
            return true;
        }
        if (m->flag.PickUp(entity, member->team, now, events)) {
            world.Leave(m->dropped);
            m->dropped = 0;
        }
        Apply(*m, events);
        return true;
    }

    void Results() {
        for (auto& [layer, m] : matches) {
            if (m.ended) continue;
            Totals(m);
            m.ended = true;
            if (const std::uint32_t carrier = m.flag.Carrier()) arena.ArenaBuff(carrier, false);
            const std::optional<std::uint8_t> winner = ar::WinnerOf(m.scores[0], m.scores[1]);
            ForInside(m, [&](Member& x) {
                host.FlagDeed(x.character, "event-matches"); // a match played (the beta quests' degree 8)
                const std::uint8_t outcome = ar::OutcomeOf(x.team, winner);
                const int coins = ar::CoinsFor(m.mode, outcome);
                const auto p = host.FlagPlayer(x.entity);
                const std::uint64_t sp = std::uint64_t(double(arena.ArenaLevelExperience(p ? p->level : x.level)) * double(m.spec->reward[outcome - 1]));
                proto::ArenaResult r;
                r.mode = std::uint8_t(m.mode);
                r.outcome = outcome;
                r.coins = std::uint8_t(coins);
                r.sp = std::uint32_t(std::min<std::uint64_t>(sp, 0xFFFFFFFFu));
                r.team0 = m.scores[0];
                r.team1 = m.scores[1];
                Send(x.entity, r);
                if (!host.FlagGive(x.entity, ar::kCoin, std::uint32_t(coins))) Tell(x.entity, ArenaCode::BagFull); // lost
                if (sp > 0) arena.ArenaSkillExperience(x.entity, sp);
                core::LogInfo("arena: {} {} ({} coins, {} SP experience)", x.name, outcome == 1 ? "wins" : outcome == 2 ? "loses" : "draws", coins, sp);
            });
            core::LogInfo("arena: the match in layer {} ends {} : {}", layer, m.scores[0], m.scores[1]);
        }
    }

    void Stop() {
        Everyone(ArenaState::Stopped);
        registered.clear();
        arrivals.clear();
        for (auto& [layer, m] : matches) {
            std::vector<std::uint32_t> left;
            ForInside(m, [&](Member& x) { left.push_back(x.entity); });
            for (const std::uint32_t e : left) SendHome(e);
            Close(m);
        }
        matches.clear();
    }

    void Publish(const ArenaBus& b) { arena.ArenaPublish(BusToHex(proto::Encode(b))); }
    void OnBus(const ArenaBus& b) {
        if (b.from == host.FlagServerId()) return;
        if (b.kind == ArenaBus::Dev) return Dev(b.action, b.mode);
        if (b.server != 0 && b.server != host.FlagServerId()) return;
        const std::uint32_t entity = host.FlagEntityOf(b.character);
        if (entity == 0) return;
        if (b.kind == ArenaBus::Cancelled) return Tell(entity, ArenaCode::TooFew);
        if (b.kind == ArenaBus::Assign) {
            proto::WorldPosition to = b.to;
            to.layer = 0;
            core::LogInfo("arena: character {} goes to its match on game server {}", b.character, b.from);
            host.FlagTravel(entity, to);
        }
    }
};

Arenas::Arenas(FlagWorldHost& host, ArenaHost& arena, World& world, Monsters* monsters, const assets::CharacterTable* characters, ArenaConfig config,
               std::uint32_t seed)
    : s_(std::make_unique<State>(host, arena, world, monsters, characters, std::move(config), seed)) {}

Arenas::~Arenas() = default;

void Arenas::Enter(std::uint32_t entity) {
    const auto p = s_->host.FlagPlayer(entity);
    if (!p) return;
    const auto it = s_->arrivals.find(p->character);
    if (it == s_->arrivals.end()) return;
    const std::uint32_t layer = it->second.first;
    s_->arrivals.erase(it);
    if (State::Match* m = s_->Of(layer)) s_->Admit(entity, *m, p->character);
}

void Arenas::Leave(std::uint32_t entity) {
    State& s = *s_;
    if (const auto it = s.registered.find(entity); it != s.registered.end()) {
        const auto [character, group] = it->second;
        s.registered.erase(it);
        s.host.FlagSubmit(
            [character](Database& db) {
                std::string error;
                DeleteEventEntry(db, EventKind::Arena, character, error);
            },
            {});
        (void)group;
    }
    State::Match* m = s.MatchOf(entity);
    if (m == nullptr) return;
    std::vector<game::FlagMatch::Event> events;
    m->flag.Gone(entity, events);
    s.Apply(*m, events);
    m->revivals.erase(entity);
    if (State::Member* member = m->ByEntity(entity)) member->entity = 0;
    s.inside.erase(entity);
}

bool Arenas::Handle(std::uint32_t entity, proto::MessageId id, std::span<const std::byte> payload, bool devCommands) {
    switch (id) {
    case proto::ArenaRegister::kId: {
        proto::ArenaRegister r;
        if (!proto::Decode(payload, r)) return false;
        s_->Register(entity, r);
        return true;
    }
    case proto::ArenaDev::kId: {
        proto::ArenaDev d;
        if (!proto::Decode(payload, d)) return false;
        if (devCommands) Dev(d.action, d.mode);
        return true;
    }
    default: return false;
    }
}

void Arenas::Dev(std::uint8_t action, std::uint8_t mode) {
    s_->Dev(action, mode);
    ArenaBus b;
    b.kind = ArenaBus::Dev;
    b.from = s_->host.FlagServerId();
    b.action = action;
    b.mode = mode;
    s_->Publish(b);
}

void Arenas::TellDesk(std::uint32_t entity, std::uint32_t npc, std::string_view codename) {
    if (codename != ar::kManager) return;
    proto::ArenaDesk desk;
    desk.npc = npc;
    if (s_->registered.contains(entity)) desk.state = ar::kRegistered;
    else if (s_->RegistrationOpen()) desk.state = std::uint16_t(std::uint16_t(s_->round->mode) | s_->round->kinds);
    else desk.state = std::uint16_t(ar::Mode::None);
    s_->Send(entity, desk);
}

bool Arenas::Select(std::uint32_t entity, std::uint32_t target) { return s_->Select(entity, target); }

void Arenas::Killed(const Monsters::Damaged& killed, const proto::WorldPosition&) {
    if (killed.nest < 0) return;
    for (auto& [layer, m] : s_->matches) {
        const auto it = m.nestPoints.find(std::size_t(killed.nest));
        if (it == m.nestPoints.end()) continue;
        if (State::Member* killer = m.ByEntity(killed.killer)) s_->AddPoints(m, *killer, it->second);
        return;
    }
}

void Arenas::Died(std::uint32_t victim, std::uint32_t killer) {
    State& s = *s_;
    State::Match* m = s.MatchOf(victim);
    if (m == nullptr) return;
    std::vector<game::FlagMatch::Event> events;
    m->flag.Gone(victim, events);
    s.Apply(*m, events);
    m->revivals[victim] = Clock::now() + std::chrono::milliseconds(ar::kReviveMs);
    s.Tell(victim, ArenaCode::Revival, 10);
    State::Member* dead = m->ByEntity(victim);
    if (dead == nullptr || m->ended || !m->playing) return;
    if (s.KindOf(*m) == ar::kFlag) {
        m->kills[dead->team == 0 ? 1 : 0] += ar::kFlagKillPoints;
        m->scoreDirty = true;
        return;
    }
    if (State::Member* k = s.MatchOf(killer) == m ? m->ByEntity(killer) : nullptr; k != nullptr && k->team != dead->team) {
        s.AddPoints(*m, *k, ar::kPlayerKillPoints);
    }
}

bool Arenas::Resurrect(std::uint32_t entity) { return s_->MatchOf(entity) != nullptr; }
void Arenas::Tick(Clock::time_point now) { s_->Tick(now); }

void Arenas::BusMessage(const std::string& text) {
    const auto bytes = BusFromHex(text);
    ArenaBus b;
    if (bytes && proto::Decode(*bytes, b)) s_->OnBus(b);
}

bool Arenas::Hostile(std::uint32_t a, std::uint32_t b) const {
    const auto ia = s_->inside.find(a), ib = s_->inside.find(b);
    if (ia == s_->inside.end() || ib == s_->inside.end() || ia->second != ib->second) return false;
    State::Match* m = s_->Of(ia->second);
    if (m == nullptr || m->ended) return false;
    const State::Member *ma = m->ByEntity(a), *mb = m->ByEntity(b);
    return ma != nullptr && mb != nullptr && ma->team != mb->team;
}
bool Arenas::InMatch(std::uint32_t entity) const { return s_->inside.contains(entity); }
std::optional<proto::WorldPosition> Arenas::ReturnPlace(std::uint32_t entity) const {
    State::Match* m = s_->MatchOf(entity);
    const State::Member* member = m != nullptr ? m->ByEntity(entity) : nullptr;
    if (member == nullptr || member->home.region == 0) return std::nullopt;
    return member->home;
}
game::arena::Mode Arenas::OpenMode() const { return s_->RegistrationOpen() ? s_->round->mode : ar::Mode::None; }
std::size_t Arenas::Matches() const { return s_->matches.size(); }
std::uint32_t Arenas::LayerOf(std::uint32_t entity) const {
    const auto it = s_->inside.find(entity);
    return it != s_->inside.end() ? it->second : 0u;
}
std::uint8_t Arenas::TeamOf(std::uint32_t entity) const {
    State::Match* m = s_->MatchOf(entity);
    const State::Member* member = m != nullptr ? m->ByEntity(entity) : nullptr;
    return member != nullptr ? member->team : std::uint8_t(0xFF);
}
std::pair<std::uint32_t, std::uint32_t> Arenas::ScoresOf(std::uint32_t layer) const {
    State::Match* m = s_->Of(layer);
    if (m == nullptr) return {0, 0};
    s_->Totals(*m);
    return {m->scores[0], m->scores[1]};
}
std::size_t Arenas::Worlds() const { return s_->config.worlds.size(); }

} // namespace sr::server
