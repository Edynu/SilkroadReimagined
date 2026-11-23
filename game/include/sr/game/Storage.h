#pragma once

// An account's storage on a game server (C7i; the original's _Chest is the account's, not a
// character's): 150 places (5 pages of 30; 180 when expanded), its own gold. How an item moves
// between the storage and the inventory's bag, or within the storage: onto an empty place (all of
// it, or part of a stack: a split), onto the same stackable item (merged up to its stack size),
// anything else swapped. Worn items do not go in directly. Headless: the game server keeps one per
// player while its window is open.

#include <cstdint>
#include <map>
#include <vector>

#include "sr/game/Inventory.h"

namespace sr::game {

struct Storage {
    static constexpr std::uint8_t kDefaultSize = 150, kMaxSize = 210; // 150 + 30 + 30: two expansions
    std::uint8_t size = kDefaultSize;
    std::uint64_t gold = 0;
    std::map<std::uint8_t, ItemInstance> places; // 0 .. size - 1

    const ItemInstance* At(std::uint8_t place) const;
    proto::ItemRecord Record(std::uint8_t place) const;

    // Where an item goes: the storage or the inventory (a bag place there).
    enum class Side : std::uint8_t { Storage, Inventory };
    struct MoveResult {
        proto::ItemStatus status = proto::ItemStatus::Ok;
        std::vector<std::uint8_t> storageTouched, inventoryTouched;
        std::vector<std::int64_t> gone; // serials of items merged away into another stack (either side): their rows go
    };
    // Moves what is at `from` on one side to `to` on the other (or the same) side; nothing changes unless Ok. Between the
    // sides the whole item (`quantity` not read), to `to` or that side's first free place; inside the storage a merge
    // (the whole stack as far as it fits) or a swap, never a split (`quantity` must be the whole). An item keeps its serial
    // whichever side it goes to (the original moves the same item id).
    MoveResult Move(const assets::ItemTable& items, Inventory& inventory, Side fromSide, std::uint8_t from, Side toSide, std::uint8_t to,
                    std::uint16_t quantity);
};

} // namespace sr::game
