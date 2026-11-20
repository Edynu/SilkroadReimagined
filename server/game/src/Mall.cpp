#include "sr/server/Mall.h"

#include <ctime>
#include <memory>

#include "sr/core/Log.h"
#include "sr/game/Mall.h"
#include "sr/server/Database.h"

namespace sr::server {

namespace proto = game::proto;
namespace code = proto::mallcode;

Mall::Mall(MallHost& host, std::shared_ptr<const assets::MallTable> catalog) : host_(host), catalog_(std::move(catalog)) {}

bool Mall::IsRequest(proto::MessageId id) {
    return id == proto::MallOpen::kId || id == proto::MallBuy::kId || id == proto::MallBuyMany::kId || id == proto::SilkHistoryQuery::kId;
}

bool Mall::Handle(std::uint32_t entity, proto::MessageId id, std::span<const std::byte> payload) {
    switch (id) {
    case proto::MallOpen::kId: {
        proto::MallOpen m;
        if (!proto::Decode(payload, m)) return false;
        Open(entity);
        return true;
    }
    case proto::MallBuy::kId: {
        proto::MallBuy m;
        if (!proto::Decode(payload, m)) return false;
        Buy(entity, {{m.tab, m.slot, m.package, m.count}}, m.points);
        return true;
    }
    case proto::MallBuyMany::kId: {
        proto::MallBuyMany m;
        if (!proto::Decode(payload, m)) return false;
        Buy(entity, m.goods, m.points);
        return true;
    }
    case proto::SilkHistoryQuery::kId: {
        proto::SilkHistoryQuery m;
        if (!proto::Decode(payload, m)) return false;
        History(entity, m);
        return true;
    }
    default: return false;
    }
}

std::optional<SilkBalance> Mall::Balance(std::uint32_t entity) const {
    const auto it = accounts_.find(entity);
    if (it == accounts_.end() || !it->second.known) return std::nullopt;
    return it->second.balance;
}

void Mall::TellBalance(std::uint32_t entity) {
    const auto buyer = host_.MallBuyer(entity);
    const auto it = accounts_.find(entity);
    if (!buyer || it == accounts_.end()) return;
    const SilkBalance& b = it->second.balance;
    host_.MallSend(buyer->peer, proto::SilkBalanceMessage::kId, proto::Encode(proto::SilkBalanceMessage{b.own, b.gift, b.point}));
}

void Mall::Entered(std::uint32_t entity, std::int64_t account) {
    accounts_[entity] = {account, {}, false};
    auto read = std::make_shared<std::pair<bool, SilkBalance>>();
    host_.MallDatabase(
        [read, account](Database& db) {
            std::string error;
            read->first = LoadSilk(db, account, read->second, error);
            if (!read->first) core::LogWarning("mall: the silk of account {}: {}", account, error);
        },
        [this, read, entity, account] {
            const auto it = accounts_.find(entity);
            if (it == accounts_.end() || it->second.account != account || !read->first) return;
            it->second.balance = read->second;
            it->second.known = true;
            TellBalance(entity);
        });
}

void Mall::Left(std::uint32_t entity) {
    accounts_.erase(entity);
    waiting_.erase(entity);
}

void Mall::BusMessage(const std::string& payload) {
    const auto notice = DecodeSilkNotice(payload);
    if (!notice) return;
    for (const auto& [entity, a] : accounts_) {
        if (a.account != notice->account) continue;
        auto read = std::make_shared<std::pair<bool, SilkBalance>>();
        const std::uint32_t who = entity;
        const std::int64_t account = a.account;
        host_.MallDatabase(
            [read, account](Database& db) {
                std::string error;
                read->first = LoadSilk(db, account, read->second, error);
            },
            [this, read, who, account, n = *notice] {
                const auto it = accounts_.find(who);
                const auto buyer = host_.MallBuyer(who);
                if (it == accounts_.end() || it->second.account != account || !read->first || !buyer) return;
                it->second.balance = read->second;
                it->second.known = true;
                TellBalance(who);
                const std::uint32_t value = std::uint32_t(std::min<std::int64_t>(n.value < 0 ? -n.value : n.value, 0xFFFFFFFF));
                host_.MallSend(buyer->peer, proto::SilkLine::kId, proto::Encode(proto::SilkLine{std::uint8_t(n.reason), std::uint8_t(n.kind), value}));
            });
    }
}

void Mall::Open(std::uint32_t entity) {
    const auto buyer = host_.MallBuyer(entity);
    if (!buyer) return;
    auto stock = std::make_shared<std::vector<std::pair<std::string, int>>>();
    host_.MallDatabase(
        [stock](Database& db) {
            std::string error;
            if (!LoadStock(db, assets::MallTable::kGroup, *stock, error)) core::LogWarning("mall: the stock: {}", error);
        },
        [this, stock, entity, character = buyer->character] {
            const auto again = host_.MallBuyer(entity);
            if (!again || again->character != character) return;
            proto::MallState state;
            for (const auto& [package, left] : *stock) {
                for (const assets::MallShop& s : catalog_->shops()) {
                    for (const assets::MallTab& t : s.tabs) {
                        for (const assets::MallGood& g : t.goods) {
                            if (g.package == package) state.stock.push_back({g.packageId, std::uint16_t(std::clamp(left, 0, 0xFFFF))});
                        }
                    }
                }
            }
            host_.MallSend(again->peer, proto::MallState::kId, proto::Encode(state));
            TellBalance(entity);
        });
}

void Mall::Refuse(std::uint64_t peer, std::uint16_t c, const std::vector<proto::MallBuyLine>& goods) {
    proto::MallBought answer;
    answer.code = c;
    answer.goods = goods;
    host_.MallSend(peer, proto::MallBought::kId, proto::Encode(answer));
}

void Mall::Buy(std::uint32_t entity, const std::vector<proto::MallBuyLine>& lines, std::uint32_t points) {
    const auto buyer = host_.MallBuyer(entity);
    if (!buyer || waiting_.contains(entity)) return;
    if (lines.empty()) return;
    const auto refuse = [&](std::uint16_t c) { Refuse(buyer->peer, c, lines); };
    if (host_.MallBagHeld(entity)) return refuse(code::kBusy);
    // Each good as the original checks it: the tab, the good, the package.
    struct Line {
        const assets::MallGood* good = nullptr;
        const assets::ItemRow* row = nullptr;
        int count = 1;
    };
    std::vector<Line> goods;
    std::vector<proto::MallBuyLine> told;
    int places = 0;
    std::uint64_t price = 0;
    for (const proto::MallBuyLine& l : lines) {
        const assets::MallTab* tab = catalog_->Tab(l.tab);
        if (tab == nullptr) return refuse(code::kNoTab);
        const assets::MallGood* good = catalog_->Find(l.tab, l.slot);
        if (good == nullptr) return refuse(code::kNoGood);
        if (good->packageId != l.package) return refuse(code::kWrongGood);
        const assets::ItemRow* row = host_.MallItem(good->itemCodename);
        if (row == nullptr || l.count == 0) return refuse(code::kFailed);
        Line g{good, row, game::MallClampCount(*good, *row, l.count)};
        places += game::MallPlacesNeeded(*good, *row, g.count);
        price += game::MallPrice(*good, g.count);
        goods.push_back(g);
        told.push_back({l.tab, l.slot, l.package, std::uint16_t(g.count)});
    }
    if (host_.MallFreePlaces(entity) < places) return refuse(code::kBagFull);
    // The silk known here (the database decides: another server or the site may have changed it meanwhile).
    const auto a = accounts_.find(entity);
    if (a == accounts_.end() || !a->second.known) return refuse(code::kFailed);
    const SilkBalance& have = a->second.balance;
    if (std::uint64_t(have.own) + have.gift + std::min(points, have.point) < price || points > have.point) return refuse(code::kNoSilk);
    // The items into the bag now, held until the database says.
    std::vector<game::ItemInstance> items;
    for (const Line& g : goods) {
        for (game::ItemInstance& i : game::MallItems(*g.good, *g.row, g.count, host_.MallNow())) items.push_back(std::move(i));
    }
    const std::vector<std::uint8_t> put = host_.MallPut(entity, std::move(items));
    if (put.empty()) return refuse(code::kBagFull);
    waiting_.insert(entity);
    struct Outcome {
        std::uint16_t code = code::kFailed;
        SilkBalance after;
    };
    auto outcome = std::make_shared<Outcome>();
    const std::int64_t account = buyer->account, character = buyer->character;
    const int shard = host_.MallShardId();
    const bool fail = std::exchange(failNextPurchase, false);
    std::vector<std::pair<std::string, std::pair<std::uint32_t, std::pair<std::uint64_t, int>>>> work; // package, id, price, count
    for (const Line& g : goods) work.push_back({g.good->package, {g.good->packageId, {game::MallPrice(*g.good, g.count), g.count}}});
    host_.MallSave(
        entity, put,
        [outcome, work, account, character, shard, points, fail](Database& tx, std::string& error) {
            std::uint32_t pointsLeft = points;
            for (const auto& [package, rest] : work) {
                const auto& [id, priced] = rest;
                const auto& [cost, count] = priced;
                bool soldOut = false;
                if (!TakeStockIn(tx, assets::MallTable::kGroup, package, count, soldOut, error)) {
                    if (soldOut) outcome->code = code::kSoldOut;
                    return false;
                }
                SilkBalance paid;
                if (!SpendSilkIn(tx, account, cost, pointsLeft, outcome->after, paid, error)) {
                    if (error.empty()) outcome->code = code::kNoSilk;
                    return false;
                }
                pointsLeft -= paid.point;
                if (!LogMallSaleIn(tx, account, shard, character, id, paid, error)) return false;
            }
            if (fail) {
                error = "a test's failure";
                return false;
            }
            outcome->code = code::kOk;
            return true;
        },
        [this, outcome, entity, character, put, told](bool ok) {
            waiting_.erase(entity);
            const auto again = host_.MallBuyer(entity);
            if (!again || again->character != character) return; // gone: what the database holds is what it has
            if (!ok || outcome->code != code::kOk) {
                host_.MallTakeBack(entity, put);
                Refuse(again->peer, outcome->code == code::kOk ? code::kFailed : outcome->code, told);
                return;
            }
            if (const auto it = accounts_.find(entity); it != accounts_.end()) {
                it->second.balance = outcome->after;
                it->second.known = true;
            }
            proto::MallBought answer;
            answer.goods = told;
            for (std::uint8_t place : put) {
                if (const auto r = host_.MallRecord(entity, place)) answer.items.push_back(*r);
            }
            core::LogInfo("mall: {} bought {} goods into {} places; silk now {} / {} / {}", again->name, told.size(), put.size(), outcome->after.own,
                          outcome->after.gift, outcome->after.point);
            host_.MallSend(again->peer, proto::MallBought::kId, proto::Encode(answer));
            TellBalance(entity);
            host_.MallBagChanged(entity);
        });
}

void Mall::History(std::uint32_t entity, const proto::SilkHistoryQuery& q) {
    const auto buyer = host_.MallBuyer(entity);
    if (!buyer) return;
    const auto a = accounts_.find(entity);
    if (a == accounts_.end()) return;
    if (q.period == 0xFF) return; // the original's cancel (closing the mall during an inquiry)
    auto rows = std::make_shared<std::pair<bool, std::vector<SilkHistoryRow>>>();
    const int list = q.list == 3 ? 3 : q.list == 2 ? 2 : 1, period = q.period;
    host_.MallDatabase(
        [rows, account = a->second.account, list, period](Database& db) {
            std::string error;
            rows->first = LoadSilkHistory(db, account, list, period, rows->second, error);
        },
        [this, rows, entity, character = buyer->character, list] {
            const auto again = host_.MallBuyer(entity);
            if (!again || again->character != character) return;
            proto::SilkHistory answer;
            answer.list = std::uint8_t(list);
            answer.result = rows->first ? 0 : 1;
            for (const SilkHistoryRow& r : rows->second) {
                const std::time_t t = std::time_t(r.when);
                std::tm tm{};
#ifdef _WIN32
                gmtime_s(&tm, &t);
#else
                gmtime_r(&t, &tm);
#endif
                answer.rows.push_back({std::uint32_t(std::max<std::int64_t>(0, r.silk)),
                                       proto::PackSilkDate(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min)});
                if (answer.rows.size() >= 1000) break;
            }
            host_.MallSend(again->peer, proto::SilkHistory::kId, proto::Encode(answer));
        });
}

} // namespace sr::server
