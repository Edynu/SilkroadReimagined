#include "sr/server/ReferenceData.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace sr::server {

namespace {

// How the server spends the drop tables (budgets per grade, gold multipliers, the uniques' counts) is not in the
// database: these are the original server's constants.
constexpr double kNegligible = 0.000001; // a class at or below this never rolls
struct Family {
    const char* name;
    int id;
    const char* table;
    std::vector<std::tuple<int, int, int>> types; // TypeID2, TypeID3, TypeID4 (-1: any)
};
const std::vector<Family>& Families() {
    static const std::vector<Family> kFamilies = {
        {"potion", 2, "_RefDropClassSel_Recover", {{3, 1, -1}}},
        {"cure", 3, "_RefDropClassSel_Cure", {{3, 2, -1}}},
        {"arrow", 4, "_RefDropClassSel_Ammo", {{3, 4, 1}}},
        {"bolt", 5, "_RefDropClassSel_Ammo", {{3, 4, 2}}},
        {"scroll", 6, "_RefDropClassSel_Scroll", {{3, 3, -1}}},
        {"tablet", 7, "_RefDropClassSel_Alchemy_Tablet", {{3, 11, 3}}},
        {"magicstone", 8, "_RefDropClassSel_Alchemy_MagicStone", {{3, 11, 1}, {3, 11, 7}}},
        {"attrstone", 9, "_RefDropClassSel_Alchemy_ATTRStone", {{3, 11, 2}}},
        {"elixir", 10, "_RefDropClassSel_Reinforce", {{3, 10, -1}}},
    };
    return kFamilies;
}
// A pass's consumable rolls in the original's order: recovery, cures,
// reinforcement, tablets, then ammunition or a scroll, then a magic or an attribute stone ("|": one of them, rand() % 2;
// ",": the families of one, picked alike).
constexpr std::pair<const char*, const char*> kCategories[] = {{"potion", "potion"}, {"cure", "cure"},
                                                               {"elixir", "elixir"}, {"tablet", "tablet"},
                                                               {"ammo-scroll", "arrow,bolt|scroll"}, {"stone", "magicstone|attrstone"}};
const std::map<int, std::tuple<int, int, int>> kBudgets = {{0, {8, 1, 1}},   {1, {9, 2, 1}},  {2, {8, 1, 1}},  {3, {60, 10, 30}}, {4, {30, 5, 4}},
                                                           {5, {60, 60, 1}}, {6, {20, 4, 1}}, {7, {30, 8, 1}}, {8, {60, 10, 30}}};
const std::map<std::string, std::tuple<int, int, int>> kBudgetMonsters = {{"MOB_RM_ROC", {250, 10, 30}}, {"MOB_TQ_WHITESNAKE", {60, 10, 60}}};
const std::map<int, int> kGoldMultipliers = {{3, 30}, {8, 30}, {4, 4}, {7, 4}};
struct UniqueCount {
    const char* who;
    const char* stage;
    int low, high;
};
constexpr UniqueCount kUniqueCounts[] = {
    {"default", "equipment", 5, 8},     {"default", "repairable", 0, 2},      {"default", "recipe", 0, 2},        {"default", "scroll", 10, 20},
    {"grade8", "equipment", 1, 2},      {"grade8", "repairable", 0, 0},       {"grade8", "recipe", 1, 2},         {"grade8", "scroll", 0, 0},
    {"MOB_RM_ROC", "equipment", 50, 80}, {"MOB_RM_ROC", "repairable", 10, 20}, {"MOB_RM_ROC", "recipe", 50, 100}, {"MOB_RM_ROC", "scroll", 100, 150},
    {"MOB_TK_BONELORD", "repairable", 0, 1}, {"MOB_TQ_WHITESNAKE", "repairable", 1, 3}, {"MOB_TQ_WHITESNAKE", "recipe", 1, 3},
};

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// A probability as the float32 the tables hold, printed so it reads back exactly.
std::string F32(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.9g", double(float(value)));
    return text;
}

struct Object {
    std::string code;
    int type2 = 0, type3 = 0, type4 = 0, rarity = 0, country = 0;
    std::int64_t link = 0;
};

// A class table's (monster level, group from 0, probability) for each ProbGroupN column.
bool ClassRows(Database& db, const std::string& table, std::vector<std::tuple<int, int, double>>& out, std::string& error) {
    QueryResult columns = db.Query("SELECT column_name FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = $1 "
                                   "AND column_name LIKE 'probgroup%' ORDER BY ordinal_position",
                                   {Lower(table)});
    if (!columns.Ok() || columns.Rows() == 0) {
        error = table + ": no ProbGroup columns";
        return false;
    }
    std::string list;
    std::vector<int> groups;
    for (int i = 0; i < columns.Rows(); ++i) {
        const std::string name = columns.Text(i, 0);
        list += ", " + name;
        groups.push_back(std::atoi(name.c_str() + 9) - 1);
    }
    QueryResult r = db.Query(std::format("SELECT monlevel{} FROM {}", list, Lower(table)));
    if (!r.Ok()) {
        error = table + ": " + r.Error();
        return false;
    }
    for (int row = 0; row < r.Rows(); ++row) {
        for (std::size_t k = 0; k < groups.size(); ++k) {
            if (!r.IsNull(row, int(k) + 1)) out.emplace_back(int(r.Int(row, 0)), groups[k], r.Double(row, int(k) + 1));
        }
    }
    return true;
}

} // namespace

bool HasReferenceTables(Database& db) {
    for (const char* table : {"_refobjcommon", "_refdropitemassign", "tab_refnest", "tab_reftactics"}) {
        QueryResult r = db.Query("SELECT to_regclass($1) IS NOT NULL", {table});
        if (!r.Ok() || !r.Bool(0, 0)) return false;
        QueryResult rows = db.Query(std::format("SELECT EXISTS (SELECT 1 FROM {})", table));
        if (!rows.Ok() || !rows.Bool(0, 0)) return false;
    }
    return true;
}

bool DropRecords(Database& db, const assets::ItemTable& items, const assets::CharacterTable& characters, std::string& out, ReferenceCounts& counts,
                 std::string& error) {
    out = "# monster drops (from the database's original tables)\n";
    std::unordered_map<std::int64_t, Object> objects;
    QueryResult o = db.Query("SELECT id, codename128, typeid2, typeid3, typeid4, rarity, country, link FROM _refobjcommon");
    QueryResult classes = db.Query("SELECT id, itemclass FROM _refobjitem");
    if (!o.Ok() || !classes.Ok()) {
        error = o.Ok() ? classes.Error() : o.Error();
        return false;
    }
    for (int i = 0; i < o.Rows(); ++i) {
        objects[o.Int(i, 0)] = {o.Text(i, 1), int(o.Int(i, 2)), int(o.Int(i, 3)), int(o.Int(i, 4)), int(o.Int(i, 5)), int(o.Int(i, 6)), o.Int(i, 7)};
    }
    std::unordered_map<std::int64_t, int> itemClass;
    for (int i = 0; i < classes.Rows(); ++i) itemClass[classes.Int(i, 0)] = int(classes.Int(i, 1));
    std::set<std::string> missingItems, missingMonsters;
    const auto hasItem = [&](const std::string& code) {
        if (items.Find(std::string_view(code)) != nullptr) return true;
        missingItems.insert(code);
        return false;
    };
    const auto hasMonster = [&](const std::string& code) {
        if (characters.Find(std::string_view(code)) != nullptr) return true;
        missingMonsters.insert(code);
        return false;
    };
    const auto emit = [&](const std::string& line) {
        out += line + "\n";
        ++counts.records;
    };

    for (const auto& [grade, b] : kBudgets) emit(std::format("budget\t{}\t{}\t{}\t{}", grade, std::get<0>(b), std::get<1>(b), std::get<2>(b)));
    emit("budget-party\t9");
    for (const auto& [code, b] : kBudgetMonsters) {
        if (hasMonster(code)) emit(std::format("budget-monster\t{}\t{}\t{}\t{}", code, std::get<0>(b), std::get<1>(b), std::get<2>(b)));
    }
    for (int grade = 0; grade < 9; ++grade) {
        const auto it = kGoldMultipliers.find(grade);
        emit(std::format("gold-multiplier\t{}\t{}", grade, it == kGoldMultipliers.end() ? 1 : it->second));
    }
    emit("gold-multiplier\tparty\t9");
    QueryResult plus = db.Query("SELECT optlevel, prob FROM _refdropoptlvlsel ORDER BY optlevel");
    if (!plus.Ok()) {
        error = plus.Error();
        return false;
    }
    for (int i = 0; i < plus.Rows(); ++i) emit(std::format("plus\t{}\t{}", plus.Int(i, 0), F32(plus.Double(i, 1))));
    for (const Family& f : Families()) emit(std::format("family\t{}\t{}\t{}", f.name, f.id, f.table));
    for (std::size_t order = 0; order < std::size(kCategories); ++order) {
        emit(std::format("category\t{}\t{}\t{}", order, kCategories[order].first, kCategories[order].second));
    }
    for (const auto& [kind, table] : {std::pair{"normal", "_RefDropClassSel_Equip"}, std::pair{"rare", "_RefDropClassSel_RareEquip"}}) {
        std::vector<std::tuple<int, int, double>> rows;
        if (!ClassRows(db, table, rows, error)) return false;
        for (const auto& [level, group, p] : rows) {
            if (p > kNegligible) emit(std::format("equip-class\t{}\t{}\t{}\t{}", level, kind, group, F32(p)));
        }
    }

    // The class tables' items: equipment by its class (AssignedGroup -1), consumables by group.
    std::map<std::tuple<int, int, int>, std::string> familyOf;
    for (const Family& f : Families()) {
        for (const auto& t : f.types) familyOf[t] = f.name;
    }
    struct Equip {
        bool rare;
        int group, country;
        std::string code, weight, absolute;
    };
    struct Consumable {
        int family;
        int group;
        std::string code, weight, absolute;
        int count;
    };
    std::vector<Equip> equip;
    std::vector<Consumable> consumable;
    QueryResult assign = db.Query("SELECT service, refitemid, assignedgroup, prob_relative, prob_absolute, dropcount FROM _refdropitemassign");
    if (!assign.Ok()) {
        error = assign.Error();
        return false;
    }
    const auto familyIndex = [&](const std::string& name) {
        for (std::size_t i = 0; i < Families().size(); ++i) {
            if (name == Families()[i].name) return int(i);
        }
        return -1;
    };
    for (int i = 0; i < assign.Rows(); ++i) {
        const auto it = objects.find(assign.Int(i, 1));
        if (assign.Int(i, 0) != 1 || it == objects.end()) continue;
        const Object& ob = it->second;
        if (assign.Int(i, 2) == -1) {
            if (ob.type2 == 1 && (ob.rarity == 0 || ob.rarity == 2) && (ob.country == 0 || ob.country == 1)) {
                const auto cls = itemClass.find(ob.link);
                equip.push_back({ob.rarity == 2, (cls == itemClass.end() ? 0 : cls->second) - 1, ob.country, ob.code, assign.Text(i, 3), assign.Text(i, 4)});
            }
            continue;
        }
        auto f = familyOf.find({ob.type2, ob.type3, ob.type4});
        if (f == familyOf.end()) f = familyOf.find({ob.type2, ob.type3, -1});
        if (f != familyOf.end()) {
            consumable.push_back({familyIndex(f->second), int(assign.Int(i, 2)) - 1, ob.code, assign.Text(i, 3), assign.Text(i, 4), int(assign.Int(i, 5))});
        }
    }
    std::sort(equip.begin(), equip.end(), [](const Equip& a, const Equip& b) {
        return std::tie(a.rare, a.group, a.country, a.code) < std::tie(b.rare, b.group, b.country, b.code);
    });
    for (const Equip& e : equip) {
        if (!hasItem(e.code)) continue;
        emit(std::format("equip-item\t{}\t{}\t{}\t{}\t{}\t{}\t{}", e.rare ? "rare" : "normal", e.group, e.country, e.code, e.weight, e.absolute,
                         items.Find(std::string_view(e.code))->requiredLevel));
    }
    for (const Family& f : Families()) {
        std::vector<std::tuple<int, int, double>> rows;
        if (!ClassRows(db, f.table, rows, error)) return false;
        for (const auto& [level, group, p] : rows) {
            if (p > kNegligible) emit(std::format("consumable-class\t{}\t{}\t{}\t{}", f.name, level, group, F32(p)));
        }
    }
    std::sort(consumable.begin(), consumable.end(), [](const Consumable& a, const Consumable& b) {
        return std::tie(a.family, a.group, a.code) < std::tie(b.family, b.group, b.code);
    });
    for (const Consumable& c : consumable) {
        if (hasItem(c.code)) {
            emit(std::format("consumable-item\t{}\t{}\t{}\t{}\t{}\t{}", Families()[std::size_t(c.family)].name, c.group, c.code, c.weight, c.absolute,
                             c.count));
        }
    }

    // A monster's drop groups (Overlap 1: a member at most once per kill, its index taken out:;
    // ROC's three set pieces all different; the tokens' and talismans' Overlap 0 may repeat). Their members with their
    // RefMagicGroupID, and those groups' MagicType 1 rows (the options an equipment member drops with: ROC's set;
    // MagicType 2, the SET pieces' tiers, writes nothing).
    std::set<std::int64_t> usedGroups;
    QueryResult rnd = db.Query("SELECT service, refmonsterid, refitemgroupid, dropratio, dropamountmin, dropamountmax, overlap "
                               "FROM _refmonster_assigneditemrnddrop ORDER BY refmonsterid, refitemgroupid");
    if (!rnd.Ok()) {
        error = rnd.Error();
        return false;
    }
    for (int i = 0; i < rnd.Rows(); ++i) {
        const auto it = objects.find(rnd.Int(i, 1));
        if (rnd.Int(i, 0) != 1 || it == objects.end() || !hasMonster(it->second.code)) continue;
        usedGroups.insert(rnd.Int(i, 2));
        emit(std::format("assigned-group\t{}\t{}\t{}\t{}\t{}\t{}", it->second.code, rnd.Int(i, 2), F32(rnd.Double(i, 3)), rnd.Int(i, 4), rnd.Int(i, 5),
                         rnd.Int(i, 6) == 1 ? 1 : 0));
    }
    QueryResult members = db.Query("SELECT refitemgroupid, refitemid, selectratio, coalesce(refmagicgroupid, 0) FROM _refdropitemgroup WHERE service = 1 "
                                   "ORDER BY refitemgroupid, refitemid");
    if (!members.Ok()) {
        error = members.Error();
        return false;
    }
    std::set<std::int64_t> usedMagicGroups;
    for (int i = 0; i < members.Rows(); ++i) {
        const auto it = objects.find(members.Int(i, 1));
        if (it == objects.end() || !usedGroups.contains(members.Int(i, 0)) || !hasItem(it->second.code)) continue;
        emit(std::format("group-item\t{}\t{}\t{}\t{}", members.Int(i, 0), it->second.code, F32(members.Double(i, 2)), members.Int(i, 3)));
        if (members.Int(i, 3) != 0) usedMagicGroups.insert(members.Int(i, 3));
    }
    QueryResult magic = db.Query("SELECT linkid, moptid, value FROM _refmagicoptgroup WHERE service = 1 AND magictype = 1 ORDER BY linkid, moptlevel, moptid");
    if (!magic.Ok()) {
        error = magic.Error();
        return false;
    }
    for (int i = 0; i < magic.Rows(); ++i) {
        if (usedMagicGroups.contains(magic.Int(i, 0))) emit(std::format("magic-group\t{}\t{}\t{}", magic.Int(i, 0), magic.Int(i, 1), magic.Int(i, 2)));
    }

    // A monster's own alchemy materials.
    QueryResult materials = db.Query("SELECT refmonsterid, refitemid, dropratio, dropamountmin, dropamountmax FROM _refmonster_assigneditemdrop "
                                     "ORDER BY refmonsterid, refitemid");
    if (!materials.Ok()) {
        error = materials.Error();
        return false;
    }
    for (int i = 0; i < materials.Rows(); ++i) {
        const auto monster = objects.find(materials.Int(i, 0));
        const auto item = objects.find(materials.Int(i, 1));
        if (monster == objects.end() || item == objects.end() || !hasMonster(monster->second.code) || !hasItem(item->second.code)) continue;
        emit(std::format("material\t{}\t{}\t{}\t{}\t{}", monster->second.code, item->second.code, F32(materials.Double(i, 2)), materials.Int(i, 3),
                         materials.Int(i, 4)));
    }

    // The monsters' profiles (_RefCustomizingReservedItemDropForMonster): each record for
    // its kind; the "repair invalid" ones (command 4, group 0) for the monster whatever its kind: the record's value
    // (P3; 0: the option's own pick, 0 for 'nrep') and the option's Param3, the durability's raise.
    QueryResult profiles = db.Query("SELECT p.refmonsterid, p.rarity, p.command, p.dropgrouptype, p.param1, p.param2, coalesce(p.param3, 0), "
                                    "coalesce(m.param3, 0) FROM _refcustomizingreserveditemdropformonster p "
                                    "LEFT JOIN _refmagicopt m ON p.command = 4 AND m.id = p.param1 "
                                    "ORDER BY p.refmonsterid, p.rarity, p.dropgrouptype, p.command, p.param1, p.param2");
    if (!profiles.Ok()) {
        error = profiles.Error();
        return false;
    }
    for (int i = 0; i < profiles.Rows(); ++i) {
        const auto monster = objects.find(profiles.Int(i, 0));
        if (monster == objects.end() || !hasMonster(monster->second.code)) continue;
        const std::int64_t command = profiles.Int(i, 2), group = profiles.Int(i, 3);
        if (command == 4) {
            emit(std::format("profile-option\t{}\t{}\t{}\t{}\t{}", monster->second.code, profiles.Int(i, 4), profiles.Int(i, 5), profiles.Int(i, 6),
                             profiles.Int(i, 7)));
        } else {
            emit(std::format("profile\t{}\t{}\t{}\t{}\t{}\t{}", monster->second.code, profiles.Int(i, 1), command, group, profiles.Int(i, 4),
                             profiles.Int(i, 5)));
        }
    }

    for (const UniqueCount& u : kUniqueCounts) {
        const std::string who = u.who;
        if (who == "default" || who == "grade8" || hasMonster(who)) emit(std::format("unique-count\t{}\t{}\t{}\t{}", u.who, u.stage, u.low, u.high));
    }
    for (const char* kind : {"WEAPON", "SHIELD", "ARMOR", "ACCESSARY"}) {
        const std::string code = std::string("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_") + kind + "_B";
        if (hasItem(code)) emit("unique-item\trecipe\t" + code);
    }
    if (hasItem("ITEM_ETC_SCROLL_RETURN_02")) emit("unique-item\tscroll\tITEM_ETC_SCROLL_RETURN_02");
    counts.missingItems = int(missingItems.size());
    counts.missingMonsters = int(missingMonsters.size());
    return true;
}

bool MonsterResistanceRecords(Database& db, MonsterResistances& out, std::string& error) {
    static constexpr const char* kColumns[32] = {
        "resistfrozen", "resistfrostbite", "resistburn", "resisteshock", "resistpoison", "resistzombie", "resistsleep", "resistroot",
        "resistslow",   "resistfear",      "resistmyopia", "resistblood", "resiststone", "resistdark", "resiststun",  "resistdisea",
        "resistchaos",  "resistcsepd",     "resistcsemd", "resistcsestr", "resistcseint", "resistcsehp", "resistcsemp", "resist24",
        "resistbomb",   "resist26",        "resist27",    "resist28",    "resist29",    "resist30",   "resist31",    "resist32"};
    std::string sql = "SELECT c.id";
    for (const char* column : kColumns) sql += std::format(", x.{}", column);
    sql += " FROM _refobjcommon c JOIN _refobjchar x ON x.id = c.link WHERE c.typeid1 = 1 AND c.typeid2 = 2";
    QueryResult r = db.Query(sql);
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    for (int i = 0; i < r.Rows(); ++i) {
        std::array<std::uint8_t, 32>& resist = out[std::uint32_t(r.Int(i, 0))];
        for (int k = 0; k < 32; ++k) resist[std::size_t(k)] = std::uint8_t(std::clamp<long long>(r.Int(i, k + 1), 0, 255));
    }
    return true;
}

bool HwanLevelRecords(Database& db, std::vector<game::HwanLevel>& out, std::string& error) {
    QueryResult r = db.Query("SELECT hwanlevel, paramfourcc1, paramvalue1, paramfourcc2, paramvalue2, paramfourcc3, paramvalue3, "
                             "paramfourcc4, paramvalue4, paramfourcc5, paramvalue5 FROM _refhwanlevel ORDER BY hwanlevel");
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    for (int i = 0; i < r.Rows(); ++i) {
        game::HwanLevel h;
        h.level = int(r.Int(i, 0));
        for (int k = 0; k < 5; ++k) {
            const std::uint32_t code = std::uint32_t(r.Int(i, 1 + 2 * k));
            const int value = int(r.Int(i, 2 + 2 * k));
            if (code == assets::SkillTag("SPDU")) h.speedPercent = value;
            if (code == assets::SkillTag("DABR")) h.damageCut = value;
        }
        out.push_back(h);
    }
    return true;
}

bool MagicOptGroupRecords(Database& db, std::vector<assets::MagicOptGroupRow>& out, std::string& error) {
    QueryResult r = db.Query("SELECT linkid, magictype, codename128, moptid, moptlevel, value FROM _refmagicoptgroup WHERE service = 1 "
                             "ORDER BY linkid, moptlevel, moptid");
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    for (int i = 0; i < r.Rows(); ++i) {
        assets::MagicOptGroupRow g;
        g.link = int(r.Int(i, 0));
        g.magicType = int(r.Int(i, 1));
        g.codename = r.Text(i, 2);
        g.optId = std::uint32_t(std::max<std::int64_t>(0, r.Int(i, 3)));
        g.optLevel = int(r.Int(i, 4));
        g.value = int(r.Int(i, 5));
        out.push_back(std::move(g));
    }
    return true;
}

bool NestRecords(Database& db, std::string& out, ReferenceCounts& counts, std::string& error) {
    QueryResult r = db.Query(
        "SELECT o.codename128, n.nregiondbid, n.flocalposx, n.flocalposz, n.nradius, n.ngenerateradius, n.nchampiongenpercentage, "
        "       n.dwdelaytimemin, n.dwdelaytimemax, n.dwmaxtotalcount, t.btaggresstype, t.nsightrange, "
        "       n.dwnestid, n.dwhiveid, coalesce(h.dwoverwritemaxtotalcount, 0), coalesce(h.gameworldid, 1) BETWEEN 51 AND 59 "
        "FROM tab_refnest n JOIN tab_reftactics t ON t.dwtacticsid = n.dwtacticsid JOIN _refobjcommon o ON o.id = t.dwobjid "
        "LEFT JOIN tab_refhive h ON h.dwhiveid = n.dwhiveid "
        "WHERE o.typeid1 = 1 AND o.typeid2 = 2 AND o.typeid3 = 1 ORDER BY n.dwnestid");
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    // The first nest at a place keeps it (codename, region, the place in whole units), as the generator did.
    std::map<std::tuple<std::string, int, long long, long long>, std::string> rows;
    // The instance worlds' own nests (the temples' and the Pharaoh's tomb's hives, worlds 51..59; the Forgotten World's are in
    // dungeon regions of their own): each instance raises them in its layer (InstanceData.cpp). Their points (the client's
    // npcpos) are listed as "!" lines: no monster of the shared world stands there.
    std::set<std::tuple<std::string, int, long long, long long>> instanceOnly;
    for (int i = 0; i < r.Rows(); ++i) {
        const std::tuple<std::string, int, long long, long long> key{r.Text(i, 0), int(std::uint16_t(r.Int(i, 1))), std::llround(float(r.Double(i, 2))),
                                                                     std::llround(float(r.Double(i, 3)))};
        if (r.Bool(i, 15)) {
            instanceOnly.insert(key);
            continue;
        }
        if (rows.contains(key)) continue;
        rows[key] = std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", r.Int(i, 4), r.Int(i, 5), r.Int(i, 6), r.Int(i, 7), r.Int(i, 8),
                                r.Int(i, 9), r.Int(i, 10) == 0 ? 1 : 0, r.Int(i, 11), r.Int(i, 12), r.Int(i, 13), r.Int(i, 14));
    }
    out = "# codename\tregion\tx\tz\tradius\tspawnRadius\tchampion%\tdelayMin\tdelayMax\tmaxCount\taggressive\tsight\tnest\thive\thiveMax "
          "(from the database's original tables)\n";
    for (const auto& [key, values] : rows) {
        out += std::format("{}\t{}\t{}\t{}\t{}\n", std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key), values);
        ++counts.records;
    }
    for (const auto& key : instanceOnly) {
        if (!rows.contains(key)) out += std::format("!{}\t{}\t{}\t{}\n", std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key));
    }
    return true;
}

} // namespace sr::server
