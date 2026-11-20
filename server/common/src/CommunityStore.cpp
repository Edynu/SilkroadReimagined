#include "sr/server/CommunityStore.h"

#include <algorithm>
#include <cctype>

#include "sr/game/CommunityProtocol.h"
#include "sr/server/Database.h"

namespace sr::server {

namespace {

constexpr int kFriendLimit = int(game::proto::kMaxFriends);
constexpr int kBlockLimit = int(game::proto::kMaxWhisperBlocks);

std::string Lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

} // namespace

bool LoadCommunity(Database& db, std::int64_t character, CommunityRows& out, std::string& error) {
    // The current name and race come from _Char: a name kept in _Friend goes stale when renamed.
    const QueryResult friends = db.Query(
        "SELECT f.friendcharid, coalesce(c.charname16, f.friendcharname), coalesce(c.refobjid, f.refobjid, 0) FROM _friend f "
        "LEFT JOIN _char c ON c.charid = f.friendcharid WHERE f.charid = $1 ORDER BY 2",
        {character});
    const QueryResult blocked = db.Query("SELECT targetname FROM _blockedwhisperers WHERE ownerid = $1 ORDER BY targetname", {character});
    const QueryResult unread = db.Query("SELECT count(*) FROM _memo WHERE charid = $1 AND status = 0", {character});
    if (!friends.Ok() || !blocked.Ok() || !unread.Ok()) {
        error = !friends.Ok() ? friends.Error() : !blocked.Ok() ? blocked.Error() : unread.Error();
        return false;
    }
    out = {};
    for (int r = 0; r < friends.Rows(); ++r) out.friends.push_back({friends.Int(r, 0), friends.Text(r, 1), std::uint32_t(friends.Int(r, 2))});
    for (int r = 0; r < blocked.Rows(); ++r) out.blocked.push_back(blocked.Text(r, 0));
    out.unread = int(unread.Int(0, 0));
    return true;
}

FriendWrite AddFriends(Database& db, std::int64_t asker, const std::string& askerName, std::int64_t target, const std::string& targetName,
                       std::uint32_t& askerModel, std::uint32_t& targetModel) {
    FriendWrite result = FriendWrite::Failed;
    std::string error;
    db.Transaction(
        [&](Database& tx) {
            // Both characters locked in one order: two proposals at once cannot pass the limits together.
            const QueryResult locked =
                tx.Query("SELECT charid, refobjid FROM _char WHERE charid IN ($1, $2) ORDER BY charid FOR UPDATE", {asker, target});
            if (!locked.Ok() || locked.Rows() != 2) return false;
            for (int r = 0; r < locked.Rows(); ++r) (locked.Int(r, 0) == asker ? askerModel : targetModel) = std::uint32_t(locked.Int(r, 1));
            const auto count = [&](std::int64_t who) {
                const QueryResult q = tx.Query("SELECT count(*) FROM _friend WHERE charid = $1", {who});
                return q.Ok() ? int(q.Int(0, 0)) : kFriendLimit;
            };
            const auto has = [&](std::int64_t who, std::int64_t other) {
                const QueryResult q = tx.Query("SELECT 1 FROM _friend WHERE charid = $1 AND friendcharid = $2", {who, other});
                return !q.Ok() || q.Rows() > 0;
            };
            if (count(asker) >= kFriendLimit) {
                result = FriendWrite::ListFull;
                return false;
            }
            if (has(asker, target) || has(target, asker)) {
                result = FriendWrite::Already;
                return false;
            }
            if (count(target) >= kFriendLimit) {
                result = FriendWrite::TargetFull;
                return false;
            }
            const char* insert = "INSERT INTO _friend (charid, friendcharid, friendcharname, refobjid) VALUES ($1, $2, $3, $4)";
            if (!tx.Query(insert, {asker, target, targetName, std::int64_t(targetModel)}).Ok()) return false;
            if (!tx.Query(insert, {target, asker, askerName, std::int64_t(askerModel)}).Ok()) return false;
            result = FriendWrite::Ok;
            return true;
        },
        error);
    return result;
}

bool RemoveFriends(Database& db, std::int64_t a, std::int64_t b) {
    return db.Query("DELETE FROM _friend WHERE (charid = $1 AND friendcharid = $2) OR (charid = $2 AND friendcharid = $1)", {a, b}).Ok();
}

BlockWrite AddWhisperBlock(Database& db, std::int64_t owner, std::string& name) {
    // A game master cannot be blocked: the procedure answers it like a name nobody has.
    if (name.starts_with("[GM]")) return BlockWrite::Unknown;
    BlockWrite result = BlockWrite::Failed;
    std::string error;
    db.Transaction(
        [&](Database& tx) {
            const QueryResult locked = tx.Query("SELECT 1 FROM _char WHERE charid = $1 FOR UPDATE", {owner});
            if (!locked.Ok()) return false;
            const QueryResult count = tx.Query("SELECT count(*) FROM _blockedwhisperers WHERE ownerid = $1", {owner});
            if (!count.Ok()) return false;
            if (count.Int(0, 0) >= kBlockLimit) {
                result = BlockWrite::Full;
                return false;
            }
            const QueryResult real = tx.Query("SELECT charname16 FROM _char WHERE lower(charname16) = $1 AND deleted = 0", {Lower(name)});
            if (!real.Ok()) return false;
            if (real.Rows() == 0) {
                result = BlockWrite::Unknown;
                return false;
            }
            name = real.Text(0, 0);
            const QueryResult there = tx.Query("SELECT 1 FROM _blockedwhisperers WHERE ownerid = $1 AND targetname = $2", {owner, name});
            if (!there.Ok()) return false;
            if (there.Rows() > 0) {
                result = BlockWrite::Exists;
                return false;
            }
            if (!tx.Query("INSERT INTO _blockedwhisperers (ownerid, targetname) VALUES ($1, $2)", {owner, name}).Ok()) return false;
            result = BlockWrite::Ok;
            return true;
        },
        error);
    return result;
}

bool RemoveWhisperBlock(Database& db, std::int64_t owner, std::string_view name) {
    const QueryResult done = db.Query("DELETE FROM _blockedwhisperers WHERE ownerid = $1 AND targetname = $2", {owner, name});
    return done.Ok() && done.Affected() > 0;
}

namespace {

constexpr const char* kLetterColumns =
    "id64, fromcharname, coalesce(message, ''), status, extract(year FROM date)::int, extract(month FROM date)::int, "
    "extract(day FROM date)::int, extract(hour FROM date)::int, extract(minute FROM date)::int";

LetterRow RowAt(const QueryResult& q, int r) {
    LetterRow row;
    row.id = q.Int(r, 0);
    row.from = q.Text(r, 1);
    row.text = q.Text(r, 2);
    row.status = int(q.Int(r, 3));
    row.year = int(q.Int(r, 4));
    row.month = int(q.Int(r, 5));
    row.day = int(q.Int(r, 6));
    row.hour = int(q.Int(r, 7));
    row.minute = int(q.Int(r, 8));
    return row;
}

} // namespace

bool LoadLetters(Database& db, std::int64_t character, std::vector<LetterRow>& out, std::string& error) {
    const QueryResult q = db.Query(std::string("SELECT ") + kLetterColumns + " FROM _memo WHERE charid = $1 ORDER BY date DESC, id64 DESC", {character});
    if (!q.Ok()) {
        error = q.Error();
        return false;
    }
    out.clear();
    for (int r = 0; r < q.Rows(); ++r) out.push_back(RowAt(q, r));
    return true;
}

bool ReadLetter(Database& db, std::int64_t character, std::int64_t id, LetterRow& out) {
    const QueryResult q = db.Query(std::string("UPDATE _memo SET status = 1 WHERE id64 = $1 AND charid = $2 RETURNING ") + kLetterColumns, {id, character});
    if (!q.Ok() || q.Rows() == 0) return false;
    out = RowAt(q, 0);
    return true;
}

bool DeleteLetter(Database& db, std::int64_t character, std::int64_t id) {
    const QueryResult q = db.Query("DELETE FROM _memo WHERE id64 = $1 AND charid = $2", {id, character});
    return q.Ok() && q.Affected() > 0;
}

LetterWriteResult AddLetter(Database& db, std::string_view to, const std::string& from, std::string_view text, std::size_t boxLimit,
                            std::int64_t& character, int& unread) {
    LetterWriteResult result = LetterWriteResult::Failed;
    std::string error;
    db.Transaction(
        [&](Database& tx) {
            const QueryResult who = tx.Query("SELECT charid FROM _char WHERE lower(charname16) = $1 AND deleted = 0 FOR UPDATE", {Lower(to)});
            if (!who.Ok()) return false;
            if (who.Rows() == 0) {
                result = LetterWriteResult::Unknown;
                return false;
            }
            character = who.Int(0, 0);
            const QueryResult count = tx.Query("SELECT count(*), coalesce(sum(CASE WHEN status = 0 THEN 1 ELSE 0 END), 0) FROM _memo WHERE charid = $1", {character});
            if (!count.Ok()) return false;
            if (count.Int(0, 0) >= std::int64_t(boxLimit)) {
                result = LetterWriteResult::BoxFull;
                return false;
            }
            const QueryResult sender = tx.Query("SELECT refobjid FROM _char WHERE charname16 = $1", {from});
            if (!sender.Ok()) return false;
            const std::int64_t model = sender.Rows() > 0 ? sender.Int(0, 0) : 0;
            if (!tx.Query("INSERT INTO _memo (charid, fromcharname, message, date, status, refobjid) VALUES ($1, $2, $3, localtimestamp, 0, $4)",
                          {character, from, text, model}).Ok()) {
                return false;
            }
            unread = int(count.Int(0, 1)) + 1;
            result = LetterWriteResult::Ok;
            return true;
        },
        error);
    return result;
}

} // namespace sr::server
