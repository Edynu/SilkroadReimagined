#include "sr/game/Storage.h"

#include <algorithm>

#include "sr/assets/GameData.h"

namespace sr::game {

using proto::ItemStatus;

const ItemInstance* Storage::At(std::uint8_t place) const {
    const auto it = places.find(place);
    return it == places.end() ? nullptr : &it->second;
}

proto::ItemRecord Storage::Record(std::uint8_t place) const {
    // The inventory's wire form, the storage's place number.
    Inventory one;
    if (const ItemInstance* item = At(place)) one.places[place] = *item;
    return one.Record(place);
}

Storage::MoveResult Storage::Move(const assets::ItemTable& items, Inventory& inventory, Side fromSide, std::uint8_t from, Side toSide,
                                  std::uint8_t to, std::uint16_t quantity) {
    MoveResult result;
    const auto refuse = [&](ItemStatus status) {
        result = {};
        result.status = status;
        return result;
    };
    const auto fits = [&](Side side, std::uint8_t place) {
        return side == Side::Storage ? place < size : place >= slot::kWorn && place < inventory.size;
    };
    if (!fits(fromSide, from) || !fits(toSide, to) || (fromSide == toSide && from == to)) return refuse(ItemStatus::BadSlot);
    // Moving within the inventory is the inventory's own move.
    if (fromSide == Side::Inventory && toSide == Side::Inventory) return refuse(ItemStatus::BadSlot);
    std::map<std::uint8_t, ItemInstance>& src = fromSide == Side::Storage ? places : inventory.places;
    std::map<std::uint8_t, ItemInstance>& dst = toSide == Side::Storage ? places : inventory.places;
    const auto s = src.find(from);
    if (s == src.end()) return refuse(ItemStatus::NoItem);
    const assets::ItemRow* row = items.Find(s->second.refId);
    if (row == nullptr) return refuse(ItemStatus::Failed);
    const std::uint16_t count = quantity == 0 ? s->second.count : quantity;
    if (count == 0 || count > s->second.count) return refuse(ItemStatus::BadSlot);
    const auto touch = [&](Side side, std::uint8_t place) {
        (side == Side::Storage ? result.storageTouched : result.inventoryTouched).push_back(place);
    };
    const auto d = dst.find(to);
    const int maxStack = MaxStack(*row);

    if (d == dst.end()) {
        // An empty place: all of it (the same item, its serial kept, as the original moves one item id), or a split (a new
        // item, serial 0).
        ItemInstance moved = s->second;
        moved.count = count;
        if (count == s->second.count) {
            src.erase(s);
        } else {
            s->second.count = std::uint16_t(s->second.count - count);
            moved.serial = 0;
        }
        dst[to] = std::move(moved);
    } else if (d->second.refId == s->second.refId && maxStack > 1) {
        // The same stackable: topped up to its stack size, the rest stays.
        const int room = maxStack - int(d->second.count);
        if (room <= 0) return refuse(ItemStatus::BadSlot);
        const std::uint16_t moved = std::uint16_t(std::min<int>(room, count));
        d->second.count = std::uint16_t(d->second.count + moved);
        if (moved == s->second.count) {
            result.gone.push_back(s->second.serial);
            src.erase(s);
        } else {
            s->second.count = std::uint16_t(s->second.count - moved);
        }
    } else {
        // Anything else: the two swap (whole stacks only), each keeping its serial.
        if (count != s->second.count) return refuse(ItemStatus::BadSlot);
        std::swap(s->second, d->second);
    }
    touch(fromSide, from);
    touch(toSide, to);
    return result;
}

} // namespace sr::game
