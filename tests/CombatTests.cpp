// One hit's damage as the original game server works it out (game/Combat.h).
// An attack whose min is its max leaves the variance nothing to move, so each rule gives an exact
// number.

#include <doctest/doctest.h>

#include <random>

#include "sr/assets/GameData.h"
#include "sr/game/Combat.h"

namespace {

using namespace sr::game;

CombatStats Attacker() {
    CombatStats s;
    s.level = 10;
    s.phyAttackMin = s.phyAttackMax = 1000;
    s.magAttackMin = s.magAttackMax = 1000;
    s.hitRate = 100;
    return s;
}

CombatStats Defender() {
    CombatStats s;
    s.level = 10;
    s.phyAbsorption = 100; // the hit halved
    s.phyDefence = 100;
    s.magAbsorption = 100;
    s.magDefence = 100;
    s.evasion = 100;
    return s;
}

AttackSpec Melee() {
    AttackSpec a;
    a.skill = 1;
    a.kind = 5; // physical melee
    return a;
}

std::uint32_t Hit(const CombatStats& attacker, const CombatStats& defender, const AttackSpec& attack, HitRoll who = {}) {
    std::mt19937 random(5);
    return RollHit(attacker, defender, attack, who, random).damage;
}

} // namespace

TEST_CASE("the hit: divided by the absorption, less the defence, then the skill's power and the mastery") {
    // 1000 / (1 + 100 / 100) - 100 = 400.
    CHECK(Hit(Attacker(), Defender(), Melee()) == 400);
    AttackSpec strong = Melee();
    strong.power = 200; // after the defence: 400 x 2
    CHECK(Hit(Attacker(), Defender(), strong) == 800);
    AttackSpec flat = Melee();
    flat.flatMin = flat.flatMax = 200; // the skill's attack joins the range first: 1200 / 2 - 100
    CHECK(Hit(Attacker(), Defender(), flat) == 500);
    AttackSpec mastered = Melee();
    mastered.mastery = 50; // the range 1 % a mastery level higher: 1500 / 2 - 100
    CHECK(Hit(Attacker(), Defender(), mastered) == 650);
    AttackSpec magic = Melee();
    magic.kind = 10;
    magic.magical = true;
    CHECK(Hit(Attacker(), Defender(), magic) == 400);
}

TEST_CASE("the hit: a monster's grade (the original's scale): a titan's, a giant's or an elite's x 1.5, an event's strong one's x 1.8") {
    CHECK(Hit(Attacker(), Defender(), Melee()) == 400);
    CombatStats titan = Attacker();
    titan.hitScale = MonsterCombatStats(sr::assets::ObjectRow{}, 5).hitScale;
    CHECK(Hit(titan, Defender(), Melee()) == 600);
    CombatStats strong = Attacker();
    strong.hitScale = MonsterCombatStats(sr::assets::ObjectRow{}, 7).hitScale;
    CHECK(Hit(strong, Defender(), Melee()) == 720);
    CHECK(MonsterCombatStats(sr::assets::ObjectRow{}, 3).hitScale == 1.0); // a unique as it is
    // An elite's range 1.5 times: (1000 x 1.5) / 2 - 100.
    CombatStats elite = Attacker();
    elite.rangeFactor = MonsterCombatStats(sr::assets::ObjectRow{}, 6).rangeFactor;
    CHECK(Hit(elite, Defender(), Melee()) == 650);
}

TEST_CASE("the hit: an attack of neither melee nor ranged (kind 8) takes no lane's damage percent nor block bonus") {
    CombatStats a = Attacker();
    a.damagePercent = {100, 100, 100, 100};
    AttackSpec dot = Melee();
    dot.kind = 8; // magical, neither melee nor ranged
    dot.magical = true;
    CHECK(Hit(a, Defender(), dot) == 400);
    AttackSpec melee = Melee();
    melee.kind = 9; // magical melee
    melee.magical = true;
    CHECK(Hit(a, Defender(), melee) == 800);
}

TEST_CASE("the hit: the level above the target, the balance, berserk, the final factors") {
    CombatStats higher = Attacker();
    higher.level = 20; // 10 levels above: x 1.3 (at most 30 %)
    CHECK(Hit(higher, Defender(), Melee()) == 520);
    CombatStats lower = Attacker();
    lower.level = 5; // below: nothing taken off
    CHECK(Hit(lower, Defender(), Melee()) == 400);
    CombatStats balanced = Attacker();
    balanced.phyBalance = 0.5; // a player's balance; a monster's hits have none
    CHECK(Hit(balanced, Defender(), Melee(), {true, false, nullptr}) == 200);
    CHECK(Hit(balanced, Defender(), Melee(), {false, true, nullptr}) == 400);
    CombatStats berserk = Attacker();
    berserk.berserk = true;
    berserk.berserkBonus = 0.5; // x (2 + 0.5)
    CHECK(Hit(berserk, Defender(), Melee()) == 1000);
    CombatStats factored = Attacker();
    factored.finalFactor = {1.5, 1.0};
    CombatStats taking = Defender();
    taking.finalTakenFactor = {0.5, 1.0};
    CHECK(Hit(factored, taking, Melee()) == 300);
    CombatStats lane = Attacker();
    lane.damagePercent = {50, 0, 0, 0}; // physical melee + 50 %
    CHECK(Hit(lane, Defender(), Melee()) == 600);
    AttackSpec shot = Melee();
    shot.kind = 6; // physical ranged: not the melee lane
    CHECK(Hit(lane, Defender(), shot) == 400);
}

TEST_CASE("the hit: criticals, blocks, ignoring a monster's defence, and a hit too small") {
    CombatStats sure = Attacker();
    sure.critical = 100;
    std::mt19937 random(5);
    const HitResult critical = RollHit(sure, Defender(), Melee(), {}, random);
    CHECK(critical.critical);
    CHECK(critical.damage == 800);
    AttackSpec magic = Melee();
    magic.kind = 10;
    magic.magical = true;
    CHECK_FALSE(RollHit(sure, Defender(), magic, {}, random).critical); // magic never criticals
    CombatStats resisting = Defender();
    resisting.criticalResist = 100000; // the chance divided to nothing
    CHECK_FALSE(RollHit(sure, resisting, Melee(), {}, random).critical);
    CombatStats wall = Defender();
    wall.block = 100;
    const HitResult blocked = RollHit(Attacker(), wall, Melee(), {}, random);
    CHECK(blocked.blocked);
    CHECK(blocked.damage == 0);
    CombatStats piercing = Attacker();
    piercing.ignoreDefenceChance = 100; // a player on a monster: the whole point
    CHECK(Hit(piercing, Defender(), Melee(), {true, false, nullptr}) == 1000);
    CHECK(Hit(piercing, Defender(), Melee(), {true, true, nullptr}) == 400); // not on a player
    // Under 5 % of the point: 1 to 10 % of it (here 1 .. 100).
    CombatStats armoured = Defender();
    armoured.phyDefence = 100000;
    for (int i = 0; i < 50; ++i) {
        const std::uint32_t d = RollHit(Attacker(), armoured, Melee(), {}, random).damage;
        CHECK(d >= 1);
        CHECK(d <= 100);
    }
}

TEST_CASE("a skill's own damage: less on a higher target, never rolled, at the target's share, at least 1") {
    // 2.5 % less a level the target is above, at least a tenth; nothing off on a lower one.
    CHECK(FixedDamage(1000, 10, 10) == 1000);
    CHECK(FixedDamage(1000, 10, 14) == 900);
    CHECK(FixedDamage(1000, 10, 5) == 1000);
    CHECK(FixedDamage(1000, 10, 60) == 100);
    AttackSpec fixed;
    fixed.skill = 2;
    fixed.formula = false; // no 'att': its own damage alone
    fixed.fixedDamage = 1000;
    // Past the defences, never blocked, never a critical.
    CombatStats wall = Defender();
    wall.block = 100;
    CombatStats sure = Attacker();
    sure.critical = 100;
    std::mt19937 random(5);
    const HitResult hit = RollHit(sure, wall, fixed, {}, random);
    CHECK_FALSE(hit.blocked);
    CHECK_FALSE(hit.critical);
    CHECK(hit.damage == 1000);
    // An area's further target: its share of it (the first 100 - falloff 35).
    CHECK(Hit(Attacker(), Defender(), fixed, {false, false, nullptr, 65}) == 650);
    // With an 'att', the two add up: 400 + 1000.
    AttackSpec both = Melee();
    both.fixedDamage = 1000;
    CHECK(Hit(Attacker(), Defender(), both) == 1400);
    // A hit not blocked does at least 1.
    CombatStats numb = Defender();
    numb.finalTakenFactor = {0.0001, 1.0};
    CHECK(Hit(Attacker(), numb, Melee()) == 1);
}

TEST_CASE("a life steal: its amount less the level gap, no more than the target has; an imbue at the skill's share") {
    AttackSpec steal;
    steal.skill = 3;
    steal.formula = false;
    steal.lifeSteal = 500;
    CHECK(Hit(Attacker(), Defender(), steal) == 500);
    CHECK(Hit(Attacker(), Defender(), steal, {false, false, nullptr, 100, 200}) == 200); // the target has 200
    CombatStats high = Defender();
    high.level = 18; // 8 above: 20 % less
    CHECK(Hit(Attacker(), high, steal) == 400);
    // An imbue's force (its own magical part, 400 here) joins at the skill's 'att' share of it.
    AttackSpec imbue;
    imbue.skill = 4;
    imbue.kind = 8;
    imbue.magical = true;
    AttackSpec half = Melee();
    half.imbuePercent = 50;
    std::mt19937 random(5);
    CHECK(RollHit(Attacker(), Defender(), half, {}, random, &imbue).damage == 600);
    AttackSpec none = Melee(); // a skill of no share (the European ones): the imbue adds nothing
    CHECK(RollHit(Attacker(), Defender(), none, {}, random, &imbue).damage == 400);
    // A target the imbue's jump added: the physical part 0, the imbue's
    // magical force at the share, then the target's share of it: 400 x 50 % x 20 %.
    HitRoll jumped;
    jumped.share = 20;
    jumped.magicalOnly = true;
    CHECK(RollHit(Attacker(), Defender(), half, jumped, random, &imbue).damage == 40);
    AttackSpec magic = half; // a magical skill keeps its own part: (400 + 200) x 20 %
    magic.kind = 8;
    magic.magical = true;
    CHECK(RollHit(Attacker(), Defender(), magic, jumped, random, &imbue).damage == 120);
}

TEST_CASE("the chance history: the long-run rate is the chance, and streaks stay short") {
    std::mt19937 random(11);
    ChanceRolls rolls;
    int successes = 0, worstMisses = 0, misses = 0;
    for (int i = 0; i < 10000; ++i) {
        if (rolls.Roll(30, 'C', 7, random)) {
            ++successes;
            misses = 0;
        } else {
            worstMisses = std::max(worstMisses, ++misses);
        }
    }
    CHECK(successes >= 2950);
    CHECK(successes <= 3050);
    // A success leaves the running value at least -70, each miss raises it by 30: by 110 no roll of
    // 0..100 can miss, so at most six in a row (a plain 30 % roll misses 15 and more in 10,000).
    CHECK(worstMisses <= 6);
    // Each skill and kind of roll keeps its own history; 0 never and 100 always.
    CHECK_FALSE(rolls.Roll(0, 'B', 7, random));
    CHECK(rolls.Roll(100, 'B', 7, random));
}

TEST_CASE("a wall ('pw'): it takes its kinds' parts of a hit by its own defence; covering every part the holder is untouched") {
    std::mt19937 random(5);
    // An Ice Wall's kinds 7 (physical), no defence of its own: a melee hit's 1000 all to the wall, nothing to the holder.
    WallSpec ice{7, 0, 0, 10000};
    HitRoll walled;
    walled.wall = &ice;
    HitResult hit = RollHit(Attacker(), Defender(), Melee(), walled, random);
    CHECK(hit.wallOnly);
    CHECK(hit.wallTaken == 1000);
    CHECK(hit.damage == 0);
    // A Fire Wall's kinds 11 (magical), defence 100, absorption 100: a magic hit's 1000 / 2 - 100 to the wall; a melee
    // hit passes it whole to the holder.
    WallSpec fire{11, 100, 100, 10000};
    walled.wall = &fire;
    AttackSpec magic = Melee();
    magic.kind = 10;
    magic.magical = true;
    hit = RollHit(Attacker(), Defender(), magic, walled, random);
    CHECK(hit.wallOnly);
    CHECK(hit.wallTaken == 400);
    hit = RollHit(Attacker(), Defender(), Melee(), walled, random);
    CHECK_FALSE(hit.wallOnly);
    CHECK(hit.wallTaken == 0);
    CHECK(hit.damage == 400);
    // A melee skill with a magical imbue at 50 % on the Ice Wall: the physical part to the wall and zeroed on the
    // holder, the imbue's 400 at its share to the holder.
    AttackSpec imbue;
    imbue.skill = 4;
    imbue.kind = 8;
    imbue.magical = true;
    AttackSpec half = Melee();
    half.imbuePercent = 50;
    walled.wall = &ice;
    hit = RollHit(Attacker(), Defender(), half, walled, random, &imbue);
    CHECK_FALSE(hit.wallOnly);
    CHECK(hit.wallTaken == 1000);
    CHECK(hit.damage == 200);
    // Spent (no HP left): it takes nothing more but still zeroes its kinds' parts until it ends (a hit does at least 1).
    WallSpec spent{7, 0, 0, 0};
    walled.wall = &spent;
    hit = RollHit(Attacker(), Defender(), Melee(), walled, random);
    CHECK_FALSE(hit.wallOnly);
    CHECK(hit.wallTaken == 0);
    CHECK(hit.damage == 1);
    // A life steal (and a skill of fixed damage) leaves the wall out.
    AttackSpec steal;
    steal.skill = 3;
    steal.formula = false;
    steal.lifeSteal = 500;
    walled.wall = &ice;
    hit = RollHit(Attacker(), Defender(), steal, walled, random);
    CHECK(hit.wallTaken == 0);
    CHECK(hit.damage == 500);
}
