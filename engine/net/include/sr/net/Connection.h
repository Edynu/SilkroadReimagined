#pragma once

// Connections between the client and our servers, and between our servers: TCP with TLS 1.3,
// on Asio and OpenSSL, which stay inside the implementation.
//
// A NetService owns the network threads (the only threads besides the job system that touch
// sockets). Connections and listeners are made by it; their methods may be
// called from any thread. Received messages wait in the connection until its owner takes
// them (Receive), typically once per tick; WaitForActivity lets a server sleep until
// something arrives.
//
// The interface is the transport's boundary: a UDP transport could implement Connection later
// without the game code noticing (the latest lane is then unreliable and sequenced).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sr/net/Framing.h"

namespace sr::net {

enum class ConnectionState : std::uint8_t { Connecting, Open, Closed };

struct ConnectionLimits {
    // The largest frame (id + payload) this side accepts; a larger one closes the connection.
    std::uint32_t maxFrame = 64 * 1024;
    // Unsent bytes allowed before the connection is closed as too slow to read.
    std::size_t maxPendingBytes = 4u << 20;
    // Received bytes not yet taken by Receive before the connection is closed (a flooding peer).
    std::size_t maxReceivedBytes = 4u << 20;
    // Connecting, the TLS handshake included, must finish within this.
    std::chrono::milliseconds handshakeTimeout{10'000};
};

struct ConnectionStats {
    std::uint64_t messagesSent = 0; // handed to Send or SendLatest (latest-lane replacements included)
    std::uint64_t messagesReceived = 0;
    std::uint64_t bytesSent = 0;     // frames, before TLS
    std::uint64_t bytesReceived = 0; // frames, after TLS
    std::uint64_t latestReplaced = 0;
};

// One message of a batch (Connection::SendMany): latestKey 0 the reliable lane, else the latest lane with that key.
struct OutgoingMessage {
    MessageId id = 0;
    std::span<const std::byte> payload;
    std::uint64_t latestKey = 0;
};

class Connection {
public:
    virtual ~Connection() = default;

    // Reliable lane: delivered, in order. Ignored once the connection is closed.
    virtual void Send(MessageId id, std::span<const std::byte> payload) = 0;
    // Latest lane: while it waits to be written, a newer message with the same key replaces it.
    virtual void SendLatest(std::uint64_t key, MessageId id, std::span<const std::byte> payload) = 0;
    // Several at once, in order, each on its lane: one lock and one wake of the writer for all of them (a server's
    // messages to one player on a tick), where Send and SendLatest take one each.
    virtual void SendMany(std::span<const OutgoingMessage> messages) {
        for (const OutgoingMessage& m : messages) {
            if (m.latestKey != 0) {
                SendLatest(m.latestKey, m.id, m.payload);
            } else {
                Send(m.id, m.payload);
            }
        }
    }
    // Appends every message received so far to `out`; returns how many.
    virtual std::size_t Receive(std::vector<Message>& out) = 0;

    virtual ConnectionState State() const = 0;
    // Why it closed (empty while it is not closed).
    virtual std::string CloseReason() const = 0;
    // Closes now; unsent messages are dropped.
    virtual void Close() = 0;
    // Closes once every message sent so far is written (a refusal, then goodbye).
    virtual void CloseAfterSend() = 0;

    virtual std::string RemoteAddress() const = 0;
    virtual ConnectionStats Stats() const = 0;

    // Tests only: writes `bytes` into the stream as they are, without a frame.
    virtual void SendRawForTest(std::span<const std::byte> bytes) = 0;
};

class Listener {
public:
    virtual ~Listener() = default;
    // The port it listens on (the one chosen by the system when asked for port 0).
    virtual std::uint16_t Port() const = 0;
    // Appends the connections accepted since the last call (each has finished its TLS
    // handshake and is Open); returns how many.
    virtual std::size_t Accept(std::vector<std::shared_ptr<Connection>>& out) = 0;
    // Connections refused or dropped before they were handed out (a failed handshake, a timeout,
    // over the connection limit).
    virtual std::uint64_t Rejected() const = 0;
    virtual void Close() = 0;
};

struct TlsServerConfig {
    std::string certificatePem; // the server's certificate
    std::string privateKeyPem;
};

struct TlsClientConfig {
    // The certificate the server must present a chain to (a CA, or the server's own
    // self-signed certificate, pinned).
    std::string trustedCertificatePem;
    // The name the server's certificate must carry; also sent as SNI.
    std::string serverName;
};

struct ListenConfig {
    std::string address = "0.0.0.0";
    std::uint16_t port = 0;
    TlsServerConfig tls;
    ConnectionLimits limits;
    // Connections open at the same time; more are refused.
    std::size_t maxConnections = 10'000;
    // From one address (loopback exempt; 0: no limit): connections open at once, handshakes
    // included, and new connections within a minute. More are closed at accept, before TLS.
    std::size_t maxPerAddress = 0;
    std::size_t acceptsPerMinute = 0;
};

// 127.0.0.0/8, ::1 or ::ffff:127.x (the address without its port).
bool IsLoopbackAddress(std::string_view address);

struct ConnectConfig {
    std::string host;
    std::uint16_t port = 0;
    TlsClientConfig tls;
    ConnectionLimits limits;
};

class NetService {
public:
    // `threads`: network threads (a client needs 1; a server a few).
    explicit NetService(unsigned threads = 1);
    // Closes every connection and listener and joins the threads.
    ~NetService();
    NetService(const NetService&) = delete;
    NetService& operator=(const NetService&) = delete;

    // Starts connecting; the connection is Connecting, then Open or Closed (with the reason).
    std::shared_ptr<Connection> Connect(const ConnectConfig& config);
    // Null when it cannot listen (`error` says why).
    std::unique_ptr<Listener> Listen(const ListenConfig& config, std::string& error);

    // Sleeps until a message arrives, a connection is accepted or one changes state, or the
    // timeout passes. True if something happened since the previous call.
    bool WaitForActivity(std::chrono::milliseconds timeout);
    // Counts as activity: wakes a WaitForActivity (work finished elsewhere, for example).
    void Wake();

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

// A self-signed certificate (an EC P-256 key; the name as subject and DNS name) for development
// and tests. Production servers use a real certificate.
bool MakeSelfSignedCertificate(const std::string& name, int validDays, std::string& certificatePem,
                               std::string& privateKeyPem, std::string& error);

} // namespace sr::net
