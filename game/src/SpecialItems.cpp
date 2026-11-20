#include "sr/game/SpecialItems.h"

#include <cstdlib>
#include <string_view>

#include "sr/assets/GameData.h"

namespace sr::game {

namespace {
bool Is(const assets::ItemRow& row, int t2, int t3) { return row.typeId[0] == 3 && row.typeId[1] == t2 && row.typeId[2] == t3; }
} // namespace

ItemCoolTime CoolTimeOf(const assets::ItemRow& row) {
    constexpr std::string_view kTag = "COOLTIME:";
    for (std::size_t k = 0; k < row.paramTexts.size(); ++k) {
        const std::string_view text = row.paramTexts[k];
        const std::size_t at = text.find(kTag);
        if (at == std::string_view::npos || row.params[k] <= 0) continue;
        // "0x..." read as hex, else a number.
        const std::string group(text.substr(at + kTag.size()));
        ItemCoolTime c;
        c.group = std::uint32_t(std::strtoul(group.c_str(), nullptr, group.starts_with("0x") || group.starts_with("0X") ? 16 : 10));
        c.ms = std::uint32_t(row.params[k]);
        return c;
    }
    return {};
}

bool IsFirework(const assets::ItemRow& row) { return Is(row, 3, 6) && row.typeId[3] != 2; }

bool IsBerserkPotion(const assets::ItemRow& row) { return Is(row, 3, 1) && row.typeId[3] == 8; }

BonusScroll BonusScrollOf(const assets::ItemRow& row) {
    BonusScroll b;
    if (!Is(row, 3, 3) || row.typeId[3] != 10) return b;
    b.key = std::uint32_t(row.params[1]);
    b.value = row.params[2];
    b.seconds = row.params[0];
    switch (b.key) {
    case kBonusExperienceKey: b.kind = BonusKind::Experience; break;
    case kBonusSkillPointsKey:
    case kBonusSkillExperienceKey: b.kind = BonusKind::SkillExperience; break;
    case kBonusBerserkTimeKey: b.kind = BonusKind::BerserkTime; break;
    default: break;
    }
    if (b.value <= 0) b.kind = BonusKind::None;
    return b;
}

bool RequirementTypeKnown(int type) {
    // -1 none (0 too: ours, for rows with no requirement written; no item in service carries it), 1 the level, 2..4 and
    // 0x10 skipped there, 0x201..0x206 the European masteries, 0x301 the special rank.
    return type == -1 || type == 0 || type == 1 || (type >= 2 && type <= 4) || type == 0x10 || (type >= 0x201 && type <= 0x206) || type == 0x301;
}

std::uint16_t OriginalUseRefusal(const assets::ItemRow& row) {
    // The requirement gate before every use: a type it does not know is logged and the use
    // refused with the code it began with, 2. In 1.188's data only the guild's mercenary scrolls
    // carry one (type 10, level 3): none of them is ever used.
    for (const int type : row.requirementTypes) {
        if (!RequirementTypeKnown(type)) return 2;
    }
    const int t4 = row.typeId[3];
    if (Is(row, 3, 13)) {
        if (t4 == 10 || t4 == 11 || t4 == 13 || t4 == 0 || t4 == 18) return 2;
        return 0;
    }
    if (Is(row, 3, 3) && (t4 == 12 || t4 == 14)) return 0x183E;
    if (Is(row, 3, 12) && t4 == 4) return 0x183E;
    if (Is(row, 3, 16)) return 0x183E;
    return 0;
}

bool IsQuestUseItem(const assets::ItemRow& row) { return (Is(row, 3, 9) && row.canUse != 0) || Is(row, 3, 15); }

} // namespace sr::game
