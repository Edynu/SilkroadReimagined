// The inventory (C7e): the item stats the tooltip shows (game::ComputeStats),
// the moves the server allows (game::Inventory), and the game data they rely on.

#include <doctest/doctest.h>

#include <cstdlib>
#include <vector>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/game/Inventory.h"
#include "sr/game/Items.h"
#include "gui/HudWindows.h"

using namespace sr;
using game::ItemInstance;
using game::proto::ItemStatus;
namespace place = game::slot;

namespace {

assets::ItemRow Row(std::uint32_t id, const char* code, int tid2, int tid3, int tid4, int country = 3, int sex = 2, int level = 1) {
    assets::ItemRow r;
    r.refObjId = id;
    r.codename = code;
    r.typeId = {3, tid2, tid3, tid4};
    r.country = country;
    r.sex = sex;
    r.requirementTypes = {1, -1, -1, -1};
    r.requirementLevels = {level, 0, 0, 0};
    r.stats.assign(117 - assets::ItemRow::kFirstStatColumn + 1, 0.0f);
    return r;
}

void SetStat(assets::ItemRow& r, int column, float value) { r.stats[std::size_t(column - assets::ItemRow::kFirstStatColumn)] = value; }

// A small itemdata: Chinese armour, a sword, a shield, a bow and its arrows, a European sword, a
// woman's chest, a level-20 chest, a ring, potions.
assets::ItemTable Items() {
    assets::ItemTable t;
    auto chest = Row(1, "CHEST", 1, 1, 3, 0);
    SetStat(chest, 63, 40);
    SetStat(chest, 64, 50);
    t.Add(chest);
    t.Add(Row(2, "LEGS", 1, 1, 4, 0));
    auto sword = Row(10, "SWORD", 1, 6, 2, 0);
    SetStat(sword, 63, 60);
    SetStat(sword, 64, 70);
    t.Add(sword);
    t.Add(Row(11, "SHIELD", 1, 4, 1, 0));
    auto bow = Row(12, "BOW", 1, 6, 6, 0);
    SetStat(bow, 93, 1);  // two-handed
    t.Add(bow);
    t.Add(Row(13, "ARROWS", 3, 4, 1));
    t.Add(Row(20, "EU_SWORD", 1, 6, 7, 1));
    t.Add(Row(30, "WOMAN_CHEST", 1, 1, 3, 0, 0));
    t.Add(Row(31, "CHEST_20", 1, 1, 3, 0, 2, 20));
    t.Add(Row(40, "RING", 1, 5, 3));
    auto potion = Row(50, "POTION", 3, 1, 1);
    potion.maxStack = 50;
    t.Add(potion);
    return t;
}

ItemInstance Item(std::uint32_t id, std::uint32_t durability = 10, std::uint16_t count = 1) {
    ItemInstance i;
    i.refId = id;
    i.durability = durability;
    i.count = count;
    return i;
}

} // namespace

TEST_CASE("item stats: the variance's 5-bit slots, the plus, durability's 99 %") {
    auto sword = Row(10, "SWORD", 1, 6, 2);
    SetStat(sword, 63, 60);  // durability 60..70
    SetStat(sword, 64, 70);
    SetStat(sword, 95, 10);  // phy. attack min 10..20, max 20..30, +2 a plus
    SetStat(sword, 96, 20);
    SetStat(sword, 97, 20);
    SetStat(sword, 98, 30);
    SetStat(sword, 99, 2);
    SetStat(sword, 94, 18);  // 1.8 m
    sword.itemClass = 4;      // degree 2, Star
    sword.rarity = 2;
    ItemInstance i = Item(10, 65);
    i.plus = 3;
    i.variance = std::uint64_t(31) | std::uint64_t(31) << 20; // durability and phy. attack at their best
    const game::ItemStats s = game::ComputeStats(sword, i, nullptr);
    CHECK(s.durability.high == 70);
    CHECK(s.durability.low == 65);
    CHECK(s.durability.percent == 99); // the original's 0.31 makes a perfect one 99
    CHECK(s.phyAttack.low == 26);      // 20 + trunc(3 * 2)
    CHECK(s.phyAttack.high == 36);
    CHECK(s.phyAttack.percent == 100);
    CHECK(s.range == doctest::Approx(1.8));
    CHECK(s.degree == 2);
    CHECK(s.seal == 0);
    CHECK(game::VarianceSlot(i.variance, 4) == 31);
}

TEST_CASE("the inventory: wearing, taking off, the shield and the two-handed weapon, stacks") {
    const assets::ItemTable items = Items();
    const game::Wearer chinese{10, 0, true};
    game::Inventory inv;
    inv.places[13] = Item(10);
    inv.places[14] = Item(11);
    inv.places[15] = Item(12);
    inv.places[16] = Item(13, 0, 250);
    inv.places[17] = Item(1);

    // Into the places their types go to, nowhere else.
    CHECK(inv.Move(items, chinese, 13, place::kWeapon, 0).status == ItemStatus::Ok);
    CHECK(inv.At(place::kWeapon)->refId == 10);
    CHECK(inv.At(13) == nullptr);
    CHECK(inv.Move(items, chinese, 17, place::kHead, 0).status == ItemStatus::WrongSlot);
    CHECK(inv.Move(items, chinese, 17, place::kChest, 0).status == ItemStatus::Ok);
    CHECK(inv.Move(items, chinese, 14, place::kShield, 0).status == ItemStatus::Ok);
    // Arrows beside a sword: no.
    CHECK(inv.Move(items, chinese, 16, place::kShield, 0).status == ItemStatus::WrongSlot);
    // The bow takes the sword's place (the sword back where the bow was), the shield off, and its
    // arrows from the bag beside it.
    const auto bow = inv.Move(items, chinese, 15, place::kWeapon, 0);
    REQUIRE(bow.status == ItemStatus::Ok);
    CHECK(inv.At(place::kWeapon)->refId == 12);
    CHECK(inv.At(15)->refId == 10);
    CHECK(inv.At(13)->refId == 11); // the shield: the first free place
    REQUIRE(inv.At(place::kShield) != nullptr);
    CHECK(inv.At(place::kShield)->refId == 13); // the arrows came along
    CHECK(inv.At(16) == nullptr);
    CHECK(bow.touched.size() == 6);
    // The shield put on beside it: the bow to a free place of the bag, the
    // arrows into the shield's place.
    const auto shieldOn = inv.Move(items, chinese, 13, place::kShield, 0);
    CHECK(shieldOn.status == ItemStatus::Ok);
    REQUIRE(inv.At(place::kShield) != nullptr);
    CHECK(inv.At(place::kShield)->refId == 11);
    CHECK(inv.At(place::kWeapon) == nullptr);
    REQUIRE(inv.At(13) != nullptr);
    CHECK(inv.At(13)->refId == 13);
    int bowAt = -1;
    for (int p = place::kWorn; p < inv.size; ++p) {
        if (inv.At(std::uint8_t(p)) != nullptr && inv.At(std::uint8_t(p))->refId == 12) bowAt = p;
    }
    REQUIRE(bowAt >= place::kWorn);
    // Back as it was: the bow on (the shield pushed to a free place, the arrows along), the shield to 13 again.
    CHECK(inv.Move(items, chinese, std::uint8_t(bowAt), place::kWeapon, 0).status == ItemStatus::Ok);
    REQUIRE(inv.At(place::kShield) != nullptr);
    CHECK(inv.At(place::kShield)->refId == 13);
    for (int p = place::kWorn; p < inv.size; ++p) {
        if (inv.At(std::uint8_t(p)) != nullptr && inv.At(std::uint8_t(p))->refId == 11 && p != 13) {
            CHECK(inv.Move(items, chinese, std::uint8_t(p), 13, 0).status == ItemStatus::Ok);
            break;
        }
    }
    REQUIRE(inv.At(13) != nullptr);
    CHECK(inv.At(13)->refId == 11);
    // Off the body: into the bag's first free place whatever place was asked, never a swap (the original's detach; a swap
    // is the other way, a bag item put on).
    const int chestTo = inv.FreeBagPlace();
    REQUIRE(chestTo >= place::kWorn);
    CHECK(inv.Move(items, chinese, place::kChest, 20, 0).status == ItemStatus::Ok);
    REQUIRE(inv.At(std::uint8_t(chestTo)) != nullptr);
    CHECK(inv.At(std::uint8_t(chestTo))->refId == 1);
    // The bow off, asked onto the shield in 13: to the first free place, its arrows to the next; the shield stays.
    const int bowTo = inv.FreeBagPlace();
    const int arrowsTo = inv.FreeBagPlace(bowTo);
    const auto off = inv.Move(items, chinese, place::kWeapon, 13, 0);
    CHECK(off.status == ItemStatus::Ok);
    CHECK(off.touched.size() == 4);
    CHECK(inv.At(place::kWeapon) == nullptr);
    CHECK(inv.At(place::kShield) == nullptr);
    REQUIRE(inv.At(13) != nullptr);
    CHECK(inv.At(13)->refId == 11);
    REQUIRE(inv.At(std::uint8_t(bowTo)) != nullptr);
    CHECK(inv.At(std::uint8_t(bowTo))->refId == 12);
    REQUIRE(inv.At(std::uint8_t(arrowsTo)) != nullptr);
    CHECK(inv.At(std::uint8_t(arrowsTo))->refId == 13);
    // The bow on again: its arrows from the bag beside it. The sword put on in its stead (a bag item onto the worn place:
    // this way the two swap): the bow to the sword's place, its arrows to the bag (the sword shoots none).
    CHECK(inv.Move(items, chinese, std::uint8_t(bowTo), place::kWeapon, 0).status == ItemStatus::Ok);
    REQUIRE(inv.At(place::kShield) != nullptr);
    CHECK(inv.At(place::kShield)->refId == 13);
    CHECK(inv.Move(items, chinese, 15, place::kWeapon, 0).status == ItemStatus::Ok);
    REQUIRE(inv.At(place::kWeapon) != nullptr);
    CHECK(inv.At(place::kWeapon)->refId == 10);
    REQUIRE(inv.At(15) != nullptr);
    CHECK(inv.At(15)->refId == 12);
    CHECK(inv.At(place::kShield) == nullptr);

    // Requirements: the country, the sex, the level, not broken.
    inv.places[21] = Item(20);
    inv.places[22] = Item(30);
    inv.places[23] = Item(31);
    inv.places[24] = Item(1, 0);
    CHECK(inv.Move(items, chinese, 21, place::kWeapon, 0).status == ItemStatus::Race);
    CHECK(inv.Move(items, chinese, 22, place::kChest, 0).status == ItemStatus::Sex);
    CHECK(inv.Move(items, chinese, 23, place::kChest, 0).status == ItemStatus::Level);
    CHECK(inv.Move(items, chinese, 24, place::kChest, 0).status == ItemStatus::Broken);
    // Rings: either ring place.
    inv.places[25] = Item(40, 0);
    CHECK(inv.Move(items, chinese, 25, place::kRingRight, 0).status == ItemStatus::Ok);
    CHECK(inv.Move(items, chinese, place::kRingRight, place::kRingLeft, 0).status == ItemStatus::Ok);

    // Stacks: a split onto an empty place, a merge, a top-up; places beyond the size refused.
    inv.places[30] = Item(50, 0, 30);
    CHECK(inv.Move(items, chinese, 30, 31, 10).status == ItemStatus::Ok);
    CHECK(inv.At(30)->count == 20);
    CHECK(inv.At(31)->count == 10);
    CHECK(inv.At(31)->serial == 0); // a new item (the server gives it an id)
    inv.places[30].serial = 7;
    const auto merged = inv.Move(items, chinese, 30, 31, 0);
    CHECK(merged.status == ItemStatus::Ok);
    CHECK(inv.At(30) == nullptr);
    CHECK(inv.At(31)->count == 30);
    REQUIRE(merged.gone.size() == 1);
    CHECK(merged.gone[0] == 7);
    inv.places[32] = Item(50, 0, 40);
    CHECK(inv.Move(items, chinese, 32, 31, 0).status == ItemStatus::Ok); // 30 + 40 > 50: 50 and 20 left
    CHECK(inv.At(31)->count == 50);
    CHECK(inv.At(32)->count == 20);
    CHECK(inv.Move(items, chinese, 31, 45, 0).status == ItemStatus::BadSlot);
    CHECK(inv.Move(items, chinese, 33, 34, 0).status == ItemStatus::NoItem);
}

TEST_CASE("the bag full: a two-handed weapon cannot push the shield off") {
    const assets::ItemTable items = Items();
    game::Inventory inv;
    inv.size = 15; // two bag places
    inv.places[place::kWeapon] = Item(10);
    inv.places[place::kShield] = Item(11);
    inv.places[13] = Item(12);
    inv.places[14] = Item(1);
    CHECK(inv.Move(items, {10, 0, true}, 13, place::kWeapon, 0).status == ItemStatus::BagFull);
    CHECK(inv.At(place::kWeapon)->refId == 10); // nothing changed

    // A bow with its arrows in the full bag: the shield swaps into the arrows' place.
    game::Inventory bow;
    bow.size = 15;
    bow.places[place::kWeapon] = Item(10);
    bow.places[place::kShield] = Item(11);
    bow.places[13] = Item(12);
    bow.places[14] = Item(13, 50);
    const auto r = bow.Move(items, {10, 0, true}, 13, place::kWeapon, 0);
    CHECK(r.status == ItemStatus::Ok);
    CHECK(bow.At(place::kWeapon)->refId == 12);
    CHECK(bow.At(13)->refId == 10);
    CHECK(bow.At(14)->refId == 11);
    CHECK(bow.At(place::kShield)->refId == 13);
}

TEST_CASE("bows and crossbows name their ammunition's TID4 in column 87 (game files)") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipping");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    assets::ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    const assets::ItemRow* arrows = items.Find("ITEM_ETC_AMMO_ARROW_01_DEF");
    const assets::ItemRow* bolts = items.Find("ITEM_ETC_AMMO_BOLT_01_DEF");
    const assets::ItemRow* bow = items.Find("ITEM_CH_BOW_01_A_DEF");
    const assets::ItemRow* crossbow = items.Find("ITEM_EU_CROSSBOW_01_A_DEF");
    const assets::ItemRow* sword = items.Find("ITEM_CH_SWORD_01_A_DEF");
    REQUIRE(arrows != nullptr);
    REQUIRE(bolts != nullptr);
    REQUIRE(bow != nullptr);
    REQUIRE(crossbow != nullptr);
    REQUIRE(sword != nullptr);
    CHECK(game::IsAmmunition(*arrows));
    // The next stack of the same kind (the auto reload): the first in the bag, arrows not bolts, nothing worn.
    {
        game::Inventory bag;
        const auto put = [&](std::uint8_t place, const assets::ItemRow& row) {
            game::ItemInstance item;
            item.refId = row.refObjId;
            item.count = 10;
            bag.places[place] = item;
        };
        put(place::kShield, *arrows);
        CHECK(bag.FindSameType(items, *arrows) == -1);
        put(20, *bolts);
        CHECK(bag.FindSameType(items, *arrows) == -1);
        put(30, *arrows);
        put(25, *arrows);
        CHECK(bag.FindSameType(items, *arrows) == 25);
        CHECK(bag.FindSameType(items, *bolts) == 20);
    }
    CHECK(game::TwoHanded(*bow));
    CHECK(game::TwoHanded(*crossbow));
    CHECK_FALSE(game::TwoHanded(*sword));
    CHECK(game::AmmunitionFor(*bow) == arrows->typeId[3]);
    CHECK(game::AmmunitionFor(*crossbow) == bolts->typeId[3]);
    CHECK(game::AmmunitionFor(*sword) == 0);
    CHECK(game::MaxStack(*arrows) == 1000); // ours (the original's 250): game::MaxStack
    CHECK(game::NewDurability(*sword) > 0);
    CHECK(game::EquipPlaceFor(*sword, false, false) == place::kWeapon);
    // Every wearable item has a place; armour by its part.
    const assets::ItemRow* head = items.Find("ITEM_CH_M_HEAVY_01_HA_A_DEF");
    if (head) CHECK(game::EquipPlaceFor(*head, false, false) == place::kHead);
}

TEST_CASE("the item tooltip's lines and the gold as the inventory writes it") {
    CHECK(sr::client::GoldText(0) == "0");
    CHECK(sr::client::GoldText(1234567) == "1,234,567");
    CHECK(sr::client::GoldText(999) == "999");
    CHECK(sr::client::GoldColor(9'999) == ui::Argb(0xFFFFFFFF));
    CHECK(sr::client::GoldColor(150'000) == ui::Argb(0xFFFFD348));
    CHECK(sr::client::GoldColor(500'000'000'000ull) == ui::Argb(0xFF8BFFE5));

    auto sword = Row(10, "SWORD", 1, 6, 2, 1, 2, 30); // European, level 30
    sword.nameKey = "SN_SWORD";
    sword.itemClass = 1;
    sword.rarity = 2;
    SetStat(sword, 63, 60);
    SetStat(sword, 64, 70);
    SetStat(sword, 95, 10);
    SetStat(sword, 96, 20);
    SetStat(sword, 97, 20);
    SetStat(sword, 98, 30);
    sr::client::HudContext context;
    context.text = [](const std::string& key) {
        if (key == "SN_SWORD") return std::string("Copper Sword");
        if (key == "UIIT_STT_WEAPON_TYPE") return std::string("Weapon type");
        if (key == "PARAM_WEAPON_SWORD") return std::string("Sword");
        if (key == "PARAM_PA") return std::string("Phy. attack power");
        if (key == "PARAM_DUR") return std::string("Durability");
        if (key == "PARAM_REQ_LV") return std::string("Required level");
        if (key == "UIO_NEWCHAR_CTL_EUROPEAN") return std::string("European");
        if (key == "PARAM_RARE_FIRST") return std::string("Seal Of Star");
        return key;
    };
    context.wearer = {10, 0, true}; // a level-10 Chinese man
    ItemInstance item = Item(10, 0);
    item.plus = 2;
    const auto lines = sr::client::ItemTooltip(sword, item, context);
    const auto find = [&](const std::string& text) {
        for (const auto& l : lines) {
            if (l.text == text) return &l;
        }
        return static_cast<const ui::TooltipLine*>(nullptr);
    };
    REQUIRE(!lines.empty());
    CHECK(lines[0].text == "Copper Sword (+2)");
    CHECK(lines[0].color == ui::Argb(0xFFFFD953)); // rare: gold
    CHECK(find("Weapon type: Sword") != nullptr);
    CHECK(find("Phy. attack power 10 ~ 20 (+0%)") != nullptr);
    const auto* durability = find("Durability 0/60 (+0%)");
    REQUIRE(durability != nullptr);
    CHECK(durability->color == ui::Argb(0xFFFF4A4A)); // broken: red
    const auto* level = find("Required level 30");
    REQUIRE(level != nullptr);
    CHECK(level->color == ui::Argb(0xFFFF4A4A));
    const auto* race = find("European");
    REQUIRE(race != nullptr);
    CHECK(race->color == ui::Argb(0xFFFF4A4A));
    CHECK(find("Seal Of Star") != nullptr);
}

TEST_CASE("avatars: one of a kind in its place, the attachment over a dress that takes one, the dress takes it off") {
    assets::ItemTable items = Items();
    const auto avatar = [&](std::uint32_t id, int tid3, int tid4, int sex = 2, int childItems = 0) {
        assets::ItemRow r = Row(id, "", 1, tid3, tid4, 3, sex);
        r.codename = "AVATAR_" + std::to_string(id);
        r.childItems = childItems;
        items.Add(r);
    };
    avatar(60, 13, 1);       // a hat
    avatar(61, 13, 2, 2, 1); // a dress that takes an attachment
    avatar(62, 13, 3);       // an attachment
    avatar(63, 13, 4, 0);    // a flag for women
    avatar(64, 13, 2, 2, 0); // a dress that takes none
    avatar(65, 14, 1);       // a Devil's Spirit
    CHECK(game::AvatarPlaceFor(*items.Find(60u)) == game::avatar::kHat);
    CHECK(game::AvatarPlaceFor(*items.Find(63u)) == game::avatar::kFlag);
    CHECK(game::AvatarPlaceFor(*items.Find(65u)) == game::avatar::kSpirit);
    CHECK(game::AvatarPlaceFor(*items.Find(1u)) == -1);
    CHECK(game::KindOf(*items.Find(65u)) == game::ItemKind::Avatar);
    const game::Wearer man{10, 0, true};
    game::Inventory inv;
    inv.size = 17; // four bag places
    inv.places[13] = Item(62, 0);
    inv.places[14] = Item(61, 0);
    inv.places[15] = Item(63, 0);
    inv.places[16] = Item(1);
    CHECK(inv.WearAvatar(items, man, 13).status == ItemStatus::NeedsDress); // no dress yet
    CHECK(inv.WearAvatar(items, man, 15).status == ItemStatus::Sex);
    CHECK(inv.WearAvatar(items, man, 16).status == ItemStatus::WrongSlot); // armour is no avatar
    CHECK(inv.WearAvatar(items, man, place::kChest).status == ItemStatus::BadSlot);
    game::Inventory::AvatarResult r = inv.WearAvatar(items, man, 14);
    REQUIRE(r.status == ItemStatus::Ok);
    CHECK(r.bag == std::vector<std::uint8_t>{14});
    CHECK(r.avatars == std::vector<std::uint8_t>{game::avatar::kDress});
    CHECK(inv.At(14) == nullptr);
    CHECK(inv.Avatar(game::avatar::kDress)->refId == 61);
    REQUIRE(inv.WearAvatar(items, man, 13).status == ItemStatus::Ok);
    CHECK(inv.Avatar(game::avatar::kAttachment)->refId == 62);
    // A second dress: its kind is worn.
    inv.places[13] = Item(64, 0);
    CHECK(inv.WearAvatar(items, man, 13).status == ItemStatus::PlaceTaken);
    // The dress off takes the attachment: two free places needed (13 is taken, 14 free, 15 the flag, 16 the chest).
    CHECK(inv.TakeOffAvatar(items, game::avatar::kDress).status == ItemStatus::BagFull);
    CHECK(inv.Avatar(game::avatar::kDress) != nullptr);
    inv.places.erase(16);
    r = inv.TakeOffAvatar(items, game::avatar::kDress);
    REQUIRE(r.status == ItemStatus::Ok);
    CHECK(r.bag == std::vector<std::uint8_t>{14, 16});
    CHECK(r.avatars == std::vector<std::uint8_t>{game::avatar::kDress, game::avatar::kAttachment});
    CHECK(inv.At(14)->refId == 61);
    CHECK(inv.At(16)->refId == 62);
    CHECK(inv.avatars.empty());
    CHECK(inv.TakeOffAvatar(items, game::avatar::kDress).status == ItemStatus::NoItem);
    // A dress that takes none: no attachment over it (the original's 0x180E).
    REQUIRE(inv.WearAvatar(items, man, 13).status == ItemStatus::Ok);
    CHECK(inv.WearAvatar(items, man, 16).status == ItemStatus::WrongSlot);
}
