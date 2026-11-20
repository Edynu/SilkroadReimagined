#include "sr/server/SilkStore.h"

#include <algorithm>
#include <charconv>
#include <format>

#include "sr/server/Database.h"

namespace sr::server {

std::string SilkChannel() { return std::string("sr_silk_") + kAccountSchema; }

std::string EncodeSilkNotice(const SilkNotice& n) {
    return std::format("{}\t{}\t{}\t{}", n.account, int(n.reason), int(n.kind), n.value);
}

std::optional<SilkNotice> DecodeSilkNotice(std::string_view payload) {
    std::int64_t parts[4]{};
    std::size_t at = 0;
    for (int k = 0; k < 4; ++k) {
        std::size_t end = payload.find('\t', at);
        if (end == std::string_view::npos) end = payload.size();
        const std::string_view cell = payload.substr(at, end - at);
        if (std::from_chars(cell.data(), cell.data() + cell.size(), parts[k]).ec != std::errc{}) return std::nullopt;
        at = end + 1;
    }
    if (parts[0] <= 0 || parts[1] < 0 || parts[1] > 5 || parts[2] < 0 || parts[2] > 2) return std::nullopt;
    return SilkNotice{parts[0], SilkReason(parts[1]), SilkKind(parts[2]), parts[3]};
}

bool LoadSilk(Database& db, std::int64_t account, SilkBalance& out, std::string& error) {
    out = {};
    QueryResult q = db.Query("SELECT silk_own, silk_gift, silk_point FROM sro_vt_account.sk_silk WHERE jid = $1", {account});
    if (!q.Ok()) {
        error = q.Error();
        return false;
    }
    if (q.Rows() > 0) {
        out.own = std::uint32_t(std::max<std::int64_t>(0, q.Int(0, 0)));
        out.gift = std::uint32_t(std::max<std::int64_t>(0, q.Int(0, 1)));
        out.point = std::uint32_t(std::max<std::int64_t>(0, q.Int(0, 2)));
    }
    return true;
}

SilkGrant GrantSilkIn(Database& tx, std::int64_t account, std::int64_t amount, SilkKind kind, SilkReason reason, std::string_view order,
                      std::string_view note, SilkBalance& after, std::string& error, bool notify) {
    if (amount == 0) return SilkGrant::Failed;
    QueryResult who = tx.Query("SELECT 1 FROM sro_vt_account.tb_user WHERE jid = $1", {account});
    if (!who.Ok()) {
        error = who.Error();
        return SilkGrant::Failed;
    }
    if (who.Rows() == 0) return SilkGrant::NoAccount;
    // One credit per order (the original's CGI_WebPurchaseSilk refuses an order number seen before).
    if (!order.empty()) {
        QueryResult seen = tx.Query("SELECT 1 FROM sro_vt_account.sk_silkbuylist WHERE ordernumber = $1", {order});
        if (!seen.Ok()) {
            error = seen.Error();
            return SilkGrant::Failed;
        }
        if (seen.Rows() > 0) return SilkGrant::Repeated;
    }
    const char* column = kind == SilkKind::Own ? "silk_own" : kind == SilkKind::Gift ? "silk_gift" : "silk_point";
    if (!tx.Query("INSERT INTO sro_vt_account.sk_silk (jid, silk_own, silk_gift, silk_point) VALUES ($1, 0, 0, 0) ON CONFLICT (jid) DO NOTHING", {account})
             .Ok()) {
        error = "sk_silk";
        return SilkGrant::Failed;
    }
    // The row locked by the change itself; a debit never below 0.
    QueryResult changed = tx.Query(std::format("UPDATE sro_vt_account.sk_silk SET {0} = {0} + $2 WHERE jid = $1 AND {0} + $2 >= 0 "
                                               "RETURNING silk_own, silk_gift, silk_point",
                                               column),
                                   {account, amount});
    if (!changed.Ok()) {
        error = changed.Error();
        return SilkGrant::Failed;
    }
    if (changed.Rows() == 0) return SilkGrant::Short;
    after.own = std::uint32_t(changed.Int(0, 0));
    after.gift = std::uint32_t(changed.Int(0, 1));
    after.point = std::uint32_t(changed.Int(0, 2));
    const std::int64_t remain = kind == SilkKind::Own ? after.own : kind == SilkKind::Gift ? after.gift : after.point;
    // The original's logs of a credit from outside (CGI_WebPurchaseSilk: SK_SilkBuyList and SK_SilkChange_BY_Web).
    QueryResult log = tx.Query("INSERT INTO sro_vt_account.sk_silkbuylist (userjid, silk_type, silk_reason, silk_offset, silk_remain, id, buyquantity, "
                               "ordernumber, slippaper, regdate) VALUES ($1, $2, $3, $4, $5, 0, 1, $6, $7, localtimestamp)",
                               {account, int(kind), int(reason), amount, remain, order, std::string(note.substr(0, 128))});
    QueryResult web = tx.Query("INSERT INTO sro_vt_account.sk_silkchange_by_web (jid, silk_remain, silk_offset, silk_type, reason) VALUES ($1, $2, $3, $4, $5)",
                               {account, remain, amount, int(kind), int(reason)});
    if (!log.Ok() || !web.Ok()) {
        error = log.Ok() ? web.Error() : log.Error();
        return SilkGrant::Failed;
    }
    if (notify && !Notify(tx, SilkChannel(), EncodeSilkNotice({account, reason, kind, amount}))) {
        error = "NOTIFY";
        return SilkGrant::Failed;
    }
    return SilkGrant::Done;
}

SilkGrant GrantSilk(Database& db, std::int64_t account, std::int64_t amount, SilkKind kind, SilkReason reason, std::string_view order,
                    std::string_view note, SilkBalance& after, std::string& error, bool notify) {
    SilkGrant result = SilkGrant::Failed;
    db.Transaction(
        [&](Database& tx) {
            result = GrantSilkIn(tx, account, amount, kind, reason, order, note, after, error, notify);
            return result == SilkGrant::Done;
        },
        error);
    return result;
}

bool GiveStartingSilk(Database& tx, std::int64_t account, std::string& error) {
    SilkBalance after;
    return GrantSilkIn(tx, account, kStartingSilk, SilkKind::Own, SilkReason::Bought, std::format("START-{}", account),
                       "The starting silk of every account (ours)", after, error, false) == SilkGrant::Done;
}

bool LoadSilkHistory(Database& db, std::int64_t account, int list, int period, std::vector<SilkHistoryRow>& out, std::string& error) {
    out.clear();
    int type = 0, reason = 0;
    switch (list) {
    case 1: type = 0, reason = 0; break;
    case 2: type = 0, reason = 3; break;
    case 3: type = 1, reason = 2; break;
    default: error = "no such list"; return false;
    }
    const char* since = period == 1 ? "1 day" : period == 2 ? "7 days" : period == 3 ? "1 month" : period == 4 ? "3 months" : nullptr;
    if (since == nullptr) {
        error = "no such period";
        return false;
    }
    QueryResult q = db.Query(std::format("SELECT silk_offset, extract(epoch FROM regdate)::bigint FROM sro_vt_account.sk_silkbuylist "
                                         "WHERE userjid = $1 AND silk_type = $2 AND silk_reason = $3 "
                                         "AND regdate BETWEEN localtimestamp - interval '{}' AND localtimestamp ORDER BY regdate DESC, buyno DESC LIMIT 1000",
                                         since),
                             {account, type, reason});
    if (!q.Ok()) {
        error = q.Error();
        return false;
    }
    for (int r = 0; r < q.Rows(); ++r) out.push_back({q.Int(r, 0), q.Int(r, 1)});
    return true;
}

bool SpendSilkIn(Database& tx, std::int64_t account, std::uint64_t price, std::uint32_t points, SilkBalance& after, SilkBalance& paid,
                 std::string& error) {
    paid = {};
    QueryResult row = tx.Query("SELECT silk_own, silk_gift, silk_point FROM sro_vt_account.sk_silk WHERE jid = $1 FOR UPDATE", {account});
    if (!row.Ok()) {
        error = row.Error();
        return false;
    }
    if (row.Rows() == 0) return false;
    const std::int64_t own = row.Int(0, 0), gift = row.Int(0, 1), point = row.Int(0, 2);
    const std::int64_t usePoints = std::min<std::int64_t>({std::int64_t(points), point, std::int64_t(price)});
    std::int64_t rest = std::int64_t(price) - usePoints;
    const std::int64_t useOwn = std::min(rest, own);
    rest -= useOwn;
    const std::int64_t useGift = std::min(rest, gift);
    rest -= useGift;
    if (rest > 0 || std::int64_t(points) > point) return false;
    QueryResult changed = tx.Query("UPDATE sro_vt_account.sk_silk SET silk_own = silk_own - $2, silk_gift = silk_gift - $3, silk_point = silk_point - $4 "
                                   "WHERE jid = $1 AND silk_own >= $2 AND silk_gift >= $3 AND silk_point >= $4 RETURNING silk_own, silk_gift, silk_point",
                                   {account, useOwn, useGift, usePoints});
    if (!changed.Ok()) {
        error = changed.Error();
        return false;
    }
    if (changed.Rows() == 0) return false;
    after = {std::uint32_t(changed.Int(0, 0)), std::uint32_t(changed.Int(0, 1)), std::uint32_t(changed.Int(0, 2))};
    paid = {std::uint32_t(useOwn), std::uint32_t(useGift), std::uint32_t(usePoints)};
    return true;
}

bool LogMallSaleIn(Database& tx, std::int64_t account, int shard, std::int64_t character, std::uint32_t package, const SilkBalance& paid, std::string& error) {
    QueryResult q = tx.Query("INSERT INTO sro_vt_account.sk_packageitemsalelog (jid, shardid, charid, packageitemid, silk_own, silk_gift, silk_point, regdate) "
                             "VALUES ($1, $2, $3, $4, $5, $6, $7, localtimestamp)",
                             {account, shard, character, std::int64_t(package), std::int64_t(paid.own), std::int64_t(paid.gift), std::int64_t(paid.point)});
    if (!q.Ok()) error = q.Error();
    return q.Ok();
}

bool TakeStockIn(Database& tx, std::string_view group, std::string_view package, int count, bool& soldOut, std::string& error) {
    soldOut = false;
    QueryResult there = tx.Query("SELECT to_regclass('_shopitemstockquantity') IS NOT NULL");
    if (!there.Ok() || !there.Bool(0, 0)) return true; // no table: nothing is limited
    QueryResult row = tx.Query("SELECT stockquantity FROM _shopitemstockquantity WHERE service = 1 AND refshopgroupcodename = $1 "
                               "AND refpackageitemcodename = $2 FOR UPDATE",
                               {group, package});
    if (!row.Ok()) {
        error = row.Error();
        return false;
    }
    if (row.Rows() == 0) return true;
    QueryResult taken = tx.Query("UPDATE _shopitemstockquantity SET stockquantity = stockquantity - $3 WHERE service = 1 AND refshopgroupcodename = $1 "
                                 "AND refpackageitemcodename = $2 AND stockquantity >= $3",
                                 {group, package, count});
    if (!taken.Ok()) {
        error = taken.Error();
        return false;
    }
    if (taken.Affected() == 0) {
        soldOut = true;
        return false;
    }
    return true;
}

bool LoadStock(Database& db, std::string_view group, std::vector<std::pair<std::string, int>>& out, std::string& error) {
    out.clear();
    QueryResult there = db.Query("SELECT to_regclass('_shopitemstockquantity') IS NOT NULL");
    if (!there.Ok() || !there.Bool(0, 0)) return true;
    QueryResult q = db.Query("SELECT refpackageitemcodename, stockquantity FROM _shopitemstockquantity WHERE service = 1 AND refshopgroupcodename = $1",
                             {group});
    if (!q.Ok()) {
        error = q.Error();
        return false;
    }
    for (int r = 0; r < q.Rows(); ++r) out.emplace_back(q.Text(r, 0), int(q.Int(r, 1)));
    return true;
}

} // namespace sr::server
