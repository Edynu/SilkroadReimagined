#include "sr/server/Exchanges.h"

#include <cmath>

#include "sr/assets/GameData.h"
#include "sr/core/Log.h"
#include "sr/game/Mall.h"
#include "sr/game/TimedServices.h"

namespace sr::server {

namespace proto = game::proto;
using Code = proto::ExchangeCode;
using Step = proto::ExchangeStep;

namespace {

// The codes after which the original ends the exchange for both: 2, 3, 5, 0x181B, 0x181C, 0x181F, 0x1820, 0x1823,
// 0x1825, 0x1811, 0x1841.
bool Fatal(Code code) {
    switch (std::uint16_t(code)) {
    case 2:
    case 3:
    case 5:
    case 0x181B:
    case 0x181C:
    case 0x181F:
    case 0x1820:
    case 0x1823:
    case 0x1825:
    case 0x1811:
    case 0x1841: return true;
    default: return false;
    }
}

int FreeBagPlaces(const game::Inventory& bag) {
    int free = 0;
    for (int p = game::slot::kWorn; p < bag.size; ++p) free += bag.places.contains(std::uint8_t(p)) ? 0 : 1;
    return free;
}

} // namespace

int Exchanges::Side::Count() const {
    int n = 0;
    for (const auto& o : offers) n += o ? 1 : 0;
    return n;
}

Exchanges::Side* Exchanges::Session::Of(std::uint32_t entity) {
    if (sides[0].entity == entity) return &sides[0];
    if (sides[1].entity == entity) return &sides[1];
    return nullptr;
}

Exchanges::Exchanges(ExchangeHost& host) : host_(host) {}
Exchanges::~Exchanges() = default;

void Exchanges::Send(std::uint32_t entity, proto::MessageId id, const std::vector<std::byte>& payload) {
    if (const auto t = host_.ExchangeTrader(entity)) host_.ExchangeSend(t->peer, id, payload);
}

template <typename T>
void Exchanges::Send(std::uint32_t entity, const T& message) {
    Send(entity, T::kId, proto::Encode(message));
}

bool Exchanges::Handle(std::uint32_t entity, proto::MessageId id, std::span<const std::byte> payload, Clock::time_point now) {
    const auto decode = [&](auto& m) { return proto::Decode(payload, m); };
    switch (id) {
    case proto::ExchangeAsk::kId:
        if (proto::ExchangeAsk m; decode(m)) return Ask(entity, m.target, now), true;
        return false;
    case proto::ExchangeReply::kId:
        if (proto::ExchangeReply m; decode(m)) return Reply(entity, m.accept != 0), true;
        return false;
    case proto::ExchangePut::kId:
        if (proto::ExchangePut m; decode(m)) return Put(entity, m.place), true;
        return false;
    case proto::ExchangeTake::kId:
        if (proto::ExchangeTake m; decode(m)) return Take(entity, m.slot), true;
        return false;
    case proto::ExchangeGold::kId:
        if (proto::ExchangeGold m; decode(m)) return Gold(entity, m.gold), true;
        return false;
    case proto::ExchangeConfirm::kId:
        if (proto::ExchangeConfirm m; decode(m)) return Confirm(entity), true;
        return false;
    case proto::ExchangeApprove::kId:
        if (proto::ExchangeApprove m; decode(m)) return Approve(entity), true;
        return false;
    case proto::ExchangeCancel::kId:
        if (proto::ExchangeCancel m; decode(m)) return Cancel(entity), true;
        return false;
    default: return false;
    }
}

bool Exchanges::Holding(std::uint32_t entity) const {
    const auto it = byEntity_.find(entity);
    return it != byEntity_.end() && it->second->open;
}

bool Exchanges::Busy(std::uint32_t entity) const { return byEntity_.contains(entity); }

std::size_t Exchanges::Open() const { return byEntity_.size() / 2; }

void Exchanges::Refuse(std::uint32_t entity, Code code) {
    const auto it = byEntity_.find(entity);
    if (Fatal(code) && it != byEntity_.end()) {
        const std::shared_ptr<Session> s = it->second;
        return Close(s, entity, code, code);
    }
    Send(entity, proto::ExchangeAnswer{code});
}

void Exchanges::Close(const std::shared_ptr<Session>& s, std::uint32_t entity, Code own, Code code) {
    for (Side& side : s->sides) {
        byEntity_.erase(side.entity);
        proto::ExchangeClosed closed;
        closed.code = side.entity == entity ? own : code;
        Send(side.entity, closed);
    }
}

// A request to exchange (the original's 0x7081): the checks, then the session opened.
void Exchanges::Ask(std::uint32_t entity, std::uint32_t target, Clock::time_point now) {
    const auto me = host_.ExchangeTrader(entity);
    if (!me) return;
    if (target == entity) return Refuse(entity, Code::InvalidTarget);
    // Its own exchange: an open one stays (the request is ignored); one only asked is given up.
    if (const auto it = byEntity_.find(entity); it != byEntity_.end()) {
        if (it->second->open) return;
        const std::shared_ptr<Session> old = it->second;
        Close(old, entity, Code::Ok, Code::CancelledByUser);
    }
    if (me->alive && me->visitor) return Refuse(entity, Code::AskerStalls);
    if (!me->alive || !me->free) return Refuse(entity, Code::SelfBusy);
    const auto them = host_.ExchangeTrader(target);
    if (!them) return Refuse(entity, Code::InvalidTarget);
    const proto::FlatPosition a = proto::ToFlat(me->position), b = proto::ToFlat(them->position);
    const double dy = double(me->position.y) - double(them->position.y);
    if (std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z) + dy * dy) >= proto::kExchangeReach) return Refuse(entity, Code::TooFar);
    if (!them->alive || !them->free || Busy(target)) return Refuse(entity, Code::Busy);
    auto s = std::make_shared<Session>();
    s->sides[0].entity = entity;
    s->sides[1].entity = target;
    s->answerBy = now + std::chrono::milliseconds(std::int64_t(proto::kExchangeAnswerSeconds * 1000.0));
    byEntity_[entity] = s;
    byEntity_[target] = s;
    core::LogInfo("exchange: {} asks {}", me->name, them->name);
    Send(target, proto::ExchangeAsked{entity, me->name});
}

// The answer: yes opens it, no ends it with 0x1828; the
// asked one dead 0x1840, the asker gone 0x1820.
void Exchanges::Reply(std::uint32_t entity, bool accept) {
    const auto it = byEntity_.find(entity);
    if (it == byEntity_.end() || it->second->open || it->second->sides[1].entity != entity) return;
    const std::shared_ptr<Session> s = it->second;
    const auto asker = host_.ExchangeTrader(s->sides[0].entity);
    const auto asked = host_.ExchangeTrader(entity);
    if (!asker) return Close(s, entity, Code::PeerGone, Code::PeerGone);
    if (!asked || !asked->alive) return Close(s, entity, Code::Cancelled, Code::PeerDead);
    // Still within reach (the original measures again on the yes: beyond it the session ends with 4).
    {
        const proto::FlatPosition a = proto::ToFlat(asker->position), b = proto::ToFlat(asked->position);
        const double dy = double(asker->position.y) - double(asked->position.y);
        if (std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z) + dy * dy) >= proto::kExchangeReach) return Close(s, entity, Code::TooFar, Code::TooFar);
    }
    if (!accept) {
        core::LogInfo("exchange: {} refuses {}", asked->name, asker->name);
        return Close(s, entity, Code::Ok, Code::Refused);
    }
    s->open = true;
    core::LogInfo("exchange: {} and {} open", asker->name, asked->name);
    Send(s->sides[0].entity, proto::ExchangeOpened{entity, asked->name});
    Send(entity, proto::ExchangeOpened{s->sides[0].entity, asker->name});
}

void Exchanges::SendSide(Session& s, Side& side) {
    const auto owner = host_.ExchangeTrader(side.entity);
    if (!owner || owner->inventory == nullptr) return;
    proto::ExchangeSide m;
    m.step = side.step;
    m.gold = side.gold;
    for (std::size_t k = 0; k < side.offers.size(); ++k) {
        if (!side.offers[k] || owner->inventory->At(side.offers[k]->place) == nullptr) continue;
        proto::ItemRecord r = owner->inventory->Record(side.offers[k]->place);
        r.slot = std::uint8_t(k);
        m.items.push_back(r);
        m.places.push_back(side.offers[k]->place);
    }
    m.mine = 1;
    host_.ExchangeSend(owner->peer, proto::ExchangeSide::kId, proto::Encode(m));
    m.mine = 0;
    m.places.clear(); // the other side's bag is its own business
    Send(s.Other(side).entity, m);
}

// and the manager's.
void Exchanges::Put(std::uint32_t entity, std::uint8_t place) {
    const auto it = byEntity_.find(entity);
    if (it == byEntity_.end() || !it->second->open) return;
    const std::shared_ptr<Session> s = it->second;
    Side& side = *s->Of(entity);
    const auto me = host_.ExchangeTrader(entity);
    const auto other = host_.ExchangeTrader(s->Other(side).entity);
    if (!me || me->inventory == nullptr) return;
    if (place < game::slot::kWorn) return Refuse(entity, Code::WornItem);
    const game::ItemInstance* item = me->inventory->At(place);
    if (item == nullptr) return Refuse(entity, Code::NoItem);
    const assets::ItemTable* items = host_.ExchangeItems();
    const assets::ItemRow* row = items != nullptr ? items->Find(item->refId) : nullptr;
    if (row == nullptr || !row->canTrade) return Refuse(entity, Code::CannotTrade);
    if (item->serial == 0) return Refuse(entity, Code::InvalidTarget); // an item never stored
    // A pet's summoning item while its pet is out (0x18B6); a rental whose period runs (the original's "awakening"
    // check, 0x18FA).
    if (host_.ExchangePetOut(entity, place)) return Refuse(entity, Code::PetSummoned);
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (item->rentUntil != 0 && item->rentUntil > now) return Refuse(entity, Code::Rented);
    if (item->bound) return Refuse(entity, Code::Rented); // bound to its character (ours: the beta quests' weapons)
    // A Devil's Spirit while awake: the same (its awakening's end in its data word).
    if (game::IsDevilSpirit(*row) && game::DevilSpiritAwake(item->durability, now)) return Refuse(entity, Code::Rented);
    if (side.step != Step::Open) return Refuse(entity, Code::Locked);
    for (const auto& o : side.offers) {
        if (o && o->place == place) return Refuse(entity, Code::AlreadyOffered);
    }
    const int count = side.Count();
    if (count >= int(proto::kExchangeSlots)) return Refuse(entity, Code::BayFull);
    if (!other || other->inventory == nullptr) return Refuse(entity, Code::PeerGone);
    if (FreeBagPlaces(*other->inventory) < count + 1) return Refuse(entity, Code::PeerBagFull);
    // Trade goods (TID 3/3/8) go by transport only.
    if (row->typeId[0] == 3 && row->typeId[1] == 3 && row->typeId[2] == 8) return Refuse(entity, Code::TradeGoods);
    for (auto& o : side.offers) {
        if (!o) {
            o = Offer{place, *item};
            break;
        }
    }
    SendSide(*s, side);
}

void Exchanges::Take(std::uint32_t entity, std::uint8_t slot) {
    const auto it = byEntity_.find(entity);
    if (it == byEntity_.end() || !it->second->open) return;
    const std::shared_ptr<Session> s = it->second;
    Side& side = *s->Of(entity);
    if (side.step != Step::Open) return Refuse(entity, Code::Locked);
    if (slot >= side.offers.size() || !side.offers[slot]) return Refuse(entity, Code::NoItem);
    side.offers[slot].reset();
    SendSide(*s, side);
}

// positive (ours: 0 takes it back), at most 10^9 and what the player holds.
void Exchanges::Gold(std::uint32_t entity, std::uint64_t gold) {
    const auto it = byEntity_.find(entity);
    if (it == byEntity_.end() || !it->second->open) return;
    const std::shared_ptr<Session> s = it->second;
    Side& side = *s->Of(entity);
    const auto me = host_.ExchangeTrader(entity);
    if (!me || me->inventory == nullptr) return;
    if (gold > std::uint64_t(INT64_MAX)) return Refuse(entity, Code::InvalidNumber);
    if (gold > proto::kExchangeMostGold) return Refuse(entity, Code::Failed);
    if (side.step != Step::Open) return Refuse(entity, Code::Locked);
    if (gold > me->inventory->gold) return Refuse(entity, Code::NotEnoughGold);
    side.gold = gold;
    SendSide(*s, side);
}

// the side's offer fixed; the other hears it with the gold.
void Exchanges::Confirm(std::uint32_t entity) {
    const auto it = byEntity_.find(entity);
    if (it == byEntity_.end() || !it->second->open) return;
    const std::shared_ptr<Session> s = it->second;
    Side& side = *s->Of(entity);
    if (side.step != Step::Open) return; // ours: a second press is not the original's fatal 0x181F (a race of the network)
    side.step = Step::Confirmed;
    SendSide(*s, side);
}

void Exchanges::Approve(std::uint32_t entity) {
    const auto it = byEntity_.find(entity);
    if (it == byEntity_.end() || !it->second->open) return;
    const std::shared_ptr<Session> s = it->second;
    Side& side = *s->Of(entity);
    Side& other = s->Other(side);
    if (side.step == Step::Approved) return; // ours: pressed again while the other has not
    if (side.step != Step::Confirmed || other.step == Step::Open) return Refuse(entity, Code::WrongStep);
    side.step = Step::Approved;
    SendSide(*s, side);
    if (other.step == Step::Approved) Deal(s);
}

void Exchanges::Deal(const std::shared_ptr<Session>& s) {
    const std::optional<ExchangeHost::Trader> traders[2] = {host_.ExchangeTrader(s->sides[0].entity), host_.ExchangeTrader(s->sides[1].entity)};
    for (const auto& t : traders) {
        if (!t || t->inventory == nullptr) return Close(s, 0, Code::PeerGone, Code::PeerGone);
    }
    Side* sides[2] = {&s->sides[0], &s->sides[1]};
    game::Inventory* bags[2] = {traders[0]->inventory, traders[1]->inventory};
    // What each offered is still where it was put (otherwise the original logs an abuse).
    for (int k = 0; k < 2; ++k) {
        for (const auto& o : sides[k]->offers) {
            const game::ItemInstance* item = o ? bags[k]->At(o->place) : nullptr;
            if (o && (item == nullptr || !(*item == o->item))) return Close(s, 0, Code::Abuse, Code::Abuse);
        }
    }
    if (sides[0]->Count() == 0 && sides[1]->Count() == 0 && sides[0]->gold == 0 && sides[1]->gold == 0) {
        return Close(s, 0, Code::NothingOffered, Code::NothingOffered);
    }
    // Each bag has the room for what it receives as it is now, each gold holds.
    for (int k = 0; k < 2; ++k) {
        if (FreeBagPlaces(*bags[k]) < sides[1 - k]->Count()) return Close(s, 0, Code::BagFull, Code::BagFull);
        if (sides[k]->gold > bags[k]->gold) return Close(s, 0, Code::NotEnoughGold, Code::NotEnoughGold);
    }
    // The items leave, then come into the first free places of the other bag; the gold changes hands.
    std::vector<game::ItemInstance> given[2];
    std::vector<std::uint8_t> left[2], arrived[2];
    for (int k = 0; k < 2; ++k) {
        for (const auto& o : sides[k]->offers) {
            if (!o) continue;
            auto item = bags[k]->places.find(o->place);
            given[k].push_back(std::move(item->second));
            bags[k]->places.erase(item);
            left[k].push_back(o->place);
        }
    }
    for (int k = 0; k < 2; ++k) {
        for (game::ItemInstance& item : given[1 - k]) {
            const std::uint8_t place = std::uint8_t(bags[k]->FreeBagPlace());
            bags[k]->places[place] = std::move(item);
            arrived[k].push_back(place);
        }
    }
    const std::uint64_t gold[2] = {sides[0]->gold, sides[1]->gold};
    for (int k = 0; k < 2; ++k) bags[k]->gold = bags[k]->gold - gold[k] + gold[1 - k];
    core::LogInfo("exchange: {} gives {} item(s) and {} gold, {} gives {} item(s) and {} gold", traders[0]->name, left[0].size(), gold[0],
                  traders[1]->name, left[1].size(), gold[1]);
    for (int k = 0; k < 2; ++k) {
        proto::ExchangeClosed done;
        done.done = 1;
        for (std::uint8_t p : arrived[k]) done.arrived.push_back(bags[k]->Record(p));
        for (std::uint8_t p : left[k]) {
            if (bags[k]->At(p) == nullptr) done.left.push_back(p); // a place given away and not filled again
        }
        done.gold = bags[k]->gold;
        host_.ExchangeSend(traders[k]->peer, proto::ExchangeClosed::kId, proto::Encode(done));
        byEntity_.erase(sides[k]->entity);
    }
    std::vector<std::uint8_t> touched[2];
    for (int k = 0; k < 2; ++k) {
        touched[k] = left[k];
        touched[k].insert(touched[k].end(), arrived[k].begin(), arrived[k].end());
    }
    host_.ExchangeSave(traders[0]->character, *bags[0], touched[0], traders[1]->character, *bags[1], touched[1]);
}

// ended with 0x182C for the other.
void Exchanges::Cancel(std::uint32_t entity) {
    const auto it = byEntity_.find(entity);
    if (it == byEntity_.end()) return;
    const std::shared_ptr<Session> s = it->second;
    Close(s, entity, Code::Ok, Code::CancelledByUser);
}

void Exchanges::End(std::uint32_t entity, Code code) {
    const auto it = byEntity_.find(entity);
    if (it == byEntity_.end()) return;
    const std::shared_ptr<Session> s = it->second;
    Close(s, entity, code, code);
}

void Exchanges::Tick(Clock::time_point now) {
    std::vector<std::shared_ptr<Session>> late;
    for (const auto& [entity, s] : byEntity_) {
        if (!s->open && now >= s->answerBy && entity == s->sides[0].entity) late.push_back(s);
    }
    for (const auto& s : late) Close(s, s->sides[1].entity, Code::Timeout, Code::Timeout);
}

} // namespace sr::server
