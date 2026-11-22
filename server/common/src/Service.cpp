#include "sr/server/Service.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "sr/core/Log.h"

namespace sr::server {

void Completions::Post(std::function<void()> fn) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(fn));
    }
    net_.Wake();
}

std::size_t Completions::Run() {
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard lock(mutex_);
        ready.swap(queue_);
    }
    for (auto& fn : ready) {
        // Work finished elsewhere and an exception in it: logged, the loop goes on (one player's failure must
        // not end the server for everyone).
        try {
            fn();
        } catch (const std::exception& e) {
            core::LogError("a completion threw: {}", e.what());
        } catch (...) {
            core::LogError("a completion threw");
        }
    }
    return ready.size();
}

void Peers::Pump(net::Listener& listener, const std::function<void(Peer&, const net::Message&)>& handle) {
    const Clock::time_point now = Clock::now();
    accepted_.clear();
    listener.Accept(accepted_);
    for (auto& connection : accepted_) {
        Peer peer;
        peer.id = nextId_++;
        peer.address = connection->RemoteAddress();
        if (const auto colon = peer.address.rfind(':'); colon != std::string::npos) peer.address.resize(colon);
        peer.connection = std::move(connection);
        peer.lastHeard = now;
        peers_.emplace(peer.id, std::move(peer));
    }

    for (auto it = peers_.begin(); it != peers_.end();) {
        Peer& peer = it->second;
        if (peer.connection->State() == net::ConnectionState::Closed) {
            if (onClosed_) onClosed_(peer);
            it = peers_.erase(it);
            continue;
        }
        messages_.clear();
        if (peer.connection->Receive(messages_) > 0) peer.lastHeard = now;
        // The message budget: filled for the time since last, then one a message.
        const bool budgeted = limits_.messagesPerSecond > 0 && (limits_.budgetLoopback || !net::IsLoopbackAddress(peer.address));
        if (budgeted) {
            if (!peer.budgetStarted) {
                peer.budgetStarted = true;
                peer.budget = limits_.messageBurst;
            } else {
                const double seconds = std::chrono::duration<double>(now - peer.budgetAt).count();
                peer.budget = std::min(limits_.messageBurst, peer.budget + seconds * limits_.messagesPerSecond);
            }
            peer.budgetAt = now;
        }
        for (const net::Message& m : messages_) {
            if (peer.dropped || peer.connection->State() == net::ConnectionState::Closed) break;
            if (budgeted && (peer.budget -= 1.0) < 0) {
                core::LogWarning("{}: {} (account {}) sends faster than {} messages a second; closing", serverName_, peer.address, peer.account,
                                 limits_.messagesPerSecond);
                peer.dropped = true;
                peer.connection->Close();
                break;
            }
            if (peer.greeted && m.id == game::proto::KeepAlive::kId) continue; // its arrival is all it says
            if (!peer.greeted) {
                game::proto::Hello hello;
                if (m.id != game::proto::Hello::kId || !game::proto::Decode(m.payload, hello)) {
                    core::LogInfo("{}: {} did not greet first (message 0x{:04x}); closing", serverName_, peer.address, m.id);
                    peer.connection->Close();
                    break;
                }
                game::proto::Welcome welcome = game::proto::AnswerHello(hello, serverName_);
                // A players' client older than the shard's floor (ClientGate.h): told where the new one is, as a protocol
                // mismatch (the clients of before say "out of date" for it).
                if (welcome.result == game::proto::WelcomeResult::Accepted && limits_.clientGate) {
                    if (auto refusal = limits_.clientGate->Refuse(hello.client)) {
                        core::LogInfo("{}: {} greets as '{}': turned away, out of date", serverName_, peer.address, hello.client);
                        welcome.result = game::proto::WelcomeResult::ProtocolMismatch;
                        welcome.reason = std::move(*refusal);
                    }
                }
                SendMessage(*peer.connection, welcome);
                if (welcome.result != game::proto::WelcomeResult::Accepted) {
                    peer.connection->CloseAfterSend();
                    break;
                }
                peer.greeted = true;
                continue;
            }
            // An exception from one message closes that peer alone (logged with the message), never the server.
            try {
                handle(peer, m);
            } catch (const std::exception& e) {
                core::LogError("{}: message 0x{:04x} from {} (account {}) threw: {}; closing it", serverName_, m.id, peer.address, peer.account, e.what());
                peer.dropped = true;
                peer.connection->Close();
                break;
            } catch (...) {
                core::LogError("{}: message 0x{:04x} from {} (account {}) threw; closing it", serverName_, m.id, peer.address, peer.account);
                peer.dropped = true;
                peer.connection->Close();
                break;
            }
        }
        const auto quiet = now - peer.lastHeard;
        if (!peer.greeted && quiet > limits_.helloTimeout) {
            peer.connection->Close();
        } else if (peer.greeted && !peer.busy && quiet > limits_.idleTimeout) {
            peer.connection->Close();
        }
        ++it;
    }
}

Peer* Peers::Find(std::uint64_t id) {
    const auto it = peers_.find(id);
    if (it == peers_.end() || it->second.connection->State() == net::ConnectionState::Closed) return nullptr;
    return &it->second;
}

void Peers::CloseAll() {
    for (auto& [id, peer] : peers_) {
        peer.connection->Close();
        if (onClosed_) onClosed_(peer);
    }
    peers_.clear();
}

bool AttemptLimiter::Allow(const std::string& address, Clock::time_point now) {
    // Forget quiet addresses now and then, so the table does not grow without end.
    if (now - lastSweep_ > window_) {
        std::erase_if(attempts_, [&](const auto& entry) { return entry.second.empty() || now - entry.second.back() > window_; });
        lastSweep_ = now;
    }
    auto& times = attempts_[address];
    while (!times.empty() && now - times.front() > window_) times.pop_front();
    if (int(times.size()) >= count_) return false;
    times.push_back(now);
    return true;
}

namespace {

bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream text;
    text << file.rdbuf();
    out = text.str();
    return true;
}

bool WriteFile(const std::string& path, const std::string& text) {
    std::error_code ec;
    if (const auto parent = std::filesystem::path(path).parent_path(); !parent.empty()) std::filesystem::create_directories(parent, ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    return bool(file);
}

} // namespace

bool LoadCertificate(const std::string& certificatePath, const std::string& keyPath, bool createIfMissing,
                     const std::string& name, net::TlsServerConfig& out, std::string& error) {
    if (createIfMissing && (!std::filesystem::exists(certificatePath) || !std::filesystem::exists(keyPath))) {
        std::string pem, key;
        if (!net::MakeSelfSignedCertificate(name, 825, pem, key, error)) return false;
        if (!WriteFile(certificatePath, pem) || !WriteFile(keyPath, key)) {
            error = "cannot write " + certificatePath + " / " + keyPath;
            return false;
        }
        core::LogInfo("made a development certificate for '{}': {}", name, certificatePath);
    }
    if (!ReadFile(certificatePath, out.certificatePem)) {
        error = "cannot read the certificate " + certificatePath;
        return false;
    }
    if (!ReadFile(keyPath, out.privateKeyPem)) {
        error = "cannot read the key " + keyPath;
        return false;
    }
    return true;
}

} // namespace sr::server
