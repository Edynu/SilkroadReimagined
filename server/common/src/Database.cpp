#include "sr/server/Database.h"

#include <libpq-fe.h>

#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <format>
#include <mutex>
#include <thread>

#include "sr/core/Log.h"
#include "sr/core/Trace.h"

namespace sr::server {

namespace {

constexpr char kHex[] = "0123456789abcdef";

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

PGconn* Pg(void* p) { return static_cast<PGconn*>(p); }

} // namespace

Param Param::Bytes(const std::vector<std::uint8_t>& bytes) {
    std::string text = "\\x";
    text.reserve(2 + bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        text += kHex[b >> 4];
        text += kHex[b & 15];
    }
    return Param(std::string_view(text));
}

bool QueryResult::IsNull(int row, int column) const {
    return !cells_[std::size_t(row) * std::size_t(columns_) + std::size_t(column)].has_value();
}

std::string QueryResult::Text(int row, int column) const {
    const auto& cell = cells_[std::size_t(row) * std::size_t(columns_) + std::size_t(column)];
    return cell ? *cell : std::string();
}

std::string_view QueryResult::View(int row, int column) const {
    const auto& cell = cells_[std::size_t(row) * std::size_t(columns_) + std::size_t(column)];
    return cell ? std::string_view(*cell) : std::string_view();
}

std::int64_t QueryResult::Int(int row, int column) const {
    const std::string text = Text(row, column);
    return text.empty() ? 0 : std::strtoll(text.c_str(), nullptr, 10);
}

bool QueryResult::Bool(int row, int column) const { return Text(row, column) == "t"; }

double QueryResult::Double(int row, int column) const {
    const std::string text = Text(row, column);
    return text.empty() ? 0.0 : std::strtod(text.c_str(), nullptr);
}

Param::Param(double number) {
    char text[64];
    const auto end = std::to_chars(text, text + sizeof(text), number).ptr;
    value_ = std::string(text, end);
}

std::vector<std::uint8_t> QueryResult::Bytes(int row, int column) const {
    const std::string text = Text(row, column);
    std::vector<std::uint8_t> out;
    if (text.size() < 2 || text[0] != '\\' || text[1] != 'x') return out;
    out.reserve((text.size() - 2) / 2);
    for (std::size_t i = 2; i + 1 < text.size(); i += 2) {
        const int hi = HexValue(text[i]), lo = HexValue(text[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(std::uint8_t(hi << 4 | lo));
    }
    return out;
}

std::unique_ptr<Database> Database::Connect(const std::string& url, std::string& error) {
    PGconn* connection = PQconnectdb(url.c_str());
    if (!connection || PQstatus(connection) != CONNECTION_OK) {
        error = connection ? PQerrorMessage(connection) : "out of memory";
        while (!error.empty() && (error.back() == '\n' || error.back() == '\r')) error.pop_back();
        if (connection) PQfinish(connection);
        return nullptr;
    }
    // Quiet the server's NOTICE lines (CREATE ... IF NOT EXISTS and similar).
    PQsetNoticeProcessor(connection, [](void*, const char*) {}, nullptr);
    return std::unique_ptr<Database>(new Database(connection, url));
}

Database::~Database() {
    if (connection_) PQfinish(Pg(connection_));
}

bool Database::Listen(std::string_view channel) {
    for (char c : channel) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false; // a plain identifier
    }
    if (!Query(std::format("LISTEN {}", channel)).Ok()) return false;
    listening_.emplace_back(channel);
    return true;
}

std::vector<Database::Notification> Database::TakeNotifications() {
    std::vector<Notification> out;
    PGconn* c = Pg(connection_);
    if (PQconsumeInput(c) == 0) {
        Healthy();
        return out;
    }
    while (PGnotify* n = PQnotifies(c)) {
        out.push_back({n->relname != nullptr ? n->relname : "", n->extra != nullptr ? n->extra : ""});
        PQfreemem(n);
    }
    return out;
}

bool Notify(Database& db, std::string_view channel, std::string_view payload) {
    if (payload.size() >= 8000) return false;
    return db.Query("SELECT pg_notify($1, $2)", {channel, payload}).Ok();
}

bool Database::Healthy() {
    if (PQstatus(Pg(connection_)) == CONNECTION_OK) return true;
    PQreset(Pg(connection_));
    if (PQstatus(Pg(connection_)) != CONNECTION_OK) return false;
    for (const std::string& channel : listening_) PQclear(PQexec(Pg(connection_), std::format("LISTEN {}", channel).c_str()));
    return true;
}

QueryResult Database::Query(std::string_view sql, std::initializer_list<Param> params) {
    return Query(sql, std::vector<Param>(params));
}

QueryResult Database::Query(std::string_view sql, const std::vector<Param>& params) {
    SR_ZONE("Database::Query");
    std::vector<const char*> values;
    values.reserve(params.size());
    for (const Param& p : params) values.push_back(p.Value() ? p.Value()->c_str() : nullptr);
    const std::string text(sql);

    PGresult* r = PQexecParams(Pg(connection_), text.c_str(), int(values.size()), nullptr, values.data(), nullptr, nullptr, 0);
    if (!r && !Healthy()) {
        r = PQexecParams(Pg(connection_), text.c_str(), int(values.size()), nullptr, values.data(), nullptr, nullptr, 0);
    }
    return Collect(r);
}

QueryResult Database::Script(std::string_view sql) {
    SR_ZONE("Database::Script");
    const std::string text(sql);
    return Collect(PQexec(Pg(connection_), text.c_str()));
}

QueryResult Database::Collect(void* pgResult) {
    PGresult* r = static_cast<PGresult*>(pgResult);
    QueryResult result;
    if (!r) {
        result.error_ = PQerrorMessage(Pg(connection_));
        return result;
    }
    const ExecStatusType status = PQresultStatus(r);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        result.error_ = PQresultErrorMessage(r);
        while (!result.error_.empty() && (result.error_.back() == '\n' || result.error_.back() == '\r')) result.error_.pop_back();
        if (const char* state = PQresultErrorField(r, PG_DIAG_SQLSTATE)) result.state_ = state;
        PQclear(r);
        return result;
    }
    result.ok_ = true;
    result.rows_ = PQntuples(r);
    result.columns_ = PQnfields(r);
    if (const char* affected = PQcmdTuples(r); affected && *affected) result.affected_ = std::atoi(affected);
    result.cells_.reserve(std::size_t(result.rows_) * std::size_t(result.columns_));
    for (int row = 0; row < result.rows_; ++row) {
        for (int column = 0; column < result.columns_; ++column) {
            if (PQgetisnull(r, row, column)) {
                result.cells_.emplace_back();
            } else {
                result.cells_.emplace_back(std::string(PQgetvalue(r, row, column), std::size_t(PQgetlength(r, row, column))));
            }
        }
    }
    PQclear(r);
    return result;
}

bool Database::Transaction(const std::function<bool(Database&)>& body, std::string& error) {
    struct Depth {
        int& depth;
        explicit Depth(int& d) : depth(d) { ++depth; }
        ~Depth() { --depth; }
    };
    if (transactionDepth_ > 0) {
        // Within another transaction: a savepoint (a nested BEGIN would do nothing and its COMMIT end the outer one).
        const std::string savepoint = "sr_nested_" + std::to_string(transactionDepth_);
        QueryResult begin = Query("SAVEPOINT " + savepoint);
        if (!begin.Ok()) {
            error = begin.Error();
            return false;
        }
        bool ok = false;
        {
            const Depth nested(transactionDepth_);
            ok = body(*this);
        }
        if (!ok) {
            Query("ROLLBACK TO SAVEPOINT " + savepoint);
            if (error.empty()) error = "the transaction was rolled back";
            return false;
        }
        QueryResult release = Query("RELEASE SAVEPOINT " + savepoint);
        if (!release.Ok()) {
            error = release.Error();
            return false;
        }
        return true;
    }
    QueryResult begin = Query("BEGIN");
    if (!begin.Ok()) {
        error = begin.Error();
        return false;
    }
    bool ok = false;
    {
        const Depth outer(transactionDepth_);
        ok = body(*this);
    }
    if (!ok) {
        Query("ROLLBACK");
        if (error.empty()) error = "the transaction was rolled back";
        return false;
    }
    QueryResult commit = Query("COMMIT");
    if (!commit.Ok()) {
        error = commit.Error();
        Query("ROLLBACK");
        return false;
    }
    return true;
}

std::int64_t Database::CopyIn(std::string_view copySql, const std::function<bool(std::string& piece)>& next, std::string& error) {
    SR_ZONE("Database::CopyIn");
    const std::string text(copySql);
    PGresult* start = PQexec(Pg(connection_), text.c_str());
    if (!start || PQresultStatus(start) != PGRES_COPY_IN) {
        error = start ? PQresultErrorMessage(start) : PQerrorMessage(Pg(connection_));
        while (!error.empty() && (error.back() == '\n' || error.back() == '\r')) error.pop_back();
        if (start) PQclear(start);
        return -1;
    }
    PQclear(start);
    std::string piece;
    bool sent = true;
    while (sent) {
        piece.clear();
        const bool more = next(piece);
        if (!piece.empty() && PQputCopyData(Pg(connection_), piece.data(), int(piece.size())) != 1) sent = false;
        if (!more) break;
    }
    if (PQputCopyEnd(Pg(connection_), sent ? nullptr : "the data could not be sent") != 1) sent = false;
    std::int64_t rows = -1;
    while (PGresult* r = PQgetResult(Pg(connection_))) {
        if (PQresultStatus(r) == PGRES_COMMAND_OK) {
            const char* affected = PQcmdTuples(r);
            rows = affected && *affected ? std::strtoll(affected, nullptr, 10) : 0;
        } else {
            error = PQresultErrorMessage(r);
            while (!error.empty() && (error.back() == '\n' || error.back() == '\r')) error.pop_back();
            rows = -1;
        }
        PQclear(r);
    }
    if (!sent && error.empty()) error = PQerrorMessage(Pg(connection_));
    return sent ? rows : -1;
}

std::string WithSearchPath(const std::string& url, std::string_view schemas) {
    std::string path;
    for (char c : schemas) {
        if (c != ' ') path += c;
    }
    if (url.starts_with("postgresql://") || url.starts_with("postgres://")) {
        // options=-c search_path=a,b in the URL's query (',' and '=' encoded).
        std::string encoded = "-c%20search_path%3D";
        for (char c : path) encoded += c == ',' ? std::string("%2C") : std::string(1, c);
        return url + (url.find('?') == std::string::npos ? "?" : "&") + "options=" + encoded;
    }
    return url + " options='-c search_path=" + path + "'";
}

struct DatabasePool::Impl {
    struct Task {
        std::function<void(Database&)> run;
        bool barrier = false;
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Task> tasks;
    bool stopping = false;
    int running = 0;
    bool barrierRunning = false;
    std::vector<std::thread> threads;
    // The next task may start: one that must run alone waits for those before it, and nothing starts beside it.
    bool CanStart() const { return !tasks.empty() && !barrierRunning && (!tasks.front().barrier || running == 0); }
};

DatabasePool::~DatabasePool() { Stop(); }

bool DatabasePool::Start(const std::string& url, unsigned threads, std::string& error) {
    Stop();
    std::vector<std::unique_ptr<Database>> connections;
    for (unsigned i = 0; i < (threads == 0 ? 1 : threads); ++i) {
        auto db = Database::Connect(url, error);
        if (!db) return false;
        connections.push_back(std::move(db));
    }
    impl_ = std::make_shared<Impl>();
    for (std::size_t i = 0; i < connections.size(); ++i) {
        impl_->threads.emplace_back([impl = impl_.get(), db = std::shared_ptr<Database>(std::move(connections[i])), i] {
            const std::string name = std::format("Database {}", i);
            core::SetThreadName(name.c_str());
            for (;;) {
                Impl::Task task;
                {
                    std::unique_lock lock(impl->mutex);
                    impl->changed.wait(lock, [&] { return impl->CanStart() || (impl->stopping && impl->tasks.empty()); });
                    if (impl->tasks.empty()) return; // stopping, and every task done
                    task = std::move(impl->tasks.front());
                    impl->tasks.pop_front();
                    ++impl->running;
                    impl->barrierRunning = task.barrier;
                }
                task.run(*db);
                {
                    std::lock_guard lock(impl->mutex);
                    --impl->running;
                    if (task.barrier) impl->barrierRunning = false;
                }
                impl->changed.notify_all(); // a barrier may have been waiting for this one
            }
        });
    }
    return true;
}

void DatabasePool::Stop() {
    if (!impl_) return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->changed.notify_all();
    for (std::thread& t : impl_->threads) t.join();
    impl_.reset();
}

void DatabasePool::Submit(std::function<void(Database&)> task) {
    if (!impl_) return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->tasks.push_back({std::move(task), false});
    }
    impl_->changed.notify_one();
}

void DatabasePool::SubmitBarrier(std::function<void(Database&)> task) {
    if (!impl_) return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->tasks.push_back({std::move(task), true});
    }
    impl_->changed.notify_all();
}

std::size_t DatabasePool::Waiting() const {
    if (!impl_) return 0;
    std::lock_guard lock(impl_->mutex);
    return impl_->tasks.size();
}

} // namespace sr::server
