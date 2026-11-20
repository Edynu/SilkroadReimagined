#pragma once

// The Open Market, the original's consignment market: its rules, shared by the server and the
// client. Listings belong to an account; their states, fees, times and limits as the original's game server has them.

#include <algorithm>
#include <array>
#include <cstdint>

namespace sr::game::openmarket {

// `_OpenMarket.Status`.
enum class Status : std::uint8_t { OnSale = 0, Sold = 1, Expired = 2, Gone = 0xFF };

// The price's brackets: below 1,000,000; to 10,000,000; to 100,000,000; above.
inline int BracketOf(std::uint64_t price) {
    return price < 1'000'000 ? 0 : price < 10'000'000 ? 1 : price < 100'000'000 ? 2 : 3;
}
// The fees' percents by bracket (server.cfg's SET_FEE_RATE "0,5,5,5" and SELL_FEE_RATE "0,10,10,10"; the shard's
// settings may change them).
struct Fees {
    std::array<int, 4> listing{0, 5, 5, 5};
    std::array<int, 4> sale{0, 10, 10, 10};
};
// The listing fee, kept as the row's Deposit (price / 100 x the bracket's percent): back when it sells.
inline std::uint64_t ListingFee(const Fees& fees, std::uint64_t price) { return price / 100 * std::uint64_t(std::max(0, fees.listing[BracketOf(price)])); }
// The sale fee, fixed when listed: none in bracket 0; else the ticket's percent when one runs (`ticket` >= 0),
// else the bracket's.
inline std::uint64_t SaleFee(const Fees& fees, std::uint64_t price, int ticket) {
    const int b = BracketOf(price);
    if (b == 0) return 0;
    return price / 100 * std::uint64_t(std::max(0, ticket >= 0 ? ticket : fees.sale[b]));
}
// What a sold listing gives back when collected: the price and the deposit less the sale fee.
inline std::uint64_t Payout(std::uint64_t price, std::uint64_t deposit, std::uint64_t saleFee) {
    return price + deposit > saleFee ? price + deposit - saleFee : 0;
}

inline constexpr std::uint64_t kPriceMin = 1, kPriceMax = 9'999'999'999; // (0x701C)
inline constexpr int kListings = 10;                                    // the room without a ticket
inline constexpr std::int64_t kListingSeconds = 259'200;                 // 3 days, plus the ticket's
inline constexpr std::int64_t kKeepSeconds = 2'419'200;                  // 28 days to collect or take back
inline constexpr std::uint8_t kFirstBagSlot = 13;                        // the equipment's places come first (0x700C)
inline constexpr int kPageRows = 10;                                     // the windows' rows (open_market_main.2dt)
inline constexpr int kSearchMost = 500;                                  // the search's TOP 500

// The Open Market ticket (the "Tiger Tablet of the Sales King", 3/3/13/19): a `_TimedJob` of category 11 for its Param1
// seconds. What it gives (three numbers, matched to its Param2..4 by their kinds, inferred): Param2 more listings,
// Param3 more seconds a listing,
// Param4 the sale fee's percent.
inline constexpr std::uint8_t kTicketCategory = 11;
struct Ticket {
    int room = 0;            // Param2 (30)
    std::int64_t seconds = 0; // Param3 (345,600: 4 days)
    int salePercent = -1;    // Param4 (1); -1: no ticket
};

// The original's codes (answered as the request's id | 0xB000; the client's lines).
namespace code {
inline constexpr std::uint16_t kOk = 1, kFailed = 2, kWrongItem = 3, kPage = 0x7007, kDeposit = 0x7008, kNotOpen = 0x7009,
                               kCannotSell = 0x700A, kBound = 0x700B, kEquipped = 0x700C, kGold = 0x700D, kBagFull = 0x700E,
                               kSlot = 0x700F, kNotTradeable = 0x7010, kRestricted = 0x7016, kRoom = 0x7017, kCount = 0x7018,
                               kExpired = 0x7019, kNotOnSale = 0x701A, kSearch = 0x701B, kPrice = 0x701C, kText = 0x701D,
                               kNotSold = 0x701E, kNotListed = 0x701F, kTakeBack = 0x7020, kShortText = 0x7021, kPet = 0x7022;
}

// The client's line for a code (empty: none).
inline const char* CodeKey(std::uint16_t c) {
    switch (c) {
    case code::kPage: return "UIIT_STT_OPEN_MARKET_ENTER_PAGE_ERROR";
    case code::kDeposit: return "UIIT_STT_OPEN_MARKET_ENTER_ERROR";
    case code::kCannotSell:
    case code::kBound: return "UIIT_STT_OPEN_MARKET_SELL_IMPOSSIBLE";
    case code::kGold: return "UIIT_MSG_GUILD_LACK_GOLD";
    case code::kBagFull: return "UIIT_MSG_STRGERR_INVENTORY_FULL";
    case code::kExpired:
    case code::kNotOnSale: return "UIIT_STT_OPEN_MARKET_BUYING_ERROR";
    case code::kSearch: return "UIIT_STT_OPEN_MARKET_FIND_ERROR";
    case code::kPrice: return "UIIT_STT_OPEN_MARKET_COST_OVER_ERROR";
    case code::kText: return "UIIT_STT_OPEN_MARKET_TEXT_ERROR";
    case code::kNotSold: return "UIIT_STT_OPEN_MARKET_RECEIPTED_ITEM_ERROR";
    case code::kNotListed:
    case code::kTakeBack: return "UIIT_STT_OPEN_MARKET_CANCEL_ITEM_ERROR";
    case code::kShortText: return "UIIT_STT_OPEN_MARKET_MIN_TEXT_ERROR";
    case code::kPet: return "UIIT_STT_OPEN_MARKET_ENTERD_PET_ERROR";
    default: return "";
    }
}

// The NPC (`Service = 0` in vSRO 1.188; ours puts it in service when the shard's open market is on).
inline constexpr const char* kNpcCodename = "NPC_OPEN_MARKET_JUEL";
inline constexpr std::uint32_t kNpcRefObjId = 36030;
// Where it stands (the database's Tab_RefNest rows 29839, 29844, 29846 of its tactics 1310; npcpos.txt has none): Jangan,
// Hotan, Constantinople; the heading word (65536 a turn, from +x toward +z).
struct NpcSpot {
    std::uint16_t region;
    float x, y, z;
    int heading;
};
inline constexpr NpcSpot kNpcSpots[] = {{25000, 1760.45f, 11.0f, 419.17f, 32221}, {23687, 1490.73f, 243.99f, 968.19f, -26215}, {26959, 17.18f, 80.44f, 258.15f, 16383}};

} // namespace sr::game::openmarket
