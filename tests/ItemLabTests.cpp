// The item lab (L12): its list of every item in service over the game files (each with a kind and
// the path our server takes for it), and the developer pages' API for it (the list, an item's page, its notes and RE
// note; set SR_DB_URL, otherwise skipped).

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/game/ItemCatalogue.h"
#include "sr/server/Database.h"
#include "sr/server/ItemLab.h"
#include "sr/server/WebAdmin.h"
#include "sr/server/WebServer.h"

using namespace std::chrono_literals;
using namespace sr;
using namespace sr::test;
using json = nlohmann::json;

namespace {

std::string ItemLabGameDir() {
    const char* dir = std::getenv("SR_GAME_DIR");
    return dir != nullptr ? dir : "";
}

} // namespace

TEST_CASE("Item lab: every item in service listed with a kind and the path our server takes (game files)") {
    const std::string dir = ItemLabGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    server::ItemLabData data;
    REQUIRE_MESSAGE(server::LoadItemLabData(fs, data, error), error);
    const game::ItemRouteData routeData{&data.skills, &data.characters, &data.talismans};
    const std::vector<game::ItemCatalogueEntry> list = game::BuildItemCatalogue(data.items, routeData);
    std::size_t inService = 0;
    for (const auto& [code, row] : data.items.rows()) inService += row.inService ? 1 : 0;
    CHECK(list.size() == inService);
    CHECK(list.size() == 11923); // the 1.188 client's item table
    std::map<std::string, int> kinds;
    std::map<game::ItemRoute, int> routes;
    int unknown = 0;
    for (const game::ItemCatalogueEntry& e : list) {
        ++kinds[std::string(e.kind.group) + "/" + e.kind.kind];
        ++routes[e.routing.route];
        if (std::string_view(e.kind.kind) == "unknown") {
            ++unknown;
            MESSAGE("no kind: " << e.row->codename);
        }
        CHECK(std::string_view(game::ItemRouteName(e.routing.route)) != "?");
        // A refusal says why.
        if (e.routing.route == game::ItemRoute::Unhandled || e.routing.route == game::ItemRoute::Declined ||
            e.routing.route == game::ItemRoute::OriginalRefuses) {
            CHECK(!e.routing.why.empty());
        }
    }
    CHECK(unknown == 0);
    for (const auto& [kind, n] : kinds) MESSAGE(kind << ": " << n);
    for (const auto& [route, n] : routes) MESSAGE(game::ItemRouteName(route) << ": " << n);

    // The paths of the items each handler is for (GameServer::UseItem's order and the other messages).
    const auto route = [&](std::string_view code) {
        const assets::ItemRow* row = data.items.Find(code);
        REQUIRE_MESSAGE(row != nullptr, code);
        return std::string(game::ItemRouteName(game::ItemRouteOf(*row, routeData).route));
    };
    CHECK(route("ITEM_CH_SWORD_01_A") == "equip");
    CHECK(route("ITEM_EU_M_HEAVY_01_BA_A") == "equip");
    CHECK(route("ITEM_CH_RING_01_A") == "equip");
    CHECK(route("ITEM_FORT_FORTRESS_HAMMER_06") == "equip");
    CHECK(route("ITEM_CH_M_TRADE_TRADER_02") == "job-suit");
    CHECK(route("ITEM_CH_M_FRPVP_VOUCHER_A") == "equip");    // no job of its own: ordinary equipment
    CHECK(route("ITEM_EU_M_TRADE_TRADER_01_01") == "equip"); // the whole-body suits too
    CHECK(route("ITEM_MALL_AVATAR_M_NASRUN") == "avatar");
    CHECK(route("ITEM_ETC_AMMO_ARROW_01") == "ammunition");
    CHECK(route("ITEM_ETC_HP_POTION_01") == "recovery");
    CHECK(route("ITEM_ETC_CURE_ALL_01") == "recovery");
    CHECK(route("ITEM_ETC_COS_HP_POTION_01") == "pet-recovery");
    CHECK(route("ITEM_COS_P_HGP_POTION_01") == "pet-food");
    CHECK(route("ITEM_COS_P_FLUTE") == "pet");
    CHECK(route("ITEM_COS_P_MYOWON_SCROLL") == "pet");
    CHECK(route("ITEM_COS_T_HORSE1") == "cos-scroll");
    CHECK(route("ITEM_COS_C_DONKEY") == "cos-scroll");
    CHECK(route("ITEM_ETC_AUTOMOB") == "not-usable"); // names a monster, CanUse 0
    CHECK(route("ITEM_COS_P_REVIVAL") == "pet-treatment");
    CHECK(route("ITEM_ETC_SCROLL_RETURN_01") == "return-scroll");
    CHECK(route("ITEM_ETC_SCROLL_RETURN_THIEFDEN_01") == "thief-den");
    CHECK(route("ITEM_MALL_REVERSE_RETURN_SCROLL") == "reverse-return");
    CHECK(route("ITEM_MALL_BOOTH_MOB_MANGYANG") == "stall-decoration");
    CHECK(route("ITEM_MALL_GLOBAL_CHATTING") == "global-chat");
    CHECK(route("ITEM_FORT_TABLET_HANGA_GOLD") == "fortress");
    CHECK(route("ITEM_FORT_SHOCK_BOMB") == "fortress");
    CHECK(route("ITEM_FORT_REPAIR_KIT_01") == "fortress");
    CHECK(route("ITEM_ETC_TRANS_MONSTER") == "monster-mask");
    CHECK(route("ITEM_MALL_MAGIC_CUBE_WEAPON") == "magic-cube");
    CHECK(route("ITEM_MALL_DAMAGE_INC_20P_SCROLL") == "buff-scroll");
    CHECK(route("ITEM_MALL_SILVER_TIME_SERVICE_TICKET_1D") == "time-ticket");
    CHECK(route("ITEM_MALL_PREMIUM_GOLDTIME") == "premium");
    CHECK(route("ITEM_MALL_RESURRECTION_60P_SCROLL") == "resurrection");
    CHECK(route("ITEM_MALL_REPAIR_HAMMER") == "repair-hammer");
    CHECK(route("ITEM_MALL_EQUIP_TRANSGENDER_1") == "gender-switch");
    CHECK(route("ITEM_MALL_CHAR_SKIN_CHANGE_SCROLL") == "skin-change");
    CHECK(route("ITEM_MALL_SKILL_RESTORATION_POTION") == "skill-withdraw");
    CHECK(route("ITEM_MALL_INVENTORY_ADDITION") == "expansion");
    CHECK(route("ITEM_MALL_NASRUN_EXTENSION") == "spirit-extension");
    CHECK(route("ITEM_MALL_PET_GROWTH_POTION") == "pet-growth");
    CHECK(route("ITEM_MALL_OPEN_MARKET_TICKET") == "time-ticket"); // the Open Market's
    CHECK(route("ITEM_ETC_TELEPORT_HOLE_TOGUI_35_50_LEVEL_1") == "dimension-hole");
    CHECK(route("ITEM_TALISMAN_TOGUI_RED_TEARS") == "talisman");
    CHECK(route("ITEM_ETC_E070919_TICKET") == "flag-card");
    CHECK(route("ITEM_ETC_GUILD_CREST_USER") == "guild-crest");
    CHECK(route("ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_A") == "alchemy");
    CHECK(route("ITEM_ETC_ARCHEMY_MAGICSTONE_STR_01") == "alchemy");
    CHECK(route("ITEM_ETC_ARCHEMY_MATERIAL_CH_MANGNYANG1") == "alchemy");
    CHECK(route("ITEM_ETC_SOCKET_STONE_SP_E") == "socket");
    CHECK(route("ITEM_ETC_ARCHEMY_RONDO_03") == "socket");
    CHECK(route("ITEM_MALL_GACHA_CARD") == "magic-pop");
    CHECK(route("ITEM_ETC_TRADE_CH_01") == "trade-good");
    CHECK(route("ITEM_QNO_CH_SMITH_1") == "quest-item");
    CHECK(route("ITEM_ETC_GOLD_01") == "gold");
    // The special items.
    CHECK(route("ITEM_MALL_WAREHOUSE_TICKET") == "original-refuses");
    CHECK(route("ITEM_MALL_STATS_POINT_RECALL") == "original-refuses");
    CHECK(route("ITEM_MALL_INDULGENCE") == "original-refuses");
    CHECK(route("ITEM_EVENT_SKILL_RESTORATION_POTION") == "original-refuses");
    CHECK(route("ITEM_EVENT_RENT_SKILLPOINT_SCROLL") == "original-refuses");
    CHECK(route("ITEM_EVENT_OPTLEVEL_UPGRADE_SCROLL_1") == "original-refuses");
    CHECK(route("ITEM_ETC_BATTLE_ARENA_TICKET") == "original-refuses");
    CHECK(route("ITEM_PRE_MALL_PET2_SYS_NAME_REMOVE_A") == "original-refuses");
    CHECK(route("ITEM_MALL_CURE_SUPERSET_1_BAG") == "recovery");
    CHECK(route("ITEM_COS_P_CURE_ALL_01") == "pet-cure");
    CHECK(route("ITEM_ETC_FIREWORK_TADPOLE") == "firework");
    CHECK(route("ITEM_ETC_SECOND_HWAN") == "berserk-potion");
    CHECK(route("ITEM_EVENT_20EXP") == "bonus-scroll");
    CHECK(route("ITEM_ETC_INCREASE_HWAN_TIME_01") == "bonus-scroll");
    CHECK(route("ITEM_ETC_E050729_SUMMON_SCROLL") == "quest-use");
    CHECK(route("ITEM_ETC_E060517_MON_GENERATION_BOX") == "quest-use");
    CHECK(route("ITEM_QNO_WC_FERRY2_1_01") == "reverse-return");
    CHECK(route("ITEM_ETC_GUILD_EU_SOLDIER1_SCROLL") == "original-refuses"); // its requirement type 10: the use's gate refuses it
    CHECK(route("ITEM_ETC_GUILD_RECALL") == "guild-recall");
    CHECK(route("ITEM_MALL_PET_WATCH_AGOL") == "pet-item"); // its jobs on its pet
    CHECK(route("ITEM_MALL_PET_SKILL_LIGHTNING") == "pet-item");
    CHECK(route("ITEM_MALL_CHANGE_NAME") == "not-usable");
    CHECK(routes[game::ItemRoute::Equip] > 7000);

    // How far ours goes with each (the list's status before the exam): a known gap is partly, a gap says what it is.
    const auto state = [&](std::string_view code) {
        const assets::ItemRow* row = data.items.Find(code);
        REQUIRE_MESSAGE(row != nullptr, code);
        return std::string(game::ItemStateName(game::ItemStateOf(game::ItemRouteOf(*row, routeData))));
    };
    std::map<std::string, int> states;
    for (const game::ItemCatalogueEntry& e : list) {
        const game::ItemState s = game::ItemStateOf(e.routing);
        ++states[game::ItemStateName(s)];
        if (s == game::ItemState::Partly) CHECK(!e.routing.why.empty());
    }
    for (const auto& [name, n] : states) MESSAGE(name << ": " << n);
    // Implemented, carried: no item in service is left unhandled (the last, the guild's 16, 2026-11-16) nor partly (the last,
    // the hwan tests' two behind QNO_*_HWAN_1_4_1's trap, 2026-11-17) nor declined (the last, the Open Market ticket, 2026-11-18).
    CHECK(states.size() == 2);
    CHECK_FALSE(states.contains("unhandled"));
    CHECK_FALSE(states.contains("partly"));
    CHECK(state("ITEM_ETC_HP_POTION_01") == "implemented");
    CHECK(state("ITEM_CH_SWORD_01_A") == "implemented");
    CHECK(state("ITEM_FORT_FORTRESS_HAMMER_06") == "implemented"); // its `why` a rule, not a gap
    CHECK(state("ITEM_MALL_PET_GROWTH_POTION") == "implemented");
    CHECK(state("ITEM_MALL_PET_WATCH_AGOL") == "implemented"); // the grab pet's bag and skill
    CHECK(state("ITEM_MALL_PET_SKILL_COLD") == "implemented");  // the attack pet's imbue
    CHECK(state("ITEM_ETC_E060209_CHOCO_CAKE") == "implemented"); // an event's food: a trade good to the original too
    CHECK(state("ITEM_MALL_WAREHOUSE_TICKET") == "implemented"); // refused as the original refuses it
    CHECK(state("ITEM_ETC_INCREASE_HWAN_TIME_01") == "implemented"); // its key the keeper's 0x3C, the berserk time
    CHECK(state("ITEM_ETC_E050729_SUMMON_SCROLL") == "implemented"); // no handler in 1.188: refused 2 as the original
    CHECK(state("ITEM_QNO_CA_WAREHOUSE_1_02") == "implemented");     // its handler, its coded quest and its chain built
    CHECK(state("ITEM_QNO_CA_GORIA_7_03") == "implemented");         // its party instance and GORIA_6's trap before it built
    CHECK(state("ITEM_QNO_CH_WHAN_2_3_01") == "implemented");        // the hwan line whole since 1_4_1's trap
    CHECK(state("ITEM_QNO_TQ_ORNAMENT_4_04") == "implemented");      // its world's four generals in turn built
    CHECK(state("ITEM_ETC_GUILD_RECALL") == "implemented");
    CHECK(state("ITEM_ETC_GUILD_CH_SOLDIER1_SCROLL") == "implemented"); // refused as the original refuses it
    CHECK(state("ITEM_MALL_OPEN_MARKET_TICKET") == "implemented");
    CHECK(state("ITEM_MALL_CHANGE_NAME") == "carried");

    // The rows the site reads: one an item, with its data.
    const std::vector<server::LabItemRow> rows = server::MakeLabItemRows(data);
    REQUIRE(rows.size() == list.size());
    const auto find = [&](std::string_view code) {
        const auto it = std::find_if(rows.begin(), rows.end(), [&](const server::LabItemRow& r) { return r.code == code; });
        REQUIRE(it != rows.end());
        return json::parse(it->data);
    };
    const json sword = find("ITEM_CH_SWORD_01_A");
    CHECK(sword["name"] == "Copper Sword");
    CHECK(sword["kind"] == "sword");
    CHECK(sword["race"] == "China");
    CHECK(sword["requirements"][0]["name"] == "level");
    CHECK(sword["stats"]["worst"]["phyAttack"].size() == 2);
    CHECK(sword["stats"]["best"]["phyAttack"][1].get<double>() >= sword["stats"]["worst"]["phyAttack"][1].get<double>());
    CHECK(sword["use"]["place"] == "weapon");
    const json potion = find("ITEM_ETC_HP_POTION_01");
    CHECK(potion["route"]["state"] == "implemented");
    CHECK(find("ITEM_MALL_PET_WATCH_AGOL")["route"]["state"] == "implemented");
    CHECK(find("ITEM_MALL_PET_WATCH_AGOL_4W")["use"]["bagPlaces"] == 28);
    CHECK(potion["use"]["hp"] == 120);
    CHECK(potion["use"]["reuse"]["china"]["ms"] == 1100);
    const json scroll = find("ITEM_ETC_SCROLL_RETURN_01");
    CHECK(scroll["use"]["castMs"] == 30000);
    CHECK(scroll["note"] == "reverse-return");
    const json horse = find("ITEM_COS_C_PEGASUS_SCROLL");
    REQUIRE(!horse["use"]["byLevel"].empty());
    CHECK(horse["use"]["byLevel"][0]["kind"] != "not found");
    const json buff = find("ITEM_MALL_DAMAGE_INC_20P_SCROLL");
    CHECK(buff["use"]["skill"]["found"] == true);
    CHECK(!buff["use"]["skill"]["effects"].empty());
    const json talisman = find("ITEM_TALISMAN_TOGUI_RED_TEARS");
    CHECK(!talisman["use"]["theme"]["code"].get<std::string>().empty());
    const json stone = find("ITEM_ETC_SOCKET_STONE_SP_E");
    CHECK(!stone["use"]["skills"].empty());
}

TEST_CASE("Item lab: the developer pages' API: the list, an item's page, its notes and RE note for a key (L12)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    server::Database& db = t.Db();
    REQUIRE_MESSAGE(server::MigrateSchema(db, error), error);
    const auto row = [](const std::string& code, const std::string& kind, const std::string& route) {
        server::LabItemRow r;
        r.code = code;
        r.id = 1;
        r.name = code;
        r.typeIds = "3/3/1/1";
        r.group = "potion";
        r.kind = kind;
        r.route = route;
        r.source = "game";
        r.state = route == "unhandled" ? "unhandled" : "implemented";
        r.data = json{{"code", code}, {"route", {{"name", route}, {"why", route == "unhandled" ? "no branch" : ""}, {"state", r.state}}}}.dump();
        return r;
    };
    // The list, then again (the item no longer in it goes).
    REQUIRE_MESSAGE(server::SaveLabItems(db, {row("ITEM_A", "hp", "recovery"), row("ITEM_B", "mp", "unhandled")}, "test1", error), error);
    CHECK(db.Query("SELECT count(*) FROM sro_vt_account.sr_lab_items").Int(0, 0) == 2);
    REQUIRE_MESSAGE(server::SaveLabItems(db, {row("ITEM_A", "hp", "recovery"), row("ITEM_C", "berserk", "unhandled")}, "test2", error), error);
    CHECK(db.Query("SELECT string_agg(code, ',' ORDER BY code) FROM sro_vt_account.sr_lab_items").Text(0, 0) == "ITEM_A,ITEM_C");

    std::string key;
    REQUIRE_MESSAGE(server::MakeDevKey(db, "agent", key, error), error);
    server::WebConfig c;
    c.databaseUrl = t.Url();
    c.port = 0;
    c.secureCookie = false;
    server::WebServer web;
    REQUIRE_MESSAGE(web.Start(c, error), error);
    httplib::Client client("127.0.0.1", web.Port());
    client.set_read_timeout(30s);
    const httplib::Headers bearer{{"Authorization", "Bearer " + key}};

    CHECK(client.Get("/api/admin/dev/items")->status == 401);
    const auto list = client.Get("/api/admin/dev/items", bearer);
    REQUIRE(list);
    REQUIRE(list->status == 200);
    const json items = json::parse(list->body)["items"];
    REQUIRE(items.size() == 2);
    CHECK(items[0]["code"] == "ITEM_C"); // by group and kind: berserk before hp
    CHECK(items[0]["why"] == "no branch");
    CHECK(items[1]["route"] == "recovery");
    CHECK(items[1]["build"] == "test2");
    CHECK(client.Get("/api/admin/dev/items/ITEM_X", bearer)->status == 404);
    const auto page = client.Get("/api/admin/dev/items/ITEM_A", bearer);
    REQUIRE(page);
    REQUIRE(page->status == 200);
    CHECK(json::parse(page->body)["item"]["code"] == "ITEM_A");
    // A verdict and an RE note on an item: in the skill lab's tables, shown with the item.
    CHECK(client.Post("/api/admin/dev/items/ITEM_X/notes", bearer, json{{"kind", "note"}, {"body", "x"}}.dump(), "application/json")->status == 404);
    CHECK(client.Post("/api/admin/dev/items/ITEM_A/notes", bearer, json{{"kind", "verdict"}, {"verdict", "works"}, {"body", "heals 120"}}.dump(),
                      "application/json")
              ->status == 200);
    CHECK(client.Put("/api/admin/dev/items/ITEM_A/re", bearer, json{{"status", "same"}, {"original", "Recovery: the potion's amount at once"}}.dump(), "application/json")
              ->status == 200);
    const json detail = json::parse(client.Get("/api/admin/dev/items/ITEM_A", bearer)->body);
    REQUIRE(detail["notes"].size() == 1);
    CHECK(detail["notes"][0]["verdict"] == "works");
    CHECK(detail["notes"][0]["author"] == "agent");
    CHECK(detail["re"]["status"] == "same");
    const json again = json::parse(client.Get("/api/admin/dev/items", bearer)->body)["items"];
    CHECK(again[1]["verdict"] == "works");
    CHECK(again[1]["notes"] == 1);
    CHECK(again[1]["re"] == "same");
    // The status's pieces: the state its path gives it, the item exam's latest results (none yet, then a run's).
    CHECK(again[0]["state"] == "unhandled");
    CHECK(again[1]["state"] == "implemented");
    CHECK(again[1]["results"].empty());
    REQUIRE(db.Query("INSERT INTO sro_vt_account.sr_lab_results (run_id, code, scenario, variant, status, summary) VALUES "
                     "(7, 'ITEM_A', 1, 'use', 'fail', 'old'), (8, 'ITEM_A', 1, 'use', 'pass', 'healed 120')")
                .Ok());
    CHECK(json::parse(client.Get("/api/admin/dev/items", bearer)->body)["items"][1]["results"]["1:use"] == "pass");
    const json examined = json::parse(client.Get("/api/admin/dev/items/ITEM_A", bearer)->body);
    REQUIRE(examined["results"].size() == 1);
    CHECK(examined["results"][0]["summary"] == "healed 120");
    // A path's RE note: for every item of the path, in the list's routes and on each of its items' pages.
    CHECK(examined["routeRe"].is_null());
    CHECK(examined["routeItems"] == 1);
    CHECK(client.Put("/api/admin/dev/item-routes/declined/re", bearer, json{{"status", "same"}}.dump(), "application/json")->status == 404);
    CHECK(client.Put("/api/admin/dev/item-routes/recovery/re", bearer, json{{"status", "partly"}, {"differs", "the pills"}}.dump(), "application/json")
              ->status == 200);
    const json withRoute = json::parse(client.Get("/api/admin/dev/items", bearer)->body);
    CHECK(withRoute["routes"]["recovery"] == "partly");
    CHECK(withRoute["items"][1]["re"] == "same"); // the item's own note stays its own
    const json routed = json::parse(client.Get("/api/admin/dev/items/ITEM_A", bearer)->body);
    CHECK(routed["routeRe"]["status"] == "partly");
    CHECK(routed["routeRe"]["differs"] == "the pills");
    CHECK(routed["re"]["status"] == "same");
    // The skills' list is not touched by an item's notes.
    CHECK(json::parse(client.Get("/api/admin/dev/skills", bearer)->body)["skills"].empty());
    web.Stop();
}

TEST_CASE("Developer pages without a login on this machine (web-dev-local): /dev and /me answer the developer 'local'; changes keep their token; /admin and a forwarded request still refused") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    server::Database& db = t.Db();
    REQUIRE_MESSAGE(server::MigrateSchema(db, error), error);
    server::LabItemRow r;
    r.code = "ITEM_A";
    r.id = 1;
    r.name = "ITEM_A";
    r.typeIds = "3/3/1/1";
    r.group = "potion";
    r.kind = "hp";
    r.route = "recovery";
    r.source = "game";
    r.state = "implemented";
    r.data = json{{"code", "ITEM_A"}, {"route", {{"name", "recovery"}, {"why", ""}, {"state", "implemented"}}}}.dump();
    REQUIRE_MESSAGE(server::SaveLabItems(db, {r}, "test", error), error);

    const auto start = [&](bool devLocal, server::WebServer& web) {
        server::WebConfig c;
        c.databaseUrl = t.Url();
        c.port = 0;
        c.secureCookie = false;
        c.devLocal = devLocal;
        REQUIRE_MESSAGE(web.Start(c, error), error);
    };
    {
        // Off (every shard's default): a login asked for.
        server::WebServer web;
        start(false, web);
        httplib::Client client("127.0.0.1", web.Port());
        CHECK(client.Get("/api/admin/dev/items")->status == 401);
        CHECK(client.Get("/api/admin/me")->status == 401);
    }
    server::WebServer web;
    start(true, web);
    httplib::Client client("127.0.0.1", web.Port());
    client.set_read_timeout(30s);
    // The list and who: the developer "local", the tester role.
    CHECK(client.Get("/api/admin/dev/items")->status == 200);
    const auto me = client.Get("/api/admin/me");
    REQUIRE(me);
    REQUIRE(me->status == 200);
    const json who = json::parse(me->body);
    CHECK(who["name"] == "local");
    CHECK(who["role"] == "tester");
    const std::string csrf = who["csrf"];
    CHECK(csrf.size() == 64);
    // A note: refused without the page's token, taken with it.
    const std::string note = json{{"kind", "note"}, {"body", "seen locally"}}.dump();
    CHECK(client.Post("/api/admin/dev/items/ITEM_A/notes", note, "application/json")->status == 403);
    const httplib::Headers token{{"X-CSRF-Token", csrf}};
    const auto added = client.Post("/api/admin/dev/items/ITEM_A/notes", token, note, "application/json");
    REQUIRE(added);
    CHECK(added->status == 200);
    CHECK(db.Query("SELECT author FROM sro_vt_account.sr_lab_notes WHERE code = 'ITEM_A'").Text(0, 0) == "local");
    // Another site's page in the browser: its origin refused.
    const httplib::Headers foreign{{"X-CSRF-Token", csrf}, {"Origin", "http://evil.example"}};
    CHECK(client.Post("/api/admin/dev/items/ITEM_A/notes", foreign, note, "application/json")->status == 403);
    // The administration still asks for a login (the tester role reaches /dev alone).
    CHECK(client.Get("/api/admin/site")->status == 401);
    // Through a proxy (a forwarding header): not this machine's developer.
    CHECK(client.Get("/api/admin/dev/items", httplib::Headers{{"X-Forwarded-For", "203.0.113.9"}})->status == 401);
}
