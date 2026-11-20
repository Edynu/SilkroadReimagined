#include "sr/server/Stalls.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "sr/assets/GameData.h"
#include "sr/assets/StallNetworkData.h"
#include "sr/core/Log.h"
#include "sr/server/ShardBus.h"

namespace sr::server {

namespace proto = game::proto;
using Code = proto::StallCode;

namespace {

// --- The shard's bus (channel sr_stalls_<shard>) --------------------------------

enum : std::uint8_t {
    kBusHello = 1,      // a server has started (the others tell it their rows)
    kBusAlive = 2,      // a server is still there
    kBusBye = 3,        // a server is stopping
    kBusRow = 4,        // a row as it is now, from its stall's server
    kBusRowGone = 5,    // a row taken out of the network
    kBusSaleAsk = 6,    // a purchase from afar, to the stall's server
    kBusSaleAnswer = 7, // its answer, to the buyer's server
};

constexpr auto kAliveEvery = std::chrono::seconds(5);
constexpr auto kPeerSilence = std::chrono::seconds(16); // three heartbeats missed: that server is gone
constexpr std::int64_t kFarSaleMs = 8000;               // a purchase from afar is made within this, or not at all
constexpr auto kSettleAfter = std::chrono::seconds(12); // no answer by then: the buyer's server reads the outcome

struct Envelope {
    std::uint16_t from = 0, to = 0; // to 0: every server
    std::uint8_t kind = 0;
    std::vector<std::uint8_t> payload;
    template <typename V>
    void Visit(V& v) {
        v("from", from);
        v("to", to);
        v("kind", kind);
        v("payload", payload, 3900);
    }
};

struct RowWire {
    std::uint64_t listing = 0;
    std::uint32_t revision = 0;
    std::int64_t character = 0;
    std::string owner;
    std::uint8_t town = 0;
    std::uint16_t region = 0;
    proto::ItemRecord item;
    std::int64_t serial = 0;
    std::uint64_t price = 0;
    std::uint32_t group = 0;
    std::uint8_t degree = 0;
    std::uint8_t grade = 0, sex = 2;
    template <typename V>
    void Visit(V& v) {
        v("listing", listing);
        v("revision", revision);
        v("character", character);
        v("owner", owner, 64);
        v("town", town);
        v("region", region);
        v("item", item);
        v("serial", serial);
        v("price", price);
        v("group", group);
        v("degree", degree);
        v("grade", grade);
        v("sex", sex);
    }
};

struct GoneWire {
    std::uint64_t listing = 0;
    template <typename V>
    void Visit(V& v) {
        v("listing", listing);
    }
};

struct SaleAskWire {
    std::uint32_t request = 0;
    std::int64_t buyer = 0;
    std::string buyerName;
    std::uint64_t listing = 0;
    std::uint32_t revision = 0;
    std::uint64_t price = 0;
    std::uint8_t place = 0;
    std::int64_t deadlineMs = 0;
    template <typename V>
    void Visit(V& v) {
        v("request", request);
        v("buyer", buyer);
        v("buyerName", buyerName, 64);
        v("listing", listing);
        v("revision", revision);
        v("price", price);
        v("place", place);
        v("deadlineMs", deadlineMs);
    }
};

struct SaleAnswerWire {
    std::uint32_t request = 0;
    Code code = Code::Ok;
    proto::ItemRecord item;
    std::int64_t serial = 0;
    template <typename V>
    void Visit(V& v) {
        v("request", request);
        v("code", code);
        v("item", item);
        v("serial", serial);
    }
};

template <typename T>
std::optional<T> DecodeFrom(const std::vector<std::uint8_t>& payload) {
    T out;
    if (!proto::Decode(std::as_bytes(std::span(payload)), out)) return std::nullopt;
    return out;
}

std::int64_t UnixMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::int64_t UnixSeconds() { return UnixMs() / 1000; }

proto::ItemRecord RecordOf(const game::ItemInstance& i, std::uint8_t slot) {
    proto::ItemRecord r;
    r.slot = slot;
    r.item = i.refId;
    r.plus = i.plus;
    r.variance = i.variance;
    r.durability = i.durability;
    r.quantity = i.count;
    for (const game::ItemInstance::Option& o : i.options) r.magic.push_back(std::uint64_t(o.id) | std::uint64_t(o.value) << 32);
    return r;
}

// A name or a line: printable, no control characters, not empty after trimming.
bool PlainText(std::string& text, std::size_t most) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.erase(text.begin());
    if (text.empty() || text.size() > most) return false;
    return std::none_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}

} // namespace

Stalls::Stalls(StallHost& host) : host_(host) {}
Stalls::~Stalls() = default;

std::uint16_t Stalls::Self() const { return host_.StallServer(); }

bool Stalls::Handle(std::uint32_t entity, proto::MessageId id, std::span<const std::byte> payload, Clock::time_point now) {
    // Each request decoded, then done; false when it does not decode.
    const auto with = [&]<typename T>(T message, auto&& run) {
        if (!proto::Decode(payload, message)) return false;
        run(message);
        return true;
    };
    switch (id) {
    case proto::StallOpen::kId: return with(proto::StallOpen{}, [&](proto::StallOpen& m) { OpenStall(entity, std::move(m.name)); });
    case proto::StallClose::kId:
        return with(proto::StallClose{}, [&](proto::StallClose&) {
            if (StallOf(entity) == nullptr) {
                Answer(entity, id, Code::OwnerOnly);
            } else {
                CloseStall(entity, Code::ClosedByHost);
            }
        });
    case proto::StallVisit::kId: return with(proto::StallVisit{}, [&](proto::StallVisit& m) { VisitStall(entity, m.owner); });
    case proto::StallLeave::kId:
        return with(proto::StallLeave{}, [&](proto::StallLeave&) {
            if (!visiting_.contains(entity)) {
                Answer(entity, id, Code::NotVisitor);
            } else {
                LeaveStall(entity, Code::Ok);
            }
        });
    case proto::StallBuy::kId: return with(proto::StallBuy{}, [&](proto::StallBuy& m) { Buy(entity, m.slot); });
    case proto::StallPut::kId: return with(proto::StallPut{}, [&](proto::StallPut& m) { Put(entity, m); });
    case proto::StallTake::kId: return with(proto::StallTake{}, [&](proto::StallTake& m) { Take(entity, m.slot); });
    case proto::StallSetOpen::kId: return with(proto::StallSetOpen{}, [&](proto::StallSetOpen& m) { SetOpen(entity, m.open != 0, m.network != 0); });
    case proto::StallSetText::kId: return with(proto::StallSetText{}, [&](proto::StallSetText& m) { SetText(entity, m.greeting != 0, std::move(m.text)); });
    case proto::StallChat::kId: return with(proto::StallChat{}, [&](proto::StallChat& m) { Chat(entity, std::move(m.text)); });
    case proto::NetworkSearch::kId: return with(proto::NetworkSearch{}, [&](proto::NetworkSearch& m) { NetworkSearch(entity, m, now); });
    case proto::NetworkPageAsk::kId: return with(proto::NetworkPageAsk{}, [&](proto::NetworkPageAsk& m) { NetworkPage(entity, m.page); });
    case proto::NetworkBuy::kId: return with(proto::NetworkBuy{}, [&](proto::NetworkBuy& m) { NetworkBuy(entity, m); });
    case proto::NetworkClose::kId: return with(proto::NetworkClose{}, [&](proto::NetworkClose&) { searches_.erase(entity); });
    default: return false;
    }
}

// --- The stall (the original's) -------------------------------------------------------------------------

void Stalls::OpenStall(std::uint32_t entity, std::string name) {
    const auto p = host_.StallPlayer(entity);
    if (!p) return;
    const auto refuse = [&](Code code) { Answer(entity, proto::StallOpen::kId, code); };
    if (stalls_.contains(entity) || visiting_.contains(entity) || waiting_.contains(entity) || !p->alive || !p->free) return refuse(Code::Busy);
    if (p->riding) return refuse(Code::Riding);
    if (p->murderer) return refuse(Code::Murderer);
    if (p->jobSuit) return refuse(Code::JobSuit);
    if (!PlainText(name, proto::kStallNameLength)) return refuse(Code::BadName);
    Stall& s = stalls_[entity];
    s.owner = entity;
    s.character = p->character;
    s.ownerName = p->name;
    s.name = std::move(name);
    s.town = p->town;
    s.skin = p->skin;
    s.region = p->position.region;
    searches_.erase(entity);
    Send(entity, ViewOf(s, true));
    host_.StallShow(proto::EntityStall{entity, s.name, s.skin});
    core::LogInfo("stall: {} opens \"{}\"{}", s.ownerName, s.name, s.town >= 0 ? " in a town" : "");
}

void Stalls::CloseStall(std::uint32_t owner, Code code) {
    const auto it = stalls_.find(owner);
    if (it == stalls_.end()) return;
    Stall& s = it->second;
    Unlist(s);
    for (const std::uint32_t v : s.visitors) {
        visiting_.erase(v);
        Send(v, proto::StallLeft{code});
    }
    Send(owner, proto::StallLeft{code == Code::ClosedByHost ? Code::Ok : code});
    host_.StallShow(proto::EntityStall{owner, {}, 0});
    core::LogInfo("stall: {}'s \"{}\" closes", s.ownerName, s.name);
    stalls_.erase(it);
}

void Stalls::VisitStall(std::uint32_t entity, std::uint32_t owner) {
    const auto refuse = [&](Code code) { Answer(entity, proto::StallVisit::kId, code); };
    Stall* s = StallOf(owner);
    if (s == nullptr) return refuse(Code::Closed);
    if (owner == entity) return refuse(Code::OwnerOnly);
    const auto p = host_.StallPlayer(entity);
    const auto o = host_.StallPlayer(owner);
    if (!p || !o) return refuse(Code::Closed);
    if (stalls_.contains(entity) || visiting_.contains(entity) || waiting_.contains(entity) || !p->alive || !p->free) return refuse(Code::Busy);
    const double dx = double(p->position.x) - o->position.x, dz = double(p->position.z) - o->position.z;
    // The original's 100 between the bodies (its hit range); ours between the centres with a body's
    // room on each side, the walk up stopping short of the booth.
    if (p->position.region != o->position.region || std::sqrt(dx * dx + dz * dz) > proto::kStallReach + 20.0) return refuse(Code::Busy);
    if (s->visitors.size() + 1 >= proto::kStallPeople) return refuse(Code::Full);
    s->visitors.push_back(entity);
    visiting_[entity] = owner;
    Send(entity, ViewOf(*s, false));
    SendAll(*s, proto::StallPeopleChanged{1, {entity, p->name}}, entity);
}

void Stalls::LeaveStall(std::uint32_t entity, Code code) {
    const auto it = visiting_.find(entity);
    if (it == visiting_.end()) return;
    Stall* s = StallOf(it->second);
    visiting_.erase(it);
    Send(entity, proto::StallLeft{code});
    if (s == nullptr) return;
    std::erase(s->visitors, entity);
    const auto p = host_.StallPlayer(entity);
    SendAll(*s, proto::StallPeopleChanged{0, {entity, p ? p->name : std::string()}});
}

void Stalls::Leave(std::uint32_t entity, Code code) {
    searches_.erase(entity);
    waiting_.erase(entity);
    if (stalls_.contains(entity)) return CloseStall(entity, code);
    if (visiting_.contains(entity)) LeaveStall(entity, Code::Ok);
}

void Stalls::Walked(std::uint32_t entity) {
    if (visiting_.contains(entity)) LeaveStall(entity, Code::Ok);
}

void Stalls::SkinChanged(std::uint32_t entity, std::uint32_t skin) {
    Stall* s = StallOf(entity);
    if (s == nullptr) return;
    s->skin = skin;
    host_.StallShow(proto::EntityStall{entity, s->name, s->skin});
}

bool Stalls::Owner(std::uint32_t entity) const { return stalls_.contains(entity); }
bool Stalls::Visiting(std::uint32_t entity) const { return visiting_.contains(entity); }
bool Stalls::Holding(std::uint32_t entity) const { return stalls_.contains(entity) || waiting_.contains(entity); }

Code Stalls::Sellable(const game::ItemInstance& item) const {
    const assets::ItemTable* items = host_.StallItems();
    const assets::ItemRow* row = items != nullptr ? items->Find(item.refId) : nullptr;
    if (row == nullptr || !row->canTrade) return Code::NotBagItem;
    if (item.rentUntil != 0 && item.rentUntil > UnixSeconds()) return Code::Rented;
    if (item.bound) return Code::Rented; // bound to its character (ours: the beta quests' weapons)
    // Trade goods (TID 3/3/8) go by transport only, as in the exchange.
    if (row->typeId[0] == 3 && row->typeId[1] == 3 && row->typeId[2] == 8) return Code::NotBagItem;
    return Code::Ok;
}

void Stalls::Put(std::uint32_t entity, const proto::StallPut& put) {
    const auto refuse = [&](Code code) { Answer(entity, proto::StallPut::kId, code); };
    Stall* s = StallOf(entity);
    if (s == nullptr) return refuse(Code::OwnerOnly);
    if (s->open) return refuse(Code::Modifying); // a change only while modifying
    if (put.slot >= proto::kStallSlots) return refuse(Code::SlotEmpty);
    if (put.price < 1 || put.price > proto::kStallMostPrice) return refuse(Code::BadPrice);
    const auto p = host_.StallPlayer(entity);
    if (!p) return;
    std::optional<Slot>& slot = s->slots[put.slot];
    if (put.place == 0) { // the price of what is there
        if (!slot) return refuse(Code::SlotEmpty);
        slot->price = put.price;
    } else {
        if (put.place < 13 || put.place >= p->inventory->size) return refuse(Code::NotBagItem);
        const game::ItemInstance* item = p->inventory->At(put.place);
        if (item == nullptr) return refuse(Code::NotBagItem);
        if (const Code why = Sellable(*item); why != Code::Ok) return refuse(why);
        if (slot) return refuse(Code::Occupied);
        if (std::any_of(s->slots.begin(), s->slots.end(), [&](const std::optional<Slot>& o) { return o && o->place == put.place; })) {
            return refuse(Code::Occupied);
        }
        slot = Slot{put.place, *item, put.price, 0};
    }
    slot->revision = ++s->revisions;
    for (const std::uint32_t who : People(*s)) Send(who, proto::StallSlotChanged{1, ItemOf(*s, put.slot, who == s->owner)});
}

void Stalls::Take(std::uint32_t entity, std::uint8_t index) {
    const auto refuse = [&](Code code) { Answer(entity, proto::StallTake::kId, code); };
    Stall* s = StallOf(entity);
    if (s == nullptr) return refuse(Code::OwnerOnly);
    if (s->open) return refuse(Code::Modifying);
    if (index >= proto::kStallSlots || !s->slots[index]) return refuse(Code::SlotEmpty);
    s->slots[index].reset();
    proto::StallSlotChanged gone;
    gone.item.slot = index;
    SendAll(*s, gone);
}

void Stalls::SetOpen(std::uint32_t entity, bool open, bool network) {
    Stall* s = StallOf(entity);
    if (s == nullptr) return Answer(entity, proto::StallSetOpen::kId, Code::OwnerOnly);
    if (open && std::none_of(s->slots.begin(), s->slots.end(), [](const std::optional<Slot>& o) { return o.has_value(); })) {
        return Answer(entity, proto::StallSetOpen::kId, Code::NothingToSell);
    }
    if (s->selling && !open) return Answer(entity, proto::StallSetOpen::kId, Code::SaleUnderWay);
    s->open = open;
    std::uint16_t registered = 0;
    if (open) {
        s->network = network && s->town >= 0;
        List(*s);
        if (network) registered = s->town >= 0 ? 1 : std::uint16_t(Code::NotTown);
    } else {
        s->network = false;
        Unlist(*s);
    }
    for (const std::uint32_t who : People(*s)) {
        Send(who, proto::StallStateChanged{std::uint8_t(open), s->name, s->greeting, who == s->owner ? registered : std::uint16_t(0)});
    }
}

void Stalls::SetText(std::uint32_t entity, bool greeting, std::string text) {
    Stall* s = StallOf(entity);
    const proto::MessageId id = proto::StallSetText::kId;
    if (s == nullptr) return Answer(entity, id, Code::OwnerOnly);
    if (greeting) {
        // The greeting may be empty and changes at any time (0x70BA 6).
        while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
        if (text.size() > proto::kStallGreetingLength || std::any_of(text.begin(), text.end(), [](char c) { return c != '\n' && static_cast<unsigned char>(c) < 0x20; })) {
            return Answer(entity, id, Code::BadName);
        }
        s->greeting = std::move(text);
    } else {
        if (s->open) return Answer(entity, id, Code::Modifying); // 0x70BA 7: only while modifying
        if (!PlainText(text, proto::kStallNameLength)) return Answer(entity, id, Code::BadName);
        s->name = std::move(text);
        host_.StallShow(proto::EntityStall{entity, s->name, s->skin});
    }
    SendAll(*s, proto::StallStateChanged{std::uint8_t(s->open), s->name, s->greeting, 0});
}

void Stalls::Chat(std::uint32_t entity, std::string text) {
    Stall* s = StallOf(entity);
    if (s == nullptr) s = VisitedBy(entity);
    if (s == nullptr || !PlainText(text, 256)) return;
    const auto p = host_.StallPlayer(entity);
    if (!p) return;
    SendAll(*s, proto::StallChatLine{p->name, std::move(text)});
}

void Stalls::Buy(std::uint32_t entity, std::uint8_t slot) {
    Stall* s = VisitedBy(entity);
    if (s == nullptr) {
        // The owner buying from its own stall is told so quietly (5); anyone else is not a visitor.
        return Send(entity, proto::StallBought{StallOf(entity) != nullptr ? Code::Self : Code::NotVisitor, 0, {}, 0});
    }
    if (const Code code = SellHere(*s, slot, entity, false); code != Code::Ok) Send(entity, proto::StallBought{code, 0, {}, 0});
}

Code Stalls::SellHere(Stall& s, std::uint8_t index, std::uint32_t buyer, bool network) {
    if (buyer == s.owner) return Code::Self;
    if (!s.open) return Code::NotOpen;
    if (s.selling) return Code::SaleUnderWay;
    if (index >= proto::kStallSlots || !s.slots[index]) return Code::SlotEmpty;
    const auto o = host_.StallPlayer(s.owner);
    const auto b = host_.StallPlayer(buyer);
    if (!o || !b) return Code::Closed;
    if (!b->alive || stalls_.contains(buyer) || waiting_.contains(buyer)) return Code::Busy;
    Slot& slot = *s.slots[index];
    const game::ItemInstance* there = o->inventory->At(slot.place);
    if (there == nullptr || there->serial != slot.item.serial || *there != slot.item) {
        // The bag no longer holds it as it was put up (it cannot move while the stall stands; a guard).
        s.slots[index].reset();
        Unlist(s, index);
        return Code::SlotEmpty;
    }
    if (b->inventory->gold < slot.price) return network ? Code::NetworkNoGold : Code::NotEnoughGold;
    const int place = b->inventory->FreeBagPlace();
    if (place < 0) return network ? Code::NetworkBagFull : Code::BagFull;
    // The item (its whole stack) and the gold change hands; both sides saved in one transaction.
    const std::uint64_t price = slot.price;
    const std::uint64_t commission = network ? price * proto::kNetworkCommissionPercent / 100 : 0;
    const std::uint8_t ownerPlace = slot.place;
    b->inventory->places[std::uint8_t(place)] = *there;
    o->inventory->places.erase(ownerPlace);
    o->inventory->gold += price - commission;
    b->inventory->gold -= price;
    host_.StallSaveSale(o->character, *o->inventory, ownerPlace, b->character, *b->inventory, std::uint8_t(place));
    s.slots[index].reset();
    Unlist(s, index);
    core::LogInfo("stall: {} buys slot {} of {}'s stall for {}{}", b->name, index, s.ownerName, price, network ? " through the network" : "");
    Send(buyer, proto::StallBought{Code::Ok, std::uint8_t(network), b->inventory->Record(std::uint8_t(place)), b->inventory->gold});
    for (const std::uint32_t who : People(s)) {
        if (who == s.owner) {
            Send(who, proto::StallSold{index, b->name, std::uint8_t(network), ownerPlace, o->inventory->gold, commission});
        } else {
            Send(who, proto::StallSold{index, b->name, std::uint8_t(network), 0, 0, 0});
        }
    }
    return Code::Ok;
}

// --- The network (ours) ----------------------------------------------------------------------------------

void Stalls::NetworkSearch(std::uint32_t entity, const proto::NetworkSearch& m, Clock::time_point now) {
    const auto p = host_.StallPlayer(entity);
    if (!p) return;
    const auto refuse = [&](Code code) { Send(entity, proto::NetworkPage{code, 0, 0, 0, {}}); };
    if (p->town < 0) return refuse(Code::NotTownSearch);
    if (const auto it = searches_.find(entity); it != searches_.end() && now - it->second.at < std::chrono::duration<double>(proto::kNetworkSearchSeconds)) {
        return refuse(Code::TooSoon);
    }
    Search& search = searches_[entity];
    search.at = now;
    search.matches.clear();
    // A type, or (ours) a name: an empty search would list the whole shard.
    std::string name = m.name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (m.group == 0 && name.empty()) return refuse(Code::NetworkFailed);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> found; // price, listing
    for (auto& [listing, row] : index_) {
        if (m.group != 0 && row.group != m.group) continue;
        if (m.degree != 0 && row.degree != m.degree) continue;
        if (m.grade != proto::NetworkGrade::All && row.grade != std::uint8_t(m.grade)) continue;
        if (m.sex == proto::NetworkSex::Male && row.sex == 0) continue;
        if (m.sex == proto::NetworkSex::Female && row.sex == 1) continue;
        if (!name.empty()) {
            if (row.name.empty()) row.name = host_.StallItemName(row.item.item); // rows told by another server
            if (row.name.find(name) == std::string::npos) continue;
        }
        found.emplace_back(row.price, listing);
    }
    std::sort(found.begin(), found.end());
    for (const auto& f : found) search.matches.push_back(f.second);
    NetworkPage(entity, 0);
}

void Stalls::NetworkPage(std::uint32_t entity, std::uint16_t page) {
    const auto it = searches_.find(entity);
    if (it == searches_.end()) return Send(entity, proto::NetworkPage{Code::NetworkFailed, 0, 0, 0, {}});
    const std::vector<std::uint64_t>& matches = it->second.matches;
    proto::NetworkPage out;
    out.page = page;
    out.total = std::uint32_t(matches.size());
    out.pages = std::uint16_t((matches.size() + proto::kNetworkPageRows - 1) / proto::kNetworkPageRows);
    for (std::size_t i = std::size_t(page) * proto::kNetworkPageRows; i < matches.size() && i < (std::size_t(page) + 1) * proto::kNetworkPageRows; ++i) {
        const auto row = index_.find(matches[i]);
        if (row == index_.end()) continue; // sold or taken back since the search
        out.rows.push_back(
            proto::NetworkRow{row->first, row->second.revision, row->second.owner, row->second.town, row->second.region, row->second.item, row->second.price});
    }
    Send(entity, out);
}

void Stalls::NetworkBuy(std::uint32_t entity, const proto::NetworkBuy& buy) {
    const auto p = host_.StallPlayer(entity);
    if (!p) return;
    const auto refuse = [&](Code code) { Send(entity, proto::StallBought{code, 1, {}, 0}); };
    if (p->town < 0) return refuse(Code::NotTownSearch);
    if (!p->alive || !p->free || stalls_.contains(entity) || visiting_.contains(entity) || waiting_.contains(entity)) return refuse(Code::Busy);
    const auto row = index_.find(buy.listing);
    if (row == index_.end() || row->second.revision != buy.revision || row->second.price != buy.price) return refuse(Code::Changed);
    const std::uint8_t index = std::uint8_t(buy.listing & 0xFF);
    if (row->second.server == Self()) {
        for (auto& [owner, s] : stalls_) {
            if (s.character != row->second.character) continue;
            if (const Code code = SellHere(s, index, entity, true); code != Code::Ok) refuse(code == Code::SlotEmpty ? Code::Changed : code);
            return;
        }
        return refuse(Code::Changed);
    }
    // Its stall stands on another server: the gold set aside and saved, the bag held, the sale asked there.
    if (!host_.StallOnBus()) return refuse(Code::NetworkFailed);
    if (p->inventory->gold < buy.price) return refuse(Code::NetworkNoGold);
    const int place = p->inventory->FreeBagPlace();
    if (place < 0) return refuse(Code::NetworkBagFull);
    p->inventory->gold -= buy.price;
    const std::uint32_t request = ++requests_;
    Pending& w = pending_[request];
    w.buyer = entity;
    w.character = p->character;
    w.listing = buy.listing;
    w.price = buy.price;
    w.serial = row->second.serial;
    w.item = row->second.item;
    w.place = std::uint8_t(place);
    w.server = row->second.server;
    w.deadlineMs = UnixMs() + kFarSaleMs;
    w.settleAt = Clock::now() + kSettleAfter;
    waiting_[entity] = request;
    const SaleAskWire ask{request, p->character, p->name, buy.listing, buy.revision, buy.price, std::uint8_t(place), w.deadlineMs};
    host_.StallSaveGold(p->character, p->inventory->gold, [this, request, ask] {
        const auto it = pending_.find(request);
        if (it == pending_.end()) return;
        it->second.asked = true;
        Publish(kBusSaleAsk, it->second.server, proto::Encode(ask));
    });
    core::LogInfo("stall: {} buys listing {:x} from server {} through the network", p->name, buy.listing, w.server);
}

void Stalls::SellFar(std::uint16_t from, std::uint32_t request, std::int64_t buyer, const std::string& buyerName, std::uint64_t listing,
                     std::uint32_t revision, std::uint64_t price, std::uint8_t place, std::int64_t deadlineMs) {
    const auto answer = [&](Code code, const proto::ItemRecord& item = {}, std::int64_t serial = 0) {
        Publish(kBusSaleAnswer, from, proto::Encode(SaleAnswerWire{request, code, item, serial}));
    };
    const auto row = index_.find(listing);
    if (row == index_.end() || row->second.server != Self() || row->second.revision != revision || row->second.price != price) return answer(Code::Changed);
    const std::uint8_t index = std::uint8_t(listing & 0xFF);
    Stall* s = nullptr;
    for (auto& [owner, stall] : stalls_) {
        if (stall.character == row->second.character) s = &stall;
    }
    if (s == nullptr || !s->open || index >= proto::kStallSlots || !s->slots[index]) return answer(Code::Changed);
    if (s->selling) return answer(Code::SaleUnderWay);
    const auto o = host_.StallPlayer(s->owner);
    if (!o) return answer(Code::Changed);
    const Slot slot = *s->slots[index];
    const game::ItemInstance* there = o->inventory->At(slot.place);
    if (there == nullptr || there->serial != slot.item.serial || *there != slot.item || slot.item.serial == 0) return answer(Code::Changed);
    s->selling = true;
    const std::uint64_t commission = price * proto::kNetworkCommissionPercent / 100;
    StallHost::FarSale sale{o->character, buyer, slot.item.serial, slot.place, place, o->inventory->gold + price - commission, deadlineMs};
    const std::uint32_t owner = s->owner;
    const std::int64_t ownerCharacter = o->character;
    const proto::ItemRecord record = RecordOf(slot.item, place);
    host_.StallSaveFarSale(sale, [this, from, request, owner, ownerCharacter, index, slot, price, commission, record, buyerName](bool made) {
        Stall* s = StallOf(owner);
        if (s != nullptr && s->character == ownerCharacter) s->selling = false;
        if (!made) {
            Publish(kBusSaleAnswer, from, proto::Encode(SaleAnswerWire{request, Code::Changed, {}, 0}));
            return;
        }
        // Made: the owner's memory follows what was saved (its bag and gold were held meanwhile); an owner
        // that left meanwhile had its gold saved without the price after the sale, so it is added there.
        const auto holder = host_.StallPlayer(owner);
        const bool present = holder && holder->character == ownerCharacter;
        if (present) {
            holder->inventory->places.erase(slot.place);
            holder->inventory->gold += price - commission;
        } else {
            host_.StallRefundOffline(ownerCharacter, price - commission);
        }
        if (s != nullptr && s->character == ownerCharacter) {
            s->slots[index].reset();
            Unlist(*s, index);
            for (const std::uint32_t who : People(*s)) {
                Send(who, who == s->owner ? proto::StallSold{index, buyerName, 1, slot.place, present ? holder->inventory->gold : 0, commission}
                                          : proto::StallSold{index, buyerName, 1, 0, 0, 0});
            }
        } else {
            PublishGone(StallListing(ownerCharacter, index));
            index_.erase(StallListing(ownerCharacter, index));
        }
        Publish(kBusSaleAnswer, from, proto::Encode(SaleAnswerWire{request, Code::Ok, record, slot.item.serial}));
    });
}

void Stalls::FarAnswered(std::uint32_t request, Code code, const proto::ItemRecord& item, std::int64_t serial) {
    const auto it = pending_.find(request);
    if (it == pending_.end() || it->second.settling) return;
    Settle(request, code == Code::Ok, item, serial);
}

void Stalls::Settle(std::uint32_t request, bool landed, const proto::ItemRecord& item, std::int64_t serial) {
    const auto it = pending_.find(request);
    if (it == pending_.end()) return;
    const Pending w = it->second;
    pending_.erase(it);
    if (const auto wait = waiting_.find(w.buyer); wait != waiting_.end() && wait->second == request) waiting_.erase(wait);
    const auto b = host_.StallPlayer(w.buyer);
    const bool here = b && b->character == w.character;
    if (landed) {
        if (!here) return; // the database holds it in the bag already
        game::ItemInstance got = game::Inventory::FromRecord(item);
        got.serial = serial != 0 ? serial : w.serial;
        b->inventory->places[w.place] = got;
        Send(w.buyer, proto::StallBought{Code::Ok, 1, b->inventory->Record(w.place), b->inventory->gold});
        core::LogInfo("stall: {} got listing {:x} through the network", b->name, w.listing);
        return;
    }
    if (!here) return host_.StallRefundOffline(w.character, w.price);
    b->inventory->gold += w.price;
    host_.StallSaveGold(w.character, b->inventory->gold, [] {});
    Send(w.buyer, proto::StallBought{Code::Changed, 1, {}, b->inventory->gold});
}

void Stalls::Tick(Clock::time_point now) {
    for (auto& [request, w] : pending_) {
        if (!w.asked || w.settling || now < w.settleAt) continue;
        // No answer: what the database says settles it (after the deadline on its own clock).
        w.settling = true;
        const proto::ItemRecord item = w.item;
        const std::int64_t serial = w.serial;
        const std::uint32_t id = request;
        host_.StallSettle(w.character, w.place, w.serial, w.deadlineMs, [this, id, item, serial](bool landed) { Settle(id, landed, item, serial); });
    }
    if (!host_.StallOnBus()) return;
    if (now >= nextAlive_) {
        nextAlive_ = now + kAliveEvery;
        Publish(kBusAlive, 0, {});
    }
    std::vector<std::uint16_t> quiet;
    for (const auto& [server, heard] : peers_) {
        if (now - heard > kPeerSilence) quiet.push_back(server);
    }
    for (const std::uint16_t server : quiet) ServerGone(server);
}

void Stalls::List(Stall& s) {
    if (!s.open || !s.network || s.town < 0) return;
    const assets::ItemTable* items = host_.StallItems();
    const assets::StallNetworkData* tables = host_.StallNetworkTables();
    if (items == nullptr || tables == nullptr) return;
    for (std::uint8_t i = 0; i < proto::kStallSlots; ++i) {
        if (!s.slots[i]) continue;
        const Slot& slot = *s.slots[i];
        const assets::ItemRow* item = items->Find(slot.item.refId);
        if (item == nullptr) continue;
        Row row;
        row.server = Self();
        row.revision = slot.revision;
        row.character = s.character;
        row.owner = s.ownerName;
        row.town = std::uint8_t(s.town);
        row.region = s.region;
        row.item = RecordOf(slot.item, 0);
        row.serial = slot.item.serial;
        row.price = slot.price;
        row.group = tables->GroupOf(std::uint8_t(item->typeId[0]), std::uint8_t(item->typeId[1]), std::uint8_t(item->typeId[2]), std::uint8_t(item->typeId[3]));
        row.degree = std::uint8_t(item->itemClass > 0 ? (item->itemClass - 1) / 3 + 1 : 0);
        row.grade = std::uint8_t(item->Rare() ? 3 : slot.item.options.empty() ? 1 : 2);
        row.sex = std::uint8_t(std::clamp(item->sex, 0, 2));
        row.name = host_.StallItemName(slot.item.refId);
        if (row.group == 0) continue; // in no group: the network cannot find it
        const std::uint64_t listing = StallListing(s.character, i);
        index_[listing] = row;
        PublishRow(listing, row);
    }
}

void Stalls::Unlist(Stall& s, int slot) {
    for (int i = 0; i < int(proto::kStallSlots); ++i) {
        if (slot >= 0 && i != slot) continue;
        const std::uint64_t listing = StallListing(s.character, std::uint8_t(i));
        const auto it = index_.find(listing);
        if (it == index_.end() || it->second.server != Self()) continue;
        index_.erase(it);
        PublishGone(listing);
    }
}

void Stalls::PublishRow(std::uint64_t listing, const Row& row, std::uint16_t to) {
    Publish(kBusRow, to,
            proto::Encode(RowWire{listing, row.revision, row.character, row.owner, row.town, row.region, row.item, row.serial, row.price, row.group, row.degree,
                                  row.grade, row.sex}));
}

void Stalls::PublishGone(std::uint64_t listing) { Publish(kBusRowGone, 0, proto::Encode(GoneWire{listing})); }

void Stalls::ServerGone(std::uint16_t server) {
    peers_.erase(server);
    std::erase_if(index_, [&](const auto& r) { return r.second.server == server; });
    // Purchases asked of it settle by the database at their time (Tick).
}

void Stalls::Hello() { Publish(kBusHello, 0, {}); }

std::string Stalls::GoodbyeText() const {
    if (!host_.StallOnBus()) return {};
    Envelope e;
    e.from = Self();
    e.kind = kBusBye;
    return BusToHex(proto::Encode(e));
}

void Stalls::Publish(std::uint8_t kind, std::uint16_t to, std::vector<std::byte> payload) {
    if (!host_.StallOnBus()) return;
    Envelope e;
    e.from = Self();
    e.to = to;
    e.kind = kind;
    e.payload.resize(payload.size());
    std::memcpy(e.payload.data(), payload.data(), payload.size());
    host_.StallPublish(BusToHex(proto::Encode(e)));
}

void Stalls::BusMessage(const std::string& text, Clock::time_point now) {
    const auto bytes = BusFromHex(text);
    if (!bytes) return;
    Envelope e;
    if (!proto::Decode(*bytes, e) || e.from == Self()) return;
    if (e.kind == kBusBye) return ServerGone(e.from);
    if (e.kind == kBusHello) {
        // It has started (again: what it had is gone); it is told this server's rows.
        if (peers_.contains(e.from)) ServerGone(e.from);
        peers_[e.from] = now;
        Publish(kBusAlive, e.from, {});
        for (const auto& [listing, row] : index_) {
            if (row.server == Self()) PublishRow(listing, row, e.from);
        }
        return;
    }
    peers_[e.from] = now;
    if (e.to != 0 && e.to != Self()) return;
    switch (e.kind) {
    case kBusRow:
        if (const auto w = DecodeFrom<RowWire>(e.payload)) {
            index_[w->listing] = Row{e.from, w->revision, w->character, w->owner, w->town, w->region, w->item, w->serial, w->price, w->group, w->degree,
                                     w->grade, w->sex, {}};
        }
        return;
    case kBusRowGone:
        if (const auto w = DecodeFrom<GoneWire>(e.payload)) {
            if (const auto it = index_.find(w->listing); it != index_.end() && it->second.server == e.from) index_.erase(it);
        }
        return;
    case kBusSaleAsk:
        if (const auto w = DecodeFrom<SaleAskWire>(e.payload)) SellFar(e.from, w->request, w->buyer, w->buyerName, w->listing, w->revision, w->price, w->place, w->deadlineMs);
        return;
    case kBusSaleAnswer:
        if (const auto w = DecodeFrom<SaleAnswerWire>(e.payload)) FarAnswered(w->request, w->code, w->item, w->serial);
        return;
    default: return;
    }
}

// --- Helpers --------------------------------------------------------------------------------------------

Stalls::Stall* Stalls::StallOf(std::uint32_t owner) {
    const auto it = stalls_.find(owner);
    return it != stalls_.end() ? &it->second : nullptr;
}

Stalls::Stall* Stalls::VisitedBy(std::uint32_t entity) {
    const auto it = visiting_.find(entity);
    return it != visiting_.end() ? StallOf(it->second) : nullptr;
}

std::vector<std::uint32_t> Stalls::People(const Stall& s) const {
    std::vector<std::uint32_t> out{s.owner};
    out.insert(out.end(), s.visitors.begin(), s.visitors.end());
    return out;
}

proto::StallItem Stalls::ItemOf(const Stall& s, std::uint8_t index, bool owner) {
    proto::StallItem out;
    out.slot = index;
    if (!s.slots[index]) return out;
    const Slot& slot = *s.slots[index];
    out.item = RecordOf(slot.item, owner ? slot.place : 0);
    out.price = slot.price;
    return out;
}

proto::StallView Stalls::ViewOf(const Stall& s, bool owner) {
    proto::StallView v;
    v.owner = s.owner;
    v.ownerName = s.ownerName;
    v.name = s.name;
    v.greeting = s.greeting;
    v.open = std::uint8_t(s.open);
    for (std::uint8_t i = 0; i < proto::kStallSlots; ++i) {
        if (s.slots[i]) v.items.push_back(ItemOf(s, i, owner));
    }
    for (const std::uint32_t visitor : s.visitors) {
        const auto p = host_.StallPlayer(visitor);
        v.people.push_back({visitor, p ? p->name : std::string()});
    }
    return v;
}

void Stalls::Answer(std::uint32_t entity, proto::MessageId request, Code code) { Send(entity, proto::StallAnswer{request, code}); }

void Stalls::Send(std::uint32_t entity, proto::MessageId id, const std::vector<std::byte>& payload) {
    if (const auto p = host_.StallPlayer(entity)) host_.StallSend(p->peer, id, payload);
}

template <typename T>
void Stalls::Send(std::uint32_t entity, const T& message) {
    Send(entity, T::kId, proto::Encode(message));
}

template <typename T>
void Stalls::SendAll(const Stall& s, const T& message, std::uint32_t except) {
    const std::vector<std::byte> payload = proto::Encode(message);
    for (const std::uint32_t who : People(s)) {
        if (who != except) Send(who, T::kId, payload);
    }
}

} // namespace sr::server
