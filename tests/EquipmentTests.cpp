// Equipment on the server, 1:1 with the original: the wear check's order, the requirement
// classes, a worn item's blues on the parameter keeper.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/game/Combat.h"
#include "sr/game/Inventory.h"
#include "sr/game/Items.h"
#include "sr/game/Stats.h"

using namespace sr;
using game::EquipCheck;
namespace place = game::slot;

namespace {

assets::ItemRow Row(int tid3, int tid4, int country, int sex, std::array<int, 4> types, std::array<int, 4> levels) {
    assets::ItemRow r;
    r.refObjId = 1;
    r.codename = "ITEM_TEST";
    r.typeId = {3, 1, tid3, tid4};
    r.country = country;
    r.sex = sex;
    r.requirementTypes = types;
    r.requirementLevels = levels;
    r.stats.assign(117 - assets::ItemRow::kFirstStatColumn + 1, 0.0f);
    return r;
}

game::Wearer Who(int level, int country, bool male) {
    game::Wearer w;
    w.level = level;
    w.country = country;
    w.male = male;
    return w;
}

} // namespace

TEST_CASE("the wear check in the original's order: sex, broken, requirements, country, then the place") {
    const game::ItemInstance item;
    // A Chinese woman's chest at level 20.
    const assets::ItemRow chest = Row(1, 3, 0, 0, {1, -1, -1, -1}, {20, 0, 0, 0});
    // Everything wrong at once: the sex answers first (0x1816).
    CHECK(game::CanEquip(chest, place::kHead, Who(1, 1, true), item) == EquipCheck::Sex);
    // The right sex, the rest wrong: the level (0x186C) before the country (0x182F) and the place (0x180E).
    CHECK(game::CanEquip(chest, place::kHead, Who(1, 1, false), item) == EquipCheck::Level);
    CHECK(game::CanEquip(chest, place::kHead, Who(20, 1, false), item) == EquipCheck::Race);
    // The place last.
    CHECK(game::CanEquip(chest, place::kHead, Who(20, 0, false), item) == EquipCheck::WrongSlot);
    CHECK(game::CanEquip(chest, place::kChest, Who(20, 0, false), item) == EquipCheck::Ok);
    // Beyond the worn places at once (0x1806).
    CHECK(game::CanEquip(chest, 20, Who(1, 1, true), item) == EquipCheck::WrongSlot);
    // A ring in either ring place.
    const assets::ItemRow ring = Row(5, 3, 3, 2, {1, -1, -1, -1}, {1, 0, 0, 0});
    CHECK(game::CanEquip(ring, place::kRingLeft, Who(1, 0, true), item) == EquipCheck::Ok);
    CHECK(game::CanEquip(ring, place::kRingRight, Who(1, 1, false), item) == EquipCheck::Ok);
    CHECK(game::CanEquip(ring, place::kEarring, Who(1, 1, false), item) == EquipCheck::WrongSlot);
    // An avatar: the sex, the requirements, the country.
    assets::ItemRow hat = Row(13, 1, 1, 1, {1, -1, -1, -1}, {30, 0, 0, 0});
    CHECK(game::CanWearAvatar(hat, Who(1, 0, false)) == EquipCheck::Sex);
    CHECK(game::CanWearAvatar(hat, Who(1, 0, true)) == EquipCheck::Level);
    CHECK(game::CanWearAvatar(hat, Who(30, 0, true)) == EquipCheck::Race);
    CHECK(game::CanWearAvatar(hat, Who(30, 1, true)) == EquipCheck::Ok);
}

TEST_CASE("the requirement classes: one pair a class, every class") {
    const game::ItemInstance item;
    // European heavy armour: level 10 and the warrior's mastery (0x201) at 5.
    const assets::ItemRow heavy = Row(11, 3, 1, 2, {1, 0x201, -1, -1}, {10, 5, 0, 0});
    game::Wearer who = Who(10, 1, true);
    // Its masteries not known (the client's view): the class not checked.
    CHECK(game::CanEquip(heavy, place::kChest, who, item) == EquipCheck::Ok);
    who.masteriesKnown = true;
    CHECK(game::CanEquip(heavy, place::kChest, who, item) == EquipCheck::Mastery);
    who.masteries[0x204] = 50; // another mastery does not count
    CHECK(game::CanEquip(heavy, place::kChest, who, item) == EquipCheck::Mastery);
    who.masteries[0x201] = 4;
    CHECK(game::CanEquip(heavy, place::kChest, who, item) == EquipCheck::Mastery);
    who.masteries[0x201] = 5;
    CHECK(game::CanEquip(heavy, place::kChest, who, item) == EquipCheck::Ok);
    // The level's class before the mastery's.
    who.level = 9;
    CHECK(game::CanEquip(heavy, place::kChest, who, item) == EquipCheck::Level);
    // Light armour: any one of its three masteries is enough.
    const assets::ItemRow light = Row(10, 3, 1, 2, {1, 0x201, 0x203, 0x206}, {1, 3, 3, 3});
    game::Wearer bard = Who(1, 1, true);
    bard.masteriesKnown = true;
    bard.masteries[0x206] = 3;
    CHECK(game::CanEquip(light, place::kChest, bard, item) == EquipCheck::Ok);
    bard.masteries[0x206] = 2;
    CHECK(game::CanEquip(light, place::kChest, bard, item) == EquipCheck::Mastery);
    // The job levels (2..4) are not this check's.
    const assets::ItemRow suit = Row(7, 1, 3, 2, {2, -1, -1, -1}, {5, 0, 0, 0});
    CHECK(game::RequirementsMet(suit, Who(1, 0, true)) == EquipCheck::Ok);
    // An unknown type refuses (the original logs it).
    const assets::ItemRow odd = Row(1, 3, 3, 2, {0x150, -1, -1, -1}, {1, 0, 0, 0});
    CHECK(game::RequirementsMet(odd, Who(1, 0, true)) == EquipCheck::WrongSlot);
}

TEST_CASE("a worn item's blues on the keeper by their code") {
    const auto code = [](const char* s) {
        std::uint32_t v = 0;
        for (; *s != 0; ++s) v = (v << 8) | std::uint8_t(*s);
        return v;
    };
    const auto one = [](const std::vector<game::StatChange>& c, std::uint16_t stat, std::uint8_t kind, float value) {
        REQUIRE(c.size() == 1);
        CHECK(c[0].stat == stat);
        CHECK(c[0].kind == kind);
        CHECK(c[0].value == value);
    };
    one(game::BlueChanges(code("str"), 7), game::stat::kStr, 0, 7);
    one(game::BlueChanges(code("int"), 9), game::stat::kInt, 0, 9);
    one(game::BlueChanges(code("hp"), 1300), game::stat::kMaxHp, 0, 1300);
    one(game::BlueChanges(code("mp"), 1300), game::stat::kMaxMp, 0, 1300);
    one(game::BlueChanges(code("cri"), 3), game::stat::kCritical, 0, 3);
    one(game::BlueChanges(code("hra"), 20), game::stat::kHitRate, 1, 20);
    one(game::BlueChanges(code("evbl"), 10), game::stat::kBlockIgnore, 0, 10);
    const auto taken = game::BlueChanges(code("dara"), 5);
    REQUIRE(taken.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(taken[i].stat == game::stat::kDamageTaken + i);
        CHECK(taken[i].kind == 2);
        CHECK(taken[i].value == -5.0f);
    }
    // Not the keeper's: the resists (the state roll reads the items), the item's own hit rate, durability, Luck.
    CHECK(game::BlueChanges(code("fz"), 10).empty());
    CHECK(game::BlueChanges(code("hr"), 10).empty());
    CHECK(game::BlueChanges(code("dura"), 10).empty());
    CHECK(game::BlueChanges(0, 10).empty());

    // An earring's STR +6 through the sheet: STR, and the health made from it.
    game::StatSheet sheet;
    sheet.Add(0x17E00009, game::BlueChanges(code("str"), 6));
    const game::CombatStats plain = game::PlayerCombatStats(50, 100, 80, {}, nullptr);
    const game::CombatStats worn = game::PlayerCombatStats(50, 100, 80, {}, nullptr, sheet);
    CHECK(worn.strength == doctest::Approx(106.0));
    CHECK(worn.maxHp == game::PlayerMaxHp(50, 106));
    CHECK(worn.maxHp > plain.maxHp);
}

TEST_CASE("European armour asks its masteries; accessories the level alone (game files)") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE(assets::MountGameArchives(fs, dir, error));
    assets::ItemTable items;
    REQUIRE(items.Load(fs, error));
    int armour = 0, accessories = 0;
    for (const auto& [code, row] : items.rows()) {
        if (row.typeId[0] != 3 || row.typeId[1] != 1) continue;
        const int tid3 = row.typeId[2];
        bool mastery = false;
        for (const int t : row.requirementTypes) mastery = mastery || (t >= 0x201 && t <= 0x206);
        if ((tid3 == 9 || tid3 == 10 || tid3 == 11) && row.country == 1) {
            ++armour;
            CHECK_MESSAGE(mastery, row.codename);
            // Heavy armour: the warrior's mastery; a wizard alone may not wear it.
            if (tid3 == 11 && mastery) {
                game::Wearer wizard = Who(255, 1, row.sex != 0);
                wizard.masteriesKnown = true;
                wizard.masteries[0x202] = 110;
                const int at = game::EquipPlaceFor(row, false, false);
                game::ItemInstance whole;
                whole.durability = 100; // not broken (checked before the requirements)
                CHECK_MESSAGE(game::CanEquip(row, at, wizard, whole) == EquipCheck::Mastery, row.codename);
            }
        }
        if (tid3 == 5 || tid3 == 12) {
            ++accessories;
            CHECK_MESSAGE(!mastery, row.codename);
        }
    }
    CHECK(armour > 1000);
    CHECK(accessories > 100);
}

TEST_CASE("an NPC's repair price: a point float32(CostRepair / most), the gold truncated") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE(assets::MountGameArchives(fs, dir, error));
    assets::ItemTable items;
    REQUIRE(items.Load(fs, error));
    const assets::ItemRow* sword = items.Find(std::string_view("ITEM_CH_SWORD_01_A"));
    REQUIRE(sword != nullptr);
    // Its most 70, one point left: 69 points at float32(CostRepair / 70) each, 195 gold (an integer point gave 138).
    const game::RepairPrice price = game::PriceRepair(*sword, 1, 70, 1'000'000, false);
    CHECK(price.points == 69);
    CHECK(price.gold == std::uint64_t(std::trunc(69.0 * double(std::max(1.0f, float(sword->costRepair) / 70.0f)))));
    CHECK(price.gold == 195);
    // A broken one counts from 1 (its first point free) and pays its revival besides.
    const game::RepairPrice broken = game::PriceRepair(*sword, 0, 70, 1'000'000, false);
    CHECK(broken.revive);
    CHECK(broken.points == 70);
    CHECK(broken.gold == 195 + sword->costRevive);
}

TEST_CASE("a weapon's whites as the server keeps them: float32, the most durability truncated") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE(assets::MountGameArchives(fs, dir, error));
    assets::ItemTable items;
    REQUIRE(items.Load(fs, error));
    const assets::ItemRow* sword = items.Find(std::string_view("ITEM_CH_SWORD_05_A"));
    REQUIRE(sword != nullptr);
    // The original's white: float32(low + float32((high - low) x float32(slot / 31) + float32(per plus x plus))).
    const auto original = [&](int lowCol, int highCol, int incCol, int slot, int plus) {
        const double low = sword->Stat(lowCol), high = sword->Stat(highCol);
        const double v = double(float(double(slot) / 31.0));
        const double plusPart = incCol > 0 ? double(float(double(sword->Stat(incCol)) * double(float(plus)))) : 0.0;
        return double(float(low + double(float((high - low) * v + plusPart))));
    };
    int fractional = 0;
    for (int slot = 0; slot <= 31; slot += 3) {
        for (const int plus : {0, 3, 7}) {
            game::ItemInstance item;
            item.refId = sword->refObjId;
            item.plus = std::uint8_t(plus);
            for (int f = 0; f < 7; ++f) item.variance |= std::uint64_t(slot) << (5 * f);
            const game::ItemStats server = game::ComputeStats(*sword, item, nullptr, 0, true);
            const game::ItemStats shown = game::ComputeStats(*sword, item, nullptr);
            CHECK(server.phyAttack.low == original(95, 96, 99, slot, plus));
            CHECK(server.phyAttack.high == original(97, 98, 99, slot, plus));
            CHECK(server.magAttack.low == original(100, 101, 104, slot, plus));
            CHECK(server.attackRating.low == original(113, 114, 115, slot, plus));
            CHECK(server.critical.low == original(116, 117, 0, slot, plus));
            fractional += server.phyAttack.low != std::trunc(server.phyAttack.low);
            // The tooltip still rounds (the 1.188 client's).
            CHECK(shown.phyAttack.low == std::round(shown.phyAttack.low));
            // The most durability: truncated on the server, never above the tooltip's rounding.
            const double most = std::trunc(double(sword->Stat(63)) + double(float((double(sword->Stat(64)) - double(sword->Stat(63))) * double(float(double(slot) / 31.0)))));
            CHECK(server.durability.high == most);
            CHECK(server.durability.high <= shown.durability.high);
        }
    }
    CHECK(fractional > 0); // the attacks are not whole numbers on the server
}

TEST_CASE("the complete armour set: six places of one family") {
    assets::ItemTable items;
    const auto piece = [&](std::uint32_t id, int tid3, int tid4) {
        assets::ItemRow r = Row(tid3, tid4, 3, 2, {1, -1, -1, -1}, {1, 0, 0, 0});
        r.refObjId = id;
        r.codename = "ITEM_PIECE_" + std::to_string(id);
        items.Add(r);
    };
    // Garments (TID3 1) for every part, a protector chest (2), European light and heavy chests (10, 11).
    constexpr int kPartTid4[6] = {1, 3, 2, 6, 4, 5}; // head, chest, shoulders, hands, legs, feet
    for (int part = 0; part < 6; ++part) piece(std::uint32_t(100 + part), 1, kPartTid4[part]);
    piece(200, 2, 3);
    game::Inventory inv;
    for (int part = 0; part < 6; ++part) {
        game::ItemInstance item;
        item.refId = std::uint32_t(100 + part);
        item.durability = 10;
        inv.places[std::uint8_t(part)] = item;
    }
    CHECK(inv.CompleteArmourFamily(items) == 1);
    // A broken piece still counts.
    inv.places[0].durability = 0;
    CHECK(inv.CompleteArmourFamily(items) == 1);
    // Five: no set.
    inv.places.erase(5);
    CHECK(inv.CompleteArmourFamily(items) == 0);
    // Another family's chest: no set.
    game::ItemInstance item;
    item.refId = 105;
    inv.places[5] = item;
    inv.places[1].refId = 200;
    CHECK(inv.CompleteArmourFamily(items) == 0);
}

TEST_CASE("the armour parts' defaults by part, the parts' defence from the float STR") {
    // Nothing worn: every part's default (the sums do not depend on the order).
    const game::CombatStats bare = game::PlayerCombatStats(50, 100, 80, {}, nullptr);
    CHECK(bare.phyParts == doctest::Approx(3.2 + 4.2 + 2.7 + 2.5 + 3.4 + 3.0));
    CHECK(bare.magParts == doctest::Approx(5.2 + 6.7 + 4.3 + 3.9 + 5.5 + 4.9));
    // A chest (place 1) worn whose reinforcements are 0: the chest's own default (4.2, 6.7) goes, not the shoulders'.
    assets::ItemRow chest = Row(1, 3, 0, 2, {1, -1, -1, -1}, {1, 0, 0, 0});
    game::ItemInstance item;
    item.refId = chest.refObjId;
    item.durability = 10;
    const game::CombatStats one = game::PlayerCombatStats(50, 100, 80, {{game::slot::kChest, &chest, &item}}, nullptr);
    CHECK(one.phyParts == doctest::Approx(bare.phyParts - 4.2));
    CHECK(one.magParts == doctest::Approx(bare.magParts - 6.7));
    // STR 100.5 through a sheet: the parts' defence from 100.5, not 100.
    game::StatSheet sheet;
    sheet.Add(1, game::StatChange{game::stat::kStr, 0, 0.5f});
    const game::CombatStats half = game::PlayerCombatStats(50, 100, 80, {}, nullptr, sheet);
    CHECK(half.phyDefence - bare.phyDefence == doctest::Approx(0.5 * bare.phyParts / 100.0));
}

TEST_CASE("a shield's block rate blue: trunc(its white block) + the value in the white's place") {
    // A white block of 12.7 and a blue of 5: the block becomes trunc(12.7) + 5 = 17, the change 17 - 12.7 on the white.
    const std::vector<game::StatChange> c = game::BlueChanges(0x627269, 5, 12.7f);
    REQUIRE(c.size() == 1);
    CHECK(c[0].stat == game::stat::kBlock);
    CHECK(c[0].kind == 0);
    CHECK(c[0].value == doctest::Approx(17.0 - 12.7).epsilon(1e-5));
    // No white (an avatar's, the Devil's Spirit's brna): the value alone.
    const std::vector<game::StatChange> d = game::BlueChanges(0x62726E61, 2, 0.0f);
    REQUIRE(d.size() == 1);
    CHECK(d[0].value == doctest::Approx(2.0));
}

TEST_CASE("the bare hands' reinforcements: as entered, then swapped once a weapon left the hand; the attacks from the float STR") {
    // As the character came in: phy 30.6..34.2 % of STR, mag 49.0..54.8 % of INT.
    const game::CombatStats entered = game::PlayerCombatStats(50, 100, 80, {}, nullptr);
    CHECK(entered.phyReinforceMin == doctest::Approx(30.6));
    CHECK(entered.phyReinforceMax == doctest::Approx(34.2));
    CHECK(entered.magReinforceMin == doctest::Approx(49.0));
    CHECK(entered.magReinforceMax == doctest::Approx(54.8));
    CHECK(entered.phyAttackMax == doctest::Approx(100 * 34.2 / 100.0).epsilon(1e-5));
    // After a weapon left the hand: the phy max and the mag min swapped, 49.0 and 34.2.
    const game::CombatStats after = game::PlayerCombatStats(50, 100, 80, {}, nullptr, game::BareHands::AfterWeapon);
    CHECK(after.phyReinforceMin == doctest::Approx(30.6));
    CHECK(after.phyReinforceMax == doctest::Approx(49.0));
    CHECK(after.magReinforceMin == doctest::Approx(34.2));
    CHECK(after.magReinforceMax == doctest::Approx(54.8));
    CHECK(after.phyAttackMax == doctest::Approx(100 * 49.0 / 100.0).epsilon(1e-5));
    CHECK(after.magAttackMin == doctest::Approx(80 * 34.2 / 100.0).epsilon(1e-5));
    // STR 100.5 through a sheet: the attacks from 100.5 (0x11 <- STR), as the parts' defence.
    game::StatSheet sheet;
    sheet.Add(1, game::StatChange{game::stat::kStr, 0, 0.5f});
    const game::CombatStats half = game::PlayerCombatStats(50, 100, 80, {}, nullptr, sheet);
    CHECK(half.phyAttackMin - entered.phyAttackMin == doctest::Approx(0.5 * entered.phyReinforceMin / 100.0));
    CHECK(half.phyAttackMax - entered.phyAttackMax == doctest::Approx(0.5 * entered.phyReinforceMax / 100.0));
    CHECK(game::PlayerCombatStats(50, 100, 80, {}, nullptr, sheet, game::BareHands::AfterWeapon).phyAttackMax ==
          doctest::Approx(100.5 * 49.0 / 100.0).epsilon(1e-5));
}
