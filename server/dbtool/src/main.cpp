// SroDb.exe - our database's tool. The database is --db URL or SR_DB_URL.
//
//   --import DIR [--replace | --if-missing] [--only DB[,DB...]] [--shard-schema NAME]
//       the original server's databases, as exported to DIR (one folder per database: schema.tsv,
//       data/<table>.tsv), into schemas of the same names in
//       lower case (SRO_VT_SHARD into --shard-schema, default sro_vt_shard); then our layout on top
//       (the tables the servers use, our sr_ tables, the indexes). A table with rows is refused
//       unless --replace (which empties it first); --if-missing does nothing when the shard schema
//       holds the reference tables already (start-dev.bat). The accounts its characters belong to
//       that the export lacks get a row without a password. Exits 1 when any table was not loaded whole.
//   --quests FILE                     the original's quest script (Script/VIETNAM/Quest.sct, read as data, never
//       run) into sr_quests, the quests the shard's _RefQuest lists, with its rewards; with
//       --import after the tables (with --if-missing only when sr_quests lacks them), alone: replaces them
//   --migrate [--shard-schema NAME]   our layout on the database, as the servers make it
//   --counts                          the tables and their rows, by schema
//   --job-week [--shard-schema NAME] the jobs' week (the original's _UpdateTriangleJobWeekly, an
//       operator's job): the thieves' and hunters' pools shared by contribution into their rewards, the rankings
//       written, the contributions set to 0
//   --copy-character FROM --to NAME --account ACCOUNT [--password P] [--shard-schema NAME]
//       a character's twin (level, skills, quests, every item new) on an account, made when missing with the source's
//       rights (two clients' tries; server::CopyCharacter); both out of the game

#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <vector>

#include "sr/core/Config.h"
#include "sr/core/Log.h"
#include "sr/server/Database.h"
#include "sr/server/Import.h"
#include "sr/server/JobStore.h"
#include "sr/server/QuestStore.h"
#include "sr/server/Store.h"

namespace {

int Counts(sr::server::Database& db) {
    using namespace sr;
    server::QueryResult tables = db.Query(
        "SELECT table_schema, table_name FROM information_schema.tables "
        "WHERE table_type = 'BASE TABLE' AND table_schema NOT IN ('pg_catalog', 'information_schema') ORDER BY 1, 2");
    if (!tables.Ok()) {
        core::LogError("SroDb: {}", tables.Error());
        return 1;
    }
    std::string schema;
    int count = 0;
    std::int64_t rows = 0, filled = 0;
    const auto flush = [&] {
        if (!schema.empty()) core::LogInfo("SroDb: {}: {} tables ({} with rows), {} rows", schema, count, filled, rows);
    };
    for (int i = 0; i < tables.Rows(); ++i) {
        if (tables.Text(i, 0) != schema) {
            flush();
            schema = tables.Text(i, 0);
            count = 0;
            rows = filled = 0;
        }
        ++count;
        const std::int64_t n = db.Query(std::format("SELECT count(*) FROM {}.{}", tables.Text(i, 0), tables.Text(i, 1))).Int(0, 0);
        rows += n;
        filled += n > 0;
    }
    flush();
    return 0;
}

// The quest script into sr_quests (through a connection whose search path is the shard's).
bool ImportQuests(const std::string& url, const std::string& shard, const std::string& file) {
    using namespace sr;
    std::string error;
    auto db = server::Database::Connect(server::WithSearchPath(url, shard + ", " + server::kAccountSchema), error);
    if (!db) {
        core::LogError("SroDb: {}", error);
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        core::LogError("SroDb: the quest script {} cannot be read", file);
        return false;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    server::QuestImportReport report;
    if (!server::ImportQuestScript(*db, bytes, report, error)) {
        core::LogError("SroDb: the quests: {}", error);
        return false;
    }
    for (const std::string& note : report.notes) core::LogInfo("SroDb: quest script: {}", note);
    core::LogInfo("SroDb: quests: {} in the script and {} the game server codes ({} of them the script's, written in our format), {} imported "
                  "({} with the database's rewards), {} the database does not list left out",
                  report.decoded, report.coded, report.replaced, report.imported, report.rewardsFromDatabase, report.unknown);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    using namespace sr;
    core::Config c;
    std::string error;
    if (!c.ParseCommandLine(argc, argv, error)) {
        core::LogError("SroDb: {}", error);
        return 1;
    }
    std::string url = c.GetString("db");
    if (url.empty()) {
        if (const char* env = std::getenv("SR_DB_URL")) url = env;
    }
    if (url.empty()) {
        core::LogError("SroDb: no database: pass --db URL or set SR_DB_URL");
        return 1;
    }
    auto db = server::Database::Connect(url, error);
    if (!db) {
        core::LogError("SroDb: {}", error);
        return 1;
    }
    const std::string shard = c.GetString("shard-schema", server::kDefaultShardSchema);
    if (c.Has("counts")) return Counts(*db);
    if (c.Has("job-week")) {
        auto shardDb = server::Database::Connect(server::WithSearchPath(url, shard + ", " + server::kAccountSchema), error);
        if (!shardDb || !server::RunJobWeek(*shardDb, error)) {
            core::LogError("SroDb: the jobs' week: {}", error);
            return 1;
        }
        core::LogInfo("SroDb: the jobs' week: the pools shared, the rankings written");
        return 0;
    }
    if (c.Has("copy-character")) {
        // A character's twin on another account (two clients' tries): --copy-character FROM --to NAME --account ACCOUNT
        // [--password P]; the account made when there is none (its rights, sec_primary / sec_content, the source's).
        const std::string from = c.GetString("copy-character"), to = c.GetString("to"), account = c.GetString("account");
        if (from.empty() || to.empty() || account.empty()) {
            core::LogError("SroDb: --copy-character FROM --to NAME --account ACCOUNT [--password P]");
            return 1;
        }
        auto shardDb = server::Database::Connect(server::WithSearchPath(url, shard + ", " + server::kAccountSchema), error);
        if (!shardDb) {
            core::LogError("SroDb: {}", error);
            return 1;
        }
        bool ok = false;
        std::optional<server::AccountRow> row = server::FindAccount(*shardDb, account, ok);
        if (!ok) {
            core::LogError("SroDb: the account {} could not be read", account);
            return 1;
        }
        if (!row) {
            const std::string password = c.GetString("password", "secret123");
            if (server::CreateAccount(*shardDb, account, password, error) != server::CreateAccountResult::Created ||
                !(row = server::FindAccount(*shardDb, account, ok))) {
                core::LogError("SroDb: the account {} could not be made: {}", account, error);
                return 1;
            }
            const bool rights = shardDb
                                    ->Query("UPDATE tb_user t SET sec_primary = s.sec_primary, sec_content = s.sec_content FROM tb_user s "
                                            "JOIN _user u ON u.userjid = s.jid JOIN _char ch ON ch.charid = u.charid "
                                            "WHERE t.jid = $1 AND lower(ch.charname16) = lower($2)",
                                            {row->id, from})
                                    .Ok();
            core::LogInfo("SroDb: account {} made (password {}){}", account, password, rights ? ", its rights the source's" : "");
        }
        if (!server::CopyCharacter(*shardDb, row->id, from, to, error)) {
            core::LogError("SroDb: {} could not be copied: {}", from, error);
            return 1;
        }
        core::LogInfo("SroDb: {} copied as {} onto {}", from, to, account);
        return 0;
    }
    if (c.Has("import")) {
        if (c.Has("if-missing") && server::HasImportedShard(*db, shard)) {
            if (!server::MigrateSchema(*db, shard, error)) {
                core::LogError("SroDb: {}", error);
                return 1;
            }
            core::LogInfo("SroDb: {} holds the original's tables already; our layout is up to date", shard);
            if (const std::string quests = c.GetString("quests"); !quests.empty()) {
                auto check = server::Database::Connect(server::WithSearchPath(url, shard + ", " + server::kAccountSchema), error);
                if (check && !server::HasScriptQuests(*check) && !ImportQuests(url, shard, quests)) return 1;
            }
            return 0;
        }
        server::ImportOptions options;
        options.dir = c.GetString("import");
        options.shardSchema = shard;
        options.replace = c.Has("replace");
        if (const std::string only = c.GetString("only"); !only.empty()) {
            for (std::size_t at = 0; at <= only.size();) {
                std::size_t end = only.find(',', at);
                if (end == std::string::npos) end = only.size();
                if (end > at) options.databases.push_back(only.substr(at, end - at));
                at = end + 1;
            }
        }
        server::ImportReport report;
        if (!server::ImportDatabases(*db, options, report, error)) {
            core::LogError("SroDb: {}", error);
            return 1;
        }
        std::int64_t rows = 0;
        int made = 0;
        for (const server::ImportReport::Table& t : report.tables) {
            rows += t.loaded;
            made += t.made;
            if (!t.problem.empty()) core::LogWarning("SroDb: {}.{}: {}", t.schema, t.name, t.problem);
        }
        core::LogInfo("SroDb: {} tables ({} made), {} rows loaded; {} not loaded whole", report.tables.size(), made, rows, report.Problems());
        if (!server::MigrateSchema(*db, shard, error)) {
            core::LogError("SroDb: our layout on the import: {}", error);
            return 1;
        }
        core::LogInfo("SroDb: our layout is on it (account schema version {}, shard {} version {})", server::SchemaVersion(*db), shard,
                      server::ShardSchemaVersion(*db, shard));
        if (const int adopted = server::AdoptShardAccounts(*db, shard); adopted > 0) {
            core::LogInfo("SroDb: {} accounts its characters belong to were not in the export: made, without a password", adopted);
        }
        if (const std::string quests = c.GetString("quests"); !quests.empty() && !ImportQuests(url, shard, quests)) return 1;
        return report.Problems() == 0 ? 0 : 1;
    }
    if (c.Has("quests")) {
        if (!server::MigrateSchema(*db, shard, error)) {
            core::LogError("SroDb: {}", error);
            return 1;
        }
        return ImportQuests(url, shard, c.GetString("quests")) ? 0 : 1;
    }
    if (c.Has("migrate")) {
        if (!server::MigrateSchema(*db, shard, error)) {
            core::LogError("SroDb: {}", error);
            return 1;
        }
        core::LogInfo("SroDb: our layout is on the database (account version {}, {} version {})", server::SchemaVersion(*db), shard,
                      server::ShardSchemaVersion(*db, shard));
        return 0;
    }
    core::LogError("SroDb: say what to do: --import DIR, --quests FILE, --migrate or --counts");
    return 1;
}
