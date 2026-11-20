// Buffs (K4): the parameter keeper's sums (game/Stats.h),
// what each buff tag changes and which buff may stand beside which (game/Buffs.h), each as an exact number.

#include <doctest/doctest.h>

#include <initializer_list>

#include "sr/assets/GameData.h"
#include "sr/game/Buffs.h"
#include "sr/game/Combat.h"
#include "sr/game/Stats.h"

using namespace sr;
namespace ps = game::stat;

namespace {

// A skill row of tags (each a code then its words, as the tail holds them).
assets::SkillRow Skill(std::uint32_t group, int level, std::initializer_list<std::uint32_t> tail, std::uint32_t overlap = 0) {
    assets::SkillRow s;
    s.id = group * 100 + std::uint32_t(level);
    s.group = group;
    s.level = level;
    s.overlap = overlap;
    s.params.assign(tail.begin(), tail.end());
    return s;
}

std::uint32_t T(const char* tag) { return assets::SkillTag(tag); }

float Of(const std::vector<game::StatChange>& changes, std::uint16_t s, std::uint8_t kind) {
    for (const game::StatChange& c : changes) {
        if (c.stat == s && c.kind == kind) return c.value;
    }
    return -12345.0f;
}

} // namespace

TEST_CASE("buffs: the keeper sums a parameter's changes by kind") {
    game::StatSheet sheet;
    CHECK(sheet.Value(ps::kPhyDefence, 500.0f) == 500.0f);
    sheet.Add(1, {ps::kPhyDefence, 0, 100.0f}); // flat
    sheet.Add(2, {ps::kPhyDefence, 1, 20.0f});  // percent
    sheet.Add(3, {ps::kPhyDefence, 1, -10.0f});
    CHECK(sheet.Value(ps::kPhyDefence, 500.0f) == doctest::Approx(600.0 * 1.10));
    // A factor multiplies as 1 + v / 100, a percent of it as v / 100.
    sheet.Add(4, {ps::kPhyDefence, 2, 50.0f});
    sheet.Add(5, {ps::kPhyDefence, 3, 50.0f});
    CHECK(sheet.Value(ps::kPhyDefence, 500.0f) == doctest::Approx(600.0 * 1.10 * 1.5 * 0.5));
    // A source's change of the same kind replaces its earlier one; removing a source takes all of it.
    sheet.Add(1, {ps::kPhyDefence, 0, 300.0f});
    CHECK(sheet.Value(ps::kPhyDefence, 500.0f) == doctest::Approx(800.0 * 1.10 * 1.5 * 0.5));
    CHECK(sheet.Remove(4));
    CHECK(sheet.Remove(5));
    CHECK_FALSE(sheet.Remove(5));
    CHECK(sheet.Value(ps::kPhyDefence, 500.0f) == doctest::Approx(800.0 * 1.10));
    // A value that comes to 0 before the factors is the factors alone (a damage-taken factor's 0 base).
    game::StatSheet taken;
    taken.Add(7, {ps::kDamageTaken, 2, -20.0f});
    taken.Add(8, {ps::kDamageTaken, 2, -10.0f});
    CHECK(taken.Value(ps::kDamageTaken, 0.0f) == doctest::Approx(0.8 * 0.9));
}

TEST_CASE("buffs: what each tag changes") {
    game::BuffBasis basis{100.0f, 80.0f, 1000.0f, 500.0f, 400.0f};
    // STR and INT: the first word, at most the second's percent of what it has.
    auto c = game::BuffChanges(Skill(1, 1, {T("stri"), 50, 30, T("inti"), 20, 50}), basis);
    CHECK(Of(c, ps::kStr, 0) == 30.0f); // 30 % of 100 under 50
    CHECK(Of(c, ps::kInt, 0) == 20.0f); // 20 under 50 % of 80
    // Defence: flat, each at most the third word's percent of the defence; percents; absorption.
    c = game::BuffChanges(Skill(1, 1, {T("defp"), 300, 300, 40, T("defr"), 10, 20, T("ar"), 5, 6}), basis);
    CHECK(Of(c, ps::kPhyDefence, 0) == 300.0f); // under 40 % of 1000
    CHECK(Of(c, ps::kMagDefence, 0) == 200.0f); // 40 % of 500
    CHECK(Of(c, ps::kPhyDefence, 1) == 10.0f);
    CHECK(Of(c, ps::kMagDefence, 1) == 20.0f);
    CHECK(Of(c, ps::kPhyAbsorption, 0) == 5.0f);
    CHECK(Of(c, ps::kMagAbsorption, 0) == 6.0f);
    // HP: the first word flat, the second a percent; hit rate, parry, critical the same pairs.
    c = game::BuffChanges(Skill(1, 1, {T("hpi"), 1000, 15, T("hr"), 30, 5, T("er"), 40, 6, T("cr"), 7, 8}), basis);
    CHECK(Of(c, ps::kMaxHp, 0) == 1000.0f);
    CHECK(Of(c, ps::kMaxHp, 1) == 15.0f);
    CHECK(Of(c, ps::kHitRate, 0) == 30.0f);
    CHECK(Of(c, ps::kHitRate, 1) == 5.0f);
    CHECK(Of(c, ps::kParry, 0) == 40.0f);
    CHECK(Of(c, ps::kParry, 1) == 6.0f);
    CHECK(Of(c, ps::kCritical, 0) == 7.0f);
    CHECK(Of(c, ps::kCritical, 1) == 8.0f);
    // Lanes by a mask (4 physical, 8 magical with 1 melee, 2 ranged): block, damage; less damage taken.
    c = game::BuffChanges(Skill(1, 1, {T("br"), 4 | 1, 25, T("dru2"), 8 | 2, 12, T("odar"), 4 | 1 | 2, 30}), basis);
    CHECK(Of(c, ps::kBlockBonus + 0, 0) == 25.0f);
    CHECK(Of(c, ps::kBlockBonus + 1, 0) == -12345.0f);
    CHECK(Of(c, ps::kDamage + 3, 0) == 12.0f);
    CHECK(Of(c, ps::kDamageTaken + 0, 2) == -30.0f);
    CHECK(Of(c, ps::kDamageTaken + 1, 2) == -30.0f);
    CHECK(Of(c, ps::kDamageTaken + 2, 2) == -12345.0f);
    // A shield's defence into attack; a toggle stops MP recovery; speed a factor.
    c = game::BuffChanges(Skill(1, 1, {T("spda"), 29, 47, T("onff"), 5000, 41, T("hste"), 70}), basis);
    CHECK(Of(c, ps::kPhyDefence, 0) == -116.0f); // 29 % of 400
    CHECK(Of(c, ps::kPhyAttackMin, 0) == 188.0f); // 47 % of 400
    CHECK(Of(c, ps::kMpRecovery, 3) == 0.0f);
    CHECK(Of(c, ps::kRunSpeed, 2) == 70.0f);
    // A trade-off buff's costs count only with their fourth word 2.
    c = game::BuffChanges(Skill(1, 1, {T("pmhp"), 900000, 0, 50, 2, T("pmdg"), 900000, 35, 35, 2, T("pmdp"), 900000, 40, 0, 1}), basis);
    CHECK(Of(c, ps::kMaxHp, 1) == -50.0f);
    CHECK(Of(c, ps::kFinalFactor + 0, 2) == -35.0f);
    CHECK(Of(c, ps::kPhyDefence, 1) == -12345.0f);
}

TEST_CASE("buffs: which may stand beside which") {
    const assets::SkillRow low = Skill(10, 3, {T("dura"), 30000}, 0x2);
    const assets::SkillRow high = Skill(10, 5, {T("dura"), 30000}, 0x2);
    const assets::SkillRow other = Skill(11, 1, {T("dura"), 30000}, 0x2);
    const assets::SkillRow apart = Skill(12, 1, {T("dura"), 30000}, 0x3);
    // A rank of the same group: a higher or equal one replaces, a lower one is refused.
    std::vector<const assets::SkillRow*> running{&low};
    game::BuffAdmission a = game::AdmitBuff(high, running);
    CHECK(a.admitted);
    CHECK(a.replaces == std::vector<std::size_t>{0});
    running = {&high};
    CHECK_FALSE(game::AdmitBuff(low, running).admitted);
    // Another group sharing an overlap group is refused; one apart stands beside.
    CHECK_FALSE(game::AdmitBuff(other, running).admitted);
    a = game::AdmitBuff(apart, running);
    CHECK(a.admitted);
    CHECK(a.replaces.empty());
    // The overlap groups: the low three bytes, but 0x1D and 0x23 (the top byte is the wait's group).
    CHECK(game::OverlapGroups(Skill(1, 1, {}, 0x29010203)) == std::vector<std::uint8_t>{3, 2, 1});
    CHECK(game::OverlapGroups(Skill(1, 1, {}, 0x00231D05)) == std::vector<std::uint8_t>{5});
}

TEST_CASE("buffs: a player's numbers with its buffs: STR first, the numbers made from it, then each number") {
    game::StatSheet sheet;
    sheet.Add(1, {ps::kStr, 0, 20.0f});
    sheet.Add(2, {ps::kPhyDefence, 1, 10.0f});
    sheet.Add(3, {ps::kMaxHp, 0, 500.0f});
    const game::CombatStats plain = game::PlayerCombatStats(20, 40, 30, {}, nullptr);
    const game::CombatStats stronger = game::PlayerCombatStats(20, 60, 30, {}, nullptr);
    const game::CombatStats buffed = game::PlayerCombatStats(20, 40, 30, {}, nullptr, sheet);
    CHECK(buffed.strength == doctest::Approx(60.0));
    CHECK(buffed.maxHp == stronger.maxHp + 500);
    CHECK(buffed.phyDefence == doctest::Approx(float(stronger.phyDefence * 1.1)));
    CHECK(plain.strength == doctest::Approx(40.0));
}
