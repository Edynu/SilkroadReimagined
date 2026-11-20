#pragma once

// The item lab (L12) on the server side: game::ItemCatalogue's entries as the rows of
// sr_lab_items, each with all its data as JSON (its names, requirements, prices and flags, its white stats, what using
// it does by its data, the path our server takes), for the site's /dev. The item exam comes later.

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "sr/assets/CollectionBook.h"
#include "sr/assets/GameData.h"
#include "sr/assets/UiLayout.h"

namespace sr::assets {
class FileSystem;
}

namespace sr::server {

class Database;

// What the list is made of: the item table and what its rows name (skills, characters, magic options, the socket
// stones' groups, the collection book), the client's texts.
struct ItemLabData {
    assets::ItemTable items;
    assets::SkillTable skills;
    assets::CharacterTable characters;
    assets::TextCatalog text;
    std::vector<assets::MagicOptionRow> options;
    std::vector<assets::MagicOptGroupRow> optionGroups;
    assets::CollectionBookData book;
    std::unordered_set<std::string> talismans; // the book's items' codenames
};
bool LoadItemLabData(const assets::FileSystem& files, ItemLabData& out, std::string& error);

// A row of sr_lab_items: the columns the site filters by, and `data` (JSON) with everything else.
struct LabItemRow {
    std::string code;
    std::uint32_t id = 0;
    std::string name;
    std::string typeIds;       // "3/1/6/2"
    std::string group, kind;   // game::CatalogueKindOf
    std::string route;         // game::ItemRouteName
    std::string state;         // game::ItemStateName: implemented, partly, unhandled, declined, carried (in `data` too)
    std::string source;        // game, mall, event, rental, quest, gm
    int race = 3;              // 0 China, 1 Europe, 3 both
    int sex = 2;               // 0 woman, 1 man, 2 both
    int level = 0;             // the level it asks (0 none)
    int degree = 0;
    int rarity = 0;            // col 15 as written
    int usable = 0;            // CanUse as written
    std::string icon;
    std::string data;
};
std::vector<LabItemRow> MakeLabItemRows(const ItemLabData& data);

// The list replaced in one transaction (the items no longer in it go); `build` names the build that made it.
bool SaveLabItems(Database& db, const std::vector<LabItemRow>& rows, const std::string& build, std::string& error);

} // namespace sr::server
