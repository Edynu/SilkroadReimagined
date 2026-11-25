// The original's coded quests of Samarkand (QNO_CA_*), Donwhang's warehouse (QNO_WC_WAREHOUSE_W_*), the Tomb of Qin Shi (QNO_TQ_*) and Roc
// Mountain (QNO_RM_*): definitions in our format, with the same conventions as OriginalQuests.cpp. Read by
// DecodeCodedQuests.
//
// The families whose items are used from the bag and every coded quest of their chains: 27 quests; the chains' 14
// others are the quest script's (Quest.sct, imported as data). What they hold besides OriginalQuests.cpp's:
// - the talk (offer, accepted, denied, not-done, done, a second not-done, bag-full, the content, greeting, done-now,
//   abort by level, count-limit, time-out, middle (told when the start's items are given), offer-again, not-party),
//   and a greeting for each NPC of the start's list;
// - the missions by kind: 1 a gathering (a helper that never holds the quest back: our `hidden`), 2 a kill, 3 a
//   delivery (its items, what the NPC hands back: our `get`, the NPC's words before and after, its bag-full), 4 items
//   from the giver, 6 a talk, 8 a capture (the traps, a caught item), 10 an item changed into another (our gathering
//   without `from`), 11 a time;
// - the start: a delivery's, a kind 4's and a capture's items are given (our `start-item`), no room: bag-full;
// - a mission that does not count never holds the quest back.
// Their items' own handlers (the searches, the captures, the spawn, the instances, the stages, the coin) are the item
// rules of game/QuestItems.cpp (OriginalUseRules), added to these definitions when the game server loads them.

#include <string_view>

#include "sr/server/QuestScript.h"

namespace sr::server {

namespace {

constexpr std::string_view kCoded = R"(
# --- Samarkand: Hoyun's mistake (QNO_CA_HORSE_2, the script's, then HORSE_3 and WAREHOUSE_1) -----------------------------

# Hoyun's leather wrapped by Saesa: a delivery whose 30 Soft Leathers the start gives (_05 no room, _07 the middle told
# then); paid at the delivery (no finisher's talk). No reward
# (_RefQuestReward IsBasicReward 0).
quest QNO_CA_HORSE_3 # data only
start level 19-
start after all QNO_CA_HORSE_2
repeat 1
giver NPC_CA_HORSE
npc NPC_CA_HORSE NPC_CA_WAREHOUSE
start-item ITEM_QNO_CA_HORSE_2_01 30
goal deliver NPC_CA_WAREHOUSE ITEM_QNO_CA_HORSE_2_01 30 talk @SN_TALK_QNO_CA_HORSE_3_06 text @SN_CON_QNO_CA_HORSE_3
talk NPC_CA_HORSE greeting @SN_NPC_CA_HORSE_QS
talk NPC_CA_HORSE offer @SN_TALK_QNO_CA_HORSE_3_01
talk NPC_CA_HORSE accepted @SN_TALK_QNO_CA_HORSE_3_02
talk NPC_CA_HORSE denied @SN_TALK_QNO_CA_HORSE_3_03
talk NPC_CA_HORSE not-done @SN_TALK_QNO_CA_HORSE_3_04
talk NPC_CA_HORSE bag-full @SN_TALK_QNO_CA_HORSE_3_05
talk NPC_CA_HORSE middle @SN_TALK_QNO_CA_HORSE_3_07
talk NPC_CA_WAREHOUSE greeting @SN_NPC_CA_WAREHOUSE_QS

# Saesa's Blue Thorn Grass: the earwigs (a helper gathering: 5 from the Evil Order magicians, used up by the search)
# used near the amphitheatre (the item rule, with its countdown) give the grass, 10 of it the goal (kind 10).
quest QNO_CA_WAREHOUSE_1
start level 19-
start after all QNO_CA_HORSE_3
repeat 1
giver NPC_CA_WAREHOUSE
npc NPC_CA_WAREHOUSE
finish NPC_CA_WAREHOUSE
goal drop 10 ITEM_QNO_CA_WAREHOUSE_1_01 at NPC_CA_WAREHOUSE text @SN_CON_QNO_CA_WAREHOUSE_1
goal drop 5 ITEM_QNO_CA_WAREHOUSE_1_02 from MOB_QT_01_ARCHER_CLON 100% at NPC_CA_HORSE text @SN_CON_QNO_CA_WAREHOUSE_1 hidden
talk NPC_CA_WAREHOUSE greeting @SN_NPC_CA_WAREHOUSE_QS
talk NPC_CA_WAREHOUSE offer @SN_TALK_QNO_CA_WAREHOUSE_1_01
talk NPC_CA_WAREHOUSE accepted @SN_TALK_QNO_CA_WAREHOUSE_1_02
talk NPC_CA_WAREHOUSE denied @SN_TALK_QNO_CA_WAREHOUSE_1_03
talk NPC_CA_WAREHOUSE not-done @SN_TALK_QNO_CA_WAREHOUSE_1_04
talk NPC_CA_WAREHOUSE bag-full @SN_TALK_QNO_CA_WAREHOUSE_1_05
talk NPC_CA_WAREHOUSE done @SN_TALK_QNO_CA_WAREHOUSE_1_06
talk NPC_CA_WAREHOUSE done-now @SN_TALK_QNO_CA_WAREHOUSE_1_07

# --- Samarkand: Saesa's revenge (the first of its chain) ----------------------------------------------------------------

# Hun bows (a helper gathering, 30 % from the Hun archers) used at the Ong habitat (the item rule) become dirty bows, 20
# the goal. Three times: taken again its talk says _02 and _04.
quest QNO_CA_WAREHOUSE_3
start level 25-
repeat 3
giver NPC_CA_WAREHOUSE
npc NPC_CA_WAREHOUSE
finish NPC_CA_WAREHOUSE
goal drop 20 ITEM_QNO_CA_WAREHOUSE_3_02 at NPC_CA_WAREHOUSE text @SN_CON_QNO_CA_WAREHOUSE_3
goal drop 20 ITEM_QNO_CA_WAREHOUSE_3_01 from MOB_CA_HUNARCHER_CLON 30% at NPC_CA_WAREHOUSE text @SN_CON_QNO_CA_WAREHOUSE_3 hidden
talk NPC_CA_WAREHOUSE greeting @SN_NPC_CA_WAREHOUSE_QS
talk NPC_CA_WAREHOUSE offer @SN_TALK_QNO_CA_WAREHOUSE_3_01
talk NPC_CA_WAREHOUSE offer-again @SN_TALK_QNO_CA_WAREHOUSE_3_02
talk NPC_CA_WAREHOUSE accepted @SN_TALK_QNO_CA_WAREHOUSE_3_03
talk NPC_CA_WAREHOUSE accepted-again @SN_TALK_QNO_CA_WAREHOUSE_3_04
talk NPC_CA_WAREHOUSE denied @SN_TALK_QNO_CA_WAREHOUSE_3_05
talk NPC_CA_WAREHOUSE not-done @SN_TALK_QNO_CA_WAREHOUSE_3_06
talk NPC_CA_WAREHOUSE done @SN_TALK_QNO_CA_WAREHOUSE_3_07
talk NPC_CA_WAREHOUSE done-now @SN_TALK_QNO_CA_WAREHOUSE_3_09

# --- Samarkand: the hidden treasure (TREASURE_1..3 the script's) --------------------------------------------------------

# The Seal Key's materials for Tricia: a Hungry Blood Ong's tear (100 % from MOB_QT_01_ONG_CLON) and an Ong's tail (10 %
# from MOB_CA_ONG; two gatherings), and Samarkand's water the potion merchant
# hands over (a delivery of nothing, its second list one water).
quest QNO_CA_TREASURE_4 # data only
start level 23-
start after all QNO_CA_TREASURE_3
repeat 1
giver NPC_CA_SMITH
npc NPC_CA_SMITH NPC_CA_POTION
finish NPC_CA_SMITH
goal drop 1 ITEM_QNO_CA_TREASURE_4_01 from MOB_QT_01_ONG_CLON 100% at NPC_CA_SMITH text @SN_CON_QNO_CA_TREASURE_4_01
goal drop 1 ITEM_QNO_CA_TREASURE_4_02 from MOB_CA_ONG 10% at NPC_CA_SMITH text @SN_CON_QNO_CA_TREASURE_4_02
goal deliver NPC_CA_POTION get ITEM_QNO_CA_TREASURE_4_03 1 talk @SN_TALK_QNO_CA_TREASURE_4_07 text @SN_CON_QNO_CA_TREASURE_4_03
talk NPC_CA_SMITH greeting @SN_NPC_CA_SMITH_QS
talk NPC_CA_SMITH offer @SN_TALK_QNO_CA_TREASURE_4_01
talk NPC_CA_SMITH accepted @SN_TALK_QNO_CA_TREASURE_4_02
talk NPC_CA_SMITH denied @SN_TALK_QNO_CA_TREASURE_4_03
talk NPC_CA_SMITH not-done @SN_TALK_QNO_CA_TREASURE_4_04
talk NPC_CA_SMITH done @SN_TALK_QNO_CA_TREASURE_4_05
talk NPC_CA_SMITH done-now @SN_TALK_QNO_CA_TREASURE_4_06
talk NPC_CA_POTION greeting @SN_NPC_CA_POTION_QS
talk NPC_CA_POTION middle @SN_TALK_QNO_CA_TREASURE_4_09
talk NPC_CA_POTION not-done @SN_TALK_QNO_CA_TREASURE_4_09
talk NPC_CA_POTION bag-full @SN_TALK_QNO_CA_TREASURE_4_08

# The treasure: 5 Seal Keys from Tricia (kind 4); a key used at the Ong habitat raises the treasure guardian (the item
# rule: MOB_QT_01_ONG 20..100 away, gone after its time), whose death drops the King's Treasure Box (100 %). Its talk:
# none held, five keys again (_05 the words, _06 the row, _07 after, _08 no room); the box opened in pages (_09, _10 the
# row, _11, _12 Next, _13, _15 Receive present), then the pay and every key taken (ours: the keys left stay). The
# original's bag-full is _05 (its words for the keys); ours _08, said for the start's room and the keys' alike.
quest QNO_CA_TREASURE_5
start level 23-
start after all QNO_CA_TREASURE_4
repeat 1
giver NPC_CA_SMITH
npc NPC_CA_SMITH
finish NPC_CA_SMITH
start-item ITEM_QNO_CA_TREASURE_5_01 5
resupply ITEM_QNO_CA_TREASURE_5_01 5
pay-take ITEM_QNO_CA_TREASURE_5_01
goal drop 1 ITEM_QNO_CA_TREASURE_5_02 from MOB_QT_01_ONG 100% at NPC_CA_SMITH text @SN_CON_QNO_CA_TREASURE_5
talk NPC_CA_SMITH greeting @SN_NPC_CA_SMITH_QS
talk NPC_CA_SMITH offer @SN_TALK_QNO_CA_TREASURE_5_01
talk NPC_CA_SMITH accepted @SN_TALK_QNO_CA_TREASURE_5_02
talk NPC_CA_SMITH denied @SN_TALK_QNO_CA_TREASURE_5_03
talk NPC_CA_SMITH not-done @SN_TALK_QNO_CA_TREASURE_5_04
talk NPC_CA_SMITH bag-full @SN_TALK_QNO_CA_TREASURE_5_08
talk NPC_CA_SMITH resupply @SN_TALK_QNO_CA_TREASURE_5_05 row @SN_TALK_QNO_CA_TREASURE_5_06
talk NPC_CA_SMITH resupply @SN_TALK_QNO_CA_TREASURE_5_07
talk NPC_CA_SMITH done @SN_TALK_QNO_CA_TREASURE_5_09 row @SN_TALK_QNO_CA_TREASURE_5_10
talk NPC_CA_SMITH done @SN_TALK_QNO_CA_TREASURE_5_11 row @SN_TALK_QNO_CA_TREASURE_5_12
talk NPC_CA_SMITH done @SN_TALK_QNO_CA_TREASURE_5_13 row @SN_TALK_QNO_CA_TREASURE_5_15
talk NPC_CA_SMITH finished @SN_TALK_QNO_CA_TREASURE_5_16
talk NPC_CA_SMITH done-now @SN_TALK_QNO_CA_TREASURE_5_14

# --- Samarkand: defeating Goria (GORIA_1, _4, _5 the script's) ---------------------------------------------------------

# Shahad's test: 70 Spirit Lost Stones given (kind 4, not counted); each golem kill its kill mission counts (100, a
# helper) changes one into a Spirit Added Stone (in state 7 or 8, a free bag place
# else _08, under 100 Added held: one Added given, one Lost taken - the Added given even with no Lost left, the
# original's order kept); 70 Added the goal (kind 10).
quest QNO_CA_GORIA_2
start level 29-
start after all QNO_CA_GORIA_1
repeat 1
giver NPC_CA_HUNTER
npc NPC_CA_HUNTER
finish NPC_CA_HUNTER
start-item ITEM_QNO_CA_GORIA_2_01 70
on kill MOB_CA_GOLEM_CLON take ITEM_QNO_CA_GORIA_2_01 give ITEM_QNO_CA_GORIA_2_02 cap 100 full @SN_TALK_QNO_CA_GORIA_2_08
goal drop 70 ITEM_QNO_CA_GORIA_2_02 at NPC_CA_HUNTER text @SN_CON_QNO_CA_GORIA_2
goal kill 100 MOB_CA_GOLEM_CLON text @SN_CON_QNO_CA_GORIA_2 hidden
talk NPC_CA_HUNTER greeting @SN_NPC_CA_HUNTER_QS
talk NPC_CA_HUNTER offer @SN_TALK_QNO_CA_GORIA_2_01
talk NPC_CA_HUNTER accepted @SN_TALK_QNO_CA_GORIA_2_02
talk NPC_CA_HUNTER denied @SN_TALK_QNO_CA_GORIA_2_03
talk NPC_CA_HUNTER not-done @SN_TALK_QNO_CA_GORIA_2_04
talk NPC_CA_HUNTER bag-full @SN_TALK_QNO_CA_GORIA_2_05
talk NPC_CA_HUNTER done @SN_TALK_QNO_CA_GORIA_2_06
talk NPC_CA_HUNTER done-now @SN_TALK_QNO_CA_GORIA_2_07

# Goria's information from Jooha at the east gate (a talk, kind 6), told to Shahad.
quest QNO_CA_GORIA_3
start level 29-
start after all QNO_CA_GORIA_2
repeat 1
giver NPC_CA_HUNTER
npc NPC_CA_HUNTER NPC_CA_SOLDIER_EA1
finish NPC_CA_HUNTER
goal talk NPC_CA_SOLDIER_EA1 talk @SN_TALK_QNO_CA_GORIA_3_05 text @SN_CON_QNO_CA_GORIA_3
talk NPC_CA_HUNTER greeting @SN_NPC_CA_HUNTER_QS
talk NPC_CA_HUNTER offer @SN_TALK_QNO_CA_GORIA_3_01
talk NPC_CA_HUNTER accepted @SN_TALK_QNO_CA_GORIA_3_02
talk NPC_CA_HUNTER denied @SN_TALK_QNO_CA_GORIA_3_03
talk NPC_CA_HUNTER not-done @SN_TALK_QNO_CA_GORIA_3_04
talk NPC_CA_HUNTER done @SN_TALK_QNO_CA_GORIA_3_07
talk NPC_CA_HUNTER middle @SN_TALK_QNO_CA_GORIA_3_08
talk NPC_CA_HUNTER done-now @SN_TALK_QNO_CA_GORIA_3_09
talk NPC_CA_SOLDIER_EA1 greeting @SN_NPC_CA_SOLDIER_EA1_QS
talk NPC_CA_SOLDIER_EA1 middle @SN_TALK_QNO_CA_GORIA_3_06
talk NPC_CA_SOLDIER_EA1 not-done @SN_TALK_QNO_CA_GORIA_3_06

# A Hun magic archer caught (kind 8): 5 traps given (ITEM_QNO_CA_GORIA_6_01, 3/3/13/1: a skill's item, used it casts its
# skill); the trap's catch (MOB_QT_01_HUNARCHER_CLON, no room _13, one already _18, failed _20) gives a Captured Hun Tribe
# (ITEM_QNO_CA_GORIA_6_02), taken back when the trap's time runs out (_16, _17, then _15) or its holder dies (_14). Its
# talk: no trap left, five again (_05, _06 the row, _07 after; the day's supply spent _08: once a game day, the start the
# first day: `resupply-daily start`). Its greeting is the European hunter's.
quest QNO_CA_GORIA_6
start level 29-
start after all QNO_CA_GORIA_5
repeat 1
giver NPC_CA_HUNTER
npc NPC_CA_HUNTER
finish NPC_CA_HUNTER
start-item ITEM_QNO_CA_GORIA_6_01 5
resupply ITEM_QNO_CA_GORIA_6_01 5
resupply-daily start
pay-take ITEM_QNO_CA_GORIA_6_01
pay-take ITEM_QNO_CA_GORIA_6_02
goal drop 1 ITEM_QNO_CA_GORIA_6_02 at NPC_CA_HUNTER text @SN_CON_QNO_CA_GORIA_6
talk NPC_CA_HUNTER greeting @SN_NPC_EU_HUNTER_QS
talk NPC_CA_HUNTER offer @SN_TALK_QNO_CA_GORIA_6_01
talk NPC_CA_HUNTER accepted @SN_TALK_QNO_CA_GORIA_6_02
talk NPC_CA_HUNTER denied @SN_TALK_QNO_CA_GORIA_6_03
talk NPC_CA_HUNTER not-done @SN_TALK_QNO_CA_GORIA_6_04
talk NPC_CA_HUNTER bag-full @SN_TALK_QNO_CA_GORIA_6_09
talk NPC_CA_HUNTER resupply @SN_TALK_QNO_CA_GORIA_6_05 row @SN_TALK_QNO_CA_GORIA_6_06
talk NPC_CA_HUNTER resupply @SN_TALK_QNO_CA_GORIA_6_07
talk NPC_CA_HUNTER resupply-spent @SN_TALK_QNO_CA_GORIA_6_08
talk NPC_CA_HUNTER done @SN_TALK_QNO_CA_GORIA_6_11
talk NPC_CA_HUNTER done-now @SN_TALK_QNO_CA_GORIA_6_12

# Goria hunted in the quest's instance (INS_QNO_CA_GORIA_7, WorldID 13): a party sharing experience and items (its talk
# says _24 otherwise); the Big Drum (kind 4, one) used at the Huns' garrison by the party's leader takes the party in (the
# item rule); Goria (MOB_QT_01_HUNLANCER) killed is the goal (kind 2, one; the instance then sends them out after 60 s);
# the kill clears the drums. No drum left: another (_05, _06; no room _08). The 1.188 server names no Cursed Stone nor
# Goria's Spear
# (its _10, _18.._22 unused: no done-now set).
quest QNO_CA_GORIA_7
start level 29-
start after all QNO_CA_GORIA_6
start party
repeat 1
giver NPC_CA_HUNTER
npc NPC_CA_HUNTER
finish NPC_CA_HUNTER
start-item ITEM_QNO_CA_GORIA_7_03 1
resupply ITEM_QNO_CA_GORIA_7_03 1
on kill MOB_QT_01_HUNLANCER clear ITEM_QNO_CA_GORIA_7_03
goal kill 1 MOB_QT_01_HUNLANCER text @SN_CON_QNO_CA_GORIA_7
talk NPC_CA_HUNTER greeting @SN_NPC_CA_HUNTER_QS
talk NPC_CA_HUNTER offer @SN_TALK_QNO_CA_GORIA_7_01
talk NPC_CA_HUNTER accepted @SN_TALK_QNO_CA_GORIA_7_02
talk NPC_CA_HUNTER denied @SN_TALK_QNO_CA_GORIA_7_03
talk NPC_CA_HUNTER not-done @SN_TALK_QNO_CA_GORIA_7_04
talk NPC_CA_HUNTER bag-full @SN_TALK_QNO_CA_GORIA_7_08
talk NPC_CA_HUNTER resupply @SN_TALK_QNO_CA_GORIA_7_05
talk NPC_CA_HUNTER done @SN_TALK_QNO_CA_GORIA_7_09
talk NPC_CA_HUNTER not-party @SN_TALK_QNO_CA_GORIA_7_24

# --- Donwhang: Irina's charms (QNO_WC_WAREHOUSE_W_1 the script's) -------------------------------------------------------

# The Earth Ghost witch doctors' charms (a helper gathering of 10,000, 50 % from the earth kings) tested from the bag (the
# item rule, the coin) give rarity charms or bad ones; 70 rarity charms the goal. Twice: taken again its talk says _07
# and _08.
quest QNO_WC_WAREHOUSE_W_2
start level 18-
start after all QNO_WC_WAREHOUSE_W_1
repeat 2
giver NPC_WC_WAREHOUSE_W
npc NPC_WC_WAREHOUSE_W
finish NPC_WC_WAREHOUSE_W
goal drop 70 ITEM_QNO_WC_WAREHOUSE_W_2_02 at NPC_WC_WAREHOUSE_W text @SN_CON_QNO_WC_WAREHOUSE_W_2_01
goal drop 10000 ITEM_QNO_WC_WAREHOUSE_W_2_01 from MOB_WC_EARTHKING 50% MOB_WC_EARTHKING_CLON 50% at NPC_WC_WAREHOUSE_W text @SN_CON_QNO_WC_WAREHOUSE_W_2_01 hidden
talk NPC_WC_WAREHOUSE_W greeting @SN_NPC_WC_WAREHOUSE_W_QS
talk NPC_WC_WAREHOUSE_W offer @SN_TALK_QNO_WC_WAREHOUSE_W_2_01
talk NPC_WC_WAREHOUSE_W offer-again @SN_TALK_QNO_WC_WAREHOUSE_W_2_07
talk NPC_WC_WAREHOUSE_W accepted @SN_TALK_QNO_WC_WAREHOUSE_W_2_02
talk NPC_WC_WAREHOUSE_W accepted-again @SN_TALK_QNO_WC_WAREHOUSE_W_2_08
talk NPC_WC_WAREHOUSE_W denied @SN_TALK_QNO_WC_WAREHOUSE_W_2_03
talk NPC_WC_WAREHOUSE_W not-done @SN_TALK_QNO_WC_WAREHOUSE_W_2_04
talk NPC_WC_WAREHOUSE_W bag-full @SN_TALK_QNO_WC_WAREHOUSE_W_2_05
talk NPC_WC_WAREHOUSE_W done @SN_TALK_QNO_WC_WAREHOUSE_W_2_06
talk NPC_WC_WAREHOUSE_W done-now @SN_TALK_QNO_WC_WAREHOUSE_W_2_10

# The same, 130 rarity charms (the coin); the Truth Eye its reward.
quest QNO_WC_WAREHOUSE_W_3
start level 18-
start after all QNO_WC_WAREHOUSE_W_2
repeat 1
giver NPC_WC_WAREHOUSE_W
npc NPC_WC_WAREHOUSE_W
finish NPC_WC_WAREHOUSE_W
goal drop 130 ITEM_QNO_WC_WAREHOUSE_W_2_02 at NPC_WC_WAREHOUSE_W text @SN_CON_QNO_WC_WAREHOUSE_W_2_02
goal drop 10000 ITEM_QNO_WC_WAREHOUSE_W_2_01 from MOB_WC_EARTHKING 50% MOB_WC_EARTHKING_CLON 50% at NPC_WC_WAREHOUSE_W text @SN_CON_QNO_WC_WAREHOUSE_W_2_02 hidden
talk NPC_WC_WAREHOUSE_W greeting @SN_NPC_WC_WAREHOUSE_W_QS
talk NPC_WC_WAREHOUSE_W offer @SN_TALK_QNO_WC_WAREHOUSE_W_3_01
talk NPC_WC_WAREHOUSE_W accepted @SN_TALK_QNO_WC_WAREHOUSE_W_3_02
talk NPC_WC_WAREHOUSE_W denied @SN_TALK_QNO_WC_WAREHOUSE_W_3_03
talk NPC_WC_WAREHOUSE_W not-done @SN_TALK_QNO_WC_WAREHOUSE_W_3_04
talk NPC_WC_WAREHOUSE_W bag-full @SN_TALK_QNO_WC_WAREHOUSE_W_3_05
talk NPC_WC_WAREHOUSE_W done @SN_TALK_QNO_WC_WAREHOUSE_W_3_06
talk NPC_WC_WAREHOUSE_W done-now @SN_TALK_QNO_WC_WAREHOUSE_W_3_07

# --- The Tomb of Qin Shi: JinJin's accessory material (ORNAMENT_1 the script's) ------------------------------------------

# The venom refined by Yang Yizhi: given at the start (the delivery's list), handed back refined (the second list).
quest QNO_TQ_ORNAMENT_2 # data only
start level 84-
start after all QNO_TQ_ORNAMENT_1
repeat 1
giver NPC_CH_ACCESSORY
npc NPC_CH_ACCESSORY NPC_CH_POTION
finish NPC_CH_ACCESSORY
start-item ITEM_QNO_TQ_ORNAMENT_2_01 1
goal deliver NPC_CH_POTION ITEM_QNO_TQ_ORNAMENT_2_01 1 get ITEM_QNO_TQ_ORNAMENT_2_02 1 talk @SN_TALK_QNO_TQ_ORNAMENT_2_06 text @SN_CON_QNO_TQ_ORNAMENT_2_01
talk NPC_CH_ACCESSORY greeting @SN_NPC_CH_ACCESSORY_QS
talk NPC_CH_ACCESSORY offer @SN_TALK_QNO_TQ_ORNAMENT_2_01
talk NPC_CH_ACCESSORY accepted @SN_TALK_QNO_TQ_ORNAMENT_2_02
talk NPC_CH_ACCESSORY denied @SN_TALK_QNO_TQ_ORNAMENT_2_03
talk NPC_CH_ACCESSORY not-done @SN_TALK_QNO_TQ_ORNAMENT_2_04
talk NPC_CH_ACCESSORY bag-full @SN_TALK_QNO_TQ_ORNAMENT_2_05
talk NPC_CH_ACCESSORY done @SN_TALK_QNO_TQ_ORNAMENT_2_09
talk NPC_CH_ACCESSORY middle @SN_TALK_QNO_TQ_ORNAMENT_2_10
talk NPC_CH_ACCESSORY done-now @SN_TALK_QNO_TQ_ORNAMENT_2_11
talk NPC_CH_POTION greeting @SN_NPC_CH_POTION_QS
talk NPC_CH_POTION middle @SN_TALK_QNO_TQ_ORNAMENT_2_07
talk NPC_CH_POTION not-done @SN_TALK_QNO_TQ_ORNAMENT_2_07
talk NPC_CH_POTION bag-full @SN_TALK_QNO_TQ_ORNAMENT_2_08

# Bi Young dug with the venom-applied pickaxes (30 given by its talk at the start, again when none is left: _05, _06 the
# row, _07; the day's supply spent _09: `resupply-daily`) at the tomb's four places (the item rule, the search); 10 the
# goal (kind 10). Its greeting is Saesa's.
quest QNO_TQ_ORNAMENT_3
start level 84-
start after all QNO_TQ_ORNAMENT_2
repeat 1
giver NPC_CH_ACCESSORY
npc NPC_CH_ACCESSORY
finish NPC_CH_ACCESSORY
start-item ITEM_QNO_TQ_ORNAMENT_3_01 30
resupply ITEM_QNO_TQ_ORNAMENT_3_01 30
resupply-daily
goal drop 10 ITEM_QNO_TQ_ORNAMENT_3_02 at NPC_CH_ACCESSORY text @SN_CON_QNO_TQ_ORNAMENT_3_01
talk NPC_CH_ACCESSORY greeting @SN_NPC_CA_WAREHOUSE_QS
talk NPC_CH_ACCESSORY offer @SN_TALK_QNO_TQ_ORNAMENT_3_01
talk NPC_CH_ACCESSORY accepted @SN_TALK_QNO_TQ_ORNAMENT_3_02
talk NPC_CH_ACCESSORY denied @SN_TALK_QNO_TQ_ORNAMENT_3_03
talk NPC_CH_ACCESSORY not-done @SN_TALK_QNO_TQ_ORNAMENT_3_04
talk NPC_CH_ACCESSORY bag-full @SN_TALK_QNO_TQ_ORNAMENT_3_08
talk NPC_CH_ACCESSORY resupply @SN_TALK_QNO_TQ_ORNAMENT_3_05 row @SN_TALK_QNO_TQ_ORNAMENT_3_06
talk NPC_CH_ACCESSORY resupply @SN_TALK_QNO_TQ_ORNAMENT_3_07
talk NPC_CH_ACCESSORY resupply-spent @SN_TALK_QNO_TQ_ORNAMENT_3_09
talk NPC_CH_ACCESSORY done @SN_TALK_QNO_TQ_ORNAMENT_3_10
talk NPC_CH_ACCESSORY done-now @SN_TALK_QNO_TQ_ORNAMENT_3_11

# The tomb snake general's blood in the quest's instance (INS_QNO_TQ_ORNAMENT_4): a sharing party; the explosive stone and a
# small calabash given at the start; the stone used in the tomb's central cave by the leader takes the party in (the item
# rule); MOB_QT_TQ_SNAKEGENERAL4 killed (kind 2, one) turns the calabash into the blood one (one or more calabashes held:
# every calabash and stone taken, one blood calabash given), asked at the hand-in (_04 without it).
quest QNO_TQ_ORNAMENT_4
start level 84-
start after all QNO_TQ_ORNAMENT_3
start party
repeat 1
giver NPC_CH_ACCESSORY
npc NPC_CH_ACCESSORY
finish NPC_CH_ACCESSORY
start-item ITEM_QNO_TQ_ORNAMENT_4_04 1
start-item ITEM_QNO_TQ_ORNAMENT_4_01 1
need item ITEM_QNO_TQ_ORNAMENT_4_02 1
on kill MOB_QT_TQ_SNAKEGENERAL4 need ITEM_QNO_TQ_ORNAMENT_4_01 clear ITEM_QNO_TQ_ORNAMENT_4_01 ITEM_QNO_TQ_ORNAMENT_4_04 give ITEM_QNO_TQ_ORNAMENT_4_02
goal kill 1 MOB_QT_TQ_SNAKEGENERAL4 text @SN_CON_QNO_TQ_ORNAMENT_4_01
talk NPC_CH_ACCESSORY greeting @SN_NPC_CH_ACCESSORY_QS
talk NPC_CH_ACCESSORY offer @SN_TALK_QNO_TQ_ORNAMENT_4_01
talk NPC_CH_ACCESSORY accepted @SN_TALK_QNO_TQ_ORNAMENT_4_02
talk NPC_CH_ACCESSORY denied @SN_TALK_QNO_TQ_ORNAMENT_4_03
talk NPC_CH_ACCESSORY not-done @SN_TALK_QNO_TQ_ORNAMENT_4_04
talk NPC_CH_ACCESSORY missing @SN_TALK_QNO_TQ_ORNAMENT_4_04
talk NPC_CH_ACCESSORY bag-full @SN_TALK_QNO_TQ_ORNAMENT_4_05
talk NPC_CH_ACCESSORY done @SN_TALK_QNO_TQ_ORNAMENT_4_18
talk NPC_CH_ACCESSORY middle @SN_TALK_QNO_TQ_ORNAMENT_4_19
talk NPC_CH_ACCESSORY done-now @SN_TALK_QNO_TQ_ORNAMENT_4_20
talk NPC_CH_ACCESSORY not-party @SN_TALK_QNO_TQ_ORNAMENT_4_21

# --- The Tomb of Qin Shi: Hwajung's worry -------------------------------------------------------------------------------

# Miaoryeong's word on the thief: a delivery of nothing, the account book handed back (the second list), brought to Hwajung.
quest QNO_TQ_FINDTHIEF_1 # data only
start level 75-
repeat 1
giver NPC_CH_DOCTOR
npc NPC_CH_DOCTOR NPC_CH_SHAMAN
finish NPC_CH_DOCTOR
goal deliver NPC_CH_SHAMAN get ITEM_QNO_TQ_FINDTHIEF_1_01 1 talk @SN_TALK_QNO_TQ_FINDTHIEF_1_05 text @SN_CON_QNO_TQ_FINDTHIEF_1_01
talk NPC_CH_DOCTOR greeting @SN_NPC_CH_DOCTOR_QS
talk NPC_CH_DOCTOR offer @SN_TALK_QNO_TQ_FINDTHIEF_1_01
talk NPC_CH_DOCTOR accepted @SN_TALK_QNO_TQ_FINDTHIEF_1_02
talk NPC_CH_DOCTOR denied @SN_TALK_QNO_TQ_FINDTHIEF_1_03
talk NPC_CH_DOCTOR not-done @SN_TALK_QNO_TQ_FINDTHIEF_1_04
talk NPC_CH_DOCTOR middle @SN_TALK_QNO_TQ_FINDTHIEF_1_09
talk NPC_CH_DOCTOR done @SN_TALK_QNO_TQ_FINDTHIEF_1_08
talk NPC_CH_DOCTOR done-now @SN_TALK_QNO_TQ_FINDTHIEF_1_10
talk NPC_CH_SHAMAN greeting @SN_NPC_CH_SHAMAN_QS
talk NPC_CH_SHAMAN middle @SN_TALK_QNO_TQ_FINDTHIEF_1_06
talk NPC_CH_SHAMAN not-done @SN_TALK_QNO_TQ_FINDTHIEF_1_06
talk NPC_CH_SHAMAN bag-full @SN_TALK_QNO_TQ_FINDTHIEF_1_07

# The thief caught (kind 8): 5 ropes given; a rope on the selected MOB_QT_TQ_THIEF_01 within 300 catches it as the quest's
# COS (the item rule), brought to Hwajung. No rope left: five again (its talk: _05, _06 the row, _07; the day's supply
# spent _08: `resupply-daily`, its start not counted). Its greeting is the witch's.
quest QNO_TQ_FINDTHIEF_2
start level 75-
start after all QNO_TQ_FINDTHIEF_1
repeat 1
giver NPC_CH_DOCTOR
npc NPC_CH_DOCTOR
finish NPC_CH_DOCTOR
start-item ITEM_QNO_TQ_FINDTHIEF_2_01 5
resupply ITEM_QNO_TQ_FINDTHIEF_2_01 5
resupply-daily
pay-take ITEM_QNO_TQ_FINDTHIEF_2_01
goal capture MOB_QT_TQ_THIEF_01_COS at NPC_CH_DOCTOR text @SN_CON_QNO_TQ_FINDTHIEF_2_01
talk NPC_CH_DOCTOR greeting @SN_NPC_EU_WITCH_QS
talk NPC_CH_DOCTOR offer @SN_TALK_QNO_TQ_FINDTHIEF_2_01
talk NPC_CH_DOCTOR accepted @SN_TALK_QNO_TQ_FINDTHIEF_2_02
talk NPC_CH_DOCTOR denied @SN_TALK_QNO_TQ_FINDTHIEF_2_03
talk NPC_CH_DOCTOR not-done @SN_TALK_QNO_TQ_FINDTHIEF_2_04
talk NPC_CH_DOCTOR bag-full @SN_TALK_QNO_TQ_FINDTHIEF_2_09
talk NPC_CH_DOCTOR resupply @SN_TALK_QNO_TQ_FINDTHIEF_2_05 row @SN_TALK_QNO_TQ_FINDTHIEF_2_06
talk NPC_CH_DOCTOR resupply @SN_TALK_QNO_TQ_FINDTHIEF_2_07
talk NPC_CH_DOCTOR resupply-spent @SN_TALK_QNO_TQ_FINDTHIEF_2_08
talk NPC_CH_DOCTOR done @SN_TALK_QNO_TQ_FINDTHIEF_2_10
talk NPC_CH_DOCTOR done-now @SN_TALK_QNO_TQ_FINDTHIEF_2_11

# --- The Tomb of Qin Shi: the White Viper (BAEKRYOUNG_1 and _3 the script's; the keys spell BAEKRYEONG) -------------------

# Yang Yizhi sends the player to WalYoung (a talk, kind 6: paid at the talk). Its offer's pages: _01, then the
# answer _03 to the player's question _02.
quest QNO_TQ_BAEKRYOUNG_2
start level 88-
start after all QNO_TQ_BAEKRYOUNG_1
repeat 1
giver NPC_CH_POTION
npc NPC_CH_POTION NPC_CH_MOONSHADOW
goal talk NPC_CH_MOONSHADOW talk @SN_TALK_QNO_TQ_BAEKRYEONG_2_07 text @SN_CON_QNO_TQ_BAEKRYOUNG_2_01
talk NPC_CH_POTION greeting @SN_NPC_CH_POTION_QS
talk NPC_CH_POTION offer @SN_TALK_QNO_TQ_BAEKRYEONG_2_01 row @SN_TALK_QNO_TQ_BAEKRYEONG_2_02
talk NPC_CH_POTION offer @SN_TALK_QNO_TQ_BAEKRYEONG_2_03
talk NPC_CH_POTION accepted @SN_TALK_QNO_TQ_BAEKRYEONG_2_04
talk NPC_CH_POTION denied @SN_TALK_QNO_TQ_BAEKRYEONG_2_05
talk NPC_CH_POTION not-done @SN_TALK_QNO_TQ_BAEKRYEONG_2_06
talk NPC_CH_POTION middle @SN_TALK_QNO_TQ_BAEKRYEONG_2_08
talk NPC_CH_MOONSHADOW greeting @SN_NPC_CH_MOONSHADOW_QS

# The tomb snake master caught (kind 8): 5 powders of dreams and visions; one on the selected MOB_TQ_SNAKEMAN_CLON within
# 300 catches it as the quest's COS for 60 minutes (the item rule), brought to Yang Yizhi. No powder left: five again
# (its talk: _05, _06, _07; the day's spent _08: `resupply-daily`, its start not counted).
quest QNO_TQ_BAEKRYOUNG_4
start level 88-
start after all QNO_TQ_BAEKRYOUNG_3
repeat 1
giver NPC_CH_POTION
npc NPC_CH_POTION
finish NPC_CH_POTION
start-item ITEM_QNO_TQ_BAEKRYEONG_4_01 5
resupply ITEM_QNO_TQ_BAEKRYEONG_4_01 5
resupply-daily
pay-take ITEM_QNO_TQ_BAEKRYEONG_4_01
goal capture MOB_QT_TQ_SNAKEMAN_CLON_COS at NPC_CH_POTION text @SN_CON_QNO_TQ_BAEKRYOUNG_4_01
talk NPC_CH_POTION greeting @SN_NPC_CH_POTION_QS
talk NPC_CH_POTION offer @SN_TALK_QNO_TQ_BAEKRYEONG_4_01
talk NPC_CH_POTION accepted @SN_TALK_QNO_TQ_BAEKRYEONG_4_02
talk NPC_CH_POTION denied @SN_TALK_QNO_TQ_BAEKRYEONG_4_03
talk NPC_CH_POTION not-done @SN_TALK_QNO_TQ_BAEKRYEONG_4_04
talk NPC_CH_POTION bag-full @SN_TALK_QNO_TQ_BAEKRYEONG_4_09
talk NPC_CH_POTION resupply @SN_TALK_QNO_TQ_BAEKRYEONG_4_05 row @SN_TALK_QNO_TQ_BAEKRYEONG_4_06
talk NPC_CH_POTION resupply @SN_TALK_QNO_TQ_BAEKRYEONG_4_07
talk NPC_CH_POTION resupply-spent @SN_TALK_QNO_TQ_BAEKRYEONG_4_08
talk NPC_CH_POTION done @SN_TALK_QNO_TQ_BAEKRYEONG_4_10
talk NPC_CH_POTION done-now @SN_TALK_QNO_TQ_BAEKRYEONG_4_11

# --- The Tomb of Qin Shi: the secret martial arts book (STONEBOARD_3 the script's) --------------------------------------

# Gwakwi's tablet fragment to Haraho in Donwhang (given at the start), who hands back the identified tablet and his wine.
quest QNO_TQ_STONEBOARD_1 # data only
start level 80-
repeat 1
giver NPC_CH_GENARAL_SW
npc NPC_CH_GENARAL_SW NPC_WC_GENARAL_SW
finish NPC_CH_GENARAL_SW
start-item ITEM_QNO_TQ_STONEBOARD_1_01 1
goal deliver NPC_WC_GENARAL_SW ITEM_QNO_TQ_STONEBOARD_1_01 1 get ITEM_QNO_TQ_STONEBOARD_1_03 1 get ITEM_QNO_TQ_STONEBOARD_1_02 1 talk @SN_TALK_QNO_TQ_STONEBOARD_1_06 text @SN_CON_QNO_TQ_STONEBOARD_1_01
talk NPC_CH_GENARAL_SW greeting @SN_NPC_CH_GENARAL_SW_QS
talk NPC_CH_GENARAL_SW offer @SN_TALK_QNO_TQ_STONEBOARD_1_01
talk NPC_CH_GENARAL_SW accepted @SN_TALK_QNO_TQ_STONEBOARD_1_02
talk NPC_CH_GENARAL_SW denied @SN_TALK_QNO_TQ_STONEBOARD_1_03
talk NPC_CH_GENARAL_SW not-done @SN_TALK_QNO_TQ_STONEBOARD_1_04
talk NPC_CH_GENARAL_SW bag-full @SN_TALK_QNO_TQ_STONEBOARD_1_05
talk NPC_CH_GENARAL_SW done @SN_TALK_QNO_TQ_STONEBOARD_1_08
talk NPC_CH_GENARAL_SW middle @SN_TALK_QNO_TQ_STONEBOARD_1_09
talk NPC_CH_GENARAL_SW done-now @SN_TALK_QNO_TQ_STONEBOARD_1_10
talk NPC_WC_GENARAL_SW greeting @SN_NPC_WC_GENARAL_SW_QS
talk NPC_WC_GENARAL_SW middle @SN_TALK_QNO_TQ_STONEBOARD_1_07
talk NPC_WC_GENARAL_SW not-done @SN_TALK_QNO_TQ_STONEBOARD_1_07
talk NPC_WC_GENARAL_SW bag-full @SN_TALK_QNO_TQ_STONEBOARD_1_05

# The four tablets, each at 1 % from the royal warriors (MOB_TQ_ROYALSOLDIER; four gatherings). Its greeting is the
# Jangan smith's.
quest QNO_TQ_STONEBOARD_2 # data only
start level 80-
start after all QNO_TQ_STONEBOARD_1
repeat 1
giver NPC_CH_GENARAL_SW
npc NPC_CH_GENARAL_SW
finish NPC_CH_GENARAL_SW
goal drop 1 ITEM_QNO_TQ_STONEBOARD_2_01 from MOB_TQ_ROYALSOLDIER 1% at NPC_CH_GENARAL_SW text @SN_CON_QNO_TQ_STONEBOARD_2_01
goal drop 1 ITEM_QNO_TQ_STONEBOARD_2_02 from MOB_TQ_ROYALSOLDIER 1% at NPC_CH_GENARAL_SW text @SN_CON_QNO_TQ_STONEBOARD_2_02
goal drop 1 ITEM_QNO_TQ_STONEBOARD_2_03 from MOB_TQ_ROYALSOLDIER 1% at NPC_CH_GENARAL_SW text @SN_CON_QNO_TQ_STONEBOARD_2_03
goal drop 1 ITEM_QNO_TQ_STONEBOARD_2_04 from MOB_TQ_ROYALSOLDIER 1% at NPC_CH_GENARAL_SW text @SN_CON_QNO_TQ_STONEBOARD_2_04
talk NPC_CH_GENARAL_SW greeting @SN_NPC_CH_SMITH_QS
talk NPC_CH_GENARAL_SW offer @SN_TALK_QNO_TQ_STONEBOARD_2_01
talk NPC_CH_GENARAL_SW accepted @SN_TALK_QNO_TQ_STONEBOARD_2_02
talk NPC_CH_GENARAL_SW denied @SN_TALK_QNO_TQ_STONEBOARD_2_03
talk NPC_CH_GENARAL_SW not-done @SN_TALK_QNO_TQ_STONEBOARD_2_04
talk NPC_CH_GENARAL_SW done @SN_TALK_QNO_TQ_STONEBOARD_2_05
talk NPC_CH_GENARAL_SW done-now @SN_TALK_QNO_TQ_STONEBOARD_2_06

# The tablets purified in the quest's instance (INS_QNO_TQ_STONEBOARD_4): a sharing party; Hyun's fake head and the tablet
# given at the start; the head used near the statue of the tomb's first floor by the leader takes the party in (the item
# rule); MOB_QT_TQ_TOMBGENERAL4 killed (kind 2, one) turns the tablet into normal tablets (the tablets and heads taken,
# one ITEM_QNO_TQ_STONEBOARD_4_02 given), asked at the hand-in. Its offer: _01, then the player's question _03 and its
# answer _04. The original says _20 for both done and not-party: kept.
quest QNO_TQ_STONEBOARD_4
start level 80-
start after all QNO_TQ_STONEBOARD_3
start party
repeat 1
giver NPC_CH_SHAMAN
npc NPC_CH_SHAMAN
finish NPC_CH_SHAMAN
start-item ITEM_QNO_TQ_STONEBOARD_4_03 1
start-item ITEM_QNO_TQ_STONEBOARD_4_01 1
need item ITEM_QNO_TQ_STONEBOARD_4_02 1
on kill MOB_QT_TQ_TOMBGENERAL4 need ITEM_QNO_TQ_STONEBOARD_4_01 clear ITEM_QNO_TQ_STONEBOARD_4_01 ITEM_QNO_TQ_STONEBOARD_4_03 give ITEM_QNO_TQ_STONEBOARD_4_02
goal kill 1 MOB_QT_TQ_TOMBGENERAL4 text @SN_CON_QNO_TQ_STONEBOARD_4_01
talk NPC_CH_SHAMAN greeting @SN_NPC_CH_SHAMAN_QS
talk NPC_CH_SHAMAN offer @SN_TALK_QNO_TQ_STONEBOARD_4_01 row @SN_TALK_QNO_TQ_STONEBOARD_4_03
talk NPC_CH_SHAMAN offer @SN_TALK_QNO_TQ_STONEBOARD_4_04
talk NPC_CH_SHAMAN accepted @SN_TALK_QNO_TQ_STONEBOARD_4_02
talk NPC_CH_SHAMAN denied @SN_TALK_QNO_TQ_STONEBOARD_4_05
talk NPC_CH_SHAMAN not-done @SN_TALK_QNO_TQ_STONEBOARD_4_06
talk NPC_CH_SHAMAN missing @SN_TALK_QNO_TQ_STONEBOARD_4_06
talk NPC_CH_SHAMAN bag-full @SN_TALK_QNO_TQ_STONEBOARD_4_07
talk NPC_CH_SHAMAN done @SN_TALK_QNO_TQ_STONEBOARD_4_20
talk NPC_CH_SHAMAN not-party @SN_TALK_QNO_TQ_STONEBOARD_4_20
talk NPC_CH_SHAMAN middle @SN_TALK_QNO_TQ_STONEBOARD_4_21
talk NPC_CH_SHAMAN done-now @SN_TALK_QNO_TQ_STONEBOARD_4_22

# --- Roc Mountain: Ajati's flying ship (FLYSHIP1_1 the script's) --------------------------------------------------------

# Ajati's letter to Salihan (given at the start), the reward at Ajati.
quest QNO_RM_FLYSHIP1_2 # data only
start level 72-
start after all QNO_RM_FLYSHIP1_1
repeat 1
giver NPC_RM_FLYSHIP1
npc NPC_RM_FLYSHIP1 NPC_RM_YOUNGWOMAN1
finish NPC_RM_FLYSHIP1
start-item ITEM_QNO_RM_FLYSHIP1_2_01 1
goal deliver NPC_RM_YOUNGWOMAN1 ITEM_QNO_RM_FLYSHIP1_2_01 1 talk @SN_TALK_QNO_RM_FLYSHIP1_2_07 text @SN_CON_QNO_RM_FLYSHIP1_2
talk NPC_RM_FLYSHIP1 greeting @SN_NPC_RM_FLYSHIP1_QS
talk NPC_RM_FLYSHIP1 offer @SN_TALK_QNO_RM_FLYSHIP1_2_01
talk NPC_RM_FLYSHIP1 accepted @SN_TALK_QNO_RM_FLYSHIP1_2_02
talk NPC_RM_FLYSHIP1 denied @SN_TALK_QNO_RM_FLYSHIP1_2_03
talk NPC_RM_FLYSHIP1 not-done @SN_TALK_QNO_RM_FLYSHIP1_2_04
talk NPC_RM_FLYSHIP1 bag-full @SN_TALK_QNO_RM_FLYSHIP1_2_05
talk NPC_RM_FLYSHIP1 done @SN_TALK_QNO_RM_FLYSHIP1_2_09
talk NPC_RM_FLYSHIP1 done-now @SN_TALK_QNO_RM_FLYSHIP1_2_10
talk NPC_RM_YOUNGWOMAN1 greeting @SN_NPC_RM_YOUNGWOMAN1_QS
talk NPC_RM_YOUNGWOMAN1 middle @SN_TALK_QNO_RM_FLYSHIP1_2_08
talk NPC_RM_YOUNGWOMAN1 not-done @SN_TALK_QNO_RM_FLYSHIP1_2_08

# The Slow One (a sloth antinoc) lured with goat feed (kind 8: 5 feeds; one on the selected MOB_QT_01_ANTELOPE_CLON makes it
# the quest's COS for 50 minutes: the item rule; brought back at a login), brought to Ajati. No feed left: five again
# (_05). Its done-now is _19 (an item's reuse words): kept.
quest QNO_RM_FLYSHIP1_3
start level 72-
start after all QNO_RM_FLYSHIP1_2
repeat 1
giver NPC_RM_FLYSHIP1
npc NPC_RM_FLYSHIP1
finish NPC_RM_FLYSHIP1
start-item ITEM_QNO_RM_FLYSHIP1_3_01 5
resupply ITEM_QNO_RM_FLYSHIP1_3_01 5
pay-take ITEM_QNO_RM_FLYSHIP1_3_01
goal capture MOB_QT_01_ANTELOPE_CLON_COS at NPC_RM_FLYSHIP1 text @SN_CON_QNO_RM_FLYSHIP1_3
talk NPC_RM_FLYSHIP1 greeting @SN_NPC_RM_FLYSHIP1_QS
talk NPC_RM_FLYSHIP1 offer @SN_TALK_QNO_RM_FLYSHIP1_3_01
talk NPC_RM_FLYSHIP1 accepted @SN_TALK_QNO_RM_FLYSHIP1_3_02
talk NPC_RM_FLYSHIP1 denied @SN_TALK_QNO_RM_FLYSHIP1_3_03
talk NPC_RM_FLYSHIP1 not-done @SN_TALK_QNO_RM_FLYSHIP1_3_04
talk NPC_RM_FLYSHIP1 bag-full @SN_TALK_QNO_RM_FLYSHIP1_3_06
talk NPC_RM_FLYSHIP1 resupply @SN_TALK_QNO_RM_FLYSHIP1_3_05
talk NPC_RM_FLYSHIP1 done @SN_TALK_QNO_RM_FLYSHIP1_3_08
talk NPC_RM_FLYSHIP1 done-now @SN_TALK_QNO_RM_FLYSHIP1_3_19

# --- Roc Mountain: Rahid (OLDWOMAN_1 the script's) ----------------------------------------------------------------------

# Bukhra remembers Issan's son: 100 Rocky's feathers (15 % from MOB_RM_ROCKY) or two months' wait (the second mission, kind
# 11, 60 days of the game's clock; the player's pick at the offer: _02 feathers, _06 the wait). Ours: the feathers' way
# (the wait not built: our format has no goal of time). Its greeting is Ajati's and its done line
# SN_TALK_QNO_RM_FLYSHIP1_1_05 (its talk says _14).
quest QNO_RM_OLDWOMAN_2
start level 79-
start after all QNO_RM_OLDWOMAN_1
repeat 1
giver NPC_RM_VILLAGECHIEF
npc NPC_RM_VILLAGECHIEF
finish NPC_RM_VILLAGECHIEF
goal drop 100 ITEM_QNO_RM_OLDWOMAN_2_01 from MOB_RM_ROCKY 15% at NPC_RM_VILLAGECHIEF text @SN_CON_QNO_RM_OLDWOMAN_2_01
talk NPC_RM_VILLAGECHIEF greeting @SN_NPC_RM_FLYSHIP1_QS
talk NPC_RM_VILLAGECHIEF offer @SN_TALK_QNO_RM_OLDWOMAN_2_01
talk NPC_RM_VILLAGECHIEF accepted @SN_TALK_QNO_RM_OLDWOMAN_2_03
talk NPC_RM_VILLAGECHIEF denied @SN_TALK_QNO_RM_OLDWOMAN_2_09
talk NPC_RM_VILLAGECHIEF not-done @SN_TALK_QNO_RM_OLDWOMAN_2_04
talk NPC_RM_VILLAGECHIEF bag-full @SN_TALK_QNO_RM_OLDWOMAN_2_05
talk NPC_RM_VILLAGECHIEF done @SN_TALK_QNO_RM_OLDWOMAN_2_14
talk NPC_RM_VILLAGECHIEF done-now @SN_TALK_QNO_RM_OLDWOMAN_2_12

# Ahmok in Hotan asked about Rahid (a talk, kind 6: his pages _05, _07, _09 between the player's _06 and _08; ours his _07),
# told to Bukhra. The start's job condition (refused when the player's job state is 2): not built.
quest QNO_RM_OLDWOMAN_3
start level 79-
start after all QNO_RM_OLDWOMAN_2
repeat 1
giver NPC_RM_VILLAGECHIEF
npc NPC_RM_VILLAGECHIEF NPC_KT_MINISTER
finish NPC_RM_VILLAGECHIEF
goal talk NPC_KT_MINISTER talk @SN_TALK_QNO_RM_OLDWOMAN_3_07 text @SN_CON_QNO_RM_OLDWOMAN_3_01
talk NPC_RM_VILLAGECHIEF greeting @SN_NPC_RM_VILLAGECHIEF_QS
talk NPC_RM_VILLAGECHIEF offer @SN_TALK_QNO_RM_OLDWOMAN_3_01
talk NPC_RM_VILLAGECHIEF accepted @SN_TALK_QNO_RM_OLDWOMAN_3_02
talk NPC_RM_VILLAGECHIEF denied @SN_TALK_QNO_RM_OLDWOMAN_3_03
talk NPC_RM_VILLAGECHIEF not-done @SN_TALK_QNO_RM_OLDWOMAN_3_04
talk NPC_RM_VILLAGECHIEF done @SN_TALK_QNO_RM_OLDWOMAN_3_11
talk NPC_RM_VILLAGECHIEF middle @SN_TALK_QNO_RM_OLDWOMAN_3_12
talk NPC_RM_VILLAGECHIEF done-now @SN_TALK_QNO_RM_OLDWOMAN_3_13
talk NPC_KT_MINISTER greeting @SN_NPC_KT_MINISTER_QS
talk NPC_KT_MINISTER middle @SN_TALK_QNO_RM_OLDWOMAN_3_10
talk NPC_KT_MINISTER not-done @SN_TALK_QNO_RM_OLDWOMAN_3_10

# The slaves asked about Rahid (a talk at NPC_RM_SLAVE2, who knows: _09; paid there). The two others answer they do not
# know (_05, _07). The same job condition as _3.
quest QNO_RM_OLDWOMAN_4
start level 79-
start after all QNO_RM_OLDWOMAN_3
repeat 1
giver NPC_RM_VILLAGECHIEF
npc NPC_RM_VILLAGECHIEF NPC_RM_SLAVE2 NPC_RM_SLAVE1 NPC_RM_SLAVE3
goal talk NPC_RM_SLAVE2 talk @SN_TALK_QNO_RM_OLDWOMAN_4_09 text @SN_CON_QNO_RM_OLDWOMAN_4
talk NPC_RM_VILLAGECHIEF greeting @SN_NPC_RM_VILLAGECHIEF_QS
talk NPC_RM_VILLAGECHIEF offer @SN_TALK_QNO_RM_OLDWOMAN_4_01
talk NPC_RM_VILLAGECHIEF accepted @SN_TALK_QNO_RM_OLDWOMAN_4_02
talk NPC_RM_VILLAGECHIEF denied @SN_TALK_QNO_RM_OLDWOMAN_4_03
talk NPC_RM_VILLAGECHIEF not-done @SN_TALK_QNO_RM_OLDWOMAN_4_04
talk NPC_RM_VILLAGECHIEF middle @SN_TALK_QNO_RM_OLDWOMAN_4_10
talk NPC_RM_SLAVE2 greeting @SN_NPC_RM_SLAVE2_QS
talk NPC_RM_SLAVE1 greeting @SN_NPC_RM_SLAVE1_QS
talk NPC_RM_SLAVE1 middle @SN_TALK_QNO_RM_OLDWOMAN_4_05
talk NPC_RM_SLAVE1 not-done @SN_TALK_QNO_RM_OLDWOMAN_4_05
talk NPC_RM_SLAVE3 greeting @SN_NPC_RM_SLAVE3_QS
talk NPC_RM_SLAVE3 middle @SN_TALK_QNO_RM_OLDWOMAN_4_07
talk NPC_RM_SLAVE3 not-done @SN_TALK_QNO_RM_OLDWOMAN_4_07

# The seven rainbow grasses: the vigor of Roc Mountain (a helper gathering, 100 % from MOB_RM_ROCKY, 5 a drop: ours one)
# used at the seven peaks in turn (the item rule, the stages, with its countdown)
# gives a pile of rainbow grass each; 7 the goal (kind 10).
quest QNO_RM_OLDWOMAN_5
start level 79-
start after all QNO_RM_OLDWOMAN_4
repeat 1
giver NPC_RM_SLAVE2
npc NPC_RM_SLAVE2
finish NPC_RM_SLAVE2
goal drop 7 ITEM_QNO_RM_OLDWOMAN_7_03 at NPC_RM_SLAVE2 text @SN_CON_QNO_RM_OLDWOMAN_5_08
goal drop 1000 ITEM_QNO_RM_OLDWOMAN_5_08 from MOB_RM_ROCKY 100% at NPC_RM_SLAVE2 text @SN_CON_QNO_RM_OLDWOMAN_2 hidden
talk NPC_RM_SLAVE2 greeting @SN_NPC_RM_SLAVE2_QS
talk NPC_RM_SLAVE2 offer @SN_TALK_QNO_RM_OLDWOMAN_5_09
talk NPC_RM_SLAVE2 accepted @SN_TALK_QNO_RM_OLDWOMAN_5_10
talk NPC_RM_SLAVE2 denied @SN_TALK_QNO_RM_OLDWOMAN_5_11
talk NPC_RM_SLAVE2 not-done @SN_TALK_QNO_RM_OLDWOMAN_5_12
talk NPC_RM_SLAVE2 done @SN_TALK_QNO_RM_OLDWOMAN_5_13
talk NPC_RM_SLAVE2 done-now @SN_TALK_QNO_RM_OLDWOMAN_5_15

# --- Roc Mountain: the Key of the Bloodline and the Roc's prizes ----------------------------------------------------------
# The key's chain at Village Chief Bukra (MEETROC_1, _2, _6; _3.._5 out of service, _RefQuest Service 0: _6 is their merged
# successor, its texts partly _3's), then the prizes: the Marks of Alliance (_6's rewards) and the Roc's spoils
# (_RefMonster_AssignedItemDrop: the Roc, Devil Saytan, the spirits) traded for alchemy stones. The rewards are
# _RefQuestReward's.

# The Signs of Patrol and Commander (a Feathermask's sign, 50 % from MOB_QT_RM_FEATHERMASK_01; a Rocky clone's, 100 % from
# MOB_QT_RM_ROCKY_CLON_01), then the Slave's Tablet the slave hands over (a delivery of nothing, its second list one tablet:
# _05 his words, _06 after, _07 no room). The start names the chief and the slave (each with a greeting); the menus
# show it at the chief.
quest QNO_RM_MEETROC_1 # data only
start level 80-
repeat 1
giver NPC_RM_VILLAGECHIEF
npc NPC_RM_VILLAGECHIEF NPC_RM_SLAVE1
finish NPC_RM_VILLAGECHIEF
goal drop 1 ITEM_QNO_RM_MEETROC_1_01 from MOB_QT_RM_FEATHERMASK_01 50% at NPC_RM_VILLAGECHIEF text @SN_CON_QNO_RM_MEETROC_1_01
goal drop 1 ITEM_QNO_RM_MEETROC_1_02 from MOB_QT_RM_ROCKY_CLON_01 100% at NPC_RM_VILLAGECHIEF text @SN_CON_QNO_RM_MEETROC_1_02
goal deliver NPC_RM_SLAVE1 get ITEM_QNO_RM_MEETROC_1_03 1 talk @SN_TALK_QNO_RM_MEETROC_1_05 text @SN_CON_QNO_RM_MEETROC_1_03
talk NPC_RM_VILLAGECHIEF greeting @SN_NPC_RM_VILLAGECHIEF_QS
talk NPC_RM_VILLAGECHIEF offer @SN_TALK_QNO_RM_MEETROC_1_01
talk NPC_RM_VILLAGECHIEF accepted @SN_TALK_QNO_RM_MEETROC_1_02
talk NPC_RM_VILLAGECHIEF denied @SN_TALK_QNO_RM_MEETROC_1_03
talk NPC_RM_VILLAGECHIEF not-done @SN_TALK_QNO_RM_MEETROC_1_04
talk NPC_RM_VILLAGECHIEF done @SN_TALK_QNO_RM_MEETROC_1_08
talk NPC_RM_VILLAGECHIEF done-now @SN_TALK_QNO_RM_MEETROC_1_09
talk NPC_RM_SLAVE1 greeting @SN_NPC_RM_SLAVE1_QS
talk NPC_RM_SLAVE1 middle @SN_TALK_QNO_RM_MEETROC_1_06
talk NPC_RM_SLAVE1 not-done @SN_TALK_QNO_RM_MEETROC_1_06
talk NPC_RM_SLAVE1 bag-full @SN_TALK_QNO_RM_MEETROC_1_07

# The Soul Stones of the Roc clans: 5 from the wing tribe (20 % from MOB_RM_WINGTRIBE_CLON and MOB_RM_WINGTRIBE), 7 from its
# spearmen (15 % each). Its finish clears _1's record (status 0, count 0; saved): the chain may
# be gone through again.
quest QNO_RM_MEETROC_2
start level 80-
start after one QNO_RM_MEETROC_1
repeat 1
finish-reset QNO_RM_MEETROC_1
giver NPC_RM_VILLAGECHIEF
npc NPC_RM_VILLAGECHIEF
finish NPC_RM_VILLAGECHIEF
goal drop 5 ITEM_QNO_RM_MEETROC_2_01 from MOB_RM_WINGTRIBE_CLON 20% MOB_RM_WINGTRIBE 20% at NPC_RM_VILLAGECHIEF text @SN_CON_QNO_RM_MEETROC_2_01
goal drop 7 ITEM_QNO_RM_MEETROC_2_02 from MOB_RM_WINGTRIBESPEAR_CLON 15% MOB_RM_WINGTRIBESPEAR 15% at NPC_RM_VILLAGECHIEF text @SN_CON_QNO_RM_MEETROC_2_02
talk NPC_RM_VILLAGECHIEF greeting @SN_NPC_RM_VILLAGECHIEF_QS
talk NPC_RM_VILLAGECHIEF offer @SN_TALK_QNO_RM_MEETROC_2_01
talk NPC_RM_VILLAGECHIEF accepted @SN_TALK_QNO_RM_MEETROC_2_02
talk NPC_RM_VILLAGECHIEF denied @SN_TALK_QNO_RM_MEETROC_2_03
talk NPC_RM_VILLAGECHIEF not-done @SN_TALK_QNO_RM_MEETROC_2_04
talk NPC_RM_VILLAGECHIEF done @SN_TALK_QNO_RM_MEETROC_2_05
talk NPC_RM_VILLAGECHIEF done-now @SN_TALK_QNO_RM_MEETROC_2_06

# The Guardrokies of the seven peaks (kind 2, one each: MOB_QT_01.._07_ROCKY_CLON2), in a party sharing experience and items
# (a party sharing experience and items); the chief's seal given at the start (kind 4, ITEM_QNO_RM_MEETROC_6_03, not
# counted). The kills are told as each is done; at the pay and the finish every _6_03 and _6_02 taken (_6_02 the retired
# _3.._5's), then _2's record cleared. The reward:
# the Key of the Bloodline (ITEM_QNO_RM_MEETROC_6_01) and the three Marks of Alliance (_RefQuestReward).
quest QNO_RM_MEETROC_6
start level 80-
start after one QNO_RM_MEETROC_2
start party
repeat 1
finish-reset QNO_RM_MEETROC_2
giver NPC_RM_VILLAGECHIEF
npc NPC_RM_VILLAGECHIEF
finish NPC_RM_VILLAGECHIEF
start-item ITEM_QNO_RM_MEETROC_6_03 1
pay-take ITEM_QNO_RM_MEETROC_6_03
pay-take ITEM_QNO_RM_MEETROC_6_02
goal kill 1 MOB_QT_01_ROCKY_CLON2 text @SN_CON_QNO_RM_MEETROC_6_01
goal kill 1 MOB_QT_02_ROCKY_CLON2 text @SN_CON_QNO_RM_MEETROC_6_02
goal kill 1 MOB_QT_03_ROCKY_CLON2 text @SN_CON_QNO_RM_MEETROC_6_03
goal kill 1 MOB_QT_04_ROCKY_CLON2 text @SN_CON_QNO_RM_MEETROC_6_04
goal kill 1 MOB_QT_05_ROCKY_CLON2 text @SN_CON_QNO_RM_MEETROC_6_05
goal kill 1 MOB_QT_06_ROCKY_CLON2 text @SN_CON_QNO_RM_MEETROC_6_06
goal kill 1 MOB_QT_07_ROCKY_CLON2 text @SN_CON_QNO_RM_MEETROC_6_07
talk NPC_RM_VILLAGECHIEF greeting @SN_NPC_RM_VILLAGECHIEF_QS
talk NPC_RM_VILLAGECHIEF offer @SN_TALK_QNO_RM_MEETROC_6_01
talk NPC_RM_VILLAGECHIEF accepted @SN_TALK_QNO_RM_MEETROC_6_02
talk NPC_RM_VILLAGECHIEF denied @SN_TALK_QNO_RM_MEETROC_6_03
talk NPC_RM_VILLAGECHIEF not-done @SN_TALK_QNO_RM_MEETROC_6_04
talk NPC_RM_VILLAGECHIEF done @SN_TALK_QNO_RM_MEETROC_6_05
talk NPC_RM_VILLAGECHIEF bag-full @SN_TALK_QNO_RM_MEETROC_6_07
talk NPC_RM_VILLAGECHIEF done-now @SN_TALK_QNO_RM_MEETROC_6_06

# The prizes. Each offered only while one of its item is held (start flag 4, its list), without a repeat limit; its
# gathering (no chance: the bag's count against the goal's) taken at the hand-in; given up, the items stay.
# 7 Marks of Alliance (ITEM_QNO_RM_MEETROC_ECT_01) at Salrihan.
quest QNO_RM_MEETROC_ECT_1
start level 80-
start holding ITEM_QNO_RM_MEETROC_ECT_01
repeat any
give-up keeps-items
giver NPC_RM_YOUNGWOMAN1
npc NPC_RM_YOUNGWOMAN1
finish NPC_RM_YOUNGWOMAN1
goal drop 7 ITEM_QNO_RM_MEETROC_ECT_01 at NPC_RM_YOUNGWOMAN1 text @SN_CON_QNO_RM_MEETROC_ECT_1
talk NPC_RM_YOUNGWOMAN1 greeting @SN_NPC_RM_YOUNGWOMAN1_QS
talk NPC_RM_YOUNGWOMAN1 offer @SN_TALK_QNO_RM_MEETROC_ECT_1_01
talk NPC_RM_YOUNGWOMAN1 accepted @SN_TALK_QNO_RM_MEETROC_ECT_1_02
talk NPC_RM_YOUNGWOMAN1 denied @SN_TALK_QNO_RM_MEETROC_ECT_1_03
talk NPC_RM_YOUNGWOMAN1 not-done @SN_TALK_QNO_RM_MEETROC_ECT_1_04
talk NPC_RM_YOUNGWOMAN1 bag-full @SN_TALK_QNO_RM_MEETROC_ECT_1_05
talk NPC_RM_YOUNGWOMAN1 done @SN_TALK_QNO_RM_MEETROC_ECT_1_06
talk NPC_RM_YOUNGWOMAN1 done-now @SN_TALK_QNO_RM_MEETROC_ECT_1_07

# 10 Marks of Alliance (ITEM_QNO_RM_MEETROC_ECT_02) at Anashya.
quest QNO_RM_MEETROC_ECT_2
start level 80-
start holding ITEM_QNO_RM_MEETROC_ECT_02
repeat any
give-up keeps-items
giver NPC_RM_YOUNGWOMAN2
npc NPC_RM_YOUNGWOMAN2
finish NPC_RM_YOUNGWOMAN2
goal drop 10 ITEM_QNO_RM_MEETROC_ECT_02 at NPC_RM_YOUNGWOMAN2 text @SN_CON_QNO_RM_MEETROC_ECT_2
talk NPC_RM_YOUNGWOMAN2 greeting @SN_NPC_RM_YOUNGWOMAN2_QS
talk NPC_RM_YOUNGWOMAN2 offer @SN_TALK_QNO_RM_MEETROC_ECT_2_01
talk NPC_RM_YOUNGWOMAN2 accepted @SN_TALK_QNO_RM_MEETROC_ECT_2_02
talk NPC_RM_YOUNGWOMAN2 denied @SN_TALK_QNO_RM_MEETROC_ECT_2_03
talk NPC_RM_YOUNGWOMAN2 not-done @SN_TALK_QNO_RM_MEETROC_ECT_2_04
talk NPC_RM_YOUNGWOMAN2 bag-full @SN_TALK_QNO_RM_MEETROC_ECT_2_05
talk NPC_RM_YOUNGWOMAN2 done @SN_TALK_QNO_RM_MEETROC_ECT_2_06
talk NPC_RM_YOUNGWOMAN2 done-now @SN_TALK_QNO_RM_MEETROC_ECT_2_07

# 15 Marks of Alliance (ITEM_QNO_RM_MEETROC_ECT_03) at Bukra.
quest QNO_RM_MEETROC_ECT_3
start level 80-
start holding ITEM_QNO_RM_MEETROC_ECT_03
repeat any
give-up keeps-items
giver NPC_RM_VILLAGECHIEF
npc NPC_RM_VILLAGECHIEF
finish NPC_RM_VILLAGECHIEF
goal drop 15 ITEM_QNO_RM_MEETROC_ECT_03 at NPC_RM_VILLAGECHIEF text @SN_CON_QNO_RM_MEETROC_ECT_3
talk NPC_RM_VILLAGECHIEF greeting @SN_NPC_RM_VILLAGECHIEF_QS
talk NPC_RM_VILLAGECHIEF offer @SN_TALK_QNO_RM_MEETROC_ECT_3_01
talk NPC_RM_VILLAGECHIEF accepted @SN_TALK_QNO_RM_MEETROC_ECT_3_02
talk NPC_RM_VILLAGECHIEF denied @SN_TALK_QNO_RM_MEETROC_ECT_3_03
talk NPC_RM_VILLAGECHIEF not-done @SN_TALK_QNO_RM_MEETROC_ECT_3_04
talk NPC_RM_VILLAGECHIEF bag-full @SN_TALK_QNO_RM_MEETROC_ECT_3_05
talk NPC_RM_VILLAGECHIEF done @SN_TALK_QNO_RM_MEETROC_ECT_3_06
talk NPC_RM_VILLAGECHIEF done-now @SN_TALK_QNO_RM_MEETROC_ECT_3_07

# 3 of the Roc's spoils (ITEM_QNO_RM_ARCHEMY_1) at NPC_KT_SMITH.
quest QNO_RM_ARCHEMY_1
start level 80-
start holding ITEM_QNO_RM_ARCHEMY_1
repeat any
give-up keeps-items
giver NPC_KT_SMITH
npc NPC_KT_SMITH
finish NPC_KT_SMITH
goal drop 3 ITEM_QNO_RM_ARCHEMY_1 at NPC_KT_SMITH text @SN_CON_QNO_RM_ARCHEMY_1
talk NPC_KT_SMITH greeting @SN_NPC_KT_SMITH_QS
talk NPC_KT_SMITH offer @SN_TALK_QNO_RM_ARCHEMY_1_01
talk NPC_KT_SMITH accepted @SN_TALK_QNO_RM_ARCHEMY_1_02
talk NPC_KT_SMITH denied @SN_TALK_QNO_RM_ARCHEMY_1_03
talk NPC_KT_SMITH not-done @SN_TALK_QNO_RM_ARCHEMY_1_04
talk NPC_KT_SMITH bag-full @SN_TALK_QNO_RM_ARCHEMY_1_05
talk NPC_KT_SMITH done @SN_TALK_QNO_RM_ARCHEMY_1_06
talk NPC_KT_SMITH done-now @SN_TALK_QNO_RM_ARCHEMY_1_07

# 3 of the Roc's spoils (ITEM_QNO_RM_ARCHEMY_2) at NPC_KT_ARMOR.
quest QNO_RM_ARCHEMY_2
start level 80-
start holding ITEM_QNO_RM_ARCHEMY_2
repeat any
give-up keeps-items
giver NPC_KT_ARMOR
npc NPC_KT_ARMOR
finish NPC_KT_ARMOR
goal drop 3 ITEM_QNO_RM_ARCHEMY_2 at NPC_KT_ARMOR text @SN_CON_QNO_RM_ARCHEMY_2
talk NPC_KT_ARMOR greeting @SN_NPC_KT_ARMOR_QS
talk NPC_KT_ARMOR offer @SN_TALK_QNO_RM_ARCHEMY_2_01
talk NPC_KT_ARMOR accepted @SN_TALK_QNO_RM_ARCHEMY_2_02
talk NPC_KT_ARMOR denied @SN_TALK_QNO_RM_ARCHEMY_2_03
talk NPC_KT_ARMOR not-done @SN_TALK_QNO_RM_ARCHEMY_2_04
talk NPC_KT_ARMOR bag-full @SN_TALK_QNO_RM_ARCHEMY_2_05
talk NPC_KT_ARMOR done @SN_TALK_QNO_RM_ARCHEMY_2_06
talk NPC_KT_ARMOR done-now @SN_TALK_QNO_RM_ARCHEMY_2_07

# 3 of the Roc's spoils (ITEM_QNO_RM_ARCHEMY_3) at NPC_KT_ACCESSORY.
quest QNO_RM_ARCHEMY_3
start level 80-
start holding ITEM_QNO_RM_ARCHEMY_3
repeat any
give-up keeps-items
giver NPC_KT_ACCESSORY
npc NPC_KT_ACCESSORY
finish NPC_KT_ACCESSORY
goal drop 3 ITEM_QNO_RM_ARCHEMY_3 at NPC_KT_ACCESSORY text @SN_CON_QNO_RM_ARCHEMY_3
talk NPC_KT_ACCESSORY greeting @SN_NPC_KT_ACCESSORY_QS
talk NPC_KT_ACCESSORY offer @SN_TALK_QNO_RM_ARCHEMY_3_01
talk NPC_KT_ACCESSORY accepted @SN_TALK_QNO_RM_ARCHEMY_3_02
talk NPC_KT_ACCESSORY denied @SN_TALK_QNO_RM_ARCHEMY_3_03
talk NPC_KT_ACCESSORY not-done @SN_TALK_QNO_RM_ARCHEMY_3_04
talk NPC_KT_ACCESSORY bag-full @SN_TALK_QNO_RM_ARCHEMY_3_05
talk NPC_KT_ACCESSORY done @SN_TALK_QNO_RM_ARCHEMY_3_06
talk NPC_KT_ACCESSORY done-now @SN_TALK_QNO_RM_ARCHEMY_3_07

# 3 of Devil Saytan's spoils (ITEM_QNO_RM_ARCHEMY_4) at NPC_KT_MANAGER.
quest QNO_RM_ARCHEMY_4
start level 80-
start holding ITEM_QNO_RM_ARCHEMY_4
repeat any
give-up keeps-items
giver NPC_KT_MANAGER
npc NPC_KT_MANAGER
finish NPC_KT_MANAGER
goal drop 3 ITEM_QNO_RM_ARCHEMY_4 at NPC_KT_MANAGER text @SN_CON_QNO_RM_ARCHEMY_4
talk NPC_KT_MANAGER greeting @SN_NPC_KT_MANAGER_QS
talk NPC_KT_MANAGER offer @SN_TALK_QNO_RM_ARCHEMY_4_01
talk NPC_KT_MANAGER accepted @SN_TALK_QNO_RM_ARCHEMY_4_02
talk NPC_KT_MANAGER denied @SN_TALK_QNO_RM_ARCHEMY_4_03
talk NPC_KT_MANAGER not-done @SN_TALK_QNO_RM_ARCHEMY_4_04
talk NPC_KT_MANAGER bag-full @SN_TALK_QNO_RM_ARCHEMY_4_05
talk NPC_KT_MANAGER done @SN_TALK_QNO_RM_ARCHEMY_4_06
talk NPC_KT_MANAGER done-now @SN_TALK_QNO_RM_ARCHEMY_4_07

# 3 of the spirits' spoils (ITEM_QNO_RM_ARCHEMY_5) at NPC_KT_MANAGER.
quest QNO_RM_ARCHEMY_5
start level 80-
start holding ITEM_QNO_RM_ARCHEMY_5
repeat any
give-up keeps-items
giver NPC_KT_MANAGER
npc NPC_KT_MANAGER
finish NPC_KT_MANAGER
goal drop 3 ITEM_QNO_RM_ARCHEMY_5 at NPC_KT_MANAGER text @SN_CON_QNO_RM_ARCHEMY_5
talk NPC_KT_MANAGER greeting @SN_NPC_KT_MANAGER_QS
talk NPC_KT_MANAGER offer @SN_TALK_QNO_RM_ARCHEMY_5_01
talk NPC_KT_MANAGER accepted @SN_TALK_QNO_RM_ARCHEMY_5_02
talk NPC_KT_MANAGER denied @SN_TALK_QNO_RM_ARCHEMY_5_03
talk NPC_KT_MANAGER not-done @SN_TALK_QNO_RM_ARCHEMY_5_04
talk NPC_KT_MANAGER bag-full @SN_TALK_QNO_RM_ARCHEMY_5_05
talk NPC_KT_MANAGER done @SN_TALK_QNO_RM_ARCHEMY_5_06
talk NPC_KT_MANAGER done-now @SN_TALK_QNO_RM_ARCHEMY_5_07

# 3 of the spirits' (MOB_RM_SPIRITS_CLON) spoils (ITEM_QNO_RM_ARCHEMY_6) at NPC_KT_MANAGER.
quest QNO_RM_ARCHEMY_6
start level 80-
start holding ITEM_QNO_RM_ARCHEMY_6
repeat any
give-up keeps-items
giver NPC_KT_MANAGER
npc NPC_KT_MANAGER
finish NPC_KT_MANAGER
goal drop 3 ITEM_QNO_RM_ARCHEMY_6 at NPC_KT_MANAGER text @SN_CON_QNO_RM_ARCHEMY_6
talk NPC_KT_MANAGER greeting @SN_NPC_KT_MANAGER_QS
talk NPC_KT_MANAGER offer @SN_TALK_QNO_RM_ARCHEMY_6_01
talk NPC_KT_MANAGER accepted @SN_TALK_QNO_RM_ARCHEMY_6_02
talk NPC_KT_MANAGER denied @SN_TALK_QNO_RM_ARCHEMY_6_03
talk NPC_KT_MANAGER not-done @SN_TALK_QNO_RM_ARCHEMY_6_04
talk NPC_KT_MANAGER bag-full @SN_TALK_QNO_RM_ARCHEMY_6_05
talk NPC_KT_MANAGER done @SN_TALK_QNO_RM_ARCHEMY_6_06
talk NPC_KT_MANAGER done-now @SN_TALK_QNO_RM_ARCHEMY_6_07

# 3 of the spirits' (_CLON2) spoils (ITEM_QNO_RM_ARCHEMY_7) at NPC_KT_MANAGER.
quest QNO_RM_ARCHEMY_7
start level 80-
start holding ITEM_QNO_RM_ARCHEMY_7
repeat any
give-up keeps-items
giver NPC_KT_MANAGER
npc NPC_KT_MANAGER
finish NPC_KT_MANAGER
goal drop 3 ITEM_QNO_RM_ARCHEMY_7 at NPC_KT_MANAGER text @SN_CON_QNO_RM_ARCHEMY_7
talk NPC_KT_MANAGER greeting @SN_NPC_KT_MANAGER_QS
talk NPC_KT_MANAGER offer @SN_TALK_QNO_RM_ARCHEMY_7_01
talk NPC_KT_MANAGER accepted @SN_TALK_QNO_RM_ARCHEMY_7_02
talk NPC_KT_MANAGER denied @SN_TALK_QNO_RM_ARCHEMY_7_03
talk NPC_KT_MANAGER not-done @SN_TALK_QNO_RM_ARCHEMY_7_04
talk NPC_KT_MANAGER bag-full @SN_TALK_QNO_RM_ARCHEMY_7_05
talk NPC_KT_MANAGER done @SN_TALK_QNO_RM_ARCHEMY_7_06
talk NPC_KT_MANAGER done-now @SN_TALK_QNO_RM_ARCHEMY_7_07

# 3 of the spirits' (_CLON3) spoils (ITEM_QNO_RM_ARCHEMY_8) at NPC_KT_MANAGER.
quest QNO_RM_ARCHEMY_8
start level 80-
start holding ITEM_QNO_RM_ARCHEMY_8
repeat any
give-up keeps-items
giver NPC_KT_MANAGER
npc NPC_KT_MANAGER
finish NPC_KT_MANAGER
goal drop 3 ITEM_QNO_RM_ARCHEMY_8 at NPC_KT_MANAGER text @SN_CON_QNO_RM_ARCHEMY_8
talk NPC_KT_MANAGER greeting @SN_NPC_KT_MANAGER_QS
talk NPC_KT_MANAGER offer @SN_TALK_QNO_RM_ARCHEMY_8_01
talk NPC_KT_MANAGER accepted @SN_TALK_QNO_RM_ARCHEMY_8_02
talk NPC_KT_MANAGER denied @SN_TALK_QNO_RM_ARCHEMY_8_03
talk NPC_KT_MANAGER not-done @SN_TALK_QNO_RM_ARCHEMY_8_04
talk NPC_KT_MANAGER bag-full @SN_TALK_QNO_RM_ARCHEMY_8_05
talk NPC_KT_MANAGER done @SN_TALK_QNO_RM_ARCHEMY_8_06
talk NPC_KT_MANAGER done-now @SN_TALK_QNO_RM_ARCHEMY_8_07

# --- The trade quests: a specialty NPC's market opened to a trader. Each in a trader's suit (job state 1), once; the
# delivery's item given at the start and handed back by its NPC, then 20 kills. AM_SPECIAL's quest is not in 1.188's
# service.

# Jangan's second market.
quest QNO_TRADE_CH_SPECIAL2_1
start level 20-
start job trader
repeat 1
giver NPC_CH_SPECIAL2
npc NPC_CH_SPECIAL2 NPC_CH_KISAENG3
finish NPC_CH_SPECIAL2
start-item ITEM_QNO_TRADE_CH_SPECIAL2_1_01 1
goal deliver NPC_CH_KISAENG3 ITEM_QNO_TRADE_CH_SPECIAL2_1_01 1 get ITEM_QNO_TRADE_CH_SPECIAL2_1_02 1 talk @SN_TALK_QNO_TRADE_CH_SPECIAL2_1_09 text @SN_CON_QNO_TRADE_CH_SPECIAL2_1_01
goal kill 20 MOB_CH_WHITETIGER text @SN_CON_QNO_TRADE_CH_SPECIAL2_1_02
talk NPC_CH_SPECIAL2 greeting @SN_NPC_CH_SPECIAL2_QS
talk NPC_CH_SPECIAL2 offer @SN_TALK_QNO_TRADE_CH_SPECIAL2_1_01
talk NPC_CH_SPECIAL2 accepted @SN_TALK_QNO_TRADE_CH_SPECIAL2_1_02
talk NPC_CH_SPECIAL2 denied @SN_TALK_QNO_TRADE_CH_SPECIAL2_1_03
talk NPC_CH_SPECIAL2 not-done @SN_TALK_QNO_TRADE_CH_SPECIAL2_1_04
talk NPC_CH_SPECIAL2 bag-full @SN_TALK_QNO_TRADE_CH_SPECIAL2_1_07
talk NPC_CH_SPECIAL2 done @SN_TALK_QNO_TRADE_CH_SPECIAL2_1_12
talk NPC_CH_SPECIAL2 done-now @SN_TALK_QNO_TRADE_CH_SPECIAL2_1_13
talk NPC_CH_KISAENG3 greeting @SN_NPC_CH_KISAENG3_QS
talk NPC_CH_KISAENG3 middle @SN_TALK_QNO_TRADE_CH_SPECIAL2_1_10
talk NPC_CH_KISAENG3 not-done @SN_TALK_QNO_TRADE_CH_SPECIAL2_1_10
talk NPC_CH_KISAENG3 bag-full @SN_TALK_QNO_TRADE_CH_SPECIAL2_1_11

# Donwhang's second market.
quest QNO_TRADE_WC_SPECIAL2_1
start level 30-
start job trader
repeat 1
giver NPC_WC_SPECIAL2
npc NPC_WC_SPECIAL2 NPC_WC_WAREHOUSE_M
finish NPC_WC_SPECIAL2
start-item ITEM_QNO_TRADE_WC_SPECIAL2_1_01 1
goal deliver NPC_WC_WAREHOUSE_M ITEM_QNO_TRADE_WC_SPECIAL2_1_01 1 get ITEM_QNO_TRADE_WC_SPECIAL2_1_02 1 talk @SN_TALK_QNO_TRADE_WC_SPECIAL2_1_09 text @SN_CON_QNO_TRADE_WC_SPECIAL2_1_01
goal kill 20 MOB_WC_HYEONGCHEON text @SN_CON_QNO_TRADE_WC_SPECIAL2_1_02
talk NPC_WC_SPECIAL2 greeting @SN_NPC_WC_SPECIAL2_QS
talk NPC_WC_SPECIAL2 offer @SN_TALK_QNO_TRADE_WC_SPECIAL2_1_01
talk NPC_WC_SPECIAL2 accepted @SN_TALK_QNO_TRADE_WC_SPECIAL2_1_02
talk NPC_WC_SPECIAL2 denied @SN_TALK_QNO_TRADE_WC_SPECIAL2_1_03
talk NPC_WC_SPECIAL2 not-done @SN_TALK_QNO_TRADE_WC_SPECIAL2_1_04
talk NPC_WC_SPECIAL2 bag-full @SN_TALK_QNO_TRADE_WC_SPECIAL2_1_07
talk NPC_WC_SPECIAL2 done @SN_TALK_QNO_TRADE_WC_SPECIAL2_1_12
talk NPC_WC_SPECIAL2 done-now @SN_TALK_QNO_TRADE_WC_SPECIAL2_1_13
talk NPC_WC_WAREHOUSE_M greeting @SN_NPC_WC_WAREHOUSE_M_QS
talk NPC_WC_WAREHOUSE_M middle @SN_TALK_QNO_TRADE_WC_SPECIAL2_1_10
talk NPC_WC_WAREHOUSE_M not-done @SN_TALK_QNO_TRADE_WC_SPECIAL2_1_10
talk NPC_WC_WAREHOUSE_M bag-full @SN_TALK_QNO_TRADE_WC_SPECIAL2_1_11

# Hotan's second market.
quest QNO_TRADE_TK_SPECIAL_1
start level 60-
start job trader
repeat 1
giver NPC_TK_SPECIAL
npc NPC_TK_SPECIAL NPC_KT_SPECIAL
finish NPC_TK_SPECIAL
start-item ITEM_QNO_TRADE_TK_SPECIAL_1_01 1
goal deliver NPC_KT_SPECIAL ITEM_QNO_TRADE_TK_SPECIAL_1_01 1 get ITEM_QNO_TRADE_TK_SPECIAL_1_02 1 talk @SN_TALK_QNO_TRADE_TK_SPECIAL_1_09 text @SN_CON_QNO_TRADE_TK_SPECIAL_1_01
goal kill 20 MOB_TK_EDIMMU_CLON text @SN_CON_QNO_TRADE_TK_SPECIAL_1_02
talk NPC_TK_SPECIAL greeting @SN_NPC_TK_SPECIAL_QS
talk NPC_TK_SPECIAL offer @SN_TALK_QNO_TRADE_TK_SPECIAL_1_01
talk NPC_TK_SPECIAL accepted @SN_TALK_QNO_TRADE_TK_SPECIAL_1_02
talk NPC_TK_SPECIAL denied @SN_TALK_QNO_TRADE_TK_SPECIAL_1_03
talk NPC_TK_SPECIAL not-done @SN_TALK_QNO_TRADE_TK_SPECIAL_1_04
talk NPC_TK_SPECIAL bag-full @SN_TALK_QNO_TRADE_TK_SPECIAL_1_07
talk NPC_TK_SPECIAL done @SN_TALK_QNO_TRADE_TK_SPECIAL_1_12
talk NPC_TK_SPECIAL done-now @SN_TALK_QNO_TRADE_TK_SPECIAL_1_13
talk NPC_KT_SPECIAL greeting @SN_NPC_KT_SPECIAL_QS
talk NPC_KT_SPECIAL middle @SN_TALK_QNO_TRADE_TK_SPECIAL_1_10
talk NPC_KT_SPECIAL not-done @SN_TALK_QNO_TRADE_TK_SPECIAL_1_10
talk NPC_KT_SPECIAL bag-full @SN_TALK_QNO_TRADE_TK_SPECIAL_1_11

# Roc Mountain's second market.
quest QNO_TRADE_RM_SPECIAL_1
start level 70-
start job trader
repeat 1
giver NPC_RM_SPECIAL
npc NPC_RM_SPECIAL NPC_RM_YOUNGWOMAN1
finish NPC_RM_SPECIAL
start-item ITEM_QNO_TRADE_RM_SPECIAL_1_01 1
goal deliver NPC_RM_YOUNGWOMAN1 ITEM_QNO_TRADE_RM_SPECIAL_1_01 1 get ITEM_QNO_TRADE_RM_SPECIAL_1_02 1 talk @SN_TALK_QNO_TRADE_RM_SPECIAL_1_09 text @SN_CON_QNO_TRADE_RM_SPECIAL_1_01
goal kill 20 MOB_RM_FEATHERMASK_CLON text @SN_CON_QNO_TRADE_RM_SPECIAL_1_02
talk NPC_RM_SPECIAL greeting @SN_NPC_RM_SPECIAL_QS
talk NPC_RM_SPECIAL offer @SN_TALK_QNO_TRADE_RM_SPECIAL_1_01
talk NPC_RM_SPECIAL accepted @SN_TALK_QNO_TRADE_RM_SPECIAL_1_02
talk NPC_RM_SPECIAL denied @SN_TALK_QNO_TRADE_RM_SPECIAL_1_03
talk NPC_RM_SPECIAL not-done @SN_TALK_QNO_TRADE_RM_SPECIAL_1_04
talk NPC_RM_SPECIAL bag-full @SN_TALK_QNO_TRADE_RM_SPECIAL_1_07
talk NPC_RM_SPECIAL done @SN_TALK_QNO_TRADE_RM_SPECIAL_1_12
talk NPC_RM_SPECIAL done-now @SN_TALK_QNO_TRADE_RM_SPECIAL_1_13
talk NPC_RM_YOUNGWOMAN1 greeting @SN_NPC_RM_YOUNGWOMAN1_QS
talk NPC_RM_YOUNGWOMAN1 middle @SN_TALK_QNO_TRADE_RM_SPECIAL_1_10
talk NPC_RM_YOUNGWOMAN1 not-done @SN_TALK_QNO_TRADE_RM_SPECIAL_1_10
talk NPC_RM_YOUNGWOMAN1 bag-full @SN_TALK_QNO_TRADE_RM_SPECIAL_1_11
)";

} // namespace

std::string_view CodedQuestsAsia() { return kCoded; }

} // namespace sr::server
