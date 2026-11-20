#pragma once

// The glows of weapons and shields.
//
// Their effects: Media resinfo\itemrare.txt and, in 1.188, resinfo\itemoptionefp.txt, read by one reader into one
// list per item (called for both by the tables' loader), one line per effect, tab-separated:
//   0 the item's codename (itemrare.txt: the Seal of Star / Moon / Sun weapons and shields, A / B / C, and their
//     _HONOR twins; itemoptionefp.txt: every weapon and shield from degree 1), 1 the .efp under the Particles root
//     (system\system_raretype_a_step1.efp, system\system_enchant_a_01.efp ...), 2 its size in percent (40..225; the
//     original divides it by 100), 3 the item model's own bone it hangs on (ai_start / ai_end of a blade, Bone01 /
//     Bone03 of a bow, shield, staff or axe), 4 an offset "x,y,z" or "none", then (itemoptionefp.txt) 5 the lowest
//     plus it shows at (0 when absent: 8 in every line) and 6 the highest (255 when absent: 100); a column 7 is not
//     read. The dressing attaches each effect whose plus range holds the item's plus.
// An item may have several lines (a two-handed blade's glow at its hilt and its tip).
//
// The plus glow ("equip power", ModDataProgEquipPow / CRTModProgEquipPow): resinfo\itemoption.txt gives a weapon's
// or shield's glow type (codename, type 1..4), resinfo\itemtypenumber.txt each type's rows by the
// lowest plus (read by the same function); the dressing takes the row of the highest lowest plus not
// above the item's plus and puts it on the item's model as a material modifier (kind 0x70000).
// Each frame its colour goes from `from` to `to` over `periodMs` and back, and its texture's offset grows
// by the frame's milliseconds x `scroll` / 20; drawn, stage 0 is the option texture on the moved
// coordinates times that colour (`colorOp`: D3DTOP 4 modulate, 5 modulate 2x), stage 1 adds the item's own texture
// (`addOp` 7, D3DTOP_ADD), its alpha the item texture's; `alphaTest` turns the alpha test on at reference 1.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sr::assets {

class FileSystem;

struct ItemGlowEffect {
    std::string effect; // .efp under the Particles root
    float scale = 1.0f;
    std::string bone;   // the item model's bone
    std::array<float, 3> offset{};
    std::uint8_t minPlus = 0, maxPlus = 255; // the item's plus it shows at
    bool ShowsAt(int plus) const { return plus >= minPlus && plus <= maxPlus; }
};

class ItemGlowTable {
public:
    // itemrare.txt, then itemoptionefp.txt when the game has it (1.150 does not).
    bool Load(const FileSystem& fs, std::string& error);
    // Replaces the table with one file's lines. Fails when a line has under five columns or its size is not a number.
    bool Decode(std::string_view text, std::string& error);
    // Adds one file's lines (`name` for the errors).
    bool Add(std::string_view text, std::string_view name, std::string& error);
    // An item's effects by its codename; null when it has none.
    const std::vector<ItemGlowEffect>* Find(std::string_view codename) const;
    const std::unordered_map<std::string, std::vector<ItemGlowEffect>>& Items() const { return items_; }
    std::size_t size() const { return items_.size(); }
    std::size_t lines() const { return lines_; }

private:
    std::unordered_map<std::string, std::vector<ItemGlowEffect>> items_;
    std::size_t lines_ = 0;
};

// One row of itemtypenumber.txt (the plus glow; above).
struct EquipGlow {
    int type = 0;
    std::uint8_t minPlus = 0;
    std::array<float, 2> scroll{}; // the texture offset's growth per 20 ms (u, v)
    std::string texture;           // prim\mtrl\itemoption\option_textureNN.ddj
    std::uint8_t colorOp = 4, addOp = 7;
    std::array<float, 3> from{}, to{}; // the colour's ends (0..1)
    std::uint32_t periodMs = 0;        // from one end to the other (0: it stays at `from`)
    bool alphaTest = false;            // the item cut out where its alpha is 0
};

class EquipGlowTable {
public:
    bool Load(const FileSystem& fs, std::string& error);
    // itemoption.txt: codename -> type. Fails on a line without a number.
    bool DecodeTypes(std::string_view text, std::string& error);
    // itemtypenumber.txt: the rows. Fails on a line of under ten columns or a number that does not read.
    bool DecodeRows(std::string_view text, std::string& error);
    // The row an item of that codename and plus glows with: its type's row of the highest lowest plus not above
    // the plus; null when the item has no type or the plus is below every row (or 0).
    const EquipGlow* Find(std::string_view codename, int plus) const;
    std::size_t types() const { return types_.size(); }
    std::size_t rows() const { return rows_.size(); }
    const std::vector<EquipGlow>& Rows() const { return rows_; }

private:
    std::unordered_map<std::string, int> types_;
    std::vector<EquipGlow> rows_;
};

} // namespace sr::assets
