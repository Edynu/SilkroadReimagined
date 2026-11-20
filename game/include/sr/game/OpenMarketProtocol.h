#pragma once

// The Open Market's messages: ours, 0x1460..0x147F (the original's 0x7506..0x750E and their
// 0xB5xx answers, 0x350D / 0x3510 the sold notice). Requests 0x1460..0x146F, answers 0x1470..0x147F.

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "sr/game/OpenMarket.h"
#include "sr/game/Protocol.h"
#include "sr/game/WorldProtocol.h"

namespace sr::game::proto {

// --- Requests ------------------------------------------------------------------------------------------------------

// The market opened at its NPC (the original's NPC function 23): the player's state while it is open.
struct OpenMarketOpen {
    static constexpr MessageId kId = 0x1460;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};
struct OpenMarketClose { // 0x7507
    static constexpr MessageId kId = 0x1461;
    template <typename V>
    void Visit(V&) {}
};
// Items put up (0x7508): each a bag place, its count (equipment 1, a stack whole) and its price.
struct OpenMarketEntry {
    std::uint8_t place = 0;
    std::uint32_t count = 1;
    std::uint64_t price = 0;
    template <typename V>
    void Visit(V& v) {
        v("place", place);
        v("count", count);
        v("price", price);
    }
};
struct OpenMarketRegister {
    static constexpr MessageId kId = 0x1462;
    std::vector<OpenMarketEntry> entries;
    template <typename V>
    void Visit(V& v) {
        v("entries", entries, openmarket::kPageRows);
    }
};
// The player's listings taken back (0x7509; on sale or expired; the deposit not returned).
struct OpenMarketTakeBack {
    static constexpr MessageId kId = 0x1463;
    std::vector<std::uint32_t> listings;
    template <typename V>
    void Visit(V& v) {
        v("listings", listings, openmarket::kPageRows);
    }
};
// A row of the last search bought (0x750A): its account and number.
struct OpenMarketBuy {
    static constexpr MessageId kId = 0x1464;
    std::uint32_t account = 0, listing = 0;
    template <typename V>
    void Visit(V& v) {
        v("account", account);
        v("listing", listing);
    }
};
// The sold listings' gold collected (0x750B).
struct OpenMarketSettle {
    static constexpr MessageId kId = 0x1465;
    std::vector<std::uint32_t> listings;
    template <typename V>
    void Visit(V& v) {
        v("listings", listings, openmarket::kPageRows);
    }
};
// A search (0x750C): the type's group (the stall network's categories), its degree (0: any), a seller (empty: any), the
// page (from 0).
struct OpenMarketSearch {
    static constexpr MessageId kId = 0x1466;
    std::uint32_t group = 0;
    std::uint8_t degree = 0;
    std::string seller;
    std::uint16_t page = 0;
    template <typename V>
    void Visit(V& v) {
        v("group", group);
        v("degree", degree);
        v("seller", seller, 32);
        v("page", page);
    }
};
// The player's listings (0x750E).
struct OpenMarketMine {
    static constexpr MessageId kId = 0x1467;
    template <typename V>
    void Visit(V&) {}
};

// --- Answers -------------------------------------------------------------------------------------------------------

// A request's outcome: `code` openmarket::code::kOk or the original's.
struct OpenMarketAnswer {
    static constexpr MessageId kId = 0x1470;
    MessageId request = 0;
    std::uint16_t code = openmarket::code::kOk;
    template <typename V>
    void Visit(V& v) {
        v("request", request);
        v("code", code);
    }
};
// The market as it is for the player (at the opening and after a change): the fees, the room, a ticket's.
struct OpenMarketState {
    static constexpr MessageId kId = 0x1471;
    std::uint8_t open = 0;
    std::vector<std::uint8_t> listingFees, saleFees; // the brackets' percents (4)
    std::uint16_t room = openmarket::kListings;
    std::int8_t ticketSale = -1; // the ticket's sale percent (-1: none)
    template <typename V>
    void Visit(V& v) {
        v("open", open);
        v("listingFees", listingFees, 4);
        v("saleFees", saleFees, 4);
        v("room", room);
        v("ticketSale", ticketSale);
    }
};
// A listing as its owner sees it (the register and settle pages, the alarm window).
struct OpenMarketListing {
    std::uint32_t listing = 0;
    std::uint8_t status = 0;  // openmarket::Status
    ItemRecord item;          // its slot 0 (gone once sold: its RefObjID and count kept)
    std::uint64_t price = 0, deposit = 0, saleFee = 0;
    std::uint32_t secondsLeft = 0; // to its end (on sale: the sale's; sold or expired: the 28 days')
    template <typename V>
    void Visit(V& v) {
        v("listing", listing);
        v("status", status);
        v("item", item);
        v("price", price);
        v("deposit", deposit);
        v("saleFee", saleFee);
        v("secondsLeft", secondsLeft);
    }
};
struct OpenMarketListings {
    static constexpr MessageId kId = 0x1472;
    std::vector<OpenMarketListing> listings;
    template <typename V>
    void Visit(V& v) {
        v("listings", listings, 128);
    }
};
// A row of a search.
struct OpenMarketRow {
    std::uint32_t account = 0, listing = 0;
    std::string seller;
    ItemRecord item;
    std::uint64_t price = 0;
    std::uint32_t secondsLeft = 0;
    template <typename V>
    void Visit(V& v) {
        v("account", account);
        v("listing", listing);
        v("seller", seller, 32);
        v("item", item);
        v("price", price);
        v("secondsLeft", secondsLeft);
    }
};
struct OpenMarketResults {
    static constexpr MessageId kId = 0x1473;
    std::uint16_t page = 0, pages = 0;
    std::uint32_t total = 0;
    std::vector<OpenMarketRow> rows;
    template <typename V>
    void Visit(V& v) {
        v("page", page);
        v("pages", pages);
        v("total", total);
        v("rows", rows, openmarket::kPageRows);
    }
};
// A line for the owner (wherever it plays): one of its listings sold (UIIT_STT_OPEN_MARKET_SELL), expired
// (_SELL_TIME_OVER), deleted at its 28 days (_ITEM_DELETE); the buyer's (_SELL_COMMODITY).
enum class OpenMarketNewsKind : std::uint8_t { Sold = 1, Expired = 2, Deleted = 3, Bought = 4 };
struct OpenMarketNews {
    static constexpr MessageId kId = 0x1474;
    OpenMarketNewsKind kind = OpenMarketNewsKind::Sold;
    std::uint32_t item = 0; // its RefObjID (the line names it)
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("item", item);
    }
};

using OpenMarketEvent = std::variant<OpenMarketAnswer, OpenMarketState, OpenMarketListings, OpenMarketResults, OpenMarketNews>;

inline bool IsOpenMarketMessage(MessageId id) { return id >= 0x1460 && id < 0x1480; }
inline bool IsOpenMarketRequest(MessageId id) { return id >= 0x1460 && id < 0x1470; }

inline bool DecodeOpenMarketEvent(MessageId id, std::span<const std::byte> payload, OpenMarketEvent& out) {
    const auto as = [&]<typename T>(T message) {
        if (!Decode(payload, message)) return false;
        out = std::move(message);
        return true;
    };
    switch (id) {
    case OpenMarketAnswer::kId: return as(OpenMarketAnswer{});
    case OpenMarketState::kId: return as(OpenMarketState{});
    case OpenMarketListings::kId: return as(OpenMarketListings{});
    case OpenMarketResults::kId: return as(OpenMarketResults{});
    case OpenMarketNews::kId: return as(OpenMarketNews{});
    default: return false;
    }
}

inline const char* OpenMarketMessageName(MessageId id) {
    switch (id) {
    case OpenMarketOpen::kId: return "OpenMarketOpen";
    case OpenMarketClose::kId: return "OpenMarketClose";
    case OpenMarketRegister::kId: return "OpenMarketRegister";
    case OpenMarketTakeBack::kId: return "OpenMarketTakeBack";
    case OpenMarketBuy::kId: return "OpenMarketBuy";
    case OpenMarketSettle::kId: return "OpenMarketSettle";
    case OpenMarketSearch::kId: return "OpenMarketSearch";
    case OpenMarketMine::kId: return "OpenMarketMine";
    case OpenMarketAnswer::kId: return "OpenMarketAnswer";
    case OpenMarketState::kId: return "OpenMarketState";
    case OpenMarketListings::kId: return "OpenMarketListings";
    case OpenMarketResults::kId: return "OpenMarketResults";
    case OpenMarketNews::kId: return "OpenMarketNews";
    default: return nullptr;
    }
}

} // namespace sr::game::proto
