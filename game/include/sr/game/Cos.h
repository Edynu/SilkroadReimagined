#pragma once

// COS: the characters a player summons from an item: rides, transports and pets. The
// original's own word is COS (NPC_COS, _CharCOS); the kind is the characterdata row's
// TID4 under TID 1/2/3 (measured on the 1.188 data, _RefObjCommon):
//
//   1 a ride      COS_C_* (horses, camels, the item mall's tiger, ostrich, ...), boarded when summoned
//   2 a transport COS_T_* (the job's trade horses, camels, buffalo, ...), boarded when summoned, left and boarded again through Ride
//   3 an attack pet (growth pet) COS_P_* with 140 rows each: one per level
//   4 a grab pet (ability pet) COS_P_*
//   5 a guild soldier (the fortress war's)
//
// Rides and transports come from a scroll (itemdata TID 3/3/3/2, used up): Desc1 names the COS; a Desc2 such as
// "5,10,20,30,45" is a level ladder, the COS then being Desc1 + "_" + the highest of those levels the summoner has
// reached (COS_C_OSTRICH_45). Pets come from a summoning item that stays in the bag (TID 3/2/1/x: x 1 an attack pet,
// 2 a grab pet): Desc1 names the pet (an attack pet's level-1 row, COS_P_WOLF_001; a grab pet's only row), a grab
// pet's Param1 its rental in minutes, counted from its first summon (UIIT_STT_COSNEWUI_ITEM_BEFORESUM). The pet itself
// is a _CharCOS row the item's _Items.Data names (made at its first summon, as the original's _AddNewCOS does).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sr::assets {
struct ItemRow;
struct ObjectRow;
struct SkillRow;
} // namespace sr::assets

namespace sr::game {

enum class CosKind : std::uint8_t { None = 0, Ride = 1, Transport = 2, AttackPet = 3, GrabPet = 4, Soldier = 5 };

// A characterdata row's COS kind (None: not a COS).
CosKind CosKindOf(const assets::ObjectRow& row);

// A ride's or a transport's scroll (itemdata TID 3/3/3/2).
bool IsCosScroll(const assets::ItemRow& row);
// A stable keeper (the NPC function 0xB: its store buys a transport back), by a part of its codename as the original's
// server gives it: NPC_CH_HORSE, NPC_WC_HORSE, NPC_KT_HORSE, NPC_EU_HORSE, NPC_CA_HORSE, NPC_SD_M_AREA_HORSE.
bool IsStableNpc(std::string_view codename);

// The codename of the COS a scroll summons for a summoner of `level` (none: not a scroll, or the summoner is below
// its ladder's first level).
std::optional<std::string> CosOfScroll(const assets::ItemRow& row, int level);

// A pet's summoning item (TID 3/2/1/1 an attack pet, 3/2/1/2 a grab pet): the pet's kind (None: not one).
CosKind PetOfItem(const assets::ItemRow& row);
// The pet a summoning item makes at its first summon (its Desc1), and a grab pet's rental in minutes (0: none).
std::string PetCodename(const assets::ItemRow& row);
int PetRentalMinutes(const assets::ItemRow& row);

// The items used on a pet's summoning item (dropped onto it in the bag; the 1.188 client's target slot 0x46): the Clock
// of Reincarnation (ITEM_COS_P_EXTENSION*, TID 3/3/13/12) gives an expired grab pet Param1
// minutes more from the moment of use ("extends the expired Silk pet's rental period by 4 weeks ... applied in the
// instant of use"); the Grass of Life (ITEM_COS_P_REVIVAL*, TID 3/3/1/6) brings back a dead attack pet ("to resurrect
// a Growth pet in the state of unable-to-summon").
enum class PetTreatment : std::uint8_t { None, Revival, Extension };
PetTreatment PetTreatmentOf(const assets::ItemRow& row);

// The mall's items used on a pet (TID 3/3/13/15: the watches, the pet skills, the potions of growth). The use names the
// pet's summoning item; the item's Param2 text lists the jobs run
// on that pet, each a timed job of the pet's (the original's _TimedJobForPet) ending on the wall clock:
// - [PSU1:SKILL,flag]: the skill as a buff on the pet (category 0, the skill keeper's); flag 1 takes the row of the
//   pet's level, SKILL_NNN ("%s_%03d" with the pet's level);
// - [BCS1:INC_INVENTORY_SIZE,A,seconds,N,...]: the grab pet's bag N places more (category 5, the composite item
//   keeper's: on, off).
// Param6's bits say which pet takes it: 1 an attack pet (COS 1/2/3/3), 2 a grab pet (COS 1/2/3/4). An attack pet's job
// lasts its
// skill's time; a grab pet's as many minutes as the used stack holds, at most its rental's minutes left, and the use
// takes that many of the stack: a watch's stack is its minutes (4,320 for three days).
struct PetItemJobs {
    std::string skill;        // PSU1's skill (empty: none)
    bool skillByLevel = false; // its flag 1
    int bagPlaces = 0;        // BCS1 INC_INVENTORY_SIZE's places (0: none)
    bool attackPet = false, grabPet = false;
    explicit operator bool() const { return !skill.empty() || bagPlaces > 0; }
};
bool IsPetMallItem(const assets::ItemRow& row); // TID 3/3/13/15
PetItemJobs PetItemJobsOf(const assets::ItemRow& row);
// The skill's codename for a pet of `petLevel` (the level's row when the job's flag asks for it).
std::string PetSkillCodename(const PetItemJobs& jobs, int petLevel);
// A grab pet's own bag (28 places of the row's InventorySize, 140, allocated); the places past it hold what they held
// when a watch's places went away.
constexpr int kGrabPetBag = 28;
// A watch wants more than this many minutes of the grab pet's rental left (refused at 2 or less).
constexpr int kPetWatchLeastMinutes = 2;
// A grab pet's job's minutes: the stack's count, at most the rental's whole minutes left (CTimeSpan's total minutes).
int PetWatchMinutes(int count, std::int64_t rentSecondsLeft);
// The pet's timed jobs as the original writes them: category 0 a skill (JobID the skill, Data2 1: TimeToKeep is the
// end), category 5 a composite item's (JobID the item, Data1 'BCS1', Data2 the kind, 1 INC_INVENTORY_SIZE, Data3 the
// places); TimeToKeep the end in Unix seconds, Serial64 the item's.
constexpr std::uint8_t kPetJobSkill = 0, kPetJobItem = 5;
constexpr std::int32_t kPetJobBagTag = 0x42435331; // 'BCS1'
constexpr std::int32_t kPetJobBagKind = 1;         // INC_INVENTORY_SIZE

// _CharCOS.State: 1 inactive (not out), 2 summoned (out; kept so through a move to
// another game server, which summons it again), 3 active, 4 dead.
enum class CosState : std::uint8_t { Inactive = 1, Summoned = 2, Active = 3, Dead = 4 };
// An attack pet's hunger points: full (the client's default).
constexpr int kCosHgpFull = 10000;

// The rules of the original we keep (the 1.188 client's texts name them):
// - none in a dungeon (UIIT_MSG_COS_CREATE_DUNGEON);
// - none within 20 s of a fight (UIIT_MSG_COS_CAN_NOT_CREATE_BATTLE), nor boarding (UIIT_MSG_COS_CAN_NOT_RIDE_BATTLE);
// - one of a kind at a time (UIIT_MSG_COS_CANT_SUMMON_DUPLICATE_PETTYPE): rides and transports count as one kind.
constexpr int kCosAfterFightSeconds = 20;
// How near its owner an unridden COS keeps (ours: the original's follow distance is not traced): farther than
// kCosFollowFar it walks back to kCosFollowNear behind it.
constexpr float kCosFollowFar = 60.0f, kCosFollowNear = 25.0f;

// A transport's owner walks no farther than this from it (world units: 100 m of the original's text, a region
// being 192 m) (UIIT_MSG_COSERR_TOO_FAR_FROM_TRADECART "A character cannot move if he is %d m away from his transport").
constexpr float kCosLeash = 1000.0f;
constexpr int kCosLeashMetres = 100;
// The jobs' suits (itemdata TID 3/1/7/x, x the job; the original's job state numbers them the same).
enum class Job : std::uint8_t { None = 0, Trader = 1, Thief = 2, Hunter = 3 };

// An attack pet's numbers on its window's Basic Info page, as the 1.188 client works them out itself (the
// server sends none): the attack ranges from the first physical and the first magical attack among its characterdata
// row's skills (cols 83..92; ours: a skill whose 'att' kind is 10 is the magical one), the defences, hit rate and parry
// from the row (cols 71, 72, 77, 75: the client's object keeps them in that column order). A hungry pet, its HGP at
// kCosHungry or below, fights at half (each number halved) and its window shows them grey.
constexpr int kCosHungry = 3000;
struct PetStats {
    int phyMin = 0, phyMax = 0, magMin = 0, magMax = 0;
    int phyDefence = 0, magDefence = 0, hitRate = 0, parry = 0;
    bool hungry = false;
};
// `skills`: the row's skills as found (nullptr where a skill is not in the table), in the row's order.
PetStats PetStatsOf(const assets::ObjectRow& row, std::span<const assets::SkillRow* const> skills, int hgp);

// An attack pet's experience and levels, gained on top of what it has; at or above its owner's level, or at
// level 110, it fills only to its level's need (the level table's, the characters'); below, it rises a level each time
// its experience covers the need, its experience less the need, up to 140 and no higher than its owner's level. Each
// level is a row of its own (COS_P_WOLF_001 .. _140: PetCodenameAtLevel).
constexpr int kAttackPetGrowthLimit = 110, kAttackPetMaxLevel = 140;
struct PetGrowth {
    int level = 1;
    std::uint64_t experience = 0;
    int levelsGained = 0;
};
// `expToNext[level]`: what that level needs to the next (0 or past the table: no further).
PetGrowth GrowPet(int level, std::uint64_t experience, std::uint64_t gained, int ownerLevel, std::span<const std::uint64_t> expToNext);
// The row of the same attack pet at `level` (its codename's last _NNN replaced; empty: not a level's codename).
std::string PetCodenameAtLevel(std::string_view codename, int level);

// An attack pet's hunger: out with its owner, it eats
// 100 / (Except_4 x 60) HGP a second (ObjectRow::except4), the fractions kept from one
// tick to the next; at each tenth of its HGP (10 %, 20 %, ...) it is saved and its owner told; at kCosHungry or below
// its numbers are halved (UIIT_MSG_COSPETERR_HGP_30LOW). At none it
// stays at none (ours: what the original does then is not known yet).
double PetHgpPerSecond(const assets::ObjectRow& row);
// The HGP recovery potion (ITEM_COS_P_HGP_POTION_*, TID 3/3/1/9): on its owner's
// attack pet, Param1 percent of the full HGP more, to the full at most; refused at full (error 0x18B3,
// UIIT_MSG_COSPETERR_HGPFULL_NODRINK).
bool IsPetHgpPotion(const assets::ItemRow& row);
int PetHgpPotionAmount(const assets::ItemRow& row);

// The pets' HP recovery kit (ITEM_ETC_COS_HP_POTION_*, TID 3/3/1/4): on a COS of
// its owner's, alive, the amount a potion gives its owner (game::RecoveryOf: Param1 x 1.02^(the owner's level - 1) x
// (1 + its STR / 416), reading the owner), given at once
// (not over steps); its lock 1.1 s (pushed at ReuseLockOf).
bool IsPetHpPotion(const assets::ItemRow& row);

// What an attack pet's death takes from its experience: with `levelExperience` its level's experience to the next
// (leveldata Exp_C) and `monsterExperience` the level's monster experience (GUST_Mob_Exp), the share is the experience
// x 2 % (doubles), cut to a whole number and halved; the
// cap the monster experience x 100; the loss the least of the two, none when the half is under 1. Killed by
// a player (`byPlayer`): 0.4 % and x 20; its owner a murderer (`murderer`): 6 % and x 300, or by a player 2 % and
// x 100. No level is too low (unlike a player's, which starts above 10);
// a kill of the relations 2 and 6 (a team's, a free PvP's) and a death with no killer
// take nothing (the callers' to say).
std::uint64_t PetDeathPenalty(std::uint64_t levelExperience, std::uint32_t monsterExperience, bool byPlayer = false, bool murderer = false);

// Whether two kinds take the same place (one of each place at a time).
inline int CosPlaceOf(CosKind kind) { return kind == CosKind::Ride || kind == CosKind::Transport ? 1 : int(kind); }

} // namespace sr::game
