#include "sr/assets/CollectionBook.h"

#include <cstdlib>

#include "sr/assets/FileSystem.h"
#include "sr/core/Text.h"

namespace sr::assets {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\r' || s.front() == '\t' || s.front() == '\xEF' || s.front() == '\xBB' || s.front() == '\xBF')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::vector<std::vector<std::string_view>> Rows(std::string_view text) {
    std::vector<std::vector<std::string_view>> out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (Trim(line).empty() || Trim(line).starts_with("//")) continue;
        std::vector<std::string_view> cells;
        std::size_t at = 0;
        for (;;) {
            const std::size_t tab = line.find('\t', at);
            cells.push_back(Trim(line.substr(at, tab == std::string_view::npos ? std::string_view::npos : tab - at)));
            if (tab == std::string_view::npos) break;
            at = tab + 1;
        }
        if (!cells.empty() && cells[0] == "1") out.push_back(std::move(cells));
    }
    return out;
}

} // namespace

bool CollectionBookData::Load(const FileSystem& fs, std::string& error) {
    const auto themes = fs.Read("server_dep\\silkroad\\textdata\\collectionbook_theme.txt");
    const auto items = fs.Read("server_dep\\silkroad\\textdata\\collectionbook_item.txt");
    if (!themes || !items) {
        error = "collectionbook_theme.txt or collectionbook_item.txt not found";
        return false;
    }
    return Decode(core::DecodeTextFile(themes->Bytes()), core::DecodeTextFile(items->Bytes()), error);
}

bool CollectionBookData::Decode(std::string_view themes, std::string_view items, std::string& error) {
    themes_.clear();
    items_.clear();
    for (const auto& c : Rows(themes)) {
        if (c.size() < 7) continue;
        themes_.push_back({std::uint32_t(std::strtoul(std::string(c[1]).c_str(), nullptr, 10)), std::string(c[2]), std::string(c[4]), std::string(c[5]),
                           std::atoi(std::string(c[6]).c_str())});
    }
    for (const auto& c : Rows(items)) {
        if (c.size() < 8) continue;
        items_.push_back({std::string(c[1]), std::string(c[3]), std::atoi(std::string(c[5]).c_str()), std::string(c[6]), std::string(c[7])});
    }
    if (themes_.empty()) error = "no collection themes";
    return !themes_.empty();
}

const CollectionThemeRow* CollectionBookData::Theme(std::uint32_t id) const {
    for (const auto& t : themes_) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

const CollectionThemeRow* CollectionBookData::Theme(std::string_view codename) const {
    for (const auto& t : themes_) {
        if (t.codename == codename) return &t;
    }
    return nullptr;
}

const CollectionItemRow* CollectionBookData::Item(std::string_view theme, int slot) const {
    for (const auto& i : items_) {
        if (i.theme == theme && i.slot == slot) return &i;
    }
    return nullptr;
}

const CollectionItemRow* CollectionBookData::Item(std::string_view itemCodename) const {
    for (const auto& i : items_) {
        if (i.item == itemCodename) return &i;
    }
    return nullptr;
}

} // namespace sr::assets
