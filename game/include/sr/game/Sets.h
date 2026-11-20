#pragma once

// Item sets: the 1.188 game server's set operator, played as it plays. Each worn piece of a set (an item whose
// `_RefObjItem.SetID` is not 0, in
// the places 0..12) counts for its set; a set's count picks one of its groups (refsetitemgroup.txt / `_RefSetItemGroup`:
// the group of the most pieces at or under the count), and that group's rows (`_RefMagicOptGroup` of
// MagicType 2) are its bonus: 'reis' (MATTR_REINFORCE_ITEM_SET) raises the total plus of every worn piece of the set,
// 'rals' (MATTR_RESIST_ALL_SET) adds to the six element resists and four state resists.
//
// The operator works on events, not on what is worn: a set is looked at again only when a piece of it goes on or comes
// off, and its bonus is put on only when its group changes; a piece taken off, a piece breaking and a level change take
// the set's resists away (until the group next changes); a piece put on
// without the group changing keeps the whites it had. `SetWear` keeps that state for one character. Headless.

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sr::assets {
class FileSystem;
struct MagicOptGroupRow;
struct MagicOptionRow;
} // namespace sr::assets

namespace sr::game {

struct ItemInstance;
struct StateDefence;

// The codes (`_RefMagicOpt.Param1`) of the two set options the 1.188 data has (options 389 and 390).
inline constexpr std::uint32_t kSetPlusCode = 0x72656973;   // 'reis', MATTR_REINFORCE_ITEM_SET
inline constexpr std::uint32_t kSetResistCode = 0x72616C73; // 'rals', MATTR_RESIST_ALL_SET

// One row of a set's group: its option (magicoption.txt's id), the option's code and the row's Value.
struct SetOption {
    std::uint32_t option = 0, code = 0, value = 0;
    bool operator==(const SetOption&) const = default;
};

// refsetitemgroup.txt (the original's `_RefSetItemGroup`, the same 18 columns in the client's file and the database):
// 0 service, 1 ID, 2 CodeName128, 3 ObjName128, 4 NameStrID128, 5 DescStrID128, 6 SetEffectMask (the server skips it),
// 7 SetMagicMask, 8..17 2SetMOptGroupID..11SetMOptGroupID.
struct SetGroup {
    int id = 0;
    std::string codename, nameKey;
    std::uint32_t magicMask = 0;            // bit i: a group at i + 2 pieces (the loader only checks it against the ids)
    std::array<std::uint32_t, 10> groups{}; // [i]: the magic option group at i + 2 pieces (0: none)
};
bool DecodeSetGroups(std::string_view utf8Text, std::vector<SetGroup>& out, std::string& error);

class SetTable {
public:
    void Add(SetGroup set) { sets_[set.id] = std::move(set); }
    void AddOption(std::uint32_t group, const SetOption& option) { options_[group].push_back(option); }
    const SetGroup* Find(int id) const;
    // The group a count of pieces picks (_PickHighestTierGroup):
    // under 2 none; else the group of the most pieces at or under the count (5 worn of a 2 / 4 / 6 set: the 4's).
    static std::uint32_t PickGroup(const SetGroup& set, int pieces);
    // The same by the set's id (0: unknown set or none).
    std::uint32_t GroupFor(int setId, int pieces) const;
    // A group's MagicType 2 rows (the only ones the operator applies).
    const std::vector<SetOption>& OptionsOf(std::uint32_t group) const;
    // What a group gives, summed by code: the plus raise ('reis') and the resists ('rals').
    int PlusOf(std::uint32_t group) const;
    int ResistOf(std::uint32_t group) const;
    bool Empty() const { return sets_.empty(); }
    std::size_t Size() const { return sets_.size(); }
    const std::map<int, SetGroup>& Sets() const { return sets_; }

private:
    std::map<int, SetGroup> sets_;
    std::map<std::uint32_t, std::vector<SetOption>> options_;
};

// The table from the sets and the magic option groups: for each group a set names, its MagicType 2 rows with their
// option's code (magicoption.txt's Param1). Rows of an option magicoption.txt lacks are left out (the original logs
// them).
SetTable MakeSetTable(const std::vector<SetGroup>& sets, const std::vector<assets::MagicOptGroupRow>& groups,
                      const std::vector<assets::MagicOptionRow>& options);
// From the game files (or the database's tables mounted over them: the server's DatabaseText): refsetitemgroup.txt,
// and the groups' rows from `groups` when given (the server's `_RefMagicOptGroup`), else refmagicoptgroup.txt.
bool LoadSetTable(const assets::FileSystem& fs, const std::vector<assets::MagicOptionRow>& options,
                  const std::vector<assets::MagicOptGroupRow>* groups, SetTable& out, std::string& error);

// A worn item as the set operator sees it.
struct SetPiece {
    std::int64_t serial = 0;  // the item (0: one not stored yet, known by its place and kind)
    std::uint32_t refId = 0;
    int place = 0;            // 0..12
    int setId = 0;            // its row's SetID (0: none)
    bool broken = false;      // no durability left: its contributions are not installed
    bool fits = true;         // its requirements met
    std::uint64_t stamp = 0;  // SetStamp: what its whites are made from
};
// What an item's whites are made from but the set (its plus, the advanced elixir's, its variance, its blues, its
// sockets): a change means the original made them again with no raise (alchemy, a Repair charge used: their
// calls pass 0).
std::uint64_t SetStamp(const ItemInstance& item);

class SetWear {
public:
    // The character as it is now: every worn piece (places 0..12) and its level. The first call after Reset is its
    // entry into the world (counts every worn piece, then each is installed). Later calls play what changed since the
    // one before, as the original plays it: pieces put on and taken off as one move; a piece broken or mended; a level
    // changed (every worn piece taken out and installed again).
    void Update(const SetTable& table, std::vector<SetPiece> worn, int level);
    // Made anew (the character entered the world: its items are new objects, their whites made with no raise).
    void Reset() { *this = SetWear{}; }

    // The raise the piece's whites were last made with (its total plus's extra; 0: none, or made again since).
    int RaiseOf(const SetPiece& piece) const;
    int RaiseOf(std::int64_t serial, std::uint64_t stamp) const;
    // A piece whose requirements fail (above the level, its masteries lowered) that a set's change installed anyway: the
    // inventory's makes every worn piece of the set again and installs it with no requirements test,
    // so its contributions stand until it is next taken out (a level change, taken off, broken). Counted as worn.
    bool Forced(const SetPiece& piece) const { return forced_.contains(KeyOf(piece)); }
    // The sets' changes standing on the keeper (each set their source): the resists of 'rals', summed over the sets.
    int ResistAll() const;
    // The standing rows of any other code, by set (none in the 1.188 data).
    std::vector<std::pair<int, SetOption>> OtherStanding() const;
    // 'rals' on a player's state defence: +value to the six elements' resists (params 0x1B..0x20) and to the
    // state buckets 15, 8, 0 and 3 (combustion, disease, sleep, fear).
    void AddResists(StateDefence& defence) const;
    // The operator's state: a set's pieces counted and the group it last put on (0: none or not counted).
    int Pieces(int setId) const;
    std::uint32_t Group(int setId) const;
    bool Entered() const { return entered_; }

private:
    struct Status {
        int count = 0;
        bool dirty = false;      // a piece went on or came off since it was last looked at
        std::uint32_t group = 0; // the group it last put on
    };
    struct Raised {
        int value = 0;
        std::uint64_t stamp = 0;
    };
    static std::int64_t KeyOf(const SetPiece& piece);
    void PutOn(const SetPiece& piece);
    void TakeOff(const SetPiece& piece);
    void Remove(const SetPiece& piece);
    void Install(const SetTable& table, const SetPiece& piece);
    void Refresh(const SetTable& table);
    void Raise(int setId, int value);

    std::map<int, Status> sets_;                                     // by SetID (erased at 0 pieces)
    std::map<std::int64_t, Raised> raised_;                          // by item
    std::map<int, std::map<std::uint32_t, std::uint32_t>> standing_; // by SetID: code -> value on the keeper
    std::vector<SetPiece> worn_;                                     // by place
    std::set<std::int64_t> forced_;                                  // unfit pieces a set's change installed (Forced)
    int level_ = 0;
    bool entered_ = false;
};

} // namespace sr::game
