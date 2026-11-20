#pragma once

// The exchange as the client sees it: the window's two sides as the server
// told them (ExchangeSide), the request waiting for the player's yes or no, and what the HUD says.
// The bag's changes when a deal is made are PlayerState's (ExchangeClosed's arrived, left and gold).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sr/game/ExchangeProtocol.h"
#include "sr/game/Items.h"

namespace sr::game {

class ExchangeView {
public:
    struct Side {
        proto::ExchangeStep step = proto::ExchangeStep::Open;
        std::uint64_t gold = 0;
        std::array<std::optional<ItemInstance>, proto::kExchangeSlots> items;
        std::array<int, proto::kExchangeSlots> places{}; // the own side's bag places (-1: none)
        Side() { places.fill(-1); }
    };
    // A line for the system messages; the HUD words it in the client's texts.
    struct Line {
        enum class Kind : std::uint8_t {
            Refused, // a request refused (code)
            Opened,  // "You started to exchange with [%s]."
            Ended,   // over without a deal (code; Ok: the player's own cancel)
            Done,    // "Exchange of the selected item(s) succeeded." and "Exchanging with [%s] is completed."
        };
        Kind kind = Kind::Refused;
        proto::ExchangeCode code = proto::ExchangeCode::Ok;
        std::string name;
    };

    std::vector<Line> Apply(const proto::ExchangeEvent& event);

    bool Open() const { return open_; }
    std::uint32_t Peer() const { return peer_; }
    const std::string& PeerName() const { return peerName_; }
    const Side& Mine() const { return mine_; }
    const Side& Theirs() const { return theirs_; }
    // A bag place the player offered (the inventory shows it held).
    bool Offered(int place) const;
    // A request waiting for the player's yes or no, once; and whether the one shown went away (the
    // asker gave up, its time ran out), once.
    std::optional<proto::ExchangeAsked> TakeRequest() { return std::exchange(request_, std::nullopt); }
    bool TakeRequestGone() { return std::exchange(requestGone_, false); }
    bool Asking() const { return asking_; }
    // Anything the window shows changed.
    std::uint64_t Revision() const { return revision_; }

private:
    void Reset();
    bool open_ = false;
    std::uint32_t peer_ = 0;
    std::string peerName_;
    Side mine_, theirs_;
    std::optional<proto::ExchangeAsked> request_;
    bool asking_ = false, requestGone_ = false;
    std::uint64_t revision_ = 1;
};

} // namespace sr::game
