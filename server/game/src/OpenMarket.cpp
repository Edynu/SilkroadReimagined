#include "sr/server/OpenMarket.h"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

#include "sr/assets/GameData.h"
#include "sr/core/Log.h"
#include "sr/server/Database.h"
#include "sr/server/ShardBus.h"
#include "sr/server/Store.h"

namespace sr::server {

namespace proto = game::proto;
namespace om = game::openmarket;
namespace code = game::openmarket::code;

namespace {

// What crosses the shard's bus (hex of its encoding): a listing sold, for its seller's server.
struct Envelope {
    std::uint16_t from = 0;
    std::uint8_t kind = 0; // 1 sold
    std::int64_t account = 0;
    std::uint32_t item = 0;
    template <typename V>
    void Visit(V& v) {
        v("from", from);
        v("kind", kind);
        v("account", account);
        v("item", item);
    }
};

proto::ItemRecord RecordOf(const StoredItem& s) {
    proto::ItemRecord r;
    r.slot = 0;
    r.item = s.refId;
    r.plus = s.plus;
    r.variance = s.variance;
    r.durability = s.durability;
    r.quantity = s.quantity;
    r.magic = s.magic;
    r.advanced = s.advanced;
    for (std::uint8_t k = 0; k < game::kSockets; ++k) {
        const auto& socket = s.sockets[k];
        if (socket.made) r.sockets.push_back({k, socket.skill, socket.level, socket.stone});
    }
    return r;
}

} // namespace

struct OpenMarket::State {
    struct Session {
        std::uint32_t npc = 0;
        // The last search's rows (a purchase names one of them): account << 32 | listing -> the price.
        std::map<std::uint64_t, std::uint64_t> searched;
        std::vector<OpenMarketRecord> listings; // the player's, as last loaded
    };
    struct Online {
        std::int64_t account = 0, character = 0;
        Clock::time_point nextAge{};
    };

    OpenMarketHost& host;
    OpenMarketConfig config;
    std::unordered_map<std::uint64_t, Session> sessions; // by connection, while the market is open
    std::unordered_map<std::uint64_t, Online> online;    // by connection
    std::set<std::uint32_t> busy;                        // entities with a request under way
    int writes = 0;

    State(OpenMarketHost& h, OpenMarketConfig c) : host(h), config(std::move(c)) {}

    template <typename T>
    void Send(std::uint64_t peer, const T& message) {
        host.OpenMarketSend(peer, T::kId, proto::Encode(message));
    }
    void Answer(std::uint64_t peer, proto::MessageId request, std::uint16_t c) { Send(peer, proto::OpenMarketAnswer{request, c}); }

    proto::OpenMarketState StateFor(std::uint64_t peer, bool open) const {
        proto::OpenMarketState s;
        s.open = open ? 1 : 0;
        for (const int f : config.fees.listing) s.listingFees.push_back(std::uint8_t(std::clamp(f, 0, 100)));
        for (const int f : config.fees.sale) s.saleFees.push_back(std::uint8_t(std::clamp(f, 0, 100)));
        std::uint32_t ticketItem = 0;
        const om::Ticket ticket = host.OpenMarketTicket(peer, ticketItem);
        s.room = std::uint16_t(om::kListings + std::max(0, ticket.room));
        s.ticketSale = std::int8_t(std::clamp(ticket.salePercent, -1, 100));
        return s;
    }

    // The account's listings loaded (aged first), kept on its session and told.
    void Refresh(std::uint64_t peer, std::function<void()> after = {}) {
        const auto actor = host.OpenMarketActorOf(peer);
        if (!actor) return;
        struct Loaded {
            std::vector<OpenMarketChange> changes;
            std::vector<OpenMarketRecord> listings;
            bool ok = false;
        };
        auto got = std::make_shared<Loaded>();
        const std::int64_t account = actor->account;
        ++writes;
        host.OpenMarketSubmit(
            [account, got](Database& db) {
                got->changes = AgeOpenMarket(db, account, om::kKeepSeconds);
                got->listings = LoadOpenMarket(db, account, got->ok);
            },
            [this, peer, got, after = std::move(after)] {
                --writes;
                TellChanges(peer, got->changes);
                if (const auto s = sessions.find(peer); s != sessions.end()) s->second.listings = got->listings;
                proto::OpenMarketListings out;
                for (const OpenMarketRecord& r : got->listings) out.listings.push_back(ListingOf(r));
                if (!out.listings.empty() || sessions.contains(peer)) Send(peer, out);
                if (after) after();
            });
    }
    static proto::OpenMarketListing ListingOf(const OpenMarketRecord& r) {
        proto::OpenMarketListing l;
        l.listing = r.listing;
        l.status = r.status;
        if (r.held) l.item = RecordOf(r.item);
        else {
            l.item.item = r.refId;
            l.item.quantity = std::uint16_t(std::min<std::uint32_t>(r.count, 0xFFFF));
        }
        l.price = r.price;
        l.deposit = r.deposit;
        l.saleFee = r.saleFee;
        l.secondsLeft = std::uint32_t(std::clamp<std::int64_t>(r.secondsLeft, 0, 0xFFFFFFFF));
        return l;
    }
    void TellChanges(std::uint64_t peer, const std::vector<OpenMarketChange>& changes) {
        for (const OpenMarketChange& c : changes) {
            const auto kind = c.status == std::uint8_t(om::Status::Expired) ? proto::OpenMarketNewsKind::Expired : proto::OpenMarketNewsKind::Deleted;
            Send(peer, proto::OpenMarketNews{kind, c.refId});
        }
    }

    // --- The requests ----------------------------------------------------------------------------------------------

    void Open(std::uint64_t peer, const proto::OpenMarketOpen& m) {
        const auto actor = host.OpenMarketActorOf(peer);
        if (!actor) return;
        if (actor->dead) return Answer(peer, m.kId, code::kRestricted);
        if (!host.OpenMarketNpcNear(actor->entity, m.npc)) {
            core::LogInfo("open market: {} asked at {}, not Juel within reach", actor->name, m.npc);
            return Answer(peer, m.kId, code::kWrongItem);
        }
        Session& s = sessions[peer];
        s.npc = m.npc;
        s.searched.clear();
        Answer(peer, m.kId, code::kOk);
        Send(peer, StateFor(peer, true));
        Refresh(peer);
    }

    void Register(std::uint64_t peer, const OpenMarketActor& a, const proto::OpenMarketRegister& m) {
        Session& s = sessions.at(peer);
        const auto refuse = [&](std::uint16_t c) { Answer(peer, m.kId, c); };
        if (m.entries.empty() || m.entries.size() > std::size_t(om::kPageRows)) return refuse(code::kFailed);
        // The room: the account's listings not gone, against 10 and the ticket's.
        std::uint32_t ticketItem = 0;
        const om::Ticket ticket = host.OpenMarketTicket(peer, ticketItem);
        const int room = om::kListings + std::max(0, ticket.room);
        if (int(s.listings.size() + m.entries.size()) > room) return refuse(code::kRoom);
        std::vector<OpenMarketPut> puts;
        std::set<std::uint8_t> places;
        std::uint64_t deposits = 0;
        const std::uint8_t size = host.OpenMarketBagSize(peer);
        // Each entry as checks it.
        for (const proto::OpenMarketEntry& e : m.entries) {
            if (e.place >= size) return refuse(code::kSlot);
            if (e.place < om::kFirstBagSlot) return refuse(code::kEquipped);
            if (!places.insert(e.place).second) return refuse(code::kWrongItem);
            const game::ItemInstance* item = host.OpenMarketBagItem(peer, e.place);
            if (item == nullptr || item->serial == 0) return refuse(code::kWrongItem);
            const assets::ItemRow* row = host.OpenMarketItem(item->refId);
            if (row == nullptr) return refuse(code::kWrongItem);
            if (row->cashItem) return refuse(code::kCannotSell);
            if (host.OpenMarketPetOut(peer, e.place)) return refuse(code::kPet);
            const bool tradeGoods = row->typeId[0] == 3 && row->typeId[1] == 3 && row->typeId[2] == 8;
            if (!row->canTrade || tradeGoods) return refuse(code::kNotTradeable);
            if (item->rentUntil != 0 || item->bound) return refuse(code::kBound);
            if (e.price < om::kPriceMin || e.price > om::kPriceMax) return refuse(code::kPrice);
            // Equipment one at a time, a stack whole (0x7018).
            if (row->maxStack <= 1 ? e.count != 1 : e.count != item->count) return refuse(code::kCount);
            const std::uint32_t group = host.OpenMarketGroupOf(*row);
            if (group == 0) return refuse(code::kCannotSell); // in no category: no search could find it (ours)
            OpenMarketPut put;
            put.account = a.account;
            put.character = a.character;
            put.seller = a.name;
            put.place = e.place;
            put.item = item->serial;
            put.refId = item->refId;
            put.group = group;
            put.degree = std::uint8_t(row->itemClass > 0 ? (row->itemClass - 1) / 3 + 1 : 0);
            put.count = e.count;
            put.price = e.price;
            put.deposit = om::ListingFee(config.fees, e.price);
            put.saleFee = om::SaleFee(config.fees, e.price, ticket.salePercent);
            put.seconds = om::kListingSeconds + std::max<std::int64_t>(0, ticket.seconds);
            put.ticket = ticketItem;
            deposits += put.deposit;
            puts.push_back(std::move(put));
        }
        const std::uint64_t gold = host.OpenMarketGold(peer);
        if (deposits > gold) return refuse(code::kDeposit);
        std::uint64_t left = gold;
        for (OpenMarketPut& p : puts) {
            left -= p.deposit;
            p.gold = left;
        }
        // The items out of the bag and the deposits out of the gold first (the original's: nothing of them is left to
        // move, sell or trade while the database works); back if their rows do not move.
        std::vector<game::ItemInstance> aside;
        for (const OpenMarketPut& p : puts) {
            std::optional<game::ItemInstance> item = host.OpenMarketTakeAside(peer, p.place);
            aside.push_back(item ? std::move(*item) : game::ItemInstance{});
        }
        host.OpenMarketSetGold(peer, left, false);
        busy.insert(a.entity);
        ++writes;
        auto made = std::make_shared<std::vector<std::optional<std::uint32_t>>>();
        host.OpenMarketWrite(
            a.character,
            [puts, made](Database& db) {
                for (const OpenMarketPut& p : puts) {
                    made->push_back(PutOpenMarket(db, p));
                    if (!made->back()) break; // the rest not tried: the bag stays as it is
                }
            },
            [this, peer, entity = a.entity, puts, made, aside = std::move(aside), request = m.kId] {
                --writes;
                busy.erase(entity);
                std::uint64_t unpaid = 0;
                bool all = true;
                for (std::size_t k = 0; k < puts.size(); ++k) {
                    if (k >= made->size() || !(*made)[k]) {
                        all = false;
                        host.OpenMarketPutBack(peer, puts[k].place, aside[k]);
                        unpaid += puts[k].deposit;
                        continue;
                    }
                    core::LogInfo("open market: {} lists item {} x{} for {} (deposit {})", puts[k].seller, puts[k].refId, puts[k].count, puts[k].price, puts[k].deposit);
                }
                if (unpaid > 0) host.OpenMarketSetGold(peer, host.OpenMarketGold(peer) + unpaid);
                Answer(peer, request, all ? code::kOk : code::kFailed);
                Refresh(peer);
            });
    }

    void TakeBack(std::uint64_t peer, const OpenMarketActor& a, const proto::OpenMarketTakeBack& m) {
        if (m.listings.empty()) return Answer(peer, m.kId, code::kNotListed);
        std::set<std::uint8_t> taken;
        std::vector<std::pair<std::uint32_t, std::uint8_t>> moves;
        for (const std::uint32_t listing : m.listings) {
            const auto place = host.OpenMarketFreePlace(peer, taken);
            if (!place) return Answer(peer, m.kId, code::kTakeBack); // "no items to cancel, or no room in the inventory"
            taken.insert(*place);
            moves.push_back({listing, *place});
        }
        busy.insert(a.entity);
        ++writes;
        struct Out {
            std::vector<std::pair<std::uint8_t, OpenMarketRecord>> back;
            OpenMarketTake last = OpenMarketTake::Ok;
        };
        auto out = std::make_shared<Out>();
        host.OpenMarketWrite(
            a.character,
            [account = a.account, character = a.character, moves, out](Database& db) {
                for (const auto& [listing, place] : moves) {
                    OpenMarketRecord r;
                    out->last = TakeBackOpenMarket(db, account, listing, character, place, r);
                    if (out->last != OpenMarketTake::Ok) break;
                    out->back.push_back({place, r});
                }
            },
            [this, peer, entity = a.entity, out, request = m.kId] {
                --writes;
                busy.erase(entity);
                for (const auto& [place, r] : out->back) host.OpenMarketPutInBag(peer, place, r.item);
                Answer(peer, request, out->last == OpenMarketTake::Ok ? code::kOk : code::kTakeBack);
                Refresh(peer);
            });
    }

    void Buy(std::uint64_t peer, const OpenMarketActor& a, const proto::OpenMarketBuy& m) {
        Session& s = sessions.at(peer);
        const auto row = s.searched.find(std::uint64_t(m.account) << 32 | m.listing);
        if (row == s.searched.end() || std::int64_t(m.account) == a.account) return Answer(peer, m.kId, code::kNotOnSale);
        const std::uint64_t price = row->second;
        const std::uint64_t gold = host.OpenMarketGold(peer);
        if (gold < price) return Answer(peer, m.kId, code::kGold);
        const auto place = host.OpenMarketFreePlace(peer, {});
        if (!place) return Answer(peer, m.kId, code::kBagFull);
        // The price out of the gold first (the purchase's transaction writes the gold left), back if it does not stand.
        host.OpenMarketSetGold(peer, gold - price, false);
        busy.insert(a.entity);
        ++writes;
        struct Out {
            OpenMarketTake result = OpenMarketTake::Failed;
            OpenMarketRecord record;
            std::int64_t seller = 0;
        };
        auto out = std::make_shared<Out>();
        host.OpenMarketWrite(
            a.character,
            [account = std::int64_t(m.account), listing = m.listing, price, buyer = a.character, place = *place, left = gold - price, out](Database& db) {
                out->result = BuyOpenMarket(db, account, listing, price, buyer, place, left, om::kKeepSeconds, out->record, out->seller);
            },
            [this, peer, entity = a.entity, name = a.name, place = *place, price, out, request = m.kId, key = row->first] {
                --writes;
                busy.erase(entity);
                if (out->result != OpenMarketTake::Ok) {
                    host.OpenMarketSetGold(peer, host.OpenMarketGold(peer) + price);
                    if (const auto s = sessions.find(peer); s != sessions.end()) s->second.searched.erase(key);
                    const std::uint16_t c = out->result == OpenMarketTake::Expired    ? code::kExpired
                                            : out->result == OpenMarketTake::BagTaken ? code::kBagFull
                                                                                      : code::kNotOnSale;
                    return Answer(peer, request, c);
                }
                host.OpenMarketPutInBag(peer, place, out->record.item);
                if (const auto s = sessions.find(peer); s != sessions.end()) s->second.searched.erase(key);
                Answer(peer, request, code::kOk);
                Send(peer, proto::OpenMarketNews{proto::OpenMarketNewsKind::Bought, out->record.refId});
                core::LogInfo("open market: {} buys {}'s item {} for {}", name, out->record.seller, out->record.refId, price);
                Sold(out->record.account, out->record.refId);
            });
    }
    // A sale told to its seller wherever it plays (the original's 0x350D relay, its 0x3510).
    void Sold(std::int64_t account, std::uint32_t item) {
        if (const std::uint64_t seller = host.OpenMarketPeerOfAccount(account); seller != 0) {
            Send(seller, proto::OpenMarketNews{proto::OpenMarketNewsKind::Sold, item});
            if (sessions.contains(seller)) Refresh(seller);
            return;
        }
        if (!host.OpenMarketOnBus()) return;
        Envelope e;
        e.from = host.OpenMarketServer();
        e.kind = 1;
        e.account = account;
        e.item = item;
        host.OpenMarketPublish(BusToHex(proto::Encode(e)));
    }

    void Settle(std::uint64_t peer, const OpenMarketActor& a, const proto::OpenMarketSettle& m) {
        Session& s = sessions.at(peer);
        if (m.listings.empty()) return Answer(peer, m.kId, code::kNotSold);
        std::uint64_t payout = 0;
        std::set<std::uint32_t> seen;
        for (const std::uint32_t listing : m.listings) {
            const auto it = std::find_if(s.listings.begin(), s.listings.end(), [&](const OpenMarketRecord& r) { return r.listing == listing; });
            if (it == s.listings.end() || it->status != std::uint8_t(om::Status::Sold) || !seen.insert(listing).second) return Answer(peer, m.kId, code::kNotSold);
            payout += om::Payout(it->price, it->deposit, it->saleFee);
        }
        const std::uint64_t gold = host.OpenMarketGold(peer);
        // The payout into the gold first (the original takes it into memory before its job; the settling's transaction
        // writes the gold), taken back if the rows are not settled.
        host.OpenMarketSetGold(peer, gold + payout, false);
        busy.insert(a.entity);
        ++writes;
        auto ok = std::make_shared<bool>(false);
        host.OpenMarketWrite(
            a.character,
            [account = a.account, listings = m.listings, payout, character = a.character, after = gold + payout, ok](Database& db) {
                *ok = SettleOpenMarket(db, account, listings, payout, character, after);
            },
            [this, peer, entity = a.entity, payout, ok, request = m.kId] {
                --writes;
                busy.erase(entity);
                if (!*ok) {
                    const std::uint64_t now = host.OpenMarketGold(peer);
                    host.OpenMarketSetGold(peer, now > payout ? now - payout : 0);
                }
                Answer(peer, request, *ok ? code::kOk : code::kNotSold);
                Refresh(peer);
            });
    }

    void Search(std::uint64_t peer, const proto::OpenMarketSearch& m) {
        if (m.group == 0) return Answer(peer, m.kId, code::kSearch);
        if (!m.seller.empty() && m.seller.size() < 2) return Answer(peer, m.kId, code::kShortText);
        struct Out {
            std::vector<OpenMarketRecord> rows;
            std::uint32_t total = 0;
        };
        auto out = std::make_shared<Out>();
        ++writes;
        host.OpenMarketSubmit(
            [m, out](Database& db) { out->rows = SearchOpenMarket(db, m.group, m.degree, m.seller, m.page, om::kPageRows, om::kSearchMost, out->total); },
            [this, peer, out, page = m.page] {
                --writes;
                const auto s = sessions.find(peer);
                if (s == sessions.end()) return;
                s->second.searched.clear();
                proto::OpenMarketResults r;
                r.page = page;
                r.total = out->total;
                r.pages = std::uint16_t((out->total + om::kPageRows - 1) / om::kPageRows);
                for (const OpenMarketRecord& o : out->rows) {
                    s->second.searched[std::uint64_t(o.account) << 32 | o.listing] = o.price;
                    proto::OpenMarketRow row;
                    row.account = std::uint32_t(o.account);
                    row.listing = o.listing;
                    row.seller = o.seller;
                    row.item = RecordOf(o.item);
                    row.price = o.price;
                    row.secondsLeft = std::uint32_t(std::clamp<std::int64_t>(o.secondsLeft, 0, 0xFFFFFFFF));
                    r.rows.push_back(std::move(row));
                }
                Send(peer, r);
            });
    }
};

OpenMarket::OpenMarket(OpenMarketHost& host, OpenMarketConfig config) : s_(std::make_unique<State>(host, std::move(config))) {}
OpenMarket::~OpenMarket() = default;

bool OpenMarket::Handle(std::uint64_t peer, proto::MessageId id, std::span<const std::byte> payload) {
    State& s = *s_;
    const auto actor = s.host.OpenMarketActorOf(peer);
    if (!actor) return true;
    if (id == proto::OpenMarketOpen::kId) {
        proto::OpenMarketOpen m;
        if (!proto::Decode(payload, m)) return false;
        s.Open(peer, m);
        return true;
    }
    if (id == proto::OpenMarketClose::kId) {
        s.sessions.erase(peer);
        s.Answer(peer, id, code::kOk);
        return true;
    }
    // Every other request needs the market open (the state 9; else 0x7009) and nothing else holding the bag.
    if (!s.sessions.contains(peer)) return s.Answer(peer, id, code::kNotOpen), true;
    const bool bag = id == proto::OpenMarketRegister::kId || id == proto::OpenMarketTakeBack::kId || id == proto::OpenMarketBuy::kId ||
                     id == proto::OpenMarketSettle::kId;
    if (bag && (s.busy.contains(actor->entity) || s.host.OpenMarketBagBusy(actor->entity))) return s.Answer(peer, id, code::kFailed), true;
    switch (id) {
    case proto::OpenMarketRegister::kId: {
        proto::OpenMarketRegister m;
        if (!proto::Decode(payload, m)) return false;
        s.Register(peer, *actor, m);
        return true;
    }
    case proto::OpenMarketTakeBack::kId: {
        proto::OpenMarketTakeBack m;
        if (!proto::Decode(payload, m)) return false;
        s.TakeBack(peer, *actor, m);
        return true;
    }
    case proto::OpenMarketBuy::kId: {
        proto::OpenMarketBuy m;
        if (!proto::Decode(payload, m)) return false;
        s.Buy(peer, *actor, m);
        return true;
    }
    case proto::OpenMarketSettle::kId: {
        proto::OpenMarketSettle m;
        if (!proto::Decode(payload, m)) return false;
        s.Settle(peer, *actor, m);
        return true;
    }
    case proto::OpenMarketSearch::kId: {
        proto::OpenMarketSearch m;
        if (!proto::Decode(payload, m)) return false;
        s.Search(peer, m);
        return true;
    }
    case proto::OpenMarketMine::kId: s.Refresh(peer); return true;
    default: return false;
    }
}

void OpenMarket::BusMessage(const std::string& text) {
    State& s = *s_;
    const auto bytes = BusFromHex(text);
    Envelope e;
    if (!bytes || !proto::Decode(*bytes, e) || e.from == s.host.OpenMarketServer()) return;
    if (e.kind != 1) return;
    if (const std::uint64_t seller = s.host.OpenMarketPeerOfAccount(e.account); seller != 0) {
        s.Send(seller, proto::OpenMarketNews{proto::OpenMarketNewsKind::Sold, e.item});
        if (s.sessions.contains(seller)) s.Refresh(seller);
    }
}

void OpenMarket::Tick(Clock::time_point now) {
    State& s = *s_;
    // Each minute for an owner online: its listings aged, their lines told.
    for (auto& [peer, o] : s.online) {
        if (now < o.nextAge) continue;
        o.nextAge = now + std::chrono::minutes(1);
        const std::uint64_t p = peer;
        auto changes = std::make_shared<std::vector<OpenMarketChange>>();
        ++s.writes;
        s.host.OpenMarketSubmit([account = o.account, changes](Database& db) { *changes = AgeOpenMarket(db, account, om::kKeepSeconds); },
                                [this, p, changes] {
                                    --s_->writes;
                                    if (changes->empty()) return;
                                    s_->TellChanges(p, *changes);
                                    if (s_->sessions.contains(p)) s_->Refresh(p);
                                });
    }
}

void OpenMarket::Entered(std::uint64_t peer) {
    State& s = *s_;
    const auto actor = s.host.OpenMarketActorOf(peer);
    if (!actor) return;
    s.online[peer] = {actor->account, actor->character, Clock::now() + std::chrono::minutes(1)};
    // Its listings at the entry (aged, their lines told): the client's alarm window shows them.
    s.Refresh(peer);
}

void OpenMarket::Left(std::uint64_t peer) {
    s_->online.erase(peer);
    s_->sessions.erase(peer);
}

bool OpenMarket::Busy(std::uint32_t entity) const { return s_->busy.contains(entity); }
bool OpenMarket::InSession(std::uint64_t peer) const { return s_->sessions.contains(peer); }
bool OpenMarket::Writing() const { return s_->writes > 0; }

} // namespace sr::server
