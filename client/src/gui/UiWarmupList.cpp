#include "gui/UiWarmupList.h"

#include <algorithm>
#include <charconv>

#include "sr/core/Path.h"

namespace sr::client {

void UiWarmupList::Decode(std::string_view text) {
    entries_.clear();
    drawn_ = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const std::size_t tab = line.find('\t');
        if (tab == std::string_view::npos || tab + 1 >= line.size()) continue;
        int age = 0;
        const auto [at, error] = std::from_chars(line.data(), line.data() + tab, age);
        if (error != std::errc() || at != line.data() + tab || age < 0) continue;
        const std::string path = core::NormalizeAssetPath(line.substr(tab + 1));
        if (path.empty()) continue;
        const auto [it, inserted] = entries_.try_emplace(path, Entry{age, false});
        if (!inserted) it->second.age = std::min(it->second.age, age);
    }
}

std::vector<std::string> UiWarmupList::Paths() const {
    std::vector<std::pair<int, const std::string*>> order;
    order.reserve(entries_.size());
    for (const auto& [path, e] : entries_) order.emplace_back(e.age, &path);
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first < b.first : *a.second < *b.second; });
    std::vector<std::string> out;
    out.reserve(order.size());
    for (const auto& [age, path] : order) out.push_back(*path);
    return out;
}

void UiWarmupList::Note(std::string_view path) {
    if (path.empty()) return;
    Entry& e = entries_[core::NormalizeAssetPath(path)];
    if (!e.drawn) ++drawn_;
    e.drawn = true;
    e.age = 0;
}

std::string UiWarmupList::Encode() const {
    std::vector<std::pair<int, const std::string*>> kept;
    kept.reserve(entries_.size());
    for (const auto& [path, e] : entries_) {
        const int age = e.drawn ? 0 : e.age + 1;
        if (age <= kForgetAfter) kept.emplace_back(age, &path);
    }
    std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first < b.first : *a.second < *b.second; });
    if (kept.size() > kMaxEntries) kept.resize(kMaxEntries);
    std::string out;
    out.reserve(kept.size() * 48);
    for (const auto& [age, path] : kept) {
        out += std::to_string(age);
        out += '\t';
        out += *path;
        out += '\n';
    }
    return out;
}

} // namespace sr::client
