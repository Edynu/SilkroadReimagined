#pragma once

// The client's text tables as the database's original tables have them (D6): the
// game server reads items, characters, skills, teleports, shops, levels and quests through the
// same parsers as the client's files, but from the shard schema's _Ref* rows. Each client file
// under server_dep\silkroad\textdata (every shard of itemdata, characterdata, skilldata, ...) is
// rebuilt: the client's rows (the database's later content stays out: the client could not show
// it), each cell the database's value where it differs from the client's (an owner's edit), the
// client's own text where they agree. The columns were measured over every row of the 1.188
// export: the client's tables are the database's minus server-only columns (characterdata leaves
// out the 32 resistances, teleportdata three columns, ...).

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sr/assets/FileSystem.h"
#include "sr/server/Database.h"

namespace sr::server {

struct DatabaseTextCounts {
    int files = 0, rows = 0;
    int changed = 0;     // cells where the database differs from the client (its value taken)
    int clientOnly = 0;  // client rows the database lacks (kept as the client has them)
    int databaseOnly = 0; // database rows the client lacks (later content, left out)
    std::vector<std::string> samples; // the first differences: file, column, the client's, the database's
    std::map<std::string, int> byColumn; // the differences by file.column
};

// A source to mount over the game files (FileSystem::kOverridePriority) serving the rebuilt files,
// or null when the database lacks the tables (`error` says which). `game` is the mounted client.
std::unique_ptr<assets::IFileSource> MakeDatabaseText(Database& db, const assets::FileSystem& game, DatabaseTextCounts& counts, std::string& error);

} // namespace sr::server
