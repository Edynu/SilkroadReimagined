#include "sr/game/Stats.h"

#include <algorithm>
#include <cmath>

#include "sr/game/Combat.h"

namespace sr::game {

void StatSheet::Add(std::uint32_t source, const StatChange& change) {
    for (Entry& e : entries_) {
        if (e.source == source && e.change.stat == change.stat && e.change.kind == change.kind) {
            e.change.value = change.value;
            return;
        }
    }
    entries_.push_back({source, change});
}

void StatSheet::Add(std::uint32_t source, const std::vector<StatChange>& changes) {
    for (const StatChange& c : changes) Add(source, c);
}

bool StatSheet::Remove(std::uint32_t source) {
    const std::size_t before = entries_.size();
    std::erase_if(entries_, [&](const Entry& e) { return e.source == source; });
    return entries_.size() != before;
}

bool StatSheet::Changes(std::uint16_t stat) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.change.stat == stat; });
}

bool BlueSums(std::uint32_t code) { return code == 0x73747261 || code == 0x696E7461 || code == 0x6D646961 || code == 0x6C756361; }

std::vector<StatChange> BlueChanges(std::uint32_t code, std::uint32_t value, float ownBlock) {
    const float v = float(value); // the option's value read as unsigned (2^32 added to a negative)
    const auto one = [&](std::uint16_t stat, std::uint8_t kind) { return std::vector<StatChange>{{stat, kind, v}}; };
    const auto four = [&](std::uint16_t first, std::uint8_t kind, float value4) {
        std::vector<StatChange> out;
        for (std::uint16_t lane = 0; lane < 4; ++lane) out.push_back({std::uint16_t(first + lane), kind, value4});
        return out;
    };
    switch (code) {
    case 0x737472:                // str
    case 0x7374726A:              // strj
    case 0x73747276: return one(stat::kStr, 0); // strv
    case 0x696E74:                               // int
    case 0x696E7476: return one(stat::kInt, 0);  // intv (MATTR_INT_AVATAR)
    case 0x73747261: return one(stat::kStr, 0);  // stra (MATTR_AVATAR_STR*: summed, BlueSums)
    case 0x696E7461: return one(stat::kInt, 0);  // inta (MATTR_AVATAR_INT*: summed)
    case 0x6D646961: return one(stat::kIgnoreDefence, 0); // mdia (MATTR_AVATAR_MDIA*: the monster defence ignored, summed)
    case 0x6C756361: return one(0xAD, 0);        // luca (MATTR_AVATAR_LUCK*: the strengthening's luck, summed)
    case 0x6870:                  // hp
    case 0x687061: return one(stat::kMaxHp, 0); // hpa
    case 0x6D70:                  // mp
    case 0x6D7061: return one(stat::kMaxMp, 0); // mpa
    case 0x687261: return one(stat::kHitRate, 1);  // hra (the avatars')
    case 0x657261: return one(stat::kParry, 1);    // era (the avatars')
    case 0x637269: return one(stat::kCritical, 0); // cri
    case 0x6576626C: return one(stat::kBlockIgnore, 0);    // evbl
    case 0x65766372: return one(stat::kCriticalResist, 0); // evcr
    case 0x68707267: return one(stat::kHpRecovery, 1);     // hprg
    case 0x6D707267: return one(stat::kMpRecovery, 1);     // mprg
    case 0x68706D70: return {{stat::kHpRecovery, 1, v}, {stat::kMpRecovery, 1, v}}; // hpmp
    case 0x68706E61: return one(stat::kHpFromStr, 1); // hpna: the HP from STR, a percent
    case 0x6D706E61: return one(stat::kMpFromInt, 1); // mpna: the MP from INT, a percent
    case 0x627269:                                     // bri (MATTR_BLOCKRATE)
    case 0x62726E61: {                                 // brna (MATTR_NASRUN_BLOCKRATE)
        // trunc(the item's own block, 0 when broken) + the value, as unsigned, in the white entry's place.
        const std::int64_t own = std::int64_t(std::trunc(ownBlock));
        return {{stat::kBlock, 0, float(double(std::uint32_t(own + std::int64_t(value))) - double(ownBlock))}};
    }
    case 0x756D6475: return one(stat::kUniqueDamage, 0); // umdu (MATTR_NASRUN_UMDU): flat
    case 0x64727561: return four(stat::kDamage, 0, v);         // drua: damage dealt, every lane
    case 0x64617261: return four(stat::kDamageTaken, 2, -v);   // dara: damage taken cut, every lane (negated)
    default: return {};
    }
}

float StatSheet::Value(std::uint16_t stat, float base) const {
    // The four buckets: the flat amounts and the percents
    // summed, the factors multiplied as 1 + v / 100, the percents of it multiplied as v / 100 (from 100).
    float flat = base, percent = 0.0f, factor = 0.0f, of = 100.0f;
    bool any = false;
    for (const Entry& e : entries_) {
        if (e.change.stat != stat) continue;
        any = true;
        switch (e.change.kind) {
        case 0: flat += e.change.value; break;
        case 1: percent += e.change.value; break;
        case 2: {
            const float f = e.change.value / 100.0f + 1.0f;
            factor = factor == 0.0f ? f : f * factor;
            break;
        }
        default: of = e.change.value * of / 100.0f; break;
        }
    }
    if (!any) return base;
    // the flat times 100 + the percents over 100; then the
    // factors (alone when that came to 0); then the percents of it.
    float value = float(double(flat) * double(percent + 100.0f) / 100.0);
    if (factor != 0.0f) value = value == 0.0f ? factor : value * factor;
    return float(double(of) / 100.0 * double(value));
}

void ApplySheet(CombatStats& s, const StatSheet& sheet) {
    if (sheet.Empty()) return;
    const auto put = [&](double& field, std::uint16_t stat) {
        if (sheet.Changes(stat)) field = double(sheet.Value(stat, float(field)));
    };
    put(s.phyAttackMin, stat::kPhyAttackMin);
    put(s.phyAttackMax, stat::kPhyAttackMax);
    put(s.magAttackMin, stat::kMagAttackMin);
    put(s.magAttackMax, stat::kMagAttackMax);
    put(s.phyDefence, stat::kPhyDefence);
    put(s.magDefence, stat::kMagDefence);
    put(s.phyAbsorption, stat::kPhyAbsorption);
    put(s.magAbsorption, stat::kMagAbsorption);
    put(s.evasion, stat::kParry);
    put(s.block, stat::kBlock);
    put(s.hitRate, stat::kHitRate);
    put(s.critical, stat::kCritical);
    put(s.criticalResist, stat::kCriticalResist);
    put(s.blockIgnore, stat::kBlockIgnore);
    put(s.ignoreDefenceChance, stat::kIgnoreDefence);
    put(s.classBonus, stat::kUniqueDamage);
    put(s.berserkBonus, stat::kBerserk);
    put(s.healReceived[0], 0xAA);
    put(s.healReceived[1], 0xAB);
    if (sheet.Changes(stat::kMpCost)) s.mpCostPercent = std::max(0.0f, sheet.Value(stat::kMpCost, 100.0f));
    for (std::uint16_t i = 0; i < 4; ++i) {
        put(s.damagePercent[i], std::uint16_t(stat::kDamage + i));
        put(s.blockBonus[i], std::uint16_t(stat::kBlockBonus + i));
        put(s.damageTaken[i], std::uint16_t(stat::kDamageTaken + i));
    }
    for (std::uint16_t i = 0; i < 2; ++i) {
        put(s.finalFactor[i], std::uint16_t(stat::kFinalFactor + i));
        put(s.finalTakenFactor[i], std::uint16_t(stat::kFinalTaken + i));
    }
    if (sheet.Changes(stat::kRange)) s.range = sheet.Value(stat::kRange, s.range);
    if (sheet.Changes(stat::kMaxHp)) s.maxHp = std::uint32_t(std::max(1.0f, sheet.Value(stat::kMaxHp, float(s.maxHp))));
    if (sheet.Changes(stat::kMaxMp)) s.maxMp = std::uint32_t(std::max(0.0f, sheet.Value(stat::kMaxMp, float(s.maxMp))));
}

} // namespace sr::game
