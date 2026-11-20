#pragma once

// The exchange between two players over the network: our own messages
// (0x1210-0x122F) for what the original's carry (its requests 0x7080-0x7084 and the item moves 0x7034
// 4, 5 and 0x0E; its answers 0xB081-0xB084 and 0x3085-0x3089). The rules, the limits and the error
// numbers are the original's; the client turns the numbers into its texts.

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "sr/game/Protocol.h"
#include "sr/game/WorldProtocol.h"

namespace sr::game::proto {

constexpr std::size_t kExchangeSlots = 12;               // a side's places
constexpr float kExchangeReach = 320.0f;                 // the request's range, the two bodies' centres
constexpr std::uint64_t kExchangeMostGold = 1000000000;  // the most gold one side offers
constexpr double kExchangeAnswerSeconds = 30.0;          // the request's wait for an answer (the proposals')

// The original's numbers for what goes wrong (its CTradeMgr's; the texts are the client's).
enum class ExchangeCode : std::uint16_t {
    Ok = 0,
    Failed = 2,              // the gold beyond the most one side offers, a proposal that could not be made
    InvalidTarget = 3,       // itself, nobody, not a player ("Cannot find target")
    TooFar = 4,              // beyond kExchangeReach (no text in the original's client; ours "The object is too far.")
    NotEnoughGold = 0x180F,  // more gold than the player holds
    WornItem = 0x1806,       // a worn item (only the bag's are offered)
    NoItem = 0x1809,         // nothing in that place
    Abuse = 0x1811,          // what was offered is no longer there at the approval
    WrongStep = 0x181C,      // an approval before both confirmed
    SelfBusy = 0x181D,       // the asker is dead, busy or not free (no text in the original's; ours CANNOT_DEAL_NORMALCY)
    Busy = 0x181E,           // the asked player is ("User is busy.")
    Locked = 0x181F,         // a side already confirmed: its offer stays as it is
    PeerGone = 0x1820,       // the other player is gone
    BayFull = 0x1821,        // 12 offered already
    BagFull = 0x1822,        // no room in a bag for what it receives (ours at the approval; the original's 0x1841 has no text)
    NothingOffered = 0x1823, // no item and no gold on either side
    Timeout = 0x1826,        // no answer within kExchangeAnswerSeconds
    Refused = 0x1828,        // the asked player said no
    InvalidNumber = 0x1829,  // a gold offer beyond what a number holds
    Cancelled = 0x182B,      // ended by a death, a leave, a teleport
    CancelledByUser = 0x182C,
    AlreadyOffered = 0x182D,
    AskerStalls = 0x1833,    // the asker is at a stall (no text in the original's; ours as SelfBusy)
    PeerBagFull = 0x1836,    // the other's bag has no room for one more
    CannotTrade = 0x183A,    // the item's CanTrade is 0 (_RefObjCommon)
    PeerDead = 0x1840,       // the asked player died before answering
    TradeGoods = 0x1850,     // a trade good (TID 3/3/8)
    PetSummoned = 0x18B6,    // a pet's summoning item while its pet is out (no pets yet)
    Rented = 0x18FA,         // a rental whose period runs (_RentItemInfo; "Items in the awakening process cannot be traded.")
};

// A side's step: putting items and gold in, confirmed (its offer fixed), approved (the deal agreed).
enum class ExchangeStep : std::uint8_t { Open = 0, Confirmed = 1, Approved = 2 };

// --- Requests (0x1210) ------------------------------------------------------------------------

// An exchange asked of a player in sight (the Action page's Exchange, the target menu's; the
// original's 0x7081).
struct ExchangeAsk {
    static constexpr MessageId kId = 0x1210;
    std::uint32_t target = 0; // its entity
    template <typename V>
    void Visit(V& v) {
        v("target", target);
    }
};

// The asked player's answer (the original's 0x7080 for a proposal of kind 1).
struct ExchangeReply {
    static constexpr MessageId kId = 0x1211;
    std::uint8_t accept = 0;
    template <typename V>
    void Visit(V& v) {
        v("accept", accept);
    }
};

// An item of the bag into the exchange (the original's 0x7034 op 4): its place in the bag.
struct ExchangePut {
    static constexpr MessageId kId = 0x1212;
    std::uint8_t place = 0;
    template <typename V>
    void Visit(V& v) {
        v("place", place);
    }
};

// An item taken back out of the exchange (op 5): its place in the exchange (0..11).
struct ExchangeTake {
    static constexpr MessageId kId = 0x1213;
    std::uint8_t slot = 0;
    template <typename V>
    void Visit(V& v) {
        v("slot", slot);
    }
};

// The gold offered (op 0x0E): the amount, which replaces the one before (0: none, ours).
struct ExchangeGold {
    static constexpr MessageId kId = 0x1214;
    std::uint64_t gold = 0;
    template <typename V>
    void Visit(V& v) {
        v("gold", gold);
    }
};

// The window's Confirm: the player's offer fixed (0x7082); once both confirmed, its Confirm again is
// the approval (0x7083). The deal is made when both approved.
struct ExchangeConfirm {
    static constexpr MessageId kId = 0x1215;
    template <typename V>
    void Visit(V&) {}
};
struct ExchangeApprove {
    static constexpr MessageId kId = 0x1216;
    template <typename V>
    void Visit(V&) {}
};

// The window's Cancel, or the request taken back (0x7084).
struct ExchangeCancel {
    static constexpr MessageId kId = 0x1217;
    template <typename V>
    void Visit(V&) {}
};

// --- Answers and news (0x1220) -------------------------------------------------------------------

// An exchange asked of the player (the original's 0x3080 kind 1): who asks.
struct ExchangeAsked {
    static constexpr MessageId kId = 0x1220;
    std::uint32_t from = 0; // its entity
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("from", from);
        v("name", name, 64);
    }
};

// A request refused (the original's error answers): what went wrong. The exchange goes on unless an
// ExchangeClosed follows.
struct ExchangeAnswer {
    static constexpr MessageId kId = 0x1221;
    ExchangeCode code = ExchangeCode::Ok;
    template <typename V>
    void Visit(V& v) {
        v("code", code);
    }
};

// The exchange is open for both (the original's 0xB081 to the asker, 0x3085 to the asked): with whom.
struct ExchangeOpened {
    static constexpr MessageId kId = 0x1222;
    std::uint32_t peer = 0;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("peer", peer);
        v("name", name, 64);
    }
};

// A side as it is now, to both players (the original's 0x3089 items and gold, 0xB082 / 0x3086 the
// confirmations): `mine` the receiver's own side; each item at its place in the exchange
// (`items[i].slot`, 0..11) and, on the owner's own side, the bag place it was put from (`places[i]`).
struct ExchangeSide {
    static constexpr MessageId kId = 0x1223;
    std::uint8_t mine = 0;
    ExchangeStep step = ExchangeStep::Open;
    std::uint64_t gold = 0;
    std::vector<ItemRecord> items;
    std::vector<std::uint8_t> places;
    template <typename V>
    void Visit(V& v) {
        v("mine", mine);
        v("step", step);
        v("gold", gold);
        v("items", items, kExchangeSlots);
        v("places", places, kExchangeSlots);
    }
};

// The exchange is over (the original's 0x3087 done, 0x3088 / 0xB084 ended): `done` the items and the
// gold moved, else `code` says why it ended (Ok: the player's own cancel). Done: the bag's places the
// player received (`arrived`) and gave (`left`), and its gold now.
struct ExchangeClosed {
    static constexpr MessageId kId = 0x1224;
    std::uint8_t done = 0;
    ExchangeCode code = ExchangeCode::Ok;
    std::vector<ItemRecord> arrived;
    std::vector<std::uint8_t> left;
    std::uint64_t gold = 0;
    template <typename V>
    void Visit(V& v) {
        v("done", done);
        v("code", code);
        v("arrived", arrived, kExchangeSlots);
        v("left", left, kExchangeSlots);
        v("gold", gold);
    }
};

using ExchangeEvent = std::variant<ExchangeAsked, ExchangeAnswer, ExchangeOpened, ExchangeSide, ExchangeClosed>;

inline bool IsExchangeMessage(MessageId id) { return id >= 0x1210 && id < 0x1230; }

// A server's exchange message as the session's event; false for another id or a malformed payload.
inline bool DecodeExchangeEvent(MessageId id, std::span<const std::byte> payload, ExchangeEvent& out) {
    const auto as = [&]<typename T>(T message) {
        if (!Decode(payload, message)) return false;
        out = std::move(message);
        return true;
    };
    switch (id) {
    case ExchangeAsked::kId: return as(ExchangeAsked{});
    case ExchangeAnswer::kId: return as(ExchangeAnswer{});
    case ExchangeOpened::kId: return as(ExchangeOpened{});
    case ExchangeSide::kId: return as(ExchangeSide{});
    case ExchangeClosed::kId: return as(ExchangeClosed{});
    default: return false;
    }
}

inline const char* ExchangeMessageName(MessageId id) {
    switch (id) {
    case ExchangeAsked::kId: return "ExchangeAsked";
    case ExchangeAnswer::kId: return "ExchangeAnswer";
    case ExchangeOpened::kId: return "ExchangeOpened";
    case ExchangeSide::kId: return "ExchangeSide";
    case ExchangeClosed::kId: return "ExchangeClosed";
    default: return nullptr;
    }
}

} // namespace sr::game::proto
