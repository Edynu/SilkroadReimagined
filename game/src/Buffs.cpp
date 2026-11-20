#include "sr/game/Buffs.h"

#include <algorithm>
#include <cmath>

#include "sr/assets/GameData.h"

namespace sr::game {

namespace {

assets::SkillParam Tag(const assets::SkillRow& skill, const char* tag) { return assets::FindSkillParam(skill, assets::SkillTag(tag)); }

float F(std::uint32_t word) { return float(word); } // the keeper takes the words as unsigned (fild + 2^32)

// The attack lanes a mask names (bits 1 melee, 2 ranged, 4 physical, 8 magical), as 0..3.
template <typename Each>
void Lanes(std::uint32_t mask, Each each) {
    if ((mask & 4) != 0 && (mask & 1) != 0) each(0);
    if ((mask & 4) != 0 && (mask & 2) != 0) each(1);
    if ((mask & 8) != 0 && (mask & 1) != 0) each(2);
    if ((mask & 8) != 0 && (mask & 2) != 0) each(3);
}

} // namespace

std::vector<StatChange> BuffChanges(const assets::SkillRow& skill, const BuffBasis& basis, const BuffBonus& bonus) {
    std::vector<StatChange> out;
    const auto put = [&](std::uint16_t stat, std::uint8_t kind, float value) { out.push_back({stat, kind, value}); };
    // A toggle stops the natural MP recovery while it is on.
    if (Tag(skill, "onff")) put(stat::kMpRecovery, 3, 0.0f);
    // The experience bonuses.
    if (const auto t = Tag(skill, "expi")) {
        put(0xB9, 0, F(t[0]));
        put(0xBA, 0, F(t[1]));
    }
    if (const auto t = Tag(skill, "expu")) {
        if (t[0] != 0) put(0xBA, 0, F(t[0]));
        if (t[1] != 0) put(0xCA, 0, F(t[1]));
    }
    // STR and INT: the buff's bonus and the first word, at most the second word's percent of what
    // the target has, when it has one.
    const auto capped = [&](const assets::SkillParam& t, float now, float extra) {
        float value = extra + F(t[0]);
        if (t[1] != 0) {
            const float share = float(double(t[1]) / 100.0 * double(std::int32_t(std::trunc(now))));
            if (share < value) value = share;
        }
        return value;
    };
    if (const auto t = Tag(skill, "stri")) put(stat::kStr, 0, capped(t, basis.strength, bonus.strength));
    if (const auto t = Tag(skill, "inti")) put(stat::kInt, 0, capped(t, basis.intelligence, bonus.intelligence));
    // A share of the shield's physical defence moved into the physical attack.
    if (const auto t = Tag(skill, "spda"); t && basis.shieldDefence > 0.0f) {
        const int from = int(std::trunc(double(F(t[0])) * double(basis.shieldDefence) / 100.0));
        const int to = int(std::trunc(double(F(t[1])) * double(basis.shieldDefence) / 100.0));
        if (from > 0) put(stat::kPhyDefence, 0, -float(from));
        if (to > 0) {
            put(stat::kPhyAttackMin, 0, float(to));
            put(stat::kPhyAttackMax, 0, float(to));
        }
    }
    // Defence: flat amounts (with the buff's bonuses), each at most the third word's percent of the
    // defence when it has one.
    if (const auto t = Tag(skill, "defp")) {
        std::uint32_t pd = t[0] + std::uint32_t(std::max(0.0f, bonus.phyDefence)), md = t[1] + std::uint32_t(std::max(0.0f, bonus.magDefence));
        if (t[2] != 0) {
            const auto cap = [&](float defence) { return std::uint32_t(std::trunc(double(defence) * (double(t[2]) / 100.0))); };
            pd = std::min(pd, cap(basis.phyDefence));
            md = std::min(md, cap(basis.magDefence));
        }
        put(stat::kPhyDefence, 0, F(pd));
        put(stat::kMagDefence, 0, F(md));
    }
    if (const auto t = Tag(skill, "defr")) {
        put(stat::kPhyDefence, 1, F(t[0]));
        put(stat::kMagDefence, 1, F(t[1]));
    }
    if (const auto t = Tag(skill, "ar")) {
        put(stat::kPhyAbsorption, 0, F(t[0]));
        put(stat::kMagAbsorption, 0, F(t[1]));
    }
    if (const auto t = Tag(skill, "cr")) {
        put(stat::kCritical, 0, F(t[0]));
        put(stat::kCritical, 1, F(t[1]));
    }
    if (const auto t = Tag(skill, "hpi")) {
        put(stat::kMaxHp, 1, F(t[1]));
        put(stat::kMaxHp, 0, F(t[0]));
    }
    if (const auto t = Tag(skill, "mpi")) {
        put(stat::kMaxMp, 1, F(t[1]));
        put(stat::kMaxMp, 0, F(t[0]));
    }
    // The elements' grades and resistances by the mask's element bits (freeze .. zombie).
    if (const auto t = Tag(skill, "reat")) {
        for (std::uint16_t e = 0; e < 6; ++e) {
            if ((t[0] & (1u << e)) != 0) put(std::uint16_t(stat::kElementGrade + e), 0, F(t[1]));
        }
    }
    if (const auto t = Tag(skill, "bgra")) {
        for (std::uint16_t e = 0; e < 6; ++e) {
            if ((t[0] & (1u << e)) != 0) put(std::uint16_t(stat::kElementResist + e), 0, F(t[1]));
        }
    }
    if (const auto t = Tag(skill, "hr")) {
        put(stat::kHitRate, 1, F(t[1]));
        put(stat::kHitRate, 0, F(t[0]));
    }
    if (const auto t = Tag(skill, "er")) {
        put(stat::kParry, 0, F(t[0]));
        put(stat::kParry, 1, F(t[1]));
    }
    if (const auto t = Tag(skill, "ru")) put(stat::kRange, 0, F(t[0]));
    // Cuts (the Chinese water's curses on an enemy).
    if (const auto t = Tag(skill, "terd")) put(stat::kParry, 0, -F(t[0]));
    if (const auto t = Tag(skill, "thrd")) put(stat::kHitRate, 0, -F(t[0]));
    if (const auto t = Tag(skill, "tpdd")) put(stat::kPhyDefence, 1, -F(t[0]));
    if (const auto t = Tag(skill, "tpad")) {
        put(stat::kPhyAttackMin, 1, -F(t[0]));
        put(stat::kPhyAttackMax, 1, -F(t[0]));
    }
    if (const auto t = Tag(skill, "irgc")) {
        put(stat::kHpRecovery, 1, F(t[0]));
        put(stat::kMpRecovery, 1, F(t[1]));
    }
    // Damage by lane: physical, then magical; by a mask.
    if (const auto t = Tag(skill, "dru")) {
        put(std::uint16_t(stat::kDamage + 0), 0, F(t[0]));
        put(std::uint16_t(stat::kDamage + 1), 0, F(t[0]));
        put(std::uint16_t(stat::kDamage + 2), 0, F(t[1]));
        put(std::uint16_t(stat::kDamage + 3), 0, F(t[1]));
    }
    if (const auto t = Tag(skill, "dru2")) Lanes(t[0], [&](int lane) { put(std::uint16_t(stat::kDamage + lane), 0, F(t[1])); });
    // Attack: physical, then magical, flat and percent.
    if (const auto t = Tag(skill, "apau")) {
        put(stat::kPhyAttackMin, 0, F(t[0]));
        put(stat::kPhyAttackMax, 0, F(t[0]));
        put(stat::kMagAttackMin, 0, F(t[1]));
        put(stat::kMagAttackMax, 0, F(t[1]));
    }
    if (const auto t = Tag(skill, "apru")) {
        put(stat::kPhyAttackMin, 1, F(t[0]));
        put(stat::kPhyAttackMax, 1, F(t[0]));
        put(stat::kMagAttackMin, 1, F(t[1]));
        put(stat::kMagAttackMax, 1, F(t[1]));
    }
    if (const auto t = Tag(skill, "dcri")) put(stat::kCriticalResist, 0, F(t[0]));
    if (const auto t = Tag(skill, "br")) Lanes(t[0], [&](int lane) { put(std::uint16_t(stat::kBlockBonus + lane), 0, F(t[1])); });
    if (const auto t = Tag(skill, "dar")) Lanes(t[0], [&](int lane) { put(std::uint16_t(stat::kDamageAbsorption + lane), 0, F(t[1])); });
    // Less damage taken by lane: a factor.
    if (const auto t = Tag(skill, "odar")) Lanes(t[0], [&](int lane) { put(std::uint16_t(stat::kDamageTaken + lane), 2, -F(t[1])); });
    // The trade-off buffs' costs (their fourth word 2 in a buff): the most HP / MP less a
    // flat amount, else a percent; the final factors; the defences.
    const auto cost = [&](const char* tag) {
        const auto t = Tag(skill, tag);
        return t && t[3] == 2 ? t : assets::SkillParam{};
    };
    if (const auto t = cost("pmhp")) {
        if (t[1] != 0) {
            put(stat::kMaxHp, 0, -F(t[1]));
        } else if (t[2] != 0) {
            put(stat::kMaxHp, 1, -F(t[2]));
        }
    }
    if (const auto t = cost("pmmp")) {
        if (t[1] != 0) {
            put(stat::kMaxMp, 0, -F(t[1]));
        } else if (t[2] != 0) {
            put(stat::kMaxMp, 1, -F(t[2]));
        }
    }
    if (const auto t = cost("pmdg")) {
        put(std::uint16_t(stat::kFinalFactor + 0), 2, -F(t[1]));
        put(std::uint16_t(stat::kFinalFactor + 1), 2, -F(t[2]));
    }
    if (const auto t = cost("pmdp")) {
        put(stat::kPhyDefence, 1, -F(t[1]));
        put(stat::kMagDefence, 1, -F(t[2]));
    }
    // Numbers of other systems, kept as they are: HP/MP recovery bonuses 0xAA/0xAB, the gold
    // drop, alchemy, luck, the drop rate, the MP cost, ignoring defence, ...
    if (const auto t = Tag(skill, "rhru")) {
        put(0xAA, 0, F(t[0]));
        put(0xAB, 0, F(t[1]));
    }
    if (const auto t = Tag(skill, "gdr")) put(0xB6, 0, F(t[0]));
    if (const auto t = Tag(skill, "alcu")) put(0xAC, 0, F(t[0]));
    if (const auto t = Tag(skill, "luck")) put(0xAD, 0, F(t[0]));
    if (const auto t = Tag(skill, "hwir")) put(0xB8, 0, F(t[0]));
    if (const auto t = Tag(skill, "dcmp")) put(stat::kMpCost, 0, -F(t[0]));
    // Walking and running: a factor; a later speed tag of the buff takes its place.
    for (const char* tag : {"hste", "hst2", "hst3"}) {
        if (const auto t = Tag(skill, tag)) {
            put(stat::kWalkSpeed, 2, F(t[0]));
            put(stat::kRunSpeed, 2, F(t[0]));
        }
    }
    if (const auto t = Tag(skill, "msid")) put(stat::kIgnoreDefence, 0, F(t[0]));
    if (const auto t = Tag(skill, "spi")) put(0xC2, 0, F(t[0]));
    if (const auto t = Tag(skill, "hwit")) put(0xC3, 0, F(t[0]));
    if (const auto t = Tag(skill, "msr")) {
        put(0xC4, 0, F(t[0]));
        put(0xC5, 0, F(t[1]));
    }
    if (const auto t = Tag(skill, "bldr")) {
        put(0xC6, 0, F(t[0]));
        put(0xC7, 0, F(t[1]));
        put(0xC8, 0, F(t[2]));
    }
    return out;
}

std::vector<std::uint8_t> OverlapGroups(const assets::SkillRow& skill) {
    std::vector<std::uint8_t> out;
    const auto add = [&](std::uint32_t packed) {
        for (int shift = 0; shift < 24; shift += 8) { // the low three bytes (the top one is the wait's group)
            const std::uint8_t g = std::uint8_t(packed >> shift);
            if (g != 0 && g != 0x1D && g != 0x23 && std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
        }
    };
    add(skill.overlap);
    if (const auto t = Tag(skill, "ovl2")) add(t[0]);
    return out;
}

BuffAdmission AdmitBuff(const assets::SkillRow& incoming, std::span<const assets::SkillRow* const> running) {
    BuffAdmission a;
    const auto family = Tag(incoming, "dttp");
    // The running buffs (not a scroll's, 'cbuf'): the same 'dttp' family or skill group.
    for (std::size_t i = 0; i < running.size(); ++i) {
        const assets::SkillRow* r = running[i];
        if (r == nullptr || Tag(*r, "cbuf")) continue;
        const auto theirs = Tag(*r, "dttp");
        if (family && theirs && family[0] == theirs[0]) {
            if (family[1] < theirs[1]) return a; // a lower rank: refused
            a.admitted = true;
            a.replaces.push_back(i);
            return a;
        }
        if (Tag(incoming, "lks2")) continue;
        if (incoming.group != 0 && incoming.group == r->group) {
            if (incoming.level < r->level) return a; // a lower level: refused
            a.admitted = true;
            a.replaces.push_back(i);
            return a;
        }
    }
    // Otherwise an overlap group held by one running refuses it.
    const std::vector<std::uint8_t> mine = OverlapGroups(incoming);
    for (const assets::SkillRow* r : running) {
        if (r == nullptr) continue;
        for (const std::uint8_t g : OverlapGroups(*r)) {
            if (std::find(mine.begin(), mine.end(), g) != mine.end()) return a;
        }
    }
    a.admitted = true;
    return a;
}

} // namespace sr::game
