#pragma once

// The Academy on the game server (the original's "training camp"): founding (level 60, 100,000
// gold), inviting a player one sees and his answer, leaving, banning, disbanding, with the original's checks, codes and
// waits; the members' list, online and offline lines, the academy's chat. The state is the original's tables
// (AcademyStore.h); the rules are the original game server's and its procedures'.
//
// Ours: the original kept an academy in its shard manager; here each game server keeps the academies of its own players
// (loaded when one enters), writes every change at once and tells the shard's other servers over the bus (channel
// sr_academy_<schema>): they load that academy again and tell their members; presence and chat lines cross the same way.
//
// The game server is its host.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "sr/game/AcademyProtocol.h"
#include "sr/game/WorldProtocol.h"
#include "sr/server/Payment.h"

namespace sr::server {

class Database;

struct AcademyActor {
    std::uint64_t peer = 0;
    std::uint32_t entity = 0;
    std::int64_t character = 0;
    std::string name;
    std::uint32_t model = 0;
    int level = 1, maxLevel = 1;
    bool dead = false;
    std::uint64_t gold = 0;
    bool busy = false;      // in an exchange or at a stall 
    bool jobChange = false; // a job suit's change under way (0x6823)
};

class AcademyHost {
public:
    virtual ~AcademyHost() = default;
    virtual void AcademySend(std::uint64_t peer, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // A player in the world by its entity (the one invited must be on this server: the original's target is in view).
    virtual std::optional<AcademyActor> AcademyPlayerAt(std::uint32_t entity) const = 0;
    // The founding's fee (100,000 gold, reason 0x16); false when the player has too little; and back.
    // A payment of `character`'s with its work in one transaction (Payment.h), in its order of saves.
    virtual void AcademyPay(std::int64_t character, std::uint64_t fee, PaymentWork work, PaymentDone done) = 0;
    // `task` on a database thread, then `done` on the server's.
    virtual void AcademySubmit(std::function<void(Database&)> task, std::function<void()> done) = 0;
    // The shard's bus: this server's id, whether there is one, and publishing on it.
    virtual std::uint16_t AcademyServer() const { return 0; }
    virtual bool AcademyOnBus() const { return false; }
    virtual void AcademyPublish(std::string text) { (void)text; }
    // Unix seconds now (tests move it).
    virtual std::int64_t AcademyNow() const;
    // A self-graduating apprentice's experience (times the server's experience rate).
    virtual void AcademyGiveExperience(std::uint64_t peer, std::uint64_t experience) { (void)peer, (void)experience; }
    // The player's honour points (the honour shop's, the Character page's) as its academy row has them.
    virtual void AcademyHonorChanged(std::uint64_t peer, std::int64_t points) { (void)peer, (void)points; }
    // The guardian's honour buffs by its academy's rank (1..4; 5 none: the buffs off).
    virtual void AcademyHonorBuffs(std::uint64_t peer, std::uint8_t rank) { (void)peer, (void)rank; }
    // A storage keeper (the ranking's NPC) within reach of the player's entity.
    virtual bool AcademyNpcNear(std::uint32_t entity, std::uint32_t npc) const { (void)entity, (void)npc; return true; }
    // A storage keeper (NPC function 3) within reach of the player: the academy's founding asks at one.
    virtual bool AcademyKeeperNear(std::uint32_t entity, std::uint32_t npc) const { (void)entity, (void)npc; return true; }
    // The shard's forbidden words (the notice, the matching's subject).
    virtual bool AcademyTextAllowed(std::string_view text) const { (void)text; return true; }
    // Where a player's entity stands (the members' marks on the world map); nothing when unknown.
    virtual std::optional<game::proto::WorldPosition> AcademyPositionOf(std::uint32_t entity) const { (void)entity; return std::nullopt; }
};

class Academies {
public:
    using Clock = std::chrono::steady_clock;

    explicit Academies(AcademyHost& host);
    ~Academies();
    Academies(const Academies&) = delete;
    Academies& operator=(const Academies&) = delete;

    // The bus: this server started; a message from another server; what to publish when it stops.
    void Hello();
    void BusMessage(const std::string& text);
    std::string GoodbyeText() const;
    // Invitations and join requests not answered in time; the daily honour ranking when the day turns.
    void Tick(Clock::time_point now);
    // The honour ranking made again now (a game master's /academy rank; tests).
    void RankNow();

    // A player entered the world (its academy loaded and told; its members told it is online), or left it.
    // `arrived`: from another game server of the shard (no online line).
    void Entered(const AcademyActor& actor, bool arrived = false);
    void Left(std::int64_t character);
    // An academy's rows changed in the database under it (a member deleted for good): loaded again here and on the
    // shard's other servers.
    void RowsChanged(std::uint32_t academy);
    // Gone to another game server (nobody told).
    void MovedAway(std::int64_t character);
    void LevelChanged(std::int64_t character, int level, int maxLevel);
    // A request (AcademyProtocol.h); false when it does not decode.
    bool Handle(const AcademyActor& actor, game::proto::MessageId id, std::span<const std::byte> payload, Clock::time_point now);
    // An academy chat line from `actor` to its members, wherever they play.
    game::proto::ChatResult Chat(const AcademyActor& actor, const game::proto::ChatLine& line);
    // A monster killed by a member: an apprentice's accumulation grows by the kill's experience
    // over the monster's whole (`ofWhole`); a received buff spends its points. The percent more experience and skill
    // experience the kill gives (0..10).
    int Kill(std::int64_t character, double ofWhole);

    // The academy a player on this server belongs to (0: none or not loaded yet).
    std::uint32_t AcademyOf(std::int64_t character) const;
    // Writes still under way (tests wait for them).
    bool Busy() const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace sr::server
