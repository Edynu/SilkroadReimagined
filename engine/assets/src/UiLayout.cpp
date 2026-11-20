#include "sr/assets/UiLayout.h"

#include "sr/assets/FileSystem.h"
#include "sr/core/Text.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace sr::assets {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> SplitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

bool IsIdentifierChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// "1, 2.5f, 3" -> numbers; a trailing 'f' and blanks are allowed, bad parts read as 0.
std::vector<float> Numbers(std::string_view raw) {
    std::vector<float> out;
    std::size_t start = 0;
    while (start <= raw.size()) {
        std::size_t end = raw.find(',', start);
        if (end == std::string_view::npos) end = raw.size();
        std::string part(Trim(raw.substr(start, end - start)));
        if (!part.empty() && (part.back() == 'f' || part.back() == 'F')) part.pop_back();
        out.push_back(part.empty() ? 0.0f : std::strtof(part.c_str(), nullptr));
        start = end + 1;
    }
    return out;
}

UiLayoutRect Rect(std::string_view raw) {
    const auto n = Numbers(raw);
    UiLayoutRect r;
    if (n.size() > 0) r.x = n[0];
    if (n.size() > 1) r.y = n[1];
    if (n.size() > 2) r.w = n[2];
    if (n.size() > 3) r.h = n[3];
    return r;
}

int Integer(std::string_view raw) { return static_cast<int>(std::strtol(std::string(Trim(raw)).c_str(), nullptr, 10)); }

std::uint8_t Channel(float v) { return static_cast<std::uint8_t>(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v); }

void AssignProperty(UiLayoutNode& node, const std::string& key, std::string_view value) {
    node.properties[key] = std::string(value);
    if (key == "ID") {
        node.id = Integer(value);
    } else if (key == "Rect") {
        node.rect = Rect(value);
    } else if (key == "ClientRect") {
        node.clientRect = Rect(value);
        node.hasClientRect = true;
    } else if (key == "Style") {
        node.style = Integer(value);
    } else if (key == "Text") {
        node.text = std::string(value);
    } else if (key == "DDJ") {
        node.ddj = std::string(value);
    } else if (key == "FontColor") {
        const auto n = Numbers(value); // a, r, g, b
        node.fontColor = {Channel(n.size() > 0 ? n[0] : 255.0f), Channel(n.size() > 1 ? n[1] : 255.0f),
                          Channel(n.size() > 2 ? n[2] : 255.0f), Channel(n.size() > 3 ? n[3] : 255.0f)};
        node.hasFontColor = true;
    } else if (key == "FontIndex") {
        node.fontIndex = Integer(value);
    } else if (key == "HAlign") {
        node.hAlign = Integer(value);
    } else if (key == "VAlign") {
        node.vAlign = Integer(value);
    } else if (key == "SubSection") {
        node.subSection = std::string(value);
    } else if (key == "UV_LT" || key == "UV_RB") {
        const auto n = Numbers(value);
        const int at = key == "UV_LT" ? 0 : 2;
        if (n.size() >= 2) {
            node.uv[at] = n[0];
            node.uv[at + 1] = n[1];
        }
    }
}

} // namespace

const UiLayoutNode* UiLayout::Find(std::string_view name) const {
    for (const UiLayoutSection& section : sections) {
        for (const UiLayoutNode& node : section.nodes) {
            if (node.name == name) return &node;
        }
    }
    return nullptr;
}

UiDefines DecodeUiDefines(std::string_view text) {
    UiDefines defines;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    for (std::string_view line : SplitLines(text)) {
        line = Trim(line.substr(0, line.find("//")));
        std::size_t n = 0;
        while (n < line.size() && IsIdentifierChar(line[n])) ++n;
        if (n > 0 && !(line[0] >= '0' && line[0] <= '9')) defines.emplace(line.substr(0, n));
    }
    return defines;
}

bool DecodeUiLayout(std::string_view text, const UiDefines& defines, UiLayout& out, std::string& error) {
    out = UiLayout{};
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);

    struct Branch {
        bool parentActive;
        bool active;
        bool hasElse;
        std::string key;
    };
    std::vector<Branch> branches;
    UiLayoutSection* section = nullptr;
    UiLayoutNode* node = nullptr;
    const auto lines = SplitLines(text);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string_view line = Trim(lines[index]);
        const std::string where = "line " + std::to_string(index + 1);

        // Preprocessor.
        if (line.starts_with("#ifdef")) {
            const bool parentActive = branches.empty() || branches.back().active;
            const std::string key(Trim(line.substr(6)));
            branches.push_back({parentActive, parentActive && defines.contains(key), false, key});
            continue;
        }
        if (line == "#else") {
            if (branches.empty() || branches.back().hasElse) {
                error = where + ": unexpected #else";
                return false;
            }
            Branch& b = branches.back();
            b.active = b.parentActive && !defines.contains(b.key);
            b.hasElse = true;
            continue;
        }
        if (line == "#endif") {
            if (branches.empty()) {
                error = where + ": #endif without #ifdef";
                return false;
            }
            branches.pop_back();
            continue;
        }
        if (!branches.empty() && !branches.back().active) continue;

        if (line.empty() || line == "Interface Text" || line == "{" || line.starts_with("//")) continue;

        // Section = Name, "x,y,w,h", "style"
        if (line.starts_with("Section")) {
            const std::size_t eq = line.find('=');
            const std::size_t q1 = line.find('"');
            if (eq != std::string_view::npos && q1 != std::string_view::npos) {
                std::string_view name = Trim(line.substr(eq + 1, q1 - eq - 1));
                if (name.ends_with(',')) name.remove_suffix(1);
                const std::size_t q2 = line.find('"', q1 + 1);
                const std::size_t q3 = q2 == std::string_view::npos ? q2 : line.find('"', q2 + 1);
                const std::size_t q4 = q3 == std::string_view::npos ? q3 : line.find('"', q3 + 1);
                if (q2 == std::string_view::npos) {
                    error = where + ": bad section line";
                    return false;
                }
                UiLayoutSection s;
                s.name = std::string(Trim(name));
                s.rect = Rect(line.substr(q1 + 1, q2 - q1 - 1));
                if (q4 != std::string_view::npos) s.style = Integer(line.substr(q3 + 1, q4 - q3 - 1));
                out.sections.push_back(std::move(s));
                section = &out.sections.back();
                node = nullptr;
                continue;
            }
        }

        if (line == "}") {
            if (node != nullptr) {
                node = nullptr;
            } else {
                section = nullptr;
            }
            continue;
        }

        std::size_t ident = 0;
        while (ident < line.size() && IsIdentifierChar(line[ident])) ++ident;

        // NAME:Type opens a control.
        if (ident > 0 && ident < line.size() && line[ident] == ':' && section != nullptr) {
            std::size_t typeEnd = ident + 1;
            while (typeEnd < line.size() && IsIdentifierChar(line[typeEnd])) ++typeEnd;
            UiLayoutNode n;
            n.name = std::string(line.substr(0, ident));
            n.type = std::string(line.substr(ident + 1, typeEnd - ident - 1));
            section->nodes.push_back(std::move(n));
            node = &section->nodes.back();
            continue;
        }

        // Key=KIND,"value" (an unterminated quote runs to the end of the line).
        if (ident > 0 && ident < line.size() && line[ident] == '=' && node != nullptr) {
            const std::size_t comma = line.find(',', ident);
            const std::size_t open = comma == std::string_view::npos ? comma : comma + 1;
            if (open < line.size() && line[open] == '"') {
                std::size_t close = line.find('"', open + 1);
                if (close == std::string_view::npos) close = line.size();
                AssignProperty(*node, std::string(line.substr(0, ident)), line.substr(open + 1, close - open - 1));
            }
            continue;
        }
        // Anything else (section-level properties, stray text) is ignored, like the original.
    }
    if (!branches.empty()) {
        error = "unterminated #ifdef " + branches.back().key;
        return false;
    }
    return true;
}

bool DecodeTextCatalog(std::string_view text, TextCatalog& out, std::string& error) {
    out.clear();
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    // A record starts with "<digits>\t"; other lines continue the previous record.
    std::vector<std::string> records;
    bool open = false;
    for (std::string_view line : SplitLines(text)) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        std::size_t digits = 0;
        while (digits < line.size() && line[digits] >= '0' && line[digits] <= '9') ++digits;
        if (digits > 0 && digits < line.size() && line[digits] == '\t') {
            records.emplace_back(line);
            open = true;
        } else if (line.starts_with("//")) {
            open = false; // a comment ends the record before it
        } else if (open && !records.empty()) {
            records.back() += '\n';
            records.back() += line;
        }
    }
    for (std::string& record : records) {
        while (!record.empty() && record.back() == '\n') record.pop_back(); // blank lines after it
        std::vector<std::string_view> columns;
        std::size_t start = 0;
        while (start <= record.size()) {
            std::size_t end = record.find('\t', start);
            if (end == std::string::npos) end = record.size();
            columns.push_back(std::string_view(record).substr(start, end - start));
            start = end + 1;
        }
        if (columns.size() < 2) continue;
        const std::string_view key = Trim(columns[1]);
        if (key.empty() || key.starts_with("//")) continue;
        // Column 8 is the English text; a placeholder ("0", "xxx", "-", "null") is no text.
        std::string_view cell = columns.size() > 8 ? Trim(columns[8]) : std::string_view{};
        if (cell == "0" || cell == "xxx" || cell == "-" || cell == "null") cell = {};
        // A few rows have no English but an English text in the next column (the localisers' slip:
        // SN_ITEM_ETC_SPEED_UP_BASIC "Biginner's movement scroll"): taken when it is plain ASCII
        // with a letter in it (ours: the original shows nothing there).
        if (cell.empty() && columns.size() > 9) {
            const std::string_view next = Trim(columns[9]);
            const bool ascii = std::all_of(next.begin(), next.end(), [](char ch) { return ch >= 0x20 && ch < 0x7F; });
            const bool letters = std::any_of(next.begin(), next.end(), [](char ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'); });
            if (ascii && letters && next != "xxx" && next != "null") cell = next;
        }
        std::string english(cell);
        for (std::size_t p = english.find("\\n"); p != std::string::npos; p = english.find("\\n", p + 1)) {
            english.replace(p, 2, "\n");
        }
        // A key given twice: the later row wins, unless it is empty.
        std::string& slot = out[std::string(key)];
        if (!english.empty() || slot.empty()) slot = std::move(english);
    }
    if (out.empty()) {
        error = "no text records";
        return false;
    }
    return true;
}

bool LoadTextCatalog(const FileSystem& files, const std::string& path, TextCatalog& out, std::string& error) {
    const auto file = files.Read(path);
    if (!file) {
        error = path + ": not found";
        return false;
    }
    const std::string text = core::DecodeTextFile(file->Bytes());
    // A list of files: every line (blank ones aside) a name ending in .txt, no tabs.
    std::vector<std::string> listed;
    bool list = true;
    for (std::string_view line : SplitLines(text)) {
        line = Trim(line);
        if (line.starts_with("\xEF\xBB\xBF")) line.remove_prefix(3);
        if (line.empty()) continue;
        std::string lower(line);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (line.find('\t') != std::string_view::npos || !lower.ends_with(".txt")) {
            list = false;
            break;
        }
        listed.push_back(std::string(line));
    }
    TextCatalog part;
    if (!list || listed.empty()) {
        if (!DecodeTextCatalog(text, part, error)) return false;
        out.merge(part);
        return true;
    }
    const std::size_t slash = path.find_last_of("\\/");
    const std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    bool any = false;
    std::string missing;
    for (const std::string& name : listed) {
        const auto listedFile = files.Read(folder + name);
        if (!listedFile) {
            missing += (missing.empty() ? "" : ", ") + name;
            continue;
        }
        // One without records is read all the same (1.188's TextData_New.txt is its header only).
        part.clear();
        std::string partError;
        DecodeTextCatalog(core::DecodeTextFile(listedFile->Bytes()), part, partError);
        out.merge(part);
        any = true;
    }
    if (!missing.empty()) error = path + " lists files not read: " + missing;
    return any;
}

} // namespace sr::assets
