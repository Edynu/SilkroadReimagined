#include "sr/game/Inventory.h"

#include <algorithm>

#include "sr/assets/GameData.h"

namespace sr::game {

namespace {

using proto::ItemStatus;

ItemStatus FromCheck(EquipCheck check) {
    switch (check) {
    case EquipCheck::Ok: return ItemStatus::Ok;
    case EquipCheck::NotWearable:
    case EquipCheck::WrongSlot: return ItemStatus::WrongSlot;
    case EquipCheck::Level: return ItemStatus::Level;
    case EquipCheck::Race: return ItemStatus::Race;
    case EquipCheck::Sex: return ItemStatus::Sex;
    case EquipCheck::Broken: return ItemStatus::Broken;
    case EquipCheck::Mastery: return ItemStatus::Mastery;
    }
    return ItemStatus::Failed;
}

// What may stand in place 7 beside a weapon: a shield with a one-handed one (or none), the
// ammunition a bow or crossbow shoots (AmmunitionFor).
bool FitsBeside(const assets::ItemRow* weapon, const assets::ItemRow& side) {
    if (IsAmmunition(side)) return weapon != nullptr && AmmunitionFor(*weapon) == side.typeId[3];
    return weapon == nullptr || !TwoHanded(*weapon);
}

} // namespace

// Clothes and robes (TID3 1 Chinese, 9 European) are not worn with armour (2, 3, 10, 11); light
// and heavy armour mix (the original's refusal UIIT_MSG_STRGERR_CANT_MIX_EXCLUSIVE_ARMOR_TYPE; the
// 1.188 client's own test of a bag item, its bit 0x80).
// 0: not a protector part, 1 clothes, 2 armour.
int ArmourFamily(const assets::ItemRow& row) {
    if (KindOf(row) != ItemKind::Armour) return 0;
    const int t = row.typeId[2];
    return t == 1 || t == 9 ? 1 : t == 2 || t == 3 || t == 10 || t == 11 ? 2 : 0;
}

const ItemInstance* Inventory::At(std::uint8_t place) const {
    const auto it = places.find(place);
    return it == places.end() ? nullptr : &it->second;
}

int Inventory::FreeBagPlace(int except) const {
    for (int p = slot::kWorn; p < size; ++p) {
        if (p != except && !places.contains(std::uint8_t(p))) return p;
    }
    return -1;
}

int Inventory::CompleteArmourFamily(const assets::ItemTable& items) const {
    int family = 0;
    for (int place = slot::kHead; place <= slot::kFeet; ++place) {
        const ItemInstance* item = At(std::uint8_t(place));
        const assets::ItemRow* row = item != nullptr ? items.Find(item->refId) : nullptr;
        if (row == nullptr || KindOf(*row) != ItemKind::Armour) return 0;
        if (family != 0 && row->typeId[2] != family) return 0;
        family = row->typeId[2];
    }
    return family;
}

int Inventory::FindSameType(const assets::ItemTable& items, const assets::ItemRow& like) const {
    for (const auto& [place, item] : places) {
        if (place < slot::kWorn) continue;
        const assets::ItemRow* row = items.Find(item.refId);
        if (row != nullptr && row->typeId[0] == like.typeId[0] && row->typeId[1] == like.typeId[1] && row->typeId[2] == like.typeId[2] &&
            row->typeId[3] == like.typeId[3]) {
            return place;
        }
    }
    return -1;
}

std::vector<proto::EquippedItem> Inventory::Worn() const {
    std::vector<proto::EquippedItem> out;
    for (const auto& [place, item] : places) {
        if (place < slot::kWorn) out.push_back({item.refId, std::uint8_t(TotalPlus(item))}); // others see the total
    }
    return out;
}

namespace {
proto::ItemRecord RecordOf(std::uint8_t place, const ItemInstance* i) {
    proto::ItemRecord r;
    r.slot = place;
    if (i != nullptr) {
        r.item = i->refId;
        r.plus = i->plus;
        r.variance = i->variance;
        r.durability = i->durability;
        r.quantity = i->count;
        for (const ItemInstance::Option& o : i->options) r.magic.push_back(std::uint64_t(o.id) | std::uint64_t(o.value) << 32);
        r.advanced = i->advanced;
        for (std::size_t k = 0; k < i->sockets.size(); ++k) {
            const ItemSocket& s = i->sockets[k];
            if (s.made) r.sockets.push_back({std::uint8_t(k), s.skill, s.level, s.stone});
        }
        r.creator = i->creator;
    }
    return r;
}
} // namespace

proto::ItemRecord Inventory::Record(std::uint8_t place) const { return RecordOf(place, At(place)); }

const ItemInstance* Inventory::Avatar(std::uint8_t place) const {
    const auto it = avatars.find(place);
    return it == avatars.end() ? nullptr : &it->second;
}

proto::ItemRecord Inventory::AvatarRecord(std::uint8_t place) const { return RecordOf(place, Avatar(place)); }

proto::ItemStatus Inventory::CheckWearAvatar(const assets::ItemTable& items, const Wearer& who, std::uint8_t from) const {
    if (from < slot::kWorn || from >= size) return ItemStatus::BadSlot;
    const ItemInstance* item = At(from);
    if (item == nullptr) return ItemStatus::NoItem;
    const assets::ItemRow* row = items.Find(item->refId);
    if (row == nullptr || AvatarPlaceFor(*row) < 0) return ItemStatus::WrongSlot;
    if (const ItemStatus s = FromCheck(CanWearAvatar(*row, who)); s != ItemStatus::Ok) return s;
    // One of each type (the server looks the avatar places through for an item of the same four type ids: 0x180E).
    const assets::ItemRow* dress = nullptr;
    for (const auto& [place, worn] : avatars) {
        const assets::ItemRow* wornRow = items.Find(worn.refId);
        if (wornRow == nullptr) continue;
        if (wornRow->typeId == row->typeId) return ItemStatus::PlaceTaken;
        if (AvatarPlaceFor(*wornRow) == avatar::kDress) dress = wornRow;
    }
    // An attachment over a dress whose ChildItemCount is 1: with no dress the 1.188 client's own refusal
    // (UIIT_STT_AVATAR_MAIL_ONLY_WEAR), over one that takes none the server's 0x180E.
    if (AvatarPlaceFor(*row) == avatar::kAttachment) {
        if (dress == nullptr) return ItemStatus::NeedsDress;
        if (dress->childItems <= 0) return ItemStatus::WrongSlot;
    }
    return ItemStatus::Ok;
}

Inventory::AvatarResult Inventory::WearAvatar(const assets::ItemTable& items, const Wearer& who, std::uint8_t from) {
    AvatarResult result;
    result.status = CheckWearAvatar(items, who, from);
    if (result.status != ItemStatus::Ok) return result;
    const auto src = places.find(from);
    int place = AvatarPlaceFor(*items.Find(src->second.refId));
    if (avatars.contains(std::uint8_t(place))) {
        place = -1;
        for (int p = 0; p < avatar::kPlaces && place < 0; ++p) {
            if (!avatars.contains(std::uint8_t(p))) place = p;
        }
        if (place < 0) {
            result.status = ItemStatus::PlaceTaken;
            return result;
        }
    }
    avatars[std::uint8_t(place)] = std::move(src->second);
    places.erase(src);
    result.bag = {from};
    result.avatars = {std::uint8_t(place)};
    return result;
}

Inventory::AvatarResult Inventory::TakeOffAvatar(const assets::ItemTable& items, std::uint8_t place) {
    AvatarResult result;
    const auto it = avatars.find(place);
    if (place >= avatar::kPlaces || it == avatars.end()) {
        result.status = ItemStatus::NoItem;
        return result;
    }
    const int first = FreeBagPlace();
    if (first < 0) {
        result.status = ItemStatus::BagFull;
        return result;
    }
    // The dress takes the attachment over it off too (TID 3/1/13/3).
    int attachment = -1, second = -1;
    const assets::ItemRow* row = items.Find(it->second.refId);
    if (row != nullptr && AvatarPlaceFor(*row) == avatar::kDress) {
        for (const auto& [p, worn] : avatars) {
            const assets::ItemRow* wornRow = items.Find(worn.refId);
            if (wornRow != nullptr && AvatarPlaceFor(*wornRow) == avatar::kAttachment) attachment = p;
        }
        if (attachment >= 0) {
            second = FreeBagPlace(first);
            if (second < 0) {
                result.status = ItemStatus::BagFull;
                return result;
            }
        }
    }
    places[std::uint8_t(first)] = std::move(it->second);
    avatars.erase(it);
    result.bag = {std::uint8_t(first)};
    result.avatars = {place};
    if (attachment >= 0) {
        places[std::uint8_t(second)] = std::move(avatars[std::uint8_t(attachment)]);
        avatars.erase(std::uint8_t(attachment));
        result.bag.push_back(std::uint8_t(second));
        result.avatars.push_back(std::uint8_t(attachment));
    }
    return result;
}

ItemInstance Inventory::FromRecord(const proto::ItemRecord& r) {
    ItemInstance i;
    i.refId = r.item;
    i.plus = r.plus;
    i.variance = r.variance;
    i.durability = r.durability;
    i.count = r.quantity;
    for (std::uint64_t m : r.magic) i.options.push_back({std::uint32_t(m & 0xFFFFFFFFu), std::uint32_t(m >> 32)});
    i.advanced = r.advanced;
    for (const proto::SocketRecord& s : r.sockets) {
        if (s.slot < i.sockets.size()) i.sockets[s.slot] = {true, s.skill, s.level, s.stone};
    }
    i.creator = r.creator;
    return i;
}

Inventory::MoveResult Inventory::Move(const assets::ItemTable& items, const Wearer& who, std::uint8_t from, std::uint8_t to,
                                      std::uint16_t quantity) {
    MoveResult result;
    const auto refuse = [&](ItemStatus status) {
        result.status = status;
        return result;
    };
    if (from == to || from >= size || to >= size) return refuse(ItemStatus::BadSlot);
    const auto src = places.find(from);
    if (src == places.end()) return refuse(ItemStatus::NoItem);
    const assets::ItemRow* row = items.Find(src->second.refId);
    if (row == nullptr) return refuse(ItemStatus::Failed);
    const std::uint16_t count = quantity == 0 ? src->second.count : quantity;
    if (count == 0 || count > src->second.count) return refuse(ItemStatus::BadSlot);
    const auto dst = places.find(to);
    const assets::ItemRow* dstRow = dst != places.end() ? items.Find(dst->second.refId) : nullptr;
    if (dst != places.end() && dstRow == nullptr) return refuse(ItemStatus::Failed);
    const auto swap = [&] {
        if (dst != places.end()) {
            std::swap(src->second, dst->second);
        } else {
            places[to] = std::move(src->second);
            places.erase(from);
        }
        result.touched = {from, to};
    };

    if (to < slot::kWorn) {
        // Onto the body: only where its type is worn and its requirements are met; what was there
        // goes back where it came from (and must be worn there when that is a worn place too).
        if (const ItemStatus s = FromCheck(CanEquip(*row, to, who, src->second)); s != ItemStatus::Ok) return refuse(s);
        if (count != src->second.count) return refuse(ItemStatus::BadSlot); // a whole stack (ammunition) only
        if (const int family = ArmourFamily(*row); family != 0) {
            for (int place = slot::kHead; place <= slot::kFeet; ++place) {
                const ItemInstance* worn = place == to ? nullptr : At(std::uint8_t(place));
                const assets::ItemRow* wornRow = worn != nullptr ? items.Find(worn->refId) : nullptr;
                const int other = wornRow != nullptr ? ArmourFamily(*wornRow) : 0;
                if (other != 0 && other != family) return refuse(ItemStatus::MixedArmour);
            }
        }
        if (dst != places.end() && from < slot::kWorn) {
            if (const ItemStatus s = FromCheck(CanEquip(*dstRow, from, who, dst->second)); s != ItemStatus::Ok) return refuse(s);
        }
        const ItemInstance* weaponItem = At(slot::kWeapon);
        const assets::ItemRow* weapon = weaponItem != nullptr ? items.Find(weaponItem->refId) : nullptr;
        // A shield put on beside a two-handed weapon sends the weapon to the bag: a free place, else the shield's own; a
        // bow or crossbow whose
        // ammunition the shield replaces needs a free place (the ammunition takes the shield's), else 0x1807.
        int weaponTo = -1;
        if (to == slot::kShield && !FitsBeside(weapon, *row)) {
            if (IsAmmunition(*row) || weapon == nullptr || !TwoHanded(*weapon) || from < slot::kWorn) return refuse(ItemStatus::WrongSlot);
            weaponTo = FreeBagPlace(from);
            if (weaponTo < 0 && dst == places.end()) weaponTo = from; // `from` empties
            if (weaponTo < 0) return refuse(ItemStatus::BagFull);
        }
        int pushTo = -1; // where the shield (or the wrong ammunition) goes when a weapon replaces it
        int ammoFrom = -1; // a bow or crossbow put on with the bag full: its ammunition's place takes the shield
        if (to == slot::kWeapon) {
            if (const ItemInstance* side = At(slot::kShield)) {
                const assets::ItemRow* sideRow = items.Find(side->refId);
                // Ammunition of another kind stays beside a bow or crossbow put on (the original moves only a shield or
                // the ammunition off a weapon that shoots none:; a bow for a crossbow leaves the arrows).
                const bool keep = sideRow != nullptr && IsAmmunition(*sideRow) && AmmunitionFor(*row) != 0;
                if (!keep && (sideRow == nullptr || !FitsBeside(row, *sideRow))) {
                    // The old weapon (if any) goes to `from`; the side item to another free place.
                    pushTo = dst != places.end() || from < slot::kWorn ? FreeBagPlace() : FreeBagPlace(from);
                    if (dst == places.end() && from >= slot::kWorn && pushTo < 0) pushTo = from; // `from` empties
                    if (pushTo < 0 && sideRow != nullptr && !IsAmmunition(*sideRow) && AmmunitionFor(*row) != 0) {
                        // The bag full: the shield into its ammunition's bag place, the ammunition beside the weapon.
                        for (int p = slot::kWorn; p < size && ammoFrom < 0; ++p) {
                            const auto it = places.find(std::uint8_t(p));
                            const assets::ItemRow* ammo = it != places.end() && p != from ? items.Find(it->second.refId) : nullptr;
                            if (ammo != nullptr && IsAmmunition(*ammo) && ammo->typeId[3] == AmmunitionFor(*row)) ammoFrom = p;
                        }
                    }
                    if (pushTo < 0 && ammoFrom < 0) return refuse(ItemStatus::BagFull);
                }
            }
        }
        swap();
        if (weaponTo >= 0) {
            // The two-handed weapon out of the hand (into `from` when that emptied; else a free place).
            places[std::uint8_t(weaponTo)] = std::move(places[slot::kWeapon]);
            places.erase(slot::kWeapon);
            result.touched.push_back(slot::kWeapon);
            if (weaponTo != from) result.touched.push_back(std::uint8_t(weaponTo));
        }
        if (pushTo >= 0) {
            places[std::uint8_t(pushTo)] = std::move(places[slot::kShield]);
            places.erase(slot::kShield);
            result.touched.push_back(slot::kShield);
            if (pushTo != from) result.touched.push_back(std::uint8_t(pushTo));
        } else if (ammoFrom >= 0) {
            std::swap(places[slot::kShield], places[std::uint8_t(ammoFrom)]);
            result.touched.push_back(slot::kShield);
            result.touched.push_back(std::uint8_t(ammoFrom));
        }
        // A bow or a crossbow taken in hand with nothing beside it takes the first stack of its arrows
        // (bolts) from the bag (ours: the original's choice of stack is not traced).
        if (to == slot::kWeapon && At(slot::kShield) == nullptr) {
            if (const int kind = AmmunitionFor(*row); kind != 0) {
                for (int p = slot::kWorn; p < size; ++p) {
                    const auto it = places.find(std::uint8_t(p));
                    const assets::ItemRow* ammo = it != places.end() ? items.Find(it->second.refId) : nullptr;
                    if (ammo == nullptr || !IsAmmunition(*ammo) || ammo->typeId[3] != kind) continue;
                    places[slot::kShield] = std::move(it->second);
                    places.erase(it);
                    result.touched.push_back(slot::kShield);
                    result.touched.push_back(std::uint8_t(p));
                    break;
                }
            }
        }
        return result;
    }
    if (from < slot::kWorn) {
        // Off the body into the bag: what is there must be worn in its place, or the place free.
        if (count != src->second.count) return refuse(ItemStatus::BadSlot);
        if (dst != places.end()) {
            if (const ItemStatus s = FromCheck(CanEquip(*dstRow, from, who, dst->second)); s != ItemStatus::Ok) return refuse(s);
        }
        // The weapon off: the arrows (bolts) beside it go to the bag with it, unless what takes its place
        // shoots them.
        int ammoTo = -1;
        if (from == slot::kWeapon) {
            if (const ItemInstance* side = At(slot::kShield)) {
                const assets::ItemRow* sideRow = items.Find(side->refId);
                const assets::ItemRow* next = dst != places.end() ? dstRow : nullptr;
                if (sideRow != nullptr && IsAmmunition(*sideRow) && !FitsBeside(next, *sideRow)) {
                    ammoTo = FreeBagPlace(to);
                    if (ammoTo < 0) return refuse(ItemStatus::BagFull);
                }
            }
        }
        swap();
        if (ammoTo >= 0) {
            places[std::uint8_t(ammoTo)] = std::move(places[slot::kShield]);
            places.erase(slot::kShield);
            result.touched.push_back(slot::kShield);
            result.touched.push_back(std::uint8_t(ammoTo));
        }
        return result;
    }
    // Bag to bag: the same stackable merges, tops up or swaps when full; part of a
    // stack onto an empty place splits; anything else swaps.
    const int most = MaxStack(*row);
    if (dst != places.end() && dst->second.refId == src->second.refId && most > 1) {
        ItemInstance& a = src->second;
        ItemInstance& b = dst->second;
        if (b.count >= most) {
            if (count != a.count) return refuse(ItemStatus::BadSlot);
            swap();
            return result;
        }
        const std::uint16_t moved = std::uint16_t(std::min<int>(count, most - b.count));
        b.count = std::uint16_t(b.count + moved);
        a.count = std::uint16_t(a.count - moved);
        if (a.count == 0) {
            result.gone.push_back(a.serial);
            places.erase(src);
        }
        result.touched = {from, to};
        return result;
    }
    if (count < src->second.count) {
        if (dst != places.end()) return refuse(ItemStatus::BadSlot); // a split goes to an empty place
        ItemInstance part = src->second;
        part.serial = 0;
        part.count = count;
        src->second.count = std::uint16_t(src->second.count - count);
        places[to] = std::move(part);
        result.touched = {from, to};
        return result;
    }
    swap();
    return result;
}

} // namespace sr::game
