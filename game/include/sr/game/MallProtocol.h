#pragma once

// The item mall, silk, Magic POP and the mall's items over the network: our own messages 0x13C0-0x13FF for what the
// original's carry (its 0x7034 operation 0x18 and 0xB034,
// 0x7121 / 0xB121, the silk messages 0x3153 / 0x3154, the history 0x711A / 0xB11A, Magic POP's 0x7118 / 0x7119 / 0x3120,
// the time services' 0x3206, the premium's 0x715F / 0x7160, the withdrawals 0x7202 / 0x7203, the repair 0x703E and
// 0x3052, the expansions' 0x3092, the skin change's 0x3207). The refusals keep the original's codes, which the client
// turns into its texts. Requests from 0x13C0 (and 0x13E0, 0x13F0), answers from 0x13D0
// (0x13E8, 0x13F8).

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "sr/game/Protocol.h"
#include "sr/game/WorldProtocol.h"

namespace sr::game::proto {

// The NPC repairs (the original's NPC function 4: the smiths and armour merchants): its
// store shows Repair and Repair all (NpcMenu::services).
constexpr std::uint32_t kNpcRepair = 0x400000;

// The mall's codes and ours.
namespace mallcode {
constexpr std::uint16_t kOk = 0;
constexpr std::uint16_t kFailed = 2;          // the original's default (no text)
constexpr std::uint16_t kNoTab = 0x1806;      // no such tab
constexpr std::uint16_t kNoGood = 0x1809;     // no good at that tab and slot
constexpr std::uint16_t kBagFull = 0x1807;    // "The inventory is full."
constexpr std::uint16_t kNoSilk = 0x1880;     // "This purchase cannot complete due to insufficient silks."
constexpr std::uint16_t kSoldOut = 0x18CF;    // "Already sold out"
constexpr std::uint16_t kWrongGood = 0x18AB;  // the package is not the good's
constexpr std::uint16_t kBusy = 0x1835;       // ours: an exchange, a stall, alchemy or a purchase under way holds the bag
} // namespace mallcode

// --- The mall ------------------------------------------------------------------------

// The mall opened (the original's 0x7121, sent only for a group with stock rows; ours always, it is cheap): MallState.
struct MallOpen {
    static constexpr MessageId kId = 0x13C0;
    template <typename V>
    void Visit(V&) {}
};
// A good bought (the original's 0x7034 operation 0x18 {group, shop index, tab, slot, package code, count, points,
// package id}): ours names the tab by its `refshoptab` id and checks the package by its id, as the original checks its
// code name. `points`: the points to use (CItemMallUsePoint; 0 none). MallBought; then SilkBalanceMessage.
struct MallBuy {
    static constexpr MessageId kId = 0x13C1;
    std::uint32_t tab = 0;
    std::uint8_t slot = 0;
    std::uint32_t package = 0;
    std::uint16_t count = 1;
    std::uint32_t points = 0;
    template <typename V>
    void Visit(V& v) {
        v("tab", tab);
        v("slot", slot);
        v("package", package);
        v("count", count);
        v("points", points);
    }
};
// Several goods at once: the worn avatars' "Buy" and the basket's lump buy (ours: one transaction, all or nothing;
// the original sends them one after another). MallBought.
struct MallBuyLine {
    std::uint32_t tab = 0;
    std::uint8_t slot = 0;
    std::uint32_t package = 0;
    std::uint16_t count = 1;
    template <typename V>
    void Visit(V& v) {
        v("tab", tab);
        v("slot", slot);
        v("package", package);
        v("count", count);
    }
};
struct MallBuyMany {
    static constexpr MessageId kId = 0x13C2;
    std::vector<MallBuyLine> goods;
    std::uint32_t points = 0;
    template <typename V>
    void Visit(V& v) {
        v("goods", goods, 32);
        v("points", points);
    }
};
// The silk history (the original's 0x711A {1, period}): list 1 the bought silk (the Buying list), 3 the gift silk
// received; period 1 a day, 2 a week, 3 a month, 4 three months. SilkHistory.
struct SilkHistoryQuery {
    static constexpr MessageId kId = 0x13C3;
    std::uint8_t list = 1, period = 1;
    template <typename V>
    void Visit(V& v) {
        v("list", list);
        v("period", period);
    }
};

// The goods' stock (the original's 0xB121 rows): the packages with a limited stock and what is left.
struct MallStock {
    std::uint32_t package = 0;
    std::uint16_t left = 0;
    template <typename V>
    void Visit(V& v) {
        v("package", package);
        v("left", left);
    }
};
struct MallState {
    static constexpr MessageId kId = 0x13D0;
    std::vector<MallStock> stock;
    template <typename V>
    void Visit(V& v) {
        v("stock", stock, 512);
    }
};
// A purchase's answer (the original's 0xB034 operation 0x18): `code` 0 done, else the refusal; done: the goods and
// the new items where they lie (ours sends them; the original's client made them from its own package data).
struct MallBought {
    static constexpr MessageId kId = 0x13D1;
    std::uint16_t code = 0;
    std::vector<MallBuyLine> goods;
    std::vector<ItemRecord> items;
    template <typename V>
    void Visit(V& v) {
        v("code", code);
        v("goods", goods, 32);
        v("items", items, 160);
    }
};
// The account's silk (the original's 0x3153 {silk, gift silk, points}): on entering the world, after a purchase,
// after a change from outside.
struct SilkBalanceMessage {
    static constexpr MessageId kId = 0x13D2;
    std::uint32_t own = 0, gift = 0, point = 0;
    template <typename V>
    void Visit(V& v) {
        v("own", own);
        v("gift", gift);
        v("point", point);
    }
};
// A silk line (the original's 0x3154 {reason, kind, value}): reason 0 "You have bought [%d] silk.", 1 canceled,
// 2 "You have received [%d] silk as gift.", 3 sent, 4 points received, 5 points used; 0xFF the trade failure.
struct SilkLine {
    static constexpr MessageId kId = 0x13D3;
    std::uint8_t reason = 0, kind = 0;
    std::uint32_t value = 0;
    template <typename V>
    void Visit(V& v) {
        v("reason", reason);
        v("kind", kind);
        v("value", value);
    }
};
// The history's answer (the original's 0xB11A): `when` packed as the original's (bits 0-5 year - 2000, 6-9 month,
// 10-14 day, 15-19 hour, 20-25 minute). result 0 found (maybe no rows), else a failure.
struct SilkHistoryRecord {
    std::uint32_t silk = 0;
    std::uint32_t when = 0;
    template <typename V>
    void Visit(V& v) {
        v("silk", silk);
        v("when", when);
    }
};
struct SilkHistory {
    static constexpr MessageId kId = 0x13D4;
    std::uint8_t list = 1, result = 0;
    std::vector<SilkHistoryRecord> rows;
    template <typename V>
    void Visit(V& v) {
        v("list", list);
        v("result", result);
        v("rows", rows, 1000);
    }
};
// The original's packing of a history date.
constexpr std::uint32_t PackSilkDate(int year, int month, int day, int hour, int minute) {
    return std::uint32_t((year - 2000) & 0x3F) | std::uint32_t(month & 0xF) << 6 | std::uint32_t(day & 0x1F) << 10 | std::uint32_t(hour & 0x1F) << 15 |
           std::uint32_t(minute & 0x3F) << 20;
}

// --- Magic POP ---------------------------------------------------------------------------

// The NPC menu's bits (NpcMenu::services): the machine (NPC function 0x11, its row "Participate in the game.") and Gori
// (0x12, "Exchange Item Exchange Coupon to Item").
constexpr std::uint32_t kNpcPopMachine = 0x100000, kNpcPopExchange = 0x200000;

// A card played at the machine (the original's 0x7118 {npc, GachaID, slot}): the prize wanted (its GachaID) and the
// card's bag place. PopPlayed.
struct PopPlay {
    static constexpr MessageId kId = 0x13E0;
    std::uint32_t npc = 0;
    std::uint32_t entry = 0;
    std::uint8_t place = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("entry", entry);
        v("place", place);
    }
};
// Every coupon of the bag exchanged at Gori (the original's 0x7119 {npc}). PopExchanged.
struct PopExchange {
    static constexpr MessageId kId = 0x13E1;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};
// A play's answer (the original's 0xB118 {1, won} / {2, code}): the card became the coupon `coupon` holding `prize` x
// `count` in `place` (its record follows in `item`).
struct PopPlayed {
    static constexpr MessageId kId = 0x13E8;
    std::uint16_t code = 0;
    std::uint8_t won = 0;
    std::uint32_t prize = 0;
    std::uint16_t count = 0;
    std::vector<ItemRecord> item; // the coupon (one)
    template <typename V>
    void Visit(V& v) {
        v("code", code);
        v("won", won);
        v("prize", prize);
        v("count", count);
        v("item", item, 1);
    }
};
// The exchange's answer: each coupon's prize where it lies now ("Has been changed to [%s][%d]."), or a refusal.
struct PopExchanged {
    static constexpr MessageId kId = 0x13E9;
    std::uint16_t code = 0;
    std::vector<ItemRecord> items;
    template <typename V>
    void Visit(V& v) {
        v("code", code);
        v("items", items, 96);
    }
};
// A win told to everyone (the original's 0x3120 {item, count}: "[%s][%d]has been won from Magic POP."), only for a
// prize the data marks visible.
struct PopAnnounce {
    static constexpr MessageId kId = 0x13EA;
    std::uint32_t item = 0;
    std::uint16_t count = 0;
    std::string name; // the winner (ours; the original's line names nobody)
    template <typename V>
    void Visit(V& v) {
        v("item", item);
        v("count", count);
        v("name", name, 32);
    }
};

// --- The mall's items -------------------------------------------------------------------

// The premium's daily uses (the original's 0x715F {entry, item, [option, point]}, the chat's /Return, /Reverse Return,
// /Resurrection): `item` the item whose use the premium gives (its RefObjID), `option` / `point` a reverse return's
// choice. ServiceAnswer (kServicePremiumUse).
struct PremiumUse {
    static constexpr MessageId kId = 0x13F0;
    std::uint32_t item = 0;
    std::uint8_t option = 0;
    std::uint32_t point = 0;
    template <typename V>
    void Visit(V& v) {
        v("item", item);
        v("option", option);
        v("point", point);
    }
};
// The premium's booth (the original's 0x7160): its stall decoration made the stall's. ServiceAnswer.
struct PremiumBooth {
    static constexpr MessageId kId = 0x13F1;
    std::uint32_t item = 0;
    template <typename V>
    void Visit(V& v) {
        v("item", item);
    }
};
// A time service paused or resumed (the original's 0x74BF {category, on}). ServiceAnswer; TimedServices.
struct TimedPause {
    static constexpr MessageId kId = 0x13F2;
    std::uint8_t category = 0;
    std::uint8_t on = 0;
    template <typename V>
    void Visit(V& v) {
        v("category", category);
        v("on", on);
    }
};
// A skill (or mastery) withdrawn with the skill restoration potion (the original's 0x7202 / 0x7203 {potion, skill or
// mastery, target level}): `potion` the potion's bag place; `mastery` 1: `id` a mastery. ServiceAnswer.
struct Withdraw {
    static constexpr MessageId kId = 0x13F3;
    std::uint8_t potion = 0;
    std::uint8_t mastery = 0;
    std::uint32_t id = 0;
    std::uint8_t level = 0;
    template <typename V>
    void Visit(V& v) {
        v("potion", potion);
        v("mastery", mastery);
        v("id", id);
        v("level", level);
    }
};
// The NPC's repair (the original's 0x703E {npc, kind, [slot]}): kind 1 the item in `place`, 2 everything worn and in
// the bag. ServiceAnswer; DurabilityChanged per item.
struct Repair {
    static constexpr MessageId kId = 0x13F4;
    std::uint32_t npc = 0;
    std::uint8_t kind = 2;
    std::uint8_t place = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("kind", kind);
        v("place", place);
    }
};
// An item used on another of the bag (the original's item use with a bag slot in its tail): the gender switch tool on
// an armour, the Extension Gear on a Devil's Spirit, the potion of growth on a growth pet's item. ServiceAnswer.
struct UseItemOn {
    static constexpr MessageId kId = 0x13F5;
    std::uint8_t place = 0;  // the tool
    std::uint8_t target = 0; // the item it is used on
    template <typename V>
    void Visit(V& v) {
        v("place", place);
        v("target", target);
    }
};
// The skin change scroll used (its window's OK: the original's item use with {model, scale}). ServiceAnswer; the
// character appears again to everyone near.
struct SkinChange {
    static constexpr MessageId kId = 0x13F6;
    std::uint8_t place = 0;
    std::uint32_t model = 0;
    std::uint8_t scale = 0;
    template <typename V>
    void Visit(V& v) {
        v("place", place);
        v("model", model);
        v("scale", scale);
    }
};
// A bonus scroll's run ended before its time (its buff board entry's right click, the box UIIT_MSG_QUESTION_SILKMALL_ITEM_USE_CANCLE_1
// / _2 answered; ours: the original's message for it is not traced): `category` the run's TimedService category.
// ServiceAnswer (kCancel); TimedServices.
struct TimedCancel {
    static constexpr MessageId kId = 0x13F7;
    std::uint8_t category = 0;
    template <typename V>
    void Visit(V& v) {
        v("category", category);
    }
};

// The time services running (the original's 0x3206 {category, 1, item, seconds left, bonus, paused} per service; a
// service gone is left out): category 3 gold / silver, 9 skill, 10 the premium package; ours: 116..119 a bonus scroll's
// run (game::kTimedBonusFirst: experience, skill points, skill experience, berserk time).
struct TimedService {
    std::uint8_t category = 0;
    std::uint32_t item = 0;
    std::uint32_t secondsLeft = 0;
    std::uint32_t bonus = 0; // yesterday's unused bonus seconds << 16 | today's
    std::uint8_t paused = 0;
    template <typename V>
    void Visit(V& v) {
        v("category", category);
        v("item", item);
        v("secondsLeft", secondsLeft);
        v("bonus", bonus);
        v("paused", paused);
    }
};
// The premium's daily uses left (its UIL1 parts): the item and its uses left in the day.
struct PremiumUses {
    std::uint32_t item = 0;
    std::uint8_t left = 0;
    std::uint8_t perDay = 0;
    template <typename V>
    void Visit(V& v) {
        v("item", item);
        v("left", left);
        v("perDay", perDay);
    }
};
struct TimedServices {
    static constexpr MessageId kId = 0x13F8;
    std::vector<TimedService> services;
    std::vector<PremiumUses> premium;
    template <typename V>
    void Visit(V& v) {
        v("services", services, 16);
        v("premium", premium, 16);
    }
};
// What a ServiceAnswer answers.
enum class ServiceRequest : std::uint8_t {
    PremiumUse = 1, PremiumBooth = 2, Pause = 3, Withdraw = 4, Repair = 5, UseOn = 6, Skin = 7, Expansion = 8, Hammer = 9, Resurrect = 10, Cube = 11,
    Cancel = 12,
};
// A service's answer: `code` 0 done, else the original's code; `a`, `b` what was done (the skill or mastery and its
// level now, the item's new reference, ...).
struct ServiceAnswer {
    static constexpr MessageId kId = 0x13F9;
    ServiceRequest request = ServiceRequest::PremiumUse;
    std::uint16_t code = 0;
    std::uint32_t a = 0, b = 0;
    template <typename V>
    void Visit(V& v) {
        v("request", request);
        v("code", code);
        v("a", a);
        v("b", b);
    }
};
// An item's durability changed (the original's 0x3052 {slot, durability}): worn down, repaired.
struct DurabilityChanged {
    static constexpr MessageId kId = 0x13FA;
    std::uint8_t place = 0;
    std::uint32_t durability = 0;
    template <typename V>
    void Visit(V& v) {
        v("place", place);
        v("durability", durability);
    }
};
// The bag or the storage grown by an expansion (the original's 0x3092 {target, size}): 1 the bag (its places, the
// worn ones counted), 2 the storage.
struct StorageGrown {
    static constexpr MessageId kId = 0x13FB;
    std::uint8_t target = 1;
    std::uint8_t size = 0;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
        v("size", size);
    }
};

// A worn Devil's Spirit asleep (its awakening over; the original's 0x3078 {3, place}, told at each of its 10-s checks
// while it stays worn asleep): its options and skills are off, its numbers sent
// again; it stays worn. `place` the avatar place (4).
struct SpiritAsleep {
    static constexpr MessageId kId = 0x13FC;
    std::uint8_t place = 4;
    template <typename V>
    void Visit(V& v) {
        v("place", place);
    }
};

// A magic cube's elixirs (the original's 0x7034 kinds 0x27 / 0x28): `kind` 0 the bag's
// item in `from` put into the cube in `cube` (CubeContents), 1 the cube's place `slot` taken out into the bag's first
// free place. Refusals: ServiceAnswer (kCube) with 0x188A, 0x188B, 0x1806, 0x1807.
struct CubeMove {
    static constexpr MessageId kId = 0x13CF;
    std::uint8_t kind = 0;
    std::uint8_t from = 0, cube = 0, slot = 0;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("from", from);
        v("cube", cube);
        v("slot", slot);
    }
};
// What a cube holds (on entering the world, after each change): its bag place and its items (each record's slot the
// cube's own place).
struct CubeContents {
    static constexpr MessageId kId = 0x13DF;
    std::uint8_t cube = 0;
    std::vector<ItemRecord> items;
    template <typename V>
    void Visit(V& v) {
        v("cube", cube);
        v("items", items, 64);
    }
};

using MallEvent = std::variant<MallState, MallBought, SilkBalanceMessage, SilkLine, SilkHistory, PopPlayed, PopExchanged, PopAnnounce, TimedServices,
                               ServiceAnswer, DurabilityChanged, StorageGrown, CubeContents, SpiritAsleep>;

// The mall's messages' range (0x13C0-0x13FF).
inline bool IsMallMessage(MessageId id) { return id >= 0x13C0 && id < 0x1400; }
// Whether one is the server's (an answer or a notice).
inline bool IsMallAnswer(MessageId id) { return (id >= 0x13D0 && id < 0x13E0) || (id >= 0x13E8 && id < 0x13F0) || id >= 0x13F8; }

inline bool DecodeMallEvent(MessageId id, std::span<const std::byte> payload, MallEvent& out) {
    const auto as = [&]<typename T>(T message) {
        if (!Decode(payload, message)) return false;
        out = std::move(message);
        return true;
    };
    switch (id) {
    case MallState::kId: return as(MallState{});
    case MallBought::kId: return as(MallBought{});
    case SilkBalanceMessage::kId: return as(SilkBalanceMessage{});
    case SilkLine::kId: return as(SilkLine{});
    case SilkHistory::kId: return as(SilkHistory{});
    case PopPlayed::kId: return as(PopPlayed{});
    case PopExchanged::kId: return as(PopExchanged{});
    case PopAnnounce::kId: return as(PopAnnounce{});
    case TimedServices::kId: return as(TimedServices{});
    case ServiceAnswer::kId: return as(ServiceAnswer{});
    case DurabilityChanged::kId: return as(DurabilityChanged{});
    case StorageGrown::kId: return as(StorageGrown{});
    case CubeContents::kId: return as(CubeContents{});
    case SpiritAsleep::kId: return as(SpiritAsleep{});
    default: return false;
    }
}

inline const char* MallMessageName(MessageId id) {
    switch (id) {
    case MallOpen::kId: return "MallOpen";
    case MallBuy::kId: return "MallBuy";
    case MallBuyMany::kId: return "MallBuyMany";
    case SilkHistoryQuery::kId: return "SilkHistoryQuery";
    case MallState::kId: return "MallState";
    case MallBought::kId: return "MallBought";
    case SilkBalanceMessage::kId: return "SilkBalance";
    case SilkLine::kId: return "SilkLine";
    case SilkHistory::kId: return "SilkHistory";
    case PopPlay::kId: return "PopPlay";
    case PopExchange::kId: return "PopExchange";
    case PopPlayed::kId: return "PopPlayed";
    case PopExchanged::kId: return "PopExchanged";
    case PopAnnounce::kId: return "PopAnnounce";
    case PremiumUse::kId: return "PremiumUse";
    case PremiumBooth::kId: return "PremiumBooth";
    case TimedPause::kId: return "TimedPause";
    case Withdraw::kId: return "Withdraw";
    case Repair::kId: return "Repair";
    case UseItemOn::kId: return "UseItemOn";
    case SkinChange::kId: return "SkinChange";
    case TimedCancel::kId: return "TimedCancel";
    case TimedServices::kId: return "TimedServices";
    case ServiceAnswer::kId: return "ServiceAnswer";
    case DurabilityChanged::kId: return "DurabilityChanged";
    case StorageGrown::kId: return "StorageGrown";
    case CubeMove::kId: return "CubeMove";
    case CubeContents::kId: return "CubeContents";
    case SpiritAsleep::kId: return "SpiritAsleep";
    default: return "Mall?";
    }
}

} // namespace sr::game::proto
