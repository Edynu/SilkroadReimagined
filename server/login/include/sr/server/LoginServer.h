#pragma once

// The login server: checks an account and its password for the game server
// the player chose, and answers with that game server's address and a one-time ticket (kept in the
// database, where the game server takes it). Failures carry what the original's title shows: wrong password with the
// attempts and the limit, unknown
// account, blocked with the reason and until when, the server full or offline; and our own limit
// of attempts per address.
//
// Nothing blocks the loop: database calls run on the pool's threads, the password check
// (Argon2id) as a job; each step posts the next back to the loop.

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include "sr/core/Jobs.h"
#include "sr/net/Connection.h"
#include "sr/server/Crypto.h"
#include "sr/server/Service.h"
#include "sr/server/Store.h"

namespace sr::server {

struct LoginConfig {
    std::string name = "SroLogin";
    net::ListenConfig listen;
    std::string databaseUrl;
    unsigned networkThreads = 2;
    unsigned databaseThreads = 2;
    core::JobSystem* jobs = nullptr; // runs the password checks (required)
    // Wrong passwords in a row for one account from one address before that address waits, and for how long (the
    // original's numbers, server.cfg GlobalManager: LoginFailureTolerance 3, LoginFailureBlockTimeMin 10; the original
    // blocked the account itself, which let anyone lock its owner out: ours holds the address alone).
    int attemptLimit = 3;
    int blockSeconds = 600;
    // Login attempts per address per minute (0: no limit).
    int addressAttempts = 20;
    // How long a ticket admits its player to the game server.
    int ticketSeconds = 30;
    // A game server whose last report is older counts as offline.
    int staleSeconds = 15;
    // tb_user.password after an account with only the original's MD5 logs in.
    LegacyPasswords legacyPasswords = LegacyPasswords::Keep;
    PeerLimits peers;
};

class LoginServer {
public:
    LoginServer();
    ~LoginServer();
    bool Start(const LoginConfig& config, std::string& error);
    void Poll(std::chrono::milliseconds wait);
    void Run(const std::atomic<bool>& stop);
    void Stop();
    std::uint16_t Port() const;

    struct Stats {
        std::uint64_t accepted = 0, refused = 0;
    };
    Stats GetStats() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace sr::server
