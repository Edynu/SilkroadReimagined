#pragma once

// The game's object tables in Media.pk2 server_dep\silkroad\textdata\.
// Rows are tab-separated (the files are UTF-16LE); comment lines start with "//".
//
// characterdata*.txt (RefObjChar):
//   0 service flag (1 = in use), 1 RefObjID, 2 codename (e.g. CHAR_EU_MAN_NOBLE),
//   the first column ending in ".bsr" = the model resource (relative to Data.pk2\res).
// itemdata*.txt (RefObjItem):
//   0 service, 1 RefObjID, 2 codename, 4 linked codename (its model is used when
//   this row has none), 9..12 type ids (TID1..4), 14 country (0 China, 1 Europe,
//   3 both), 52 model .bsr ("xxx" = none, relative to res\), 58 sex (0 woman,
//   1 man, 2 both).

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sr::assets {

class FileSystem;

struct ObjectRow {
    std::uint32_t refObjId = 0;
    std::string codename;
    std::array<int, 4> typeId{}; // TID1..TID4 (cols 9..12)
    std::string nameKey;         // col 5 (SN_...)
    int level = 0;               // col 57
    std::uint32_t maxHp = 0;     // col 59
    int rarity = 0;              // col 15: a monster's kind (0 general, 3 unique; champions and giants are made at spawn); a transport's capacity class (1, 2)
    bool cashItem = false;       // col 7: a mall object (a mall transport's bag is 1.4 times a bought one's)
    std::uint32_t experience = 0; // col 79: what killing it gives
    float bodyRadius = 0;        // col 50
    std::array<std::uint32_t, 10> skills{}; // cols 83..92: a monster's skills (skilldata ids; 0 none)
    // col 97 (_RefObjChar.Except_4): an attack pet's minutes to eat 100 HGP (its HGP drains by 100 / (this x 60) a
    // second; 5 for every wolf and bear)
    int except4 = 0;
    // cols 81, 82 (the original's Knockdown, KO_RecoverTime): bit 0 it may be knocked down, bit 1 back;
    // how long it lies, before the skill's own time.
    int knockdown = 0;
    int knockdownRecoverMs = 0;
    // A monster's fighting numbers: cols 71 / 72 physical / magical defence, 73 / 74 physical / magical
    // parry, 75 evasion, 76 block, 77 hit rate, 78 critical.
    float phyDefence = 0, magDefence = 0, phyParry = 0, magParry = 0, evasion = 0, block = 0, hitRate = 0, critical = 0;
    int country = 0; // col 14: 0 China, 1 Europe, 3 either
    int scalePercent = 100;      // col 48: the body's size in percent
    std::uint32_t maxContain = 0; // col 40 (MaxContain): a monster's HP back each 4 s while it stands (its natural recovery)
    int materialKind = 0;        // col 93: the .bsr material set it wears (champions wear set 2)
    float walkSpeed = 0, runSpeed = 0; // cols 46, 47
    std::string iconPath;        // col 54, relative to the icon folder (a COS's status icon)
    bool canRide = false;        // col 66: a COS that is boarded through its Ride command (transports)
    std::string modelPath; // .bsr; a clone row (a monster's variant) takes its base row's
    std::string linkedCodename; // col 4: the base row of a clone, which has no model of its own
    bool inService = false;
};

// Rows of one characterdata shard (text already decoded to UTF-8).
bool DecodeObjectRows(std::string_view utf8Text, std::vector<ObjectRow>& out, std::string& error);

// Codename -> row, from every characterdata*.txt shard in the file system.
class CharacterTable {
public:
    bool Load(const FileSystem& fs, std::string& error);
    const ObjectRow* Find(std::string_view codename) const;
    const ObjectRow* Find(std::uint32_t refObjId) const;
    // A row added by hand (tests).
    void Add(ObjectRow row) {
        byId_[row.refObjId] = row.codename;
        rows_[row.codename] = std::move(row);
    }
    const std::unordered_map<std::string, ObjectRow>& rows() const { return rows_; }
    // The row's model as a file-system path: as written, or under "res\" (the
    // tables name models relative to Data.pk2's res folder). Empty if neither exists.
    static std::string ResolveModel(const FileSystem& fs, const ObjectRow& row);
    std::size_t size() const { return rows_.size(); }

private:
    std::unordered_map<std::string, ObjectRow> rows_;
    std::unordered_map<std::uint32_t, std::string> byId_;
};

struct ItemRow {
    std::uint32_t refObjId = 0;
    std::string codename;
    std::string linkedCodename;
    std::array<int, 4> typeId{}; // TID1..TID4
    int country = 0;
    int sex = 2;
    std::string modelPath; // as written; empty when none
    std::string dropModelPath; // col 53: the model lying on the ground (pouch, box, coins); empty when none
    bool inService = false;
    std::string nameKey;  // SN_... in textdataname
    std::string descKey;
    std::string iconPath; // relative to icon\ (e.g. item\china\...ddj)
    int requiredLevel = 0;
    int maxStack = 1;
    std::uint64_t sellPrice = 0; // col 31: what an NPC pays for one
    std::uint64_t price = 0;     // col 26 (_RefObjCommon.Price): alchemy's measure (the Rondos, dismantling)
    // cols 27, 28 (_RefObjCommon.CostRepair, CostRevive): an NPC's repair, max(1, CostRepair / the most durability) gold a
    // point, and the revival of a broken one.
    std::uint64_t costRepair = 0, costRevive = 0;
    bool canRepair = false, canRevive = false; // cols 22, 23 (_RefObjCommon.CanRepair, CanRevive): an NPC or the hammer may mend it
    bool cashItem = false;       // col 7: a mall item (never destroyed by a failed reinforcement)
    bool canSell = true;         // col 17: 0 the shops refuse it
    bool canTrade = true;        // col 16 (_RefObjCommon.CanTrade): 0 it cannot be offered in an exchange (0x183A)
    // col 20 (_RefObjCommon.CanDrop), the original's two bits: 1 it may be dropped by hand (not when it cannot be traded and
    // CanDrop is 1), 2 a death may drop it. 3 for a row made without the column.
    int canDrop = 3;
    // The rest of _RefObjCommon's flags, as written (the item catalogue shows them): col 18 CanBuy, 19 CanBorrow (a bit
    // mask: 255 in most rows), 21 CanPick, 24 CanUse (0 not, 1 used from the bag, 129 the mall's services), 25 CanThrow;
    // col 13 DecayTime (ms on the ground), 29 CostBorrow, 30 KeepingFee (the storage's).
    // canBorrow (col 19): 0x80 the account's storage takes it, 0x40 a guild's (255 for a row made without the column).
    int canBuy = 1, canBorrow = 0xFF, canPick = 1, canUse = 0, canThrow = 0;
    int decayMs = 0;
    std::uint64_t costBorrow = 0, keepingFee = 0;
    int setId = 0; // col 62 (_RefObjItem.SetID): the set an item belongs to (0 none)
    // col 15: 0 normal, 2 rare (Seal of Star / Moon / Sun), 3 a set item (1.188's Egyptian A / B),
    // 6 a rare set item, 7 legend (later content; vsro's _RefObjCommon has all of them).
    int rarity = 0;
    bool Rare() const { return rarity == 2 || rarity == 6; }
    bool SetItem() const { return rarity == 3 || rarity == 6; }
    int itemClass = 0; // col 61: degree = (class - 1) / 3 + 1; the seal = (class - 1) % 3
    // cols 32..39: up to four requirements (type 1 = character level, else a mastery id)
    std::array<int, 4> requirementTypes{}, requirementLevels{};
    int requiredStr = 0, requiredInt = 0; // cols 59, 60
    // cols 63..117 as numbers: the white-stat ranges (min / max / per plus) the tooltip
    // interpolates with the item's variance. Stat(column) reads one.
    std::vector<float> stats;
    float Stat(int column) const {
        const int i = column - kFirstStatColumn;
        return i >= 0 && static_cast<std::size_t>(i) < stats.size() ? stats[static_cast<std::size_t>(i)] : 0.0f;
    }
    static constexpr int kFirstStatColumn = 63;
    // cols 118..157: Param1..20 (potion amounts, cure points, a return scroll's ms...) and the text
    // beside each (a speed scroll's skill codename, "COOLTIME:0x..", RESURRECT...; empty for xxx).
    std::array<int, 20> params{};
    std::array<std::string, 20> paramTexts;
    int maxPossess = -1;         // col 40: the most one character may carry (-1: no limit)
    std::string useKind;         // col 123: what using it does (RESURRECT / THIEFDEN: a return scroll, Param1 its ms)
    // The "COOLTIME:0xGG" description pair (cols 119..157, the last wins): how long after use the
    // item cannot be used again (ms) and the group of items that share that wait; 0 / 0 when none.
    int cooldownMs = 0, cooldownGroup = 0;
    int maxMagicOptions = 0;     // col 158
    // col 159 (_RefObjItem.ChildItemCount): an avatar dress that takes an attachment has 1 (114 of the 1.188 dresses).
    int childItems = 0;
};

// leveldata.txt: per level (column 0) the EXP to the next one (column 1).
// expToNext[level] (index 0 unused); false when no rows.
bool DecodeLevelTable(std::string_view utf8Text, std::vector<std::uint64_t>& expToNext, std::string& error);
// Column 2: the skill points a mastery costs to go from this level to the next.
bool DecodeMasteryCosts(std::string_view utf8Text, std::vector<std::uint32_t>& spByLevel, std::string& error);
// Column 5: the experience a general monster of this level gives (the skill experience's measure).
bool DecodeMonsterExperience(std::string_view utf8Text, std::vector<std::uint32_t>& byLevel, std::string& error);
// Columns 6..8 (_RefLevel.JobExp_Trader / _Robber / _Hunter): the experience each job level needs (-1 past the
// table); byLevel[level] (index 0 unused).
bool DecodeJobExperience(std::string_view utf8Text, std::vector<std::array<std::int64_t, 3>>& byLevel, std::string& error);
// levelgold.txt: the gold a monster of each level drops, min and max (cols 1, 2).
bool DecodeLevelGold(std::string_view utf8Text, std::vector<std::pair<std::uint32_t, std::uint32_t>>& byLevel, std::string& error);

// magicoption.txt: 0 service, 1 the param id (the low half of an item's
// option on the wire), 2 the MATTR_* name, 3 the param name ("+" shown, "-" a penalty, "@"
// hidden), 4 the degree, 8..10 packed range words (u16 pairs: min / steps up to max).
struct MagicOptionRow {
    std::uint32_t paramId = 0;
    std::string optionName, paramName;
    int degree = 0;
    // col 7 (_RefMagicOpt.Param1): the attribute's four-letter code ("str" 0x737472, "int" 0x696E74; Magic POP's prize
    // tokens name it).
    std::uint32_t attribute = 0;
    // col 5 (_RefMagicOpt.Prob): the row's share among its option's levels in a drop's million roll; its weight there
    // is trunc(Prob x 1,000,000).
    float prob = 0.0f;
    bool hasRange = false;
    std::array<std::uint32_t, 3> rangeWords{}; // cols 8..10: Param2, Param3, Param4
    // cols 29, 31, .. 47 (AvailItemGroup1..10): the items it may be put on (weapon, shield, armor, accessory, helm,
    // mail, pants, earring, necklace, ring, ...); "xxx" left out.
    std::vector<std::string> availGroups;
    // cols 30, 32, .. 48 (ReqClass1..10): the least ItemClass each of availGroups takes, beside it.
    std::vector<int> reqClasses;
};
bool DecodeMagicOptions(std::string_view utf8Text, std::vector<MagicOptionRow>& out, std::string& error);

// magicoptionassign.txt (the original's `_RefMagicOptAssign`: the client's 74 rows are the database's, every column):
// 0 service, 1 Race (an item's Country: 0 China, 1 Europe, 3 both), 2 TID3, 3 TID4, 4..28 AvailMOpt1..25: the options
// (MOptName128) a dropped piece of that kind may get as random blues, in column order. As the original builds them: a
// row of Service 0 is left out, the list ends at its first "xxx" or empty cell, an option
// already listed is not listed again; rows of the same kind make one list.
struct MagicOptionAssignRow {
    int race = 0, tid3 = 0, tid4 = 0;
    std::vector<std::string> options;
};
bool DecodeMagicOptionAssign(std::string_view utf8Text, std::vector<MagicOptionAssignRow>& out, std::string& error);

// refmagicoptgroup.txt (the original's `_RefMagicOptGroup`, the same rows in the client's file and the database):
// 0 service, 1 LinkID, 2 MagicType, 3 its codename, 4 MOptID, 5 MOptLevel, 6 Value. A socket stone's Param1 names a
// LinkID of MagicType 3, whose rows are its skills (MOptID a skilldata id) by level.
struct MagicOptGroupRow {
    int link = 0, magicType = 0;
    std::string codename;
    std::uint32_t optId = 0;
    int optLevel = 0, value = 0;
};
bool DecodeMagicOptGroups(std::string_view utf8Text, std::vector<MagicOptGroupRow>& out, std::string& error);

// Quests. questdata.txt: 0 service, 1 id, 2 codename, 3 level, 5 the
// title key (SN_<CODENAME>), 6 the reward window's title (SN_PAY_*), 8 its text (SN_PAYCON_*,
// SML2), 9 / 10 the guide texts (SN_NN_*, SN_NC_*). questcontentsdata.txt, by codename: 4 the
// give-up warning (0: UIIT_MSG_QUEST_GIVEUP_WINDOW_1, else _2), 5..12 the objective keys
// (SN_CON_*, "xxx" none). Texts are in textquest.txt.
struct QuestRow {
    std::uint32_t id = 0;
    std::string codename;
    int level = 0;
    std::string titleKey, payKey, payTextKey, guideKey, conditionKey;
    int giveUpWarning = 0;
    std::vector<std::string> objectiveKeys;
};
bool DecodeQuestData(std::string_view utf8Text, std::vector<QuestRow>& out, std::string& error);
// Fills the rows' give-up warning and objective keys from questcontentsdata (first row wins).
void DecodeQuestContents(std::string_view utf8Text, std::vector<QuestRow>& rows);
class QuestTable {
public:
    bool Load(const FileSystem& fs, std::string& error);
    const QuestRow* Find(std::uint32_t id) const;
    const QuestRow* Find(std::string_view codename) const;
    const std::vector<QuestRow>& Rows() const { return rows_; }

private:
    std::vector<QuestRow> rows_;
};

// gameguidedata.txt (the Game Guide): 0 enabled, 1 id, 3 depth (0 a
// category, 1 an article), 4 a category's article count / an article's category, 5 the
// article's texthelp key, 6 its title key. Ids 50001..50021 are the event guide's articles
// for events 1..21; the rest below 50000 the general guide.
struct GuideRow {
    std::uint32_t id = 0;
    bool enabled = false;
    int depth = 0;
    int parentOrCount = 0;
    std::string contentKey, menuKey;
};
bool DecodeGameGuide(std::string_view utf8Text, std::vector<GuideRow>& out, std::string& error);

// Rows of one itemdata shard (text already decoded to UTF-8).
bool DecodeItemRows(std::string_view utf8Text, std::vector<ItemRow>& out, std::string& error);

// The teleports (teleportbuilding / teleportdata / teleportlink): gates
// (buildings standing where the map draws a portal), the points they and NPCs stand for (with
// where a traveller arrives), and the links between points with their fee. `height`, `radius`,
// `recall` and the links' level range are not confirmed yet.
struct TeleportGate {
    std::uint32_t refObjId = 0;
    std::string codename, nameKey; // cols 2, 5
    std::uint16_t region = 0;      // col 41: where it stands (a dungeon's region has bit 15 set)
    float x = 0, y = 0, z = 0;     // cols 43..45, in that region
    float height = 0, radius = 0;  // cols 49, 50: the box a click picks
    std::string model;             // col 52: its model (a gate spawned as an object, the Forgotten World's gap)
    std::array<std::uint8_t, 4> typeId{}; // cols 9..12: TypeID1..4 (4/1/1/1 a fortress's gate, 4/1/1/2 its revival gate)
    bool FortressGate() const { return typeId == std::array<std::uint8_t, 4>{4, 1, 1, 1}; }
};
struct TeleportPoint {
    std::uint32_t id = 0;
    std::string codename;            // col 2
    std::uint32_t ownerRefObjId = 0; // col 3: the gate or NPC it belongs to
    std::string nameKey;             // col 4
    std::uint16_t region = 0;        // col 5: where a traveller arrives (0 with 6..8: nowhere)
    float x = 0, y = 0, z = 0;       // cols 6..8
    bool recall = false;             // col 10: may be designated as the return point
    bool HasArrival() const { return region != 0 || x != 0 || y != 0 || z != 0; }
};
struct TeleportLink {
    std::uint32_t from = 0, to = 0; // cols 1, 2: teleportdata ids
    std::uint32_t fee = 0;          // col 3
    int minLevel = 0, maxLevel = 0; // a level restriction (kind 1) among cols 6..20, 0: none
    // The five restrictions of cols 6..20 (`_RefTeleLink` Restrict1..5 with Data1..5), checked in turn
    // (in this order): kind 1 the level in
    // [a, b], 2 no transport out, 3 a mask (unused), 4 item a in the bag, 5 item a used up, 6 the job
    // state 4, 7 the job's suit with a transport; 0 none.
    struct Restriction {
        int kind = 0;
        std::uint32_t a = 0, b = 0;
    };
    std::array<Restriction, 5> restrictions{};
};
bool DecodeTeleportGates(std::string_view utf8Text, std::vector<TeleportGate>& out);
bool DecodeTeleportPoints(std::string_view utf8Text, std::vector<TeleportPoint>& out);
bool DecodeTeleportLinks(std::string_view utf8Text, std::vector<TeleportLink>& out);

// npcpos.txt: where the NPCs stand (and the monsters' spawn points): characterdata RefObjID, the
// region (signed in the file: dungeons are negative, bit 15 set), x, y, z in it. No facing.
struct NpcPosition {
    std::uint32_t refObjId = 0;
    std::uint16_t region = 0;
    float x = 0, y = 0, z = 0;
};
bool DecodeNpcPositions(std::string_view utf8Text, std::vector<NpcPosition>& out);
// server_dep\silkroad\textdata\atstructeffect.txt: a fortress structure's models by damage.
struct AtStructLook {
    std::string models[3]; // whole, damaged, destroyed; empty: the row's AssocFileObj
};
class AtStructTable {
public:
    void Decode(std::string_view text);
    const AtStructLook* Find(const std::string& codename) const;
    std::size_t Size() const { return looks_.size(); }

private:
    std::unordered_map<std::string, AtStructLook> looks_;
};

// server_dep\silkroad\textdata\actionwnddata.txt: the Action window's icons (the main window's Actions page):
// id, service, a Korean name, the text key, the icon, its group (1 commands, 4 emotes), its place in the
// group. `//` starts a line left out.
struct ActionRow {
    std::uint32_t id = 0;
    std::string textKey, icon;
    int group = 0, slot = 0;
};
std::vector<ActionRow> DecodeActionData(std::string_view text);

class TeleportTable {
public:
    bool Load(const FileSystem& fs, std::string& error);
    // From the three tables' text (UTF-8; tests make their own gates).
    void Decode(std::string_view gates, std::string_view points, std::string_view links);
    const TeleportGate* Gate(std::uint32_t refObjId) const;
    // The point a gate or NPC stands for (its reference id), and where it leads.
    const TeleportPoint* PointOf(std::uint32_t ownerRefObjId) const;
    const TeleportPoint* Point(std::uint32_t id) const;
    const TeleportPoint* PointNamed(std::string_view codename) const;
    std::vector<const TeleportLink*> LinksFrom(std::uint32_t pointId) const;
    const std::vector<TeleportGate>& Gates() const { return gates_; }
    const std::vector<TeleportPoint>& Points() const { return points_; }
    const std::vector<TeleportLink>& Links() const { return links_; }

private:
    std::vector<TeleportGate> gates_;
    std::vector<TeleportPoint> points_;
    std::vector<TeleportLink> links_;
};

class ItemTable {
public:
    bool Load(const FileSystem& fs, std::string& error);
    const ItemRow* Find(std::string_view codename) const;
    const ItemRow* Find(std::uint32_t refObjId) const;
    // A row added by hand (tests).
    void Add(ItemRow row) {
        byId_[row.refObjId] = row.codename;
        byCode_[row.codename] = std::move(row);
    }
    // The row's model as a file-system path (falling back once to the linked row's
    // model, like the original). Empty when there is none.
    std::string ResolveModel(const FileSystem& fs, const ItemRow& row) const;
    // The model of the item dropped on the ground (col 53), else its own model.
    std::string ResolveDropModel(const FileSystem& fs, const ItemRow& row) const;
    std::size_t size() const { return byCode_.size(); }
    const std::unordered_map<std::string, ItemRow>& rows() const { return byCode_; }

private:
    std::unordered_map<std::string, ItemRow> byCode_;
    std::unordered_map<std::uint32_t, std::string> byId_;
};

// skilldata rows (SkillData_*.txt shards).
struct SkillRow {
    std::uint32_t id = 0;        // col 1
    std::uint32_t group = 0;     // col 2
    std::string codename;        // col 3
    std::string baseName;        // col 5: the animation set key (skilleffect.txt); xxx for monster skills
    std::uint32_t original = 0;  // col 6 (Basic_Original): the skill it is a copy of (a monster kin's attack: the Blood
                                 // Sandman's ATTACK01 is the Dark Sandman's), whose sound rows it has when it has none
    int level = 0;               // col 7, within the group
    int activity = 0;            // col 8: 0 = passive
    std::uint32_t chainNext = 0; // col 9: a chain's next sub-row (sub-rows are never shown or learned)
    int preparingMs = 0, castingMs = 0, durationMs = 0; // cols 11, 12, 13
    int cooldownMs = 0;          // col 14
    int coolTimeMs = 0;          // col 15 (Action_CoolTime)
    int flyingSpeed = 0;         // col 16 (Action_FlyingSpeed): a shot's or a throw's flight
    int interruptible = 0;       // col 17 (Action_Interruptable)
    int inTown = 0;              // col 20 (Action_InTown)
    int cooldownGroup = 0;       // col 18, its top byte: skills of one group share the wait
    std::uint32_t overlap = 0;   // col 18 (Action_Overlap) whole: four bytes, a buff's overlap groups (game::OverlapGroups)
    int autoAttack = 0;          // col 19 (Action_AutoAttackType): 1 the basic attack follows it on its target, else the fight ends
    int range = 0;               // col 21, Action_Range (in units, as positions; 0: the weapon's)
    int aiType = 0;              // col 67 (AI_SkillType): 72 'H' a heal that makes threat, 68 'D' a death skill, 83 'S' on appearing
    int aiChance = 0;            // col 66 (AI_AttackChance): a monster's weight for it (0: never)
    bool targetRequired = false; // col 22
    bool targetBuilding = false; // col 25: may be used on a fortress's structures (TargetType_Building)
    // Whom it may be cast on (cols 26..33, as the original's _RefSkill names them: TargetGroup_Self,
    // _Ally, _Party, _Enemy_M, _Enemy_P, _Neutral, _DontCare, TargetEtc_SelectDeadBody).
    bool targetSelf = false, targetAlly = false, targetParty = false, targetEnemyMonster = false, targetEnemyPlayer = false,
         targetNeutral = false, targetAny = false, targetDeadBody = false;
    std::array<std::uint32_t, 2> masteries{}; // cols 34, 35
    std::array<int, 2> masteryLevels{};       // cols 36, 37
    std::array<std::uint32_t, 3> prerequisiteGroups{}; // cols 40..42
    std::array<int, 3> prerequisiteLevels{};           // cols 43..45
    int requiredStr = 0, requiredInt = 0;        // cols 38, 39
    int spCost = 0;              // col 46 (0 = not trainable)
    int race = 0;                // col 47: 0 China, 1 Europe, 3 any
    std::array<int, 2> weaponKinds{255, 255};   // cols 50, 51 (255 = none)
    int hpCost = 0, mpCost = 0;                 // cols 52, 53: consumed HP / MP
    int hpCostRatio = 0, mpCostRatio = 0;       // cols 54, 55 (percent)
    int boardRow = 255;          // col 59 (255 = not on the board)
    int boardColumn = 255;       // col 60 (0..7)
    std::string iconPath;        // col 61, relative to the icon folder
    std::string nameKey;         // col 62
    std::string descKey;         // col 64: the tooltip's description
    std::string studyKey;        // col 65: the practice box's explanation
    // The parameter tail (cols 69..) carries the 'efta' tag: an active buff of this skill
    // is sent with a status byte (character data, entity spawns).
    bool buffStatus = false;
    // More tags of the tail (each a four-letter code as a decimal number, its arguments after it):
    std::uint32_t buffDurationMs = 0; // 'dura': how long its buff lasts (4-week ones overflow to negative: read as u32)
    bool hasDuration = false;         // 'dura' present (0 then means no buff icon)
    bool buffRider = false;           // 'getv' RPBU / STDU / DTDR: the buff add carries extra ms
    bool buffSecondary = false;       // 'bbuf': shown in the board's second group
    bool cancelBlocked = false;       // 'nbuf' without 'lnks': right-click does not end it
    bool cancelConfirm = false;       // 'cbuf': ending it asks first
    bool cancelWithToken = false;     // 'lnks': the cancel request names the instance
    bool buffIndefinite = false;      // 'efr' with first argument 3: no timer
    // 'att' (three letters): the skill hits. Its arguments: the kind (5 melee, 6 shot, 8 an imbue: a
    // buff whose force joins each hit while it lasts, 9 a monster's ranged, 10 magic), the power in percent of the attack, a flat attack added (monsters' skills
    // carry their whole attack here: min, max), then a number not used yet. 'mc': hits per use (its
    // second argument; the basic attacks' swings that strike twice).
    bool attack = false;
    int attackKind = 0, attackPower = 100, attackMin = 0, attackMax = 0;
    int hits = 1;
    bool consumesAmmo = false; // 'cnsm': each use takes an arrow or a bolt (bows, crossbows)
    int speedPercent = 0;             // 'hste': the moving speed it adds (percent)
    bool inService = false;      // col 0
    // The parameter tail as it is (cols 69..117, to its last non-zero): tags, each a code of up to
    // four letters as a number, followed by its arguments. Read it with ForEachSkillParam.
    std::vector<std::uint32_t> params;
};
bool DecodeSkillRows(std::string_view utf8Text, std::vector<SkillRow>& out, std::string& error);

// The 1.188 client's encrypted text tables: skilldata_<N>enc.txt, the shards textdata\skilldataenc.txt lists (the client
// reads those, not the older plain skilldata_<N>.txt, which lack 2,066 skills in service: the socket stones' among them).
// All but their last 22 bytes are a byte-wise additive stream ; those 22 are UTF-16LE "\r\n//encrypt". True when `bytes` ended so, decrypted in place
// then (UTF-16LE text with its BOM); false (untouched) for a file without the marker.
bool DecryptTextTable(std::vector<std::byte>& bytes);
// The shards a list file names (skilldataenc.txt: one file name a line), lower-cased, under textdata.
std::vector<std::string> ListedShards(const FileSystem& fs, std::string_view listFile);

// A parameter tag's code from its letters ("att" -> 0x617474).
constexpr std::uint32_t SkillTag(std::string_view letters) {
    std::uint32_t v = 0;
    for (const char c : letters) v = v << 8 | static_cast<unsigned char>(c);
    return v;
}
// How many places of the tail a tag takes, itself included, as the 1.188 client's parser steps; 0 for a code it does
// not know (it never meets one in the game's data).
int SkillParamLength(std::uint32_t code);
// Each tag of a row's tail, in order, with its arguments; the walk ends where the parser's does
// ('ssou', an unknown code, the tail's end).
template <typename Fn> void ForEachSkillParam(const SkillRow& row, Fn&& fn) {
    const std::vector<std::uint32_t>& p = row.params;
    for (std::size_t i = 0; i < p.size();) {
        if (p[i] == 0) {
            ++i;
            continue;
        }
        const int length = SkillParamLength(p[i]);
        if (length <= 0) return;
        // 'ssou' (a monster's summon) ends the walk: its groups of four run to the row's end.
        if (p[i] == SkillTag("ssou")) {
            fn(p[i], std::span<const std::uint32_t>(p.data() + i + 1, p.size() - i - 1));
            return;
        }
        const std::size_t end = std::min(p.size(), i + static_cast<std::size_t>(length));
        fn(p[i], std::span<const std::uint32_t>(p.data() + i + 1, end - i - 1));
        i += static_cast<std::size_t>(length);
    }
}
// The arguments of a row's first tag `code` (for 'efr' the first whose kind, its first argument, is
// `efrKind`); empty when the row has none. A missing argument at the tail's end reads as 0.
struct SkillParam {
    std::array<std::uint32_t, 6> args{};
    bool present = false;
    explicit operator bool() const { return present; }
    std::uint32_t operator[](std::size_t i) const { return i < args.size() ? args[i] : 0u; }
    int Int(std::size_t i) const { return static_cast<int>((*this)[i]); }
};
SkillParam FindSkillParam(const SkillRow& row, std::uint32_t code, std::uint32_t efrKind = 0);

// skillmasterydata.txt
struct MasteryRow {
    std::uint32_t id = 0;   // col 0
    std::string nameKey;    // col 2
    std::string descKey;    // col 4
    std::string tabKey;     // col 5
    int tab = 255;          // col 6 (255 = hidden)
    int kind = 0;           // col 7: 0 weapon, 1 attribute
    std::string iconPath;   // col 11, relative to the icon folder
    std::string focusIconPath; // col 12
};
bool DecodeMasteryRows(std::string_view utf8Text, std::vector<MasteryRow>& out, std::string& error);

// skillgroup.txt
struct SkillGroupRow {
    std::uint32_t id = 0;      // col 1
    std::uint32_t mastery = 0; // col 2
    int boardRow = 0;          // col 4
    std::string nameKey;       // col 5
    std::string iconPath;      // col 6, relative to the icon folder
};
bool DecodeSkillGroupRows(std::string_view utf8Text, std::vector<SkillGroupRow>& out, std::string& error);

// Skills, masteries and groups (loaded on first use).
class SkillTable {
public:
    bool Load(const FileSystem& fs, std::string& error);
    const SkillRow* Find(std::uint32_t id) const;
    const SkillRow* Find(std::string_view codename) const;
    const MasteryRow* Mastery(std::uint32_t id) const;
    const SkillGroupRow* Group(std::uint32_t id) const;
    // The skill group shown on a mastery's board row (its name and icon), or nullptr.
    const SkillGroupRow* BoardRowGroup(std::uint32_t mastery, int boardRow) const;
    // The row of `group` at `level` (nullptr when there is none): the one learned and cast, the head of its chain.
    const SkillRow* GroupLevel(std::uint32_t group, int level) const;
    // The head of the chain `row` belongs to (its group's row at its level), else `row` itself: a sub-row kept
    // from before the chain heads were found (a quick slot, a learned row) casts and learns as its head.
    const SkillRow& Head(const SkillRow& row) const {
        const SkillRow* head = GroupLevel(row.group, row.level);
        return head != nullptr && head != &row && row.spCost == 0 && head->spCost > 0 ? *head : row;
    }
    // The skills on a mastery's board: one representative row per group (level 1),
    // by board row and column.
    std::vector<const SkillRow*> Board(std::uint32_t mastery) const;
    const std::vector<MasteryRow>& masteries() const { return masteries_; }
    std::size_t size() const { return skills_.size(); }
    const std::unordered_map<std::uint32_t, SkillRow>& rows() const { return skills_; }
    // Tests: a row of their own.
    void Add(SkillRow row) {
        byGroupLevel_[static_cast<std::uint64_t>(row.group) << 8 | static_cast<std::uint8_t>(row.level)] = row.id;
        byCode_[row.codename] = row.id;
        skills_[row.id] = std::move(row);
    }

private:
    std::unordered_map<std::uint32_t, SkillRow> skills_;
    std::unordered_map<std::uint64_t, std::uint32_t> byGroupLevel_; // group << 8 | level -> id
    std::unordered_map<std::string, std::uint32_t> byCode_;
    std::vector<MasteryRow> masteries_;
    std::vector<SkillGroupRow> groups_; // every board row (col 1 is not unique)
    mutable std::unordered_map<std::uint32_t, std::vector<const SkillRow*>> boards_; // Board() results
};

} // namespace sr::assets
