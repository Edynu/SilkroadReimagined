#pragma once

// NPC shops from the server_dep textdata tables (only rows whose column 0 is 1 count):
//   refshopgroup          2 id, 3 group code, 4 NPC codename
//   refmappingshopgroup   2 group code, 3 store code
//   refmappingshopwithtab 2 store code, 3 tab group code
//   refshoptabgroup       2 id, 3 code, 4 label key (the NPC menu row)
//   refshoptab            2 id, 3 tab code, 4 tab group code, 5 label key
//   refshopgoods          2 tab code, 3 package code, 4 slot
//   refpackageitem        3 package code, 6 name key, 7 description key, 8 icon
//   refpricepolicyofitem  2 package code, 3 payment type (1 gold), 4 price
//   refscrapofpackageitem 2 package code, 3 item codename, 4 plus, 5 variance, 6 data (durability / stack)
// The tab byte a purchase sends counts the NPC's tabs in that order (groups, stores,
// tab groups, tabs). A few goods out of service in the original are added on top (Shop.cpp
// kExtraGoods: the wolf's flute at the stables).

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sr::assets {

class FileSystem;

struct ShopGood {
    int slot = 0;
    std::string package;
    std::string itemCodename;
    int plus = 0;
    std::uint64_t variance = 0;
    int data = 0; // durability or the stack's count
    int paymentType = 1;
    std::uint32_t guildPoints = 0; // a guild's good: its price in guild points, paid with the gold (payment device 8)
    std::uint64_t price = 0;
    // Its other prices, each paid with the gold (takes every one the package
    // has): honour points (device 32, the academy's) and tokens from the bag (devices 64, 128, 256, 512: the Egypt
    // tokens ITEM_ETC_SD_TOKEN_01..04; 1024: Arena Coins, ITEM_ETC_ARENA_COIN).
    std::uint32_t honorPoints = 0;
    std::array<std::uint32_t, 5> tokens{};
};
// The token a payment device's place in ShopGood::tokens takes (its codename).
inline constexpr std::array<const char*, 5> kShopTokens = {"ITEM_ETC_SD_TOKEN_01", "ITEM_ETC_SD_TOKEN_02", "ITEM_ETC_SD_TOKEN_03",
                                                           "ITEM_ETC_SD_TOKEN_04", "ITEM_ETC_ARENA_COIN"};

struct ShopTab {
    std::string code;
    std::string labelKey;
    int index = 0; // the tab byte a purchase sends
    std::vector<ShopGood> goods; // by slot
};

struct ShopTabGroup {
    std::string code;
    std::string labelKey;
    std::vector<ShopTab> tabs;
};

struct NpcShop {
    std::vector<ShopTabGroup> groups;
};

class ShopTable {
public:
    bool Load(const FileSystem& fs, std::string& error);
    const NpcShop* Find(std::string_view npcCodename) const;
    // Tests: a shop of their own.
    void Add(std::string npcCodename, NpcShop shop) { shops_[std::move(npcCodename)] = std::move(shop); }
    std::size_t size() const { return shops_.size(); }
    const std::unordered_map<std::string, NpcShop>& shops() const { return shops_; }

private:
    std::unordered_map<std::string, NpcShop> shops_;
};

// The item mall's catalogue (ITEM_MALL_PLAN 1.2): the shop group GROUP_MALL's shops
// (refmappingshopgroup, in the table's order), each its tab groups' tabs (refshoptab, by id) and their goods
// (refshopgoods by slot -> refpackageitem: 2 id, 3 code, 4 SaleTag, 6 name, 7 description, 8 icon -> its silk price,
// payment device 2 in refpricepolicyofitem: 4 the previous cost, 5 the cost -> refscrapofpackageitem's item). Only
// rows in service (column 0 is 1). The client also reads the menu (textdata mallitemmenulistdata.txt: section
// ITEMMALL_MENU_LIST: 0 service, 1 order, 2 shop code ("xxx": none), 3 name key, 4 tab art, 5 icon; section
// ITEMMALL_HOTITEM_LIST: 0 service, 1 a Korean name, 2 order, 3 the SaleTag bit, 4 the name key).
// Ours: the mall's packages our shard gives away (the user's call 2026-11-18: the Open Market's ticket, the "Tiger
// Tablet of the Sales King", free). Their price shows and costs 0.
inline constexpr const char* kFreeMallPackages[] = {"PACKAGE_ITEM_MALL_OPEN_MARKET_TICKET"};

struct MallGood {
    int slot = 0;
    std::uint32_t packageId = 0;
    std::string package;
    std::string nameKey, descKey, icon;
    std::uint16_t saleTag = 0;    // bits: 1 new, 2 limited, 4 sale, 8 popular, 16 stock, 32 a period
    std::uint32_t price = 0;      // silk (payment device 2)
    std::uint32_t previousPrice = 0;
    bool free = false;            // ours: given away on our shard (kFreeMallPackages), its price 0
    std::string itemCodename;
    int plus = 0;
    std::uint64_t variance = 0;
    int data = 0; // the stack bought (0: one, or the count chosen)
};
struct MallTab {
    std::uint32_t id = 0; // refshoptab's id: what a purchase names
    std::string code, labelKey;
    std::vector<MallGood> goods; // by slot
};
struct MallShop {
    std::string code;
    std::vector<MallTab> tabs;
};
struct MallMenuRow {
    bool service = true;
    int order = 0;
    std::string shop; // the shop's code; empty: the hot store or the basket
    std::string nameKey, tabArt, icon;
};
struct MallHotRow {
    bool service = true;
    int order = 0;
    int tag = 0;
    std::string nameKey;
};

class MallTable {
public:
    static constexpr const char* kGroup = "GROUP_MALL";
    // The shops of `group` (the mall's by default); the menu when the game files have it.
    bool Load(const FileSystem& fs, std::string& error, std::string_view group = kGroup);
    const std::vector<MallShop>& shops() const { return shops_; }
    const std::vector<MallMenuRow>& menu() const { return menu_; }
    const std::vector<MallHotRow>& hot() const { return hot_; }
    const MallShop* Shop(std::string_view code) const;
    const MallTab* Tab(std::uint32_t id) const;
    const MallGood* Find(std::uint32_t tab, int slot) const;
    // The first good of a package (and its tab).
    const MallGood* FindPackage(std::uint32_t package, const MallTab** tab = nullptr) const;
    std::size_t Goods() const;
    // Tests: a shop of their own.
    void Add(MallShop shop) { shops_.push_back(std::move(shop)); }

private:
    std::vector<MallShop> shops_;
    std::vector<MallMenuRow> menu_;
    std::vector<MallHotRow> hot_;
};

} // namespace sr::assets
