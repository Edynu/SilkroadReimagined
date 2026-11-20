#include "sr/server/Parties.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sr/core/Log.h"
#include "sr/game/Jobs.h"
#include "sr/server/ShardBus.h"

namespace sr::server {

namespace proto = game::proto;

namespace {

// Two places within `range`: on the ground the regions' coordinates join; a dungeon's (region with
// 0x8000) only within the same one. `vertical`: the height counts (the loot's and the gold's reach
// are 3D, the experience's on the ground).
bool Within(const proto::WorldPosition& a, const proto::WorldPosition& b, double range, bool vertical) {
    double dx = 0, dz = 0;
    if ((a.region & 0x8000) != 0 || (b.region & 0x8000) != 0) {
        if (a.region != b.region) return false;
        dx = double(a.x) - double(b.x);
        dz = double(a.z) - double(b.z);
    } else {
        const proto::FlatPosition fa = proto::ToFlat(a), fb = proto::ToFlat(b);
        dx = fa.x - fb.x;
        dz = fa.z - fb.z;
    }
    const double dy = vertical ? double(a.y) - double(b.y) : 0.0;
    return std::sqrt(dx * dx + dy * dy + dz * dz) <= range;
}

// --- The shard's bus -------------------------------------------------

enum : std::uint8_t {
    kBusRequest = 1,   // a member's request, to the party's home
    kBusAdd = 2,       // an invited player said yes, to the party's home
    kBusState = 3,     // a party as it is now, from its home
    kBusGone = 4,      // a party dismissed, from its home
    kBusParty = 5,     // a message to a party's members
    kBusTell = 6,      // a message to one player
    kBusEntry = 7,     // a listing as it is now
    kBusEntryGone = 8, // a listing removed
    kBusLeft = 9,      // a player left the world
    kBusHello = 10,    // a server has started
    kBusAlive = 11,    // a server is still there
    kBusBye = 12,      // a server is stopping
    kBusArrived = 13,  // a member came from another server of the shard
};

constexpr auto kAliveEvery = std::chrono::seconds(5);
constexpr auto kPeerSilence = std::chrono::seconds(16); // three heartbeats missed: that server is gone

// A number no other server gives: its server's id above a count of its own.
std::uint32_t ShardWideId(std::uint16_t server, std::uint32_t count) { return (std::uint32_t(server) << 20) | (count & 0xFFFFF); }

struct Envelope {
    std::uint16_t from = 0, to = 0; // to 0: every server
    std::uint8_t kind = 0;
    std::uint32_t party = 0, character = 0, entity = 0, except = 0;
    std::uint16_t message = 0;
    std::vector<std::uint8_t> payload;
    template <typename V>
    void Visit(V& v) {
        v("from", from);
        v("to", to);
        v("kind", kind);
        v("party", party);
        v("character", character);
        v("entity", entity);
        v("except", except);
        v("message", message);
        v("payload", payload, 3900);
    }
};

struct PlayerWire {
    std::uint32_t character = 0, entity = 0;
    std::uint16_t server = 0, level = 1;
    std::string name;
    std::uint8_t race = 0, alive = 1;
    std::uint32_t hp = 0, maxHp = 0, mp = 0, maxMp = 0, model = 0, mastery1 = 0, mastery2 = 0;
    std::uint8_t jobState = 4, alias = 0, jobChanging = 0;
    template <typename V>
    void Visit(V& v) {
        v("character", character);
        v("entity", entity);
        v("server", server);
        v("level", level);
        v("name", name, 64);
        v("race", race);
        v("alive", alive);
        v("hp", hp);
        v("maxHp", maxHp);
        v("mp", mp);
        v("maxMp", maxMp);
        v("model", model);
        v("mastery1", mastery1);
        v("mastery2", mastery2);
        v("jobState", jobState);
        v("alias", alias);
        v("jobChanging", jobChanging);
    }
};

PlayerWire ToWire(const PartyHost::Player& p) {
    return {p.character, p.entity, p.server, std::uint16_t(std::clamp(p.level, 1, 65535)), p.name, p.race, std::uint8_t(p.alive ? 1 : 0), p.hp, p.maxHp, p.mp, p.maxMp, p.model, p.mastery1, p.mastery2,
            p.jobState, std::uint8_t(p.alias ? 1 : 0), std::uint8_t(p.jobChanging ? 1 : 0)};
}

PartyHost::Player FromWire(const PlayerWire& w) {
    PartyHost::Player p;
    p.character = w.character;
    p.entity = w.entity;
    p.server = w.server;
    p.level = w.level;
    p.name = w.name;
    p.race = w.race;
    p.alive = w.alive != 0;
    p.hp = w.hp;
    p.maxHp = w.maxHp;
    p.mp = w.mp;
    p.maxMp = w.maxMp;
    p.model = w.model;
    p.mastery1 = w.mastery1;
    p.mastery2 = w.mastery2;
    p.jobState = w.jobState;
    p.alias = w.alias != 0;
    p.jobChanging = w.jobChanging != 0;
    return p;
}

struct RequestWire {
    PlayerWire player;
    std::uint16_t message = 0;
    std::vector<std::uint8_t> bytes;
    template <typename V>
    void Visit(V& v) {
        v("player", player);
        v("message", message);
        v("bytes", bytes, 2048);
    }
};

struct AddWire {
    PlayerWire member, inviter;
    template <typename V>
    void Visit(V& v) {
        v("member", member);
        v("inviter", inviter);
    }
};

struct MemberWire {
    std::uint32_t character = 0, entity = 0;
    std::uint16_t server = 0;
    std::string name;
    std::uint8_t race = 0, level = 1, hp = 10, mp = 10;
    std::uint32_t model = 0, mastery1 = 0, mastery2 = 0;
    std::uint8_t jobState = 4;
    template <typename V>
    void Visit(V& v) {
        v("character", character);
        v("entity", entity);
        v("server", server);
        v("name", name, 64);
        v("race", race);
        v("level", level);
        v("hp", hp);
        v("mp", mp);
        v("model", model);
        v("mastery1", mastery1);
        v("mastery2", mastery2);
        v("jobState", jobState);
    }
};

struct StateWire {
    std::uint16_t home = 0;
    std::uint8_t settings = 0;
    std::uint32_t leader = 0;
    std::vector<MemberWire> members;
    std::uint32_t moved = 0; // the character that has just changed server (its people are told afresh)
    template <typename V>
    void Visit(V& v) {
        v("home", home);
        v("settings", settings);
        v("leader", leader);
        v("members", members, 16);
        v("moved", moved);
    }
};

struct EntryWire {
    std::uint32_t id = 0;
    std::uint16_t home = 0;
    PlayerWire leader;
    proto::PartyMatchForm form;
    std::uint8_t members = 1, capacity = 4;
    template <typename V>
    void Visit(V& v) {
        v("id", id);
        v("home", home);
        v("leader", leader);
        v("form", form);
        v("members", members);
        v("capacity", capacity);
    }
};

template <typename T>
std::optional<T> DecodeFrom(const std::vector<std::uint8_t>& payload) {
    T out;
    if (!proto::Decode(std::as_bytes(std::span(payload)), out)) return std::nullopt;
    return out;
}

// A message by its id.
template <typename Variant, std::size_t I = 0>
std::optional<Variant> DecodeVariant(proto::MessageId id, std::span<const std::byte> payload, bool* known = nullptr) {
    if constexpr (I >= std::variant_size_v<Variant>) {
        return std::nullopt;
    } else {
        using T = std::variant_alternative_t<I, Variant>;
        if (T::kId != id) return DecodeVariant<Variant, I + 1>(id, payload, known);
        if (known != nullptr) *known = true;
        T t;
        if (!proto::Decode(payload, t)) return std::nullopt;
        return Variant{std::move(t)};
    }
}

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

} // namespace

// --- Lookups -----------------------------------------------------------------------------------------

std::optional<PartyHost::Player> Parties::Here(std::uint32_t entity) const {
    std::optional<PartyHost::Player> p = host_.PlayerAt(entity);
    if (p) p->server = Self();
    return p;
}
std::optional<PartyHost::Player> Parties::HereOf(const Member& member) const {
    return member.server == Self() ? Here(member.entity) : std::nullopt;
}

Parties::Party* Parties::PartyOfCharacter(std::uint32_t character) {
    const auto it = partyOf_.find(character);
    if (it == partyOf_.end()) return nullptr;
    const auto p = parties_.find(it->second);
    return p != parties_.end() ? &p->second : nullptr;
}
const Parties::Party* Parties::PartyOfCharacter(std::uint32_t character) const {
    return const_cast<Parties*>(this)->PartyOfCharacter(character);
}
Parties::Party* Parties::PartyAt(std::uint32_t entity) {
    const std::optional<PartyHost::Player> p = Here(entity);
    return p ? PartyOfCharacter(p->character) : nullptr;
}
const Parties::Party* Parties::PartyAt(std::uint32_t entity) const { return const_cast<Parties*>(this)->PartyAt(entity); }

std::uint32_t Parties::PartyOf(std::uint32_t entity) const {
    const Party* party = PartyAt(entity);
    return party != nullptr ? party->id : 0;
}
std::vector<Parties::MemberView> Parties::MembersOf(std::uint32_t entity) const {
    std::vector<MemberView> out;
    if (const Party* party = PartyAt(entity)) {
        for (const Member& m : party->members) {
            int level = m.level;
            if (const auto here = HereOf(m)) level = here->level;
            out.push_back({m.character, m.name, level, m.server, m.entity});
        }
    }
    return out;
}

bool Parties::Leads(std::uint32_t entity) const {
    const std::optional<PartyHost::Player> p = const_cast<Parties*>(this)->Here(entity);
    const Party* party = PartyAt(entity);
    return p && party != nullptr && party->leader == p->character;
}
std::uint8_t Parties::SettingsOf(std::uint32_t entity) const {
    const Party* party = PartyAt(entity);
    return party != nullptr ? party->settings : 0;
}
std::vector<std::uint32_t> Parties::MemberEntities(std::uint32_t entity) const {
    std::vector<std::uint32_t> out;
    if (const Party* party = PartyAt(entity)) {
        for (const Member& m : party->members) {
            if (m.server == Self()) out.push_back(m.entity);
        }
    }
    return out;
}
bool Parties::SharesItems(std::uint32_t characterA, std::uint32_t characterB) const {
    const Party* a = PartyOfCharacter(characterA);
    return a != nullptr && a == PartyOfCharacter(characterB) && (a->settings & game::kPartyItemShare) != 0;
}

proto::PartyMemberRecord Parties::RecordOf(const Member& member) const {
    proto::PartyMemberRecord r;
    r.id = member.character;
    r.entity = member.entity;
    r.name = member.name;
    r.level = member.level;
    r.race = member.race;
    r.hpTenths = member.hp;
    r.mpTenths = member.mp;
    r.model = member.model;
    r.mastery1 = member.mastery1;
    r.mastery2 = member.mastery2;
    if (const std::optional<PartyHost::Player> p = HereOf(member)) {
        r.name = p->name;
        r.level = std::uint8_t(std::clamp(p->level, 1, 255));
        r.race = p->race;
        r.hpTenths = game::PartyTenths(p->hp, p->maxHp, p->alive);
        r.mpTenths = game::PartyTenths(p->mp, p->maxMp, false);
        if (p->position) r.position = *p->position;
        r.model = p->model;
        r.mastery1 = p->mastery1;
        r.mastery2 = p->mastery2;
    }
    return r;
}

proto::PartyInfo Parties::InfoOf(const Party& party) const {
    proto::PartyInfo info;
    info.party = party.id;
    info.settings = party.settings;
    info.leader = party.leader;
    for (const Member& m : party.members) info.members.push_back(RecordOf(m));
    return info;
}

Parties::Member Parties::MakeMember(const PartyHost::Player& p) const {
    Member m;
    m.character = p.character;
    m.entity = p.entity;
    m.server = p.server;
    m.name = p.name;
    m.race = p.race;
    m.model = p.model;
    m.level = std::uint8_t(std::clamp(p.level, 1, 255));
    m.hp = game::PartyTenths(p.hp, p.maxHp, p.alive);
    m.mp = game::PartyTenths(p.mp, p.maxMp, false);
    m.mastery1 = p.mastery1;
    m.mastery2 = p.mastery2;
    m.jobState = p.jobState;
    if (p.position) m.position = *p.position;
    return m;
}

// Entities and places mean something only on their own server: a member on another one is shown
// without either (a place of nothing is out of reach, so its bar shows it far away).
PartyMessage Parties::Localised(const Party& party, PartyMessage message) const {
    const auto member = [&](std::uint32_t character) -> const Member* {
        const auto it = std::find_if(party.members.begin(), party.members.end(), [&](const Member& m) { return m.character == character; });
        return it != party.members.end() ? &*it : nullptr;
    };
    const auto localise = [&](proto::PartyMemberRecord& r) {
        const Member* m = member(r.id);
        r.position = {};
        if (m == nullptr || m->server != Self()) {
            r.entity = 0;
            return;
        }
        r.entity = m->entity;
        if (const auto p = HereOf(*m); p && p->position) r.position = *p->position;
    };
    std::visit(Overloaded{[&](proto::PartyInfo& info) {
                              for (proto::PartyMemberRecord& r : info.members) localise(r);
                          },
                          [&](proto::PartyMemberJoined& joined) { localise(joined.member); },
                          [&](proto::PartyMemberUpdate& u) {
                              const Member* m = member(u.member);
                              if (m != nullptr && m->server == Self()) return;
                              u.entity = 0;
                              u.mask &= std::uint8_t(~proto::kPartyPosition);
                              u.position = {};
                          },
                          [](auto&) {}},
               message);
    return message;
}

void Parties::TellHere(const Party& party, const PartyMessage& message, std::uint32_t exceptCharacter) {
    const PartyMessage local = Localised(party, message);
    for (const Member& m : party.members) {
        if (m.server == Self() && m.character != exceptCharacter) host_.Tell(m.entity, local);
    }
}

void Parties::TellParty(const Party& party, const PartyMessage& message, std::uint32_t exceptCharacter) {
    TellHere(party, message, exceptCharacter);
    if (std::none_of(party.members.begin(), party.members.end(), [&](const Member& m) { return m.server != Self(); })) return;
    std::visit([&](const auto& m) { const auto bytes = proto::Encode(m); Publish(kBusParty, 0, party.id, 0, 0, exceptCharacter, std::decay_t<decltype(m)>::kId, bytes); }, message);
}

void Parties::TellOne(const PartyHost::Player& who, const PartyMessage& message) {
    if (who.server == Self()) return host_.Tell(who.entity, message);
    std::visit([&](const auto& m) { const auto bytes = proto::Encode(m); Publish(kBusTell, who.server, 0, who.character, who.entity, 0, std::decay_t<decltype(m)>::kId, bytes); }, message);
}

void Parties::Result(std::uint32_t entity, proto::PartyAction action, proto::PartyStatus status, std::string name) {
    host_.Tell(entity, proto::PartyResult{action, status, std::move(name)});
}
void Parties::Result(const PartyHost::Player& who, proto::PartyAction action, proto::PartyStatus status, std::string name) {
    TellOne(who, proto::PartyResult{action, status, std::move(name)});
}

void Parties::MatchAnswer(const PartyHost::Player& who, proto::PartyMatchAction action, proto::PartyStatus status, std::uint32_t entry) {
    TellOne(who, proto::PartyMatchAnswer{action, status, entry});
}

// --- The shard's other game servers -------------------------------------------------------------------

void Parties::Publish(std::uint8_t kind, std::uint16_t to, std::uint32_t party, std::uint32_t character, std::uint32_t entity, std::uint32_t except,
                      std::uint16_t message, std::span<const std::byte> payload) {
    if (!host_.PartyOnBus()) return;
    Envelope e;
    e.from = Self();
    e.to = to;
    e.kind = kind;
    e.party = party;
    e.character = character;
    e.entity = entity;
    e.except = except;
    e.message = message;
    e.payload.resize(payload.size());
    if (!payload.empty()) std::memcpy(e.payload.data(), payload.data(), payload.size());
    std::string text = BusToHex(proto::Encode(e));
    if (text.size() >= kBusTextLimit) {
        core::LogWarning("parties: a bus message of {} bytes not sent (kind {})", text.size(), int(kind));
        return;
    }
    host_.PartyPublish(std::move(text));
}

void Parties::PublishState(Party& party, std::uint32_t moved) {
    party.shared = party.shared || std::any_of(party.members.begin(), party.members.end(), [&](const Member& m) { return m.server != Self(); });
    if (!party.shared) return;
    StateWire w;
    w.home = party.home;
    w.settings = party.settings;
    w.leader = party.leader;
    for (const Member& m : party.members) w.members.push_back({m.character, m.entity, m.server, m.name, m.race, m.level, m.hp, m.mp, m.model, m.mastery1, m.mastery2, m.jobState});
    w.moved = moved;
    Publish(kBusState, 0, party.id, 0, 0, 0, 0, proto::Encode(w));
}

void Parties::PublishEntry(const Entry& e, std::uint16_t to) {
    EntryWire w;
    w.id = e.id;
    w.home = e.home;
    w.leader = ToWire(e.leader);
    w.form = e.form;
    w.members = e.members;
    w.capacity = e.capacity;
    Publish(kBusEntry, to, 0, 0, 0, 0, 0, proto::Encode(w));
}

// What a listing shows of its party, kept at its home and told to the others.
void Parties::SyncEntry(std::uint32_t leaderCharacter) {
    const auto it = entryOf_.find(leaderCharacter);
    if (it == entryOf_.end()) return;
    Entry& e = entries_[it->second];
    if (e.home != Self()) return;
    const Party* party = PartyOfCharacter(leaderCharacter);
    e.members = std::uint8_t(party != nullptr ? party->members.size() : 1);
    e.capacity = std::uint8_t(game::PartyCapacity(party != nullptr ? party->settings : e.form.settings));
    PublishEntry(e);
}

void Parties::Hello() { Publish(kBusHello, 0, 0, 0, 0, 0, 0, {}); }

std::string Parties::GoodbyeText() const {
    if (!host_.PartyOnBus()) return {};
    Envelope e;
    e.from = Self();
    e.kind = kBusBye;
    return BusToHex(proto::Encode(e));
}

void Parties::BusMessage(const std::string& text, Clock::time_point now) {
    const auto bytes = BusFromHex(text);
    if (!bytes) return;
    Envelope e;
    if (!proto::Decode(*bytes, e) || e.from == Self()) return;
    if (e.kind == kBusBye) return ServerGone(e.from);
    if (e.kind == kBusHello) {
        // It has started (again: what it had is gone); it is told what it missed.
        if (peers_.contains(e.from)) ServerGone(e.from);
        peers_[e.from] = now;
        Publish(kBusAlive, e.from, 0, 0, 0, 0, 0, {});
        for (const auto& [id, entry] : entries_) {
            if (entry.home == Self()) PublishEntry(entry, e.from);
        }
        return;
    }
    peers_[e.from] = now;
    if (e.to != 0 && e.to != Self()) return;
    const auto payload = std::as_bytes(std::span(e.payload));
    switch (e.kind) {
    case kBusRequest: {
        const auto r = DecodeFrom<RequestWire>(e.payload);
        if (!r) return;
        const auto request = DecodeVariant<PartyRequest>(r->message, std::as_bytes(std::span(r->bytes)));
        if (request) Apply(FromWire(r->player), *request, now);
        return;
    }
    case kBusAdd: {
        if (const auto a = DecodeFrom<AddWire>(e.payload)) ApplyAdd(FromWire(a->member), FromWire(a->inviter), e.party);
        return;
    }
    case kBusState: {
        const auto w = DecodeFrom<StateWire>(e.payload);
        if (!w) return;
        // What it had of the party before: its people's places, the loot's turn.
        Party before;
        if (const auto old = parties_.find(e.party); old != parties_.end()) {
            before = old->second;
            for (const Member& m : before.members) partyOf_.erase(m.character);
        }
        if (std::none_of(w->members.begin(), w->members.end(), [&](const MemberWire& m) { return m.server == Self(); })) {
            parties_.erase(e.party);
            return;
        }
        Party& p = parties_[e.party];
        p = Party{};
        p.id = e.party;
        p.home = w->home;
        p.shared = true;
        p.settings = w->settings;
        p.leader = w->leader;
        for (const MemberWire& mw : w->members) {
            Member m;
            m.character = mw.character;
            m.entity = mw.entity;
            m.server = mw.server;
            m.name = mw.name;
            m.race = mw.race;
            m.model = mw.model;
            m.level = mw.level;
            m.hp = mw.hp;
            m.mp = mw.mp;
            m.mastery1 = mw.mastery1;
            m.mastery2 = mw.mastery2;
            m.jobState = mw.jobState;
            if (const auto b = std::find_if(before.members.begin(), before.members.end(), [&](const Member& x) { return x.character == m.character; }); b != before.members.end()) {
                m.position = b->position;
                m.positionAt = b->positionAt;
            }
            p.members.push_back(std::move(m));
            partyOf_[mw.character] = e.party;
        }
        // The loot's turn goes on, the members who came last at its end.
        for (const std::uint32_t c : before.lootTurn) {
            if (std::any_of(p.members.begin(), p.members.end(), [&](const Member& m) { return m.character == c; })) p.lootTurn.push_back(c);
        }
        for (const Member& m : p.members) {
            if (std::find(p.lootTurn.begin(), p.lootTurn.end(), m.character) == p.lootTurn.end()) p.lootTurn.push_back(m.character);
        }
        if (w->moved != 0) TellHere(p, InfoOf(p), 0);
        return;
    }
    case kBusArrived: {
        if (const auto p = DecodeFrom<PlayerWire>(e.payload)) {
            moving_.erase(p->character);
            MemberArrived(FromWire(*p));
        }
        return;
    }
    case kBusGone: {
        if (const auto it = parties_.find(e.party); it != parties_.end()) {
            for (const Member& m : it->second.members) partyOf_.erase(m.character);
            parties_.erase(it);
        }
        return;
    }
    case kBusParty: {
        const auto it = parties_.find(e.party);
        if (it == parties_.end()) return;
        const auto message = DecodeVariant<PartyMessage>(e.message, payload);
        if (!message) return;
        // A member's news: what the mirror remembers of it too.
        if (const auto* u = std::get_if<proto::PartyMemberUpdate>(&*message)) {
            for (Member& m : it->second.members) {
                if (m.character != u->member) continue;
                if ((u->mask & proto::kPartyLevel) != 0) m.level = u->level;
                if ((u->mask & proto::kPartyVitals) != 0) {
                    m.hp = u->hpTenths;
                    m.mp = u->mpTenths;
                }
                if ((u->mask & proto::kPartyMasteries) != 0) {
                    m.mastery1 = u->mastery1;
                    m.mastery2 = u->mastery2;
                }
            }
        }
        TellHere(it->second, *message, e.except);
        return;
    }
    case kBusTell: {
        const auto message = DecodeVariant<PartyMessage>(e.message, payload);
        const std::optional<PartyHost::Player> p = Here(e.entity);
        if (message && p && p->character == e.character) host_.Tell(e.entity, *message);
        return;
    }
    case kBusEntry: {
        const auto w = DecodeFrom<EntryWire>(e.payload);
        if (!w) return;
        Entry& entry = entries_[w->id];
        // A listing handed to a new master: the old one's key goes.
        if (entry.id == w->id && entry.leader.character != w->leader.character) entryOf_.erase(entry.leader.character);
        entry.id = w->id;
        entry.home = w->home;
        entry.leader = FromWire(w->leader);
        entry.form = w->form;
        entry.members = w->members;
        entry.capacity = w->capacity;
        entryOf_[entry.leader.character] = w->id;
        return;
    }
    case kBusEntryGone: {
        if (const auto it = entries_.find(e.party); it != entries_.end()) {
            entryOf_.erase(it->second.leader.character);
            entries_.erase(it);
        }
        return;
    }
    case kBusLeft: {
        if (const auto p = DecodeFrom<PlayerWire>(e.payload)) CharacterGone(FromWire(*p));
        return;
    }
    default: return;
    }
}

void Parties::ServerGone(std::uint16_t server) {
    peers_.erase(server);
    core::LogInfo("parties: game server {} is gone", server);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.home == server) {
            entryOf_.erase(it->second.leader.character);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = joinRequests_.begin(); it != joinRequests_.end();) {
        if (it->second.applicant.server == server) {
            MatchAnswer(it->second.leader, proto::PartyMatchAction::Answer, proto::PartyStatus::Timeout, it->second.entry);
            applying_.erase(it->second.applicant.character);
            it = joinRequests_.erase(it);
        } else {
            ++it;
        }
    }
    std::vector<std::uint32_t> ids;
    for (const auto& [id, party] : parties_) ids.push_back(id);
    for (const std::uint32_t id : ids) {
        const auto it = parties_.find(id);
        if (it == parties_.end()) continue;
        Party& party = it->second;
        if (party.home == server) {
            // What it decided is lost with it: its members here are told it is over.
            TellHere(party, proto::PartyDismissed{2}, 0);
            for (const Member& m : party.members) partyOf_.erase(m.character);
            parties_.erase(it);
            continue;
        }
        if (party.home != Self()) continue;
        std::vector<std::uint32_t> lost;
        for (const Member& m : party.members) {
            if (m.server == server) lost.push_back(m.character);
        }
        for (const std::uint32_t character : lost) {
            const auto still = parties_.find(id);
            if (still != parties_.end()) Remove(still->second, character, proto::PartyLeaveReason::Logout);
        }
    }
}

// --- Requests ----------------------------------------------------------------------------------------

Parties::Handled Parties::Handle(std::uint32_t entity, proto::MessageId id, std::span<const std::byte> payload, Clock::time_point now) {
    const std::optional<PartyHost::Player> me = Here(entity);
    if (!me) return Handled::No;
    bool known = false;
    const std::optional<PartyRequest> request = DecodeVariant<PartyRequest>(id, payload, &known);
    if (!known) return Handled::No;
    if (!request) return Handled::Malformed;
    const std::uint16_t home = HomeFor(*me, *request);
    if (home == Self()) {
        Apply(*me, *request, now);
        return Handled::Yes;
    }
    RequestWire w;
    w.player = ToWire(*me);
    w.message = id;
    w.bytes.resize(payload.size());
    if (!payload.empty()) std::memcpy(w.bytes.data(), payload.data(), payload.size());
    Publish(kBusRequest, home, 0, 0, 0, 0, 0, proto::Encode(w));
    return Handled::Yes;
}

std::uint16_t Parties::HomeFor(const PartyHost::Player& me, const PartyRequest& request) const {
    // An invitation is between players who see each other, and the list is everybody's.
    if (std::holds_alternative<proto::PartyInvite>(request) || std::holds_alternative<proto::PartyInviteAnswer>(request) ||
        std::holds_alternative<proto::PartyMatchList>(request)) {
        return Self();
    }
    if (const auto* join = std::get_if<proto::PartyMatchJoin>(&request)) {
        const auto it = entries_.find(join->entry);
        return it != entries_.end() ? it->second.home : Self();
    }
    const Party* party = PartyOfCharacter(me.character);
    return party != nullptr ? party->home : Self();
}

void Parties::Apply(const PartyHost::Player& me, const PartyRequest& request, Clock::time_point now) {
    std::visit(Overloaded{[&](const proto::PartyInvite& m) { Invite(me, m, now); },
                          [&](const proto::PartyInviteAnswer& m) { Answer(me, m.accept != 0); },
                          [&](const proto::PartyLeave&) { Leave(me, proto::PartyLeaveReason::Left); },
                          [&](const proto::PartyKick& m) { Kick(me, m.member); },
                          [&](const proto::PartySetSettings& m) { SetSettings(me, m.settings, proto::PartyAction::Settings); },
                          [&](const proto::PartyMatchRegister& m) { Register(me, m.form); },
                          [&](const proto::PartyMatchDelete&) { Delete(me); },
                          [&](const proto::PartyMatchList& m) { List(me, m); },
                          [&](const proto::PartyMatchJoin& m) { Join(me, m.entry, now); },
                          [&](const proto::PartyMatchJoinAnswer& m) { AnswerJoin(me, m.accept != 0); }},
               request);
}

void Parties::Invite(const PartyHost::Player& me, const proto::PartyInvite& m, Clock::time_point now) {
    using S = proto::PartyStatus;
    const auto refuse = [&](S status, std::string name = {}) { Result(me, proto::PartyAction::Invite, status, std::move(name)); };
    Party* party = PartyOfCharacter(me.character);
    if (!me.alive) return refuse(S::Busy);
    if (party != nullptr) {
        // Into the party: the master, or anyone when the party lets them; not when full
        // (8 for an exp-shared party, else 4).
        if (party->leader != me.character && (party->settings & game::kPartyAnyoneInvites) == 0) return refuse(S::NotLeader);
        if (int(party->members.size()) >= game::PartyCapacity(party->settings)) {
            return refuse((party->settings & game::kPartyExpShare) != 0 ? S::FullShared : S::FullFree);
        }
    } else if (me.level < game::kPartyMinLevel) {
        return refuse(S::LowLevel);
    }
    // The invited player: in the world, a player, near, alive, in no party, without a listing of its
    // own, not asked by someone else already.
    const std::optional<PartyHost::Player> target = m.target != me.entity ? Here(m.target) : std::nullopt;
    if (!target) return refuse(S::InvalidUser);
    if (!me.position || !target->position || !Within(*me.position, *target->position, game::kPartyInviteRange, true)) return refuse(S::TooFar, target->name);
    if (!target->alive) return refuse(S::MatchBusy, target->name); // the original's 0x2C06 for a dead target
    if (const Party* theirs = PartyOfCharacter(target->character)) return refuse(theirs == party ? S::ExistingMember : S::OtherParty, target->name);
    if (entryOf_.contains(target->character)) return refuse(S::MatchListed, target->name);
    if (proposals_.contains(target->entity) || applying_.contains(target->character)) return refuse(S::Busy, target->name);
    // The jobs' pair rule is not asked here: the invitee is asked first and a mismatch refused to both once it accepts
    // (-> the shard manager -> 0xB060 / 0xB062 and 0xB067).
    Proposal p;
    p.inviter = me.entity;
    p.inviterCharacter = me.character;
    p.join = party != nullptr;
    p.settings = party != nullptr ? party->settings : std::uint8_t(m.settings & game::kPartySettingsMask);
    p.until = now + std::chrono::seconds(game::kPartyProposalSeconds);
    proposals_[target->entity] = p;
    host_.Tell(target->entity, proto::PartyInvited{me.entity, me.name, p.settings, p.join});
    core::LogInfo("parties: {} asks {} to party ({}, settings {})", me.name, target->name, p.join ? "join" : "a new one", p.settings);
}

void Parties::Answer(const PartyHost::Player& me, bool accept) {
    using S = proto::PartyStatus;
    const auto it = proposals_.find(me.entity);
    if (it == proposals_.end()) return;
    const Proposal p = it->second;
    proposals_.erase(it);
    const std::optional<PartyHost::Player> inviter = Here(p.inviter);
    if (!inviter || inviter->character != p.inviterCharacter) return Result(me, proto::PartyAction::Answer, S::NoInviter);
    if (!accept) {
        core::LogInfo("parties: {} says no to {}", me.name, inviter->name);
        return Result(*inviter, proto::PartyAction::Invite, p.join ? S::JoinRefused : S::Refused, me.name);
    }
    if (PartyOfCharacter(me.character) != nullptr) return Result(*inviter, proto::PartyAction::Invite, S::OtherParty, me.name);
    Party* party = PartyOfCharacter(inviter->character);
    if (party == nullptr) {
        if (p.join) return Result(me, proto::PartyAction::Answer, S::NoParty); // the party went meanwhile
        if (!game::JobsMayParty(me.jobState, inviter->jobState)) {
            Result(me, proto::PartyAction::Answer, S::JobMismatch);
            return Result(*inviter, proto::PartyAction::Invite, S::JobMismatch, me.name);
        }
        Party& formed = Form(*inviter, me, p.settings);
        // A listing the master had is now its party's.
        CheckFull(formed);
        return;
    }
    if (party->home != Self()) {
        // Decided where the party lives.
        AddWire a;
        a.member = ToWire(me);
        a.inviter = ToWire(*inviter);
        Publish(kBusAdd, party->home, party->id, 0, 0, 0, 0, proto::Encode(a));
        return;
    }
    ApplyAdd(me, *inviter, party->id);
}

void Parties::ApplyAdd(const PartyHost::Player& member, const PartyHost::Player& inviter, std::uint32_t partyId) {
    using S = proto::PartyStatus;
    const auto found = parties_.find(partyId);
    if (found == parties_.end()) return Result(member, proto::PartyAction::Answer, S::NoParty);
    if (PartyOfCharacter(member.character) != nullptr) return Result(inviter, proto::PartyAction::Invite, S::OtherParty, member.name);
    Party& party = found->second;
    if (int(party.members.size()) >= game::PartyCapacity(party.settings)) {
        const S full = (party.settings & game::kPartyExpShare) != 0 ? S::FullShared : S::FullFree;
        Result(member, proto::PartyAction::Answer, full);
        return Result(inviter, proto::PartyAction::Invite, full, member.name);
    }
    // The pair rule against the first member.
    if (!party.members.empty() && !game::JobsMayParty(member.jobState, party.members.front().jobState)) {
        Result(member, proto::PartyAction::Answer, S::JobMismatch);
        return Result(inviter, proto::PartyAction::Invite, S::JobMismatch, member.name);
    }
    Add(party, member);
    CheckFull(party);
}

Parties::Party& Parties::Form(const PartyHost::Player& leader, const PartyHost::Player& member, std::uint8_t settings) {
    const std::uint32_t id = ShardWideId(Self(), nextParty_++);
    Party& party = parties_[id];
    party.id = id;
    party.home = Self();
    party.settings = std::uint8_t(settings & game::kPartySettingsMask);
    party.leader = leader.character;
    for (const PartyHost::Player* p : {&leader, &member}) {
        party.members.push_back(MakeMember(*p));
        party.lootTurn.push_back(p->character);
        partyOf_[p->character] = party.id;
    }
    PublishState(party);
    TellParty(party, InfoOf(party));
    core::LogInfo("parties: {} forms party {} with {} (settings {})", leader.name, party.id, member.name, party.settings);
    return party;
}

bool Parties::Add(Party& party, const PartyHost::Player& member) {
    const Member m = MakeMember(member);
    party.members.push_back(m);
    party.lootTurn.push_back(member.character);
    partyOf_[member.character] = party.id;
    PublishState(party);
    TellParty(party, proto::PartyMemberJoined{RecordOf(m)}, member.character);
    TellOne(member, InfoOf(party));
    core::LogInfo("parties: {} joins party {} ({} members)", member.name, party.id, party.members.size());
    return true;
}

void Parties::CheckFull(const Party& party) {
    if (int(party.members.size()) < game::PartyCapacity(party.settings)) return SyncEntry(party.leader);
    if (entryOf_.contains(party.leader)) RemoveListing(party.leader, proto::PartyStatus::AlreadyFull);
}

void Parties::Leave(const PartyHost::Player& me, proto::PartyLeaveReason reason) {
    Party* party = PartyOfCharacter(me.character);
    if (party == nullptr) return;
    // The master leaves as any member does: the next one leads (the original's leave has no other path; its party of
    // two goes).
    Remove(*party, me.character, reason);
}

void Parties::Kick(const PartyHost::Player& me, std::uint32_t member) {
    Party* party = PartyOfCharacter(me.character);
    if (party == nullptr) return;
    // The master only, not itself, a member of its party.
    if (party->leader != me.character) return Result(me, proto::PartyAction::Kick, proto::PartyStatus::NotLeader);
    if (member == me.character) return;
    if (std::none_of(party->members.begin(), party->members.end(), [&](const Member& m) { return m.character == member; })) return;
    core::LogInfo("parties: {} sends member {} out of party {}", me.name, member, party->id);
    Remove(*party, member, proto::PartyLeaveReason::Kicked);
}

void Parties::Remove(Party& party, std::uint32_t character, proto::PartyLeaveReason reason) {
    const auto it = std::find_if(party.members.begin(), party.members.end(), [&](const Member& m) { return m.character == character; });
    if (it == party.members.end()) return;
    // Everyone, the member too, hears of it before it goes.
    TellParty(party, proto::PartyMemberLeft{character, reason});
    party.members.erase(it);
    std::erase(party.lootTurn, character);
    partyOf_.erase(character);
    if (party.members.size() < 2) return Dismiss(party, 2);
    if (party.leader == character) {
        // The first member left leads: the members are kept in the order they joined (the client's "entrusted to [%s]");
        // the old master's listing is the new master's (0x3864 kind 0x12).
        party.leader = party.members.front().character;
        PublishState(party);
        TellParty(party, proto::PartyLeaderChanged{party.leader});
        if (entryOf_.contains(character)) HandListing(character, party.members.front());
        core::LogInfo("parties: party {} is led by {} now", party.id, party.leader);
        return;
    }
    PublishState(party);
    SyncEntry(party.leader);
}

void Parties::Dismiss(Party& party, std::uint8_t reason) {
    const std::uint32_t id = party.id;
    TellParty(party, proto::PartyDismissed{reason});
    if (party.shared) Publish(kBusGone, 0, id, 0, 0, 0, 0, {});
    if (entryOf_.contains(party.leader)) RemoveListing(party.leader, proto::PartyStatus::LeaderLeft);
    for (const Member& m : party.members) partyOf_.erase(m.character);
    parties_.erase(id); // `party` is gone
    core::LogInfo("parties: party {} dismissed", id);
}

void Parties::SetSettings(const PartyHost::Player& me, std::uint8_t settings, proto::PartyAction action) {
    using S = proto::PartyStatus;
    Party* party = PartyOfCharacter(me.character);
    if (party == nullptr) return Result(me, action, S::NoParty);
    if (party->leader != me.character) return Result(me, action, S::NotLeader);
    settings &= game::kPartySettingsMask;
    if (settings == party->settings) return;
    // A party of more than 4 keeps its exp shared (ours: the size a party of free exp holds).
    if (int(party->members.size()) > game::PartyCapacity(settings)) return Result(me, action, S::FullFree);
    party->settings = settings;
    PublishState(*party);
    TellParty(*party, proto::PartySettingsChanged{settings});
    if (const auto e = entryOf_.find(me.character); e != entryOf_.end()) entries_[e->second].form.settings = settings;
    core::LogInfo("parties: {} sets party {} to settings {}", me.name, party->id, settings);
    CheckFull(*party);
}

// --- Matching ----------------------------------------------------------------------------------------

void Parties::Register(const PartyHost::Player& me, const proto::PartyMatchForm& form) {
    using S = proto::PartyStatus;
    const bool modify = entryOf_.contains(me.character);
    const proto::PartyMatchAction action = modify ? proto::PartyMatchAction::Modify : proto::PartyMatchAction::Register;
    const auto refuse = [&](S status) { MatchAnswer(me, action, status, 0); };
    Party* party = PartyOfCharacter(me.character);
    // alive; the master of its party, or of level 5 without one.
    if (!me.alive) return refuse(S::Busy);
    if (party != nullptr && party->leader != me.character) return refuse(S::MatchLeader);
    if (party == nullptr && me.level < game::kPartyMinLevel) return refuse(S::LowLevel);
    if (form.purpose >= game::kPartyPurposes || form.title.empty()) return refuse(S::Unknown);
    // The jobs: no suit's
    // change under way (0x2C26), an alias in a suit (5), the purpose the job's (0x2C23).
    if (me.jobChanging) return refuse(S::JobChanging);
    if (me.jobState != game::kNoJob && !me.alias) return refuse(S::NoAlias);
    if (!game::JobMayListPurpose(me.jobState, form.purpose)) return refuse(S::JobMismatch);
    if (form.levelMin < 1 || form.levelMax < form.levelMin) return refuse(S::OutOfLevel);
    // Ours: the settings chosen with the listing are the party's.
    const std::uint8_t settings = std::uint8_t(form.settings & game::kPartySettingsMask);
    if (party != nullptr) {
        if (int(party->members.size()) > game::PartyCapacity(settings)) return refuse(S::FullFree);
        if (int(party->members.size()) >= game::PartyCapacity(settings)) return refuse(S::AlreadyFull);
        if (party->settings != settings) {
            party->settings = settings;
            PublishState(*party);
            TellParty(*party, proto::PartySettingsChanged{settings});
        }
    }
    std::uint32_t id = 0;
    if (modify) {
        id = entryOf_[me.character];
    } else {
        id = ShardWideId(Self(), nextEntry_++);
        entryOf_[me.character] = id;
    }
    Entry& e = entries_[id];
    e.id = id;
    e.home = Self();
    e.leader = me;
    e.form = form;
    e.form.settings = settings;
    SyncEntry(me.character);
    MatchAnswer(me, action, S::Ok, id);
    core::LogInfo("parties: {} {} listing {} '{}'", me.name, modify ? "changes" : "makes", id, form.title);
}

void Parties::Delete(const PartyHost::Player& me) {
    using S = proto::PartyStatus;
    const Party* party = PartyOfCharacter(me.character);
    if (party != nullptr && party->leader != me.character) return MatchAnswer(me, proto::PartyMatchAction::Delete, S::MatchLeader, 0);
    const auto it = entryOf_.find(me.character);
    if (it == entryOf_.end()) return MatchAnswer(me, proto::PartyMatchAction::Delete, S::MatchNone, 0);
    const std::uint32_t id = it->second;
    entryOf_.erase(it);
    entries_.erase(id);
    Publish(kBusEntryGone, 0, id, 0, 0, 0, 0, {});
    MatchAnswer(me, proto::PartyMatchAction::Delete, S::Ok, id);
}

void Parties::HandListing(std::uint32_t leaderCharacter, const Member& next) {
    const auto it = entryOf_.find(leaderCharacter);
    if (it == entryOf_.end()) return;
    const std::uint32_t id = it->second;
    Entry& e = entries_[id];
    entryOf_.erase(it);
    // A join request waiting on the old master is refused (it asked a master who is gone).
    if (const auto r = joinRequests_.find(leaderCharacter); r != joinRequests_.end() && r->second.entry == id) {
        MatchAnswer(r->second.applicant, proto::PartyMatchAction::Answer, proto::PartyStatus::JoinRefused, id);
        applying_.erase(r->second.applicant.character);
        joinRequests_.erase(r);
    }
    PartyHost::Player leader;
    leader.character = next.character;
    leader.entity = next.entity;
    leader.server = next.server;
    leader.name = next.name;
    leader.level = next.level;
    leader.race = next.race;
    leader.model = next.model;
    leader.mastery1 = next.mastery1;
    leader.mastery2 = next.mastery2;
    leader.jobState = next.jobState;
    if (const auto here = HereOf(next)) leader = *here;
    e.leader = leader;
    entryOf_[leader.character] = id;
    core::LogInfo("parties: listing {} is {}'s now", id, leader.name);
    SyncEntry(leader.character);
}

void Parties::RemoveListing(std::uint32_t leaderCharacter, proto::PartyStatus why) {
    const auto it = entryOf_.find(leaderCharacter);
    if (it == entryOf_.end()) return;
    const std::uint32_t id = it->second;
    const PartyHost::Player leader = entries_[id].leader;
    entryOf_.erase(it);
    entries_.erase(id);
    Publish(kBusEntryGone, 0, id, 0, 0, 0, 0, {});
    // A join request waiting on it is answered for the leader.
    if (const auto r = joinRequests_.find(leaderCharacter); r != joinRequests_.end() && r->second.entry == id) {
        MatchAnswer(r->second.applicant, proto::PartyMatchAction::Answer, proto::PartyStatus::JoinRefused, id);
        applying_.erase(r->second.applicant.character);
        joinRequests_.erase(r);
    }
    MatchAnswer(leader, proto::PartyMatchAction::Removed, why, id);
}

void Parties::List(const PartyHost::Player& me, const proto::PartyMatchList& m) {
    std::vector<const Entry*> fit;
    for (const auto& [id, e] : entries_) {
        if (m.purpose != 0xFF && e.form.purpose != m.purpose) continue;
        if (m.levelMin != 0 && e.form.levelMax < m.levelMin) continue;
        if (m.levelMax != 0 && e.form.levelMin > m.levelMax) continue;
        if (!m.name.empty() && e.leader.name.find(m.name) == std::string::npos) continue;
        fit.push_back(&e);
    }
    proto::PartyMatchListing out;
    const std::size_t pages = std::max<std::size_t>(1, (fit.size() + game::kPartyMatchPageSize - 1) / game::kPartyMatchPageSize);
    out.pages = std::uint8_t(std::min<std::size_t>(pages, 255));
    out.page = std::uint8_t(std::min<std::size_t>(m.page, pages - 1));
    if (const auto own = entryOf_.find(me.character); own != entryOf_.end()) out.own = own->second;
    for (std::size_t i = std::size_t(out.page) * game::kPartyMatchPageSize; i < fit.size() && out.entries.size() < std::size_t(game::kPartyMatchPageSize); ++i) {
        const Entry& e = *fit[i];
        proto::PartyMatchEntry r;
        r.entry = e.id;
        r.leader = e.leader.character;
        r.form = e.form;
        r.name = e.leader.name;
        r.race = e.leader.race;
        r.members = e.members;
        r.capacity = e.capacity;
        out.entries.push_back(std::move(r));
    }
    TellOne(me, out);
}

void Parties::Join(const PartyHost::Player& me, std::uint32_t entry, Clock::time_point now) {
    using S = proto::PartyStatus;
    const auto refuse = [&](S status) { MatchAnswer(me, proto::PartyMatchAction::Join, status, entry); };
    // The shard manager's checks in its order (SM): the applicant in a party (0x2C18), with a listing of its
    // own (0x2C25); the listing gone (0x2C1C); the party full (0x2C13 exp shared, else 0x2C14); the pair rule against
    // the master (0x2C23); the level outside the range (0x2C1E); another's request waiting on the listing (0x2C1F).
    // Ours besides: a dead applicant, or one whose own request or invitation waits.
    if (!me.alive) return refuse(S::MatchBusy);
    if (PartyOfCharacter(me.character) != nullptr) return refuse(S::OtherParty);
    if (entryOf_.contains(me.character)) return refuse(S::MatchListed);
    if (applying_.contains(me.character) || (me.server == Self() && proposals_.contains(me.entity))) return refuse(S::MatchJoining);
    const auto it = entries_.find(entry);
    if (it == entries_.end()) return refuse(S::MatchNone);
    const Entry& e = it->second;
    const Party* party = PartyOfCharacter(e.leader.character);
    if (party != nullptr && int(party->members.size()) >= game::PartyCapacity(party->settings)) {
        return refuse((party->settings & game::kPartyExpShare) != 0 ? S::FullShared : S::FullFree);
    }
    if (!game::JobsMayParty(me.jobState, party != nullptr && !party->members.empty() ? party->members.front().jobState : e.leader.jobState)) {
        return refuse(S::JobMismatch);
    }
    if (me.level < e.form.levelMin || me.level > e.form.levelMax) return refuse(S::MatchLevel);
    if (joinRequests_.contains(e.leader.character)) return refuse(S::MatchJoining);
    joinRequests_[e.leader.character] = {me, e.leader, entry, now + std::chrono::seconds(game::kPartyProposalSeconds)};
    applying_[me.character] = e.leader.character;
    TellOne(e.leader, proto::PartyMatchJoinRequest{entry, me.character, me.name, std::uint8_t(std::clamp(me.level, 1, 255)), me.race});
    MatchAnswer(me, proto::PartyMatchAction::Join, S::Ok, entry);
    core::LogInfo("parties: {} asks to join listing {}", me.name, entry);
}

void Parties::AnswerJoin(const PartyHost::Player& me, bool accept) {
    using S = proto::PartyStatus;
    const auto it = joinRequests_.find(me.character);
    if (it == joinRequests_.end()) return;
    const JoinRequest r = it->second;
    joinRequests_.erase(it);
    applying_.erase(r.applicant.character);
    const auto answer = [&](S status) {
        MatchAnswer(r.applicant, proto::PartyMatchAction::Answer, status, r.entry);
        MatchAnswer(me, proto::PartyMatchAction::Answer, status, r.entry);
    };
    if (!accept) {
        core::LogInfo("parties: {} turns {} down", me.name, r.applicant.name);
        return answer(S::JoinRefused);
    }
    if (PartyOfCharacter(r.applicant.character) != nullptr) return answer(S::OtherParty);
    const auto e = entries_.find(r.entry);
    Party* party = PartyOfCharacter(me.character);
    if (party == nullptr) {
        // The listing's leader without a party: one is formed with the listing's settings.
        const std::uint8_t settings = e != entries_.end() ? e->second.form.settings : std::uint8_t(game::kPartyExpShare | game::kPartyItemShare);
        answer(S::Ok);
        Party& formed = Form(me, r.applicant, settings);
        CheckFull(formed);
        return;
    }
    if (int(party->members.size()) >= game::PartyCapacity(party->settings)) return answer(S::AlreadyFull);
    answer(S::Ok);
    Add(*party, r.applicant);
    CheckFull(*party);
}

// --- The world ---------------------------------------------------------------------------------------

void Parties::CharacterGone(const PartyHost::Player& me) {
    // What it was asking for, what was waiting on it.
    if (const auto a = applying_.find(me.character); a != applying_.end()) {
        if (const auto r = joinRequests_.find(a->second); r != joinRequests_.end()) {
            MatchAnswer(r->second.leader, proto::PartyMatchAction::Answer, proto::PartyStatus::Timeout, r->second.entry);
            joinRequests_.erase(r);
        }
        applying_.erase(a);
    }
    if (const auto r = joinRequests_.find(me.character); r != joinRequests_.end()) {
        MatchAnswer(r->second.applicant, proto::PartyMatchAction::Answer, proto::PartyStatus::JoinRefused, r->second.entry);
        applying_.erase(r->second.applicant.character);
        joinRequests_.erase(r);
    }
    if (Party* party = PartyOfCharacter(me.character)) {
        if (party->home == Self()) {
            Remove(*party, me.character, proto::PartyLeaveReason::Logout);
        } else if (me.server == Self()) {
            // Gone from here at once; its home tells the rest. Its entity means nothing any more.
            std::erase_if(party->members, [&](const Member& m) { return m.character == me.character; });
            std::erase(party->lootTurn, me.character);
            partyOf_.erase(me.character);
        }
    }
    if (const auto e = entryOf_.find(me.character); e != entryOf_.end() && entries_[e->second].home == Self()) {
        Publish(kBusEntryGone, 0, e->second, 0, 0, 0, 0, {});
        entries_.erase(e->second);
        entryOf_.erase(e);
    }
}

void Parties::MovedAway(std::uint32_t entity) {
    const std::optional<PartyHost::Player> me = Here(entity);
    DropProposals(entity, me);
    if (me) moving_[me->character] = *me;
}

void Parties::Arrived(std::uint32_t entity) {
    const std::optional<PartyHost::Player> me = Here(entity);
    if (!me) return;
    Publish(kBusArrived, 0, 0, 0, 0, 0, 0, proto::Encode(ToWire(*me)));
    MemberArrived(*me);
}

// A member (or a listing's leader) is on another server now: what keeps it says so to the rest.
void Parties::MemberArrived(const PartyHost::Player& me) {
    if (Party* party = PartyOfCharacter(me.character); party != nullptr && party->home == Self()) {
        for (Member& m : party->members) {
            if (m.character != me.character) continue;
            m.entity = me.entity;
            m.server = me.server;
        }
        PublishState(*party, me.character);
        TellHere(*party, InfoOf(*party), 0);
    }
    if (const auto e = entryOf_.find(me.character); e != entryOf_.end() && entries_[e->second].home == Self()) {
        Entry& entry = entries_[e->second];
        entry.leader = me;
        PublishEntry(entry);
    }
}

void Parties::MoveFailed(std::uint32_t character) {
    const auto it = moving_.find(character);
    if (it == moving_.end()) return;
    const PartyHost::Player me = it->second;
    moving_.erase(it);
    CharacterGone(me);
    Publish(kBusLeft, 0, 0, 0, 0, 0, 0, proto::Encode(ToWire(me)));
}

void Parties::LeaveWorld(std::uint32_t entity) {
    const std::optional<PartyHost::Player> me = Here(entity);
    DropProposals(entity, me);
    if (!me) return;
    CharacterGone(*me);
    Publish(kBusLeft, 0, 0, 0, 0, 0, 0, proto::Encode(ToWire(*me)));
}

// Requests to it, from it, waiting on it.
void Parties::DropProposals(std::uint32_t entity, const std::optional<PartyHost::Player>& me) {
    if (const auto p = proposals_.find(entity); p != proposals_.end()) {
        Result(p->second.inviter, proto::PartyAction::Invite, proto::PartyStatus::InvalidUser, me ? me->name : std::string());
        proposals_.erase(p);
    }
    for (auto it = proposals_.begin(); it != proposals_.end();) {
        if (it->second.inviter == entity) {
            Result(it->first, proto::PartyAction::Answer, proto::PartyStatus::NoInviter);
            it = proposals_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Parties::Chat(std::uint32_t entity, const proto::ChatLine& line) {
    const Party* party = PartyAt(entity);
    const std::optional<PartyHost::Player> me = Here(entity);
    if (party == nullptr || !me) return false;
    TellParty(*party, line, me->character);
    return true;
}

void Parties::Tick(Clock::time_point now) {
    // The other servers: this one is still here; one long silent is gone.
    if (host_.PartyOnBus()) {
        if (now >= nextAlive_) {
            nextAlive_ = now + kAliveEvery;
            Publish(kBusAlive, 0, 0, 0, 0, 0, 0, {});
        }
        std::vector<std::uint16_t> silent;
        for (const auto& [server, heard] : peers_) {
            if (now - heard > kPeerSilence) silent.push_back(server);
        }
        for (const std::uint16_t server : silent) ServerGone(server);
    }
    // Requests whose 30 s ran out: the asker told, the asked one's box closed.
    for (auto it = proposals_.begin(); it != proposals_.end();) {
        if (now < it->second.until) {
            ++it;
            continue;
        }
        const std::optional<PartyHost::Player> invited = Here(it->first);
        Result(it->second.inviter, proto::PartyAction::Invite, proto::PartyStatus::Timeout, invited ? invited->name : std::string());
        Result(it->first, proto::PartyAction::Answer, proto::PartyStatus::Timeout);
        it = proposals_.erase(it);
    }
    for (auto it = joinRequests_.begin(); it != joinRequests_.end();) {
        if (now < it->second.until) {
            ++it;
            continue;
        }
        MatchAnswer(it->second.applicant, proto::PartyMatchAction::Answer, proto::PartyStatus::Timeout, it->second.entry);
        MatchAnswer(it->second.leader, proto::PartyMatchAction::Answer, proto::PartyStatus::Timeout, it->second.entry);
        applying_.erase(it->second.applicant.character);
        it = joinRequests_.erase(it);
    }
    // The members' news, four times a second (ours): level, HP and MP by the tenth, where they are
    // (every 2 s when moved more than 10). Each server tells of its own members.
    if (now < nextNews_) return;
    nextNews_ = now + std::chrono::milliseconds(250);
    for (auto& [id, party] : parties_) {
        for (Member& m : party.members) {
            const std::optional<PartyHost::Player> p = HereOf(m);
            if (!p) continue;
            proto::PartyMemberUpdate u;
            u.member = m.character;
            u.level = std::uint8_t(std::clamp(p->level, 1, 255));
            u.hpTenths = game::PartyTenths(p->hp, p->maxHp, p->alive);
            u.mpTenths = game::PartyTenths(p->mp, p->maxMp, false);
            u.entity = m.entity;
            u.mastery1 = p->mastery1;
            u.mastery2 = p->mastery2;
            if (u.level != m.level) u.mask |= proto::kPartyLevel;
            if (u.mastery1 != m.mastery1 || u.mastery2 != m.mastery2) u.mask |= proto::kPartyMasteries;
            if (u.hpTenths != m.hp || u.mpTenths != m.mp) u.mask |= proto::kPartyVitals;
            if (p->position && now - m.positionAt >= std::chrono::seconds(2) && !Within(*p->position, m.position, 10.0, true)) {
                u.mask |= proto::kPartyPosition;
                u.position = *p->position;
            }
            if (u.mask == 0) continue;
            m.level = u.level;
            m.hp = u.hpTenths;
            m.mp = u.mpTenths;
            m.mastery1 = u.mastery1;
            m.mastery2 = u.mastery2;
            if ((u.mask & proto::kPartyPosition) != 0) {
                m.position = u.position;
                m.positionAt = now;
            } else {
                u.position = m.position;
            }
            TellParty(party, u);
        }
    }
}

// --- Rewards and loot --------------------------------------------------------------------------------

Parties::Kill Parties::ShareKill(const std::vector<std::pair<std::uint32_t, std::uint64_t>>& damageBy, int monsterLevel, bool partyMonster,
                                 const std::optional<proto::WorldPosition>& at) const {
    Kill out;
    std::uint64_t total = 0;
    for (const auto& [who, amount] : damageBy) total += amount;
    if (total == 0) return out;
    // The damage grouped: an exp-shared party's members
    // as one, everyone else alone; each group's best damager.
    struct Group {
        const Party* party = nullptr;
        std::uint32_t solo = 0;
        std::uint64_t damage = 0, bestDamage = 0;
        std::uint32_t best = 0;
    };
    std::vector<Group> groups;
    for (const auto& [who, amount] : damageBy) {
        if (amount == 0) continue;
        const std::optional<PartyHost::Player> p = Here(who);
        const Party* party = p ? PartyOfCharacter(p->character) : nullptr;
        if (party != nullptr && (party->settings & game::kPartyExpShare) == 0) party = nullptr;
        auto g = std::find_if(groups.begin(), groups.end(), [&](const Group& x) { return party != nullptr ? x.party == party : x.party == nullptr && x.solo == who; });
        if (g == groups.end()) {
            groups.push_back({party, party != nullptr ? 0u : who});
            g = groups.end() - 1;
        }
        g->damage += amount;
        if (amount > g->bestDamage) {
            g->bestDamage = amount;
            g->best = who;
        }
    }
    if (groups.empty()) return out;
    const Group& top = *std::max_element(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.damage < b.damage; });
    out.owner = top.best;
    // The shared experience reaches members near the owner (where it fell when the owner is gone).
    std::optional<proto::WorldPosition> centre = at;
    if (const std::optional<PartyHost::Player> owner = Here(out.owner); owner && owner->position) centre = owner->position;
    for (const Group& g : groups) {
        const double part = double(g.damage) / double(total);
        if (g.party == nullptr) {
            const std::optional<PartyHost::Player> p = Here(g.solo);
            if (!p) continue;
            const Party* party = PartyOfCharacter(p->character); // one whose exp is not shared
            const double factor = game::SoloExperienceFactor(party != nullptr ? int(party->members.size()) : 0, p->level, monsterLevel, partyMonster);
            out.shares.push_back({g.solo, part * factor});
            continue;
        }
        // The members alive, in the world, within 1000 on the ground, each its share of the party's part; on this server
        // (the others are elsewhere).
        std::vector<game::PartyKiller> eligible;
        std::vector<std::uint32_t> entities;
        for (const Member& m : g.party->members) {
            const std::optional<PartyHost::Player> p = HereOf(m);
            if (!p || !p->alive || !p->position || !centre || !Within(*p->position, *centre, game::kPartyShareRange, false)) continue;
            eligible.push_back({p->level, p->race == 1});
            entities.push_back(m.entity);
        }
        const game::PartyExperience shares = game::PartyExperienceShares(eligible, monsterLevel, partyMonster);
        for (std::size_t i = 0; i < entities.size(); ++i) out.shares.push_back({entities[i], part * shares.factors[i]});
    }
    return out;
}

std::uint32_t Parties::LootRecipient(std::uint32_t picker) {
    const std::optional<PartyHost::Player> me = Here(picker);
    Party* party = me ? PartyOfCharacter(me->character) : nullptr;
    if (party == nullptr || (party->settings & game::kPartyItemShare) == 0 || !me->position) return picker;
    // Each member in turn, as many tries as members: the first alive and within 1000 of the picker
    // takes it; none: the picker.
    for (std::size_t tries = party->lootTurn.size(); tries > 0; --tries) {
        const std::uint32_t character = party->lootTurn.front();
        party->lootTurn.pop_front();
        party->lootTurn.push_back(character);
        const auto m = std::find_if(party->members.begin(), party->members.end(), [&](const Member& x) { return x.character == character; });
        if (m == party->members.end()) continue;
        const std::optional<PartyHost::Player> p = HereOf(*m);
        if (p && p->alive && p->position && Within(*p->position, *me->position, game::kPartyShareRange, true)) return m->entity;
    }
    return picker;
}

std::vector<std::pair<std::uint32_t, std::uint64_t>> Parties::SplitGold(std::uint32_t picker, std::uint64_t amount) const {
    const std::optional<PartyHost::Player> me = Here(picker);
    const Party* party = me ? PartyOfCharacter(me->character) : nullptr;
    if (party == nullptr || (party->settings & game::kPartyItemShare) == 0 || !me->position) return {{picker, amount}};
    // The picker, then the members within 1000 of it.
    std::vector<std::uint32_t> receivers{picker};
    for (const Member& m : party->members) {
        if (m.server == Self() && m.entity == picker) continue;
        const std::optional<PartyHost::Player> p = HereOf(m);
        if (p && p->position && Within(*p->position, *me->position, game::kPartyShareRange, true)) receivers.push_back(m.entity);
    }
    const std::vector<std::uint64_t> shares = game::SplitPartyGold(amount, receivers.size());
    std::vector<std::pair<std::uint32_t, std::uint64_t>> out;
    for (std::size_t i = 0; i < receivers.size(); ++i) out.push_back({receivers[i], shares[i]});
    return out;
}

void Parties::TellLoot(std::uint32_t recipient, std::uint32_t refId, std::uint16_t count, std::uint64_t gold) {
    const std::optional<PartyHost::Player> p = Here(recipient);
    const Party* party = p ? PartyOfCharacter(p->character) : nullptr;
    if (party == nullptr) return;
    // An item to the whole party (the loot notice; not for gold); a share of gold to the one who got it (ours: how the
    // original tells it is not known yet).
    if (refId == 0) return host_.Tell(recipient, proto::PartyLoot{p->character, 0, 0, gold});
    TellParty(*party, proto::PartyLoot{p->character, refId, count, gold});
}

} // namespace sr::server
