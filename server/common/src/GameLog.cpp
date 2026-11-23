#include "sr/server/GameLog.h"

#include <chrono>
#include <ctime>
#include <format>
#include <fstream>
#include <sstream>

#include "sr/core/Log.h"
#include "sr/server/Database.h"

namespace sr::server {

namespace {

std::int64_t UnixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::tm Local(std::int64_t unix) {
    const std::time_t t = std::time_t(unix);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    return local;
}

// The server's local time (the original's GetDate()), as COPY reads a timestamp.
std::string Stamp(std::int64_t unix) {
    const std::tm t = Local(unix);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

// A text field for COPY's text format (and the spill's lines): its tabs, line ends and backslashes escaped; empty is
// NULL (the original's procedures leave an empty position or description out).
std::string Field(const std::string& s, std::size_t most) {
    if (s.empty()) return "\\N";
    std::string out;
    for (const char c : s.substr(0, most)) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

std::string Unfield(const std::string& s) {
    if (s == "\\N") return {};
    std::string out;
    for (std::size_t k = 0; k < s.size(); ++k) {
        if (s[k] != '\\' || k + 1 == s.size()) {
            out += s[k];
            continue;
        }
        const char c = s[++k];
        out += c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c;
    }
    return out;
}

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', start);
        out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) return out;
        start = tab + 1;
    }
}

std::string CharLine(const CharLogRow& r) {
    return std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\n", r.character, Stamp(r.at), int(r.event), r.data1, r.data2, Field(r.pos, 64), Field(r.desc, 128));
}
std::string ItemLine(const ItemLogRow& r) {
    return std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", Stamp(r.at), r.character, r.refId, r.data, int(r.storage), int(r.operation), int(r.from),
                       int(r.to), Field(r.pos, 64), Field(r.desc, 128), r.serial, r.gold);
}

} // namespace

std::string LogSchemaOf(const std::string& shardSchema) { return shardSchema == kDefaultShardSchema ? std::string("sro_vt_log") : shardSchema + "_log"; }

GameLog::GameLog(std::string schema, std::filesystem::path spill) : schema_(std::move(schema)), spill_(std::move(spill)) {}

void GameLog::Char(CharLogRow row) {
    if (row.at == 0) row.at = UnixNow();
    queued_.chars.push_back(std::move(row));
}

void GameLog::Item(ItemLogRow row) {
    if (row.at == 0) row.at = UnixNow();
    queued_.items.push_back(std::move(row));
}

GameLog::Batch GameLog::Take() { return std::exchange(queued_, Batch{}); }

bool GameLog::Spilled() const {
    std::lock_guard lock(writing_);
    std::error_code ec;
    return std::filesystem::exists(spill_, ec) && std::filesystem::file_size(spill_, ec) > 0;
}

bool GameLog::EnsureMonth(Database& tx, const std::string& table, std::int64_t at) {
    if (plain_.contains(table)) return true; // a table made before ours (an import's, with rows): no partitions
    if (!partitioned_.contains(table)) {
        const bool is = tx.Query("SELECT EXISTS (SELECT 1 FROM pg_partitioned_table WHERE partrelid = to_regclass($1))", {schema_ + "." + table}).Bool(0, 0);
        (is ? partitioned_ : plain_).insert(table);
        if (!is) return true;
    }
    const std::tm t = Local(at);
    const int year = t.tm_year + 1900, month = t.tm_mon + 1;
    if (months_.contains({table, year * 100 + month})) return true;
    const int nextYear = month == 12 ? year + 1 : year, nextMonth = month == 12 ? 1 : month + 1;
    // In a savepoint of its own: refused (the default partition holds rows of that month already), the month's rows go
    // to the default partition.
    std::string error;
    const bool made = tx.Transaction(
        [&](Database& sp) {
            return sp.Query(std::format("CREATE TABLE IF NOT EXISTS {0}.{1}_y{2:04}m{3:02} PARTITION OF {0}.{1} "
                                        "FOR VALUES FROM ('{2:04}-{3:02}-01') TO ('{4:04}-{5:02}-01')",
                                        schema_, table, year, month, nextYear, nextMonth))
                .Ok();
        },
        error);
    if (!made) core::LogWarning("game log: no partition of {} for {:04}-{:02} ({}): its rows in the default one", table, year, month, error);
    months_.insert({table, year * 100 + month});
    return true;
}

bool GameLog::WriteRows(Database& db, const Batch& batch, std::string& error) {
    return db.Transaction(
        [&](Database& tx) {
            // The months' partitions first.
            for (const CharLogRow& r : batch.chars)
                if (!EnsureMonth(tx, "_logeventchar", r.at)) return false;
            for (const ItemLogRow& r : batch.items) {
                if (!EnsureMonth(tx, "_logeventitem", r.at)) return false;
                if (r.operation == std::uint8_t(logs::ItemOp::BuyCash) && !EnsureMonth(tx, "_logcashitem", r.at)) return false;
            }
            if (!batch.chars.empty()) {
                std::size_t k = 0;
                const std::int64_t n = tx.CopyIn(std::format("COPY {}._logeventchar (charid, eventtime, eventid, data1, data2, eventpos, strdesc) FROM STDIN", schema_),
                                                 [&](std::string& piece) {
                                                     if (k == batch.chars.size()) return false;
                                                     piece = CharLine(batch.chars[k++]);
                                                     return true;
                                                 },
                                                 error);
                if (n < 0) return false;
            }
            if (!batch.items.empty()) {
                std::size_t k = 0;
                const std::int64_t n = tx.CopyIn(std::format("COPY {}._logeventitem (eventtime, charid, itemrefid, dwdata, targetstorage, operation, slot_from, "
                                                             "slot_to, eventpos, strdesc, serial64, gold) FROM STDIN",
                                                             schema_),
                                                 [&](std::string& piece) {
                                                     if (k == batch.items.size()) return false;
                                                     piece = ItemLine(batch.items[k++]);
                                                     return true;
                                                 },
                                                 error);
                if (n < 0) return false;
                // A mall purchase: its _LogCashItem row too (the original's _AddLogItem for operation 35).
                for (const ItemLogRow& r : batch.items) {
                    if (r.operation != std::uint8_t(logs::ItemOp::BuyCash)) continue;
                    const QueryResult cash = tx.Query(std::format("INSERT INTO {}._logcashitem (refitemid, charid, cnt, eventtime, serial64) "
                                                                  "VALUES ($1, $2, $3, $4::timestamp, $5)",
                                                                  schema_),
                                                      {std::int64_t(r.refId), r.character, std::int64_t(r.data), Stamp(r.at), r.serial});
                    if (!cash.Ok()) {
                        error = cash.Error();
                        return false;
                    }
                }
            }
            return true;
        },
        error);
}

void GameLog::ToSpill(const Batch& batch) {
    std::ofstream out(spill_, std::ios::binary | std::ios::app);
    for (const CharLogRow& r : batch.chars) out << "C\t" << r.at << '\t' << CharLine(r);
    for (const ItemLogRow& r : batch.items) out << "I\t" << r.at << '\t' << ItemLine(r);
    if (!out) core::LogWarning("game log: {} rows lost (the spill {} cannot be written)", batch.chars.size() + batch.items.size(), spill_.string());
}

GameLog::Batch GameLog::FromSpill() {
    Batch b;
    std::ifstream in(spill_, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> f = Split(line);
        const auto number = [](const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); };
        if (f.size() == 9 && f[0] == "C") {
            CharLogRow r;
            r.at = number(f[1]);
            r.character = number(f[2]);
            r.event = std::uint8_t(number(f[4]));
            r.data1 = std::int32_t(number(f[5]));
            r.data2 = std::int32_t(number(f[6]));
            r.pos = Unfield(f[7]);
            r.desc = Unfield(f[8]);
            b.chars.push_back(std::move(r));
        } else if (f.size() == 14 && f[0] == "I") {
            ItemLogRow r;
            r.at = number(f[1]);
            r.character = number(f[3]);
            r.refId = std::uint32_t(number(f[4]));
            r.data = std::int32_t(number(f[5]));
            r.storage = std::uint8_t(number(f[6]));
            r.operation = std::uint8_t(number(f[7]));
            r.from = std::uint8_t(number(f[8]));
            r.to = std::uint8_t(number(f[9]));
            r.pos = Unfield(f[10]);
            r.desc = Unfield(f[11]);
            r.serial = number(f[12]);
            r.gold = number(f[13]);
            b.items.push_back(std::move(r));
        }
    }
    return b;
}

bool GameLog::Write(Database& db, const Batch& batch) {
    std::lock_guard lock(writing_);
    std::error_code ec;
    const bool spilled = std::filesystem::exists(spill_, ec) && std::filesystem::file_size(spill_, ec) > 0;
    std::string error;
    if (spilled) {
        const Batch old = FromSpill();
        if (!WriteRows(db, old, error)) {
            ToSpill(batch); // still refused: this batch after the others
            core::LogWarning("game log: the database refuses the logs ({}): {} rows kept in {}", error, batch.chars.size() + batch.items.size(), spill_.string());
            return false;
        }
        std::filesystem::remove(spill_, ec);
        core::LogInfo("game log: {} spilled rows written", old.chars.size() + old.items.size());
    }
    if (batch.Empty()) return true;
    if (WriteRows(db, batch, error)) return true;
    ToSpill(batch);
    core::LogWarning("game log: the database refuses the logs ({}): {} rows kept in {}", error, batch.chars.size() + batch.items.size(), spill_.string());
    return false;
}

} // namespace sr::server
