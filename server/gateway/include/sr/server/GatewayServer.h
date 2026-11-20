#pragma once

// The gateway: where a client arrives. After Hello it answers the server
// list (the game servers as they report themselves in the database, and where to log in) and the
// notices. It keeps no game state; the lists are refreshed from the database in the background, so
// a request is answered at once.

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include "sr/net/Connection.h"
#include "sr/server/Database.h"
#include "sr/server/Service.h"

namespace sr::server {

struct GatewayConfig {
    std::string name = "SroGateway";
    net::ListenConfig listen;
    std::string databaseUrl;
    unsigned networkThreads = 2;
    // Where clients log in (sent with the server list).
    std::string loginHost = "127.0.0.1";
    std::uint16_t loginPort = 0;
    // A game server whose last report is older counts as offline.
    int staleSeconds = 15;
    std::chrono::milliseconds refresh{1000};
    int noticeCount = 5;
    PeerLimits peers;
};

class GatewayServer {
public:
    GatewayServer();
    ~GatewayServer();
    // Connects to the database (migrating its schema), loads the lists and listens.
    bool Start(const GatewayConfig& config, std::string& error);
    // One turn of the loop: waits up to `wait` for something to do, then does it.
    void Poll(std::chrono::milliseconds wait);
    // Polls until `stop` is set.
    void Run(const std::atomic<bool>& stop);
    void Stop();
    std::uint16_t Port() const;
    std::size_t Connections() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace sr::server
