#pragma once

// The item mall's rules both sides share (ITEM_MALL_PLAN 1.5): how many of a good may be
// bought at once, the bag places it takes (the original's `_GetNeededSlotCountByBuyPackageItem`), its price, and the
// items it makes. Headless.

#include <cstdint>
#include <vector>

#include "sr/assets/GameData.h"
#include "sr/assets/Shop.h"
#include "sr/game/Items.h"

namespace sr::game {

// The most of one good bought at once that is not a chosen stack (the confirm window's spin, the server's check).
constexpr int kMallMostAtOnce = 5;

// A single stackable consumable (TID 3.3) whose package's `Data` is 0: the count chosen is one stack in one place.
bool MallCountIsStack(const assets::MallGood& good, const assets::ItemRow& row);
// The count's range: up to the item's stack for a chosen stack, else 1..5.
int MallMostCount(const assets::MallGood& good, const assets::ItemRow& row);
int MallClampCount(const assets::MallGood& good, const assets::ItemRow& row, int count);
// The bag places a purchase of `count` takes: 1 for a chosen stack, else one per item (the count).
int MallPlacesNeeded(const assets::MallGood& good, const assets::ItemRow& row, int count);
// The silk it costs: the price times the count.
std::uint64_t MallPrice(const assets::MallGood& good, int count);
// The items a purchase makes, one per place: the package's plus and white stats, the stack (`Data`, else the count for
// a chosen stack, else 1), the new durability; a Devil's Spirit awake from `nowSeconds` for its Param1 (its end in the
// data word).
std::vector<ItemInstance> MallItems(const assets::MallGood& good, const assets::ItemRow& row, int count, std::int64_t nowSeconds);

// The avatars' kinds the try-on wears: a hat, a dress, an attachment (TID 3/1/13/1..3),
// or the Devil's Spirit (3/1/14/1). 0: not one.
int MallAvatarKind(const assets::ItemRow& row);

// The Devil's Spirit (3/1/14/1): its awakening's end lives in the data word (Unix seconds; 0 never awakened).
bool IsDevilSpirit(const assets::ItemRow& row);

} // namespace sr::game
