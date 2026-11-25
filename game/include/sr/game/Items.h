#pragma once

// Items the player has: what the server keeps of one item (its row in
// itemdata, the plus, the 64-bit variance of its white stats, its durability, its magic options,
// a stack's count) and the stats the original's tooltip shows for it, computed as the original
// does. Headless; the server uses the same numbers.

#include <array>
#include <map>
#include <cstdint>
#include <string>
#include <vector>

namespace sr::assets {
struct ItemRow;
struct MagicOptionRow;
} // namespace sr::assets

namespace sr::game {

// One of an item's three sockets (`_BindingOptionWithItem` bOptType 1, nSlot its index):
// not made, made and empty, or holding a socket stone's skill at the level its insertion rolled.
struct ItemSocket {
    bool made = false;
    std::uint32_t skill = 0; // the stone's skill at its level (nOptID; 0: empty)
    std::uint8_t level = 0;  // nOptLvl
    std::uint32_t stone = 0; // the stone's RefObjID (nParam1)
    bool Filled() const { return made && skill != 0; }
    bool operator==(const ItemSocket&) const = default;
};
inline constexpr int kSockets = 3;
using ItemSockets = std::array<ItemSocket, kSockets>;

struct ItemInstance {
    std::int64_t serial = 0;      // the server's id for this one (0: not stored yet)
    std::uint32_t refId = 0;      // itemdata's RefObjID (0: none)
    std::uint8_t plus = 0;        // OptLevel
    std::uint64_t variance = 0;   // twelve 5-bit numbers, one per white stat
    std::uint32_t durability = 0; // equipment: what is left
    std::uint16_t count = 1;      // stackables: how many
    struct Option {
        std::uint32_t id = 0;    // magicoption.txt's id
        std::uint32_t value = 0;
        bool operator==(const Option&) const = default;
    };
    std::vector<Option> options; // at most 12
    std::int64_t rentUntil = 0;  // a rental's end (_RentItemInfo.PeriodEndTime, Unix seconds); 0: not rented
    bool bound = false;          // bound to its character (ours: the beta quests' rewards; never traded, dropped or put in a guild's storage)
    // An advanced elixir's plus (`_BindingOptionWithItem` type 2: its nOptValue, the recipe's Param8) and the recipe
    // (its nOptID); 0: none. Never in OptLevel: the stats, the name and the look go by TotalPlus.
    std::uint8_t advanced = 0;
    std::uint32_t advancedRecipe = 0;
    // Its sockets (none made on most items: only those whose row's Param20 allows them).
    ItemSockets sockets{};
    // Who made it (_Items.CreaterName): a specialty good's buyer's alias (who may sell it where).
    std::string creator;
    bool Empty() const { return refId == 0; }
    bool operator==(const ItemInstance&) const = default;
};

// The plus the item's stats, its name and its look go by: its own and an advanced elixir's. The reinforcement's chance
// and its failures read the item's own.
inline int TotalPlus(const ItemInstance& item) { return int(item.plus) + int(item.advanced); }

// The kinds the tooltip and the equipment tell apart, by the row's type ids (TID1..TID4).
enum class ItemKind : std::uint8_t { Other, Weapon, Shield, Armour, Accessory, JobSuit, Avatar, Expendable, Pet };
ItemKind KindOf(const assets::ItemRow& row);
// What _items.data holds for an item: a count (anything but equipment), else its durability; the
// monster mask (ITEM_ETC_TRANS_MONSTER, TID 3/2/2, the only one) keeps its monster there, held in its durability.
// A pet's summoning item (TID 3/2/1) keeps its pet there too: _CharCOS.ID (0: never summoned;
// the original's _AddNewCOS writes it), held in its durability.
bool DataIsCount(const assets::ItemRow& row);
bool IsMonsterMask(const assets::ItemRow& row);
bool IsCosSummoner(const assets::ItemRow& row);

// One white stat as shown: its value (low ~ high for a range), the "(+X%)", whether the item has it.
struct ItemStat {
    bool shown = false;
    double low = 0, high = 0;
    int percent = 0;
};

struct ItemStats {
    ItemStat phyAttack, magAttack;           // weapons: min ~ max
    ItemStat phyDefence, magDefence;         // armour, shields
    ItemStat phyAbsorption, magAbsorption;   // accessories
    ItemStat durability;                     // low: what is left, high: the most
    ItemStat attackRating, parry, critical, blocking;
    ItemStat phyReinforce, magReinforce;     // percent values (weapons: min ~ max)
    double range = 0;                        // weapons: metres
    int degree = 0;                          // (ItemClass - 1) / 3 + 1; 0 none
    int seal = -1;                           // rare items: 0 Star, 1 Moon, 2 Sun; -1 none
};

// The variance's slot i (0..11): its 5-bit number, 0..31.
inline int VarianceSlot(std::uint64_t variance, int i) { return int((variance >> (5 * i)) & 31); }

// `options` (magicoption.txt) for the blues that change durability, attack rating and parry; may
// be null. `setPlus`: a worn set piece's raise of its total plus (game::SetWear::RaiseOf;
// the server's own, never stored nor sent). `server`: the numbers as the game server keeps them: every white a
// float32, low + float32((high -
// low) x float32(slot / 31) + float32(per plus x the total plus)), the hit rate and parry blues x (1 + v / 100) as floats,
// the most durability truncated; else as the 1.188 client's tooltip shows them (rounded).
ItemStats ComputeStats(const assets::ItemRow& row, const ItemInstance& item, const std::vector<assets::MagicOptionRow>* options,
                       int setPlus = 0, bool server = false);

// --- Wearing items --------------------------------------------

// The inventory's places: 0..12 what the character wears (the original's numbers), 13.. the bag.
namespace slot {
constexpr int kHead = 0, kChest = 1, kShoulders = 2, kHands = 3, kLegs = 4, kFeet = 5, kWeapon = 6, kShield = 7, kJobSuit = 8,
              kEarring = 9, kNecklace = 10, kRingLeft = 11, kRingRight = 12, kWorn = 13;
} // namespace slot

// Who would wear it: the character's level, country (0 China, 1 Europe) and sex, and its masteries' levels (skillmasterydata
// id -> level) for the European armour's mastery pairs; `masteriesKnown` false (the client's view before the server's
// list): that class of requirements is not checked.
struct Wearer {
    int level = 1;
    int country = 0;
    bool male = true;
    std::map<std::uint32_t, int> masteries;
    bool masteriesKnown = false;
};

// Mastery: the requirement pairs of the European masteries' class unmet (the original's 0x186C, as the level's).
enum class EquipCheck : std::uint8_t { Ok, NotWearable, WrongSlot, Level, Race, Sex, Broken, Mastery };

// Whether the item may be worn in that place (0..12), in the original's order (the first that fails answers): the
// place within the worn ones, the sex (col 58: 2
// either), not broken, the requirements (RequirementsMet), the country (col 14: 3 any), then the place its type selects
// (armour by its part, the weapon 6, a shield or ammunition 7, the job suit 8, earring 9, necklace 10, a ring 11 or 12).
// The original's ReqStr and ReqInt (0x1830, 0x1831, between the requirements and the country) are 0 in every 1.188
// equipment row (measured): not read.
EquipCheck CanEquip(const assets::ItemRow& row, int place, const Wearer& who, const ItemInstance& item);
// The row's four requirement pairs (cols 32..39) by class: type 1
// the level, 0x201..0x206 a European mastery's level; within a class one pair met is enough, every class named must be
// met (Level, then Mastery); -1 none (and 0: a row made by hand); 2..4 (the job suits' job levels) and 0x10 are not checked here; any other type
// is refused (WrongSlot: the original logs it as an unknown mastery).
EquipCheck RequirementsMet(const assets::ItemRow& row, const Wearer& who);
// Whether a worn item's level requirements (type 1) are met at `level`: one above it gives nothing while worn (a death
// that takes the character back under its weapon's level: the weapon as bare hands; the user's call 2026-11-13).
bool LevelFits(const assets::ItemRow& row, int level);

// --- Avatars ----------------------------------------------------------------

// The avatar places (_InventoryForAvatar's slots 0..4), one a kind: the hat, the dress, the attachment, the flag
// (avatars, TID 3/1/13/1..4) and the Devil's Spirit (TID 3/1/14), as the 1.188 equipment window's dress side shows
// them (picks the slot by the type: TID3 13 at TID4 - 1, TID3 14 at TID4 + 3).
namespace avatar {
constexpr int kHat = 0, kDress = 1, kAttachment = 2, kFlag = 3, kSpirit = 4, kPlaces = 5;
} // namespace avatar
// The avatar place an item is worn in; -1 for one that is no avatar.
int AvatarPlaceFor(const assets::ItemRow& row);
// Whether an avatar may be worn: the original's checks for any worn item but its place (the level, the country, the
// sex; an avatar passes any place).
EquipCheck CanWearAvatar(const assets::ItemRow& row, const Wearer& who);
// The place an item dropped on the equipment goes to (rings: the left if free, else the right if
// free, else the left); -1 for an item nobody wears.
int EquipPlaceFor(const assets::ItemRow& row, bool leftRingTaken, bool rightRingTaken);
// A two-handed weapon (col 93); ammunition (TID 3/3/4: arrows TID4 1, bolts 2), and the TID4 of the
// ammunition a weapon shoots: a bow (TID4 6) arrows, a crossbow (12) bolts, others none (0). (data:
// col 87 names it for bows only, so it goes by the weapon's type.)
bool TwoHanded(const assets::ItemRow& row);
bool IsAmmunition(const assets::ItemRow& row);
int AmmunitionFor(const assets::ItemRow& weapon);
// A new item's durability: the most for a variance of 0 (col 63).
std::uint32_t NewDurability(const assets::ItemRow& row);
// The most of one stack (col 57); ours larger for elixirs (100) and arrows and bolts (1,000), the server's and the
// client's alike.
int MaxStack(const assets::ItemRow& row);
// A grab pet's watch (3/3/13/15 with Param6's bit 2; its count is its minutes): a whole item, never split nor merged, sold
// whole (the original's).
bool IsWholeItem(const assets::ItemRow& row);
// May it be dropped by hand (CanDrop's bit 1, not when it cannot be traded and CanDrop is 1; never a pet's or a ride's
// scroll nor a magic cube: TID2 2 but 3/2/2), and does a death drop it (bit 2)?
bool CanDropByHand(const assets::ItemRow& row);
bool CanDropAtDeath(const assets::ItemRow& row);

// --- Repairing at an NPC ----------------------------------------

// Whether an NPC's repair takes the item: equipment (TID 3/1) but accessories (TID3 5, 12),
// without "repair invalid" (MATTR_NOT_REPARABLE) and, on an honor item, with a Repair charge left (MATTR_REPAIR's value 2
// or more; each repair takes one).
bool Repairable(const assets::ItemRow& row, const ItemInstance& item, const std::vector<assets::MagicOptionRow>* options);
// The price of putting an item back to its most durability `most`: a broken one (durability 0) counts from 1 and costs
// its CostRevive besides; each missing
// point max(1, CostRepair / most) gold. The server cuts the price toward 0 and stops at the points the gold pays for (at
// most 100,000) and at 100,000,000 gold; the client's box rounds and has no budget (`budget` 0). 0 when nothing is missing.
struct RepairPrice {
    std::uint32_t points = 0;  // the durability given back (a broken one's first point among them)
    std::uint64_t gold = 0;
    bool revive = false;       // broken: its revival's price is in `gold`
};
RepairPrice PriceRepair(const assets::ItemRow& row, std::uint32_t durability, std::uint32_t most, std::uint64_t budget, bool rounded);

// An item a shop never sells back once sold (the 1.188 client's list, made as its item table loads): TID 3/3 with TID3 8,
// every COS item (3/2), an avatar (3/1/13), a quest's or an event's item by its codename (ITEM_QNO, ITEM_QTUTORIAL,
// ITEM_QSP, ITEM_QCX, ITEM_ETC_E...) or name (SN_ITEM_EVENT..., SN_ITEM_ETC_TAIWAN_50000_GOLD). Its sale is asked first
// ("This item is not available for repurchase. Are you sure you want to sell the item?").
bool NoBuyBack(const assets::ItemRow& row);

// A magic option's place in its range, 0..100 (the tooltip's "(+X%)"): the row of its code at the
// item's degree gives the range. -1 when the option is unknown.
int OptionPercent(const std::vector<assets::MagicOptionRow>& options, const ItemInstance::Option& option, int degree);
const assets::MagicOptionRow* FindOption(const std::vector<assets::MagicOptionRow>& options, std::uint32_t id);

} // namespace sr::game
