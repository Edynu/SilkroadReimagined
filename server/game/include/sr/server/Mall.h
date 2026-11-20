#pragma once

// The item mall and silk on the game server:
//
// - Silk is the account's (`sk_silk`, SilkStore.h): read when a player enters the world and told (SilkBalanceMessage),
//   kept here while it plays, told again after each purchase; a change made elsewhere (the website, a GM, a test
//   standing in for the site) comes on the account's bus channel (SilkChannel): the server that has the account's
//   player reads the row again and tells the balance and the line (SilkLine).
// - A purchase (MallBuy, MallBuyMany): the goods by their tab and slot (the package checked), the count by the
//   original's rule (game::MallPlacesNeeded), the bag's free places before anything (0x1807), the silk known here
//   (0x1880; the database decides); then one transaction: the silk taken (the row locked, never below 0), the stock,
//   the items into the bag places this server chose, the logs. The bag is held while it runs (no move, use or deal),
//   the items stand in the bag already and go if the transaction fails (ours: the original logs the player out).
//   Several goods are one transaction, all or nothing (ours; the original buys them one by one).
// - No player-state check (the original has none: dead, riding or in a war, a player may buy), but a
//   bag held by an exchange, a stall or alchemy refuses (0x1835, ours: they hold the bag against every change).
// - The history (SilkHistoryQuery): the account's rows of the original's `sk_silkbuylist`.
//
// The game server is its host.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sr/assets/Shop.h"
#include "sr/game/Items.h"
#include "sr/game/MallProtocol.h"
#include "sr/server/SilkStore.h"

namespace sr::assets {
class ItemTable;
}

namespace sr::server {

class Database;

class MallHost {
public:
    virtual ~MallHost() = default;
    // A player in this server's world as the mall needs it (none: not one here).
    struct Buyer {
        std::uint64_t peer = 0;
        std::int64_t character = 0;
        std::int64_t account = 0;
        std::string name;
    };
    virtual std::optional<Buyer> MallBuyer(std::uint32_t entity) = 0;
    // Whether something else holds the player's bag now (an exchange, a stall, alchemy).
    virtual bool MallBagHeld(std::uint32_t entity) const = 0;
    virtual const assets::ItemRow* MallItem(std::string_view codename) const = 0;
    // The bag's free places (none for the worn ones).
    virtual int MallFreePlaces(std::uint32_t entity) const = 0;
    // The items into the bag's first free places (each its own; an id of its own given): the places, empty when they
    // do not fit (then nothing was put).
    virtual std::vector<std::uint8_t> MallPut(std::uint32_t entity, std::vector<game::ItemInstance> items) = 0;
    // Those places emptied again (a purchase the database refused).
    virtual void MallTakeBack(std::uint32_t entity, const std::vector<std::uint8_t>& places) = 0;
    // The bag's record of a place (what the client is told).
    virtual std::optional<game::proto::ItemRecord> MallRecord(std::uint32_t entity, std::uint8_t place) const = 0;
    // One transaction after every write before it: the player's bag at `touched` (with its own saves still waiting)
    // and `work` inside it; `done(ok)` on the host's turn. A failed one leaves the saves to be made again.
    virtual void MallSave(std::uint32_t entity, const std::vector<std::uint8_t>& touched, std::function<bool(Database& tx, std::string& error)> work,
                          std::function<void(bool ok)> done) = 0;
    virtual void MallSend(std::uint64_t peer, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // `work` on the database (no order needed), then `done` on the host's turn.
    virtual void MallDatabase(std::function<void(Database&)> work, std::function<void()> done) = 0;
    // The bag changed (quests count their items).
    virtual void MallBagChanged(std::uint32_t entity) = 0;
    virtual int MallShardId() const = 0;
    virtual std::int64_t MallNow() const = 0;
};

class Mall {
public:
    Mall(MallHost& host, std::shared_ptr<const assets::MallTable> catalog);

    // A player in the world: its account's silk read and told.
    void Entered(std::uint32_t entity, std::int64_t account);
    void Left(std::uint32_t entity);
    // The account's silk changed elsewhere (the bus): its player here is told.
    void BusMessage(const std::string& payload);

    bool Handle(std::uint32_t entity, game::proto::MessageId id, std::span<const std::byte> payload);
    static bool IsRequest(game::proto::MessageId id);
    // A purchase under way holds the bag.
    bool Busy(std::uint32_t entity) const { return waiting_.contains(entity); }
    // The silk known for a player here (none: not one).
    std::optional<SilkBalance> Balance(std::uint32_t entity) const;
    const assets::MallTable* Catalog() const { return catalog_.get(); }
    // Tests: a purchase's database work fails after the items are written (the rollback's check).
    bool failNextPurchase = false;

private:
    void Open(std::uint32_t entity);
    void Buy(std::uint32_t entity, const std::vector<game::proto::MallBuyLine>& goods, std::uint32_t points);
    void History(std::uint32_t entity, const game::proto::SilkHistoryQuery& q);
    void Refuse(std::uint64_t peer, std::uint16_t code, const std::vector<game::proto::MallBuyLine>& goods);
    void TellBalance(std::uint32_t entity);

    MallHost& host_;
    std::shared_ptr<const assets::MallTable> catalog_;
    struct Account {
        std::int64_t account = 0;
        SilkBalance balance;
        bool known = false;
    };
    std::unordered_map<std::uint32_t, Account> accounts_; // by the player's entity
    std::unordered_set<std::uint32_t> waiting_;           // a purchase at the database
};

} // namespace sr::server
