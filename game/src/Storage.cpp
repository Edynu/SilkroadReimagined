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
    if (fromSide != toSide) {
        // Between the bag and the storage the whole item moves, its count not read (the original's transfer): to the
        // asked place, or the first free place of that side when it is taken (the storage from 0, the bag from 13); no
        // merge, no swap, no split.
        int place = to;
        if (dst.contains(to)) {
            place = -1;
            if (toSide == Side::Inventory) {
                place = inventory.FreeBagPlace();
            } else {
                for (int p = 0; p < size && place < 0; ++p) {
                    if (!places.contains(std::uint8_t(p))) place = p;
                }
            }
            if (place < 0) return refuse(ItemStatus::BagFull);
        }
        dst[std::uint8_t(place)] = std::move(s->second);
        src.erase(s);
        touch(fromSide, from);
        touch(toSide, std::uint8_t(place));
        return result;
    }
    const auto d = dst.find(to);
    const int maxStack = MaxStack(*row);
    const bool whole = IsWholeItem(*row);

    // Inside the storage: merges and swaps as in the bag; never a split (only the inventory splits, the original's 5).
    if (d == dst.end()) {
        if (count != s->second.count) return refuse(ItemStatus::BadSlot);
        dst[to] = std::move(s->second);
        src.erase(s);
    } else if (d->second.refId == s->second.refId && maxStack > 1 && !whole && int(d->second.count) < maxStack) {
        // The same stackable with room: as much of the whole source stack as fits, the rest stays.
        const int room = maxStack - int(d->second.count);
        const std::uint16_t moved = std::uint16_t(std::min<int>(room, s->second.count));
        d->second.count = std::uint16_t(d->second.count + moved);
        if (moved == s->second.count) {
            result.gone.push_back(s->second.serial);
            src.erase(s);
        } else {
            s->second.count = std::uint16_t(s->second.count - moved);
        }
    } else {
        // Anything else (another item, a full stack): the two swap, each keeping its serial.
        std::swap(s->second, d->second);
    }
    touch(fromSide, from);
    touch(toSide, to);
    return result;
}

} // namespace sr::game
