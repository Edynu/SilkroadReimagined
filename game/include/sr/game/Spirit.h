#pragma once

// The Devil's Spirit (3/1/14/1, avatar place 4): what it gives while worn, 1:1 with the
// 1.188 game server. Its plus (OptLevel 0..10, raised by Sabakun's jewel) picks a row of three tables, the client's
// refabilitybyitemoptleveldata.txt / refmagicoptbyitemoptleveldata.txt / refskillbyitemoptleveldata.txt (the database's
// `_RefAbilityByItemOptLevel`, `_RefMagicOptByItemOptLevel`, `_RefSkillByItemOptLevel`: the same rows, measured
// 2026-11-16): options on the parameter keeper (HPNA, MPNA percents of the HP from STR and the MP from INT, BLOCKRATE)
// and skills the wearer may cast while it is on (the options with the spirit as their source, then the skills
// attached). A spirit whose (item, plus) has no row installs nothing at all, not even its own blues (0x1811, and stops
// there). Asleep (its awakening over), it gives nothing: the avatar inventory's check every 10 s takes its
// contributions off. Headless.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sr/game/Stats.h"

namespace sr::assets {
class FileSystem;
struct ItemRow;
struct MagicOptionRow;
} // namespace sr::assets

namespace sr::game {

struct ItemInstance;

// refabilitybyitemoptleveldata.txt: 0 Service, 1 ID (the link), 2 RefItemID, 3 ItemOptLevel.
struct SpiritAbilityRow {
    int id = 0;
    std::uint32_t item = 0;
    int plus = 0;
};
// refmagicoptbyitemoptleveldata.txt: 0 Link, 1 RefMagicOptID, 2 MagicOptValue, 3 TooltipType, 4 TooltipCodename.
struct SpiritOptionRow {
    int link = 0;
    std::uint32_t option = 0, value = 0;
    int tooltipType = 0;  // the tooltip's group: 0 basic, 1 added, 2 magic
    std::string tooltip;  // its line's text key ("xxx": the option's own line)
};
// refskillbyitemoptleveldata.txt: 0 Link, 1 RefSkillID.
struct SpiritSkillRow {
    int link = 0;
    std::uint32_t skill = 0;
};
bool DecodeSpiritAbilities(std::string_view utf8Text, std::vector<SpiritAbilityRow>& out, std::string& error);
bool DecodeSpiritOptions(std::string_view utf8Text, std::vector<SpiritOptionRow>& out, std::string& error);
bool DecodeSpiritSkills(std::string_view utf8Text, std::vector<SpiritSkillRow>& out, std::string& error);

// One (item, plus)'s row: its options (magicoption.txt's id, the option's code, the row's value) in the table's order,
// and its skills (skilldata ids).
struct SpiritLevel {
    struct Option {
        std::uint32_t option = 0, code = 0, value = 0;
        int tooltipType = 0;  // its tooltip's group (SpiritOptionRow)
        std::string tooltip;  // its line's text key ("xxx": the option's own line)
        bool operator==(const Option&) const = default;
    };
    std::vector<Option> options;
    std::vector<std::uint32_t> skills;
};

class SpiritTable {
public:
    void Add(std::uint32_t item, int plus, SpiritLevel level) { levels_[{item, plus}] = std::move(level); }
    // The row of a spirit at a plus (the original's key); null: none (it then gives nothing).
    const SpiritLevel* Find(std::uint32_t item, int plus) const;
    std::size_t Size() const { return levels_.size(); }
    bool Empty() const { return levels_.empty(); }
    const std::map<std::pair<std::uint32_t, int>, SpiritLevel>& Levels() const { return levels_; }

private:
    std::map<std::pair<std::uint32_t, int>, SpiritLevel> levels_;
};

// The table from the three files' rows (Service 1 only), each option with its code (magicoption.txt's Param1; one
// magicoption.txt lacks is left out).
SpiritTable MakeSpiritTable(const std::vector<SpiritAbilityRow>& abilities, const std::vector<SpiritOptionRow>& options,
                            const std::vector<SpiritSkillRow>& skills, const std::vector<assets::MagicOptionRow>& magicOptions);
// From the game files (or the database's tables mounted over them: the server's DatabaseText).
bool LoadSpiritTable(const assets::FileSystem& fs, const std::vector<assets::MagicOptionRow>& magicOptions, SpiritTable& out, std::string& error);

// What a worn Devil's Spirit gives now.
struct SpiritGift {
    bool awake = false;   // awake: its skills' 'reqi 14 1' holds
    bool found = false;   // its (item, plus) row exists: its contributions are on the keeper
    int plus = 0;
    // Its by-plus options, then its own blues (an UMDU stone's), on the keeper with the spirit as their source.
    std::vector<StatChange> changes;
    // Its skills (the learned map's), cast as an item's (SkillBook::itemSkills).
    std::vector<std::uint32_t> skills;
};
// `now` Unix seconds. Not a spirit, asleep, or no row for its plus: no changes and no skills (`awake` says which).
SpiritGift SpiritGiftOf(const SpiritTable& table, const assets::ItemRow& row, const ItemInstance& item,
                        const std::vector<assets::MagicOptionRow>* magicOptions, std::int64_t now);

} // namespace sr::game
