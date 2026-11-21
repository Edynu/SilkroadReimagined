#include "sr/net/Connection.h"

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "sr/core/Log.h"
#include "sr/core/Trace.h"

namespace sr::net {

using Tcp = asio::ip::tcp;
using TlsStream = asio::ssl::stream<Tcp::socket>;

class ConnectionImpl;
class ListenerImpl;

struct NetService::Impl {
    asio::io_context io;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::vector<std::thread> threads;

    std::mutex activityMutex;
    std::condition_variable activityChanged;
    std::uint64_t activity = 0;
    std::uint64_t activitySeen = 0;

    std::mutex registryMutex;
    std::vector<std::weak_ptr<ConnectionImpl>> connections;
    std::vector<std::weak_ptr<ListenerImpl>> listeners;

    void Notify() {
        {
            std::lock_guard lock(activityMutex);
            ++activity;
        }
        activityChanged.notify_all();
    }

    template <typename T>
    static void Register(std::vector<std::weak_ptr<T>>& list, const std::shared_ptr<T>& item) {
        if (list.size() >= 64 && list.size() == list.capacity()) {
            std::erase_if(list, [](const std::weak_ptr<T>& w) { return w.expired(); });
        }
        list.push_back(item);
    }
};

namespace {

std::shared_ptr<asio::ssl::context> MakeContext(asio::ssl::context::method method) {
    auto context = std::make_shared<asio::ssl::context>(method);
    SSL_CTX_set_min_proto_version(context->native_handle(), TLS1_3_VERSION);
    return context;
}

bool IsPeerClose(const asio::error_code& ec) {
    return ec == asio::error::eof || ec == asio::error::connection_reset ||
           ec == asio::ssl::error::stream_truncated;
}

} // namespace

class ConnectionImpl final : public Connection, public std::enable_shared_from_this<ConnectionImpl> {
public:
    ConnectionImpl(std::shared_ptr<NetService::Impl> service, std::shared_ptr<asio::ssl::context> tls,
                   Tcp::socket socket, const ConnectionLimits& limits)
        : service_(std::move(service)), tls_(std::move(tls)), stream_(std::move(socket), *tls_),
          timer_(stream_.get_executor()), resolver_(stream_.get_executor()), limits_(limits),
          parser_(limits.maxFrame) {}

    // Server side: the TCP connection is accepted; runs the TLS handshake. `onOpen` hands the
    // connection to its listener, `onReject` counts it as rejected if it never opens.
    void StartServer(std::function<void(std::shared_ptr<ConnectionImpl>)> onOpen, std::function<void()> onReject) {
        onOpen_ = std::move(onOpen);
        onReject_ = std::move(onReject);
        asio::post(stream_.get_executor(), [self = shared_from_this()] {
            self->ArmHandshakeTimer();
            self->stream_.async_handshake(asio::ssl::stream_base::server, [self](const asio::error_code& ec) {
                if (ec) return self->CloseNow(std::format("TLS handshake failed: {}", ec.message()));
                self->OnOpen();
            });
        });
    }

    // Client side: resolve, connect, verify the server, handshake.
    void StartClient(const ConnectConfig& config) {
        asio::post(stream_.get_executor(), [self = shared_from_this(), config] {
            self->ArmHandshakeTimer();
            SSL_set_tlsext_host_name(self->stream_.native_handle(), config.tls.serverName.c_str());
            self->stream_.set_verify_mode(asio::ssl::verify_peer);
            self->stream_.set_verify_callback(asio::ssl::host_name_verification(config.tls.serverName));
            self->resolver_.async_resolve(
                config.host, std::to_string(config.port),
                [self](const asio::error_code& ec, const Tcp::resolver::results_type& results) {
                    if (ec) return self->CloseNow(std::format("cannot resolve the host: {}", ec.message()));
                    self->ConnectTo(results, 0);
                });
        });
    }

    void Send(MessageId id, std::span<const std::byte> payload) override {
        Queue([&](OutgoingQueue& q) { q.Push(id, payload); });
    }

    void SendLatest(std::uint64_t key, MessageId id, std::span<const std::byte> payload) override {
        Queue([&](OutgoingQueue& q) { q.PushLatest(key, id, payload); });
    }

    void SendMany(std::span<const OutgoingMessage> messages) override {
        if (messages.empty()) return;
        Queue(
            [&](OutgoingQueue& q) {
                for (const OutgoingMessage& m : messages) {
                    if (m.latestKey != 0) {
                        q.PushLatest(m.latestKey, m.id, m.payload);
                    } else {
                        q.Push(m.id, m.payload);
                    }
                }
            },
            messages.size());
    }

    std::size_t Receive(std::vector<Message>& out) override {
        std::lock_guard lock(mutex_);
        const std::size_t n = received_.size();
        for (Message& m : received_) out.push_back(std::move(m));
        received_.clear();
        receivedBytes_ = 0;
        return n;
    }

    ConnectionState State() const override { return state_.load(); }

    std::string CloseReason() const override {
        std::lock_guard lock(mutex_);
        return closeReason_;
    }

    void Close() override {
        asio::post(stream_.get_executor(), [self = shared_from_this()] { self->CloseNow("closed by this side"); });
    }

    void CloseAfterSend() override {
        closeAfterSend_ = true;
        asio::post(stream_.get_executor(), [self = shared_from_this()] { self->StartWrite(); });
    }

    std::string RemoteAddress() const override {
        std::lock_guard lock(mutex_);
        return remote_;
    }

    ConnectionStats Stats() const override {
        std::lock_guard lock(mutex_);
        ConnectionStats s = stats_;
        s.latestReplaced = outgoing_.Replaced();
        return s;
    }

    void SendRawForTest(std::span<const std::byte> bytes) override {
        asio::post(stream_.get_executor(), [self = shared_from_this(), data = std::vector<std::byte>(bytes.begin(), bytes.end())] {
            if (self->state_ != ConnectionState::Open) return;
            auto buffer = std::make_shared<std::vector<std::byte>>(data);
            asio::async_write(self->stream_, asio::buffer(*buffer), [self, buffer](const asio::error_code&, std::size_t) {});
        });
    }

private:
    template <typename F>
    void Queue(F&& push, std::size_t messages = 1) {
        if (state_ == ConnectionState::Closed || closeAfterSend_) return;
        bool overflow = false;
        {
            std::lock_guard lock(mutex_);
            push(outgoing_);
            stats_.messagesSent += messages;
            overflow = outgoing_.PendingBytes() > limits_.maxPendingBytes;
        }
        if (overflow) {
            asio::post(stream_.get_executor(), [self = shared_from_this()] {
                self->CloseNow("the peer does not read fast enough (send queue full)");
            });
            return;
        }
        if (state_ == ConnectionState::Open && !writeScheduled_.exchange(true)) {
            asio::post(stream_.get_executor(), [self = shared_from_this()] { self->StartWrite(); });
        }
    }

    // The system may pick a local port whose connection to the same server is still closing
    // (TIME_WAIT); connecting again picks another.
    static bool IsPortCollision(const asio::error_code& ec) {
#ifdef _WIN32
        constexpr int kDuplicateName = 52; // ERROR_DUP_NAME, what ConnectEx reports for it
        if (ec.value() == kDuplicateName) return true;
#endif
        return ec == asio::error::address_in_use;
    }

    void ConnectTo(const Tcp::resolver::results_type& results, int attempt) {
        asio::async_connect(stream_.lowest_layer(), results,
                            [self = shared_from_this(), results, attempt](const asio::error_code& ec, const Tcp::endpoint&) {
                                if (self->state_ == ConnectionState::Closed) return;
                                if (ec && IsPortCollision(ec) && attempt < 3) {
                                    asio::error_code ignored;
                                    self->stream_.lowest_layer().close(ignored);
                                    return self->ConnectTo(results, attempt + 1);
                                }
                                if (ec) return self->CloseNow(std::format("cannot connect: {}", ec.message()));
                                // Every message goes out at once: no waiting to gather small writes (Nagle's
                                // algorithm), which with the peer's delayed acknowledgements holds a click for up to
                                // 200 ms.
                                asio::error_code noDelay;
                                self->stream_.lowest_layer().set_option(Tcp::no_delay(true), noDelay);
                                self->stream_.async_handshake(asio::ssl::stream_base::client, [self](const asio::error_code& ec) {
                                    if (ec) return self->CloseNow(std::format("TLS handshake failed: {}", ec.message()));
                                    self->OnOpen();
                                });
                            });
    }

    void ArmHandshakeTimer() {
        timer_.expires_after(limits_.handshakeTimeout);
        timer_.async_wait([self = shared_from_this()](const asio::error_code& ec) {
            if (!ec && self->state_ == ConnectionState::Connecting) self->CloseNow("connecting timed out");
        });
    }

    void OnOpen() {
        timer_.cancel();
        asio::error_code ec;
        const Tcp::endpoint remote = stream_.lowest_layer().remote_endpoint(ec);
        {
            std::lock_guard lock(mutex_);
            remote_ = ec ? std::string("?") : std::format("{}:{}", remote.address().to_string(), remote.port());
        }
        state_ = ConnectionState::Open;
        if (onOpen_) onOpen_(shared_from_this());
        onOpen_ = nullptr;
        onReject_ = nullptr;
        service_->Notify();
        StartRead();
        StartWrite();
    }

    void StartRead() {
        stream_.async_read_some(asio::buffer(readBuffer_), [self = shared_from_this()](const asio::error_code& ec, std::size_t n) {
            self->OnRead(ec, n);
        });
    }

    void OnRead(const asio::error_code& ec, std::size_t n) {
        if (state_ == ConnectionState::Closed) return;
        if (ec) return CloseNow(IsPeerClose(ec) ? std::string("closed by the peer") : std::format("receive failed: {}", ec.message()));
        std::vector<Message> fresh;
        std::string error;
        if (!parser_.Feed(std::span(readBuffer_.data(), n), fresh, error)) return CloseNow("malformed frame: " + error);
        {
            std::lock_guard lock(mutex_);
            stats_.bytesReceived += n; // as read (packed frames count as the bytes they took)
        }
        if (!fresh.empty()) {
            bool flooded = false;
            {
                std::lock_guard lock(mutex_);
                for (Message& m : fresh) {
                    // The flood limit counts each message as its own frame: a packed run cannot slip past it.
                    receivedBytes_ += kFrameHeaderSize + m.payload.size();
                    ++stats_.messagesReceived;
                    received_.push_back(std::move(m));
                }
                flooded = receivedBytes_ > limits_.maxReceivedBytes;
            }
            if (flooded) return CloseNow("too much received and not taken (receive queue full)");
            service_->Notify();
        }
        StartRead();
    }

    void StartWrite() {
        writeScheduled_ = false;
        if (writing_ || state_ != ConnectionState::Open) return;
        {
            std::lock_guard lock(mutex_);
            if (outgoing_.Take(writeBuffer_)) {
                stats_.bytesSent += writeBuffer_.size();
            } else {
                writeBuffer_.clear();
            }
        }
        if (writeBuffer_.empty()) {
            if (closeAfterSend_) CloseNow("closed by this side after sending");
            return;
        }
        writing_ = true;
        asio::async_write(stream_, asio::buffer(writeBuffer_), [self = shared_from_this()](const asio::error_code& ec, std::size_t) {
            self->writing_ = false;
            if (self->state_ == ConnectionState::Closed) return;
            if (ec) return self->CloseNow(std::format("send failed: {}", ec.message()));
            self->StartWrite();
        });
    }

    void CloseNow(std::string reason) {
        if (state_ == ConnectionState::Closed) return;
        const bool wasOpen = state_ == ConnectionState::Open;
        {
            std::lock_guard lock(mutex_);
            closeReason_ = std::move(reason);
        }
        state_ = ConnectionState::Closed;
        timer_.cancel();
        resolver_.cancel();
        asio::error_code ec;
        stream_.lowest_layer().shutdown(Tcp::socket::shutdown_both, ec);
        stream_.lowest_layer().close(ec);
        if (!wasOpen && onReject_) onReject_();
        onOpen_ = nullptr;
        onReject_ = nullptr;
        service_->Notify();
    }

    std::shared_ptr<NetService::Impl> service_;
    std::shared_ptr<asio::ssl::context> tls_;
    TlsStream stream_;
    asio::steady_timer timer_;
    Tcp::resolver resolver_;
    ConnectionLimits limits_;

    std::atomic<ConnectionState> state_{ConnectionState::Connecting};
    std::atomic<bool> writeScheduled_{false};
    std::atomic<bool> closeAfterSend_{false};

    // Only on the connection's strand.
    std::function<void(std::shared_ptr<ConnectionImpl>)> onOpen_;
    std::function<void()> onReject_;
    FrameParser parser_;
    std::array<std::byte, 16 * 1024> readBuffer_;
    std::vector<std::byte> writeBuffer_;
    bool writing_ = false;

    // Shared with the threads that send and receive.
    mutable std::mutex mutex_;
    OutgoingQueue outgoing_;
    std::vector<Message> received_;
    std::size_t receivedBytes_ = 0;
    ConnectionStats stats_;
    std::string closeReason_;
    std::string remote_;
};

class ListenerImpl final : public std::enable_shared_from_this<ListenerImpl> {
public:
    ListenerImpl(std::shared_ptr<NetService::Impl> service, std::shared_ptr<asio::ssl::context> tls, const ListenConfig& config)
        : service_(std::move(service)), tls_(std::move(tls)), acceptor_(asio::make_strand(service_->io)),
          limits_(config.limits), maxConnections_(config.maxConnections), maxPerAddress_(config.maxPerAddress),
          acceptsPerMinute_(config.acceptsPerMinute) {}

    bool Open(const ListenConfig& config, std::string& error) {
        asio::error_code ec;
        const asio::ip::address address = asio::ip::make_address(config.address, ec);
        if (ec) {
            error = std::format("bad listen address '{}': {}", config.address, ec.message());
            return false;
        }
        const Tcp::endpoint endpoint(address, config.port);
        acceptor_.open(endpoint.protocol(), ec);
        if (!ec) acceptor_.bind(endpoint, ec);
        if (!ec) acceptor_.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            error = std::format("cannot listen on {}:{}: {}", config.address, config.port, ec.message());
            return false;
        }
        port_ = acceptor_.local_endpoint(ec).port();
        return true;
    }

    void StartAccept() {
        acceptor_.async_accept(asio::make_strand(service_->io), [self = shared_from_this()](const asio::error_code& ec, Tcp::socket socket) {
            if (ec == asio::error::operation_aborted || !self->acceptor_.is_open()) return;
            if (!ec) {
                // Every message goes out at once (no Nagle's algorithm): the world's updates are small and must not
                // wait for the client's acknowledgements.
                asio::error_code noDelay;
                socket.set_option(Tcp::no_delay(true), noDelay);
                self->OnAccept(std::move(socket));
            }
            self->StartAccept();
        });
    }

    std::uint16_t Port() const { return port_; }

    std::size_t Accept(std::vector<std::shared_ptr<Connection>>& out) {
        std::lock_guard lock(mutex_);
        const std::size_t n = accepted_.size();
        for (auto& c : accepted_) out.push_back(std::move(c));
        accepted_.clear();
        return n;
    }

    std::uint64_t Rejected() const { return rejected_.load(); }

    void Close() {
        asio::post(acceptor_.get_executor(), [self = shared_from_this()] {
            asio::error_code ec;
            self->acceptor_.close(ec);
        });
    }

private:
    void OnAccept(Tcp::socket socket) {
        std::erase_if(live_, [](const Live& l) {
            const auto c = l.connection.lock();
            return !c || c->State() == ConnectionState::Closed;
        });
        asio::error_code ec;
        const Tcp::endpoint remote = socket.remote_endpoint(ec);
        const std::string address = ec ? std::string("?") : remote.address().to_string();
        if (live_.size() >= maxConnections_ || !AddressAllowed(address)) {
            socket.close(ec);
            ++rejected_;
            return;
        }
        auto connection = std::make_shared<ConnectionImpl>(service_, tls_, std::move(socket), limits_);
        live_.push_back({connection, address});
        {
            std::lock_guard lock(service_->registryMutex);
            NetService::Impl::Register(service_->connections, connection);
        }
        connection->StartServer(
            [self = shared_from_this()](std::shared_ptr<ConnectionImpl> c) {
                std::lock_guard lock(self->mutex_);
                self->accepted_.push_back(std::move(c));
            },
            [self = shared_from_this()] { ++self->rejected_; });
    }

    std::shared_ptr<NetService::Impl> service_;
    std::shared_ptr<asio::ssl::context> tls_;
    Tcp::acceptor acceptor_;
    ConnectionLimits limits_;
    // One address's share (on the acceptor's strand): its open connections, handshakes included, and its
    // accepts within the last minute; loopback is never limited (the tests, the bot benchmark).
    bool AddressAllowed(const std::string& address) {
        if (IsLoopbackAddress(address)) return true;
        if (maxPerAddress_ != 0) {
            const auto open = std::count_if(live_.begin(), live_.end(), [&](const Live& l) { return l.address == address; });
            if (std::size_t(open) >= maxPerAddress_) {
                core::LogWarning("net: {} refused: {} connections open from it", address, open);
                return false;
            }
        }
        if (acceptsPerMinute_ != 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastSweep_ > std::chrono::minutes(1)) {
                std::erase_if(accepts_, [&](const auto& e) { return e.second.empty() || now - e.second.back() > std::chrono::minutes(1); });
                lastSweep_ = now;
            }
            auto& times = accepts_[address];
            while (!times.empty() && now - times.front() > std::chrono::minutes(1)) times.pop_front();
            if (times.size() >= acceptsPerMinute_) {
                if (times.size() == acceptsPerMinute_) {
                    core::LogWarning("net: {} refused: {} connections within a minute", address, times.size());
                    times.push_back(now); // one over the limit: the warning said once a burst
                }
                return false;
            }
            times.push_back(now);
        }
        return true;
    }

    struct Live {
        std::weak_ptr<ConnectionImpl> connection;
        std::string address;
    };

    std::size_t maxConnections_;
    std::size_t maxPerAddress_;
    std::size_t acceptsPerMinute_;
    std::uint16_t port_ = 0;
    std::vector<Live> live_; // on the acceptor's strand
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> accepts_;
    std::chrono::steady_clock::time_point lastSweep_{};
    std::atomic<std::uint64_t> rejected_{0};
    std::mutex mutex_;
    std::vector<std::shared_ptr<Connection>> accepted_;
};

namespace {

// What NetService::Listen hands out: closes the listener when dropped.
class ListenerHandle final : public Listener {
public:
    explicit ListenerHandle(std::shared_ptr<ListenerImpl> impl) : impl_(std::move(impl)) {}
    ~ListenerHandle() override { impl_->Close(); }
    std::uint16_t Port() const override { return impl_->Port(); }
    std::size_t Accept(std::vector<std::shared_ptr<Connection>>& out) override { return impl_->Accept(out); }
    std::uint64_t Rejected() const override { return impl_->Rejected(); }
    void Close() override { impl_->Close(); }

private:
    std::shared_ptr<ListenerImpl> impl_;
};

// A connection that never started (bad configuration): Closed with its reason.
class FailedConnection final : public Connection {
public:
    explicit FailedConnection(std::string reason) : reason_(std::move(reason)) {}
    void Send(MessageId, std::span<const std::byte>) override {}
    void SendLatest(std::uint64_t, MessageId, std::span<const std::byte>) override {}
    std::size_t Receive(std::vector<Message>&) override { return 0; }
    ConnectionState State() const override { return ConnectionState::Closed; }
    std::string CloseReason() const override { return reason_; }
    void Close() override {}
    void CloseAfterSend() override {}
    std::string RemoteAddress() const override { return {}; }
    ConnectionStats Stats() const override { return {}; }
    void SendRawForTest(std::span<const std::byte>) override {}

private:
    std::string reason_;
};

} // namespace

bool IsLoopbackAddress(std::string_view address) {
    return address.starts_with("127.") || address == "::1" || address.starts_with("::ffff:127.");
}

NetService::NetService(unsigned threads) : impl_(std::make_shared<Impl>()) {
    impl_->work.emplace(asio::make_work_guard(impl_->io));
    const unsigned count = threads == 0 ? 1 : threads;
    for (unsigned i = 0; i < count; ++i) {
        impl_->threads.emplace_back([impl = impl_.get(), i] {
            const std::string name = std::format("Network {}", i);
            core::SetThreadName(name.c_str());
            for (;;) {
                try {
                    impl->io.run();
                    return;
                } catch (const std::exception& e) {
                    core::LogError("network thread: {}", e.what());
                }
            }
        });
    }
}

NetService::~NetService() {
    std::vector<std::shared_ptr<ConnectionImpl>> connections;
    std::vector<std::shared_ptr<ListenerImpl>> listeners;
    {
        std::lock_guard lock(impl_->registryMutex);
        for (auto& w : impl_->connections) {
            if (auto c = w.lock()) connections.push_back(std::move(c));
        }
        for (auto& w : impl_->listeners) {
            if (auto l = w.lock()) listeners.push_back(std::move(l));
        }
    }
    for (auto& l : listeners) l->Close();
    for (auto& c : connections) c->Close();
    connections.clear();
    listeners.clear();
    // Every socket and timer is closed: the threads return once their handlers have run.
    impl_->work.reset();
    for (std::thread& t : impl_->threads) t.join();
    impl_->threads.clear();
}

std::shared_ptr<Connection> NetService::Connect(const ConnectConfig& config) {
    std::shared_ptr<asio::ssl::context> tls;
    try {
        tls = MakeContext(asio::ssl::context::tls_client);
    } catch (const std::exception& e) {
        return std::make_shared<FailedConnection>(std::format("TLS setup failed: {}", e.what()));
    }
    asio::error_code ec;
    tls->add_certificate_authority(asio::buffer(config.tls.trustedCertificatePem), ec);
    if (ec) return std::make_shared<FailedConnection>(std::format("the trusted certificate is unusable: {}", ec.message()));
    tls->set_verify_mode(asio::ssl::verify_peer);

    auto connection = std::make_shared<ConnectionImpl>(impl_, tls, Tcp::socket(asio::make_strand(impl_->io)), config.limits);
    {
        std::lock_guard lock(impl_->registryMutex);
        Impl::Register(impl_->connections, connection);
    }
    connection->StartClient(config);
    return connection;
}

std::unique_ptr<Listener> NetService::Listen(const ListenConfig& config, std::string& error) {
    std::shared_ptr<asio::ssl::context> tls;
    try {
        tls = MakeContext(asio::ssl::context::tls_server);
    } catch (const std::exception& e) {
        error = std::format("TLS setup failed: {}", e.what());
        return nullptr;
    }
    asio::error_code ec;
    tls->use_certificate_chain(asio::buffer(config.tls.certificatePem), ec);
    if (ec) {
        error = std::format("the server certificate is unusable: {}", ec.message());
        return nullptr;
    }
    tls->use_private_key(asio::buffer(config.tls.privateKeyPem), asio::ssl::context::pem, ec);
    if (ec) {
        error = std::format("the server key is unusable: {}", ec.message());
        return nullptr;
    }
    auto listener = std::make_shared<ListenerImpl>(impl_, tls, config);
    if (!listener->Open(config, error)) return nullptr;
    {
        std::lock_guard lock(impl_->registryMutex);
        Impl::Register(impl_->listeners, listener);
    }
    listener->StartAccept();
    return std::make_unique<ListenerHandle>(std::move(listener));
}

void NetService::Wake() { impl_->Notify(); }

bool NetService::WaitForActivity(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->activityMutex);
    impl_->activityChanged.wait_for(lock, timeout, [&] { return impl_->activity != impl_->activitySeen; });
    const bool happened = impl_->activity != impl_->activitySeen;
    impl_->activitySeen = impl_->activity;
    return happened;
}

} // namespace sr::net
