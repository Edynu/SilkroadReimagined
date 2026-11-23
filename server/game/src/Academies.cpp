#include "sr/server/Academies.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <iterator>
#include <ctime>
#include <map>
#include <unordered_map>

#include "sr/core/Log.h"
#include "sr/game/Academy.h"
#include "sr/server/AcademyStore.h"
#include "sr/server/Database.h"
#include "sr/server/ShardBus.h"

namespace sr::server {

namespace proto = game::proto;
namespace academy = game::academy;
namespace code = game::academy::code;

std::int64_t AcademyHost::AcademyNow() const { return std::int64_t(std::time(nullptr)); }

namespace {

enum BusKind : std::uint8_t {
    kBusHello = 1,
    kBusBye = 2,
    kBusReload = 3,
    kBusNews = 4,
    kBusPresence = 5,
    kBusChat = 6,
    kBusJoinAsk = 7,  // a join request for a listed academy, to its guardian's server (text: the guardian's character)
    kBusJoinDone = 8, // its end, to the applicant's server (a: 1 joined, 0 refused, 2 no reply; b: a refusal's code low byte)
    kBusRanks = 9,    // the honour ranking made again
    kBusBuff = 10,    // a received buff for a member playing on the server named (text: "server donor points")
    kBusPlace = 11,   // where a member is (text: "region x y z")
};

// What crosses the shard's bus (hex of its encoding).
struct Envelope {
    std::uint16_t from = 0;
    std::uint8_t kind = 0;
    std::uint32_t academy = 0;
    std::int64_t character = 0;
    std::uint8_t a = 0, b = 0;
    std::string name, text;
    template <typename V>
    void Visit(V& v) {
        v("from", from);
        v("kind", kind);
        v("academy", academy);
        v("character", character);
        v("a", a);
        v("b", b);
        v("name", name, 32);
        v("text", text, 512);
    }
};

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

struct Academies::State {
    struct Online {
        std::uint64_t peer = 0;
        std::uint32_t entity = 0;
        std::string name;
        int level = 1, maxLevel = 1;
        std::uint32_t academy = 0;
        bool loaded = false;
        std::map<std::int64_t, std::int64_t> waits; // job -> its end (Unix seconds)
        std::int64_t honorTold = -1; // the honour points the host has
        std::uint8_t rankWorn = 0;   // the honour buffs worn (0: not told yet, 5: none)
        // The experience buffs' slots: `start` the server's seconds (-1 never).
        struct Slot {
            int slot = 0;
            std::int64_t donor = 0;
            std::int64_t start = -1;
            std::uint32_t points = 0;
            academy::BuffType type = academy::BuffType::Idle;
            bool dirty = false;
        };
        std::vector<Slot> slots;
        bool accumulating = false; // the accumulation begun and not passed nor cleared 
        std::int64_t minute = -1;  // the last minute the slots were looked at
        proto::WorldPosition placeTold; // where its members were last told it is
    };
    struct Invite {
        std::uint32_t academy = 0;
        std::int64_t guardian = 0;
        std::uint64_t guardianPeer = 0;
        std::string guardianName, targetName;
        std::uint64_t targetPeer = 0;
        Clock::time_point until{};
    };

    AcademyHost& host;
    std::unordered_map<std::int64_t, Online> online;
    std::map<std::uint32_t, AcademyRow> academies; // those of this server's players
    std::map<std::int64_t, std::uint16_t> elsewhere; // members online on another game server
    std::map<std::int64_t, Invite> invites;         // by the one invited
    // Join requests from the matching list: those whose guardian plays here (by the guardian), and this server's applicants'.
    struct JoinIn {
        std::uint32_t academy = 0;
        std::int64_t applicant = 0;
        std::string name;
        int level = 1, maxLevel = 1;
        std::uint16_t from = 0; // the applicant's server (0: this one)
        Clock::time_point until{};
    };
    struct JoinOut {
        std::uint32_t academy = 0;
        Clock::time_point until{};
    };
    std::map<std::int64_t, JoinIn> joinsIn;
    std::map<std::int64_t, JoinOut> joinsOut;
    // Where members are, as last told (this server's and the others'), for the world map's marks.
    std::map<std::int64_t, proto::WorldPosition> places;
    Clock::time_point nextPlaces{};
    int day = -1; // the local day the honour ranking was last checked on
    int writes = 0;

    explicit State(AcademyHost& h) : host(h) {}

    std::uint16_t Self() const { return host.AcademyServer(); }
    std::int64_t Now() const { return host.AcademyNow(); }
    Online* Find(std::int64_t character) {
        const auto it = online.find(character);
        return it != online.end() ? &it->second : nullptr;
    }
    template <typename T>
    void Send(std::uint64_t peer, const T& message) {
        host.AcademySend(peer, T::kId, proto::Encode(message));
    }
    void Refuse(std::uint64_t peer, proto::MessageId request, std::uint16_t c, std::int64_t seconds = 0) {
        Send(peer, proto::AcademyRefusal{request, c, std::uint32_t(std::max<std::int64_t>(0, seconds))});
    }
    void Publish(Envelope e) {
        if (!host.AcademyOnBus()) return;
        e.from = Self();
        host.AcademyPublish(BusToHex(proto::Encode(e)));
    }
    // A wait still running: its seconds left (0: none).
    std::int64_t WaitLeft(const Online& o, std::int64_t job) const {
        const auto it = o.waits.find(job);
        return it == o.waits.end() ? 0 : std::max<std::int64_t>(0, it->second - Now());
    }
    void SetWait(Online& o, std::int64_t character, std::int64_t job, std::int64_t seconds) {
        const std::int64_t until = Now() + seconds;
        o.waits[job] = until;
        host.AcademySubmit([character, job, until](Database& db) { SaveAcademyWait(db, character, job, until); }, [] {});
    }
    void SetWaitOf(std::int64_t character, std::int64_t job, std::int64_t seconds) {
        if (Online* o = Find(character)) return SetWait(*o, character, job, seconds);
        const std::int64_t until = Now() + seconds;
        host.AcademySubmit([character, job, until](Database& db) { SaveAcademyWait(db, character, job, until); }, [] {});
    }

    bool IsOnline(std::int64_t character) const { return online.contains(character) || elsewhere.contains(character); }
    const AcademyMemberRow* Member(const AcademyRow& row, std::int64_t character) const {
        for (const AcademyMemberRow& m : row.members)
            if (m.character == character) return &m;
        return nullptr;
    }
    const AcademyMemberRow* Guardian(const AcademyRow& row) const {
        for (const AcademyMemberRow& m : row.members)
            if (m.memberClass == std::uint8_t(academy::Class::Guardian)) return &m;
        return nullptr;
    }
    int Count(const AcademyRow& row, academy::Class c) const {
        return int(std::count_if(row.members.begin(), row.members.end(), [&](const AcademyMemberRow& m) { return m.memberClass == std::uint8_t(c); }));
    }

    proto::AcademyInfo Info(std::uint32_t id) const {
        proto::AcademyInfo info;
        const auto it = academies.find(id);
        if (it == academies.end()) return info;
        const AcademyRow& row = it->second;
        info.id = row.id;
        info.rank = row.rank;
        info.graduates = std::uint32_t(row.graduates);
        info.evaluation = std::uint32_t(row.evaluation);
        info.founded = row.founded;
        info.noticeTitle = row.noticeTitle;
        info.notice = row.notice;
        info.matchEntry = row.matchEntry;
        info.matchKind = row.matchKind;
        info.matchTitle = row.matchTitle;
        for (const AcademyMemberRow& m : row.members) {
            proto::AcademyMember x;
            x.character = std::uint32_t(m.character);
            x.name = m.name;
            x.model = m.model;
            x.memberClass = m.memberClass;
            int level = m.level, maxLevel = m.maxLevel;
            if (const auto o = online.find(m.character); o != online.end()) level = o->second.level, maxLevel = std::max(maxLevel, o->second.maxLevel);
            x.level = std::uint8_t(std::clamp(level, 0, 255));
            x.maxLevel = std::uint8_t(std::clamp(maxLevel, 0, 255));
            x.grade = academy::GradeOf(academy::Class(m.memberClass), maxLevel);
            x.honor = std::uint32_t(std::max<std::int64_t>(0, m.honor));
            x.online = IsOnline(m.character) ? 1 : 0;
            info.members.push_back(std::move(x));
        }
        return info;
    }
    // This server's members of the academy told it as it is.
    void TellInfo(std::uint32_t id) {
        const proto::AcademyInfo info = Info(id);
        for (auto& [character, o] : online) {
            if (o.academy != id || !o.loaded) continue;
            Send(o.peer, info);
            RefreshHonor(character, o);
        }
    }
    // The host's honour points and the guardian's honour buffs as the player's academy row has them (none outside one).
    void RefreshHonor(std::int64_t character, Online& o) {
        std::int64_t honor = 0;
        std::uint8_t rank = std::uint8_t(academy::kNoRank);
        if (const auto a = o.academy != 0 ? academies.find(o.academy) : academies.end(); a != academies.end()) {
            if (const AcademyMemberRow* m = Member(a->second, character)) {
                honor = std::max<std::int64_t>(0, m->honor);
                if (m->memberClass == std::uint8_t(academy::Class::Guardian)) rank = a->second.rank;
            }
        }
        if (o.honorTold != honor) {
            o.honorTold = honor;
            host.AcademyHonorChanged(o.peer, honor);
        }
        if (o.rankWorn != rank) {
            o.rankWorn = rank;
            host.AcademyHonorBuffs(o.peer, rank);
        }
    }
    // A line to this server's members of the academy (`subject` the one it names: its "self" form), then to the others'.
    void News(std::uint32_t id, proto::AcademyNewsKind kind, const std::string& name, std::uint8_t cls, std::int64_t subject, bool publish) {
        for (const auto& [character, o] : online) {
            if ((o.academy != id || !o.loaded) && character != subject) continue;
            Send(o.peer, proto::AcademyNews{kind, name, cls, std::uint8_t(character == subject ? 1 : 0)});
        }
        if (!publish) return;
        Envelope e;
        e.kind = kBusNews;
        e.academy = id;
        e.character = subject;
        e.a = std::uint8_t(kind);
        e.b = cls;
        e.name = name;
        Publish(std::move(e));
    }
    void PublishReload(std::uint32_t id) {
        Envelope e;
        e.kind = kBusReload;
        e.academy = id;
        Publish(std::move(e));
    }
    void PublishPresence(std::int64_t character, std::uint32_t id, bool on) {
        Envelope e;
        e.kind = kBusPresence;
        e.academy = id;
        e.character = character;
        e.a = on ? 1 : 0;
        Publish(std::move(e));
    }
    // An academy loaded again from the database; this server's players moved in or out of it by its rows; told.
    void Reload(std::uint32_t id, std::function<void()> after = {}) {
        ++writes;
        auto row = std::make_shared<std::optional<AcademyRow>>();
        host.AcademySubmit(
            [id, row](Database& db) {
                bool ok = false;
                *row = LoadAcademy(db, id, ok);
            },
            [this, id, row, after = std::move(after)] {
                --writes;
                if (*row) academies[id] = std::move(**row);
                else academies.erase(id);
                for (auto& [character, o] : online) {
                    const bool member = *row && Member(academies[id], character) != nullptr;
                    if (member && o.academy != id) o.academy = id;
                    if (!member && o.academy == id) {
                        o.academy = 0;
                        DropSlots(o);
                        Send(o.peer, proto::AcademyInfo{});
                        RefreshHonor(character, o);
                    }
                    if (member) PruneDonors(character, o);
                }
                if (!*row) academies.erase(id);
                TellInfo(id);
                Forget(id);
                if (after) after();
            });
    }
    // An academy none of this server's players belongs to any more: dropped.
    void Forget(std::uint32_t id) {
        for (const auto& [character, o] : online)
            if (o.academy == id) return;
        academies.erase(id);
    }
    void Changed(std::uint32_t id, std::function<void()> after = {}) {
        Reload(id, std::move(after));
        PublishReload(id);
    }

    // --- Where the members are (the world map's marks) -----------------------------------------------------------

    // A member's place to this server's members of `id` (not itself).
    void TellPlace(std::uint32_t id, std::int64_t character, const proto::WorldPosition& at) {
        places[character] = at;
        for (const auto& [other, o] : online) {
            if (other == character || o.academy != id || !o.loaded) continue;
            Send(o.peer, proto::AcademyPlace{std::uint32_t(character), at});
        }
    }
    // Every 5 s (ours; the original's with the party's position news): this server's members moved more
    // than 10 since last told, to their academy everywhere.
    void Places(Clock::time_point now) {
        if (now < nextPlaces) return;
        nextPlaces = now + std::chrono::seconds(5);
        for (auto& [character, o] : online) {
            if (!o.loaded || o.academy == 0) continue;
            const auto at = host.AcademyPositionOf(o.entity);
            if (!at) continue;
            const proto::FlatPosition a = proto::ToFlat(*at), b = proto::ToFlat(o.placeTold);
            if (o.placeTold.region != 0 && at->region == o.placeTold.region && std::hypot(a.x - b.x, a.z - b.z) <= 10.0) continue;
            o.placeTold = *at;
            TellPlace(o.academy, character, *at);
            Envelope e;
            e.kind = kBusPlace;
            e.academy = o.academy;
            e.character = character;
            e.text = std::format("{} {} {} {}", at->region, at->x, at->y, at->z);
            Publish(std::move(e));
        }
    }
    // The members' places known, to one who just came (its academy loaded).
    void TellPlaces(std::int64_t character, const Online& o) {
        const auto a = academies.find(o.academy);
        if (a == academies.end()) return;
        for (const AcademyMemberRow& m : a->second.members) {
            if (m.character == character || !IsOnline(m.character)) continue;
            if (const auto p = places.find(m.character); p != places.end()) Send(o.peer, proto::AcademyPlace{std::uint32_t(m.character), p->second});
        }
    }

    // --- The experience buffs ---------------------------------------------------------

    using Slot = Online::Slot;
    std::optional<std::uint8_t> ClassOf(std::int64_t character, const Online& o) const {
        const auto a = o.academy != 0 ? academies.find(o.academy) : academies.end();
        if (a == academies.end()) return std::nullopt;
        const AcademyMemberRow* m = Member(a->second, character);
        return m != nullptr ? std::optional<std::uint8_t>(m->memberClass) : std::nullopt;
    }
    static Slot* SlotAt(Online& o, int slot) {
        for (Slot& s : o.slots)
            if (s.slot == slot) return &s;
        return nullptr;
    }
    // Its day's minutes left (0: over).
    int MinutesLeft(const Slot& s) const {
        if (s.start < 0) return 0;
        return std::max<int>(0, academy::kBuffMinutes - int((Now() - s.start) / 60));
    }
    static void Clear(Slot& s, academy::BuffType type) {
        s.donor = 0;
        s.start = -1;
        s.points = 0;
        s.type = type;
        s.dirty = true;
    }
    void SendAccumulation(const Online& o, std::uint8_t kind, std::uint32_t minutes, std::uint32_t points) {
        Send(o.peer, proto::AcademyAccumulation{kind, minutes, points});
    }
    void SendReserve(const Online& o, std::uint8_t kind, const Slot& s, std::uint32_t minutes) {
        Send(o.peer, proto::AcademyReserve{kind, {{std::uint32_t(s.donor), minutes, s.points}}});
    }
    // The slots changed written (each a row of _TrainingCampBuffStatus).
    void SaveSlots(std::int64_t character, Online& o) {
        std::vector<AcademyBuffRow> rows;
        for (Slot& s : o.slots) {
            if (!s.dirty) continue;
            s.dirty = false;
            rows.push_back({s.slot, s.donor, s.start < 0 ? -1 : std::max<std::int64_t>(0, Now() - s.start), s.points, std::uint8_t(s.type)});
        }
        if (rows.empty()) return;
        host.AcademySubmit(
            [character, rows](Database& db) {
                for (const AcademyBuffRow& r : rows)
                    if (!SaveAcademyBuff(db, character, r)) core::LogWarning("academy: buff slot {} of {} not saved", r.slot, character);
            },
            [] {});
    }
    // At the entry: the buffs received still running (0xB483 kind 1), then the accumulation as it stands
    // (0x3C82 kind 1, or 7 when its day is over). A buff from one no longer of the academy is gone.
    void TellBuffs(std::int64_t character, Online& o) {
        const auto a = academies.find(o.academy);
        proto::AcademyReserve list{1, {}};
        for (Slot& s : o.slots) {
            if (s.slot == 0 || s.type != academy::BuffType::Received) continue;
            const int left = MinutesLeft(s);
            if (left == 0 || a == academies.end() || Member(a->second, s.donor) == nullptr) {
                Clear(s, academy::BuffType::Empty);
                continue;
            }
            list.entries.push_back({std::uint32_t(s.donor), std::uint32_t(left), s.points});
        }
        Send(o.peer, list);
        if (Slot* s0 = SlotAt(o, 0); s0 != nullptr && o.accumulating) {
            const int left = s0->type == academy::BuffType::Accumulating ? MinutesLeft(*s0) : 0;
            if (left == 0 && s0->type != academy::BuffType::Over) {
                s0->type = academy::BuffType::Over;
                s0->dirty = true;
            }
            SendAccumulation(o, left > 0 ? 1 : 7, std::uint32_t(left), s0->points);
        }
        SaveSlots(character, o);
    }
    // The slots of one whose academy is gone (its rows are deleted with its membership).
    static void DropSlots(Online& o) {
        o.slots.clear();
        o.accumulating = false;
    }
    // Buffs from members who left, taken off (the original's kind 9: "MemberDeleted Mentor's ExpBuff Deactivate").
    void PruneDonors(std::int64_t character, Online& o) {
        const auto a = academies.find(o.academy);
        if (a == academies.end()) return;
        for (Slot& s : o.slots) {
            if (s.slot == 0 || s.type != academy::BuffType::Received || Member(a->second, s.donor) != nullptr) continue;
            SendReserve(o, 9, s, 0);
            Clear(s, academy::BuffType::Empty);
        }
        SaveSlots(character, o);
    }
    // A buff received (kind 0): an apprentice's accumulation ends (kind 5); the slot its
    // donor holds, else an empty one, held for a day with `points`.
    bool Receive(std::int64_t character, Online& o, std::int64_t donor, std::uint32_t points) {
        const auto cls = ClassOf(character, o);
        if (!cls || *cls == std::uint8_t(academy::Class::Assistant) || donor == 0) return false;
        if (*cls == std::uint8_t(academy::Class::Apprentice)) {
            if (Slot* s0 = SlotAt(o, 0)) Clear(*s0, academy::BuffType::Idle);
            o.accumulating = false;
            SendAccumulation(o, 5, 0, 0);
        }
        Slot* to = nullptr;
        for (Slot& s : o.slots)
            if (s.slot > 0 && s.donor == donor && s.type == academy::BuffType::Received) to = &s;
        for (Slot& s : o.slots)
            if (to == nullptr && s.slot > 0 && s.type == academy::BuffType::Empty) to = &s;
        if (to == nullptr) {
            core::LogWarning("academy: no slot for {}'s buff from {}", character, donor);
            SaveSlots(character, o);
            return false;
        }
        to->donor = donor;
        to->start = Now();
        to->points = points;
        to->type = academy::BuffType::Received;
        to->dirty = true;
        SendReserve(o, 0, *to, std::uint32_t(academy::kBuffMinutes));
        SaveSlots(character, o);
        return true;
    }
    // A buff for a member playing elsewhere or nowhere: its server's, else the database's.
    void ReceiveFar(std::int64_t character, std::int64_t donor, std::uint32_t points) {
        if (const auto it = elsewhere.find(character); it != elsewhere.end()) {
            Envelope e;
            e.kind = kBusBuff;
            e.character = character;
            e.text = std::format("{} {} {}", it->second, donor, points);
            return Publish(std::move(e));
        }
        host.AcademySubmit([character, donor, points](Database& db) { ReceiveAcademyBuff(db, character, donor, points); }, [] {});
    }
    // The pass (0x7483): a member not the guardian whose accumulation began an hour ago; the guardian must be
    // on (the shard manager: 0xB483 kind 2, UIIT_MSG_TC_PASS_ERROR); the guardian then a buff of 0.2 of the points, the
    // apprentice one of 0.8 (SM).
    void Pass(const AcademyActor& a) {
        Online* o = Find(a.character);
        if (o == nullptr || !o->loaded || o->academy == 0) return;
        const auto cls = ClassOf(a.character, *o);
        Slot* s0 = SlotAt(*o, 0);
        // Ours: only an accumulation begun (the original passes a slot never begun too, whose time is 2000-01-01).
        if (!cls || *cls == std::uint8_t(academy::Class::Guardian) || s0 == nullptr || !o->accumulating) return;
        if (s0->start >= 0 && Now() - s0->start < std::int64_t(academy::kPassAfterMinutes) * 60) return;
        const AcademyMemberRow* g = Guardian(academies.at(o->academy));
        if (g == nullptr || !IsOnline(g->character)) return Send(o->peer, proto::AcademyReserve{2, {}});
        const std::uint32_t points = s0->points;
        const std::uint32_t toGuardian = academy::ShareOf(points, academy::kGuardianShare);
        const std::uint32_t toSelf = academy::ShareOf(points, academy::kApprenticeShare);
        core::LogInfo("academy: {} passes {} points ({} to the guardian, {} back)", a.name, points, toGuardian, toSelf);
        if (Online* go = Find(g->character); go != nullptr && go->loaded) Receive(g->character, *go, a.character, toGuardian);
        else ReceiveFar(g->character, a.character, toGuardian);
        Receive(a.character, *o, g->character, toSelf);
    }
    // A kill (-> _Helper): an apprentice below 40 begins its
    // accumulation at its first kill (kind 0) and adds the kill's points while it runs; then the first buff received
    // spends 10 of its points (gone at 0: kind 5).
    int Kill(std::int64_t character, double ofWhole) {
        Online* o = Find(character);
        if (o == nullptr || !o->loaded || o->academy == 0 || o->slots.empty()) return 0;
        const auto cls = ClassOf(character, *o);
        if (!cls || *cls == std::uint8_t(academy::Class::Assistant)) return 0;
        if (*cls == std::uint8_t(academy::Class::Apprentice)) {
            if (o->level >= academy::kAccumulateBelowLevel) return 0;
            if (Slot* s0 = SlotAt(*o, 0); s0 != nullptr && !(o->accumulating && s0->type == academy::BuffType::Over)) {
                if (!o->accumulating) {
                    const AcademyMemberRow* g = Guardian(academies.at(o->academy));
                    o->accumulating = true;
                    s0->type = academy::BuffType::Accumulating;
                    s0->start = Now();
                    s0->donor = g != nullptr ? g->character : 0;
                    s0->points = 0;
                    s0->dirty = true;
                    SendAccumulation(*o, 0, std::uint32_t(academy::kBuffMinutes), 0);
                }
                if (s0->type == academy::BuffType::Accumulating) {
                    s0->points += std::uint32_t(academy::KillPoints(ofWhole));
                    s0->dirty = true;
                }
            }
        }
        int spent = 0;
        for (Slot& s : o->slots) {
            if (s.slot == 0 || s.type != academy::BuffType::Received) continue;
            spent = int(std::min<std::uint32_t>(academy::kSpendPerKill, s.points));
            s.points -= std::uint32_t(spent);
            s.dirty = true;
            if (s.points == 0) {
                SendReserve(*o, 5, s, 0);
                Clear(s, academy::BuffType::Empty);
                SaveSlots(character, *o);
            }
            break;
        }
        return spent;
    }
    // Each minute: the accumulation's day over (kind 7), a received buff's (kind 5); the
    // minutes told at every tenth and the last ten; the slots written.
    void Minute(std::int64_t character, Online& o) {
        if (Slot* s0 = SlotAt(o, 0); s0 != nullptr && o.accumulating && s0->type == academy::BuffType::Accumulating) {
            const int left = MinutesLeft(*s0);
            if (left == 0) {
                s0->type = academy::BuffType::Over;
                s0->dirty = true;
                SendAccumulation(o, 7, 0, s0->points);
            } else if (academy::TellMinutes(left)) {
                SendAccumulation(o, 10, std::uint32_t(left), 0);
            }
            s0->dirty = true; // its points kept as they grow
        }
        for (Slot& s : o.slots) {
            if (s.slot == 0 || s.type != academy::BuffType::Received) continue;
            const int left = MinutesLeft(s);
            if (left == 0) {
                SendReserve(o, 5, s, 0);
                Clear(s, academy::BuffType::Empty);
            } else if (academy::TellMinutes(left)) {
                SendReserve(o, 10, s, std::uint32_t(left));
            }
        }
        SaveSlots(character, o);
    }
    // An apprentice reaching level 40: its
    // accumulation and its received buffs end (kind 8).
    void ReachedForty(std::int64_t character, Online& o) {
        if (Slot* s0 = SlotAt(o, 0); s0 != nullptr && o.accumulating) {
            Clear(*s0, academy::BuffType::Idle);
            o.accumulating = false;
            SendAccumulation(o, 8, 0, 0);
        }
        for (Slot& s : o.slots) {
            if (s.slot == 0 || s.type != academy::BuffType::Received) continue;
            SendReserve(o, 8, s, 0);
            Clear(s, academy::BuffType::Empty);
        }
        SaveSlots(character, o);
    }

    // --- The requests --------------------------------------------------------------------------------------------

    void Found(const AcademyActor& a) {
        Online* o = Find(a.character);
        if (o == nullptr || !o->loaded) return Refuse(a.peer, proto::AcademyFound::kId, code::kFailed);
        if (a.dead) return Refuse(a.peer, proto::AcademyFound::kId, code::kDead);
        if (a.level < academy::kFoundLevel) return Refuse(a.peer, proto::AcademyFound::kId, code::kLevel);
        if (a.gold < academy::kFoundGold) return Refuse(a.peer, proto::AcademyFound::kId, code::kGold);
        if (o->academy != 0) return Refuse(a.peer, proto::AcademyFound::kId, code::kOtherAcademy);
        if (const std::int64_t left = WaitLeft(*o, academy::kWaitFound)) return Refuse(a.peer, proto::AcademyFound::kId, code::kFoundWait, left);
        // The academy's rows and the 100,000 in one transaction (the original charges once _TRAINING_CAMP_CREATE has stood).
        ++writes;
        auto made = std::make_shared<std::uint32_t>(0);
        auto result = std::make_shared<std::pair<bool, AcademyWrite>>(false, AcademyWrite::Failed); // ran, FoundAcademy's answer
        const std::int64_t character = a.character;
        const std::uint64_t peer = a.peer;
        const std::string name = a.name;
        const int level = a.level, maxLevel = a.maxLevel;
        host.AcademyPay(
            character, academy::kFoundGold,
            [character, level, maxLevel, made, result](Database& tx) -> std::optional<std::uint64_t> {
                result->first = true;
                std::string error;
                result->second = FoundAcademy(tx, character, level, maxLevel, *made, error);
                if (result->second != AcademyWrite::Ok) {
                    core::LogWarning("academy: founding for {}: {}", character, error);
                    return std::nullopt;
                }
                return 0;
            },
            [this, character, peer, name, made, result](bool ok, std::uint64_t) {
                --writes;
                if (!ok) {
                    return Refuse(peer, proto::AcademyFound::kId,
                                  !result->first                               ? code::kGold
                                  : result->second == AcademyWrite::AlreadyMember ? code::kOtherAcademy
                                                                                  : code::kFailed);
                }
                core::LogInfo("academy: {} founds academy {}", name, *made);
                if (Online* x = Find(character)) x->academy = *made;
                Reload(*made, [this, made, name, character] { News(*made, proto::AcademyNewsKind::Founded, name, 0, character, false); });
            });
    }

    // The guardian's own checks: alive, a member, the guardian.
    std::uint16_t GuardianCheck(const AcademyActor& a, const Online& o) const {
        if (a.dead) return code::kDead;
        if (o.academy == 0 || !academies.contains(o.academy)) return code::kInvalidTarget;
        const AcademyMemberRow* me = Member(academies.at(o.academy), a.character);
        if (me == nullptr || me->memberClass != std::uint8_t(academy::Class::Guardian)) return code::kNotGuardian;
        return 0;
    }

    void Disband(const AcademyActor& a) {
        Online* o = Find(a.character);
        if (o == nullptr) return;
        if (const std::uint16_t c = GuardianCheck(a, *o)) return Refuse(a.peer, proto::AcademyDisband::kId, c);
        const AcademyRow& row = academies.at(o->academy);
        // Seven days after its founding.
        if (const std::int64_t left = row.founded + academy::kWeek - Now(); left > 0) return Refuse(a.peer, proto::AcademyDisband::kId, code::kDisbandWait, left);
        const std::uint32_t id = o->academy;
        const std::int64_t character = a.character;
        const std::uint64_t peer = a.peer;
        ++writes;
        auto result = std::make_shared<AcademyWrite>(AcademyWrite::Failed);
        host.AcademySubmit(
            [id, character, result](Database& db) {
                std::string error;
                *result = DisbandAcademy(db, id, character, error);
                if (*result != AcademyWrite::Ok) core::LogWarning("academy: disbanding {}: {}", id, error);
            },
            [this, id, character, peer, result] {
                --writes;
                if (*result != AcademyWrite::Ok) return Refuse(peer, proto::AcademyDisband::kId, code::kFailed);
                core::LogInfo("academy: {} disbanded", id);
                // Seven days before its guardian founds another (2/7).
                SetWaitOf(character, academy::kWaitFound, academy::kWeek);
                News(id, proto::AcademyNewsKind::Disbanded, {}, 0, 0, true);
                Changed(id);
            });
    }

    static constexpr double kInviteReach = 600.0; // the original's reach of a player (CGObjChar_CheckHitRange)
    void Invite(const AcademyActor& a, std::uint32_t entity, Clock::time_point now) {
        Online* o = Find(a.character);
        if (o == nullptr) return;
        const proto::MessageId req = proto::AcademyInvite::kId;
        if (const std::uint16_t c = GuardianCheck(a, *o)) return Refuse(a.peer, req, c);
        if (entity == a.entity) return Refuse(a.peer, req, code::kInvalidTarget);
        if (const std::int64_t left = WaitLeft(*o, academy::kWaitRecruit)) return Refuse(a.peer, req, code::kRecruitWait, left);
        const std::optional<AcademyActor> t = host.AcademyPlayerAt(entity);
        Online* to = t ? Find(t->character) : nullptr;
        if (!t || to == nullptr || !to->loaded) return Refuse(a.peer, req, code::kInvalidTarget);
        // Within the original's reach of a player (600, the same layer).
        const auto here = host.AcademyPositionOf(a.entity), there = host.AcademyPositionOf(entity);
        if (!here || !there || here->layer != there->layer) return Refuse(a.peer, req, code::kInvalidTarget);
        const proto::FlatPosition pa = proto::ToFlat(*here), pb = proto::ToFlat(*there);
        if (std::hypot(pa.x - pb.x, pa.z - pb.z) > kInviteReach) return Refuse(a.peer, req, code::kInvalidTarget);
        // The places by its level now (below 40 an apprentice's, 40..59 an assistant's, 60 and up none); the database
        // stores the class by its highest level (_TrainingCamp_FnAddMember).
        const AcademyRow& row = academies.at(o->academy);
        const academy::Class byLevel = academy::ClassOfLevel(t->level);
        if (byLevel == academy::Class::Guardian) return Refuse(a.peer, req, code::kInvalidTarget);
        if (byLevel == academy::Class::Assistant && Count(row, byLevel) >= academy::kMostAssistants) return Refuse(a.peer, req, code::kAssistantsFull);
        if (byLevel == academy::Class::Apprentice && Count(row, byLevel) >= academy::kMostApprentices) return Refuse(a.peer, req, code::kApprenticesFull);
        // The one invited: not dealing, alive, no rejoining wait, in no academy.
        if (t->busy) return Refuse(a.peer, req, code::kBusy);
        if (t->dead) return Refuse(a.peer, req, code::kInvalidTarget);
        if (WaitLeft(*to, academy::kWaitRejoinAssistant) > 0 || WaitLeft(*to, academy::kWaitRejoinApprentice) > 0) return Refuse(a.peer, req, code::kRejoinWait);
        if (to->academy == o->academy) return Refuse(a.peer, req, code::kAlreadyMember);
        if (to->academy != 0) return Refuse(a.peer, req, code::kOtherAcademy);
        if (t->jobChange) return Refuse(a.peer, req, code::kJobChange);
        const academy::Class cls = academy::ClassOfLevel(t->maxLevel);
        if (cls == academy::Class::Guardian) return Refuse(a.peer, req, code::kFailed);
        if (invites.contains(t->character)) return Refuse(a.peer, req, code::kFailed);
        invites[t->character] = {o->academy, a.character, a.peer, a.name, t->name, t->peer, now + std::chrono::seconds(academy::kInviteSeconds)};
        Send(t->peer, proto::AcademyAsk{a.name});
        Send(a.peer, proto::AcademyNews{proto::AcademyNewsKind::Pending, t->name, std::uint8_t(cls), 0});
        core::LogInfo("academy: {} invites {} into academy {}", a.name, t->name, o->academy);
    }

    void Answer(const AcademyActor& a, bool accept) {
        const auto it = invites.find(a.character);
        if (it == invites.end()) return;
        const struct Invite inv = it->second;
        invites.erase(it);
        if (!accept) return Refuse(inv.guardianPeer, proto::AcademyInvite::kId, code::kRefused);
        const std::int64_t character = a.character;
        const std::uint64_t peer = a.peer;
        const std::string name = a.name;
        const int level = a.level, maxLevel = a.maxLevel;
        ++writes;
        auto result = std::make_shared<AcademyWrite>(AcademyWrite::Failed);
        host.AcademySubmit(
            [inv, character, level, maxLevel, result](Database& db) {
                std::string error;
                *result = AddAcademyMember(db, inv.academy, character, level, maxLevel, error);
                if (*result == AcademyWrite::Failed) core::LogWarning("academy: adding {} to {}: {}", character, inv.academy, error);
            },
            [this, inv, character, peer, name, result] {
                --writes;
                if (*result != AcademyWrite::Ok) {
                    const std::uint16_t c = *result == AcademyWrite::AlreadyMember    ? code::kOtherAcademy
                                            : *result == AcademyWrite::AssistantsFull  ? code::kAssistantsFull
                                            : *result == AcademyWrite::ApprenticesFull ? code::kApprenticesFull
                                            : *result == AcademyWrite::NoAcademy       ? code::kNoAcademy
                                                                                       : code::kFailed;
                    Refuse(inv.guardianPeer, proto::AcademyInvite::kId, c);
                    return Refuse(peer, proto::AcademyAnswer::kId, c);
                }
                core::LogInfo("academy: {} joins academy {}", name, inv.academy);
                if (Online* x = Find(character)) x->academy = inv.academy;
                Changed(inv.academy, [this, inv, name, character] {
                    const AcademyRow* row = academies.contains(inv.academy) ? &academies.at(inv.academy) : nullptr;
                    const AcademyMemberRow* m = row != nullptr ? Member(*row, character) : nullptr;
                    News(inv.academy, proto::AcademyNewsKind::Joined, name, m != nullptr ? m->memberClass : 0, character, true);
                });
            });
    }

    // A member out: a ban by the guardian (`ban`) or its own leave.
    void Remove(const AcademyActor& a, const std::string& name, bool ban) {
        Online* o = Find(a.character);
        if (o == nullptr) return;
        const proto::MessageId req = ban ? proto::AcademyBan::kId : proto::AcademyLeave::kId;
        // No life asked (the original's); outside an academy 0x680B.
        if (o->academy == 0 || !academies.contains(o->academy)) return Refuse(a.peer, req, code::kNoAcademy);
        const AcademyRow& row = academies.at(o->academy);
        const AcademyMemberRow* me = Member(row, a.character);
        const bool isGuardian = me != nullptr && me->memberClass == std::uint8_t(academy::Class::Guardian);
        const AcademyMemberRow* target = nullptr;
        if (ban) {
            if (!isGuardian) return Refuse(a.peer, req, code::kNotGuardian);
            for (const AcademyMemberRow& m : row.members)
                if (Lower(m.name) == Lower(name)) target = &m;
            if (target == nullptr) return Refuse(a.peer, req, code::kNotMember);
            if (target->character == a.character) return Refuse(a.peer, req, code::kNotGuardian);
            // "To ban an apprentice, delete the registration on the matching list first" (0x6829).
            if (row.matchEntry != 0) return Refuse(a.peer, req, code::kBanWhileListed);
        } else {
            // The guardian does not leave (he disbands): 0x6816.
            if (isGuardian) return Refuse(a.peer, req, code::kNotGuardian);
            target = me;
        }
        if (target == nullptr) return Refuse(a.peer, req, code::kNotMember);
        // 24 hours after joining, neither (0x6820, 0x6821).
        if (const std::int64_t left = target->joined + academy::kDay - Now(); left > 0) return Refuse(a.peer, req, ban ? code::kBanWait : code::kLeaveWait, left);
        const std::uint32_t id = o->academy;
        const std::int64_t who = target->character, guardian = a.character;
        const std::uint8_t cls = target->memberClass;
        const std::string whoName = target->name;
        const std::uint64_t peer = a.peer;
        ++writes;
        auto result = std::make_shared<AcademyWrite>(AcademyWrite::Failed);
        host.AcademySubmit(
            [id, who, ban, result](Database& db) {
                std::string error;
                *result = RemoveAcademyMember(db, id, who, ban, error);
                if (*result == AcademyWrite::Failed) core::LogWarning("academy: removing {} from {}: {}", who, id, error);
            },
            [this, id, who, guardian, cls, whoName, peer, ban, req, result] {
                --writes;
                if (*result != AcademyWrite::Ok) return Refuse(peer, req, code::kFailed);
                core::LogInfo("academy: {} {} academy {}", whoName, ban ? "is banned from" : "leaves", id);
                if (ban) {
                    // A ban of an apprentice: the guardian recruits nobody for 24 hours ("excluding guardian assistant").
                    if (cls == std::uint8_t(academy::Class::Apprentice)) SetWaitOf(guardian, academy::kWaitRecruit, academy::kDay);
                } else if (cls == std::uint8_t(academy::Class::Assistant)) {
                    SetWaitOf(who, academy::kWaitRejoinAssistant, academy::kRejoinAssistantSeconds);
                } else {
                    SetWaitOf(who, academy::kWaitRejoinApprentice, academy::kRejoinApprenticeSeconds);
                }
                News(id, ban ? proto::AcademyNewsKind::Banned : proto::AcademyNewsKind::Left, whoName, cls, who, true);
                Changed(id);
            });
    }

    // --- Graduation ----------------------------------------------------------------------

    // A member graduates (a member, not the guardian, an apprentice from level 40 rating its guardian 0..4, an
    // assistant from 60), or the guardian graduates one (an apprentice past 40, an assistant past 60; no rating,
    // no reward).
    void Graduate(const AcademyActor& a, std::uint8_t rating, const std::string& name, bool forced) {
        Online* o = Find(a.character);
        if (o == nullptr) return;
        const proto::MessageId req = forced ? proto::AcademyGraduateForce::kId : proto::AcademyGraduate::kId;
        if (o->academy == 0 || !academies.contains(o->academy)) return Refuse(a.peer, req, code::kNoAcademy);
        const AcademyRow& row = academies.at(o->academy);
        const AcademyMemberRow* me = Member(row, a.character);
        const AcademyMemberRow* target = nullptr;
        int evaluation = 0;
        if (forced) {
            if (me == nullptr || me->memberClass != std::uint8_t(academy::Class::Guardian)) return Refuse(a.peer, req, code::kNotGuardian);
            for (const AcademyMemberRow& m : row.members)
                if (Lower(m.name) == Lower(name)) target = &m;
            if (target == nullptr) return Refuse(a.peer, req, code::kNotMember);
            if (target->character == a.character) return Refuse(a.peer, req, code::kWrongTarget);
            const bool apprentice = target->memberClass == std::uint8_t(academy::Class::Apprentice);
            if (target->maxLevel <= (apprentice ? academy::kApprenticeGraduates : academy::kAssistantGraduates)) return Refuse(a.peer, req, code::kForcedLevel);
        } else {
            if (me == nullptr) return Refuse(a.peer, req, code::kNotMember);
            if (me->memberClass == std::uint8_t(academy::Class::Guardian)) return Refuse(a.peer, req, code::kWrongTarget);
            target = me;
            const bool apprentice = me->memberClass == std::uint8_t(academy::Class::Apprentice);
            if (a.maxLevel < (apprentice ? academy::kApprenticeGraduates : academy::kAssistantGraduates)) return Refuse(a.peer, req, code::kGraduateLevel);
            if (apprentice) {
                if (rating >= std::size(academy::kRatingPoints)) return Refuse(a.peer, req, code::kWrongTarget);
                evaluation = academy::kRatingPoints[rating];
            }
        }
        const std::uint32_t id = o->academy;
        const std::int64_t who = target->character;
        const std::string whoName = target->name;
        const std::uint64_t peer = a.peer;
        ++writes;
        auto cls = std::make_shared<std::uint8_t>(0);
        auto result = std::make_shared<AcademyWrite>(AcademyWrite::Failed);
        host.AcademySubmit(
            [id, who, evaluation, cls, result](Database& db) {
                std::string error;
                *result = GraduateAcademyMember(db, id, who, evaluation, *cls, error);
                if (*result == AcademyWrite::Failed) core::LogWarning("academy: graduating {} from {}: {}", who, id, error);
            },
            [this, id, who, whoName, peer, req, forced, cls, result] {
                --writes;
                if (*result != AcademyWrite::Ok) return Refuse(peer, req, *result == AcademyWrite::NoAcademy ? code::kNoAcademy : code::kNotMember);
                core::LogInfo("academy: {} graduates from academy {}{}", whoName, id, forced ? " (forced)" : "");
                // A self-graduating apprentice's experience (as itself and as an apprentice).
                if (!forced && *cls == std::uint8_t(academy::Class::Apprentice)) host.AcademyGiveExperience(peer, academy::kGraduateExperience);
                News(id, proto::AcademyNewsKind::Graduated, whoName, *cls, who, true);
                Changed(id);
            });
    }

    // --- The notice and the ranking -------------------------------------------------------------------------------

    void Notice(const AcademyActor& a, const std::string& title, const std::string& text) {
        Online* o = Find(a.character);
        if (o == nullptr) return;
        const proto::MessageId req = proto::AcademyNoticeWrite::kId;
        if (const std::uint16_t c = GuardianCheck(a, *o)) return Refuse(a.peer, req, c);
        if (title.size() > academy::kNoticeTitleMax || text.size() > academy::kNoticeTextMax) return Refuse(a.peer, req, code::kFailed);
        // The forbidden words (on both).
        if (!host.AcademyTextAllowed(title) || !host.AcademyTextAllowed(text)) return Refuse(a.peer, req, code::kFailed);
        const std::uint32_t id = o->academy;
        const std::uint64_t peer = a.peer;
        ++writes;
        auto ok = std::make_shared<bool>(false);
        host.AcademySubmit([id, title, text, ok](Database& db) { *ok = SaveAcademyNotice(db, id, title, text); },
                           [this, id, peer, req, ok] {
                               --writes;
                               if (!*ok) return Refuse(peer, req, code::kFailed);
                               Changed(id, [this, id] { News(id, proto::AcademyNewsKind::NoticeChanged, {}, 0, 0, true); });
                           });
    }

    void Ranking(const AcademyActor& a, std::uint32_t npc) {
        if (!host.AcademyNpcNear(a.entity, npc)) return Refuse(a.peer, proto::AcademyRankingAsk::kId, code::kInvalidTarget);
        const std::uint64_t peer = a.peer;
        auto rows = std::make_shared<std::vector<AcademyRankEntry>>();
        host.AcademySubmit([rows](Database& db) { *rows = LoadAcademyRanking(db); },
                           [this, peer, rows] {
                               proto::AcademyRanking answer;
                               for (const AcademyRankEntry& e : *rows) {
                                   answer.rows.push_back({std::uint8_t(e.place), e.rank, e.guardian, std::uint8_t(std::clamp(e.level, 0, 255)),
                                                          std::uint32_t(std::max<std::int64_t>(0, e.graduates)), e.guild});
                               }
                               Send(peer, answer);
                           });
    }

    // The daily ranking (the shard's first server to see the day turn makes it); then every server's academies' ranks again.
    void CheckRanking(bool force) {
        ++writes;
        auto ran = std::make_shared<bool>(false);
        host.AcademySubmit(
            [force, ran](Database& db) {
                std::string error;
                *ran = UpdateAcademyHonorRanking(db, force, error);
                if (!error.empty()) core::LogWarning("academy: the honour ranking: {}", error);
            },
            [this, ran] {
                --writes;
                if (!*ran) return;
                core::LogInfo("academy: the honour ranking made again");
                ApplyRanks();
                Envelope e;
                e.kind = kBusRanks;
                Publish(std::move(e));
            });
    }
    void ApplyRanks() {
        ++writes;
        auto ranks = std::make_shared<std::map<std::uint32_t, std::uint8_t>>();
        host.AcademySubmit([ranks](Database& db) { *ranks = LoadAcademyRanks(db); },
                           [this, ranks] {
                               --writes;
                               for (auto& [id, row] : academies) {
                                   const auto it = ranks->find(id);
                                   row.rank = it != ranks->end() ? it->second : std::uint8_t(academy::kNoRank);
                               }
                               for (auto& [id, row] : academies) TellInfo(id);
                           });
    }

    // --- Matching ------------------------------------------------------------------------

    void MatchSave(const AcademyActor& a, std::uint8_t kind, const std::string& title, bool modify) {
        Online* o = Find(a.character);
        if (o == nullptr) return;
        const proto::MessageId req = modify ? proto::AcademyMatchModify::kId : proto::AcademyMatchRegister::kId;
        if (a.dead) return Refuse(a.peer, req, code::kDead);
        if (!modify && a.jobChange) return Refuse(a.peer, req, code::kJobChange);
        if (o->academy == 0 || !academies.contains(o->academy)) return Refuse(a.peer, req, code::kNotPosition);
        if (const std::uint16_t c = GuardianCheck(a, *o)) return Refuse(a.peer, req, c);
        // The recruiting ban (a banned apprentice's 24 hours) keeps it off the list too: no join by matching meanwhile.
        if (!modify) {
            if (const std::int64_t left = WaitLeft(*o, academy::kWaitRecruit)) return Refuse(a.peer, req, code::kRecruitWait, left);
        }
        if (title.empty() || title.size() > academy::kMatchTitleMax || !host.AcademyTextAllowed(title)) return Refuse(a.peer, req, code::kFailed);
        const AcademyRow& row = academies.at(o->academy);
        // The class it takes must have a place (the client's own checks).
        if (kind == std::uint8_t(academy::MatchKind::Assistants)) {
            if (Count(row, academy::Class::Assistant) >= academy::kMostAssistants) return Refuse(a.peer, req, code::kAssistantsFull);
        } else if (kind == std::uint8_t(academy::MatchKind::Apprentices)) {
            if (Count(row, academy::Class::Apprentice) >= academy::kMostApprentices) return Refuse(a.peer, req, code::kApprenticesFull);
        } else {
            return Refuse(a.peer, req, code::kFailed);
        }
        const std::uint32_t id = o->academy;
        const std::uint64_t peer = a.peer;
        ++writes;
        auto result = std::make_shared<AcademyWrite>(AcademyWrite::Failed);
        host.AcademySubmit(
            [id, kind, title, modify, result](Database& db) {
                std::string error;
                *result = SaveAcademyMatch(db, id, kind, title, modify, error);
                if (*result == AcademyWrite::Failed) core::LogWarning("academy: matching entry of {}: {}", id, error);
            },
            [this, id, peer, req, modify, result] {
                --writes;
                if (*result != AcademyWrite::Ok) return Refuse(peer, req, *result == AcademyWrite::NotMember ? code::kNoInformation : code::kFailed);
                Send(peer, proto::AcademyNews{modify ? proto::AcademyNewsKind::MatchChanged : proto::AcademyNewsKind::MatchEntered, {}, 0, 1});
                Changed(id);
            });
    }

    void MatchDelete(const AcademyActor& a) {
        Online* o = Find(a.character);
        if (o == nullptr) return;
        if (const std::uint16_t c = GuardianCheck(a, *o)) return Refuse(a.peer, proto::AcademyMatchDelete::kId, c);
        Unlist(o->academy, a.peer, proto::AcademyNewsKind::MatchDeleted);
    }
    // The academy's entry off the list; `peer` told (`kind`: deleted, or pulled when full).
    void Unlist(std::uint32_t id, std::uint64_t peer, proto::AcademyNewsKind kind) {
        ++writes;
        auto ok = std::make_shared<bool>(false);
        host.AcademySubmit([id, ok](Database& db) { *ok = DeleteAcademyMatch(db, id); },
                           [this, id, peer, kind, ok] {
                               --writes;
                               if (!*ok) return Refuse(peer, proto::AcademyMatchDelete::kId, code::kNoInformation);
                               if (peer != 0) Send(peer, proto::AcademyNews{kind, {}, 0, 1});
                               Changed(id);
                           });
    }

    void MatchList(const AcademyActor& a, const proto::AcademyMatchListAsk& ask) {
        const std::uint64_t peer = a.peer;
        auto pages = std::make_shared<int>(1);
        auto rows = std::make_shared<std::vector<AcademyMatchEntry>>();
        host.AcademySubmit(
            [ask, pages, rows](Database& db) { *rows = LoadAcademyMatches(db, ask.page, academy::kMatchRows, ask.honorClass, ask.kind, ask.name, *pages); },
            [this, peer, ask, pages, rows] {
                proto::AcademyMatchList list;
                list.page = ask.page;
                list.pages = std::uint8_t(std::clamp(*pages, 1, 255));
                for (const AcademyMatchEntry& e : *rows) {
                    list.rows.push_back({e.id, std::uint8_t(std::clamp(e.level, 0, 255)), e.race, e.guardian, e.title,
                                         std::uint32_t(std::max<std::int64_t>(0, e.graduates)), e.members, e.rank, e.kind});
                }
                Send(peer, list);
            });
    }

    // A player asks to join a listed academy: its own checks (as an invitation's target), the entry's kind against its
    // class (0x682A), then the guardian asked wherever it plays.
    void MatchJoin(const AcademyActor& a, std::uint32_t entry, Clock::time_point now) {
        Online* o = Find(a.character);
        if (o == nullptr || !o->loaded) return;
        const proto::MessageId req = proto::AcademyMatchJoin::kId;
        if (a.dead) return Refuse(a.peer, req, code::kDead);
        if (a.busy) return Refuse(a.peer, req, code::kBusy);
        if (a.jobChange) return Refuse(a.peer, req, code::kJobChange);
        if (o->academy != 0) return Refuse(a.peer, req, code::kOtherAcademy);
        if (WaitLeft(*o, academy::kWaitRejoinAssistant) > 0 || WaitLeft(*o, academy::kWaitRejoinApprentice) > 0) return Refuse(a.peer, req, code::kRejoinWait);
        if (joinsOut.contains(a.character)) return Refuse(a.peer, req, code::kFailed);
        const academy::Class cls = academy::ClassOfLevel(a.maxLevel);
        const std::int64_t character = a.character;
        const std::uint64_t peer = a.peer;
        const std::string name = a.name;
        const int level = a.level, maxLevel = a.maxLevel;
        joinsOut[character] = {0, now + std::chrono::seconds(academy::kInviteSeconds + 5)};
        auto found = std::make_shared<std::optional<AcademyMatchEntry>>();
        host.AcademySubmit([entry, found](Database& db) { *found = FindAcademyMatch(db, entry); },
                           [this, character, peer, name, level, maxLevel, cls, found] {
                               const auto out = joinsOut.find(character);
                               if (out == joinsOut.end()) return;
                               if (!*found) {
                                   joinsOut.erase(out);
                                   return Refuse(peer, proto::AcademyMatchJoin::kId, code::kNoInformation);
                               }
                               const AcademyMatchEntry& e = **found;
                               const bool fits = (cls == academy::Class::Apprentice && e.kind == std::uint8_t(academy::MatchKind::Apprentices)) ||
                                                 (cls == academy::Class::Assistant && e.kind == std::uint8_t(academy::MatchKind::Assistants));
                               if (!fits) {
                                   joinsOut.erase(out);
                                   return Refuse(peer, proto::AcademyMatchJoin::kId, code::kJoinKind);
                               }
                               out->second.academy = e.academy;
                               Send(peer, proto::AcademyMatchProgress{1});
                               if (online.contains(e.guardianCharacter)) return AskGuardian(e.guardianCharacter, e.academy, character, name, level, maxLevel, 0);
                               Envelope env;
                               env.kind = kBusJoinAsk;
                               env.academy = e.academy;
                               env.character = character;
                               env.a = std::uint8_t(std::clamp(level, 0, 255));
                               env.b = std::uint8_t(std::clamp(maxLevel, 0, 255));
                               env.name = name;
                               env.text = std::to_string(e.guardianCharacter);
                               Publish(std::move(env));
                           });
    }
    // The guardian (on this server) asked "[%s] has requested for Academy registration".
    void AskGuardian(std::int64_t guardian, std::uint32_t id, std::int64_t applicant, const std::string& name, int level, int maxLevel, std::uint16_t from) {
        Online* g = Find(guardian);
        if (g == nullptr || joinsIn.contains(guardian)) return JoinDone(from, id, applicant, 0, 0);
        joinsIn[guardian] = {id, applicant, name, level, maxLevel, from, Clock::now() + std::chrono::seconds(academy::kInviteSeconds)};
        Send(g->peer, proto::AcademyAsk{name, 1, std::uint8_t(std::clamp(level, 0, 255))});
    }
    // A join request's end for the applicant, here or on its server (`how`: 1 joined, 0 refused, 2 no reply).
    void JoinDone(std::uint16_t server, std::uint32_t id, std::int64_t applicant, std::uint8_t how, std::uint16_t refusal) {
        if (server != 0 && server != Self()) {
            Envelope e;
            e.kind = kBusJoinDone;
            e.academy = id;
            e.character = applicant;
            e.a = how;
            e.b = std::uint8_t(refusal & 0xFF);
            e.text = std::to_string(server);
            return Publish(std::move(e));
        }
        joinsOut.erase(applicant);
        Online* o = Find(applicant);
        if (o == nullptr) return;
        Send(o->peer, proto::AcademyMatchProgress{0});
        if (how == 1) {
            o->academy = id;
            Send(o->peer, proto::AcademyNews{proto::AcademyNewsKind::JoinComplete, {}, 0, 1});
            Reload(id);
        } else if (how == 2) {
            Send(o->peer, proto::AcademyNews{proto::AcademyNewsKind::NoReply, {}, 0, 1});
        } else if (refusal != 0) {
            Refuse(o->peer, proto::AcademyMatchJoin::kId, std::uint16_t(0x6800 | refusal));
        } else {
            Send(o->peer, proto::AcademyNews{proto::AcademyNewsKind::JoinCancelled, {}, 0, 1});
        }
    }
    // The guardian's answer: the applicant added (the class's places checked again in the database), the entry taken off the
    // list when that class is now full ("PULL_DELETE").
    void MatchReply(const AcademyActor& a, bool accept) {
        const auto it = joinsIn.find(a.character);
        if (it == joinsIn.end()) return;
        const JoinIn join = it->second;
        joinsIn.erase(it);
        if (!accept) return JoinDone(join.from, join.academy, join.applicant, 0, 0);
        const std::uint64_t peer = a.peer;
        ++writes;
        auto result = std::make_shared<AcademyWrite>(AcademyWrite::Failed);
        host.AcademySubmit(
            [join, result](Database& db) {
                std::string error;
                *result = AddAcademyMember(db, join.academy, join.applicant, join.level, join.maxLevel, error);
                if (*result == AcademyWrite::Failed) core::LogWarning("academy: adding {} to {}: {}", join.applicant, join.academy, error);
            },
            [this, join, peer, result] {
                --writes;
                if (*result != AcademyWrite::Ok) {
                    const std::uint16_t c = *result == AcademyWrite::AlreadyMember    ? code::kOtherAcademy
                                            : *result == AcademyWrite::AssistantsFull  ? code::kAssistantsFull
                                            : *result == AcademyWrite::ApprenticesFull ? code::kApprenticesFull
                                                                                       : code::kFailed;
                    Refuse(peer, proto::AcademyMatchReply::kId, c);
                    return JoinDone(join.from, join.academy, join.applicant, 0, std::uint16_t(c & 0xFF));
                }
                core::LogInfo("academy: {} joins academy {} from the matching list", join.name, join.academy);
                JoinDone(join.from, join.academy, join.applicant, 1, 0);
                Changed(join.academy, [this, join, peer] {
                    const auto a = academies.find(join.academy);
                    if (a == academies.end()) return;
                    const AcademyMemberRow* m = Member(a->second, join.applicant);
                    const std::uint8_t cls = m != nullptr ? m->memberClass : std::uint8_t(0);
                    News(join.academy, proto::AcademyNewsKind::Joined, join.name, cls, join.applicant, true);
                    const AcademyRow& row = a->second;
                    const bool full = row.matchEntry != 0 &&
                                      ((row.matchKind == std::uint8_t(academy::MatchKind::Apprentices) &&
                                        Count(row, academy::Class::Apprentice) >= academy::kMostApprentices) ||
                                       (row.matchKind == std::uint8_t(academy::MatchKind::Assistants) &&
                                        Count(row, academy::Class::Assistant) >= academy::kMostAssistants));
                    if (full) Unlist(join.academy, peer, proto::AcademyNewsKind::MatchPulled);
                });
            });
    }
};

Academies::Academies(AcademyHost& host) : s_(std::make_unique<State>(host)) {}
Academies::~Academies() = default;

void Academies::Hello() {
    Envelope e;
    e.kind = kBusHello;
    s_->Publish(std::move(e));
}

void Academies::RankNow() { s_->CheckRanking(true); }

std::string Academies::GoodbyeText() const {
    if (!s_->host.AcademyOnBus()) return {};
    Envelope e;
    e.from = s_->Self();
    e.kind = kBusBye;
    return BusToHex(proto::Encode(e));
}

void Academies::BusMessage(const std::string& text) {
    State& s = *s_;
    const auto bytes = BusFromHex(text);
    Envelope e;
    if (!bytes || !proto::Decode(*bytes, e) || e.from == s.Self()) return;
    switch (e.kind) {
    case kBusHello:
        // A server (re)started: who of ours is online, again.
        for (const auto& [character, o] : s.online)
            if (o.academy != 0) s.PublishPresence(character, o.academy, true);
        return;
    case kBusBye: {
        std::vector<std::uint32_t> touched;
        for (auto it = s.elsewhere.begin(); it != s.elsewhere.end();) {
            if (it->second != e.from) {
                ++it;
                continue;
            }
            it = s.elsewhere.erase(it);
        }
        for (const auto& [id, row] : s.academies) touched.push_back(id);
        for (const std::uint32_t id : touched) s.TellInfo(id);
        return;
    }
    case kBusReload:
        if (s.academies.contains(e.academy)) s.Reload(e.academy);
        return;
    case kBusNews:
        s.News(e.academy, proto::AcademyNewsKind(e.a), e.name, e.b, e.character, false);
        return;
    case kBusPresence:
        if (e.a != 0) s.elsewhere[e.character] = e.from;
        else if (const auto it = s.elsewhere.find(e.character); it != s.elsewhere.end() && it->second == e.from) s.elsewhere.erase(it);
        if (s.academies.contains(e.academy)) s.TellInfo(e.academy);
        return;
    case kBusJoinAsk: {
        // A join request for a guardian playing here.
        const std::int64_t guardian = std::strtoll(e.text.c_str(), nullptr, 10);
        if (s.online.contains(guardian)) s.AskGuardian(guardian, e.academy, e.character, e.name, e.a, e.b, e.from);
        return;
    }
    case kBusJoinDone:
        if (std::strtol(e.text.c_str(), nullptr, 10) == s.Self()) s.JoinDone(0, e.academy, e.character, e.a, e.b);
        return;
    case kBusRanks: s.ApplyRanks(); return;
    case kBusPlace: {
        proto::WorldPosition at;
        unsigned region = 0;
        if (std::sscanf(e.text.c_str(), "%u %f %f %f", &region, &at.x, &at.y, &at.z) != 4) return;
        at.region = std::uint16_t(region);
        if (s.academies.contains(e.academy)) s.TellPlace(e.academy, e.character, at);
        return;
    }
    case kBusBuff: {
        // A received buff for a member said to play here: given, else kept in the database.
        unsigned server = 0;
        long long donor = 0, points = 0;
        if (std::sscanf(e.text.c_str(), "%u %lld %lld", &server, &donor, &points) != 3 || server != s.Self()) return;
        if (State::Online* o = s.Find(e.character); o != nullptr && o->loaded) {
            s.Receive(e.character, *o, donor, std::uint32_t(points));
        } else {
            const std::int64_t character = e.character;
            s.host.AcademySubmit([character, donor, points](Database& db) { ReceiveAcademyBuff(db, character, donor, points); }, [] {});
        }
        return;
    }
    case kBusChat:
        for (const auto& [character, o] : s.online) {
            if (o.academy != e.academy || !o.loaded) continue;
            proto::ChatLine line;
            line.kind = proto::ChatKind::Academy;
            line.name = e.name;
            line.text = e.text;
            s.Send(o.peer, line);
        }
        return;
    default: return;
    }
}

void Academies::Tick(Clock::time_point now) {
    State& s = *s_;
    for (auto it = s.invites.begin(); it != s.invites.end();) {
        if (now < it->second.until) {
            ++it;
            continue;
        }
        // "Registration has been cancelled due to the late response time." to both.
        s.Send(it->second.guardianPeer, proto::AcademyNews{proto::AcademyNewsKind::NoReply, it->second.targetName, 0, 0});
        s.Send(it->second.targetPeer, proto::AcademyNews{proto::AcademyNewsKind::NoReply, it->second.guardianName, 0, 1});
        it = s.invites.erase(it);
    }
    // Join requests: the guardian's box gone and the applicant told; an applicant whose guardian never answered.
    for (auto it = s.joinsIn.begin(); it != s.joinsIn.end();) {
        if (now < it->second.until) {
            ++it;
            continue;
        }
        if (State::Online* g = s.Find(it->first)) s.Send(g->peer, proto::AcademyNews{proto::AcademyNewsKind::NoReply, it->second.name, 0, 0});
        const State::JoinIn join = it->second;
        it = s.joinsIn.erase(it);
        s.JoinDone(join.from, join.academy, join.applicant, 2, 0);
    }
    for (auto it = s.joinsOut.begin(); it != s.joinsOut.end();) {
        if (now < it->second.until) {
            ++it;
            continue;
        }
        const std::int64_t applicant = it->first;
        it = s.joinsOut.erase(it);
        if (State::Online* o = s.Find(applicant)) {
            s.Send(o->peer, proto::AcademyMatchProgress{0});
            s.Send(o->peer, proto::AcademyNews{proto::AcademyNewsKind::NoReply, {}, 0, 1});
        }
    }
    s.Places(now);
    // The experience buffs each minute.
    const std::int64_t minute = s.Now() / 60;
    for (auto& [character, o] : s.online) {
        if (!o.loaded || o.academy == 0 || o.slots.empty() || o.minute == minute) continue;
        const bool first = o.minute < 0;
        o.minute = minute;
        if (!first) s.Minute(character, o);
    }
    // The honour ranking once a day (_TRAINING_CAMP_STARTUP_HONOR_UPDATE_NEEDED: at the start, then as the day turns).
    const std::time_t t = std::time_t(s.Now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    if (local.tm_yday != s.day) {
        s.day = local.tm_yday;
        s.CheckRanking(false);
    }
}

void Academies::Entered(const AcademyActor& actor, bool arrived) {
    State& s = *s_;
    State::Online& o = s.online[actor.character];
    o.peer = actor.peer;
    o.entity = actor.entity;
    o.name = actor.name;
    o.level = actor.level;
    o.maxLevel = actor.maxLevel;
    o.loaded = false;
    s.elsewhere.erase(actor.character);
    struct Loaded {
        std::uint32_t academy = 0;
        std::optional<AcademyRow> row;
        std::map<std::int64_t, std::int64_t> waits;
        std::vector<AcademyBuffRow> buffs;
    };
    auto got = std::make_shared<Loaded>();
    const std::int64_t character = actor.character;
    const int level = actor.level, maxLevel = actor.maxLevel;
    ++s.writes;
    s.host.AcademySubmit(
        [character, level, maxLevel, got](Database& db) {
            bool ok = false;
            got->waits = LoadAcademyWaits(db, character, ok);
            got->academy = ::sr::server::AcademyOf(db, character, ok);
            if (got->academy != 0) {
                SaveAcademyLevels(db, character, level, maxLevel);
                got->row = LoadAcademy(db, got->academy, ok);
                got->buffs = LoadAcademyBuffs(db, character, ok);
            }
        },
        [this, character, arrived, got] {
            State& s = *s_;
            --s.writes;
            State::Online* o = s.Find(character);
            if (o == nullptr) return;
            o->waits = std::move(got->waits);
            o->academy = got->row ? got->academy : 0;
            o->loaded = true;
            if (got->row) s.academies[got->academy] = std::move(*got->row);
            if (o->academy == 0) {
                s.RefreshHonor(character, *o);
                return s.Send(o->peer, proto::AcademyInfo{});
            }
            o->slots.clear();
            for (const AcademyBuffRow& r : got->buffs) {
                State::Slot slot;
                slot.slot = r.slot;
                slot.donor = r.donor;
                slot.start = r.since < 0 ? -1 : s.Now() - r.since;
                slot.points = std::uint32_t(std::clamp<std::int64_t>(r.points, 0, 0xFFFFFFFF));
                slot.type = game::academy::BuffType(r.type);
                o->slots.push_back(slot);
            }
            if (const State::Slot* s0 = State::SlotAt(*o, 0)) {
                o->accumulating = s0->type == game::academy::BuffType::Accumulating || s0->type == game::academy::BuffType::Over;
            }
            s.TellInfo(o->academy);
            s.TellBuffs(character, *o);
            s.TellPlaces(character, *o);
            s.PublishPresence(character, o->academy, true);
            if (arrived) return;
            const AcademyMemberRow* m = s.Member(s.academies.at(o->academy), character);
            // "%s [%s] has logged on to the game." to the others.
            for (const auto& [other, x] : s.online) {
                if (other == character || x.academy != o->academy || !x.loaded) continue;
                s.Send(x.peer, proto::AcademyNews{proto::AcademyNewsKind::Online, o->name, m != nullptr ? m->memberClass : std::uint8_t(0), 0});
            }
            Envelope e;
            e.kind = kBusNews;
            e.academy = o->academy;
            e.character = 0;
            e.a = std::uint8_t(proto::AcademyNewsKind::Online);
            e.b = m != nullptr ? m->memberClass : 0;
            e.name = o->name;
            s.Publish(std::move(e));
        });
}

void Academies::RowsChanged(std::uint32_t academy) { s_->Changed(academy); }

void Academies::Left(std::int64_t character) {
    State& s = *s_;
    const auto it = s.online.find(character);
    if (it == s.online.end()) return;
    const std::uint32_t id = it->second.academy;
    const std::string name = it->second.name;
    s.SaveSlots(character, it->second);
    s.online.erase(it);
    s.places.erase(character);
    for (auto i = s.invites.begin(); i != s.invites.end();) {
        if (i->first == character || i->second.guardian == character) i = s.invites.erase(i);
        else ++i;
    }
    s.joinsOut.erase(character);
    if (const auto j = s.joinsIn.find(character); j != s.joinsIn.end()) {
        const State::JoinIn join = j->second;
        s.joinsIn.erase(j);
        s.JoinDone(join.from, join.academy, join.applicant, 0, 0);
    }
    if (id == 0) return;
    std::uint8_t cls = 0;
    if (const auto a = s.academies.find(id); a != s.academies.end()) {
        if (const AcademyMemberRow* m = s.Member(a->second, character)) cls = m->memberClass;
    }
    s.News(id, proto::AcademyNewsKind::Offline, name, cls, 0, true);
    s.PublishPresence(character, id, false);
    s.TellInfo(id);
    s.Forget(id);
}

void Academies::MovedAway(std::int64_t character) {
    State& s = *s_;
    const auto it = s.online.find(character);
    if (it == s.online.end()) return;
    const std::uint32_t id = it->second.academy;
    s.SaveSlots(character, it->second);
    s.online.erase(it);
    if (id != 0) s.Forget(id);
}

void Academies::LevelChanged(std::int64_t character, int level, int maxLevel) {
    State& s = *s_;
    State::Online* o = s.Find(character);
    if (o == nullptr) return;
    const int was = o->level;
    o->level = level;
    o->maxLevel = std::max(o->maxLevel, maxLevel);
    if (o->academy == 0) return;
    // A level gained to 40 or more (the new level above the old, from 0x28).
    if (const auto cls = s.ClassOf(character, *o);
        cls && *cls == std::uint8_t(game::academy::Class::Apprentice) && level > was && level >= game::academy::kAccumulateBelowLevel) {
        s.ReachedForty(character, *o);
    }
    if (const auto a = s.academies.find(o->academy); a != s.academies.end()) {
        for (AcademyMemberRow& m : a->second.members) {
            if (m.character != character) continue;
            m.level = level;
            m.maxLevel = std::max(m.maxLevel, maxLevel);
        }
    }
    const std::uint32_t id = o->academy;
    s.host.AcademySubmit([character, level, maxLevel](Database& db) { SaveAcademyLevels(db, character, level, maxLevel); }, [] {});
    s.TellInfo(id);
    s.PublishReload(id);
}

bool Academies::Handle(const AcademyActor& actor, proto::MessageId id, std::span<const std::byte> payload, Clock::time_point now) {
    State& s = *s_;
    switch (id) {
    case proto::AcademyFound::kId: s.Found(actor); return true;
    case proto::AcademyDisband::kId: s.Disband(actor); return true;
    case proto::AcademyInvite::kId: {
        proto::AcademyInvite m;
        if (!proto::Decode(payload, m)) return false;
        s.Invite(actor, m.target, now);
        return true;
    }
    case proto::AcademyAnswer::kId: {
        proto::AcademyAnswer m;
        if (!proto::Decode(payload, m)) return false;
        s.Answer(actor, m.accept != 0);
        return true;
    }
    case proto::AcademyBan::kId: {
        proto::AcademyBan m;
        if (!proto::Decode(payload, m)) return false;
        s.Remove(actor, m.name, true);
        return true;
    }
    case proto::AcademyLeave::kId: s.Remove(actor, {}, false); return true;
    case proto::AcademyGraduate::kId: {
        proto::AcademyGraduate m;
        if (!proto::Decode(payload, m)) return false;
        s.Graduate(actor, m.rating, {}, false);
        return true;
    }
    case proto::AcademyGraduateForce::kId: {
        proto::AcademyGraduateForce m;
        if (!proto::Decode(payload, m)) return false;
        s.Graduate(actor, 0, m.name, true);
        return true;
    }
    case proto::AcademyNoticeWrite::kId: {
        proto::AcademyNoticeWrite m;
        if (!proto::Decode(payload, m)) return false;
        s.Notice(actor, m.title, m.text);
        return true;
    }
    case proto::AcademyRankingAsk::kId: {
        proto::AcademyRankingAsk m;
        if (!proto::Decode(payload, m)) return false;
        s.Ranking(actor, m.npc);
        return true;
    }
    case proto::AcademyMatchRegister::kId: {
        proto::AcademyMatchRegister m;
        if (!proto::Decode(payload, m)) return false;
        s.MatchSave(actor, m.kind, m.title, false);
        return true;
    }
    case proto::AcademyMatchModify::kId: {
        proto::AcademyMatchModify m;
        if (!proto::Decode(payload, m)) return false;
        s.MatchSave(actor, m.kind, m.title, true);
        return true;
    }
    case proto::AcademyMatchDelete::kId: s.MatchDelete(actor); return true;
    case proto::AcademyMatchListAsk::kId: {
        proto::AcademyMatchListAsk m;
        if (!proto::Decode(payload, m)) return false;
        s.MatchList(actor, m);
        return true;
    }
    case proto::AcademyMatchJoin::kId: {
        proto::AcademyMatchJoin m;
        if (!proto::Decode(payload, m)) return false;
        s.MatchJoin(actor, m.entry, now);
        return true;
    }
    case proto::AcademyMatchReply::kId: {
        proto::AcademyMatchReply m;
        if (!proto::Decode(payload, m)) return false;
        s.MatchReply(actor, m.accept != 0);
        return true;
    }
    case proto::AcademyPassBuff::kId: s.Pass(actor); return true;
    default: return false;
    }
}

proto::ChatResult Academies::Chat(const AcademyActor& actor, const proto::ChatLine& line) {
    State& s = *s_;
    const State::Online* o = s.Find(actor.character);
    if (o == nullptr || o->academy == 0) return proto::ChatResult::InvalidCommand;
    for (const auto& [character, x] : s.online) {
        if (character == actor.character || x.academy != o->academy || !x.loaded) continue;
        s.Send(x.peer, line);
    }
    Envelope e;
    e.kind = kBusChat;
    e.academy = o->academy;
    e.name = line.name;
    e.text = line.text;
    s.Publish(std::move(e));
    return proto::ChatResult::Ok;
}

int Academies::Kill(std::int64_t character, double ofWhole) { return s_->Kill(character, ofWhole); }

std::uint32_t Academies::AcademyOf(std::int64_t character) const {
    const auto it = s_->online.find(character);
    return it != s_->online.end() ? it->second.academy : 0;
}

bool Academies::Busy() const { return s_->writes > 0; }

} // namespace sr::server
