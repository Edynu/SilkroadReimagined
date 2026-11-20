// The original's shard backup read directly (SqlServerBackup) and imported for a new server (ImportBackup).
//
// SR_VSRO_DATA names the folder SroSetup reads (SRO_VT_SHARD.bak, Quest.sct); without it these skip. SR_VSRO_EXPORT,
// when set, names an export of the same backups made through SQL Server (<db>/schema.tsv, <db>/data/<table>.tsv): the
// reader must give the same tables, columns and rows.

#include <doctest/doctest.h>

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ServerTestSupport.h"
#include "sr/server/Import.h"
#include "sr/server/SqlServerBackup.h"

using namespace sr;
using namespace sr::test;

namespace {

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = line.find('\t', start);
        out.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) return out;
        start = end + 1;
    }
}

// Whether a cell of ours is the export's: floats as numbers (the export printed 7 or 15 digits, ours reads back exactly),
// bytea as the same bytes (its 0x.. against COPY's \\x..).
bool SameCell(const std::string& type, const std::string& ours, const std::string& theirs) {
    if (ours == theirs) return true;
    if (type == "real" || type == "float") {
        double a = 0, b = 0;
        if (std::from_chars(ours.data(), ours.data() + ours.size(), a).ec != std::errc() ||
            std::from_chars(theirs.data(), theirs.data() + theirs.size(), b).ec != std::errc()) {
            return false;
        }
        return type == "real" ? float(a) == float(b) || std::abs(a - b) <= std::abs(b) * 1e-6 : std::abs(a - b) <= std::abs(b) * 1e-14;
    }
    if (ours.starts_with("\\\\x") && theirs.starts_with("0x")) {
        std::string hex = theirs.substr(2);
        for (char& c : hex) c = char(std::tolower(static_cast<unsigned char>(c)));
        return ours.substr(3) == hex;
    }
    return false;
}

} // namespace

TEST_CASE("SqlServerBackup: the vSRO shard backup's tables and rows, as SQL Server itself exported them") {
    const std::filesystem::path data = EnvPath("SR_VSRO_DATA"), exportDir = EnvPath("SR_VSRO_EXPORT");
    if (data.empty() || exportDir.empty() || !std::filesystem::exists(data / "SRO_VT_SHARD.bak")) {
        MESSAGE("SR_VSRO_DATA or SR_VSRO_EXPORT not set - skipped");
        return;
    }
    server::SqlServerBackup backup;
    std::string error;
    REQUIRE_MESSAGE(backup.Open(data / "SRO_VT_SHARD.bak", error), error);
    const std::filesystem::path shard = exportDir / "SRO_VT_SHARD";
    // The export's schema: table -> its columns' names and types.
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> schema;
    {
        std::ifstream in(shard / "schema.tsv");
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::vector<std::string> c = Split(line);
            if (c.size() > 3) schema[c[0].substr(c[0].find('.') + 1)].push_back({c[2], c[3]});
        }
    }
    CHECK(backup.Tables().size() == schema.size());
    std::size_t rows = 0, tablesSame = 0;
    for (const server::BackupTable& t : backup.Tables()) {
        CAPTURE(t.name);
        const auto it = schema.find(t.name);
        REQUIRE(it != schema.end());
        REQUIRE(t.columns.size() == it->second.size());
        for (std::size_t i = 0; i < t.columns.size(); ++i) {
            CHECK(t.columns[i].name == it->second[i].first);
            CHECK(t.columns[i].type == it->second[i].second);
        }
        std::vector<std::string> ours;
        if (!backup.ReadRows(t, [&](std::string&& line) { ours.push_back(std::move(line)); }, error)) {
            // A large value out of the row: only SQL Server's own diagram table holds one.
            CHECK_MESSAGE(t.name == "dtproperties", error);
            continue;
        }
        std::ifstream in(shard / "data" / (t.name + ".tsv"));
        std::vector<std::string> theirs;
        std::string line;
        std::getline(in, line); // the header
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) theirs.push_back(line);
        }
        CHECK(ours.size() == theirs.size());
        if (ours.size() != theirs.size()) continue;
        // In no order: both sorted by their cells as the export prints them (floats' digits aside, nearly always the same
        // order; a pair that differs is compared cell by cell).
        std::sort(ours.begin(), ours.end());
        std::sort(theirs.begin(), theirs.end());
        std::size_t differing = 0;
        for (std::size_t r = 0; r < ours.size(); ++r) {
            if (ours[r] == theirs[r]) continue;
            const std::vector<std::string> a = Split(ours[r]), b = Split(theirs[r]);
            bool same = a.size() == b.size();
            for (std::size_t i = 0; same && i < a.size(); ++i) same = SameCell(t.columns[i].type, a[i], b[i]);
            if (!same) {
                ++differing;
                if (differing <= 2) MESSAGE("ours:   ", ours[r].substr(0, 200), "\nexport: ", theirs[r].substr(0, 200));
            }
        }
        CHECK(differing == 0);
        rows += ours.size();
        tablesSame += differing == 0;
    }
    MESSAGE(backup.Tables().size(), " tables, ", tablesSame, " of them row for row the export's; ", rows, " rows");
}

TEST_CASE("ImportBackup: a new shard gets the game's content from the backup and none of its players") {
    const std::filesystem::path data = EnvPath("SR_VSRO_DATA");
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    if (data.empty() || !std::filesystem::exists(data / "SRO_VT_SHARD.bak")) {
        MESSAGE("SR_VSRO_DATA not set - skipped");
        return;
    }
    CHECK(server::ImportsRowsOf("_RefObjCommon"));
    CHECK(server::ImportsRowsOf("Tab_RefNest"));
    CHECK(server::ImportsRowsOf("_Schedule"));
    CHECK_FALSE(server::ImportsRowsOf("Tab_RefRanking_TraderActivity"));
    CHECK_FALSE(server::ImportsRowsOf("_Char"));
    CHECK_FALSE(server::ImportsRowsOf("_Inventory"));
    server::SqlServerBackup backup;
    std::string error;
    REQUIRE_MESSAGE(backup.Open(data / "SRO_VT_SHARD.bak", error), error);
    server::ImportReport report;
    REQUIRE_MESSAGE(server::ImportBackup(t.Db(), backup, {}, report, error), error);
    for (const auto& row : report.tables) CHECK_MESSAGE(row.problem.empty(), row.name, ": ", row.problem);
    REQUIRE_MESSAGE(server::MigrateSchema(t.Db(), error), error);
    CHECK(server::HasImportedShard(t.Db(), server::kDefaultShardSchema));
    const auto count = [&](const char* table) { return t.Db().Query(std::string("SELECT count(*) FROM sro_vt_shard.") + table).Int(0, 0); };
    CHECK(count("_refobjcommon") > 30000);
    CHECK(count("_refskill") > 30000);
    CHECK(count("tab_refnest") > 10000);
    CHECK(count("_schedule") > 0);
    // The original's players stay out: its characters (but our "nobody" row 0), items, inventories, guilds, logs.
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_shard._char WHERE charid <> 0").Int(0, 0) == 0);
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_shard._items WHERE id64 <> 0").Int(0, 0) == 0);
    CHECK(count("_inventory") == 0);
    CHECK(count("_log_seek_n_destroy_item_fast") == 0);
    // A key from the backup's catalog, and a default.
    CHECK(t.Db().Query("SELECT count(*) FROM pg_constraint c JOIN pg_class r ON r.oid = c.conrelid JOIN pg_namespace n ON n.oid = r.relnamespace "
                       "WHERE n.nspname = 'sro_vt_shard' AND r.relname = '_refobjcommon' AND c.contype = 'p'")
              .Int(0, 0) == 1);
    // Again with replace: the content tables emptied and loaded again, the same rows.
    server::ImportReport again;
    server::BackupImportOptions replace;
    replace.replace = true;
    REQUIRE_MESSAGE(server::ImportBackup(t.Db(), backup, replace, again, error), error);
    CHECK(again.Problems() == 0);
    CHECK(count("_refobjcommon") > 30000);
}
