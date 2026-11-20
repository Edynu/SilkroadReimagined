#pragma once

// Friends, whisper blocks and letters in the original's tables: _Friend,
// _BlockedWhisperers and _Memo. Our own queries, written from what the original's procedures do
// (_Friend_Add, _Friend_Remove, _BlockedWhisperer_Add, _BlockedWhisperer_Remove, _Memo_Add,
// _Memo_Delete); none of them is called.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sr::server {

class Database;

struct FriendRow {
    std::int64_t character = 0;
    std::string name;
    std::uint32_t model = 0;
};

struct CommunityRows {
    std::vector<FriendRow> friends;
    std::vector<std::string> blocked;
    int unread = 0;
};

struct LetterRow {
    std::int64_t id = 0;
    std::string from, text;
    int status = 0;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
};

bool LoadCommunity(Database& db, std::int64_t character, CommunityRows& out, std::string& error);

// Both characters take each other (_Friend_Add's checks, in its order, each from the asker's side).
enum class FriendWrite : std::uint8_t { Ok, ListFull, Already, TargetFull, Failed };
FriendWrite AddFriends(Database& db, std::int64_t asker, const std::string& askerName, std::int64_t target, const std::string& targetName,
                       std::uint32_t& askerModel, std::uint32_t& targetModel);
bool RemoveFriends(Database& db, std::int64_t a, std::int64_t b);

// _BlockedWhisperer_Add's checks; `name` becomes the character's own spelling.
enum class BlockWrite : std::uint8_t { Ok, Exists, Unknown, Full, Failed };
BlockWrite AddWhisperBlock(Database& db, std::int64_t owner, std::string& name);
bool RemoveWhisperBlock(Database& db, std::int64_t owner, std::string_view name);

// Newest first.
bool LoadLetters(Database& db, std::int64_t character, std::vector<LetterRow>& out, std::string& error);
// The letter, now counted as read; false if it is not this character's.
bool ReadLetter(Database& db, std::int64_t character, std::int64_t id, LetterRow& out);
bool DeleteLetter(Database& db, std::int64_t character, std::int64_t id);
enum class LetterWriteResult : std::uint8_t { Ok, Unknown, BoxFull, Failed };
// `character`: whom it reached; the unread letters there now in `unread`.
LetterWriteResult AddLetter(Database& db, std::string_view to, const std::string& from, std::string_view text, std::size_t boxLimit,
                            std::int64_t& character, int& unread);

} // namespace sr::server
