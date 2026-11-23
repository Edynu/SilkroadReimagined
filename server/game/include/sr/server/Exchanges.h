#pragma once

// The exchange between two players on the game server: the original's rules:
//
// - A player asks another within 320 (the two centres); both alive and free of any other dealing.
//   The asked one has 30 s to answer.
// - Open, each side puts up to 12 items of its bag (not worn ones, not those whose CanTrade is 0)
//   while the other's bag has a free place for each, and gold (at most what it holds, at most 10^9).
// - Confirm fixes a side's offer; once both confirmed, each approves; when both approved the items and
//   the gold change hands at once, if each bag still has the room for what it receives.
// - A cancel, a death, a leave or a teleport ends it; nothing moves.
//
// While an exchange is open its two players' bags and gold are held (the host refuses their moves).
// Two players in an exchange always stand on the same game server: it needs them within 320 of each
// other, and nobody walks across a border between two servers; a teleport or a
// hand-off ends it.
//
// The game server is its host.

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "sr/game/ExchangeProtocol.h"
#include "sr/game/Inventory.h"

namespace sr::assets {
class ItemTable;
}

namespace sr::server {

class ExchangeHost {
public:
    virtual ~ExchangeHost() = default;
    // A player in this server's world as the exchange needs it (none: not a player here).
    struct Trader {
        std::uint64_t peer = 0;
        std::int64_t character = 0;
        std::string name;
        bool alive = true;
        bool free = true; // in no other dealing (a stall, the Open Market, ...)
        game::proto::WorldPosition position;
        game::Inventory* inventory = nullptr;
        bool visitor = false; // at a stall as its visitor (0x1833)
    };
    virtual std::optional<Trader> ExchangeTrader(std::uint32_t entity) = 0;
    virtual void ExchangeSend(std::uint64_t peer, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    virtual const assets::ItemTable* ExchangeItems() const = 0;
    // A pet's summoning item whose pet is out.
    virtual bool ExchangePetOut(std::uint32_t entity, std::uint8_t place) const {
        (void)entity;
        (void)place;
        return false;
    }
    // A deal made: both bags and both golds are as they are in memory now; `touched` the places of
    // each bag that changed. The host saves both sides in one transaction.
    virtual void ExchangeSave(std::int64_t a, const game::Inventory& bagA, const std::vector<std::uint8_t>& touchedA, std::int64_t b,
                              const game::Inventory& bagB, const std::vector<std::uint8_t>& touchedB) = 0;
};

class Exchanges {
public:
    using Clock = std::chrono::steady_clock;

    explicit Exchanges(ExchangeHost& host);
    ~Exchanges();

    // A request of the player `entity` (false: it does not decode).
    bool Handle(std::uint32_t entity, game::proto::MessageId id, std::span<const std::byte> payload, Clock::time_point now);
    // The requests' answers whose time ran out.
    void Tick(Clock::time_point now);
    // The player's exchange, asked or open, ends (a death, a leave, a teleport); both hear `code`.
    void End(std::uint32_t entity, game::proto::ExchangeCode code = game::proto::ExchangeCode::Cancelled);
    // In an open exchange: its bag and gold are held.
    bool Holding(std::uint32_t entity) const;
    // In one, asked or open.
    bool Busy(std::uint32_t entity) const;
    std::size_t Open() const;

private:
    struct Offer {
        std::uint8_t place = 0;   // in the owner's bag
        game::ItemInstance item;  // as it was put (a stack that changed since, by a pick-up, is not dealt)
    };
    struct Side {
        std::uint32_t entity = 0;
        std::array<std::optional<Offer>, game::proto::kExchangeSlots> offers;
        std::uint64_t gold = 0;
        game::proto::ExchangeStep step = game::proto::ExchangeStep::Open;
        int Count() const;
    };
    struct Session {
        Side sides[2]; // the asker, the asked
        bool open = false;
        Clock::time_point answerBy;
        Side* Of(std::uint32_t entity);
        Side& Other(const Side& side) { return &side == &sides[0] ? sides[1] : sides[0]; }
    };

    void Ask(std::uint32_t entity, std::uint32_t target, Clock::time_point now);
    void Reply(std::uint32_t entity, bool accept);
    void Put(std::uint32_t entity, std::uint8_t place);
    void Take(std::uint32_t entity, std::uint8_t slot);
    void Gold(std::uint32_t entity, std::uint64_t gold);
    void Confirm(std::uint32_t entity);
    void Approve(std::uint32_t entity);
    void Cancel(std::uint32_t entity);
    void Deal(const std::shared_ptr<Session>& s);
    // A step refused: its player hears why; a step the original's CTradeMgr cannot go on from
    // ends the exchange for both.
    void Refuse(std::uint32_t entity, game::proto::ExchangeCode code);
    // Ended: `entity` (0: none) hears `own`, the other side `code`.
    void Close(const std::shared_ptr<Session>& s, std::uint32_t entity, game::proto::ExchangeCode own, game::proto::ExchangeCode code);
    void SendSide(Session& s, Side& side);
    void Send(std::uint32_t entity, game::proto::MessageId id, const std::vector<std::byte>& payload);
    template <typename T>
    void Send(std::uint32_t entity, const T& message);

    ExchangeHost& host_;
    std::unordered_map<std::uint32_t, std::shared_ptr<Session>> byEntity_;
};

} // namespace sr::server
