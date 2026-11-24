#include "sr/game/QuestItems.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace sr::game {

namespace {

struct Row {
    std::string_view item;
    QuestItemUse use;
};

constexpr QuestItemTaker Q = QuestItemTaker::Quest;

// The quest items' uses: the places and numbers of each quest's own use of its items and of its countdown.
const Row kRows[] = {
    {"ITEM_QNO_CA_GORIA_7_03", {Q, "QNO_CA_GORIA_7", "instance",
                                "the Big Drum at region 26991 (1154, 142, 1506) within 700: its auto-share party's master takes the party into INS_QNO_CA_GORIA_7 (talks _11.._13; the drum itself refused 2)", true}},
    {"ITEM_QNO_CA_TREASURE_5_01", {Q, "QNO_CA_TREASURE_5", "spawn",
                                   "the key at region 26219 (1525, 152, 1495) within 400: MOB_QT_01_ONG within 20..100, gone after 5 minutes (talk _17)", true}},
    {"ITEM_QNO_CA_WAREHOUSE_1_02", {Q, "QNO_CA_WAREHOUSE_1", "search",
                                    "a ten-second search at region 27237 (1057, 218, 634) within 1000: 68 in 101 a Blue Thorn Grass (_01), under 10 held", true}},
    {"ITEM_QNO_CA_WAREHOUSE_3_01", {Q, "QNO_CA_WAREHOUSE_3", "search",
                                    "a ten-second search at region 26219 (1525, 152, 1495) within 500: 51 in 101 a _02, under 20 held", true}},
    {"ITEM_QNO_CH_HWAN_1_3_02", {Q, "QNO_CH_HWAN_1_3_1 QNO_EU_HWAN_1_3_1", "spawn",
                                 "the Spirit's Bell at region 23980 (706, 230, 1218) within 100: MOB_QT_CH_TOMBSTONE_01 within 90..100; its kill swaps the bells for a Spirit's Shell (_03)", true}},
    {"ITEM_QNO_CH_WHAN_2_3_01", {Q, "QNO_CH_HWAN_2_3 QNO_EU_HWAN_2_3", "target",
                                 "on the selected MOB_CH_YEOHA (or _CLON) within 300: one _02, none held before (talk _09)", true}},
    {"ITEM_QNO_CH_WHAN_2_4_01", {Q, "QNO_CH_HWAN_2_4 QNO_EU_HWAN_2_4", "search",
                                 "a ten-second search at region 25954 (1672, 180, 232) within 300: 11 in 101 the Relic of Sun God (_02), none held", true}},
    {"ITEM_QNO_EU_ADVENTURER_1_01", {Q, "QNO_EU_ADVENTURER_1", "spawn",
                                     "the shiny moss at region 26714 (986, 20, 340) within 420: MOB_QT_01_CRAB within 20..100 (talks _08, _09)", true}},
    {"ITEM_QNO_EU_EASTEU_11_02", {Q, "QNO_EU_EASTEU_11_1", "capture",
                                  "the wind pouch on the selected MOB_EU_LADON within 300, 50 in 101 (+3 a level above the COS): MOB_QT_01_LADON_COS for 20 minutes (talks _09.._11, _16.._20)", true}},
    {"ITEM_QNO_EU_EASTEU_17_1_01", {Q, "QNO_EU_EASTEU_17_1", "capture",
                                    "the remedy of sound on the selected MOB_EU_SIREN_CLON within 300, 50 in 101 (+3 a level above): MOB_QT_01_SIREN_CLON_COS for 30 minutes (talks _09.._11, _16.._20)", true}},
    {"ITEM_QNO_EU_EASTEU_19_01", {Q, "QNO_EU_EASTEU_19", "spawn",
                                  "the Golden Apple at region 25420 (126, -41, 1863) within 400, under 20 _02: MOB_QT_01_LADON within 20..100 (talk _13)", true}},
    {"ITEM_QNO_EU_EASTEU_19_03", {Q, "QNO_EU_EASTEU_19", "search",
                                  "the Long Scissors' ten seconds at region 25420 (126, -41, 1863) within 400: a Golden Apple (_01), under 20 held", true}},
    {"ITEM_QNO_EU_EASTEU_21_01", {Q, "QNO_EU_EASTEU_21", "instance",
                                  "at region 24902 (886, 854, 8) within 600: its auto-share party's master takes the party into INS_QNO_EU_EASTEU_21 (Cerberus; talks _08.._10)", true}},
    {"ITEM_QNO_EU_EASTEU_3_02", {Q, "QNO_EU_EASTEU_1", "search",
                                 "the holy water's ten seconds at region 27211 (249, 24, 759) within 200: 51 in 101 a Stable Filth (EASTEU_1_02), under 5 held",
                                 true}},
    {"ITEM_QNO_EU_EASTEU_5_01", {Q, "QNO_EU_EASTEU_5", "capture",
                                 "the Chaos Medicine on the selected MOB_EU_DOWB within 300, 50 in 101 (+3 a level above): MOB_QT_01_DOWB_COS for 20 minutes (talks _12.._14, _19.._23)", true}},
    {"ITEM_QNO_EU_GENERAL_2_01", {Q, "QNO_EU_GENERAL_2", "spawn",
                                  "the pirate's flag in an auto-share party at region 25690 (773, 124, 110) within 700: one MOB_QT_01_ROGUE and four MOB_QT_01_ROGUE_CLON within 20..100, gone after 5 minutes (talks _08, _09)", true}},
    {"ITEM_QNO_EU_IVY_2_03", {Q, "QNO_EU_IVY_2", "search",
                              "the small knife's ten seconds at region 25950 (253, 85, 449) within 600: 51 in 101 a vine (_02), under 20 held", true}},
    {"ITEM_QNO_EU_IVY_3_01", {Q, "QNO_EU_IVY_3", "spawn",
                              "the Horn Trumpet in an auto-share party (_26) at the first of three sites whose sign is not held (region 27237, "
                              "25954, 25187; else _25): its keeper (MOB_QT_01_PUNISHER, MOB_QT_03_PUNISHER_CLON, MOB_QT_01_SOLDIER_CLON) 20..100 "
                              "away, gone after 5 minutes; its kill drops the sign", true}},
    {"ITEM_QNO_EU_IVY_3_05", {Q, "QNO_EU_IVY_3", "make",
                              "the Punisher's Blood with the three signs held and a free place: the Key of Sign (_06), one of each sign taken", true}},
    {"ITEM_QNO_EU_IVY_3_06", {Q, "QNO_EU_IVY_3", "instance",
                              "the Key of Sign at region 25183 (1882, -169, 632) within 600: its auto-share party's master takes the party into INS_QNO_EU_IVY_3 (talks _08.._10)", true}},
    {"ITEM_QNO_EU_WITCH_1_02", {Q, "QNO_EU_WITCH_1", "roll",
                                "the Blood of Evil Wolf: 1 in 2 an Unopening Box (GENERAL_5_01) becomes a Curse Lifted Box (_01), under 60 held; else talk _09", true}},
    {"ITEM_QNO_RM_FLYSHIP1_3_01", {Q, "QNO_RM_FLYSHIP1_3", "capture",
                                   "the Goat Feed on the selected MOB_QT_01_ANTELOPE_CLON, 50 in 101 (-3 a level the COS is above): MOB_QT_01_ANTELOPE_CLON_COS for 50 minutes (talks _09, _10, _12.._18)", true}},
    {"ITEM_QNO_RM_OLDWOMAN_5_08", {Q, "QNO_RM_OLDWOMAN_5", "stages",
                                   "seven places in turn, each its wait and chance (5 s 26 in 101 .. 40 s always): an ITEM_QNO_RM_OLDWOMAN_7_03, under 7 held", true}},
    {"ITEM_QNO_TQ_BAEKRYEONG_4_01", {Q, "QNO_TQ_BAEKRYOUNG_4", "capture",
                                     "the Powder of Dreams and Visions on the selected MOB_TQ_SNAKEMAN_CLON within 300, 50 in 101 (+3 a level above): MOB_QT_TQ_SNAKEMAN_CLON_COS for 60 minutes (talks _12.._15, _17.._20)", true}},
    {"ITEM_QNO_TQ_FINDTHIEF_2_01", {Q, "QNO_TQ_FINDTHIEF_2", "capture",
                                    "Hwajung's Rope on the selected MOB_QT_TQ_THIEF_01 within 300, 50 in 101 (+3 a level above): MOB_QT_TQ_THIEF_01_COS for 20 minutes (talks _12.._20)", true}},
    {"ITEM_QNO_TQ_ORNAMENT_3_01", {Q, "QNO_TQ_ORNAMENT_3", "search",
                                   "a ten-second search at one of four places of the tomb (region 32773) within 300: 51 in 101 a _02, under 10 held", true}},
    {"ITEM_QNO_TQ_ORNAMENT_4_04", {Q, "QNO_TQ_ORNAMENT_4", "instance",
                                   "the Explosive Stone with the calabash, at region 32773 (184, 72, 157) within 2000: its auto-share party's master takes the "
                                   "party into INS_QNO_TQ_ORNAMENT_4, the four snake generals in turn (talks _21.._23)", true}},
    {"ITEM_QNO_TQ_STONEBOARD_4_03", {Q, "QNO_TQ_STONEBOARD_4", "instance",
                                     "the Fake Head of Hyun Lang with the tablet, at one of two places of region 32775 within 600: its auto-share party's master "
                                     "takes the party into INS_QNO_TQ_STONEBOARD_4, the four tomb generals in turn (talks _23.._25)", true}},
    {"ITEM_QNO_WC_WAREHOUSE_W_2_01", {Q, "QNO_WC_WAREHOUSE_W_2 QNO_WC_WAREHOUSE_W_3", "coin",
                                      "a coin's toss: a Rarity Charm (_02) or a Bad Charm (_03) while under 70 (W_3: 130) _02 are held; at state 8 given but the amulet kept (the original's quirk)", true}},
    {"ITEM_ETC_E060526_SUMMON_PARTY_SCROLL_A", {QuestItemTaker::GuildSupport, "QEV_GUILD_SUPPORT", "event",
                                                "a strong monster of the user's level for each of its party's members, 20..100 away"}},
    {"ITEM_ETC_E060517_MON_GENERATION_BOX", {QuestItemTaker::Pandora, "QEV_PANDORA_BOX_100LV", "event",
                                             "the Pandora box; for 20 minutes, four strong monsters around it whenever none stands, the box's cast after each"}},
};

// The ground searches as rules (each handler's checks in its order; the countdown's handler's roll).
const std::vector<OriginalUseRule> kRules = {
    // Its use and its countdown (rand % 101 <= 67).
    {"QNO_CA_WAREHOUSE_1",
     "on use ITEM_QNO_CA_WAREHOUSE_1_02 status 1 7 first-time refuse @UIIT_MSG_QUEST_ERR_CANNOT_USE_ITEM cap-first at 27237 1057 218 634 1000 "
     "away @SN_TALK_QNO_CA_WAREHOUSE_1_08 cap 10 ITEM_QNO_CA_WAREHOUSE_1_01 full @SN_TALK_QNO_CA_WAREHOUSE_1_10 wait 10 chance 68 of 101 "
     "give ITEM_QNO_CA_WAREHOUSE_1_01 failed @SN_TALK_QNO_CA_WAREHOUSE_1_09"},
    // Its use and its countdown (rand % 101 >= 50).
    {"QNO_CA_WAREHOUSE_3",
     "on use ITEM_QNO_CA_WAREHOUSE_3_01 status 1 7 8 at 26219 1525 152 1495 500 away @SN_TALK_QNO_CA_WAREHOUSE_3_10 cap 20 "
     "ITEM_QNO_CA_WAREHOUSE_3_02 full @SN_TALK_QNO_CA_WAREHOUSE_3_11 wait 10 chance 51 of 101 give ITEM_QNO_CA_WAREHOUSE_3_02 "
     "failed @SN_TALK_QNO_CA_WAREHOUSE_3_08"},
    // Its use and its countdown (rand % 101 <= 50).
    {"QNO_EU_EASTEU_1",
     "on use ITEM_QNO_EU_EASTEU_3_02 status 1 7 8 one-at-a-time @SN_TALK_QNO_EU_EASTEU_3_13 refuse @SN_TALK_QNO_EU_EASTEU_3_13 "
     "at 27211 249 24 759 200 away @SN_TALK_QNO_EU_EASTEU_1_09 cap 5 ITEM_QNO_EU_EASTEU_1_02 @SN_TALK_QNO_EU_EASTEU_1_11 "
     "full @SN_TALK_QNO_EU_EASTEU_1_10 wait 10 chance 51 of 101 give ITEM_QNO_EU_EASTEU_1_02 failed @SN_TALK_QNO_EU_EASTEU_1_12"},
    // (the Long Scissors), its countdown (no roll).
    {"QNO_EU_EASTEU_19",
     "on use ITEM_QNO_EU_EASTEU_19_03 status 1 7 8 refuse @UIIT_MSG_QUEST_ERR_CANNOT_USE_ITEM at 25420 126 -41 1863 400 "
     "away @SN_TALK_QNO_EU_EASTEU_19_12 cap 20 ITEM_QNO_EU_EASTEU_19_01 @UIIT_MSG_QUEST_ERR_CANNOT_USE_ITEM full @SN_TALK_QNO_EU_EASTEU_19_11 "
     "wait 10 give ITEM_QNO_EU_EASTEU_19_01"},
    // Its use and its countdown (rand % 101 >= 50).
    {"QNO_EU_IVY_2",
     "on use ITEM_QNO_EU_IVY_2_03 status 1 7 one-at-a-time refuse @UIIT_MSG_QUEST_ERR_CANNOT_USE_ITEM at 25950 253 85 449 600 "
     "away @SN_TALK_QNO_EU_IVY_2_09 cap 20 ITEM_QNO_EU_IVY_2_02 full @SN_TALK_QNO_EU_IVY_2_15 wait 10 chance 51 of 101 "
     "give ITEM_QNO_EU_IVY_2_02 failed @SN_TALK_QNO_EU_IVY_2_10"},
    // Its use and its countdown (rand % 101 <= 50); the tomb's four places.
    {"QNO_TQ_ORNAMENT_3",
     "on use ITEM_QNO_TQ_ORNAMENT_3_01 status 1 7 first-time refuse @UIIT_MSG_QUEST_ERR_CANNOT_USE_ITEM cap-first "
     "at -32763 -16604 1150 9113 300 at -32763 8497 1171 16664 300 at -32763 16754 1163 -8286 300 at -32763 -8440 1204 -17074 300 "
     "away @SN_TALK_QNO_TQ_ORNAMENT_3_12 cap 10 ITEM_QNO_TQ_ORNAMENT_3_02 full @SN_TALK_QNO_TQ_ORNAMENT_3_14 wait 10 chance 51 of 101 "
     "give ITEM_QNO_TQ_ORNAMENT_3_02 failed @SN_TALK_QNO_TQ_ORNAMENT_3_13"},
    // Its use and its countdown (rand % 101 <= 10); the place as whole numbers.
    {"QNO_CH_HWAN_2_4",
     "on use ITEM_QNO_CH_WHAN_2_4_01 status 7 8 at 25954 1672 180 232 300 away @SN_TALK_QNO_CH_HWAN_2_4_09 cap 1 ITEM_QNO_CH_WHAN_2_4_02 "
     "@SN_TALK_QNO_CH_HWAN_2_4_09 wait 10 chance 11 of 101 give ITEM_QNO_CH_WHAN_2_4_02 failed @SN_TALK_QNO_CH_HWAN_2_4_10 "
     "got @SN_TALK_QNO_CH_HWAN_2_4_08"},
    // Its use and its countdown the same, its own words.
    {"QNO_EU_HWAN_2_4",
     "on use ITEM_QNO_CH_WHAN_2_4_01 status 7 8 at 25954 1672 180 232 300 away @SN_TALK_QNO_EU_HWAN_2_4_09 cap 1 ITEM_QNO_CH_WHAN_2_4_02 "
     "@SN_TALK_QNO_EU_HWAN_2_4_09 wait 10 chance 11 of 101 give ITEM_QNO_CH_WHAN_2_4_02 failed @SN_TALK_QNO_EU_HWAN_2_4_10 "
     "got @SN_TALK_QNO_EU_HWAN_2_4_08"},

    // --- The spawns (the quest service's spawn a radius 20 + 80 x rand/32767 about the player, banded) --
    // The Golden Apple's branch states 1/7/8 then not 8 (CANNOT_USE), under
    // 20 _02 (CANNOT_USE), the scissors' place (_13).
    {"QNO_EU_EASTEU_19",
     "on use ITEM_QNO_EU_EASTEU_19_01 status 1 7 refuse @UIIT_MSG_QUEST_ERR_CANNOT_USE_ITEM cap-first cap 20 ITEM_QNO_EU_EASTEU_19_02 "
     "@UIIT_MSG_QUEST_ERR_CANNOT_USE_ITEM at 25420 126 -41 1863 400 away @SN_TALK_QNO_EU_EASTEU_19_13 spawn MOB_QT_01_LADON 1 radius 20 100"},
    // states 7/1 (CANNOT_USE), the place (_17); its timer 0x493E0 ms on the monster
    // (gone without dying).
    {"QNO_CA_TREASURE_5",
     "on use ITEM_QNO_CA_TREASURE_5_01 status 1 7 refuse @UIIT_MSG_QUEST_ERR_CANNOT_USE_ITEM at 26219 1525 152 1495 400 "
     "away @SN_TALK_QNO_CA_TREASURE_5_17 spawn MOB_QT_01_ONG 1 radius 20 100 life 300"},
    // state 8 (_09), else 7/1 (silent), the place (_08).
    {"QNO_EU_ADVENTURER_1",
     "on use ITEM_QNO_EU_ADVENTURER_1_01 status 1 7 met @SN_TALK_QNO_EU_ADVENTURER_1_09 at 26714 986 20 340 420 "
     "away @SN_TALK_QNO_EU_ADVENTURER_1_08 spawn MOB_QT_01_CRAB 1 radius 20 100"},
    // states 7/1 (silent), the quest's party flag with an exp and item auto-share party (_08), the place (_09); one
    // rogue, then four clones, each with the five-minute timer.
    {"QNO_EU_GENERAL_2",
     "on use ITEM_QNO_EU_GENERAL_2_01 status 1 7 party @SN_TALK_QNO_EU_GENERAL_2_08 at 25690 773 124 110 700 away @SN_TALK_QNO_EU_GENERAL_2_09 "
     "spawn MOB_QT_01_ROGUE 1 radius 20 100 life 300 spawn MOB_QT_01_ROGUE_CLON 4 radius 20 100 life 300"},
    // states 7/1, never finished (both CANNOT_USE), no _03 held (silent, asked before and after
    // the place), the place (_10); the radius 90 + 10 x rand. Its kill handler (the tombstone's
    // kill with no _03 held takes every bell, gives a shell, says _09) is the quest's `on kill` rule (QuestDef::onKill).
    {"QNO_CH_HWAN_1_3_1",
     "on use ITEM_QNO_CH_HWAN_1_3_02 status 1 7 first-time refuse @UIIT_MSG_QUEST_ERR_CANNOT_USE_ITEM cap-first cap 1 ITEM_QNO_CH_HWAN_1_3_03 "
     "at 23980 706 230 1218 100 away @SN_TALK_QNO_CH_HWAN_1_3_1_10 spawn MOB_QT_CH_TOMBSTONE_01 1 radius 90 100"},
    // (its kill handler the same): its own words.
    {"QNO_EU_HWAN_1_3_1",
     "on use ITEM_QNO_CH_HWAN_1_3_02 status 1 7 first-time refuse @UIIT_MSG_QUEST_ERR_CANNOT_USE_ITEM cap-first cap 1 ITEM_QNO_CH_HWAN_1_3_03 "
     "at 23980 706 230 1218 100 away @SN_TALK_QNO_EU_HWAN_1_3_1_10 spawn MOB_QT_CH_TOMBSTONE_01 1 radius 90 100"},

    // --- The captures (their COS by its RefObjID, its level from the reference, its tactics; a minute's tick, the
    // COS's events). Caught: rand() % 101 below 50, plus 3 a level the player stands above the COS; the monster gone
    // without dying, the COS in its place. Its use (0x14 minutes), its tick, its events.
    {"QNO_EU_EASTEU_5",
     "on use ITEM_QNO_EU_EASTEU_5_01 status 1 7 target MOB_EU_DOWB within 300 none @UIIT_SKILL_USE_FAIL_NONESELECTETARGET "
     "far @UIIT_MSG_QUEST_ERR_TOO_FAR_FROM_CATCH_MONSTER wrong @SN_TALK_QNO_EU_EASTEU_5_23 failed @SN_TALK_QNO_EU_EASTEU_5_22 "
     "capture MOB_QT_01_DOWB_COS minutes 20 roll 50 step 3 already @SN_TALK_QNO_EU_EASTEU_5_21 warn 10 @SN_TALK_QNO_EU_EASTEU_5_19 "
     "warn 5 @SN_TALK_QNO_EU_EASTEU_5_20 ran @SN_TALK_QNO_EU_EASTEU_5_12 owner-died @SN_TALK_QNO_EU_EASTEU_5_13 cos-died @SN_TALK_QNO_EU_EASTEU_5_14"},
    // Its use, its tick, its events.
    {"QNO_EU_EASTEU_11_1",
     "on use ITEM_QNO_EU_EASTEU_11_02 status 1 7 target MOB_EU_LADON within 300 none @UIIT_SKILL_USE_FAIL_NONESELECTETARGET "
     "far @UIIT_MSG_QUEST_ERR_TOO_FAR_FROM_CATCH_MONSTER wrong @SN_TALK_QNO_EU_EASTEU_11_1_20 failed @SN_TALK_QNO_EU_EASTEU_11_1_19 "
     "capture MOB_QT_01_LADON_COS minutes 20 roll 50 step 3 already @SN_TALK_QNO_EU_EASTEU_11_1_18 warn 10 @SN_TALK_QNO_EU_EASTEU_11_1_16 "
     "warn 5 @SN_TALK_QNO_EU_EASTEU_11_1_17 ran @SN_TALK_QNO_EU_EASTEU_11_1_09 owner-died @SN_TALK_QNO_EU_EASTEU_11_1_10 "
     "cos-died @SN_TALK_QNO_EU_EASTEU_11_1_11"},
    // (0x1E minutes), its tick, its events.
    {"QNO_EU_EASTEU_17_1",
     "on use ITEM_QNO_EU_EASTEU_17_1_01 status 1 7 target MOB_EU_SIREN_CLON within 300 none @UIIT_SKILL_USE_FAIL_NONESELECTETARGET "
     "far @UIIT_MSG_QUEST_ERR_TOO_FAR_FROM_CATCH_MONSTER wrong @SN_TALK_QNO_EU_EASTEU_17_1_20 failed @SN_TALK_QNO_EU_EASTEU_17_1_19 "
     "capture MOB_QT_01_SIREN_CLON_COS minutes 30 roll 50 step 3 already @SN_TALK_QNO_EU_EASTEU_17_1_18 warn 10 @SN_TALK_QNO_EU_EASTEU_17_1_16 "
     "warn 5 @SN_TALK_QNO_EU_EASTEU_17_1_17 ran @SN_TALK_QNO_EU_EASTEU_17_1_09 owner-died @SN_TALK_QNO_EU_EASTEU_17_1_10 "
     "cos-died @SN_TALK_QNO_EU_EASTEU_17_1_11"},
    // Its use, its tick, its events.
    {"QNO_TQ_FINDTHIEF_2",
     "on use ITEM_QNO_TQ_FINDTHIEF_2_01 status 1 7 target MOB_QT_TQ_THIEF_01 within 300 none @UIIT_SKILL_USE_FAIL_NONESELECTETARGET "
     "far @UIIT_MSG_QUEST_ERR_TOO_FAR_FROM_CATCH_MONSTER wrong @SN_TALK_QNO_TQ_FINDTHIEF_2_20 failed @SN_TALK_QNO_TQ_FINDTHIEF_2_15 "
     "capture MOB_QT_TQ_THIEF_01_COS minutes 20 roll 50 step 3 already @SN_TALK_QNO_TQ_FINDTHIEF_2_18 warn 10 @SN_TALK_QNO_TQ_FINDTHIEF_2_16 "
     "warn 5 @SN_TALK_QNO_TQ_FINDTHIEF_2_17 ran @SN_TALK_QNO_TQ_FINDTHIEF_2_12 owner-died @SN_TALK_QNO_TQ_FINDTHIEF_2_13 "
     "cos-died @SN_TALK_QNO_TQ_FINDTHIEF_2_14"},
    // (0x3C minutes), its tick, its events.
    {"QNO_TQ_BAEKRYOUNG_4",
     "on use ITEM_QNO_TQ_BAEKRYEONG_4_01 status 1 7 target MOB_TQ_SNAKEMAN_CLON within 300 none @UIIT_SKILL_USE_FAIL_NONESELECTETARGET "
     "far @UIIT_MSG_QUEST_ERR_TOO_FAR_FROM_CATCH_MONSTER wrong @SN_TALK_QNO_TQ_BAEKRYEONG_4_18 failed @SN_TALK_QNO_TQ_BAEKRYEONG_4_12 "
     "capture MOB_QT_TQ_SNAKEMAN_CLON_COS minutes 60 roll 50 step 3 already @SN_TALK_QNO_TQ_BAEKRYEONG_4_17 "
     "warn 10 @SN_TALK_QNO_TQ_BAEKRYEONG_4_19 warn 5 @SN_TALK_QNO_TQ_BAEKRYEONG_4_20 ran @SN_TALK_QNO_TQ_BAEKRYEONG_4_13 "
     "owner-died @SN_TALK_QNO_TQ_BAEKRYEONG_4_14 cos-died @SN_TALK_QNO_TQ_BAEKRYEONG_4_15"},
    // (0x32 minutes): no reach asked, the selected one must be the antelope (_18
    // with none selected too); the roll the other way, less 3 a level the COS stands above the
    // player; caught it says _09.
    {"QNO_RM_FLYSHIP1_3",
     "on use ITEM_QNO_RM_FLYSHIP1_3_01 status 1 7 target MOB_QT_01_ANTELOPE_CLON wrong @SN_TALK_QNO_RM_FLYSHIP1_3_18 "
     "failed @SN_TALK_QNO_RM_FLYSHIP1_3_16 got @SN_TALK_QNO_RM_FLYSHIP1_3_09 capture MOB_QT_01_ANTELOPE_CLON_COS minutes 50 roll 50 step 3 below "
     "already @SN_TALK_QNO_RM_FLYSHIP1_3_13 warn 10 @SN_TALK_QNO_RM_FLYSHIP1_3_14 warn 5 @SN_TALK_QNO_RM_FLYSHIP1_3_15 "
     "ran @SN_TALK_QNO_RM_FLYSHIP1_3_10 owner-died @SN_TALK_QNO_RM_FLYSHIP1_3_17 cos-died @SN_TALK_QNO_RM_FLYSHIP1_3_12"},
    // --- The traps set from the bag (a 3/3/13/1 item whose skill has 'trap' and 'qest': Skills::SetTrap). The trap's catch of
    // its monster is the quest's event 0x1D; the
    // catch is an item, kept for its minutes (the record's byte +4, word +0xA 10). None of these setups makes a COS record:
    // its level 0, so the roll is 50 in 101 whatever the levels.
    // states 7/1 (silent), MOB_QT_01_HUNARCHER_CLON (silent), a bag place (_13), one held
    // (byte +4 above 0, _18), the roll (_20): a Captured Hun Tribe, the trapped monster out. Its minute (_16 at 10, _17
    // at 5; at 0 every _02 taken, _15); its holder's death with a _02 held: every one taken, _14, and the minutes left
    // at 5 unmarked (_15 again when they run out, the trap's catch refused meanwhile: the original's quirk, kept).
    {"QNO_CA_GORIA_6",
     "on use ITEM_QNO_CA_GORIA_6_01 trap status 1 7 target MOB_QT_01_HUNARCHER_CLON full @SN_TALK_QNO_CA_GORIA_6_13 failed @SN_TALK_QNO_CA_GORIA_6_20 "
     "capture-item ITEM_QNO_CA_GORIA_6_02 minutes 20 roll 50 step 3 below died-wait 5 already @SN_TALK_QNO_CA_GORIA_6_18 "
     "warn 10 @SN_TALK_QNO_CA_GORIA_6_16 warn 5 @SN_TALK_QNO_CA_GORIA_6_17 ran @SN_TALK_QNO_CA_GORIA_6_15 owner-died @SN_TALK_QNO_CA_GORIA_6_14"},
    // The hwan tests' 1_4_1 (CH, EU; ITEM_QNO_CH_HWAN_1_4_04, 3/3/13/1, 100 given at the
    // start): states 7/1, MOB_TK_BONEGENERAL, a bag place (_10); one rand() % 101: under 2 the Normal Spirit (_01, said _08: the
    // goal), under 30 the Flutter Spirit (_02, _14: a helper), else _09; the general stays in the world either way.
    {"QNO_CH_HWAN_1_4_1",
     "on use ITEM_QNO_CH_HWAN_1_4_04 trap status 1 7 target MOB_TK_BONEGENERAL full @SN_TALK_QNO_CH_HWAN_1_4_1_10 failed @SN_TALK_QNO_CH_HWAN_1_4_1_09 "
     "capture-outcomes outcome ITEM_QNO_CH_HWAN_1_4_01 2 @SN_TALK_QNO_CH_HWAN_1_4_1_08 outcome ITEM_QNO_CH_HWAN_1_4_02 30 @SN_TALK_QNO_CH_HWAN_1_4_1_14 stays"},
    {"QNO_EU_HWAN_1_4_1",
     "on use ITEM_QNO_CH_HWAN_1_4_04 trap status 1 7 target MOB_TK_BONEGENERAL full @SN_TALK_QNO_EU_HWAN_1_4_1_10 failed @SN_TALK_QNO_EU_HWAN_1_4_1_09 "
     "capture-outcomes outcome ITEM_QNO_CH_HWAN_1_4_01 2 @SN_TALK_QNO_EU_HWAN_1_4_1_08 outcome ITEM_QNO_CH_HWAN_1_4_02 30 @SN_TALK_QNO_EU_HWAN_1_4_1_14 stays"},
    // (the net): states 7/1, MOB_AM_ROGUE, a bag place (_12), no "one held" asked (a second
    // catch gives another and its 20 minutes again), the roll (_19): a Captured Pirate. Its minute (_15, _16; at 0 _14),
    // its holder's death (_13).
    {"QNO_EU_GENERAL_1",
     "on use ITEM_QNO_EU_GENERAL_1_01 trap status 1 7 target MOB_AM_ROGUE full @SN_TALK_QNO_EU_GENERAL_1_12 failed @SN_TALK_QNO_EU_GENERAL_1_19 "
     "capture-item ITEM_QNO_EU_GENERAL_1_02 minutes 20 roll 50 step 3 below warn 10 @SN_TALK_QNO_EU_GENERAL_1_15 warn 5 @SN_TALK_QNO_EU_GENERAL_1_16 "
     "ran @SN_TALK_QNO_EU_GENERAL_1_14 owner-died @SN_TALK_QNO_EU_GENERAL_1_13"},
    // (IVY_2's trap, SKILL_QNO_EU_IVY_2_01_01): states 7/1, MOB_QT_01_PUNISHER_CLON (the Closier
    // Informant), a bag place (_08), one held (_13), the roll (_17): the informant (_01) for 30 minutes, _07. Its minute
    // first a guardian (MOB_QT_02_PUNISHER_CLON) raised within 20 x rand/32767 of
    // its holder unless in a town, then the count (_11, _12; at 0 every _01 taken, _10); its holder's
    // death (_10).
    // Nemea's Lion in Demetri's trap (QNO_EU_EASTEU_14_1): on a
    // lion (MOB_EU_LION_CLON): states 7/1, a bag place (_10), one held already (_18), the roll rand % 101 under 50 less 3 a level
    // above: the lion as an item (_14_1_02, _08) for 20 minutes, else _20; its minute
    // (_16 at 10, _17 at 5, _15 the trap opened), its holder's death (_11).
    {"QNO_EU_EASTEU_14_1",
     "on use ITEM_QNO_EU_EASTEU_14_02 trap status 1 7 target MOB_EU_LION_CLON full @SN_TALK_QNO_EU_EASTEU_14_1_10 "
     "failed @SN_TALK_QNO_EU_EASTEU_14_1_20 got @SN_TALK_QNO_EU_EASTEU_14_1_08 capture-item ITEM_QNO_EU_EASTEU_14_1_02 minutes 20 "
     "roll 50 step 3 below already @SN_TALK_QNO_EU_EASTEU_14_1_18 warn 10 @SN_TALK_QNO_EU_EASTEU_14_1_16 "
     "warn 5 @SN_TALK_QNO_EU_EASTEU_14_1_17 ran @SN_TALK_QNO_EU_EASTEU_14_1_15 owner-died @SN_TALK_QNO_EU_EASTEU_14_1_11"},
    {"QNO_EU_IVY_1",
     "on use ITEM_QNO_EU_IVY_2_01 trap status 1 7 target MOB_QT_01_PUNISHER_CLON full @SN_TALK_QNO_EU_IVY_1_08 failed @SN_TALK_QNO_EU_IVY_1_17 "
     "got @SN_TALK_QNO_EU_IVY_1_07 capture-item ITEM_QNO_EU_IVY_1_01 minutes 30 roll 50 step 3 below raise MOB_QT_02_PUNISHER_CLON 0 20 "
     "already @SN_TALK_QNO_EU_IVY_1_13 warn 10 @SN_TALK_QNO_EU_IVY_1_11 warn 5 @SN_TALK_QNO_EU_IVY_1_12 ran @SN_TALK_QNO_EU_IVY_1_10 "
     "owner-died @SN_TALK_QNO_EU_IVY_1_10"},
    // no _02 held (silent), the selection (NONESELECTETARGET),
    // within 300 (silent), states 7/1 (silent), a yeoha (_09): one _02; the monster untouched.
    {"QNO_CH_HWAN_2_3",
     "on use ITEM_QNO_CH_WHAN_2_3_01 status 1 7 cap-first cap 1 ITEM_QNO_CH_WHAN_2_3_02 target MOB_CH_YEOHA_CLON MOB_CH_YEOHA within 300 "
     "none @UIIT_SKILL_USE_FAIL_NONESELECTETARGET wrong @SN_TALK_QNO_CH_HWAN_2_3_09 give ITEM_QNO_CH_WHAN_2_3_02"},
    {"QNO_EU_HWAN_2_3",
     "on use ITEM_QNO_CH_WHAN_2_3_01 status 1 7 cap-first cap 1 ITEM_QNO_CH_WHAN_2_3_02 target MOB_CH_YEOHA_CLON MOB_CH_YEOHA within 300 "
     "none @UIIT_SKILL_USE_FAIL_NONESELECTETARGET wrong @SN_TALK_QNO_EU_HWAN_2_3_09 give ITEM_QNO_CH_WHAN_2_3_02"},

    // --- The old woman's seven stages (its table; the countdown
    // rand % 101 <= 25, 30, 40, 50, 60, 75, 100). The stage is the record's step less one; elsewhere or at 7
    // held _17, no bag place _16; the step goes on after a won roll even when the grant fails.
    {"QNO_RM_OLDWOMAN_5",
     "on use ITEM_QNO_RM_OLDWOMAN_5_08 status 7 8 stage 24180 1081 5775 1631 270 5 26 stage 24427 1282 5590 285 280 10 31 "
     "stage 23664 44 4767 1898 300 15 41 stage 23153 1825 4689 1871 270 20 51 stage 22893 1322 4378 1761 270 25 61 "
     "stage 22132 1023 5785 1845 270 30 76 stage 22379 1013 5910 85 280 40 101 away @SN_TALK_QNO_RM_OLDWOMAN_5_17 "
     "cap 7 ITEM_QNO_RM_OLDWOMAN_7_03 @SN_TALK_QNO_RM_OLDWOMAN_5_17 full @SN_TALK_QNO_RM_OLDWOMAN_5_16 give ITEM_QNO_RM_OLDWOMAN_7_03 "
     "failed @SN_TALK_QNO_RM_OLDWOMAN_5_18"},
    // --- The witch's roll: states 7/1 (silent), a bag place (_10); rand % 2:
    // 0 the box's curse lifted (under 60 _01 held, one GENERAL_5_01 taken for one _01), 1 _09. Used either way.
    {"QNO_EU_WITCH_1",
     "on use ITEM_QNO_EU_WITCH_1_02 status 1 7 full @SN_TALK_QNO_EU_WITCH_1_10 chance 1 of 2 limit 60 ITEM_QNO_EU_WITCH_1_01 "
     "take ITEM_QNO_EU_GENERAL_5_01 give ITEM_QNO_EU_WITCH_1_01 failed @SN_TALK_QNO_EU_WITCH_1_09"},
    // --- The amulet's coin (_W_3): states 7/8 (silent); rand % 2: 0 a
    // Rarity Charm (_02), 1 a Bad Charm (_03), both while under 70 (W_3: 130) _02 held (else refused, kept); taken only in
    // state 7: in 8 the charm is given and the amulet kept (refused 2) - the original's quirk, kept.
    {"QNO_WC_WAREHOUSE_W_2",
     "on use ITEM_QNO_WC_WAREHOUSE_W_2_01 status 7 8 cap 70 ITEM_QNO_WC_WAREHOUSE_W_2_02 chance 1 of 2 give ITEM_QNO_WC_WAREHOUSE_W_2_02 "
     "or-give ITEM_QNO_WC_WAREHOUSE_W_2_03 kept-in 8"},
    {"QNO_WC_WAREHOUSE_W_3",
     "on use ITEM_QNO_WC_WAREHOUSE_W_2_01 status 7 8 cap 130 ITEM_QNO_WC_WAREHOUSE_W_2_02 chance 1 of 2 give ITEM_QNO_WC_WAREHOUSE_W_2_02 "
     "or-give ITEM_QNO_WC_WAREHOUSE_W_2_03 kept-in 8"},

    // --- The quest instances (the item's use, then the party's walk in, the world's tick and its unique's death).
    // The use: states 1/7/8, the auto-share party (_11), the place (_13), a party (silent), its master (_12). The
    // walk in: a trade transport out (_26), the members on this server who finished the quest before or hold the
    // drum; always 0 (the drum kept). The world's tick: 30000 ms, then the unique
    // 20..100 away (_14, _25, a 20-minute timer), _16 at 600000 ms, _17 at 900000, _23 at 1200000, everyone out at 1210000;
    // its death (_15).
    {"QNO_CA_GORIA_7",
     "on use ITEM_QNO_CA_GORIA_7_03 status 1 7 8 party @SN_TALK_QNO_CA_GORIA_7_11 at 26991 1154 142 1506 700 away @SN_TALK_QNO_CA_GORIA_7_13 "
     "instance INS_QNO_CA_GORIA_7 unique MOB_QT_01_HUNLANCER after 30 minutes 20 master @SN_TALK_QNO_CA_GORIA_7_12 "
     "transport @SN_TALK_QNO_CA_GORIA_7_26 comes @SN_TALK_QNO_CA_GORIA_7_14 begun @SN_TALK_QNO_CA_GORIA_7_25 warn 10 @SN_TALK_QNO_CA_GORIA_7_16 "
     "warn 5 @SN_TALK_QNO_CA_GORIA_7_17 slain @SN_TALK_QNO_CA_GORIA_7_15 over @SN_TALK_QNO_CA_GORIA_7_23"},
    // the world, then the place _10, the party _08, its
    // master _09; its world's tick: 30000 ms, the unique, 2 s, Cerberus's _14, 5 s, _21 and the 20-minute timer,
    // _16, _17, _19; Cerberus's death (_15).
    {"QNO_EU_EASTEU_21",
     "on use ITEM_QNO_EU_EASTEU_21_01 status 1 7 8 place-first party @SN_TALK_QNO_EU_EASTEU_21_08 at 24902 886 854 8 600 away @SN_TALK_QNO_EU_EASTEU_21_10 "
     "instance INS_QNO_EU_EASTEU_21 unique MOB_QT_01_KERBEROS after 30 minutes 20 master @SN_TALK_QNO_EU_EASTEU_21_09 "
     "comes @SN_TALK_QNO_EU_EASTEU_21_14 begun @SN_TALK_QNO_EU_EASTEU_21_21 warn 10 @SN_TALK_QNO_EU_EASTEU_21_16 warn 5 @SN_TALK_QNO_EU_EASTEU_21_17 "
     "slain @SN_TALK_QNO_EU_EASTEU_21_15 over @SN_TALK_QNO_EU_EASTEU_21_19"},
    // Its use, the Key of Sign (_06): the world, the place _10, the
    // party _08, its master _09; its tick 30000 ms, Ivy, 2 s, then 5 s apart Ivy's
    // _11, _12, Ivy's _13, 5 s, _24 and the 20-minute timer, _14, _15, _22; her death
    // (_21).
    {"QNO_EU_IVY_3",
     "on use ITEM_QNO_EU_IVY_3_06 status 1 7 8 place-first party @SN_TALK_QNO_EU_IVY_3_08 at 25183 1882 -169 632 600 away @SN_TALK_QNO_EU_IVY_3_10 "
     "instance INS_QNO_EU_IVY_3 unique MOB_QT_01_IVY after 30 minutes 20 master @SN_TALK_QNO_EU_IVY_3_09 comes @SN_TALK_QNO_EU_IVY_3_11 "
     "comes @SN_TALK_QNO_EU_IVY_3_12 comes @SN_TALK_QNO_EU_IVY_3_13 begun @SN_TALK_QNO_EU_IVY_3_24 warn 10 @SN_TALK_QNO_EU_IVY_3_14 "
     "warn 5 @SN_TALK_QNO_EU_IVY_3_15 slain @SN_TALK_QNO_EU_IVY_3_21 over @SN_TALK_QNO_EU_IVY_3_22"},
    // states 1/7/8, the world, the calabash (_01) and the stone held (silently), the tomb's floor 0x8005 within 2000 of
    // (184, 72, 157) (_23), the party (_21), its master (_22); the
    // walker (a trade transport out _27; the members on this server). Its world's tick the generals
    // in turn (CurrentUnique 0..3, MOB_QT_TQ_SNAKEGENERAL1..4) 20..100 from the player, the first 30000 ms on, each next 10000 ms
    // after the one before dies (MobKillTime); 2 s on its line (_06, _09, _12, _15), 5 s on its battle; from the first one's
    // battle _26, the gauge's 30 minutes and EVENT_TIMER 1200000 then 300000 twice: _24, _25, at 30 minutes the
    // general's last words (_08, _11, _14, _17) and everyone out 10 s later; a general's death
    // its words (_07, _10, _13); the fourth's _16 and a way out 20..100 from it (the runtime teleport, its link empty: back
    // where its user came in), everyone left out at the time's end.
    {"QNO_TQ_ORNAMENT_4",
     "on use ITEM_QNO_TQ_ORNAMENT_4_04 status 1 7 8 need ITEM_QNO_TQ_ORNAMENT_4_01 ITEM_QNO_TQ_ORNAMENT_4_04 place-first party @SN_TALK_QNO_TQ_ORNAMENT_4_21 "
     "at -32763 184 72 157 2000 away @SN_TALK_QNO_TQ_ORNAMENT_4_23 instance INS_QNO_TQ_ORNAMENT_4 after 30 minutes 30 "
     "master @SN_TALK_QNO_TQ_ORNAMENT_4_22 transport @SN_TALK_QNO_TQ_ORNAMENT_4_27 begun @SN_TALK_QNO_TQ_ORNAMENT_4_26 "
     "warn 10 @SN_TALK_QNO_TQ_ORNAMENT_4_24 warn 5 @SN_TALK_QNO_TQ_ORNAMENT_4_25 "
     "general MOB_QT_TQ_SNAKEGENERAL1 says @SN_TALK_QNO_TQ_ORNAMENT_4_06 dies @SN_TALK_QNO_TQ_ORNAMENT_4_07 last @SN_TALK_QNO_TQ_ORNAMENT_4_08 "
     "general MOB_QT_TQ_SNAKEGENERAL2 says @SN_TALK_QNO_TQ_ORNAMENT_4_09 dies @SN_TALK_QNO_TQ_ORNAMENT_4_10 last @SN_TALK_QNO_TQ_ORNAMENT_4_11 "
     "general MOB_QT_TQ_SNAKEGENERAL3 says @SN_TALK_QNO_TQ_ORNAMENT_4_12 dies @SN_TALK_QNO_TQ_ORNAMENT_4_13 last @SN_TALK_QNO_TQ_ORNAMENT_4_14 "
     "general MOB_QT_TQ_SNAKEGENERAL4 says @SN_TALK_QNO_TQ_ORNAMENT_4_15 dies @SN_TALK_QNO_TQ_ORNAMENT_4_16 last @SN_TALK_QNO_TQ_ORNAMENT_4_17 next 10 "
     "way-out"},
    // the same with the tablet (_01) and the head held, two places of the tomb's
    // floor 0x8007 within 600 ((-3, 11, 674), (2, 11, 1090); _25), the party (_23), its master (_24); the walker (_29).
    // Its tick MOB_QT_TQ_TOMBGENERAL1..4, lines _08, _11, _14, _17, deaths _09, _12, _15 and _18, the battle _28, _26,
    // _27, the last words _10, _13, _16, _19; the fourth's death a way out as ORNAMENT_4's.
    {"QNO_TQ_STONEBOARD_4",
     "on use ITEM_QNO_TQ_STONEBOARD_4_03 status 1 7 8 need ITEM_QNO_TQ_STONEBOARD_4_01 ITEM_QNO_TQ_STONEBOARD_4_03 place-first "
     "party @SN_TALK_QNO_TQ_STONEBOARD_4_23 at -32761 -3 11 674 600 at -32761 2 11 1090 600 away @SN_TALK_QNO_TQ_STONEBOARD_4_25 "
     "instance INS_QNO_TQ_STONEBOARD_4 after 30 minutes 30 master @SN_TALK_QNO_TQ_STONEBOARD_4_24 transport @SN_TALK_QNO_TQ_STONEBOARD_4_29 "
     "begun @SN_TALK_QNO_TQ_STONEBOARD_4_28 warn 10 @SN_TALK_QNO_TQ_STONEBOARD_4_26 warn 5 @SN_TALK_QNO_TQ_STONEBOARD_4_27 "
     "general MOB_QT_TQ_TOMBGENERAL1 says @SN_TALK_QNO_TQ_STONEBOARD_4_08 dies @SN_TALK_QNO_TQ_STONEBOARD_4_09 last @SN_TALK_QNO_TQ_STONEBOARD_4_10 "
     "general MOB_QT_TQ_TOMBGENERAL2 says @SN_TALK_QNO_TQ_STONEBOARD_4_11 dies @SN_TALK_QNO_TQ_STONEBOARD_4_12 last @SN_TALK_QNO_TQ_STONEBOARD_4_13 "
     "general MOB_QT_TQ_TOMBGENERAL3 says @SN_TALK_QNO_TQ_STONEBOARD_4_14 dies @SN_TALK_QNO_TQ_STONEBOARD_4_15 last @SN_TALK_QNO_TQ_STONEBOARD_4_16 "
     "general MOB_QT_TQ_TOMBGENERAL4 says @SN_TALK_QNO_TQ_STONEBOARD_4_17 dies @SN_TALK_QNO_TQ_STONEBOARD_4_18 last @SN_TALK_QNO_TQ_STONEBOARD_4_19 next 10 "
     "way-out"},
    // The same handler's Horn Trumpet (_01): states 1/7/8, the auto-share party (_26), then the three
    // sites in turn (their table on the stack,region, x, y, z, radius; the sign; its keeper): the first
    // the player stands at without its sign raises the keeper 20 + 80 x rand away, its five-minute timer (0x493E0, the
    // despawn) and its death handler (the sign dropped for each party member within
    // 1000 holding neither it nor the Key; ours the quest's hidden gathering goals, 100 % to the kill's holders); none: _25.
    {"QNO_EU_IVY_3",
     "on use ITEM_QNO_EU_IVY_3_01 status 1 7 8 party @SN_TALK_QNO_EU_IVY_3_26 "
     "site 27237 1057 218 634 1000 ITEM_QNO_EU_IVY_3_04 MOB_QT_01_PUNISHER radius 20 100 life 300 "
     "site 25954 1661 180 210 700 ITEM_QNO_EU_IVY_3_03 MOB_QT_03_PUNISHER_CLON radius 20 100 life 300 "
     "site 25187 646 182 204 600 ITEM_QNO_EU_IVY_3_02 MOB_QT_01_SOLDIER_CLON radius 20 100 life 300 away @SN_TALK_QNO_EU_IVY_3_25"},
    // Its Punisher's Blood (_05): states 1/7/8, a free place, the three signs held (all silently), then
    // the Key of Sign given and one of each sign taken; the grant failing refuses it.
    {"QNO_EU_IVY_3",
     "on use ITEM_QNO_EU_IVY_3_05 status 1 7 8 room need ITEM_QNO_EU_IVY_3_04 ITEM_QNO_EU_IVY_3_03 ITEM_QNO_EU_IVY_3_02 give ITEM_QNO_EU_IVY_3_06"},
};

// The guild support's monsters, levels 30..110 (QEV_GUILD_SUPPORT's spawn, one a level).
constexpr std::string_view kGuildSupport[81] = {
    "MOB_EVE_STRONG_WC_HYEONGCHEON", "MOB_EVE_STRONG_CA_PERYTON_CLON", "MOB_EVE_STRONG_OA_SCORPION", "MOB_EVE_STRONG_CA_ONG_CLON",
    "MOB_EVE_STRONG_CA_ONG", "MOB_EVE_STRONG_OA_YEOWA_CLON", "MOB_EVE_STRONG_OA_BLACKROBBERARCHER", "MOB_EVE_STRONG_CA_MONKEY_CLON",
    "MOB_EVE_STRONG_OA_BLACKROBBER", "MOB_EVE_STRONG_CA_GOLEM_CLON", "MOB_EVE_STRONG_OA_REDSCORPION", "MOB_EVE_STRONG_OA_REDEYEGHOST",
    "MOB_EVE_STRONG_OA_REDEYEGHOST_CLON", "MOB_EVE_STRONG_KT_ONG_CLON2", "MOB_EVE_STRONG_KT_BUNWANG_CLON", "MOB_EVE_STRONG_KT_BUNWANG",
    "MOB_EVE_STRONG_KT_DESERTTRUNKZ_CLON", "MOB_EVE_STRONG_KT_WHITESPIDER_CLON", "MOB_EVE_STRONG_KT_WHITESPIDER", "MOB_EVE_STRONG_KT_MUJIGI",
    "MOB_EVE_STRONG_KT_MUJIGI_CLON", "MOB_EVE_STRONG_KK_ISHADE", "MOB_EVE_STRONG_KK_SPIDER_CLON", "MOB_EVE_STRONG_KK_SPIDER",
    "MOB_EVE_STRONG_KK_PENON_F", "MOB_EVE_STRONG_KK_PENON_W", "MOB_EVE_STRONG_KK_PLANAR", "MOB_EVE_STRONG_KK_PLANAR_CLON",
    "MOB_EVE_STRONG_KK_GWHITESPIDER", "MOB_EVE_STRONG_KK_YETI", "MOB_EVE_STRONG_KK_YETI_CLON", "MOB_EVE_STRONG_DH_BUGGHOST_CLON",
    "MOB_EVE_STRONG_DH_WORKEARTHGHOST_CLON", "MOB_EVE_STRONG_DH_BUGGHOST", "MOB_EVE_STRONG_DH_WORKEARTHGHOST",
    "MOB_EVE_STRONG_DH_SOLDIEREARTHGHOST_CLON", "MOB_EVE_STRONG_DH_SOLDIEREARTHGHOST", "MOB_EVE_STRONG_DH_BLINDMON_CLON",
    "MOB_EVE_STRONG_DH_BLINDMON", "MOB_EVE_STRONG_DH_YACHA_CLON", "MOB_EVE_STRONG_RM_FEATHERMASK_CLON", "MOB_EVE_STRONG_TK_BONESOLDIER_CLON",
    "MOB_EVE_STRONG_RM_ROCKY_CLON", "MOB_EVE_STRONG_RM_FEATHERMASK", "MOB_EVE_STRONG_TK_BONEHUNTER_CLON", "MOB_EVE_STRONG_RM_GOAT_CLON",
    "MOB_EVE_STRONG_TK_BONEHUNTER", "MOB_EVE_STRONG_TK_BONEMAGE_CLON", "MOB_EVE_STRONG_RM_WINGTRIBE_CLON", "MOB_EVE_STRONG_TK_BONEGENERAL_CLON",
    "MOB_EVE_STRONG_RM_WINGTRIBE", "MOB_EVE_STRONG_TQ_TOMBSOLDIER_CLON2", "MOB_EVE_STRONG_RM_ANTELOPE_CLON",
    "MOB_EVE_STRONG_TQ_QINSHITOMBSTONE_CLON", "MOB_EVE_STRONG_TQ_TOMBARCHER_CLON2", "MOB_EVE_STRONG_RM_WINGTRIBESPEAR_CLON",
    "MOB_EVE_STRONG_TQ_TOMBSTONEGHOST_CLON2", "MOB_EVE_STRONG_RM_WINGTRIBESPEAR", "MOB_EVE_STRONG_TQ_ROYALSOLDIER_CLON2",
    "MOB_EVE_STRONG_TQ_ROYALSOLDIER_CLON", "MOB_EVE_STRONG_TQ_TOMBBUGGHOST_CLON2", "MOB_EVE_STRONG_TQ_SNAKESLAVE_CLON2",
    "MOB_EVE_STRONG_TQ_SNAKEWOMAN_CLON2", "MOB_EVE_STRONG_TQ_SNAKESLAVE_CLON", "MOB_EVE_STRONG_TQ_SNAKEDEMON_CLON2",
    "MOB_EVE_STRONG_TQ_SNAKESLAVE", "MOB_EVE_STRONG_TQ_SNAKEDEMON_CLON", "MOB_EVE_STRONG_TQ_SNAKEMAN_CLON2", "MOB_EVE_STRONG_TQ_SNAKEMAN_CLON",
    "MOB_EVE_STRONG_TQ_SNAKEMAN", "MOB_EVE_STRONG_TQ_MANGNYANG", "MOB_EVE_STRONG_SD_WENEG", "MOB_EVE_STRONG_SD_KEISAS",
    "MOB_EVE_STRONG_SD_SAND_HYENA", "MOB_EVE_STRONG_SD_RERET", "MOB_EVE_STRONG_SD_MUMMY_RAGE", "MOB_EVE_STRONG_SD_TELBASTA",
    "MOB_EVE_STRONG_SD_EVILBUG", "MOB_EVE_STRONG_SD_BLOODKHEPRI", "MOB_EVE_STRONG_SD_AKERU", "MOB_EVE_STRONG_SD_SOPEDU",
};

// The Pandora's table (QEV_PANDORA_BOX_100LV's setup, its map's level keys): levels 21..29 below the
// guild support's 30..110, which it shares. Two rows say 28: the map keeps the first (MOB_EVE_STRONG_WC_EARTHGHOST_CLON);
// the second (MOB_EVE_STRONG_AM_SOLDIER) is never picked.
constexpr std::string_view kPandoraLow[9] = {
    "MOB_EVE_STRONG_AM_CRAB_CLON",       "MOB_EVE_STRONG_WC_DEVILBUG",   "MOB_EVE_STRONG_AM_CRAB",
    "MOB_EVE_STRONG_AM_WOLF_CLON",       "MOB_EVE_STRONG_WC_DEMONHORSE", "MOB_EVE_STRONG_AM_ARCHER_CLON",
    "MOB_EVE_STRONG_WC_EARTHGHOST",      "MOB_EVE_STRONG_WC_EARTHGHOST_CLON",
    "MOB_EVE_STRONG_AM_PUNISHER_CLON",
};

std::string_view PandoraAt(int level) { // 21..110
    return level < 30 ? kPandoraLow[level - 21] : kGuildSupport[level - 30];
}

} // namespace

QuestItemUse QuestItemUseOf(std::string_view codename) {
    for (const Row& r : kRows)
        if (r.item == codename) return r.use;
    return {};
}

const std::vector<OriginalUseRule>& OriginalUseRules() { return kRules; }

int AddOriginalUseRules(QuestDef& q) {
    int added = 0;
    for (const OriginalUseRule& r : kRules) {
        if (r.quest != q.codename) continue;
        QuestDef parsed;
        std::string error;
        if (!ParseQuest("quest " + std::string(r.quest) + "\n" + std::string(r.line) + "\n", parsed, error) || parsed.onUse.size() != 1) continue;
        const QuestDef::UseRule& u = parsed.onUse.front();
        const bool has = std::any_of(q.onUse.begin(), q.onUse.end(), [&](const QuestDef::UseRule& o) { return o.item == u.item; });
        if (has) continue;
        q.onUse.push_back(u);
        ++added;
    }
    return added;
}

SpawnOffset QuestSpawnOffset(float near, float far, std::mt19937& random) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float r = near + unit(random) * (far - near);
    if (r <= 1e-6f) return {}; //'s first compare: no offset
    const float third = std::max(1.0f, r) / 3.0f;
    const int band = std::uniform_int_distribution<int>(0, 100)(random);
    const float base = band < 70 ? third * 2.0f : band < 90 ? third : 0.0f;
    const float distance = base + unit(random) * third;
    const float bearing = unit(random) * 6.2831853f;
    return {std::cos(bearing) * distance, std::sin(bearing) * distance};
}

std::uint32_t QuestWorldId(std::string_view codename) {
    // _RefGame_World (the 1.188 database's rows 10..39).
    constexpr std::pair<std::string_view, std::uint32_t> kWorlds[] = {
        {"INS_QNO_RM_OLDWOMAN_6", 10}, {"INS_QNO_EU_EASTEU_21", 11},    {"INS_QNO_EU_IVY_3", 12},
        {"INS_QNO_CA_GORIA_7", 13},    {"INS_QNO_TQ_STONEBOARD_4", 16}, {"INS_QNO_TQ_ORNAMENT_4", 17},
        {"INS_QNO_TQ_GENERALORDER_4", 18},
        {"INS_QNO_CH_HWAN_1_5", 32},   {"INS_QNO_EU_HWAN_1_5", 33},     {"INS_QNO_CH_HWAN_2_5", 34},
        {"INS_QNO_EU_HWAN_2_5", 35},   {"INS_QNO_CH_HWAN_1_1", 36},     {"INS_QNO_CH_HWAN_2_1", 37},
        {"INS_QNO_EU_HWAN_1_1", 38},   {"INS_QNO_EU_HWAN_2_1", 39},
    };
    for (const auto& [name, id] : kWorlds)
        if (name == codename) return id;
    return 0;
}

int CaptureChance(int roll, int step, bool below, int playerLevel, int cosLevel) {
    if (below) return cosLevel > playerLevel ? roll - step * (cosLevel - playerLevel) : roll;
    return playerLevel > cosLevel ? roll + step * (playerLevel - cosLevel) : roll;
}

std::string_view GuildSupportMonster(int level) {
    if (level < kSummonEventLevel) return {};
    return kGuildSupport[std::min(level, 110) - 30];
}

std::vector<std::string_view> PandoraMonsters(int level) {
    // The first key at or above the level (lower_bound over 21..110); none above 110 is asked (the level is capped).
    const int top = std::max(21, std::min(level, 110));
    std::vector<std::string_view> rows;
    for (int l = top; l >= 21 && rows.size() < 10; --l) rows.push_back(PandoraAt(l));
    std::vector<std::string_view> out;
    // Ascending, the lowest repeated in front when fewer than ten.
    for (std::size_t i = rows.size(); i < 10 && !rows.empty(); ++i) out.push_back(rows.back());
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) out.push_back(*it);
    return out;
}

} // namespace sr::game
