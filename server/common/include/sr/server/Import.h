#pragma once

// The original server's databases into ours (phase D1): an export of its SQL
// Server databases (one folder per database: schema.tsv with every column, data/<table>.tsv with
// the rows) becomes PostgreSQL schemas of the same names in
// lower case, tables made from schema.tsv with the types of PostgreSQL, the rows
// loaded with COPY (the export's text is COPY's own format: tabs, \N for NULL, backslash escapes).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "sr/server/Database.h"
#include "sr/server/SqlServerBackup.h"

namespace sr::server {

struct ImportOptions {
    std::filesystem::path dir;                    // <dir>/<DATABASE>/schema.tsv and <dir>/<DATABASE>/data/*.tsv
    std::string shardSchema = kDefaultShardSchema; // where SRO_VT_SHARD goes (sro_vt_shard2 for a second shard)
    std::vector<std::string> databases;           // the folders to import (none: every one with a schema.tsv);
                                                  // SRO_CERTIFICATION never (our own server registry replaces it)
    bool replace = false;                         // a table that has rows is emptied first (else it is refused)
};

struct ImportReport {
    struct Table {
        std::string schema, name;
        std::int64_t expected = 0, loaded = 0; // the data file's rows, the rows COPY took
        bool made = false;                     // the table was made (it was not there)
        std::string problem;                   // why it was not loaded (empty: loaded, or no data)
    };
    std::vector<Table> tables;
    int Problems() const;
};

// The PostgreSQL type of an export column ("tinyint" -> "smallint", ...); empty for a type it does not know.
std::string ImportedType(const std::string& sqlServerType, int length, int precision, int scale);
// An export's default ("((0))", "('xxx')", "(getdate())") as PostgreSQL's, or empty when it has none we can carry.
std::string ImportedDefault(const std::string& sqlServerDefault);

// False with `error` when the import could not run; problems with single tables are in the report.
bool ImportDatabases(Database& db, const ImportOptions& options, ImportReport& report, std::string& error);

// The original's shard backup (SRO_VT_SHARD.bak, read by SqlServerBackup) into the shard's schema, for SroSetup: every
// table of it made (or given the columns it lacks), but rows only for the game's content (ImportsRowsOf): a new server
// starts with none of the original's accounts, characters, items, guilds or logs.
struct BackupImportOptions {
    std::string shardSchema = kDefaultShardSchema;
    bool replace = false; // a content table that has rows is emptied first (else it is refused)
};
bool ImportBackup(Database& db, const SqlServerBackup& backup, const BackupImportOptions& options, ImportReport& report, std::string& error);
// Whether a table of the original's shard brings its rows: its reference tables (_Ref*, Tab_Ref*, but not the
// rankings Tab_RefRanking*, which are players, nor _RefDummySlot, which our migrations fill) and the content the servers read (_Schedule, _ItemQuotation,
// _AssociationReputation, _TrijobRewards).
bool ImportsRowsOf(std::string_view table);

// Whether the shard schema holds the original's reference tables already (an import was done).
bool HasImportedShard(Database& db, const std::string& shardSchema);

// The accounts a shard's characters belong to (_user) that the account tables lack (an export
// without its account database): a tb_user row each, named after _accountjid where it says, with no
// password (it cannot log in until one is set), so a new account never takes their number. How many.
int AdoptShardAccounts(Database& db, const std::string& shardSchema);

} // namespace sr::server
