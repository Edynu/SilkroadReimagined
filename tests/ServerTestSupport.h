#pragma once

// Helpers of the server tests: a temporary database per test (SR_DB_URL's role, which needs
// CREATEDB; without SR_DB_URL the tests that need it are skipped), a test certificate, and a
// client that greets, sends and waits while it turns the servers' loops.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sr/assets/GameData.h"
#include "sr/assets/Terrain.h"
#include "sr/core/Config.h"
#include "sr/game/Protocol.h"
#include "sr/net/Connection.h"
#include "sr/server/Database.h"
#include "sr/server/ShardLayout.h"
#include "sr/server/Store.h"

#ifdef _WIN32
#include <process.h>
#define SR_GETPID _getpid
#else
#include <unistd.h>
#define SR_GETPID getpid
#endif

namespace sr::test {

using namespace std::chrono_literals;
namespace proto = sr::game::proto;

// The original's data a test may read, never in the repository (README "Tests"): SR_VSRO_EXPORT an export
// of the shard's backup made through SQL Server (<db>/schema.tsv, <db>/data/<table>.tsv); SR_QUEST_SCRIPT the
// server's Quest.sct, else SR_VSRO_DATA's. An empty path when unset: the tests that need it skip.
inline std::filesystem::path EnvPath(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v ? std::filesystem::path(v) : std::filesystem::path();
}
inline std::filesystem::path ExportDir() { return EnvPath("SR_VSRO_EXPORT"); }
inline std::filesystem::path QuestScriptPath() {
    if (std::filesystem::path p = EnvPath("SR_QUEST_SCRIPT"); !p.empty()) return p;
    const std::filesystem::path data = EnvPath("SR_VSRO_DATA");
    return data.empty() ? data : data / "Quest.sct";
}

// The world in two game servers: region kEast is server 1's, kWest server 2's; the
// teleport points 1 and 2 are a developer's way to each.
inline const std::uint16_t kEast = assets::RegionId(168, 97), kWest = assets::RegionId(79, 105);

struct SplitWorld {
    std::shared_ptr<const server::ShardLayout> layout;
    std::shared_ptr<assets::TeleportTable> teleports;
    std::shared_ptr<const assets::SkillTable> skills; // optional
    template <typename Database>
    explicit SplitWorld(Database& t) {
        core::Config c;
        std::string error;
        REQUIRE(c.ParseText("server.1.regions = CHINA\nserver.2.regions = Eu\n", error));
        auto l = std::make_shared<server::ShardLayout>();
        REQUIRE(server::ShardLayout::Read(c, *l, error));
        layout = l;
        REQUIRE(t.Db().Query("CREATE TABLE sro_vt_shard._refregion (wregionid smallint, continentname varchar(32))").Ok());
        REQUIRE(t.Db().Query("INSERT INTO sro_vt_shard._refregion VALUES ($1, 'CHINA'), ($2, 'Eu')", {int(std::int16_t(kEast)), int(std::int16_t(kWest))}).Ok());
        const auto row = [](std::initializer_list<std::string> cells) {
            std::string out;
            for (const std::string& cell : cells) out += (out.empty() ? "" : "\t") + cell;
            return out + "\n";
        };
        teleports = std::make_shared<assets::TeleportTable>();
        teleports->Decode("", row({"1", "1", "GATE_E", "0", "SN_E", std::to_string(kEast), "1000", "0", "1000", "0", "0", "0", "1"}) +
                                  row({"1", "2", "GATE_W", "0", "SN_W", std::to_string(kWest), "1000", "0", "1000", "0", "0", "0", "1"}), "");
    }
    // Into a game server's settings.
    template <typename Config>
    void Apply(Config& config) const {
        config.layout = layout;
        config.teleports = teleports;
        config.devCommands = true;
        config.moveWait = 1s;
        if (skills) config.skills = skills;
    }
};


// A database of its own for each test, made through SR_DB_URL's role (it needs CREATEDB) and
// dropped afterwards. Without SR_DB_URL the database tests are skipped.
class TestDatabase {
public:
    TestDatabase() {
        const char* base = std::getenv("SR_DB_URL");
        if (!base || !*base) return;
        std::string error;
        admin_ = server::Database::Connect(base, error);
        REQUIRE_MESSAGE(admin_, error);
        static int counter = 0; // one per test file is enough: the names also carry a time
        name_ = std::format("sro_test_{}_{}_{}", SR_GETPID(), ++counter, std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
        admin_->Query("DROP DATABASE IF EXISTS " + name_ + " WITH (FORCE)");
        const server::QueryResult made = admin_->Query("CREATE DATABASE " + name_);
        REQUIRE_MESSAGE(made.Ok(), made.Error());
        url_ = WithDatabase(base, name_);
        db_ = server::Database::Connect(url_, error);
        REQUIRE_MESSAGE(db_, error);
        REQUIRE_MESSAGE(server::MigrateSchema(*db_, error), error);
    }
    ~TestDatabase() {
        db_.reset();
        if (admin_) admin_->Query("DROP DATABASE IF EXISTS " + name_ + " WITH (FORCE)");
    }
    bool Available() const { return db_ != nullptr; }
    server::Database& Db() { return *db_; }
    const std::string& Url() const { return url_; }

private:
    // postgresql://user:password@host:port/database?options -> the same with `database`.
    static std::string WithDatabase(std::string url, const std::string& database) {
        const std::size_t scheme = url.find("://");
        const std::size_t slash = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
        const std::size_t query = url.find('?');
        const std::string options = query == std::string::npos ? "" : url.substr(query);
        return (slash == std::string::npos ? url : url.substr(0, slash)) + "/" + database + options;
    }
    std::unique_ptr<server::Database> admin_, db_;
    std::string name_, url_;
};

#define SKIP_WITHOUT_DATABASE(db)                                        \
    if (!(db).Available()) {                                             \
        MESSAGE("SR_DB_URL is not set: database tests skipped");         \
        return;                                                          \
    }

struct TestCertificate {
    std::string pem, key;
    TestCertificate() {
        std::string error;
        REQUIRE(net::MakeSelfSignedCertificate("localhost", 1, pem, key, error));
    }
};
inline const TestCertificate& Certificate() {
    static const TestCertificate c;
    return c;
}

inline net::ListenConfig Local() {
    net::ListenConfig l;
    l.address = "127.0.0.1";
    l.tls = {Certificate().pem, Certificate().key};
    return l;
}

inline server::GameServerInfo TestServer(std::uint16_t id = 1, std::uint32_t online = 3, std::uint32_t capacity = 100) {
    server::GameServerInfo s;
    s.id = id;
    s.name = std::format("Server {}", id);
    s.group = 1;
    s.groupName = "Silkroad";
    s.host = "127.0.0.1";
    s.port = std::uint16_t(16000 + id);
    s.capacity = capacity;
    s.online = online;
    return s;
}

// A client of one server: connects, greets, then sends and waits for answers. `pump` turns the
// servers' loops while it waits.
class Client {
public:
    Client(net::NetService& net, std::uint16_t port, std::function<void()> pump) : pump_(std::move(pump)) {
        net::ConnectConfig c;
        c.host = "127.0.0.1";
        c.port = port;
        c.tls = {Certificate().pem, "localhost"};
        connection_ = net.Connect(c);
    }
    net::Connection& Connection() { return *connection_; }

    template <typename T>
    void Send(const T& message) {
        connection_->Send(T::kId, proto::Encode(message));
    }

    // Waits for the next message with T's id; false if the connection closed or time ran out.
    template <typename T>
    bool Expect(T& out, std::chrono::milliseconds timeout = 10s) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            for (std::size_t i = 0; i < inbox_.size(); ++i) {
                if (inbox_[i].id == T::kId) {
                    const bool ok = proto::Decode(inbox_[i].payload, out);
                    inbox_.erase(inbox_.begin() + std::ptrdiff_t(i));
                    return ok;
                }
            }
            if (connection_->State() == net::ConnectionState::Closed && inbox_.empty()) {
                connection_->Receive(inbox_);
                if (inbox_.empty()) return false;
                continue;
            }
            if (std::chrono::steady_clock::now() > end) return false;
            pump_();
            connection_->Receive(inbox_);
        }
    }

    bool Greet(std::uint32_t protocol = proto::kProtocolVersion) {
        proto::Hello hello;
        hello.protocol = protocol;
        hello.client = "sr_tests";
        Send(hello);
        proto::Welcome welcome;
        return Expect(welcome) && welcome.result == proto::WelcomeResult::Accepted;
    }

    // Greets as `client` (a players' client names its version): the server's whole answer.
    proto::Welcome GreetAs(const std::string& client) {
        proto::Hello hello;
        hello.client = client;
        Send(hello);
        proto::Welcome welcome;
        if (!Expect(welcome)) welcome.result = proto::WelcomeResult::Refused;
        return welcome;
    }

    bool WaitClosed(std::chrono::milliseconds timeout = 10s) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (connection_->State() != net::ConnectionState::Closed) {
            if (std::chrono::steady_clock::now() > end) return false;
            pump_();
        }
        return true;
    }

private:
    std::shared_ptr<net::Connection> connection_;
    std::function<void()> pump_;
    std::vector<net::Message> inbox_;
};


} // namespace sr::test
