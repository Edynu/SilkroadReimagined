#include "sr/server/Alchemy.h"

#include <algorithm>
#include <set>

#include "sr/assets/GameData.h"
#include "sr/core/Log.h"

namespace sr::server {

namespace proto = game::proto;
namespace alchemy = game::alchemy;
using proto::AlchemyOperation;
using proto::AlchemyOutcome;

namespace {

bool InBag(const game::Inventory& bag, std::uint8_t place) { return place >= game::slot::kWorn && place < bag.size; }

// The not-free code (the open market's and the trade's: 0x5419).
constexpr std::uint16_t kNotFree = 0x5419;

} // namespace

Alchemy::Alchemy(AlchemyHost& host, std::uint32_t seed) : host_(host), random_(seed) {}

bool Alchemy::Handle(std::uint32_t entity, proto::MessageId id, std::span<const std::byte> payload, Clock::time_point) {
    switch (id) {
    case proto::AlchemyStrengthen::kId: {
        proto::AlchemyStrengthen m;
        if (!proto::Decode(payload, m)) return false;
        if (!Busy(entity)) Strengthen(entity, m);
        return true;
    }
    case proto::AlchemyCancel::kId: {
        proto::AlchemyCancel m;
        if (!proto::Decode(payload, m)) return false;
        if (const auto it = runs_.find(entity); it != runs_.end()) {
            const AlchemyOperation operation = it->second.operation;
            runs_.erase(it);
            proto::AlchemyResult r;
            r.operation = operation;
            r.outcome = AlchemyOutcome::Cancelled;
            Answer(entity, r);
        }
        return true;
    }
    case proto::AlchemyManufacture::kId: {
        proto::AlchemyManufacture m;
        if (!proto::Decode(payload, m)) return false;
        if (!Busy(entity)) Manufacture(entity, m);
        return true;
    }
    case proto::AlchemyDisjoint::kId: {
        proto::AlchemyDisjoint m;
        if (!proto::Decode(payload, m)) return false;
        if (!Busy(entity)) Disjoint(entity, m);
        return true;
    }
    case proto::AlchemyDismantle::kId: {
        proto::AlchemyDismantle m;
        if (!proto::Decode(payload, m)) return false;
        if (!Busy(entity)) Dismantle(entity, m);
        return true;
    }
    case proto::SocketMake::kId: {
        proto::SocketMake m;
        if (!proto::Decode(payload, m)) return false;
        if (!Busy(entity)) SocketMake(entity, m);
        return true;
    }
    case proto::SocketInsert::kId: {
        proto::SocketInsert m;
        if (!proto::Decode(payload, m)) return false;
        if (!Busy(entity)) SocketInsert(entity, m);
        return true;
    }
    case proto::SocketErase::kId: {
        proto::SocketErase m;
        if (!proto::Decode(payload, m)) return false;
        if (!Busy(entity)) SocketErase(entity, m);
        return true;
    }
    case proto::AvatarGrant::kId: {
        proto::AvatarGrant m;
        if (!proto::Decode(payload, m)) return false;
        if (!Busy(entity)) AvatarGrant(entity, m);
        return true;
    }
    default: return false;
    }
}

void Alchemy::Tick(Clock::time_point now) {
    if (now < nextTick_) return;
    nextTick_ = now + std::chrono::seconds(1);
    for (auto it = runs_.begin(); it != runs_.end();) {
        const std::uint32_t entity = it->first;
        Run& run = it->second;
        bool over = false;
        if (run.operation == AlchemyOperation::Strengthen || run.operation == AlchemyOperation::SocketInsert) {
            // Phase 3 completes it (the 4th tick), else the phase grows (a socket stone's
            // insertion runs the same action state).
            if (run.phase == 3) {
                if (run.operation == AlchemyOperation::Strengthen) Complete(entity, run);
                else SocketInserted(entity, run);
                over = true;
            } else {
                ++run.phase;
            }
        } else if (run.operation == AlchemyOperation::Disjoint && ++run.phase >= 3) {
            run.phase = 0;
            over = !DisjointStep(entity, run);
        }
        it = over ? runs_.erase(it) : std::next(it);
    }
}

void Alchemy::End(std::uint32_t entity) {
    if (const auto it = runs_.find(entity); it != runs_.end()) {
        const AlchemyOperation operation = it->second.operation;
        runs_.erase(it);
        proto::AlchemyResult r;
        r.operation = operation;
        r.outcome = AlchemyOutcome::Cancelled;
        Answer(entity, r);
    }
}

void Alchemy::Answer(std::uint32_t entity, const proto::AlchemyResult& result) {
    core::LogInfo("alchemy: entity {}: operation {} outcome {} code 0x{:04x} place {} plus {} -> {} option {} value {}", entity, int(result.operation),
                  int(result.outcome), result.code, result.place, result.plusBefore, result.plusAfter, result.option, result.value);
    if (const auto p = host_.AlchemyPlayer(entity)) host_.AlchemySend(p->peer, proto::AlchemyResult::kId, proto::Encode(result));
}

void Alchemy::Refuse(std::uint32_t entity, AlchemyOperation operation, std::uint16_t code, std::uint8_t place) {
    proto::AlchemyResult r;
    r.operation = operation;
    r.outcome = AlchemyOutcome::Refused;
    r.code = code;
    r.place = place;
    Answer(entity, r);
}

int Alchemy::Held(const game::Inventory& bag, const assets::ItemRow* row) const {
    if (row == nullptr) return 0;
    int n = 0;
    for (const auto& [place, item] : bag.places) {
        if (place >= game::slot::kWorn && item.refId == row->refObjId) n += item.count;
    }
    return n;
}

void Alchemy::Take(game::Inventory& bag, const assets::ItemRow* row, int count, std::vector<std::uint8_t>& touched,
                   std::vector<std::int64_t>& gone) const {
    for (auto it = bag.places.begin(); it != bag.places.end() && count > 0;) {
        if (it->first < game::slot::kWorn || row == nullptr || it->second.refId != row->refObjId) {
            ++it;
            continue;
        }
        const int n = std::min(count, int(it->second.count));
        count -= n;
        touched.push_back(it->first);
        if (n == it->second.count) {
            gone.push_back(it->second.serial);
            it = bag.places.erase(it);
        } else {
            it->second.count = std::uint16_t(it->second.count - n);
            ++it;
        }
    }
}

void Alchemy::UseOne(game::Inventory& bag, std::uint8_t place, std::vector<std::uint8_t>& touched, std::vector<std::int64_t>& gone) {
    const auto it = bag.places.find(place);
    if (it == bag.places.end()) return;
    touched.push_back(place);
    if (it->second.count > 1) {
        --it->second.count;
        return;
    }
    gone.push_back(it->second.serial);
    bag.places.erase(it);
}

// --- Strengthening ----------------------------------------------------------------------------------------------------

void Alchemy::Strengthen(std::uint32_t entity, const proto::AlchemyStrengthen& m) {
    const auto p = host_.AlchemyPlayer(entity);
    const assets::ItemTable* items = host_.AlchemyItems();
    const auto* options = host_.AlchemyOptions();
    if (!p || items == nullptr || options == nullptr) return;
    constexpr auto op = AlchemyOperation::Strengthen;
    if (!p->alive || !p->free) return Refuse(entity, op, kNotFree, m.target);
    game::Inventory& bag = *p->inventory;
    if (!InBag(bag, m.target) || !InBag(bag, m.material) || (m.powder != 0 && !InBag(bag, m.powder))) return Refuse(entity, op, alchemy::code::kBadSlot, m.target);
    if (m.target == m.material || m.target == m.powder || (m.powder != 0 && m.powder == m.material)) return Refuse(entity, op, alchemy::code::kTwice, m.target);
    const game::ItemInstance* item = bag.At(m.target);
    const game::ItemInstance* material = bag.At(m.material);
    const game::ItemInstance* powder = m.powder != 0 ? bag.At(m.powder) : nullptr;
    if (item == nullptr) return Refuse(entity, op, alchemy::code::kTargetMissing, m.target);
    if (material == nullptr) return Refuse(entity, op, alchemy::code::kRecipeMissing, m.target);
    if (m.powder != 0 && powder == nullptr) return Refuse(entity, op, alchemy::code::kNothing, m.target);
    const assets::ItemRow* targetRow = items->Find(item->refId);
    const assets::ItemRow* materialRow = items->Find(material->refId);
    const assets::ItemRow* powderRow = powder != nullptr ? items->Find(powder->refId) : nullptr;
    if (targetRow == nullptr || materialRow == nullptr || (powder != nullptr && powderRow == nullptr)) return Refuse(entity, op, alchemy::code::kNothing, m.target);
    std::uint16_t code = 0;
    switch (alchemy::KindOf(*materialRow)) {
    case alchemy::Kind::Elixir: code = alchemy::CheckReinforce(*targetRow, *item, *materialRow, powderRow); break;
    case alchemy::Kind::MagicStone: code = powder != nullptr ? alchemy::code::kNotMaterial : alchemy::CheckMagicStone(*targetRow, *item, *materialRow, *options); break;
    case alchemy::Kind::AttrStone: code = powder != nullptr ? alchemy::code::kNotMaterial : alchemy::CheckAttrStone(*targetRow, *materialRow); break;
    case alchemy::Kind::AdvancedElixir: code = powder != nullptr ? alchemy::code::kNotMaterial : alchemy::CheckAdvanced(*targetRow, *item, *materialRow); break;
    default: code = alchemy::code::kNotMaterial;
    }
    if (code != 0) return Refuse(entity, op, code, m.target);
    Run run;
    run.operation = op;
    run.target = m.target;
    run.material = m.material;
    run.powder = m.powder;
    run.targetSerial = item->serial;
    run.materialSerial = material->serial;
    run.powderSerial = powder != nullptr ? powder->serial : 0;
    runs_[entity] = std::move(run);
}

void Alchemy::Complete(std::uint32_t entity, Run& run) {
    const auto p = host_.AlchemyPlayer(entity);
    const assets::ItemTable* items = host_.AlchemyItems();
    const auto* options = host_.AlchemyOptions();
    if (!p || items == nullptr || options == nullptr) return;
    constexpr auto op = AlchemyOperation::Strengthen;
    game::Inventory& bag = *p->inventory;
    // Checked again: the same items where they were.
    game::ItemInstance* item = bag.places.contains(run.target) ? &bag.places[run.target] : nullptr;
    const game::ItemInstance* material = bag.At(run.material);
    const game::ItemInstance* powder = run.powder != 0 ? bag.At(run.powder) : nullptr;
    if (item == nullptr || item->serial != run.targetSerial || material == nullptr || material->serial != run.materialSerial ||
        (run.powder != 0 && (powder == nullptr || powder->serial != run.powderSerial))) {
        return Refuse(entity, op, alchemy::code::kNothing, run.target);
    }
    const assets::ItemRow* targetRow = items->Find(item->refId);
    const assets::ItemRow* materialRow = items->Find(material->refId);
    const assets::ItemRow* powderRow = powder != nullptr ? items->Find(powder->refId) : nullptr;
    if (targetRow == nullptr || materialRow == nullptr) return Refuse(entity, op, alchemy::code::kNothing, run.target);

    proto::AlchemyResult r;
    r.operation = op;
    r.place = run.target;
    std::vector<std::uint8_t> touched{run.target};
    std::vector<std::int64_t> gone;
    const game::ItemInstance stone = *material;
    // The plus the window shows: the total (an advanced elixir's in it; 1.188 merges them).
    r.plusBefore = std::uint8_t(std::min(255, game::TotalPlus(*item)));
    if (alchemy::KindOf(*materialRow) == alchemy::Kind::AdvancedElixir) {
        // No roll: the binding is made; only the recipe is used.
        if (const std::uint16_t code = alchemy::CheckAdvanced(*targetRow, *item, *materialRow); code != 0) return Refuse(entity, op, code, run.target);
        alchemy::ApplyAdvanced(*item, *materialRow);
        r.outcome = AlchemyOutcome::Success;
        r.plusAfter = std::uint8_t(std::min(255, game::TotalPlus(*item)));
        UseOne(bag, run.material, touched, gone);
    } else if (alchemy::KindOf(*materialRow) == alchemy::Kind::Elixir) {
        const alchemy::ReinforceResult result = alchemy::Reinforce(*targetRow, *item, *materialRow, powderRow, *options, p->luck, random_);
        r.outcome = result.success ? AlchemyOutcome::Success : result.destroyed ? AlchemyOutcome::Destroyed : AlchemyOutcome::Failure;
        r.durabilityLost = result.durabilityLost;
        r.astralKept = result.astralKept;
        if (result.durabilityLost) {
            // The most durability fell: what is left is at most it.
            const game::ItemStats stats = game::ComputeStats(*targetRow, *item, options, 0, true);
            if (stats.durability.high > 0) item->durability = std::min<std::uint32_t>(item->durability, std::uint32_t(stats.durability.high));
        }
        r.plusAfter = std::uint8_t(std::min(255, game::TotalPlus(*item)));
        UseOne(bag, run.material, touched, gone);
        if (run.powder != 0) UseOne(bag, run.powder, touched, gone);
        if (result.destroyed) {
            gone.push_back(item->serial);
            bag.places.erase(run.target);
        }
    } else {
        const bool attribute = alchemy::KindOf(*materialRow) == alchemy::Kind::AttrStone;
        const alchemy::StoneResult result = attribute ? alchemy::ApplyAttrStone(*targetRow, *item, *materialRow, stone, *options, p->alchemy, random_)
                                                      : alchemy::ApplyMagicStone(*targetRow, *item, *materialRow, stone, *options, p->alchemy, random_);
        r.attribute = attribute;
        r.outcome = !result.success ? AlchemyOutcome::Failure : result.code != 0 ? AlchemyOutcome::Refused : AlchemyOutcome::Success;
        r.code = !result.success ? alchemy::code::kFailed : result.code;
        if (attribute) {
            const auto fields = alchemy::WhiteFields(*targetRow);
            std::string_view field = result.option;
            if (field.starts_with("NATTR_")) field.remove_prefix(6);
            r.option = std::uint32_t(std::find(fields.begin(), fields.end(), field) - fields.begin());
        } else if (const assets::MagicOptionRow* row = alchemy::OptionRow(*options, result.option, materialRow->params[0])) {
            r.option = row->paramId;
        }
        r.value = result.value;
        r.assimilatedField = std::int8_t(result.whiteField);
        // A stone that lowered the most durability (its DUR field re-rolled, an assimilation): what is left at most it, as
        // the original's recalculation lowers it after any change.
        if (result.success) {
            const game::ItemStats stats = game::ComputeStats(*targetRow, *item, options, 0, true);
            if (stats.durability.high > 0) item->durability = std::min<std::uint32_t>(item->durability, std::uint32_t(stats.durability.high));
        }
        r.assimilatedOption = std::int8_t(result.optionIndex);
        r.apeUsed = result.apeUsed;
        r.plusAfter = std::uint8_t(std::min(255, game::TotalPlus(*item)));
        // A stone refused after its roll (its charges full) is kept; else it is used.
        if (!result.success || result.code == 0) UseOne(bag, run.material, touched, gone);
    }
    host_.AlchemyBag(entity, std::move(touched), std::move(gone));
    Answer(entity, r);
}

// --- Making a stone ---------------------------------------------------------------------------------------------------

void Alchemy::Manufacture(std::uint32_t entity, const proto::AlchemyManufacture& m) {
    const auto p = host_.AlchemyPlayer(entity);
    const assets::ItemTable* items = host_.AlchemyItems();
    if (!p || items == nullptr) return;
    constexpr auto op = AlchemyOperation::Manufacture;
    if (!p->alive || !p->free) return Refuse(entity, op, kNotFree, m.tablet);
    game::Inventory& bag = *p->inventory;
    const game::ItemInstance* tablet = InBag(bag, m.tablet) ? bag.At(m.tablet) : nullptr;
    const assets::ItemRow* tabletRow = tablet != nullptr ? items->Find(tablet->refId) : nullptr;
    alchemy::Recipe recipe;
    if (tabletRow == nullptr || !alchemy::TabletRecipe(*items, *tabletRow, recipe)) return Refuse(entity, op, alchemy::code::kMakeFailed, m.tablet);
    // Each element the tablet lists from a placed stack holding enough of it.
    std::vector<std::pair<std::uint8_t, int>> uses;
    std::set<std::uint8_t> seen{m.tablet};
    for (const auto& [element, need] : recipe.elements) {
        bool found = false;
        for (const std::uint8_t place : m.elements) {
            const game::ItemInstance* e = InBag(bag, place) && !seen.contains(place) ? bag.At(place) : nullptr;
            if (e != nullptr && e->refId == element->refObjId && int(e->count) >= need) {
                uses.push_back({place, need});
                seen.insert(place);
                found = true;
                break;
            }
        }
        if (!found) return Refuse(entity, op, alchemy::code::kElementsShort, m.tablet);
    }
    // Room for the stone before anything is used (0x5808).
    if (bag.FreeBagPlace() < 0) return Refuse(entity, op, alchemy::code::kNoRoom, m.tablet);
    std::vector<std::uint8_t> touched;
    std::vector<std::int64_t> gone;
    game::ItemInstance stone;
    stone.refId = recipe.stone->refObjId;
    stone.count = 1;
    stone.plus = std::uint8_t(alchemy::StoneRate(*recipe.stone, false, random_));
    if (!host_.AlchemyGive(entity, {stone}, touched)) return Refuse(entity, op, alchemy::code::kNoRoom, m.tablet);
    UseOne(bag, m.tablet, touched, gone);
    for (const auto& [place, need] : uses) {
        auto& e = bag.places[place];
        touched.push_back(place);
        if (int(e.count) == need) {
            gone.push_back(e.serial);
            bag.places.erase(place);
        } else {
            e.count = std::uint16_t(e.count - need);
        }
    }
    host_.AlchemyBag(entity, std::move(touched), std::move(gone));
    proto::AlchemyResult r;
    r.operation = op;
    r.outcome = AlchemyOutcome::Success;
    r.place = m.tablet;
    r.option = recipe.stone->refObjId;
    r.value = stone.plus;
    Answer(entity, r);
}

// --- Disjoining ------------------------------------------------------------------------------------------------------

void Alchemy::Disjoint(std::uint32_t entity, const proto::AlchemyDisjoint& m) {
    const auto p = host_.AlchemyPlayer(entity);
    const assets::ItemTable* items = host_.AlchemyItems();
    if (!p || items == nullptr) return;
    constexpr auto op = AlchemyOperation::Disjoint;
    if (!p->alive || !p->free) return Refuse(entity, op, kNotFree);
    game::Inventory& bag = *p->inventory;
    if (m.materials.empty()) return Refuse(entity, op, alchemy::code::kNoTarget);
    std::set<std::uint8_t> seen;
    int rondos = 0;
    for (const std::uint8_t place : m.materials) {
        const game::ItemInstance* material = InBag(bag, place) ? bag.At(place) : nullptr;
        const assets::ItemRow* row = material != nullptr ? items->Find(material->refId) : nullptr;
        if (row == nullptr || alchemy::KindOf(*row) != alchemy::Kind::ByProduct || !seen.insert(place).second) return Refuse(entity, op, alchemy::code::kCannotTarget, place);
        rondos += alchemy::VoidRondos(*row, material->count);
    }
    if (Held(bag, items->Find(std::string_view("ITEM_ETC_ARCHEMY_RONDO_01"))) < rondos) return Refuse(entity, op, alchemy::code::kNotEnough);
    Run run;
    run.operation = op;
    run.queue = m.materials;
    if (DisjointStep(entity, run)) runs_[entity] = std::move(run);
}

bool Alchemy::DisjointStep(std::uint32_t entity, Run& run) {
    const auto p = host_.AlchemyPlayer(entity);
    const assets::ItemTable* items = host_.AlchemyItems();
    if (!p || items == nullptr || run.queue.empty()) return false;
    constexpr auto op = AlchemyOperation::Disjoint;
    game::Inventory& bag = *p->inventory;
    const std::uint8_t place = run.queue.front();
    run.queue.erase(run.queue.begin());
    const game::ItemInstance* material = bag.At(place);
    const assets::ItemRow* row = material != nullptr ? items->Find(material->refId) : nullptr;
    const assets::ItemRow* rondo = items->Find(std::string_view("ITEM_ETC_ARCHEMY_RONDO_01"));
    if (row == nullptr || alchemy::KindOf(*row) != alchemy::Kind::ByProduct) {
        Refuse(entity, op, alchemy::code::kCannotTarget, place);
        return false;
    }
    const int count = material->count;
    const int need = alchemy::VoidRondos(*row, count);
    if (Held(bag, rondo) < need) {
        Refuse(entity, op, alchemy::code::kNotEnough, place);
        return false;
    }
    // The whole stack into its elements; room for them first (the bag as it was if not).
    const game::Inventory before = bag;
    std::vector<std::uint8_t> touched{place};
    std::vector<std::int64_t> gone{material->serial};
    bag.places.erase(place);
    std::vector<game::ItemInstance> made;
    for (const auto& [element, n] : alchemy::DisjointYield(*items, *row, count)) {
        for (int left = n; left > 0;) {
            const int stack = std::min(left, std::max(1, game::MaxStack(*element)));
            game::ItemInstance e;
            e.refId = element->refObjId;
            e.count = std::uint16_t(stack);
            made.push_back(e);
            left -= stack;
        }
    }
    if (!host_.AlchemyGive(entity, made, touched)) {
        bag = before;
        Refuse(entity, op, alchemy::code::kNoRoom, place);
        return false;
    }
    Take(bag, rondo, need, touched, gone);
    host_.AlchemyBag(entity, std::move(touched), std::move(gone));
    proto::AlchemyResult r;
    r.operation = op;
    r.outcome = AlchemyOutcome::Success;
    r.place = place;
    r.value = std::uint32_t(count);
    r.remaining = std::uint8_t(run.queue.size());
    Answer(entity, r);
    return !run.queue.empty();
}

// --- Dismantling -----------------------------------------------------------------------------------------------------

void Alchemy::Dismantle(std::uint32_t entity, const proto::AlchemyDismantle& m) {
    const auto p = host_.AlchemyPlayer(entity);
    const assets::ItemTable* items = host_.AlchemyItems();
    const auto* options = host_.AlchemyOptions();
    if (!p || items == nullptr || options == nullptr) return;
    constexpr auto op = AlchemyOperation::Dismantle;
    if (!p->alive || !p->free) return Refuse(entity, op, kNotFree, m.target);
    game::Inventory& bag = *p->inventory;
    const game::ItemInstance* item = InBag(bag, m.target) ? bag.At(m.target) : nullptr;
    const assets::ItemRow* row = item != nullptr ? items->Find(item->refId) : nullptr;
    if (row == nullptr) return Refuse(entity, op, alchemy::code::kCannotTarget, m.target);
    if (const std::uint16_t code = alchemy::CheckDismantle(*row, *item, *options); code != 0) return Refuse(entity, op, code, m.target);
    const assets::ItemRow* rondo = items->Find(std::string_view("ITEM_ETC_ARCHEMY_RONDO_02"));
    const int need = alchemy::DestroyerRondos(*row);
    if (Held(bag, rondo) < need) return Refuse(entity, op, alchemy::code::kNotEnough, m.target);
    static const alchemy::StoneBucket kNone;
    const alchemy::StoneBucket* bucket = host_.AlchemyStones(alchemy::Degree(*row));
    const alchemy::DismantleYield yield = alchemy::Dismantle(*items, *row, *item, bucket != nullptr ? *bucket : kNone, random_);
    std::vector<game::ItemInstance> made;
    for (const auto& [element, n] : yield.elements) {
        for (int left = n; left > 0;) {
            const int stack = std::min(left, std::max(1, game::MaxStack(*element)));
            game::ItemInstance e;
            e.refId = element->refObjId;
            e.count = std::uint16_t(stack);
            made.push_back(e);
            left -= stack;
        }
    }
    for (const auto& [stone, rate] : yield.stones) {
        game::ItemInstance s;
        s.refId = stone->refObjId;
        s.plus = std::uint8_t(rate);
        made.push_back(s);
    }
    if (yield.recipe != nullptr) {
        game::ItemInstance recipe;
        recipe.refId = yield.recipe->refObjId;
        made.push_back(recipe);
    }
    // Room first (0x5808), then the Rondos and the piece used.
    const std::int64_t serial = item->serial;
    std::vector<std::uint8_t> touched{m.target};
    std::vector<std::int64_t> gone{serial};
    if (!host_.AlchemyGive(entity, made, touched)) return Refuse(entity, op, alchemy::code::kNoRoom, m.target);
    Take(bag, rondo, need, touched, gone);
    bag.places.erase(m.target);
    host_.AlchemyBag(entity, std::move(touched), std::move(gone));
    proto::AlchemyResult r;
    r.operation = op;
    r.outcome = AlchemyOutcome::Success;
    r.place = m.target;
    Answer(entity, r);
}

// --- Sockets -------------------------------------------------------------------------------

int Alchemy::FirstOf(const game::Inventory& bag, const assets::ItemTable& items, bool (*kind)(const assets::ItemRow&)) {
    for (const auto& [place, item] : bag.places) {
        if (place < game::slot::kWorn || place >= bag.size) continue;
        if (const assets::ItemRow* row = items.Find(item.refId); row != nullptr && kind(*row)) return place;
    }
    return -1;
}

namespace {
// What every socket operation looks at first: a bag place (0x6C06), an item there (3).
bool SocketFirst(game::Inventory& bag, const assets::ItemTable& items, std::uint8_t target, std::uint16_t& code, game::ItemInstance*& item,
                 const assets::ItemRow*& row) {
    code = 0;
    item = nullptr;
    row = nullptr;
    if (!InBag(bag, target)) {
        code = alchemy::code::kSocketSlot;
        return false;
    }
    const auto it = bag.places.find(target);
    item = it != bag.places.end() ? &it->second : nullptr;
    row = item != nullptr ? items.Find(item->refId) : nullptr;
    if (row == nullptr) code = alchemy::code::kSocketNoItem;
    return code == 0;
}
// The item's own codes, which come before the material is looked for.
bool EarlyCode(std::uint16_t code) {
    return code == alchemy::code::kSocketSlot || code == alchemy::code::kOnlyEquipment || code == alchemy::code::kCannotReinforce ||
           code == alchemy::code::kSocketNotStuff;
}
} // namespace

// The smith's blessing: the bag place (9), an item there (3), an avatar (8), a rental still running (3), the blessing's
// checks (game::alchemy::CheckAvatarGrant), then the option written, the avatar saved at once, nothing taken. The
// original's roll is 100 %: it never fails. Ours asks for the smith in reach first.
void Alchemy::AvatarGrant(std::uint32_t entity, const proto::AvatarGrant& m) {
    const auto p = host_.AlchemyPlayer(entity);
    const assets::ItemTable* items = host_.AlchemyItems();
    const std::vector<assets::MagicOptionRow>* options = host_.AlchemyOptions();
    const std::vector<assets::MagicOptionAssignRow>* assign = host_.AlchemyAssign();
    if (!p || items == nullptr) return;
    constexpr auto op = AlchemyOperation::AvatarGrant;
    if (!host_.AlchemyAtNpc(entity, m.npc, proto::kNpcAvatarGrant)) return Refuse(entity, op, alchemy::code::kGrantWrong, m.place);
    if (!p->alive || !p->free) return Refuse(entity, op, kNotFree, m.place);
    game::Inventory& bag = *p->inventory;
    if (!InBag(bag, m.place)) return Refuse(entity, op, alchemy::code::kGrantPlace, m.place);
    const auto it = bag.places.find(m.place);
    game::ItemInstance* item = it != bag.places.end() ? &it->second : nullptr;
    const assets::ItemRow* row = item != nullptr ? items->Find(item->refId) : nullptr;
    if (row == nullptr) return Refuse(entity, op, alchemy::code::kGrantNoItem, m.place);
    if (!alchemy::IsGrantAvatar(*row)) return Refuse(entity, op, alchemy::code::kGrantNotAvatar, m.place);
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (item->rentUntil != 0 && item->rentUntil <= now) return Refuse(entity, op, alchemy::code::kGrantNoItem, m.place);
    if (options == nullptr || assign == nullptr) return Refuse(entity, op, alchemy::code::kGrantWrong, m.place);
    if (const std::uint16_t code = alchemy::CheckAvatarGrant(*row, *item, m.option, *options, *assign); code != 0) return Refuse(entity, op, code, m.place);
    const game::ItemInstance::Option written = alchemy::GrantAvatarOption(*row, *item, m.option, *options, random_);
    host_.AlchemyBag(entity, {m.place}, {});
    proto::AlchemyResult r;
    r.operation = op;
    r.outcome = AlchemyOutcome::Success;
    r.place = m.place;
    r.option = written.id;
    r.value = written.value;
    Answer(entity, r);
}

void Alchemy::SocketMake(std::uint32_t entity, const proto::SocketMake& m) {
    const auto p = host_.AlchemyPlayer(entity);
    const assets::ItemTable* items = host_.AlchemyItems();
    if (!p || items == nullptr) return;
    constexpr auto op = AlchemyOperation::SocketMake;
    if (!p->alive || !p->free) return Refuse(entity, op, kNotFree, m.target);
    game::Inventory& bag = *p->inventory;
    std::uint16_t code = 0;
    game::ItemInstance* item = nullptr;
    const assets::ItemRow* row = nullptr;
    if (!SocketFirst(bag, *items, m.target, code, item, row)) return Refuse(entity, op, code, m.target);
    // In the original's order: the item's kind, the maker (0x6C08), then the sockets.
    code = alchemy::CheckSocketMake(*row, *item, m.index);
    if (EarlyCode(code)) return Refuse(entity, op, code, m.target);
    const int maker = FirstOf(bag, *items, alchemy::IsSocketMaker);
    if (maker < 0) return Refuse(entity, op, alchemy::code::kSocketNoMaterial, m.target);
    if (code != 0) return Refuse(entity, op, code, m.target);
    alchemy::MakeSocket(*item, m.index);
    std::vector<std::uint8_t> touched{m.target};
    std::vector<std::int64_t> gone;
    UseOne(bag, std::uint8_t(maker), touched, gone);
    host_.AlchemyBag(entity, std::move(touched), std::move(gone));
    proto::AlchemyResult r;
    r.operation = op;
    r.outcome = AlchemyOutcome::Success;
    r.place = m.target;
    r.socket = m.index;
    Answer(entity, r);
}

void Alchemy::SocketInsert(std::uint32_t entity, const proto::SocketInsert& m) {
    const auto p = host_.AlchemyPlayer(entity);
    const assets::ItemTable* items = host_.AlchemyItems();
    if (!p || items == nullptr) return;
    constexpr auto op = AlchemyOperation::SocketInsert;
    if (!p->alive || !p->free) return Refuse(entity, op, kNotFree, m.target);
    game::Inventory& bag = *p->inventory;
    std::uint16_t code = 0;
    game::ItemInstance* item = nullptr;
    const assets::ItemRow* row = nullptr;
    if (!SocketFirst(bag, *items, m.target, code, item, row)) return Refuse(entity, op, code, m.target);
    if (!InBag(bag, m.stone) || m.stone == m.target) return Refuse(entity, op, alchemy::code::kSocketSlot, m.target);
    const game::ItemInstance* stone = bag.At(m.stone);
    const assets::ItemRow* stoneRow = stone != nullptr ? items->Find(stone->refId) : nullptr;
    if (stoneRow == nullptr) return Refuse(entity, op, alchemy::code::kRecipeMissing, m.target);
    if (const std::uint16_t c = alchemy::CheckSocketInsert(*row, *item, *stoneRow, m.index); c != 0) return Refuse(entity, op, c, m.target);
    // A good start has no answer until it completes (on the 4th tick, checked again then).
    Run run;
    run.operation = op;
    run.target = m.target;
    run.material = m.stone;
    run.socket = m.index;
    run.targetSerial = item->serial;
    run.materialSerial = stone->serial;
    runs_[entity] = std::move(run);
}

void Alchemy::SocketInserted(std::uint32_t entity, Run& run) {
    const auto p = host_.AlchemyPlayer(entity);
    const assets::ItemTable* items = host_.AlchemyItems();
    if (!p || items == nullptr) return;
    constexpr auto op = AlchemyOperation::SocketInsert;
    game::Inventory& bag = *p->inventory;
    // The same items where they were (0x5406, 0x5407), then the insertion's checks again.
    const auto it = bag.places.find(run.target);
    game::ItemInstance* item = it != bag.places.end() && it->second.serial == run.targetSerial ? &it->second : nullptr;
    const game::ItemInstance* stone = bag.At(run.material);
    if (stone != nullptr && stone->serial != run.materialSerial) stone = nullptr;
    const assets::ItemRow* row = item != nullptr ? items->Find(item->refId) : nullptr;
    const assets::ItemRow* stoneRow = stone != nullptr ? items->Find(stone->refId) : nullptr;
    if (row == nullptr) return Refuse(entity, op, alchemy::code::kTargetMissing, run.target);
    if (stoneRow == nullptr) return Refuse(entity, op, alchemy::code::kRecipeMissing, run.target);
    if (const std::uint16_t c = alchemy::CheckSocketInsert(*row, *item, *stoneRow, run.socket); c != 0) return Refuse(entity, op, c, run.target);
    // The level: the lowest of three picks among the stone's group's rows.
    static const std::vector<assets::MagicOptGroupRow> kNone;
    const std::vector<assets::MagicOptGroupRow>* groups = host_.AlchemySocketGroups();
    const assets::MagicOptGroupRow* picked = alchemy::RollSocketLevel(alchemy::StoneSkills(groups != nullptr ? *groups : kNone, *stoneRow), random_);
    if (picked == nullptr) return Refuse(entity, op, alchemy::code::kRecipeMismatch, run.target);
    const std::uint32_t stoneId = stoneRow->refObjId;
    alchemy::InsertStone(*item, run.socket, picked->optId, picked->optLevel, stoneId);
    std::vector<std::uint8_t> touched{run.target};
    std::vector<std::int64_t> gone;
    UseOne(bag, run.material, touched, gone);
    host_.AlchemyBag(entity, std::move(touched), std::move(gone));
    proto::AlchemyResult r;
    r.operation = op;
    r.outcome = AlchemyOutcome::Success;
    r.place = run.target;
    r.socket = run.socket;
    r.option = picked->optId;
    r.value = std::uint32_t(picked->optLevel);
    r.stone = stoneId;
    Answer(entity, r);
}

void Alchemy::SocketErase(std::uint32_t entity, const proto::SocketErase& m) {
    const auto p = host_.AlchemyPlayer(entity);
    const assets::ItemTable* items = host_.AlchemyItems();
    if (!p || items == nullptr) return;
    constexpr auto op = AlchemyOperation::SocketErase;
    if (!p->alive || !p->free) return Refuse(entity, op, kNotFree, m.target);
    game::Inventory& bag = *p->inventory;
    std::uint16_t code = 0;
    game::ItemInstance* item = nullptr;
    const assets::ItemRow* row = nullptr;
    if (!SocketFirst(bag, *items, m.target, code, item, row)) return Refuse(entity, op, code, m.target);
    // The item's checks, the Destruction Rondo (0x6C08), then a stone there (0x6C11).
    code = alchemy::CheckSocketErase(*row, *item, m.index);
    if (EarlyCode(code)) return Refuse(entity, op, code, m.target);
    const int rondo = FirstOf(bag, *items, alchemy::IsSocketEraser);
    if (rondo < 0) return Refuse(entity, op, alchemy::code::kSocketNoMaterial, m.target);
    if (code != 0) return Refuse(entity, op, code, m.target);
    const game::ItemSocket was = item->sockets[m.index];
    alchemy::EraseStone(*item, m.index);
    std::vector<std::uint8_t> touched{m.target};
    std::vector<std::int64_t> gone;
    UseOne(bag, std::uint8_t(rondo), touched, gone);
    host_.AlchemyBag(entity, std::move(touched), std::move(gone));
    proto::AlchemyResult r;
    r.operation = op;
    r.outcome = AlchemyOutcome::Success;
    r.place = m.target;
    r.socket = m.index;
    r.option = was.skill;
    r.value = was.level;
    r.stone = was.stone;
    Answer(entity, r);
}

} // namespace sr::server
