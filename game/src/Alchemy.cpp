#include "sr/game/Alchemy.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

#include "sr/assets/GameData.h"

namespace sr::game::alchemy {

namespace {

bool IsProtection(std::string_view name) {
    return name == "MATTR_ATHANASIA" || name == "MATTR_LUCK" || name == "MATTR_SOLID" || name == "MATTR_ASTRAL" || name == "MATTR_APE";
}
// Options assimilation never re-rolls (3.2): the protections, the durability's and the honor items' Repair.
bool NeverRerolled(std::string_view name) {
    return IsProtection(name) || name == "MATTR_DEC_MAXDUR" || name == "MATTR_NOT_REPARABLE" || name == "MATTR_REPAIR";
}

const assets::MagicOptionRow* RowOf(const Options& options, const ItemInstance::Option& o) { return FindOption(options, o.id); }

// The index of the item's option of that name, -1 when it has none.
int OptionIndex(const ItemInstance& item, const Options& options, std::string_view name) {
    for (std::size_t i = 0; i < item.options.size(); ++i) {
        const assets::MagicOptionRow* row = RowOf(options, item.options[i]);
        if (row != nullptr && row->optionName == name) return int(i);
    }
    return -1;
}

// The index of the item's option of the same code as that name's (`_RefMagicOpt.Param1`: MATTR_LUCK and
// MATTR_LUCK_SET are both 'luck'), -1 when it has none. The original finds a charge so ('luck', 'atha', 'soli',
// 'astr').
int ChargeIndex(const ItemInstance& item, const Options& options, std::string_view name) {
    std::uint32_t code = 0;
    for (const assets::MagicOptionRow& row : options) {
        if (row.optionName == name) {
            code = row.attribute;
            break;
        }
    }
    if (code == 0) return OptionIndex(item, options, name);
    for (std::size_t i = 0; i < item.options.size(); ++i) {
        const assets::MagicOptionRow* row = RowOf(options, item.options[i]);
        if (row != nullptr && row->attribute == code) return int(i);
    }
    return -1;
}

// The names the option tables use for an item (AvailItemGroup).
std::vector<std::string_view> GroupsOf(const assets::ItemRow& row) {
    const int tid3 = row.typeId[2], tid4 = row.typeId[3];
    std::vector<std::string_view> groups;
    if (tid3 == 6) groups.push_back("weapon");
    else if (tid3 == 4) groups.push_back("shield");
    else if (tid3 == 1 || tid3 == 2 || tid3 == 3 || tid3 == 9 || tid3 == 10 || tid3 == 11) {
        groups.push_back("armor");
        if (tid4 == 1) groups.push_back("helm");
        if (tid4 == 3) groups.push_back("mail");
        if (tid4 == 4) groups.push_back("pants");
    } else if (tid3 == 5 || tid3 == 12) {
        groups.push_back("accessory");
        if (tid4 == 1) groups.push_back("earring");
        if (tid4 == 2) groups.push_back("necklace");
        if (tid4 == 3) groups.push_back("ring");
    } else if (tid3 == 14 && row.typeId[0] == 3 && row.typeId[1] == 1) {
        // The Devil's Spirit: its options' group (TID 3/1/14, 0xF2C), the UMDU stone's.
        groups.push_back("avatar_special_armlet");
    }
    return groups;
}

// The kind word of an attribute stone's Desc4 (weapon, armor, shield, accessory).
std::string_view KindWord(const assets::ItemRow& row) {
    const int tid3 = row.typeId[2];
    if (tid3 == 6) return "weapon";
    if (tid3 == 4) return "shield";
    if (tid3 == 5 || tid3 == 12) return "accessory";
    return "armor";
}

void SetField(ItemInstance& item, int field, int value) {
    const std::uint64_t mask = std::uint64_t(31) << (5 * field);
    item.variance = (item.variance & ~mask) | (std::uint64_t(std::clamp(value, 0, 31)) << (5 * field));
}

// A stone's chance (3.2): rand() % 100 <= clamp(Param4 + bonus, 5, 100), so 50 is 51 %.
bool StoneRoll(const assets::ItemRow& stone, int bonus, std::mt19937& random, int& chance) {
    chance = std::clamp(stone.params[3] + bonus, 5, 100);
    return Below(random, 100) <= chance;
}

// A stone's assimilation (3.2): when its rate beats rand() % 101, an APE charge stops it; else a white field (not
// the one just set) or an option (a magic stone: the first; an attribute stone: the one picked) takes a new value.
void Assimilate(const assets::ItemRow& target, ItemInstance& item, int rate, int setField, bool magicStone, const Options& options,
                std::mt19937& random, StoneResult& r) {
    if (rate <= Below(random, 101)) return;
    if (UseCharge(item, options, "MATTR_APE")) {
        r.apeUsed = true;
        return;
    }
    const std::vector<std::string_view> fields = WhiteFields(target);
    const int white = int(fields.size());
    const int n = white + int(item.options.size());
    if (n <= 0) return;
    const int pick = Below(random, n);
    if (pick < white) {
        if (pick == setField) return;
        SetField(item, pick, RollWhite(target, random));
        r.assimilated = true;
        r.whiteField = pick;
        return;
    }
    if (item.options.empty()) return;
    const int index = magicStone ? 0 : pick - white;
    if (index < 0 || index >= int(item.options.size())) return;
    const assets::MagicOptionRow* row = RowOf(options, item.options[std::size_t(index)]);
    if (row == nullptr || NeverRerolled(row->optionName)) return;
    const std::vector<std::uint32_t> values = OptionValues(options, row->optionName, row->degree);
    if (values.empty()) return;
    item.options[std::size_t(index)].value = std::clamp<std::uint32_t>(values[std::size_t(Below(random, int(values.size())))], 1, 1700);
    r.assimilated = true;
    r.optionIndex = index;
}

const char* const kElementNames[4] = {"EARTH", "WATER", "FIRE", "WIND"};

const assets::ItemRow* ElementOf(const assets::ItemTable& items, int kind, int degree) {
    return items.Find(std::string_view(std::format("ITEM_ETC_ARCHEMY_ELEMENT_{}_{:02}", kElementNames[kind], degree)));
}

} // namespace

Kind KindOf(const assets::ItemRow& row) {
    const auto& t = row.typeId;
    if (t[0] != 3) return Kind::None;
    if (t[1] == 1) {
        const int tid3 = t[2];
        const bool equipment = tid3 == 1 || tid3 == 2 || tid3 == 3 || tid3 == 4 || tid3 == 5 || tid3 == 6 || tid3 == 9 || tid3 == 10 ||
                               tid3 == 11 || tid3 == 12;
        return equipment ? Kind::Equipment : Kind::None;
    }
    if (t[1] != 3) return Kind::None;
    if (t[2] == 10) {
        if (t[3] == 1) return Kind::Elixir;
        if (t[3] == 2) return Kind::LuckyPowder;
        if (t[3] == 4) return Kind::AdvancedElixir;
        return Kind::None;
    }
    if (t[2] == 11) {
        switch (t[3]) {
        case 1:
        case 7: return Kind::MagicStone;
        case 2: return Kind::AttrStone;
        case 3: return Kind::Tablet;
        case 4: return Kind::ByProduct;
        case 5: return Kind::Element;
        case 6: return Kind::Rondo;
        default: return Kind::None;
        }
    }
    return Kind::None;
}

int Degree(const assets::ItemRow& row) {
    if (row.typeId[0] == 3 && row.typeId[1] == 1) return row.itemClass > 0 ? (row.itemClass - 1) / 3 + 1 : 0;
    return row.itemClass;
}

int Below(std::mt19937& random, int n) { return n <= 1 ? 0 : std::uniform_int_distribution<int>(0, n - 1)(random); }

// --- Charges ---------------------------------------------------------------------------------------------------------

std::uint32_t OptionValue(const ItemInstance& item, const Options& options, std::string_view name) {
    const int i = ChargeIndex(item, options, name);
    return i < 0 ? 0 : item.options[std::size_t(i)].value;
}

bool UseCharge(ItemInstance& item, const Options& options, std::string_view name) {
    const int i = ChargeIndex(item, options, name);
    if (i < 0 || item.options[std::size_t(i)].value == 0) return false;
    if (--item.options[std::size_t(i)].value == 0) item.options.erase(item.options.begin() + i);
    return true;
}

int OptionCapacity(const assets::ItemRow& row, const ItemInstance& item, const Options& options) {
    int extra = 0;
    for (const ItemInstance::Option& o : item.options) {
        const assets::MagicOptionRow* m = RowOf(options, o);
        if (m != nullptr && (m->optionName == "MATTR_DEC_MAXDUR" || m->optionName == "MATTR_NOT_REPARABLE")) ++extra;
    }
    return std::min(12, row.maxMagicOptions + extra);
}

// --- Reinforcement -----------------------------------------------------------------------------------------------------

int ChanceByte(const assets::ItemRow& row, int plus) {
    plus = std::max(0, plus);
    const int tier = std::clamp(plus / 4 + 1, 1, 3); // Param2, Param3, Param4
    const std::uint32_t param = std::uint32_t(row.params[std::size_t(tier)]);
    const int column = plus < 12 ? plus % 4 : 3;
    return int((param >> (8 * (3 - column))) & 0xFF);
}

bool ElixirFits(const assets::ItemRow& elixir, const assets::ItemRow& target) {
    const int tid3 = target.typeId[2];
    for (const int param : {elixir.params[0], elixir.params[4]}) {
        for (int b = 0; b < 4; ++b) {
            if (int((std::uint32_t(param) >> (8 * b)) & 0xFF) == tid3 && tid3 != 0) return true;
        }
    }
    return false;
}

std::uint16_t CheckReinforce(const assets::ItemRow& target, const ItemInstance& item, const assets::ItemRow& elixir,
                             const assets::ItemRow* powder) {
    // A Devil's Spirit (3/1/14) takes elixirs only with its Param2 1.
    const bool spirit = target.typeId[0] == 3 && target.typeId[1] == 1 && target.typeId[2] == 14;
    if (spirit && target.params[1] != 1) return code::kCannotReinforce;
    if (!spirit && KindOf(target) != Kind::Equipment) return code::kOnlyEquipment;
    if (item.plus >= 250) return code::kCannotReinforce;
    if (KindOf(elixir) != Kind::Elixir) return code::kNotMaterial;
    if (!ElixirFits(elixir, target)) return code::kRecipeMismatch;
    if (powder != nullptr) {
        if (KindOf(*powder) != Kind::LuckyPowder) return code::kNotMaterial;
        if (powder->params[0] != Degree(target)) return code::kPowderDegree;
    }
    return 0;
}

Chance ReinforceChance(const assets::ItemRow& elixir, const assets::ItemRow* powder, int plus, bool hasLuck, int bonus) {
    Chance c;
    int percent = ChanceByte(elixir, plus);
    if (powder != nullptr && percent < 100) percent += ChanceByte(*powder, plus);
    if (hasLuck && percent < 100) {
        percent += 5;
        c.usesLuck = true;
    }
    c.percent = std::clamp(percent + bonus, 10, 100);
    return c;
}

ReinforceResult Reinforce(const assets::ItemRow& target, ItemInstance& item, const assets::ItemRow& elixir, const assets::ItemRow* powder,
                          const Options& options, int bonus, std::mt19937& random) {
    ReinforceResult r;
    r.plusBefore = item.plus;
    const Chance chance = ReinforceChance(elixir, powder, item.plus, OptionValue(item, options, "MATTR_LUCK") > 0, bonus);
    if (chance.usesLuck) UseCharge(item, options, "MATTR_LUCK");
    r.chance = chance.percent;
    if (Below(random, 100) < chance.percent) {
        r.success = true;
        item.plus = std::uint8_t(std::min(250, item.plus + 1));
        r.plusAfter = item.plus;
        return r;
    }
    // A failure: from +5 half destroy (not a cash item), half the durability penalty.
    const int before = item.plus;
    bool destroy = false, penalty = false;
    if (before >= 5) {
        if (Below(random, 100) < 50) destroy = !target.cashItem;
        else penalty = true;
    }
    if (destroy && UseCharge(item, options, "MATTR_ATHANASIA")) destroy = false;
    if (penalty && UseCharge(item, options, "MATTR_SOLID")) penalty = false;
    r.destroyed = destroy;
    if (!destroy) {
        if (penalty && OptionFits(options, "MATTR_DEC_MAXDUR", target)) {
            // MATTR_DEC_MAXDUR's level-3 row, its value + 36..50, at most 99.
            const assets::MagicOptionRow* row = OptionRow(options, "MATTR_DEC_MAXDUR", 3);
            if (row != nullptr) {
                const int add = int(36.0 + double(Below(random, 32768)) / 32767.0 * 14.0);
                const int at = OptionIndex(item, options, "MATTR_DEC_MAXDUR");
                if (at >= 0) {
                    ItemInstance::Option& o = item.options[std::size_t(at)];
                    o.id = row->paramId;
                    o.value = std::uint32_t(std::clamp(int(o.value) + add, 1, 99));
                } else if (item.options.size() < 12) {
                    item.options.push_back({row->paramId, std::uint32_t(std::clamp(add, 1, 99))});
                }
                r.durabilityLost = true;
            }
        }
        if (before >= 1 && before <= 3) item.plus = 0;
        else if (before >= 4) {
            r.astralKept = UseCharge(item, options, "MATTR_ASTRAL");
            item.plus = r.astralKept ? 4 : 0;
        }
    }
    r.plusAfter = item.plus;
    return r;
}

// --- The advanced elixir ----------------------------------------------------------------------------------------------------

namespace {
// The rarity class the recipes are kept by: a normal or set item 1, a SoX or rare set item 2; 0 for others.
int RarityClass(const assets::ItemRow& row) { return row.rarity == 0 || row.rarity == 3 ? 1 : row.rarity == 2 || row.rarity == 6 ? 2 : 0; }
} // namespace

std::uint16_t CheckAdvanced(const assets::ItemRow& target, const ItemInstance& item, const assets::ItemRow& recipe) {
    if (KindOf(target) != Kind::Equipment) return code::kOnlyEquipment;
    if (item.plus >= 250) return code::kCannotReinforce;
    if (KindOf(recipe) != Kind::AdvancedElixir) return code::kNotMaterial;
    if (item.advanced > 0) return code::kAlreadyDone;
    if (!ElixirFits(recipe, target)) return code::kRecipeMismatch;
    if (RarityClass(target) != recipe.rarity) return code::kRecipeMismatch;
    if (recipe.itemClass < Degree(target)) return code::kRecipeLevel;
    return 0;
}

void ApplyAdvanced(ItemInstance& item, const assets::ItemRow& recipe) {
    item.advanced = std::uint8_t(std::clamp(recipe.params[7], 0, 255));
    item.advancedRecipe = recipe.refObjId;
}

const assets::ItemRow* DismantleRecipe(const assets::ItemTable& items, const assets::ItemRow& target, const ItemInstance& item) {
    const int weight = target.rarity == 2 || target.rarity == 6 ? 8 : target.rarity == 3 ? 3 : 1;
    const int tier = target.itemClass > 0 ? (target.itemClass - 1) % 3 + 1 : 1;
    const int total = TotalPlus(item);
    // Ours: the white stats' small merit (at most about 0.25) is left out.
    const int score = weight * tier + (total <= 4 ? 2 : total <= 6 ? 4 : 6);
    if (score < 10) return nullptr;
    const int tid3 = target.typeId[2];
    const char* kind = tid3 == 6 ? "WE" : tid3 == 4 ? "SH" : tid3 == 5 || tid3 == 12 ? "AC" : "AR";
    const bool rare = target.rarity == 2 || target.rarity == 6;
    return items.Find(std::string_view(std::format("ITEM_ETC_ARCHEMY_UPPER_REINFORCE_RECIPE_{}_{}{}_{}", kind, rare ? "RARE_" : "", score < 20 ? "A" : "B",
                                                   Degree(target) + 1)));
}

// --- Stones --------------------------------------------------------------------------------------------------------------

std::vector<std::string_view> WhiteFields(const assets::ItemRow& row) {
    const int tid3 = row.typeId[2];
    if (tid3 == 6) return {"DUR", "PASTR", "MAINT", "HR", "PA", "MA", "CRITICAL"};
    if (tid3 == 4) return {"DUR", "PDSTR", "MDINT", "BR", "PD", "MD"};
    if (tid3 == 5 || tid3 == 12) return {"PAR", "MAR"};
    if (KindOf(row) == Kind::Equipment) return {"DUR", "PDSTR", "MDINT", "PD", "MD", "ER"};
    return {};
}

int RollWhite(const assets::ItemRow& row, std::mt19937& random) {
    static constexpr int kSteps[6] = {0, 23, 43, 63, 83, 100};
    const int k = row.rarity == 2 ? 5 : 4;
    int least = 5;
    for (int i = 0; i < k; ++i) least = std::min(least, Below(random, 6));
    return kSteps[least] * 31 / 100;
}

const assets::MagicOptionRow* OptionRow(const Options& options, std::string_view name, int degree) {
    for (const assets::MagicOptionRow& row : options) {
        if (row.optionName == name && row.degree == degree) return &row;
    }
    return nullptr;
}

bool OptionFits(const Options& options, std::string_view name, const assets::ItemRow& target) {
    // Its level-1 row (else its lowest).
    const assets::MagicOptionRow* first = nullptr;
    for (const assets::MagicOptionRow& row : options) {
        if (row.optionName == name && (first == nullptr || row.degree < first->degree)) first = &row;
    }
    if (first == nullptr) return false;
    const std::vector<std::string_view> groups = GroupsOf(target);
    for (const std::string& g : first->availGroups) {
        if (std::find(groups.begin(), groups.end(), g) != groups.end()) return true;
    }
    return false;
}

bool RowFits(const assets::MagicOptionRow& row, const assets::ItemRow& target) {
    const std::vector<std::string_view> groups = GroupsOf(target);
    for (std::size_t i = 0; i < row.availGroups.size(); ++i) {
        const int reqClass = i < row.reqClasses.size() ? row.reqClasses[i] : 0;
        if (std::find(groups.begin(), groups.end(), row.availGroups[i]) != groups.end() && target.itemClass >= reqClass) return true;
    }
    return false;
}

std::vector<std::uint32_t> OptionValues(const Options& options, std::string_view name, int degree) {
    std::vector<std::uint32_t> values;
    const assets::MagicOptionRow* row = OptionRow(options, name, degree);
    if (row == nullptr || !row->hasRange) return values;
    for (const std::uint32_t word : row->rangeWords) {
        if (const std::uint32_t high = word >> 16; high != 0) values.push_back(high);
        if (const std::uint32_t low = word & 0xFFFF; low != 0) values.push_back(low);
    }
    return values;
}

std::uint16_t CheckMagicStone(const assets::ItemRow& target, const ItemInstance& item, const assets::ItemRow& stone, const Options& options) {
    // The first slot (every strengthening's): a Devil's Spirit only with its Param2 1 (its UMDU stone).
    const bool spirit = target.typeId[0] == 3 && target.typeId[1] == 1 && target.typeId[2] == 14;
    if (spirit && target.params[1] != 1) return code::kCannotReinforce;
    if (!spirit && KindOf(target) != Kind::Equipment) return code::kOnlyEquipment;
    if (KindOf(stone) != Kind::MagicStone) return code::kNotMaterial;
    // The stone's degree is the item's (the original checks it after the roll; ours before: the client refuses it too).
    if (stone.params[0] != Degree(target)) return code::kCannotReinforce;
    const std::string_view name = stone.paramTexts[0];
    if (!OptionFits(options, name, target)) return code::kRecipeMismatch;
    if (name == "MATTR_REPAIR" && OptionIndex(item, options, name) < 0) return code::kHonor;
    if (name == "MATTR_ASTRAL" && OptionValue(item, options, "MATTR_ATHANASIA") <= OptionValue(item, options, "MATTR_ASTRAL")) return code::kAstral;
    // A new option needs a place (the original finds it full only after its roll; ours refuses first).
    if (OptionIndex(item, options, name) < 0 && int(item.options.size()) >= OptionCapacity(target, item, options)) return code::kChargesFull;
    return 0;
}

std::uint16_t CheckAttrStone(const assets::ItemRow& target, const assets::ItemRow& stone) {
    if (KindOf(target) != Kind::Equipment) return code::kOnlyEquipment;
    if (KindOf(stone) != Kind::AttrStone) return code::kNotMaterial;
    if (stone.params[0] != Degree(target)) return code::kStoneMismatch;
    // Its Desc4 names the kinds it fits ("armor,shield" both).
    const std::string_view kinds = stone.paramTexts[3], kind = KindWord(target);
    bool fits = false;
    for (std::size_t at = 0; at <= kinds.size();) {
        const std::size_t comma = std::min(kinds.find(',', at), kinds.size());
        if (kinds.substr(at, comma - at) == kind) fits = true;
        at = comma + 1;
    }
    if (!fits) return code::kStoneMismatch;
    std::string_view field = stone.paramTexts[0];
    if (field.starts_with("NATTR_")) field.remove_prefix(6);
    const auto fields = WhiteFields(target);
    if (std::find(fields.begin(), fields.end(), field) == fields.end()) return code::kStoneMismatch;
    return 0;
}

StoneResult ApplyMagicStone(const assets::ItemRow& target, ItemInstance& item, const assets::ItemRow& stone, const ItemInstance& stoneItem,
                            const Options& options, int bonus, std::mt19937& random) {
    StoneResult r;
    r.option = stone.paramTexts[0];
    if (!StoneRoll(stone, bonus, random, r.chance)) return r;
    r.success = true;
    Assimilate(target, item, stoneItem.plus, -1, true, options, random, r);
    const int at = OptionIndex(item, options, r.option);
    if (IsProtection(r.option) || r.option == "MATTR_REPAIR") {
        // A charge more, at most 6 (Repair 7).
        const std::uint32_t most = r.option == "MATTR_REPAIR" ? 7 : 6;
        const std::uint32_t now = at >= 0 ? item.options[std::size_t(at)].value : 0;
        if (now >= most) {
            r.code = code::kChargesFull;
            return r;
        }
        r.value = now + 1;
    } else {
        const std::vector<std::uint32_t> values = OptionValues(options, r.option, stone.params[0]);
        if (values.empty()) {
            r.code = code::kRecipeMismatch;
            return r;
        }
        r.value = std::clamp<std::uint32_t>(values[std::size_t(Below(random, int(values.size())))], 1, 1700);
    }
    const assets::MagicOptionRow* row = OptionRow(options, r.option, stone.params[0]);
    const std::uint32_t id = row != nullptr ? row->paramId : (at >= 0 ? item.options[std::size_t(at)].id : 0);
    if (at >= 0) {
        item.options[std::size_t(at)] = {id, r.value};
    } else if (int(item.options.size()) < OptionCapacity(target, item, options) && id != 0) {
        item.options.push_back({id, r.value});
        r.added = true;
    } else {
        r.code = code::kChargesFull;
    }
    return r;
}

StoneResult ApplyAttrStone(const assets::ItemRow& target, ItemInstance& item, const assets::ItemRow& stone, const ItemInstance& stoneItem,
                           const Options& options, int bonus, std::mt19937& random) {
    StoneResult r;
    r.option = stone.paramTexts[0];
    if (!StoneRoll(stone, bonus, random, r.chance)) return r;
    r.success = true;
    std::string_view field = r.option;
    if (field.starts_with("NATTR_")) field.remove_prefix(6);
    const auto fields = WhiteFields(target);
    const auto it = std::find(fields.begin(), fields.end(), field);
    if (it == fields.end()) {
        r.code = code::kStoneMismatch;
        return r;
    }
    const int index = int(it - fields.begin());
    r.value = std::uint32_t(RollWhite(target, random));
    SetField(item, index, int(r.value));
    Assimilate(target, item, stoneItem.plus, index, false, options, random, r);
    return r;
}

// --- Making a stone ------------------------------------------------------------------------------------------------------

bool TabletRecipe(const assets::ItemTable& items, const assets::ItemRow& tablet, Recipe& out) {
    out = {};
    if (KindOf(tablet) != Kind::Tablet) return false;
    out.stone = items.Find(std::string_view(tablet.paramTexts[4]));
    for (std::size_t i = 0; i < 4; ++i) {
        if (tablet.paramTexts[i].empty() || tablet.params[i] <= 0) continue;
        const assets::ItemRow* element = items.Find(std::string_view(tablet.paramTexts[i]));
        if (element == nullptr) return false;
        out.elements.push_back({element, tablet.params[i]});
    }
    return out.stone != nullptr && !out.elements.empty();
}

int StoneRate(const assets::ItemRow& stone, bool dismantled, std::mt19937& random) {
    // From a tablet only a magic or an attribute stone (3/3/11/1, 3/3/11/2) is given one:
    // a protection stone, Sabakun's jewel (3/3/10/1, its Param2 the chance bytes) come with 0.
    if (!dismantled && !(stone.typeId[0] == 3 && stone.typeId[1] == 3 && stone.typeId[2] == 11 && (stone.typeId[3] == 1 || stone.typeId[3] == 2))) return 0;
    std::vector<int> rates;
    const auto bytes = [&](int param) {
        for (int b = 3; b >= 0; --b) {
            if (const int v = int((std::uint32_t(param) >> (8 * b)) & 0xFF); v != 0) rates.push_back(v);
        }
    };
    bytes(stone.params[1]);
    if (dismantled) bytes(stone.params[2]);
    return rates.empty() ? 0 : rates[std::size_t(Below(random, int(rates.size())))];
}

// --- Breaking down -------------------------------------------------------------------------------------------------------

int VoidRondos(const assets::ItemRow& material, int count) { return int(material.price / 1000 + 1) * std::max(0, count); }

std::vector<std::pair<const assets::ItemRow*, int>> DisjointYield(const assets::ItemTable& items, const assets::ItemRow& material, int count) {
    std::vector<std::pair<const assets::ItemRow*, int>> out;
    for (std::size_t i = 0; i < 4; ++i) {
        if (material.paramTexts[i].empty() || material.params[i] <= 0) continue;
        if (const assets::ItemRow* element = items.Find(std::string_view(material.paramTexts[i]))) out.push_back({element, material.params[i] * count});
    }
    return out;
}

int DestroyerRondos(const assets::ItemRow& equipment) { return int(equipment.price / 20000 + 1); }

std::uint16_t CheckDismantle(const assets::ItemRow& target, const ItemInstance& item, const Options& options) {
    if (target.typeId[0] == 3 && target.typeId[1] == 1 && target.typeId[2] == 13) return code::kAvatar;
    if (target.typeId[0] == 3 && target.typeId[1] == 1 && target.typeId[2] == 14) return code::kDevilSpirit;
    if (KindOf(target) != Kind::Equipment) return code::kCannotTarget;
    if (OptionIndex(item, options, "MATTR_NOT_REPARABLE") >= 0) return code::kRepairInvalid;
    if (OptionIndex(item, options, "MATTR_REPAIR") >= 0) return code::kHonorRondo;
    return 0;
}

DismantleYield Dismantle(const assets::ItemTable& items, const assets::ItemRow& target, const ItemInstance& item, const StoneBucket& bucket,
                         std::mt19937& random) {
    // The elements' and the stones' prices by degree.
    static constexpr double kElementPrice[12] = {75, 140, 215, 290, 365, 458, 551, 660, 776, 903, 1035, 1180};
    static constexpr double kStonePrice[12] = {18750, 34950, 53700, 72340, 91150, 114280, 137690, 164830, 193830, 225240, 258180, 294706};
    // Ordered pairs of different elements (0 EARTH, 1 WATER, 2 FIRE, 3 WIND), one picked by rand() % 12.
    static constexpr int kPairs[24] = {0, 1, 0, 2, 0, 3, 1, 0, 1, 2, 1, 3, 2, 0, 2, 1, 2, 3, 3, 0, 3, 1, 3, 2};
    DismantleYield out;
    const int degree = std::clamp(Degree(target), 1, 12);
    const double v = double(target.price), plus = double(item.plus);
    const auto add = [&](const assets::ItemRow* row, int count) {
        if (row == nullptr || count <= 0) return;
        for (auto& [r, n] : out.elements) {
            if (r == row) {
                n += count;
                return;
            }
        }
        out.elements.push_back({row, count});
    };
    const int first = Below(random, 12), second = Below(random, 12);
    for (int e = 0; e < 2; ++e) {
        const double share = double(Below(random, 6) + 15) / 100.0 + plus * 0.025;
        add(ElementOf(items, kPairs[2 * first + e], degree), int(std::trunc(share * v / kElementPrice[degree - 1])) + 1);
    }
    for (int e = 0; e < 2; ++e) {
        const double share = double(Below(random, 6) + 10) / 100.0 + plus * 0.020;
        add(ElementOf(items, kPairs[2 * second + e], degree), int(std::trunc(share * v / kElementPrice[degree - 1])) + 1);
    }
    const auto pick = [&](const std::vector<std::pair<const assets::ItemRow*, double>>& list) -> const assets::ItemRow* {
        double total = 0;
        for (const auto& [row, w] : list) total += w;
        if (total <= 0) return nullptr;
        double u = std::uniform_real_distribution<double>(0.0, total)(random);
        for (const auto& [row, w] : list) {
            if ((u -= w) <= 0 && w > 0) return row;
        }
        return list.back().first;
    };
    const int attrStones = std::min(2, int(std::trunc(double(Below(random, 6) + 5) * v / 100.0 / kStonePrice[degree - 1])));
    for (int i = 0; i < attrStones; ++i) {
        if (const assets::ItemRow* stone = pick(bucket.attr)) out.stones.push_back({stone, StoneRate(*stone, true, random)});
    }
    out.recipe = DismantleRecipe(items, target, item);
    const double options = double(item.options.size());
    const int magicStones = std::min(2, int(std::trunc(double(Below(random, 6) + 5) * options * v / 100.0 / kStonePrice[degree - 1])));
    for (int i = 0; i < magicStones; ++i) {
        if (const assets::ItemRow* stone = pick(bucket.magic)) out.stones.push_back({stone, StoneRate(*stone, true, random)});
    }
    return out;
}

// --- Sockets ---------------------------------------------------------------------------------------------------------------

namespace {
bool Typed(const assets::ItemRow& row, int tid3, int tid4) {
    return row.typeId[0] == 3 && row.typeId[1] == 3 && row.typeId[2] == tid3 && row.typeId[3] == tid4;
}
// The first slot's checks every socket operation makes: equipment and not a job suit, a
// Devil's Spirit only with its Param2 1.
std::uint16_t SocketFirstSlot(const assets::ItemRow& target) {
    const bool equipment = target.typeId[0] == 3 && target.typeId[1] == 1;
    if (!equipment || target.typeId[2] == 7) return code::kOnlyEquipment;
    if (target.typeId[2] == 14 && target.params[1] != 1) return code::kCannotReinforce;
    return 0;
}
bool ValidIndex(int index) { return index >= 0 && index < kSockets; }
} // namespace

bool IsSocketStone(const assets::ItemRow& row) { return Typed(row, 11, 8); }
bool IsSocketMaker(const assets::ItemRow& row) { return Typed(row, 11, 10); }
bool IsSocketEraser(const assets::ItemRow& row) { return Typed(row, 11, 9); }

int SocketCount(const assets::ItemRow& row) { return std::clamp(row.params[19], 0, kSockets); }

int MadeSockets(const ItemInstance& item) {
    int n = 0;
    for (const ItemSocket& s : item.sockets) n += s.made ? 1 : 0;
    return n;
}

std::uint16_t CheckSocketMake(const assets::ItemRow& target, const ItemInstance& item, int index) {
    if (!ValidIndex(index)) return code::kSocketSlot;
    if (const std::uint16_t c = SocketFirstSlot(target); c != 0) return c;
    if (KindOf(target) != Kind::Equipment) return code::kSocketNotStuff;
    if (target.params[19] <= 0) return code::kSocketNotAllowed;
    if (MadeSockets(item) >= target.params[19]) return code::kSocketsFull;
    if (item.sockets[std::size_t(index)].made) return code::kSocketMade;
    return 0;
}

std::uint16_t CheckSocketInsert(const assets::ItemRow& target, const ItemInstance& item, const assets::ItemRow& stone, int index) {
    if (!ValidIndex(index)) return code::kSocketSlot;
    if (const std::uint16_t c = SocketFirstSlot(target); c != 0) return c;
    if (!IsSocketStone(stone)) return code::kRecipeMissing;
    const ItemSocket& socket = item.sockets[std::size_t(index)];
    if (!socket.made) return code::kSocketMissing;
    if (socket.Filled()) return code::kSocketInUse;
    for (const ItemSocket& other : item.sockets) {
        if (other.Filled() && other.stone == stone.refObjId) return code::kSocketSameStone;
    }
    if (Degree(target) < stone.itemClass) return code::kSocketDegree;
    if (SocketStoneDisabled(stone)) return code::kSocketStoneKind;
    const int tid3 = target.typeId[2], tid4 = target.typeId[3];
    const bool armour = tid3 == 1 || tid3 == 2 || tid3 == 3 || tid3 == 9 || tid3 == 10 || tid3 == 11;
    const bool fits = stone.params[1] == 2 ? armour && tid4 == 3 : stone.params[1] == 1 ? tid3 == 6 || (tid4 >= 3 && tid4 <= 15) : false;
    if (!fits) return code::kSocketStoneKind;
    return 0;
}

bool SocketStoneDisabled(const assets::ItemRow& stone) { return stone.codename.starts_with("ITEM_ETC_SOCKET_STONE_DMRETURN"); }

std::uint16_t CheckSocketErase(const assets::ItemRow& target, const ItemInstance& item, int index) {
    if (!ValidIndex(index)) return code::kSocketSlot;
    if (const std::uint16_t c = SocketFirstSlot(target); c != 0) return c;
    if (!item.sockets[std::size_t(index)].Filled()) return code::kSocketEmpty;
    return 0;
}

std::vector<const assets::MagicOptGroupRow*> StoneSkills(const std::vector<assets::MagicOptGroupRow>& groups, const assets::ItemRow& stone) {
    std::vector<const assets::MagicOptGroupRow*> rows;
    for (const assets::MagicOptGroupRow& g : groups) {
        if (g.magicType == 3 && g.link == stone.params[0] && g.optId != 0) rows.push_back(&g);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) { return a->optLevel < b->optLevel; });
    return rows;
}

const assets::MagicOptGroupRow* RollSocketLevel(const std::vector<const assets::MagicOptGroupRow*>& rows, std::mt19937& random) {
    if (rows.empty()) return nullptr;
    const int n = int(rows.size());
    int least = n - 1;
    for (int k = 0; k < 3; ++k) least = std::min(least, Below(random, n));
    return rows[std::size_t(least)];
}

void MakeSocket(ItemInstance& item, int index) {
    if (ValidIndex(index)) item.sockets[std::size_t(index)] = {true, 0, 0, 0};
}

void InsertStone(ItemInstance& item, int index, std::uint32_t skill, int level, std::uint32_t stone) {
    if (ValidIndex(index)) item.sockets[std::size_t(index)] = {true, skill, std::uint8_t(std::clamp(level, 0, 255)), stone};
}

void EraseStone(ItemInstance& item, int index) {
    if (ValidIndex(index) && item.sockets[std::size_t(index)].made) item.sockets[std::size_t(index)] = {true, 0, 0, 0};
}

namespace {

// An option's name as the avatar's list spells it (case ignored); empty when it is not there.
std::string ListedName(const std::vector<std::string>& list, std::string_view option) {
    for (const std::string& name : list) {
        if (name.size() == option.size() && std::equal(name.begin(), name.end(), option.begin(), [](char a, char b) {
                return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b));
            })) {
            return name;
        }
    }
    return {};
}

} // namespace

bool IsRangeCode(std::uint32_t code) {
    static constexpr std::uint32_t kCodes[] = {0x73747276 /*strv*/, 0x696E7476 /*intv*/, 0x73747261 /*stra*/, 0x696E7461 /*inta*/,
                                               0x687261 /*hra*/,    0x657261 /*era*/,    0x687061 /*hpa*/,    0x6D7061 /*mpa*/,
                                               0x68706E61 /*hpna*/, 0x6D706E61 /*mpna*/, 0x6C756361 /*luca*/, 0x72656973 /*reis*/,
                                               0x64757261 /*dura*/, 0x64727561 /*drua*/, 0x64617261 /*dara*/, 0x6D646961 /*mdia*/,
                                               0x68707267 /*hprg*/, 0x6D707267 /*mprg*/};
    return std::find(std::begin(kCodes), std::end(kCodes), code) != std::end(kCodes);
}

std::uint32_t RangeValue(const assets::MagicOptionRow& row, std::mt19937& random) {
    const double low = double(row.rangeWords[1]), high = double(row.rangeWords[2]);
    if (high == low) return row.rangeWords[1];
    const double v = double(float(double(Below(random, 32768)) / 32767.0)) * (high - low) + low;
    return v <= 0.0 ? 0u : std::uint32_t(std::trunc(v));
}

bool IsGrantAvatar(const assets::ItemRow& row) {
    return row.typeId[0] == 3 && row.typeId[1] == 1 && row.typeId[2] == 13 && row.typeId[3] != 4;
}

std::vector<std::string> AvatarGrantList(const std::vector<assets::MagicOptionAssignRow>& assign, const assets::ItemRow& avatar) {
    std::vector<std::string> out;
    if (!IsGrantAvatar(avatar)) return out;
    for (const assets::MagicOptionAssignRow& a : assign) {
        if (a.tid3 != avatar.typeId[2] || a.tid4 != avatar.typeId[3] || (a.race != 3 && a.race != avatar.country)) continue;
        for (const std::string& name : a.options) {
            if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
        }
    }
    return out;
}

std::uint16_t CheckAvatarGrant(const assets::ItemRow& row, const ItemInstance& item, std::string_view option, const Options& options,
                               const std::vector<assets::MagicOptionAssignRow>& assign) {
    if (!IsGrantAvatar(row)) return code::kGrantNotAvatar;
    const int capacity = OptionCapacity(row, item, options);
    if (int(item.options.size()) > capacity) return code::kGrantMax;
    const std::string name = ListedName(AvatarGrantList(assign, row), option);
    if (name.empty() || std::none_of(options.begin(), options.end(), [&](const assets::MagicOptionRow& m) { return m.optionName == name; })) return code::kGrantWrong;
    const assets::MagicOptionRow* at = OptionRow(options, name, Degree(row));
    if (at == nullptr) return code::kGrantDegree;
    // (A charge at its most, 6: the avatars' lists hold none.)
    const bool held = std::any_of(item.options.begin(), item.options.end(), [&](const ItemInstance::Option& o) { return o.id == at->paramId; });
    if (!held && int(item.options.size()) >= capacity) return code::kGrantFull;
    return 0;
}

ItemInstance::Option GrantAvatarOption(const assets::ItemRow& row, ItemInstance& item, std::string_view option, const Options& options,
                                       std::mt19937& random) {
    // The option's own spelling (the request's case ignored).
    const assets::MagicOptionRow* named = nullptr;
    for (const assets::MagicOptionRow& m : options) {
        if (m.optionName.size() == option.size() && std::equal(m.optionName.begin(), m.optionName.end(), option.begin(), [](char a, char b) {
                return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b));
            })) {
            named = &m;
            break;
        }
    }
    const assets::MagicOptionRow* at = named != nullptr ? OptionRow(options, named->optionName, Degree(row)) : nullptr;
    if (at == nullptr) return {};
    std::uint32_t value = 0;
    if (IsRangeCode(at->attribute)) {
        value = RangeValue(*at, random);
    } else {
        const std::vector<std::uint32_t> values = OptionValues(options, at->optionName, at->degree);
        value = values.empty() ? 0u : values[std::size_t(Below(random, int(values.size())))];
    }
    value = std::clamp<std::uint32_t>(value, 1, 1700);
    const ItemInstance::Option written{at->paramId, value};
    const auto it = std::find_if(item.options.begin(), item.options.end(), [&](const ItemInstance::Option& o) { return o.id == at->paramId; });
    if (it != item.options.end()) *it = written;
    else item.options.push_back(written);
    return written;
}

void AddSocketSkills(const ItemInstance& item, std::vector<std::uint32_t>& out) {
    for (const ItemSocket& s : item.sockets) {
        if (s.Filled() && std::find(out.begin(), out.end(), s.skill) == out.end()) out.push_back(s.skill);
    }
}

} // namespace sr::game::alchemy
