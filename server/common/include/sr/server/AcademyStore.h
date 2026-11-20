#pragma once

// The Academy in the original's tables: `_TrainingCamp`, `_TrainingCampMember`,
// `_TrainingCampBuffStatus`, `_TrainingCampSubMentorHonorPoint`, the waits in `_TimedJob` category 2 (7..10). Our own
// queries, the rules of the original's procedures (_TRAINING_CAMP_CREATE, _TrainingCamp_FnAddMember,
// _TRAINING_CAMP_DELMEMBER, _TRAINING_CAMP_DELCAMP), each in one transaction.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sr::server {

class Database;

struct AcademyMemberRow {
    std::int64_t character = 0;
    std::uint32_t model = 0;
    std::string name;
    std::int64_t joined = 0; // Unix seconds
    std::uint8_t memberClass = 0;
    int joinedLevel = 0, level = 0, maxLevel = 0;
    std::int64_t honor = 0;
};

struct AcademyRow {
    std::uint32_t id = 0;
    std::int64_t founded = 0; // Unix seconds
    std::uint8_t rank = 5;
    std::int64_t graduates = 0, evaluation = 0;
    std::string noticeTitle, notice;
    std::vector<AcademyMemberRow> members;
    // Its entry on the matching list (0: none).
    std::uint32_t matchEntry = 0;
    std::uint8_t matchKind = 0;
    std::string matchTitle;
};

// A member's experience buff slot (`_TrainingCampBuffStatus`): `since` the seconds since its
// StartingTime (-1: the original's never, '2000-01-01'), `type` game::academy::BuffType.
struct AcademyBuffRow {
    int slot = 0;
    std::int64_t donor = 0;
    std::int64_t since = -1;
    std::int64_t points = 0;
    std::uint8_t type = 0;
};
std::vector<AcademyBuffRow> LoadAcademyBuffs(Database& db, std::int64_t character, bool& ok);
// One slot written as it stands (`since` as above).
bool SaveAcademyBuff(Database& db, std::int64_t character, const AcademyBuffRow& row);
// A received buff for a member playing nowhere (the pass's other half): the slot its donor holds, else an empty one, held
// from now with `points`. False when it has none free.
bool ReceiveAcademyBuff(Database& db, std::int64_t character, std::int64_t donor, std::int64_t points);
// The slots a donor gave the others of its academy cleared (it left).
void ClearAcademyBuffsFrom(Database& db, std::int64_t donor);

// An academy with its members; nothing when there is none. `ok` false when the query failed.
std::optional<AcademyRow> LoadAcademy(Database& db, std::uint32_t academy, bool& ok);
// The academy a character belongs to (0: none).
std::uint32_t AcademyOf(Database& db, std::int64_t character, bool& ok);

// What a membership change gives back: the new academy (founding), or the original's refusal (the procedures' negative
// returns as named here; the caller turns them into the client's codes).
enum class AcademyWrite : std::int8_t {
    Ok = 0,
    AlreadyMember,  // -1001
    NoAcademy,      // -1000 / -2000
    GuardianTaken,  // -14: a second guardian
    AssistantsFull, // -11
    ApprenticesFull, // -12
    NotMember,      // -2001 (or the guardian, who never leaves)
    Failed,
};
// Founds an academy for `character` (its class by its highest level: the guardian); its kept honour points come back.
// `level` and `maxLevel` the game server's (the class by the highest level reached; `_char` may not have it yet).
AcademyWrite FoundAcademy(Database& db, std::int64_t character, int level, int maxLevel, std::uint32_t& academy, std::string& error);
AcademyWrite AddAcademyMember(Database& db, std::uint32_t academy, std::int64_t character, int level, int maxLevel, std::string& error);
// A member out (not the guardian): `ban` keeps an assistant's points whole, a leave takes 10 of them.
AcademyWrite RemoveAcademyMember(Database& db, std::uint32_t academy, std::int64_t character, bool ban, std::string& error);
// The guardian's academy gone; its assistants' points kept.
AcademyWrite DisbandAcademy(Database& db, std::uint32_t academy, std::int64_t guardian, std::string& error);
// A member's levels written (_TRAINING_CAMP_REFRESHMEMBERSTATUS targets 2 and 6).
void SaveAcademyLevels(Database& db, std::int64_t character, int level, int maxLevel);

// A member graduates (_TRAINING_CAMP_MEMBER_GRADUATE's rules): an assistant's points kept, an apprentice's honour to the
// guardian and assistants and `evaluation` to the academy (kept at 0 or more) when it joined at 30 or below,
// the graduate counted; its class comes back.
AcademyWrite GraduateAcademyMember(Database& db, std::uint32_t academy, std::int64_t character, int evaluation, std::uint8_t& memberClass,
                                   std::string& error);
bool SaveAcademyNotice(Database& db, std::uint32_t academy, const std::string& title, const std::string& text);

// The daily honour ranking (_TRAINING_CAMP_UPDATEHONORRANK's rules), when today's has not run yet (`force`: again); true
// when it ran (the first game server of the shard to ask does it).
bool UpdateAcademyHonorRanking(Database& db, bool force, std::string& error);
struct AcademyRankEntry {
    int place = 0;
    std::uint8_t rank = 5;
    std::string guardian, guild;
    int level = 0;
    std::int64_t graduates = 0;
};
std::vector<AcademyRankEntry> LoadAcademyRanking(Database& db);
// Each ranked academy's rank (the others 5).
std::map<std::uint32_t, std::uint8_t> LoadAcademyRanks(Database& db);

// The matching list (ours, `sr_academy_matching`): an academy's entry and a page of them.
struct AcademyMatchEntry {
    std::uint32_t id = 0, academy = 0;
    std::uint8_t kind = 1;
    std::string title;
    std::int64_t guardianCharacter = 0;
    std::string guardian;
    int level = 0;
    std::uint8_t race = 0, rank = 5, members = 0;
    std::int64_t graduates = 0;
};
// Registers (or, `modify`, changes) the academy's entry; NotMember when modifying one it has not, AlreadyMember when
// registering a second.
AcademyWrite SaveAcademyMatch(Database& db, std::uint32_t academy, std::uint8_t kind, const std::string& title, bool modify, std::string& error);
bool DeleteAcademyMatch(Database& db, std::uint32_t academy);
std::optional<AcademyMatchEntry> AcademyMatchOf(Database& db, std::uint32_t academy);
std::optional<AcademyMatchEntry> FindAcademyMatch(Database& db, std::uint32_t entry);
// A page of `rows` entries by the search (honour class 0 all, kind 0 all, the guardian's name a prefix); `pages` the count.
std::vector<AcademyMatchEntry> LoadAcademyMatches(Database& db, int page, int rows, int honorClass, int kind, const std::string& name, int& pages);

// A character's academy waits (timed jobs 2/7..2/10): their ends in Unix seconds, by job id.
std::map<std::int64_t, std::int64_t> LoadAcademyWaits(Database& db, std::int64_t character, bool& ok);
void SaveAcademyWait(Database& db, std::int64_t character, std::int64_t job, std::int64_t until);

} // namespace sr::server
