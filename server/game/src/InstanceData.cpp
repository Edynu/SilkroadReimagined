// The Forgotten World's catalog from the database's original tables and the client's files.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <numbers>

#include "sr/assets/Dungeon.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/assets/Model.h"
#include "sr/assets/Terrain.h"
#include "sr/game/NavMesh.h"
#include "sr/server/Database.h"
#include "sr/server/Instances.h"

namespace sr::server {

namespace proto = game::proto;

namespace {

std::string Trim(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
    std::size_t first = 0;
    while (first < text.size() && text[first] == ' ') ++first;
    return text.substr(first);
}
std::string Lower(std::string text) {
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
std::string WithoutPrefix(std::string text, std::string_view prefix) {
    if (text.starts_with(prefix)) text.erase(0, prefix.size());
    return text;
}
// A nest's heading (wInitialDir, a turn of 65536 from +x toward +z) as our yaw (from +z toward +x).
float YawOf(std::int64_t dir) { return float(std::numbers::pi / 2.0 - double(std::uint16_t(dir)) / 65536.0 * 2.0 * std::numbers::pi); }

struct TriggerTables {
    std::unordered_map<std::int64_t, TriggerSpec> triggers;                  // by id
    std::unordered_map<std::int64_t, std::vector<std::int64_t>> byCategory; // category -> triggers
    std::unordered_map<std::int64_t, std::vector<std::int64_t>> byWorld;    // world -> categories
};

bool LoadTriggers(Database& db, TriggerTables& out, std::string& error) {
    const auto fail = [&](const QueryResult& r) {
        error = r.Error();
        return false;
    };
    QueryResult common = db.Query("SELECT id, codename128 FROM _reftriggercommon WHERE service = 1");
    if (!common.Ok()) return fail(common);
    std::unordered_map<std::int64_t, std::string> kind;
    for (int i = 0; i < common.Rows(); ++i) kind[common.Int(i, 0)] = WithoutPrefix(Trim(common.Text(i, 1)), "TRIGGER_");
    // The parameters by their group.
    std::unordered_map<std::string, std::map<std::string, std::string>> conditionParams, actionParams;
    for (const auto& [table, into] : {std::pair{"_reftriggerconditionparam", &conditionParams}, std::pair{"_reftriggeractionparam", &actionParams}}) {
        QueryResult r = db.Query(std::format("SELECT groupcodename128, valuecodename128, value FROM {} WHERE service = 1 ORDER BY id", table));
        if (!r.Ok()) return fail(r);
        for (int i = 0; i < r.Rows(); ++i) (*into)[Trim(r.Text(i, 0))][Trim(r.Text(i, 1))] = Trim(r.Text(i, 2));
    }
    QueryResult t = db.Query("SELECT id, codename128, isactive, isrepeat FROM _reftrigger WHERE service = 1");
    if (!t.Ok()) return fail(t);
    for (int i = 0; i < t.Rows(); ++i) {
        TriggerSpec& spec = out.triggers[t.Int(i, 0)];
        spec.id = std::uint32_t(t.Int(i, 0));
        spec.codename = Trim(t.Text(i, 1));
        spec.active = t.Int(i, 2) != 0;
        spec.repeat = t.Int(i, 3) != 0;
    }
    QueryResult ev = db.Query("SELECT b.triggerid, e.reftriggercommonid FROM _reftriggerbindevent b JOIN _reftriggerevent e ON e.id = b.triggereventid "
                              "WHERE b.service = 1 AND e.service = 1 ORDER BY b.id");
    if (!ev.Ok()) return fail(ev);
    for (int i = 0; i < ev.Rows(); ++i) {
        if (auto it = out.triggers.find(ev.Int(i, 0)); it != out.triggers.end()) it->second.events.push_back(kind[ev.Int(i, 1)]);
    }
    QueryResult co = db.Query("SELECT b.triggerid, c.reftriggercommonid, c.ontrue, c.onfalse, c.paramgroupcodename128 FROM _reftriggerbindcondition b "
                              "JOIN _reftriggercondition c ON c.id = b.triggerconditionid WHERE b.service = 1 AND c.service = 1 ORDER BY b.triggerid, c.sequence, b.id");
    if (!co.Ok()) return fail(co);
    for (int i = 0; i < co.Rows(); ++i) {
        const auto it = out.triggers.find(co.Int(i, 0));
        if (it == out.triggers.end()) continue;
        TriggerSpec::Condition c;
        c.kind = kind[co.Int(i, 1)];
        c.onTrue = Trim(co.Text(i, 2));
        c.onFalse = Trim(co.Text(i, 3));
        if (const auto p = conditionParams.find(Trim(co.Text(i, 4))); p != conditionParams.end()) c.params = p->second;
        it->second.conditions.push_back(std::move(c));
    }
    QueryResult ac = db.Query("SELECT b.triggerid, a.reftriggercommonid, a.delay, a.paramgroupcodename128 FROM _reftriggerbindaction b "
                              "JOIN _reftriggeraction a ON a.id = b.triggeractionid WHERE b.service = 1 AND a.service = 1 ORDER BY b.triggerid, b.id");
    if (!ac.Ok()) return fail(ac);
    for (int i = 0; i < ac.Rows(); ++i) {
        const auto it = out.triggers.find(ac.Int(i, 0));
        if (it == out.triggers.end()) continue;
        TriggerSpec::Action a;
        a.kind = kind[ac.Int(i, 1)];
        a.delayMs = std::uint32_t(std::max<std::int64_t>(0, ac.Int(i, 2)));
        if (const auto p = actionParams.find(Trim(ac.Text(i, 3))); p != actionParams.end()) a.params = p->second;
        it->second.actions.push_back(std::move(a));
    }
    QueryResult cat = db.Query("SELECT triggercategoryid, triggerid FROM _reftriggercategorybindtrigger WHERE service = 1 ORDER BY id");
    if (!cat.Ok()) return fail(cat);
    for (int i = 0; i < cat.Rows(); ++i) out.byCategory[cat.Int(i, 0)].push_back(cat.Int(i, 1));
    QueryResult bind = db.Query("SELECT gameworldid, triggercategoryid FROM _refgameworldbindtriggercategory WHERE service = 1 ORDER BY id");
    if (!bind.Ok()) return fail(bind);
    for (int i = 0; i < bind.Rows(); ++i) out.byWorld[bind.Int(i, 0)].push_back(bind.Int(i, 1));
    return true;
}

std::vector<TriggerSpec> TriggersOf(const TriggerTables& t, std::int64_t world) {
    std::vector<TriggerSpec> out;
    const auto cats = t.byWorld.find(world);
    if (cats == t.byWorld.end()) return out;
    for (const std::int64_t c : cats->second) {
        const auto list = t.byCategory.find(c);
        if (list == t.byCategory.end()) continue;
        for (const std::int64_t id : list->second) {
            if (const auto it = t.triggers.find(id); it != t.triggers.end()) out.push_back(it->second);
        }
    }
    return out;
}

// A dungeon's doors (the Flame Mountain's): its rooms' floor navmeshes name them as
// event zones ("pos_structure_god_big_gate_flame_1_r90": the zone and its turn in degrees), marking the doorway's cells
// or edges; the door stands at their middle, turned by its room's yaw and its own. By the zone's name.
struct DoorSpot {
    proto::WorldPosition position;
    float yaw = 0;
};
std::unordered_map<std::string, DoorSpot> DungeonDoorSpots(const assets::FileSystem& files, std::uint16_t region, std::vector<std::string>& notes) {
    std::unordered_map<std::string, DoorSpot> out;
    std::string error;
    std::vector<assets::DungeonInfoEntry> dungeons;
    const auto info = files.Read("dungeon\\dungeoninfo.txt");
    if (!info || !assets::DecodeDungeonInfo(info->Bytes(), dungeons, error)) return out;
    const auto entry = std::find_if(dungeons.begin(), dungeons.end(), [&](const assets::DungeonInfoEntry& e) { return e.regionId == region; });
    const auto dof = entry != dungeons.end() ? files.Read(entry->path) : std::nullopt;
    assets::DungeonData data;
    if (!dof || !assets::DecodeDof(dof->Bytes(), data, error)) {
        notes.push_back(std::format("dungeon {}: {}", region, error.empty() ? "not found" : error));
        return out;
    }
    for (const assets::DungeonData::Block& block : data.blocks) {
        assets::ResourceData resource;
        const auto bsr = files.Read(block.path);
        if (!bsr || !assets::DecodeBsr(bsr->Bytes(), resource, error) || resource.navMeshPath.empty()) continue;
        const auto bms = files.Read(resource.navMeshPath);
        assets::ObjectNavMesh raw;
        if (!bms || !assets::DecodeBmsNavMesh(bms->Bytes(), raw, error) || raw.empty()) continue;
        const auto mesh = game::NavObjectMesh::Build(raw);
        const float c = std::cos(block.yaw), s = std::sin(block.yaw);
        for (std::size_t k = 0; k < mesh->events.size(); ++k) {
            std::string name = mesh->events[k];
            if (!name.starts_with("pos_structure_god")) continue;
            float sx = 0, sy = 0, sz = 0;
            int n = 0;
            for (const auto& e : mesh->edges) {
                if ((e.event & 0xC0u) == 0 || (e.event & 0x3Fu) != k) continue;
                sx += 0.5f * (mesh->vertices[e.a].x + mesh->vertices[e.b].x);
                sy += 0.5f * (mesh->vertices[e.a].y + mesh->vertices[e.b].y);
                sz += 0.5f * (mesh->vertices[e.a].z + mesh->vertices[e.b].z);
                ++n;
            }
            for (const auto& t : mesh->triangles) {
                if ((t.event & 0xC0u) == 0 || (t.event & 0x3Fu) != k) continue;
                sx += t.cx;
                sy += t.cy;
                sz += t.cz;
                ++n;
            }
            if (n == 0) continue;
            const float lx = sx / float(n), lz = sz / float(n);
            float turn = 0.0f;
            if (const std::size_t r = name.rfind("_r"); r != std::string::npos) {
                turn = float(std::atof(name.c_str() + r + 2)) * std::numbers::pi_v<float> / 180.0f;
                name.resize(r);
            }
            DoorSpot spot;
            // Block space to the dungeon's, as NavDungeon::Block::ToWorld.
            spot.position = {region, lx * c - lz * s + block.position[0], block.position[1] + sy / float(n), lx * s + lz * c + block.position[2]};
            spot.yaw = block.yaw + turn;
            for (char& ch : name) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            out[name] = spot;
        }
    }
    return out;
}

} // namespace

std::string TriggerSpec::Param(const Action& a, const std::string& name) const {
    const auto it = a.params.find(name);
    return it != a.params.end() ? it->second : std::string();
}

const GameWorldSpec* GameWorldCatalog::TombOfPoint(std::uint32_t point) const {
    for (const GameWorldSpec& w : tombs) {
        if (point != 0 && w.gatePoint == point) return &w;
    }
    return nullptr;
}

const GameWorldSpec* GameWorldCatalog::World(std::uint32_t id) const {
    for (const GameWorldSpec& w : worlds) {
        if (w.id == id) return &w;
    }
    return nullptr;
}
const CollectionTheme* GameWorldCatalog::Theme(std::uint32_t id) const {
    for (const CollectionTheme& t : themes) {
        if (t.id == id) return &t;
    }
    return nullptr;
}
const CollectionTheme* GameWorldCatalog::Theme(std::string_view codename) const {
    for (const CollectionTheme& t : themes) {
        if (t.codename == codename) return &t;
    }
    return nullptr;
}

bool LoadGameWorlds(Database& db, const assets::FileSystem& files, const assets::CharacterTable& characters, const assets::ItemTable& items,
                    GameWorldCatalog& out, std::vector<std::string>& notes, std::string& error) {
    out = {};
    const auto fail = [&](const QueryResult& r) {
        error = r.Error();
        return false;
    };
    // The worlds: the Forgotten World's (their codenames INS_GOD_*), the Cave of Discipline, the Pharaoh's tomb's.
    QueryResult w = db.Query("SELECT id, worldcodename128, worldmaxcount, worldmaxusercount, worlddurationtime, worldemptyremaintime, configgroupcodename128 "
                             "FROM _refgame_world WHERE worldcodename128 LIKE 'INS_GOD_%' OR worldcodename128 = 'INS_SIEGE_DUNGEON' "
                             "OR worldcodename128 LIKE 'INS_PHARAOH_%' OR (worldcodename128 LIKE 'INS_TEMPLE_%' AND worldcodename128 <> 'INS_TEMPLE_DEFAULT') "
                             "ORDER BY id");
    if (!w.Ok()) return fail(w);
    std::unordered_map<std::string, std::map<std::string, std::string>> config;
    QueryResult c = db.Query("SELECT groupcodename128, valuecodename128, value FROM _refgame_world_config WHERE service = 1 ORDER BY id");
    if (!c.Ok()) return fail(c);
    std::unordered_map<std::string, std::vector<std::uint32_t>> warnings; // GAMEWORLD_DISAPPEARANCE_TIME's rows, by group
    for (int i = 0; i < c.Rows(); ++i) {
        config[Trim(c.Text(i, 0))][Trim(c.Text(i, 1))] = Trim(c.Text(i, 2));
        if (Trim(c.Text(i, 1)) == "GAMEWORLD_DISAPPEARANCE_TIME") warnings[Trim(c.Text(i, 0))].push_back(std::uint32_t(std::atoi(Trim(c.Text(i, 2)).c_str())));
    }
    // The tomb's gates: the point of each world that leads in (GATE_TOMB_GATE_OUT_1..3) and its name.
    QueryResult tg = db.Query("SELECT id, genworldid, zonename128 FROM _refteleport WHERE service = 1 AND assocrefobjcodename128 LIKE 'STORE_TOMB_GATE_OUT_%'");
    if (!tg.Ok()) return fail(tg);
    QueryResult g = db.Query("SELECT valuecodename128, value FROM _refgameworldgroup_config WHERE service = 1 AND groupcodename128 = 'GROUP_CONFIG_INS_GOD'");
    if (!g.Ok()) return fail(g);
    for (int i = 0; i < g.Rows(); ++i) {
        const std::string key = Trim(g.Text(i, 0)), value = Trim(g.Text(i, 1));
        // Minutes (the original multiplies by 60: 300 is five hours).
        if (key == "ENTER_WORLD_GROUP_COOL_TIME") out.reentrySeconds = std::uint32_t(std::max(0, std::atoi(value.c_str()))) * 60;
        if (key == "TIMEDJOB_COOL_TIME_FORCLIENT") out.jobSeconds = std::uint32_t(std::max(0, std::atoi(value.c_str()))) * 60;
        if (key == "TIMEDJOB_ICONURL_FORCLIENT") {
            out.jobIcon = value;
            for (std::size_t at = out.jobIcon.find("\\\\"); at != std::string::npos; at = out.jobIcon.find("\\\\")) out.jobIcon.erase(at, 1);
        }
        if (key == "TIMEDJOB_SUBJECT_FORCLIENT") out.jobSubject = value;
        if (key == "TIMEDJOB_EXPANATION_FORCLIENT") out.jobText = value;
    }
    QueryResult start = db.Query("SELECT worldid, regionid, posx, posy, posz FROM _refinstance_world_start_pos");
    if (!start.Ok()) return fail(start);
    std::unordered_map<std::int64_t, proto::WorldPosition> starts;
    for (int i = 0; i < start.Rows(); ++i) {
        starts.try_emplace(start.Int(i, 0), proto::WorldPosition{std::uint16_t(start.Int(i, 1)), float(start.Int(i, 2)), float(start.Int(i, 3)),
                                                                 float(start.Int(i, 4))});
    }
    // The nests of each world's hives (Tab_RefHive.GameWorldID), with their tactics' aggression and sight.
    QueryResult n = db.Query(
        "SELECT h.gameworldid, h.hatchobjtype, n.dwnestid, n.dwhiveid, o.codename128, n.nregiondbid, n.flocalposx, n.flocalposy, n.flocalposz, "
        "       n.winitialdir, n.nradius, n.ngenerateradius, CASE WHEN c.dwtacticsid IS NULL THEN 0 ELSE n.nchampiongenpercentage END, "
        "       n.dwdelaytimemin, n.dwdelaytimemax, n.dwmaxtotalcount, "
        "       n.btrespawn, n.bttype, t.btaggresstype, t.nsightrange, t.tracedata, t.bttraceboundary, coalesce(c.nsightrange, 0) "
        "FROM tab_refnest n JOIN tab_refhive h ON h.dwhiveid = n.dwhiveid JOIN tab_reftactics t ON t.dwtacticsid = n.dwtacticsid "
        "LEFT JOIN tab_reftactics c ON c.dwtacticsid = t.dwchampiontacticsid AND t.dwchampiontacticsid <> 0 "
        "JOIN _refobjcommon o ON o.id = t.dwobjid WHERE (h.gameworldid >= 63 AND h.gameworldid <= 84) OR h.gameworldid = 61 "
        "OR (h.gameworldid >= 51 AND h.gameworldid <= 59) ORDER BY n.dwnestid");
    if (!n.Ok()) return fail(n);
    // The doors: _RefEventZone's OBJECT zones (strParam1 the structure) at objectstring.ifo's spots.
    std::unordered_map<std::string, assets::NamedSpot> spots;
    if (const auto file = files.Read("navmesh\\objectstring.ifo")) {
        std::vector<assets::NamedSpot> list;
        if (!assets::DecodeObjectStrings(file->Bytes(), list, error)) return false;
        for (assets::NamedSpot& spot : list) spots[spot.name] = std::move(spot);
    } else {
        notes.push_back("navmesh\\objectstring.ifo is missing: no doors");
    }
    QueryResult z = db.Query("SELECT zonename, strparam1 FROM _refeventzone WHERE service = 1 AND eventname = 'OBJECT' AND zonename LIKE 'POS_STRUCTURE_GOD_%'");
    if (!z.Ok()) return fail(z);
    std::unordered_map<std::string, std::string> doorStructure;
    for (int i = 0; i < z.Rows(); ++i) doorStructure[Trim(z.Text(i, 0))] = Trim(z.Text(i, 1));
    TriggerTables triggers;
    if (!LoadTriggers(db, triggers, error)) return false;
    std::unordered_map<std::uint16_t, std::unordered_map<std::string, DoorSpot>> dungeonDoors; // by the dungeon's region

    for (int i = 0; i < w.Rows(); ++i) {
        GameWorldSpec spec;
        spec.id = std::uint32_t(w.Int(i, 0));
        spec.codename = Trim(w.Text(i, 1));
        spec.maxCount = std::uint32_t(std::max<std::int64_t>(1, w.Int(i, 2)));
        spec.maxUsers = std::uint32_t(std::max<std::int64_t>(1, w.Int(i, 3)));
        spec.durationMs = std::uint32_t(std::max<std::int64_t>(60'000, w.Int(i, 4)));
        spec.emptyMs = std::uint32_t(std::max<std::int64_t>(10'000, w.Int(i, 5)));
        const auto& cfg = config[Trim(w.Text(i, 6))];
        const auto number = [&](const char* key, int fallback) {
            const auto it = cfg.find(key);
            return it != cfg.end() ? std::atoi(it->second.c_str()) : fallback;
        };
        spec.minLevel = number("ENTER_LIMIT_CONDITION_LEVEL_MINIMUM_NUM", 1);
        spec.maxLevel = number("ENTER_LIMIT_CONDITION_LEVEL_MAXIMUM_NUM", 0);
        spec.gapMs = std::uint32_t(std::max(10'000, number("GAMEWORLD_DISAPPEARANCE_TIME", 300'000)));
        if (const auto it = cfg.find("ENTER_LIMIT_CONDITION_JOB_CLOTHES_TAKE_OFF"); it != cfg.end()) spec.noJobSuit = it->second == "ON";
        if (const auto it = cfg.find("ENTER_LIMIT_CONDITION_PARTY"); it != cfg.end()) spec.party = it->second == "ON";
        if (const auto it = cfg.find("ENTER_LIMIT_CONDITION_ENTRY"); it != cfg.end() && it->second == "NUM_LIMIT") {
            spec.entriesPerDay = std::max(0, number("ENTER_LIMIT_CONDITION_ENTRY_MAXIMUM_NUM", 0));
        }
        // The tomb's warnings before its end (the Forgotten World's one value is its gap's time: GameWorldSpec::gapMs).
        if (const auto it = warnings.find(Trim(w.Text(i, 6))); it != warnings.end() && spec.codename.starts_with("INS_PHARAOH_")) {
            spec.warnMs = it->second;
            std::sort(spec.warnMs.rbegin(), spec.warnMs.rend());
        }
        for (int k = 0; k < tg.Rows(); ++k) {
            if (std::uint32_t(tg.Int(k, 1)) != spec.id) continue;
            spec.gatePoint = std::uint32_t(tg.Int(k, 0));
            spec.nameKey = Trim(tg.Text(k, 2));
        }
        spec.config = cfg;
        // A temple has no start place of its own: its door's destination (ENTER_GATE_TARGAET_*).
        if (spec.codename.starts_with("INS_TEMPLE_") && !starts.contains(spec.id) && cfg.contains("ENTER_GATE_TARGAET_REGION")) {
            starts[spec.id] = proto::WorldPosition{std::uint16_t(number("ENTER_GATE_TARGAET_REGION", 0)), float(std::atof(cfg.at("ENTER_GATE_TARGAET_X").c_str())),
                                                   float(std::atof(cfg.at("ENTER_GATE_TARGAET_Y").c_str())), float(std::atof(cfg.at("ENTER_GATE_TARGAET_Z").c_str()))};
        }
        const auto s = starts.find(spec.id);
        if (s == starts.end()) {
            notes.push_back(spec.codename + ": no start place");
            continue;
        }
        spec.start = s->second;
        out.worlds.push_back(std::move(spec));
    }
    for (int i = 0; i < n.Rows(); ++i) {
        const std::uint32_t worldId = std::uint32_t(n.Int(i, 0));
        auto world = std::find_if(out.worlds.begin(), out.worlds.end(), [&](const GameWorldSpec& x) { return x.id == worldId; });
        if (world == out.worlds.end()) continue;
        const std::string code = Trim(n.Text(i, 4));
        const assets::ObjectRow* row = characters.Find(code);
        if (row == nullptr) {
            notes.push_back(std::format("{}: nest {}: {} is not in the client's characterdata", world->codename, n.Int(i, 2), code));
            continue;
        }
        const proto::WorldPosition at{std::uint16_t(n.Int(i, 5)), float(n.Double(i, 6)), float(n.Double(i, 7)), float(n.Double(i, 8))};
        if (n.Int(i, 1) == 2) { // an NPC hive
            world->npcs.push_back({row->refObjId, code, at, YawOf(n.Int(i, 9))});
            continue;
        }
        MonsterNest nest;
        nest.refObjId = row->refObjId;
        nest.position = at;
        nest.radius = float(n.Int(i, 10));
        nest.spawnRadius = float(n.Int(i, 11));
        nest.championPercent = int(n.Int(i, 12));
        nest.delayMin = float(n.Int(i, 13)) / 1000.0f;
        nest.delayMax = float(n.Int(i, 14)) / 1000.0f;
        nest.maxCount = int(std::max<std::int64_t>(1, n.Int(i, 15)));
        nest.respawn = n.Int(i, 16) != 0;
        nest.waiting = n.Int(i, 17) == 1;
        nest.aggressive = n.Int(i, 18) == 0;
        nest.sight = float(n.Int(i, 19));
        nest.trace = float(n.Int(i, 20));
        nest.traceBoundary = int(n.Int(i, 21));
        nest.championSight = float(n.Int(i, 22));
        // Its world's PARTYMONSTER_SPAWN (OFF in every instance world in service): no party monster unless ON.
        const auto party = world->config.find("PARTYMONSTER_SPAWN");
        nest.partyMonsters = party != world->config.end() && party->second == "ON";
        nest.nestId = std::uint32_t(n.Int(i, 2));
        nest.hive = std::uint32_t(n.Int(i, 3));
        nest.yaw = YawOf(n.Int(i, 9));
        world->nests.push_back(nest);
    }
    for (GameWorldSpec& spec : out.worlds) {
        spec.triggers = TriggersOf(triggers, spec.id);
        // Its doors: the zones its triggers switch.
        std::set<std::string> zones;
        for (const TriggerSpec& t : spec.triggers) {
            for (const TriggerSpec::Action& a : t.actions) {
                if (a.kind == "ACTION_SWITCH_EVENTZONE") zones.insert(t.Param(a, "EVENT_ZONE_SELECT"));
            }
        }
        int unplaced = 0;
        // A world in a dungeon: its doors are its rooms' (read once a dungeon).
        const bool inDungeon = (spec.start.region & 0x8000) != 0;
        if (inDungeon && !dungeonDoors.contains(spec.start.region)) dungeonDoors[spec.start.region] = DungeonDoorSpots(files, spec.start.region, notes);
        const auto DungeonSpot = [&](const std::string& zone) -> const DoorSpot* {
            if (!inDungeon) return nullptr;
            const auto& spots = dungeonDoors[spec.start.region];
            const auto it = spots.find(zone);
            return it != spots.end() ? &it->second : nullptr;
        };
        for (const std::string& zone : zones) {
            const auto structure = doorStructure.find(zone);
            const assets::ObjectRow* row = structure != doorStructure.end() ? characters.Find(structure->second) : nullptr;
            GameWorldSpec::Door d;
            d.zone = zone;
            d.navName = Lower(zone);
            if (const auto spot = spots.find(zone); spot != spots.end()) {
                d.position = {spot->second.region, spot->second.x, spot->second.y, spot->second.z};
                d.yaw = spot->second.yaw;
                d.navKey = spot->second.id;
            } else if (const DoorSpot* inside = DungeonSpot(zone); inside != nullptr) {
                d.position = inside->position;
                d.yaw = inside->yaw;
                d.navKey = 0; // walks stop at its cells and edges by its name
            } else {
                ++unplaced;
                continue;
            }
            if (row == nullptr) {
                ++unplaced;
                continue;
            }
            d.refObjId = row->refObjId;
            spec.doors.push_back(std::move(d));
        }
        if (unplaced > 0) notes.push_back(std::format("{}: {} doors without a place (they stand open)", spec.codename, unplaced));
    }
    out.sharedTriggers = TriggersOf(triggers, 1);

    // The holes (TID 3/3/12/7) and the gap's object.
    std::uint32_t gapModel = 0;
    if (QueryResult r = db.Query("SELECT id FROM _refobjcommon WHERE codename128 = 'INS_QUEST_TELEPORT'"); r.Ok() && r.Rows() > 0) gapModel = std::uint32_t(r.Int(0, 0));
    for (const auto& [code, row] : items.rows()) {
        if (row.typeId != std::array<int, 4>{3, 3, 12, 7} || !row.inService) continue;
        const std::string worldCode = Trim(row.paramTexts[1]);
        const auto world = std::find_if(out.worlds.begin(), out.worlds.end(), [&](const GameWorldSpec& x) { return x.codename == worldCode; });
        if (world == out.worlds.end()) continue;
        HoleSpec h;
        h.item = row.refObjId;
        h.world = world->id;
        h.grade = std::uint8_t(std::clamp(row.params[0], 1, 4));
        h.minLevel = row.params[2];
        h.maxLevel = row.params[3];
        h.theme = Trim(row.paramTexts[8]);
        h.gapModel = gapModel;
        h.waitMs = std::uint32_t(std::max(0, row.cooldownMs));
        h.waitGroup = std::uint32_t(std::max(0, row.cooldownGroup));
        out.holes[h.item] = h;
    }
    // The collection book.
    QueryResult th = db.Query("SELECT id, codename128, completenum FROM _refcollectionbook_theme WHERE service = 1 ORDER BY id");
    if (!th.Ok()) return fail(th);
    for (int i = 0; i < th.Rows(); ++i) out.themes.push_back({std::uint32_t(th.Int(i, 0)), Trim(th.Text(i, 1)), std::uint8_t(th.Int(i, 2))});
    QueryResult ti = db.Query("SELECT codename128, themecodename128, slotindex FROM _refcollectionbook_item WHERE service = 1");
    if (!ti.Ok()) return fail(ti);
    for (int i = 0; i < ti.Rows(); ++i) {
        const assets::ItemRow* item = items.Find(Trim(ti.Text(i, 0)));
        const CollectionTheme* theme = out.Theme(Trim(ti.Text(i, 1)));
        if (item == nullptr || theme == nullptr) continue;
        out.talismans[item->refObjId] = {theme->id, std::uint8_t(ti.Int(i, 2))};
    }
    // The fortresses' Cave of Discipline (INS_SIEGE_DUNGEON, world 61) apart from the Forgotten
    // World's: no hole opens it, the fortress manager does.
    if (const auto it = std::find_if(out.worlds.begin(), out.worlds.end(), [](const GameWorldSpec& w) { return w.codename == "INS_SIEGE_DUNGEON"; });
        it != out.worlds.end()) {
        out.siege = std::move(*it);
        out.worlds.erase(it);
    }
    // The job temples (INS_TEMPLE_*): their schedule opens them (Temples.h).
    if (const auto area = config.find("GROUP_TEMPLE_DEFAULT"); area != config.end()) out.templeArea = area->second;
    for (auto it = out.worlds.begin(); it != out.worlds.end();) {
        if (!it->codename.starts_with("INS_TEMPLE_")) {
            ++it;
            continue;
        }
        out.temples.push_back(std::move(*it));
        it = out.worlds.erase(it);
    }
    // The Pharaoh's tomb (INS_PHARAOH_*): entered through its gates by a party, not by a hole.
    for (auto it = out.worlds.begin(); it != out.worlds.end();) {
        if (!it->codename.starts_with("INS_PHARAOH_")) {
            ++it;
            continue;
        }
        if (it->gatePoint == 0) notes.push_back(it->codename + ": no gate leads in");
        out.tombs.push_back(std::move(*it));
        it = out.worlds.erase(it);
    }
    return true;
}

} // namespace sr::server
