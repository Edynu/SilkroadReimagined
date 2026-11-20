#include "sr/assets/ItemGlow.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>

#include "sr/assets/FileSystem.h"
#include "sr/core/Text.h"

namespace sr::assets {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\r' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Each line's tab-separated columns, trimmed; blank lines and "//" comments skipped. `visit` gets the line's
// number and its columns, and returns false to stop.
template <typename Visit>
bool ForEachLine(std::string_view text, Visit&& visit) {
    std::size_t number = 0;
    std::vector<std::string_view> columns;
    for (std::size_t start = 0; start < text.size();) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = Trim(text.substr(start, end - start));
        start = end + 1;
        ++number;
        if (line.empty() || line.starts_with("//")) continue;
        columns.clear();
        for (std::size_t from = 0; from <= line.size();) {
            const std::size_t tab = std::min(line.find('\t', from), line.size());
            columns.push_back(Trim(line.substr(from, tab - from)));
            from = tab + 1;
        }
        if (!visit(number, columns)) return false;
    }
    return true;
}

bool ReadInt(std::string_view s, int& out) {
    return !s.empty() && std::from_chars(s.data(), s.data() + s.size(), out).ec == std::errc{};
}

// "a,b[,c]": `count` numbers separated by commas; false when fewer read.
template <std::size_t N>
bool ReadFloats(std::string_view s, std::array<float, N>& out) {
    const std::string text(s);
    const char* p = text.c_str();
    for (float& v : out) {
        char* next = nullptr;
        v = std::strtof(p, &next);
        if (next == p) return false;
        p = *next == ',' ? next + 1 : next;
    }
    return true;
}

} // namespace

bool ItemGlowTable::Load(const FileSystem& fs, std::string& error) {
    items_.clear();
    lines_ = 0;
    auto rare = fs.Read("resinfo\\itemrare.txt");
    if (!rare) {
        error = "resinfo\\itemrare.txt not found";
        return false;
    }
    if (!Add(core::DecodeTextFile(rare->Bytes()), "itemrare.txt", error)) return false;
    if (auto enchant = fs.Read("resinfo\\itemoptionefp.txt")) return Add(core::DecodeTextFile(enchant->Bytes()), "itemoptionefp.txt", error);
    return true;
}

bool ItemGlowTable::Decode(std::string_view text, std::string& error) {
    items_.clear();
    lines_ = 0;
    return Add(text, "itemrare.txt", error);
}

bool ItemGlowTable::Add(std::string_view text, std::string_view name, std::string& error) {
    return ForEachLine(text, [&](std::size_t number, const std::vector<std::string_view>& columns) {
        const std::string where = std::string(name) + " line " + std::to_string(number);
        if (columns.size() < 5) {
            error = where + ": " + std::to_string(columns.size()) + " columns";
            return false;
        }
        ItemGlowEffect e;
        e.effect = std::string(columns[1]);
        int percent = 0;
        if (!ReadInt(columns[2], percent)) {
            error = where + ": size " + std::string(columns[2]);
            return false;
        }
        e.scale = static_cast<float>(percent) / 100.0f;
        if (columns[3] != "none") e.bone = std::string(columns[3]);
        if (columns[4] != "none" && !ReadFloats(columns[4], e.offset)) {
            error = where + ": offset " + std::string(columns[4]);
            return false;
        }
        // The plus range (0 and 255 when the columns are absent).
        int low = 0, high = 255;
        if (columns.size() > 5 && !columns[5].empty() && !ReadInt(columns[5], low)) {
            error = where + ": lowest plus " + std::string(columns[5]);
            return false;
        }
        if (columns.size() > 6 && !columns[6].empty() && !ReadInt(columns[6], high)) {
            error = where + ": highest plus " + std::string(columns[6]);
            return false;
        }
        e.minPlus = static_cast<std::uint8_t>(std::clamp(low, 0, 255));
        e.maxPlus = static_cast<std::uint8_t>(std::clamp(high, 0, 255));
        items_[std::string(columns[0])].push_back(std::move(e));
        ++lines_;
        return true;
    });
}

const std::vector<ItemGlowEffect>* ItemGlowTable::Find(std::string_view codename) const {
    const auto it = items_.find(std::string(codename));
    return it == items_.end() ? nullptr : &it->second;
}

bool EquipGlowTable::Load(const FileSystem& fs, std::string& error) {
    types_.clear();
    rows_.clear();
    auto types = fs.Read("resinfo\\itemoption.txt");
    auto rows = fs.Read("resinfo\\itemtypenumber.txt");
    if (!types || !rows) {
        error = std::string(!types ? "resinfo\\itemoption.txt" : "resinfo\\itemtypenumber.txt") + " not found";
        return false;
    }
    return DecodeTypes(core::DecodeTextFile(types->Bytes()), error) && DecodeRows(core::DecodeTextFile(rows->Bytes()), error);
}

bool EquipGlowTable::DecodeTypes(std::string_view text, std::string& error) {
    types_.clear();
    return ForEachLine(text, [&](std::size_t number, const std::vector<std::string_view>& columns) {
        int type = 0;
        if (columns.size() < 2 || !ReadInt(columns[1], type)) {
            error = "itemoption.txt line " + std::to_string(number) + ": no type";
            return false;
        }
        types_[std::string(columns[0])] = type;
        return true;
    });
}

bool EquipGlowTable::DecodeRows(std::string_view text, std::string& error) {
    rows_.clear();
    return ForEachLine(text, [&](std::size_t number, const std::vector<std::string_view>& c) {
        const std::string where = "itemtypenumber.txt line " + std::to_string(number);
        if (c.size() < 10) {
            error = where + ": " + std::to_string(c.size()) + " columns";
            return false;
        }
        EquipGlow g;
        int minPlus = 0, colorOp = 0, addOp = 0, period = 0, alpha = 0;
        if (!ReadInt(c[0], g.type) || !ReadInt(c[1], minPlus) || !ReadFloats(c[2], g.scroll) || !ReadInt(c[4], colorOp) ||
            !ReadInt(c[5], addOp) || !ReadFloats(c[6], g.from) || !ReadFloats(c[7], g.to) || !ReadInt(c[8], period) || !ReadInt(c[9], alpha)) {
            error = where + ": a number does not read";
            return false;
        }
        g.minPlus = static_cast<std::uint8_t>(std::clamp(minPlus, 0, 255));
        g.texture = std::string(c[3]);
        g.colorOp = static_cast<std::uint8_t>(colorOp);
        g.addOp = static_cast<std::uint8_t>(addOp);
        g.periodMs = static_cast<std::uint32_t>(std::max(period, 0));
        g.alphaTest = alpha != 0;
        rows_.push_back(std::move(g));
        return true;
    });
}

const EquipGlow* EquipGlowTable::Find(std::string_view codename, int plus) const {
    const auto type = types_.find(std::string(codename));
    if (type == types_.end() || plus <= 0) return nullptr;
    const EquipGlow* best = nullptr;
    for (const EquipGlow& g : rows_) {
        if (g.type == type->second && g.minPlus <= plus && (best == nullptr || g.minPlus > best->minPlus)) best = &g;
    }
    return best;
}

} // namespace sr::assets
