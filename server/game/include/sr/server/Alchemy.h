#pragma once

// Alchemy on the game server: the original's rules (game/Alchemy.h) and timing.
//
// - Strengthening (an elixir with or without lucky powder, a magic or an attribute stone on a piece of equipment in
//   the bag) is checked when asked, then completes on the 4th tick of the character's one-second tick (about 3..4 s),
//   checked again; a cancel or a death before that stops it with nothing used. What it used is gone either way (a
//   stone refused after its roll is kept).
// - Making a stone and dismantling happen at once; disjoining does its first stack at once and each next one three
//   ticks later.
// - Sockets: making one and erasing a stone at once (the bag's first maker, its first
//   Destruction Rondo); putting a stone in on the 4th tick as a strengthening, checked again then, its level the
//   lowest of three picks among its group's rows.
// - While something runs, the player's bag is held (the host refuses its moves); asking again is ignored.
//
// Alchemy is a player's own: its two players never meet, so a shard's two game servers need nothing from each other.
// The game server is its host.

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <unordered_map>
#include <vector>

#include "sr/game/Alchemy.h"
#include "sr/game/AlchemyProtocol.h"
#include "sr/game/Inventory.h"

namespace sr::assets {
class ItemTable;
struct MagicOptionRow;
struct MagicOptGroupRow;
struct MagicOptionAssignRow;
} // namespace sr::assets

namespace sr::server {

class AlchemyHost {
public:
    virtual ~AlchemyHost() = default;
    struct Alchemist {
        std::uint64_t peer = 0;
        bool alive = true;
        bool free = true; // in no exchange or stall
        game::Inventory* inventory = nullptr;
        int luck = 0, alchemy = 0; // the character's bonuses (the premium buffs' `luck` and `alcu`)
    };
    virtual std::optional<Alchemist> AlchemyPlayer(std::uint32_t entity) = 0;
    virtual void AlchemySend(std::uint64_t peer, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    virtual const assets::ItemTable* AlchemyItems() const = 0;
    virtual const std::vector<assets::MagicOptionRow>* AlchemyOptions() const = 0;
    // The magic option groups (_RefMagicOptGroup): the socket stones' skills by level (null: none).
    virtual const std::vector<assets::MagicOptGroupRow>* AlchemySocketGroups() const { return nullptr; }
    // The options each kind of item may get (magicoptionassign.txt, _RefMagicOptAssign): the avatars' lists at the smith
    // (null: no avatar is granted anything).
    virtual const std::vector<assets::MagicOptionAssignRow>* AlchemyAssign() const { return nullptr; }
    // Whether the NPC `npc` stands in the player's reach and offers `service` (NpcMenu.services).
    virtual bool AlchemyAtNpc(std::uint32_t /*entity*/, std::uint32_t /*npc*/, std::uint32_t /*service*/) const { return false; }
    // The stones a dismantle of that degree picks among (null: none).
    virtual const game::alchemy::StoneBucket* AlchemyStones(int degree) const = 0;
    // New items into the bag (a stack merged where its row stacks), all or none; the places they went to.
    virtual bool AlchemyGive(std::uint32_t entity, const std::vector<game::ItemInstance>& items, std::vector<std::uint8_t>& touched) = 0;
    // The bag changed at `touched` (`gone`: the items deleted): told to the player, then saved.
    virtual void AlchemyBag(std::uint32_t entity, std::vector<std::uint8_t> touched, std::vector<std::int64_t> gone) = 0;
    // A magic cube in the bag's place `cube` standing for an elixir: the elixir in its store's first place (none: it holds
    // none, or it is no cube), and one elixir used out of it (the last place's: the original's latest slot; told and
    // saved; false: none).
    virtual std::optional<game::ItemInstance> AlchemyCubeElixir(std::uint32_t /*entity*/, std::uint8_t /*cube*/) { return std::nullopt; }
    virtual bool AlchemyCubeUseOne(std::uint32_t /*entity*/, std::uint8_t /*cube*/) { return false; }
};

class Alchemy {
public:
    using Clock = std::chrono::steady_clock;

    explicit Alchemy(AlchemyHost& host, std::uint32_t seed = std::random_device{}());

    // A request of the player `entity` (false: it does not decode).
    bool Handle(std::uint32_t entity, game::proto::MessageId id, std::span<const std::byte> payload, Clock::time_point now);
    // The characters' one-second tick (each run's phase).
    void Tick(Clock::time_point now);
    // A death, a leave, a teleport: what runs stops (nothing used).
    void End(std::uint32_t entity);
    // Something runs: the bag is held.
    bool Busy(std::uint32_t entity) const { return runs_.contains(entity); }

private:
    struct Run {
        game::proto::AlchemyOperation operation = game::proto::AlchemyOperation::Strengthen;
        std::uint8_t target = 0, material = 0, powder = 0;
        std::int64_t targetSerial = 0, materialSerial = 0, powderSerial = 0;
        std::vector<std::uint8_t> queue; // disjoining: the stacks still to do
        int phase = 0;
        std::uint8_t socket = 0; // a socket stone's insertion: the socket (its stone in `material`)
        bool cube = false;       // a strengthening's elixir out of the magic cube in `material`
    };

    void Strengthen(std::uint32_t entity, const game::proto::AlchemyStrengthen& m);
    void Complete(std::uint32_t entity, Run& run);
    void Manufacture(std::uint32_t entity, const game::proto::AlchemyManufacture& m);
    void Disjoint(std::uint32_t entity, const game::proto::AlchemyDisjoint& m);
    // One disjoining stack; false when the run is over.
    bool DisjointStep(std::uint32_t entity, Run& run);
    void Dismantle(std::uint32_t entity, const game::proto::AlchemyDismantle& m);
    void SocketMake(std::uint32_t entity, const game::proto::SocketMake& m);
    void SocketInsert(std::uint32_t entity, const game::proto::SocketInsert& m);
    void SocketInserted(std::uint32_t entity, Run& run);
    void SocketErase(std::uint32_t entity, const game::proto::SocketErase& m);
    void AvatarGrant(std::uint32_t entity, const game::proto::AvatarGrant& m);
    // The bag's first item of a kind (the socket maker, the Destruction Rondo); -1 none.
    static int FirstOf(const game::Inventory& bag, const assets::ItemTable& items, bool (*kind)(const assets::ItemRow&));

    void Answer(std::uint32_t entity, const game::proto::AlchemyResult& result);
    void Refuse(std::uint32_t entity, game::proto::AlchemyOperation operation, std::uint16_t code, std::uint8_t place = 0);
    // The bag's Rondos of that codename, and taking `count` of them across stacks (the places touched, the stacks gone).
    int Held(const game::Inventory& bag, const assets::ItemRow* row) const;
    void Take(game::Inventory& bag, const assets::ItemRow* row, int count, std::vector<std::uint8_t>& touched, std::vector<std::int64_t>& gone) const;
    // One of the stack at `place` used (the last: the place empties).
    static void UseOne(game::Inventory& bag, std::uint8_t place, std::vector<std::uint8_t>& touched, std::vector<std::int64_t>& gone);

    AlchemyHost& host_;
    std::unordered_map<std::uint32_t, Run> runs_;
    Clock::time_point nextTick_{};
    std::mt19937 random_;
};

} // namespace sr::server
