#include "sr/game/Exchange.h"

#include <utility>

#include "sr/game/Inventory.h"

namespace sr::game {

void ExchangeView::Reset() {
    open_ = false;
    peer_ = 0;
    peerName_.clear();
    mine_ = {};
    theirs_ = {};
    ++revision_;
}

bool ExchangeView::Offered(int place) const {
    if (!open_) return false;
    for (int p : mine_.places) {
        if (p == place) return true;
    }
    return false;
}

std::vector<ExchangeView::Line> ExchangeView::Apply(const proto::ExchangeEvent& event) {
    using Kind = Line::Kind;
    std::vector<Line> lines;
    if (const auto* asked = std::get_if<proto::ExchangeAsked>(&event)) {
        request_ = *asked;
        asking_ = true;
    } else if (const auto* answer = std::get_if<proto::ExchangeAnswer>(&event)) {
        lines.push_back({Kind::Refused, answer->code, {}});
    } else if (const auto* opened = std::get_if<proto::ExchangeOpened>(&event)) {
        Reset();
        open_ = true;
        peer_ = opened->peer;
        peerName_ = opened->name;
        asking_ = false;
        request_.reset();
        lines.push_back({Kind::Opened, proto::ExchangeCode::Ok, opened->name});
    } else if (const auto* side = std::get_if<proto::ExchangeSide>(&event)) {
        Side& s = side->mine != 0 ? mine_ : theirs_;
        s = {};
        s.step = side->step;
        s.gold = side->gold;
        for (std::size_t k = 0; k < side->items.size(); ++k) {
            const proto::ItemRecord& r = side->items[k];
            if (r.slot >= proto::kExchangeSlots) continue;
            s.items[r.slot] = Inventory::FromRecord(r);
            if (k < side->places.size()) s.places[r.slot] = side->places[k];
        }
        ++revision_;
    } else if (const auto* closed = std::get_if<proto::ExchangeClosed>(&event)) {
        const std::string name = peerName_;
        // A request shown and not answered goes with it.
        if (asking_) {
            asking_ = false;
            request_.reset();
            requestGone_ = true;
        }
        Reset();
        if (closed->done != 0) {
            lines.push_back({Kind::Done, proto::ExchangeCode::Ok, name});
        } else {
            lines.push_back({Kind::Ended, closed->code, name});
        }
    }
    return lines;
}

} // namespace sr::game
