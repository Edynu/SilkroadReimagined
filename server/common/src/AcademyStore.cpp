#include "sr/server/AcademyStore.h"

#include <algorithm>
#include <ctime>

#include "sr/core/Log.h"
#include "sr/game/Academy.h"
#include "sr/server/Database.h"

namespace sr::server {

namespace academy = game::academy;

std::optional<AcademyRow> LoadAcademy(Database& db, std::uint32_t id, bool& ok) {
    QueryResult a = db.Query("SELECT id, extract(epoch FROM creationdate)::bigint, rank, graduatecount, evaluationpoint, coalesce(commenttitle, ''), "
                             "coalesce(comment, '') FROM _trainingcamp WHERE id = $1",
                             {std::int64_t(id)});
    QueryResult m = db.Query("SELECT charid, refobjid, charname, extract(epoch FROM joindate)::bigint, memberclass, charjoinedlevel, charcurlevel, "
                             "charmaxlevel, honorpoint FROM _trainingcampmember WHERE campid = $1 ORDER BY memberclass, joindate",
                             {std::int64_t(id)});
    ok = a.Ok() && m.Ok();
    if (!ok || a.Rows() == 0) return std::nullopt;
    AcademyRow row;
    row.id = std::uint32_t(a.Int(0, 0));
    row.founded = a.Int(0, 1);
    row.rank = std::uint8_t(a.Int(0, 2));
    row.graduates = a.Int(0, 3);
    row.evaluation = a.Int(0, 4);
    row.noticeTitle = a.Text(0, 5);
    row.notice = a.Text(0, 6);
    for (int r = 0; r < m.Rows(); ++r) {
        AcademyMemberRow x;
        x.character = m.Int(r, 0);
        x.model = std::uint32_t(m.Int(r, 1));
        x.name = m.Text(r, 2);
        x.joined = m.Int(r, 3);
        x.memberClass = std::uint8_t(m.Int(r, 4));
        x.joinedLevel = int(m.Int(r, 5));
        x.level = int(m.Int(r, 6));
        x.maxLevel = int(m.Int(r, 7));
        x.honor = m.Int(r, 8);
        row.members.push_back(std::move(x));
    }
    if (QueryResult s = db.Query("SELECT id, kind, title FROM sr_academy_matching WHERE campid = $1", {std::int64_t(id)}); s.Ok() && s.Rows() > 0) {
        row.matchEntry = std::uint32_t(s.Int(0, 0));
        row.matchKind = std::uint8_t(s.Int(0, 1));
        row.matchTitle = s.Text(0, 2);
    }
    return row;
}

std::uint32_t AcademyOf(Database& db, std::int64_t character, bool& ok) {
    QueryResult r = db.Query("SELECT campid FROM _trainingcampmember WHERE charid = $1", {character});
    ok = r.Ok();
    return ok && r.Rows() > 0 ? std::uint32_t(r.Int(0, 0)) : 0u;
}

namespace {

// _TrainingCamp_FnAddMember's rules inside a transaction: the class by the highest level reached, the counts, the row
// (its joined level the highest reached), the buff rows (the guardian slot 0 and 1..5 of type 3, an apprentice 0 and 1).
AcademyWrite AddMember(Database& tx, std::uint32_t academy, std::int64_t character, int level, int maxLevel, std::int64_t honor, std::string& error) {
    QueryResult already = tx.Query("SELECT 1 FROM _trainingcampmember WHERE charid = $1", {character});
    if (!already.Ok()) return error = already.Error(), AcademyWrite::Failed;
    if (already.Rows() > 0) return AcademyWrite::AlreadyMember;
    QueryResult c = tx.Query("SELECT charname16, refobjid FROM _char WHERE charid = $1", {character});
    if (!c.Ok() || c.Rows() == 0) return error = c.Ok() ? "no character" : c.Error(), AcademyWrite::Failed;
    const std::string name = c.Text(0, 0);
    const std::int64_t model = c.Int(0, 1);
    const academy::Class cls = academy::ClassOfLevel(maxLevel);
    QueryResult count = tx.Query("SELECT count(*) FROM _trainingcampmember WHERE campid = $1 AND memberclass = $2", {std::int64_t(academy), int(cls)});
    if (!count.Ok()) return error = count.Error(), AcademyWrite::Failed;
    const std::int64_t n = count.Int(0, 0);
    if (cls == academy::Class::Guardian && n >= 1) return AcademyWrite::GuardianTaken;
    if (cls == academy::Class::Assistant && n >= academy::kMostAssistants) return AcademyWrite::AssistantsFull;
    if (cls == academy::Class::Apprentice && n >= academy::kMostApprentices) return AcademyWrite::ApprenticesFull;
    if (!tx.Query("INSERT INTO _trainingcampmember (campid, charid, refobjid, charname, joindate, memberclass, charjoinedlevel, charcurlevel, "
                  "charmaxlevel, honorpoint) VALUES ($1, $2, $3, $4, localtimestamp, $5, $6, $7, $6, $8)",
                  {std::int64_t(academy), character, model, name, int(cls), maxLevel, level, honor})
             .Ok()) {
        return error = "the member's row", AcademyWrite::Failed;
    }
    if (cls != academy::Class::Assistant) {
        const int last = cls == academy::Class::Guardian ? 5 : 1;
        if (!tx.Query("INSERT INTO _trainingcampbuffstatus (campid, recipientcharid, buffslotidx, donorcharid, startingtime, remainbuffpoint, bufftype) "
                      "SELECT $1, $2, g, 0, '2000-01-01'::timestamp, 0, CASE WHEN g = 0 THEN 0 ELSE 3 END "
                      "FROM generate_series(0, $3::int) g ON CONFLICT DO NOTHING",
                      {std::int64_t(academy), character, last})
                 .Ok()) {
            return error = "the buff rows", AcademyWrite::Failed;
        }
    }
    return AcademyWrite::Ok;
}

// A character's points kept from an academy before (`_TrainingCampSubMentorHonorPoint`), taken back.
std::int64_t TakeKeptPoints(Database& tx, std::int64_t character) {
    QueryResult r = tx.Query("DELETE FROM _trainingcampsubmentorhonorpoint WHERE charid = $1 RETURNING honorpoint", {character});
    return r.Ok() && r.Rows() > 0 ? r.Int(0, 0) : 0;
}

// An assistant's points kept for its next academy (added to what it keeps already).
bool KeepPoints(Database& tx, std::int64_t character, std::int64_t points) {
    if (points <= 0) return true;
    return tx.Query("INSERT INTO _trainingcampsubmentorhonorpoint (charid, honorpoint) VALUES ($1, $2) "
                    "ON CONFLICT (charid) DO UPDATE SET honorpoint = _trainingcampsubmentorhonorpoint.honorpoint + EXCLUDED.honorpoint",
                    {character, points})
        .Ok();
}

} // namespace

AcademyWrite FoundAcademy(Database& db, std::int64_t character, int level, int maxLevel, std::uint32_t& academy, std::string& error) {
    AcademyWrite result = AcademyWrite::Failed;
    db.Transaction(
        [&](Database& tx) {
            QueryResult already = tx.Query("SELECT 1 FROM _trainingcampmember WHERE charid = $1", {character});
            if (!already.Ok()) return false;
            if (already.Rows() > 0) {
                result = AcademyWrite::AlreadyMember;
                return false;
            }
            const std::int64_t kept = TakeKeptPoints(tx, character);
            QueryResult made = tx.Query("INSERT INTO _trainingcamp (creationdate, rank, graduatecount, evaluationpoint, latestevaluationdate, commenttitle, comment) "
                                        "VALUES (localtimestamp, 5, 0, 0, localtimestamp, '', '') RETURNING id");
            if (!made.Ok() || made.Rows() == 0) return false;
            academy = std::uint32_t(made.Int(0, 0));
            result = AddMember(tx, academy, character, level, maxLevel, kept, error);
            return result == AcademyWrite::Ok;
        },
        error);
    return result;
}

AcademyWrite AddAcademyMember(Database& db, std::uint32_t academy, std::int64_t character, int level, int maxLevel, std::string& error) {
    AcademyWrite result = AcademyWrite::Failed;
    db.Transaction(
        [&](Database& tx) {
            QueryResult exists = tx.Query("SELECT 1 FROM _trainingcamp WHERE id = $1", {std::int64_t(academy)});
            if (!exists.Ok()) return false;
            if (exists.Rows() == 0) {
                result = AcademyWrite::NoAcademy;
                return false;
            }
            const std::int64_t kept = TakeKeptPoints(tx, character);
            result = AddMember(tx, academy, character, level, maxLevel, kept, error);
            return result == AcademyWrite::Ok;
        },
        error);
    return result;
}

AcademyWrite RemoveAcademyMember(Database& db, std::uint32_t academy, std::int64_t character, bool ban, std::string& error) {
    AcademyWrite result = AcademyWrite::Failed;
    db.Transaction(
        [&](Database& tx) {
            QueryResult m = tx.Query("SELECT memberclass, honorpoint FROM _trainingcampmember WHERE campid = $1 AND charid = $2", {std::int64_t(academy), character});
            if (!m.Ok()) return false;
            if (m.Rows() == 0 || m.Int(0, 0) == int(academy::Class::Guardian)) {
                result = AcademyWrite::NotMember;
                return false;
            }
            if (m.Int(0, 0) == int(academy::Class::Assistant)) {
                const std::int64_t points = ban ? m.Int(0, 1) : std::max<std::int64_t>(0, m.Int(0, 1) - 10);
                if (!KeepPoints(tx, character, points)) return false;
            } else if (!tx.Query("DELETE FROM _trainingcampbuffstatus WHERE campid = $1 AND recipientcharid = $2", {std::int64_t(academy), character}).Ok()) {
                return false;
            }
            if (!tx.Query("DELETE FROM _trainingcampmember WHERE campid = $1 AND charid = $2", {std::int64_t(academy), character}).Ok()) return false;
            result = AcademyWrite::Ok;
            return true;
        },
        error);
    return result;
}

AcademyWrite DisbandAcademy(Database& db, std::uint32_t academy, std::int64_t guardian, std::string& error) {
    AcademyWrite result = AcademyWrite::Failed;
    db.Transaction(
        [&](Database& tx) {
            QueryResult g = tx.Query("SELECT memberclass FROM _trainingcampmember WHERE campid = $1 AND charid = $2", {std::int64_t(academy), guardian});
            if (!g.Ok()) return false;
            if (g.Rows() == 0 || g.Int(0, 0) != int(academy::Class::Guardian)) {
                result = AcademyWrite::NotMember;
                return false;
            }
            QueryResult assistants = tx.Query("SELECT charid, honorpoint FROM _trainingcampmember WHERE campid = $1 AND memberclass = 1 AND honorpoint > 0",
                                              {std::int64_t(academy)});
            if (!assistants.Ok()) return false;
            for (int r = 0; r < assistants.Rows(); ++r) {
                if (!KeepPoints(tx, assistants.Int(r, 0), assistants.Int(r, 1))) return false;
            }
            if (!tx.Query("DELETE FROM _trainingcampmember WHERE campid = $1", {std::int64_t(academy)}).Ok() ||
                !tx.Query("DELETE FROM _trainingcampbuffstatus WHERE campid = $1", {std::int64_t(academy)}).Ok() ||
                !tx.Query("UPDATE _trainingcamphonorrank SET campid = NULL, rank = NULL WHERE campid = $1", {std::int64_t(academy)}).Ok() ||
                !tx.Query("DELETE FROM sr_academy_matching WHERE campid = $1", {std::int64_t(academy)}).Ok() ||
                !tx.Query("DELETE FROM _trainingcamp WHERE id = $1", {std::int64_t(academy)}).Ok()) {
                return false;
            }
            result = AcademyWrite::Ok;
            return true;
        },
        error);
    return result;
}

AcademyWrite GraduateAcademyMember(Database& db, std::uint32_t academy, std::int64_t character, int evaluation, std::uint8_t& memberClass,
                                   std::string& error) {
    AcademyWrite result = AcademyWrite::Failed;
    db.Transaction(
        [&](Database& tx) {
            QueryResult exists = tx.Query("SELECT 1 FROM _trainingcamp WHERE id = $1", {std::int64_t(academy)});
            if (!exists.Ok()) return false;
            if (exists.Rows() == 0) {
                result = AcademyWrite::NoAcademy;
                return false;
            }
            QueryResult m = tx.Query("SELECT memberclass, honorpoint, charjoinedlevel FROM _trainingcampmember WHERE campid = $1 AND charid = $2",
                                     {std::int64_t(academy), character});
            if (!m.Ok()) return false;
            if (m.Rows() == 0 || m.Int(0, 0) == int(academy::Class::Guardian)) {
                result = AcademyWrite::NotMember;
                return false;
            }
            memberClass = std::uint8_t(m.Int(0, 0));
            if (memberClass == std::uint8_t(academy::Class::Assistant)) {
                // An assistant has no rating; its points are kept for its next academy.
                if (!KeepPoints(tx, character, m.Int(0, 1))) return false;
            } else {
                const int joined = int(m.Int(0, 2));
                if (const int honor = academy::GuardianHonorFor(joined); honor > 0) {
                    if (!tx.Query("UPDATE _trainingcampmember SET honorpoint = honorpoint + $2 WHERE campid = $1 AND memberclass = 0",
                                  {std::int64_t(academy), honor})
                             .Ok() ||
                        !tx.Query("UPDATE _trainingcampmember SET honorpoint = honorpoint + $2 WHERE campid = $1 AND memberclass = 1",
                                  {std::int64_t(academy), honor / 10})
                             .Ok() ||
                        !tx.Query("UPDATE _trainingcamp SET graduatecount = graduatecount + 1, evaluationpoint = greatest(evaluationpoint + $2, 0), "
                                  "latestevaluationdate = localtimestamp WHERE id = $1",
                                  {std::int64_t(academy), evaluation})
                             .Ok()) {
                        return false;
                    }
                } else if (!tx.Query("UPDATE _trainingcamp SET graduatecount = graduatecount + 1 WHERE id = $1", {std::int64_t(academy)}).Ok()) {
                    return false;
                }
                if (!tx.Query("DELETE FROM _trainingcampbuffstatus WHERE campid = $1 AND recipientcharid = $2", {std::int64_t(academy), character}).Ok()) {
                    return false;
                }
            }
            if (!tx.Query("DELETE FROM _trainingcampmember WHERE campid = $1 AND charid = $2", {std::int64_t(academy), character}).Ok()) return false;
            result = AcademyWrite::Ok;
            return true;
        },
        error);
    return result;
}

bool SaveAcademyNotice(Database& db, std::uint32_t academy, const std::string& title, const std::string& text) {
    return db.Query("UPDATE _trainingcamp SET commenttitle = $2, comment = $3 WHERE id = $1", {std::int64_t(academy), title, text}).Ok();
}

bool UpdateAcademyHonorRanking(Database& db, bool force, std::string& error) {
    bool ran = false;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            // Once a day for the shard: the date row taken under its lock (_TRAINING_CAMP_STARTUP_HONOR_UPDATE_NEEDED).
            QueryResult due = tx.Query(force ? "UPDATE _trainingcamphonorrankupdatedate SET lastupdatedate = localtimestamp RETURNING 1"
                                             : "UPDATE _trainingcamphonorrankupdatedate SET lastupdatedate = localtimestamp "
                                               "WHERE lastupdatedate::date < current_date RETURNING 1");
            if (!due.Ok()) return false;
            if (due.Rows() == 0) return true;
            if (!tx.Query("UPDATE _trainingcamp SET rank = 5").Ok() || !tx.Query("UPDATE _trainingcamphonorrank SET campid = NULL, rank = NULL").Ok()) return false;
            QueryResult best = tx.Query("SELECT id FROM _trainingcamp WHERE evaluationpoint >= $1 ORDER BY evaluationpoint DESC, latestevaluationdate ASC LIMIT $2",
                                        {academy::kRankMinimumEvaluation, academy::kRankedAcademies});
            if (!best.Ok()) return false;
            for (int r = 0; r < best.Rows(); ++r) {
                const int place = r + 1;
                const int rank = academy::RankOfPlace(place);
                if (!tx.Query("UPDATE _trainingcamphonorrank SET campid = $2, rank = $3 WHERE ranking = $1", {place, best.Int(r, 0), rank}).Ok() ||
                    !tx.Query("UPDATE _trainingcamp SET rank = $2 WHERE id = $1", {best.Int(r, 0), rank}).Ok()) {
                    return false;
                }
            }
            ran = true;
            return true;
        },
        error);
    return ok && ran;
}

std::vector<AcademyRankEntry> LoadAcademyRanking(Database& db) {
    std::vector<AcademyRankEntry> out;
    QueryResult r = db.Query("SELECT h.ranking, h.rank, m.charname, coalesce(c.curlevel, m.charcurlevel), t.graduatecount, coalesce(g.name, '') "
                             "FROM _trainingcamphonorrank h JOIN _trainingcamp t ON t.id = h.campid "
                             "JOIN _trainingcampmember m ON m.campid = h.campid AND m.memberclass = 0 "
                             "LEFT JOIN _char c ON c.charid = m.charid LEFT JOIN _guild g ON g.id = c.guildid AND c.guildid > 0 "
                             "WHERE h.campid IS NOT NULL ORDER BY h.ranking");
    for (int i = 0; r.Ok() && i < r.Rows(); ++i) {
        AcademyRankEntry e;
        e.place = int(r.Int(i, 0));
        e.rank = std::uint8_t(r.Int(i, 1));
        e.guardian = r.Text(i, 2);
        e.level = int(r.Int(i, 3));
        e.graduates = r.Int(i, 4);
        e.guild = r.Text(i, 5);
        out.push_back(std::move(e));
    }
    return out;
}

std::map<std::uint32_t, std::uint8_t> LoadAcademyRanks(Database& db) {
    std::map<std::uint32_t, std::uint8_t> out;
    QueryResult r = db.Query("SELECT id, rank FROM _trainingcamp");
    for (int i = 0; r.Ok() && i < r.Rows(); ++i) out[std::uint32_t(r.Int(i, 0))] = std::uint8_t(r.Int(i, 1));
    return out;
}

namespace {

// The entries with what the list shows: the guardian's name, level and race (Europe's figures from 14,000 on), the
// academy's graduates, members and rank.
constexpr const char* kMatchSelect =
    "SELECT s.id, s.campid, s.kind, s.title, m.charid, m.charname, coalesce(c.curlevel, m.charcurlevel), "
    "CASE WHEN m.refobjid >= 14000 THEN 1 ELSE 0 END, t.rank, "
    "(SELECT count(*) FROM _trainingcampmember x WHERE x.campid = s.campid), t.graduatecount "
    "FROM sr_academy_matching s JOIN _trainingcamp t ON t.id = s.campid "
    "JOIN _trainingcampmember m ON m.campid = s.campid AND m.memberclass = 0 LEFT JOIN _char c ON c.charid = m.charid ";

AcademyMatchEntry MatchAt(const QueryResult& r, int i) {
    AcademyMatchEntry e;
    e.id = std::uint32_t(r.Int(i, 0));
    e.academy = std::uint32_t(r.Int(i, 1));
    e.kind = std::uint8_t(r.Int(i, 2));
    e.title = r.Text(i, 3);
    e.guardianCharacter = r.Int(i, 4);
    e.guardian = r.Text(i, 5);
    e.level = int(r.Int(i, 6));
    e.race = std::uint8_t(r.Int(i, 7));
    e.rank = std::uint8_t(r.Int(i, 8));
    e.members = std::uint8_t(r.Int(i, 9));
    e.graduates = r.Int(i, 10);
    return e;
}

} // namespace

AcademyWrite SaveAcademyMatch(Database& db, std::uint32_t academy, std::uint8_t kind, const std::string& title, bool modify, std::string& error) {
    if (modify) {
        QueryResult r = db.Query("UPDATE sr_academy_matching SET kind = $2, title = $3 WHERE campid = $1 RETURNING id", {std::int64_t(academy), int(kind), title});
        if (!r.Ok()) return error = r.Error(), AcademyWrite::Failed;
        return r.Rows() > 0 ? AcademyWrite::Ok : AcademyWrite::NotMember;
    }
    QueryResult r = db.Query("INSERT INTO sr_academy_matching (campid, kind, title) VALUES ($1, $2, $3) ON CONFLICT (campid) DO NOTHING RETURNING id",
                             {std::int64_t(academy), int(kind), title});
    if (!r.Ok()) return error = r.Error(), AcademyWrite::Failed;
    return r.Rows() > 0 ? AcademyWrite::Ok : AcademyWrite::AlreadyMember;
}

bool DeleteAcademyMatch(Database& db, std::uint32_t academy) {
    QueryResult r = db.Query("DELETE FROM sr_academy_matching WHERE campid = $1 RETURNING id", {std::int64_t(academy)});
    return r.Ok() && r.Rows() > 0;
}

std::optional<AcademyMatchEntry> AcademyMatchOf(Database& db, std::uint32_t academy) {
    QueryResult r = db.Query(std::string(kMatchSelect) + "WHERE s.campid = $1", {std::int64_t(academy)});
    if (!r.Ok() || r.Rows() == 0) return std::nullopt;
    return MatchAt(r, 0);
}

std::optional<AcademyMatchEntry> FindAcademyMatch(Database& db, std::uint32_t entry) {
    QueryResult r = db.Query(std::string(kMatchSelect) + "WHERE s.id = $1", {std::int64_t(entry)});
    if (!r.Ok() || r.Rows() == 0) return std::nullopt;
    return MatchAt(r, 0);
}

std::vector<AcademyMatchEntry> LoadAcademyMatches(Database& db, int page, int rows, int honorClass, int kind, const std::string& name, int& pages) {
    std::vector<AcademyMatchEntry> out;
    const std::string where = "WHERE ($1 = 0 OR t.rank = $1) AND ($2 = 0 OR s.kind = $2) AND ($3 = '' OR lower(m.charname) LIKE lower($3) || '%') ";
    QueryResult n = db.Query("SELECT count(*) FROM sr_academy_matching s JOIN _trainingcamp t ON t.id = s.campid "
                             "JOIN _trainingcampmember m ON m.campid = s.campid AND m.memberclass = 0 " + where,
                             {honorClass, kind, name});
    const std::int64_t total = n.Ok() && n.Rows() > 0 ? n.Int(0, 0) : 0;
    pages = int(std::max<std::int64_t>(1, (total + rows - 1) / rows));
    QueryResult r = db.Query(std::string(kMatchSelect) + where + "ORDER BY s.id LIMIT $4 OFFSET $5",
                             {honorClass, kind, name, rows, std::int64_t(std::max(0, page)) * rows});
    for (int i = 0; r.Ok() && i < r.Rows(); ++i) out.push_back(MatchAt(r, i));
    return out;
}

// The slots' times kept as the original's local StartingTime; read back as the seconds since it, so the server's own clock
// never meets the database's time zone.
std::vector<AcademyBuffRow> LoadAcademyBuffs(Database& db, std::int64_t character, bool& ok) {
    std::vector<AcademyBuffRow> out;
    QueryResult r = db.Query("SELECT buffslotidx, coalesce(donorcharid, 0), CASE WHEN startingtime < '2001-01-01' THEN -1 "
                             "ELSE greatest(0, extract(epoch FROM localtimestamp - startingtime))::bigint END, coalesce(remainbuffpoint, 0), "
                             "coalesce(bufftype, 0) FROM _trainingcampbuffstatus WHERE recipientcharid = $1 ORDER BY buffslotidx",
                             {character});
    ok = r.Ok();
    for (int i = 0; ok && i < r.Rows(); ++i) {
        AcademyBuffRow row;
        row.slot = int(r.Int(i, 0));
        row.donor = r.Int(i, 1);
        row.since = r.Int(i, 2);
        row.points = r.Int(i, 3);
        row.type = std::uint8_t(r.Int(i, 4));
        out.push_back(row);
    }
    return out;
}

bool SaveAcademyBuff(Database& db, std::int64_t character, const AcademyBuffRow& row) {
    return db.Query("UPDATE _trainingcampbuffstatus SET donorcharid = $3, startingtime = CASE WHEN $4::bigint < 0 THEN '2000-01-01'::timestamp "
                    "ELSE localtimestamp - make_interval(secs => $4::bigint) END, remainbuffpoint = $5, bufftype = $6 "
                    "WHERE recipientcharid = $1 AND buffslotidx = $2",
                    {character, row.slot, row.donor, row.since, row.points, int(row.type)})
        .Ok();
}

bool ReceiveAcademyBuff(Database& db, std::int64_t character, std::int64_t donor, std::int64_t points) {
    QueryResult r = db.Query("UPDATE _trainingcampbuffstatus SET donorcharid = $2, startingtime = localtimestamp, remainbuffpoint = $3, bufftype = $4 "
                             "WHERE recipientcharid = $1 AND buffslotidx = (SELECT buffslotidx FROM _trainingcampbuffstatus WHERE recipientcharid = $1 "
                             "AND buffslotidx > 0 AND (donorcharid = $2 OR bufftype = $5) ORDER BY (donorcharid = $2) DESC, buffslotidx LIMIT 1)",
                             {character, donor, points, int(academy::BuffType::Received), int(academy::BuffType::Empty)});
    return r.Ok() && r.Affected() > 0;
}

void ClearAcademyBuffsFrom(Database& db, std::int64_t donor) {
    db.Query("UPDATE _trainingcampbuffstatus SET donorcharid = 0, startingtime = '2000-01-01', remainbuffpoint = 0, bufftype = $2 "
             "WHERE donorcharid = $1 AND buffslotidx > 0",
             {donor, int(academy::BuffType::Empty)});
}

void SaveAcademyLevels(Database& db, std::int64_t character, int level, int maxLevel) {
    db.Query("UPDATE _trainingcampmember SET charcurlevel = $2, charmaxlevel = greatest(charmaxlevel, $3) WHERE charid = $1", {character, level, maxLevel});
}

std::map<std::int64_t, std::int64_t> LoadAcademyWaits(Database& db, std::int64_t character, bool& ok) {
    std::map<std::int64_t, std::int64_t> out;
    QueryResult r = db.Query("SELECT jobid, max(coalesce(data1, 0)) FROM _timedjob WHERE charid = $1 AND category = $2 AND jobid BETWEEN $3 AND $4 GROUP BY jobid",
                             {character, academy::kTimedCategory, academy::kWaitFound, academy::kWaitRejoinAssistant});
    ok = r.Ok();
    for (int i = 0; ok && i < r.Rows(); ++i) out[r.Int(i, 0)] = r.Int(i, 1);
    return out;
}

void SaveAcademyWait(Database& db, std::int64_t character, std::int64_t job, std::int64_t until) {
    std::string error;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            const std::int64_t seconds = std::max<std::int64_t>(0, until - std::int64_t(std::time(nullptr)));
            return tx.Query("DELETE FROM _timedjob WHERE charid = $1 AND category = $2 AND jobid = $3", {character, academy::kTimedCategory, job}).Ok() &&
                   tx.Query("INSERT INTO _timedjob (charid, category, jobid, timetokeep, data1, data2, serial64, jid) "
                            "VALUES ($1, $2, $3, $4, $5, 0, 0, coalesce((SELECT userjid FROM _user WHERE charid = $1), 0))",
                            {character, academy::kTimedCategory, job, seconds, until})
                       .Ok();
        },
        error);
    if (!ok) core::LogWarning("an academy wait ({} job {}): {}", character, job, error);
}

} // namespace sr::server
