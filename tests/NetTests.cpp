#include <doctest/doctest.h>

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "sr/game/Protocol.h"
#include "sr/net/Connection.h"
#include "sr/net/Framing.h"

using namespace std::chrono_literals;
using namespace sr::net;

namespace {

std::vector<std::byte> Bytes(std::string_view text) {
    const auto* p = reinterpret_cast<const std::byte*>(text.data());
    return {p, p + text.size()};
}

std::string Text(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::vector<std::byte> U32(std::uint32_t v) {
    std::vector<std::byte> out(4);
    for (int i = 0; i < 4; ++i) out[i] = std::byte((v >> (8 * i)) & 0xFF);
    return out;
}

// One certificate for every test (key generation is the slow part).
struct Certificate {
    std::string pem, key;
    Certificate() {
        std::string error;
        REQUIRE_MESSAGE(MakeSelfSignedCertificate("localhost", 1, pem, key, error), error);
    }
};
const Certificate& TestCertificate() {
    static const Certificate certificate;
    return certificate;
}

ListenConfig LocalListen() {
    ListenConfig config;
    config.address = "127.0.0.1";
    config.tls = {TestCertificate().pem, TestCertificate().key};
    return config;
}

ConnectConfig LocalConnect(std::uint16_t port) {
    ConnectConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.tls = {TestCertificate().pem, "localhost"};
    return config;
}

// Polls `done` until it holds or the time is up, sleeping on the services' activity.
template <typename F>
bool Until(NetService& net, F done, std::chrono::milliseconds timeout = 20s) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        net.WaitForActivity(5ms);
    }
    return true;
}

// Accepts connections and sends every message back.
struct EchoServer {
    std::unique_ptr<Listener> listener;
    std::vector<std::shared_ptr<Connection>> connections;
    std::vector<Message> scratch;

    EchoServer(NetService& net, ListenConfig config = LocalListen()) {
        std::string error;
        listener = net.Listen(config, error);
        REQUIRE_MESSAGE(listener, error);
    }
    void Pump() {
        listener->Accept(connections);
        for (auto& c : connections) {
            scratch.clear();
            c->Receive(scratch);
            for (const Message& m : scratch) c->Send(m.id, m.payload);
        }
    }
};

} // namespace

TEST_CASE("Framing: frames split at every byte still parse") {
    std::vector<std::byte> stream;
    AppendFrame(stream, 0x0102, Bytes("hello"));
    AppendFrame(stream, 0xFFFF, {});
    AppendFrame(stream, 7, Bytes("world!"));
    // Header: u32 length (id + payload) then u16 id, little-endian.
    CHECK(stream[0] == std::byte{7});
    CHECK(stream[4] == std::byte{0x02});
    CHECK(stream[5] == std::byte{0x01});

    for (std::size_t cut = 0; cut <= stream.size(); ++cut) {
        FrameParser parser(1024);
        std::vector<Message> out;
        std::string error;
        REQUIRE(parser.Feed(std::span(stream.data(), cut), out, error));
        REQUIRE(parser.Feed(std::span(stream.data() + cut, stream.size() - cut), out, error));
        REQUIRE(out.size() == 3);
        CHECK(out[0].id == 0x0102);
        CHECK(Text(out[0].payload) == "hello");
        CHECK(out[1].id == 0xFFFF);
        CHECK(out[1].payload.empty());
        CHECK(Text(out[2].payload) == "world!");
        CHECK(parser.Buffered() == 0);
    }
}

TEST_CASE("Framing: a malformed length is refused before allocating") {
    std::vector<Message> out;
    std::string error;
    FrameParser tooShort(1024);
    CHECK_FALSE(tooShort.Feed(U32(1), out, error));
    CHECK(error.find("shorter") != std::string::npos);
    FrameParser tooLong(1024);
    CHECK_FALSE(tooLong.Feed(U32(1025), out, error));
    CHECK(error.find("exceeds") != std::string::npos);
    FrameParser huge(1024);
    CHECK_FALSE(huge.Feed(U32(0xFFFFFFFF), out, error));
    CHECK(out.empty());
}

TEST_CASE("Framing: the latest lane keeps only the newest value, in the first one's place") {
    OutgoingQueue queue;
    queue.Push(1, Bytes("a"));
    queue.PushLatest(42, 2, Bytes("pos 1"));
    queue.Push(3, Bytes("b"));
    queue.PushLatest(42, 2, Bytes("pos 2"));
    queue.PushLatest(7, 2, Bytes("other"));
    queue.PushLatest(42, 2, Bytes("pos 3 (newest)"));
    CHECK(queue.Replaced() == 2);

    std::vector<std::byte> frames;
    REQUIRE(queue.Take(frames));
    CHECK(queue.Empty());
    CHECK(queue.PendingBytes() == 0);
    FrameParser parser(1024);
    std::vector<Message> out;
    std::string error;
    REQUIRE(parser.Feed(frames, out, error));
    REQUIRE(out.size() == 4);
    CHECK(Text(out[0].payload) == "a");
    CHECK(Text(out[1].payload) == "pos 3 (newest)");
    CHECK(Text(out[2].payload) == "b");
    CHECK(Text(out[3].payload) == "other");

    // After a Take, the key starts over: the next value is a new message.
    queue.PushLatest(42, 2, Bytes("pos 4"));
    REQUIRE(queue.Take(frames));
    out.clear();
    REQUIRE(parser.Feed(frames, out, error));
    REQUIRE(out.size() == 1);
    CHECK(Text(out[0].payload) == "pos 4");
    CHECK_FALSE(queue.Take(frames));
}

TEST_CASE("Net: messages over TLS, both ways, and the version check") {
    NetService server(2), client(1);
    EchoServer echo(server);
    auto c = client.Connect(LocalConnect(echo.listener->Port()));

    sr::game::proto::Hello hello;
    hello.client = "test";
    c->Send(sr::game::proto::Hello::kId, sr::game::proto::Encode(hello)); // queued while connecting
    std::vector<Message> got;
    REQUIRE(Until(client, [&] {
        echo.Pump();
        c->Receive(got);
        return !got.empty() || c->State() == ConnectionState::Closed;
    }));
    REQUIRE_MESSAGE(c->State() == ConnectionState::Open, c->CloseReason());
    REQUIRE(got.size() == 1);
    CHECK(got[0].id == sr::game::proto::Hello::kId);
    sr::game::proto::Hello back;
    REQUIRE(sr::game::proto::Decode(got[0].payload, back));
    CHECK(back.client == "test");
    CHECK(c->RemoteAddress().starts_with("127.0.0.1:"));
    CHECK(c->Stats().messagesSent == 1);
    CHECK(c->Stats().messagesReceived == 1);
}

TEST_CASE("Net: 1,000 connections exchange messages over TLS") {
    NetService server(4), client(2);
    EchoServer echo(server);
    constexpr int kConnections = 1000;
    constexpr int kMessages = 5;
    std::vector<std::shared_ptr<Connection>> clients;
    for (int i = 0; i < kConnections; ++i) {
        clients.push_back(client.Connect(LocalConnect(echo.listener->Port())));
        for (int m = 0; m < kMessages; ++m) {
            const std::string text = std::to_string(i) + "/" + std::to_string(m);
            clients.back()->Send(std::uint16_t(0x1000 + m), Bytes(text));
        }
    }
    std::vector<std::vector<Message>> got(kConnections);
    int complete = 0;
    const bool done = Until(client, [&] {
        echo.Pump();
        complete = 0;
        for (int i = 0; i < kConnections; ++i) {
            clients[i]->Receive(got[i]);
            if (got[i].size() == kMessages) ++complete;
        }
        return complete == kConnections;
    }, 60s);
    int closed = 0;
    std::string firstReason;
    for (auto& c : clients) {
        if (c->State() == ConnectionState::Closed) {
            if (closed++ == 0) firstReason = c->CloseReason();
        }
    }
    INFO("closed: ", closed, " first reason: ", firstReason);
    REQUIRE(done);
    CHECK(echo.connections.size() == kConnections);
    CHECK(closed == 0);
    for (int i = 0; i < kConnections; ++i) {
        for (int m = 0; m < kMessages; ++m) {
            CHECK(got[i][m].id == 0x1000 + m);
            CHECK(Text(got[i][m].payload) == std::to_string(i) + "/" + std::to_string(m));
        }
    }

    // The server closes first, as ours will: its side keeps the closing state (TIME_WAIT), not
    // the client's ports, so test runs in a row do not run out of them.
    for (auto& c : echo.connections) c->Close();
    CHECK(Until(client, [&] {
        for (auto& c : clients) {
            if (c->State() != ConnectionState::Closed) return false;
        }
        return true;
    }));
    CHECK(clients[0]->CloseReason() == "closed by the peer");
}

TEST_CASE("Net: a server the client does not trust is refused") {
    NetService server(1), client(1);
    EchoServer echo(server);
    std::string otherPem, otherKey, error;
    REQUIRE(MakeSelfSignedCertificate("localhost", 1, otherPem, otherKey, error));

    ConnectConfig config = LocalConnect(echo.listener->Port());
    config.tls.trustedCertificatePem = otherPem; // pins a different certificate
    auto c = client.Connect(config);
    REQUIRE(Until(client, [&] { echo.Pump(); return c->State() == ConnectionState::Closed; }));
    CHECK(c->CloseReason().find("TLS handshake failed") != std::string::npos);

    // The right certificate under the wrong name is refused too.
    config = LocalConnect(echo.listener->Port());
    config.tls.serverName = "gateway.example";
    auto named = client.Connect(config);
    REQUIRE(Until(client, [&] { echo.Pump(); return named->State() == ConnectionState::Closed; }));
    CHECK(named->CloseReason().find("TLS handshake failed") != std::string::npos);
    REQUIRE(Until(server, [&] { return echo.listener->Rejected() == 2; }));
    CHECK(echo.connections.empty());
}

TEST_CASE("Net: malformed and oversized frames close only their connection") {
    NetService server(2), client(1);
    ListenConfig listen = LocalListen();
    listen.limits.maxFrame = 1024;
    EchoServer echo(server, listen);
    auto good = client.Connect(LocalConnect(echo.listener->Port()));
    auto tooShort = client.Connect(LocalConnect(echo.listener->Port()));
    auto tooLong = client.Connect(LocalConnect(echo.listener->Port()));
    auto bigMessage = client.Connect(LocalConnect(echo.listener->Port()));
    REQUIRE(Until(client, [&] {
        echo.Pump();
        return good->State() == ConnectionState::Open && tooShort->State() == ConnectionState::Open &&
               tooLong->State() == ConnectionState::Open && bigMessage->State() == ConnectionState::Open &&
               echo.connections.size() == 4;
    }));

    tooShort->SendRawForTest(U32(1));
    tooLong->SendRawForTest(U32(0x7FFFFFFF));
    bigMessage->Send(5, std::vector<std::byte>(1024, std::byte{1})); // 2 + 1024 > 1024
    REQUIRE(Until(client, [&] {
        echo.Pump();
        return tooShort->State() == ConnectionState::Closed && tooLong->State() == ConnectionState::Closed &&
               bigMessage->State() == ConnectionState::Closed;
    }));
    int malformed = 0;
    for (auto& c : echo.connections) {
        if (c->State() == ConnectionState::Closed) {
            CHECK(c->CloseReason().starts_with("malformed frame"));
            ++malformed;
        }
    }
    CHECK(malformed == 3);

    // The good one still works.
    good->Send(9, Bytes("still here"));
    std::vector<Message> got;
    REQUIRE(Until(client, [&] { echo.Pump(); good->Receive(got); return !got.empty(); }));
    CHECK(Text(got[0].payload) == "still here");
}

TEST_CASE("Net: peers that never finish TLS, and the connection limit") {
    NetService server(1), client(1);
    ListenConfig listen = LocalListen();
    listen.limits.handshakeTimeout = 300ms;
    listen.maxConnections = 2;
    EchoServer echo(server, listen);

    // Plain TCP: one peer says nothing, one sends text that is not TLS.
    asio::io_context io;
    asio::ip::tcp::socket silent(io), garbage(io);
    const asio::ip::tcp::endpoint at(asio::ip::make_address("127.0.0.1"), echo.listener->Port());
    asio::error_code ec;
    silent.connect(at, ec);
    REQUIRE_FALSE(ec);
    garbage.connect(at, ec);
    REQUIRE_FALSE(ec);
    asio::write(garbage, asio::buffer(std::string("GET / HTTP/1.1\r\n\r\n")), ec);
    REQUIRE(Until(server, [&] { echo.Pump(); return echo.listener->Rejected() == 2; }, 5s));
    CHECK(echo.connections.empty());

    // Two connections fill the limit; a third is refused.
    auto a = client.Connect(LocalConnect(echo.listener->Port()));
    auto b = client.Connect(LocalConnect(echo.listener->Port()));
    REQUIRE(Until(client, [&] { echo.Pump(); return echo.connections.size() == 2; }));
    auto c = client.Connect(LocalConnect(echo.listener->Port()));
    REQUIRE(Until(client, [&] { echo.Pump(); return c->State() == ConnectionState::Closed; }));
    CHECK(echo.connections.size() == 2);
    CHECK(echo.listener->Rejected() == 3);
}

TEST_CASE("Net: close after send delivers the last message first") {
    NetService server(1), client(1);
    EchoServer echo(server);
    auto c = client.Connect(LocalConnect(echo.listener->Port()));
    REQUIRE(Until(client, [&] { echo.Pump(); return echo.connections.size() == 1; }));

    sr::game::proto::Welcome refused;
    refused.result = sr::game::proto::WelcomeResult::ServerFull;
    refused.reason = "full";
    echo.connections[0]->Send(sr::game::proto::Welcome::kId, sr::game::proto::Encode(refused));
    echo.connections[0]->CloseAfterSend();
    echo.connections[0]->Send(1, Bytes("never sent")); // after CloseAfterSend: dropped

    std::vector<Message> got;
    REQUIRE(Until(client, [&] { c->Receive(got); return c->State() == ConnectionState::Closed; }));
    c->Receive(got);
    REQUIRE(got.size() == 1);
    sr::game::proto::Welcome back;
    REQUIRE(sr::game::proto::Decode(got[0].payload, back));
    CHECK(back.result == sr::game::proto::WelcomeResult::ServerFull);
    CHECK(c->CloseReason() == "closed by the peer");
    REQUIRE(Until(server, [&] { return echo.connections[0]->State() == ConnectionState::Closed; }));
    CHECK(echo.connections[0]->CloseReason() == "closed by this side after sending");
}

TEST_CASE("Net: the latest lane delivers values in order, ending with the newest") {
    NetService server(1), client(1);
    EchoServer echo(server);
    auto c = client.Connect(LocalConnect(echo.listener->Port()));
    REQUIRE(Until(client, [&] { echo.Pump(); return c->State() == ConnectionState::Open && echo.connections.size() == 1; }));

    std::shared_ptr<Connection> sender = echo.connections[0];
    echo.connections.clear(); // not echoed: the server only sends
    constexpr std::uint32_t kUpdates = 20000;
    for (std::uint32_t i = 1; i <= kUpdates; ++i) sender->SendLatest(1, 0x1000, U32(i));
    sender->Send(0x1001, Bytes("end"));

    std::vector<Message> got;
    REQUIRE(Until(client, [&] { c->Receive(got); return !got.empty() && got.back().id == 0x1001; }));
    std::uint32_t last = 0;
    for (std::size_t i = 0; i + 1 < got.size(); ++i) {
        std::uint32_t v = 0;
        std::memcpy(&v, got[i].payload.data(), 4);
        CHECK(v > last); // never an older value after a newer one
        last = v;
    }
    CHECK(last == kUpdates);
    CHECK(got.size() - 1 + sender->Stats().latestReplaced == kUpdates);
    MESSAGE("latest lane: ", got.size() - 1, " of ", kUpdates, " values written, ", sender->Stats().latestReplaced, " replaced while waiting");
}

TEST_CASE("Net: the service shuts down with connections still held") {
    std::shared_ptr<Connection> kept;
    std::unique_ptr<EchoServer> echo;
    {
        NetService server(1);
        echo = std::make_unique<EchoServer>(server);
        {
            NetService client(1);
            kept = client.Connect(LocalConnect(echo->listener->Port()));
            REQUIRE(Until(client, [&] { echo->Pump(); return kept->State() == ConnectionState::Open; }));
        } // the client's service closes `kept` and joins
        CHECK(kept->State() == ConnectionState::Closed);
        kept->Send(1, Bytes("ignored"));
        echo.reset();
    }
    kept.reset();
}
