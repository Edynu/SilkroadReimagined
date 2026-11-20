#pragma once

// The original interface layouts (Media.pk2 resinfo\*.txt, e.g. resinfo\pstitle.txt
// for the login screen) and the interface text table (textuisystem.txt).
//
// A layout file is text (usually UTF-16LE) of the form
//
//   Interface Text
//   Section = Name, "x,y,w,h", "style"
//   {
//     GDR_BTN_OK:CIFButton
//     {
//       ID=INTEGER,"5"
//       Rect=RECT,"0,0,92,41"
//       DDJ=STRING,"interface\login\lgn_ok.ddj"
//     }
//   }
//
// with #ifdef / #else / #endif branches keyed by the symbols in config\define.txt.
// Rects are in the original's 1600 x 1200 virtual space or relative to the parent.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sr::assets {

struct UiLayoutRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

struct UiLayoutColor {
    std::uint8_t a = 255, r = 255, g = 255, b = 255;
};

struct UiLayoutNode {
    std::string name; // e.g. GDR_BTN_OK
    std::string type; // e.g. CIFButton
    int id = -1;
    UiLayoutRect rect;
    UiLayoutRect clientRect; // insets for text inside the control
    bool hasClientRect = false;
    int style = 0;
    std::string text; // a textuisystem key (UIIT_..., UIO_...) or literal text
    std::string ddj;  // texture path; for frames a prefix completed by left_up.ddj etc.
    UiLayoutColor fontColor;
    bool hasFontColor = false;
    int fontIndex = 0;
    int hAlign = 0; // 0 left, 1 centre, 2 right
    int vAlign = 1; // 0 top, 1 centre, 2 bottom
    // The part of the texture the control shows (UV_LT / UV_RB, 0..1).
    float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // u0, v0, u1, v1
    std::string subSection;
    std::unordered_map<std::string, std::string> properties; // every property, raw value
};

struct UiLayoutSection {
    std::string name;
    UiLayoutRect rect;
    int style = 0;
    std::vector<UiLayoutNode> nodes;
};

struct UiLayout {
    std::vector<UiLayoutSection> sections;
    // First node with this name in any section, or nullptr.
    const UiLayoutNode* Find(std::string_view name) const;
};

using UiDefines = std::unordered_set<std::string>;

// config\define.txt: one symbol per line, "//" comments.
UiDefines DecodeUiDefines(std::string_view utf8Text);

// Text already decoded to UTF-8 (core::DecodeTextFile).
bool DecodeUiLayout(std::string_view utf8Text, const UiDefines& defines, UiLayout& out, std::string& error);

// textuisystem.txt (and the other localized text tables): tab-separated records
// starting with "<number>\t"; column 1 is the key, column 8 the English text.
// "\n" escapes become newlines.
using TextCatalog = std::unordered_map<std::string, std::string>;
bool DecodeTextCatalog(std::string_view utf8Text, TextCatalog& out, std::string& error);

// A text table by its path, merged into `out`. In 1.188 textdataname.txt and textquest.txt hold no
// records but the names of the files that do, one a line, in the same folder (TextData_Object.txt,
// TextData_Equip&Skill.txt, ...): each is read in turn. A table of records is decoded as it is.
// False when the file (or every file it lists) is missing; `error` says which.
class FileSystem;
bool LoadTextCatalog(const FileSystem& files, const std::string& path, TextCatalog& out, std::string& error);

} // namespace sr::assets
