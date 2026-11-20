#include "sr/assets/Gacha.h"

#include <cstdlib>

#include "sr/assets/FileSystem.h"
#include "sr/core/Text.h"

namespace sr::assets {

namespace {

constexpr const char* kTextData = "server_dep\\silkroad\\textdata\\";

// Each in-service row's cells.
template <typename F>
void ForEachRow(std::string_view text, F&& f) {
    std::vector<std::string_view> cells;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.starts_with("1\t")) continue;
        cells.clear();
        std::size_t at = 0;
        while (at <= line.size()) {
            std::size_t tab = line.find('\t', at);
            if (tab == std::string_view::npos) tab = line.size();
            cells.push_back(line.substr(at, tab - at));
            at = tab + 1;
        }
        f(cells);
    }
}

int Int(std::string_view s) { return static_cast<int>(std::strtol(std::string(s).c_str(), nullptr, 10)); }

std::string ReadText(const FileSystem& fs, const char* name) {
    auto data = fs.Read(std::string(kTextData) + name);
    return data ? core::DecodeTextFile(data->Bytes()) : std::string();
}

} // namespace

bool GachaTable::ParseEntries(std::string_view text, std::string& error) {
    entries_.clear();
    ForEachRow(text, [&](const std::vector<std::string_view>& c) {
        if (c.size() < 9) return;
        GachaEntry e;
        e.set = Int(c[1]);
        e.item = std::uint32_t(Int(c[2]));
        e.ratio = Int(c[3]);
        e.count = Int(c[4]);
        e.id = std::uint32_t(Int(c[5]));
        e.visible = Int(c[6]) != 0;
        if (Int(c[7]) != 0 && c[8] != "xxx") e.options = std::string(c[8]);
        if (e.item != 0 && e.id != 0) entries_.push_back(std::move(e));
    });
    if (entries_.empty()) {
        error = "no Magic POP entries";
        return false;
    }
    return true;
}

bool GachaTable::ParseMachines(std::string_view text, std::string& error) {
    machines_.clear();
    ForEachRow(text, [&](const std::vector<std::string_view>& c) {
        if (c.size() < 4) return;
        machines_.push_back({std::uint32_t(Int(c[1])), Int(c[2]), Int(c[3])});
    });
    if (machines_.empty()) {
        error = "no Magic POP machines";
        return false;
    }
    return true;
}

void GachaTable::ParseTree(std::string_view text) {
    tree_.clear();
    ForEachRow(text, [&](const std::vector<std::string_view>& c) {
        if (c.size() < 10) return;
        GachaTreeRow r;
        for (int k = 0; k < 4; ++k) r.typeId[std::size_t(k)] = Int(c[std::size_t(1 + k)]);
        r.bigId = Int(c[5]);
        r.bigText = std::string(c[6]);
        r.midId = Int(c[7]);
        r.midText = std::string(c[8]);
        r.maxDegree = Int(c[9]);
        tree_.push_back(std::move(r));
    });
}

bool GachaTable::Load(const FileSystem& fs, std::string& error, bool tree) {
    if (!ParseEntries(ReadText(fs, "gachaitemset.txt"), error) || !ParseMachines(ReadText(fs, "gachanpcmap.txt"), error)) return false;
    if (tree) ParseTree(ReadText(fs, "refgachatreeforclientuidata.txt"));
    return true;
}

const GachaEntry* GachaTable::Find(std::uint32_t gachaId) const {
    for (const GachaEntry& e : entries_) {
        if (e.id == gachaId) return &e;
    }
    return nullptr;
}

const GachaMachine* GachaTable::Machine(std::uint32_t npc) const {
    for (const GachaMachine& m : machines_) {
        if (m.npc == npc) return &m;
    }
    return nullptr;
}

} // namespace sr::assets
