// The original's coded quests of Constantinople's East Europe chain (QNO_EU_EASTEU_*, _ADVENTURER_*, _GENERAL_*, _IVY_*,
// _WITCH_*) and the hwan army tests (QNO_CH_HWAN_*, QNO_EU_HWAN_*): definitions in our format, with the same
// conventions as OriginalQuests.cpp. Read by DecodeCodedQuests. Numbers, levels, names and rewards come at the import
// from _RefQuest and _RefQuestReward (the hwan levels 1 and 2 of QNO_*_HWAN_1_6 / _2_6 among them).
//
// What these quests use besides OriginalQuests.cpp's:
// - the quests first, three ways: every one finished (`start after all`), one of them (`after one`), and every one
//   under way (`start during`);
// - a party sharing experience and items (`start party`, the `not-party` talk);
// - the NPCs: the first offers, the last mission's NPC takes it back, each with its greeting;
// - the talk: offer, accepted, denied, not-done, done, not-done-2, bag-full, the content, greeting, done-now, abort by
//   level, count-limit, time-out, middle, offer-again, not-party;
// - the missions: 1 a gathering (counted, or a helper that never holds the quest back: `hidden`); 2 a kill (champions
//   too); 3 a delivery (its items handed over at the start, its talk); 4 items from the NPC (`start-item`); 6 a talk; 8 a
//   capture (the traps given, the roll, the catch as an item, else a COS following its catcher: `goal capture`); 10 an
//   item changed (`goal drop` without sources);
// - the quest's own instance world (INS_QNO_*), entered by a teleport or its item.

#include <string_view>

#include "sr/server/QuestScript.h"

namespace sr::server {

namespace {

constexpr std::string_view kCoded = R"(
# QNO_EU_EASTEU_1.
# The witch's first task: 25 Stable Sentries (MOB_QT_01_BAROI, the stable's nests) and 5 Stable Filths made by Gabriel's
# holy water (ITEM_QNO_EU_EASTEU_3_02, EASTEU_3's reward) used at the stable (a ground search: game::OriginalUseRules).
# The Filths are a mission of kind 10 (an item changed into another: the item held): a drop goal without sources. Its
# talk says _05 instead of _04 once holy water is held.
quest QNO_EU_EASTEU_1
start level 1-
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH
finish NPC_EU_WITCH
goal kill 25 MOB_QT_01_BAROI text @SN_CON_QNO_EU_EASTEU_1_01
goal drop 5 ITEM_QNO_EU_EASTEU_1_02 text @SN_CON_QNO_EU_EASTEU_1_02
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_EASTEU_1_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_EASTEU_1_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_EASTEU_1_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_EASTEU_1_04
talk NPC_EU_WITCH done @SN_TALK_QNO_EU_EASTEU_1_07
talk NPC_EU_WITCH bag-full @SN_TALK_QNO_EU_EASTEU_1_06
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_WITCH done-now @SN_TALK_QNO_EU_EASTEU_1_08

# QNO_EU_EASTEU_2.
# Taken at the priest while EASTEU_1 runs: Uvetino's hat handed over at the start and brought to him; done there.
quest QNO_EU_EASTEU_2
start level 1-
start during QNO_EU_EASTEU_1
repeat 1
giver NPC_EU_PRIEST
npc NPC_EU_PRIEST NPC_EU_UNION2
finish NPC_EU_UNION2
start-item ITEM_QNO_EU_EASTEU_2_01 1
goal deliver NPC_EU_UNION2 ITEM_QNO_EU_EASTEU_2_01 1 talk @SN_TALK_QNO_EU_EASTEU_2_06 text @SN_CON_QNO_EU_EASTEU_2
talk NPC_EU_PRIEST greeting @SN_NPC_EU_PRIEST_QS
talk NPC_EU_UNION2 greeting @SN_NPC_EU_UNION2_QS
talk NPC_EU_PRIEST offer @SN_TALK_QNO_EU_EASTEU_2_01
talk NPC_EU_PRIEST accepted @SN_TALK_QNO_EU_EASTEU_2_02
talk NPC_EU_PRIEST denied @SN_TALK_QNO_EU_EASTEU_2_03
talk NPC_EU_PRIEST not-done @SN_TALK_QNO_EU_EASTEU_2_04
talk NPC_EU_UNION2 not-done @SN_TALK_QNO_EU_EASTEU_2_04
talk NPC_EU_PRIEST bag-full @SN_TALK_QNO_EU_EASTEU_2_05
talk NPC_EU_PRIEST middle @SN_TALK_QNO_EU_EASTEU_2_07

# QNO_EU_EASTEU_3.
# Uvetino's horns, while EASTEU_1 runs and after EASTEU_2; the reward is ten holy water (_RefQuestRewardItems). Its talk
# also gives more holy water once a day to one who finished it and holds none while EASTEU_1 runs (row _09, words
# _05, _10, _11, _12): not built (ours: EASTEU_1 given up and taken again).
quest QNO_EU_EASTEU_3
start level 1-
start after all QNO_EU_EASTEU_2
start during QNO_EU_EASTEU_1
repeat 1
giver NPC_EU_UNION2
npc NPC_EU_UNION2
finish NPC_EU_UNION2
goal drop 20 ITEM_QNO_EU_EASTEU_3_01 from MOB_EU_DOWB_CLON 20% at NPC_EU_UNION2 text @SN_CON_QNO_EU_EASTEU_3
talk NPC_EU_UNION2 offer @SN_TALK_QNO_EU_EASTEU_3_01
talk NPC_EU_UNION2 accepted @SN_TALK_QNO_EU_EASTEU_3_02
talk NPC_EU_UNION2 denied @SN_TALK_QNO_EU_EASTEU_3_03
talk NPC_EU_UNION2 not-done @SN_TALK_QNO_EU_EASTEU_3_04
talk NPC_EU_UNION2 done @SN_TALK_QNO_EU_EASTEU_3_07
talk NPC_EU_UNION2 bag-full @SN_TALK_QNO_EU_EASTEU_3_06
talk NPC_EU_UNION2 greeting @SN_NPC_EU_UNION2_QS
talk NPC_EU_UNION2 done-now @SN_TALK_QNO_EU_EASTEU_3_08

# QNO_EU_EASTEU_4.
# Its talk asks first (_01) and has the player answer (_02) before the offer (_03): three pages here.
quest QNO_EU_EASTEU_4
start level 2-
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH
finish NPC_EU_WITCH
goal drop 35 ITEM_QNO_EU_EASTEU_4_01 from MOB_EU_KYKLOPES_CLON2 25% at NPC_EU_WITCH text @SN_CON_QNO_EU_EASTEU_4
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_EASTEU_4_01
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_EASTEU_4_02
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_EASTEU_4_03
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_EASTEU_4_04
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_EASTEU_4_05
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_EASTEU_4_06
talk NPC_EU_WITCH done @SN_TALK_QNO_EU_EASTEU_4_07
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_WITCH done-now @SN_TALK_QNO_EU_EASTEU_4_08

# QNO_EU_EASTEU_5.
# A capture (mission kind 8): the Chaos Medicine (five of them) used on a Dow Genema (MOB_EU_DOWB, the roll 50) makes it
# follow as MOB_QT_01_DOWB_COS for its time (the minute's warnings _19, _20), brought to the witch. The catch is the item
# rule's (`on use ... capture`); the witch's daily new medicine (_05, the row _16, _17; the day's spent _15):
# `resupply-daily start`. A choice of two scrolls (_RefQuestReward).
quest QNO_EU_EASTEU_5
start level 2-
start after all QNO_EU_EASTEU_4
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH
finish NPC_EU_WITCH
start-item ITEM_QNO_EU_EASTEU_5_01 5
resupply ITEM_QNO_EU_EASTEU_5_01 5
resupply-daily start
pay-take ITEM_QNO_EU_EASTEU_5_01
goal capture MOB_QT_01_DOWB_COS at NPC_EU_WITCH text @SN_CON_QNO_EU_EASTEU_5
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_EASTEU_5_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_EASTEU_5_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_EASTEU_5_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_EASTEU_5_04
talk NPC_EU_WITCH done @SN_TALK_QNO_EU_EASTEU_5_08
talk NPC_EU_WITCH bag-full @SN_TALK_QNO_EU_EASTEU_5_06
talk NPC_EU_WITCH resupply @SN_TALK_QNO_EU_EASTEU_5_05 row @SN_TALK_QNO_EU_EASTEU_5_16
talk NPC_EU_WITCH resupply @SN_TALK_QNO_EU_EASTEU_5_17
talk NPC_EU_WITCH resupply-spent @SN_TALK_QNO_EU_EASTEU_5_15
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_WITCH done-now @SN_TALK_QNO_EU_EASTEU_5_11

# QNO_EU_EASTEU_9.
# A talk with Adria (NPC_EU_HUNTER), done there.
quest QNO_EU_EASTEU_9
start level 5-
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH NPC_EU_HUNTER
finish NPC_EU_HUNTER
goal talk NPC_EU_HUNTER talk @SN_TALK_QNO_EU_EASTEU_9_05 text @SN_CON_QNO_EU_EASTEU_9
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_HUNTER greeting @SN_NPC_EU_HUNTER_QS
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_EASTEU_9_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_EASTEU_9_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_EASTEU_9_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_EASTEU_9_04
talk NPC_EU_HUNTER not-done @SN_TALK_QNO_EU_EASTEU_9_04
talk NPC_EU_WITCH middle @SN_TALK_QNO_EU_EASTEU_9_06

# QNO_EU_EASTEU_11.
# Adria's Wind Pouch handed over at the start and taken to the witch; done there.
quest QNO_EU_EASTEU_11
start level 5-
start after all QNO_EU_EASTEU_10
repeat 1
giver NPC_EU_HUNTER
npc NPC_EU_HUNTER NPC_EU_WITCH
finish NPC_EU_WITCH
start-item ITEM_QNO_EU_EASTEU_11_01 1
goal deliver NPC_EU_WITCH ITEM_QNO_EU_EASTEU_11_01 1 talk @SN_TALK_QNO_EU_EASTEU_11_06 text @SN_CON_QNO_EU_EASTEU_11
talk NPC_EU_HUNTER greeting @SN_NPC_EU_HUNTER_QS
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_HUNTER offer @SN_TALK_QNO_EU_EASTEU_11_01
talk NPC_EU_HUNTER accepted @SN_TALK_QNO_EU_EASTEU_11_02
talk NPC_EU_HUNTER denied @SN_TALK_QNO_EU_EASTEU_11_03
talk NPC_EU_HUNTER not-done @SN_TALK_QNO_EU_EASTEU_11_04
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_EASTEU_11_04
talk NPC_EU_HUNTER bag-full @SN_TALK_QNO_EU_EASTEU_11_05
talk NPC_EU_HUNTER middle @SN_TALK_QNO_EU_EASTEU_11_07

# QNO_EU_EASTEU_11_1.
# A capture as EASTEU_5's: five Wind Pouches on a Simos Ladon (MOB_EU_LADON), its MOB_QT_01_LADON_COS brought to the
# witch (summoned again at a login; its minutes counted).
quest QNO_EU_EASTEU_11_1
start level 5-
start after all QNO_EU_EASTEU_11
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH
finish NPC_EU_WITCH
start-item ITEM_QNO_EU_EASTEU_11_02 5
resupply ITEM_QNO_EU_EASTEU_11_02 5
resupply-daily start
pay-take ITEM_QNO_EU_EASTEU_11_02
goal capture MOB_QT_01_LADON_COS at NPC_EU_WITCH text @SN_CON_QNO_EU_EASTEU_11_1
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_EASTEU_11_1_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_EASTEU_11_1_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_EASTEU_11_1_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_EASTEU_11_1_04
talk NPC_EU_WITCH done @SN_TALK_QNO_EU_EASTEU_11_1_07
talk NPC_EU_WITCH bag-full @SN_TALK_QNO_EU_EASTEU_11_1_06
talk NPC_EU_WITCH resupply @SN_TALK_QNO_EU_EASTEU_11_1_05
talk NPC_EU_WITCH resupply-spent @SN_TALK_QNO_EU_EASTEU_11_1_14
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_WITCH done-now @SN_TALK_QNO_EU_EASTEU_11_1_08

# QNO_EU_EASTEU_15.
# A talk with General Ratchel, done there.
quest QNO_EU_EASTEU_15
start level 7-
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH NPC_EU_GENERAL
finish NPC_EU_GENERAL
goal talk NPC_EU_GENERAL talk @SN_TALK_QNO_EU_EASTEU_15_05 text @SN_CON_QNO_EU_EASTEU_15
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_GENERAL greeting @SN_NPC_EU_GENERAL_QS
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_EASTEU_15_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_EASTEU_15_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_EASTEU_15_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_EASTEU_15_04
talk NPC_EU_GENERAL not-done @SN_TALK_QNO_EU_EASTEU_15_04
talk NPC_EU_GENERAL done @SN_TALK_QNO_EU_EASTEU_15_05
talk NPC_EU_WITCH middle @SN_TALK_QNO_EU_EASTEU_15_06

# QNO_EU_EASTEU_17.
# The Forgotten Harp (EASTEU_16's, taken at its hand-in) handed over again at the start, taken to the witch.
quest QNO_EU_EASTEU_17
start level 7-
start after all QNO_EU_EASTEU_16
repeat 1
giver NPC_EU_GENERAL
npc NPC_EU_GENERAL NPC_EU_WITCH
finish NPC_EU_WITCH
start-item ITEM_QNO_EU_EASTEU_16_01 1
goal deliver NPC_EU_WITCH ITEM_QNO_EU_EASTEU_16_01 1 talk @SN_TALK_QNO_EU_EASTEU_17_06 text @SN_CON_QNO_EU_EASTEU_17
talk NPC_EU_GENERAL greeting @SN_NPC_EU_GENERAL_QS
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_GENERAL offer @SN_TALK_QNO_EU_EASTEU_17_01
talk NPC_EU_GENERAL accepted @SN_TALK_QNO_EU_EASTEU_17_02
talk NPC_EU_GENERAL denied @SN_TALK_QNO_EU_EASTEU_17_03
talk NPC_EU_GENERAL not-done @SN_TALK_QNO_EU_EASTEU_17_04
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_EASTEU_17_04
talk NPC_EU_GENERAL bag-full @SN_TALK_QNO_EU_EASTEU_17_05
talk NPC_EU_GENERAL middle @SN_TALK_QNO_EU_EASTEU_17_07

# QNO_EU_EASTEU_17_1.
# A capture as EASTEU_5's: five Remedies of Sound on a Seirenes (MOB_EU_SIREN_CLON), its
# MOB_QT_01_SIREN_CLON_COS brought to the witch.
quest QNO_EU_EASTEU_17_1
start level 7-
start after all QNO_EU_EASTEU_17
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH
finish NPC_EU_WITCH
start-item ITEM_QNO_EU_EASTEU_17_1_01 5
resupply ITEM_QNO_EU_EASTEU_17_1_01 5
resupply-daily start
pay-take ITEM_QNO_EU_EASTEU_17_1_01
goal capture MOB_QT_01_SIREN_CLON_COS at NPC_EU_WITCH text @SN_CON_QNO_EU_EASTEU_17_1
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_EASTEU_17_1_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_EASTEU_17_1_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_EASTEU_17_1_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_EASTEU_17_1_04
talk NPC_EU_WITCH done @SN_TALK_QNO_EU_EASTEU_17_1_07
talk NPC_EU_WITCH bag-full @SN_TALK_QNO_EU_EASTEU_17_1_06
talk NPC_EU_WITCH resupply @SN_TALK_QNO_EU_EASTEU_17_1_05
talk NPC_EU_WITCH resupply-spent @SN_TALK_QNO_EU_EASTEU_17_1_14
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_WITCH done-now @SN_TALK_QNO_EU_EASTEU_17_1_08

# QNO_EU_EASTEU_19.
# Thirty Long Scissors from the witch (mission kind 4) cut Golden Apples at the tree (a search: game::OriginalUseRules);
# an apple used there raises MOB_QT_01_LADON, whose kills drop the 15 Bloody Orbs at 50 %. Its end clears the three
# items.
quest QNO_EU_EASTEU_19
start level 9-
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH
finish NPC_EU_WITCH
start-item ITEM_QNO_EU_EASTEU_19_03 30
resupply ITEM_QNO_EU_EASTEU_19_03 30
resupply-daily start
goal drop 15 ITEM_QNO_EU_EASTEU_19_02 from MOB_QT_01_LADON 50% at NPC_EU_WITCH text @SN_CON_QNO_EU_EASTEU_19
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_EASTEU_19_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_EASTEU_19_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_EASTEU_19_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_EASTEU_19_04
talk NPC_EU_WITCH done @SN_TALK_QNO_EU_EASTEU_19_05
talk NPC_EU_WITCH bag-full @SN_TALK_QNO_EU_EASTEU_19_14
talk NPC_EU_WITCH resupply @SN_TALK_QNO_EU_EASTEU_19_07 row @SN_TALK_QNO_EU_EASTEU_19_08
talk NPC_EU_WITCH resupply @SN_TALK_QNO_EU_EASTEU_19_09
talk NPC_EU_WITCH resupply-spent @SN_TALK_QNO_EU_EASTEU_19_10
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_WITCH done-now @SN_TALK_QNO_EU_EASTEU_19_06

# QNO_EU_EASTEU_21.
# Cerberus: a party sharing experience and items (not-party _20); the Call of Hell (mission kind 4) used at Jupiter
# Temple by the party's leader takes the party into INS_QNO_EU_EASTEU_21 (_RefGame_World 11, 8 players), where
# MOB_QT_01_KERBEROS is killed. Its talk gives a lost Call again (_11, _12).
quest QNO_EU_EASTEU_21
start level 9-
start after all QNO_EU_EASTEU_20
start party
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH
finish NPC_EU_WITCH
start-item ITEM_QNO_EU_EASTEU_21_01 1
goal kill 1 MOB_QT_01_KERBEROS text @SN_CON_QNO_EU_EASTEU_21
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_EASTEU_21_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_EASTEU_21_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_EASTEU_21_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_EASTEU_21_04
talk NPC_EU_WITCH done @SN_TALK_QNO_EU_EASTEU_21_06
talk NPC_EU_WITCH bag-full @SN_TALK_QNO_EU_EASTEU_21_05
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_WITCH done-now @SN_TALK_QNO_EU_EASTEU_21_18
talk NPC_EU_WITCH middle @SN_TALK_QNO_EU_EASTEU_21_07
talk NPC_EU_WITCH not-party @SN_TALK_QNO_EU_EASTEU_21_20

# QNO_EU_ADVENTURER_1.
# Three times (offer-again _02). Shiny Moss from Evil Order Archers (a helper gathering) used at the Trojan horse raises
# MOB_QT_01_CRAB, whose kills drop the Yellow Eggs.
quest QNO_EU_ADVENTURER_1
start level 16-
repeat 3
giver NPC_EU_ADVENTURER
npc NPC_EU_ADVENTURER
finish NPC_EU_ADVENTURER
goal drop 5 ITEM_QNO_EU_ADVENTURER_1_02 from MOB_QT_01_CRAB 50% at NPC_EU_ADVENTURER text @SN_CON_QNO_EU_ADVENTURER_1
goal drop 5 ITEM_QNO_EU_ADVENTURER_1_01 from MOB_AM_ARCHER_CLON 50% at NPC_EU_ADVENTURER text @SN_CON_QNO_EU_ADVENTURER_1 hidden
talk NPC_EU_ADVENTURER offer @SN_TALK_QNO_EU_ADVENTURER_1_01
talk NPC_EU_ADVENTURER accepted @SN_TALK_QNO_EU_ADVENTURER_1_03
talk NPC_EU_ADVENTURER denied @SN_TALK_QNO_EU_ADVENTURER_1_04
talk NPC_EU_ADVENTURER not-done @SN_TALK_QNO_EU_ADVENTURER_1_05
talk NPC_EU_ADVENTURER done @SN_TALK_QNO_EU_ADVENTURER_1_06
talk NPC_EU_ADVENTURER greeting @SN_NPC_EU_ADVENTURER_QS
talk NPC_EU_ADVENTURER done-now @SN_TALK_QNO_EU_ADVENTURER_1_07
talk NPC_EU_ADVENTURER offer-again @SN_TALK_QNO_EU_ADVENTURER_1_02

# QNO_EU_GENERAL_1.
# A capture whose catch is an item: five nets set on the ground catch a Pirate Brigand (MOB_AM_ROGUE, the roll 20) as
# the Captured Pirate, kept for its minutes (_14 .. _16). The nets' daily renewal (_05, the row _06, _07; the day's
# spent _08): `resupply-daily start`.
quest QNO_EU_GENERAL_1
start level 13-
repeat 1
giver NPC_EU_GENERAL
npc NPC_EU_GENERAL
finish NPC_EU_GENERAL
start-item ITEM_QNO_EU_GENERAL_1_01 5
resupply ITEM_QNO_EU_GENERAL_1_01 5
resupply-daily start
pay-take ITEM_QNO_EU_GENERAL_1_01
pay-take ITEM_QNO_EU_GENERAL_1_02
goal drop 1 ITEM_QNO_EU_GENERAL_1_02 at NPC_EU_GENERAL text @SN_CON_QNO_EU_GENERAL_1
talk NPC_EU_GENERAL offer @SN_TALK_QNO_EU_GENERAL_1_01
talk NPC_EU_GENERAL accepted @SN_TALK_QNO_EU_GENERAL_1_02
talk NPC_EU_GENERAL denied @SN_TALK_QNO_EU_GENERAL_1_03
talk NPC_EU_GENERAL not-done @SN_TALK_QNO_EU_GENERAL_1_04
talk NPC_EU_GENERAL done @SN_TALK_QNO_EU_GENERAL_1_10
talk NPC_EU_GENERAL bag-full @SN_TALK_QNO_EU_GENERAL_1_09
talk NPC_EU_GENERAL resupply @SN_TALK_QNO_EU_GENERAL_1_05 row @SN_TALK_QNO_EU_GENERAL_1_06
talk NPC_EU_GENERAL resupply @SN_TALK_QNO_EU_GENERAL_1_07
talk NPC_EU_GENERAL resupply-spent @SN_TALK_QNO_EU_GENERAL_1_08
talk NPC_EU_GENERAL greeting @SN_NPC_EU_GENERAL_QS
talk NPC_EU_GENERAL done-now @SN_TALK_QNO_EU_GENERAL_1_11

# QNO_EU_GENERAL_2.
# A party sharing experience and items (not-party _10). Two gatherings: the Assault Commander's Spoil from
# MOB_QT_01_ROGUE at 100 % (counted), and the Pirate Flag from MOB_AM_ROGUE champions at 25 % (a helper; ours from any
# MOB_AM_ROGUE: a source has no class). The flag used at Halikarnassos raises the commander and his clone; it says _07
# and _11 when they are got.
quest QNO_EU_GENERAL_2
start level 13-
start after all QNO_EU_GENERAL_1
start party
repeat 1
giver NPC_EU_GENERAL
npc NPC_EU_GENERAL
finish NPC_EU_GENERAL
goal drop 1 ITEM_QNO_EU_GENERAL_2_03 from MOB_QT_01_ROGUE 100% at NPC_EU_GENERAL text @SN_CON_QNO_EU_GENERAL_2
goal drop 1 ITEM_QNO_EU_GENERAL_2_01 from MOB_AM_ROGUE 25% at NPC_EU_GENERAL text @SN_CON_QNO_EU_GENERAL_2 hidden
talk NPC_EU_GENERAL offer @SN_TALK_QNO_EU_GENERAL_2_01
talk NPC_EU_GENERAL accepted @SN_TALK_QNO_EU_GENERAL_2_02
talk NPC_EU_GENERAL denied @SN_TALK_QNO_EU_GENERAL_2_03
talk NPC_EU_GENERAL not-done @SN_TALK_QNO_EU_GENERAL_2_04
talk NPC_EU_GENERAL done @SN_TALK_QNO_EU_GENERAL_2_06
talk NPC_EU_GENERAL done-now @SN_TALK_QNO_EU_GENERAL_2_11
talk NPC_EU_GENERAL bag-full @SN_TALK_QNO_EU_GENERAL_2_05
talk NPC_EU_GENERAL greeting @SN_NPC_EU_GENERAL_QS
talk NPC_EU_GENERAL not-party @SN_TALK_QNO_EU_GENERAL_2_10

# QNO_EU_WITCH_1.
# The 60 Unopening Boxes GENERAL_5 brought her handed back (mission kind 4); the Blood of Evil Wolf (a helper gathering
# from MOB_AM_WOLF at 50 %, up to 5,000) poured on them (a roll: _09, _10) makes Curse
# Lifted Boxes, 60 of them the goal (mission kind 10).
quest QNO_EU_WITCH_1
start level 14-
start after all QNO_EU_GENERAL_5
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH
finish NPC_EU_WITCH
start-item ITEM_QNO_EU_GENERAL_5_01 60
goal drop 60 ITEM_QNO_EU_WITCH_1_01 text @SN_CON_QNO_EU_WITCH_1
goal drop 5000 ITEM_QNO_EU_WITCH_1_02 from MOB_AM_WOLF 50% at NPC_EU_WITCH text @SN_CON_QNO_EU_WITCH_1 hidden
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_WITCH_1_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_WITCH_1_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_WITCH_1_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_WITCH_1_04
talk NPC_EU_WITCH done @SN_TALK_QNO_EU_WITCH_1_07
talk NPC_EU_WITCH bag-full @SN_TALK_QNO_EU_WITCH_1_05
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_WITCH done-now @SN_TALK_QNO_EU_WITCH_1_08

# QNO_EU_IVY_1.
# Ratchel's: 15 Closier Guardians (MOB_QT_02_PUNISHER_CLON, raised round the holder of the informant each minute) and
# the Closier Informant (MOB_QT_01_PUNISHER_CLON) caught by IVY_2's trap as ITEM_QNO_EU_IVY_1_01 (the original's
# record names IVY_1_02, the reward's hammer).
quest QNO_EU_IVY_1
start level 20-
start after all QNO_CA_HUNTER_1
repeat 1
giver NPC_EU_GENERAL
npc NPC_EU_GENERAL
finish NPC_EU_GENERAL
goal drop 1 ITEM_QNO_EU_IVY_1_01 at NPC_EU_GENERAL text @SN_CON_QNO_EU_IVY_1_01
goal kill 15 MOB_QT_02_PUNISHER_CLON text @SN_CON_QNO_EU_IVY_1_02
pay-take ITEM_QNO_EU_IVY_1_01
talk NPC_EU_GENERAL offer @SN_TALK_QNO_EU_IVY_1_01
talk NPC_EU_GENERAL accepted @SN_TALK_QNO_EU_IVY_1_02
talk NPC_EU_GENERAL denied @SN_TALK_QNO_EU_IVY_1_03
talk NPC_EU_GENERAL not-done @SN_TALK_QNO_EU_IVY_1_04
talk NPC_EU_GENERAL done @SN_TALK_QNO_EU_IVY_1_06
talk NPC_EU_GENERAL bag-full @SN_TALK_QNO_EU_IVY_1_08
talk NPC_EU_GENERAL greeting @SN_NPC_EU_GENERAL_QS
talk NPC_EU_GENERAL done-now @SN_TALK_QNO_EU_IVY_1_16

# QNO_EU_IVY_2.
# Taken at Demetri while IVY_1 runs. Small Knives from Evil Order Soldiers (a helper; its record names NPC_CA_HORSE, a
# slip kept) cut Vine Stalks at the pond ruins (a search: game::OriginalUseRules);
# 14 of them (mission kind 10) brought back to Demetri, who pays five traps (_RefQuestRewardItems).
quest QNO_EU_IVY_2
start level 20-
start during QNO_EU_IVY_1
repeat 1
giver NPC_EU_ADVENTURER
npc NPC_EU_ADVENTURER
finish NPC_EU_ADVENTURER
goal drop 14 ITEM_QNO_EU_IVY_2_02 text @SN_CON_QNO_EU_IVY_2
goal drop 500 ITEM_QNO_EU_IVY_2_03 from MOB_AM_SOLDIER 25% at NPC_CA_HORSE text @SN_CON_QNO_EU_IVY_2 hidden
talk NPC_EU_ADVENTURER offer @SN_TALK_QNO_EU_IVY_2_01
talk NPC_EU_ADVENTURER accepted @SN_TALK_QNO_EU_IVY_2_02
talk NPC_EU_ADVENTURER denied @SN_TALK_QNO_EU_IVY_2_03
talk NPC_EU_ADVENTURER not-done @SN_TALK_QNO_EU_IVY_2_04
talk NPC_EU_ADVENTURER done @SN_TALK_QNO_EU_IVY_2_07
talk NPC_EU_ADVENTURER bag-full @SN_TALK_QNO_EU_IVY_2_06
talk NPC_EU_ADVENTURER greeting @SN_NPC_EU_ADVENTURER_QS
talk NPC_EU_ADVENTURER done-now @SN_TALK_QNO_EU_IVY_2_08

# QNO_EU_IVY_3.
# A party sharing experience and items (not-party _23). Five Horn Trumpets (mission kind 4) used at the three places
# raise the sign keepers, whose kills drop the three signs; with the Punisher's Blood (MOB_AM_PUNISHER, 5 %) they make
# the Key of Sign, which takes the party into INS_QNO_EU_IVY_3 (_RefGame_World 12) to Ivy (MOB_QT_01_IVY), whose Red Hair
# is the goal. The original counts the signs too; they are spent on the key: ours keeps them as helpers.
quest QNO_EU_IVY_3
start level 20-
start after all QNO_EU_IVY_1
start party
repeat 1
giver NPC_EU_GENERAL
npc NPC_EU_GENERAL
finish NPC_EU_GENERAL
start-item ITEM_QNO_EU_IVY_3_01 5
resupply ITEM_QNO_EU_IVY_3_01 5
resupply-daily start
goal drop 1 ITEM_QNO_EU_IVY_3_08 from MOB_QT_01_IVY 100% at NPC_EU_GENERAL text @SN_CON_QNO_EU_IVY_3
goal drop 1 ITEM_QNO_EU_IVY_3_02 from MOB_QT_01_SOLDIER_CLON 100% at NPC_EU_GENERAL text @SN_CON_QNO_EU_IVY_3 hidden
goal drop 1 ITEM_QNO_EU_IVY_3_03 from MOB_QT_03_PUNISHER_CLON 100% at NPC_EU_GENERAL text @SN_CON_QNO_EU_IVY_3 hidden
goal drop 1 ITEM_QNO_EU_IVY_3_04 from MOB_QT_01_PUNISHER 100% at NPC_EU_GENERAL text @SN_CON_QNO_EU_IVY_3 hidden
goal drop 1 ITEM_QNO_EU_IVY_3_05 from MOB_AM_PUNISHER 5% at NPC_EU_GENERAL text @SN_CON_QNO_EU_IVY_3 hidden
talk NPC_EU_GENERAL offer @SN_TALK_QNO_EU_IVY_3_01
talk NPC_EU_GENERAL accepted @SN_TALK_QNO_EU_IVY_3_02
talk NPC_EU_GENERAL denied @SN_TALK_QNO_EU_IVY_3_03
talk NPC_EU_GENERAL not-done @SN_TALK_QNO_EU_IVY_3_04
talk NPC_EU_GENERAL done @SN_TALK_QNO_EU_IVY_3_06
talk NPC_EU_GENERAL bag-full @SN_TALK_QNO_EU_IVY_3_05
talk NPC_EU_GENERAL resupply @SN_TALK_QNO_EU_IVY_3_16 row @SN_TALK_QNO_EU_IVY_3_17
talk NPC_EU_GENERAL resupply @SN_TALK_QNO_EU_IVY_3_18
talk NPC_EU_GENERAL resupply-spent @SN_TALK_QNO_EU_IVY_3_19
talk NPC_EU_GENERAL greeting @SN_NPC_EU_GENERAL_QS
talk NPC_EU_GENERAL done-now @SN_TALK_QNO_EU_IVY_3_20
talk NPC_EU_GENERAL not-party @SN_TALK_QNO_EU_IVY_3_23

# QNO_CH_HWAN_1_1.
# The first army test: race only here. It sends the player into the quest's own instance INS_QNO_CH_HWAN_1_1
# (_RefGame_World, one player; 720 s) to beat MOB_QT_CH_SOLDIER_01_1, a champion. The trial (the general's row _05: the
# general at 10 s, a soldier every 30 s for 5 minutes, passed at its death or after 10 minutes, lost when dead).
quest QNO_CH_HWAN_1_1
start level 95-
start race china
repeat 1
giver NPC_CH_GENARAL
npc NPC_CH_GENARAL
finish NPC_CH_GENARAL
goal kill 1 MOB_QT_CH_SOLDIER_01_1 class champion text @SN_CON_QNO_CH_HWAN_1_1
trial INS_QNO_CH_HWAN_1_1 at NPC_CH_GENARAL row @SN_TALK_QNO_CH_HWAN_1_1_05 start 22966 1043 52 1087 refuse @SN_TALK_QNO_CH_HWAN_1_1_10 no-job after 10 boss MOB_QT_CH_SOLDIER_01_1 22966 893 52 1087 radius 90 100 minutes 10 warn 5 @SN_TALK_QNO_CH_HWAN_1_1_11 adds MOB_QT_CH_SOLDIER_01_2 22966 893 52 1087 radius 90 100 every 30 pass-on-time passed @SN_TALK_QNO_CH_HWAN_1_1_09 lost @SN_TALK_QNO_CH_HWAN_1_1_08
talk NPC_CH_GENARAL trial @SN_TALK_QNO_CH_HWAN_1_1_04
talk NPC_CH_GENARAL offer @SN_TALK_QNO_CH_HWAN_1_1_01
talk NPC_CH_GENARAL accepted @SN_TALK_QNO_CH_HWAN_1_1_02
talk NPC_CH_GENARAL denied @SN_TALK_QNO_CH_HWAN_1_1_03
talk NPC_CH_GENARAL not-done @SN_TALK_QNO_CH_HWAN_1_1_04
talk NPC_CH_GENARAL done @SN_TALK_QNO_CH_HWAN_1_1_07
talk NPC_CH_GENARAL greeting @SN_NPC_CH_GENARAL_QS
talk NPC_CH_GENARAL done-now @SN_TALK_QNO_CH_HWAN_1_1_09

# QNO_CH_HWAN_1_2.
# A talk with the next NPC, done there.
quest QNO_CH_HWAN_1_2
start level 95-
start after all QNO_CH_HWAN_1_1
repeat 1
giver NPC_CH_GENARAL
npc NPC_CH_GENARAL NPC_CH_SHAMAN
finish NPC_CH_SHAMAN
goal talk NPC_CH_SHAMAN talk @SN_TALK_QNO_CH_HWAN_1_2_05 text @SN_CON_QNO_CH_HWAN_1_2
talk NPC_CH_GENARAL greeting @SN_NPC_CH_GENARAL_QS
talk NPC_CH_SHAMAN greeting @SN_NPC_CH_SHAMAN_QS
talk NPC_CH_GENARAL offer @SN_TALK_QNO_CH_HWAN_1_2_01
talk NPC_CH_GENARAL accepted @SN_TALK_QNO_CH_HWAN_1_2_02
talk NPC_CH_GENARAL denied @SN_TALK_QNO_CH_HWAN_1_2_03
talk NPC_CH_GENARAL not-done @SN_TALK_QNO_CH_HWAN_1_2_04
talk NPC_CH_SHAMAN not-done @SN_TALK_QNO_CH_HWAN_1_2_04
talk NPC_CH_GENARAL middle @SN_TALK_QNO_CH_HWAN_1_2_06

# QNO_CH_HWAN_1_3.
# The Stone Beast's Bell from the tomb's ghosts at 50 %, brought to the priest.
quest QNO_CH_HWAN_1_3
start level 95-
start after all QNO_CH_HWAN_1_2
repeat 1
giver NPC_CH_SHAMAN
npc NPC_CH_SHAMAN NPC_CH_PRIEST
finish NPC_CH_PRIEST
goal drop 1 ITEM_QNO_CH_HWAN_1_3_01 from MOB_TQ_TOMBSTONEGHOST_CLON2 50% MOB_TQ_TOMBSTONEGHOST_CLON 50% MOB_TQ_TOMBSTONEGHOST 50% at NPC_CH_PRIEST text @SN_CON_QNO_CH_HWAN_1_3
talk NPC_CH_SHAMAN greeting @SN_NPC_CH_SHAMAN_QS
talk NPC_CH_PRIEST greeting @SN_NPC_CH_PRIEST_QS
talk NPC_CH_SHAMAN offer @SN_TALK_QNO_CH_HWAN_1_3_01
talk NPC_CH_SHAMAN accepted @SN_TALK_QNO_CH_HWAN_1_3_02
talk NPC_CH_SHAMAN denied @SN_TALK_QNO_CH_HWAN_1_3_03
talk NPC_CH_SHAMAN not-done @SN_TALK_QNO_CH_HWAN_1_3_04
talk NPC_CH_PRIEST not-done @SN_TALK_QNO_CH_HWAN_1_3_04
talk NPC_CH_PRIEST done @SN_TALK_QNO_CH_HWAN_1_3_05
talk NPC_CH_PRIEST done-now @SN_TALK_QNO_CH_HWAN_1_3_07
talk NPC_CH_SHAMAN middle @SN_TALK_QNO_CH_HWAN_1_3_06

# QNO_CH_HWAN_1_3_1.
# The priest purifies the bell (its talk gives the Spirit's Bell at the accept, the player's answer _02); the bell used
# at the cemetery (region 23980) raises MOB_QT_CH_TOMBSTONE_01, whose death gives the Spirit's Shell (its kill; the
# record's chance 0).
quest QNO_CH_HWAN_1_3_1
start level 95-
start after all QNO_CH_HWAN_1_3
repeat 1
giver NPC_CH_PRIEST
npc NPC_CH_PRIEST NPC_CH_SHAMAN
finish NPC_CH_SHAMAN
start-item ITEM_QNO_CH_HWAN_1_3_02 1
goal drop 1 ITEM_QNO_CH_HWAN_1_3_03 from MOB_QT_CH_TOMBSTONE_01 0% at NPC_CH_SHAMAN text @SN_CON_QNO_CH_HWAN_1_3_1
talk NPC_CH_PRIEST greeting @SN_NPC_CH_PRIEST_QS
talk NPC_CH_SHAMAN greeting @SN_NPC_CH_SHAMAN_QS
talk NPC_CH_PRIEST offer @SN_TALK_QNO_CH_HWAN_1_3_1_01
talk NPC_CH_PRIEST accepted @SN_TALK_QNO_CH_HWAN_1_3_1_03
talk NPC_CH_PRIEST denied @SN_TALK_QNO_CH_HWAN_1_3_1_04
talk NPC_CH_PRIEST not-done @SN_TALK_QNO_CH_HWAN_1_3_1_05
talk NPC_CH_SHAMAN not-done @SN_TALK_QNO_CH_HWAN_1_3_1_05
talk NPC_CH_SHAMAN done @SN_TALK_QNO_CH_HWAN_1_3_1_07
talk NPC_CH_SHAMAN bag-full @SN_TALK_QNO_CH_HWAN_1_3_1_06
talk NPC_CH_SHAMAN done-now @SN_TALK_QNO_CH_HWAN_1_3_1_09
talk NPC_CH_PRIEST middle @SN_TALK_QNO_CH_HWAN_1_3_1_08

# QNO_CH_HWAN_1_4.
# A charm handed over at the start and taken to the next NPC; done there.
quest QNO_CH_HWAN_1_4
start level 95-
start after all QNO_CH_HWAN_1_3_1
repeat 1
giver NPC_CH_SHAMAN
npc NPC_CH_SHAMAN NPC_CH_GENARAL_SW
finish NPC_CH_GENARAL_SW
start-item ITEM_QNO_CH_HWAN_1_4_03 1
goal deliver NPC_CH_GENARAL_SW ITEM_QNO_CH_HWAN_1_4_03 1 talk @SN_TALK_QNO_CH_HWAN_1_4_05 text @SN_CON_QNO_CH_HWAN_1_4
talk NPC_CH_SHAMAN greeting @SN_NPC_CH_SHAMAN_QS
talk NPC_CH_GENARAL_SW greeting @SN_NPC_CH_GENARAL_SW_QS
talk NPC_CH_SHAMAN offer @SN_TALK_QNO_CH_HWAN_1_4_01
talk NPC_CH_SHAMAN accepted @SN_TALK_QNO_CH_HWAN_1_4_02
talk NPC_CH_SHAMAN denied @SN_TALK_QNO_CH_HWAN_1_4_03
talk NPC_CH_SHAMAN not-done @SN_TALK_QNO_CH_HWAN_1_4_07
talk NPC_CH_GENARAL_SW not-done @SN_TALK_QNO_CH_HWAN_1_4_07
talk NPC_CH_SHAMAN bag-full @SN_TALK_QNO_CH_HWAN_1_4_04
talk NPC_CH_SHAMAN middle @SN_TALK_QNO_CH_HWAN_1_4_06

# QNO_CH_HWAN_1_4_1.
# A trap set on the ground (100 traps) catches a Niya General's spirit (the roll 30): a Normal Spirit (the goal) or a
# Flutter Spirit (a helper, paid extra by its talk).
quest QNO_CH_HWAN_1_4_1
start level 95-
start after all QNO_CH_HWAN_1_4
repeat 1
giver NPC_CH_GENARAL_SW
npc NPC_CH_GENARAL_SW NPC_CH_SHAMAN
finish NPC_CH_SHAMAN
start-item ITEM_QNO_CH_HWAN_1_4_04 100
goal drop 1 ITEM_QNO_CH_HWAN_1_4_01 at NPC_CH_SHAMAN text @SN_CON_QNO_CH_HWAN_1_4_1
goal drop 1 ITEM_QNO_CH_HWAN_1_4_02 from MOB_TK_BONEGENERAL 0% at NPC_CH_SHAMAN text @SN_CON_QNO_CH_HWAN_1_4_1 hidden
talk NPC_CH_GENARAL_SW greeting @SN_NPC_CH_GENARAL_SW_QS
talk NPC_CH_SHAMAN greeting @SN_NPC_CH_SHAMAN_QS
talk NPC_CH_GENARAL_SW offer @SN_TALK_QNO_CH_HWAN_1_4_1_01
talk NPC_CH_GENARAL_SW accepted @SN_TALK_QNO_CH_HWAN_1_4_1_02
talk NPC_CH_GENARAL_SW denied @SN_TALK_QNO_CH_HWAN_1_4_1_10
talk NPC_CH_GENARAL_SW not-done @SN_TALK_QNO_CH_HWAN_1_4_1_03
talk NPC_CH_SHAMAN not-done @SN_TALK_QNO_CH_HWAN_1_4_1_03
talk NPC_CH_SHAMAN done @SN_TALK_QNO_CH_HWAN_1_4_1_06
talk NPC_CH_GENARAL_SW bag-full @SN_TALK_QNO_EU_EASTEU_14_1_06
talk NPC_CH_GENARAL_SW middle @SN_TALK_QNO_CH_HWAN_1_4_1_07

# QNO_CH_HWAN_1_5.
# The exorcist's charm (mission kind 4); it sends the player into INS_QNO_CH_HWAN_1_5 to beat MOB_QT_CH_CHARGHOST_01 (a
# champion). The ritual (row _04: the ghost at 12 s, 10 minutes; its kill counted only in berserk; its death in berserk
# swaps the charm for the Piece of Spirit, else _09), the piece taken at the hand-in (the original takes it after the
# pay; ours asks it there).
quest QNO_CH_HWAN_1_5
start level 95-
start after all QNO_CH_HWAN_1_4_1
repeat 1
giver NPC_CH_SHAMAN
npc NPC_CH_SHAMAN
finish NPC_CH_SHAMAN
start-item ITEM_QNO_CH_HWAN_1_5_01 1
goal kill 1 MOB_QT_CH_CHARGHOST_01 class champion berserk text @SN_CON_QNO_CH_HWAN_1_5
trial INS_QNO_CH_HWAN_1_5 at NPC_CH_SHAMAN row @SN_TALK_QNO_CH_HWAN_1_5_04 start 25253 152 241 811 refuse @SN_TALK_QNO_CH_HWAN_1_1_10 after 12 boss MOB_QT_CH_CHARGHOST_01 25253 177 233 684 radius 0 0 minutes 10 warn 5 @SN_TALK_QNO_CH_HWAN_1_5_10 berserk take ITEM_QNO_CH_HWAN_1_5_01 give ITEM_QNO_CH_HWAN_1_5_02 failed @SN_TALK_QNO_CH_HWAN_1_5_09 out 10
need item ITEM_QNO_CH_HWAN_1_5_02 1
talk NPC_CH_SHAMAN trial @SN_TALK_QNO_CH_HWAN_1_5_03
talk NPC_CH_SHAMAN offer @SN_TALK_QNO_CH_HWAN_1_5_01
talk NPC_CH_SHAMAN accepted @SN_TALK_QNO_CH_HWAN_1_5_02
talk NPC_CH_SHAMAN denied @SN_TALK_QNO_CH_HWAN_1_5_05
talk NPC_CH_SHAMAN not-done @SN_TALK_QNO_CH_HWAN_1_5_04
talk NPC_CH_SHAMAN done @SN_TALK_QNO_CH_HWAN_1_5_06
talk NPC_CH_SHAMAN bag-full @SN_TALK_QNO_CH_HWAN_1_5_08
talk NPC_CH_SHAMAN greeting @SN_NPC_CH_SHAMAN_QS
talk NPC_CH_SHAMAN done-now @SN_TALK_QNO_CH_HWAN_1_5_07

# QNO_CH_HWAN_1_6.
# The Piece of Spirit handed over at the start and taken to the general (the delivery's conversation: _06, _08 .. _07,
# _09; ours its first line). _RefQuestReward's Hwan 1: the reward raises the player's hwan level (ours
# QuestHost::Reward::hwan).
quest QNO_CH_HWAN_1_6
start level 95-
start after all QNO_CH_HWAN_1_5
repeat 1
giver NPC_CH_SHAMAN
npc NPC_CH_SHAMAN NPC_CH_GENARAL
finish NPC_CH_GENARAL
start-item ITEM_QNO_CH_HWAN_1_5_02 1
goal deliver NPC_CH_GENARAL ITEM_QNO_CH_HWAN_1_5_02 1 talk @SN_TALK_QNO_CH_HWAN_1_6_06 text @SN_CON_QNO_CH_HWAN_1_6
talk NPC_CH_SHAMAN greeting @SN_NPC_CH_SHAMAN_QS
talk NPC_CH_GENARAL greeting @SN_NPC_CH_GENARAL_QS
talk NPC_CH_SHAMAN offer @SN_TALK_QNO_CH_HWAN_1_6_01
talk NPC_CH_SHAMAN accepted @SN_TALK_QNO_CH_HWAN_1_6_02
talk NPC_CH_SHAMAN denied @SN_TALK_QNO_CH_HWAN_1_6_03
talk NPC_CH_SHAMAN not-done @SN_TALK_QNO_CH_HWAN_1_6_04
talk NPC_CH_GENARAL not-done @SN_TALK_QNO_CH_HWAN_1_6_04
talk NPC_CH_SHAMAN bag-full @SN_TALK_QNO_CH_HWAN_1_6_05
talk NPC_CH_SHAMAN middle @SN_TALK_QNO_CH_HWAN_1_6_10

# QNO_CH_HWAN_2_1.
# The second army test: 50 Terminate Traps (mission kind 8, a helper) and the instance INS_QNO_CH_HWAN_2_1, where the
# trapped MOB_QT_CH_SOLDIER_02_1 (a champion) calls its doctor. The trial as 1_1's (the doctor MOB_QT_CH_SOLDIER_02_2
# with the general, its next 600 s on; a trap holds either one 10 s: not built).
quest QNO_CH_HWAN_2_1
start level 100-
start after all QNO_CH_HWAN_1_6
repeat 1
giver NPC_CH_GENARAL
npc NPC_CH_GENARAL
finish NPC_CH_GENARAL
start-item ITEM_QNO_CH_WHAN_2_1_01 50
goal kill 1 MOB_QT_CH_SOLDIER_02_1 class champion text @SN_CON_QNO_CH_HWAN_2_1
trial INS_QNO_CH_HWAN_2_1 at NPC_CH_GENARAL row @SN_TALK_QNO_CH_HWAN_2_1_05 start 22966 1043 52 1087 refuse @SN_TALK_QNO_CH_HWAN_1_1_10 no-job after 10 boss MOB_QT_CH_SOLDIER_02_1 22966 893 52 1087 radius 90 100 minutes 10 warn 5 @SN_TALK_QNO_CH_HWAN_2_1_14 adds MOB_QT_CH_SOLDIER_02_2 22966 893 52 1087 radius 90 100 every 600 pass-on-time passed @SN_TALK_QNO_CH_HWAN_2_1_12 lost @SN_TALK_QNO_CH_HWAN_2_1_11
talk NPC_CH_GENARAL trial @SN_TALK_QNO_CH_HWAN_2_1_04
goal drop 1 ITEM_QNO_CH_WHAN_2_1_01 at NPC_CH_GENARAL text @SN_CON_QNO_CH_HWAN_2_1 hidden
talk NPC_CH_GENARAL offer @SN_TALK_QNO_CH_HWAN_2_1_01
talk NPC_CH_GENARAL accepted @SN_TALK_QNO_CH_HWAN_2_1_03
talk NPC_CH_GENARAL denied @SN_TALK_QNO_CH_HWAN_2_1_02
talk NPC_CH_GENARAL not-done @SN_TALK_QNO_CH_HWAN_2_1_04
talk NPC_CH_GENARAL done @SN_TALK_QNO_CH_HWAN_2_1_10
talk NPC_CH_GENARAL bag-full @SN_TALK_QNO_CH_HWAN_2_1_06
talk NPC_CH_GENARAL greeting @SN_NPC_CH_GENARAL_QS
talk NPC_CH_GENARAL done-now @SN_TALK_QNO_CH_HWAN_2_1_12

# QNO_CH_HWAN_2_2.
# A talk with the next NPC, done there.
quest QNO_CH_HWAN_2_2
start level 100-
start after all QNO_CH_HWAN_2_1
repeat 1
giver NPC_CH_GENARAL
npc NPC_CH_GENARAL NPC_CH_SHAMAN
finish NPC_CH_SHAMAN
goal talk NPC_CH_SHAMAN talk @SN_TALK_QNO_CH_HWAN_2_2_05 text @SN_CON_QNO_CH_HWAN_2_2
talk NPC_CH_GENARAL greeting @SN_NPC_CH_GENARAL_QS
talk NPC_CH_SHAMAN greeting @SN_NPC_CH_SHAMAN_QS
talk NPC_CH_GENARAL offer @SN_TALK_QNO_CH_HWAN_2_2_01
talk NPC_CH_GENARAL accepted @SN_TALK_QNO_CH_HWAN_2_2_02
talk NPC_CH_GENARAL denied @SN_TALK_QNO_CH_HWAN_2_2_03
talk NPC_CH_GENARAL not-done @SN_TALK_QNO_CH_HWAN_2_2_04
talk NPC_CH_SHAMAN not-done @SN_TALK_QNO_CH_HWAN_2_2_04
talk NPC_CH_GENARAL middle @SN_TALK_QNO_CH_HWAN_2_2_06

# QNO_CH_HWAN_2_3.
# Energy of Mountain from the goats (a helper, 100 %); used on a selected MOB_CH_YEOHA (within 300) it
# becomes the Branch of Life (mission kind 10), the goal.
quest QNO_CH_HWAN_2_3
start level 100-
start after all QNO_CH_HWAN_2_2
repeat 1
giver NPC_CH_SHAMAN
npc NPC_CH_SHAMAN
finish NPC_CH_SHAMAN
goal drop 1 ITEM_QNO_CH_WHAN_2_3_02 text @SN_CON_QNO_CH_HWAN_2_3
goal drop 100 ITEM_QNO_CH_WHAN_2_3_01 from MOB_RM_GOAT_CLON 100% MOB_RM_GOAT 100% at NPC_CH_SHAMAN text @SN_CON_QNO_CH_HWAN_2_3 hidden
talk NPC_CH_SHAMAN offer @SN_TALK_QNO_CH_HWAN_2_3_01
talk NPC_CH_SHAMAN accepted @SN_TALK_QNO_CH_HWAN_2_3_02
talk NPC_CH_SHAMAN denied @SN_TALK_QNO_CH_HWAN_2_3_03
talk NPC_CH_SHAMAN not-done @SN_TALK_QNO_CH_HWAN_2_3_04
talk NPC_CH_SHAMAN done @SN_TALK_QNO_CH_HWAN_2_3_05
talk NPC_CH_SHAMAN bag-full @SN_TALK_QNO_TQ_BAEKRYEONG_4_09
talk NPC_CH_SHAMAN greeting @SN_NPC_CH_SHAMAN_QS
talk NPC_CH_SHAMAN done-now @SN_TALK_QNO_CH_HWAN_2_3_08
talk NPC_CH_SHAMAN middle @SN_TALK_QNO_CH_HWAN_2_3_06

# QNO_CH_HWAN_2_4.
# Shining Crystals from the ishades (a helper, 100 %); used at the place (a search, 11 in 101) one
# becomes the Relic of Sun God (mission kind 10), the goal: game::OriginalUseRules.
quest QNO_CH_HWAN_2_4
start level 100-
start after all QNO_CH_HWAN_2_3
repeat 1
giver NPC_CH_SHAMAN
npc NPC_CH_SHAMAN
finish NPC_CH_SHAMAN
goal drop 1 ITEM_QNO_CH_WHAN_2_4_02 text @SN_CON_QNO_CH_HWAN_2_4
goal drop 100 ITEM_QNO_CH_WHAN_2_4_01 from MOB_KK_ISHADE 100% MOB_KK_ISHADE_CLON 100% at NPC_CH_SHAMAN text @SN_CON_QNO_CH_HWAN_2_4 hidden
talk NPC_CH_SHAMAN offer @SN_TALK_QNO_CH_HWAN_2_4_01
talk NPC_CH_SHAMAN accepted @SN_TALK_QNO_CH_HWAN_2_4_02
talk NPC_CH_SHAMAN denied @SN_TALK_QNO_CH_HWAN_2_4_03
talk NPC_CH_SHAMAN not-done @SN_TALK_QNO_CH_HWAN_2_4_04
talk NPC_CH_SHAMAN done @SN_TALK_QNO_CH_HWAN_2_4_05
talk NPC_CH_SHAMAN greeting @SN_NPC_CH_SHAMAN_QS
talk NPC_CH_SHAMAN done-now @SN_TALK_QNO_CH_HWAN_2_4_08
talk NPC_CH_SHAMAN middle @SN_TALK_QNO_CH_HWAN_2_4_06

# QNO_CH_HWAN_2_5.
# The charm (mission kind 4) and the instance INS_QNO_CH_HWAN_2_5 where MOB_QT_CH_CHARGHOST_02 is beaten. The ritual as
# 1_5's.
quest QNO_CH_HWAN_2_5
start level 100-
start after all QNO_CH_HWAN_2_4
repeat 1
giver NPC_CH_SHAMAN
npc NPC_CH_SHAMAN
finish NPC_CH_SHAMAN
start-item ITEM_QNO_CH_HWAN_2_5_01 1
goal kill 1 MOB_QT_CH_CHARGHOST_02 berserk text @SN_CON_QNO_CH_HWAN_2_5
trial INS_QNO_CH_HWAN_2_5 at NPC_CH_SHAMAN row @SN_TALK_QNO_CH_HWAN_2_5_04 start 25253 152 241 811 refuse @SN_TALK_QNO_CH_HWAN_1_1_10 after 12 boss MOB_QT_CH_CHARGHOST_02 25253 177 233 684 radius 0 0 minutes 10 warn 5 @SN_TALK_QNO_CH_HWAN_2_5_10 berserk take ITEM_QNO_CH_HWAN_2_5_01 give ITEM_QNO_CH_HWAN_2_5_02 failed @SN_TALK_QNO_CH_HWAN_2_5_09 out 10
need item ITEM_QNO_CH_HWAN_2_5_02 1
talk NPC_CH_SHAMAN trial @SN_TALK_QNO_CH_HWAN_2_5_03
talk NPC_CH_SHAMAN offer @SN_TALK_QNO_CH_HWAN_2_5_01
talk NPC_CH_SHAMAN accepted @SN_TALK_QNO_CH_HWAN_2_5_02
talk NPC_CH_SHAMAN denied @SN_TALK_QNO_CH_HWAN_2_5_05
talk NPC_CH_SHAMAN not-done @SN_TALK_QNO_CH_HWAN_2_5_03
talk NPC_CH_SHAMAN done @SN_TALK_QNO_CH_HWAN_2_5_07
talk NPC_CH_SHAMAN bag-full @SN_TALK_QNO_CH_HWAN_2_5_06
talk NPC_CH_SHAMAN greeting @SN_NPC_CH_SHAMAN_QS
talk NPC_CH_SHAMAN done-now @SN_TALK_QNO_CH_HWAN_2_5_08

# QNO_CH_HWAN_2_6.
# The piece handed over at the start and taken to the general. _RefQuestReward's Hwan 2 (as 1_6).
quest QNO_CH_HWAN_2_6
start level 100-
start after all QNO_CH_HWAN_2_5
repeat 1
giver NPC_CH_SHAMAN
npc NPC_CH_SHAMAN NPC_CH_GENARAL
finish NPC_CH_GENARAL
start-item ITEM_QNO_CH_HWAN_2_5_02 1
goal deliver NPC_CH_GENARAL ITEM_QNO_CH_HWAN_2_5_02 1 talk @SN_TALK_QNO_CH_HWAN_2_6_06 text @SN_CON_QNO_CH_HWAN_2_6
talk NPC_CH_SHAMAN greeting @SN_NPC_CH_SHAMAN_QS
talk NPC_CH_GENARAL greeting @SN_NPC_CH_GENARAL_QS
talk NPC_CH_SHAMAN offer @SN_TALK_QNO_CH_HWAN_2_6_01
talk NPC_CH_SHAMAN accepted @SN_TALK_QNO_CH_HWAN_2_6_02
talk NPC_CH_SHAMAN denied @SN_TALK_QNO_CH_HWAN_2_6_03
talk NPC_CH_SHAMAN not-done @SN_TALK_QNO_CH_HWAN_2_6_04
talk NPC_CH_GENARAL not-done @SN_TALK_QNO_CH_HWAN_2_6_04
talk NPC_CH_SHAMAN bag-full @SN_TALK_QNO_CH_HWAN_2_6_05
talk NPC_CH_SHAMAN middle @SN_TALK_QNO_CH_HWAN_2_6_10

# QNO_EU_HWAN_1_1.
# The first army test: race only here. It sends the player into the quest's own instance INS_QNO_EU_HWAN_1_1
# (_RefGame_World, one player; 720 s) to beat MOB_QT_EU_SOLDIER_01_1, a champion. The trial as China's (the soldiers at
# their own place).
quest QNO_EU_HWAN_1_1
start level 95-
start race europe
repeat 1
giver NPC_EU_GENERAL
npc NPC_EU_GENERAL
finish NPC_EU_GENERAL
goal kill 1 MOB_QT_EU_SOLDIER_01_1 class champion text @SN_CON_QNO_EU_HWAN_1_1
trial INS_QNO_EU_HWAN_1_1 at NPC_EU_GENERAL row @SN_TALK_QNO_EU_HWAN_1_1_05 start 22454 1195 91 653 refuse @SN_TALK_QNO_CH_HWAN_1_1_10 no-job after 10 boss MOB_QT_EU_SOLDIER_01_1 22454 1102 91 704 radius 90 100 minutes 10 warn 5 @SN_TALK_QNO_EU_HWAN_1_1_11 adds MOB_QT_EU_SOLDIER_01_2 22454 1149 91 653 radius 90 100 every 30 pass-on-time passed @SN_TALK_QNO_EU_HWAN_1_1_09 lost @SN_TALK_QNO_EU_HWAN_1_1_08
talk NPC_EU_GENERAL trial @SN_TALK_QNO_EU_HWAN_1_1_04
talk NPC_EU_GENERAL offer @SN_TALK_QNO_EU_HWAN_1_1_01
talk NPC_EU_GENERAL accepted @SN_TALK_QNO_EU_HWAN_1_1_02
talk NPC_EU_GENERAL denied @SN_TALK_QNO_EU_HWAN_1_1_03
talk NPC_EU_GENERAL not-done @SN_TALK_QNO_EU_HWAN_1_1_04
talk NPC_EU_GENERAL done @SN_TALK_QNO_EU_HWAN_1_1_07
talk NPC_EU_GENERAL greeting @SN_NPC_EU_GENERAL_QS
talk NPC_EU_GENERAL done-now @SN_TALK_QNO_EU_HWAN_1_1_09

# QNO_EU_HWAN_1_2.
# A talk with the next NPC, done there.
quest QNO_EU_HWAN_1_2
start level 95-
start after all QNO_EU_HWAN_1_1
repeat 1
giver NPC_EU_GENERAL
npc NPC_EU_GENERAL NPC_EU_WITCH
finish NPC_EU_WITCH
goal talk NPC_EU_WITCH talk @SN_TALK_QNO_EU_HWAN_1_2_05 text @SN_CON_QNO_EU_HWAN_1_2
talk NPC_EU_GENERAL greeting @SN_NPC_EU_GENERAL_QS
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_GENERAL offer @SN_TALK_QNO_EU_HWAN_1_2_01
talk NPC_EU_GENERAL accepted @SN_TALK_QNO_EU_HWAN_1_2_02
talk NPC_EU_GENERAL denied @SN_TALK_QNO_EU_HWAN_1_2_03
talk NPC_EU_GENERAL not-done @SN_TALK_QNO_EU_HWAN_1_2_04
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_HWAN_1_2_04
talk NPC_EU_GENERAL middle @SN_TALK_QNO_EU_HWAN_1_2_06

# QNO_EU_HWAN_1_3.
# The Stone Beast's Bell from the tomb's ghosts at 50 %, brought to the priest.
quest QNO_EU_HWAN_1_3
start level 95-
start after all QNO_EU_HWAN_1_2
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH NPC_CH_PRIEST
finish NPC_CH_PRIEST
goal drop 1 ITEM_QNO_CH_HWAN_1_3_01 from MOB_TQ_TOMBSTONEGHOST_CLON2 50% MOB_TQ_TOMBSTONEGHOST_CLON 50% MOB_TQ_TOMBSTONEGHOST 50% at NPC_CH_PRIEST text @SN_CON_QNO_EU_HWAN_1_3
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_CH_PRIEST greeting @SN_NPC_CH_PRIEST_QS
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_HWAN_1_3_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_HWAN_1_3_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_HWAN_1_3_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_HWAN_1_3_04
talk NPC_CH_PRIEST not-done @SN_TALK_QNO_EU_HWAN_1_3_04
talk NPC_CH_PRIEST done @SN_TALK_QNO_EU_HWAN_1_3_05
talk NPC_CH_PRIEST done-now @SN_TALK_QNO_EU_HWAN_1_3_07
talk NPC_EU_WITCH middle @SN_TALK_QNO_EU_HWAN_1_3_06

# QNO_EU_HWAN_1_3_1.
# The priest purifies the bell (its talk gives the Spirit's Bell at the accept, the player's answer _02); the bell used
# at the cemetery (region 23980) raises MOB_QT_CH_TOMBSTONE_01, whose death gives the Spirit's Shell (its kill; the
# record's chance 0).
quest QNO_EU_HWAN_1_3_1
start level 95-
start after all QNO_EU_HWAN_1_3
repeat 1
giver NPC_CH_PRIEST
npc NPC_CH_PRIEST NPC_EU_WITCH
finish NPC_EU_WITCH
start-item ITEM_QNO_CH_HWAN_1_3_02 1
goal drop 1 ITEM_QNO_CH_HWAN_1_3_03 from MOB_QT_CH_TOMBSTONE_01 0% at NPC_EU_WITCH text @SN_CON_QNO_EU_HWAN_1_3_1
talk NPC_CH_PRIEST greeting @SN_NPC_CH_PRIEST_QS
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_CH_PRIEST offer @SN_TALK_QNO_EU_HWAN_1_3_1_01
talk NPC_CH_PRIEST accepted @SN_TALK_QNO_EU_HWAN_1_3_1_03
talk NPC_CH_PRIEST denied @SN_TALK_QNO_EU_HWAN_1_3_1_04
talk NPC_CH_PRIEST not-done @SN_TALK_QNO_EU_HWAN_1_3_1_05
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_HWAN_1_3_1_05
talk NPC_EU_WITCH done @SN_TALK_QNO_EU_HWAN_1_3_1_07
talk NPC_EU_WITCH bag-full @SN_TALK_QNO_EU_HWAN_1_3_1_06
talk NPC_EU_WITCH done-now @SN_TALK_QNO_EU_HWAN_1_3_1_09
talk NPC_CH_PRIEST middle @SN_TALK_QNO_EU_HWAN_1_3_1_08

# QNO_EU_HWAN_1_4.
# A charm handed over at the start and taken to the next NPC; done there.
quest QNO_EU_HWAN_1_4
start level 95-
start after all QNO_EU_HWAN_1_3_1
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH NPC_EU_HUNTER
finish NPC_EU_HUNTER
start-item ITEM_QNO_EU_HWAN_1_4_01 1
goal deliver NPC_EU_HUNTER ITEM_QNO_EU_HWAN_1_4_01 1 talk @SN_TALK_QNO_EU_HWAN_1_4_05 text @SN_CON_QNO_EU_HWAN_1_4
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_HUNTER greeting @SN_NPC_EU_HUNTER_QS
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_HWAN_1_4_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_HWAN_1_4_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_HWAN_1_4_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_HWAN_1_4_07
talk NPC_EU_HUNTER not-done @SN_TALK_QNO_EU_HWAN_1_4_07
talk NPC_EU_WITCH bag-full @SN_TALK_QNO_EU_HWAN_1_4_04
talk NPC_EU_WITCH middle @SN_TALK_QNO_EU_HWAN_1_4_06

# QNO_EU_HWAN_1_4_1.
# A trap set on the ground (100 traps) catches a Niya General's spirit (the roll 30): a Normal Spirit (the goal) or a
# Flutter Spirit (a helper, paid extra by its talk).
quest QNO_EU_HWAN_1_4_1
start level 95-
start after all QNO_EU_HWAN_1_4
repeat 1
giver NPC_EU_HUNTER
npc NPC_EU_HUNTER NPC_EU_WITCH
finish NPC_EU_WITCH
start-item ITEM_QNO_CH_HWAN_1_4_04 100
goal drop 1 ITEM_QNO_CH_HWAN_1_4_01 at NPC_EU_WITCH text @SN_CON_QNO_EU_HWAN_1_4_1
goal drop 1 ITEM_QNO_CH_HWAN_1_4_02 from MOB_TK_BONEGENERAL 0% at NPC_EU_WITCH text @SN_CON_QNO_EU_HWAN_1_4_1 hidden
talk NPC_EU_HUNTER greeting @SN_NPC_EU_HUNTER_QS
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_HUNTER offer @SN_TALK_QNO_EU_HWAN_1_4_1_01
talk NPC_EU_HUNTER accepted @SN_TALK_QNO_EU_HWAN_1_4_1_02
talk NPC_EU_HUNTER denied @SN_TALK_QNO_EU_HWAN_1_4_1_10
talk NPC_EU_HUNTER not-done @SN_TALK_QNO_EU_HWAN_1_4_1_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_HWAN_1_4_1_03
talk NPC_EU_WITCH done @SN_TALK_QNO_EU_HWAN_1_4_1_06
talk NPC_EU_HUNTER bag-full @SN_TALK_QNO_EU_EASTEU_14_1_06
talk NPC_EU_HUNTER middle @SN_TALK_QNO_EU_HWAN_1_4_1_07

# QNO_EU_HWAN_1_5.
# The exorcist's charm (mission kind 4); it sends the player into INS_QNO_EU_HWAN_1_5 to beat MOB_QT_CH_CHARGHOST_01 (a
# champion). The ritual as China's.
quest QNO_EU_HWAN_1_5
start level 95-
start after all QNO_EU_HWAN_1_4_1
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH
finish NPC_EU_WITCH
start-item ITEM_QNO_EU_HWAN_1_5_01 1
goal kill 1 MOB_QT_CH_CHARGHOST_01 class champion berserk text @SN_CON_QNO_EU_HWAN_1_5
trial INS_QNO_EU_HWAN_1_5 at NPC_EU_WITCH row @SN_TALK_QNO_EU_HWAN_1_5_04 start 27728 1779 724 1596 refuse @SN_TALK_QNO_CH_HWAN_1_1_10 after 12 boss MOB_QT_CH_CHARGHOST_01 27728 1752 693 1673 radius 0 0 minutes 10 warn 5 @SN_TALK_QNO_EU_HWAN_1_5_10 berserk take ITEM_QNO_EU_HWAN_1_5_01 give ITEM_QNO_CH_HWAN_1_5_02 failed @SN_TALK_QNO_EU_HWAN_1_5_09 out 10
need item ITEM_QNO_CH_HWAN_1_5_02 1
talk NPC_EU_WITCH trial @SN_TALK_QNO_EU_HWAN_1_5_03
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_HWAN_1_5_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_HWAN_1_5_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_HWAN_1_5_05
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_HWAN_1_5_04
talk NPC_EU_WITCH done @SN_TALK_QNO_EU_HWAN_1_5_06
talk NPC_EU_WITCH bag-full @SN_TALK_QNO_EU_HWAN_1_5_08
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_WITCH done-now @SN_TALK_QNO_EU_HWAN_1_5_07

# QNO_EU_HWAN_1_6.
# The Piece of Spirit handed over at the start and taken to the general (the delivery's conversation: _06, _08 .. _07,
# _09; ours its first line). _RefQuestReward's Hwan 1: the reward raises the player's hwan level (ours
# QuestHost::Reward::hwan).
quest QNO_EU_HWAN_1_6
start level 95-
start after all QNO_EU_HWAN_1_5
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH NPC_EU_GENERAL
finish NPC_EU_GENERAL
start-item ITEM_QNO_CH_HWAN_1_5_02 1
goal deliver NPC_EU_GENERAL ITEM_QNO_CH_HWAN_1_5_02 1 talk @SN_TALK_QNO_EU_HWAN_1_6_06 text @SN_CON_QNO_EU_HWAN_1_6
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_GENERAL greeting @SN_NPC_EU_GENERAL_QS
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_HWAN_1_6_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_HWAN_1_6_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_HWAN_1_6_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_HWAN_1_6_04
talk NPC_EU_GENERAL not-done @SN_TALK_QNO_EU_HWAN_1_6_04
talk NPC_EU_WITCH bag-full @SN_TALK_QNO_EU_HWAN_1_6_05
talk NPC_EU_WITCH middle @SN_TALK_QNO_EU_HWAN_1_6_10

# QNO_EU_HWAN_2_1.
# The second army test: 50 Terminate Traps (mission kind 8, a helper) and the instance INS_QNO_EU_HWAN_2_1, where the
# trapped MOB_QT_EU_SOLDIER_02_1 (a champion) calls its doctor. The trial as China's.
quest QNO_EU_HWAN_2_1
start level 100-
start after all QNO_EU_HWAN_1_6
repeat 1
giver NPC_EU_GENERAL
npc NPC_EU_GENERAL
finish NPC_EU_GENERAL
start-item ITEM_QNO_EU_WHAN_2_1_01 50
goal kill 1 MOB_QT_EU_SOLDIER_02_1 class champion text @SN_CON_QNO_EU_HWAN_2_1
trial INS_QNO_EU_HWAN_2_1 at NPC_EU_GENERAL row @SN_TALK_QNO_EU_HWAN_2_1_05 start 22454 1195 91 653 refuse @SN_TALK_QNO_CH_HWAN_1_1_10 no-job after 10 boss MOB_QT_EU_SOLDIER_02_1 22454 1102 91 704 radius 90 100 minutes 10 warn 5 @SN_TALK_QNO_EU_HWAN_2_1_14 adds MOB_QT_EU_SOLDIER_02_2 22454 1149 91 653 radius 90 100 every 600 pass-on-time passed @SN_TALK_QNO_EU_HWAN_2_1_12 lost @SN_TALK_QNO_EU_HWAN_2_1_11
talk NPC_EU_GENERAL trial @SN_TALK_QNO_EU_HWAN_2_1_04
goal drop 1 ITEM_QNO_EU_WHAN_2_1_01 at NPC_CH_GENARAL text @SN_CON_QNO_CH_HWAN_2_1 hidden
talk NPC_EU_GENERAL offer @SN_TALK_QNO_EU_HWAN_2_1_01
talk NPC_EU_GENERAL accepted @SN_TALK_QNO_EU_HWAN_2_1_03
talk NPC_EU_GENERAL denied @SN_TALK_QNO_EU_HWAN_2_1_02
talk NPC_EU_GENERAL not-done @SN_TALK_QNO_EU_HWAN_2_1_04
talk NPC_EU_GENERAL done @SN_TALK_QNO_EU_HWAN_2_1_10
talk NPC_EU_GENERAL bag-full @SN_TALK_QNO_EU_HWAN_2_1_06
talk NPC_EU_GENERAL greeting @SN_NPC_EU_GENERAL_QS
talk NPC_EU_GENERAL done-now @SN_TALK_QNO_EU_HWAN_2_1_12

# QNO_EU_HWAN_2_2.
# A talk with the next NPC, done there.
quest QNO_EU_HWAN_2_2
start level 100-
start after all QNO_EU_HWAN_2_1
repeat 1
giver NPC_EU_GENERAL
npc NPC_EU_GENERAL NPC_EU_WITCH
finish NPC_EU_WITCH
goal talk NPC_EU_WITCH talk @SN_TALK_QNO_EU_HWAN_2_2_05 text @SN_CON_QNO_EU_HWAN_2_2
talk NPC_EU_GENERAL greeting @SN_NPC_EU_GENERAL_QS
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_GENERAL offer @SN_TALK_QNO_EU_HWAN_2_2_01
talk NPC_EU_GENERAL accepted @SN_TALK_QNO_EU_HWAN_2_2_02
talk NPC_EU_GENERAL denied @SN_TALK_QNO_EU_HWAN_2_2_03
talk NPC_EU_GENERAL not-done @SN_TALK_QNO_EU_HWAN_2_2_04
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_HWAN_2_2_04
talk NPC_EU_GENERAL middle @SN_TALK_QNO_EU_HWAN_2_2_06

# QNO_EU_HWAN_2_3.
# Energy of Mountain from the goats (a helper, 100 %); used on a selected MOB_CH_YEOHA (within 300) it
# becomes the Branch of Life (mission kind 10), the goal.
quest QNO_EU_HWAN_2_3
start level 100-
start after all QNO_EU_HWAN_2_2
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH
finish NPC_EU_WITCH
goal drop 1 ITEM_QNO_CH_WHAN_2_3_02 text @SN_CON_QNO_EU_HWAN_2_3
goal drop 100 ITEM_QNO_CH_WHAN_2_3_01 from MOB_RM_GOAT_CLON 100% MOB_RM_GOAT 100% at NPC_EU_WITCH text @SN_CON_QNO_EU_HWAN_2_3 hidden
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_HWAN_2_3_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_HWAN_2_3_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_HWAN_2_3_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_HWAN_2_3_04
talk NPC_EU_WITCH done @SN_TALK_QNO_EU_HWAN_2_3_05
talk NPC_EU_WITCH bag-full @SN_TALK_QNO_TQ_BAEKRYEONG_4_09
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_WITCH done-now @SN_TALK_QNO_EU_HWAN_2_3_08
talk NPC_EU_WITCH middle @SN_TALK_QNO_EU_HWAN_2_3_06

# QNO_EU_HWAN_2_4.
# Shining Crystals from the ishades (a helper, 100 %); used at the place (a search, 11 in 101) one
# becomes the Relic of Sun God (mission kind 10), the goal: game::OriginalUseRules.
quest QNO_EU_HWAN_2_4
start level 100-
start after all QNO_EU_HWAN_2_3
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH
finish NPC_EU_WITCH
goal drop 1 ITEM_QNO_CH_WHAN_2_4_02 text @SN_CON_QNO_EU_HWAN_2_4
goal drop 100 ITEM_QNO_CH_WHAN_2_4_01 from MOB_KK_ISHADE 100% MOB_KK_ISHADE_CLON 100% at NPC_EU_WITCH text @SN_CON_QNO_EU_HWAN_2_4 hidden
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_HWAN_2_4_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_HWAN_2_4_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_HWAN_2_4_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_HWAN_2_4_04
talk NPC_EU_WITCH done @SN_TALK_QNO_EU_HWAN_2_4_05
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_WITCH done-now @SN_TALK_QNO_EU_HWAN_2_4_08
talk NPC_EU_WITCH middle @SN_TALK_QNO_EU_HWAN_2_4_06

# QNO_EU_HWAN_2_5.
# The charm (mission kind 4) and the instance INS_QNO_EU_HWAN_2_5 where MOB_QT_CH_CHARGHOST_02 is beaten. The ritual as
# 1_5's.
quest QNO_EU_HWAN_2_5
start level 100-
start after all QNO_EU_HWAN_2_4
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH
finish NPC_EU_WITCH
start-item ITEM_QNO_EU_HWAN_2_5_01 1
goal kill 1 MOB_QT_CH_CHARGHOST_02 berserk text @SN_CON_QNO_EU_HWAN_2_5
trial INS_QNO_EU_HWAN_2_5 at NPC_EU_WITCH row @SN_TALK_QNO_EU_HWAN_2_5_04 start 27728 1779 724 1596 refuse @SN_TALK_QNO_CH_HWAN_1_1_10 after 12 boss MOB_QT_CH_CHARGHOST_02 27728 1752 693 1673 radius 0 0 minutes 10 warn 5 @SN_TALK_QNO_EU_HWAN_2_5_10 berserk take ITEM_QNO_EU_HWAN_2_5_01 give ITEM_QNO_CH_HWAN_2_5_02 failed @SN_TALK_QNO_EU_HWAN_2_5_09 out 10
need item ITEM_QNO_CH_HWAN_2_5_02 1
talk NPC_EU_WITCH trial @SN_TALK_QNO_EU_HWAN_2_5_03
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_HWAN_2_5_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_HWAN_2_5_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_HWAN_2_5_05
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_HWAN_2_5_03
talk NPC_EU_WITCH done @SN_TALK_QNO_EU_HWAN_2_5_07
talk NPC_EU_WITCH bag-full @SN_TALK_QNO_EU_HWAN_2_5_06
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_WITCH done-now @SN_TALK_QNO_EU_HWAN_2_5_08

# QNO_EU_HWAN_2_6.
# The piece handed over at the start and taken to the general. _RefQuestReward's Hwan 2 (as 1_6).
quest QNO_EU_HWAN_2_6
start level 100-
start after all QNO_EU_HWAN_2_5
repeat 1
giver NPC_EU_WITCH
npc NPC_EU_WITCH NPC_EU_GENERAL
finish NPC_EU_GENERAL
start-item ITEM_QNO_CH_HWAN_2_5_02 1
goal deliver NPC_EU_GENERAL ITEM_QNO_CH_HWAN_2_5_02 1 talk @SN_TALK_QNO_EU_HWAN_2_6_06 text @SN_CON_QNO_EU_HWAN_2_6
talk NPC_EU_WITCH greeting @SN_NPC_EU_WITCH_QS
talk NPC_EU_GENERAL greeting @SN_NPC_EU_GENERAL_QS
talk NPC_EU_WITCH offer @SN_TALK_QNO_EU_HWAN_2_6_01
talk NPC_EU_WITCH accepted @SN_TALK_QNO_EU_HWAN_2_6_02
talk NPC_EU_WITCH denied @SN_TALK_QNO_EU_HWAN_2_6_03
talk NPC_EU_WITCH not-done @SN_TALK_QNO_EU_HWAN_2_6_04
talk NPC_EU_GENERAL not-done @SN_TALK_QNO_EU_HWAN_2_6_04
talk NPC_EU_WITCH bag-full @SN_TALK_QNO_EU_HWAN_2_6_05
talk NPC_EU_WITCH middle @SN_TALK_QNO_EU_HWAN_2_6_10
)";

} // namespace

std::string_view CodedQuestsEurope() { return kCoded; }

} // namespace sr::server
