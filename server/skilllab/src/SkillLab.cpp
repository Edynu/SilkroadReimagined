#include "sr/server/SkillLab.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <span>
#include <tuple>

#include <nlohmann/json.hpp>

#include "sr/assets/FileSystem.h"
#include "sr/game/SkillCatalogue.h"
#include "sr/game/SkillText.h"
#include "sr/server/Database.h"

namespace sr::server {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

std::string TagName(std::uint32_t code) {
    std::string s;
    for (int shift = 24; shift >= 0; shift -= 8) {
        if (const char c = char((code >> shift) & 0xFF); c != 0) s += c;
    }
    return s;
}

const char* PhaseName(int phase) {
    static const char* const kNames[] = {"READY", "WAIT", "SHOT", "ACT_OS", "ACT_OL", "ACT_OE", "ACT_S", "ACT_L", "DEACT", "S_RETURN"};
    return phase >= 0 && phase < int(std::size(kNames)) ? kNames[phase] : "?";
}

std::string Kinds(const game::SkillTraits& t) {
    const std::pair<bool, const char*> flags[] = {
        {t.passive, "passive"}, {t.attack, "attack"},   {t.area, "area"},       {t.chain, "chain"},       {t.states, "states"},
        {t.imbue, "imbue"},     {t.buff, "buff"},       {t.debuff, "debuff"},   {t.toggle, "toggle"},     {t.heal, "heal"},
        {t.cure, "cure"},       {t.resurrect, "resurrect"}, {t.harmony, "harmony"}, {t.taunt, "taunt"},   {t.stealth, "stealth"},
        {t.detect, "detect"},   {t.teleport, "teleport"}, {t.transform, "transform"}, {t.summon, "summon"}, {t.trap, "trap"},
        {t.link, "link"},       {t.reflect, "reflect"},
    };
    std::string out;
    for (const auto& [on, name] : flags) {
        if (!on) continue;
        if (!out.empty()) out += ',';
        out += name;
    }
    return out;
}

const char* TargetName(game::SkillTraits::Target t) {
    switch (t) {
    case game::SkillTraits::Target::Self: return "self";
    case game::SkillTraits::Target::Party: return "party";
    case game::SkillTraits::Target::Ally: return "ally";
    case game::SkillTraits::Target::Enemy: return "enemy";
    case game::SkillTraits::Target::DeadBody: return "dead body";
    case game::SkillTraits::Target::Area: return "area";
    }
    return "?";
}

json Tags(const assets::SkillRow& row) {
    json tags = json::array();
    assets::ForEachSkillParam(row, [&](std::uint32_t code, std::span<const std::uint32_t> args) {
        json a = json::array();
        for (const std::uint32_t v : args) a.push_back(std::int64_t(std::int32_t(v)));
        tags.push_back({{"tag", TagName(code)}, {"args", std::move(a)}});
    });
    return tags;
}

json Times(const assets::SkillTable& skills, const assets::SkillRow& row) {
    const game::SkillTimes t = game::TimesOf(skills, row);
    return {{"preparingMs", t.preparingMs}, {"castingMs", t.castingMs}, {"rowMs", t.rowMs}, {"holdMs", t.HoldMs()},
            {"cycleMs", t.CycleMs(row.autoAttack != 0)}, {"reuseMs", t.reuseMs}, {"coolTimeMs", row.coolTimeMs}, {"cooldownGroup", t.cooldownGroup}};
}

json Presentation(const LabData& data, const assets::SkillRow& row) {
    const bool named = !row.baseName.empty() && row.baseName != "xxx";
    const assets::SkillAnimation* a = data.animations.Find(named ? row.baseName : row.codename);
    if (a == nullptr) return {{"found", false}};
    const auto clips = [](const std::vector<int>& states) {
        json out = json::array();
        for (const int s : states) out.push_back(assets::AnimationStateName(s));
        return out;
    };
    // The checks: what our client would play or load and find (the files of the game data alone; how it looks is the
    // client exam's, scenario 5).
    json clipMissing = json::array(), clipDefault = json::array(), effectMissing = json::array(), soundMissing = json::array();
    int clipsChecked = 0, effectsChecked = 0, soundsChecked = 0;
    std::set<std::string> effectsSeen, soundsSeen;
    const auto effect = [&](const std::string& path) {
        if (path.empty() || !effectsSeen.insert(path).second) return;
        ++effectsChecked;
        if (data.files != nullptr && !data.files->Exists(path)) effectMissing.push_back(path);
    };
    const auto sound = [&](const std::string& path) {
        if (path.empty() || !soundsSeen.insert(path).second) return;
        ++soundsChecked;
        if (data.files != nullptr && !data.files->Exists(path)) soundMissing.push_back(path);
    };
    json stages = json::array();
    for (const assets::SkillEffectStage& s : a->stages) {
        json st = {{"phase", PhaseName(s.phase)}, {"action", s.action}, {"move", s.move}};
        if (s.startEvent != 0) st["event"] = s.startEvent;
        if (s.damageEvent) st["damage"] = true;
        if (!s.object.empty()) st["object"] = s.object;
        if (!s.arrival.empty()) st["arrival"] = s.arrival;
        if (!s.start.bone.empty()) st["from"] = s.start.bone;
        if (!s.target.bone.empty()) st["to"] = s.target.bone;
        if (!s.scripts.empty()) st["scripts"] = s.scripts;
        if (!s.soundBegin.empty()) st["soundBegin"] = s.soundBegin;
        if (!s.soundEnd.empty()) st["soundEnd"] = s.soundEnd;
        stages.push_back(std::move(st));
        const auto file = [](const std::string& p) {
            std::string l = p;
            for (char& ch : l) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            return l.ends_with(".efp") || l.ends_with(".bsr") || l.ends_with(".bms") ? p : std::string();
        };
        effect(file(s.object));
        effect(file(s.arrival));
        sound(s.soundBegin);
        sound(s.soundEnd);
    }
    json hits = json::object();
    for (const auto& [key, value] : {std::pair{"blocked", &a->defenseEffect}, {"hit", &a->damageEffect}, {"critical", &a->criticalEffect},
                                     {"arrowTrail", &a->arrowTrail}, {"arrowForce", &a->arrowForce}}) {
        if (value->empty()) continue;
        hits[key] = *value;
        effect(*value);
    }
    effect(a->objectEffect);
    // The clips in its row's set (what our client plays, skillaniset2 col 5), on each body of its race; a clip the set
    // lacks comes from the empty-handed set ("default"), one found nowhere is a skill shown wrong.
    std::vector<int> states = a->ready;
    states.insert(states.end(), a->wait.begin(), a->wait.end());
    states.insert(states.end(), a->shot.begin(), a->shot.end());
    const std::string set = a->group.empty() ? std::string("default") : a->group;
    for (const LabData::Body& body : data.bodies) {
        if (row.race != body.race && row.race != 3) continue;
        for (const int state : states) {
            ++clipsChecked;
            if (!body.sets.StateAnimation(set, std::uint32_t(state)).empty()) continue;
            const std::string what = assets::AnimationStateName(state) + " (" + body.name + ", set " + set + ")";
            if (!body.sets.StateAnimation("default", std::uint32_t(state)).empty()) {
                clipDefault.push_back(what);
            } else {
                clipMissing.push_back(what);
            }
        }
    }
    json p = {{"found", true}, {"set", a->group}, {"ready", clips(a->ready)}, {"wait", clips(a->wait)}, {"shot", clips(a->shot)}, {"stages", std::move(stages)},
              {"hitEffects", std::move(hits)}, {"hideWeapon", a->hideWeapon}};
    if (!a->objectEffect.empty()) p["objectEffect"] = a->objectEffect;
    if (a->trailMs > 0) p["trail"] = {{"ms", a->trailMs}, {"texture", a->trailTexture}};
    p["checks"] = {{"clips", {{"checked", clipsChecked}, {"missing", std::move(clipMissing)}, {"fromDefault", std::move(clipDefault)}}},
                   {"effects", {{"checked", effectsChecked}, {"missing", std::move(effectMissing)}}},
                   {"sounds", {{"checked", soundsChecked}, {"missing", std::move(soundMissing)}}}};
    return p;
}

} // namespace

bool LoadLabData(const assets::FileSystem& files, LabData& out, std::string& error) {
    if (!out.skills.Load(files, error)) return false;
    if (!out.animations.Load(files, error)) return false;
    if (!assets::LoadTextCatalog(files, "server_dep\\silkroad\\textdata\\textuisystem.txt", out.text, error)) return false;
    // In 1.188 textdataname.txt lists the files that hold the names (TextData_Object.txt, TextData_Equip&Skill.txt, ...).
    if (!assets::LoadTextCatalog(files, "server_dep\\silkroad\\textdata\\textdataname.txt", out.text, error)) return false;
    // The players' bodies, their animation sets (what the clips are looked up in).
    out.files = &files;
    assets::CharacterTable characters;
    if (!characters.Load(files, error)) return false;
    for (const auto& [code, race, name] : {std::tuple{"CHAR_CH_MAN_ADVENTURER", 0, "China man"}, {"CHAR_CH_WOMAN_ADVENTURER", 0, "China woman"},
                                           {"CHAR_EU_MAN_NOBLE", 1, "Europe man"}, {"CHAR_EU_WOMAN_NOBLE", 1, "Europe woman"}}) {
        const assets::ObjectRow* row = characters.Find(std::string_view(code));
        if (row == nullptr) return (error = std::string("no body ") + code), false;
        assets::ModelData model;
        if (!assets::LoadModel(files, assets::CharacterTable::ResolveModel(files, *row), model, error)) return false;
        LabData::Body body;
        body.race = race;
        body.name = name;
        body.sets.animationPaths = model.animationPaths;
        body.sets.animationSets = model.animationSets;
        out.bodies.push_back(std::move(body));
    }
    return true;
}

std::vector<LabSkillRow> MakeLabSkillRows(const LabData& data) {
    const game::SkillTextLookup text = [&](const std::string& key) {
        const auto it = data.text.find(key);
        return it != data.text.end() ? it->second : key;
    };
    const auto named = [&](const std::string& key) {
        const auto it = data.text.find(key);
        return it != data.text.end() ? it->second : std::string();
    };
    std::vector<LabSkillRow> out;
    for (const game::SkillCatalogueEntry& e : game::BuildSkillCatalogue(data.skills)) {
        const assets::SkillRow& first = e.First();
        LabSkillRow row;
        row.code = e.code;
        row.group = first.group;
        row.race = first.race;
        row.mastery = e.mastery->id;
        row.masteryName = named(e.mastery->nameKey);
        row.board = (first.boardRow == 255 ? 99 : first.boardRow) * 8 + (first.boardColumn == 255 ? 7 : first.boardColumn);
        row.name = named(first.nameKey);
        if (row.name.empty()) row.name = first.codename;
        row.kinds = Kinds(e.traits);
        row.route = game::SkillRouteName(e.route);
        row.motion = game::SkillMotionName(e.motion);
        row.levels = int(e.ranks.size());
        row.icon = first.iconPath;
        json weapons = json::array();
        for (const int kind : first.weaponKinds) {
            if (kind == 255) continue;
            const char* key = game::WeaponTextKey(kind);
            const std::string name = key != nullptr ? text(key) : std::to_string(kind);
            weapons.push_back({{"kind", kind}, {"name", name}});
            if (!row.weapons.empty()) row.weapons += ", ";
            row.weapons += name;
        }
        json ranks = json::array();
        for (const assets::SkillRow* r : e.ranks) {
            json chain = json::array();
            for (const assets::SkillRow* c : game::ChainOf(data.skills, *r)) {
                chain.push_back({{"id", c->id}, {"codename", c->codename}, {"durationMs", c->durationMs}, {"hits", c->attack ? c->hits : 0}, {"tags", Tags(*c)}});
            }
            const game::SkillItemNeeds items = game::SkillItemLine(*r, text);
            ranks.push_back({
                {"id", r->id},
                {"codename", r->codename},
                {"level", r->level},
                {"masteryLevel", r->masteryLevels[0]},
                {"sp", r->spCost},
                {"str", r->requiredStr},
                {"int", r->requiredInt},
                {"costs", {{"mp", r->mpCost}, {"hp", r->hpCost}, {"mpRatio", r->mpCostRatio}, {"hpRatio", r->hpCostRatio}, {"ammunition", r->consumesAmmo}}},
                {"times", Times(data.skills, *r)},
                {"hits", game::HitsOf(data.skills, *r)},
                {"range", r->range},
                {"chain", std::move(chain)},
                {"tooltip", {{"costs", game::SkillCostLines(*r, text)}, {"items", items.line}, {"effects", game::SkillEffectLines(*r, &data.skills, text)}}},
                {"tags", Tags(*r)},
            });
        }
        const json presentation = Presentation(data, first);
        if (presentation.contains("checks")) {
            const json& k = presentation["checks"];
            row.files = k["clips"]["missing"].empty() && k["effects"]["missing"].empty() && k["sounds"]["missing"].empty() ? "ok" : "missing";
        }
        const json j = {
            {"code", e.code},
            {"name", row.name},
            {"description", named(first.descKey)},
            {"study", named(first.studyKey)},
            {"race", first.race == 0 ? "China" : first.race == 1 ? "Europe" : "both"},
            {"mastery", {{"id", e.mastery->id}, {"name", row.masteryName}}},
            {"board", {{"row", first.boardRow}, {"column", first.boardColumn}}},
            {"activity", first.activity},
            {"kinds", row.kinds},
            {"target", TargetName(e.traits.target)},
            {"route", {{"name", row.route}, {"text", game::SkillRouteText(e.route)}}},
            {"motion", {{"name", row.motion}, {"holdMs", game::TimesOf(data.skills, e.Top()).CycleMs(e.Top().autoAttack != 0)}}},
            {"weapons", std::move(weapons)},
            {"targets",
             {{"required", first.targetRequired}, {"self", first.targetSelf}, {"ally", first.targetAlly}, {"party", first.targetParty},
              {"enemyMonster", first.targetEnemyMonster}, {"enemyPlayer", first.targetEnemyPlayer}, {"neutral", first.targetNeutral},
              {"any", first.targetAny}, {"deadBody", first.targetDeadBody}, {"building", first.targetBuilding}}},
            {"action",
             {{"autoAttack", first.autoAttack}, {"overlap", first.overlap}, {"interruptible", first.interruptible}, {"inTown", first.inTown},
              {"flyingSpeed", first.flyingSpeed}, {"range", first.range}}},
            {"presentation", presentation},
            {"ranks", std::move(ranks)},
        };
        row.data = j.dump();
        out.push_back(std::move(row));
    }
    return out;
}

bool SaveLabSkills(Database& db, const std::vector<LabSkillRow>& rows, const std::string& build, std::string& error) {
    return db.Transaction(
        [&](Database& d) {
            if (!d.Query("CREATE TEMP TABLE lab_seen (code varchar(64) PRIMARY KEY) ON COMMIT DROP").Ok()) return false;
            for (const LabSkillRow& r : rows) {
                const QueryResult q = d.Query(
                    "INSERT INTO sro_vt_account.sr_lab_skills (code, group_id, race, mastery, mastery_name, board, name, kinds, route, motion, weapons, "
                    "levels, icon, data, build, files, updated_at) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14::jsonb, $15, $16, now()) "
                    "ON CONFLICT (code) DO UPDATE SET group_id = EXCLUDED.group_id, race = EXCLUDED.race, mastery = EXCLUDED.mastery, "
                    "mastery_name = EXCLUDED.mastery_name, board = EXCLUDED.board, name = EXCLUDED.name, kinds = EXCLUDED.kinds, route = EXCLUDED.route, "
                    "motion = EXCLUDED.motion, weapons = EXCLUDED.weapons, levels = EXCLUDED.levels, icon = EXCLUDED.icon, data = EXCLUDED.data, "
                    "build = EXCLUDED.build, files = EXCLUDED.files, updated_at = now()",
                    {r.code, std::int64_t(r.group), r.race, std::int64_t(r.mastery), r.masteryName, r.board, r.name, r.kinds, r.route, r.motion, r.weapons,
                     r.levels, r.icon, r.data, build, r.files});
                if (!q.Ok()) {
                    error = r.code + ": " + q.Error();
                    return false;
                }
                if (!d.Query("INSERT INTO lab_seen (code) VALUES ($1) ON CONFLICT DO NOTHING", {r.code}).Ok()) return false;
            }
            return d.Query("DELETE FROM sro_vt_account.sr_lab_skills s WHERE NOT EXISTS (SELECT 1 FROM lab_seen n WHERE n.code = s.code)").Ok();
        },
        error);
}

std::string BuildCommit(const std::string& root) {
    fs::path git = fs::path(root) / ".git";
    const auto read = [](const fs::path& p) {
        std::ifstream in(p);
        std::string line;
        std::getline(in, line);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        return line;
    };
    // A worktree's .git is a file naming its own folder ("gitdir: ..."), whose commondir names the repository's (the
    // branches' refs and packed-refs).
    fs::path common = git;
    if (std::error_code ec; fs::is_regular_file(git, ec)) {
        const std::string link = read(git);
        if (!link.starts_with("gitdir: ")) return "unknown";
        git = fs::path(root) / fs::path(link.substr(8));
        common = git;
        if (const std::string c = read(git / "commondir"); !c.empty()) common = git / fs::path(c);
    }
    std::string head = read(git / "HEAD");
    if (head.starts_with("ref: ")) {
        const std::string ref = head.substr(5);
        std::string hash = read(git / ref);
        if (hash.empty()) hash = read(common / ref);
        if (hash.empty()) {
            std::ifstream packed(common / "packed-refs");
            for (std::string line; std::getline(packed, line);) {
                if (line.size() > 41 && line.substr(41) == ref) hash = line.substr(0, 40);
            }
        }
        head = hash;
    }
    return head.size() >= 10 ? head.substr(0, 10) : std::string("unknown");
}

} // namespace sr::server
