#pragma once

// PostgreSQL through libpq. A Database is one connection, used by one
// thread at a time; DatabasePool gives each of its threads its own. Queries take their values as
// parameters (never pasted into the SQL), in text form; byte strings travel as bytea hex.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sr::server {

// One query parameter: text, a number, bytes, or NULL.
class Param {
public:
    Param(std::nullptr_t) {}
    Param(std::string_view text) : value_(std::string(text)) {}
    Param(const std::string& text) : value_(text) {}
    Param(const char* text) : value_(std::string(text)) {}
    Param(std::int64_t number) : value_(std::to_string(number)) {}
    Param(int number) : value_(std::to_string(number)) {}
    Param(unsigned number) : value_(std::to_string(number)) {}
    // A real, written so that it reads back to the same float (shortest round trip).
    Param(double number);
    static Param Bytes(const std::vector<std::uint8_t>& bytes);
    const std::optional<std::string>& Value() const { return value_; }

private:
    std::optional<std::string> value_;
};

class QueryResult {
public:
    bool Ok() const { return ok_; }
    const std::string& Error() const { return error_; }
    // The SQLSTATE of a failure ("23505" a unique violation, ...); empty on success.
    const std::string& State() const { return state_; }
    int Rows() const { return rows_; }
    int Columns() const { return columns_; }
    int Affected() const { return affected_; }

    bool IsNull(int row, int column) const;
    std::string Text(int row, int column) const;
    // The same without a copy (valid while the result lives; empty for NULL).
    std::string_view View(int row, int column) const;
    std::int64_t Int(int row, int column) const;
    bool Bool(int row, int column) const;
    double Double(int row, int column) const;
    std::vector<std::uint8_t> Bytes(int row, int column) const;

private:
    friend class Database;
    bool ok_ = false;
    std::string error_, state_;
    int rows_ = 0, columns_ = 0, affected_ = 0;
    std::vector<std::optional<std::string>> cells_; // row-major
};

class Database {
public:
    // `url`: a libpq connection string or postgresql:// URL. Null on failure (`error` says why).
    static std::unique_ptr<Database> Connect(const std::string& url, std::string& error);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    QueryResult Query(std::string_view sql, std::initializer_list<Param> params = {});
    QueryResult Query(std::string_view sql, const std::vector<Param>& params);
    // Several statements, no parameters (schema migrations).
    QueryResult Script(std::string_view sql);
    // Runs `body` inside BEGIN / COMMIT; rolls back if it returns false or a statement fails. Inside another transaction
    // (a store function called within a larger one) it is a savepoint: its work undone alone when `body` fails, kept or
    // undone with the outer transaction otherwise.
    bool Transaction(const std::function<bool(Database&)>& body, std::string& error);
    // A COPY ... FROM STDIN statement fed by `next`, which fills its argument with the next piece of
    // the data (COPY's text format) and returns false when there is no more. The rows taken, or -1
    // (`error` says why).
    std::int64_t CopyIn(std::string_view copySql, const std::function<bool(std::string& piece)>& next, std::string& error);
    // Whether the connection is still usable (reconnects once if it is not).
    bool Healthy();

    // PostgreSQL's LISTEN / NOTIFY, the bus a shard's game servers meet on:
    // this connection listens on `channel` (a plain name); what arrived since the last call, without
    // waiting (a dedicated connection, polled by its server's loop). Sending is Notify (below), on
    // any connection; a NOTIFY inside a transaction arrives when it commits.
    bool Listen(std::string_view channel);
    struct Notification {
        std::string channel, payload;
    };
    std::vector<Notification> TakeNotifications();

private:
    explicit Database(void* connection, std::string url) : connection_(connection), url_(std::move(url)) {}
    QueryResult Collect(void* result); // takes a PGresult
    void* connection_; // PGconn
    std::string url_;
    int transactionDepth_ = 0; // Transaction's nesting: 0 outside one
    std::vector<std::string> listening_; // listened to again after a reconnect
};

// Database work off the caller's thread (persistence): `threads` dedicated
// threads, each with its own connection (database calls block, so they are not jobs). Each task
// runs on whichever thread is free.
class DatabasePool {
public:
    DatabasePool() = default;
    ~DatabasePool();
    DatabasePool(const DatabasePool&) = delete;
    DatabasePool& operator=(const DatabasePool&) = delete;

    bool Start(const std::string& url, unsigned threads, std::string& error);
    void Stop();
    void Submit(std::function<void(Database&)> task);
    // Runs once every task submitted before it has finished, and alone: what it reads is what they wrote.
    void SubmitBarrier(std::function<void(Database&)> task);
    std::size_t Waiting() const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

// A notification on `channel` (at most 7,999 bytes of payload; listeners get it at the commit).
bool Notify(Database& db, std::string_view channel, std::string_view payload);

// `url` with the schemas a connection looks in for names without one (PostgreSQL's search_path),
// e.g. "sro_vt_shard2, sro_vt_account" for a game server on another shard.
std::string WithSearchPath(const std::string& url, std::string_view schemas);

// The original's layout (server/common/src/Schema.cpp): the account schema
// (sro_vt_account) and a shard's (sro_vt_shard by default), the tables the servers use and our own
// sr_ tables; applies the migrations not yet applied, in order, each in its own transaction, under
// an advisory lock so servers starting together do not race. A new database also gets the default
// search path "sro_vt_shard, sro_vt_account" (this connection too).
inline constexpr const char* kAccountSchema = "sro_vt_account";
inline constexpr const char* kDefaultShardSchema = "sro_vt_shard";
bool MigrateSchema(Database& db, std::string& error);
bool MigrateSchema(Database& db, const std::string& shardSchema, std::string& error);
// The versions applied to the account schema and to a shard's, and the latest of each.
int SchemaVersion(Database& db);
int ShardSchemaVersion(Database& db, const std::string& shardSchema = kDefaultShardSchema);
int LatestSchemaVersion();
int LatestShardSchemaVersion();
// Whether `name` can be a schema's name (lower-case letters, digits and '_', not first a digit).
bool ValidSchemaName(std::string_view name);

} // namespace sr::server
