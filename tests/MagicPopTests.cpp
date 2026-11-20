// Magic POP: the roll at the original's odds, the consolation weights, the options string,
// the coupon's params; through the game server: a card played at the machine becomes the WIN or LOSE coupon in place,
// the refusals in the original's order, Gori's exchange of every coupon (set SR_DB_URL for those; otherwise skipped).

#include <doctest/doctest.h>

#include <cmath>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "ServerTestSupport.h"
#include "sr/assets/Gacha.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/MagicPop.h"
#include "sr/game/MallProtocol.h"
#include "sr/server/GameServer.h"
#include "sr/server/MagicPop.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;
namespace pop = sr::game::pop;

namespace {

constexpr std::uint32_t kCard = 9238, kWin = 9239, kLose = 9240, kSword = 920001, kPotion = 920002, kStone = 920003;
constexpr std::uint32_t kMachine = 9251, kGori = 9252, kOther = 9300;

std::shared_ptr<assets::ItemTable> Items() {
    auto items = std::make_shared<assets::ItemTable>();
    const auto add = [&](std::uint32_t id, const char* code, std::array<int, 4> tid, int stack = 1) {
        assets::ItemRow r;
        r.refObjId = id;
        r.codename = code;
        r.typeId = tid;
        r.inService = true;
        r.maxStack = stack;
        r.itemClass = 1;
        return r;
    };
    assets::ItemRow card = add(kCard, "ITEM_MALL_GACHA_CARD", {3, 3, 14, 1});
    card.paramTexts[0] = "ITEM_MALL_GACHA_CARD_LOSE";
    card.paramTexts[1] = "ITEM_MALL_GACHA_CARD_WIN";
    items->Add(card);
    assets::ItemRow win = add(kWin, "ITEM_MALL_GACHA_CARD_WIN", {3, 3, 14, 2});
    win.params[0] = 1;
    items->Add(win);
    items->Add(add(kLose, "ITEM_MALL_GACHA_CARD_LOSE", {3, 3, 14, 2}));
    items->Add(add(kSword, "ITEM_CH_SWORD_01_A", {3, 1, 6, 2}));
    items->Add(add(kPotion, "ITEM_ETC_HP_POTION_05", {3, 3, 1, 1}, 50));
    items->Add(add(kStone, "ITEM_ETC_ARCHEMY_MAGICSTONE_SOLID_01", {3, 3, 11, 7}, 50));
    return items;
}

// Set 1: the sword at 3,000 (with STR +3, INT +3, +3), the stone always; set 2: potions x 50 (weight 7,000) and the
// stone (3,000).
std::shared_ptr<assets::GachaTable> Table(bool visible = false) {
    auto t = std::make_shared<assets::GachaTable>();
    t->Add(assets::GachaEntry{1, kSword, 3000, 1, 1, visible, "[BIIV]<M:str,1,3><M:int,1,3><O:3>"});
    t->Add(assets::GachaEntry{1, kStone, 10000, 1, 2, visible, ""});
    t->Add(assets::GachaEntry{2, kPotion, 7000, 50, 10, false, ""});
    t->Add(assets::GachaEntry{2, kStone, 3000, 1, 11, false, ""});
    t->Add(assets::GachaMachine{kMachine, 1, 2});
    return t;
}

std::shared_ptr<std::vector<assets::MagicOptionRow>> Options() {
    auto o = std::make_shared<std::vector<assets::MagicOptionRow>>();
    assets::MagicOptionRow str;
    str.paramId = 11;
    str.optionName = "MATTR_STR";
    str.degree = 1;
    str.attribute = 0x737472;
    o->push_back(str);
    assets::MagicOptionRow in = str;
    in.paramId = 5;
    in.optionName = "MATTR_INT";
    in.attribute = 0x696E74;
    o->push_back(in);
    return o;
}

struct PopServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    explicit PopServer(TestDatabase& db) : t(db) {
        config.listen = Local();
        config.databaseUrl = t.Url();
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
        rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
        rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
        rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
        rules->items = Items();
        config.rules = rules;
        config.gacha = Table();
        config.magicOptions = Options();
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        config.databaseThreads = 2;
        const std::uint16_t region = assets::RegionId(168, 97);
        config.npcs.push_back({kMachine, "NPC_CH_GACHA_MACHINE", {region, 1050.0f, 0.0f, 1000.0f}, 0.0f});
        config.npcs.push_back({kGori, "NPC_CH_GACHA_OPERATOR", {region, 1080.0f, 0.0f, 1000.0f}, 0.0f});
        config.npcs.push_back({kOther, "NPC_CH_POTION", {region, 1100.0f, 0.0f, 1000.0f}, 0.0f});
        config.npcs.push_back({kMachine, "NPC_CH_GACHA_MACHINE", {region, 1700.0f, 0.0f, 1000.0f}, 0.0f}); // out of reach
    }
    struct Player {
        std::unique_ptr<Client> client;
        std::int64_t character = 0;
        std::uint32_t entity = 0;
        proto::InventoryList bag;
        std::uint32_t machine = 0, gori = 0, other = 0, far = 0;
    };
    Player Make(const std::string& name, const std::vector<server::StoredItem>& items) {
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
        p.character = server::ListCharacters(t.Db(), account, config.id, 0, ok)[0].id;
        server::CharacterRow::Place here;
        here.region = assets::RegionId(168, 97);
        here.x = 1000;
        here.z = 1000;
        REQUIRE(server::SavePlaces(t.Db(), {{p.character, here}}));
        REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {account}).Ok());
        if (!items.empty()) REQUIRE(server::GiveItems(t.Db(), config.id, name, items, 0, error) == server::GiveResult::Done);
        REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, $2)", {account, int(config.id)}).Ok());
        p.client->Send(proto::CharacterSelect{name});
        REQUIRE(p.client->Expect(r));
        p.client->Send(proto::WorldEnter{});
        proto::WorldEntered w;
        REQUIRE(p.client->Expect(w));
        REQUIRE(w.status == proto::WorldStatus::Ok);
        p.entity = w.entity;
        REQUIRE(p.client->Expect(p.bag));
        while (p.machine == 0 || p.gori == 0 || p.other == 0 || p.far == 0) {
            proto::EntityAppeared seen;
            REQUIRE(p.client->Expect(seen));
            if (seen.character.model == kMachine && seen.position.x < 1200) p.machine = seen.entity;
            if (seen.character.model == kMachine && seen.position.x > 1200) p.far = seen.entity;
            if (seen.character.model == kGori) p.gori = seen.entity;
            if (seen.character.model == kOther) p.other = seen.entity;
        }
        return p;
    }
    std::uint32_t RefAt(const Player& p, int slot) {
        const server::QueryResult q = t.Db().Query("SELECT i.refitemid FROM _inventory v JOIN _items i ON i.id64 = v.itemid WHERE v.charid = $1 AND v.slot = $2",
                                                   {p.character, slot});
        return q.Rows() > 0 ? std::uint32_t(q.Int(0, 0)) : 0;
    }
};

server::StoredItem Stored(std::uint32_t refId, std::uint16_t count = 1) {
    server::StoredItem s;
    s.refId = refId;
    s.quantity = count;
    s.counted = true;
    return s;
}

template <typename T>
T Next(PopServer::Player& p) {
    T m;
    REQUIRE(p.client->Expect(m));
    return m;
}

} // namespace

TEST_CASE("Magic POP: the roll at the original's odds: 3,000 wins 35.9 %, 10,000 always; the consolation by set 2's weights") {
    const auto table = Table();
    const pop::WasteSet waste = pop::MakeWasteSet(*table, 2);
    REQUIRE(waste.sums == std::vector<int>{7000, 10000});
    const assets::GachaEntry& sword = *table->Find(1);
    const assets::GachaEntry& stone = *table->Find(2);
    CHECK(std::abs(pop::TrueChance(3000) - 0.359) < 0.001);
    CHECK(std::abs(pop::TrueChance(5) - 0.00061) < 0.00001);
    CHECK(pop::TrueChance(10000) == 1.0);
    std::mt19937 random(12345);
    std::uniform_int_distribution<std::uint32_t> draw(0, pop::kDrawRange - 1);
    int won = 0, always = 0, potions = 0;
    constexpr int kRolls = 100000;
    for (int k = 0; k < kRolls; ++k) {
        const pop::Roll r = pop::RollPrize(sword, waste, draw(random), draw(random));
        REQUIRE(r.prize != nullptr);
        if (r.won) {
            ++won;
            CHECK(r.prize->item == kSword);
        } else if (r.prize->item == kPotion) {
            ++potions;
        }
        always += pop::RollPrize(stone, waste, draw(random), draw(random)).won;
    }
    CHECK(std::abs(double(won) / kRolls - pop::TrueChance(3000)) < 0.005);
    CHECK(always == kRolls);
    // The losses' prizes by weight: 7,000 of 10,000 (the original's draw favours the low running sums a little).
    const double lostPotions = double(potions) / double(kRolls - won);
    const double expected = (4.0 * 2768 + 3.0 * (7000 - 2768)) / 32768.0;
    CHECK(std::abs(lostPotions - expected) < 0.01);
    // The draws themselves: r1 below the Ratio wins; r2 by the lower bound of the running sums.
    CHECK(pop::RollPrize(sword, waste, 2999, 0).won);
    CHECK_FALSE(pop::RollPrize(sword, waste, 3000, 0).won);
    CHECK(pop::RollPrize(sword, waste, 20000 + 2999, 0).won); // mod 10,000
    CHECK(pop::RollPrize(sword, waste, 3000, 7000).prize->item == kPotion);
    CHECK(pop::RollPrize(sword, waste, 3000, 7001).prize->item == kStone);
    CHECK(pop::RollPrize(sword, waste, 3000, 0).prize->item == kPotion);
}

TEST_CASE("Magic POP: the options string, the coupon's params and the item a coupon becomes") {
    std::vector<pop::Token> tokens;
    REQUIRE(pop::ParseOptions("[BIIV]<M:str,1,3><M:int,1,3><O:3>", tokens));
    REQUIRE(tokens.size() == 3);
    CHECK(tokens[0].kind == 'M');
    CHECK(tokens[0].attribute == "str");
    CHECK(tokens[0].level == 1);
    CHECK(tokens[0].value == 3);
    CHECK(tokens[2].kind == 'O');
    CHECK_FALSE(pop::ParseOptions("<O:3>", tokens));                        // no marker
    CHECK_FALSE(pop::ParseOptions("[BIIV]<O:3><O:4>", tokens));             // a kind twice
    CHECK_FALSE(pop::ParseOptions("[BIIV]<O:1><D:2><V:3><M:a,1,1><M:b,1,1><M:c,1,1>", tokens)); // six
    CHECK(pop::AttributeCode("int") == 0x696E74);
    const auto table = Table();
    const auto params = pop::CouponParams(*table->Find(1));
    REQUIRE(params.size() == 8);
    CHECK(params[0] == kSword);
    CHECK(params[1] == 1);
    CHECK((params[2] & 0xFF) == 1);
    CHECK(((params[2] >> 8) & 0xFF) == 'M');
    CHECK((params[2] >> 24) == 0x737472);
    CHECK(params[3] == 3);
    // Through an item's options and back.
    game::ItemInstance coupon;
    pop::SetParams(coupon, params);
    const auto prize = pop::ReadCoupon(pop::ParamsOf(coupon));
    REQUIRE(prize);
    CHECK(prize->item == kSword);
    REQUIRE(prize->tokens.size() == 3);
    CHECK(prize->tokens[1].attribute == "int");
    const auto items = Items();
    game::ItemInstance fresh;
    fresh.refId = kSword;
    fresh.durability = 40;
    fresh.variance = 12345;
    const game::ItemInstance sword = pop::PrizeItem(*prize, *items->Find(kSword), fresh, *Options());
    CHECK(sword.plus == 3);
    CHECK(sword.durability == 40);
    CHECK(sword.variance == 12345);
    REQUIRE(sword.options.size() == 2);
    CHECK(sword.options[0] == game::ItemInstance::Option{11, 3});
    CHECK(sword.options[1] == game::ItemInstance::Option{5, 3});
    // An expendable takes the count.
    pop::CouponPrize potions{kPotion, 50, {}};
    game::ItemInstance p;
    CHECK(pop::PrizeItem(potions, *items->Find(kPotion), p, {}).count == 50);
}

TEST_CASE("Magic POP: a card played becomes the WIN or LOSE coupon in place; the refusals in the original's order; Gori exchanges every coupon") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    PopServer s(t);
    s.config.popDraw = 100; // r1 = 100: wins a Ratio above it; lost, r2 = 100 picks the potions
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Player", {Stored(kCard), Stored(kCard), Stored(kPotion, 5), Stored(kCard)});
    // The refusals, in order.
    const auto play = [&](std::uint32_t npc, std::uint32_t entry, std::uint8_t place) {
        p.client->Send(proto::PopPlay{npc, entry, place});
        return Next<proto::PopPlayed>(p);
    };
    CHECK(play(999999, 1, 13).code == 0);       // no such NPC
    CHECK(play(p.far, 1, 13).code == 3);        // out of reach
    CHECK(play(p.other, 1, 13).code == 0x1C0F); // not the machine
    CHECK(play(p.machine, 1, 200).code == 0x1806);
    CHECK(play(p.machine, 1, 40).code == 0x1809); // nothing there
    CHECK(play(p.machine, 1, 15).code == 0x1806); // not a card (the potions)
    CHECK(play(p.machine, 10, 13).code == 2);     // an entry of the waste set
    CHECK(play(p.machine, 77, 13).code == 2);     // no such entry
    // Won: the card at 13 is the WIN coupon carrying the sword and its options.
    auto won = play(p.machine, 1, 13);
    REQUIRE(won.code == 0);
    CHECK(won.won == 1);
    CHECK(won.prize == kSword);
    REQUIRE(won.item.size() == 1);
    CHECK(won.item[0].slot == 13);
    CHECK(won.item[0].item == kWin);
    CHECK(won.item[0].magic.size() == 8);
    CHECK(s.RefAt(p, 13) == kWin);
    // Exchange at the machine: not Gori.
    p.client->Send(proto::PopExchange{p.machine});
    CHECK(Next<proto::PopExchanged>(p).code == 0x1C0F);
    // At Gori: every coupon becomes its prize in place; the cards stay cards.
    p.client->Send(proto::PopExchange{p.gori});
    const auto changed = Next<proto::PopExchanged>(p);
    REQUIRE(changed.code == 0);
    REQUIRE(changed.items.size() == 1);
    CHECK(changed.items[0].slot == 13);
    CHECK(changed.items[0].item == kSword);
    CHECK(changed.items[0].plus == 3);
    CHECK(changed.items[0].magic.size() == 2);
    CHECK(s.RefAt(p, 13) == kSword);
    CHECK(s.RefAt(p, 14) == kCard);
    // Nothing more to exchange: done, nothing changed.
    p.client->Send(proto::PopExchange{p.gori});
    CHECK(Next<proto::PopExchanged>(p).items.empty());
}

TEST_CASE("Magic POP: a loss gives the LOSE coupon with the consolation prize; its exchange gives the potions as one stack of 50") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    PopServer s(t);
    s.config.popDraw = 5000; // r1 5,000: the sword lost; r2 5,000: the potions (the running sums 7,000, 10,000)
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Loser", {Stored(kCard)});
    p.client->Send(proto::PopPlay{p.machine, 1, 13});
    const auto lost = Next<proto::PopPlayed>(p);
    REQUIRE(lost.code == 0);
    CHECK(lost.won == 0);
    CHECK(lost.prize == kPotion);
    CHECK(lost.count == 50);
    CHECK(lost.item.at(0).item == kLose);
    CHECK(s.RefAt(p, 13) == kLose);
    p.client->Send(proto::PopExchange{p.gori});
    const auto got = Next<proto::PopExchanged>(p);
    REQUIRE(got.items.size() == 1);
    CHECK(got.items[0].item == kPotion);
    CHECK(got.items[0].quantity == 50);
}
