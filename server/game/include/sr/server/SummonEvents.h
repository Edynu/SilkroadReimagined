#pragma once

// The two events the original's game server always runs for the items' uses (game/QuestItems.h), offered a quest or
// TID3 15 item after the player's quests:
//
// - The guild support's party summon scroll (QEV_GUILD_SUPPORT, ITEM_ETC_E060526_SUMMON_PARTY_SCROLL_A): a strong event
//   monster of the user's level (30..110), one for each member of its party (alone: one), each 20..100 away.
// - The Pandora box (QEV_PANDORA_BOX_100LV, ITEM_ETC_E060517_MON_GENERATION_BOX): the box (MOB_EVENT_PANDORA) comes up
//   by the user; its daemon, every 5 s for 20 minutes, brings four strong monsters 50 from the place whenever none of
//   them stands, each a random one of the ten strongest at or under the user's level. They go with the box (its death,
//   the daemon's end, its user gone).
//
// Both refuse (their words, the use then refused 2): in a town, under level 30, in the Battle Arena, under level 10.
// What they raise are the world's monsters (Monsters.h), on the user's game server: nothing reaches another player, so
// nothing goes over the shard's bus. The game server is its host.

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "sr/game/WorldProtocol.h"

namespace sr::server {

class SummonHost {
public:
    virtual ~SummonHost() = default;
    struct User {
        std::string name;
        int level = 1;
        game::proto::WorldPosition position;
        bool town = false;  // its region is a town's (_RefRegion IsBattleField 0)
        bool arena = false; // in a Battle Arena match
        int party = 0;      // its party's members, on every server (0: none)
    };
    virtual std::optional<User> SummonUser(std::uint32_t entity) = 0;
    // A text key said to the user (the original's 0x30EC).
    virtual void SummonSay(std::uint32_t entity, const std::string& key) = 0;
    // A monster of that codename at `at` (within `spread` of it), on ground reached from `from` (its layer), passive with
    // its sight, never refilled: its nest (none: no such monster or no ground there). Not `kept`, the nest goes by itself
    // once its monster is dead and lain; kept, it stays (SummonStanding asks it) until SummonRemove.
    virtual std::optional<std::size_t> SummonSpawn(std::string_view codename, const game::proto::WorldPosition& from, const game::proto::WorldPosition& at,
                                                   float spread, bool kept) = 0;
    // A kept nest's monsters standing (0: dead or gone), and the nest taken away with them.
    virtual int SummonStanding(std::size_t nest) = 0;
    virtual void SummonRemove(std::size_t nest) = 0;
    // A kept nest's monster casts a skill (its codename) now, at no target: those around see it (the box's cast after each
    // wave).
    virtual void SummonCast(std::size_t nest, std::string_view skill) {
        (void)nest;
        (void)skill;
    }
};

class SummonEvents {
public:
    using Clock = std::chrono::steady_clock;
    SummonEvents(SummonHost& host, std::uint32_t seed);

    // The item offered to the events (after the player's quests): true when one takes it (the caller uses it up).
    bool UseItem(std::uint32_t entity, const std::string& item, Clock::time_point now);
    // The Pandora daemons' runs.
    void Tick(Clock::time_point now);

    // What runs (tests).
    std::size_t Boxes() const { return boxes_.size(); }
    struct BoxView {
        std::uint32_t owner = 0;
        std::size_t box = 0;
        int runs = 0, casts = 0;
        std::vector<std::size_t> monsters;
    };
    std::vector<BoxView> BoxViews() const;

private:
    struct Box {
        std::uint32_t owner = 0;
        std::size_t box = 0;                    // the box's nest
        game::proto::WorldPosition place;       // where it was opened: the waves come up around it
        std::vector<std::string_view> monsters; // the ten
        int runs = 0;
        int casts = 0; // its skill cast after each wave 
        Clock::time_point next;
        std::vector<std::size_t> spawned; // its monsters' nests
    };
    bool Refused(std::uint32_t entity, const SummonHost::User& u, bool pandora);
    void GuildSupport(std::uint32_t entity, const SummonHost::User& u);
    void Pandora(std::uint32_t entity, const SummonHost::User& u, Clock::time_point now);
    void End(Box& b);
    game::proto::WorldPosition Around(const game::proto::WorldPosition& at, float near, float far);

    SummonHost& host_;
    std::mt19937 random_;
    std::vector<Box> boxes_;
};

} // namespace sr::server
