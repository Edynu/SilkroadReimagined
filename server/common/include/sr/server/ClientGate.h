#pragma once

// The oldest client a shard lets in (shard.cfg; every server program, checked at Hello: the gateway turns an old client
// away before it logs in). The players' client names itself "SroReimagined <version>" in Hello (its build's version);
// one older than the floor is answered ProtocolMismatch with words that say where to get the new one (the clients
// before 0.1.3 named themselves "SroReimagined 0.1" and show their own "out of date" line).
//
//   min-client-version = 0.1.3          a fixed floor (unset: none)
//   client-version-from-patch = true    the floor follows the client SroDeploy published last (web-patch-dir's
//                                       windows-x64/client.json "version", read again every minute): an upload of a new
//                                       client turns the older ones away, without a restart
//   client-download-url = https://...   where the refusal sends the player (the website's download page)
// Other programs (the bots, the tests' clients) do not name themselves so and are let in.

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "sr/core/Config.h"

namespace sr::server {

class ClientVersionGate {
public:
    ClientVersionGate(std::string minimum, std::string patchDir, std::string downloadUrl);
    // From a program's settings; none when neither floor is set.
    static std::shared_ptr<ClientVersionGate> FromConfig(const core::Config& config);

    // The words of the refusal for a client that greets so, or none (let in).
    std::optional<std::string> Refuse(std::string_view client);
    // The floor now: the fixed one or the published one, the newer (empty: none).
    std::string Required();

    // "0.1.2" against "0.1.10": by their numbers, a missing one 0.
    static bool Older(std::string_view version, std::string_view than);
    // The version in a client's Hello name ("SroReimagined 0.1.2"), or none for other programs.
    static std::optional<std::string> ClientVersion(std::string_view client);
    // The "version" of SroDeploy's client.json text (empty: none).
    static std::string PublishedVersion(std::string_view json);

private:
    std::string minimum_, patchDir_, downloadUrl_;
    std::mutex mutex_; // the gateway and the login server of SroControl share one
    std::string published_;
    std::chrono::steady_clock::time_point readAt_{};
};

} // namespace sr::server
