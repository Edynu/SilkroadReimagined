#include <doctest/doctest.h>

#include <map>

#include "sr/assets/GameData.h"

using namespace sr::assets;

TEST_CASE("DecodeObjectRows reads RefObjID, codename and model") {
    const char* text = "// comment\r\n"
                       "1\t1907\tCHAR_CH_MAN_ADVENTURER\txxx\t16\t50\t100\tchar\\china\\chinaman_adventurer.bsr\t0\r\n"
                       "0\t1908\tCHAR_OLD\t\t1\r\n";
    std::vector<ObjectRow> rows;
    std::string error;
    REQUIRE(DecodeObjectRows(text, rows, error));
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].inService);
    CHECK(rows[0].refObjId == 1907);
    CHECK(rows[0].codename == "CHAR_CH_MAN_ADVENTURER");
    CHECK(rows[0].modelPath == "char\\china\\chinaman_adventurer.bsr");
    CHECK_FALSE(rows[1].inService);
    CHECK(rows[1].modelPath.empty());
    CHECK_FALSE(DecodeObjectRows("characterdata_5000.txt\r\n", rows, error)); // an index file: no object rows
}

TEST_CASE("DecodeObjectRows: a COS's speeds, icon and ride flag") {
    std::string line = "1\t2000\tCOS_C_HORSE1"; // 1.188's 104 columns
    for (int c = 3; c < 104; ++c) {
        line += '\t';
        line += c == 46 ? "30" : c == 47 ? "90" : c == 54 ? "cos\\horse.ddj" : c == 66 ? "1" : "0";
    }
    std::vector<ObjectRow> rows;
    std::string error;
    REQUIRE(DecodeObjectRows(line, rows, error));
    CHECK(rows[0].walkSpeed == 30.0f);
    CHECK(rows[0].runSpeed == 90.0f);
    CHECK(rows[0].iconPath == "cos\\horse.ddj");
    CHECK(rows[0].canRide);
}

#include <cstring>

#include "sr/assets/Model.h"
#include "sr/assets/Outfit.h"

namespace {

void PutU32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}

} // namespace

TEST_CASE("FindPartLink finds the item and character cover tables at the end of a .bsr") {
    std::vector<std::byte> item(0x80, std::byte{0x7F}); // other sections
    PutU32(item, 1); // a
    PutU32(item, 1); // b: body armour
    PutU32(item, 1); // c: replaces the skin
    PutU32(item, 2); // count
    PutU32(item, 11);
    PutU32(item, 0);
    PutU32(item, 12);
    PutU32(item, 0);
    PartLink link;
    REQUIRE(FindPartLink(item, false, 1, link));
    CHECK(link.b == 1);
    CHECK(link.c == 1);
    REQUIRE(link.pairs.size() == 2);
    CHECK(link.pairs[1][0] == 12);

    std::vector<std::byte> body(0x80, std::byte{0x7F});
    PutU32(body, 0);
    PutU32(body, 0);
    PutU32(body, 0);
    PutU32(body, 3);
    for (std::uint32_t i = 0; i < 3; ++i) {
        PutU32(body, 10 + i); // cover key
        PutU32(body, i + 1);  // mesh index
    }
    PutU32(body, 0); // nComboNum
    REQUIRE(FindPartLink(body, true, 6, link));
    REQUIRE(link.pairs.size() == 3);
    CHECK(link.pairs[2][0] == 12);
    CHECK(link.pairs[2][1] == 3);
    CHECK_FALSE(FindPartLink(body, true, 2, link)); // mesh index 3 is out of range
}

TEST_CASE("BindWorldPositions composes parent rotations") {
    SkeletonData skeleton;
    Bone root;
    root.name = "root";
    root.local.translation = {1, 0, 0};
    root.local.rotation = {0, 0.70710678f, 0, 0.70710678f}; // 90 degrees about Y
    Bone child;
    child.name = "child";
    child.parent = 0;
    child.local.translation = {0, 0, 2};
    skeleton.bones = {root, child};
    const auto positions = BindWorldPositions(skeleton);
    REQUIRE(positions.size() == 2);
    // +Z rotated 90 degrees about Y goes to +X.
    CHECK(positions[1][0] == doctest::Approx(3.0f));
    CHECK(positions[1][2] == doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("FindPartLink accepts an item table followed by one more u32") {
    std::vector<std::byte> item(0x80, std::byte{0x7F});
    PutU32(item, 1);
    PutU32(item, 1);
    PutU32(item, 1);
    PutU32(item, 1);
    PutU32(item, 2); // key
    PutU32(item, 0);
    PutU32(item, 0); // trailing
    PartLink link;
    REQUIRE(FindPartLink(item, false, 2, link));
    REQUIRE(link.pairs.size() == 1);
    CHECK(link.pairs[0][0] == 2);
}

TEST_CASE("FindPartLink takes an item table whose keys carry the value 1 (a thief's suit, a job suit's part 16)") {
    std::vector<std::byte> item(0x80, std::byte{0x7F});
    PutU32(item, 1);
    PutU32(item, 16); // b: the job suit
    PutU32(item, 1);  // c: hides the body under it
    PutU32(item, 3);
    PutU32(item, 0); // key, value
    PutU32(item, 0);
    PutU32(item, 2);
    PutU32(item, 1);
    PutU32(item, 3);
    PutU32(item, 0);
    PartLink link;
    REQUIRE(FindPartLink(item, false, 1, link));
    CHECK(link.b == 16);
    REQUIRE(link.pairs.size() == 3);
    CHECK(link.pairs[1][0] == 2);
    CHECK(link.pairs[1][1] == 1);
}

TEST_CASE("DecodeLevelTable reads the EXP to the next level") {
    std::vector<std::uint64_t> exp;
    std::string error;
    REQUIRE(DecodeLevelTable("1\t35\t5\n2\t95\t10\n// comment\n110\t9000000000\t0\n", exp, error));
    REQUIRE(exp.size() == 111);
    CHECK(exp[1] == 35);
    CHECK(exp[2] == 95);
    CHECK(exp[110] == 9000000000ull);
    CHECK_FALSE(DecodeLevelTable("", exp, error));
}

TEST_CASE("DecodeItemRows reads the name key, icon, level and stack size") {
    std::string line = "1\t62\tITEM_ETC_HP_POTION_01\txxx\txxx\tSN_ITEM_ETC_HP_POTION_01\tSN_ITEM_ETC_HP_POTION_01_TT_DESC";
    for (int c = 7; c < 59; ++c) {
        std::string value = "0";
        if (c >= 9 && c <= 12) value = std::to_string(c == 9 ? 3 : c == 10 ? 3 : 1);
        if (c == 33) value = "4";
        if (c == 52) value = "xxx";
        if (c == 53) value = "item\\etc\\drop_ch_bag.bsr";
        if (c == 54) value = "item\\etc\\hp_potion_01.ddj";
        if (c == 57) value = "50";
        line += "\t" + value;
    }
    std::vector<ItemRow> rows;
    std::string error;
    REQUIRE_MESSAGE(DecodeItemRows(line + "\n", rows, error), error);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].nameKey == "SN_ITEM_ETC_HP_POTION_01");
    CHECK(rows[0].iconPath == "item\\etc\\hp_potion_01.ddj");
    CHECK(rows[0].requiredLevel == 4);
    CHECK(rows[0].maxStack == 50);
    CHECK(rows[0].typeId[1] == 3);
    CHECK(rows[0].modelPath.empty());
    CHECK(rows[0].dropModelPath == "item\\etc\\drop_ch_bag.bsr");
}

TEST_CASE("DecodeObjectRows reads level, HP, size, experience, skills and material set") {
    std::vector<std::string> c(104, "0"); // 1.188's layout
    c[0] = "1";
    c[1] = "1933";
    c[2] = "MOB_CH_TIGER";
    c[5] = "SN_MOB_CH_TIGER";
    c[48] = "120";
    c[57] = "5";
    c[59] = "54";
    c[79] = "24";
    c[83] = "160";
    c[84] = "161";
    c[93] = "3";
    c[52] = "mob\\china\\tiger.bsr";
    std::string line;
    for (const std::string& v : c) line += (line.empty() ? "" : "\t") + v;
    std::vector<ObjectRow> rows;
    std::string error;
    REQUIRE(DecodeObjectRows(line + "\r\n", rows, error));
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].level == 5);
    CHECK(rows[0].maxHp == 54);
    CHECK(rows[0].scalePercent == 120);
    CHECK(rows[0].experience == 24);
    CHECK(rows[0].skills[0] == 160);
    CHECK(rows[0].skills[1] == 161);
    CHECK(rows[0].materialKind == 3);
    CHECK(rows[0].modelPath == "mob\\china\\tiger.bsr");
}

TEST_CASE("item and skill rows carry their use delay and the group that shares it") {
    std::vector<std::string> c(160, "0");
    c[0] = "1";
    c[1] = "62";
    c[2] = "ITEM_ETC_PILL";
    c[3] = c[4] = "xxx";
    c[5] = "SN_ITEM";
    c[6] = "SN_ITEM_DESC";
    c[9] = c[10] = "3";
    c[11] = "1";
    c[52] = "xxx";
    c[54] = "item\\etc\\pill.ddj";
    c[57] = "10";
    c[118] = "600000"; // an earlier pair: replaced by the last COOLTIME one
    c[119] = "COOLTIME:0x000000F2";
    c[156] = "500";
    c[157] = "COOLTIME:0x1F3"; // only the low byte is the group
    std::string line;
    for (std::size_t i = 0; i < c.size(); ++i) line += (i ? "\t" : "") + c[i];
    std::vector<ItemRow> items;
    std::string error;
    REQUIRE_MESSAGE(DecodeItemRows(line + "\n", items, error), error);
    REQUIRE(items.size() == 1);
    CHECK(items[0].cooldownMs == 500);
    CHECK(items[0].cooldownGroup == 0xF3);

    std::vector<std::string> s(118, "0");
    s[0] = "1";
    s[1] = "300";
    s[3] = "SKILL_300";
    s[8] = "1";
    s[14] = "12000";
    s[18] = std::to_string((7u << 24) | 0x123456u); // the group is the top byte
    s[61] = "skill\\china\\x.ddj";
    std::string skill;
    for (std::size_t i = 0; i < s.size(); ++i) skill += (i ? "\t" : "") + s[i];
    std::vector<SkillRow> skills;
    REQUIRE_MESSAGE(DecodeSkillRows(skill + "\n", skills, error), error);
    REQUIRE(skills.size() == 1);
    CHECK(skills[0].cooldownMs == 12000);
    CHECK(skills[0].cooldownGroup == 7);
}

namespace {
// A skilldata row with the columns the client reads.
std::string SkillLine(int id, int group, int level, int mastery, int row, int column, int sp, int chainNext = 0) {
    std::vector<std::string> c(118, "0");
    c[0] = "1";
    c[1] = std::to_string(id);
    c[2] = std::to_string(group);
    c[3] = "SKILL_" + std::to_string(id);
    c[7] = std::to_string(level);
    c[8] = "1";
    c[9] = std::to_string(chainNext);
    c[34] = std::to_string(mastery);
    c[36] = std::to_string(level);
    c[46] = std::to_string(sp);
    c[59] = std::to_string(row);
    c[60] = std::to_string(column);
    c[61] = "skill\\china\\x.ddj";
    c[62] = "SN_SKILL_" + std::to_string(id);
    std::string line;
    for (std::size_t i = 0; i < c.size(); ++i) line += (i ? "\t" : "") + c[i];
    return line + "\n";
}
} // namespace

TEST_CASE("skill rows decode with their board place, costs and chains") {
    std::vector<SkillRow> rows;
    std::string error;
    REQUIRE_MESSAGE(DecodeSkillRows(SkillLine(10, 5, 1, 257, 0, 2, 30) + SkillLine(11, 5, 2, 257, 0, 2, 40, 12) +
                                        SkillLine(12, 6, 1, 257, 0, 3, 50),
                                    rows, error),
                    error);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].group == 5);
    CHECK(rows[1].level == 2);
    CHECK(rows[1].chainNext == 12);
    CHECK(rows[0].masteries[0] == 257);
    CHECK(rows[0].spCost == 30);
    CHECK(rows[0].boardColumn == 2);
    CHECK(rows[0].iconPath == "skill\\china\\x.ddj");
    CHECK(rows[0].nameKey == "SN_SKILL_10");
    CHECK_FALSE(DecodeSkillRows("1\t2\t3\n", rows, error));

    std::vector<MasteryRow> masteries;
    REQUIRE(DecodeMasteryRows("257\tx\tUIIT_STT_MASTERY_VI\t1\tdesc\ttab\t0\t0\t2\t3\t0\tskillmastery\\china\\sword.ddj\tfocus.ddj\n",
                              masteries, error));
    CHECK(masteries[0].id == 257);
    CHECK(masteries[0].nameKey == "UIIT_STT_MASTERY_VI");
    CHECK(masteries[0].iconPath == "skillmastery\\china\\sword.ddj");
    std::vector<SkillGroupRow> groups;
    REQUIRE(DecodeSkillGroupRows("1\t5\t257\tx\t3\tSN_GROUP\tskillgroup\\china\\g.ddj\n", groups, error));
    CHECK(groups[0].id == 5);
    CHECK(groups[0].boardRow == 3);
}

TEST_CASE("DecodeMasteryCosts reads leveldata's mastery SP column") {
    std::vector<std::uint32_t> sp;
    std::string error;
    REQUIRE(DecodeMasteryCosts("1\t35\t18\n2\t95\t26\n", sp, error));
    CHECK(sp[1] == 18);
    CHECK(sp[2] == 26);
}

TEST_CASE("DecodeObjectRows reads a clone row's base link (it has no model of its own)") {
    const char* text = "1\t1933\tMOB_CH_TIGER\txxx\txxx\tSN_MOB_CH_TIGER\tx\tx\tx\t1\t2\t1\t1\r\n"
                       "1\t1934\tMOB_CH_TIGER_CLON\tx\tMOB_CH_TIGER\tSN_MOB_CH_TIGER_CLON\tx\tx\tx\t1\t2\t1\t1\r\n";
    std::vector<ObjectRow> rows;
    std::string error;
    REQUIRE(DecodeObjectRows(text, rows, error));
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].linkedCodename.empty());
    CHECK(rows[1].linkedCodename == "MOB_CH_TIGER");
    CHECK(rows[1].modelPath.empty());
}

TEST_CASE("Teleport tables: gates, points and links") {
    std::string gate = "1\t20001\tSTORE_CH_GATE";
    const std::map<int, const char*> cells = {{5, "SN_STORE_CH_GATE"}, {41, "-32764"}, {43, "1254"}, {44, "-6"},
                                              {45, "1374"},             {49, "40"},     {50, "25"}};
    for (int c = 3; c < 58; ++c) gate += std::string("\t") + (cells.contains(c) ? cells.at(c) : "0");
    std::vector<TeleportGate> gates;
    REQUIRE(DecodeTeleportGates(gate + "\n0\t1\tOFF\n", gates));
    REQUIRE(gates.size() == 1);
    CHECK(gates[0].refObjId == 20001);
    CHECK(gates[0].nameKey == "SN_STORE_CH_GATE");
    CHECK(gates[0].radius == 25.0f);
    CHECK(gates[0].region == 0x8004); // a dungeon's region
    CHECK(gates[0].x == 1254.0f);
    CHECK(gates[0].y == -6.0f);
    CHECK(gates[0].z == 1374.0f);
    std::vector<TeleportPoint> points;
    REQUIRE(DecodeTeleportPoints("1\t2\tGATE_CH\t20001\tSN_GATE_CH\t25000\t1\t2\t3\t0\t1\t0\t1\n", points));
    CHECK(points[0].id == 2);
    CHECK(points[0].ownerRefObjId == 20001);
    CHECK(points[0].recall);
    CHECK(points[0].region == 25000);
    CHECK(points[0].x == 1.0f);
    CHECK(points[0].z == 3.0f);
    CHECK(points[0].HasArrival());
    std::vector<TeleportLink> links;
    REQUIRE(DecodeTeleportLinks("1\t2\t5\t1000\t1\t0\t1\t20\t80\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\n", links));
    CHECK(links[0].from == 2);
    CHECK(links[0].to == 5);
    CHECK(links[0].fee == 1000);
    CHECK(links[0].minLevel == 20);
    CHECK(links[0].maxLevel == 80);
}

TEST_CASE("quest rows and their contents") {
    std::vector<QuestRow> rows;
    std::string error;
    REQUIRE(DecodeQuestData("1\t3\tQNO_CH_SMITH_1\t1\tk\tSN_QNO_CH_SMITH_1\tSN_PAY_QNO_CH_SMITH_1\tx\tSN_PAYCON_QNO_CH_SMITH_1\tSN_NN_A\tSN_NC_A\n"
                            "0\t4\tQOFF\t1\tk\tSN_QOFF\n",
                            rows, error));
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == 3);
    CHECK(rows[0].titleKey == "SN_QNO_CH_SMITH_1");
    CHECK(rows[0].payTextKey == "SN_PAYCON_QNO_CH_SMITH_1");
    DecodeQuestContents("QNO_CH_SMITH_1\tk\t3\txxx\t1\tSN_CON_QNO_CH_SMITH_1_1\tSN_CON_QNO_CH_SMITH_1_2\txxx\n"
                        "QNO_CH_SMITH_1\tk\t3\txxx\t0\tSN_CON_LATER\n",
                        rows);
    CHECK(rows[0].giveUpWarning == 1);
    REQUIRE(rows[0].objectiveKeys.size() == 2);
    CHECK(rows[0].objectiveKeys[1] == "SN_CON_QNO_CH_SMITH_1_2");
}
