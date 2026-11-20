#pragma once

// What a skill does and costs, in the original's own words: the lines of its tooltip, as the 1.188 client builds them
// (the costs, the lines of the parameter tags, a chain's), from the client's text keys. Shared by the client's tooltip
// (its layout and colours
// there) and the skill lab's list. Where the original glues two texts together
// ("FreezingProbability") ours puts a space between.

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace sr::assets {
class SkillTable;
struct SkillRow;
} // namespace sr::assets

namespace sr::game {

// A text of the client's tables by its key (the key itself, or empty, when it has none).
using SkillTextLookup = std::function<std::string(const std::string& key)>;

// The original's texts are printf formats; the numbers come formatted, put in order.
std::string FormatText(const std::string& format, std::initializer_list<std::string> args);

// A weapon kind's name key (TID4 of a weapon; skilldata cols 50 and 51, reqi's weapon sub-kinds); null for none.
const char* WeaponTextKey(int kind);

// "Consumed HP/MP", their ratios, then "Used weapon: X, Y".
std::vector<std::string> SkillCostLines(const assets::SkillRow& row, const SkillTextLookup& text);

// The items to wear ('reqi', up to five: class and kind) on one line, "Required Item" (one of them) or with 'reqn'
// "Essential Item" (all of them); `kinds` each as class << 8 | kind. Empty when it asks for none.
struct SkillItemNeeds {
    std::string line;
    bool essential = false;
    std::vector<std::uint16_t> kinds;
};
SkillItemNeeds SkillItemLine(const assets::SkillRow& row, const SkillTextLookup& text);

// What one row does, one line or more per tag in the original's order (no chain, no 'rmut').
std::vector<std::string> SkillRowEffectLines(const assets::SkillRow& row, const SkillTextLookup& text);

// What the skill does as its tooltip says it: a chain's summary over its hits (col 9), else its own lines and those of
// the skill its 'rmut' names. `skills` null: its own lines alone.
std::vector<std::string> SkillEffectLines(const assets::SkillRow& row, const assets::SkillTable* skills, const SkillTextLookup& text);

} // namespace sr::game
