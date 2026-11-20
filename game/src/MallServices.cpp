#include "sr/game/MallServices.h"

#include <algorithm>
#include <string_view>

namespace sr::game {

bool HasDurability(const assets::ItemRow& row) {
    const ItemKind kind = KindOf(row);
    return (kind == ItemKind::Weapon || kind == ItemKind::Shield || kind == ItemKind::Armour) && NewDurability(row) > 0;
}

std::uint32_t MostDurability(const assets::ItemRow& row, const ItemInstance& item, const std::vector<assets::MagicOptionRow>* options) {
    if (!HasDurability(row)) return 0;
    const ItemStats s = ComputeStats(row, item, options, 0, true); // the server's truncated most
    return std::uint32_t(std::max(0.0, s.durability.high));
}

int RollWear(Wear kind, const std::function<bool(int)>& wears, int count, std::mt19937& random) {
    std::uniform_int_distribution<int> percent(0, 99);
    const int chance = std::min(100, kWearPercent * std::max(0, count));
    if (kind != Wear::Armour) {
        const int at = kind == Wear::Weapon ? slot::kWeapon : slot::kShield;
        return wears(at) && percent(random) < chance ? at : -1;
    }
    for (const int at : kArmourWearPlaces) {
        if (percent(random) < chance) return wears(at) ? at : -1;
    }
    return -1;
}

std::uint64_t RepairPointCost(const assets::ItemRow& row, std::uint32_t most) {
    if (most == 0) return 1;
    return std::max<std::uint64_t>(1, std::uint64_t(row.costRepair) / most);
}

RepairQuote QuoteRepair(const assets::ItemRow& row, std::uint32_t durability, std::uint32_t most, std::uint64_t gold) {
    RepairQuote q;
    if (durability >= most) return q;
    const std::uint64_t point = RepairPointCost(row, most);
    const std::uint64_t affordable = gold / point;
    q.points = std::uint32_t(std::min<std::uint64_t>({std::uint64_t(most - durability), affordable, kRepairMostPoints}));
    q.gold = std::min<std::uint64_t>(std::uint64_t(q.points) * point, kRepairMostGold);
    return q;
}

bool IsRepairNpc(std::string_view codename) {
    static constexpr std::string_view kNames[] = {"NPC_CH_SMITH", "NPC_CH_ARMOR", "NPC_WC_SMITH", "NPC_WC_ARMOR", "NPC_KT_SMITH",
                                                  "NPC_KT_ARMOR", "NPC_EU_SMITH", "NPC_EU_ARMOR", "NPC_CA_SMITH", "NPC_CA_ARMOR",
                                                  "NPC_SD_M_AREA_SMITH", "NPC_SD_M_AREA_ARMOR", "NPC_SD_T_AREA_SMITH", "NPC_SD_T_AREA_ARMOR"};
    // The original looks each name up inside the codename (strstr); the fortresses' smiths are another NPC's.
    if (codename.find("FORTRESS") != std::string_view::npos) return false;
    for (std::string_view n : kNames) {
        if (codename.find(n) != std::string_view::npos) return true;
    }
    return false;
}

bool IsAvatarGrantNpc(std::string_view codename) {
    static constexpr std::string_view kNames[] = {"NPC_CH_SMITH", "NPC_WC_SMITH", "NPC_KT_SMITH", "NPC_EU_SMITH", "NPC_CA_SMITH",
                                                  "NPC_SD_M_AREA_SMITH", "NPC_SD_T_AREA_SMITH"};
    if (codename.find("FORTRESS") != std::string_view::npos) return false;
    for (std::string_view n : kNames) {
        if (codename.find(n) != std::string_view::npos) return true;
    }
    return false;
}

bool IsGenderSwitchTool(const assets::ItemRow& row) { return row.typeId == std::array<int, 4>{3, 3, 13, 8}; }
bool IsRepairHammer(const assets::ItemRow& row) { return row.typeId == std::array<int, 4>{3, 3, 13, 7}; }
bool IsSkinChangeScroll(const assets::ItemRow& row) { return row.typeId == std::array<int, 4>{3, 3, 13, 9}; }

std::uint16_t GenderSwitchRefusal(const assets::ItemRow& tool, int place, int bagSize, const assets::ItemRow* target, const assets::ItemRow* counterpart) {
    if (place < slot::kWorn || place >= bagSize) return 0x1882;
    if (target == nullptr || KindOf(*target) != ItemKind::Armour) return 0x1884;
    const int degree = target->itemClass > 0 ? (target->itemClass - 1) / 3 + 1 : 0;
    if (degree > tool.params[0]) return 0x1883;
    if (counterpart == nullptr || counterpart == target || counterpart->refObjId == target->refObjId) return 2;
    return 0;
}

WithdrawMode WithdrawModeOf(const assets::ItemRow& potion) {
    if (potion.codename == "ITEM_MALL_SKILL_RESTORATION_POTION" || potion.codename == "ITEM_QNO_RM_OLDWOMAN_2_02") return WithdrawMode::Mall;
    if (potion.codename == "ITEM_QSP_ALL_POTION_1_01") return WithdrawMode::Quest;
    return WithdrawMode::None;
}

namespace {
// Mode 3: 80 % of the SP back (basis - basis x 20 / 100).
std::uint32_t Refund(std::uint64_t basis, WithdrawMode mode) {
    if (mode == WithdrawMode::Quest) return std::uint32_t(basis - basis * 20 / 100);
    return std::uint32_t(basis);
}
} // namespace

Withdrawal WithdrawSkill(const SkillBook& book, const assets::SkillTable& table, std::uint32_t skillId, int target, WithdrawMode mode, std::uint32_t goldMin) {
    Withdrawal w;
    const assets::SkillRow* skill = table.Find(skillId);
    if (skill == nullptr || skill->group == 0 || book.GroupLevel(skill->group) <= 0) {
        w.code = withdrawcode::kNotLearned;
        return w;
    }
    const int level = book.GroupLevel(skill->group);
    if (target < 0 || target >= level) {
        w.code = withdrawcode::kLevelTooLow;
        return w;
    }
    // A learned skill that needs this group above the target (a prerequisite).
    for (const auto& [group, learned] : book.groups) {
        if (learned <= 0 || group == skill->group) continue;
        const assets::SkillRow* other = table.GroupLevel(group, learned);
        if (other == nullptr) continue;
        for (std::size_t i = 0; i < other->prerequisiteGroups.size(); ++i) {
            if (other->prerequisiteGroups[i] == skill->group && other->prerequisiteLevels[i] > target) {
                w.code = withdrawcode::kRequired;
                return w;
            }
        }
    }
    std::uint64_t sp = 0, gold = 0;
    int masteryLevel = 0;
    for (std::uint32_t m : skill->masteries) masteryLevel = std::max(masteryLevel, m != 0 ? book.MasteryLevel(m) : 0);
    for (int l = target + 1; l <= level; ++l) {
        if (const assets::SkillRow* row = table.GroupLevel(skill->group, l)) sp += std::uint64_t(std::max(0, row->spCost));
        if (mode == WithdrawMode::Quest) gold += std::uint64_t(goldMin) * 10u * std::uint64_t(10 + masteryLevel);
    }
    w.levels = level - target;
    w.skillPoints = Refund(sp, mode);
    w.gold = gold;
    const assets::SkillRow* now = target > 0 ? table.GroupLevel(skill->group, target) : nullptr;
    w.skillNow = now != nullptr ? now->id : 0;
    return w;
}

Withdrawal WithdrawMastery(const SkillBook& book, const assets::SkillTable& table, const std::vector<std::uint32_t>& masteryCosts, std::uint32_t mastery,
                           int target, WithdrawMode mode, std::uint32_t goldMin) {
    Withdrawal w;
    const int level = book.MasteryLevel(mastery);
    if (level <= 0) {
        w.code = withdrawcode::kNotLearned;
        return w;
    }
    if (target < 0 || target >= level) {
        w.code = withdrawcode::kLevelTooLow;
        return w;
    }
    // A learned skill whose mastery requirement the lower level would no longer meet.
    for (const auto& [group, learned] : book.groups) {
        if (learned <= 0) continue;
        const assets::SkillRow* other = table.GroupLevel(group, learned);
        if (other == nullptr) continue;
        for (std::size_t i = 0; i < other->masteries.size(); ++i) {
            if (other->masteries[i] == mastery && other->masteryLevels[i] > target) {
                w.code = withdrawcode::kRequired;
                return w;
            }
        }
    }
    std::uint64_t sp = 0, gold = 0;
    for (int l = target; l < level; ++l) {
        if (const std::uint32_t cost = MasteryCost(masteryCosts, l); cost != ~0u) sp += cost;
        if (mode == WithdrawMode::Quest) gold += std::uint64_t(goldMin) * 10u * std::uint64_t(10 + l + 1);
    }
    w.levels = level - target;
    w.skillPoints = Refund(sp, mode);
    w.gold = gold;
    return w;
}

bool IsMagicCube(const assets::ItemRow& row) { return row.typeId == std::array<int, 4>{3, 2, 3, 1}; }
int CubePlaces(const assets::ItemRow& cube) { return std::max(0, cube.params[8]); }
bool CubeTakes(const assets::ItemRow& cube, const assets::ItemRow& item) {
    return IsMagicCube(cube) && item.typeId[0] == 3 && item.typeId[1] == 3 && cube.params[9] != 0 && item.params[9] == cube.params[9];
}

} // namespace sr::game
