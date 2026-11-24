// The drops' random blues, 1:1 with the original game server: the rule over the 1.188 data (the gate, the
// count, the lists by kind, the rows by the million roll, the values, Repair never), a pass's piece kept with or without
// blues, a unique's and a profile's extra pieces lost when the roll refuses them ('nrep' after the blues on the
// enhanced ones), ROC's group options, and the item's own numbers and alchemy reading the ROC set's _SET options by
// code. Needs SR_GAME_DIR (the client's magicoption.txt and magicoptionassign.txt are the database's rows); otherwise
// skipped.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Text.h"
#include "sr/game/Alchemy.h"
#include "sr/game/Items.h"
#include "sr/server/Drops.h"

using namespace sr;

namespace {

struct BlueData {
    assets::FileSystem fs;
    assets::ItemTable items;
    std::shared_ptr<std::vector<assets::MagicOptionRow>> options = std::make_shared<std::vector<assets::MagicOptionRow>>();
    std::vector<assets::MagicOptionAssignRow> assign;
    bool ok = false;
    BlueData() {
        const char* dir = std::getenv("SR_GAME_DIR");
        std::string error;
        if (dir == nullptr || !assets::MountGameArchives(fs, dir, error) || !items.Load(fs, error)) return;
        const auto o = fs.Read("server_dep\\silkroad\\textdata\\magicoption.txt");
        const auto a = fs.Read("server_dep\\silkroad\\textdata\\magicoptionassign.txt");
        ok = o && a && assets::DecodeMagicOptions(core::DecodeTextFile(o->Bytes()), *options, error) &&
             assets::DecodeMagicOptionAssign(core::DecodeTextFile(a->Bytes()), assign, error);
    }
    const assets::ItemRow& Item(std::string_view code) const {
        const assets::ItemRow* row = items.Find(code);
        REQUIRE_MESSAGE(row != nullptr, std::string(code));
        return *row;
    }
    const assets::MagicOptionRow& Option(std::uint32_t id) const {
        const assets::MagicOptionRow* row = game::FindOption(*options, id);
        REQUIRE(row != nullptr);
        return *row;
    }
    std::uint32_t Id(std::string_view name, int degree) const {
        const assets::MagicOptionRow* row = game::alchemy::OptionRow(*options, name, degree);
        REQUIRE_MESSAGE(row != nullptr, std::string(name));
        return row->paramId;
    }
};

const BlueData& Data() {
    static const BlueData d;
    return d;
}

#define SKIP_WITHOUT_GAME_FILES(d)                                         \
    if (!(d).ok) {                                                         \
        MESSAGE("SR_GAME_DIR missing (or its option tables) - skipped");   \
        return;                                                            \
    }

std::vector<std::string> Prefixed(std::initializer_list<const char*> names) {
    std::vector<std::string> out;
    for (const char* n : names) out.push_back(std::string("MATTR_") + n);
    return out;
}

// The lists by kind as the 1.188 data has them (the original's table, measured on magicoptionassign.txt).
const std::vector<std::string> kWeapon = Prefixed({"STR", "INT", "ATHANASIA", "SOLID", "LUCK", "DUR", "HR", "EVADE_BLOCK", "REPAIR"});
const std::vector<std::string> kShield = Prefixed({"STR", "INT", "ATHANASIA", "SOLID", "LUCK", "DUR", "EVADE_CRITICAL"});
const std::vector<std::string> kArmourBig = Prefixed({"STR", "INT", "ATHANASIA", "SOLID", "LUCK", "DUR", "ER", "HP", "MP"});
const std::vector<std::string> kArmourSmall = Prefixed({"STR", "INT", "ATHANASIA", "SOLID", "LUCK", "DUR", "ER"});
const std::vector<std::string> kAccessory =
    Prefixed({"STR", "INT", "RESIST_FROSTBITE", "RESIST_BURN", "RESIST_ESHOCK", "RESIST_POISON", "RESIST_ZOMBIE", "ATHANASIA", "LUCK"});

// The values a level-1 row gives (the original's table); the protections' at any level.
const std::map<std::string, std::set<std::uint32_t>> kValues = {
    {"MATTR_STR", {1, 2, 3}},          {"MATTR_INT", {1, 2, 3}},          {"MATTR_HR", {5, 10, 20}},
    {"MATTR_ER", {5, 10, 20}},         {"MATTR_DUR", {10, 20, 30, 60}},   {"MATTR_HP", {5, 10, 20, 30}},
    {"MATTR_MP", {5, 10, 20, 30}},     {"MATTR_EVADE_BLOCK", {3, 6, 18, 35}}, {"MATTR_EVADE_CRITICAL", {3, 9, 18, 35}},
    {"MATTR_RESIST_FROSTBITE", {1, 3, 5}}, {"MATTR_RESIST_ESHOCK", {1, 3, 5}}, {"MATTR_RESIST_BURN", {1, 3, 5}},
    {"MATTR_RESIST_POISON", {1, 3, 5}},    {"MATTR_RESIST_ZOMBIE", {1, 3, 5}}, {"MATTR_ATHANASIA", {1, 2, 3, 4, 5, 6}},
    {"MATTR_LUCK", {1, 2, 3, 4, 5, 6}},    {"MATTR_SOLID", {1, 2, 3, 4, 5, 6}}};

bool Protection(const std::string& name) { return name == "MATTR_ATHANASIA" || name == "MATTR_LUCK" || name == "MATTR_SOLID"; }

// The first in-service piece of a kind (lowest id), of rarity 0, China's.
const assets::ItemRow& PieceOf(int tid3, int tid4) {
    const assets::ItemRow* best = nullptr;
    for (const auto& [code, row] : Data().items.rows()) {
        if (row.typeId != std::array<int, 4>{3, 1, tid3, tid4} || row.rarity != 0 || row.country != 0 || !row.inService || row.maxMagicOptions != 9) continue;
        if (best == nullptr || row.refObjId < best->refObjId) best = &row;
    }
    REQUIRE(best != nullptr);
    return *best;
}

// The durability a drop's piece starts with (its whites', the most).
std::uint32_t BaseDurability(const assets::ItemRow& row, const game::ItemInstance& item) {
    game::ItemInstance bare = item;
    bare.options.clear();
    return std::uint32_t(game::ComputeStats(row, bare, nullptr, 0, true).durability.high);
}

} // namespace

TEST_CASE("Drops' blues: the 1.188 option tables (the lists by kind, the rows' Prob and ReqClass)") {
    const BlueData& d = Data();
    SKIP_WITHOUT_GAME_FILES(d);
    CHECK(d.options->size() == 391);
    CHECK(d.assign.size() == 74);
    const auto list = [&](int race, int tid3, int tid4) {
        for (const assets::MagicOptionAssignRow& a : d.assign) {
            if (a.race == race && a.tid3 == tid3 && a.tid4 == tid4) return a.options;
        }
        return std::vector<std::string>{};
    };
    for (int tid4 = 2; tid4 <= 6; ++tid4) CHECK(list(0, 6, tid4) == kWeapon);
    for (int tid4 = 7; tid4 <= 15; ++tid4) CHECK(list(1, 6, tid4) == kWeapon);
    CHECK(list(0, 4, 1) == kShield);
    CHECK(list(1, 4, 2) == kShield);
    for (int tid3 : {1, 2, 3}) {
        for (int tid4 : {1, 3, 4}) CHECK(list(0, tid3, tid4) == kArmourBig);
        for (int tid4 : {2, 5, 6}) CHECK(list(0, tid3, tid4) == kArmourSmall);
    }
    for (int tid3 : {9, 10, 11}) {
        for (int tid4 : {1, 3, 4}) CHECK(list(1, tid3, tid4) == kArmourBig);
        for (int tid4 : {2, 5, 6}) CHECK(list(1, tid3, tid4) == kArmourSmall);
    }
    for (int tid4 = 1; tid4 <= 3; ++tid4) {
        CHECK(list(0, 5, tid4) == kAccessory);
        CHECK(list(1, 12, tid4) == kAccessory);
    }
    // Every listed option has Prob 1 at every level but the protections (0.3) and Repair (0); ReqClass 1.
    for (const assets::MagicOptionRow& o : *d.options) {
        const bool listed = std::find(kWeapon.begin(), kWeapon.end(), o.optionName) != kWeapon.end() ||
                            std::find(kArmourBig.begin(), kArmourBig.end(), o.optionName) != kArmourBig.end() ||
                            std::find(kAccessory.begin(), kAccessory.end(), o.optionName) != kAccessory.end() ||
                            o.optionName == "MATTR_EVADE_CRITICAL";
        if (!listed) continue;
        CAPTURE(o.optionName);
        CHECK(o.prob == doctest::Approx(Protection(o.optionName) ? 0.3 : o.optionName == "MATTR_REPAIR" ? 0.0 : 1.0));
        REQUIRE(o.reqClasses.size() == o.availGroups.size());
        for (int c : o.reqClasses) CHECK(c == 1);
    }
}

TEST_CASE("Drops' blues: the rule over the 1.188 data (the gate, the count, the lists, level-1 values, Repair never)") {
    const BlueData& d = Data();
    SKIP_WITHOUT_GAME_FILES(d);
    std::string error;
    server::DropTable drops;
    REQUIRE_MESSAGE(drops.LoadText("budget\t0\t8\t1\t1\n", d.items, error), error);
    drops.SetMagicOptions(d.options, d.assign);
    std::mt19937 random(2026);

    // Every written option: in its kind's list, once, no Repair, a level-1 row (the protections 1..4), a value of its list.
    std::map<std::string, std::map<int, int>> levels; // protections: name -> MLevel -> count
    std::map<std::string, std::set<std::uint32_t>> seenValues;
    const auto check = [&](const assets::ItemRow& row, const game::ItemInstance& item, const std::vector<std::string>& list) {
        std::set<std::string> names;
        for (const game::ItemInstance::Option& o : item.options) {
            const assets::MagicOptionRow& option = d.Option(o.id);
            CAPTURE(option.optionName);
            CHECK(std::find(list.begin(), list.end(), option.optionName) != list.end());
            CHECK(option.optionName != "MATTR_REPAIR");
            CHECK(names.insert(option.optionName).second);
            if (Protection(option.optionName)) {
                ++levels[option.optionName][option.degree];
                CHECK(option.degree <= 4);
            } else {
                CHECK(option.degree == 1);
            }
            CHECK(game::alchemy::RowFits(option, row));
            const auto values = kValues.find(option.optionName);
            REQUIRE(values != kValues.end());
            CHECK(values->second.contains(o.value));
            seenValues[option.optionName].insert(o.value);
        }
        CHECK(int(item.options.size()) <= 6);
    };

    // The gate: a normal piece 31 times in 101; no blues when refused.
    const assets::ItemRow& sword = d.Item("ITEM_CH_SWORD_01_A");
    const int n = 30000;
    int passed = 0;
    std::map<std::size_t, int> written;
    for (int i = 0; i < n; ++i) {
        game::ItemInstance item = drops.Instance(sword, 1, 0, random);
        item.options = {{1, 1}};
        const bool blue = drops.RollBlues(sword, item, random);
        passed += blue;
        if (!blue) {
            CHECK(item.options.empty()); // cleared first
            continue;
        }
        ++written[item.options.size()];
        check(sword, item, kWeapon);
    }
    MESSAGE("a normal sword: " << passed << " of " << n << " past the gate");
    CHECK(double(passed) / n == doctest::Approx(31.0 / 101.0).epsilon(0.05));

    // A seal (Rarity 2) always; the same count, lists and level-1 values. The written counts of a list of nine with
    // Repair in it (simulated over the rule: none 6.1 %, 1 55.7, 2 27.0, 3 9.2, 4 1.8, 5 0.15; mean 1.45).
    const assets::ItemRow& seal = d.Item("ITEM_CH_SWORD_01_A_RARE");
    REQUIRE(seal.rarity == 2);
    written.clear();
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        game::ItemInstance item = drops.Instance(seal, 1, 0, random);
        REQUIRE(drops.RollBlues(seal, item, random));
        ++written[item.options.size()];
        sum += double(item.options.size());
        check(seal, item, kWeapon);
    }
    for (const auto& [k, count] : written) MESSAGE("a seal with " << k << " blues: " << 100.0 * count / n << " %");
    CHECK(double(written[0]) / n == doctest::Approx(0.061).epsilon(0.15));
    CHECK(double(written[1]) / n == doctest::Approx(0.557).epsilon(0.04));
    CHECK(double(written[2]) / n == doctest::Approx(0.270).epsilon(0.06));
    CHECK(double(written[3]) / n == doctest::Approx(0.092).epsilon(0.12));
    CHECK(sum / n == doctest::Approx(1.45).epsilon(0.03));
    // Every option of the weapons' list but Repair comes.
    for (const std::string& name : kWeapon) {
        if (name != "MATTR_REPAIR") CHECK_MESSAGE(seenValues.contains(name), name);
    }

    // The other kinds' lists (seals of each kind: the gate passed), their values at level 1.
    const std::vector<std::pair<const assets::ItemRow*, const std::vector<std::string>*>> kinds = {
        {&d.Item("ITEM_CH_SHIELD_01_A_RARE"), &kShield},
        {&d.Item("ITEM_CH_RING_01_A_RARE"), &kAccessory},
    };
    for (const auto& [row, list] : kinds) {
        std::set<std::string> seen;
        for (int i = 0; i < 6000; ++i) {
            game::ItemInstance item = drops.Instance(*row, 1, 0, random);
            REQUIRE(drops.RollBlues(*row, item, random));
            check(*row, item, *list);
            for (const auto& o : item.options) seen.insert(d.Option(o.id).optionName);
        }
        CHECK(seen == std::set<std::string>(list->begin(), list->end()));
    }
    for (const auto& [tid4, list] : {std::pair{1, &kArmourBig}, std::pair{2, &kArmourSmall}}) {
        const assets::ItemRow& armour = PieceOf(3, tid4);
        std::set<std::string> seen;
        for (int i = 0; i < 20000; ++i) {
            game::ItemInstance item = drops.Instance(armour, 1, 0, random);
            if (!drops.RollBlues(armour, item, random)) continue;
            check(armour, item, *list);
            for (const auto& o : item.options) seen.insert(d.Option(o.id).optionName);
        }
        CHECK(seen == std::set<std::string>(list->begin(), list->end()));
    }
    // Every value of each list comes (the protections' 1..6 at every level).
    for (const auto& [name, values] : kValues) {
        CAPTURE(name);
        CHECK(seenValues[name] == values);
    }
    // The protections' levels: 1, 2, 3 30 % each, 4 10 % (Prob 0.3 at every level, the million roll).
    for (const auto& [name, byLevel] : levels) {
        int total = 0;
        for (const auto& [level, count] : byLevel) total += count;
        CAPTURE(name);
        CHECK(double(byLevel.at(1)) / total == doctest::Approx(0.3).epsilon(0.12));
        CHECK(double(byLevel.at(4)) / total == doctest::Approx(0.1).epsilon(0.25));
    }

    // A kind without a list (a Chinese piece of a European weapon's TID4) and a table without the option tables: no
    // blues, false, even for a seal.
    assets::ItemRow unlisted = seal;
    unlisted.typeId[3] = 9;
    game::ItemInstance item = drops.Instance(seal, 1, 0, random);
    CHECK_FALSE(drops.RollBlues(unlisted, item, random));
    CHECK(item.options.empty());
    server::DropTable bare;
    REQUIRE(bare.LoadText("budget\t0\t8\t1\t1\n", d.items, error));
    item = bare.Instance(seal, 1, 0, random);
    CHECK_FALSE(bare.RollBlues(seal, item, random));
    CHECK(item.options.empty());
}

TEST_CASE("Drops' blues: a pass's piece drops with or without them; a unique's and a profile's extra pieces only with them") {
    const BlueData& d = Data();
    SKIP_WITHOUT_GAME_FILES(d);
    std::string error;
    // Level 50 pieces of class 0: a sword or a ring alike (each kept: absolute 99). A unique's default 8 enhanced pieces,
    // an ordinary-only unique's 8, a profile of 8 with "repair invalid" always.
    const std::string records =
        "budget\t0\t8\t1\t1\n"
        "budget\t3\t60\t0\t1\n"
        "equip-class\t50\tnormal\t0\t1\n"
        "equip-item\tnormal\t0\t0\tITEM_CH_SWORD_01_A\t100\t99\t1\n"
        "equip-item\tnormal\t0\t0\tITEM_CH_RING_01_A\t100\t99\t1\n"
        "unique-count\tdefault\tequipment\t8\t8\n"
        "unique-count\tdefault\trepairable\t0\t0\n"
        "unique-count\tMOB_TEST_ORDINARY\tequipment\t0\t0\n"
        "unique-count\tMOB_TEST_ORDINARY\trepairable\t8\t8\n"
        "profile\tMOB_TEST_PROFILED\t3\t1\t11\t0\t0\n"
        "profile\tMOB_TEST_PROFILED\t3\t1\t12\t1\t0\n"
        "profile\tMOB_TEST_PROFILED\t3\t2\t0\t8\t8\n"
        "profile-option\tMOB_TEST_PROFILED\t65\t100\t0\t400\n";
    server::DropTable drops;
    REQUIRE_MESSAGE(drops.LoadText(records, d.items, error), error);
    drops.SetMagicOptions(d.options, d.assign);
    std::mt19937 random(7);
    const server::GameRates rates;
    const std::uint32_t nrep = d.Id("MATTR_NOT_REPARABLE", 1);
    assets::ObjectRow monster;
    monster.level = 50;

    // A pass's piece: one a kill, blue about 31 times in 101, kept either way.
    monster.codename = "MOB_TEST";
    int pieces = 0, blue = 0;
    const int kills = 6000;
    for (int k = 0; k < kills; ++k) {
        for (const auto& drop : drops.Roll(monster, 0, 50, rates, random)) {
            ++pieces;
            blue += !drop.item.options.empty() ? 1 : 0;
            for (const auto& o : drop.item.options) CHECK(o.id != nrep);
        }
    }
    MESSAGE(kills << " kills: " << pieces << " pieces, " << blue << " with blues");
    CHECK(pieces >= kills * 99 / 100);
    // Past the gate a sword writes none 6 % of the time (a Repair draw), a ring always one: 97 % of the 31 in 101.
    CHECK(double(blue) / pieces == doctest::Approx(31.0 / 101.0 * 0.97).epsilon(0.07));

    // A unique's enhanced pieces: an accessory drawn again, a piece the gate refuses lost (8 x 31/101 kept), each a sword
    // with 'nrep' after its blues (value 0, durability x 5, most durability x 5 too).
    monster.codename = "MOB_TEST_UNIQUE";
    const int uniques = 2000;
    pieces = 0;
    for (int k = 0; k < uniques; ++k) {
        for (const auto& drop : drops.Roll(monster, 3, 50, rates, random)) {
            ++pieces;
            const assets::ItemRow* row = d.items.Find(drop.item.refId);
            REQUIRE(row != nullptr);
            CHECK(row->codename == "ITEM_CH_SWORD_01_A");
            REQUIRE(!drop.item.options.empty());
            CHECK(drop.item.options.back().id == nrep);
            CHECK(drop.item.options.back().value == 0);
            for (std::size_t i = 0; i + 1 < drop.item.options.size(); ++i) CHECK(drop.item.options[i].id != nrep);
            CHECK(drop.item.plus >= 1);
            CHECK(drop.item.plus <= 5);
            const std::uint32_t base = BaseDurability(*row, drop.item);
            CHECK(drop.item.durability == base * 5);
            double duru = 0;
            for (const auto& o : drop.item.options) duru += d.Option(o.id).optionName == "MATTR_DUR" ? double(o.value) : 0.0;
            const double most = game::ComputeStats(*row, drop.item, d.options.get(), 0, true).durability.high;
            CHECK(most == doctest::Approx(double(base) + std::trunc(double(base) * (400.0 + duru) / 100.0)));
        }
    }
    MESSAGE(uniques << " uniques: " << pieces << " enhanced pieces kept (8 rolled each)");
    CHECK(double(pieces) / uniques == doctest::Approx(8.0 * 31.0 / 101.0).epsilon(0.06));

    // An ordinary-only unique: an accessory lost too (8 x 1/2 x 31/101), no 'nrep'.
    monster.codename = "MOB_TEST_ORDINARY";
    pieces = 0;
    for (int k = 0; k < uniques; ++k) {
        for (const auto& drop : drops.Roll(monster, 3, 50, rates, random)) {
            ++pieces;
            CHECK(d.items.Find(drop.item.refId)->codename == "ITEM_CH_SWORD_01_A");
            for (const auto& o : drop.item.options) CHECK(o.id != nrep);
        }
    }
    MESSAGE(uniques << " ordinary-only uniques: " << pieces << " pieces kept");
    CHECK(double(pieces) / uniques == doctest::Approx(4.0 * 31.0 / 101.0).epsilon(0.08));

    // A profile's pieces: an accessory lost, the roll's refusal lost; its "repair invalid" record (chance 100: rand() %
    // 101 < 100) after the blues, P3 0: value 0, the durability x 5.
    monster.codename = "MOB_TEST_PROFILED";
    pieces = 0;
    int marked = 0;
    for (int k = 0; k < uniques; ++k) {
        for (const auto& drop : drops.Roll(monster, 3, 50, rates, random)) {
            ++pieces;
            const assets::ItemRow* row = d.items.Find(drop.item.refId);
            CHECK(row->codename == "ITEM_CH_SWORD_01_A");
            const bool repairInvalid = !drop.item.options.empty() && drop.item.options.back().id == nrep;
            marked += repairInvalid ? 1 : 0;
            if (repairInvalid) CHECK(drop.item.options.back().value == 0);
            for (std::size_t i = 0; i + 1 < drop.item.options.size(); ++i) CHECK(drop.item.options[i].id != nrep);
            CHECK(drop.item.durability == BaseDurability(*row, drop.item) * (repairInvalid ? 5u : 1u));
        }
    }
    MESSAGE(uniques << " profiled kills: " << pieces << " pieces kept (8 rolled each), " << marked << " repair invalid");
    CHECK(double(pieces) / uniques == doctest::Approx(4.0 * 31.0 / 101.0).epsilon(0.08));
    CHECK(marked >= pieces * 95 / 100);
    CHECK(marked < pieces);

    // Without the option tables no piece gets blues and every extra piece is lost.
    server::DropTable none;
    REQUIRE(none.LoadText(records, d.items, error));
    monster.codename = "MOB_TEST_UNIQUE";
    for (int k = 0; k < 50; ++k) CHECK(none.Roll(monster, 3, 50, rates, random).empty());
}

TEST_CASE("Drops' blues: a drop group's piece gets its magic group's MagicType 1 rows (ROC's set), Overlap 1 all different") {
    const BlueData& d = Data();
    SKIP_WITHOUT_GAME_FILES(d);
    std::string error;
    // As ReferenceData.cpp writes them from the database: MOB_RM_ROC's group 1 (Overlap 1, 3..3, ratio 1), four of its
    // members with RefMagicGroupID 1, MAGIC_NOR_ROCSET_001's rows; an Undine's group 10 (SET A shields, magic group 10:
    // MagicType 2, no line); an SD token's group (an etc item with a magic group of its own).
    const std::string records =
        "budget\t3\t60\t0\t1\n"
        "assigned-group\tMOB_RM_ROC\t1\t1\t3\t3\t1\n"
        "group-item\t1\tITEM_ROC_CH_M_HEAVY_HA_SET\t0.0139\t1\n"
        "group-item\t1\tITEM_ROC_CH_M_HEAVY_SA_SET\t0.0139\t1\n"
        "group-item\t1\tITEM_ROC_CH_M_CLOTHES_SA_SET\t0.0139\t1\n"
        "group-item\t1\tITEM_ROC_EU_M_CLOTHES_FA_SET\t0.0138\t1\n"
        "magic-group\t1\t357\t6\n"
        "magic-group\t1\t358\t6\n"
        "magic-group\t1\t359\t60\n"
        "magic-group\t1\t362\t160\n"
        "magic-group\t1\t363\t6\n"
        "assigned-group\tMOB_GOD_GHOST_UNDINE_B1\t10\t1\t1\t1\t0\n"
        "group-item\t10\tITEM_CH_SHIELD_11_SET_A_RARE\t0.5\t10\n"
        "group-item\t10\tITEM_EU_SHIELD_11_SET_A_RARE\t0.5\t10\n"
        "assigned-group\tMOB_TEST_TOKENS\t2\t1\t3\t3\t0\n"
        "group-item\t2\tITEM_ETC_SD_TOKEN_01\t1\t1\n";
    server::DropTable drops;
    REQUIRE_MESSAGE(drops.LoadText(records, d.items, error), error);
    drops.SetMagicOptions(d.options, d.assign);
    std::mt19937 random(98);
    const server::GameRates rates;
    assets::ObjectRow roc;
    roc.codename = "MOB_RM_ROC";
    roc.level = 98;
    const std::vector<game::ItemInstance::Option> rocSet = {{357, 6}, {358, 6}, {359, 60}, {362, 160}, {363, 6}};
    for (int k = 0; k < 300; ++k) {
        const auto loot = drops.Roll(roc, 3, 98, rates, random);
        REQUIRE(loot.size() == 3);
        std::set<std::uint32_t> refs;
        for (const auto& drop : loot) {
            refs.insert(drop.item.refId);
            CHECK(drop.item.plus == 0);
            REQUIRE(drop.item.options.size() == rocSet.size());
            for (std::size_t i = 0; i < rocSet.size(); ++i) {
                CHECK(drop.item.options[i].id == rocSet[i].id);
                CHECK(drop.item.options[i].value == rocSet[i].value);
            }
        }
        CHECK(refs.size() == 3); // Overlap 1: three different pieces
    }
    // MagicType 2 groups write nothing (the SET shields drop bare); an etc item never takes options.
    assets::ObjectRow undine;
    undine.codename = "MOB_GOD_GHOST_UNDINE_B1";
    undine.level = 105;
    for (int k = 0; k < 50; ++k) {
        for (const auto& drop : drops.Roll(undine, 3, 105, rates, random)) CHECK(drop.item.options.empty());
    }
    assets::ObjectRow tokens;
    tokens.codename = "MOB_TEST_TOKENS";
    tokens.level = 50;
    const auto loot = drops.Roll(tokens, 3, 50, rates, random);
    CHECK(loot.size() == 3); // Overlap 0: the one token three times
    for (const auto& drop : loot) CHECK(drop.item.options.empty());
}

TEST_CASE("Drops' blues: the ROC set's _SET options count by code (parry, durability, the Luck charge; 'nrep' x 5)") {
    const BlueData& d = Data();
    SKIP_WITHOUT_GAME_FILES(d);
    const assets::ItemRow& helm = d.Item("ITEM_ROC_CH_M_HEAVY_HA_SET");
    game::ItemInstance plain;
    plain.refId = helm.refObjId;
    plain.variance = 0x3FFFFFFF; // every white at 31 (some parry to raise)
    const game::ItemStats before = game::ComputeStats(helm, plain, d.options.get(), 0, true);
    REQUIRE(before.parry.shown);
    game::ItemInstance roc = plain;
    roc.options = {{357, 6}, {358, 6}, {359, 60}, {362, 160}, {363, 6}};
    const game::ItemStats after = game::ComputeStats(helm, roc, d.options.get(), 0, true);
    // 'er' (MATTR_ER_SET): the parry x 1.6; 'duru' (MATTR_DUR_SET): the most durability + 160 %.
    CHECK(after.parry.low == doctest::Approx(double(float(before.parry.low * 0.6 + before.parry.low))));
    CHECK(after.durability.high == doctest::Approx(before.durability.high + std::trunc(before.durability.high * 160.0 / 100.0)));
    // The same numbers as the plain MATTR_ER / MATTR_DUR of those values.
    game::ItemInstance plainBlues = plain;
    plainBlues.options = {{d.Id("MATTR_ER", 1), 60}, {d.Id("MATTR_DUR", 1), 160}};
    const game::ItemStats same = game::ComputeStats(helm, plainBlues, d.options.get(), 0, true);
    CHECK(after.parry.low == doctest::Approx(same.parry.low));
    CHECK(after.durability.high == doctest::Approx(same.durability.high));
    // 'luck' (MATTR_LUCK_SET): a Luck charge in alchemy.
    CHECK(game::alchemy::OptionValue(roc, *d.options, "MATTR_LUCK") == 6);
    CHECK(game::alchemy::UseCharge(roc, *d.options, "MATTR_LUCK"));
    CHECK(roc.options.back().id == 363);
    CHECK(roc.options.back().value == 5);
    // 'nrep' raises the most durability by its row's Param3 (400 %) whatever its value (a drop writes 0).
    game::ItemInstance repairInvalid = plain;
    repairInvalid.options = {{d.Id("MATTR_NOT_REPARABLE", 1), 0}};
    CHECK(game::ComputeStats(helm, repairInvalid, d.options.get(), 0, true).durability.high == doctest::Approx(before.durability.high * 5.0));
    // 'dura' (MATTR_DEC_MAXDUR) takes its value off.
    game::ItemInstance worn = plain;
    worn.options = {{d.Id("MATTR_DEC_MAXDUR", 3), 40}};
    CHECK(game::ComputeStats(helm, worn, d.options.get(), 0, true).durability.high ==
          doctest::Approx(std::max(1.0, before.durability.high + std::trunc(before.durability.high * -40.0 / 100.0))));
}

TEST_CASE("Drops: a piece's plus from the cumulative table, at most +7 (the original's Loot_SelectEquipmentPlus; U5-05)") {
    assets::ItemTable none;
    std::string error;
    // Every roll on level 10: +7 (a budget row too: a table needs one).
    server::DropTable drops;
    std::string text = "budget\t0\t8\t1\t1\n";
    for (int level = 0; level <= 12; ++level) text += "plus\t" + std::to_string(level) + "\t" + (level >= 10 ? "1" : "0") + "\n";
    REQUIRE_MESSAGE(drops.LoadText(text, none, error), error);
    std::mt19937 random(5);
    for (int i = 0; i < 200; ++i) CHECK(drops.RollPlus(random) == 7);
    // Half on +0, half on +3.
    server::DropTable split;
    std::string half = "budget\t0\t8\t1\t1\n";
    for (int level = 0; level <= 12; ++level) half += "plus\t" + std::to_string(level) + "\t" + (level < 3 ? "0.5" : "1") + "\n";
    REQUIRE_MESSAGE(split.LoadText(half, none, error), error);
    int zero = 0, three = 0;
    for (int i = 0; i < 2000; ++i) {
        const int plus = split.RollPlus(random);
        zero += plus == 0;
        three += plus == 3;
    }
    CHECK(zero + three == 2000);
    CHECK(zero > 850);
    CHECK(three > 850);
}
