#pragma once

// The fortress war over the network: our own messages (0x11A0-0x11DF)
// for what the original's carry (its fortress requests, 0x705E with an action byte and the NPC, and
// their answers 0xB05E; its fortress broadcasts 0x385F with a kind byte; the fortress list a player
// is told on entering). The refusals keep the original's codes, which the client
// turns into its texts (codes 0x2807..0x284C).

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "sr/game/Protocol.h"

namespace sr::game::proto {

// The original's action numbers (0x705E's byte): 6 the
// fortress official's window, 7 applying, 8 cancelling.
enum class FortressAction : std::uint8_t {
    TaxQuery = 0, TaxRate = 1, TaxLevy = 2, Hired = 3, Hire = 4, Schedule = 5, Query = 6, Apply = 7, Cancel = 8, Build = 0x0A, Upgrade = 0x0B, Repair = 0x0C,
    ForgeQuery = 0x0D, ForgeStart = 0x0E, ForgeCancel = 0x0F, ForgeCollect = 0x10, // the smith's
    TrainQuery = 0x11, TrainStart = 0x12, TrainCancel = 0x13, TrainCollect = 0x14, // the trainer's
    Pulley = 0x15, DismissObject = 0x16, RemoveBarricade = 0x17, Structures = 0x18,
    Introduction = 0x19, Introduce = 0x1A, GpHistory = 0x1B, Bless = 0x1C,
    DungeonQuery = 0x1D, DungeonCreate = 0x1E, DungeonEnter = 0x1F, DungeonLeave = 0x20,
    UseItem = 0x42 /* ours: a fortress item's use (the original's 0x704C) */, Return = 0x41 /* ours: Return to fortress */, Attack = 0x40 /* ours: a structure refused */
};

// An application's side (the original's request type byte, `_SiegeFortressRequest.RequestType`):
// 0 to take the fortress, 1 to defend it (only the owner's union may).
enum class FortressSide : std::uint8_t { Attack = 0, Defend = 1 };

// An NPC's service in its menu (NpcMenu): a fortress's official (`_RefSiegeFortress.RequestNPCName128`;
// the original's NPC function option 0x18): its row "Application / cancellation of fortress war".
constexpr std::uint32_t kNpcFortressOfficial = 0x100;
constexpr std::uint32_t kNpcFortressBattleAide = 0x200; // its structure management (NPC function 0x19)
constexpr std::uint32_t kNpcFortressManager = 0x8000;   // the fortress manager (NPC function 0x17): tax management
constexpr std::uint32_t kNpcFortressPulley = 0x10000;   // a gate's pulley (NPC function 0x1F; STRUCTURE_GATE_PULLEY_*)
constexpr std::uint32_t kNpcFortressSmith = 0x20000;    // the smith (NPC function 0x1A): production and repair
constexpr std::uint32_t kNpcFortressTrainer = 0x40000;  // the trainer (0x1B): training
constexpr std::uint32_t kNpcFortressDungeon = 0x80000;  // the Cave of Discipline's teleporter inside (NPC_SIEGE_DUNGEON_TELEPORT): the way out

// --- Requests (0x11A0) ------------------------------------------------------------------------

// The fortress manager's tax (the original's 0x705E actions 0, 1, 2 with its NPC): the
// fortress's rate and its gathered tax (FortressTaxInfo), a new rate (-20..+20, in the tax period, the holder's
// master), the tax levied to the master (FortressAnswer; the gold in GoldChanged).
struct FortressTaxQuery {
    static constexpr MessageId kId = 0x11A8;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};
struct FortressTaxRate {
    static constexpr MessageId kId = 0x11A9;
    std::uint32_t npc = 0;
    std::int16_t rate = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("rate", rate);
    }
};
struct FortressTaxLevy {
    static constexpr MessageId kId = 0x11AA;
    std::uint32_t npc = 0;
    std::int64_t amount = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("amount", amount);
    }
};

// The fortress official's row "Application / cancellation of fortress war" (action 6, its NPC):
// FortressApplyInfo answers.
struct FortressApplyOpen {
    static constexpr MessageId kId = 0x11A0;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};

// The player's guild applies for a fortress's war (action 7), at its official.
struct FortressApply {
    static constexpr MessageId kId = 0x11A1;
    std::uint32_t npc = 0;
    std::uint32_t fortress = 0;
    FortressSide side = FortressSide::Attack;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("fortress", fortress);
        v("side", side);
    }
};

// Return to fortress (the Action window's 1015; the original's 0x705D): its guild holds it.
struct FortressReturn {
    static constexpr MessageId kId = 0x11A4;
    std::uint32_t fortress = 0;
    template <typename V>
    void Visit(V& v) {
        v("fortress", fortress);
    }
};

// The battle aide's structure management (the original's 0x705E actions 0x18, 0x0C, 0x0B):
// the fortress's structures (FortressStructureList answers), a repair, an upgrade by a branch (0..3: the towers' elements).
struct FortressStructuresQuery {
    static constexpr MessageId kId = 0x11A5;
    std::uint32_t npc = 0;
    std::uint32_t fortress = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("fortress", fortress);
    }
};
struct FortressRepair {
    static constexpr MessageId kId = 0x11A6;
    std::uint32_t npc = 0;
    std::uint32_t fortress = 0;
    std::uint32_t zone = 0; // the structure's place (RefEventStructID)
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("fortress", fortress);
        v("zone", zone);
    }
};
struct FortressUpgrade {
    static constexpr MessageId kId = 0x11A7;
    std::uint32_t npc = 0;
    std::uint32_t fortress = 0;
    std::uint32_t zone = 0;
    std::uint8_t branch = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("fortress", fortress);
        v("zone", zone);
        v("branch", branch);
    }
};

// A barricade built at the battle aide (the original's 0x705E action 0x0A {u32 fortress, u32 zone} at NPC function
// 0x19): the zone's own barricade (its `_RefEventZone.strParam1`) for the asker's guild. The stock 1.188 client never
// sends it but prints its answer (UIIT_MSG_FORT_BATTLEAIDE_CONSTRUCTION_COMPLETE); ours sends it
// from a test script only.
struct FortressBuild {
    static constexpr MessageId kId = 0x11AC;
    std::uint32_t npc = 0;
    std::uint32_t fortress = 0;
    std::uint32_t zone = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("fortress", fortress);
        v("zone", zone);
    }
};

// A gate's pulley worked (the original's 0x705E action 0x15 {u32 fortress, u16 op} at NPC function 0x1F): `op` 2 opens
// the gate, 0 closes it (its state word's bit 1). The answer carries the
// gate's zone and its new state word in `amount`.
struct FortressPulley {
    static constexpr MessageId kId = 0x11B6;
    std::uint32_t npc = 0;
    std::uint32_t fortress = 0;
    std::uint16_t op = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("fortress", fortress);
        v("op", op);
    }
};

// The fortress NPCs' other actions (the original's 0x705E {NPC, action, ...}): the
// manager's hired flags (3), a hire (4: `value` the bit, 1 the battle aide, 2 the smith, 4 the trainer), the war's schedule
// (5), the introduction read (0x19) and written (0x1A: `text`), the GP history (0x1B), a blessing (0x1C: `value` its BlessID),
// the smith's and the trainer's forge (0x0D..0x14: `value` the item, `value2` the count), the Cave of Discipline (0x1D..0x20).
struct FortressManage {
    static constexpr MessageId kId = 0x11AB;
    std::uint32_t npc = 0;
    FortressAction action = FortressAction::Hired;
    std::uint32_t fortress = 0;
    std::uint32_t value = 0, value2 = 0;
    std::string text;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("action", action);
        v("fortress", fortress);
        v("value", value);
        v("value2", value2);
        v("text", text, 255);
    }
};

// A fortress war's item used (the original's 0x704C {u8 slot, u16 TID, ...}): a manual
// (TID4 8) on `zone` (0: ours, the nearest free zone of its kind within its reach), tablets, guards and flags at a point
// (`region`, `x`, `y`, `z`). Answered by FortressAnswer UseItem (refused: the original's item codes 0x18D6..0x18F4).
struct FortressUseItem {
    static constexpr MessageId kId = 0x11BB;
    std::uint8_t slot = 0;
    std::uint32_t fortress = 0;
    std::uint32_t zone = 0;
    std::uint16_t region = 0;
    float x = 0, y = 0, z = 0;
    std::uint32_t target = 0; // a repair kit's structure, the shock bomb's enemy (the original's item use on a target)
    template <typename V>
    void Visit(V& v) {
        v("slot", slot);
        v("fortress", fortress);
        v("zone", zone);
        v("region", region);
        v("x", x);
        v("y", y);
        v("z", z);
        v("target", target);
    }
};

// A barricade taken down, or a guard or a summoned monster dismissed (the original's 0x705E 0x17 / 0x16 {object,
// fortress}; the target window's button): the answer's zone the barricade's.
struct FortressRemove {
    static constexpr MessageId kId = 0x11B5;
    std::uint32_t object = 0;
    std::uint32_t fortress = 0;
    template <typename V>
    void Visit(V& v) {
        v("object", object);
        v("fortress", fortress);
    }
};

// The guild's application taken back (action 8; the fee is not given back).
struct FortressCancel {
    static constexpr MessageId kId = 0x11A2;
    std::uint32_t npc = 0;
    std::uint32_t fortress = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("fortress", fortress);
    }
};

// A development server's switch of the war's periods (the original's GM command /SIEGE with
// STARTREQ, ENDREQ, STARTWAR, ENDWAR; textuisystem SR_IMPLE_COMMAND_SIEGE_*), and ours: the player
// taken beside `fortress`'s heart, or beside the structure `entity` (not 0), or in front of its structure place `zone`
// (not 0: its RefEventStructID, built or not; the item exam's manuals).
enum class FortressDevAction : std::uint8_t { StartRequests = 1, EndRequests = 2, StartWar = 3, EndWar = 4, GoTo = 5, StartTax = 6, EndTax = 7 };
struct FortressDev {
    static constexpr MessageId kId = 0x11A3;
    FortressDevAction action = FortressDevAction::StartRequests;
    std::uint32_t fortress = 0;
    std::uint32_t entity = 0;
    std::uint32_t zone = 0;
    template <typename V>
    void Visit(V& v) {
        v("action", action);
        v("fortress", fortress);
        v("entity", entity);
        v("zone", zone);
    }
};

// --- What the server says (0x11C0) -------------------------------------------------------------

// A fortress's tax as its manager tells it (the original's {u32 fortress, u16 rate, i64 tax}): its rate, the tax
// gathered, whether the tax period runs (when rates are set and tax levied), and which of its NPCs it taxes
// (`_RefSiegeFortress.TaxTargets`, the window's check boxes: bit 0 the smith, 1 the armour shop, 2 the grocer,
// 3 the druggist, 4 the gate, 5 the specialty store, 6 the warp NPC; the 1.188 client reads it from its own
// siegefortress.txt, ours from the server).
struct FortressTaxInfo {
    static constexpr MessageId kId = 0x11C6;
    std::uint32_t npc = 0;
    std::uint32_t fortress = 0;
    std::int16_t rate = 0;
    std::int64_t tax = 0;
    bool period = false;
    std::uint16_t targets = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("fortress", fortress);
        v("rate", rate);
        v("tax", tax);
        v("period", period);
        v("targets", targets);
    }
};

// A request's answer (0xB05E): ok, or the original's refusal code (game::FortressError).
struct FortressAnswer {
    static constexpr MessageId kId = 0x11C0;
    FortressAction action = FortressAction::Apply;
    bool ok = false;
    std::uint16_t error = 0;
    std::uint32_t fortress = 0;
    FortressSide side = FortressSide::Attack;
    std::uint32_t zone = 0; // a repair's or an upgrade's structure (the original's 0x0B / 0x0C answers carry it)
    std::int64_t amount = 0; // a tax rate's new rate, a levy's gold (the original's {1, 1, u16 rate}, {2, 1, i64 amount})
    template <typename V>
    void Visit(V& v) {
        v("action", action);
        v("ok", ok);
        v("error", error);
        v("fortress", fortress);
        v("side", side);
        v("zone", zone);
        v("amount", amount);
    }
};

// A structure as the battle aide's window shows it (ours: the original's 0x18 answer gives the places and their
// waits, the rest comes with 0x385F). `upgrades`: what each branch makes (0: none), `upgradeGold` / `upgradePoints`
// / `upgradeLevel` the next level's price and level, `waitMinutes` until an upgrade is allowed.
struct FortressStructureRecord {
    std::uint32_t zone = 0;
    std::uint16_t region = 0; // its place (objectstring.ifo's): the map's icon
    float x = 0, z = 0;
    std::uint32_t refObjId = 0;
    std::uint8_t kind = 0;
    std::uint32_t hp = 0, maxHp = 0;
    std::uint16_t state = 0;
    std::uint32_t repairCost = 0;
    std::uint32_t upgradeGold = 0, upgradePoints = 0;
    std::uint8_t upgradeLevel = 0;
    std::uint32_t waitMinutes = 0;
    std::vector<std::uint32_t> upgrades;
    template <typename V>
    void Visit(V& v) {
        v("zone", zone);
        v("region", region);
        v("x", x);
        v("z", z);
        v("refObjId", refObjId);
        v("kind", kind);
        v("hp", hp);
        v("maxHp", maxHp);
        v("state", state);
        v("repairCost", repairCost);
        v("upgradeGold", upgradeGold);
        v("upgradePoints", upgradePoints);
        v("upgradeLevel", upgradeLevel);
        v("waitMinutes", waitMinutes);
        v("upgrades", upgrades, 4);
    }
};
struct FortressStructureList {
    static constexpr MessageId kId = 0x11C5;
    std::uint32_t npc = 0;
    std::uint32_t fortress = 0;
    std::vector<FortressStructureRecord> structures;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("fortress", fortress);
        v("structures", structures, 256);
    }
};

// A fortress as everyone is told it: its owner (0: none), the guild holding it for now in a war
// (0: none), its tax rate, whether its war runs.
struct FortressRecord {
    std::uint32_t id = 0;
    std::string nameKey; // SN_FORTRESS_*
    std::uint32_t owner = 0;
    std::string ownerName;
    std::uint32_t holder = 0;
    std::string holderName;
    std::uint16_t taxRate = 0;
    bool atWar = false;
    std::vector<std::uint16_t> regions; // its area
    // As the original's list (0x385F kind 0): the controlling guild's master and the fortress's
    // introduction (`_SiegeFortress.Introduction`), shown in the town banner and the world map's tooltip.
    std::string masterName;
    std::string introduction;
    std::uint32_t crest = 0; // the controlling guild's emblem's revision (the world map's crest box; 0: none)
    template <typename V>
    void Visit(V& v) {
        v("id", id);
        v("nameKey", nameKey, 128);
        v("owner", owner);
        v("ownerName", ownerName, 64);
        v("holder", holder);
        v("holderName", holderName, 64);
        v("taxRate", taxRate);
        v("atWar", atWar);
        v("regions", regions, 1024);
        v("masterName", masterName, 64);
        v("introduction", introduction, 256);
        v("crest", crest);
    }
};

// Every fortress and the periods, on entering and whenever an owner or a period changes.
struct FortressList {
    static constexpr MessageId kId = 0x11C1;
    std::vector<FortressRecord> fortresses;
    bool requestsOpen = false;
    bool warOn = false;
    bool taxPeriod = false; // the list's state bit 4 (1 war, 2 applications, 4 tax)
    template <typename V>
    void Visit(V& v) {
        v("fortresses", fortresses, 16);
        v("requestsOpen", requestsOpen);
        v("warOn", warOn);
        v("taxPeriod", taxPeriod);
    }
};

// The fortress official's window (the original's 0xB05E action 6): a row per fortress the official
// serves, the war's next date and the application period (Unix seconds, 0: none scheduled), and the
// player's guild's application (fortress 0: none).
struct FortressApplyRow {
    std::uint32_t fortress = 0;
    std::string nameKey;
    std::string ownerName;
    std::uint32_t fee = 0;
    bool defend = false; // the player's guild is in this fortress's owner's union: it may only defend
    template <typename V>
    void Visit(V& v) {
        v("fortress", fortress);
        v("nameKey", nameKey, 128);
        v("ownerName", ownerName, 64);
        v("fee", fee);
        v("defend", defend);
    }
};
struct FortressApplyInfo {
    static constexpr MessageId kId = 0x11C2;
    std::uint32_t npc = 0;
    std::vector<FortressApplyRow> rows;
    std::int64_t warStart = 0, warEnd = 0;
    std::int64_t requestStart = 0, requestEnd = 0;
    bool requestsOpen = false;
    std::uint32_t applied = 0; // the guild's application's fortress (0: none)
    FortressSide side = FortressSide::Attack;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("rows", rows, 16);
        v("warStart", warStart);
        v("warEnd", warEnd);
        v("requestStart", requestStart);
        v("requestEnd", requestEnd);
        v("requestsOpen", requestsOpen);
        v("applied", applied);
        v("side", side);
    }
};

// What the war's broadcasts say: the original's 0x385F kinds, by their numbers (the
// 1.188 client's handler and its lines UIIT_MSG_FORT_*). Our fields; the client prints the stock line.
enum class FortressNoticeKind : std::uint8_t {
    WarSoon = 1,               // the war period began: the war in 30 minutes (UIIT_MSG_FORT_WAR_BEFOREBEGIN; 30 fixed in the client)
    WarBegin = 2,              // UIIT_MSG_FORT_WAR_BEGIN
    WarEnd30 = 3,              // UIIT_MSG_FORT_WAR_BEFOREEND, 30 minutes (fixed in the client)
    WarEnd20 = 4,              // 20
    WarEnd10 = 5,              // 10
    WarEnd = 6,                // UIIT_MSG_FORT_WAR_END
    TaxRate = 7,               // `fortress`'s tax rate is `value` (a signed rate in its low 16 bits; no line: the open NPC and tax windows show it)
    Conquer = 8,               // `guild` holds `fortress` (the heart taken; UIIT_MSG_FORT_WAR_CONQUER)
    WarEnd1 = 9,               // 1 minute
    SealReleasing = 0x0A,      // the guard towers fell: the heart open in `value` seconds (UIIT_MSG_FORT_STRUCTURE_STATUS_CANCEL)
    StructureDestroyed = 0x0B, // `nameKey` destroyed (UIIT_MSG_FORT_STRUCTURE_STATUS_DESTROY; a war camp of `guild`: _CAMP_STATUS_DESTROY)
    RankUp = 0x0E,             // `guild` (a player's name) acquired the rank `value`, its buff's name `nameKey` (UIIT_MSG_FORT_BATTLERANK_GRANT)
    Hired = 0x12,              // the fortress's hired NPCs: `value` the flags (no line; the manager's hire rows)
    RewardMaster = 0x13,       // the war's reward to the holder's master (UIIT_MSG_FORT_REWARD_MESSAGE_MASTER)
    Reward = 0x14,             // `value` 0: printed as 0x13 (the client prints only 0)
    DungeonCreated = 0x15,     // UIIT_MSG_FORTRESS_DUNGEON_CREATE_COMPLATE_MSG
    DungeonClosed = 0x16,      // UIIT_MSG_FORTRESS_DUNGEON_CLOSE_COMPLATE_MSG

    TaxOpen = 0x31,            // the tax period began (no line)
    TaxClosed = 0x32,          // it ended
    RequestsOpen = 0x33,       // the application period began (no line)
    RequestsClosed = 0x34,     // it ended
    HeartOpen = 0x40,          // ours: the heart may be attacked (no line; the client counts the 180 s itself)
    ForgeDone = 0x41,          // ours: the smith's or the trainer's work `value` (its item) done (UIIT_MSG_FORT_SMITH_PRODUCT_COMPLETE)
};
// The end warning's kind for `minutes` before the war's end (the original's 3, 4, 5, 9: 30, 20, 10, 1 minutes).
constexpr FortressNoticeKind WarEndWarning(int minutes) {
    return minutes >= 30 ? FortressNoticeKind::WarEnd30
           : minutes >= 20 ? FortressNoticeKind::WarEnd20
           : minutes >= 10 ? FortressNoticeKind::WarEnd10
                           : FortressNoticeKind::WarEnd1;
}
// The minutes the stock client prints for a warning's kind (0: not a warning).
constexpr int WarningMinutes(FortressNoticeKind k) {
    switch (k) {
    case FortressNoticeKind::WarSoon: return 30;
    case FortressNoticeKind::WarEnd30: return 30;
    case FortressNoticeKind::WarEnd20: return 20;
    case FortressNoticeKind::WarEnd10: return 10;
    case FortressNoticeKind::WarEnd1: return 1;
    default: return 0;
    }
}
struct FortressNotice {
    static constexpr MessageId kId = 0x11C3;
    FortressNoticeKind kind = FortressNoticeKind::WarBegin;
    std::uint32_t fortress = 0;
    std::uint32_t value = 0;
    std::string guild;
    std::string nameKey; // a structure's name (characterdata's)
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("fortress", fortress);
        v("value", value);
        v("guild", guild, 64);
        v("nameKey", nameKey, 128);
    }
};

// A structure in view (its entity): which fortress, its kind (game::StructureKind) and owner, so the
// client shows the structure's target window and knows whom it may hit (told after its appearance).
struct EntityStructure {
    static constexpr MessageId kId = 0x11C4;
    std::uint32_t entity = 0;
    std::uint32_t fortress = 0;
    std::uint8_t kind = 0;
    std::uint32_t owner = 0;
    std::uint32_t maxHp = 0;
    std::uint16_t pickRadius = 0, pickHeight = 0; // the cylinder a click takes it in (ours, from its model; 0: unknown)
    std::uint16_t state = 0;                      // bit 0 destroyed, bit 1 open (a gate: the original's state word)
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("fortress", fortress);
        v("kind", kind);
        v("owner", owner);
        v("maxHp", maxHp);
        v("pickRadius", pickRadius);
        v("pickHeight", pickHeight);
        v("state", state);
    }
};

// The player's battle score in a fortress war (the original's 0x385F 0x11 {u32, kills, deaths}): after
// each of his kills and deaths, and on entering while the war runs; his battle rank with it (ours). The client's
// player mini info shows it while he stands in a fortress at war.
struct FortressScore {
    static constexpr MessageId kId = 0x11C7;
    std::uint32_t fortress = 0;
    std::uint32_t kills = 0, deaths = 0;
    std::uint8_t rank = 0;
    template <typename V>
    void Visit(V& v) {
        v("fortress", fortress);
        v("kills", kills);
        v("deaths", deaths);
        v("rank", rank);
    }
};

// A structure's or a war object's health changed without a hit (a repair kit's pulse; ours, the original's health update):
// to those who see it.
struct StructureHealth {
    static constexpr MessageId kId = 0x11C8;
    std::uint32_t entity = 0;
    std::uint32_t hp = 0, maxHp = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("hp", hp);
        v("maxHp", maxHp);
    }
};

// The war's schedule (the manager's action 5, `CIFFortressBusiness` "Jobs for Fortress War"; the original's answer {ten u8,
// u8 applied}): the last and the next war's month, day, weekday (0 Sunday), hour and minute (all 0: none), and whether the
// player's guild applied.
struct FortressSchedule {
    static constexpr MessageId kId = 0x11C9;
    std::uint32_t fortress = 0;
    std::array<std::uint8_t, 5> last{}, next{};
    bool applied = false;
    template <typename V>
    void Visit(V& v) {
        v("fortress", fortress);
        for (std::size_t i = 0; i < 5; ++i) v("last", last[i]);
        for (std::size_t i = 0; i < 5; ++i) v("next", next[i]);
        v("applied", applied);
    }
};
// The holder's introduction (the manager's 0x19 answer {u32 fortress, string}).
struct FortressIntroduction {
    static constexpr MessageId kId = 0x11CA;
    std::uint32_t fortress = 0;
    std::string text;
    template <typename V>
    void Visit(V& v) {
        v("fortress", fortress);
        v("text", text, 255);
    }
};
// A forge's work (the smith's 0x0D / the trainer's 0x11 answer {fortress, has, [item, i16 count, u8 done, u64]}): the item,
// how many, whether it is done and when it finishes (Unix seconds).
struct FortressForge {
    static constexpr MessageId kId = 0x11CB;
    std::uint32_t npc = 0;
    std::uint8_t side = 0; // 0 the smith, 1 the trainer
    std::uint32_t fortress = 0;
    bool has = false;
    std::uint32_t item = 0;
    std::uint16_t count = 0;
    bool done = false;
    std::int64_t finishes = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("side", side);
        v("fortress", fortress);
        v("has", has);
        v("item", item);
        v("count", count);
        v("done", done);
        v("finishes", finishes);
    }
};
// The Cave of Discipline (the manager's 0x1D answer {u8 count, u8 open, [u8 players], u8 admitted}): the day's creations,
// whether one is open and how many are inside, whether the player was admitted to it.
struct FortressDungeon {
    static constexpr MessageId kId = 0x11CC;
    std::uint32_t npc = 0;
    std::uint32_t fortress = 0;
    std::uint8_t count = 0;
    bool open = false;
    std::uint8_t players = 0;
    bool admitted = false;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("fortress", fortress);
        v("count", count);
        v("open", open);
        v("players", players);
        v("admitted", admitted);
    }
};

// Every message the server sends of fortresses, as one session event.
using FortressEvent = std::variant<FortressAnswer, FortressList, FortressApplyInfo, FortressNotice, EntityStructure, FortressStructureList, FortressTaxInfo,
                                   FortressScore, StructureHealth, FortressSchedule, FortressIntroduction, FortressForge, FortressDungeon>;

// The fortress messages' range (0x11A0-0x11DF); the server's are from 0x11C0.
inline bool IsFortressMessage(MessageId id) { return id >= 0x11A0 && id < 0x11E0; }

// A server's fortress message as the session's event; false for another id or a malformed payload.
inline bool DecodeFortressEvent(MessageId id, std::span<const std::byte> payload, FortressEvent& out) {
    const auto as = [&]<typename T>(T message) {
        if (!Decode(payload, message)) return false;
        out = std::move(message);
        return true;
    };
    switch (id) {
    case FortressAnswer::kId: return as(FortressAnswer{});
    case FortressTaxInfo::kId: return as(FortressTaxInfo{});
    case FortressList::kId: return as(FortressList{});
    case FortressApplyInfo::kId: return as(FortressApplyInfo{});
    case FortressNotice::kId: return as(FortressNotice{});
    case EntityStructure::kId: return as(EntityStructure{});
    case FortressStructureList::kId: return as(FortressStructureList{});
    case FortressScore::kId: return as(FortressScore{});
    case StructureHealth::kId: return as(StructureHealth{});
    case FortressSchedule::kId: return as(FortressSchedule{});
    case FortressIntroduction::kId: return as(FortressIntroduction{});
    case FortressForge::kId: return as(FortressForge{});
    case FortressDungeon::kId: return as(FortressDungeon{});
    default: return false;
    }
}

inline const char* FortressMessageName(MessageId id) {
    switch (id) {
    case FortressAnswer::kId: return "FortressAnswer";
    case FortressTaxInfo::kId: return "FortressTaxInfo";
    case FortressTaxQuery::kId: return "FortressTaxQuery";
    case FortressTaxRate::kId: return "FortressTaxRate";
    case FortressTaxLevy::kId: return "FortressTaxLevy";
    case FortressList::kId: return "FortressList";
    case FortressApplyInfo::kId: return "FortressApplyInfo";
    case FortressNotice::kId: return "FortressNotice";
    case EntityStructure::kId: return "EntityStructure";
    case FortressStructureList::kId: return "FortressStructureList";
    case FortressScore::kId: return "FortressScore";
    case StructureHealth::kId: return "StructureHealth";
    case FortressSchedule::kId: return "FortressSchedule";
    case FortressIntroduction::kId: return "FortressIntroduction";
    case FortressForge::kId: return "FortressForge";
    case FortressDungeon::kId: return "FortressDungeon";
    case FortressManage::kId: return "FortressManage";
    case FortressPulley::kId: return "FortressPulley";
    case FortressUseItem::kId: return "FortressUseItem";
    case FortressRemove::kId: return "FortressRemove";
    default: return nullptr;
    }
}

} // namespace sr::game::proto
