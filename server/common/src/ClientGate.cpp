#include "sr/server/ClientGate.h"

#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <vector>

#include "sr/core/Log.h"

namespace sr::server {

namespace {

constexpr std::string_view kClientName = "SroReimagined ";
constexpr auto kReadAgain = std::chrono::minutes(1);

std::vector<long long> Numbers(std::string_view v) {
    std::vector<long long> out;
    long long n = 0;
    bool any = false;
    for (const char c : v) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            n = n * 10 + (c - '0');
            any = true;
        } else if (c == '.') {
            out.push_back(any ? n : 0);
            n = 0;
            any = false;
        } else {
            break; // "0.1.3-beta": the numbers before it
        }
    }
    out.push_back(any ? n : 0);
    return out;
}

} // namespace

ClientVersionGate::ClientVersionGate(std::string minimum, std::string patchDir, std::string downloadUrl)
    : minimum_(std::move(minimum)), patchDir_(std::move(patchDir)), downloadUrl_(std::move(downloadUrl)) {}

std::shared_ptr<ClientVersionGate> ClientVersionGate::FromConfig(const core::Config& config) {
    const std::string minimum = config.GetString("min-client-version", "");
    const std::string patch = config.GetBool("client-version-from-patch", false) ? config.GetString("web-patch-dir", "") : std::string();
    if (minimum.empty() && patch.empty()) return nullptr;
    auto gate = std::make_shared<ClientVersionGate>(minimum, patch, config.GetString("client-download-url", ""));
    core::LogInfo("clients older than {} are turned away{}", gate->Required().empty() ? std::string("the published one") : gate->Required(),
                  patch.empty() ? "" : std::format(" (the floor follows {})", patch));
    return gate;
}

bool ClientVersionGate::Older(std::string_view version, std::string_view than) {
    std::vector<long long> a = Numbers(version), b = Numbers(than);
    a.resize(std::max(a.size(), b.size()), 0);
    b.resize(a.size(), 0);
    return a < b;
}

std::optional<std::string> ClientVersionGate::ClientVersion(std::string_view client) {
    if (!client.starts_with(kClientName)) return std::nullopt;
    std::string v(client.substr(kClientName.size()));
    if (v.empty() || !std::isdigit(static_cast<unsigned char>(v.front()))) return std::nullopt;
    return v;
}

std::string ClientVersionGate::PublishedVersion(std::string_view json) {
    // {"version": "0.1.3", ...}: the key, its colon, its string (SroDeploy writes it so; nothing more is read).
    std::size_t at = json.find("\"version\"");
    if (at == std::string_view::npos) return {};
    at = json.find(':', at + 9);
    if (at == std::string_view::npos) return {};
    at = json.find('"', at);
    if (at == std::string_view::npos) return {};
    const std::size_t end = json.find('"', at + 1);
    if (end == std::string_view::npos || end - at - 1 > 32) return {};
    std::string v(json.substr(at + 1, end - at - 1));
    for (const char c : v) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.' && c != '-' && !std::isalpha(static_cast<unsigned char>(c))) return {};
    }
    return v;
}

std::string ClientVersionGate::Required() {
    std::lock_guard lock(mutex_);
    if (!patchDir_.empty() && std::chrono::steady_clock::now() - readAt_ >= kReadAgain) {
        readAt_ = std::chrono::steady_clock::now();
        std::ifstream in(std::filesystem::path(patchDir_) / "windows-x64" / "client.json", std::ios::binary);
        if (in) {
            std::stringstream text;
            text << in.rdbuf();
            const std::string v = PublishedVersion(text.str());
            if (!v.empty() && v != published_) {
                if (!published_.empty()) core::LogInfo("a new client is published: {} (the older ones are turned away)", v);
                published_ = v;
            }
        }
    }
    if (minimum_.empty()) return published_;
    if (published_.empty()) return minimum_;
    return Older(minimum_, published_) ? published_ : minimum_;
}

std::optional<std::string> ClientVersionGate::Refuse(std::string_view client) {
    const std::optional<std::string> version = ClientVersion(client);
    if (!version) return std::nullopt;
    const std::string required = Required();
    if (required.empty() || !Older(*version, required)) return std::nullopt;
    std::string words = std::format("Your client ({}) is out of date: version {} is out.", *version, required);
    words += downloadUrl_.empty() ? " Start the launcher to update, or download the new client from the website."
                                  : std::format(" Start the launcher to update, or download the new client from {}", downloadUrl_);
    return words;
}

} // namespace sr::server
