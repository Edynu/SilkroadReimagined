// The exchange between two players through the game server: asked, refused,
// out of reach; items and gold put in, taken out, refused by the original's rules; confirmed, approved,
// dealt and saved in one go; a cancel and a death ending it (set SR_DB_URL; otherwise skipped).

#include <doctest/doctest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ServerTestSupport.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/ExchangeProtocol.h"
#include "sr/server/GameServer.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;
using Code = proto::ExchangeCode;

namespace {

constexpr std::uint32_t kSword = 900001, kPotion = 900002, kQuest = 900003, kGoods = 900004;

std::shared_ptr<assets::ItemTable> Items() {
    auto items = std::make_shared<assets::ItemTable>();
    assets::ItemRow sword;
    sword.refObjId = kSword;
    sword.codename = "ITEM_TEST_SWORD";
    sword.typeId = {3, 1, 6, 2};
    sword.inService = true;
    items->Add(sword);
    assets::ItemRow potion;
    potion.refObjId = kPotion;
    potion.codename = "ITEM_TEST_POTION";
    potion.typeId = {3, 3, 1, 1};
    potion.inService = true;
    potion.maxStack = 50;
    items->Add(potion);
    assets::ItemRow quest = potion;
    quest.refObjId = kQuest;
    quest.codename = "ITEM_TEST_QUEST";
    quest.canTrade = false; // _RefObjCommon.CanTrade 0
    items->Add(quest);
    assets::ItemRow goods = potion;
    goods.refObjId = kGoods;
    goods.codename = "ITEM_ETC_TRADE_TEST";
    goods.typeId = {3, 3, 8, 1}; // trade goods
    items->Add(goods);
    return items;
}

struct ExchangeServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    explicit ExchangeServer(TestDatabase& db) : t(db) {
        config.listen = Local();
        config.databaseUrl = t.Url();
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
        rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
        rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
        rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
        rules->items = Items();
        config.rules = rules;
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        config.databaseThreads = 2;
        config.devCommands = true;
    }
    struct Player {
        std::unique_ptr<Client> client;
        std::int64_t character = 0;
        std::uint32_t entity = 0;
        proto::InventoryList bag;
    };
    // A character at (x, 1000) of Jangan's region with `items` in its bag (from place 13 on) and `gold`, entered.
    Player Make(const std::string& name, float x, const std::vector<server::StoredItem>& items, std::uint64_t gold,
                const std::function<void(std::int64_t character)>& beforeEntering = {}) {
        Player p;
        REQUIRE(server::CreateAccount(t.Db(), name + "acct", "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        const std::int64_t account = server::FindAccount(t.Db(), name + "acct", ok)->id;
        p.client = std::make_unique<Client>(net, game.Port(), [this] { game.Poll(2ms); });
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
        m.weapon = 10;
        p.client->Send(m);
        proto::LobbyResult r;
        REQUIRE(p.client->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        const std::vector<server::CharacterRow> list = server::ListCharacters(t.Db(), account, config.id, 0, ok);
        REQUIRE(list.size() == 1);
        p.character = list[0].id;
        server::CharacterRow::Place here;
        here.region = assets::RegionId(168, 97);
        here.x = x;
        here.z = 1000;
        REQUIRE(server::SavePlaces(t.Db(), {{p.character, here}}));
        // GiveItems is for a character nobody plays: its session aside for a moment.
        const std::int64_t jid = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {p.character}).Int(0, 0);
        REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {jid}).Ok());
        REQUIRE(server::GiveItems(t.Db(), config.id, name, items, gold, error) == server::GiveResult::Done);
        if (beforeEntering) beforeEntering(p.character);
        REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, $2)", {jid, int(config.id)}).Ok());
        p.client->Send(proto::CharacterSelect{name});
        REQUIRE(p.client->Expect(r));
        p.client->Send(proto::WorldEnter{});
        proto::WorldEntered w;
        REQUIRE(p.client->Expect(w));
        REQUIRE(w.status == proto::WorldStatus::Ok);
        p.entity = w.entity;
        REQUIRE(p.client->Expect(p.bag));
        return p;
    }
    // `a` asks `b`, `b` says yes: both see it open.
    void Open(Player& a, Player& b) {
        a.client->Send(proto::ExchangeAsk{b.entity});
        proto::ExchangeAsked asked;
        REQUIRE(b.client->Expect(asked));
        CHECK(asked.from == a.entity);
        b.client->Send(proto::ExchangeReply{1});
        proto::ExchangeOpened opened;
        REQUIRE(a.client->Expect(opened));
        CHECK(opened.peer == b.entity);
        REQUIRE(b.client->Expect(opened));
        CHECK(opened.peer == a.entity);
    }
};

server::StoredItem Stored(std::uint32_t refId, std::uint16_t count = 1) {
    server::StoredItem i;
    i.refId = refId;
    i.quantity = count;
    i.durability = count == 1 && refId == kSword ? 30 : 0;
    i.counted = refId != kSword;
    return i;
}

template <typename T>
T Next(ExchangeServer::Player& p) {
    T m;
    REQUIRE(p.client->Expect(m));
    return m;
}

Code Refusal(ExchangeServer::Player& p) { return Next<proto::ExchangeAnswer>(p).code; }

// The next news of a side: the player's own (`mine`) or the other's.
proto::ExchangeSide SideNews(ExchangeServer::Player& p, bool mine) {
    for (;;) {
        const auto m = Next<proto::ExchangeSide>(p);
        if ((m.mine != 0) == mine) return m;
    }
}

// The bag place an item of `refId` came in at.
std::uint8_t PlaceOf(const proto::InventoryList& bag, std::uint32_t refId) {
    for (const proto::ItemRecord& i : bag.items) {
        if (i.item == refId) return i.slot;
    }
    FAIL("no such item in the bag");
    return 0;
}

} // namespace

TEST_CASE("Game server: an exchange asked: out of reach, of itself, refused") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ExchangeServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Asker", 1000, {}, 0);
    auto b = s.Make("Neighbour", 1100, {}, 0);
    auto c = s.Make("Faraway", 1400, {}, 0);

    a.client->Send(proto::ExchangeAsk{c.entity}); // 400 away: beyond 320
    CHECK(Refusal(a) == Code::TooFar);
    a.client->Send(proto::ExchangeAsk{a.entity});
    CHECK(Refusal(a) == Code::InvalidTarget);
    a.client->Send(proto::ExchangeAsk{999999});
    CHECK(Refusal(a) == Code::InvalidTarget);

    // Asked, and no: the asker hears it.
    a.client->Send(proto::ExchangeAsk{b.entity});
    CHECK(Next<proto::ExchangeAsked>(b).name == "Asker");
    // Meanwhile the asked one cannot be asked by another (300 apart: within reach).
    c.client->Send(proto::ExchangeAsk{b.entity});
    CHECK(Refusal(c) == Code::Busy);
    b.client->Send(proto::ExchangeReply{0});
    CHECK(Next<proto::ExchangeClosed>(a).code == Code::Refused);
    CHECK(Next<proto::ExchangeClosed>(b).code == Code::Ok);

    // An approval before both confirmed ends it for both (the original's 0x181C).
    s.Open(a, b);
    a.client->Send(proto::ExchangeConfirm{});
    CHECK(SideNews(a, true).step == proto::ExchangeStep::Confirmed);
    b.client->Send(proto::ExchangeApprove{});
    CHECK(Next<proto::ExchangeClosed>(a).code == Code::WrongStep);
    CHECK(Next<proto::ExchangeClosed>(b).code == Code::WrongStep);

    // A cancel: the other hears "cancelled by the user"; a death ends it too.
    s.Open(a, b);
    b.client->Send(proto::ExchangeCancel{});
    CHECK(Next<proto::ExchangeClosed>(b).code == Code::Ok);
    CHECK(Next<proto::ExchangeClosed>(a).code == Code::CancelledByUser);
    s.Open(a, b);
    a.client->Send(proto::DevDie{});
    CHECK(Next<proto::ExchangeClosed>(a).code == Code::Cancelled);
    CHECK(Next<proto::ExchangeClosed>(b).code == Code::Cancelled);
    s.game.Stop();
}

TEST_CASE("Game server: an exchange made: items and gold change hands, saved together") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ExchangeServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Seller", 1000, {Stored(kSword), Stored(kPotion, 10), Stored(kQuest)}, 5000);
    auto b = s.Make("Buyer", 1100, {Stored(kPotion, 3)}, 100);
    const std::uint8_t sword = PlaceOf(a.bag, kSword), potion = PlaceOf(a.bag, kPotion), quest = PlaceOf(a.bag, kQuest);
    const std::uint8_t theirs = PlaceOf(b.bag, kPotion);
    s.Open(a, b);

    // The original's refusals: a quest item, a worn one, the same twice, more gold than it holds.
    a.client->Send(proto::ExchangePut{quest});
    CHECK(Refusal(a) == Code::CannotTrade);
    a.client->Send(proto::ExchangePut{1});
    CHECK(Refusal(a) == Code::WornItem);
    a.client->Send(proto::ExchangePut{sword});
    auto mine = SideNews(a, true);
    CHECK(mine.mine == 1);
    REQUIRE(mine.items.size() == 1);
    CHECK(mine.items[0].item == kSword);
    CHECK(mine.places == std::vector<std::uint8_t>{sword});
    auto seen = SideNews(b, false);
    CHECK(seen.mine == 0);
    REQUIRE(seen.items.size() == 1);
    CHECK(seen.items[0].item == kSword);
    CHECK(seen.places.empty());
    a.client->Send(proto::ExchangePut{sword});
    CHECK(Refusal(a) == Code::AlreadyOffered);
    a.client->Send(proto::ExchangePut{potion});
    CHECK(SideNews(b, false).items.size() == 2);
    a.client->Send(proto::ExchangeGold{999999});
    CHECK(Refusal(a) == Code::NotEnoughGold);
    a.client->Send(proto::ExchangeGold{1000});
    CHECK(SideNews(b, false).gold == 1000);
    // The potions back out of it.
    a.client->Send(proto::ExchangeTake{1});
    seen = SideNews(b, false);
    REQUIRE(seen.items.size() == 1);
    CHECK(seen.items[0].item == kSword);
    // The bag is held while it is open.
    a.client->Send(proto::ItemMove{potion, 40, 0});
    proto::ItemMoved moved;
    REQUIRE(a.client->Expect(moved));
    CHECK(moved.status == proto::ItemStatus::Exchanging);

    b.client->Send(proto::ExchangePut{theirs});
    CHECK(SideNews(a, false).items.size() == 1);

    // Confirmed (an offer then stays), approved: the deal.
    a.client->Send(proto::ExchangeConfirm{});
    CHECK(SideNews(b, false).step == proto::ExchangeStep::Confirmed);
    b.client->Send(proto::ExchangeConfirm{});
    CHECK(SideNews(a, false).step == proto::ExchangeStep::Confirmed);
    a.client->Send(proto::ExchangeApprove{});
    b.client->Send(proto::ExchangeApprove{});
    const auto doneA = Next<proto::ExchangeClosed>(a);
    const auto doneB = Next<proto::ExchangeClosed>(b);
    CHECK(doneA.done == 1);
    CHECK(doneA.gold == 4000);
    CHECK(doneA.left.empty()); // the sword's place took the potions at once
    REQUIRE(doneA.arrived.size() == 1);
    CHECK(doneA.arrived[0].item == kPotion);
    CHECK(doneA.arrived[0].slot == sword);
    CHECK(doneA.arrived[0].quantity == 3);
    CHECK(doneB.done == 1);
    CHECK(doneB.gold == 1100);
    CHECK(doneB.left.empty());
    REQUIRE(doneB.arrived.size() == 1);
    CHECK(doneB.arrived[0].item == kSword);
    CHECK(doneB.arrived[0].slot == theirs);

    // In the database, together: the sword is the buyer's, the potions the seller's, both golds.
    const auto has = [&](std::int64_t character, std::uint32_t refId) {
        return t.Db()
            .Query("SELECT count(*) FROM _inventory i JOIN _items t ON t.id64 = i.itemid WHERE i.charid = $1 AND t.refitemid = $2", {character, std::int64_t(refId)})
            .Int(0, 0);
    };
    const auto gold = [&](std::int64_t character) { return t.Db().Query("SELECT remaingold FROM _char WHERE charid = $1", {character}).Int(0, 0); };
    for (int i = 0; i < 200 && has(b.character, kSword) == 0; ++i) s.game.Poll(5ms);
    CHECK(has(b.character, kSword) == 1);
    CHECK(has(a.character, kSword) == 0);
    CHECK(has(a.character, kPotion) == 2); // its own 10 and the 3 received
    // The stacks keep their counts (_items.data), the sword its durability.
    const auto data = [&](std::int64_t character, std::uint8_t slot) {
        return t.Db().Query("SELECT it.data FROM _inventory i JOIN _items it ON it.id64 = i.itemid WHERE i.charid = $1 AND i.slot = $2",
                            {character, int(slot)}).Int(0, 0);
    };
    CHECK(data(a.character, sword) == 3);
    CHECK(data(a.character, potion) == 10);
    CHECK(data(b.character, theirs) == 30);
    CHECK(has(b.character, kPotion) == 0);
    CHECK(gold(a.character) == 4000);
    CHECK(gold(b.character) == 1100);

    // Free again: its bag moves.
    a.client->Send(proto::ItemMove{potion, 40, 0});
    REQUIRE(a.client->Expect(moved));
    CHECK(moved.status == proto::ItemStatus::Ok);
    s.game.Stop();
}

TEST_CASE("Game server: an exchange refuses trade goods and a rental whose period runs") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ExchangeServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    // Its sword a rental for another day (_RentItemInfo by the item's id), another for a day past.
    auto a = s.Make("Renter", 1000, {Stored(kSword), Stored(kGoods, 5), Stored(kSword)}, 0, [&](std::int64_t character) {
        const server::QueryResult swords = t.Db().Query(
            "SELECT i.itemid FROM _inventory i JOIN _items it ON it.id64 = i.itemid WHERE i.charid = $1 AND it.refitemid = $2 ORDER BY i.slot",
            {character, std::int64_t(kSword)});
        REQUIRE(swords.Rows() == 2);
        REQUIRE(t.Db()
                    .Query("INSERT INTO _rentiteminfo (nitemdbid, nrenttype, ncandelete, ncanrecharge, periodbegintime, periodendtime) "
                           "VALUES ($1, 1, 1, 0, now() - interval '1 day', now() + interval '1 day'), ($2, 1, 1, 0, now() - interval '2 days', now() - interval '1 day')",
                           {swords.Int(0, 0), swords.Int(1, 0)})
                    .Ok());
    });
    auto b = s.Make("Partner", 1100, {}, 0);
    std::vector<std::uint8_t> swords;
    std::uint8_t goods = 0;
    for (const proto::ItemRecord& i : a.bag.items) {
        if (i.item == kSword) swords.push_back(i.slot);
        if (i.item == kGoods) goods = i.slot;
    }
    REQUIRE(swords.size() == 2);
    s.Open(a, b);
    a.client->Send(proto::ExchangePut{goods});
    CHECK(Refusal(a) == Code::TradeGoods);
    a.client->Send(proto::ExchangePut{swords[0]});
    CHECK(Refusal(a) == Code::Rented);
    // Its period over: it goes.
    a.client->Send(proto::ExchangePut{swords[1]});
    const auto seen = SideNews(b, false);
    REQUIRE(seen.items.size() == 1);
    CHECK(seen.items[0].item == kSword);
    s.game.Stop();
}
