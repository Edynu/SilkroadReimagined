#pragma once

// The Open Market on a game server: the consignment market at its NPC. Its listings live in the
// shard's database (the original's `_OpenMarket`, a listing per account), so a listing made on one game server is found,
// bought and collected from any; a sale's line goes to its seller's server over the bus.
//
// An item moves between the bag and a listing in one database transaction first; the bag in memory follows when it is
// done (no item save is queued for it: the database holds it already), and the bag is held meanwhile.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "sr/game/Items.h"
#include "sr/game/OpenMarket.h"
#include "sr/game/OpenMarketProtocol.h"

namespace sr::assets {
struct ItemRow;
}

namespace sr::server {

class Database;
struct StoredItem;

struct OpenMarketActor {
    std::uint64_t peer = 0;
    std::uint32_t entity = 0;
    std::int64_t account = 0, character = 0;
    std::string name;
    bool dead = false;
};

class OpenMarketHost {
public:
    virtual ~OpenMarketHost() = default;
    virtual void OpenMarketSend(std::uint64_t peer, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // `task` on a database thread, then `done` on the server's (a read, or the account's listings aged).
    virtual void OpenMarketSubmit(std::function<void(Database&)> task, std::function<void()> done) = 0;
    // The same in `character`'s order of saves (its bag and gold written by the task: after its saves asked before it).
    virtual void OpenMarketWrite(std::int64_t character, std::function<void(Database&)> task, std::function<void()> done) = 0;
    // A player on this server by its connection, and by its account (0: none).
    virtual std::optional<OpenMarketActor> OpenMarketActorOf(std::uint64_t peer) const = 0;
    virtual std::uint64_t OpenMarketPeerOfAccount(std::int64_t account) const = 0;
    // The market's NPC within reach of the player's entity.
    virtual bool OpenMarketNpcNear(std::uint32_t entity, std::uint32_t npc) const = 0;
    // The bag held by something else (an exchange, a stall, alchemy) or a save of it still on its way.
    virtual bool OpenMarketBagBusy(std::uint32_t entity) const = 0;
    virtual std::uint64_t OpenMarketGold(std::uint64_t peer) const = 0;
    // The gold in memory, and told; `save`: written too (else the market's own transaction writes it).
    virtual void OpenMarketSetGold(std::uint64_t peer, std::uint64_t gold, bool save = true) = 0;
    virtual const game::ItemInstance* OpenMarketBagItem(std::uint64_t peer, std::uint8_t place) const = 0;
    virtual std::uint8_t OpenMarketBagSize(std::uint64_t peer) const = 0;
    // An item out of the bag in memory before its listing is written (its rows are the market's transaction's), and told.
    virtual std::optional<game::ItemInstance> OpenMarketTakeAside(std::uint64_t peer, std::uint8_t place) = 0;
    // An item set aside back into its place (its rows never moved), or the first free place when that was filled
    // meanwhile (then saved there), and told.
    virtual void OpenMarketPutBack(std::uint64_t peer, std::uint8_t place, game::ItemInstance item) = 0;
    // An item into the bag in memory at a place (its rows moved already), or the first free place when that was filled
    // meanwhile (then saved there), and told.
    virtual void OpenMarketPutInBag(std::uint64_t peer, std::uint8_t place, const StoredItem& item) = 0;
    // A free bag place (none: the bag is full).
    virtual std::optional<std::uint8_t> OpenMarketFreePlace(std::uint64_t peer, const std::set<std::uint8_t>& taken) const = 0;
    // A summoned pet's item (0x7022).
    virtual bool OpenMarketPetOut(std::uint64_t peer, std::uint8_t place) const = 0;
    // The player's Open Market ticket (a category 11 timed service), and its item (0: none).
    virtual game::openmarket::Ticket OpenMarketTicket(std::uint64_t peer, std::uint32_t& item) const = 0;
    virtual const assets::ItemRow* OpenMarketItem(std::uint32_t refId) const = 0;
    // The stall network's category group of an item's type (0: in none).
    virtual std::uint32_t OpenMarketGroupOf(const assets::ItemRow& row) const = 0;
    virtual std::uint16_t OpenMarketServer() const { return 0; }
    virtual bool OpenMarketOnBus() const { return false; }
    virtual void OpenMarketPublish(std::string text) { (void)text; }
};

struct OpenMarketConfig {
    game::openmarket::Fees fees;
};

class OpenMarket {
public:
    using Clock = std::chrono::steady_clock;

    OpenMarket(OpenMarketHost& host, OpenMarketConfig config);
    ~OpenMarket();
    OpenMarket(const OpenMarket&) = delete;
    OpenMarket& operator=(const OpenMarket&) = delete;

    // A request (OpenMarketProtocol.h); false when it does not decode.
    bool Handle(std::uint64_t peer, game::proto::MessageId id, std::span<const std::byte> payload);
    // A message from another game server.
    void BusMessage(const std::string& text);
    // Each second: the listings' times of the players on this server (each minute each).
    void Tick(Clock::time_point now);
    // A player entered the world (its listings aged and its news told), or left it.
    void Entered(std::uint64_t peer);
    void Left(std::uint64_t peer);
    // A request under way for this player (its bag held).
    bool Busy(std::uint32_t entity) const;
    // The player's market window open (its session).
    bool InSession(std::uint64_t peer) const;
    // Writes under way (tests wait for them).
    bool Writing() const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace sr::server
