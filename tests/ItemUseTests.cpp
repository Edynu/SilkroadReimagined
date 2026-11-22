// The special items: their rules on the data (the COOLTIME pair, the bonus scrolls' keys,
// the pill bag's cure, the uses the original refuses, the job place's other items), and their uses on a game server:
// fireworks, the berserk potion, the bonus scrolls (one of a key, kept over a relog), the quests' items, the refusals,
// the PvP cape worn as ordinary equipment, the quest's reverse return certificate, the pets' universal pill, the pill
// bag's lock. Set SR_DB_URL (and SR_GAME_DIR for the game files' cases); otherwise skipped.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/ItemCatalogue.h"
#include "sr/game/Jobs.h"
#include "sr/game/Potions.h"
#include "sr/game/SpecialItems.h"
#include "sr/game/States.h"
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

constexpr std::uint32_t kFirework = 930001, kHwan = 930002, kExpScroll = 930003, kSkillScroll = 930004, kStatReset = 930005,
                        kWarehouse = 930006, kSkillPoints = 930007, kArena = 930008, kQuestItem = 930009, kCape = 930010,
                        kCertificate = 930011, kPetPill = 930012, kPillBag = 930013, kCoolFirework = 930014, kIndulgence = 930015;

assets::ItemRow Row(std::uint32_t id, const char* codename, std::array<int, 4> tid) {
    assets::ItemRow r;
    r.refObjId = id;
    r.codename = codename;
    r.typeId = tid;
    r.inService = true;
    r.canUse = tid[1] == 3 ? 1 : 0;
    r.maxStack = tid[1] == 3 ? 50 : 1;
    return r;
}

std::shared_ptr<assets::ItemTable> Items() {
    auto items = std::make_shared<assets::ItemTable>();
    items->Add(Row(kFirework, "ITEM_ETC_FIREWORK_TADPOLE", {3, 3, 6, 1}));
    assets::ItemRow cool = Row(kCoolFirework, "ITEM_ETC_FIREWORK_STAR_SHOWER", {3, 3, 6, 1});
    cool.params[1] = 60000;
    cool.paramTexts[1] = "COOLTIME:0x000000C9";
    items->Add(cool);
    assets::ItemRow hwan = Row(kHwan, "ITEM_EVENT_HWAN_POTION", {3, 3, 1, 8});
    hwan.params[0] = 5;
    items->Add(hwan);
    assets::ItemRow exp = Row(kExpScroll, "ITEM_EVENT_20EXP", {3, 3, 3, 10});
    exp.params = {1200, std::int32_t(game::kBonusExperienceKey), 20, 1};
    exp.paramTexts[1] = "ParamKeeper FourCC";
    items->Add(exp);
    assets::ItemRow skill = Row(kSkillScroll, "ITEM_EVENT_30SP", {3, 3, 3, 10});
    skill.params = {1200, std::int32_t(game::kBonusSkillExperienceKey), 30, 1};
    items->Add(skill);
    items->Add(Row(kStatReset, "ITEM_MALL_STATS_POINT_RECALL", {3, 3, 13, 13}));
    items->Add(Row(kWarehouse, "ITEM_MALL_WAREHOUSE_TICKET", {3, 3, 13, 10}));
    items->Add(Row(kIndulgence, "ITEM_MALL_INDULGENCE", {3, 3, 13, 18}));
    items->Add(Row(kSkillPoints, "ITEM_EVENT_RENT_SKILLPOINT_SCROLL", {3, 3, 3, 12}));
    items->Add(Row(kArena, "ITEM_ETC_BATTLE_ARENA_TICKET", {3, 3, 12, 4}));
    items->Add(Row(kQuestItem, "ITEM_ETC_E050729_SUMMON_SCROLL", {3, 3, 9, 0}));
    assets::ItemRow cape = Row(kCape, "ITEM_CH_M_FRPVP_VOUCHER_A", {3, 1, 7, 5});
    cape.sex = 2;
    cape.params[1] = 1;
    items->Add(cape);
    assets::ItemRow certificate = Row(kCertificate, "ITEM_QNO_WC_FERRY2_1_01", {3, 3, 3, 3});
    certificate.params[0] = 1000;
    items->Add(certificate);
    assets::ItemRow petPill = Row(kPetPill, "ITEM_COS_P_CURE_ALL_01", {3, 3, 2, 7});
    for (int k = 0; k < 6; ++k) petPill.params[std::size_t(k)] = 36;
    items->Add(petPill);
    assets::ItemRow bag = Row(kPillBag, "ITEM_MALL_CURE_SUPERSET_1_BAG", {3, 3, 2, 8});
    bag.params[0] = bag.params[1] = -1;
    bag.paramTexts[0] = "ITEM_ETC_CURE_ALL_02";
    bag.paramTexts[1] = "ITEM_ETC_CURE_RANDOM_01";
    items->Add(bag);
    return items;
}

server::StoredItem Stored(std::uint32_t refId, std::uint16_t count = 1) {
    server::StoredItem i;
    i.refId = refId;
    i.quantity = count;
    i.counted = refId != kCape;
    return i;
}

struct UseServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    explicit UseServer(TestDatabase& db) : t(db) {
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
        config.logoutCountdown = std::chrono::seconds(0);
        config.databaseThreads = 2;
        config.levels.assign(120, 50'000'000);
        config.monsterExperience.assign(120, 1000);
    }
    struct Player {
        std::unique_ptr<Client> client;
        std::string name;
        std::int64_t account = 0, character = 0;
        std::uint32_t entity = 0;
        proto::InventoryList bag;
    };
    // A level-30 Chinese man with `items` in his bag, entered (made the first time).
    Player Make(const std::string& name, const std::vector<server::StoredItem>& items = {}, bool create = true) {
        Player p;
        p.name = name;
        if (create) REQUIRE(server::CreateAccount(t.Db(), name + "acct", "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        p.account = server::FindAccount(t.Db(), name + "acct", ok)->id;
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
    Player Relog(Player& p) {
        p.client->Connection().Close();
        for (int i = 0; i < 500 && t.Db().Query("SELECT count(*) FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {p.account}).Int(0, 0) > 0; ++i) {
            game.Poll(2ms);
        }
        for (int i = 0; i < 100; ++i) game.Poll(2ms);
        REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {p.account}).Ok());
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

// The next answer about `item`: its refusal's code, or 0 when it was used (ItemUsed by its user).
std::uint16_t Answer(UseServer::Player& p, std::uint32_t item) {
    for (int i = 0; i < 400; ++i) {
        proto::ItemRefused refused;
        if (p.client->Expect(refused, 5ms) && refused.item == item) return refused.code;
        proto::ItemUsed used;
        if (p.client->Expect(used, 5ms) && used.item == item && used.entity == p.entity) return 0;
    }
    FAIL("no answer");
    return 0xFFFF;
}
std::uint16_t Use(UseServer& s, UseServer::Player& p, std::uint32_t item, std::uint8_t option = 0) {
    proto::UseItem use;
    use.slot = s.PlaceOf(p, item);
    use.option = option;
    p.client->Send(use);
    return Answer(p, item);
}

} // namespace

TEST_CASE("Special items: their rules on the data") {
    const auto items = Items();
    // The COOLTIME pair: the group in hex, the wait in its own param.
    const game::ItemCoolTime cool = game::CoolTimeOf(*items->Find(kCoolFirework));
    CHECK(cool.group == 0xC9);
    CHECK(cool.ms == 60000);
    CHECK(game::CoolTimeOf(*items->Find(kFirework)).ms == 0);
    // Fireworks, not the shock bomb.
    CHECK(game::IsFirework(*items->Find(kFirework)));
    CHECK_FALSE(game::IsFirework(Row(1, "ITEM_FORT_SHOCK_BOMB", {3, 3, 6, 2})));
    CHECK(game::IsBerserkPotion(*items->Find(kHwan)));
    // The bonus scrolls by key.
    const game::BonusScroll exp = game::BonusScrollOf(*items->Find(kExpScroll));
    CHECK(exp.kind == game::BonusKind::Experience);
    CHECK(exp.value == 20);
    CHECK(exp.seconds == 1200);
    CHECK(game::BonusScrollOf(*items->Find(kSkillScroll)).kind == game::BonusKind::SkillExperience);
    assets::ItemRow sp = *items->Find(kSkillScroll);
    sp.params[1] = std::int32_t(game::kBonusSkillPointsKey);
    CHECK(game::BonusScrollOf(sp).kind == game::BonusKind::SkillExperience);
    sp.params[1] = std::int32_t(game::kBonusBerserkTimeKey);
    CHECK(game::BonusScrollOf(sp).kind == game::BonusKind::BerserkTime);
    sp.params[1] = 12345;
    CHECK_FALSE(game::BonusScrollOf(sp));
    // The uses the original refuses, with its codes.
    CHECK(game::OriginalUseRefusal(*items->Find(kStatReset)) == 2);
    CHECK(game::OriginalUseRefusal(*items->Find(kWarehouse)) == 2);
    CHECK(game::OriginalUseRefusal(*items->Find(kIndulgence)) == 2);
    CHECK(game::OriginalUseRefusal(*items->Find(kSkillPoints)) == 0x183E);
    CHECK(game::OriginalUseRefusal(*items->Find(kArena)) == 0x183E);
    CHECK(game::OriginalUseRefusal(Row(2, "ITEM_PRE_MALL_PET2_SYS_NAME_REMOVE_A", {3, 3, 16, 3})) == 0x183E);
    CHECK(game::OriginalUseRefusal(*items->Find(kFirework)) == 0);
    CHECK(game::OriginalUseRefusal(Row(3, "ITEM_MALL_PREMIUM_GOLDTIME", {3, 3, 13, 14})) == 0);
    // The quests' and events' items.
    CHECK(game::IsQuestUseItem(*items->Find(kQuestItem)));
    CHECK(game::IsQuestUseItem(Row(4, "ITEM_ETC_E060517_MON_GENERATION_BOX", {3, 3, 15, 1})));
    assets::ItemRow carried = Row(5, "ITEM_QNO_CH_SMITH_1", {3, 3, 9, 0});
    carried.canUse = 0;
    CHECK_FALSE(game::IsQuestUseItem(carried));
    // The job place's other items: no league's suit.
    CHECK_FALSE(game::IsJobSuit(*items->Find(kCape)));
    CHECK(game::IsJobSuit(Row(6, "ITEM_CH_M_TRADE_TRADER_02", {3, 1, 7, 1})));
    CHECK_FALSE(game::IsJobSuit(Row(7, "ITEM_CH_M_TRADE_TRADER_04_01", {3, 1, 7, 6})));
    // The pill bag: freeze and frostbite outright (its -1s as 16 bits), nothing else; the universal pill's lock.
    const game::CureSpec cure = game::PillCureOf(*items->Find(kPillBag));
    CHECK(cure.elementMask == (game::StateBit(game::StateKind::Freeze) | game::StateBit(game::StateKind::Frostbite)));
    CHECK(cure.elementLevel == 65535);
    CHECK(game::ReuseLockOf(*items->Find(kPillBag), false).bit == 0x40);
    CHECK(game::ReuseLockOf(*items->Find(kPillBag), false).ms == 1100);
    // The catalogue's routes for them.
    const game::ItemRouteData none;
    const auto route = [&](std::uint32_t id) { return std::string(game::ItemRouteName(game::ItemRouteOf(*items->Find(id), none).route)); };
    CHECK(route(kFirework) == "firework");
    CHECK(route(kHwan) == "berserk-potion");
    CHECK(route(kExpScroll) == "bonus-scroll");
    CHECK(route(kStatReset) == "original-refuses");
    CHECK(route(kWarehouse) == "original-refuses");
    CHECK(route(kSkillPoints) == "original-refuses");
    CHECK(route(kQuestItem) == "quest-use");
    CHECK(route(kCape) == "equip");
    CHECK(route(kCertificate) == "reverse-return");
    CHECK(route(kPetPill) == "pet-cure");
    CHECK(route(kPillBag) == "recovery");
    // No handler takes this one in the original's 1.188 server: refused 2 as it is.
    CHECK(game::ItemStateOf(game::ItemRouteOf(*items->Find(kQuestItem), none)) == game::ItemState::Implemented);
    CHECK(game::ItemStateOf(game::ItemRouteOf(*items->Find(kStatReset), none)) == game::ItemState::Implemented);
}

TEST_CASE("Special items: the pill bag's cure ends freeze and frostbite, leaves the rest") {
    const auto items = Items();
    game::StateSet set;
    game::ChanceRolls rolls;
    std::mt19937 random(7);
    const auto now = game::StateSet::Clock::now();
    const auto state = [&](game::StateKind kind) {
        game::ActiveState a;
        a.kind = kind;
        a.level = 5;
        a.durationMs = 60000;
        a.started = a.nextTick = now;
        return a;
    };
    REQUIRE(set.Apply(state(game::StateKind::Freeze)));
    REQUIRE(set.Apply(state(game::StateKind::Burn)));
    CHECK(set.Cure(game::PillCureOf(*items->Find(kPillBag)), rolls, random, now + 10ms));
    CHECK_FALSE(set.In(game::StateKind::Freeze));
    CHECK(set.In(game::StateKind::Burn));
}

TEST_CASE("Game server: the special items' uses") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    UseServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Special", {Stored(kFirework, 3), Stored(kCoolFirework, 2), Stored(kHwan, 2), Stored(kExpScroll, 2), Stored(kSkillScroll, 1),
                                Stored(kStatReset), Stored(kWarehouse), Stored(kSkillPoints), Stored(kArena), Stored(kQuestItem),
                                Stored(kCape), Stored(kCertificate), Stored(kPetPill), Stored(kPillBag, 3), Stored(kIndulgence)});
    const auto count = [&](std::uint32_t item) {
        return t.Db()
            .Query("SELECT coalesce(sum(i.data), 0) FROM sro_vt_shard._inventory v JOIN sro_vt_shard._items i ON i.id64 = v.itemid "
                   "WHERE v.charid = $1 AND i.refitemid = $2 AND v.slot >= 13",
                   {p.character, std::int64_t(item)})
            .Int(0, 0);
    };

    SUBCASE("fireworks: used up and shown; a COOLTIME pair waits (0x185B)") {
        CHECK(Use(s, p, kFirework) == 0);
        CHECK(Use(s, p, kCoolFirework) == 0);
        proto::Cooldown wait;
        REQUIRE(p.client->Expect(wait));
        CHECK(wait.id == kCoolFirework);
        CHECK(wait.ms == 60000);
        CHECK(Use(s, p, kCoolFirework) == game::kCoolTimeRefused);
        for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
        CHECK(count(kFirework) == 2);
        CHECK(count(kCoolFirework) == 1);
    }
    SUBCASE("the berserk potion: the gauge full; refused in berserk (0x18CE)") {
        CHECK(Use(s, p, kHwan) == 0);
        proto::BerserkPoints points;
        for (int i = 0; i < 10 && points.points != 5; ++i) REQUIRE(p.client->Expect(points));
        CHECK(points.points == 5);
        p.client->Send(proto::BerserkRequest{});
        proto::BerserkAnswer berserk;
        REQUIRE(p.client->Expect(berserk));
        CHECK(berserk.status == proto::BerserkStatus::Ok);
        CHECK(Use(s, p, kHwan) == game::kBerserkPotionRefused);
    }
    SUBCASE("the bonus scrolls: one of a key, another key beside it, kept over a relog with their time") {
        CHECK(Use(s, p, kExpScroll) == 0);
        CHECK(Use(s, p, kExpScroll) == game::kBonusRunning);
        CHECK(Use(s, p, kSkillScroll) == 0);
        for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
        CHECK(t.Db().Query("SELECT count(*) FROM _timedjob WHERE charid = $1 AND category IN (116, 118)", {p.character}).Int(0, 0) == 2);
        CHECK(t.Db().Query("SELECT data3 FROM _timedjob WHERE charid = $1 AND category = 116", {p.character}).Int(0, 0) == 20);
        auto again = s.Relog(p);
        for (int i = 0; i < 200; ++i) s.game.Poll(2ms); // its services read
        CHECK(Use(s, again, kExpScroll) == game::kBonusRunning); // still running after the relog
    }
    SUBCASE("a bonus scroll on the buff board (its time service entry), ended by its player: its key free again (2026-11-22)") {
        CHECK(Use(s, p, kExpScroll) == 0);
        // Told among the time services, by its item, with its time left.
        bool shown = false;
        proto::TimedServices told;
        for (int i = 0; i < 10 && !shown && p.client->Expect(told); ++i) {
            for (const proto::TimedService& service : told.services) {
                if (service.category == game::kTimedBonusFirst && service.item == kExpScroll && service.secondsLeft > 0) shown = true;
            }
        }
        CHECK(shown);
        // Ended: answered, told again without it, and a second scroll of its key taken at once.
        p.client->Send(proto::TimedCancel{game::kTimedBonusFirst});
        proto::ServiceAnswer answer;
        REQUIRE(p.client->Expect(answer));
        CHECK(answer.request == proto::ServiceRequest::Cancel);
        CHECK(answer.code == 0);
        // (Those told before the end may still wait in the inbox: the newest is without it.)
        bool gone = false;
        for (int i = 0; i < 10 && !gone && p.client->Expect(told, 2s); ++i) {
            gone = std::none_of(told.services.begin(), told.services.end(), [](const proto::TimedService& t) { return t.category == game::kTimedBonusFirst; });
        }
        CHECK(gone);
        CHECK(Use(s, p, kExpScroll) == 0);
        // Nothing of that kind running, or not a bonus's category: refused.
        p.client->Send(proto::TimedCancel{game::kTimedBonusFirst + 1});
        REQUIRE(p.client->Expect(answer));
        CHECK(answer.code == 2);
        p.client->Send(proto::TimedCancel{game::kTimedGoldSilver});
        REQUIRE(p.client->Expect(answer));
        CHECK(answer.code == 2);
    }
    SUBCASE("the uses the original refuses: its codes, the items kept") {
        CHECK(Use(s, p, kStatReset) == 2);
        CHECK(Use(s, p, kWarehouse) == 2);
        CHECK(Use(s, p, kIndulgence) == 2);
        CHECK(Use(s, p, kSkillPoints) == 0x183E);
        CHECK(Use(s, p, kArena) == 0x183E);
        // A quest's item no quest of ours takes (the original's 2 when none does).
        CHECK(Use(s, p, kQuestItem) == game::kNoQuestTakesIt);
        for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
        CHECK(count(kStatReset) == 1);
        CHECK(count(kQuestItem) == 1);
    }
    SUBCASE("the PvP cape: worn in the job place at once, as ordinary equipment, and taken off") {
        p.client->Send(proto::ItemMove{s.PlaceOf(p, kCape), game::slot::kJobSuit, 0});
        proto::ItemMoved moved;
        REQUIRE(p.client->Expect(moved));
        CHECK(moved.status == proto::ItemStatus::Ok); // no league, no town, no 10 s change (0x189B)
        CHECK(moved.code == 0);
        p.client->Send(proto::ItemMove{game::slot::kJobSuit, std::uint8_t(game::slot::kWorn + 20), 0});
        REQUIRE(p.client->Expect(moved));
        CHECK(moved.status == proto::ItemStatus::Ok);
    }
    SUBCASE("the quest's certificate is a reverse return scroll; the pet pill wants a COS; the pill bag's lock") {
        CHECK(Use(s, p, kCertificate, proto::kReverseToDeath) == 0x1886); // no place of death: the reverse return's own check
        CHECK(Use(s, p, kPetPill) == 3);                                    // no COS out
        CHECK(Use(s, p, kPillBag) == 0);
        CHECK(Use(s, p, kPillBag) == 0x185B); // its kind locked for 1.1 s
    }
}

TEST_CASE("Game server: a 100 % experience scroll doubles a kill's experience (game files)") {
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
    const assets::ItemRow* scroll = items->Find(std::string_view("ITEM_ETC_INTERNAL_100EXP_SCROLL"));
    REQUIRE(scroll != nullptr);
    REQUIRE(game::BonusScrollOf(*scroll).kind == game::BonusKind::Experience);
    REQUIRE(game::BonusScrollOf(*scroll).value == 100);
    UseServer s(t);
    auto rules = std::const_pointer_cast<game::CharacterRules>(s.config.rules);
    rules->items = items;
    rules->creation.AddWeapon(game::CreationRules::Race::China, sword->refObjId);
    s.config.files = &fs;
    s.config.characters = characters;
    s.config.skills = skills;
    s.config.monsterExperience.assign(120, 100);
    server::MonsterNest nest;
    nest.refObjId = weakest->refObjId;
    nest.position = {assets::RegionId(170, 97), 1030, 0, 1000}; // the field east of Jangan
    nest.radius = 300;
    nest.maxCount = 1;
    s.config.monsterNests = {nest};
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    REQUIRE(server::CreateAccount(t.Db(), "Readeracct", "secret1", error) == server::CreateAccountResult::Created);
    UseServer::Player p;
    p.name = "Reader";
    bool ok = false;
    p.account = server::FindAccount(t.Db(), "Readeracct", ok)->id;
    p.client = std::make_unique<Client>(s.net, s.game.Port(), [&] { s.game.Poll(2ms); });
    REQUIRE(p.client->Greet());
    proto::LobbyEnter e;
    e.ticket = server::IssueTicket(t.Db(), p.account, s.config.id, 30, error);
    p.client->Send(e);
    proto::LobbyEntered entered;
    REQUIRE(p.client->Expect(entered));
    proto::CharacterCreate m;
    m.name = "Reader";
    m.model = 100;
    m.chest = 1;
    m.legs = 2;
    m.shoes = 3;
    m.weapon = sword->refObjId;
    p.client->Send(m);
    proto::LobbyResult r;
    REQUIRE(p.client->Expect(r));
    REQUIRE(r.status == proto::LobbyStatus::Ok);
    p.character = server::ListCharacters(t.Db(), p.account, s.config.id, 0, ok)[0].id;
    REQUIRE(server::SaveProgress(t.Db(), {{p.character, std::uint8_t(weakest->level + 3), 0, 0, 0, 40, 40, 0}}));
    REQUIRE(server::SavePlaces(t.Db(), {{p.character, {assets::RegionId(170, 97), 1000.0f, 0.0f, 1000.0f, 0.0f}}}));
    REQUIRE(server::SaveVitals(t.Db(), p.character, 100'000, 100'000));
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {p.account}).Ok());
    REQUIRE(server::GiveItems(t.Db(), s.config.id, "Reader", {Stored(scroll->refObjId)}, 0, error) == server::GiveResult::Done);
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, $2)", {p.account, int(s.config.id)}).Ok());
    p.client->Send(proto::CharacterSelect{"Reader"});
    REQUIRE(p.client->Expect(r));
    p.client->Send(proto::WorldEnter{});
    proto::WorldEntered w;
    REQUIRE(p.client->Expect(w));
    p.entity = w.entity;
    REQUIRE(p.client->Expect(p.bag));
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
    CHECK(Use(s, p, scroll->refObjId) == 0);
    const auto doubled = kill();
    CHECK(doubled.experience == plain.experience * 2);
    CHECK(doubled.skillExperience == plain.skillExperience); // an experience scroll: the skill experience as it was
}
