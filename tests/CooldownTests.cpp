#include <doctest/doctest.h>

#include "sr/assets/Cooldown.h"

using namespace sr::assets;
using Kind = CooldownStart::Kind;

namespace {
// The packed type word of TID1..TID4 (bits 2, 5, 7, 11).
std::uint16_t Word(int t1, int t2, int t3, int t4) {
    return static_cast<std::uint16_t>(t1 << 2 | t2 << 5 | t3 << 7 | t4 << 11);
}
} // namespace

TEST_CASE("potion lanes come from the packed type word") {
    CHECK(PotionCategory(Word(3, 3, 1, 1)) == 1); // HP potion
    CHECK(PotionCategory(Word(3, 3, 1, 2)) == 2);
    CHECK(PotionCategory(Word(3, 3, 1, 3)) == 3);
    CHECK(PotionCategory(Word(3, 3, 1, 4)) == 4);
    CHECK(PotionCategory(Word(3, 3, 1, 7)) == 6);
    CHECK(PotionCategory(Word(3, 3, 1, 9)) == 7);
    CHECK(PotionCategory(Word(3, 3, 1, 6)) == 0); // no lane
    CHECK(PotionCategory(Word(3, 3, 2, 1)) == 13); // the universal pill
    CHECK(PotionCategory(Word(3, 3, 2, 6)) == 14);
    CHECK(PotionCategory(Word(3, 3, 2, 7)) == 15);
    CHECK(PotionCategory(Word(3, 3, 3, 1)) == 0);
    CHECK(PotionCategory(Word(1, 1, 1, 1)) == 0);
}

TEST_CASE("potion and skill wait lengths") {
    ItemRow chinese, european, percent;
    european.country = 1;
    percent.params[1] = 5;
    CHECK(PotionCooldownMs(1, chinese) == 1000);
    CHECK(PotionCooldownMs(2, european) == 15000);
    CHECK(PotionCooldownMs(1, percent) == 4000);
    CHECK(PotionCooldownMs(13, chinese) == 20000);
    CHECK(PotionCooldownMs(4, chinese) == 1000);

    SkillRow plain;
    plain.cooldownMs = 8000;
    CHECK(SkillCooldownMs(plain) == 8000);
    SkillRow action;
    action.activity = 2;
    action.preparingMs = 500;
    action.castingMs = 700;
    action.durationMs = 1000;
    action.cooldownMs = 1500; // the authored CoolTime alone, however long the action
    CHECK(SkillCooldownMs(action) == 1500);
    action.cooldownMs = 5000;
    CHECK(SkillCooldownMs(action) == 5000);
}

TEST_CASE("the board shares a lane between items, a group between authored items, a skill group between skills") {
    CooldownBoard board;
    board.Start({Kind::ItemLane, 1, 0, 0, 1000}, 0.0);
    // any HP potion, whatever its reference, is shut
    REQUIRE(board.Item(Word(3, 3, 1, 1), 111, 0, 500.0) != nullptr);
    REQUIRE(board.Item(Word(3, 3, 1, 1), 222, 0, 500.0) != nullptr);
    CHECK(board.Item(Word(3, 3, 1, 2), 333, 0, 500.0) == nullptr); // the MP lane is open
    CHECK(board.Item(Word(3, 3, 1, 1), 111, 0, 1000.0) == nullptr); // an item's wait is gone when it ends

    board.Start({Kind::ItemAuthored, 0, 7, 0, 3000}, 0.0);
    CHECK(board.Item(Word(3, 3, 3, 1), 900, 7, 100.0) != nullptr); // the same group
    CHECK(board.Item(Word(3, 3, 3, 1), 901, 8, 100.0) == nullptr); // another group
    board.Start({Kind::ItemAuthored, 0, 0, 55, 3000}, 0.0);
    CHECK(board.Item(Word(3, 3, 3, 1), 55, 0, 100.0) != nullptr); // no group: that reference only
    CHECK(board.Item(Word(3, 3, 3, 1), 56, 0, 100.0) == nullptr);

    board.Start({Kind::Skill, 0, 4, 300, 10000}, 0.0);
    CHECK(board.Skill(300, 4, 5000.0) != nullptr);
    CHECK(board.Skill(301, 4, 5000.0) != nullptr); // the group is shut with it
    CHECK(board.Skill(302, 0, 5000.0) == nullptr);
    CHECK(board.Skill(300, 4, 10400.0) != nullptr); // kept for the flash
    CHECK(board.Skill(300, 4, 10600.0) == nullptr);

    // a new start replaces the old one of its lane
    board.Start({Kind::ItemLane, 1, 0, 0, 4000}, 900.0);
    const CooldownWait* w = board.Item(Word(3, 3, 1, 1), 1, 0, 1000.0);
    REQUIRE(w != nullptr);
    CHECK(w->durationMs == 4000.0);
    CHECK(w->startMs == 900.0);
}

TEST_CASE("the sweep frame follows the remaining time, then a flash, then nothing") {
    const CooldownWait wait{1000.0, 10000.0};
    CooldownSweep sweep;
    REQUIRE(SweepFor(wait, 1000.0, sweep)); // just started: nothing swept yet
    CHECK_FALSE(sweep.flash);
    CHECK(sweep.columns == 16);
    CHECK(sweep.frame == 0);
    REQUIRE(SweepFor(wait, 6000.0, sweep)); // half left
    CHECK(sweep.frame == 119);              // trunc(239 - 0.5 * 239)
    REQUIRE(SweepFor(wait, 10999.0, sweep));
    CHECK(sweep.frame >= 238);
    REQUIRE(SweepFor(wait, 11250.0, sweep)); // after the end: the flash, 4 x 4
    CHECK(sweep.flash);
    CHECK(sweep.columns == 4);
    CHECK(sweep.frame == 7);                 // trunc(15 - 0.5 * 15)
    CHECK_FALSE(SweepFor(wait, 11500.0, sweep));
}

TEST_CASE("the countdown counts seconds, minutes, hours") {
    const CooldownWait wait{0.0, 7300000.0};
    CooldownCount count;
    REQUIRE(CountFor(wait, 0.0, count));
    CHECK(count.tier == 2);
    CHECK(count.value == 2);
    REQUIRE(CountFor({0.0, 90000.0}, 0.0, count));
    CHECK(count.tier == 1);
    CHECK(count.value == 1);
    REQUIRE(CountFor({0.0, 45500.0}, 0.0, count));
    CHECK(count.tier == 0);
    CHECK(count.value == 45);
    CHECK_FALSE(CountFor({0.0, 1000.0}, 1000.0, count));
}
