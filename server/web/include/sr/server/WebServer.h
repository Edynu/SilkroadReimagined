#pragma once

// the website's side of the servers. A small HTTP API the site reads,
// behind a reverse proxy that adds TLS (it listens on 127.0.0.1 by default), and optionally the built
// site itself. Like every server it meets the others only in the database: it never connects to a game
// server, so it can run on any machine that reaches the database.
//
//   GET  /api/status     the shards: up, players, capacity, game servers up (cached a few seconds)
//   GET  /api/server     the shard's name, level cap, rates, whether registration is open and needs a key
//   GET  /api/rankings   the top players, guilds and job holders (cached minutes; Store.h LoadRankings)
//   GET  /api/uniques    each unique monster: alive or not since when, its last killer (the game servers'
//                        sr_unique_events; cached a few seconds)
//   POST /api/register   {"name", "password", "email"?, "key"?}: an account as SroLogin --create-account
//                        makes one (Store.h CreateAccount), attempts limited per address
//
// Its threads: the listener and a few workers (cpp-httplib's), which block on sockets, and the database
// pool's; reads are answered from caches the pool refreshes, so a burst of visitors costs the database
// one query per cache period.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sr/server/Database.h"
#include "sr/server/Store.h"

namespace sr::server {

struct WebConfig {
    std::string databaseUrl;
    std::string host = "127.0.0.1"; // behind the reverse proxy; 0.0.0.0 to be reached directly
    std::uint16_t port = 15790;     // 0: any free port (the tests)
    std::string siteRoot;           // the built site (website/dist), served at /; empty: the API only
    // The site's public address (scheme://host, e.g. https://example.com): the sitemap's and robots.txt's links. Empty:
    // the request's own host (https behind a trusted proxy that says so, else http).
    std::string siteUrl;
    bool trustProxy = false;        // take the visitor's address from X-Forwarded-For when the peer is local
    int threads = 4;                // workers answering requests

    // What the site shows: the shard's settings (the same file the game servers read).
    std::string shardName;
    int levelCap = 110;
    double rateExp = 1.0, rateSp = 1.0, rateGold = 1.0, rateDrop = 1.0;

    // Registration.
    bool registration = true;
    bool betaKeys = false; // an account needs a key from SroWeb --make-keys
    int registerAttempts = 5;
    std::chrono::seconds registerWindow{3600};
    LegacyPasswords legacyPasswords = LegacyPasswords::Keep;

    // The administration (sr/server/WebAdmin.h).
    bool admin = true;
    // Addresses or ranges (a.b.c.d/n, IPv6 x:y::/n) the admin API answers; 127.0.0.1, ::1 and "localhost" are one (this
    // machine, however the browser reaches it). Empty: any.
    std::vector<std::string> adminAllow;
    std::vector<std::string> origins;    // more origins a change may come from (scheme://host[:port]); its own always
    bool secureCookie = true;            // the session cookie only over HTTPS (browsers count http://localhost as secure)
    // The developer pages (/dev) without a login for this machine (a developer's own shard, deploy/dev-shard.cfg): a request
    // from 127.0.0.1 / ::1 with no proxy's forwarding header is the developer "local" (the tester role: /dev alone; /admin
    // still asks for a login). Never on a shard behind a reverse proxy on the same machine: its visitors come from there too.
    bool devLocal = false;
    int adminLoginAttempts = 10;         // admin logins tried per address in 15 minutes
    std::string filesDir = "web-files";  // uploads: images/ (served at /files/)
    // The client's download: SroDeploy's patch folder (its <channel>/client.json names the players' zip beside it, made
    // when a client patch is published), and the address the web server serves that folder at. Empty: no download.
    std::string patchDir;
    std::string patchUrl = "/patch";
    // Support's attachments (screenshots, videos): never served to the public (the administration reads them).
    std::string supportDir = "web-support";
    int supportAttempts = 5; // tickets sent per address an hour (0: no limit)
    std::int64_t supportMaxBytes = std::int64_t(4096) << 20;   // all support files together (the disk's guard)
    std::int64_t supportTicketMaxBytes = std::int64_t(200) << 20; // one ticket's files together
    // Requests to /api an address may make a minute (0: no limit; SroWeb's setting web-api-per-minute is 300 unless set):
    // a page asks a few, a script hammering it is answered 429 before it costs the database anything.
    int apiPerMinute = 0;
    std::string labDir = "web-lab";      // the skill lab's videos (videos/; served to the developer pages alone)
    std::int64_t imageMaxBytes = std::int64_t(8) << 20;   // an image's largest (8 MB)

    std::string shardSchema = kDefaultShardSchema; // its tables and its bus (the game servers' --shard-schema)
    std::chrono::seconds statusAge{5}, rankingAge{300};
    int rankingCount = 10;
    int staleSeconds = 15; // a game server silent this long is down (as the gateway's)
};

class WebServer {
public:
    WebServer();
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    bool Start(const WebConfig& config, std::string& error);
    // What the shard's game servers said on its bus since the last call (a unique came or fell: the list is
    // read again at once). Called often by its owner's loop (SroWeb's main).
    void Poll();
    void Stop();
    std::uint16_t Port() const;

    struct State;

private:
    std::unique_ptr<State> s_;
};

} // namespace sr::server
