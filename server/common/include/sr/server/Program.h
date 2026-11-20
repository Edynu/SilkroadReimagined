#pragma once

// The command line shared by the server programs (SroGateway, SroLogin, SroGame).
//
//   --config FILE          settings file (key = value; default: <program>.cfg if it exists)
//   --db URL               PostgreSQL connection (default: the SR_DB_URL environment variable)
//   db-host, db-port, db-name, db-user, db-password: the connection by parts when no URL is given (the password
//                          better from the SR_DB_PASSWORD environment variable)
//   --listen ADDRESS       default 0.0.0.0
//   --port N
//   --certificate FILE     PEM certificate (default dev/server.pem)
//   --key FILE             PEM private key (default dev/server.key)
//   --dev-certificate      make a self-signed certificate for --tls-name if the files are missing
//   --tls-name NAME        default localhost
//   connections-per-address N   connections open at once from one address (default 8; 0 no limit; loopback exempt)
//   connects-per-minute N       new connections from one address within a minute (default 30; 0 no limit)
//   messages-per-second N, message-burst N   a connection's message budget (default 50, 200; 0 none; Service.h PeerLimits)

#include <atomic>
#include <string>

#include "sr/core/Config.h"
#include "sr/net/Connection.h"

namespace sr::server {

struct ProgramOptions {
    core::Config config;
    std::string databaseUrl;
    net::ListenConfig listen;
    double messagesPerSecond = 50.0, messageBurst = 200.0; // copied into each server's PeerLimits
};

// Parses the command line and the settings file. False (with `error`) when the program cannot start.
bool ReadProgramOptions(int argc, char** argv, const char* program, std::uint16_t defaultPort, ProgramOptions& out,
                        std::string& error);
// Loads (or, with --dev-certificate, makes) the certificate into `out.listen.tls`, before serving.
bool LoadProgramCertificate(ProgramOptions& out, std::string& error);

// Set when the console asks the program to stop (Ctrl+C, closing the window).
const std::atomic<bool>& StopRequested();
void InstallStopHandler();

} // namespace sr::server
