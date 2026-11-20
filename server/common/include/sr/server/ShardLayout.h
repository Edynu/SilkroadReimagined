#pragma once

// Which game server of a shard runs which part of the world: read from the
// shard's settings file, the same file on every machine.
//
//   shard-name = Silkroad
//   shard-capacity = 3000
//   server.1.name = East
//   server.1.public-host = east.example.com
//   server.1.port = 15781
//   server.1.regions = CHINA, West_China, Oasis_Kingdom, Thief Village
//   server.2.name = West
//   server.2.regions = Eu, Am, GOD_*, ARENA_*
//
// The world is cut by the area names the game's own table gives every region (_RefRegion's
// ContinentName), so a dungeon, a fortress or an arena is one more name in a list. Names are separated
// by commas; a name may end in * (every area it starts); * alone takes whatever no other server
// names, and one server with * is the whole world in one process. Each area has exactly one owner.
//
// The players: `shard-capacity` is the shard's, all its game servers together (the login refuses one more; the server
// list and the website show it). The world is split by place, so the players gather where they like: each game server
// must carry the whole shard, and its `server.N.capacity` (the most it takes, for a machine that cannot) is the
// shard's unless written.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sr/core/Config.h"

namespace sr::server {

struct ShardServer {
    std::uint16_t id = 0;
    std::string name;
    std::string publicHost = "127.0.0.1";
    std::uint16_t port = 0; // 0: the program's default
    std::uint32_t capacity = 0; // the most players it takes (the shard's when the file does not say)
    std::vector<std::string> regions; // as written
};

class ShardLayout {
public:
    // False (with `error`) for a malformed file. A file naming no server gives an empty layout: a game
    // server alone, owning everything.
    static bool Read(const core::Config& config, ShardLayout& out, std::string& error);

    static constexpr std::uint32_t kDefaultShardCapacity = 3000; // shard-capacity unless written

    bool Empty() const { return servers_.empty(); }
    const std::string& Name() const { return name_; }
    // The shard's players, all its game servers together.
    std::uint32_t Capacity() const { return capacity_; }
    const std::vector<ShardServer>& Servers() const { return servers_; }
    const ShardServer* Server(std::uint16_t id) const;

    // The server that owns an area (0: none does).
    std::uint16_t OwnerOf(std::string_view area) const;
    // Every area of the game's data has exactly one owner, and every name written is one the data has
    // (a typo stops the server rather than leaving part of the world unserved). False with what is wrong.
    bool Check(const std::vector<std::string>& areas, std::string& error) const;

private:
    std::string name_;
    std::uint32_t capacity_ = kDefaultShardCapacity;
    std::vector<ShardServer> servers_;
};

} // namespace sr::server
