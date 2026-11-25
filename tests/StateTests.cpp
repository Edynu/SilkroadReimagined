// States from hits (K3): the original game server's rules,
// each as an exact number (game/States.h), and the monsters that take them (server/game Monsters.h).

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <thread>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/assets/Terrain.h"
#include "sr/core/Jobs.h"
#include "sr/game/Combat.h"
#include "sr/game/Passives.h"
#include "sr/game/States.h"
#include "sr/server/Monsters.h"
#include "sr/server/World.h"

using namespace sr;
using game::StateKind;
using Clock = std::chrono::steady_clock;

namespace {

game::StateGift Gift(StateKind kind, std::initializer_list<int> words) {
    game::StateGift g;
    g.kind = kind;
    std::size_t i = 0;
    for (const int w : words) g.words[i++] = w;
    return g;
}

game::StateDefence Defence(int level) {
    game::StateDefence d;
    d.level = level;
    return d;
}

game::ActiveState State(StateKind kind, int level, std::uint32_t ms, Clock::time_point at, int value = 0) {
    game::ActiveState s;
    s.kind = kind;
    s.level = level;
    s.durationMs = ms;
    s.started = s.nextTick = at;
    s.value = value;
    return s;
}

} // namespace

TEST_CASE("states: each kind's bit in the mask (burn and shock swap) and its resistance bucket") {
    CHECK(game::StateBit(StateKind::Freeze) == 0x1);
    CHECK(game::StateBit(StateKind::Frostbite) == 0x2);
    CHECK(game::StateBit(StateKind::Shock) == 0x4);
    CHECK(game::StateBit(StateKind::Burn) == 0x8);
    CHECK(game::StateBit(StateKind::Poison) == 0x10);
    CHECK(game::StateBit(StateKind::Sleep) == 0x40);
    CHECK(game::StateBit(StateKind::Stun) == 0x4000);
    CHECK(game::StateBit(StateKind::Combustion) == 0x400000);
    CHECK(game::StateBit(StateKind::TimeBomb) == 0x1000000);
    CHECK(game::ResistBucket(StateKind::Freeze) == -1);
    CHECK(game::ResistBucket(StateKind::Sleep) == 0);
    CHECK(game::ResistBucket(StateKind::Dark) == 6);
    CHECK(game::ResistBucket(StateKind::Stun) == 7);
    CHECK(game::ResistBucket(StateKind::Decay) == 10);
    CHECK(game::ResistBucket(StateKind::Impotent) == 12);
    CHECK(game::ResistBucket(StateKind::TimeBomb) == 16);
    // The burn's damage by its rate: the server's table, the last entry beyond it.
    CHECK(game::BurnRate(0) == 0);
    CHECK(game::BurnRate(1) == 8);
    CHECK(game::BurnRate(88) == 1000);
    CHECK(game::BurnRate(91) == 1112);
    CHECK(game::BurnRate(99) == 1469);
    CHECK(game::BurnRate(140) == 5533);
    CHECK(game::BurnRate(200) == 5533);
}

TEST_CASE("states: an element's level is its power cut by the target's resistance, its time a step a level") {
    game::StateDefence d = Defence(100);
    // Freeze of power 216 on a level-100 monster (ResistFrozen 90): level 21, 2.037 s.
    d.elementResist[0] = 90;
    game::StateOdds o = game::OddsOf(Gift(StateKind::Freeze, {216, 50}), d, 0);
    CHECK(o.chance == 50);
    CHECK(o.level == 21);
    CHECK(o.durationMs == 21u * 97u);
    // On a level-5 monster (10): level 194, 18.8 s; the susceptibility adds to the chance.
    d.elementResist[0] = 10;
    o = game::OddsOf(Gift(StateKind::Freeze, {216, 50}), d, 20);
    CHECK(o.chance == 70);
    CHECK(o.level == 194);
    CHECK(o.durationMs == 194u * 97u);
    // 100 resists it; a grade as high as the level leaves none.
    d.elementResist[0] = 100;
    CHECK(game::OddsOf(Gift(StateKind::Freeze, {216, 50}), d, 0).level == 0);
    d.elementResist[0] = 90;
    d.elementGrade[0] = 21;
    CHECK(game::OddsOf(Gift(StateKind::Freeze, {216, 50}), d, 0).level == 0);
    // Each element its step and its own resistance (by its bit: shock the third, burn the fourth).
    game::StateDefence e = Defence(30);
    e.elementResist = {60, 60, 50, 40, 60, 60};
    CHECK(game::OddsOf(Gift(StateKind::Frostbite, {164, 100}), e, 0).durationMs == 65u * 250u);
    CHECK(game::OddsOf(Gift(StateKind::Shock, {216, 25, 50}), e, 0).durationMs == 108u * 500u);
    CHECK(game::OddsOf(Gift(StateKind::Burn, {202, 20, 91}), e, 0).durationMs == 121u * 750u);
    CHECK(game::OddsOf(Gift(StateKind::Poison, {148, 20, 247}), e, 0).durationMs == 59u * 1000u);
}

TEST_CASE("states: the others reach targets up to ten times their level; beyond, less time and chance; resistances") {
    const game::StateGift sleep = Gift(StateKind::Sleep, {20000, 80, 10});
    // Up to level 100: all of it.
    game::StateOdds o = game::OddsOf(sleep, Defence(100), 0);
    CHECK(o.chance == 80);
    CHECK(o.level == 10);
    CHECK(o.durationMs == 20000u);
    // Level 105: 2.5 % of the time and 5 % of the chance a level over.
    o = game::OddsOf(sleep, Defence(105), 0);
    CHECK(o.durationMs == 17500u);
    CHECK(o.chance == 60);
    // Far over: never under half the time and a tenth of the chance.
    o = game::OddsOf(sleep, Defence(200), 0);
    CHECK(o.durationMs == 10000u);
    CHECK(o.chance == 8);
    // A susceptibility (disease) adds to the chance and lifts the reach by ten levels.
    o = game::OddsOf(sleep, Defence(105), 20);
    CHECK(o.durationMs == 20000u);
    CHECK(o.chance == 100);
    // The percent resistance (a monster's Resist column), then a flat one halved a level over its grade.
    game::StateDefence d = Defence(50);
    d.percent[0] = 50;
    CHECK(game::OddsOf(sleep, d, 0).chance == 40);
    d.percent[0] = 100;
    CHECK(game::OddsOf(sleep, d, 0).chance == 0);
    d.percent[0] = 0;
    d.flat[0] = 40;
    d.grade[0] = 8; // two levels under the state's: 40 -> 10
    CHECK(game::OddsOf(sleep, d, 0).chance == 70);
    d.grade[0] = 12; // above it: all of it
    CHECK(game::OddsOf(sleep, d, 0).chance == 40);
}

TEST_CASE("states: a roll keeps its history by kind; a stronger state replaces, an equal or weaker one is refused") {
    std::mt19937 random(5);
    game::ChanceRolls rolls;
    const auto now = Clock::now();
    // Certain rolls (100) take; the record carries the kind's numbers.
    std::vector<game::StateGift> gifts{Gift(StateKind::Burn, {100, 100, 91}), Gift(StateKind::Bleed, {30000, 100, 8, 156, 20}),
                                       Gift(StateKind::Stun, {5000, 100, 10})};
    const auto took = game::RollStates(gifts, Defence(1), 0, rolls, random, 7, 99, 0, now);
    REQUIRE(took.size() == 3);
    CHECK(took[0].kind == StateKind::Burn);
    CHECK(took[0].value == game::BurnRate(91));
    CHECK(took[0].tickMs == 2000);
    CHECK(took[1].value == 156);
    CHECK(took[1].value2 == 20);
    CHECK(took[2].durationMs == 5000u);
    CHECK(took[2].source == 7u);
    // Over many rolls a chance's rate is its chance.
    int stuns = 0;
    for (int i = 0; i < 1000; ++i) stuns += game::RollStates(std::vector{Gift(StateKind::Stun, {5000, 15, 10})}, Defence(1), 0, rolls, random, 7, 99, 0, now).size();
    CHECK(stuns > 120);
    CHECK(stuns < 180);

    game::StateSet set;
    CHECK(set.Apply(State(StateKind::Stun, 8, 5000, now)));
    CHECK_FALSE(set.Apply(State(StateKind::Stun, 8, 9000, now)));  // as strong: refused, its time not renewed
    CHECK_FALSE(set.Apply(State(StateKind::Stun, 5, 9000, now)));  // weaker
    CHECK(set.Find(StateKind::Stun)->durationMs == 5000u);
    CHECK(set.Apply(State(StateKind::Stun, 10, 9000, now)));       // stronger
    CHECK(set.Find(StateKind::Stun)->durationMs == 9000u);
    CHECK(set.Mask() == 0x4000u);
    // It ends when its time has passed.
    CHECK(set.Expire(now + std::chrono::milliseconds(9000)).empty());
    const auto ended = set.Expire(now + std::chrono::milliseconds(9001));
    REQUIRE(ended.size() == 1);
    CHECK(ended[0] == StateKind::Stun);
    CHECK(set.Mask() == 0u);
}

TEST_CASE("states: panic cuts HP recovery and combustion MP recovery by their second number (params 0x8F, 0x90)") {
    game::StateSet set;
    CHECK(game::RecoveryCut(set, 1000, true) == 1000);
    game::ActiveState panic;
    panic.kind = game::StateKind::Panic;
    panic.level = 1;
    panic.durationMs = 10000;
    panic.value2 = 50; // the recovery's cut, percent
    panic.started = game::ActiveState::Clock::now();
    REQUIRE(set.Apply(panic));
    CHECK(game::RecoveryCut(set, 1000, true) == 500);  // HP halved
    CHECK(game::RecoveryCut(set, 1000, false) == 1000); // MP untouched
    CHECK(game::RecoveryCut(set, 3, true) == 1);        // cut toward 0
    game::ActiveState combustion = panic;
    combustion.kind = game::StateKind::Combustion;
    combustion.value2 = 100;
    REQUIRE(set.Apply(combustion));
    CHECK(game::RecoveryCut(set, 1000, false) == 0); // all of it: none, never below 0
}

TEST_CASE("states: burn, poison and bleed tick every 2 s from the start; poison never takes the last point") {
    const auto t0 = Clock::now();
    game::StateSet set;
    game::ActiveState burn = State(StateKind::Burn, 20, 15000, t0, 1112);
    burn.tickMs = 2000;
    set.Apply(burn);
    auto ticks = set.Ticks(t0, 5000, 0, 25.0); // the first at once, over the magical absorption
    REQUIRE(ticks.size() == 1);
    CHECK(ticks[0].hp == std::uint32_t(1112.0 / 1.25));
    int count = 1;
    for (int ms = 50; ms <= 15000; ms += 50) count += int(set.Ticks(t0 + std::chrono::milliseconds(ms), 5000, 0, 25.0).size());
    CHECK(count == 8); // 0, 2, ..., 14 s
    game::StateSet poisoned;
    game::ActiveState p = State(StateKind::Poison, 50, 10000, t0, 500);
    p.tickMs = 2000;
    poisoned.Apply(p);
    ticks = poisoned.Ticks(t0, 120, 0, 0.0);
    REQUIRE(ticks.size() == 1);
    CHECK(ticks[0].hp == 119u);
    game::StateSet bleeding;
    game::ActiveState b = State(StateKind::Bleed, 8, 30000, t0, 156);
    b.tickMs = 3000; // the skill's 'puls'
    bleeding.Apply(b);
    CHECK(bleeding.Ticks(t0, 100, 0, 0.0)[0].hp == 156u); // it may kill
    CHECK(bleeding.Ticks(t0 + std::chrono::milliseconds(2500), 100, 0, 0.0).empty());
    CHECK(bleeding.Ticks(t0 + std::chrono::milliseconds(3000), 100, 0, 0.0).size() == 1);
}

TEST_CASE("states: what they do to the numbers and the moves; a hit's damage wakes and breaks a stun one time in four") {
    const auto now = Clock::now();
    game::StateSet set;
    set.Apply(State(StateKind::Shock, 50, 5000, now, 50));
    set.Apply(State(StateKind::Dark, 10, 5000, now, 30));
    game::ActiveState bleed = State(StateKind::Bleed, 8, 5000, now, 156);
    bleed.value2 = 20;
    set.Apply(bleed);
    set.Apply(State(StateKind::Decay, 10, 5000, now, 500));
    set.Apply(State(StateKind::Impotent, 10, 5000, now, 35));
    set.Apply(State(StateKind::Division, 10, 5000, now, 35));
    game::CombatStats s;
    s.evasion = 200;
    s.hitRate = 300;
    s.phyDefence = 1000;
    s.magDefence = 1000;
    set.Modify(s);
    CHECK(s.evasion == doctest::Approx(100.0));
    CHECK(s.hitRate == doctest::Approx(210.0));
    CHECK(s.phyDefence == doctest::Approx((1000.0 - 500.0) * 0.8)); // the keeper: decay's flat 500, then bleed's 20 % off
    CHECK(s.magDefence == doctest::Approx(800.0));
    CHECK(s.finalFactor[0] == doctest::Approx(0.65));
    CHECK(s.finalTakenFactor[1] == doctest::Approx(1.35));
    // Decay never takes more than 70 % of the defence.
    game::StateSet decayed;
    decayed.Apply(State(StateKind::Decay, 10, 5000, now, 5000));
    game::CombatStats t;
    t.phyDefence = 1000;
    decayed.Modify(t);
    CHECK(t.phyDefence == doctest::Approx(300.0));
    // Moves: frostbite halves the speed and doubles the attack time over slow's 75 / 125.
    game::StateSet slow;
    slow.Apply(State(StateKind::Slow, 10, 5000, now));
    CHECK(slow.SpeedPercent() == 75);
    CHECK(slow.AttackTimePercent() == 125);
    slow.Apply(State(StateKind::Frostbite, 30, 5000, now));
    CHECK(slow.SpeedPercent() == 50);
    CHECK(slow.AttackTimePercent() == 200);
    // The client's from the mask its states message gives (its action clips at 100 / percent).
    CHECK(game::AttackTimePercent(game::StateBit(StateKind::Slow)) == 125);
    CHECK(game::AttackTimePercent(game::StateBit(StateKind::Slow) | game::StateBit(StateKind::Frostbite)) == 200);
    CHECK(game::AttackTimePercent(game::StateBit(StateKind::Burn)) == 100);
    CHECK_FALSE(slow.Locked());
    game::StateSet held;
    held.Apply(State(StateKind::Root, 10, 5000, now));
    CHECK(held.Rooted());
    CHECK_FALSE(held.Locked());
    held.Apply(State(StateKind::Sleep, 10, 5000, now));
    CHECK(held.Locked());
    // A hit's damage: the sleep ends; a stun ends one time in four (the target's own history).
    std::mt19937 random(3);
    game::ChanceRolls own;
    CHECK(held.Damaged(own, random) == std::vector{StateKind::Sleep});
    int broken = 0;
    for (int i = 0; i < 400; ++i) {
        game::StateSet stunned;
        stunned.Apply(State(StateKind::Stun, 10, 5000, now));
        broken += stunned.Damaged(own, random).empty() ? 0 : 1;
    }
    CHECK(broken > 80);
    CHECK(broken < 120);
    // Fear makes it idle against the one who feared it; confusion against anyone.
    game::StateSet afraid;
    game::ActiveState fear = State(StateKind::Fear, 10, 5000, now);
    fear.source = 42;
    afraid.Apply(fear);
    CHECK(afraid.Idles(42));
    CHECK_FALSE(afraid.Idles(43));
}

TEST_CASE("states: a knock-down's chance and time, a knock-back's; down attacks and against-a-state in the hit") {
    // chance x 2 x level / (level + the target's level), at most 75.
    CHECK(game::KnockDownChance(85, 50, 80) == 51);
    CHECK(game::KnockDownChance(100, 50, 1) == 75);
    CHECK(game::KnockDownChance(10, 50, 90) == 10);
    CHECK(game::KnockDownMs(3000, 1000) == 4500u);
    std::mt19937 random(9);
    game::ChanceRolls rolls;
    game::AttackSpec a;
    a.skill = 1234;
    a.knockDownLevel = 100;
    a.knockDownChance = 100; // 75 on a level-1 target
    a.knockBackChance = 100;
    a.knockBackDistance = 50;
    int down = 0, back = 0;
    for (int i = 0; i < 400; ++i) {
        const game::Knock k = game::RollKnock(a, 1, true, true, rolls, random);
        down += k == game::Knock::Down ? 1 : 0;
        back += k == game::Knock::Back ? 1 : 0;
    }
    CHECK(down > 270);
    CHECK(down < 330);
    CHECK(back == 400 - down); // a knock-back when it did not fall
    CHECK(game::RollKnock(a, 1, false, true, rolls, random) == game::Knock::Back);
    CHECK(game::RollKnock(a, 1, false, false, rolls, random) == game::Knock::None);

    // The hit: a skill's own damage (no rolls), +50 % against a stunned target, 150 % on a down one.
    game::CombatStats attacker, defender;
    attacker.level = defender.level = 50;
    game::AttackSpec own;
    own.formula = false;
    own.fixedDamage = 1000;
    own.againstMask = 0x41C0;
    own.againstPercent = 50;
    game::HitRoll who;
    CHECK(game::RollHit(attacker, defender, own, who, random).damage == 1000u);
    who.defenderStates = game::StateBit(StateKind::Stun);
    CHECK(game::RollHit(attacker, defender, own, who, random).damage == 1500u);
    who.defenderStates = game::StateBit(StateKind::Burn);
    CHECK(game::RollHit(attacker, defender, own, who, random).damage == 1000u);
    // A down attack: its formula's damage times 1.5 on a target that is down (same rolls both times).
    game::AttackSpec strike;
    strike.downPercent = 150;
    attacker.phyAttackMin = attacker.phyAttackMax = 1000;
    attacker.hitRate = 100;
    defender.evasion = 0;
    std::mt19937 r1(4), r2(4);
    game::HitRoll up, lying;
    lying.defenderDown = true;
    const std::uint32_t standing = game::RollHit(attacker, defender, strike, up, r1).damage;
    const std::uint32_t downed = game::RollHit(attacker, defender, strike, lying, r2).damage;
    CHECK(downed == std::uint32_t(std::trunc(double(standing) * 1.5)));
}

TEST_CASE("states: a monster's defence from its Resist columns") {
    std::array<std::uint8_t, 32> resist{};
    for (std::size_t i = 0; i < resist.size(); ++i) resist[i] = std::uint8_t(i + 1);
    const game::StateDefence d = server::MonsterStateDefence(77, resist);
    CHECK(d.level == 77);
    // Frozen, Frostbite, then EShock before Burn (the elements by their bits), Poison, Zombie.
    CHECK(d.elementResist == std::array<int, 6>{1, 2, 4, 3, 5, 6});
    CHECK(d.percent[0] == 7);  // Sleep
    CHECK(d.percent[5] == 12); // Blood
    CHECK(d.percent[6] == 14); // Dark (Stone skipped)
    CHECK(d.percent[7] == 15); // Stun
    CHECK(d.percent[15] == 23); // CseMP
    CHECK(d.percent[16] == 25); // Bomb (Resist24 skipped)
}

// --- The monsters that take them (game files) ---------------------------------------------------

namespace {

std::filesystem::path GameDir() {
    const char* dir = std::getenv("SR_GAME_DIR");
    return dir ? std::filesystem::path(dir) : std::filesystem::path();
}

struct MonsterField {
    core::JobSystem jobs{2};
    server::World world;
    std::unique_ptr<server::Monsters> monsters;
    server::WorldClock::time_point now = server::WorldClock::now();
    std::uint32_t player = 0, monster = 0;
    game::CombatStats playerStats;
    std::vector<server::Monsters::Player> players;
    server::Monsters::Events events;
    std::vector<server::Monsters::Tick> ticks;
    std::vector<server::WorldMessage> told;
    // Harmony's protection on the player (server::Skills::Protection).
    std::uint8_t protectedGrades = 0;
    int protectedLevel = 0;

    MonsterField(const assets::FileSystem& fs) : world(server::WorldConfig{}, jobs, &fs) {}
    void Step(double seconds) {
        const auto end = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
        while (now < end) {
            now += std::chrono::milliseconds(50);
            std::vector<server::WorldMessage> out;
            world.Update(now, out);
            told.insert(told.end(), out.begin(), out.end());
            const auto place = world.Where(player);
            players.clear();
            if (place) players.push_back({player, place->position, true, &playerStats});
            if (place) {
                players.back().protectedGrades = protectedGrades;
                players.back().protectedLevel = protectedLevel;
            }
            monsters->Update(now, players, events, &ticks);
        }
    }
    bool Moving() const { return world.Moving(monster); }
};

} // namespace

TEST_CASE("states: a monster does what its states say: locked, slowed, knocked down, burning, afraid, woken (game files; K3)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    assets::SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    // A monster that fights and may be knocked down (a low level, enough health for the test's hits
    // and burns), one nest of it beside a player in Constantinople (the world tests' spot).
    const assets::ObjectRow* row = nullptr;
    for (const auto& [code, r] : characters.rows()) {
        if (r.typeId[1] != 2 || r.typeId[2] != 1 || (r.knockdown & 1) == 0 || r.level < 1 || r.level > 30 || r.maxHp < 500 || r.rarity != 0) continue;
        const assets::SkillRow* attack = r.skills[0] != 0 ? skills.Find(r.skills[0]) : nullptr;
        if (attack == nullptr || !attack->attack || attack->range > 100) continue;
        if (row == nullptr || r.refObjId < row->refObjId) row = &r;
    }
    REQUIRE(row != nullptr);
    MESSAGE("the monster: " << row->codename << ", level " << row->level << ", " << row->maxHp << " HP");
    MonsterField f(fs);
    server::WorldPlayer p;
    p.peer = 1;
    p.position = {assets::RegionId(79, 105), 335, 140, 1460};
    p.dropOnGround = true;
    f.player = f.world.Enter(p);
    server::MonsterNest nest;
    nest.refObjId = row->refObjId;
    nest.position = {assets::RegionId(79, 105), 375, 140, 1460};
    nest.radius = 300;
    f.monsters = std::make_unique<server::Monsters>(f.world, characters, &skills, std::vector{nest}, 1);
    f.playerStats.level = 1;
    f.playerStats.maxHp = 1'000'000;
    for (int i = 0; i < 400 && f.monsters->Alive() == 0; ++i) {
        f.Step(0.1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // the navigation loads on the jobs
    }
    REQUIRE(f.monsters->Alive() == 1);
    const auto place = f.world.Where(f.player);
    REQUIRE(place);
    const auto near = f.monsters->Near(place->position, 400);
    REQUIRE(near.size() == 1);
    f.monster = near[0];
    const auto state = [&](StateKind kind, int level, std::uint32_t ms, int value = 0) {
        game::ActiveState s = State(kind, level, ms, f.now, value);
        s.source = f.player;
        if (kind == StateKind::Burn || kind == StateKind::Poison) s.tickMs = 2000;
        return s;
    };
    const auto afflict = [&](std::vector<game::ActiveState> states, game::Knock knock = game::Knock::None, std::uint32_t knockMs = 0) {
        server::Monsters::Affliction a;
        a.states = std::move(states);
        a.knock = knock;
        a.knockMs = knockMs;
        a.from = f.world.Where(f.player)->position;
        return f.monsters->Damage(f.monster, f.player, 1, f.now, &a);
    };
    const auto swingsIn = [&](double seconds) {
        f.events = {};
        f.Step(seconds);
        return f.events.swings.size();
    };

    // A hit turns it on the player: it runs over and swings.
    f.monsters->Damage(f.monster, f.player, 1, f.now);
    CHECK(swingsIn(6.0) > 0);

    // Stunned for 5 s: it stands and does nothing, then fights again.
    server::Monsters::Damaged d = afflict({state(StateKind::Stun, 10, 5000)});
    CHECK(d.statesChanged);
    CHECK(f.monsters->TargetOf(f.monster)->states == game::StateBit(StateKind::Stun));
    CHECK(swingsIn(4.5) == 0);
    CHECK_FALSE(f.Moving());
    f.Step(0.6);
    CHECK(f.monsters->TargetOf(f.monster)->states == 0u);
    CHECK(swingsIn(6.0) > 0);

    // Asleep: a hit with damage wakes it.
    afflict({state(StateKind::Sleep, 10, 20000)});
    CHECK(f.monsters->TargetOf(f.monster)->states == game::StateBit(StateKind::Sleep));
    d = f.monsters->Damage(f.monster, f.player, 5, f.now);
    CHECK(d.statesChanged);
    CHECK(f.monsters->TargetOf(f.monster)->states == 0u);

    // Knocked down: down for its time (only down attacks reach it), no swings, then up.
    d = afflict({}, game::Knock::Down, 4500);
    CHECK(d.knock == game::Knock::Down);
    CHECK(f.monsters->TargetOf(f.monster)->down);
    CHECK_FALSE(f.monsters->TargetOf(f.monster)->canFall);
    CHECK(swingsIn(4.4) == 0);
    f.Step(0.2);
    CHECK_FALSE(f.monsters->TargetOf(f.monster)->down);

    // Burning: a tick at once, then every 2 s, its damage the rate over its magical absorption.
    f.ticks.clear();
    const game::CombatStats* stats = f.monsters->Stats(f.monster);
    REQUIRE(stats != nullptr);
    afflict({state(StateKind::Burn, 20, 7000, 30)});
    f.Step(7.5);
    int burns = 0;
    for (const auto& t : f.ticks) {
        if (t.monster != f.monster || t.tick.kind != StateKind::Burn) continue;
        ++burns;
        CHECK(t.tick.hp == std::uint32_t(std::trunc(float(30.0 / (stats->magAbsorption / 100.0 + 1.0)))));
    }
    CHECK(burns == 4); // 0, 2, 4, 6 s
    CHECK(std::find(f.events.statesChanged.begin(), f.events.statesChanged.end(), f.monster) != f.events.statesChanged.end());

    // Feared by its target: it drops the fight where it stands.
    REQUIRE(f.monsters->Hp(f.monster) > 0);
    f.monsters->Damage(f.monster, f.player, 1, f.now);
    f.Step(1.0);
    afflict({state(StateKind::Fear, 10, 10000)});
    CHECK(swingsIn(5.0) == 0);
}

TEST_CASE("Harmony: a monster of the grade and up to the level it keeps off never starts a fight; one above does; a struck one fights back (game files; K11)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    assets::SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    // A general monster (grade 0: Harmony A..C's 'pola 1') that fights, in an aggressive nest 40 from the player.
    const assets::ObjectRow* row = nullptr;
    for (const auto& [code, r] : characters.rows()) {
        if (r.typeId[1] != 2 || r.typeId[2] != 1 || r.level < 5 || r.level > 30 || r.maxHp < 500 || r.rarity != 0) continue;
        const assets::SkillRow* attack = r.skills[0] != 0 ? skills.Find(r.skills[0]) : nullptr;
        if (attack == nullptr || !attack->attack || attack->range > 100) continue;
        if (row == nullptr || r.refObjId < row->refObjId) row = &r;
    }
    REQUIRE(row != nullptr);
    MESSAGE("the monster: " << row->codename << ", level " << row->level);
    const auto field = [&](std::uint8_t grades, int level) {
        auto f = std::make_unique<MonsterField>(fs);
        server::WorldPlayer p;
        p.peer = 1;
        p.position = {assets::RegionId(79, 105), 335, 140, 1460};
        p.dropOnGround = true;
        f->player = f->world.Enter(p);
        server::MonsterNest nest;
        nest.refObjId = row->refObjId;
        nest.position = {assets::RegionId(79, 105), 375, 140, 1460};
        nest.radius = 300;
        nest.aggressive = true;
        nest.sight = 150;
        f->monsters = std::make_unique<server::Monsters>(f->world, characters, &skills, std::vector{nest}, 1);
        f->playerStats.level = 1;
        f->playerStats.maxHp = 1'000'000;
        f->protectedGrades = grades;
        f->protectedLevel = level;
        for (int i = 0; i < 400 && f->monsters->Alive() == 0; ++i) {
            f->Step(0.1);
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // the navigation loads on the jobs
        }
        REQUIRE(f->monsters->Alive() == 1);
        const auto near = f->monsters->Near(f->world.Where(f->player)->position, 400);
        REQUIRE(near.size() == 1);
        f->monster = near[0];
        return f;
    };
    const auto swingsIn = [](MonsterField& f, double seconds) {
        f.events = {};
        f.Step(seconds);
        std::size_t n = 0;
        for (const auto& s : f.events.swings) n += s.target == f.player;
        return n;
    };
    {
        // Kept off: grade 0, up to its level. It never starts a fight; struck, it fights back.
        auto f = field(1, row->level);
        CHECK(swingsIn(*f, 8.0) == 0);
        f->monsters->Damage(f->monster, f->player, 1, f->now);
        CHECK(swingsIn(*f, 6.0) > 0);
    }
    {
        // One level short of it: it comes for the player.
        auto f = field(1, row->level - 1);
        CHECK(swingsIn(*f, 8.0) > 0);
    }
    {
        // Another grade only (grade 1's bit): a general monster is not kept off.
        auto f = field(2, 999);
        CHECK(swingsIn(*f, 8.0) > 0);
    }
}

TEST_CASE("a monster's swing begun lands though its target ran out of reach, and it stands through it (game files; the user's report 2026-11-14)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    assets::SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    // A general melee monster in an aggressive nest 40 from the player.
    const assets::ObjectRow* row = nullptr;
    for (const auto& [code, r] : characters.rows()) {
        if (r.typeId[1] != 2 || r.typeId[2] != 1 || r.level < 5 || r.level > 30 || r.maxHp < 500 || r.rarity != 0) continue;
        const assets::SkillRow* attack = r.skills[0] != 0 ? skills.Find(r.skills[0]) : nullptr;
        if (attack == nullptr || !attack->attack || attack->range > 30) continue;
        if (row == nullptr || r.refObjId < row->refObjId) row = &r;
    }
    REQUIRE(row != nullptr);
    MonsterField f(fs);
    server::WorldPlayer p;
    p.peer = 1;
    p.position = {assets::RegionId(79, 105), 335, 140, 1460};
    p.dropOnGround = true;
    f.player = f.world.Enter(p);
    server::MonsterNest nest;
    nest.refObjId = row->refObjId;
    nest.position = {assets::RegionId(79, 105), 375, 140, 1460};
    nest.radius = 300;
    nest.aggressive = true;
    nest.sight = 150;
    f.monsters = std::make_unique<server::Monsters>(f.world, characters, &skills, std::vector{nest}, 1);
    f.playerStats.level = 1;
    f.playerStats.maxHp = 1'000'000;
    for (int i = 0; i < 400 && f.monsters->Alive() == 0; ++i) {
        f.Step(0.1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // the navigation loads on the jobs
    }
    REQUIRE(f.monsters->Alive() == 1);
    const auto near = f.monsters->Near(f.world.Where(f.player)->position, 400);
    REQUIRE(near.size() == 1);
    f.monster = near[0];
    // Its first swing at the player.
    const assets::SkillRow* swung = nullptr;
    for (int i = 0; i < 200 && swung == nullptr; ++i) {
        f.events = {};
        f.Step(0.05);
        for (const auto& s : f.events.swings) {
            if (s.target == f.player) swung = skills.Find(s.skill);
        }
    }
    REQUIRE(swung != nullptr);
    // The player gone 150 off at once (a run, far beyond its reach): the swing still lands, the monster standing.
    f.world.Teleport(f.player, {assets::RegionId(79, 105), 335, 140, 1460 + 150});
    f.events = {};
    bool moved = false;
    for (int i = 0; i * 50 < std::max(500, swung->durationMs) - 100; ++i) {
        f.Step(0.05);
        moved = moved || f.Moving();
    }
    std::size_t landed = 0;
    for (const auto& h : f.events.hits) landed += h.target == f.player;
    CHECK(landed > 0);
    CHECK_FALSE(moved);
    // Its swing over, it runs after the player again.
    for (int i = 0; i < 40 && !f.Moving(); ++i) f.Step(0.05);
    CHECK(f.Moving());
}

TEST_CASE("a skill that costs its caster HP ('pmhp' kind 0): the Qin tomb's bomb stone ghost's blast lands, then the ghost dies by it (game files; U1-02)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    assets::CharacterTable all;
    REQUIRE_MESSAGE(all.Load(fs, error), error);
    assets::SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    const assets::ObjectRow* ghost = all.Find("MOB_TQ_BOMBSTONEGHOST");
    const assets::SkillRow* blast = skills.Find("MSKILL_TQ_BOMBSTONEGHOST_ATTACK05");
    REQUIRE(ghost != nullptr);
    REQUIRE(blast != nullptr);
    const assets::SkillParam cost = assets::FindSkillParam(*blast, assets::SkillTag("pmhp"));
    REQUIRE(cost);
    CHECK(cost[0] == 5000); // no recovery for 5 s
    CHECK(cost[2] == 100);  // 100 % of its HP
    CHECK(cost[3] == 0);    // kind 0: a hit on itself
    // The ghost with its blast alone, in an aggressive nest 40 from the player.
    assets::ObjectRow row = *ghost;
    row.skills = {};
    row.skills[0] = blast->id;
    assets::CharacterTable characters;
    characters.Add(row);
    MonsterField f(fs);
    server::WorldPlayer p;
    p.peer = 1;
    p.position = {assets::RegionId(79, 105), 335, 140, 1460};
    p.dropOnGround = true;
    f.player = f.world.Enter(p);
    server::MonsterNest nest;
    nest.refObjId = row.refObjId;
    nest.position = {assets::RegionId(79, 105), 375, 140, 1460};
    nest.radius = 300;
    nest.aggressive = true;
    nest.sight = 150;
    f.monsters = std::make_unique<server::Monsters>(f.world, characters, &skills, std::vector{nest}, 1);
    f.playerStats.level = 1;
    f.playerStats.maxHp = 1'000'000;
    for (int i = 0; i < 400 && f.monsters->Alive() == 0; ++i) {
        f.Step(0.1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // the navigation loads on the jobs
    }
    REQUIRE(f.monsters->Alive() == 1);
    f.monster = f.monsters->Near(f.world.Where(f.player)->position, 400).at(0);
    // Its blast swung at the player: the hits land first, then its cost kills it (a plain hit, no attacker, no share).
    bool swung = false;
    for (int i = 0; i < 300 && !swung; ++i) {
        f.events = {};
        f.Step(0.05);
        for (const auto& s : f.events.swings) swung = swung || (s.target == f.player && s.skill == blast->id);
    }
    REQUIRE(swung);
    std::size_t landed = 0;
    const server::Monsters::Tick* own = nullptr;
    f.ticks.clear();
    for (int i = 0; i * 50 < std::max(500, blast->durationMs) + 500 && own == nullptr; ++i) {
        f.Step(0.05);
        for (const auto& h : f.events.hits) landed += h.target == f.player;
        for (const auto& t : f.ticks) {
            if (t.monster == f.monster && t.self) own = &t;
        }
    }
    CHECK(landed > 0);
    REQUIRE(own != nullptr);
    CHECK(own->damaged.killed);
    CHECK(own->damaged.killer == 0);
    CHECK(f.monsters->CorpseOf(f.monster).has_value()); // lying dead
}

TEST_CASE("a chaser runs after a running player without stopping at each think (game files; the user's clip 2026-11-14)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    assets::SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    const assets::ObjectRow* row = nullptr;
    for (const auto& [code, r] : characters.rows()) {
        if (r.typeId[1] != 2 || r.typeId[2] != 1 || r.level < 5 || r.level > 30 || r.maxHp < 500 || r.rarity != 0 || r.runSpeed <= 0) continue;
        const assets::SkillRow* attack = r.skills[0] != 0 ? skills.Find(r.skills[0]) : nullptr;
        if (attack == nullptr || !attack->attack || attack->range > 30) continue;
        if (row == nullptr || r.refObjId < row->refObjId) row = &r;
    }
    REQUIRE(row != nullptr);
    MonsterField f(fs);
    server::WorldPlayer p;
    p.peer = 1;
    p.position = {assets::RegionId(79, 105), 335, 140, 1460};
    p.dropOnGround = true;
    f.player = f.world.Enter(p);
    server::MonsterNest nest;
    nest.refObjId = row->refObjId;
    nest.position = {assets::RegionId(79, 105), 375, 140, 1460};
    nest.radius = 300;
    nest.aggressive = true;
    nest.sight = 150;
    f.monsters = std::make_unique<server::Monsters>(f.world, characters, &skills, std::vector{nest}, 1);
    f.playerStats.level = 1;
    f.playerStats.maxHp = 1'000'000;
    for (int i = 0; i < 400 && f.monsters->Alive() == 0; ++i) {
        f.Step(0.1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(f.monsters->Alive() == 1);
    f.monster = f.monsters->Near(f.world.Where(f.player)->position, 400)[0];
    // Engaged: its first swing.
    bool swung = false;
    for (int i = 0; i < 200 && !swung; ++i) {
        f.events = {};
        f.Step(0.05);
        for (const auto& s : f.events.swings) swung = swung || s.target == f.player;
    }
    REQUIRE(swung);
    f.Step(2.0); // its swing played out
    // The player runs off south in a straight line, far: once after it, the chaser keeps running (before: a stop at
    // each think, when it reached the spot the player had left).
    REQUIRE(f.world.MoveTo(f.player, {assets::RegionId(79, 105), 335, 140, 1460 - 250}) == server::World::MoveRefused::None);
    // A stop not for a swing (one within the next 0.3 s) is the stutter.
    int stops = 0;
    bool was = false, started = false;
    std::string timeline;
    int stoppedAt = -1;
    for (int i = 0; i < 40; ++i) { // 2 s
        f.events = {};
        f.Step(0.05);
        const bool moving = f.Moving();
        bool swing = false;
        for (const auto& s : f.events.swings) swing = swing || s.target == f.player;
        timeline += swing ? 'S' : moving ? '-' : '.';
        started = started || moving;
        if (started && was && !moving) stoppedAt = i;
        if (swing) stoppedAt = -1;
        if (stoppedAt >= 0 && i - stoppedAt >= 6) {
            ++stops;
            stoppedAt = -1;
        }
        was = moving;
    }
    CHECK(started);
    MESSAGE("the chase (- running, . standing, S a swing): " << timeline);
    CHECK(stops == 0);
}

TEST_CASE("threat: a monster keeps two opponents, turns to the one that threatens it more, idles when its target's threat is cut to nothing (game files; K12)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    assets::SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    const assets::ObjectRow* row = nullptr;
    for (const auto& [code, r] : characters.rows()) {
        if (r.typeId[1] != 2 || r.typeId[2] != 1 || r.level < 5 || r.level > 30 || r.maxHp < 5000 || r.rarity != 0) continue;
        const assets::SkillRow* attack = r.skills[0] != 0 ? skills.Find(r.skills[0]) : nullptr;
        if (attack == nullptr || !attack->attack || attack->range > 100) continue;
        if (row == nullptr || r.refObjId < row->refObjId) row = &r;
    }
    REQUIRE(row != nullptr);
    MonsterField f(fs);
    server::WorldPlayer p;
    p.peer = 1;
    p.position = {assets::RegionId(79, 105), 335, 140, 1460};
    p.dropOnGround = true;
    f.player = f.world.Enter(p);
    server::WorldPlayer q = p;
    q.peer = 2;
    q.position.x = 345;
    const std::uint32_t other = f.world.Enter(q);
    server::MonsterNest nest;
    nest.refObjId = row->refObjId;
    nest.position = {assets::RegionId(79, 105), 375, 140, 1460};
    nest.radius = 300;
    f.monsters = std::make_unique<server::Monsters>(f.world, characters, &skills, std::vector{nest}, 1);
    f.playerStats.level = 1;
    f.playerStats.maxHp = 1'000'000;
    // Both players for the monsters (the field's Step gives only the first).
    const auto step = [&](double seconds) {
        const auto end = f.now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
        while (f.now < end) {
            f.now += std::chrono::milliseconds(50);
            std::vector<server::WorldMessage> out;
            f.world.Update(f.now, out);
            f.players.clear();
            for (const std::uint32_t e : {f.player, other}) {
                if (const auto place = f.world.Where(e)) f.players.push_back({e, place->position, true, &f.playerStats});
            }
            f.monsters->Update(f.now, f.players, f.events, &f.ticks);
        }
    };
    for (int i = 0; i < 400 && f.monsters->Alive() == 0; ++i) {
        step(0.1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(f.monsters->Alive() == 1);
    const auto near = f.monsters->Near(f.world.Where(f.player)->position, 400);
    REQUIRE(near.size() == 1);
    const std::uint32_t mob = near[0];
    const auto hit = [&](std::uint32_t who, std::uint32_t damage, std::int64_t threat, int percent = 0) {
        server::Monsters::Affliction a;
        a.threatGiven = true;
        a.threat = threat;
        a.threatPercent = percent;
        a.from = f.world.Where(who)->position;
        f.monsters->Damage(mob, who, damage, f.now, &a);
        step(0.1);
    };
    // The first to hit engages it, its threat x 1.15 truncated (114: 1.15 is not exact, as the original's double).
    hit(f.player, 10, 100);
    CHECK(f.monsters->CurrentTarget(mob) == std::pair<std::uint32_t, std::int64_t>{f.player, 114});
    // A second opponent: recorded; as much threat (114) keeps the current, more turns it.
    hit(other, 10, 114);
    CHECK(f.monsters->CurrentTarget(mob).first == f.player);
    hit(other, 10, 1);
    CHECK(f.monsters->CurrentTarget(mob) == std::pair<std::uint32_t, std::int64_t>{other, 115});
    // A heal's threat (the first heals: from it, to the monster fighting the healed): 164 against 115.
    f.monsters->AddThreat(mob, f.player, 50, f.now);
    CHECK(f.monsters->CurrentTarget(mob) == std::pair<std::uint32_t, std::int64_t>{f.player, 164});
    // A cut of the current target's threat to nothing (a dtnt): the monster idles and forgets both.
    hit(f.player, 0, -1000);
    CHECK(f.monsters->CurrentTarget(mob).first == 0u);
}

TEST_CASE("time bomb: its fuse one of its first word's bytes in seconds; at its natural end the holder takes its damage and the bomber's threat (game files; K12)") {
    // The roll: SKILL_EU_WARLOCK_BLOODA_EXPLOSION_B's 'tb 84545280 100 9 8239'.
    std::mt19937 random(3);
    game::ChanceRolls rolls;
    const auto start = Clock::now();
    std::set<std::uint32_t> fuses;
    for (int i = 0; i < 60; ++i) {
        const auto took = game::RollStates(std::vector{Gift(StateKind::TimeBomb, {84545280, 100, 9, 8239})}, Defence(1), 0, rolls, random, 7, 99, 0, start);
        REQUIRE(took.size() == 1);
        CHECK(took[0].kind == StateKind::TimeBomb);
        CHECK(took[0].value == 8239);
        CHECK(took[0].level == 9);
        fuses.insert(took[0].durationMs);
    }
    CHECK(fuses == std::set<std::uint32_t>{5000, 10000, 15000});
    CHECK(game::ResistBucket(StateKind::TimeBomb) == 16);
    CHECK(game::StateBit(StateKind::TimeBomb) == 0x1000000u);

    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    assets::SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    const assets::ObjectRow* row = nullptr;
    for (const auto& [code, r] : characters.rows()) {
        if (r.typeId[1] != 2 || r.typeId[2] != 1 || r.level < 5 || r.level > 30 || r.maxHp < 5000 || r.rarity != 0) continue;
        const assets::SkillRow* attack = r.skills[0] != 0 ? skills.Find(r.skills[0]) : nullptr;
        if (attack == nullptr || !attack->attack || attack->range > 100) continue;
        if (row == nullptr || r.refObjId < row->refObjId) row = &r;
    }
    REQUIRE(row != nullptr);
    MonsterField f(fs);
    server::WorldPlayer p;
    p.peer = 1;
    p.position = {assets::RegionId(79, 105), 335, 140, 1460};
    p.dropOnGround = true;
    f.player = f.world.Enter(p);
    server::MonsterNest nest;
    nest.refObjId = row->refObjId;
    nest.position = {assets::RegionId(79, 105), 375, 140, 1460};
    nest.radius = 300;
    f.monsters = std::make_unique<server::Monsters>(f.world, characters, &skills, std::vector{nest}, 1);
    f.playerStats.level = 1;
    f.playerStats.maxHp = 1'000'000;
    for (int i = 0; i < 400 && f.monsters->Alive() == 0; ++i) {
        f.Step(0.1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(f.monsters->Alive() == 1);
    const auto near = f.monsters->Near(f.world.Where(f.player)->position, 400);
    REQUIRE(near.size() == 1);
    const std::uint32_t mob = near[0];
    const auto bomb = [&](std::uint32_t ms) {
        game::ActiveState s = State(StateKind::TimeBomb, 9, ms, f.now, 300);
        s.source = f.player;
        s.skill = 1234;
        server::Monsters::Affliction a;
        a.states = {s};
        a.threatGiven = true; // a bomb alone gives no threat as it is set
        a.from = f.world.Where(f.player)->position;
        return f.monsters->Damage(mob, f.player, 0, f.now, &a);
    };
    // Set on an idle monster: no threat yet, it stays idle; nothing bursts before its fuse is out.
    const server::Monsters::Damaged set = bomb(5000);
    CHECK(set.statesChanged);
    const std::uint32_t hpBefore = set.hp;
    f.ticks.clear();
    f.Step(4.9);
    CHECK(f.ticks.empty());
    CHECK(f.monsters->CurrentTarget(mob).first == 0u);
    // Its natural end: the holder takes the damage (fixed), from the bomber, its skill's strike; and
    // the bomber's threat as much (x 1.15 engaging: 345).
    f.Step(0.3);
    REQUIRE(f.ticks.size() == 1);
    CHECK(f.ticks[0].tick.kind == StateKind::TimeBomb);
    CHECK(f.ticks[0].tick.hp == 300u);
    CHECK(f.ticks[0].tick.skill == 1234u);
    CHECK(f.ticks[0].tick.source == f.player);
    CHECK(f.ticks[0].damaged.hp == hpBefore - 300);
    CHECK(f.monsters->CurrentTarget(mob) == std::pair<std::uint32_t, std::int64_t>{f.player, 345});
}

TEST_CASE("static monsters: walk and run speeds 0 (Hotan's Blood Devils, the trunks, the traps) never wander or chase; others do (game files)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    assets::SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    const assets::ObjectRow* flower = characters.Find(std::string_view("MOB_KT_DESERTTRUNKZ_CLON"));
    const assets::ObjectRow* walker = characters.Find(std::string_view("MOB_KT_BUNWANG"));
    REQUIRE(flower != nullptr);
    REQUIRE(walker != nullptr);
    CHECK(flower->walkSpeed == 0.0f);
    CHECK(flower->runSpeed == 0.0f);
    MonsterField f(fs);
    server::WorldPlayer p;
    p.peer = 1;
    p.position = {assets::RegionId(79, 105), 335, 140, 1460};
    p.dropOnGround = true;
    f.player = f.world.Enter(p);
    f.playerStats.level = 1;
    f.playerStats.maxHp = 1'000'000;
    server::MonsterNest a, b;
    a.refObjId = flower->refObjId;
    a.position = {assets::RegionId(79, 105), 535, 140, 1460};
    a.radius = 300;
    b.refObjId = walker->refObjId;
    b.position = {assets::RegionId(79, 105), 535, 140, 1660};
    b.radius = 300;
    f.monsters = std::make_unique<server::Monsters>(f.world, characters, &skills, std::vector{a, b}, 1);
    for (int i = 0; i < 400 && f.monsters->Alive() < 2; ++i) {
        f.Step(0.1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(f.monsters->Alive() == 2);
    std::uint32_t flowerId = 0, walkerId = 0;
    for (const std::uint32_t id : f.monsters->Near(a.position, 150)) flowerId = id;
    for (const std::uint32_t id : f.monsters->Near(b.position, 150)) walkerId = id;
    REQUIRE(flowerId != 0);
    REQUIRE(walkerId != 0);
    const auto Distance = [](const sr::game::proto::WorldPosition& u, const sr::game::proto::WorldPosition& v) {
        const sr::game::proto::FlatPosition x = sr::game::proto::ToFlat(u), y = sr::game::proto::ToFlat(v);
        return float(std::hypot(x.x - y.x, x.z - y.z));
    };
    const sr::game::proto::WorldPosition flowerAt = f.world.Where(flowerId)->position, walkerAt = f.world.Where(walkerId)->position;
    // Idle for 40 s: the flower stays put; the other wanders in its circle.
    f.Step(40.0);
    CHECK(Distance(f.world.Where(flowerId)->position, flowerAt) < 0.01f);
    CHECK(Distance(f.world.Where(walkerId)->position, walkerAt) > 1.0f);
    // Hit from 200 away: the flower does not come after the player; the other does. The walker first put 200 from the
    // player where the straight way between them is clear (its wander may have ended behind a wall, where a chase steps
    // back by the original's probe, Monsters' WayRound; Constantinople's streets have many).
    const sr::game::proto::WorldPosition me = f.world.Where(f.player)->position;
    const auto clear = [&](const sr::game::proto::WorldPosition& u, const sr::game::proto::WorldPosition& v) {
        const auto end = f.world.WalkEnd(u, v);
        return end && Distance(*end, v) < 1.0f;
    };
    bool placed = false;
    for (int k = 0; k < 24 && !placed; ++k) {
        const double t = double(k) * 2.0 * 3.14159265358979 / 24.0;
        const sr::game::proto::FlatPosition c = sr::game::proto::ToFlat(me);
        const sr::game::proto::WorldPosition spot = sr::game::proto::FromFlat({c.x + 200.0 * std::cos(t), c.z + 200.0 * std::sin(t)}, me.y);
        if (!clear(me, spot)) continue;
        REQUIRE(f.world.Teleport(walkerId, *f.world.WalkEnd(me, spot)));
        f.Step(0.5);
        placed = clear(f.world.Where(walkerId)->position, me);
    }
    REQUIRE(placed);
    for (const std::uint32_t mob : {flowerId, walkerId}) {
        server::Monsters::Affliction hit;
        hit.from = f.world.Where(f.player)->position;
        f.monsters->Damage(mob, f.player, 1, f.now, &hit);
    }
    const sr::game::proto::WorldPosition flowerBefore = f.world.Where(flowerId)->position;
    f.Step(5.0);
    CHECK(Distance(f.world.Where(flowerId)->position, flowerBefore) < 0.01f);
    CHECK(Distance(f.world.Where(walkerId)->position, f.world.Where(f.player)->position) < 120.0f);
}

TEST_CASE("a player's resistances to states: the jewellery's options and its passives; a freeze on it weaker") {
    using namespace sr::game;
    // Two options as magicoption.txt names them, worn on a ring.
    std::vector<sr::assets::MagicOptionRow> options(2);
    options[0].paramId = 1;
    options[0].optionName = "MATTR_RESIST_FROSTBITE";
    options[1].paramId = 2;
    options[1].optionName = "MATTR_RESIST_STUN";
    ItemInstance ring;
    ring.options = {{1, 40}, {2, 15}};
    sr::assets::ItemRow row;
    const std::vector<WornItem> worn{{11, &row, &ring}};
    Passives passives;
    passives.resistFlat[0] = 10; // a passive's sleep resistance
    passives.resistGrade[0] = 3;
    const StateDefence d = PlayerStateDefence(80, worn, &options, &passives);
    CHECK(d.level == 80);
    CHECK(d.elementResist[0] == 40); // freeze
    CHECK(d.elementResist[1] == 40); // frostbite (the same option)
    CHECK(d.elementResist[2] == 0);
    CHECK(d.percent[7] == 15); // stun's bucket
    CHECK(d.flat[0] == 10);
    CHECK(d.grade[0] == 3);
    // A freeze of power 100: level 100 without the ring, 60 with it (100 - 40 %), its time likewise shorter.
    StateGift freeze;
    freeze.kind = StateKind::Freeze;
    freeze.words = {100, 100, 0, 0, 0, 0};
    StateDefence bare;
    bare.level = 80;
    const StateOdds without = OddsOf(freeze, bare, 0), with = OddsOf(freeze, d, 0);
    CHECK(without.level == 100);
    CHECK(with.level == 60);
    CHECK(with.durationMs < without.durationMs);
}
