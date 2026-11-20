#pragma once

// The skill lab's exam (L3..L6): our game server run in-process on a database of its own (no
// nests: nothing but the lab's monsters is about), a character of each mastery made fresh, the mastery learned and its
// skills cast one by one through the protocol as a player's client would, what happened measured against what the data
// says (game::SkillCatalogue), each result written to the lab's tables (sr_lab_runs, sr_lab_results) for the site's
// /dev. Scenario 1 (still): a monster that never moves nor fights, a million health.

#include <cstdint>
#include <string>
#include <vector>

namespace sr::server {

struct ExamOptions {
    std::string resultsUrl;              // the database the site reads (its role makes the exam's own database: CREATEDB)
    std::string gameDir;                 // the 1.188 client's files
    std::vector<std::string> masteries;  // ids or names ("258", "Spear", "Bow"); empty: every mastery
    int race = -1;                       // 0 China, 1 Europe, -1 both
    std::string skill;                   // a group's code: that skill alone (its mastery's character made for it)
    std::vector<int> scenarios{1};
    std::string build;                   // named on the run (the repository's commit)
    bool keepDatabase = false;           // the exam's own database left for a look
    // A share of the work (SroSkillLab --workers): every `parts`-th group of each mastery from the `part`-th (0-based);
    // the mastery's armour comparison (scenario 4) in part 0 alone.
    int part = 0, parts = 1;
    std::int64_t run = 0;                // an existing run to write into (the workers of one exam); 0: a new one
};
// A new run's row (the parallel exam's, its workers writing into it): its id, 0 when it could not be made.
std::int64_t NewExamRun(const std::string& resultsUrl, const std::string& build, const std::string& filter, const std::string& scenarios, std::string& error);

struct ExamReport {
    std::int64_t run = 0;
    int passed = 0, failed = 0, skipped = 0;
};

bool RunExam(const ExamOptions& options, ExamReport& report, std::string& error);

} // namespace sr::server
