#pragma once

// What every server loop shares: work finished on other threads comes back
// to the loop (Completions); each connection must greet first (Peers: Hello within a deadline,
// the version check, then the server's own messages); attempts per address are limited; the TLS
// certificate is loaded, or made for development.

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "sr/game/Protocol.h"
#include "sr/net/Connection.h"

namespace sr::server {

using Clock = std::chrono::steady_clock;

// Functions posted from any thread, run by the server loop (Run). Posting wakes the loop.
class Completions {
public:
    explicit Completions(net::NetService& net) : net_(net) {}
    void Post(std::function<void()> fn);
    // Runs every function posted so far; returns how many.
    std::size_t Run();

private:
    net::NetService& net_;
    std::mutex mutex_;
    std::vector<std::function<void()>> queue_;
};

struct Peer {
    std::uint64_t id = 0;
    std::shared_ptr<net::Connection> connection;
    std::string address; // without the port
    bool greeted = false;
    Clock::time_point lastHeard;
    bool busy = false;        // a request of this peer is being worked on (servers use it)
    std::int64_t account = 0; // the signed-in account, once a server knows it
    std::int64_t session = 0; // a game server's: the token its account's online session is held by (0: none)
    // Its message budget (PeerLimits): what is left, and when it was last filled.
    double budget = 0.0;
    Clock::time_point budgetAt;
    bool budgetStarted = false;
    bool dropped = false; // closed by the loop (a flood, a handler's exception): nothing more of it is handled
};

struct PeerLimits {
    std::chrono::milliseconds helloTimeout{10'000};
    std::chrono::milliseconds idleTimeout{120'000};
    // Messages a connection may send: a bucket of `messageBurst`, filled `messagesPerSecond` a second. A peer
    // that empties it is closed (a player's clicks stay far below; a flood would stall the loop for everyone).
    // 0: no budget. Loopback peers are exempt unless `budgetLoopback` (the tests, the bot benchmark).
    double messagesPerSecond = 50.0;
    double messageBurst = 200.0;
    bool budgetLoopback = false;
};

// The connections of one listener, from accept to close.
class Peers {
public:
    Peers(std::string serverName, PeerLimits limits) : serverName_(std::move(serverName)), limits_(limits) {}

    // Takes new connections, answers Hello, closes peers that break the rules or time out, and
    // calls `handle(peer, message)` for every other message of a greeted peer.
    void Pump(net::Listener& listener, const std::function<void(Peer&, const net::Message&)>& handle);
    // The peer with this id, if it is still connected (work finished later finds it again).
    Peer* Find(std::uint64_t id);
    std::size_t Count() const { return peers_.size(); }
    void CloseAll();
    // Called once for each peer that goes away (it closed, or CloseAll), before it is forgotten.
    void OnClosed(std::function<void(Peer&)> fn) { onClosed_ = std::move(fn); }

private:
    std::string serverName_;
    PeerLimits limits_;
    std::uint64_t nextId_ = 1;
    std::unordered_map<std::uint64_t, Peer> peers_;
    std::vector<std::shared_ptr<net::Connection>> accepted_;
    std::vector<net::Message> messages_;
    std::function<void(Peer&)> onClosed_;
};

// Sends one message of our protocol.
template <typename T>
void SendMessage(net::Connection& connection, const T& message) {
    connection.Send(T::kId, game::proto::Encode(message));
}

// At most `count` attempts per address within `window`.
class AttemptLimiter {
public:
    AttemptLimiter(int count, std::chrono::seconds window) : count_(count), window_(window) {}
    // Records an attempt; false if the address is over its limit (the attempt is not counted).
    bool Allow(const std::string& address, Clock::time_point now = Clock::now());

private:
    int count_;
    std::chrono::seconds window_;
    std::unordered_map<std::string, std::deque<Clock::time_point>> attempts_;
    Clock::time_point lastSweep_;
};

// Reads the certificate and key (PEM files). With `createIfMissing`, makes a self-signed pair for
// `name` and writes them first (development: the client pins that certificate).
bool LoadCertificate(const std::string& certificatePath, const std::string& keyPath, bool createIfMissing,
                     const std::string& name, net::TlsServerConfig& out, std::string& error);

} // namespace sr::server
