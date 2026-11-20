#pragma once

// The NPC bandits on the game server: the original's caravans.
//
// - A caravan is a trader's or a thief's transport carrying specialty goods, registered once per player when goods go
//   into it (Trades::CargoChanged; the original also on an NPC's close and a transport's summoning, both with goods
//   aboard). Its first wave comes 60-120 s after, and again 60-120 s after each.
// - At its time: gone (the player away, no transport, no goods) the caravan is dropped; with the transport in a town
//   the wave is skipped; else a wave around the transport: the count and level from the trade's value and stars, thief
//   monsters against a trader, hunter monsters against a thief, of the player's continent's list, each within 15 m.
// - The bandits are the world's monsters (Monsters.h): their own tactics (whom they go for, between the trader and his
//   transport, gone 20 s after their last hit or in a town). Nothing links them to their caravan, as in the original.
//
// The game server is its host.

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>

#include "sr/game/Jobs.h"
#include "sr/game/WorldProtocol.h"
#include "sr/server/Monsters.h"

namespace sr::server {

class BanditHost {
public:
    virtual ~BanditHost() = default;
    // A player's caravan as it is now (none: the player gone, no transport out, or no specialty goods in it).
    struct Caravan {
        std::string name;                        // the player's (the log's)
        std::uint8_t jobState = game::kNoJob;    // 1 a trader, 2 a thief
        int level = 1;                           // the greater of its level and its highest mastery's (the bandits')
        int valueLevel = 1;                      // its highest level (the trade value's)
        std::string transportCodename;           // its capacity class
        game::proto::WorldPosition at;           // the transport's place
        std::uint16_t playerRegion = 0;          // the player's (the bandits' list by its continent)
        std::uint64_t goods = 0;                 // the specialty goods aboard
        bool bargain = false;                    // bargain goods among them
    };
    virtual std::optional<Caravan> BanditCaravan(std::uint32_t player) = 0;
    // `_RefRegion`: a town (IsBattleField 0) and the bandits' list of its continent (-1: none).
    virtual bool BanditTown(std::uint16_t region) const = 0;
    virtual int BanditZone(std::uint16_t region) const = 0;
    // Where a straight walk from the transport toward a bandit's point ends (none: no ground there).
    virtual std::optional<game::proto::WorldPosition> BanditGround(const game::proto::WorldPosition& from, const game::proto::WorldPosition& to) const = 0;
    // A bandit's nest put in the world (false: it found no ground there).
    virtual bool BanditSpawn(const MonsterNest& nest) = 0;
    virtual const game::JobRules& BanditRules() const = 0;
};

class Bandits {
public:
    using Clock = std::chrono::steady_clock;

    Bandits(BanditHost& host, game::BanditTable table, std::uint32_t seed);

    // Goods went into the player's transport: its caravan, unless it has one (its first wave 60-120 s on).
    void Register(std::uint32_t player, Clock::time_point now);
    void Tick(Clock::time_point now);
    std::size_t Caravans() const { return caravans_.size(); }
    // The waves' interval for tries (SroGame --bandit-seconds A,B; the original's 60, 60).
    void SetInterval(double minSeconds, double spanSeconds);
    // A wave at once (the tests' and the GM's way); the bandits made.
    int Wave(std::uint32_t player);

private:
    Clock::time_point Next(Clock::time_point now);

    BanditHost& host_;
    game::BanditTable table_;
    std::mt19937 random_;
    std::unordered_map<std::uint32_t, Clock::time_point> caravans_; // a player -> its next wave
    double minSeconds_ = game::kCaravanIntervalMs / 1000.0, spanSeconds_ = game::kCaravanIntervalMs / 1000.0;
};

} // namespace sr::server
