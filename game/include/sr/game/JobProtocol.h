#pragma once

// The jobs over the network: our own messages (0x1320-0x135F) for what the original's carry
// (its league requests 0x70E1..0x70E6 and their answers 0xB0E1..0xB0E6, the job's experience 0x30E6, a player's job
// state in its spawn and 0x30CF). The refusals keep the original's codes, which the client turns into its texts.

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "sr/game/Protocol.h"

namespace sr::game::proto {

// --- The league at its NPC -----------------------------------------------------------------

// The NPC functions the original gives by codename (game::LeagueOfNpc): its rows in the NPC window.
constexpr std::uint32_t kNpcJobTrader = 0x800, kNpcJobThief = 0x1000, kNpcJobHunter = 0x2000;
// A specialty NPC (function 0x0C): its row 0x39 "Trade Export Details" opens the price board.
constexpr std::uint32_t kNpcTrade = 0x4000;

// The refusals: the original's codes (its job error table), which the client turns into its texts.
enum class JobCode : std::uint16_t {
    Ok = 0,
    InvalidTarget = 3,         // the NPC not in reach or not the league's (UIIT_STT_ERR_COMMON_INVALID_TARGET)
    NotEnoughGold = 0x4807,    // the fee (join, an alias's change)
    NotOffered = 0x480F,       // the NPC keeps another league
    LevelTooLow = 0x4819,      // under level 20
    InALeague = 0x4820,        // already in one
    SuitWorn = 0x4822,         // the suit is on (leave, the alias)
    NameNotAllowed = 0x4823,   // the alias's characters or length
    RejoinWait = 0x4824,       // within 7 days of leaving
    NameIsCharacter = 0x4825,  // a character has that name
    NameIsAlias = 0x4826,      // another alias is that name
    NotInLeague = 0x4828,      // in none (leave, the alias), or the type is no league's
    NoPrevious = 0x4829,       // no previous job information
    NoOutcome = 0x482A,        // nothing to collect
    QuestUnderWay = 0x482E,    // a job quest under way (leave)
    Failed = 0xFFFF,           // ours: the database refused
};

// What a JobAnswer answers (the original's answers 0xB0E1 join, 0xB0E2 leave, 0xB0E3 the alias, 0xB0E6 the
// previous information).
enum class JobRequest : std::uint8_t { Join = 1, Leave = 2, Alias = 3, Rank = 4, Outcome = 5, Previous = 6 };

// A league's ranking at its keeper (the original's 0x70E4 {npc, type, kind}): the NPC's league
// `type`, `kind` 0 activity, 1 the week's donation / contribution. JobRanking; refusals JobAnswer (3, 0x4828, 0x480F).
struct JobRankQuery {
    static constexpr MessageId kId = 0x1327;
    std::uint32_t npc = 0;
    std::uint8_t type = 0, kind = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("type", type);
        v("kind", kind);
    }
};
// The week's outcome at a thief's or hunter's keeper (0x70E5 {npc, action}): action 0 asks how much, 1 collects it.
// JobOutcomeAnswer; refusals JobAnswer (3, 0x4828, 0x480F, 0x482A), the last three silent on the client.
struct JobOutcome {
    static constexpr MessageId kId = 0x1328;
    std::uint32_t npc = 0;
    std::uint8_t action = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("action", action);
    }
};

// Joining the NPC's league of `type` (0x70E1). JobAnswer; on success the new JobInfoMessage and the fee's gold.
struct JobJoin {
    static constexpr MessageId kId = 0x1320;
    std::uint32_t npc = 0;
    std::uint8_t type = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("type", type);
    }
};

// Leaving the league (0x70E2). JobAnswer; on success the JobInfoMessage of no league.
struct JobLeave {
    static constexpr MessageId kId = 0x1321;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};

// The alias (0x70E3): `action` 0 asks whether the name is free, 1 sets it (a change costs level x 50,000). JobAnswer
// with the action; a set's success also brings the JobInfoMessage.
constexpr std::uint8_t kAliasCheck = 0, kAliasSet = 1;
struct JobAlias {
    static constexpr MessageId kId = 0x1322;
    std::uint32_t npc = 0;
    std::uint8_t action = kAliasCheck;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("action", action);
        v("name", name, 32);
    }
};

// The previous job information (0x70E6): JobPrevious, or JobAnswer with NoPrevious.
struct JobPreviousQuery {
    static constexpr MessageId kId = 0x1323;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};

// A league request's answer: Ok, or the refusal's code. `action` the alias's (0 check, 1 set).
struct JobAnswer {
    static constexpr MessageId kId = 0x1342;
    JobRequest request = JobRequest::Join;
    JobCode code = JobCode::Ok;
    std::uint8_t action = 0;
    template <typename V>
    void Visit(V& v) {
        v("request", request);
        v("code", code);
        v("action", action);
    }
};

// The levels and experience of the three jobs before the leagues (`_OldTrijob`; the window CIFPrevJobInfo).
struct JobPrevious {
    static constexpr MessageId kId = 0x1343;
    std::uint8_t traderLevel = 1, thiefLevel = 1, hunterLevel = 1;
    std::uint32_t traderExp = 0, thiefExp = 0, hunterExp = 0;
    template <typename V>
    void Visit(V& v) {
        v("traderLevel", traderLevel);
        v("traderExp", traderExp);
        v("thiefLevel", thiefLevel);
        v("thiefExp", thiefExp);
        v("hunterLevel", hunterLevel);
        v("hunterExp", hunterExp);
    }
};

// --- Trade ----------------------------------------------------------------------------------
//
// Buying is the shop's own (ShopBuy at a specialty NPC: the goods go into the trader's transport, never the bag); the
// transport's bag is the COS's (CosBag). Selling is from that bag.

// The refusals: the original's codes, which the client turns into its texts.
enum class TradeCode : std::uint16_t {
    Ok = 0,
    NotSeller = 5,             // selling: not a trader or a thief in the suit
    NoItem = 0xF001,           // ours: nothing at that place of the transport's bag
    CartFull = 0xF002,         // ours: no room in the transport's bag
    NotEnoughGold = 0xF003,    // ours: the goods cost more than the trader has
    NotDealer = 0x1845,        // the NPC does not deal in these (no specialty NPC; its own town's goods it takes)
    NotTradeLevel = 0x184E,    // under level 20 (UIIT_MSG_SPECIALTY_ERR_NOTTRADE)
    NotTrader = 0x1860,        // buying: a trader in the suit with a transport out (UIIT_MSG_STRGERR_ONLY_TRADER_CAN_BUY_SPECIALTY)
    TooFarFromCart = 0x1865,   // the transport farther than 100 m
    StolenGoods = 0x1868,      // goods someone else bought: only the thieves' town buyer takes them
    NotSellTime = 0x18AF,      // bargain goods outside the bargain hours
    QuestNotDone = 0x18B0,     // the NPC's trade quest not done
    BargainLocked = 0x18D1,    // bargain goods again too soon after a purchase
    MaxTrading = 0x1893,       // the shard's PK switch on: no cargo past three stars (UIIT_MSG_STRGERR_MAX_TRADING_LIMIT_EXCEEDED)
};

enum class TradeRequest : std::uint8_t { Buy = 1, Sell = 2 };

// Goods sold from the transport's bag at a specialty NPC or the thieves' town buyer (the original's item operation
// from a COS's bag): `count` 0 the whole stack. TradeAnswer; the gold in GoldChanged, the bag in CosBag.
struct TradeSell {
    static constexpr MessageId kId = 0x1324;
    std::uint32_t npc = 0;
    std::uint8_t place = 0;
    std::uint16_t count = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("place", place);
        v("count", count);
    }
};

// Every specialty good of the transport's bag sold at once (the client's "Sell All").
struct TradeSellAll {
    static constexpr MessageId kId = 0x1325;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};

// A purchase's or a sale's answer: Ok, or the refusal's code; the goods' count and what they cost or brought.
struct TradeAnswer {
    static constexpr MessageId kId = 0x1345;
    TradeRequest request = TradeRequest::Buy;
    TradeCode code = TradeCode::Ok;
    std::uint16_t count = 0;
    std::uint64_t gold = 0;
    template <typename V>
    void Visit(V& v) {
        v("request", request);
        v("code", code);
        v("count", count);
        v("gold", gold);
    }
};

// A specialty NPC's prices now (the original's 0x30E0 {count, (RefItemID, u32 price)...}): told when its shop opens and
// every 10 minutes to those it is open for.
struct TradePrice {
    std::uint32_t item = 0;
    std::uint32_t price = 0;
    template <typename V>
    void Visit(V& v) {
        v("item", item);
        v("price", price);
    }
};
struct TradePrices {
    static constexpr MessageId kId = 0x1346;
    std::uint32_t npc = 0;
    std::vector<TradePrice> prices;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("prices", prices, 64);
    }
};

// The trader's trade scale (the original's 0x30E8 {u8 stars}): 0 without goods, 1..5; told whenever the cargo changes.
// Ours also its rule, for the deal window's scale of a count typed (1.188's client works it out from its own tables):
// the goods aboard, the trader's gold unit (game::JobRules::GoldUnit), the transport's capacity, the scale's first four
// bounds and the shard's three-star cap.
struct TradeScale {
    static constexpr MessageId kId = 0x1347;
    std::uint8_t stars = 0;
    std::uint32_t goods = 0, unit = 0;
    std::uint8_t capacity = 0, threeStarCap = 0;
    std::vector<std::uint32_t> scale;
    template <typename V>
    void Visit(V& v) {
        v("stars", stars);
        v("goods", goods);
        v("unit", unit);
        v("capacity", capacity);
        v("threeStarCap", threeStarCap);
        v("scale", scale, 5);
    }
};

// The price board (CIFTradeInfoWnd): a trade centre's quotes, asked by its NPC's RefObjID
// (the original's 0x74D4); answered with TradeBoard (0xB4D4: its goods and their quotes in hundredths). Any town's
// board, whichever game server holds its NPC (read from the database's markets).
struct TradeBoardQuery {
    static constexpr MessageId kId = 0x1326;
    std::uint32_t npcRef = 0;
    template <typename V>
    void Visit(V& v) {
        v("npcRef", npcRef);
    }
};
struct TradeQuote {
    std::uint32_t item = 0;
    std::uint16_t quote = 0;
    template <typename V>
    void Visit(V& v) {
        v("item", item);
        v("quote", quote);
    }
};
struct TradeBoard {
    static constexpr MessageId kId = 0x1349;
    std::uint32_t npcRef = 0;
    std::vector<TradeQuote> quotes;
    template <typename V>
    void Visit(V& v) {
        v("npcRef", npcRef);
        v("quotes", quotes, 64);
    }
};

// The bargain goods' sale hours told to everyone (UIIT_MSG_SPECIALTY_ANNOUNCE_SELL_READY 30 minutes before, _START,
// _END): `moment` 1 ready, 2 started, 3 over.
struct TradeBargainNotice {
    static constexpr MessageId kId = 0x1348;
    std::uint8_t moment = 0;
    template <typename V>
    void Visit(V& v) {
        v("moment", moment);
    }
};

// --- The job's state ----------------------------------------------------------------------------------------------

// The player's own league (game::JobInfo; the original's character data and 0xB0E1): told on entering the world
// and whenever it changes. type 0: in none.
struct JobInfoMessage {
    static constexpr MessageId kId = 0x1340;
    std::uint8_t type = 0;
    std::uint8_t level = 1;
    std::uint64_t exp = 0;
    std::uint32_t contribution = 0, reward = 0;
    std::string alias;
    // The character's data on entering the world (the original's 0x3013): its contribution then is what the
    // contribution ranking's own entry shows all session.
    bool entry = false;
    template <typename V>
    void Visit(V& v) {
        v("type", type);
        v("level", level);
        v("exp", exp);
        v("contribution", contribution);
        v("reward", reward);
        v("alias", alias, 32);
        v("entry", entry);
    }
};

// A player's job as others see it (the original's spawn fields and 0x30CF): its state from the suit worn (1 trader,
// 2 thief, 3 hunter; game::kNoJob none), its job level and its alias, which those near show in place of its name
// while the suit is on. Told with its appearance (World::SetExtra) and when the suit goes on or comes off.
struct EntityJob {
    static constexpr MessageId kId = 0x1341;
    std::uint32_t entity = 0;
    std::uint8_t state = 4;
    std::uint8_t level = 0;
    std::string alias;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("state", state);
        v("level", level);
        v("alias", alias, 32);
    }
};

// A player's job suit going on (`on` 1) or coming off (0) in `seconds` (the original's 0x3041 {entity, 2, 2, 10} to
// those near: the change's bar), to the player and those near; seconds 0: the change is over (made, or given up).
struct EntityJobChange {
    static constexpr MessageId kId = 0x1344;
    std::uint32_t entity = 0;
    std::uint8_t on = 1;
    std::uint16_t seconds = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("on", on);
        v("seconds", seconds);
    }
};

// The player's job experience changed (the original's 0x30E6 {type, level, u32 exp}): `delta` gained
// (positive) or lost (negative), its level and experience now, whether the level rose, and the gold a sale donated to
// the hunters' league (its own line, `UIIT_MSG_SPECIALTY_CONTRIBUTION_HUNTERGUILD`; 0: none).
struct JobExpChanged {
    static constexpr MessageId kId = 0x134A;
    std::uint8_t type = 0;
    std::uint8_t level = 1;
    std::int64_t delta = 0;
    std::uint64_t exp = 0;
    bool levelUp = false;
    std::uint64_t donated = 0;
    template <typename V>
    void Visit(V& v) {
        v("type", type);
        v("level", level);
        v("delta", delta);
        v("exp", exp);
        v("levelUp", levelUp);
        v("donated", donated);
    }
};

// A ranking's rows (0xB0E4 {1, type, kind, u8 count, rows}): rank, alias, job level, exp (activity) or the week's
// contribution, and for activity the player's country (0 Chinese, 1 European: its grade names').
struct JobRankRecord {
    std::uint8_t rank = 0;
    std::string alias;
    std::uint8_t level = 0;
    std::uint32_t value = 0;
    std::uint8_t country = 0;
    template <typename V>
    void Visit(V& v) {
        v("rank", rank);
        v("alias", alias, 32);
        v("level", level);
        v("value", value);
        v("country", country);
    }
};
struct JobRanking {
    static constexpr MessageId kId = 0x134B;
    std::uint8_t type = 0, kind = 0;
    std::vector<JobRankRecord> rows;
    template <typename V>
    void Visit(V& v) {
        v("type", type);
        v("kind", kind);
        v("rows", rows, 64);
    }
};
// The outcome's answer (0xB0E5 {1, action, u32}): what there is to collect (action 0, even 0), or what was paid (1).
struct JobOutcomeAnswer {
    static constexpr MessageId kId = 0x134C;
    std::uint32_t npc = 0;
    std::uint8_t action = 0;
    std::uint32_t amount = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("action", action);
        v("amount", amount);
    }
};

// A trader league member's safe trade (the original's 0x34D5): `active` after the
// change and its code: 0x40 reset, 0x41 the day's limit reached, 0x50 begun ({count, limit}), 0x51 at log-in ({count,
// limit} when active). The HUD's icon shows while active.
struct SafeTradeState {
    static constexpr MessageId kId = 0x134D;
    std::uint8_t active = 0, code = 0x40, count = 0, limit = 0;
    template <typename V>
    void Visit(V& v) {
        v("active", active);
        v("code", code);
        v("count", count);
        v("limit", limit);
    }
};

// The bargain goods' lock-out (the original's 0x320C {7, seconds}, timed job type 7):
// its seconds left of 10 minutes; the buff board's kind 6 icon (icon_trade_timer.ddj) until they run out.
struct TradeBargainLock {
    static constexpr MessageId kId = 0x134E;
    std::uint32_t seconds = 0;
    template <typename V>
    void Visit(V& v) {
        v("seconds", seconds);
    }
};

using JobEvent = std::variant<JobInfoMessage, EntityJob, JobAnswer, JobPrevious, EntityJobChange, TradeAnswer, TradePrices, TradeScale, TradeBargainNotice, TradeBoard,
                              JobExpChanged, JobRanking, JobOutcomeAnswer, SafeTradeState, TradeBargainLock>;

// The job messages' range (0x1320-0x135F); the server's are from 0x1340.
inline bool IsJobMessage(MessageId id) { return id >= 0x1320 && id < 0x1360; }

inline bool DecodeJobEvent(MessageId id, std::span<const std::byte> payload, JobEvent& out) {
    const auto as = [&]<typename T>(T message) {
        if (!Decode(payload, message)) return false;
        out = std::move(message);
        return true;
    };
    switch (id) {
    case JobInfoMessage::kId: return as(JobInfoMessage{});
    case EntityJob::kId: return as(EntityJob{});
    case JobAnswer::kId: return as(JobAnswer{});
    case JobPrevious::kId: return as(JobPrevious{});
    case EntityJobChange::kId: return as(EntityJobChange{});
    case TradeAnswer::kId: return as(TradeAnswer{});
    case TradePrices::kId: return as(TradePrices{});
    case TradeScale::kId: return as(TradeScale{});
    case TradeBargainNotice::kId: return as(TradeBargainNotice{});
    case TradeBoard::kId: return as(TradeBoard{});
    case JobExpChanged::kId: return as(JobExpChanged{});
    case JobRanking::kId: return as(JobRanking{});
    case JobOutcomeAnswer::kId: return as(JobOutcomeAnswer{});
    case SafeTradeState::kId: return as(SafeTradeState{});
    case TradeBargainLock::kId: return as(TradeBargainLock{});
    default: return false;
    }
}

inline const char* JobMessageName(MessageId id) {
    switch (id) {
    case JobInfoMessage::kId: return "JobInfo";
    case EntityJob::kId: return "EntityJob";
    case JobAnswer::kId: return "JobAnswer";
    case JobPrevious::kId: return "JobPrevious";
    case EntityJobChange::kId: return "EntityJobChange";
    case TradeAnswer::kId: return "TradeAnswer";
    case TradePrices::kId: return "TradePrices";
    case TradeScale::kId: return "TradeScale";
    case TradeBargainNotice::kId: return "TradeBargainNotice";
    case TradeBoard::kId: return "TradeBoard";
    case JobExpChanged::kId: return "JobExpChanged";
    case JobRanking::kId: return "JobRanking";
    case JobOutcomeAnswer::kId: return "JobOutcomeAnswer";
    case SafeTradeState::kId: return "SafeTradeState";
    case TradeBargainLock::kId: return "TradeBargainLock";
    case JobRankQuery::kId: return "JobRankQuery";
    case JobOutcome::kId: return "JobOutcome";
    case TradeBoardQuery::kId: return "TradeBoardQuery";
    case TradeSell::kId: return "TradeSell";
    case TradeSellAll::kId: return "TradeSellAll";
    case JobJoin::kId: return "JobJoin";
    case JobLeave::kId: return "JobLeave";
    case JobAlias::kId: return "JobAlias";
    case JobPreviousQuery::kId: return "JobPreviousQuery";
    default: return nullptr;
    }
}

} // namespace sr::game::proto
