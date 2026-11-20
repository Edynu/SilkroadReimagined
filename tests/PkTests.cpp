// Murderers' rules (step 1): the PK counts of a kill, their relief, the death's numbers and the
// item drop's chance, the killer's experience, the 48 h keeper's walk and the attack check's order.

#include <doctest/doctest.h>

#include "sr/game/Heals.h"
#include "sr/game/Pk.h"
#include "sr/game/PkProtocol.h"

using namespace sr;
namespace proto = sr::game::proto;
using game::AttackMode;
using game::KillRelation;
using game::PvpState;
using S = proto::SkillUseStatus;

TEST_CASE("PK: a murder's counts: the worked example, the points by TotalPK, DailyPK by the gap") {
    // A level 60 kills an innocent level 50, his first murder: TotalPK 1, points 1,200, DailyPK 1 + 2 x 2 = 5.
    CHECK(game::PenaltyForMurder(1) == 1'200);
    CHECK(game::DailyForMurder(60, 50) == 5);
    // 1,200 for the 1st and 2nd, 2,400 for the 3rd and 4th, ... 9,600 for the 15th.
    CHECK(game::PenaltyForMurder(2) == 1'200);
    CHECK(game::PenaltyForMurder(3) == 2'400);
    CHECK(game::PenaltyForMurder(4) == 2'400);
    CHECK(game::PenaltyForMurder(5) == 3'600);
    CHECK(game::PenaltyForMurder(15) == 9'600);
    // A victim of his level or higher: 1; 4 below: 1; 5 below: 3; 9 below 3; 10 below 5.
    CHECK(game::DailyForMurder(50, 50) == 1);
    CHECK(game::DailyForMurder(50, 70) == 1);
    CHECK(game::DailyForMurder(54, 50) == 1);
    CHECK(game::DailyForMurder(55, 50) == 3);
    CHECK(game::DailyForMurder(59, 50) == 3);
    // Four such murders and the day's 15 is reached (5 x 3 = 15).
    CHECK(3 * game::DailyForMurder(60, 50) == game::kDailyPkMost);
}

TEST_CASE("PK: the relief: the monster table, the deaths") {
    CHECK(game::MonsterKillRelief(64, 60) == 10);
    CHECK(game::MonsterKillRelief(90, 60) == 10);
    CHECK(game::MonsterKillRelief(63, 60) == 7);
    CHECK(game::MonsterKillRelief(61, 60) == 7);
    CHECK(game::MonsterKillRelief(60, 60) == 5);
    CHECK(game::MonsterKillRelief(57, 60) == 5);
    CHECK(game::MonsterKillRelief(56, 60) == 3);
    CHECK(game::MonsterKillRelief(54, 60) == 3);
    CHECK(game::MonsterKillRelief(53, 60) == 1);
    CHECK(game::MonsterKillRelief(1, 60) == 1);
    // 1,200 points: 240 monsters of his level, or two deaths to monsters.
    CHECK(240 * game::MonsterKillRelief(60, 60) == 1'200);
    CHECK(game::DeathRelief(KillRelation::Monster) == 600);
    CHECK(game::DeathRelief(KillRelation::GuildWar) == 200);
    CHECK(game::DeathRelief(KillRelation::Open) == 0);
    CHECK(game::DeathRelief(KillRelation::Job) == 0);
    CHECK(game::DeathTakesDaily(KillRelation::Monster));
    CHECK(game::DeathTakesDaily(KillRelation::GuildWar));
    CHECK_FALSE(game::DeathTakesDaily(KillRelation::Open));
}

TEST_CASE("PK: a death's item drop by points, at its edges, and the relations and levels that drop") {
    CHECK(game::DeathItemChance(0) == 5);
    CHECK(game::DeathItemChance(1) == 30);
    CHECK(game::DeathItemChance(3'999) == 30);
    CHECK(game::DeathItemChance(4'000) == 50);
    CHECK(game::DeathItemChance(14'999) == 50);
    CHECK(game::DeathItemChance(15'000) == 70);
    CHECK(game::DeathItemChance(29'999) == 70);
    CHECK(game::DeathItemChance(30'000) == 100);
    CHECK(game::DeathItemChance(200'000) == 100);
    CHECK(game::DeathMayDrop(KillRelation::Monster, 11));
    CHECK(game::DeathMayDrop(KillRelation::Open, 50));
    CHECK(game::DeathMayDrop(KillRelation::GuildWar, 50));
    CHECK_FALSE(game::DeathMayDrop(KillRelation::Monster, 10));
    CHECK_FALSE(game::DeathMayDrop(KillRelation::Job, 50));
    CHECK_FALSE(game::DeathMayDrop(KillRelation::Team, 50));
    CHECK_FALSE(game::DeathMayDrop(KillRelation::FortressWar, 50));
    // The murderer's equipment roll: a place 0..12; the weapon, the job suit and a quiver's place again among 0..5.
    CHECK(game::MurdererEquipmentPlace(3, 5, false) == 3);
    CHECK(game::MurdererEquipmentPlace(6, 2, false) == 2);
    CHECK(game::MurdererEquipmentPlace(8, 4, false) == 4);
    CHECK(game::MurdererEquipmentPlace(7, 1, false) == 7);
    CHECK(game::MurdererEquipmentPlace(7, 1, true) == 1);
    CHECK(game::MurdererEquipmentPlace(12, 0, false) == 12);
}

TEST_CASE("PK: the death's experience rows: a murderer's 2 % / x 100 and 6 % / x 300, not in the open field") {
    game::DeathFacts f;
    f.level = 60;
    f.levelExperience = 10'000'000;
    f.monsterExperience = 1'000'000; // the cap never binds: the share decides
    // An innocent: 2 % to a monster, 0.4 % to a player.
    CHECK(game::DeathExperienceLoss(f) == 199'999); // the float's 0.0199999995
    f.byPlayer = true;
    CHECK(game::DeathExperienceLoss(f) == 40'000);
    // A murderer: 2 % to a player, 6 % to a monster.
    f.murderer = true;
    CHECK(game::DeathExperienceLoss(f) == 199'999);
    f.byPlayer = false;
    CHECK(game::DeathExperienceLoss(f) == 599'999);
    // The cap: the monster experience x 300 for a murderer killed by a monster, halved by a thief monster.
    f.monsterExperience = 1'000;
    CHECK(game::DeathExperienceLoss(f) == 300'000);
    f.thiefMonster = true;
    CHECK(game::DeathExperienceLoss(f) == 150'000);
    // Level 10: nothing.
    f.level = 10;
    CHECK(game::DeathExperienceLoss(f) == 0);
    // Which deaths take the murderer's numbers: points and not relation 5 (a murderer killed by a player in the open).
    CHECK(game::MurdererNumbers(1'200, KillRelation::Monster));
    CHECK(game::MurdererNumbers(1'200, KillRelation::GuildWar));
    CHECK_FALSE(game::MurdererNumbers(1'200, KillRelation::Open));
    CHECK_FALSE(game::MurdererNumbers(0, KillRelation::Monster));
    CHECK(game::MurdererSkillPointLoss(true) == 20);
    CHECK(game::MurdererSkillPointLoss(false) == 60);
    // The existing DeathPenalty agrees with the innocent's row.
    CHECK(game::DeathPenalty(60, 10'000'000, 1'000'000, 0.0, true) == 40'000);
    // A murderer raised by a resurrection gets half the share: 50 % of 1,000 -> 250.
    game::ReviveSpec revive;
    revive.maxLevel = 100;
    revive.experiencePercent = 50;
    CHECK(game::RevivalFor(revive, 100, 100, 1'000).experience == 500);
    CHECK(game::RevivalFor(revive, 100, 100, 1'000, true).experience == 250);
}

TEST_CASE("PK: the killer's experience: the gap and party factors, doubled for a murderer") {
    CHECK(game::PvpLevelGapFactor(60, 60) == doctest::Approx(1.0));
    CHECK(game::PvpLevelGapFactor(70, 60) == doctest::Approx(1.0));
    CHECK(game::PvpLevelGapFactor(75, 60) == doctest::Approx(0.9));
    CHECK(game::PvpLevelGapFactor(56, 60) == doctest::Approx(0.9));
    CHECK(game::PvpLevelGapFactor(53, 60) == doctest::Approx(0.4));
    CHECK(game::PvpLevelGapFactor(1, 60) == doctest::Approx(0.1));
    // The party factor: 1 when the victim is 3 below; else 1 + 0.03 x min(d + 3, 13) (+ the shared party's bonus), 1..4.
    CHECK(game::PvpPartyFactor(57, 60, true, true) == doctest::Approx(1.0));
    CHECK(game::PvpPartyFactor(60, 60, false, false) == doctest::Approx(1.09).epsilon(1e-6));
    CHECK(game::PvpPartyFactor(80, 60, false, false) == doctest::Approx(1.39).epsilon(1e-6));
    CHECK(game::PvpPartyFactor(69, 60, true, true) == doctest::Approx(1.0 + 0.9 + 0.36).epsilon(1e-6));
    CHECK(game::PvpPartyFactor(69, 60, true, false) == doctest::Approx(1.0 + 0.45 + 0.36).epsilon(1e-6));
    // 1,000 x 1 x rate 1 x 1.09 = 1,090; doubled 2,180; the rate x 10: 10,900.
    CHECK(game::PvpExperience(1'000, 60, 60, 1.0, false, false, false) == 1'090);
    CHECK(game::PvpExperience(1'000, 60, 60, 1.0, false, false, true) == 2'180);
    CHECK(game::PvpExperience(1'000, 60, 60, 10.0, false, false, false) == 10'900);
}

TEST_CASE("PK: the 48 h keeper's catch-up over 0, 47, 48, 100 and 2,000 hours") {
    const std::int64_t h = 3600;
    // TotalPK 3 with a fresh period.
    auto s = game::KeeperAfter(3, game::kKeeperSeconds, 0);
    CHECK(s.totalPk == 3);
    CHECK(s.secondsLeft == game::kKeeperSeconds);
    CHECK_FALSE(s.cleared);
    s = game::KeeperAfter(3, game::kKeeperSeconds, 47 * h);
    CHECK(s.totalPk == 3);
    CHECK(s.secondsLeft == h);
    s = game::KeeperAfter(3, game::kKeeperSeconds, 48 * h);
    CHECK(s.totalPk == 2);
    CHECK(s.taken == 1);
    CHECK(s.secondsLeft == game::kKeeperSeconds);
    s = game::KeeperAfter(3, game::kKeeperSeconds, 100 * h);
    CHECK(s.totalPk == 1);
    CHECK(s.secondsLeft == 44 * h);
    s = game::KeeperAfter(3, game::kKeeperSeconds, 2'000 * h);
    CHECK(s.totalPk == 0);
    CHECK(s.taken == 3);
    CHECK(s.cleared);
    // Half a period left: 24 h takes one.
    s = game::KeeperAfter(2, 24 * h, 24 * h);
    CHECK(s.totalPk == 1);
}

namespace {
game::PvpPair Pair(int strikerLevel, int targetLevel) {
    game::PvpPair p;
    p.striker.level = strikerLevel;
    p.target.level = targetLevel;
    return p;
}
} // namespace

TEST_CASE("PK: the attack check's order") {
    // Two innocents of level 20+ on a battlefield: a PK attack.
    auto p = Pair(30, 30);
    auto v = game::AttackCheck(p);
    CHECK(v.allowed);
    CHECK(v.pk);
    // Not on a battlefield; one party.
    p.target.battlefield = false;
    CHECK(game::AttackCheck(p).code == S::NotBattlefield);
    p.target.battlefield = true;
    p.sameParty = true;
    CHECK(game::AttackCheck(p).code == S::PartyMember);
    p.sameParty = false;
    // A murderer attacked: allowed, no PK, no record; even by one at the daily limit.
    p.target.state = PvpState::Murderer;
    p.target.points = 1'200;
    p.striker.dailyPk = 15;
    v = game::AttackCheck(p);
    CHECK(v.allowed);
    CHECK_FALSE(v.pk);
    CHECK_FALSE(game::HitMakesRecord(p));
    CHECK_FALSE(game::KillIsMurder(p));
    // The daily limit only for innocents: 0x3014.
    p.target.state = PvpState::Neutral;
    p.target.points = 0;
    CHECK(game::AttackCheck(p).code == S::DailyPkLimit);
    // A record held on him: allowed again.
    p.strikerHolds = true;
    CHECK(game::AttackCheck(p).allowed);
    p.strikerHolds = false;
    p.striker.dailyPk = 0;
    // The points at 200,000: 0x3015.
    p.striker.points = 200'000;
    CHECK(game::AttackCheck(p).code == S::TotalPkLimit);
    p.striker.points = 0;
    // A level 19 striker, a level 19 target.
    CHECK(game::AttackCheck(Pair(19, 30)).code == S::YourLevelTooLow);
    CHECK(game::AttackCheck(Pair(30, 19)).code == S::TargetLevelTooLow);
    // An assaulter attacked: allowed without a PK; the hit makes a record (striking an assaulter makes one an assaulter).
    p.target.state = PvpState::Assaulter;
    p.target.hasRecords = true;
    v = game::AttackCheck(p);
    CHECK(v.allowed);
    CHECK_FALSE(v.pk);
    CHECK(game::HitMakesRecord(p));
    CHECK_FALSE(game::KillIsMurder(p));
    // An area's mode 3: an innocent's area never hits an assaulter, nor a new innocent victim; a murderer yes.
    p.mode = AttackMode::Area;
    CHECK_FALSE(game::AttackCheck(p).allowed);
    p.target.state = PvpState::Neutral;
    p.target.hasRecords = false;
    CHECK_FALSE(game::AttackCheck(p).allowed);
    p.strikerHolds = true;
    CHECK(game::AttackCheck(p).allowed);
    p.strikerHolds = false;
    p.target.state = PvpState::Murderer;
    p.target.points = 1;
    CHECK(game::AttackCheck(p).allowed);
    p.mode = AttackMode::Chosen;
    p.target.state = PvpState::Neutral;
    p.target.points = 0;
    // An innocent struck: a record; a kill of him a murder.
    CHECK(game::HitMakesRecord(p));
    CHECK(game::KillIsMurder(p));
    // The PK switch: innocents and hostile jobs refused (0x3025); guild wars and free PvP stay.
    p.pkProhibited = true;
    CHECK(game::AttackCheck(p).code == S::PkProhibited);
    p.hostileJobs = true;
    CHECK(game::AttackCheck(p).code == S::PkProhibited);
    p.hostileJobs = false;
    p.guildWar = true;
    CHECK(game::AttackCheck(p).allowed);
    p.guildWar = false;
    p.freePvpFoes = true;
    CHECK(game::AttackCheck(p).allowed);
    CHECK_FALSE(game::HitMakesRecord(p));
    p.freePvpFoes = false;
    p.pkProhibited = false;
    // Hostile jobs: allowed, but not in the suit lock (0x3019); safe trade's code before the guild war.
    p.hostileJobs = true;
    p.jobLock = true;
    CHECK(game::AttackCheck(p).code == S::JobLock);
    p.jobLock = false;
    CHECK(game::AttackCheck(p).allowed);
    p.safeTrade = S::SafeTrade;
    p.guildWar = true;
    CHECK(game::AttackCheck(p).code == S::SafeTrade);
}

TEST_CASE("PK: the messages round-trip (0x1400-0x141F)") {
    proto::PkEvent e;
    const auto bytes = proto::Encode(proto::PkCounters{5, 1, 1'200});
    REQUIRE(proto::DecodePkEvent(proto::PkCounters::kId, bytes, e));
    const auto* c = std::get_if<proto::PkCounters>(&e);
    REQUIRE(c != nullptr);
    CHECK(c->daily == 5);
    CHECK(c->total == 1);
    CHECK(c->points == 1'200);
    const auto state = proto::Encode(proto::EntityPvpState{77, 2});
    REQUIRE(proto::DecodePkEvent(proto::EntityPvpState::kId, state, e));
    CHECK(std::get<proto::EntityPvpState>(e).state == 2);
    CHECK(proto::IsPkMessage(0x1400));
    CHECK_FALSE(proto::IsPkMessage(0x1420));
}
