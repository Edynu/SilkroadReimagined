#include "sr/server/Program.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <csignal>
#include <cstdlib>

#include "sr/server/Service.h"

namespace sr::server {

namespace {

std::atomic<bool> gStop{false};

// A part of a connection URL with the characters URLs reserve percent-encoded (a password's @, :, / ...).
std::string UrlPart(const std::string& text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

extern "C" void OnStopSignal(int) { gStop = true; }

} // namespace

bool ReadProgramOptions(int argc, char** argv, const char* program, std::uint16_t defaultPort, ProgramOptions& out,
                        std::string& error) {
    core::Config commandLine;
    if (!commandLine.ParseCommandLine(argc, argv, error)) return false;
    const std::string file = commandLine.GetString("config", std::string(program) + ".cfg");
    // A file asked for by name must be there: without it a server would quietly run as something else.
    if (commandLine.Has("config") && !std::filesystem::exists(file)) {
        error = "no such settings file: " + file;
        return false;
    }
    if (!out.config.LoadFile(file, error)) return false;
    if (!out.config.ParseCommandLine(argc, argv, error)) return false; // the command line wins

    // The database: a whole URL (db, or SR_DB_URL), else its parts (db-host, db-port, db-name, db-user and db-password,
    // or the password from SR_DB_PASSWORD, which keeps it out of the settings file).
    out.databaseUrl = out.config.GetString("db");
    if (out.databaseUrl.empty()) {
        if (const char* env = std::getenv("SR_DB_URL")) out.databaseUrl = env;
    }
    if (out.databaseUrl.empty() && out.config.Has("db-name")) {
        std::string password = out.config.GetString("db-password");
        if (password.empty()) {
            if (const char* env = std::getenv("SR_DB_PASSWORD")) password = env;
        }
        const std::string user = out.config.GetString("db-user", "sro");
        out.databaseUrl = std::format("postgresql://{}{}@{}:{}/{}", UrlPart(user), password.empty() ? "" : ":" + UrlPart(password),
                                      out.config.GetString("db-host", "127.0.0.1"), out.config.GetInt("db-port", 5432),
                                      UrlPart(out.config.GetString("db-name")));
    }
    if (out.databaseUrl.empty()) {
        error = "no database: set db-host, db-port, db-name, db-user (and db-password or SR_DB_PASSWORD) in the settings, "
                "or pass --db URL, or set SR_DB_URL (postgresql://user:password@host:port/database)";
        return false;
    }
    out.listen.address = out.config.GetString("listen", "0.0.0.0");
    out.listen.port = std::uint16_t(out.config.GetInt("port", defaultPort));
    // One address's share of the listener (a household or a cafe behind one address fits; a flood does not).
    out.listen.maxPerAddress = std::size_t(std::max(0, out.config.GetInt("connections-per-address", 8)));
    out.listen.acceptsPerMinute = std::size_t(std::max(0, out.config.GetInt("connects-per-minute", 30)));
    out.messagesPerSecond = std::max(0.0, double(out.config.GetInt("messages-per-second", 50)));
    out.messageBurst = std::max(1.0, double(out.config.GetInt("message-burst", 200)));
    return true;
}

bool LoadProgramCertificate(ProgramOptions& out, std::string& error) {
    return LoadCertificate(out.config.GetString("certificate", "dev/server.pem"), out.config.GetString("key", "dev/server.key"),
                           out.config.GetBool("dev-certificate", false), out.config.GetString("tls-name", "localhost"),
                           out.listen.tls, error);
}

const std::atomic<bool>& StopRequested() { return gStop; }

void InstallStopHandler() {
    std::signal(SIGINT, OnStopSignal);
    std::signal(SIGTERM, OnStopSignal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, OnStopSignal);
#endif
}

} // namespace sr::server
