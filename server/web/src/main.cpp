// SroWeb.exe - the website's API (sr/server/WebServer.h). Reads the shard's settings
// file like the other servers (--config deploy\dev-shard.cfg: its name, level cap and rates are what the site
// shows). Options: sr/server/Program.h (--config, --db), and
//   web-host (127.0.0.1), web-port (15790)    where it listens (behind the reverse proxy)
//   web-root DIR                             also serve the built site (website\dist) at /
//   web-trust-proxy                          the visitor's address from X-Forwarded-For (a local proxy)
//   web-site-url URL                         the site's public address (https://example.com): /sitemap.xml and
//                                            /robots.txt link to it (unset: the request's own host)
//   web-registration (true)                  accounts can be made on the site
//   web-beta-keys (false)                    an account needs a key
//   web-register-attempts (5)                accounts tried per address an hour (0: no limit)
//   web-threads (4)                          workers answering requests
//   --legacy-passwords keep|clear            as SroLogin.exe
//   --make-keys N [--key-note TEXT]          makes N beta keys, prints them once and exits
//   the administration (sr/server/WebAdmin.h; its accounts are made only here):
//   --make-admin NAME [--role admin|moderator|tester] [--password P]   an admin (a password made up when none is given;
//                                            a tester reaches the developer pages /dev alone),
//                                            its authenticator secret printed once (add it to an app: a login needs its code)
//   --admin-new-code NAME                    a new authenticator secret (the old one and its sessions end)
//   --admin-password NAME --password P       a new password (its sessions end)
//   --admin-disable NAME, --admin-enable NAME
//   --make-dev-key NAME                      a key for scripts and agents on the developer pages' API (/api/admin/dev,
//                                            "Authorization: Bearer srdev_..."), printed once; --dev-key-disable NAME, --dev-key-enable NAME
//   web-admin (true)                         the administration's API at /api/admin
//   web-admin-allow (any)                    addresses or IPv4 ranges it answers, e.g. "127.0.0.1, 10.0.0.0/8"
//   web-origins                              more origins a change may come from (e.g. the dev page http://localhost:5173)
//   web-cookie-secure (true)                 the session cookie only over HTTPS (http://localhost counts as secure)
//   web-dev-local (false)                    the developer pages (/dev) without a login from this machine (127.0.0.1 / ::1,
//                                            no proxy's forwarding header): a developer's own shard only, never behind a proxy
//   web-files (web-files)                    the uploads' folder (images/, client/; served at /files/)
//   web-upload-max-mb (20480)                the client upload's largest

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

#include "sr/core/Log.h"
#include "sr/server/Database.h"
#include "sr/server/Program.h"
#include "sr/server/Crypto.h"
#include "sr/server/Store.h"
#include "sr/server/WebAdmin.h"
#include "sr/server/WebServer.h"

int main(int argc, char** argv) {
    using namespace sr;
    server::ProgramOptions options;
    std::string error;
    if (!server::ReadProgramOptions(argc, argv, "SroWeb", 15790, options, error)) {
        core::LogError("SroWeb: {}", error);
        return 1;
    }
    const core::Config& c = options.config;

    if (c.Has("make-keys")) {
        auto db = server::Database::Connect(options.databaseUrl, error);
        if (!db || !server::MigrateSchema(*db, error)) {
            core::LogError("SroWeb: {}", error);
            return 1;
        }
        const auto keys = server::MakeBetaKeys(*db, std::clamp(c.GetInt("make-keys", 1), 1, 10000), c.GetString("key-note", ""), error);
        if (keys.empty()) {
            core::LogError("SroWeb: {}", error);
            return 1;
        }
        for (const std::string& key : keys) std::printf("%s\n", key.c_str());
        return 0;
    }

    // The developer pages' keys.
    for (const char* key : {"make-dev-key", "dev-key-disable", "dev-key-enable"}) {
        if (!c.Has(key)) continue;
        auto db = server::Database::Connect(options.databaseUrl, error);
        if (!db || !server::MigrateSchema(*db, error)) {
            core::LogError("SroWeb: {}", error);
            return 1;
        }
        const std::string name = c.GetString(key, "");
        std::string made;
        const bool ok = std::string_view(key) == "make-dev-key" ? server::MakeDevKey(*db, name, made, error)
                                                                : server::SetDevKeyDisabled(*db, name, std::string_view(key) == "dev-key-disable", error);
        if (!ok) {
            core::LogError("SroWeb: {}", error);
            return 1;
        }
        if (!made.empty()) std::printf("%s\n", made.c_str());
        core::LogInfo("SroWeb: {} {}", key, name);
        return 0;
    }

    // The administration's accounts, on the command line only.
    for (const char* key : {"make-admin", "admin-new-code", "admin-password", "admin-disable", "admin-enable"}) {
        if (!c.Has(key)) continue;
        auto db = server::Database::Connect(options.databaseUrl, error);
        if (!db || !server::MigrateSchema(*db, error)) {
            core::LogError("SroWeb: {}", error);
            return 1;
        }
        const std::string name = c.GetString(key, "");
        std::string secret, password = c.GetString("password", "");
        bool ok = false;
        if (std::string_view(key) == "make-admin") {
            const bool madeUp = password.empty();
            if (madeUp) password = server::Base32(server::RandomBytes(15)); // 24 letters and digits
            ok = server::MakeAdmin(*db, name, c.GetString("role", "admin"), password, secret, error);
            if (ok && madeUp) std::printf("password: %s\n", password.c_str());
        } else if (std::string_view(key) == "admin-new-code") {
            ok = server::ResetAdminSecret(*db, name, secret, error);
        } else if (std::string_view(key) == "admin-password") {
            ok = server::SetAdminPassword(*db, name, password, error);
        } else {
            ok = server::SetAdminDisabled(*db, name, std::string_view(key) == "admin-disable", error);
        }
        if (!ok) {
            core::LogError("SroWeb: {}", error);
            return 1;
        }
        if (!secret.empty()) {
            std::printf("authenticator secret: %s\n%s\n", secret.c_str(), server::AuthenticatorUri(name, secret).c_str());
            std::printf("Add it to an authenticator app (Google Authenticator, Aegis, 1Password, ...): a login needs its 6-digit code.\n");
        }
        core::LogInfo("SroWeb: {} {}", key, name);
        return 0;
    }

    server::WebConfig web;
    web.databaseUrl = options.databaseUrl;
    web.host = c.GetString("web-host", "127.0.0.1");
    web.port = std::uint16_t(c.GetInt("web-port", 15790));
    web.siteRoot = c.GetString("web-root", "");
    web.siteUrl = c.GetString("web-site-url", "");
    while (!web.siteUrl.empty() && web.siteUrl.back() == '/') web.siteUrl.pop_back();
    web.trustProxy = c.GetBool("web-trust-proxy", false);
    web.threads = c.GetInt("web-threads", web.threads);
    web.registration = c.GetBool("web-registration", true);
    web.betaKeys = c.GetBool("web-beta-keys", false);
    web.registerAttempts = c.GetInt("web-register-attempts", web.registerAttempts);
    web.legacyPasswords = c.GetString("legacy-passwords", "keep") == "clear" ? server::LegacyPasswords::Clear : server::LegacyPasswords::Keep;
    // The shard's settings, read as the game servers read them (SroGame's main.cpp).
    web.shardName = c.GetString("shard-name", "");
    web.shardSchema = c.GetString("shard-schema", server::kDefaultShardSchema);
    const auto list = [&](const char* key) {
        std::vector<std::string> out;
        std::string all = c.GetString(key, "");
        for (std::size_t at = 0; at <= all.size();) {
            std::size_t end = all.find(',', at);
            if (end == std::string::npos) end = all.size();
            std::string item = all.substr(at, end - at);
            item.erase(0, item.find_first_not_of(' '));
            item.erase(item.find_last_not_of(' ') + 1);
            if (!item.empty()) out.push_back(item);
            at = end + 1;
        }
        return out;
    };
    web.admin = c.GetBool("web-admin", true);
    web.adminAllow = list("web-admin-allow");
    web.origins = list("web-origins");
    web.secureCookie = c.GetBool("web-cookie-secure", true);
    web.devLocal = c.GetBool("web-dev-local", false);
    web.filesDir = c.GetString("web-files", "web-files");
    web.labDir = c.GetString("web-lab", "web-lab");
    web.uploadMaxBytes = std::int64_t(std::clamp(c.GetInt("web-upload-max-mb", 20480), 1, 200000)) << 20;
    web.levelCap = std::clamp(c.GetInt("level-cap", 110), 1, 140);
    web.rateExp = c.GetDouble("rate-exp", 1.0);
    web.rateSp = c.GetDouble("rate-sp", 1.0);
    web.rateGold = c.GetDouble("rate-gold", 1.0);
    web.rateDrop = c.GetDouble("rate-drop-equipment", 1.0);

    // The tables it reads and writes are there, made or brought up to date as the other servers do.
    {
        auto db = server::Database::Connect(options.databaseUrl, error);
        if (!db || !server::MigrateSchema(*db, error)) {
            core::LogError("SroWeb: {}", error);
            return 1;
        }
    }
    server::WebServer server;
    if (!server.Start(web, error)) {
        core::LogError("SroWeb: {}", error);
        return 1;
    }
    core::LogInfo("SroWeb: the site's API on http://{}:{}/api{}{}", web.host, server.Port(), web.siteRoot.empty() ? "" : ", the site from ",
                  web.siteRoot);
    if (web.betaKeys) core::LogInfo("SroWeb: registration needs a beta key (SroWeb --make-keys N)");
    if (web.admin) {
        core::LogInfo("SroWeb: the administration at /admin ({}); accounts: SroWeb --make-admin NAME",
                      web.adminAllow.empty() ? std::string("from any address") : "from " + c.GetString("web-admin-allow", ""));
    }
    server::InstallStopHandler();
    // The bus asked a few times a second: a unique's coming or death reaches the site at once.
    while (!server::StopRequested().load()) {
        server.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server.Stop();
    core::LogInfo("SroWeb: stopped");
    return 0;
}
