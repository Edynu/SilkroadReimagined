#pragma once

// The stall network's tables (Media.pk2 `server_dep\silkroad\textdata\`), measured over
// the 1.188 files:
//
// fmncategorytreedata.txt (72 rows): 0 in service, 1 code (WK_...), 2 its text key, 3 the parent's code (`xxx`:
//   a class of its own), 4 the type's network group (1..57; 0 for a class), 5 the degrees to choose from (11, 5
//   for the sockets' stones, 0 none).
// fmntidgroupmapdata.txt (138 rows): 0 in service, 1 a group, 2..5 TID1..TID4 (0: any) of the items it covers;
//   a row naming all four wins over one with a 0 (measured: one group per item that way).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sr::assets {

class FileSystem;

struct StallNetworkType {
    std::string code, textKey;
    std::uint32_t group = 0;
    std::uint8_t degrees = 0; // 0: no degree to choose
};

struct StallNetworkClass {
    std::string code, textKey;
    std::vector<StallNetworkType> types;
};

class StallNetworkData {
public:
    void ParseCategories(std::string_view text);
    void ParseGroups(std::string_view text);
    bool Load(const FileSystem& files, std::string& error);

    const std::vector<StallNetworkClass>& Classes() const { return classes_; }
    // The network group of an item by its TIDs (TID1 is 3 for every item; 0: in no group).
    std::uint32_t GroupOf(std::uint8_t tid1, std::uint8_t tid2, std::uint8_t tid3, std::uint8_t tid4) const;
    // The type of a group (null: none).
    const StallNetworkType* TypeOf(std::uint32_t group) const;

private:
    struct Rule {
        std::uint32_t group = 0;
        std::uint8_t tid[4]{};
    };
    std::vector<StallNetworkClass> classes_;
    std::vector<Rule> rules_;
};

} // namespace sr::assets
