// The skill lab: its list of every board skill group over the game files (each level's row
// the head of its chain, the path our server takes), and the developer pages' API (a tester's session, a key's
// script, the notes, the RE notes; L2 the setup's developer commands through the game server; set SR_DB_URL,
// otherwise skipped).

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/NavLoader.h"
#include "sr/game/NavMesh.h"
#include "sr/game/SkillCatalogue.h"
#include "sr/game/States.h"
#include "sr/server/Crypto.h"
#include "sr/server/Database.h"
#include "sr/server/GameServer.h"
#include "sr/server/Store.h"
#include "sr/server/SkillLab.h"
#include "sr/server/WebAdmin.h"
#include "sr/server/WebServer.h"

using namespace std::chrono_literals;
using namespace sr;
using namespace sr::test;
using json = nlohmann::json;

namespace {

std::string LabGameDir() {
    const char* dir = std::getenv("SR_GAME_DIR");
    return dir != nullptr ? dir : "";
}

} // namespace

TEST_CASE("Skill lab: every board group listed, each level's row the head of its chain, the path our server takes (game files)") {
    const std::string dir = LabGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    server::LabData data;
    REQUIRE_MESSAGE(server::LoadLabData(fs, data, error), error);
    const std::vector<game::SkillCatalogueEntry> list = game::BuildSkillCatalogue(data.skills);
    CHECK(list.size() == 468); // the 13 masteries' boards of 1.188
    std::set<std::uint32_t> chained;
    for (const auto& [id, row] : data.skills.rows()) {
        if (row.chainNext != 0) chained.insert(row.chainNext);
    }
    std::map<game::SkillRoute, int> routes;
    int levels = 0, subRows = 0, free = 0;
    for (const game::SkillCatalogueEntry& e : list) {
        ++routes[e.route];
        for (const assets::SkillRow* r : e.ranks) {
            ++levels;
            // The row learned and cast at each level: never a chain's sub-row (they share the head's group and level;
            // the last read had won: a chain learned for nothing and cast as its last hit, 2026-11-15).
            if (chained.contains(r->id)) ++subRows;
            if (r->activity != 0 && r->spCost <= 0) ++free;
            CHECK(&data.skills.Head(*r) == r);
        }
    }
    CHECK(levels > 3000);
    CHECK(subRows == 0);
    CHECK(free == 0);
    // A sub-row kept from before (a quick slot) casts and learns as its head.
    const assets::SkillRow* tail = data.skills.Find(std::string_view("SKILL_CH_SPEAR_CHAIN_A_3S_09"));
    REQUIRE(tail != nullptr);
    CHECK(data.skills.Head(*tail).codename == "SKILL_CH_SPEAR_CHAIN_A_1S_09");
    // Chain Spear - Tiger: three rows, three hits, held through them all.
    const assets::SkillRow* head = data.skills.GroupLevel(tail->group, 9);
    REQUIRE(head != nullptr);
    CHECK(head->codename == "SKILL_CH_SPEAR_CHAIN_A_1S_09");
    CHECK(game::HitsOf(data.skills, *head) == 3);
    CHECK(game::TimesOf(data.skills, *head).rowMs.size() == 3);
    CHECK(game::TimesOf(data.skills, *head).HoldMs() == 891 + 771 + 1071);
    CHECK(game::SkillMotionOf(*head) == game::SkillMotion::Stands);
    // The paths: none but the five taunts and aggro cuts our server does not cast yet, and the Rogue's two Scorns.
    for (const auto& [route, n] : routes) MESSAGE(std::string(game::SkillRouteName(route)) << ": " << n);
    CHECK(routes[game::SkillRoute::Unhandled] <= 5);
    CHECK(routes[game::SkillRoute::Attack] >= 200);
    CHECK(routes[game::SkillRoute::Passive] == 50);
    // The rows the site reads: one a group, with its data.
    const std::vector<server::LabSkillRow> rows = server::MakeLabSkillRows(data);
    REQUIRE(rows.size() == list.size());
    const auto spear = std::find_if(rows.begin(), rows.end(), [](const server::LabSkillRow& r) { return r.code == "SKILL_CH_SPEAR_CHAIN_A"; });
    REQUIRE(spear != rows.end());
    CHECK(spear->name == "Chain Spear - Tiger");
    CHECK(spear->route == "attack");
    CHECK(spear->motion == "stands");
    const json j = json::parse(spear->data);
    CHECK(j["ranks"].size() == 9);
    CHECK(j["ranks"][8]["hits"] == 3);
    CHECK(j["ranks"][8]["chain"].size() == 2);
    CHECK(j["presentation"]["found"] == true);
    CHECK(j["presentation"]["set"] == "spear");
    CHECK(!j["ranks"][8]["tooltip"]["effects"].empty());
    // The presentation's files: its clips on both Chinese bodies, its effects and sounds, all found.
    CHECK(spear->files == "ok");
    CHECK(j["presentation"]["checks"]["clips"]["checked"].get<int>() >= 2);
    CHECK(j["presentation"]["checks"]["clips"]["missing"].empty());
    int missing = 0;
    for (const server::LabSkillRow& r : rows) missing += r.files == "missing" ? 1 : 0;
    MESSAGE(missing << " groups with a clip, an effect or a sound not found");
}

namespace {

struct Account {
    std::string name, password;
    std::vector<std::uint8_t> secret;
};

// A logged-in browser: its cookie and second token.
struct Session {
    httplib::Client* client = nullptr;
    std::string cookie, csrf;
    httplib::Result Send(const char* method, const std::string& path, const json& body = json::object(), bool token = true) const {
        httplib::Headers h{{"Cookie", cookie}};
        if (token) h.emplace("X-CSRF-Token", csrf);
        if (std::string_view(method) == "GET") return client->Get(path, h);
        if (std::string_view(method) == "PUT") return client->Put(path, h, body.dump(), "application/json");
        if (std::string_view(method) == "DELETE") return client->Delete(path, h, body.dump(), "application/json");
        return client->Post(path, h, body.dump(), "application/json");
    }
};

Session LogIn(httplib::Client& client, const Account& a) {
    const auto r = client.Post("/api/admin/login",
                               json{{"name", a.name}, {"password", a.password}, {"code", server::TotpCode(a.secret, std::int64_t(std::time(nullptr)))}}.dump(),
                               "application/json");
    REQUIRE(r);
    REQUIRE(r->status == 200);
    const std::string set = r->get_header_value("Set-Cookie");
    return Session{&client, set.substr(0, set.find(';')), json::parse(r->body)["csrf"]};
}

server::LabSkillRow Row(const std::string& code, std::uint32_t mastery, const std::string& route) {
    server::LabSkillRow r;
    r.code = code;
    r.group = 1;
    r.mastery = mastery;
    r.masteryName = "Spear";
    r.name = code;
    r.kinds = "attack";
    r.route = route;
    r.motion = "stands";
    r.levels = 1;
    r.data = json{{"code", code}, {"ranks", json::array({{{"tags", json::array({{{"tag", "att"}, {"args", {5, 100}}}})}}})}}.dump();
    return r;
}

} // namespace

TEST_CASE("Skill lab: the developer pages' API: a tester's session, a key's script, verdicts, notes and the RE notes") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    server::Database& db = t.Db();
    REQUIRE_MESSAGE(server::MigrateSchema(db, error), error);
    // The list: two groups, then one again (the other goes).
    REQUIRE_MESSAGE(server::SaveLabSkills(db, {Row("SKILL_A", 1, "attack"), Row("SKILL_B", 1, "unhandled")}, "test1", error), error);
    CHECK(db.Query("SELECT count(*) FROM sro_vt_account.sr_lab_skills").Int(0, 0) == 2);
    REQUIRE_MESSAGE(server::SaveLabSkills(db, {Row("SKILL_A", 1, "attack"), Row("SKILL_C", 2, "self-buff")}, "test2", error), error);
    CHECK(db.Query("SELECT string_agg(code, ',' ORDER BY code) FROM sro_vt_account.sr_lab_skills").Text(0, 0) == "SKILL_A,SKILL_C");
    CHECK(db.Query("SELECT build FROM sro_vt_account.sr_lab_skills WHERE code = 'SKILL_A'").Text(0, 0) == "test2");

    const auto make = [&](const char* name, const char* role) {
        Account a{name, std::string(name) + "-long-password", {}};
        std::string secret;
        REQUIRE_MESSAGE(server::MakeAdmin(db, a.name, role, a.password, secret, error), error);
        a.secret = server::FromBase32(secret);
        return a;
    };
    const Account tester = make("tester1", "tester"), boss = make("boss1", "admin");
    std::string key;
    REQUIRE_MESSAGE(server::MakeDevKey(db, "agent", key, error), error);
    CHECK(key.starts_with("srdev_"));
    CHECK(key.size() == 6 + 64);

    server::WebConfig c;
    c.databaseUrl = t.Url();
    c.port = 0;
    c.secureCookie = false;
    c.adminLoginAttempts = 100;
    server::WebServer web;
    REQUIRE_MESSAGE(web.Start(c, error), error);
    httplib::Client client("127.0.0.1", web.Port());
    client.set_read_timeout(30s);

    // Nobody: refused.
    CHECK(client.Get("/api/admin/dev/skills")->status == 401);
    // A tester: its account and /dev, not the administration's pages.
    const Session s = LogIn(client, tester);
    CHECK(json::parse(s.Send("GET", "/api/admin/me")->body)["role"] == "tester");
    CHECK(s.Send("GET", "/api/admin/audit")->status == 403);
    CHECK(s.Send("GET", "/api/admin/site")->status == 403);
    const auto list = s.Send("GET", "/api/admin/dev/skills");
    REQUIRE(list);
    REQUIRE(list->status == 200);
    const json skills = json::parse(list->body)["skills"];
    REQUIRE(skills.size() == 2);
    CHECK(skills[0]["code"] == "SKILL_A");
    CHECK(skills[0]["verdict"].is_null());
    // A verdict needs the page's token; then it shows in the list and on the skill's page.
    CHECK(s.Send("POST", "/api/admin/dev/skills/SKILL_A/notes", {{"kind", "verdict"}, {"verdict", "broken"}, {"body", "no hits"}}, false)->status == 403);
    CHECK(s.Send("POST", "/api/admin/dev/skills/SKILL_A/notes", {{"kind", "verdict"}, {"verdict", "great"}})->status == 400);
    CHECK(s.Send("POST", "/api/admin/dev/skills/SKILL_X/notes", {{"kind", "note"}, {"body", "x"}})->status == 404);
    const auto added = s.Send("POST", "/api/admin/dev/skills/SKILL_A/notes", {{"kind", "verdict"}, {"verdict", "broken"}, {"scenario", 2}, {"body", "walks between shots"}});
    REQUIRE(added);
    REQUIRE(added->status == 200);
    const std::int64_t noteId = json::parse(added->body)["id"];
    CHECK(json::parse(s.Send("GET", "/api/admin/dev/skills")->body)["skills"][0]["verdict"] == "broken");
    // A key's script (an agent): no session, no token; its RE note and a tag's.
    const httplib::Headers bearer{{"Authorization", "Bearer " + key}};
    CHECK(client.Get("/api/admin/dev/skills/SKILL_A", httplib::Headers{{"Authorization", "Bearer srdev_" + std::string(64, '0')}})->status == 401);
    const auto page = client.Get("/api/admin/dev/skills/SKILL_A", bearer);
    REQUIRE(page);
    REQUIRE(page->status == 200);
    const json detail = json::parse(page->body);
    CHECK(detail["skill"]["code"] == "SKILL_A");
    REQUIRE(detail["notes"].size() == 1);
    CHECK(detail["notes"][0]["author"] == "tester1");
    CHECK(detail["notes"][0]["scenario"] == 2);
    CHECK(detail["re"].is_null());
    CHECK(client.Put("/api/admin/dev/skills/SKILL_A/re", bearer, json{{"status", "maybe"}}.dump(), "application/json")->status == 400);
    CHECK(client.Put("/api/admin/dev/skills/SKILL_A/re", bearer,
                     json{{"status", "differs"}, {"original", "Held through every row."}, {"differs", "Ours walks."}, {"sources", "server"}}.dump(),
                     "application/json")
              ->status == 200);
    CHECK(client.Put("/api/admin/dev/tags/att", bearer, json{{"meaning", "An attack: kind, power %, min, max."}, {"args", "kind, power, min, max, ?"}}.dump(),
                     "application/json")
              ->status == 200);
    // The client's side apart (its animations, effects and sounds).
    CHECK(client.Put("/api/admin/dev/skills/SKILL_A/re", bearer, json{{"side", "client"}, {"status", "partly"}, {"original", "Holds its last frame."}}.dump(),
                     "application/json")
              ->status == 200);
    CHECK(client.Put("/api/admin/dev/skills/SKILL_A/re", bearer, json{{"side", "both"}, {"status", "same"}}.dump(), "application/json")->status == 400);
    const json again = json::parse(client.Get("/api/admin/dev/skills/SKILL_A", bearer)->body);
    CHECK(again["re"]["status"] == "differs");
    CHECK(again["re"]["author"] == "agent");
    CHECK(again["reClient"]["status"] == "partly");
    CHECK(json::parse(client.Get("/api/admin/dev/skills", bearer)->body)["skills"][0]["reClient"] == "partly");
    const json tags = json::parse(client.Get("/api/admin/dev/tags", bearer)->body)["tags"];
    REQUIRE(!tags.empty());
    CHECK(tags[0]["tag"] == "att");
    CHECK(tags[0]["groups"] == 2);
    CHECK(tags[0]["note"]["meaning"] == "An attack: kind, power %, min, max.");
    CHECK(db.Query("SELECT last_used_at IS NOT NULL FROM sro_vt_account.sr_web_dev_keys WHERE name = 'agent'").Bool(0, 0));
    // A disabled key: refused.
    REQUIRE_MESSAGE(server::SetDevKeyDisabled(db, "agent", true, error), error);
    CHECK(client.Get("/api/admin/dev/skills", bearer)->status == 401);
    // The summary by mastery; a note removed by its author (another's only by an admin).
    const json summary = json::parse(s.Send("GET", "/api/admin/dev/summary")->body)["masteries"];
    REQUIRE(summary.size() == 2);
    CHECK(summary[0]["groups"] == 1);
    CHECK(summary[0]["verdicts"]["broken"] == 1);
    const Session admin = LogIn(client, boss);
    CHECK(admin.Send("GET", "/api/admin/audit")->status == 200);
    CHECK(s.Send("DELETE", "/api/admin/dev/notes/" + std::to_string(noteId))->status == 200);
    CHECK(json::parse(s.Send("GET", "/api/admin/dev/skills/SKILL_A")->body)["notes"].empty());
    // A note about the animation (its aspect); an unknown aspect refused.
    CHECK(s.Send("POST", "/api/admin/dev/skills/SKILL_A/notes", {{"kind", "note"}, {"aspect", "smell"}, {"body", "x"}})->status == 400);
    CHECK(s.Send("POST", "/api/admin/dev/skills/SKILL_A/notes", {{"kind", "note"}, {"aspect", "animation"}, {"body", "the shot skips"}})->status == 200);
    CHECK(json::parse(s.Send("GET", "/api/admin/dev/skills/SKILL_A")->body)["notes"][0]["aspect"] == "animation");
    // Every change is in the audit log, with who made it.
    CHECK(db.Query("SELECT count(*) FROM sro_vt_account.sr_web_audit WHERE action LIKE 'lab-%'").Int(0, 0) == 6);
    CHECK(db.Query("SELECT count(*) FROM sro_vt_account.sr_web_audit WHERE action = 'lab-re' AND name = 'key:agent'").Int(0, 0) == 2); // the server's and the client's
    web.Stop();
}

// --- L2: the setup's developer commands through the game server ----------------------------------------------------

namespace {

// The next message of `T` that `pred` takes (others of `T` skipped), within `wait`.
template <typename T, typename P>
bool Await(Client& c, T& out, P pred, std::chrono::milliseconds wait = 10s) {
    const auto end = std::chrono::steady_clock::now() + wait;
    while (std::chrono::steady_clock::now() < end) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
        if (!c.Expect(out, std::max(left, std::chrono::milliseconds(1)))) return false;
        if (pred(out)) return true;
    }
    return false;
}

float FlatDistance(const game::proto::WorldPosition& a, const game::proto::WorldPosition& b) {
    const game::proto::FlatPosition p = game::proto::ToFlat(a), q = game::proto::ToFlat(b);
    return float(std::hypot(p.x - q.x, p.z - q.z));
}

} // namespace

TEST_CASE("Skill lab: the setup's developer commands through the game server: a mastery learned, a weapon worn, still and fleeing monsters, "
          "states and knocks, the reset, the removal (game files)") {
    namespace proto = game::proto;
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const std::string dir = LabGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    auto items = std::make_shared<assets::ItemTable>();
    REQUIRE_MESSAGE(items->Load(fs, error), error);
    const assets::ItemRow* spear = items->Find(std::string_view("ITEM_CH_SPEAR_01_A"));
    const assets::SkillRow* chain = skills->Find(std::string_view("SKILL_CH_SPEAR_CHAIN_A_1S_01"));
    REQUIRE(spear != nullptr);
    REQUIRE(chain != nullptr);
    const std::uint32_t spearMastery = chain->masteries[0];

    core::JobSystem jobs{2};
    net::NetService net{1};
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    auto rules = std::make_shared<game::CharacterRules>();
    for (auto [first, last] : {std::pair<char32_t, char32_t>{U'0', U'9'}, {U'A', U'Z'}, {U'a', U'z'}}) rules->names.Allow(first, last);
    rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
    rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
    rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
    rules->items = items;
    config.rules = rules;
    config.files = &fs;
    config.characters = characters;
    config.skills = skills;
    config.levels.assign(150, 1000);
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.databaseThreads = 2;
    config.devCommands = true;
    REQUIRE_MESSAGE(game.Start(config, error), error);

    // A level-1 character in the field west of Constantinople (the town itself is safe ground: no monster comes in).
    REQUIRE(server::CreateAccount(t.Db(), "labacct", "secret1", error) == server::CreateAccountResult::Created);
    bool ok = false;
    const std::int64_t account = server::FindAccount(t.Db(), "labacct", ok)->id;
    Client c(net, game.Port(), [&] { game.Poll(2ms); });
    REQUIRE(c.Greet());
    proto::LobbyEnter e;
    e.ticket = server::IssueTicket(t.Db(), account, config.id, 30, error);
    c.Send(e);
    proto::LobbyEntered lobby;
    REQUIRE(c.Expect(lobby));
    proto::CharacterCreate m;
    m.name = "Labber";
    m.model = 100;
    m.chest = 1;
    m.legs = 2;
    m.shoes = 3;
    m.weapon = 10;
    c.Send(m);
    proto::LobbyResult r;
    REQUIRE(c.Expect(r));
    REQUIRE(r.status == proto::LobbyStatus::Ok);
    const std::int64_t character = server::ListCharacters(t.Db(), account, config.id, 0, ok)[0].id;
    server::CharacterRow::Place here;
    here.region = assets::RegionId(76, 105);
    here.x = 600;
    here.y = 140;
    here.z = 900;
    REQUIRE(server::SavePlaces(t.Db(), {{character, here}}));
    // The creation's stand-in weapon (id 10, no item of the game files) out of its place: the spear goes there.
    REQUIRE(t.Db().Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND slot = 6", {character}).Ok());
    c.Send(proto::CharacterSelect{"Labber"});
    REQUIRE(c.Expect(r));
    c.Send(proto::WorldEnter{});
    proto::WorldEntered entered;
    REQUIRE(c.Expect(entered));
    REQUIRE(entered.status == proto::WorldStatus::Ok);
    const std::uint32_t me = entered.entity;

    // The spear mastery at 40: the character raised to 40, every group of its board at the highest level 40 allows.
    c.Send(proto::DevLearn{spearMastery, 40, 0});
    proto::SkillBookList book;
    REQUIRE(Await(c, book, [&](const proto::SkillBookList& b) {
        return std::any_of(b.masteries.begin(), b.masteries.end(), [&](const proto::MasteryRecord& x) { return x.mastery == spearMastery && x.level == 40; });
    }));
    proto::Progress progress;
    REQUIRE(Await(c, progress, [](const proto::Progress& p) { return p.level == 40; }));
    std::uint32_t chainSkill = 0;
    int learned = 0;
    for (const std::uint32_t id : book.skills) {
        const assets::SkillRow* s = skills->Find(id);
        REQUIRE(s != nullptr);
        if (s->masteries[0] != spearMastery) continue;
        ++learned;
        CHECK(s->masteryLevels[0] <= 40);
        CHECK(&skills->Head(*s) == s); // a chain's head
        if (s->group == chain->group) chainSkill = id;
        // The highest level 40 allows: the next one asks for more (or there is none).
        if (const assets::SkillRow* next = skills->GroupLevel(s->group, s->level + 1)) CHECK(next->masteryLevels[0] > 40);
    }
    CHECK(learned >= 10);
    REQUIRE(chainSkill != 0);
    MESSAGE(learned << " spear skills learned; Chain Spear - Tiger at level " << skills->Find(chainSkill)->level);
    const auto masteryLevel = [&] {
        const server::QueryResult q = t.Db().Query("SELECT level FROM _charskillmastery WHERE charid = $1 AND masteryid = $2", {character, std::int64_t(spearMastery)});
        return q.Ok() && q.Rows() == 1 ? q.Int(0, 0) : std::int64_t(-1);
    };
    for (int i = 0; i < 300 && masteryLevel() != 40; ++i) game.Poll(10ms);
    CHECK(masteryLevel() == 40);

    // The spear given and worn in the weapon's place.
    c.Send(proto::DevGive{spear->codename, 1, true});
    proto::ItemMoved moved;
    REQUIRE(Await(c, moved, [](const proto::ItemMoved& x) { return x.to == 6; }));
    CHECK(moved.status == proto::ItemStatus::Ok);

    // A still monster 40 ahead with a million health: it appears and stays put.
    c.Send(proto::DevSpawn{"MOB_CH_MANGNYANG", 40, 0, proto::LabBehaviour::Still, 1'000'000, 7});
    proto::DevSpawned still;
    REQUIRE(c.Expect(still));
    CHECK(still.tag == 7);
    REQUIRE(still.entity != 0);
    proto::EntityAppeared seen;
    REQUIRE(Await(c, seen, [&](const proto::EntityAppeared& x) { return x.entity == still.entity; }));
    CHECK(seen.character.hp == 1'000'000u);
    // Chain Spear on it: the player walks into reach, the chain's three hits land; the monster never moves nor swings.
    c.Send(proto::UseSkill{chainSkill, still.entity});
    int hits = 0, monsterSwings = 0, monsterMoves = 0;
    const auto until = std::chrono::steady_clock::now() + 6s;
    while (std::chrono::steady_clock::now() < until) {
        proto::EntityDamaged hit;
        while (c.Expect(hit, 20ms)) {
            if (hit.attacker == me && hit.target == still.entity) ++hits;
        }
        proto::EntitySwing swing;
        while (c.Expect(swing, 1ms)) {
            if (swing.attacker == still.entity) ++monsterSwings;
        }
        proto::EntityMoved move;
        while (c.Expect(move, 1ms)) {
            if (move.entity == still.entity) ++monsterMoves;
        }
    }
    MESSAGE(hits << " hits on the still monster");
    CHECK(hits >= 3);
    CHECK(monsterSwings == 0);
    CHECK(monsterMoves == 0);

    // A stun on it (told as its states), a knock-back and a stun on the player itself (told to all).
    c.Send(proto::DevAfflict{still.entity, std::uint8_t(game::StateKind::Stun), 1, 4000, 0, 0});
    proto::EntityStates states;
    REQUIRE(Await(c, states, [&](const proto::EntityStates& x) {
        return x.entity == still.entity &&
               std::any_of(x.states.begin(), x.states.end(), [](const proto::StateRecord& s) { return s.kind == std::uint8_t(game::StateKind::Stun); });
    }));
    c.Send(proto::DevAfflict{me, std::uint8_t(game::StateKind::Stun), 1, 4000, 0, 2});
    proto::EntityKnocked knocked;
    REQUIRE(Await(c, knocked, [&](const proto::EntityKnocked& x) { return x.entity == me; }));
    CHECK(knocked.kind == proto::KnockKind::Back);
    REQUIRE(Await(c, states, [&](const proto::EntityStates& x) { return x.entity == me && !x.states.empty(); }));

    // The reset: the stun gone, the vitals full.
    c.Send(proto::DevReset{proto::DevReset::kAll});
    REQUIRE(Await(c, states, [&](const proto::EntityStates& x) { return x.entity == me && x.states.empty(); }));
    proto::Vitals vitals;
    REQUIRE(Await(c, vitals, [](const proto::Vitals& v) { return v.hp == v.maxHp && v.mp == v.maxMp; }));

    // A fleeing monster 30 behind: it walks straight away from the player.
    c.Send(proto::DevSpawn{"MOB_CH_MANGNYANG", -30, 0, proto::LabBehaviour::FleeWalk, 0, 8});
    proto::DevSpawned flee;
    REQUIRE(Await(c, flee, [](const proto::DevSpawned& x) { return x.tag == 8; }));
    REQUIRE(flee.entity != 0);
    proto::EntityMoved away;
    REQUIRE(Await(c, away, [&](const proto::EntityMoved& x) { return x.entity == flee.entity; }, 10s));
    // Where the player stands now (it walked to the still monster and was knocked back).
    proto::EntityMoved mine;
    proto::WorldPosition at{here.region, here.x, here.y, here.z};
    while (c.Expect(mine, 1ms)) {
        if (mine.entity == me) at = mine.to;
    }
    CHECK(FlatDistance(away.to, at) > FlatDistance(away.from, at) + 50.0f);

    // Both taken away.
    c.Send(proto::DevRemove{0});
    std::set<std::uint32_t> gone;
    proto::EntityGone left;
    while (gone.size() < 2 && Await(c, left, [&](const proto::EntityGone& x) { return x.entity == still.entity || x.entity == flee.entity; }, 5s)) {
        gone.insert(left.entity);
    }
    CHECK(gone.size() == 2);
    game.Stop();
}

TEST_CASE("Skill lab: a target behind a wall is out of sight: the cast refused (0x3010), one in the open struck (game files)") {
    namespace proto = game::proto;
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const std::string dir = LabGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    // Constantinople's navigation, to find a wall: from a place in the town, a spot within a bow's reach the walk to
    // which is blocked (behind a wall or a building's side) and one it reaches (in the open).
    core::JobSystem jobs{2};
    game::NavWorld nav;
    {
        game::NavLoader loader(fs, jobs);
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) loader.Request(assets::RegionId(79 + dx, 105 + dz));
        }
        for (int i = 0; i < 1000 && nav.Size() < 9; ++i) {
            loader.Update(nav);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    REQUIRE(nav.Size() == 9);
    const std::uint16_t region = assets::RegionId(79, 105);
    const auto floor = [&](float x, float z) {
        const float above[3] = {x, 5000.0f, z}, down[3] = {0.0f, -1.0f, 0.0f};
        game::NavHit top;
        if (!nav.Ray(region, above, down, 10000.0f, top) || top.region != region) return game::NavPosition{};
        return nav.Drop(region, x, top.y, z);
    };
    game::NavPosition here;
    float behind[2] = {0, 0}, open[2] = {0, 0};
    bool foundBehind = false, foundOpen = false;
    for (float x = 700.0f; x <= 1300.0f && !(foundBehind && foundOpen); x += 50.0f) {
        for (float z = 700.0f; z <= 1300.0f && !(foundBehind && foundOpen); z += 50.0f) {
            const game::NavPosition from = floor(x, z);
            if (!from.Valid()) continue;
            bool b = false, o = false;
            float bx = 0, bz = 0, ox = 0, oz = 0;
            for (int k = 0; k < 32 && !(b && o); ++k) {
                const float a = float(k) * 0.19635f, d = 100.0f;
                const float tx = x + d * std::sin(a), tz = z + d * std::cos(a);
                const game::NavPosition to = floor(tx, tz);
                if (!to.Valid() || std::abs(to.y - from.y) > 30.0f) continue;
                game::NavPosition end;
                const std::uint32_t walked = nav.Walk(from, region, tx, tz, end);
                const bool reached = (walked & (game::nav_walk::kBlocked | game::nav_walk::kFailed)) == 0 && std::hypot(end.x - tx, end.z - tz) < 1.0f;
                if (!reached && !b && std::hypot(end.x - x, end.z - z) < 60.0f) {
                    b = true, bx = tx - x, bz = tz - z;
                } else if (reached && !o) {
                    o = true, ox = tx - x, oz = tz - z;
                }
            }
            if (b && o) {
                here = from;
                behind[0] = bx, behind[1] = bz, open[0] = ox, open[1] = oz;
                foundBehind = foundOpen = true;
            }
        }
    }
    REQUIRE(foundBehind);
    MESSAGE("the archer at " << here.x << ", " << here.z << "; a target behind a wall at +" << behind[0] << ", +" << behind[1] << ", one in the open at +"
                             << open[0] << ", +" << open[1]);

    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    auto items = std::make_shared<assets::ItemTable>();
    REQUIRE_MESSAGE(items->Load(fs, error), error);
    const assets::SkillRow* shot = skills->Find(std::string_view("SKILL_CH_BOW_CRITICAL_A_01"));
    REQUIRE(shot != nullptr);
    net::NetService net{1};
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    auto rules = std::make_shared<game::CharacterRules>();
    for (auto [first, last] : {std::pair<char32_t, char32_t>{U'0', U'9'}, {U'A', U'Z'}, {U'a', U'z'}}) rules->names.Allow(first, last);
    rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
    rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
    rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
    rules->items = items;
    config.rules = rules;
    config.files = &fs;
    config.characters = characters;
    config.skills = skills;
    config.levels.assign(150, 1000);
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.databaseThreads = 2;
    config.devCommands = true;
    REQUIRE_MESSAGE(game.Start(config, error), error);
    REQUIRE(server::CreateAccount(t.Db(), "sightacct", "secret1", error) == server::CreateAccountResult::Created);
    bool ok = false;
    const std::int64_t account = server::FindAccount(t.Db(), "sightacct", ok)->id;
    Client c(net, game.Port(), [&] { game.Poll(2ms); });
    REQUIRE(c.Greet());
    proto::LobbyEnter e;
    e.ticket = server::IssueTicket(t.Db(), account, config.id, 30, error);
    c.Send(e);
    proto::LobbyEntered lobby;
    REQUIRE(c.Expect(lobby));
    proto::CharacterCreate m;
    m.name = "Archer";
    m.model = 100;
    m.chest = 1;
    m.legs = 2;
    m.shoes = 3;
    m.weapon = 10;
    c.Send(m);
    proto::LobbyResult r;
    REQUIRE(c.Expect(r));
    REQUIRE(r.status == proto::LobbyStatus::Ok);
    const std::int64_t character = server::ListCharacters(t.Db(), account, config.id, 0, ok)[0].id;
    server::CharacterRow::Place place;
    place.region = region;
    place.x = here.x;
    place.y = here.y;
    place.z = here.z;
    REQUIRE(server::SavePlaces(t.Db(), {{character, place}}));
    REQUIRE(t.Db().Query("UPDATE _inventory SET itemid = 0 WHERE charid = $1 AND slot < 13", {character}).Ok());
    c.Send(proto::CharacterSelect{"Archer"});
    REQUIRE(c.Expect(r));
    c.Send(proto::WorldEnter{});
    proto::WorldEntered entered;
    REQUIRE(c.Expect(entered));
    REQUIRE(entered.status == proto::WorldStatus::Ok);
    // The bow mastery, a bow and arrows.
    c.Send(proto::DevLearn{shot->masteries[0], 20, 0});
    proto::SkillBookList book;
    REQUIRE(Await(c, book, [&](const proto::SkillBookList& b) { return !b.skills.empty(); }));
    c.Send(proto::DevGive{"ITEM_CH_BOW_01_A", 1, true});
    proto::ItemMoved moved;
    REQUIRE(Await(c, moved, [](const proto::ItemMoved& x) { return x.to == 6; }));
    c.Send(proto::DevGive{"ITEM_ETC_AMMO_ARROW_01", 500, true});
    REQUIRE(Await(c, moved, [](const proto::ItemMoved& x) { return x.to == 7; }));
    const auto castAt = [&](float dx, float dz, std::uint32_t tag) {
        c.Send(proto::DevSpawn{"MOB_CH_MANGNYANG", dx, dz, proto::LabBehaviour::Still, 1'000'000, tag});
        proto::DevSpawned spawned;
        REQUIRE(Await(c, spawned, [&](const proto::DevSpawned& x) { return x.tag == tag; }));
        REQUIRE(spawned.entity != 0);
        c.Send(proto::DevReset{proto::DevReset::kAll});
        c.Send(proto::UseSkill{shot->id, spawned.entity});
        proto::SkillUseAnswer refused;
        const bool obstacle = Await(c, refused, [&](const proto::SkillUseAnswer& a) { return a.status == proto::SkillUseStatus::Obstacle; }, 3s);
        proto::EntityDamaged hit;
        const bool struck = !obstacle && Await(c, hit, [&](const proto::EntityDamaged& h) { return h.target == spawned.entity; }, 5s);
        c.Send(proto::DevRemove{0});
        return std::pair{obstacle, struck};
    };
    const auto [blocked, struckBehind] = castAt(behind[0], behind[1], 1);
    CHECK(blocked);
    CHECK_FALSE(struckBehind);
    const auto [refusedOpen, struckOpen] = castAt(open[0], open[1], 2);
    CHECK_FALSE(refusedOpen);
    CHECK(struckOpen);
    game.Stop();
}
