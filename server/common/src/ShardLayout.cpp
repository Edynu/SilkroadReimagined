#include "sr/server/ShardLayout.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <map>

namespace sr::server {

namespace {

std::string Lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

bool IsPrefix(std::string_view pattern) { return pattern.size() > 1 && pattern.back() == '*'; }
bool IsRest(std::string_view pattern) { return pattern == "*"; }

bool Matches(std::string_view pattern, std::string_view lowerArea) {
    const std::string p = Lower(pattern);
    if (IsPrefix(p)) return lowerArea.starts_with(std::string_view(p).substr(0, p.size() - 1));
    return p == lowerArea;
}

// Who an area belongs to by how specific the name is: an exact name, then a prefix, then *.
struct Claim {
    std::uint16_t server = 0;
    int rank = 0; // 3 exact, 2 prefix, 1 rest
    bool tied = false; // two servers claim it at the same rank
};

Claim ClaimOf(const std::vector<ShardServer>& servers, std::string_view area) {
    const std::string key = Lower(area);
    Claim best;
    for (const ShardServer& s : servers) {
        int rank = 0;
        for (const std::string& pattern : s.regions) {
            const int r = IsRest(pattern) ? 1 : !Matches(pattern, key) ? 0 : IsPrefix(pattern) ? 2 : 3;
            rank = std::max(rank, r);
        }
        if (rank == 0) continue;
        if (rank > best.rank) {
            best = {s.id, rank, false};
        } else if (rank == best.rank && s.id != best.server) {
            best.tied = true;
        }
    }
    return best;
}

} // namespace

bool ShardLayout::Read(const core::Config& config, ShardLayout& out, std::string& error) {
    out = {};
    out.name_ = config.GetString("shard-name", "Silkroad");
    if (config.Has("shard-capacity")) {
        const int capacity = config.GetInt("shard-capacity", 0);
        if (capacity <= 0) {
            error = "shard-capacity: not a number of players";
            return false;
        }
        out.capacity_ = std::uint32_t(capacity);
    }
    std::map<std::uint16_t, ShardServer> found;
    for (const auto& [key, value] : config.Values()) {
        std::string_view k = key;
        if (!k.starts_with("server.")) continue;
        k.remove_prefix(7);
        const std::size_t dot = k.find('.');
        if (dot == std::string_view::npos) continue; // server-name and the like are not ours
        unsigned id = 0;
        const auto number = std::from_chars(k.data(), k.data() + dot, id);
        if (number.ec != std::errc{} || number.ptr != k.data() + dot || id == 0 || id > 65535) {
            error = std::format("{}: the part after 'server.' must be a game server's number (1 and up)", key);
            return false;
        }
        ShardServer& s = found[std::uint16_t(id)];
        s.id = std::uint16_t(id);
        const std::string_view field = k.substr(dot + 1);
        if (field == "name") {
            s.name = value;
        } else if (field == "public-host") {
            s.publicHost = value;
        } else if (field == "port") {
            int port = 0;
            const auto p = std::from_chars(value.data(), value.data() + value.size(), port);
            if (p.ec != std::errc{} || p.ptr != value.data() + value.size() || port <= 0 || port > 65535) {
                error = std::format("{}: not a port number: '{}'", key, value);
                return false;
            }
            s.port = std::uint16_t(port);
        } else if (field == "capacity") {
            unsigned capacity = 0;
            const auto p = std::from_chars(value.data(), value.data() + value.size(), capacity);
            if (p.ec != std::errc{} || p.ptr != value.data() + value.size() || capacity == 0) {
                error = std::format("{}: not a number of players: '{}'", key, value);
                return false;
            }
            s.capacity = capacity;
        } else if (field == "regions") {
            for (std::string_view rest = value; !rest.empty();) {
                const std::size_t comma = rest.find(',');
                const std::string_view item = Trim(rest.substr(0, comma));
                if (!item.empty()) s.regions.emplace_back(item);
                rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
            }
        } else {
            error = std::format("{}: no such setting of a game server (name, public-host, port, capacity, regions)", key);
            return false;
        }
    }
    int everything = 0;
    for (auto& [id, s] : found) {
        if (s.regions.empty()) {
            error = std::format("server.{}.regions: which areas does this game server run? (* for all of them)", id);
            return false;
        }
        if (std::any_of(s.regions.begin(), s.regions.end(), [](const std::string& r) { return IsRest(r); })) ++everything;
        if (s.name.empty()) s.name = std::format("Server {}", id);
        if (s.capacity == 0) s.capacity = out.capacity_; // each carries the whole shard
        out.servers_.push_back(std::move(s));
    }
    if (everything > 1) {
        error = "more than one game server says regions = *: only one may take what the others leave";
        out.servers_.clear();
        return false;
    }
    return true;
}

const ShardServer* ShardLayout::Server(std::uint16_t id) const {
    const auto it = std::find_if(servers_.begin(), servers_.end(), [&](const ShardServer& s) { return s.id == id; });
    return it != servers_.end() ? &*it : nullptr;
}

std::uint16_t ShardLayout::OwnerOf(std::string_view area) const {
    const Claim claim = ClaimOf(servers_, area);
    return claim.tied ? 0 : claim.server;
}

bool ShardLayout::Check(const std::vector<std::string>& areas, std::string& error) const {
    for (const std::string& area : areas) {
        const Claim claim = ClaimOf(servers_, area);
        if (claim.server == 0) {
            error = std::format("no game server runs the area '{}': name it in a server's regions", area);
            return false;
        }
        if (claim.tied) {
            error = std::format("more than one game server claims the area '{}'", area);
            return false;
        }
    }
    for (const ShardServer& s : servers_) {
        for (const std::string& pattern : s.regions) {
            if (IsRest(pattern)) continue;
            const bool known = std::any_of(areas.begin(), areas.end(), [&](const std::string& area) { return Matches(pattern, Lower(area)); });
            if (!known) {
                error = std::format("server.{}.regions: the game has no area '{}'", s.id, pattern);
                return false;
            }
        }
    }
    return true;
}

} // namespace sr::server
