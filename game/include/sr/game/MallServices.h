#pragma once

// The item mall's services both sides share: equipment wear and its repair (the
// NPC's price, the hammer), the equipment gender switch, the skin change's checks, skill and mastery withdrawal with
// the skill restoration potion, the magic cubes. The original's rules and codes. Headless.

#include <array>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include "sr/assets/GameData.h"
#include "sr/game/Items.h"
#include "sr/game/Skills.h"

namespace sr::game {

// --- Wear and repair -------------------------------------------------------------

// A point lost on a roll of 2 %: the weapon for each hit that landed, the first of these places that rolls for each hit
// taken, the shield for each blocked hit.
constexpr int kWearPercent = 2;
constexpr std::array<int, 7> kArmourWearPlaces = {slot::kHead, slot::kChest, slot::kShoulders, slot::kHands, slot::kLegs, slot::kFeet, slot::kShield};
enum class Wear : std::uint8_t { Weapon, Armour, Shield };
// The place a use's wear takes a point from, or -1: the chance min(100, 2 % x `count`, the use's hits); the weapon or
// the shield one roll; the armour's places rolled in
// turn, the first that rolls losing the point, nothing when it holds nothing that wears (`wears(place)`).
int RollWear(Wear kind, const std::function<bool(int)>& wears, int count, std::mt19937& random);

// Equipment that wears and mends: a weapon, a shield or armour with a durability (accessories have none).
bool HasDurability(const assets::ItemRow& row);
// The most durability of this item (its white stats' and blues'; ComputeStats).
std::uint32_t MostDurability(const assets::ItemRow& row, const ItemInstance& item, const std::vector<assets::MagicOptionRow>* options);

// The NPC's repair price: a point costs CostRepair / the most, at least 1;
// the points are the missing ones cut to what `gold` buys and to 100,000; the price at most 100,000,000.
struct RepairQuote {
    std::uint32_t points = 0;
    std::uint64_t gold = 0;
};
constexpr std::uint32_t kRepairMostPoints = 100000;
constexpr std::uint64_t kRepairMostGold = 100000000;
std::uint64_t RepairPointCost(const assets::ItemRow& row, std::uint32_t most);
RepairQuote QuoteRepair(const assets::ItemRow& row, std::uint32_t durability, std::uint32_t most, std::uint64_t gold);

// The codes of the NPC's repair (its 0xB03E) and the hammer.
namespace repaircode {
constexpr std::uint16_t kNoNpc = 5;            // the NPC has no repair
constexpr std::uint16_t kNoItem = 3;
constexpr std::uint16_t kNoGold = 0x1C07;
constexpr std::uint16_t kNothingToMend = 0x1C09;
constexpr std::uint16_t kNotRepairable = 0x1C11;
constexpr std::uint16_t kCannotRevive = 0x1C12;
constexpr std::uint16_t kHammerNothing = 0x1888; // "No item needs repairing."
} // namespace repaircode

// The NPCs that repair (NPC function 4: the smiths and the armour merchants of every town).
bool IsRepairNpc(std::string_view codename);
// The smiths that grant an avatar's magic option (NPC function 0x20: those whose codename holds NPC_CH_SMITH,
// NPC_WC_SMITH, NPC_KT_SMITH, NPC_EU_SMITH, NPC_CA_SMITH, NPC_SD_M_AREA_SMITH, NPC_SD_T_AREA_SMITH; the armour sellers
// do not).
bool IsAvatarGrantNpc(std::string_view codename);

// --- The equipment gender switch (4.2) ------------------------------------------------------------------------------

// The tool (TID 3/3/13/8) on the item in `place` of the bag: 0, or the original's code (0x1882 not a bag place, 0x1884
// not armour, 0x1883 its degree over the tool's Param1, 2 no counterpart).
std::uint16_t GenderSwitchRefusal(const assets::ItemRow& tool, int place, int bagSize, const assets::ItemRow* target, const assets::ItemRow* counterpart);
bool IsGenderSwitchTool(const assets::ItemRow& row);
bool IsRepairHammer(const assets::ItemRow& row);
bool IsSkinChangeScroll(const assets::ItemRow& row);

// --- Skill and mastery withdrawal (4.4) -----------------------------------------------------------------------------

// The potions that withdraw: the mall's and the old woman's (mode 6: all of
// the SP back, no gold), the quest's (mode 3: 80 %, gold by the level).
enum class WithdrawMode : std::uint8_t { None = 0, Quest = 3, Mall = 6 };
WithdrawMode WithdrawModeOf(const assets::ItemRow& potion);
namespace withdrawcode {
constexpr std::uint16_t kNotLearned = 0x7801, kLevelTooLow = 0x7802, kNoGold = 0x7804, kNoPotion = 0x7805, kRequired = 0x7806, kFailed = 0x7803;
} // namespace withdrawcode
struct Withdrawal {
    std::uint16_t code = 0;
    int levels = 0;           // the levels withdrawn (the potions it takes)
    std::uint32_t skillPoints = 0;
    std::uint64_t gold = 0;
    std::uint32_t skillNow = 0; // a skill: the row it keeps (0: none left of its group)
};
// A skill's group down to `target` (0: unlearned), or a mastery's: the checks in the original's order, the SP back
// (the removed levels' learning SP; a mastery's crossed levels' costs) and the gold (mode 3: GoldMin by the highest
// level x 10 x (10 + the mastery's level) a level; inferred).
Withdrawal WithdrawSkill(const SkillBook& book, const assets::SkillTable& table, std::uint32_t skill, int target, WithdrawMode mode, std::uint32_t goldMin);
Withdrawal WithdrawMastery(const SkillBook& book, const assets::SkillTable& table, const std::vector<std::uint32_t>& masteryCosts, std::uint32_t mastery,
                           int target, WithdrawMode mode, std::uint32_t goldMin);

// --- Magic cubes (5.4) ----------------------------------------------------------------------------------------------

// A cube (TID 3/2/3/1): Param9 its places, Param10 the elixirs' kind it keeps (1 weapon, 2 armour, 4 accessory, 8 shield).
bool IsMagicCube(const assets::ItemRow& row);
int CubePlaces(const assets::ItemRow& cube);
// Whether an item goes into the cube: a 3/3 item whose Param10 is the cube's (the B elixirs).
bool CubeTakes(const assets::ItemRow& cube, const assets::ItemRow& item);
namespace cubecode {
constexpr std::uint16_t kFull = 0x188A, kNoStore = 0x188B, kSellRefused = 0x188E, kBagFull = 0x1807, kNotTaken = 0x1806;
} // namespace cubecode

// --- The skin change (4.3) ------------------------------------------------------------------------------------------
namespace skincode {
constexpr std::uint16_t kWrongModel = 2, kUndress = 0x1892;
} // namespace skincode

} // namespace sr::game
