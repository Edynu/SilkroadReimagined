#include "sr/audio/SoundTables.h"

#include <algorithm>
#include <charconv>

namespace sr::audio {

namespace {

constexpr std::string_view kSpace = " \t\r\n";

std::string_view Trim(std::string_view s) {
    const std::size_t a = s.find_first_not_of(kSpace);
    if (a == std::string_view::npos) return {};
    return s.substr(a, s.find_last_not_of(kSpace) - a + 1);
}

// Calls `line` for each line of `text` (without its end).
template <typename F>
void Lines(std::string_view text, F&& line) {
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        line(text.substr(0, end));
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
}

// The line split at tabs.
std::vector<std::string_view> Fields(std::string_view line, char separator = '\t') {
    std::vector<std::string_view> out;
    while (true) {
        const std::size_t end = line.find(separator);
        out.push_back(line.substr(0, end));
        if (end == std::string_view::npos) break;
        line.remove_prefix(end + 1);
    }
    return out;
}

// `-` and blanks are empty.
std::string Field(std::string_view s) {
    s = Trim(s);
    return s == "-" ? std::string() : std::string(s);
}

float Number(std::string_view s, float fallback) {
    s = Trim(s);
    float v = fallback;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc() ? v : fallback;
}

// The text between the first pair of double quotes (empty when none).
std::string_view Quoted(std::string_view s) {
    const std::size_t a = s.find('"');
    if (a == std::string_view::npos) return {};
    const std::size_t b = s.find('"', a + 1);
    return b == std::string_view::npos ? std::string_view() : s.substr(a + 1, b - a - 1);
}

bool Matches(std::string_view want, const std::string& have) { return want.empty() || have.empty() || want == have; }

} // namespace

bool EffectSoundTable::Parse(std::string_view text, std::string& error) {
    rows_.clear();
    bySkill_.clear();
    byObject_.clear();
    Lines(text, [&](std::string_view line) {
        if (Trim(line).starts_with("//")) return;
        const std::vector<std::string_view> f = Fields(line);
        if (f.size() < 11) return;
        EffectSoundRow row;
        row.object = Field(f[1]);
        row.handle = Field(f[2]);
        if (row.object.empty() || row.handle.empty()) return;
        row.skill = Field(f[3]);
        for (int i = 0; i < 3; ++i) row.events[std::size_t(i)] = Field(f[std::size_t(4 + i)]);
        std::string folder = Field(f[8]);
        const std::string file = Field(f[9]);
        // Most rows end the folder with '\'; 1.188's event rows (Event\Summer_ghost, ...) do not.
        if (!folder.empty() && folder.back() != '\\' && folder.back() != '/') folder += '\\';
        if (!file.empty()) row.path = "prim\\snd\\" + folder + file;
        row.volume = std::clamp(Number(f[10], 100.0f), 0.0f, 100.0f) / 100.0f;
        const auto index = std::uint32_t(rows_.size());
        if (!row.skill.empty()) {
            bySkill_[row.skill].push_back(index);
        } else {
            byObject_[row.object].push_back(index);
        }
        rows_.push_back(std::move(row));
    });
    if (rows_.empty()) {
        error = "effectsound.txt: no rows";
        return false;
    }
    return true;
}

std::vector<const EffectSoundRow*> EffectSoundTable::Find(const Key& key) const {
    std::vector<const EffectSoundRow*> out;
    const auto add = [&](const std::vector<std::uint32_t>& indices) {
        for (const std::uint32_t i : indices) {
            const EffectSoundRow& r = rows_[i];
            if (r.path.empty() || r.handle != key.handle || (!key.object.empty() && r.object != key.object)) continue;
            if (!Matches(key.events[0], r.events[0]) || !Matches(key.events[1], r.events[1]) || !Matches(key.events[2], r.events[2])) continue;
            out.push_back(&r);
        }
    };
    if (!key.skill.empty()) {
        if (const auto it = bySkill_.find(std::string(key.skill)); it != bySkill_.end()) add(it->second);
    } else if (!key.object.empty()) {
        if (const auto it = byObject_.find(std::string(key.object)); it != byObject_.end()) add(it->second);
    } else {
        for (const auto& [object, indices] : byObject_) add(indices);
    }
    return out;
}

const EffectSoundRow* EffectSoundTable::Pick(const Key& key, std::minstd_rand& random) const {
    const std::vector<const EffectSoundRow*> rows = Find(key);
    if (rows.empty()) return nullptr;
    return rows[std::uniform_int_distribution<std::size_t>(0, rows.size() - 1)(random)];
}

bool AreaSoundTable::Parse(std::string_view text, std::string& error) {
    areas_.clear();
    int section = -1; // 0 day, 1 night (the first and the second <2> of an area)
    Lines(text, [&](std::string_view raw) {
        std::string_view line = Trim(raw);
        if (line.empty() || line.starts_with("//")) return;
        if (line.starts_with("<1>")) {
            AreaSounds area;
            area.name = std::string(Trim(line.substr(3)));
            areas_.push_back(std::move(area));
            section = -1;
            return;
        }
        if (areas_.empty()) return;
        AreaSounds& area = areas_.back();
        if (line.starts_with("<2>")) {
            section = std::min(section + 1, 1);
            return;
        }
        if (line.starts_with("<3>")) {
            if (section < 0) return;
            const std::string_view file = Trim(Quoted(line));
            if (file.empty()) return;
            AreaSounds::Ambient a;
            a.path = "prim\\snd\\env\\" + std::string(file);
            // "min~max" after the closing quote.
            const std::string_view rest = Trim(line.substr(line.find('"', line.find('"') + 1) + 1));
            const std::size_t tilde = rest.find('~');
            if (tilde != std::string_view::npos) {
                a.minSeconds = Number(rest.substr(0, tilde), 0.0f);
                a.maxSeconds = std::max(a.minSeconds, Number(rest.substr(tilde + 1), 0.0f));
            }
            (section == 0 ? area.day : area.night).push_back(std::move(a));
            return;
        }
        if (area.music.empty() && section < 0) area.music = std::string(Trim(Quoted(line)));
    });
    if (areas_.empty()) {
        error = "effectenvsnd.txt: no areas";
        return false;
    }
    return true;
}

const AreaSounds* AreaSoundTable::Find(std::string_view name) const {
    for (const AreaSounds& a : areas_) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

} // namespace sr::audio
