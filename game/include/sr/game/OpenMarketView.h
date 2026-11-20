#pragma once

// The client's view of the Open Market: the server's OpenMarketEvents applied (whether it is
// open, its fees and room, the player's listings, the last search's page), the lines the client writes for them (their
// 1.188 text keys), and the items the player has chosen to register but not sent yet (the register page's pending rows).

#include <cstdint>
#include <string>
#include <vector>

#include "sr/game/OpenMarket.h"
#include "sr/game/OpenMarketProtocol.h"

namespace sr::game {

class OpenMarketView {
public:
    // A line for the system messages: a text key, and the item it names (0: none; its name fills the %s).
    struct Line {
        std::string key;
        std::uint32_t item = 0;
    };
    // An item chosen for registration (the register page's pending row).
    struct Pending {
        std::uint8_t place = 0;
        std::uint32_t refId = 0;
        std::uint32_t count = 1;
        std::uint64_t price = 0;
    };

    // `time`: the HUD's clock (the listings' times are counted from it).
    std::vector<Line> Apply(const proto::OpenMarketEvent& event, double time);

    bool Open() const { return open_; }
    const proto::OpenMarketState& State() const { return state_; }
    const std::vector<proto::OpenMarketListing>& Listings() const { return listings_; }
    double ListingsAt() const { return listingsAt_; }
    const proto::OpenMarketResults& Results() const { return results_; }
    // The fees as the server told them.
    openmarket::Fees Fees() const;
    // The deposit the pending rows will cost.
    std::uint64_t PendingDeposit() const;
    // The sold listings' sums (the settle page): the prices, the sale fees, the deposits back, what the player gets.
    struct Sums {
        std::uint64_t prices = 0, saleFees = 0, deposits = 0, payout = 0;
    };
    Sums Sold() const;

    std::vector<Pending> pending;
    // Changes with every event (the windows redraw).
    std::uint64_t version = 0;
    // The market just opened (taken once: the window opens).
    bool TakeOpened() {
        const bool o = opened_;
        opened_ = false;
        return o;
    }

private:
    bool open_ = false, opened_ = false;
    proto::OpenMarketState state_;
    std::vector<proto::OpenMarketListing> listings_;
    double listingsAt_ = 0.0;
    proto::OpenMarketResults results_;
};

} // namespace sr::game
