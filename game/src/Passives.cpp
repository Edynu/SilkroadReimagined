#include "sr/game/Passives.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

#include "sr/assets/GameData.h"
#include "sr/game/Items.h"

namespace sr::game {

namespace {

using assets::SkillTag;

// The state resistance buckets' bits in a 'real' mask, bucket by bucket (sleep .. time bomb; the original tests them in
// this order).
constexpr std::uint32_t kRealBits[kResistBuckets] = {0x40,    0x80,    0x100,    0x200,    0x400,    0x800,    0x2000,   0x4000,   0x8000,
                                                     0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000, 0x400000, 0x1000000};

// The codes a skill names with 'getv', in order.
std::vector<std::uint32_t> Reads(const assets::SkillRow& skill) {
    std::vector<std::uint32_t> out;
    assets::ForEachSkillParam(skill, [&](std::uint32_t code, std::span<const std::uint32_t> args) {
        if (code == SkillTag("getv") && !args.empty()) out.push_back(args[0]);
    });
    return out;
}

} // namespace

WornKinds WornKindsOf(const std::vector<WornItem>& worn) {
    WornKinds k;
    for (const WornItem& w : worn) {
        if (w.row == nullptr) continue;
        if (w.item != nullptr && w.item->durability == 0 && NewDurability(*w.row) != 0) continue; // broken
        const auto& t = w.row->typeId;
        if (w.place == slot::kWeapon && KindOf(*w.row) == ItemKind::Weapon) {
            k.weapon = t[3];
        } else if (w.place == slot::kShield && KindOf(*w.row) == ItemKind::Shield) {
            k.shield = t[3];
        } else if (KindOf(*w.row) == ItemKind::Armour && t[3] >= 1 && t[3] <= 6) {
            k.armour[std::size_t(t[3])] = t[2];
        }
    }
    return k;
}

bool EquipmentFits(const assets::SkillRow& skill, const WornKinds& worn) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> wants;
    bool every = false;
    assets::ForEachSkillParam(skill, [&](std::uint32_t code, std::span<const std::uint32_t> args) {
        if (code == SkillTag("reqi") && wants.size() < 5) wants.push_back({args.size() > 0 ? args[0] : 0u, args.size() > 1 ? args[1] : 0u});
        if (code == SkillTag("reqn")) every = true;
    });
    if (wants.empty()) return true;
    // One condition: the weapon's or the shield's sort, an armour kind on all
    // six parts (sort 0) or something on the part of that number (its sort is the part).
    const auto holds = [&](std::uint32_t kind, std::uint32_t sort) {
        switch (kind) {
        case 6: return worn.weapon != 0 && std::uint32_t(worn.weapon) == sort;
        case 4: return worn.shield != 0 && std::uint32_t(worn.shield) == sort;
        case 1:
        case 2:
        case 3:
        case 9:
        case 10:
        case 11:
            if (sort == 0) {
                for (std::size_t part = 1; part <= 6; ++part) {
                    if (std::uint32_t(worn.armour[part]) != kind) return false;
                }
                return true;
            }
            return sort <= 6 && worn.armour[sort] != 0;
        // The Devil's Spirit's skills (reqi 14 1): an item in avatar place 4, not expired
        // and the sort 1 (the buff's own install compares the item's
        // TID4 instead and asks nothing of the time: the same for the spirit, the only item there).
        case 14: return sort == 1 && worn.spirit;
        default: return false;
        }
    };
    if (every) {
        return std::all_of(wants.begin(), wants.end(), [&](const auto& w) { return holds(w.first, w.second); });
    }
    return std::any_of(wants.begin(), wants.end(), [&](const auto& w) { return holds(w.first, w.second); });
}

void SkillModifiers::Set(std::uint32_t code, int value) {
    for (auto& [c, v] : values_) {
        if (c == code) {
            v = value;
            return;
        }
    }
    values_.push_back({code, value});
}

std::optional<int> SkillModifiers::Find(std::uint32_t code) const {
    for (const auto& [c, v] : values_) {
        if (c == code) return v;
    }
    return std::nullopt;
}

SkillBoosts BoostsOf(const assets::SkillRow& skill, bool magical, const SkillModifiers& modifiers) {
    SkillBoosts b;
    const std::vector<std::uint32_t> reads = Reads(skill);
    if (reads.empty() || modifiers.Empty()) return b;
    const auto named = [&](const char* code) { return std::find(reads.begin(), reads.end(), SkillTag(code)) != reads.end(); };
    const auto value = [&](const char* code) -> std::optional<int> {
        if (!named(code)) return std::nullopt;
        return modifiers.Find(SkillTag(code));
    };
    // The first of the codes the skill names decides (one it names without a value: none of them).
    const auto first = [&](std::initializer_list<const char*> codes) -> std::optional<int> {
        for (const char* code : codes) {
            if (named(code)) return modifiers.Find(SkillTag(code));
        }
        return std::nullopt;
    };
    const auto factor = [](int v) { return 1.0 + double(v) / 100.0; };
    if (magical) {
        // The magical variance: each hit rate in turn, each attack percent in turn.
        for (const char* code : {"DGHR", "E2AH"}) {
            if (const auto v = value(code)) b.spread += *v;
        }
        for (const char* code : {"EAAT", "COAT", "FIAT", "LIAT", "DTAT", "BLAT", "MUAT", "HLAT"}) {
            if (const auto v = value(code)) b.pointFactor *= factor(*v);
        }
    } else {
        // The physical one: the first hit rate it names, the first attack percent.
        if (const auto v = first({"DGHR", "E2AH"})) b.spread = *v;
        if (const auto v = first({"DGAT", "CBAT", "E1SA", "E2SA", "E2AA"})) b.pointFactor = factor(*v);
    }
    if (const auto v = value("SAAA")) b.fixedDamage = *v;
    if (const auto v = value("BSHP")) b.lifeSteal = *v;
    for (const char* code : {"CBRA", "WIRU"}) {
        if (const auto v = value(code)) b.range += *v;
    }
    for (const char* code : {"WIMD", "BDMD", "HLMD"}) {
        if (const auto v = value(code)) b.manaCuts.push_back(*v);
    }
    if (const auto v = value("BDMD")) b.upkeepCut = *v;
    // Its effect's time: the poison blade's or stealth's milliseconds, else the curses' percent.
    if (named("RPBU") || named("STDU")) {
        if (const auto v = first({"RPBU", "STDU"})) b.extraMs = std::uint32_t(std::max(0, *v));
    } else if (const auto v = value("DTDR")) {
        b.extraPercent = *v;
    }
    if (const auto v = value("RPTU")) b.poisonLevel = *v;
    if (const auto v = value("RPDU")) b.poisonDamage = *v;
    if (const auto v = value("HLRU")) b.heal = *v;
    // The blesses: STR, INT, then the physical defence's, else the magical's.
    if (const auto v = value("HLFS")) b.bonus.strength = float(*v);
    if (const auto v = value("HLMI")) b.bonus.intelligence = float(*v);
    if (named("HLBP")) {
        if (const auto v = value("HLBP")) b.bonus.phyDefence = float(*v);
    } else if (const auto v = value("HLSM")) {
        b.bonus.magDefence = float(*v);
    }
    return b;
}

std::uint32_t CutMana(std::uint32_t mana, const std::vector<int>& cuts) {
    for (const int cut : cuts) mana = std::uint32_t(std::max(0.0, std::trunc((1.0 - double(cut) / 100.0) * double(mana))));
    return mana;
}

std::uint32_t ExtendedMs(std::uint32_t durationMs, const SkillBoosts& boosts) {
    // The effect ends when its 'dura' and the extra have passed.
    if (boosts.extraMs > 0) return durationMs + boosts.extraMs;
    if (boosts.extraPercent != 0) {
        const double extra = std::trunc(double(boosts.extraPercent) / 100.0 * double(durationMs));
        return std::uint32_t(std::max(0.0, double(durationMs) + extra));
    }
    return durationMs;
}

void Passives::AddTo(StatSheet& sheet) const {
    for (const Entry& e : entries) {
        if (e.active) sheet.Add(kPassiveSource | (e.skill & 0xFFFFFFu), e.changes);
    }
}

Passives PassivesOf(const SkillBook& book, const assets::SkillTable& table, const WornKinds& worn, const BuffBasis& basis) {
    // Each learned group's row at its level, of Activity 0, in the order of their ids.
    std::vector<const assets::SkillRow*> rows;
    for (const auto& [group, level] : book.groups) {
        const assets::SkillRow* row = table.GroupLevel(group, level);
        if (row != nullptr && row->inService && row->activity == 0) rows.push_back(row);
    }
    // The passives of what it wears (its sockets' stones): running while worn, through the
    // same buff routine.
    for (const std::uint32_t id : book.itemSkills) {
        const assets::SkillRow* row = table.Find(id);
        if (row != nullptr && row->inService && row->activity == 0 && std::find(rows.begin(), rows.end(), row) == rows.end()) rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(), [](const assets::SkillRow* a, const assets::SkillRow* b) { return a->id < b->id; });
    Passives out;
    for (const assets::SkillRow* row : rows) {
        Passives::Entry e;
        e.skill = row->id;
        e.active = EquipmentFits(*row, worn);
        e.changes = BuffChanges(*row, basis);
        if (e.active) {
            // Its named values, and its state resistances (the highest grade, its highest flat amount;
            // ours: the containers' order is not traced).
            assets::ForEachSkillParam(*row, [&](std::uint32_t code, std::span<const std::uint32_t> args) {
                if (code == SkillTag("setv") && args.size() >= 2) out.modifiers.Set(args[0], int(args[1]));
                if (code == SkillTag("real") && args.size() >= 3) {
                    const int flat = int(args[1]), grade = int(args[2]);
                    for (std::size_t b = 0; b < kResistBuckets; ++b) {
                        if ((args[0] & kRealBits[b]) == 0) continue;
                        if (grade > out.resistGrade[b] || (grade == out.resistGrade[b] && flat > out.resistFlat[b])) {
                            out.resistGrade[b] = grade;
                            out.resistFlat[b] = flat;
                        }
                    }
                }
            });
        }
        out.entries.push_back(std::move(e));
    }
    return out;
}


StateDefence PlayerStateDefence(int level, const std::vector<WornItem>& worn, const std::vector<assets::MagicOptionRow>* options,
                                const Passives* passives) {
    StateDefence d;
    d.level = level;
    if (passives != nullptr) {
        d.flat = passives->resistFlat;
        d.grade = passives->resistGrade;
    }
    if (options == nullptr) return d;
    // The options by name: the elements (freeze .. zombie, StateDefence::elementResist's order) and the buckets.
    struct ElementOption {
        const char* name;
        int first, second; // the elements it raises (-1 none)
    };
    constexpr ElementOption kElements[] = {{"MATTR_RESIST_FROSTBITE", 0, 1}, {"MATTR_RESIST_ESHOCK", 2, -1}, {"MATTR_RESIST_BURN", 3, -1},
                                           {"MATTR_RESIST_POISON", 4, -1},  {"MATTR_RESIST_ZOMBIE", 5, -1}};
    struct BucketOption {
        const char* name;
        int bucket; // sleep 0, root 1, slow 2, fear 3, .. stun 7, disease 8, .. combustion 15
    };
    constexpr BucketOption kBuckets[] = {{"MATTR_RESIST_SLEEP", 0}, {"MATTR_RESIST_FEAR", 3}, {"MATTR_RESIST_STUN", 7},
                                         {"MATTR_RESIST_DISEASE", 8}, {"MATTR_RESIST_CSMP", 15}};
    for (const WornItem& w : worn) {
        if (w.item == nullptr) continue;
        for (const ItemInstance::Option& o : w.item->options) {
            const assets::MagicOptionRow* row = FindOption(*options, o.id);
            if (row == nullptr) continue;
            for (const ElementOption& e : kElements) {
                if (row->optionName != e.name) continue;
                d.elementResist[std::size_t(e.first)] += int(o.value);
                if (e.second >= 0) d.elementResist[std::size_t(e.second)] += int(o.value);
            }
            for (const BucketOption& b : kBuckets) {
                if (row->optionName == b.name) d.percent[std::size_t(b.bucket)] += int(o.value);
            }
        }
    }
    return d;
}

} // namespace sr::game
