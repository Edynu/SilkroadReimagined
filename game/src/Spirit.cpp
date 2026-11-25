#include "sr/game/Spirit.h"

#include <algorithm>
#include <charconv>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Text.h"
#include "sr/game/Items.h"
#include "sr/game/Mall.h"
#include "sr/game/TimedServices.h"

namespace sr::game {

namespace {

constexpr std::string_view kAbilityFile = "server_dep\\silkroad\\textdata\\refabilitybyitemoptleveldata.txt";
constexpr std::string_view kOptionFile = "server_dep\\silkroad\\textdata\\refmagicoptbyitemoptleveldata.txt";
constexpr std::string_view kSkillFile = "server_dep\\silkroad\\textdata\\refskillbyitemoptleveldata.txt";

std::string_view Trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\r' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool Number(std::string_view s, long long& out) {
    s = Trimmed(s);
    if (s.starts_with("\xEF\xBB\xBF")) s.remove_prefix(3); // a BOM left on the first cell
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

std::vector<std::string_view> Cells(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = line.find('\t', start);
        out.push_back(line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) return out;
        start = end + 1;
    }
}

// Each data line's numbers (its first `need` cells numbers; comments, headers and short lines skipped).
template <typename F>
void Lines(std::string_view text, std::size_t need, F&& row) {
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = Trimmed(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line.starts_with("//")) continue;
        const std::vector<std::string_view> c = Cells(line);
        if (c.size() < need) continue;
        std::vector<long long> n(need);
        bool ok = true;
        for (std::size_t i = 0; i < need && ok; ++i) ok = Number(c[i], n[i]);
        if (ok) row(n);
    }
}

} // namespace

bool DecodeSpiritAbilities(std::string_view text, std::vector<SpiritAbilityRow>& out, std::string& error) {
    out.clear();
    Lines(text, 4, [&](const std::vector<long long>& n) {
        if (n[0] != 1) return; // Service
        out.push_back({int(n[1]), std::uint32_t(n[2]), int(n[3])});
    });
    if (out.empty()) error = "no Devil's Spirit rows (refabilitybyitemoptleveldata.txt)";
    return !out.empty();
}

bool DecodeSpiritOptions(std::string_view text, std::vector<SpiritOptionRow>& out, std::string& error) {
    out.clear();
    Lines(text, 4, [&](const std::vector<long long>& n) { out.push_back({int(n[0]), std::uint32_t(n[1]), std::uint32_t(n[2]), int(n[3]), {}}); });
    // Each row's tooltip text key (its fifth cell), in the same order as the rows read.
    std::size_t at = 0, start = 0;
    while (start < text.size() && at < out.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = Trimmed(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line.starts_with("//")) continue;
        const std::vector<std::string_view> c = Cells(line);
        if (c.size() < 4) continue;
        long long first = 0;
        if (!Number(c[0], first)) continue;
        if (c.size() > 4) out[at].tooltip = std::string(Trimmed(c[4]));
        ++at;
    }
    if (out.empty()) error = "no Devil's Spirit option rows (refmagicoptbyitemoptleveldata.txt)";
    return !out.empty();
}

bool DecodeSpiritSkills(std::string_view text, std::vector<SpiritSkillRow>& out, std::string& error) {
    out.clear();
    Lines(text, 2, [&](const std::vector<long long>& n) { out.push_back({int(n[0]), std::uint32_t(n[1])}); });
    if (out.empty()) error = "no Devil's Spirit skill rows (refskillbyitemoptleveldata.txt)";
    return !out.empty();
}

const SpiritLevel* SpiritTable::Find(std::uint32_t item, int plus) const {
    const auto it = levels_.find({item, plus});
    return it != levels_.end() ? &it->second : nullptr;
}

SpiritTable MakeSpiritTable(const std::vector<SpiritAbilityRow>& abilities, const std::vector<SpiritOptionRow>& options,
                            const std::vector<SpiritSkillRow>& skills, const std::vector<assets::MagicOptionRow>& magicOptions) {
    SpiritTable table;
    for (const SpiritAbilityRow& a : abilities) {
        SpiritLevel level;
        for (const SpiritOptionRow& o : options) {
            if (o.link != a.id) continue;
            const assets::MagicOptionRow* m = FindOption(magicOptions, o.option);
            if (m != nullptr) level.options.push_back({o.option, m->attribute, o.value, o.tooltipType, o.tooltip});
        }
        for (const SpiritSkillRow& s : skills) {
            if (s.link == a.id) level.skills.push_back(s.skill);
        }
        table.Add(a.item, a.plus, std::move(level));
    }
    return table;
}

bool LoadSpiritTable(const assets::FileSystem& fs, const std::vector<assets::MagicOptionRow>& magicOptions, SpiritTable& out, std::string& error) {
    const auto read = [&](std::string_view path, std::string& text) {
        const auto file = fs.Read(path);
        if (!file) {
            error = std::string(path.substr(path.rfind('\\') + 1)) + " is missing";
            return false;
        }
        text = core::DecodeTextFile(file->Bytes());
        return true;
    };
    std::string a, o, s;
    if (!read(kAbilityFile, a) || !read(kOptionFile, o) || !read(kSkillFile, s)) return false;
    std::vector<SpiritAbilityRow> abilities;
    std::vector<SpiritOptionRow> options;
    std::vector<SpiritSkillRow> skills;
    if (!DecodeSpiritAbilities(a, abilities, error) || !DecodeSpiritOptions(o, options, error) || !DecodeSpiritSkills(s, skills, error)) return false;
    out = MakeSpiritTable(abilities, options, skills, magicOptions);
    return true;
}

SpiritGift SpiritGiftOf(const SpiritTable& table, const assets::ItemRow& row, const ItemInstance& item,
                        const std::vector<assets::MagicOptionRow>* magicOptions, std::int64_t now) {
    SpiritGift g;
    if (!IsDevilSpirit(row)) return g;
    g.plus = item.plus;
    // Awake while its data word, the awakening's end, is after now; asleep, the 10-s check took its
    // contributions off.
    g.awake = DevilSpiritAwake(item.durability, now);
    if (!g.awake) return g;
    // Its (item, OptLevel) row (the plus alone: an advanced elixir's never goes on a spirit); none: nothing at all.
    const SpiritLevel* level = table.Find(item.refId, item.plus);
    if (level == nullptr) return g;
    g.found = true;
    // each row's option by its code, the row's value;
    // brna with the spirit's own block, 0 (its row has none).
    for (const SpiritLevel::Option& o : level->options) {
        const std::vector<StatChange> c = BlueChanges(o.code, o.value);
        g.changes.insert(g.changes.end(), c.begin(), c.end());
    }
    // Then its whites (none: every white column of the eighteen rows is 0) and its own blues: an UMDU stone's, say. A
    // later change of a stat and kind replaces the earlier (the same source).
    if (magicOptions != nullptr) {
        for (const ItemInstance::Option& o : item.options) {
            if (const assets::MagicOptionRow* m = FindOption(*magicOptions, o.id)) {
                const std::vector<StatChange> c = BlueChanges(m->attribute, o.value);
                g.changes.insert(g.changes.end(), c.begin(), c.end());
            }
        }
    }
    g.skills = level->skills;
    return g;
}

} // namespace sr::game
