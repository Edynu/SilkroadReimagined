#pragma once

// The title's messages: the gateway's server list and notices, and the
// login. They carry what the original's title flow carries in
// our own form. Every connection starts with Hello / Welcome (Protocol.h).

#include <cstdint>
#include <string>
#include <vector>

#include "sr/game/Protocol.h"

namespace sr::game::proto {

// --- Gateway (0x0100) -------------------------------------------------------------------------

struct ServerListRequest {
    static constexpr MessageId kId = 0x0100;
    template <typename V>
    void Visit(V&) {}
};

struct ServerGroup {
    std::uint8_t id = 0;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("id", id);
        v("name", name, 32);
    }
};

enum class ServerState : std::uint8_t {
    Offline = 0, // not reporting (stopped, or checking): listed, not joinable
    Open = 1,
    Full = 2, // online players reached the capacity
};

struct ServerEntry {
    std::uint16_t id = 0;
    std::string name;
    std::uint8_t group = 0;
    std::uint16_t online = 0;
    std::uint16_t capacity = 0;
    ServerState state = ServerState::Offline;
    template <typename V>
    void Visit(V& v) {
        v("id", id);
        v("name", name, 32);
        v("group", group);
        v("online", online);
        v("capacity", capacity);
        v("state", state);
    }
};

struct ServerList {
    static constexpr MessageId kId = 0x0101;
    std::vector<ServerGroup> groups;
    std::vector<ServerEntry> servers;
    // Where to log in.
    std::string loginHost;
    std::uint16_t loginPort = 0;
    template <typename V>
    void Visit(V& v) {
        v("groups", groups, 64);
        v("servers", servers, 256);
        v("loginHost", loginHost, 255);
        v("loginPort", loginPort);
    }
};

struct NoticesRequest {
    static constexpr MessageId kId = 0x0102;
    template <typename V>
    void Visit(V&) {}
};

struct Notice {
    std::string title;
    std::string body;
    std::int64_t posted = 0; // Unix seconds
    template <typename V>
    void Visit(V& v) {
        v("title", title, 128);
        v("body", body, 4096);
        v("posted", posted);
    }
};

struct Notices {
    static constexpr MessageId kId = 0x0103;
    std::vector<Notice> notices;
    template <typename V>
    void Visit(V& v) {
        v("notices", notices, 16);
    }
};

// --- Login (0x0200) ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxAccountName = 32;
inline constexpr std::size_t kMaxPassword = 64;
inline constexpr std::size_t kTicketBytes = 32;

// Never Describe a LoginRequest or a LoginResult into a log: they carry the password and the ticket.
struct LoginRequest {
    static constexpr MessageId kId = 0x0200;
    std::string account;
    std::string password;
    std::uint16_t server = 0; // the shard chosen from the list (its row's id)
    std::uint16_t lobby = 0;  // developers: which of its game servers shows the characters (0: the one with the most room)
    template <typename V>
    void Visit(V& v) {
        v("account", account, kMaxAccountName);
        v("password", password, kMaxPassword);
        v("server", server);
        v("lobby", lobby);
    }
};

// The original's login failures, plus our own limits.
enum class LoginStatus : std::uint8_t {
    Accepted = 0,
    WrongPassword = 1,     // `attempts` of `attemptLimit`; at the limit the account is blocked for a while
    UnknownAccount = 2,
    Blocked = 3,           // `reason`, until `blockedUntil` (0: no end)
    AlreadyLoggedIn = 4,
    ServerFull = 5,
    ServerUnavailable = 6, // unknown or offline game server
    TooManyAttempts = 7,   // from this address; try again later
    Invalid = 8,           // a malformed account name or password
    Busy = 9,              // the login server cannot answer now (its database)
};

struct LoginResult {
    static constexpr MessageId kId = 0x0201;
    LoginStatus status = LoginStatus::Accepted;
    std::uint8_t attempts = 0;
    std::uint8_t attemptLimit = 0;
    std::string reason;
    std::int64_t blockedUntil = 0; // Unix seconds
    // Accepted: the game server to connect to, and the one-time ticket it admits.
    std::string host;
    std::uint16_t port = 0;
    std::vector<std::uint8_t> ticket;
    template <typename V>
    void Visit(V& v) {
        v("status", status);
        v("attempts", attempts);
        v("attemptLimit", attemptLimit);
        v("reason", reason, 256);
        v("blockedUntil", blockedUntil);
        v("host", host, 255);
        v("port", port);
        v("ticket", ticket, kTicketBytes);
    }
};

} // namespace sr::game::proto
