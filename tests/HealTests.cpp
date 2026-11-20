// Heals and cures: a heal's amounts (game/Heals.h) and a cure's work on a set of states (game/States.h), each as an
// exact number.

#include <doctest/doctest.h>

#include <initializer_list>
#include <random>

#include "sr/assets/GameData.h"
#include "sr/game/Heals.h"
#include "sr/game/MasteryGap.h"
#include "sr/game/States.h"

using namespace sr;

namespace {

std::uint32_t T(const char* tag) { return assets::SkillTag(tag); }

assets::SkillRow Skill(std::initializer_list<std::uint32_t> tail) {
    assets::SkillRow s;
    s.id = 1;
    s.params.assign(tail.begin(), tail.end());
    return s;
}

game::ActiveState State(game::StateKind kind, int level, std::uint32_t ms, game::ActiveState::Clock::time_point at) {
    game::ActiveState s;
    s.kind = kind;
    s.level = level;
    s.durationMs = ms;
    s.started = at;
    s.nextTick = at;
    return s;
}

} // namespace

TEST_CASE("heals: the amount, its percent of it, a transfer's share, the target's percent more, the weapon's bonus") {
    game::CombatStats healer, target;
    target.maxHp = 5000;
    // The amount and its percent of it (the Cleric's heal passive adds to the percent): 73 + 14.
    game::HealSpec h = game::HealOf(Skill({T("heal"), 73, 0, 0, 0}), 20);
    CHECK(h.hpPercent == 20);
    CHECK(game::HealFor(h, 100, healer, target).hp == 87u);
    // The target's percent more (0xAA): 87 x 1.1, a whole number.
    target.healReceived[0] = 10.0;
    CHECK(game::HealFor(h, 100, healer, target).hp == 95u);
    target.healReceived[0] = 0.0;
    // A transfer's further target: its share of the amount (50 % of 4029).
    CHECK(game::HealFor(game::HealOf(Skill({T("heal"), 4029})), 50, healer, target).hp == 2014u);
    // 'eshp': the percent is of the target's most HP, in place of the amount.
    h = game::HealOf(Skill({T("eshp"), T("heal"), 246, 0, 0, 0}), 20);
    CHECK(h.ofMostHp);
    CHECK(game::HealFor(h, 100, healer, target).hp == 1000u);
    // MP: the amount and its percent.
    const game::HealAmount mp = game::HealFor(game::HealOf(Skill({T("heal"), 0, 0, 246, 10})), 100, healer, target);
    CHECK(mp.hp == 0u);
    CHECK(mp.mp == 270u);
    // The weapon's bonus: its magical attack's two ends x 0.5 x the INT balance x 150 / 100.
    healer.weaponMagMin = 100;
    healer.weaponMagMax = 200;
    healer.magBalance = 0.8;
    CHECK(game::HealFor(game::HealOf(Skill({T("heal"), 73, 0, 0, 0, T("mwhh"), 150})), 100, healer, target).hp == 73u + 180u);
    CHECK(game::HealFor(game::HealOf(Skill({T("heal"), 0, 0, 246, 0, T("mwmh"), 150})), 100, healer, target).mp == 246u + 180u);
}

TEST_CASE("the gap between the level and the highest mastery: less experience, more skill points, up to a gap of 9") {
    using game::MasteryGapRatesOf;
    CHECK(MasteryGapRatesOf(30, 30).experience == doctest::Approx(1.0f));
    CHECK(MasteryGapRatesOf(30, 30).skill == doctest::Approx(1.0f));
    CHECK(MasteryGapRatesOf(30, 25).experience == doctest::Approx(0.5f));
    CHECK(MasteryGapRatesOf(30, 25).skill == doctest::Approx(1.5f));
    // The user's case: level 30, skills at 21.
    CHECK(MasteryGapRatesOf(30, 21).experience == doctest::Approx(0.1f));
    CHECK(MasteryGapRatesOf(30, 21).skill == doctest::Approx(1.9f));
    // No further than that, however wide.
    CHECK(MasteryGapRatesOf(60, 1).experience == doctest::Approx(0.1f));
    CHECK(MasteryGapRatesOf(60, 1).skill == doctest::Approx(1.9f));
    // Masteries above the level turn it round; past 120 it does not count.
    CHECK(MasteryGapRatesOf(20, 23).experience == doctest::Approx(1.3f));
    CHECK(MasteryGapRatesOf(20, 23).skill == doctest::Approx(0.7f));
    CHECK(MasteryGapRatesOf(30, 121).experience == doctest::Approx(1.0f));
    CHECK(MasteryGapRatesOf(30, 121).skill == doctest::Approx(1.0f));
}

TEST_CASE("the death penalty and a resurrection's numbers (K9)") {
    // Up to level 10: nothing. Above: the least of the level's monster experience x 100 and 2 % of
    // its experience (a float's 0.02), at least 1; less a reduction's percent.
    CHECK(game::DeathPenalty(10, 1'000'000, 500) == 0u);
    CHECK(game::DeathPenalty(30, 1'000'000, 500) == 19'999u); // 2 % (0.0199999995...) under 50,000
    CHECK(game::DeathPenalty(30, 10'000'000, 500) == 50'000u); // 500 x 100 under 199,999
    CHECK(game::DeathPenalty(30, 10'000'000, 500, 30.0) == 35'000u);
    CHECK(game::DeathPenalty(30, 40, 500) == 0u); // its 2 % is under 1
    CHECK(game::DeathPenalty(30, 60, 0) == 1u);   // at least 1
    // Killed by a player: 0.4 % and x 20.
    CHECK(game::DeathPenalty(30, 1'000'000, 500, 0.0, true) == 4'000u);   // 0.4 % under 10,000
    CHECK(game::DeathPenalty(30, 10'000'000, 500, 0.0, true) == 10'000u); // 500 x 20 under 40,000
    // A resurrection: HP and MP its amounts and percents of the most; its percent of what the
    // death took back, rounded up.
    game::ReviveSpec r;
    r.maxLevel = 27;
    r.experiencePercent = 7;
    r.hpPercent = 10;
    r.mpPercent = 10;
    const game::Revival a = game::RevivalFor(r, 5000, 3333, 19'999);
    CHECK(a.hp == 500u);
    CHECK(a.mp == 333u);
    CHECK(a.experience == 1400u); // 1,399.93
    r.hp = 152;
    r.mp = 152;
    r.hpPercent = r.mpPercent = 0;
    r.experiencePercent = 5;
    const game::Revival b = game::RevivalFor(r, 5000, 3333, 20'000);
    CHECK(b.hp == 152u);
    CHECK(b.mp == 152u);
    CHECK(b.experience == 1000u); // exactly 1,000: not up
    CHECK(game::RevivalFor(r, 5000, 3333, 0).experience == 0u);
    // From a skill's 'resu' and 'heal'.
    const game::ReviveSpec s = game::ReviveOf(Skill({T("heal"), 0, 10, 0, 10, T("resu"), 42, 7}));
    CHECK(s.maxLevel == 42);
    CHECK(s.experiencePercent == 7);
    CHECK(s.hpPercent == 10);
    CHECK(s.mpPercent == 10);
}

TEST_CASE("cures: the elements lose the cure level's worth of their step, the curses go at their chance, 'rcur' takes that many") {
    using game::StateKind;
    const auto t0 = game::ActiveState::Clock::now();
    const game::CureSpec cure = game::CureOf(Skill({T("curt"), 63, 36, T("curl"), 25145280, 100, 3, T("rcur"), 2}));
    CHECK(cure.elementMask == 63u);
    CHECK(cure.elementLevel == 36);
    CHECK(cure.curseChance == 100);
    CHECK(cure.curseLevel == 3);
    CHECK(cure.most == 2);
    std::mt19937 random(3);
    // A burn of level 50 (37.5 s) loses 36 x 750 ms; again, it is gone. A freeze of level 30 goes at once.
    {
        game::StateSet set;
        game::ChanceRolls rolls;
        set.Apply(State(StateKind::Burn, 50, 37'500, t0));
        game::CureSpec elements = cure;
        elements.most = 0;
        CHECK(set.Cure(elements, rolls, random, t0));
        REQUIRE(set.In(StateKind::Burn));
        CHECK(set.Find(StateKind::Burn)->durationMs == 10'500u);
        CHECK(set.Cure(elements, rolls, random, t0));
        CHECK_FALSE(set.In(StateKind::Burn));
        set.Apply(State(StateKind::Freeze, 30, 2'910, t0));
        CHECK(set.Cure(elements, rolls, random, t0));
        CHECK_FALSE(set.In(StateKind::Freeze));
    }
    // A curse at or under the cure's level: its chance (100); a level above, divided by the gap.
    {
        game::StateSet set;
        game::ChanceRolls rolls;
        set.Apply(State(StateKind::Sleep, 3, 20'000, t0));
        game::CureSpec curses = cure;
        curses.most = 0;
        CHECK(set.Cure(curses, rolls, random, t0));
        CHECK_FALSE(set.In(StateKind::Sleep));
    }
    int cured = 0;
    for (int i = 0; i < 2000; ++i) {
        game::StateSet set;
        game::ChanceRolls rolls; // each a first roll: a success when 0..100 falls at or under the chance
        set.Apply(State(StateKind::Fear, 6, 20'000, t0));
        game::CureSpec curses = cure;
        curses.most = 0;
        set.Cure(curses, rolls, random, t0);
        cured += set.In(StateKind::Fear) ? 0 : 1;
    }
    MESSAGE("a fear 3 levels above: cured " << cured << " of 2000 (33 %: 34 of the roll's 101 values)");
    CHECK(cured > 2000 * 28 / 100);
    CHECK(cured < 2000 * 40 / 100);
    // 'rcur' 2 of four curses: two go, two stay.
    {
        game::StateSet set;
        game::ChanceRolls rolls;
        for (const StateKind k : {StateKind::Sleep, StateKind::Root, StateKind::Fear, StateKind::Dark}) set.Apply(State(k, 1, 20'000, t0));
        CHECK(set.Cure(cure, rolls, random, t0));
        CHECK(set.All().size() == 2u);
    }
    // Nothing of its masks: nothing.
    {
        game::StateSet set;
        game::ChanceRolls rolls;
        set.Apply(State(StateKind::Sleep, 1, 20'000, t0));
        game::CureSpec elements = cure;
        elements.curseMask = 0;
        CHECK_FALSE(set.Cure(elements, rolls, random, t0));
        CHECK(set.In(StateKind::Sleep));
    }
}
