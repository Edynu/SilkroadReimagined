// The Devil's Spirit, 1:1 with the 1.188 game server: its by-plus tables over the 1.188 data,
// what a spirit gives at a plus (options, skills, its own blues; asleep or without a row nothing), the most HP and MP
// with HPNA / MPNA (the keeper's 0x34 / 0x35), the block with brna, the damage on a unique with umdu (0x103), the skills'
// 'reqi 14 1', Sabakun's jewel and the UMDU stone in our alchemy, and on a game server: an awake spirit worn at +3 (its
// numbers on DevSheet, its skills in the book, one cast), taken off, and asleep while worn. The game files' cases need
// SR_GAME_DIR, the server's SR_DB_URL as well; otherwise skipped.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/core/Text.h"
#include "sr/game/Alchemy.h"
#include "sr/game/Combat.h"
#include "sr/game/Items.h"
#include "sr/game/MallProtocol.h"
#include "sr/game/Passives.h"
#include "sr/game/Spirit.h"
#include "sr/game/Stats.h"
#include "sr/server/GameServer.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;

namespace {

constexpr std::uint32_t kHpna = 0x68706E61, kMpna = 0x6D706E61, kBrna = 0x62726E61, kBri = 0x627269, kUmdu = 0x756D6475;

std::uint32_t Code(const char* s) {
    std::uint32_t v = 0;
    for (; *s != 0; ++s) v = (v << 8) | std::uint8_t(*s);
    return v;
}

// The 1.188 options the spirit's rows name (magicoption.txt's ids and codes; tables of data).
std::vector<assets::MagicOptionRow> SpiritOptions() {
    const auto option = [](std::uint32_t id, const char* name, std::uint32_t code) {
        assets::MagicOptionRow o;
        o.paramId = id;
        o.optionName = name;
        o.attribute = code;
        o.degree = 1;
        return o;
    };
    return {option(353, "MATTR_NASRUN_UMDU", kUmdu), option(354, "MATTR_NASRUN_HPNA", kHpna), option(355, "MATTR_NASRUN_MPNA", kMpna),
            option(356, "MATTR_NASRUN_BLOCKRATE", kBrna)};
}

// ITEM_MALL_AVATAR_M_NASRUN's rows +0, +3 and +6 as the 1.188 tables have them (links 1, 4, 7).
game::SpiritTable Table() {
    const std::vector<game::SpiritAbilityRow> abilities = {{1, 24405, 0}, {4, 24405, 3}, {7, 24405, 6}};
    const std::vector<game::SpiritOptionRow> options = {{1, 354, 15, 0}, {1, 355, 15, 0}, {4, 354, 18, 0}, {4, 355, 18, 0}, {4, 356, 1, 1},
                                                        {7, 354, 21, 0}, {7, 355, 21, 0}, {7, 356, 2, 1}};
    const std::vector<game::SpiritSkillRow> skills = {{1, 31142}, {4, 31143}, {4, 31151}, {7, 31144}, {7, 31152}};
    return game::MakeSpiritTable(abilities, options, skills, SpiritOptions());
}

assets::ItemRow SpiritRow(std::uint32_t id = 24405) {
    assets::ItemRow r;
    r.refObjId = id;
    r.codename = "ITEM_MALL_AVATAR_M_NASRUN";
    r.typeId = {3, 1, 14, 1};
    r.inService = true;
    r.params[0] = 2419200;
    r.params[1] = 1;
    return r;
}

std::int64_t Now() { return std::int64_t(std::time(nullptr)); }

} // namespace

TEST_CASE("Devil's Spirit: what it gives at a plus, awake; asleep or without a row nothing") {
    const game::SpiritTable t = Table();
    const std::vector<assets::MagicOptionRow> options = SpiritOptions();
    const assets::ItemRow row = SpiritRow();
    game::ItemInstance item;
    item.refId = 24405;
    item.plus = 3;
    item.durability = std::uint32_t(Now() + 3600);

    const game::SpiritGift g = game::SpiritGiftOf(t, row, item, &options, Now());
    CHECK(g.awake);
    CHECK(g.found);
    REQUIRE(g.changes.size() == 3);
    CHECK(g.changes[0].stat == game::stat::kHpFromStr);
    CHECK(g.changes[0].kind == 1);
    CHECK(g.changes[0].value == 18.0f);
    CHECK(g.changes[1].stat == game::stat::kMpFromInt);
    CHECK(g.changes[1].kind == 1);
    CHECK(g.changes[1].value == 18.0f);
    CHECK(g.changes[2].stat == game::stat::kBlock);
    CHECK(g.changes[2].kind == 0);
    CHECK(g.changes[2].value == 1.0f);
    CHECK(g.skills == std::vector<std::uint32_t>{31143, 31151});

    // +0: the HP and MP alone, Eccentric Demon; +6: brna 2, Destroy the Devil and the second defence ignore.
    item.plus = 0;
    const game::SpiritGift zero = game::SpiritGiftOf(t, row, item, &options, Now());
    CHECK(zero.changes.size() == 2);
    CHECK(zero.skills == std::vector<std::uint32_t>{31142});
    item.plus = 6;
    CHECK(game::SpiritGiftOf(t, row, item, &options, Now()).skills == std::vector<std::uint32_t>{31144, 31152});

    // Its own blue, an UMDU stone's: the damage on a unique, flat, after the plus's options.
    item.plus = 3;
    item.options = {{353, 6}};
    const game::SpiritGift stone = game::SpiritGiftOf(t, row, item, &options, Now());
    REQUIRE(stone.changes.size() == 4);
    CHECK(stone.changes[3].stat == game::stat::kUniqueDamage);
    CHECK(stone.changes[3].kind == 0);
    CHECK(stone.changes[3].value == 6.0f);

    // No row for its plus (+11, or an event spirit the tables lack): nothing at all, not even its own blue.
    item.plus = 11;
    const game::SpiritGift none = game::SpiritGiftOf(t, row, item, &options, Now());
    CHECK(none.awake);
    CHECK_FALSE(none.found);
    CHECK(none.changes.empty());
    CHECK(none.skills.empty());

    // Asleep (its awakening over, or never awakened): nothing; not a spirit: nothing.
    item.plus = 3;
    item.durability = std::uint32_t(Now() - 1);
    const game::SpiritGift asleep = game::SpiritGiftOf(t, row, item, &options, Now());
    CHECK_FALSE(asleep.awake);
    CHECK(asleep.changes.empty());
    CHECK(asleep.skills.empty());
    item.durability = 0;
    CHECK_FALSE(game::SpiritGiftOf(t, row, item, &options, Now()).awake);
    assets::ItemRow hat = row;
    hat.typeId = {3, 1, 13, 1};
    item.durability = std::uint32_t(Now() + 3600);
    CHECK(game::SpiritGiftOf(t, hat, item, &options, Now()).changes.empty());
}

TEST_CASE("Devil's Spirit: HPNA / MPNA raise the HP from STR and the MP from INT before the level's factor") {
    game::StatSheet sheet;
    sheet.Add(0x17E00104, game::BlueChanges(kHpna, 20));
    sheet.Add(0x17E00104, game::BlueChanges(kMpna, 25));
    const game::CombatStats plain = game::PlayerCombatStats(50, 100, 80, {}, nullptr);
    const game::CombatStats worn = game::PlayerCombatStats(50, 100, 80, {}, nullptr, sheet);
    const double factor = std::pow(1.02, 49.0);
    CHECK(plain.maxHp == std::uint32_t(std::trunc(factor * 100.0 * 10.0)));
    CHECK(worn.maxHp == std::uint32_t(std::trunc(factor * 120.0 * 10.0)));
    CHECK(worn.maxMp == std::uint32_t(std::trunc(factor * 100.0 * 10.0)));
    CHECK(worn.maxHp == game::PlayerVitalFrom(50, 120.0));
    // STR and INT themselves unchanged; a flat HP (a blue's) on top, a level 1 character's 20 STR at 18 %: 236.
    CHECK(worn.strength == doctest::Approx(100.0));
    sheet.Add(0x17E00009, game::BlueChanges(Code("hp"), 300));
    CHECK(game::PlayerCombatStats(50, 100, 80, {}, nullptr, sheet).maxHp == worn.maxHp + 300);
    game::StatSheet low;
    low.Add(1, game::BlueChanges(kHpna, 18));
    CHECK(game::PlayerCombatStats(1, 20, 20, {}, nullptr, low).maxHp == 236);
    CHECK(game::PlayerCombatStats(1, 20, 20, {}, nullptr, low).maxMp == 200);
    // No HPNA: as before (PlayerMaxHp).
    CHECK(game::PlayerMaxHp(50, 100) == plain.maxHp);
}

TEST_CASE("Devil's Spirit: brna and bri put trunc(the item's own block) + v in its white entry's place") {
    // The spirit's own block is 0: brna is its value.
    const auto brna = game::BlueChanges(kBrna, 2);
    REQUIRE(brna.size() == 1);
    CHECK(brna[0].stat == game::stat::kBlock);
    CHECK(brna[0].kind == 0);
    CHECK(brna[0].value == 2.0f);
    game::StatSheet sheet;
    sheet.Add(0x17E00104, brna);
    CHECK(game::PlayerCombatStats(30, 50, 50, {}, nullptr, sheet).block == doctest::Approx(2.0));
    // A shield's white block 13.7 kept in our base: bri 3 makes it trunc(13.7) + 3 = 16 in all.
    const auto bri = game::BlueChanges(kBri, 3, 13.7f);
    REQUIRE(bri.size() == 1);
    game::CombatStats s;
    s.block = 13.7;
    game::StatSheet shield;
    shield.Add(0x17E00007, bri);
    game::ApplySheet(s, shield);
    CHECK(s.block == doctest::Approx(16.0).epsilon(1e-5));
}

TEST_CASE("Devil's Spirit: umdu adds its percent of the damage on a unique monster, before the lane's factor") {
    game::StatSheet sheet;
    sheet.Add(0x17E00104, game::BlueChanges(kUmdu, 10));
    const game::CombatStats attacker = game::PlayerCombatStats(80, 300, 50, {}, nullptr, sheet);
    CHECK(attacker.classBonus == doctest::Approx(10.0));
    assets::ObjectRow mob;
    mob.level = 80;
    mob.maxHp = 1000000;
    mob.phyDefence = 100;
    mob.evasion = 10;
    mob.hitRate = 10;
    const game::CombatStats unique = game::MonsterCombatStats(mob, 3);
    const game::CombatStats general = game::MonsterCombatStats(mob, 0);
    CHECK(unique.targetClass == 3);
    CHECK(general.targetClass == 0);
    CHECK(game::MonsterCombatStats(mob, 0x13).targetClass == 3);
    game::CombatStats strong = attacker;
    strong.phyAttackMin = strong.phyAttackMax = 5000;
    game::AttackSpec a;
    a.formula = true;
    a.kind = 5;
    a.power = 100;
    game::HitRoll who;
    who.attackerPlayer = true;
    // The same rolls on both: the unique's 10 % more (cut to whole numbers each).
    std::mt19937 r1(7), r2(7);
    const game::HitResult onGeneral = game::RollHit(strong, general, a, who, r1);
    const game::HitResult onUnique = game::RollHit(strong, unique, a, who, r2);
    REQUIRE_FALSE(onGeneral.blocked);
    REQUIRE_FALSE(onUnique.blocked);
    CHECK(double(onUnique.damage) == doctest::Approx(double(onGeneral.damage) * 1.1).epsilon(0.002));
    // Without umdu: the same.
    game::CombatStats plain = strong;
    plain.classBonus = 0;
    std::mt19937 r3(7), r4(7);
    CHECK(game::RollHit(plain, unique, a, who, r3).damage == game::RollHit(plain, general, a, who, r4).damage);
}

TEST_CASE("Devil's Spirit: the 1.188 tables, its skills, its jewel and its stone (game files)") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE(assets::MountGameArchives(fs, dir, error));
    std::vector<assets::MagicOptionRow> options;
    const auto file = fs.Read("server_dep\\silkroad\\textdata\\magicoption.txt");
    REQUIRE(file);
    REQUIRE(assets::DecodeMagicOptions(core::DecodeTextFile(file->Bytes()), options, error));
    game::SpiritTable t;
    REQUIRE_MESSAGE(game::LoadSpiritTable(fs, options, t, error), error);
    CHECK(t.Size() == 90);
    // The ten spirits with rows: the mall's eight at +0..+10, the old event pair at +0 alone.
    std::map<std::uint32_t, int> levels;
    for (const auto& [key, level] : t.Levels()) ++levels[key.first];
    CHECK(levels.size() == 10);
    for (std::uint32_t id = 24405; id <= 24412; ++id) CHECK(levels[id] == 11);
    CHECK(levels[24413] == 1);
    CHECK(levels[24414] == 1);
    // The plain ones: HP / MP 15 + plus, the block 1 from +3, 2 from +6; the unique ones 20 + plus, 1 / 2 / 3.
    for (std::uint32_t id = 24405; id <= 24412; ++id) {
        const bool unique = id >= 24411;
        for (int plus = 0; plus <= 10; ++plus) {
            const game::SpiritLevel* l = t.Find(id, plus);
            REQUIRE(l != nullptr);
            std::map<std::uint32_t, std::uint32_t> byCode;
            for (const auto& o : l->options) byCode[o.code] = o.value;
            CHECK(byCode[kHpna] == std::uint32_t((unique ? 20 : 15) + plus));
            CHECK(byCode[kMpna] == std::uint32_t((unique ? 20 : 15) + plus));
            const std::uint32_t block = unique ? (plus < 3 ? 1 : plus < 6 ? 2 : 3) : (plus < 3 ? 0 : plus < 6 ? 1 : 2);
            CHECK(byCode[kBrna] == block);
            CHECK(l->skills.size() == (plus < 3 ? 1u : 2u));
        }
    }
    CHECK(t.Find(24405, 11) == nullptr);
    CHECK(t.Find(24413, 1) == nullptr);
    CHECK(t.Find(24971, 0) == nullptr); // the later event spirits (P2 1) have none: worn, they give nothing
    const game::SpiritLevel* m3 = t.Find(24405, 3);
    REQUIRE(m3 != nullptr);
    CHECK(m3->skills == std::vector<std::uint32_t>{31143, 31151});
    CHECK(t.Find(24409, 0)->skills == std::vector<std::uint32_t>{31148}); // the blue one's own Eccentric Demon
    CHECK(t.Find(24413, 0)->skills == std::vector<std::uint32_t>{31141});

    // The skills: buffs of 10 minutes (dru, hst3), reuse 30 minutes; the defence ignore 5 minutes (msid 1 / 2), reuse
    // 20; each asks 'reqi 14 1', held by an awake spirit in avatar place 4 alone.
    assets::SkillTable skills;
    REQUIRE(skills.Load(fs, error));
    const auto tag = [](const assets::SkillRow& s, const char* code) { return assets::FindSkillParam(s, assets::SkillTag(code)); };
    game::WornKinds bare, spirit;
    spirit.spirit = true;
    for (const std::uint32_t id : {31141u, 31142u, 31143u, 31144u, 31145u, 31146u, 31147u, 31148u, 31149u, 31150u}) {
        const assets::SkillRow* s = skills.Find(id);
        REQUIRE(s != nullptr);
        CHECK(s->activity == 2);
        CHECK(s->cooldownMs == 1800000);
        CHECK(s->buffDurationMs == 600000);
        REQUIRE(tag(*s, "dru"));
        REQUIRE(tag(*s, "hst3"));
        CHECK_FALSE(tag(*s, "msch")); // no transform tag: the devil's look is not the server's
        CHECK(game::EquipmentFits(*s, spirit));
        CHECK_FALSE(game::EquipmentFits(*s, bare));
    }
    CHECK(tag(*skills.Find(31141), "dru").Int(0) == 5);
    CHECK(tag(*skills.Find(31142), "dru").Int(0) == 20);
    CHECK(tag(*skills.Find(31143), "dru").Int(0) == 25);
    CHECK(tag(*skills.Find(31144), "dru").Int(0) == 30);
    CHECK(tag(*skills.Find(31144), "hst3").Int(0) == 20);
    for (const std::uint32_t id : {31151u, 31152u}) {
        const assets::SkillRow* s = skills.Find(id);
        REQUIRE(s != nullptr);
        CHECK(s->activity == 1);
        CHECK(s->cooldownMs == 1200000);
        CHECK(s->buffDurationMs == 300000);
        CHECK(tag(*s, "msid").Int(0) == int(id - 31150));
        CHECK(game::EquipmentFits(*s, spirit));
        CHECK_FALSE(game::EquipmentFits(*s, bare));
    }

    // Its alchemy: Sabakun's jewel fits it alone (Param1 14), its chance bytes 80, 70, 60, 50 / 40, 30, 25, 25 / 25;
    // the old event spirit (P2 0) is refused 0x540F; the scroll makes the jewel from 50 / 50 / 5 / 5 Devil's Blood with
    // no assimilation rate; the UMDU stone goes on it (avatar_special_armlet) for 2, 4, 6, 8 or 10.
    assets::ItemTable items;
    REQUIRE(items.Load(fs, error));
    const assets::ItemRow* mall = items.Find("ITEM_MALL_AVATAR_M_NASRUN");
    const assets::ItemRow* old = items.Find("ITEM_EVENT_AVATAR_M_NASRUN");
    const assets::ItemRow* jewel = items.Find("ITEM_ETC_NASRUN_ARCHEMY_UPGRADE_STONE");
    const assets::ItemRow* scroll = items.Find("ITEM_ETC_NASRUN_ARCHEMY_RECIPE");
    const assets::ItemRow* umdu = items.Find("ITEM_MALL_NASRUN_ARCHEMY_MAGICSTONE_UMDU");
    const assets::ItemRow* sword = items.Find("ITEM_CH_SWORD_01_A_DEF");
    REQUIRE((mall && old && jewel && scroll && umdu && sword));
    game::ItemInstance item;
    item.refId = mall->refObjId;
    item.durability = std::uint32_t(Now() + 3600);
    CHECK(game::alchemy::CheckReinforce(*mall, item, *jewel, nullptr) == 0);
    CHECK(game::alchemy::CheckReinforce(*old, item, *jewel, nullptr) == game::alchemy::code::kCannotReinforce);
    game::ItemInstance blade;
    blade.refId = sword->refObjId;
    CHECK(game::alchemy::CheckReinforce(*sword, blade, *jewel, nullptr) == game::alchemy::code::kRecipeMismatch);
    const int bytes[] = {80, 70, 60, 50, 40, 30, 25, 25, 25, 25, 25, 25, 25};
    for (int plus = 0; plus <= 12; ++plus) CHECK(game::alchemy::ChanceByte(*jewel, plus) == bytes[plus]);
    game::alchemy::Recipe recipe;
    REQUIRE(game::alchemy::TabletRecipe(items, *scroll, recipe));
    CHECK(recipe.stone == jewel);
    REQUIRE(recipe.elements.size() == 4);
    CHECK(recipe.elements[0].second == 50);
    CHECK(recipe.elements[1].second == 50);
    CHECK(recipe.elements[2].second == 5);
    CHECK(recipe.elements[3].second == 5);
    std::mt19937 random(5);
    for (int i = 0; i < 20; ++i) CHECK(game::alchemy::StoneRate(*jewel, false, random) == 0);
    CHECK(game::alchemy::CheckMagicStone(*mall, item, *umdu, options) == 0);
    CHECK(game::alchemy::CheckMagicStone(*old, item, *umdu, options) == game::alchemy::code::kCannotReinforce);
    CHECK(game::alchemy::CheckMagicStone(*sword, blade, *umdu, options) == game::alchemy::code::kRecipeMismatch);
    game::ItemInstance stoneItem;
    stoneItem.refId = umdu->refObjId;
    std::map<std::uint32_t, int> seen;
    for (int i = 0; i < 200; ++i) {
        game::ItemInstance target = item;
        const game::alchemy::StoneResult r = game::alchemy::ApplyMagicStone(*mall, target, *umdu, stoneItem, options, 0, random);
        REQUIRE(r.success); // Param4 100: always
        CHECK(r.code == 0);
        REQUIRE(target.options.size() == 1);
        ++seen[target.options[0].value];
        CHECK(target.durability == item.durability); // the data word, the awakening's end, untouched
        const assets::MagicOptionRow* o = game::FindOption(options, target.options[0].id);
        REQUIRE(o != nullptr);
        CHECK(o->attribute == kUmdu);
    }
    CHECK(seen.size() == 5);
    for (const std::uint32_t v : {2u, 4u, 6u, 8u, 10u}) CHECK(seen.contains(v));
}

namespace {

struct SpiritServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    std::shared_ptr<assets::ItemTable> items = std::make_shared<assets::ItemTable>();
    std::uint32_t chest = 0, legs = 0, shoes = 0, sword = 0;
    SpiritServer(TestDatabase& db, const assets::FileSystem& fs) : t(db) {
        REQUIRE(items->Load(fs, error));
        const auto id = [&](const char* code) {
            const assets::ItemRow* r = items->Find(std::string_view(code));
            REQUIRE_MESSAGE(r != nullptr, code);
            return r->refObjId;
        };
        chest = id("ITEM_CH_M_HEAVY_01_BA_A_DEF");
        legs = id("ITEM_CH_M_HEAVY_01_LA_A_DEF");
        shoes = id("ITEM_CH_M_HEAVY_01_FA_A_DEF");
        sword = id("ITEM_CH_SWORD_01_A_DEF");
        config.listen = Local();
        config.databaseUrl = t.Url();
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
        rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
        rules->creation.AddArmourSet(game::CreationRules::Race::China, true, chest, legs, shoes);
        rules->creation.AddWeapon(game::CreationRules::Race::China, sword);
        rules->items = items;
        config.rules = rules;
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        config.logoutCountdown = std::chrono::seconds(0);
        config.databaseThreads = 2;
        config.devCommands = true; // DevGive, DevSheet
        auto options = std::make_shared<std::vector<assets::MagicOptionRow>>();
        const auto file = fs.Read("server_dep\\silkroad\\textdata\\magicoption.txt");
        REQUIRE(file);
        REQUIRE(assets::DecodeMagicOptions(core::DecodeTextFile(file->Bytes()), *options, error));
        config.magicOptions = options;
        auto skills = std::make_shared<assets::SkillTable>();
        REQUIRE(skills->Load(fs, error));
        config.skills = skills;
        auto spirits = std::make_shared<game::SpiritTable>();
        REQUIRE(game::LoadSpiritTable(fs, *options, *spirits, error));
        config.spirits = spirits;
        config.spiritCheck = 200ms;
    }
    struct Player {
        std::unique_ptr<Client> client;
        std::uint32_t entity = 0;
    };
    Player Make(const std::string& name) {
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
        m.chest = chest;
        m.legs = legs;
        m.shoes = shoes;
        m.weapon = sword;
        p.client->Send(m);
        proto::LobbyResult r;
        REQUIRE(p.client->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        const std::int64_t character = server::ListCharacters(t.Db(), account, config.id, 0, ok)[0].id;
        server::CharacterRow::Place here;
        here.region = assets::RegionId(168, 97);
        here.x = 1000;
        here.z = 1000;
        REQUIRE(server::SavePlaces(t.Db(), {{character, here}}));
        p.client->Send(proto::CharacterSelect{name});
        REQUIRE(p.client->Expect(r));
        p.client->Send(proto::WorldEnter{});
        proto::WorldEntered w;
        REQUIRE(p.client->Expect(w));
        REQUIRE(w.status == proto::WorldStatus::Ok);
        p.entity = w.entity;
        proto::InventoryList bag;
        REQUIRE(p.client->Expect(bag));
        return p;
    }
    // A spirit given (DevGive: its plus, data word and blues) into the bag: its place.
    std::uint8_t Give(Player& p, const char* code, std::uint8_t plus, std::uint32_t data, std::vector<std::uint64_t> magic = {}) {
        const assets::ItemRow* row = items->Find(std::string_view(code));
        REQUIRE(row != nullptr);
        p.client->Send(proto::DevGive{code, 1, false, magic, plus, data});
        const auto end = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < end) {
            proto::ItemMoved moved;
            if (!p.client->Expect(moved, 2s)) break;
            for (const proto::ItemRecord& i : moved.changed) {
                if (i.item == row->refObjId && i.plus == plus && i.durability == data && i.slot >= game::slot::kWorn) return i.slot;
            }
        }
        FAIL("the spirit never came into the bag");
        return 0;
    }
    proto::AvatarMoved Avatar(Player& p, bool wear, std::uint8_t place) {
        p.client->Send(proto::AvatarMove{wear, place});
        proto::AvatarMoved moved;
        REQUIRE(p.client->Expect(moved));
        return moved;
    }
    std::map<std::string, float> Sheet(Player& p) {
        p.client->Send(proto::DevSheet{});
        proto::DevSheetAnswer a;
        std::map<std::string, float> out;
        if (p.client->Expect(a)) {
            for (const proto::DevNumber& n : a.numbers) out[n.name] = n.value;
        }
        return out;
    }
};

double Number(const std::map<std::string, float>& m, const char* name) {
    const auto it = m.find(name);
    return it != m.end() ? double(it->second) : 0.0;
}

} // namespace

TEST_CASE("Game server: a Devil's Spirit worn at +3 gives its numbers and skills; off, they go; asleep while worn, they go "
          "too") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    SpiritServer s(t, fs);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto p = s.Make("Nasrun");

    const auto before = s.Sheet(p);
    const double str = Number(before, "strength"), intel = Number(before, "intelligence");
    REQUIRE(str > 0);
    CHECK(Number(before, "spiritOn") == 0);
    CHECK(Number(before, "block") == 0);

    // ITEM_MALL_AVATAR_M_NASRUN at +3, awake a day, an UMDU blue of 6 (option 353): HPNA 18, MPNA 18, brna 1; Devil
    // Confusion and the first defence ignore.
    const std::uint32_t day = std::uint32_t(Now() + 86400);
    const std::uint8_t at = s.Give(p, "ITEM_MALL_AVATAR_M_NASRUN", 3, day, {(std::uint64_t(6) << 32) | 353});
    const proto::AvatarMoved on = s.Avatar(p, true, at);
    REQUIRE(on.status == proto::ItemStatus::Ok);
    REQUIRE(on.avatars.size() == 1);
    CHECK(on.avatars[0].slot == game::avatar::kSpirit);
    auto sheet = s.Sheet(p);
    CHECK(Number(sheet, "spiritOn") == 1);
    const auto part = [](double base, float percent) { return double(float(base * double(percent + 100.0f) / 100.0)); };
    CHECK(Number(sheet, "maxHp") == double(game::PlayerVitalFrom(1, part(std::trunc(str), 18.0f))));
    CHECK(Number(sheet, "maxMp") == double(game::PlayerVitalFrom(1, part(std::trunc(intel), 18.0f))));
    CHECK(Number(sheet, "maxHp") > Number(before, "maxHp"));
    CHECK(Number(sheet, "block") == doctest::Approx(1.0));
    CHECK(Number(sheet, "uniqueDamage") == doctest::Approx(6.0));
    CHECK(Number(sheet, "itemSkill31143") == 1);
    CHECK(Number(sheet, "itemSkill31151") == 1);
    CHECK(Number(sheet, "strength") == doctest::Approx(str)); // STR itself unchanged

    // Its defence ignore cast (a free buff, reqi 14 1 held): the chance on the sheet; the spirit taken off ends it.
    p.client->Send(proto::UseSkill{31151, 0});
    proto::SkillUseAnswer used;
    const auto end = std::chrono::steady_clock::now() + 10s;
    bool answered = false;
    while (!answered && std::chrono::steady_clock::now() < end && p.client->Expect(used, 2s)) answered = used.skill == 31151;
    REQUIRE(answered);
    CHECK(used.status == proto::SkillUseStatus::Ok);
    double ignore = 0;
    for (int i = 0; i < 50 && ignore == 0; ++i) ignore = Number(s.Sheet(p), "ignoreDefence");
    CHECK(ignore == doctest::Approx(1.0));

    const proto::AvatarMoved off = s.Avatar(p, false, game::avatar::kSpirit);
    REQUIRE(off.status == proto::ItemStatus::Ok);
    sheet = s.Sheet(p);
    CHECK(Number(sheet, "spiritOn") == 0);
    CHECK(Number(sheet, "maxHp") == Number(before, "maxHp"));
    CHECK(Number(sheet, "maxMp") == Number(before, "maxMp"));
    CHECK(Number(sheet, "block") == 0);
    CHECK(Number(sheet, "uniqueDamage") == 0);
    CHECK(Number(sheet, "ignoreDefence") == 0);
    CHECK(sheet.count("itemSkill31143") == 0);
    CHECK(sheet.count("itemSkill31151") == 0);

    // A second one at +0 awake for 2 s more, worn: Eccentric Demon, HPNA 15; then its awakening ends while it is worn: its
    // numbers and skills go, its wearer is told (again at the next check), it stays worn.
    const std::uint32_t soon = std::uint32_t(Now() + 2);
    const std::uint8_t second = s.Give(p, "ITEM_MALL_AVATAR_M_NASRUN", 0, soon);
    REQUIRE(s.Avatar(p, true, second).status == proto::ItemStatus::Ok);
    sheet = s.Sheet(p);
    CHECK(Number(sheet, "spiritOn") == 1);
    CHECK(Number(sheet, "itemSkill31142") == 1);
    CHECK(Number(sheet, "maxHp") == double(game::PlayerVitalFrom(1, part(std::trunc(str), 15.0f))));
    proto::SpiritAsleep asleep;
    REQUIRE(p.client->Expect(asleep, 8s));
    CHECK(asleep.place == game::avatar::kSpirit);
    sheet = s.Sheet(p);
    CHECK(Number(sheet, "spiritOn") == 0);
    CHECK(Number(sheet, "maxHp") == Number(before, "maxHp"));
    CHECK(sheet.count("itemSkill31142") == 0);
    REQUIRE(p.client->Expect(asleep, 3s));
    // Still worn: taking it off answers with the spirit coming back into the bag.
    const proto::AvatarMoved back = s.Avatar(p, false, game::avatar::kSpirit);
    CHECK(back.status == proto::ItemStatus::Ok);
    CHECK(back.avatarsEmptied == std::vector<std::uint8_t>{std::uint8_t(game::avatar::kSpirit)});
}
