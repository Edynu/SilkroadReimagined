// SroLogin.exe - the login server. Options: sr/server/Program.h, and
//   --workers N                                 password-check jobs (default: the hardware's)
//   --create-account NAME --password PW         adds an account and exits
//   --block NAME --reason TEXT [--seconds N]    blocks an account (no --seconds: no end) and exits
//   --unblock NAME                              and exits
//   --make-certificate                          makes the development certificate if missing, exits
//   --address-attempts N                        login attempts an address may make a minute (20; 0: no limit,
//                                               as start-dev.bat runs it: every local client is 127.0.0.1)
//   --legacy-passwords keep|clear               tb_user.password beside our hash: the original's MD5
//                                               (the owner's web tools keep working; default) or nothing

#include "sr/core/Jobs.h"
#include "sr/core/Log.h"
#include "sr/server/Database.h"
#include "sr/server/LoginServer.h"
#include "sr/server/Program.h"
#include "sr/server/Store.h"

namespace {

int Administer(const sr::server::ProgramOptions& options) {
    using namespace sr;
    std::string error;
    auto db = server::Database::Connect(options.databaseUrl, error);
    if (!db || !server::MigrateSchema(*db, error)) {
        core::LogError("SroLogin: {}", error);
        return 1;
    }
    const core::Config& c = options.config;
    if (c.Has("create-account")) {
        const std::string name = c.GetString("create-account");
        const auto legacy = c.GetString("legacy-passwords", "keep") == "clear" ? server::LegacyPasswords::Clear : server::LegacyPasswords::Keep;
        switch (server::CreateAccount(*db, name, c.GetString("password"), error, legacy)) {
        case server::CreateAccountResult::Created: core::LogInfo("SroLogin: account {} created", name); return 0;
        case server::CreateAccountResult::NameTaken: core::LogError("SroLogin: {} exists already", name); return 1;
        case server::CreateAccountResult::InvalidName: core::LogError("SroLogin: names are 3-32 letters, digits or _"); return 1;
        case server::CreateAccountResult::InvalidPassword: core::LogError("SroLogin: passwords are 6-64 bytes"); return 1;
        case server::CreateAccountResult::KeyRefused: // no beta key is given here
        case server::CreateAccountResult::Failed: core::LogError("SroLogin: {}", error); return 1;
        }
        return 1;
    }
    if (c.Has("block")) {
        const bool ok = server::SetBlock(*db, c.GetString("block"), c.GetString("reason", "blocked"), c.GetInt("seconds", 0));
        core::LogInfo("SroLogin: {}", ok ? "blocked" : "no such account");
        return ok ? 0 : 1;
    }
    const bool ok = server::SetBlock(*db, c.GetString("unblock"), "", 0);
    core::LogInfo("SroLogin: {}", ok ? "unblocked" : "no such account");
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    using namespace sr;
    server::ProgramOptions options;
    std::string error;
    if (!server::ReadProgramOptions(argc, argv, "SroLogin", 15780, options, error)) {
        core::LogError("SroLogin: {}", error);
        return 1;
    }
    const core::Config& c = options.config;
    if (c.Has("make-certificate")) {
        // Development: makes the certificate every server and the client share, then exits.
        options.config.Set("dev-certificate", "true");
        if (!server::LoadProgramCertificate(options, error)) {
            core::LogError("SroLogin: {}", error);
            return 1;
        }
        return 0;
    }
    if (c.Has("create-account") || c.Has("block") || c.Has("unblock")) return Administer(options);

    core::JobSystem jobs(c.GetInt("workers", core::JobSystem::kDefaultWorkers));
    server::LoginConfig config;
    config.databaseUrl = options.databaseUrl;
    config.jobs = &jobs;
    config.addressAttempts = c.GetInt("address-attempts", config.addressAttempts);
    config.legacyPasswords = c.GetString("legacy-passwords", "keep") == "clear" ? server::LegacyPasswords::Clear : server::LegacyPasswords::Keep;
    if (!server::LoadProgramCertificate(options, error)) {
        core::LogError("SroLogin: {}", error);
        return 1;
    }
    config.listen = options.listen;
    config.peers.messagesPerSecond = options.messagesPerSecond;
    config.peers.messageBurst = options.messageBurst;
    config.peers.clientGate = options.clientGate;
    server::LoginServer login;
    if (!login.Start(config, error)) {
        core::LogError("SroLogin: {}", error);
        return 1;
    }
    server::InstallStopHandler();
    login.Run(server::StopRequested());
    login.Stop();
    core::LogInfo("SroLogin: stopped");
    return 0;
}
