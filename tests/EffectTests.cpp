#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "sr/assets/Effect.h"

using namespace sr::assets;

namespace {

// Builds a JMXVEFF file the way the format lays it out.
struct Writer {
    std::vector<std::byte> bytes;
    void Raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        bytes.insert(bytes.end(), b, b + n);
    }
    void U8(std::uint8_t v) { Raw(&v, 1); }
    void U32(std::uint32_t v) { Raw(&v, 4); }
    void F32(float v) { Raw(&v, 4); }
    void Str(const std::string& s) {
        U32(static_cast<std::uint32_t>(s.size()));
        Raw(s.data(), s.size());
    }
    void Command(const std::string& name, std::uint8_t flags = 0) {
        U8(1);
        Str(name);
        U8(flags);
        U8(0);
        F32(0);
        F32(1);
        F32(0);
    }
    void Absent() { U8(0); }
};

void Node(Writer& w, const std::string& name, int children) {
    w.U32(0);
    w.Str(name);
    w.U32(2); // controllers
    w.Str(name == "child" ? "NormalTimeLoopLife" : "NormalTimeLife");
    w.Str("StaticEmit");
    w.U32(0);
    w.U32(20);
    w.U32(2);
    w.U32(10);
    w.F32(1.5f);
    w.U32(40); // frames
    w.U32(1);  // parameters
    w.Str("BlendScaleGraph");
    w.F32(0);
    w.F32(1);
    w.U32(2);
    w.F32(0);
    w.F32(1);
    w.F32(1);
    w.F32(1);
    w.F32(1);
    w.F32(3);
    w.F32(3);
    w.F32(3);
    w.U32(0); // pre
    w.U32(1); // emitter
    w.Command("StaticEmit");
    w.U32(0);
    w.U32(20);
    w.U32(2);
    w.U32(10);
    w.F32(1.5f);
    w.U32(0); // post emitter
    w.Command("NormalTimeExtinct");
    w.U32(1);
    w.Command("ProgramUpdate");
    w.U8(0);
    w.U8(1);
    w.U32(0);
    w.U32(0);
    w.U32(0);
    w.U8(0);
    w.U32(0);
    w.U8(0);
    w.Command("ViewBillboard");
    w.U32(1);  // back faces
    w.U32(5);  // SRCALPHA
    w.U32(2);  // ONE
    for (int i = 0; i < 6; ++i) w.U32(0);
    w.U32(1);
    w.Str("");
    w.U32(1);
    w.Str("skill\\spark.ddj");
    w.Command("RenderPlate");
    w.U32(0); // trailing
    w.U32(2); // render program
    w.Command("SetGraphDiffuse");
    w.U32(2);
    const std::uint8_t bgra[8] = {10, 20, 30, 255, 1, 2, 3, 0};
    w.Raw(bgra, 8);
    w.Command("SetConeVel", 2);
    for (float v : {1.0f, 2.0f, 30.0f, 0.0f, 0.0f, 0.0f}) w.F32(v);
    w.U32(static_cast<std::uint32_t>(children));
}

} // namespace

TEST_CASE("DecodeEfp reads nodes, programs and the resource") {
    Writer w;
    w.Raw("JMXVEFF 0012", 12);
    w.F32(2.0f);
    Node(w, "root", 1);
    Node(w, "child", 0);
    EffectData effect;
    std::string error;
    REQUIRE_MESSAGE(DecodeEfp(w.bytes, effect, error), error);
    CHECK(effect.version == 12);
    CHECK(effect.scale == 2.0f);
    CHECK(effect.root.name == "root");
    CHECK(effect.root.frames == 40);
    REQUIRE(effect.root.children.size() == 1);
    CHECK(effect.root.children[0].name == "child");
    REQUIRE(effect.root.controllers.size() == 2);
    CHECK(effect.root.controllers[1].values == std::vector<float>{0, 20, 2, 10, 1.5f});
    CHECK_FALSE(effect.root.Loops());
    CHECK(effect.root.children[0].Loops()); // NormalTimeLoopLife
    const EffectCommand* emit = EffectNode::Find(effect.root.emitterProgram, "StaticEmit");
    REQUIRE(emit != nullptr);
    CHECK(emit->values == std::vector<float>{0, 20, 2, 10, 1.5f});
    CHECK(effect.root.life.name == "NormalTimeExtinct");
    CHECK(effect.root.view.name == "ViewBillboard");
    CHECK(effect.root.render.name == "RenderPlate");
    CHECK(effect.root.keepOrigin);
    CHECK(effect.root.resource.additive());
    REQUIRE(effect.root.resource.meshes.size() == 1);
    CHECK(effect.root.resource.meshes[0].textures[0] == "skill\\spark.ddj");
    const EffectCommand* diffuse = EffectNode::Find(effect.root.renderProgram, "SetGraphDiffuse");
    REQUIRE(diffuse != nullptr);
    REQUIRE(diffuse->colors.size() == 2);
    CHECK(diffuse->colors[0] == std::array<std::uint8_t, 4>{30, 20, 10, 255});
    REQUIRE(effect.root.scaleGraphs.size() == 1);
    CHECK(effect.root.scaleGraphs[0].points[1].value[0] == 3.0f);

    w.bytes.push_back(std::byte{0});
    CHECK_FALSE(DecodeEfp(w.bytes, effect, error)); // trailing bytes
    CHECK_FALSE(DecodeEfp(std::vector<std::byte>(12), effect, error));
}

#include "sr/assets/SkillAnimation.h"

TEST_CASE("skilleffect.txt: animations, hit effects, stages and character info") {
    std::string ani = "1\t0\tSKILL_CH_SWORD_SMASH";
    // after the service column: 1 name, 2..4, 5 group, 6..8 animations, 9..11, 12 defense, 13 damage, ... 18 critical
    const char* cols[] = {"x", "0", "FALSE", "1", "SWORD", "ANI_SKILL_1", "", "ANI_SKILL_2", "", "", "",
                          "hiteffect\\def.efp", "hiteffect\\hit_1.efp", "", "", "", "", "hiteffect\\crit.efp", "xxx"};
    for (std::size_t i = 1; i < std::size(cols); ++i) ani += std::string("\t") + cols[i];
    std::vector<std::string> stage(28, "0");
    stage[0] = "smash";
    stage[1] = "SKILL_CH_SWORD_SMASH_01";
    stage[2] = "SHOT";
    stage[3] = "1";
    stage[4] = "TRUE";
    stage[6] = "CHAR_BASE";
    stage[7] = "3";
    stage[9] = "3";
    stage[10] = "4";
    stage[11] = "1";
    stage[13] = "at_stop"; // read without regard to case
    stage[16] = "1,0,0,0,500,0";
    stage[23] = "arrive.efp";
    stage[14] = "MOV_NONE,0,0,0";
    stage[17] = "skill\\china\\sword";
    stage[18] = "smash.efp";
    stage[19] = "@Bip01 R Hand";
    stage[20] = "0,1,2";
    stage[21] = "#Bip01 Head"; // a point in the bone's frame
    stage[22] = "1,0,0";
    stage[24] = "450"; // 361..720: about X, minus 360
    stage[25] = "SCT_SHAKECAM";
    std::string stageLine;
    for (const std::string& v : stage) stageLine += (stageLine.empty() ? "" : "\t") + v;
    const std::string text = "#section skillaniset2\n" + ani + "\n#section skilleffectset\n" + stageLine +
                             "\n#section characterInfo\nMOB_CH_TIGER\tx\t30\tx\tx\tx\tx\tBip01 Spine\t0,5,0\tblood_red\tx\t1,0\n";
    std::unordered_map<std::string, SkillAnimation> rows;
    std::unordered_map<std::string, CharacterEffectInfo> characters;
    std::string error;
    REQUIRE_MESSAGE(DecodeSkillAnimations(text, rows, error, &characters), error);
    const SkillAnimation& row = rows.at("SKILL_CH_SWORD_SMASH");
    CHECK(row.group == "sword");
    CHECK_FALSE(row.overlap);
    CHECK(row.hideWeapon);
    CHECK(row.damageEffect == "hiteffect\\hit_1.efp");
    CHECK(row.defenseEffect == "hiteffect\\def.efp");
    CHECK(row.criticalEffect == "hiteffect\\crit.efp");
    REQUIRE(row.stages.size() == 1);
    const SkillEffectStage& s = row.stages[0];
    CHECK(s.phase == 2);
    CHECK(s.startEvent == 1);
    CHECK(s.damageEvent);
    CHECK(s.scale == 1);
    CHECK(s.action == "AT_STOP");
    CHECK(s.slot == 3);
    CHECK(s.trade == 3);
    CHECK(s.kill == 4);
    CHECK(s.lifeMs == 500.0f);
    CHECK(s.arrival == "arrive.efp");
    CHECK(s.object == "skill\\china\\sword\\smash.efp");
    CHECK(s.start.bone == "Bip01 R Hand");
    CHECK_FALSE(s.start.keepTurn);
    CHECK_FALSE(s.start.inBone);
    CHECK(s.start.offset[2] == 2.0f);
    CHECK(s.target.bone == "Bip01 Head");
    CHECK_FALSE(s.target.keepTurn);
    CHECK(s.target.inBone);
    CHECK(s.target.offset[0] == 1.0f);
    CHECK(s.rotateAxis == 'x');
    CHECK(s.rotateRadians == doctest::Approx(90.0f / 360.0f * 6.2831853f));
    CHECK(s.scripts == std::vector<std::string>{"SCT_SHAKECAM"});
    const CharacterEffectInfo& tiger = characters.at("MOB_CH_TIGER");
    CHECK(tiger.height == 30.0f);
    CHECK(tiger.anchorBone == "Bip01 Spine");
    CHECK(tiger.anchorOffset[1] == 5.0f);
    CHECK(tiger.blood == "hiteffect\\blood_red.efp");
    CHECK(tiger.bringsRain);
}

#include "sr/assets/Model.h"

TEST_CASE("DecodeBsr reads the modifier sets (glow, flip-books, attached effects)") {
    Writer w;
    w.Raw("JMXVRES 0109", 12);
    const std::size_t pointers = w.bytes.size();
    for (int i = 0; i < 13; ++i) w.U32(0);
    while (w.bytes.size() < 0x40) w.U8(0);
    w.U32(0); // kinds
    w.Str("gate");
    const auto point = [&](int section) {
        const auto at = static_cast<std::uint32_t>(w.bytes.size());
        std::memcpy(w.bytes.data() + pointers + 4 * static_cast<std::size_t>(section), &at, 4);
    };
    point(0);
    w.U32(0); // material sets
    point(1);
    w.U32(0); // meshes
    point(6);
    w.U32(1); // one set
    w.U32(2);
    w.U32(0xFFFFFFFF);
    w.Str("ambient");
    w.U32(3);
    // a material modifier: base, timeline of 2 keys, enables, states, rate, cull
    w.U32(0x00000);
    for (std::uint32_t v : {0u, 0u, 0x110u, 0xFFFFFFFFu, 0u, 0u}) w.U32(v);
    for (int i = 0; i < 4; ++i) w.U8(0);
    w.U32(1000);
    w.U32(2);
    w.U32(1);
    w.U32(2);
    w.U32(0);
    for (float f : {1.0f, 0.0f, 0.0f, 1.0f}) w.F32(f);
    w.U32(1000);
    for (float f : {0.0f, 0.0f, 1.0f, 1.0f}) w.F32(f);
    for (std::uint32_t v : {0u, 1u, 1u, 0u}) w.U32(v);
    const std::uint8_t states[12] = {5, 2};
    w.Raw(states, 12);
    w.F32(0.5f);
    w.U32(1);
    // a texture flip-book
    w.U32(0x10000);
    for (int i = 0; i < 6; ++i) w.U32(0);
    w.U8(0);
    w.U8(15);
    w.U8(2);
    w.U8(0);
    for (std::uint32_t v : {0u, 0u, 10u, 4u, 4u}) w.U32(v);
    for (int i = 0; i < 16; ++i) w.F32(0);
    // an attached effect
    w.U32(0x30000);
    for (int i = 0; i < 6; ++i) w.U32(0);
    for (int i = 0; i < 4; ++i) w.U8(0);
    w.U32(1);
    w.U32(1);
    w.Str("building\\gate_light.efp");
    w.Str("Bone01");
    for (float f : {1.0f, 2.0f, 3.0f}) w.F32(f);
    w.U32(0);
    w.U8(0);
    w.U8(1);
    w.U8(0);
    w.U8(0);
    w.U32(0); // the second list: no sets
    ResourceData resource;
    std::string error;
    REQUIRE_MESSAGE(DecodeBsr(w.bytes, resource, error), error);
    CHECK_FALSE(resource.modifiersUnread);
    REQUIRE(resource.modifierSets.size() == 1);
    const ResourceData::ModifierSet& set = resource.modifierSets[0];
    CHECK(set.kind == 2);
    CHECK(set.animationSet == "ambient");
    REQUIRE(set.materials.size() == 1);
    CHECK(set.materials[0].keys.size() == 2);
    CHECK(set.materials[0].keys[1].rgba[2] == 1.0f);
    CHECK(set.materials[0].states[1] == 2);
    CHECK(set.materials[0].twoSided);
    REQUIRE(set.textures.size() == 1);
    CHECK(set.textures[0].words[2] == 10);
    CHECK(set.textures[0].end == 15);
    REQUIRE(set.particles.size() == 1);
    CHECK(set.particles[0].atRoot);
    CHECK(set.particles[0].effect == "building\\gate_light.efp");
    CHECK(set.particles[0].offset[2] == 3.0f);
    CHECK(set.particles[0].flags[1] == 1);
}

#include "sr/assets/ItemGlow.h"

TEST_CASE("itemrare.txt: an item's glows, their size, bone and offset") {
    sr::assets::ItemGlowTable table;
    std::string error;
    const std::string text = "ITEM_CH_BLADE_01_A_RARE\tsystem\\system_raretype_b_step1.efp\t130\tai_start\tnone\r\n"
                             "ITEM_CH_BLADE_01_A_RARE\tsystem\\system_raretype_b_step1_add.efp\t130\tai_end\tnone\r\n"
                             "ITEM_EU_SHIELD_01_C_RARE\tsystem\\system_rareshield_c.efp\t90\tBone01\t1,2.5,-3\r\n";
    REQUIRE_MESSAGE(table.Decode(text, error), error);
    CHECK(table.size() == 2);
    CHECK(table.lines() == 3);
    const auto* blade = table.Find("ITEM_CH_BLADE_01_A_RARE");
    REQUIRE(blade != nullptr);
    REQUIRE(blade->size() == 2);
    CHECK((*blade)[0].effect == "system\\system_raretype_b_step1.efp");
    CHECK((*blade)[0].scale == doctest::Approx(1.3f));
    CHECK((*blade)[1].bone == "ai_end");
    CHECK((*blade)[1].offset[0] == 0.0f);
    const auto* shield = table.Find("ITEM_EU_SHIELD_01_C_RARE");
    REQUIRE(shield != nullptr);
    CHECK((*shield)[0].offset[1] == 2.5f);
    CHECK((*shield)[0].offset[2] == -3.0f);
    CHECK(table.Find("ITEM_CH_SWORD_01_A") == nullptr);
    CHECK_FALSE(table.Decode("ITEM_X\tsystem\\a.efp\tbig\tBone01\tnone\n", error));
    CHECK_FALSE(table.Decode("ITEM_X\tsystem\\a.efp\n", error));
    // itemoptionefp.txt's lines carry a plus range: shown from +8 to +100; the header a comment.
    REQUIRE_MESSAGE(table.Add("//Item Code Name\tParticle\tScale\tBone\tOffset\r\n"
                              "ITEM_CH_BLADE_01_A_RARE\tsystem\\system_enchant_b_01.efp\t130\tai_end\tnone\t8\t100\t100\r\n",
                              "itemoptionefp.txt", error),
                    error);
    CHECK(table.lines() == 1);
    const auto* sparks = table.Find("ITEM_CH_BLADE_01_A_RARE");
    REQUIRE(sparks != nullptr);
    CHECK_FALSE((*sparks)[0].ShowsAt(7));
    CHECK((*sparks)[0].ShowsAt(8));
    CHECK((*sparks)[0].ShowsAt(100));
    CHECK_FALSE((*sparks)[0].ShowsAt(101));
}

#include "sr/assets/Dress.h"

TEST_CASE("the plus glow: a type's row by the plus, and the row riding on an item's outfit path") {
    sr::assets::EquipGlowTable table;
    std::string error;
    REQUIRE_MESSAGE(table.DecodeTypes("//_HONOR\ttype_number\r\nITEM_CH_BOW_10_C_RARE\t2\r\n", error), error);
    REQUIRE_MESSAGE(table.DecodeRows("// a header\r\n\r\n"
                                     "2 \t3\t0.005000,-0.005000\tprim\\mtrl\\itemoption\\option_texture13.ddj\t4\t7\t1.000,1.000,1.000\t0.988,1.000,0.690\t1000\t1\r\n\r\n"
                                     "2 \t9\t0.005000,-0.005000\tprim\\mtrl\\itemoption\\option_texture14.ddj\t5\t7\t0.992,0.996,0.510\t0.831,0.231,0.000\t1000\t1\r\n",
                                     error),
                    error);
    CHECK(table.types() == 1);
    CHECK(table.rows() == 2);
    // The row of the highest lowest plus not above the item's; none below the first, nor at 0.
    CHECK(table.Find("ITEM_CH_BOW_10_C_RARE", 2) == nullptr);
    REQUIRE(table.Find("ITEM_CH_BOW_10_C_RARE", 8) != nullptr);
    CHECK(table.Find("ITEM_CH_BOW_10_C_RARE", 8)->minPlus == 3);
    const sr::assets::EquipGlow* nine = table.Find("ITEM_CH_BOW_10_C_RARE", 9);
    REQUIRE(nine != nullptr);
    CHECK(nine->colorOp == 5);
    CHECK(table.Find("ITEM_CH_SWORD_01_A", 9) == nullptr);
    CHECK_FALSE(table.DecodeRows("2\t9\tx\n", error));

    // On the outfit's path and off it again, whole; a path without one stays as it is.
    std::string path = sr::assets::WithGlow("res\\item\\china\\weapon\\bow_10.bsr", *nine);
    CHECK(path.find('|') == std::string::npos);
    sr::assets::EquipGlow back;
    REQUIRE(sr::assets::SplitGlow(path, back));
    CHECK(path == "res\\item\\china\\weapon\\bow_10.bsr");
    CHECK(back.texture == nine->texture);
    CHECK(back.scroll[1] == doctest::Approx(-0.005f));
    CHECK(back.colorOp == 5);
    CHECK(back.from[1] == doctest::Approx(0.996f));
    CHECK(back.to[1] == doctest::Approx(0.231f));
    CHECK(back.periodMs == 1000);
    CHECK(back.alphaTest);
    std::string plain = "res\\item\\china\\weapon\\bow_10.bsr";
    CHECK_FALSE(sr::assets::SplitGlow(plain, back));
    CHECK(plain == "res\\item\\china\\weapon\\bow_10.bsr");
}
