#include "sr/assets/GameGuide.h"

#include <cstdlib>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Text.h"

namespace sr::assets {

namespace {

// Each non-empty line's tab-separated columns (a "//" line is a remark).
template <typename Fn> void ForEachRow(std::string_view text, Fn&& fn) {
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty() || line.starts_with("//")) continue;
        std::vector<std::string_view> c;
        for (std::size_t pos = 0;;) {
            const std::size_t tab = line.find('\t', pos);
            c.push_back(line.substr(pos, tab == std::string_view::npos ? std::string_view::npos : tab - pos));
            if (tab == std::string_view::npos) break;
            pos = tab + 1;
        }
        fn(c);
    }
}

std::uint32_t U32(std::string_view s) { return static_cast<std::uint32_t>(std::strtoul(std::string(s).c_str(), nullptr, 10)); }

} // namespace

bool DecodeGameGuide(std::string_view text, std::vector<GameGuideRow>& out, std::string& error) {
    out.clear();
    ForEachRow(text, [&](const std::vector<std::string_view>& c) {
        if (c.size() < 7) return;
        GameGuideRow row;
        row.inService = c[0] == "1";
        row.id = U32(c[1]);
        row.level = static_cast<int>(U32(c[3]));
        row.parent = U32(c[4]);
        row.pageKey = std::string(c[5]);
        row.menuKey = std::string(c[6]);
        out.push_back(std::move(row));
    });
    if (out.empty()) error = "no rows";
    return !out.empty();
}

bool DecodeEventGuide(std::string_view text, std::vector<EventGuideRow>& out, std::string& error) {
    out.clear();
    ForEachRow(text, [&](const std::vector<std::string_view>& c) {
        if (c.size() < 7) return;
        EventGuideRow row;
        row.inService = c[0] == "1";
        row.who = std::string(c[2]);
        row.kind = static_cast<int>(U32(c[3]));
        row.value = std::string(c[4]);
        row.guide = U32(c[6]);
        out.push_back(std::move(row));
    });
    if (out.empty()) error = "no rows";
    return !out.empty();
}

bool LoadGameGuide(const FileSystem& files, GameGuideData& out, std::string& error) {
    const auto topics = files.Read("server_dep\\silkroad\\textdata\\gameguidedata.txt");
    const auto events = files.Read("server_dep\\silkroad\\textdata\\eventguidedata.txt");
    if (!topics || !events) {
        error = "gameguidedata.txt or eventguidedata.txt missing";
        return false;
    }
    if (!DecodeGameGuide(core::DecodeTextFile(topics->Bytes()), out.topics, error) ||
        !DecodeEventGuide(core::DecodeTextFile(events->Bytes()), out.events, error)) {
        return false;
    }
    out.questLevels.clear();
    if (QuestTable quests; quests.Load(files, error)) {
        for (const QuestRow& q : quests.Rows()) out.questLevels[q.codename] = q.level;
    }
    error.clear();
    return true;
}

} // namespace sr::assets
