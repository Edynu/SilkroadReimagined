// SroGateway.exe - the gateway. Options: sr/server/Program.h, and
//   --login-host HOST --login-port N   where clients log in (sent with the server list)
//   --post-notice TITLE --body TEXT    adds a notice and exits

#include "sr/core/Log.h"
#include "sr/server/Database.h"
#include "sr/server/GatewayServer.h"
#include "sr/server/Program.h"
#include "sr/server/Store.h"

int main(int argc, char** argv) {
    using namespace sr;
    server::ProgramOptions options;
    std::string error;
    if (!server::ReadProgramOptions(argc, argv, "SroGateway", 15779, options, error)) {
        core::LogError("SroGateway: {}", error);
        return 1;
    }
    if (options.config.Has("post-notice")) {
        auto db = server::Database::Connect(options.databaseUrl, error);
        if (!db || !server::MigrateSchema(*db, error)) {
            core::LogError("SroGateway: {}", error);
            return 1;
        }
        const bool ok = server::PostNotice(*db, options.config.GetString("post-notice"), options.config.GetString("body"));
        core::LogInfo("SroGateway: notice {}", ok ? "posted" : "not posted");
        return ok ? 0 : 1;
    }

    server::GatewayConfig config;
    config.databaseUrl = options.databaseUrl;
    config.loginHost = options.config.GetString("login-host", "127.0.0.1");
    config.loginPort = std::uint16_t(options.config.GetInt("login-port", 15780));
    if (!server::LoadProgramCertificate(options, error)) {
        core::LogError("SroGateway: {}", error);
        return 1;
    }
    config.listen = options.listen;
    config.peers.messagesPerSecond = options.messagesPerSecond;
    config.peers.messageBurst = options.messageBurst;
    server::GatewayServer gateway;
    if (!gateway.Start(config, error)) {
        core::LogError("SroGateway: {}", error);
        return 1;
    }
    server::InstallStopHandler();
    gateway.Run(server::StopRequested());
    gateway.Stop();
    core::LogInfo("SroGateway: stopped");
    return 0;
}
