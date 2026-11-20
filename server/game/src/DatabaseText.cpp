#include "sr/server/DatabaseText.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sr/assets/GameData.h"
#include "sr/core/Path.h"
#include "sr/core/Log.h"
#include "sr/core/Text.h"

namespace sr::server {

namespace {

constexpr std::string_view kTextData = "server_dep\\silkroad\\textdata\\";

// One client table: its file name (every shard: the name, or the name and digits / '_'), the
// database tables whose columns make its columns in order (each without the columns the client
// lacks), and the client columns that name a row.
struct Part {
    const char* alias;
    const char* table;
    std::vector<const char*> without; // column names the client lacks
    const char* withoutPrefix = nullptr; // and every column starting so
};
struct Spec {
    const char* file;
    std::vector<Part> parts;
    const char* join; // how the parts' rows meet (empty: one part)
    std::vector<int> key;
    // A list file naming the shards (skilldataenc.txt: the client's encrypted skilldata_<N>enc.txt, read decrypted,
    // assets::DecryptTextTable); none: the plain shards named `file`.
    const char* list = nullptr;
};

const std::vector<Spec>& Specs() {
    // Measured 2026-11-07 over every row of the 1.188 export against the 1.188 client.
    static const std::vector<Spec> kSpecs = {
        {"itemdata", {{"c", "_refobjcommon", {"link"}}, {"x", "_refobjitem", {"id", "link"}}}, "x.id = c.link", {1}},
        {"characterdata", {{"c", "_refobjcommon", {"link"}}, {"x", "_refobjchar", {"id", "link"}, "resist"}}, "x.id = c.link", {1}},
        {"teleportbuilding", {{"c", "_refobjcommon", {"link"}}, {"x", "_refobjstruct", {"id", "link"}}}, "x.id = c.link", {1}},
        {"skilldata", {{"s", "_refskill", {}}}, "", {1}},
        // The 1.188 client's own skill shards, encrypted: rebuilt too, as plain text (the
        // skill table takes the socket stones' skills from them); measured 2026-11-12 equal to _refskill in every row.
        {"skilldata", {{"s", "_refskill", {}}}, "", {1}, "skilldataenc.txt"},
        {"teleportdata", {{"t", "_refteleport", {"assocrefobjcodename128", "bindinteractionmask", "fixedservice"}}}, "", {1}},
        {"teleportlink", {{"t", "_reftelelink", {"checkresult"}}}, "", {1, 2}},
        {"refshopgroup", {{"t", "_refshopgroup", {}}}, "", {2}},
        {"refmappingshopgroup", {{"t", "_refmappingshopgroup", {}}}, "", {2, 3}},
        {"refmappingshopwithtab", {{"t", "_refmappingshopwithtab", {}}}, "", {2, 3}},
        {"refshoptabgroup", {{"t", "_refshoptabgroup", {}}}, "", {2}},
        {"refshoptab", {{"t", "_refshoptab", {}}}, "", {2}}, // the client's has one more column of its own
        {"refshopgoods", {{"t", "_refshopgoods", {}}}, "", {2, 3}},
        {"refpricepolicyofitem", {{"t", "_refpricepolicyofitem", {}}}, "", {2, 3}},
        {"refscrapofpackageitem", {{"t", "_refscrapofpackageitem", {}}}, "", {2, 3}},
        {"refpackageitem", {{"t", "_refpackageitem", {}}}, "", {2}},
        // Magic POP: the client's files are the database's rows in service.
        {"gachaitemset", {{"t", "_refgachaitemset", {}}}, "", {5}},
        {"gachanpcmap", {{"t", "_refgachanpcmap", {}}}, "", {1}},
        // The item sets: the client's 42 rows are the database's, every column (measured
        // 2026-11-16; the export's lost names aside).
        {"refsetitemgroup", {{"t", "_refsetitemgroup", {}}}, "", {1}},
        // The Devil's Spirit by plus: the client's 90 / 250 / 154 rows are the database's,
        // every column (measured 2026-11-16).
        {"refabilitybyitemoptleveldata", {{"t", "_refabilitybyitemoptlevel", {}}}, "", {1}},
        {"refmagicoptbyitemoptleveldata", {{"t", "_refmagicoptbyitemoptlevel", {}}}, "", {0, 1}},
        {"refskillbyitemoptleveldata", {{"t", "_refskillbyitemoptlevel", {}}}, "", {0, 1}},
        // The magic options and the drops' blue lists: the client's
        // 391 / 74 rows are the database's in service, every column (measured 2026-11-16).
        {"magicoption", {{"t", "_refmagicopt", {}}}, "", {1}},
        {"magicoptionassign", {{"t", "_refmagicoptassign", {}}}, "", {1, 2, 3}},
        {"leveldata", {{"t", "_reflevel", {}}}, "", {0}},
        {"questdata", {{"t", "_refquest", {}}}, "", {1}},
    };
    return kSpecs;
}

std::string Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.remove_suffix(1);
    return std::string(s);
}

// A cell as a value: numbers as the float32 the tables hold (so "1", "1.0" and "1e+00" agree), text as it is.
std::string Value(std::string_view cell) {
    const std::string s = Trim(cell);
    if (s.empty()) return s;
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (end != nullptr && *end == '\0') {
        char text[40];
        std::snprintf(text, sizeof(text), "%.9g", double(float(d)));
        return std::string("#") + text;
    }
    return s;
}

bool FileOf(std::string_view normalized, std::string_view name) {
    if (!normalized.starts_with(kTextData) || !normalized.ends_with(".txt")) return false;
    const std::string_view file = normalized.substr(kTextData.size(), normalized.size() - kTextData.size() - 4);
    if (!file.starts_with(name)) return false;
    const std::string_view rest = file.substr(name.size());
    return rest.find_first_not_of("0123456789_") == std::string_view::npos;
}

class MemorySource final : public assets::IFileSource {
public:
    void Add(std::string path, std::string text) {
        std::vector<std::byte> bytes(text.size() + 3);
        bytes[0] = std::byte{0xEF};
        bytes[1] = std::byte{0xBB};
        bytes[2] = std::byte{0xBF}; // UTF-8, said so
        std::memcpy(bytes.data() + 3, text.data(), text.size());
        files_[core::NormalizeAssetPath(path)] = std::move(bytes);
        paths_.push_back(std::move(path));
    }
    std::string_view Name() const override { return "the database"; }
    bool Contains(const std::string& normalizedPath) const override { return files_.contains(normalizedPath); }
    std::optional<assets::FileData> Read(const std::string& normalizedPath) const override {
        const auto it = files_.find(normalizedPath);
        if (it == files_.end()) return std::nullopt;
        return assets::FileData::View(it->second);
    }
    void ListFiles(std::vector<std::string>& out) const override { out.insert(out.end(), paths_.begin(), paths_.end()); }
    bool Empty() const { return files_.empty(); }

private:
    std::unordered_map<std::string, std::vector<std::byte>> files_;
    std::vector<std::string> paths_;
};

// What the client draws with, the server never: model, icon and effect paths (AssocFile*, UI_*).
// The client's values stay (measured 2026-11-07: the 1.188 client's drop models and icons differ
// from the database's in 85 places, all its own).
bool Presentation(std::string_view column) { return column.starts_with("assocfile") || column.starts_with("ui_"); }

// Text the export lost: its Korean came through as '?' and some signs changed with it (CLAUDE.md,
// Sources; questdata's descriptions: '|' as U+00A6); the client has it as it was.
bool LostText(std::string_view database, std::string_view) { return database.find('?') != std::string_view::npos; }

// The SELECT giving a spec's rows in the client's column order, each column as text; `names` the
// columns, `qualified` each as the SQL names it, `from` the tables.
bool SelectOf(Database& db, const Spec& spec, std::string& sql, std::vector<std::string>& names, std::vector<std::string>& qualified,
              std::string& from, std::string& error) {
    std::string columns;
    for (const Part& part : spec.parts) {
        QueryResult listed = db.Query("SELECT column_name FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = $1 "
                                      "ORDER BY ordinal_position",
                                      {part.table});
        if (!listed.Ok() || listed.Rows() == 0) {
            error = std::string(part.table) + " is not in the database";
            return false;
        }
        for (int i = 0; i < listed.Rows(); ++i) {
            const std::string name = listed.Text(i, 0);
            if (std::find(part.without.begin(), part.without.end(), name) != part.without.end()) continue;
            if (part.withoutPrefix != nullptr && name.starts_with(part.withoutPrefix)) continue;
            std::string quoted = "\"";
            for (char c : name) quoted += c == '"' ? std::string("\"\"") : std::string(1, c);
            quoted += "\"";
            columns += std::format("{}{}.{}::text", columns.empty() ? "" : ", ", part.alias, quoted);
            names.push_back(name);
            qualified.push_back(std::format("{}.{}", part.alias, quoted));
        }
        from += std::format("{}{} {}", from.empty() ? "" : " JOIN ", part.table, part.alias);
        if (&part != &spec.parts.front()) from += std::string(" ON ") + spec.join;
    }
    sql = std::format("SELECT {} FROM {}", columns, from);
    return true;
}

// A line's tab-separated cells as views (`out` reused between lines).
void Cells(std::string_view line, std::vector<std::string_view>& out) {
    out.clear();
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = line.find('\t', start);
        out.push_back(line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) return;
        start = end + 1;
    }
}

// A data row: not a comment, its first cell a number (the service flag, a level), long enough for the key.
bool DataRow(std::string_view line, const std::vector<std::string_view>& cells, int need) {
    return !line.starts_with("//") && int(cells.size()) > need && !cells[0].empty() &&
           std::all_of(cells[0].begin(), cells[0].end(), [](unsigned char c) { return std::isdigit(c) || c == ' '; });
}

} // namespace

std::unique_ptr<assets::IFileSource> MakeDatabaseText(Database& db, const assets::FileSystem& game, DatabaseTextCounts& counts, std::string& error) {
    auto source = std::make_unique<MemorySource>();
    // The text tables among the game's files (normalized once).
    std::vector<std::pair<std::string, std::string>> textFiles; // path, normalized
    for (const std::string& path : game.ListFolder(kTextData)) textFiles.emplace_back(path, path);
    std::vector<std::string> missing;
    std::vector<std::string_view> cells;
    for (const Spec& spec : Specs()) {
        std::string sql, from;
        std::vector<std::string> names, qualified;
        if (!SelectOf(db, spec, sql, names, qualified, from, error)) {
            missing.push_back(spec.file);
            continue;
        }
        const int need = *std::max_element(spec.key.begin(), spec.key.end());
        std::vector<bool> presentation(names.size());
        for (std::size_t c = 0; c < names.size(); ++c) presentation[c] = Presentation(names[c]);
        // The client's files first: the rows it has are the ones asked for (a table keyed by one number
        // asks for those numbers only: the database's later content is not fetched).
        std::vector<std::pair<std::string, std::string>> texts;
        std::string wanted = "{";
        bool numeric = spec.key.size() == 1;
        std::vector<std::string> files = spec.list != nullptr ? assets::ListedShards(game, spec.list) : std::vector<std::string>{};
        if (spec.list == nullptr) {
            for (const auto& [path, normalized] : textFiles) {
                if (FileOf(normalized, spec.file)) files.push_back(path);
            }
        }
        for (const std::string& path : files) {
            const auto file = game.Read(path);
            if (!file) continue;
            const std::span<const std::byte> raw = file->Bytes();
            std::vector<std::byte> bytes(raw.begin(), raw.end());
            texts.emplace_back(path, assets::DecryptTextTable(bytes) ? core::DecodeTextFile(bytes) : core::DecodeTextFile(raw));
            if (!numeric) continue;
            const std::string& text = texts.back().second;
            for (std::size_t start = 0; start < text.size();) {
                std::size_t end = text.find('\n', start);
                if (end == std::string::npos) end = text.size();
                std::string_view line(text.data() + start, end - start);
                start = end + 1;
                if (line.ends_with('\r')) line.remove_suffix(1);
                Cells(line, cells);
                if (!DataRow(line, cells, need)) continue;
                const std::string key = Trim(cells[std::size_t(spec.key[0])]);
                if (key.empty() || !std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isdigit(c) || c == '-'; })) {
                    numeric = false;
                    continue;
                }
                wanted += (wanted.size() > 1 ? "," : "") + key;
            }
        }
        wanted += "}";
        QueryResult rows = numeric ? db.Query(sql + " WHERE " + qualified[std::size_t(spec.key[0])] + " = ANY($1::bigint[])", {wanted}) : db.Query(sql);
        QueryResult total = db.Query("SELECT count(*) FROM " + from);
        if (!rows.Ok() || !total.Ok()) {
            error = std::format("{}: {}", spec.file, rows.Ok() ? total.Error() : rows.Error());
            return nullptr;
        }
        // The database's rows by their key.
        std::unordered_map<std::string, int> byKey;
        byKey.reserve(std::size_t(rows.Rows()));
        for (int r = 0; r < rows.Rows(); ++r) {
            std::string key;
            for (int k : spec.key) key += Value(rows.View(r, k)) + "\t";
            byKey.emplace(std::move(key), r);
        }
        std::unordered_set<int> used;
        std::vector<std::pair<int, std::string_view>> changes; // column, the database's value
        for (auto& [path, text] : texts) {
            std::string out;
            out.reserve(text.size() + text.size() / 16);
            for (std::size_t start = 0; start < text.size();) {
                std::size_t end = text.find('\n', start);
                if (end == std::string::npos) end = text.size();
                std::string_view line(text.data() + start, end - start);
                start = end + 1;
                if (line.ends_with('\r')) line.remove_suffix(1);
                Cells(line, cells);
                const bool row = DataRow(line, cells, need);
                auto it = byKey.end();
                if (row) {
                    std::string key;
                    for (int k : spec.key) key += Value(cells[std::size_t(k)]) + "\t";
                    it = byKey.find(key);
                }
                if (!row) {
                    // A comment, the header, an index's file name: as it is.
                } else if (it == byKey.end()) {
                    ++counts.clientOnly;
                } else {
                    used.insert(it->second);
                    ++counts.rows;
                    // The database's value where it differs; the client's own text where they agree (and in
                    // its own columns past the database's, where the database has NULL, in what only it
                    // draws with, where the export lost the text).
                    changes.clear();
                    const int columns = std::min(int(cells.size()), rows.Columns());
                    for (int c = 0; c < columns; ++c) {
                        const std::string_view value = rows.View(it->second, c);
                        const std::string_view mine = cells[std::size_t(c)];
                        if (value == mine || presentation[std::size_t(c)] || rows.IsNull(it->second, c)) continue;
                        if (Value(value) == Value(mine) || LostText(value, mine)) continue;
                        changes.emplace_back(c, value);
                        if (counts.samples.size() < 40) {
                            counts.samples.push_back(std::format("{} {} {}: {} -> {}", spec.file, cells[std::size_t(spec.key[0])], names[std::size_t(c)], mine, value));
                        }
                        ++counts.changed;
                        ++counts.byColumn[std::string(spec.file) + "." + names[std::size_t(c)]];
                    }
                    if (!changes.empty()) {
                        std::size_t next = 0;
                        for (std::size_t c = 0; c < cells.size(); ++c) {
                            if (c) out += '\t';
                            if (next < changes.size() && changes[next].first == int(c)) {
                                out += changes[next++].second;
                            } else {
                                out += cells[c];
                            }
                        }
                        out += "\r\n";
                        continue;
                    }
                }
                out += line;
                out += "\r\n";
            }
            source->Add(path, std::move(out));
            ++counts.files;
        }
        counts.databaseOnly += int(total.Int(0, 0)) - int(used.size());
    }
    if (!missing.empty()) {
        error = "not in the database: ";
        for (std::size_t i = 0; i < missing.size(); ++i) error += (i ? ", " : "") + missing[i];
        if (source->Empty()) return nullptr;
        core::LogWarning("database text: {} (the client's files serve them)", error);
    }
    return source;
}

} // namespace sr::server
