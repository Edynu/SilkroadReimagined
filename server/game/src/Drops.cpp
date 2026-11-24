#include "sr/server/Drops.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "sr/assets/GameData.h"
#include "sr/core/Log.h"
#include "sr/game/Alchemy.h"

namespace sr::server {

namespace {

namespace alchemy = game::alchemy;

// Option codes (`_RefMagicOpt.Param1`).
constexpr std::uint32_t kCodeNrep = 0x6E726570, kCodeRein = 0x7265696E;
// A try that comes again ('nrep' in a list, an accessory in a unique's enhanced loop) has no end in the original when
// nothing else can come; ours stops after this many (never reached with the 1.188 data).
constexpr int kRetryGuard = 1000;

// The original's rand(): 0..32767.
int Rand(std::mt19937& random) { return alchemy::Below(random, 32768); }

// A row's weight in its option's million roll: trunc(float Prob x 1,000,000).
std::uint32_t Weight(const assets::MagicOptionRow& row) {
    const double w = std::trunc(double(row.prob) * 1'000'000.0);
    return w <= 0.0 ? 0u : w >= 4.0e9 ? 4'000'000'000u : std::uint32_t(w);
}

// An option's row by the million roll: r = ((rand << 15) |
// rand()) % 1,000,000 against the running sum of its rows' weights in ID order, the first whose sum is at least r.
const assets::MagicOptionRow* MillionRoll(const std::vector<const assets::MagicOptionRow*>& rows, std::mt19937& random) {
    const std::uint32_t low = std::uint32_t(Rand(random)), high = std::uint32_t(Rand(random));
    const std::uint32_t r = ((high << 15) | low) % 1'000'000u;
    std::uint32_t sum = 0;
    for (const assets::MagicOptionRow* row : rows) {
        sum += Weight(*row);
        if (r <= sum) return row;
    }
    return nullptr;
}

// A value of a row as its option's value kind makes one: 'nrep' and 'rein' 0; the range
// codes trunc(Param3 + float(rand / 32767) x (Param4 - Param3)) (no drop reaches it in 1.188);
// every other one of the row's list, each alike: the non-zero u16 halves of Param2, Param3, Param4, the high half first
// (the stones' list), 0 when it is empty.
std::uint32_t PickValue(const assets::MagicOptionRow& row, std::mt19937& random) {
    if (row.attribute == kCodeNrep || row.attribute == kCodeRein) return 0;
    if (alchemy::IsRangeCode(row.attribute)) return alchemy::RangeValue(row, random); // the avatars' codes, dura, reis
    std::vector<std::uint32_t> values;
    if (row.hasRange) {
        for (const std::uint32_t word : row.rangeWords) {
            if (const std::uint32_t high = word >> 16; high != 0) values.push_back(high);
            if (const std::uint32_t low = word & 0xFFFF; low != 0) values.push_back(low);
        }
    }
    return values.empty() ? 0u : values[std::size_t(alchemy::Below(random, int(values.size())))];
}

// A list's key: TID4 << 16 | TID3.
std::uint32_t ListKey(int tid3, int tid4) { return std::uint32_t(tid4 & 0xFF) << 16 | std::uint32_t(tid3 & 0xFF); }

bool IsEquipment(const assets::ItemRow& row) {
    const game::ItemKind kind = game::KindOf(row);
    return kind == game::ItemKind::Weapon || kind == game::ItemKind::Shield || kind == game::ItemKind::Armour || kind == game::ItemKind::Accessory;
}

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> cells;
    std::size_t start = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', start);
        cells.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    if (!cells.empty() && !cells.back().empty() && cells.back().back() == '\r') cells.back().pop_back();
    return cells;
}

int ToInt(const std::string& s) { return std::atoi(s.c_str()); }
double ToDouble(const std::string& s) { return std::atof(s.c_str()); }

double Uniform(std::mt19937& random) { return std::uniform_real_distribution<double>(0.0, 1.0)(random); }
int Between(std::mt19937& random, int low, int high) {
    return high <= low ? low : std::uniform_int_distribution<int>(low, high)(random);
}

} // namespace

bool DropTable::Load(const std::string& path, const assets::ItemTable& items, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    std::stringstream text;
    text << file.rdbuf();
    return LoadText(text.str(), items, error);
}

bool DropTable::LoadText(const std::string& text, const assets::ItemTable& items, std::string& error) {
    std::istringstream in(text);
    items_ = &items;
    const auto item = [&](const std::string& code) { return items.Find(std::string_view(code)); };
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> c = Split(line);
        const std::string& kind = c[0];
        if (kind == "budget" && c.size() >= 5) {
            budgets_[ToInt(c[1])] = {ToInt(c[2]), ToInt(c[3]), ToInt(c[4])};
        } else if (kind == "budget-party" && c.size() >= 2) {
            partyAttempts_ = ToInt(c[1]);
        } else if (kind == "budget-monster" && c.size() >= 5) {
            monsterBudgets_[c[1]] = {ToInt(c[2]), ToInt(c[3]), ToInt(c[4])};
        } else if (kind == "gold-multiplier" && c.size() >= 3) {
            goldMultipliers_[c[1]] = ToDouble(c[2]);
        } else if (kind == "plus" && c.size() >= 3) {
            const std::size_t at = std::size_t(ToInt(c[1]));
            if (plusCumulative_.size() <= at) plusCumulative_.resize(at + 1, 1.0);
            plusCumulative_[at] = ToDouble(c[2]);
        } else if (kind == "category" && c.size() >= 4) {
            Category cat{c[2], {}};
            std::stringstream alternatives(c[3]);
            for (std::string a; std::getline(alternatives, a, '|');) {
                std::vector<std::string>& families = cat.alternatives.emplace_back();
                std::stringstream names(a);
                for (std::string f; std::getline(names, f, ',');) families.push_back(f);
            }
            const std::size_t at = std::size_t(ToInt(c[1]));
            if (categories_.size() <= at) categories_.resize(at + 1);
            categories_[at] = std::move(cat);
        } else if (kind == "equip-class" && c.size() >= 5) {
            (c[2] == "rare" ? rareClasses_ : normalClasses_)[ToInt(c[1])].push_back({ToInt(c[3]), ToDouble(c[4])});
        } else if (kind == "equip-item" && c.size() >= 8) {
            const assets::ItemRow* row = item(c[4]);
            const int country = ToInt(c[3]);
            if (row == nullptr || country < 0 || country > 1) continue;
            (c[1] == "rare" ? rareItems_ : normalItems_)[country][ToInt(c[2])].push_back(
                {row, ToDouble(c[5]), std::min(1.0, (ToDouble(c[6]) + 1.0) / 100.0), 1, ToInt(c[7])});
        } else if (kind == "consumable-class" && c.size() >= 5) {
            consumableClasses_[c[1]][ToInt(c[2])].push_back({ToInt(c[3]), ToDouble(c[4])});
        } else if (kind == "consumable-item" && c.size() >= 7) {
            const assets::ItemRow* row = item(c[3]);
            if (row == nullptr) continue;
            consumableItems_[c[1]][ToInt(c[2])].push_back({row, ToDouble(c[4]), std::min(1.0, (ToDouble(c[5]) + 1.0) / 100.0), std::max(1, ToInt(c[6])), 0});
        } else if (kind == "assigned" && c.size() >= 7) {
            if (const assets::ItemRow* row = item(c[2])) assigned_[c[1]].push_back({row, ToDouble(c[3]), ToInt(c[4]), ToInt(c[5]), ToInt(c[6])});
        } else if (kind == "material" && c.size() >= 6) {
            if (const assets::ItemRow* row = item(c[2])) materials_[c[1]].push_back({row, ToDouble(c[3]), ToInt(c[4]), ToInt(c[5]), 0});
        } else if (kind == "assigned-group" && c.size() >= 7) {
            assignedGroups_[c[1]].push_back({ToInt(c[2]), ToDouble(c[3]), ToInt(c[4]), ToInt(c[5]), ToInt(c[6]) != 0});
        } else if (kind == "group-item" && c.size() >= 4) {
            // The fifth column (ours, from the database): the member's RefMagicGroupID.
            if (const assets::ItemRow* row = item(c[2])) groupItems_[ToInt(c[1])].push_back({row, ToDouble(c[3]), c.size() >= 5 ? ToInt(c[4]) : 0});
        } else if (kind == "magic-group" && c.size() >= 4) {
            magicGroups_[ToInt(c[1])].push_back({std::uint32_t(ToInt(c[2])), std::uint32_t(std::max(0, ToInt(c[3])))});
        } else if (kind == "unique-count" && c.size() >= 5) {
            UniqueCounts& u = uniqueCounts_[c[1]];
            const std::pair<int, int> range{ToInt(c[3]), ToInt(c[4])};
            if (c[2] == "equipment") u.equipment = range;
            else if (c[2] == "repairable") u.repairable = range;
            else if (c[2] == "recipe") u.recipe = range;
            else if (c[2] == "scroll") u.scroll = range;
        } else if (kind == "profile" && c.size() >= 7) {
            Profile& p = profiles_[c[1]][ToInt(c[2])];
            const int command = ToInt(c[3]), group = ToInt(c[4]);
            const std::pair<int, int> params{ToInt(c[5]), ToInt(c[6])};
            std::optional<std::pair<int, int>>* slot = nullptr;
            if (command == 1 && group == 11) slot = &p.gold;
            else if (command == 1 && group == 12) slot = &p.tries;
            else if (command == 2 && group == 0) slot = &p.equipment;
            else if (command == 2 && group == 6) slot = &p.elixirs;
            else if (command == 2 && group == 10) slot = &p.tablets;
            else if (command == 2 && group == 14) slot = &p.scrolls;
            else if (command == 3 && group == 1) slot = &p.rare;
            if (slot != nullptr && !*slot) *slot = params; // a group's first record
        } else if (kind == "profile-option" && c.size() >= 5) {
            // The sixth column (ours): the option's Param3, the durability's raise; a file without it raises by the value.
            const std::uint32_t value = std::uint32_t(std::max(0, ToInt(c[4])));
            profileOptions_[c[1]].push_back({std::uint32_t(ToInt(c[2])), ToInt(c[3]), value, c.size() >= 6 ? std::uint32_t(std::max(0, ToInt(c[5]))) : value});
        } else if (kind == "unique-item" && c.size() >= 3) {
            const assets::ItemRow* row = item(c[2]);
            if (row == nullptr) continue;
            if (c[1] == "recipe") uniqueRecipes_.push_back(row);
            else uniqueScroll_ = row;
        }
    }
    for (ClassTable* table : {&normalClasses_, &rareClasses_}) {
        for (auto& [level, row] : *table) std::sort(row.begin(), row.end());
    }
    for (auto& [family, table] : consumableClasses_) {
        for (auto& [level, row] : table) std::sort(row.begin(), row.end());
    }
    if (budgets_.empty()) {
        error = "the drop records hold no budgets";
        return false;
    }
    return true;
}

const DropTable::ClassRow* DropTable::Classes(const ClassTable& table, int level) const {
    const auto it = table.find(level);
    return it == table.end() ? nullptr : &it->second;
}

const DropTable::ClassRow* DropTable::ClassesAtOrBelow(const ClassTable& table, int level) const {
    for (int l = level; l >= 1; --l) {
        if (const auto it = table.find(l); it != table.end()) return &it->second;
    }
    return nullptr;
}

int DropTable::RollGroup(const ClassRow& row, double scale, std::mt19937& random) const {
    double total = 0;
    for (const auto& [group, p] : row) total += p;
    if (total * scale > 1.0 && total > 0) scale = 1.0 / total; // a rate past certainty: the row as a whole once
    const double u = Uniform(random);
    double running = 0;
    for (const auto& [group, p] : row) {
        running += p * scale;
        if (running >= u) return group;
    }
    return -1;
}

int DropTable::RollPlus(std::mt19937& random) const {
    const double u = Uniform(random);
    for (std::size_t i = 0; i < plusCumulative_.size(); ++i) {
        if (plusCumulative_[i] >= u) return int(i);
    }
    return 0;
}

game::ItemInstance DropTable::Instance(const assets::ItemRow& row, int count, int plus, std::mt19937& random) const {
    game::ItemInstance item;
    item.refId = row.refObjId;
    item.plus = std::uint8_t(std::clamp(plus, 0, 255));
    item.count = std::uint16_t(std::clamp(count, 1, std::max(1, game::MaxStack(row))));
    if (game::KindOf(row) == game::ItemKind::Weapon || game::KindOf(row) == game::ItemKind::Armour || game::KindOf(row) == game::ItemKind::Shield ||
        game::KindOf(row) == game::ItemKind::Accessory) {
        // White stats: each the least of three rolls 0..31 (weapons 7, armour and shields 6,
        // accessories 2), five bits each from the lowest.
        const int kind = row.typeId[2];
        const int fields = kind == 6 ? 7 : (kind == 5 || kind == 12) ? 2 : 6;
        for (int f = 0; f < fields; ++f) {
            const double least = std::min({Uniform(random), Uniform(random), Uniform(random)});
            item.variance |= std::uint64_t(std::min(31, int(least * 31.0))) << (5 * f);
        }
        const game::ItemStats stats = game::ComputeStats(row, item, nullptr, 0, true);
        item.durability = stats.durability.high > 0 ? std::uint32_t(stats.durability.high) : game::NewDurability(row);
        item.count = 1;
    }
    return item;
}

void DropTable::SetMagicOptions(std::shared_ptr<const std::vector<assets::MagicOptionRow>> options, const std::vector<assets::MagicOptionAssignRow>& assign) {
    magicOptions_ = std::move(options);
    families_.clear();
    familyIndex_.clear();
    for (auto& lists : blueLists_) lists.clear();
    notReparable_ = nullptr;
    if (magicOptions_ == nullptr) return;
    // Each option's rows in ID order (`_RefMagicOpt` in service by MOptName128).
    std::vector<const assets::MagicOptionRow*> rows;
    for (const assets::MagicOptionRow& row : *magicOptions_) rows.push_back(&row);
    std::stable_sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) { return a->paramId < b->paramId; });
    for (const assets::MagicOptionRow* row : rows) {
        const auto [it, added] = familyIndex_.try_emplace(row->optionName, int(families_.size()));
        if (added) families_.emplace_back();
        families_[std::size_t(it->second)].push_back(row);
    }
    // The lists: by Race, then TID4 << 16 | TID3; an option already in a list is not put in again.
    int unknown = 0;
    for (const assets::MagicOptionAssignRow& a : assign) {
        if (a.race < 0 || a.race > 3) continue;
        std::vector<int>& list = blueLists_[std::size_t(a.race)][ListKey(a.tid3, a.tid4)];
        for (const std::string& name : a.options) {
            const auto it = familyIndex_.find(name);
            if (it == familyIndex_.end()) {
                ++unknown; // the original would hold a null option there; ours leaves it out
                continue;
            }
            if (std::find(list.begin(), list.end(), it->second) == list.end()) list.push_back(it->second);
        }
    }
    if (unknown > 0) core::LogWarning("drops: {} options of magicoptionassign.txt are not in magicoption.txt (left out)", unknown);
    // 'nrep' (MATTR_NOT_REPARABLE, its level-1 row).
    if (const auto it = familyIndex_.find("MATTR_NOT_REPARABLE"); it != familyIndex_.end()) {
        for (const assets::MagicOptionRow* row : families_[std::size_t(it->second)]) {
            if (row->degree == 1) notReparable_ = row;
        }
    }
    // The drop groups' options the table lacks (the original logs each when the piece drops).
    for (const auto& [link, list] : magicGroups_) {
        for (const GroupOption& o : list) {
            if (game::FindOption(*magicOptions_, o.option) == nullptr) core::LogWarning("drops: magic group {}'s option {} is not in magicoption.txt", link, o.option);
        }
    }
}

bool DropTable::RollBlues(const assets::ItemRow& row, game::ItemInstance& item, std::mt19937& random) const {
    // 1. The options cleared.
    item.options.clear();
    if (magicOptions_ == nullptr) return false;
    // 2. The gate: equipment of Rarity exactly 2 always (not 6: a rare set piece rolls), any other
    // piece when rand() % 101 <= 30.
    const bool sealed = row.typeId[0] == 3 && row.typeId[1] == 1 && row.rarity == 2;
    if (!sealed && alchemy::Below(random, 101) > 30) return false;
    // 3. The count: the least of MaxMagicOptCount and four rolls 1..6, 1..6, within MaxMagicOptCount.
    int count = row.maxMagicOptions;
    for (int i = 0; i < 4; ++i) count = std::min(count, alchemy::Below(random, 6) + 1);
    count = std::clamp(count, 1, 6);
    if (row.maxMagicOptions >= 1) count = std::min(count, row.maxMagicOptions);
    // 4. The list by the piece's country, TID3, TID4; none: no blues.
    if (row.country < 0 || row.country > 3) return false;
    const auto& lists = blueLists_[std::size_t(row.country)];
    const auto list = lists.find(ListKey(row.typeId[2], row.typeId[3]));
    if (list == lists.end()) return false;
    const std::vector<int>& families = list->second;
    // 5. The tries.
    std::vector<int> written; // the list's indices written
    int retries = 0;
    for (int t = 0; t < count; ++t) {
        if (families.empty()) continue;
        const int index = alchemy::Below(random, int(families.size()));
        if (std::find(written.begin(), written.end(), index) != written.end()) continue; // the try lost
        const assets::MagicOptionRow* option = MillionRoll(families_[std::size_t(families[std::size_t(index)])], random);
        if (option == nullptr) continue; // no row came (Repair: Prob 0), the index left drawable
        if (option->attribute == kCodeNrep) {
            if (++retries < kRetryGuard) --t; // the try again
            continue;
        }
        if (!alchemy::RowFits(*option, row)) continue; // the groups and ReqClass
        if (item.options.size() >= 12) break;
        item.options.push_back({option->paramId, PickValue(*option, random)});
        written.push_back(index);
    }
    // 6. MagParamNum the options written; the answer 1 even when every try was lost.
    return true;
}

DropTable::Special DropTable::SpecialPiece(int group, const assets::ObjectRow& monster, std::mt19937& random, Drop& out) const {
    if (!MakeEquipment(group, false, monster, random, out)) return Special::None;
    const assets::ItemRow* row = items_ != nullptr ? items_->Find(out.item.refId) : nullptr;
    if (row == nullptr) return Special::None;
    if (game::KindOf(*row) == game::ItemKind::Accessory) return Special::Accessory;
    out.item.plus = std::uint8_t(std::min(Between(random, 1, 5), Between(random, 1, 5)));
    return RollBlues(*row, out.item, random) ? Special::Made : Special::Refused;      // releases it
}

void DropTable::AddNotReparable(const assets::ItemRow& row, game::ItemInstance& item) const {
    if (notReparable_ == nullptr || int(item.options.size()) >= row.maxMagicOptions) return;
    if (row.typeId[0] == 3 && row.typeId[1] == 1 && row.rarity == 2) return; // a seal never
    item.options.push_back({notReparable_->paramId, 0});                    // the 'nrep' kind's pick: 0
    // The durability + trunc(it x Param3 / 100): x 5 with Param3 400.
    const std::uint64_t raise = std::uint64_t(item.durability) * notReparable_->rangeWords[1] / 100;
    item.durability = std::uint32_t(std::min<std::uint64_t>(std::uint64_t(item.durability) + raise, 0xFFFFFFFFu));
}

bool DropTable::MakeEquipment(int group, bool rare, const assets::ObjectRow& monster, std::mt19937& random, Drop& out) const {
    int country = monster.country;
    if (country == 3) country = Uniform(random) < 0.5 ? 0 : 1;
    if (country != 0 && country != 1) return false;
    const auto& buckets = (rare ? rareItems_ : normalItems_)[country];
    const int step = rare ? 3 : 1;
    const int level = std::max(1, monster.level);
    for (int g = group; g >= 0; g -= step) {
        const auto it = buckets.find(g);
        if (it == buckets.end() || it->second.empty()) continue; // an empty class: the next one down
        // Drawn by weight among all of the class's items.
        double total = 0;
        for (const Candidate& c : it->second) total += c.weight;
        if (total <= 0) continue;
        double u = Uniform(random) * total;
        const Candidate* chosen = nullptr;
        for (const Candidate& c : it->second) {
            chosen = &c;
            if ((u -= c.weight) <= 0) break;
        }
        // Above the monster's level: the next class down, an item of the same type drawn evenly from it, until one fits;
        // a lower class without that type: nothing; past the last class the last drawn is kept.
        for (int lower = g - step; chosen != nullptr && chosen->requiredLevel > level && lower >= 0; lower -= step) {
            const auto down = buckets.find(lower);
            std::vector<const Candidate*> same;
            if (down != buckets.end()) {
                for (const Candidate& c : down->second) {
                    if (c.row->typeId == chosen->row->typeId) same.push_back(&c);
                }
            }
            if (same.empty()) return false;
            chosen = same[std::size_t(Uniform(random) * double(same.size())) % same.size()];
        }
        if (chosen == nullptr || Uniform(random) > chosen->admit) return false;
        out.item = Instance(*chosen->row, 1, RollPlus(random), random);
        return true;
    }
    return false;
}

std::vector<DropTable::Drop> DropTable::Roll(const assets::ObjectRow& monster, std::uint8_t rarity, int playerLevel, const GameRates& rates,
                                             std::mt19937& random) const {
    std::vector<Drop> planned;
    const int grade = rarity & 15;
    const bool party = (rarity >> 4) == 1;
    Budget budget = budgets_.contains(grade) ? budgets_.at(grade) : budgets_.contains(0) ? budgets_.at(0) : Budget{};
    if (const auto it = monsterBudgets_.find(monster.codename); it != monsterBudgets_.end()) budget = it->second;
    // Its profile for this kind: the passes, the tries (the kinds' multipliers replaced), the gold, the extras.
    const Profile* profile = nullptr;
    if (const auto it = profiles_.find(monster.codename); it != profiles_.end()) {
        if (const auto kind = it->second.find(grade); kind != it->second.end()) profile = &kind->second;
    }
    if (profile != nullptr) {
        budget.passes = profile->gold ? profile->gold->second : profile->tries ? profile->tries->second : budget.passes;
        budget.attempts = profile->tries ? profile->tries->first : 1;
    }
    if (party) budget.attempts *= partyAttempts_;
    const double classScale = profile != nullptr && profile->rare ? double(std::max(1, profile->rare->second)) : 1.0;
    const int level = std::max(1, monster.level);

    // 1. A profile's extras; else a unique's: each stage the monster's own, else grade 8's, else the default's.
    if (profile != nullptr) {
        ProfileExtras(*profile, monster, level, random, planned);
    } else if (grade == 3 || grade == 8) {
        const auto stage = [&](std::optional<std::pair<int, int>> UniqueCounts::*member) -> std::optional<std::pair<int, int>> {
            for (const std::string& key : {monster.codename, std::string(grade == 8 ? "grade8" : ""), std::string("default")}) {
                const auto it = uniqueCounts_.find(key);
                if (!key.empty() && it != uniqueCounts_.end() && it->second.*member) return it->second.*member;
            }
            return std::nullopt;
        };
        const auto equipmentCount = stage(&UniqueCounts::equipment), repairableCount = stage(&UniqueCounts::repairable);
        const auto recipeCount = stage(&UniqueCounts::recipe), scrollCount = stage(&UniqueCounts::scroll);
        if (equipmentCount || repairableCount || recipeCount || scrollCount) {
            const std::pair<int, int> none{0, 0};
            int lowest = -1;
            if (const ClassRow* row = ClassesAtOrBelow(normalClasses_, level)) {
                for (const auto& [group, p] : *row) {
                    if (p > 0) {
                        lowest = group;
                        break;
                    }
                }
            }
            // The enhanced pieces: a piece the blues refuse is lost, an
            // accessory drawn again; 'nrep' after the blues. Then the ordinary pieces: one the
            // blues refuse lost, an accessory too.
            const auto [eLow, eHigh] = equipmentCount.value_or(none);
            const int enhanced = Between(random, eLow, eHigh);
            int accessories = 0;
            for (int i = 0; i < enhanced && lowest >= 0; ++i) {
                Drop d;
                const Special made = SpecialPiece(lowest, monster, random, d);
                if (made == Special::Accessory && ++accessories < kRetryGuard) {
                    --i;
                    continue;
                }
                if (made != Special::Made) continue;
                if (const assets::ItemRow* row = items_ != nullptr ? items_->Find(d.item.refId) : nullptr) AddNotReparable(*row, d.item);
                planned.push_back(std::move(d));
            }
            const auto [rLow, rHigh] = repairableCount.value_or(none);
            for (int i = Between(random, rLow, rHigh); i > 0 && lowest >= 0; --i) {
                Drop d;
                if (SpecialPiece(lowest, monster, random, d) == Special::Made) planned.push_back(std::move(d));
            }
            const auto [cLow, cHigh] = recipeCount.value_or(none);
            for (int i = Between(random, cLow, cHigh); i > 0 && !uniqueRecipes_.empty(); --i) {
                planned.push_back({Instance(*uniqueRecipes_[std::size_t(Between(random, 0, int(uniqueRecipes_.size()) - 1))], 1, 0, random), 0});
            }
            const auto [sLow, sHigh] = scrollCount.value_or(none);
            for (int i = Between(random, sLow, sHigh); i > 0 && uniqueScroll_ != nullptr; --i) {
                planned.push_back({Instance(*uniqueScroll_, 1, 0, random), 0});
            }
        }
    }

    // 2. Its assigned drops: groups, fixed rows, its own materials.
    if (const auto it = assignedGroups_.find(monster.codename); it != assignedGroups_.end()) {
        for (const AssignedGroup& g : it->second) {
            if (Uniform(random) > g.chance * rates.rare) continue;
            const auto members = groupItems_.find(g.group);
            if (members == groupItems_.end() || members->second.empty()) continue;
            std::vector<GroupItem> pool = members->second;
            for (int n = Between(random, g.min, g.max); n > 0 && !pool.empty(); --n) {
                double total = 0;
                for (const GroupItem& m : pool) total += m.ratio;
                double u = Uniform(random) * total;
                std::size_t pick = 0;
                for (; pick + 1 < pool.size(); ++pick) {
                    if ((u -= pool[pick].ratio) <= 0) break;
                }
                Drop d{Instance(*pool[pick].row, 1, 0, random), 0};
                // An equipment piece: its magic group's MagicType 1 rows, in their order: the row's value, or one of its
                // option's own when 0; no gate, no capacity, no
                // group check, no random blues. An option the table lacks is left out.
                if (IsEquipment(*pool[pick].row) && magicOptions_ != nullptr) {
                    if (const auto group = magicGroups_.find(pool[pick].magicGroup); group != magicGroups_.end()) {
                        for (const GroupOption& o : group->second) {
                            const assets::MagicOptionRow* option = game::FindOption(*magicOptions_, o.option);
                            if (option == nullptr || d.item.options.size() >= 12) continue;
                            d.item.options.push_back({option->paramId, o.value != 0 ? o.value : PickValue(*option, random)});
                        }
                    }
                }
                planned.push_back(std::move(d));
                if (g.distinct) pool.erase(pool.begin() + std::ptrdiff_t(pick)); // Overlap 1
            }
        }
    }
    if (const auto it = assigned_.find(monster.codename); it != assigned_.end()) {
        for (const Assigned& a : it->second) {
            for (int t = Between(random, a.minTries, a.maxTries); t > 0; --t) {
                if (Uniform(random) <= a.chance) planned.push_back({Instance(*a.row, 1, a.plus, random), 0});
            }
        }
    }
    if (const auto it = materials_.find(monster.codename); it != materials_.end()) {
        for (const Assigned& m : it->second) {
            for (int t = Between(random, m.minTries, m.maxTries); t > 0; --t) {
                if (Uniform(random) < m.chance * rates.materials) planned.push_back({Instance(*m.row, 1, 0, random), 0});
            }
        }
    }

    // 3. The passes: gold, equipment, the consumable categories, until the capacity.
    const auto full = [&] { return int(planned.size()) >= budget.capacity; };
    for (int pass = 0; pass < budget.passes && !full(); ++pass) {
        if (std::size_t(level) < levelGold_.size() && Uniform(random) < std::min(1.0, rates.goldChance)) {
            const auto [low, high] = levelGold_[std::size_t(level)];
            double amount = double(low) + std::floor(Uniform(random) * double(high - low));
            // The kind's multiplier (a profile's own in place of the uniques' and giants', rarity 7's kept).
            if (profile == nullptr || grade == 7) {
                if (const auto m = goldMultipliers_.find(std::to_string(grade)); m != goldMultipliers_.end()) amount *= m->second;
            }
            if (profile != nullptr) amount *= profile->gold ? double(profile->gold->first) : 1.0;
            if (party) {
                if (const auto m = goldMultipliers_.find("party"); m != goldMultipliers_.end()) amount *= m->second;
            }
            amount *= rates.gold;
            if (amount >= 1.0) planned.push_back({{}, std::uint64_t(amount)});
        }
        if (full()) break;
        // Rare equipment about one pass in a thousand; a profile's (3, 1) record: when rand() % P1 == 1.
        const bool rare = profile != nullptr && profile->rare ? Between(random, 0, std::max(1, profile->rare->first) - 1) == 1
                                                              : Uniform(random) < std::min(1.0, 33.0 / 32768.0 * rates.rare);
        if (const ClassRow* row = Classes(rare ? rareClasses_ : normalClasses_, level)) {
            // Tries until one gives an item.
            for (int attempt = 0; attempt < budget.attempts; ++attempt) {
                const int group = RollGroup(*row, (rare ? 1.0 : rates.equipment) * classScale, random);
                if (group < 0) continue;
                Drop d;
                if (!MakeEquipment(group, rare, monster, random, d)) continue;
                // Its blues after the plus and the whites; a piece without them drops all the same.
                if (const assets::ItemRow* piece = items_ != nullptr ? items_->Find(d.item.refId) : nullptr) RollBlues(*piece, d.item, random);
                planned.push_back(std::move(d));
                break;
            }
        }
        for (const Category& cat : categories_) {
            if (full()) break;
            if (cat.alternatives.empty()) continue;
            const auto pick = [&](const auto& list) -> const auto& {
                return list[list.size() == 1 ? 0 : std::size_t(Between(random, 0, int(list.size()) - 1))];
            };
            const std::vector<std::string>& families = pick(cat.alternatives);
            if (families.empty()) continue;
            const std::string& family = pick(families);
            const double scale = cat.name == "elixir" ? rates.elixirs : (cat.name == "stone" || cat.name == "tablet") ? rates.materials : rates.potions;
            const auto table = consumableClasses_.find(family);
            const auto bucket = consumableItems_.find(family);
            if (table == consumableClasses_.end() || bucket == consumableItems_.end()) continue;
            const ClassRow* row = Classes(table->second, level);
            if (row == nullptr) continue;
            // Tries until one gives an item (a class without an item, or an item not kept, tries again).
            for (int attempt = 0; attempt < budget.attempts; ++attempt) {
                const int group = RollGroup(*row, scale * classScale, random);
                if (group < 0) continue;
                const auto it = bucket->second.find(group);
                if (it == bucket->second.end() || it->second.empty()) continue;
                double total = 0;
                for (const Candidate& c : it->second) total += c.weight;
                double u = Uniform(random) * total;
                const Candidate* chosen = &it->second.front();
                for (const Candidate& c : it->second) {
                    chosen = &c;
                    if ((u -= c.weight) <= 0) break;
                }
                if (Uniform(random) > chosen->admit) continue;
                planned.push_back({Instance(*chosen->row, chosen->count, 0, random), 0});
                break;
            }
        }
    }

    // 4. Kept by the level gap.
    const int gap = playerLevel - level;
    int threshold = gap > 6 ? int(std::trunc((1.0 - 0.04 * double(gap)) * 100.0)) : 100;
    if (grade == 7) threshold = std::min(100, threshold * 4);
    std::vector<Drop> kept;
    for (Drop& d : planned) {
        if (threshold >= 0 && Between(random, 0, 100) <= threshold) kept.push_back(std::move(d));
    }
    return kept;
}

std::vector<std::pair<const assets::ItemRow*, double>> DropTable::Consumables(const std::string& family, int group) const {
    std::vector<std::pair<const assets::ItemRow*, double>> out;
    const auto it = consumableItems_.find(family);
    if (it == consumableItems_.end()) return out;
    const auto bucket = it->second.find(group);
    if (bucket == it->second.end()) return out;
    for (const Candidate& c : bucket->second) out.push_back({c.row, c.weight});
    return out;
}

void DropTable::ProfileExtras(const Profile& profile, const assets::ObjectRow& monster, int level, std::mt19937& random,
                              std::vector<Drop>& planned) const {
    const bool rarity8 = monster.rarity == 8; // the reference's Rarity byte (the temple gods' bodies)
    // (2, 0): equipment pieces of the level's class: an accessory
    // is lost, the plus the least of two rolls 1..5, the blues, a piece they refuse lost (each counts). Then, after the
    // blues, the command-4 options at their chances: each written with the record's value (0: its
    // option's own pick, 0 for 'nrep'); 'nrep' on a piece under its MaxMagicOptCount and not of Rarity 2 also raises its
    // durability by the option's Param3 in percent (x 5), and only such a raise keeps a rarity-8 monster's piece.
    if (profile.equipment) {
        int lowest = -1;
        if (const ClassRow* row = ClassesAtOrBelow(normalClasses_, level)) {
            for (const auto& [group, p] : *row) {
                if (p > 0) {
                    lowest = group;
                    break;
                }
            }
        }
        const std::vector<ProfileOption>* options = nullptr;
        if (const auto it = profileOptions_.find(monster.codename); it != profileOptions_.end()) options = &it->second;
        for (int n = Between(random, profile.equipment->first, profile.equipment->second); n > 0 && lowest >= 0; --n) {
            Drop d;
            if (SpecialPiece(lowest, monster, random, d) != Special::Made) continue;
            const assets::ItemRow* row = items_ != nullptr ? items_->Find(d.item.refId) : nullptr;
            if (row == nullptr) continue;
            bool boosted = false;
            for (const ProfileOption& o : options != nullptr ? *options : std::vector<ProfileOption>{}) {
                if (Between(random, 0, 100) >= o.chance) continue;
                const assets::MagicOptionRow* option = magicOptions_ != nullptr ? game::FindOption(*magicOptions_, o.id) : nullptr;
                if (magicOptions_ != nullptr && option == nullptr) continue; // no such option
                const bool notReparable = option == nullptr || option->optionName == "MATTR_NOT_REPARABLE";
                const int before = int(d.item.options.size());
                if (notReparable && before < row->maxMagicOptions && !(row->typeId[1] == 1 && row->rarity == 2)) {
                    const std::uint64_t raise = std::uint64_t(d.item.durability) * o.raise / 100;
                    d.item.durability = std::uint32_t(std::min<std::uint64_t>(std::uint64_t(d.item.durability) + raise, 0xFFFFFFFFu));
                    boosted = true;
                }
                if (before >= 12) continue;
                d.item.options.push_back({o.id, o.value != 0 ? o.value : option != nullptr ? PickValue(*option, random) : 0u});
            }
            if (rarity8 && !boosted) continue;
            planned.push_back(std::move(d));
        }
    }
    // (2, 6): B elixirs, each one of the four, no roll.
    if (profile.elixirs && !uniqueRecipes_.empty()) {
        for (int n = Between(random, profile.elixirs->first, profile.elixirs->second); n > 0; --n) {
            planned.push_back({Instance(*uniqueRecipes_[std::size_t(Between(random, 0, int(uniqueRecipes_.size()) - 1))], 1, 0, random), 0});
        }
    }
    // (2, 10): tablets of the level's first class, weighted; the first that fails ends them.
    if (profile.tablets) {
        const auto table = consumableClasses_.find("tablet");
        const auto bucket = consumableItems_.find("tablet");
        const ClassRow* row = table != consumableClasses_.end() ? Classes(table->second, level) : nullptr;
        int group = -1;
        for (const auto& [g, p] : row != nullptr ? *row : ClassRow{}) {
            if (p > 0) {
                group = g;
                break;
            }
        }
        const auto items = bucket != consumableItems_.end() && group >= 0 ? bucket->second.find(group) : decltype(bucket->second.end()){};
        for (int n = Between(random, profile.tablets->first, profile.tablets->second); n > 0; --n) {
            if (bucket == consumableItems_.end() || group < 0 || items == bucket->second.end() || items->second.empty()) break;
            double total = 0;
            for (const Candidate& c : items->second) total += c.weight;
            double u = Uniform(random) * total;
            const Candidate* chosen = &items->second.front();
            for (const Candidate& c : items->second) {
                chosen = &c;
                if ((u -= c.weight) <= 0) break;
            }
            if (Uniform(random) > chosen->admit) break;
            planned.push_back({Instance(*chosen->row, chosen->count, 0, random), 0});
        }
    }
    // (2, 14): the special return scrolls, none for a rarity-8 monster.
    if (profile.scrolls && uniqueScroll_ != nullptr && !rarity8) {
        for (int n = Between(random, profile.scrolls->first, profile.scrolls->second); n > 0; --n) planned.push_back({Instance(*uniqueScroll_, 1, 0, random), 0});
    }
}

} // namespace sr::server
