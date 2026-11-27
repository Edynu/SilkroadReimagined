#include "sr/server/Guilds.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <type_traits>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "sr/core/Log.h"
#include "sr/game/GuildMercenary.h"
#include "sr/game/Protocol.h"
#include "sr/server/Database.h"

namespace sr::server {

namespace proto = game::proto;
using game::GuildError;

namespace {

std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The shard's bus: what one game server tells the others, our encoding in
// hexadecimal in a NOTIFY's payload (under its 8,000 bytes).
enum : std::uint8_t { kBusReload = 1, kBusForward = 2, kBusPresence = 3, kBusHello = 4, kBusRequest = 5, kBusPenalty = 6, kBusElection = 7, kBusWars = 8,
                      kBusRecall = 9 };
enum : std::uint8_t { kToGuild = 0, kToCharacter = 2 };
struct BusEnvelope {
    std::uint16_t from = 0;
    std::uint8_t kind = 0;
    std::uint8_t scope = 0;      // forward: to a guild's members, or to one character
    std::uint32_t guild = 0;
    std::int64_t character = 0;  // forward: the one left out (a guild) or told; presence; request; penalty
    std::uint32_t value = 0;     // presence: 1 online; penalty: seconds; election: 1 running; recall: its seconds
    std::vector<std::uint32_t> guilds, alliances; // reload
    std::uint16_t message = 0;   // forward, request: its id
    std::vector<std::uint8_t> payload;
    std::string name;            // request: the asker's name; recall: the master's (its place in `payload`)
    template <typename V>
    void Visit(V& v) {
        v("from", from);
        v("kind", kind);
        v("scope", scope);
        v("guild", guild);
        v("character", character);
        v("value", value);
        v("guilds", guilds, 64);
        v("alliances", alliances, 64);
        v("message", message);
        v("payload", payload, 3800);
        v("name", name, 64);
    }
};

std::string ToHex(std::span<const std::byte> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::byte b : bytes) {
        out += kDigits[std::to_integer<unsigned>(b) >> 4];
        out += kDigits[std::to_integer<unsigned>(b) & 15];
    }
    return out;
}

std::optional<std::vector<std::byte>> FromHex(std::string_view text) {
    if (text.size() % 2 != 0) return std::nullopt;
    std::vector<std::byte> out(text.size() / 2);
    const auto digit = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = digit(text[2 * i]), lo = digit(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = std::byte(hi * 16 + lo);
    }
    return out;
}

} // namespace

struct Guilds::State {
    GuildHost& host;
    explicit State(GuildHost& h) : host(h) {}

    struct Member {
        std::int64_t character = 0;
        std::string name;
        std::uint8_t memberClass = game::kGuildMember;
        std::uint8_t level = 1;
        std::uint32_t donation = 0;
        std::uint32_t permission = 0;
        std::string nickname;
        std::uint32_t model = 0;
        std::uint8_t position = game::kPositionNone; // in the fortress war
        std::uint32_t contribution = 0;              // points won in guild wars
        std::chrono::system_clock::time_point lastSeen; // its last logout (online: now)
    };
    struct Guild {
        std::uint32_t id = 0;
        std::string name;
        int level = 1;
        std::uint32_t points = 0;
        std::string title, notice;
        std::uint32_t alliance = 0;
        std::uint32_t lastCrest = 0, crest = 0; // the emblem's revisions (crest 0: none)
        std::uint32_t booty = 0;                // war rewards to collect
        std::uint8_t mercenaryAttr = 0;         // the mercenaries' abilities bought (game::kMerc* bits)
        std::vector<Member> members;
        Member* Find(std::int64_t character) {
            for (Member& m : members) {
                if (m.character == character) return &m;
            }
            return nullptr;
        }
        const Member* Master() const {
            for (const Member& m : members) {
                if (m.memberClass == game::kGuildMaster) return &m;
            }
            return nullptr;
        }
    };
    struct Online {
        std::uint64_t peer = 0;
        std::uint32_t entity = 0;
    };
    // A proposal waiting for its answer, by the invited character.
    struct Proposal {
        std::uint8_t kind = 0;
        std::int64_t from = 0;
        std::uint32_t guild = 0; // the inviter's
        std::uint32_t targetGuild = 0;
        Clock::time_point expires;
        proto::GuildWarTerms terms; // a war's
    };
    // Wars by id (_GuildWar): the challenger first; its end (none: unlimited); a surrender running.
    struct War {
        std::uint32_t id = 0;
        std::uint32_t guilds[2]{};
        std::uint32_t points[2]{};
        proto::GuildWarTerms terms;
        std::uint32_t lodged = 0;
        std::optional<std::chrono::system_clock::time_point> ends;
        std::optional<Clock::time_point> surrender;
        std::uint32_t surrendering = 0;
        int Side(std::uint32_t guild) const { return guilds[0] == guild ? 0 : guilds[1] == guild ? 1 : -1; }
    };
    std::map<std::uint32_t, War> wars;
    std::set<std::uint32_t> warsStarting; // guild pairs being written (the lower id << 32 | the higher, folded)
    const War* WarBetween(std::uint32_t a, std::uint32_t b) const {
        for (const auto& [id, w] : wars) {
            if ((w.guilds[0] == a && w.guilds[1] == b) || (w.guilds[0] == b && w.guilds[1] == a)) return &w;
        }
        return nullptr;
    }
    int Hostiles(std::uint32_t guild) const {
        return int(std::count_if(wars.begin(), wars.end(), [&](const auto& w) { return w.second.Side(guild) >= 0; }));
    }

    std::unordered_map<std::uint32_t, Guild> guilds;
    std::unordered_map<std::int64_t, std::uint32_t> memberOf;
    std::unordered_map<std::string, std::uint32_t> names; // lower case -> guild (0: being created)
    std::unordered_map<std::uint32_t, AllianceRow> alliances;
    std::unordered_map<std::int64_t, Online> online;
    std::unordered_map<std::int64_t, std::chrono::system_clock::time_point> penalties;
    std::unordered_map<std::int64_t, Proposal> proposals;
    std::unordered_set<std::int64_t> creating;
    // The shard's other game servers: who of theirs is online, the elections they hold, and each
    // guild's local changes (a reload loaded before a change of ours is loaded again).
    std::unordered_set<std::int64_t> remoteOnline;
    std::unordered_map<std::uint32_t, std::uint16_t> remoteElections; // guild -> the server holding it
    std::unordered_map<std::uint32_t, std::uint64_t> changes;
    struct Touched {
        std::set<std::uint32_t> guilds, alliances;
    } touched;
    std::int64_t answerCharacter = 0; // a request passed on from another server: its answers go back there
    // Elections by guild (in memory: one lost with a restart is called again).
    struct Election {
        Clock::time_point ends;
        std::vector<std::int64_t> candidates;          // in the members' order
        std::unordered_set<std::int64_t> voters;
        std::unordered_map<std::int64_t, std::int64_t> ballots; // voter -> candidate
    };
    std::unordered_map<std::uint32_t, Election> elections;
    int electionSeconds = game::kElectionSeconds;
    int warEndSeconds = game::kGuildWarEndSeconds;
    // The emblems' pixels by kind (proto::kCrestGuild / kCrestUnion) and id: the current ones.
    std::map<std::pair<std::uint8_t, std::uint32_t>, std::vector<std::uint8_t>> crests;

    // Writes, one at a time in the order made (the next goes when the last is done).
    struct PendingWrite {
        std::function<void(Database&)> task;
        std::function<void()> done;
        std::function<void(std::function<void()>)> start; // a payment's (WritePaid): run at its turn, `next` once it is done
    };
    std::deque<PendingWrite> writes;
    bool writing = false;
    std::uint64_t mutations = 0; // writes of guild state asked so far
    void Write(std::function<void(Database&)> task, std::function<void()> done = {}) {
        ++mutations;
        WriteQuiet(std::move(task), std::move(done));
    }
    // In the same order, but not a change of a guild (the bus's messages, loads).
    void WriteQuiet(std::function<void(Database&)> task, std::function<void()> done = {}) {
        writes.push_back({std::move(task), std::move(done), {}});
        if (!writing) NextWrite();
    }
    // A payment's work on the guilds' rows (Payment.h), in the guilds' order of writes and its payer's order of saves
    // both: at its turn here it is handed to the payer's (GuildHost::GuildPay), the next write after it.
    void WritePaid(std::int64_t character, std::uint64_t fee, PaymentWork work, PaymentDone done) {
        ++mutations;
        writes.push_back({{}, {}, [this, character, fee, work = std::move(work), done = std::move(done)](std::function<void()> next) {
                              host.GuildPay(character, fee, work, [done, next](bool ok, std::uint64_t credit) {
                                  if (done) done(ok, credit);
                                  next();
                              });
                          }});
        if (!writing) NextWrite();
    }
    void NextWrite() {
        if (writes.empty()) {
            writing = false;
            return;
        }
        writing = true;
        PendingWrite w = std::move(writes.front());
        writes.pop_front();
        if (w.start) return w.start([this] { NextWrite(); });
        host.GuildSubmit(std::move(w.task), [this, done = std::move(w.done)] {
            if (done) done();
            NextWrite();
        });
    }

    template <typename T>
    void Send(std::uint64_t peer, const T& message) {
        if (peer != 0) {
            host.GuildSend(peer, T::kId, proto::Encode(message));
        } else if (answerCharacter != 0) {
            SendTo(answerCharacter, message); // a request from another server: back there
        }
    }
    template <typename T>
    void SendTo(std::int64_t character, const T& message) {
        if (const auto it = online.find(character); it != online.end()) {
            host.GuildSend(it->second.peer, T::kId, proto::Encode(message));
        } else if (remoteOnline.contains(character)) {
            BusEnvelope e;
            e.kind = kBusForward;
            e.scope = kToCharacter;
            e.character = character;
            Forward(e, T::kId, proto::Encode(message));
        }
    }

    // --- The shard's bus --------------------------------------------------------------------------

    bool OnBus() const { return !host.GuildChannel().empty(); }
    // Published in the order of the writes (after the ones before it are in the database).
    void Publish(BusEnvelope e) {
        if (!OnBus()) return;
        e.from = host.GuildServer();
        std::string text = ToHex(proto::Encode(e));
        if (text.size() >= 7990) {
            core::LogWarning("guilds: a bus message of {} bytes not sent (kind {}, message 0x{:04x})", text.size(), int(e.kind), e.message);
            return;
        }
        WriteQuiet([channel = host.GuildChannel(), text = std::move(text)](Database& db) { Notify(db, channel, text); });
    }
    void Forward(BusEnvelope e, proto::MessageId id, const std::vector<std::byte>& bytes) {
        if (!OnBus() || bytes.size() > 3800) return; // the big ones (a guild's whole data) come with its reload
        e.message = id;
        e.payload.resize(bytes.size());
        std::memcpy(e.payload.data(), bytes.data(), bytes.size());
        Publish(std::move(e));
    }
    // A guild changed here (its union and the union's guilds with it): the others load them again once
    // this handler's writes are in.
    void Touch(std::uint32_t guild) {
        const auto g = guilds.find(guild);
        if (guild == 0) return;
        touched.guilds.insert(guild);
        ++changes[guild];
        if (g == guilds.end() || g->second.alliance == 0) return;
        touched.alliances.insert(g->second.alliance);
        if (const auto a = alliances.find(g->second.alliance); a != alliances.end()) {
            for (std::uint32_t id : a->second.guilds) {
                if (id != 0) {
                    touched.guilds.insert(id);
                    ++changes[id];
                }
            }
        }
    }
    // A guild, its union and the union's guilds, as they are now (nothing counted).
    Touched Around(std::uint32_t guild) const {
        Touched out;
        const auto g = guilds.find(guild);
        if (guild == 0 || g == guilds.end()) return out;
        out.guilds.insert(guild);
        if (g->second.alliance == 0) return out;
        out.alliances.insert(g->second.alliance);
        if (const auto a = alliances.find(g->second.alliance); a != alliances.end()) {
            for (std::uint32_t id : a->second.guilds) {
                if (id != 0) out.guilds.insert(id);
            }
        }
        return out;
    }
    void Mark(const Touched& t) {
        for (std::uint32_t id : t.guilds) {
            touched.guilds.insert(id);
            ++changes[id];
        }
        touched.alliances.insert(t.alliances.begin(), t.alliances.end());
    }
    void PublishTouched() {
        if (touched.guilds.empty() && touched.alliances.empty()) return;
        BusEnvelope e;
        e.kind = kBusReload;
        e.guilds.assign(touched.guilds.begin(), touched.guilds.end());
        e.alliances.assign(touched.alliances.begin(), touched.alliances.end());
        touched = {};
        Publish(std::move(e));
    }
    void PublishPresence(std::int64_t character, bool on) {
        BusEnvelope e;
        e.kind = kBusPresence;
        e.character = character;
        e.value = on ? 1 : 0;
        Publish(std::move(e));
    }
    void Answer(std::uint64_t peer, proto::GuildAction action, GuildError error, std::uint32_t value = 0) {
        Send(peer, proto::GuildAnswer{action, std::uint16_t(error), value});
    }

    // --- The guild recall ---------------------------------------------

    // Asked of a member: where its master stood, until when a yes is taken.
    struct RecallAsk {
        std::uint32_t guild = 0;
        proto::WorldPosition to;
        Clock::time_point until;
        std::uint32_t seconds = 0; // the ask's time, the yes's wait on the gauge
    };
    std::unordered_map<std::int64_t, RecallAsk> recallAsks;
    // The guild's members online here, but the master and those travelling (the original's 0x7030 at each member's game
    // server,not asked under a quest's block or a teleport under way), asked.
    void AskRecall(const Guild& g, std::int64_t master, const std::string& masterName, const proto::WorldPosition& to, std::uint32_t seconds) {
        const Clock::time_point until = Clock::now() + std::chrono::seconds(seconds);
        for (const Member& m : g.members) {
            if (m.character == master) continue;
            const auto it = online.find(m.character);
            if (it == online.end() || !host.GuildRecallAskable(it->second.entity)) continue;
            recallAsks[m.character] = {g.id, to, until, seconds};
            host.GuildSend(it->second.peer, proto::GuildRecallAsk::kId, proto::Encode(proto::GuildRecallAsk{masterName, seconds}));
            core::LogInfo("guilds: {} asked to come to its master {}", m.name, masterName);
        }
    }
    // A member's answer (the original's 0x7031). Ours takes a yes only to the
    // place asked, within its time, from a member of that guild still (the original took any 0x7031 with the place its
    // client sent back); every refusal is the original's 0x18C1.
    void RecallAnswer(const GuildActor& a, const proto::GuildRecallAnswer& m) {
        const auto it = recallAsks.find(a.character);
        const std::optional<RecallAsk> ask = it != recallAsks.end() ? std::optional<RecallAsk>(it->second) : std::nullopt;
        if (it != recallAsks.end()) recallAsks.erase(it);
        if (m.accept == 0) return;
        const Guild* g = GuildOfCharacter(a.character);
        if (!ask || Clock::now() >= ask->until || g == nullptr || g->id != ask->guild) {
            return Answer(a.peer, proto::GuildAction::RecallAnswer, GuildError(0x18C1));
        }
        const std::uint16_t code = host.GuildRecallGo(a.entity, ask->to, ask->seconds);
        Answer(a.peer, proto::GuildAction::RecallAnswer, GuildError(code));
        if (code == 0) core::LogInfo("guilds: {} answers its master's recall: on its gauge, then on its way", a.name);
    }
    // Every member online (but `except`), here and on the shard's other servers.
    template <typename T>
    void ToGuild(const Guild& g, const T& message, std::int64_t except = 0) {
        const std::vector<std::byte> bytes = proto::Encode(message);
        ToGuildHere(g, T::kId, bytes, except);
        BusEnvelope e;
        e.kind = kBusForward;
        e.scope = kToGuild;
        e.guild = g.id;
        e.character = except;
        Forward(std::move(e), T::kId, bytes);
    }
    void ToGuildHere(const Guild& g, proto::MessageId id, const std::vector<std::byte>& bytes, std::int64_t except) {
        for (const Member& m : g.members) {
            if (m.character == except) continue;
            if (const auto it = online.find(m.character); it != online.end()) host.GuildSend(it->second.peer, id, bytes);
        }
    }
    template <typename T>
    void ToUnion(const AllianceRow& a, const T& message, std::int64_t except = 0) {
        for (std::uint32_t id : a.guilds) {
            if (const auto it = guilds.find(id); id != 0 && it != guilds.end()) ToGuild(it->second, message, except);
        }
    }

    Guild* GuildOfCharacter(std::int64_t character) {
        const auto it = memberOf.find(character);
        if (it == memberOf.end()) return nullptr;
        const auto g = guilds.find(it->second);
        return g != guilds.end() ? &g->second : nullptr;
    }
    static bool May(const Member& m, std::uint32_t permission) {
        return m.memberClass == game::kGuildMaster || (m.permission & permission) != 0;
    }
    std::optional<std::uint32_t> PenaltyLeft(std::int64_t character) {
        const auto it = penalties.find(character);
        if (it == penalties.end()) return std::nullopt;
        const auto left = std::chrono::ceil<std::chrono::seconds>(it->second - std::chrono::system_clock::now()).count();
        if (left <= 0) {
            penalties.erase(it);
            return std::nullopt;
        }
        return std::uint32_t(left);
    }
    void Penalise(std::int64_t character) {
        penalties[character] = std::chrono::system_clock::now() + std::chrono::seconds(game::kGuildPenaltySeconds);
        Write([character](Database& db) { AddGuildPenalty(db, character, game::kGuildPenaltySeconds); });
        BusEnvelope e;
        e.kind = kBusPenalty;
        e.character = character;
        e.value = game::kGuildPenaltySeconds;
        Publish(std::move(e));
    }

    proto::GuildMemberRecord Record(const Member& m) const {
        proto::GuildMemberRecord r;
        r.character = std::uint32_t(m.character);
        r.name = m.name;
        r.memberClass = m.memberClass;
        r.level = m.level;
        r.donation = m.donation;
        r.permission = m.permission;
        r.nickname = m.nickname;
        r.model = m.model;
        r.online = online.contains(m.character) || remoteOnline.contains(m.character) ? 1 : 0;
        r.position = m.position;
        r.contribution = m.contribution;
        return r;
    }
    proto::GuildInfo Info(const Guild& g) const {
        proto::GuildInfo info;
        info.id = g.id;
        info.name = g.name;
        info.level = std::uint8_t(g.level);
        info.points = g.points;
        info.noticeTitle = g.title;
        info.notice = g.notice;
        info.unionId = g.alliance;
        for (const Member& m : g.members) info.members.push_back(Record(m));
        info.crest = g.crest;
        info.mercenaryAttr = g.mercenaryAttr;
        info.mercenaries = host.GuildMercenaries();
        return info;
    }
    proto::UnionInfo UnionOf(std::uint32_t alliance) const {
        proto::UnionInfo u;
        const auto it = alliances.find(alliance);
        if (alliance == 0 || it == alliances.end()) return u;
        u.id = alliance;
        u.crest = it->second.crest;
        for (std::uint32_t id : it->second.guilds) {
            const auto g = guilds.find(id);
            if (id == 0 || g == guilds.end()) continue;
            proto::UnionGuildRecord r;
            r.id = id;
            r.name = g->second.name;
            r.level = std::uint8_t(g->second.level);
            if (const Member* master = g->second.Master()) {
                r.master = master->name;
                r.masterModel = master->model;
            }
            r.members = std::uint16_t(g->second.members.size());
            u.guilds.push_back(std::move(r));
        }
        return u;
    }

    // A member's guild as those around it see it, now and for newcomers.
    void TellAppearance(std::int64_t character) {
        const auto o = online.find(character);
        if (o == online.end()) return;
        proto::EntityGuild tag;
        tag.entity = o->second.entity;
        if (Guild* g = GuildOfCharacter(character)) {
            tag.guild = g->id;
            tag.name = g->name;
            tag.unionId = g->alliance;
            tag.crest = g->crest;
            if (const auto u = alliances.find(g->alliance); g->alliance != 0 && u != alliances.end()) tag.unionCrest = u->second.crest;
            if (const Member* m = g->Find(character)) {
                tag.nickname = m->nickname;
                tag.fortressRole = m->position;
            }
        }
        const std::vector<std::byte> bytes = proto::Encode(tag);
        host.GuildToObservers(tag.entity, proto::EntityGuild::kId, bytes);
        host.GuildAppearance(tag.entity, proto::EntityGuild::kId, tag.guild != 0 ? bytes : std::vector<std::byte>{});
    }
    void TellGuildAppearance(const Guild& g) {
        for (const Member& m : g.members) TellAppearance(m.character);
    }
    void TellUnion(std::uint32_t alliance) {
        const auto it = alliances.find(alliance);
        if (it == alliances.end()) return;
        const proto::UnionInfo info = UnionOf(alliance);
        ToUnion(it->second, info);
    }

    // --- Guilds ------------------------------------------------------------------------------------

    void Create(const GuildActor& a, const proto::GuildCreate& r) {
        const auto refuse = [&](GuildError e, std::uint32_t value = 0) { Answer(a.peer, proto::GuildAction::Create, e, value); };
        if (a.dead) return refuse(GuildError::Dead);
        if (a.level < game::kGuildCreateLevel) return refuse(GuildError::CreatorLevel);
        if (a.gold < game::kGuildCreateGold) return refuse(GuildError::NoGold);
        if (a.jobSuit) return refuse(GuildError::CreateInSuit); // after the gold
        if (const auto left = PenaltyLeft(a.character)) return refuse(GuildError::CreatePenalty, *left);
        if (memberOf.contains(a.character) || creating.contains(a.character)) return refuse(GuildError::InAnotherGuild);
        const double reach = host.GuildDistance(a.entity, r.npc);
        if (!host.IsGuildNpc(r.npc) || reach < 0 || reach > game::kGuildNpcReach) return refuse(GuildError::Failed);
        if (!game::GuildNameLengthOk(r.name)) return refuse(GuildError::NameLength);
        // Ours: the name rules' letters too (the original's client refuses such a name with this code;
        // its server only filters words).
        if (!host.GuildNameAllowed(r.name)) return refuse(GuildError::NameNotAllowed);
        const std::string key = Lower(r.name);
        if (names.contains(key)) return refuse(GuildError::NameTaken);
        Found(a, r.name, true);
    }

    // A developer's guild (the item exam's fortress positions): its master `a`, none of the creation's checks but the
    // name taken and another guild, no gold.
    void DevCreate(const GuildActor& a, const std::string& name, int level) {
        if (names.contains(Lower(name)) || memberOf.contains(a.character) || creating.contains(a.character)) {
            return Answer(a.peer, proto::GuildAction::Create, GuildError::NameTaken);
        }
        Found(a, name, false, std::clamp(level, 1, 5));
    }

    // The guild made (_Guild, its master's _GuildMember) and stood: its master the Commander (position 1).
    void Found(const GuildActor& a, const std::string& guildName, bool takeGold, int level = 1) {
        const proto::GuildCreate r{0, guildName};
        const std::string key = Lower(r.name);
        names[key] = 0;
        creating.insert(a.character);
        auto made = std::make_shared<std::pair<std::uint32_t, bool>>(0, false);
        auto ran = std::make_shared<bool>(false);
        const std::int64_t character = a.character;
        const std::uint64_t peer = a.peer;
        auto work = [made, ran, name = r.name, character, level](Database& db) {
            *ran = true;
            std::string error;
            made->first = CreateGuild(db, name, character, made->second, error);
            if (made->first == 0 && !made->second) core::LogWarning("guilds: creating '{}' failed: {}", name, error);
            if (made->first != 0 && level > 1) db.Query("UPDATE _guild SET lvl = $2 WHERE id = $1", {std::int64_t(made->first), level}); // a developer's
        };
        auto finish = [this, made, ran, name = r.name, key, character, peer, actor = a, level] {
            creating.erase(character);
            if (made->first == 0) {
                names.erase(key);
                return Answer(peer, proto::GuildAction::Create,
                              !*ran ? GuildError::NoGold : made->second ? GuildError::NameTaken : GuildError::CannotCreate);
            }
            Guild g;
            g.id = made->first;
            g.name = name;
            g.level = std::uint8_t(level);
            Member master;
            master.character = character;
            master.name = actor.name;
            master.memberClass = game::kGuildMaster;
            master.position = game::kPositionCommander;
            master.level = std::uint8_t(actor.level);
            master.permission = game::kGuildPermAll;
            master.model = actor.model;
            g.members.push_back(master);
            names[key] = g.id;
            memberOf[character] = g.id;
            const Guild& stored = guilds[g.id] = std::move(g);
            core::LogInfo("guilds: {} created guild {} '{}'", actor.name, stored.id, stored.name);
            SendTo(character, Info(stored));
            Answer(peer, proto::GuildAction::Create, GuildError::Ok);
            TellAppearance(character);
            // The shard's other servers: the new guild, its master online (it stands only now).
            if (online.contains(character)) PublishPresence(character, true);
            Touch(stored.id);
            PublishTouched();
        };
        if (!takeGold) return Write(std::move(work), std::move(finish)); // a developer's: no fee
        // The guild's rows and the 500,000 in one transaction (the original charges once _Guild_Create has stood).
        WritePaid(character, game::kGuildCreateGold,
                  [work, made](Database& tx) -> std::optional<std::uint64_t> {
                      work(tx);
                      return made->first != 0 ? std::optional<std::uint64_t>(0) : std::nullopt;
                  },
                  [finish, made](bool ok, std::uint64_t) {
                      if (!ok) made->first = 0; // not stood (the gold's write failed with it)
                      finish();
                  });
    }

    void Invite(const GuildActor& a, const proto::GuildInvite& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::Invite, e); };
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return; // the original answers nothing
        const Member* me = g->Find(a.character);
        if (me == nullptr || !May(*me, game::kGuildPermInvite)) return refuse(GuildError::Denied);
        if (r.target == a.entity) return;
        if (int(g->members.size()) >= game::GuildMemberCap(g->level)) return refuse(GuildError::Full);
        if (host.GuildInFortressWar(g->id)) return refuse(GuildError::WarNoInvite);
        const std::optional<GuildActor> t = host.GuildPlayerAt(r.target);
        const double reach = host.GuildDistance(a.entity, r.target);
        if (!t || reach < 0 || reach > game::kGuildInviteReach) return refuse(GuildError::Failed);
        if (proposals.contains(t->character)) return refuse(GuildError::TargetBusy);
        if (t->dead) return refuse(GuildError::Failed);
        if (PenaltyLeft(t->character)) return refuse(GuildError::JoinPenalty);
        if (const auto it = memberOf.find(t->character); it != memberOf.end()) {
            return refuse(it->second == g->id ? GuildError::ExistingMember : GuildError::InAnotherGuild);
        }
        if (creating.contains(t->character)) return refuse(GuildError::InAnotherGuild);
        if (t->jobSuit) return refuse(GuildError::JoinInSuit); // the invitee's suit, last
        proposals[t->character] = {proto::kProposalGuild, a.character, g->id, 0, Clock::now() + std::chrono::seconds(game::kProposalSeconds)};
        Send(t->peer, proto::GuildProposal{proto::kProposalGuild, a.entity, a.name, g->name});
    }

    void AnswerProposal(const GuildActor& a, const proto::GuildProposalAnswer& r) {
        const auto it = proposals.find(a.character);
        if (it == proposals.end() || it->second.kind != r.kind) return;
        const Proposal p = it->second;
        proposals.erase(it);
        if (p.kind == proto::kProposalWar) {
            const auto from = online.find(p.from);
            if (from == online.end()) return Answer(a.peer, proto::GuildAction::WarAnswer, GuildError::InviterGone);
            return AnswerWar(a, p, from->second.peer, r.accept != 0);
        }
        const bool guild = p.kind == proto::kProposalGuild;
        const proto::GuildAction asked = guild ? proto::GuildAction::Invite : proto::GuildAction::UnionInvite;
        const proto::GuildAction answered = guild ? proto::GuildAction::Join : proto::GuildAction::UnionJoin;
        const auto inviter = online.find(p.from);
        const std::uint64_t inviterPeer = inviter != online.end() ? inviter->second.peer : 0;
        if (inviterPeer == 0) return Answer(a.peer, answered, GuildError::InviterGone);
        if (r.accept == 0) {
            const GuildError e = guild ? GuildError::JoinRefused : GuildError::UnionRefused;
            Answer(inviterPeer, asked, e);
            return Answer(a.peer, answered, e);
        }
        if (guild) return Join(a, p, inviterPeer);
        JoinUnion(a, p, inviterPeer);
    }

    void Join(const GuildActor& a, const Proposal& p, std::uint64_t inviterPeer) {
        // Ours: checked again (the original's game server sends the join on; its database refuses a
        // character already in a guild).
        const auto g = guilds.find(p.guild);
        GuildError e = GuildError::Ok;
        if (g == guilds.end()) e = GuildError::Failed;
        else if (memberOf.contains(a.character)) e = GuildError::InAnotherGuild;
        else if (int(g->second.members.size()) >= game::GuildMemberCap(g->second.level)) e = GuildError::Full;
        if (e != GuildError::Ok) {
            Answer(inviterPeer, proto::GuildAction::Invite, e);
            return Answer(a.peer, proto::GuildAction::Join, e);
        }
        Guild& guild = g->second;
        Member m;
        m.character = a.character;
        m.name = a.name;
        m.level = std::uint8_t(a.level);
        m.model = a.model;
        guild.members.push_back(m);
        memberOf[a.character] = guild.id;
        Write([id = guild.id, character = a.character](Database& db) { AddGuildMember(db, id, character, game::kGuildMember, 0); });
        core::LogInfo("guilds: {} joined guild {} '{}'", a.name, guild.id, guild.name);
        ToGuild(guild, proto::GuildMemberJoined{Record(m)}, a.character);
        Send(a.peer, Info(guild));
        if (guild.alliance != 0) {
            Send(a.peer, UnionOf(guild.alliance));
            TellUnion(guild.alliance); // the member counts
        }
        Answer(inviterPeer, proto::GuildAction::Invite, GuildError::Ok);
        Answer(a.peer, proto::GuildAction::Join, GuildError::Ok);
        TellAppearance(a.character);
    }

    // A member out of its guild (1 left, 2 put out): its penalty, everyone told.
    void Remove(Guild& g, std::int64_t character, std::uint8_t reason) {
        const auto it = std::find_if(g.members.begin(), g.members.end(), [&](const Member& m) { return m.character == character; });
        if (it == g.members.end()) return;
        const std::string name = it->name;
        ToGuild(g, proto::GuildMemberRemoved{std::uint32_t(character), reason, name});
        g.members.erase(it);
        memberOf.erase(character);
        Write([id = g.id, character](Database& db) { RemoveGuildMember(db, id, character); });
        Penalise(character);
        SendTo(character, proto::GuildInfo{});
        TellAppearance(character);
        if (g.alliance != 0) TellUnion(g.alliance);
        core::LogInfo("guilds: {} {} guild {} '{}'", name, reason == 2 ? "was put out of" : "left", g.id, g.name);
    }

    void Leave(const GuildActor& a, const proto::GuildLeave& r) {
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return;
        // Any NPC within its reach; the master cannot leave (ignored).
        const double reach = host.GuildDistance(a.entity, r.npc);
        if (!host.IsNpc(r.npc) || reach < 0 || reach > game::kGuildNpcReach) return Answer(a.peer, proto::GuildAction::Leave, GuildError::Failed);
        const Member* me = g->Find(a.character);
        if (me == nullptr || me->memberClass == game::kGuildMaster) return;
        if (host.GuildInFortressWar(g->id)) return Answer(a.peer, proto::GuildAction::Leave, GuildError::WarNoLeave);
        if (host.GuildStorageHolder(g->id) == a.character) return Answer(a.peer, proto::GuildAction::Leave, GuildError::StorageInUse);
        if (InElection(*g, a.character)) return Answer(a.peer, proto::GuildAction::Leave, GuildError::ElectionNoLeave);
        Remove(*g, a.character, 1);
        Answer(a.peer, proto::GuildAction::Leave, GuildError::Ok);
    }

    void Kick(const GuildActor& a, const proto::GuildKick& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::Kick, e); };
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return;
        const Member* me = g->Find(a.character);
        if (me == nullptr || !May(*me, game::kGuildPermKick)) return refuse(GuildError::Denied);
        if (host.GuildInFortressWar(g->id)) return refuse(GuildError::WarNoKick);
        if (r.name.size() < 2) return;
        const std::string key = Lower(r.name);
        const auto target = std::find_if(g->members.begin(), g->members.end(), [&](const Member& m) { return Lower(m.name) == key; });
        if (target == g->members.end()) return; // not found: no answer
        if (target->character == a.character) return refuse(GuildError::KickSelf);
        if (target->memberClass == game::kGuildMaster) return refuse(GuildError::MasterNotExpellable);
        if (host.GuildStorageHolder(g->id) == target->character) return refuse(GuildError::StorageInUse);
        if (InElection(*g, target->character)) return refuse(GuildError::ElectionNoKick);
        Remove(*g, target->character, 2);
        Answer(a.peer, proto::GuildAction::Kick, GuildError::Ok);
    }

    void Disband(const GuildActor& a, const proto::GuildDisband& r) {
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return;
        const Member* me = g->Find(a.character);
        // Not the master: refused on the kick's answer in the original (B0F4); ours on the disband's.
        if (me == nullptr || me->memberClass != game::kGuildMaster) return Answer(a.peer, proto::GuildAction::Disband, GuildError::Denied);
        const double reach = host.GuildDistance(a.entity, r.npc);
        if (!host.IsGuildNpc(r.npc) || reach < 0 || reach > game::kGuildNpcReach) return Answer(a.peer, proto::GuildAction::Disband, GuildError::Failed);
        if (Hostiles(g->id) > 0) return Answer(a.peer, proto::GuildAction::Disband, GuildError::DisbandWar);
        if (host.GuildHoldsFortress(g->id)) return Answer(a.peer, proto::GuildAction::Disband, GuildError::DisbandFortress);
        if (host.GuildInFortressWar(g->id)) return Answer(a.peer, proto::GuildAction::Disband, GuildError::DisbandSiege);
        if (host.GuildStorageHolder(g->id) != 0) return Answer(a.peer, proto::GuildAction::Disband, GuildError::StorageInUse);
        // Ours: a guild in a union leaves it first (the original's shard manager's way is not known).
        if (g->alliance != 0) LeaveUnion(*g, 2);
        const std::uint32_t id = g->id;
        const std::string name = g->name;
        ToGuild(*g, proto::GuildDisbanded{name});
        std::vector<std::int64_t> members;
        for (const Member& m : g->members) {
            members.push_back(m.character);
            memberOf.erase(m.character);
        }
        names.erase(Lower(name));
        guilds.erase(id);
        elections.erase(id);
        crests.erase({proto::kCrestGuild, id});
        Write([id](Database& db) { DisbandGuild(db, id); });
        // Only the master who disbanded it gets the penalty.
        Penalise(a.character);
        for (std::int64_t c : members) TellAppearance(c);
        for (auto it = proposals.begin(); it != proposals.end();) it = it->second.guild == id ? proposals.erase(it) : std::next(it);
        Answer(a.peer, proto::GuildAction::Disband, GuildError::Ok);
        core::LogInfo("guilds: {} disbanded guild {} '{}'", a.name, id, name);
    }

    void Notice(const GuildActor& a, const proto::GuildNoticeEdit& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::Notice, e); };
        if (a.dead) return refuse(GuildError::Dead);
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return refuse(GuildError::NoGuild);
        const Member* me = g->Find(a.character);
        if (me == nullptr || !May(*me, game::kGuildPermNotice)) return refuse(GuildError::Denied);
        const std::size_t title = game::Utf8Length(r.title), body = game::Utf8Length(r.body);
        if (title < 1 || title > proto::kGuildNoticeTitle) return refuse(GuildError::NoticeTitle);
        if (body < 1 || body > proto::kGuildNoticeBody) return refuse(GuildError::NoticeBody);
        g->title = r.title;
        g->notice = r.body;
        Write([id = g->id, t = r.title, b = r.body](Database& db) { SaveGuildNotice(db, id, t, b); });
        ToGuild(*g, proto::GuildNoticeChanged{r.title, r.body});
        Answer(a.peer, proto::GuildAction::Notice, GuildError::Ok);
    }

    void Permissions(const GuildActor& a, const proto::GuildPermissions& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::Permissions, e); };
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return;
        const Member* me = g->Find(a.character);
        if (me == nullptr || me->memberClass != game::kGuildMaster) return refuse(GuildError::Denied);
        // At most 12 of a guild may chat in the union, the master among them.
        std::unordered_map<std::int64_t, std::uint32_t> after;
        for (const Member& m : g->members) after[m.character] = m.permission;
        std::vector<std::pair<std::int64_t, std::uint32_t>> changes;
        for (const proto::GuildPermissionRecord& p : r.members) {
            Member* m = g->Find(p.character);
            if (m == nullptr || m->memberClass == game::kGuildMaster) continue;
            after[m->character] = p.permission;
            changes.emplace_back(m->character, p.permission);
        }
        if (const std::int64_t holder = host.GuildStorageHolder(g->id); holder != 0) {
            const Member* m = g->Find(holder);
            if (m != nullptr && m->memberClass != game::kGuildMaster && (after[holder] & game::kGuildPermStorage) == 0) return refuse(GuildError::StorageRightInUse);
        }
        int holders = 0;
        for (const Member& m : g->members) {
            if (m.memberClass == game::kGuildMaster || (after[m.character] & game::kGuildPermUnionChat) != 0) ++holders;
        }
        if (holders > game::kUnionChatHolders) return refuse(GuildError::UnionChatFull);
        for (const auto& [character, permission] : changes) {
            Member* m = g->Find(character);
            m->permission = permission;
            ToGuild(*g, proto::GuildMemberChanged{Record(*m)});
        }
        Write([id = g->id, changes](Database& db) { SaveGuildPermissions(db, id, changes); });
        Answer(a.peer, proto::GuildAction::Permissions, GuildError::Ok);
    }

    void MasterTransfer(const GuildActor& a, const proto::GuildMasterTransfer& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::MasterTransfer, e); };
        if (std::int64_t(r.character) == a.character) return refuse(GuildError::Failed);
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return refuse(GuildError::NoGuild);
        // A vote open in the guild (its end decides), then the master's own right, then the fortress war (0x4C5D).
        if (elections.contains(g->id) || remoteElections.contains(g->id)) return refuse(GuildError::Denied);
        Member* me = g->Find(a.character);
        if (me == nullptr || me->memberClass != game::kGuildMaster) return refuse(GuildError::Denied);
        if (host.GuildInFortressWar(g->id)) return refuse(GuildError::PositionInWar);
        const double reach = host.GuildDistance(a.entity, r.npc);
        if (!host.IsGuildNpc(r.npc) || reach < 0 || reach > game::kGuildNpcReach) return refuse(GuildError::Failed);
        Member* next = g->Find(r.character);
        if (next == nullptr) return refuse(GuildError::MemberNotFound);
        // As _Guild_Delegate_Master: the old master a member without authority, the new one every right.
        me->memberClass = game::kGuildMember;
        me->permission = 0;
        me->position = game::kPositionNone;
        next->memberClass = game::kGuildMaster;
        next->permission = game::kGuildPermAll;
        next->position = game::kPositionCommander;
        Write([id = g->id, from = a.character, to = next->character](Database& db) { DelegateGuildMaster(db, id, from, to); });
        ToGuild(*g, proto::GuildMemberChanged{Record(*me)});
        ToGuild(*g, proto::GuildMemberChanged{Record(*next)});
        TellAppearance(me->character); // the commander's position moved: those around told
        TellAppearance(next->character);
        if (g->alliance != 0) TellUnion(g->alliance); // its master's name
        Answer(a.peer, proto::GuildAction::MasterTransfer, GuildError::Ok);
        core::LogInfo("guilds: {} handed guild {} '{}' to {}", a.name, g->id, g->name, next->name);
    }

    void Nickname(const GuildActor& a, const proto::GuildNickname& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::Nickname, e); };
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return;
        if (g->level < game::kGuildNicknameLevel) return refuse(GuildError::NicknameLevel);
        const Member* me = g->Find(a.character);
        if (me == nullptr || me->memberClass != game::kGuildMaster) return refuse(GuildError::Denied);
        if (r.name.size() > game::kGuildNicknameMax || (!r.name.empty() && !host.GuildNameAllowed(r.name))) return refuse(GuildError::NicknameLetters);
        Member* m = g->Find(r.character);
        if (m == nullptr) return refuse(GuildError::MemberNotFound);
        m->nickname = r.name;
        Write([id = g->id, character = m->character, name = r.name](Database& db) { SaveGuildNickname(db, id, character, name); });
        ToGuild(*g, proto::GuildMemberChanged{Record(*m)});
        TellAppearance(m->character);
        Answer(a.peer, proto::GuildAction::Nickname, GuildError::Ok);
    }

    void SavePoints(const Guild& g, std::int64_t character, std::uint32_t donation) {
        Write([id = g.id, level = g.level, points = g.points, character, donation](Database& db) { SaveGuildPoints(db, id, level, points, character, donation); });
        ToGuild(g, proto::GuildPoints{std::uint8_t(g.level), g.points});
    }

    void LevelUp(const GuildActor& a, const proto::GuildLevelUp& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::LevelUp, e); };
        if (a.dead) return refuse(GuildError::Dead);
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return refuse(GuildError::NoGuild);
        const Member* me = g->Find(a.character);
        if (me == nullptr || me->memberClass != game::kGuildMaster) return refuse(GuildError::Denied);
        if (g->level >= game::kGuildMaxLevel) return refuse(GuildError::LevelMax);
        const double reach = host.GuildDistance(a.entity, r.npc);
        if (!host.IsGuildNpc(r.npc) || reach < 0 || reach > game::kGuildNpcReach) return refuse(GuildError::Failed);
        const std::uint64_t gold = game::kGuildLevelUpGold[std::size_t(g->level)];
        const std::uint32_t points = game::kGuildLevelUpPoints[std::size_t(g->level)];
        if (a.gold < gold) return refuse(GuildError::LevelUpGold);
        if (g->points < points) return refuse(GuildError::LevelUpPoints);
        // Raised now (the writes asked after this one see it), and written with the price and the history's row in one
        // transaction at the writer's turn (the original raises the level, then charges on its success); given back if
        // it does not stand.
        g->points -= points;
        ++g->level;
        auto ran = std::make_shared<bool>(false);
        WritePaid(a.character, gold,
                  [ran, id = g->id, level = g->level, left = g->points, name = a.name, points](Database& tx) -> std::optional<std::uint64_t> {
                      *ran = true;
                      if (!SaveGuildPoints(tx, id, level, left, 0, 0) || !AddGpHistory(tx, id, name, points, game::kGpReasonGrowth)) return std::nullopt;
                      return 0;
                  },
                  [this, ran, peer = a.peer, id = g->id, points](bool ok, std::uint64_t) {
                      const auto it = guilds.find(id);
                      if (!ok) {
                          if (it != guilds.end()) {
                              --it->second.level;
                              it->second.points += points;
                              SavePoints(it->second, 0, 0);
                          }
                          return Answer(peer, proto::GuildAction::LevelUp, *ran ? GuildError::Failed : GuildError::LevelUpGold);
                      }
                      if (it != guilds.end()) {
                          ToGuild(it->second, proto::GuildPoints{std::uint8_t(it->second.level), it->second.points});
                          if (it->second.alliance != 0) TellUnion(it->second.alliance);
                      }
                      Answer(peer, proto::GuildAction::LevelUp, GuildError::Ok);
                  });
    }

    // An ability for the guild's mercenaries (the original's 0x7259: the game server's checks, then the shard manager's job
    // with the points; game/GuildMercenary.h): at a guild NPC in reach (3), on a shard that runs them (3). Set now (the
    // writes asked after see it), written with the gold and the points in one transaction, given back if it does not stand;
    // its members told again (GuildInfo), here and on the shard's other servers, and its soldiers out raised again.
    void MercenaryAbility(const GuildActor& a, const proto::GuildMercenaryAbility& r) {
        const auto answer = [&](std::uint16_t code) {
            host.GuildSend(a.peer, proto::GuildAnswer::kId, proto::Encode(proto::GuildAnswer{proto::GuildAction::MercenaryAbility, code, 0}));
        };
        if (!host.GuildMercenaries()) return answer(std::uint16_t(GuildError::Failed));
        const double reach = host.GuildDistance(a.entity, r.npc);
        if (!host.IsGuildNpc(r.npc) || reach < 0 || reach > game::kGuildNpcReach) return answer(std::uint16_t(GuildError::Failed));
        Guild* g = GuildOfCharacter(a.character);
        const Member* me = g != nullptr ? g->Find(a.character) : nullptr;
        const std::uint16_t code = game::MercenaryPurchaseCheck(g != nullptr, g != nullptr ? g->level : 0, me != nullptr && me->memberClass == game::kGuildMaster,
                                                                a.gold, g != nullptr ? g->points : 0, g != nullptr ? g->mercenaryAttr : 0, r.ability);
        if (code != 0) return answer(code);
        const game::MercenaryPrice price = game::MercenaryPriceOf(r.ability);
        const std::uint8_t before = g->mercenaryAttr;
        const std::uint8_t bits = r.ability == 0 ? std::uint8_t(0) : std::uint8_t(before | r.ability);
        g->points -= price.points;
        g->mercenaryAttr = bits;
        auto ran = std::make_shared<bool>(false);
        WritePaid(a.character, price.gold,
                  [ran, id = g->id, level = g->level, left = g->points, bits](Database& tx) -> std::optional<std::uint64_t> {
                      *ran = true;
                      if (!SaveGuildPoints(tx, id, level, left, 0, 0)) return std::nullopt;
                      if (!tx.Query("UPDATE _guild SET mercenaryattr = $2 WHERE id = $1", {std::int64_t(id), int(bits)}).Ok()) return std::nullopt;
                      return 0;
                  },
                  [this, ran, peer = a.peer, name = a.name, id = g->id, points = price.points, before, bits](bool ok, std::uint64_t) {
                      const auto it = guilds.find(id);
                      if (!ok) {
                          if (it != guilds.end()) {
                              it->second.points += points;
                              it->second.mercenaryAttr = before;
                              SavePoints(it->second, 0, 0);
                          }
                          host.GuildSend(peer, proto::GuildAnswer::kId,
                                         proto::Encode(proto::GuildAnswer{proto::GuildAction::MercenaryAbility,
                                                                          std::uint16_t(*ran ? GuildError::Failed : GuildError::NoGold), 0}));
                          return;
                      }
                      if (it != guilds.end()) {
                          ToGuild(it->second, Info(it->second));
                          host.GuildMercenaryChanged(id, bits);
                      }
                      core::LogInfo("guilds: {} buys its guild's mercenaries' ability {:#04x} (now {:#04x})", name, int(bits == 0 ? 0 : bits & ~before), int(bits));
                      host.GuildSend(peer, proto::GuildAnswer::kId, proto::Encode(proto::GuildAnswer{proto::GuildAction::MercenaryAbility, 0, 0}));
                  });
    }

    // A fortress position for a member (0x70FF, the checks in its order).
    void PositionGrant(const GuildActor& a, const proto::GuildPositionGrant& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::Position, e); };
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return refuse(GuildError::NoGuild);
        const Member* me = g->Find(a.character);
        if (me == nullptr || me->memberClass != game::kGuildMaster) return refuse(GuildError::Denied);
        if (host.GuildInFortressWar(g->id)) return refuse(GuildError::PositionInWar);
        Member* m = g->Find(std::int64_t(r.character));
        if (m == nullptr) return refuse(GuildError::Failed);
        if (r.position == game::kPositionNone) {
            if (m->position == game::kPositionCommander) return refuse(GuildError::PositionMaster);
            if (m->position == game::kPositionNone) return refuse(GuildError::PositionNone);
        } else {
            if (m->position != game::kPositionNone) return refuse(GuildError::PositionHeld);
            if (r.position == game::kPositionCommander) return refuse(GuildError::PositionMaster);
            // Ours: nothing but the listed positions.
            if (game::GuildPositionKey(r.position) == nullptr) return refuse(GuildError::Failed);
            if (game::GuildPositionNeedsFortress(r.position) && !host.GuildHoldsFortress(g->id)) return refuse(GuildError::PositionNoFortress);
            const auto held = std::count_if(g->members.begin(), g->members.end(), [&](const Member& x) { return x.position == r.position; });
            if (held >= game::GuildPositionPlaces(r.position)) return refuse(GuildError::PositionFull);
        }
        m->position = r.position;
        Write([id = g->id, character = m->character, position = r.position](Database& db) { SaveGuildPosition(db, id, character, position); });
        ToGuild(*g, proto::GuildMemberChanged{Record(*m)});
        TellAppearance(m->character); // those around told its position (the original's 0x3103)
        Answer(a.peer, proto::GuildAction::Position, GuildError::Ok);
        core::LogInfo("guilds: {} gives {} position {}", a.name, m->name, int(r.position));
    }

    // What the guild's points went on (0x7501; a guild NPC within reach).
    void GpHistory(const GuildActor& a, const proto::GuildGpHistoryOpen& r) {
        if (!AtGuildNpc(a, r.npc)) return Answer(a.peer, proto::GuildAction::GpHistory, GuildError::Failed);
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return Answer(a.peer, proto::GuildAction::GpHistory, GuildError::NoGuild);
        auto rows = std::make_shared<std::vector<GpHistoryRow>>();
        host.GuildSubmit([id = g->id, rows](Database& db) { *rows = LoadGpHistory(db, id); },
                         [this, peer = a.peer, rows] {
                             proto::GuildGpHistory answer;
                             for (const GpHistoryRow& row : *rows) {
                                 answer.rows.push_back({row.name, row.points, std::uint8_t(row.reason), std::uint16_t(row.year), std::uint8_t(row.month), std::uint8_t(row.day),
                                                        std::uint8_t(row.hour), std::uint8_t(row.minute)});
                             }
                             Send(peer, answer);
                         });
    }

    void Donate(const GuildActor& a, const proto::GuildDonate& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::Donate, e); };
        if (r.points == 0 || r.points > a.skillPoints) return refuse(GuildError::LevelUpPoints);
        if (a.dead) return refuse(GuildError::Dead);
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return refuse(GuildError::NoGuild);
        Member* me = g->Find(a.character);
        if (me == nullptr || !host.TakeSkillPoints(a.peer, r.points)) return refuse(GuildError::LevelUpPoints);
        // One guild point for each skill point.
        g->points += r.points;
        me->donation += r.points;
        SavePoints(*g, me->character, me->donation);
        ToGuild(*g, proto::GuildMemberChanged{Record(*me)});
        Answer(a.peer, proto::GuildAction::Donate, GuildError::Ok, r.points);
    }

    // --- Unions ------------------------------------------------------------------------------------

    void InviteUnion(const GuildActor& a, const proto::UnionInvite& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::UnionInvite, e); };
        const std::optional<GuildActor> t = host.GuildPlayerAt(r.target);
        const double reach = host.GuildDistance(a.entity, r.target);
        if (!t || reach < 0 || reach > game::kGuildInviteReach) return refuse(GuildError::Failed);
        if (a.dead) return refuse(GuildError::Dead);
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return refuse(GuildError::NoGuild);
        const Member* me = g->Find(a.character);
        const auto alliance = alliances.find(g->alliance);
        const bool leads = g->alliance == 0 || (alliance != alliances.end() && alliance->second.guilds[0] == g->id);
        if (me == nullptr || me->memberClass != game::kGuildMaster || !leads) return refuse(GuildError::Denied);
        if (host.GuildInFortressWar(g->id)) return refuse(GuildError::WarNoUnionInvite);
        if (g->level < game::kUnionMinLevel) return refuse(GuildError::UnionLevel);
        Guild* other = GuildOfCharacter(t->character);
        if (other == nullptr) return refuse(GuildError::NoTargetGuild);
        if (other->id == g->id) return refuse(GuildError::OwnGuild);
        if (other->level < game::kUnionMinLevel) return refuse(GuildError::UnionTargetLevel);
        if (other->alliance != 0) return refuse(GuildError::TargetHasUnion);
        const Member* theirs = other->Find(t->character);
        if (theirs == nullptr || theirs->memberClass != game::kGuildMaster) return refuse(GuildError::TargetDenied);
        if (WarBetween(g->id, other->id) != nullptr) return refuse(GuildError::UnionAtWar);
        // Ours here: the eight places (the original's shard manager refuses a ninth, 0x4C29).
        if (alliance != alliances.end() && std::count(alliance->second.guilds.begin(), alliance->second.guilds.end(), 0u) == 0) {
            return refuse(GuildError::UnionFull);
        }
        if (proposals.contains(t->character)) return refuse(GuildError::TargetBusy);
        proposals[t->character] = {proto::kProposalUnion, a.character, g->id, other->id, Clock::now() + std::chrono::seconds(game::kProposalSeconds)};
        Send(t->peer, proto::GuildProposal{proto::kProposalUnion, a.entity, a.name, g->name});
    }

    void JoinUnion(const GuildActor& a, const Proposal& p, std::uint64_t inviterPeer) {
        const auto leader = guilds.find(p.guild);
        Guild* joining = GuildOfCharacter(a.character);
        GuildError e = GuildError::Ok;
        if (leader == guilds.end() || joining == nullptr || joining->id != p.targetGuild) e = GuildError::Failed;
        else if (joining->alliance != 0) e = GuildError::TargetHasUnion;
        if (e != GuildError::Ok) {
            Answer(inviterPeer, proto::GuildAction::UnionInvite, e);
            return Answer(a.peer, proto::GuildAction::UnionJoin, e);
        }
        Guild& lead = leader->second;
        const std::uint32_t joinId = joining->id;
        const std::string joinName = joining->name;
        if (lead.alliance == 0) {
            // The first acceptance makes the union, the inviting guild leading (_AllianceCreate).
            auto made = std::make_shared<std::uint32_t>(0);
            Write([made, l = lead.id, joinId](Database& db) { *made = CreateAlliance(db, l, joinId); },
                  [this, made, l = lead.id, joinId, joinName, inviterPeer, peer = a.peer] {
                      const auto lg = guilds.find(l), jg = guilds.find(joinId);
                      if (*made == 0 || lg == guilds.end() || jg == guilds.end()) {
                          Answer(inviterPeer, proto::GuildAction::UnionInvite, GuildError::Failed);
                          return Answer(peer, proto::GuildAction::UnionJoin, GuildError::Failed);
                      }
                      AllianceRow row;
                      row.id = *made;
                      row.guilds[0] = l;
                      row.guilds[1] = joinId;
                      alliances[row.id] = row;
                      lg->second.alliance = row.id;
                      jg->second.alliance = row.id;
                      Joined(row.id, joinName, inviterPeer, peer, {l, joinId});
                      // The union stands only now: the shard's other servers load it and its guilds.
                      Touch(l);
                      PublishTouched();
                  });
            return;
        }
        AllianceRow& row = alliances[lead.alliance];
        const auto slot = std::find(row.guilds.begin(), row.guilds.end(), 0u);
        if (slot == row.guilds.end()) {
            Answer(inviterPeer, proto::GuildAction::UnionInvite, GuildError::UnionFull);
            return Answer(a.peer, proto::GuildAction::UnionJoin, GuildError::UnionFull);
        }
        *slot = joinId;
        joining->alliance = row.id;
        Write([row](Database& db) { SaveAlliance(db, row); });
        Joined(row.id, joinName, inviterPeer, a.peer, {joinId});
    }

    void Joined(std::uint32_t alliance, const std::string& guildName, std::uint64_t inviterPeer, std::uint64_t peer,
                std::vector<std::uint32_t> changed) {
        TellUnion(alliance);
        ToUnion(alliances[alliance], proto::UnionChanged{1, guildName});
        for (std::uint32_t id : changed) {
            if (const auto g = guilds.find(id); g != guilds.end()) TellGuildAppearance(g->second);
        }
        Answer(inviterPeer, proto::GuildAction::UnionInvite, GuildError::Ok);
        Answer(peer, proto::GuildAction::UnionJoin, GuildError::Ok);
        core::LogInfo("guilds: '{}' joined union {}", guildName, alliance);
    }

    // A guild out of its union (event 2 left, 3 put out). The union dissolves when its leading guild
    // goes or fewer than two remain (the original's shard manager decides it; inferred).
    void LeaveUnion(Guild& g, std::uint8_t event) {
        const auto it = alliances.find(g.alliance);
        if (it == alliances.end()) {
            g.alliance = 0;
            return;
        }
        AllianceRow& row = it->second;
        const bool leader = row.guilds[0] == g.id;
        std::replace(row.guilds.begin(), row.guilds.end(), g.id, 0u);
        g.alliance = 0;
        const auto remaining = std::count_if(row.guilds.begin(), row.guilds.end(), [](std::uint32_t id) { return id != 0; });
        ToGuild(g, proto::UnionChanged{event, g.name});
        ToGuild(g, proto::UnionInfo{});
        TellGuildAppearance(g);
        if (leader || remaining < 2) {
            ToUnion(row, proto::UnionChanged{event, g.name});
            Dissolve(row.id);
            return;
        }
        // Packed to the front (the leader stays first).
        std::stable_partition(row.guilds.begin(), row.guilds.end(), [](std::uint32_t id) { return id != 0; });
        Write([copy = row](Database& db) { SaveAlliance(db, copy); });
        ToUnion(row, proto::UnionChanged{event, g.name});
        TellUnion(row.id);
    }

    void Dissolve(std::uint32_t alliance) {
        const auto it = alliances.find(alliance);
        if (it == alliances.end()) return;
        const AllianceRow row = it->second;
        alliances.erase(it);
        crests.erase({proto::kCrestUnion, alliance});
        Write([alliance](Database& db) { RemoveAlliance(db, alliance); });
        for (std::uint32_t id : row.guilds) {
            const auto g = guilds.find(id);
            if (id == 0 || g == guilds.end()) continue;
            g->second.alliance = 0;
            ToGuild(g->second, proto::UnionChanged{4, {}});
            ToGuild(g->second, proto::UnionInfo{});
            TellGuildAppearance(g->second);
        }
        core::LogInfo("guilds: union {} dissolved", alliance);
    }

    // --- The master's election ------------------------------------------

    bool InElection(const Guild& g, std::int64_t character) const {
        const auto it = elections.find(g.id);
        if (it == elections.end()) return false;
        const Election& e = it->second;
        return e.voters.contains(character) || std::find(e.candidates.begin(), e.candidates.end(), character) != e.candidates.end();
    }
    // Days since a member was last on (0 while online).
    double DaysAway(const Member& m) const {
        if (online.contains(m.character)) return 0.0;
        return std::chrono::duration<double>(std::chrono::system_clock::now() - m.lastSeen).count() / 86400.0;
    }
    proto::GuildElection ElectionFor(const Guild& g, const Election& e, std::int64_t member) const {
        proto::GuildElection out;
        out.active = 1;
        out.secondsLeft = std::uint32_t(std::max<std::int64_t>(0, std::chrono::ceil<std::chrono::seconds>(e.ends - Clock::now()).count()));
        for (std::int64_t c : e.candidates) {
            const auto m = std::find_if(g.members.begin(), g.members.end(), [&](const Member& x) { return x.character == c; });
            if (m == g.members.end()) continue;
            const auto votes = std::count_if(e.ballots.begin(), e.ballots.end(), [&](const auto& b) { return b.second == c; });
            out.candidates.push_back({std::uint32_t(c), m->name, std::uint16_t(votes)});
        }
        if (const auto b = e.ballots.find(member); b != e.ballots.end()) out.ballot = std::uint32_t(b->second);
        out.mayVote = e.voters.contains(member) ? 1 : 0;
        return out;
    }
    void TellElection(const Guild& g, const Election& e) {
        for (const Member& m : g.members) SendTo(m.character, ElectionFor(g, e, m.character));
    }
    bool AtGuildNpc(const GuildActor& a, std::uint32_t npc) const {
        const double reach = host.GuildDistance(a.entity, npc);
        return host.IsGuildNpc(npc) && reach >= 0 && reach <= game::kGuildNpcReach;
    }

    // A member asks for the master's dismissal: at a guild NPC, in
    // a guild (0x4C0D), no election running (0x4C37), the master found and away 45 days
    // (`_Guild_Master_Election_Available`, else 0x4C33). Ours (the shard manager's part): the
    // candidates the members but the master on within 7 days, the voters those on within 3, none
    // standing: 0x4C33.
    void ElectionStart(const GuildActor& a, const proto::GuildElectionStart& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::ElectionStart, e); };
        if (!AtGuildNpc(a, r.npc)) return refuse(GuildError::Failed);
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return refuse(GuildError::NoGuild);
        if (elections.contains(g->id) || remoteElections.contains(g->id)) return refuse(GuildError::ElectionOpen);
        const Member* master = g->Master();
        if (master == nullptr || master->character == a.character || DaysAway(*master) < game::kElectionAbsenceDays) return refuse(GuildError::NoElection);
        Election e;
        e.ends = Clock::now() + std::chrono::seconds(electionSeconds);
        for (const Member& m : g->members) {
            if (m.memberClass == game::kGuildMaster) continue;
            const double away = DaysAway(m);
            if (away <= game::kElectionCandidateDays) e.candidates.push_back(m.character);
            if (away <= game::kElectionVoterDays) e.voters.insert(m.character);
        }
        if (e.candidates.empty()) return refuse(GuildError::NoElection);
        const Election& made = elections[g->id] = std::move(e);
        Answer(a.peer, proto::GuildAction::ElectionStart, GuildError::Ok);
        BusEnvelope running;
        running.kind = kBusElection;
        running.guild = g->id;
        running.value = 1;
        Publish(std::move(running));
        core::LogInfo("guilds: {} calls an election in guild {} '{}' ({} candidates, {} voters)", a.name, g->id, g->name, made.candidates.size(),
                      made.voters.size());
        TellElection(*g, made);
    }

    // The vote's window (the vote's candidates and their ballots, 0xB106).
    // A request for an election another server of the shard holds: passed on to it (this server checked
    // the NPC), its answers come back to the member.
    template <typename T>
    bool PassOn(const GuildActor& a, const Guild* g, const T& request) {
        const auto remote = g != nullptr && a.entity != 0 ? remoteElections.find(g->id) : remoteElections.end();
        if (remote == remoteElections.end() || elections.contains(g->id)) return false;
        BusEnvelope e;
        e.kind = kBusRequest;
        e.guild = g->id;
        e.character = a.character;
        e.name = a.name;
        e.value = remote->second;
        const std::vector<std::byte> bytes = proto::Encode(request);
        e.message = T::kId;
        e.payload.resize(bytes.size());
        std::memcpy(e.payload.data(), bytes.data(), bytes.size());
        Publish(std::move(e));
        return true;
    }

    void ElectionOpen(const GuildActor& a, const proto::GuildElectionOpen& r) {
        Guild* g = GuildOfCharacter(a.character);
        if (PassOn(a, g, r)) return;
        const auto it = g != nullptr ? elections.find(g->id) : elections.end();
        if (it == elections.end()) return Answer(a.peer, proto::GuildAction::ElectionOpen, GuildError::NoVote);
        proto::GuildElection view = ElectionFor(*g, it->second, a.character);
        view.show = 1;
        Send(a.peer, view);
    }

    // A ballot: at a guild NPC, an election running (0x5006), a voter (0x5008); a second one moves it.
    void ElectionVote(const GuildActor& a, const proto::GuildElectionVote& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::ElectionVote, e); };
        if (a.entity != 0 && !AtGuildNpc(a, r.npc)) return refuse(GuildError::Failed); // passed on: checked where it was asked
        Guild* g = GuildOfCharacter(a.character);
        if (PassOn(a, g, r)) return;
        const auto it = g != nullptr ? elections.find(g->id) : elections.end();
        if (it == elections.end()) return refuse(GuildError::NoVote);
        Election& e = it->second;
        if (!e.voters.contains(a.character)) return refuse(GuildError::NoVotingRight);
        if (std::find(e.candidates.begin(), e.candidates.end(), std::int64_t(r.candidate)) == e.candidates.end()) return refuse(GuildError::Failed);
        e.ballots[a.character] = r.candidate;
        Answer(a.peer, proto::GuildAction::ElectionVote, GuildError::Ok);
        // The shard manager decides at the ballot (SM): the candidate's ballots x 100 / the candidates,
        // 60 or more, wins at once (0x5007; 0x3908 kind 1, then the hand-over SM).
        const long votes = long(std::count_if(e.ballots.begin(), e.ballots.end(), [&](const auto& b) { return b.second == std::int64_t(r.candidate); }));
        if (double(votes) * 100.0 / double(e.candidates.size()) >= game::kElectionWinPercent) {
            const Election decided = std::move(e);
            elections.erase(it);
            EndElection(*g, decided, std::int64_t(r.candidate));
            Touch(g->id);
            PublishTouched();
            return;
        }
        TellElection(*g, e);
    }

    // An election over: won at a ballot (`winner`), the master handed over as a hand-over makes one; or its time up
    // with nobody at 60 % (`winner` 0): cancelled (0x3908 kind 2; the client's
    // "The election of the leader has been cancelled due to inability to satisfy requirements.").
    void EndElection(Guild& g, const Election& e, std::int64_t winner) {
        const long best = long(std::count_if(e.ballots.begin(), e.ballots.end(), [&](const auto& b) { return b.second == winner; }));
        if (winner != 0 && g.Find(winner) == nullptr) winner = 0;
        for (const Member& m : g.members) SendTo(m.character, proto::GuildElection{});
        BusEnvelope over;
        over.kind = kBusElection;
        over.guild = g.id;
        Publish(std::move(over));
        Member* next = winner != 0 ? g.Find(winner) : nullptr;
        Member* old = nullptr;
        for (Member& m : g.members) {
            if (m.memberClass == game::kGuildMaster) old = &m;
        }
        if (next == nullptr || old == nullptr) {
            ToGuild(g, proto::GuildElectionEnded{g.name, {}});
            core::LogInfo("guilds: the election in guild {} '{}' ends without a winner", g.id, g.name);
            return;
        }
        old->memberClass = game::kGuildMember;
        old->permission = 0;
        next->memberClass = game::kGuildMaster;
        next->permission = game::kGuildPermAll;
        old->position = game::kPositionNone;
        next->position = game::kPositionCommander;
        Write([id = g.id, from = old->character, to = next->character](Database& db) { DelegateGuildMaster(db, id, from, to); });
        ToGuild(g, proto::GuildMemberChanged{Record(*old)});
        ToGuild(g, proto::GuildMemberChanged{Record(*next)});
        if (g.alliance != 0) TellUnion(g.alliance);
        ToGuild(g, proto::GuildElectionEnded{g.name, next->name});
        core::LogInfo("guilds: {} wins the election in guild {} '{}' ({} ballots)", next->name, g.id, g.name, best);
    }

    // --- Guild wars -------------------------------------------------------

    proto::GuildWars WarsOf(std::uint32_t guild) const {
        proto::GuildWars out;
        for (const auto& [id, w] : wars) {
            const int side = w.Side(guild);
            if (side < 0) continue;
            const auto enemy = guilds.find(w.guilds[1 - side]);
            proto::GuildWarRecord r;
            r.guild = w.guilds[1 - side];
            if (enemy != guilds.end()) {
                r.name = enemy->second.name;
                r.level = std::uint8_t(enemy->second.level);
                r.members = std::uint16_t(enemy->second.members.size());
                if (const Member* m = enemy->second.Master()) r.master = m->name;
            }
            r.ours = w.points[side];
            r.theirs = w.points[1 - side];
            r.terms = w.terms;
            if (w.ends) {
                r.secondsLeft = std::uint32_t(std::max<std::int64_t>(0, std::chrono::ceil<std::chrono::seconds>(*w.ends - std::chrono::system_clock::now()).count()));
            }
            if (w.surrender) r.ending = std::uint32_t(std::max<std::int64_t>(1, std::chrono::ceil<std::chrono::seconds>(*w.surrender - Clock::now()).count()));
            out.wars.push_back(std::move(r));
        }
        return out;
    }
    void TellWars(std::uint32_t guild) {
        if (const auto g = guilds.find(guild); g != guilds.end()) ToGuildHere(g->second, proto::GuildWars::kId, proto::Encode(WarsOf(guild)), 0);
    }
    // A war changed: both guilds' members here told, the shard's other servers load the wars again.
    void WarsChanged(std::uint32_t a, std::uint32_t b) {
        TellWars(a);
        TellWars(b);
        ++mutations;
        BusEnvelope e;
        e.kind = kBusWars;
        Publish(std::move(e));
    }
    void News(std::uint32_t guild, proto::GuildWarNews news) {
        if (const auto g = guilds.find(guild); g != guilds.end()) ToGuild(g->second, news);
    }

    // A war declared, the original's checks in its order: the guild
    // by its name (0x4C25), its master in the game and within 300 (0x4C60), in a guild (0x4C0D), the
    // terms, not one's own (3), the master (0x4C1E), the stake in hand (0x4C43) and at most 500,000,000
    // (0x4C5E), 50 hostile guilds on either side (0x4C3E, 0x4C3F), not at war already (0x4C3A), not the
    // same union (0x4C41); then the proposal to the other master.
    void WarDeclare(const GuildActor& a, const proto::GuildWarDeclare& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::WarDeclare, e); };
        const auto target = names.find(Lower(r.target));
        const auto them = target != names.end() ? guilds.find(target->second) : guilds.end();
        if (them == guilds.end()) return refuse(GuildError::NoTargetGuild);
        const Member* theirMaster = them->second.Master();
        const auto there = theirMaster != nullptr ? online.find(theirMaster->character) : online.end();
        if (there == online.end()) return refuse(GuildError::WarFar);
        const double far = host.GuildDistance(a.entity, there->second.entity);
        if (far < 0 || far > game::kGuildWarReach) return refuse(GuildError::WarFar);
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return refuse(GuildError::NoGuild);
        if (!game::GuildWarTermsValid(r.terms)) return refuse(GuildError::Failed);
        if (g->id == them->second.id) return refuse(GuildError::Failed);
        const Member* me = g->Find(a.character);
        if (me == nullptr || me->memberClass != game::kGuildMaster) return refuse(GuildError::Denied);
        if (a.gold < r.terms.wager) return refuse(GuildError::WarGold);
        if (r.terms.wager > game::kGuildWarMaxWager) return refuse(GuildError::WagerLimit);
        if (Hostiles(g->id) >= game::kGuildWarMaxHostile) return refuse(GuildError::HostileFull);
        if (Hostiles(them->second.id) >= game::kGuildWarMaxHostile) return refuse(GuildError::TargetHostileFull);
        if (WarBetween(g->id, them->second.id) != nullptr) return refuse(GuildError::AlreadyEnemies);
        if (g->alliance != 0 && g->alliance == them->second.alliance) return refuse(GuildError::AllyWar);
        if (host.GuildInEventWorld(a.entity) && host.GuildInEventWorld(there->second.entity)) return refuse(GuildError::WarInEvent);
        if (host.GuildInArena(a.entity) || host.GuildInArena(there->second.entity)) return refuse(GuildError::WarInArena);
        if (proposals.contains(theirMaster->character)) return refuse(GuildError::TargetBusy);
        Proposal p;
        p.kind = proto::kProposalWar;
        p.from = a.character;
        p.guild = g->id;
        p.targetGuild = them->second.id;
        p.expires = Clock::now() + std::chrono::seconds(game::kProposalSeconds);
        p.terms = r.terms;
        proposals[theirMaster->character] = p;
        Send(there->second.peer, proto::GuildWarProposal{a.name, g->name, r.terms});
        core::LogInfo("guilds: {} of '{}' declares war on '{}' ({} gold, victory {})", a.name, g->name, them->second.name, r.terms.wager, int(r.terms.victory));
    }

    // The other master's answer: refused ("[%s]guild refused to participate in the war."), or the
    // war begins once both masters paid the stake (each its half of the original's LodgedGold).
    void AnswerWar(const GuildActor& a, const Proposal& p, std::uint64_t fromPeer, bool accept) {
        const auto mine = guilds.find(p.targetGuild), theirs = guilds.find(p.guild);
        if (!accept) {
            if (mine != guilds.end()) Send(fromPeer, proto::GuildWarNews{2, mine->second.name, {}, {}});
            Answer(fromPeer, proto::GuildAction::WarDeclare, GuildError::JoinRefused);
            return Answer(a.peer, proto::GuildAction::WarAnswer, GuildError::JoinRefused);
        }
        const auto fail = [&](GuildError e) {
            Answer(fromPeer, proto::GuildAction::WarDeclare, e);
            Answer(a.peer, proto::GuildAction::WarAnswer, e);
        };
        if (mine == guilds.end() || theirs == guilds.end()) return fail(GuildError::NoTargetGuild);
        const std::uint64_t pair = (std::uint64_t(std::min(p.guild, p.targetGuild)) << 32) | std::max(p.guild, p.targetGuild);
        if (WarBetween(p.guild, p.targetGuild) != nullptr || warsStarting.contains(std::uint32_t(pair ^ (pair >> 32)))) {
            return fail(GuildError::AlreadyEnemies);
        }
        if (Hostiles(p.guild) >= game::kGuildWarMaxHostile || Hostiles(p.targetGuild) >= game::kGuildWarMaxHostile) return fail(GuildError::HostileFull);
        // "If the leader of guild doesn't have guild war points to spend, You can't accept the war proposal."
        if (a.gold < p.terms.wager) return fail(GuildError::WarGold);
        if (!host.TakeGold(fromPeer, p.terms.wager)) return fail(GuildError::WarGold);
        if (!host.TakeGold(a.peer, p.terms.wager)) {
            host.GiveGold(fromPeer, p.terms.wager);
            return fail(GuildError::WarGold);
        }
        GuildWarRow row;
        row.first = p.guild;
        row.second = p.targetGuild;
        row.victory = p.terms.victory;
        row.wager = p.terms.wager;
        row.lodged = p.terms.wager * 2;
        const std::int64_t seconds = game::GuildWarSeconds(p.terms);
        row.secondsLeft = p.terms.unlimited != 0 || seconds == 0 ? -1 : seconds;
        row.period = (p.terms.unlimited != 0 ? 1u << 24 : 0u) | std::uint32_t(p.terms.days) << 16 | std::uint32_t(p.terms.hours) << 8 | p.terms.minutes;
        const std::uint32_t key = std::uint32_t(pair ^ (pair >> 32));
        warsStarting.insert(key);
        auto made = std::make_shared<std::uint32_t>(0);
        Write([made, row](Database& db) { *made = BeginGuildWar(db, row); },
              [this, made, row, key, terms = p.terms, fromPeer, peer = a.peer] {
                  warsStarting.erase(key);
                  if (*made == 0) {
                      host.GiveGold(fromPeer, terms.wager);
                      host.GiveGold(peer, terms.wager);
                      Answer(fromPeer, proto::GuildAction::WarDeclare, GuildError::Failed);
                      return Answer(peer, proto::GuildAction::WarAnswer, GuildError::Failed);
                  }
                  War w;
                  w.id = *made;
                  w.guilds[0] = row.first;
                  w.guilds[1] = row.second;
                  w.terms = terms;
                  w.lodged = row.lodged;
                  if (row.secondsLeft >= 0) w.ends = std::chrono::system_clock::now() + std::chrono::seconds(row.secondsLeft);
                  wars[w.id] = w;
                  Answer(fromPeer, proto::GuildAction::WarDeclare, GuildError::Ok);
                  Answer(peer, proto::GuildAction::WarAnswer, GuildError::Ok);
                  const std::string a = guilds.contains(row.first) ? guilds[row.first].name : std::string();
                  const std::string b = guilds.contains(row.second) ? guilds[row.second].name : std::string();
                  // "The war between [%s]guild and [%s]guild has now started." to both.
                  News(row.first, proto::GuildWarNews{1, a, b, {}});
                  News(row.second, proto::GuildWarNews{1, a, b, {}});
                  core::LogInfo("guilds: war {} begins: '{}' against '{}'", w.id, a, b);
                  WarsChanged(row.first, row.second);
              });
    }

    // A master gives the war up: in a guild (0x4C0D), at war with that guild,
    // the master (0x4C1E); a minute later the other guild wins (the shard manager's delay, the client's
    // "is to close in a minute").
    void WarEnd(const GuildActor& a, const proto::GuildWarEnd& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::WarEnd, e); };
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return refuse(GuildError::NoGuild);
        const War* found = WarBetween(g->id, r.guild);
        if (found == nullptr) return refuse(GuildError::NotEnemies);
        const Member* me = g->Find(a.character);
        if (me == nullptr || me->memberClass != game::kGuildMaster) return refuse(GuildError::Denied);
        War& w = wars[found->id];
        if (w.surrender) return refuse(GuildError::Failed);
        w.surrender = Clock::now() + std::chrono::seconds(warEndSeconds);
        w.surrendering = g->id;
        Answer(a.peer, proto::GuildAction::WarEnd, GuildError::Ok);
        const std::string first = guilds[w.guilds[0]].name, second = guilds[w.guilds[1]].name;
        News(w.guilds[0], proto::GuildWarNews{4, first, second, {}});
        News(w.guilds[1], proto::GuildWarNews{4, first, second, {}});
        TellWars(w.guilds[0]);
        TellWars(w.guilds[1]);
    }

    // A war over: the winner's guild gets both stakes as its reward (_GuildWar_EndOfWar's Booty), none
    // (a draw at the end of its time): each its own back; both told.
    void EndWar(std::uint32_t id, std::uint32_t winner) {
        const auto it = wars.find(id);
        if (it == wars.end()) return;
        const War w = it->second;
        wars.erase(it);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> rewards;
        if (winner != 0) {
            rewards.push_back({winner, w.lodged});
        } else {
            rewards.push_back({w.guilds[0], w.lodged / 2});
            rewards.push_back({w.guilds[1], w.lodged - w.lodged / 2});
        }
        for (const auto& [guild, booty] : rewards) {
            if (const auto g = guilds.find(guild); g != guilds.end()) g->second.booty += booty;
        }
        Write([id, rewards](Database& db) { EndGuildWar(db, id, rewards); });
        const std::string first = guilds.contains(w.guilds[0]) ? guilds[w.guilds[0]].name : std::string();
        const std::string second = guilds.contains(w.guilds[1]) ? guilds[w.guilds[1]].name : std::string();
        // "The war between [%s]guild and [%s]guild has been closed.", then the winner's and loser's lines.
        News(w.guilds[0], proto::GuildWarNews{5, first, second, {}});
        News(w.guilds[1], proto::GuildWarNews{5, first, second, {}});
        if (winner != 0) {
            const std::uint32_t loser = winner == w.guilds[0] ? w.guilds[1] : w.guilds[0];
            News(winner, proto::GuildWarNews{6, winner == w.guilds[0] ? second : first, {}, {}});
            News(loser, proto::GuildWarNews{7, winner == w.guilds[0] ? first : second, {}, {}});
        }
        core::LogInfo("guilds: war {} over ('{}' {} : {} '{}'), {}", id, first, w.points[0], w.points[1], second,
                      winner != 0 ? "won by " + (winner == w.guilds[0] ? first : second) : std::string("a draw"));
        Touch(w.guilds[0]);
        Touch(w.guilds[1]);
        WarsChanged(w.guilds[0], w.guilds[1]);
        PublishTouched();
    }

    // A kill between guilds at war (the shard manager's count SM):
    // the points by the levels (game::GuildWarKillPoints) to the killer's side, the kill and contribution to the
    // killer, the death to the victim, its line to both; then the war over when the killer's side reached the
    // victory's points, or when its end time is past (the more points win; even: the second guild, SM).
    void Killed(std::int64_t killer, std::int64_t victim, int killerLevel, int victimLevel) {
        Guild* kg = GuildOfCharacter(killer);
        Guild* vg = GuildOfCharacter(victim);
        if (kg == nullptr || vg == nullptr) return;
        const War* found = WarBetween(kg->id, vg->id);
        if (found == nullptr) return;
        War& w = wars[found->id];
        const int side = w.Side(kg->id);
        const std::uint32_t gain = game::GuildWarKillPoints(killerLevel, victimLevel);
        w.points[side] += gain;
        Member* km = kg->Find(killer);
        const Member* vm = vg->Find(victim);
        if (km != nullptr) {
            km->contribution += gain;
            ToGuild(*kg, proto::GuildMemberChanged{Record(*km)});
        }
        Write([id = w.id, first = side == 0, gain, kgid = kg->id, killer, vgid = vg->id, victim](Database& db) {
            CountGuildWarKill(db, id, first, gain, kgid, killer, vgid, victim);
        });
        // "[%s]killed [%s]or [%s]'s belonging."
        const proto::GuildWarNews news{3, km != nullptr ? km->name : std::string(), vm != nullptr ? vm->name : std::string(), vg->name};
        News(kg->id, news);
        News(vg->id, news);
        const std::uint32_t goal = game::kGuildWarVictoryPoints[std::min<std::size_t>(w.terms.victory, game::kGuildWarVictoryPoints.size() - 1)];
        if (goal != 0 && w.points[side] >= goal) return EndWar(w.id, kg->id);
        if (w.ends && std::chrono::system_clock::now() >= *w.ends) return EndWar(w.id, w.points[0] > w.points[1] ? w.guilds[0] : w.guilds[1]);
        WarsChanged(kg->id, vg->id);
    }

    // The reward to collect: asked (0x7114, in a guild
    // 0x4C0D, none 0x4C45) and taken by the master at a guild NPC (0x7113, 0x4C1E, 0x4C45).
    void WarReward(const GuildActor& a, const proto::GuildWarRewardQuery& r) {
        if (!AtGuildNpc(a, r.npc)) return Answer(a.peer, proto::GuildAction::WarReward, GuildError::Failed);
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return Answer(a.peer, proto::GuildAction::WarReward, GuildError::NoGuild);
        if (g->booty == 0) return Answer(a.peer, proto::GuildAction::WarReward, GuildError::NoWarReward);
        Send(a.peer, proto::GuildWarReward{g->booty});
    }
    void WarClaim(const GuildActor& a, const proto::GuildWarRewardClaim& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::WarClaim, e); };
        if (!AtGuildNpc(a, r.npc)) return refuse(GuildError::Failed);
        Guild* g = GuildOfCharacter(a.character);
        if (g == nullptr) return refuse(GuildError::NoGuild);
        const Member* me = g->Find(a.character);
        if (me == nullptr || me->memberClass != game::kGuildMaster) return refuse(GuildError::Denied);
        if (g->booty == 0) return refuse(GuildError::NoWarReward);
        // Out of the guild's row and into the master's gold in one transaction, what the database held paid (the
        // original's shard manager empties the booty first; ours pays it once, never twice).
        WritePaid(a.character, 0, [id = g->id](Database& tx) -> std::optional<std::uint64_t> { return TakeGuildBooty(tx, id); },
                  [this, peer = a.peer, name = a.name, id = g->id](bool ok, std::uint64_t gold) {
                      const auto it = guilds.find(id);
                      if (ok && it != guilds.end()) it->second.booty = 0;
                      if (!ok) return Answer(peer, proto::GuildAction::WarClaim, GuildError::Failed);
                      if (gold == 0) return Answer(peer, proto::GuildAction::WarClaim, GuildError::NoWarReward);
                      Answer(peer, proto::GuildAction::WarClaim, GuildError::Ok);
                      core::LogInfo("guilds: {} collects {} gold of war rewards for '{}'", name, gold, it != guilds.end() ? it->second.name : std::string());
                  });
    }

    void ReloadWars() {
        auto rows = std::make_shared<std::vector<GuildWarRow>>();
        auto ok = std::make_shared<bool>(false);
        WriteQuiet([rows, ok](Database& db) {
            std::string error;
            *ok = LoadGuildWars(db, *rows, error);
        },
                   [this, rows, ok] {
                       if (!*ok) return;
                       std::set<std::uint32_t> concerned;
                       for (const auto& [id, w] : wars) concerned.insert({w.guilds[0], w.guilds[1]});
                       std::map<std::uint32_t, War> fresh;
                       for (const GuildWarRow& row : *rows) {
                           War w = FromWarRow(row);
                           if (const auto old = wars.find(w.id); old != wars.end()) {
                               w.surrender = old->second.surrender; // a surrender's minute runs where it was asked
                               w.surrendering = old->second.surrendering;
                           }
                           concerned.insert({w.guilds[0], w.guilds[1]});
                           fresh[w.id] = w;
                       }
                       wars = std::move(fresh);
                       for (std::uint32_t g : concerned) TellWars(g);
                   });
    }
    static War FromWarRow(const GuildWarRow& row) {
        War w;
        w.id = row.id;
        w.guilds[0] = row.first;
        w.guilds[1] = row.second;
        w.points[0] = row.points1;
        w.points[1] = row.points2;
        w.lodged = row.lodged;
        w.terms.victory = row.victory;
        w.terms.wager = row.wager;
        w.terms.unlimited = (row.period >> 24) & 1;
        w.terms.days = std::uint8_t(row.period >> 16);
        w.terms.hours = std::uint8_t(row.period >> 8);
        w.terms.minutes = std::uint8_t(row.period);
        if (row.secondsLeft >= 0) w.ends = std::chrono::system_clock::now() + std::chrono::seconds(row.secondsLeft);
        return w;
    }

    // --- Another server's changes --------------------------------------------------------------------

    void Reload(std::vector<std::uint32_t> ids, std::vector<std::uint32_t> unions) {
        std::map<std::uint32_t, std::uint64_t> seen;
        for (std::uint32_t id : ids) seen[id] = changes[id];
        auto data = std::make_shared<GuildData>();
        auto ok = std::make_shared<bool>(false);
        WriteQuiet([ids, unions, data, ok](Database& db) {
            std::string error;
            *ok = LoadSomeGuilds(db, ids, unions, *data, error);
            if (!*ok) core::LogWarning("guilds: loading guilds another server changed: {}", error);
        },
              [this, ids, unions, data, ok, seen] {
                  if (!*ok) return;
                  // A change of ours since: its writes come after this load; load again after them.
                  for (const auto& [id, count] : seen) {
                      if (changes[id] != count) return Reload(ids, unions);
                  }
                  Apply(*data, ids, unions);
              });
    }

    void Apply(GuildData& data, const std::vector<std::uint32_t>& ids, const std::vector<std::uint32_t>& unions) {
        for (std::uint32_t id : unions) {
            const auto row = std::find_if(data.alliances.begin(), data.alliances.end(), [&](const AllianceRow& a) { return a.id == id; });
            if (row != data.alliances.end()) {
                alliances[id] = *row;
            } else {
                alliances.erase(id);
            }
            crests.erase({proto::kCrestUnion, id});
        }
        std::set<std::int64_t> concerned; // the members before and after, to tell those online here
        std::map<std::uint32_t, std::uint8_t> mercenariesBefore; // the abilities before (another server's purchase)
        for (std::uint32_t id : ids) {
            if (const auto old = guilds.find(id); old != guilds.end()) {
                mercenariesBefore[id] = old->second.mercenaryAttr;
                for (const Member& m : old->second.members) {
                    concerned.insert(m.character);
                    if (const auto it = memberOf.find(m.character); it != memberOf.end() && it->second == id) memberOf.erase(it);
                }
                names.erase(Lower(old->second.name));
                guilds.erase(old);
            }
            crests.erase({proto::kCrestGuild, id});
        }
        for (GuildRow& row : data.guilds) {
            Guild g = FromRow(row);
            for (const Member& m : g.members) concerned.insert(m.character);
            names[Lower(g.name)] = g.id;
            if (const auto before = mercenariesBefore.find(g.id); before != mercenariesBefore.end() && before->second != g.mercenaryAttr) {
                host.GuildMercenaryChanged(g.id, g.mercenaryAttr);
            }
            guilds[g.id] = std::move(g);
        }
        for (CrestRow& c : data.crests) {
            if (c.pixels.size() == proto::kCrestBytes) crests[{c.kind, c.id}] = std::move(c.pixels);
        }
        for (std::uint32_t id : ids) {
            if (!guilds.contains(id)) elections.erase(id);
        }
        for (std::int64_t character : concerned) {
            const auto o = online.find(character);
            if (o == online.end()) continue;
            Guild* g = GuildOfCharacter(character);
            host.GuildSend(o->second.peer, proto::GuildInfo::kId, proto::Encode(g != nullptr ? Info(*g) : proto::GuildInfo{}));
            if (g != nullptr && g->alliance != 0) host.GuildSend(o->second.peer, proto::UnionInfo::kId, proto::Encode(UnionOf(g->alliance)));
            if (g == nullptr) host.GuildSend(o->second.peer, proto::UnionInfo::kId, proto::Encode(proto::UnionInfo{}));
            TellAppearance(character);
        }
    }

    Guild FromRow(GuildRow& row) {
        Guild g;
        g.id = row.id;
        g.name = row.name;
        g.level = std::clamp(row.level, 1, game::kGuildMaxLevel);
        g.points = row.points;
        g.title = row.noticeTitle;
        g.notice = row.notice;
        g.alliance = row.alliance;
        g.lastCrest = row.lastCrest;
        g.crest = row.crest;
        g.booty = row.booty;
        g.mercenaryAttr = row.mercenaryAttr;
        for (GuildMemberRow& m : row.members) {
            Member member;
            member.character = m.character;
            member.name = m.name;
            member.memberClass = std::uint8_t(m.memberClass);
            member.level = std::uint8_t(std::clamp(m.level, 1, 255));
            member.donation = m.donation;
            member.permission = member.memberClass == game::kGuildMaster ? game::kGuildPermAll : m.permission;
            member.nickname = m.nickname;
            member.position = std::uint8_t(m.position);
            member.contribution = m.contribution;
            member.model = m.model;
            member.lastSeen = std::chrono::system_clock::now() - std::chrono::seconds(m.awaySeconds);
            memberOf[m.character] = g.id;
            g.members.push_back(std::move(member));
        }
        return g;
    }

    // --- Emblems ---------------------------------------------------------

    // The original's checks (a guild's, a
    // union's): in a guild (0x4C0D), its level 4 (a union's 5; 0x4C4A), its master (0x4C1E); a union's
    // also in a union (0x4C2F) whose leading guild it is (else 0x4C1E).
    GuildError CrestChecks(const GuildActor& a, std::uint8_t kind, Guild*& g, AllianceRow*& u) {
        g = GuildOfCharacter(a.character);
        if (g == nullptr) return GuildError::NoGuild;
        if (g->level < (kind == proto::kCrestGuild ? game::kGuildCrestLevel : game::kUnionCrestLevel)) return GuildError::LevelTooLow;
        const Member* m = g->Find(a.character);
        if (m == nullptr || m->memberClass != game::kGuildMaster) return GuildError::Denied;
        if (kind == proto::kCrestGuild) return GuildError::Ok;
        const auto it = g->alliance != 0 ? alliances.find(g->alliance) : alliances.end();
        if (it == alliances.end()) return GuildError::NotInUnion;
        if (it->second.guilds[0] != g->id) return GuildError::Denied;
        u = &it->second;
        return GuildError::Ok;
    }
    // An emblem changed: the members' own data (GuildInfo / UnionInfo), and every member's look for
    // those who see it.
    void CrestChanged(std::uint8_t kind, Guild& g, AllianceRow* u) {
        if (kind == proto::kCrestGuild) {
            ToGuild(g, Info(g));
            TellGuildAppearance(g);
            return;
        }
        TellUnion(u->id);
        for (std::uint32_t id : u->guilds) {
            if (const auto it = guilds.find(id); id != 0 && it != guilds.end()) TellGuildAppearance(it->second);
        }
    }

    void CrestRegister(const GuildActor& a, const proto::GuildCrestRegister& r) {
        const bool ofGuild = r.kind == proto::kCrestGuild;
        const proto::GuildAction action = ofGuild ? proto::GuildAction::CrestRegister : proto::GuildAction::UnionCrestRegister;
        if (r.kind > proto::kCrestUnion || r.pixels.size() != proto::kCrestBytes) return Answer(a.peer, action, GuildError::Failed);
        Guild* g = nullptr;
        AllianceRow* u = nullptr;
        if (const GuildError e = CrestChecks(a, r.kind, g, u); e != GuildError::Ok) return Answer(a.peer, action, e);
        std::uint32_t& current = ofGuild ? g->crest : u->crest;
        std::uint32_t& last = ofGuild ? g->lastCrest : u->lastCrest;
        if (current != 0) return Answer(a.peer, action, GuildError::CrestExists);
        // The item is used up only now (ours: the original's item use hands it to these checks).
        if (!host.TakeGuildItem(a.peer, r.slot, ofGuild ? "ITEM_ETC_GUILD_CREST_USER" : "ITEM_ETC_UNION_CREST_USER")) {
            return Answer(a.peer, action, GuildError::Failed);
        }
        current = last = last + 1;
        const std::uint32_t id = ofGuild ? g->id : u->id;
        crests[{r.kind, id}] = r.pixels;
        Write([kind = r.kind, id, revision = current, pixels = r.pixels](Database& db) { SaveCrest(db, kind, id, revision, revision, pixels); });
        Answer(a.peer, action, GuildError::Ok);
        core::LogInfo("guilds: {} registers the emblem of {} {} (revision {})", a.name, ofGuild ? "guild" : "union", id, current);
        CrestChanged(r.kind, *g, u);
    }

    void CrestDelete(const GuildActor& a, const proto::GuildCrestDelete& r) {
        const bool ofGuild = r.kind == proto::kCrestGuild;
        const proto::GuildAction action = ofGuild ? proto::GuildAction::CrestDelete : proto::GuildAction::UnionCrestDelete;
        if (r.kind > proto::kCrestUnion) return Answer(a.peer, action, GuildError::Failed);
        Guild* g = nullptr;
        AllianceRow* u = nullptr;
        if (const GuildError e = CrestChecks(a, r.kind, g, u); e != GuildError::Ok) return Answer(a.peer, action, e);
        std::uint32_t& current = ofGuild ? g->crest : u->crest;
        const std::uint32_t last = ofGuild ? g->lastCrest : u->lastCrest;
        if (current == 0) return Answer(a.peer, action, GuildError::NoCrest);
        if (!host.TakeGuildItem(a.peer, r.slot, ofGuild ? "ITEM_ETC_GUILD_CREST_DELETE" : "ITEM_ETC_UNION_CREST_DELETE")) {
            return Answer(a.peer, action, GuildError::Failed);
        }
        current = 0;
        const std::uint32_t id = ofGuild ? g->id : u->id;
        crests.erase({r.kind, id});
        Write([kind = r.kind, id, last](Database& db) { SaveCrest(db, kind, id, last, 0, {}); });
        Answer(a.peer, action, GuildError::Ok);
        core::LogInfo("guilds: {} deletes the emblem of {} {}", a.name, ofGuild ? "guild" : "union", id);
        CrestChanged(r.kind, *g, u);
    }

    // An emblem's pixels to whoever asks, while that revision is the current one.
    void CrestRequest(const GuildActor& a, const proto::GuildCrestRequest& r) {
        std::uint32_t current = 0;
        if (r.kind == proto::kCrestGuild) {
            if (const auto g = guilds.find(r.id); g != guilds.end()) current = g->second.crest;
        } else if (const auto u = alliances.find(r.id); u != alliances.end()) {
            current = u->second.crest;
        }
        const auto it = crests.find({r.kind, r.id});
        if (current == 0 || current != r.revision || it == crests.end()) return;
        Send(a.peer, proto::GuildCrest{r.kind, r.id, current, it->second});
    }

    void UnionLeaveRequest(const GuildActor& a) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::UnionLeave, e); };
        Guild* g = GuildOfCharacter(a.character);
        if (g != nullptr && host.GuildInFortressWar(g->id)) return refuse(GuildError::WarNoUnionLeave);
        if (g == nullptr) return refuse(GuildError::NoGuild);
        const Member* me = g->Find(a.character);
        if (me == nullptr || me->memberClass != game::kGuildMaster) return refuse(GuildError::Denied);
        if (g->alliance == 0) return refuse(GuildError::NotInUnion);
        LeaveUnion(*g, 2);
        Answer(a.peer, proto::GuildAction::UnionLeave, GuildError::Ok);
    }

    void UnionKickRequest(const GuildActor& a, const proto::UnionKick& r) {
        const auto refuse = [&](GuildError e) { Answer(a.peer, proto::GuildAction::UnionKick, e); };
        Guild* g = GuildOfCharacter(a.character);
        if (g != nullptr && host.GuildInFortressWar(g->id)) return refuse(GuildError::WarNoUnionKick);
        if (g == nullptr) return refuse(GuildError::NoGuild);
        const Member* me = g->Find(a.character);
        if (me == nullptr || me->memberClass != game::kGuildMaster) return refuse(GuildError::Denied);
        const auto alliance = alliances.find(g->alliance);
        if (g->alliance == 0 || alliance == alliances.end()) return refuse(GuildError::NotInUnion);
        if (alliance->second.guilds[0] != g->id) return refuse(GuildError::Denied);
        const auto& slots = alliance->second.guilds;
        const auto target = guilds.find(r.guild);
        if (r.guild == g->id || target == guilds.end() || std::find(slots.begin(), slots.end(), r.guild) == slots.end()) {
            return refuse(GuildError::NotInThisUnion);
        }
        LeaveUnion(target->second, 3);
        Answer(a.peer, proto::GuildAction::UnionKick, GuildError::Ok);
    }
};

Guilds::Guilds(GuildHost& host) : s_(std::make_unique<State>(host)) {}
Guilds::~Guilds() = default;

void Guilds::Load(GuildData data) {
    State& s = *s_;
    for (GuildRow& row : data.guilds) {
        State::Guild g = s.FromRow(row);
        s.names[Lower(g.name)] = g.id;
        s.guilds[g.id] = std::move(g);
    }
    for (const AllianceRow& a : data.alliances) s.alliances[a.id] = a;
    for (CrestRow& c : data.crests) {
        if (c.pixels.size() == proto::kCrestBytes) s.crests[{c.kind, c.id}] = std::move(c.pixels);
    }
    for (const GuildWarRow& w : data.wars) s.wars[w.id] = State::FromWarRow(w);
    const auto now = std::chrono::system_clock::now();
    for (const GuildPenaltyRow& p : data.penalties) s.penalties[p.character] = now + std::chrono::seconds(p.secondsLeft);
}

void Guilds::Entered(const GuildActor& a) {
    State& s = *s_;
    s.online[a.character] = {a.peer, a.entity};
    s.remoteOnline.erase(a.character);
    State::Guild* g = s.GuildOfCharacter(a.character);
    if (g == nullptr) return;
    s.PublishPresence(a.character, true);
    if (State::Member* m = g->Find(a.character)) {
        m->level = std::uint8_t(a.level);
        m->model = a.model;
        m->name = a.name;
        s.ToGuild(*g, proto::GuildMemberChanged{s.Record(*m)}, a.character);
    }
    s.Send(a.peer, s.Info(*g));
    if (g->alliance != 0) s.Send(a.peer, s.UnionOf(g->alliance));
    s.Send(a.peer, s.WarsOf(g->id));
    s.TellAppearance(a.character);
}

void Guilds::MovedAway(std::int64_t character) {
    State& s = *s_;
    s.online.erase(character);
    s.recallAsks.erase(character);
    s.remoteOnline.insert(character);
}

void Guilds::FortressPositionsReset(std::uint32_t guild) {
    State& s = *s_;
    const auto g = s.guilds.find(guild);
    if (g == s.guilds.end()) return;
    bool any = false;
    for (auto& m : g->second.members) {
        // Positions 4 and up (war, production, training, the engineers), as the database's reset; the Commander keeps his.
        if (m.position < 4) continue;
        m.position = game::kPositionNone;
        any = true;
        s.Write([guild, character = m.character](Database& db) { SaveGuildPosition(db, guild, character, game::kPositionNone); });
        s.ToGuild(g->second, proto::GuildMemberChanged{s.Record(m)});
    }
    if (!any) return;
    s.Touch(guild);
    s.PublishTouched();
}

void Guilds::RowsChanged(std::uint32_t guild) {
    State& s = *s_;
    const State::Touched around = s.Around(guild);
    std::vector<std::uint32_t> ids(around.guilds.begin(), around.guilds.end()), unions(around.alliances.begin(), around.alliances.end());
    if (ids.empty()) ids.push_back(guild);
    s.Reload(ids, unions);
    // The others load them too (no change of ours counted: this server's own load is under way).
    s.touched.guilds.insert(ids.begin(), ids.end());
    s.touched.alliances.insert(unions.begin(), unions.end());
    s.PublishTouched();
}

void Guilds::Left(std::int64_t character) {
    State& s = *s_;
    s.online.erase(character);
    s.recallAsks.erase(character);
    s.remoteOnline.erase(character);
    if (State::Guild* g = s.GuildOfCharacter(character)) {
        s.PublishPresence(character, false);
        if (State::Member* m = g->Find(character)) {
            m->lastSeen = std::chrono::system_clock::now();
            s.ToGuild(*g, proto::GuildMemberChanged{s.Record(*m)}, character);
        }
    }
}

void Guilds::PointsEarned(std::int64_t character, std::uint32_t points) {
    State& s = *s_;
    State::Guild* g = s.GuildOfCharacter(character);
    State::Member* m = g != nullptr ? g->Find(character) : nullptr;
    if (m == nullptr || points == 0) return;
    // As a donation of skill points, but the member keeps them.
    g->points += points;
    m->donation += points;
    s.SavePoints(*g, m->character, m->donation);
    s.ToGuild(*g, proto::GuildMemberChanged{s.Record(*m)});
    s.Touch(g->id);
    s.PublishTouched();
}

void Guilds::LevelChanged(std::int64_t character, int level) {
    State& s = *s_;
    State::Guild* g = s.GuildOfCharacter(character);
    State::Member* m = g != nullptr ? g->Find(character) : nullptr;
    if (m == nullptr) return;
    m->level = std::uint8_t(level);
    s.Write([character, level](Database& db) { SaveGuildMemberLevel(db, character, level); });
    s.ToGuild(*g, proto::GuildMemberChanged{s.Record(*m)});
    s.Touch(g->id);
    s.PublishTouched();
}

void Guilds::DevCreate(const GuildActor& actor, const std::string& name, int level) { s_->DevCreate(actor, name, level); }

bool Guilds::Handle(const GuildActor& actor, proto::MessageId id, std::span<const std::byte> payload) {
    State& s = *s_;
    // A request that wrote: the guilds it may have changed (the actor's before and after, their
    // unions' guilds) for the shard's other servers to load again once its writes are in.
    const std::uint32_t was = GuildOf(actor.character);
    const State::Touched before = s.Around(was);
    const std::uint64_t writes = s.mutations;
    const auto with = [&]<typename T>(T message, auto&& handle) {
        if (!proto::Decode(payload, message)) return false;
        handle(message);
        if (s.mutations != writes) {
            const std::uint32_t now = GuildOf(actor.character);
            // In a guild now (made or joined): online for the other servers too.
            if (now != 0 && now != was && s.online.contains(actor.character)) s.PublishPresence(actor.character, true);
            s.Mark(before);
            s.Touch(now);
            if constexpr (std::is_same_v<T, proto::UnionKick>) s.Touch(message.guild); // the guild put out
            s.PublishTouched();
        }
        return true;
    };
    switch (id) {
    case proto::GuildCreate::kId: return with(proto::GuildCreate{}, [&](const auto& m) { s.Create(actor, m); });
    case proto::GuildMercenaryAbility::kId: return with(proto::GuildMercenaryAbility{}, [&](const auto& m) { s.MercenaryAbility(actor, m); });
    case proto::GuildInvite::kId: return with(proto::GuildInvite{}, [&](const auto& m) { s.Invite(actor, m); });
    case proto::GuildProposalAnswer::kId: return with(proto::GuildProposalAnswer{}, [&](const auto& m) { s.AnswerProposal(actor, m); });
    case proto::GuildLeave::kId: return with(proto::GuildLeave{}, [&](const auto& m) { s.Leave(actor, m); });
    case proto::GuildKick::kId: return with(proto::GuildKick{}, [&](const auto& m) { s.Kick(actor, m); });
    case proto::GuildDisband::kId: return with(proto::GuildDisband{}, [&](const auto& m) { s.Disband(actor, m); });
    case proto::GuildNoticeEdit::kId: return with(proto::GuildNoticeEdit{}, [&](const auto& m) { s.Notice(actor, m); });
    case proto::GuildPermissions::kId: return with(proto::GuildPermissions{}, [&](const auto& m) { s.Permissions(actor, m); });
    case proto::GuildMasterTransfer::kId: return with(proto::GuildMasterTransfer{}, [&](const auto& m) { s.MasterTransfer(actor, m); });
    case proto::GuildNickname::kId: return with(proto::GuildNickname{}, [&](const auto& m) { s.Nickname(actor, m); });
    case proto::GuildPositionGrant::kId: return with(proto::GuildPositionGrant{}, [&](const auto& m) { s.PositionGrant(actor, m); });
    case proto::GuildGpHistoryOpen::kId: return with(proto::GuildGpHistoryOpen{}, [&](const auto& m) { s.GpHistory(actor, m); });
    case proto::GuildLevelUp::kId: return with(proto::GuildLevelUp{}, [&](const auto& m) { s.LevelUp(actor, m); });
    case proto::GuildDonate::kId: return with(proto::GuildDonate{}, [&](const auto& m) { s.Donate(actor, m); });
    case proto::UnionInvite::kId: return with(proto::UnionInvite{}, [&](const auto& m) { s.InviteUnion(actor, m); });
    case proto::UnionLeave::kId: return with(proto::UnionLeave{}, [&](const auto&) { s.UnionLeaveRequest(actor); });
    case proto::UnionKick::kId: return with(proto::UnionKick{}, [&](const auto& m) { s.UnionKickRequest(actor, m); });
    case proto::GuildCrestRegister::kId: return with(proto::GuildCrestRegister{}, [&](const auto& m) { s.CrestRegister(actor, m); });
    case proto::GuildCrestDelete::kId: return with(proto::GuildCrestDelete{}, [&](const auto& m) { s.CrestDelete(actor, m); });
    case proto::GuildCrestRequest::kId: return with(proto::GuildCrestRequest{}, [&](const auto& m) { s.CrestRequest(actor, m); });
    case proto::GuildElectionStart::kId: return with(proto::GuildElectionStart{}, [&](const auto& m) { s.ElectionStart(actor, m); });
    case proto::GuildElectionOpen::kId: return with(proto::GuildElectionOpen{}, [&](const auto& m) { s.ElectionOpen(actor, m); });
    case proto::GuildElectionVote::kId: return with(proto::GuildElectionVote{}, [&](const auto& m) { s.ElectionVote(actor, m); });
    case proto::GuildWarDeclare::kId: return with(proto::GuildWarDeclare{}, [&](const auto& m) { s.WarDeclare(actor, m); });
    case proto::GuildWarEnd::kId: return with(proto::GuildWarEnd{}, [&](const auto& m) { s.WarEnd(actor, m); });
    case proto::GuildWarRewardQuery::kId: return with(proto::GuildWarRewardQuery{}, [&](const auto& m) { s.WarReward(actor, m); });
    case proto::GuildWarRewardClaim::kId: return with(proto::GuildWarRewardClaim{}, [&](const auto& m) { s.WarClaim(actor, m); });
    case proto::GuildRecallAnswer::kId: return with(proto::GuildRecallAnswer{}, [&](const auto& m) { s.RecallAnswer(actor, m); });
    default: return false;
    }
}

proto::ChatResult Guilds::Chat(const GuildActor& actor, const proto::ChatLine& line) {
    State& s = *s_;
    State::Guild* g = s.GuildOfCharacter(actor.character);
    if (line.kind == proto::ChatKind::Guild) {
        if (g == nullptr) return proto::ChatResult::InvalidCommand;
        s.ToGuild(*g, line, actor.character);
        return proto::ChatResult::Ok;
    }
    // The union's: from a guild in a union, by its master or a member allowed to (bit 0x04;
    // 0x200C, 0x200E), to every member of its guilds online.
    if (g == nullptr || g->alliance == 0 || !s.alliances.contains(g->alliance)) return proto::ChatResult::NotInUnion;
    const State::Member* me = g->Find(actor.character);
    if (me == nullptr || !State::May(*me, game::kGuildPermUnionChat)) return proto::ChatResult::NoUnionRight;
    s.ToUnion(s.alliances[g->alliance], line, actor.character);
    return proto::ChatResult::Ok;
}

void Guilds::Tick(Clock::time_point now) {
    State& s = *s_;
    // Recalls not answered in time (the member's client says so itself: "Response time has passed").
    std::erase_if(s.recallAsks, [&](const auto& r) { return now >= r.second.until; });
    for (auto it = s.proposals.begin(); it != s.proposals.end();) {
        if (now < it->second.expires) {
            ++it;
            continue;
        }
        // Not answered in time: both told (0x4C10; a war's: "The leader of the selected guild is not responding.").
        if (it->second.kind == proto::kProposalWar) {
            if (const auto from = s.online.find(it->second.from); from != s.online.end()) {
                s.Answer(from->second.peer, proto::GuildAction::WarDeclare, GuildError::TimedOut);
            }
            if (const auto target = s.online.find(it->first); target != s.online.end()) {
                s.Answer(target->second.peer, proto::GuildAction::WarAnswer, GuildError::TimedOut);
            }
            it = s.proposals.erase(it);
            continue;
        }
        const bool guild = it->second.kind == proto::kProposalGuild;
        if (const auto inviter = s.online.find(it->second.from); inviter != s.online.end()) {
            s.Answer(inviter->second.peer, guild ? proto::GuildAction::Invite : proto::GuildAction::UnionInvite, GuildError::TimedOut);
        }
        if (const auto target = s.online.find(it->first); target != s.online.end()) {
            s.Answer(target->second.peer, guild ? proto::GuildAction::Join : proto::GuildAction::UnionJoin, GuildError::TimedOut);
        }
        it = s.proposals.erase(it);
    }
    // Wars whose time is up (the more points win; even: it goes on until a kill breaks it, the shard manager's timer
    // SM) and surrenders whose minute is over.
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> over;
        const auto wall = std::chrono::system_clock::now();
        for (const auto& [id, w] : s.wars) {
            if (w.surrender && now >= *w.surrender) {
                over.push_back({id, w.surrendering == w.guilds[0] ? w.guilds[1] : w.guilds[0]});
            } else if (w.ends && wall >= *w.ends && w.points[0] != w.points[1]) {
                over.push_back({id, w.points[0] > w.points[1] ? w.guilds[0] : w.guilds[1]});
            }
        }
        for (const auto& [id, winner] : over) s.EndWar(id, winner);
    }
    for (auto it = s.elections.begin(); it != s.elections.end();) {
        if (now < it->second.ends) {
            ++it;
            continue;
        }
        const State::Election e = std::move(it->second);
        const std::uint32_t id = it->first;
        it = s.elections.erase(it);
        if (const auto g = s.guilds.find(id); g != s.guilds.end()) {
            s.EndElection(g->second, e, 0); // time up without 60 %: cancelled
            s.Touch(id);
        }
    }
    s.PublishTouched();
}

std::uint32_t Guilds::GuildOf(std::int64_t character) const {
    const auto it = s_->memberOf.find(character);
    return it != s_->memberOf.end() ? it->second : 0;
}

bool Guilds::AtWar(std::int64_t a, std::int64_t b) const {
    const std::uint32_t ga = GuildOf(a), gb = GuildOf(b);
    return ga != 0 && gb != 0 && ga != gb && s_->WarBetween(ga, gb) != nullptr;
}

bool Guilds::TakeGuildPoints(std::uint32_t guild, std::uint32_t points) {
    State& s = *s_;
    const auto g = s.guilds.find(guild);
    if (g == s.guilds.end() || g->second.points < points) return false;
    if (points == 0) return true;
    g->second.points -= points;
    s.SavePoints(g->second, 0, 0);
    s.Touch(guild);
    s.PublishTouched();
    return true;
}

bool Guilds::AnyHolds(std::uint32_t guild, std::uint8_t position) const {
    const auto g = s_->guilds.find(guild);
    if (g == s_->guilds.end()) return false;
    return std::any_of(g->second.members.begin(), g->second.members.end(), [&](const auto& m) { return (m.position & position) != 0; });
}

std::uint8_t Guilds::PositionOf(std::int64_t character) const {
    State& s = *s_;
    State::Guild* g = s.GuildOfCharacter(character);
    const State::Member* m = g != nullptr ? g->Find(character) : nullptr;
    return m != nullptr ? m->position : std::uint8_t(0);
}

bool Guilds::AtAnyWar(std::int64_t character) const {
    const std::uint32_t g = GuildOf(character);
    return g != 0 && s_->Hostiles(g) > 0;
}

void Guilds::PlayerKilled(std::int64_t killer, std::int64_t victim, int killerLevel, int victimLevel) {
    State& s = *s_;
    s.Killed(killer, victim, killerLevel, victimLevel);
    s.PublishTouched();
}

Guilds::StorageAccess Guilds::StorageAccessOf(std::int64_t character) const {
    StorageAccess a;
    const auto of = s_->memberOf.find(character);
    const auto g = of != s_->memberOf.end() ? s_->guilds.find(of->second) : s_->guilds.end();
    if (g == s_->guilds.end()) {
        a.refusal = 0x4C0D;
        return a;
    }
    a.guild = g->first;
    a.level = g->second.level;
    if (a.level < 2) {
        a.refusal = 0x4C4A;
        return a;
    }
    const auto m = std::find_if(g->second.members.begin(), g->second.members.end(), [&](const auto& m) { return m.character == character; });
    if (m == g->second.members.end() || (m->memberClass != game::kGuildMaster && (m->permission & game::kGuildPermStorage) == 0)) a.refusal = 0x4C1E;
    return a;
}

Guilds::PointsPayment Guilds::CanPayPoints(std::int64_t character, std::uint32_t points) const {
    const auto of = s_->memberOf.find(character);
    const auto g = of != s_->memberOf.end() ? s_->guilds.find(of->second) : s_->guilds.end();
    if (g == s_->guilds.end()) return PointsPayment::NotMaster;
    const auto m = std::find_if(g->second.members.begin(), g->second.members.end(), [&](const auto& x) { return x.character == character; });
    if (m == g->second.members.end() || m->memberClass != game::kGuildMaster) return PointsPayment::NotMaster;
    return g->second.points < points ? PointsPayment::Short : PointsPayment::Ok;
}

void Guilds::SpendPoints(std::int64_t character, std::uint32_t points, std::uint8_t reason) {
    State& s = *s_;
    State::Guild* g = s.GuildOfCharacter(character);
    const State::Member* m = g != nullptr ? g->Find(character) : nullptr;
    if (m == nullptr || points == 0) return;
    g->points -= std::min(g->points, points);
    s.SavePoints(*g, 0, 0);
    s.Write([id = g->id, name = m->name, points, reason](Database& db) { AddGpHistory(db, id, name, points, reason); });
    s.Touch(g->id);
    s.PublishTouched();
}

std::optional<Guilds::Facts> Guilds::FactsOfGuild(std::uint32_t guild) const {
    const auto g = s_->guilds.find(guild);
    if (guild == 0 || g == s_->guilds.end()) return std::nullopt;
    Facts f{g->first, g->second.name, g->second.level, g->second.members.size(), g->second.alliance, false, {}, g->second.crest, g->second.points,
            g->second.mercenaryAttr};
    if (const auto a = g->second.alliance != 0 ? s_->alliances.find(g->second.alliance) : s_->alliances.end(); a != s_->alliances.end()) {
        f.unionLeader = a->second.guilds[0] == g->first;
    }
    for (const auto& m : g->second.members) {
        if (m.memberClass == game::kGuildMaster) f.masterName = m.name;
    }
    return f;
}

std::optional<Guilds::Facts> Guilds::FactsOf(std::int64_t character) const {
    const auto of = s_->memberOf.find(character);
    if (of == s_->memberOf.end()) return std::nullopt;
    std::optional<Facts> f = FactsOfGuild(of->second);
    if (!f) return f;
    const auto& members = s_->guilds.at(of->second).members;
    const auto m = std::find_if(members.begin(), members.end(), [&](const auto& m) { return m.character == character; });
    f->master = m != members.end() && m->memberClass == game::kGuildMaster;
    return f;
}

bool Guilds::PayWithPoints(std::uint32_t guild, std::uint32_t points, std::int64_t character, std::uint64_t fee, PaymentWork work, PaymentDone done) {
    State& s = *s_;
    const auto g = s.guilds.find(guild);
    if (g == s.guilds.end() || g->second.points < points) return false;
    g->second.points -= points;
    State* st = &s;
    s.WritePaid(character, fee,
                [work = std::move(work), guild, level = g->second.level, left = g->second.points](Database& tx) -> std::optional<std::uint64_t> {
                    if (!SaveGuildPoints(tx, guild, level, left, 0, 0)) return std::nullopt;
                    return work ? work(tx) : std::optional<std::uint64_t>(0);
                },
                [st, guild, points, done = std::move(done)](bool ok, std::uint64_t credit) {
                    if (const auto it = st->guilds.find(guild); it != st->guilds.end()) {
                        if (!ok) {
                            it->second.points += points;
                            st->SavePoints(it->second, 0, 0);
                        } else {
                            st->ToGuild(it->second, proto::GuildPoints{std::uint8_t(it->second.level), it->second.points});
                        }
                        st->Touch(guild);
                        st->PublishTouched();
                    }
                    if (done) done(ok, credit);
                });
    return true;
}

bool Guilds::MemberNamed(std::int64_t character, std::string_view name) const {
    const auto of = s_->memberOf.find(character);
    if (of == s_->memberOf.end()) return false;
    const auto g = s_->guilds.find(of->second);
    if (g == s_->guilds.end()) return false;
    const std::string key = Lower(std::string(name));
    return std::any_of(g->second.members.begin(), g->second.members.end(), [&](const auto& m) { return Lower(m.name) == key; });
}

std::size_t Guilds::Count() const { return s_->guilds.size(); }
bool Guilds::Writing() const { return s_->writing || !s_->writes.empty(); }

void Guilds::Hello() {
    BusEnvelope e;
    e.kind = kBusHello;
    s_->Publish(std::move(e));
}

void Guilds::BusMessage(const std::string& text) {
    State& s = *s_;
    const auto bytes = FromHex(text);
    BusEnvelope e;
    if (!bytes || !proto::Decode(*bytes, e)) {
        core::LogWarning("guilds: a bus message that does not decode ({} characters)", text.size());
        return;
    }
    if (e.from == s.host.GuildServer()) return; // our own, back
    const std::vector<std::byte> payload(reinterpret_cast<const std::byte*>(e.payload.data()),
                                         reinterpret_cast<const std::byte*>(e.payload.data()) + e.payload.size());
    switch (e.kind) {
    case kBusReload: s.Reload(e.guilds, e.alliances); return;
    case kBusForward:
        if (e.scope == kToCharacter) {
            if (const auto it = s.online.find(e.character); it != s.online.end()) s.host.GuildSend(it->second.peer, e.message, payload);
        } else if (const auto g = s.guilds.find(e.guild); g != s.guilds.end()) {
            s.ToGuildHere(g->second, e.message, payload, e.character);
        }
        return;
    case kBusPresence:
        if (e.value != 0) {
            s.remoteOnline.insert(e.character);
        } else {
            s.remoteOnline.erase(e.character);
        }
        return;
    case kBusHello:
        // Who of ours is online in a guild, for the newcomer.
        for (const auto& [character, o] : s.online) {
            if (s.memberOf.contains(character)) s.PublishPresence(character, true);
        }
        for (const auto& [guild, election] : s.elections) {
            BusEnvelope running;
            running.kind = kBusElection;
            running.guild = guild;
            running.value = 1;
            s.Publish(std::move(running));
        }
        return;
    case kBusElection:
        if (e.value != 0) {
            s.remoteElections[e.guild] = e.from;
        } else {
            s.remoteElections.erase(e.guild);
        }
        return;
    case kBusPenalty: s.penalties[e.character] = std::chrono::system_clock::now() + std::chrono::seconds(e.value); return;
    case kBusRecall: {
        // Another server's master recalls: its guild's members online here asked.
        proto::WorldPosition to;
        const auto g = s.guilds.find(e.guild);
        if (g != s.guilds.end() && proto::Decode(payload, to)) s.AskRecall(g->second, e.character, e.name, to, e.value);
        return;
    }
    case kBusWars: s.ReloadWars(); return;
    case kBusRequest: {
        if (e.value != s.host.GuildServer() || !s.elections.contains(e.guild)) return; // not ours to hold
        GuildActor actor;
        actor.character = e.character;
        actor.name = e.name;
        s.answerCharacter = e.character;
        Handle(actor, e.message, payload);
        s.answerCharacter = 0;
        return;
    }
    default: return;
    }
}
bool Guilds::Recall(const GuildActor& master, const proto::WorldPosition& to, std::uint32_t seconds) {
    State& s = *s_;
    const State::Guild* g = s.GuildOfCharacter(master.character);
    if (g == nullptr) return false;
    core::LogInfo("guilds: {} of '{}' recalls its members ({} s to answer)", master.name, g->name, seconds);
    s.AskRecall(*g, master.character, master.name, to, seconds);
    // The shard's other servers ask theirs (the original's relay 0x7030 through its shard manager).
    BusEnvelope e;
    e.kind = kBusRecall;
    e.guild = g->id;
    e.character = master.character;
    e.name = master.name;
    e.value = seconds;
    const std::vector<std::byte> place = proto::Encode(to);
    e.payload.resize(place.size());
    std::memcpy(e.payload.data(), place.data(), place.size());
    s.Publish(std::move(e));
    return true;
}

bool Guilds::RecallAsked(std::int64_t character) const { return s_->recallAsks.contains(character); }

void Guilds::SetElectionSeconds(int seconds) { s_->electionSeconds = std::max(1, seconds); }
void Guilds::SetWarEndSeconds(int seconds) { s_->warEndSeconds = std::max(0, seconds); }

} // namespace sr::server
