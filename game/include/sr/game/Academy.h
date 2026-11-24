#pragma once

// The Academy's rules (the original's "training camp"): the member classes by level, the counts,
// the founding's price, the waits and the refusals' codes, shared by the server and the client.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace sr::game {

namespace academy {

// A member's class (_TrainingCampMember.MemberClass): the guardian who founded it, a guardian assistant, an apprentice.
enum class Class : std::uint8_t { Guardian = 0, Assistant = 1, Apprentice = 2 };

inline constexpr int kFoundLevel = 60;                // (cmp al, 0x3C)
inline constexpr std::uint64_t kFoundGold = 100'000;  //, the fee
inline constexpr int kApprenticeBelow = 40;            // _TrainingCamp_FnAddMember: the highest level reached below 40
inline constexpr int kAssistantBelow = 60;             // below 60 an assistant; else a guardian
inline constexpr int kMostAssistants = 2, kMostApprentices = 5;
inline constexpr int kApprenticeGraduates = 40, kAssistantGraduates = 60;
inline constexpr int kForcedApprentice = 41, kForcedAssistant = 61;       // (< 0x29, < 0x3D)

// The waits (the client's texts: 1.188's UIIT_MSG_TC_ERROR_*), each a `_TimedJob` of category 2: 7 founding again
// after a disband, 8 recruiting after a ban, joining again after leaving 9 an
// apprentice's, 10 an assistant's (picks by the leaver's class). Their lengths (the penalty keeper,
// its table by job): 7 a week, 8 and 9 a day, 10 three days.
inline constexpr std::int64_t kTimedCategory = 2;
inline constexpr std::int64_t kWaitFound = 7, kWaitRecruit = 8, kWaitRejoinApprentice = 9, kWaitRejoinAssistant = 10;
inline constexpr std::int64_t kWeek = 7 * 24 * 3600, kDay = 24 * 3600;
inline constexpr std::int64_t kRejoinAssistantSeconds = 3 * kDay, kRejoinApprenticeSeconds = kDay; // "72 hours ... 24 hours"
inline constexpr std::int64_t kInviteSeconds = 30; // ours: an invitation's answer (UIIT_MSG_TC_JOIN_NOREPLY; the original's not read)

// The refusals (the original's codes; the client's group 0x1D words them).
namespace code {
inline constexpr std::uint16_t kInvalidTarget = 3, kDead = 0x6807, kLevel = 0x6809, kGold = 0x680A, kNoAcademy = 0x680B,
                               kAlreadyMember = 0x680F, kAssistantsFull = 0x6810, kRefused = 0x6811, kOtherAcademy = 0x6812,
                               kFailed = 0x6813, kNotMember = 0x6815, kNotGuardian = 0x6816, kGraduateLevel = 0x681A,
                               kFoundWait = 0x681C, kDisbandWait = 0x681D, kRejoinWait = 0x681E, kRecruitWait = 0x681F,
                               kBanWait = 0x6820, kLeaveWait = 0x6821, kForcedLevel = 0x6822, kJobChange = 0x6823,
                               kNotPosition = 0x6824, kApprenticesFull = 0x6828, kBusy = 0x6806, kWrongTarget = 5,
                               kNoInformation = 0x6826, kBanWhileListed = 0x6829, kJoinKind = 0x682A, kNoGuild = 0x6817,
                               kNoText = 0x6817; // UIIT_MSG_GUILD_COMMON_NOTEXIST: a notice's title or text empty or too long
} // namespace code

// A character's class in an academy by the highest level it reached (_TrainingCamp_FnAddMember).
inline Class ClassOfLevel(int maxLevel) {
    return maxLevel < kApprenticeBelow ? Class::Apprentice : maxLevel < kAssistantBelow ? Class::Assistant : Class::Guardian;
}

// A member's grade, the title the client shows (1 Primary apprentice, 2 Apprentice, 3 Intermediate
// apprentice, 4 Advanced apprentice, 5 Primary guardian, 6 Intermediate guardian, 7 Guardian), by its class and highest
// level as the 1.188 client works it out: an apprentice up to 10, 20, 30, then advanced; an assistant from 40
// below 50, then intermediate; the guardian from 60.
inline std::uint8_t GradeOf(Class c, int maxLevel) {
    if (c == Class::Guardian) return 7;
    if (c == Class::Assistant) return maxLevel < 50 ? 5 : 6;
    return maxLevel <= 10 ? 1 : maxLevel <= 20 ? 2 : maxLevel <= 30 ? 3 : 4;
}

// The text key of a refusal (the original's table), empty when it has none.
const char* RefusalKey(std::uint16_t code);

// --- Graduation, honour, the ranking ---

// An apprentice's rating of its guardian, the judge window's choice 0..4 (Very dissatisfied .. Very satisfied),
// as the academy's evaluation points (the game server's table); 5 or more is refused.
inline constexpr int kRatingPoints[5] = {-3, -1, 1, 3, 5};
// A self-graduating apprentice's experience (ApplyExpDelta 2,000,000, times the server's experience rate);
// an assistant and a forced graduation get none.
inline constexpr std::uint64_t kGraduateExperience = 2'000'000;
// _TRAINING_CAMP_MEMBER_GRADUATE: an apprentice who joined at 30 or below gives the guardian 40 - its joined level, each
// assistant a tenth of that, the academy a graduate and the rating's points; above 30 a graduate only.
inline constexpr int kHonorJoinedAtMost = 30;
inline int GuardianHonorFor(int joinedLevel) { return joinedLevel <= kHonorJoinedAtMost ? kApprenticeGraduates - joinedLevel : 0; }
// The daily honour ranking (_TRAINING_CAMP_UPDATEHONORRANK): the 50 best by evaluation (at least 10), places 1-5 rank 1,
// 6-15 rank 2, 16-30 rank 3, 31-50 rank 4; 5 none.
inline constexpr int kRankedAcademies = 50, kRankMinimumEvaluation = 10, kNoRank = 5;
inline std::uint8_t RankOfPlace(int place) { return place <= 5 ? 1 : place <= 15 ? 2 : place <= 30 ? 3 : place <= 50 ? 4 : 5; }
// The guardian's honour buffs (rank 1 crown, 2 gold, 3 silver, 4 copper; worn
// cumulatively, rank r every buff from 4 down to r).
inline constexpr const char* kHonorBuffs[4] = {"SKILL_TC_CROWN_BUFF_01", "SKILL_TC_GOLD_BUFF_01", "SKILL_TC_SILVER_BUFF_01", "SKILL_TC_COPPER_BUFF_01"};

// --- Matching ---

// Whom a listed academy takes (the register box's choice).
enum class MatchKind : std::uint8_t { Apprentices = 1, Assistants = 2 };
inline constexpr int kMatchRows = 12;  // the window's slots (ifmentormatch.txt 70..81)
inline constexpr int kRankingRows = 10; // the ranking window's slots (ifapprenticeshiphonorranking.txt 30..39)
// The notice's lengths in bytes, each at least 1 (the original's server: _TrainingCamp's CommentTitle and Comment columns).
inline constexpr std::size_t kNoticeTitleMax = 128, kNoticeTextMax = 2048, kMatchTitleMax = 60;

// --- The experience buffs ---

// A member's buff slots (_TrainingCampBuffStatus.BuffType): slot 0 an apprentice's accumulation (idle, running, its time
// over), the others a received buff's place (empty, held). The guardian has slots 1..5, an apprentice slot 1, an
// assistant none (_TrainingCamp_FnAddMember).
enum class BuffType : std::uint8_t { Idle = 0, Accumulating = 1, Over = 2, Empty = 3, Received = 4 };
// Both last a day from their start (0x5A0 minutes); the minutes left are told at every tenth and
// each of the last ten.
inline constexpr int kBuffMinutes = 1440;
inline bool TellMinutes(int left) { return left % 10 == 0 || left <= 10; }
// The accumulation may be passed an hour after it began (the client's board, 3,600,000 ms; the server,
// 60 minutes).
inline constexpr int kPassAfterMinutes = 60;
// An apprentice's kill: the kill's experience over the monster's whole
// (the share and the level gaps of a kill's experience) x 100 x 0.1, whole points, at most 30; none from level
// 40 nor for an assistant.
inline constexpr int kKillPointsMost = 30, kAccumulateBelowLevel = 40;
inline int KillPoints(double ofWhole) { return std::min(kKillPointsMost, int(std::max(0.0, ofWhole * 100.0 * 0.1))); }
// A received buff spends 10 points a kill for that many percent more experience and skill experience, at
// most 30 % of the kill's own (0.3).
inline constexpr int kSpendPerKill = 10;
inline constexpr double kBonusMostOfKill = 0.30000001192092896;
// The pass: the guardian a buff of 0.2 of the points, the apprentice one of 0.8; whole points (truncated).
inline constexpr double kGuardianShare = 0.20000000298023224, kApprenticeShare = 0.800000011920929;
inline std::uint32_t ShareOf(std::uint32_t points, double share) { return std::uint32_t(double(float(points)) * share); }

} // namespace academy

} // namespace sr::game
