#include "sr/server/LoginServer.h"

#include <cctype>
#include <format>
#include <functional>
#include <optional>
#include <unordered_map>

#include "sr/core/Log.h"
#include "sr/core/Trace.h"
#include "sr/game/TitleProtocol.h"
#include "sr/server/Database.h"
#include "sr/server/Store.h"

namespace sr::server {

namespace proto = game::proto;

struct LoginServer::State {
    LoginConfig config;
    std::unique_ptr<net::NetService> net;
    std::unique_ptr<net::Listener> listener;
    std::unique_ptr<Completions> completions;
    DatabasePool database;
    std::unique_ptr<Peers> peers;
    std::unique_ptr<AttemptLimiter> limiter;
    // Wrong passwords by account name and address: the original's tolerance
    // (3, then 10 minutes) held against that address alone, so nobody else can lock an account's owner out; an
    // unknown name is counted and answered as a wrong password (which names exist is not told).
    struct Misses {
        int count = 0;
        Clock::time_point last, until;
    };
    std::unordered_map<std::string, Misses> misses;
    Clock::time_point missesSwept;
    std::string decoyHash; // an Argon2id hash checked for an unknown name: the same time as a real one
    // Password checks still running: Stop waits for them (they post into `completions`).
    core::JobCounter checks;
    Stats stats;

    // The answer to a peer's login, if it is still connected; frees it for another attempt.
    void Answer(std::uint64_t peerId, const proto::LoginResult& result) {
        if (result.status == proto::LoginStatus::Accepted) {
            ++stats.accepted;
        } else {
            ++stats.refused;
        }
        Peer* peer = peers->Find(peerId);
        if (!peer) return;
        peer->busy = false;
        SendMessage(*peer->connection, result);
        if (result.status == proto::LoginStatus::Accepted) peer->connection->CloseAfterSend();
    }

    static proto::LoginResult Status(proto::LoginStatus status) {
        proto::LoginResult r;
        r.status = status;
        return r;
    }

    void Handle(Peer& peer, const net::Message& m) {
        if (m.id != proto::LoginRequest::kId) {
            core::LogInfo("{}: {} sent message 0x{:04x}, which the login server does not take; closing", config.name, peer.address, m.id);
            peer.connection->Close();
            return;
        }
        proto::LoginRequest request;
        if (!proto::Decode(m.payload, request)) {
            peer.connection->Close();
            return;
        }
        if (peer.busy) return; // one attempt at a time per connection
        if (config.addressAttempts > 0 && !limiter->Allow(peer.address)) {
            Answer(peer.id, Status(proto::LoginStatus::TooManyAttempts));
            return;
        }
        if (!ValidAccountName(request.account) || !ValidPassword(request.password)) {
            Answer(peer.id, Status(proto::LoginStatus::Invalid));
            return;
        }
        std::string key = MissKey(request.account, peer.address);
        if (const auto it = misses.find(key); it != misses.end() && Clock::now() < it->second.until) {
            Answer(peer.id, Status(proto::LoginStatus::TooManyAttempts));
            return;
        }
        peer.busy = true;
        Lookup(peer.id, std::move(request), std::move(key));
    }

    static std::string MissKey(const std::string& account, const std::string& address) {
        std::string key;
        for (char c : account) key += char(std::tolower(static_cast<unsigned char>(c)));
        return key + '|' + address;
    }

    // 1. The account and the chosen game server.
    void Lookup(std::uint64_t peerId, proto::LoginRequest request, std::string key) {
        database.Submit([this, peerId, request = std::move(request), key = std::move(key)](Database& db) mutable {
            bool accountOk = false, serversOk = false, sessionOk = true;
            std::optional<AccountRow> account = FindAccount(db, request.account, accountOk);
            const bool online = account && HasLiveSession(db, account->id, config.staleSeconds, sessionOk);
            // Any of the shard's game servers can show the characters; where the chosen one stands
            // decides where it plays.
            const std::vector<GameServerInfo> servers = ListGameServers(db, config.staleSeconds, serversOk);
            std::optional<GameServerInfo> server = ChooseLobbyServer(servers, request.server, request.lobby);
            // The shard as a whole: its game servers' players against its capacity (all of them together).
            bool shardFull = false;
            if (server) {
                for (const GameServerInfo& shard : ShardsOf(servers))
                    if (shard.schema == server->schema) shardFull = shard.online >= shard.capacity;
            }
            completions->Post([this, peerId, request = std::move(request), key = std::move(key), account = std::move(account),
                               server = std::move(server), online, shardFull, ok = accountOk && serversOk && sessionOk]() mutable {
                if (!ok) return Answer(peerId, Status(proto::LoginStatus::Busy));
                // Everything about the account is told only after its password (a stranger learns nothing of it).
                Check(peerId, std::move(request.password), std::move(key), std::move(account),
                      [this, peerId, online, shardFull, server = std::move(server)](const AccountRow& account) mutable {
                          if (account.blocked) {
                              proto::LoginResult r = Status(proto::LoginStatus::Blocked);
                              r.reason = account.blockReason;
                              r.blockedUntil = account.blockedUntil;
                              return Answer(peerId, r);
                          }
                          if (online) return Answer(peerId, Status(proto::LoginStatus::AlreadyLoggedIn));
                          if (!server || !server->reporting) return Answer(peerId, Status(proto::LoginStatus::ServerUnavailable));
                          if (shardFull || server->online >= server->capacity) return Answer(peerId, Status(proto::LoginStatus::ServerFull));
                          Admit(peerId, account.id, std::move(*server));
                      });
            });
        });
    }

    // 2. The password, as a job (Argon2id takes tens of milliseconds): against our hash, or the
    // original's MD5 for an account that has only that (imported); that one gets our hash now.
    // An unknown name is checked against the decoy hash, so it takes a real check's time.
    void Check(std::uint64_t peerId, std::string password, std::string key, std::optional<AccountRow> account,
               std::function<void(const AccountRow&)> admitted) {
        struct Work {
            std::string password;
            std::optional<AccountRow> account;
            std::string upgraded; // our hash of an original's MD5 password
        };
        auto work = std::make_shared<Work>(Work{std::move(password), std::move(account), {}});
        config.jobs->Run(
            [this, peerId, work, key = std::move(key), admitted = std::move(admitted)]() mutable {
                bool right = false;
                if (!work->account) {
                    VerifyPassword(work->password, decoyHash);
                } else if (!work->account->passwordHash.empty()) {
                    right = VerifyPassword(work->password, work->account->passwordHash);
                } else if (VerifyLegacyPassword(work->password, work->account->legacyHash)) {
                    right = true;
                    work->upgraded = HashPassword(work->password); // ours from now on
                }
                work->password.assign(work->password.size(), '\0'); // the password is not kept
                completions->Post([this, peerId, work, right, key = std::move(key), admitted = std::move(admitted)] {
                    if (!right) return Refuse(peerId, key);
                    misses.erase(key);
                    if (!work->upgraded.empty()) {
                        database.Submit([this, account = work->account->id, phc = work->upgraded](Database& db) {
                            if (StorePasswordHash(db, account, phc, config.legacyPasswords)) {
                                core::LogInfo("{}: account {} logged in with the original's password; ours is stored", config.name, account);
                            }
                        });
                    }
                    admitted(*work->account);
                });
            },
            core::JobPriority::Normal, &checks);
    }

    // 3a. A wrong password (or no such account): counted for that name from that address; at the limit that address
    // waits (TooManyAttempts) while the account stays open to everyone else.
    void Refuse(std::uint64_t peerId, const std::string& key) {
        const Clock::time_point now = Clock::now();
        const auto window = std::chrono::seconds(config.blockSeconds);
        if (now - missesSwept > std::chrono::minutes(1)) {
            std::erase_if(misses, [&](const auto& e) { return now >= e.second.until && now - e.second.last > window; });
            missesSwept = now;
        }
        Misses& m = misses[key];
        if (now - m.last > window) m.count = 0; // a quiet spell forgets them
        m.last = now;
        ++m.count;
        const bool limit = m.count >= config.attemptLimit;
        proto::LoginResult r = Status(limit ? proto::LoginStatus::TooManyAttempts : proto::LoginStatus::WrongPassword);
        r.attempts = std::uint8_t(std::min(m.count, 255));
        r.attemptLimit = std::uint8_t(config.attemptLimit);
        if (limit) {
            m.until = now + window;
            m.count = 0;
            core::LogInfo("{}: {} wrong passwords for '{}'; that address waits {} s", config.name, config.attemptLimit, key, config.blockSeconds);
        }
        Answer(peerId, r);
    }

    // 3b. The right password: a ticket for the chosen game server.
    void Admit(std::uint64_t peerId, std::int64_t accountId, GameServerInfo server) {
        database.Submit([this, peerId, accountId, server = std::move(server)](Database& db) {
            RecordLogin(db, accountId);
            std::string error;
            std::vector<std::uint8_t> ticket = IssueTicket(db, accountId, server.id, config.ticketSeconds, error);
            if (ticket.empty()) core::LogError("{}: no ticket for account {}: {}", config.name, accountId, error);
            completions->Post([this, peerId, ticket = std::move(ticket), host = server.host, port = server.port] {
                if (ticket.empty()) return Answer(peerId, Status(proto::LoginStatus::Busy));
                proto::LoginResult r = Status(proto::LoginStatus::Accepted);
                r.host = host;
                r.port = port;
                r.ticket = ticket;
                Answer(peerId, r);
            });
        });
    }
};

LoginServer::LoginServer() = default;
LoginServer::~LoginServer() { Stop(); }

bool LoginServer::Start(const LoginConfig& config, std::string& error) {
    Stop();
    if (!config.jobs) {
        error = "the login server needs a job system";
        return false;
    }
    auto state = std::make_unique<State>();
    state->config = config;
    {
        auto db = Database::Connect(config.databaseUrl, error);
        if (!db) {
            error = "database: " + error;
            return false;
        }
        if (!MigrateSchema(*db, error)) return false;
        PurgeTickets(*db);
    }
    if (!state->database.Start(config.databaseUrl, config.databaseThreads, error)) {
        error = "database: " + error;
        return false;
    }
    state->net = std::make_unique<net::NetService>(config.networkThreads);
    state->completions = std::make_unique<Completions>(*state->net);
    state->listener = state->net->Listen(config.listen, error);
    if (!state->listener) return false;
    state->peers = std::make_unique<Peers>(config.name, config.peers);
    state->limiter = std::make_unique<AttemptLimiter>(config.addressAttempts, std::chrono::seconds(60));
    state->decoyHash = HashPassword("an unknown account's decoy");
    core::LogInfo("{}: listening on {}:{}", config.name, config.listen.address, state->listener->Port());
    state_ = std::move(state);
    return true;
}

void LoginServer::Poll(std::chrono::milliseconds wait) {
    if (!state_) return;
    SR_ZONE("LoginServer::Poll");
    State& s = *state_;
    s.net->WaitForActivity(wait);
    s.completions->Run();
    s.peers->Pump(*s.listener, [&](Peer& peer, const net::Message& m) { s.Handle(peer, m); });
}

void LoginServer::Run(const std::atomic<bool>& stop) {
    while (!stop) Poll(std::chrono::milliseconds(100));
}

void LoginServer::Stop() {
    if (!state_) return;
    state_->peers->CloseAll();
    state_->listener->Close();
    state_->config.jobs->Wait(state_->checks); // password checks post into the completions
    state_->database.Stop();
    state_.reset();
}

std::uint16_t LoginServer::Port() const { return state_ ? state_->listener->Port() : 0; }
LoginServer::Stats LoginServer::GetStats() const { return state_ ? state_->stats : Stats{}; }

} // namespace sr::server
