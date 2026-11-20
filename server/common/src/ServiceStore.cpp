#include "sr/server/ServiceStore.h"

#include <string>

#include "sr/server/Database.h"

namespace sr::server {

bool LoadTimedJobs(Database& db, std::int64_t character, const std::vector<std::uint8_t>& categories, std::vector<TimedJobRow>& out) {
    out.clear();
    std::string list = "{";
    for (std::size_t k = 0; k < categories.size(); ++k) list += (k ? "," : "") + std::to_string(int(categories[k]));
    list += "}";
    QueryResult q = db.Query("SELECT category, jobid, timetokeep, coalesce(data1, 0), coalesce(data2, 0), coalesce(data3, 0), coalesce(data4, 0), "
                             "coalesce(data5, 0), coalesce(data6, 0), coalesce(data7, 0), coalesce(data8, 0) FROM _timedjob "
                             "WHERE charid = $1 AND category = ANY($2::smallint[]) ORDER BY id",
                             {character, list});
    if (!q.Ok()) return false;
    for (int r = 0; r < q.Rows(); ++r) {
        TimedJobRow row;
        row.category = std::uint8_t(q.Int(r, 0));
        row.job = std::uint32_t(q.Int(r, 1));
        row.timeToKeep = std::int32_t(q.Int(r, 2));
        for (int k = 0; k < 8; ++k) row.data[k] = std::int32_t(q.Int(r, 3 + k));
        out.push_back(row);
    }
    return true;
}

bool SaveTimedJob(Database& db, std::int64_t character, std::uint8_t category, const TimedJobRow* row) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            if (!tx.Query("DELETE FROM _timedjob WHERE charid = $1 AND category = $2", {character, int(category)}).Ok()) return false;
            if (row == nullptr) return true;
            return tx.Query("INSERT INTO _timedjob (charid, category, jobid, timetokeep, data1, data2, data3, data4, data5, data6, data7, data8, serial64, jid) "
                            "SELECT $1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, 0, coalesce((SELECT userjid FROM _user WHERE charid = $1), 0)",
                            {character, int(category), std::int64_t(row->job), int(row->timeToKeep), int(row->data[0]), int(row->data[1]), int(row->data[2]),
                             int(row->data[3]), int(row->data[4]), int(row->data[5]), int(row->data[6]), int(row->data[7])})
                .Ok();
        },
        error);
}

bool SaveChestSize(Database& db, std::int64_t account, int size) {
    return db.Query("INSERT INTO _chestinfo (jid, chestsize) VALUES ($1, $2) ON CONFLICT (jid) DO UPDATE SET chestsize = EXCLUDED.chestsize", {account, size})
        .Ok();
}

} // namespace sr::server
