#pragma once

// The jobs' trade on the game server: the original's rules (the markets):
//
// - The specialty NPCs' markets (`_ItemQuotation`) live on the game server that holds the NPC: a good's quote falls as
//   it is sold there and climbs back each 10 minutes; the changes are saved; those with the NPC's shop open are told
//   the prices (TradePrices).
// - Buying (the shop's ShopBuy of a specialty good): a trader in the suit with its transport out and within 100 m,
//   level 20, the NPC's trade quest done, bargain goods only in the bargain hours and not again within 10 minutes; the
//   goods go into the transport's bag only (onto the trader's own stack of them, else a free place), made by the
//   trader's alias, at the shop's price.
// - Selling from the transport's bag: a trader or a thief in the suit, the transport within 100 m; at a specialty NPC
//   only goods its alias bought, at the quote (no quote, its own town's goods: their sale price), the market moving by
//   the count; at the thieves' town buyer any goods, at 75 % of their price.
// - The trade scale (stars) of the transport's cargo, told to the trader when it changes (TradeScale).
//
// Job experience from a sale, the pools and safe trade come with later steps.
//
// The game server is its host.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "sr/game/Items.h"
#include "sr/game/JobProtocol.h"
#include "sr/game/Jobs.h"

namespace sr::assets {
class ItemTable;
struct ItemRow;
}

namespace sr::server {

class TradeHost {
public:
    virtual ~TradeHost() = default;
    struct Trader {
        std::uint64_t peer = 0;
        std::int64_t character = 0;
        std::string name;
        int level = 1, highestLevel = 1;
        std::uint64_t gold = 0;
        std::uint8_t jobState = game::kNoJob; // from the suit worn
        std::string alias;
    };
    virtual std::optional<Trader> TradeTrader(std::uint32_t entity) = 0;
    // The codename and RefObjID of the NPC `npc` when it stands within the talk's reach of `entity` (none: too far, or
    // no NPC).
    virtual std::optional<std::pair<std::string, std::uint32_t>> TradeNpcNear(std::uint32_t entity, std::uint32_t npc) const = 0;
    virtual bool TradeQuestDone(std::uint32_t entity, std::string_view quest) const = 0;
    // The shard's PK switch (pk-prohibited): no trader's cargo past three stars.
    virtual bool TradeThreeStarCap() const { return false; }
    // The trader's transport (its entity, its model's codename, its trade bag), none when it has none out.
    struct Cart {
        std::uint32_t entity = 0;
        std::string codename;
        std::map<std::uint8_t, game::ItemInstance>* bag = nullptr;
        int size = 0;
    };
    virtual std::optional<Cart> TradeCart(std::uint32_t owner) = 0;
    virtual void TradeCartChanged(std::uint32_t cart) = 0;
    // Between two entities on the ground (-1: one is not in the world).
    virtual double TradeDistance(std::uint32_t a, std::uint32_t b) const = 0;
    virtual bool TradeTakeGold(std::uint64_t peer, std::uint64_t amount) = 0;
    virtual void TradeGiveGold(std::uint64_t peer, std::uint64_t amount) = 0;
    virtual void TradeSend(std::uint64_t peer, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    virtual const assets::ItemTable* TradeItems() const = 0;
    virtual const game::JobRules& TradeRules() const = 0;
    // The markets' stocks that changed (row id, CurStockAmount), to be saved.
    virtual void TradeSaveStocks(std::vector<std::pair<std::uint32_t, std::int64_t>> stocks) = 0;
    // The fortress tax of the NPC `npc` (its entity) on `amount` for `entity` (0: none),
    // and its collection once a deal is made.
    virtual std::int64_t TradeTaxOn(std::uint32_t entity, std::uint32_t npc, std::uint64_t amount) const = 0;
    virtual void TradeCollectTax(std::uint32_t npc, std::int64_t tax) = 0;
    // A sale made: its gold (less the tax), its payout before the tax, the margin the job
    // experience is reckoned on ((price - 383) x count; 383 x count at the thieves' buyer) and the cargo's stars. The
    // host gives the gold and the experience (a party's shares, the pools); it answers the gold the seller got.
    // A bargain good bought: the 10 minutes' lock-out begun (`until` Unix seconds): kept as a timed job and told.
    virtual void TradeBargainLocked(std::uint32_t entity, std::int64_t until) {
        (void)entity;
        (void)until;
    }
    // The cargo's stars told anew (a change): a trader's safe trade ends on any but one.
    virtual void TradeStarsChanged(std::uint32_t owner, int stars) {
        (void)owner;
        (void)stars;
    }
    virtual std::uint64_t TradeSold(std::uint32_t seller, std::uint64_t gold, std::uint64_t payout, std::uint64_t margin, int stars) {
        (void)margin;
        (void)stars;
        (void)payout;
        if (const auto t = TradeTrader(seller)) TradeGiveGold(t->peer, gold);
        return gold;
    }
    // Goods are in `owner`'s transport (a purchase, its cargo changed): its caravan for the bandits (Bandits.h).
    virtual void TradeCaravan(std::uint32_t owner) { (void)owner; }
    // The market of the NPC `npcRef` as the database has it now (any game server's), then `done` on the host's turn.
    virtual void TradeLoadMarket(std::uint32_t npcRef, std::function<void(std::vector<game::MarketRow>)> done) = 0;
};

class Trades {
public:
    using Clock = std::chrono::steady_clock;

    explicit Trades(TradeHost& host);

    // This server's specialty NPCs' markets (their rows; LoadMarkets).
    void AddMarkets(std::vector<game::MarketRow> rows);
    std::size_t Markets() const { return markets_.size(); }
    // A good's price at the NPC `npcRef` now (none: no quote there).
    std::optional<std::uint32_t> PriceAt(std::uint32_t npcRef, std::uint32_t item) const;

    // A specialty good of the shop at `npc` bought (`count` of it for `price` in all; the shop found and the good
    // priced by the host).
    // `price` is what the trader pays, the fortress tax `tax` in it (collected once the goods are in).
    void Buy(std::uint32_t entity, std::uint32_t npc, const assets::ItemRow& good, int count, std::uint64_t price, Clock::time_point now,
             std::int64_t tax = 0);
    // A request of the player `entity` (TradeSell, TradeSellAll; false: it does not decode).
    bool Handle(std::uint32_t entity, game::proto::MessageId id, std::span<const std::byte> payload);
    static bool IsRequest(game::proto::MessageId id);
    // A shop opened at `npc` (its entity): a specialty NPC's prices told, and again each 10 minutes while it stays near.
    void ShopOpened(std::uint32_t entity, std::uint32_t npc);
    // The trader's cargo changed by other means (its transport gone): its stars told again.
    void CargoChanged(std::uint32_t owner);
    void Left(std::uint32_t entity);
    // A specialty NPC closed by a trader: bargain goods bought there and aboard lock further bargain purchases for 10
    // minutes from now (the original's timed job (7, 0) turned (7, 1) at the close).
    void ShopClosed(std::uint32_t entity, bool bargainAboard, Clock::time_point now);
    // A lock-out kept from before (the character's timed job), `left` of it to run.
    void SetBargainLock(std::uint32_t entity, Clock::duration left) { bargainLock_[entity] = Clock::now() + left; }
    // The markets' 10-minute recovery.
    void Tick(Clock::time_point now);
    // The bargain hours: bargain goods are sold only while they run.
    void SetBargainHours(bool on) { bargainHours_ = on; }
    // A developer's reset of the player's waits (DevReset kWaits): the bargain goods' lock gone.
    void ClearBargainLock(std::uint32_t entity) { bargainLock_.erase(entity); }
    bool BargainHours() const { return bargainHours_; }

private:
    void Sell(std::uint32_t entity, std::uint32_t npc, std::uint8_t place, std::uint16_t count, bool all);
    void Answer(std::uint64_t peer, game::proto::TradeRequest request, game::proto::TradeCode code, std::uint16_t count = 0, std::uint64_t gold = 0);
    void TellPrices(std::uint64_t peer, std::uint32_t npcEntity, std::uint32_t npcRef);
    game::MarketRow* Row(std::uint32_t npcRef, std::uint32_t item);

    TradeHost& host_;
    std::unordered_map<std::uint32_t, std::vector<game::MarketRow>> markets_; // by the NPC's RefObjID
    std::unordered_map<std::uint32_t, std::uint32_t> watching_;               // a player -> the specialty NPC's shop it opened
public:
    // The trader's stars as last told (0: none).
    int StarsOf(std::uint32_t owner) const {
        const auto it = stars_.find(owner);
        return it != stars_.end() ? it->second : 0;
    }

private:
    std::unordered_map<std::uint32_t, std::uint8_t> stars_;                   // a trader -> its stars last told
    std::unordered_map<std::uint32_t, Clock::time_point> bargainLock_;        // a trader -> no bargain goods until then
    std::unordered_set<std::uint32_t> bargainBought_;                          // traders who bought bargain goods this visit
    Clock::time_point nextRecovery_{};
    bool bargainHours_ = false;
};

} // namespace sr::server
