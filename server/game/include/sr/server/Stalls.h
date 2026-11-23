#pragma once

// Stalls and the stall network on the game server.
//
// The stall is the original's (its requests 0x70B1-0x70BA):
// - A player opens one where it stands (not dead, riding, a murderer, in a job's suit or in another
//   dealing), in its modifying state, with a name; its owner then puts up to 10 items of its bag at a
//   price each (1..10^9), takes them back, renames it, sets the greeting, and opens it for business (with
//   at least one item) or back to modifying. Items that cannot be traded (CanTrade 0, a running rental,
//   trade goods, as the exchange) are refused.
// - Others in sight walk in from within 100 (8 people at most, its owner among them), see its items,
//   chat with its people, buy a slot's whole stack while it is open for business (gold and a free bag place
//   needed), and leave (a walk leaves too). The item and the gold change hands in one database transaction.
// - Opening for business, its owner may register its items in the network (ours: in a town only); a sale
//   through the network costs the seller 1% (the client's UIIT_MSG_WARENETWORK_REGIST_03, _CHARGE).
// - Its owner closes it; a death, a leave or a teleport closes it too (its visitors hear 0x3C15). While it
//   stands its owner stays put and its bag and gold are held (the host refuses what would change them).
//
// The network is ours: every open stall in a town lists its items in a shard-wide index that
// every game server keeps (the shard's bus, channel sr_stalls_<shard>; the parties' heartbeat drops a quiet
// server's stalls). A search (a type's group and a degree) is answered from memory at most once every 3 s
// per player; its matches stay for that player and are paged for free, 15 rows a page, cheapest first. A row
// is bought where its stall stands: here directly, else over the bus (the buyer's gold set aside and saved
// first, its bag held; the stall's server makes the sale in one transaction before a deadline on the
// database's clock; an answer that never comes is settled by reading where the item is).
//
// The game server is its host.

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "sr/game/Inventory.h"
#include "sr/game/StallProtocol.h"

namespace sr::assets {
class ItemTable;
class StallNetworkData;
} // namespace sr::assets

namespace sr::server {

class StallHost {
public:
    // A pet's summoning item whose pet is out; a place of the bag to be saved as it is now; the session to end (a far
    // purchase that found no room: the next entry reads the database).
    virtual bool StallPetOut(std::uint32_t entity, std::uint8_t place) const {
        (void)entity;
        (void)place;
        return false;
    }
    virtual void StallPlaceMoved(std::uint32_t entity, std::uint8_t place) {
        (void)entity;
        (void)place;
    }
    virtual void StallBagLost(std::uint32_t entity) { (void)entity; }
    virtual ~StallHost() = default;
    // A player in this server's world as the stall needs it (none: not a player here).
    struct Player {
        std::uint64_t peer = 0;
        std::int64_t character = 0;
        std::string name;
        bool alive = true;
        bool free = true;      // in no other dealing (an exchange, a cast, a teleport on its way)
        bool riding = false;
        bool murderer = false;
        bool jobSuit = false;
        int town = -1;         // the town it stands in (regioninfo.txt's area index; -1: none)
        std::uint32_t skin = 0; // its stall's decoration (an item mall booth's RefObjID; 0: the plain booth)
        game::proto::WorldPosition position;
        game::Inventory* inventory = nullptr;
    };
    virtual std::optional<Player> StallPlayer(std::uint32_t entity) = 0;
    virtual void StallSend(std::uint64_t peer, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // A player's stall as those around see it (also kept for new viewers).
    virtual void StallShow(const game::proto::EntityStall& stall) = 0;
    virtual const assets::ItemTable* StallItems() const = 0;
    virtual const assets::StallNetworkData* StallNetworkTables() const = 0;
    // An item's name as players read it, in lower case (the detailed search's name; empty: not known).
    virtual std::string StallItemName(std::uint32_t refId) const = 0;
    // A sale on this server: both bags and golds as they are in memory now, saved in one transaction.
    virtual void StallSaveSale(std::int64_t owner, const game::Inventory& ownerBag, std::uint8_t ownerPlace, std::int64_t buyer,
                               const game::Inventory& buyerBag, std::uint8_t buyerPlace) = 0;
    // The buyer's gold set aside for a purchase from afar: saved, then `saved` runs on the loop.
    virtual void StallSaveGold(std::int64_t character, std::uint64_t gold, std::function<void()> saved) = 0;
    // A sale from afar on the stall's server, in one transaction: the item `serial` leaves the owner's
    // `ownerPlace` for the buyer's `buyerPlace` (which must be empty), the owner's gold becomes `ownerGold`;
    // only before `deadlineMs` (Unix ms) on the database's clock. `done(true)` when it was made.
    struct FarSale {
        std::int64_t owner = 0, buyer = 0, serial = 0;
        std::uint8_t ownerPlace = 0, buyerPlace = 0;
        std::uint64_t ownerGold = 0;
        std::int64_t deadlineMs = 0;
    };
    virtual void StallSaveFarSale(const FarSale& sale, std::function<void(bool)> done) = 0;
    // The buyer's side of a sale whose answer never came: whether the item `serial` is in the buyer's place
    // (read once the database's clock is past the deadline; `landed` runs on the loop).
    virtual void StallSettle(std::int64_t buyer, std::uint8_t place, std::int64_t serial, std::int64_t deadlineMs, std::function<void(bool landed)> landed) = 0;
    // Gold given back to a character that left the world meanwhile (added to what the database holds).
    virtual void StallRefundOffline(std::int64_t character, std::uint64_t gold) = 0;
    // The shard's bus.
    virtual std::uint16_t StallServer() const = 0;
    virtual bool StallOnBus() const = 0;
    virtual void StallPublish(std::string text) = 0;
};

class Stalls {
public:
    using Clock = std::chrono::steady_clock;

    explicit Stalls(StallHost& host);
    ~Stalls();

    // A request of the player `entity` (false: it does not decode).
    bool Handle(std::uint32_t entity, game::proto::MessageId id, std::span<const std::byte> payload, Clock::time_point now);
    // Purchases from afar whose answer is late, servers gone quiet.
    void Tick(Clock::time_point now);
    // The player leaves its stall, as owner (the stall closes, its people hear `code`) or visitor: a death, a
    // leave, a teleport. Call while the host still has it.
    void Leave(std::uint32_t entity, game::proto::StallCode code = game::proto::StallCode::HostLeft);
    // A visitor walks away (its walk leaves the stall).
    void Walked(std::uint32_t entity);
    // The player has a stall up (it stays put; its bag and gold are held).
    bool Owner(std::uint32_t entity) const;
    // The player visits one.
    bool Visiting(std::uint32_t entity) const;
    // Its bag and gold are held: an owner, or a buyer waiting for a purchase from afar.
    bool Holding(std::uint32_t entity) const;
    std::size_t Open() const { return stalls_.size(); }
    // The player's decoration changed (a booth item used): its stall, if one stands, shows it at once.
    void SkinChanged(std::uint32_t entity, std::uint32_t skin);

    // --- The shard's other game servers -------------------------------------------------------------
    void Hello();
    void BusMessage(const std::string& text, Clock::time_point now);
    std::string GoodbyeText() const;
    // The network's rows known (every server's).
    std::size_t Listed() const { return index_.size(); }

private:
    struct Slot {
        std::uint8_t place = 0; // in the owner's bag
        game::ItemInstance item; // as it was put up (sold only while the bag still holds it so)
        std::uint64_t price = 0;
        std::uint32_t revision = 0;
    };
    struct Stall {
        std::uint32_t owner = 0;
        std::int64_t character = 0;
        std::string ownerName, name, greeting;
        bool open = false;
        bool network = false; // its owner registered it in the network (opening for business)
        int town = -1;
        std::uint16_t region = 0;
        std::uint32_t skin = 0;
        std::array<std::optional<Slot>, game::proto::kStallSlots> slots;
        std::vector<std::uint32_t> visitors;
        bool selling = false; // a sale from afar is being made (others wait: the original's busy state)
        std::uint32_t revisions = 0;
    };
    // A row of the network, wherever its stall stands.
    struct Row {
        std::uint16_t server = 0;
        std::uint32_t revision = 0;
        std::int64_t character = 0;
        std::string owner;
        std::uint8_t town = 0;
        std::uint16_t region = 0;
        game::proto::ItemRecord item;
        std::int64_t serial = 0;
        std::uint64_t price = 0;
        std::uint32_t group = 0;
        std::uint8_t degree = 0;
        std::uint8_t grade = 0; // game::proto::NetworkGrade: 1 general, 2 magic, 3 rare
        std::uint8_t sex = 2;   // who may wear it (itemdata's: 0 female, 1 male, 2 anyone)
        std::string name;       // lower case
    };
    struct Search {
        std::vector<std::uint64_t> matches; // listings, cheapest first
        Clock::time_point at;
    };
    // A purchase from afar this server's player waits for.
    struct Pending {
        std::uint32_t buyer = 0;
        std::int64_t character = 0;
        std::uint64_t listing = 0, price = 0;
        std::int64_t serial = 0;
        game::proto::ItemRecord item; // the row as it was bought
        std::uint8_t place = 0;
        std::uint16_t server = 0;
        std::int64_t deadlineMs = 0;
        Clock::time_point settleAt;
        bool asked = false, settling = false;
    };

    void OpenStall(std::uint32_t entity, std::string name);
    void CloseStall(std::uint32_t owner, game::proto::StallCode code);
    void VisitStall(std::uint32_t entity, std::uint32_t owner);
    void LeaveStall(std::uint32_t entity, game::proto::StallCode code);
    void Buy(std::uint32_t entity, std::uint8_t slot);
    void Put(std::uint32_t entity, const game::proto::StallPut& put);
    void Take(std::uint32_t entity, std::uint8_t slot);
    void SetOpen(std::uint32_t entity, bool open, bool network);
    void SetText(std::uint32_t entity, bool greeting, std::string text);
    void Chat(std::uint32_t entity, std::string text);
    void NetworkSearch(std::uint32_t entity, const game::proto::NetworkSearch& search, Clock::time_point now);
    void NetworkPage(std::uint32_t entity, std::uint16_t page);
    void NetworkBuy(std::uint32_t entity, const game::proto::NetworkBuy& buy);

    // A sale on this server (a visitor's buy, or a network row whose stall is here); Ok when made.
    game::proto::StallCode SellHere(Stall& stall, std::uint8_t slot, std::uint32_t buyer, bool network);
    // The stall's server: a sale from afar asked over the bus.
    void SellFar(std::uint16_t from, std::uint32_t request, std::int64_t buyer, const std::string& buyerName, std::uint64_t listing,
                 std::uint32_t revision, std::uint64_t price, std::uint8_t place, std::int64_t deadlineMs);
    void FarAnswered(std::uint32_t request, game::proto::StallCode code, const game::proto::ItemRecord& item, std::int64_t serial);
    void Settle(std::uint32_t request, bool landed, const game::proto::ItemRecord& item, std::int64_t serial);

    // The network: a stall's rows told to every server (open for business in a town), or taken back.
    void List(Stall& stall);
    void Unlist(Stall& stall, int slot = -1);
    void PublishRow(std::uint64_t listing, const Row& row, std::uint16_t to = 0);
    void PublishGone(std::uint64_t listing);
    void ServerGone(std::uint16_t server);

    Stall* StallOf(std::uint32_t owner);
    Stall* VisitedBy(std::uint32_t entity);
    std::vector<std::uint32_t> People(const Stall& stall) const; // the owner and its visitors
    game::proto::StallView ViewOf(const Stall& stall, bool owner);
    game::proto::StallItem ItemOf(const Stall& stall, std::uint8_t slot, bool owner);
    // Why an item of the bag cannot be put up (Ok: it can).
    game::proto::StallCode Sellable(const game::ItemInstance& item) const;
    void Answer(std::uint32_t entity, game::proto::MessageId request, game::proto::StallCode code);
    void Send(std::uint32_t entity, game::proto::MessageId id, const std::vector<std::byte>& payload);
    template <typename T>
    void Send(std::uint32_t entity, const T& message);
    template <typename T>
    void SendAll(const Stall& stall, const T& message, std::uint32_t except = 0);
    void Publish(std::uint8_t kind, std::uint16_t to, std::vector<std::byte> payload);
    std::uint16_t Self() const;

    StallHost& host_;
    std::unordered_map<std::uint32_t, Stall> stalls_;           // by owner entity
    std::unordered_map<std::uint32_t, std::uint32_t> visiting_; // visitor -> owner
    std::map<std::uint64_t, Row> index_;                        // the shard's network, by listing
    std::unordered_map<std::uint32_t, Search> searches_;         // by player entity
    std::unordered_map<std::uint32_t, Pending> pending_;         // purchases from afar, by request
    std::unordered_map<std::uint32_t, std::uint32_t> waiting_;   // buyer entity -> request
    std::unordered_map<std::uint16_t, Clock::time_point> peers_; // the other servers, last heard
    Clock::time_point nextAlive_{};
    std::uint32_t requests_ = 0;
};

// A network listing: the owner's character and the slot.
inline std::uint64_t StallListing(std::int64_t character, std::uint8_t slot) { return std::uint64_t(character) << 8 | slot; }

} // namespace sr::server
