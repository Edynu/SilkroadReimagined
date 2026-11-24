// Stalls and the stall network through the game server: a stall opened, items put up and
// refused by the original's rules, opened for business; visited, chatted in, bought from and saved in one go;
// closed by its owner or by its owner's leave; the network's search (in a town, once every 3 s), its pages
// and a purchase through it (set SR_DB_URL; otherwise skipped). The purchase between two game servers is in
// CrossServerTests.cpp.

#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ServerTestSupport.h"
#include "sr/assets/GameData.h"
#include "sr/assets/RegionInfo.h"
#include "sr/assets/StallNetworkData.h"
#include "sr/core/Jobs.h"
#include "sr/game/StallProtocol.h"
#include "sr/server/GameServer.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;
using Code = proto::StallCode;

namespace {

constexpr std::uint32_t kSword = 900101, kPotion = 900102, kQuest = 900103, kGoods = 900104, kBooth = 900105;
constexpr std::uint32_t kSwordGroup = 1, kPotionGroup = 42;

std::shared_ptr<assets::ItemTable> Items() {
    auto items = std::make_shared<assets::ItemTable>();
    assets::ItemRow sword;
    sword.refObjId = kSword;
    sword.codename = "ITEM_TEST_SWORD";
    sword.typeId = {3, 1, 6, 2};
    sword.itemClass = 4; // degree 2
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
    quest.canTrade = false;
    items->Add(quest);
    assets::ItemRow goods = potion;
    goods.refObjId = kGoods;
    goods.codename = "ITEM_ETC_TRADE_TEST";
    goods.typeId = {3, 3, 8, 1};
    items->Add(goods);
    assets::ItemRow booth = potion;
    booth.refObjId = kBooth;
    booth.codename = "ITEM_MALL_BOOTH_TEST";
    booth.typeId = {3, 3, 3, 4}; // a stall decoration (the item mall's booths)
    booth.maxStack = 1;
    items->Add(booth);
    return items;
}

// Jangan's region a town; the region east of it not.
std::shared_ptr<assets::RegionAreaTable> Towns() {
    auto towns = std::make_shared<assets::RegionAreaTable>();
    std::string error;
    REQUIRE(towns->Parse("#TOWN\tJangan\n168\t97\tALL\n#FIELD\tOutside\n169\t97\tALL\n", error));
    return towns;
}

std::shared_ptr<assets::StallNetworkData> Network() {
    auto n = std::make_shared<assets::StallNetworkData>();
    n->ParseCategories("1\tWK_CH_WEAPON\tUIIT_CTL_WK_CH_WEAPON\txxx\t0\t0\n"
                       "1\tWK_CH_WEAPON_SWORD\tUIIT_CTL_WK_CH_WEAPON_SWORD\tWK_CH_WEAPON\t1\t11\n"
                       "1\tWK_ARTICLES\tUIIT_CTL_WK_ARTICLES\txxx\t0\t0\n"
                       "1\tWK_ARTICLES_RECOVERY\tUIIT_CTL_WK_ARTICLES_RECOVERY\tWK_ARTICLES\t42\t0\n");
    n->ParseGroups("1\t1\t3\t1\t6\t2\n1\t42\t3\t3\t1\t1\n1\t54\t3\t3\t13\t0\n");
    return n;
}

struct StallServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    explicit StallServer(TestDatabase& db) : t(db) {
        config.listen = Local();
        config.databaseUrl = t.Url();
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
        rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
        rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
        rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
        rules->items = Items();
        config.rules = rules;
        config.towns = Towns();
        config.stallNetwork = Network();
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
    // A character at (x, 1000) of `region` (Jangan's by default) with `items` in its bag (from place 13 on)
    // and `gold`, entered.
    Player Make(const std::string& name, float x, const std::vector<server::StoredItem>& items, std::uint64_t gold,
                std::uint16_t region = assets::RegionId(168, 97), int level = 0) {
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
        here.region = region;
        here.x = x;
        here.z = 1000;
        REQUIRE(server::SavePlaces(t.Db(), {{p.character, here}}));
        if (level > 0) REQUIRE(server::SaveProgress(t.Db(), {{p.character, std::uint8_t(level), 0, 0, 0, 40, 40, 0}}));
        const std::int64_t jid = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {p.character}).Int(0, 0);
        REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {jid}).Ok());
        REQUIRE(server::GiveItems(t.Db(), config.id, name, items, gold, error) == server::GiveResult::Done);
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
};

server::StoredItem Stored(std::uint32_t refId, std::uint16_t count = 1) {
    server::StoredItem i;
    i.refId = refId;
    i.quantity = count;
    i.durability = refId == kSword ? 30 : 0;
    i.counted = refId != kSword;
    return i;
}

template <typename T>
T Next(StallServer::Player& p) {
    T m;
    REQUIRE(p.client->Expect(m));
    return m;
}

Code Refusal(StallServer::Player& p) { return Next<proto::StallAnswer>(p).code; }

std::uint8_t PlaceOf(const proto::InventoryList& bag, std::uint32_t refId, int nth = 0) {
    for (const proto::ItemRecord& i : bag.items) {
        if (i.item == refId && nth-- == 0) return i.slot;
    }
    FAIL("no such item in the bag");
    return 0;
}

// The owner's stall opened and `items` (bag place, price) put up at slots 0.., then opened for business.
void OpenWith(StallServer::Player& o, const std::vector<std::pair<std::uint8_t, std::uint64_t>>& items, bool open = true) {
    o.client->Send(proto::StallOpen{"Shop"});
    REQUIRE(Next<proto::StallView>(o).owner == o.entity);
    std::uint8_t slot = 0;
    for (const auto& [place, price] : items) {
        o.client->Send(proto::StallPut{slot++, place, price});
        REQUIRE(Next<proto::StallSlotChanged>(o).present == 1);
    }
    if (!open) return;
    o.client->Send(proto::StallSetOpen{1, 1});
    REQUIRE(Next<proto::StallStateChanged>(o).open == 1);
}

} // namespace

TEST_CASE("Game server: a stall opened, items put up by the original's rules, opened for business") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    StallServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto o = s.Make("Owner", 1000, {Stored(kSword), Stored(kPotion, 30), Stored(kQuest), Stored(kGoods)}, 0, assets::RegionId(168, 97), 10);
    const std::uint8_t sword = PlaceOf(o.bag, kSword), potion = PlaceOf(o.bag, kPotion), quest = PlaceOf(o.bag, kQuest), goods = PlaceOf(o.bag, kGoods);

    o.client->Send(proto::StallOpen{"   "});
    CHECK(Refusal(o) == Code::BadNameLength); // 1..64 bytes (0x3C09)
    // A party invitation of its waiting: no stall (0x3C16, the original's rule); declined, it may.
    auto g = s.Make("Guest", 1000, {}, 0);
    for (bool seen = false; !seen;) seen = Next<proto::EntityAppeared>(o).entity == g.entity; // in its sight first
    o.client->Send(proto::PartyInvite{g.entity, 0});
    proto::PartyInvited invited;
    REQUIRE(g.client->Expect(invited));
    o.client->Send(proto::StallOpen{"Bargains"});
    CHECK(Refusal(o) == Code::Busy);
    g.client->Send(proto::PartyInviteAnswer{0});
    for (int i = 0; i < 50; ++i) s.game.Poll(2ms);
    o.client->Send(proto::StallOpen{"Bargains"});
    const auto view = Next<proto::StallView>(o);
    CHECK(view.name == "Bargains");
    CHECK(view.open == 0);
    CHECK(view.items.empty());

    o.client->Send(proto::StallPut{0, sword, 0});
    CHECK(Refusal(o) == Code::BadPrice);
    o.client->Send(proto::StallPut{0, sword, proto::kStallMostPrice + 1});
    CHECK(Refusal(o) == Code::BadPrice);
    CHECK(proto::kStallMostPrice == 9999999999ull); // the original's most
    o.client->Send(proto::StallPut{0, 5, 100}); // a worn place
    CHECK(Refusal(o) == Code::NotBagItem);
    o.client->Send(proto::StallPut{0, quest, 100}); // CanTrade 0
    CHECK(Refusal(o) == Code::NotTradeable);
    o.client->Send(proto::StallPut{0, goods, 100}); // trade goods
    CHECK(Refusal(o) == Code::NotTradeable);
    o.client->Send(proto::StallPut{proto::kStallSlots, sword, 100});
    CHECK(Refusal(o) == Code::SlotEmpty);
    o.client->Send(proto::StallSetOpen{1, 1});
    CHECK(Refusal(o) == Code::NothingToSell);

    o.client->Send(proto::StallPut{0, sword, 5000});
    auto changed = Next<proto::StallSlotChanged>(o);
    CHECK(changed.present == 1);
    CHECK(changed.item.slot == 0);
    CHECK(changed.item.item.item == kSword);
    CHECK(changed.item.item.slot == sword); // the owner sees where it lies in its bag
    CHECK(changed.item.price == 5000);
    o.client->Send(proto::StallPut{1, sword, 10});
    CHECK(Refusal(o) == Code::AlreadyUp); // already up in slot 0 (0x3C13)
    o.client->Send(proto::StallPut{0, potion, 300}); // slot 0 holds the sword: the first free one takes it
    const auto moved = Next<proto::StallSlotChanged>(o);
    CHECK(moved.item.slot == 1);
    CHECK(moved.item.item.quantity == 30);
    o.client->Send(proto::StallPut{1, 0, 400}); // the price alone
    CHECK(Next<proto::StallSlotChanged>(o).item.price == 400);
    o.client->Send(proto::StallTake{1});
    CHECK(Next<proto::StallSlotChanged>(o).present == 0);
    o.client->Send(proto::StallTake{1});
    CHECK(Refusal(o) == Code::SlotEmpty);

    o.client->Send(proto::StallSetOpen{1, 1});
    const auto opened = Next<proto::StallStateChanged>(o);
    CHECK(opened.open == 1);
    CHECK(opened.network == 1); // in a town: registered in the network
    o.client->Send(proto::StallPut{1, potion, 300});
    CHECK(Refusal(o) == Code::Modifying); // changes only while modifying
    o.client->Send(proto::StallSetText{0, "Renamed"});
    CHECK(Refusal(o) == Code::Modifying);
    o.client->Send(proto::StallSetText{1, std::string(65, 'w')}); // the greeting's 64 bytes (0x3C0A)
    CHECK(Refusal(o) == Code::GreetingTooLong);
    o.client->Send(proto::StallSetText{1, "Welcome!"}); // the greeting at any time
    CHECK(Next<proto::StallStateChanged>(o).greeting == "Welcome!");

    // Its owner stays put: a walk is not taken.
    const auto before = s.game.GetWorld()->Where(o.entity);
    REQUIRE(before);
    o.client->Send(proto::MoveTo{proto::FromFlat({1100.0, 1000.0}, before->position.y)});
    for (int i = 0; i < 50; ++i) s.game.Poll(5ms);
    CHECK(s.game.GetWorld()->Where(o.entity)->position.x == doctest::Approx(before->position.x));
}

TEST_CASE("Game server: a stall visited, chatted in and bought from, saved in one go") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    StallServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto o = s.Make("Seller", 1000, {Stored(kSword), Stored(kPotion, 20)}, 100);
    auto v = s.Make("Visitor", 1060, {}, 10000);
    auto poor = s.Make("Poor", 1040, {}, 10);
    auto f = s.Make("Faraway", 1400, {}, 0);
    const std::uint8_t sword = PlaceOf(o.bag, kSword), potion = PlaceOf(o.bag, kPotion);
    OpenWith(o, {{sword, 5000}, {potion, 700}}, false);

    v.client->Send(proto::StallBuy{0});
    CHECK(Next<proto::StallBought>(v).code == Code::NotVisitor);
    f.client->Send(proto::StallVisit{o.entity});
    CHECK(Refusal(f) == Code::TooFar); // beyond the original's reach (100 between the positions in 3D)
    o.client->Send(proto::StallVisit{o.entity});
    CHECK(Refusal(o) == Code::OwnerOnly);
    v.client->Send(proto::StallVisit{o.entity});
    const auto seen = Next<proto::StallView>(v);
    CHECK(seen.name == "Shop");
    REQUIRE(seen.items.size() == 2);
    CHECK(seen.items[0].item.item == kSword);
    CHECK(seen.items[0].item.slot == 0); // a visitor does not see the owner's bag
    CHECK(Next<proto::StallPeopleChanged>(o).person.name == "Visitor");
    poor.client->Send(proto::StallVisit{o.entity});
    REQUIRE(Next<proto::StallView>(poor).people.size() == 2); // the stall's people, itself among them
    CHECK(Next<proto::StallPeopleChanged>(o).person.name == "Poor");
    CHECK(Next<proto::StallPeopleChanged>(v).person.name == "Poor");

    v.client->Send(proto::StallBuy{0});
    CHECK(Next<proto::StallBought>(v).code == Code::NotOpen); // being modified
    o.client->Send(proto::StallSetOpen{1, 1});
    CHECK(Next<proto::StallStateChanged>(v).open == 1);
    poor.client->Send(proto::StallBuy{1});
    CHECK(Next<proto::StallBought>(poor).code == Code::NotEnoughGold);
    v.client->Send(proto::StallBuy{7});
    CHECK(Next<proto::StallBought>(v).code == Code::SlotEmpty);

    v.client->Send(proto::StallChat{"how much for the sword?"});
    CHECK(Next<proto::StallChatLine>(o).text == "how much for the sword?");
    CHECK(Next<proto::StallChatLine>(poor).name == "Visitor");

    v.client->Send(proto::StallBuy{0});
    const auto got = Next<proto::StallBought>(v);
    REQUIRE(got.code == Code::Ok);
    CHECK(got.item.item == kSword);
    CHECK(got.gold == 5000);
    const auto sold = Next<proto::StallSold>(o);
    CHECK(sold.slot == 0);
    CHECK(sold.buyer == "Visitor");
    CHECK(sold.place == sword);
    CHECK(sold.gold == 5100);
    CHECK(Next<proto::StallSold>(poor).slot == 0);
    v.client->Send(proto::StallBuy{0});
    CHECK(Next<proto::StallBought>(v).code == Code::SlotEmpty);

    // Both sides in the database: the sword in the visitor's bag at the place it was told, the gold moved.
    const auto holder = [&](std::uint8_t place, std::int64_t character) {
        return t.Db()
            .Query("SELECT count(*) FROM _inventory i JOIN _items it ON it.id64 = i.itemid WHERE i.charid = $1 AND i.slot = $2 AND it.refitemid = $3",
                   {character, int(place), std::int64_t(kSword)})
            .Int(0, 0);
    };
    const auto gold = [&](std::int64_t character) { return t.Db().Query("SELECT remaingold FROM _char WHERE charid = $1", {character}).Int(0, 0); };
    for (int i = 0; i < 200 && holder(got.item.slot, v.character) == 0; ++i) s.game.Poll(5ms);
    CHECK(holder(got.item.slot, v.character) == 1);
    CHECK(holder(sword, o.character) == 0);
    CHECK(gold(v.character) == 5000);
    CHECK(gold(o.character) == 5100);

    // A walk takes a visitor out; the owner closing sends the others out.
    const auto at = s.game.GetWorld()->Where(v.entity);
    REQUIRE(at);
    v.client->Send(proto::MoveTo{proto::FromFlat({1080.0, 1020.0}, at->position.y)});
    CHECK(Next<proto::StallLeft>(v).code == Code::Ok);
    CHECK(Next<proto::StallPeopleChanged>(o).joined == 0);
    o.client->Send(proto::StallClose{});
    CHECK(Next<proto::StallLeft>(poor).code == Code::ClosedByHost);
    CHECK(Next<proto::StallLeft>(o).code == Code::Ok);
    proto::EntityStall gone;
    do {
        gone = Next<proto::EntityStall>(v);
    } while (!gone.name.empty());
    CHECK(gone.entity == o.entity);
}

TEST_CASE("Game server: a stall closes when its owner leaves the world; its visitors hear why") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    StallServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto o = s.Make("Leaver", 1000, {Stored(kPotion, 5)}, 0);
    auto v = s.Make("Browser", 1050, {}, 0);
    OpenWith(o, {{PlaceOf(o.bag, kPotion), 50}});
    v.client->Send(proto::StallVisit{o.entity});
    REQUIRE(Next<proto::StallView>(v).open == 1);
    o.client->Send(proto::WorldLeave{});
    CHECK(Next<proto::StallLeft>(v).code == Code::HostLeft);
}

TEST_CASE("Game server: the stall network: a search in a town once every 3 s, its pages, a purchase through it") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    StallServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    std::vector<server::StoredItem> ten(10, Stored(kPotion, 3));
    auto a = s.Make("PotionsA", 900, ten, 0);
    auto b = s.Make("PotionsB", 1300, std::vector<server::StoredItem>(8, Stored(kPotion, 2)), 0);
    auto c = s.Make("Swords", 1600, {Stored(kSword)}, 0);
    auto buyer = s.Make("Shopper", 1100, {}, 100000);
    auto outside = s.Make("Outsider", 1000, {}, 0, assets::RegionId(169, 97));
    auto field = s.Make("FieldShop", 500, {Stored(kPotion, 1)}, 0, assets::RegionId(169, 97));
    std::vector<std::pair<std::uint8_t, std::uint64_t>> aItems, bItems;
    for (int i = 0; i < 10; ++i) aItems.emplace_back(PlaceOf(a.bag, kPotion, i), 100 + i * 10); // 100..190
    for (int i = 0; i < 8; ++i) bItems.emplace_back(PlaceOf(b.bag, kPotion, i), 105 + i * 10);  // 105..175
    OpenWith(a, aItems);
    OpenWith(b, bItems);
    OpenWith(c, {{PlaceOf(c.bag, kSword), 9000}});
    // Opened for business outside a town: not in the network (the client's "use in a town" box).
    field.client->Send(proto::StallOpen{"Field"});
    Next<proto::StallView>(field);
    field.client->Send(proto::StallPut{0, PlaceOf(field.bag, kPotion), 1});
    Next<proto::StallSlotChanged>(field);
    field.client->Send(proto::StallSetOpen{1, 1});
    CHECK(Next<proto::StallStateChanged>(field).network == std::uint16_t(Code::NotTown));

    outside.client->Send(proto::NetworkSearch{kPotionGroup, 0});
    CHECK(Next<proto::NetworkPage>(outside).code == Code::NotTownSearch);

    buyer.client->Send(proto::NetworkSearch{kPotionGroup, 0});
    const auto first = Next<proto::NetworkPage>(buyer);
    REQUIRE(first.code == Code::Ok);
    CHECK(first.total == 18);
    CHECK(first.pages == 2);
    REQUIRE(first.rows.size() == proto::kNetworkPageRows);
    CHECK(first.rows[0].price == 100); // cheapest first
    CHECK(first.rows[1].price == 105);
    CHECK(first.rows[1].owner == "PotionsB");
    for (std::size_t i = 1; i < first.rows.size(); ++i) CHECK(first.rows[i - 1].price <= first.rows[i].price);
    buyer.client->Send(proto::NetworkSearch{kSwordGroup, 0});
    CHECK(Next<proto::NetworkPage>(buyer).code == Code::TooSoon); // within 3 s
    buyer.client->Send(proto::NetworkPageAsk{1}); // pages are free
    const auto second = Next<proto::NetworkPage>(buyer);
    CHECK(second.code == Code::Ok);
    CHECK(second.rows.size() == 3);

    // Bought through the network on this server: the cheapest potions, PotionsA's slot 0.
    const proto::NetworkRow row = first.rows[0];
    buyer.client->Send(proto::NetworkBuy{row.listing, row.revision, row.price});
    const auto got = Next<proto::StallBought>(buyer);
    REQUIRE(got.code == Code::Ok);
    CHECK(got.network == 1);
    CHECK(got.item.item == kPotion);
    CHECK(got.gold == 100000 - 100);
    const auto soldA = Next<proto::StallSold>(a);
    CHECK(soldA.network == 1);
    CHECK(soldA.commission == 1); // 1% of 100
    CHECK(soldA.gold == 99);
    buyer.client->Send(proto::NetworkBuy{row.listing, row.revision, row.price});
    CHECK(Next<proto::StallBought>(buyer).code == Code::Changed); // sold already

    // A new search after 3 s: by degree too; the sold row is gone.
    std::this_thread::sleep_for(3100ms);
    buyer.client->Send(proto::NetworkSearch{kSwordGroup, 3});
    CHECK(Next<proto::NetworkPage>(buyer).total == 0); // the sword is degree 2
    std::this_thread::sleep_for(3100ms);
    buyer.client->Send(proto::NetworkSearch{kSwordGroup, 2});
    const auto swords = Next<proto::NetworkPage>(buyer);
    REQUIRE(swords.rows.size() == 1);
    CHECK(swords.rows[0].owner == "Swords");
    CHECK(swords.rows[0].town == 0);
    // The detailed search (ours): a part of the name alone (every type), the grade, who may wear it.
    std::this_thread::sleep_for(3100ms);
    proto::NetworkSearch byName;
    byName.name = "SWORD"; // any case; the server reads the names (here the codenames: no game files)
    buyer.client->Send(byName);
    const auto named = Next<proto::NetworkPage>(buyer);
    REQUIRE(named.rows.size() == 1);
    CHECK(named.rows[0].item.item == kSword);
    std::this_thread::sleep_for(3100ms);
    proto::NetworkSearch magic{kSwordGroup, 0, proto::NetworkGrade::Magic};
    buyer.client->Send(magic);
    CHECK(Next<proto::NetworkPage>(buyer).total == 0); // a plain sword: General
    std::this_thread::sleep_for(3100ms);
    proto::NetworkSearch general{kSwordGroup, 0, proto::NetworkGrade::General};
    buyer.client->Send(general);
    CHECK(Next<proto::NetworkPage>(buyer).total == 1);
    std::this_thread::sleep_for(3100ms);
    buyer.client->Send(proto::NetworkSearch{});
    CHECK(Next<proto::NetworkPage>(buyer).code == Code::NetworkFailed); // neither a type nor a name
}

TEST_CASE("Game server: a stall decoration used becomes the player's booth, kept, seen by others") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    StallServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto o = s.Make("Decorator", 1000, {Stored(kBooth), Stored(kPotion, 2)}, 0);
    auto v = s.Make("Onlooker", 1050, {}, 0);
    o.client->Send(proto::UseItem{PlaceOf(o.bag, kBooth)});
    const auto skin = [&] { return t.Db().Query("SELECT coalesce(param1, 0) FROM _staticavatar WHERE charid = $1", {o.character}).Int(0, 0); };
    // Used up: the bag no longer holds it (the bag's save and the skin's are separate writes, either first).
    const auto held = [&] {
        return t.Db().Query("SELECT count(*) FROM _inventory i JOIN _items it ON it.id64 = i.itemid WHERE i.charid = $1 AND it.refitemid = $2",
                            {o.character, std::int64_t(kBooth)}).Int(0, 0);
    };
    for (int i = 0; i < 200 && (skin() == 0 || held() != 0); ++i) s.game.Poll(5ms);
    CHECK(skin() == kBooth);
    CHECK(held() == 0);
    o.client->Send(proto::StallOpen{"Decorated"});
    Next<proto::StallView>(o);
    proto::EntityStall seen;
    do {
        seen = Next<proto::EntityStall>(v);
    } while (seen.entity != o.entity);
    CHECK(seen.name == "Decorated");
    CHECK(seen.skin == kBooth);
}
