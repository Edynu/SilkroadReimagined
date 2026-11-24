// Alchemy: the rules on the 1.188 game data, then our game server's side.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <string>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Text.h"
#include "sr/game/Alchemy.h"

using namespace sr;
namespace alchemy = sr::game::alchemy;

namespace {

struct Data {
    assets::FileSystem fs;
    assets::ItemTable items;
    std::vector<assets::MagicOptionRow> options;
    std::vector<assets::MagicOptGroupRow> groups; // refmagicoptgroup.txt (the socket stones' skills)
    std::vector<assets::MagicOptionAssignRow> assign; // magicoptionassign.txt (the avatars' lists at the smith)
    bool ok = false;
    Data() {
        const char* dir = std::getenv("SR_GAME_DIR");
        if (dir == nullptr) return;
        std::string error;
        if (!assets::MountGameArchives(fs, dir, error) || !items.Load(fs, error)) return;
        const auto file = fs.Read("server_dep\\silkroad\\textdata\\magicoption.txt");
        if (!file || !assets::DecodeMagicOptions(core::DecodeTextFile(file->Bytes()), options, error)) return;
        const auto groupFile = fs.Read("server_dep\\silkroad\\textdata\\refmagicoptgroup.txt");
        if (!groupFile || !assets::DecodeMagicOptGroups(core::DecodeTextFile(groupFile->Bytes()), groups, error)) return;
        const auto assignFile = fs.Read("server_dep\\silkroad\\textdata\\magicoptionassign.txt");
        if (!assignFile || !assets::DecodeMagicOptionAssign(core::DecodeTextFile(assignFile->Bytes()), assign, error)) return;
        ok = true;
    }
    const assets::ItemRow& Row(const char* code) const {
        const assets::ItemRow* row = items.Find(std::string_view(code));
        REQUIRE_MESSAGE(row != nullptr, code);
        return *row;
    }
    // The first item in service of those TypeIDs whose MaxMagicOptCount is `capacity` (-1: any), by codename.
    const assets::ItemRow* First(int tid3, int tid4, int capacity = -1) const {
        const assets::ItemRow* best = nullptr;
        for (const auto& [code, row] : items.rows()) {
            if (row.typeId[0] == 3 && row.typeId[1] == 1 && row.typeId[2] == tid3 && row.typeId[3] == tid4 && (capacity < 0 || row.maxMagicOptions == capacity) &&
                (best == nullptr || code < best->codename)) {
                best = &row;
            }
        }
        return best;
    }
    const assets::MagicOptionRow& Option(std::string_view name, int degree = 1) const {
        const assets::MagicOptionRow* row = alchemy::OptionRow(options, name, degree);
        REQUIRE_MESSAGE(row != nullptr, std::string(name));
        return *row;
    }
};

const Data& GameData() {
    static const Data data;
    return data;
}

#define REQUIRE_GAME_DATA(d)                                   \
    const Data& d = GameData();                                \
    if (!d.ok) {                                               \
        MESSAGE("SR_GAME_DIR missing - skipped");              \
        return;                                                \
    }

} // namespace

TEST_CASE("Alchemy: the reinforcement chances are the elixirs' and powders' bytes, high first, clamped 10..100") {
    REQUIRE_GAME_DATA(d);
    const assets::ItemRow& a = d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_A");
    const assets::ItemRow& b = d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B");
    const assets::ItemRow& powder = d.Row("ITEM_ETC_ARCHEMY_REINFORCE_PROB_UP_A_01");
    // The note's table: B alone, B with powder A, A alone, by the plus before.
    const int bAlone[13] = {50, 40, 30, 19, 17, 17, 17, 17, 17, 12, 12, 12, 12};
    const int bPowder[13] = {100, 70, 50, 27, 25, 25, 25, 25, 25, 20, 20, 20, 20};
    const int aAlone[13] = {25, 20, 15, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10};
    for (int plus = 0; plus < 13; ++plus) {
        CAPTURE(plus);
        CHECK(alchemy::ReinforceChance(b, nullptr, plus, false).percent == bAlone[plus]);
        CHECK(alchemy::ReinforceChance(b, &powder, plus, false).percent == bPowder[plus]);
        CHECK(alchemy::ReinforceChance(a, nullptr, plus, false).percent == aAlone[plus]);
    }
    // Luck's 5 while under 100 (and then used), the premium's bonus after.
    const alchemy::Chance luck = alchemy::ReinforceChance(b, nullptr, 3, true, 5);
    CHECK(luck.percent == 29);
    CHECK(luck.usesLuck);
    CHECK_FALSE(alchemy::ReinforceChance(b, &powder, 0, true).usesLuck);
}

TEST_CASE("Alchemy: which elixir fits which item, the powder's degree, the plus limit") {
    REQUIRE_GAME_DATA(d);
    const assets::ItemRow& sword = d.Row("ITEM_CH_SWORD_01_A");
    const assets::ItemRow& shield = d.Row("ITEM_CH_SHIELD_01_A");
    const assets::ItemRow& euArmour = d.Row("ITEM_EU_M_HEAVY_01_HA_A");
    const assets::ItemRow& euRing = d.Row("ITEM_EU_RING_01_A");
    CHECK(alchemy::ElixirFits(d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B"), sword));
    CHECK_FALSE(alchemy::ElixirFits(d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B"), shield));
    CHECK(alchemy::ElixirFits(d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_SHIELD_B"), shield));
    // Armour A fits China's (1..3) only, B Europe's (9..11) too; accessories A 5, B 12 too.
    CHECK_FALSE(alchemy::ElixirFits(d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_ARMOR_A"), euArmour));
    CHECK(alchemy::ElixirFits(d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_ARMOR_B"), euArmour));
    CHECK_FALSE(alchemy::ElixirFits(d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_ACCESSARY_A"), euRing));
    CHECK(alchemy::ElixirFits(d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_ACCESSARY_B"), euRing));

    game::ItemInstance item;
    item.refId = sword.refObjId;
    const assets::ItemRow& elixir = d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B");
    CHECK(alchemy::CheckReinforce(sword, item, elixir, nullptr) == 0);
    CHECK(alchemy::CheckReinforce(sword, item, elixir, &d.Row("ITEM_ETC_ARCHEMY_REINFORCE_PROB_UP_A_01")) == 0);
    CHECK(alchemy::CheckReinforce(sword, item, elixir, &d.Row("ITEM_ETC_ARCHEMY_REINFORCE_PROB_UP_A_02")) == alchemy::code::kPowderDegree);
    CHECK(alchemy::CheckReinforce(shield, item, elixir, nullptr) == alchemy::code::kRecipeMismatch);
    CHECK(alchemy::CheckReinforce(d.Row("ITEM_ETC_HP_POTION_01"), item, elixir, nullptr) == alchemy::code::kOnlyEquipment);
    item.plus = 250;
    CHECK(alchemy::CheckReinforce(sword, item, elixir, nullptr) == alchemy::code::kCannotReinforce);
}

TEST_CASE("Alchemy: a failed reinforcement: +1..+3 to +0, from +4 to +0 or +4 with Astral, from +5 half destroyed unless Immortal, half the durability unless Steady") {
    REQUIRE_GAME_DATA(d);
    const assets::ItemRow& sword = d.Row("ITEM_CH_SWORD_01_A");
    const assets::ItemRow& elixir = d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_A");
    const auto option = [&](const char* name, int degree) {
        const assets::MagicOptionRow* row = alchemy::OptionRow(d.options, name, degree);
        REQUIRE_MESSAGE(row != nullptr, name);
        return row->paramId;
    };
    std::mt19937 random(5);
    int successes = 0, destroyed = 0, penalties = 0, failures = 0;
    for (int i = 0; i < 4000; ++i) {
        game::ItemInstance item;
        item.refId = sword.refObjId;
        item.plus = 7;
        const alchemy::ReinforceResult r = alchemy::Reinforce(sword, item, elixir, nullptr, d.options, 0, random);
        CHECK(r.chance == 10);
        if (r.success) {
            ++successes;
            CHECK(item.plus == 8);
            continue;
        }
        ++failures;
        if (r.destroyed) ++destroyed;
        if (r.durabilityLost) {
            ++penalties;
            REQUIRE(item.options.size() == 1);
            CHECK(item.options[0].id == option("MATTR_DEC_MAXDUR", 3));
            CHECK(item.options[0].value >= 36);
            CHECK(item.options[0].value <= 50); // 36 + rand() / 32767 x 14: 50 only at rand() 32767
        }
        if (!r.destroyed) CHECK(item.plus == 0);
    }
    MESSAGE("+7 with elixir A: " << successes << " successes, " << destroyed << " destroyed, " << penalties << " penalties of " << failures);
    CHECK(successes > 300);
    CHECK(successes < 500);
    CHECK(destroyed > failures * 45 / 100);
    CHECK(destroyed + penalties == failures);

    // +1..+3 fall to 0 and nothing else; +4 kept by an Astral charge; Immortal and Steady stop their halves.
    for (int i = 0; i < 200; ++i) {
        game::ItemInstance item;
        item.plus = std::uint8_t(1 + i % 3);
        const alchemy::ReinforceResult r = alchemy::Reinforce(sword, item, elixir, nullptr, d.options, -100, random); // the floor: 10 %
        if (!r.success) {
            CHECK(item.plus == 0);
            CHECK_FALSE(r.destroyed);
            CHECK_FALSE(r.durabilityLost);
        }
    }
    for (int i = 0; i < 300; ++i) {
        game::ItemInstance item;
        item.plus = 9;
        item.options = {{option("MATTR_ASTRAL", 3), 6}, {option("MATTR_ATHANASIA", 3), 6}, {option("MATTR_SOLID", 3), 6}};
        const alchemy::ReinforceResult r = alchemy::Reinforce(sword, item, elixir, nullptr, d.options, -100, random);
        if (r.success) continue;
        CHECK_FALSE(r.destroyed);
        CHECK_FALSE(r.durabilityLost);
        CHECK(r.astralKept);
        CHECK(item.plus == 4);
        CHECK(alchemy::OptionValue(item, d.options, "MATTR_ASTRAL") == 5);
        CHECK(alchemy::OptionValue(item, d.options, "MATTR_ATHANASIA") + alchemy::OptionValue(item, d.options, "MATTR_SOLID") == 11);
    }
}

TEST_CASE("Alchemy: stones: a magic stone's chance, its option's values at its degree, charges up to 6; an attribute stone's field") {
    REQUIRE_GAME_DATA(d);
    const assets::ItemRow& sword = d.Row("ITEM_CH_SWORD_01_A");
    CHECK(alchemy::OptionValues(d.options, "MATTR_STR", 1) == std::vector<std::uint32_t>{1, 2, 3});
    const assets::ItemRow& str = d.Row("ITEM_ETC_ARCHEMY_MAGICSTONE_STR_01");
    game::ItemInstance item, stone;
    item.refId = sword.refObjId;
    CHECK(alchemy::CheckMagicStone(sword, item, str, d.options) == 0);
    CHECK(alchemy::CheckMagicStone(sword, item, d.Row("ITEM_ETC_ARCHEMY_MAGICSTONE_STR_02"), d.options) == alchemy::code::kCannotReinforce);
    std::mt19937 random(9);
    int wins = 0;
    for (int i = 0; i < 2000; ++i) {
        game::ItemInstance target = item;
        const alchemy::StoneResult r = alchemy::ApplyMagicStone(sword, target, str, stone, d.options, 0, random);
        CHECK(r.chance == 50);
        if (!r.success) continue;
        ++wins;
        REQUIRE(target.options.size() == 1);
        CHECK(target.options[0].value >= 1);
        CHECK(target.options[0].value <= 3);
    }
    MESSAGE("STR stones on a sword: " << wins << " of 2000");
    CHECK(wins > 2000 * 46 / 100); // 51 %
    CHECK(wins < 2000 * 56 / 100);

    // A protection stone (Param4 100) always succeeds; charges stop at 6.
    const assets::ItemRow& immortal = d.Row("ITEM_ETC_ARCHEMY_MAGICSTONE_ATHANASIA_01");
    game::ItemInstance protectedItem = item;
    for (int i = 0; i < 6; ++i) CHECK(alchemy::ApplyMagicStone(sword, protectedItem, immortal, stone, d.options, 0, random).code == 0);
    CHECK(alchemy::OptionValue(protectedItem, d.options, "MATTR_ATHANASIA") == 6);
    CHECK(alchemy::ApplyMagicStone(sword, protectedItem, immortal, stone, d.options, 0, random).code == alchemy::code::kChargesFull);
    // Astral needs Immortal above it.
    CHECK(alchemy::CheckMagicStone(sword, item, d.Row("ITEM_ETC_ARCHEMY_MAGICSTONE_ASTRAL_01"), d.options) == alchemy::code::kAstral);
    CHECK(alchemy::CheckMagicStone(sword, protectedItem, d.Row("ITEM_ETC_ARCHEMY_MAGICSTONE_ASTRAL_01"), d.options) == 0);

    // An attribute stone sets its field (a sword's PA is field 4) to one of 0, 7, 13, 19, 25, 31.
    const assets::ItemRow& pa = d.Row("ITEM_ETC_ARCHEMY_ATTRSTONE_PA_01");
    CHECK(alchemy::CheckAttrStone(sword, pa) == 0);
    CHECK(alchemy::CheckAttrStone(d.Row("ITEM_CH_SHIELD_01_A"), pa) == alchemy::code::kStoneMismatch);
    std::map<int, int> values;
    for (int i = 0; i < 2000; ++i) {
        game::ItemInstance target = item;
        const alchemy::StoneResult r = alchemy::ApplyAttrStone(sword, target, pa, stone, d.options, 0, random);
        if (r.success) ++values[game::VarianceSlot(target.variance, 4)];
    }
    for (const auto& [value, n] : values) CHECK((value == 0 || value == 7 || value == 13 || value == 19 || value == 25 || value == 31));
    CHECK(values[0] > values[7]);
    CHECK(values[7] > values[13]);
}

TEST_CASE("Alchemy: a tablet's recipe, the Rondos' counts, what breaking down gives") {
    REQUIRE_GAME_DATA(d);
    alchemy::Recipe recipe;
    REQUIRE(alchemy::TabletRecipe(d.items, d.Row("ITEM_ETC_ARCHEMY_MAGICTABLET_STR_01"), recipe));
    CHECK(recipe.stone->codename == "ITEM_ETC_ARCHEMY_MAGICSTONE_STR_01");
    REQUIRE(recipe.elements.size() == 4);
    CHECK(recipe.elements[0].first->codename == "ITEM_ETC_ARCHEMY_ELEMENT_EARTH_01");
    CHECK(recipe.elements[0].second == 110);
    CHECK(recipe.elements[1].second == 30);
    std::mt19937 random(3);
    for (int i = 0; i < 50; ++i) {
        const int rate = alchemy::StoneRate(*recipe.stone, false, random);
        CHECK((rate == 10 || rate == 20 || rate == 30));
    }

    const assets::ItemRow& sword = d.Row("ITEM_CH_SWORD_01_A");
    CHECK(alchemy::DestroyerRondos(sword) == int(sword.price / 20000 + 1));
    game::ItemInstance item;
    item.refId = sword.refObjId;
    item.plus = 3;
    alchemy::StoneBucket bucket;
    bucket.attr = {{&d.Row("ITEM_ETC_ARCHEMY_ATTRSTONE_PA_01"), 60.0}};
    const alchemy::DismantleYield yield = alchemy::Dismantle(d.items, sword, item, bucket, random);
    CHECK(yield.elements.size() >= 2);
    for (const auto& [row, n] : yield.elements) {
        CHECK(row->codename.ends_with("_01"));
        CHECK(n >= 1);
    }
    CHECK(alchemy::CheckDismantle(d.Row("ITEM_ETC_HP_POTION_01"), item, d.options) == alchemy::code::kCannotTarget);
}


// --- Our game server's alchemy over a test host ------------------------------------------------------------------------

#include "sr/server/Alchemy.h"

namespace {

struct AlchemyBench : server::AlchemyHost {
    const Data& d;
    game::Inventory bag;
    bool alive = true;
    std::vector<game::proto::AlchemyResult> results;
    std::int64_t nextSerial = 1000;
    explicit AlchemyBench(const Data& data) : d(data) { bag.size = 13 + 96; }
    std::optional<Alchemist> AlchemyPlayer(std::uint32_t) override { return Alchemist{1, alive, true, &bag, 0, 0}; }
    void AlchemySend(std::uint64_t, game::proto::MessageId id, const std::vector<std::byte>& payload) override {
        game::proto::AlchemyResult r;
        if (id == game::proto::AlchemyResult::kId && game::proto::Decode(payload, r)) results.push_back(r);
    }
    const assets::ItemTable* AlchemyItems() const override { return &d.items; }
    const std::vector<assets::MagicOptionRow>* AlchemyOptions() const override { return &d.options; }
    const std::vector<assets::MagicOptGroupRow>* AlchemySocketGroups() const override { return &d.groups; }
    const std::vector<assets::MagicOptionAssignRow>* AlchemyAssign() const override { return &d.assign; }
    bool atSmith = true; // the NPC asked for stands in reach with the service
    bool AlchemyAtNpc(std::uint32_t, std::uint32_t npc, std::uint32_t service) const override {
        return atSmith && npc == 99 && service == game::proto::kNpcAvatarGrant;
    }
    const game::alchemy::StoneBucket* AlchemyStones(int) const override { return nullptr; }
    bool AlchemyGive(std::uint32_t, const std::vector<game::ItemInstance>& items, std::vector<std::uint8_t>& touched) override {
        for (game::ItemInstance item : items) {
            const int place = bag.FreeBagPlace();
            if (place < 0) return false;
            item.serial = nextSerial++;
            bag.places[std::uint8_t(place)] = item;
            touched.push_back(std::uint8_t(place));
        }
        return true;
    }
    void AlchemyBag(std::uint32_t, std::vector<std::uint8_t>, std::vector<std::int64_t>) override {}
    void Put(std::uint8_t place, const char* code, int count = 1, int plus = 0) {
        game::ItemInstance item;
        item.refId = d.Row(code).refObjId;
        item.count = std::uint16_t(count);
        item.plus = std::uint8_t(plus);
        item.serial = nextSerial++;
        bag.places[place] = item;
    }
    template <typename T>
    void Ask(server::Alchemy& alchemy, const T& m) {
        REQUIRE(alchemy.Handle(7, T::kId, game::proto::Encode(m), server::Alchemy::Clock::now()));
    }
};

} // namespace

TEST_CASE("Alchemy on the game server: a strengthening's result on the 4th tick, the elixir used; a cancel uses nothing; a stone made, a stack disjoined") {
    REQUIRE_GAME_DATA(d);
    AlchemyBench bench(d);
    server::Alchemy alchemy(bench, 17);
    bench.Put(13, "ITEM_CH_SWORD_01_A");
    bench.Put(14, "ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B", 3);
    bench.Put(15, "ITEM_ETC_ARCHEMY_REINFORCE_PROB_UP_A_01", 2);
    auto now = server::Alchemy::Clock::now();
    const auto tick = [&] {
        now += std::chrono::seconds(1);
        alchemy.Tick(now);
    };
    // A wrong pairing is refused at once: the powder of another degree.
    bench.Put(16, "ITEM_ETC_ARCHEMY_REINFORCE_PROB_UP_A_02");
    bench.Ask(alchemy, game::proto::AlchemyStrengthen{13, 14, 16});
    REQUIRE(bench.results.size() == 1);
    CHECK(bench.results[0].outcome == game::proto::AlchemyOutcome::Refused);
    CHECK(bench.results[0].code == game::alchemy::code::kPowderDegree);
    bench.results.clear();

    // +0 with elixir B and powder A: 100 %, on the 4th tick.
    bench.Ask(alchemy, game::proto::AlchemyStrengthen{13, 14, 15});
    CHECK(alchemy.Busy(7));
    for (int i = 0; i < 3; ++i) {
        tick();
        CHECK(bench.results.empty());
    }
    tick();
    REQUIRE(bench.results.size() == 1);
    CHECK(bench.results[0].outcome == game::proto::AlchemyOutcome::Success);
    CHECK(bench.results[0].plusAfter == 1);
    CHECK(bench.bag.At(13)->plus == 1);
    CHECK(bench.bag.At(14)->count == 2);
    CHECK(bench.bag.At(15)->count == 1);
    CHECK_FALSE(alchemy.Busy(7));
    bench.results.clear();

    // A cancel before the result: nothing used.
    bench.Ask(alchemy, game::proto::AlchemyStrengthen{13, 14, 0});
    tick();
    bench.Ask(alchemy, game::proto::AlchemyCancel{});
    REQUIRE(bench.results.size() == 1);
    CHECK(bench.results[0].outcome == game::proto::AlchemyOutcome::Cancelled);
    CHECK(bench.bag.At(14)->count == 2);
    CHECK(bench.bag.At(13)->plus == 1);
    bench.results.clear();

    // A stone from a tablet and its elements: at once, exactly the listed counts used.
    bench.Put(20, "ITEM_ETC_ARCHEMY_MAGICTABLET_STR_01", 2);
    bench.Put(21, "ITEM_ETC_ARCHEMY_ELEMENT_EARTH_01", 150);
    bench.Put(22, "ITEM_ETC_ARCHEMY_ELEMENT_WATER_01", 30);
    bench.Put(23, "ITEM_ETC_ARCHEMY_ELEMENT_FIRE_01", 31);
    bench.Put(24, "ITEM_ETC_ARCHEMY_ELEMENT_WIND_01", 40);
    bench.Ask(alchemy, game::proto::AlchemyManufacture{20, {21, 22, 23, 24}});
    REQUIRE(bench.results.size() == 1);
    CHECK(bench.results[0].outcome == game::proto::AlchemyOutcome::Success);
    CHECK(bench.bag.At(20)->count == 1);
    CHECK(bench.bag.At(21)->count == 40);
    CHECK(bench.bag.At(22) == nullptr);
    CHECK(bench.bag.At(23)->count == 1);
    CHECK(bench.bag.At(24)->count == 10);
    int stones = 0;
    for (const auto& [place, item] : bench.bag.places) {
        if (item.refId == d.Row("ITEM_ETC_ARCHEMY_MAGICSTONE_STR_01").refObjId) {
            ++stones;
            CHECK((item.plus == 10 || item.plus == 20 || item.plus == 30));
        }
    }
    CHECK(stones == 1);
    // Short of an element: refused, nothing used.
    bench.results.clear();
    bench.Ask(alchemy, game::proto::AlchemyManufacture{20, {21, 23, 24}});
    REQUIRE(bench.results.size() == 1);
    CHECK(bench.results[0].code == game::alchemy::code::kElementsShort);
    CHECK(bench.bag.At(20)->count == 1);
}

TEST_CASE("Alchemy on the game server: a magic cube stands for the elixir: its first elixir's kind and chance, one used out of its last place, the cube kept") {
    REQUIRE_GAME_DATA(d);
    // A cube of our own (TID 3/2/3/1) beside the game's items; its store as the game server keeps it.
    struct CubeBench : AlchemyBench {
        assets::ItemTable items;
        std::map<std::uint8_t, game::ItemInstance> store;
        std::uint8_t cubePlace = 0;
        explicit CubeBench(const Data& data) : AlchemyBench(data), items(data.items) {
            assets::ItemRow cube;
            cube.refObjId = 990001;
            cube.codename = "ITEM_T_MAGIC_CUBE";
            cube.typeId = {3, 2, 3, 1};
            cube.inService = true;
            cube.maxStack = 1;
            items.Add(cube);
        }
        const assets::ItemTable* AlchemyItems() const override { return &items; }
        std::optional<game::ItemInstance> AlchemyCubeElixir(std::uint32_t, std::uint8_t place) override {
            if (place != cubePlace || store.empty()) return std::nullopt;
            return store.begin()->second;
        }
        bool AlchemyCubeUseOne(std::uint32_t, std::uint8_t place) override {
            if (place != cubePlace || store.empty()) return false;
            auto last = std::prev(store.end());
            if (last->second.count > 1) --last->second.count;
            else store.erase(last);
            return true;
        }
        void Store(std::uint8_t slot, const char* code, int count) {
            game::ItemInstance item;
            item.refId = d.Row(code).refObjId;
            item.count = std::uint16_t(count);
            item.serial = nextSerial++;
            store[slot] = item;
        }
    };
    CubeBench bench(d);
    server::Alchemy alchemy(bench, 17);
    bench.Put(13, "ITEM_CH_SWORD_01_A");
    game::ItemInstance cube;
    cube.refId = 990001;
    cube.count = 1;
    cube.serial = bench.nextSerial++;
    bench.bag.places[14] = cube;
    bench.cubePlace = 14;
    bench.Put(15, "ITEM_ETC_ARCHEMY_REINFORCE_PROB_UP_A_01", 2);
    auto now = server::Alchemy::Clock::now();
    const auto run = [&] {
        for (int i = 0; i < 4; ++i) {
            now += std::chrono::seconds(1);
            alchemy.Tick(now);
        }
    };
    // Empty: no elixir (0x5407).
    bench.Ask(alchemy, game::proto::AlchemyStrengthen{13, 14, 15});
    REQUIRE(bench.results.size() == 1);
    CHECK(bench.results[0].code == game::alchemy::code::kRecipeMissing);
    bench.results.clear();
    // An armour elixir first in it: not for a sword (its kind checked as the elixir's own).
    bench.Store(0, "ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_ARMOR_B", 1);
    bench.Ask(alchemy, game::proto::AlchemyStrengthen{13, 14, 15});
    REQUIRE(bench.results.size() == 1);
    CHECK(bench.results[0].outcome == game::proto::AlchemyOutcome::Refused);
    CHECK(bench.results[0].code != 0);
    bench.results.clear();
    // Weapon elixirs: +0 with elixir B and powder A at 100 %; one leaves the cube's last place, the cube stays.
    bench.store.clear();
    bench.Store(0, "ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B", 1);
    bench.Store(1, "ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B", 2);
    bench.Ask(alchemy, game::proto::AlchemyStrengthen{13, 14, 15});
    run();
    REQUIRE(bench.results.size() == 1);
    CHECK(bench.results[0].outcome == game::proto::AlchemyOutcome::Success);
    CHECK(bench.bag.At(13)->plus == 1);
    REQUIRE(bench.bag.At(14) != nullptr); // the cube kept
    CHECK(bench.bag.At(14)->refId == 990001);
    CHECK(bench.bag.At(15)->count == 1);   // the powder used
    REQUIRE(bench.store.size() == 2);
    CHECK(bench.store[1].count == 1);
    CHECK(bench.store[0].count == 1);
    bench.results.clear();
    // Again (no powder): the last place emptied and gone.
    bench.Ask(alchemy, game::proto::AlchemyStrengthen{13, 14, 0});
    run();
    REQUIRE(bench.results.size() == 1);
    CHECK(bench.results[0].outcome != game::proto::AlchemyOutcome::Refused);
    CHECK(bench.store.size() == 1);
    CHECK(bench.store.contains(0));
    REQUIRE(bench.bag.At(14) != nullptr);
}

TEST_CASE("Alchemy on the game server: a piece dismantled with the Destroyer Rondos it needs; refused without them") {
    REQUIRE_GAME_DATA(d);
    AlchemyBench bench(d);
    server::Alchemy alchemy(bench, 23);
    bench.Put(13, "ITEM_CH_SWORD_01_A", 1, 2);
    bench.Ask(alchemy, game::proto::AlchemyDismantle{13});
    REQUIRE(bench.results.size() == 1);
    CHECK(bench.results[0].code == game::alchemy::code::kNotEnough);
    CHECK(bench.bag.At(13) != nullptr);
    bench.results.clear();
    const int need = game::alchemy::DestroyerRondos(d.Row("ITEM_CH_SWORD_01_A"));
    bench.Put(30, "ITEM_ETC_ARCHEMY_RONDO_02", need + 2);
    bench.Ask(alchemy, game::proto::AlchemyDismantle{13});
    REQUIRE(bench.results.size() == 1);
    CHECK(bench.results[0].outcome == game::proto::AlchemyOutcome::Success);
    CHECK(bench.bag.At(13) == nullptr);
    CHECK(bench.bag.At(30)->count == 2);
    int elements = 0;
    for (const auto& [place, item] : bench.bag.places) {
        if (d.items.Find(item.refId)->codename.starts_with("ITEM_ETC_ARCHEMY_ELEMENT_")) elements += item.count;
    }
    MESSAGE("a +2 degree-1 sword gave " << elements << " elements");
    CHECK(elements >= 4);
}


TEST_CASE("Alchemy: the advanced elixir: its checks, its plus beside the item's own, the total in the stats, a dismantle's recipe a level up") {
    REQUIRE_GAME_DATA(d);
    const assets::ItemRow& sword = d.Row("ITEM_CH_SWORD_01_A");
    const assets::ItemRow& weA1 = d.Row("ITEM_ETC_ARCHEMY_UPPER_REINFORCE_RECIPE_WE_A_1");
    const assets::ItemRow& weB3 = d.Row("ITEM_ETC_ARCHEMY_UPPER_REINFORCE_RECIPE_WE_B_3");
    const assets::ItemRow& weRareA1 = d.Row("ITEM_ETC_ARCHEMY_UPPER_REINFORCE_RECIPE_WE_RARE_A_1");
    const assets::ItemRow& shA1 = d.Row("ITEM_ETC_ARCHEMY_UPPER_REINFORCE_RECIPE_SH_A_1");
    CHECK(game::alchemy::KindOf(weA1) == game::alchemy::Kind::AdvancedElixir);
    game::ItemInstance item;
    item.refId = sword.refObjId;
    item.plus = 3;
    CHECK(game::alchemy::CheckAdvanced(sword, item, weA1) == 0);
    CHECK(game::alchemy::CheckAdvanced(sword, item, weB3) == 0); // a higher level fits a lower degree
    CHECK(game::alchemy::CheckAdvanced(sword, item, weRareA1) == game::alchemy::code::kRecipeMismatch); // a SoX recipe on a normal sword
    CHECK(game::alchemy::CheckAdvanced(sword, item, shA1) == game::alchemy::code::kRecipeMismatch);
    // A level below the item's degree.
    const assets::ItemRow* degree2 = d.items.Find(std::string_view("ITEM_CH_SWORD_02_A"));
    REQUIRE(degree2 != nullptr);
    REQUIRE(game::alchemy::Degree(*degree2) == 2);
    game::ItemInstance other;
    other.refId = degree2->refObjId;
    CHECK(game::alchemy::CheckAdvanced(*degree2, other, weA1) == game::alchemy::code::kRecipeLevel);
    // Applied: the item's own plus stays, the advanced is the recipe's Param8 (+1 A, +2 B), once.
    game::alchemy::ApplyAdvanced(item, weB3);
    CHECK(item.plus == 3);
    CHECK(item.advanced == 2);
    CHECK(item.advancedRecipe == weB3.refObjId);
    CHECK(game::TotalPlus(item) == 5);
    CHECK(game::alchemy::CheckAdvanced(sword, item, weA1) == game::alchemy::code::kAlreadyDone);
    // The stats take the total: a +3 with +2 advanced is a +5's.
    game::ItemInstance five;
    five.refId = sword.refObjId;
    five.plus = 5;
    CHECK(game::ComputeStats(sword, item, &d.options).phyAttack.high == doctest::Approx(game::ComputeStats(sword, five, &d.options).phyAttack.high));
    // A reinforcement's chance reads the item's own plus (+3: B's 19), not the total.
    CHECK(game::alchemy::ReinforceChance(d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B"), nullptr, item.plus, false).percent == 19);
    // A dismantle's recipe: a SoX piece's merit (8 x its tier + 2..6) gives an A or a B one level above its degree; a
    // normal piece's never reaches 10.
    const assets::ItemRow* sox = nullptr;
    for (const char* code : {"ITEM_CH_SWORD_01_C_RARE", "ITEM_CH_SWORD_01_B_RARE", "ITEM_CH_SWORD_01_A_RARE"}) {
        if ((sox = d.items.Find(std::string_view(code))) != nullptr) break;
    }
    REQUIRE(sox != nullptr);
    game::ItemInstance soxItem;
    soxItem.refId = sox->refObjId;
    const assets::ItemRow* recipe = game::alchemy::DismantleRecipe(d.items, *sox, soxItem);
    REQUIRE(recipe != nullptr);
    MESSAGE(sox->codename << " gives " << recipe->codename);
    CHECK(recipe->codename.starts_with("ITEM_ETC_ARCHEMY_UPPER_REINFORCE_RECIPE_WE_RARE_"));
    CHECK(recipe->codename.ends_with("_2"));
    CHECK(game::alchemy::DismantleRecipe(d.items, sword, item) == nullptr);
}

TEST_CASE("Alchemy on the game server: an advanced elixir never fails, on the 4th tick; only the recipe is used; the total plus told") {
    REQUIRE_GAME_DATA(d);
    AlchemyBench bench(d);
    server::Alchemy alchemy(bench, 29);
    bench.Put(13, "ITEM_CH_SWORD_01_A", 1, 3);
    bench.Put(14, "ITEM_ETC_ARCHEMY_UPPER_REINFORCE_RECIPE_WE_B_1");
    bench.Put(15, "ITEM_ETC_ARCHEMY_UPPER_REINFORCE_RECIPE_WE_A_1");
    auto now = server::Alchemy::Clock::now();
    bench.Ask(alchemy, game::proto::AlchemyStrengthen{13, 14, 0});
    for (int i = 0; i < 4; ++i) {
        now += std::chrono::seconds(1);
        alchemy.Tick(now);
    }
    REQUIRE(bench.results.size() == 1);
    CHECK(bench.results[0].outcome == game::proto::AlchemyOutcome::Success);
    CHECK(bench.results[0].plusBefore == 3);
    CHECK(bench.results[0].plusAfter == 5);
    CHECK(bench.bag.At(13)->plus == 3);
    CHECK(bench.bag.At(13)->advanced == 2);
    CHECK(bench.bag.At(14) == nullptr);
    // A second one is refused at once (0x5426), nothing used.
    bench.results.clear();
    bench.Ask(alchemy, game::proto::AlchemyStrengthen{13, 15, 0});
    REQUIRE(bench.results.size() == 1);
    CHECK(bench.results[0].code == game::alchemy::code::kAlreadyDone);
    CHECK(bench.bag.At(15) != nullptr);
}

// --- Sockets ------------------------------------------------------------------------------------

TEST_CASE("Sockets: which items take them (Param20), the stones' kinds and groups, the checks' codes in the original's order (1.188 data)") {
    REQUIRE_GAME_DATA(d);
    // Param20: degree-11 weapons, chest pieces and the Chinese shields have 3; others none.
    int socketed = 0, weapons = 0, chests = 0, shields = 0;
    for (const auto& [code, row] : d.items.rows()) {
        if (!row.inService || row.typeId[0] != 3 || row.typeId[1] != 1 || alchemy::SocketCount(row) == 0) continue;
        ++socketed;
        CHECK(alchemy::SocketCount(row) == 3);
        CHECK(alchemy::Degree(row) >= 11);
        weapons += row.typeId[2] == 6 ? 1 : 0;
        shields += row.typeId[2] == 4 ? 1 : 0;
        chests += row.typeId[2] != 6 && row.typeId[2] != 4 && row.typeId[3] == 3 ? 1 : 0;
    }
    MESSAGE(socketed << " items take sockets: " << weapons << " weapons, " << chests << " chest pieces, " << shields << " shields");
    CHECK(socketed == weapons + chests + shields);
    CHECK(socketed >= 170);
    CHECK(alchemy::SocketCount(d.Row("ITEM_CH_SWORD_01_A")) == 0);
    CHECK(alchemy::SocketCount(d.Row("ITEM_CH_M_CLOTHES_11_HA_A")) == 0); // a helm: none
    // The 14 stones in service: each its group's skills (MagicType 3), level 1 first; the maker and the eraser.
    int stones = 0;
    for (const auto& [code, row] : d.items.rows()) {
        if (!row.inService || !alchemy::IsSocketStone(row)) continue;
        ++stones;
        const auto skills = alchemy::StoneSkills(d.groups, row);
        CAPTURE(code);
        REQUIRE((skills.size() == 3 || skills.size() == 5));
        for (std::size_t i = 0; i < skills.size(); ++i) CHECK(skills[i]->optLevel == int(i) + 1);
        CHECK((row.params[1] == 1 || row.params[1] == 2));
        CHECK(row.itemClass == 5);
    }
    CHECK(stones == 14);
    CHECK(alchemy::IsSocketMaker(d.Row("ITEM_ETC_ARCHEMY_ETC_02")));
    CHECK(alchemy::IsSocketEraser(d.Row("ITEM_ETC_ARCHEMY_RONDO_03")));
    CHECK_FALSE(alchemy::IsSocketStone(d.Row("ITEM_ETC_ARCHEMY_MAGICSTONE_STR_01")));

    const assets::ItemRow& chest = d.Row("ITEM_CH_M_CLOTHES_11_BA_A_RARE");
    const assets::ItemRow& bow = d.Row("ITEM_CH_BOW_11_A_RARE");
    const assets::ItemRow& gold = d.Row("ITEM_ETC_SOCKET_STONE_GOLD_E");  // armour
    const assets::ItemRow& heal = d.Row("ITEM_ETC_SOCKET_STONE_HEAL_E");  // weapon
    const assets::ItemRow& sp = d.Row("ITEM_ETC_SOCKET_STONE_SP_E");      // armour
    game::ItemInstance item;
    // Making: the item's kind first, then the sockets (the maker is the caller's).
    CHECK(alchemy::CheckSocketMake(d.Row("ITEM_ETC_HP_POTION_01"), item, 0) == alchemy::code::kOnlyEquipment);
    CHECK(alchemy::CheckSocketMake(d.Row("ITEM_CH_SWORD_01_A"), item, 0) == alchemy::code::kSocketNotAllowed);
    CHECK(alchemy::CheckSocketMake(chest, item, 3) == alchemy::code::kSocketSlot);
    CHECK(alchemy::CheckSocketMake(chest, item, 1) == 0);
    alchemy::MakeSocket(item, 1);
    CHECK(alchemy::CheckSocketMake(chest, item, 1) == alchemy::code::kSocketMade);
    // Putting a stone in: the socket made and empty, the kind, the same stone once.
    CHECK(alchemy::CheckSocketInsert(chest, item, gold, 0) == alchemy::code::kSocketMissing);
    CHECK(alchemy::CheckSocketInsert(chest, item, d.Row("ITEM_ETC_HP_POTION_01"), 1) == alchemy::code::kRecipeMissing);
    CHECK(alchemy::CheckSocketInsert(chest, item, gold, 1) == 0);
    game::ItemInstance weapon;
    alchemy::MakeSocket(weapon, 0);
    CHECK(alchemy::CheckSocketInsert(bow, weapon, gold, 0) == alchemy::code::kSocketStoneKind); // an armour stone on a weapon
    CHECK(alchemy::CheckSocketInsert(bow, weapon, heal, 0) == 0);
    // The original's weapon check lets a chest pass (TID4 3..15 as written): ours keeps it.
    CHECK(alchemy::CheckSocketInsert(chest, item, heal, 1) == 0);
    alchemy::InsertStone(item, 1, 31207, 2, gold.refObjId);
    CHECK(item.sockets[1].Filled());
    CHECK(alchemy::CheckSocketInsert(chest, item, sp, 1) == alchemy::code::kSocketInUse);
    alchemy::MakeSocket(item, 2);
    CHECK(alchemy::CheckSocketInsert(chest, item, gold, 2) == alchemy::code::kSocketSameStone);
    CHECK(alchemy::CheckSocketInsert(chest, item, sp, 2) == 0);
    // The DMRETURN stone is kept out (ours: its 'bldr' never acts in the original).
    const assets::ItemRow& reflect = d.Row("ITEM_ETC_SOCKET_STONE_DMRETURN_E");
    CHECK(alchemy::SocketStoneDisabled(reflect));
    CHECK_FALSE(alchemy::SocketStoneDisabled(gold));
    CHECK(alchemy::CheckSocketInsert(chest, item, reflect, 2) == alchemy::code::kSocketStoneKind);
    // Erasing: a stone there.
    CHECK(alchemy::CheckSocketErase(chest, item, 2) == alchemy::code::kSocketEmpty);
    CHECK(alchemy::CheckSocketErase(chest, item, 0) == alchemy::code::kSocketEmpty);
    CHECK(alchemy::CheckSocketErase(chest, item, 1) == 0);
    alchemy::EraseStone(item, 1);
    CHECK(item.sockets[1].made);
    CHECK_FALSE(item.sockets[1].Filled());
    // A worn item's skills: its filled sockets'.
    alchemy::InsertStone(item, 2, 31204, 2, sp.refObjId);
    std::vector<std::uint32_t> skills;
    alchemy::AddSocketSkills(item, skills);
    CHECK(skills == std::vector<std::uint32_t>{31204});
    // Every made socket full: 0x6C09 before 0x6C0B.
    alchemy::MakeSocket(item, 0);
    CHECK(alchemy::CheckSocketMake(chest, item, 0) == alchemy::code::kSocketsFull);
}

TEST_CASE("Sockets: an insertion's level is the lowest of three picks among the group's rows") {
    REQUIRE_GAME_DATA(d);
    std::mt19937 random(5);
    const auto shares = [&](const char* stone) {
        const auto rows = alchemy::StoneSkills(d.groups, d.Row(stone));
        std::vector<double> counts(rows.size());
        constexpr int kRolls = 60000;
        for (int i = 0; i < kRolls; ++i) {
            const assets::MagicOptGroupRow* g = alchemy::RollSocketLevel(rows, random);
            REQUIRE(g != nullptr);
            counts[std::size_t(g->optLevel - 1)] += 100.0 / kRolls;
        }
        return counts;
    };
    // Three levels: 70.4 / 25.9 / 3.7 %; five: 48.8 / 29.6 / 15.2 / 5.6 / 0.8 %.
    const std::vector<double> three = shares("ITEM_ETC_SOCKET_STONE_GOLD_E"), five = shares("ITEM_ETC_SOCKET_STONE_HEAL_E");
    REQUIRE(three.size() == 3);
    REQUIRE(five.size() == 5);
    const double expected3[] = {70.4, 25.9, 3.7}, expected5[] = {48.8, 29.6, 15.2, 5.6, 0.8};
    for (int i = 0; i < 3; ++i) CHECK(std::abs(three[std::size_t(i)] - expected3[i]) < 1.0);
    for (int i = 0; i < 5; ++i) CHECK(std::abs(five[std::size_t(i)] - expected5[i]) < 1.0);
    MESSAGE("three levels: " << three[0] << " / " << three[1] << " / " << three[2] << " %; five: " << five[0] << " / " << five[1] << " / " << five[2]
                             << " / " << five[3] << " / " << five[4] << " %");
}

TEST_CASE("Sockets on the game server: made with the bag's maker, a stone in on the 4th tick (a cancel or a death stops it), erased with Rondo 03") {
    REQUIRE_GAME_DATA(d);
    AlchemyBench bench(d);
    server::Alchemy alchemy(bench, 31);
    auto now = server::Alchemy::Clock::now();
    const auto tick = [&] {
        now += std::chrono::seconds(1);
        alchemy.Tick(now);
    };
    namespace proto = game::proto;
    bench.Put(13, "ITEM_CH_M_CLOTHES_11_BA_A_RARE");
    bench.Put(14, "ITEM_ETC_SOCKET_STONE_GOLD_E", 2);
    bench.Put(15, "ITEM_CH_BOW_11_A_RARE");
    bench.Put(16, "ITEM_ETC_SOCKET_STONE_HEAL_E");
    const auto last = [&] {
        REQUIRE_FALSE(bench.results.empty());
        const proto::AlchemyResult r = bench.results.back();
        bench.results.clear();
        return r;
    };
    // No maker: 0x6C08; a potion: the item's own code first.
    bench.Ask(alchemy, proto::SocketMake{13, 0});
    CHECK(last().code == alchemy::code::kSocketNoMaterial);
    bench.Put(17, "ITEM_ETC_HP_POTION_01");
    bench.Ask(alchemy, proto::SocketMake{17, 0});
    CHECK(last().code == alchemy::code::kOnlyEquipment);
    // With two makers: socket 0 made at once, one maker used; twice 0x6C0B.
    bench.Put(20, "ITEM_ETC_ARCHEMY_ETC_02", 2);
    bench.Ask(alchemy, proto::SocketMake{13, 0});
    proto::AlchemyResult r = last();
    CHECK(r.outcome == proto::AlchemyOutcome::Success);
    CHECK(r.operation == proto::AlchemyOperation::SocketMake);
    CHECK(r.socket == 0);
    CHECK(bench.bag.At(13)->sockets[0].made);
    CHECK(bench.bag.At(20)->count == 1);
    bench.Ask(alchemy, proto::SocketMake{13, 0});
    CHECK(last().code == alchemy::code::kSocketMade);
    CHECK(bench.bag.At(20)->count == 1);
    // A stone into the unmade socket 1: refused at once (0x6C0D), nothing used.
    bench.Ask(alchemy, proto::SocketInsert{13, 14, 1});
    CHECK(last().code == alchemy::code::kSocketMissing);
    CHECK_FALSE(alchemy.Busy(7));
    // Into socket 0: no answer until the 4th tick; a cancel before it uses nothing.
    bench.Ask(alchemy, proto::SocketInsert{13, 14, 0});
    CHECK(bench.results.empty());
    CHECK(alchemy.Busy(7));
    tick();
    tick();
    bench.Ask(alchemy, proto::AlchemyCancel{});
    r = last();
    CHECK(r.outcome == proto::AlchemyOutcome::Cancelled);
    CHECK(r.operation == proto::AlchemyOperation::SocketInsert);
    CHECK(bench.bag.At(14)->count == 2);
    CHECK_FALSE(bench.bag.At(13)->sockets[0].Filled());
    // A death at a tick stops it too.
    bench.Ask(alchemy, proto::SocketInsert{13, 14, 0});
    tick();
    alchemy.End(7);
    CHECK(last().outcome == proto::AlchemyOutcome::Cancelled);
    CHECK(bench.bag.At(14)->count == 2);
    // Done: three ticks nothing, the 4th the stone's skill at a level of its group, one stone used.
    bench.Ask(alchemy, proto::SocketInsert{13, 14, 0});
    for (int i = 0; i < 3; ++i) {
        tick();
        CHECK(bench.results.empty());
    }
    tick();
    r = last();
    CHECK(r.outcome == proto::AlchemyOutcome::Success);
    CHECK(r.stone == d.Row("ITEM_ETC_SOCKET_STONE_GOLD_E").refObjId);
    CHECK((r.value >= 1 && r.value <= 3));
    CHECK(r.option == 31205 + r.value);
    const game::ItemSocket s = bench.bag.At(13)->sockets[0];
    CHECK(s.skill == r.option);
    CHECK(s.level == r.value);
    CHECK(s.stone == r.stone);
    CHECK(bench.bag.At(14)->count == 1);
    // The same stone again into another socket: 0x6C0C.
    bench.Ask(alchemy, proto::SocketMake{13, 2});
    CHECK(last().outcome == proto::AlchemyOutcome::Success);
    CHECK(bench.bag.At(20) == nullptr); // the second maker used
    bench.Ask(alchemy, proto::SocketInsert{13, 14, 2});
    CHECK(last().code == alchemy::code::kSocketSameStone);
    // A weapon stone and its weapon's socket: none made yet (0x6C0D); a stone moved away before the 4th tick: 0x5407.
    bench.Ask(alchemy, proto::SocketInsert{15, 16, 0});
    CHECK(last().code == alchemy::code::kSocketMissing);
    // Erasing: no Rondo 03 0x6C08; the empty socket 2 0x6C11; socket 0's stone gone, the socket stays, a Rondo used.
    bench.Ask(alchemy, proto::SocketErase{13, 0});
    CHECK(last().code == alchemy::code::kSocketNoMaterial);
    bench.Put(21, "ITEM_ETC_ARCHEMY_RONDO_03", 3);
    bench.Ask(alchemy, proto::SocketErase{13, 2});
    CHECK(last().code == alchemy::code::kSocketEmpty);
    CHECK(bench.bag.At(21)->count == 3);
    bench.Ask(alchemy, proto::SocketErase{13, 0});
    r = last();
    CHECK(r.outcome == proto::AlchemyOutcome::Success);
    CHECK(r.operation == proto::AlchemyOperation::SocketErase);
    CHECK(r.stone == d.Row("ITEM_ETC_SOCKET_STONE_GOLD_E").refObjId);
    CHECK(bench.bag.At(13)->sockets[0].made);
    CHECK_FALSE(bench.bag.At(13)->sockets[0].Filled());
    CHECK(bench.bag.At(21)->count == 2);
}

TEST_CASE("Sockets: the item record carries them and the bag reads them back") {
    game::ItemInstance item;
    item.refId = 1234;
    alchemy::MakeSocket(item, 0);
    alchemy::MakeSocket(item, 2);
    alchemy::InsertStone(item, 2, 31241, 2, 26092);
    game::Inventory bag;
    bag.size = 45;
    bag.places[13] = item;
    const game::proto::ItemRecord record = bag.Record(13);
    REQUIRE(record.sockets.size() == 2);
    CHECK(record.sockets[0].slot == 0);
    CHECK(record.sockets[0].skill == 0);
    CHECK(record.sockets[1].slot == 2);
    CHECK(record.sockets[1].skill == 31241);
    game::proto::InventoryList list{45, 0, {record}}, back;
    REQUIRE(game::proto::Decode(game::proto::Encode(list), back));
    REQUIRE(back.items.size() == 1);
    const game::ItemInstance again = game::Inventory::FromRecord(back.items[0]);
    CHECK(again.sockets == item.sockets);
}

// --- The store: a socketed item saved and loaded (the database's _BindingOptionWithItem, type 1) ---------------------------

#include "ServerTestSupport.h"

using sr::test::TestDatabase;

TEST_CASE("Sockets in the database: a socketed item's type-1 binding rows written with it, read back, rewritten, freed with it") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    REQUIRE(server::CreateAccount(t.Db(), "socketacct", "secret1", error) == server::CreateAccountResult::Created);
    bool ok = false;
    const std::int64_t account = server::FindAccount(t.Db(), "socketacct", ok)->id;
    server::NewCharacter n;
    n.name = "Socketer";
    n.model = 1907;
    n.height = 34;
    n.strength = n.intelligence = 20;
    n.hp = n.mp = 200;
    REQUIRE(server::CreateCharacter(t.Db(), account, 1, n, 4, error) == server::CreateCharacterResult::Created);
    const std::vector<server::CharacterRow> list = server::ListCharacters(t.Db(), account, 1, 0, ok);
    REQUIRE(list.size() == 1);
    const std::int64_t character = list[0].id;
    server::StoredItem chest;
    chest.refId = 4321;
    chest.durability = 50;
    chest.sockets[0] = {true, 31207, 2, 26083};
    chest.sockets[1] = {true, 0, 0, 0};
    REQUIRE(server::GiveItems(t.Db(), 1, "Socketer", {chest}, 0, error) == server::GiveResult::Done);
    const auto load = [&] {
        const std::optional<server::CharacterRow> row = server::LoadCharacter(t.Db(), account, 1, character, ok);
        REQUIRE(row);
        for (const server::StoredItem& i : row->items) {
            if (i.refId == 4321) return i;
        }
        FAIL("the chest is not there");
        return server::StoredItem{};
    };
    server::StoredItem loaded = load();
    CHECK(loaded.sockets == chest.sockets);
    // The rows as the original's: an empty socket all 0 but its slot; a filled one its skill, level and stone.
    server::QueryResult rows = t.Db().Query("SELECT nslot, noptid, noptlvl, noptvalue, nparam1, nparam2 FROM _bindingoptionwithitem "
                                            "WHERE nitemdbid = $1 AND bopttype = 1 ORDER BY nslot",
                                            {loaded.id});
    REQUIRE(rows.Ok());
    REQUIRE(rows.Rows() == 2);
    CHECK(rows.Int(0, 0) == 0);
    CHECK(rows.Int(0, 1) == 31207);
    CHECK(rows.Int(0, 2) == 2);
    CHECK(rows.Int(0, 3) == 0);
    CHECK(rows.Int(0, 4) == 26083);
    CHECK(rows.Int(1, 0) == 1);
    CHECK(rows.Int(1, 1) == 0);
    CHECK(rows.Int(1, 4) == 0);
    // Its stone erased and a third socket made: the rows rewritten.
    loaded.sockets[0] = {true, 0, 0, 0};
    loaded.sockets[2] = {true, 31241, 3, 26092};
    loaded.counted = false;
    REQUIRE(server::SaveItems(t.Db(), character, {loaded}, {}, {}));
    const server::StoredItem again = load();
    CHECK(again.sockets == loaded.sockets);
    CHECK(t.Db().Query("SELECT count(*) FROM _bindingoptionwithitem WHERE nitemdbid = $1 AND bopttype = 1", {loaded.id}).Int(0, 0) == 3);
    // Gone: its rows with it.
    REQUIRE(server::SaveItems(t.Db(), character, {}, {loaded.slot}, {loaded.id}));
    CHECK(t.Db().Query("SELECT count(*) FROM _bindingoptionwithitem WHERE nitemdbid = $1", {loaded.id}).Int(0, 0) == 0);
}

TEST_CASE("Alchemy on accessories: the elixirs, the magic stones whose options they take, the attribute stones' fields") {
    REQUIRE_GAME_DATA(d);
    // The accessories' blues by _RefMagicOptAssign (TID3 5 and 12, TID4 1..3, both countries: the same nine).
    const std::set<std::string> assigned = {"MATTR_STR",           "MATTR_INT",         "MATTR_RESIST_FROSTBITE", "MATTR_RESIST_BURN", "MATTR_RESIST_ESHOCK",
                                            "MATTR_RESIST_POISON", "MATTR_RESIST_ZOMBIE", "MATTR_ATHANASIA",      "MATTR_LUCK"};
    for (const char* code : {"ITEM_CH_RING_01_A", "ITEM_CH_EARRING_01_A", "ITEM_CH_NECKLACE_01_A", "ITEM_EU_RING_01_A", "ITEM_EU_EARRING_01_A",
                             "ITEM_EU_NECKLACE_01_A"}) {
        const assets::ItemRow& accessory = d.Row(code);
        game::ItemInstance item;
        item.refId = accessory.refObjId;
        // Every degree-1 magic stone: it fits when its option's level-1 row's groups take an accessory (the stone check's
        // rule, 3.2; the assign table is the random blues' list, not the stones'): the nine assigned, and Astral and APE
        // too (their groups name the accessory). Astral asks Immortal above it first (0x5424).
        std::set<std::string> fit, grouped;
        for (const assets::MagicOptionRow& o : d.options) {
            if (o.degree == 1 && std::find(o.availGroups.begin(), o.availGroups.end(), "accessory") != o.availGroups.end()) grouped.insert(o.optionName);
        }
        for (const auto& [name, stone] : d.items.rows()) {
            if (name.rfind("ITEM_ETC_ARCHEMY_MAGICSTONE_", 0) != 0 || stone.params[0] != 1) continue;
            const std::string option = stone.paramTexts[0];
            const std::uint16_t check = alchemy::CheckMagicStone(accessory, item, stone, d.options);
            if (check == 0) fit.insert(option);
            else if (option == "MATTR_ASTRAL") CHECK(check == alchemy::code::kAstral);
            else CHECK_MESSAGE(check == alchemy::code::kRecipeMismatch, name, " on ", accessory.codename, ": ", check);
            if (check == alchemy::code::kRecipeMismatch) CHECK_MESSAGE(!grouped.contains(option), option, " is refused on ", accessory.codename, " though its groups take it");
        }
        for (const std::string& o : fit) CHECK_MESSAGE(grouped.contains(o), o, " fits ", accessory.codename, " but its groups do not take an accessory");
        for (const std::string& o : assigned) CHECK_MESSAGE(fit.contains(o), o, " is assigned to ", accessory.codename, " but no stone of it fits");
        CHECK(fit.contains("MATTR_APE"));
        // The attribute stones: the absorptions (PAR field 0, MAR field 1), nothing else.
        CHECK(alchemy::WhiteFields(accessory) == std::vector<std::string_view>{"PAR", "MAR"});
        CHECK(alchemy::CheckAttrStone(accessory, d.Row("ITEM_ETC_ARCHEMY_ATTRSTONE_PAR_01")) == 0);
        CHECK(alchemy::CheckAttrStone(accessory, d.Row("ITEM_ETC_ARCHEMY_ATTRSTONE_MAR_01")) == 0);
        CHECK(alchemy::CheckAttrStone(accessory, d.Row("ITEM_ETC_ARCHEMY_ATTRSTONE_PA_01")) == alchemy::code::kStoneMismatch);
        // The elixirs: accessory A fits China's (TID3 5) alone, B both; the others none.
        const bool china = accessory.typeId[2] == 5;
        CHECK(alchemy::ElixirFits(d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_ACCESSARY_A"), accessory) == china);
        CHECK(alchemy::ElixirFits(d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_ACCESSARY_B"), accessory));
        CHECK_FALSE(alchemy::ElixirFits(d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B"), accessory));
        CHECK_FALSE(alchemy::ElixirFits(d.Row("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_ARMOR_B"), accessory));
    }
}

TEST_CASE("Avatars at the smith: the lists, the capacity, the values, the codes in the original's order") {
    REQUIRE_GAME_DATA(d);
    const assets::ItemRow* hat = d.First(13, 1, 2);
    const assets::ItemRow* bareHat = d.First(13, 1, 0);
    const assets::ItemRow* dress = d.First(13, 2, 4);
    const assets::ItemRow* attachment = d.First(13, 3, 1);
    const assets::ItemRow* flag = d.First(13, 4);
    const assets::ItemRow* spirit = d.First(14, 1);
    REQUIRE(hat != nullptr);
    REQUIRE(bareHat != nullptr);
    REQUIRE(dress != nullptr);
    REQUIRE(attachment != nullptr);
    REQUIRE(flag != nullptr);
    REQUIRE(spirit != nullptr);

    // The lists, as the 1.188 window shows them (magicoptionassign.txt's rows 13/1..13/3); none for a flag or a spirit.
    CHECK(alchemy::AvatarGrantList(d.assign, *hat) ==
          std::vector<std::string>{"MATTR_AVATAR_STR", "MATTR_AVATAR_INT", "MATTR_AVATAR_ER", "MATTR_AVATAR_MP", "MATTR_AVATAR_DARA", "MATTR_AVATAR_MPRG"});
    CHECK(alchemy::AvatarGrantList(d.assign, *attachment) == std::vector<std::string>{"MATTR_AVATAR_HR", "MATTR_AVATAR_HPRG"});
    const std::vector<std::string> dressList = alchemy::AvatarGrantList(d.assign, *dress);
    CHECK(dressList.size() == 20);
    CHECK(dressList[1] == "MATTR_AVATAR_STR_2");
    CHECK(alchemy::AvatarGrantList(d.assign, *flag).empty());
    CHECK(alchemy::AvatarGrantList(d.assign, *spirit).empty());

    // The checks in order: not an avatar the smith takes (8: a weapon, a flag, a Devil's Spirit), an option of no list
    // (0xA; the case of the name ignored), full (2), more than it may hold (6).
    game::ItemInstance item;
    item.refId = hat->refObjId;
    CHECK(alchemy::CheckAvatarGrant(d.Row("ITEM_CH_SWORD_01_A"), item, "MATTR_AVATAR_STR", d.options, d.assign) == alchemy::code::kGrantNotAvatar);
    CHECK(alchemy::CheckAvatarGrant(*flag, item, "MATTR_STR_AVATAR", d.options, d.assign) == alchemy::code::kGrantNotAvatar);
    CHECK(alchemy::CheckAvatarGrant(*spirit, item, "MATTR_NASRUN_HPNA", d.options, d.assign) == alchemy::code::kGrantNotAvatar);
    CHECK(alchemy::CheckAvatarGrant(*hat, item, "MATTR_STR", d.options, d.assign) == alchemy::code::kGrantWrong);       // the armour's STR: no avatar's
    CHECK(alchemy::CheckAvatarGrant(*hat, item, "MATTR_AVATAR_HR", d.options, d.assign) == alchemy::code::kGrantWrong); // the dress's, not the hat's
    CHECK(alchemy::CheckAvatarGrant(*hat, item, "mattr_avatar_str", d.options, d.assign) == 0);
    CHECK(alchemy::CheckAvatarGrant(*bareHat, item, "MATTR_AVATAR_STR", d.options, d.assign) == alchemy::code::kGrantFull);

    // Granted: each its fixed value (Param3 = Param4 on every avatar row but the flag's), the row of the avatar's degree.
    std::mt19937 random(5);
    const game::ItemInstance::Option str = alchemy::GrantAvatarOption(*hat, item, "mattr_avatar_str", d.options, random);
    CHECK(str.id == d.Option("MATTR_AVATAR_STR").paramId);
    CHECK(str.value == 1);
    const game::ItemInstance::Option mp = alchemy::GrantAvatarOption(*hat, item, "MATTR_AVATAR_MP", d.options, random);
    CHECK(mp.value == 150);
    REQUIRE(item.options.size() == 2);
    // Full now: a new one refused (2), a held one written again in its place (no third option).
    CHECK(alchemy::CheckAvatarGrant(*hat, item, "MATTR_AVATAR_ER", d.options, d.assign) == alchemy::code::kGrantFull);
    CHECK(alchemy::CheckAvatarGrant(*hat, item, "MATTR_AVATAR_STR", d.options, d.assign) == 0);
    alchemy::GrantAvatarOption(*hat, item, "MATTR_AVATAR_STR", d.options, random);
    CHECK(item.options.size() == 2);
    CHECK(item.options[0] == str);
    // Holding more than it may (a hat of none with one: 6).
    game::ItemInstance over;
    over.refId = bareHat->refObjId;
    over.options.push_back(str);
    CHECK(alchemy::CheckAvatarGrant(*bareHat, over, "MATTR_AVATAR_STR", d.options, d.assign) == alchemy::code::kGrantMax);

    // A dress: STR, STR_2, STR_3 and STR_4 are four options (found by their row's ID), each 1.
    game::ItemInstance robe;
    robe.refId = dress->refObjId;
    for (const char* name : {"MATTR_AVATAR_STR", "MATTR_AVATAR_STR_2", "MATTR_AVATAR_STR_3", "MATTR_AVATAR_STR_4"}) {
        REQUIRE(alchemy::CheckAvatarGrant(*dress, robe, name, d.options, d.assign) == 0);
        CHECK(alchemy::GrantAvatarOption(*dress, robe, name, d.options, random).value == 1);
    }
    CHECK(robe.options.size() == 4);
    CHECK(alchemy::CheckAvatarGrant(*dress, robe, "MATTR_AVATAR_HP", d.options, d.assign) == alchemy::code::kGrantFull);

    // The range kind: the flag's STR (Param3 1, Param4 9) 1..8, 9 once in 32,768 picks; a fixed row its Param3.
    const assets::MagicOptionRow& strv = d.Option("MATTR_STR_AVATAR");
    CHECK(alchemy::IsRangeCode(strv.attribute));
    std::map<std::uint32_t, int> seen;
    for (int i = 0; i < 20000; ++i) ++seen[alchemy::RangeValue(strv, random)];
    CHECK(seen.begin()->first == 1);
    CHECK(std::prev(seen.end())->first >= 8);
    CHECK(std::prev(seen.end())->first <= 9);
    CHECK(alchemy::RangeValue(d.Option("MATTR_AVATAR_HP"), random) == 150);
}

TEST_CASE("Avatars at the smith on the game server: granted at once and free, the answer's option and value; refused away from the smith, in a wrong place, a spent rental") {
    REQUIRE_GAME_DATA(d);
    AlchemyBench bench(d);
    server::Alchemy alchemy(bench, 23);
    const assets::ItemRow* dress = d.First(13, 2, 4);
    REQUIRE(dress != nullptr);
    bench.Put(13, dress->codename.c_str());
    bench.Put(14, "ITEM_CH_SWORD_01_A");
    using game::proto::AvatarGrant;
    const auto last = [&] {
        REQUIRE_FALSE(bench.results.empty());
        const game::proto::AlchemyResult r = bench.results.back();
        bench.results.clear();
        CHECK(r.operation == game::proto::AlchemyOperation::AvatarGrant);
        return r;
    };

    bench.Ask(alchemy, AvatarGrant{99, 13, "MATTR_AVATAR_HP"});
    game::proto::AlchemyResult r = last();
    CHECK(r.outcome == game::proto::AlchemyOutcome::Success);
    CHECK(r.option == d.Option("MATTR_AVATAR_HP").paramId);
    CHECK(r.value == 150);
    REQUIRE(bench.bag.At(13)->options.size() == 1);
    CHECK(bench.bag.At(13)->options[0].value == 150);
    CHECK_FALSE(alchemy.Busy(7));

    // The refusals: no smith in reach (ours), outside the bag (9), an empty place (3), not an avatar (8), a wrong option
    // (0xA), a rental past its end (3).
    bench.atSmith = false;
    bench.Ask(alchemy, AvatarGrant{99, 13, "MATTR_AVATAR_STR"});
    CHECK(last().outcome == game::proto::AlchemyOutcome::Refused);
    bench.atSmith = true;
    bench.Ask(alchemy, AvatarGrant{99, 5, "MATTR_AVATAR_STR"});
    CHECK(last().code == alchemy::code::kGrantPlace);
    bench.Ask(alchemy, AvatarGrant{99, 40, "MATTR_AVATAR_STR"});
    CHECK(last().code == alchemy::code::kGrantNoItem);
    bench.Ask(alchemy, AvatarGrant{99, 14, "MATTR_AVATAR_STR"});
    CHECK(last().code == alchemy::code::kGrantNotAvatar);
    bench.Ask(alchemy, AvatarGrant{99, 13, "MATTR_LUCK"});
    CHECK(last().code == alchemy::code::kGrantWrong);
    bench.bag.places[13].rentUntil = 1000;
    bench.Ask(alchemy, AvatarGrant{99, 13, "MATTR_AVATAR_STR"});
    CHECK(last().code == alchemy::code::kGrantNoItem);
    CHECK(bench.bag.At(13)->options.size() == 1);
}
