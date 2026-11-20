#pragma once

// The item lab's exam (L13): our game server run in-process on a database of its own, players
// made fresh (each race and sex, at the level cap and at level 1), each item of the paths it knows given to one and used
// or worn through the protocol as a player's client would, what happened measured against what the item's data and the
// original's rules say (game::ItemCatalogue, Potions.h, Items.h), each result written to the lab's tables
// (sr_lab_runs, sr_lab_results: the item's codename as the code) for the site's /dev Items tab (its Tested column).
// Scenario 1: the item works (drunk, read, worn); scenario 2: the rules refuse what they should (a locked kind, the
// level, the country, the sex, a missing recall point, ...).
//
// One game server, many players: with `workers` above 1 the exam's process keeps the server and starts that many
// worker processes (this program again), each a share of the items on players of its own, connected to it.

#include <cstdint>
#include <string>
#include <vector>

namespace sr::server {

struct ItemExamOptions {
    std::string resultsUrl;          // the database the site reads (its role makes the exam's own database: CREATEDB)
    std::string gameDir;             // the 1.188 client's files
    std::vector<std::string> routes; // the paths (game::ItemRouteName: "recovery", "equip", ...); empty: every one it knows
    std::string item;                // one item's codename alone
    int limit = 0;                   // at most this many items of each path (0: all)
    std::string build;               // named on the run (the repository's commit)
    bool keepDatabase = false;       // the exam's own database left for a look
    int workers = 1;                 // worker processes on the one game server (1: the exam's own thread plays)
    std::string logDir;              // the workers' logs (default lab-logs)
    // A worker's own (given by the exam's process): its share (every `parts`-th item of each path from the `part`-th),
    // the run it writes into, the game server's port, its certificate (a PEM file) and its database's name.
    int part = 0, parts = 1;
    std::int64_t run = 0;
    std::uint16_t port = 0;
    std::string pemFile, examDatabase;
};

struct ItemExamReport {
    std::int64_t run = 0;
    int passed = 0, failed = 0, skipped = 0;
};

// The paths the exam knows, in the order it runs them.
std::vector<std::string> ItemExamRoutes();

bool RunItemExam(const ItemExamOptions& options, ItemExamReport& report, std::string& error);

} // namespace sr::server
