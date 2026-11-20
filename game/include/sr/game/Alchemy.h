#pragma once

// Alchemy's rules: the vSRO 1.188 game server's strengthening with elixirs and lucky powder,
// magic and attribute stones, making a stone from a tablet and its elements, and breaking materials and equipment
// down with the Rondos. The server rolls them; the client uses the checks for its window's slots. Headless.
//
// The die is the original's rand() % n (Below). Every function that changes an item changes only what it is given;
// what is used up (the elixir, the powder, a stone, the Rondos) is the caller's to take.

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sr/game/Items.h"

namespace sr::assets {
struct ItemRow;
struct MagicOptionRow;
struct MagicOptGroupRow;
struct MagicOptionAssignRow;
class ItemTable;
} // namespace sr::assets

namespace sr::game::alchemy {

using Options = std::vector<assets::MagicOptionRow>;

// What an item is to alchemy, by its.
enum class Kind : std::uint8_t {
    None,
    Equipment,      // a weapon, shield, armour or accessory (not a job suit, an avatar or a Devil's Spirit)
    Elixir,         // 3/3/10/1
    LuckyPowder,    // 3/3/10/2
    AdvancedElixir, // 3/3/10/4
    MagicStone,     // 3/3/11/1, and the protection stones 3/3/11/7
    AttrStone,      // 3/3/11/2
    Tablet,         // 3/3/11/3
    ByProduct,      // 3/3/11/4: a monster's material
    Element,        // 3/3/11/5
    Rondo,          // 3/3/11/6: 01 the Void Rondo (disjoint), 02 the Destroyer Rondo (dismantle)
};
Kind KindOf(const assets::ItemRow& row);
// The degree: equipment's (ItemClass - 1) / 3 + 1, anything else its ItemClass.
int Degree(const assets::ItemRow& row);
// The original's rand() % n, for n > 0.
int Below(std::mt19937& random, int n);

// The server's codes (the client's texts).
namespace code {
constexpr std::uint16_t kTargetMissing = 0x5406, kRecipeMissing = 0x5407, kOnlyEquipment = 0x5408, kRecipeMismatch = 0x5409,
                        kPowderDegree = 0x540A, kCannotReinforce = 0x540F, kNotMaterial = 0x5410, kTwice = 0x5412, kNothing = 0x5413,
                        kBadSlot = 0x5414, kStoneMismatch = 0x5421, kChargesFull = 0x5422, kFailed = 0x5423, kAstral = 0x5424,
                        kHonor = 0x5425, kAlreadyDone = 0x5426, kRecipeLevel = 0x5427;
constexpr std::uint16_t kMakeFailed = 0x5806, kNotEnough = 0x5807, kNoRoom = 0x5808, kElementsShort = 0x5809, kNoTarget = 0x5810,
                        kCannotTarget = 0x5811, kTwoPieces = 0x5812, kRepairInvalid = 0x5813, kHonorRondo = 0x5815, kAvatar = 0x5816,
                        kDevilSpirit = 0x5817;
// The sockets' (3 is the original's own "no item" before them).
constexpr std::uint16_t kSocketNoItem = 3, kSocketSlot = 0x6C06, kSocketNoMaterial = 0x6C08, kSocketsFull = 0x6C09, kSocketNotAllowed = 0x6C0A,
                        kSocketMade = 0x6C0B, kSocketSameStone = 0x6C0C, kSocketMissing = 0x6C0D, kSocketStoneKind = 0x6C0E,
                        kSocketInUse = 0x6C0F, kSocketDegree = 0x6C10, kSocketEmpty = 0x6C11, kSocketTwice = 0x6C12, kSocketNotStuff = 0x6C13;
// The smith's blessing of an avatar (0x34AA {2, code}). The 1.188 client's own lines:
// 6 kGrantMax "Additional magic options cannot be granted.", 7 kGrantFailed (never sent: the roll is 100 %), 8
// kGrantNotAvatar "... only possible on avatar items.", 0xA kGrantWrong "Incorrect magic option."; the others its
// default line.
constexpr std::uint16_t kGrantFull = 2, kGrantNoItem = 3, kGrantMax = 6, kGrantFailed = 7, kGrantNotAvatar = 8, kGrantPlace = 9, kGrantWrong = 0xA,
                        kGrantDegree = 0xB;
} // namespace code

// --- Options' charges (Immortal, Steady, Astral, Luck, APE: a value of n is n charges) -----------------------------------

// The value of the item's option of that name's code (MATTR_LUCK: 'luck', so MATTR_LUCK_SET's too, as the original
// finds a charge by code), 0 when it has none.
std::uint32_t OptionValue(const ItemInstance& item, const Options& options, std::string_view name);
// Uses one charge of it (the last removes the option); false when it has none.
bool UseCharge(ItemInstance& item, const Options& options, std::string_view name);
// How many options the item may hold: min(12, its row's MaxMagicOptCount + its
// MATTR_DEC_MAXDUR and MATTR_NOT_REPARABLE options).
int OptionCapacity(const assets::ItemRow& row, const ItemInstance& item, const Options& options);

// --- Reinforcement ----------------------------------------------------------------------------

// An elixir's or a powder's chance byte for the current plus: Param2 / 3 / 4 by plus / 4, the byte plus % 4 from the
// high one (from +12 the last).
int ChanceByte(const assets::ItemRow& row, int plus);
// Whether the elixir fits the item: the item's TypeID3 among the bytes of the elixir's Param1 and Param5.
bool ElixirFits(const assets::ItemRow& elixir, const assets::ItemRow& target);
// The checks before the roll: 0, or the server's code (only equipment; the plus under 250; the elixir fits; the
// powder's degree is the item's).
std::uint16_t CheckReinforce(const assets::ItemRow& target, const ItemInstance& item, const assets::ItemRow& elixir,
                             const assets::ItemRow* powder);
// The chance, 10..100: the elixir's byte, the powder's while under 100, 5 for a Luck charge while under 100 (then
// `usesLuck`), the character's bonus (the premium buffs' `luck`).
struct Chance {
    int percent = 10;
    bool usesLuck = false;
};
Chance ReinforceChance(const assets::ItemRow& elixir, const assets::ItemRow* powder, int plus, bool hasLuck, int bonus = 0);

struct ReinforceResult {
    bool success = false;
    bool destroyed = false;       // a failure from +5 (not a cash item), no Immortal charge
    bool durabilityLost = false;  // the other half: MATTR_DEC_MAXDUR raised, no Steady charge
    bool astralKept = false;      // a failure from +4 or more kept at +4 by an Astral charge
    int chance = 0;
    int plusBefore = 0, plusAfter = 0;
};
// One reinforcement: rolls and changes `item` (its plus, its charges, the durability penalty). CheckReinforce first.
ReinforceResult Reinforce(const assets::ItemRow& target, ItemInstance& item, const assets::ItemRow& elixir, const assets::ItemRow* powder,
                          const Options& options, int bonus, std::mt19937& random);

// --- The advanced elixir (3.8) -------------------------------------------------------------------------------------------

// Its checks: equipment under +250 (its own plus), none yet (0x5426), the item's kind among the
// recipe's (0x5409), a SoX recipe (Rarity 2) on a SoX or rare set item and a normal one on the others (0x5409), the
// recipe's level (ItemClass) at least the item's degree (0x5427).
std::uint16_t CheckAdvanced(const assets::ItemRow& target, const ItemInstance& item, const assets::ItemRow& recipe);
// It never fails: the item's advanced plus is the recipe's Param8 (+1 for A, +2 for B).
void ApplyAdvanced(ItemInstance& item, const assets::ItemRow& recipe);
// The recipe a dismantle gives: the merit (rarity 1 normal, 8 SoX, 3 a set, 8 a rare
// set, times the tier (ItemClass - 1) % 3 + 1, + 2 / 4 / 6 by the total plus to +4 / +6 / above) 10..19 an A, 20 and
// more a B, of the piece's kind and rarity and one level above its degree; null for none.
const assets::ItemRow* DismantleRecipe(const assets::ItemTable& items, const assets::ItemRow& target, const ItemInstance& item);

// --- Stones (3.2, 3.3) ------------------------------------------------------------------------------------------------

// The white stats' fields of an item's kind, in their variance order (field i at bits 5i): weapon DUR, PASTR, MAINT,
// HR, PA, MA, CRITICAL; armour DUR, PDSTR, MDINT, PD, MD, ER; shield DUR, PDSTR, MDINT, BR, PD, MD; accessory PAR, MAR.
std::vector<std::string_view> WhiteFields(const assets::ItemRow& row);
// A white field's new value: the least of k rolls 0..5 (k 5 for a rare item, else 4) as 0, 7, 13, 19, 25, 31.
int RollWhite(const assets::ItemRow& row, std::mt19937& random);
// Whether an option (its level-1 row's AvailItemGroup) may be put on the item (weapon, shield, armor, accessory,
// helm, mail, pants, earring, necklace, ring).
bool OptionFits(const Options& options, std::string_view name, const assets::ItemRow& target);
// Whether one row of an option takes the item: one of its groups names the item's
// kind and the item's ItemClass is at least that group's ReqClass.
bool RowFits(const assets::MagicOptionRow& row, const assets::ItemRow& target);
// The values an option may take at a degree (its row at that MLevel: the non-zero u16 halves of Param2..4, the high
// half first); empty when it has no such row.
std::vector<std::uint32_t> OptionValues(const Options& options, std::string_view name, int degree);
const assets::MagicOptionRow* OptionRow(const Options& options, std::string_view name, int degree);

std::uint16_t CheckMagicStone(const assets::ItemRow& target, const ItemInstance& item, const assets::ItemRow& stone, const Options& options);
std::uint16_t CheckAttrStone(const assets::ItemRow& target, const assets::ItemRow& stone);

struct StoneResult {
    bool success = false;
    int chance = 0;
    std::uint16_t code = 0;     // a refusal after the roll (the charges full), else 0
    std::string option;         // the stone's option (MATTR_* / NATTR_*)
    std::uint32_t value = 0;    // its new value (a magic option) or the field's (an attribute)
    bool added = false;         // a magic option the item did not have
    bool assimilated = false;   // the stone's assimilation changed something else
    bool apeUsed = false;       // ... or an APE charge stopped it
    int whiteField = -1;        // the white field assimilation set (-1: none)
    int optionIndex = -1;       // the option assimilation set (-1: none)
};
// A magic stone (3.2; `stoneItem.plus` is its assimilation rate): the roll rand() % 100 <= clamp(Param4 + bonus, 5,
// 100); then a charge + 1 (at most 6, Repair 7) or a new value from the option's list at the stone's degree.
// CheckMagicStone first.
StoneResult ApplyMagicStone(const assets::ItemRow& target, ItemInstance& item, const assets::ItemRow& stone, const ItemInstance& stoneItem,
                            const Options& options, int bonus, std::mt19937& random);
// An attribute stone (3.3): the same roll; its field set anew by RollWhite. CheckAttrStone first.
StoneResult ApplyAttrStone(const assets::ItemRow& target, ItemInstance& item, const assets::ItemRow& stone, const ItemInstance& stoneItem,
                           const Options& options, int bonus, std::mt19937& random);

// --- Making a stone (3.4) -----------------------------------------------------------------------------------------------

struct Recipe {
    const assets::ItemRow* stone = nullptr;                                // the tablet's Desc5
    std::vector<std::pair<const assets::ItemRow*, int>> elements;          // its Desc1..4 with Param1..4
};
// A tablet's recipe; false when the tablet names an item the table does not have.
bool TabletRecipe(const assets::ItemTable& items, const assets::ItemRow& tablet, Recipe& out);
// A new stone's assimilation rate (its OptLevel): made from a tablet one of its Param2's bytes {10, 20, 30} for a magic
// or an attribute stone (3/3/11/1, 3/3/11/2; anything else made, a protection stone, Sabakun's jewel, 0);
// dismantled, one of Param2's and Param3's {10, 20, 30, 5, 35}.
int StoneRate(const assets::ItemRow& stone, bool dismantled, std::mt19937& random);

// --- Breaking down (3.5) ------------------------------------------------------------------------------------------------

// Void Rondos a monster material's stack needs: (Price / 1000 + 1) x its count.
int VoidRondos(const assets::ItemRow& material, int count);
// What it gives: its Param_i x count of each of its Desc_i elements.
std::vector<std::pair<const assets::ItemRow*, int>> DisjointYield(const assets::ItemTable& items, const assets::ItemRow& material, int count);
// Destroyer Rondos a piece of equipment needs: Price / 20000 + 1.
int DestroyerRondos(const assets::ItemRow& equipment);
// The checks of a dismantle: 0, or the code (not equipment, an avatar, a Devil's Spirit, "repair invalid", an honor item).
std::uint16_t CheckDismantle(const assets::ItemRow& target, const ItemInstance& item, const Options& options);

// The stones of one degree a dismantle picks among, by weight (the server's `_RefDropItemAssign` buckets).
struct StoneBucket {
    std::vector<std::pair<const assets::ItemRow*, double>> magic, attr;
};
struct DismantleYield {
    std::vector<std::pair<const assets::ItemRow*, int>> elements;
    std::vector<std::pair<const assets::ItemRow*, int>> stones; // each with its assimilation rate
    const assets::ItemRow* recipe = nullptr;                    // an advanced elixir (DismantleRecipe)
};
// Elements from two pairs of different kinds, attribute and magic stones up to two each.
DismantleYield Dismantle(const assets::ItemTable& items, const assets::ItemRow& target, const ItemInstance& item, const StoneBucket& bucket,
                         std::mt19937& random);

// --- Sockets (6) ---------------------------------------------------------------------------------------------------------

// The socket stones (3/3/11/8: Param1 their skill group's LinkID, Param2 1 a weapon's, 2 a chest's, ItemClass their
// grade), the maker (3/3/11/10, ITEM_ETC_ARCHEMY_ETC_02) and the eraser (3/3/11/9, ITEM_ETC_ARCHEMY_RONDO_03).
bool IsSocketStone(const assets::ItemRow& row);
bool IsSocketMaker(const assets::ItemRow& row);
bool IsSocketEraser(const assets::ItemRow& row);
// How many sockets an item may have: its Param20, 0..3 (173 pieces of degree 11 in 1.188).
int SocketCount(const assets::ItemRow& row);
// The made ones.
int MadeSockets(const ItemInstance& item);
// The checks of making socket `index` (the original's order after the slot and the item): equipment and not a job suit
// (0x5408), a Devil's Spirit only with its Param2 1 (0x540F), socket alchemy's
// equipment (0x6C13), sockets allowed (0x6C0A), fewer made than allowed (0x6C09), that one not made (0x6C0B). The maker
// in the bag is the caller's (0x6C08).
std::uint16_t CheckSocketMake(const assets::ItemRow& target, const ItemInstance& item, int index);
// The checks of putting `stone` into socket `index` (at the completion): equipment
// (0x5408), a socket stone (0x5407), the socket made (0x6C0D) and empty (0x6C0F), no other socket holding the same
// stone (0x6C0C), the item's degree at least the stone's ItemClass (0x6C10), the stone's kind: Param2 2 armour of
// the chest (TID3 1, 2, 3, 9, 10, 11 with TID4 3), 1 a weapon as the original writes it (TID3 6, or TID4 3..15: its
// slip lets a chest pass too) (0x6C0E).
std::uint16_t CheckSocketInsert(const assets::ItemRow& target, const ItemInstance& item, const assets::ItemRow& stone, int index);
// A stone ours does not let in (refused as a stone the item does not take, 0x6C0E): the DMRETURN stone, whose 'bldr'
// never acts in the original (its blocked hit leaves before it; the user's call 2026-11-12).
// Items already holding one keep it (it does nothing).
bool SocketStoneDisabled(const assets::ItemRow& stone);
// The checks of erasing the stone of socket `index`: equipment (0x5408), the socket
// holding a stone (0x6C11). The Rondo in the bag is the caller's (0x6C08).
std::uint16_t CheckSocketErase(const assets::ItemRow& target, const ItemInstance& item, int index);
// A stone's skills by level: the rows of its group (`_RefMagicOptGroup` LinkID Param1, MagicType 3), by level.
std::vector<const assets::MagicOptGroupRow*> StoneSkills(const std::vector<assets::MagicOptGroupRow>& groups, const assets::ItemRow& stone);
// The level an insertion gives: the lowest of three uniform picks among the group's rows
// (three levels: 70.4 / 25.9 / 3.7 %; five: 48.8 / 29.6 / 15.2 / 5.6 / 0.8 %); null when it has none.
const assets::MagicOptGroupRow* RollSocketLevel(const std::vector<const assets::MagicOptGroupRow*>& rows, std::mt19937& random);
// The sockets' changes (the binding rows: an empty one's nSlot alone; a filled one its skill, level and stone).
void MakeSocket(ItemInstance& item, int index);
void InsertStone(ItemInstance& item, int index, std::uint32_t skill, int level, std::uint32_t stone);
void EraseStone(ItemInstance& item, int index);
// The skills an item's filled sockets give its wearer, added to `out`.
void AddSocketSkills(const ItemInstance& item, std::vector<std::uint32_t>& out);

// --- The smith's blessing of an avatar ------------------------------------------------
// The town smiths' "Grant magic option on avatar item": one option from the avatar's list put on an avatar in the bag,
// free and
// always (a 100 % roll), until it holds as many as it may.

// The value kinds' range codes: the avatars' ('stra', 'inta', 'strv', 'intv', 'hra', 'era',
// 'hpa', 'mpa', 'luca', 'drua', 'dara', 'mdia', 'hprg', 'mprg'), the Devil's Spirit's ('hpna', 'mpna'), 'dura', 'reis'.
bool IsRangeCode(std::uint32_t code);
// A range row's value: trunc(Param3 + float(rand / 32767) x (Param4 - Param3)); Param3 when they are equal.
std::uint32_t RangeValue(const assets::MagicOptionRow& row, std::mt19937& random);
// An avatar the smith takes: 3/1/13 (a Devil's Spirit, 3/1/14, is not one: 8). Ours leaves out the flag (13/4) as the
// 1.188 window does, which the original's server would take.
bool IsGrantAvatar(const assets::ItemRow& row);
// The options the smith offers for an avatar: magicoptionassign.txt's row of its TID3 / TID4 (Race its Country or 3), in
// order, as the 1.188 window lists them; empty for anything else.
std::vector<std::string> AvatarGrantList(const std::vector<assets::MagicOptionAssignRow>& assign, const assets::ItemRow& avatar);
// BlessItem's checks in order: an avatar (8), its options above what it may hold (6), the option in its list
// (0xA; case ignored), a row of it at the avatar's degree (0xB), a new one while it holds all it may (2); 0
// when it may be granted. Ours takes the avatar's list where the original takes the option's groups, whose
// entry for armour (TID3 2) takes every avatar too: a hand-made request could put STR, Luck or Immortal on one; the
// stock client offers only the list, so the two agree on everything it sends.
std::uint16_t CheckAvatarGrant(const assets::ItemRow& row, const ItemInstance& item, std::string_view option, const Options& options,
                               const std::vector<assets::MagicOptionAssignRow>& assign);
// The grant (CheckAvatarGrant first): the option's row at the avatar's degree, its value (a range code's pick, then
// 1..1,700), written again where the avatar holds that row (by
// its ID, so STR beside STR_2) or added; the option written.
ItemInstance::Option GrantAvatarOption(const assets::ItemRow& row, ItemInstance& item, std::string_view option, const Options& options,
                                       std::mt19937& random);

} // namespace sr::game::alchemy
