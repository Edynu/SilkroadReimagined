#pragma once

// The Forgotten World's talisman collections as the 1.188 client reads them:
// server_dep\silkroad\textdata\collectionbook_theme.txt (the service flag, the theme's id, its codename, its Korean
// name, its name and description keys, the slots it has) and collectionbook_item.txt (the service flag, the talisman's
// codename, its Korean name, its theme's codename, a flag, its slot, its story's key, its picture).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sr::assets {

class FileSystem;

struct CollectionThemeRow {
    std::uint32_t id = 0;
    std::string codename, nameKey, descKey;
    int complete = 8;
};
struct CollectionItemRow {
    std::string item;  // ITEM_TALISMAN_*
    std::string theme; // its theme's codename
    int slot = 0;      // 1..
    std::string storyKey;
    std::string picture; // icon\item\etc\talisman_*.ddj
};

class CollectionBookData {
public:
    bool Load(const FileSystem& fs, std::string& error);
    bool Decode(std::string_view themes, std::string_view items, std::string& error);
    const std::vector<CollectionThemeRow>& Themes() const { return themes_; }
    const CollectionThemeRow* Theme(std::uint32_t id) const;
    const CollectionThemeRow* Theme(std::string_view codename) const;
    // A theme's talismans by slot (null where none).
    const CollectionItemRow* Item(std::string_view theme, int slot) const;
    const CollectionItemRow* Item(std::string_view itemCodename) const;

private:
    std::vector<CollectionThemeRow> themes_;
    std::vector<CollectionItemRow> items_;
};

} // namespace sr::assets
