#pragma once

// The game guide's tables (the Help key's window, 1.188's CIFGameGuide), from Media.pk2's textdata:
//
// gameguidedata.txt, a topic a line, tab-separated:
//   0 in service (1), 1 its id, 2 its Korean name (unused), 3 its level (0 a heading, 1 a topic under one),
//   4 a topic's heading id (a heading: how many topics it has), 5 its page's text key (texthelp.txt, SML2 markup),
//   6 its menu row's text key.
// The ids from 50000 are the event guide's pages, which eventguidedata.txt shows.
//
// eventguidedata.txt, an event a line, tab-separated:
//   0 in service, 1 the event's Korean name, 2 for whom ("xxx" any, 중국 China, 유럽 Europe, "CH-"/"EU-" and a job),
//   3 its kind (1 a level reached, 2 a quest taken, 3 an NPC chosen, 4 an item got, 5 a joining, 6 a state,
//   7 an action icon clicked, 8 a window opened, 9 another: 1 the first login, 2 the first move, 3 the first field,
//   4 back in town after a death, 5 another's stall clicked, 6 the guide's icon), 4 its value (the level, the quest's
//   codename, ...), 5 a flag (0 / 1; not read), 6 the guide's page id (gameguidedata.txt).
// The ids from 100000 are the quest guide's: its headings and the quests' pages (SN_PAYCON_*).

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace sr::assets {

class FileSystem;

struct GameGuideRow {
    bool inService = false;
    std::uint32_t id = 0;
    int level = 0;           // 0 a heading, 1 a topic
    std::uint32_t parent = 0; // a topic's heading (a heading: its topics' count)
    std::string pageKey, menuKey;
};
bool DecodeGameGuide(std::string_view utf8Text, std::vector<GameGuideRow>& out, std::string& error);

struct EventGuideRow {
    bool inService = false;
    std::string who;   // "xxx" any; else the country (and a job)
    int kind = 0;      // 1 level, 2 quest taken, 3 NPC chosen, 4 item got, 5 joining, 6 state, 7 action icon, 8 window, 9 other
    std::string value; // the level, the quest's codename, ...
    std::uint32_t guide = 0; // gameguidedata.txt's page id
};
bool DecodeEventGuide(std::string_view utf8Text, std::vector<EventGuideRow>& out, std::string& error);

struct GameGuideData {
    std::vector<GameGuideRow> topics;
    std::vector<EventGuideRow> events;
    // questdata.txt's levels by codename (the quest guide lists the quests up to the player's level + 10).
    std::map<std::string, int> questLevels;
};
// The tables from the game's files.
bool LoadGameGuide(const FileSystem& files, GameGuideData& out, std::string& error);

} // namespace sr::assets
