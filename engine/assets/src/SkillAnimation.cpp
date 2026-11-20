#include "sr/assets/SkillAnimation.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "sr/assets/FileSystem.h"
#include "sr/core/Text.h"

namespace sr::assets {

namespace {
struct Named {
    const char* name;
    int state;
};
// ANI_* names and their .bsr states.
constexpr Named kStates[] = {
    {"STAND1", 0},       {"WALK", 1},         {"ATTACK1", 2},     {"DAMAGE1", 3},      {"DIE1", 4},
    {"ATTACK2", 5},      {"ATTREADY", 6},     {"RUN", 7},         {"DAMAGE2", 9},      {"SIT_DOWN", 13},
    {"SIT", 14},         {"STAND_UP", 15},    {"ATTACK3", 16},    {"ATTACK4", 17},     {"DIE2", 18},
    {"DEFENCE", 22},     {"DIE1_RM", 36},     {"DIE2_RM", 37},    {"PICK", 38},        {"DOWN", 62},
    {"DOWN_RM", 63},     {"DOWN_DAMAGE", 64}, {"DOWN_UP", 65},    {"DOWN_DIE", 66},    {"STUN", 79},
    {"ATTACK5", 183},    {"ATTACK6", 184},    {"ATTACK7", 185},   {"ATTACK8", 186},    {"HAMMER", 187},
    {"HANDLOOF", 188},   {"TROW", 189},       {"ATTACK9", 190},
};
} // namespace

int AnimationState(std::string_view name) {
    if (name.starts_with("ANI_")) name.remove_prefix(4);
    for (const Named& n : kStates) {
        if (name == n.name) return n.state;
    }
    const auto number = [&](std::string_view prefix) {
        return name.starts_with(prefix) ? std::atoi(std::string(name.substr(prefix.size())).c_str()) : 0;
    };
    // SKILL_1..10 26.., 11..20 68.., 21..40 101.., 41..100 123..; READY01..04 40..; WAIT01..04 91..
    if (const int n = number("SKILL_"); n > 0) {
        if (n <= 10) return 25 + n;
        if (n <= 20) return 57 + n;
        if (n <= 40) return 80 + n;
        if (n <= 100) return 82 + n;
    }
    if (const int n = number("READY"); n >= 1 && n <= 4) return 39 + n;
    if (const int n = number("WAIT"); n >= 1 && n <= 4) return 90 + n;
    if (const int n = number("EMOTION_"); n >= 1 && n <= 10) return 49 + n;
    return -1;
}

std::string AnimationStateName(int state) {
    for (const Named& n : kStates) {
        if (state == n.state) return std::string("ANI_") + n.name;
    }
    const auto named = [](const char* prefix, int n) { return std::string("ANI_") + prefix + (n < 10 && prefix[0] != 'S' ? "0" : "") + std::to_string(n); };
    if (state >= 26 && state <= 35) return named("SKILL_", state - 25);
    if (state >= 68 && state <= 77) return named("SKILL_", state - 57);
    if (state >= 101 && state <= 120) return named("SKILL_", state - 80);
    if (state >= 123 && state <= 182) return named("SKILL_", state - 82);
    if (state >= 40 && state <= 43) return named("READY", state - 39);
    if (state >= 91 && state <= 94) return named("WAIT", state - 90);
    if (state >= 50 && state <= 59) return std::string("ANI_EMOTION_") + std::to_string(state - 49);
    return std::to_string(state);
}

namespace {

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

std::string_view Trim(std::string_view t) {
    while (!t.empty() && (t.front() == ' ' || t.front() == '"')) t.remove_prefix(1);
    while (!t.empty() && (t.back() == ' ' || t.back() == '"')) t.remove_suffix(1);
    return t;
}

// A file column: empty for "xxx" / "none" / "0".
std::string FilePath(std::string_view v) {
    v = Trim(v);
    const std::string lower = Lower(v);
    return v.empty() || lower == "xxx" || lower == "none" || lower == "0" ? std::string() : std::string(v);
}

std::vector<std::string_view> Split(std::string_view list, char separator) {
    std::vector<std::string_view> out;
    for (std::size_t at = 0; at <= list.size();) {
        std::size_t next = list.find(separator, at);
        if (next == std::string_view::npos) next = list.size();
        out.push_back(Trim(list.substr(at, next - at)));
        at = next + 1;
    }
    return out;
}

float Number(std::string_view v) { return std::strtof(std::string(Trim(v)).c_str(), nullptr); }

std::array<float, 3> Vector(std::string_view v) {
    std::array<float, 3> out{};
    const auto parts = Split(v, ',');
    for (std::size_t i = 0; i < 3 && i < parts.size(); ++i) out[i] = Number(parts[i]);
    return out;
}

SkillEffectStage::Anchor Anchor(std::string_view token, std::string_view offset) {
    SkillEffectStage::Anchor a;
    token = Trim(token);
    const std::string lower = Lower(token);
    if (token == "*") {
        a.atHeight = true;
    } else if (!token.empty() && lower != "none" && lower != "xxx") {
        const bool marked = token.front() == '@' || token.front() == '#';
        a.keepTurn = !marked;
        a.inBone = token.front() == '#';
        a.bone = std::string(marked ? token.substr(1) : token);
    }
    a.offset = Vector(offset);
    return a;
}

std::string Upper(std::string_view s) {
    std::string out(Trim(s));
    for (char& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return out;
}

int Phase(std::string_view v) {
    static const char* const kNames[] = {"READY", "WAIT", "SHOT", "ACT_OS", "ACT_OL", "ACT_OE", "ACT_S", "ACT_L", "DEACT", "S_RETURN"};
    const std::string name = Upper(v);
    for (int i = 0; i < 10; ++i) {
        if (name == kNames[i]) return i;
    }
    return std::atoi(name.c_str());
}

// One skilleffectset row (28+ columns).
SkillEffectStage Stage(const std::vector<std::string_view>& c) {
    SkillEffectStage s;
    s.phase = Phase(c[2]);
    s.startEvent = std::atoi(std::string(Trim(c[3])).c_str());
    const std::string damage = Lower(Trim(c[4]));
    s.damageEvent = damage == "true" || damage == "1";
    const std::string scale = Lower(Trim(c[6]));
    s.scale = scale == "char_base" ? 1 : scale == "mob_base" ? 2 : 0;
    s.createCount = std::max(1, std::atoi(std::string(Trim(c[11])).c_str()));
    if (const auto fade = Split(c[12], ','); !fade.empty()) {
        s.fadeMs = Number(fade[0]);
        if (fade.size() > 1) s.arrivalFade = Number(fade[1]);
    }
    if (const auto param = Split(c[15], ','); !param.empty()) {
        s.arc = Number(param[0]);
        if (param.size() > 2) {
            s.tiltMin = Number(param[1]);
            s.tiltMax = Number(param[2]);
        }
    }
    s.slot = std::atoi(std::string(Trim(c[7])).c_str());
    s.trade = std::atoi(std::string(Trim(c[9])).c_str());
    s.kill = std::atoi(std::string(Trim(c[10])).c_str());
    if (const auto options = Split(c[16], ','); options.size() > 4) {
        s.lifeMs = Number(options[4]);
        if (Lower(options[0]) == "true") {
            s.rayDirection = Number(options[1]);
            s.rayDistance = Number(options[2]);
            s.raySpacing = Number(options[3]);
        }
    }
    s.arrival = FilePath(c[23]);
    s.action = Upper(c[13]);
    const auto move = Split(c[14], ',');
    if (!move.empty()) s.move = Upper(move[0]);
    if (move.size() > 1) s.moveDelayMs = Number(move[1]);
    if (move.size() > 2) s.startSpeed = Number(move[2]);
    if (move.size() > 3) s.endSpeed = Number(move[3]);
    const std::string dir = FilePath(c[17]), file = FilePath(c[18]);
    if (!file.empty()) s.object = dir.empty() || dir.ends_with('\\') || dir.ends_with('/') ? dir + file : dir + "\\" + file;
    if (const float turn = Number(c[24]); turn > 0.0f) {
        const float degrees = turn > 720.0f ? turn - 720.0f : turn > 360.0f ? turn - 360.0f : turn;
        s.rotateAxis = turn > 720.0f ? 'z' : turn > 360.0f ? 'x' : 'y';
        s.rotateRadians = degrees / 360.0f * 6.283185482025146f;
    }
    s.start = Anchor(c[19], c[20]);
    s.target = Anchor(c[21], c[22]);
    if (s.target.atHeight) { // a target "*" puts the start on the turned root too
        s.start.bone.clear();
        s.start.keepTurn = true;
    }
    for (std::string_view script : Split(c[25], ',')) {
        if (!script.empty() && Lower(script) != "none") s.scripts.emplace_back(script);
    }
    // Its sounds: the name under prim\snd\ (as the 1.188 client builds it).
    const auto sound = [&](std::size_t i) {
        const std::string name = i < c.size() ? FilePath(c[i]) : std::string();
        return name.empty() || name == "-" ? std::string() : "prim\\snd\\" + name;
    };
    s.soundBegin = sound(26);
    s.soundEnd = sound(27);
    return s;
}

} // namespace

bool DecodeSkillAnimations(std::string_view text, std::unordered_map<std::string, SkillAnimation>& out, std::string& error,
                           std::unordered_map<std::string, CharacterEffectInfo>* characters,
                           std::unordered_map<std::string, std::vector<SkillEffectStage>>* stageLists) {
    out.clear();
    std::unordered_map<std::string, std::vector<SkillEffectStage>> stages;
    int section = 0; // 0 other, 1 skillaniset, 2 skillaniset2, 3 skilleffectset, 4 characterinfo
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("#section")) {
            std::string name(line.substr(8));
            name.erase(0, name.find_first_not_of(" \t"));
            name = name.substr(0, name.find_first_of(" \t"));
            for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            section = name == "skillaniset2" ? 2 : name == "skillaniset" ? 1 : name == "skilleffectset" ? 3 : name == "characterinfo" ? 4 : 0;
            continue;
        }
        if (section == 0 || line.empty() || line.starts_with("//")) continue;
        std::vector<std::string_view> c;
        for (std::size_t at = 0; at <= line.size();) {
            std::size_t tab = line.find('\t', at);
            if (tab == std::string_view::npos) tab = line.size();
            c.push_back(line.substr(at, tab - at));
            at = tab + 1;
        }
        if (section == 3) {
            if (c.size() >= 28 && !Trim(c[1]).empty()) stages[std::string(Trim(c[1]))].push_back(Stage(c));
            continue;
        }
        if (section == 4) {
            if (characters != nullptr && c.size() >= 10 && !Trim(c[0]).empty()) {
                CharacterEffectInfo info;
                info.height = Number(c[2]);
                const std::string bone = FilePath(c[7]);
                info.anchorBone = bone;
                info.anchorOffset = Vector(c[8]);
                if (const std::string blood = FilePath(c[9]); !blood.empty()) info.blood = "hiteffect\\" + blood + ".efp";
                if (c.size() > 11) info.bringsRain = (std::atoi(std::string(Trim(c[11].substr(0, c[11].find(',')))).c_str()) & 1) != 0;
                (*characters)[std::string(Trim(c[0]))] = std::move(info);
            }
            continue;
        }
        if (section == 2) {
            if (c.empty() || std::atoi(std::string(c[0]).c_str()) == 0) continue; // out of service
            c.erase(c.begin());
        }
        if (c.size() < 9 || c[1].empty()) continue;
        SkillAnimation row;
        row.group = std::string(c[5]);
        for (char& ch : row.group) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const auto states = [](std::string_view list, std::vector<int>& to) {
            for (std::size_t at = 0; at <= list.size();) {
                std::size_t comma = list.find(',', at);
                if (comma == std::string_view::npos) comma = list.size();
                std::string_view token = list.substr(at, comma - at);
                while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
                while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
                if (const int state = AnimationState(token); state >= 0) to.push_back(state);
                at = comma + 1;
            }
        };
        states(c[6], row.ready);
        states(c[7], row.wait);
        states(c[8], row.shot);
        const auto column = [&](std::size_t i) { return i < c.size() ? FilePath(c[i]) : std::string(); };
        row.priority = c.size() > 2 ? std::atoi(std::string(Trim(c[2])).c_str()) : 0;
        row.overlap = !(c.size() > 3 && (Trim(c[3]) == "FALSE" || Trim(c[3]) == "false"));
        row.hideWeapon = c.size() > 4 ? std::uint8_t(std::clamp(std::atoi(std::string(Trim(c[4])).c_str()), 0, 255)) : std::uint8_t(0);
        row.defenseEffect = column(12);
        row.damageEffect = column(13);
        row.criticalEffect = column(18);
        row.arrowTrail = column(19);
        row.arrowForce = column(20);
        if (std::string effect = column(22); effect.size() > 4 && effect.ends_with(".efp")) row.objectEffect = std::move(effect);
        if (c.size() > 23) {
            const std::string_view mode = Trim(c[23]);
            if (mode == "Yaw") row.spineAxis = {1, 0, 0};
            if (mode == "YawR") row.spineAxis = {-1, 0, 0};
            if (mode == "Pitch") row.spineAxis = {0, 1, 0};
            if (mode == "PitchR") row.spineAxis = {0, -1, 0};
            if (mode == "Roll") row.spineAxis = {0, 0, -1};
            if (mode == "RollR") row.spineAxis = {0, 0, 1};
        }
        if (std::string texture = column(17); texture.size() > 4 && texture.ends_with(".ddj")) {
            row.trailTexture = "textures\\" + texture;
            row.trailMs = c.size() > 14 ? std::atoi(std::string(Trim(c[14])).c_str()) : 0;
            if (c.size() > 15) {
                std::array<int, 4> argb{};
                std::size_t n = 0;
                for (std::string_view v : Split(c[15], ',')) {
                    if (n < 4) argb[n++] = std::clamp(std::atoi(std::string(Trim(v)).c_str()), 0, 255);
                }
                if (n == 4 && (argb[0] | argb[1] | argb[2] | argb[3]) != 0) {
                    row.trailColour = std::uint32_t(argb[1]) | std::uint32_t(argb[2]) << 8 | std::uint32_t(argb[3]) << 16 | std::uint32_t(argb[0]) << 24;
                }
            }
        }
        out[std::string(c[1])] = std::move(row);
    }
    // Each row's stages: listed under its name, or its name + "_01".
    for (auto& [name, row] : out) {
        auto it = stages.find(name);
        if (it == stages.end()) it = stages.find(name + "_01");
        if (it != stages.end()) row.stages = it->second;
    }
    if (stageLists != nullptr) *stageLists = std::move(stages);
    if (out.empty()) {
        error = "no skillaniset rows";
        return false;
    }
    return true;
}

bool SkillAnimationTable::Load(const FileSystem& fs, std::string& error) {
    auto file = fs.Read("server_dep\\silkroad\\textdata\\skilleffect.txt");
    if (!file) {
        error = "skilleffect.txt not found";
        return false;
    }
    return DecodeSkillAnimations(core::DecodeTextFile(file->Bytes()), rows_, error, &characters_, &stages_);
}

const std::vector<SkillEffectStage>* SkillAnimationTable::Stages(std::string_view name) const {
    auto it = stages_.find(std::string(name));
    if (it == stages_.end()) it = stages_.find(std::string(name) + "_01");
    return it == stages_.end() ? nullptr : &it->second;
}

const CharacterEffectInfo* SkillAnimationTable::Character(std::string_view codename) const {
    const auto it = characters_.find(std::string(codename));
    return it == characters_.end() ? nullptr : &it->second;
}

const SkillAnimation* SkillAnimationTable::Find(std::string_view baseName) const {
    const auto it = rows_.find(std::string(baseName));
    return it == rows_.end() ? nullptr : &it->second;
}

} // namespace sr::assets
