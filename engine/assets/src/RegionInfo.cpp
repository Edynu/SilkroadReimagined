#include "sr/assets/RegionInfo.h"

#include <algorithm>
#include <charconv>

#include "sr/assets/FileSystem.h"
#include "sr/core/Text.h"

namespace sr::assets {

namespace {

constexpr std::string_view kSpace = " \t\r\n";

std::string_view Trim(std::string_view s) {
    const std::size_t a = s.find_first_not_of(kSpace);
    if (a == std::string_view::npos) return {};
    return s.substr(a, s.find_last_not_of(kSpace) - a + 1);
}

std::vector<std::string_view> Words(std::string_view line) {
    std::vector<std::string_view> out;
    while (true) {
        const std::size_t a = line.find_first_not_of(kSpace);
        if (a == std::string_view::npos) break;
        line.remove_prefix(a);
        const std::size_t end = line.find_first_of(kSpace);
        out.push_back(line.substr(0, end));
        if (end == std::string_view::npos) break;
        line.remove_prefix(end);
    }
    return out;
}

float Number(std::string_view s, float fallback) {
    s = Trim(s);
    float v = fallback;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc() ? v : fallback;
}

} // namespace

bool RegionAreaTable::Parse(std::string_view text, std::string& error) {
    areas_.clear();
    regions_.clear();
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = Trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (line.empty() || line.starts_with("//")) continue;
        const std::vector<std::string_view> w = Words(line);
        if (line.front() == '#') {
            Area area;
            area.town = w[0] == "#TOWN";
            if (w.size() > 1) area.name = std::string(w[1]);
            if (w.size() > 2) area.dungeon = std::string(w[2]);
            areas_.push_back(std::move(area));
            continue;
        }
        if (areas_.empty() || w.size() < 3) continue;
        const int x = int(Number(w[0], -1.0f)), y = int(Number(w[1], -1.0f));
        if (x < 0 || x > 255 || y < 0 || y > 255) continue;
        Part part;
        part.area = std::uint32_t(areas_.size() - 1);
        if (w[2] == "RECT") {
            if (w.size() < 7) continue;
            part.all = false;
            part.x0 = std::min(Number(w[3], 0.0f), Number(w[5], 0.0f));
            part.x1 = std::max(Number(w[3], 0.0f), Number(w[5], 0.0f));
            part.z0 = std::min(Number(w[4], 0.0f), Number(w[6], 0.0f));
            part.z1 = std::max(Number(w[4], 0.0f), Number(w[6], 0.0f));
        } else if (w[2] != "ALL") {
            continue;
        }
        regions_[std::uint16_t(x | y << 8)].push_back(part);
    }
    if (areas_.empty()) {
        error = "regioninfo.txt: no areas";
        return false;
    }
    return true;
}

bool RegionAreaTable::Load(const FileSystem& files, std::string& error) {
    const auto file = files.Read("server_dep\\silkroad\\textdata\\regioninfo.txt");
    if (!file) {
        error = "regioninfo.txt is missing";
        return false;
    }
    return Parse(core::DecodeTextFile(file->Bytes()), error);
}

const RegionAreaTable::Area* RegionAreaTable::Find(std::uint16_t region, float x, float z) const {
    const auto it = regions_.find(region);
    if (it == regions_.end()) return nullptr;
    const Part* all = nullptr;
    for (const Part& p : it->second) {
        if (p.all) {
            if (all == nullptr) all = &p;
        } else if (x >= p.x0 && x <= p.x1 && z >= p.z0 && z <= p.z1) {
            return &areas_[p.area];
        }
    }
    return all != nullptr ? &areas_[all->area] : nullptr;
}

int RegionAreaTable::TownAt(std::uint16_t region, float x, float z) const {
    const Area* a = Find(region, x, z);
    return a != nullptr && a->town ? int(a - areas_.data()) : -1;
}

} // namespace sr::assets
