// Capture the Flag and Event So-Ok's rules (game/FlagWorld.h): the
// registration's checks, the matches formed (ours), the flag's play, the round's times, the prize swap's draw. The
// servers' side (registration from both servers, a match with players of both, the way home) is in CrossServerTests.

#include <doctest/doctest.h>

#include <array>
#include <ostream>
#include <random>
#include <string>

#include "sr/game/Arena.h"
#include "sr/game/EventProtocol.h"
#include "sr/game/FlagWorld.h"
#include "sr/game/Pk.h"

using namespace sr;
namespace proto = sr::game::proto;
using game::FlagCode;
using Kind = game::FlagMatch::Event::Kind;

TEST_CASE("Capture the Flag: So-Ok's checks: level 20 reached, alive, in town, no suit, no free PvP, no war, not dealing") {
    game::FlagEntrant e;
    e.maxLevel = 19;
    CHECK(game::RegistrationRefusal(e) == FlagCode::Level);
    e.maxLevel = 20;
    CHECK(game::RegistrationRefusal(e) == FlagCode::Done);
    for (bool game::FlagEntrant::*flag : {&game::FlagEntrant::jobSuit, &game::FlagEntrant::freePvp, &game::FlagEntrant::guildWar, &game::FlagEntrant::interacting,
                                          &game::FlagEntrant::questBlock}) {
        game::FlagEntrant x = e;
        x.*flag = true;
        CHECK(game::RegistrationRefusal(x) == FlagCode::Conditions);
    }
    game::FlagEntrant dead = e;
    dead.alive = false;
    CHECK(game::RegistrationRefusal(dead) == FlagCode::Conditions);
    game::FlagEntrant field = e;
    field.inTown = false;
    CHECK(game::RegistrationRefusal(field) == FlagCode::Conditions);
    // The level is the highest reached: one who fell to 19 after deaths still may.
    game::FlagEntrant fell = e;
    fell.maxLevel = 21;
    CHECK(game::RegistrationRefusal(fell) == FlagCode::Done);
}

TEST_CASE("Capture the Flag: the matches as the shard manager forms them: by level, 16 a match, the odd last out, the fewest by group, the teams in turn") {
    // The level groups (SM).
    CHECK(game::flag::GroupOfLevel(19) == 0);
    CHECK(game::flag::GroupOfLevel(20) == 1);
    CHECK(game::flag::GroupOfLevel(35) == 1);
    CHECK(game::flag::GroupOfLevel(36) == 2);
    CHECK(game::flag::GroupOfLevel(50) == 2);
    CHECK(game::flag::GroupOfLevel(51) == 3);
    CHECK(game::flag::GroupOfLevel(80) == 4);
    CHECK(game::flag::GroupOfLevel(90) == 5);
    CHECK(game::flag::GroupOfLevel(91) == 6);
    CHECK(game::flag::MinPlayersOfGroup(2) == 4);
    CHECK(game::flag::MinPlayersOfGroup(3) == 8);

    // Six of 25..29 (group 1, fewest 4): one match, the teams in turn by level from the coin's first: 1, 0, 0, 1, 1, 0.
    std::vector<game::FlagEntry> six;
    for (int i = 0; i < 6; ++i) six.push_back({std::uint32_t(100 + i), 29 - i, i});
    game::FlagPlan plan = game::PlanMatches(six, 0, 8, [] { return 1; });
    REQUIRE(plan.matches.size() == 1);
    CHECK(plan.cancelled.empty());
    CHECK(plan.matches[0].group == 1);
    const std::vector<std::pair<std::uint32_t, std::uint8_t>> turn{{105, 1}, {104, 0}, {103, 0}, {102, 1}, {101, 1}, {100, 0}};
    CHECK(plan.matches[0].players == turn);

    // One alone: the odd last out; one below 20: refused.
    plan = game::PlanMatches({{7, 40, 0}, {8, 15, 1}});
    CHECK(plan.matches.empty());
    CHECK(plan.cancelled == std::vector<std::uint32_t>{8, 7});

    // Levels are not kept apart: 25, 26, 85, 86 make one match of group 5, whose fewest is 8: cancelled whole; with
    // our tries' fewest of 2 it runs.
    const std::vector<game::FlagEntry> mixed{{1, 25, 0}, {2, 26, 1}, {3, 85, 2}, {4, 86, 3}};
    plan = game::PlanMatches(mixed);
    CHECK(plan.matches.empty());
    CHECK(plan.cancelled.size() == 4);
    plan = game::PlanMatches(mixed, 2);
    REQUIRE(plan.matches.size() == 1);
    CHECK(plan.matches[0].group == 5);
    CHECK(game::flag::MonsterOfGroup(plan.matches[0].group) == "MOB_EV_KANGAROO_85");

    // Twenty of 50: 16 (8 a side), then 4 (group 2's fewest).
    std::vector<game::FlagEntry> many;
    for (int i = 0; i < 20; ++i) many.push_back({std::uint32_t(i + 1), 50, i});
    plan = game::PlanMatches(many);
    REQUIRE(plan.matches.size() == 2);
    CHECK(plan.matches[0].players.size() == 16);
    CHECK(plan.matches[1].players.size() == 4);
    for (const auto& m : plan.matches) {
        int sides[2] = {};
        for (const auto& [c, team] : m.players) ++sides[team];
        CHECK(sides[0] == sides[1]);
    }
}

TEST_CASE("Capture the Flag: the flag: taken from the white pole in 5 s, placed on the own pole in 5 s, taken back from it") {
    game::FlagMatch m;
    std::vector<game::FlagMatch::Event> events;
    const std::uint32_t red = 11, red2 = 12, blue = 21;
    // The white pole holds the flag: a take for anyone.
    CHECK(m.OnPole(0));
    REQUIRE(m.SelectPole(0, red, 0, 1000, events) == FlagCode::Done);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Kind::Started);
    // A second actor on that pole while the first acts: FF 12.
    events.clear();
    CHECK(m.SelectPole(0, blue, 1, 2000, events) == FlagCode::TakeBusy);
    // An empty pole: nothing.
    CHECK_FALSE(m.SelectPole(2, blue, 1, 2000, events).has_value());
    m.Tick(5999, events);
    CHECK(m.Carrier() == 0);
    m.Tick(6000, events);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Kind::Carrier);
    CHECK(m.Carrier() == red);
    CHECK_FALSE(m.OnPole(0));
    // Only the carrier places, on his own pole (red's is 1); not the white, not the other team's.
    events.clear();
    CHECK_FALSE(m.SelectPole(1, red2, 0, 7000, events).has_value());
    CHECK_FALSE(m.SelectPole(2, red, 0, 7000, events).has_value());
    REQUIRE(m.SelectPole(1, red, 0, 7000, events) == FlagCode::Done);
    m.Tick(12000, events);
    CHECK(events.back().kind == Kind::Placed);
    CHECK(m.OnPole(1));
    CHECK(m.Carrier() == 0);
    REQUIRE(m.Winner().has_value());
    CHECK(*m.Winner() == 0);
    // Red cannot take its own pole's flag; blue can, in 5 s.
    events.clear();
    CHECK_FALSE(m.SelectPole(1, red2, 0, 13000, events).has_value());
    REQUIRE(m.SelectPole(1, blue, 1, 13000, events) == FlagCode::Done);
    m.Tick(18000, events);
    CHECK(m.Carrier() == blue);
    CHECK_FALSE(m.Winner().has_value());
}

TEST_CASE("Capture the Flag: the carrier's death or his 60 s drop the flag; anyone takes it at once; his action ends with him") {
    game::FlagMatch m;
    std::vector<game::FlagMatch::Event> events;
    REQUIRE(m.SelectPole(0, 1, 0, 0, events) == FlagCode::Done);
    m.Tick(5000, events);
    REQUIRE(m.Carrier() == 1);
    events.clear();
    m.Gone(1, events); // killed
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Kind::Dropped);
    CHECK(m.Dropped());
    // A pole holds nothing now: no take anywhere.
    CHECK_FALSE(m.SelectPole(0, 2, 1, 6000, events).has_value());
    events.clear();
    REQUIRE(m.PickUp(2, 1, 6000, events));
    CHECK(m.Carrier() == 2);
    CHECK_FALSE(m.Dropped());
    CHECK_FALSE(m.PickUp(3, 0, 6000, events));
    // 60 s without placing: dropped.
    events.clear();
    m.Tick(6000 + game::flag::kCarryMs - 1, events);
    CHECK(events.empty());
    m.Tick(6000 + game::flag::kCarryMs, events);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Kind::Dropped);
    // An action cut by its actor's death.
    game::FlagMatch n;
    events.clear();
    REQUIRE(n.SelectPole(0, 5, 0, 0, events) == FlagCode::Done);
    n.Gone(5, events);
    CHECK(n.ActionAt(0).actor == 0);
    n.Tick(10000, events);
    CHECK(n.Carrier() == 0);
    CHECK(n.OnPole(0));
    // At the end: the flag on the white pole is a draw.
    CHECK_FALSE(n.Winner().has_value());
}

TEST_CASE("Capture the Flag: the round's times (ours): registration 10 minutes before the play, the lines at 5 and 1, the play, the winners' minute") {
    game::FlagSchedule s;
    s.cycle = true;
    s.registerSeconds = 120;
    s.playSeconds = 300;
    s.winnersSeconds = 60;
    const auto c = game::NextFlagCycle(1'000'000, s);
    REQUIRE(c.has_value());
    CHECK(c->open <= 1'000'000);
    CHECK(c->stop > 1'000'000);
    CHECK(c->start == c->open + 120);
    CHECK(c->end == c->start + 300);
    CHECK(c->stop == c->end + 60);
    CHECK(c->standby1 == c->start - 60);
    CHECK(c->standby5 == c->open); // the registration is shorter than 5 minutes
    // Two servers asking at different moments of the same round agree on it.
    CHECK(game::NextFlagCycle(c->open + 7, s)->open == c->open);
    // Daily times: the next play's registration opens 10 minutes before it.
    game::FlagSchedule daily;
    daily.times = {20 * 60};
    const auto d = game::NextFlagCycle(1'000'000, daily);
    REQUIRE(d.has_value());
    CHECK(d->start - d->open == 600);
    CHECK(d->start - d->standby5 == 300);
    CHECK(d->start - d->standby1 == 60);
    CHECK(d->end - d->start == 1210);
    CHECK(d->stop > 1'000'000);
}

TEST_CASE("So-Ok's prize swap: the binary's tables, the draw by its float weights within 1 % over 100,000 draws") {
    const game::SwapQuest& ice = game::SwapIceTrophies();
    CHECK(ice.id == 10145);
    CHECK(ice.token == "ITEM_ETC_E080723_ICETROPHY");
    REQUIRE(ice.prizes.size() == 15);
    float sum = 0;
    for (const game::SwapPrize& p : ice.prizes) sum += p.weight;
    CHECK(sum == doctest::Approx(100.0f));
    REQUIRE(game::SwapTickets().prizes.size() == 16);
    sum = 0;
    for (const game::SwapPrize& p : game::SwapTickets().prizes) sum += p.weight;
    CHECK(sum == doctest::Approx(100.0f));
    CHECK(game::SwapQuestById(10077) == &game::SwapTickets());
    // The first row whose running sum reaches r; the edges.
    CHECK(game::DrawPrize(ice.prizes, 0.0f) == 0);
    CHECK(game::DrawPrize(ice.prizes, 1.0f) == 0);
    CHECK(game::DrawPrize(ice.prizes, 1.5f) == 1);
    CHECK(game::DrawPrize(ice.prizes, 100.0f) == 14);
    std::mt19937 random(7);
    std::array<int, 15> counts{};
    constexpr int kDraws = 100'000;
    for (int i = 0; i < kDraws; ++i) {
        const float r = float(std::uniform_int_distribution<int>(0, 32767)(random)) / 32767.0f * 100.0f;
        ++counts[game::DrawPrize(ice.prizes, r)];
    }
    for (std::size_t i = 0; i < counts.size(); ++i) CHECK(std::abs(100.0 * counts[i] / kDraws - ice.prizes[i].weight) < 1.0);
}

TEST_CASE("The events' messages (0x1360-0x13BF) and the event relation in the attack check (0x20: foes at once, no record, no murder)") {
    proto::FlagWorldNotice n{1, std::uint8_t(FlagCode::KeyTaken), 233, 0, "Ann"};
    proto::EventEvent e;
    REQUIRE(proto::DecodeEventEvent(proto::FlagWorldNotice::kId, proto::Encode(n), e));
    const auto& back = std::get<proto::FlagWorldNotice>(e);
    CHECK(back.value == 233);
    CHECK(back.name == "Ann");
    proto::EventDesk desk;
    desk.npc = 5;
    desk.flagState = 1;
    desk.quests.push_back({10145, "SN_QEV_CH_EVENT_KISAENG_081201"});
    REQUIRE(proto::DecodeEventEvent(proto::EventDesk::kId, proto::Encode(desk), e));
    CHECK(std::get<proto::EventDesk>(e).quests.at(0).quest == 10145);
    CHECK(proto::IsEventMessage(0x1360));
    CHECK(proto::IsEventMessage(0x13BF));
    CHECK_FALSE(proto::IsEventMessage(0x13C0));

    game::PvpPair p;
    p.striker.level = p.target.level = 10; // below the PK level, in a dungeon (no battlefield), one party
    p.striker.battlefield = p.target.battlefield = false;
    p.sameParty = true;
    CHECK_FALSE(game::AttackCheck(p).allowed);
    p.eventRelation = game::kEventFlagWorld;
    const game::AttackVerdict v = game::AttackCheck(p);
    CHECK(v.allowed);
    CHECK_FALSE(v.pk);
    CHECK_FALSE(game::HitMakesRecord(p));
    CHECK_FALSE(game::KillIsMurder(p));
}

TEST_CASE("The Battle Arena's flag maps: every pole its own, several flags at once, a placed flag stolen, carriers' drops") {
    using K = game::ArenaFlags::Event::Kind;
    game::ArenaFlags f;
    f.Reset({0, 0, 0, 1, 1, 1, 2, 2, 2}); // the 16 map: 3 white, 3 red (team 0's), 3 blue (team 1's)
    REQUIRE(f.Poles() == 9);
    for (int i = 0; i < 9; ++i) CHECK(f.OnPole(i) == (i < 3)); // the white ones show a flag
    constexpr std::uint32_t kRed = 11, kBlue = 22, kRed2 = 33;
    std::vector<game::ArenaFlags::Event> ev;
    std::int64_t t = 1'000;
    // Two takes at once on two white poles; the same pole busy for a third.
    CHECK(f.SelectPole(0, kRed, 0, t, ev) == game::FlagCode::Done);
    CHECK(f.SelectPole(0, kBlue, 1, t, ev) == game::FlagCode::TakeBusy);
    CHECK(f.SelectPole(1, kBlue, 1, t, ev) == game::FlagCode::Done);
    CHECK_FALSE(f.SelectPole(3, kRed2, 0, t, ev).has_value()); // his own team's empty pole, not carrying: nothing
    ev.clear();
    f.Tick(t + game::flag::kActionMs, ev);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].kind == K::Carrier);
    CHECK(ev[0].pole == 0);
    CHECK(f.Carrying(kRed));
    CHECK(f.Carrying(kBlue));
    CHECK_FALSE(f.OnPole(0));
    CHECK_FALSE(f.OnPole(1));
    // A carrier at a pole showing a flag: nothing (the server's "you have a flag"); at the other team's pole: nothing.
    CHECK_FALSE(f.SelectPole(2, kRed, 0, t, ev).has_value());
    CHECK_FALSE(f.SelectPole(6, kRed, 0, t, ev).has_value());
    // Placed on their own poles: 1000 each.
    t += game::flag::kActionMs;
    CHECK(f.SelectPole(3, kRed, 0, t, ev) == game::FlagCode::Done);
    CHECK(f.SelectPole(6, kBlue, 1, t, ev) == game::FlagCode::Done);
    ev.clear();
    f.Tick(t + game::flag::kActionMs, ev);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].kind == K::Placed);
    CHECK(f.FlagsOn(0) == 1);
    CHECK(f.FlagsOn(1) == 1);
    CHECK(f.Carriers().empty());
    // The red flag stolen from its pole by the blue player.
    t += game::flag::kActionMs;
    CHECK_FALSE(f.SelectPole(6, kBlue, 1, t, ev).has_value()); // his own team's flag: nothing
    CHECK(f.SelectPole(3, kBlue, 1, t, ev) == game::FlagCode::Done);
    ev.clear();
    f.Tick(t + game::flag::kActionMs, ev);
    CHECK(f.FlagsOn(0) == 0);
    CHECK(f.Carrying(kBlue));
    // His 60 s: it drops; another picks it up at once; his death drops it again.
    t += game::flag::kActionMs;
    ev.clear();
    f.Tick(t + game::flag::kCarryMs, ev);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].kind == K::Dropped);
    const std::uint32_t dropped = ev[0].flag;
    CHECK(f.Dropped(dropped));
    CHECK_FALSE(f.Carrying(kBlue));
    ev.clear();
    CHECK(f.PickUp(dropped, kRed2, 0, t, ev));
    CHECK(ev[0].kind == K::Carrier);
    CHECK(ev[0].flag == dropped);
    CHECK_FALSE(f.PickUp(dropped, kRed, 0, t, ev)); // gone
    ev.clear();
    f.Gone(kRed2, ev);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].kind == K::Dropped);
    CHECK(ev[0].flag != dropped);
    CHECK(game::PoleTeam(1) == 0);
    CHECK(game::PoleTeam(2) == 1);
    CHECK(game::PoleTeam(0) == 2);
}

TEST_CASE("The Battle Arena's rules: the manager's checks in order, coins by mode, the winner, the matches") {
    using game::ArenaCode;
    using M = game::arena::Mode;
    game::ArenaEntrant e;
    e.maxLevel = 40;
    e.open = true;
    CHECK(game::RegistrationRefusal(M::Random, e) == ArenaCode::Done);
    game::ArenaEntrant x = e;
    x.registered = true;
    CHECK(game::RegistrationRefusal(M::Random, x) == ArenaCode::Already);
    x = e;
    x.maxLevel = 19;
    CHECK(game::RegistrationRefusal(M::Random, x) == ArenaCode::Level);
    x = e;
    x.open = false;
    CHECK(game::RegistrationRefusal(M::Random, x) == ArenaCode::NotNow);
    CHECK(game::RegistrationRefusal(M::Party, e) == ArenaCode::NotLeader);
    CHECK(game::RegistrationRefusal(M::Guild, e) == ArenaCode::NotMaster);
    CHECK(game::RegistrationRefusal(M::Job, e) == ArenaCode::JobSuit); // the job mode needs a suit
    x = e;
    x.jobState = 2;
    CHECK(game::RegistrationRefusal(M::Job, x) == ArenaCode::Done);
    CHECK(game::RegistrationRefusal(M::Random, x) == ArenaCode::JobSuit); // the others forbid one
    x = e;
    x.conditions.inTown = false;
    CHECK(game::RegistrationRefusal(M::Random, x) == ArenaCode::Conditions);

    CHECK(game::arena::CoinsFor(M::Random, 1) == 5);
    CHECK(game::arena::CoinsFor(M::Party, 2) == 1);
    CHECK(game::arena::CoinsFor(M::Guild, 1) == 7);
    CHECK(game::arena::CoinsFor(M::Job, 3) == 2);
    CHECK(*game::arena::WinnerOf(7, 6) == 0);
    CHECK(*game::arena::WinnerOf(1000, 1001) == 1);
    CHECK_FALSE(game::arena::WinnerOf(5, 5).has_value());
    CHECK(game::arena::OutcomeOf(0, std::uint8_t(0)) == 1);
    CHECK(game::arena::OutcomeOf(1, std::uint8_t(0)) == 2);
    CHECK(game::arena::OutcomeOf(1, std::nullopt) == 3);

    // Random: four of one group, one match 2 v 2 on the smallest map.
    std::vector<game::ArenaUnit> four;
    for (std::uint32_t i = 1; i <= 4; ++i) four.push_back({{i}, 45, std::int64_t(i)});
    game::ArenaPlan plan = game::PlanArena(M::Random, four);
    REQUIRE(plan.matches.size() == 1);
    CHECK(plan.matches[0].size == 8);
    CHECK(plan.matches[0].players.size() == 4);
    // Party: each party a team, paired; one left over is cancelled.
    plan = game::PlanArena(M::Party, {{{1, 2, 3, 4}, 50, 1}, {{5, 6, 7}, 50, 2}, {{8, 9}, 50, 3}});
    REQUIRE(plan.matches.size() == 1);
    for (const auto& [c, team] : plan.matches[0].players) CHECK(team == (c <= 4 ? 0 : 1));
    CHECK(plan.cancelled == std::vector<std::uint32_t>{8, 9});
    // Party and guild: paired by their registrants' levels, the lowest first (SM): 30 with 31, 69 with 70.
    std::vector<game::ArenaUnit> parties;
    const int leaders[] = {70, 30, 31, 69};
    for (std::uint32_t i = 0; i < 4; ++i) {
        game::ArenaUnit u{{10 * (i + 1), 10 * (i + 1) + 1}, leaders[i], std::int64_t(i)};
        u.leaderLevel = leaders[i];
        u.levelSum = leaders[i] * 2;
        parties.push_back(u);
    }
    plan = game::PlanArena(M::Party, parties);
    REQUIRE(plan.matches.size() == 2);
    CHECK(plan.matches[0].players.front().first == 20); // the 30's party
    CHECK(plan.matches[0].players.back().first == 31);  // with the 31's
    CHECK(plan.matches[0].group == game::flag::GroupOfLevel(30));
    CHECK(plan.matches[1].group == game::flag::GroupOfLevel(69));
    // Guild: the 16 map when both together pass 16.
    game::ArenaUnit big{{}, 60, 1}, small{{}, 60, 2};
    for (std::uint32_t c = 1; c <= 12; ++c) big.characters.push_back(c);
    for (std::uint32_t c = 21; c <= 26; ++c) small.characters.push_back(c);
    plan = game::PlanArena(M::Guild, {big, small});
    REQUIRE(plan.matches.size() == 1);
    CHECK(plan.matches[0].size == 16);
    // Job (SM): one thief against a trader and a hunter makes no match (2 a side at least); two a side make
    // one on the 8 map, the thieves the other team; at 2 a side the next trader or hunter above 50 stops it.
    game::ArenaUnit trader{{1}, 40, 1, 1}, hunter{{2}, 45, 2, 3}, thief{{3}, 42, 3, 2}, thief2{{4}, 60, 4, 2};
    plan = game::PlanArena(M::Job, {trader, hunter, thief});
    CHECK(plan.matches.empty());
    CHECK(plan.cancelled.size() == 3);
    plan = game::PlanArena(M::Job, {trader, hunter, thief, thief2});
    REQUIRE(plan.matches.size() == 1);
    CHECK(plan.matches[0].size == 8);
    CHECK(plan.matches[0].group == game::flag::GroupOfLevel(42));
    for (const auto& [c, team] : plan.matches[0].players) CHECK(team == (c >= 3 ? 1 : 0));
    trader.level = 55;
    hunter.level = 56;
    plan = game::PlanArena(M::Job, {trader, hunter, thief, thief2});
    CHECK(plan.matches.empty());
    CHECK(plan.cancelled.size() == 4);
}