#include "sr/game/Mall.h"

#include <algorithm>

namespace sr::game {

bool MallCountIsStack(const assets::MallGood& good, const assets::ItemRow& row) {
    return row.typeId[0] == 3 && row.typeId[1] == 3 && good.data == 0 && MaxStack(row) > 1;
}

int MallMostCount(const assets::MallGood& good, const assets::ItemRow& row) {
    return MallCountIsStack(good, row) ? MaxStack(row) : kMallMostAtOnce;
}

int MallClampCount(const assets::MallGood& good, const assets::ItemRow& row, int count) { return std::clamp(count, 1, MallMostCount(good, row)); }

int MallPlacesNeeded(const assets::MallGood& good, const assets::ItemRow& row, int count) {
    return MallCountIsStack(good, row) ? 1 : MallClampCount(good, row, count);
}

std::uint64_t MallPrice(const assets::MallGood& good, int count) { return std::uint64_t(good.price) * std::uint64_t(std::max(count, 0)); }

bool IsDevilSpirit(const assets::ItemRow& row) {
    return row.typeId[0] == 3 && row.typeId[1] == 1 && row.typeId[2] == 14 && row.typeId[3] == 1;
}

int MallAvatarKind(const assets::ItemRow& row) {
    if (row.typeId[0] != 3 || row.typeId[1] != 1) return 0;
    if (row.typeId[2] == 13 && row.typeId[3] >= 1 && row.typeId[3] <= 3) return row.typeId[3];
    if (IsDevilSpirit(row)) return 4;
    return 0;
}

std::vector<ItemInstance> MallItems(const assets::MallGood& good, const assets::ItemRow& row, int count, std::int64_t nowSeconds) {
    std::vector<ItemInstance> out;
    const bool stack = MallCountIsStack(good, row);
    const int n = MallPlacesNeeded(good, row, count);
    const bool counted = DataIsCount(row);
    for (int k = 0; k < n; ++k) {
        ItemInstance item;
        item.refId = row.refObjId;
        item.plus = std::uint8_t(std::clamp(good.plus, 0, 255));
        item.variance = good.variance;
        if (stack) {
            item.count = std::uint16_t(MallClampCount(good, row, count));
        } else if (good.data > 0 && MaxStack(row) > 1) {
            item.count = std::uint16_t(std::min(good.data, MaxStack(row)));
        } else {
            item.count = 1;
        }
        if (IsDevilSpirit(row)) {
            item.durability = std::uint32_t(nowSeconds + std::max(row.params[0], 0)); // awake at purchase (re/58 5.3, inferred)
        } else if (!counted) {
            item.durability = good.data > 0 ? std::uint32_t(good.data) : NewDurability(row);
        }
        out.push_back(std::move(item));
    }
    return out;
}

} // namespace sr::game
