#include "sr/server/Community.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <unordered_map>

#include "sr/core/Log.h"
#include "sr/server/CommunityStore.h"
#include "sr/server/Database.h"
#include "sr/server/ShardBus.h"

namespace sr::server {

namespace proto = game::proto;
using Code = proto::CommunityCode;
using Topic = proto::CommunityTopic;

namespace {

enum : std::uint8_t {
    kBusHello = 1,
    kBusAlive = 2,
    kBusBye = 3,
    kBusEntered = 4,  // a character came online; `list` its friends: who of them is on the receiver?
    kBusPresent = 5,  // the answer: of those, `list` are here
    kBusLeft = 6,
    kBusAsk = 7,      // a friend proposal to the character of that name
    kBusAskSeen = 8,  // the server holding it says so
    kBusReply = 9,    // how a proposal ended, when it did not end in a friendship
    kBusAdded = 10,   // the proposal was accepted and written
    kBusRemoved = 11, // a friendship is over
    kBusLetter = 12,  // a letter reached a character on the receiver
};

constexpr auto kAliveEvery = std::chrono::seconds(5);
constexpr auto kPeerSilence = std::chrono::seconds(16); // three heartbeats missed
constexpr auto kAskSeenWithin = std::chrono::seconds(1);
constexpr auto kAskAnswerWithin = std::chrono::duration<double>(proto::kFriendAnswerSeconds);

struct Envelope {
    std::uint16_t from = 0, to = 0; // to 0: every server
    std::uint8_t kind = 0;
    std::uint32_t request = 0;
    std::uint32_t character = 0, other = 0;
    std::uint32_t model = 0;
    std::uint16_t value = 0; // a code, or the unread letters
    std::string name, second; // a proposal: the name asked for, the asker's
    std::vector<std::uint32_t> list;
    template <typename V>
    void Visit(V& v) {
        v("from", from);
        v("to", to);
        v("kind", kind);
        v("request", request);
        v("character", character);
        v("other", other);
        v("model", model);
        v("value", value);
        v("name", name, 64);
        v("second", second, 64);
        v("list", list, proto::kMaxFriends);
    }
};

std::string Key(std::string_view name) {
    std::string out(name);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

std::size_t Letters(std::string_view utf8) {
    return std::size_t(std::count_if(utf8.begin(), utf8.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

template <typename T>
std::vector<std::byte> Bytes(const T& message) {
    return proto::Encode(message);
}

} // namespace

struct Community::State {
    struct Friend {
        std::int64_t character = 0;
        std::string name;
        std::uint32_t model = 0;
        bool on = false;
        std::uint16_t server = 0; // where it is, while on
    };
    struct Online {
        std::uint64_t peer = 0;
        std::string name;
        std::uint32_t model = 0;
        bool loaded = false;
        std::vector<Friend> friends;
        std::vector<std::string> blocked;
        int unread = 0;
    };
    struct Asking {
        std::uint64_t peer = 0;
        std::int64_t character = 0;
        std::string target;
        Clock::time_point until{};
        Clock::time_point seenBy{};
        bool seen = false;
    };
    struct Proposal {
        std::int64_t asker = 0;
        std::string askerName;
        std::uint32_t askerModel = 0;
        std::uint16_t askerServer = 0;
        std::int64_t target = 0;
        Clock::time_point until{};
    };

    CommunityHost& host;
    std::unordered_map<std::int64_t, Online> online;
    std::unordered_map<std::string, std::int64_t> byName;
    // Who on this server has that character as a friend.
    std::unordered_map<std::int64_t, std::vector<std::int64_t>> watchers;
    std::map<std::uint32_t, Asking> asks;
    std::map<std::uint32_t, Proposal> proposals;
    std::map<std::uint16_t, Clock::time_point> peers;
    Clock::time_point nextAlive{};
    std::uint32_t requests = 0;

    explicit State(CommunityHost& h) : host(h) {}

    std::uint16_t Self() const { return host.CommunityServer(); }
    Online* Find(std::int64_t character) {
        const auto it = online.find(character);
        return it != online.end() ? &it->second : nullptr;
    }
    template <typename T>
    void Send(std::uint64_t peer, const T& message) {
        host.CommunitySend(peer, T::kId, Bytes(message));
    }
    void Tell(std::uint64_t peer, Topic topic, Code code, std::string name = {}) { Send(peer, proto::CommunityAnswer{topic, code, std::move(name)}); }
    void Publish(Envelope e) {
        if (!host.CommunityOnBus()) return;
        e.from = Self();
        host.CommunityPublish(BusToHex(proto::Encode(e)));
    }

    static proto::FriendRecord Record(const Friend& f) { return {std::uint32_t(f.character), f.name, f.model, std::uint8_t(f.on ? 1 : 0)}; }

    void Watch(std::int64_t who, std::int64_t watcher) { watchers[who].push_back(watcher); }
    void Unwatch(std::int64_t who, std::int64_t watcher) {
        const auto it = watchers.find(who);
        if (it == watchers.end()) return;
        std::erase(it->second, watcher);
        if (it->second.empty()) watchers.erase(it);
    }

    // `who` came online on `server`, or went: those here who have it as a friend are told.
    void Presence(std::int64_t who, bool on, std::uint16_t server, bool announce) {
        const auto it = watchers.find(who);
        if (it == watchers.end()) return;
        for (const std::int64_t watcher : it->second) {
            Online* o = Find(watcher);
            if (o == nullptr) continue;
            for (Friend& f : o->friends) {
                if (f.character != who) continue;
                f.on = on;
                f.server = server;
                Send(o->peer, proto::FriendPresence{std::uint32_t(who), std::uint8_t(on ? 1 : 0), std::uint8_t(announce ? 1 : 0)});
            }
        }
    }

    void ServerGone(std::uint16_t server) {
        peers.erase(server);
        for (auto& [character, o] : online) {
            for (Friend& f : o.friends) {
                if (!f.on || f.server != server) continue;
                f.on = false;
                Send(o.peer, proto::FriendPresence{std::uint32_t(f.character), 0, 1});
            }
        }
        std::erase_if(proposals, [&](const auto& p) { return p.second.askerServer == server; });
    }

    // --- A proposal's end ----------------------------------------------------------------------------

    void FriendGained(Online& o, std::int64_t self, Friend f) {
        const std::int64_t character = f.character;
        const auto have = std::find_if(o.friends.begin(), o.friends.end(), [&](const Friend& x) { return x.character == character; });
        if (have != o.friends.end()) return;
        o.friends.push_back(f);
        Watch(character, self);
        Send(o.peer, proto::FriendAdded{Record(f)});
    }

    void AskEnded(std::uint32_t request, Code code, const std::string& name) {
        const auto it = asks.find(request);
        if (it == asks.end()) return;
        Tell(it->second.peer, Topic::Friend, code, name);
        asks.erase(it);
    }

    void TellAsker(std::uint32_t request, const Proposal& p, Code code, const std::string& name) {
        if (p.askerServer == Self()) return AskEnded(request, code, name);
        Envelope e;
        e.to = p.askerServer;
        e.kind = kBusReply;
        e.request = request;
        e.value = std::uint16_t(code);
        e.name = name;
        Publish(std::move(e));
    }

    void Answer(const CommunityActor& actor, const proto::FriendReply& reply, Clock::time_point) {
        const auto it = proposals.find(reply.request);
        if (it == proposals.end() || it->second.target != actor.character) return;
        const Proposal p = it->second;
        proposals.erase(it);
        if (reply.accept == 0) return TellAsker(reply.request, p, Code::Refused, actor.name);
        struct Result {
            FriendWrite write = FriendWrite::Failed;
            std::uint32_t askerModel = 0, targetModel = 0;
        };
        auto result = std::make_shared<Result>();
        const std::uint32_t request = reply.request;
        host.CommunitySubmit(
            [p, actor, result](Database& db) { result->write = AddFriends(db, p.asker, p.askerName, actor.character, actor.name, result->askerModel, result->targetModel); },
            [this, p, actor, result, request] {
                Online* me = Find(actor.character);
                switch (result->write) {
                case FriendWrite::Ok: {
                    if (me != nullptr) FriendGained(*me, actor.character, {p.asker, p.askerName, result->askerModel, true, p.askerServer});
                    if (p.askerServer == Self()) {
                        asks.erase(request);
                        if (Online* asker = Find(p.asker)) FriendGained(*asker, p.asker, {actor.character, actor.name, result->targetModel, true, Self()});
                    } else {
                        Envelope e;
                        e.to = p.askerServer;
                        e.kind = kBusAdded;
                        e.request = request;
                        e.character = std::uint32_t(p.asker);
                        e.other = std::uint32_t(actor.character);
                        e.model = result->targetModel;
                        e.name = actor.name;
                        Publish(std::move(e));
                    }
                    return;
                }
                case FriendWrite::ListFull: // the asker's list
                    if (me != nullptr) Tell(me->peer, Topic::Friend, Code::TargetListFull, p.askerName);
                    return TellAsker(request, p, Code::FriendListFull, actor.name);
                case FriendWrite::TargetFull: // the one who said yes
                    if (me != nullptr) Tell(me->peer, Topic::Friend, Code::FriendListFull, p.askerName);
                    return TellAsker(request, p, Code::TargetListFull, actor.name);
                case FriendWrite::Already:
                    if (me != nullptr) Tell(me->peer, Topic::Friend, Code::AlreadyFriend, p.askerName);
                    return TellAsker(request, p, Code::AlreadyFriend, actor.name);
                case FriendWrite::Failed:
                    if (me != nullptr) Tell(me->peer, Topic::Friend, Code::NoAnswer, p.askerName);
                    return TellAsker(request, p, Code::NoAnswer, actor.name);
                }
            });
    }

    void Offer(std::uint32_t request, Online& target, std::int64_t targetCharacter, std::int64_t asker, const std::string& askerName, std::uint32_t askerModel,
               std::uint16_t askerServer, Clock::time_point now) {
        Proposal p;
        p.asker = asker;
        p.askerName = askerName;
        p.askerModel = askerModel;
        p.askerServer = askerServer;
        p.target = targetCharacter;
        p.until = kAskAnswerWithin.count() > 0 ? now + std::chrono::duration_cast<Clock::duration>(kAskAnswerWithin) + std::chrono::seconds(5)
                                               : Clock::time_point::max();
        proposals[request] = std::move(p);
        Send(target.peer, proto::FriendProposal{request, askerName});
    }

    void Ask(const CommunityActor& actor, const proto::FriendAsk& ask, Clock::time_point now) {
        Online* o = Find(actor.character);
        if (o == nullptr || !o->loaded) return;
        const std::string key = Key(ask.name);
        // The original's checks, in its order: the name, then the list.
        if (key.empty() || key == Key(actor.name) || !host.CommunityNameAllowed(ask.name)) return Tell(o->peer, Topic::Friend, Code::UnknownUser, ask.name);
        for (const Friend& f : o->friends) {
            if (Key(f.name) == key) return Tell(o->peer, Topic::Friend, Code::AlreadyFriend, ask.name);
        }
        if (o->friends.size() >= proto::kMaxFriends) return Tell(o->peer, Topic::Friend, Code::FriendListFull, ask.name);
        for (const auto& [request, a] : asks) {
            if (a.character == actor.character) return Tell(o->peer, Topic::Friend, Code::InProgress, ask.name);
        }
        const std::uint32_t request = (std::uint32_t(Self()) << 20) | (++requests & 0xFFFFF);
        Asking a;
        a.peer = o->peer;
        a.character = actor.character;
        a.target = ask.name;
        a.until = kAskAnswerWithin.count() > 0 ? now + std::chrono::duration_cast<Clock::duration>(kAskAnswerWithin) : Clock::time_point::max();
        if (const auto here = byName.find(key); here != byName.end()) {
            Online* target = Find(here->second);
            if (target == nullptr) return Tell(o->peer, Topic::Friend, Code::UnknownUser, ask.name);
            a.seen = true;
            asks[request] = std::move(a);
            Offer(request, *target, here->second, actor.character, actor.name, actor.model, Self(), now);
            return;
        }
        if (peers.empty()) return Tell(o->peer, Topic::Friend, Code::UnknownUser, ask.name);
        a.seenBy = now + kAskSeenWithin;
        asks[request] = std::move(a);
        Envelope e;
        e.kind = kBusAsk;
        e.request = request;
        e.character = std::uint32_t(actor.character);
        e.model = actor.model;
        e.name = key;
        e.second = actor.name;
        Publish(std::move(e));
    }

    void Delete(const CommunityActor& actor, const proto::FriendDelete& del) {
        Online* o = Find(actor.character);
        if (o == nullptr) return;
        const std::int64_t other = del.character;
        const auto it = std::find_if(o->friends.begin(), o->friends.end(), [&](const Friend& f) { return f.character == other; });
        if (it == o->friends.end()) return;
        auto done = std::make_shared<bool>(false);
        const std::int64_t me = actor.character;
        host.CommunitySubmit([me, other, done](Database& db) { *done = RemoveFriends(db, me, other); },
                             [this, me, other, done] {
                                 if (!*done) return;
                                 Dropped(me, other);
                                 if (Find(other) != nullptr) {
                                     Dropped(other, me);
                                 } else {
                                     Envelope e;
                                     e.kind = kBusRemoved;
                                     e.character = std::uint32_t(other);
                                     e.other = std::uint32_t(me);
                                     Publish(std::move(e));
                                 }
                             });
    }

    void Dropped(std::int64_t character, std::int64_t lost) {
        Online* o = Find(character);
        if (o == nullptr) return;
        const auto removed = std::erase_if(o->friends, [&](const Friend& f) { return f.character == lost; });
        if (removed == 0) return;
        Unwatch(lost, character);
        Send(o->peer, proto::FriendRemoved{std::uint32_t(lost)});
    }

    // --- Letters --------------------------------------------------------------------------------------

    void ReachLetter(std::int64_t character, int unread) {
        if (Online* o = Find(character)) {
            o->unread = unread;
            Send(o->peer, proto::LetterArrived{std::uint16_t(unread)});
            return;
        }
        Envelope e;
        e.kind = kBusLetter;
        e.character = std::uint32_t(character);
        e.value = std::uint16_t(unread);
        Publish(std::move(e));
    }

    void SendListing(std::uint64_t peer, std::int64_t character) {
        struct Loaded {
            std::vector<LetterRow> rows;
            bool ok = false;
        };
        auto loaded = std::make_shared<Loaded>();
        host.CommunitySubmit(
            [character, loaded](Database& db) {
                std::string error;
                loaded->ok = LoadLetters(db, character, loaded->rows, error);
            },
            [this, peer, loaded] {
                if (!loaded->ok) return;
                proto::LetterListing listing;
                for (const LetterRow& r : loaded->rows) {
                    if (listing.rows.size() >= proto::kMaxLetters + 64) break;
                    listing.rows.push_back({std::uint64_t(r.id), r.from, std::uint8_t(r.status != 0 ? 1 : 0), std::uint16_t(r.year), std::uint8_t(r.month),
                                            std::uint8_t(r.day), std::uint8_t(r.hour), std::uint8_t(r.minute)});
                }
                Send(peer, listing);
            });
    }

    void OpenLetter(const CommunityActor& actor, const proto::LetterOpen& open) {
        struct Read {
            LetterRow row;
            bool ok = false;
        };
        auto read = std::make_shared<Read>();
        const std::int64_t character = actor.character;
        const std::int64_t id = std::int64_t(open.id);
        host.CommunitySubmit([character, id, read](Database& db) { read->ok = ReadLetter(db, character, id, read->row); },
                             [this, peer = actor.peer, read] {
                                 if (read->ok) Send(peer, proto::LetterBody{std::uint64_t(read->row.id), read->row.from, read->row.text});
                             });
    }

    void WriteLetter(const CommunityActor& actor, const proto::LetterWrite& write) {
        if (write.to.empty() || Letters(write.text) == 0 || Letters(write.text) > proto::kLetterChars || !host.CommunityTextAllowed(write.text)) {
            return Tell(actor.peer, Topic::Letter, Code::LetterFailed, write.to);
        }
        struct Sent {
            LetterWriteResult result = LetterWriteResult::Failed;
            std::int64_t character = 0;
            int unread = 0;
        };
        auto sent = std::make_shared<Sent>();
        host.CommunitySubmit(
            [write, from = actor.name, sent](Database& db) { sent->result = AddLetter(db, write.to, from, write.text, proto::kMaxLetters, sent->character, sent->unread); },
            [this, peer = actor.peer, to = write.to, sent] {
                switch (sent->result) {
                case LetterWriteResult::Ok:
                    Tell(peer, Topic::Letter, Code::Ok, to);
                    ReachLetter(sent->character, sent->unread);
                    return;
                case LetterWriteResult::Unknown: return Tell(peer, Topic::Letter, Code::LetterUnknownUser, to);
                case LetterWriteResult::BoxFull: return Tell(peer, Topic::Letter, Code::TargetBoxFull, to);
                case LetterWriteResult::Failed: return Tell(peer, Topic::Letter, Code::LetterFailed, to);
                }
            });
    }

    void WriteMany(const CommunityActor& actor, const proto::LetterWriteMany& write) {
        // Who may: a guild at all, then its level by the kind (1 the guild's, 2 the union's).
        const int level = host.CommunityGuildLevel(actor.character);
        if (level == 0) return Tell(actor.peer, Topic::LetterMany, Code::NoGuild);
        if (write.kind != 1 && write.kind != 2) return;
        if (level < (write.kind == 1 ? 3 : 4)) return Tell(actor.peer, Topic::LetterMany, Code::GuildTooLow);
        if (write.names.empty() || Letters(write.text) == 0 || Letters(write.text) > proto::kLetterChars || !host.CommunityTextAllowed(write.text)) {
            return Tell(actor.peer, Topic::LetterMany, Code::LetterFailed);
        }
        // A filtered name ends the request (silently); a guild letter goes only to the guild's members (the others
        // dropped); a union letter keeps every name (the original's).
        proto::LetterWriteMany kept = write;
        kept.names.clear();
        for (const std::string& name : write.names) {
            if (!host.CommunityTextAllowed(name)) return;
            if (write.kind == 1 && !host.CommunityGuildMember(actor.character, name)) continue;
            kept.names.push_back(name);
        }
        if (kept.names.empty()) return Tell(actor.peer, Topic::LetterMany, Code::LetterFailed);
        struct Reached {
            std::string name;
            LetterWriteResult result = LetterWriteResult::Failed;
            std::int64_t character = 0;
            int unread = 0;
        };
        auto reached = std::make_shared<std::vector<Reached>>();
        host.CommunitySubmit(
            [write = std::move(kept), from = actor.name, reached](Database& db) {
                for (const std::string& name : write.names) {
                    Reached r;
                    r.name = name;
                    r.result = AddLetter(db, name, from, write.text, proto::kMaxLetters, r.character, r.unread);
                    reached->push_back(std::move(r));
                }
            },
            [this, peer = actor.peer, reached] {
                bool anyOk = false;
                for (const Reached& r : *reached) {
                    if (r.result == LetterWriteResult::Ok) {
                        anyOk = true;
                        ReachLetter(r.character, r.unread);
                    } else if (r.result == LetterWriteResult::BoxFull) {
                        Tell(peer, Topic::LetterMany, Code::TargetBoxFull, r.name);
                    }
                }
                if (anyOk) Tell(peer, Topic::Letter, Code::Ok);
            });
    }

    void Block(const CommunityActor& actor, const proto::WhisperBlock& block) {
        if (block.op != 1 && block.op != 2) return;
        struct Done {
            std::uint8_t result = 2;
            std::string name;
        };
        auto done = std::make_shared<Done>();
        done->name = block.name;
        const std::int64_t me = actor.character;
        const std::uint8_t op = block.op;
        host.CommunitySubmit(
            [me, op, done](Database& db) {
                if (op == 2) {
                    done->result = RemoveWhisperBlock(db, me, done->name) ? 0 : 2;
                    return;
                }
                switch (AddWhisperBlock(db, me, done->name)) {
                case BlockWrite::Ok: done->result = 0; break;
                case BlockWrite::Exists: done->result = 1; break;
                case BlockWrite::Unknown: done->result = 2; break;
                case BlockWrite::Full: done->result = 3; break;
                case BlockWrite::Failed: done->result = 4; break;
                }
            },
            [this, me, op, done] {
                Online* o = Find(me);
                if (o == nullptr) return;
                if (done->result == 0) {
                    if (op == 1) o->blocked.push_back(done->name);
                    if (op == 2) std::erase_if(o->blocked, [&](const std::string& n) { return Key(n) == Key(done->name); });
                }
                Send(o->peer, proto::WhisperBlockChanged{op, done->result, done->name});
            });
    }
};

Community::Community(CommunityHost& host) : s_(std::make_unique<State>(host)) {}
Community::~Community() = default;

void Community::Hello() {
    Envelope e;
    e.kind = kBusHello;
    s_->Publish(std::move(e));
}

std::string Community::GoodbyeText() const {
    if (!s_->host.CommunityOnBus()) return {};
    Envelope e;
    e.from = s_->Self();
    e.kind = kBusBye;
    return BusToHex(proto::Encode(e));
}

void Community::BusMessage(const std::string& text, Clock::time_point now) {
    State& s = *s_;
    const auto bytes = BusFromHex(text);
    Envelope e;
    if (!bytes || !proto::Decode(*bytes, e) || e.from == s.Self()) return;
    if (e.kind == kBusBye) return s.ServerGone(e.from);
    if (e.kind == kBusHello) {
        // Started (again: its players are gone).
        if (s.peers.contains(e.from)) s.ServerGone(e.from);
        s.peers[e.from] = now;
        Envelope alive;
        alive.kind = kBusAlive;
        alive.to = e.from;
        s.Publish(std::move(alive));
        return;
    }
    s.peers[e.from] = now;
    if (e.to != 0 && e.to != s.Self()) return;
    switch (e.kind) {
    case kBusEntered: {
        s.Presence(e.character, true, e.from, e.value == 0);
        Envelope here;
        here.kind = kBusPresent;
        here.to = e.from;
        here.character = e.character;
        for (const std::uint32_t id : e.list) {
            if (s.Find(id) != nullptr) here.list.push_back(id);
        }
        if (!here.list.empty()) s.Publish(std::move(here));
        return;
    }
    case kBusPresent: {
        State::Online* o = s.Find(e.character);
        if (o == nullptr) return;
        for (const std::uint32_t id : e.list) {
            for (State::Friend& f : o->friends) {
                if (f.character != std::int64_t(id)) continue;
                f.on = true;
                f.server = e.from;
                s.Send(o->peer, proto::FriendPresence{id, 1, 0});
            }
        }
        return;
    }
    case kBusLeft: return s.Presence(e.character, false, 0, true);
    case kBusAsk: {
        const auto here = s.byName.find(e.name);
        State::Online* target = here != s.byName.end() ? s.Find(here->second) : nullptr;
        if (target == nullptr) return; // another server may have it
        s.Offer(e.request, *target, here->second, e.character, e.second, e.model, e.from, now);
        Envelope seen;
        seen.kind = kBusAskSeen;
        seen.to = e.from;
        seen.request = e.request;
        s.Publish(std::move(seen));
        return;
    }
    case kBusAskSeen:
        if (const auto it = s.asks.find(e.request); it != s.asks.end()) it->second.seen = true;
        return;
    case kBusReply: return s.AskEnded(e.request, Code(e.value), e.name);
    case kBusAdded: {
        s.asks.erase(e.request);
        if (State::Online* asker = s.Find(e.character)) s.FriendGained(*asker, e.character, {e.other, e.name, e.model, true, e.from});
        return;
    }
    case kBusRemoved: return s.Dropped(e.character, e.other);
    case kBusLetter:
        if (State::Online* o = s.Find(e.character)) {
            o->unread = e.value;
            s.Send(o->peer, proto::LetterArrived{e.value});
        }
        return;
    default: return;
    }
}

void Community::Tick(Clock::time_point now) {
    State& s = *s_;
    if (s.host.CommunityOnBus()) {
        if (now >= s.nextAlive) {
            s.nextAlive = now + kAliveEvery;
            Envelope e;
            e.kind = kBusAlive;
            s.Publish(std::move(e));
        }
        std::vector<std::uint16_t> silent;
        for (const auto& [server, heard] : s.peers) {
            if (now - heard > kPeerSilence) silent.push_back(server);
        }
        for (const std::uint16_t server : silent) s.ServerGone(server);
    }
    for (auto it = s.asks.begin(); it != s.asks.end();) {
        const State::Asking& a = it->second;
        if (!a.seen && now >= a.seenBy) {
            s.Tell(a.peer, Topic::Friend, Code::UnknownUser, a.target);
        } else if (now >= a.until) {
            s.Tell(a.peer, Topic::Friend, Code::NoAnswer, a.target);
        } else {
            ++it;
            continue;
        }
        it = s.asks.erase(it);
    }
    std::erase_if(s.proposals, [&](const auto& p) { return now >= p.second.until; });
}

void Community::Entered(const CommunityActor& actor, bool arrived) {
    State& s = *s_;
    State::Online& entry = s.online[actor.character];
    entry = {};
    entry.peer = actor.peer;
    entry.name = actor.name;
    entry.model = actor.model;
    s.byName[Key(actor.name)] = actor.character;
    struct Loaded {
        CommunityRows rows;
        bool ok = false;
    };
    auto loaded = std::make_shared<Loaded>();
    const std::int64_t character = actor.character;
    const std::uint64_t peer = actor.peer;
    s.host.CommunitySubmit(
        [character, loaded](Database& db) {
            std::string error;
            loaded->ok = LoadCommunity(db, character, loaded->rows, error);
            if (!loaded->ok) core::LogWarning("community: {}: {}", character, error);
        },
        [this, character, peer, loaded, arrived] {
            State& state = *s_;
            State::Online* o = state.Find(character);
            if (o == nullptr || o->peer != peer || !loaded->ok) return;
            o->loaded = true;
            o->blocked = loaded->rows.blocked;
            o->unread = loaded->rows.unread;
            proto::CommunityLists lists;
            Envelope entered;
            entered.kind = kBusEntered;
            entered.character = std::uint32_t(character);
            entered.value = arrived ? 1 : 0;
            for (const FriendRow& row : loaded->rows.friends) {
                State::Friend f;
                f.character = row.character;
                f.name = row.name;
                f.model = row.model;
                f.on = state.Find(row.character) != nullptr;
                f.server = f.on ? state.Self() : 0;
                o->friends.push_back(f);
                state.Watch(row.character, character);
                lists.friends.push_back(State::Record(f));
                if (entered.list.size() < proto::kMaxFriends) entered.list.push_back(std::uint32_t(row.character));
            }
            lists.blocked = o->blocked;
            lists.unread = std::uint16_t(std::max(o->unread, 0));
            state.Send(o->peer, lists);
            state.Presence(character, true, state.Self(), !arrived);
            state.Publish(std::move(entered));
        });
}

void Community::MovedAway(std::int64_t character) {
    State& s = *s_;
    const auto it = s.online.find(character);
    if (it == s.online.end()) return;
    for (const State::Friend& f : it->second.friends) s.Unwatch(f.character, character);
    s.byName.erase(Key(it->second.name));
    s.online.erase(it);
    std::erase_if(s.proposals, [&](const auto& p) { return p.second.target == character; });
}

void Community::GoneAfterMove(std::int64_t character) {
    State& s = *s_;
    s.Presence(character, false, 0, true);
    Envelope e;
    e.kind = kBusLeft;
    e.character = std::uint32_t(character);
    s.Publish(std::move(e));
}

void Community::Left(std::int64_t character) {
    State& s = *s_;
    if (!s.online.contains(character)) return;
    MovedAway(character);
    GoneAfterMove(character);
}

bool Community::Handle(const CommunityActor& actor, proto::MessageId id, std::span<const std::byte> payload, Clock::time_point now) {
    State& s = *s_;
    const auto as = [&]<typename T>(T message, auto&& then) {
        if (!proto::Decode(payload, message)) return false;
        then(message);
        return true;
    };
    switch (id) {
    case proto::FriendAsk::kId: return as(proto::FriendAsk{}, [&](const proto::FriendAsk& m) { s.Ask(actor, m, now); });
    case proto::FriendReply::kId: return as(proto::FriendReply{}, [&](const proto::FriendReply& m) { s.Answer(actor, m, now); });
    case proto::FriendDelete::kId: return as(proto::FriendDelete{}, [&](const proto::FriendDelete& m) { s.Delete(actor, m); });
    case proto::LettersAsk::kId: return as(proto::LettersAsk{}, [&](const proto::LettersAsk&) { s.SendListing(actor.peer, actor.character); });
    case proto::LetterOpen::kId: return as(proto::LetterOpen{}, [&](const proto::LetterOpen& m) { s.OpenLetter(actor, m); });
    case proto::LetterWrite::kId: return as(proto::LetterWrite{}, [&](const proto::LetterWrite& m) { s.WriteLetter(actor, m); });
    case proto::LetterWriteMany::kId: return as(proto::LetterWriteMany{}, [&](const proto::LetterWriteMany& m) { s.WriteMany(actor, m); });
    case proto::LetterDelete::kId:
        return as(proto::LetterDelete{}, [&](const proto::LetterDelete& m) {
            const std::int64_t character = actor.character;
            const std::int64_t letter = std::int64_t(m.id);
            const std::uint64_t peer = actor.peer;
            s.host.CommunitySubmit([character, letter](Database& db) { DeleteLetter(db, character, letter); }, [this, peer, character] { s_->SendListing(peer, character); });
        });
    case proto::WhisperBlock::kId: return as(proto::WhisperBlock{}, [&](const proto::WhisperBlock& m) { s.Block(actor, m); });
    default: return true;
    }
}

bool Community::BlocksWhispersFrom(std::int64_t character, std::string_view sender) const {
    const auto it = s_->online.find(character);
    if (it == s_->online.end()) return false;
    const std::string key = Key(sender);
    return std::any_of(it->second.blocked.begin(), it->second.blocked.end(), [&](const std::string& n) { return Key(n) == key; });
}

} // namespace sr::server
