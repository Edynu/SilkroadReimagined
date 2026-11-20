#include "sr/server/MagicPop.h"

#include <algorithm>
#include <charconv>
#include <format>

#include "sr/core/Log.h"
#include "sr/game/Items.h"

namespace sr::server {

namespace proto = game::proto;

namespace {
// The play's refusals.
constexpr std::uint16_t kNoNpc = 0, kTooFar = 3, kNotMachine = 0x1C0F, kNotBag = 0x1806, kNoItem = 0x1809, kRentOver = 0x183D, kNoEntry = 2,
                        kFailed = 5;
} // namespace

MagicPop::MagicPop(PopHost& host, std::shared_ptr<const assets::GachaTable> table) : host_(host), table_(std::move(table)) {}

bool MagicPop::IsRequest(proto::MessageId id) { return id == proto::PopPlay::kId || id == proto::PopExchange::kId; }

bool MagicPop::Handle(std::uint32_t entity, proto::MessageId id, std::span<const std::byte> payload) {
    if (id == proto::PopPlay::kId) {
        proto::PopPlay m;
        if (!proto::Decode(payload, m)) return false;
        Play(entity, m);
        return true;
    }
    if (id == proto::PopExchange::kId) {
        proto::PopExchange m;
        if (!proto::Decode(payload, m)) return false;
        Exchange(entity, m);
        return true;
    }
    return false;
}

std::string MagicPop::EncodeAnnounce(const proto::PopAnnounce& a) { return std::format("{}\t{}\t{}", a.item, a.count, a.name); }

std::optional<proto::PopAnnounce> MagicPop::DecodeAnnounce(std::string_view payload) {
    const std::size_t a = payload.find('\t'), b = a == std::string_view::npos ? a : payload.find('\t', a + 1);
    if (b == std::string_view::npos) return std::nullopt;
    proto::PopAnnounce out;
    std::uint32_t count = 0;
    if (std::from_chars(payload.data(), payload.data() + a, out.item).ec != std::errc{} ||
        std::from_chars(payload.data() + a + 1, payload.data() + b, count).ec != std::errc{}) {
        return std::nullopt;
    }
    out.count = std::uint16_t(std::min<std::uint32_t>(count, 0xFFFF));
    out.name = std::string(payload.substr(b + 1, 32));
    return out;
}

void MagicPop::Played(std::uint64_t peer, std::uint16_t code) {
    proto::PopPlayed answer;
    answer.code = code;
    host_.PopSend(peer, proto::PopPlayed::kId, proto::Encode(answer));
}

void MagicPop::Play(std::uint32_t entity, const proto::PopPlay& r) {
    const auto player = host_.PopPlayer(entity);
    if (!player || player->bag == nullptr || waiting_.contains(entity)) return;
    const auto refuse = [&](std::uint16_t code) { Played(player->peer, code); };
    // The original's order.
    const auto npc = host_.PopNpc(r.npc);
    if (!npc) return refuse(kNoNpc);
    if (!host_.PopInReach(entity, r.npc)) return refuse(kTooFar);
    const assets::GachaMachine* machine = table_->Machine(npc->refObjId);
    if (machine == nullptr) return refuse(kNotMachine); // no function 0x11
    game::Inventory& bag = *player->bag;
    if (r.place < game::slot::kWorn || r.place >= bag.size) return refuse(kNotBag);
    auto it = bag.places.find(r.place);
    if (it == bag.places.end()) return refuse(kNoItem);
    const assets::ItemRow* card = host_.PopItem(it->second.refId);
    if (card == nullptr || !game::pop::IsCard(*card)) return refuse(kNotBag);
    if (it->second.rentUntil != 0 && it->second.rentUntil < host_.PopNow()) return refuse(kRentOver);
    const assets::GachaEntry* chosen = table_->Find(r.entry);
    if (chosen == nullptr || chosen->set != machine->selection) return refuse(kNoEntry);
    const game::pop::WasteSet waste = game::pop::MakeWasteSet(*table_, machine->waste);
    const game::pop::Roll roll = game::pop::RollPrize(*chosen, waste, host_.PopDraw(), host_.PopDraw());
    if (roll.prize == nullptr) return refuse(kNoEntry);
    // The card's Desc2 names the coupon a win makes, Desc1 a loss's (both 3/3/14/2).
    const assets::ItemRow* coupon = host_.PopItem(std::string_view(card->paramTexts[roll.won ? 1 : 0]));
    if (coupon == nullptr || !game::pop::IsCoupon(*coupon)) return refuse(kFailed);
    // In place: the same item, the same place; its reference and params changed.
    const game::ItemInstance before = it->second;
    it->second.refId = coupon->refObjId;
    it->second.count = 1;
    it->second.durability = 0;
    it->second.rentUntil = 0;
    game::pop::SetParams(it->second, game::pop::CouponParams(*roll.prize));
    waiting_.insert(entity);
    const std::uint8_t place = r.place;
    const bool won = game::pop::CouponWon(*coupon);
    const proto::PopAnnounce told{roll.prize->item, std::uint16_t(std::clamp(roll.prize->count, 1, 0xFFFF)), player->name};
    const bool visible = roll.won && roll.prize->visible;
    core::LogInfo("magic pop: {} plays for entry {} (item {}): {}, prize {} x{}", player->name, chosen->id, chosen->item, roll.won ? "won" : "lost",
                  roll.prize->item, roll.prize->count);
    host_.PopSave(entity, {place}, [this, entity, character = player->character, place, before, won, told, visible](bool ok) {
        waiting_.erase(entity);
        const auto again = host_.PopPlayer(entity);
        if (!again || again->character != character || again->bag == nullptr) return;
        auto at = again->bag->places.find(place);
        if (!ok) {
            // The database refused: the card as it was (ours: the original has no answer here).
            if (at != again->bag->places.end() && at->second.serial == before.serial) at->second = before;
            Played(again->peer, kFailed);
            return;
        }
        proto::PopPlayed answer;
        answer.won = won ? 1 : 0;
        answer.prize = told.item;
        answer.count = told.count;
        if (at != again->bag->places.end()) answer.item.push_back(again->bag->Record(place));
        host_.PopSend(again->peer, proto::PopPlayed::kId, proto::Encode(answer));
        host_.PopBagChanged(entity);
        // Only an entry the data marks visible (none in 1.188's: the original never announces with it).
        if (visible) host_.PopAnnounce(told);
    });
}

void MagicPop::Exchange(std::uint32_t entity, const proto::PopExchange& r) {
    const auto player = host_.PopPlayer(entity);
    if (!player || player->bag == nullptr || waiting_.contains(entity)) return;
    const auto refuse = [&](std::uint16_t code) {
        proto::PopExchanged answer;
        answer.code = code;
        host_.PopSend(player->peer, proto::PopExchanged::kId, proto::Encode(answer));
    };
    const auto npc = host_.PopNpc(r.npc);
    if (!npc) return refuse(kNoNpc);
    if (!host_.PopInReach(entity, r.npc)) return refuse(kTooFar);
    if (!IsGori(npc->codename)) return refuse(kNotMachine); // no function 0x12
    // Every coupon from place 13 on, in place; a bag without one: done, nothing changed.
    game::Inventory& bag = *player->bag;
    std::vector<std::pair<std::uint8_t, game::ItemInstance>> before;
    const std::int64_t now = host_.PopNow();
    for (auto& [place, item] : bag.places) {
        if (place < game::slot::kWorn) continue;
        const assets::ItemRow* row = host_.PopItem(item.refId);
        if (row == nullptr || !game::pop::IsCoupon(*row) || (item.rentUntil != 0 && item.rentUntil < now)) continue;
        const auto params = game::pop::ParamsOf(item);
        const auto prize = game::pop::ReadCoupon(params);
        const assets::ItemRow* prizeRow = prize ? host_.PopItem(prize->item) : nullptr;
        if (prizeRow == nullptr) continue;
        before.emplace_back(place, item);
        game::ItemInstance made = game::pop::PrizeItem(*prize, *prizeRow, host_.PopFresh(*prizeRow, prize->count), host_.PopOptions());
        // An equipment prize the coupon leaves open: no 'O', its plus the least of two rolls 1..5; no 'M', the drops'
        // random blues (the spec's flag 2 unset: the random rule), the answer ignored.
        const game::ItemKind kind = game::KindOf(*prizeRow);
        if (kind == game::ItemKind::Weapon || kind == game::ItemKind::Shield || kind == game::ItemKind::Armour || kind == game::ItemKind::Accessory) {
            const auto named = [&](char k) { return std::any_of(prize->tokens.begin(), prize->tokens.end(), [&](const game::pop::Token& t) { return t.kind == k; }); };
            if (!named('O')) {
                const int a = int(host_.PopDraw() % 5) + 1, b = int(host_.PopDraw() % 5) + 1;
                made.plus = std::uint8_t(std::min(a, b));
            }
            if (!named('M')) host_.PopRandomBlues(*prizeRow, made);
        }
        made.serial = item.serial;
        item = std::move(made);
    }
    if (before.empty()) {
        host_.PopSend(player->peer, proto::PopExchanged::kId, proto::Encode(proto::PopExchanged{}));
        return;
    }
    std::vector<std::uint8_t> touched;
    for (const auto& [place, item] : before) touched.push_back(place);
    waiting_.insert(entity);
    core::LogInfo("magic pop: {} exchanges {} coupons", player->name, before.size());
    host_.PopSave(entity, touched, [this, entity, character = player->character, before, touched](bool ok) {
        waiting_.erase(entity);
        const auto again = host_.PopPlayer(entity);
        if (!again || again->character != character || again->bag == nullptr) return;
        proto::PopExchanged answer;
        if (!ok) {
            for (const auto& [place, item] : before) {
                auto at = again->bag->places.find(place);
                if (at != again->bag->places.end() && at->second.serial == item.serial) at->second = item;
            }
            answer.code = kFailed;
        } else {
            for (std::uint8_t place : touched) {
                if (again->bag->At(place) != nullptr) answer.items.push_back(again->bag->Record(place));
            }
        }
        host_.PopSend(again->peer, proto::PopExchanged::kId, proto::Encode(answer));
        if (ok) host_.PopBagChanged(entity);
    });
}

} // namespace sr::server
