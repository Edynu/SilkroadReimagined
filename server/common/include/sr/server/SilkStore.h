#pragma once

// Silk: the account's, in the original's table
// `sro_vt_account.sk_silk {jid, silk_own, silk_gift, silk_point}` (one row per account; none counts as 0 / 0 / 0),
// shared by every shard's game servers and the website. A credit from outside the player's game server (a grant,
// the starting silk, the site) writes the row and its log in one transaction and says so on the account's bus
// channel (`SilkChannel()`), so the game server that holds the account's session tells the player.
//
// The log is the original's: `sk_silkbuylist` (the client's "Buying list" reads type 0 / reason 0 rows, the original's
// `_GetSilkHistory`) and `sk_silkchange_by_web` for a credit from outside, `sk_packageitemsalelog` for a purchase
// (MallStore.h).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sr::server {

class Database;

struct SilkBalance {
    std::uint32_t own = 0;   // bought silk (silk_own)
    std::uint32_t gift = 0;  // gift silk (silk_gift)
    std::uint32_t point = 0; // points (silk_point; the client's "P", the procedures' mileage)
    bool operator==(const SilkBalance&) const = default;
};

// Every account's silk at its registration (ours; given per account, as the original keeps silk). 10,000 from
// 2026-11-14; none for new accounts from 2026-11-21 (the user's call, "for now"): the accounts made before keep theirs.
constexpr std::uint32_t kStartingSilk = 0;

// The reasons of the original's silk line (0x3154) and its `sk_silkbuylist.silk_reason`.
enum class SilkReason : std::uint8_t {
    Bought = 0,       // "You have bought [%d] silk." (a charge; the Buying list's rows)
    BuyCancelled = 1, // "The purchase of [%d] silk has been canceled."
    GiftReceived = 2, // "You have received [%d] silk as gift."
    GiftSent = 3,     // "You have sent [%d] silk as gift."
    PointsReceived = 4,
    PointsUsed = 5,
};
// Which of the three values a change is to (`sk_silkbuylist.silk_type`: 0 own, 1 gift, 2 points).
enum class SilkKind : std::uint8_t { Own = 0, Gift = 1, Point = 2 };

// The account-wide bus channel (every shard's game servers listen): "<jid>\t<reason>\t<kind>\t<value>".
std::string SilkChannel();
struct SilkNotice {
    std::int64_t account = 0;
    SilkReason reason = SilkReason::GiftReceived;
    SilkKind kind = SilkKind::Own;
    std::int64_t value = 0;
};
std::string EncodeSilkNotice(const SilkNotice& n);
std::optional<SilkNotice> DecodeSilkNotice(std::string_view payload);

// The account's three values (none: 0 / 0 / 0). False when the query failed.
bool LoadSilk(Database& db, std::int64_t account, SilkBalance& out, std::string& error);

// A credit (`amount` > 0) or debit (< 0, never below 0: refused) of one value from outside a purchase: the row (made
// when missing), the log rows, the bus line; one transaction (`db` may already be in one: then it is joined).
// `order` names the credit once: a second with the same order is refused (the original's CGI_WebPurchaseSilk;
// empty: no order). The balance after it in `after`.
enum class SilkGrant : std::uint8_t { Done, NoAccount, Repeated, Short, Failed };
SilkGrant GrantSilk(Database& db, std::int64_t account, std::int64_t amount, SilkKind kind, SilkReason reason, std::string_view order,
                    std::string_view note, SilkBalance& after, std::string& error, bool notify = true);
// Inside a transaction already open (account creation): the same without the transaction of its own.
SilkGrant GrantSilkIn(Database& tx, std::int64_t account, std::int64_t amount, SilkKind kind, SilkReason reason, std::string_view order,
                      std::string_view note, SilkBalance& after, std::string& error, bool notify = true);
// The starting silk of a new account (kStartingSilk), inside its creation's transaction.
bool GiveStartingSilk(Database& tx, std::int64_t account, std::string& error);

// The history (the original's 0x711A / `_GetSilkHistory`): the rows of one list within the last `period` (1 one
// day, 2 a week, 3 a month, 4 three months), the newest first. `list` 1: bought (type 0 reason 0), 2: own given
// (type 0 reason 3), 3: gift received (type 1 reason 2).
struct SilkHistoryRow {
    std::int64_t silk = 0;
    std::int64_t when = 0; // Unix seconds (the database's local time as UTC, as regdate is written)
};
bool LoadSilkHistory(Database& db, std::int64_t account, int list, int period, std::vector<SilkHistoryRow>& out, std::string& error);

// --- The mall's purchase, inside its transaction (Mall.h) -------------------------------------------------------------

// A price taken (the original's `_ConsumeSilkByGameServer` inside `_UPDATE_COST`): `points` of the points chosen, the
// rest from own silk first, then gift silk (ours: the original's split is not traced).
// The row locked by the update; false with nothing changed when the account has too little (or `error` set when the
// query failed). The balance after it in `after`, what each value gave in `paid`.
bool SpendSilkIn(Database& tx, std::int64_t account, std::uint64_t price, std::uint32_t points, SilkBalance& after, SilkBalance& paid, std::string& error);
// The original's log of a package bought (`SK_PackageItemSaleLog`: what each value paid).
bool LogMallSaleIn(Database& tx, std::int64_t account, int shard, std::int64_t character, std::uint32_t package, const SilkBalance& paid, std::string& error);
// A limited good's stock (`_shopitemstockquantity` of the shard: RefShopGroupCodeName, RefPackageItemCodeName,
// StockQuantity), taken by `count` only while that many are left. True when the good has no stock row (no limit).
// `soldOut` true when it has one and too little is left.
bool TakeStockIn(Database& tx, std::string_view group, std::string_view package, int count, bool& soldOut, std::string& error);
// Every limited good's stock left in `group`: (package code, left).
bool LoadStock(Database& db, std::string_view group, std::vector<std::pair<std::string, int>>& out, std::string& error);

} // namespace sr::server
