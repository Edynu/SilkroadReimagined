#include "sr/game/OpenMarketView.h"

#include <algorithm>

namespace sr::game {

std::vector<OpenMarketView::Line> OpenMarketView::Apply(const proto::OpenMarketEvent& event, double time) {
    std::vector<Line> lines;
    ++version;
    if (const auto* answer = std::get_if<proto::OpenMarketAnswer>(&event)) {
        if (answer->request == proto::OpenMarketClose::kId) {
            open_ = false;
            pending.clear();
        }
        if (answer->code != openmarket::code::kOk) {
            if (const char* key = openmarket::CodeKey(answer->code); *key != '\0') lines.push_back({key, 0});
        } else if (answer->request == proto::OpenMarketRegister::kId) {
            pending.clear(); // registered: the pending rows are listings now
        }
    } else if (const auto* state = std::get_if<proto::OpenMarketState>(&event)) {
        if (state->open != 0 && !open_) opened_ = true;
        open_ = state->open != 0;
        state_ = *state;
    } else if (const auto* listings = std::get_if<proto::OpenMarketListings>(&event)) {
        listings_ = listings->listings;
        listingsAt_ = time;
    } else if (const auto* results = std::get_if<proto::OpenMarketResults>(&event)) {
        results_ = *results;
    } else if (const auto* news = std::get_if<proto::OpenMarketNews>(&event)) {
        // 1.188's words: "(%s) was sold on consignment", "the consignment period for (%s) has expired", "... registered for
        // (sold through) consignment ... has now been deleted", "you bought [%s]".
        switch (news->kind) {
        case proto::OpenMarketNewsKind::Sold: lines.push_back({"UIIT_STT_OPEN_MARKET_SELL", news->item}); break;
        case proto::OpenMarketNewsKind::Expired: lines.push_back({"UIIT_STT_OPEN_MARKET_SELL_TIME_OVER", news->item}); break;
        case proto::OpenMarketNewsKind::Deleted: lines.push_back({"UIIT_STT_OPEN_MARKET_ITEM_DELETE", news->item}); break;
        case proto::OpenMarketNewsKind::Bought: lines.push_back({"UIIT_STT_OPEN_MARKET_SELL_COMMODITY", news->item}); break;
        }
    }
    return lines;
}

openmarket::Fees OpenMarketView::Fees() const {
    openmarket::Fees f;
    for (std::size_t k = 0; k < f.listing.size(); ++k) {
        if (k < state_.listingFees.size()) f.listing[k] = state_.listingFees[k];
        if (k < state_.saleFees.size()) f.sale[k] = state_.saleFees[k];
    }
    return f;
}

std::uint64_t OpenMarketView::PendingDeposit() const {
    const openmarket::Fees f = Fees();
    std::uint64_t sum = 0;
    for (const Pending& p : pending) sum += openmarket::ListingFee(f, p.price);
    return sum;
}

OpenMarketView::Sums OpenMarketView::Sold() const {
    Sums s;
    for (const proto::OpenMarketListing& l : listings_) {
        if (l.status != std::uint8_t(openmarket::Status::Sold)) continue;
        s.prices += l.price;
        s.saleFees += l.saleFee;
        s.deposits += l.deposit;
        s.payout += openmarket::Payout(l.price, l.deposit, l.saleFee);
    }
    return s;
}

} // namespace sr::game
