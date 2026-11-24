// Parties: the shared rules (game/Party.h), the client's party view, and the
// game server's parties and matching (server/Parties.h) over a made-up host.

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <thread>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/assets/Terrain.h"
#include "sr/core/Jobs.h"
#include "sr/game/Party.h"
#include "sr/game/PlayerState.h"
#include "sr/server/GameServer.h"
#include "sr/server/Monsters.h"
#include "sr/server/Parties.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
using S = proto::PartyStatus;

namespace {

// The game server as the parties see it: players by entity, and what each was told.
struct Host : server::PartyHost {
    std::map<std::uint32_t, Player> players;
    std::map<std::uint32_t, std::vector<server::PartyMessage>> told;
    std::optional<Player> PlayerAt(std::uint32_t entity) const override {
        const auto it = players.find(entity);
        return it != players.end() ? std::optional<Player>(it->second) : std::nullopt;
    }
    void Tell(std::uint32_t entity, const server::PartyMessage& message) override { told[entity].push_back(message); }
    // A player at (x, z) of Constantinople's region, of a level, Chinese or European.
    Player& Add(std::uint32_t entity, const char* name, int level, float x = 500, float z = 500, std::uint8_t race = 0) {
        Player& p = players[entity];
        p.entity = entity;
        p.character = entity + 1000;
        p.name = name;
        p.level = level;
        p.race = race;
        p.hp = p.maxHp = 1000;
        p.mp = p.maxMp = 500;
        p.position = proto::WorldPosition{0x6950, x, 0, z};
        return p;
    }
    // The messages of a kind told to a player (taken: the next call finds only newer ones).
    template <typename T>
    std::vector<T> Take(std::uint32_t entity) {
        std::vector<T> out;
        std::vector<server::PartyMessage>& all = told[entity];
        for (const server::PartyMessage& m : all) {
            if (const T* t = std::get_if<T>(&m)) out.push_back(*t);
        }
        std::erase_if(all, [](const server::PartyMessage& m) { return std::holds_alternative<T>(m); });
        return out;
    }
    template <typename T>
    std::optional<T> Last(std::uint32_t entity) {
        const std::vector<T> all = Take<T>(entity);
        return all.empty() ? std::nullopt : std::optional<T>(all.back());
    }
};

template <typename T>
server::Parties::Handled Send(server::Parties& parties, std::uint32_t entity, const T& m, server::Parties::Clock::time_point now = {}) {
    const std::vector<std::byte> bytes = proto::Encode(m);
    return parties.Handle(entity, T::kId, bytes, now);
}

// Two players in a party: Alpha (1, the master) asked Beta (2) with `settings`.
void Pair(Host& host, server::Parties& parties, std::uint8_t settings) {
    REQUIRE(Send(parties, 1, proto::PartyInvite{2, settings}) == server::Parties::Handled::Yes);
    REQUIRE(Send(parties, 2, proto::PartyInviteAnswer{1}) == server::Parties::Handled::Yes);
    REQUIRE(parties.PartyOf(1) != 0);
    host.told.clear();
}

} // namespace

TEST_CASE("parties: the shared rules: sizes, experience shares, gold, tenths") {
    CHECK(game::PartyCapacity(game::kPartyExpShare) == 8);
    CHECK(game::PartyCapacity(game::kPartyItemShare) == 4);
    CHECK(game::PartyCapacity(0) == 4);

    // Two Chinese of levels 20 and 30: the bonus 1.1, the shares by level.
    game::PartyExperience e = game::PartyExperienceShares({{20, false}, {30, false}}, 25, false);
    CHECK(e.bonus == doctest::Approx(1.1));
    CHECK(e.levelSum == 50);
    REQUIRE(e.factors.size() == 2);
    CHECK(e.factors[0] == doctest::Approx(0.4 * 1.1));
    CHECK(e.factors[1] == doctest::Approx(0.6 * 1.1));
    // A European with two Chinese: 1 + 0 (the first European) + 0.2 (two Chinese).
    e = game::PartyExperienceShares({{40, true}, {10, false}, {10, false}}, 40, false);
    CHECK(e.bonus == doctest::Approx(1.2));
    CHECK(e.factors[0] == doctest::Approx(40.0 / 60.0 * 1.2));
    CHECK(e.factors[1] == doctest::Approx(10.0 / 60.0 * 1.2));
    // Two Europeans: 1 + 0.2.
    CHECK(game::PartyExperienceShares({{10, true}, {10, true}}, 10, false).bonus == doctest::Approx(1.2));
    // One taking over 0.7 of three, the highest 21 or more: half the bonus for it.
    e = game::PartyExperienceShares({{80, false}, {10, false}, {10, false}}, 80, false);
    CHECK(e.bonus == doctest::Approx(1.2));
    CHECK(e.factors[0] == doctest::Approx(0.8 * 1.1));
    CHECK(e.factors[1] == doctest::Approx(0.1 * 1.2));
    // Below level 21 the bonus stays whole.
    e = game::PartyExperienceShares({{16, false}, {2, false}, {2, false}}, 16, false);
    CHECK(e.factors[0] == doctest::Approx(0.8 * 1.2));
    // A party monster below the average level (33): half.
    e = game::PartyExperienceShares({{80, false}, {10, false}, {10, false}}, 30, true);
    CHECK(e.averageLevel == 33);
    CHECK(e.factors[1] == doctest::Approx(0.1 * 1.2 * 0.5));
    CHECK(game::PartyExperienceShares({{80, false}, {10, false}, {10, false}}, 33, true).factors[1] == doctest::Approx(0.1 * 1.2));
    // Outside an exp-shared party: 3 % a member after the first, half for a party monster below the player.
    CHECK(game::SoloExperienceFactor(0, 20, 20, false) == doctest::Approx(1.0));
    CHECK(game::SoloExperienceFactor(4, 20, 20, false) == doctest::Approx(1.09));
    CHECK(game::SoloExperienceFactor(4, 20, 10, true) == doctest::Approx(1.09 * 0.5));
    CHECK(game::SoloExperienceFactor(0, 20, 25, true) == doctest::Approx(1.0));

    // Gold among three: equal shares, the picker the rest.
    CHECK(game::SplitPartyGold(100, 3) == std::vector<std::uint64_t>{34, 33, 33});
    CHECK(game::SplitPartyGold(7, 1) == std::vector<std::uint64_t>{7});
    // Tenths: a living member at least one.
    CHECK(game::PartyTenths(1000, 1000, true) == 11); // a living member's HP: 1 + trunc(10 x HP / most)
    CHECK(game::PartyTenths(910, 1000, true) == 10);  // 91 %: the bar shows full
    CHECK(game::PartyTenths(549, 1000, true) == 6);
    CHECK(game::PartyTenths(1, 1000, true) == 1);
    CHECK(game::PartyTenths(1, 1000, false) == 0);
    CHECK(game::PartyTenths(0, 1000, true) == 0);
}

TEST_CASE("parties: an invitation answered: a party of two, its master, the request's checks") {
    Host host;
    server::Parties parties(host);
    host.Add(1, "Alpha", 20);
    host.Add(2, "Beta", 10, 520, 500);
    host.Add(3, "Gamma", 4, 540, 500);
    host.Add(4, "Delta", 30, 2000, 500); // far away
    // Too low a level to make a party; nobody of that entity; too far.
    Send(parties, 3, proto::PartyInvite{1, 0});
    CHECK(host.Last<proto::PartyResult>(3)->status == S::LowLevel);
    Send(parties, 1, proto::PartyInvite{99, 0});
    CHECK(host.Last<proto::PartyResult>(1)->status == S::InvalidUser);
    Send(parties, 1, proto::PartyInvite{4, 0});
    CHECK(host.Last<proto::PartyResult>(1)->status == S::TooFar);

    // Asked: Beta is told who asks and the settings; it says no, Alpha hears it.
    Send(parties, 1, proto::PartyInvite{2, game::kPartyExpShare | game::kPartyItemShare});
    const auto invited = host.Last<proto::PartyInvited>(2);
    REQUIRE(invited);
    CHECK(invited->from == 1);
    CHECK(invited->name == "Alpha");
    CHECK(invited->settings == (game::kPartyExpShare | game::kPartyItemShare));
    CHECK_FALSE(invited->join);
    Send(parties, 2, proto::PartyInviteAnswer{0});
    const auto refused = host.Last<proto::PartyResult>(1);
    REQUIRE(refused);
    CHECK(refused->status == S::Refused);
    CHECK(refused->name == "Beta");
    CHECK(parties.PartyOf(1) == 0);

    // Asked again and accepted: one party, Alpha its master, both told the whole party.
    Send(parties, 1, proto::PartyInvite{2, game::kPartyExpShare | game::kPartyItemShare});
    Send(parties, 2, proto::PartyInviteAnswer{1});
    const auto infoA = host.Last<proto::PartyInfo>(1), infoB = host.Last<proto::PartyInfo>(2);
    REQUIRE(infoA);
    REQUIRE(infoB);
    CHECK(infoA->party == infoB->party);
    CHECK(infoA->leader == 1001);
    REQUIRE(infoA->members.size() == 2);
    CHECK(infoA->members[0].name == "Alpha");
    CHECK(infoA->members[1].name == "Beta");
    CHECK(infoA->members[1].entity == 2);
    CHECK(parties.MemberEntities(2) == std::vector<std::uint32_t>{1, 2});
    CHECK(parties.SettingsOf(2) == (game::kPartyExpShare | game::kPartyItemShare));

    // Beta (not the master, anyone may not invite) cannot ask Gamma; Alpha can, and Gamma joins.
    Send(parties, 2, proto::PartyInvite{3, 0});
    CHECK(host.Last<proto::PartyResult>(2)->status == S::NotLeader);
    Send(parties, 1, proto::PartyInvite{3, 0});
    const auto joinAsk = host.Last<proto::PartyInvited>(3);
    REQUIRE(joinAsk);
    CHECK(joinAsk->join);
    Send(parties, 3, proto::PartyInviteAnswer{1});
    CHECK(host.Last<proto::PartyMemberJoined>(2)->member.name == "Gamma");
    CHECK(host.Last<proto::PartyInfo>(3)->members.size() == 3);
    // A member of this party asked again; a player of another party.
    Send(parties, 1, proto::PartyInvite{2, 0});
    CHECK(host.Last<proto::PartyResult>(1)->status == S::ExistingMember);
}

TEST_CASE("parties: a request runs out after 30 s; a full party refuses more") {
    Host host;
    server::Parties parties(host);
    host.Add(1, "Alpha", 20);
    for (std::uint32_t e = 2; e <= 6; ++e) host.Add(e, "Member", 10, 500.0f + float(e) * 5.0f, 500);
    const server::Parties::Clock::time_point t0{};
    Send(parties, 1, proto::PartyInvite{2, 0}, t0);
    parties.Tick(t0 + std::chrono::seconds(29));
    CHECK(host.Take<proto::PartyResult>(1).empty());
    parties.Tick(t0 + std::chrono::seconds(31));
    CHECK(host.Last<proto::PartyResult>(1)->status == S::Timeout);
    CHECK(host.Last<proto::PartyResult>(2)->status == S::Timeout); // its box closes
    Send(parties, 2, proto::PartyInviteAnswer{1}, t0 + std::chrono::seconds(31));
    CHECK(parties.PartyOf(1) == 0);

    // A party of free exp holds 4.
    Pair(host, parties, 0);
    for (std::uint32_t e = 3; e <= 4; ++e) {
        Send(parties, 1, proto::PartyInvite{e, 0});
        Send(parties, e, proto::PartyInviteAnswer{1});
    }
    CHECK(parties.MemberEntities(1).size() == 4);
    Send(parties, 1, proto::PartyInvite{5, 0});
    CHECK(host.Last<proto::PartyResult>(1)->status == S::FullFree);
}

TEST_CASE("parties: leave, kick, dismiss, a master leaving the world hands over, settings changed") {
    Host host;
    server::Parties parties(host);
    host.Add(1, "Alpha", 20);
    host.Add(2, "Beta", 10, 510, 500);
    host.Add(3, "Gamma", 10, 520, 500);
    host.Add(4, "Delta", 10, 530, 500);
    Pair(host, parties, game::kPartyExpShare);
    for (std::uint32_t e = 3; e <= 4; ++e) {
        Send(parties, 1, proto::PartyInvite{e, 0});
        Send(parties, e, proto::PartyInviteAnswer{1});
    }
    host.told.clear();
    // Only the master sends a member away; everyone, the member too, hears why.
    Send(parties, 2, proto::PartyKick{1004});
    CHECK(host.Last<proto::PartyResult>(2)->status == S::NotLeader);
    Send(parties, 1, proto::PartyKick{1004});
    for (std::uint32_t e : {1u, 2u, 3u, 4u}) {
        const auto left = host.Last<proto::PartyMemberLeft>(e);
        REQUIRE(left);
        CHECK(left->member == 1004);
        CHECK(left->reason == proto::PartyLeaveReason::Kicked);
    }
    CHECK(parties.PartyOf(4) == 0);
    // Gamma leaves.
    Send(parties, 3, proto::PartyLeave{});
    CHECK(host.Last<proto::PartyMemberLeft>(1)->reason == proto::PartyLeaveReason::Left);
    CHECK(parties.MemberEntities(1) == std::vector<std::uint32_t>{1, 2});
    // The master changes the settings: everyone told; a member may not.
    Send(parties, 2, proto::PartySetSettings{game::kPartyItemShare});
    CHECK(host.Last<proto::PartyResult>(2)->status == S::NotLeader);
    Send(parties, 1, proto::PartySetSettings{game::kPartyItemShare | game::kPartyAnyoneInvites});
    CHECK(host.Last<proto::PartySettingsChanged>(2)->settings == (game::kPartyItemShare | game::kPartyAnyoneInvites));
    CHECK(parties.SharesItems(1001, 1002));
    // Anyone may invite now: Beta asks Gamma back.
    Send(parties, 2, proto::PartyInvite{3, 0});
    Send(parties, 3, proto::PartyInviteAnswer{1});
    CHECK(parties.MemberEntities(1).size() == 3);
    host.told.clear();
    // The master leaves the world: Beta leads, everyone told.
    parties.LeaveWorld(1);
    CHECK(host.Last<proto::PartyMemberLeft>(2)->reason == proto::PartyLeaveReason::Logout);
    CHECK(host.Last<proto::PartyLeaderChanged>(3)->member == 1002);
    host.players.erase(1);
    // Beta, the master now, leaves: two were left, so the party goes (SM).
    Send(parties, 2, proto::PartyLeave{});
    CHECK(host.Last<proto::PartyDismissed>(3)->reason == 2);
    CHECK(parties.PartyOf(3) == 0);
    CHECK(parties.Count() == 0);
    // A party of two losing a member is dismissed too.
    host.Add(1, "Alpha", 20);
    Pair(host, parties, 0);
    Send(parties, 2, proto::PartyLeave{});
    CHECK(host.Last<proto::PartyDismissed>(1)->reason == 2);
    CHECK(parties.Count() == 0);
}

TEST_CASE("parties: the members' news: HP and MP by the tenth, level, place") {
    Host host;
    server::Parties parties(host);
    host.Add(1, "Alpha", 20);
    host.Add(2, "Beta", 10, 510, 500);
    Pair(host, parties, 0);
    const server::Parties::Clock::time_point t0{};
    parties.Tick(t0 + std::chrono::seconds(1)); // the first news: the places
    host.told.clear();
    host.players[2].hp = 960; // 1 + 9 tenths... and 11 before: a tenth less
    parties.Tick(t0 + std::chrono::seconds(2));
    auto u = host.Last<proto::PartyMemberUpdate>(1);
    REQUIRE(u);
    CHECK(u->member == 1002);
    CHECK((u->mask & proto::kPartyVitals) != 0);
    CHECK(u->hpTenths == 10);
    host.players[2].hp = 950; // the same tenth: nothing
    parties.Tick(t0 + std::chrono::seconds(3));
    CHECK_FALSE(host.Last<proto::PartyMemberUpdate>(1));
    host.players[2].level = 11;
    host.players[2].position->x += 100;
    parties.Tick(t0 + std::chrono::seconds(6));
    u = host.Last<proto::PartyMemberUpdate>(1);
    REQUIRE(u);
    CHECK((u->mask & proto::kPartyLevel) != 0);
    CHECK(u->level == 11);
    CHECK((u->mask & proto::kPartyPosition) != 0);
    CHECK(u->position.x == doctest::Approx(610));
    // Its two highest masteries (mask 8): told when they change; the party's record carries them.
    host.players[2].mastery1 = 257;
    host.players[2].mastery2 = 275;
    parties.Tick(t0 + std::chrono::seconds(7));
    u = host.Last<proto::PartyMemberUpdate>(1);
    REQUIRE(u);
    CHECK(u->mask == proto::kPartyMasteries);
    CHECK(u->mastery1 == 257u);
    CHECK(u->mastery2 == 275u);
    parties.Tick(t0 + std::chrono::seconds(8));
    CHECK_FALSE(host.Last<proto::PartyMemberUpdate>(1));
    game::PartyView view;
    view.Apply(game::TitleEvent{proto::PartyInfo{1, 0, 1001, {proto::PartyMemberRecord{1002, 2, "Beta"}}}}, 0.0);
    view.Apply(game::TitleEvent{*u}, 0.0);
    REQUIRE(view.Member(1002) != nullptr);
    CHECK(view.Member(1002)->mastery1 == 257u);
    CHECK(view.Member(1002)->mastery2 == 275u);
}

TEST_CASE("parties: a member's two highest masteries: trained ones, highest first, the lower id on a tie") {
    CHECK(game::DominantMasteries({}) == std::pair<std::uint32_t, std::uint32_t>{0, 0});
    CHECK(game::DominantMasteries({{257, 0}, {258, 0}}) == std::pair<std::uint32_t, std::uint32_t>{0, 0});
    CHECK(game::DominantMasteries({{257, 12}}) == std::pair<std::uint32_t, std::uint32_t>{257, 0});
    CHECK(game::DominantMasteries({{257, 12}, {258, 40}, {275, 30}, {276, 0}}) == std::pair<std::uint32_t, std::uint32_t>{258, 275});
    CHECK(game::DominantMasteries({{513, 20}, {518, 20}, {515, 5}}) == std::pair<std::uint32_t, std::uint32_t>{513, 518});
}

TEST_CASE("parties: a kill's experience and its owner, shared by an exp-shared party near it") {
    Host host;
    server::Parties parties(host);
    host.Add(1, "Alpha", 30);
    host.Add(2, "Beta", 20, 600, 500);
    host.Add(3, "Gamma", 10, 2000, 500); // in the party, too far
    host.Add(4, "Delta", 40, 520, 500);  // alone
    Pair(host, parties, game::kPartyExpShare);
    Send(parties, 1, proto::PartyInvite{3, 0});
    host.players[3].position = proto::WorldPosition{0x6950, 540, 0, 500};
    Send(parties, 1, proto::PartyInvite{3, 0});
    Send(parties, 3, proto::PartyInviteAnswer{1});
    host.players[3].position = proto::WorldPosition{0x6950, 2000, 0, 500};
    REQUIRE(parties.MemberEntities(1).size() == 3);
    // Alpha 400 and Beta 200 of the party (600 together), Delta 500 alone: the party did the most, Alpha its best.
    const server::Parties::Kill kill = parties.ShareKill({{1, 400}, {4, 500}, {2, 200}}, 25, false, proto::WorldPosition{0x6950, 500, 0, 500});
    CHECK(kill.owner == 1);
    REQUIRE(kill.shares.size() == 3);
    std::map<std::uint32_t, double> share;
    for (const auto& s : kill.shares) share[s.entity] = s.share;
    // The party's 6/11 shared by Alpha and Beta (Gamma too far): bonus 1.1, 30/50 and 20/50.
    CHECK(share[1] == doctest::Approx(6.0 / 11.0 * 0.6 * 1.1));
    CHECK(share[2] == doctest::Approx(6.0 / 11.0 * 0.4 * 1.1));
    CHECK_FALSE(share.contains(3));
    CHECK(share[4] == doctest::Approx(5.0 / 11.0));
    // Delta's 500 the most: the drops are its.
    CHECK(parties.ShareKill({{1, 200}, {4, 500}, {2, 200}}, 25, false, std::nullopt).owner == 4);
    // A dead member shares nothing; a party whose exp is not shared counts alone (3 % a member).
    host.players[2].alive = false;
    CHECK(parties.ShareKill({{1, 100}}, 25, false, std::nullopt).shares.size() == 1);
    host.players[2].alive = true;
    Send(parties, 1, proto::PartySetSettings{game::kPartyItemShare});
    const server::Parties::Kill free = parties.ShareKill({{1, 100}, {2, 100}}, 25, false, std::nullopt);
    REQUIRE(free.shares.size() == 2);
    CHECK(free.shares[0].share == doctest::Approx(0.5 * 1.06));
}

TEST_CASE("parties: items go round the members near, gold is split, items kept for a member are the party's") {
    Host host;
    server::Parties parties(host);
    host.Add(1, "Alpha", 20);
    host.Add(2, "Beta", 20, 520, 500);
    host.Add(3, "Gamma", 20, 540, 500);
    Pair(host, parties, game::kPartyItemShare);
    Send(parties, 1, proto::PartyInvite{3, 0});
    Send(parties, 3, proto::PartyInviteAnswer{1});
    // The turn: Alpha, Beta, Gamma, Alpha... whoever picks up.
    CHECK(parties.LootRecipient(2) == 1);
    CHECK(parties.LootRecipient(2) == 2);
    CHECK(parties.LootRecipient(1) == 3);
    CHECK(parties.LootRecipient(3) == 1);
    // One far from the picker is passed over (its turn taken).
    host.players[2].position->x = 5000;
    CHECK(parties.LootRecipient(1) == 3);
    CHECK(parties.LootRecipient(1) == 1);
    // Gold among those near the picker, the picker the rest.
    const auto gold = parties.SplitGold(3, 101);
    REQUIRE(gold.size() == 2);
    CHECK(gold[0] == std::pair<std::uint32_t, std::uint64_t>{3, 51});
    CHECK(gold[1] == std::pair<std::uint32_t, std::uint64_t>{1, 50});
    CHECK(parties.SharesItems(1001, 1003));
    // Without the share: the picker's.
    Send(parties, 1, proto::PartySetSettings{0});
    CHECK(parties.LootRecipient(3) == 3);
    CHECK(parties.SplitGold(3, 101).size() == 1);
    CHECK_FALSE(parties.SharesItems(1001, 1003));
    // The loot notice to the party.
    host.told.clear();
    parties.TellLoot(3, 5000, 2, 0);
    const auto loot = host.Last<proto::PartyLoot>(1);
    REQUIRE(loot);
    CHECK(loot->member == 1003);
    CHECK(loot->refId == 5000);
}

TEST_CASE("party matching: a listing made, listed, asked to join, accepted; gone when full; our settings chosen with it") {
    Host host;
    server::Parties parties(host);
    host.Add(1, "Alpha", 30);
    host.Add(2, "Beta", 25, 2000, 2000); // far: matching needs no nearness
    host.Add(3, "Gamma", 50);
    host.Add(4, "Delta", 4);
    host.Add(5, "Eps", 28);
    host.Add(6, "Zeta", 29);
    // Too low a level to list; a bad range.
    proto::PartyMatchForm form{game::kPartyExpShare | game::kPartyItemShare, std::uint8_t(game::PartyPurpose::Hunting), 20, 40, "For open hunting"};
    Send(parties, 4, proto::PartyMatchRegister{form});
    CHECK(host.Last<proto::PartyMatchAnswer>(4)->status == S::LowLevel);
    proto::PartyMatchForm bad = form;
    bad.levelMin = 50;
    Send(parties, 1, proto::PartyMatchRegister{bad});
    CHECK(host.Last<proto::PartyMatchAnswer>(1)->status == S::OutOfLevel);
    // Listed without a party (ours: the exp and items shared chosen with it).
    form.settings = game::kPartyItemShare; // free exp: 4 members
    Send(parties, 1, proto::PartyMatchRegister{form});
    const auto made = host.Last<proto::PartyMatchAnswer>(1);
    REQUIRE(made);
    CHECK(made->action == proto::PartyMatchAction::Register);
    CHECK(made->status == S::Ok);
    const std::uint32_t entry = made->entry;
    // Listed: Beta sees it.
    Send(parties, 2, proto::PartyMatchList{0, 0xFF, 0, 0, ""});
    auto listing = host.Last<proto::PartyMatchListing>(2);
    REQUIRE(listing);
    REQUIRE(listing->entries.size() == 1);
    CHECK(listing->entries[0].entry == entry);
    CHECK(listing->entries[0].name == "Alpha");
    CHECK(listing->entries[0].form.title == "For open hunting");
    CHECK(listing->entries[0].members == 1);
    CHECK(listing->entries[0].capacity == 4);
    CHECK(listing->own == 0);
    // Filters: another purpose, a level outside, a name.
    Send(parties, 2, proto::PartyMatchList{0, std::uint8_t(game::PartyPurpose::Thief), 0, 0, ""});
    CHECK(host.Last<proto::PartyMatchListing>(2)->entries.empty());
    Send(parties, 2, proto::PartyMatchList{0, 0xFF, 45, 50, ""});
    CHECK(host.Last<proto::PartyMatchListing>(2)->entries.empty());
    Send(parties, 2, proto::PartyMatchList{0, 0xFF, 0, 0, "Alp"});
    CHECK(host.Last<proto::PartyMatchListing>(2)->entries.size() == 1);
    // Gamma is above its range; Beta asks: Alpha is asked, Beta waits.
    Send(parties, 3, proto::PartyMatchJoin{entry});
    CHECK(host.Last<proto::PartyMatchAnswer>(3)->status == S::MatchLevel);
    Send(parties, 2, proto::PartyMatchJoin{entry});
    CHECK(host.Last<proto::PartyMatchAnswer>(2)->status == S::Ok);
    const auto request = host.Last<proto::PartyMatchJoinRequest>(1);
    REQUIRE(request);
    CHECK(request->name == "Beta");
    CHECK(request->level == 25);
    // Asked twice: refused while the first waits.
    Send(parties, 2, proto::PartyMatchJoin{entry});
    CHECK(host.Last<proto::PartyMatchAnswer>(2)->status == S::MatchJoining);
    // Accepted: a party of the listing's settings, Alpha its master.
    Send(parties, 1, proto::PartyMatchJoinAnswer{1});
    CHECK(host.Last<proto::PartyMatchAnswer>(2)->status == S::Ok);
    const auto info = host.Last<proto::PartyInfo>(2);
    REQUIRE(info);
    CHECK(info->settings == game::kPartyItemShare);
    CHECK(info->leader == 1001);
    CHECK(parties.PartyOf(2) == parties.PartyOf(1));
    // The listing shows 2 of 4; Eps and Zeta fill it: then it goes.
    Send(parties, 3, proto::PartyMatchList{0, 0xFF, 0, 0, ""});
    CHECK(host.Last<proto::PartyMatchListing>(3)->entries[0].members == 2);
    for (std::uint32_t e : {5u, 6u}) {
        Send(parties, e, proto::PartyMatchJoin{entry});
        Send(parties, 1, proto::PartyMatchJoinAnswer{1});
    }
    CHECK(parties.MemberEntities(1).size() == 4);
    const auto removed = host.Last<proto::PartyMatchAnswer>(1);
    REQUIRE(removed);
    CHECK(removed->action == proto::PartyMatchAction::Removed);
    CHECK(removed->status == S::AlreadyFull);
    CHECK(parties.Listings() == 0);
    // A refused request; a listing gone with its dismissed party.
    host.Add(7, "Eta", 30);
    host.Add(8, "Theta", 30);
    proto::PartyMatchForm second{game::kPartyExpShare, std::uint8_t(game::PartyPurpose::Quest), 1, 90, "Quests"};
    Send(parties, 7, proto::PartyMatchRegister{second});
    const std::uint32_t entry2 = host.Last<proto::PartyMatchAnswer>(7)->entry;
    Send(parties, 8, proto::PartyMatchJoin{entry2});
    Send(parties, 7, proto::PartyMatchJoinAnswer{0});
    CHECK(host.Last<proto::PartyMatchAnswer>(8)->status == S::JoinRefused);
    Send(parties, 8, proto::PartyMatchJoin{entry2});
    Send(parties, 7, proto::PartyMatchJoinAnswer{1});
    CHECK(parties.SettingsOf(8) == game::kPartyExpShare);
    // The listing changed (ours: its settings are the party's), then the party dismissed: gone.
    second.settings = game::kPartyExpShare | game::kPartyItemShare;
    Send(parties, 7, proto::PartyMatchRegister{second});
    CHECK(host.Last<proto::PartyMatchAnswer>(7)->action == proto::PartyMatchAction::Modify);
    CHECK(host.Last<proto::PartySettingsChanged>(8)->settings == (game::kPartyExpShare | game::kPartyItemShare));
    Send(parties, 7, proto::PartyLeave{});
    CHECK(host.Last<proto::PartyMatchAnswer>(7)->action == proto::PartyMatchAction::Removed);
    CHECK(parties.Listings() == 0);
    // Taken off by its maker.
    Send(parties, 3, proto::PartyMatchRegister{form});
    Send(parties, 3, proto::PartyMatchDelete{});
    CHECK(host.Last<proto::PartyMatchAnswer>(3)->action == proto::PartyMatchAction::Delete);
    CHECK(parties.Listings() == 0);
}

TEST_CASE("party matching: a master leaving a party of three: the next member leads and keeps the listing") {
    Host host;
    server::Parties parties(host);
    host.Add(1, "Alpha", 30);
    host.Add(2, "Beta", 30);
    host.Add(3, "Gamma", 30);
    host.Add(4, "Delta", 30);
    proto::PartyMatchForm form{game::kPartyExpShare, std::uint8_t(game::PartyPurpose::Hunting), 1, 90, "Hunting"};
    Send(parties, 1, proto::PartyMatchRegister{form});
    const std::uint32_t entry = host.Last<proto::PartyMatchAnswer>(1)->entry;
    for (std::uint32_t e : {2u, 3u}) {
        Send(parties, e, proto::PartyMatchJoin{entry});
        Send(parties, 1, proto::PartyMatchJoinAnswer{1});
    }
    REQUIRE(parties.MemberEntities(1).size() == 3);
    // Alpha leaves (its window's own button): no dismissal; Beta, the next to have joined, leads.
    Send(parties, 1, proto::PartyLeave{});
    CHECK(parties.PartyOf(1) == 0);
    CHECK(parties.MemberEntities(2) == std::vector<std::uint32_t>{2, 3});
    CHECK(host.Last<proto::PartyLeaderChanged>(3)->member == 1002);
    CHECK_FALSE(host.Last<proto::PartyDismissed>(2));
    // The listing stays, Beta's now: listed under its name, Beta its owner, Delta may still ask to join.
    CHECK(parties.Listings() == 1);
    Send(parties, 4, proto::PartyMatchList{0, 0xFF, 0, 0, ""});
    const auto listing = host.Last<proto::PartyMatchListing>(4);
    REQUIRE(listing);
    REQUIRE(listing->entries.size() == 1);
    CHECK(listing->entries[0].name == "Beta");
    CHECK(listing->entries[0].members == 2);
    Send(parties, 2, proto::PartyMatchList{0, 0xFF, 0, 0, ""});
    CHECK(host.Last<proto::PartyMatchListing>(2)->own == entry);
    Send(parties, 4, proto::PartyMatchJoin{entry});
    REQUIRE(host.Last<proto::PartyMatchJoinRequest>(2));
    Send(parties, 2, proto::PartyMatchJoinAnswer{1});
    CHECK(parties.MemberEntities(2).size() == 3);
}

TEST_CASE("parties: the client's party view from the session's events, and what it posts") {
    game::PlayerState state;
    proto::WorldEntered entered;
    entered.entity = 2;
    entered.character.name = "Beta";
    state.Apply(game::TitleEvent{entered});
    proto::PartyInfo info;
    info.party = 5;
    info.settings = game::kPartyExpShare;
    info.leader = 1001;
    info.members.push_back({1001, 1, "Alpha", 20});
    info.members.push_back({1002, 2, "Beta", 10});
    state.Apply(game::TitleEvent{info});
    CHECK(state.self.party == 5);
    CHECK_FALSE(state.self.partyLeader);
    CHECK(state.party.SelfId() == 1002);
    CHECK(state.party.Others().size() == 1);
    CHECK(state.party.Capacity() == 8);
    state.Apply(game::TitleEvent{proto::PartyMemberJoined{{1003, 3, "Gamma", 5}}});
    CHECK(state.party.members.size() == 3);
    CHECK(state.party.notices.back().key == "UIIT_MSG_PARTY_JOIN");
    CHECK(state.party.notices.back().args == std::vector<std::string>{"Gamma"});
    proto::PartyMemberUpdate u;
    u.member = 1003;
    u.mask = proto::kPartyVitals;
    u.hpTenths = 3;
    u.mpTenths = 7;
    state.Apply(game::TitleEvent{u});
    CHECK(state.party.Member(1003)->hpTenths == 3);
    CHECK(state.party.Member(1003)->mpTenths == 7);
    state.Apply(game::TitleEvent{proto::PartyMemberLeft{1003, proto::PartyLeaveReason::Kicked}});
    CHECK(state.party.notices.back().key == "UIIT_MSG_PARTY_BOOTED");
    CHECK(state.party.members.size() == 2);
    state.Apply(game::TitleEvent{proto::PartyLeaderChanged{1002}});
    CHECK(state.self.partyLeader);
    CHECK(state.party.notices.back().key == "UIIT_MSG_PARTY_HOST_ROLE_DELEGATED");
    CHECK(state.party.notices.back().args == std::vector<std::string>{"Beta"});
    state.Apply(game::TitleEvent{proto::PartyLoot{1001, 0, 0, 120}});
    CHECK(state.party.notices.back().key == "UIIT_MSG_PARTYGET_GOLD");
    // A teleport keeps the party; the dismissal ends it.
    state.Apply(game::TitleEvent{entered});
    CHECK(state.self.party == 5);
    state.Apply(game::TitleEvent{proto::PartyDismissed{1}});
    CHECK(state.self.party == 0);
    CHECK_FALSE(state.party.InParty());
    CHECK(state.party.notices.back().key == "UIIT_MSG_PARTY_BROKEN");
    // A request, then the party's refusal texts.
    state.Apply(game::TitleEvent{proto::PartyInvited{1, "Alpha", game::kPartyItemShare, false}});
    REQUIRE(state.party.invitation);
    CHECK(state.party.invitation->name == "Alpha");
    state.Apply(game::TitleEvent{proto::PartyResult{proto::PartyAction::Answer, S::Timeout, ""}});
    CHECK_FALSE(state.party.invitation);
    CHECK(state.party.notices.back().key == "UIIT_MSG_PARTYERR_TIMEOUT");
    // Matching: registered, a join request waiting for the player, answered.
    state.Apply(game::TitleEvent{proto::PartyMatchAnswer{proto::PartyMatchAction::Register, S::Ok, 9}});
    CHECK(state.party.ownEntry == 9);
    state.Apply(game::TitleEvent{proto::PartyMatchJoinRequest{9, 1004, "Delta", 12, 1}});
    REQUIRE(state.party.joinRequest);
    state.Apply(game::TitleEvent{proto::PartyMatchAnswer{proto::PartyMatchAction::Answer, S::Ok, 9}});
    CHECK_FALSE(state.party.joinRequest);
    state.Apply(game::TitleEvent{proto::PartyMatchAnswer{proto::PartyMatchAction::Removed, S::AlreadyFull, 9}});
    CHECK(state.party.ownEntry == 0);
    CHECK(state.party.notices.back().key == "UIIT_MSG_PARTYMATCH_DELETE_COMPLETE2");
}

// --- On our game server and database, over the wire --------------------------------------------------

namespace {

std::shared_ptr<game::CharacterRules> PartyRules() {
    auto rules = std::make_shared<game::CharacterRules>();
    for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
    rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
    rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
    rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
    return rules;
}

// A game server in-process and its players: each made in the lobby, of a level, entered.
struct PartyServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    std::uint32_t weapon = 10; // what the players are made with in hand
    explicit PartyServer(TestDatabase& db) : t(db) {
        config.listen = Local();
        config.databaseUrl = t.Url();
        config.rules = PartyRules();
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        config.devCommands = true;
        config.databaseThreads = 1;
    }
    bool Start() { return game.Start(config, error); }
    void Pump() { game.Poll(2ms); }
    struct Player {
        std::unique_ptr<Client> client;
        std::int64_t character = 0;
        proto::WorldEntered entered;
    };
    // `before` runs on the character before it enters (its level, gold, place, items).
    Player Make(const std::string& name, int level, const std::function<void(std::int64_t)>& before = {}) {
        Player p;
        REQUIRE(server::CreateAccount(t.Db(), name + "acct", "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        const std::int64_t account = server::FindAccount(t.Db(), name + "acct", ok)->id;
        p.client = std::make_unique<Client>(net, game.Port(), [this] { Pump(); });
        REQUIRE(p.client->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), account, config.id, 30, error);
        p.client->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(p.client->Expect(entered));
        proto::CharacterCreate m;
        m.name = name;
        m.model = 100;
        m.chest = 1;
        m.legs = 2;
        m.shoes = 3;
        m.weapon = weapon;
        p.client->Send(m);
        proto::LobbyResult r;
        REQUIRE(p.client->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        const std::vector<server::CharacterRow> list = server::ListCharacters(t.Db(), account, config.id, 0, ok);
        REQUIRE(list.size() == 1);
        p.character = list[0].id;
        REQUIRE(server::SaveProgress(t.Db(), {{p.character, std::uint8_t(level), 0, 0, 0, 40, 40, 0}}));
        if (before) before(p.character);
        p.client->Send(proto::CharacterSelect{name});
        REQUIRE(p.client->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        p.client->Send(proto::WorldEnter{});
        REQUIRE(p.client->Expect(p.entered));
        REQUIRE(p.entered.status == proto::WorldStatus::Ok);
        return p;
    }
};

std::filesystem::path PartyGameDir() {
    const char* dir = std::getenv("SR_GAME_DIR");
    return dir ? std::filesystem::path(dir) : std::filesystem::path();
}

} // namespace

TEST_CASE("Game server: a party over the wire: invite, decline, accept, the members told, chat, settings, kick, leave, dismiss, handed over") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    PartyServer s(t);
    REQUIRE_MESSAGE(s.Start(), s.error);
    auto a = s.Make("Alpha", 20);
    auto b = s.Make("Beta", 15);
    auto c = s.Make("Gamma", 10);
    auto d = s.Make("Delta", 3);
    const auto id = [](const PartyServer::Player& p) { return std::uint32_t(p.character); };

    // Delta is too low to make a party; Delta says no to Alpha.
    d.client->Send(proto::PartyInvite{a.entered.entity, 0});
    proto::PartyResult result;
    REQUIRE(d.client->Expect(result));
    CHECK(result.status == S::LowLevel);
    a.client->Send(proto::PartyInvite{d.entered.entity, game::kPartyExpShare});
    proto::PartyInvited invited;
    REQUIRE(d.client->Expect(invited));
    CHECK(invited.name == "Alpha");
    d.client->Send(proto::PartyInviteAnswer{0});
    REQUIRE(a.client->Expect(result));
    CHECK(result.status == S::Refused);
    CHECK(result.name == "Delta");

    // Beta accepts a party of shared exp and items: both told the whole party, Alpha its master.
    a.client->Send(proto::PartyInvite{b.entered.entity, game::kPartyExpShare | game::kPartyItemShare});
    REQUIRE(b.client->Expect(invited));
    CHECK(invited.settings == (game::kPartyExpShare | game::kPartyItemShare));
    b.client->Send(proto::PartyInviteAnswer{1});
    proto::PartyInfo info;
    REQUIRE(a.client->Expect(info));
    REQUIRE(b.client->Expect(info));
    CHECK(info.leader == id(a));
    REQUIRE(info.members.size() == 2);
    CHECK(info.members[0].name == "Alpha");
    CHECK(info.members[0].level == 20);
    CHECK(info.members[1].name == "Beta");
    CHECK(info.members[1].entity == b.entered.entity);
    CHECK(info.members[1].hpTenths > 0);
    // Gamma joins: the others hear of it, Gamma gets the party.
    a.client->Send(proto::PartyInvite{c.entered.entity, 0});
    REQUIRE(c.client->Expect(invited));
    CHECK(invited.join);
    c.client->Send(proto::PartyInviteAnswer{1});
    proto::PartyMemberJoined joined;
    REQUIRE(a.client->Expect(joined));
    REQUIRE(b.client->Expect(joined));
    CHECK(joined.member.name == "Gamma");
    REQUIRE(c.client->Expect(info));
    CHECK(info.members.size() == 3);

    // The party's chat reaches the members only.
    b.client->Send(proto::ChatSend{proto::ChatKind::Party, 1, "", "heal please"});
    proto::ChatAnswer said;
    REQUIRE(b.client->Expect(said));
    CHECK(said.result == proto::ChatResult::Ok);
    proto::ChatLine line;
    REQUIRE(a.client->Expect(line));
    CHECK(line.kind == proto::ChatKind::Party);
    CHECK(line.name == "Beta");
    CHECK(line.text == "heal please");
    REQUIRE(c.client->Expect(line));
    d.client->Send(proto::ChatSend{proto::ChatKind::Party, 2, "", "me too"});
    REQUIRE(d.client->Expect(said));
    CHECK(said.result == proto::ChatResult::NotInParty);

    // A member's HP: Beta dies, the others see its bar empty (by the tenth).
    b.client->Send(proto::DevDie{});
    proto::PartyMemberUpdate update;
    bool emptied = false;
    for (int i = 0; i < 10 && !emptied; ++i) emptied = a.client->Expect(update, 2s) && update.member == id(b) && update.hpTenths == 0;
    CHECK(emptied);
    b.client->Send(proto::Resurrect{1}); // up again where it fell

    // The master changes the settings (ours): everyone told.
    a.client->Send(proto::PartySetSettings{game::kPartyItemShare});
    proto::PartySettingsChanged settings;
    REQUIRE(c.client->Expect(settings));
    CHECK(settings.settings == game::kPartyItemShare);
    // Only the master sends a member away: Gamma kicked, everyone (Gamma too) told.
    b.client->Send(proto::PartyKick{id(c)});
    REQUIRE(b.client->Expect(result));
    CHECK(result.status == S::NotLeader);
    a.client->Send(proto::PartyKick{id(c)});
    proto::PartyMemberLeft left;
    REQUIRE(c.client->Expect(left));
    CHECK(left.member == id(c));
    CHECK(left.reason == proto::PartyLeaveReason::Kicked);
    REQUIRE(b.client->Expect(left));
    REQUIRE(a.client->Expect(left));
    // Beta leaves: one is left, the party is dismissed.
    b.client->Send(proto::PartyLeave{});
    REQUIRE(b.client->Expect(left)); // its own leaving too
    CHECK(left.member == id(b));
    REQUIRE(a.client->Expect(left));
    CHECK(left.reason == proto::PartyLeaveReason::Left);
    proto::PartyDismissed dismissed;
    REQUIRE(a.client->Expect(dismissed));
    CHECK(dismissed.reason == 2);

    // Again, three of them; the master's leave hands over as a member's does: Beta leads.
    const auto form = [&](std::vector<PartyServer::Player*> members) {
        for (std::size_t i = 1; i < members.size(); ++i) {
            a.client->Send(proto::PartyInvite{members[i]->entered.entity, game::kPartyExpShare});
            REQUIRE(members[i]->client->Expect(invited));
            members[i]->client->Send(proto::PartyInviteAnswer{1});
            REQUIRE(members[i]->client->Expect(info));
        }
    };
    form({&a, &b, &c});
    a.client->Send(proto::PartyLeave{});
    {
        proto::PartyLeaderChanged next;
        REQUIRE(c.client->Expect(next));
        CHECK(next.member == id(b));
        REQUIRE(b.client->Expect(left));
        CHECK(left.member == id(a));
        CHECK(left.reason == proto::PartyLeaveReason::Left);
    }
    // Beta leaves the two left: the party goes; then Alpha forms again and leaves the world: Beta leads.
    b.client->Send(proto::PartyLeave{});
    REQUIRE(c.client->Expect(dismissed));
    CHECK(dismissed.reason == 2);
    REQUIRE(b.client->Expect(left));
    CHECK(left.member == id(b));
    form({&a, &b, &c});
    a.client->Send(proto::WorldLeave{});
    proto::WorldLeft out;
    REQUIRE(a.client->Expect(out));
    REQUIRE(b.client->Expect(left));
    CHECK(left.member == id(a));
    CHECK(left.reason == proto::PartyLeaveReason::Logout);
    proto::PartyLeaderChanged leader;
    REQUIRE(c.client->Expect(leader));
    CHECK(leader.member == id(b));
    s.game.Stop();
}

TEST_CASE("Game server: shared loot over the wire: gold split among the members near, items in turn, the party told") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    PartyServer s(t);
    // The creation's items as rows (an item picked up needs its row).
    auto rules = PartyRules();
    auto table = std::make_shared<assets::ItemTable>();
    const auto row = [&](std::uint32_t id, const char* code, int tid3, int tid4) {
        assets::ItemRow r;
        r.refObjId = id;
        r.codename = code;
        r.typeId = {3, 1, tid3, tid4};
        r.sex = 2;
        r.requirementTypes = {1, -1, -1, -1};
        r.requirementLevels = {1, 0, 0, 0};
        r.stats.assign(55, 0.0f);
        r.stats[0] = 40;
        table->Add(r);
    };
    row(1, "CHEST", 1, 3);
    row(2, "LEGS", 1, 4);
    row(3, "SHOES", 1, 6);
    row(10, "SWORD", 6, 2);
    row(7, "ITEM_ETC_GOLD_01", 3, 5); // the pouches gold lies on the ground as
    row(8, "ITEM_ETC_GOLD_02", 3, 5);
    row(9, "ITEM_ETC_GOLD_03", 3, 5);
    rules->items = table;
    s.config.rules = rules;
    REQUIRE_MESSAGE(s.Start(), s.error);
    auto a = s.Make("Alpha", 20, [&](std::int64_t character) { REQUIRE(server::SaveGold(t.Db(), character, 1000)); });
    auto b = s.Make("Beta", 20);
    a.client->Send(proto::PartyInvite{b.entered.entity, game::kPartyItemShare});
    proto::PartyInvited invited;
    REQUIRE(b.client->Expect(invited));
    b.client->Send(proto::PartyInviteAnswer{1});
    proto::PartyInfo info;
    REQUIRE(a.client->Expect(info));
    REQUIRE(b.client->Expect(info));
    // A ground item Beta sees appear (the gold or the item Alpha dropped).
    const auto dropped = [&](proto::EntityKind kind) {
        proto::EntityAppeared seen;
        for (int i = 0; i < 20; ++i) {
            if (b.client->Expect(seen, 2s) && seen.kind == kind) return seen.entity;
        }
        return 0u;
    };
    // Alpha drops 101 gold, Beta picks it up: 51 for Beta (the picker), 50 for Alpha.
    a.client->Send(proto::DropGold{101});
    proto::GoldChanged gold;
    REQUIRE(a.client->Expect(gold));
    CHECK(gold.gold == 899);
    std::uint32_t pouch = dropped(proto::EntityKind::GroundItem);
    REQUIRE(pouch != 0);
    b.client->Send(proto::PickUp{pouch});
    REQUIRE(b.client->Expect(gold));
    CHECK(gold.gold == 51);
    REQUIRE(a.client->Expect(gold));
    CHECK(gold.gold == 949);
    proto::PartyLoot loot;
    REQUIRE(a.client->Expect(loot));
    CHECK(loot.refId == 0);
    CHECK(loot.gold == 50);
    REQUIRE(b.client->Expect(loot));
    CHECK(loot.gold == 51);

    // Alpha's sword into the bag and dropped; Beta picks it up: it goes to Alpha (the first's turn),
    // the party told.
    a.client->Send(proto::ItemMove{6, 13, 0});
    proto::ItemMoved moved;
    REQUIRE(a.client->Expect(moved));
    REQUIRE(moved.status == proto::ItemStatus::Ok);
    a.client->Send(proto::DropItem{13});
    REQUIRE(a.client->Expect(moved));
    std::uint32_t sword = dropped(proto::EntityKind::GroundItem);
    REQUIRE(sword != 0);
    b.client->Send(proto::PickUp{sword});
    REQUIRE(a.client->Expect(moved));
    CHECK(moved.status == proto::ItemStatus::Ok);
    REQUIRE(moved.changed.size() == 1);
    CHECK(moved.changed[0].item == 10);
    REQUIRE(b.client->Expect(loot));
    CHECK(loot.member == std::uint32_t(a.character));
    CHECK(loot.refId == 10);
    REQUIRE(a.client->Expect(loot));
    // Dropped again: Beta's turn now.
    a.client->Send(proto::DropItem{moved.changed[0].slot});
    REQUIRE(a.client->Expect(moved));
    sword = dropped(proto::EntityKind::GroundItem);
    REQUIRE(sword != 0);
    b.client->Send(proto::PickUp{sword});
    REQUIRE(b.client->Expect(moved));
    CHECK(moved.status == proto::ItemStatus::Ok);
    REQUIRE(moved.changed.size() == 1);
    CHECK(moved.changed[0].item == 10);
    REQUIRE(a.client->Expect(loot));
    CHECK(loot.member == std::uint32_t(b.character));

    // Without the share (the master changes it): the picker keeps the gold.
    a.client->Send(proto::PartySetSettings{0});
    proto::PartySettingsChanged changed;
    REQUIRE(b.client->Expect(changed));
    a.client->Send(proto::DropGold{10});
    REQUIRE(a.client->Expect(gold));
    pouch = dropped(proto::EntityKind::GroundItem);
    REQUIRE(pouch != 0);
    b.client->Send(proto::PickUp{pouch});
    REQUIRE(b.client->Expect(gold));
    CHECK(gold.gold == 61);
    s.game.Stop();
}

TEST_CASE("Game server: party matching over the wire: listed with our settings, listed to others, a join asked and accepted, gone when dismissed") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    PartyServer s(t);
    REQUIRE_MESSAGE(s.Start(), s.error);
    auto a = s.Make("Alpha", 30);
    auto b = s.Make("Beta", 25);
    auto c = s.Make("Gamma", 60);
    // Alpha lists a party (none yet) with free exp and shared items (ours: chosen here).
    proto::PartyMatchForm form{game::kPartyItemShare, std::uint8_t(game::PartyPurpose::Quest), 20, 40, "When will the quests end?"};
    a.client->Send(proto::PartyMatchRegister{form});
    proto::PartyMatchAnswer answer;
    REQUIRE(a.client->Expect(answer));
    CHECK(answer.action == proto::PartyMatchAction::Register);
    REQUIRE(answer.status == S::Ok);
    const std::uint32_t entry = answer.entry;
    // Beta lists the listings: Alpha's, 1 of 4.
    b.client->Send(proto::PartyMatchList{0, 0xFF, 0, 0, ""});
    proto::PartyMatchListing listing;
    REQUIRE(b.client->Expect(listing));
    REQUIRE(listing.entries.size() == 1);
    CHECK(listing.entries[0].entry == entry);
    CHECK(listing.entries[0].name == "Alpha");
    CHECK(listing.entries[0].form.purpose == std::uint8_t(game::PartyPurpose::Quest));
    CHECK(listing.entries[0].form.title == "When will the quests end?");
    CHECK(listing.entries[0].members == 1);
    CHECK(listing.entries[0].capacity == 4);
    // Gamma is above the range; Beta asks, Alpha is asked and accepts.
    c.client->Send(proto::PartyMatchJoin{entry});
    REQUIRE(c.client->Expect(answer));
    CHECK(answer.status == S::MatchLevel);
    b.client->Send(proto::PartyMatchJoin{entry});
    REQUIRE(b.client->Expect(answer));
    CHECK(answer.action == proto::PartyMatchAction::Join);
    CHECK(answer.status == S::Ok);
    proto::PartyMatchJoinRequest request;
    REQUIRE(a.client->Expect(request));
    CHECK(request.name == "Beta");
    CHECK(request.level == 25);
    a.client->Send(proto::PartyMatchJoinAnswer{1});
    REQUIRE(b.client->Expect(answer));
    CHECK(answer.action == proto::PartyMatchAction::Answer);
    CHECK(answer.status == S::Ok);
    proto::PartyInfo info;
    REQUIRE(b.client->Expect(info));
    CHECK(info.settings == game::kPartyItemShare); // the listing's settings
    CHECK(info.leader == std::uint32_t(a.character));
    CHECK(info.members.size() == 2);
    // The listing shows 2 now; Alpha dismisses: the listing is gone.
    c.client->Send(proto::PartyMatchList{0, 0xFF, 0, 0, ""});
    REQUIRE(c.client->Expect(listing));
    REQUIRE(listing.entries.size() == 1);
    CHECK(listing.entries[0].members == 2);
    a.client->Send(proto::PartyLeave{});
    bool removed = false;
    for (int i = 0; i < 4 && !removed; ++i) removed = a.client->Expect(answer, 2s) && answer.action == proto::PartyMatchAction::Removed;
    CHECK(removed);
    c.client->Send(proto::PartyMatchList{0, 0xFF, 0, 0, ""});
    REQUIRE(c.client->Expect(listing));
    CHECK(listing.entries.empty());
    s.game.Stop();
}

namespace {

// Two servers' parties joined by a bus held in the test, so time is the test's own.
struct BusHost : Host {
    std::uint16_t id = 0;
    std::vector<std::string> sent;
    std::uint16_t PartyServer() const override { return id; }
    bool PartyOnBus() const override { return true; }
    void PartyPublish(std::string text) override { sent.push_back(std::move(text)); }
};

template <typename T>
void Ask(server::Parties& parties, std::uint32_t entity, const T& message, server::Parties::Clock::time_point now) {
    REQUIRE(parties.Handle(entity, T::kId, proto::Encode(message), now) == server::Parties::Handled::Yes);
}

} // namespace

TEST_CASE("Parties on two servers: what a server decides, and a server gone silent taking its parties and listings with it") {
    using Clock = server::Parties::Clock;
    BusHost hostA, hostB;
    hostA.id = 1;
    hostB.id = 2;
    hostA.Add(10, "Ann", 30);
    hostB.Add(20, "Bob", 30);
    server::Parties a(hostA), b(hostB);
    const Clock::time_point now = Clock::now();
    const auto flush = [&] {
        while (!hostA.sent.empty() || !hostB.sent.empty()) {
            const auto fromA = std::exchange(hostA.sent, {});
            const auto fromB = std::exchange(hostB.sent, {});
            for (const std::string& text : fromA) b.BusMessage(text, now);
            for (const std::string& text : fromB) a.BusMessage(text, now);
        }
    };
    a.Hello();
    b.Hello();
    flush();
    CHECK(a.OthersOnline());
    CHECK(b.OthersOnline());

    proto::PartyMatchForm form;
    form.settings = game::kPartyExpShare;
    form.levelMin = 1;
    form.levelMax = 110;
    form.title = "hunting";
    Ask(a, 10, proto::PartyMatchRegister{form}, now);
    flush();
    REQUIRE(hostA.Take<proto::PartyMatchAnswer>(10).size() == 1);
    CHECK(b.Listings() == 1);
    Ask(b, 20, proto::PartyMatchList{}, now);
    const auto list = hostB.Take<proto::PartyMatchListing>(20);
    REQUIRE(list.size() == 1);
    REQUIRE(list[0].entries.size() == 1);
    CHECK(list[0].entries[0].name == "Ann");

    // Bob's request crosses to Ann's server, which forms the party; Bob's server keeps a mirror.
    Ask(b, 20, proto::PartyMatchJoin{list[0].entries[0].entry}, now);
    flush();
    CHECK(hostA.Take<proto::PartyMatchJoinRequest>(10).size() == 1);
    Ask(a, 10, proto::PartyMatchJoinAnswer{1}, now);
    flush();
    CHECK(a.Count() == 1);
    CHECK(b.Count() == 1);
    const auto info = hostB.Take<proto::PartyInfo>(20);
    REQUIRE(info.size() == 1);
    REQUIRE(info[0].members.size() == 2);
    CHECK(info[0].members[0].entity == 0u); // Ann: on the other server, no entity of ours
    CHECK(info[0].members[1].entity == 20u);
    CHECK(b.MemberEntities(20) == std::vector<std::uint32_t>{20}); // only those here

    // A member's health crosses, without a place or an entity from the other world.
    hostB.players[20].hp = 100;
    b.Tick(now + std::chrono::seconds(1));
    flush();
    const auto news = hostA.Take<proto::PartyMemberUpdate>(10);
    REQUIRE(news.size() == 1);
    CHECK(news[0].hpTenths == 2); // 1 + a tenth
    CHECK(news[0].entity == 0u);
    CHECK((news[0].mask & proto::kPartyPosition) == 0);

    // Each hears nothing from the other for 16 s: Ann's party loses Bob and ends; Bob's mirror ends
    // with the home that kept it, and the listing with it.
    const Clock::time_point later = now + std::chrono::seconds(17);
    a.Tick(later);
    CHECK_FALSE(a.OthersOnline());
    CHECK(hostA.Take<proto::PartyDismissed>(10).size() == 1);
    CHECK(a.Count() == 0);
    b.Tick(later);
    CHECK(hostB.Take<proto::PartyDismissed>(20).size() == 1);
    CHECK(b.Count() == 0);
    CHECK(b.Listings() == 0);
}

TEST_CASE("Game server: a kill's experience shared by an exp-shared party: a member who did no harm gets its share (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = PartyGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    auto items = std::make_shared<assets::ItemTable>();
    REQUIRE_MESSAGE(items->Load(fs, error), error);
    // A Chinese sword of the first levels, the one of the highest level (a bare hand does 1).
    const assets::ItemRow* sword = nullptr;
    for (const auto& [code, r] : items->rows()) {
        if (r.typeId != std::array<int, 4>{3, 1, 6, 2} || r.country != 0 || !r.inService || r.requiredLevel > 5 || r.rarity != 0) continue;
        if (sword == nullptr || r.requiredLevel > sword->requiredLevel || (r.requiredLevel == sword->requiredLevel && r.refObjId < sword->refObjId)) sword = &r;
    }
    REQUIRE(sword != nullptr);
    // The weakest monster of the first levels that fights (its attack a skill of the table).
    const assets::ObjectRow* weakest = nullptr;
    for (const auto& [code, r] : characters->rows()) {
        if (r.typeId[1] != 2 || r.typeId[2] != 1 || r.level < 5 || r.level > 8 || r.rarity != 0 || r.maxHp < 20 || r.experience == 0) continue;
        if (!code.starts_with("MOB_") || code.find("_NPC") != std::string::npos || code.find("THIEF") != std::string::npos) continue;
        const assets::SkillRow* attack = r.skills[0] != 0 ? skills->Find(r.skills[0]) : nullptr;
        if (attack == nullptr || !attack->attack) continue;
        if (weakest == nullptr || r.maxHp < weakest->maxHp) weakest = &r;
    }
    REQUIRE(weakest != nullptr);
    MESSAGE("the monster: " << weakest->codename << ", level " << weakest->level << ", " << weakest->maxHp << " HP");
    PartyServer s(t);
    auto rules = PartyRules();
    rules->items = items;
    rules->creation.AddWeapon(game::CreationRules::Race::China, sword->refObjId);
    s.config.rules = rules;
    s.weapon = sword->refObjId;
    s.config.files = &fs;
    s.config.characters = characters;
    s.config.skills = skills;
    s.config.levels.assign(120, 50'000'000);
    s.config.monsterExperience.assign(120, 100);
    server::MonsterNest nest;
    nest.refObjId = weakest->refObjId;
    nest.position = {assets::RegionId(76, 105), 640, 140, 900}; // the field west of Constantinople (the town is safe ground)
    nest.radius = 300;
    s.config.monsterNests = {nest};
    REQUIRE_MESSAGE(s.Start(), s.error);
    // Alpha and Beta three levels above it (the experience's level gap still 1), at full health,
    // beside the nest west of Constantinople, the sword in hand; Gamma there too, alone.
    const int level = weakest->level + 3;
    const auto place = [&](float x) {
        return [&, x](std::int64_t character) {
            server::CharacterRow::Place here;
            here.region = assets::RegionId(76, 105);
            here.x = x + 265.0f;
            here.y = 140;
            here.z = 900;
            REQUIRE(server::SavePlaces(t.Db(), {{character, here}}));
            REQUIRE(server::SaveVitals(t.Db(), character, 100'000, 100'000));
        };
    };
    auto a = s.Make("Alpha", level, place(345));
    auto b = s.Make("Beta", level, place(325));
    auto c = s.Make("Gamma", level, place(335));
    a.client->Send(proto::PartyInvite{b.entered.entity, game::kPartyExpShare | game::kPartyItemShare});
    proto::PartyInvited invited;
    REQUIRE(b.client->Expect(invited));
    b.client->Send(proto::PartyInviteAnswer{1});
    proto::PartyInfo info;
    REQUIRE(a.client->Expect(info));
    // The monster comes up once the navigation is in; Alpha attacks it until it dies.
    std::uint32_t monster = 0;
    for (int i = 0; i < 40 && monster == 0; ++i) {
        proto::EntityAppeared seen;
        if (a.client->Expect(seen, 1s) && seen.kind == proto::EntityKind::Monster) monster = seen.entity;
    }
    REQUIRE(monster != 0);
    a.client->Send(proto::AttackTarget{monster});
    proto::ExperienceGained ga, gb, gc;
    REQUIRE(a.client->Expect(ga, 60s));
    REQUIRE(b.client->Expect(gb, 5s));
    CHECK(ga.from == monster);
    CHECK(gb.from == monster);
    // Of one level, the two share it equally (each half of the party's kill times the bonus 1.1);
    // Gamma, outside the party, did no harm and gets nothing.
    CHECK(ga.experience > 0);
    CHECK(gb.experience == ga.experience);
    CHECK(gb.skillExperience == ga.skillExperience);
    CHECK_FALSE(c.client->Expect(gc, 1s));
    s.game.Stop();
}
