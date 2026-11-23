// The item mall's items: the time tickets' record, day and battle bonus; the premium package's
// parts and daily uses; the resurrection scroll; the expansions; the Devil's Spirit's awakening and its Extension Gear;
// the scrolls' and balloons' buffs and a gold ticket's bonus on a kill (game files). Set SR_DB_URL (and SR_GAME_DIR for
// the game files' case); otherwise skipped.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/OpenMarket.h"
#include "sr/game/Mall.h"
#include "sr/game/MallProtocol.h"
#include "sr/game/TimedServices.h"
#include "sr/server/GameServer.h"
#include "sr/server/ServiceStore.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;

namespace {

constexpr std::uint32_t kGold1D = 920001, kSkillGold4W = 920002, kPremium = 920003, kPremiumTicket = 920004, kReturnHigh = 920005,
                        kReverse = 920006, kResurrect100 = 920007, kBooth = 920008, kBagPlus = 920009, kStoragePlus = 920010, kSpirit = 920011,
                        kExtension = 920012, kMarket = 920013, kResurrect60 = 920014, kNoStorage = 920015, kNoDrop = 920016;
constexpr std::uint32_t kKeeper = 920100; // a storage keeper (NPC_CH_WAREHOUSE_M)

assets::ItemRow Row(std::uint32_t id, const char* codename, std::array<int, 4> tid) {
    assets::ItemRow r;
    r.refObjId = id;
    r.codename = codename;
    r.typeId = tid;
    r.inService = true;
    r.maxStack = tid[1] == 3 ? 50 : 1;
    r.cashItem = true;
    return r;
}

assets::ItemRow Ticket(std::uint32_t id, const char* codename, int kind, int seconds, int exp, int skill) {
    assets::ItemRow r = Row(id, codename, {3, 3, 13, kind});
    r.params[0] = seconds;
    r.params[2] = 10'800'000; // 3 hours a day
    r.params[3] = exp;
    r.params[4] = skill;
    return r;
}

std::shared_ptr<assets::ItemTable> Items() {
    auto items = std::make_shared<assets::ItemTable>();
    items->Add(Ticket(kGold1D, "ITEM_MALL_GOLD_TIME_SERVICE_TICKET_1D", 4, 86400, 100, 100));
    items->Add(Ticket(kSkillGold4W, "ITEM_MALL_SKILL_GOLD_TIME_SERVICE_TICKET_4W", 5, 2419200, 0, 100));
    items->Add(Ticket(kPremiumTicket, "ITEM_PRE_GOLTAM_4W", 4, 2419200, 100, 100));
    assets::ItemRow premium = Row(kPremium, "ITEM_MALL_PREMIUM_GOLDTIME", {3, 3, 13, 14});
    premium.params[0] = 2419200;
    premium.paramTexts[1] = "[UIU1:ITEM_PRE_GOLTAM_4W],[UIL1:ITEM_MALL_RESURRECTION_100P_SCROLL, A, 86400, 1],[USU1:SKILL_MALL_PRE_APRU_4W_01]";
    premium.paramTexts[2] = "[UIL1:ITEM_MALL_REVERSE_RETURN_SCROLL, A, 86400, 3],[UIL1:ITEM_MALL_RETURN_SCROLL_HIGH_SPEED, A, 86400, 3]";
    premium.paramTexts[3] = "[BFI1:ITEM_MALL_BOOTH_MOB_MANGYANG,A,2419200],[UQL1:QSP_ALL_POTION_3, A, 2419200, 3]";
    items->Add(premium);
    assets::ItemRow ret = Row(kReturnHigh, "ITEM_MALL_RETURN_SCROLL_HIGH_SPEED", {3, 3, 3, 1});
    ret.params[0] = 1000;
    ret.paramTexts[2] = "RESURRECT";
    items->Add(ret);
    assets::ItemRow reverse = Row(kReverse, "ITEM_MALL_REVERSE_RETURN_SCROLL", {3, 3, 3, 3});
    reverse.params[0] = 1000;
    items->Add(reverse);
    assets::ItemRow rise = Row(kResurrect100, "ITEM_MALL_RESURRECTION_100P_SCROLL", {3, 3, 13, 6});
    rise.params[1] = 100;
    rise.params[2] = 100;
    items->Add(rise);
    assets::ItemRow rise60 = Row(kResurrect60, "ITEM_MALL_RESURRECTION_60P_SCROLL", {3, 3, 13, 6});
    rise60.params[1] = 100;
    rise60.params[2] = 60;
    items->Add(rise60);
    items->Add(Row(kBooth, "ITEM_MALL_BOOTH_MOB_MANGYANG", {3, 3, 3, 4}));
    assets::ItemRow bag = Row(kBagPlus, "ITEM_MALL_INVENTORY_ADDITION", {3, 3, 13, 17});
    bag.params[0] = 32;
    bag.params[1] = 0;
    items->Add(bag);
    assets::ItemRow storage = Row(kStoragePlus, "ITEM_MALL_WAREHOUSE_ADDITION", {3, 3, 13, 17});
    storage.params[0] = 30;
    storage.params[1] = 1;
    items->Add(storage);
    assets::ItemRow spirit = Row(kSpirit, "ITEM_MALL_AVATAR_M_NASRUN", {3, 1, 14, 1});
    spirit.params[0] = 2419200;
    items->Add(spirit);
    assets::ItemRow extension = Row(kExtension, "ITEM_MALL_NASRUN_EXTENSION", {3, 3, 13, 16});
    extension.params[0] = 2419200;
    items->Add(extension);
    items->Add(Row(kMarket, "ITEM_MALL_OPEN_MARKET_TICKET", {3, 3, 13, 19}));
    // A quest item no storage takes (CanBorrow 0), and one never dropped by hand (CanDrop 0).
    assets::ItemRow unstored = Row(kNoStorage, "ITEM_QNO_TEST_LETTER", {3, 3, 9, 1});
    unstored.canBorrow = 0;
    items->Add(unstored);
    assets::ItemRow undroppable = Row(kNoDrop, "ITEM_MALL_TEST_KEEPSAKE", {3, 3, 13, 19});
    undroppable.canDrop = 0;
    items->Add(undroppable);
    return items;
}

server::StoredItem Stored(std::uint32_t refId, std::uint16_t count = 1, std::uint32_t data = 0) {
    server::StoredItem i;
    i.refId = refId;
    i.quantity = count;
    i.counted = refId != kSpirit;
    i.durability = data;
    return i;
}

struct ItemServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    explicit ItemServer(TestDatabase& db) : t(db) {
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
        // A dropped connection leaves at once (no logout count kept in the world): Relog edits the rows in between.
        config.logoutCountdown = std::chrono::seconds(0);
        config.databaseThreads = 2;
        config.devCommands = true;
        config.levels.assign(120, 50'000'000);
        config.monsterExperience.assign(120, 1000);
        config.npcs.push_back({kKeeper, "NPC_CH_WAREHOUSE_M", {assets::RegionId(168, 97), 1050.0f, 0.0f, 1000.0f}, 0.0f});
    }
    struct Player {
        std::unique_ptr<Client> client;
        std::string name;
        std::int64_t account = 0, character = 0;
        std::uint32_t entity = 0;
        proto::InventoryList bag;
    };
    std::int64_t AccountOf(const std::string& name) {
        bool ok = false;
        return server::FindAccount(t.Db(), name + "acct", ok)->id;
    }
    // A level-30 character with `items` in its bag, entered (made the first time).
    Player Make(const std::string& name, const std::vector<server::StoredItem>& items = {}, bool create = true) {
        Player p;
        p.name = name;
        if (create) REQUIRE(server::CreateAccount(t.Db(), name + "acct", "secret1", error) == server::CreateAccountResult::Created);
        p.account = AccountOf(name);
        p.client = std::make_unique<Client>(net, game.Port(), [this] { game.Poll(2ms); });
        REQUIRE(p.client->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), p.account, config.id, 30, error);
        p.client->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(p.client->Expect(entered));
        proto::LobbyResult r;
        if (create) {
            proto::CharacterCreate m;
            m.name = name;
            m.model = 100;
            m.chest = 1;
            m.legs = 2;
            m.shoes = 3;
            m.weapon = 10;
            p.client->Send(m);
            REQUIRE(p.client->Expect(r));
            REQUIRE(r.status == proto::LobbyStatus::Ok);
        }
        bool ok = false;
        p.character = server::ListCharacters(t.Db(), p.account, config.id, 0, ok)[0].id;
        if (create) {
            REQUIRE(server::SaveProgress(t.Db(), {{p.character, 30, 1'000'000, 0, 0, 40, 40, 0}}));
            server::CharacterRow::Place here;
            here.region = assets::RegionId(168, 97);
            here.x = 1000;
            here.z = 1000;
            REQUIRE(server::SavePlaces(t.Db(), {{p.character, here}}));
            if (!items.empty()) {
                REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {p.account}).Ok());
                REQUIRE(server::GiveItems(t.Db(), config.id, name, items, 0, error) == server::GiveResult::Done);
                REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, $2)", {p.account, int(config.id)}).Ok());
            }
        }
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
    // Out (the session freed), then in again.
    Player Relog(Player& p, const std::function<void()>& between = {}) {
        p.client->Connection().Close();
        for (int i = 0; i < 500 && t.Db().Query("SELECT count(*) FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {p.account}).Int(0, 0) > 0; ++i) {
            game.Poll(2ms);
        }
        for (int i = 0; i < 100; ++i) game.Poll(2ms);
        // The session the test put back by hand (GiveItems) carries no connection's token: freed here as the server would.
        REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {p.account}).Ok());
        if (between) between();
        return Make(p.name, {}, false);
    }
    std::uint8_t PlaceOf(const Player& p, std::uint32_t refId) {
        for (const proto::ItemRecord& i : p.bag.items) {
            if (i.item == refId) return i.slot;
        }
        FAIL("no such item");
        return 0;
    }
};

template <typename T>
T Next(ItemServer::Player& p, std::chrono::milliseconds timeout = 10s) {
    T m;
    REQUIRE(p.client->Expect(m, timeout));
    return m;
}

// The services told until one holds `category` (or none of them do).
proto::TimedServices Services(ItemServer::Player& p) { return Next<proto::TimedServices>(p); }
const proto::TimedService* Find(const proto::TimedServices& s, std::uint8_t category) {
    for (const proto::TimedService& x : s.services) {
        if (x.category == category) return &x;
    }
    return nullptr;
}

} // namespace

TEST_CASE("Time services: the original's record, the day's turn on the wall clock, the bonus spent only in battle") {
    const auto items = Items();
    const std::int64_t start = 1'800'000'000;
    game::TimedService s = game::TimedService::Begin(*items->Find(kSkillGold4W), game::kTimedSkill, start);
    CHECK(s.days == 28);
    CHECK(s.daysLeft == 28);
    CHECK(s.bonusToday == 10800);
    CHECK(s.skillPercent == 100);
    CHECK(s.expPercent == 0);
    CHECK(s.BonusOn());
    // An hour of battle spends an hour.
    s.SpendBattle(3600);
    CHECK(s.bonusToday == 7200);
    // 25 hours later: the day turned; today's unused moved to yesterday's, a new day's given.
    CHECK(s.Advance(start + 25 * 3600));
    CHECK(s.daysLeft == 27);
    CHECK(s.secondsIntoDay == 3600);
    CHECK(s.bonusYesterday == 7200);
    CHECK(s.bonusToday == 10800);
    // Spent from yesterday's first.
    s.SpendBattle(7300);
    CHECK(s.bonusYesterday == 0);
    CHECK(s.bonusToday == 10700);
    // The record and back.
    const game::TimedService again = game::TimedService::FromRecord(game::kTimedSkill, kSkillGold4W, s.TimeToKeep(), std::int32_t(s.start), s.Data2(), 0, 0, 0,
                                                                    items->Find(kSkillGold4W));
    CHECK(again.daysLeft == 27);
    CHECK(again.secondsIntoDay == 3600);
    CHECK(again.bonusToday == 10700);
    CHECK(again.skillPercent == 100);
    // Three days offline: yesterday's is a whole day's (the older lost).
    game::TimedService later = again;
    CHECK(later.Advance(start + 4 * 86400 + 10));
    CHECK(later.daysLeft == 24);
    CHECK(later.bonusYesterday == 10800);
    // Paused: the time stands, the bonus is not spent.
    later.Pause(true, start + 4 * 86400 + 10);
    later.SpendBattle(500);
    CHECK(later.bonusToday == 10800);
    CHECK_FALSE(later.BonusOn());
    CHECK(later.Advance(start + 10 * 86400));
    CHECK(later.daysLeft == 24);
    later.Pause(false, start + 10 * 86400);
    CHECK(later.Advance(start + 11 * 86400 + 20));
    CHECK(later.daysLeft == 23);
    // The end: a one-day ticket the day after.
    game::TimedService day = game::TimedService::Begin(*items->Find(kGold1D), game::kTimedGoldSilver, start);
    CHECK(day.Advance(start + 86399));
    CHECK_FALSE(day.Advance(start + 86400));
    CHECK(game::TimedService::Begin(*items->Find(kGold1D), 3, start).SecondsLeft(start + 400) == 86000);
}

TEST_CASE("Premium gold time: the package's parts, the day's uses") {
    const auto items = Items();
    const auto parts = game::PremiumPartsOf(*items->Find(kPremium));
    REQUIRE(parts.size() == 7);
    CHECK(parts[0].tag == "UIU1");
    CHECK(parts[0].target == "ITEM_PRE_GOLTAM_4W");
    CHECK(parts[1].tag == "UIL1");
    CHECK(parts[1].target == "ITEM_MALL_RESURRECTION_100P_SCROLL");
    CHECK(parts[1].period == 86400);
    CHECK(parts[1].count == 1);
    CHECK(parts[2].tag == "USU1");
    CHECK(parts[4].target == "ITEM_MALL_RETURN_SCROLL_HIGH_SPEED");
    CHECK(parts[4].count == 3);
    CHECK(parts[5].tag == "BFI1");
    CHECK(parts[5].period == 2419200);
    CHECK(parts[6].tag == "UQL1");
    game::PremiumState p;
    p.item = kPremium;
    p.start = 1000;
    p.seconds = 2419200;
    p.used = {1, 3, 2};
    p.Advance(1000 + 86399);
    CHECK(p.used[1] == 3);
    p.Advance(1000 + 86400);
    CHECK(p.used == std::array<int, 3>{0, 0, 0});
    CHECK(p.periodStart == 1000 + 86400);
    CHECK(p.Running(1000 + 2419199));
    CHECK_FALSE(p.Running(1000 + 2419200));
}

TEST_CASE("Game server: a time ticket: one of a kind, told, kept over a relog, its time running while away") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ItemServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Ticketed", {Stored(kGold1D, 2), Stored(kSkillGold4W), Stored(kMarket)});
    CHECK(Services(p).services.empty()); // on entering: none
    p.client->Send(proto::UseItem{s.PlaceOf(p, kGold1D)});
    auto told = Services(p);
    const proto::TimedService* gold = Find(told, game::kTimedGoldSilver);
    REQUIRE(gold != nullptr);
    CHECK(gold->item == kGold1D);
    CHECK(gold->secondsLeft > 86000);
    CHECK(gold->bonus == 10800);
    // A second gold or silver one: refused (0x1894), the item kept.
    p.client->Send(proto::UseItem{s.PlaceOf(p, kGold1D)});
    proto::ItemRefused refused;
    for (;;) {
        refused = Next<proto::ItemRefused>(p);
        if (refused.item == kGold1D) break;
    }
    CHECK(refused.code == 0x1894);
    // A skill one beside it.
    p.client->Send(proto::UseItem{s.PlaceOf(p, kSkillGold4W)});
    for (;;) {
        told = Services(p);
        if (Find(told, game::kTimedSkill) != nullptr) break;
    }
    CHECK(Find(told, game::kTimedGoldSilver) != nullptr);
    // The Open Market ticket: its own service (category 11), no bonus of its own.
    p.client->Send(proto::UseItem{s.PlaceOf(p, kMarket)});
    for (;;) {
        told = Services(p);
        if (Find(told, game::openmarket::kTicketCategory) != nullptr) break;
    }
    CHECK(Find(told, game::openmarket::kTicketCategory)->item == kMarket);
    // Kept: `_timedjob` holds both in the original's categories.
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
    CHECK(t.Db().Query("SELECT count(*) FROM _timedjob WHERE charid = $1 AND category IN (3, 9)", {p.character}).Int(0, 0) == 2);
    auto again = s.Relog(p);
    told = Services(again);
    CHECK(Find(told, game::kTimedGoldSilver) != nullptr);
    CHECK(Find(told, game::kTimedSkill) != nullptr);
    // Two days later (its start moved back): the one-day ticket ran out while away; the four-week one runs on.
    auto third = s.Relog(again, [&] {
        REQUIRE(t.Db().Query("UPDATE _timedjob SET data1 = data1 - 2 * 86400 WHERE charid = $1 AND category IN (3, 9)", {p.character}).Ok());
    });
    told = Services(third);
    CHECK(Find(told, game::kTimedGoldSilver) == nullptr);
    const proto::TimedService* skill = Find(told, game::kTimedSkill);
    REQUIRE(skill != nullptr);
    CHECK(skill->secondsLeft <= 26u * 86400u + 10u);
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
    CHECK(t.Db().Query("SELECT count(*) FROM _timedjob WHERE charid = $1 AND category = 3", {p.character}).Int(0, 0) == 0);
}

TEST_CASE("Game server: the premium: its ticket and daily uses, three /Return a day and the fourth refused, /Resurrection only when dead") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ItemServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Premier", {Stored(kPremium)});
    Services(p);
    // Without the premium: 0x18CC.
    p.client->Send(proto::PremiumUse{kReturnHigh, 0, 0});
    CHECK(Next<proto::ServiceAnswer>(p).code == 0x18CC);
    p.client->Send(proto::UseItem{s.PlaceOf(p, kPremium)});
    auto told = Services(p);
    const proto::TimedService* package = Find(told, game::kTimedPremium);
    REQUIRE(package != nullptr);
    CHECK(package->item == kPremium);
    CHECK(Find(told, game::kTimedGoldSilver) != nullptr); // its 4-week gold time
    REQUIRE(told.premium.size() == 3);
    CHECK(told.premium[2].item == kReturnHigh);
    CHECK(told.premium[2].left == 3);
    // Three high-speed returns work (each cancelled before it lands), the fourth is refused.
    for (int k = 0; k < 3; ++k) {
        p.client->Send(proto::PremiumUse{kReturnHigh, 0, 0});
        const auto a = Next<proto::ServiceAnswer>(p);
        CHECK(a.code == 0);
        CHECK(a.b == std::uint32_t(2 - k));
        Next<proto::ReturnStarted>(p);
        p.client->Send(proto::CancelReturn{});
        for (int i = 0; i < 50; ++i) s.game.Poll(2ms);
    }
    p.client->Send(proto::PremiumUse{kReturnHigh, 0, 0});
    CHECK(Next<proto::ServiceAnswer>(p).code == 0x18C6);
    // /Resurrection alive: 0x1887; a second package: 0x1894.
    p.client->Send(proto::PremiumUse{kResurrect100, 0, 0});
    CHECK(Next<proto::ServiceAnswer>(p).code == 0x1887);
    // The day's uses kept over a relog.
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
    auto again = s.Relog(p);
    told = Services(again);
    REQUIRE(told.premium.size() == 3);
    CHECK(told.premium[2].left == 0);
    // A day later they are back.
    auto third = s.Relog(again, [&] {
        REQUIRE(t.Db().Query("UPDATE _timedjob SET data1 = data1 - 86401, data5 = data5 - 86401 WHERE charid = $1 AND category = 10", {p.character}).Ok());
    });
    told = Services(third);
    REQUIRE(told.premium.size() == 3);
    CHECK(told.premium[2].left == 3);
}

TEST_CASE("Game server: the resurrection scroll: only for the dead; 100 % of the lost experience back where it fell, 60 % rounded up") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ItemServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Riser", {Stored(kResurrect100, 2), Stored(kResurrect60)});
    p.client->Send(proto::UseItem{s.PlaceOf(p, kResurrect100)});
    proto::ItemRefused refused;
    for (;;) {
        refused = Next<proto::ItemRefused>(p);
        if (refused.item == kResurrect100) break;
    }
    CHECK(refused.code == 0x1887);
    const auto experience = [&] { return t.Db().Query("SELECT exp FROM _char WHERE charid = $1", {p.character}).Int(0, 0); };
    p.client->Send(proto::DevDie{});
    for (int i = 0; i < 200; ++i) s.game.Poll(2ms);
    // Dead: the progress tells what it lost.
    proto::Progress progress;
    while (p.client->Expect(progress, 300ms)) {
    }
    const std::uint64_t afterDeath = progress.experience;
    CHECK(afterDeath < 1'000'000);
    p.client->Send(proto::UseItem{s.PlaceOf(p, kResurrect100)});
    proto::Progress risen;
    for (int i = 0; i < 20; ++i) {
        REQUIRE(p.client->Expect(risen, 2s));
        if (risen.experience != afterDeath) break;
    }
    CHECK(risen.experience == 1'000'000);
    (void)experience;
    // Again, with the 60 % one.
    p.client->Send(proto::DevDie{});
    for (int i = 0; i < 200; ++i) s.game.Poll(2ms);
    while (p.client->Expect(progress, 300ms)) {
    }
    const std::uint64_t lost = 1'000'000 - progress.experience;
    p.client->Send(proto::UseItem{s.PlaceOf(p, kResurrect60)});
    for (int i = 0; i < 20; ++i) {
        REQUIRE(p.client->Expect(risen, 2s));
        if (risen.experience != progress.experience) break;
    }
    CHECK(risen.experience == progress.experience + (lost * 60 + 99) / 100);
}

TEST_CASE("Game server: the expansions: a bag of 45 becomes 77 and no more; the storage 150 -> 180 -> 210, the third refused") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ItemServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Grower", {Stored(kBagPlus, 2), Stored(kStoragePlus, 3)});
    CHECK(p.bag.size == 45);
    p.client->Send(proto::UseItem{s.PlaceOf(p, kBagPlus)});
    const auto grown = Next<proto::StorageGrown>(p);
    CHECK(grown.target == 1);
    CHECK(grown.size == 77);
    p.client->Send(proto::UseItem{s.PlaceOf(p, kBagPlus)});
    proto::ItemRefused refused;
    for (;;) {
        refused = Next<proto::ItemRefused>(p);
        if (refused.item == kBagPlus) break;
    }
    CHECK(refused.code == 0x18FE);
    // The storage: not before it was opened this session (2), then +30 twice, the third past 210.
    p.client->Send(proto::UseItem{s.PlaceOf(p, kStoragePlus)});
    for (;;) {
        refused = Next<proto::ItemRefused>(p);
        if (refused.item == kStoragePlus) break;
    }
    CHECK(refused.code == 2);
    std::uint32_t keeper = 0;
    while (keeper == 0) {
        const auto m = Next<proto::EntityAppeared>(p);
        if (m.character.model == kKeeper) keeper = m.entity;
    }
    p.client->Send(proto::StorageOpen{keeper, 0});
    CHECK(Next<proto::StorageList>(p).size == 150);
    for (int size : {180, 210}) {
        p.client->Send(proto::UseItem{s.PlaceOf(p, kStoragePlus)});
        const auto g = Next<proto::StorageGrown>(p);
        CHECK(g.target == 2);
        CHECK(g.size == size);
    }
    p.client->Send(proto::UseItem{s.PlaceOf(p, kStoragePlus)});
    for (;;) {
        refused = Next<proto::ItemRefused>(p);
        if (refused.item == kStoragePlus) break;
    }
    CHECK(refused.code == 0x18FE);
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
    CHECK(t.Db().Query("SELECT inventorysize FROM _char WHERE charid = $1", {p.character}).Int(0, 0) == 77);
    CHECK(t.Db().Query("SELECT chestsize FROM _chestinfo WHERE jid = $1", {p.account}).Int(0, 0) == 210);
}

TEST_CASE("Game server: the Devil's Spirit: worn only while awake (0x18F9); the Extension Gear needs it awakened (0x18F8) and adds four weeks") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ItemServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    const std::int64_t now = std::int64_t(std::time(nullptr));
    auto p = s.Make("Devil", {Stored(kSpirit, 1, 0), Stored(kSpirit, 1, std::uint32_t(now - 10)), Stored(kExtension, 3)});
    std::uint8_t never = 0, expired = 0;
    for (const proto::ItemRecord& i : p.bag.items) {
        if (i.item == kSpirit && i.durability == 0) never = i.slot;
        if (i.item == kSpirit && i.durability != 0) expired = i.slot;
    }
    REQUIRE(never != 0);
    REQUIRE(expired != 0);
    // Expired: not worn.
    p.client->Send(proto::AvatarMove{true, expired});
    CHECK(Next<proto::ItemRefused>(p).code == 0x18F9);
    CHECK(Next<proto::AvatarMoved>(p).status != proto::ItemStatus::Ok);
    // Never awakened: the extension refused; on a worn place: 0x1882.
    const std::uint8_t gear = s.PlaceOf(p, kExtension);
    p.client->Send(proto::UseItemOn{gear, never});
    CHECK(Next<proto::ServiceAnswer>(p).code == 0x18F8);
    p.client->Send(proto::UseItemOn{gear, 6});
    CHECK(Next<proto::ServiceAnswer>(p).code == 0x1882);
    // Expired once: extended from now by four weeks; then worn.
    p.client->Send(proto::UseItemOn{gear, expired});
    proto::ItemMoved changed = Next<proto::ItemMoved>(p);
    REQUIRE(changed.changed.size() == 1);
    CHECK(std::abs(std::int64_t(changed.changed[0].durability) - (now + 2419200)) < 30);
    const auto a = Next<proto::ServiceAnswer>(p);
    CHECK(a.code == 0);
    CHECK(a.b > 2419000u);
    p.client->Send(proto::AvatarMove{true, expired});
    CHECK(Next<proto::AvatarMoved>(p).status == proto::ItemStatus::Ok);
}

TEST_CASE("Game server: a gold ticket doubles a kill's experience and skill experience; a balloon's and a damage scroll's buffs (game files)") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    auto items = std::make_shared<assets::ItemTable>();
    REQUIRE_MESSAGE(items->Load(fs, error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    const assets::ItemRow* sword = nullptr;
    for (const auto& [code, r] : items->rows()) {
        if (r.typeId != std::array<int, 4>{3, 1, 6, 2} || r.country != 0 || !r.inService || r.requiredLevel > 5 || r.rarity != 0) continue;
        if (sword == nullptr || r.requiredLevel > sword->requiredLevel || (r.requiredLevel == sword->requiredLevel && r.refObjId < sword->refObjId)) sword = &r;
    }
    REQUIRE(sword != nullptr);
    const assets::ObjectRow* weakest = nullptr;
    for (const auto& [code, r] : characters->rows()) {
        if (r.typeId[1] != 2 || r.typeId[2] != 1 || r.level < 5 || r.level > 8 || r.rarity != 0 || r.maxHp < 20 || r.experience == 0) continue;
        if (!code.starts_with("MOB_") || code.find("_NPC") != std::string::npos || code.find("THIEF") != std::string::npos) continue;
        const assets::SkillRow* attack = r.skills[0] != 0 ? skills->Find(r.skills[0]) : nullptr;
        if (attack == nullptr || !attack->attack) continue;
        if (weakest == nullptr || r.maxHp < weakest->maxHp) weakest = &r;
    }
    REQUIRE(weakest != nullptr);
    ItemServer s(t);
    auto rules = std::const_pointer_cast<game::CharacterRules>(s.config.rules);
    rules->items = items;
    rules->creation.AddWeapon(game::CreationRules::Race::China, sword->refObjId);
    s.config.files = &fs;
    s.config.characters = characters;
    s.config.skills = skills;
    s.config.monsterExperience.assign(120, 100);
    server::MonsterNest nest;
    nest.refObjId = weakest->refObjId;
    nest.position = {assets::RegionId(170, 97), 1030, 0, 1000}; // the field east of Jangan (the town is safe ground)
    nest.radius = 300;
    nest.maxCount = 1;
    s.config.monsterNests = {nest};
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    const auto byName = [&](const char* code) { return items->Find(std::string_view(code))->refObjId; };
    const std::uint32_t ticket = byName("ITEM_MALL_GOLD_TIME_SERVICE_TICKET_1D"), damage = byName("ITEM_MALL_DAMAGE_INC_20P_SCROLL");
    std::uint32_t balloon = 0;
    for (const auto& [code, r] : items->rows()) {
        if (code.starts_with("ITEM_MALL_BALLOON") && r.typeId == std::array<int, 4>{3, 3, 13, 1} && balloon == 0) balloon = r.refObjId;
    }
    REQUIRE(balloon != 0);
    // A sword in hand (the creation's weapon), the items in the bag; three levels above the monster.
    REQUIRE(server::CreateAccount(t.Db(), "Fighteracct", "secret1", error) == server::CreateAccountResult::Created);
    s.config.levels.assign(120, 50'000'000);
    auto p = [&] {
        ItemServer::Player q;
        q.name = "Fighter";
        q.account = s.AccountOf("Fighter");
        q.client = std::make_unique<Client>(s.net, s.game.Port(), [&] { s.game.Poll(2ms); });
        REQUIRE(q.client->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), q.account, s.config.id, 30, error);
        q.client->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(q.client->Expect(entered));
        proto::CharacterCreate m;
        m.name = "Fighter";
        m.model = 100;
        m.chest = 1;
        m.legs = 2;
        m.shoes = 3;
        m.weapon = sword->refObjId;
        q.client->Send(m);
        proto::LobbyResult r;
        REQUIRE(q.client->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        bool ok = false;
        q.character = server::ListCharacters(t.Db(), q.account, s.config.id, 0, ok)[0].id;
        REQUIRE(server::SaveProgress(t.Db(), {{q.character, std::uint8_t(weakest->level + 3), 0, 0, 0, 40, 40, 0}}));
        REQUIRE(server::SavePlaces(t.Db(), {{q.character, {assets::RegionId(170, 97), 1000.0f, 0.0f, 1000.0f, 0.0f}}}));
        REQUIRE(server::SaveVitals(t.Db(), q.character, 100'000, 100'000));
        REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {q.account}).Ok());
        REQUIRE(server::GiveItems(t.Db(), s.config.id, "Fighter", {Stored(ticket), Stored(damage), Stored(balloon)}, 0, error) == server::GiveResult::Done);
        REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, $2)", {q.account, int(s.config.id)}).Ok());
        q.client->Send(proto::CharacterSelect{"Fighter"});
        REQUIRE(q.client->Expect(r));
        q.client->Send(proto::WorldEnter{});
        proto::WorldEntered w;
        REQUIRE(q.client->Expect(w));
        q.entity = w.entity;
        REQUIRE(q.client->Expect(q.bag));
        return q;
    }();
    const auto kill = [&] {
        std::uint32_t monster = 0;
        for (int i = 0; i < 60 && monster == 0; ++i) {
            proto::EntityAppeared seen;
            if (p.client->Expect(seen, 1s) && seen.kind == proto::EntityKind::Monster) monster = seen.entity;
        }
        REQUIRE(monster != 0);
        p.client->Send(proto::AttackTarget{monster});
        proto::ExperienceGained g;
        REQUIRE(p.client->Expect(g, 60s));
        CHECK(g.from == monster);
        return g;
    };
    const auto plain = kill();
    CHECK(plain.experience > 0);
    // The scrolls: their skills' buffs on the reader (the damage +20 %, the balloon's).
    for (std::uint32_t scroll : {damage, balloon}) {
        p.client->Send(proto::UseItem{s.PlaceOf(p, scroll)});
        proto::ItemUsed used;
        for (;;) {
            used = Next<proto::ItemUsed>(p);
            if (used.item == scroll) break;
        }
        CHECK(used.entity == p.entity);
    }
    p.client->Send(proto::UseItem{s.PlaceOf(p, ticket)});
    proto::TimedServices told;
    for (;;) {
        told = Services(p);
        if (Find(told, game::kTimedGoldSilver) != nullptr) break;
    }
    const auto doubled = kill();
    CHECK(doubled.experience == plain.experience * 2);
    CHECK(doubled.skillExperience == plain.skillExperience * 2);
}

TEST_CASE("Game server: the storage: an item keeps its id both ways, a split is a new item, a merge frees the one merged away; the gold in one transaction") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ItemServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Vaulter", {Stored(kReturnHigh, 10), Stored(kBooth, 1), Stored(kNoStorage, 1), Stored(kNoDrop, 1)});
    const auto settle = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 500 && !done(); ++i) s.game.Poll(2ms);
        return done();
    };
    const auto bagId = [&](std::uint8_t place) {
        return t.Db().Query("SELECT coalesce(max(itemid), 0) FROM _inventory WHERE charid = $1 AND slot = $2", {p.character, int(place)}).Int(0, 0);
    };
    const auto chestId = [&](std::uint8_t place) {
        return t.Db().Query("SELECT coalesce(max(itemid), 0) FROM _chest WHERE userjid = $1 AND slot = $2", {p.account, int(place)}).Int(0, 0);
    };
    const auto item = [&](std::int64_t id) {
        const auto q = t.Db().Query("SELECT refitemid, data FROM _items WHERE id64 = $1", {id});
        return std::pair<std::int64_t, std::int64_t>{q.Int(0, 0), q.Int(0, 1)};
    };
    const auto gold = [&] {
        return std::pair<std::int64_t, std::int64_t>{t.Db().Query("SELECT remaingold FROM _char WHERE charid = $1", {p.character}).Int(0, 0),
                                                     t.Db().Query("SELECT coalesce(max(gold), 0) FROM _accountjid WHERE jid = $1", {p.account}).Int(0, 0)};
    };
    const auto open = [&] {
        std::uint32_t keeper = 0;
        while (keeper == 0) {
            const auto m = Next<proto::EntityAppeared>(p);
            if (m.character.model == kKeeper) keeper = m.entity;
        }
        p.client->Send(proto::StorageOpen{keeper, 0});
        (void)Next<proto::StorageList>(p);
    };
    const auto move = [&](bool fromStorage, std::uint8_t from, bool toStorage, std::uint8_t to, std::uint16_t quantity = 0) {
        proto::StorageMove m;
        m.fromStorage = fromStorage;
        m.from = from;
        m.toStorage = toStorage;
        m.to = to;
        m.quantity = quantity;
        p.client->Send(m);
        return Next<proto::StorageMoved>(p).status;
    };
    const std::uint8_t scrolls = s.PlaceOf(p, kReturnHigh), booth = s.PlaceOf(p, kBooth);
    const std::int64_t scrollId = bagId(scrolls), boothId = bagId(booth);
    REQUIRE(scrollId != 0);
    REQUIRE(boothId != 0);
    // A deposit costs its keeping fee (at least 1 gold): some gold first.
    p = s.Relog(p, [&] { REQUIRE(t.Db().Query("UPDATE _char SET remaingold = 1000 WHERE charid = $1", {p.character}).Ok()); });
    open();

    // In: the same item in the storage's place, gone from the bag (the original's _STRG_MOVE_ITEM moves one item id).
    CHECK(move(false, booth, true, 0) == proto::ItemStatus::Ok);
    CHECK(settle([&] { return chestId(0) == boothId; }));
    CHECK(bagId(booth) == 0);
    CHECK(item(boothId).first == kBooth);
    // Out again: the same item back.
    CHECK(move(true, 0, false, booth) == proto::ItemStatus::Ok);
    CHECK(settle([&] { return bagId(booth) == boothId; }));
    CHECK(chestId(0) == 0);
    CHECK(item(boothId).first == kBooth);

    // Between the bag and the storage the whole item moves (the original's): onto a taken place it goes to the storage's
    // first free place instead; asked for 4, all 10 come out, the same item.
    CHECK(move(false, booth, true, 0) == proto::ItemStatus::Ok);
    CHECK(move(false, scrolls, true, 0) == proto::ItemStatus::Ok);
    CHECK(settle([&] { return chestId(1) == scrollId; }));
    CHECK(move(true, 1, false, scrolls, 4) == proto::ItemStatus::Ok);
    CHECK(settle([&] { return bagId(scrolls) == scrollId && chestId(1) == 0; }));
    CHECK(item(scrollId) == std::pair<std::int64_t, std::int64_t>{kReturnHigh, 10});
    // Two stacks of them in the storage (split in the bag: the 4 a new item), merged there: the one merged away freed.
    const std::uint8_t spare = 30;
    proto::ItemMove split;
    split.from = scrolls;
    split.to = spare;
    split.quantity = 4;
    p.client->Send(split);
    (void)Next<proto::ItemMoved>(p);
    CHECK(settle([&] { return bagId(spare) != 0; }));
    const std::int64_t part = bagId(spare);
    CHECK(part != scrollId);
    CHECK(move(false, scrolls, true, 2) == proto::ItemStatus::Ok);
    CHECK(move(false, spare, true, 3) == proto::ItemStatus::Ok);
    CHECK(settle([&] { return chestId(2) == scrollId && chestId(3) == part; }));
    // Inside the storage nothing splits (only the inventory does); the 4 onto the 6 merge.
    CHECK(move(true, 2, true, 4, 3) == proto::ItemStatus::BadSlot);
    CHECK(move(true, 3, true, 2) == proto::ItemStatus::Ok);
    CHECK(settle([&] { return chestId(3) == 0 && item(scrollId).second == 10; }));
    CHECK(item(part).first == 0);
    // A quest item no storage takes: 0x1843; an item whose CanDrop has no bit 1 stays in the bag: 0x1838.
    const std::uint8_t letter = s.PlaceOf(p, kNoStorage), keepsake = s.PlaceOf(p, kNoDrop);
    CHECK(move(false, letter, true, 5) == proto::ItemStatus::Failed);
    CHECK(Next<proto::ItemRefused>(p).code == 0x1843);
    p.client->Send(proto::DropItem{keepsake});
    CHECK(Next<proto::ItemRefused>(p).code == 0x1838);
    CHECK(Next<proto::ItemMoved>(p).status == proto::ItemStatus::Failed);

    // The gold: the character's and the storage's written together.
    p = s.Relog(p, [&] { REQUIRE(t.Db().Query("UPDATE _char SET remaingold = 10000 WHERE charid = $1", {p.character}).Ok()); });
    open();
    p.client->Send(proto::StorageGold{true, 3000, 0});
    CHECK(Next<proto::StorageGoldChanged>(p).gold == 3000);
    CHECK(Next<proto::GoldChanged>(p).gold == 7000);
    CHECK(settle([&] { return gold() == std::pair<std::int64_t, std::int64_t>{7000, 3000}; }));
    p.client->Send(proto::StorageGold{false, 1000, 0});
    CHECK(Next<proto::StorageGoldChanged>(p).gold == 2000);
    CHECK(settle([&] { return gold() == std::pair<std::int64_t, std::int64_t>{8000, 2000}; }));
    // One transaction: a storage that cannot be written leaves the character's gold as it was.
    server::ExchangeSaveSide bag;
    bag.character = p.character;
    bag.gold = 1;
    server::StorageSaveSide missing;
    missing.guild = 0x7FFFFFF0;
    missing.gold = 5;
    CHECK_FALSE(server::SaveStorageMove(t.Db(), bag, missing, true));
    CHECK(gold() == std::pair<std::int64_t, std::int64_t>{8000, 2000});
}

TEST_CASE("Game server: a save that does not land ends the session (the original's 'AQ Failed! Log out!!'); the next entry reads the database") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ItemServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Unsaved", {Stored(kBooth, 1)});
    const std::uint8_t booth = s.PlaceOf(p, kBooth);
    const std::uint8_t to = booth == 20 ? 21 : 20;
    // The bag's table away for a moment: the move's save fails, the player is put out.
    REQUIRE(t.Db().Query("ALTER TABLE _inventory RENAME TO _inventory_away").Ok());
    proto::ItemMove m;
    m.from = booth;
    m.to = to;
    p.client->Send(m);
    proto::Disconnected out;
    CHECK(p.client->Expect(out, 10s));
    CHECK(out.reason == proto::DisconnectReason::Kicked);
    REQUIRE(t.Db().Query("ALTER TABLE _inventory_away RENAME TO _inventory").Ok());
    // In again: the bag as the database has it, the item once (where it was, or moved if a retry landed).
    p = s.Relog(p);
    int found = 0;
    for (const proto::ItemRecord& i : p.bag.items) {
        if (i.item != kBooth) continue;
        ++found;
        CHECK((i.slot == booth || i.slot == to));
    }
    CHECK(found == 1);
}