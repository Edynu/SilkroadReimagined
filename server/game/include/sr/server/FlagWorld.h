#pragma once

// Capture the Flag (the original's FlagWorld) and Event So-Ok's desk, card and prize
// swap on a game server.
//
// - Every game server of the shard runs the same round (game::FlagCycle from the shard's schedule; the developer's
//   switches shift it on every server through the bus) and tells its own players its lines; So-Ok's desk takes their
//   registrations into the shard's queue (sr_event_entries, EventStore.h) with where each registered from.
// - The server owning the event map (region 0x8009; the host) forms the matches at the play's start (game::PlanMatches),
//   each a layer of the map (game::proto::WorldPosition::layer) with its poles, its two rooms' monsters, its doors; it
//   takes in its own players at once and tells the others' servers on the bus, whose players come over (a move to the
//   host's camp) and are taken in on arrival. Nothing of a running match crosses the bus.
// - In a match: the two teams are the original's relation 0x20 (foes, no records, no PK, KillRelation::Team); a death
//   rises at its camp 30 s later; the key, the doors, the flag (game::FlagMatch); the clock every 10 s; at the end one
//   Ice Trophy each to the winners (lost in a full bag, as the original's), the others home at once, the winners at the
//   stop. A character out of the game inside is kept at its home.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sr/game/FlagWorld.h"
#include "sr/game/WorldProtocol.h"
#include "sr/server/Monsters.h"

namespace sr::assets {
class CharacterTable;
class ItemTable;
struct ItemRow;
} // namespace sr::assets

namespace sr::server {

class Database;
class World;

class FlagWorldHost {
public:
    virtual ~FlagWorldHost() = default;
    struct Player {
        std::uint64_t peer = 0;
        std::int64_t character = 0;
        std::int64_t account = 0; // its JID (the card's row)
        std::string name;
        int level = 1;            // now
        game::FlagEntrant entrant; // its maxLevel the highest reached
        game::proto::WorldPosition at;
        bool dead = false;
    };
    virtual std::optional<Player> FlagPlayer(std::uint32_t entity) = 0;
    virtual std::uint32_t FlagEntityOf(std::int64_t character) = 0;
    virtual std::uint16_t FlagServerId() = 0;
    // The game server owning the event map's region (the host).
    virtual std::uint16_t FlagHostServer() = 0;
    virtual void FlagPublish(std::string text) = 0;
    virtual void FlagSend(std::uint32_t entity, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // Every player in this server's world.
    virtual void FlagSendAll(game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // Those who see it, itself too.
    virtual void FlagSendAround(std::uint32_t entity, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // Its flag team as those around see it (0xFF: none).
    virtual void FlagSetTeam(std::uint32_t entity, std::uint8_t team) = 0;
    // Out of its party, as its own leave (as the original's shard manager does before it puts a player into a match).
    virtual void FlagLeaveParty(std::uint32_t entity) = 0;
    // Taken there: a layer's place on this server at once; else a teleport, or a move to the server owning the place.
    virtual void FlagTravel(std::uint32_t entity, const game::proto::WorldPosition& to) = 0;
    // Items in its bag: given (false: no room, nothing given), counted, taken (false: too few, nothing taken), a free place.
    virtual bool FlagGive(std::uint32_t entity, std::string_view item, std::uint32_t count) = 0;
    virtual std::uint32_t FlagCount(std::uint32_t entity, std::string_view item) = 0;
    virtual bool FlagTake(std::uint32_t entity, std::string_view item, std::uint32_t count) = 0;
    virtual bool FlagRoom(std::uint32_t entity) = 0;
    // One of the item in a bag place used up (the card).
    virtual bool FlagUseUp(std::uint32_t entity, std::uint8_t slot) = 0;
    virtual void FlagSubmit(std::function<void(Database&)> task, std::function<void()> done) = 0;
    // A dead player up at `at` with its health and mana full (the camp's revival).
    virtual void FlagRevive(std::uint32_t entity, const game::proto::WorldPosition& at) = 0;
    // The carrier's buff (SKILL_EVENT_FLAG_GET_01) on or off.
    virtual void FlagBuff(std::uint32_t entity, bool on) = 0;
    // A pole's action started: its walk and its fight stopped.
    virtual void FlagHold(std::uint32_t entity) = 0;
    // Untouchable for a while (body mode 2 on entering the match world).
    virtual void FlagUntouchable(std::uint32_t entity, std::chrono::milliseconds time) = 0;
    virtual void FlagWalkTo(std::uint32_t entity, const game::proto::WorldPosition& to) = 0;
    // Whether a walk between the two is open (the dropped flag's test).
    virtual bool FlagWayClear(const game::proto::WorldPosition& from, const game::proto::WorldPosition& to) = 0;
    virtual std::int64_t FlagUnixNow() = 0;
    // A counted deed of a character (ours, sr_char_records: the beta quests; game/BetaQuests.h): a match played, Capture
    // the Flag's or the Battle Arena's (`event-matches`). A host that does not count them ignores it.
    virtual void FlagDeed(std::int64_t /*character*/, std::string_view /*name*/) {}
};

struct FlagWorldConfig {
    bool enabled = false;          // the event's switch (ours the shard's event-ctf)
    game::FlagSchedule schedule;   // the shard's ctf-times, or a cycle (--ctf-cycle)
    int minPlayers = game::flag::kMinPlayers;
    int teamMost = game::flag::kTeamMost;
    std::int64_t warmupMs = game::flag::kWarmupMs;
};

class FlagWorlds {
public:
    using Clock = std::chrono::steady_clock;
    FlagWorlds(FlagWorldHost& host, World& world, Monsters* monsters, const assets::CharacterTable* characters, FlagWorldConfig config,
               std::uint32_t seed);
    ~FlagWorlds();
    FlagWorlds(const FlagWorlds&) = delete;
    FlagWorlds& operator=(const FlagWorlds&) = delete;

    // A player in the world (one coming into a match is taken in).
    void Enter(std::uint32_t entity);
    // Gone from this server's world (out of the game, or to another server): its registration cancelled, its flag dropped.
    void Leave(std::uint32_t entity);
    // A request (false: not one of these, or it does not decode).
    bool Handle(std::uint32_t entity, game::proto::MessageId id, std::span<const std::byte> payload, bool devCommands);
    // An NPC talked to: true when it is a match's pole, key or flag (its action done here).
    bool Select(std::uint32_t entity, std::uint32_t target);
    // After an NpcMenu: Event So-Ok's desk (her state byte and her coded quests' rows).
    void TellDesk(std::uint32_t entity, std::uint32_t npc, std::string_view codename);
    // An item used from the bag: true when it is the Subscription Card (done here).
    bool UseItem(std::uint32_t entity, std::uint8_t slot, const assets::ItemRow& row);
    // A monster died (the key's carrier leaves the key).
    void Killed(const Monsters::Damaged& killed, const game::proto::WorldPosition& at);
    // A player died (`killer` the player who killed him, 0 none): his flag dropped, his revival queued, the trophy.
    void Died(std::uint32_t victim, std::uint32_t killer);
    // The death box's camp button: true when he is in a match (ours: up at the camp after the client's 10 s countdown).
    bool Resurrect(std::uint32_t entity);
    // A walk asked from `from` to `to` (a locked door in its way: FF 08).
    void Walked(std::uint32_t entity, const game::proto::WorldPosition& from, const game::proto::WorldPosition& to);
    void Tick(Clock::time_point now);
    void BusMessage(const std::string& text);
    // The developer's switch (FlagWorldDev's actions; on every server of the shard).
    void Dev(std::uint8_t action);

    // The two players are foes of one match (relation 0x20).
    bool Hostile(std::uint32_t a, std::uint32_t b) const;
    // In a match's layer (its own rules for PvP, death and revival).
    bool InMatch(std::uint32_t entity) const;
    // Where a player in a match comes back to (its home); none: not in one.
    std::optional<game::proto::WorldPosition> ReturnPlace(std::uint32_t entity) const;

    // Tests and tools.
    enum class Phase : std::uint8_t { Off, Waiting, Registration, Play, Winners };
    Phase CurrentPhase() const;
    bool Registered(std::uint32_t entity) const;
    std::size_t Matches() const;
    std::uint32_t LayerOf(std::uint32_t entity) const; // 0: none
    std::uint8_t TeamOf(std::uint32_t entity) const;   // 0xFF: none
    std::vector<std::uint32_t> PolesOf(std::uint32_t layer) const; // white, red, blue
    std::uint32_t KeyOf(std::uint32_t layer) const;     // the key lying (0: none)
    std::uint32_t DroppedFlagOf(std::uint32_t layer) const;
    std::vector<std::size_t> NestsOf(std::uint32_t layer) const; // red room's ten, then blue's
    bool DoorOpen(std::uint32_t layer, std::uint8_t team) const;
    const game::FlagMatch* MatchOf(std::uint32_t layer) const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace sr::server
