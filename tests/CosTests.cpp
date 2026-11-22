// which COS a scroll summons, and the rides and transports through the game server:
// summoned beside their owner, a ride boarded at once, a rider at its mount's speeds with its mount walking its
// legs, one of a kind, Ride and Dismiss, and the owner's death (only the COS ridden goes) (set SR_DB_URL; otherwise skipped).

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <set>
#include <span>
#include <memory>
#include <string>
#include <vector>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/assets/RegionInfo.h"
#include "sr/assets/Shop.h"
#include "sr/core/Jobs.h"
#include "sr/game/Cos.h"
#include "sr/game/CosProtocol.h"
#include "sr/game/JobProtocol.h"
#include "sr/game/Potions.h"
#include "sr/server/GameServer.h"
#include "sr/server/JobStore.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;

namespace {

constexpr std::uint32_t kHorseScroll = 900101, kCartScroll = 900102, kTraderSuit = 900104, kHunterSuit = 900105;
constexpr std::uint32_t kHorse = 900201, kCart = 900202;
constexpr std::uint32_t kFlute = 900106, kRabbitScroll = 900107, kWolf = 900203, kRabbit = 900204, kHerb = 900108;
constexpr std::uint32_t kClock = 900109, kGrass = 900110; // a Clock of Reincarnation (28 days), a Grass of Life
constexpr std::uint32_t kReverseScroll = 900130, kReturnScroll = 900131; // the reverse return scroll's test
constexpr std::uint32_t kHgpPotion = 900111;             // an HGP recovery potion (ours: the whole HGP)
constexpr std::uint32_t kHpKit = 900112;                 // the pets' HP recovery kit (Param1 360, the first one's)

// A pet's summoning item (TID 3/2/1/kind-2: 1 an attack pet, 2 a grab pet), its pet in Desc1, a rental in Param1.
assets::ItemRow PetItem(std::uint32_t id, const char* codename, int type, const char* pet, int rentalMinutes = 0) {
    assets::ItemRow r;
    r.refObjId = id;
    r.codename = codename;
    r.typeId = {3, 2, 1, type};
    r.inService = true;
    r.paramTexts[0] = pet;
    r.params[0] = rentalMinutes;
    return r;
}

// A job's suit (TID 3/1/7/job: 1 trader, 3 hunter).
assets::ItemRow Suit(std::uint32_t id, const char* codename, int job) {
    assets::ItemRow r;
    r.refObjId = id;
    r.codename = codename;
    r.typeId = {3, 1, 7, job};
    r.inService = true;
    r.country = 3;
    return r;
}
constexpr float kHorseRun = 110.0f, kHorseWalk = 30.0f;

assets::ItemRow Scroll(std::uint32_t id, const char* codename, const char* cos, const char* ladder = "") {
    assets::ItemRow r;
    r.refObjId = id;
    r.codename = codename;
    r.typeId = {3, 3, 3, 2};
    r.inService = true;
    r.maxStack = 10;
    r.paramTexts[0] = cos;
    r.paramTexts[1] = ladder;
    return r;
}

assets::ObjectRow Cos(std::uint32_t id, const char* codename, int kind) {
    assets::ObjectRow r;
    r.refObjId = id;
    r.codename = codename;
    r.typeId = {1, 2, 3, kind};
    r.maxHp = 5000;
    r.walkSpeed = kHorseWalk;
    r.runSpeed = kHorseRun;
    r.inService = true;
    return r;
}

struct CosServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    explicit CosServer(TestDatabase& db) : t(db) {
        config.listen = Local();
        config.databaseUrl = t.Url();
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
        rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
        rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
        rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
        auto items = std::make_shared<assets::ItemTable>();
        items->Add(Scroll(kHorseScroll, "ITEM_COS_C_TEST_HORSE", "COS_C_TEST_HORSE"));
        items->Add(Scroll(kCartScroll, "ITEM_COS_T_TEST_CART", "COS_T_TEST_CART"));
        items->Add(Suit(kTraderSuit, "ITEM_CH_M_TRADE_TRADER_TEST", 1));
        items->Add(Suit(kHunterSuit, "ITEM_CH_M_TRADE_HUNTER_TEST", 3));
        items->Add(PetItem(kFlute, "ITEM_COS_P_TEST_FLUTE", 1, "COS_P_TEST_WOLF_001"));
        items->Add(PetItem(kRabbitScroll, "ITEM_COS_P_TEST_RABBIT_SCROLL", 2, "COS_P_TEST_RABBIT", 40320));
        assets::ItemRow herb;
        herb.refObjId = kHerb;
        herb.codename = "ITEM_ETC_TEST_HERB";
        herb.typeId = {3, 3, 1, 1};
        herb.inService = true;
        herb.maxStack = 50;
        herb.sellPrice = 4'000; // the fortress tax's sale
        items->Add(herb);
        assets::ItemRow clock = herb, grass = herb;
        clock.refObjId = kClock;
        clock.codename = "ITEM_COS_P_TEST_EXTENSION";
        clock.typeId = {3, 3, 13, 12};
        clock.params[0] = 40320;
        grass.refObjId = kGrass;
        grass.codename = "ITEM_COS_P_TEST_REVIVAL";
        grass.typeId = {3, 3, 1, 6};
        items->Add(clock);
        items->Add(grass);
        assets::ItemRow hgp = herb;
        hgp.refObjId = kHgpPotion;
        hgp.codename = "ITEM_COS_P_TEST_HGP_POTION";
        hgp.typeId = {3, 3, 1, 9};
        hgp.params[0] = 100;
        items->Add(hgp);
        assets::ItemRow kit = herb;
        kit.refObjId = kHpKit;
        kit.codename = "ITEM_ETC_COS_TEST_HP_POTION";
        kit.typeId = {3, 3, 1, 4};
        kit.params[0] = 360;
        items->Add(kit);
        // A reverse return scroll and a return scroll, each read in a tenth of a second.
        assets::ItemRow reverse = herb, back = herb;
        reverse.refObjId = kReverseScroll;
        reverse.codename = "ITEM_MALL_REVERSE_RETURN_SCROLL";
        reverse.typeId = {3, 3, 3, 3};
        reverse.params[0] = 100;
        back.refObjId = kReturnScroll;
        back.codename = "ITEM_ETC_SCROLL_RETURN_TEST";
        back.typeId = {3, 3, 3, 1};
        back.params[0] = 100;
        back.paramTexts[2] = "RESURRECT";
        items->Add(reverse);
        items->Add(back);
        rules->items = items;
        config.rules = rules;
        auto characters = std::make_shared<assets::CharacterTable>();
        characters->Add(Cos(kHorse, "COS_C_TEST_HORSE", 1));
        characters->Add(Cos(kCart, "COS_T_TEST_CART", 2));
        assets::ObjectRow testWolf = Cos(kWolf, "COS_P_TEST_WOLF_001", 3);
        testWolf.except4 = 1; // 100 HGP a minute: hungry fast
        characters->Add(testWolf);
        characters->Add(Cos(kRabbit, "COS_P_TEST_RABBIT", 4));
        config.characters = characters;
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
    // A character at (x, z) of a region (Jangan's unless given) with `items` in its bag, of `level`, entered.
    // `before` runs on its character before it enters (its league set, ...).
    Player Make(const std::string& name, float x, const std::vector<server::StoredItem>& items, int level = 1, std::uint16_t region = 0, float y = 0,
                float z = 1000, const std::function<void(std::int64_t)>& before = {}) {
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
        here.region = region != 0 ? region : assets::RegionId(168, 97);
        here.x = x;
        here.y = y;
        here.z = z;
        REQUIRE(server::SavePlaces(t.Db(), {{p.character, here}}));
        if (level > 1) {
            REQUIRE(server::SaveProgress(t.Db(), {{p.character, std::uint8_t(level), 0, 0, 0, 40, 40, 0}}));
            REQUIRE(server::SaveVitals(t.Db(), p.character, 100'000, 100'000));
        }
        const std::int64_t jid = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {p.character}).Int(0, 0);
        REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {jid}).Ok());
        if (!items.empty()) REQUIRE(server::GiveItems(t.Db(), config.id, name, items, 0, error) == server::GiveResult::Done);
        REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, $2)", {jid, int(config.id)}).Ok());
        if (before) before(p.character);
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

// Jangan's region a town (the job suit goes on and comes off only in one).
std::shared_ptr<assets::RegionAreaTable> JobTowns() {
    auto towns = std::make_shared<assets::RegionAreaTable>();
    std::string error;
    REQUIRE(towns->Parse("#TOWN\tJangan\n168\t97\tALL\n", error));
    return towns;
}

server::StoredItem Stored(std::uint32_t refId, std::uint16_t count) {
    server::StoredItem i;
    i.refId = refId;
    i.quantity = count;
    i.counted = true;
    return i;
}

// A pet's summoning item as the shop sells one: its _Items.Data 0 (never summoned).
server::StoredItem PetStored(std::uint32_t refId) {
    server::StoredItem i;
    i.refId = refId;
    i.quantity = 1;
    i.counted = false;
    return i;
}

std::uint8_t PlaceOf(const proto::InventoryList& bag, std::uint32_t refId) {
    for (const proto::ItemRecord& i : bag.items) {
        if (i.item == refId) return i.slot;
    }
    FAIL("no such item in the bag");
    return 0;
}

template <typename T>
T Next(CosServer::Player& p) {
    T m;
    REQUIRE(p.client->Expect(m));
    return m;
}

// The next appearance of a COS in `p`'s view.
proto::EntityAppeared NextCos(CosServer::Player& p) {
    for (;;) {
        const auto m = Next<proto::EntityAppeared>(p);
        if (m.kind == proto::EntityKind::Cos) return m;
    }
}

// The next leg of `entity` that `p` hears.
proto::EntityMoved NextLeg(CosServer::Player& p, std::uint32_t entity) {
    for (;;) {
        const auto m = Next<proto::EntityMoved>(p);
        if (m.entity == entity) return m;
    }
}

} // namespace

TEST_CASE("COS: a scroll's COS, by its level ladder") {
    const assets::ItemRow plain = Scroll(1, "ITEM_COS_C_HORSE1", "COS_C_HORSE1");
    CHECK(game::IsCosScroll(plain));
    CHECK(game::CosOfScroll(plain, 1) == "COS_C_HORSE1");
    const assets::ItemRow ladder = Scroll(2, "ITEM_COS_C_OSTRICH_SCROLL", "COS_C_OSTRICH", "5,10,20,30,45,60,75,90,105,120");
    CHECK_FALSE(game::CosOfScroll(ladder, 4));
    CHECK(game::CosOfScroll(ladder, 5) == "COS_C_OSTRICH_5");
    CHECK(game::CosOfScroll(ladder, 59) == "COS_C_OSTRICH_45");
    CHECK(game::CosOfScroll(ladder, 120) == "COS_C_OSTRICH_120");
    assets::ItemRow notScroll = plain;
    notScroll.typeId = {3, 3, 3, 1};
    CHECK_FALSE(game::IsCosScroll(notScroll));
    CHECK_FALSE(game::CosOfScroll(notScroll, 50));
    // The kinds by characterdata's TID4.
    CHECK(game::CosKindOf(Cos(1, "COS_C_HORSE1", 1)) == game::CosKind::Ride);
    CHECK(game::CosKindOf(Cos(1, "COS_T_HORSE1", 2)) == game::CosKind::Transport);
    CHECK(game::CosKindOf(Cos(1, "COS_P_WOLF_01", 3)) == game::CosKind::AttackPet);
    assets::ObjectRow monster = Cos(1, "MOB_CH_TIGER", 1);
    monster.typeId = {1, 2, 1, 1};
    CHECK(game::CosKindOf(monster) == game::CosKind::None);
    CHECK(game::CosPlaceOf(game::CosKind::Ride) == game::CosPlaceOf(game::CosKind::Transport));
}

TEST_CASE("COS: an attack pet's numbers on its Basic Info page, as the 1.188 client works them out") {
    assets::ObjectRow wolf = Cos(1, "COS_P_WOLF_006", 3);
    wolf.phyDefence = 41;
    wolf.magDefence = 33;
    wolf.hitRate = 57;
    wolf.evasion = 25;
    wolf.phyParry = 99; // not the window's parry
    assets::SkillRow none; // a buff: no attack
    assets::SkillRow bite;
    bite.attack = true;
    bite.attackKind = 5;
    bite.attackMin = 30;
    bite.attackMax = 45;
    assets::SkillRow secondBite = bite;
    secondBite.attackMin = 900;
    assets::SkillRow howl;
    howl.attack = true;
    howl.attackKind = 10;
    howl.attackMin = 20;
    howl.attackMax = 27;
    const std::array<const assets::SkillRow*, 5> skills{nullptr, &none, &bite, &secondBite, &howl};
    // Fed: the first physical and the first magical attack, the row's numbers as they are.
    const game::PetStats fed = game::PetStatsOf(wolf, skills, game::kCosHgpFull);
    CHECK_FALSE(fed.hungry);
    CHECK(fed.phyMin == 30);
    CHECK(fed.phyMax == 45);
    CHECK(fed.magMin == 20);
    CHECK(fed.magMax == 27);
    CHECK(fed.phyDefence == 41);
    CHECK(fed.magDefence == 33);
    CHECK(fed.hitRate == 57);
    CHECK(fed.parry == 25);
    // Hungry (30% HGP or less): every number halved.
    const game::PetStats hungry = game::PetStatsOf(wolf, skills, game::kCosHungry);
    CHECK(hungry.hungry);
    CHECK(hungry.phyMin == 15);
    CHECK(hungry.phyMax == 22);
    CHECK(hungry.magMax == 13);
    CHECK(hungry.phyDefence == 20);
    CHECK(hungry.parry == 12);
    CHECK_FALSE(game::PetStatsOf(wolf, skills, game::kCosHungry + 1).hungry);
    // No attack among its skills: nothing to show but zeros.
    const game::PetStats plain = game::PetStatsOf(wolf, std::span<const assets::SkillRow* const>{}, game::kCosHgpFull);
    CHECK(plain.phyMax == 0);
    CHECK(plain.magMax == 0);
}

TEST_CASE("COS: an attack pet's experience and levels, as") {
    const std::vector<std::uint64_t> need = {0, 100, 200, 300, 400, 500}; // level 1 needs 100 to 2, ...
    // Below its owner's level: a level each time the need is covered, the rest carried.
    game::PetGrowth g = game::GrowPet(1, 50, 400, 10, need);
    CHECK(g.level == 3);
    CHECK(g.experience == 150);
    CHECK(g.levelsGained == 2);
    // At its owner's level: full, no further.
    g = game::GrowPet(1, 50, 400, 2, need);
    CHECK(g.level == 2);
    CHECK(g.experience == 200);
    g = game::GrowPet(3, 0, 1000, 3, need);
    CHECK(g.level == 3);
    CHECK(g.experience == 300);
    CHECK(g.levelsGained == 0);
    // Past the table (level 6 has no need in it): no further either (its experience as it is).
    g = game::GrowPet(6, 0, 10'000, 50, need);
    CHECK(g.level == 6);
    CHECK(g.experience == 10'000);
    // At 110: no further, whatever its owner's level.
    std::vector<std::uint64_t> long_(150, 10);
    CHECK(game::GrowPet(110, 0, 1000, 120, long_).level == 110);
    CHECK(game::GrowPet(108, 0, 1000, 120, long_).level == 110);
    // Its hunger: 100 HGP every Except_4 minutes; the potion's Param1 percent of the full.
    assets::ObjectRow wolf;
    wolf.except4 = 5;
    CHECK(game::PetHgpPerSecond(wolf) == doctest::Approx(100.0 / 300.0));
    wolf.except4 = 0;
    CHECK(game::PetHgpPerSecond(wolf) == 0.0);
    assets::ItemRow potion;
    potion.typeId = {3, 3, 1, 9};
    potion.params[0] = 10;
    CHECK(game::IsPetHgpPotion(potion));
    CHECK(game::PetHgpPotionAmount(potion) == 1000);
    // The pets' HP recovery kit: a flat potion worked out for its user, locked 1.1 s whatever the user's race.
    assets::ItemRow kit;
    kit.typeId = {3, 3, 1, 4};
    kit.params[0] = 360;
    CHECK(game::IsPetHpPotion(kit));
    CHECK_FALSE(game::IsPetHpPotion(potion));
    CHECK(game::ReuseLockOf(kit, true).bit == 0x08);
    CHECK(game::ReuseLockOf(kit, true).ms == 1100);
    CHECK(game::RecoveryOf(kit, 1, 0.0, 0.0, 0, 0).hp == 360);
    // Its death penalty: the half of 2 % of its level's experience, at most its monster
    // experience x 100; by a player 0.4 % and x 20; its owner a murderer 6 % and x 300; none when the half is under 1.
    CHECK(game::PetDeathPenalty(1'000'000, 500) == 9'999u);   // 19,999 halved, under 50,000
    CHECK(game::PetDeathPenalty(10'000'000, 500) == 50'000u); // 99,999 over the cap
    CHECK(game::PetDeathPenalty(1'000'000, 500, true) == 2'000u);
    CHECK(game::PetDeathPenalty(1'000'000, 500, false, true) == 29'999u);
    CHECK(game::PetDeathPenalty(1'000'000, 10, true, true) == 1'000u); // 2 % halved, over 10 x 100
    CHECK(game::PetDeathPenalty(100, 500) == 0u);  // 1 halved: none
    CHECK(game::PetDeathPenalty(1'000, 0) == 1u);  // the cap at least 1
    CHECK(game::PetDeathPenalty(10'000, 100) == 99u);
    // The rows of its levels.
    CHECK(game::PetCodenameAtLevel("COS_P_WOLF_001", 2) == "COS_P_WOLF_002");
    CHECK(game::PetCodenameAtLevel("COS_P_WOLF_099", 110) == "COS_P_WOLF_110");
    CHECK(game::PetCodenameAtLevel("COS_P_RABBIT", 2).empty());
}

TEST_CASE("Game server: an attack pet fights a monster: its hits, the monster's back, the kill's experience and levels; it dies (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const char* dirEnv = std::getenv("SR_GAME_DIR");
    if (dirEnv == nullptr || *dirEnv == 0) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirEnv, error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    auto items = std::make_shared<assets::ItemTable>();
    REQUIRE_MESSAGE(items->Load(fs, error), error);
    const assets::ItemRow* flute = items->Find(std::string_view("ITEM_COS_P_FLUTE"));
    const assets::ObjectRow* wolf1 = characters->Find(std::string_view("COS_P_WOLF_001"));
    REQUIRE(flute != nullptr);
    REQUIRE(wolf1 != nullptr);
    // A monster of the first level that fights (a level-1 wolf's match), with health for a few of its hits.
    const assets::ObjectRow* foe = nullptr;
    for (const auto& [code, r] : characters->rows()) {
        if (r.typeId[1] != 2 || r.typeId[2] != 1 || r.level < 1 || r.level > 8 || r.rarity != 0 || r.maxHp < 30 || r.experience == 0) continue;
        if (!code.starts_with("MOB_") || code.find("_NPC") != std::string::npos || code.find("THIEF") != std::string::npos) continue;
        const assets::SkillRow* attack = r.skills[0] != 0 ? skills->Find(r.skills[0]) : nullptr;
        if (attack == nullptr || !attack->attack || attack->range > 100) continue;
        if (foe == nullptr || r.level < foe->level || (r.level == foe->level && r.maxHp < foe->maxHp)) foe = &r;
    }
    REQUIRE(foe != nullptr);
    MESSAGE("the monster: " << foe->codename << ", level " << foe->level << ", " << foe->maxHp << " HP");
    CosServer s(t);
    auto rules = std::make_shared<game::CharacterRules>();
    for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
    rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
    rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
    rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
    rules->items = items;
    s.config.rules = rules;
    s.config.files = &fs;
    s.config.characters = characters;
    s.config.skills = skills;
    s.config.levels.assign(150, 5); // a few experience a level: a kill raises it (to 2: level 2 needs more, for the death penalty)
    s.config.levels[2] = 10'000'000;
    s.config.monsterExperience.assign(150, 100);
    server::MonsterNest nest;
    nest.refObjId = foe->refObjId;
    nest.position = {assets::RegionId(76, 105), 640, 140, 900};
    nest.radius = 300;
    nest.delayMin = nest.delayMax = 1;
    s.config.monsterNests = {nest};
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    // Its owner level 20, beside the nest in the field west of Constantinople (the town itself, 77..81 x 103..107, is
    // safe ground: no monster comes in).
    auto a = s.Make("Tamer", 600, {PetStored(flute->refObjId)}, 20, assets::RegionId(76, 105), 140, 900);
    const auto nextMonster = [&](std::uint32_t not_) {
        for (int i = 0; i < 60; ++i) {
            proto::EntityAppeared seen;
            if (a.client->Expect(seen, 1s) && seen.kind == proto::EntityKind::Monster && seen.entity != not_) return seen.entity;
        }
        return std::uint32_t(0);
    };
    const std::uint32_t monster = nextMonster(0);
    REQUIRE(monster != 0);
    a.client->Send(proto::UseItem{PlaceOf(a.bag, flute->refObjId)});
    const auto wolf = Next<proto::CosSummoned>(a);
    REQUIRE(wolf.kind == std::uint8_t(game::CosKind::AttackPet));
    CHECK(wolf.model == wolf1->refObjId);

    // Told to attack: it swings, its hits land (the monster's health going down by each), the monster turns on it.
    a.client->Send(proto::CosCommand{wolf.entity, proto::CosOrder::Attack, monster});
    bool swung = false, struck = false, hitBack = false;
    std::uint32_t lastHp = 0;
    proto::CosExperience gained;
    for (int i = 0; i < 400 && gained.gained == 0; ++i) {
        proto::EntitySwing swing;
        proto::EntityDamaged hit;
        if (a.client->Expect(swing, 20ms) && swing.attacker == wolf.entity) swung = true;
        while (a.client->Expect(hit, 10ms)) {
            if (hit.attacker == wolf.entity && hit.target == monster) {
                if (lastHp != 0) CHECK(hit.hp == (hit.damage >= lastHp ? 0 : lastHp - hit.damage));
                lastHp = hit.hp;
                struck = struck || hit.damage > 0;
                MESSAGE("the wolf hits for " << hit.damage << ", the monster at " << hit.hp);
            }
            if (hit.target == wolf.entity && hit.attacker == monster) {
                hitBack = true;
                MESSAGE("the monster hits the wolf for " << hit.damage << ", the wolf at " << hit.hp);
            }
        }
        a.client->Expect(gained, 10ms);
    }
    CHECK(swung);
    CHECK(struck);
    MESSAGE("the monster hit the wolf back: " << hitBack);
    // The kill: its experience, its levels (no higher than its owner's), its new row.
    REQUIRE(gained.gained > 0);
    CHECK(gained.entity == wolf.entity);
    const auto risen = Next<proto::CosSummoned>(a);
    CHECK(risen.entity == wolf.entity);
    CHECK(risen.level > 1);
    CHECK(risen.level <= 20);
    const assets::ObjectRow* row = characters->Find(std::string_view(game::PetCodenameAtLevel("COS_P_WOLF_001", risen.level)));
    REQUIRE(row != nullptr);
    CHECK(risen.model == row->refObjId);
    CHECK(risen.hp == row->maxHp);
    const std::int64_t petRow = t.Db().Query("SELECT id FROM _charcos WHERE ownercharid = $1", {a.character}).Int(0, 0);
    const auto rowLevel = [&] { return t.Db().Query("SELECT lvl FROM _charcos WHERE id = $1", {petRow}).Int(0, 0); };
    for (int i = 0; i < 300 && rowLevel() != risen.level; ++i) s.game.Poll(10ms);
    CHECK(rowLevel() == risen.level);

    // At 1 HP in its level-1 shape (its level 2 kept, 50,000 experience into it): on the next monster it dies (its row
    // Dead, back into its item), its experience less its death penalty (10,000,000 x 2 % halved, 99,999, over the cap of
    // the monster experience 100 x 100: 10,000).
    CHECK(risen.level == 2);
    a.client->Send(proto::CosDismiss{wolf.entity});
    CHECK(Next<proto::CosDismissed>(a).entity == wolf.entity);
    for (int i = 0; i < 300 && t.Db().Query("SELECT state FROM _charcos WHERE id = $1", {petRow}).Int(0, 0) != 1; ++i) s.game.Poll(10ms);
    REQUIRE(t.Db().Query("UPDATE _charcos SET hp = 1, lvl = 2, refcharid = $2, expoffset = 50000 WHERE id = $1", {petRow, std::int64_t(wolf1->refObjId)}).Ok());
    const std::uint32_t second = nextMonster(monster);
    REQUIRE(second != 0);
    a.client->Send(proto::UseItem{PlaceOf(a.bag, flute->refObjId)});
    const auto weak = Next<proto::CosSummoned>(a);
    CHECK(weak.hp == 1);
    a.client->Send(proto::CosCommand{weak.entity, proto::CosOrder::Attack, second});
    bool died = false;
    for (int i = 0; i < 600 && !died; ++i) {
        proto::EntityDamaged hit;
        if (a.client->Expect(hit, 50ms) && hit.target == weak.entity && (hit.flags & proto::kDamageKilled) != 0) died = true;
    }
    REQUIRE(died);
    CHECK(Next<proto::CosDismissed>(a).entity == weak.entity);
    for (int i = 0; i < 300 && t.Db().Query("SELECT state FROM _charcos WHERE id = $1", {petRow}).Int(0, 0) != 4; ++i) s.game.Poll(10ms);
    CHECK(t.Db().Query("SELECT state FROM _charcos WHERE id = $1", {petRow}).Int(0, 0) == std::int64_t(game::CosState::Dead));
    CHECK(t.Db().Query("SELECT expoffset FROM _charcos WHERE id = $1", {petRow}).Int(0, 0) == 50'000 - 10'000);
    CHECK(t.Db().Query("SELECT lvl FROM _charcos WHERE id = $1", {petRow}).Int(0, 0) == 2);
    // Dead, its item summons it no more.
    a.client->Send(proto::UseItem{PlaceOf(a.bag, flute->refObjId)});
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COSPETERR_CANT_USEITEM");
    s.game.Stop();
}

TEST_CASE("Game server: a ride summoned, ridden, left and sent away") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Rider", 1000, {Stored(kHorseScroll, 2), Stored(kCartScroll, 1)});
    auto b = s.Make("Watcher", 1100, {});

    // The scroll: used up, the horse beside its owner, boarded at once.
    a.client->Send(proto::UseItem{PlaceOf(a.bag, kHorseScroll)});
    const auto summoned = Next<proto::CosSummoned>(a);
    CHECK(summoned.model == kHorse);
    CHECK(summoned.kind == std::uint8_t(game::CosKind::Ride));
    CHECK(summoned.maxHp == 5000);
    CHECK(summoned.item == kHorseScroll);
    const auto used = Next<proto::ItemMoved>(a);
    REQUIRE(used.changed.size() == 1);
    CHECK(used.changed[0].quantity == 1);
    const auto riding = Next<proto::CosRiding>(a);
    CHECK(riding.rider == a.entity);
    CHECK(riding.mount == summoned.entity);
    CHECK(riding.riding);
    // Its neighbour sees the horse as its owner's COS, and who rides it.
    const auto seen = NextCos(b);
    CHECK(seen.entity == summoned.entity);
    CHECK(seen.amount == a.entity);
    CHECK(seen.character.model == kHorse);
    CHECK(Next<proto::CosRiding>(b).mount == summoned.entity);

    // A rider goes at its mount's run speed, and its mount walks the same leg.
    a.client->Send(proto::MoveTo{{assets::RegionId(168, 97), 1300, 0, 1000}});
    const auto leg = NextLeg(a, a.entity);
    CHECK(leg.speed == doctest::Approx(kHorseRun));
    const auto mountLeg = NextLeg(b, summoned.entity);
    CHECK(mountLeg.speed == doctest::Approx(kHorseRun));
    CHECK(mountLeg.to.x == doctest::Approx(leg.to.x));

    // One of a kind: a second ride, or a transport, is refused (and the scroll kept).
    a.client->Send(proto::UseItem{PlaceOf(a.bag, kHorseScroll)});
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COS_CANT_SUMMON_DUPLICATE_PETTYPE");
    a.client->Send(proto::UseItem{PlaceOf(a.bag, kCartScroll)}); // and a transport is no job for one without a trader's or a thief's suit
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_STRGERR_YOU_CANT_ACTIVATE_CART");

    // A rider attacks nobody: the request is dropped (no swing).
    a.client->Send(proto::AttackTarget{b.entity});

    // Someone else's COS takes no orders.
    b.client->Send(proto::CosDismiss{summoned.entity});
    b.client->Send(proto::CosRide{summoned.entity, false});

    // Off: a ride is not left standing, getting off ends it; on foot again, at a player's speed.
    a.client->Send(proto::CosRide{summoned.entity, false});
    CHECK_FALSE(Next<proto::CosRiding>(a).riding);
    CHECK_FALSE(Next<proto::CosRiding>(b).riding);
    CHECK(Next<proto::CosDismissed>(a).entity == summoned.entity);
    for (;;) {
        if (Next<proto::EntityGone>(b).entity == summoned.entity) break;
    }
    a.client->Send(proto::MoveTo{{assets::RegionId(168, 97), 1000, 0, 1000}});
    CHECK(NextLeg(a, a.entity).speed == doctest::Approx(50.0f));

    // A new one, sent away while ridden: gone the same way.
    a.client->Send(proto::UseItem{PlaceOf(a.bag, kHorseScroll)});
    const auto second = Next<proto::CosSummoned>(a);
    CHECK(Next<proto::CosRiding>(a).riding);
    a.client->Send(proto::CosDismiss{second.entity});
    CHECK_FALSE(Next<proto::CosRiding>(a).riding);
    CHECK(Next<proto::CosDismissed>(a).entity == second.entity);
    s.game.Stop();
}

TEST_CASE("Game server: a transport: a trader's, boarded when summoned, left and boarded through Ride, leashed, kept through its owner's death beside it") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    // Its suit worn from the start (put on through its league's rules elsewhere: "a job suit's rules").
    const auto wearing = [&](std::uint32_t suit) {
        return [&t, suit](std::int64_t character) {
            REQUIRE(t.Db().Query("INSERT INTO _inventory (charid, slot, itemid) SELECT $1, 8, b.itemid FROM _inventory b JOIN _items it ON it.id64 = b.itemid "
                                 "WHERE b.charid = $1 AND it.refitemid = $2 LIMIT 1 ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                                 {character, std::int64_t(suit)})
                        .Ok());
            REQUIRE(t.Db().Query("UPDATE _inventory b SET itemid = 0 FROM _items it WHERE it.id64 = b.itemid AND b.charid = $1 AND it.refitemid = $2 AND b.slot <> 8",
                                 {character, std::int64_t(suit)})
                        .Ok());
        };
    };
    // Without a job's suit, and in a hunter's: no transport (the original's texts).
    auto walker = s.Make("Walker", 1100, {Stored(kCartScroll, 1)});
    walker.client->Send(proto::UseItem{PlaceOf(walker.bag, kCartScroll)});
    CHECK(Next<proto::CosRefused>(walker).text == "UIIT_MSG_STRGERR_YOU_CANT_ACTIVATE_CART");
    auto hunter = s.Make("Hunter", 1200, {Stored(kCartScroll, 1), Stored(kHunterSuit, 1)}, 1, 0, 0, 1000, wearing(kHunterSuit));
    hunter.client->Send(proto::UseItem{PlaceOf(hunter.bag, kCartScroll)});
    CHECK(Next<proto::CosRefused>(hunter).text == "UIIT_MSG_STRGERR_YOU_CANT_ACTIVATE_CART");
    // A trader's: summoned.
    auto a = s.Make("Trader", 1000, {Stored(kCartScroll, 2), Stored(kTraderSuit, 1)}, 1, 0, 0, 1000, wearing(kTraderSuit));
    a.client->Send(proto::UseItem{PlaceOf(a.bag, kCartScroll)});
    const auto summoned = Next<proto::CosSummoned>(a);
    CHECK(summoned.kind == std::uint8_t(game::CosKind::Transport));
    CHECK(NextCos(a).entity == summoned.entity); // its owner sees it too
    // Boarded at once, as a ride is.
    const auto riding = Next<proto::CosRiding>(a);
    CHECK(riding.riding);
    CHECK(riding.mount == summoned.entity);
    // Left: it stays (only a transport is left and boarded again), and steps aside.
    a.client->Send(proto::CosRide{summoned.entity, false});
    CHECK_FALSE(Next<proto::CosRiding>(a).riding);
    bool stepped = false;
    for (int i = 0; i < 5 && !stepped; ++i) {
        const auto leg = NextLeg(a, summoned.entity);
        stepped = std::abs(leg.to.x - leg.from.x) + std::abs(leg.to.z - leg.from.z) > 1.0f;
    }
    CHECK(stepped);
    // Boarded again (it stays beside its owner), and left again.
    a.client->Send(proto::CosRide{summoned.entity, true});
    CHECK(Next<proto::CosRiding>(a).riding);
    a.client->Send(proto::CosRide{summoned.entity, false});
    CHECK_FALSE(Next<proto::CosRiding>(a).riding);

    // Left standing, it leashes its owner: no walk to beyond 100 m of it; a nearer one goes.
    a.client->Send(proto::MoveTo{{assets::RegionId(168, 97), 1000, 0, 1000 + 1200}});
    const auto far = Next<proto::CosRefused>(a);
    CHECK(far.text == "UIIT_MSG_COSERR_TOO_FAR_FROM_TRADECART");
    CHECK(far.arg == "100");
    a.client->Send(proto::MoveTo{{assets::RegionId(168, 97), 1000, 0, 1000 + 500}});
    CHECK(NextLeg(a, a.entity).to.z == doctest::Approx(1500.0f));
    // It follows at its run pace.
    for (;;) {
        const auto leg = NextLeg(a, summoned.entity);
        if (leg.speed > 0.0f) {
            CHECK(leg.speed == doctest::Approx(kHorseRun));
            break;
        }
    }

    // Its owner's death: walking beside it, not riding it, it stays (the original's death takes only the COS ridden).
    a.client->Send(proto::DevDie{});
    proto::CosDismissed gone;
    CHECK_FALSE(a.client->Expect(gone, 1500ms));
    s.game.Stop();
}

TEST_CASE("Game server: pets: their rows made at the first summon, kept through unsummons, back in their items when unsummoned") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Tamer", 1000, {PetStored(kFlute), PetStored(kRabbitScroll), Stored(kClock, 1), Stored(kGrass, 1), Stored(kHgpPotion, 2), Stored(kHpKit, 3)});
    auto b = s.Make("Watcher", 1100, {});
    const std::uint8_t flute = PlaceOf(a.bag, kFlute), rabbit = PlaceOf(a.bag, kRabbitScroll);
    const std::uint8_t clock = PlaceOf(a.bag, kClock), grass = PlaceOf(a.bag, kGrass);
    const auto held = [&](std::uint32_t refId) {
        return t.Db().Query("SELECT count(*) FROM _inventory i JOIN _items it ON it.id64 = i.itemid WHERE i.charid = $1 AND it.refitemid = $2",
                            {a.character, std::int64_t(refId)}).Int(0, 0);
    };
    // The database as the server's saves leave it (they are written on its own threads).
    const auto eventually = [&](const std::function<bool()>& holds) {
        for (int i = 0; i < 500 && !holds(); ++i) s.game.Poll(10ms);
        return holds();
    };
    const auto itemData = [&](std::uint32_t refId) {
        return t.Db().Query("SELECT it.data FROM _inventory i JOIN _items it ON it.id64 = i.itemid WHERE i.charid = $1 AND it.refitemid = $2",
                            {a.character, std::int64_t(refId)}).Int(0, 0);
    };
    const auto state = [&](std::int64_t row) { return t.Db().Query("SELECT state FROM _charcos WHERE id = $1", {row}).Int(0, 0); };

    // The attack pet's first summon: its row made (level 1, full HP and HGP), linked from its item, which stays.
    a.client->Send(proto::UseItem{flute});
    const auto wolf = Next<proto::CosSummoned>(a);
    CHECK(wolf.kind == std::uint8_t(game::CosKind::AttackPet));
    CHECK(wolf.model == kWolf);
    CHECK(wolf.level == 1);
    CHECK(wolf.hgp == game::kCosHgpFull);
    CHECK(wolf.hp == 5000);
    CHECK(wolf.experience == 0);
    CHECK(wolf.rentSeconds == 0);
    REQUIRE(eventually([&] { return itemData(kFlute) != 0; }));
    const std::int64_t wolfRow = itemData(kFlute);
    CHECK(eventually([&] { return state(wolfRow) == std::int64_t(game::CosState::Summoned); }));
    CHECK(t.Db().Query("SELECT ownercharid FROM _charcos WHERE id = $1", {wolfRow}).Int(0, 0) == a.character);

    // One attack pet at a time; a grab pet beside it, its rental running from its first summon (28 days).
    a.client->Send(proto::UseItem{flute});
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COS_CANT_SUMMON_DUPLICATE_PETTYPE");
    a.client->Send(proto::UseItem{rabbit});
    const auto bunny = Next<proto::CosSummoned>(a);
    CHECK(bunny.kind == std::uint8_t(game::CosKind::GrabPet));
    CHECK(bunny.rentSeconds > 27u * 86400u); // its rental left, for its window's Rental period
    CHECK(bunny.rentSeconds <= 28u * 86400u);
    REQUIRE(eventually([&] { return itemData(kRabbitScroll) != 0; }));
    const std::int64_t days = t.Db().Query("SELECT extract(epoch FROM (rentendtime - localtimestamp))::bigint / 86400 FROM _charcos WHERE id = $1",
                                           {itemData(kRabbitScroll)}).Int(0, 0);
    CHECK(days >= 27);
    CHECK(days <= 28);

    // Naming: a name the rules allow, no other pet's, once for good. (Its watcher sees the wolf, still unnamed, first.)
    for (;;) {
        const auto seen = NextCos(b);
        if (seen.entity != wolf.entity) continue;
        CHECK(seen.character.name.empty());
        CHECK(seen.amount == a.entity);
        break;
    }
    a.client->Send(proto::CosRename{wolf.entity, "F"});
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COSPETERR_PETNAME_NOTPUT");
    a.client->Send(proto::CosRename{wolf.entity, "Fang"});
    CHECK(Next<proto::CosRenamed>(a).name == "Fang");
    // Its name told to those who see it, and kept for newcomers (shown under its owner's name over it).
    CHECK(Next<proto::CosRenamed>(b).name == "Fang");
    auto c = s.Make("Newcomer", 1200, {});
    for (;;) {
        const auto seen = NextCos(c);
        if (seen.entity != wolf.entity) continue;
        CHECK(seen.character.name == "Fang");
        CHECK(seen.amount == a.entity);
        break;
    }
    // What its item says, the news of row `id` that `holds` (earlier news of it, kept unread, passed over).
    const auto petNews = [&](std::int64_t id, const std::function<bool(const proto::CosPetItem&)>& holds) {
        for (int i = 0; i < 20; ++i) {
            const auto m = Next<proto::CosPetItems>(a);
            for (const proto::CosPetItem& p : m.pets) {
                if (p.id == id && holds(p)) return true;
            }
        }
        return false;
    };
    CHECK(petNews(wolfRow, [](const proto::CosPetItem& p) { return p.name == "Fang" && p.state == std::uint8_t(game::CosState::Summoned); }));
    CHECK(t.Db().Query("SELECT charname FROM _charcos WHERE id = $1", {wolfRow}).Text(0, 0) == "Fang");
    a.client->Send(proto::CosRename{wolf.entity, "Fang2"});
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COSPET_NOTREVISION_NAME");
    a.client->Send(proto::CosRename{bunny.entity, "fang"});
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COSPETERR_PETNAME_SUMENESS");

    // Its pet out, the item stays in the bag: not dropped, sold or stored (the original's texts).
    a.client->Send(proto::DropItem{flute});
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COSPETERR_CANT_TRADE");
    a.client->Send(proto::ShopSell{0, flute, 1});
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COSPETERR_CANT_SELL_ACTIVATED_COSITEM");
    proto::StorageMove store;
    store.toStorage = true;
    store.from = flute;
    a.client->Send(store);
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COSPETERR_CANT_MOVETO_CHEST2");
    CHECK(itemData(kFlute) == wolfRow);

    // A grab pet's rental over while it is out: back in its item, dead; its item summons it no more.
    const std::int64_t bunnyRow = itemData(kRabbitScroll);
    a.client->Send(proto::CosDismiss{bunny.entity});
    CHECK(Next<proto::CosDismissed>(a).entity == bunny.entity);
    CHECK(eventually([&] { return state(bunnyRow) == std::int64_t(game::CosState::Inactive); }));
    t.Db().Query("UPDATE _charcos SET rentendtime = localtimestamp + interval '2 seconds' WHERE id = $1", {bunnyRow});
    a.client->Send(proto::UseItem{rabbit});
    const auto lastDays = Next<proto::CosSummoned>(a);
    CHECK(lastDays.rentSeconds <= 3u); // 2 s and the timestamp's fraction
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_STT_COSNEWUI_ITEM_EXPIRERENT");
    CHECK(Next<proto::CosDismissed>(a).entity == lastDays.entity);
    CHECK(petNews(bunnyRow, [](const proto::CosPetItem& p) { return p.state == std::uint8_t(game::CosState::Dead); })); // its item says so
    CHECK(eventually([&] { return state(bunnyRow) == std::int64_t(game::CosState::Dead); }));
    a.client->Send(proto::UseItem{rabbit});
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COSPETERR_CANT_USEITEM");

    // A Clock of Reincarnation dropped on its item: 28 days more from now, summoned again; on the wolf's item: the
    // wrong object. A Grass of Life on the wolf, alive: refused.
    a.client->Send(proto::CosUseOnPet{clock, flute});
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COSPETERR_CANT_USE_WRONGOBJECT");
    a.client->Send(proto::CosUseOnPet{clock, rabbit});
    CHECK(petNews(bunnyRow, [](const proto::CosPetItem& p) { return p.state == std::uint8_t(game::CosState::Inactive) && p.rentSeconds > 27u * 86400u; }));
    CHECK(eventually([&] { return state(bunnyRow) == std::int64_t(game::CosState::Inactive) && held(kClock) == 0; }));
    a.client->Send(proto::UseItem{rabbit});
    const auto renewed = Next<proto::CosSummoned>(a);
    CHECK(renewed.rentSeconds > 27u * 86400u);
    a.client->Send(proto::CosDismiss{renewed.entity});
    CHECK(Next<proto::CosDismissed>(a).entity == renewed.entity);
    a.client->Send(proto::CosUseOnPet{grass, flute});
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COSPETERR_CANT_USEITEM");

    // Unsummoned: its row kept, inactive; summoned again: the same row (no second one made).
    a.client->Send(proto::CosDismiss{wolf.entity});
    CHECK(Next<proto::CosDismissed>(a).entity == wolf.entity);
    CHECK(eventually([&] { return state(wolfRow) == std::int64_t(game::CosState::Inactive); }));
    a.client->Send(proto::UseItem{flute});
    const auto again = Next<proto::CosSummoned>(a);
    CHECK(again.model == kWolf);
    CHECK(eventually([&] { return state(wolfRow) == std::int64_t(game::CosState::Summoned); }));
    CHECK(itemData(kFlute) == wolfRow);
    CHECK(t.Db().Query("SELECT count(*) FROM _charcos WHERE ownercharid = $1", {a.character}).Int(0, 0) == 2);

    // Follow: a pet's own command (someone else's pet takes none).
    a.client->Send(proto::CosCommand{again.entity, proto::CosOrder::Follow});

    // Its hunger: without a living attack pet out, the HGP potion is refused; out at 1 HGP the wolf eats it (told at
    // none); the potion feeds it (to the full: this potion's 100 %), used up; full, the next is refused.
    const std::uint8_t potion = PlaceOf(a.bag, kHgpPotion);
    a.client->Send(proto::CosDismiss{again.entity});
    CHECK(Next<proto::CosDismissed>(a).entity == again.entity);
    CHECK(eventually([&] { return state(wolfRow) == std::int64_t(game::CosState::Inactive); }));
    a.client->Send(proto::UseItem{potion});
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COSERR_YOU_DONT_HAVE_ACTIVE_COS_OBJ");
    const std::uint8_t kit = PlaceOf(a.bag, kHpKit);
    a.client->Send(proto::UseItem{kit}); // the HP kit: no COS out either
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COSERR_YOU_DONT_HAVE_ACTIVE_COS_OBJ");
    REQUIRE(t.Db().Query("UPDATE _charcos SET hgp = 1, hp = 100 WHERE id = $1", {wolfRow}).Ok());
    a.client->Send(proto::UseItem{flute});
    const auto hungry = Next<proto::CosSummoned>(a);
    CHECK(hungry.hgp == 1);
    CHECK(hungry.hp == 100);
    CHECK(Next<proto::CosHgp>(a).hgp == 0);
    CHECK(eventually([&] { return t.Db().Query("SELECT hgp FROM _charcos WHERE id = $1", {wolfRow}).Int(0, 0) == 0; }));
    a.client->Send(proto::UseItem{potion});
    const auto fed = Next<proto::CosHgp>(a);
    CHECK(fed.entity == hungry.entity);
    CHECK(fed.hgp == game::kCosHgpFull);
    CHECK(eventually([&] { return t.Db().Query("SELECT hgp FROM _charcos WHERE id = $1", {wolfRow}).Int(0, 0) == game::kCosHgpFull; }));
    a.client->Send(proto::UseItem{potion});
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COSPETERR_HGPFULL_NODRINK");

    // The HP kit heals it at once (360 grown by its owner's STR: more than 360, at most its whole 5,000), one used up and
    // its health saved. (Its natural recovery's ticks, 80 each, come in between: passed over.)
    a.client->Send(proto::UseItem{kit});
    std::uint32_t healed = 0;
    for (int i = 0; i < 40 && healed == 0; ++i) {
        const auto m = Next<proto::EntityDamaged>(a);
        if (m.target == hungry.entity && m.damage == 0 && m.hp >= 100u + 360u) healed = m.hp;
    }
    CHECK(healed >= 100u + 360u);
    CHECK(healed <= 5000u);
    // Standing beside its owner, its natural recovery: 1.6 % of its 5,000 (80) at its next tick (4 s).
    proto::EntityDamaged rest;
    for (int i = 0; i < 80 && !(rest.target == hungry.entity && rest.damage == 0); ++i) {
        if (!a.client->Expect(rest, 100ms)) rest = {};
    }
    CHECK(rest.hp == healed + 80u);
    proto::ItemMoved used;
    for (int i = 0; i < 40 && !(used.from == kit && !used.changed.empty()); ++i) used = Next<proto::ItemMoved>(a);
    REQUIRE_FALSE(used.changed.empty());
    CHECK(used.changed[0].quantity == 2);
    CHECK(eventually([&] { return t.Db().Query("SELECT hp FROM _charcos WHERE id = $1", {wolfRow}).Int(0, 0) >= std::int64_t(healed); }));

    // Unsummoned: the wolf back in its item, inactive (its owner's death would leave it out: the original's takes only
    // the COS ridden).
    a.client->Send(proto::CosDismiss{hungry.entity});
    CHECK(Next<proto::CosDismissed>(a).entity == hungry.entity);
    CHECK(eventually([&] { return state(wolfRow) == std::int64_t(game::CosState::Inactive); }));

    // The wolf dead (its fight is to come: made so here), a Grass of Life on its item brings it back, its HP full.
    REQUIRE(t.Db().Query("UPDATE _charcos SET state = 4, hp = 0 WHERE id = $1", {wolfRow}).Ok());
    a.client->Send(proto::CosUseOnPet{grass, flute});
    CHECK(petNews(wolfRow, [](const proto::CosPetItem& p) { return p.state == std::uint8_t(game::CosState::Inactive); }));
    CHECK(eventually([&] { return state(wolfRow) == std::int64_t(game::CosState::Inactive) && held(kGrass) == 0; }));
    CHECK(t.Db().Query("SELECT hp FROM _charcos WHERE id = $1", {wolfRow}).Int(0, 0) == 5000);
    s.game.Stop();
}

TEST_CASE("Game server: a grab pet picks up into its bag; its bag and its owner's inventory trade items; its settings") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Gatherer", 1000, {PetStored(kRabbitScroll), Stored(kHerb, 5), Stored(kHerb, 3)});
    const auto eventually = [&](const std::function<bool()>& holds) {
        for (int i = 0; i < 500 && !holds(); ++i) s.game.Poll(10ms);
        return holds();
    };
    // Herbs as stored: in the pet's bag, in the inventory.
    const auto inBag = [&](std::int64_t cos) {
        return t.Db().Query("SELECT coalesce(sum(it.data), 0) FROM _invcos b JOIN _items it ON it.id64 = b.itemid WHERE b.cosid = $1 AND it.refitemid = $2",
                            {cos, std::int64_t(kHerb)}).Int(0, 0);
    };
    const auto inInventory = [&] {
        return t.Db().Query("SELECT coalesce(sum(it.data), 0) FROM _inventory i JOIN _items it ON it.id64 = i.itemid WHERE i.charid = $1 AND it.refitemid = $2",
                            {a.character, std::int64_t(kHerb)}).Int(0, 0);
    };
    std::vector<std::uint8_t> herbs;
    for (const proto::ItemRecord& r : a.bag.items) {
        if (r.item == kHerb) herbs.push_back(r.slot);
    }
    REQUIRE(herbs.size() == 2);

    // Summoned: grabbing on (its owner's items, every kind), an empty bag of its own 28 places (a watch adds
    // more).
    a.client->Send(proto::UseItem{PlaceOf(a.bag, kRabbitScroll)});
    const auto pet = Next<proto::CosSummoned>(a);
    CHECK(pet.option == (proto::kGrabOn | proto::kGrabGold | proto::kGrabEquipment | proto::kGrabOther));
    auto bag = Next<proto::CosBag>(a);
    CHECK(bag.size == game::kGrabPetBag);
    CHECK(bag.items.empty());

    // Herbs dropped: the pet goes for them and picks them up into its bag (the second onto the first's stack).
    a.client->Send(proto::DropItem{herbs[0]});
    bag = Next<proto::CosBag>(a);
    REQUIRE(bag.items.size() == 1);
    CHECK(bag.items[0].item == kHerb);
    CHECK(bag.items[0].quantity == 5);
    a.client->Send(proto::DropItem{herbs[1]});
    bag = Next<proto::CosBag>(a);
    REQUIRE(bag.items.size() == 1);
    CHECK(bag.items[0].quantity == 8);
    const std::int64_t cos = t.Db().Query("SELECT it.data FROM _inventory i JOIN _items it ON it.id64 = i.itemid WHERE i.charid = $1 AND it.refitemid = $2",
                                          {a.character, std::int64_t(kRabbitScroll)}).Int(0, 0);
    CHECK(eventually([&] { return inBag(cos) == 8 && inInventory() == 0; }));

    // Out of its bag into the inventory: the place it is taken to, saved on both sides together.
    a.client->Send(proto::CosBagMove{pet.entity, bag.items[0].slot, herbs[0], true, false});
    proto::ItemMoved moved;
    for (;;) {
        moved = Next<proto::ItemMoved>(a);
        if (!moved.changed.empty() && moved.changed[0].item == kHerb) break;
    }
    CHECK(moved.changed[0].slot == herbs[0]);
    CHECK(moved.changed[0].quantity == 8);
    CHECK(Next<proto::CosBag>(a).items.empty());
    CHECK(eventually([&] { return inBag(cos) == 0 && inInventory() == 8; }));
    // And back into the bag; its own summoning item never.
    a.client->Send(proto::CosBagMove{pet.entity, herbs[0], 3, false, true});
    bag = Next<proto::CosBag>(a);
    REQUIRE(bag.items.size() == 1);
    CHECK(bag.items[0].slot == 3);
    CHECK(eventually([&] { return inBag(cos) == 8 && inInventory() == 0; }));
    a.client->Send(proto::CosBagMove{pet.entity, PlaceOf(a.bag, kRabbitScroll), 4, false, true});
    CHECK(Next<proto::CosRefused>(a).text == "UIIT_MSG_COSPETERR_CANT_MOVETO_SUMITEM");

    // Grabbing off: told back; what lies on the ground stays there.
    a.client->Send(proto::CosSetOption{pet.entity, 0});
    CHECK(Next<proto::CosOption>(a).option == 0);
    a.client->Send(proto::CosBagMove{pet.entity, 3, herbs[1], true, false});
    REQUIRE(a.client->Expect(moved));
    CHECK(Next<proto::CosBag>(a).items.empty());
    a.client->Send(proto::DropItem{herbs[1]});
    proto::CosBag none;
    CHECK_FALSE(a.client->Expect(none, 2s));
    s.game.Stop();
}

TEST_CASE("Game server: the reverse return scroll: refused with the original's codes, the recall point a return scroll leaves, back to it") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Wanderer", 600, {Stored(kReverseScroll, 3), Stored(kReturnScroll, 1)}, 20);
    const std::uint8_t reverse = PlaceOf(a.bag, kReverseScroll), back = PlaceOf(a.bag, kReturnScroll);
    REQUIRE(reverse != 0);
    REQUIRE(back != 0);
    const auto refusal = [&](std::uint8_t option, std::uint32_t point = 0) {
        a.client->Send(proto::UseItem{reverse, option, point});
        return Next<proto::ItemRefused>(a).code;
    };
    // No recall point, no place of death, no such option: refused, the scroll kept.
    CHECK(refusal(proto::kReverseToRecall) == 0x1885);
    CHECK(refusal(proto::kReverseToDeath) == 0x1886);
    CHECK(refusal(9) == 2);
    CHECK(refusal(proto::kReverseToPoint, 999999) == 2); // no such map point
    // A return scroll read here keeps this place as the recall point, in _char too.
    a.client->Send(proto::UseItem{back});
    Next<proto::ReturnStarted>(a);
    for (int i = 0; i < 50 && t.Db().Query("SELECT telregion FROM sro_vt_shard._char WHERE charid = $1", {a.character}).Int(0, 0) == 0; ++i) {
        s.game.Poll(20ms);
    }
    const server::QueryResult kept = t.Db().Query("SELECT telregion, telposx, telposz FROM sro_vt_shard._char WHERE charid = $1", {a.character});
    CHECK(std::uint16_t(std::int16_t(kept.Int(0, 0))) == assets::RegionId(168, 97));
    CHECK(kept.Double(0, 1) == doctest::Approx(600.0));
    // Walked away, the reverse scroll's recall choice takes it back there: read for its time, then the travel.
    proto::WorldPosition away{assets::RegionId(168, 97), 700, 0, 1000};
    a.client->Send(proto::MoveTo{away});
    for (int i = 0; i < 100; ++i) s.game.Poll(20ms);
    a.client->Send(proto::UseItem{reverse, proto::kReverseToRecall});
    const auto started = Next<proto::ReturnStarted>(a);
    CHECK(started.item == kReverseScroll);
    proto::WorldEntered arrived;
    bool there = false;
    for (int i = 0; i < 100 && !there; ++i) {
        if (a.client->Expect(arrived, 50ms)) there = true;
    }
    REQUIRE(there);
    CHECK(arrived.position.region == assets::RegionId(168, 97));
    CHECK(arrived.position.x == doctest::Approx(600.0f).epsilon(0.01));
    s.game.Stop();
}

TEST_CASE("Game server: the auto potion's rows of ours: the attack pet's health kept up with the pets' HP kit, the settings kept") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Keeper", 1000, {PetStored(kFlute), Stored(kHpKit, 3)});
    const std::uint8_t flute = PlaceOf(a.bag, kFlute);
    const auto eventually = [&](const std::function<bool()>& holds) {
        for (int i = 0; i < 500 && !holds(); ++i) s.game.Poll(10ms);
        return holds();
    };
    const auto itemData = [&](std::uint32_t refId) {
        return t.Db().Query("SELECT it.data FROM _inventory i JOIN _items it ON it.id64 = i.itemid WHERE i.charid = $1 AND it.refitemid = $2",
                            {a.character, std::int64_t(refId)}).Int(0, 0);
    };
    // The wolf out once (its row made), back in, its health set low, out again.
    a.client->Send(proto::UseItem{flute});
    const auto first = Next<proto::CosSummoned>(a);
    REQUIRE(eventually([&] { return itemData(kFlute) != 0; }));
    const std::int64_t wolfRow = itemData(kFlute);
    a.client->Send(proto::CosDismiss{first.entity});
    CHECK(Next<proto::CosDismissed>(a).entity == first.entity);
    REQUIRE(eventually([&] { return t.Db().Query("SELECT state FROM _charcos WHERE id = $1", {wolfRow}).Int(0, 0) == std::int64_t(game::CosState::Inactive); }));
    REQUIRE(t.Db().Query("UPDATE _charcos SET hp = 100 WHERE id = $1", {wolfRow}).Ok());
    a.client->Send(proto::UseItem{flute});
    const auto wolf = Next<proto::CosSummoned>(a);
    REQUIRE(wolf.hp == 100);

    // The vehicle's row (no vehicle out: nothing) and the attack pet's at 50 %, both with the kit; the server keeps
    // them and answers with them.
    proto::AutoPotionSettings settings;
    settings.vehicle = {true, 60, kHpKit};
    settings.attackPet = {true, 50, kHpKit};
    a.client->Send(settings);
    proto::AutoPotionSettings kept; // (the one the world's entry sent comes first)
    for (int i = 0; i < 5 && kept.attackPet.item == 0; ++i) kept = Next<proto::AutoPotionSettings>(a);
    CHECK(kept.attackPet.enabled);
    CHECK(kept.attackPet.percent == 50);
    CHECK(kept.attackPet.item == kHpKit);
    CHECK(kept.vehicle.item == kHpKit);
    CHECK(eventually([&] {
        return t.Db().Query("SELECT count(*) FROM sr_auto_potion WHERE charid = $1 AND configtype IN (101, 102) AND refobjid = $2",
                            {a.character, std::int64_t(kHpKit)}).Int(0, 0) == 2;
    }));
    // At 100 of 5,000 the wolf is under its row's half: a kit used on it at once, more after the delay until it is over.
    std::uint32_t healed = 0;
    for (int i = 0; i < 60 && healed == 0; ++i) {
        const auto m = Next<proto::EntityDamaged>(a);
        if (m.target == wolf.entity && m.damage == 0 && m.hp >= 100u + 360u) healed = m.hp;
    }
    CHECK(healed >= 100u + 360u);
    proto::ItemMoved used;
    for (int i = 0; i < 40 && used.changed.empty(); ++i) used = Next<proto::ItemMoved>(a);
    REQUIRE_FALSE(used.changed.empty());
    CHECK(used.changed[0].quantity == 2);
    // Its settings read back with the character.
    bool ok = false;
    const std::int64_t account = server::FindAccount(t.Db(), "Keeperacct", ok)->id;
    const auto row = server::LoadCharacter(t.Db(), account, s.config.id, a.character, ok);
    REQUIRE(row);
    CHECK(row->autoPotion.rows[game::AutoPotion::kAttackPet] == game::AutoPotion::Entry{true, 50, kHpKit});
    CHECK(row->autoPotion.rows[game::AutoPotion::kVehicle] == game::AutoPotion::Entry{true, 60, kHpKit});
    s.game.Stop();
}

TEST_CASE("Game server: a job's suit: the league and alias kept, the state, level and alias those near see") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    s.config.towns = JobTowns();
    s.config.jobSuitSeconds = 0.3;
    s.config.jobLockSeconds = 0.2;
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    // A trader of job level 3 with the alias Merchant, its suit in the bag; a neighbour.
    const auto league = [&](std::int64_t character) {
        REQUIRE(server::SaveJob(t.Db(), character, {game::JobType::Trader, 3, 1234, 0, 0}));
        REQUIRE(server::SaveAlias(t.Db(), character, "Merchant"));
    };
    auto a = s.Make("Seller", 1000, {Stored(kTraderSuit, 1)}, 30, 0, 0, 1000, league);
    auto b = s.Make("Watcher", 1100, {});
    // Its own league, told on entering; no suit yet: no job state.
    proto::JobInfoMessage info;
    for (int i = 0; i < 20 && info.alias.empty(); ++i) info = Next<proto::JobInfoMessage>(a);
    CHECK(info.type == 1);
    CHECK(info.level == 3);
    CHECK(info.exp == 1234);
    CHECK(info.alias == "Merchant");
    // The suit on: those near hear its state, level and alias; it hears its own.
    a.client->Send(proto::ItemMove{PlaceOf(a.bag, kTraderSuit), game::slot::kJobSuit, 0});
    const auto seen = Next<proto::EntityJob>(b);
    CHECK(seen.entity == a.entity);
    CHECK(seen.state == 1);
    CHECK(seen.level == 3);
    CHECK(seen.alias == "Merchant");
    CHECK(Next<proto::EntityJob>(a).state == 1);
    for (int i = 0; i < 150; ++i) s.game.Poll(2ms); // past the job lock
    // Off again: no job (the alias gone with it).
    a.client->Send(proto::ItemMove{game::slot::kJobSuit, 13, 0});
    const auto off = Next<proto::EntityJob>(b);
    CHECK(off.state == game::kNoJob);
    CHECK(off.alias.empty());
    // The league and alias as the database keeps them.
    game::JobInfo job;
    std::string alias, error;
    std::int64_t leftAt = 0;
    REQUIRE(server::LoadJob(t.Db(), a.character, job, alias, leftAt, error));
    CHECK(leftAt == 0);
    CHECK(job == game::JobInfo{game::JobType::Trader, 3, 1234, 0, 0});
    CHECK(alias == "Merchant");
    CHECK(t.Db().Query("SELECT count(*) FROM _charnicknamelist WHERE nickname16 = 'Merchant'").Int(0, 0) == 1);
    s.game.Stop();
}

TEST_CASE("Game server: the league at its NPC: join, the alias, leave, the previous information") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    // Jangan's trader keeper by the start place, and the thieves' keeper beside it.
    constexpr std::uint32_t kDoctor = 2027, kThiefKeeper = 2178;
    s.config.npcs.push_back({kDoctor, "NPC_CH_DOCTOR", {assets::RegionId(168, 97), 1050.0f, 0.0f, 1000.0f}, 0.0f});
    s.config.npcs.push_back({kThiefKeeper, "NPC_TD_THIEF_SELL", {assets::RegionId(168, 97), 1080.0f, 0.0f, 1000.0f}, 0.0f});
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    // A level-30 character with 120,000 gold and an old trader's level 2; a level-10 one.
    const auto rich = [&](std::int64_t character) {
        REQUIRE(server::SaveGold(t.Db(), character, 120'000));
        REQUIRE(t.Db().Query("INSERT INTO _oldtrijob VALUES ($1, 2, 500, 3, 700, 1, 0)", {character}).Ok());
    };
    auto a = s.Make("Joiner", 1000, {}, 30, 0, 0, 1000, rich);
    auto low = s.Make("Lowly", 1010, {}, 10);
    CHECK(Next<proto::JobInfoMessage>(a).type == 0); // on entering: in no league
    std::uint32_t doctor = 0, thiefKeeper = 0;
    while (doctor == 0 || thiefKeeper == 0) {
        const auto m = Next<proto::EntityAppeared>(a);
        if (m.character.model == kDoctor) doctor = m.entity;
        if (m.character.model == kThiefKeeper) thiefKeeper = m.entity;
    }
    const auto answer = [&](CosServer::Player& p) { return Next<proto::JobAnswer>(p); };
    const auto join = [&](CosServer::Player& p, std::uint32_t npc, std::uint8_t type) {
        p.client->Send(proto::JobJoin{npc, type});
        return answer(p).code;
    };
    const auto alias = [&](std::uint32_t npc, std::uint8_t action, const std::string& name) {
        a.client->Send(proto::JobAlias{npc, action, name});
        const auto m = answer(a);
        CHECK(m.request == proto::JobRequest::Alias);
        CHECK(m.action == action);
        return m.code;
    };

    // Join: the level, the NPC's league, then in.
    CHECK(join(low, doctor, 1) == proto::JobCode::LevelTooLow);
    CHECK(join(a, doctor, 2) == proto::JobCode::NotOffered);
    CHECK(join(a, doctor, 9) == proto::JobCode::NotInLeague);
    CHECK(join(a, doctor, 1) == proto::JobCode::Ok);
    // The fee (30 - 20) x 5,000 taken; the old trader's level and experience the league's start, its row gone; a
    // trader's safe trade made.
    CHECK(Next<proto::GoldChanged>(a).gold == 70'000);
    const auto joined = Next<proto::JobInfoMessage>(a);
    CHECK(joined.type == 1);
    CHECK(joined.level == 2);
    CHECK(joined.exp == 500);
    CHECK(t.Db().Query("SELECT count(*) FROM _oldtrijob WHERE charid = $1", {a.character}).Int(0, 0) == 0);
    CHECK(t.Db().Query("SELECT count(*) FROM _chartrijobsafetrade WHERE charid = $1", {a.character}).Int(0, 0) == 1);
    CHECK(join(a, doctor, 1) == proto::JobCode::InALeague);

    // The alias: a character's name, the rules, another league's keeper; a free one, then set (the first free).
    CHECK(alias(doctor, proto::kAliasCheck, "lowly") == proto::JobCode::NameIsCharacter);
    CHECK(alias(doctor, proto::kAliasCheck, "x") == proto::JobCode::NameNotAllowed);
    CHECK(alias(doctor, proto::kAliasCheck, "Bad name") == proto::JobCode::NameNotAllowed);
    CHECK(alias(thiefKeeper, proto::kAliasCheck, "Trade1") == proto::JobCode::NotOffered);
    CHECK(alias(doctor, proto::kAliasCheck, "Trade1") == proto::JobCode::Ok);
    CHECK(alias(doctor, proto::kAliasSet, "Trade1") == proto::JobCode::Ok);
    CHECK(Next<proto::JobInfoMessage>(a).alias == "Trade1");
    CHECK(alias(doctor, proto::kAliasCheck, "TRADE1") == proto::JobCode::NameIsAlias);
    // A change costs 30 x 50,000.
    CHECK(alias(doctor, proto::kAliasSet, "Trade2") == proto::JobCode::NotEnoughGold);
    // Kept in _char (and in the shard-wide list of aliases).
    CHECK(t.Db().Query("SELECT nickname16 FROM _char WHERE charid = $1", {a.character}).Text(0, 0) == "Trade1");

    // The previous information: gone with the join; another's shown.
    a.client->Send(proto::JobPreviousQuery{doctor});
    CHECK(answer(a).code == proto::JobCode::NoPrevious);
    REQUIRE(t.Db().Query("INSERT INTO _oldtrijob VALUES ($1, 4, 9000, 5, 8000, 6, 7000)", {low.character}).Ok());
    low.client->Send(proto::JobPreviousQuery{doctor});
    const auto old = Next<proto::JobPrevious>(low);
    CHECK(old.traderLevel == 4);
    CHECK(old.thiefExp == 8000);
    CHECK(old.hunterLevel == 6);

    // Leave: at another league's keeper no; at its own, free; no league, the 7-day wait.
    a.client->Send(proto::JobLeave{thiefKeeper});
    CHECK(answer(a).code == proto::JobCode::NotOffered);
    a.client->Send(proto::JobLeave{doctor});
    const auto left = answer(a);
    CHECK(left.request == proto::JobRequest::Leave);
    CHECK(left.code == proto::JobCode::Ok);
    CHECK(Next<proto::JobInfoMessage>(a).type == 0);
    CHECK(join(a, doctor, 1) == proto::JobCode::RejoinWait);
    a.client->Send(proto::JobLeave{doctor});
    CHECK(answer(a).code == proto::JobCode::NotInLeague);
    game::JobInfo job;
    std::string kept, error;
    std::int64_t leftAt = 0;
    REQUIRE(server::LoadJob(t.Db(), a.character, job, kept, leftAt, error));
    CHECK(job.type == game::JobType::None);
    CHECK(leftAt != 0);
    CHECK(kept.empty()); // the alias gone with the league, still taken (the list keeps it)
    CHECK(t.Db().Query("SELECT count(*) FROM _charnicknamelist WHERE nickname16 = 'Trade1'").Int(0, 0) == 1);
    CHECK(t.Db().Query("SELECT count(*) FROM _chartrijobsafetrade WHERE charid = $1", {a.character}).Int(0, 0) == 0);
    s.game.Stop();
}

TEST_CASE("Game server: one copy of a character in the world: a second connection's entry replaces the first, its pets once (the user's report 2026-11-13)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Twin", 1000, {PetStored(kFlute), PetStored(kRabbitScroll)});
    auto watcher = s.Make("Watch", 1050, {});
    // Both pets out.
    a.client->Send(proto::UseItem{PlaceOf(a.bag, kFlute)});
    const auto wolf = Next<proto::CosSummoned>(a);
    a.client->Send(proto::UseItem{PlaceOf(a.bag, kRabbitScroll)});
    const auto rabbit = Next<proto::CosSummoned>(a);
    REQUIRE(wolf.entity != rabbit.entity);
    // What the watcher saw so far, read off: from here on only what the second entry brings.
    for (int i = 0; i < 20; ++i) {
        proto::EntityAppeared seen;
        while (watcher.client->Expect(seen, 50ms)) {
        }
    }
    // The account's lock lost meanwhile (a game server that stopped reporting, a release by a late close, ...), and the
    // player, whose first client hangs, logs in again with a second one.
    bool ok = false;
    const std::int64_t account = server::FindAccount(t.Db(), "Twinacct", ok)->id;
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {account}).Ok());
    Client again(s.net, s.game.Port(), [&] { s.game.Poll(2ms); });
    REQUIRE(again.Greet());
    proto::LobbyEnter e;
    e.ticket = server::IssueTicket(t.Db(), account, s.config.id, 30, s.error);
    again.Send(e);
    proto::LobbyEntered entered;
    REQUIRE(again.Expect(entered));
    REQUIRE(entered.status == proto::LobbyEnterStatus::Accepted);
    again.Send(proto::CharacterSelect{"Twin"});
    proto::LobbyResult selected;
    REQUIRE(again.Expect(selected));
    REQUIRE(selected.status == proto::LobbyStatus::Ok);
    again.Send(proto::WorldEnter{});
    proto::WorldEntered world;
    REQUIRE(again.Expect(world));
    REQUIRE(world.status == proto::WorldStatus::Ok);
    CHECK(world.entity != a.entity);
    // The first connection hears why and is closed; those near see its character and its pets go.
    proto::Disconnected bye;
    REQUIRE(a.client->Expect(bye));
    CHECK(bye.reason == proto::DisconnectReason::Replaced);
    CHECK(a.client->WaitClosed());
    std::set<std::uint32_t> gone;
    for (int i = 0; i < 50 && !(gone.contains(a.entity) && gone.contains(wolf.entity) && gone.contains(rabbit.entity)); ++i) {
        proto::EntityGone g;
        if (watcher.client->Expect(g, 200ms)) gone.insert(g.entity);
    }
    CHECK(gone.contains(a.entity));
    CHECK(gone.contains(wolf.entity));
    CHECK(gone.contains(rabbit.entity));
    // One Twin in sight of the watcher, and at most one of each pet: never a second from rows still marked out.
    int twins = 0, wolves = 0, rabbits = 0;
    for (int i = 0; i < 40; ++i) {
        proto::EntityAppeared seen;
        if (!watcher.client->Expect(seen, 50ms)) continue;
        if (seen.kind == proto::EntityKind::Character && seen.character.name == "Twin") ++twins;
        if (seen.kind == proto::EntityKind::Cos && seen.character.model == kWolf) ++wolves;
        if (seen.kind == proto::EntityKind::Cos && seen.character.model == kRabbit) ++rabbits;
    }
    CHECK(twins == 1);
    CHECK(wolves <= 1);
    CHECK(rabbits <= 1);
    // The first connection's close never frees the session the second holds now (its own token).
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
    const auto session = t.Db().Query("SELECT token FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {account});
    REQUIRE(session.Rows() == 1);
    CHECK(session.Int(0, 0) != 0);
    s.game.Stop();
}

TEST_CASE("Game server: a job suit's rules: the refusals' codes, the 10 s change those near see, the lock") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    s.config.towns = JobTowns();
    s.config.jobSuitSeconds = 1.0;
    s.config.jobLockSeconds = 0.5;
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    const auto trader = [&](const char* alias) {
        return [&t, alias](std::int64_t character) {
            REQUIRE(server::SaveJob(t.Db(), character, {game::JobType::Trader, 1, 0, 0, 0}));
            if (alias[0] != '\0') REQUIRE(server::SaveAlias(t.Db(), character, alias));
        };
    };
    const auto wear = [&](CosServer::Player& p, std::uint8_t from, std::uint8_t to) {
        p.client->Send(proto::ItemMove{from, to, 0});
        return Next<proto::ItemMoved>(p);
    };
    // In no league; a hunter's suit for a trader; no alias yet; out of town.
    auto plain = s.Make("Plain", 1000, {Stored(kTraderSuit, 1)});
    auto r = wear(plain, PlaceOf(plain.bag, kTraderSuit), game::slot::kJobSuit);
    CHECK(r.status == proto::ItemStatus::JobSuit);
    CHECK(r.code == 0x189E);
    auto other = s.Make("Other", 1010, {Stored(kHunterSuit, 1)}, 1, 0, 0, 1000, trader("Other1"));
    r = wear(other, PlaceOf(other.bag, kHunterSuit), game::slot::kJobSuit);
    CHECK(r.code == 0x189E);
    auto nameless = s.Make("Nameless", 1020, {Stored(kTraderSuit, 1)}, 1, 0, 0, 1000, trader(""));
    r = wear(nameless, PlaceOf(nameless.bag, kTraderSuit), game::slot::kJobSuit);
    CHECK(r.code == 0x189F);
    auto outside = s.Make("Outside", 1000, {Stored(kTraderSuit, 1)}, 1, assets::RegionId(168, 98), 0, 1000, trader("Out1"));
    r = wear(outside, PlaceOf(outside.bag, kTraderSuit), game::slot::kJobSuit);
    CHECK(r.code == 0x18A2);
    CHECK(outside.client->Connection().State() != net::ConnectionState::Closed);

    // A trader with its alias in town: the change starts (0x189B), it and those near see its 10 s; then the suit is on.
    auto a = s.Make("Wearer", 1030, {Stored(kTraderSuit, 1)}, 1, 0, 0, 1000, trader("Wearer1"));
    // Those near see it (a newcomer is in the views around it within kViewTurn ticks): by the clock.
    for (const auto until = std::chrono::steady_clock::now() + 500ms; std::chrono::steady_clock::now() < until;) s.game.Poll(2ms);
    const std::uint8_t place = PlaceOf(a.bag, kTraderSuit);
    r = wear(a, place, game::slot::kJobSuit);
    CHECK(r.status == proto::ItemStatus::JobChange);
    CHECK(r.code == 0x189B);
    // Meanwhile another move of the suit: a change under way.
    r = wear(a, place, game::slot::kJobSuit);
    CHECK(r.code == 0x1847);
    const auto bar = Next<proto::EntityJobChange>(a);
    CHECK(bar.entity == a.entity);
    CHECK(bar.on == 1);
    CHECK(bar.seconds == 1);
    proto::EntityJobChange seen;
    for (int i = 0; i < 20 && seen.entity != a.entity; ++i) seen = Next<proto::EntityJobChange>(plain);
    CHECK(seen.seconds == 1);
    // Made: the move's answer, the bar's end, the job state.
    r = Next<proto::ItemMoved>(a);
    CHECK(r.status == proto::ItemStatus::Ok);
    CHECK(r.to == game::slot::kJobSuit);
    CHECK(Next<proto::EntityJobChange>(a).seconds == 0);
    CHECK(Next<proto::EntityJob>(a).state == 1);
    // Within the lock it stays on.
    r = wear(a, game::slot::kJobSuit, place);
    CHECK(r.code == 0x1847);
    // Past the (test's 1 s) lock by the clock: a poll returns early whenever something happens.
    for (const auto until = std::chrono::steady_clock::now() + 1200ms; std::chrono::steady_clock::now() < until;) s.game.Poll(2ms);
    // Past it: off in its turn.
    r = wear(a, game::slot::kJobSuit, place);
    CHECK(r.status == proto::ItemStatus::JobChange);
    CHECK(Next<proto::EntityJobChange>(a).on == 0);
    r = Next<proto::ItemMoved>(a);
    CHECK(r.status == proto::ItemStatus::Ok);
    CHECK(r.to == place);
    CHECK(Next<proto::EntityJob>(a).state == game::kNoJob);
    s.game.Stop();
}

TEST_CASE("Game server: trade: goods bought into the transport in Jangan, sold in Donwhang at the quote, the market moved") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    constexpr std::uint32_t kGood = 900300, kChSpecial = 2010, kWcSpecial = 2059;
    // ITEM_ETC_TRADE_CH_01: a specialty good (TID 3/3/8/1) of 40 a stack, 383 at Jangan's specialty shop.
    auto items = std::make_shared<assets::ItemTable>(*s.config.rules->items);
    assets::ItemRow good;
    good.refObjId = kGood;
    good.codename = "ITEM_ETC_TRADE_CH_01";
    good.typeId = {3, 3, 8, 1};
    good.inService = true;
    good.maxStack = 40;
    good.sellPrice = 191;
    good.canTrade = false;
    items->Add(good);
    assets::ItemRow bargain = good; // ITEM_ETC_TRADE_CH_05: a bargain good (TID4 2)
    bargain.refObjId = kGood + 1;
    bargain.codename = "ITEM_ETC_TRADE_CH_05";
    bargain.typeId = {3, 3, 8, 2};
    items->Add(bargain);
    auto rules = std::make_shared<game::CharacterRules>(*s.config.rules);
    rules->items = items;
    s.config.rules = rules;
    auto shops = std::make_shared<assets::ShopTable>();
    assets::NpcShop shop;
    assets::ShopGood g;
    g.slot = 0;
    g.itemCodename = good.codename;
    g.price = game::kGoodsPrice;
    assets::ShopGood b = g;
    b.slot = 1;
    b.itemCodename = bargain.codename;
    shop.groups.push_back({"G", "G", {{"T", "T", 0, {g, b}}}});
    // The bargain hours: a day-long window from midnight, so they run now.
    s.config.bargainSchedule.push_back({game::kScheduleSpecialGoods, 0, 0, 1, 0, 0, 0, 0, 86400});
    shops->Add("NPC_CH_SPECIAL", shop);
    shops->Add("NPC_WC_SPECIAL", shop); // its own goods (in the real data: Donwhang's)
    s.config.shops = shops;
    s.config.towns = JobTowns();
    s.config.npcs.push_back({kChSpecial, "NPC_CH_SPECIAL", {assets::RegionId(168, 97), 1050.0f, 0.0f, 1000.0f}, 0.0f});
    s.config.npcs.push_back({kWcSpecial, "NPC_WC_SPECIAL", {assets::RegionId(168, 97), 1080.0f, 0.0f, 1000.0f}, 0.0f});
    // The database's rows: Donwhang's quote for the good (at rest at its UB), the cart's trade bag, the gold levels.
    REQUIRE(t.Db().Query("CREATE TABLE _itemquotation (id integer PRIMARY KEY, service integer, assocnpc integer, refitemid integer, basequot real, "
                         "quot_lb real, quot_ub real, basestockamount integer, fluctuateamount integer, curstockamount integer)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _itemquotation VALUES (32, 1, $1, $2, 1.88, 1.58, 2.18, 50000, 250, 42499)", {std::int64_t(kWcSpecial), std::int64_t(kGood)}).Ok());
    REQUIRE(t.Db().Query("CREATE TABLE _refobjcommon (id integer PRIMARY KEY, codename128 varchar(129), link integer)").Ok());
    REQUIRE(t.Db().Query("CREATE TABLE _refobjchar (id integer PRIMARY KEY, inventorysize integer)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refobjcommon VALUES ($1, 'COS_T_TEST_CART', 1)", {std::int64_t(kCart)}).Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refobjchar VALUES (1, 40)").Ok());
    REQUIRE(t.Db().Query("CREATE TABLE _refdropgold (monlevel integer, goldmin integer)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refdropgold SELECT l, (3.5 * l + 25)::integer FROM generate_series(1, 140) l").Ok());
    REQUIRE(t.Db().Query("CREATE TABLE _trijobrewards (jobtype integer PRIMARY KEY, reward integer)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _trijobrewards VALUES (2, 0), (3, 0)").Ok());
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);

    // A level-30 trader in its suit with the alias Seller1, 100,000 gold.
    const auto trader = [&](std::int64_t character) {
        REQUIRE(server::SaveJob(t.Db(), character, {game::JobType::Trader, 1, 0, 0, 0}));
        REQUIRE(server::SaveAlias(t.Db(), character, "Seller1"));
        REQUIRE(server::SaveGold(t.Db(), character, 100'000));
        REQUIRE(t.Db().Query("INSERT INTO _inventory (charid, slot, itemid) SELECT $1, 8, b.itemid FROM _inventory b JOIN _items it ON it.id64 = b.itemid "
                             "WHERE b.charid = $1 AND it.refitemid = $2 LIMIT 1 ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                             {character, std::int64_t(kTraderSuit)})
                    .Ok());
        REQUIRE(t.Db().Query("UPDATE _inventory b SET itemid = 0 FROM _items it WHERE it.id64 = b.itemid AND b.charid = $1 AND it.refitemid = $2 AND b.slot <> 8",
                             {character, std::int64_t(kTraderSuit)})
                    .Ok());
    };
    auto a = s.Make("Seller", 1000, {Stored(kCartScroll, 1), Stored(kTraderSuit, 1)}, 30, 0, 0, 1000, trader);
    std::uint32_t ch = 0, wc = 0;
    while (ch == 0 || wc == 0) {
        const auto m = Next<proto::EntityAppeared>(a);
        if (m.character.model == kChSpecial) ch = m.entity;
        if (m.character.model == kWcSpecial) wc = m.entity;
    }
    const auto buy = [&](std::uint16_t count) {
        a.client->Send(proto::ShopBuy{ch, 0, 0, count});
        return Next<proto::TradeAnswer>(a);
    };
    // No transport out: only a trader with one buys.
    auto r = buy(40);
    CHECK(r.code == proto::TradeCode::NotTrader);
    a.client->Send(proto::UseItem{PlaceOf(a.bag, kCartScroll)});
    const auto cart = Next<proto::CosSummoned>(a);
    const auto empty = Next<proto::CosBag>(a);
    CHECK(empty.entity == cart.entity);
    CHECK(empty.size == 40);

    // 40 bought: 15,320, into the transport, made by its alias; one star.
    r = buy(40);
    CHECK(r.code == proto::TradeCode::Ok);
    CHECK(r.count == 40);
    CHECK(r.gold == 15'320);
    CHECK(Next<proto::GoldChanged>(a).gold == 100'000 - 15'320);
    const auto loaded = Next<proto::CosBag>(a);
    REQUIRE(loaded.items.size() == 1);
    CHECK(loaded.items[0].item == kGood);
    CHECK(loaded.items[0].quantity == 40);
    CHECK(loaded.items[0].creator == "Seller1");
    CHECK(Next<proto::TradeScale>(a).stars == 1);

    // Donwhang's prices: 834 a good (the quote 2.18, at UB).
    a.client->Send(proto::ShopOpen{wc});
    proto::TradePrices prices = Next<proto::TradePrices>(a);
    REQUIRE(prices.prices.size() == 1);
    CHECK(prices.prices[0].item == kGood);
    CHECK(prices.prices[0].price == 834);
    // Sold there: 33,360; the market moved (CurStock 42,539, the next good 831), saved.
    a.client->Send(proto::TradeSell{wc, loaded.items[0].slot, 0});
    r = Next<proto::TradeAnswer>(a);
    CHECK(r.request == proto::TradeRequest::Sell);
    CHECK(r.code == proto::TradeCode::Ok);
    CHECK(r.count == 40);
    CHECK(r.gold == 33'360);
    CHECK(Next<proto::GoldChanged>(a).gold == 100'000 - 15'320 + 33'360);
    // Its job experience at one star: floor(18,040 x 0.8) = 14,432; 3,336 donated to the hunters' league
    // (the original's worked example), into the hunters' pool.
    const auto exp = Next<proto::JobExpChanged>(a);
    CHECK(exp.type == 1);
    CHECK(exp.delta == 14'432);
    CHECK(exp.donated == 3'336);
    std::int64_t pool = 0;
    for (int i = 0; i < 200 && pool == 0; ++i) {
        s.game.Poll(2ms);
        pool = t.Db().Query("SELECT reward FROM _trijobrewards WHERE jobtype = 3").Int(0, 0);
    }
    CHECK(pool == 3'336);
    CHECK(Next<proto::CosBag>(a).items.empty());
    CHECK(Next<proto::TradeScale>(a).stars == 0);
    a.client->Send(proto::ShopOpen{wc});
    prices = Next<proto::TradePrices>(a);
    CHECK(prices.prices[0].price == 831);
    std::int64_t stock = 0;
    for (int i = 0; i < 200 && stock != 42'539; ++i) {
        s.game.Poll(2ms);
        stock = t.Db().Query("SELECT curstockamount FROM _itemquotation WHERE id = 32").Int(0, 0);
    }
    CHECK(stock == 42'539);

    // A good sold in its own town (no quote there): its sale price, 191.
    r = buy(1);
    CHECK(r.code == proto::TradeCode::Ok);
    const auto one = Next<proto::CosBag>(a);
    REQUIRE(one.items.size() == 1);
    a.client->Send(proto::TradeSell{ch, one.items[0].slot, 0});
    r = Next<proto::TradeAnswer>(a);
    CHECK(r.code == proto::TradeCode::Ok);
    CHECK(r.gold == 191);

    // The bargain hours run (they began at the server's start); a bargain good bought, then not again within 10 minutes.
    a.client->Send(proto::ShopBuy{ch, 0, 1, 1});
    CHECK(Next<proto::TradeAnswer>(a).code == proto::TradeCode::Ok);
    // Its lock-out told (the buff board's kind 6) and kept as our timed job 100/7 (2/7 is the academy founding wait).
    CHECK(Next<proto::TradeBargainLock>(a).seconds == 600);
    std::int64_t lockJobs = 0;
    for (int i = 0; i < 200 && lockJobs == 0; ++i) {
        s.game.Poll(2ms);
        lockJobs = t.Db().Query("SELECT count(*) FROM _timedjob WHERE charid = $1 AND category = 100 AND jobid = 7 AND data1 > 0", {a.character}).Int(0, 0);
    }
    CHECK(lockJobs == 1);
    a.client->Send(proto::ShopBuy{ch, 0, 1, 1});
    CHECK(Next<proto::TradeAnswer>(a).code == proto::TradeCode::BargainLocked);
    s.game.Stop();
}

TEST_CASE("Game server: the fortress tax: on a shop's goods at a fortress's NPC, written once 10,000 gathered") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    constexpr std::uint32_t kPotionNpc = 2017;
    auto shops = std::make_shared<assets::ShopTable>();
    assets::NpcShop shop;
    assets::ShopGood herb;
    herb.slot = 0;
    herb.itemCodename = "ITEM_ETC_TEST_HERB";
    herb.price = 20'000; // a stack of 50 a buy: 1,000,000
    shop.groups.push_back({"G", "G", {{"T", "T", 0, {herb}}}});
    shops->Add("NPC_CH_POTION", shop);
    s.config.shops = shops;
    s.config.npcs.push_back({kPotionNpc, "NPC_CH_POTION", {assets::RegionId(168, 97), 1050.0f, 0.0f, 1000.0f}, 0.0f});
    // Jangan's fortress taxes its potion trader (AssocFile1_128), at 10 %.
    REQUIRE(t.Db().Query("CREATE TABLE _refsiegefortress (fortressid integer PRIMARY KEY, codename128 varchar(129), service integer)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refsiegefortress VALUES (1, 'FORTRESS_JANGAN', 1)").Ok());
    REQUIRE(t.Db().Query("CREATE TABLE _refobjcommon (id integer PRIMARY KEY, service integer, codename128 varchar(129), link integer, assocfile1_128 varchar(129))").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refobjcommon VALUES ($1, 1, 'NPC_CH_POTION', 0, 'FORTRESS_JANGAN')", {std::int64_t(kPotionNpc)}).Ok());
    REQUIRE(t.Db().Query("INSERT INTO _siegefortress (fortressid, guildid, taxratio, tax) VALUES (1, 0, 10, 0)").Ok());
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    const auto rich = [&](std::int64_t character) { REQUIRE(server::SaveGold(t.Db(), character, 10'000'000)); };
    auto a = s.Make("Buyer", 1000, {}, 30, 0, 0, 1000, rich);
    std::uint32_t npc = 0;
    while (npc == 0) {
        const auto m = Next<proto::EntityAppeared>(a);
        if (m.character.model == kPotionNpc) npc = m.entity;
    }
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms); // the rates read
    // 50 at 20,000: 1,000,000 and 100,000 tax.
    a.client->Send(proto::ShopBuy{npc, 0, 0, 50});
    CHECK(Next<proto::GoldChanged>(a).gold == 10'000'000 - 1'100'000);
    // Written: at least 10,000 gathered.
    std::int64_t tax = 0;
    for (int i = 0; i < 200 && tax == 0; ++i) {
        s.game.Poll(2ms);
        tax = t.Db().Query("SELECT tax FROM _siegefortress WHERE fortressid = 1").Int(0, 0);
    }
    CHECK(tax == 100'000);
    // An ordinary sale is taxed too: 50 at 4,000 is 200,000, 20,000 of it the fortress's.
    const auto bag = Next<proto::ShopAnswer>(a);
    REQUIRE_FALSE(bag.changed.empty());
    a.client->Send(proto::ShopSell{npc, bag.changed.front().slot, 50});
    CHECK(Next<proto::GoldChanged>(a).gold == 10'000'000 - 1'100'000 + 180'000);
    for (int i = 0; i < 200 && tax == 100'000; ++i) {
        s.game.Poll(2ms);
        tax = t.Db().Query("SELECT tax FROM _siegefortress WHERE fortressid = 1").Int(0, 0);
    }
    CHECK(tax == 120'000);
    s.game.Stop();
}

TEST_CASE("Game server: the bandits: a trader's caravan out of town draws a wave of thief monsters around the transport, after him or it (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const char* dirEnv = std::getenv("SR_GAME_DIR");
    if (dirEnv == nullptr || *dirEnv == 0) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirEnv, error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    CosServer s(t);
    constexpr std::uint32_t kGood = 900300, kSpecial = 2010;
    characters->Add(Cos(kCart, "COS_T_TEST_CART", 2));
    // The European thief monsters of levels 20..40 (`MOB_EU_THIEF_NPC_*`, TID 1/2/1/2), as the reference has them.
    std::set<std::uint32_t> thieves;
    for (const auto& [code, r] : characters->rows()) {
        if (code.starts_with("MOB_EU_THIEF_NPC_") && r.typeId[1] == 2 && r.typeId[2] == 1 && r.typeId[3] == 2 && r.level >= 20 && r.level <= 40) {
            thieves.insert(r.refObjId);
        }
    }
    REQUIRE(!thieves.empty());
    auto items = std::make_shared<assets::ItemTable>(*s.config.rules->items);
    assets::ItemRow good;
    good.refObjId = kGood;
    good.codename = "ITEM_ETC_TRADE_CH_01";
    good.typeId = {3, 3, 8, 1};
    good.inService = true;
    good.maxStack = 40;
    good.sellPrice = 191;
    items->Add(good);
    auto rules = std::make_shared<game::CharacterRules>(*s.config.rules);
    rules->items = items;
    s.config.rules = rules;
    s.config.files = &fs;
    s.config.characters = characters;
    s.config.skills = skills;
    auto shops = std::make_shared<assets::ShopTable>();
    assets::NpcShop shop;
    assets::ShopGood g;
    g.slot = 0;
    g.itemCodename = good.codename;
    g.price = game::kGoodsPrice;
    shop.groups.push_back({"G", "G", {{"T", "T", 0, {g}}}});
    shops->Add("NPC_CH_SPECIAL", shop);
    s.config.shops = shops;
    // West of Constantinople (a field: IsBattleField 1, continent Eu; the town's own regions, 77..81 x 103..107, are
    // safe ground), its navigation loaded.
    const std::uint16_t field = assets::RegionId(76, 105);
    s.config.npcs.push_back({kSpecial, "NPC_CH_SPECIAL", {field, 640.0f, 140.0f, 900.0f}, 0.0f});
    s.config.banditMinSeconds = 0.3; // the first wave at once (the original's 60-120 s)
    s.config.banditSpanSeconds = 0.0;
    REQUIRE(t.Db().Query("CREATE TABLE _refobjcommon (id integer PRIMARY KEY, service integer, codename128 varchar(129), link integer, "
                         "typeid1 integer, typeid2 integer, typeid3 integer, typeid4 integer)").Ok());
    REQUIRE(t.Db().Query("CREATE TABLE _refobjchar (id integer PRIMARY KEY, inventorysize integer, lvl integer)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refobjcommon VALUES ($1, 1, 'COS_T_TEST_CART', 1, 1, 2, 3, 2)", {std::int64_t(kCart)}).Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refobjchar VALUES (1, 40, 1)").Ok());
    int link = 2;
    for (const std::uint32_t id : thieves) {
        const assets::ObjectRow* r = characters->Find(id);
        REQUIRE(t.Db().Query("INSERT INTO _refobjcommon VALUES ($1, 1, $2, $3, 1, 2, 1, 2)", {std::int64_t(id), r->codename, link}).Ok());
        REQUIRE(t.Db().Query("INSERT INTO _refobjchar VALUES ($1, 0, $2)", {link, r->level}).Ok());
        ++link;
    }
    REQUIRE(t.Db().Query("CREATE TABLE _refregion (wregionid smallint PRIMARY KEY, isbattlefield integer, continentname varchar(129))").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refregion VALUES ($1, 1, 'Eu')", {std::int64_t(field)}).Ok());
    REQUIRE(t.Db().Query("CREATE TABLE _refdropgold (monlevel integer, goldmin integer)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refdropgold SELECT l, (3.5 * l + 25)::integer FROM generate_series(1, 140) l").Ok());
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);

    // A level-30 trader in its suit, its transport out beside it.
    const auto trader = [&](std::int64_t character) {
        REQUIRE(server::SaveJob(t.Db(), character, {game::JobType::Trader, 1, 0, 0, 0}));
        REQUIRE(server::SaveAlias(t.Db(), character, "Caravan1"));
        REQUIRE(server::SaveGold(t.Db(), character, 100'000));
        REQUIRE(t.Db().Query("INSERT INTO _inventory (charid, slot, itemid) SELECT $1, 8, b.itemid FROM _inventory b JOIN _items it ON it.id64 = b.itemid "
                             "WHERE b.charid = $1 AND it.refitemid = $2 LIMIT 1 ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                             {character, std::int64_t(kTraderSuit)})
                    .Ok());
        REQUIRE(t.Db().Query("UPDATE _inventory b SET itemid = 0 FROM _items it WHERE it.id64 = b.itemid AND b.charid = $1 AND it.refitemid = $2 AND b.slot <> 8",
                             {character, std::int64_t(kTraderSuit)})
                    .Ok());
    };
    auto a = s.Make("Hauler", 600, {Stored(kCartScroll, 1), Stored(kTraderSuit, 1)}, 30, field, 140, 900, trader);
    std::uint32_t npc = 0;
    while (npc == 0) {
        const auto m = Next<proto::EntityAppeared>(a);
        if (m.character.model == kSpecial) npc = m.entity;
    }
    a.client->Send(proto::UseItem{PlaceOf(a.bag, kCartScroll)});
    const auto cart = Next<proto::CosSummoned>(a);
    Next<proto::CosBag>(a);
    // Riding it: a hit on the rider strikes the transport.
    a.client->Send(proto::CosRide{cart.entity, true});
    // 40 goods into it: the caravan registered; its first wave 0.3 s on.
    a.client->Send(proto::ShopBuy{npc, 0, 0, 40});
    REQUIRE(Next<proto::TradeAnswer>(a).code == proto::TradeCode::Ok);
    std::set<std::uint32_t> bandits;
    proto::WorldPosition cartAt;
    bool cartSeen = false;
    bool struck = false;
    std::uint32_t struckTarget = 0;
    for (int i = 0; i < 1500 && !struck; ++i) {
        proto::EntityAppeared seen;
        if (a.client->Expect(seen, 10ms)) {
            if (seen.entity == cart.entity) {
                cartAt = seen.position;
                cartSeen = true;
            }
            if (seen.kind == proto::EntityKind::Monster && thieves.contains(seen.character.model)) {
                bandits.insert(seen.entity);
                const assets::ObjectRow* r = characters->Find(seen.character.model);
                MESSAGE("a bandit: " << r->codename << ", level " << r->level);
                // Levels 26..29 (max(30 - 4, 16) + 0..3 at one star; 30 at the 2 % doubled stars).
                CHECK(r->level >= 26);
                CHECK(r->level <= 30);
                // Within 15 m of the transport (it stands beside its owner).
                if (cartSeen) {
                    const proto::FlatPosition p = proto::ToFlat(seen.position), q = proto::ToFlat(cartAt);
                    CHECK(std::hypot(p.x - q.x, p.z - q.z) <= 151.0);
                }
            }
        }
        proto::EntityDamaged hit;
        while (a.client->Expect(hit, 5ms)) {
            if (bandits.contains(hit.attacker) && (hit.target == a.entity || hit.target == cart.entity)) {
                struck = true;
                struckTarget = hit.target;
            }
        }
    }
    // A wave each 0.3 s here, 1 or 2 a wave (one more at 50 %, doubled at 2 %).
    CHECK(cartSeen);
    CHECK(!bandits.empty());
    CHECK(struck);
    CHECK(struckTarget == cart.entity); // the rider's hits land on his mount
    s.game.Stop();
}

TEST_CASE("Game server: the jobs' PvP: a thief strikes a trader in the field, not one in town; a rider's hits land on his transport (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const char* dirEnv = std::getenv("SR_GAME_DIR");
    if (dirEnv == nullptr || *dirEnv == 0) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirEnv, error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    characters->Add(Cos(kCart, "COS_T_TEST_CART", 2));
    CosServer s(t);
    s.config.files = &fs;
    s.config.characters = characters;
    s.config.skills = skills; // the basic attack's skill
    constexpr std::uint32_t kThiefSuit = 900140;
    auto items = std::make_shared<assets::ItemTable>(*s.config.rules->items);
    items->Add(Suit(kThiefSuit, "ITEM_CH_M_TRADE_THIEF_TEST", 2));
    auto rules = std::make_shared<game::CharacterRules>(*s.config.rules);
    rules->items = items;
    s.config.rules = rules;
    // Jangan's south field (a battlefield) and a town region beside it (`_RefRegion.IsBattleField`).
    const std::uint16_t field = assets::RegionId(170, 97), town = assets::RegionId(168, 98); // Jangan's field east of it; its town
    REQUIRE(t.Db().Query("CREATE TABLE _refregion (wregionid smallint PRIMARY KEY, isbattlefield integer, continentname varchar(129))").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refregion VALUES ($1, 1, 'CHINA'), ($2, 0, 'CHINA')", {std::int64_t(field), std::int64_t(town)}).Ok());
    REQUIRE(t.Db().Query("CREATE TABLE _refdropgold (monlevel integer, goldmin integer)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refdropgold SELECT l, (3.5 * l + 25)::integer FROM generate_series(1, 140) l").Ok());
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    // Each in its league's suit from the start (place 8; no 10 s lock at login).
    const auto suited = [&](game::JobType type, std::uint32_t suit) {
        return [&t, type, suit](std::int64_t character) {
            REQUIRE(server::SaveJob(t.Db(), character, {type, 1, 5000, 0, 0}));
            REQUIRE(t.Db().Query("INSERT INTO _inventory (charid, slot, itemid) SELECT $1, 8, b.itemid FROM _inventory b JOIN _items it ON it.id64 = b.itemid "
                                 "WHERE b.charid = $1 AND it.refitemid = $2 LIMIT 1 ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                                 {character, std::int64_t(suit)})
                        .Ok());
            REQUIRE(t.Db().Query("UPDATE _inventory b SET itemid = 0 FROM _items it WHERE it.id64 = b.itemid AND b.charid = $1 AND it.refitemid = $2 AND b.slot <> 8",
                                 {character, std::int64_t(suit)})
                        .Ok());
        };
    };
    auto trader = s.Make("Hauler", 1000, {Stored(kTraderSuit, 1), Stored(kCartScroll, 1)}, 30, field, 0, 1000, suited(game::JobType::Trader, kTraderSuit));
    auto thief = s.Make("Robber", 1010, {Stored(kThiefSuit, 1)}, 30, field, 0, 1000, suited(game::JobType::Thief, kThiefSuit));
    auto townie = s.Make("Shopper", 1000, {Stored(kTraderSuit, 1)}, 30, town, 0, 1000, suited(game::JobType::Trader, kTraderSuit));
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
    // A trader in town: "Cannot attack in the safe zone." (0x3018).
    thief.client->Send(proto::AttackTarget{townie.entity});
    proto::SkillUseAnswer refused;
    for (int i = 0; i < 20 && refused.status == proto::SkillUseStatus::Ok; ++i) refused = Next<proto::SkillUseAnswer>(thief);
    CHECK(refused.status == proto::SkillUseStatus::NotBattlefield);
    // The trader on his transport (summoned before any fight: not in battle): the hits land on it.
    trader.client->Send(proto::UseItem{PlaceOf(trader.bag, kCartScroll)});
    const auto cart = Next<proto::CosSummoned>(trader);
    trader.client->Send(proto::CosRide{cart.entity, true});
    for (int i = 0; i < 50; ++i) s.game.Poll(2ms);
    thief.client->Send(proto::AttackTarget{trader.entity});
    bool cartStruck = false, riderStruck = false;
    for (int i = 0; i < 400 && !cartStruck; ++i) {
        proto::EntityDamaged hit;
        while (trader.client->Expect(hit, 10ms)) {
            if (hit.attacker != thief.entity) continue;
            if (hit.target == cart.entity && hit.damage > 0) cartStruck = true;
            if (hit.target == trader.entity && hit.damage > 0) riderStruck = true;
        }
    }
    CHECK(cartStruck);
    CHECK_FALSE(riderStruck);
    // Off it: the trader himself struck.
    trader.client->Send(proto::CosRide{cart.entity, false});
    for (int i = 0; i < 50; ++i) s.game.Poll(2ms);
    thief.client->Send(proto::AttackTarget{trader.entity});
    bool struck = false;
    for (int i = 0; i < 400 && !struck; ++i) {
        proto::EntityDamaged hit;
        if (trader.client->Expect(hit, 10ms) && hit.attacker == thief.entity && hit.target == trader.entity && hit.damage > 0) struck = true;
    }
    CHECK(struck);
    // A job's kill: the trader loses 3 x GoldUnit(30) = 486 job exp, the thief gains A x 1 = 162.
    thief.client->Send(proto::DevStrike{trader.entity, 1'000'000});
    CHECK(Next<proto::JobExpChanged>(trader).delta == -486);
    const auto won = Next<proto::JobExpChanged>(thief);
    CHECK(won.type == 2);
    CHECK(won.delta == 162);
    s.game.Stop();
}

TEST_CASE("Game server: the cargo: a killed transport drops its goods; the trader may not take them without a transport, a thief takes them into his (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const char* dirEnv = std::getenv("SR_GAME_DIR");
    if (dirEnv == nullptr || *dirEnv == 0) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirEnv, error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    // A frail cart (40 health): a few bare-handed hits fell it.
    assets::ObjectRow frail = Cos(kCart, "COS_T_TEST_CART", 2);
    frail.maxHp = 40;
    characters->Add(frail);
    auto teleports = std::make_shared<assets::TeleportTable>(); // the return points (a revival in town)
    REQUIRE_MESSAGE(teleports->Load(fs, error), error);
    CosServer s(t);
    s.config.files = &fs;
    s.config.characters = characters;
    s.config.skills = skills;
    s.config.teleports = teleports;
    constexpr std::uint32_t kThiefSuit = 900140, kGood = 900300, kSpecial = 2010;
    auto items = std::make_shared<assets::ItemTable>(*s.config.rules->items);
    items->Add(Suit(kThiefSuit, "ITEM_CH_M_TRADE_THIEF_TEST", 2));
    assets::ItemRow good;
    good.refObjId = kGood;
    good.codename = "ITEM_ETC_TRADE_CH_01";
    good.typeId = {3, 3, 8, 1};
    good.inService = true;
    good.maxStack = 40;
    good.sellPrice = 191;
    items->Add(good);
    auto rules = std::make_shared<game::CharacterRules>(*s.config.rules);
    rules->items = items;
    s.config.rules = rules;
    auto shops = std::make_shared<assets::ShopTable>();
    assets::NpcShop shop;
    assets::ShopGood g;
    g.slot = 0;
    g.itemCodename = good.codename;
    g.price = game::kGoodsPrice;
    shop.groups.push_back({"G", "G", {{"T", "T", 0, {g}}}});
    shops->Add("NPC_CH_SPECIAL", shop);
    s.config.shops = shops;
    const std::uint16_t field = assets::RegionId(168, 96); // the field south of Jangan (a thief's cart is not summoned in town)
    s.config.npcs.push_back({kSpecial, "NPC_CH_SPECIAL", {field, 1020.0f, 0.0f, 1000.0f}, 0.0f});
    REQUIRE(t.Db().Query("CREATE TABLE _refobjcommon (id integer PRIMARY KEY, codename128 varchar(129), link integer)").Ok());
    REQUIRE(t.Db().Query("CREATE TABLE _refobjchar (id integer PRIMARY KEY, inventorysize integer)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refobjcommon VALUES ($1, 'COS_T_TEST_CART', 1)", {std::int64_t(kCart)}).Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refobjchar VALUES (1, 40)").Ok());
    REQUIRE(t.Db().Query("CREATE TABLE _refdropgold (monlevel integer, goldmin integer)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refdropgold SELECT l, (3.5 * l + 25)::integer FROM generate_series(1, 140) l").Ok());
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    const auto suited = [&](game::JobType type, std::uint32_t suit, const char* alias) {
        return [&t, type, suit, alias](std::int64_t character) {
            REQUIRE(server::SaveJob(t.Db(), character, {type, 1, 0, 0, 0}));
            REQUIRE(server::SaveAlias(t.Db(), character, alias));
            REQUIRE(server::SaveGold(t.Db(), character, 100'000));
            REQUIRE(t.Db().Query("INSERT INTO _inventory (charid, slot, itemid) SELECT $1, 8, b.itemid FROM _inventory b JOIN _items it ON it.id64 = b.itemid "
                                 "WHERE b.charid = $1 AND it.refitemid = $2 LIMIT 1 ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                                 {character, std::int64_t(suit)})
                        .Ok());
            REQUIRE(t.Db().Query("UPDATE _inventory b SET itemid = 0 FROM _items it WHERE it.id64 = b.itemid AND b.charid = $1 AND it.refitemid = $2 AND b.slot <> 8",
                                 {character, std::int64_t(suit)})
                        .Ok());
        };
    };
    auto trader = s.Make("Hauler", 1000, {Stored(kTraderSuit, 1), Stored(kCartScroll, 1)}, 30, field, 0, 1000,
                         suited(game::JobType::Trader, kTraderSuit, "Hauler1"));
    auto thief = s.Make("Robber", 1010, {Stored(kThiefSuit, 1), Stored(kCartScroll, 1)}, 30, field, 0, 1000,
                        suited(game::JobType::Thief, kThiefSuit, "Robber1"));
    std::uint32_t npc = 0;
    while (npc == 0) {
        const auto m = Next<proto::EntityAppeared>(trader);
        if (m.character.model == kSpecial) npc = m.entity;
    }
    // The trader's 40 goods in his cart.
    trader.client->Send(proto::UseItem{PlaceOf(trader.bag, kCartScroll)});
    const auto cart = Next<proto::CosSummoned>(trader);
    trader.client->Send(proto::ShopBuy{npc, 0, 0, 40});
    REQUIRE(Next<proto::TradeAnswer>(trader).code == proto::TradeCode::Ok);
    // The thief fells the cart: its goods on the ground (no owner's mark), the cart gone.
    thief.client->Send(proto::AttackTarget{cart.entity});
    std::uint32_t dropped = 0;
    for (int i = 0; i < 1500 && dropped == 0; ++i) {
        proto::EntityAppeared seen;
        if (thief.client->Expect(seen, 10ms) && seen.kind == proto::EntityKind::GroundItem && seen.character.model == kGood) dropped = seen.entity;
    }
    REQUIRE(dropped != 0);
    // The trader, his transport dead: none to take them into (0x1861).
    trader.client->Send(proto::PickUp{dropped});
    proto::ItemMoved answer;
    for (int i = 0; i < 20 && answer.code == 0; ++i) answer = Next<proto::ItemMoved>(trader);
    CHECK(answer.status == proto::ItemStatus::JobSuit);
    CHECK(answer.code == 0x1861);
    // The thief with his own transport out: into it, the trader's alias still its maker.
    thief.client->Send(proto::UseItem{PlaceOf(thief.bag, kCartScroll)});
    const auto own = Next<proto::CosSummoned>(thief);
    thief.client->Send(proto::PickUp{dropped});
    proto::CosBag bag;
    for (int i = 0; i < 40 && bag.items.empty(); ++i) {
        bag = Next<proto::CosBag>(thief);
        if (bag.entity != own.entity) bag.items.clear();
    }
    REQUIRE(bag.items.size() == 1);
    CHECK(bag.items[0].item == kGood);
    CHECK(bag.items[0].quantity == 40);
    CHECK(bag.items[0].creator == "Hauler1");
    // The thief dies riding it: he comes off, the transport stays with the goods (the user's account, 2026-11-22).
    thief.client->Send(proto::DevDie{});
    proto::CosRiding off;
    for (int i = 0; i < 20 && (off.mount != own.entity || off.riding); ++i) off = Next<proto::CosRiding>(thief);
    CHECK(off.mount == own.entity);
    CHECK_FALSE(off.riding);
    proto::CosDismissed gone;
    CHECK_FALSE(thief.client->Expect(gone, 1500ms));
    // Back to life at his return point: the transport left behind, its goods on the ground where it stood.
    thief.client->Send(proto::Resurrect{1});
    REQUIRE(thief.client->Expect(gone, 10s));
    CHECK(gone.entity == own.entity);
    std::uint32_t again = 0;
    for (int i = 0; i < 1500 && again == 0; ++i) {
        proto::EntityAppeared seen;
        if (trader.client->Expect(seen, 10ms) && seen.kind == proto::EntityKind::GroundItem && seen.character.model == kGood) again = seen.entity;
    }
    CHECK(again != 0);
    s.game.Stop();
}

TEST_CASE("Game server: the thieves' town scroll: a thief's only, with his transport along; a ridden thief's transport kept out of a town (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const char* dirEnv = std::getenv("SR_GAME_DIR");
    if (dirEnv == nullptr || *dirEnv == 0) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirEnv, error), error);
    auto teleports = std::make_shared<assets::TeleportTable>();
    REQUIRE_MESSAGE(teleports->Load(fs, error), error);
    const assets::TeleportPoint* den = teleports->PointNamed("GATE_TD");
    REQUIRE(den != nullptr);
    CosServer s(t);
    s.config.teleports = teleports;
    constexpr std::uint32_t kThiefSuit = 900140, kDenScroll = 900141;
    auto items = std::make_shared<assets::ItemTable>(*s.config.rules->items);
    items->Add(Suit(kThiefSuit, "ITEM_CH_M_TRADE_THIEF_TEST", 2));
    assets::ItemRow scroll; // ITEM_ETC_SCROLL_RETURN_THIEFDEN_01 (3/3/3/1, THIEFDEN), read in a tenth of a second here
    scroll.refObjId = kDenScroll;
    scroll.codename = "ITEM_ETC_SCROLL_RETURN_THIEFDEN_TEST";
    scroll.typeId = {3, 3, 3, 1};
    scroll.inService = true;
    scroll.maxStack = 10;
    scroll.params[0] = 100;
    scroll.paramTexts[2] = "THIEFDEN";
    scroll.paramTexts[3] = "STORE_TD_GATE"; // the building whose point it leads to (GATE_TD)
    items->Add(scroll);
    auto rules = std::make_shared<game::CharacterRules>(*s.config.rules);
    rules->items = items;
    s.config.rules = rules;
    // The field south of Jangan and Jangan's town above it (`_RefRegion.IsBattleField`).
    const std::uint16_t field = assets::RegionId(168, 96), town = assets::RegionId(168, 97);
    REQUIRE(t.Db().Query("CREATE TABLE _refregion (wregionid smallint PRIMARY KEY, isbattlefield integer, continentname varchar(129))").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refregion VALUES ($1, 1, 'CHINA'), ($2, 0, 'CHINA'), ($3, 0, 'CHINA')",
                         {std::int64_t(field), std::int64_t(town), std::int64_t(den->region)})
                .Ok());
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    const auto suited = [&](game::JobType type, std::uint32_t suit) {
        return [&t, type, suit](std::int64_t character) {
            REQUIRE(server::SaveJob(t.Db(), character, {type, 1, 0, 0, 0}));
            REQUIRE(t.Db().Query("INSERT INTO _inventory (charid, slot, itemid) SELECT $1, 8, b.itemid FROM _inventory b JOIN _items it ON it.id64 = b.itemid "
                                 "WHERE b.charid = $1 AND it.refitemid = $2 LIMIT 1 ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                                 {character, std::int64_t(suit)})
                        .Ok());
            REQUIRE(t.Db().Query("UPDATE _inventory b SET itemid = 0 FROM _items it WHERE it.id64 = b.itemid AND b.charid = $1 AND it.refitemid = $2 AND b.slot <> 8",
                                 {character, std::int64_t(suit)})
                        .Ok());
        };
    };
    auto trader = s.Make("Hauler", 1000, {Stored(kTraderSuit, 1), Stored(kDenScroll, 1), Stored(kReturnScroll, 1), Stored(kCartScroll, 1)}, 30, field,
                         0, 1000, suited(game::JobType::Trader, kTraderSuit));
    auto thief = s.Make("Robber", 1010, {Stored(kThiefSuit, 1), Stored(kDenScroll, 1), Stored(kCartScroll, 1)}, 30, field, 0, 1000,
                        suited(game::JobType::Thief, kThiefSuit));
    // A trader may not read it (0x186A).
    trader.client->Send(proto::UseItem{PlaceOf(trader.bag, kDenScroll)});
    CHECK(Next<proto::ItemRefused>(trader).code == 0x186A);
    // A plain return scroll not with a transport out (0x185E).
    trader.client->Send(proto::UseItem{PlaceOf(trader.bag, kCartScroll)});
    Next<proto::CosSummoned>(trader);
    trader.client->Send(proto::UseItem{PlaceOf(trader.bag, kReturnScroll)});
    CHECK(Next<proto::ItemRefused>(trader).code == 0x185E);
    // The thief on his transport may not take it into Jangan's town.
    thief.client->Send(proto::UseItem{PlaceOf(thief.bag, kCartScroll)});
    const auto cart = Next<proto::CosSummoned>(thief);
    thief.client->Send(proto::CosRide{cart.entity, true});
    for (int i = 0; i < 50; ++i) s.game.Poll(2ms);
    thief.client->Send(proto::MoveTo{{town, 1000.0f, 0.0f, 100.0f}});
    CHECK(Next<proto::CosRefused>(thief).text == "UIIT_MSG_JSERR_ROBBER_CANT_ENTER_TOWN_WITH_CART");
    // His scroll read: he and his transport at the thieves' town's gate.
    thief.client->Send(proto::UseItem{PlaceOf(thief.bag, kDenScroll)});
    CHECK(Next<proto::ReturnStarted>(thief).ms == 100);
    proto::WorldEntered moved;
    bool arrived = false;
    for (int i = 0; i < 400 && !arrived; ++i) {
        if (thief.client->Expect(moved, 10ms)) arrived = moved.position.region == den->region;
    }
    CHECK(arrived);
    // The transport beside him there.
    proto::EntityAppeared seen;
    bool cartThere = false;
    for (int i = 0; i < 200 && !cartThere; ++i) {
        if (thief.client->Expect(seen, 10ms) && seen.entity == cart.entity) cartThere = seen.position.region == den->region;
    }
    CHECK(cartThere);
    s.game.Stop();
}

TEST_CASE("The jobs' week: the thieves' pool shared by contribution, the rankings written, the contributions reset") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Picker", 1000, {}, 30);
    auto b = s.Make("Snatcher", 1010, {}, 40);
    s.game.Stop();
    REQUIRE(server::SaveJob(t.Db(), a.character, {game::JobType::Thief, 2, 900, 100, 0}));
    REQUIRE(server::SaveJob(t.Db(), b.character, {game::JobType::Thief, 3, 50, 300, 5}));
    REQUIRE(server::SaveAlias(t.Db(), a.character, "Picky"));
    REQUIRE(server::SaveAlias(t.Db(), b.character, "Snatchy"));
    REQUIRE(t.Db().Query("CREATE TABLE _trijobrewards (jobtype integer PRIMARY KEY, reward integer)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _trijobrewards VALUES (2, 1000), (3, 0)").Ok());
    REQUIRE(t.Db().Query("CREATE TABLE _refobjcommon (id integer PRIMARY KEY, country integer)").Ok()); // the races' countries
    for (const char* job : {"trader", "robber", "hunter"}) {
        REQUIRE(t.Db().Query(std::string("CREATE TABLE tab_refranking_") + job + "activity (rank integer, nickname varchar(17), joblevel integer, jobexp integer, country integer)").Ok());
        REQUIRE(t.Db().Query(std::string("CREATE TABLE tab_refranking_") + job + "contribution (rank integer, nickname varchar(17), joblevel integer, contribution integer)").Ok());
    }
    std::string error;
    REQUIRE_MESSAGE(server::RunJobWeek(t.Db(), error), error);
    // 1,000 x 100 / 400 = 250; 1,000 x 300 / 400 = 750 on top of the 5 kept.
    CHECK(t.Db().Query("SELECT reward FROM _chartrijob WHERE charid = $1", {a.character}).Int(0, 0) == 250);
    CHECK(t.Db().Query("SELECT reward FROM _chartrijob WHERE charid = $1", {b.character}).Int(0, 0) == 755);
    CHECK(t.Db().Query("SELECT reward FROM _trijobrewards WHERE jobtype = 2").Int(0, 0) == 0);
    CHECK(t.Db().Query("SELECT count(*) FROM _chartrijob WHERE contribution <> 0").Int(0, 0) == 0);
    // The rankings: activity by level then exp, contribution by contribution.
    std::vector<server::JobRankRow> rows;
    REQUIRE(server::LoadJobRanking(t.Db(), game::JobType::Thief, 0, rows, error));
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].alias == "Snatchy");
    CHECK(rows[0].level == 3);
    CHECK(rows[1].alias == "Picky");
    CHECK(rows[1].value == 900);
    REQUIRE(server::LoadJobRanking(t.Db(), game::JobType::Thief, 1, rows, error));
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].alias == "Snatchy");
    CHECK(rows[0].value == 300);
    // The outcome collected once.
    CHECK(server::TakeJobReward(t.Db(), b.character) == 755);
    CHECK(server::TakeJobReward(t.Db(), b.character) == 0);
}

TEST_CASE("Game server: the rankings and the week's outcome at the league keepers") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    constexpr std::uint32_t kDoctor = 2027, kThiefKeeper = 2178;
    s.config.npcs.push_back({kDoctor, "NPC_CH_DOCTOR", {assets::RegionId(168, 97), 1050.0f, 0.0f, 1000.0f}, 0.0f});
    s.config.npcs.push_back({kThiefKeeper, "NPC_TD_THIEF_SELL", {assets::RegionId(168, 97), 1080.0f, 0.0f, 1000.0f}, 0.0f});
    for (const char* job : {"trader", "robber", "hunter"}) {
        REQUIRE(t.Db().Query(std::string("CREATE TABLE tab_refranking_") + job + "activity (rank integer, nickname varchar(17), joblevel integer, jobexp integer, country integer)").Ok());
        REQUIRE(t.Db().Query(std::string("CREATE TABLE tab_refranking_") + job + "contribution (rank integer, nickname varchar(17), joblevel integer, contribution integer)").Ok());
    }
    REQUIRE(t.Db().Query("INSERT INTO tab_refranking_robberactivity VALUES (1, 'Snatchy', 3, 50, 1), (2, 'Picky', 2, 900, 0)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO tab_refranking_robbercontribution VALUES (1, 'Snatchy', 3, 300)").Ok());
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    // A thief with 4,321 gold to collect, and one in no league.
    auto a = s.Make("Robber", 1000, {}, 30, 0, 0, 1000, [&](std::int64_t character) {
        REQUIRE(server::SaveGold(t.Db(), character, 1'000));
        REQUIRE(server::SaveJob(t.Db(), character, {game::JobType::Thief, 2, 900, 100, 4'321}));
    });
    auto b = s.Make("Nobody", 1010, {}, 30);
    // The entry's league: marked as the entry's, with the week's contribution so far (the ranking's own entry).
    const auto entered = Next<proto::JobInfoMessage>(a);
    CHECK(entered.entry);
    CHECK(entered.contribution == 100);
    std::uint32_t doctor = 0, thiefKeeper = 0;
    while (doctor == 0 || thiefKeeper == 0) {
        const auto m = Next<proto::EntityAppeared>(a);
        if (m.character.model == kDoctor) doctor = m.entity;
        if (m.character.model == kThiefKeeper) thiefKeeper = m.entity;
    }
    const auto refused = [&](CosServer::Player& p, proto::JobRequest request) {
        const auto m = Next<proto::JobAnswer>(p);
        CHECK(m.request == request);
        return m.code;
    };

    // A ranking: any league's at any keeper, to anyone; the type and the kind checked.
    a.client->Send(proto::JobRankQuery{doctor, 2, 0});
    const auto activity = Next<proto::JobRanking>(a);
    CHECK(activity.type == 2);
    CHECK(activity.kind == 0);
    REQUIRE(activity.rows.size() == 2);
    CHECK(activity.rows[0].alias == "Snatchy");
    CHECK(activity.rows[0].country == 1);
    CHECK(activity.rows[1].value == 900);
    b.client->Send(proto::JobRankQuery{doctor, 2, 1});
    const auto contribution = Next<proto::JobRanking>(b);
    REQUIRE(contribution.rows.size() == 1);
    CHECK(contribution.rows[0].value == 300);
    a.client->Send(proto::JobRankQuery{doctor, 3, 0});
    CHECK(Next<proto::JobRanking>(a).rows.empty()); // the hunters' none yet
    a.client->Send(proto::JobRankQuery{doctor, 9, 0});
    CHECK(refused(a, proto::JobRequest::Rank) == proto::JobCode::NotInLeague);
    a.client->Send(proto::JobRankQuery{doctor, 2, 2});
    CHECK(refused(a, proto::JobRequest::Rank) == proto::JobCode::NotOffered);

    // The outcome: a thief's or hunter's, at its own league's keeper; asked, collected once.
    b.client->Send(proto::JobOutcome{thiefKeeper, 0});
    CHECK(refused(b, proto::JobRequest::Outcome) == proto::JobCode::NotInLeague);
    a.client->Send(proto::JobOutcome{doctor, 0});
    CHECK(refused(a, proto::JobRequest::Outcome) == proto::JobCode::NotOffered);
    a.client->Send(proto::JobOutcome{thiefKeeper, 0});
    const auto asked = Next<proto::JobOutcomeAnswer>(a);
    CHECK(asked.action == 0);
    CHECK(asked.amount == 4'321);
    a.client->Send(proto::JobOutcome{thiefKeeper, 1});
    CHECK(Next<proto::GoldChanged>(a).gold == 5'321);
    const auto paid = Next<proto::JobOutcomeAnswer>(a);
    CHECK(paid.action == 1);
    CHECK(paid.amount == 4'321);
    CHECK(t.Db().Query("SELECT reward FROM _chartrijob WHERE charid = $1", {a.character}).Int(0, 0) == 0);
    a.client->Send(proto::JobOutcome{thiefKeeper, 1});
    CHECK(refused(a, proto::JobRequest::Outcome) == proto::JobCode::NoOutcome);
    a.client->Send(proto::JobOutcome{thiefKeeper, 0});
    CHECK(Next<proto::JobOutcomeAnswer>(a).amount == 0); // asked again: nothing, still told
    s.game.Stop();
}

TEST_CASE("Game server: the jobs' rest: safe trade, the logout in a suit, parties and guilds by job, a dropped connection kept") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    s.config.logoutCountdown = 1s;
    constexpr std::uint32_t kGood = 900300, kChSpecial = 2010, kThiefSuit = 900140, kGuildNpc = 2080;
    auto items = std::make_shared<assets::ItemTable>(*s.config.rules->items);
    assets::ItemRow good;
    good.refObjId = kGood;
    good.codename = "ITEM_ETC_TRADE_CH_01";
    good.typeId = {3, 3, 8, 1};
    good.inService = true;
    good.maxStack = 40;
    good.sellPrice = 191;
    good.canTrade = false;
    items->Add(good);
    items->Add(Suit(kThiefSuit, "ITEM_CH_M_TRADE_THIEF_TEST", 2));
    auto rules = std::make_shared<game::CharacterRules>(*s.config.rules);
    rules->items = items;
    s.config.rules = rules;
    auto shops = std::make_shared<assets::ShopTable>();
    assets::NpcShop shop;
    assets::ShopGood g;
    g.slot = 0;
    g.itemCodename = good.codename;
    g.price = game::kGoodsPrice;
    shop.groups.push_back({"G", "G", {{"T", "T", 0, {g}}}});
    shops->Add("NPC_CH_SPECIAL", shop);
    s.config.shops = shops;
    s.config.towns = JobTowns();
    // All in the field east of Jangan (the town, 167..169 x 97..98, is safe ground: no attack there).
    s.config.npcs.push_back({kChSpecial, "NPC_CH_SPECIAL", {assets::RegionId(170, 97), 1050.0f, 0.0f, 1000.0f}, 0.0f});
    s.config.npcs.push_back({kGuildNpc, "NPC_CH_GUILD_TEST", {assets::RegionId(170, 97), 1020.0f, 0.0f, 1000.0f}, 0.0f});
    // Jangan's south field, a battlefield (`_RefRegion.IsBattleField`); the transport's bag; the gold levels.
    const std::uint16_t field = assets::RegionId(170, 97);
    REQUIRE(t.Db().Query("CREATE TABLE _refregion (wregionid smallint PRIMARY KEY, isbattlefield integer, continentname varchar(129))").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refregion VALUES ($1, 1, 'CHINA')", {std::int64_t(field)}).Ok());
    REQUIRE(t.Db().Query("CREATE TABLE _refobjcommon (id integer PRIMARY KEY, codename128 varchar(129), link integer)").Ok());
    REQUIRE(t.Db().Query("CREATE TABLE _refobjchar (id integer PRIMARY KEY, inventorysize integer)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refobjcommon VALUES ($1, 'COS_T_TEST_CART', 1)", {std::int64_t(kCart)}).Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refobjchar VALUES (1, 40)").Ok());
    REQUIRE(t.Db().Query("CREATE TABLE _refdropgold (monlevel integer, goldmin integer)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _refdropgold SELECT l, (3.5 * l + 25)::integer FROM generate_series(1, 140) l").Ok());
    REQUIRE(t.Db().Query("CREATE TABLE _trijobrewards (jobtype integer PRIMARY KEY, reward integer)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _trijobrewards VALUES (2, 0), (3, 0)").Ok());
    // A gate beside the field's players, its point leading to a place in region 168,96 (a ferry that lets transports cross).
    const auto row = [](std::initializer_list<std::string> cells) {
        std::string out;
        for (const std::string& cell : cells) out += (out.empty() ? "" : "\t") + cell;
        return out + "\n";
    };
    std::string gate = "1\t9001\tSTORE_TEST_GATE";
    for (int col = 3; col < 58; ++col) {
        gate += "\t";
        if (col == 41) gate += std::to_string(field);
        else if (col == 43) gate += "1000";
        else if (col == 45) gate += "1010";
        else gate += "0";
    }
    std::string link = "1\t1\t2\t0";
    for (int col = 4; col < 21; ++col) link += "\t0";
    auto teleports = std::make_shared<assets::TeleportTable>();
    teleports->Decode(gate + "\n",
                      row({"1", "1", "GATE_A", "9001", "SN_A", "0", "0", "0", "0", "0", "0", "0", "1"}) +
                          row({"1", "2", "GATE_B", "0", "SN_B", std::to_string(assets::RegionId(168, 96)), "1000", "0", "1000", "0", "0", "0", "1"}),
                      link + "\n");
    s.config.teleports = teleports;
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);

    // Each in its league's suit from the start (place 8), the trader with its safe trade row (yesterday's: reset today).
    const auto suited = [&](game::JobType type, std::uint32_t suit, const std::string& alias) {
        return [&t, type, suit, alias](std::int64_t character) {
            REQUIRE(server::SaveJob(t.Db(), character, {type, 1, 5000, 0, 0}));
            if (!alias.empty()) REQUIRE(server::SaveAlias(t.Db(), character, alias));
            REQUIRE(server::SaveGold(t.Db(), character, 600'000));
            REQUIRE(t.Db().Query("INSERT INTO _inventory (charid, slot, itemid) SELECT $1, 8, b.itemid FROM _inventory b JOIN _items it ON it.id64 = b.itemid "
                                 "WHERE b.charid = $1 AND it.refitemid = $2 LIMIT 1 ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                                 {character, std::int64_t(suit)})
                        .Ok());
            REQUIRE(t.Db().Query("UPDATE _inventory b SET itemid = 0 FROM _items it WHERE it.id64 = b.itemid AND b.charid = $1 AND it.refitemid = $2 AND b.slot <> 8",
                                 {character, std::int64_t(suit)})
                        .Ok());
            if (type == game::JobType::Trader) {
                REQUIRE(t.Db().Query("INSERT INTO _chartrijobsafetrade VALUES ($1, 5, 0, localtimestamp - interval '1 day')", {character}).Ok());
            }
        };
    };
    auto trader = s.Make("Hauler", 1000, {Stored(kTraderSuit, 1), Stored(kCartScroll, 1)}, 30, field, 0, 1000, suited(game::JobType::Trader, kTraderSuit, "Caravan1"));
    auto thief = s.Make("Robber", 1010, {Stored(kThiefSuit, 1)}, 30, field, 0, 1000, suited(game::JobType::Thief, kThiefSuit, ""));
    auto plain = s.Make("Walker", 1020, {}, 30, field);
    std::uint32_t ch = 0, guildNpc = 0;
    while (ch == 0 || guildNpc == 0) {
        const auto m = Next<proto::EntityAppeared>(trader);
        if (m.character.model == kChSpecial) ch = m.entity;
        if (m.character.model == kGuildNpc) guildNpc = m.entity;
    }
    // At log-in: not active (0x40).
    const auto login = Next<proto::SafeTradeState>(trader);
    CHECK(login.active == 0);

    // Guilds (0x4C57 / 0x4C56): no guild made in a suit (after the level and the gold).
    trader.client->Send(proto::GuildCreate{guildNpc, "Movers"});
    const auto made = Next<proto::GuildAnswer>(trader);
    CHECK(made.action == proto::GuildAction::Create);
    CHECK(made.error == std::uint16_t(game::GuildError::CreateInSuit));

    // Parties (0x2C23): the invitee asked first, the mismatch refused to both once it accepts: a
    // thief and a trader not; a trader and one with no job not either.
    const auto refusedParty = [&](CosServer::Player& inviter, CosServer::Player& invitee) {
        inviter.client->Send(proto::PartyInvite{invitee.entity, 0});
        CHECK(Next<proto::PartyInvited>(invitee).from == inviter.entity);
        invitee.client->Send(proto::PartyInviteAnswer{1});
        const auto toInviter = Next<proto::PartyResult>(inviter), toInvitee = Next<proto::PartyResult>(invitee);
        return toInviter.status == proto::PartyStatus::JobMismatch && toInvitee.status == proto::PartyStatus::JobMismatch;
    };
    CHECK(refusedParty(thief, trader));
    CHECK(refusedParty(trader, plain));
    // Party matching: the purposes by job; a suit without an alias refused (code 5).
    proto::PartyMatchForm form;
    form.purpose = 0;
    form.levelMin = 1;
    form.levelMax = 100;
    form.title = "Go";
    trader.client->Send(proto::PartyMatchRegister{form});
    CHECK(Next<proto::PartyMatchAnswer>(trader).status == proto::PartyStatus::JobMismatch);
    form.purpose = 3;
    thief.client->Send(proto::PartyMatchRegister{form});
    CHECK(Next<proto::PartyMatchAnswer>(thief).status == proto::PartyStatus::NoAlias);
    form.purpose = 2;
    trader.client->Send(proto::PartyMatchRegister{form});
    CHECK(Next<proto::PartyMatchAnswer>(trader).status == proto::PartyStatus::Ok);
    trader.client->Send(proto::PartyMatchDelete{});

    // Safe trade: one star of goods in the transport, the specialty NPC's talk (yesterday's count reset to 0), its shop
    // put away: begun, 1 of 8.
    trader.client->Send(proto::UseItem{PlaceOf(trader.bag, kCartScroll)});
    const auto cart = Next<proto::CosSummoned>(trader);
    CHECK(Next<proto::CosBag>(trader).items.empty());
    // Boarded when summoned: off it, on his own feet (a rider attacks nobody; his own attack is tried below).
    REQUIRE(Next<proto::CosRiding>(trader).riding);
    trader.client->Send(proto::CosRide{cart.entity, false});
    REQUIRE_FALSE(Next<proto::CosRiding>(trader).riding);
    trader.client->Send(proto::ShopBuy{ch, 0, 0, 40});
    REQUIRE(Next<proto::TradeAnswer>(trader).code == proto::TradeCode::Ok);
    CHECK(Next<proto::TradeScale>(trader).stars == 1);
    const auto bag = Next<proto::CosBag>(trader);
    REQUIRE_FALSE(bag.items.empty());
    // A logout with goods in a COS: 60 s; the player's cancel stops it.
    trader.client->Send(proto::LogoutRequest{proto::LogoutKind::Restart});
    auto out = Next<proto::LogoutAnswer>(trader);
    CHECK(out.status == proto::LogoutStatus::Counting);
    CHECK(out.seconds == 60);
    trader.client->Send(proto::LogoutCancel{});
    CHECK(Next<proto::LogoutAnswer>(trader).status == proto::LogoutStatus::Cancelled);
    // The logout in combat (a hit within 20 s): a suit held (0x801); one with no job, not in combat, free.
    thief.client->Send(proto::DevStrike{trader.entity, 1});
    for (int i = 0; i < 50; ++i) s.game.Poll(2ms);
    trader.client->Send(proto::LogoutRequest{proto::LogoutKind::Exit});
    CHECK(Next<proto::LogoutAnswer>(trader).status == proto::LogoutStatus::InCombat);
    thief.client->Send(proto::LogoutRequest{proto::LogoutKind::Exit});
    CHECK(Next<proto::LogoutAnswer>(thief).status == proto::LogoutStatus::InCombat);
    plain.client->Send(proto::LogoutRequest{proto::LogoutKind::Exit});
    out = Next<proto::LogoutAnswer>(plain);
    CHECK(out.status == proto::LogoutStatus::Counting);
    CHECK(out.seconds == 1);
    plain.client->Send(proto::LogoutCancel{});
    CHECK(Next<proto::LogoutAnswer>(plain).status == proto::LogoutStatus::Cancelled);
    trader.client->Send(proto::NpcTalk{ch});
    const auto menu = Next<proto::NpcMenu>(trader);
    CHECK(menu.safe.option == 0);
    CHECK(menu.safe.limit == 8);
    CHECK(menu.safe.count == 0);
    trader.client->Send(proto::ShopClose{ch});
    auto safe = Next<proto::SafeTradeState>(trader);
    CHECK(safe.active == 1);
    CHECK(safe.code == 0x50);
    CHECK(safe.count == 1);
    CHECK(safe.limit == 8);
    // Again while on: not counted.
    trader.client->Send(proto::ShopClose{ch});
    CHECK(Next<proto::SafeTradeState>(trader).count == 1);
    std::int64_t kept = 0;
    for (int i = 0; i < 2000 && kept != 1; ++i) {
        s.game.Poll(2ms);
        kept = t.Db().Query("SELECT ablecount * 10 + status FROM _chartrijobsafetrade WHERE charid = $1", {trader.character}).Int(0, 0) == 11 ? 1 : 0;
    }
    CHECK(kept == 1);
    // The protection: the thief neither on him nor on his transport (0x3024); he not on the thief (0x3006).
    const auto refusedOn = [&](CosServer::Player& p, std::uint32_t target) {
        p.client->Send(proto::AttackTarget{target});
        proto::SkillUseAnswer r;
        for (int i = 0; i < 20 && r.status == proto::SkillUseStatus::Ok; ++i) r = Next<proto::SkillUseAnswer>(p);
        return r.status;
    };
    CHECK(refusedOn(thief, trader.entity) == proto::SkillUseStatus::SafeTrade);
    CHECK(refusedOn(thief, cart.entity) == proto::SkillUseStatus::SafeTrade);
    CHECK(refusedOn(trader, thief.entity) == proto::SkillUseStatus::JobMonster);

    // A sale at a specialty NPC ends it (0x40).
    trader.client->Send(proto::TradeSell{ch, bag.items[0].slot, 0});
    safe = Next<proto::SafeTradeState>(trader);
    CHECK(safe.active == 0);
    CHECK(safe.code == 0x40);

    // A dropped connection: the character stays the logout's second (1 s) before those near see it go.
    const auto dropped = std::chrono::steady_clock::now();
    plain.client->Connection().Close();
    proto::EntityGone gone;
    bool left = false;
    while (!left && std::chrono::steady_clock::now() - dropped < 5s) {
        if (thief.client->Expect(gone, 50ms) && gone.entity == plain.entity) left = true;
    }
    CHECK(left);
    CHECK(std::chrono::steady_clock::now() - dropped >= 900ms);

    // Through a gate his transport goes along, beside him at the far side.
    trader.client->Send(proto::UseGate{9001, 2});
    proto::WorldEntered moved;
    bool arrived = false;
    for (int i = 0; i < 400 && !arrived; ++i) {
        if (trader.client->Expect(moved, 10ms)) arrived = moved.position.region == assets::RegionId(168, 96);
    }
    CHECK(arrived);
    proto::EntityAppeared seen;
    bool cartThere = false;
    for (int i = 0; i < 200 && !cartThere; ++i) {
        if (trader.client->Expect(seen, 10ms) && seen.entity == cart.entity) cartThere = seen.position.region == assets::RegionId(168, 96);
    }
    CHECK(cartThere);
    s.game.Stop();
}

TEST_CASE("Game server: a ferry's ticket seller: its own teleport point's crossings, as a gate's (the user's report 2026-11-14)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    constexpr std::uint32_t kFerry = 900301;
    const auto row = [](std::initializer_list<std::string> cells) {
        std::string out;
        for (const std::string& cell : cells) out += (out.empty() ? "" : "\t") + cell;
        return out + "\n";
    };
    // Its links: to the shore, free of restrictions (as every ferry's but Samarkand's); to a dock behind restriction 2, no
    // transport out (as the towns' gates).
    const auto link = [](int to, int restriction) {
        std::string out = "1\t1\t" + std::to_string(to) + "\t100\t0\t0\t" + std::to_string(restriction);
        for (int col = 7; col < 21; ++col) out += "\t0";
        return out + "\n";
    };
    // The seller's point (teleportdata's owner its model, as NPC_CH_FERRY's GATE_NPC_CH_FERRY).
    auto teleports = std::make_shared<assets::TeleportTable>();
    teleports->Decode("",
                      row({"1", "1", "GATE_NPC_TEST_FERRY", std::to_string(kFerry), "SN_FERRY", "0", "0", "0", "0", "0", "0", "0", "1"}) +
                          row({"1", "2", "GATE_TEST_SHORE", "0", "SN_SHORE", std::to_string(assets::RegionId(170, 98)), "1000", "0", "1000", "0", "0", "0", "1"}) +
                          row({"1", "3", "GATE_TEST_DOCK", "0", "SN_DOCK", std::to_string(assets::RegionId(170, 98)), "500", "0", "500", "0", "0", "0", "1"}),
                      link(2, 0) + link(3, 2));
    s.config.teleports = teleports;
    s.config.npcs.push_back({kFerry, "NPC_CH_FERRY", {assets::RegionId(168, 97), 1050.0f, 0.0f, 1000.0f}, 0.0f});
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    // A trader in his suit, with his fare and a transport's scroll.
    const auto trader = [&](std::int64_t character) {
        REQUIRE(server::SaveJob(t.Db(), character, {game::JobType::Trader, 1, 0, 0, 0}));
        REQUIRE(server::SaveGold(t.Db(), character, 1000));
        REQUIRE(t.Db().Query("INSERT INTO _inventory (charid, slot, itemid) SELECT $1, 8, b.itemid FROM _inventory b JOIN _items it ON it.id64 = b.itemid "
                             "WHERE b.charid = $1 AND it.refitemid = $2 LIMIT 1 ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                             {character, std::int64_t(kTraderSuit)})
                    .Ok());
        REQUIRE(t.Db().Query("UPDATE _inventory b SET itemid = 0 FROM _items it WHERE it.id64 = b.itemid AND b.charid = $1 AND it.refitemid = $2 AND b.slot <> 8",
                             {character, std::int64_t(kTraderSuit)})
                    .Ok());
    };
    auto a = s.Make("Sailor", 1000, {Stored(kCartScroll, 1), Stored(kTraderSuit, 1)}, 30, 0, 0, 1000, trader);
    std::uint32_t npc = 0;
    while (npc == 0) {
        const auto m = Next<proto::EntityAppeared>(a);
        if (m.character.model == kFerry) npc = m.entity;
    }
    // Its menu has the teleport row (no return point: its column 10 is 0).
    a.client->Send(proto::NpcTalk{npc});
    const auto menu = Next<proto::NpcMenu>(a);
    CHECK((menu.services & proto::kGateTeleport) != 0);
    CHECK((menu.services & proto::kGateRecallPoint) == 0);
    // On his transport: the dock's link refuses it; the ferry takes them across for its fare, the transport along.
    a.client->Send(proto::UseItem{PlaceOf(a.bag, kCartScroll)});
    const auto cart = Next<proto::CosSummoned>(a);
    a.client->Send(proto::UseGate{npc, 3});
    CHECK(Next<proto::GateRefused>(a).status == proto::GateStatus::Transport);
    a.client->Send(proto::UseGate{npc, 2});
    CHECK(Next<proto::GoldChanged>(a).gold == 900);
    const auto there = Next<proto::WorldEntered>(a);
    CHECK(there.position.region == assets::RegionId(170, 98));
    bool cartThere = false;
    for (int i = 0; i < 300 && !cartThere; ++i) {
        proto::EntityAppeared seen;
        if (a.client->Expect(seen, 10ms) && seen.entity == cart.entity) cartThere = seen.position.region == assets::RegionId(170, 98);
    }
    CHECK(cartThere);
    // Its owner's client told again (it clears its own at WorldEntered), and the trader on it again.
    const auto told = Next<proto::CosSummoned>(a);
    CHECK(told.entity == cart.entity);
    CHECK(told.kind == std::uint8_t(game::CosKind::Transport));
    const auto riding = Next<proto::CosRiding>(a);
    CHECK(riding.mount == cart.entity);
    CHECK(riding.riding);
    s.game.Stop();
}

TEST_CASE("Game server: an attack pet's stance: defensive, it fights only on its owner's Attack; offensive, it defends its owner (game files; the user's report 2026-11-14)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const char* dirEnv = std::getenv("SR_GAME_DIR");
    if (dirEnv == nullptr || *dirEnv == 0) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirEnv, error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    auto items = std::make_shared<assets::ItemTable>();
    REQUIRE_MESSAGE(items->Load(fs, error), error);
    const assets::ItemRow* flute = items->Find(std::string_view("ITEM_COS_P_FLUTE"));
    REQUIRE(flute != nullptr);
    // A weak fighting monster in an aggressive nest beside the owner: it comes for him.
    const assets::ObjectRow* foe = nullptr;
    for (const auto& [code, r] : characters->rows()) {
        if (r.typeId[1] != 2 || r.typeId[2] != 1 || r.level < 1 || r.level > 8 || r.rarity != 0 || r.maxHp < 30) continue;
        if (!code.starts_with("MOB_") || code.find("_NPC") != std::string::npos || code.find("THIEF") != std::string::npos) continue;
        const assets::SkillRow* attack = r.skills[0] != 0 ? skills->Find(r.skills[0]) : nullptr;
        if (attack == nullptr || !attack->attack || attack->range > 100) continue;
        if (foe == nullptr || r.level < foe->level || (r.level == foe->level && r.maxHp < foe->maxHp)) foe = &r;
    }
    REQUIRE(foe != nullptr);
    CosServer s(t);
    auto rules = std::make_shared<game::CharacterRules>();
    for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
    rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
    rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
    rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
    rules->items = items;
    s.config.rules = rules;
    s.config.files = &fs;
    s.config.characters = characters;
    s.config.skills = skills;
    s.config.levels.assign(150, 1'000'000);
    s.config.monsterExperience.assign(150, 1);
    server::MonsterNest nest;
    nest.refObjId = foe->refObjId;
    nest.position = {assets::RegionId(76, 105), 640, 140, 900};
    nest.radius = 300;
    nest.delayMin = nest.delayMax = 1;
    nest.aggressive = true;
    nest.sight = 200;
    s.config.monsterNests = {nest};
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Keeper", 600, {PetStored(flute->refObjId)}, 20, assets::RegionId(76, 105), 140, 900);
    a.client->Send(proto::UseItem{PlaceOf(a.bag, flute->refObjId)});
    auto wolf = Next<proto::CosSummoned>(a);
    // A new pet is defensive (its row's PetOption 0, the original's default).
    REQUIRE_FALSE(wolf.aggressive);
    // The monster comes and strikes its owner or the wolf (the nearest fighter), again and again: the wolf never swings.
    // Watched until the monster has struck and a while after (not longer: a defensive wolf struck again and again dies).
    const auto watch = [&](int rounds) {
        bool ownerHit = false, wolfSwung = false;
        int after = 0;
        for (int i = 0; i < rounds && after < 100; ++i) {
            if (ownerHit) ++after;
            proto::EntitySwing swing;
            proto::EntityDamaged hit;
            if (a.client->Expect(swing, 10ms) && swing.attacker == wolf.entity) wolfSwung = true;
            while (a.client->Expect(hit, 5ms)) ownerHit = ownerHit || ((hit.target == a.entity || hit.target == wolf.entity) && hit.attacker != wolf.entity);
        }
        return std::pair{ownerHit, wolfSwung};
    };
    const auto [struck, swungDefensive] = watch(600);
    REQUIRE(struck);
    CHECK_FALSE(swungDefensive);
    // Offensive: kept in its row, as it was after an unsummon and a new summon; it defends its owner.
    a.client->Send(proto::CosCommand{wolf.entity, proto::CosOrder::Stance, 0});
    REQUIRE(Next<proto::CosStance>(a).aggressive);
    a.client->Send(proto::CosDismiss{wolf.entity});
    Next<proto::CosDismissed>(a);
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms); // its row saved
    a.client->Send(proto::UseItem{PlaceOf(a.bag, flute->refObjId)});
    wolf = Next<proto::CosSummoned>(a);
    CHECK(wolf.aggressive);
    const auto [again, swungOffensive] = watch(600);
    CHECK(swungOffensive);
    s.game.Stop();
}

TEST_CASE("Game server: a pet out at logout comes back at the next entry, its stance kept (the user's report 2026-11-14)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    CosServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Homer", 1000, {PetStored(kFlute)});
    a.client->Send(proto::UseItem{PlaceOf(a.bag, kFlute)});
    const auto wolf = Next<proto::CosSummoned>(a);
    a.client->Send(proto::CosCommand{wolf.entity, proto::CosOrder::Stance, 0});
    REQUIRE(Next<proto::CosStance>(a).aggressive);
    // Out of the world, and in again.
    a.client->Send(proto::WorldLeave{});
    Next<proto::WorldLeft>(a);
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms); // its row saved
    a.client->Send(proto::CharacterSelect{"Homer"});
    REQUIRE(Next<proto::LobbyResult>(a).status == proto::LobbyStatus::Ok);
    a.client->Send(proto::WorldEnter{});
    REQUIRE(Next<proto::WorldEntered>(a).status == proto::WorldStatus::Ok);
    const auto back = Next<proto::CosSummoned>(a);
    CHECK(back.model == wolf.model);
    CHECK(back.aggressive);
    s.game.Stop();
}
