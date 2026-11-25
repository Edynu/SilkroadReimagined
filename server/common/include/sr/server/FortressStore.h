#pragma once

// The fortress war's tables: the original's
// `_SiegeFortress` (a fortress's owner, the guild holding it in a war, its tax), `_SiegeFortressRequest`
// (the guilds applied for its war), `_SiegeFortressStruct` (its structures' state) and
// `_SiegeFortressStoneState` (the damage each guild dealt its heart), written as the original's game
// server and procedures write them, with our own queries;
// and the reference that makes the fortresses (`_RefSiegeFortress`, `_RefGame_World[_Config]`,
// `_RefEventZone`, `_RefRegion`).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sr::server {

class Database;

struct FortressRow {
    std::uint32_t id = 0;
    std::uint32_t guild = 0;     // GuildID: the owner (0: none)
    std::uint32_t tempGuild = 0; // TempGuildID: the guild holding it for now in a war
    int taxRatio = 0;
    std::int64_t tax = 0;
    std::uint8_t npcHired = 0;    // NPCHired: bits 1 the battle aide, 2 the smith, 4 the trainer (the manager's hire rows)
    std::string introduction;     // Introduction: the holder's words (the list's third string, the manager's 0x19)
    std::int64_t dungeonTime = 0; // CreatedDungeonTime (local Unix seconds; 0: none) and CreatedDungeonCount
    int dungeonCount = 0;
    bool introPermission = true;  // IntroductionModificationPermission (0: the introduction may not be changed, 0x284C)
};
struct FortressRequestRow {
    std::uint32_t fortress = 0;
    std::uint32_t guild = 0;
    std::uint8_t type = 0; // 0 attack, 1 defend
};
struct FortressStructRow {
    std::uint32_t fortress = 0;
    std::uint32_t owner = 0;
    std::uint32_t eventId = 0; // RefEventStructID: its _RefEventZone row
    std::uint32_t refObjId = 0;
    std::int64_t hp = 0;
    int state = 0;
    std::int64_t madeAt = 0; // MakeDate (Unix seconds): when it was built or last upgraded
};
struct FortressStoneRow {
    std::uint32_t fortress = 0;
    std::uint32_t guild = 0;
    std::int64_t damage = 0;
};
// A fortress war's object (`_SiegeFortressObject`, `_AddSiegeFortressObject`): a flag, a guard or a summoned monster
// standing for its guild at a point (no zone), with its placer's level (`OwnerLevel`).
struct FortressObjectRow {
    std::int64_t id = 0; // ID (the identity)
    std::uint32_t fortress = 0, owner = 0, refObjId = 0;
    std::int64_t hp = 0;
    std::uint16_t region = 0;
    float x = 0, y = 0, z = 0, direction = 0;
    int ownerLevel = 0;
};
// A character's war record (`_SiegeFortressBattleRecord`), read back at the start.
struct FortressBattleRecordRow {
    std::uint32_t fortress = 0;
    std::int64_t character = 0;
    int kills = 0, deaths = 0, rank = 0;
};
// A forge's work (`_SiegeFortressItemForge`: one per fortress and side; the side by its item, the trainer's TID 3,3,3,2).
struct FortressForgeRow {
    std::uint32_t fortress = 0, item = 0;
    int amount = 0;
    bool finished = false;
    std::int64_t start = 0, finish = 0; // Unix seconds
};
struct FortressState {
    std::vector<FortressRow> fortresses;
    std::vector<FortressRequestRow> requests;
    std::vector<FortressStructRow> structures;
    std::vector<FortressStoneRow> stones;
    std::vector<FortressObjectRow> objects;
    std::vector<FortressBattleRecordRow> records;
    std::vector<FortressForgeRow> forges;
};
// A forge's work begun, finished, its amount lowered (gone at 0) or cancelled (`_SiegeFortressItemForge`).
bool InsertFortressForge(Database& db, const FortressForgeRow& row);
bool SaveFortressForge(Database& db, const FortressForgeRow& row);
bool DeleteFortressForge(Database& db, std::uint32_t fortress, std::uint32_t item);
// The smith's and the trainer's list (`_RefSiegeFortressItemForge`): an item a fortress makes, its price and minutes each, and
// whether it is the trainer's (TID 3,3,3,2).
struct ForgeReferenceRow {
    std::uint32_t fortress = 0, item = 0;
    std::uint64_t gold = 0;
    std::uint32_t guildPoints = 0;
    int minutes = 0;
    bool trainer = false;
};
bool LoadForgeReference(Database& db, std::vector<ForgeReferenceRow>& out, std::string& error);
// A siege quest (`_RefSiegeQuest`: its target type 1 tower, 2 gate, 3 heart, 4 flag, 5 war camp, 6 guard, 7 player; the
// count; whether the count accumulates) with its rewards (`_RefSiegeQuestReward`: type 1 an item x value, 2 SP experience
// x `GUST_Mob_Exp`); the war's rewards (`_RefSiegeFortressRewards`: type 1 an item to the master, 5 SP experience x value).
struct SiegeQuestRow {
    int id = 0, type = 0, count = 0;
    bool accumulates = false;
    struct Reward {
        int type = 0;
        std::uint32_t ref = 0;
        std::uint32_t value = 0;
    };
    std::vector<Reward> rewards;
};
struct SiegeRewardRow {
    std::uint32_t fortress = 0;
    int type = 0;
    std::uint32_t value = 0, count = 0;
};
bool LoadSiegeQuests(Database& db, std::vector<SiegeQuestRow>& quests, std::vector<SiegeRewardRow>& rewards, std::string& error);
// The Cave of Discipline's rows (`_RefSiegeDungeon`).
struct SiegeDungeonRow {
    std::uint32_t fortress = 0, world = 0;
    int maxCreate = 0;
    std::uint64_t gold = 0;
    std::uint32_t guildPoints = 0;
};
bool LoadSiegeDungeons(Database& db, std::vector<SiegeDungeonRow>& out, std::string& error);
bool LoadFortressState(Database& db, FortressState& out, std::string& error);
// The manager's writes: the hired NPCs (`_SiegeFortressHireNPC`'s update), the introduction, the Cave of Discipline's count and
// its last creation's time (local Unix seconds).
bool SaveFortressHired(Database& db, std::uint32_t fortress, std::uint8_t hired);
bool SaveFortressIntroduction(Database& db, std::uint32_t fortress, const std::string& text);
bool SaveFortressDungeon(Database& db, std::uint32_t fortress, int count, std::int64_t createdAt);
// The blessings (`_RefSiegeBlessBuff`): a fortress's buff rows, their skill and price.
struct BlessRow {
    std::uint32_t id = 0, fortress = 0, skill = 0;
    std::uint64_t gold = 0;
    std::uint32_t guildPoints = 0;
};
bool LoadBlessBuffs(Database& db, std::vector<BlessRow>& out, std::string& error);
// An object added (its new ID; 0: not written) and one gone.

std::int64_t AddFortressObject(Database& db, const FortressObjectRow& row);
bool DeleteFortressObject(Database& db, std::int64_t id);

// A fortress's rows where none are, as the original's `_InsertSiegeFortress` makes them: its
// `_SiegeFortress` row (every value the default) and a `_SiegeFortressStruct` row per structure
// (owner 0, its full HP, state 0) whose RefEventStructID has none.
bool InsertFortress(Database& db, std::uint32_t fortress, const std::vector<FortressStructRow>& structures, std::string& error);
// An application (`_SiegeFortressRequestSiege` inserts it; the fee is the game server's gold, saved
// with the character), and one taken back.
bool InsertFortressRequest(Database& db, const FortressRequestRow& request);
bool DeleteFortressRequest(Database& db, std::uint32_t fortress, std::uint32_t guild);
// A structure's state (`UPDATE _SiegeFortressStruct SET OwnerGuildID, RefObjID,
// HP, State, MakeDate WHERE RefEventStructID`).
bool SaveFortressStruct(Database& db, const FortressStructRow& row);
// A structure kind's reference (`_RefObjCommon`, `_RefObjChar`, `_RefSiegeStructUpgrade`): what its upgrade
// costs (Except_1..3: gold, guild points, minutes since it was made), the level the next one asks, its health,
// repair and revival, and what it becomes (`UpgradeStructName1..4`; empty: none).
struct StructureReference {
    std::uint32_t refObjId = 0;
    std::string codename;
    int reqLevel = 0;
    std::uint32_t gold = 0, guildPoints = 0, minutes = 0;
    std::uint32_t maxHp = 0, costRepair = 0, costRevive = 0;
    bool canRevive = false;
    std::array<std::string, 4> upgrades;
    // `_RefSiegeStructUpgrade`: the level a heart broken takes it down to (the row whose first upgrade it is, the least
    // name), else its line's first level (BaseStructCodeName); empty: none.
    std::string below, base;
};
bool LoadStructureReferences(Database& db, std::vector<StructureReference>& out, std::string& error);
// The battle ranks (`_RefSiegeFortressBattleRank`): the kills each asks (ReqPKCount) and its buff (BindedSkillID).
struct BattleRankRow {
    int rank = 0;
    std::uint32_t kills = 0;
    std::uint32_t skill = 0;
};
bool LoadBattleRanks(Database& db, std::vector<BattleRankRow>& out, std::string& error);
// A character's war record (`_SiegeFortressBattleRecord`, a row per fortress and character; gone at the war's end).
bool SaveBattleRecord(Database& db, std::uint32_t fortress, std::int64_t character, int kills, int killed, int rank);
// The damage a guild dealt a fortress's heart (a row per guild, replaced).
bool SaveFortressStone(Database& db, const FortressStoneRow& row);
// The heart broken: the guild that holds the fortress for now (`_SiegeFortress.TempGuildID`), the
// heart's damage forgotten (`_SiegeFortressTempFinished`).
bool SetFortressHolder(Database& db, std::uint32_t fortress, std::uint32_t holder);
// The war over (`_SiegeFortressFinished`): its battle records, applications and the heart's damage
// gone; a holder other than the owner becomes the owner (NPCHired 0) and the old owner's members'
// fortress positions of 4 and more are cleared (`_Guild_ResetSiegeAuthority`); TempGuildID 0.
bool FinishFortress(Database& db, std::uint32_t fortress, std::uint32_t owner, std::uint32_t holder, std::string& error);
// A fortress's tax rate (`_UpdateSiegeFortressTaxRatio`).
bool SaveFortressTaxRatio(Database& db, std::uint32_t fortress, int ratio);
// The tax gathered added to `_SiegeFortress.Tax` (any game server's, as one increment).
bool AddFortressTax(Database& db, std::uint32_t fortress, std::int64_t delta);
// The objects a fortress taxes (`_RefObjCommon.AssocFile1_128` names it): object id ->
// fortress id, only for fortresses in service (the client's siegefortress.txt holds only those, so a row naming another
// fortress has none).
bool LoadTaxedObjects(Database& db, std::vector<std::pair<std::uint32_t, std::uint32_t>>& out, std::string& error);

// --- The reference ---------------------------------------------------------------------------------

struct FortressReferenceRow {
    std::uint32_t id = 0;
    std::string codename, nameKey;  // CodeName128, NameID128
    std::string official;           // RequestNPCName128
    std::uint32_t fee = 0;          // RequestFee
    std::uint16_t taxTargets = 0;   // TaxTargets: which of its NPCs it taxes (the manager's window's boxes)
    int maxAdmission = 0, maxGuard = 0, maxBarricade = 0; // MaxAdmission, MaxGuard, MaxBarricade
    std::string world;              // its _RefGame_World (INS_FORT_JA)
    std::string linkedTeleport;     // LinkedTeleportCodeName: where those made to leave go (GATE_CH)
    std::vector<std::uint16_t> regions; // its area's (_RefRegion of its REF_REGION_BIND_ASSOC_SERVER)
    struct Structure {
        std::uint32_t eventId = 0;  // _RefEventZone.ID
        std::string zone;           // ZoneName: the spot's name in navmesh\objectstring.ifo
        std::string event;          // EventName (FORT_STONE, GUARD_TOWER, GATE, ...)
        std::string codename;       // strParam1: the structure built there
    };
    std::vector<Structure> structures;
};
// The fortresses in service (`_RefSiegeFortress.Service` 1) with their worlds, areas and structures;
// none when the tables are not there (a database without the original's import).
// The instance worlds' regions (_RefInstance_World_Region with _RefGame_World's code names):
// (region, world code name); nothing when the original's tables were not imported.
bool LoadWorldRegions(Database& db, std::vector<std::pair<std::uint16_t, std::string>>& out, std::string& error);
bool LoadFortressReference(Database& db, std::vector<FortressReferenceRow>& out, std::string& error);

// A fortress war's item: its row of `_RefObjItem` as the war's uses read it: TID 3,3,3,6..11
// (tablets, guards, manuals, flags, scrolls), the repair kits 3,3,1,10, the shock bomb 3,3,6,2.
struct FortressItemRow {
    std::uint32_t refObjId = 0;
    std::string codename;
    std::array<int, 4> typeId{};
    std::string target; // Desc1_128: the structure, monster or skill it makes
    int place = 0;      // Param2: 1 in a fortress's world, 2 outside (0: any)
    int time = 0;       // Param3: 1 during the war, 2 outside it (0: any)
    int reuseMs = 0;    // Param4: its COOLTIME's wait
    int mask = 0;       // Param5: the fortress positions that may use it (0: anyone)
    int range = 0;      // Range
    // Desc4's "COOLTIME:0x.." group: every item of it waits with the one used (0xF0 the tablets, guards and scrolls, 0xF1
    // the flags, 0xF2 the repair kits); 0 its own wait (the manuals, the shock bomb). game::CoolTimeKey.
    std::uint32_t coolGroup = 0;
};
bool LoadFortressItems(Database& db, std::vector<FortressItemRow>& out, std::string& error);
// A guard's or a tablet monster's variant allowed at a fortress (`_RefSiegeFortressGuard`: every group at levels 1..140), and
// a summon scroll's unique's (`_RefSiegeLvlSummonMonster`, fortress 0): its group (OrgObjCodeName128) and level.
struct SiegeVariantRow {
    std::uint32_t fortress = 0;
    std::uint32_t refObjId = 0;
    std::string group;
    int level = 0;
};
bool LoadFortressGuards(Database& db, std::vector<SiegeVariantRow>& out, std::string& error);
bool LoadSiegeSummons(Database& db, std::vector<SiegeVariantRow>& out, std::string& error);


} // namespace sr::server
