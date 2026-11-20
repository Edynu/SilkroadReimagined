#pragma once

// The quests' and events' items used from the bag: what takes each one in the original's game server, shared by our
// server and the item catalogue. Headless.
//
// The original sends a quest item (TID3 9) or a TID3 15 item, in a world that is no fortress's (else 0x18F3), to first
// the player's quests (each quest's own use of its items), then the events registered at the start; the first that
// takes it uses it up; none: refused 2. The only other way, the flag event (the reference's flag 2), is So-Ok's card's
// alone (names ITEM_ETC_E070919_TICKET).
//
// - The coded quests' uses: one quest each, taking only its own items. Each is a rule of our quest format
//   (QuestDef::UseRule, `on use`: the ground searches, the spawns, the traps' captures, the quest instances, the seven
//   stages, the witch's roll, the amulet's coin), the original's numbers in OriginalUseRules, joining its quest's
//   definition at the load. Not built: IVY_3's punishers and the ORNAMENT_4 / STONEBOARD_4 worlds' generals.
//   OriginalUseRules also holds three `trap` rules (QNO_CA_GORIA_6, QNO_EU_GENERAL_1, QNO_EU_IVY_1): their items are
//   skills' items (3/3/13/1) set from the bag as traps, no quest items; the trap's catch is the quest's event 0x1D, taken by
//   the rule (server Quests::Trapped).
// - The two events always registered: the guild support's party summon scroll and the
//   Pandora box (SummonEvent*).
// - Every other quest or event item: no handler at all in 1.188 (the E050729 summon scrolls, the E060517 party summon
//   scroll, the rice dumpling soup, the Halloween bombs, the E100428 ticket, the E100222 rental box and scroll; the
//   events that would name them are never made): refused 2 always.

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "sr/game/Quest.h"

namespace sr::game {

constexpr std::uint16_t kQuestItemInFortress = 0x18F3; // used in a fortress's world

enum class QuestItemTaker : std::uint8_t {
    None,         // no handler in 1.188: refused 2
    Quest,        // a coded quest's handler: taken only for a player holding that quest
    GuildSupport, // QEV_GUILD_SUPPORT (11000): the party summon scroll
    Pandora,      // QEV_PANDORA_BOX_100LV (10100): the Pandora box
};
struct QuestItemUse {
    QuestItemTaker taker = QuestItemTaker::None;
    std::string_view quests;  // the coded quests whose handler takes it (space-separated)
    std::string_view kind;     // search, capture, spawn, instance, coin, stages, roll, event
    std::string_view what;     // what the handler does, in a line
    bool ruled = false;        // its handler is an OriginalUseRules rule (built; waits only for its quest)
};
QuestItemUse QuestItemUseOf(std::string_view codename);

// The original's ground searches as our rules (`on use` lines), by the coded quest they belong to:
// added to that quest's definition when it has none for the item (Quests::Load; the quests are not built yet).
struct OriginalUseRule {
    std::string_view quest;
    std::string_view line;
};
const std::vector<OriginalUseRule>& OriginalUseRules();
// The quest's rules from OriginalUseRules that it does not have yet (by item), appended; how many.
int AddOriginalUseRules(QuestDef& q);

// The quest service's spawn about the player (QuestDef::UseRule::Spawn): a radius R drawn between `near` and `far`
// (the handlers' 20 + 80 x rand/32767),
// then a distance in it banded by rand() % 101 (under 70: 2R/3 to R; 70..89: R/3 to 2R/3; else 0 to R/3) at a bearing
// over the whole turn. The offset from the player.
struct SpawnOffset {
    float dx = 0, dz = 0;
};
SpawnOffset QuestSpawnOffset(float near, float far, std::mt19937& random);
// A capture's chance in 101 (QuestDef::UseRule::Capture): `roll`, plus `step` a level the player stands above the COS
// (`below`: less `step` a level the COS stands above the player), as the classes' handlers compute it.
int CaptureChance(int roll, int step, bool below, int playerLevel, int cosLevel);

// A quest instance world's _RefGame_World ID (INS_QNO_*: Type 1, 100 instances, 8 players; 0: not one of them).
std::uint32_t QuestWorldId(std::string_view codename);

// The Pandora box's skill after each wave (cast with no target). Its reference: att kind 5, no damage (Param 'att'
// 5 0 0 0 100), 'mc' 2; MOB_EVENT_PANDORA's
// DefaultSkill_1, level 1, no tactics row (it never starts a fight itself).
constexpr std::string_view kPandoraCast = "MSKILL_EVENT_PANDORA_ATTACK01";

// --- The two summon events ---------------------------------------------------------------------------------
constexpr std::uint32_t kGuildSupportEvent = 11000; // 0x2AF8, QEV_GUILD_SUPPORT (its setup)
constexpr std::uint32_t kPandoraEvent = 10100;      // 0x2774, QEV_PANDORA_BOX_100LV (its setup)
constexpr std::string_view kGuildSupportItem = "ITEM_ETC_E060526_SUMMON_PARTY_SCROLL_A";
constexpr std::string_view kPandoraItem = "ITEM_ETC_E060517_MON_GENERATION_BOX";
constexpr std::string_view kPandoraBox = "MOB_EVENT_PANDORA";
// The handler's checks in its order: not in a town (the region's _RefRegion IsBattleField 0): the box's or the scroll's
// words; the level under 30: UIIT_MSG_PANDORA_USE_ERROR; in the Battle Arena: UIIT_MSG_ARENA_ERR_PANDORA_USE; under 10
// (any of its events): SN_TALK_QEV_CH_EVENT_0729_02. Each refusal returns 0 (the use then refused 2).
constexpr int kSummonEventLevel = 30;
constexpr int kSummonLowLevel = 10;
constexpr std::string_view kPandoraInTown = "SN_TALK_QEV_CH_EVENT_KISAENG_060517_N";   // "You can't use the box of Pandora."
constexpr std::string_view kSummonInTown = "SN_TALK_QEV_CH_EVENT_0818_01";             // "...Summon scrolls within town."
constexpr std::string_view kSummonLevelLow = "UIIT_MSG_PANDORA_USE_ERROR";             // "...over Lv. 30."
constexpr std::string_view kSummonInArena = "UIIT_MSG_ARENA_ERR_PANDORA_USE";
constexpr std::string_view kSummonUnderTen = "SN_TALK_QEV_CH_EVENT_0729_02";
constexpr std::string_view kSummonOne = "SN_TALK_QEV_CH_EVENT_0729_03";                // no party: one monster
constexpr std::string_view kSummonParty = "SN_TALK_QEV_CH_EVENT_KISAENG_060517_L";     // as many as the party's members
constexpr std::string_view kPandoraOpened = "SN_TALK_QEV_CH_EVENT_KISAENG_060517_M";
// The guild support's monster: one of 81 by the user's level, 30..110 (above 110 its last, the
// pick); empty under 30. Each comes up 20..100 from the user at a random bearing.
std::string_view GuildSupportMonster(int level);
constexpr float kSummonNear = 20.0f, kSummonFar = 100.0f;
// The Pandora's ten: the setup's table by level (21..110, 91 rows, the second at 28 not kept),
// from the first at or above min(level, 110) down to ten rows, the lowest repeated when fewer. Empty when
// none (the original then spawns nothing).
std::vector<std::string_view> PandoraMonsters(int level);
// Its spawner: every 5 s, 240 times (20 minutes); while none of its monsters stands, 4 come up, each a random one of
// the ten, 50 from the box at a random bearing; they are grade 7: gone with the box (its death), at the spawner's end,
// or when its user is gone.
constexpr int kPandoraPeriodSeconds = 5;
constexpr int kPandoraRuns = 240;
constexpr int kPandoraWave = 4;
constexpr float kPandoraRing = 50.0f;
// Tab_RefTactics of every MOB_EVE_STRONG_*: btAggressType 1 (they fight back, none starts a fight), nSightRange 130.
constexpr float kSummonSight = 130.0f;

} // namespace sr::game
