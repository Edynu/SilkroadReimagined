// The item mall's services: wear and its repair (the NPC's price and a partial repair,
// the hammer), the equipment gender switch, the skin change, skill and mastery withdrawal with the skill restoration
// potion, the magic cubes. The rules alone, then through the game server (set SR_DB_URL; otherwise skipped).

#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "ServerTestSupport.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/MallProtocol.h"
#include "sr/game/MallServices.h"
#include "sr/server/GameServer.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;

namespace {

constexpr std::uint32_t kSword = 920001, kChestM = 920002, kChestW = 920003, kChestHigh = 920004, kChestAlone = 920005;
constexpr std::uint32_t kTool = 920010, kHammer = 920011, kScroll = 920012, kPotion = 920013, kCube = 920014, kElixir = 920015, kArmourElixir = 920016;
constexpr std::uint32_t kMastery = 257;
constexpr std::uint32_t kWoman = 101;

assets::ItemRow Row(std::uint32_t id, const char* codename, std::array<int, 4> tid, int maxStack = 1) {
    assets::ItemRow r;
    r.refObjId = id;
    r.codename = codename;
    r.typeId = tid;
    r.inService = true;
    r.maxStack = maxStack;
    r.country = 3;
    return r;
}

assets::ItemRow Equipment(std::uint32_t id, const char* codename, std::array<int, 4> tid, int itemClass, const char* counterpart) {
    assets::ItemRow r = Row(id, codename, tid);
    r.canRepair = true;
    r.costRepair = 1000;
    r.itemClass = itemClass;
    r.linkedCodename = counterpart;
    r.stats = {40.0f, 60.0f}; // Dur_L, Dur_U: the most at a variance of 0 is 40
    r.sex = 2;
    return r;
}

std::shared_ptr<assets::ItemTable> Items() {
    auto items = std::make_shared<assets::ItemTable>();
    items->Add(Equipment(kSword, "ITEM_TEST_SWORD", {3, 1, 6, 2}, 4, "xxx"));
    items->Add(Equipment(kChestM, "ITEM_CH_M_HEAVY_TEST", {3, 1, 1, 3}, 4, "ITEM_CH_W_HEAVY_TEST"));
    items->Add(Equipment(kChestW, "ITEM_CH_W_HEAVY_TEST", {3, 1, 1, 3}, 4, "ITEM_CH_M_HEAVY_TEST"));
    items->Add(Equipment(kChestHigh, "ITEM_CH_M_HEAVY_HIGH", {3, 1, 1, 3}, 31, "ITEM_CH_W_HEAVY_TEST"));
    items->Add(Equipment(kChestAlone, "ITEM_CH_M_HEAVY_ALONE", {3, 1, 1, 3}, 4, "xxx"));
    assets::ItemRow tool = Row(kTool, "ITEM_MALL_EQUIP_TRANSGENDER_1", {3, 3, 13, 8}, 5);
    tool.params[0] = 3;
    items->Add(tool);
    assets::ItemRow hammer = Row(kHammer, "ITEM_MALL_REPAIR_HAMMER", {3, 3, 13, 7}, 11);
    hammer.params[0] = 2;
    items->Add(hammer);
    items->Add(Row(kScroll, "ITEM_MALL_CHAR_SKIN_CHANGE_SCROLL", {3, 3, 13, 9}));
    items->Add(Row(kPotion, "ITEM_MALL_SKILL_RESTORATION_POTION", {3, 3, 13, 0}, 50));
    assets::ItemRow cube = Row(kCube, "ITEM_MALL_MAGIC_CUBE_WEAPON", {3, 2, 3, 1});
    cube.params[8] = 2; // two places
    cube.params[9] = 1; // weapon elixirs
    items->Add(cube);
    assets::ItemRow elixir = Row(kElixir, "ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B", {3, 3, 10, 1}, 100);
    elixir.params[9] = 1;
    items->Add(elixir);
    assets::ItemRow other = elixir;
    other.refObjId = kArmourElixir;
    other.codename = "ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_ARMOR_B";
    other.params[9] = 2;
    items->Add(other);
    return items;
}

assets::SkillRow Skill(std::uint32_t id, std::uint32_t group, int level, int sp, int masteryLevel, std::uint32_t needGroup = 0, int needLevel = 0) {
    assets::SkillRow s;
    s.id = id;
    s.codename = "SKILL_TEST_" + std::to_string(id);
    s.group = group;
    s.level = level;
    s.spCost = sp;
    s.masteries = {kMastery, 0};
    s.masteryLevels = {masteryLevel, 0};
    s.prerequisiteGroups = {needGroup, 0, 0};
    s.prerequisiteLevels = {needLevel, 0, 0};
    s.inService = true;
    return s;
}

std::shared_ptr<assets::SkillTable> Skills() {
    auto t = std::make_shared<assets::SkillTable>();
    t->Add(Skill(5001, 1, 1, 10, 1));
    t->Add(Skill(5002, 1, 2, 20, 2));
    t->Add(Skill(5003, 1, 3, 30, 3));
    t->Add(Skill(5011, 2, 1, 50, 5, 1, 2)); // needs group 1 at level 2 and the mastery at 5
    t->Add(Skill(5021, 3, 1, 15, 1));
    t->Add(Skill(5022, 3, 2, 25, 1));
    return t;
}

} // namespace

TEST_CASE("Mall services: the rules: wear's rate, the repair's price, the gender switch's refusals, withdrawal's SP, the cubes") {
    const auto items = Items();
    // A use's wear: a point of the weapon on 2 % x the use's hits, at most 100 %: one hit 2 %, three 6 %.
    std::mt19937 random(7);
    const auto rate = [&](game::Wear kind, int count, int place) {
        int lost = 0;
        for (int k = 0; k < 100000; ++k) lost += game::RollWear(kind, [](int) { return true; }, count, random) == place;
        return lost;
    };
    CHECK(rate(game::Wear::Weapon, 1, game::slot::kWeapon) > 1800);
    CHECK(rate(game::Wear::Weapon, 1, game::slot::kWeapon) < 2200);
    CHECK(rate(game::Wear::Weapon, 3, game::slot::kWeapon) > 5600);
    CHECK(rate(game::Wear::Weapon, 3, game::slot::kWeapon) < 6400);
    CHECK(rate(game::Wear::Weapon, 60, game::slot::kWeapon) == 100000);
    // Hits taken: the head first, then the chest, ...: the first piece rolls far more often than the last.
    int head = 0, shield = 0;
    for (int k = 0; k < 100000; ++k) {
        const int at = game::RollWear(game::Wear::Armour, [](int) { return true; }, 1, random);
        head += at == game::slot::kHead;
        shield += at == game::slot::kShield;
    }
    CHECK(head > 1800);
    CHECK(shield < head);
    CHECK(game::RollWear(game::Wear::Shield, [](int) { return false; }, 1, random) == -1);
    // The armour pass stops at the first place that rolls, even an empty one (nothing worn there: nothing loses a point).
    int chest = 0;
    for (int k = 0; k < 100000; ++k) chest += game::RollWear(game::Wear::Armour, [](int at) { return at != game::slot::kHead; }, 50, random) == game::slot::kChest;
    CHECK(chest == 0); // at 100 % the empty head always rolls first
    // The price: CostRepair 1,000 over the most 40: 25 a point; 30 missing cost 750; 500 gold buy 20.
    const assets::ItemRow& sword = *items->Find(kSword);
    CHECK(game::RepairPointCost(sword, 40) == 25);
    CHECK(game::QuoteRepair(sword, 10, 40, 10000).points == 30);
    CHECK(game::QuoteRepair(sword, 10, 40, 10000).gold == 750);
    CHECK(game::QuoteRepair(sword, 10, 40, 500).points == 20);
    CHECK(game::QuoteRepair(sword, 10, 40, 500).gold == 500);
    CHECK(game::QuoteRepair(sword, 40, 40, 500).points == 0);
    // A cheap item: at least 1 a point.
    assets::ItemRow cheap = sword;
    cheap.costRepair = 3;
    CHECK(game::RepairPointCost(cheap, 40) == 1);
    CHECK(game::IsRepairNpc("NPC_CH_SMITH"));
    CHECK(game::IsRepairNpc("NPC_EU_ARMOR"));
    CHECK_FALSE(game::IsRepairNpc("NPC_CH_FORTRESS_SMITH1"));
    CHECK_FALSE(game::IsRepairNpc("NPC_CH_POTION"));
    // The gender switch: a worn place 0x1882, a weapon 0x1884, a degree over 3 0x1883, no counterpart 2.
    const assets::ItemRow& tool = *items->Find(kTool);
    CHECK(game::GenderSwitchRefusal(tool, 1, 45, items->Find(kChestM), items->Find(kChestW)) == 0x1882);
    CHECK(game::GenderSwitchRefusal(tool, 13, 45, &sword, nullptr) == 0x1884);
    CHECK(game::GenderSwitchRefusal(tool, 13, 45, items->Find(kChestHigh), items->Find(kChestW)) == 0x1883);
    CHECK(game::GenderSwitchRefusal(tool, 13, 45, items->Find(kChestAlone), nullptr) == 2);
    CHECK(game::GenderSwitchRefusal(tool, 13, 45, items->Find(kChestM), items->Find(kChestW)) == 0);
    // Withdrawal: the mall's potion gives all the SP back, the quest's 80 % and gold.
    const auto skills = Skills();
    game::SkillBook book;
    book.masteries[kMastery] = 5;
    book.groups = {{1, 3}, {2, 1}, {3, 2}};
    const std::vector<std::uint32_t> costs = {0, 100, 200, 300, 400, 500, 600};
    CHECK(game::WithdrawSkill(book, *skills, 5003, 1, game::WithdrawMode::Mall, 0).code == game::withdrawcode::kRequired); // group 2 needs 2
    const game::Withdrawal one = game::WithdrawSkill(book, *skills, 5003, 2, game::WithdrawMode::Mall, 0);
    CHECK(one.code == 0);
    CHECK(one.levels == 1);
    CHECK(one.skillPoints == 30);
    CHECK(one.skillNow == 5002);
    const game::Withdrawal two = game::WithdrawSkill(book, *skills, 5022, 0, game::WithdrawMode::Mall, 0);
    CHECK(two.levels == 2);
    CHECK(two.skillPoints == 40);
    CHECK(two.skillNow == 0);
    const game::Withdrawal quest = game::WithdrawSkill(book, *skills, 5022, 0, game::WithdrawMode::Quest, 95);
    CHECK(quest.skillPoints == 32);
    CHECK(quest.gold == 2u * 95u * 10u * 15u);
    CHECK(game::WithdrawSkill(book, *skills, 5022, 2, game::WithdrawMode::Mall, 0).code == game::withdrawcode::kLevelTooLow);
    CHECK(game::WithdrawSkill(book, *skills, 5011, 0, game::WithdrawMode::Mall, 0).code == 0);
    game::SkillBook none;
    CHECK(game::WithdrawSkill(none, *skills, 5001, 0, game::WithdrawMode::Mall, 0).code == game::withdrawcode::kNotLearned);
    CHECK(game::WithdrawMastery(book, *skills, costs, kMastery, 3, game::WithdrawMode::Mall, 0).code == game::withdrawcode::kRequired);
    book.groups.erase(2);
    const game::Withdrawal mastery = game::WithdrawMastery(book, *skills, costs, kMastery, 3, game::WithdrawMode::Mall, 0);
    CHECK(mastery.code == 0);
    CHECK(mastery.levels == 2);
    CHECK(mastery.skillPoints == 300 + 400);
    // The cubes: weapon elixirs only.
    CHECK(game::CubeTakes(*items->Find(kCube), *items->Find(kElixir)));
    CHECK_FALSE(game::CubeTakes(*items->Find(kCube), *items->Find(kArmourElixir)));
    CHECK(game::CubePlaces(*items->Find(kCube)) == 2);
}

namespace {

struct ServiceServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    explicit ServiceServer(TestDatabase& db) : t(db) {
        config.listen = Local();
        config.databaseUrl = t.Url();
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
        rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
        rules->creation.AddFigure(kWoman, game::CreationRules::Race::China, false);
        rules->creation.AddFigure(200, game::CreationRules::Race::Europe, true);
        rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
        rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
        rules->items = Items();
        config.rules = rules;
        config.skills = Skills();
        config.masteryCosts = {0, 100, 200, 300, 400, 500, 600};
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        config.databaseThreads = 2;
        config.skillSeed = 11;
        GameServerConfig::NpcSpawn smith;
        smith.refObjId = 7001;
        smith.codename = "NPC_CH_SMITH";
        smith.position = {assets::RegionId(168, 97), 1000, 0, 1050};
        config.npcs.push_back(smith);
    }
    using GameServerConfig = server::GameServerConfig;
    struct Player {
        std::unique_ptr<Client> client;
        std::int64_t account = 0, character = 0;
        std::uint32_t entity = 0;
        proto::InventoryList bag;
    };
    // A character at (x, 1000) of Jangan's region with `items` in its bag and `gold`; `prepare` before it enters.
    Player Make(const std::string& name, float x, const std::vector<server::StoredItem>& items, std::uint64_t gold,
                const std::function<void(std::int64_t)>& prepare = {}) {
        Player p;
        REQUIRE(server::CreateAccount(t.Db(), name + "acct", "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        p.account = server::FindAccount(t.Db(), name + "acct", ok)->id;
        Connect(p);
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
        server::CharacterRow::Place here;
        here.region = assets::RegionId(168, 97);
        here.x = x;
        here.z = 1000;
        REQUIRE(server::SavePlaces(t.Db(), {{p.character, here}}));
        REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {p.account}).Ok());
        if (!items.empty() || gold > 0) REQUIRE(server::GiveItems(t.Db(), config.id, name, items, gold, error) == server::GiveResult::Done);
        if (prepare) prepare(p.character);
        REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, $2)", {p.account, int(config.id)}).Ok());
        Enter(p, name);
        return p;
    }
    void Connect(Player& p) {
        p.client = std::make_unique<Client>(net, game.Port(), [this] { game.Poll(2ms); });
        REQUIRE(p.client->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), p.account, config.id, 30, error);
        p.client->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(p.client->Expect(entered));
        REQUIRE_MESSAGE(entered.status == proto::LobbyEnterStatus::Accepted, int(entered.status));
    }
    // Until `watcher` has seen `entity` appear.
    void Sees(Player& watcher, std::uint32_t entity) {
        proto::EntityAppeared seen;
        for (int k = 0; k < 100; ++k) {
            REQUIRE(watcher.client->Expect(seen));
            if (seen.entity == entity) return;
        }
        FAIL("never seen");
    }
    void Enter(Player& p, const std::string& name) {
        proto::LobbyResult r;
        p.client->Send(proto::CharacterSelect{name});
        REQUIRE(p.client->Expect(r));
        p.client->Send(proto::WorldEnter{});
        proto::WorldEntered w;
        REQUIRE(p.client->Expect(w));
        REQUIRE(w.status == proto::WorldStatus::Ok);
        p.entity = w.entity;
        REQUIRE(p.client->Expect(p.bag));
    }
    // Out and back in (a relog).
    void Relog(Player& p, const std::string& name) {
        p.client->Connection().Close();
        for (int i = 0; i < 500 && t.Db().Query("SELECT count(*) FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {p.account}).Int(0, 0) > 0; ++i) {
            game.Poll(2ms);
        }
        for (int i = 0; i < 100; ++i) game.Poll(2ms);
        // The session the test put back by hand (Make) carries no connection's token: freed here.
        REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {p.account}).Ok());
        Connect(p);
        Enter(p, name);
    }
    std::uint8_t PlaceOf(const proto::InventoryList& bag, std::uint32_t item) {
        for (const proto::ItemRecord& r : bag.items) {
            if (r.item == item) return r.slot;
        }
        FAIL("no such item");
        return 0;
    }
};

server::StoredItem Stored(std::uint32_t refId, std::uint16_t count = 1, std::uint32_t durability = 0) {
    server::StoredItem s;
    s.refId = refId;
    s.quantity = count;
    s.durability = durability;
    s.counted = durability == 0;
    return s;
}

template <typename T>
T Next(ServiceServer::Player& p) {
    T m;
    REQUIRE(p.client->Expect(m));
    return m;
}

proto::ServiceAnswer Answer(ServiceServer::Player& p) { return Next<proto::ServiceAnswer>(p); }

} // namespace

TEST_CASE("Mall services: the NPC's repair: its price, a partial repair, the codes; the hammer mends all free, then nothing to mend") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ServiceServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Mender", 1000, {Stored(kSword, 1, 10), Stored(kSword, 1, 25), Stored(kHammer, 2)}, 500);
    // The smith: the NPC the menu says repairs.
    std::uint32_t smith = 0;
    proto::EntityAppeared seen;
    for (int k = 0; k < 50 && smith == 0; ++k) {
        if (!p.client->Expect(seen, 500ms)) break;
        if (seen.character.model == 7001) smith = seen.entity;
    }
    REQUIRE(smith != 0);
    p.client->Send(proto::NpcTalk{smith});
    CHECK((Next<proto::NpcMenu>(p).services & proto::kNpcRepair) != 0);
    const std::uint8_t first = s.PlaceOf(p.bag, kSword);
    // 500 gold buy 20 of the 30 points missing (25 a point): 30 durability, all the gold.
    p.client->Send(proto::Repair{smith, 1, first});
    auto d = Next<proto::DurabilityChanged>(p);
    CHECK(d.place == first);
    CHECK(d.durability == 30);
    CHECK(Next<proto::GoldChanged>(p).gold == 0);
    CHECK(Answer(p).code == 0);
    // No gold left: 0x1C07; not a repairable item (the hammer): 0x1C11; nothing to mend at the most: 0x1C09.
    p.client->Send(proto::Repair{smith, 2, 0});
    CHECK(Answer(p).code == game::repaircode::kNoGold);
    p.client->Send(proto::Repair{smith, 1, s.PlaceOf(p.bag, kHammer)});
    CHECK(Answer(p).code == game::repaircode::kNoGold); // the gold is checked first (0x703E)
    // The hammer: both swords to 40, free; the second time nothing to mend (0x1888), the hammer kept.
    p.client->Send(proto::UseItem{s.PlaceOf(p.bag, kHammer), 0, 0});
    CHECK(Next<proto::DurabilityChanged>(p).durability == 40);
    CHECK(Next<proto::DurabilityChanged>(p).durability == 40);
    CHECK(Answer(p).request == proto::ServiceRequest::Hammer);
    p.client->Send(proto::UseItem{s.PlaceOf(p.bag, kHammer), 0, 0});
    CHECK(Answer(p).code == game::repaircode::kHammerNothing);
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
    CHECK(t.Db().Query("SELECT count(*) FROM _inventory v JOIN _items i ON i.id64 = v.itemid WHERE v.charid = $1 AND i.refitemid = $2 AND i.data = 40",
                       {p.character, std::int64_t(kSword)})
              .Int(0, 0) == 2);
    CHECK(t.Db().Query("SELECT i.data FROM _inventory v JOIN _items i ON i.id64 = v.itemid WHERE v.charid = $1 AND i.refitemid = $2", {p.character, std::int64_t(kHammer)})
              .Int(0, 0) == 1);
}

TEST_CASE("Mall services: the gender switch's refusals; the armour changed, kept over a relog; the skin change refused dressed, seen by a neighbour") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ServiceServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Tailor", 1000, {Stored(kTool, 2), Stored(kChestM, 1, 40), Stored(kChestHigh, 1, 40), Stored(kSword, 1, 40), Stored(kScroll, 1)}, 0);
    const std::uint8_t tool = s.PlaceOf(p.bag, kTool);
    p.client->Send(proto::UseItemOn{tool, 1}); // a worn place
    CHECK(Answer(p).code == 0x1882);
    p.client->Send(proto::UseItemOn{tool, s.PlaceOf(p.bag, kSword)});
    CHECK(Answer(p).code == 0x1884);
    p.client->Send(proto::UseItemOn{tool, s.PlaceOf(p.bag, kChestHigh)});
    CHECK(Answer(p).code == 0x1883);
    const std::uint8_t chest = s.PlaceOf(p.bag, kChestM);
    p.client->Send(proto::UseItemOn{tool, chest});
    const auto changed = Next<proto::ItemMoved>(p);
    REQUIRE(changed.changed.size() == 1);
    CHECK(changed.changed[0].item == kChestW);
    CHECK(changed.changed[0].durability == 40);
    CHECK(Answer(p).code == 0);
    s.Relog(p, "Tailor");
    CHECK(s.PlaceOf(p.bag, kChestW) == chest);
    // The skin change: to a woman while dressed (the creation's armour): 0x1892.
    auto n = s.Make("Watcher", 1010, {}, 0);
    s.Sees(n, p.entity);
    p.client->Send(proto::SkinChange{s.PlaceOf(p.bag, kScroll), kWoman, 40});
    CHECK(Answer(p).code == game::skincode::kUndress);
    // Another race's model: 2.
    p.client->Send(proto::SkinChange{s.PlaceOf(p.bag, kScroll), 200, 40});
    CHECK(Answer(p).code == game::skincode::kWrongModel);
    // The same sex, another figure is free to take; then the neighbour sees it.
    p.client->Send(proto::SkinChange{s.PlaceOf(p.bag, kScroll), 100, 60});
    CHECK(Answer(p).code == 0);
    proto::EntityLook look;
    for (int k = 0; k < 20; ++k) {
        REQUIRE(n.client->Expect(look));
        if (look.entity == p.entity) break;
    }
    CHECK(look.entity == p.entity);
    CHECK(look.height == 60);
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
    CHECK(t.Db().Query("SELECT scale FROM _char WHERE charid = $1", {p.character}).Int(0, 0) == 60);
}

TEST_CASE("Mall services: a woman made of a man once undressed; the new model kept") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ServiceServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Changer", 1000, {Stored(kScroll, 1)}, 0, [&](std::int64_t character) {
        REQUIRE(t.Db().Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND slot < 13 AND slot <> 6", {character}).Ok());
    });
    auto n = s.Make("Neighbour", 1010, {}, 0);
    s.Sees(n, p.entity);
    p.client->Send(proto::SkinChange{s.PlaceOf(p.bag, kScroll), kWoman, 34});
    const auto a = Answer(p);
    CHECK(a.code == 0);
    CHECK(a.a == kWoman);
    proto::EntityLook look;
    for (int k = 0; k < 20; ++k) {
        REQUIRE(n.client->Expect(look));
        if (look.entity == p.entity) break;
    }
    CHECK(look.model == kWoman);
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
    CHECK(t.Db().Query("SELECT refobjid FROM _char WHERE charid = $1", {p.character}).Int(0, 0) == kWoman);
}

TEST_CASE("Mall services: withdrawal: two levels take two potions and give the SP back; a mastery refused while a skill needs it") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ServiceServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Unlearner", 1000, {Stored(kPotion, 3)}, 0, [&](std::int64_t character) {
        REQUIRE(t.Db().Query("INSERT INTO _charskillmastery (charid, masteryid, level) VALUES ($1, $2, 5) "
                             "ON CONFLICT (charid, masteryid) DO UPDATE SET level = 5",
                             {character, std::int64_t(kMastery)})
                    .Ok());
        for (std::int64_t skill : {5003, 5011, 5022}) {
            REQUIRE(t.Db().Query("INSERT INTO _charskill (charid, skillid, enable) VALUES ($1, $2, 1)", {character, skill}).Ok());
        }
        REQUIRE(t.Db().Query("UPDATE _char SET remainskillpoint = 100 WHERE charid = $1", {character}).Ok());
    });
    const std::uint8_t potion = s.PlaceOf(p.bag, kPotion);
    // The mastery to 3: the group-2 skill needs it at 5.
    p.client->Send(proto::Withdraw{potion, 1, kMastery, 3});
    CHECK(Answer(p).code == game::withdrawcode::kRequired);
    // Group 1 to 1: group 2 needs it at 2.
    p.client->Send(proto::Withdraw{potion, 0, 5003, 1});
    CHECK(Answer(p).code == game::withdrawcode::kRequired);
    // Group 3 from 2 to 0: two potions, 40 SP back.
    p.client->Send(proto::Withdraw{potion, 0, 5022, 0});
    const auto a = Answer(p);
    CHECK(a.code == 0);
    CHECK(a.b == 0);
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
    CHECK(t.Db().Query("SELECT remainskillpoint FROM _char WHERE charid = $1", {p.character}).Int(0, 0) == 140);
    CHECK(t.Db().Query("SELECT count(*) FROM _charskill WHERE charid = $1 AND skillid IN (5021, 5022)", {p.character}).Int(0, 0) == 0);
    CHECK(t.Db().Query("SELECT i.data FROM _inventory v JOIN _items i ON i.id64 = v.itemid WHERE v.charid = $1 AND i.refitemid = $2", {p.character, std::int64_t(kPotion)})
              .Int(0, 0) == 1);
    // Group 1 from 3 to 2: one potion left, enough; then none: 0x7805.
    p.client->Send(proto::Withdraw{potion, 0, 5003, 2});
    CHECK(Answer(p).code == 0);
    p.client->Send(proto::Withdraw{potion, 0, 5002, 1}); // no potion left there
    CHECK(Answer(p).code != 0);
}

TEST_CASE("Mall services: a magic cube filled with its kind of elixir to its limit, emptied into the bag, kept over a relog") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ServiceServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Cuber", 1000, {Stored(kCube, 1, 0), Stored(kElixir, 5), Stored(kElixir, 3), Stored(kElixir, 2), Stored(kArmourElixir, 1)}, 0);
    REQUIRE(Next<proto::CubeContents>(p).items.empty());
    const std::uint8_t cube = s.PlaceOf(p.bag, kCube);
    std::vector<std::uint8_t> elixirs;
    for (const proto::ItemRecord& r : p.bag.items) {
        if (r.item == kElixir) elixirs.push_back(r.slot);
    }
    REQUIRE(elixirs.size() == 3);
    p.client->Send(proto::CubeMove{0, s.PlaceOf(p.bag, kArmourElixir), cube, 0});
    CHECK(Answer(p).code == game::cubecode::kNotTaken);
    p.client->Send(proto::CubeMove{0, elixirs[0], cube, 0});
    CHECK(Next<proto::CubeContents>(p).items.size() == 1);
    CHECK(Answer(p).code == 0);
    p.client->Send(proto::CubeMove{0, elixirs[1], cube, 0});
    CHECK(Next<proto::CubeContents>(p).items.size() == 2);
    CHECK(Answer(p).code == 0);
    p.client->Send(proto::CubeMove{0, elixirs[2], cube, 0});
    CHECK(Answer(p).code == game::cubecode::kFull);
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
    CHECK(t.Db().Query("SELECT count(*) FROM _inventoryforlinkedstorage").Int(0, 0) == 2);
    s.Relog(p, "Cuber");
    const auto kept = Next<proto::CubeContents>(p);
    REQUIRE(kept.items.size() == 2);
    // One taken out to the bag's first free place.
    p.client->Send(proto::CubeMove{1, 0, cube, kept.items[0].slot});
    const auto moved = Next<proto::ItemMoved>(p);
    REQUIRE(moved.changed.size() == 1);
    CHECK(moved.changed[0].item == kElixir);
    CHECK(Next<proto::CubeContents>(p).items.size() == 1);
    CHECK(Answer(p).code == 0);
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
    CHECK(t.Db().Query("SELECT count(*) FROM _inventoryforlinkedstorage").Int(0, 0) == 1);
    CHECK(t.Db().Query("SELECT count(*) FROM _inventory v JOIN _items i ON i.id64 = v.itemid WHERE v.charid = $1 AND i.refitemid = $2", {p.character, std::int64_t(kElixir)})
              .Int(0, 0) == 2);
}
