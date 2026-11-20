#include "sr/game/Items.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "sr/assets/GameData.h"

namespace sr::game {

namespace {

// The original rounds by adding a half and truncating.
double Round(double v) { return std::trunc(v + 0.5); }

double Lerp(double low, double high, int slot) { return low + (high - low) * double(slot) / 31.0; }

int Percent(double value, double low, double high) {
    if (high == low) return 100;
    return int(std::trunc((value - low) / (high - low) * 100.0));
}

double Fraction(double value, double low, double high) { return high == low ? 1.0 : (value - low) / (high - low); }

// The option codes the item's own numbers read (`_RefMagicOpt.Param1`; and
// the total plus find them by code, so the ROC set's MATTR_ER_SET and MATTR_DUR_SET count as 'er' and 'duru').
constexpr std::uint32_t kCodeHr = 0x6872, kCodeEr = 0x6572, kCodeDuru = 0x64757275, kCodeDura = 0x64757261, kCodeNrep = 0x6E726570,
                        kCodeRein = 0x7265696E;

// The sum of an item's options of one code ('duru', ...).
double OptionSum(const ItemInstance& item, const std::vector<assets::MagicOptionRow>* options, std::uint32_t code) {
    if (options == nullptr) return 0.0;
    double sum = 0.0;
    for (const ItemInstance::Option& o : item.options) {
        const assets::MagicOptionRow* row = FindOption(*options, o.id);
        if (row != nullptr && row->attribute == code) sum += double(o.value);
    }
    return sum;
}

// 'nrep' ("repair invalid") raises the most durability by its row's Param3 in percent (400: x 5), whatever the item's
// value (the option kind's Param3; the drops write it with 0).
double NotReparableSum(const ItemInstance& item, const std::vector<assets::MagicOptionRow>* options) {
    if (options == nullptr) return 0.0;
    double sum = 0.0;
    for (const ItemInstance::Option& o : item.options) {
        const assets::MagicOptionRow* row = FindOption(*options, o.id);
        if (row != nullptr && row->attribute == kCodeNrep) sum += double(row->rangeWords[1]);
    }
    return sum;
}

} // namespace

bool IsMonsterMask(const assets::ItemRow& row) { return row.typeId[0] == 3 && row.typeId[1] == 2 && row.typeId[2] == 2; }
bool IsCosSummoner(const assets::ItemRow& row) { return row.typeId[0] == 3 && row.typeId[1] == 2 && row.typeId[2] == 1; }
bool DataIsCount(const assets::ItemRow& row) { return row.typeId[1] != 1 && !IsMonsterMask(row) && !IsCosSummoner(row); }

ItemKind KindOf(const assets::ItemRow& row) {
    const auto& t = row.typeId;
    if (t[0] != 3) return ItemKind::Other;
    if (t[1] == 2) return ItemKind::Pet;
    if (t[1] == 3) return ItemKind::Expendable;
    if (t[1] != 1) return ItemKind::Other;
    switch (t[2]) {
    case 6: return ItemKind::Weapon;
    case 4: return ItemKind::Shield;
    case 1:
    case 2:
    case 3:
    case 9:
    case 10:
    case 11: return ItemKind::Armour;
    case 5:
    case 12: return ItemKind::Accessory;
    case 7: return ItemKind::JobSuit;
    case 13: // avatars
    case 14: // the Devil's Spirit
        return ItemKind::Avatar;
    default: return ItemKind::Other;
    }
}

bool TwoHanded(const assets::ItemRow& row) { return KindOf(row) == ItemKind::Weapon && row.Stat(93) != 0.0f; }

bool IsAmmunition(const assets::ItemRow& row) { return row.typeId[0] == 3 && row.typeId[1] == 3 && row.typeId[2] == 4; }

std::uint32_t NewDurability(const assets::ItemRow& row) {
    const ItemKind kind = KindOf(row);
    if (kind != ItemKind::Weapon && kind != ItemKind::Shield && kind != ItemKind::Armour) return 0;
    return std::uint32_t(std::max(0.0, Round(double(row.Stat(63)))));
}

int AmmunitionFor(const assets::ItemRow& weapon) {
    if (KindOf(weapon) != ItemKind::Weapon) return 0;
    return weapon.typeId[3] == 6 ? 1 : weapon.typeId[3] == 12 ? 2 : 0;
}

int MaxStack(const assets::ItemRow& row) {
    // Ours (the user's call, 2026-11-12): elixirs stack to 100 (the original's 1), arrows and bolts to 1,000, or their
    // row's own when it is more (the mall's quivers of 10,000).
    const auto& t = row.typeId;
    if (t[0] == 3 && t[1] == 3 && t[2] == 10 && t[3] == 1) return 100;
    if (t[0] == 3 && t[1] == 3 && t[2] == 4 && (t[3] == 1 || t[3] == 2)) return std::max(1000, row.maxStack);
    return std::max(1, row.maxStack);
}

int EquipPlaceFor(const assets::ItemRow& row, bool leftRingTaken, bool rightRingTaken) {
    // Observed: (armour by its part, table).
    constexpr int kArmour[] = {-1, slot::kHead, slot::kShoulders, slot::kChest, slot::kLegs, slot::kHands, slot::kFeet};
    const int part = row.typeId[3];
    switch (KindOf(row)) {
    case ItemKind::Armour: return part >= 1 && part <= 6 ? kArmour[part] : -1;
    case ItemKind::Weapon: return slot::kWeapon;
    case ItemKind::Shield: return slot::kShield;
    case ItemKind::JobSuit: return slot::kJobSuit;
    case ItemKind::Accessory:
        if (part == 1) return slot::kEarring;
        if (part == 2) return slot::kNecklace;
        if (part == 3) return !leftRingTaken ? slot::kRingLeft : !rightRingTaken ? slot::kRingRight : slot::kRingLeft;
        return -1;
    default: return IsAmmunition(row) ? slot::kShield : -1;
    }
}

bool LevelFits(const assets::ItemRow& row, int level) {
    for (int i = 0; i < 4; ++i) {
        if (row.requirementTypes[std::size_t(i)] == 1 && level < row.requirementLevels[std::size_t(i)]) return false;
    }
    return true;
}

EquipCheck RequirementsMet(const assets::ItemRow& row, const Wearer& who) {
    bool levelNamed = false, levelMet = false, masteryNamed = false, masteryMet = false;
    for (std::size_t i = 0; i < 4; ++i) {
        const int type = row.requirementTypes[i], level = row.requirementLevels[i];
        // -1 none (0 too: a row made by hand; no 1.188 row has it), 2..4 and 0x10 not this check's.
        if (type == -1 || type == 0 || (type >= 2 && type <= 4) || type == 0x10) continue;
        if (type == 1) {
            levelNamed = true;
            levelMet = levelMet || who.level >= level;
        } else if (type >= 0x201 && type <= 0x206) {
            if (!who.masteriesKnown) continue;
            masteryNamed = true;
            const auto it = who.masteries.find(std::uint32_t(type));
            masteryMet = masteryMet || (it != who.masteries.end() && it->second >= level);
        } else {
            return EquipCheck::WrongSlot; // 0x301 and the unknown types: none is in the 1.188 data
        }
    }
    if (levelNamed && !levelMet) return EquipCheck::Level;
    if (masteryNamed && !masteryMet) return EquipCheck::Mastery;
    return EquipCheck::Ok;
}

EquipCheck CanEquip(const assets::ItemRow& row, int place, const Wearer& who, const ItemInstance& item) {
    const int wanted = EquipPlaceFor(row, false, false);
    if (wanted < 0) return EquipCheck::NotWearable;
    if (place < 0 || place >= slot::kWorn) return EquipCheck::WrongSlot;                       // 0x1806
    if (row.sex != 2 && (row.sex == 1) != who.male) return EquipCheck::Sex;                     // 0x1816
    if (NewDurability(row) > 0 && item.durability == 0) return EquipCheck::Broken;             // 0x1837
    if (const EquipCheck r = RequirementsMet(row, who); r != EquipCheck::Ok) return r;          // 0x186C
    if (row.country != 3 && row.country != who.country) return EquipCheck::Race;               // 0x182F
    const bool ring = wanted == slot::kRingLeft;
    if (ring ? (place != slot::kRingLeft && place != slot::kRingRight) : place != wanted) return EquipCheck::WrongSlot; // 0x180E
    return EquipCheck::Ok;
}

int AvatarPlaceFor(const assets::ItemRow& row) {
    // Observed: (TID3 13: TID4 - 1; TID3 14: TID4 + 3).
    const auto& t = row.typeId;
    if (t[0] != 3 || t[1] != 1) return -1;
    if (t[2] == 13 && t[3] >= 1 && t[3] <= 4) return t[3] - 1;
    if (t[2] == 14 && t[3] == 1) return avatar::kSpirit;
    return -1;
}

EquipCheck CanWearAvatar(const assets::ItemRow& row, const Wearer& who) {
    // The original's order: the sex, the requirements, the country.
    if (AvatarPlaceFor(row) < 0) return EquipCheck::NotWearable;
    if (row.sex != 2 && (row.sex == 1) != who.male) return EquipCheck::Sex;
    if (const EquipCheck r = RequirementsMet(row, who); r != EquipCheck::Ok) return r;
    if (row.country != 3 && row.country != who.country) return EquipCheck::Race;
    return EquipCheck::Ok;
}

const assets::MagicOptionRow* FindOption(const std::vector<assets::MagicOptionRow>& options, std::uint32_t id) {
    for (const assets::MagicOptionRow& row : options) {
        if (row.paramId == (id & 0xFFFF)) return &row;
    }
    return nullptr;
}

int OptionPercent(const std::vector<assets::MagicOptionRow>& options, const ItemInstance::Option& option, int degree) {
    const assets::MagicOptionRow* row = FindOption(options, option.id);
    if (row == nullptr) return -1;
    // The range of the same code at the item's degree.
    if (row->degree != degree) {
        for (const assets::MagicOptionRow& other : options) {
            if (other.optionName == row->optionName && other.degree == degree) {
                row = &other;
                break;
            }
        }
    }
    if (!row->hasRange) return -1;
    // The range: the high half of the first word, up to the last non-zero half (high, low, ...).
    const double low = double(row->rangeWords[0] >> 16);
    double high = low;
    for (std::uint32_t word : row->rangeWords) {
        for (std::uint32_t half : {word >> 16, word & 0xFFFF}) {
            if (half != 0) high = double(half);
        }
    }
    if (high <= low) return 100;
    return std::max(0, int(std::trunc((double(option.value) - low) / (high - low) * 100.0)));
}

ItemStats ComputeStats(const assets::ItemRow& row, const ItemInstance& item, const std::vector<assets::MagicOptionRow>* options, int setPlus,
                       bool server) {
    ItemStats s;
    const ItemKind kind = KindOf(row);
    const auto col = [&](int c) { return double(row.Stat(c)); };
    const auto slot = [&](int i) { return VarianceSlot(item.variance, i); };
    // The total plus: the plus, the advanced elixir's (its binding options of type 2), its set's raise (the extra
    // argument: 'reis') and the "rein" blue
    // (MATTR_REINFORCE_ITEM's +1 / +2: the reinforce scrolls' 30 minutes).
    const double opt = TotalPlus(item) + setPlus + OptionSum(item, options, kCodeRein);
    if (row.itemClass > 0) s.degree = (row.itemClass - 1) / 3 + 1;
    if (row.Rare() && row.itemClass > 0) s.seal = (row.itemClass - 1) % 3;
    if (kind != ItemKind::Weapon && kind != ItemKind::Shield && kind != ItemKind::Armour && kind != ItemKind::Accessory) return s;

    // The server's white: float32(low + float32((high - low) x v + float32(per plus x the total
    // plus))), v = float32(slot / 31); the sums in the FPU's double precision.
    const float opt32 = float(opt);
    const auto exact = [&](int lowCol, int highCol, int incCol, int sl) {
        const double low = col(lowCol), high = col(highCol);
        const double v = double(float(double(slot(sl)) / 31.0));
        const double plusPart = incCol > 0 ? double(float(col(incCol) * double(opt32))) : 0.0;
        return double(float(low + double(float((high - low) * v + plusPart))));
    };
    const auto exactStat = [&](ItemStat& st, int lowCol, int highCol, int incCol, int sl) {
        st.low = st.high = exact(lowCol, highCol, incCol, sl);
        st.percent = Percent(Lerp(col(lowCol), col(highCol), slot(sl)), col(lowCol), col(highCol));
        st.shown = st.low != 0.0;
    };

    // An integer stat with a plus, a float one with a plus, an integer one without.
    const auto intPlus = [&](ItemStat& st, int lowCol, int highCol, int incCol, int sl) {
        if (server) return exactStat(st, lowCol, highCol, incCol, sl);
        const double plusPart = std::trunc(opt * col(incCol));
        st.low = st.high = Round(Lerp(col(lowCol), col(highCol), slot(sl)) + plusPart);
        st.percent = Percent(st.low - plusPart, col(lowCol), col(highCol));
        st.shown = st.low != 0.0;
    };
    const auto floatPlus = [&](ItemStat& st, int lowCol, int highCol, int incCol, int sl) {
        if (server) return exactStat(st, lowCol, highCol, incCol, sl);
        const double plusPart = opt * col(incCol);
        st.low = st.high = Lerp(col(lowCol), col(highCol), slot(sl)) + plusPart;
        st.percent = Percent(st.low - plusPart, col(lowCol), col(highCol));
        st.shown = st.low != 0.0;
    };
    const auto intPlain = [&](ItemStat& st, int lowCol, int highCol, int sl) {
        if (server) return exactStat(st, lowCol, highCol, 0, sl);
        st.low = st.high = Round(Lerp(col(lowCol), col(highCol), slot(sl)));
        st.percent = Percent(st.low, col(lowCol), col(highCol));
        st.shown = st.low != 0.0;
    };
    // A range (min ~ max) whose percent averages the two ends' places.
    const auto range = [&](ItemStat& st, int minLow, int minHigh, int maxLow, int maxHigh, int incCol, int sl, bool integer, double scale) {
        if (server) {
            // Both ends of the slot's variance, the min's taking the max's per plus too.
            st.low = exact(minLow, minHigh, incCol, sl) * scale;
            st.high = exact(maxLow, maxHigh, incCol, sl) * scale;
            st.percent = 0;
            st.shown = st.high != 0.0;
            return;
        }
        const double plusPart = incCol > 0 ? std::trunc(opt * col(incCol)) : 0.0;
        const double a = Lerp(col(minLow), col(minHigh), slot(sl)), b = Lerp(col(maxLow), col(maxHigh), slot(sl));
        st.low = integer ? Round(a + plusPart) : a * scale;
        st.high = integer ? Round(b + plusPart) : b * scale;
        const double fa = Fraction(integer ? st.low - plusPart : a, col(minLow), col(minHigh));
        const double fb = Fraction(integer ? st.high - plusPart : b, col(maxLow), col(maxHigh));
        st.percent = int(std::trunc((fa + fb) * 0.5 * 100.0));
        st.shown = st.high != 0.0;
    };
    // A reinforce: the column in tenths of a percent (inferred: the loader's 1/1000, shown x 100).
    const auto reinforce = [&](ItemStat& st, int lowCol, int highCol, int sl) {
        if (server) {
            st.low = st.high = exact(lowCol, highCol, 0, sl) / 10.0;
            st.percent = Percent(Lerp(col(lowCol), col(highCol), slot(sl)), col(lowCol), col(highCol));
            st.shown = st.low != 0.0;
            return;
        }
        const double v = Lerp(col(lowCol), col(highCol), slot(sl));
        st.low = st.high = v / 10.0;
        st.percent = Percent(v, col(lowCol), col(highCol));
        st.shown = v != 0.0;
    };

    // Durability: the variance's slot 0 for everything but accessories; the blues change the most.
    if (kind != ItemKind::Accessory) {
        // The server truncates it (low + float32((high - low) x v), cut toward 0); the tooltip rounds.
        double most = server ? std::trunc(col(63) + double(float((col(64) - col(63)) * double(float(double(slot(0)) / 31.0))))) : Round(Lerp(col(63), col(64), slot(0)));
        // By code: 'duru' (MATTR_DUR, MATTR_DUR_SET) adds its value in percent, 'dura' (MATTR_DEC_MAXDUR)
        // takes its value off, 'nrep' (MATTR_NOT_REPARABLE, the uniques' and profiles' pieces) adds its row's Param3.
        const double change = OptionSum(item, options, kCodeDuru) + NotReparableSum(item, options) - OptionSum(item, options, kCodeDura);
        if (change != 0.0) most = std::max(1.0, most + std::trunc(most * change / 100.0));
        s.durability.high = most;
        s.durability.low = std::min<double>(item.durability, most);
        s.durability.percent = int(std::trunc(double(slot(0)) / double(0.31f))); // 31 shows 99 (observed)
        s.durability.shown = most != 0.0;
    }
    // Attack rating and parry: the blue's percent of the white value on top (inferred shape).
    const auto withBlue = [&](ItemStat& st, std::uint32_t code) {
        const double extra = OptionSum(item, options, code);
        if (extra == 0.0 || !st.shown) return;
        // The server: x (1 + v / 100) as a float32, no floor; the tooltip truncates the added part.
        if (server) st.low = st.high = double(float(st.low * (extra / 100.0) + st.low));
        else st.low = st.high = std::max(1.0, st.low + std::trunc(st.low * extra / 100.0));
    };
    switch (kind) {
    case ItemKind::Weapon:
        range(s.phyReinforce, 105, 106, 107, 108, 0, 1, false, 0.1);
        range(s.magReinforce, 109, 110, 111, 112, 0, 2, false, 0.1);
        intPlus(s.attackRating, 113, 114, 115, 3);
        withBlue(s.attackRating, kCodeHr);
        range(s.phyAttack, 95, 96, 97, 98, 99, 4, true, 1.0);
        range(s.magAttack, 100, 101, 102, 103, 104, 5, true, 1.0);
        intPlain(s.critical, 116, 117, 6);
        s.range = col(94) / 10.0;
        break;
    case ItemKind::Shield:
        reinforce(s.phyReinforce, 82, 83, 1);
        reinforce(s.magReinforce, 84, 85, 2);
        intPlain(s.blocking, 74, 75, 3);
        floatPlus(s.phyDefence, 65, 66, 67, 4);
        floatPlus(s.magDefence, 76, 77, 78, 5);
        break;
    case ItemKind::Armour:
        reinforce(s.phyReinforce, 82, 83, 1);
        reinforce(s.magReinforce, 84, 85, 2);
        floatPlus(s.phyDefence, 65, 66, 67, 3);
        floatPlus(s.magDefence, 76, 77, 78, 4);
        intPlus(s.parry, 68, 69, 70, 5);
        withBlue(s.parry, kCodeEr);
        break;
    case ItemKind::Accessory:
        floatPlus(s.phyAbsorption, 71, 72, 73, 0);
        floatPlus(s.magAbsorption, 79, 80, 81, 1);
        break;
    default: break;
    }
    return s;
}

bool Repairable(const assets::ItemRow& row, const ItemInstance& item, const std::vector<assets::MagicOptionRow>* options) {
    if (row.typeId[0] != 3 || row.typeId[1] != 1 || row.typeId[2] == 5 || row.typeId[2] == 12) return false;
    if (options == nullptr) return true;
    for (const ItemInstance::Option& o : item.options) {
        const assets::MagicOptionRow* m = FindOption(*options, o.id);
        if (m == nullptr) continue;
        if (m->optionName == "MATTR_NOT_REPARABLE") return false;
        if (m->optionName == "MATTR_REPAIR" && o.value < 2) return false;
    }
    return true;
}

RepairPrice PriceRepair(const assets::ItemRow& row, std::uint32_t durability, std::uint32_t most, std::uint64_t budget, bool rounded) {
    RepairPrice p;
    if (most == 0) return p;
    p.revive = durability == 0;
    const std::uint32_t from = std::max<std::uint32_t>(durability, 1);
    if (from >= most && !p.revive) return p;
    const std::uint32_t missing = most > from ? most - from : 0;
    // A point's price as the original keeps it: float32(CostRepair / most), at least 1.
    const double perPoint = double(std::max(1.0f, float(row.costRepair) / float(most)));
    std::uint64_t revive = p.revive ? row.costRevive : 0;
    std::uint32_t points = missing;
    if (budget != 0) {
        if (revive > budget) return {};
        const double affordable = std::trunc(double(budget - revive) / perPoint);
        points = std::uint32_t(std::min<double>({double(missing), affordable, 100'000.0}));
    }
    const double cost = double(points) * perPoint;
    p.gold = std::min<std::uint64_t>(std::uint64_t(rounded ? std::round(cost) : std::trunc(cost)), 100'000'000) + revive;
    p.points = points + (p.revive ? 1u : 0u);
    return p;
}

} // namespace sr::game
