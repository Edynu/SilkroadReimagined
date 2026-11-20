#include "sr/server/Import.h"

#include "sr/server/SilkStore.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <functional>
#include <map>

#include "sr/core/Log.h"

namespace sr::server {

namespace {

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = line.find('\t', start);
        out.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) return out;
        start = end + 1;
    }
}

bool ReadLine(std::istream& in, std::string& line) {
    if (!std::getline(in, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

// A name PostgreSQL takes unquoted in lower case (the export's are letters, digits and '_').
bool PlainName(const std::string& name) {
    if (name.empty() || name.size() > 63 || std::isdigit(static_cast<unsigned char>(name[0]))) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

// A name as SQL takes it: unquoted when PostgreSQL takes it so (lower case, not a reserved word),
// else quoted (the export has names starting with a digit, a reserved word, Vietnamese letters).
struct Names {
    std::vector<std::string> reserved; // pg_get_keywords() R and T, sorted
    std::string operator()(const std::string& name) const {
        if (PlainName(name) && !std::binary_search(reserved.begin(), reserved.end(), name)) return name;
        std::string quoted = "\"";
        for (char c : name) quoted += c == '"' ? std::string("\"\"") : std::string(1, c);
        return quoted + "\"";
    }
};

struct Column {
    std::string name; // lower case
    std::string type; // PostgreSQL's
    bool notNull = false, identity = false, key = false;
    int keyOrdinal = 0; // its place in the primary key when known (a backup's), else the table's order
    std::string defaultValue;
};

struct Table {
    std::string source; // the export's name (data/<source>.tsv)
    std::string name;   // ours: lower case
    std::vector<Column> columns;
};

// schema.tsv: table, col, column, type, len, prec, scale, nullable, identity, pk, default.
bool ReadSchema(const std::filesystem::path& file, std::vector<Table>& tables, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = file.string() + ": cannot be read";
        return false;
    }
    std::string line;
    if (!ReadLine(in, line)) {
        error = file.string() + ": empty";
        return false;
    }
    const std::vector<std::string> head = SplitTabs(line);
    const auto at = [&](const char* name) {
        const auto it = std::find(head.begin(), head.end(), name);
        return it == head.end() ? -1 : int(it - head.begin());
    };
    const int cTable = at("table"), cColumn = at("column"), cType = at("type"), cLen = at("len"), cPrec = at("prec"), cScale = at("scale"),
              cNullable = at("nullable"), cIdentity = at("identity"), cPk = at("pk"), cDefault = at("default");
    if (cTable < 0 || cColumn < 0 || cType < 0) {
        error = file.string() + ": not an export's schema (no table, column, type)";
        return false;
    }
    std::map<std::string, std::size_t> index;
    while (ReadLine(in, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> cells = SplitTabs(line);
        const auto cell = [&](int i) { return i >= 0 && std::size_t(i) < cells.size() ? cells[std::size_t(i)] : std::string(); };
        std::string table = cell(cTable);
        // dbo.X -> x; another owner's O.X -> o_x.
        const std::size_t dot = table.find('.');
        const std::string owner = dot == std::string::npos ? "dbo" : table.substr(0, dot);
        if (dot != std::string::npos) table = table.substr(dot + 1);
        const std::string ours = Lower(owner == "dbo" ? table : owner + "_" + table);
        auto [it, fresh] = index.emplace(ours, tables.size());
        if (fresh) tables.push_back({table, ours, {}});
        Column c;
        c.name = Lower(cell(cColumn));
        c.type = ImportedType(Lower(cell(cType)), std::atoi(cell(cLen).c_str()), std::atoi(cell(cPrec).c_str()), std::atoi(cell(cScale).c_str()));
        if (c.type.empty()) {
            error = std::format("{}: {}.{} has a type the import does not know: {}", file.string(), ours, c.name, cell(cType));
            return false;
        }
        c.notNull = cell(cNullable) == "False";
        c.identity = cell(cIdentity) == "True" && (c.type == "integer" || c.type == "bigint" || c.type == "smallint");
        c.key = !cell(cPk).empty() && cell(cPk) != "0";
        c.defaultValue = ImportedDefault(cell(cDefault));
        tables[it->second].columns.push_back(std::move(c));
    }
    return true;
}

std::string CreateSql(const std::string& schema, const Table& t, const Names& ident) {
    std::string sql = std::format("CREATE TABLE {}.{} (", schema, ident(t.name));
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
        const Column& c = t.columns[i];
        sql += std::format("{}{} {}", i ? ", " : "", ident(c.name), c.type);
        if (c.identity) sql += " GENERATED BY DEFAULT AS IDENTITY";
        if (c.notNull) sql += " NOT NULL";
        if (!c.defaultValue.empty() && !c.identity) sql += " DEFAULT " + c.defaultValue;
    }
    std::vector<const Column*> keyed;
    for (const Column& c : t.columns)
        if (c.key) keyed.push_back(&c);
    std::stable_sort(keyed.begin(), keyed.end(), [](const Column* a, const Column* b) { return a->keyOrdinal < b->keyOrdinal; });
    for (const Column* c : keyed) keys.push_back(ident(c->name));
    if (!keys.empty()) {
        sql += ", PRIMARY KEY (";
        for (std::size_t i = 0; i < keys.size(); ++i) sql += (i ? ", " : "") + keys[i];
        sql += ")";
    }
    return sql + ")";
}

// Made when missing; one there already (ours, or an earlier import) gets the columns it lacks. False with the report
// row's problem when it could not be.
bool PrepareTable(Database& db, const std::string& schema, const Table& t, const Names& ident, ImportReport::Table& row) {
    const std::string qualified = schema + "." + ident(t.name);
    if (!db.Query("SELECT to_regclass($1) IS NOT NULL", {qualified}).Bool(0, 0)) {
        QueryResult made = db.Query(CreateSql(schema, t, ident));
        if (!made.Ok()) {
            row.problem = made.Error();
            return false;
        }
        row.made = true;
        return true;
    }
    for (const Column& c : t.columns) {
        QueryResult added = db.Query(std::format("ALTER TABLE {} ADD COLUMN IF NOT EXISTS {} {}{}", qualified, ident(c.name), c.type,
                                                 c.defaultValue.empty() ? "" : " DEFAULT " + c.defaultValue));
        if (!added.Ok()) row.problem = added.Error();
    }
    return row.problem.empty();
}

// Rows into a table with COPY (`list` its columns, `feed` a piece of COPY's text each call, false when there is no
// more), in one transaction: a table with rows is refused unless `replace` (then emptied first); identity columns go
// on past the highest id. The rows COPY took, or -1 with `error`.
std::int64_t LoadRows(Database& db, const std::string& qualified, const Table& t, const std::string& list, bool replace, const Names& ident,
                      const std::function<bool(std::string&)>& feed, std::string& error) {
    std::int64_t loaded = -1;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            QueryResult has = tx.Query(std::format("SELECT EXISTS (SELECT 1 FROM {})", qualified));
            if (!has.Ok()) return false;
            if (has.Bool(0, 0)) {
                if (!replace) {
                    error = "has rows already (--replace empties it first)";
                    return false;
                }
                if (!tx.Query(std::format("TRUNCATE {}", qualified)).Ok()) return false;
            }
            loaded = tx.CopyIn(std::format("COPY {} ({}) FROM STDIN", qualified, list), feed, error);
            if (loaded < 0) return false;
            for (const Column& c : t.columns) {
                if (!c.identity) continue;
                QueryResult moved = tx.Query(std::format("SELECT setval(pg_get_serial_sequence($1, $2), coalesce(max({0}), 0) + 1, false) FROM {1}",
                                                         ident(c.name), qualified),
                                             {qualified, c.name});
                if (!moved.Ok()) {
                    error = moved.Error();
                    return false;
                }
            }
            return true;
        },
        error);
    if (!ok) {
        if (error.empty()) error = "not loaded";
        return -1;
    }
    return loaded;
}

// The rows of a data file, without its header line.
std::int64_t CountRows(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string line;
    std::int64_t rows = -1; // the header
    while (std::getline(in, line)) ++rows;
    return std::max<std::int64_t>(rows, 0);
}

} // namespace

int ImportReport::Problems() const {
    return int(std::count_if(tables.begin(), tables.end(), [](const Table& t) { return !t.problem.empty(); }));
}

std::string ImportedType(const std::string& type, int length, int precision, int scale) {
    if (type == "tinyint" || type == "smallint" || type == "bit") return "smallint"; // no tinyint; bit stays a number
    if (type == "int") return "integer";
    if (type == "bigint") return "bigint";
    if (type == "real") return "real";
    if (type == "float") return "double precision";
    if (type == "numeric" || type == "decimal") return precision > 0 ? std::format("numeric({},{})", precision, scale) : "numeric";
    if (type == "money") return "numeric(19,4)";
    if (type == "smallmoney") return "numeric(10,4)";
    if (type == "varchar" || type == "nvarchar") return length > 0 ? std::format("varchar({})", length) : "text";
    if (type == "char" || type == "nchar") return length > 0 ? std::format("char({})", length) : "text";
    if (type == "text" || type == "ntext" || type == "xml" || type == "sql_variant" || type == "sysname") return "text";
    if (type == "datetime" || type == "smalldatetime" || type == "datetime2") return "timestamp";
    if (type == "date") return "date";
    if (type == "time") return "time";
    if (type == "datetimeoffset") return "timestamptz";
    if (type == "uniqueidentifier") return "uuid";
    if (type == "binary" || type == "varbinary" || type == "image" || type == "timestamp" || type == "rowversion") return "bytea";
    return {};
}

std::string ImportedDefault(const std::string& raw) {
    std::string v = raw;
    // SQL Server wraps defaults in parentheses, sometimes twice: ((0)), ('xxx'), (getdate()).
    while (v.size() >= 2 && v.front() == '(' && v.back() == ')') v = v.substr(1, v.size() - 2);
    if (v.empty()) return {};
    if (Lower(v) == "getdate()") return "localtimestamp";
    // A number.
    std::size_t i = v[0] == '-' ? 1 : 0;
    bool number = i < v.size(), dot = false;
    for (; i < v.size() && number; ++i) {
        if (v[i] == '.' && !dot) {
            dot = true;
        } else if (!std::isdigit(static_cast<unsigned char>(v[i]))) {
            number = false;
        }
    }
    if (number) return v;
    // A quoted string (N'...' too), quotes inside doubled as in PostgreSQL.
    if (v.size() >= 2 && (v[0] == 'N' || v[0] == 'n') && v[1] == '\'') v = v.substr(1);
    if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') return v;
    return {}; // a function or expression of SQL Server's: none
}

bool ImportDatabases(Database& db, const ImportOptions& options, ImportReport& report, std::string& error) {
    if (!ValidSchemaName(options.shardSchema)) {
        error = "not a schema name: " + options.shardSchema;
        return false;
    }
    std::vector<std::string> folders = options.databases;
    if (folders.empty()) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(options.dir, ec)) {
            if (entry.is_directory() && std::filesystem::exists(entry.path() / "schema.tsv")) folders.push_back(entry.path().filename().string());
        }
        std::sort(folders.begin(), folders.end());
        if (ec) {
            error = options.dir.string() + ": " + ec.message();
            return false;
        }
    }
    // SRO_CERTIFICATION (the original's farm: machines, modules, ports) is not ours to keep: our own
    // registry of shards and servers replaces it (decided 2026-11-07).
    std::erase_if(folders, [](const std::string& f) {
        if (Lower(f) != "sro_certification") return false;
        core::LogInfo("import: {} left out (our own server registry replaces it)", f);
        return true;
    });
    if (folders.empty()) {
        error = options.dir.string() + ": no database folders (each with a schema.tsv)";
        return false;
    }
    if (!db.Query("SET client_encoding TO 'UTF8'").Ok()) {
        error = "client_encoding";
        return false;
    }
    Names ident;
    QueryResult words = db.Query("SELECT word FROM pg_get_keywords() WHERE catcode IN ('R', 'T') ORDER BY word");
    for (int i = 0; words.Ok() && i < words.Rows(); ++i) ident.reserved.push_back(words.Text(i, 0));
    std::sort(ident.reserved.begin(), ident.reserved.end());
    for (const std::string& folder : folders) {
        const std::string schema = Lower(folder) == "sro_vt_shard" ? options.shardSchema : Lower(folder);
        if (!ValidSchemaName(schema)) {
            error = "not a schema name: " + schema;
            return false;
        }
        std::vector<Table> tables;
        if (!ReadSchema(options.dir / folder / "schema.tsv", tables, error)) return false;
        if (!db.Query(std::format("CREATE SCHEMA IF NOT EXISTS {}", schema)).Ok()) {
            error = "schema " + schema;
            return false;
        }
        core::LogInfo("import: {} -> {}: {} tables", folder, schema, tables.size());
        for (const Table& t : tables) {
            ImportReport::Table& row = report.tables.emplace_back();
            row.schema = schema;
            row.name = t.name;
            const std::string qualified = schema + "." + ident(t.name);
            if (!PrepareTable(db, schema, t, ident, row)) continue;
            const std::filesystem::path data = options.dir / folder / "data" / (t.source + ".tsv");
            if (!std::filesystem::exists(data)) continue; // its schema only (the export holds no rows of it)
            row.expected = CountRows(data);
            std::ifstream in(data, std::ios::binary);
            std::string header;
            if (!ReadLine(in, header)) {
                row.problem = "no header line";
                continue;
            }
            // The file's columns, as the COPY's list (its order, which may not be the table's).
            std::vector<std::string> names = SplitTabs(header);
            std::vector<bool> bytes(names.size(), false);
            std::string list;
            for (std::size_t i = 0; i < names.size(); ++i) {
                names[i] = Lower(names[i]);
                list += (i ? ", " : "") + ident(names[i]);
                for (const Column& c : t.columns) {
                    if (c.name == names[i] && c.type == "bytea") bytes[i] = true;
                }
            }
            if (!row.problem.empty()) continue;
            const bool anyBytes = std::find(bytes.begin(), bytes.end(), true) != bytes.end();
            std::string copyError;
            const std::int64_t loaded = LoadRows(
                db, qualified, t, list, options.replace, ident,
                        [&](std::string& piece) {
                            if (!anyBytes) {
                                // Whole blocks: the export's text is COPY's.
                                piece.resize(1 << 20);
                                in.read(piece.data(), std::streamsize(piece.size()));
                                piece.resize(std::size_t(in.gcount()));
                                return bool(in);
                            }
                            // Binary cells: the export's 0x.. is COPY's \\x.. (its backslash escaped).
                            std::string line;
                            if (!ReadLine(in, line)) return false;
                            std::vector<std::string> cells = SplitTabs(line);
                            for (std::size_t i = 0; i < cells.size() && i < bytes.size(); ++i) {
                                if (bytes[i] && cells[i].starts_with("0x")) cells[i] = "\\\\x" + cells[i].substr(2);
                            }
                            for (std::size_t i = 0; i < cells.size(); ++i) piece += (i ? "\t" : "") + cells[i];
                            piece += '\n';
                            return true;
                        },
                copyError);
            if (loaded < 0) {
                row.problem = copyError;
                continue;
            }
            row.loaded = loaded;
            if (loaded != row.expected) row.problem = std::format("{} rows loaded of the file's {}", loaded, row.expected);
        }
    }
    return true;
}

bool ImportsRowsOf(std::string_view table) {
    const std::string name = Lower(std::string(table));
    if (name.starts_with("tab_refranking")) return false; // the jobs' rankings: players
    if (name == "_refdummyslot") return false;            // our migrations fill it (the numbers 0..239)
    if (name.starts_with("_ref") || name.starts_with("tab_ref")) return true;
    return name == "_schedule" || name == "_itemquotation" || name == "_associationreputation" || name == "_trijobrewards";
}

bool ImportBackup(Database& db, const SqlServerBackup& backup, const BackupImportOptions& options, ImportReport& report, std::string& error) {
    const std::string schema = options.shardSchema;
    if (!ValidSchemaName(schema)) {
        error = "not a schema name: " + schema;
        return false;
    }
    if (backup.Tables().empty()) {
        error = "the backup has no tables";
        return false;
    }
    if (!db.Query("SET client_encoding TO 'UTF8'").Ok() || !db.Query(std::format("CREATE SCHEMA IF NOT EXISTS {}", schema)).Ok()) {
        error = "cannot prepare the schema " + schema;
        return false;
    }
    Names ident;
    QueryResult words = db.Query("SELECT word FROM pg_get_keywords() WHERE catcode IN ('R', 'T') ORDER BY word");
    for (int i = 0; words.Ok() && i < words.Rows(); ++i) ident.reserved.push_back(words.Text(i, 0));
    std::sort(ident.reserved.begin(), ident.reserved.end());
    core::LogInfo("import: the backup -> {}: {} tables", schema, backup.Tables().size());
    for (const BackupTable& source : backup.Tables()) {
        Table t;
        t.source = source.name;
        t.name = Lower(source.owner == "dbo" ? source.name : source.owner + "_" + source.name);
        for (const BackupColumn& bc : source.columns) {
            Column c;
            c.name = Lower(bc.name);
            c.type = ImportedType(bc.type, bc.length, bc.precision, bc.scale);
            if (c.type.empty()) {
                error = std::format("{}.{} has a type the import does not know: {}", t.name, c.name, bc.type);
                return false;
            }
            c.notNull = !bc.nullable;
            c.identity = bc.identity && (c.type == "integer" || c.type == "bigint" || c.type == "smallint");
            c.key = bc.keyOrdinal > 0;
            c.keyOrdinal = bc.keyOrdinal;
            c.defaultValue = ImportedDefault(bc.defaultValue);
            t.columns.push_back(std::move(c));
        }
        ImportReport::Table& row = report.tables.emplace_back();
        row.schema = schema;
        row.name = t.name;
        if (!PrepareTable(db, schema, t, ident, row) || !ImportsRowsOf(source.name)) continue;
        std::vector<std::string> lines;
        std::string readError;
        if (!backup.ReadRows(source, [&](std::string&& line) { lines.push_back(std::move(line)); }, readError)) {
            row.problem = readError;
            continue;
        }
        row.expected = std::int64_t(lines.size());
        if (lines.empty()) continue;
        std::string list;
        for (std::size_t i = 0; i < t.columns.size(); ++i) list += (i ? ", " : "") + ident(t.columns[i].name);
        std::size_t next = 0;
        std::string copyError;
        const std::int64_t loaded = LoadRows(
            db, schema + "." + ident(t.name), t, list, options.replace, ident,
            [&](std::string& piece) {
                // About a megabyte a piece.
                while (next < lines.size() && piece.size() < (1u << 20)) {
                    piece += lines[next++];
                    piece += '\n';
                }
                return !piece.empty();
            },
            copyError);
        if (loaded < 0) {
            row.problem = copyError;
            continue;
        }
        row.loaded = loaded;
        if (loaded != row.expected) row.problem = std::format("{} rows loaded of the backup's {}", loaded, row.expected);
    }
    return true;
}

bool HasImportedShard(Database& db, const std::string& shardSchema) {
    if (!ValidSchemaName(shardSchema)) return false;
    QueryResult there = db.Query("SELECT to_regclass($1) IS NOT NULL", {shardSchema + "._refobjcommon"});
    if (!there.Ok() || !there.Bool(0, 0)) return false;
    QueryResult rows = db.Query(std::format("SELECT EXISTS (SELECT 1 FROM {}._refobjcommon)", shardSchema));
    return rows.Ok() && rows.Bool(0, 0);
}

int AdoptShardAccounts(Database& db, const std::string& shardSchema) {
    if (!ValidSchemaName(shardSchema)) return 0;
    QueryResult made = db.Query(std::format(
        "INSERT INTO {1}.tb_user (jid, struserid, password, regtime) "
        "SELECT DISTINCT ON (x.userjid) x.userjid, left(coalesce(a.accountid, 'account' || x.userjid), 25), '', localtimestamp "
        "FROM {0}._user x LEFT JOIN {0}._accountjid a ON a.jid = x.userjid "
        "WHERE NOT EXISTS (SELECT 1 FROM {1}.tb_user u WHERE u.jid = x.userjid) ORDER BY x.userjid "
        "ON CONFLICT DO NOTHING",
        shardSchema, kAccountSchema));
    if (!made.Ok()) return 0;
    db.Query(std::format("SELECT setval(pg_get_serial_sequence('{0}.tb_user', 'jid'), coalesce((SELECT max(jid) FROM {0}.tb_user), 0) + 1, false)",
                         kAccountSchema));
    // An account made here is in the database as the starting silk's migration found the others: it gets the same
    // (SilkStore.h kStartingSilk), unless it has a silk row already.
    if (made.Affected() > 0) {
        QueryResult without = db.Query(std::format("SELECT u.jid FROM {1}.tb_user u JOIN {0}._user x ON x.userjid = u.jid "
                                                   "WHERE NOT EXISTS (SELECT 1 FROM {1}.sk_silk s WHERE s.jid = u.jid) GROUP BY u.jid",
                                                   shardSchema, kAccountSchema));
        for (int r = 0; without.Ok() && r < without.Rows(); ++r) {
            std::string error;
            db.Transaction([&](Database& tx) { return GiveStartingSilk(tx, without.Int(r, 0), error); }, error);
        }
    }
    return made.Affected();
}

} // namespace sr::server
