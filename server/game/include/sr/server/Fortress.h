#pragma once

// The game server's fortress war: the fortresses in service and their
// structures (the heart, the guard towers, the gates) standing in the world, the application period
// and the war on their schedule, applying and cancelling at a fortress's official, the war itself
// (who may hit a structure, the guard towers' seal on the heart, the heart's capture by the guild that
// dealt it the most damage, the structures handed to that guild), and the owner kept at the war's end.
// The rules and numbers are the original game server's (game/Fortress.h); the state is the original's tables
// (FortressStore.h).
//
// Ours: the original's game server kept its fortresses' state and wrote it through queued queries
// and the shard database's procedures; here the game server keeps them in memory (loaded at the
// start), changes them at once and writes each change behind, one write at a time in order. The
// structures are monsters of the world that never move nor fight (Monsters::AddFixed), so the players'
// attacks reach them on the monsters' path; players fighting players in the war (PvP) come from the
// game server's hostility, which asks Hostile() here.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <array>
#include <vector>

#include "sr/game/Fortress.h"
#include "sr/game/FortressProtocol.h"
#include "sr/game/WorldProtocol.h"
#include "sr/server/FortressStore.h"
#include "sr/server/Payment.h"

namespace sr::assets {
class CharacterTable;
class FileSystem;
} // namespace sr::assets

namespace sr::server {

class Database;

// A fortress as its reference makes it (FortressReferenceRow, with the structures' places from the
// client's navmesh\objectstring.ifo and their characters from characterdata).
struct FortressSpec {
    std::uint32_t id = 0;
    std::string codename, nameKey;  // FORTRESS_JANGAN, SN_FORTRESS_JANGAN
    std::string official;           // its official's codename (RequestNPCName128)
    std::uint32_t fee = 0;          // an attacker's application fee (RequestFee)
    std::uint16_t taxTargets = 0;   // which of its NPCs it taxes (TaxTargets; the manager's window shows them)
    int maxAdmission = 0, maxGuard = 0, maxBarricade = 0; // MaxAdmission, MaxGuard, MaxBarricade
    std::vector<std::uint16_t> regions; // its area
    std::string linkedTeleport;         // where those made to leave go (LinkedTeleportCodeName)
    std::string revivalGate;            // its Gate of Resurrection's teleport (GATE_JA_REVIVAL_GATE)
    std::vector<std::string> insideGates, outsideGates; // `_RefTeleport`'s gates of its world, and of the world outside
    struct Structure {
        std::uint32_t eventId = 0;   // RefEventStructID
        std::uint32_t refObjId = 0;  // the structure built there first (its level 1)
        game::StructureKind kind = game::StructureKind::None;
        std::string nameKey;         // characterdata's name
        std::uint32_t maxHp = 0;     // its MaxHP
        game::proto::WorldPosition position;
        float yaw = 0;
        std::uint32_t navKey = 0;    // its placed object in the navigation (objectstring.ifo's id)
        std::string navName;         // its zone's name in lower case: the event cells' of the fortress's object
    };
    std::vector<Structure> structures;
    // Ours, as the original's game servers: every server holds every fortress, and only
    // the one whose part of the world holds its area runs it (its structures, its war); the others keep a mirror.
    bool local = true;
};

// A player as the fortress war sees it (the game server fills it in).
struct FortressActor {
    std::uint64_t peer = 0;
    std::uint32_t entity = 0;
    std::int64_t character = 0;
    int level = 0;
    std::string name;
    std::uint64_t gold = 0;
};
// A structure's size from its model (ours; the database's radius for structures is 0).
struct StructureShape {
    float body = 0.0f;                            // added to the reach of those who attack it
    std::uint16_t pickRadius = 0, pickHeight = 0; // the cylinder a click takes it in
    float minX = 0, maxX = 0, minZ = 0, maxZ = 0; // its model's footprint, model space (0: unknown)
    // Its navigation object's outline edges (the .bms object navmesh's perimeter), model space: x, y, z of each end.
    std::vector<std::array<float, 6>> outline;
};
// The point of a navigation object's outline (model space, StructureShape::outline) an attacker at (px, pz) reaches it
// at: the foot on the nearest edge whose span holds the point in (x, z), else the nearest end; x, y, z.
std::array<double, 3> OutlineContact(const std::vector<std::array<float, 6>>& outline, double px, double pz);
// A guild as the fortress war needs it (from the guilds): for a member, whether it is the master.
struct FortressGuild {
    std::uint32_t id = 0;
    std::string name;
    int level = 0;
    std::size_t members = 0;
    std::uint32_t alliance = 0;
    bool master = false;
    std::string masterName;
    std::uint32_t crest = 0; // the guild's emblem's revision (0: none)
    std::uint32_t points = 0; // its guild points
};

// What the fortress war needs of the game server.
class FortressHost {
public:
    virtual ~FortressHost() = default;
    virtual void FortressSend(std::uint64_t peer, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // A counted deed of a character (ours, sr_char_records: the beta quests): a fortress war fought (`fortress-war`).
    virtual void FortressDeed(std::int64_t /*character*/, std::string_view /*name*/) {}
    // To every player in the world.
    virtual void FortressBroadcast(game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    virtual std::optional<FortressGuild> FortressGuildOf(std::int64_t character) const = 0;
    virtual std::optional<FortressGuild> FortressGuildById(std::uint32_t guild) const = 0;
    // An NPC's codename (empty: not an NPC) and how far it stands from a player (negative: not in the world).
    virtual std::string FortressNpc(std::uint32_t entity) const = 0;
    virtual double FortressDistance(std::uint32_t a, std::uint32_t b) const = 0;
    virtual bool FortressTakeGold(std::uint64_t peer, std::uint64_t amount) = 0;
    // A payment of `character`'s (Payment.h) with `points` of guild `guild`'s points: the points, the fee, the gold and
    // `work` (the fortress's rows) in one transaction, as the original's procedure for each paid operation.
    virtual void FortressPay(std::int64_t character, std::uint64_t fee, std::uint32_t guild, std::uint32_t points, PaymentWork work, PaymentDone done) {
        (void)character;
        (void)fee;
        (void)guild;
        (void)points;
        (void)work;
        if (done) done(false, 0);
    }
    // A structure standing in the world (a fixed monster of that characterdata id with that health;
    // 0: it could not be made), and gone.
    virtual std::uint32_t SpawnStructure(const FortressSpec::Structure& spot, std::uint32_t refObjId, std::uint32_t hp, std::uint32_t maxHp, bool destroyed) = 0;
    virtual void RemoveStructure(std::uint32_t entity) = 0;
    // What those who see the entity, and newcomers after its appearance, are told of it.
    virtual void StructureAppearance(std::uint32_t entity, game::proto::MessageId id, std::vector<std::byte> payload) = 0;
    // `task` on a database thread, then `done` on the server's.
    virtual void FortressSubmit(std::function<void(Database&)> task, std::function<void()> done) = 0;
    // A development server's teleport of a player (FortressDevAction::GoTo).
    virtual void FortressTeleport(std::uint32_t entity, const game::proto::WorldPosition& to) {
        (void)entity;
        (void)to;
    }
    // Its fortress position (`_GuildMember.SiegeAuthority`; 0: none), and guild points taken from a guild (false: too few).
    virtual std::uint8_t FortressPositionOf(std::int64_t character) const {
        (void)character;
        return 0;
    }
    virtual bool FortressTakeGuildPoints(std::uint32_t guild, std::uint32_t points) {
        (void)guild;
        return points == 0;
    }
    // Is there a teleport point of that codename, with an arrival?
    virtual bool FortressHasPoint(const std::string& teleportCode) const {
        (void)teleportCode;
        return false;
    }
    // A player to the teleport point of that codename; false when it has no such point.
    virtual bool FortressSendTo(std::uint64_t peer, const std::string& teleportCode) {
        (void)peer;
        (void)teleportCode;
        return false;
    }
    // An event zone standing in the way of walks, or not (World::SetNavZone).
    virtual void FortressZoneBlocks(std::uint32_t key, const std::string& name, bool blocks) {
        (void)key;
        (void)name;
        (void)blocks;
    }
    // Every player in `regions` for whom `stays` is false goes to the teleport point of that codename.
    virtual void FortressEvacuate(const std::vector<std::uint16_t>& regions, const std::string& teleportCode, const std::function<bool(std::int64_t)>& stays) {
        (void)regions;
        (void)teleportCode;
        (void)stays;
    }
    // A kind of structure's size.
    virtual StructureShape StructureShapeOf(std::uint32_t refObjId) {
        (void)refObjId;
        return {};
    }
    // A battle rank's buff: `oldSkill`'s taken off, `newSkill`'s put on (0: none); the player's name and the buff's
    // name key back (empty when the player is not here).
    virtual std::pair<std::string, std::string> FortressRankBuff(std::int64_t character, std::uint32_t oldSkill, std::uint32_t newSkill) {
        (void)character;
        (void)oldSkill;
        (void)newSkill;
        return {};
    }
    // A message to one player by character (none: not here), and to a guild's members and its union's.
    virtual void FortressSendCharacter(std::int64_t character, game::proto::MessageId id, const std::vector<std::byte>& payload) {
        (void)character;
        (void)id;
        (void)payload;
    }
    virtual void FortressSendGuild(std::uint32_t guild, game::proto::MessageId id, const std::vector<std::byte>& payload) {
        (void)guild;
        (void)id;
        (void)payload;
    }
    // An NPC whose codename holds `part`, standing in one of `regions` (a developer's way to the battle aide).
    virtual std::uint32_t FortressNpcIn(const std::string& part, const std::vector<std::uint16_t>& regions) const {
        (void)part;
        (void)regions;
        return 0;
    }
    // Where an entity stands (none: not in the world).
    virtual std::optional<game::proto::WorldPosition> FortressEntityPosition(std::uint32_t entity) const {
        (void)entity;
        return std::nullopt;
    }
    // The tax: gold levied to the master; a fortress's new rate (the host's taxes follow).
    virtual void FortressGiveGold(std::uint64_t peer, std::uint64_t amount) {
        (void)peer;
        (void)amount;
    }
    // A fortress taken from `guild`: its members' fortress positions (but the Commander's) are gone (the database's reset).
    virtual void FortressLost(std::uint32_t guild) { (void)guild; }
    virtual void FortressTaxRateChanged(std::uint32_t fortress, std::uint32_t guild, int rate) {
        (void)fortress;
        (void)guild;
        (void)rate;
    }
    // The shard's bus (channel sr_fortress_<shard>): this server's id, whether the bus is
    // there, and a message (our encoding in hexadecimal) to every server of the shard.
    // The item at a player's bag place (its characterdata id; none: nothing there), one of it used up; a character's id
    // and most health by its codename (none: not in characterdata).
    virtual std::optional<std::uint32_t> FortressItemAt(std::uint64_t peer, std::uint8_t slot) const {
        (void)peer;
        (void)slot;
        return std::nullopt;
    }
    virtual void FortressTakeItem(std::uint64_t peer, std::uint8_t slot) {
        (void)peer;
        (void)slot;
    }
    virtual std::optional<std::pair<std::uint32_t, std::uint32_t>> FortressCharacter(const std::string& codename) const {
        (void)codename;
        return std::nullopt;
    }
    // A characterdata row as the war's objects need it (none: not there): its kind (a structure's or an object's, by its
    // TIDs), most health, level and codename.
    struct CharacterInfo {
        game::StructureKind kind = game::StructureKind::None;
        std::uint32_t maxHp = 0;
        int level = 0;
        std::string codename;
    };
    virtual std::optional<CharacterInfo> FortressCharacterRow(std::uint32_t refObjId) const {
        (void)refObjId;
        return std::nullopt;
    }

    // --- The war's objects and the items on a target ---
    // A guard or a tablet's monster standing for a side (Monsters::AddWarMonster; 0: it could not), and a summon scroll's
    // unique at a place (a wild one: its nest's; 0: it could not).
    virtual std::uint32_t FortressSpawnWarMonster(std::uint32_t refObjId, const game::proto::WorldPosition& at, float yaw, std::uint32_t guild,
                                                  std::uint32_t alliance, std::uint32_t hp) {
        (void)refObjId;
        (void)at;
        (void)yaw;
        (void)guild;
        (void)alliance;
        (void)hp;
        return 0;
    }
    virtual std::uint32_t FortressSpawnUnique(std::uint32_t refObjId, const game::proto::WorldPosition& at) {
        (void)refObjId;
        (void)at;
        return 0;
    }
    // A structure's health raised by `amount` (a repair kit's pulse; its new health, 0: not standing), told to those around.

    virtual std::uint32_t FortressHealStructure(std::uint32_t entity, std::uint32_t amount) {
        (void)entity;
        (void)amount;
        return 0;
    }
    // A flag's aura (its characterdata's DefaultSkill_1) over the players of `guild` and its union within the skill's
    // reach of the flag: a buff kept on while they stand there, or (a 'heal' with a 'puls') a heal when `pulse` is due.
    virtual void FortressFlagAura(std::uint32_t flag, std::uint32_t refObjId, std::uint32_t guild, bool pulse) {
        (void)flag;
        (void)refObjId;
        (void)guild;
        (void)pulse;
    }
    // The pulse period of a flag's aura in ms (its skill's 'puls'; 0: a buff, no pulse).
    virtual int FortressFlagPulseMs(std::uint32_t refObjId) const {
        (void)refObjId;
        return 0;
    }
    // A repair kit's skill as its rows give it (SKILL_FORT_REPAIR_KIT_0n: 'heal' percent of the most health, 'puls', 'dura';
    // none: no such skill).
    struct KitSkill {
        int percent = 0, pulseMs = 0, durationMs = 0;
    };
    virtual std::optional<KitSkill> FortressKitSkill(const std::string& skill) const {
        (void)skill;
        return std::nullopt;
    }
    // A skill an item gives (its Param1) cast by the player on a target as a learned one would be (the shock bomb).
    virtual bool FortressCastItemSkill(std::uint64_t peer, const std::string& skill, std::uint32_t target) {
        (void)peer;
        (void)skill;
        (void)target;
        return false;
    }
    // --- The manager's other rows ---
    // A blessing's skill on the player: whether it is known and may stand beside what runs (the original's buff replacement
    // check, 0x300C), and its reuse (Action_ReuseDelay); then given (its 'dura').
    struct BlessCheck {
        bool known = false, admitted = false;
        int reuseMs = 0;
    };
    virtual BlessCheck FortressBlessCheck(std::uint64_t peer, std::uint32_t skill) const {
        (void)peer;
        (void)skill;
        return {};
    }
    virtual void FortressBlessGive(std::uint64_t peer, std::uint32_t skill) {
        (void)peer;
        (void)skill;
    }
    // A line of the guild's points spent (`_GPHistory`, the reasons game::kGpReason*), and the guild's history to a player
    // (the guild window's GuildGpHistory).
    virtual void FortressGpSpent(std::uint32_t guild, const std::string& name, std::uint32_t points, int reason) {
        (void)guild;
        (void)name;
        (void)points;
        (void)reason;
    }
    virtual void FortressGpHistoryTo(std::uint64_t peer, std::uint32_t guild) {
        (void)peer;
        (void)guild;
    }
    // --- Siege quests and the war's rewards ---
    // The living party members of a character in its world within `reach` of it (characters; itself not among them).
    virtual std::vector<std::int64_t> FortressPartyNear(std::int64_t character, double reach) const {
        (void)character;
        (void)reach;
        return {};
    }
    // A siege quest's SP experience: alone, the doer's level's `GUST_Mob_Exp` x `value`; in a party, the party's
    // level's x `value` / its size to each living member in the doer's world within 1,000. The characters given.
    virtual std::vector<std::int64_t> FortressQuestSp(std::int64_t character, std::uint32_t value) {
        (void)character;
        (void)value;
        return {};
    }
    // An item to a character here (false: not here, or no room).
    virtual bool FortressGiveItemTo(std::int64_t character, std::uint32_t item, std::uint32_t count) {
        (void)character;
        (void)item;
        (void)count;
        return false;
    }
    // The war's end: every player of the guild in these regions gets `value` x his level's `GUST_Mob_Exp` as
    // SP experience; how many.
    virtual int FortressAreaSp(const std::vector<std::uint16_t>& regions, std::uint32_t guild, std::uint32_t value) {
        (void)regions;
        (void)guild;
        (void)value;
        return 0;
    }
    // A guild's master's character (0: none).
    virtual std::int64_t FortressMasterOf(std::uint32_t guild) const {
        (void)guild;
        return 0;
    }
    // --- The Cave of Discipline ---
    // An instance of the siege world opened (its layer; 0: it could not, 0x2845), a player taken in (false: refused), a player
    // out, whether it still stands, how many are inside, the world's WorldMaxUserCount, the instance a player is in (0: none).
    virtual std::uint32_t FortressDungeonOpen(const std::string& opener) {
        (void)opener;
        return 0;
    }
    virtual bool FortressDungeonAdmit(std::uint64_t peer, std::uint32_t layer) {
        (void)peer;
        (void)layer;
        return false;
    }
    // Out of the Cave by its leave (0x20): to the character's recall point (`_Char.TelRegion`), which
    // entering set to where it entered from; false: no recall point (the answer's code 5).
    virtual bool FortressDungeonExit(std::uint64_t peer) {
        (void)peer;
        return false;
    }
    // Whether the player enters that Cave again free (the world's admission check).
    virtual bool FortressDungeonReadmits(std::uint64_t peer, std::uint32_t layer) const {
        (void)peer;
        (void)layer;
        return false;
    }
    virtual bool FortressDungeonStands(std::uint32_t layer) const {
        (void)layer;
        return false;
    }
    virtual std::size_t FortressDungeonInside(std::uint32_t layer) const {
        (void)layer;
        return 0;
    }
    virtual std::uint32_t FortressDungeonMaxUsers() const { return 30; }
    virtual std::uint32_t FortressDungeonOf(std::uint64_t peer) const {
        (void)peer;
        return 0;
    }
    // --- The smith and the trainer ---
    // Whether a member of the guild holds a fortress position (Production 8, Training 0x10: the forge's 0.85).
    virtual bool FortressGuildHasPosition(std::uint32_t guild, std::uint8_t position) const {
        (void)guild;
        (void)position;
        return false;
    }
    // Items into a player's bag (false: they do not fit, nothing given).
    virtual bool FortressGiveItem(std::uint64_t peer, std::uint32_t item, std::uint32_t count) {
        (void)peer;
        (void)item;
        (void)count;
        return false;
    }
    // A straight walk from `from` to `to` hits no wall (the region manager's move test;'s 0x18D9).
    virtual bool FortressWalkClear(const game::proto::WorldPosition& from, const game::proto::WorldPosition& to) const {
        (void)from;
        (void)to;
        return true;
    }
    virtual std::uint16_t FortressServerId() const { return 0; }
    virtual bool FortressOnBus() const { return false; }
    virtual void FortressPublish(std::string text) { (void)text; }
};

// The schedule's settings.
struct FortressConfig {
    // The battle ranks (LoadBattleRanks): the kills each asks and its buff.
    std::vector<BattleRankRow> battleRanks;
    // The structures' reference (LoadStructureReferences): their upgrades, costs and health.
    std::vector<StructureReference> structureReferences;
    // `_Schedule`'s rows of the war's two schedules (game::kScheduleSiegeRequest, kScheduleSiegeWar).
    std::vector<game::ScheduleRow> schedule;
    // Ours, for tests and tries: a cycle from the server's start, replacing the schedule when its war
    // lasts: `requestSeconds` of applications, `gapSeconds` after, then `warSeconds` of war, again.
    int requestSeconds = 0, gapSeconds = 0, warSeconds = 0;
    // The warnings before the war's end (the original's kinds 3, 4, 5, 9); empty: none, not even the war period's first.
    std::vector<int> warningMinutes = {30, 20, 10, 1};
    // Ours, for tests and tries: every structure's most health (0: its reference's MaxHP).
    std::uint32_t structureHp = 0;
    // The heart's seal after the last guard tower fell (game::kHeartSealSeconds).
    int heartSealSeconds = game::kHeartSealSeconds;
    // Ours: the server that keeps the periods (applications, war, tax) and tells the others over the bus, as the
    // original's shard manager relays them: the one that runs the fortress of the
    // lowest id. The others follow its word.
    bool periodKeeper = true;
    // The fortress items (LoadFortressItems).
    std::vector<FortressItemRow> items;
    // How long a flag stands (game::kFlagSeconds; tests shorten it).
    int flagSeconds = game::kFlagSeconds;
    // The guards' and tablet monsters' variants by fortress, and the summon scrolls' uniques (LoadFortressGuards,
    // LoadSiegeSummons).
    std::vector<SiegeVariantRow> guards, summons;
    // The blessings (LoadBlessBuffs).
    std::vector<BlessRow> blesses;
    // The smith's and the trainer's list (LoadForgeReference).
    std::vector<ForgeReferenceRow> forges;
    // The siege quests (`_RefSiegeQuest`, `_RefSiegeQuestReward`) and the war's rewards (`_RefSiegeFortressRewards`).
    std::vector<SiegeQuestRow> quests;
    std::vector<SiegeRewardRow> rewards;
    // The Cave of Discipline (`_RefSiegeDungeon`): a fortress's world, creations a day, the entry's gold and GP; its time
    // (seconds; 0 the world's WorldDurationTime; tests shorten it).
    std::vector<SiegeDungeonRow> dungeons;
    int dungeonSeconds = 0;
    // Seconds a forge's minute lasts (60; tests shorten it).
    double forgeSecondsPerMinute = 60.0;

};

// The fortresses in service from the database's reference (LoadFortressReference) with their
// structures' places from the client's navmesh\objectstring.ifo and their characters from characterdata
// (a structure whose place or character is not found is left out, logged); none without the reference.
bool LoadFortressSpecs(Database& db, const assets::FileSystem& files, const assets::CharacterTable& characters, std::vector<FortressSpec>& out,
                       std::string& error);
// `_Schedule`'s rows of the war's two schedules (none without the table).
bool LoadFortressSchedule(Database& db, std::vector<game::ScheduleRow>& out, std::string& error);
// `_Schedule`'s rows of the schedules `defines` (`_RefScheduleDefine`; none without the table).
bool LoadSchedules(Database& db, const std::vector<int>& defines, std::vector<game::ScheduleRow>& out, std::string& error);

class Fortresses {
public:
    using Clock = std::chrono::steady_clock;
    Fortresses(FortressHost& host, std::vector<FortressSpec> specs, FortressConfig config);
    ~Fortresses();
    Fortresses(const Fortresses&) = delete;
    Fortresses& operator=(const Fortresses&) = delete;

    // The state the database keeps; the rows a fortress lacks are made (as `_InsertSiegeFortress`
    // does) and written. Then the structures stand in the world.
    void Load(FortressState state);
    // A player entered the world: it is told the fortresses and the periods.
    void Entered(const FortressActor& actor);
    // A fortress request (FortressProtocol.h ids); false when it does not decode. `devCommands`: the
    // server takes FortressDev.
    bool Handle(const FortressActor& actor, game::proto::MessageId id, std::span<const std::byte> payload, bool devCommands);
    // The periods by the schedule (or the cycle), the war's timers. `unixNow`: the wall clock.
    void Tick(Clock::time_point now, std::int64_t unixNow);
    // A development server's switch of the periods (the GM's /SIEGE commands): from then on the
    // periods follow these switches only (on a server that keeps no periods: passed to the one that does).
    void Dev(game::proto::FortressDevAction action);
    // The shard's bus: this server has started (the others tell it what they keep), and a message from another server.
    void Hello();
    void BusMessage(const std::string& text);
    // The tax period now (the keeper's word on the other servers).
    bool TaxPeriod() const;

    // --- The structures, as the game server's attacks see them ---
    bool IsStructure(std::uint32_t entity) const;
    // A war's object of the fortresses (a flag, a guard, a tablet's monster): its own side may not hit it.
    bool IsWarObject(std::uint32_t entity) const;
    // The point of a structure's navigation object an attacker at (x, z) reaches it at: in the object's frame, the foot
    // on the nearest outline edge whose span holds the point, else the nearest outline vertex; its height the edge's
    // there. Without an outline: the footprint's nearest point at the structure's height. None: not a structure, or its size unknown.
    std::optional<game::proto::WorldPosition> NearestPoint(std::uint32_t entity, double x, double z) const;
    // Why a player (its character) may not hit this structure (None: it may).
    game::FortressError MayHit(std::int64_t attacker, std::uint32_t structure) const;
    // A hit that took `damage` from it; and its destruction by `killer` (a character).
    void StructureHit(std::uint32_t structure, std::int64_t attacker, std::uint64_t damage, std::uint32_t hpLeft);
    void StructureDestroyed(std::uint32_t structure, std::int64_t killer);

    // --- Players in the war ---
    // Are two players (characters, and the regions they stand in) enemies: both in the area of a
    // fortress at war, on the war's two sides (game::PlayersHostileInWar). The game server's hostility
    // asks this.
    bool Hostile(std::int64_t a, std::uint16_t regionA, std::int64_t b, std::uint16_t regionB) const;
    // A player who may be fought there: in the area of a fortress at war, of one of its war's guilds.
    bool Fighting(std::int64_t character, std::uint16_t region) const;
    // Is that region in the area of a fortress at war? In a fortress's area at all (its world, INS_FORT_*)?
    bool WarArea(std::uint16_t region) const;
    bool FortressArea(std::uint16_t region) const;
    // Where one who died there rises when it chooses its point: the
    // holder's guild at the Gate of Resurrection, its union at a gate inside, the others at a gate outside (their
    // war camp when it is built). Empty: the region is not at war.
    std::string RevivalGate(std::int64_t character, std::uint16_t region);
    // An attacker with a war camp rises beside it ("their command
    // post at a random angle within 250"); none: no camp, or not at war there.
    std::optional<game::proto::WorldPosition> RevivalPlace(std::int64_t character, std::uint16_t region);
    // May a player go into this region (a gate's arrival): the area of a fortress at war only for its
    // war's guilds ("The fortress-owning guild member or the guild
    // members applied for the fortress war are only allowed to enter the fortress during the fortress war.").
    bool MayEnter(std::int64_t character, std::uint16_t region) const;
    // One who may not be in the area of a fortress at war (a login there) goes to its linked gate.
    void SendOut(const FortressActor& actor, std::uint16_t region);
    // A player killed by another in a war (the game server's kill hook): its battle record (ours: in
    // memory, the record's table comes with the battle ranks).
    void PlayerKilled(std::int64_t killer, std::int64_t victim);
    // A guild owns a fortress (a guild holding one may not disband; 0x2811 for applying).
    bool GuildHoldsFortress(std::uint32_t guild) const;
    // A guild owns or holds a fortress, or applied to the war over one.
    bool GuildInWar(std::uint32_t guild) const;

    // --- For tests and tools ---
    bool RequestsOpen() const;
    bool WarOn() const;
    std::uint32_t Owner(std::uint32_t fortress) const;
    std::uint32_t Holder(std::uint32_t fortress) const;
    // The entities of a fortress's structures of a kind (in its reference's order; 0 for one not standing).
    std::vector<std::uint32_t> Structures(std::uint32_t fortress, game::StructureKind kind) const;
    // The entities of a fortress's war objects of a kind standing (flags, guards, summoned monsters).
    std::vector<std::uint32_t> Objects(std::uint32_t fortress, game::StructureKind kind) const;

    std::size_t Count() const;
    bool Writing() const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace sr::server
