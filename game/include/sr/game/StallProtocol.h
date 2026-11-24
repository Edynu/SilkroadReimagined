#pragma once

// The stall and the stall network over the network: our own messages (0x1230-0x125F)
// for what the original's carry (its requests 0x70B1-0x70B5 and 0x70BA's sub-operations, the network's
// 0x7461-0x7463; its answers 0xB0B1-0xB0BA, 0xB461, 0xB463 and the news 0x30B7-0x30BB). The stall's rules,
// limits and codes are the original's; the client turns the codes into its texts. The network is ours: a shard-wide
// index searched by the
// client's class, type and degree, its matches paged 15 at a time.

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "sr/game/Protocol.h"
#include "sr/game/WorldProtocol.h"

namespace sr::game::proto {

constexpr std::size_t kStallSlots = 10;                 // a stall's places (0..9)
constexpr std::size_t kStallPeople = 8;                 // the member limit, its owner among them (0x3C18)
constexpr float kStallReach = 100.0f;                   // a visitor's walk-up range
// A price's range (0x3C08 outside it): the original's up to 9,999,999,999, the add's and the reprice's; its 0 passes the
// server, ours refuses it (a slip in the box would give the item away).
constexpr std::uint64_t kStallMostPrice = 9999999999ull;
constexpr std::size_t kStallNameLength = 64;            // the name's bytes, 1..64 (0x3C09; the original's)
constexpr std::size_t kStallGreetingMost = 64;          // the greeting's bytes (0x3C0A beyond; the original's)
constexpr std::size_t kStallGreetingLength = 256;       // ours: the greeting's room in a message
constexpr std::size_t kNetworkPageRows = 15;            // the window's rows a page
constexpr double kNetworkSearchSeconds = 3.0;           // ours: a new search at most this often (the client: 10 s)
constexpr std::uint32_t kStallVendorState = 80;         // the owner's clip: animation state 80 (*_vendor01.ban)
constexpr std::uint64_t kNetworkCommissionPercent = 1;  // a sale through the network costs its seller 1%
                                                        // (UIIT_MSG_WARENETWORK_REGIST_03, _CHARGE)

// The original's numbers for what goes wrong (CStall / CFleaMarket's; the texts are the client's).
enum class StallCode : std::uint16_t {
    Ok = 0,
    SaleUnderWay = 2,        // another sale of that stall is being made (the original's quiet refusal)
    TooFar = 4,              // a visit beyond the original's reach (no text)
    PetSummoned = 0x18B6,    // a pet's summoning item while its pet is out
    Self = 5,                // the owner buying from its own stall (quiet)
    NotBagItem = 0x3C06,     // not an item of the bag (a place below 13, or nothing there)
    Occupied = 0x3C07,       // the count is not the item's whole stack (the original's; ours always puts the whole stack)
    BadPrice = 0x3C08,       // a price outside 1..kStallMostPrice
    BadNameLength = 0x3C09,  // a name empty or longer than kStallNameLength bytes
    GreetingTooLong = 0x3C0A,// a greeting longer than kStallGreetingMost bytes
    NothingToSell = 0x3C0C,  // opening for business with nothing registered
    Closed = 0x3C0E,         // the stall is closed (or gone)
    NotVisitor = 0x3C0F,     // not a visitor of that stall
    SlotEmpty = 0x3C10,      // nothing in that slot (any more), or a slot of 10 or more
    AlreadyUp = 0x3C13,      // the bag's item is up in a slot already
    NotEnoughGold = 0x3C11,
    BagFull = 0x3C12,
    HostLeft = 0x3C15,       // its owner left the world, died or went away
    Busy = 0x3C16,           // no stall may be opened here or now (dead, moving, in another dealing)
    ClosedByHost = 0x3C17,
    Full = 0x3C18,           // the member limit (8)
    OwnerOnly = 0x3C2A,      // the owner may not (visit its own) / only the owner may (close it)
    Modifying = 0x3C2B,      // the stall is being modified (a change only while modifying; a buy while it is)
    NotTradeable = 0x3C2D,   // an item that may not be traded (CanTrade 0) or trade goods (by transport only)
    NotOpen = 0x3C2E,        // buying while the stall is not open for business
    NoFreeSlot = 0x3C30,     // the asked slot taken and no other free
    Riding = 0x3C34,
    BadName = 0x3C38,
    Murderer = 0x3C39,
    PetItem = 0x3C3A,        // a summoned pet's item
    JobSuit = 0x3C3B,
    NetworkFailed = 0x3C41,  // the network: the item is in no network group / the search failed
    TooSoon = 0x3C46,        // the network: a new search within kNetworkSearchSeconds (the client's "scan failed")
    NotTownSearch = 0x3C49,  // the network's window used outside a town (the client closes it)
    NotTown = 0x3C4A,        // the network: not in a town (open for business elsewhere: not registered)
    Changed = 0x3C50,        // the network: the row is no longer as it was shown (sold, taken back, repriced)
    NetworkNoGold = 0x3C52,
    NetworkBagFull = 0x3C53,
    Rented = 0x3C55,         // an item in its awakening (a running rental)
};

// --- Requests (0x1230) -------------------------------------------------------------------------

// A stall opened where the player stands, in its modifying state (the Action page's Stall; 0x70B1).
struct StallOpen {
    static constexpr MessageId kId = 0x1230;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("name", name, kStallNameLength);
    }
};

// The owner closes it (0x70B2).
struct StallClose {
    static constexpr MessageId kId = 0x1231;
    template <typename V>
    void Visit(V&) {}
};

// A visitor walks into a stall in sight (a click on the booth; 0x70B3): its owner's entity.
struct StallVisit {
    static constexpr MessageId kId = 0x1232;
    std::uint32_t owner = 0;
    template <typename V>
    void Visit(V& v) {
        v("owner", owner);
    }
};

// A visitor leaves (0x70B5).
struct StallLeave {
    static constexpr MessageId kId = 0x1233;
    template <typename V>
    void Visit(V&) {}
};

// A visitor buys a slot's item, its whole stack (0x70B4).
struct StallBuy {
    static constexpr MessageId kId = 0x1234;
    std::uint8_t slot = 0;
    template <typename V>
    void Visit(V& v) {
        v("slot", slot);
    }
};

// The owner puts an item of its bag up at a slot (0x70BA 2), or changes a slot's price (0x70BA 1:
// `place` 0). The count is the stack's own (ours: a stack is sold whole).
struct StallPut {
    static constexpr MessageId kId = 0x1235;
    std::uint8_t slot = 0;
    std::uint8_t place = 0; // the bag's place (13 and up); 0: the slot's item stays, its price changes
    std::uint64_t price = 0;
    template <typename V>
    void Visit(V& v) {
        v("slot", slot);
        v("place", place);
        v("price", price);
    }
};

// The owner takes a slot's item back (0x70BA 3).
struct StallTake {
    static constexpr MessageId kId = 0x1236;
    std::uint8_t slot = 0;
    template <typename V>
    void Visit(V& v) {
        v("slot", slot);
    }
};

// The owner's Open / Modify (0x70BA 5): 1 open for business, 0 being modified; opening, `network` 1 when the
// owner said yes to registering its items in the network (UIIT_MSG_WARENETWORK_REGIST_01).
struct StallSetOpen {
    static constexpr MessageId kId = 0x1237;
    std::uint8_t open = 0;
    std::uint8_t network = 0;
    template <typename V>
    void Visit(V& v) {
        v("open", open);
        v("network", network);
    }
};

// The owner's name or greeting (0x70BA 7 / 6).
struct StallSetText {
    static constexpr MessageId kId = 0x1238;
    std::uint8_t greeting = 0; // 1 the greeting, 0 the name
    std::string text;
    template <typename V>
    void Visit(V& v) {
        v("greeting", greeting);
        v("text", text, kStallGreetingLength);
    }
};

// A line said in the stall, to its people only.
struct StallChat {
    static constexpr MessageId kId = 0x1239;
    std::string text;
    template <typename V>
    void Visit(V& v) {
        v("text", text, 256);
    }
};

// The network: a new search (the window's Search; 0x7461 1 / 2): a type's group and a degree (0: any). Ours,
// the detailed search (the words the 1.188 client carries unused, UIIT_CTL_WARENETWORK_DETAIL_*): the grade (0 all,
// 1 general, 2 magic: blue options, 3 rare: a seal), who may wear it (0 all, 1 male, 2 female) and a part of its
// name (any case; with a name the group may be 0: every type). At most once every kNetworkSearchSeconds; the first
// page answers it.
enum class NetworkGrade : std::uint8_t { All = 0, General = 1, Magic = 2, Rare = 3 };
enum class NetworkSex : std::uint8_t { All = 0, Male = 1, Female = 2 };
struct NetworkSearch {
    static constexpr MessageId kId = 0x1240;
    std::uint32_t group = 0;
    std::uint8_t degree = 0;
    NetworkGrade grade = NetworkGrade::All;
    NetworkSex sex = NetworkSex::All;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("group", group);
        v("degree", degree);
        v("grade", grade);
        v("sex", sex);
        v("name", name, 64);
    }
};

// Another page of the last search's matches (0x7461 3; free).
struct NetworkPageAsk {
    static constexpr MessageId kId = 0x1241;
    std::uint16_t page = 0;
    template <typename V>
    void Visit(V& v) {
        v("page", page);
    }
};

// A row bought from afar (0x7463): the row as it was shown (`listing`, `revision`, `price`).
struct NetworkBuy {
    static constexpr MessageId kId = 0x1242;
    std::uint64_t listing = 0;
    std::uint32_t revision = 0;
    std::uint64_t price = 0;
    template <typename V>
    void Visit(V& v) {
        v("listing", listing);
        v("revision", revision);
        v("price", price);
    }
};

// The window closed: the search's matches are let go (0x7462).
struct NetworkClose {
    static constexpr MessageId kId = 0x1243;
    template <typename V>
    void Visit(V&) {}
};

// --- Answers and news (0x1248) -----------------------------------------------------------------

// A request refused: which (its message id) and why.
struct StallAnswer {
    static constexpr MessageId kId = 0x1248;
    std::uint16_t request = 0;
    StallCode code = StallCode::Ok;
    template <typename V>
    void Visit(V& v) {
        v("request", request);
        v("code", code);
    }
};

// A stall's item: its slot, the item (its record's slot: the owner's bag place, to the owner only; 0 to
// a visitor), the price.
struct StallItem {
    std::uint8_t slot = 0;
    ItemRecord item;
    std::uint64_t price = 0;
    template <typename V>
    void Visit(V& v) {
        v("slot", slot);
        v("item", item);
        v("price", price);
    }
};

struct StallPerson {
    std::uint32_t entity = 0;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("name", name, 64);
    }
};

// A stall as it is, to its owner when opened and to a visitor walking in (0xB0B1 / 0xB0B3).
struct StallView {
    static constexpr MessageId kId = 0x1249;
    std::uint32_t owner = 0;
    std::string ownerName, name, greeting;
    std::uint8_t open = 0; // open for business (else being modified)
    std::vector<StallItem> items;
    std::vector<StallPerson> people; // the visitors
    template <typename V>
    void Visit(V& v) {
        v("owner", owner);
        v("ownerName", ownerName, 64);
        v("name", name, kStallNameLength);
        v("greeting", greeting, kStallGreetingLength);
        v("open", open);
        v("items", items, kStallSlots);
        v("people", people, kStallPeople);
    }
};

// A slot changed, to the stall's people (0xB0BA 1 / 2 / 3): its item, or none (`present` 0).
struct StallSlotChanged {
    static constexpr MessageId kId = 0x124A;
    std::uint8_t present = 0;
    StallItem item;
    template <typename V>
    void Visit(V& v) {
        v("present", present);
        v("item", item);
    }
};

// The stall's state, name or greeting changed, to its people (0xB0BA 5 / 6, 0x30BB). To the owner opening
// for business, `network`: 1 registered in the network, 0x3C4A not in a town (not registered), 0 nothing to say
// (not asked for).
struct StallStateChanged {
    static constexpr MessageId kId = 0x124B;
    std::uint8_t open = 0;
    std::string name, greeting;
    std::uint16_t network = 0;
    template <typename V>
    void Visit(V& v) {
        v("open", open);
        v("name", name, kStallNameLength);
        v("greeting", greeting, kStallGreetingLength);
        v("network", network);
    }
};

// A visitor came or left (0x30B7 1 / 2).
struct StallPeopleChanged {
    static constexpr MessageId kId = 0x124C;
    std::uint8_t joined = 0;
    StallPerson person;
    template <typename V>
    void Visit(V& v) {
        v("joined", joined);
        v("person", person);
    }
};

// A line said in the stall (0x30B7 3).
struct StallChatLine {
    static constexpr MessageId kId = 0x124D;
    std::string name, text;
    template <typename V>
    void Visit(V& v) {
        v("name", name, 64);
        v("text", text, 256);
    }
};

// A slot's item sold (0x30B7 4), to the stall's people: the buyer's name, `network` 1 when bought through the
// network. To the owner also the bag place the item left, its gold now and the commission it paid.
struct StallSold {
    static constexpr MessageId kId = 0x124E;
    std::uint8_t slot = 0;
    std::string buyer;
    std::uint8_t network = 0;
    std::uint8_t place = 0;
    std::uint64_t gold = 0;
    std::uint64_t commission = 0;
    template <typename V>
    void Visit(V& v) {
        v("slot", slot);
        v("buyer", buyer, 64);
        v("network", network);
        v("place", place);
        v("gold", gold);
        v("commission", commission);
    }
};

// What the buyer got (0xB0B4, 0xB463): `code` Ok and the item at its bag place, the gold now; else why not.
// `network` 1 for a purchase through the network.
struct StallBought {
    static constexpr MessageId kId = 0x124F;
    StallCode code = StallCode::Ok;
    std::uint8_t network = 0;
    ItemRecord item;
    std::uint64_t gold = 0;
    template <typename V>
    void Visit(V& v) {
        v("code", code);
        v("network", network);
        v("item", item);
        v("gold", gold);
    }
};

// The player is out of the stall it was in or had (0xB0B2, 0xB0B5, 0x30B9 to its people): `code` Ok its own
// leave or close, else why (0x3C17 closed by its owner, 0x3C15 its owner went).
struct StallLeft {
    static constexpr MessageId kId = 0x1250;
    StallCode code = StallCode::Ok;
    template <typename V>
    void Visit(V& v) {
        v("code", code);
    }
};

// A player's stall as those around see it (0x30B8 / 0x30B9 / 0x30BB): its name (empty: none any more) and
// skin (an item's RefObjID, 0 the plain booth by race). Also sent with the player to new viewers.
struct EntityStall {
    static constexpr MessageId kId = 0x1251;
    std::uint32_t entity = 0;
    std::string name;
    std::uint32_t skin = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("name", name, kStallNameLength);
        v("skin", skin);
    }
};

// A row of the network: the stall's slot as it is now somewhere in the shard.
struct NetworkRow {
    std::uint64_t listing = 0;  // the owner's character << 8 | the slot
    std::uint32_t revision = 0; // the slot's changes (a purchase names the one it saw)
    std::string owner;          // the owner's name
    std::uint8_t town = 0;      // its town: regioninfo.txt's area index
    std::uint16_t region = 0;   // the region it stands in (its zone's name on the client)
    ItemRecord item;            // its slot: 0
    std::uint64_t price = 0;
    template <typename V>
    void Visit(V& v) {
        v("listing", listing);
        v("revision", revision);
        v("owner", owner, 64);
        v("town", town);
        v("region", region);
        v("item", item);
        v("price", price);
    }
};

// A page of the last search (0xB461): `code` Ok, or why not (TooSoon, NotTown, NetworkFailed); the page
// (from 0), the pages and the matches in all, its rows (at most kNetworkPageRows), cheapest first.
struct NetworkPage {
    static constexpr MessageId kId = 0x1252;
    StallCode code = StallCode::Ok;
    std::uint16_t page = 0, pages = 0;
    std::uint32_t total = 0;
    std::vector<NetworkRow> rows;
    template <typename V>
    void Visit(V& v) {
        v("code", code);
        v("page", page);
        v("pages", pages);
        v("total", total);
        v("rows", rows, kNetworkPageRows);
    }
};

using StallEvent = std::variant<StallAnswer, StallView, StallSlotChanged, StallStateChanged, StallPeopleChanged, StallChatLine, StallSold,
                                StallBought, StallLeft, EntityStall, NetworkPage>;

inline bool IsStallMessage(MessageId id) { return id >= 0x1230 && id < 0x1260; }

// A server's stall message as the session's event; false for another id or a malformed payload.
inline bool DecodeStallEvent(MessageId id, std::span<const std::byte> payload, StallEvent& out) {
    const auto as = [&]<typename T>(T message) {
        if (!Decode(payload, message)) return false;
        out = std::move(message);
        return true;
    };
    switch (id) {
    case StallAnswer::kId: return as(StallAnswer{});
    case StallView::kId: return as(StallView{});
    case StallSlotChanged::kId: return as(StallSlotChanged{});
    case StallStateChanged::kId: return as(StallStateChanged{});
    case StallPeopleChanged::kId: return as(StallPeopleChanged{});
    case StallChatLine::kId: return as(StallChatLine{});
    case StallSold::kId: return as(StallSold{});
    case StallBought::kId: return as(StallBought{});
    case StallLeft::kId: return as(StallLeft{});
    case EntityStall::kId: return as(EntityStall{});
    case NetworkPage::kId: return as(NetworkPage{});
    default: return false;
    }
}

inline const char* StallMessageName(MessageId id) {
    switch (id) {
    case StallAnswer::kId: return "StallAnswer";
    case StallView::kId: return "StallView";
    case StallSlotChanged::kId: return "StallSlotChanged";
    case StallStateChanged::kId: return "StallStateChanged";
    case StallPeopleChanged::kId: return "StallPeopleChanged";
    case StallChatLine::kId: return "StallChatLine";
    case StallSold::kId: return "StallSold";
    case StallBought::kId: return "StallBought";
    case StallLeft::kId: return "StallLeft";
    case EntityStall::kId: return "EntityStall";
    case NetworkPage::kId: return "NetworkPage";
    default: return nullptr;
    }
}

} // namespace sr::game::proto
