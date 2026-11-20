#include "sr/assets/StallNetworkData.h"

#include <charconv>

#include "sr/assets/FileSystem.h"
#include "sr/core/Text.h"

namespace sr::assets {

namespace {

constexpr std::string_view kSpace = " \t\r\n\xEF\xBB\xBF";

std::string_view Trim(std::string_view s) {
    const std::size_t a = s.find_first_not_of(kSpace);
    if (a == std::string_view::npos) return {};
    return s.substr(a, s.find_last_not_of(kSpace) - a + 1);
}

template <typename F>
void Rows(std::string_view text, F&& row) {
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = Trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (line.empty() || line.starts_with("//")) continue;
        std::vector<std::string_view> fields;
        while (true) {
            const std::size_t tab = line.find('\t');
            fields.push_back(Trim(line.substr(0, tab)));
            if (tab == std::string_view::npos) break;
            line.remove_prefix(tab + 1);
        }
        row(fields);
    }
}

int Int(std::string_view s) {
    int v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

} // namespace

void StallNetworkData::ParseCategories(std::string_view text) {
    classes_.clear();
    Rows(text, [&](const std::vector<std::string_view>& f) {
        if (f.size() < 6 || Int(f[0]) != 1) return;
        if (f[3] == "xxx") {
            classes_.push_back(StallNetworkClass{std::string(f[1]), std::string(f[2]), {}});
            return;
        }
        for (StallNetworkClass& c : classes_) {
            if (c.code == f[3]) {
                c.types.push_back(StallNetworkType{std::string(f[1]), std::string(f[2]), std::uint32_t(Int(f[4])), std::uint8_t(Int(f[5]))});
                return;
            }
        }
    });
}

void StallNetworkData::ParseGroups(std::string_view text) {
    rules_.clear();
    Rows(text, [&](const std::vector<std::string_view>& f) {
        if (f.size() < 6 || Int(f[0]) != 1) return;
        Rule r;
        r.group = std::uint32_t(Int(f[1]));
        for (int i = 0; i < 4; ++i) r.tid[i] = std::uint8_t(Int(f[2 + i]));
        rules_.push_back(r);
    });
}

bool StallNetworkData::Load(const FileSystem& files, std::string& error) {
    const auto categories = files.Read("server_dep\\silkroad\\textdata\\fmncategorytreedata.txt");
    const auto groups = files.Read("server_dep\\silkroad\\textdata\\fmntidgroupmapdata.txt");
    if (!categories || !groups) {
        error = "the stall network's tables (fmncategorytreedata.txt, fmntidgroupmapdata.txt) are missing";
        return false;
    }
    ParseCategories(core::DecodeTextFile(categories->Bytes()));
    ParseGroups(core::DecodeTextFile(groups->Bytes()));
    if (classes_.empty() || rules_.empty()) {
        error = "the stall network's tables are empty";
        return false;
    }
    return true;
}

std::uint32_t StallNetworkData::GroupOf(std::uint8_t tid1, std::uint8_t tid2, std::uint8_t tid3, std::uint8_t tid4) const {
    // A row naming all four first, then one with a 0 (any) among them: over the 1.188 items (11,920 of TID1 3)
    // this gives each at most one group; 86 have none (trade goods and a few others).
    const std::uint8_t tid[4] = {tid1, tid2, tid3, tid4};
    std::uint32_t wild = 0;
    for (const Rule& r : rules_) {
        bool match = true, any = false;
        for (int i = 0; i < 4 && match; ++i) {
            any = any || r.tid[i] == 0;
            match = r.tid[i] == 0 || r.tid[i] == tid[i];
        }
        if (!match) continue;
        if (!any) return r.group;
        if (wild == 0) wild = r.group;
    }
    return wild;
}

const StallNetworkType* StallNetworkData::TypeOf(std::uint32_t group) const {
    for (const StallNetworkClass& c : classes_) {
        for (const StallNetworkType& t : c.types) {
            if (t.group == group) return &t;
        }
    }
    return nullptr;
}

} // namespace sr::assets
