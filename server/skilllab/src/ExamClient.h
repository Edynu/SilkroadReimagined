#pragma once

// The lab's exams' player client (the skill exam's and the item exam's): a connection to the exam's game server that
// keeps what it receives with when it came; waiting turns `pump` (the in-process server's loop on the exam's thread,
// or a short sleep in a worker process whose server runs in another).

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sr/game/Protocol.h"
#include "sr/net/Connection.h"

namespace sr::server::lab {

using Clock = std::chrono::steady_clock;

// postgresql://user:password@host:port/database?options -> the same with `database`.
inline std::string WithDatabase(const std::string& url, const std::string& database) {
    const std::size_t scheme = url.find("://");
    const std::size_t slash = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    const std::size_t query = url.find('?');
    const std::string options = query == std::string::npos ? "" : url.substr(query);
    return (slash == std::string::npos ? url : url.substr(0, slash)) + "/" + database + options;
}

inline int Ms(Clock::duration d) { return int(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()); }

class ExamClient {
public:
    struct Received {
        Clock::time_point at;
        net::Message message;
    };
    ExamClient(net::NetService& net, std::uint16_t port, const std::string& pem, std::function<void()> pump) : pump_(std::move(pump)) {
        net::ConnectConfig c;
        c.host = "127.0.0.1";
        c.port = port;
        c.tls = {pem, "localhost"};
        connection_ = net.Connect(c);
    }
    ~ExamClient() {
        if (connection_) connection_->Close();
    }
    template <typename T>
    void Send(const T& message) {
        connection_->Send(T::kId, game::proto::Encode(message));
    }
    // What came, kept from now on.
    void Pump(std::chrono::milliseconds span) {
        const auto end = Clock::now() + span;
        do {
            pump_();
            Take();
        } while (Clock::now() < end);
    }
    std::size_t Mark() const { return log_.size(); }
    // The first `T` from `mark` on that `pred` takes, waiting up to `timeout` for it.
    template <typename T, typename P>
    std::optional<T> Wait(std::size_t mark, P pred, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        const auto end = Clock::now() + timeout;
        std::size_t at = mark;
        for (;;) {
            for (; at < log_.size(); ++at) {
                if (log_[at].message.id != T::kId) continue;
                T out;
                if (game::proto::Decode(log_[at].message.payload, out) && pred(out)) return out;
            }
            if (Clock::now() > end || connection_->State() == net::ConnectionState::Closed) return std::nullopt;
            pump_();
            Take();
        }
    }
    template <typename T>
    std::optional<T> Next(std::size_t mark, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        return Wait<T>(mark, [](const T&) { return true; }, timeout);
    }
    // Every `T` from `mark` on, with when it came.
    template <typename T>
    std::vector<std::pair<Clock::time_point, T>> Since(std::size_t mark) const {
        std::vector<std::pair<Clock::time_point, T>> out;
        for (std::size_t i = mark; i < log_.size(); ++i) {
            if (log_[i].message.id != T::kId) continue;
            T m;
            if (game::proto::Decode(log_[i].message.payload, m)) out.push_back({log_[i].at, std::move(m)});
        }
        return out;
    }
    void Forget() { log_.clear(); }
    // What came, kept, without turning the pump: an idle player's connection read (what nobody takes fills its receive
    // queue, and a full one closes the connection: net::ConnectionLimits::maxReceivedBytes).
    void Drain() { Take(); }
    bool Open() const { return connection_->State() != net::ConnectionState::Closed; }

private:
    void Take() {
        std::vector<net::Message> in;
        connection_->Receive(in);
        const auto now = Clock::now();
        for (net::Message& m : in) log_.push_back({now, std::move(m)});
    }
    std::shared_ptr<net::Connection> connection_;
    std::function<void()> pump_;
    std::vector<Received> log_;
};

} // namespace sr::server::lab
