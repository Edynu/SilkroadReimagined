// SroControl.exe - the front machine's program: the gateway, the login server
// and the shard's control in one process. The game servers are programs of their own, usually on
// other machines. Options: sr/server/Program.h (--config FILE: the shard's settings, ShardLayout.h), and
//   gateway-port (15779), login-port (15780)   where they listen
//   login-host (127.0.0.1)                     the address clients are told to log in at
//   --address-attempts N, --legacy-passwords   as SroLogin.exe
//   --workers N                                password-check jobs (default: the hardware's)
//   --status                                   prints the shard's game servers once and exits
//
// The control's part so far: it says which game servers are up and notices when one stops reporting.
// Starting and restarting them (from the settings) comes later.

#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <mutex>

#include "sr/core/Jobs.h"
#include "sr/core/Log.h"
#include "sr/server/Database.h"
#include "sr/server/GatewayServer.h"
#include "sr/server/InstanceStore.h"
#include "sr/server/LoginServer.h"
#include "sr/server/Program.h"
#include "sr/server/ShardLayout.h"
#include "sr/server/Store.h"

namespace {

using namespace sr;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr int kStaleSeconds = 15; // as the gateway's and the login server's

// A game server as the settings call it (East, West), else as it reports itself.
std::string Describe(const server::GameServerInfo& s, const server::ShardLayout& layout) {
    const server::ShardServer* row = layout.Server(s.id);
    return std::format("{} (server {}) at {}:{}, {} of {} players", row != nullptr ? row->name : s.name, s.id, s.host, s.port, s.online, s.capacity);
}

// Asks the database who is up, a few seconds apart, without holding up the loop; says what changed.
class ShardWatch {
public:
    explicit ShardWatch(const server::ShardLayout& layout) : layout_(layout) {}
    bool Start(const std::string& url, std::string& error) { return pool_.Start(url, 1, error); }
    void Tick() {
        Apply();
        if (asking_ || Clock::now() < nextAsk_) return;
        asking_ = true;
        pool_.Submit([this](server::Database& db) {
            bool ok = false;
            auto servers = server::ListGameServers(db, kStaleSeconds, ok);
            const std::lock_guard lock(mutex_);
            fresh_ = std::move(servers);
            ready_ = ok;
            answered_ = true;
        });
    }
    void Stop() { pool_.Stop(); }

private:
    void Apply() {
        std::vector<server::GameServerInfo> servers;
        {
            const std::lock_guard lock(mutex_);
            if (!answered_) return;
            answered_ = false;
            asking_ = false;
            nextAsk_ = Clock::now() + 5s;
            if (!ready_) return core::LogWarning("control: cannot read the game servers from the database");
            servers = std::move(fresh_);
        }
        for (const server::GameServerInfo& s : servers) {
            const auto was = up_.find(s.id);
            if (was != up_.end() && was->second == s.reporting) continue;
            if (s.reporting) {
                core::LogInfo("control: {} is up", Describe(s, layout_));
            } else if (was != up_.end()) {
                core::LogWarning("control: {} has stopped reporting", Describe(s, layout_));
            }
            up_[s.id] = s.reporting;
        }
    }

    const server::ShardLayout& layout_;
    server::DatabasePool pool_;
    std::mutex mutex_;
    std::vector<server::GameServerInfo> fresh_;
    bool ready_ = false, answered_ = false, asking_ = false;
    Clock::time_point nextAsk_{};
    std::map<std::uint16_t, bool> up_;
};

int Status(const server::ProgramOptions& options, const server::ShardLayout& layout) {
    std::string error;
    auto db = server::Database::Connect(options.databaseUrl, error);
    if (!db) {
        core::LogError("SroControl: {}", error);
        return 1;
    }
    bool ok = false;
    const auto servers = server::ListGameServers(*db, kStaleSeconds, ok);
    if (!ok) {
        core::LogError("SroControl: cannot read the game servers");
        return 1;
    }
    for (const server::GameServerInfo& s : servers) core::LogInfo("{}: {}", s.reporting ? "up  " : "down", Describe(s, layout));
    if (servers.empty()) core::LogInfo("no game server has reported yet");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    server::ProgramOptions options;
    std::string error;
    if (!server::ReadProgramOptions(argc, argv, "SroControl", 15779, options, error)) {
        core::LogError("SroControl: {}", error);
        return 1;
    }
    const core::Config& c = options.config;
    server::ShardLayout layout;
    if (!server::ShardLayout::Read(c, layout, error)) {
        core::LogError("SroControl: {}", error);
        return 1;
    }
    if (c.Has("status")) return Status(options, layout);
    for (const server::ShardServer& s : layout.Servers()) {
        std::string regions;
        for (const std::string& r : s.regions) regions += (regions.empty() ? "" : ", ") + r;
        core::LogInfo("SroControl: {} (server {}) runs {}", s.name, s.id, regions);
    }
    if (!server::LoadProgramCertificate(options, error)) {
        core::LogError("SroControl: {}", error);
        return 1;
    }

    // The shard starts: its characters' instance entries emptied, as the original's shard manager does at its start
    // (_GameWorldInitialize): the instances lived in the game servers' memory, the day's counts begin again.
    {
        const std::string shard = c.GetString("shard-schema", server::kDefaultShardSchema);
        auto db = server::Database::Connect(server::WithSearchPath(options.databaseUrl, shard + ", " + server::kAccountSchema), error);
        if (!db || !server::ClearInstanceEntries(*db, error)) {
            core::LogWarning("SroControl: the instance entries were not emptied: {}", error);
            error.clear();
        } else {
            core::LogInfo("SroControl: the characters' instance entries emptied (the shard's start)");
        }
    }
    core::JobSystem jobs(c.GetInt("workers", core::JobSystem::kDefaultWorkers));
    server::GatewayConfig gatewayConfig;
    gatewayConfig.databaseUrl = options.databaseUrl;
    gatewayConfig.listen = options.listen;
    gatewayConfig.peers.messagesPerSecond = options.messagesPerSecond;
    gatewayConfig.peers.messageBurst = options.messageBurst;
    gatewayConfig.peers.clientGate = options.clientGate;
    gatewayConfig.listen.port = std::uint16_t(c.GetInt("gateway-port", 15779));
    gatewayConfig.loginHost = c.GetString("login-host", "127.0.0.1");
    gatewayConfig.loginPort = std::uint16_t(c.GetInt("login-port", 15780));
    server::GatewayServer gateway;
    if (!gateway.Start(gatewayConfig, error)) {
        core::LogError("SroControl: gateway: {}", error);
        return 1;
    }

    server::LoginConfig loginConfig;
    loginConfig.databaseUrl = options.databaseUrl;
    loginConfig.jobs = &jobs;
    loginConfig.listen = options.listen;
    loginConfig.peers.messagesPerSecond = options.messagesPerSecond;
    loginConfig.peers.messageBurst = options.messageBurst;
    loginConfig.peers.clientGate = options.clientGate;
    loginConfig.listen.port = gatewayConfig.loginPort;
    loginConfig.addressAttempts = c.GetInt("address-attempts", loginConfig.addressAttempts);
    loginConfig.legacyPasswords = c.GetString("legacy-passwords", "keep") == "clear" ? server::LegacyPasswords::Clear : server::LegacyPasswords::Keep;
    server::LoginServer login;
    if (!login.Start(loginConfig, error)) {
        core::LogError("SroControl: login: {}", error);
        return 1;
    }

    ShardWatch watch(layout);
    if (!watch.Start(options.databaseUrl, error)) {
        core::LogError("SroControl: {}", error);
        return 1;
    }
    core::LogInfo("SroControl: gateway on :{}, login on :{}", gateway.Port(), login.Port());
    server::InstallStopHandler();
    // One thread turns them all: what they wait for is the network and the database, and the password
    // checks are jobs.
    while (!server::StopRequested().load()) {
        gateway.Poll(2ms);
        login.Poll(2ms);
        watch.Tick();
    }
    watch.Stop();
    login.Stop();
    gateway.Stop();
    core::LogInfo("SroControl: stopped");
    return 0;
}
