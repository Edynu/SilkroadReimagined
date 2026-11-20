#include "sr/server/GatewayServer.h"

#include <format>
#include <map>

#include "sr/core/Log.h"
#include "sr/core/Trace.h"
#include "sr/game/TitleProtocol.h"
#include "sr/server/Store.h"

namespace sr::server {

namespace proto = game::proto;

struct GatewayServer::State {
    GatewayConfig config;
    std::unique_ptr<net::NetService> net;
    std::unique_ptr<net::Listener> listener;
    std::unique_ptr<Completions> completions;
    DatabasePool database;
    std::unique_ptr<Peers> peers;

    proto::ServerList list;
    proto::Notices notices;
    bool refreshing = false;
    Clock::time_point nextRefresh;

    void Apply(const std::vector<GameServerInfo>& servers, const std::vector<NoticeRow>& rows) {
        proto::ServerList fresh;
        std::map<std::uint8_t, std::string> groups;
        // A shard is one row, however many game servers run it.
        for (const GameServerInfo& s : ShardsOf(servers)) {
            proto::ServerEntry e;
            e.id = s.id;
            e.name = s.name;
            e.group = s.group;
            e.online = std::uint16_t(std::min<std::uint32_t>(s.online, 0xFFFF));
            e.capacity = std::uint16_t(std::min<std::uint32_t>(s.capacity, 0xFFFF));
            e.state = !s.reporting ? proto::ServerState::Offline
                                   : (s.online >= s.capacity ? proto::ServerState::Full : proto::ServerState::Open);
            fresh.servers.push_back(std::move(e));
            groups.emplace(s.group, s.groupName);
        }
        for (const auto& [id, name] : groups) fresh.groups.push_back({id, name});
        fresh.loginHost = config.loginHost;
        fresh.loginPort = config.loginPort;
        list = std::move(fresh);
        notices.notices.clear();
        for (const NoticeRow& n : rows) notices.notices.push_back({n.title, n.body, n.posted});
    }

    // The lists from the database, on a database thread; applied by the loop.
    void Refresh() {
        refreshing = true;
        database.Submit([this, stale = config.staleSeconds, count = config.noticeCount](Database& db) {
            bool serversOk = false, noticesOk = false;
            auto servers = ListGameServers(db, stale, serversOk);
            auto rows = LatestNotices(db, count, noticesOk);
            completions->Post([this, servers = std::move(servers), rows = std::move(rows), ok = serversOk && noticesOk] {
                refreshing = false;
                nextRefresh = Clock::now() + config.refresh;
                if (ok) Apply(servers, rows);
            });
        });
    }

    void Handle(Peer& peer, const net::Message& m) {
        switch (m.id) {
        case proto::ServerListRequest::kId: SendMessage(*peer.connection, list); return;
        case proto::NoticesRequest::kId: SendMessage(*peer.connection, notices); return;
        default:
            core::LogInfo("{}: {} sent message 0x{:04x}, which the gateway does not take; closing", config.name, peer.address, m.id);
            peer.connection->Close();
        }
    }
};

GatewayServer::GatewayServer() = default;
GatewayServer::~GatewayServer() { Stop(); }

bool GatewayServer::Start(const GatewayConfig& config, std::string& error) {
    Stop();
    auto state = std::make_unique<State>();
    state->config = config;
    {
        auto db = Database::Connect(config.databaseUrl, error);
        if (!db) {
            error = "database: " + error;
            return false;
        }
        if (!MigrateSchema(*db, error)) return false;
        bool serversOk = false, noticesOk = false;
        const auto servers = ListGameServers(*db, config.staleSeconds, serversOk);
        const auto notices = LatestNotices(*db, config.noticeCount, noticesOk);
        if (!serversOk || !noticesOk) {
            error = "database: cannot read the server list";
            return false;
        }
        state->Apply(servers, notices);
    }
    if (!state->database.Start(config.databaseUrl, 1, error)) {
        error = "database: " + error;
        return false;
    }
    state->net = std::make_unique<net::NetService>(config.networkThreads);
    state->completions = std::make_unique<Completions>(*state->net);
    state->listener = state->net->Listen(config.listen, error);
    if (!state->listener) return false;
    state->peers = std::make_unique<Peers>(config.name, config.peers);
    state->nextRefresh = Clock::now() + config.refresh;
    core::LogInfo("{}: listening on {}:{}", config.name, config.listen.address, state->listener->Port());
    state_ = std::move(state);
    return true;
}

void GatewayServer::Poll(std::chrono::milliseconds wait) {
    if (!state_) return;
    SR_ZONE("GatewayServer::Poll");
    State& s = *state_;
    s.net->WaitForActivity(std::min(wait, std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::max(Clock::duration::zero(), s.nextRefresh - Clock::now()))));
    s.completions->Run();
    if (!s.refreshing && Clock::now() >= s.nextRefresh) s.Refresh();
    s.peers->Pump(*s.listener, [&](Peer& peer, const net::Message& m) { s.Handle(peer, m); });
}

void GatewayServer::Run(const std::atomic<bool>& stop) {
    while (!stop) Poll(std::chrono::milliseconds(100));
}

void GatewayServer::Stop() {
    if (!state_) return;
    state_->peers->CloseAll();
    state_->listener->Close();
    state_->database.Stop(); // waits for a refresh in flight; its completion is dropped with the state
    state_.reset();
}

std::uint16_t GatewayServer::Port() const { return state_ ? state_->listener->Port() : 0; }
std::size_t GatewayServer::Connections() const { return state_ ? state_->peers->Count() : 0; }

} // namespace sr::server
