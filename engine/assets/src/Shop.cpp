#include "sr/assets/Shop.h"

#include <algorithm>
#include <cstdlib>

#include "sr/assets/FileSystem.h"
#include "sr/core/Path.h"
#include "sr/core/Text.h"

namespace sr::assets {

namespace {
constexpr const char* kTextData = "server_dep\\silkroad\\textdata\\";

using Row = std::vector<std::string>;

// Every in-service row of every shard whose file name is `name` or `name` + a suffix.
std::vector<Row> ReadTable(const FileSystem& fs, std::string_view name) {
    std::vector<Row> rows;
    for (const std::string& path : fs.ListFolder(kTextData)) {
        const std::string key = core::NormalizeAssetPath(path);
        const std::string_view file = std::string_view(key).substr(key.rfind('\\') + 1);
        if (!key.starts_with(kTextData) || !file.starts_with(name) || !key.ends_with(".txt")) continue;
        // "refshoptab" must not take "refshoptabgroup": after the name only digits or '_' may follow.
        const std::string_view rest = file.substr(name.size(), file.size() - name.size() - 4);
        if (!rest.empty() && rest.find_first_not_of("0123456789_") != std::string_view::npos) continue;
        auto data = fs.Read(path);
        if (!data) continue;
        const std::string text = core::DecodeTextFile(data->Bytes());
        std::size_t start = 0;
        while (start < text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string_view line(text.data() + start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (!line.starts_with("1\t")) continue;
            Row row;
            std::size_t at = 0;
            while (at <= line.size()) {
                std::size_t tab = line.find('\t', at);
                if (tab == std::string_view::npos) tab = line.size();
                row.emplace_back(line.substr(at, tab - at));
                at = tab + 1;
            }
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

const std::string& Col(const Row& row, std::size_t i) {
    static const std::string empty;
    return i < row.size() ? row[i] : empty;
}
int Int(const std::string& s) { return static_cast<int>(std::strtol(s.c_str(), nullptr, 10)); }

// Goods the original's database lists but keeps out of service, on sale here (ours, the user's call 2026-11-13). The
// 1.188 client's tables lack their rows, so the client and the server both add them here. Each row is the database's:
// _refshopgoods (tab, slot), _refscrapofpackageitem (the item), _refpricepolicyofitem (gold), all service 0.
struct ExtraGood {
    const char* tab;
    int slot;
    const char* package;
    const char* item;
    std::uint64_t price;
};
// The wolf's flute in every stable's Pet tab (SN_TAB_PET, empty in 1.188).
constexpr ExtraGood kExtraGoods[] = {
    {"STORE_CH_STABLE_TAB3", 0, "PACKAGE_ITEM_COS_P_FLUTE", "ITEM_COS_P_FLUTE", 2000000},
    {"STORE_WC_STABLE_TAB3", 0, "PACKAGE_ITEM_COS_P_FLUTE", "ITEM_COS_P_FLUTE", 2000000},
    {"STORE_KT_STABLE_TAB3", 0, "PACKAGE_ITEM_COS_P_FLUTE", "ITEM_COS_P_FLUTE", 2000000},
    {"STORE_EU_STABLE_TAB3", 0, "PACKAGE_ITEM_COS_P_FLUTE", "ITEM_COS_P_FLUTE", 2000000},
    {"STORE_CA_STABLE_TAB3", 0, "PACKAGE_ITEM_COS_P_FLUTE", "ITEM_COS_P_FLUTE", 2000000},
};
} // namespace

bool ShopTable::Load(const FileSystem& fs, std::string& error) {
    shops_.clear();
    const auto groups = ReadTable(fs, "refshopgroup");
    const auto groupStores = ReadTable(fs, "refmappingshopgroup");
    const auto storeTabGroups = ReadTable(fs, "refmappingshopwithtab");
    const auto tabGroups = ReadTable(fs, "refshoptabgroup");
    const auto tabs = ReadTable(fs, "refshoptab");
    const auto goods = ReadTable(fs, "refshopgoods");
    const auto prices = ReadTable(fs, "refpricepolicyofitem");
    const auto scraps = ReadTable(fs, "refscrapofpackageitem");
    if (groups.empty() || tabs.empty() || goods.empty()) {
        error = "shop tables (refshopgroup / refshoptab / refshopgoods) not found";
        return false;
    }
    // Packages: the item they hold and their gold price.
    std::unordered_map<std::string, ShopGood> packages;
    for (const Row& r : scraps) {
        ShopGood& g = packages[Col(r, 2)];
        g.package = Col(r, 2);
        g.itemCodename = Col(r, 3);
        g.plus = Int(Col(r, 4));
        g.variance = std::strtoull(Col(r, 5).c_str(), nullptr, 10);
        g.data = Int(Col(r, 6));
    }
    // refpricepolicyofitem: service, country, package, payment device, previous cost, cost, params.
    for (const Row& r : prices) {
        const auto it = packages.find(Col(r, 2));
        if (it == packages.end()) continue;
        const int payment = Int(Col(r, 3));
        const auto cost = std::uint32_t(std::strtoul(Col(r, 5).c_str(), nullptr, 10));
        if (payment == 8) it->second.guildPoints = cost;
        if (payment == 32) it->second.honorPoints = cost;
        for (int k = 0; k < 5; ++k) {
            if (payment == (64 << k)) it->second.tokens[std::size_t(k)] = cost;
        }
        // Gold (1) wins; another currency only when there is no gold price.
        if (payment == 1 || it->second.price == 0) {
            it->second.paymentType = payment;
            it->second.price = std::strtoull(Col(r, 5).c_str(), nullptr, 10);
        }
    }
    std::unordered_map<std::string, std::vector<ShopGood>> goodsByTab;
    for (const Row& r : goods) {
        const auto it = packages.find(Col(r, 3));
        if (it == packages.end() || it->second.itemCodename.empty()) continue;
        ShopGood g = it->second;
        g.slot = Int(Col(r, 4));
        auto& list = goodsByTab[Col(r, 2)];
        if (std::none_of(list.begin(), list.end(), [&](const ShopGood& o) { return o.slot == g.slot; })) list.push_back(std::move(g));
    }
    // Ours on top (kExtraGoods), unless the tables already have that slot or that package there.
    for (const ExtraGood& x : kExtraGoods) {
        auto& list = goodsByTab[x.tab];
        if (std::any_of(list.begin(), list.end(), [&](const ShopGood& o) { return o.slot == x.slot || o.package == x.package; })) continue;
        ShopGood g;
        g.slot = x.slot;
        g.package = x.package;
        g.itemCodename = x.item;
        g.paymentType = 1;
        g.price = x.price;
        list.push_back(std::move(g));
    }
    for (auto& [code, list] : goodsByTab) {
        std::sort(list.begin(), list.end(), [](const ShopGood& a, const ShopGood& b) { return a.slot < b.slot; });
    }
    std::unordered_map<std::string, std::string> tabGroupLabels;
    for (const Row& r : tabGroups) tabGroupLabels[Col(r, 3)] = Col(r, 4);

    for (const Row& group : groups) {
        NpcShop& shop = shops_[Col(group, 4)];
        int index = 0;
        for (const ShopTabGroup& existing : shop.groups) index += static_cast<int>(existing.tabs.size());
        for (const Row& gs : groupStores) {
            if (Col(gs, 2) != Col(group, 3)) continue;
            for (const Row& st : storeTabGroups) {
                if (Col(st, 2) != Col(gs, 3)) continue;
                ShopTabGroup tg;
                tg.code = Col(st, 3);
                if (const auto label = tabGroupLabels.find(tg.code); label != tabGroupLabels.end()) tg.labelKey = label->second;
                for (const Row& t : tabs) {
                    if (Col(t, 4) != tg.code) continue;
                    ShopTab tab;
                    tab.code = Col(t, 3);
                    tab.labelKey = Col(t, 5);
                    tab.index = index++;
                    if (const auto it = goodsByTab.find(tab.code); it != goodsByTab.end()) tab.goods = it->second;
                    tg.tabs.push_back(std::move(tab));
                }
                if (!tg.tabs.empty()) shop.groups.push_back(std::move(tg));
            }
        }
    }
    std::erase_if(shops_, [](const auto& entry) { return entry.second.groups.empty(); });
    if (shops_.empty()) {
        error = "no NPC shops";
        return false;
    }
    return true;
}

const NpcShop* ShopTable::Find(std::string_view npcCodename) const {
    const auto it = shops_.find(std::string(npcCodename));
    return it == shops_.end() ? nullptr : &it->second;
}

bool MallTable::Load(const FileSystem& fs, std::string& error, std::string_view group) {
    shops_.clear();
    menu_.clear();
    hot_.clear();
    const auto groupStores = ReadTable(fs, "refmappingshopgroup");
    const auto storeTabGroups = ReadTable(fs, "refmappingshopwithtab");
    const auto tabs = ReadTable(fs, "refshoptab");
    const auto goods = ReadTable(fs, "refshopgoods");
    const auto packages = ReadTable(fs, "refpackageitem");
    const auto prices = ReadTable(fs, "refpricepolicyofitem");
    const auto scraps = ReadTable(fs, "refscrapofpackageitem");
    std::unordered_map<std::string, MallGood> byPackage;
    for (const Row& r : packages) {
        MallGood& g = byPackage[Col(r, 3)];
        g.package = Col(r, 3);
        g.packageId = std::uint32_t(std::strtoul(Col(r, 2).c_str(), nullptr, 10));
        g.saleTag = std::uint16_t(Int(Col(r, 4)));
        g.nameKey = Col(r, 6);
        g.descKey = Col(r, 7);
        g.icon = Col(r, 8);
    }
    for (const Row& r : scraps) {
        const auto it = byPackage.find(Col(r, 2));
        if (it == byPackage.end() || !it->second.itemCodename.empty()) continue; // one item each in the mall
        it->second.itemCodename = Col(r, 3);
        it->second.plus = Int(Col(r, 4));
        it->second.variance = std::strtoull(Col(r, 5).c_str(), nullptr, 10);
        it->second.data = Int(Col(r, 6));
    }
    for (const Row& r : prices) {
        if (Int(Col(r, 3)) != 2) continue; // silk
        const auto it = byPackage.find(Col(r, 2));
        if (it == byPackage.end()) continue;
        it->second.previousPrice = std::uint32_t(std::strtoul(Col(r, 4).c_str(), nullptr, 10));
        it->second.price = std::uint32_t(std::strtoul(Col(r, 5).c_str(), nullptr, 10));
    }
    for (const char* code : kFreeMallPackages) {
        if (const auto it = byPackage.find(code); it != byPackage.end() && it->second.price != 0) {
            it->second.previousPrice = it->second.price; // shown struck through: was N silk
            it->second.price = 0;
            it->second.free = true;
        }
    }
    std::unordered_map<std::string, std::vector<MallGood>> goodsByTab;
    for (const Row& r : goods) {
        const auto it = byPackage.find(Col(r, 3));
        if (it == byPackage.end() || it->second.itemCodename.empty() || (it->second.price == 0 && !it->second.free)) continue;
        MallGood g = it->second;
        g.slot = Int(Col(r, 4));
        auto& list = goodsByTab[Col(r, 2)];
        if (std::none_of(list.begin(), list.end(), [&](const MallGood& o) { return o.slot == g.slot; })) list.push_back(std::move(g));
    }
    for (auto& [code, list] : goodsByTab) {
        std::sort(list.begin(), list.end(), [](const MallGood& a, const MallGood& b) { return a.slot < b.slot; });
    }
    for (const Row& gs : groupStores) {
        if (Col(gs, 2) != group) continue;
        MallShop shop;
        shop.code = Col(gs, 3);
        for (const Row& st : storeTabGroups) {
            if (Col(st, 2) != shop.code) continue;
            std::vector<const Row*> its;
            for (const Row& t : tabs) {
                if (Col(t, 4) == Col(st, 3)) its.push_back(&t);
            }
            std::sort(its.begin(), its.end(), [](const Row* a, const Row* b) { return Int(Col(*a, 2)) < Int(Col(*b, 2)); });
            for (const Row* t : its) {
                MallTab tab;
                tab.id = std::uint32_t(Int(Col(*t, 2)));
                tab.code = Col(*t, 3);
                tab.labelKey = Col(*t, 5);
                if (const auto it = goodsByTab.find(tab.code); it != goodsByTab.end()) tab.goods = it->second;
                if (!tab.goods.empty()) shop.tabs.push_back(std::move(tab));
            }
        }
        if (!shop.tabs.empty()) shops_.push_back(std::move(shop));
    }
    // The menu (the client's file; the server has no need of it).
    if (auto data = fs.Read(std::string(kTextData) + "mallitemmenulistdata.txt")) {
        const std::string text = core::DecodeTextFile(data->Bytes());
        std::string section;
        std::size_t start = 0;
        while (start < text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string_view line(text.data() + start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty()) continue;
            Row row;
            std::size_t at = 0;
            while (at <= line.size()) {
                std::size_t tab = line.find('\t', at);
                if (tab == std::string_view::npos) tab = line.size();
                row.emplace_back(line.substr(at, tab - at));
                at = tab + 1;
            }
            if (Col(row, 0) == "#section") {
                section = Col(row, 1);
                continue;
            }
            if (section == "ITEMMALL_MENU_LIST" && row.size() >= 6) {
                MallMenuRow m;
                m.service = Col(row, 0) == "1";
                m.order = Int(Col(row, 1));
                m.shop = Col(row, 2) == "xxx" ? std::string() : Col(row, 2);
                m.nameKey = Col(row, 3);
                m.tabArt = Col(row, 4);
                m.icon = Col(row, 5);
                menu_.push_back(std::move(m));
            } else if (section == "ITEMMALL_HOTITEM_LIST" && row.size() >= 5) {
                hot_.push_back({Col(row, 0) == "1", Int(Col(row, 2)), Int(Col(row, 3)), Col(row, 4)});
            }
        }
        std::sort(menu_.begin(), menu_.end(), [](const MallMenuRow& a, const MallMenuRow& b) { return a.order < b.order; });
        std::sort(hot_.begin(), hot_.end(), [](const MallHotRow& a, const MallHotRow& b) { return a.order < b.order; });
    }
    if (shops_.empty()) {
        error = std::string("no shops in ") + std::string(group);
        return false;
    }
    return true;
}

const MallShop* MallTable::Shop(std::string_view code) const {
    for (const MallShop& s : shops_) {
        if (s.code == code) return &s;
    }
    return nullptr;
}

const MallTab* MallTable::Tab(std::uint32_t id) const {
    for (const MallShop& s : shops_) {
        for (const MallTab& t : s.tabs) {
            if (t.id == id) return &t;
        }
    }
    return nullptr;
}

const MallGood* MallTable::Find(std::uint32_t tab, int slot) const {
    const MallTab* t = Tab(tab);
    if (t == nullptr) return nullptr;
    for (const MallGood& g : t->goods) {
        if (g.slot == slot) return &g;
    }
    return nullptr;
}

const MallGood* MallTable::FindPackage(std::uint32_t package, const MallTab** tab) const {
    for (const MallShop& s : shops_) {
        for (const MallTab& t : s.tabs) {
            for (const MallGood& g : t.goods) {
                if (g.packageId != package) continue;
                if (tab != nullptr) *tab = &t;
                return &g;
            }
        }
    }
    return nullptr;
}

std::size_t MallTable::Goods() const {
    std::size_t n = 0;
    for (const MallShop& s : shops_) {
        for (const MallTab& t : s.tabs) n += t.goods.size();
    }
    return n;
}

} // namespace sr::assets
