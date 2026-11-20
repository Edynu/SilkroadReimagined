// The mall's items used on a pet: the watches (a grab pet's bag 28 places more and a
// skill, for as many minutes as the stack holds) and the pet skills (an attack pet's imbue for 30 minutes), as the
// original game server runs them; their jobs kept in _TimedJobForPet. The data's
// rules alone, then with the game files (SR_GAME_DIR) and a database (SR_DB_URL) the game server's uses.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/Cos.h"
#include "sr/game/CosProtocol.h"
#include "sr/game/MallProtocol.h"
#include "sr/game/Skills.h"
#include "sr/game/States.h"
#include "sr/server/GameServer.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;

namespace {

// The game's tables, read once for the cases that want them (none: SR_GAME_DIR not set).
struct GameTables {
    assets::FileSystem fs;
    std::shared_ptr<assets::ItemTable> items = std::make_shared<assets::ItemTable>();
    std::shared_ptr<assets::SkillTable> skills = std::make_shared<assets::SkillTable>();
    std::shared_ptr<assets::CharacterTable> characters = std::make_shared<assets::CharacterTable>();
};
GameTables* Tables() {
    static std::unique_ptr<GameTables> tables;
    static bool tried = false;
    if (tried) return tables.get();
    tried = true;
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr || *dir == 0) return nullptr;
    auto t = std::make_unique<GameTables>();
    std::string error;
    if (!assets::MountGameArchives(t->fs, dir, error) || !t->items->Load(t->fs, error) || !t->skills->Load(t->fs, error) ||
        !t->characters->Load(t->fs, error)) {
        MESSAGE("the game files: " << error);
        return nullptr;
    }
    tables = std::move(t);
    return tables.get();
}

// A 1.188 row as the item table has it: TID 3/3/13/15, Param1 the seconds, Param2's jobs, Param6 the pet's bits.
assets::ItemRow PetMallRow(const char* codename, int seconds, const char* jobs, int pets) {
    assets::ItemRow r;
    r.codename = codename;
    r.typeId = {3, 3, 13, 15};
    r.inService = true;
    r.params[0] = seconds;
    r.params[1] = 1;
    r.paramTexts[1] = jobs;
    r.params[5] = pets;
    return r;
}

struct PetServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    std::int64_t shift = 0; // the wall clock moved on (the pets' jobs end on it)
    // With nests the navigation too (they wake where it is in).
    PetServer(TestDatabase& db, const GameTables& g, std::vector<server::MonsterNest> nests = {}) : t(db) {
        if (!nests.empty()) config.files = &g.fs;
        config.listen = Local();
        config.databaseUrl = t.Url();
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
        rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
        rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
        rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
        rules->items = g.items;
        config.rules = rules;
        config.characters = g.characters;
        config.skills = g.skills;
        config.levels.assign(150, 10'000'000);
        config.monsterExperience.assign(150, 100);
        config.monsterNests = std::move(nests);
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        config.databaseThreads = 2;
        config.devCommands = true;
        config.wallClock = [this] { return std::int64_t(std::time(nullptr)) + shift; };
    }
    struct Player {
        std::unique_ptr<Client> client;
        std::int64_t character = 0;
        std::uint32_t entity = 0;
        proto::InventoryList bag;
    };
    // A character of `level` at (x, z) of a region with `items` in its bag, entered.
    Player Make(const std::string& name, const std::vector<server::StoredItem>& items, int level, std::uint16_t region, float x, float y, float z) {
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
        here.y = y;
        here.z = z;
        REQUIRE(server::SavePlaces(t.Db(), {{p.character, here}}));
        REQUIRE(server::SaveProgress(t.Db(), {{p.character, std::uint8_t(level), 0, 0, 0, 40, 40, 0}}));
        REQUIRE(server::SaveVitals(t.Db(), p.character, 100'000, 100'000));
        const std::int64_t jid = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {p.character}).Int(0, 0);
        REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {jid}).Ok());
        REQUIRE(server::GiveItems(t.Db(), config.id, name, items, 0, error) == server::GiveResult::Done);
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

server::StoredItem Stack(std::uint32_t refId, std::uint16_t count) {
    server::StoredItem i;
    i.refId = refId;
    i.quantity = count;
    i.counted = true;
    return i;
}
server::StoredItem PetItem(std::uint32_t refId) {
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
T Next(PetServer::Player& p) {
    T m;
    REQUIRE(p.client->Expect(m));
    return m;
}
// The answer to an item used on a pet's item.
proto::ServiceAnswer UseOn(PetServer::Player& p, std::uint8_t tool, std::uint8_t pet) {
    p.client->Send(proto::UseItemOn{tool, pet});
    for (;;) {
        const auto a = Next<proto::ServiceAnswer>(p);
        if (a.request == proto::ServiceRequest::UseOn) return a;
    }
}
// The pet's bag as its owner is told it next with that size (others before it passed over).
proto::CosBag BagOfSize(PetServer::Player& p, int size) {
    proto::CosBag bag;
    for (int i = 0; i < 50; ++i) {
        bag = Next<proto::CosBag>(p);
        if (bag.size == size) return bag;
    }
    FAIL("no bag of that size");
    return bag;
}
bool Holds(const proto::CosBag& bag, std::uint8_t place) {
    for (const proto::ItemRecord& r : bag.items) {
        if (r.slot == place) return true;
    }
    return false;
}
std::int64_t UnixNow() { return std::int64_t(std::time(nullptr)); }

// A field monster of the first levels that fights (a level-1 wolf's match; its nest fills again a second after a kill),
// the one of the most health.
const assets::ObjectRow* FieldMonster(const GameTables& g) {
    const assets::ObjectRow* foe = nullptr;
    for (const auto& [code, r] : g.characters->rows()) {
        if (r.typeId[1] != 2 || r.typeId[2] != 1 || r.level < 1 || r.level > 3 || r.rarity != 0 || r.maxHp < 50 || r.maxHp > 3000) continue;
        if (!code.starts_with("MOB_") || code.find("_NPC") != std::string::npos || code.find("THIEF") != std::string::npos ||
            code.find("EVENT") != std::string::npos || code.find("_EV_") != std::string::npos || code.find("SUMMON") != std::string::npos ||
            code.find("AUTOMOB") != std::string::npos) {
            continue;
        }
        const assets::SkillRow* attack = r.skills[0] != 0 ? g.skills->Find(r.skills[0]) : nullptr;
        if (attack == nullptr || !attack->attack || attack->range > 100) continue;
        if (foe == nullptr || r.level < foe->level || (r.level == foe->level && r.maxHp > foe->maxHp)) foe = &r;
    }
    return foe;
}

} // namespace

TEST_CASE("Pet items: the jobs of a watch and of a pet skill, the skill's level row, a watch's minutes") {
    const assets::ItemRow watch = PetMallRow("ITEM_MALL_PET_WATCH_CHULHYEN", 259200,
                                             "[PSU1:SKILL_MALL_PET_WATCH_CHULHYEN_01,0],[BCS1:INC_INVENTORY_SIZE,A,259200,28,0,0,0,0]", 2);
    const game::PetItemJobs w = game::PetItemJobsOf(watch);
    CHECK(bool(w));
    CHECK(w.skill == "SKILL_MALL_PET_WATCH_CHULHYEN_01");
    CHECK_FALSE(w.skillByLevel);
    CHECK(w.bagPlaces == 28);
    CHECK(w.grabPet);
    CHECK_FALSE(w.attackPet);
    CHECK(game::PetSkillCodename(w, 7) == "SKILL_MALL_PET_WATCH_CHULHYEN_01");
    const assets::ItemRow cold = PetMallRow("ITEM_MALL_PET_SKILL_COLD", 1800, "[PSU1:SKILL_MALL_PET_SKILL_COLD,1]", 1);
    const game::PetItemJobs c = game::PetItemJobsOf(cold);
    CHECK(c.skill == "SKILL_MALL_PET_SKILL_COLD");
    CHECK(c.skillByLevel);
    CHECK(c.bagPlaces == 0);
    CHECK(c.attackPet);
    CHECK_FALSE(c.grabPet);
    // "%s_%03d" with the pet's level.
    CHECK(game::PetSkillCodename(c, 7) == "SKILL_MALL_PET_SKILL_COLD_007");
    CHECK(game::PetSkillCodename(c, 110) == "SKILL_MALL_PET_SKILL_COLD_110");
    // Not a pet item: no jobs.
    assets::ItemRow other = cold;
    other.typeId = {3, 3, 13, 14};
    CHECK_FALSE(game::PetItemJobsOf(other));
    // A watch's minutes: the stack's count, at most the rental's whole minutes left.
    CHECK(game::PetWatchMinutes(4320, 28 * 86400) == 4320);
    CHECK(game::PetWatchMinutes(4320, 86400 + 59) == 1440);
    CHECK(game::PetWatchMinutes(10, 3 * 60) == 3);
    CHECK(game::PetWatchMinutes(10, 59) == 0);
}

TEST_CASE("Pet items: every 1.188 pet mall item's jobs name skills of the table, a watch's stack its minutes (game files)") {
    GameTables* g = Tables();
    if (g == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    int watches = 0, skills = 0, growth = 0;
    for (const auto& [code, row] : g->items->rows()) {
        if (!row.inService || !game::IsPetMallItem(row)) continue;
        const game::PetItemJobs jobs = game::PetItemJobsOf(row);
        INFO(code);
        REQUIRE(bool(jobs));
        CHECK(jobs.attackPet != jobs.grabPet); // one kind of pet each (Param6 1 or 2)
        if (jobs.grabPet) {
            ++watches;
            // The watches: a skill of their own and 28 places; their stack is their minutes (MaxStack = Param1 / 60).
            CHECK(jobs.bagPlaces == 28);
            CHECK(row.maxStack == row.params[0] / 60);
            const assets::SkillRow* s = g->skills->Find(std::string_view(jobs.skill));
            REQUIRE(s != nullptr);
            CHECK_FALSE(game::IsImbue(*s));
        } else if (jobs.skillByLevel) {
            ++skills;
            // The pet skills: an imbue of each level 1..110, 30 minutes, with its states.
            CHECK(jobs.bagPlaces == 0);
            for (int level = 1; level <= game::kAttackPetGrowthLimit; ++level) {
                const assets::SkillRow* s = g->skills->Find(std::string_view(game::PetSkillCodename(jobs, level)));
                REQUIRE_MESSAGE(s != nullptr, game::PetSkillCodename(jobs, level));
                CHECK(game::IsImbue(*s));
                CHECK(s->buffDurationMs == std::uint32_t(row.params[0]) * 1000u);
                CHECK_FALSE(game::StateGiftsOf(*s).empty());
            }
        } else {
            ++growth; // the potions of growth (their own route)
            CHECK(g->skills->Find(std::string_view(jobs.skill)) != nullptr);
        }
    }
    CHECK(watches == 4);
    CHECK(skills == 3);
    CHECK(growth >= 2);
}

TEST_CASE("Game server: the pets' watches: a grab pet's bag 28 places more and its skill for the stack's minutes; one at a time; the "
          "codes; kept over an unsummon and a relog; over at its end, what lay past its places kept (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    GameTables* g = Tables();
    if (g == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    const assets::ItemRow* rabbit = g->items->Find(std::string_view("ITEM_COS_P_RABBIT_SCROLL_SILK"));
    const assets::ItemRow* chulhyen = g->items->Find(std::string_view("ITEM_MALL_PET_WATCH_CHULHYEN"));
    const assets::ItemRow* agol = g->items->Find(std::string_view("ITEM_MALL_PET_WATCH_AGOL"));
    const assets::ItemRow* cold = g->items->Find(std::string_view("ITEM_MALL_PET_SKILL_COLD"));
    const assets::ItemRow* potion = g->items->Find(std::string_view("ITEM_ETC_HP_POTION_01"));
    const assets::SkillRow* watchSkill = g->skills->Find(std::string_view("SKILL_MALL_PET_WATCH_CHULHYEN_01"));
    REQUIRE(rabbit != nullptr);
    REQUIRE(chulhyen != nullptr);
    REQUIRE(agol != nullptr);
    REQUIRE(cold != nullptr);
    REQUIRE(potion != nullptr);
    REQUIRE(watchSkill != nullptr);
    PetServer s(t, *g);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Keeper", {PetItem(rabbit->refObjId), Stack(chulhyen->refObjId, 4320), Stack(agol->refObjId, 10), Stack(cold->refObjId, 1),
                               Stack(potion->refObjId, 5)},
                    10, assets::RegionId(168, 97), 1000, 0, 1000);
    const std::uint8_t petPlace = PlaceOf(a.bag, rabbit->refObjId), threeDays = PlaceOf(a.bag, chulhyen->refObjId),
                       tenMinutes = PlaceOf(a.bag, agol->refObjId), coldPlace = PlaceOf(a.bag, cold->refObjId),
                       potionPlace = PlaceOf(a.bag, potion->refObjId);
    const auto eventually = [&](const std::function<bool()>& holds) {
        for (int i = 0; i < 500 && !holds(); ++i) s.game.Poll(10ms);
        return holds();
    };
    const auto petRow = [&] {
        return t.Db().Query("SELECT it.data FROM _inventory i JOIN _items it ON it.id64 = i.itemid WHERE i.charid = $1 AND i.slot = $2", {a.character, int(petPlace)})
            .Int(0, 0);
    };
    const auto jobs = [&](std::int64_t cos) { return t.Db().Query("SELECT count(*) FROM _timedjobforpet WHERE charid = $1", {cos}).Int(0, 0); };
    const auto summon = [&] {
        a.client->Send(proto::UseItem{petPlace});
        const auto pet = Next<proto::CosSummoned>(a);
        REQUIRE(pet.kind == std::uint8_t(game::CosKind::GrabPet));
        return pet;
    };
    const auto dismiss = [&](std::uint32_t entity) {
        a.client->Send(proto::CosDismiss{entity});
        REQUIRE(Next<proto::CosDismissed>(a).entity == entity);
        for (int i = 0; i < 50; ++i) s.game.Poll(2ms); // its row saved
    };

    // Its pet not out: 0x18B8 (the item's pet); a place that is no bag place: 3.
    CHECK(UseOn(a, threeDays, petPlace).code == 0x18B8);
    CHECK(UseOn(a, threeDays, 5).code == 3);
    // Out: its own 28 places.
    auto pet = summon();
    CHECK(BagOfSize(a, game::kGrabPetBag).items.empty());
    const std::int64_t cos = petRow();
    REQUIRE(cos != 0);
    // A pet skill is an attack pet's: 3 (Param6 bit 1 on a grab pet).
    CHECK(UseOn(a, coldPlace, petPlace).code == 3);
    // Two minutes or less of its rental left: 2.
    dismiss(pet.entity);
    const auto rentIn = [&](std::int64_t seconds) {
        REQUIRE(t.Db().Query("UPDATE _charcos SET rentendtime = to_timestamp($2::bigint) AT TIME ZONE current_setting('TimeZone') WHERE id = $1",
                             {cos, UnixNow() + seconds})
                    .Ok());
    };
    rentIn(100);
    pet = summon();
    CHECK(UseOn(a, threeDays, petPlace).code == 2);
    dismiss(pet.entity);
    rentIn(28 * 86400);
    pet = summon();
    BagOfSize(a, game::kGrabPetBag);

    // The three days' watch (4,320 a stack: its minutes, all taken, its rental longer than them): its bag 56, its skill on
    // it, its two jobs written (category 0 the skill, its end, Data2 1; category 5 the item, 'BCS1', INC_INVENTORY_SIZE, 28).
    const std::int64_t before = UnixNow();
    const auto used = UseOn(a, threeDays, petPlace);
    REQUIRE(used.code == 0);
    CHECK(used.a == chulhyen->refObjId);
    CHECK(used.b == 4320);
    CHECK(BagOfSize(a, 2 * game::kGrabPetBag).size == 56);
    proto::ItemMoved gone;
    for (int i = 0; i < 20 && !(gone.from == threeDays && !gone.emptied.empty()); ++i) gone = Next<proto::ItemMoved>(a);
    CHECK(gone.emptied == std::vector<std::uint8_t>{threeDays});
    proto::EntityBuffs buffs;
    for (int i = 0; i < 20 && buffs.entity != pet.entity; ++i) buffs = Next<proto::EntityBuffs>(a);
    CHECK(buffs.skills == std::vector<std::uint32_t>{watchSkill->id});
    REQUIRE(eventually([&] { return jobs(cos) == 2; }));
    const auto skillJob = t.Db().Query("SELECT timetokeep, data2 FROM _timedjobforpet WHERE charid = $1 AND category = 0 AND jobid = $2",
                                       {cos, std::int64_t(watchSkill->id)});
    REQUIRE(skillJob.Rows() == 1);
    CHECK(skillJob.Int(0, 0) >= before + 4320 * 60);
    CHECK(skillJob.Int(0, 0) <= UnixNow() + 4320 * 60);
    CHECK(skillJob.Int(0, 1) == 1);
    const auto bagJob = t.Db().Query("SELECT data1, data2, data3 FROM _timedjobforpet WHERE charid = $1 AND category = 5 AND jobid = $2",
                                     {cos, std::int64_t(chulhyen->refObjId)});
    REQUIRE(bagJob.Rows() == 1);
    CHECK(bagJob.Int(0, 0) == game::kPetJobBagTag);
    CHECK(bagJob.Int(0, 1) == game::kPetJobBagKind);
    CHECK(bagJob.Int(0, 2) == 28);
    // Another watch while one runs: 0x1894 (the bag's job).
    CHECK(UseOn(a, tenMinutes, petPlace).code == 0x1894);

    // A potion in one of the new places (30): kept there.
    a.client->Send(proto::CosBagMove{pet.entity, potionPlace, 30, false, true});
    CHECK(Holds(BagOfSize(a, 56), 30));
    // Back into its item and out again: its jobs read with it (as the original reads them).
    dismiss(pet.entity);
    pet = summon();
    CHECK(Holds(BagOfSize(a, 56), 30));
    // Out of the world with it out, and in again: back with its bag.
    a.client->Send(proto::WorldLeave{});
    Next<proto::WorldLeft>(a);
    for (int i = 0; i < 100; ++i) s.game.Poll(2ms);
    a.client->Send(proto::CharacterSelect{"Keeper"});
    REQUIRE(Next<proto::LobbyResult>(a).status == proto::LobbyStatus::Ok);
    a.client->Send(proto::WorldEnter{});
    REQUIRE(Next<proto::WorldEntered>(a).status == proto::WorldStatus::Ok);
    pet = Next<proto::CosSummoned>(a);
    CHECK(Holds(BagOfSize(a, 56), 30));

    // Its end on the wall clock (three days on): its own 28 places again, the potion past them kept but out of reach;
    // its jobs gone.
    s.shift = 4320 * 60 + 2;
    const auto shrunk = BagOfSize(a, game::kGrabPetBag);
    CHECK_FALSE(Holds(shrunk, 30));
    CHECK(eventually([&] { return jobs(cos) == 0; }));
    CHECK(t.Db().Query("SELECT count(*) FROM _invcos WHERE cosid = $1 AND slot = 30 AND itemid <> 0", {cos}).Int(0, 0) == 1);
    // The ten minutes' watch now: its ten taken (the stack emptied), the potion in reach again.
    const auto again = UseOn(a, tenMinutes, petPlace);
    CHECK(again.code == 0);
    CHECK(again.b == 10);
    CHECK(Holds(BagOfSize(a, 56), 30));
    s.game.Stop();
}

TEST_CASE("Game server: the pet skills: an attack pet's imbue of its level for 30 minutes, its states on the pet's hits; one element at a "
          "time; the codes; over at its end (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    GameTables* g = Tables();
    if (g == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    const assets::ItemRow* flute = g->items->Find(std::string_view("ITEM_COS_P_FLUTE"));
    const assets::ItemRow* cold = g->items->Find(std::string_view("ITEM_MALL_PET_SKILL_COLD"));
    const assets::ItemRow* fire = g->items->Find(std::string_view("ITEM_MALL_PET_SKILL_FIRE"));
    const assets::ItemRow* watch = g->items->Find(std::string_view("ITEM_MALL_PET_WATCH_AGOL"));
    const assets::SkillRow* cold1 = g->skills->Find(std::string_view("SKILL_MALL_PET_SKILL_COLD_001"));
    const assets::SkillRow* fire1 = g->skills->Find(std::string_view("SKILL_MALL_PET_SKILL_FIRE_001"));
    REQUIRE(flute != nullptr);
    REQUIRE(cold != nullptr);
    REQUIRE(fire != nullptr);
    REQUIRE(watch != nullptr);
    REQUIRE(cold1 != nullptr);
    REQUIRE(fire1 != nullptr);
    const assets::ObjectRow* foe = FieldMonster(*g);
    REQUIRE(foe != nullptr);
    MESSAGE("the monster: " << foe->codename << ", level " << foe->level << ", " << foe->maxHp << " HP");
    server::MonsterNest nest;
    nest.refObjId = foe->refObjId;
    nest.position = {assets::RegionId(76, 105), 640, 140, 900};
    nest.radius = 300;
    nest.delayMin = nest.delayMax = 1;
    PetServer s(t, *g, {nest});
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Tamer", {PetItem(flute->refObjId), Stack(cold->refObjId, 2), Stack(fire->refObjId, 1), Stack(watch->refObjId, 10)}, 20,
                    assets::RegionId(76, 105), 600, 140, 900);
    const std::uint8_t petPlace = PlaceOf(a.bag, flute->refObjId), coldPlace = PlaceOf(a.bag, cold->refObjId),
                       firePlace = PlaceOf(a.bag, fire->refObjId), watchPlace = PlaceOf(a.bag, watch->refObjId);
    std::uint32_t monster = 0;
    for (int i = 0; i < 60 && monster == 0; ++i) {
        proto::EntityAppeared seen;
        if (a.client->Expect(seen, 1s) && seen.kind == proto::EntityKind::Monster) monster = seen.entity;
    }
    REQUIRE(monster != 0);
    a.client->Send(proto::UseItem{petPlace});
    const auto wolf = Next<proto::CosSummoned>(a);
    REQUIRE(wolf.kind == std::uint8_t(game::CosKind::AttackPet));
    const std::int64_t cos =
        t.Db().Query("SELECT it.data FROM _inventory i JOIN _items it ON it.id64 = i.itemid WHERE i.charid = $1 AND i.slot = $2", {a.character, int(petPlace)})
            .Int(0, 0);
    REQUIRE(cos != 0);
    const auto buffsOf = [&](std::uint32_t entity) {
        proto::EntityBuffs b;
        for (int i = 0; i < 40; ++i) {
            b = Next<proto::EntityBuffs>(a);
            if (b.entity == entity) return b;
        }
        FAIL("no buffs of it");
        return b;
    };

    // A watch is a grab pet's: 3.
    CHECK(UseOn(a, watchPlace, petPlace).code == 3);
    // The cold: its level's row (level 1: _001) on the wolf for its 30 minutes, one taken.
    const std::int64_t before = UnixNow();
    const auto used = UseOn(a, coldPlace, petPlace);
    REQUIRE(used.code == 0);
    CHECK(used.b == 1);
    CHECK(buffsOf(wolf.entity).skills == std::vector<std::uint32_t>{cold1->id});
    proto::ItemMoved taken;
    for (int i = 0; i < 20 && taken.from != coldPlace; ++i) taken = Next<proto::ItemMoved>(a);
    REQUIRE_FALSE(taken.changed.empty());
    CHECK(taken.changed[0].quantity == 1);
    for (int i = 0; i < 300 && t.Db().Query("SELECT count(*) FROM _timedjobforpet WHERE charid = $1", {cos}).Int(0, 0) == 0; ++i) s.game.Poll(10ms);
    const auto job = t.Db().Query("SELECT timetokeep FROM _timedjobforpet WHERE charid = $1 AND category = 0 AND jobid = $2", {cos, std::int64_t(cold1->id)});
    REQUIRE(job.Rows() == 1);
    CHECK(job.Int(0, 0) >= before + 1800);
    CHECK(job.Int(0, 0) <= UnixNow() + 1800);
    // The same again: 0x1894 (its skill's job running); the fire while the cold runs: 2 (the pets' imbues
    // share overlap group 1: the cast's buff check).
    CHECK(UseOn(a, coldPlace, petPlace).code == 0x1894);
    CHECK(UseOn(a, firePlace, petPlace).code == 2);

    // Its hits carry the imbue, and its states take on the monsters (frostbite at 25 %, freeze at 5 %: the pet's chance
    // rolls bring one within four rolls); each kill, on to the next monster of the nest.
    std::uint32_t current = monster;
    a.client->Send(proto::CosCommand{wolf.entity, proto::CosOrder::Attack, current});
    bool imbued = false, cooled = false;
    int kills = 0;
    for (int i = 0; i < 1500 && !cooled; ++i) {
        proto::EntityDamaged hit;
        while (a.client->Expect(hit, 10ms)) {
            if (hit.attacker != wolf.entity) continue;
            if (hit.imbue == cold1->id) imbued = true;
            if ((hit.flags & proto::kDamageKilled) != 0 && hit.target == current) {
                current = 0;
                ++kills;
            }
        }
        proto::EntityAppeared seen;
        while (current == 0 && a.client->Expect(seen, 10ms)) {
            if (seen.kind != proto::EntityKind::Monster) continue;
            current = seen.entity;
            a.client->Send(proto::CosCommand{wolf.entity, proto::CosOrder::Attack, current});
        }
        proto::EntityStates states;
        while (a.client->Expect(states, 10ms)) {
            if (states.entity == wolf.entity || states.entity == a.entity) continue;
            for (const proto::StateRecord& r : states.states) {
                if (r.kind == std::uint8_t(game::StateKind::Frostbite) || r.kind == std::uint8_t(game::StateKind::Freeze)) cooled = true;
            }
        }
        proto::CosDismissed dead;
        REQUIRE_FALSE(a.client->Expect(dead, 1ms)); // the wolf stands
    }
    MESSAGE("monsters killed before a state took: " << kills);
    CHECK(imbued);
    CHECK(cooled);

    // Its end (30 minutes on the wall clock): off the wolf, its job gone; the fire now takes.
    s.shift = 1800 + 2;
    proto::EntityBuffs off;
    for (int i = 0; i < 100; ++i) {
        off = buffsOf(wolf.entity);
        if (off.skills.empty()) break;
    }
    CHECK(off.skills.empty());
    for (int i = 0; i < 300 && t.Db().Query("SELECT count(*) FROM _timedjobforpet WHERE charid = $1", {cos}).Int(0, 0) != 0; ++i) s.game.Poll(10ms);
    CHECK(t.Db().Query("SELECT count(*) FROM _timedjobforpet WHERE charid = $1", {cos}).Int(0, 0) == 0);
    CHECK(UseOn(a, firePlace, petPlace).code == 0);
    CHECK(buffsOf(wolf.entity).skills == std::vector<std::uint32_t>{fire1->id});
    s.game.Stop();
}

TEST_CASE("Game server: the pet skill of lightning: the wolf's hit jumps to the nearest other monster within its range "
          "(game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    GameTables* g = Tables();
    if (g == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    const assets::ItemRow* flute = g->items->Find(std::string_view("ITEM_COS_P_FLUTE"));
    const assets::ItemRow* lightning = g->items->Find(std::string_view("ITEM_MALL_PET_SKILL_LIGHTNING"));
    const assets::SkillRow* lightning1 = g->skills->Find(std::string_view("SKILL_MALL_PET_SKILL_LIGHTNING_001"));
    const assets::ObjectRow* foe = FieldMonster(*g);
    REQUIRE(flute != nullptr);
    REQUIRE(lightning != nullptr);
    REQUIRE(lightning1 != nullptr);
    REQUIRE(foe != nullptr);
    // Three of them close together (within the jump's 35 of each other).
    server::MonsterNest nest;
    nest.refObjId = foe->refObjId;
    nest.position = {assets::RegionId(76, 105), 640, 140, 900};
    nest.radius = 300;
    nest.spawnRadius = 8;
    nest.maxCount = 3;
    nest.delayMin = nest.delayMax = 1;
    PetServer s(t, *g, {nest});
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Thunder", {PetItem(flute->refObjId), Stack(lightning->refObjId, 1)}, 20, assets::RegionId(76, 105), 600, 140, 900);
    const std::uint8_t petPlace = PlaceOf(a.bag, flute->refObjId), lightningPlace = PlaceOf(a.bag, lightning->refObjId);
    std::vector<std::uint32_t> seen;
    for (int i = 0; i < 60 && seen.size() < 3; ++i) {
        proto::EntityAppeared m;
        if (a.client->Expect(m, 1s) && m.kind == proto::EntityKind::Monster) seen.push_back(m.entity);
    }
    REQUIRE(seen.size() >= 2);
    a.client->Send(proto::UseItem{petPlace});
    const auto wolf = Next<proto::CosSummoned>(a);
    REQUIRE(UseOn(a, lightningPlace, petPlace).code == 0);
    std::set<std::uint32_t> alive(seen.begin(), seen.end());
    std::uint32_t current = 0;
    bool jumped = false;
    for (int i = 0; i < 1500 && !jumped; ++i) {
        if (current == 0 && !alive.empty()) {
            current = *alive.begin();
            a.client->Send(proto::CosCommand{wolf.entity, proto::CosOrder::Attack, current});
        }
        proto::EntityDamaged hit;
        while (a.client->Expect(hit, 10ms)) {
            if ((hit.flags & proto::kDamageKilled) != 0) {
                alive.erase(hit.target);
                if (hit.target == current) current = 0;
            }
            if (hit.attacker != wolf.entity || hit.imbue != lightning1->id) continue;
            if (hit.target != current && alive.contains(hit.target)) jumped = true; // the jump's hit, on another one
        }
        proto::EntityAppeared m;
        while (a.client->Expect(m, 10ms)) {
            if (m.kind == proto::EntityKind::Monster) alive.insert(m.entity);
        }
    }
    CHECK(jumped);
    s.game.Stop();
}
