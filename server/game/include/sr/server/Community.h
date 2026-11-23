#pragma once

// Friends, the whisper block list and letters on the game server: the original's rules, in the original's tables
// (CommunityStore.h).
//
// - A friend is proposed by name, wherever the character is; it has 30 s to answer (ours). Up to 20
//   friends each; both sides gain each other, or lose each other, at once.
// - The block list (20 names) refuses whispers from those names.
// - A letter goes to a character by name, online or not; the box holds 20, text up to 300 letters. A
//   letter to many: a guild's members from guild level 3, a union's masters from level 4.
//
// Across a shard's game servers everything goes over the bus (channel sr_community_<schema>): a proposal is published
// with the name and whoever holds that character says so; who is
// online crosses as players enter and leave, and a server gone silent takes its players offline. The
// lists are in the database and each player's are kept by the server the player is on.
//
// The game server is its host.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sr/game/CommunityProtocol.h"

namespace sr::server {

class Database;

struct CommunityActor {
    std::uint64_t peer = 0;
    std::int64_t character = 0;
    std::string name;
    std::uint32_t model = 0;
};

class CommunityHost {
public:
    virtual ~CommunityHost() = default;
    virtual void CommunitySend(std::uint64_t peer, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // `task` on a database thread, then `done` on the server's.
    virtual void CommunitySubmit(std::function<void(Database&)> task, std::function<void()> done) = 0;
    // The shard's bus: this game server's id, whether there is one, and publishing on it.
    virtual std::uint16_t CommunityServer() const { return 0; }
    virtual bool CommunityOnBus() const { return false; }
    virtual void CommunityPublish(std::string text) { (void)text; }
    // A character's name the name rules take; a letter's text the abuse filter takes.
    virtual bool CommunityNameAllowed(std::string_view name) const { return !name.empty(); }
    virtual bool CommunityTextAllowed(std::string_view text) const {
        (void)text;
        return true;
    }
    // The level of the guild a character is in (0: none).
    // Is `name` a member of `character`'s guild?
    virtual bool CommunityGuildMember(std::int64_t character, std::string_view name) const {
        (void)character;
        (void)name;
        return true;
    }
    virtual int CommunityGuildLevel(std::int64_t character) const {
        (void)character;
        return 0;
    }
};

class Community {
public:
    using Clock = std::chrono::steady_clock;

    explicit Community(CommunityHost& host);
    ~Community();

    // The bus: this server has started; what to publish when it stops; a message from it.
    void Hello();
    std::string GoodbyeText() const;
    void BusMessage(const std::string& text, Clock::time_point now);
    void Tick(Clock::time_point now);

    // `arrived`: from another game server of the shard, so its friends' lamps change but nothing is said.
    void Entered(const CommunityActor& actor, bool arrived = false);
    void Left(std::int64_t character);
    // Gone to another game server: nobody is told; GoneAfterMove if it never arrives.
    void MovedAway(std::int64_t character);
    void GoneAfterMove(std::int64_t character);
    // A request from a player in the world; false when it does not decode.
    bool Handle(const CommunityActor& actor, game::proto::MessageId id, std::span<const std::byte> payload, Clock::time_point now);

    // Whether a character on this server refuses whispers from that name.
    bool BlocksWhispersFrom(std::int64_t character, std::string_view sender) const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace sr::server
