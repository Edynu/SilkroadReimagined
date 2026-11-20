#pragma once

// The fortress war: the original's rules and numbers, shared by the game
// server and the client, the schedule of its periods, and the client's picture of the fortresses
// (FortressView), fed by the session's fortress events.
//
// Its rules follow the original's game server (the fortress manager, the war's hostility, the heart's capture), the
// original's database tables (`_RefSiegeFortress`, `_Schedule`), and the 1.188 client for what it shows.

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sr/game/FortressProtocol.h"

namespace sr::game {

// --- The rules -----------------------------------------------------------------------------------

// Applying: the guild's level and members.
constexpr int kFortressApplyGuildLevel = 3; // a guild below level 3 -> 0x280C
constexpr int kFortressApplyMembers = 7;    // the guild's member count below 7 -> 0x280D

// What a player's hit does to a structure (the heart, the guard towers, the gates): only the weapon's
// own attack, rolled, times the factor of its kind (TID4), truncated. The structure's defence,
// the attacker's level, stats, mastery and skills do not count. No factor (bare hands): a hit of 1.
struct StructureWeapon {
    bool magic = false; // rolls the weapon's magical attack
    double factor = 0.0;
};
constexpr StructureWeapon StructureWeaponOf(int typeId4) {
    switch (typeId4) {
    case 2: return {false, 0.30};    // Chinese sword
    case 3: return {false, 0.27};    // blade
    case 4: return {false, 0.522};   // spear
    case 5: return {false, 0.4698};  // glaive
    case 6: return {false, 0.126};   // bow
    case 7: return {false, 0.4356};  // European sword
    case 8: return {false, 0.66};    // two-handed sword
    case 9: return {false, 0.2508};  // axe
    case 10: return {true, 0.2075};  // dark staff
    case 11: return {true, 0.2075};  // two-handed staff
    case 12: return {false, 0.141};  // crossbow
    case 13: return {false, 0.165};  // dagger
    case 14: return {true, 0.25};    // harp
    case 15: return {true, 0.2075};  // cleric's staff
    case 16: return {false, 1.0};    // fortress hammer and axe
    default: return {};
    }
}
// How near an NPC must stand (the guilds' NPC reach; the original checks the NPC's function in reach).
constexpr float kFortressNpcReach = 300.0f;

// The refusals' codes (the original's, which the 1.188 client turns into its texts).
enum class FortressError : std::uint16_t {
    None = 0,
    Failed = 2,             // the database's write failed
    NoFortress = 3,         // no such fortress (or the application is for another)
    NoRecallPoint = 5,      // the Cave's leave without a recall point
    NotHolder = 0x2806,     // the fortress is not the asker's guild's (the manager's actions)
    OnlyMaster = 0x2807,    // UIIT_MSG_FORT_ETC_ERROR_ONLYGUILDMASTER_ACTION
    NotTaxPeriod = 0x2808,  // a rate or a levy outside the tax period
    RateRange = 0x2815,     // a rate outside -20..+20
    SameRate = 0x2838,      // the rate it has already
    NotInGuild = 0x280B,    // UIIT_MSG_FORT_ETC_ERROR_NOTJOINGUILD
    GuildLevel = 0x280C,    // UIIT_MSG_FORT_ETC_ERROR_TOOLESSGUILDLV
    GuildMembers = 0x280D,  // UIIT_MSG_FORT_ETC_ERROR_TOOLESSGUILDMB
    Gold = 0x280F,          // UIIT_MSG_INTERACTION_FAIL_NOT_ENOUGH_MONEY
    AlreadyApplied = 0x2810, // UIIT_MSG_FORT_OFFICIAL_FORTWAR_APPLY_ERROR_03
    OwnsFortress = 0x2811,  // UIIT_MSG_FORT_ETC_ERROR_ALREADYPOSSESSFORT
    WrongAlliance = 0x2812, // UIIT_MSG_FORT_ETC_ERROR_WRONGALLIANCE
    NoApplication = 0x2813, // UIIT_MSG_FORT_OFFICIAL_FORTWAR_APPLY_ERROR_01
    NotApplyPeriod = 0x2814, // UIIT_MSG_FORT_OFFICIAL_FORTWAR_APPLY_ERROR_02
    WrongNpc = 0x283A,      // not the fortress's official (applying; the client prints nothing)
    WrongNpcCancel = 0x283B, // the same, cancelling
    InWar = 0x2818,          // the battle aide's work is not for a war's time (UIIT_MSG_FORT_ETC_ERROR_WRONGACTION_FORTWAR)
    // The manager's other rows, the smith and the trainer, the Cave of Discipline (the keys by
    // the client's table):
    HireDone = 0x2809,       // UIIT_MSG_FORT_MANAGER_EMPLOY_DUPLICATE
    ForgeBusy = 0x280A,      // UIIT_MSG_FORT_SMITH_PRODUCT_ERROR_01
    GuildPoints = 0x280E,    // UIIT_MSG_ERROR_GUILD_LEVEL_UP_GP_DEFICIT
    ForgeCount = 0x2816,     // UIIT_MSG_FORT_SMITH_PRODUCT_ERROR_02
    ForgePosition = 0x2817,  // UIIT_MSG_GUILDERR_PERMISSION_DENIED
    ForgeNone = 0x2819,      // UIIT_MSG_FORT_SMITH_PRODUCT_ERROR_03
    ForgeNotDone = 0x281A,   // _04
    ForgeTooMany = 0x281B,   // _05
    ForgeBagFull = 0x281C,   // UIIT_MSG_STRGERR_INVENTORY_FULL
    NotUnion = 0x283D,       // neither the holder's guild nor its union (the client prints nothing)
    DungeonPosition = 0x283E, // not position 1, 2 or 4 (prints nothing)
    IntroLength = 0x283F,    // UIIT_MSG_FORTRESS_ERR_OVER_MAX_GUILD_INTRODUCTION_LEN
    NoBless = 0x2840,        // UIIT_MSG_FORTRESS_ERR_NOT_FOUND_BLESS_BUFF
    BlessFailed = 0x2841,    // UIIT_MSG_FORTRESS_ERR_CANT_ENGAGE_BLESS_BUFF
    NoDungeon = 0x2842,      // UIIT_MSG_FORTRESS_ERR_NOT_FOUND_DUNGEON
    DungeonCount = 0x2843,   // UIIT_MSG_FORTRESS_ERR_CANT_CREATE_DUNGEON_BY_MAX_COUNT
    DungeonOpen = 0x2844,    // UIIT_MSG_FORTRESS_ERR_ALREADY_CREATED_DUNGEON
    DungeonCreate = 0x2845,  // UIIT_MSG_FORTRESS_ERR_FAILED_CREATE_DUNGEON
    DungeonClosed = 0x2847,  // UIIT_MSG_FORTRESS_ERR_NOT_CREATED_DUNGEON
    DungeonPay = 0x2848,     // UIIT_MSG_FORTRESS_ERR_IMPOSSIBLE_ENTRY_DUNGEON
    DungeonFull = 0x2849,    // UIIT_MSG_FORTRESS_ERR_IMPOSSIBLE_ENTRY_DUNGEON_BY_FULL
    BlessWait = 0x284A,      // UIIT_SKILL_USE_FAIL_TIMEDELAY_COOL_TIME
    DungeonTime = 0x284B,    // UIIT_MSG_FORTRESS_ERR_NOT_CREATED_TIME_DUNGEON
    IntroPermission = 0x284C, // the introduction may not be changed (IntroductionModificationPermission 0; prints nothing)
    BlessOverlap = 0x300C,   // the buff may not stand beside one running (a skill's refusal)

    BuildNoReference = 0x281E, // the aide's 0x0A: the zone's structure has no reference row (..._ACTION_ERROR_01)
    BuildNotBarricade = 0x281F, // the zone's structure is no barricade (TID 1/2/5/6)
    BuildWrongFortress = 0x2820, // the zone is not of the fortress asked
    NoStructure = 0x2821,    // UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_01 (0x281E..0x2826, 0x2831, 0x2832)
    UpgradeBranch = 0x2824,
    WrongFortress = 0x2826,
    RepairGold = 0x2830,     // UIIT_MSG_FORT_BATTLEAIDE_REPAIR_FAIL
    RepairFull = 0x2831,
    NoRevive = 0x2832,
    UpgradeRole = 0x2833,    // UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_06 (ours: the permission's line)
    UpgradeLevel = 0x2834,   // UIIT_MSG_FORT_BATTLEAIDE_UPGRADE_FAIL2
    UpgradeGold = 0x2835,    // UIIT_MSG_FORT_BATTLEAIDE_UPGRADE_FAIL
    UpgradePoints = 0x2836,
    UpgradeWait = 0x2837,    // UIIT_MSG_FORT_BATTLEAIDE_UPGRADE_FAIL3
    UpgradeNotWhole = 0x2839, // UIIT_MSG_FORT_BATTLEAIDE_UPGRADE_FAIL4
    // The fortress items:
    ItemNotUsable = 0x18D6,  // UIIT_MSG_FORT_NOT_USE_ITEM (a manual on another kind of zone)
    ItemWrongZone = 0x18D7,  // the zone not the fortress's
    ItemTooFar = 0x18D8,     // beyond the item's Range
    ItemNotWalkable = 0x18D9,
    ItemPosition = 0x18E0,   // UIIT_STT_FORT_ITEMUSE_ERROR_POSITION
    ItemNotInFortress = 0x18E1, // UIIT_STT_FORT_ITEMUSE_ERROR_FORTIN
    ItemNotInWar = 0x18E2,   // UIIT_STT_FORT_ITEMUSE_ERROR_FORTPERIOD
    ItemDefender = 0x18E3,   // UIIT_STT_FORT_ITEMUSE_ERROR_ONLY_ATTGUILD: a defending guild may not
    ItemAttacker = 0x18E4,   // UIIT_STT_FORT_ITEMUSE_ERROR_ONLY_DEFENGUILD
    ItemNotHolder = 0x18E5,  // "You don't own any fortress" (UIIT_MSG_FORT_POSITION_GRANT_FAIL_05)
    ItemPermission = 0x18E6, // UIIT_STT_FORT_ITEMUSE_ERROR_POSITION (the guild's permission)
    ItemCampBuilt = 0x18E8,  // the guild has a war camp already (UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_NOTBUILD)
    ItemTargetFar = 0x18EA,  // a repair kit's structure beyond its Range (UIIT_MSG_FORT_NOT_USE_ITEM)
    ItemWrongTarget = 0x18EC, // not a structure (UIIT_SKILL_USE_FAIL_WRONGTARGET)
    ItemTargetState = 0x18ED, // a structure destroyed, or whole (UIIT_SKILL_USE_FAIL_WRONGTARGET)
    ItemGroup = 0x18E7,      // a guard's or a monster's group not allowed at the fortress (the permission's text)
    NoVariant = 0x1811,      // no variant of the group at the placer's level ("Trying to summon unknown SiegeGuarder")
    NoSummon = 5,            // a scroll's unique: no variant at the reader's level (code 5)
    ItemInWar = 0x18EE,      // not during the war

    ItemLimit = 0x18EF,      // UIIT_MSG_FORT_ITEMUSE_ERR_NUMLIMIT
    ItemAlreadyBuilt = 0x18F1, // UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_ALREADY_BUILD
    ItemInFortress = 0x18F3, // not inside a fortress's world
    ItemZoneTaken = 0x18F4,
    // Removing (the battle aide's 0x16 / 0x17):
    NotGuard = 0x282A,       // UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_04
    NotBarricade = 0x282B,   // _05
    NotOwnObject = 0x282C,   // _06
    DismissRole = 0x282E,    // _06
    RemoveRole = 0x282F,     // _06
    PulleyFortress = 0x2829, // a pulley of another fortress (UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_03)
    GateDestroyed = 0x283C,  // a destroyed gate's pulley (UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_02)
    ReturnUniform = 0x7C06,  // UIIT_MSG_FORT_PORTALSTONE_FAIL_02
    ReturnNotHolder = 0x7C07, // UIIT_MSG_FORT_PORTALSTONE_FAIL_03
    ReturnWaiting = 0x7C08,  // UIIT_MSG_FORT_PORTALSTONE_FAIL_01
    // Ours, for a structure the player may not hit (the original answers with skill refusals; the
    // 1.188 client's texts):
    StructureOutOfWar = 0x2F01,  // UIIT_MSG_FORT_BATTLE_ERR_CAN_ONLYPERIODFORT
    StructureOwnGuild = 0x2F02,  // UIIT_MSG_FORT_BATTLE_ERR_CANT_ATTACK_GUILD
    StructureOwnUnion = 0x2F03,  // UIIT_MSG_FORT_BATTLE_ERR_CANT_ATTACK_ALLYGUILD
    HeartSealed = 0x2F04,        // UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_NOTDESTROYGUARD
    HeartReleasing = 0x2F05,     // UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_DELAYTIME
    NotParticipant = 0x2F06,     // UIIT_MSG_FORT_ENTRANCE_FORTRESS_FAIL_02 (ours: only the war's guilds hit its structures)
    GateOpen = 0x2F07,           // an open gate is not hit (the original's skill refusal 0x3042; the client's line not traced: none)
};

// What an application's checks look at, in the original's order.
struct FortressApplyFacts {
    bool requestsOpen = false;
    bool fortressKnown = false;
    bool atOfficial = false;        // talking with this fortress's official (`RequestNPCName128`)
    std::uint32_t guild = 0;
    bool guildHoldsFortress = false; // any fortress
    bool ownerOfThis = false;
    bool alliedWithOwner = false;    // in the owner's union
    proto::FortressSide side = proto::FortressSide::Attack;
    bool master = false;
    int guildLevel = 0;
    std::size_t members = 0;
    std::uint64_t gold = 0, fee = 0;
    bool applied = false;            // the guild applied for a fortress already
};
FortressError ApplyRefusal(const FortressApplyFacts& f);

struct FortressCancelFacts {
    bool requestsOpen = false;
    bool fortressKnown = false;
    bool atOfficial = false;
    std::uint32_t guild = 0;
    bool master = false;
    bool ownerOfThis = false;
    std::uint32_t appliedFortress = 0; // the guild's application (0: none)
    std::uint32_t fortress = 0;
};
FortressError CancelRefusal(const FortressCancelFacts& f);

// A structure's kind: its characterdata TID4 (TypeID 1/2/5/x; the procedures' table of
// `_SiegeFortressResetStructUpgradeLevel_NoTx`): 1 the heart, 2 a guard tower, 3 a gate, 4 a
// defensive position, 5 a war camp (headquarters), 6 a barricade.
// The structures of a fortress's zones (TID 1,2,5,x: the kind is TID4), then ours for the war's objects that stand at a point
// of their own (`_SiegeFortressObject`, TID 1,2,4,x): a flag (TID4 2), a tablet's monster (3), a guard (4).
enum class StructureKind : std::uint8_t {
    None = 0, Heart = 1, GuardTower = 2, Gate = 3, Defensive = 4, Headquarters = 5, Barricade = 6, Flag = 7, Summoned = 8, Guard = 9
};
inline StructureKind StructureKindOf(int tid1, int tid2, int tid3, int tid4) {
    if (tid1 == 1 && tid2 == 2 && tid3 == 4) return tid4 == 2 ? StructureKind::Flag : tid4 == 3 ? StructureKind::Summoned : tid4 == 4 ? StructureKind::Guard : StructureKind::None;
    return tid1 == 1 && tid2 == 2 && tid3 == 5 && tid4 >= 1 && tid4 <= 6 ? StructureKind(tid4) : StructureKind::None;
}
inline bool IsFortressObject(StructureKind k) { return k == StructureKind::Flag || k == StructureKind::Summoned || k == StructureKind::Guard; }
// How long a war's object stands (a flag 120,000 ms, the other kind 1,200,000 ms; ours: that kind is a tablet's
// monster, inferred). Guards stay.
constexpr int kFlagSeconds = 120, kSummonedSeconds = 1200;
// A repair kit's skill (SKILL_FORT_REPAIR_KIT_0n: 'dura' 12,000, 'puls' 3,000, 'heal' n % of the most health): its
// pulses on the structure (ours: one every 3 s from the use, four in all; inferred from the rows).
constexpr int kRepairKitPulseMs = 3000, kRepairKitMs = 12000;

// A player as the war's checks see it: its guild and union (0: none).
struct WarSide {
    std::uint32_t guild = 0;
    std::uint32_t alliance = 0;
};
// May a player hit a structure: only while the war
// runs; never one of its own guild's or its union's (an owner of none: anyone's).
FortressError StructureRefusal(bool warOn, WarSide owner, WarSide attacker);
// Are two players enemies in a fortress war: not in one
// union, not of one guild (two without a guild are of "one"), and the war runs.
bool PlayersHostileInWar(bool warOn, WarSide a, WarSide b);
// The guild that dealt the heart the most damage: the
// first of the highest (0: none).
std::uint32_t TopDamager(const std::vector<std::pair<std::uint32_t, std::uint64_t>>& damage);

// The war's times: a temporary holder's guild may enter five minutes after the capture
// (300 s; UIIT_MSG_FORT_ETC_ERR_CANT_ENTER_5MINBEFORE); the
// losers are taken out of the fortress 180 s after the war; the heart opens 180 s after the last guard tower fell (the
// client's "in a minute" is wrong); the war begins 30 minutes into its `_Schedule` period (the shard manager's: the
// period's start + 1,800 s; its end warnings at its end - 1800, 1200, 600 and 60 s).
constexpr int kCaptureEntrySeconds = 300;
constexpr int kEvacuateSeconds = 180;
constexpr int kHeartSealSeconds = 180;
constexpr int kWarLeadSeconds = 1800;

// --- The schedule (`_Schedule` rows of `_RefScheduleDefine`) ---------------------------------------

// The schedules the fortress war uses: 5 the application period, 6 the war (SCHEDULE_DEF_SIEGE_*).
constexpr int kScheduleSiegeRequest = 5, kScheduleSiegeWar = 6;

// A `_Schedule` row as the shard manager reads it (by MainInterval_Type: 1 every day, 3 every week on a day, 4 some
// weeks on a day; the others unused by the database's rows): within DateStart..DateEnd, each day (1), each week on
// SubInterval_DayOfWeek (3; 1 Sunday .. 7 Saturday, the day's tm_wday + 1), or (4) in the weeks whose number counted
// from DateStart's week (1, plus one each Sunday after it) modulo MainInterval_TypeDate (0 as TypeDate) is
// SubInterval_Weeks, on that day (the tax row 446: TypeDate 2, Weeks 2, Saturday: the second Saturday of each two
// weeks), from
// its start time for its duration. Times are the server's local time, as the original's.
struct ScheduleRow {
    int define = 0;
    std::int64_t dateStart = 0, dateEnd = 0; // local Unix-like seconds (std::mktime of the dates)
    int mainType = 1;
    int dayOfWeek = 0;
    int hour = 0, minute = 0, second = 0;
    int durationSeconds = 0;
    int weeks = 0;    // SubInterval_Weeks (type 4: the cycle's week it runs in)
    int typeDate = 1; // MainInterval_TypeDate (type 4: the cycle's weeks)
};
// A period [start, end) in Unix seconds.
struct Period {
    std::int64_t start = 0, end = 0;
    bool Contains(std::int64_t t) const { return t >= start && t < end; }
};
// The period of `define` that holds `now`, else the next to come (none: none left).
std::optional<Period> SchedulePeriod(const std::vector<ScheduleRow>& rows, int define, std::int64_t now);

// --- The tax -----------------------------------------------------------

// The tax period's schedule (SCHEDULE_DEF_SIEGE_TAX_PERIOD) and a rate's bounds.
constexpr int kScheduleSiegeTax = 8;
constexpr int kTaxRateMin = -20, kTaxRateMax = 20;
// The tax on `price` at a fortress's NPC or gate: price x rate / 100, cut
// toward 0; a positive rate is waived for the holder's guild and its union (`exempt`), a negative one lowers everyone's.
std::int64_t FortressTax(std::uint64_t price, int rate, bool exempt);
// What `_SiegeFortress.Tax` is written at: once the change gathered is at least this (10,000).
constexpr std::int64_t kTaxWriteThreshold = 10'000;
// A date "YYYY-MM-DD hh:mm:ss[.fff]" as local Unix seconds (0: not a date).
std::int64_t LocalSeconds(const std::string& date);

// --- The client's picture --------------------------------------------------------------------------

class FortressView {
public:
    // A line for the system messages: its text key and arguments in order (a fortress's or a
    // structure's name as a text key to look up; `number` a %d).
    struct Line {
        std::string key;
        std::string guild;
        std::string fortressKey;
        std::string structureKey;
        int number = -1;
        std::uint32_t structureRef = 0; // a structure named by its reference object (the client's tables name it)
        std::string value;              // a plain argument after the fortress's name (a tax rate, a levy's gold)
        std::uint32_t itemRef = 0;      // an item named by its reference object (the forge's lines)
    };
    std::vector<Line> Apply(const proto::FortressEvent& event);
    void ClearEntities() { structures_.clear(); }
    void Forget(std::uint32_t entity) { structures_.erase(entity); }

    const std::vector<proto::FortressRecord>& Fortresses() const { return list_.fortresses; }
    const proto::FortressRecord* Fortress(std::uint32_t id) const;
    bool RequestsOpen() const { return list_.requestsOpen; }
    bool WarOn() const { return list_.warOn; }
    // The fortress at war whose area holds a region (0: none).
    std::uint32_t WarArea(std::uint16_t region) const;
    // The official's window as last told; the server asked it open (taken once).
    const proto::FortressApplyInfo& ApplyInfo() const { return apply_; }
    bool TakeApplyShow() { return std::exchange(applyShow_, false); }
    // The battle aide's structures as last told; the server sent them (taken once).
    const proto::FortressStructureList& StructureList() const { return structureList_; }
    bool TakeStructuresShow() { return std::exchange(structuresShow_, false); }
    // The fortress manager's tax as last told; the server sent it (taken once: the
    // tax management window opens).
    const proto::FortressTaxInfo& TaxInfo() const { return tax_; }
    bool TakeTaxShow() { return std::exchange(taxShow_, false); }
    // A structure in view (null: not one), and all of them by entity.
    const proto::EntityStructure* Structure(std::uint32_t entity) const;
    const std::map<std::uint32_t, proto::EntityStructure>& Structures() const { return structures_; }
    // The player's battle score in the war (its fortress 0: none told since the war began).
    const proto::FortressScore& Score() const { return score_; }
    bool TaxPeriod() const { return list_.taxPeriod; }
    // The manager's other rows: the fortress's hired NPCs (0x385F 0x12 and the answers),
    // the war's schedule and the introduction as last told (taken once: shown as lines).
    std::uint8_t Hired(std::uint32_t fortress) const {
        const auto it = hired_.find(fortress);
        return it != hired_.end() ? it->second : 0;
    }
    std::optional<proto::FortressSchedule> TakeSchedule() { return std::exchange(schedule_, std::nullopt); }
    std::optional<proto::FortressIntroduction> TakeIntroduction() { return std::exchange(introduction_, std::nullopt); }
    // The smith's or the trainer's work and the Cave of Discipline as last told (steps 8 and 10).
    const proto::FortressForge& Forge() const { return forge_; }
    bool TakeForgeShow() { return std::exchange(forgeShow_, false); }
    const proto::FortressDungeon& Dungeon() const { return dungeon_; }
    bool TakeDungeonShow() { return std::exchange(dungeonShow_, false); }
    // Answers since the last call.
    std::vector<proto::FortressAnswer> TakeAnswers() { return std::exchange(answers_, {}); }
    std::uint64_t Revision() const { return revision_; }

private:
    proto::FortressList list_;
    proto::FortressApplyInfo apply_;
    bool applyShow_ = false;
    proto::FortressStructureList structureList_;
    bool structuresShow_ = false;
    proto::FortressTaxInfo tax_;
    bool taxShow_ = false;
    proto::FortressScore score_;
    std::map<std::uint32_t, proto::EntityStructure> structures_;
    std::map<std::uint32_t, std::uint8_t> hired_;
    std::optional<proto::FortressSchedule> schedule_;
    std::optional<proto::FortressIntroduction> introduction_;
    proto::FortressForge forge_;
    bool forgeShow_ = false;
    proto::FortressDungeon dungeon_;
    bool dungeonShow_ = false;
    std::vector<proto::FortressAnswer> answers_;
    std::uint64_t revision_ = 0;
};

// The text key of a refusal's code (the 1.188 client's switch; empty: it prints nothing).
const char* FortressErrorKey(std::uint16_t code);

} // namespace sr::game
