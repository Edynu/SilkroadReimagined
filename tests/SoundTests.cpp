// The sound files and tables (sr::audio_data), without the game: hand-made WAVE files and table
// lines in the game's layouts. GameDataTests.cpp decodes the real ones.

#include <doctest/doctest.h>

#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "sr/audio/Decode.h"
#include "sr/audio/SoundTables.h"

using namespace sr::audio;

namespace {

void Put16(std::vector<std::byte>& out, std::uint16_t v) {
    out.push_back(std::byte(v & 0xFF));
    out.push_back(std::byte(v >> 8));
}
void Put32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(std::byte((v >> (8 * i)) & 0xFF));
}
void PutTag(std::vector<std::byte>& out, const char* tag) {
    for (int i = 0; i < 4; ++i) out.push_back(std::byte(tag[i]));
}

// A RIFF WAVE of PCM with an extra chunk (odd-sized, padded) before the data.
std::vector<std::byte> Wave(int channels, int bits, int rate, const std::vector<std::byte>& data) {
    std::vector<std::byte> out;
    PutTag(out, "RIFF");
    Put32(out, 0); // the size: not read
    PutTag(out, "WAVE");
    PutTag(out, "fmt ");
    Put32(out, 16);
    Put16(out, 1);
    Put16(out, std::uint16_t(channels));
    Put32(out, std::uint32_t(rate));
    Put32(out, std::uint32_t(rate * channels * bits / 8));
    Put16(out, std::uint16_t(channels * bits / 8));
    Put16(out, std::uint16_t(bits));
    PutTag(out, "LIST");
    Put32(out, 3);
    out.insert(out.end(), {std::byte('a'), std::byte('b'), std::byte('c'), std::byte(0)});
    PutTag(out, "data");
    Put32(out, std::uint32_t(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

} // namespace

TEST_CASE("a PCM WAVE decodes: 16-bit as it is, 8-bit unsigned to 16-bit, chunks skipped") {
    Pcm pcm;
    std::string error;
    const std::vector<std::byte> samples16 = {std::byte(0x34), std::byte(0x12), std::byte(0xFF), std::byte(0xFF), std::byte(0x00), std::byte(0x80)};
    REQUIRE_MESSAGE(DecodeWav(Wave(1, 16, 22050, samples16), pcm, error), error);
    CHECK(pcm.channels == 1);
    CHECK(pcm.rate == 22050);
    REQUIRE(pcm.samples.size() == 3);
    CHECK(pcm.samples[0] == 0x1234);
    CHECK(pcm.samples[1] == -1);
    CHECK(pcm.samples[2] == -32768);

    const std::vector<std::byte> samples8 = {std::byte(128), std::byte(255), std::byte(0), std::byte(129)};
    REQUIRE_MESSAGE(DecodeSound(Wave(2, 8, 11025, samples8), pcm, error), error);
    CHECK(pcm.channels == 2);
    CHECK(pcm.Frames() == 2);
    CHECK(pcm.samples[0] == 0);
    CHECK(pcm.samples[1] == 127 * 256);
    CHECK(pcm.samples[2] == -128 * 256);
    CHECK(pcm.samples[3] == 256);

    // An empty data chunk is a file of silence.
    REQUIRE_MESSAGE(DecodeWav(Wave(1, 16, 44100, {}), pcm, error), error);
    CHECK(pcm.Frames() == 0);
}

TEST_CASE("files that are not PCM WAVE or Vorbis are refused") {
    Pcm pcm;
    std::string error;
    std::vector<std::byte> adpcm = Wave(1, 16, 22050, {std::byte(0), std::byte(0)});
    adpcm[20] = std::byte(2); // the format tag: MS ADPCM
    CHECK_FALSE(DecodeWav(adpcm, pcm, error));
    CHECK(error.find("not PCM") != std::string::npos);
    const std::vector<std::byte> junk(64, std::byte(7));
    CHECK_FALSE(DecodeSound(junk, pcm, error));
    std::vector<std::byte> ogg = junk;
    std::memcpy(ogg.data(), "OggS", 4);
    CHECK_FALSE(DecodeSound(ogg, pcm, error));
    OggStream stream;
    CHECK_FALSE(stream.Open(ogg, error));
    std::int16_t out[8] = {};
    CHECK(stream.Read(out, 4, true) == 0);
}

TEST_CASE("effectsound.txt rows: by object and handle, by skill, events as filters") {
    const std::string text =
        "//\tobject\thandle\tskill_ID\tevent1\tevent2\tevent3\tblank\tfolder\tfilename\tvolume\tdescription1\t\r\n"
        "\tUI\tSND_BUTTON_CLICK\t-\t-\t-\t-\t0\tui\\\tuibutton_a.wav\t80\tclick\t\r\n"
        "\tUI\tSND_BUTTON_CLICK\t-\t-\t-\t-\t0\tui\\\tuibutton_b.wav\t80\tclick\t\r\n"
        "//\tUI\tSND_WARP\t-\t-\t-\t-\t0\tui\\\tGatein.wav\t80\tcommented out\t\r\n"
        "\tITEM\tSND_EQUIP\t-\tSWORD\t-\t-\t0\tui\\\titSword.wav\t80\t\t\r\n"
        "\tITEM\tSND_EQUIP\t-\tSHIELD\t-\t-\t0\tui\\\titShield.wav\t80\t\t\r\n"
        "\tPLAYER\tSND_DMG\t-\tSWORD\t-\tNORMAL\t0\tplayer\\\tbatSwordHit1N.wav\t80\t\t\r\n"
        "\tPLAYER\tSND_DMG\t-\tSWORD\t-\tCRITYCAL\t0\tplayer\\\tbatSwordCHit.wav\t100\t\t\r\n"
        "\tPLAYER\tSND_SWING1\tSKILL_CH_SWORD_BASE\t-\t-\t-\t0\tplayer\\\tbatSwordSwing1.wav\t80\t\t\r\n"
        "\tPLAYER\tSND_SWING4\t-\t-\t-\t-\t0\t-\t-\t-\t\t\r\n"
        "\tMOB_MANGNYANG\tVOC_DEATH\t-\t-\t-\t-\t0\tmonster\\\tcm_Mang_die.wav\t80\t\t\r\n"
        "\tPLAYER\tSND_DDMG\tSKILL_EU_WARLOCK_SOULA_RETURN_A\t-\t-\t-\t0\tskill2\\\tusk_warlock_blood_return_dmg_a.wav\t100\t\t\r\n";
    EffectSoundTable t;
    std::string error;
    REQUIRE_MESSAGE(t.Parse(text, error), error);
    CHECK(t.Size() == 10);
    CHECK(t.Find({"UI", "SND_BUTTON_CLICK"}).size() == 2);
    CHECK(t.Find({"UI", "SND_WARP"}).empty());
    const auto sword = t.Find({"ITEM", "SND_EQUIP", {}, {"SWORD"}});
    REQUIRE(sword.size() == 1);
    CHECK(sword[0]->path == "prim\\snd\\ui\\itSword.wav");
    CHECK(sword[0]->volume == doctest::Approx(0.8f));
    CHECK(t.Find({"PLAYER", "SND_DMG", {}, {"SWORD", {}, "CRITYCAL"}}).size() == 1);
    CHECK(t.Find({"PLAYER", "SND_DMG", {}, {"SWORD"}}).size() == 2);
    // A skill's rows only by the skill; the row without a file is never found.
    CHECK(t.Find({{}, "SND_SWING1", "SKILL_CH_SWORD_BASE"}).size() == 1);
    CHECK(t.Find({"PLAYER", "SND_SWING1"}).empty());
    CHECK(t.Find({"PLAYER", "SND_SWING4"}).empty());
    CHECK(t.HasSkill("SKILL_CH_SWORD_BASE"));
    CHECK(t.HasObject("MOB_MANGNYANG"));
    std::minstd_rand random;
    CHECK(t.Pick({"MOB_MANGNYANG", "VOC_DEATH"}, random) != nullptr);
    CHECK(t.Pick({"MOB_MANGNYANG", "VOC_SHOUT1"}, random) == nullptr);
    // A guarding skill's own hit sound (GameAudio::Guarded): by the skill.
    CHECK(t.Pick({"PLAYER", "SND_DDMG", "SKILL_EU_WARLOCK_SOULA_RETURN_A"}, random) != nullptr);
}

TEST_CASE("effectenvsnd.txt and regioninfo.txt: the music and ambience of the area a point is in") {
    const std::string sounds =
        "<1>\tTown\t\t\r\n"
        "\t\"Town.ogg\"\t\t\r\n"
        "\t<2>\tday\t\r\n"
        "\t\t\t<3>\t\"day_wind.wav\"\t0~0\t\r\n"
        "\t\t\t<3>\t\"day_bird01.wav\"  15~30\t\r\n"
        "\t<2>\tnight\t\r\n"
        "\t\t\t<3>\t\"night_wind.wav\"\t 0~0\r\n"
        "<1>\tField\r\n"
        "\t\"Field.ogg\"\r\n"
        "\t<2>\tday\r\n"
        "\t\t<3> \"a.wav\" 10~20\r\n"
        "\t<2>\tnight\r\n"
        "\t\t<3> \"b.wav\" 10~20\r\n";
    AreaSoundTable areas;
    std::string error;
    REQUIRE_MESSAGE(areas.Parse(sounds, error), error);
    REQUIRE(areas.Areas().size() == 2);
    const AreaSounds* town = areas.Find("Town");
    REQUIRE(town != nullptr);
    CHECK(town->music == "Town.ogg");
    REQUIRE(town->day.size() == 2);
    CHECK(town->day[0].path == "prim\\snd\\env\\day_wind.wav");
    CHECK(town->day[0].Loop());
    CHECK(town->day[1].minSeconds == 15.0f);
    CHECK(town->day[1].maxSeconds == 30.0f);
    REQUIRE(town->night.size() == 1);
    CHECK(areas.Find("Field")->night[0].path == "prim\\snd\\env\\b.wav");

    const std::string regions =
        "\r\n#TOWN\tTown\t\t\r\n"
        "10\t20\tALL\r\n"
        "11\t20\tRECT\t0\t1600\t960\t1920\r\n"
        "\r\n#FIELD\tField\r\n"
        "11\t20\tALL\r\n"
        "10\t20\tRECT\t0\t0\t100\t100\r\n"
        "#FIELD\tCave\tdonwhang\r\n"
        "1\t128\tALL\r\n";
    RegionAreaTable table;
    REQUIRE_MESSAGE(table.Parse(regions, error), error);
    CHECK(table.Areas().size() == 3);
    CHECK(table.Areas()[0].town);
    CHECK(table.Areas()[2].dungeon == "donwhang");
    const auto name = [&](int x, int y, float px, float pz) {
        const RegionAreaTable::Area* a = table.Find(std::uint16_t(x | y << 8), px, pz);
        return a != nullptr ? a->name : std::string("-");
    };
    CHECK(name(10, 20, 500, 500) == "Town");
    CHECK(name(10, 20, 50, 50) == "Field");     // a part cut out of the town
    CHECK(name(11, 20, 500, 1700) == "Town");   // the town's part of the field's region
    CHECK(name(11, 20, 1500, 1700) == "Field");
    CHECK(name(1, 128, 0, 0) == "Cave");        // a dungeon (region 0x8001)
    CHECK(name(12, 20, 0, 0) == "-");
}
