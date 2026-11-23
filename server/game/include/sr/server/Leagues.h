#pragma once

// The jobs' leagues at their NPCs on the game server: the original's rules (its 0x70E1 join, 0x70E2 leave, 0x70E3 the
// alias, 0x70E6 the previous information), our messages (JobProtocol.h):
//
// - Join: at a keeper of that league (game::LeagueOfNpc) in reach; level 20 or more; in no league; the fee
//   (level - 20) x 5,000; not within 7 days of leaving one. The league starts from the character's old level and
//   experience of that job (`_OldTrijob`, then gone); a trader's safe trade is made.
// - Leave: at its league's keeper; the suit off; no job quest under way. No fee; the alias goes with the league (never
//   given again); the 7-day wait starts.
// - The alias: at its league's keeper, in the league, the suit off; 2..12 bytes of the name rules' letters; free
//   of every character's name and every alias ever given (shard-wide, in the database). Asked first (a check),
//   then set; a change costs level x 50,000 (the first is free).
// - The previous information: the old levels of the three jobs, at any keeper.
//
// The league is the character's own: its changes go to the database before they are answered (the player's
// requests wait one at a time), and the host tells the player (and those near) what changed.
//
// The game server is its host.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "sr/game/JobProtocol.h"
#include "sr/game/Jobs.h"
#include "sr/server/Payment.h"

namespace sr::server {

class Database;

class LeagueHost {
public:
    virtual ~LeagueHost() = default;
    // A player in this server's world as the league needs it (none: not a player here). The pointers are the
    // player's own state, good until the host's next turn.
    struct Member {
        std::uint64_t peer = 0;
        std::int64_t character = 0;
        int level = 1;
        std::uint64_t gold = 0;
        std::uint8_t jobState = game::kNoJob; // from the suit worn
        bool jobQuest = false;                // a job quest under way
        game::JobInfo* job = nullptr;
        std::string* alias = nullptr;
        std::int64_t* leftAt = nullptr; // when it last left a league (Unix seconds; 0 none within 7 days)
    };
    virtual std::optional<Member> LeagueMember(std::uint32_t entity) = 0;
    // The league `npc` keeps when it stands within the talk's reach of `entity` (None: too far, or no keeper).
    virtual game::JobType LeagueKeeperNear(std::uint32_t entity, std::uint32_t npc) const = 0;
    // The name rules' letters (an alias's length is checked apart).
    virtual bool AliasLettersOk(std::string_view alias) const = 0;
    // A payment of `character`'s with its work in one transaction (Payment.h), in its order of saves, after every write
    // before it (as LeagueDatabase).
    virtual void LeaguePay(std::int64_t character, std::uint64_t fee, PaymentWork work, PaymentDone done) = 0;
    virtual void LeagueSend(std::uint64_t peer, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // The player's league or alias changed: the host tells it (JobInfoMessage) and those near.
    virtual void LeagueChanged(std::uint32_t entity) = 0;
    // `work` on the database after every write before it, then `done` on the host's turn.
    virtual void LeagueDatabase(std::function<void(Database&)> work, std::function<void()> done) = 0;
    // Unix seconds now (tests move it).
    virtual std::int64_t LeagueNow() const = 0;
};

class Leagues {
public:
    explicit Leagues(LeagueHost& host);
    void Rank(std::uint32_t entity, const game::proto::JobRankQuery& r);
    void Outcome(std::uint32_t entity, const game::proto::JobOutcome& r);

    // A request of the player `entity` (false: it does not decode, or is no request of ours).
    bool Handle(std::uint32_t entity, game::proto::MessageId id, std::span<const std::byte> payload);
    // Whether a message is one of the league's requests.
    static bool IsRequest(game::proto::MessageId id);

private:
    void Join(std::uint32_t entity, const game::proto::JobJoin& r);
    void Leave(std::uint32_t entity, const game::proto::JobLeave& r);
    void Alias(std::uint32_t entity, const game::proto::JobAlias& r);
    void Previous(std::uint32_t entity, const game::proto::JobPreviousQuery& r);
    void Answer(std::uint64_t peer, game::proto::JobRequest request, game::proto::JobCode code, std::uint8_t action = 0);
    // The player again after a database turn: the same character still here.
    std::optional<LeagueHost::Member> Again(std::uint32_t entity, std::int64_t character);

    LeagueHost& host_;
    std::unordered_set<std::uint32_t> waiting_; // players whose request is at the database
};

} // namespace sr::server
