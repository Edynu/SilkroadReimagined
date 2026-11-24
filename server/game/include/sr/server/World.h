#pragma once

// The game server's world (C6b): who is where, how
// they walk, and who sees whom.
//
// - Ticks at a set rate (20 a second by default; 10, 30 or more as a setting). The world is split
//   into zones, squares of regions; each tick steps every zone as a job, all in parallel; a zone
//   touches only its own walkers. Between ticks the owner (the server's loop) adds and removes
//   players and gives them moves.
// - A move is checked when it is asked: the walk from where the walker is towards the point, on
//   the server's own navigation (game/NavMesh.h, the client's rules): it ends where the way is
//   blocked. The walker then goes there in a straight leg at its run speed; the server never takes
//   a position from a client, so it cannot be too fast or go through walls.
// - Who sees whom: each player sees what stands within a radius of it (a grid of cells finds the
//   candidates), by kind, the nearest first, each kind up to its share (players, monsters, items on
//   the ground); a share's unused places go to the nearest of what the other kinds have left over,
//   so the view fills one way or the other. NPCs within the radius always show. What it already
//   sees ranks a little nearer and stays a little farther (no flicker at the edge). Its view is
//   ranked by distance into three tiers: the nearest hear of a new leg at once, the next every 2nd
//   tick, the rest every 4th (a leg sent late starts from where the walker is then); the game
//   server sends combat's details only to the nearer tiers (ForEachObserverWithin). What changed
//   in a view each tick becomes messages: appeared, gone, and new legs (on the latest lane: only the
//   newest leg of a walker matters).
// - A player enters once the navigation around it is loaded; its first message is its own
//   character (WorldEntered), as the original sends its character's data before anything else.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "sr/game/WorldProtocol.h"

namespace sr::assets {
class FileSystem;
}
namespace sr::core {
class JobSystem;
}

namespace sr::server {

using WorldClock = std::chrono::steady_clock;

struct WorldConfig {
    double tickRate = 20.0;       // ticks a second
    int zoneRegions = 16;         // a zone: a square of regions this many wide
    // The defaults (decided 2026-11-07, measured with 1,000 bots packed near Jangan on 6 cores: about
    // 17 ms a tick at most, 11-13 KB/s a player): 600 map units, 300 players, 150 monsters, 50 items.
    float viewRadius = 6000.0f;   // a player sees this far (world units: a region is 1920, the minimap's are a tenth)
    std::size_t viewPlayers = 300, viewMonsters = 150, viewItems = 50; // the shares, the nearest of each kind
    std::size_t viewTier1 = 150, viewTier2 = 150; // the nearest this many: every tick; the next: every 2nd; the rest: every 4th
    float viewKeep = 0.1f;        // what is seen already ranks this much nearer and stays this much farther
    float gridCell = 960.0f;      // the candidates' grid (half a region)
    float walkSpeed = 16.0f;      // player figures (characterdata columns 46, 47)
    float runSpeed = 50.0f;
    float maxMove = 2.0f * 1920.0f; // a click no farther than this
    int maxCatchUp = 4;           // ticks run at once after a stall; the rest are dropped
    // Its game server's id: the shard's servers give entity ids from ranges of their own (server N from N x 2^28;
    // ids 0..15 apart, a higher id shares id % 16's range), so an id names one entity across the shard. A range used
    // up starts again from its bottom, past the ids still in use.
    std::uint16_t server = 0;
    std::uint32_t idRange = 0; // tests: a smaller range than 2^28 (its wrap)
};

// A player's character entering the world.
struct WorldPlayer {
    std::uint64_t peer = 0;
    std::int64_t character = 0;
    game::proto::CharacterSummary summary;
    game::proto::WorldPosition position;
    float yaw = 0;
    bool dropOnGround = false; // put down on the surface nearest its height (a first entry)
};

// A message's bytes, read-only and shared by everyone they are told to: a walker's new leg is encoded once and goes to
// hundreds of players without a copy (a copy is an allocation; a crowd's tick holds thousands of them).
class SharedBytes {
public:
    SharedBytes() = default;
    SharedBytes(std::vector<std::byte> bytes) : bytes_(std::make_shared<const std::vector<std::byte>>(std::move(bytes))) {}
    std::span<const std::byte> Span() const { return bytes_ ? std::span<const std::byte>(*bytes_) : std::span<const std::byte>(); }
    operator std::span<const std::byte>() const { return Span(); }
    std::size_t size() const { return bytes_ ? bytes_->size() : 0; }
    bool empty() const { return size() == 0; }

private:
    std::shared_ptr<const std::vector<std::byte>> bytes_;
};

// What a tick sends.
struct WorldMessage {
    std::uint64_t peer = 0;
    game::proto::MessageId id = 0;
    SharedBytes payload;
    std::uint64_t latestKey = 0; // not 0: the latest lane, with this key
};

// How the ticks went since the last TakeStats (the bot benchmark, C6f): a tick is the zones'
// steps (in parallel), then who sees whom; its budget is 1 / tickRate.
struct WorldStats {
    std::uint64_t ticks = 0;
    std::uint64_t dropped = 0;      // ticks given up after a stall (beyond maxCatchUp)
    std::uint64_t overBudget = 0;   // ticks longer than their budget
    double tickMsSum = 0, tickMsMax = 0;
    double zoneMsMax = 0;           // the slowest zone's step
    double interestMsMax = 0;
    std::size_t zonesMax = 0;       // zones with walkers on a tick
    std::size_t entities = 0;
    std::uint64_t messages = 0;     // told to players
    std::uint64_t legs = 0;         // walks started (MoveTo taken)
    std::uint64_t zoneChanges = 0;  // walkers that walked into another zone
    std::uint64_t refused = 0;      // clicks not taken (too far, the navigation not in, no floor, a failed walk)
    std::uint64_t refusedFar = 0, refusedNavigation = 0, refusedFloor = 0, refusedWalk = 0; // the same by reason
    // Views: entries summed over every player and tick (an average = seen / watcherTicks), the
    // largest one view held, and by kind.
    std::uint64_t watcherTicks = 0, seen = 0, seenPlayers = 0, seenMonsters = 0, seenItems = 0;
    std::size_t seenMax = 0;
    std::uint64_t legsSent = 0, legsDeferred = 0; // legs told at once, and held for a farther tier's tick
    std::uint64_t viewsWorkedOut = 0; // views whose members were worked out again (the rest stood; by kind: these)

    // Another reading added to this one (counts summed, the largest kept, the entities as the later reading saw them):
    // each reader of the world's stats (the log, the site's load sample) keeps its own total.
    void Add(const WorldStats& w) {
        ticks += w.ticks;
        dropped += w.dropped;
        overBudget += w.overBudget;
        tickMsSum += w.tickMsSum;
        tickMsMax = std::max(tickMsMax, w.tickMsMax);
        zoneMsMax = std::max(zoneMsMax, w.zoneMsMax);
        interestMsMax = std::max(interestMsMax, w.interestMsMax);
        zonesMax = std::max(zonesMax, w.zonesMax);
        entities = w.entities;
        messages += w.messages;
        legs += w.legs;
        zoneChanges += w.zoneChanges;
        refused += w.refused;
        refusedFar += w.refusedFar;
        refusedNavigation += w.refusedNavigation;
        refusedFloor += w.refusedFloor;
        refusedWalk += w.refusedWalk;
        watcherTicks += w.watcherTicks;
        seen += w.seen;
        seenPlayers += w.seenPlayers;
        seenMonsters += w.seenMonsters;
        seenItems += w.seenItems;
        seenMax = std::max(seenMax, w.seenMax);
        legsSent += w.legsSent;
        legsDeferred += w.legsDeferred;
        viewsWorkedOut += w.viewsWorkedOut;
    }
};

class World {
public:
    // `files`: the game's archives, for the navigation; null: no navigation (every walk goes
    // straight, heights stay as given). `jobs` steps the zones and loads the navigation.
    World(const WorldConfig& config, core::JobSystem& jobs, const assets::FileSystem* files);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // The owner's thread only (between ticks).
    std::uint32_t Enter(const WorldPlayer& player);
    void Leave(std::uint32_t entity);
    // A walk there at its run speed (`walk`: its walk speed; monsters wandering).
    // Why a walk was not taken (WorldStats counts them; the server logs a player's).
    enum class MoveRefused : std::uint8_t { None, Unknown, Far, Navigation, Floor, Walk };
    MoveRefused MoveTo(std::uint32_t entity, const game::proto::WorldPosition& target, bool walk = false);
    // A walk toward `toward` and on past it the same way, leg after leg, until a leg stops short (a wall, the end of
    // the navigation) or another walk, a blink or a teleport takes over (a click on the sky: the original walks the
    // clicker on that way till something stops it; the user's report 2026-11-14).
    MoveRefused MoveToward(std::uint32_t entity, const game::proto::WorldPosition& toward);
    // Whether it walks toward a heading now.
    bool WalksToward(std::uint32_t entity) const;
    // Takes a player elsewhere at once (a teleport, C6e): it leaves its observers' view, waits for
    // the navigation there, is put on the ground and told WorldEntered again (its picture starts
    // afresh). False: not in the world.
    bool Teleport(std::uint32_t entity, const game::proto::WorldPosition& to);
    // A blink (a teleport skill): at once as far towards `to` as its navigation's way goes (no farther
    // than `most`), its observers told it stands there; where it ends (none: not in the world, or no
    // way on the navigation).
    std::optional<game::proto::WorldPosition> Blink(std::uint32_t entity, const game::proto::WorldPosition& to, float most);
    // Runs the ticks due by `now`, then works out what each player is told.
    void Update(WorldClock::time_point now, std::vector<WorldMessage>& out);
    WorldClock::time_point NextTick() const;

    struct Place {
        game::proto::WorldPosition position;
        float yaw = 0;
    };
    // Where it is now (none: not in the world, or not placed yet).
    std::optional<Place> Where(std::uint32_t entity) const;
    // Its characterdata model (0: not in the world).
    std::uint32_t ModelOf(std::uint32_t entity) const;
    bool Moving(std::uint32_t entity) const;
    // Where a walking entity's leg ends (none when it stands): a chaser's aim.
    std::optional<game::proto::WorldPosition> Heading(std::uint32_t entity) const;
    // An NPC standing at `position` facing `yaw` (characterdata RefObjID `model`): seen by the
    // players near it, never moving, watching nobody; placed at once (its height is the file's).
    // `name`: a custom NPC's (shown over its head; empty: the model's own).
    std::uint32_t AddNpc(std::uint32_t model, const game::proto::WorldPosition& position, float yaw, std::string_view name = {});
    // An item lying on the ground (itemdata RefObjID, a stack's count or the gold, its plus): an
    // entity seen by the players near it, like an NPC; Leave takes it away. `state` goes out as its
    // `rarity`: the original's byte for the name's colour (the item's rarity, | 1 for equipment with
    // magic options). `name`: specialty goods' owner (the alias of the trader who bought them) or a quest or event
    // item's (3/3/9, the player it belongs to), shown after the item's name.
    std::uint32_t AddGroundItem(std::uint32_t item, const game::proto::WorldPosition& position, float yaw, std::uint32_t amount,
                                std::uint8_t plus, std::uint8_t state = 0, std::string_view name = {});
    // A skill object (proto::EntityKind::SkillObject: the skill's id, its owner's entity) standing at
    // `position`, seen like an NPC; Leave takes it away.
    std::uint32_t AddSkillObject(std::uint32_t skill, std::uint32_t owner, const game::proto::WorldPosition& position, float yaw);
    // A monster (characterdata RefObjID `model`) put down on the ground near `position`'s height:
    // seen by the players near it, watching nobody, walking at its own speeds; 0 when the ground there
    // is not known (its navigation is not in, or no floor under the point).
    struct MonsterLook {
        std::uint32_t model = 0;
        std::uint8_t rarity = 0;
        std::uint8_t appearance = 0; // a bandit's look, told as its spawn's `amount`
        std::uint32_t hp = 0;
        float walkSpeed = 0, runSpeed = 0;
    };
    std::uint32_t AddMonster(const MonsterLook& look, const game::proto::WorldPosition& position, float yaw);
    // A monster that stands exactly at `position` (its height as given, the navigation not needed) and
    // never walks: a fortress's structure.
    std::uint32_t AddStructure(const MonsterLook& look, const game::proto::WorldPosition& position, float yaw);
    // A player's COS (proto::EntityKind::Cos): walks at its own speeds like a monster, seen as a
    // player is; `owner` the player's entity. Put down on the ground near `position`'s height (0: the ground there
    // is not known), or exactly there when the world has no navigation.
    std::uint32_t AddCos(const MonsterLook& look, std::uint32_t owner, const game::proto::WorldPosition& position, float yaw);
    // A monster's or a COS's characterdata row from now on (an attack pet's next level): newcomers see that one.
    void SetModel(std::uint32_t entity, std::uint32_t model);
    // A COS's name (a pet's, once named) from now on: newcomers see it in its appearance.
    void SetName(std::uint32_t entity, std::string name);
    // Its walk and run speeds (0: the configured players'), a walk under way at them at once: a rider takes its mount's.
    void SetSpeeds(std::uint32_t entity, float walkSpeed, float runSpeed);
    // `mount` carries `rider` (0: nothing does): it is put where the rider stands and from then on walks every leg
    // the rider walks (MoveTo, Blink), so the two stay together and the mount's observers see it go.
    void Carry(std::uint32_t rider, std::uint32_t mount);
    // The region's navigation is in: monsters can stand and walk there (it is held while players are
    // near: the regions around each player).
    bool NavigationLoaded(std::uint16_t region) const;
    // Whether this world walks on navigation at all (none: walks unchecked, a test's world).
    bool HasNavigation() const;
    // Where a straight walk from `from` toward `to` ends on the navigation (stopped where it is blocked: never inside
    // a wall or an object); `to` itself without navigation; none when `from` stands on none.
    std::optional<game::proto::WorldPosition> WalkEnd(const game::proto::WorldPosition& from, const game::proto::WorldPosition& to) const;
    // Stealth and invisibility (a 'hide' buff): hidden by `mask` at
    // `level` (0 mask: seen), it leaves the sight of every watcher that does not detect it, and
    // detection (a 'dtt' / 'dttp' buff): a watcher sees a hidden one when their masks share a bit and
    // the hide's level is not above its detection's. A hidden one still sees.
    void SetHidden(std::uint32_t entity, std::uint8_t mask, std::uint8_t level);
    void SetDetection(std::uint32_t entity, std::uint8_t mask, std::uint8_t level);
    // Its party (0: none): a hidden one's party members always see it.
    void SetParty(std::uint32_t entity, std::uint32_t party);
    // The event zones players walked into since the last call, in order (game::NavZonesEntered: a cell
    // of a zone entered, or a zone's edge crossed, as the original's navigation reports them): the game server takes them
    // through a TELEPORT zone's link. The zone's name as its navmesh writes it.
    struct ZoneEntered {
        std::uint32_t entity = 0;
        std::string zone;
    };
    std::vector<ZoneEntered> TakeZonesEntered();
    // An event zone standing in the way of walks (game::NavWorld::SetZoneBlocks) in a layer (an instance's door;
    // 0 the shared world's); not during an update.
    void SetNavZone(std::uint32_t key, const std::string& name, bool blocks, std::uint32_t layer = 0);
    // Its health as newcomers are told it.
    void SetHp(std::uint32_t entity, std::uint32_t hp);
    // Its states as newcomers are told them after its appearance (EntityStates; their time left
    // counted from `at`; empty: none).
    void SetStates(std::uint32_t entity, std::vector<game::proto::StateRecord> states, WorldClock::time_point at);
    // The buffs it holds (their skills) as newcomers are told them after its appearance (EntityBuffs;
    // empty: none).
    void SetBuffs(std::uint32_t entity, std::vector<std::uint32_t> skills);
    // A message newcomers are told after its appearance, with its states and buffs, one of each id (a
    // player's guild, EntityGuild; its berserk, EntityBerserk); an empty payload takes that one away.
    void SetExtra(std::uint32_t entity, game::proto::MessageId id, std::vector<std::byte> payload);
    // A player's health and mana and their most, as its next WorldEntered (a teleport) tells it.
    void SetVitals(std::uint32_t entity, std::uint32_t hp, std::uint32_t maxHp, std::uint32_t mp, std::uint32_t maxMp);
    // Its run speed times `factor` (a speed buff: 2 for +100 %): a walk under way goes on at it at once (Resume).
    void SetSpeedFactor(std::uint32_t entity, float factor);
    // Its pace now: its walk and run speeds (the run's with its factor), the speed of its walk under way (0: it
    // stands) and whether it walks rather than runs (none: no such entity).
    struct Pace {
        float walk = 0, run = 0, now = 0;
        bool walking = false;
    };
    std::optional<Pace> PaceOf(std::uint32_t entity) const;
    // A walk under way started again from where it stands to the same end, at its speeds now (told as a new leg).
    void Resume(std::uint32_t entity);
    // Its moves at its walk speed from its next leg on (a player's walk mode: the Action page's Walk / Run).
    void SetWalking(std::uint32_t entity, bool walking);
    void ForEachPlayer(const std::function<void(std::int64_t character, const Place& place)>& visit) const;
    // What it wears now (the look newcomers are told; those who see it already hear of the change
    // from the game server).
    void SetEquipment(std::uint32_t entity, std::vector<game::proto::EquippedItem> equipment);
    // The dress side's avatars it wears (shown to those who see it).
    void SetAvatars(std::uint32_t entity, std::vector<game::proto::EquippedItem> avatars);
    // A transform's look: what newcomers are shown of it instead of its
    // own (its model, height, equipment, avatars; none: its own again); and its own as the world keeps it.
    void SetShownLook(std::uint32_t entity, std::optional<game::proto::CharacterSummary> look);
    // A character's own model and height changed (the skin change).
    void SetModelLook(std::uint32_t entity, std::uint32_t model, std::uint8_t height);
    std::optional<game::proto::CharacterSummary> OwnLook(std::uint32_t entity) const;
    // The players who see `entity` in their view, and itself: who hears it speak (local chat).
    void ForEachObserver(std::uint32_t entity, const std::function<void(std::uint64_t peer)>& visit) const;
    // The same, only those whose view holds it in tier `maxTier` or nearer (0 the nearest; itself
    // counts as 0): combat's details for those near enough to see them.
    void ForEachObserverWithin(std::uint32_t entity, int maxTier, const std::function<void(std::uint64_t peer)>& visit) const;
    // A player's peer (0: not a player, or not in the world).
    std::uint64_t PeerOf(std::uint32_t entity) const;
    // A player's own limits on its view (0: the share): kept to at most the shares, and a kind it
    // lowered takes no lent places. Returns what applies now.
    struct ViewLimitsApplied {
        std::size_t players = 0, monsters = 0;
    };
    // `radius`: how far it sees in world units, at most the server's (0: the server's).
    ViewLimitsApplied SetViewLimits(std::uint32_t entity, std::size_t players, std::size_t monsters, float radius = 0.0f);
    const WorldConfig& Config() const;
    std::size_t Entities() const;
    std::uint64_t Ticks() const;
    double TickRate() const;
    // Zones that held walkers on the last tick, and how long the last tick's zone steps took.
    std::size_t ActiveZones() const;
    double LastStepMs() const;
    // The ticks since the last call (and starts counting afresh).
    WorldStats TakeStats();

private:
    // The next leg of a walk toward a heading: one walk's reach on along its way.
    MoveRefused WalkHeading(std::uint32_t entity);
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace sr::server
