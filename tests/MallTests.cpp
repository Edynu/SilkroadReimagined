// The item mall and silk: silk kept per account (the starting 10,000, a grant's logs and
// line, the migration given once), the purchase through the game server in one transaction (the bag's places, the
// silk refused, the stock, a failure leaving everything as it was), the goods bought together, the history; and the
// rules both sides share (set SR_DB_URL for the database's; otherwise skipped).

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ServerTestSupport.h"
#include "sr/assets/GameData.h"
#include "sr/assets/Shop.h"
#include "sr/core/Jobs.h"
#include "sr/game/Mall.h"
#include "sr/game/MallProtocol.h"
#include "sr/server/GameServer.h"
#include "sr/server/Mall.h"
#include "sr/server/SilkStore.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;
namespace code = proto::mallcode;

namespace {

constexpr std::uint32_t kTicket = 910001, kHat = 910002, kDress = 910003, kAttach = 910004, kQuiver = 910005, kBag = 910006, kStone = 910007;
constexpr std::uint32_t kTabSpecial = 501, kTabAvatar = 502;

assets::ItemRow Row(std::uint32_t id, const char* codename, std::array<int, 4> tid, int maxStack = 1) {
    assets::ItemRow r;
    r.refObjId = id;
    r.codename = codename;
    r.typeId = tid;
    r.inService = true;
    r.maxStack = maxStack;
    r.cashItem = true;
    return r;
}

std::shared_ptr<assets::ItemTable> Items() {
    auto items = std::make_shared<assets::ItemTable>();
    items->Add(Row(kTicket, "ITEM_MALL_GOLD_TIME_SERVICE_TICKET_4W", {3, 3, 13, 4}));
    items->Add(Row(kHat, "ITEM_MALL_AVATAR_W_KNIGHT_HAT", {3, 1, 13, 1}));
    items->Add(Row(kDress, "ITEM_MALL_AVATAR_W_KNIGHT", {3, 1, 13, 2}));
    items->Add(Row(kAttach, "ITEM_MALL_AVATAR_W_ATTACH", {3, 1, 13, 3}));
    items->Add(Row(kQuiver, "ITEM_MALL_QUIVER", {3, 3, 4, 1}, 10000));
    items->Add(Row(kBag, "ITEM_MALL_HP_SUPERSET_2_BAG", {3, 3, 1, 1}, 1000));
    items->Add(Row(kStone, "ITEM_ETC_ARCHEMY_MAGICSTONE_ASTRAL_01", {3, 3, 11, 7}, 50));
    return items;
}

assets::MallGood Good(int slot, std::uint32_t package, const char* code, const char* item, std::uint32_t price, int data = 0) {
    assets::MallGood g;
    g.slot = slot;
    g.packageId = package;
    g.package = code;
    g.itemCodename = item;
    g.price = price;
    g.data = data;
    return g;
}

std::shared_ptr<assets::MallTable> Catalog() {
    auto mall = std::make_shared<assets::MallTable>();
    assets::MallShop consume;
    consume.code = "MALL_CONSUME";
    assets::MallTab special;
    special.id = kTabSpecial;
    special.code = "MALL_CONSUME_SPECIAL";
    special.goods = {Good(0, 7001, "PACKAGE_ITEM_MALL_GOLD_TIME_SERVICE_TICKET_4W", "ITEM_MALL_GOLD_TIME_SERVICE_TICKET_4W", 130),
                     Good(1, 7002, "PACKAGE_ITEM_MALL_QUIVER", "ITEM_MALL_QUIVER", 8, 10000),
                     Good(2, 7003, "PACKAGE_ITEM_MALL_HP_SUPERSET_2_BAG", "ITEM_MALL_HP_SUPERSET_2_BAG", 5, 1000),
                     Good(3, 7004, "PACKAGE_ITEM_ETC_ARCHEMY_MAGICSTONE_ASTRAL_01", "ITEM_ETC_ARCHEMY_MAGICSTONE_ASTRAL_01", 2)};
    consume.tabs.push_back(special);
    mall->Add(consume);
    assets::MallShop avatar;
    avatar.code = "MALL_AVATAR";
    assets::MallTab hats;
    hats.id = kTabAvatar;
    hats.code = "MALL_AVATAR_HAT";
    hats.goods = {Good(0, 7101, "PACKAGE_ITEM_MALL_AVATAR_W_KNIGHT_HAT", "ITEM_MALL_AVATAR_W_KNIGHT_HAT", 160),
                  Good(1, 7102, "PACKAGE_ITEM_MALL_AVATAR_W_KNIGHT", "ITEM_MALL_AVATAR_W_KNIGHT", 398),
                  Good(2, 7103, "PACKAGE_ITEM_MALL_AVATAR_W_ATTACH", "ITEM_MALL_AVATAR_W_ATTACH", 40)};
    avatar.tabs.push_back(hats);
    mall->Add(avatar);
    return mall;
}

struct MallServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    explicit MallServer(TestDatabase& db) : t(db) {
        config.listen = Local();
        config.databaseUrl = t.Url();
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
        rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
        rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
        rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
        rules->items = Items();
        config.rules = rules;
        config.mall = Catalog();
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        config.databaseThreads = 2;
    }
    struct Player {
        std::unique_ptr<Client> client;
        std::int64_t account = 0, character = 0;
        std::uint32_t entity = 0;
        proto::InventoryList bag;
        proto::SilkBalanceMessage silk;
    };
    // A character with `silk` (own) and `bagFill` items of stone filling its bag from place 13, entered.
    Player Make(const std::string& name, std::uint32_t silk, int bagFill = 0) {
        Player p;
        REQUIRE(server::CreateAccount(t.Db(), name + "acct", "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        p.account = server::FindAccount(t.Db(), name + "acct", ok)->id;
        // Every account starts with 10,000: set what the test wants.
        REQUIRE(t.Db().Query("UPDATE sro_vt_account.sk_silk SET silk_own = $2 WHERE jid = $1", {p.account, std::int64_t(silk)}).Affected() == 1);
        p.client = std::make_unique<Client>(net, game.Port(), [this] { game.Poll(2ms); });
        REQUIRE(p.client->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), p.account, config.id, 30, error);
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
        p.character = server::ListCharacters(t.Db(), p.account, config.id, 0, ok)[0].id;
        if (bagFill > 0) {
            std::vector<server::StoredItem> items;
            for (int k = 0; k < bagFill; ++k) {
                server::StoredItem s;
                s.refId = kStone;
                s.quantity = 1;
                s.counted = true;
                items.push_back(s);
            }
            REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {p.account}).Ok());
            REQUIRE(server::GiveItems(t.Db(), config.id, name, items, 0, error) == server::GiveResult::Done);
            REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, $2)", {p.account, int(config.id)}).Ok());
        }
        p.client->Send(proto::CharacterSelect{name});
        REQUIRE(p.client->Expect(r));
        p.client->Send(proto::WorldEnter{});
        proto::WorldEntered w;
        REQUIRE(p.client->Expect(w));
        REQUIRE(w.status == proto::WorldStatus::Ok);
        p.entity = w.entity;
        REQUIRE(p.client->Expect(p.bag));
        REQUIRE(p.client->Expect(p.silk));
        return p;
    }
    std::int64_t Silk(const Player& p) {
        return t.Db().Query("SELECT silk_own FROM sro_vt_account.sk_silk WHERE jid = $1", {p.account}).Int(0, 0);
    }
    int ItemsOf(const Player& p, std::uint32_t refId) {
        return int(t.Db()
                       .Query("SELECT count(*) FROM _inventory v JOIN _items i ON i.id64 = v.itemid WHERE v.charid = $1 AND i.refitemid = $2",
                              {p.character, std::int64_t(refId)})
                       .Int(0, 0));
    }
};

template <typename T>
T Next(MallServer::Player& p) {
    T m;
    REQUIRE(p.client->Expect(m));
    return m;
}

proto::MallBought Buy(MallServer::Player& p, std::uint32_t tab, std::uint8_t slot, std::uint32_t package, std::uint16_t count = 1) {
    p.client->Send(proto::MallBuy{tab, slot, package, count, 0});
    return Next<proto::MallBought>(p);
}

} // namespace

TEST_CASE("Mall: the rules: the count, the places a purchase takes, the price, the items it makes") {
    const auto items = Items();
    const auto mall = Catalog();
    const assets::MallGood* quiver = mall->Find(kTabSpecial, 1);
    const assets::MallGood* bag = mall->Find(kTabSpecial, 2);
    const assets::MallGood* stone = mall->Find(kTabSpecial, 3);
    const assets::MallGood* hat = mall->Find(kTabAvatar, 0);
    REQUIRE(quiver != nullptr);
    REQUIRE(hat != nullptr);
    // Arrows x 10,000: the package's own stack, one per count (the count up to 5).
    CHECK(game::MallPlacesNeeded(*quiver, *items->Find(kQuiver), 1) == 1);
    const auto arrows = game::MallItems(*quiver, *items->Find(kQuiver), 1, 0);
    REQUIRE(arrows.size() == 1);
    CHECK(arrows[0].count == 10000);
    CHECK(game::MaxStack(*items->Find(kQuiver)) == 10000);
    // A hat x 7 is clamped to 5 and takes five places.
    CHECK(game::MallClampCount(*hat, *items->Find(kHat), 7) == 5);
    CHECK(game::MallPlacesNeeded(*hat, *items->Find(kHat), 7) == 5);
    CHECK(game::MallPlacesNeeded(*hat, *items->Find(kHat), 3) == 3);
    // A stackable with Data 0: the count chosen in one place, up to its stack.
    CHECK(game::MallCountIsStack(*stone, *items->Find(kStone)));
    CHECK(game::MallMostCount(*stone, *items->Find(kStone)) == 50);
    CHECK(game::MallPlacesNeeded(*stone, *items->Find(kStone), 30) == 1);
    CHECK(game::MallItems(*stone, *items->Find(kStone), 30, 0)[0].count == 30);
    // The superset bag: 1,000 a package.
    CHECK(game::MallItems(*bag, *items->Find(kBag), 2, 0).size() == 2);
    CHECK(game::MallItems(*bag, *items->Find(kBag), 2, 0)[1].count == 1000);
    CHECK(game::MallPrice(*hat, 3) == 480);
    CHECK(proto::PackSilkDate(2026, 11, 14, 13, 5) == (26u | 11u << 6 | 14u << 10 | 13u << 15 | 5u << 20));
}

TEST_CASE("Silk: every new account has 10,000; a grant writes the row, its logs and the bus; an order counts once; the history") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    REQUIRE(server::CreateAccount(t.Db(), "silky", "secret1", error) == server::CreateAccountResult::Created);
    bool ok = false;
    const std::int64_t jid = server::FindAccount(t.Db(), "silky", ok)->id;
    server::SilkBalance b;
    REQUIRE(server::LoadSilk(t.Db(), jid, b, error));
    CHECK(b == server::SilkBalance{10000, 0, 0});
    // An account without a row reads 0 / 0 / 0.
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sk_silk WHERE jid = $1", {jid}).Ok());
    REQUIRE(server::LoadSilk(t.Db(), jid, b, error));
    CHECK(b == server::SilkBalance{});
    // A grant (made from outside: the bus says so).
    auto listener = server::Database::Connect(t.Url(), error);
    REQUIRE(listener);
    REQUIRE(listener->Listen(server::SilkChannel()));
    server::SilkBalance after;
    CHECK(server::GrantSilk(t.Db(), jid, 100, server::SilkKind::Own, server::SilkReason::GiftReceived, "ORDER-1", "a test", after, error) ==
          server::SilkGrant::Done);
    CHECK(after == server::SilkBalance{100, 0, 0});
    CHECK(server::GrantSilk(t.Db(), jid, 100, server::SilkKind::Own, server::SilkReason::GiftReceived, "ORDER-1", "again", after, error) ==
          server::SilkGrant::Repeated);
    CHECK(server::GrantSilk(t.Db(), jid, -101, server::SilkKind::Own, server::SilkReason::GiftReceived, "", "too much", after, error) ==
          server::SilkGrant::Short);
    CHECK(server::GrantSilk(t.Db(), 999999, 5, server::SilkKind::Own, server::SilkReason::GiftReceived, "", "nobody", after, error) ==
          server::SilkGrant::NoAccount);
    std::vector<server::Database::Notification> heard;
    for (int k = 0; k < 100 && heard.empty(); ++k) {
        heard = listener->TakeNotifications();
        if (heard.empty()) std::this_thread::sleep_for(10ms);
    }
    REQUIRE(heard.size() == 1);
    const auto notice = server::DecodeSilkNotice(heard[0].payload);
    REQUIRE(notice);
    CHECK(notice->account == jid);
    CHECK(notice->reason == server::SilkReason::GiftReceived);
    CHECK(notice->value == 100);
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_account.sk_silkchange_by_web WHERE jid = $1", {jid}).Int(0, 0) == 2);
    // The history: the bought list holds the starting silk (type 0, reason 0); the grant was a gift's reason (2).
    std::vector<server::SilkHistoryRow> rows;
    REQUIRE(server::LoadSilkHistory(t.Db(), jid, 1, 1, rows, error));
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].silk == 10000);
    REQUIRE(t.Db().Query("UPDATE sro_vt_account.sk_silkbuylist SET regdate = localtimestamp - interval '3 days' WHERE userjid = $1", {jid}).Ok());
    REQUIRE(server::LoadSilkHistory(t.Db(), jid, 1, 1, rows, error));
    CHECK(rows.empty()); // not within one day any more
    REQUIRE(server::LoadSilkHistory(t.Db(), jid, 1, 2, rows, error));
    CHECK(rows.size() == 1); // within the week
}

TEST_CASE("Silk: the migration gives every account already there 10,000, once") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    // Accounts made before silk came (as an imported database's): no silk rows, migration 9 not applied yet.
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.tb_user (struserid, password) VALUES ('old1', ''), ('old2', '')").Ok());
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_schema_migrations WHERE version = 9").Ok());
    REQUIRE(t.Db().Query("UPDATE sro_vt_account.sk_silk SET silk_own = 250").Ok()); // nobody has a row yet
    REQUIRE(server::MigrateSchema(t.Db(), error));
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_account.sk_silk WHERE silk_own = 10000").Int(0, 0) == 2);
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_account.sk_silkbuylist WHERE ordernumber LIKE 'START-%'").Int(0, 0) == 2);
    // Applied once: migrating again gives nothing more.
    REQUIRE(server::MigrateSchema(t.Db(), error));
    CHECK(t.Db().Query("SELECT sum(silk_own) FROM sro_vt_account.sk_silk").Int(0, 0) == 20000);
}

TEST_CASE("Silk: 1,000 debits of 1 from three connections on a balance of 500 end at 0, 500 refused") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    REQUIRE(server::CreateAccount(t.Db(), "racer", "secret1", error) == server::CreateAccountResult::Created);
    bool ok = false;
    const std::int64_t jid = server::FindAccount(t.Db(), "racer", ok)->id;
    REQUIRE(t.Db().Query("UPDATE sro_vt_account.sk_silk SET silk_own = 500 WHERE jid = $1", {jid}).Ok());
    server::DatabasePool pool;
    REQUIRE(pool.Start(t.Url(), 3, error));
    std::atomic<int> done{0}, refused{0}, taken{0};
    for (int k = 0; k < 1000; ++k) {
        pool.Submit([&](server::Database& db) {
            std::string e;
            server::SilkBalance after, paid;
            bool spent = false;
            db.Transaction([&](server::Database& tx) { return spent = server::SpendSilkIn(tx, jid, 1, 0, after, paid, e); }, e);
            (spent ? taken : refused).fetch_add(1);
            done.fetch_add(1);
        });
    }
    while (done.load() < 1000) std::this_thread::sleep_for(5ms);
    pool.Stop();
    CHECK(taken.load() == 500);
    CHECK(refused.load() == 500);
    CHECK(t.Db().Query("SELECT silk_own FROM sro_vt_account.sk_silk WHERE jid = $1", {jid}).Int(0, 0) == 0);
}

TEST_CASE("Mall: a purchase: the gold ticket's 130 of 200 into the first free place; told; the logs") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    MallServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Buyer", 200, 2);
    CHECK(p.silk.own == 200);
    const auto bought = Buy(p, kTabSpecial, 0, 7001);
    REQUIRE(bought.code == code::kOk);
    REQUIRE(bought.items.size() == 1);
    CHECK(bought.items[0].item == kTicket);
    CHECK(bought.items[0].slot == 15); // 13 and 14 hold the stones
    CHECK(Next<proto::SilkBalanceMessage>(p).own == 70);
    CHECK(s.Silk(p) == 70);
    CHECK(s.ItemsOf(p, kTicket) == 1);
    CHECK(t.Db().Query("SELECT silk_own FROM sro_vt_account.sk_packageitemsalelog WHERE jid = $1 AND packageitemid = 7001", {p.account}).Int(0, 0) == 130);
    // The bag after a relog holds it where it was told.
    // The package must be the good's (0x18AB); no such tab (0x1806), no good at the slot (0x1809).
    CHECK(Buy(p, kTabSpecial, 0, 7002).code == code::kWrongGood);
    CHECK(Buy(p, 999, 0, 7001).code == code::kNoTab);
    CHECK(Buy(p, kTabSpecial, 9, 7001).code == code::kNoGood);
    // 70 silk refuses a 130 good (0x1880) and nothing is taken.
    CHECK(Buy(p, kTabSpecial, 0, 7001).code == code::kNoSilk);
    CHECK(s.Silk(p) == 70);
    CHECK(s.ItemsOf(p, kTicket) == 1);
}

TEST_CASE("Mall: refusals: a full bag before the silk; 129 silk for a 130 good; arrows in one place, hats one each; the stock") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    MallServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    // A bag of 45 (32 free places) with 32 stones: full.
    auto full = s.Make("Full", 1000, 32);
    CHECK(Buy(full, kTabSpecial, 0, 7001).code == code::kBagFull);
    CHECK(s.Silk(full) == 1000);
    auto poor = s.Make("Poor", 129);
    CHECK(Buy(poor, kTabSpecial, 0, 7001).code == code::kNoSilk);
    CHECK(s.Silk(poor) == 129);
    // Arrows x 10,000: one place; hats x 3: three places, 480 silk.
    auto rich = s.Make("Rich", 5000, 0);
    auto arrows = Buy(rich, kTabSpecial, 1, 7002);
    REQUIRE(arrows.code == code::kOk);
    REQUIRE(arrows.items.size() == 1);
    CHECK(arrows.items[0].quantity == 10000);
    CHECK(Next<proto::SilkBalanceMessage>(rich).own == 4992);
    auto hats = Buy(rich, kTabAvatar, 0, 7101, 3);
    REQUIRE(hats.code == code::kOk);
    CHECK(hats.items.size() == 3);
    CHECK(Next<proto::SilkBalanceMessage>(rich).own == 4992 - 480);
    // Seven asked: five bought.
    auto five = Buy(rich, kTabAvatar, 0, 7101, 7);
    REQUIRE(five.code == code::kOk);
    CHECK(five.items.size() == 5);
    CHECK(five.goods[0].count == 5);
    Next<proto::SilkBalanceMessage>(rich);
    // A limited good: its stock row at 0 refuses (Already sold out) and takes nothing.
    REQUIRE(t.Db()
                .Query("CREATE TABLE IF NOT EXISTS _shopitemstockquantity (service smallint, country integer, refshopgroupcodename varchar(129), "
                       "refpackageitemcodename varchar(129), conststockquantity smallint, stockquantity smallint)")
                .Ok());
    REQUIRE(t.Db().Query("INSERT INTO _shopitemstockquantity VALUES (1, 15, 'GROUP_MALL', 'PACKAGE_ITEM_MALL_AVATAR_W_ATTACH', 1, 1)").Ok());
    rich.client->Send(proto::MallOpen{});
    const auto state = Next<proto::MallState>(rich);
    REQUIRE(state.stock.size() == 1);
    CHECK(state.stock[0].package == 7103);
    CHECK(state.stock[0].left == 1);
    Next<proto::SilkBalanceMessage>(rich);
    const std::int64_t before = s.Silk(rich);
    REQUIRE(Buy(rich, kTabAvatar, 2, 7103).code == code::kOk);
    Next<proto::SilkBalanceMessage>(rich);
    CHECK(s.Silk(rich) == before - 40);
    CHECK(Buy(rich, kTabAvatar, 2, 7103).code == code::kSoldOut);
    CHECK(s.Silk(rich) == before - 40);
    CHECK(s.ItemsOf(rich, kAttach) == 1);
}

TEST_CASE("Mall: a purchase the database fails leaves the silk and the bag as they were; a set bought together is all or nothing") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    MallServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Careful", 1000, 30); // 2 free places
    s.game.GetMall()->failNextPurchase = true;
    CHECK(Buy(p, kTabAvatar, 0, 7101).code == code::kFailed);
    CHECK(s.Silk(p) == 1000);
    CHECK(s.ItemsOf(p, kHat) == 0);
    // The bag is the same: a second purchase lands in the same place.
    const auto ok = Buy(p, kTabAvatar, 0, 7101);
    REQUIRE(ok.code == code::kOk);
    CHECK(ok.items[0].slot == 43);
    Next<proto::SilkBalanceMessage>(p);
    // Hat + dress + attachment: 598 together; with one free place: nothing.
    proto::MallBuyMany set;
    set.goods = {{kTabAvatar, 0, 7101, 1}, {kTabAvatar, 1, 7102, 1}, {kTabAvatar, 2, 7103, 1}};
    p.client->Send(set);
    CHECK(Next<proto::MallBought>(p).code == code::kBagFull);
    CHECK(s.Silk(p) == 840);
    auto q = s.Make("Dresser", 1000);
    q.client->Send(set);
    const auto all = Next<proto::MallBought>(q);
    REQUIRE(all.code == code::kOk);
    CHECK(all.items.size() == 3);
    CHECK(Next<proto::SilkBalanceMessage>(q).own == 402);
    CHECK(s.Silk(q) == 402);
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_account.sk_packageitemsalelog WHERE jid = $1", {q.account}).Int(0, 0) == 3);
    // Too little for the whole set: nothing of it.
    auto r = s.Make("Short", 597);
    r.client->Send(set);
    CHECK(Next<proto::MallBought>(r).code == code::kNoSilk);
    CHECK(s.Silk(r) == 597);
    CHECK(s.ItemsOf(r, kHat) + s.ItemsOf(r, kDress) + s.ItemsOf(r, kAttach) == 0);
}

TEST_CASE("Mall: the silk history: the starting silk in the bought list, packed dates; nothing in a day after a clock move") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    MallServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Reader", 10000);
    p.client->Send(proto::SilkHistoryQuery{1, 3});
    auto h = Next<proto::SilkHistory>(p);
    CHECK(h.result == 0);
    REQUIRE(h.rows.size() == 1);
    CHECK(h.rows[0].silk == 10000);
    CHECK(((h.rows[0].when >> 6) & 0xF) >= 1); // a month
    REQUIRE(t.Db().Query("UPDATE sro_vt_account.sk_silkbuylist SET regdate = localtimestamp - interval '2 days' WHERE userjid = $1", {p.account}).Ok());
    p.client->Send(proto::SilkHistoryQuery{1, 1});
    CHECK(Next<proto::SilkHistory>(p).rows.empty());
}
