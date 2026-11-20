// The quests the original codes in its game server rather than its quest script (184 of the 712 the client lists), as
// definitions in our format (docs: game/Quest.h): each quest's start (its level, its race, its repeats, its givers,
// the quests before it), its missions (gather from monsters, kill, deliver, gather from an NPC, talk, capture) and its
// talk (offer, accepted, denied, not-done, done, not-done-2, bag-full, the content, greeting, done-now, ...). Data: the
// numbers, levels, texts' keys and rewards are put in at the import from _RefQuest and _RefQuestReward, as for the
// script's.

#include <string_view>

#include "sr/server/QuestScript.h"

namespace sr::server {

namespace {

constexpr std::string_view kCoded = R"(
# The inventory expansions: a bag larger by _RefQuestReward's Inventory (+10, +4, +4, +4; +10, +4 in Europe).
quest QSP_CH_EXINVENTORY_1
start level 5-
start race china
repeat 1
giver NPC_CH_ACCESSORY
npc NPC_CH_ACCESSORY
finish NPC_CH_ACCESSORY
goal drop 10 ITEM_QSP_CH_EXINVENTORY_1_01 from MOB_CH_MANGNYANG 33% at NPC_CH_ACCESSORY text @SN_CON_QSP_CH_EXINVENTORY_1
talk NPC_CH_ACCESSORY greeting @SN_NPC_CH_ACCESSORY_QS
talk NPC_CH_ACCESSORY offer @SN_TALK_QSP_CH_EXINVENTORY_1_01
talk NPC_CH_ACCESSORY accepted @SN_TALK_QSP_CH_EXINVENTORY_1_02
talk NPC_CH_ACCESSORY denied @SN_TALK_QSP_CH_EXINVENTORY_1_03
talk NPC_CH_ACCESSORY not-done @SN_TALK_QSP_CH_EXINVENTORY_1_04
talk NPC_CH_ACCESSORY done @SN_TALK_QSP_CH_EXINVENTORY_1_05
talk NPC_CH_ACCESSORY done-now @SN_TALK_QSP_CH_EXINVENTORY_1_06

quest QSP_WC_EXINVENTORY_2
start level 30-
start race china
start after all QSP_CH_EXINVENTORY_1
repeat 1
giver NPC_WC_ACCESSORY
npc NPC_WC_ACCESSORY
finish NPC_WC_ACCESSORY
goal drop 20 ITEM_QSP_WC_EXINVENTORY_2_01 from MOB_WC_EARTHKING 20% at NPC_WC_ACCESSORY text @SN_CON_QSP_WC_EXINVENTORY_2
talk NPC_WC_ACCESSORY greeting @SN_NPC_WC_ACCESSORY_QS
talk NPC_WC_ACCESSORY offer @SN_TALK_QSP_WC_EXINVENTORY_2_01
talk NPC_WC_ACCESSORY accepted @SN_TALK_QSP_WC_EXINVENTORY_2_02
talk NPC_WC_ACCESSORY denied @SN_TALK_QSP_WC_EXINVENTORY_2_03
talk NPC_WC_ACCESSORY not-done @SN_TALK_QSP_WC_EXINVENTORY_2_04
talk NPC_WC_ACCESSORY done @SN_TALK_QSP_WC_EXINVENTORY_2_05
talk NPC_WC_ACCESSORY done-now @SN_TALK_QSP_WC_EXINVENTORY_2_06

# Hotan's also takes two General's Tokens and 10,000 gold at the hand-in (short of the tokens, talk _05).
quest QSP_KT_EXINVENTORY_3
start level 60-
start after one QSP_WC_EXINVENTORY_2 QSP_CA_EXINVENTORY_2
repeat 1
giver NPC_KT_ACCESSORY
npc NPC_KT_ACCESSORY
finish NPC_KT_ACCESSORY
need item ITEM_ETC_LEVEL_TOKEN_03 2
need gold 10000
goal drop 30 ITEM_QSP_KT_EXINVENTORY_3_01 from MOB_KK_GWHITESPIDER 10% at NPC_KT_ACCESSORY text @SN_CON_QSP_KT_EXINVENTORY_3
talk NPC_KT_ACCESSORY greeting @SN_NPC_KT_ACCESSORY_QS
talk NPC_KT_ACCESSORY offer @SN_TALK_QSP_KT_EXINVENTORY_3_01
talk NPC_KT_ACCESSORY accepted @SN_TALK_QSP_KT_EXINVENTORY_3_02
talk NPC_KT_ACCESSORY denied @SN_TALK_QSP_KT_EXINVENTORY_3_03
talk NPC_KT_ACCESSORY not-done @SN_TALK_QSP_KT_EXINVENTORY_3_04
talk NPC_KT_ACCESSORY missing @SN_TALK_QSP_KT_EXINVENTORY_3_05
talk NPC_KT_ACCESSORY done @SN_TALK_QSP_KT_EXINVENTORY_3_06
talk NPC_KT_ACCESSORY done-now @SN_TALK_QSP_KT_EXINVENTORY_3_07

quest QSP_RM_EXINVENTORY_4
start level 90-
start after all QSP_KT_EXINVENTORY_3
repeat 1
giver NPC_RM_YOUNGWOMAN2
npc NPC_RM_YOUNGWOMAN2
finish NPC_RM_YOUNGWOMAN2
goal drop 50 ITEM_QSP_RM_EXINVENTORY_4_01 from MOB_RM_ROCKY 10% at NPC_RM_YOUNGWOMAN2 text @SN_CON_QSP_RM_EXINVENTORY_4
talk NPC_RM_YOUNGWOMAN2 greeting @SN_NPC_RM_YOUNGWOMAN2_QS
talk NPC_RM_YOUNGWOMAN2 offer @SN_TALK_QSP_RM_EXINVENTORY_4_01
talk NPC_RM_YOUNGWOMAN2 accepted @SN_TALK_QSP_RM_EXINVENTORY_4_02
talk NPC_RM_YOUNGWOMAN2 denied @SN_TALK_QSP_RM_EXINVENTORY_4_03
talk NPC_RM_YOUNGWOMAN2 not-done @SN_TALK_QSP_RM_EXINVENTORY_4_04
talk NPC_RM_YOUNGWOMAN2 done @SN_TALK_QSP_RM_EXINVENTORY_4_05
talk NPC_RM_YOUNGWOMAN2 done-now @SN_TALK_QSP_RM_EXINVENTORY_4_06

quest QSP_EU_EXINVENTORY_1
start level 5-
start race europe
repeat 1
giver NPC_EU_ACCESSORY
npc NPC_EU_ACCESSORY
finish NPC_EU_ACCESSORY
goal drop 10 ITEM_QSP_EU_EXINVENTORY_1_01 from MOB_EU_MOVOI 33% at NPC_EU_ACCESSORY text @SN_CON_QSP_EU_EXINVENTORY_1
talk NPC_EU_ACCESSORY greeting @SN_NPC_EU_ACCESSORY_QS
talk NPC_EU_ACCESSORY offer @SN_TALK_QSP_EU_EXINVENTORY_1_01
talk NPC_EU_ACCESSORY accepted @SN_TALK_QSP_EU_EXINVENTORY_1_02
talk NPC_EU_ACCESSORY denied @SN_TALK_QSP_EU_EXINVENTORY_1_03
talk NPC_EU_ACCESSORY not-done @SN_TALK_QSP_EU_EXINVENTORY_1_04
talk NPC_EU_ACCESSORY done @SN_TALK_QSP_EU_EXINVENTORY_1_05
talk NPC_EU_ACCESSORY done-now @SN_TALK_QSP_EU_EXINVENTORY_1_06

quest QSP_CA_EXINVENTORY_2
start level 30-
start race europe
start after all QSP_EU_EXINVENTORY_1
repeat 1
giver NPC_CA_ACCESSORY
npc NPC_CA_ACCESSORY
finish NPC_CA_ACCESSORY
goal drop 20 ITEM_QSP_CA_EXINVENTORY_2_01 from MOB_AM_PUNISHER 20% at NPC_CA_ACCESSORY text @SN_CON_QSP_CA_EXINVENTORY_2
talk NPC_CA_ACCESSORY greeting @SN_NPC_CA_ACCESSORY_QS
talk NPC_CA_ACCESSORY offer @SN_TALK_QSP_CA_EXINVENTORY_2_01
talk NPC_CA_ACCESSORY accepted @SN_TALK_QSP_CA_EXINVENTORY_2_02
talk NPC_CA_ACCESSORY denied @SN_TALK_QSP_CA_EXINVENTORY_2_03
talk NPC_CA_ACCESSORY not-done @SN_TALK_QSP_CA_EXINVENTORY_2_04
talk NPC_CA_ACCESSORY done @SN_TALK_QSP_CA_EXINVENTORY_2_05
talk NPC_CA_ACCESSORY done-now @SN_TALK_QSP_CA_EXINVENTORY_2_06

# The Beginner's Assistant: given at its level (_RefCharDefault_Quest), a gift from any town's potion merchant (its
# talk names the five: Constantinople, Samarkand, Jangan, Donwhang, Hotan), _RefQuestReward's items
# (a weapon of the player's country chosen, or scrolls). Talk _01 the gift, _02 the bag full, _03 told when given.
quest QEV_ALL_BASIC_0
start given
start level 1-
repeat 1
npc NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION
goal talk NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION talk @SN_TALK_QEV_ALL_BASIC_0_01 text @SN_CON_QEV_ALL_BASIC_0
talk NPC_CH_POTION bag-full @SN_TALK_QEV_ALL_BASIC_0_02
talk NPC_WC_POTION bag-full @SN_TALK_QEV_ALL_BASIC_0_02
talk NPC_KT_POTION bag-full @SN_TALK_QEV_ALL_BASIC_0_02
talk NPC_EU_POTION bag-full @SN_TALK_QEV_ALL_BASIC_0_02
talk NPC_CA_POTION bag-full @SN_TALK_QEV_ALL_BASIC_0_02
talk NPC_CH_POTION accepted @SN_TALK_QEV_ALL_BASIC_0_03

quest QEV_ALL_BASIC_1
start given
start level 5-
repeat 1
npc NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION
goal talk NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION talk @SN_TALK_QEV_ALL_BASIC_1_01 text @SN_CON_QEV_ALL_BASIC_1
talk NPC_CH_POTION bag-full @SN_TALK_QEV_ALL_BASIC_1_02
talk NPC_WC_POTION bag-full @SN_TALK_QEV_ALL_BASIC_1_02
talk NPC_KT_POTION bag-full @SN_TALK_QEV_ALL_BASIC_1_02
talk NPC_EU_POTION bag-full @SN_TALK_QEV_ALL_BASIC_1_02
talk NPC_CA_POTION bag-full @SN_TALK_QEV_ALL_BASIC_1_02
talk NPC_CH_POTION accepted @SN_TALK_QEV_ALL_BASIC_1_03

quest QEV_ALL_BASIC_2
start given
start level 8-
repeat 1
npc NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION
goal talk NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION talk @SN_TALK_QEV_ALL_BASIC_2_01 text @SN_CON_QEV_ALL_BASIC_2
talk NPC_CH_POTION bag-full @SN_TALK_QEV_ALL_BASIC_2_02
talk NPC_WC_POTION bag-full @SN_TALK_QEV_ALL_BASIC_2_02
talk NPC_KT_POTION bag-full @SN_TALK_QEV_ALL_BASIC_2_02
talk NPC_EU_POTION bag-full @SN_TALK_QEV_ALL_BASIC_2_02
talk NPC_CA_POTION bag-full @SN_TALK_QEV_ALL_BASIC_2_02
talk NPC_CH_POTION accepted @SN_TALK_QEV_ALL_BASIC_2_03

quest QEV_ALL_BASIC_3
start given
start level 10-
repeat 1
npc NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION
goal talk NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION talk @SN_TALK_QEV_ALL_BASIC_3_01 text @SN_CON_QEV_ALL_BASIC_3
talk NPC_CH_POTION bag-full @SN_TALK_QEV_ALL_BASIC_3_02
talk NPC_WC_POTION bag-full @SN_TALK_QEV_ALL_BASIC_3_02
talk NPC_KT_POTION bag-full @SN_TALK_QEV_ALL_BASIC_3_02
talk NPC_EU_POTION bag-full @SN_TALK_QEV_ALL_BASIC_3_02
talk NPC_CA_POTION bag-full @SN_TALK_QEV_ALL_BASIC_3_02
talk NPC_CH_POTION accepted @SN_TALK_QEV_ALL_BASIC_3_03

quest QEV_ALL_BASIC_4
start given
start level 16-
repeat 1
npc NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION
goal talk NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION talk @SN_TALK_QEV_ALL_BASIC_4_01 text @SN_CON_QEV_ALL_BASIC_4
talk NPC_CH_POTION bag-full @SN_TALK_QEV_ALL_BASIC_4_02
talk NPC_WC_POTION bag-full @SN_TALK_QEV_ALL_BASIC_4_02
talk NPC_KT_POTION bag-full @SN_TALK_QEV_ALL_BASIC_4_02
talk NPC_EU_POTION bag-full @SN_TALK_QEV_ALL_BASIC_4_02
talk NPC_CA_POTION bag-full @SN_TALK_QEV_ALL_BASIC_4_02
talk NPC_CH_POTION accepted @SN_TALK_QEV_ALL_BASIC_4_03

quest QEV_ALL_BASIC_5
start given
start level 20-
repeat 1
npc NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION
goal talk NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION talk @SN_TALK_QEV_ALL_BASIC_5_01 text @SN_CON_QEV_ALL_BASIC_5
talk NPC_CH_POTION bag-full @SN_TALK_QEV_ALL_BASIC_5_02
talk NPC_WC_POTION bag-full @SN_TALK_QEV_ALL_BASIC_5_02
talk NPC_KT_POTION bag-full @SN_TALK_QEV_ALL_BASIC_5_02
talk NPC_EU_POTION bag-full @SN_TALK_QEV_ALL_BASIC_5_02
talk NPC_CA_POTION bag-full @SN_TALK_QEV_ALL_BASIC_5_02
talk NPC_CH_POTION accepted @SN_TALK_QEV_ALL_BASIC_5_03

quest QEV_ALL_BASIC_6
start given
start level 24-
repeat 1
npc NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION
goal talk NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION talk @SN_TALK_QEV_ALL_BASIC_6_01 text @SN_CON_QEV_ALL_BASIC_6
talk NPC_CH_POTION bag-full @SN_TALK_QEV_ALL_BASIC_6_02
talk NPC_WC_POTION bag-full @SN_TALK_QEV_ALL_BASIC_6_02
talk NPC_KT_POTION bag-full @SN_TALK_QEV_ALL_BASIC_6_02
talk NPC_EU_POTION bag-full @SN_TALK_QEV_ALL_BASIC_6_02
talk NPC_CA_POTION bag-full @SN_TALK_QEV_ALL_BASIC_6_02
talk NPC_CH_POTION accepted @SN_TALK_QEV_ALL_BASIC_6_03

quest QEV_ALL_BASIC_7
start given
start level 30-
repeat 1
npc NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION
goal talk NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION talk @SN_TALK_QEV_ALL_BASIC_7_01 text @SN_CON_QEV_ALL_BASIC_7
talk NPC_CH_POTION bag-full @SN_TALK_QEV_ALL_BASIC_7_02
talk NPC_WC_POTION bag-full @SN_TALK_QEV_ALL_BASIC_7_02
talk NPC_KT_POTION bag-full @SN_TALK_QEV_ALL_BASIC_7_02
talk NPC_EU_POTION bag-full @SN_TALK_QEV_ALL_BASIC_7_02
talk NPC_CA_POTION bag-full @SN_TALK_QEV_ALL_BASIC_7_02
talk NPC_CH_POTION accepted @SN_TALK_QEV_ALL_BASIC_7_03

quest QEV_ALL_BASIC_8
start given
start level 32-
repeat 1
npc NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION
goal talk NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION talk @SN_TALK_QEV_ALL_BASIC_8_01 text @SN_CON_QEV_ALL_BASIC_8
talk NPC_CH_POTION bag-full @SN_TALK_QEV_ALL_BASIC_8_02
talk NPC_WC_POTION bag-full @SN_TALK_QEV_ALL_BASIC_8_02
talk NPC_KT_POTION bag-full @SN_TALK_QEV_ALL_BASIC_8_02
talk NPC_EU_POTION bag-full @SN_TALK_QEV_ALL_BASIC_8_02
talk NPC_CA_POTION bag-full @SN_TALK_QEV_ALL_BASIC_8_02
talk NPC_CH_POTION accepted @SN_TALK_QEV_ALL_BASIC_8_03

quest QEV_ALL_BASIC_9
start given
start level 40-
repeat 1
npc NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION
goal talk NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION talk @SN_TALK_QEV_ALL_BASIC_9_01 text @SN_CON_QEV_ALL_BASIC_9
talk NPC_CH_POTION bag-full @SN_TALK_QEV_ALL_BASIC_9_02
talk NPC_WC_POTION bag-full @SN_TALK_QEV_ALL_BASIC_9_02
talk NPC_KT_POTION bag-full @SN_TALK_QEV_ALL_BASIC_9_02
talk NPC_EU_POTION bag-full @SN_TALK_QEV_ALL_BASIC_9_02
talk NPC_CA_POTION bag-full @SN_TALK_QEV_ALL_BASIC_9_02
talk NPC_CH_POTION accepted @SN_TALK_QEV_ALL_BASIC_9_03

quest QEV_ALL_BASIC_10
start given
start level 42-
repeat 1
npc NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION
goal talk NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION talk @SN_TALK_QEV_ALL_BASIC_10_01 text @SN_CON_QEV_ALL_BASIC_10
talk NPC_CH_POTION bag-full @SN_TALK_QEV_ALL_BASIC_10_02
talk NPC_WC_POTION bag-full @SN_TALK_QEV_ALL_BASIC_10_02
talk NPC_KT_POTION bag-full @SN_TALK_QEV_ALL_BASIC_10_02
talk NPC_EU_POTION bag-full @SN_TALK_QEV_ALL_BASIC_10_02
talk NPC_CA_POTION bag-full @SN_TALK_QEV_ALL_BASIC_10_02
talk NPC_CH_POTION accepted @SN_TALK_QEV_ALL_BASIC_10_03

quest QEV_ALL_BASIC_11
start given
start level 50-
repeat 1
npc NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION
goal talk NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION talk @SN_TALK_QEV_ALL_BASIC_11_01 text @SN_CON_QEV_ALL_BASIC_11
talk NPC_CH_POTION bag-full @SN_TALK_QEV_ALL_BASIC_11_02
talk NPC_WC_POTION bag-full @SN_TALK_QEV_ALL_BASIC_11_02
talk NPC_KT_POTION bag-full @SN_TALK_QEV_ALL_BASIC_11_02
talk NPC_EU_POTION bag-full @SN_TALK_QEV_ALL_BASIC_11_02
talk NPC_CA_POTION bag-full @SN_TALK_QEV_ALL_BASIC_11_02
talk NPC_CH_POTION accepted @SN_TALK_QEV_ALL_BASIC_11_03

quest QEV_ALL_BASIC_12
start given
start level 60-
repeat 1
npc NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION
goal talk NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION talk @SN_TALK_QEV_ALL_BASIC_12_01 text @SN_CON_QEV_ALL_BASIC_12
talk NPC_CH_POTION bag-full @SN_TALK_QEV_ALL_BASIC_12_02
talk NPC_WC_POTION bag-full @SN_TALK_QEV_ALL_BASIC_12_02
talk NPC_KT_POTION bag-full @SN_TALK_QEV_ALL_BASIC_12_02
talk NPC_EU_POTION bag-full @SN_TALK_QEV_ALL_BASIC_12_02
talk NPC_CA_POTION bag-full @SN_TALK_QEV_ALL_BASIC_12_02
talk NPC_CH_POTION accepted @SN_TALK_QEV_ALL_BASIC_12_03

# The script's quests whose talk the script codes itself (CONVERSATION_SINGLE: they were left waiting), as plain
# definitions; their goals as the script's data gives them.

# A two-page offer: its story, then the request (QNO_KT_POTION_2_CONVERSATION: page 1 _01, page 2 _02 to accept).
quest QNO_KT_POTION_2
start level 67-
repeat 3
giver NPC_KT_POTION
npc NPC_KT_POTION
finish NPC_KT_POTION
goal drop 100 ITEM_QNO_KT_POTION_2_01 from MOB_TK_BONEMAGE_CLON 20% MOB_TK_BONEMAGE 20% at NPC_KT_POTION text @SN_CON_QNO_KT_POTION_2
talk NPC_KT_POTION greeting @SN_NPC_KT_POTION_QS
talk NPC_KT_POTION offer @SN_TALK_QNO_KT_POTION_2_01
talk NPC_KT_POTION offer @SN_TALK_QNO_KT_POTION_2_02
talk NPC_KT_POTION accepted @SN_TALK_QNO_KT_POTION_2_03
talk NPC_KT_POTION denied @SN_TALK_QNO_KT_POTION_2_04
talk NPC_KT_POTION not-done @SN_TALK_QNO_KT_POTION_2_05
talk NPC_KT_POTION done @SN_TALK_QNO_KT_POTION_2_06
talk NPC_KT_POTION done-now @SN_TALK_QNO_KT_POTION_2_07

# Given by the ferryman, the stones taken to the smith (QNO_WC_FERRY3_1_CONVERSATION_NPC_WC_FERRY3 and _SMITH: the
# ferryman's offer _01, accepted _02, not done _04; the smith's done _05, not done _07).
quest QNO_WC_FERRY3_1
start level 30-
repeat 1
giver NPC_WC_FERRY3
npc NPC_WC_FERRY3 NPC_WC_SMITH
finish NPC_WC_SMITH
goal drop 70 ITEM_QNO_WC_FERRY3_1_01 from MOB_WC_HYEONGCHEON 20% at NPC_WC_SMITH text @SN_CON_QNO_WC_FERRY3_1
talk NPC_WC_FERRY3 greeting @SN_NPC_WC_FERRY3_QS
talk NPC_WC_FERRY3 offer @SN_TALK_QNO_WC_FERRY3_1_01
talk NPC_WC_FERRY3 accepted @SN_TALK_QNO_WC_FERRY3_1_02
talk NPC_WC_FERRY3 not-done @SN_TALK_QNO_WC_FERRY3_1_04
talk NPC_WC_SMITH greeting @SN_NPC_WC_FERRY3_QS
talk NPC_WC_SMITH not-done @SN_TALK_QNO_WC_FERRY3_1_07
talk NPC_WC_SMITH done @SN_TALK_QNO_WC_FERRY3_1_05
talk NPC_WC_SMITH done-now @SN_TALK_QNO_WC_FERRY3_1_06

# The ferryman's story (QNO_WC_FERRY4_1_CONVERSATION): asked to listen (_01; refused _07, no room for the gift
# _02), its pages _03 to _06, then 650 experience and Asa's Flute in the same talk (_08). No goal: told and paid at once.
quest QNO_WC_FERRY4_1
start level 25-
repeat 1
giver NPC_WC_FERRY4
npc NPC_WC_FERRY4
finish NPC_WC_FERRY4
talk NPC_WC_FERRY4 greeting @SN_NPC_WC_FERRY3_QS
talk NPC_WC_FERRY4 offer @SN_TALK_QNO_WC_FERRY4_1_01
talk NPC_WC_FERRY4 denied @SN_TALK_QNO_WC_FERRY4_1_07
talk NPC_WC_FERRY4 bag-full @SN_TALK_QNO_WC_FERRY4_1_02
talk NPC_WC_FERRY4 accepted @SN_TALK_QNO_WC_FERRY4_1_03
talk NPC_WC_FERRY4 accepted @SN_TALK_QNO_WC_FERRY4_1_04
talk NPC_WC_FERRY4 accepted @SN_TALK_QNO_WC_FERRY4_1_05
talk NPC_WC_FERRY4 accepted @SN_TALK_QNO_WC_FERRY4_1_06
talk NPC_WC_FERRY4 finished @SN_TALK_QNO_WC_FERRY4_1_08

# Egypt's gods of the temple (QNO_SD_MA_029..034): Senmute gives the Seal Crystal (_1) on accepting (no room: _05);
# whoever holds it in the god's temple room when the god falls has it become the god's heart (_2), the goal
# (QNO_UNIQUE_DEAD_MA_*: the region, the unique's codename, the crystal taken, the heart added); given up, the
# crystal is taken back (AbortQuest_SD_MA_*).

quest QNO_SD_MA_029
start level 107-
start after all QNO_SD_MA_028
repeat 1
giver NPC_SD_M_AREA_SENMUTE
npc NPC_SD_M_AREA_SENMUTE
finish NPC_SD_M_AREA_SENMUTE
start-item ITEM_QNO_SD_MA_029_1 1
goal drop 1 ITEM_QNO_SD_MA_029_2 from MOB_SD_SELKIS 0% at NPC_SD_M_AREA_SENMUTE text @SN_CON_QNO_SD_MA_029_1
on unique-dead MOB_SD_SELKIS region -32758 take ITEM_QNO_SD_MA_029_1 give ITEM_QNO_SD_MA_029_2
talk NPC_SD_M_AREA_SENMUTE greeting @SN_NPC_SD_M_AREA_SENMUTE_QS
talk NPC_SD_M_AREA_SENMUTE offer @SN_TALK_QNO_SD_MA_029_01
talk NPC_SD_M_AREA_SENMUTE accepted @SN_TALK_QNO_SD_MA_029_02
talk NPC_SD_M_AREA_SENMUTE denied @SN_TALK_QNO_SD_MA_029_03
talk NPC_SD_M_AREA_SENMUTE not-done @SN_TALK_QNO_SD_MA_029_04
talk NPC_SD_M_AREA_SENMUTE bag-full @SN_TALK_QNO_SD_MA_029_05
talk NPC_SD_M_AREA_SENMUTE done @SN_TALK_QNO_SD_MA_029_06
talk NPC_SD_M_AREA_SENMUTE done-now @SN_TALK_QNO_SD_MA_029_07

quest QNO_SD_MA_030
start level 107-
start after all QNO_SD_MA_029
repeat 1
giver NPC_SD_M_AREA_SENMUTE
npc NPC_SD_M_AREA_SENMUTE
finish NPC_SD_M_AREA_SENMUTE
start-item ITEM_QNO_SD_MA_030_1 1
goal drop 1 ITEM_QNO_SD_MA_030_2 from MOB_SD_NEITH 0% at NPC_SD_M_AREA_SENMUTE text @SN_CON_QNO_SD_MA_030_1
on unique-dead MOB_SD_NEITH region -32757 take ITEM_QNO_SD_MA_030_1 give ITEM_QNO_SD_MA_030_2
talk NPC_SD_M_AREA_SENMUTE greeting @SN_NPC_SD_M_AREA_SENMUTE_QS
talk NPC_SD_M_AREA_SENMUTE offer @SN_TALK_QNO_SD_MA_030_01
talk NPC_SD_M_AREA_SENMUTE accepted @SN_TALK_QNO_SD_MA_030_02
talk NPC_SD_M_AREA_SENMUTE denied @SN_TALK_QNO_SD_MA_030_03
talk NPC_SD_M_AREA_SENMUTE not-done @SN_TALK_QNO_SD_MA_030_04
talk NPC_SD_M_AREA_SENMUTE bag-full @SN_TALK_QNO_SD_MA_030_05
talk NPC_SD_M_AREA_SENMUTE done @SN_TALK_QNO_SD_MA_030_06
talk NPC_SD_M_AREA_SENMUTE done-now @SN_TALK_QNO_SD_MA_030_07

quest QNO_SD_MA_031
start level 107-
start after all QNO_SD_MA_030
repeat 1
giver NPC_SD_M_AREA_SENMUTE
npc NPC_SD_M_AREA_SENMUTE
finish NPC_SD_M_AREA_SENMUTE
start-item ITEM_QNO_SD_MA_031_1 1
goal drop 1 ITEM_QNO_SD_MA_031_2 from MOB_SD_ANUBIS 0% at NPC_SD_M_AREA_SENMUTE text @SN_CON_QNO_SD_MA_031_1
on unique-dead MOB_SD_ANUBIS region -32756 take ITEM_QNO_SD_MA_031_1 give ITEM_QNO_SD_MA_031_2
talk NPC_SD_M_AREA_SENMUTE greeting @SN_NPC_SD_M_AREA_SENMUTE_QS
talk NPC_SD_M_AREA_SENMUTE offer @SN_TALK_QNO_SD_MA_031_01
talk NPC_SD_M_AREA_SENMUTE accepted @SN_TALK_QNO_SD_MA_031_02
talk NPC_SD_M_AREA_SENMUTE denied @SN_TALK_QNO_SD_MA_031_03
talk NPC_SD_M_AREA_SENMUTE not-done @SN_TALK_QNO_SD_MA_031_04
talk NPC_SD_M_AREA_SENMUTE bag-full @SN_TALK_QNO_SD_MA_031_05
talk NPC_SD_M_AREA_SENMUTE done @SN_TALK_QNO_SD_MA_031_06
talk NPC_SD_M_AREA_SENMUTE done-now @SN_TALK_QNO_SD_MA_031_07

quest QNO_SD_MA_032
start level 107-
start after all QNO_SD_MA_031
repeat 1
giver NPC_SD_M_AREA_SENMUTE
npc NPC_SD_M_AREA_SENMUTE
finish NPC_SD_M_AREA_SENMUTE
start-item ITEM_QNO_SD_MA_032_1 1
goal drop 1 ITEM_QNO_SD_MA_032_2 from MOB_SD_ISIS 0% at NPC_SD_M_AREA_SENMUTE text @SN_CON_QNO_SD_MA_032_1
on unique-dead MOB_SD_ISIS region -32755 take ITEM_QNO_SD_MA_032_1 give ITEM_QNO_SD_MA_032_2
talk NPC_SD_M_AREA_SENMUTE greeting @SN_NPC_SD_M_AREA_SENMUTE_QS
talk NPC_SD_M_AREA_SENMUTE offer @SN_TALK_QNO_SD_MA_032_01
talk NPC_SD_M_AREA_SENMUTE accepted @SN_TALK_QNO_SD_MA_032_02
talk NPC_SD_M_AREA_SENMUTE denied @SN_TALK_QNO_SD_MA_032_03
talk NPC_SD_M_AREA_SENMUTE not-done @SN_TALK_QNO_SD_MA_032_04
talk NPC_SD_M_AREA_SENMUTE bag-full @SN_TALK_QNO_SD_MA_032_05
talk NPC_SD_M_AREA_SENMUTE done @SN_TALK_QNO_SD_MA_032_06
talk NPC_SD_M_AREA_SENMUTE done-now @SN_TALK_QNO_SD_MA_032_07

quest QNO_SD_MA_033
start level 107-
start after all QNO_SD_MA_032
repeat 1
giver NPC_SD_M_AREA_SENMUTE
npc NPC_SD_M_AREA_SENMUTE
finish NPC_SD_M_AREA_SENMUTE
start-item ITEM_QNO_SD_MA_033_1 1
goal drop 1 ITEM_QNO_SD_MA_033_2 from MOB_SD_HAROERIS 0% at NPC_SD_M_AREA_SENMUTE text @SN_CON_QNO_SD_MA_033_1
on unique-dead MOB_SD_HAROERIS region -32754 take ITEM_QNO_SD_MA_033_1 give ITEM_QNO_SD_MA_033_2
talk NPC_SD_M_AREA_SENMUTE greeting @SN_NPC_SD_M_AREA_SENMUTE_QS
talk NPC_SD_M_AREA_SENMUTE offer @SN_TALK_QNO_SD_MA_033_01
talk NPC_SD_M_AREA_SENMUTE accepted @SN_TALK_QNO_SD_MA_033_02
talk NPC_SD_M_AREA_SENMUTE denied @SN_TALK_QNO_SD_MA_033_03
talk NPC_SD_M_AREA_SENMUTE not-done @SN_TALK_QNO_SD_MA_033_04
talk NPC_SD_M_AREA_SENMUTE bag-full @SN_TALK_QNO_SD_MA_033_05
talk NPC_SD_M_AREA_SENMUTE done @SN_TALK_QNO_SD_MA_033_06
talk NPC_SD_M_AREA_SENMUTE done-now @SN_TALK_QNO_SD_MA_033_07

quest QNO_SD_MA_034
start level 108-
start after all QNO_SD_MA_033
repeat 1
giver NPC_SD_M_AREA_SENMUTE
npc NPC_SD_M_AREA_SENMUTE
finish NPC_SD_M_AREA_SENMUTE
start-item ITEM_QNO_SD_MA_034_1 1
goal drop 1 ITEM_QNO_SD_MA_034_2 from MOB_SD_SETH 0% at NPC_SD_M_AREA_SENMUTE text @SN_CON_QNO_SD_MA_034_1
on unique-dead MOB_SD_SETH region -32753 take ITEM_QNO_SD_MA_034_1 give ITEM_QNO_SD_MA_034_2
talk NPC_SD_M_AREA_SENMUTE greeting @SN_NPC_SD_M_AREA_SENMUTE_QS
talk NPC_SD_M_AREA_SENMUTE offer @SN_TALK_QNO_SD_MA_034_01
talk NPC_SD_M_AREA_SENMUTE accepted @SN_TALK_QNO_SD_MA_034_02
talk NPC_SD_M_AREA_SENMUTE denied @SN_TALK_QNO_SD_MA_034_03
talk NPC_SD_M_AREA_SENMUTE not-done @SN_TALK_QNO_SD_MA_034_04
talk NPC_SD_M_AREA_SENMUTE bag-full @SN_TALK_QNO_SD_MA_034_05
talk NPC_SD_M_AREA_SENMUTE done @SN_TALK_QNO_SD_MA_034_06
talk NPC_SD_M_AREA_SENMUTE done-now @SN_TALK_QNO_SD_MA_034_07

# The premium's quest (its UQL1 part): purification seeds from every monster for the
# premium's holder, exchanged at the potion merchants for skill points.
quest QSP_ALL_POTION_3
# Ours: named by its pay string, Purification Seed (Premium Quest); its NameString SN_QSP_ALL_POTION_3 has no words in
# 1.188's text tables.
name @SN_PAY_QSP_ALL_POTION_3
start level 1-
# Taken while the premium's package names it with uses left (the allowed less the used); ours: no limit of its own (the
# original's repeat limit of 3 would stop a second package's).
start premium
repeat any
giver NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION
npc NPC_CH_POTION NPC_WC_POTION NPC_KT_POTION NPC_EU_POTION NPC_CA_POTION
# A kill drops one at 5 % x the level scale (0.05 x 100 x the level gap's scale), up to 500.
goal drop 500 ITEM_QSP_ALL_POTION_3_01 from any 5% by-level text @SN_CON_QSP_ALL_POTION_3_01
# The five rows (the numbers _RefQuestRewardItems' kind 4: 1,200 .. 10,000 SP; the English rows' "300 .. 2500 skill
# points" are the client's other words for them).
tier ITEM_QSP_ALL_POTION_3_01 100 sp 1200 talk @SN_TALK_QSP_ALL_POTION_3_07
tier ITEM_QSP_ALL_POTION_3_01 200 sp 2800 talk @SN_TALK_QSP_ALL_POTION_3_08
tier ITEM_QSP_ALL_POTION_3_01 300 sp 4800 talk @SN_TALK_QSP_ALL_POTION_3_09
tier ITEM_QSP_ALL_POTION_3_01 400 sp 6400 talk @SN_TALK_QSP_ALL_POTION_3_10
tier ITEM_QSP_ALL_POTION_3_01 500 sp 10000 talk @SN_TALK_QSP_ALL_POTION_3_11
# Each merchant's eight lines: offer, accepted, denied, not-done, missing
# (short of the row picked), done, finished, done-now (500 held).
talk NPC_CH_POTION greeting @SN_NPC_CH_POTION_QS
talk NPC_CH_POTION offer @SN_TALK_QSP_ALL_POTION_3_01
talk NPC_CH_POTION accepted @SN_TALK_QSP_ALL_POTION_3_02
talk NPC_CH_POTION denied @SN_TALK_QSP_ALL_POTION_3_03
talk NPC_CH_POTION not-done @SN_TALK_QSP_ALL_POTION_3_04
talk NPC_CH_POTION missing @SN_TALK_QSP_ALL_POTION_3_05
talk NPC_CH_POTION done @SN_TALK_QSP_ALL_POTION_3_06
talk NPC_CH_POTION finished @SN_TALK_QSP_ALL_POTION_3_12
talk NPC_CH_POTION done-now @SN_TALK_QSP_ALL_POTION_3_13
talk NPC_WC_POTION greeting @SN_NPC_WC_POTION_QS
talk NPC_WC_POTION offer @SN_TALK_QSP_ALL_POTION_3_01
talk NPC_WC_POTION accepted @SN_TALK_QSP_ALL_POTION_3_02
talk NPC_WC_POTION denied @SN_TALK_QSP_ALL_POTION_3_03
talk NPC_WC_POTION not-done @SN_TALK_QSP_ALL_POTION_3_04
talk NPC_WC_POTION missing @SN_TALK_QSP_ALL_POTION_3_05
talk NPC_WC_POTION done @SN_TALK_QSP_ALL_POTION_3_06
talk NPC_WC_POTION finished @SN_TALK_QSP_ALL_POTION_3_12
talk NPC_WC_POTION done-now @SN_TALK_QSP_ALL_POTION_3_13
talk NPC_KT_POTION greeting @SN_NPC_KT_POTION_QS
talk NPC_KT_POTION offer @SN_TALK_QSP_ALL_POTION_3_14
talk NPC_KT_POTION accepted @SN_TALK_QSP_ALL_POTION_3_15
talk NPC_KT_POTION denied @SN_TALK_QSP_ALL_POTION_3_16
talk NPC_KT_POTION not-done @SN_TALK_QSP_ALL_POTION_3_17
talk NPC_KT_POTION missing @SN_TALK_QSP_ALL_POTION_3_18
talk NPC_KT_POTION done @SN_TALK_QSP_ALL_POTION_3_19
talk NPC_KT_POTION finished @SN_TALK_QSP_ALL_POTION_3_20
talk NPC_KT_POTION done-now @SN_TALK_QSP_ALL_POTION_3_13
talk NPC_EU_POTION greeting @SN_NPC_EU_POTION_QS
talk NPC_EU_POTION offer @SN_TALK_QSP_ALL_POTION_3_21
talk NPC_EU_POTION accepted @SN_TALK_QSP_ALL_POTION_3_22
talk NPC_EU_POTION denied @SN_TALK_QSP_ALL_POTION_3_23
talk NPC_EU_POTION not-done @SN_TALK_QSP_ALL_POTION_3_24
talk NPC_EU_POTION missing @SN_TALK_QSP_ALL_POTION_3_25
talk NPC_EU_POTION done @SN_TALK_QSP_ALL_POTION_3_26
talk NPC_EU_POTION finished @SN_TALK_QSP_ALL_POTION_3_26
talk NPC_EU_POTION done-now @SN_TALK_QSP_ALL_POTION_3_26
talk NPC_CA_POTION greeting @SN_NPC_CA_POTION_QS
talk NPC_CA_POTION offer @SN_TALK_QSP_ALL_POTION_3_21
talk NPC_CA_POTION accepted @SN_TALK_QSP_ALL_POTION_3_22
talk NPC_CA_POTION denied @SN_TALK_QSP_ALL_POTION_3_23
talk NPC_CA_POTION not-done @SN_TALK_QSP_ALL_POTION_3_24
talk NPC_CA_POTION missing @SN_TALK_QSP_ALL_POTION_3_25
talk NPC_CA_POTION done @SN_TALK_QSP_ALL_POTION_3_26
talk NPC_CA_POTION finished @SN_TALK_QSP_ALL_POTION_3_27
talk NPC_CA_POTION done-now @SN_TALK_QSP_ALL_POTION_3_28
)";

} // namespace

bool DecodeCodedQuests(std::vector<game::QuestDef>& out, std::string& error) {
    // Each family's block (this file's, then those of their own files: OriginalQuestsAsia.cpp, OriginalQuestsEurope.cpp);
    // one quest from each `quest` line to the next.
    for (const std::string_view block : {kCoded, CodedQuestsAsia(), CodedQuestsEurope(), CodedQuestsTowns()}) {
        std::size_t at = block.find("\nquest ");
        while (at != std::string_view::npos) {
            const std::size_t next = block.find("\nquest ", at + 1);
            const std::string_view text = block.substr(at + 1, next == std::string_view::npos ? std::string_view::npos : next - at - 1);
            game::QuestDef q;
            if (!game::ParseQuest(text, q, error)) {
                error = "the original's coded quests: " + error;
                return false;
            }
            out.push_back(std::move(q));
            at = next;
        }
    }
    return true;
}

} // namespace sr::server
