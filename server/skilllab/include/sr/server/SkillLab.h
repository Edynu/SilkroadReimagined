#pragma once

// The skill lab on the server side: its list into the database (game::SkillCatalogue's entries
// as the rows of sr_lab_skills, each with all its data as JSON, for the site's /dev and the client's F1 Skill lab
// through the game server); the exam comes next (L3..L6).

#include <cstdint>
#include <string>
#include <vector>

#include "sr/assets/GameData.h"
#include "sr/assets/Model.h"
#include "sr/assets/SkillAnimation.h"
#include "sr/assets/UiLayout.h"

namespace sr::assets {
class FileSystem;
}

namespace sr::server {

class Database;

// What the list is made of: the skill table, the skills' presentation, the client's texts, and for the presentation's
// checks the game files and the players' four bodies (China and Europe, a man and a woman: their animation sets).
struct LabData {
    assets::SkillTable skills;
    assets::SkillAnimationTable animations;
    assets::TextCatalog text;
    const assets::FileSystem* files = nullptr;
    struct Body {
        int race = 0;
        std::string name;
        assets::ResourceData sets;
    };
    std::vector<Body> bodies;
};
bool LoadLabData(const assets::FileSystem& files, LabData& out, std::string& error);

// A row of sr_lab_skills: the columns the site filters by, and `data` (JSON) with everything else.
struct LabSkillRow {
    std::string code;
    std::uint32_t group = 0;
    int race = 0; // 0 China, 1 Europe, 3 both
    std::uint32_t mastery = 0;
    std::string masteryName;
    int board = 0; // its place on the mastery's board (row * 8 + column), the site's order
    std::string name, kinds, route, motion, weapons;
    int levels = 0;
    std::string icon;
    std::string files; // its presentation's files: "ok", "missing" (a clip, an effect or a sound not found), "" none to check
    std::string data;
};
std::vector<LabSkillRow> MakeLabSkillRows(const LabData& data);

// The list replaced in one transaction (the groups no longer in it go); `build` names the build that made it.
bool SaveLabSkills(Database& db, const std::vector<LabSkillRow>& rows, const std::string& build, std::string& error);

// The repository's HEAD commit, short (read from .git under `root`), else "unknown".
std::string BuildCommit(const std::string& root = ".");

} // namespace sr::server
