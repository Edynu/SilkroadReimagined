// The quests' and events' items used from the bag (game/QuestItems.h): the `on use` rule of
// our quest format and the original's ground searches written in it, the quests' handler on a made-up host (the place,
// the states, the cap, the wait and the roll, the words), the two summon events on a made-up host (the guild support's
// party summon scroll, the Pandora box and its daemon), the catalogue's states of the 44 items.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/ItemCatalogue.h"
#include "sr/game/Quest.h"
#include "sr/game/QuestItems.h"
#include "sr/game/SpecialItems.h"
#include "sr/server/GameServer.h"
#include "sr/server/QuestStore.h"
#include "sr/server/Quests.h"
#include "sr/server/Store.h"
#include "sr/server/SummonEvents.h"

using namespace sr;
namespace proto = sr::game::proto;
using namespace std::chrono_literals;

namespace {

game::QuestDef ParseOne(std::string_view text) {
    game::QuestDef q;
    std::string error;
    REQUIRE_MESSAGE(game::ParseQuest(text, q, error), error);
    return q;
}

// One player (entity 1): its place, its bag by codename, whether it has an empty place; what it was told.
struct UseHost : server::QuestHost {
    Player who{1, 77, "Seeker", 40, "europe", 0, true};
    std::map<std::string, std::uint32_t> bag;
    bool freePlace = true;
    std::vector<std::string> said;
    std::map<std::uint32_t, Player> others;      // more players (a party's members)
    std::map<std::uint32_t, std::uint32_t> layers; // the instance layer each is in (none: the shared world)
    std::optional<Player> QuestPlayer(std::uint32_t entity) override {
        std::optional<Player> p;
        if (entity == 1) p = who;
        else if (others.contains(entity)) p = others[entity];
        if (p && layers.contains(entity)) p->position.layer = layers[entity];
        return p;
    }
    void QuestSend(std::uint64_t, proto::MessageId, const std::vector<std::byte>&) override {}
    std::string QuestNpcNear(std::uint32_t, std::uint32_t npc) override { return npc == 9 ? "NPC_T_WITCH" : std::string(); }
    int QuestHour() override { return 12; }
    std::set<std::uint32_t> emptyBags; // players holding nothing (the bag is everyone's else)
    std::uint32_t QuestItemCount(std::uint32_t entity, const std::string& item) override { return !emptyBags.contains(entity) && bag.contains(item) ? bag[item] : 0; }
    bool QuestGive(std::uint32_t, const std::vector<game::QuestGoal::Items>& items) override {
        if (giveFails) return false;
        for (const auto& i : items) bag[i.item] += i.count;
        return true;
    }
    bool QuestTake(std::uint32_t, const std::vector<game::QuestGoal::Items>& items) override {
        for (const auto& i : items)
            if (bag[i.item] < i.count) return false;
        for (const auto& i : items) bag[i.item] -= i.count;
        return true;
    }
    bool giveFails = false;
    bool QuestTakeGold(std::uint32_t, std::uint64_t) override { return false; }
    bool QuestRoom(std::uint32_t, const std::vector<game::QuestGoal::Items>&) override { return true; }
    void QuestReward(std::uint32_t, const Reward&) override {}
    void QuestDrop(std::uint32_t, std::uint32_t, const std::string&) override {}
    void QuestSave(std::int64_t, const server::QuestRecord&) override {}
    bool QuestThemeDone(std::uint32_t, const std::string&) override { return false; }
    bool QuestFreePlace(std::uint32_t) override { return freePlace; }
    std::vector<std::pair<std::uint32_t, std::string>> told; // by player
    void QuestSay(std::uint32_t entity, const std::string& key) override {
        said.push_back(key);
        told.push_back({entity, key});
    }
    bool Told(std::uint32_t entity, const std::string& key) const {
        return std::find(told.begin(), told.end(), std::pair{entity, key}) != told.end();
    }
    std::string LastSaid() const { return said.empty() ? std::string() : said.back(); }
    // The other handlers: the selection, the raised monsters, the caught COS, the monsters' levels.
    std::optional<Selected> selected;
    std::optional<Selected> QuestSelected(std::uint32_t) override { return selected; }
    struct Raised {
        std::string monster;
        proto::WorldPosition at;
        bool gone = false;
        bool standing = true;
    };
    std::vector<Raised> raised;
    std::optional<std::size_t> QuestSpawn(std::uint32_t, const std::string& codename, const proto::WorldPosition& at) override {
        if (codename.starts_with("MOB_T_NONE")) return std::nullopt;
        raised.push_back({codename, at});
        return raised.size() - 1;
    }
    void QuestDespawn(std::size_t nest) override { raised.at(nest).gone = true; }
    int QuestSpawnStanding(std::size_t nest) override { return nest < raised.size() && !raised[nest].gone && raised[nest].standing ? 1 : 0; }
    // The instance: the party here, its master, a transport, the layers.
    std::vector<std::uint32_t> party;
    bool master = false, transport = false;
    std::uint32_t nextLayer = 7;
    std::vector<std::uint32_t> PartyHere() const { return party; }
    std::vector<std::uint32_t> QuestPartyHere(std::uint32_t) override { return party; }
    bool QuestPartyMaster(std::uint32_t entity) override { return entity == 1 && master; }
    bool QuestTransportOut(std::uint32_t entity) override { return entity == 1 && transport; }
    std::uint32_t QuestWorldOpen(std::uint32_t, const std::string&) override { return nextLayer++; }
    bool QuestWorldEnter(std::uint32_t entity, std::uint32_t layer) override {
        layers[entity] = layer;
        return true;
    }
    void QuestWorldLeave(std::uint32_t entity) override { layers[entity] = 0; }
    std::uint32_t caughtMonster = 0;
    std::string caughtAs;
    std::map<std::uint32_t, bool> coses; // standing
    std::uint32_t QuestCatch(std::uint32_t, std::uint32_t monster, const std::string& cos) override {
        caughtMonster = monster;
        caughtAs = cos;
        const std::uint32_t c = 500 + std::uint32_t(coses.size());
        coses[c] = true;
        return c;
    }
    bool QuestCosStanding(std::uint32_t cos) override { return coses.contains(cos) && coses[cos]; }
    void QuestCosRemove(std::uint32_t cos) override { coses.erase(cos); }
    std::map<std::string, int> levels;
    int QuestMonsterLevel(const std::string& codename) override { return levels.contains(codename) ? levels[codename] : 0; }
};

server::QuestRecord Record(std::uint32_t quest, proto::QuestStatus status, std::uint16_t times = 0, std::uint16_t progress = 0) {
    server::QuestRecord r;
    r.quest = quest;
    r.status = status;
    r.times = times;
    r.progress[0] = progress;
    return r;
}

// The summon events' host: one user (entity 1); the nests made, standing or not.
struct SummonsHost : server::SummonHost {
    User user{"Opener", 40, {25000, 100.0f, 0.0f, 100.0f, 0}, false, false, 0};
    bool present = true;
    std::vector<std::string> said;
    struct Nest {
        std::string monster;
        proto::WorldPosition at;
        float spread = 0;
        bool kept = false;
        int standing = 1;
        bool removed = false;
    };
    std::vector<Nest> nests;
    std::vector<std::pair<std::size_t, std::string>> casts;
    void SummonCast(std::size_t nest, std::string_view skill) override { casts.push_back({nest, std::string(skill)}); }
    std::optional<User> SummonUser(std::uint32_t entity) override { return entity == 1 && present ? std::optional(user) : std::nullopt; }
    void SummonSay(std::uint32_t, const std::string& key) override { said.push_back(key); }
    std::optional<std::size_t> SummonSpawn(std::string_view codename, const proto::WorldPosition&, const proto::WorldPosition& at, float spread,
                                           bool kept) override {
        nests.push_back({std::string(codename), at, spread, kept});
        return nests.size() - 1;
    }
    int SummonStanding(std::size_t nest) override { return nests[nest].removed ? 0 : nests[nest].standing; }
    void SummonRemove(std::size_t nest) override { nests[nest].removed = true; }
    int Made(std::string_view monster) const {
        return int(std::count_if(nests.begin(), nests.end(), [&](const Nest& n) { return n.monster == monster; }));
    }
    std::string LastSaid() const { return said.empty() ? std::string() : said.back(); }
};

double Distance(const proto::WorldPosition& a, const proto::WorldPosition& b) {
    const proto::FlatPosition fa = proto::ToFlat(a), fb = proto::ToFlat(b);
    return std::hypot(fa.x - fb.x, fa.z - fb.z);
}

} // namespace

TEST_CASE("Quest item use: the `on use` rule reads and writes back the same; the original's searches are rules") {
    const game::QuestDef q = ParseOne("quest QNO_T_DIG\nid 20100\nstart level 1-\n"
                                      "on use ITEM_T_SPADE status 1 7 8 first-time one-at-a-time @SN_T_BUSY refuse \"Not now.\" cap-first "
                                      "at 25000 100.5 -3 200 50 at -32763 -16604 1150 9113 300 away @SN_T_AWAY cap 5 ITEM_T_ORE @SN_T_CAP "
                                      "full @SN_T_FULL wait 10 chance 68 of 101 give ITEM_T_ORE failed @SN_T_FAIL got @SN_T_GOT\n"
                                      "goal drop 5 ITEM_T_ORE from MOB_T_ROCK 0%\n");
    REQUIRE(q.onUse.size() == 1);
    const game::QuestDef::UseRule& u = q.onUse[0];
    CHECK(u.item == "ITEM_T_SPADE");
    CHECK(u.statuses == std::vector<std::uint8_t>{1, 7, 8});
    CHECK(u.firstTime);
    CHECK(u.oneAtATime);
    CHECK(u.busy.key == "SN_T_BUSY");
    CHECK(u.refuse.literal == "Not now.");
    CHECK(u.capFirst);
    REQUIRE(u.at.size() == 2);
    CHECK(u.at[0].region == 25000);
    CHECK(u.at[0].x == doctest::Approx(100.5));
    CHECK(u.at[0].y == doctest::Approx(-3));
    CHECK(u.at[1].region == 32773); // a dungeon's region, written as a 16-bit signed number
    CHECK(u.cap == 5);
    CHECK(u.capItem == "ITEM_T_ORE");
    CHECK(u.capped.key == "SN_T_CAP");
    CHECK(u.seconds == 10);
    CHECK(u.chance == 68);
    CHECK(u.outOf == 101);
    CHECK(u.give == "ITEM_T_ORE");
    CHECK(u.failed.key == "SN_T_FAIL");
    CHECK(u.got.key == "SN_T_GOT");
    // Written and read back: the same.
    CHECK(ParseOne(game::WriteQuest(q)) == q);
    // Mistakes are told.
    game::QuestDef bad;
    std::string error;
    CHECK_FALSE(game::ParseQuest("quest QNO_T\non use ITEM_T chance 5 of 3\n", bad, error));
    CHECK_FALSE(game::ParseQuest("quest QNO_T\non use ITEM_T status 2\n", bad, error));
    CHECK_FALSE(game::ParseQuest("quest QNO_T\non use ITEM_T sideways\n", bad, error));
    CHECK(error.find("sideways") != std::string::npos);

    // The original's rules: each a rule of its quest, its item's handler marked built.
    std::set<std::string> quests;
    for (const game::OriginalUseRule& r : game::OriginalUseRules()) {
        CAPTURE(r.quest);
        game::QuestDef def;
        def.codename = std::string(r.quest);
        REQUIRE(game::AddOriginalUseRules(def) >= 1);
        game::QuestDef one;
        std::string error;
        REQUIRE_MESSAGE(game::ParseQuest("quest " + std::string(r.quest) + "\n" + std::string(r.line) + "\n", one, error), error);
        REQUIRE(one.onUse.size() == 1);
        const game::QuestItemUse use = game::QuestItemUseOf(one.onUse[0].item);
        if (one.onUse[0].trap) {
            // A trap's rule (GORIA_6, GENERAL_1, IVY_1): its item a skill's (3/3/13/1), set from the bag, not a quest item; the
            // rule is its catch's (QuestCaptureTests).
            CHECK(use.taker == game::QuestItemTaker::None);
        } else {
            CHECK(use.taker == game::QuestItemTaker::Quest);
            CHECK(use.ruled);
            CHECK(std::string(use.quests).find(r.quest) != std::string::npos);
        }
        CHECK(game::AddOriginalUseRules(def) == 0); // not twice
        CHECK(ParseOne(game::WriteQuest(def)) == def);
        quests.insert(def.codename);
    }
    CHECK(quests.size() == 36); // 28 bag uses' quests, the four traps' (EASTEU_14_1's lion), ORNAMENT_4's and STONEBOARD_4's instances, the two hwan 1_4_1's
    // QNO_CA_WAREHOUSE_1 (its countdown): its numbers.
    game::QuestDef warehouse;
    warehouse.codename = "QNO_CA_WAREHOUSE_1";
    game::AddOriginalUseRules(warehouse);
    const game::QuestDef::UseRule& w = warehouse.onUse.at(0);
    CHECK(w.item == "ITEM_QNO_CA_WAREHOUSE_1_02");
    CHECK(w.statuses == std::vector<std::uint8_t>{1, 7});
    CHECK(w.firstTime);
    CHECK(w.capFirst);
    REQUIRE(w.at.size() == 1);
    CHECK(w.at[0].region == 27237);
    CHECK(w.at[0].radius == 1000.0f);
    CHECK(w.cap == 10);
    CHECK(w.seconds == 10);
    CHECK(w.chance == 68);
    CHECK(w.outOf == 101);
    CHECK(w.give == "ITEM_QNO_CA_WAREHOUSE_1_01");
    CHECK(w.failed.key == "SN_TALK_QNO_CA_WAREHOUSE_1_09");
    // A definition's own rule for the item is kept.
    game::QuestDef own = ParseOne("quest QNO_CA_WAREHOUSE_1\non use ITEM_QNO_CA_WAREHOUSE_1_02 give ITEM_T_ORE\n");
    CHECK(game::AddOriginalUseRules(own) == 0);
    CHECK(own.onUse.at(0).give == "ITEM_T_ORE");
}

TEST_CASE("Quest item use: a quest's rule takes its item only as the original's handler does (a made-up host)") {
    UseHost host;
    server::Quests quests(host, {});
    // A ground search as QNO_CA_WAREHOUSE_1's: states 1 and 7, first time only, the cap asked first (silently), the place,
    // the bag's room, ten seconds, then the roll.
    quests.Load({ParseOne("quest QNO_T_DIG\nid 20100\nstart level 1-\n"
                          "on use ITEM_T_SPADE first-time refuse @SN_T_REFUSE cap-first at 25000 100 0 100 50 away @SN_T_AWAY "
                          "cap 3 ITEM_T_ORE full @SN_T_FULL wait 10 chance 101 of 101 give ITEM_T_ORE got @SN_T_GOT\n"
                          "goal drop 3 ITEM_T_ORE from MOB_T_ROCK 0%\n"),
                 ParseOne("quest QNO_T_LUCKLESS\nid 20101\nstart level 1-\n"
                          "on use ITEM_T_DICE status 1 7 8 one-at-a-time @SN_T_BUSY wait 5 chance 0 of 101 give ITEM_T_ORE failed @SN_T_FAIL\n"
                          "goal drop 1 ITEM_T_GEM from MOB_T_ROCK 0%\n"),
                 ParseOne("quest QNO_T_FOUND\nid 20102\nstart level 1-\non use ITEM_T_MAP status 7 8 give ITEM_T_GEM\n"
                          "goal drop 1 ITEM_T_GEM from MOB_T_ROCK 0%\n")});
    host.who.position = {25000, 300.0f, 0.0f, 300.0f, 0}; // 283 from the point: away
    quests.Enter(1, {Record(20100, proto::QuestStatus::Achieving), Record(20101, proto::QuestStatus::Achieving),
                     Record(20102, proto::QuestStatus::Achieving)});

    SUBCASE("no quest's rule names it: none takes it (the caller's 2)") {
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_STRAY"));
        CHECK(host.said.empty());
    }
    SUBCASE("away, at the cap, without room: refused with its words; at the place: taken, given after its wait") {
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_SPADE"));
        CHECK(host.LastSaid() == "SN_T_AWAY");
        host.who.position = {25000, 120.0f, 0.0f, 110.0f, 0}; // 22 from it
        host.bag["ITEM_T_ORE"] = 3;
        const std::size_t said = host.said.size();
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_SPADE")); // the cap, asked first and silently
        CHECK(host.said.size() == said);
        host.bag["ITEM_T_ORE"] = 0;
        host.freePlace = false;
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_SPADE"));
        CHECK(host.LastSaid() == "SN_T_FULL");
        host.freePlace = true;
        CHECK(quests.UseItem(1, "ITEM_T_SPADE"));
        CHECK(quests.Searching(1, 20100));
        // The wait runs: nothing yet; ten seconds on, one given (and said), the goal counts it.
        quests.Tick(std::chrono::steady_clock::now() + 2s);
        CHECK(host.bag["ITEM_T_ORE"] == 0);
        quests.Tick(std::chrono::steady_clock::now() + 11s);
        CHECK(host.bag["ITEM_T_ORE"] == 1);
        CHECK(host.LastSaid() == "SN_T_GOT");
        CHECK_FALSE(quests.Searching(1, 20100));
        CHECK(quests.Record(1, 20100)->progress[0] == 1);
    }
    SUBCASE("the quest's state: a finished quest, a second time, the goals' state; one wait at a time; a lost roll") {
        // State 7 (a goal progressed) and 8 only for QNO_T_FOUND: at 1 (nothing counted yet) refused silently.
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_MAP"));
        host.bag["ITEM_T_GEM"] = 0;
        // The dice: taken (states 1 7 8), its wait running refuses a second (its words); five seconds on, the roll lost.
        CHECK(quests.UseItem(1, "ITEM_T_DICE"));
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_DICE"));
        CHECK(host.LastSaid() == "SN_T_BUSY");
        quests.Tick(std::chrono::steady_clock::now() + 6s);
        CHECK(host.LastSaid() == "SN_T_FAIL");
        CHECK(host.bag["ITEM_T_ORE"] == 0);
        // Finished before (first-time): refused with its words; not under way (finished): the same.
        server::Quests again(host, {});
        again.Load({ParseOne("quest QNO_T_DIG\nid 20100\nstart level 1-\non use ITEM_T_SPADE first-time refuse @SN_T_REFUSE give ITEM_T_ORE\n"
                             "goal drop 3 ITEM_T_ORE from MOB_T_ROCK 0%\n")});
        again.Enter(1, {Record(20100, proto::QuestStatus::Achieving, 1)});
        CHECK_FALSE(again.UseItem(1, "ITEM_T_SPADE"));
        CHECK(host.LastSaid() == "SN_T_REFUSE");
        again.Enter(1, {Record(20100, proto::QuestStatus::Done, 1)});
        host.said.clear();
        CHECK_FALSE(again.UseItem(1, "ITEM_T_SPADE"));
        CHECK(host.LastSaid() == "SN_T_REFUSE");
        // Under way the first time, no wait: given at once.
        again.Enter(1, {Record(20100, proto::QuestStatus::Achieving)});
        CHECK(again.UseItem(1, "ITEM_T_SPADE"));
        CHECK(host.bag["ITEM_T_ORE"] == 1);
        CHECK_FALSE(again.Searching(1, 20100));
    }
    SUBCASE("a quest the player does not have takes nothing; the original's rule comes with its quest") {
        server::Quests original(host, {});
        original.Load({ParseOne("quest QNO_CA_WAREHOUSE_1\nid 203\nstart level 19-\ngoal drop 10 ITEM_QNO_CA_WAREHOUSE_1_01 from MOB_QT_01_ARCHER_CLON 0%\n")});
        REQUIRE(original.Find(203) != nullptr);
        CHECK(original.Find(203)->onUse.size() == 1); // QNO_CA_WAREHOUSE_1's rule, added at the load
        original.Enter(1, {});
        CHECK_FALSE(original.UseItem(1, "ITEM_QNO_CA_WAREHOUSE_1_02"));
        // Held, at the amphitheatre's place (region 27237, 1057 218 634 within 1000): taken, ten seconds.
        original.Enter(1, {Record(203, proto::QuestStatus::Achieving)});
        host.who.position = {27237, 1000.0f, 218.0f, 600.0f, 0};
        CHECK(original.UseItem(1, "ITEM_QNO_CA_WAREHOUSE_1_02"));
        CHECK(original.Searching(1, 203));
        host.who.position = {25000, 1000.0f, 218.0f, 600.0f, 0};
        CHECK_FALSE(original.UseItem(1, "ITEM_QNO_CA_WAREHOUSE_1_02"));
        CHECK(host.LastSaid() == "SN_TALK_QNO_CA_WAREHOUSE_1_08");
    }
}

TEST_CASE("Quest item use: the two summon events (the guild support's scroll, the Pandora box; a made-up host)") {
    // Their tables: the guild support's monster by level, the Pandora's ten.
    CHECK(game::GuildSupportMonster(29).empty());
    CHECK(game::GuildSupportMonster(30) == "MOB_EVE_STRONG_WC_HYEONGCHEON");
    CHECK(game::GuildSupportMonster(110) == "MOB_EVE_STRONG_SD_SOPEDU");
    CHECK(game::GuildSupportMonster(140) == "MOB_EVE_STRONG_SD_SOPEDU");
    const auto ten = game::PandoraMonsters(30);
    REQUIRE(ten.size() == 10);
    CHECK(ten.front() == "MOB_EVE_STRONG_AM_CRAB_CLON"); // level 21
    CHECK(ten[7] == "MOB_EVE_STRONG_WC_EARTHGHOST_CLON"); // 28: the first of the two
    CHECK(ten.back() == "MOB_EVE_STRONG_WC_HYEONGCHEON"); // 30
    const auto few = game::PandoraMonsters(23); // 21..23, the lowest repeated in front
    REQUIRE(few.size() == 10);
    CHECK(std::count(few.begin(), few.end(), "MOB_EVE_STRONG_AM_CRAB_CLON") == 8);
    CHECK(few.back() == "MOB_EVE_STRONG_AM_CRAB");
    const auto top = game::PandoraMonsters(130); // capped at 110
    CHECK(top.front() == "MOB_EVE_STRONG_SD_WENEG"); // 101
    CHECK(top.back() == "MOB_EVE_STRONG_SD_SOPEDU");

    SummonsHost host;
    server::SummonEvents events(host, 7);
    const auto now = std::chrono::steady_clock::now();
    const std::string scroll(game::kGuildSupportItem), box(game::kPandoraItem);

    SUBCASE("not theirs: not taken") {
        CHECK_FALSE(events.UseItem(1, "ITEM_ETC_E050729_SUMMON_SCROLL", now));
        CHECK_FALSE(events.UseItem(1, "ITEM_EVENT_RENT_E100222_MON_GENERATION_BOX", now));
        CHECK(host.said.empty());
    }
    SUBCASE("the scroll: refused in a town, under 30, in the arena; one monster alone, as many as the party's members") {
        host.user.town = true;
        CHECK_FALSE(events.UseItem(1, scroll, now));
        CHECK(host.LastSaid() == game::kSummonInTown);
        host.user.town = false;
        host.user.level = 29;
        CHECK_FALSE(events.UseItem(1, scroll, now));
        CHECK(host.LastSaid() == game::kSummonLevelLow);
        host.user.level = 40;
        host.user.arena = true;
        CHECK_FALSE(events.UseItem(1, scroll, now));
        CHECK(host.LastSaid() == game::kSummonInArena);
        host.user.arena = false;
        CHECK(host.nests.empty());
        CHECK(events.UseItem(1, scroll, now));
        CHECK(host.LastSaid() == game::kSummonOne);
        REQUIRE(host.nests.size() == 1);
        CHECK(host.nests[0].monster == game::GuildSupportMonster(40));
        CHECK_FALSE(host.nests[0].kept);
        const double d = Distance(host.user.position, host.nests[0].at);
        CHECK(d >= 19.9);
        CHECK(d <= 100.1);
        host.user.party = 4;
        CHECK(events.UseItem(1, scroll, now));
        CHECK(host.LastSaid() == game::kSummonParty);
        CHECK(host.nests.size() == 5);
        CHECK(events.Boxes() == 0);
    }
    SUBCASE("the Pandora box: refused in a town (its words); the box, its waves of four whenever none stands, its end") {
        host.user.town = true;
        CHECK_FALSE(events.UseItem(1, box, now));
        CHECK(host.LastSaid() == game::kPandoraInTown);
        host.user.town = false;
        host.user.level = 30;
        CHECK(events.UseItem(1, box, now));
        CHECK(host.LastSaid() == game::kPandoraOpened);
        REQUIRE(host.nests.size() == 1);
        CHECK(host.nests[0].monster == game::kPandoraBox);
        CHECK(host.nests[0].kept);
        CHECK(events.Boxes() == 1);
        // Run 1 (5 s): nothing; run 2 (10 s): four of the ten, 50 from the place.
        events.Tick(now + 5s);
        CHECK(host.nests.size() == 1);
        events.Tick(now + 10s);
        REQUIRE(host.nests.size() == 5);
        for (std::size_t i = 1; i < 5; ++i) {
            CHECK(std::find(ten.begin(), ten.end(), host.nests[i].monster) != ten.end());
            CHECK(Distance(host.user.position, host.nests[i].at) == doctest::Approx(50.0).epsilon(0.01));
            CHECK(host.nests[i].kept);
        }
        // Standing: no more; one still standing: none either; all down: four more (the dead ones taken away).
        events.Tick(now + 15s);
        CHECK(host.nests.size() == 5);
        for (std::size_t i = 1; i < 4; ++i) host.nests[i].standing = 0;
        events.Tick(now + 20s);
        CHECK(host.nests.size() == 5);
        host.nests[4].standing = 0;
        events.Tick(now + 25s);
        CHECK(host.nests.size() == 9);
        CHECK(host.nests[1].removed);
        // The box dies: the daemon ends, its monsters go with it.
        host.nests[0].standing = 0;
        events.Tick(now + 30s);
        CHECK(events.Boxes() == 0);
        for (std::size_t i = 5; i < 9; ++i) CHECK(host.nests[i].removed);
    }
    SUBCASE("the Pandora's daemon ends after 240 runs, or when its user is gone") {
        CHECK(events.UseItem(1, box, now));
        events.Tick(now + 10s);
        CHECK(events.Boxes() == 1);
        events.Tick(now + std::chrono::seconds(5 * 240));
        CHECK(events.Boxes() == 0);
        CHECK(host.nests[0].removed);
        CHECK(events.UseItem(1, box, now));
        host.present = false;
        events.Tick(now + 5s);
        CHECK(events.Boxes() == 0);
    }
}

TEST_CASE("Quest item use: the catalogue's state of each quest and event item") {
    const auto row = [](std::string codename, std::array<int, 4> tid) {
        assets::ItemRow r;
        r.codename = std::move(codename);
        r.typeId = tid;
        r.canUse = 1;
        r.inService = true;
        return r;
    };
    const game::ItemRouteData none;
    const auto state = [&](const assets::ItemRow& r) { return game::ItemStateOf(game::ItemRouteOf(r, none)); };
    // No handler in 1.188: refused 2 always, as the original's: implemented.
    for (const char* code : {"ITEM_ETC_E050729_SUMMON_ONE_SCROLL", "ITEM_ETC_E050729_SUMMON_SCROLL", "ITEM_ETC_E050729_SUMMON_TWO_SCROLL",
                             "ITEM_ETC_E060125_RICEDUMPLINGSOUP", "ITEM_ETC_E060517_SUMMON_PARTY_SCROLL", "ITEM_ETC_HALLOWEEN_BOMB_BKPAINT",
                             "ITEM_ETC_HALLOWEEN_BOMB_DUST", "ITEM_ETC_HALLOWEEN_BOMB_EGG", "ITEM_ETC_HALLOWEEN_BOMB_REDPAINT",
                             "ITEM_ETC_HALLOWEEN_BOMB_SPIDERWEB", "ITEM_EVENT_E100428_TICKET"}) {
        CAPTURE(code);
        CHECK(game::QuestItemUseOf(code).taker == game::QuestItemTaker::None);
        CHECK(state(row(code, {3, 3, 9, 0})) == game::ItemState::Implemented);
    }
    CHECK(state(row("ITEM_EVENT_RENT_E100222_MON_GENERATION_BOX", {3, 3, 15, 1})) == game::ItemState::Implemented);
    CHECK(state(row("ITEM_EVENT_RENT_E100222_SUMMON_SCROLL", {3, 3, 15, 2})) == game::ItemState::Implemented);
    // The guild support's scroll and the Pandora box (its cast after each wave too): implemented.
    CHECK(state(row(std::string(game::kGuildSupportItem), {3, 3, 15, 2})) == game::ItemState::Implemented);
    CHECK(state(row(std::string(game::kPandoraItem), {3, 3, 15, 1})) == game::ItemState::Implemented);
    // The 29 quest items: each a coded quest's; whole (implemented) when its handler, its quest and the quests before it are
    // built, else partly with what stands in the way (ItemCatalogue's QuestItemGap, 2026-11-17: 27 whole; partly the hwan line's
    // second test, behind QNO_*_HWAN_1_4_1's trap).
    const std::set<std::string> partly = {}; // none since the hwan tests' 1_4_1 trap (2026-11-17)
    int quest = 0, ruled = 0;
    for (const char* code :
         {"ITEM_QNO_CA_GORIA_7_03", "ITEM_QNO_CA_TREASURE_5_01", "ITEM_QNO_CA_WAREHOUSE_1_02", "ITEM_QNO_CA_WAREHOUSE_3_01", "ITEM_QNO_CH_HWAN_1_3_02",
          "ITEM_QNO_CH_WHAN_2_3_01", "ITEM_QNO_CH_WHAN_2_4_01", "ITEM_QNO_EU_ADVENTURER_1_01", "ITEM_QNO_EU_EASTEU_11_02", "ITEM_QNO_EU_EASTEU_17_1_01",
          "ITEM_QNO_EU_EASTEU_19_01", "ITEM_QNO_EU_EASTEU_19_03", "ITEM_QNO_EU_EASTEU_21_01", "ITEM_QNO_EU_EASTEU_3_02", "ITEM_QNO_EU_EASTEU_5_01",
          "ITEM_QNO_EU_GENERAL_2_01", "ITEM_QNO_EU_IVY_2_03", "ITEM_QNO_EU_IVY_3_01", "ITEM_QNO_EU_IVY_3_05", "ITEM_QNO_EU_IVY_3_06",
          "ITEM_QNO_EU_WITCH_1_02", "ITEM_QNO_RM_FLYSHIP1_3_01", "ITEM_QNO_RM_OLDWOMAN_5_08", "ITEM_QNO_TQ_BAEKRYEONG_4_01",
          "ITEM_QNO_TQ_FINDTHIEF_2_01", "ITEM_QNO_TQ_ORNAMENT_3_01", "ITEM_QNO_TQ_ORNAMENT_4_04", "ITEM_QNO_TQ_STONEBOARD_4_03",
          "ITEM_QNO_WC_WAREHOUSE_W_2_01"}) {
        CAPTURE(code);
        const game::QuestItemUse u = game::QuestItemUseOf(code);
        CHECK(u.taker == game::QuestItemTaker::Quest);
        CHECK(!u.kind.empty());
        ++quest;
        ruled += u.ruled ? 1 : 0;
        CHECK(state(row(code, {3, 3, 9, 0})) == (partly.contains(code) ? game::ItemState::Partly : game::ItemState::Implemented));
    }
    CHECK(quest == 29);
    CHECK(ruled == 29); // every one
}

TEST_CASE("Quest item use: the other handlers' parts read and write back the same; the original's numbers") {
    const game::QuestDef q = ParseOne("quest QNO_T_ALL\nid 20110\nstart level 1-\n"
                                      "on use ITEM_T_FLAG status 1 7 met @SN_T_MET party @SN_T_PARTY at 25000 100 0 100 50 "
                                      "spawn MOB_T_A 1 radius 20 100 life 300 spawn MOB_T_B 4 radius 90 100\n"
                                      "on use ITEM_T_TRAP target MOB_T_A MOB_T_B within 300 none @SN_T_NONE far @SN_T_FAR wrong @SN_T_WRONG "
                                      "failed @SN_T_FAILED got @SN_T_GOT capture MOB_T_A_COS minutes 20 roll 50 step 3 below already @SN_T_ALREADY "
                                      "warn 10 @SN_T_W10 warn 5 @SN_T_W5 ran @SN_T_RAN owner-died @SN_T_OWNER cos-died @SN_T_COS\n"
                                      "on use ITEM_T_ESSENCE status 7 8 stage 24180 1081 5775 1631 270 5 26 stage 22379 1013 5910 85 280 40 101 "
                                      "give ITEM_T_GRASS\n"
                                      "on use ITEM_T_COIN status 7 8 chance 1 of 2 limit 60 ITEM_T_A take ITEM_T_B give ITEM_T_A or-give ITEM_T_C kept-in 8\n"
                                      "on use ITEM_T_DRUM instance INS_QNO_T unique MOB_T_BOSS master @SN_T_LEADER comes @SN_T_C1 comes @SN_T_C2 warn 10 @SN_T_W begun @SN_T_B "
                                      "slain @SN_T_S over @SN_T_O\n"
                                      "goal capture MOB_T_A_COS at NPC_T_WITCH text \"Bring it\"\n");
    REQUIRE(q.onUse.size() == 5);
    const game::QuestDef::UseRule& flag = q.onUse[0];
    CHECK(flag.met.key == "SN_T_MET");
    CHECK(flag.party);
    CHECK(flag.noParty.key == "SN_T_PARTY");
    REQUIRE(flag.spawns.size() == 2);
    CHECK(flag.spawns[0].seconds == 300);
    CHECK(flag.spawns[1].count == 4);
    CHECK(flag.spawns[1].near == 90.0f);
    const game::QuestDef::UseRule& trap = q.onUse[1];
    CHECK(trap.targets == std::vector<std::string>{"MOB_T_A", "MOB_T_B"});
    CHECK(trap.targetRange == 300.0f);
    REQUIRE(trap.capture);
    CHECK(trap.capture->cos == "MOB_T_A_COS");
    CHECK(trap.capture->below);
    CHECK(trap.capture->warnings.size() == 2);
    CHECK(trap.capture->cosDied.key == "SN_T_COS");
    const game::QuestDef::UseRule& essence = q.onUse[2];
    REQUIRE(essence.stages.size() == 2);
    CHECK(essence.stages[0].at.region == 24180);
    CHECK(essence.stages[1].seconds == 40);
    CHECK(essence.stages[1].chance == 101);
    const game::QuestDef::UseRule& coin = q.onUse[3];
    CHECK(coin.orGive == "ITEM_T_C");
    CHECK(coin.take == "ITEM_T_B");
    CHECK(coin.limit == 60);
    CHECK(coin.keptIn == std::vector<std::uint8_t>{8});
    REQUIRE(q.onUse[4].instance);
    CHECK(q.onUse[4].instance->world == "INS_QNO_T");
    CHECK(q.onUse[4].instance->near == 20.0f); // the default 20..100
    CHECK(q.onUse[4].instance->unique == "MOB_T_BOSS");
    CHECK(q.onUse[4].instance->comes.size() == 2);
    CHECK(q.onUse[4].instance->warnings.size() == 1);
    REQUIRE(q.goals.size() == 1);
    CHECK(q.goals[0].kind == game::QuestGoalKind::Capture);
    CHECK(q.goals[0].npc == "NPC_T_WITCH");
    CHECK(ParseOne(game::WriteQuest(q)) == q);
    game::QuestDef bad;
    std::string error;
    CHECK_FALSE(game::ParseQuest("quest QNO_T\non use ITEM_T minutes 5\n", bad, error)); // a capture's part without a capture
    CHECK_FALSE(game::ParseQuest("quest QNO_T\non use ITEM_T stage 1 0 0 0 10 5 102\n", bad, error));
    CHECK_FALSE(game::ParseQuest("quest QNO_T\ngoal capture MOB_T_COS\n", bad, error));

    // The original's numbers.
    const auto rulesOf = [](std::string_view quest) {
        game::QuestDef d;
        d.codename = std::string(quest);
        game::AddOriginalUseRules(d);
        return d.onUse;
    };
    const auto general = rulesOf("QNO_EU_GENERAL_2");
    REQUIRE(general.size() == 1);
    CHECK(general[0].party);
    REQUIRE(general[0].spawns.size() == 2);
    CHECK(general[0].spawns[0].monster == "MOB_QT_01_ROGUE");
    CHECK(general[0].spawns[1].monster == "MOB_QT_01_ROGUE_CLON");
    CHECK(general[0].spawns[1].count == 4);
    CHECK(general[0].spawns[1].seconds == 300);
    const auto hwan = rulesOf("QNO_CH_HWAN_1_3_1");
    REQUIRE(hwan.size() == 1);
    CHECK(hwan[0].spawns.at(0).near == 90.0f);
    CHECK(hwan[0].firstTime);
    CHECK(rulesOf("QNO_EU_EASTEU_19").size() == 2); // the scissors' search and the apple's spawn
    const auto fly = rulesOf("QNO_RM_FLYSHIP1_3");
    REQUIRE(fly.at(0).capture);
    CHECK(fly[0].capture->below);
    CHECK(fly[0].capture->minutes == 50);
    CHECK(fly[0].targetRange == 0.0f);
    const auto thief = rulesOf("QNO_TQ_BAEKRYOUNG_4");
    CHECK(thief.at(0).capture->minutes == 60);
    const auto oldwoman = rulesOf("QNO_RM_OLDWOMAN_5");
    REQUIRE(oldwoman.at(0).stages.size() == 7);
    CHECK(oldwoman[0].stages[0].seconds == 5);
    CHECK(oldwoman[0].stages[0].chance == 26); // rand() % 101 <= 25
    CHECK(oldwoman[0].stages[6].chance == 101);
    CHECK(oldwoman[0].cap == 7);
    const auto coinW3 = rulesOf("QNO_WC_WAREHOUSE_W_3");
    CHECK(coinW3.at(0).cap == 130);
    CHECK(coinW3[0].keptIn == std::vector<std::uint8_t>{8});
    // The capture's chance (FLYSHIP's) and the spawn's banded radius.
    CHECK(game::CaptureChance(50, 3, false, 40, 30) == 80);
    CHECK(game::CaptureChance(50, 3, false, 30, 40) == 50);
    CHECK(game::CaptureChance(50, 3, true, 30, 40) == 20);
    CHECK(game::CaptureChance(50, 3, true, 40, 30) == 50);
    std::mt19937 random(3);
    int inner = 0, outer = 0;
    for (int i = 0; i < 2000; ++i) {
        const game::SpawnOffset o = game::QuestSpawnOffset(20.0f, 100.0f, random);
        const float d = std::hypot(o.dx, o.dz);
        CHECK(d <= 100.01f);
        const game::SpawnOffset h = game::QuestSpawnOffset(90.0f, 100.0f, random);
        CHECK(std::hypot(h.dx, h.dz) <= 100.01f);
        if (std::hypot(h.dx, h.dz) < 34.0f) ++inner;
        if (std::hypot(h.dx, h.dz) >= 60.0f) ++outer;
    }
    CHECK(inner > 100);   // about 11 % within a third
    CHECK(outer > 1200);  // about 70 % in the outer third
}

TEST_CASE("Quest item use: the spawns, the captures, the stages, the roll and the coin (a made-up host)") {
    UseHost host;
    server::Quests quests(host, {});
    quests.Load({ParseOne("quest QNO_T_FLAG\nid 20120\nstart level 1-\n"
                          "on use ITEM_T_FLAG status 1 7 refuse @SN_T_REFUSE met @SN_T_MET party @SN_T_PARTY at 25000 100 0 100 50 away @SN_T_AWAY "
                          "spawn MOB_T_ROGUE 1 radius 20 100 life 300 spawn MOB_T_CLONE 4 radius 20 100 life 300\n"
                          "on use ITEM_T_NOTHING spawn MOB_T_NONE 1\n"
                          "goal kill 1 MOB_T_ROGUE\n"),
                 ParseOne("quest QNO_T_TRAP\nid 20121\nstart level 1-\n"
                          "on use ITEM_T_TRAP status 1 7 target MOB_T_DOWB within 300 none @SN_T_NONE far @SN_T_FAR wrong @SN_T_WRONG "
                          "failed @SN_T_FAILED capture MOB_T_DOWB_COS minutes 20 roll 50 step 3 already @SN_T_ALREADY warn 10 @SN_T_W10 "
                          "warn 5 @SN_T_W5 ran @SN_T_RAN owner-died @SN_T_OWNER cos-died @SN_T_COSDIED\n"
                          "goal capture MOB_T_DOWB_COS at NPC_T_WITCH\n"),
                 ParseOne("quest QNO_T_STAGES\nid 20122\nstart level 1-\n"
                          "on use ITEM_T_ESSENCE status 7 8 stage 25000 100 0 100 50 5 101 stage 25000 400 0 400 50 10 101 away @SN_T_AWAY "
                          "cap 2 ITEM_T_GRASS @SN_T_AWAY full @SN_T_FULL give ITEM_T_GRASS failed @SN_T_LOST\n"
                          "goal kill 5 MOB_T_ROCKY\ngoal drop 2 ITEM_T_GRASS from MOB_T_NONE 0%\n"),
                 ParseOne("quest QNO_T_ROLL\nid 20123\nstart level 1-\n"
                          "on use ITEM_T_BLOOD status 1 7 full @SN_T_FULL chance 2 of 2 limit 2 ITEM_T_LIFTED take ITEM_T_BOX give ITEM_T_LIFTED\n"
                          "on use ITEM_T_CURSED status 1 7 chance 0 of 2 give ITEM_T_LIFTED failed @SN_T_CURSED\n"
                          "goal drop 3 ITEM_T_LIFTED from MOB_T_NONE 0%\n"),
                 ParseOne("quest QNO_T_COIN\nid 20124\nstart level 1-\n"
                          "on use ITEM_T_AMULET status 7 8 cap 3 ITEM_T_RARE chance 2 of 2 give ITEM_T_RARE or-give ITEM_T_BAD kept-in 8\n"
                          "on use ITEM_T_BADLUCK status 7 8 chance 0 of 2 give ITEM_T_RARE or-give ITEM_T_BAD kept-in 8\n"
                          "goal drop 2 ITEM_T_GOLD from MOB_T_NONE 0%\n")});
    host.who.position = {25000, 100.0f, 0.0f, 100.0f, 0};
    const auto rec = [](std::uint32_t quest, std::uint16_t progress = 0) { return Record(quest, proto::QuestStatus::Achieving, 0, progress); };

    SUBCASE("spawns: the state's words, the party, the place; one rogue and four clones about the player, gone after their time") {
        quests.Enter(1, {Record(20120, proto::QuestStatus::Achieved, 0, 1)});
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_FLAG"));
        CHECK(host.LastSaid() == "SN_T_MET"); // its goals met: its own words
        quests.Enter(1, {rec(20120)});
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_FLAG"));
        CHECK(host.LastSaid() == "SN_T_PARTY"); // no auto-share party
        host.who.sharedParty = true;
        host.who.position = {25000, 300.0f, 0.0f, 300.0f, 0};
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_FLAG"));
        CHECK(host.LastSaid() == "SN_T_AWAY");
        host.who.position = {25000, 110.0f, 0.0f, 100.0f, 0};
        CHECK(quests.UseItem(1, "ITEM_T_FLAG"));
        REQUIRE(host.raised.size() == 5);
        CHECK(host.raised[0].monster == "MOB_T_ROGUE");
        for (std::size_t i = 1; i < 5; ++i) CHECK(host.raised[i].monster == "MOB_T_CLONE");
        for (const auto& r : host.raised) CHECK(Distance(host.who.position, r.at) <= 100.1);
        CHECK(quests.Raised() == 5);
        quests.Tick(std::chrono::steady_clock::now() + 299s);
        CHECK_FALSE(host.raised[0].gone);
        quests.Tick(std::chrono::steady_clock::now() + 301s);
        for (const auto& r : host.raised) CHECK(r.gone);
        CHECK(quests.Raised() == 0);
        // No such monster: nothing raised, the use refused (the item kept).
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_NOTHING"));
    }
    SUBCASE("captures: the selection, its reach, the monster; caught as the COS for its minutes, its words, handed in at the NPC") {
        quests.Enter(1, {rec(20121)});
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_TRAP"));
        CHECK(host.LastSaid() == "SN_T_NONE");
        host.selected = server::QuestHost::Selected{77, "MOB_T_DOWB", {25000, 600.0f, 0.0f, 100.0f, 0}};
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_TRAP"));
        CHECK(host.LastSaid() == "SN_T_FAR");
        host.selected = server::QuestHost::Selected{78, "MOB_T_WOLF", {25000, 150.0f, 0.0f, 100.0f, 0}};
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_TRAP"));
        CHECK(host.LastSaid() == "SN_T_WRONG");
        // The roll: 50 + 3 a level above the COS: at 17 levels above, always (101 of 101).
        host.selected = server::QuestHost::Selected{77, "MOB_T_DOWB", {25000, 150.0f, 0.0f, 100.0f, 0}};
        host.levels["MOB_T_DOWB_COS"] = 23;
        CHECK(quests.UseItem(1, "ITEM_T_TRAP"));
        CHECK(host.caughtMonster == 77);
        CHECK(host.caughtAs == "MOB_T_DOWB_COS");
        const auto [cos, minutes] = quests.Caught(1, 20121);
        CHECK(cos != 0);
        CHECK(minutes == 20);
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_TRAP"));
        CHECK(host.LastSaid() == "SN_T_ALREADY");
        SUBCASE("its minutes: the warnings at 10 and 5, then it runs away") {
            quests.Tick(std::chrono::steady_clock::now() + 10min + 1s);
            CHECK(host.LastSaid() == "SN_T_W10");
            CHECK(quests.Caught(1, 20121).second == 10);
            quests.Tick(std::chrono::steady_clock::now() + 15min + 1s);
            CHECK(host.LastSaid() == "SN_T_W5");
            quests.Tick(std::chrono::steady_clock::now() + 20min + 1s);
            CHECK(host.LastSaid() == "SN_T_RAN");
            CHECK(quests.Caught(1, 20121).first == 0);
            CHECK_FALSE(host.coses.contains(cos));
        }
        SUBCASE("its owner's death, its own death") {
            quests.Died(1);
            CHECK(host.LastSaid() == "SN_T_OWNER");
            CHECK(quests.Caught(1, 20121).first == 0);
            CHECK(quests.UseItem(1, "ITEM_T_TRAP")); // caught again
            host.coses[quests.Caught(1, 20121).first] = false;
            quests.Tick(std::chrono::steady_clock::now() + 2s);
            CHECK(host.LastSaid() == "SN_T_COSDIED");
            CHECK(quests.Caught(1, 20121).first == 0);
        }
        SUBCASE("handed in at its NPC: the goal met, the COS taken") {
            quests.Handle(1, proto::QuestHandIn::kId, proto::Encode(proto::QuestHandIn{9, 20121, {}}));
            CHECK(quests.Record(1, 20121)->status == proto::QuestStatus::Done); // its only goal met there: paid
            CHECK(quests.Caught(1, 20121).first == 0);
            CHECK_FALSE(host.coses.contains(cos));
        }
        SUBCASE("a lost roll: the trap used, nothing caught") {
            server::Quests other(host, {});
            other.Load({ParseOne("quest QNO_T_TRAP\nid 20121\nstart level 1-\non use ITEM_T_TRAP target MOB_T_DOWB failed @SN_T_FAILED "
                                 "capture MOB_T_DOWB_COS roll 0 step 0\ngoal capture MOB_T_DOWB_COS at NPC_T_WITCH\n")});
            other.Enter(1, {rec(20121)});
            CHECK(other.UseItem(1, "ITEM_T_TRAP"));
            CHECK(host.LastSaid() == "SN_T_FAILED");
            CHECK(other.Caught(1, 20121).first == 0);
        }
    }
    SUBCASE("stages: the step's place in turn, its wait and chance, the step on after each") {
        quests.Enter(1, {rec(20122)});
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_ESSENCE")); // state 1: not 7 or 8, silently
        CHECK(host.said.empty());
        quests.Enter(1, {rec(20122, 1)});
        host.who.position = {25000, 400.0f, 0.0f, 400.0f, 0}; // the second stage's place, the first's turn
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_ESSENCE"));
        CHECK(host.LastSaid() == "SN_T_AWAY");
        host.who.position = {25000, 100.0f, 0.0f, 100.0f, 0};
        CHECK(quests.UseItem(1, "ITEM_T_ESSENCE"));
        quests.Tick(std::chrono::steady_clock::now() + 4s);
        CHECK(host.bag["ITEM_T_GRASS"] == 0);
        quests.Tick(std::chrono::steady_clock::now() + 6s);
        CHECK(host.bag["ITEM_T_GRASS"] == 1);
        CHECK(quests.Record(1, 20122)->progress[7] == 1); // the step, the record's last count
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_ESSENCE")); // the first place: no longer its turn
        CHECK(host.LastSaid() == "SN_T_AWAY");
        host.who.position = {25000, 400.0f, 0.0f, 400.0f, 0};
        host.freePlace = false;
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_ESSENCE"));
        CHECK(host.LastSaid() == "SN_T_FULL");
        host.freePlace = true;
        CHECK(quests.UseItem(1, "ITEM_T_ESSENCE"));
        quests.Tick(std::chrono::steady_clock::now() + 11s);
        CHECK(host.bag["ITEM_T_GRASS"] == 2);
        CHECK(quests.Record(1, 20122)->progress[7] == 2);
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_ESSENCE")); // past the last stage
    }
    SUBCASE("the roll: one taken for the gift, none given at the limit (the item used), a lost roll's words") {
        quests.Enter(1, {rec(20123)});
        host.bag["ITEM_T_BOX"] = 3;
        CHECK(quests.UseItem(1, "ITEM_T_BLOOD"));
        CHECK(host.bag["ITEM_T_BOX"] == 2);
        CHECK(host.bag["ITEM_T_LIFTED"] == 1);
        CHECK(quests.UseItem(1, "ITEM_T_BLOOD"));
        CHECK(host.bag["ITEM_T_LIFTED"] == 2);
        CHECK(quests.UseItem(1, "ITEM_T_BLOOD")); // at the limit: used, nothing exchanged
        CHECK(host.bag["ITEM_T_BOX"] == 1);
        CHECK(host.bag["ITEM_T_LIFTED"] == 2);
        host.bag["ITEM_T_LIFTED"] = 0;
        host.bag["ITEM_T_BOX"] = 0;
        CHECK(quests.UseItem(1, "ITEM_T_BLOOD")); // no box: used, nothing given
        CHECK(host.bag["ITEM_T_LIFTED"] == 0);
        CHECK(quests.UseItem(1, "ITEM_T_CURSED"));
        CHECK(host.LastSaid() == "SN_T_CURSED");
        host.freePlace = false;
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_BLOOD"));
        CHECK(host.LastSaid() == "SN_T_FULL");
    }
    SUBCASE("the coin: taken in state 7; in state 8 given and kept (the original's quirk); at its cap kept; its grant failing kept") {
        host.bag["ITEM_T_GOLD"] = 1; // a goal progressed: state 7
        quests.Enter(1, {rec(20124)});
        CHECK(quests.UseItem(1, "ITEM_T_AMULET"));
        CHECK(host.bag["ITEM_T_RARE"] == 1);
        CHECK(quests.UseItem(1, "ITEM_T_BADLUCK"));
        CHECK(host.bag["ITEM_T_BAD"] == 1);
        host.bag["ITEM_T_GOLD"] = 2; // its goals met: state 8
        quests.Enter(1, {rec(20124)});
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_AMULET")); // refused 2...
        CHECK(host.bag["ITEM_T_RARE"] == 2);             // ...the charm given all the same
        host.bag["ITEM_T_RARE"] = 3;
        host.bag["ITEM_T_GOLD"] = 1;
        quests.Enter(1, {rec(20124)});
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_AMULET"));
        CHECK(host.bag["ITEM_T_RARE"] == 3);
        host.bag["ITEM_T_RARE"] = 0;
        host.giveFails = true;
        CHECK_FALSE(quests.UseItem(1, "ITEM_T_BADLUCK"));
    }
}

TEST_CASE("Quest item use: the Pandora box casts its skill after each wave (a made-up host)") {
    SummonsHost host;
    server::SummonEvents events(host, 11);
    const auto now = std::chrono::steady_clock::now();
    CHECK(events.UseItem(1, std::string(game::kPandoraItem), now));
    events.Tick(now + 5s);
    CHECK(host.casts.empty()); // the first run binds the box
    events.Tick(now + 10s);
    REQUIRE(host.casts.size() == 1);
    CHECK(host.casts[0].first == 0); // the box's nest
    CHECK(host.casts[0].second == game::kPandoraCast);
    CHECK(events.BoxViews().at(0).casts == 1);
    events.Tick(now + 15s); // its monsters stand: no wave, no cast
    CHECK(host.casts.size() == 1);
    for (std::size_t i = 1; i < host.nests.size(); ++i) host.nests[i].standing = 0;
    events.Tick(now + 20s);
    CHECK(host.casts.size() == 2);
}

TEST_CASE("Game server: a quest's key raises its monster, its trap catches the selected monster as the quest's COS (game files)") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    test::TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    auto items = std::make_shared<assets::ItemTable>();
    REQUIRE_MESSAGE(items->Load(fs, error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    const assets::ItemRow* key = items->Find(std::string_view("ITEM_QNO_CA_TREASURE_5_01"));
    const assets::ItemRow* trap = items->Find(std::string_view("ITEM_QNO_EU_EASTEU_5_01"));
    const assets::ObjectRow* ong = characters->Find(std::string_view("MOB_QT_01_ONG"));
    const assets::ObjectRow* dowb = characters->Find(std::string_view("MOB_EU_DOWB"));
    const assets::ObjectRow* dowbCos = characters->Find(std::string_view("MOB_QT_01_DOWB_COS"));
    REQUIRE(key != nullptr);
    REQUIRE(trap != nullptr);
    REQUIRE(ong != nullptr);
    REQUIRE(dowb != nullptr);
    REQUIRE(dowbCos != nullptr);
    // Two of the coded quests by hand (ours: their definitions come with their families); the original's rules for their
    // items join them at the load (game::AddOriginalUseRules).
    for (const char* text : {"quest QNO_CA_TREASURE_5\nid 20140\nstart level 1-\ngiver NPC_T_NOBODY\ngoal kill 1 MOB_QT_01_ONG\n",
                             "quest QNO_EU_EASTEU_5\nid 20141\nstart level 1-\ngiver NPC_T_NOBODY\ngoal capture MOB_QT_01_DOWB_COS at NPC_EU_WITCH\n"}) {
        REQUIRE_MESSAGE(server::SaveQuest(t.Db(), ParseOne(text), server::QuestOrigin::Ours, error), error);
    }
    core::JobSystem jobs{2};
    net::NetService net{1};
    server::GameServerConfig config;
    config.listen = test::Local();
    config.databaseUrl = t.Url();
    auto rules = std::make_shared<game::CharacterRules>();
    for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
    rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
    rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
    rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
    rules->items = items;
    config.rules = rules;
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.logoutCountdown = std::chrono::seconds(0);
    config.files = &fs;
    config.characters = characters;
    // Where the key works (QNO_CA_TREASURE_5: region 26219 (1525, 152, 1495) within 400), a Dow Genemas beside.
    const proto::WorldPosition place{26219, 1525.0f, 152.0f, 1495.0f, 0};
    server::MonsterNest nest;
    nest.refObjId = dowb->refObjId;
    nest.position = {26219, 1560.0f, 152.0f, 1495.0f, 0};
    nest.radius = 100;
    nest.maxCount = 1;
    config.monsterNests = {nest};
    server::GameServer game;
    REQUIRE_MESSAGE(game.Start(config, error), error);
    REQUIRE(server::CreateAccount(t.Db(), "Catcheracct", "secret1", error) == server::CreateAccountResult::Created);
    bool ok = false;
    const std::int64_t account = server::FindAccount(t.Db(), "Catcheracct", ok)->id;
    test::Client c(net, game.Port(), [&] { game.Poll(2ms); });
    REQUIRE(c.Greet());
    proto::LobbyEnter e;
    e.ticket = server::IssueTicket(t.Db(), account, config.id, 30, error);
    c.Send(e);
    proto::LobbyEntered entered;
    REQUIRE(c.Expect(entered));
    proto::CharacterCreate m;
    m.name = "Catcher";
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
    REQUIRE(server::SaveProgress(t.Db(), {{character, 80, 0, 0, 0, 40, 40, 0}}));
    REQUIRE(server::SavePlaces(t.Db(), {{character, {place.region, place.x, place.y, place.z, 0.0f}}}));
    REQUIRE(server::SaveVitals(t.Db(), character, 1'000'000, 100'000));
    for (const std::int64_t quest : {20140, 20141}) {
        REQUIRE(t.Db().Query("INSERT INTO _charquest (charid, questid, status, achievementcount, starttime, endtime, questdata1, questdata2) "
                             "VALUES ($1, $2, 1, 0, now(), now(), 0, 0)",
                             {character, quest})
                    .Ok());
    }
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {account}).Ok());
    server::StoredItem keys, traps;
    keys.refId = key->refObjId;
    traps.refId = trap->refObjId;
    keys.quantity = traps.quantity = 2;
    keys.counted = traps.counted = true;
    REQUIRE(server::GiveItems(t.Db(), config.id, "Catcher", {keys, traps}, 0, error) == server::GiveResult::Done);
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, $2)", {account, int(config.id)}).Ok());
    c.Send(proto::CharacterSelect{"Catcher"});
    REQUIRE(c.Expect(r));
    c.Send(proto::WorldEnter{});
    proto::WorldEntered w;
    REQUIRE(c.Expect(w));
    REQUIRE(w.status == proto::WorldStatus::Ok);
    proto::InventoryList bag;
    REQUIRE(c.Expect(bag));
    std::map<std::uint32_t, std::uint8_t> slot;
    for (const proto::ItemRecord& i : bag.items) slot[i.item] = i.slot;
    REQUIRE(slot.contains(key->refObjId));
    REQUIRE(slot.contains(trap->refObjId));
    // Messages seen: the monsters and COS that appear, the uses' answers.
    std::map<std::uint32_t, proto::EntityAppeared> seen;
    std::set<std::uint32_t> gone;
    const auto pump = [&](auto&& until, std::chrono::milliseconds most) {
        const auto end = std::chrono::steady_clock::now() + most;
        while (std::chrono::steady_clock::now() < end && !until()) {
            proto::EntityAppeared a;
            if (c.Expect(a, 5ms)) seen[a.entity] = a;
            proto::EntityGone g;
            if (c.Expect(g, 5ms)) gone.insert(g.entity);
        }
        return until();
    };
    const auto find = [&](std::uint32_t model, proto::EntityKind kind) -> std::uint32_t {
        for (const auto& [entity, a] : seen)
            if (a.character.model == model && a.kind == kind && !gone.contains(entity)) return entity;
        return 0;
    };
    REQUIRE(pump([&] { return find(dowb->refObjId, proto::EntityKind::Monster) != 0; }, 20s));
    const std::uint32_t target = find(dowb->refObjId, proto::EntityKind::Monster);

    // The key at its place: its monster comes up within 100 of the player (and goes after five minutes, not waited for).
    proto::UseItem use;
    use.slot = slot[key->refObjId];
    c.Send(use);
    REQUIRE(pump([&] { return find(ong->refObjId, proto::EntityKind::Monster) != 0; }, 10s));
    const proto::EntityAppeared& raised = seen[find(ong->refObjId, proto::EntityKind::Monster)];
    const proto::FlatPosition a = proto::ToFlat(raised.position), b = proto::ToFlat(place);
    CHECK(std::hypot(a.x - b.x, a.z - b.z) <= 101.0);

    // The trap on the selected Dow Genemas (the use's target, the client's selection): at level 80 above the COS's 12, caught for
    // sure (50 + 3 x 68);
    // the monster goes without dying, the quest's COS comes as the player's.
    use.slot = slot[trap->refObjId];
    use.target = target;
    c.Send(use);
    REQUIRE(pump([&] { return gone.contains(target) && find(dowbCos->refObjId, proto::EntityKind::Cos) != 0; }, 10s));
    // A second trap at once: its COOLTIME's wait refuses it first (0x185B, before the quests' handlers, as the original's);
    // after it the quest's own words (SN_TALK_QNO_EU_EASTEU_5_21) and 2, the made-up host's case.
    c.Send(use);
    proto::ItemRefused refused;
    REQUIRE(c.Expect(refused, 5s));
    CHECK(refused.code == game::kCoolTimeRefused);
}

TEST_CASE("Quest item use: a quest's instance: its master's party in, its unique, its time, its death, a member out of the party (a made-up host)") {
    UseHost host;
    host.who.sharedParty = true;
    host.who.position = {25000, 100.0f, 0.0f, 100.0f, 0};
    for (const std::uint32_t e : {2u, 3u, 4u}) {
        server::QuestHost::Player m = host.who;
        m.peer = e;
        m.name = "Member" + std::to_string(e);
        host.others[e] = m;
    }
    host.party = {1, 2, 3, 4};
    server::Quests quests(host, {});
    quests.Load({ParseOne("quest QNO_T_DRUM\nid 20130\nstart level 1-\n"
                          "on use ITEM_T_DRUM status 1 7 8 party @SN_T_PARTY at 25000 100 0 100 700 away @SN_T_AWAY instance INS_QNO_T "
                          "unique MOB_T_BOSS after 30 minutes 20 master @SN_T_MASTER transport @SN_T_TRANSPORT comes @SN_T_COMES1 "
                          "comes @SN_T_COMES2 begun @SN_T_BEGUN warn 10 @SN_T_W10 warn 5 @SN_T_W5 slain @SN_T_SLAIN over @SN_T_OVER\n"
                          "goal kill 1 MOB_T_BOSS\n")});
    quests.Enter(1, {Record(20130, proto::QuestStatus::Achieving)});
    quests.Enter(2, {Record(20130, proto::QuestStatus::Done, 1)}); // finished it before: comes along
    quests.Enter(3, {Record(20130, proto::QuestStatus::Achieving)}); // under way without the drum: stays
    quests.Enter(4, {});                                            // holds a drum: comes along
    host.bag["ITEM_T_DRUM"] = 1; // the host's bag is every player's here...
    host.emptyBags = {3};         // ...but 3's
    // Not its master, a transport out: refused with their words.
    CHECK_FALSE(quests.UseItem(1, "ITEM_T_DRUM"));
    CHECK(host.LastSaid() == "SN_T_MASTER");
    host.master = true;
    host.transport = true;
    CHECK_FALSE(quests.UseItem(1, "ITEM_T_DRUM"));
    CHECK(host.LastSaid() == "SN_T_TRANSPORT");
    host.transport = false;
    CHECK(quests.Worlds().empty());
    // Opened: the drum kept (the use refused 2, the original's), the master in its layer at once; the members who may come
    // asked first (QuestEnterAsk, the original's invitation), in on their yes.
    CHECK_FALSE(quests.UseItem(1, "ITEM_T_DRUM"));
    REQUIRE(quests.Worlds().size() == 1);
    const std::uint32_t layer = quests.Worlds()[0];
    CHECK(host.layers[1] == layer);
    CHECK((!host.layers.contains(2) || host.layers[2] == 0));
    const auto reply = [&](std::uint32_t member, std::uint8_t yes) {
        return quests.Handle(member, proto::QuestEnterReply::kId, proto::Encode(proto::QuestEnterReply{20130, yes}));
    };
    CHECK(reply(2, 1));
    CHECK(reply(4, 1));
    CHECK(reply(3, 1)); // never asked: nothing
    CHECK(reply(2, 1)); // answered already: nothing more
    CHECK(host.layers[2] == layer);
    CHECK(host.layers[4] == layer);
    CHECK((!host.layers.contains(3) || host.layers[3] == 0));
    const auto start = std::chrono::steady_clock::now();
    quests.Tick(start + 29s);
    CHECK(host.raised.empty());
    quests.Tick(start + 31s);
    REQUIRE(host.raised.size() == 1);
    CHECK(host.raised[0].monster == "MOB_T_BOSS");
    CHECK(host.raised[0].at.layer == layer);
    proto::WorldPosition here = host.who.position;
    here.layer = layer;
    CHECK(Distance(here, host.raised[0].at) <= 100.1);
    // The opening: its first line 2 s on, the next 5 s later, the battle begun 5 s after the last (the original's 2000 and
    // 5000 ms, IVY_3's tick).
    CHECK_FALSE(host.Told(2, "SN_T_COMES1"));
    quests.Tick(start + 33s);
    CHECK(host.Told(2, "SN_T_COMES1"));
    CHECK_FALSE(host.Told(2, "SN_T_COMES2"));
    quests.Tick(start + 38s);
    CHECK(host.Told(2, "SN_T_COMES2"));
    CHECK_FALSE(host.Told(4, "SN_T_BEGUN"));
    quests.Tick(start + 43s);
    CHECK(host.Told(4, "SN_T_BEGUN"));

    SUBCASE("slain: its words, everyone out, the instance gone") {
        host.raised[0].standing = false;
        quests.Tick(start + 40s);
        CHECK(host.Told(1, "SN_T_SLAIN"));
        quests.Tick(start + 41s);
        CHECK(host.layers[1] == 0);
        CHECK(host.layers[2] == 0);
        CHECK(quests.Worlds().empty());
    }
    SUBCASE("its time: the warnings, then its words, the unique gone and everyone out 10 s later") {
        // Its 20 minutes from the battle's begin (43 s).
        quests.Tick(start + 43s + 10min + 1s);
        CHECK(host.Told(1, "SN_T_W10"));
        CHECK_FALSE(host.Told(1, "SN_T_W5"));
        quests.Tick(start + 43s + 15min + 1s);
        CHECK(host.Told(2, "SN_T_W5"));
        quests.Tick(start + 43s + 20min + 1s);
        CHECK(host.Told(4, "SN_T_OVER"));
        CHECK(host.raised[0].gone);
        CHECK(host.layers[1] == layer);
        quests.Tick(start + 43s + 20min + 12s);
        CHECK(host.layers[1] == 0);
        CHECK(quests.Worlds().empty());
    }
    SUBCASE("a member out of the party: told, out 5 s later; the others stay") {
        host.party = {1, 4};
        quests.Tick(start + 50s);
        CHECK(host.Told(2, "UIIT_MSG_QUEST_INS_CONDITION"));
        CHECK(host.layers[2] == layer);
        quests.Tick(start + 56s);
        CHECK(host.layers[2] == 0);
        CHECK(host.layers[4] == layer);
        CHECK(quests.Worlds().size() == 1);
    }
}
