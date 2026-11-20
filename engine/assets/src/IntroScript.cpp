#include "sr/assets/IntroScript.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>

#include "sr/assets/Terrain.h"

namespace sr::assets {

namespace {

std::vector<std::string_view> Tokens(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t' || line[pos] == '\r')) ++pos;
        const std::size_t start = pos;
        while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t' && line[pos] != '\r') ++pos;
        if (pos > start) tokens.push_back(line.substr(start, pos - start));
    }
    return tokens;
}

bool Number(std::string_view token, float& value) {
    std::string text(token);
    if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) text.pop_back();
    char* end = nullptr;
    value = std::strtof(text.c_str(), &end);
    return !text.empty() && end == text.c_str() + text.size() && std::isfinite(value);
}

float CatmullRom(float t, float a, float b, float c, float d) {
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * (2.0f * b + (c - a) * t + (2.0f * a - 5.0f * b + 4.0f * c - d) * t2 + (-a + 3.0f * b - 3.0f * c + d) * t3);
}

// Unwraps each angle to within pi of the one before, so the spline turns the short way.
float CatmullRomAngle(float t, float a, float b, float c, float d) {
    constexpr float pi = std::numbers::pi_v<float>;
    const auto near = [](float previous, float value) {
        while (value - previous > pi) value -= 2.0f * pi;
        while (value - previous < -pi) value += 2.0f * pi;
        return value;
    };
    b = near(a, b);
    c = near(b, c);
    d = near(c, d);
    return CatmullRom(t, a, b, c, d);
}

} // namespace

bool DecodeIntroCamera(std::string_view text, std::vector<CameraKey>& out, std::string& error) {
    out.clear();
    std::size_t lineNumber = 0, start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(start, end - start);
        start = end + 1;
        ++lineNumber;
        const auto tokens = Tokens(line);
        if (tokens.size() < 2 || tokens[1] != "S_CameraInsert") continue;
        float v[10];
        bool ok = tokens.size() >= 12;
        for (int i = 0; ok && i < 10; ++i) ok = Number(tokens[static_cast<std::size_t>(2 + i)], v[i]);
        if (!ok) {
            error = "line " + std::to_string(lineNumber) + ": bad S_CameraInsert";
            return false;
        }
        CameraKey key;
        key.time = v[0];
        key.sectorX = static_cast<int>(v[1]);
        key.sectorY = static_cast<int>(v[2]);
        for (int i = 0; i < 3; ++i) {
            key.position[i] = v[3 + i];
            key.rotation[i] = v[6 + i];
        }
        key.distance = v[9];
        if (!out.empty() && key.time <= out.back().time) {
            error = "line " + std::to_string(lineNumber) + ": camera key times must increase";
            return false;
        }
        out.push_back(key);
    }
    if (out.empty()) {
        error = "no S_CameraInsert keys";
        return false;
    }
    return true;
}

CameraKey SampleCameraTrack(std::span<const CameraKey> keys, float time) {
    if (keys.empty()) return {};
    if (time <= keys.front().time) return keys.front();
    if (time >= keys.back().time) return keys.back();
    std::size_t next = 1;
    while (keys[next].time < time) ++next;
    const CameraKey& a = keys[next >= 2 ? next - 2 : 0];
    const CameraKey& b = keys[next - 1];
    const CameraKey& c = keys[next];
    const CameraKey& d = keys[next + 1 < keys.size() ? next + 1 : next];
    const float t = (time - b.time) / (c.time - b.time);

    // Positions in b's sector.
    const auto shifted = [&](const CameraKey& k, int axis) {
        if (axis == 0) return k.position[0] + static_cast<float>(k.sectorX - b.sectorX) * kRegionSize;
        if (axis == 2) return k.position[2] + static_cast<float>(k.sectorY - b.sectorY) * kRegionSize;
        return k.position[axis];
    };
    CameraKey out = b;
    out.time = time;
    for (int i = 0; i < 3; ++i) {
        out.position[i] = CatmullRom(t, shifted(a, i), shifted(b, i), shifted(c, i), shifted(d, i));
        out.rotation[i] = CatmullRomAngle(t, a.rotation[i], b.rotation[i], c.rotation[i], d.rotation[i]);
    }
    out.distance = CatmullRom(t, a.distance, b.distance, c.distance, d.distance);
    return out;
}

float SceneCommand::Number(std::size_t i, float otherwise) const {
    float v = 0.0f;
    return i < args.size() && ::sr::assets::Number(args[i], v) ? v : otherwise;
}

bool DecodeSceneScript(std::string_view text, std::vector<SceneCommand>& out, std::string& error) {
    out.clear();
    std::size_t lineNumber = 0, start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        ++lineNumber;
        if (const std::size_t comment = line.find("//"); comment != std::string_view::npos) line = line.substr(0, comment);
        // Its words; a quoted one whole (its quotes off).
        std::vector<std::string> words;
        for (std::size_t i = 0; i < line.size();) {
            const char c = line[i];
            if (c == ' ' || c == '\t' || c == '\r') {
                ++i;
                continue;
            }
            if (c == '"') {
                const std::size_t close = line.find('"', i + 1);
                const std::size_t stop = close == std::string_view::npos ? line.size() : close;
                words.emplace_back(line.substr(i + 1, stop - i - 1));
                i = stop + 1;
                continue;
            }
            const std::size_t from = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
            words.emplace_back(line.substr(from, i - from));
        }
        if (words.empty()) continue;
        SceneCommand command;
        if (words.size() < 2 || !Number(words[0], command.time)) {
            error = "line " + std::to_string(lineNumber) + ": no time";
            return false;
        }
        command.verb = std::move(words[1]);
        command.args.assign(std::make_move_iterator(words.begin() + 2), std::make_move_iterator(words.end()));
        out.push_back(std::move(command));
    }
    std::stable_sort(out.begin(), out.end(), [](const SceneCommand& a, const SceneCommand& b) { return a.time < b.time; });
    if (out.empty()) {
        error = "no commands";
        return false;
    }
    return true;
}

bool SceneCameraKey(const SceneCommand& c, bool timed, CameraKey& out) {
    const std::size_t first = timed ? 1 : 0;
    if (c.args.size() < first + 9) return false;
    float v[9];
    for (std::size_t i = 0; i < 9; ++i) {
        if (!Number(c.args[first + i], v[i])) return false;
    }
    out = {};
    out.time = timed ? c.Number(0) : 0.0f;
    out.sectorX = static_cast<int>(v[0]);
    out.sectorY = static_cast<int>(v[1]);
    for (int i = 0; i < 3; ++i) {
        out.position[i] = v[2 + i];
        out.rotation[i] = v[5 + i];
    }
    out.distance = v[8];
    return true;
}

} // namespace sr::assets
