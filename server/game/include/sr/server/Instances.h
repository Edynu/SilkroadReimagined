#pragma once

// The Forgotten World on the game server.
//
// - A Dimension Hole used by a party master in a town opens an instance of its world on this server (an instance
//   never moves between servers) and the Gap of Dimensions at the master's feet for GAMEWORLD_DISAPPEARANCE_TIME.
//   The master's party enters through the gap while it stands, each member checked (level band, alive, no job
//   clothes, not dealing, not in another instance, the re-entry limit, the world's players).
// - An instance is a layer of the world (game::proto::WorldPosition::layer): its monsters (its world's hives and
//   nests, spawned once, some waiting for a trigger), its NPCs (the exits), its doors (structures standing in the
//   way until a trigger opens them) are seen and met only in it. Its world's triggers run on its kills and
//   entrances (Triggers below).
// - A player leaves by an exit (back where it entered from), when the instance's time is over (everyone back), or
//   by leaving the game (it comes back where it entered from: its saved place). Nobody inside for
//   WorldEmptyRemainTime once its gap is gone, the instance ends.
// - World 1's triggers (the Dimension Pillars) run on the kills in the shared world.
// - The collection book: a talisman used from the bag is registered (_CharCollectionBook), one taken; the quests'
//   `collect` goals ask whether a theme is complete.
// - The party recall pillar (NPC_FWORLD_RECALL_PARTY): the party's master inside summons a member who stands in a town,
//   on this server or the shard's other: asked, it comes from its town into the instance, and goes back
//   to it when it leaves.

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "sr/game/InstanceProtocol.h"
#include "sr/game/WorldProtocol.h"
#include "sr/server/InstanceStore.h"
#include "sr/server/Monsters.h"

namespace sr::assets {
class CharacterTable;
class FileSystem;
class ItemTable;
struct ItemRow;
} // namespace sr::assets

namespace sr::server {

class Database;
class World;

// A trigger of the original's (_RefTrigger and its events, conditions and actions), as the database writes it.
struct TriggerSpec {
    std::uint32_t id = 0;
    std::string codename;
    bool active = true;
    bool repeat = false; // fires every time (else once an instance)
    std::vector<std::string> events; // EVENT_MONSTER_KILL, EVENT_WORLD_ENTRANCE (without TRIGGER_)
    struct Condition {
        std::string kind;            // CONDITION_MONSTER_HIVE_KILL, ...
        std::string onTrue, onFalse; // NEXT, RETURN_TRUE, RETURN_FALSE
        std::map<std::string, std::string> params;
    };
    std::vector<Condition> conditions; // in their sequence
    struct Action {
        std::string kind;       // ACTION_MONSTER_ACTIVE_NEST, ...
        std::uint32_t delayMs = 0;
        std::map<std::string, std::string> params;
    };
    std::vector<Action> actions;
    std::string Param(const Action& a, const std::string& name) const;
};

// An instance world (_RefGame_World 63..84) with what an instance of it holds.
struct GameWorldSpec {
    std::uint32_t id = 0;
    std::string codename;
    std::uint32_t maxCount = 50, maxUsers = 4;  // instances at once, players in one
    std::uint32_t durationMs = 7'200'000, emptyMs = 600'000;
    std::uint32_t gapMs = 300'000;              // GAMEWORLD_DISAPPEARANCE_TIME: the gap stands this long
    int minLevel = 1, maxLevel = 0;             // ENTER_LIMIT_CONDITION_LEVEL_* (0: none)
    bool noJobSuit = true;                      // ENTER_LIMIT_CONDITION_JOB_CLOTHES_TAKE_OFF
    game::proto::WorldPosition start;           // _RefInstance_World_Start_Pos
    std::vector<MonsterNest> nests;             // its monster nests (nestId, hive; waiting: btType 1)
    struct Npc {
        std::uint32_t refObjId = 0;
        std::string codename;
        game::proto::WorldPosition position;
        float yaw = 0;
    };
    std::vector<Npc> npcs;                      // its NPC hives' nests (the exits, the recall pillars)
    struct Door {
        std::string zone;          // POS_STRUCTURE_GOD_...
        std::uint32_t refObjId = 0; // its structure
        game::proto::WorldPosition position;
        float yaw = 0;
        std::uint32_t navKey = 0;  // objectstring.ifo's id
        std::string navName;       // the zone's name in lower case
    };
    std::vector<Door> doors;                    // the doors its triggers open
    std::vector<TriggerSpec> triggers;
    // A party world entered through a gate (the Pharaoh's tomb): its party's master opens it, the
    // members join his; `entriesPerDay` (ENTER_LIMIT_CONDITION_ENTRY_MAXIMUM_NUM, 0: none) entries a character a day; the
    // gate point that leads in (`_RefTeleport` of the world) and its name (the difficulty's, SN_ZONE_21836_10..12); the
    // warnings before its end (GAMEWORLD_DISAPPEARANCE_TIME, ms left).
    bool party = false;
    int entriesPerDay = 0;
    std::uint32_t gatePoint = 0;
    std::string nameKey;
    std::vector<std::uint32_t> warnMs;
    std::map<std::string, std::string> config; // its group's `_RefGame_World_Config` rows (the temples' doors and rules)
};

// A Dimension Hole (TID 3/3/12/7; _RefObjItem's Param1..10).
struct HoleSpec {
    std::uint32_t item = 0;    // its itemdata RefObjID
    std::uint32_t world = 0;   // the world it opens
    std::uint8_t grade = 1;
    int minLevel = 0, maxLevel = 0;
    std::string theme;         // the theme's name key
    std::uint32_t gapModel = 0; // the gap's object (INS_QUEST_TELEPORT)
    std::uint32_t waitMs = 0, waitGroup = 0; // its COOLTIME (Param10): 5 hours for every hole of group 0x71
};

struct CollectionTheme {
    std::uint32_t id = 0;
    std::string codename;
    std::uint8_t complete = 8; // CompleteNum: the slots it has
};
struct TalismanSpec {
    std::uint32_t theme = 0;
    std::uint8_t slot = 0;
};

struct GameWorldCatalog {
    std::vector<GameWorldSpec> worlds;
    std::optional<GameWorldSpec> siege; // the fortresses' Cave of Discipline (INS_SIEGE_DUNGEON), opened by a fortress manager
    std::vector<GameWorldSpec> tombs;   // the Pharaoh's tomb's three difficulties (INS_PHARAOH_*), entered through their gates
    std::vector<GameWorldSpec> temples; // the job temples (INS_TEMPLE_SELK .. _SETH), opened on their schedule
    std::map<std::string, std::string> templeArea; // GROUP_TEMPLE_DEFAULT's rows: the area's and every temple's entry rules
    const GameWorldSpec* TombOfPoint(std::uint32_t point) const;
    std::vector<TriggerSpec> sharedTriggers;                // world 1's (INS_DEFAULT)
    std::unordered_map<std::uint32_t, HoleSpec> holes;      // by item
    std::vector<CollectionTheme> themes;
    std::unordered_map<std::uint32_t, TalismanSpec> talismans; // by item
    std::uint32_t reentrySeconds = 300;                     // ENTER_WORLD_GROUP_COOL_TIME
    // The re-entry limit's timed job as the client shows it (TIMEDJOB_*_FORCLIENT).
    std::string jobIcon, jobSubject, jobText;
    std::uint32_t jobSeconds = 0;
    const GameWorldSpec* World(std::uint32_t id) const;
    const CollectionTheme* Theme(std::uint32_t id) const;
    const CollectionTheme* Theme(std::string_view codename) const;
};

// The catalog from the database's original tables and the client's files (objectstring.ifo, characterdata,
// itemdata); worlds whose places are unknown are left out (named in `notes`).
bool LoadGameWorlds(Database& db, const assets::FileSystem& files, const assets::CharacterTable& characters, const assets::ItemTable& items,
                    GameWorldCatalog& out, std::vector<std::string>& notes, std::string& error);

class InstanceHost {
public:
    virtual ~InstanceHost() = default;
    struct Player {
        std::uint64_t peer = 0;
        std::int64_t character = 0;
        std::string name;
        int level = 1;
        std::uint32_t party = 0; // 0: none
        bool partyMaster = false;
        bool inTown = false;
        bool dead = false, riding = false, jobSuit = false, dealing = false, atWar = false, freePvp = false;
    };
    // Its party's members, on every game server of the shard.
    struct Member {
        std::uint32_t character = 0;
        std::string name;
        int level = 0;
        std::uint16_t server = 0;
        std::uint32_t entity = 0; // on its server
    };
    virtual std::vector<Member> InstancePartyMembers(std::uint32_t entity) = 0;
    // The character's entity on this server (0: not here), this server's id, a message to the shard's other game servers.
    virtual std::uint32_t InstanceEntityOf(std::uint32_t character) = 0;
    virtual std::uint16_t InstanceServerId() = 0;
    virtual void InstancePublish(std::string text) = 0;
    virtual std::optional<Player> InstancePlayer(std::uint32_t entity) = 0;
    virtual void InstanceSend(std::uint64_t peer, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // The player taken there at once (a teleport; the layer is the instance's, 0 the shared world).
    virtual void InstanceTeleport(std::uint32_t entity, const game::proto::WorldPosition& to) = 0;
    // The item in a bag place (its RefObjID; 0: none) and one of it taken (false: not there).
    virtual std::uint32_t InstanceItemAt(std::uint32_t entity, std::uint8_t slot) = 0;
    virtual bool InstanceTakeOne(std::uint32_t entity, std::uint8_t slot) = 0;
    virtual void InstanceSaveEntry(std::int64_t character, const InstanceEntry& entry) = 0;
    virtual void InstanceSaveBook(std::int64_t character, const BookEntry& entry) = 0;
    virtual void InstanceSaveWait(std::int64_t character, const ItemWait& wait) = 0;
    // Its collection book changed (the quests' collect goals).
    virtual void InstanceBookChanged(std::uint32_t entity) = 0;
    // A monster's level (characterdata), for the triggers.
    virtual int InstanceMonsterLevel(std::uint32_t refObjId) = 0;
};

class Instances {
public:
    using Clock = std::chrono::steady_clock;
    Instances(InstanceHost& host, World& world, Monsters& monsters, const assets::CharacterTable& characters,
              std::shared_ptr<const GameWorldCatalog> catalog, std::uint32_t seed);
    ~Instances();
    Instances(const Instances&) = delete;
    Instances& operator=(const Instances&) = delete;

    // A player in the world: its book, its instance entries, its holes' wait; it is told its book and the wait left.
    void Enter(std::uint32_t entity, std::int64_t character, std::vector<BookEntry> book, std::vector<InstanceEntry> entries,
               std::vector<ItemWait> waits = {});
    // Gone from the world (out of the game, or to another server): out of its instance.
    void Leave(std::uint32_t entity);
    // A request (false: not one of these, or it does not decode).
    bool Handle(std::uint32_t entity, game::proto::MessageId id, std::span<const std::byte> payload);
    // An item used from the bag: true when it is a hole or a talisman (done or refused here).
    bool UseItem(std::uint32_t entity, std::uint8_t slot, const assets::ItemRow& row);
    // A monster died: `killer` the player whose hit killed it (0: none), `at` where it fell.
    void Killed(std::uint32_t killer, const Monsters::Damaged& killed, const game::proto::WorldPosition& at);
    // A player's teleport into an instance has landed (its WorldEntered sent): the entrance's triggers and its
    // InstanceState.
    void Arrived(std::uint32_t entity);
    // Timers: gaps disappear, instances end, delayed actions run, summons not answered lapse.
    void Tick(Clock::time_point now);
    // A message of the shard's other game servers (a summon of a member who is there, its answer).
    void BusMessage(const std::string& text);

    // Where a player in an instance comes back to (its saved place, an exit's end); none: not in one.
    std::optional<game::proto::WorldPosition> ReturnPlace(std::uint32_t entity) const;
    // The instance's start (a resurrection there); none: not in one.
    std::optional<game::proto::WorldPosition> StartOf(std::uint32_t entity) const;
    bool ThemeComplete(std::uint32_t entity, std::string_view theme) const;
    std::uint32_t WorldOf(std::uint32_t entity) const; // 0: not in an instance

    // Tests and tools.
    std::size_t Count() const;
    std::uint32_t GapOf(std::uint32_t master) const;      // the gap the player opened (0: none)
    std::uint32_t LayerOf(std::uint32_t entity) const;    // the instance's layer it is in (0: none)
    bool DoorOpen(std::uint32_t layer, const std::string& zone) const;
    std::size_t NestOf(std::uint32_t layer, std::uint32_t nestId) const; // the Monsters index (SIZE_MAX: none)
    std::vector<std::uint32_t> ExitsOf(std::uint32_t layer) const;       // its exits' entities
    std::vector<std::uint32_t> PillarsOf(std::uint32_t layer) const;     // its party recall pillars' entities
    bool AtPillar(std::uint32_t entity, std::uint32_t pillar);             // the player stands by its instance's pillar
    // The fortresses' Cave of Discipline: an instance of the catalog's siege world opened (its
    // layer; 0: none, or no such world), its time `seconds` (0: the world's); a player taken into it (false: not here, or it
    // is gone); whether a player re-enters it free (their latest instance entry is this layer of it, still standing,
    // with its key); a player out of it, to `to` (the leave's recall point) or else back where it came from; whether it
    // still stands and who is inside.
    std::uint32_t OpenSiege(const std::string& opener, std::uint32_t seconds = 0);
    bool AdmitSiege(std::uint32_t entity, std::uint32_t layer);
    bool ReadmitsSiege(std::uint32_t entity, std::uint32_t layer) const;
    void ExitSiege(std::uint32_t entity, const game::proto::WorldPosition* to = nullptr);
    bool Stands(std::uint32_t layer) const;
    std::size_t Inside(std::uint32_t layer) const;
    std::uint32_t SiegeMaxUsers() const;
    // A quest's instance world (_RefGame_World's INS_QNO_*, without nests or triggers; its
    // quest raises what is in it): an instance of it opened (its layer; 0: none). A player taken into a layer at `at` (its
    // own place there: the original keeps each member's place); out by ExitSiege. Nobody inside, it ends.
    std::uint32_t OpenQuestWorld(const std::string& codename, std::uint32_t worldId, const std::string& opener);
    bool AdmitAt(std::uint32_t entity, std::uint32_t layer, const game::proto::WorldPosition& at);
    // A world its own code plays (the Roc's Peak): an instance of `spec` (kept) opened, without a gap;
    // it lasts its duration, or its empty time with nobody inside; its layer (0: none). Closed at once by CloseWorld
    // (everyone inside back where it came from). The players in a layer, and an exit put in one (an object whose use
    // takes the user out: LeaveInstance, or EnterGap on a gap's model).
    std::uint32_t OpenWorld(const GameWorldSpec& spec, const std::string& opener);
    // The Pharaoh's tomb: a player through the gate of a tomb's world (the catalog's `tombs`):
    // taken in (Ok) or why not. A free re-entry first (the instance of its latest entry standing); then its checks (not in
    // an instance, level, no job suit, not riding, no free PvP), the day's entries (`entriesPerDay`), a party of two at
    // least; the party's master opens a new instance and his party is told the difficulty; a member goes into his
    // master's (another difficulty's, or none: refused).
    game::proto::GateStatus EnterTomb(std::uint32_t entity, const GameWorldSpec& world);
    const GameWorldSpec* TombOfPoint(std::uint32_t point) const; // the tomb's world a gate point leads into (null: none)
    void CloseWorld(std::uint32_t layer);
    std::vector<std::uint32_t> PlayersIn(std::uint32_t layer) const;
    std::uint32_t AddExit(std::uint32_t layer, std::uint32_t refObjId, const game::proto::WorldPosition& at, float yaw = 0.0f);
    // A player in an instance taken on into another (Seth's temple from Haroeris's), keeping where it came from at first;
    // false when it is in none, or the other is gone.
    bool Transfer(std::uint32_t entity, std::uint32_t layer, const game::proto::WorldPosition& at);

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace sr::server
