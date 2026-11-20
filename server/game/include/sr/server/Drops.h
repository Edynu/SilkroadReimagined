#pragma once

// What a killed monster drops. The tables are the original's, in the database (a monster's own materials among them);
// the client's own levelgold.txt (gold by level) is read from the game files. A kill:
//
// 1. A unique's extras first: equipment of its level's lowest class, reinforcement recipes, return
//    scrolls, counts from the unique-count rows.
// 2. Its assigned drops: groups at their chance (members picked by weight), fixed rows (tries at a
//    chance each), then its own materials (each at its chance).
// 3. Passes (the budget by its kind): each makes one gold heap (levelgold's range times the kind's
//    multiplier), one equipment roll (the SoX table about one pass in a thousand; a class by the
//    monster's level, the item by the monster's country, weighted, not above the monster's level,
//    kept at its own chance; a plus from the plus table, white stats the least of three rolls each)
//    and one roll of each consumable category in the original's order (potion, cure, elixir, tablet, ammunition or a
//    scroll, a magic or an attribute stone). A roll
//    makes its tries until one gives an item (a try whose class or item fails goes on to
//    the next), on the class table's row of the monster's exact level (none, nothing).
//    The capacity stops it. Elixirs come only from here (and a unique's extras): no monster has one
//    in its own tables.
// 4. Each item is kept by the level gap: a player more than 6 above loses 4 % a level.
//
// A monster with a profile for its kind (_RefCustomizingReservedItemDropForMonster) takes its passes, tries and gold
// multiplier from it, and its extras in place
// of all of a unique's: equipment pieces (with "repair invalid" at a chance), B elixirs, tablets that cannot miss,
// special return scrolls; Roc's (3, 1) record makes its passes' equipment never rare and every roll 175 times as
// likely. A kind it does not list gives none (Haroeris' body drops nothing).
//
// The rates (SroGame.cfg) scale: the gold's amount and its chance, the equipment classes, the SoX
// gate and the SoX groups, materials (tablets, stones, the monster's own), elixirs, potions (potions,
// cures, scrolls, arrows and bolts).
//
// Random blues (RollBlues): the passes' pieces get them now and then and drop either way; the extra pieces of a unique
// or a profile get them or are lost; a drop group's piece gets its magic group's fixed options (ROC's set) instead.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "sr/game/Items.h"

namespace sr::assets {
class ItemTable;
struct ItemRow;
struct ObjectRow;
struct MagicOptionRow;
struct MagicOptionAssignRow;
} // namespace sr::assets

namespace sr::server {

// The server's rates (SroGame.cfg; every one 1 = the original's): experience and skill experience
// from kills (C10c), and the drops (C10d: how much gold, how often each kind of item).
struct GameRates {
    double experience = 1.0;      // rate-exp
    double skillExperience = 1.0; // rate-sp
    double gold = 1.0;            // rate-gold: the gold of a drop
    double goldChance = 1.0;      // rate-drop-gold: how often gold drops (the original's always)
    double equipment = 1.0;       // rate-drop-equipment: weapons, armour, accessories
    double rare = 1.0;            // rate-drop-sox: Seal of Star / Moon / Sun
    double materials = 1.0;       // rate-drop-material: alchemy materials, stones and tablets
    double elixirs = 1.0;         // rate-drop-elixir
    double potions = 1.0;         // rate-drop-potion: potions, cures, scrolls, ammunition
    int levelCap = 110;           // level-cap (vSRO 1.188 content runs to 110)
};

class DropTable {
public:
    // Reads monster_drops.tsv; codenames the item table does not know are left out. False (with
    // `error`) when the file is missing or holds nothing.
    bool Load(const std::string& path, const assets::ItemTable& items, std::string& error);
    // The same records as text (ReferenceData.h: read from the database's original tables).
    bool LoadText(const std::string& text, const assets::ItemTable& items, std::string& error);
    void SetLevelGold(std::vector<std::pair<std::uint32_t, std::uint32_t>> byLevel) { levelGold_ = std::move(byLevel); }
    bool Empty() const { return budgets_.empty(); }

    struct Drop {
        game::ItemInstance item; // refId 0: gold
        std::uint64_t gold = 0;
    };
    std::vector<Drop> Roll(const assets::ObjectRow& monster, std::uint8_t rarity, int playerLevel, const GameRates& rates, std::mt19937& random) const;
    // A consumable family's items of one group with their weights (alchemy's dismantling picks its stones from the
    // magic and attribute stones of the piece's degree: group degree - 1).
    std::vector<std::pair<const assets::ItemRow*, double>> Consumables(const std::string& family, int group) const;

    // A new item as a drop makes it (an equipment's white stats rolled, its durability the most; Magic POP's prize too).
    game::ItemInstance Instance(const assets::ItemRow& row, int count, int plus, std::mt19937& random) const;

    // The magic options the blues come from: every option row (magicoption.txt, the database's _RefMagicOpt through
    // DatabaseText) and the lists of the options each kind of piece may get (magicoptionassign.txt, _RefMagicOptAssign).
    // Without them no piece gets blues (and the extra pieces, whose blues decide them, are all lost).
    void SetMagicOptions(std::shared_ptr<const std::vector<assets::MagicOptionRow>> options, const std::vector<assets::MagicOptionAssignRow>& assign);
    bool HasMagicOptions() const { return magicOptions_ != nullptr; }
    // A new piece's random blues, in the original's steps: its options cleared;
    // the gate: a piece of Rarity 2 (the seals) always, any other when rand() % 101 <= 30; the count: the least of its
    // MaxMagicOptCount and four rolls 1..6; the list by its country, TID3 and TID4; that many tries, each a uniform
    // index into the list (an index already written loses the try), the option's row by the million roll over its
    // levels' trunc(Prob x 10^6) in ID order (none: the try lost; 'nrep': the try again), the row's groups
    // and ReqClass taking the piece (else the try lost), its value one of the row's list. False when the gate refused it
    // or its kind has no list (no blues); true otherwise, even when every try was lost.
    bool RollBlues(const assets::ItemRow& row, game::ItemInstance& item, std::mt19937& random) const;

private:
    struct Budget {
        int capacity = 8, passes = 1, attempts = 1;
    };
    struct Candidate {
        const assets::ItemRow* row = nullptr;
        double weight = 0;
        double admit = 1;  // its own chance to be kept (absolute %)
        int count = 1;     // a stack's count (consumables)
        int requiredLevel = 0;
    };
    // A class table: each level's groups with their chances, in group order.
    using ClassRow = std::vector<std::pair<int, double>>;
    using ClassTable = std::unordered_map<int, ClassRow>; // by level
    struct Assigned {
        const assets::ItemRow* row = nullptr;
        double chance = 1;
        int minTries = 1, maxTries = 1;
        int plus = 0;
    };
    struct AssignedGroup {
        int group = 0;
        double chance = 0;
        int min = 1, max = 1;
        bool distinct = false;
    };
    // A unique's extras by stage; a monster's row names only the stages it changes (the rest are
    // grade 8's or the default's).
    struct UniqueCounts {
        std::optional<std::pair<int, int>> equipment, repairable, recipe, scroll;
    };

    // The row of the monster's exact level (the passes); AtOrBelow: else the nearest below (a unique's extras).
    const ClassRow* Classes(const ClassTable& table, int level) const;
    const ClassRow* ClassesAtOrBelow(const ClassTable& table, int level) const;
    int RollGroup(const ClassRow& row, double scale, std::mt19937& random) const;
    bool MakeEquipment(int group, bool rare, const assets::ObjectRow& monster, std::mt19937& random, Drop& out) const;
    int RollPlus(std::mt19937& random) const;
    // An extra piece (Loot_InitializeSpecialEquipmentProperties): an
    // equipment piece of the class, an accessory refused, the plus the least of two rolls 1..5, the blues, a piece they
    // refuse lost.
    enum class Special { None, Accessory, Refused, Made };
    Special SpecialPiece(int group, const assets::ObjectRow& monster, std::mt19937& random, Drop& out) const;
    // 'nrep' on a unique's enhanced piece after its blues: value 0, the durability + its Param3 %.
    void AddNotReparable(const assets::ItemRow& row, game::ItemInstance& item) const;

    std::unordered_map<int, Budget> budgets_;
    std::unordered_map<std::string, Budget> monsterBudgets_;
    int partyAttempts_ = 9;
    std::unordered_map<std::string, double> goldMultipliers_; // by grade number or "party"
    std::vector<double> plusCumulative_;
    ClassTable normalClasses_, rareClasses_;
    // Equipment by rare / group / country (0 CH, 1 EU).
    std::unordered_map<int, std::vector<Candidate>> normalItems_[2], rareItems_[2];
    struct Category {
        std::string name;
        std::vector<std::vector<std::string>> alternatives; // one of them a pass, then one of its families
    };
    std::vector<Category> categories_;
    std::unordered_map<std::string, ClassTable> consumableClasses_;
    std::unordered_map<std::string, std::unordered_map<int, std::vector<Candidate>>> consumableItems_;
    std::unordered_map<std::string, std::vector<Assigned>> assigned_;
    // A monster's own alchemy materials (_RefMonster_AssignedItemDrop): a count from min..max, each
    // kept at its chance times rate-drop-material.
    std::unordered_map<std::string, std::vector<Assigned>> materials_;
    std::unordered_map<std::string, std::vector<AssignedGroup>> assignedGroups_;
    // A drop group's members (_RefDropItemGroup): the item, its SelectRatio, its RefMagicGroupID.
    struct GroupItem {
        const assets::ItemRow* row = nullptr;
        double ratio = 0;
        int magicGroup = 0;
    };
    std::unordered_map<int, std::vector<GroupItem>> groupItems_;
    // A magic group's MagicType 1 rows (_RefMagicOptGroup by LinkID, in table order): an option and its value (0: one of
    // the option's own).
    struct GroupOption {
        std::uint32_t option = 0;
        std::uint32_t value = 0;
    };
    std::unordered_map<int, std::vector<GroupOption>> magicGroups_;
    std::unordered_map<std::string, UniqueCounts> uniqueCounts_;
    // A monster's profile for one kind: each group's first record (P1, P2).
    struct Profile {
        std::optional<std::pair<int, int>> gold;      // (1, 11): the gold multiplier, the passes
        std::optional<std::pair<int, int>> tries;     // (1, 12): the tries, the passes without (1, 11)
        std::optional<std::pair<int, int>> equipment; // (2, 0): pieces
        std::optional<std::pair<int, int>> elixirs;   // (2, 6): B elixirs
        std::optional<std::pair<int, int>> tablets;   // (2, 10): tablet rolls that cannot miss
        std::optional<std::pair<int, int>> scrolls;   // (2, 14): ITEM_ETC_SCROLL_RETURN_02
        std::optional<std::pair<int, int>> rare;      // (3, 1): the rare modulus, the class multiplier
    };
    struct ProfileOption {
        std::uint32_t id = 0;    // _RefMagicOpt.ID (65 MATTR_NOT_REPARABLE)
        int chance = 0;          // kept when rand() % 101 < it
        std::uint32_t value = 0; // the record's P3 (0: the option's own pick, 0 for 'nrep')
        std::uint32_t raise = 0; // 'nrep': the durability's raise in percent, the option's Param3 (400)
    };
    std::unordered_map<std::string, std::unordered_map<int, Profile>> profiles_; // by monster, then kind
    std::unordered_map<std::string, std::vector<ProfileOption>> profileOptions_;
    void ProfileExtras(const Profile& profile, const assets::ObjectRow& monster, int level, std::mt19937& random, std::vector<Drop>& planned) const;
    std::vector<const assets::ItemRow*> uniqueRecipes_;
    const assets::ItemRow* uniqueScroll_ = nullptr;
    const assets::ItemTable* items_ = nullptr;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> levelGold_;

    // The blues' reference (SetMagicOptions): the option rows; each option's rows in ID order, by name; the
    // lists by the piece's country (0..3), then TID4 << 16 | TID3, as indices into families_.
    std::shared_ptr<const std::vector<assets::MagicOptionRow>> magicOptions_;
    std::vector<std::vector<const assets::MagicOptionRow*>> families_;
    std::unordered_map<std::string, int> familyIndex_;
    std::array<std::unordered_map<std::uint32_t, std::vector<int>>, 4> blueLists_;
    const assets::MagicOptionRow* notReparable_ = nullptr; // MATTR_NOT_REPARABLE's level-1 row
};

} // namespace sr::server
