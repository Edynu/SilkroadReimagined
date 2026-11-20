#pragma once

// A character's inventory: the places 0..12 it wears items in, then the
// bag up to its size; the gold. How an item moves between two places, as the original's server
// answers the client's move (kind 0): onto the body only where the item's type is worn and its
// requirements are met, a two-handed weapon taking the shield off into the bag, off the body into
// the bag, stacks merged, topped up or split in the bag, anything else swapped. The avatars worn beside, in places
// of their own. Headless: the game server keeps one per player; tests run it on hand-made rows.

#include <cstdint>
#include <map>
#include <vector>

#include "sr/game/Items.h"
#include "sr/game/WorldProtocol.h"

namespace sr::assets {
class ItemTable;
}

namespace sr::game {

// An armour piece's family for the mix rule: 0 none (not head..feet armour), 1 clothes and robes (TID3 1, 9), 2 armour
// (2, 3, 10, 11). Pieces of the two families are never worn together.
int ArmourFamily(const assets::ItemRow& row);

class Inventory {
public:
    std::uint8_t size = 45;              // places, the 13 worn ones included
    std::uint64_t gold = 0;
    std::map<std::uint8_t, ItemInstance> places; // by place

    // The 13 worn places and at most three tabs of 32 (the 1.188 client's inventory window: makes a tab per
    // 32 places and stops above three; refuses to grow to 100 bag places or more).
    static constexpr std::uint8_t kMaxSize = 13 + 3 * 32;

    struct MoveResult {
        proto::ItemStatus status = proto::ItemStatus::Ok;
        std::vector<std::uint8_t> touched; // the places that changed
        std::vector<std::int64_t> gone;    // serials of items merged away
    };
    // Moves `quantity` (0: all) of what is in `from` to `to`; nothing changes unless Ok.
    MoveResult Move(const assets::ItemTable& items, const Wearer& who, std::uint8_t from, std::uint8_t to, std::uint16_t quantity);

    const ItemInstance* At(std::uint8_t place) const;
    // The first empty place of the bag (not `except`); -1 when it is full.
    int FreeBagPlace(int except = -1) const;
    // The complete armour set's family: the TID3 every one of places 0..5 holds armour of (1 / 9 clothes and robe,
    // 2 / 10 protector and light, 3 / 11 armor
    // and heavy), broken or unfit pieces counted; 0 when one is empty or the families differ.
    int CompleteArmourFamily(const assets::ItemTable& items) const;
    // The first place of the bag holding an item of these four type ids; -1 when none (the next stack of arrows or
    // bolts).
    int FindSameType(const assets::ItemTable& items, const assets::ItemRow& like) const;
    // What the character wears, by place (the look others see).
    std::vector<proto::EquippedItem> Worn() const;
    // An item where it lies, as the wire carries it; and back.
    proto::ItemRecord Record(std::uint8_t place) const;
    static ItemInstance FromRecord(const proto::ItemRecord& record);

    // --- The avatars ---
    // What the character wears of avatars, by avatar place (avatar::kHat .. kSpirit; _InventoryForAvatar's slots).
    std::map<std::uint8_t, ItemInstance> avatars;
    const ItemInstance* Avatar(std::uint8_t place) const;
    proto::ItemRecord AvatarRecord(std::uint8_t place) const;
    struct AvatarResult {
        proto::ItemStatus status = proto::ItemStatus::Ok;
        std::vector<std::uint8_t> bag;     // the bag's places that changed
        std::vector<std::uint8_t> avatars; // the avatar places that changed
    };
    // Whether the avatar in the bag's place `from` may be put on, as the original's server decides: an avatar of the
    // bag; the level, country and sex; none of
    // its four type ids worn already (PlaceTaken); an attachment only over a dress (NeedsDress) that takes one
    // (WrongSlot: the original's 0x180E).
    proto::ItemStatus CheckWearAvatar(const assets::ItemTable& items, const Wearer& who, std::uint8_t from) const;
    // Puts it on, into the place of its kind (another free avatar place when an imported character has that one
    // taken by another kind).
    AvatarResult WearAvatar(const assets::ItemTable& items, const Wearer& who, std::uint8_t from);
    // Takes the avatar in `place` off into the bag's first free place; the dress takes the attachment worn over it
    // into the next one (BagFull when the bag has no room for both).
    AvatarResult TakeOffAvatar(const assets::ItemTable& items, std::uint8_t place);
};

} // namespace sr::game
