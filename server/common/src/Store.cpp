#include "sr/server/Store.h"

#include "sr/server/SilkStore.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <format>
#include <set>
#include <unordered_set>

#include "sr/core/Log.h"
#include "sr/game/OpenMarket.h"
#include "sr/server/Crypto.h"
#include "sr/server/JobStore.h"

// The original's tables: what each call reads and writes is the original's
// meaning of its columns, as measured on the 1.188 database (its procedures were never
// copied). Times in its `timestamp` columns are the
// server's local time (localtimestamp), as SQL Server's getdate() wrote them.

namespace sr::server {

namespace {

std::string Lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

// _blockeduser.type for a login block (*to check against the original's tools*).
constexpr int kLoginBlock = 1;
// A block without end: SQL Server's last datetime, as the original's tools would write it.
constexpr const char* kNoEnd = "9999-12-31 23:59:59";
// The 1.188 export's content id (SRO_CERTIFICATION.Shard.ContentID): new notices and shards get it.
constexpr int kContentId = 22;
// The rows a new character gets, as the original makes them: inventory places 0..108 (13 worn,
// 96 in the bag), 5 avatar places, 51 quick slots.
constexpr int kInventoryRows = 109;
constexpr int kAvatarRows = 5;
constexpr int kQuickSlots = 51;
constexpr int kMaxMagic = 12;

// A timestamp column as Unix seconds (it holds local time).
constexpr const char* kEpoch = "extract(epoch FROM ({} AT TIME ZONE current_setting('TimeZone')))::bigint";
std::string Epoch(std::string_view column) { return std::vformat(kEpoch, std::make_format_args(column)); }

// The magparam columns, for a SELECT or an UPDATE's SET.
std::string MagColumns() {
    std::string out;
    for (int k = 1; k <= kMaxMagic; ++k) out += std::format("{}magparam{}", k > 1 ? ", " : "", k);
    return out;
}

// What a stored item's columns say (from a SELECT of refitemid, optlevel, variance, data,
// magparamnum, magparam1..12 starting at `first`).
void ReadItem(const QueryResult& r, int row, int first, StoredItem& item) {
    item.refId = std::uint32_t(r.Int(row, first));
    item.plus = std::uint8_t(r.Int(row, first + 1));
    item.variance = std::uint64_t(r.Int(row, first + 2));
    const std::int64_t data = r.Int(row, first + 3);
    item.durability = std::uint32_t(std::max<std::int64_t>(0, data));
    item.quantity = std::uint16_t(std::clamp<std::int64_t>(data, 0, 0xFFFF));
    const int count = std::clamp(int(r.Int(row, first + 4)), 0, kMaxMagic);
    for (int k = 0; k < count; ++k) {
        if (!r.IsNull(row, first + 5 + k)) item.magic.push_back(std::uint64_t(r.Int(row, first + 5 + k)));
    }
    item.rentUntil = r.Int(row, first + 5 + kMaxMagic);
    item.advanced = std::uint8_t(std::clamp<std::int64_t>(r.Int(row, first + 6 + kMaxMagic), 0, 255));
    item.advancedRecipe = std::uint32_t(r.Int(row, first + 7 + kMaxMagic));
    item.bound = r.Int(row, first + 9 + kMaxMagic) != 0;
    // Its sockets: "slot:skill:level:stone" each, comma-separated (ItemSelect).
    item.sockets = {};
    const std::string_view list = r.View(row, first + 8 + kMaxMagic);
    for (std::size_t at = 0; at < list.size();) {
        const std::size_t comma = std::min(list.find(',', at), list.size());
        const std::string one(list.substr(at, comma - at));
        long long slot = -1, skill = 0, level = 0, stone = 0;
        if (std::sscanf(one.c_str(), "%lld:%lld:%lld:%lld", &slot, &skill, &level, &stone) == 4 && slot >= 0 && slot < game::kSockets) {
            item.sockets[std::size_t(slot)] = {true, std::uint32_t(skill), std::uint8_t(std::clamp<long long>(level, 0, 255)), std::uint32_t(stone)};
        }
        at = comma + 1;
    }
}

std::string ItemSelect(std::string_view alias) {
    std::string out = std::format("{0}.refitemid, coalesce({0}.optlevel, 0), coalesce({0}.variance, 0), {0}.data, {0}.magparamnum", alias);
    for (int k = 1; k <= kMaxMagic; ++k) out += std::format(", {}.magparam{}", alias, k);
    // A rental's end (_RentItemInfo by the item's id): its latest PeriodEndTime, INT64_MAX when one has
    // none, 0 when the item is no rental.
    out += std::format(", coalesce((SELECT CASE WHEN bool_or(r.periodendtime IS NULL) THEN 9223372036854775807 "
                       "ELSE max(extract(epoch FROM r.periodendtime))::bigint END FROM _rentiteminfo r WHERE r.nitemdbid = {}.id64), 0)",
                       alias);
    // Its advanced elixir (`_BindingOptionWithItem` type 2: the value, the recipe).
    out += std::format(", coalesce((SELECT b.noptvalue FROM _bindingoptionwithitem b WHERE b.nitemdbid = {0}.id64 AND b.bopttype = 2 LIMIT 1), 0)"
                       ", coalesce((SELECT b.noptid FROM _bindingoptionwithitem b WHERE b.nitemdbid = {0}.id64 AND b.bopttype = 2 LIMIT 1), 0)",
                       alias);
    // Its sockets (type 1, a row per made socket).
    out += std::format(", coalesce((SELECT string_agg(concat_ws(':', b.nslot, b.noptid, b.noptlvl, coalesce(b.nparam1, 0)), ',' ORDER BY b.nslot) "
                       "FROM _bindingoptionwithitem b WHERE b.nitemdbid = {}.id64 AND b.bopttype = 1), '')",
                       alias);
    // Bound to its character (ours, sr_bound_items).
    out += std::format(", (EXISTS (SELECT 1 FROM sr_bound_items bi WHERE bi.id64 = {}.id64))::int", alias);
    return out;
}

// Writes what makes `item` this one into the _items row `id` (made if missing, a serial given when
// it has none); its place is the caller's.
bool WriteItem(Database& tx, std::int64_t id, const StoredItem& item, std::string& error) {
    std::vector<Param> p = {id,
                            std::int64_t(item.refId),
                            int(item.plus),
                            std::int64_t(item.variance),
                            std::int64_t(item.counted ? item.quantity : item.durability),
                            int(std::min<std::size_t>(item.magic.size(), kMaxMagic))};
    std::string sets, values;
    for (int k = 0; k < kMaxMagic; ++k) {
        if (std::size_t(k) < item.magic.size()) {
            p.push_back(std::int64_t(item.magic[std::size_t(k)]));
        } else {
            p.push_back(nullptr);
        }
        values += std::format(", ${}", 7 + k);
        sets += std::format(", magparam{0} = EXCLUDED.magparam{0}", k + 1);
    }
    QueryResult put = tx.Query(std::format("INSERT INTO _items (id64, refitemid, optlevel, variance, data, magparamnum, {}) "
                                           "VALUES ($1, $2, $3, $4, $5, $6{}) "
                                           "ON CONFLICT (id64) DO UPDATE SET refitemid = EXCLUDED.refitemid, optlevel = EXCLUDED.optlevel, "
                                           "variance = EXCLUDED.variance, data = EXCLUDED.data, magparamnum = EXCLUDED.magparamnum{}",
                                           MagColumns(), values, sets),
                               p);
    if (!put.Ok()) {
        error = put.Error();
        return false;
    }
    // In use, with a serial of its own.
    QueryResult pool = tx.Query("INSERT INTO _itempool (itemid, inuse) VALUES ($1, 1) ON CONFLICT (itemid) DO UPDATE SET inuse = 1", {id});
    QueryResult serial = tx.Query(
        "WITH s AS (UPDATE _latestitemserial SET latestitemserial = latestitemserial + 1 RETURNING latestitemserial) "
        "UPDATE _items SET serial64 = s.latestitemserial FROM s WHERE id64 = $1 AND serial64 = 0",
        {id});
    if (!pool.Ok() || !serial.Ok()) {
        error = pool.Ok() ? serial.Error() : pool.Error();
        return false;
    }
    // Bound to its character (ours): once, the mark kept with the item's id wherever it goes.
    if (item.bound && !tx.Query("INSERT INTO sr_bound_items (id64) VALUES ($1) ON CONFLICT DO NOTHING", {id}).Ok()) {
        error = "the bound item's row";
        return false;
    }
    // Its advanced elixir, once (the original's _Bind_Option_Manager job 0: type 2, slot 0, level 0, the recipe and
    // its Param8; one per item).
    if (item.advanced > 0) {
        QueryResult bind = tx.Query("INSERT INTO _bindingoptionwithitem (nitemdbid, bopttype, nslot, noptid, noptlvl, noptvalue, nparam1, nparam2) "
                                    "SELECT $1, 2, 0, $2, 0, $3, 0, 0 WHERE NOT EXISTS "
                                    "(SELECT 1 FROM _bindingoptionwithitem WHERE nitemdbid = $1 AND bopttype = 2)",
                                    {id, std::int64_t(item.advancedRecipe), int(item.advanced)});
        if (!bind.Ok()) {
            error = bind.Error();
            return false;
        }
    }
    // Its sockets (the original's _Bind_Option_Manager rows of type 1, one per made socket: an empty one all 0 but its
    // slot; a filled one the skill, its level and the stone). A socket is never unmade: none made, none written.
    if (std::any_of(item.sockets.begin(), item.sockets.end(), [](const game::ItemSocket& s) { return s.made; })) {
        if (!tx.Query("DELETE FROM _bindingoptionwithitem WHERE nitemdbid = $1 AND bopttype = 1", {id}).Ok()) {
            error = "the sockets' rows";
            return false;
        }
        for (std::size_t k = 0; k < item.sockets.size(); ++k) {
            const game::ItemSocket& s = item.sockets[k];
            if (!s.made) continue;
            QueryResult row = tx.Query("INSERT INTO _bindingoptionwithitem (nitemdbid, bopttype, nslot, noptid, noptlvl, noptvalue, nparam1, nparam2) "
                                       "VALUES ($1, 1, $2, $3, $4, 0, $5, 0)",
                                       {id, int(k), std::int64_t(s.skill), int(s.level), std::int64_t(s.stone)});
            if (!row.Ok()) {
                error = row.Error();
                return false;
            }
        }
    }
    return true;
}

// `count` item ids for new items: free ones from _itempool first (the original reuses them), else
// new _items rows; each in use, blank, with a fresh serial. Inside a transaction.
std::vector<std::int64_t> AllocItems(Database& tx, int count) {
    std::vector<std::int64_t> ids;
    if (count <= 0) return ids;
    QueryResult reused = tx.Query(
        "WITH free AS (SELECT itemid FROM _itempool WHERE inuse = 0 AND itemid <> 0 ORDER BY itemid LIMIT $1 FOR UPDATE SKIP LOCKED) "
        "UPDATE _itempool p SET inuse = 1 FROM free WHERE p.itemid = free.itemid RETURNING p.itemid",
        {count});
    if (!reused.Ok()) return {};
    for (int k = 0; k < reused.Rows(); ++k) ids.push_back(reused.Int(k, 0));
    std::string list = "{";
    for (std::size_t k = 0; k < ids.size(); ++k) list += (k ? "," : "") + std::to_string(ids[k]);
    list += "}";
    std::string blank;
    for (int k = 1; k <= kMaxMagic; ++k) blank += std::format(", magparam{} = NULL", k);
    if (!ids.empty()) {
        QueryResult reset = tx.Query(
            std::format("WITH s AS (UPDATE _latestitemserial SET latestitemserial = latestitemserial + $2 RETURNING latestitemserial - $2 AS base) "
                        "UPDATE _items i SET refitemid = 0, optlevel = 0, variance = 0, data = 0, creatername = NULL, magparamnum = 0{}, "
                        "serial64 = s.base + x.ord FROM s, unnest($1::bigint[]) WITH ORDINALITY AS x(id, ord) WHERE i.id64 = x.id",
                        blank),
            {list, int(ids.size())});
        if (!reset.Ok()) return {};
    }
    const int more = count - int(ids.size());
    if (more > 0) {
        QueryResult made = tx.Query(
            "WITH s AS (UPDATE _latestitemserial SET latestitemserial = latestitemserial + $1 RETURNING latestitemserial - $1 AS base), "
            "n AS (INSERT INTO _items (refitemid, optlevel, variance, data, magparamnum, serial64) "
            "      SELECT 0, 0, 0, 0, 0, s.base + g FROM s, generate_series(1, $1::int) g RETURNING id64) "
            "INSERT INTO _itempool (itemid, inuse) SELECT id64, 1 FROM n RETURNING itemid",
            {more});
        if (!made.Ok()) return {};
        for (int k = 0; k < made.Rows(); ++k) ids.push_back(made.Int(k, 0));
    }
    return ids;
}

// An item no one has any more: blank, back in the pool (as the original's deletion leaves it).
bool FreeItem(Database& tx, std::int64_t id) {
    if (id == 0) return true;
    // Its bindings go with it (the original's _Bind_Option_Manager rows: sockets, an advanced elixir).
    if (!tx.Query("DELETE FROM _bindingoptionwithitem WHERE nitemdbid = $1", {id}).Ok()) return false;
    // So does its bound mark (ours): the row is reused for another item.
    if (!tx.Query("DELETE FROM sr_bound_items WHERE id64 = $1", {id}).Ok()) return false;
    std::string blank;
    for (int k = 1; k <= kMaxMagic; ++k) blank += std::format(", magparam{} = NULL", k);
    return tx.Query(std::format("UPDATE _items SET refitemid = 0, optlevel = 0, variance = 0, data = 0, creatername = NULL, magparamnum = 0{} "
                                "WHERE id64 = $1",
                                blank),
                    {id})
               .Ok() &&
           tx.Query("INSERT INTO _itempool (itemid, inuse) VALUES ($1, 0) ON CONFLICT (itemid) DO UPDATE SET inuse = 0", {id}).Ok();
}

// A block of an account (`seconds` 0: no end): the punishment and the login block it points at.
bool Block(Database& tx, std::int64_t account, std::string_view reason, std::int64_t seconds) {
    const auto end = [](int param) {
        return std::format("CASE WHEN ${0}::bigint = 0 THEN '{1}'::timestamp ELSE localtimestamp + make_interval(secs => ${0}::bigint) END", param, kNoEnd);
    };
    QueryResult p = tx.Query(std::format("INSERT INTO sro_vt_account._punishment (userjid, type, executor, shard, charname, charinfo, posinfo, "
                                         "guide, description, raisetime, blockstarttime, blockendtime, punishtime, status) "
                                         "VALUES ($1, $4, 'SroReimagined', 0, NULL, '', '', '', left($2, 1024), localtimestamp, localtimestamp, "
                                         "{}, localtimestamp, 1) RETURNING serialno",
                                         end(3)),
                             {account, reason, seconds, kLoginBlock});
    if (!p.Ok() || p.Rows() != 1) return false;
    const std::int64_t serial = p.Int(0, 0);
    return tx.Query(std::format("INSERT INTO sro_vt_account._blockeduser (userjid, userid, type, serialno, timebegin, timeend) "
                                "SELECT u.jid, u.struserid, $3, $2, localtimestamp, {} FROM sro_vt_account.tb_user u WHERE u.jid = $1 "
                                "ON CONFLICT (userjid, type) DO UPDATE SET serialno = EXCLUDED.serialno, timebegin = EXCLUDED.timebegin, "
                                "timeend = EXCLUDED.timeend",
                                end(4)),
                    {account, serial, kLoginBlock, seconds})
        .Ok();
}

} // namespace

bool ValidAccountName(std::string_view name) {
    if (name.size() < 3 || name.size() > 25) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

bool ValidPassword(std::string_view password) { return password.size() >= 6 && password.size() <= 64; }

// --- Accounts ---------------------------------------------------------------------------------

bool ValidEmail(std::string_view email) {
    if (email.size() < 6 || email.size() > 50) return false;
    if (std::any_of(email.begin(), email.end(), [](unsigned char c) { return c <= ' ' || c == 0x7F; })) return false;
    const std::size_t at = email.find('@');
    if (at == std::string_view::npos || at == 0 || email.find('@', at + 1) != std::string_view::npos) return false;
    const std::size_t dot = email.find('.', at + 2);
    return dot != std::string_view::npos && dot + 1 < email.size();
}

namespace {

// A key's letters and digits only, in upper case: what is hashed and what is shown in groups.
std::string KeyLetters(std::string_view key) {
    std::string out;
    for (const unsigned char c : key)
        if (std::isalnum(c)) out += char(std::toupper(c));
    return out;
}

} // namespace

std::vector<std::uint8_t> BetaKeyHash(std::string_view key) {
    const std::string letters = KeyLetters(key);
    return Sha256(std::span(reinterpret_cast<const std::uint8_t*>(letters.data()), letters.size()));
}

std::vector<std::string> MakeBetaKeys(Database& db, int count, std::string_view note, std::string& error) {
    // 32 letters and digits without the ones read alike (0 O, 1 I): 5 bits each, 80 bits a key.
    static constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::vector<std::string> keys;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            for (int i = 0; i < count; ++i) {
                const std::vector<std::uint8_t> random = RandomBytes(16);
                std::string key;
                for (int c = 0; c < 16; ++c) {
                    if (c > 0 && c % 4 == 0) key += '-';
                    key += kAlphabet[random[std::size_t(c)] % 32];
                }
                if (!tx.Query("INSERT INTO sro_vt_account.sr_beta_keys (key_hash, note) VALUES ($1, $2)", {Param::Bytes(BetaKeyHash(key)), note}).Ok())
                    return false;
                keys.push_back(std::move(key));
            }
            return true;
        },
        error);
    if (!ok) keys.clear();
    return keys;
}

CreateAccountResult CreateAccount(Database& db, std::string_view name, std::string_view password, std::string& error, LegacyPasswords legacy,
                                  const NewAccountDetails& details) {
    if (!ValidAccountName(name)) return CreateAccountResult::InvalidName;
    if (!ValidPassword(password)) return CreateAccountResult::InvalidPassword;
    const std::string hash = HashPassword(password);
    if (hash.empty()) {
        error = "password hashing failed";
        return CreateAccountResult::Failed;
    }
    const std::string old = legacy == LegacyPasswords::Keep ? LegacyPasswordHash(password) : std::string();
    CreateAccountResult result = CreateAccountResult::Failed;
    db.Transaction(
        [&](Database& tx) {
            QueryResult made = tx.Query("INSERT INTO sro_vt_account.tb_user (struserid, password, regtime, email, reg_ip) "
                                        "VALUES ($1, $2, localtimestamp, $3, $4) RETURNING jid",
                                        {name, old, details.email.empty() ? Param(nullptr) : Param(details.email),
                                         details.address.empty() ? Param(nullptr) : Param(details.address.substr(0, 25))});
            if (!made.Ok()) {
                if (made.State() == "23505") {
                    result = CreateAccountResult::NameTaken;
                } else {
                    error = made.Error();
                }
                return false;
            }
            if (details.betaKeyHash) {
                QueryResult used = tx.Query("UPDATE sro_vt_account.sr_beta_keys SET used_by = $2, used_at = now() WHERE key_hash = $1 AND used_by IS NULL",
                                            {Param::Bytes(*details.betaKeyHash), made.Int(0, 0)});
                if (!used.Ok()) {
                    error = used.Error();
                    return false;
                }
                if (used.Affected() != 1) {
                    result = CreateAccountResult::KeyRefused;
                    return false;
                }
            }
            QueryResult ours = tx.Query("INSERT INTO sro_vt_account.sr_account_security (jid, password_hash) VALUES ($1, $2)", {made.Int(0, 0), hash});
            if (!ours.Ok()) {
                error = ours.Error();
                return false;
            }
            // Every new account's starting silk (SilkStore.h kStartingSilk), in the same transaction.
            if (!GiveStartingSilk(tx, made.Int(0, 0), error)) return false;
            result = CreateAccountResult::Created;
            return true;
        },
        error);
    return result;
}

std::optional<AccountRow> FindAccount(Database& db, std::string_view name, bool& ok) {
    QueryResult r = db.Query(std::format("SELECT u.jid, u.struserid, coalesce(s.password_hash, ''), u.password, coalesce(s.failed_logins, 0), "
                                         "       b.userjid IS NOT NULL, "
                                         "       CASE WHEN b.timeend IS NULL OR b.timeend >= '9999-01-01' THEN 0 ELSE {} END, "
                                         "       coalesce(p.description, '') "
                                         "FROM sro_vt_account.tb_user u "
                                         "LEFT JOIN sro_vt_account.sr_account_security s ON s.jid = u.jid "
                                         "LEFT JOIN sro_vt_account._blockeduser b ON b.userjid = u.jid AND b.type = $2 "
                                         "     AND b.timebegin <= localtimestamp AND b.timeend > localtimestamp "
                                         "LEFT JOIN sro_vt_account._punishment p ON p.serialno = b.serialno "
                                         "WHERE lower(u.struserid) = $1",
                                         Epoch("b.timeend")),
                             {Lower(name), kLoginBlock});
    ok = r.Ok();
    if (!ok || r.Rows() == 0) return std::nullopt;
    AccountRow row;
    row.id = r.Int(0, 0);
    row.name = r.Text(0, 1);
    row.passwordHash = r.Text(0, 2);
    row.legacyHash = r.Text(0, 3);
    row.failedLogins = int(r.Int(0, 4));
    row.blocked = r.Bool(0, 5);
    row.blockedUntil = r.Int(0, 6);
    row.blockReason = r.Text(0, 7);
    return row;
}

int RecordWrongPassword(Database& db, std::int64_t account, int limit, int blockSeconds, const std::string& reason) {
    int count = 0;
    std::string error;
    db.Transaction(
        [&](Database& tx) {
            // Counted under the row's lock: concurrent wrong passwords count once each.
            QueryResult r = tx.Query("INSERT INTO sro_vt_account.sr_account_security (jid, failed_logins) VALUES ($1, 1) "
                                     "ON CONFLICT (jid) DO UPDATE SET failed_logins = sr_account_security.failed_logins + 1 RETURNING failed_logins",
                                     {account});
            if (!r.Ok() || r.Rows() != 1) return false;
            count = int(r.Int(0, 0));
            if (count < limit) return true;
            count = limit;
            return tx.Query("UPDATE sro_vt_account.sr_account_security SET failed_logins = 0 WHERE jid = $1", {account}).Ok() &&
                   Block(tx, account, reason, blockSeconds);
        },
        error);
    return count;
}

void RecordLogin(Database& db, std::int64_t account) {
    db.Query("INSERT INTO sro_vt_account.sr_account_security (jid, last_login_at) VALUES ($1, now()) "
             "ON CONFLICT (jid) DO UPDATE SET failed_logins = 0, last_login_at = now()",
             {account});
}

bool StorePasswordHash(Database& db, std::int64_t account, const std::string& phc, LegacyPasswords legacy) {
    const bool ours = db.Query("INSERT INTO sro_vt_account.sr_account_security (jid, password_hash) VALUES ($1, $2) "
                               "ON CONFLICT (jid) DO UPDATE SET password_hash = EXCLUDED.password_hash",
                               {account, phc})
                          .Ok();
    if (!ours || legacy == LegacyPasswords::Keep) return ours;
    return db.Query("UPDATE sro_vt_account.tb_user SET password = '' WHERE jid = $1", {account}).Ok();
}

std::optional<std::pair<std::int64_t, std::string>> AccountBlock(Database& db, std::int64_t account) {
    QueryResult r = db.Query(std::format("SELECT CASE WHEN b.timeend >= '9999-01-01' THEN 0 ELSE {} END, coalesce(p.description, '') "
                                         "FROM sro_vt_account._blockeduser b LEFT JOIN sro_vt_account._punishment p ON p.serialno = b.serialno "
                                         "WHERE b.userjid = $1 AND b.type = $2 AND b.timebegin <= localtimestamp AND b.timeend > localtimestamp",
                                         Epoch("b.timeend")),
                             {account, kLoginBlock});
    if (!r.Ok() || r.Rows() == 0) return std::nullopt;
    return std::make_pair(r.Int(0, 0), r.Text(0, 1));
}

AccountRestrictions LoadRestrictions(Database& db, std::int64_t account) {
    AccountRestrictions out;
    QueryResult r = db.Query(std::format("SELECT b.type, CASE WHEN b.timeend >= '9999-01-01' THEN 0 ELSE {} END "
                                         "FROM sro_vt_account._blockeduser b "
                                         "WHERE b.userjid = $1 AND b.type IN (3, 4) AND b.timebegin <= localtimestamp AND b.timeend > localtimestamp",
                                         Epoch("b.timeend")),
                             {account});
    for (int i = 0; r.Ok() && i < r.Rows(); ++i) {
        std::optional<std::int64_t>& until = r.Int(i, 0) == 3 ? out.tradeUntil : out.chatUntil;
        const std::int64_t end = r.Int(i, 1);
        // The later of two rows of a kind (no end the latest).
        if (!until || end == 0 || (*until != 0 && end > *until)) until = end;
    }
    return out;
}

bool SetBlock(Database& db, std::string_view name, std::string_view reason, std::int64_t seconds) {
    QueryResult who = db.Query("SELECT jid FROM sro_vt_account.tb_user WHERE lower(struserid) = $1", {Lower(name)});
    if (!who.Ok() || who.Rows() != 1) return false;
    const std::int64_t account = who.Int(0, 0);
    if (reason.empty()) {
        return db.Query("DELETE FROM sro_vt_account._blockeduser WHERE userjid = $1 AND type = $2", {account, kLoginBlock}).Ok();
    }
    std::string error;
    return db.Transaction([&](Database& tx) { return Block(tx, account, reason, seconds); }, error);
}

// --- Game servers -----------------------------------------------------------------------------

bool SaveServerSample(Database& db, const ServerSample& s) {
    const bool saved = db.Query("INSERT INTO sro_vt_account.sr_server_samples (shard_id, ticks, tick_avg_ms, tick_max_ms, over_budget, budget_ms, players, monsters) "
                                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)",
                                {int(s.server), std::int64_t(s.ticks), s.tickAvgMs, s.tickMaxMs, std::int64_t(s.overBudget), s.budgetMs, std::int64_t(s.players),
                                 std::int64_t(s.monsters)})
                           .Ok();
    db.Query("DELETE FROM sro_vt_account.sr_server_samples WHERE shard_id = $1 AND at < now() - interval '7 days'", {int(s.server)});
    return saved;
}

bool ReportGameServer(Database& db, const GameServerInfo& s, std::string& error) {
    const std::int64_t capacity = std::min<std::int64_t>(s.capacity, 32767);
    const int ratio = capacity > 0 ? int(std::min<std::int64_t>(100, std::int64_t(s.online) * 100 / capacity)) : 0;
    QueryResult shard = db.Query(
        "INSERT INTO sro_vt_account._shard (nid, nfarmid, ncontentid, szname, szdesc, szdbconfig, nmaxuser, nstatus, ncurrentuserratio) "
        "VALUES ($1, $2, $3, left($4, 32), '', $5, $6, 1, $7) "
        "ON CONFLICT (nid) DO UPDATE SET nfarmid = EXCLUDED.nfarmid, szname = EXCLUDED.szname, szdbconfig = EXCLUDED.szdbconfig, "
        "  nmaxuser = EXCLUDED.nmaxuser, nstatus = 1, ncurrentuserratio = EXCLUDED.ncurrentuserratio",
        {int(s.id), int(s.group), kContentId, s.name, s.schema, capacity, ratio});
    QueryResult report = db.Query(
        "INSERT INTO sro_vt_account.sr_shard_reports (shard_id, group_name, host, port, online, reported_at, stopped, started_at, shard_capacity) "
        "VALUES ($1, $2, $3, $4, $5, now(), false, now(), $6) "
        "ON CONFLICT (shard_id) DO UPDATE SET group_name = $2, host = $3, port = $4, online = $5, reported_at = now(), stopped = false, "
        "  shard_capacity = $6, "
        // A new run when it had stopped or been silent a minute (its uptime starts again).
        "  started_at = CASE WHEN sr_shard_reports.stopped OR sr_shard_reports.started_at IS NULL "
        "                      OR sr_shard_reports.reported_at < now() - interval '60 seconds' THEN now() ELSE sr_shard_reports.started_at END",
        {int(s.id), s.groupName, s.host, int(s.port), std::int64_t(s.online), std::int64_t(s.shardCapacity)});
    if (!shard.Ok()) error = shard.Error();
    if (!report.Ok()) error = report.Error();
    return shard.Ok() && report.Ok();
}

bool StopGameServer(Database& db, std::uint16_t id) {
    const bool a = db.Query("UPDATE sro_vt_account.sr_shard_reports SET stopped = true, online = 0 WHERE shard_id = $1", {int(id)}).Ok();
    const bool b = db.Query("UPDATE sro_vt_account._shard SET nstatus = 0, ncurrentuserratio = 0 WHERE nid = $1", {int(id)}).Ok();
    return a && b;
}

std::vector<GameServerInfo> ListGameServers(Database& db, int staleSeconds, bool& ok) {
    QueryResult r = db.Query(
        "SELECT s.nid, s.szname, s.nfarmid, coalesce(r.group_name, ''), coalesce(r.host, ''), coalesce(r.port, 0), s.nmaxuser, "
        "       coalesce(r.online, 0), s.szdbconfig, "
        "       coalesce(NOT r.stopped AND r.reported_at > now() - make_interval(secs => $1), false), "
        "       coalesce(floor(extract(epoch FROM r.started_at))::bigint, 0), coalesce(r.shard_capacity, 0) "
        "FROM sro_vt_account._shard s LEFT JOIN sro_vt_account.sr_shard_reports r ON r.shard_id = s.nid ORDER BY s.nid",
        {staleSeconds});
    ok = r.Ok();
    std::vector<GameServerInfo> out;
    if (!ok) return out;
    for (int i = 0; i < r.Rows(); ++i) {
        GameServerInfo s;
        s.id = std::uint16_t(r.Int(i, 0));
        s.name = r.Text(i, 1);
        s.group = std::uint8_t(r.Int(i, 2));
        s.groupName = r.Text(i, 3);
        s.host = r.Text(i, 4);
        s.port = std::uint16_t(r.Int(i, 5));
        s.capacity = std::uint32_t(r.Int(i, 6));
        s.online = std::uint32_t(r.Int(i, 7));
        s.schema = r.Text(i, 8);
        s.reporting = r.Bool(i, 9);
        s.startedAt = r.Int(i, 10);
        s.shardCapacity = std::uint32_t(r.Int(i, 11));
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<GameServerInfo> ShardsOf(const std::vector<GameServerInfo>& servers) {
    std::vector<GameServerInfo> shards;
    for (const GameServerInfo& s : servers) {
        const auto same = std::find_if(shards.begin(), shards.end(), [&](const GameServerInfo& x) { return x.schema == s.schema; });
        if (same == shards.end()) {
            shards.push_back(s);
            continue;
        }
        same->online += s.online;
        same->capacity += s.capacity;
        same->shardCapacity = std::max(same->shardCapacity, s.shardCapacity);
        same->reporting = same->reporting || s.reporting;
    }
    // The shard's own capacity where its servers report it (all of them together), else theirs added up.
    for (GameServerInfo& shard : shards)
        if (shard.shardCapacity > 0) shard.capacity = shard.shardCapacity;
    return shards;
}

std::optional<GameServerInfo> ChooseLobbyServer(const std::vector<GameServerInfo>& servers, std::uint16_t row, std::uint16_t lobby) {
    const auto named = std::find_if(servers.begin(), servers.end(), [&](const GameServerInfo& s) { return s.id == row; });
    if (named == servers.end()) return std::nullopt;
    const GameServerInfo* best = nullptr;
    // Most room first (by share, so a small server is not filled before a big one); the lowest number on a tie.
    const auto load = [](const GameServerInfo& s) { return s.capacity > 0 ? double(s.online) / double(s.capacity) : 1.0; };
    for (const GameServerInfo& s : servers) {
        if (s.schema != named->schema || !s.reporting) continue;
        if (s.id == lobby) return s;
        if (best == nullptr || load(s) < load(*best)) best = &s;
    }
    if (best == nullptr) return std::nullopt;
    return *best;
}

std::vector<std::pair<std::int16_t, std::string>> LoadRegionAreas(Database& db, bool& ok) {
    std::vector<std::pair<std::int16_t, std::string>> out;
    const QueryResult exists = db.Query("SELECT to_regclass('_refregion') IS NOT NULL");
    ok = exists.Ok();
    if (!ok || !exists.Bool(0, 0)) return out;
    const QueryResult r = db.Query("SELECT wregionid, continentname FROM _refregion");
    ok = r.Ok();
    if (!ok) return out;
    out.reserve(std::size_t(r.Rows()));
    for (int i = 0; i < r.Rows(); ++i) out.emplace_back(std::int16_t(r.Int(i, 0)), r.Text(i, 1));
    return out;
}

ClimateData LoadClimates(Database& db, bool& ok) {
    ClimateData out;
    const QueryResult exists = db.Query("SELECT to_regclass('_refclimate') IS NOT NULL AND to_regclass('_refregion') IS NOT NULL");
    ok = exists.Ok();
    if (!ok || !exists.Bool(0, 0)) return out;
    const QueryResult c = db.Query(
        "SELECT id, initialweather, initialamount, changeweather, division, duration, durationvariance, snowfall, snowfallvariance, probsnow, "
        "rainfall, rainfallvariance, probrain FROM _refclimate ORDER BY id");
    ok = c.Ok();
    if (!ok) return out;
    for (int i = 0; i < c.Rows(); ++i) {
        game::ClimateRow r;
        r.id = std::int32_t(c.Int(i, 0));
        r.initialWeather = std::uint8_t(c.Int(i, 1));
        r.initialAmount = std::uint8_t(c.Int(i, 2));
        r.changes = c.Int(i, 3) != 0;
        r.division = std::uint8_t(c.Int(i, 4));
        r.durationMs = std::int32_t(c.Int(i, 5));
        r.durationVarianceMs = std::int32_t(c.Int(i, 6));
        r.snowfall = std::uint8_t(c.Int(i, 7));
        r.snowfallVariance = std::uint8_t(c.Int(i, 8));
        r.probSnow = std::uint8_t(c.Int(i, 9));
        r.rainfall = std::uint8_t(c.Int(i, 10));
        r.rainfallVariance = std::uint8_t(c.Int(i, 11));
        r.probRain = std::uint8_t(c.Int(i, 12));
        out.climates.push_back(r);
    }
    const QueryResult regions = db.Query("SELECT wregionid, climate FROM _refregion WHERE climate IS NOT NULL");
    ok = regions.Ok();
    if (!ok) return out;
    for (int i = 0; i < regions.Rows(); ++i) out.regionClimate[std::uint16_t(std::int16_t(regions.Int(i, 0)))] = std::int32_t(regions.Int(i, 1));
    return out;
}

// --- Login tickets ----------------------------------------------------------------------------

std::vector<std::uint8_t> IssueTicket(Database& db, std::int64_t account, std::uint16_t server, int seconds, std::string& error) {
    std::vector<std::uint8_t> ticket = RandomBytes(32);
    if (ticket.empty()) {
        error = "no random bytes";
        return {};
    }
    QueryResult r = db.Query(
        "INSERT INTO sro_vt_account.sr_login_tickets (token_hash, jid, shard_id, expires_at) VALUES ($1, $2, $3, now() + make_interval(secs => $4))",
        {Param::Bytes(Sha256(ticket)), account, int(server), seconds});
    if (!r.Ok()) {
        error = r.Error();
        return {};
    }
    return ticket;
}

std::vector<std::uint8_t> IssueMoveTicket(Database& db, std::int64_t account, std::int64_t character, std::uint16_t to, int seconds, std::string& error) {
    std::vector<std::uint8_t> ticket = RandomBytes(32);
    if (ticket.empty()) {
        error = "no random bytes";
        return {};
    }
    const bool ok = db.Transaction(
        [&](Database& tx) {
            return tx.Query("INSERT INTO sro_vt_account.sr_login_tickets (token_hash, jid, shard_id, expires_at, character_id) "
                            "VALUES ($1, $2, $3, now() + make_interval(secs => $4), $5)",
                            {Param::Bytes(Sha256(ticket)), account, int(to), seconds, character}).Ok() &&
                   tx.Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id, token) VALUES ($1, $2, 0) "
                            "ON CONFLICT (jid) DO UPDATE SET shard_id = $2, since = now(), token = 0",
                            {account, int(to)}).Ok();
        },
        error);
    return ok ? ticket : std::vector<std::uint8_t>{};
}

void ReleaseAbandonedSessions(Database& db, std::uint16_t server, const std::vector<std::int64_t>& connected, int seconds) {
    std::string ids = "{";
    for (const std::int64_t id : connected) ids += (ids.size() > 1 ? "," : "") + std::to_string(id);
    ids += "}";
    db.Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE shard_id = $1 AND since < now() - make_interval(secs => $2) AND NOT (jid = ANY($3::int[]))",
             {int(server), seconds, ids});
}

std::optional<std::int64_t> ConsumeTicket(Database& db, const std::vector<std::uint8_t>& ticket, std::uint16_t server, std::int64_t* character) {
    if (ticket.size() != 32) return std::nullopt;
    // Deleted whatever its server or age: a ticket shown to the wrong server is burnt.
    QueryResult r = db.Query("DELETE FROM sro_vt_account.sr_login_tickets WHERE token_hash = $1 RETURNING jid, shard_id = $2 AND expires_at > now(), character_id",
                             {Param::Bytes(Sha256(ticket)), int(server)});
    if (!r.Ok() || r.Rows() != 1 || !r.Bool(0, 1)) return std::nullopt;
    if (character != nullptr) *character = r.Int(0, 2);
    return r.Int(0, 0);
}

int PurgeTickets(Database& db) {
    QueryResult r = db.Query("DELETE FROM sro_vt_account.sr_login_tickets WHERE expires_at <= now()");
    return r.Ok() ? r.Affected() : 0;
}

// --- Online sessions --------------------------------------------------------------------------

bool ClaimSession(Database& db, std::int64_t account, std::uint16_t server, int staleSeconds, std::int64_t token, bool& ok) {
    // New, or taken over from a game server that no longer reports.
    QueryResult r = db.Query(
        "INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id, token) VALUES ($1, $2, $4) "
        "ON CONFLICT (jid) DO UPDATE SET shard_id = $2, since = now(), token = $4, character_id = NULL "
        "  WHERE NOT EXISTS (SELECT 1 FROM sro_vt_account.sr_shard_reports g WHERE g.shard_id = sr_online_sessions.shard_id AND NOT g.stopped "
        "                    AND g.reported_at > now() - make_interval(secs => $3)) "
        "RETURNING 1",
        {account, int(server), staleSeconds, token});
    ok = r.Ok();
    return ok && r.Rows() == 1;
}

void AdoptSession(Database& db, std::int64_t account, std::uint16_t server, std::int64_t token) {
    db.Query("UPDATE sro_vt_account.sr_online_sessions SET token = $3 WHERE jid = $1 AND shard_id = $2", {account, int(server), token});
}

void ReleaseSession(Database& db, std::int64_t account, std::uint16_t server, std::int64_t token) {
    db.Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1 AND shard_id = $2 AND token = $3", {account, int(server), token});
}

void ReleaseServerSessions(Database& db, std::uint16_t server) {
    db.Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE shard_id = $1", {int(server)});
}

bool HasLiveSession(Database& db, std::int64_t account, int staleSeconds, bool& ok) {
    QueryResult r = db.Query(
        "SELECT 1 FROM sro_vt_account.sr_online_sessions s JOIN sro_vt_account.sr_shard_reports g ON g.shard_id = s.shard_id "
        "WHERE s.jid = $1 AND NOT g.stopped AND g.reported_at > now() - make_interval(secs => $2)",
        {account, staleSeconds});
    ok = r.Ok();
    return ok && r.Rows() > 0;
}

// --- Characters -------------------------------------------------------------------------------

namespace {

// Everything of a character, once its deletion's wait has passed, as the original's permanent deletion
// (_DeleteCharPermanently_NoTX) clears it: its own rows and items, its pets with their bags, its place in a guild and an
// academy, both sides of its friendships, its letters and whisper blocks. Its _char row (marked deleted) and its name
// in _charnamelist stay: the name is never free again, as in the original. A guild's master is left as it is (the
// request refuses one; nothing is deleted while it leads). `gone`: what it left (its character 0 when nothing was done).
bool DeleteCharacterData(Database& tx, std::int64_t character, CharacterGone& gone) {
    gone = {};
    const QueryResult master = tx.Query("SELECT 1 FROM _guildmember WHERE charid = $1 AND memberclass = 0", {character});
    if (!master.Ok()) return false;
    if (master.Rows() > 0) return true;
    gone.character = character;
    QueryResult items = tx.Query("SELECT itemid FROM _inventory WHERE charid = $1 AND itemid <> 0 "
                                 "UNION SELECT itemid FROM _inventoryforavatar WHERE charid = $1 AND itemid <> 0 "
                                 "UNION SELECT b.itemid FROM _invcos b JOIN _charcos c ON c.id = b.cosid "
                                 "WHERE c.ownercharid = $1 AND b.itemid IS NOT NULL AND b.itemid <> 0",
                                 {character});
    if (!items.Ok()) return false;
    for (int k = 0; k < items.Rows(); ++k) {
        if (!FreeItem(tx, items.Int(k, 0))) return false;
    }
    // Its pets: their bags, timed jobs and rows.
    if (!tx.Query("DELETE FROM _invcos WHERE cosid IN (SELECT id FROM _charcos WHERE ownercharid = $1)", {character}).Ok() ||
        !tx.Query("DELETE FROM _timedjobforpet WHERE charid IN (SELECT id FROM _charcos WHERE ownercharid = $1)", {character}).Ok() ||
        !tx.Query("DELETE FROM _charcos WHERE ownercharid = $1", {character}).Ok()) {
        return false;
    }
    // Its guild and its academy.
    const QueryResult guild = tx.Query("DELETE FROM _guildmember WHERE charid = $1 RETURNING guildid", {character});
    const QueryResult camp = tx.Query("DELETE FROM _trainingcampmember WHERE charid = $1 RETURNING campid", {character});
    if (!guild.Ok() || !camp.Ok()) return false;
    if (guild.Rows() > 0) gone.guild = std::uint32_t(guild.Int(0, 0));
    if (camp.Rows() > 0) gone.academy = std::uint32_t(camp.Int(0, 0));
    // Its friends both ways, its letters, its whisper blocks, the academy's buffs it gave or held, its safe trade.
    if (!tx.Query("DELETE FROM _trainingcampbuffstatus WHERE recipientcharid = $1 OR donorcharid = $1", {character}).Ok() ||
        !tx.Query("DELETE FROM _friend WHERE charid = $1 OR friendcharid = $1", {character}).Ok() ||
        !tx.Query("DELETE FROM _memo WHERE charid = $1", {character}).Ok() ||
        !tx.Query("DELETE FROM _blockedwhisperers WHERE ownerid = $1", {character}).Ok() ||
        !tx.Query("DELETE FROM _chartrijobsafetrade WHERE charid = $1", {character}).Ok()) {
        return false;
    }
    for (const char* table : {"_inventory", "_inventoryforavatar", "_charskill", "_charskillmastery", "_charquest", "_clientconfig", "_timedjob",
                              "_staticavatar", "_chartrijob", "_deletedchar", "_user", "sr_char_state", "sr_auto_potion", "sr_char_records", "sr_bargain_lock",
                              "_charcollectionbook", "_charinstanceworlddata"}) {
        if (!tx.Query(std::format("DELETE FROM {} WHERE charid = $1", table), {character}).Ok()) return false;
    }
    return tx.Query("UPDATE _char SET deleted = 1 WHERE charid = $1", {character}).Ok();
}

} // namespace

std::vector<CharacterRow> ListCharacters(Database& db, std::int64_t account, std::uint16_t, int deleteSeconds, bool& ok, std::vector<CharacterGone>* gone) {
    std::vector<CharacterRow> out;
    // Those whose wait has passed go first.
    QueryResult due = db.Query("SELECT d.charid FROM _deletedchar d JOIN _user u ON u.charid = d.charid "
                               "WHERE u.userjid = $1 AND d.deleteddate + make_interval(secs => $2) <= localtimestamp",
                               {account, deleteSeconds});
    ok = due.Ok();
    for (int k = 0; ok && k < due.Rows(); ++k) {
        std::string error;
        const std::int64_t character = due.Int(k, 0);
        CharacterGone left;
        ok = db.Transaction([&](Database& tx) { return DeleteCharacterData(tx, character, left); }, error);
        if (ok && gone != nullptr && left.character != 0) gone->push_back(left);
    }
    QueryResult r = db.Query(
        "SELECT c.charid, c.charname16, c.refobjid, c.scale, c.curlevel, c.expoffset, c.strength, c.intellect, c.remainstatpoint, c.hp, c.mp, "
        "       c.deleted <> 0, coalesce(ceil(extract(epoch FROM d.deleteddate + make_interval(secs => $2) - localtimestamp))::bigint, 0) "
        "FROM _user u JOIN _char c ON c.charid = u.charid LEFT JOIN _deletedchar d ON d.charid = c.charid "
        "WHERE u.userjid = $1 AND c.charid <> 0 ORDER BY c.charid",
        {account, deleteSeconds});
    QueryResult items = db.Query("SELECT i.charid, i.slot, it.refitemid, coalesce(it.optlevel, 0) FROM _inventory i "
                                 "JOIN _items it ON it.id64 = i.itemid JOIN _user u ON u.charid = i.charid "
                                 "WHERE u.userjid = $1 AND i.slot < 13 AND i.itemid <> 0 ORDER BY i.charid, i.slot",
                                 {account});
    ok = ok && r.Ok() && items.Ok();
    if (!ok) return out;
    for (int i = 0; i < r.Rows(); ++i) {
        CharacterRow c;
        c.id = r.Int(i, 0);
        c.name = r.Text(i, 1);
        c.model = std::uint32_t(r.Int(i, 2));
        c.height = std::uint8_t(r.Int(i, 3));
        c.level = std::uint8_t(r.Int(i, 4));
        c.experience = std::uint64_t(r.Int(i, 5));
        c.strength = std::uint16_t(r.Int(i, 6));
        c.intelligence = std::uint16_t(r.Int(i, 7));
        c.statPoints = std::uint16_t(r.Int(i, 8));
        c.hp = std::uint32_t(r.Int(i, 9));
        c.mp = std::uint32_t(r.Int(i, 10));
        c.deleting = r.Bool(i, 11);
        c.deleteSeconds = c.deleting ? std::max<std::int64_t>(0, r.Int(i, 12)) : 0;
        for (int k = 0; k < items.Rows(); ++k) {
            if (items.Int(k, 0) == c.id) {
                c.equipment.push_back({EquipSlot(items.Int(k, 1)), std::uint32_t(items.Int(k, 2)), std::uint8_t(items.Int(k, 3))});
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

CreateCharacterResult CreateCharacter(Database& db, std::int64_t account, std::uint16_t, const NewCharacter& character, int maxCharacters,
                                      std::string& error) {
    CreateCharacterResult result = CreateCharacterResult::Failed;
    db.Transaction(
        [&](Database& tx) {
            // One creation per account at a time, so two at once cannot both pass the count.
            if (!tx.Query("SELECT pg_advisory_xact_lock($1)", {account}).Ok()) return false;
            QueryResult count = tx.Query("SELECT count(*) FROM _user WHERE userjid = $1", {account});
            if (!count.Ok()) {
                error = count.Error();
                return false;
            }
            if (count.Int(0, 0) >= maxCharacters) {
                result = CreateCharacterResult::TooMany;
                return false;
            }
            // As the original's creation makes it: 45 places, the place given on entering (ours).
            QueryResult made = tx.Query(
                "INSERT INTO _char (refobjid, charname16, scale, strength, intellect, curlevel, maxlevel, hp, mp, posx, posy, posz, inventorysize, "
                "                   lastlogout, worldid, appointedteleport) "
                "VALUES ($1, $2, $3, $4, $5, $6, $6, $7, $8, 0, 0, 0, 45, localtimestamp, 1, $9) RETURNING charid",
                {std::int64_t(character.model), character.name, int(character.height), int(character.strength), int(character.intelligence),
                 int(character.level), std::int64_t(character.hp), std::int64_t(character.mp), std::int64_t(character.returnGate)});
            if (!made.Ok()) {
                if (made.State() == "23505") {
                    result = CreateCharacterResult::NameTaken;
                } else {
                    error = made.Error();
                }
                return false;
            }
            const std::int64_t id = made.Int(0, 0);
            const auto run = [&](std::string_view sql, std::vector<Param> params) {
                QueryResult q = tx.Query(sql, params);
                if (!q.Ok()) error = q.Error();
                return q.Ok();
            };
            if (!run("INSERT INTO _user (userjid, charid) VALUES ($1, $2)", {account, id}) ||
                !run("INSERT INTO _charnamelist (charname16, charid) VALUES (left($1, 17), $2)", {character.name, id}) ||
                !run("INSERT INTO _inventory (charid, slot, itemid) SELECT $1, g, 0 FROM generate_series(0, $2::int - 1) g", {id, kInventoryRows}) ||
                !run("INSERT INTO _inventoryforavatar (charid, slot, itemid) SELECT $1, g, 0 FROM generate_series(0, $2::int - 1) g", {id, kAvatarRows}) ||
                !run("INSERT INTO _clientconfig (charid, configtype, slotseq, slottype, data) VALUES ($1, 0, 0, 0, 0)", {id}) ||
                !run("INSERT INTO _clientconfig (charid, configtype, slotseq, slottype, data) SELECT $1, 1, g, 0, 0 FROM generate_series(0, $2::int - 1) g",
                     {id, kQuickSlots}) ||
                !run("INSERT INTO _clientconfig (charid, configtype, slotseq, slottype, data) SELECT $1, t, 0, 0, 0 FROM generate_series(11, 14) t", {id}) ||
                !run("INSERT INTO _staticavatar (charid) VALUES ($1)", {id}) ||
                !run("INSERT INTO _chartrijob (charid, jobtype, level, exp, contribution, reward) VALUES ($1, 0, 1, 0, 0, 0)", {id}) ||
                !run("INSERT INTO sr_char_state (charid, entered) VALUES ($1, false)", {id})) {
                return false;
            }
            // What the original's creation gives it from the database's tables, when they are there (an
            // import): every mastery of its race at level 0, the race's basic skills, its first quests
            // (the race is the model's country; 3: both).
            QueryResult defaults = tx.Query("SELECT to_regclass('_refobjcommon') IS NOT NULL AND to_regclass('_refchardefault_skillmastery') IS NOT NULL "
                                            "AND to_regclass('_refchardefault_skill') IS NOT NULL, "
                                            "to_regclass('_refchardefault_quest') IS NOT NULL AND to_regclass('_refquest') IS NOT NULL");
            if (!defaults.Ok()) {
                error = defaults.Error();
                return false;
            }
            const std::string race = "(d.race = 3 OR d.race = (SELECT o.country FROM _refobjcommon o WHERE o.id = $2))";
            if (defaults.Bool(0, 0) &&
                (!run("INSERT INTO _charskillmastery (charid, masteryid, level) SELECT $1, d.masteryid, 0 FROM _refchardefault_skillmastery d WHERE " + race +
                          " ON CONFLICT DO NOTHING",
                      {id, std::int64_t(character.model)}) ||
                 !run("INSERT INTO _charskill (charid, skillid, enable) SELECT $1, d.skillid, 1 FROM _refchardefault_skill d WHERE " + race +
                          " ON CONFLICT DO NOTHING",
                      {id, std::int64_t(character.model)}))) {
                return false;
            }
            if (defaults.Bool(0, 1) &&
                !run("INSERT INTO _charquest (charid, questid, status, achievementcount, starttime, endtime, questdata1, questdata2) "
                     "SELECT $1, q.id, 1, 0, localtimestamp, localtimestamp, 0, 0 FROM _refquest q WHERE q.codename IN "
                     "(SELECT d.codename FROM _refchardefault_quest d WHERE " + race + " AND d.requiredlevel = 1 AND d.service = 1) ON CONFLICT DO NOTHING",
                     {id, std::int64_t(character.model)})) {
                return false;
            }
            // What it wears, each a new item in its place.
            const std::vector<std::int64_t> ids = AllocItems(tx, int(character.equipment.size()));
            if (ids.size() != character.equipment.size()) {
                error = "no item ids";
                return false;
            }
            for (std::size_t k = 0; k < ids.size(); ++k) {
                const CharacterRow::Item& worn = character.equipment[k];
                StoredItem item;
                item.refId = worn.item;
                item.plus = worn.plus;
                item.durability = worn.durability;
                item.quantity = worn.quantity;
                item.counted = worn.durability == 0; // ammunition
                if (!WriteItem(tx, ids[k], item, error) ||
                    !run("UPDATE _inventory SET itemid = $3 WHERE charid = $1 AND slot = $2", {id, int(worn.slot), ids[k]})) {
                    return false;
                }
            }
            // The original's gifts to every new character: the 60 % and the 100 % experience helps and 20 beginner's return
            // scrolls, in the bag's first places from 13 (when the reference table has them in service: an import's).
            QueryResult reference = tx.Query("SELECT count(*) = 2 FROM pg_attribute WHERE attrelid = to_regclass('_refobjcommon') "
                                             "AND attname IN ('codename128', 'service') AND NOT attisdropped");
            if (!reference.Ok()) {
                error = reference.Error();
                return false;
            }
            if (reference.Bool(0, 0)) {
                static constexpr std::pair<std::string_view, std::uint16_t> kGifts[] = {
                    {"ITEM_ETC_E060118_60EXP_HELP", 1}, {"ITEM_ETC_E060118_100EXP_HELP", 1}, {"ITEM_ETC_SCROLL_RETURN_NEWBIE_01", 20}};
                int place = 13;
                for (const auto& [codename, count] : kGifts) {
                    QueryResult ref = tx.Query("SELECT id FROM _refobjcommon WHERE codename128 = $1 AND service = 1", {std::string(codename)});
                    if (!ref.Ok()) {
                        error = ref.Error();
                        return false;
                    }
                    if (ref.Rows() == 0) continue;
                    const std::vector<std::int64_t> gift = AllocItems(tx, 1);
                    if (gift.size() != 1) {
                        error = "no item ids";
                        return false;
                    }
                    StoredItem item;
                    item.refId = std::uint32_t(ref.Int(0, 0));
                    item.quantity = count;
                    item.counted = true;
                    if (!WriteItem(tx, gift[0], item, error) || !run("UPDATE _inventory SET itemid = $3 WHERE charid = $1 AND slot = $2", {id, place, gift[0]})) {
                        return false;
                    }
                    ++place;
                }
            }
            result = CreateCharacterResult::Created;
            return true;
        },
        error);
    return result;
}

bool CopyCharacter(Database& db, std::int64_t account, std::string_view from, std::string_view to, std::string& error) {
    return db.Transaction(
        [&](Database& tx) {
            const auto run = [&](std::string_view sql, std::vector<Param> params) {
                QueryResult q = tx.Query(sql, std::move(params));
                if (!q.Ok()) error = q.Error();
                return q.Ok();
            };
            // A table's columns but those named (and its identity, made by the database), as "a, b, c".
            const auto columns = [&](std::string_view table, std::initializer_list<std::string_view> without, std::string& out) {
                QueryResult q = tx.Query("SELECT attname FROM pg_attribute WHERE attrelid = $1::regclass AND attnum > 0 AND NOT attisdropped "
                                         "AND attidentity = '' ORDER BY attnum",
                                         {std::string(table)});
                if (!q.Ok()) {
                    error = q.Error();
                    return false;
                }
                out.clear();
                for (int r = 0; r < q.Rows(); ++r) {
                    const std::string name = q.Text(r, 0);
                    if (std::find(without.begin(), without.end(), name) != without.end()) continue;
                    out += (out.empty() ? "" : ", ") + name;
                }
                return true;
            };
            QueryResult src = tx.Query("SELECT charid FROM _char WHERE lower(charname16) = $1 AND deleted = 0", {Lower(from)});
            if (!src.Ok() || src.Rows() == 0) {
                error = src.Ok() ? std::format("no character {}", from) : src.Error();
                return false;
            }
            const std::int64_t old = src.Int(0, 0);
            QueryResult taken = tx.Query("SELECT 1 FROM _char WHERE lower(charname16) = $1", {Lower(to)});
            if (!taken.Ok() || taken.Rows() != 0) {
                error = taken.Ok() ? std::format("the name {} is taken", to) : taken.Error();
                return false;
            }
            // Its row: the name its own, no alias, no guild.
            std::string list;
            if (!columns("_char", {"charname16", "nickname16", "guildid"}, list)) return false;
            QueryResult made = tx.Query(std::format("INSERT INTO _char (charname16, nickname16, guildid, {0}) SELECT $2, '', 0, {0} FROM _char "
                                                    "WHERE charid = $1 RETURNING charid",
                                                    list),
                                        {old, std::string(to)});
            if (!made.Ok() || made.Rows() == 0) {
                error = made.Ok() ? "no row made" : made.Error();
                return false;
            }
            const std::int64_t id = made.Int(0, 0);
            if (!run("INSERT INTO _user (userjid, charid) VALUES ($1, $2)", {account, id}) ||
                !run("INSERT INTO _charnamelist (charname16, charid) VALUES (left($1, 17), $2)", {std::string(to), id})) {
                return false;
            }
            // Its rows by character, the character's id its own (the places' items put in below).
            for (const char* table : {"_charskill", "_charskillmastery", "_charquest", "_clientconfig", "_staticavatar", "_chartrijob", "sr_char_state",
                                      "sr_auto_potion", "_charcollectionbook", "_timedjob", "_inventory", "_inventoryforavatar"}) {
                if (!columns(table, {"charid"}, list) ||
                    !run(std::format("INSERT INTO {0} (charid, {1}) SELECT $2, {1} FROM {0} WHERE charid = $1", table, list), {old, id})) {
                    return false;
                }
            }
            if (!run("UPDATE _inventory SET itemid = 0 WHERE charid = $1", {id}) || !run("UPDATE _inventoryforavatar SET itemid = 0 WHERE charid = $1", {id}) ||
                !run("UPDATE _timedjob SET jid = $2 WHERE charid = $1", {id, account})) {
                return false;
            }
            // An item as a new one: its row, its bindings, a cube's contents, a pet's COS (new: theirs alone).
            std::string itemColumns, cosColumns, petJobColumns;
            if (!columns("_items", {"serial64"}, itemColumns) || !columns("_charcos", {"ownercharid"}, cosColumns) ||
                !columns("_timedjobforpet", {"charid"}, petJobColumns)) {
                return false;
            }
            std::string sets;
            for (std::size_t start = 0; start < itemColumns.size();) {
                const std::size_t comma = std::min(itemColumns.find(", ", start), itemColumns.size());
                const std::string name = itemColumns.substr(start, comma - start);
                sets += (sets.empty() ? "" : ", ") + name + " = s." + name;
                start = comma + 2;
            }
            int items = 0;
            std::function<std::int64_t(std::int64_t)> copyItem = [&](std::int64_t item) -> std::int64_t {
                const std::vector<std::int64_t> fresh = AllocItems(tx, 1);
                if (fresh.size() != 1) {
                    error = "no item id";
                    return 0;
                }
                const std::int64_t twin = fresh[0];
                ++items;
                if (!run(std::format("UPDATE _items n SET {} FROM _items s WHERE s.id64 = $1 AND n.id64 = $2", sets), {item, twin}) ||
                    !run("INSERT INTO _bindingoptionwithitem (nitemdbid, bopttype, nslot, noptid, noptlvl, noptvalue, nparam1, nparam2) "
                         "SELECT $2, bopttype, nslot, noptid, noptlvl, noptvalue, nparam1, nparam2 FROM _bindingoptionwithitem WHERE nitemdbid = $1",
                         {item, twin})) {
                    return 0;
                }
                QueryResult inside = tx.Query("SELECT slot, itemid FROM _inventoryforlinkedstorage WHERE linkeditemid = $1 AND itemid <> 0", {item});
                if (!inside.Ok()) {
                    error = inside.Error();
                    return 0;
                }
                for (int r = 0; r < inside.Rows(); ++r) {
                    const std::int64_t child = copyItem(inside.Int(r, 1));
                    if (child == 0 || !run("INSERT INTO _inventoryforlinkedstorage (linkeditemid, slot, itemid) VALUES ($1, $2, $3)",
                                           {twin, inside.Int(r, 0), child})) {
                        return 0;
                    }
                }
                // A pet's item (TID 3/2/1, ITEM_COS_P_*): its data the COS row (others' data is their count).
                QueryResult pet = tx.Query("SELECT c.id FROM _items i JOIN _refobjcommon r ON r.id = i.refitemid AND r.typeid1 = 3 AND r.typeid2 = 2 "
                                           "AND r.typeid3 = 1 JOIN _charcos c ON c.id = i.data AND c.ownercharid = $2 WHERE i.id64 = $1 AND i.data > 0",
                                           {item, old});
                if (!pet.Ok()) {
                    error = pet.Error();
                    return 0;
                }
                if (pet.Rows() > 0) {
                    const std::int64_t cos = pet.Int(0, 0);
                    QueryResult copy = tx.Query(std::format("INSERT INTO _charcos (ownercharid, {0}) SELECT $2, {0} FROM _charcos WHERE id = $1 RETURNING id",
                                                            cosColumns),
                                                {cos, id});
                    if (!copy.Ok() || copy.Rows() == 0) {
                        error = copy.Ok() ? "no COS made" : copy.Error();
                        return 0;
                    }
                    const std::int64_t cosTwin = copy.Int(0, 0);
                    if (!run(std::format("INSERT INTO _timedjobforpet (charid, {0}) SELECT $2, {0} FROM _timedjobforpet WHERE charid = $1", petJobColumns),
                             {cos, cosTwin}) ||
                        !run("UPDATE _items SET data = $2 WHERE id64 = $1", {twin, cosTwin})) {
                        return 0;
                    }
                }
                return twin;
            };
            for (const char* table : {"_inventory", "_inventoryforavatar"}) {
                QueryResult held = tx.Query(std::format("SELECT slot, itemid FROM {} WHERE charid = $1 AND itemid <> 0", table), {old});
                if (!held.Ok()) {
                    error = held.Error();
                    return false;
                }
                for (int r = 0; r < held.Rows(); ++r) {
                    const std::int64_t twin = copyItem(held.Int(r, 1));
                    if (twin == 0 || !run(std::format("UPDATE {} SET itemid = $3 WHERE charid = $1 AND slot = $2", table), {id, held.Int(r, 0), twin})) {
                        return false;
                    }
                }
            }
            core::LogInfo("store: {} copied as {} (character {}, {} items)", from, to, id, items);
            return true;
        },
        error);
}

bool CharacterNameFree(Database& db, std::uint16_t, std::string_view name, bool& ok) {
    QueryResult r = db.Query("SELECT 1 FROM _char WHERE lower(charname16) = $1", {Lower(name)});
    ok = r.Ok();
    return ok && r.Rows() == 0;
}

namespace {

// The state of the account's character `name`: NotFound (also once its deletion's wait has
// passed), Deleting, or Done (normal).
CharacterChange CharacterState(Database& db, std::int64_t account, std::string_view name, std::int64_t delaySeconds, std::int64_t& id) {
    QueryResult r = db.Query(
        "SELECT c.charid, c.deleted <> 0, coalesce(d.deleteddate + make_interval(secs => $3::bigint) <= localtimestamp, false) "
        "FROM _char c JOIN _user u ON u.charid = c.charid LEFT JOIN _deletedchar d ON d.charid = c.charid "
        "WHERE u.userjid = $1 AND lower(c.charname16) = $2",
        {account, Lower(name), delaySeconds});
    if (!r.Ok()) return CharacterChange::Failed;
    if (r.Rows() == 0 || r.Bool(0, 2)) return CharacterChange::NotFound;
    id = r.Int(0, 0);
    return r.Bool(0, 1) ? CharacterChange::Deleting : CharacterChange::Done;
}

constexpr std::int64_t kForever = std::int64_t(100) * 365 * 24 * 3600;

} // namespace

CharacterChange DeleteCharacter(Database& db, std::int64_t account, std::uint16_t, std::string_view name, int) {
    std::int64_t id = 0;
    const CharacterChange state = CharacterState(db, account, name, kForever, id);
    if (state != CharacterChange::Done) return state;
    // A guild's master is not deleted while it leads its guild (the original refuses it: it leaves, or hands the guild
    // on, first).
    const QueryResult master = db.Query("SELECT 1 FROM _guildmember WHERE charid = $1 AND memberclass = 0", {id});
    if (!master.Ok()) return CharacterChange::Failed;
    if (master.Rows() > 0) return CharacterChange::GuildMaster;
    // As the original's: the request's time, and the character marked.
    std::string error;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            return tx.Query("INSERT INTO _deletedchar (charid, userjid, deleteddate) VALUES ($1, $2, localtimestamp) "
                            "ON CONFLICT (charid) DO UPDATE SET deleteddate = EXCLUDED.deleteddate",
                            {id, account})
                       .Ok() &&
                   tx.Query("UPDATE _char SET deleted = 1 WHERE charid = $1", {id}).Ok();
        },
        error);
    return ok ? CharacterChange::Done : CharacterChange::Failed;
}

CharacterChange RestoreCharacter(Database& db, std::int64_t account, std::uint16_t, std::string_view name, int delaySeconds) {
    std::int64_t id = 0;
    const CharacterChange state = CharacterState(db, account, name, delaySeconds, id);
    if (state == CharacterChange::Done) return CharacterChange::NotDeleting;
    if (state != CharacterChange::Deleting) return state;
    std::string error;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            return tx.Query("DELETE FROM _deletedchar WHERE charid = $1", {id}).Ok() && tx.Query("UPDATE _char SET deleted = 0 WHERE charid = $1", {id}).Ok();
        },
        error);
    return ok ? CharacterChange::Done : CharacterChange::Failed;
}

CharacterChange SelectCharacter(Database& db, std::int64_t account, std::uint16_t, std::string_view name, std::int64_t& id) {
    return CharacterState(db, account, name, kForever, id);
}

std::optional<CharacterRow> LoadCharacter(Database& db, std::int64_t account, std::uint16_t, std::int64_t character, bool& ok) {
    QueryResult r = db.Query(
        "SELECT c.charid, c.charname16, c.refobjid, c.scale, c.curlevel, c.expoffset, c.strength, c.intellect, c.remainstatpoint, c.hp, c.mp, "
        "       c.latestregion, c.posx, c.posy, c.posz, coalesce(s.yaw, 0), s.entered, c.remaingold, c.inventorysize, c.appointedteleport, "
        "       c.remainskillpoint, c.sexpoffset, c.remainhwancount, c.hwanlevel, coalesce(c.autoinvestexp, 0), c.maxlevel, "
        "       c.telregion, c.telposx, c.telposy, c.telposz, c.diedregion, c.diedposx, c.diedposy, c.diedposz, "
        "       c.dailypk, c.totalpk, c.pkpenaltypoint "
        "FROM _char c JOIN _user u ON u.charid = c.charid AND u.userjid = $2 LEFT JOIN sr_char_state s ON s.charid = c.charid "
        "WHERE c.charid = $1 AND c.deleted = 0",
        {character, account});
    QueryResult items = db.Query(std::format("SELECT it.id64, i.slot, {} FROM _inventory i JOIN _items it ON it.id64 = i.itemid "
                                             "WHERE i.charid = $1 AND i.itemid <> 0 ORDER BY i.slot",
                                             ItemSelect("it")),
                                 {character});
    QueryResult avatars = db.Query(std::format("SELECT it.id64, a.slot, {} FROM _inventoryforavatar a JOIN _items it ON it.id64 = a.itemid "
                                               "WHERE a.charid = $1 AND a.itemid <> 0 ORDER BY a.slot",
                                               ItemSelect("it")),
                                   {character});
    QueryResult slots = db.Query("SELECT slotseq, slottype, data FROM _clientconfig WHERE charid = $1 AND configtype = 1 AND slottype <> 0 ORDER BY slotseq",
                                 {character});
    QueryResult potion = db.Query("SELECT configtype, data FROM _clientconfig WHERE charid = $1 AND configtype BETWEEN 11 AND 14 AND slotseq = 0",
                                  {character});
    QueryResult potionItems = db.Query("SELECT configtype, refobjid, setting FROM sr_auto_potion WHERE charid = $1", {character});
    QueryResult longCooldowns = db.Query("SELECT data1, data2 FROM _timedjob WHERE charid = $1 AND category = 6 AND jobid = 200", {character});
    // Its buffs as the original keeps them: _timedjob category 0, the skill and its time left (seconds).
    QueryResult buffs = db.Query("SELECT jobid, timetokeep::bigint * 1000 FROM _timedjob WHERE charid = $1 AND category = 0 AND timetokeep > 0", {character});
    QueryResult masteries = db.Query("SELECT masteryid, level FROM _charskillmastery WHERE charid = $1 AND level > 0", {character});
    QueryResult skills = db.Query("SELECT skillid FROM _charskill WHERE charid = $1 AND enable <> 0", {character});
    QueryResult skin = db.Query("SELECT coalesce(param1, 0) FROM _staticavatar WHERE charid = $1", {character});
    QueryResult keeper = db.Query("SELECT timetokeep, coalesce(data1, 0) FROM _timedjob WHERE charid = $1 AND category = 2 AND jobid = 0 ORDER BY id DESC LIMIT 1",
                                  {character});
    QueryResult quests = db.Query(
        "SELECT questid, status, achievementcount, extract(epoch FROM starttime)::bigint, extract(epoch FROM endtime)::bigint, questdata1, questdata2 "
        "FROM _charquest WHERE charid = $1",
        {character});
    QueryResult deeds = db.Query("SELECT name, count FROM sr_char_records WHERE charid = $1", {character});
    ok = r.Ok() && items.Ok() && avatars.Ok() && slots.Ok() && longCooldowns.Ok() && buffs.Ok() && masteries.Ok() && skills.Ok() && potion.Ok() && potionItems.Ok() &&
         skin.Ok() && quests.Ok() && keeper.Ok() && deeds.Ok();
    if (!ok || r.Rows() == 0) return std::nullopt;
    CharacterRow c;
    c.id = r.Int(0, 0);
    c.name = r.Text(0, 1);
    c.model = std::uint32_t(r.Int(0, 2));
    c.height = std::uint8_t(r.Int(0, 3));
    c.level = std::uint8_t(r.Int(0, 4));
    c.experience = std::uint64_t(r.Int(0, 5));
    c.strength = std::uint16_t(r.Int(0, 6));
    c.intelligence = std::uint16_t(r.Int(0, 7));
    c.statPoints = std::uint16_t(r.Int(0, 8));
    c.hp = std::uint32_t(r.Int(0, 9));
    c.mp = std::uint32_t(r.Int(0, 10));
    // Its place: once it entered the world (ours), or wherever an imported character's _char says.
    const bool entered = r.IsNull(0, 16) ? r.Int(0, 11) != 0 : r.Bool(0, 16);
    if (entered) {
        c.place = CharacterRow::Place{std::uint16_t(r.Int(0, 11)), float(r.Double(0, 12)), float(r.Double(0, 13)), float(r.Double(0, 14)),
                                      float(r.Double(0, 15))};
    }
    c.gold = std::uint64_t(r.Int(0, 17));
    c.inventorySize = std::uint8_t(std::clamp<std::int64_t>(r.Int(0, 18), 0, 255));
    c.returnGate = std::uint32_t(r.Int(0, 19));
    c.skillPoints = std::uint32_t(r.Int(0, 20));
    c.skillExperience = std::uint32_t(r.Int(0, 21));
    c.berserkPoints = std::uint8_t(std::clamp<long long>(r.Int(0, 22), 0, 5));
    c.hwanLevel = int(r.Int(0, 23));
    c.marks = std::uint8_t(r.Int(0, 24));
    c.maxLevel = std::uint8_t(std::max<long long>(r.Int(0, 25), c.level));
    // The regions are the original's signed 16-bit numbers (a dungeon's negative).
    c.recallSpot = {std::uint16_t(std::int16_t(r.Int(0, 26))), float(r.Double(0, 27)), float(r.Double(0, 28)), float(r.Double(0, 29))};
    c.deathSpot = {std::uint16_t(std::int16_t(r.Int(0, 30))), float(r.Double(0, 31)), float(r.Double(0, 32)), float(r.Double(0, 33))};
    c.stallSkin = skin.Rows() > 0 ? std::uint32_t(skin.Int(0, 0)) : 0;
    c.dailyPk = std::uint8_t(std::clamp<long long>(r.Int(0, 34), 0, 15));
    c.totalPk = std::uint16_t(std::clamp<long long>(r.Int(0, 35), 0, 15));
    c.pkPoints = std::int32_t(std::clamp<long long>(r.Int(0, 36), 0, 200'000));
    // Its academy's honour points, when the database has the academies' table (an import; 0 otherwise).
    if (QueryResult t = db.Query("SELECT to_regclass('_trainingcampmember') IS NOT NULL"); t.Ok() && t.Rows() > 0 && t.Bool(0, 0)) {
        if (QueryResult h = db.Query("SELECT coalesce(max(honorpoint), 0) FROM _trainingcampmember WHERE charid = $1", {character}); h.Ok() && h.Rows() > 0) {
            c.honorPoints = std::max<std::int64_t>(0, h.Int(0, 0));
        }
    }
    if (keeper.Rows() > 0) c.pkKeeper = CharacterRow::PkKeeper{keeper.Int(0, 0), keeper.Int(0, 1)};
    for (int k = 0; k < buffs.Rows(); ++k) c.buffs.push_back({std::uint32_t(buffs.Int(k, 0)), std::uint64_t(buffs.Int(k, 1))});
    for (int k = 0; k < longCooldowns.Rows(); ++k) c.longCooldowns.push_back({std::uint32_t(longCooldowns.Int(k, 0)), longCooldowns.Int(k, 1)});
    for (int k = 0; k < masteries.Rows(); ++k) c.masteries.push_back({std::uint32_t(masteries.Int(k, 0)), int(masteries.Int(k, 1))});
    for (int k = 0; k < deeds.Rows(); ++k) c.deeds[std::string(deeds.View(k, 0))] = int(deeds.Int(k, 1));
    for (int k = 0; k < quests.Rows(); ++k) {
        c.quests.push_back({std::uint32_t(quests.Int(k, 0)), std::uint8_t(quests.Int(k, 1)), std::uint16_t(quests.Int(k, 2)), quests.Int(k, 3),
                            quests.Int(k, 4), quests.Int(k, 5), quests.Int(k, 6)});
    }
    for (int k = 0; k < skills.Rows(); ++k) c.skills.push_back(std::uint32_t(skills.Int(k, 0)));
    for (int k = 0; k < items.Rows(); ++k) {
        StoredItem item;
        item.id = items.Int(k, 0);
        item.slot = std::uint8_t(items.Int(k, 1));
        ReadItem(items, k, 2, item);
        if (item.slot < 13) c.equipment.push_back({EquipSlot(item.slot), item.refId, item.plus});
        c.items.push_back(std::move(item));
    }
    for (int k = 0; k < avatars.Rows(); ++k) {
        StoredItem item;
        item.id = avatars.Int(k, 0);
        item.slot = std::uint8_t(avatars.Int(k, 1));
        ReadItem(avatars, k, 2, item);
        c.avatars.push_back(std::move(item));
    }
    for (int k = 0; k < slots.Rows(); ++k) {
        c.quickSlots.push_back({std::uint8_t(slots.Int(k, 0)), std::uint8_t(slots.Int(k, 1)), std::uint32_t(slots.Int(k, 2))});
    }
    // Its auto potion: the words its rows have. All 0 (as a new character's rows start) is the
    // original's never-set state: the defaults then (the 1.188 client keeps its own defaults until
    // the window's Confirm sends words).
    bool potionSet = false;
    for (int k = 0; k < potion.Rows(); ++k) potionSet = potionSet || potion.Int(k, 1) != 0;
    if (potionSet) {
        for (int k = 0; k < potion.Rows(); ++k) {
            const int type = int(potion.Int(k, 0));
            const std::uint32_t data = std::uint32_t(potion.Int(k, 1));
            if (type >= 11 && type <= 13) {
                c.autoPotion.rows[std::size_t(type - 11)] = game::DecodeAutoPotionWord(std::uint16_t(data & 0xFFFF));
            } else if (type == 14) {
                bool on = false;
                int ms = 0;
                game::DecodeAutoPotionDelay(std::uint8_t(data & 0xFF), on, ms);
                c.autoPotion.delayEnabled = on;
                c.autoPotion.delayMs = std::uint16_t(ms);
            }
        }
    }
    for (int k = 0; k < potionItems.Rows(); ++k) {
        const int type = int(potionItems.Int(k, 0));
        if (type >= 11 && type <= 13) {
            c.autoPotion.rows[std::size_t(type - 11)].item = std::uint32_t(potionItems.Int(k, 1));
        } else if (type == 101 || type == 102) { // ours: the vehicle's and the attack pet's rows, whole
            game::AutoPotion::Entry& e = c.autoPotion.rows[std::size_t(game::AutoPotion::kVehicle + type - 101)];
            e = game::DecodeAutoPotionWord(std::uint16_t(potionItems.Int(k, 2) & 0xFFFF));
            e.item = std::uint32_t(potionItems.Int(k, 1));
        }
    }
    c.autoPotion.Clamp();
    std::string error;
    if (!LoadCollectionBook(db, character, c.book, error) || !LoadInstanceEntries(db, character, c.instanceEntries, error) ||
        !LoadItemWaits(db, character, c.itemWaits, error) || !LoadJob(db, character, c.job, c.alias, c.jobLeftAt, error) ||
        !LoadSafeTrade(db, character, c.safeTrade, error) || !LoadBargainLock(db, character, c.bargainUntil, error)) {
        ok = false;
        return std::nullopt;
    }
    return c;
}

bool SavePlaces(Database& db, const std::vector<CharacterPlace>& places) {
    if (places.empty()) return true;
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            for (const CharacterPlace& p : places) {
                if (!tx.Query("UPDATE _char SET latestregion = $2, posx = $3, posy = $4, posz = $5 WHERE charid = $1",
                              {p.character, int(std::int16_t(p.place.region)), double(p.place.x), double(p.place.y), double(p.place.z)})
                         .Ok() ||
                    !tx.Query("INSERT INTO sr_char_state (charid, yaw, entered) VALUES ($1, $2, true) "
                              "ON CONFLICT (charid) DO UPDATE SET yaw = EXCLUDED.yaw, entered = true",
                              {p.character, double(p.place.yaw)})
                         .Ok()) {
                    return false;
                }
            }
            return true;
        },
        error);
}

bool SaveItems(Database& db, std::int64_t character, const std::vector<StoredItem>& present, const std::vector<std::uint8_t>& emptied,
               const std::vector<std::int64_t>& gone, const AvatarPlaces& avatars) {
    std::string error;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            std::unordered_set<std::int64_t> kept;
            for (const StoredItem& i : present) kept.insert(i.id);
            for (const StoredItem& i : avatars.present) kept.insert(i.id);
            // Another character's, a storage's or a pet bag's item is never touched from here.
            const auto foreign = [&](std::int64_t id) {
                QueryResult q = tx.Query("SELECT 1 FROM _inventory WHERE itemid = $1 AND charid <> $2 UNION ALL SELECT 1 FROM _chest WHERE itemid = $1 "
                                         "UNION ALL SELECT 1 FROM _guildchest WHERE itemid = $1 "
                                         "UNION ALL SELECT 1 FROM _inventoryforavatar WHERE itemid = $1 AND charid <> $2 "
                                         "UNION ALL SELECT 1 FROM _invcos WHERE itemid = $1 UNION ALL SELECT 1 FROM _inventoryforlinkedstorage WHERE itemid = $1 "
                                         "UNION ALL SELECT 1 FROM _openmarket WHERE itemid = $1", // held by the Open Market
                                         {id, character});
                return !q.Ok() || q.Rows() > 0;
            };
            // The avatar places emptied first (what left them for the bag is kept).
            for (std::uint8_t slot : avatars.emptied) {
                QueryResult was = tx.Query("SELECT itemid FROM _inventoryforavatar WHERE charid = $1 AND slot = $2 FOR UPDATE", {character, int(slot)});
                if (!was.Ok()) return false;
                if (was.Rows() == 0) continue;
                const std::int64_t id = was.Int(0, 0);
                if (!tx.Query("UPDATE _inventoryforavatar SET itemid = 0 WHERE charid = $1 AND slot = $2", {character, int(slot)}).Ok()) return false;
                if (id != 0 && !kept.contains(id) && !foreign(id) && !FreeItem(tx, id)) return false;
            }
            for (std::int64_t id : gone) {
                if (id == 0 || kept.contains(id) || foreign(id)) continue;
                if (!tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND itemid = $2", {character, id}).Ok() || !FreeItem(tx, id)) return false;
            }
            for (std::uint8_t slot : emptied) {
                QueryResult was = tx.Query("SELECT itemid FROM _inventory WHERE charid = $1 AND slot = $2 FOR UPDATE", {character, int(slot)});
                if (!was.Ok()) return false;
                if (was.Rows() == 0) continue;
                const std::int64_t id = was.Int(0, 0);
                if (!tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND slot = $2", {character, int(slot)}).Ok()) return false;
                // What left the inventory (dropped, sold, stored) is freed; what moved within it is kept.
                if (id != 0 && !kept.contains(id) && !foreign(id) && !FreeItem(tx, id)) return false;
            }
            for (const StoredItem& i : present) {
                if (i.id == 0) continue;
                if (foreign(i.id)) {
                    core::LogWarning("items of character {}: item {} belongs elsewhere; not saved", character, i.id);
                    continue;
                }
                if (!WriteItem(tx, i.id, i, error)) return false;
                // In its place and nowhere else of this character's (an avatar taken off leaves its avatar place).
                if (!tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND itemid = $2 AND slot <> $3", {character, i.id, int(i.slot)}).Ok() ||
                    !tx.Query("UPDATE _inventoryforavatar SET itemid = 0 WHERE charid = $1 AND itemid = $2", {character, i.id}).Ok() ||
                    !tx.Query("INSERT INTO _inventory (charid, slot, itemid) VALUES ($1, $2, $3) ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                              {character, int(i.slot), i.id})
                         .Ok()) {
                    return false;
                }
            }
            // The avatars put on: in their avatar places, out of the bag.
            for (const StoredItem& i : avatars.present) {
                if (i.id == 0) continue;
                if (foreign(i.id)) {
                    core::LogWarning("avatars of character {}: item {} belongs elsewhere; not saved", character, i.id);
                    continue;
                }
                if (!WriteItem(tx, i.id, i, error)) return false;
                if (!tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND itemid = $2", {character, i.id}).Ok() ||
                    !tx.Query("UPDATE _inventoryforavatar SET itemid = 0 WHERE charid = $1 AND itemid = $2 AND slot <> $3", {character, i.id, int(i.slot)})
                         .Ok() ||
                    !tx.Query("INSERT INTO _inventoryforavatar (charid, slot, itemid) VALUES ($1, $2, $3) "
                              "ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                              {character, int(i.slot), i.id})
                         .Ok()) {
                    return false;
                }
            }
            return true;
        },
        error);
    if (!ok) core::LogWarning("items of character {}: {}", character, error);
    return ok;
}

bool SaveFarSale(Database& db, std::int64_t owner, std::uint8_t ownerPlace, std::uint64_t ownerGold, std::int64_t buyer, std::uint8_t buyerPlace,
                 std::int64_t serial, std::int64_t deadlineMs) {
    std::string error;
    bool late = false;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            // The buyer's row first: a settling read (FarSaleLanded) waits until this is done.
            if (QueryResult lock = tx.Query("SELECT 1 FROM _char WHERE charid = $1 FOR UPDATE", {buyer}); !lock.Ok() || lock.Rows() == 0) return false;
            QueryResult clock = tx.Query("SELECT (extract(epoch FROM clock_timestamp()) * 1000)::bigint < $1", {deadlineMs});
            if (!clock.Ok() || clock.Rows() == 0 || clock.Text(0, 0) != "t") {
                late = true;
                return false;
            }
            QueryResult was = tx.Query("SELECT itemid FROM _inventory WHERE charid = $1 AND slot = $2 FOR UPDATE", {owner, int(ownerPlace)});
            if (!was.Ok() || was.Rows() == 0 || was.Int(0, 0) != serial) return false;
            QueryResult free = tx.Query("SELECT itemid FROM _inventory WHERE charid = $1 AND slot = $2 FOR UPDATE", {buyer, int(buyerPlace)});
            if (!free.Ok() || (free.Rows() > 0 && free.Int(0, 0) != 0)) return false;
            return tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND slot = $2", {owner, int(ownerPlace)}).Ok() &&
                   tx.Query("INSERT INTO _inventory (charid, slot, itemid) VALUES ($1, $2, $3) ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                            {buyer, int(buyerPlace), serial})
                       .Ok() &&
                   tx.Query("UPDATE _char SET remaingold = $2 WHERE charid = $1", {owner, std::int64_t(ownerGold)}).Ok();
        },
        error);
    if (!ok && !late) core::LogWarning("stall sale of item {} from character {} to {}: {}", serial, owner, buyer, error.empty() ? "refused" : error);
    return ok;
}

std::optional<bool> FarSaleLanded(Database& db, std::int64_t buyer, std::uint8_t place, std::int64_t serial, std::int64_t deadlineMs) {
    std::optional<bool> out;
    std::string error;
    db.Transaction(
        [&](Database& tx) {
            if (QueryResult lock = tx.Query("SELECT 1 FROM _char WHERE charid = $1 FOR UPDATE", {buyer}); !lock.Ok()) return false;
            QueryResult clock = tx.Query("SELECT (extract(epoch FROM clock_timestamp()) * 1000)::bigint >= $1", {deadlineMs});
            if (!clock.Ok() || clock.Rows() == 0 || clock.Text(0, 0) != "t") return false;
            QueryResult at = tx.Query("SELECT itemid FROM _inventory WHERE charid = $1 AND slot = $2", {buyer, int(place)});
            if (!at.Ok()) return false;
            out = at.Rows() > 0 && at.Int(0, 0) == serial;
            return true;
        },
        error);
    return out;
}

bool AddGold(Database& db, std::int64_t character, std::uint64_t gold) {
    return db.Query("UPDATE _char SET remaingold = remaingold + $2 WHERE charid = $1", {character, std::int64_t(gold)}).Ok();
}

// --- The Open Market ----------------------------------------------------------------------------

namespace {

// A listing's columns (`om`) and, when held, its item's (`it`, LEFT JOINed): what OpenMarketRecordAt reads.
std::string OpenMarketColumns() {
    return "om.jid, om.persnalid, om.charname16, om.status, om.refitemid, om.tidgroupid, om.itemclass, om.sellcnt, om.price, om.deposit, om.sellfee, "
           "greatest(0, extract(epoch FROM om.enddate - localtimestamp))::bigint, om.itemid, " +
           ItemSelect("it");
}
OpenMarketRecord OpenMarketRecordAt(const QueryResult& r, int k) {
    OpenMarketRecord o;
    o.account = r.Int(k, 0);
    o.listing = std::uint32_t(r.Int(k, 1));
    o.seller = r.Text(k, 2);
    o.status = std::uint8_t(r.Int(k, 3));
    o.refId = std::uint32_t(r.Int(k, 4));
    o.group = std::uint32_t(r.Int(k, 5));
    o.degree = std::uint8_t(std::clamp<std::int64_t>(r.Int(k, 6), 0, 255));
    o.count = std::uint32_t(std::max<std::int64_t>(0, r.Int(k, 7)));
    o.price = std::uint64_t(std::max<std::int64_t>(0, r.Int(k, 8)));
    o.deposit = std::uint64_t(std::max<std::int64_t>(0, r.Int(k, 9)));
    o.saleFee = std::uint64_t(std::max<std::int64_t>(0, r.Int(k, 10)));
    o.secondsLeft = r.Int(k, 11);
    o.item.id = r.Int(k, 12);
    o.held = o.item.id != 0 && !r.IsNull(k, 13);
    if (o.held) ReadItem(r, k, 13, o.item);
    return o;
}
// The row of a listing, locked, with its item.
std::optional<OpenMarketRecord> LockOpenMarket(Database& tx, std::int64_t account, std::uint32_t listing) {
    QueryResult lock = tx.Query("SELECT 1 FROM _openmarket WHERE jid = $1 AND persnalid = $2 FOR UPDATE", {account, std::int64_t(listing)});
    if (!lock.Ok() || lock.Rows() == 0) return std::nullopt;
    QueryResult r = tx.Query(std::format("SELECT {}, (om.enddate <= localtimestamp) FROM _openmarket om LEFT JOIN _items it ON it.id64 = om.itemid AND om.itemid <> 0 "
                                         "WHERE om.jid = $1 AND om.persnalid = $2",
                                         OpenMarketColumns()),
                             {account, std::int64_t(listing)});
    if (!r.Ok() || r.Rows() == 0) return std::nullopt;
    OpenMarketRecord o = OpenMarketRecordAt(r, 0);
    if (r.Bool(0, r.Columns() - 1)) o.secondsLeft = 0;
    return o;
}
// A bag place free in the database, then the item put there.
bool PutInBag(Database& tx, std::int64_t character, std::uint8_t place, std::int64_t item) {
    QueryResult free = tx.Query("SELECT itemid FROM _inventory WHERE charid = $1 AND slot = $2 FOR UPDATE", {character, int(place)});
    if (!free.Ok() || (free.Rows() > 0 && free.Int(0, 0) != 0)) return false;
    return tx.Query("INSERT INTO _inventory (charid, slot, itemid) VALUES ($1, $2, $3) ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                    {character, int(place), item})
        .Ok();
}

} // namespace

std::optional<std::uint32_t> PutOpenMarket(Database& db, const OpenMarketPut& put) {
    std::optional<std::uint32_t> made;
    std::string error;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            QueryResult was = tx.Query("SELECT itemid FROM _inventory WHERE charid = $1 AND slot = $2 FOR UPDATE", {put.character, int(put.place)});
            if (!was.Ok() || was.Rows() == 0 || was.Int(0, 0) != put.item) return false;
            // The account's next number (the procedure's MAX + 1), its rows locked.
            QueryResult next = tx.Query("SELECT coalesce(max(persnalid) + 1, 0) FROM _openmarket WHERE jid = $1", {put.account});
            if (!next.Ok() || next.Rows() == 0) return false;
            const std::int64_t listing = next.Int(0, 0);
            if (!tx.Query("INSERT INTO _openmarket (jid, persnalid, charname16, status, refitemid, tidgroupid, itemclass, itemid, sellcnt, regdate, enddate, "
                          "price, deposit, sellfee, usecash, serial64) VALUES ($1, $2, $3, 0, $4, $5, $6, $7, $8, localtimestamp, "
                          "localtimestamp + make_interval(secs => $9::bigint), $10, $11, $12, $13, coalesce((SELECT serial64 FROM _items WHERE id64 = $7), 0))",
                          {put.account, listing, put.seller, std::int64_t(put.refId), std::int64_t(put.group), int(put.degree), put.item,
                           std::int64_t(put.count), put.seconds, std::int64_t(put.price), std::int64_t(put.deposit), std::int64_t(put.saleFee),
                           std::int64_t(put.ticket)})
                     .Ok() ||
                !tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND slot = $2", {put.character, int(put.place)}).Ok() ||
                !tx.Query("UPDATE _char SET remaingold = $2 WHERE charid = $1", {put.character, std::int64_t(put.gold)}).Ok()) {
                return false;
            }
            made = std::uint32_t(listing);
            return true;
        },
        error);
    if (!ok) {
        core::LogWarning("open market: listing item {} of character {}: {}", put.item, put.character, error.empty() ? "refused" : error);
        return std::nullopt;
    }
    return made;
}

std::vector<OpenMarketRecord> LoadOpenMarket(Database& db, std::int64_t account, bool& ok) {
    std::vector<OpenMarketRecord> out;
    QueryResult r = db.Query(std::format("SELECT {} FROM _openmarket om LEFT JOIN _items it ON it.id64 = om.itemid AND om.itemid <> 0 "
                                         "WHERE om.jid = $1 AND om.status <> 255 ORDER BY om.persnalid",
                                         OpenMarketColumns()),
                             {account});
    ok = r.Ok();
    for (int k = 0; ok && k < r.Rows(); ++k) out.push_back(OpenMarketRecordAt(r, k));
    return out;
}

std::vector<OpenMarketRecord> SearchOpenMarket(Database& db, std::uint32_t group, std::uint8_t degree, const std::string& seller, int page, int rows,
                                               int most, std::uint32_t& total) {
    std::vector<OpenMarketRecord> out;
    total = 0;
    // The original's TOP 500: on sale, not ended, the newest first.
    const std::string where = "om.tidgroupid = $1 AND ($2::int = 0 OR om.itemclass = $2) AND ($3::text = '' OR lower(om.charname16) = lower($3)) "
                              "AND om.status = 0 AND om.enddate > localtimestamp AND om.itemid <> 0";
    QueryResult count = db.Query("SELECT least(count(*), $4) FROM _openmarket om WHERE " + where, {std::int64_t(group), int(degree), seller, most});
    if (!count.Ok() || count.Rows() == 0) return out;
    total = std::uint32_t(count.Int(0, 0));
    const int offset = std::max(0, page) * rows;
    if (offset >= int(total)) return out;
    QueryResult r = db.Query(std::format("SELECT {} FROM _openmarket om LEFT JOIN _items it ON it.id64 = om.itemid WHERE {} "
                                         "ORDER BY om.regdate DESC, om.jid, om.persnalid LIMIT $4 OFFSET $5",
                                         OpenMarketColumns(), where),
                             {std::int64_t(group), int(degree), seller, std::min(rows, int(total) - offset), offset});
    for (int k = 0; r.Ok() && k < r.Rows(); ++k) out.push_back(OpenMarketRecordAt(r, k));
    return out;
}

OpenMarketTake BuyOpenMarket(Database& db, std::int64_t account, std::uint32_t listing, std::uint64_t price, std::int64_t buyer, std::uint8_t place,
                             std::uint64_t buyerGold, std::int64_t keepSeconds, OpenMarketRecord& out, std::int64_t& sellerCharacter) {
    OpenMarketTake result = OpenMarketTake::Failed;
    std::string error;
    db.Transaction(
        [&](Database& tx) {
            const auto row = LockOpenMarket(tx, account, listing);
            if (!row) return result = OpenMarketTake::NotListed, false;
            if (row->status != std::uint8_t(game::openmarket::Status::OnSale) || !row->held || row->price != price) return result = OpenMarketTake::NotOnSale, false;
            if (row->secondsLeft <= 0) return result = OpenMarketTake::Expired, false;
            if (!PutInBag(tx, buyer, place, row->item.id)) return result = OpenMarketTake::BagTaken, false;
            if (!tx.Query("UPDATE _openmarket SET status = 1, itemid = 0, enddate = localtimestamp + make_interval(secs => $3::bigint) "
                          "WHERE jid = $1 AND persnalid = $2",
                          {account, std::int64_t(listing), keepSeconds})
                     .Ok() ||
                !tx.Query("UPDATE _char SET remaingold = $2 WHERE charid = $1", {buyer, std::int64_t(buyerGold)}).Ok()) {
                return false;
            }
            QueryResult seller = tx.Query("SELECT charid FROM _char WHERE charname16 = $1", {row->seller});
            sellerCharacter = seller.Ok() && seller.Rows() > 0 ? seller.Int(0, 0) : 0;
            out = *row;
            result = OpenMarketTake::Ok;
            return true;
        },
        error);
    if (result == OpenMarketTake::Failed) core::LogWarning("open market: purchase of {}/{}: {}", account, listing, error);
    return result;
}

OpenMarketTake TakeBackOpenMarket(Database& db, std::int64_t account, std::uint32_t listing, std::int64_t character, std::uint8_t place,
                                  OpenMarketRecord& out) {
    OpenMarketTake result = OpenMarketTake::Failed;
    std::string error;
    db.Transaction(
        [&](Database& tx) {
            const auto row = LockOpenMarket(tx, account, listing);
            if (!row) return result = OpenMarketTake::NotListed, false;
            const auto status = game::openmarket::Status(row->status);
            if ((status != game::openmarket::Status::OnSale && status != game::openmarket::Status::Expired) || !row->held) {
                return result = OpenMarketTake::NotOnSale, false;
            }
            if (!PutInBag(tx, character, place, row->item.id)) return result = OpenMarketTake::BagTaken, false;
            if (!tx.Query("UPDATE _openmarket SET status = 255, itemid = 0, price = 0, deposit = 0 WHERE jid = $1 AND persnalid = $2", {account, std::int64_t(listing)})
                     .Ok()) {
                return false;
            }
            out = *row;
            result = OpenMarketTake::Ok;
            return true;
        },
        error);
    if (result == OpenMarketTake::Failed) core::LogWarning("open market: taking back {}/{}: {}", account, listing, error);
    return result;
}

bool SettleOpenMarket(Database& db, std::int64_t account, const std::vector<std::uint32_t>& listings, std::uint64_t payout, std::int64_t character,
                      std::uint64_t gold) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            std::uint64_t sum = 0;
            for (const std::uint32_t listing : listings) {
                const auto row = LockOpenMarket(tx, account, listing);
                if (!row || row->status != std::uint8_t(game::openmarket::Status::Sold)) return false;
                sum += game::openmarket::Payout(row->price, row->deposit, row->saleFee);
                if (!tx.Query("UPDATE _openmarket SET status = 255, itemid = 0, price = 0, deposit = 0 WHERE jid = $1 AND persnalid = $2",
                              {account, std::int64_t(listing)})
                         .Ok()) {
                    return false;
                }
            }
            if (sum != payout) return false;
            return tx.Query("UPDATE _char SET remaingold = $2 WHERE charid = $1", {character, std::int64_t(gold)}).Ok();
        },
        error);
}

std::vector<OpenMarketChange> AgeOpenMarket(Database& db, std::int64_t account, std::int64_t keepSeconds) {
    std::vector<OpenMarketChange> out;
    std::string error;
    db.Transaction(
        [&](Database& tx) {
            out.clear();
            // On sale past its end: expired, kept 28 days more.
            QueryResult expired = tx.Query("UPDATE _openmarket SET status = 2, enddate = enddate + make_interval(secs => $2::bigint) "
                                           "WHERE jid = $1 AND status = 0 AND enddate <= localtimestamp RETURNING persnalid, refitemid",
                                           {account, keepSeconds});
            if (!expired.Ok()) return false;
            for (int k = 0; k < expired.Rows(); ++k) out.push_back({std::uint32_t(expired.Int(k, 0)), std::uint32_t(expired.Int(k, 1)), 2});
            // Sold or expired past their 28 days: gone; an unsold item deleted (_STRG_DEL_ITEM_OPENMARKET), unsettled gold lost.
            QueryResult done = tx.Query("SELECT persnalid, refitemid, itemid FROM _openmarket WHERE jid = $1 AND status IN (1, 2) AND enddate <= localtimestamp FOR UPDATE",
                                        {account});
            if (!done.Ok()) return false;
            for (int k = 0; k < done.Rows(); ++k) {
                const std::int64_t item = done.Int(k, 2);
                if (!tx.Query("UPDATE _openmarket SET status = 255, itemid = 0, price = 0, deposit = 0 WHERE jid = $1 AND persnalid = $2", {account, done.Int(k, 0)})
                         .Ok() ||
                    (item != 0 && !FreeItem(tx, item))) {
                    return false;
                }
                out.push_back({std::uint32_t(done.Int(k, 0)), std::uint32_t(done.Int(k, 1)), 255});
            }
            return true;
        },
        error);
    return out;
}

bool SaveSidesIn(Database& tx, const ExchangeSaveSide& a, const ExchangeSaveSide& b, std::string& error, bool gold) {
    {
        {
            const std::array<const ExchangeSaveSide*, 2> sides{&a, &b};
            std::unordered_set<std::int64_t> kept;
            for (const ExchangeSaveSide* s : sides) {
                for (const StoredItem& i : s->present) kept.insert(i.id);
            }
            // Anywhere but these two characters' inventories: another character's, a storage's.
            const auto foreign = [&](std::int64_t id) {
                QueryResult q = tx.Query("SELECT 1 FROM _inventory WHERE itemid = $1 AND charid <> $2 AND charid <> $3 "
                                         "UNION ALL SELECT 1 FROM _chest WHERE itemid = $1 UNION ALL SELECT 1 FROM _guildchest WHERE itemid = $1 "
                                         "UNION ALL SELECT 1 FROM _invcos WHERE itemid = $1 "
                                         "UNION ALL SELECT 1 FROM _inventoryforlinkedstorage WHERE itemid = $1 UNION ALL SELECT 1 FROM _openmarket WHERE itemid = $1",
                                         {id, a.character, b.character});
                return !q.Ok() || q.Rows() > 0;
            };
            // First every place either side emptied (what went to the other side is placed below).
            std::vector<std::int64_t> left;
            for (const ExchangeSaveSide* s : sides) {
                for (std::uint8_t slot : s->emptied) {
                    QueryResult was = tx.Query("SELECT itemid FROM _inventory WHERE charid = $1 AND slot = $2 FOR UPDATE", {s->character, int(slot)});
                    if (!was.Ok()) return false;
                    if (was.Rows() == 0) continue;
                    if (!tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND slot = $2", {s->character, int(slot)}).Ok()) return false;
                    left.push_back(was.Int(0, 0));
                }
            }
            // Items merged into others (a side's own moves still waiting when the deal was made) go.
            for (const ExchangeSaveSide* s : sides) {
                for (std::int64_t id : s->gone) {
                    if (id == 0 || kept.contains(id) || foreign(id)) continue;
                    if (!tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND itemid = $2", {s->character, id}).Ok() || !FreeItem(tx, id)) return false;
                }
            }
            for (std::int64_t id : left) {
                if (id != 0 && !kept.contains(id) && !foreign(id) && !FreeItem(tx, id)) return false;
            }
            // Then each item where it is now, on whichever side, and nowhere else of the two.
            for (const ExchangeSaveSide* s : sides) {
                for (const StoredItem& i : s->present) {
                    if (i.id == 0) continue;
                    if (foreign(i.id)) {
                        core::LogWarning("exchange of characters {} and {}: item {} belongs elsewhere; not saved", a.character, b.character, i.id);
                        continue;
                    }
                    if (!WriteItem(tx, i.id, i, error)) return false;
                    if (!tx.Query("UPDATE _inventory SET itemid = 0 WHERE (charid = $1 OR charid = $2) AND itemid = $3 AND NOT (charid = $4 AND slot = $5)",
                                  {a.character, b.character, i.id, s->character, int(i.slot)})
                             .Ok() ||
                        !tx.Query("INSERT INTO _inventory (charid, slot, itemid) VALUES ($1, $2, $3) ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                                  {s->character, int(i.slot), i.id})
                             .Ok()) {
                        return false;
                    }
                }
            }
            for (const ExchangeSaveSide* s : sides) {
                if (gold && !tx.Query("UPDATE _char SET remaingold = $2 WHERE charid = $1", {s->character, std::int64_t(s->gold)}).Ok()) return false;
            }
            return true;
        }
    }
}

bool SaveExchange(Database& db, const ExchangeSaveSide& a, const ExchangeSaveSide& b) {
    std::string error;
    const bool ok = db.Transaction([&](Database& tx) { return SaveSidesIn(tx, a, b, error, true); }, error);
    if (!ok) core::LogWarning("exchange of characters {} and {}: {}", a.character, b.character, error);
    return ok;
}

bool SaveShopDeal(Database& db, const ExchangeSaveSide& side, std::int64_t honorSpent) {
    std::string error;
    // One side: the other is the same character with nothing moved (its gold the same).
    ExchangeSaveSide none;
    none.character = side.character;
    none.gold = side.gold;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            if (honorSpent > 0 &&
                !tx.Query("UPDATE _trainingcampmember SET honorpoint = greatest(honorpoint - $2, 0) WHERE charid = $1", {side.character, honorSpent}).Ok()) {
                return false;
            }
            return SaveSidesIn(tx, side, none, error, true);
        },
        error);
    if (!ok) core::LogWarning("shop deal of character {}: {}", side.character, error);
    return ok;
}

GiveResult GiveItems(Database& db, std::uint16_t, std::string_view name, const std::vector<StoredItem>& items, std::uint64_t gold, std::string& error) {
    GiveResult result = GiveResult::Failed;
    db.Transaction(
        [&](Database& tx) {
            QueryResult c = tx.Query("SELECT c.charid, u.userjid, c.inventorysize FROM _char c JOIN _user u ON u.charid = c.charid "
                                     "WHERE lower(c.charname16) = $1 AND c.deleted = 0 FOR UPDATE OF c",
                                     {Lower(name)});
            if (!c.Ok() || c.Rows() == 0) {
                result = c.Ok() ? GiveResult::NoCharacter : GiveResult::Failed;
                if (!c.Ok()) error = c.Error();
                return false;
            }
            const std::int64_t character = c.Int(0, 0), account = c.Int(0, 1);
            const int size = int(c.Int(0, 2));
            // The game server keeps an online character's items in memory: not while it plays.
            QueryResult online = tx.Query("SELECT 1 FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {account});
            if (!online.Ok() || online.Rows() > 0) {
                result = online.Ok() ? GiveResult::Online : GiveResult::Failed;
                return false;
            }
            QueryResult taken = tx.Query("SELECT slot FROM _inventory WHERE charid = $1 AND itemid <> 0", {character});
            if (!taken.Ok()) {
                error = taken.Error();
                return false;
            }
            std::vector<bool> used(std::size_t(std::max(size, 13)), false);
            for (int k = 0; k < taken.Rows(); ++k) {
                if (const auto s = std::size_t(taken.Int(k, 0)); s < used.size()) used[s] = true;
            }
            const std::vector<std::int64_t> ids = AllocItems(tx, int(items.size()));
            if (ids.size() != items.size()) {
                error = "no item ids";
                return false;
            }
            for (std::size_t k = 0; k < items.size(); ++k) {
                std::size_t place = 13;
                while (place < used.size() && used[place]) ++place;
                if (place >= used.size()) {
                    result = GiveResult::BagFull;
                    return false;
                }
                used[place] = true;
                if (!WriteItem(tx, ids[k], items[k], error)) return false;
                if (!tx.Query("INSERT INTO _inventory (charid, slot, itemid) VALUES ($1, $2, $3) ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                              {character, int(place), ids[k]})
                         .Ok()) {
                    return false;
                }
            }
            if (gold > 0 && !tx.Query("UPDATE _char SET remaingold = remaingold + $2 WHERE charid = $1", {character, std::int64_t(gold)}).Ok()) return false;
            result = GiveResult::Done;
            return true;
        },
        error);
    return result;
}

std::vector<std::int64_t> ReserveItemIds(Database& db, int count) {
    std::vector<std::int64_t> ids;
    std::string error;
    db.Transaction(
        [&](Database& tx) {
            ids = AllocItems(tx, count);
            return int(ids.size()) == count;
        },
        error);
    if (int(ids.size()) != count) ids.clear();
    return ids;
}

// --- Storage ------------------------------------------------------------------------------------

std::optional<StorageRow> LoadStorage(Database& db, std::int64_t account, std::uint16_t) {
    QueryResult size = db.Query("SELECT chestsize FROM _chestinfo WHERE jid = $1", {account});
    QueryResult gold = db.Query("SELECT gold FROM _accountjid WHERE jid = $1", {account});
    QueryResult items = db.Query(std::format("SELECT c.slot, c.itemid, {} FROM _chest c JOIN _items it ON it.id64 = c.itemid "
                                             "WHERE c.userjid = $1 AND c.itemid <> 0 ORDER BY c.slot",
                                             ItemSelect("it")),
                                 {account});
    if (!size.Ok() || !gold.Ok() || !items.Ok()) return std::nullopt;
    StorageRow row;
    if (size.Rows() > 0) row.size = std::uint8_t(std::clamp<std::int64_t>(size.Int(0, 0), 0, 255));
    if (gold.Rows() > 0) row.gold = std::uint64_t(gold.Int(0, 0));
    for (int k = 0; k < items.Rows(); ++k) {
        StoredItem item;
        item.slot = std::uint8_t(items.Int(k, 0));
        item.id = items.Int(k, 1);
        ReadItem(items, k, 2, item);
        row.items.push_back(std::move(item));
    }
    return row;
}

std::optional<StorageRow> LoadGuildChest(Database& db, std::uint32_t guild) {
    QueryResult gold = db.Query("SELECT gold FROM _guild WHERE id = $1", {std::int64_t(guild)});
    QueryResult items = db.Query(std::format("SELECT c.slot, c.itemid, {} FROM _guildchest c JOIN _items it ON it.id64 = c.itemid "
                                             "WHERE c.guildid = $1 AND c.itemid <> 0 ORDER BY c.slot",
                                             ItemSelect("it")),
                                 {std::int64_t(guild)});
    if (!gold.Ok() || !items.Ok() || gold.Rows() == 0) return std::nullopt;
    StorageRow row;
    row.gold = std::uint64_t(gold.Int(0, 0));
    for (int k = 0; k < items.Rows(); ++k) {
        StoredItem item;
        item.slot = std::uint8_t(items.Int(k, 0));
        item.id = items.Int(k, 1);
        ReadItem(items, k, 2, item);
        row.items.push_back(std::move(item));
    }
    return row;
}

bool SaveStorageMove(Database& db, const ExchangeSaveSide& bag, const StorageSaveSide& storage, bool gold) {
    std::string error;
    const std::int64_t character = bag.character;
    const bool guildChest = storage.guild != 0;
    // The storage's own table, its owner's column and its owner.
    const char* table = guildChest ? "_guildchest" : "_chest";
    const char* owner = guildChest ? "guildid" : "userjid";
    const std::int64_t holder = guildChest ? std::int64_t(storage.guild) : storage.account;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            if (gold) {
                // Both gold columns together (the original's procedures update both or neither).
                if (!tx.Query("UPDATE _char SET remaingold = $2 WHERE charid = $1", {character, std::int64_t(bag.gold)}).Ok()) return false;
                if (guildChest) {
                    QueryResult g = tx.Query("UPDATE _guild SET gold = $2 WHERE id = $1", {holder, std::int64_t(storage.gold)});
                    if (!g.Ok() || g.Affected() != 1) return false;
                } else if (!tx.Query("INSERT INTO _accountjid (accountid, jid, gold) SELECT u.struserid, u.jid, $2 FROM sro_vt_account.tb_user u WHERE u.jid = $1 "
                                     "ON CONFLICT (accountid) DO UPDATE SET jid = EXCLUDED.jid, gold = EXCLUDED.gold",
                                     {holder, std::int64_t(storage.gold)})
                                .Ok()) {
                    return false;
                }
            }
            // An account's storage has its size's row from its first use (_chestinfo).
            if (!guildChest &&
                !tx.Query("INSERT INTO _chestinfo (jid, chestsize) SELECT $1, 150 WHERE NOT EXISTS (SELECT 1 FROM _chestinfo WHERE jid = $1)", {holder}).Ok()) {
                return false;
            }
            std::unordered_set<std::int64_t> kept;
            for (const StoredItem& i : bag.present) kept.insert(i.id);
            for (const StoredItem& i : storage.present) kept.insert(i.id);
            // Held anywhere but this character's bag and avatars and this storage: another character's, another storage,
            // a pet's bag, a cube, the Open Market.
            const std::string elsewhere = std::format("SELECT 1 FROM _inventory WHERE itemid = $1 AND charid <> $2 "
                                                      "UNION ALL SELECT 1 FROM _inventoryforavatar WHERE itemid = $1 AND charid <> $2 "
                                                      "UNION ALL SELECT 1 FROM _chest WHERE itemid = $1 AND {} "
                                                      "UNION ALL SELECT 1 FROM _guildchest WHERE itemid = $1 AND {} "
                                                      "UNION ALL SELECT 1 FROM _invcos WHERE itemid = $1 "
                                                      "UNION ALL SELECT 1 FROM _inventoryforlinkedstorage WHERE itemid = $1 "
                                                      "UNION ALL SELECT 1 FROM _openmarket WHERE itemid = $1",
                                                      guildChest ? "true" : "userjid <> $3", guildChest ? "guildid <> $3" : "true");
            const auto foreign = [&](std::int64_t id) {
                QueryResult q = tx.Query(elsewhere, {id, character, holder});
                return !q.Ok() || q.Rows() > 0;
            };
            // The places either side emptied first (what went to the other side is placed below).
            std::vector<std::int64_t> left;
            for (std::uint8_t slot : bag.emptied) {
                QueryResult was = tx.Query("SELECT itemid FROM _inventory WHERE charid = $1 AND slot = $2 FOR UPDATE", {character, int(slot)});
                if (!was.Ok()) return false;
                if (was.Rows() == 0) continue;
                if (!tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND slot = $2", {character, int(slot)}).Ok()) return false;
                if (!was.IsNull(0, 0)) left.push_back(was.Int(0, 0));
            }
            for (std::uint8_t slot : storage.emptied) {
                QueryResult was = tx.Query(std::format("SELECT itemid FROM {} WHERE {} = $1 AND slot = $2 FOR UPDATE", table, owner), {holder, int(slot)});
                if (!was.Ok()) return false;
                if (was.Rows() == 0) continue;
                if (!tx.Query(std::format("UPDATE {} SET itemid = 0 WHERE {} = $1 AND slot = $2", table, owner), {holder, int(slot)}).Ok()) return false;
                if (!was.IsNull(0, 0)) left.push_back(was.Int(0, 0));
            }
            // What merged away, or left both sides, goes.
            left.insert(left.end(), bag.gone.begin(), bag.gone.end());
            for (std::int64_t id : left) {
                if (id == 0 || kept.contains(id) || foreign(id)) continue;
                if (!tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND itemid = $2", {character, id}).Ok() ||
                    !tx.Query(std::format("UPDATE {} SET itemid = 0 WHERE {} = $1 AND itemid = $2", table, owner), {holder, id}).Ok() || !FreeItem(tx, id)) {
                    return false;
                }
            }
            // Each item where it is now, with its own id, and nowhere else of the two sides.
            for (const StoredItem& i : bag.present) {
                if (i.id == 0) continue;
                if (foreign(i.id)) {
                    core::LogWarning("storage move of character {}: item {} belongs elsewhere; not saved", character, i.id);
                    continue;
                }
                if (!WriteItem(tx, i.id, i, error)) return false;
                if (!tx.Query(std::format("UPDATE {} SET itemid = 0 WHERE {} = $1 AND itemid = $2", table, owner), {holder, i.id}).Ok() ||
                    !tx.Query("UPDATE _inventoryforavatar SET itemid = 0 WHERE charid = $1 AND itemid = $2", {character, i.id}).Ok() ||
                    !tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND itemid = $2 AND slot <> $3", {character, i.id, int(i.slot)}).Ok() ||
                    !tx.Query("INSERT INTO _inventory (charid, slot, itemid) VALUES ($1, $2, $3) ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                              {character, int(i.slot), i.id})
                         .Ok()) {
                    return false;
                }
            }
            for (const StoredItem& stored : storage.present) {
                std::int64_t id = stored.id;
                if (id == 0) {
                    // Every item the game server sends has its id; one without gets a new row rather than be lost.
                    const std::vector<std::int64_t> fresh = AllocItems(tx, 1);
                    if (fresh.size() != 1) return false;
                    id = fresh[0];
                } else if (foreign(id)) {
                    core::LogWarning("storage move of character {}: item {} belongs elsewhere; not saved", character, id);
                    continue;
                }
                if (!WriteItem(tx, id, stored, error)) return false;
                if (!tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND itemid = $2", {character, id}).Ok() ||
                    !tx.Query("UPDATE _inventoryforavatar SET itemid = 0 WHERE charid = $1 AND itemid = $2", {character, id}).Ok() ||
                    !tx.Query(std::format("UPDATE {} SET itemid = 0 WHERE {} = $1 AND itemid = $2 AND slot <> $3", table, owner), {holder, id, int(stored.slot)})
                         .Ok() ||
                    !tx.Query(std::format("INSERT INTO {0} ({1}, slot, itemid) VALUES ($1, $2, $3) ON CONFLICT ({1}, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                                          table, owner),
                              {holder, int(stored.slot), id})
                         .Ok()) {
                    return false;
                }
            }
            return true;
        },
        error);
    if (!ok) core::LogWarning("storage move of character {} ({} {}): {}", character, guildChest ? "guild" : "account", holder, error);
    return ok;
}

// --- The rest of a character --------------------------------------------------------------------

bool SaveLongCooldown(Database& db, std::int64_t character, std::uint32_t skill, std::int64_t started) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            return tx.Query("DELETE FROM _timedjob WHERE charid = $1 AND category = 6 AND jobid = 200 AND data1 = $2", {character, std::int64_t(skill)}).Ok() &&
                   tx.Query("INSERT INTO _timedjob (charid, category, jobid, timetokeep, data1, data2, serial64, jid) "
                            "VALUES ($1, 6, 200, 1, $2, $3, 0, coalesce((SELECT userjid FROM _user WHERE charid = $1), 0))",
                            {character, std::int64_t(skill), started}).Ok();
        },
        error);
}

bool DeleteLongCooldowns(Database& db, std::int64_t character, const std::vector<std::uint32_t>& skills) {
    bool ok = true;
    for (const std::uint32_t skill : skills) {
        ok = db.Query("DELETE FROM _timedjob WHERE charid = $1 AND category = 6 AND jobid = 200 AND data1 = $2", {character, std::int64_t(skill)}).Ok() && ok;
    }
    return ok;
}

bool SaveBuffs(Database& db, std::int64_t character, const std::vector<CharacterRow::Buff>& buffs) {
    // _timedjob category 0: the skill as the job, its time left in TimeToKeep in
    // seconds (rounded up, so a buff with less than one left is not lost), the account as JID - as
    // the original makes and updates them (duration / 1000).
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            if (!tx.Query("DELETE FROM _timedjob WHERE charid = $1 AND category = 0", {character}).Ok()) return false;
            for (const CharacterRow::Buff& b : buffs) {
                if (b.remainingMs == 0) continue;
                if (!tx.Query("INSERT INTO _timedjob (charid, category, jobid, timetokeep, data1, data2, serial64, jid) "
                              "VALUES ($1, 0, $2, $3, 0, 0, 0, coalesce((SELECT userjid FROM _user WHERE charid = $1), 0))",
                              {character, std::int64_t(b.skill), std::int64_t(std::min<std::uint64_t>((b.remainingMs + 999) / 1000, 0x7FFFFFFF))})
                         .Ok()) {
                    return false;
                }
            }
            return true;
        },
        error);
}

bool SaveGold(Database& db, std::int64_t character, std::uint64_t gold) {
    return db.Query("UPDATE _char SET remaingold = $2 WHERE charid = $1", {character, std::int64_t(gold)}).Ok();
}

bool SaveReturnGate(Database& db, std::int64_t character, std::uint32_t gate) {
    return db.Query("UPDATE _char SET appointedteleport = $2 WHERE charid = $1", {character, std::int64_t(gate)}).Ok();
}

bool LoadTeleportZones(Database& db, std::unordered_map<std::string, std::string>& out, std::string& error) {
    out.clear();
    const QueryResult r = db.Query("SELECT zonename, coalesce(strparam1, '') FROM _refeventzone WHERE service = 1 AND eventname = 'TELEPORT'");
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    for (int i = 0; i < r.Rows(); ++i) {
        std::string zone = r.Text(i, 0);
        std::transform(zone.begin(), zone.end(), zone.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        out[zone] = r.Text(i, 1);
    }
    return true;
}

bool SaveStallSkin(Database& db, std::int64_t character, std::uint32_t refId) {
    QueryResult q = db.Query("UPDATE _staticavatar SET param1 = $2 WHERE charid = $1", {character, std::int64_t(refId)});
    if (!q.Ok()) return false;
    if (q.Affected() > 0) return true;
    return db.Query("INSERT INTO _staticavatar (charid, param1) VALUES ($1, $2)", {character, std::int64_t(refId)}).Ok();
}

// --- Pets (_CharCOS) ------------------------------------------------------------------------------------------
// RentEndTime as Unix seconds (a timestamp without a zone, in the session's: the same both ways, kEpoch).
namespace {
constexpr const char* kCosRentWrite = "CASE WHEN $10::bigint = 0 THEN NULL ELSE to_timestamp($10::bigint) AT TIME ZONE current_setting('TimeZone') END";
} // namespace

std::optional<CosRow> LoadCos(Database& db, std::int64_t id) {
    QueryResult q = db.Query(std::format("SELECT id, ownercharid, refcharid, hp, state, coalesce(charname, ''), lvl, expoffset, hgp, petoption, "
                                         "coalesce({}, 0) FROM _charcos WHERE id = $1",
                                         Epoch("rentendtime")),
                             {id});
    if (!q.Ok() || q.Rows() == 0) return std::nullopt;
    CosRow r;
    r.id = q.Int(0, 0);
    r.owner = q.Int(0, 1);
    r.refId = std::uint32_t(q.Int(0, 2));
    r.hp = std::uint32_t(std::max<std::int64_t>(0, q.Int(0, 3)));
    r.state = std::uint8_t(q.Int(0, 4));
    r.name = q.Text(0, 5);
    r.level = int(q.Int(0, 6));
    r.experience = q.Int(0, 7);
    r.hgp = int(q.Int(0, 8));
    r.option = std::uint32_t(q.Int(0, 9));
    r.rentUntil = q.Int(0, 10);
    // Its bag's size from the reference tables (an imported shard's; absent: 0, the game server decides).
    if (db.Query("SELECT to_regclass('_refobjchar') IS NOT NULL AND to_regclass('_refobjcommon') IS NOT NULL").Bool(0, 0)) {
        QueryResult size = db.Query("SELECT c.inventorysize FROM _refobjcommon o JOIN _refobjchar c ON c.id = o.link WHERE o.id = $1", {std::int64_t(r.refId)});
        if (size.Ok() && size.Rows() > 0) r.bagSize = int(size.Int(0, 0));
    }
    QueryResult items = db.Query(std::format("SELECT b.slot, {} FROM _invcos b JOIN _items it ON it.id64 = b.itemid "
                                             "WHERE b.cosid = $1 AND b.itemid IS NOT NULL AND b.itemid <> 0 ORDER BY b.slot",
                                             ItemSelect("it")),
                                 {id});
    if (!items.Ok()) return std::nullopt;
    for (int k = 0; k < items.Rows(); ++k) {
        StoredItem item;
        item.slot = std::uint8_t(items.Int(k, 0));
        ReadItem(items, k, 1, item);
        r.bag.push_back(std::move(item));
    }
    // Its timed jobs.
    if (db.Query("SELECT to_regclass('_timedjobforpet') IS NOT NULL").Bool(0, 0)) {
        QueryResult jobs = db.Query("SELECT category, jobid, timetokeep, coalesce(data1, 0), coalesce(data2, 0), coalesce(data3, 0), coalesce(data4, 0), "
                                    "coalesce(data5, 0), coalesce(data6, 0), coalesce(data7, 0), coalesce(data8, 0), serial64 FROM _timedjobforpet "
                                    "WHERE charid = $1 ORDER BY id",
                                    {id});
        if (!jobs.Ok()) return std::nullopt;
        for (int k = 0; k < jobs.Rows(); ++k) {
            PetJobRow j;
            j.category = std::uint8_t(jobs.Int(k, 0));
            j.job = std::uint32_t(jobs.Int(k, 1));
            j.until = jobs.Int(k, 2);
            for (int d = 0; d < 8; ++d) j.data[d] = std::int32_t(jobs.Int(k, 3 + d));
            j.serial = jobs.Int(k, 11);
            r.jobs.push_back(j);
        }
    }
    return r;
}

bool AddPetJob(Database& db, std::int64_t cos, const PetJobRow& job) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            if (!tx.Query("DELETE FROM _timedjobforpet WHERE charid = $1 AND category = $2 AND jobid = $3", {cos, int(job.category), std::int64_t(job.job)}).Ok()) {
                return false;
            }
            return tx.Query("INSERT INTO _timedjobforpet (charid, category, jobid, timetokeep, data1, data2, data3, data4, data5, data6, data7, data8, serial64, jid) "
                            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, 0)",
                            {cos, int(job.category), std::int64_t(job.job), std::int64_t(std::clamp<std::int64_t>(job.until, 0, 0x7FFFFFFF)), int(job.data[0]),
                             int(job.data[1]), int(job.data[2]), int(job.data[3]), int(job.data[4]), int(job.data[5]), int(job.data[6]), int(job.data[7]),
                             job.serial})
                .Ok();
        },
        error);
}

bool EndPetJob(Database& db, std::int64_t cos, std::uint8_t category, std::uint32_t job) {
    return db.Query("DELETE FROM _timedjobforpet WHERE charid = $1 AND category = $2 AND jobid = $3", {cos, int(category), std::int64_t(job)}).Ok();
}

bool SaveCosItems(Database& db, const ExchangeSaveSide& inventory, std::int64_t cos, const std::vector<StoredItem>& present,
                  const std::vector<std::uint8_t>& emptied) {
    std::string error;
    const std::int64_t character = inventory.character;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            std::unordered_set<std::int64_t> kept;
            for (const StoredItem& i : inventory.present) kept.insert(i.id);
            for (const StoredItem& i : present) kept.insert(i.id);
            // Anywhere but this character's inventory and this pet's bag: another's, a storage, another pet's bag.
            const auto foreign = [&](std::int64_t id) {
                QueryResult q = tx.Query("SELECT 1 FROM _inventory WHERE itemid = $1 AND charid <> $2 UNION ALL SELECT 1 FROM _chest WHERE itemid = $1 "
                                         "UNION ALL SELECT 1 FROM _guildchest WHERE itemid = $1 "
                                         "UNION ALL SELECT 1 FROM _invcos WHERE itemid = $1 AND cosid <> $3 UNION ALL SELECT 1 FROM _openmarket WHERE itemid = $1",
                                         {id, character, cos});
                return !q.Ok() || q.Rows() > 0;
            };
            // The places either side emptied (what went to the other side is placed below), then what left both: freed.
            std::vector<std::int64_t> left;
            for (std::uint8_t slot : inventory.emptied) {
                QueryResult was = tx.Query("SELECT itemid FROM _inventory WHERE charid = $1 AND slot = $2 FOR UPDATE", {character, int(slot)});
                if (!was.Ok()) return false;
                if (was.Rows() == 0) continue;
                if (!tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND slot = $2", {character, int(slot)}).Ok()) return false;
                left.push_back(was.Int(0, 0));
            }
            for (std::uint8_t slot : emptied) {
                QueryResult was = tx.Query("SELECT itemid FROM _invcos WHERE cosid = $1 AND slot = $2 FOR UPDATE", {cos, int(slot)});
                if (!was.Ok()) return false;
                if (was.Rows() == 0) continue;
                if (!tx.Query("UPDATE _invcos SET itemid = 0 WHERE cosid = $1 AND slot = $2", {cos, int(slot)}).Ok()) return false;
                if (!was.IsNull(0, 0)) left.push_back(was.Int(0, 0));
            }
            left.insert(left.end(), inventory.gone.begin(), inventory.gone.end());
            for (std::int64_t id : left) {
                if (id == 0 || kept.contains(id) || foreign(id)) continue;
                if (!tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND itemid = $2", {character, id}).Ok() ||
                    !tx.Query("UPDATE _invcos SET itemid = 0 WHERE cosid = $1 AND itemid = $2", {cos, id}).Ok() || !FreeItem(tx, id)) {
                    return false;
                }
            }
            for (const StoredItem& i : inventory.present) {
                if (i.id == 0 || foreign(i.id)) continue;
                if (!WriteItem(tx, i.id, i, error)) return false;
                if (!tx.Query("UPDATE _invcos SET itemid = 0 WHERE cosid = $1 AND itemid = $2", {cos, i.id}).Ok() ||
                    !tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND itemid = $2 AND slot <> $3", {character, i.id, int(i.slot)}).Ok() ||
                    !tx.Query("INSERT INTO _inventory (charid, slot, itemid) VALUES ($1, $2, $3) ON CONFLICT (charid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                              {character, int(i.slot), i.id})
                         .Ok()) {
                    return false;
                }
            }
            for (const StoredItem& i : present) {
                if (i.id == 0 || foreign(i.id)) continue;
                if (!WriteItem(tx, i.id, i, error)) return false;
                if (!tx.Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND itemid = $2", {character, i.id}).Ok() ||
                    !tx.Query("UPDATE _invcos SET itemid = 0 WHERE cosid = $1 AND itemid = $2 AND slot <> $3", {cos, i.id, int(i.slot)}).Ok() ||
                    !tx.Query("INSERT INTO _invcos (cosid, slot, itemid) VALUES ($1, $2, $3) ON CONFLICT (cosid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                              {cos, int(i.slot), i.id})
                         .Ok()) {
                    return false;
                }
            }
            return true;
        },
        error);
    if (!ok) core::LogWarning("a pet's bag ({}) and its owner's items ({}) not saved: {}", cos, character, error);
    return ok;
}

std::int64_t AddCos(Database& db, const CosRow& row, std::int64_t item, std::string& error) {
    std::int64_t id = 0;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            QueryResult put = tx.Query(std::format("INSERT INTO _charcos (ownercharid, refcharid, hp, mp, keepernpc, state, charname, lvl, expoffset, hgp, "
                                                   "petoption, rentendtime) VALUES ($1, $2, $3, 0, 0, $4, NULLIF($5, ''), $6, $7, $8, $9, {}) RETURNING id",
                                                   kCosRentWrite),
                                       {row.owner, std::int64_t(row.refId), std::int64_t(row.hp), std::int64_t(row.state), row.name, std::int64_t(row.level),
                                        row.experience, std::int64_t(row.hgp), std::int64_t(row.option), row.rentUntil});
            if (!put.Ok() || put.Rows() == 0) return false;
            id = put.Int(0, 0);
            return item == 0 || tx.Query("UPDATE _items SET data = $2 WHERE id64 = $1", {item, id}).Affected() == 1;
        },
        error);
    return ok ? id : 0;
}

CosNaming NameCos(Database& db, std::int64_t id, std::string_view name) {
    CosNaming result = CosNaming::Failed;
    std::string error;
    db.Transaction(
        [&](Database& tx) {
            // One naming at a time across the shard (two pets asking for one name at once: the second hears it is taken).
            if (!tx.Query("SELECT pg_advisory_xact_lock(hashtext('_charcos.charname'))").Ok()) return false;
            QueryResult taken = tx.Query("SELECT 1 FROM _charcos WHERE lower(charname) = lower($1) AND id <> $2", {std::string(name), id});
            if (!taken.Ok()) return false;
            if (taken.Rows() > 0) {
                result = CosNaming::Taken;
                return true;
            }
            QueryResult set = tx.Query("UPDATE _charcos SET charname = $2 WHERE id = $1 AND coalesce(charname, '') = ''", {id, std::string(name)});
            if (!set.Ok()) return false;
            result = set.Affected() == 1 ? CosNaming::Named : CosNaming::HasOne;
            return true;
        },
        error);
    if (result == CosNaming::Failed) core::LogWarning("a pet ({}) not named: {}", id, error);
    return result;
}

bool SaveCos(Database& db, const CosRow& row) {
    // Its name only ever set (NameCos names it; a save of a row read before that keeps it).
    return db.Query(std::format("UPDATE _charcos SET ownercharid = $2, refcharid = $3, hp = $4, state = $5, charname = coalesce(NULLIF($6, ''), charname), lvl = $7, "
                                "expoffset = $8, hgp = $9, rentendtime = {}, petoption = $11 WHERE id = $1",
                                kCosRentWrite),
                    {row.id, row.owner, std::int64_t(row.refId), std::int64_t(row.hp), std::int64_t(row.state), row.name, std::int64_t(row.level),
                     row.experience, std::int64_t(row.hgp), row.rentUntil, std::int64_t(row.option)})
        .Ok();
}

std::vector<CosRow> LoadCosOf(Database& db, std::int64_t owner, const std::vector<std::uint32_t>& models) {
    std::vector<CosRow> out;
    if (models.empty()) return out;
    std::string list;
    for (const std::uint32_t m : models) list += (list.empty() ? "" : ",") + std::to_string(m);
    QueryResult ids = db.Query(std::format("SELECT id FROM _charcos WHERE ownercharid = $1 AND refcharid IN ({}) ORDER BY id DESC", list), {owner});
    for (int k = 0; ids.Ok() && k < ids.Rows(); ++k) {
        std::optional<CosRow> row = LoadCos(db, ids.Int(k, 0));
        if (!row) continue;
        // The goods' buyers (who may sell them where).
        QueryResult made = db.Query("SELECT b.slot, coalesce(it.creatername, '') FROM _invcos b JOIN _items it ON it.id64 = b.itemid "
                                    "WHERE b.cosid = $1 AND b.itemid IS NOT NULL AND b.itemid <> 0",
                                    {row->id});
        for (int m = 0; made.Ok() && m < made.Rows(); ++m) {
            for (StoredItem& i : row->bag)
                if (i.slot == std::uint8_t(made.Int(m, 0))) i.creator = made.Text(m, 1);
        }
        out.push_back(std::move(*row));
    }
    return out;
}

bool SaveCart(Database& tx, std::int64_t cos, const std::vector<StoredItem>& present, std::string& error) {
    QueryResult was = tx.Query("SELECT slot, coalesce(itemid, 0) FROM _invcos WHERE cosid = $1 FOR UPDATE", {cos});
    if (!was.Ok()) {
        error = was.Error();
        return false;
    }
    std::set<int> places;
    std::unordered_set<std::int64_t> kept;
    for (const StoredItem& i : present) {
        places.insert(i.slot);
        kept.insert(i.id);
    }
    for (const StoredItem& i : present) {
        if (i.id == 0) continue;
        if (!WriteItem(tx, i.id, i, error)) return false;
        if (!tx.Query("UPDATE _items SET creatername = NULLIF($2, '') WHERE id64 = $1", {i.id, i.creator}).Ok() ||
            !tx.Query("INSERT INTO _invcos (cosid, slot, itemid) VALUES ($1, $2, $3) ON CONFLICT (cosid, slot) DO UPDATE SET itemid = EXCLUDED.itemid",
                      {cos, int(i.slot), i.id})
                 .Ok()) {
            error = "a transport's place";
            return false;
        }
    }
    for (int k = 0; k < was.Rows(); ++k) {
        const int slot = int(was.Int(k, 0));
        const std::int64_t id = was.Int(k, 1);
        if (!places.contains(slot) && !tx.Query("UPDATE _invcos SET itemid = 0 WHERE cosid = $1 AND slot = $2", {cos, slot}).Ok()) {
            error = "a transport's place";
            return false;
        }
        // A good no place holds any more (sold, dropped): freed, unless it is somewhere else now.
        if (id == 0 || kept.contains(id)) continue;
        QueryResult elsewhere = tx.Query("SELECT 1 FROM _inventory WHERE itemid = $1 UNION ALL SELECT 1 FROM _chest WHERE itemid = $1 "
                                         "UNION ALL SELECT 1 FROM _invcos WHERE itemid = $1 AND cosid <> $2",
                                         {id, cos});
        if (!elsewhere.Ok() || (elsewhere.Rows() == 0 && !FreeItem(tx, id))) {
            error = "a transport's good";
            return false;
        }
    }
    return true;
}

bool DeleteCos(Database& db, std::int64_t cos) {
    std::string error;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            QueryResult goods = tx.Query("SELECT coalesce(itemid, 0) FROM _invcos WHERE cosid = $1", {cos});
            if (!goods.Ok()) return false;
            for (int k = 0; k < goods.Rows(); ++k) {
                const std::int64_t id = goods.Int(k, 0);
                if (id == 0) continue;
                QueryResult elsewhere = tx.Query("SELECT 1 FROM _inventory WHERE itemid = $1 UNION ALL SELECT 1 FROM _invcos WHERE itemid = $1 AND cosid <> $2",
                                                 {id, cos});
                if (!elsewhere.Ok() || (elsewhere.Rows() == 0 && !FreeItem(tx, id))) return false;
            }
            return tx.Query("DELETE FROM _invcos WHERE cosid = $1", {cos}).Ok() && tx.Query("DELETE FROM _charcos WHERE id = $1", {cos}).Ok();
        },
        error);
    if (!ok) core::LogWarning("a COS row ({}) not deleted: {}", cos, error);
    return ok;
}

bool SaveCharQuest(Database& db, std::int64_t character, const CharacterRow::Quest& q) {
    return db.Query("INSERT INTO _charquest (charid, questid, status, achievementcount, starttime, endtime, questdata1, questdata2) "
                    "VALUES ($1, $2, $3, $4, to_timestamp($5) AT TIME ZONE 'UTC', to_timestamp($6) AT TIME ZONE 'UTC', $7, $8) "
                    "ON CONFLICT (charid, questid) DO UPDATE SET status = excluded.status, achievementcount = excluded.achievementcount, "
                    "starttime = excluded.starttime, endtime = excluded.endtime, questdata1 = excluded.questdata1, questdata2 = excluded.questdata2",
                    {character, std::int64_t(q.quest), std::int64_t(q.status), std::int64_t(q.times), q.started, q.ended, q.data1, q.data2})
        .Ok();
}

bool SaveInventorySize(Database& db, std::int64_t character, std::uint8_t size) {
    return db.Query("UPDATE _char SET inventorysize = $2 WHERE charid = $1", {character, std::int64_t(size)}).Ok();
}

bool SaveRecallSpot(Database& db, std::int64_t character, const CharacterRow::Spot& spot) {
    return db.Query("UPDATE _char SET telregion = $2, telposx = $3, telposy = $4, telposz = $5 WHERE charid = $1",
                    {character, std::int64_t(std::int16_t(spot.region)), double(spot.x), double(spot.y), double(spot.z)})
        .Ok();
}

bool SaveDeathSpot(Database& db, std::int64_t character, const CharacterRow::Spot& spot) {
    return db.Query("UPDATE _char SET diedregion = $2, diedposx = $3, diedposy = $4, diedposz = $5 WHERE charid = $1",
                    {character, std::int64_t(std::int16_t(spot.region)), double(spot.x), double(spot.y), double(spot.z)})
        .Ok();
}

bool LoadOptionalTeleports(Database& db, std::vector<OptionalTeleportRow>& out, std::string& error) {
    QueryResult r = db.Query("SELECT id, regionid, pos_x, pos_y, pos_z, levelmin, levelmax FROM _refoptionalteleport WHERE service = 1 ORDER BY id");
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    out.clear();
    for (int k = 0; k < r.Rows(); ++k) {
        out.push_back({std::uint32_t(r.Int(k, 0)), std::uint16_t(std::int16_t(r.Int(k, 1))), float(r.Double(k, 2)), float(r.Double(k, 3)),
                       float(r.Double(k, 4)), int(r.Int(k, 5)), int(r.Int(k, 6))});
    }
    return true;
}

bool SaveHwanLevel(Database& db, std::int64_t character, int level) {
    return db.Query("UPDATE _char SET hwanlevel = $2 WHERE charid = $1", {character, std::int64_t(level)}).Ok();
}

bool SaveMarks(Database& db, std::int64_t character, std::uint8_t marks) {
    return db.Query("UPDATE _char SET autoinvestexp = $2 WHERE charid = $1", {character, std::int64_t(marks)}).Ok();
}

bool SaveVitals(Database& db, std::int64_t character, std::uint32_t hp, std::uint32_t mp) {
    return db.Query("UPDATE _char SET hp = $2, mp = $3 WHERE charid = $1", {character, std::int64_t(hp), std::int64_t(mp)}).Ok();
}

bool SaveLogout(Database& db, std::int64_t character) {
    return db.Query("UPDATE _char SET lastlogout = localtimestamp WHERE charid = $1", {character}).Ok();
}

bool SavePkCounts(Database& db, std::int64_t character, std::uint8_t daily, std::uint16_t total, std::int32_t points) {
    return db.Query("UPDATE _char SET dailypk = $2, totalpk = $3, pkpenaltypoint = $4 WHERE charid = $1",
                    {character, std::int64_t(daily), std::int64_t(total), std::int64_t(points)})
        .Ok();
}

bool SavePkKeeper(Database& db, std::int64_t character, std::optional<CharacterRow::PkKeeper> keeper) {
    std::string error;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            if (!tx.Query("DELETE FROM _timedjob WHERE charid = $1 AND category = 2 AND jobid = 0", {character}).Ok()) return false;
            if (!keeper) return true;
            return tx.Query("INSERT INTO _timedjob (charid, category, jobid, timetokeep, data1, data2, serial64, jid) "
                            "VALUES ($1, 2, 0, $2, $3, 0, 0, coalesce((SELECT userjid FROM _user WHERE charid = $1), 0))",
                            {character, keeper->secondsLeft, keeper->stamp})
                .Ok();
        },
        error);
    if (!ok) core::LogWarning("the PK keeper ({}): {}", character, error);
    return ok;
}

bool SaveBerserkPoints(Database& db, std::int64_t character, std::uint8_t points) {
    return db.Query("UPDATE _char SET remainhwancount = $2 WHERE charid = $1", {character, std::int64_t(points)}).Ok();
}

bool SaveProgress(Database& db, const std::vector<CharacterProgress>& progress) {
    bool ok = true;
    for (const CharacterProgress& p : progress) {
        ok = db.Query("UPDATE _char SET curlevel = $2, maxlevel = GREATEST(maxlevel, $2), expoffset = $3, remainskillpoint = $4, sexpoffset = $5, "
                      "strength = $6, intellect = $7, remainstatpoint = $8 WHERE charid = $1",
                      {p.character, int(p.level), std::int64_t(p.experience), std::int64_t(p.skillPoints), std::int64_t(p.skillExperience),
                       int(p.strength), int(p.intelligence), int(p.statPoints)})
                 .Ok() &&
             ok;
    }
    return ok;
}

bool SaveMastery(Database& db, std::int64_t character, std::uint32_t mastery, int level) {
    return db
        .Query("INSERT INTO _charskillmastery (charid, masteryid, level) VALUES ($1, $2, $3) "
               "ON CONFLICT (charid, masteryid) DO UPDATE SET level = EXCLUDED.level",
               {character, std::int64_t(mastery), level})
        .Ok();
}

bool SaveSkill(Database& db, std::int64_t character, std::uint32_t previous, std::uint32_t skill) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            if (previous != 0 && previous != skill &&
                !tx.Query("DELETE FROM _charskill WHERE charid = $1 AND skillid = $2", {character, std::int64_t(previous)}).Ok()) {
                return false;
            }
            return tx.Query("INSERT INTO _charskill (charid, skillid, enable) VALUES ($1, $2, 1) ON CONFLICT (charid, skillid) DO UPDATE SET enable = 1",
                            {character, std::int64_t(skill)})
                .Ok();
        },
        error);
}

bool SaveQuickSlot(Database& db, std::int64_t character, const CharacterRow::QuickSlot& slot) {
    // Every slot has a row (an empty one: type 0), as the original makes them.
    const bool empty = slot.kind == 0;
    return db
        .Query("INSERT INTO _clientconfig (charid, configtype, slotseq, slottype, data) VALUES ($1, 1, $2, $3, $4) "
               "ON CONFLICT (charid, configtype, slotseq) DO UPDATE SET slottype = EXCLUDED.slottype, data = EXCLUDED.data",
               {character, int(slot.slot), empty ? 0 : int(slot.kind), empty ? std::int64_t(0) : std::int64_t(std::int32_t(slot.value))})
        .Ok();
}

bool SaveAutoPotion(Database& db, std::int64_t character, const game::AutoPotion& settings) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            const auto word = [&](int type, std::int64_t data) {
                return tx
                    .Query("INSERT INTO _clientconfig (charid, configtype, slotseq, slottype, data) VALUES ($1, $2, 0, 0, $3) "
                           "ON CONFLICT (charid, configtype, slotseq) DO UPDATE SET slottype = 0, data = EXCLUDED.data",
                           {character, type, data})
                    .Ok();
            };
            for (int r = 0; r < game::AutoPotion::kOriginalRows; ++r) {
                const game::AutoPotion::Entry& e = settings.rows[std::size_t(r)];
                if (!word(11 + r, game::EncodeAutoPotionWord(e))) return false;
                const bool kept = e.item != 0
                                      ? tx.Query("INSERT INTO sr_auto_potion (charid, configtype, refobjid) VALUES ($1, $2, $3) "
                                                 "ON CONFLICT (charid, configtype) DO UPDATE SET refobjid = EXCLUDED.refobjid",
                                                 {character, 11 + r, std::int64_t(e.item)})
                                            .Ok()
                                      : tx.Query("DELETE FROM sr_auto_potion WHERE charid = $1 AND configtype = $2", {character, 11 + r}).Ok();
                if (!kept) return false;
            }
            // Ours: the vehicle's and the attack pet's rows, whole, in sr_auto_potion alone (types 101, 102); none for a
            // row as it starts (off, no item).
            for (int r = game::AutoPotion::kVehicle; r < game::AutoPotion::kRows; ++r) {
                const game::AutoPotion::Entry& e = settings.rows[std::size_t(r)];
                const int type = 101 + r - game::AutoPotion::kVehicle;
                if (!e.enabled && e.item == 0) {
                    if (!tx.Query("DELETE FROM sr_auto_potion WHERE charid = $1 AND configtype = $2", {character, type}).Ok()) return false;
                    continue;
                }
                if (!tx.Query("INSERT INTO sr_auto_potion (charid, configtype, refobjid, setting) VALUES ($1, $2, $3, $4) "
                              "ON CONFLICT (charid, configtype) DO UPDATE SET refobjid = EXCLUDED.refobjid, setting = EXCLUDED.setting",
                              {character, type, std::int64_t(e.item), int(game::EncodeAutoPotionWord(e))})
                         .Ok()) {
                    return false;
                }
            }
            return word(14, game::EncodeAutoPotionDelay(settings.delayEnabled, settings.delayMs));
        },
        error);
}

// --- Notices ------------------------------------------------------------------------------------

std::vector<NoticeRow> LatestNotices(Database& db, int count, bool& ok) {
    QueryResult r = db.Query(std::format("SELECT subject, article, {} FROM sro_vt_account._notice ORDER BY editdate DESC, id DESC LIMIT $1",
                                         Epoch("editdate")),
                             {count});
    ok = r.Ok();
    std::vector<NoticeRow> out;
    for (int i = 0; ok && i < r.Rows(); ++i) out.push_back({r.Text(i, 0), r.Text(i, 1), r.Int(i, 2)});
    return out;
}

bool PostNotice(Database& db, std::string_view title, std::string_view body) {
    return db.Query("INSERT INTO sro_vt_account._notice (contentid, subject, article, editdate) VALUES ($3, left($1, 80), left($2, 1024), localtimestamp)",
                    {title, body, kContentId})
        .Ok();
}

} // namespace sr::server

// --- The website's rankings ---------------------------------------------------------------------

namespace sr::server {

Rankings LoadRankings(Database& db, int count, bool& ok) {
    // A player's account, not a game master's (the original's sec_primary 1, and its GMs' "[GM]" names, in
    // case an account was not marked) nor a benchmark bot's.
    static constexpr const char* kPlayer = "c.charid <> 0 AND c.deleted = 0 AND a.sec_primary >= 3 AND lower(a.struserid) !~ '^bot[0-9]+$' "
                                           "AND upper(c.charname16) NOT LIKE '[GM]%'";
    Rankings out;
    ok = false;
    const QueryResult players = db.Query(std::format("SELECT c.charname16, c.curlevel, coalesce(g.name, ''), c.refobjid "
                                                     "FROM _char c JOIN _user u ON u.charid = c.charid "
                                                     "JOIN sro_vt_account.tb_user a ON a.jid = u.userjid "
                                                     "LEFT JOIN _guildmember m ON m.charid = c.charid AND m.guildid <> 0 "
                                                     "LEFT JOIN _guild g ON g.id = m.guildid "
                                                     "WHERE {} ORDER BY c.curlevel DESC, c.expoffset DESC, c.charid LIMIT $1",
                                                     kPlayer),
                                         {count});
    if (!players.Ok()) return out;
    for (int r = 0; r < players.Rows(); ++r) {
        // The player characters' models: Chinese 1907-1932, European 14875-14900 (characterdata).
        const std::int64_t model = players.Int(r, 3);
        std::string detail = players.Text(r, 2);
        if (detail.empty()) detail = model >= 14875 ? "European" : "Chinese";
        out.players.push_back({players.Text(r, 0), int(players.Int(r, 1)), std::move(detail)});
    }
    const QueryResult guilds = db.Query("SELECT g.name, g.lvl, coalesce((SELECT m.charname FROM _guildmember m "
                                        "WHERE m.guildid = g.id AND m.memberclass = 0 LIMIT 1), '') "
                                        "FROM _guild g WHERE g.id <> 0 ORDER BY g.lvl DESC, g.gatheredsp DESC, g.id LIMIT $1",
                                        {count});
    if (!guilds.Ok()) return out;
    for (int r = 0; r < guilds.Rows(); ++r) out.guilds.push_back({guilds.Text(r, 0), int(guilds.Int(r, 1)), guilds.Text(r, 2)});
    // _CharTrijob.JobType: 1 trader, 2 thief, 3 hunter (the original's _QueryOldTrijobInfo).
    const QueryResult jobs = db.Query(std::format("SELECT c.charname16, j.level, j.jobtype "
                                                  "FROM _chartrijob j JOIN _char c ON c.charid = j.charid "
                                                  "JOIN _user u ON u.charid = c.charid JOIN sro_vt_account.tb_user a ON a.jid = u.userjid "
                                                  "WHERE j.jobtype BETWEEN 1 AND 3 AND {} ORDER BY j.level DESC, j.exp DESC, c.charid LIMIT $1",
                                                  kPlayer),
                                      {count});
    if (!jobs.Ok()) return out;
    static constexpr const char* kJob[4] = {"", "Trader", "Thief", "Hunter"};
    for (int r = 0; r < jobs.Rows(); ++r) out.jobs.push_back({jobs.Text(r, 0), int(jobs.Int(r, 1)), kJob[std::clamp<std::int64_t>(jobs.Int(r, 2), 0, 3)]});
    // The uniques' killers (the killing hit's player, sr_unique_events), the last one each killed beside.
    const QueryResult kills = db.Query("SELECT killer_name, count(*), (array_agg(name ORDER BY at DESC))[1] FROM sr_unique_events "
                                       "WHERE event = 2 AND killer_name IS NOT NULL AND upper(killer_name) NOT LIKE '[GM]%' "
                                       "GROUP BY killer_name ORDER BY count(*) DESC, max(at) LIMIT $1",
                                       {count});
    if (!kills.Ok()) return out;
    for (int r = 0; r < kills.Rows(); ++r) out.uniqueKills.push_back({kills.Text(r, 0), int(kills.Int(r, 1)), kills.Text(r, 2)});
    ok = true;
    return out;
}

ShardStats LoadShardStats(Database& db, bool& ok) {
    ShardStats s;
    const QueryResult r = db.Query(
        "SELECT (SELECT count(*) FROM sro_vt_account.tb_user WHERE lower(struserid) !~ '^bot[0-9]+$'), "
        "       (SELECT count(*) FROM _char WHERE charid <> 0 AND deleted = 0), "
        "       (SELECT count(*) FROM _guild WHERE id <> 0), "
        "       (SELECT count(*) FROM sr_unique_events WHERE event = 2), "
        "       (SELECT count(*) FROM sr_unique_events WHERE event = 2 AND at > now() - interval '1 day')");
    ok = r.Ok();
    if (!ok) return s;
    s.accounts = r.Int(0, 0);
    s.characters = r.Int(0, 1);
    s.guilds = r.Int(0, 2);
    s.uniquesKilled = r.Int(0, 3);
    s.uniquesKilledToday = r.Int(0, 4);
    return s;
}

} // namespace sr::server

// --- The unique monsters ----------------------------------------------------------------------------

namespace sr::server {

bool RecordUniqueEvent(Database& db, const UniqueEvent& e) {
    return db
        .Query("INSERT INTO sr_unique_events (ref_obj_id, codename, name, event, server_id, region, pos_x, pos_z, killer_id, killer_name, at) "
               "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, to_timestamp($11))",
               {std::int64_t(e.refObjId), e.codename, e.name.empty() ? e.codename : e.name, int(e.kind), int(e.server), int(std::int16_t(e.region)),
                double(e.x), double(e.z), e.killerId != 0 ? Param(e.killerId) : Param(nullptr),
                e.killerName.empty() ? Param(nullptr) : Param(e.killerName), e.at})
        .Ok();
}

bool ForgetServerUniques(Database& db, std::uint16_t server, double at) {
    return db
        .Query("INSERT INTO sr_unique_events (ref_obj_id, codename, name, event, server_id, region, pos_x, pos_z, at) "
               "SELECT l.ref_obj_id, l.codename, l.name, 3, l.server_id, l.region, l.pos_x, l.pos_z, to_timestamp($2) "
               "FROM (SELECT DISTINCT ON (ref_obj_id) * FROM sr_unique_events ORDER BY ref_obj_id, at DESC, id DESC) l "
               "WHERE l.event = 1 AND l.server_id = $1",
               {int(server), at})
        .Ok();
}

int PruneUniqueEvents(Database& db, int days) {
    const QueryResult r = db.Query(
        "DELETE FROM sr_unique_events WHERE at < now() - make_interval(days => $1) "
        "AND id NOT IN (SELECT DISTINCT ON (ref_obj_id) id FROM sr_unique_events ORDER BY ref_obj_id, at DESC, id DESC) "
        "AND id NOT IN (SELECT DISTINCT ON (ref_obj_id) id FROM sr_unique_events WHERE event = 2 ORDER BY ref_obj_id, at DESC, id DESC)",
        {days});
    return r.Ok() ? r.Affected() : -1;
}

std::vector<UniqueState> LoadUniques(Database& db, bool& ok) {
    std::vector<UniqueState> out;
    const QueryResult r = db.Query(
        "WITH latest AS (SELECT DISTINCT ON (ref_obj_id) ref_obj_id, name, event, at, server_id FROM sr_unique_events ORDER BY ref_obj_id, at DESC, id DESC), "
        "     killed AS (SELECT DISTINCT ON (ref_obj_id) ref_obj_id, killer_name, at FROM sr_unique_events WHERE event = 2 "
        "                ORDER BY ref_obj_id, at DESC, id DESC), "
        "     up AS (SELECT shard_id FROM sro_vt_account.sr_shard_reports WHERE NOT stopped AND reported_at > now() - interval '15 seconds') "
        "SELECT l.name, l.event = 1 AND u.shard_id IS NOT NULL AS alive, floor(extract(epoch FROM l.at))::bigint, coalesce(k.killer_name, ''), "
        "       coalesce(floor(extract(epoch FROM k.at))::bigint, 0) "
        "FROM latest l LEFT JOIN killed k ON k.ref_obj_id = l.ref_obj_id LEFT JOIN up u ON u.shard_id = l.server_id "
        "ORDER BY alive DESC, l.at DESC, l.name");
    ok = r.Ok();
    if (!ok) return out;
    for (int i = 0; i < r.Rows(); ++i) out.push_back({r.Text(i, 0), r.Bool(i, 1), r.Int(i, 2), r.Text(i, 3), r.Int(i, 4)});
    return out;
}

// --- The item mall's services -------------------------------------------------------

std::unordered_map<std::int64_t, std::vector<StoredItem>> LoadCubeItems(Database& db, const std::vector<std::int64_t>& cubes, bool& ok) {
    std::unordered_map<std::int64_t, std::vector<StoredItem>> out;
    ok = true;
    if (cubes.empty()) return out;
    std::string list = "{";
    for (std::size_t k = 0; k < cubes.size(); ++k) list += (k ? "," : "") + std::to_string(cubes[k]);
    list += "}";
    QueryResult q = db.Query(std::format("SELECT l.linkeditemid, l.slot, it.id64, {} FROM _inventoryforlinkedstorage l JOIN _items it ON it.id64 = l.itemid "
                                         "WHERE l.linkeditemid = ANY($1::bigint[]) AND l.itemid <> 0 AND it.refitemid <> 0 ORDER BY l.linkeditemid, l.slot",
                                         ItemSelect("it")),
                             {list});
    if (!q.Ok()) {
        ok = false;
        return out;
    }
    for (int r = 0; r < q.Rows(); ++r) {
        StoredItem item;
        item.slot = std::uint8_t(q.Int(r, 1));
        item.id = q.Int(r, 2);
        ReadItem(q, r, 3, item);
        out[q.Int(r, 0)].push_back(std::move(item));
    }
    return out;
}

bool SaveCubeChange(Database& db, const ExchangeSaveSide& side, std::int64_t cube, std::uint8_t slot, const StoredItem* item, bool into) {
    std::string error;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            if (into) {
                // In the cube first: the bag's save below then leaves it (never frees it).
                if (item == nullptr || !WriteItem(tx, item->id, *item, error)) return false;
                if (!tx.Query("DELETE FROM _inventoryforlinkedstorage WHERE linkeditemid = $1 AND slot = $2", {cube, int(slot)}).Ok() ||
                    !tx.Query("INSERT INTO _inventoryforlinkedstorage (linkeditemid, slot, itemid) VALUES ($1, $2, $3)", {cube, int(slot), item->id}).Ok()) {
                    return false;
                }
            } else if (!tx.Query("DELETE FROM _inventoryforlinkedstorage WHERE linkeditemid = $1 AND slot = $2", {cube, int(slot)}).Ok()) {
                return false;
            }
            return SaveSidesIn(tx, side, side, error, false);
        },
        error);
    if (!ok) core::LogWarning("magic cube {} of character {}: {}", cube, side.character, error);
    return ok;
}

bool SaveCubeUse(Database& db, std::int64_t cube, std::uint8_t slot, std::int64_t item, const StoredItem* left) {
    std::string error;
    const bool ok = db.Transaction(
        [&](Database& tx) {
            if (left != nullptr) return WriteItem(tx, left->id, *left, error);
            return tx.Query("DELETE FROM _inventoryforlinkedstorage WHERE linkeditemid = $1 AND slot = $2", {cube, int(slot)}).Ok() &&
                   tx.Query("DELETE FROM _items WHERE id64 = $1", {item}).Ok();
        },
        error);
    if (!ok) core::LogWarning("magic cube {}: an elixir used: {}", cube, error);
    return ok;
}

bool SaveCharacterModel(Database& db, std::int64_t character, std::uint32_t model, std::uint8_t scale) {
    return db.Query("UPDATE _char SET refobjid = $2, scale = $3 WHERE charid = $1", {character, std::int64_t(model), int(scale)}).Affected() == 1;
}

bool SaveSkillWithdrawal(Database& db, std::int64_t character, std::uint32_t previous, std::uint32_t now) {
    std::string error;
    return db.Transaction(
        [&](Database& tx) {
            if (previous != 0 && !tx.Query("DELETE FROM _charskill WHERE charid = $1 AND skillid = $2", {character, std::int64_t(previous)}).Ok()) return false;
            if (now != 0 && !tx.Query("INSERT INTO _charskill (charid, skillid, enable) VALUES ($1, $2, 1) ON CONFLICT DO NOTHING", {character, std::int64_t(now)}).Ok()) {
                return false;
            }
            return true;
        },
        error);
}

} // namespace sr::server
