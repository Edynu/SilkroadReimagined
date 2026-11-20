#pragma once

// The quests in our database: sr_quests holds each quest's definition as
// its text, the original's (imported from its quest script) and a server owner's alike; the game server reads them all
// at its start. The connection's search path names the shard's schema (WithSearchPath).

#include <cstdint>
#include <span>
#include <map>
#include <string>
#include <vector>

#include "sr/game/Quest.h"
#include "sr/server/Database.h"

namespace sr::server {

// Where a quest comes from.
enum class QuestOrigin : std::uint8_t { Script = 0, Ours = 1 };

struct QuestImportReport {
    int decoded = 0;          // definitions the script holds
    int coded = 0;            // the original's quests its game server codes (OriginalQuests.cpp)
    int replaced = 0;         // of them, the script's own quests written in our format instead (their talk coded in the script)
    int imported = 0;         // written to sr_quests
    int unknown = 0;          // the script's quests the database's _RefQuest does not list (later or custom content: left out)
    int rewardsFromDatabase = 0;
    std::vector<std::string> notes; // what the script holds that was not read
};

// The original's quests: the script's definitions that _RefQuest lists in service, with _RefQuest's number, level and
// texts' keys, and the rewards of _RefQuestReward and _RefQuestRewardItems (the database's over the script's); the
// rows of the script's origin are replaced, a server owner's own kept.
bool ImportQuestScript(Database& db, std::span<const std::uint8_t> script, QuestImportReport& report, std::string& error);

// Whether sr_quests holds the original's quests already.
bool HasScriptQuests(Database& db);

struct StoredQuest {
    game::QuestDef def;
    QuestOrigin origin = QuestOrigin::Script;
};
// Every quest; a definition that does not read is left out and named in `problems`.
bool LoadQuests(Database& db, std::vector<StoredQuest>& out, std::vector<std::string>& problems, std::string& error);
// A character's counted deeds (ours, sr_char_records; the beta quests' conditions, QuestDef::records): `n` added to the
// count of `name`, and every count of the character read.
bool AddCharRecord(Database& db, std::int64_t character, std::string_view name, int n, std::string& error);
bool LoadCharRecords(Database& db, std::int64_t character, std::map<std::string, int>& out, std::string& error);
// One quest written (made or replaced by its codename).
bool SaveQuest(Database& db, const game::QuestDef& q, QuestOrigin origin, std::string& error);

} // namespace sr::server
