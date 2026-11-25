#include "sr/assets/GameData.h"

#include <cctype>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <cstdlib>
#include <unordered_set>

#include "sr/assets/FileSystem.h"
#include "sr/core/Path.h"
#include "sr/core/Text.h"

namespace sr::assets {

namespace {

constexpr const char* kTextData = "server_dep\\silkroad\\textdata\\";

bool EndsWithBsr(std::string_view s) {
    if (s.size() < 4) return false;
    const std::string_view tail = s.substr(s.size() - 4);
    return (tail[0] == '.') && (tail[1] == 'b' || tail[1] == 'B') && (tail[2] == 's' || tail[2] == 'S') &&
           (tail[3] == 'r' || tail[3] == 'R');
}

bool EndsWithCpd(std::string_view s) {
    if (s.size() < 4) return false;
    const std::string_view tail = s.substr(s.size() - 4);
    return (tail[0] == '.') && (tail[1] == 'c' || tail[1] == 'C') && (tail[2] == 'p' || tail[2] == 'P') &&
           (tail[3] == 'd' || tail[3] == 'D');
}

std::vector<std::string_view> Columns(std::string_view line) {
    std::vector<std::string_view> columns;
    for (std::size_t pos = 0;;) {
        const std::size_t tab = line.find('\t', pos);
        columns.push_back(line.substr(pos, tab == std::string_view::npos ? std::string_view::npos : tab - pos));
        if (tab == std::string_view::npos) break;
        pos = tab + 1;
    }
    return columns;
}

template <typename RowFn>
void ForEachLine(std::string_view text, RowFn row) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.starts_with("//")) continue;
        row(line);
    }
}

int Int(std::string_view s) { return static_cast<int>(std::strtol(std::string(s).c_str(), nullptr, 10)); }
float Float(std::string_view s) { return std::strtof(std::string(s).c_str(), nullptr); }

// Every shard under textdata whose file name starts with `prefix`.
template <typename ShardFn>
std::size_t ForEachShard(const FileSystem& fs, std::string_view prefix, ShardFn shard) {
    std::size_t count = 0;
    for (const std::string& path : fs.ListFolder(kTextData)) {
        const std::string key = core::NormalizeAssetPath(path);
        const std::string_view name = std::string_view(key).substr(key.rfind('\\') + 1);
        if (!key.starts_with(kTextData) || !name.starts_with(prefix) || !key.ends_with(".txt")) continue;
        auto file = fs.Read(path);
        if (!file) continue;
        shard(core::DecodeTextFile(file->Bytes()));
        ++count;
    }
    return count;
}

// The encrypted tables' stream: the byte at file offset i gets ADD[k % 167] - SUB[k % 495] added,
// k = 0x8C1F + i. The two tables (SUB, 495 bytes; ADD, 167 bytes) are the client's data.
constexpr std::uint8_t kTableSub[495] = {
    0x0D, 0x05, 0x90, 0x41, 0xF9, 0xD0, 0x65, 0xBF, 0xF9, 0x0B, 0x15, 0x93, 0x80, 0xFB, 0x01, 0x02, 0xB6, 0x08, 0xC4, 0x3C,
    0xC1, 0x49, 0x94, 0x4D, 0xCE, 0x1D, 0xFD, 0x69, 0xEA, 0x19, 0xC9, 0x57, 0x9C, 0x4D, 0x84, 0x62, 0xE3, 0x67, 0xF9, 0x87,
    0xF4, 0xF9, 0x93, 0xDA, 0xE5, 0x15, 0xF1, 0x4C, 0xA4, 0xEC, 0xBC, 0xCF, 0xDD, 0xB3, 0x6F, 0x04, 0x3D, 0x70, 0x1C, 0x74,
    0x21, 0x6B, 0x00, 0x71, 0x31, 0x7F, 0x54, 0xB3, 0x72, 0x6C, 0xAA, 0x42, 0xC1, 0x78, 0x61, 0x3E, 0xD5, 0xF2, 0xE1, 0x27,
    0x36, 0x71, 0x3A, 0x25, 0x36, 0x57, 0xD1, 0xF8, 0x70, 0x86, 0xBD, 0x0E, 0x58, 0xB3, 0x76, 0x6D, 0xC3, 0x50, 0xF6, 0x6C,
    0xA0, 0x10, 0x06, 0x64, 0xA2, 0xD6, 0x2C, 0xD4, 0x27, 0x30, 0xA5, 0x36, 0x1C, 0x1E, 0x3E, 0x58, 0x9D, 0x59, 0x76, 0x9D,
    0xA7, 0x42, 0x5A, 0xF0, 0x00, 0xBC, 0x69, 0x31, 0x40, 0x1E, 0xFA, 0x09, 0x1D, 0xE7, 0xEE, 0xE4, 0x54, 0x89, 0x36, 0x7C,
    0x67, 0xC8, 0x65, 0x22, 0x7E, 0xA3, 0x60, 0x44, 0x1E, 0xBC, 0x68, 0x6F, 0x15, 0x2A, 0xFD, 0x9D, 0x3F, 0x36, 0x6B, 0x28,
    0x06, 0x67, 0xFE, 0xC6, 0x49, 0x6B, 0x9B, 0x3F, 0x80, 0x2A, 0xD2, 0xD4, 0xD3, 0x20, 0x1B, 0x96, 0xF4, 0xD2, 0xCA, 0x8C,
    0x74, 0xEE, 0x0B, 0x6A, 0xE1, 0xE9, 0xC6, 0xD2, 0x6E, 0x33, 0x63, 0xC0, 0xE9, 0xD0, 0x37, 0xA9, 0x3C, 0xF7, 0x18, 0xF2,
    0x4A, 0x74, 0xEC, 0x41, 0x61, 0x7A, 0x19, 0x47, 0x8F, 0xA0, 0xBB, 0x94, 0x8F, 0x3D, 0x11, 0x11, 0x26, 0xCF, 0x69, 0x18,
    0x1B, 0x2C, 0x87, 0x6D, 0xB3, 0x22, 0x6C, 0x78, 0x41, 0xCC, 0xC2, 0x84, 0xC5, 0xCB, 0x01, 0x6A, 0x37, 0x00, 0x01, 0x65,
    0x4F, 0xA7, 0x85, 0x85, 0x15, 0x59, 0x05, 0x67, 0xF2, 0x4F, 0xAB, 0xB7, 0x88, 0xFA, 0x69, 0x24, 0x9E, 0xC6, 0x7B, 0x3F,
    0xD5, 0x0E, 0x4D, 0x7B, 0xFB, 0xB1, 0x21, 0x3C, 0xB0, 0xC0, 0xCB, 0x2C, 0xAA, 0x26, 0x8D, 0xCC, 0xDD, 0xDA, 0xC1, 0xF8,
    0xCA, 0x7F, 0x6A, 0x3F, 0x2A, 0x61, 0xE7, 0x60, 0x5C, 0xCE, 0xD3, 0x4C, 0xAC, 0x45, 0x40, 0x62, 0xEA, 0x51, 0xF1, 0x66,
    0x5D, 0x2C, 0x45, 0xD6, 0x8B, 0x7D, 0xCE, 0x9C, 0xF5, 0xBB, 0xF7, 0x52, 0x24, 0x1A, 0x13, 0x02, 0x2B, 0x00, 0xBB, 0xA1,
    0x8F, 0x6E, 0x7A, 0x33, 0xAD, 0x5F, 0xF4, 0x4A, 0x82, 0x76, 0xAB, 0xDE, 0x80, 0x98, 0x8B, 0x26, 0x4F, 0x33, 0xD8, 0x68,
    0x1E, 0xD9, 0xAE, 0x06, 0x6B, 0x7E, 0xA9, 0x95, 0x67, 0x60, 0xEB, 0xE8, 0xD0, 0x7D, 0x07, 0x4B, 0xF1, 0xAA, 0x9A, 0xC5,
    0x29, 0x93, 0x9D, 0x5C, 0x92, 0x3F, 0x15, 0xDE, 0x48, 0xF1, 0xCA, 0xEA, 0xC9, 0x78, 0x3C, 0x28, 0x7E, 0xB0, 0x46, 0xD3,
    0x71, 0x6C, 0xD7, 0xBD, 0x2C, 0xF7, 0x25, 0x2F, 0xC7, 0xDD, 0xB4, 0x6D, 0x35, 0xBB, 0xA7, 0xDA, 0x3E, 0x3D, 0xA7, 0xCA,
    0xBD, 0x87, 0xDD, 0x9F, 0x22, 0x3D, 0x50, 0xD2, 0x30, 0xD5, 0x14, 0x5B, 0x8F, 0xF4, 0xAF, 0xAA, 0xA0, 0xFC, 0x17, 0x3D,
    0x33, 0x10, 0x99, 0xDC, 0x76, 0xA9, 0x40, 0x1B, 0x64, 0x14, 0xDF, 0x35, 0x68, 0x66, 0x5B, 0x49, 0x05, 0x33, 0x68, 0x26,
    0xC8, 0xBA, 0xD1, 0x8D, 0x39, 0x2B, 0xFB, 0x3E, 0x24, 0x52, 0x2F, 0x9A, 0x69, 0xBC, 0xF2, 0xB2, 0xAC, 0xB8, 0xEF, 0xA1,
    0x17, 0x29, 0x2D, 0xEE, 0xF5, 0x23, 0x21, 0xEC, 0x81, 0xC7, 0x5B, 0xC0, 0x82, 0xCC, 0xD2, 0x91, 0x9D, 0x29, 0x93, 0x0C,
    0x9D, 0x5D, 0x57, 0xAD, 0xD4, 0xC6, 0x40, 0x93, 0x8D, 0xE9, 0xD3, 0x35, 0x9D, 0xC6, 0xD3,
};
constexpr std::uint8_t kTableAdd[167] = {
    0x07, 0x83, 0xBC, 0xEE, 0x4B, 0x79, 0x19, 0xB6, 0x2A, 0x53, 0x4F, 0x3A, 0xCF, 0x71, 0xE5, 0x3C, 0x2D, 0x18, 0x14, 0xCB,
    0xB6, 0xBC, 0xAA, 0x9A, 0x31, 0x42, 0x3A, 0x13, 0x42, 0xC9, 0x63, 0xFC, 0x54, 0x1D, 0xF2, 0xC1, 0x8A, 0xDD, 0x1C, 0xB3,
    0x52, 0xEA, 0x9B, 0xD7, 0xC4, 0xBA, 0xF8, 0x12, 0x74, 0x92, 0x30, 0xC9, 0xD6, 0x56, 0x15, 0x52, 0x53, 0x60, 0x11, 0x33,
    0xC5, 0x9D, 0x30, 0x9A, 0xE5, 0xD2, 0x93, 0x99, 0xEB, 0xCF, 0xAA, 0x79, 0xE3, 0x78, 0x6A, 0xB9, 0x02, 0xE0, 0xCE, 0x8E,
    0xF3, 0x63, 0x5A, 0x73, 0x74, 0xF3, 0x72, 0xAA, 0x2C, 0x9F, 0xBB, 0x33, 0x91, 0xDE, 0x5F, 0x91, 0x66, 0x48, 0xD1, 0x7A,
    0xFD, 0x3F, 0x91, 0x3E, 0x5D, 0x22, 0xEC, 0xEF, 0x7C, 0xA5, 0x43, 0xC0, 0x1D, 0x4F, 0x60, 0x7F, 0x0B, 0x4A, 0x4B, 0x2A,
    0x43, 0x06, 0x46, 0x14, 0x45, 0xD0, 0xC5, 0x83, 0x92, 0xE4, 0x16, 0xD0, 0xA3, 0xA1, 0x13, 0xDA, 0xD1, 0x51, 0x07, 0xEB,
    0x7D, 0xCE, 0xA5, 0xDB, 0x78, 0xE0, 0xC1, 0x0B, 0xE5, 0x8E, 0x1C, 0x7C, 0xB4, 0xDF, 0xED, 0xB8, 0x53, 0xBA, 0x2C, 0xB5,
    0xBB, 0x56, 0xFB, 0x68, 0x95, 0x6E, 0x65,
};
constexpr std::uint32_t kTableSeed = 0x8C1F;

// A text table as the client reads it: an encrypted one decrypted first.
std::string TextTable(const FileData& file) {
    const std::span<const std::byte> raw = file.Bytes();
    std::vector<std::byte> bytes(raw.begin(), raw.end());
    if (!DecryptTextTable(bytes)) return core::DecodeTextFile(raw);
    return core::DecodeTextFile(bytes);
}

std::string UnderRes(const FileSystem& fs, const std::string& path) {
    if (path.empty()) return {};
    if (fs.Exists(path)) return path;
    const std::string underRes = "res\\" + path;
    return fs.Exists(underRes) ? underRes : std::string();
}

} // namespace

bool DecodeItemRows(std::string_view text, std::vector<ItemRow>& out, std::string& error) {
    out.clear();
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        if (c.size() < 59 || c[2].empty()) return;
        ItemRow row;
        row.inService = c[0] == "1";
        row.refObjId = static_cast<std::uint32_t>(std::strtoul(std::string(c[1]).c_str(), nullptr, 10));
        row.codename = std::string(c[2]);
        row.linkedCodename = std::string(c[4]);
        for (int i = 0; i < 4; ++i) row.typeId[static_cast<std::size_t>(i)] = Int(c[static_cast<std::size_t>(9 + i)]);
        row.country = Int(c[14]);
        const std::string_view model = c[52];
        if (!model.empty() && model != "xxx" && model != "XXX") row.modelPath = std::string(model);
        const std::string_view drop = c[53];
        if (!drop.empty() && drop != "xxx" && drop != "XXX") row.dropModelPath = std::string(drop);
        row.sex = Int(c[58]);
        row.nameKey = std::string(c[5]);
        row.descKey = std::string(c[6]);
        row.requiredLevel = Int(c[33]);
        const std::string_view icon = c[54];
        if (!icon.empty() && icon != "xxx" && icon != "XXX") row.iconPath = std::string(icon);
        row.maxStack = std::max(1, Int(c[57]));
        if (c.size() > 31) row.sellPrice = std::uint64_t(std::max(0, Int(c[31])));
        if (c.size() > 26) row.price = std::uint64_t(std::max(0, Int(c[26])));
        if (c.size() > 28) {
            row.costRepair = std::uint64_t(std::max(0, Int(c[27])));
            row.costRevive = std::uint64_t(std::max(0, Int(c[28])));
            row.canRepair = Int(c[22]) != 0;
            row.canRevive = Int(c[23]) != 0;
        }
        row.cashItem = Int(c[7]) != 0;
        if (c.size() > 16) row.canTrade = Int(c[16]) != 0;
        if (c.size() > 17) row.canSell = Int(c[17]) != 0;
        if (c.size() > 20) row.canDrop = Int(c[20]);
        row.decayMs = Int(c[13]);
        row.canBuy = Int(c[18]);
        row.canBorrow = Int(c[19]);
        row.canPick = Int(c[21]);
        row.canUse = Int(c[24]);
        row.canThrow = Int(c[25]);
        row.costBorrow = std::uint64_t(std::max(0, Int(c[29])));
        row.keepingFee = std::uint64_t(std::max(0, Int(c[30])));
        if (c.size() > 62) row.setId = Int(c[62]);
        row.rarity = Int(c[15]);
        for (std::size_t i = 0; i < 4; ++i) {
            row.requirementTypes[i] = Int(c[32 + 2 * i]);
            row.requirementLevels[i] = Int(c[33 + 2 * i]);
        }
        if (c.size() > 59) row.requiredStr = Int(c[59]);
        if (c.size() > 60) row.requiredInt = Int(c[60]);
        if (c.size() > 61) row.itemClass = Int(c[61]);
        for (std::size_t i = ItemRow::kFirstStatColumn; i <= 117 && i < c.size(); ++i) {
            row.stats.push_back(std::strtof(std::string(c[i]).c_str(), nullptr));
        }
        for (std::size_t i = 0; i < row.params.size(); ++i) {
            if (118 + 2 * i < c.size()) row.params[i] = Int(c[118 + 2 * i]);
            if (119 + 2 * i < c.size() && c[119 + 2 * i] != "xxx") row.paramTexts[i] = std::string(c[119 + 2 * i]);
        }
        if (c.size() > 40) row.maxPossess = Int(c[40]);
        if (c.size() > 158) row.maxMagicOptions = Int(c[158]);
        if (c.size() > 159) row.childItems = Int(c[159]);
        if (c.size() > 123 && c[123] != "xxx") row.useKind = std::string(c[123]);
        // The twenty description pairs (value, "KIND:arg"): the last COOLTIME one gives the use
        // delay (the value, ms) and the group sharing it (the low byte of the hex argument).
        for (std::size_t i = 119; i < c.size() && i < 159; i += 2) {
            const std::string_view tag = c[i];
            if (!tag.starts_with("COOLTIME:")) continue;
            const long long ms = std::strtoll(std::string(c[i - 1]).c_str(), nullptr, 10);
            if (ms < 0 || ms > 0x7fffffffLL) continue;
            row.cooldownMs = static_cast<int>(ms);
            row.cooldownGroup = static_cast<int>(std::strtoull(std::string(tag.substr(9)).c_str(), nullptr, 16) & 0xFF);
        }
        out.push_back(std::move(row));
    });
    if (out.empty()) {
        error = "no item rows";
        return false;
    }
    return true;
}

bool DecodeLevelTable(std::string_view text, std::vector<std::uint64_t>& expToNext, std::string& error) {
    expToNext.clear();
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        if (c.size() < 2) return;
        const int level = Int(c[0]);
        if (level <= 0 || level > 255) return;
        if (expToNext.size() <= static_cast<std::size_t>(level)) expToNext.resize(static_cast<std::size_t>(level) + 1, 0);
        expToNext[static_cast<std::size_t>(level)] = std::strtoull(std::string(c[1]).c_str(), nullptr, 10);
    });
    if (expToNext.empty()) {
        error = "no level rows";
        return false;
    }
    return true;
}

bool DecodeMonsterExperience(std::string_view text, std::vector<std::uint32_t>& byLevel, std::string& error) {
    byLevel.clear();
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        if (c.size() < 6) return;
        const int level = Int(c[0]);
        if (level <= 0 || level > 255) return;
        if (byLevel.size() <= static_cast<std::size_t>(level)) byLevel.resize(static_cast<std::size_t>(level) + 1, 0);
        byLevel[static_cast<std::size_t>(level)] = static_cast<std::uint32_t>(std::max(0, Int(c[5])));
    });
    if (byLevel.empty()) {
        error = "no level rows";
        return false;
    }
    return true;
}

bool DecodeJobExperience(std::string_view text, std::vector<std::array<std::int64_t, 3>>& byLevel, std::string& error) {
    byLevel.clear();
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        if (c.size() < 9) return;
        const int level = Int(c[0]);
        if (level <= 0 || level > 255) return;
        if (byLevel.size() <= static_cast<std::size_t>(level)) byLevel.resize(static_cast<std::size_t>(level) + 1, {0, 0, 0});
        for (std::size_t j = 0; j < 3; ++j) byLevel[static_cast<std::size_t>(level)][j] = Int(c[6 + j]);
    });
    if (byLevel.empty()) {
        error = "no level rows";
        return false;
    }
    return true;
}

bool DecodeLevelGold(std::string_view text, std::vector<std::pair<std::uint32_t, std::uint32_t>>& byLevel, std::string& error) {
    byLevel.clear();
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        if (c.size() < 3) return;
        const int level = Int(c[0]);
        if (level <= 0 || level > 255) return;
        if (byLevel.size() <= static_cast<std::size_t>(level)) byLevel.resize(static_cast<std::size_t>(level) + 1, {0, 0});
        const auto low = static_cast<std::uint32_t>(std::max(0, Int(c[1]))), high = static_cast<std::uint32_t>(std::max(0, Int(c[2])));
        byLevel[static_cast<std::size_t>(level)] = {low, std::max(low, high)};
    });
    if (byLevel.empty()) {
        error = "no level rows";
        return false;
    }
    return true;
}

bool DecodeMagicOptions(std::string_view text, std::vector<MagicOptionRow>& out, std::string& error) {
    out.clear();
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        if (c.size() < 5 || Int(c[0]) != 1) return;
        MagicOptionRow row;
        const int id = Int(c[1]);
        if (id <= 0 || id > 0xFFFF) return;
        row.paramId = static_cast<std::uint32_t>(id);
        row.optionName = std::string(c[2]);
        row.paramName = std::string(c[3]);
        row.degree = Int(c[4]);
        if (c.size() > 5) row.prob = std::strtof(std::string(c[5]).c_str(), nullptr);
        if (c.size() > 7) row.attribute = static_cast<std::uint32_t>(std::strtoul(std::string(c[7]).c_str(), nullptr, 10));
        if (c.size() >= 11) {
            row.hasRange = true;
            for (int i = 0; i < 3; ++i) {
                const std::string word(c[static_cast<std::size_t>(8 + i)]);
                char* end = nullptr;
                const long long v = std::strtoll(word.c_str(), &end, 10);
                if (word.empty() || end == word.c_str() || v < 0 || v > 0xFFFFFFFFLL) {
                    row.hasRange = false;
                    break;
                }
                row.rangeWords[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(v);
            }
        }
        for (std::size_t i = 29; i < c.size() && i <= 47; i += 2) {
            if (c[i].empty() || c[i] == "xxx") continue;
            row.availGroups.emplace_back(c[i]);
            row.reqClasses.push_back(i + 1 < c.size() ? Int(c[i + 1]) : 0);
        }
        out.push_back(std::move(row));
    });
    if (out.empty()) {
        error = "no magic option rows";
        return false;
    }
    return true;
}

bool DecodeMagicOptionAssign(std::string_view text, std::vector<MagicOptionAssignRow>& out, std::string& error) {
    out.clear();
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        if (c.size() < 5 || c[0].empty() || Int(c[0]) == 0) return;
        MagicOptionAssignRow row{Int(c[1]), Int(c[2]), Int(c[3]), {}};
        if (row.race < 0 || row.race > 3 || row.tid3 <= 0 || row.tid4 <= 0) return;
        for (std::size_t i = 4; i < c.size() && i < 29; ++i) {
            if (c[i].empty() || c[i] == "xxx") break;
            if (std::find(row.options.begin(), row.options.end(), c[i]) == row.options.end()) row.options.emplace_back(c[i]);
        }
        out.push_back(std::move(row));
    });
    if (out.empty()) {
        error = "no magic option assign rows";
        return false;
    }
    return true;
}

bool DecryptTextTable(std::vector<std::byte>& bytes) {
    static constexpr char16_t kMarker[] = u"\r\n//encrypt";
    constexpr std::size_t kMarkerBytes = (std::size(kMarker) - 1) * 2;
    if (bytes.size() < kMarkerBytes) return false;
    const std::size_t body = bytes.size() - kMarkerBytes;
    for (std::size_t i = 0; i + 1 < std::size(kMarker); ++i) {
        const auto lo = std::to_integer<std::uint8_t>(bytes[body + 2 * i]), hi = std::to_integer<std::uint8_t>(bytes[body + 2 * i + 1]);
        if (lo != std::uint8_t(kMarker[i] & 0xFF) || hi != std::uint8_t(kMarker[i] >> 8)) return false;
    }
    for (std::size_t i = 0; i < body; ++i) {
        const std::uint32_t k = kTableSeed + std::uint32_t(i);
        const std::uint8_t key = std::uint8_t(kTableAdd[k % 167] - kTableSub[k % 495]);
        bytes[i] = std::byte(std::uint8_t(std::to_integer<std::uint8_t>(bytes[i]) + key));
    }
    return true;
}

std::vector<std::string> ListedShards(const FileSystem& fs, std::string_view listFile) {
    std::vector<std::string> out;
    const auto file = fs.Read(std::string(kTextData) + std::string(listFile));
    if (!file) return out;
    const std::string text = core::DecodeTextFile(file->Bytes());
    ForEachLine(text, [&](std::string_view line) {
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty()) return;
        std::string name(line);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        out.push_back(std::string(kTextData) + name);
    });
    return out;
}

bool DecodeMagicOptGroups(std::string_view text, std::vector<MagicOptGroupRow>& out, std::string& error) {
    out.clear();
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        if (c.size() < 7 || Int(c[0]) != 1) return;
        MagicOptGroupRow row;
        row.link = Int(c[1]);
        row.magicType = Int(c[2]);
        row.codename = std::string(c[3]);
        row.optId = static_cast<std::uint32_t>(std::max(0, Int(c[4])));
        row.optLevel = Int(c[5]);
        row.value = Int(c[6]);
        out.push_back(std::move(row));
    });
    if (out.empty()) {
        error = "no magic option group rows";
        return false;
    }
    return true;
}

bool DecodeQuestData(std::string_view text, std::vector<QuestRow>& out, std::string& error) {
    out.clear();
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        if (c.size() < 6 || Int(c[0]) != 1) return;
        QuestRow row;
        row.id = static_cast<std::uint32_t>(Int(c[1]));
        row.codename = std::string(c[2]);
        row.level = Int(c[3]);
        row.titleKey = std::string(c[5]);
        if (c.size() > 6) row.payKey = std::string(c[6]);
        if (c.size() > 8) row.payTextKey = std::string(c[8]);
        if (c.size() > 9) row.guideKey = std::string(c[9]);
        if (c.size() > 10) row.conditionKey = std::string(c[10]);
        if (row.id != 0) out.push_back(std::move(row));
    });
    if (out.empty()) {
        error = "no quest rows";
        return false;
    }
    return true;
}

void DecodeQuestContents(std::string_view text, std::vector<QuestRow>& rows) {
    std::unordered_map<std::string, QuestRow*> byCodename;
    for (QuestRow& row : rows) byCodename.emplace(row.codename, &row);
    std::set<std::string> seen;
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        if (c.size() < 5) return;
        const std::string codename(c[0]);
        if (!seen.insert(codename).second) return;
        const auto it = byCodename.find(codename);
        if (it == byCodename.end()) return;
        it->second->giveUpWarning = Int(c[4]);
        for (std::size_t i = 5; i < c.size() && i <= 12; ++i) {
            if (!c[i].empty() && c[i] != "xxx") it->second->objectiveKeys.emplace_back(c[i]);
        }
    });
}

bool QuestTable::Load(const FileSystem& fs, std::string& error) {
    auto data = fs.Read(std::string(kTextData) + "questdata.txt");
    if (!data) {
        error = "questdata.txt not found";
        return false;
    }
    if (!DecodeQuestData(core::DecodeTextFile(data->Bytes()), rows_, error)) return false;
    if (auto contents = fs.Read(std::string(kTextData) + "questcontentsdata.txt")) {
        DecodeQuestContents(core::DecodeTextFile(contents->Bytes()), rows_);
    }
    return true;
}

const QuestRow* QuestTable::Find(std::uint32_t id) const {
    for (const QuestRow& row : rows_) {
        if (row.id == id) return &row;
    }
    return nullptr;
}

const QuestRow* QuestTable::Find(std::string_view codename) const {
    for (const QuestRow& row : rows_) {
        if (row.codename == codename) return &row;
    }
    return nullptr;
}

bool DecodeGameGuide(std::string_view text, std::vector<GuideRow>& out, std::string& error) {
    out.clear();
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        if (c.size() < 7 || c[1].empty() || !std::isdigit(static_cast<unsigned char>(c[1][0]))) return;
        GuideRow row;
        row.enabled = c[0] == "1";
        row.id = static_cast<std::uint32_t>(std::strtoul(std::string(c[1]).c_str(), nullptr, 10));
        row.depth = Int(c[3]);
        row.parentOrCount = Int(c[4]);
        row.contentKey = std::string(c[5]);
        row.menuKey = std::string(c[6]);
        out.push_back(std::move(row));
    });
    if (out.empty()) {
        error = "no game guide rows";
        return false;
    }
    return true;
}

bool DecodeMasteryCosts(std::string_view text, std::vector<std::uint32_t>& spByLevel, std::string& error) {
    spByLevel.clear();
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        if (c.size() < 3) return;
        const int level = Int(c[0]);
        if (level <= 0 || level > 255) return;
        if (spByLevel.size() <= static_cast<std::size_t>(level)) spByLevel.resize(static_cast<std::size_t>(level) + 1, 0);
        spByLevel[static_cast<std::size_t>(level)] = static_cast<std::uint32_t>(std::strtoul(std::string(c[2]).c_str(), nullptr, 10));
    });
    if (spByLevel.empty()) {
        error = "no level rows";
        return false;
    }
    return true;
}

bool ItemTable::Load(const FileSystem& fs, std::string& error) {
    byCode_.clear();
    byId_.clear();
    ForEachShard(fs, "itemdata", [&](const std::string& text) {
        std::vector<ItemRow> rows;
        std::string ignored;
        if (!DecodeItemRows(text, rows, ignored)) return;
        for (ItemRow& row : rows) {
            if (!row.inService && byCode_.contains(row.codename)) continue;
            byId_[row.refObjId] = row.codename;
            byCode_[row.codename] = std::move(row);
        }
    });
    if (byCode_.empty()) {
        error = "no itemdata rows under " + std::string(kTextData);
        return false;
    }
    return true;
}

const ItemRow* ItemTable::Find(std::string_view codename) const {
    const auto it = byCode_.find(std::string(codename));
    return it == byCode_.end() ? nullptr : &it->second;
}

const ItemRow* ItemTable::Find(std::uint32_t refObjId) const {
    const auto it = byId_.find(refObjId);
    return it == byId_.end() ? nullptr : Find(it->second);
}

std::string ItemTable::ResolveModel(const FileSystem& fs, const ItemRow& row) const {
    if (!row.modelPath.empty()) return UnderRes(fs, row.modelPath);
    if (const ItemRow* linked = Find(row.linkedCodename); linked != nullptr && linked != &row) {
        return UnderRes(fs, linked->modelPath);
    }
    return {};
}

std::string ItemTable::ResolveDropModel(const FileSystem& fs, const ItemRow& row) const {
    if (std::string path = UnderRes(fs, row.dropModelPath); !path.empty()) return path;
    return ResolveModel(fs, row);
}

bool DecodeObjectRows(std::string_view text, std::vector<ObjectRow>& out, std::string& error) {
    out.clear();
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.starts_with("//")) continue;
        std::vector<std::string_view> columns;
        for (std::size_t pos = 0;;) {
            const std::size_t tab = line.find('\t', pos);
            columns.push_back(line.substr(pos, tab == std::string_view::npos ? std::string_view::npos : tab - pos));
            if (tab == std::string_view::npos) break;
            pos = tab + 1;
        }
        if (columns.size() < 3 || columns[2].empty()) continue;
        ObjectRow row;
        row.inService = columns[0] == "1";
        row.refObjId = static_cast<std::uint32_t>(std::strtoul(std::string(columns[1]).c_str(), nullptr, 10));
        row.codename = std::string(columns[2]);
        if (columns.size() > 12) {
            for (std::size_t i = 0; i < 4; ++i) row.typeId[i] = Int(columns[9 + i]);
        }
        if (columns.size() > 5) row.nameKey = std::string(columns[5]);
        if (columns.size() > 7) row.cashItem = Int(columns[7]) != 0;
        if (columns.size() > 4 && columns[4] != "xxx" && columns[4] != "XXX") row.linkedCodename = std::string(columns[4]);
        if (columns.size() > 40) row.maxContain = static_cast<std::uint32_t>(std::max(0, Int(columns[40])));
        if (columns.size() > 31) {
            row.canSell = Int(columns[17]) != 0;
            row.sellPrice = Int(columns[31]);
        }
        if (columns.size() > 59) {
            row.level = Int(columns[57]);
            row.maxHp = static_cast<std::uint32_t>(std::strtoul(std::string(columns[59]).c_str(), nullptr, 10));
            if (const int scale = Int(columns[48]); scale > 0) row.scalePercent = scale;
        }
        if (columns.size() > 93) {
            row.materialKind = Int(columns[93]);
            row.rarity = Int(columns[15]);
            row.experience = static_cast<std::uint32_t>(std::strtoul(std::string(columns[79]).c_str(), nullptr, 10));
            row.bodyRadius = static_cast<float>(std::atof(std::string(columns[50]).c_str()));
            row.knockdown = Int(columns[81]);
            row.knockdownRecoverMs = Int(columns[82]);
            for (std::size_t i = 0; i < row.skills.size(); ++i) row.skills[i] = static_cast<std::uint32_t>(Int(columns[83 + i]));
            if (columns.size() > 97) row.except4 = Int(columns[97]);
            const auto number = [&](std::size_t i) { return static_cast<float>(std::atof(std::string(columns[i]).c_str())); };
            row.phyDefence = number(71);
            row.magDefence = number(72);
            row.phyParry = number(73);
            row.magParry = number(74);
            row.evasion = number(75);
            row.block = number(76);
            row.hitRate = number(77);
            row.critical = number(78);
            row.country = Int(columns[14]);
        }
        if (columns.size() > 66) {
            row.walkSpeed = static_cast<float>(std::atof(std::string(columns[46]).c_str()));
            row.runSpeed = static_cast<float>(std::atof(std::string(columns[47]).c_str()));
            if (columns[54] != "xxx") row.iconPath = std::string(columns[54]);
            row.canRide = Int(columns[66]) != 0;
        }
        for (std::size_t i = 3; i < columns.size(); ++i) {
            if (EndsWithBsr(columns[i])) {
                row.modelPath = std::string(columns[i]);
                break;
            }
        }
        // A few name a compound of parts instead (.cpd: the Flame Master, the tomb's thieves).
        for (std::size_t i = 3; i < columns.size() && row.modelPath.empty(); ++i) {
            if (EndsWithCpd(columns[i])) row.modelPath = std::string(columns[i]);
        }
        out.push_back(std::move(row));
    }
    if (out.empty()) {
        error = "no rows";
        return false;
    }
    return true;
}

bool CharacterTable::Load(const FileSystem& fs, std::string& error) {
    rows_.clear();
    std::size_t shards = 0;
    for (const std::string& path : fs.ListFolder(kTextData)) {
        const std::string key = core::NormalizeAssetPath(path);
        const std::string_view name = std::string_view(key).substr(key.rfind('\\') + 1);
        if (!key.starts_with(kTextData) || !name.starts_with("characterdata") || !key.ends_with(".txt")) continue;
        auto file = fs.Read(path);
        if (!file) continue;
        std::vector<ObjectRow> rows;
        std::string ignored; // an index file listing the shards has no object rows
        if (!DecodeObjectRows(core::DecodeTextFile(file->Bytes()), rows, ignored)) continue;
        ++shards;
        for (ObjectRow& row : rows) {
            // Later rows win, like the original's table load.
            if (row.inService || !rows_.contains(row.codename)) rows_[row.codename] = std::move(row);
        }
    }
    if (rows_.empty()) {
        error = "no characterdata rows under " + std::string(kTextData);
        return false;
    }
    (void)shards;
    // A clone row has no model: it wears its base row's (one hop in the data; follow a few).
    for (auto& [codename, row] : rows_) {
        const ObjectRow* base = &row;
        for (int hops = 0; base->modelPath.empty() && !base->linkedCodename.empty() && hops < 4; ++hops) {
            const auto it = rows_.find(base->linkedCodename);
            if (it == rows_.end() || &it->second == base) break;
            base = &it->second;
        }
        if (row.modelPath.empty() && !base->modelPath.empty()) row.modelPath = base->modelPath;
    }
    byId_.clear();
    for (const auto& [codename, row] : rows_) byId_[row.refObjId] = codename;
    return true;
}

std::string CharacterTable::ResolveModel(const FileSystem& fs, const ObjectRow& row) {
    if (row.modelPath.empty()) return {};
    if (fs.Exists(row.modelPath)) return row.modelPath;
    const std::string underRes = "res\\" + row.modelPath;
    return fs.Exists(underRes) ? underRes : std::string();
}

const ObjectRow* CharacterTable::Find(std::string_view codename) const {
    const auto it = rows_.find(std::string(codename));
    return it == rows_.end() ? nullptr : &it->second;
}

const ObjectRow* CharacterTable::Find(std::uint32_t refObjId) const {
    const auto it = byId_.find(refObjId);
    return it == byId_.end() ? nullptr : Find(it->second);
}

namespace {
std::uint32_t U32(std::string_view s) { return static_cast<std::uint32_t>(std::strtoul(std::string(s).c_str(), nullptr, 10)); }
std::string Path(std::string_view s) { return s.empty() || s == "xxx" || s == "XXX" ? std::string() : std::string(s); }
} // namespace

bool DecodeSkillRows(std::string_view text, std::vector<SkillRow>& out, std::string& error) {
    out.clear();
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        if (c.size() < 66 || c[1].empty()) return;
        SkillRow row;
        row.inService = c[0] == "1";
        row.id = U32(c[1]);
        row.group = U32(c[2]);
        row.codename = std::string(c[3]);
        row.baseName = std::string(c[5]);
        row.original = U32(c[6]);
        row.level = Int(c[7]);
        row.activity = Int(c[8]);
        row.chainNext = U32(c[9]);
        row.preparingMs = Int(c[11]);
        row.castingMs = Int(c[12]);
        row.durationMs = Int(c[13]);
        row.cooldownMs = Int(c[14]);
        row.coolTimeMs = Int(c[15]);
        row.flyingSpeed = Int(c[16]);
        row.interruptible = Int(c[17]);
        row.inTown = Int(c[20]);
        row.overlap = U32(c[18]);
        row.autoAttack = Int(c[19]);
        row.cooldownGroup = static_cast<int>((row.overlap >> 24) & 0xFF);
        row.range = Int(c[21]);
        if (c.size() > 67) row.aiType = Int(c[67]);
        if (c.size() > 66) row.aiChance = Int(c[66]);
        row.targetRequired = Int(c[22]) != 0;
        row.targetBuilding = Int(c[25]) != 0;
        row.targetSelf = Int(c[26]) != 0;
        row.targetAlly = Int(c[27]) != 0;
        row.targetParty = Int(c[28]) != 0;
        row.targetEnemyMonster = Int(c[29]) != 0;
        row.targetEnemyPlayer = Int(c[30]) != 0;
        row.targetNeutral = Int(c[31]) != 0;
        row.targetAny = Int(c[32]) != 0;
        row.targetDeadBody = Int(c[33]) != 0;
        row.masteries = {U32(c[34]), U32(c[35])};
        row.masteryLevels = {Int(c[36]), Int(c[37])};
        row.prerequisiteGroups = {U32(c[40]), U32(c[41]), U32(c[42])};
        row.prerequisiteLevels = {Int(c[43]), Int(c[44]), Int(c[45])};
        row.requiredStr = Int(c[38]);
        row.requiredInt = Int(c[39]);
        row.spCost = Int(c[46]);
        row.race = Int(c[47]);
        row.weaponKinds = {Int(c[50]), Int(c[51])};
        row.hpCost = Int(c[52]);
        row.mpCost = Int(c[53]);
        row.hpCostRatio = Int(c[54]);
        row.mpCostRatio = Int(c[55]);
        row.boardRow = Int(c[59]);
        row.boardColumn = Int(c[60]);
        row.iconPath = Path(c[61]);
        row.nameKey = std::string(c[62]);
        row.descKey = std::string(c[64]);
        row.studyKey = std::string(c[65]);
        constexpr auto tag = [](const char (&t)[5]) {
            return static_cast<std::uint32_t>(t[0]) << 24 | static_cast<std::uint32_t>(t[1]) << 16 |
                   static_cast<std::uint32_t>(t[2]) << 8 | static_cast<std::uint32_t>(t[3]);
        };
        const auto arg = [&](std::size_t i) {
            return i < c.size() ? static_cast<std::uint32_t>(std::strtoll(std::string(c[i]).c_str(), nullptr, 10)) : 0u;
        };
        bool nbuf = false;
        for (std::size_t i = 69; i < c.size(); ++i) {
            const std::uint32_t code = arg(i);
            if (code == tag("efta")) row.buffStatus = true;
            if (code == tag("dura") && !row.hasDuration) {
                row.hasDuration = true;
                row.buffDurationMs = arg(i + 1);
            }
            if (code == tag("getv")) {
                const std::uint32_t v = arg(i + 1);
                if (v == tag("RPBU") || v == tag("STDU") || v == tag("DTDR")) row.buffRider = true;
            }
            if (code == tag("hste") && row.speedPercent == 0) row.speedPercent = static_cast<int>(arg(i + 1));
            if (code == tag("bbuf")) row.buffSecondary = true;
            if (code == tag("nbuf")) nbuf = true;
            if (code == tag("cbuf")) row.cancelConfirm = true;
            if (code == tag("lnks")) row.cancelWithToken = true;
            if (code == 0x656672u && arg(i + 1) == 3) row.buffIndefinite = true; // 'efr' (three letters)
            if (code == 0x617474u && !row.attack) { // 'att'
                row.attack = true;
                row.attackKind = static_cast<int>(arg(i + 1));
                row.attackPower = static_cast<int>(arg(i + 2));
                row.attackMin = static_cast<int>(arg(i + 3));
                row.attackMax = static_cast<int>(arg(i + 4));
            }
            if (code == tag("cnsm")) row.consumesAmmo = true;
            if (code == 0x6D63u && row.attack && arg(i + 2) > 0 && arg(i + 2) < 16) row.hits = static_cast<int>(arg(i + 2)); // 'mc'

        }
        row.cancelBlocked = nbuf && !row.cancelWithToken;
        std::size_t last = std::min<std::size_t>(c.size(), 118);
        while (last > 69 && arg(last - 1) == 0) --last;
        for (std::size_t i = 69; i < last; ++i) row.params.push_back(arg(i));
        if (row.id != 0) out.push_back(std::move(row));
    });
    if (out.empty()) {
        error = "no skill rows";
        return false;
    }
    return true;
}

int SkillParamLength(std::uint32_t code) {
    // Places per tag, the tag included: how far a reader steps past each, measured to walk every row of the 1.188
    // skilldata to its end. 'efr' is 7 whatever its kind; 'ssou' ends the walk.
    struct Entry {
        std::string_view tag;
        int length;
    };
    static constexpr Entry kLengths[] = {
        {"ao", 1},   {"ar", 3},   {"bl", 6},   {"br", 3},   {"bu", 4},   {"ca", 4},   {"ck", 2},   {"cr", 3},   {"da", 2},
        {"dn", 5},   {"ds", 5},   {"er", 3},   {"es", 4},   {"fb", 3},   {"fe", 4},   {"fz", 3},   {"hr", 3},   {"kb", 3},
        {"ko", 3},   {"mc", 3},   {"my", 5},   {"ps", 4},   {"pw", 5},   {"rt", 4},   {"ru", 2},   {"se", 4},   {"sl", 4},
        {"st", 4},   {"tb", 5},   {"zb", 3},   {"att", 6},  {"dac", 2},  {"dar", 3},  {"dru", 3},  {"dtt", 3},  {"efr", 7},
        {"gdr", 2},  {"hpi", 3},  {"mom", 5},  {"mpi", 3},  {"msr", 3},  {"nmf", 1},  {"nmh", 1},  {"pao", 2},  {"skc", 4},
        {"spi", 2},  {"abir", 3}, {"abnb", 2}, {"agac", 3}, {"alcu", 2}, {"apau", 3}, {"apru", 3}, {"atca", 3}, {"bbuf", 1},
        {"bcnt", 2}, {"bgra", 3}, {"bldr", 5}, {"cabf", 2}, {"cbuf", 1}, {"chcr", 2}, {"chpi", 2}, {"chst", 2}, {"civi", 2},
        {"cmcr", 2}, {"cnsm", 4}, {"cshp", 7}, {"csit", 5}, {"csmd", 5}, {"csmp", 7}, {"cspd", 5}, {"cssr", 5}, {"csum", 2},
        {"cura", 2}, {"curl", 4}, {"curt", 3}, {"dcmp", 2}, {"dcri", 2}, {"defp", 4}, {"defr", 3}, {"dgmp", 2}, {"dmgr", 5},
        {"dmgt", 3}, {"dra2", 2}, {"dru2", 3}, {"dspl", 3}, {"dtnt", 3}, {"dttp", 3}, {"dura", 2}, {"efta", 1}, {"eshp", 1},
        {"esht", 3}, {"expi", 3}, {"expu", 3}, {"fitp", 2}, {"getv", 2}, {"heal", 5}, {"hel2", 5}, {"hide", 4}, {"hitm", 1},
        {"hntp", 1}, {"hst2", 2}, {"hst3", 2}, {"hste", 2}, {"hwir", 2}, {"hwit", 2}, {"iddm", 2}, {"inti", 3}, {"irgc", 3},
        {"iwky", 1}, {"jobs", 2}, {"lfst", 2}, {"lkag", 3}, {"lkcp", 2}, {"lkdd", 2}, {"lkdh", 4}, {"lkdr", 4}, {"lks2", 1},
        {"lnks", 5}, {"luck", 2}, {"mcap", 3}, {"mscc", 2}, {"msch", 3}, {"msid", 2}, {"mstc", 2}, {"mwdt", 2}, {"mwhh", 2},
        {"mwhs", 2}, {"mwmh", 2}, {"mwtt", 2}, {"nbuf", 1}, {"odar", 3}, {"onff", 3}, {"ovl2", 2}, {"pcdu", 3}, {"pchr", 2},
        {"pdm2", 2}, {"pdmg", 2}, {"php1", 1}, {"pls2", 5}, {"pmdg", 5}, {"pmdp", 5}, {"pmhp", 5}, {"pmmp", 5}, {"pmp1", 1},
        {"pola", 3}, {"puls", 2}, {"pwdt", 2}, {"pwtt", 2}, {"qest", 5}, {"rcur", 2}, {"real", 4}, {"reat", 3}, {"reqa", 2},
        {"reqc", 2}, {"reqi", 3}, {"reqn", 1}, {"resu", 3}, {"rhru", 3}, {"rmut", 2}, {"rpkt", 1}, {"rsag", 1}, {"saps", 3},
        {"scls", 2}, {"setv", 4}, {"slcc", 1}, {"smac", 3}, {"spda", 3}, {"ssou", 1}, {"stns", 4}, {"stri", 3}, {"summ", 6},
        {"tant", 3}, {"tcmd", 3}, {"tctc", 1}, {"tel2", 3}, {"tel3", 3}, {"tel4", 3}, {"tele", 3}, {"terd", 2}, {"thld", 2},
        {"thrd", 2}, {"tkss", 2}, {"tnt2", 3}, {"tpad", 2}, {"tpdd", 2}, {"tran", 3}, {"trap", 1},
    };
    static const std::unordered_map<std::uint32_t, int> byCode = [] {
        std::unordered_map<std::uint32_t, int> m;
        for (const Entry& e : kLengths) m.emplace(SkillTag(e.tag), e.length);
        return m;
    }();
    const auto it = byCode.find(code);
    return it == byCode.end() ? 0 : it->second;
}

SkillParam FindSkillParam(const SkillRow& row, std::uint32_t code, std::uint32_t efrKind) {
    SkillParam found;
    ForEachSkillParam(row, [&](std::uint32_t c, std::span<const std::uint32_t> args) {
        if (found.present || c != code) return;
        if (code == SkillTag("efr") && (args.empty() || args[0] != efrKind)) return;
        found.present = true;
        for (std::size_t i = 0; i < args.size() && i < found.args.size(); ++i) found.args[i] = args[i];
    });
    return found;
}

bool DecodeMasteryRows(std::string_view text, std::vector<MasteryRow>& out, std::string& error) {
    out.clear();
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        if (c.size() < 12 || U32(c[0]) == 0) return;
        MasteryRow row;
        row.id = U32(c[0]);
        row.nameKey = std::string(c[2]);
        row.descKey = std::string(c[4]);
        row.tabKey = std::string(c[5]);
        row.tab = Int(c[6]);
        row.kind = Int(c[7]);
        row.iconPath = Path(c[11]);
        if (c.size() > 12) row.focusIconPath = Path(c[12]);
        out.push_back(std::move(row));
    });
    if (out.empty()) {
        error = "no mastery rows";
        return false;
    }
    return true;
}

bool DecodeSkillGroupRows(std::string_view text, std::vector<SkillGroupRow>& out, std::string& error) {
    out.clear();
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        // A row is a mastery's board row (cols 2 and 4 numbers).
        if (c.size() < 7 || c[2].empty() || c[4].empty() || !std::isdigit(static_cast<unsigned char>(c[2][0])) ||
            !std::isdigit(static_cast<unsigned char>(c[4][0]))) {
            return;
        }
        SkillGroupRow row;
        row.id = U32(c[1]);
        row.mastery = U32(c[2]);
        row.boardRow = Int(c[4]);
        row.nameKey = std::string(c[5]);
        row.iconPath = Path(c[6]);
        out.push_back(std::move(row));
    });
    if (out.empty()) {
        error = "no skill group rows";
        return false;
    }
    return true;
}

bool SkillTable::Load(const FileSystem& fs, std::string& error) {
    skills_.clear();
    boards_.clear();
    byGroupLevel_.clear();
    byCode_.clear();
    std::vector<std::uint32_t> loaded; // the ids in the order they were read
    const auto take = [&](const std::string& text) {
        std::vector<SkillRow> rows;
        std::string ignored;
        if (!DecodeSkillRows(text, rows, ignored)) return;
        for (SkillRow& row : rows) {
            if (!row.inService && skills_.contains(row.id)) continue;
            loaded.push_back(row.id);
            byCode_[row.codename] = row.id;
            skills_[row.id] = std::move(row);
        }
    };
    // The 1.188 client's own table: the encrypted shards skilldataenc.txt lists (decrypts them; they
    // equal _RefSkill row for row). The plain shards (skilldata_<N>.txt) are an older
    // copy, 2,066 skills in service short (761 of the players' among them, the socket stones' too) and 100 rows
    // behind: read only when no encrypted shard is listed.
    bool encrypted = false;
    for (const std::string& path : ListedShards(fs, "skilldataenc.txt")) {
        if (const auto file = fs.Read(path)) {
            take(TextTable(*file));
            encrypted = true;
        }
    }
    if (!encrypted) ForEachShard(fs, "skilldata", [&](const std::string& text) { take(text); });
    if (skills_.empty()) {
        error = "no skilldata rows under " + std::string(kTextData);
        return false;
    }
    // A group's row at a level is the one learned and cast: in service, and the head of its chain. A chain's sub-rows
    // share their head's group and level (SKILL_CH_SPEAR_CHAIN_A_1S_01, _2S_01, _3S_01 are all group 210, level 1)
    // and were read after it: the last read had won, so from level 2 a chain was learned for no skill points and cast
    // as its last hit alone (the skill lab's list found it, 2026-11-15). Between rows alike, the later read wins.
    std::unordered_set<std::uint32_t> chained;
    for (const auto& [id, row] : skills_) {
        if (row.chainNext != 0) chained.insert(row.chainNext);
    }
    const auto rank = [&](const SkillRow& r) { return (r.inService ? 0 : 2) + (chained.contains(r.id) ? 1 : 0); };
    for (const std::uint32_t id : loaded) {
        const SkillRow& row = skills_.at(id);
        const std::uint64_t key = static_cast<std::uint64_t>(row.group) << 8 | static_cast<std::uint8_t>(row.level);
        const auto it = byGroupLevel_.find(key);
        if (it == byGroupLevel_.end() || rank(row) <= rank(skills_.at(it->second))) byGroupLevel_[key] = id;
    }
    std::string ignored;
    if (auto file = fs.Read(std::string(kTextData) + "skillmasterydata.txt")) {
        DecodeMasteryRows(core::DecodeTextFile(file->Bytes()), masteries_, ignored);
    }
    if (auto file = fs.Read(std::string(kTextData) + "skillgroup.txt")) {
        std::vector<SkillGroupRow> rows;
        if (DecodeSkillGroupRows(core::DecodeTextFile(file->Bytes()), rows, ignored)) {
            groups_ = std::move(rows);
        }
    }
    return true;
}

const SkillRow* SkillTable::Find(std::uint32_t id) const {
    const auto it = skills_.find(id);
    return it == skills_.end() ? nullptr : &it->second;
}

const SkillRow* SkillTable::Find(std::string_view codename) const {
    const auto it = byCode_.find(std::string(codename));
    return it == byCode_.end() ? nullptr : Find(it->second);
}

const MasteryRow* SkillTable::Mastery(std::uint32_t id) const {
    for (const MasteryRow& m : masteries_) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

const SkillGroupRow* SkillTable::Group(std::uint32_t id) const {
    for (const SkillGroupRow& g : groups_) {
        if (g.id == id) return &g;
    }
    return nullptr;
}

const SkillGroupRow* SkillTable::BoardRowGroup(std::uint32_t mastery, int boardRow) const {
    for (const SkillGroupRow& group : groups_) {
        if (group.mastery == mastery && group.boardRow == boardRow) return &group;
    }
    return nullptr;
}

const SkillRow* SkillTable::GroupLevel(std::uint32_t group, int level) const {
    const auto it = byGroupLevel_.find(static_cast<std::uint64_t>(group) << 8 | static_cast<std::uint8_t>(level));
    return it == byGroupLevel_.end() ? nullptr : Find(it->second);
}

std::vector<const SkillRow*> SkillTable::Board(std::uint32_t mastery) const {
    if (const auto cached = boards_.find(mastery); cached != boards_.end()) return cached->second;
    // Rows another row chains to are sub-rows of a combo, never on the board.
    std::unordered_set<std::uint32_t> chained;
    for (const auto& [id, row] : skills_) {
        if (row.chainNext != 0) chained.insert(row.chainNext);
    }
    std::unordered_map<std::uint32_t, const SkillRow*> byGroup;
    for (const auto& [id, row] : skills_) {
        if (!row.inService || row.masteries[0] != mastery || row.boardRow >= 255 || row.boardColumn >= 8 ||
            row.spCost <= 0 || chained.contains(id) || row.masteryLevels[0] > 90) {
            continue;
        }
        const SkillRow*& slot = byGroup[row.group];
        if (slot == nullptr || row.level < slot->level) slot = &row;
    }
    std::vector<const SkillRow*> out;
    for (const auto& [group, row] : byGroup) out.push_back(row);
    std::sort(out.begin(), out.end(), [](const SkillRow* a, const SkillRow* b) {
        return a->boardRow != b->boardRow ? a->boardRow < b->boardRow : a->boardColumn < b->boardColumn;
    });
    boards_[mastery] = out;
    return out;
}

namespace {

// Tab-separated rows in service (column 0 is 1).
std::vector<std::vector<std::string_view>> ServiceRows(std::string_view text) {
    std::vector<std::vector<std::string_view>> rows;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    for (std::size_t start = 0; start < text.size();) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.starts_with("//")) continue;
        std::vector<std::string_view> c;
        for (std::size_t pos = 0;;) {
            const std::size_t tab = line.find('\t', pos);
            c.push_back(line.substr(pos, tab == std::string_view::npos ? std::string_view::npos : tab - pos));
            if (tab == std::string_view::npos) break;
            pos = tab + 1;
        }
        if (c.size() > 1 && c[0] == "1") rows.push_back(std::move(c));
    }
    return rows;
}

std::uint32_t U32Of(std::string_view s) { return static_cast<std::uint32_t>(std::strtoul(std::string(s).c_str(), nullptr, 10)); }
float FloatOf(std::string_view s) { return std::strtof(std::string(s).c_str(), nullptr); }

} // namespace

bool DecodeTeleportGates(std::string_view text, std::vector<TeleportGate>& out) {
    out.clear();
    for (const auto& c : ServiceRows(text)) {
        if (c.size() < 51) continue;
        TeleportGate g;
        g.refObjId = U32Of(c[1]);
        g.codename = std::string(c[2]);
        g.nameKey = std::string(c[5]);
        for (int i = 0; i < 4; ++i) g.typeId[std::size_t(i)] = static_cast<std::uint8_t>(U32Of(c[std::size_t(9 + i)]));
        if (c.size() > 45) {
            g.region = static_cast<std::uint16_t>(std::strtol(std::string(c[41]).c_str(), nullptr, 10));
            g.x = FloatOf(c[43]);
            g.y = FloatOf(c[44]);
            g.z = FloatOf(c[45]);
        }
        g.height = FloatOf(c[49]);
        g.radius = FloatOf(c[50]);
        if (c.size() > 52 && c[52] != "xxx" && !c[52].empty()) g.model = std::string(c[52]);
        if (g.refObjId != 0) out.push_back(std::move(g));
    }
    return !out.empty();
}

bool DecodeTeleportPoints(std::string_view text, std::vector<TeleportPoint>& out) {
    out.clear();
    for (const auto& c : ServiceRows(text)) {
        if (c.size() < 11) continue;
        TeleportPoint p;
        p.id = U32Of(c[1]);
        p.codename = std::string(c[2]);
        p.ownerRefObjId = U32Of(c[3]);
        p.nameKey = std::string(c[4]);
        p.region = static_cast<std::uint16_t>(std::strtol(std::string(c[5]).c_str(), nullptr, 10));
        p.x = FloatOf(c[6]);
        p.y = FloatOf(c[7]);
        p.z = FloatOf(c[8]);
        p.recall = c[10] == "1";
        if (p.id != 0) out.push_back(std::move(p));
    }
    return !out.empty();
}

bool DecodeTeleportLinks(std::string_view text, std::vector<TeleportLink>& out) {
    out.clear();
    for (const auto& c : ServiceRows(text)) {
        if (c.size() < 4) continue;
        TeleportLink l;
        l.from = U32Of(c[1]);
        l.to = U32Of(c[2]);
        l.fee = U32Of(c[3]);
        // Five restriction triples (kind, a, b) in cols 6..20; kind 1 is the level.
        for (std::size_t k = 6, n = 0; k + 2 < c.size() && k <= 18; k += 3, ++n) {
            l.restrictions[n] = {static_cast<int>(U32Of(c[k])), U32Of(c[k + 1]), U32Of(c[k + 2])};
            if (U32Of(c[k]) != 1) continue;
            l.minLevel = static_cast<int>(U32Of(c[k + 1]));
            l.maxLevel = static_cast<int>(U32Of(c[k + 2]));
        }
        if (l.from != 0 && l.to != 0) out.push_back(l);
    }
    return !out.empty();
}

bool DecodeNpcPositions(std::string_view text, std::vector<NpcPosition>& out) {
    out.clear();
    ForEachLine(text, [&](std::string_view line) {
        const auto c = Columns(line);
        if (c.size() < 5) return;
        NpcPosition p;
        p.refObjId = std::uint32_t(Int(c[0]));
        p.region = std::uint16_t(std::int16_t(Int(c[1])));
        p.x = Float(c[2]);
        p.y = Float(c[3]);
        p.z = Float(c[4]);
        if (p.refObjId != 0) out.push_back(p);
    });
    return !out.empty();
}

bool TeleportTable::Load(const FileSystem& fs, std::string& error) {
    const auto read = [&](const char* name, auto decode, auto& into) {
        auto file = fs.Read(std::string(kTextData) + name);
        if (!file) {
            error = std::string(name) + " not found";
            return false;
        }
        decode(core::DecodeTextFile(file->Bytes()), into);
        return true;
    };
    return read("teleportbuilding.txt", DecodeTeleportGates, gates_) && read("teleportdata.txt", DecodeTeleportPoints, points_) &&
           read("teleportlink.txt", DecodeTeleportLinks, links_);
}

void TeleportTable::Decode(std::string_view gates, std::string_view points, std::string_view links) {
    DecodeTeleportGates(gates, gates_);
    DecodeTeleportPoints(points, points_);
    DecodeTeleportLinks(links, links_);
}

const TeleportGate* TeleportTable::Gate(std::uint32_t refObjId) const {
    for (const TeleportGate& g : gates_) {
        if (g.refObjId == refObjId) return &g;
    }
    return nullptr;
}

const TeleportPoint* TeleportTable::PointOf(std::uint32_t ownerRefObjId) const {
    for (const TeleportPoint& p : points_) {
        if (p.ownerRefObjId == ownerRefObjId) return &p;
    }
    return nullptr;
}

void AtStructTable::Decode(std::string_view text) {
    AtStructLook* current = nullptr;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line[0] != '#') continue;
        std::vector<std::string_view> c;
        for (std::size_t p = 0; p <= line.size();) {
            std::size_t tab = line.find('\t', p);
            if (tab == std::string_view::npos) tab = line.size();
            c.push_back(line.substr(p, tab - p));
            p = tab + 1;
        }
        if (c[0] == "#TARGET" && c.size() > 1) {
            current = &looks_[std::string(c[1])];
        } else if (c[0] == "#BSR" && c.size() > 2 && current != nullptr) {
            const int stage = Int(c[1]);
            if (stage >= 0 && stage < 3) current->models[stage] = std::string(c[2]);
        }
    }
}

const AtStructLook* AtStructTable::Find(const std::string& codename) const {
    const auto it = looks_.find(codename);
    return it != looks_.end() ? &it->second : nullptr;
}

std::vector<ActionRow> DecodeActionData(std::string_view text) {
    std::vector<ActionRow> rows;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.starts_with("//")) continue;
        std::vector<std::string_view> c;
        for (std::size_t p = 0; p <= line.size();) {
            std::size_t tab = line.find('\t', p);
            if (tab == std::string_view::npos) tab = line.size();
            c.push_back(line.substr(p, tab - p));
            p = tab + 1;
        }
        if (c.size() < 7 || Int(c[1]) == 0) continue;
        ActionRow r;
        r.id = U32(c[0]);
        r.textKey = std::string(c[3]);
        r.icon = std::string(c[4]);
        r.group = Int(c[5]);
        r.slot = Int(c[6]);
        rows.push_back(std::move(r));
    }
    return rows;
}

const TeleportPoint* TeleportTable::PointNamed(std::string_view codename) const {
    for (const TeleportPoint& p : points_) {
        if (p.codename == codename) return &p;
    }
    return nullptr;
}

const TeleportPoint* TeleportTable::Point(std::uint32_t id) const {
    for (const TeleportPoint& p : points_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

std::vector<const TeleportLink*> TeleportTable::LinksFrom(std::uint32_t pointId) const {
    std::vector<const TeleportLink*> out;
    for (const TeleportLink& l : links_) {
        if (l.from == pointId) out.push_back(&l);
    }
    return out;
}

} // namespace sr::assets
