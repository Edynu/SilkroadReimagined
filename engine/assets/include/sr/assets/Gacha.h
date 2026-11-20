#pragma once

// Magic POP's tables: server_dep\silkroad\textdata\
//   gachaitemset.txt   0 service, 1 the set, 2 the prize's RefObjID, 3 Ratio (a set-1 prize's chance in ten-thousandths,
//                      a set-2 prize's weight), 4 the count, 5 GachaID (unique; what a play names), 6 Visible (its win
//                      announced), 7 param1 (1: 8 holds the prize's options string), 8 param1's text, 9..14 unused
//   gachanpcmap.txt    0 service, 1 the machine's RefObjID, 2 its selection (WIN) set, 3 its waste (LOSE) set
//   refgachatreeforclientuidata.txt  0 service, 1..4 TID1..4, 5 the big category's id, 6 its text, 7 the middle one's id,
//                      8 its text, 9 the most degree (0: none chosen)
// The client's files are the database's rows in service (`_RefGachaItemSet`, `_RefGachaNpcMap`; the server rebuilds
// them from the database: DatabaseText.h). Only rows whose column 0 is 1.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sr::assets {

class FileSystem;

struct GachaEntry {
    int set = 0;
    std::uint32_t item = 0;
    int ratio = 0;
    int count = 1;
    std::uint32_t id = 0; // GachaID
    bool visible = false;
    std::string options; // param1's text when param1 is set ("[BIIV]<M:str,1,3>..."); empty: none
};
struct GachaMachine {
    std::uint32_t npc = 0; // the machine's RefObjID
    int selection = 0, waste = 0;
};
struct GachaTreeRow {
    std::array<int, 4> typeId{};
    int bigId = 0;
    std::string bigText;
    int midId = 0;
    std::string midText;
    int maxDegree = 0;
};

class GachaTable {
public:
    // The entries and machines (the tree when `tree`: the client's own file).
    bool Load(const FileSystem& fs, std::string& error, bool tree = true);
    bool ParseEntries(std::string_view text, std::string& error);
    bool ParseMachines(std::string_view text, std::string& error);
    void ParseTree(std::string_view text);

    const std::vector<GachaEntry>& entries() const { return entries_; }
    const std::vector<GachaMachine>& machines() const { return machines_; }
    const std::vector<GachaTreeRow>& tree() const { return tree_; }
    const GachaEntry* Find(std::uint32_t gachaId) const;
    const GachaMachine* Machine(std::uint32_t npc) const;
    // Tests: rows of their own.
    void Add(GachaEntry e) { entries_.push_back(std::move(e)); }
    void Add(GachaMachine m) { machines_.push_back(m); }

private:
    std::vector<GachaEntry> entries_;
    std::vector<GachaMachine> machines_;
    std::vector<GachaTreeRow> tree_;
};

} // namespace sr::assets
