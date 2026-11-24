#include "sr/server/Trades.h"

#include <algorithm>

#include "sr/assets/GameData.h"
#include "sr/core/Log.h"

namespace sr::server {

namespace proto = game::proto;
using proto::TradeCode;
using proto::TradeRequest;

namespace {

// The bargain goods' lockout after a purchase (timed job 7; its text says 10 minutes, the length not traced).
constexpr auto kBargainLock = std::chrono::minutes(10);

} // namespace

Trades::Trades(TradeHost& host) : host_(host) {}

void Trades::AddMarkets(std::vector<game::MarketRow> rows) {
    for (game::MarketRow& r : rows) markets_[r.npc].push_back(r);
}

game::MarketRow* Trades::Row(std::uint32_t npcRef, std::uint32_t item) {
    const auto it = markets_.find(npcRef);
    if (it == markets_.end()) return nullptr;
    for (game::MarketRow& r : it->second) {
        if (r.item == item) return &r;
    }
    return nullptr;
}

std::optional<std::uint32_t> Trades::PriceAt(std::uint32_t npcRef, std::uint32_t item) const {
    const auto it = markets_.find(npcRef);
    if (it == markets_.end()) return std::nullopt;
    for (const game::MarketRow& r : it->second) {
        if (r.item == item) return r.Price();
    }
    return std::nullopt;
}

bool Trades::IsRequest(proto::MessageId id) {
    return id == proto::TradeSell::kId || id == proto::TradeSellAll::kId || id == proto::TradeBoardQuery::kId;
}

bool Trades::Handle(std::uint32_t entity, proto::MessageId id, std::span<const std::byte> payload) {
    if (id == proto::TradeSell::kId) {
        proto::TradeSell m;
        if (!proto::Decode(payload, m)) return false;
        Sell(entity, m.npc, m.place, m.count, false);
        return true;
    }
    if (id == proto::TradeBoardQuery::kId) {
        proto::TradeBoardQuery m;
        if (!proto::Decode(payload, m)) return false;
        const auto t = host_.TradeTrader(entity);
        if (!t) return true;
        // The quotes of any town's market: this server's own from memory, another's from the database.
        const auto answer = [this, peer = t->peer, npcRef = m.npcRef](const std::vector<game::MarketRow>& rows) {
            proto::TradeBoard board;
            board.npcRef = npcRef;
            for (const game::MarketRow& r : rows) board.quotes.push_back({r.item, std::uint16_t(r.Quote())});
            host_.TradeSend(peer, proto::TradeBoard::kId, proto::Encode(board));
        };
        if (const auto here = markets_.find(m.npcRef); here != markets_.end()) {
            answer(here->second);
        } else {
            host_.TradeLoadMarket(m.npcRef, answer);
        }
        return true;
    }
    if (id == proto::TradeSellAll::kId) {
        proto::TradeSellAll m;
        if (!proto::Decode(payload, m)) return false;
        Sell(entity, m.npc, 0, 0, true);
        return true;
    }
    return false;
}

void Trades::Answer(std::uint64_t peer, TradeRequest request, TradeCode code, std::uint16_t count, std::uint64_t gold) {
    host_.TradeSend(peer, proto::TradeAnswer::kId, proto::Encode(proto::TradeAnswer{request, code, count, gold}));
}

void Trades::Buy(std::uint32_t entity, std::uint32_t npc, const assets::ItemRow& good, int count, std::uint64_t price, Clock::time_point now,
                 std::int64_t tax) {
    const auto t = host_.TradeTrader(entity);
    if (!t) return;
    const auto refuse = [&](TradeCode code) {
        Answer(t->peer, TradeRequest::Buy, code);
        core::LogInfo("trade: {} buys {} x{}: refused (0x{:04X})", t->name, good.codename, count, std::uint16_t(code));
    };
    const auto at = host_.TradeNpcNear(entity, npc);
    if (!at) return refuse(TradeCode::NotDealer);
    // The checks in the original's order.
    const auto cart = host_.TradeCart(entity);
    if (t->jobState != std::uint8_t(game::JobType::Trader) || !cart || cart->bag == nullptr || cart->size <= 0) return refuse(TradeCode::NotTrader);
    if (t->level < game::kJobJoinLevel) return refuse(TradeCode::NotTradeLevel);
    if (const std::string_view quest = game::TradeQuestOf(at->first); !quest.empty() && !host_.TradeQuestDone(entity, quest)) return refuse(TradeCode::QuestNotDone);
    if (game::IsBargainGood(good)) {
        if (!bargainHours_) return refuse(TradeCode::NotSellTime);
        if (const auto lock = bargainLock_.find(entity); lock != bargainLock_.end() && now < lock->second) return refuse(TradeCode::BargainLocked);
    }
    const double far = host_.TradeDistance(entity, cart->entity);
    if (far < 0.0 || far > game::kCartReach) return refuse(TradeCode::TooFarFromCart);
    // The shard's PK switch on: the cargo with these goods no more than three stars (0x1893; ours refuses the purchase,
    // the original cuts it to what fits).
    if (host_.TradeThreeStarCap()) {
        std::uint64_t goods = std::uint64_t(std::max(0, count));
        if (const assets::ItemTable* items = host_.TradeItems()) {
            for (const auto& [p, item] : *cart->bag) {
                if (const assets::ItemRow* row = items->Find(item.refId); row != nullptr && game::IsSpecialtyGood(*row)) goods += item.count;
            }
        }
        if (game::CargoStars(host_.TradeRules(), goods, t->highestLevel, cart->codename) > 3) return refuse(TradeCode::MaxTrading);
    }
    if (t->gold < price) return refuse(TradeCode::NotEnoughGold);
    // Into the transport's bag: onto the trader's own stack of the good with room, else the first free place.
    const int stack = std::max(1, int(good.maxStack));
    std::map<std::uint8_t, game::ItemInstance>& bag = *cart->bag;
    int place = -1;
    for (const auto& [p, item] : bag) {
        if (item.refId == good.refObjId && item.creator == t->alias && int(item.count) + count <= stack) {
            place = p;
            break;
        }
    }
    if (place < 0) {
        for (int p = 0; p < cart->size; ++p) {
            if (!bag.contains(std::uint8_t(p))) {
                place = p;
                break;
            }
        }
    }
    if (place < 0) return refuse(TradeCode::CartFull);
    if (!host_.TradeTakeGold(t->peer, price)) return refuse(TradeCode::NotEnoughGold);
    game::ItemInstance& item = bag[std::uint8_t(place)];
    if (item.refId == 0) {
        item.refId = good.refObjId;
        item.count = 0;
        item.creator = t->alias; // its buyer's alias
    }
    item.count = std::uint16_t(item.count + count);
    // Bargain goods bought: the lock comes when the trader closes the NPC with them aboard (ShopClosed), not now.
    if (game::IsBargainGood(good)) bargainBought_.insert(entity);
    if (tax != 0) host_.TradeCollectTax(npc, tax);
    core::LogInfo("trade: {} buys {} x{} for {} into its transport (place {})", t->name, good.codename, count, price, place);
    host_.TradeCartChanged(cart->entity);
    Answer(t->peer, TradeRequest::Buy, TradeCode::Ok, std::uint16_t(count), price);
    CargoChanged(entity);
}

void Trades::Sell(std::uint32_t entity, std::uint32_t npc, std::uint8_t place, std::uint16_t count, bool all) {
    const auto t = host_.TradeTrader(entity);
    if (!t) return;
    const auto refuse = [&](TradeCode code) {
        Answer(t->peer, TradeRequest::Sell, code);
        core::LogInfo("trade: {} sells: refused (0x{:04X})", t->name, std::uint16_t(code));
    };
    // The checks in the original's order.
    if (t->jobState != std::uint8_t(game::JobType::Trader) && t->jobState != std::uint8_t(game::JobType::Thief)) return refuse(TradeCode::NotSeller);
    const auto at = host_.TradeNpcNear(entity, npc);
    const bool stolenBuyer = at && game::IsStolenGoodsBuyer(at->first);
    if (!at || (!game::IsSpecialtyNpc(at->first) && !stolenBuyer)) return refuse(TradeCode::NotDealer);
    if (const std::string_view quest = game::TradeQuestOf(at->first); !quest.empty() && !host_.TradeQuestDone(entity, quest)) return refuse(TradeCode::QuestNotDone);
    const auto cart = host_.TradeCart(entity);
    const assets::ItemTable* items = host_.TradeItems();
    if (!cart || cart->bag == nullptr || items == nullptr) return refuse(TradeCode::NoItem);
    std::map<std::uint8_t, game::ItemInstance>& bag = *cart->bag;
    // What goes: one place's stack (or part of it), or every specialty good the seller may sell here.
    std::vector<std::pair<std::uint8_t, std::uint16_t>> going;
    TradeCode first = TradeCode::NoItem;
    for (const auto& [p, item] : bag) {
        if (!all && p != place) continue;
        const assets::ItemRow* row = items->Find(item.refId);
        if (row == nullptr || !game::IsSpecialtyGood(*row)) continue;
        if (!stolenBuyer && item.creator != t->alias) {
            first = TradeCode::StolenGoods;
            continue;
        }
        going.emplace_back(p, all || count == 0 ? item.count : std::min(count, item.count));
    }
    if (going.empty()) return refuse(first);
    const double far = host_.TradeDistance(entity, cart->entity);
    if (far < 0.0 || far > game::kCartReach) return refuse(TradeCode::TooFarFromCart);
    // The payout: the quote (the price before each sale), its own town's goods their sale price, the thieves' buyer
    // 75 % of the goods' price; the market moves by the count.
    std::uint64_t gold = 0, margin = 0;
    std::uint32_t sold = 0;
    const int stars = stars_.contains(entity) ? stars_[entity] : 0; // the cargo's before the sale
    std::vector<std::pair<std::uint32_t, std::int64_t>> stocks;
    for (const auto& [p, n] : going) {
        game::ItemInstance& item = bag[p];
        if (stolenBuyer) {
            gold += game::StolenGoodsPayout(n);
            margin += std::uint64_t(game::kGoodsPrice) * n;
        } else if (game::MarketRow* row = Row(at->second, item.refId)) {
            const std::uint32_t price = row->Price();
            gold += std::uint64_t(price) * n;
            if (price > game::kGoodsPrice) margin += std::uint64_t(price - game::kGoodsPrice) * n;
            row->Sold(n);
            stocks.emplace_back(row->id, row->stock);
        } else {
            gold += std::uint64_t(game::kGoodsSellPrice) * n;
        }
        sold += n;
        item.count = std::uint16_t(item.count - n);
        if (item.count == 0) bag.erase(p);
    }
    // Less the fortress tax of the NPC's town.
    const std::uint64_t payout = gold;
    const std::int64_t tax = host_.TradeTaxOn(entity, npc, gold);
    gold = tax >= 0 ? gold - std::min<std::uint64_t>(gold, std::uint64_t(tax)) : gold + std::uint64_t(-tax);
    if (tax != 0) host_.TradeCollectTax(npc, tax);
    gold = host_.TradeSold(entity, gold, payout, margin, stars); // the gold and the job experience (a party's shares, the pools)
    if (!stocks.empty()) host_.TradeSaveStocks(std::move(stocks));
    core::LogInfo("trade: {} sells {} goods at {} for {}", t->name, sold, at->first, gold);
    host_.TradeCartChanged(cart->entity);
    Answer(t->peer, TradeRequest::Sell, TradeCode::Ok, std::uint16_t(std::min<std::uint32_t>(sold, 0xFFFF)), gold);
    CargoChanged(entity);
}

void Trades::CargoChanged(std::uint32_t owner) {
    const auto t = host_.TradeTrader(owner);
    if (!t) return;
    const auto cart = host_.TradeCart(owner);
    const assets::ItemTable* items = host_.TradeItems();
    std::uint64_t goods = 0;
    if (cart && cart->bag != nullptr && items != nullptr) {
        for (const auto& [p, item] : *cart->bag) {
            if (const assets::ItemRow* row = items->Find(item.refId); row != nullptr && game::IsSpecialtyGood(*row)) goods += item.count;
        }
    }
    const auto stars = std::uint8_t(cart ? game::CargoStars(host_.TradeRules(), goods, t->highestLevel, cart->codename) : 0);
    if (goods > 0) host_.TradeCaravan(owner); // the bandits' caravan
    const auto told = stars_.find(owner);
    if (told != stars_.end() && told->second == stars) return;
    stars_[owner] = stars;
    host_.TradeSend(t->peer, proto::TradeScale::kId, proto::Encode(proto::TradeScale{stars}));
    host_.TradeStarsChanged(owner, stars);
}

void Trades::TellPrices(std::uint64_t peer, std::uint32_t npcEntity, std::uint32_t npcRef) {
    const auto it = markets_.find(npcRef);
    if (it == markets_.end()) return;
    proto::TradePrices m;
    m.npc = npcEntity;
    for (const game::MarketRow& r : it->second) m.prices.push_back({r.item, r.Price()});
    host_.TradeSend(peer, proto::TradePrices::kId, proto::Encode(m));
}

void Trades::ShopOpened(std::uint32_t entity, std::uint32_t npc) {
    const auto t = host_.TradeTrader(entity);
    const auto at = host_.TradeNpcNear(entity, npc);
    if (!t || !at || !markets_.contains(at->second)) return;
    watching_[entity] = npc;
    TellPrices(t->peer, npc, at->second);
}

void Trades::Left(std::uint32_t entity) {
    watching_.erase(entity);
    stars_.erase(entity);
    bargainLock_.erase(entity);
    bargainBought_.erase(entity);
}

void Trades::ShopClosed(std::uint32_t entity, bool bargainAboard, Clock::time_point now) {
    if (bargainBought_.erase(entity) == 0 || !bargainAboard) return;
    bargainLock_[entity] = now + kBargainLock;
    const std::int64_t until = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() +
                               std::chrono::duration_cast<std::chrono::seconds>(kBargainLock).count();
    host_.TradeBargainLocked(entity, until);
}

void Trades::Tick(Clock::time_point now) {
    if (nextRecovery_ == Clock::time_point{}) nextRecovery_ = now + std::chrono::seconds(game::kMarketTickSeconds);
    if (now < nextRecovery_) return;
    nextRecovery_ = now + std::chrono::seconds(game::kMarketTickSeconds);
    // Each market's stocks fall back toward their rest (every 600,000 ms); the changed ones saved.
    std::vector<std::pair<std::uint32_t, std::int64_t>> stocks;
    for (auto& [npc, rows] : markets_) {
        for (game::MarketRow& r : rows) {
            const std::int64_t before = r.stock;
            r.Recover();
            if (r.stock != before) stocks.emplace_back(r.id, r.stock);
        }
    }
    if (!stocks.empty()) host_.TradeSaveStocks(std::move(stocks));
    // Those with a specialty NPC's shop open (and still at it) hear its prices (0x30E0).
    for (auto it = watching_.begin(); it != watching_.end();) {
        const auto t = host_.TradeTrader(it->first);
        const auto at = t ? host_.TradeNpcNear(it->first, it->second) : std::nullopt;
        if (!t || !at) {
            it = watching_.erase(it);
            continue;
        }
        TellPrices(t->peer, it->second, at->second);
        ++it;
    }
}

} // namespace sr::server
