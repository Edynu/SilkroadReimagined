// The quests: the definition's text format, the original's quest script
// read as data (SR_QUEST_SCRIPT or SR_VSRO_DATA's Quest.sct: skipped without it), its import into the database.

#include <format>
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <map>
#include <set>
#include <thread>
#include <tuple>

#include "ServerTestSupport.h"
#include "sr/core/Jobs.h"
#include "sr/game/BetaQuests.h"
#include "sr/game/CharacterRules.h"
#include "sr/server/GameServer.h"
#include "sr/server/Store.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/game/Quest.h"
#include "sr/server/QuestScript.h"
#include "sr/server/QuestStore.h"
#include "sr/server/Quests.h"
#include "sr/game/QuestProtocol.h"
#include "sr/game/QuestView.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;

namespace {

// The coded quests of the families' own files (OriginalQuestsAsia.cpp, OriginalQuestsEurope.cpp), counted by their
// `quest` lines: added to OriginalQuests.cpp's 29.
std::size_t FamiliesCoded() {
    std::size_t n = 0;
    for (const std::string_view block : {server::CodedQuestsAsia(), server::CodedQuestsEurope(), server::CodedQuestsTowns()})
        for (std::size_t at = block.find("\nquest "); at != std::string_view::npos; at = block.find("\nquest ", at + 1)) ++n;
    return n;
}

std::filesystem::path ScriptPath() { return QuestScriptPath(); }

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

const game::QuestDef* Find(const std::vector<game::QuestDef>& all, std::string_view code) {
    for (const game::QuestDef& q : all)
        if (q.codename == code) return &q;
    return nullptr;
}

// A quest of our own with every part of the format.
game::QuestDef Sample() {
    game::QuestDef q;
    q.codename = "QNO_MY_WOLVES";
    q.id = 20001;
    q.level = 25;
    q.name = {{}, "Wolves at the \"Gate\""};
    q.rewardTitle = game::TextKey("SN_PAY_QNO_MY_WOLVES");
    q.minLevel = 20;
    q.maxLevel = 30;
    q.afterAll = {"QNO_CH_SMITH_1"};
    q.afterOne = {"QNO_A", "QNO_B"};
    q.race = "china";
    q.jobs = {"hunter"};
    q.hwan = 3;
    q.time = "night";
    q.quota = 20;
    q.realClock = true;
    q.timerSeconds = 120;
    q.repeat = 3;
    q.giveUpLocks = true;
    q.givers = {"NPC_CH_SMITH"};
    q.npcs = {"NPC_CH_SMITH", "NPC_CH_POTION"};
    q.finisher = "NPC_CH_POTION";
    q.startItems = {{"ITEM_MY_LETTER", 1}};
    game::QuestGoal kill;
    kill.kind = game::QuestGoalKind::Kill;
    kill.count = 50;
    kill.monsters = {"MOB_CH_WOLF", "MOB_CH_WOLF_CLON"};
    kill.classes = {game::MonsterClass::Normal, game::MonsterClass::Champion};
    kill.text = {{}, "Wolves: line two\nand three"};
    game::QuestGoal drop;
    drop.kind = game::QuestGoalKind::Drop;
    drop.count = 20;
    drop.item = "ITEM_MY_FANG";
    drop.sources = {{"MOB_CH_WOLF", 25}, {"MOB_CH_WOLF_CLON", 1.3}};
    drop.npc = "NPC_CH_POTION";
    drop.hidden = true;
    game::QuestGoal deliver;
    deliver.kind = game::QuestGoalKind::Deliver;
    deliver.npc = "NPC_CH_POTION";
    deliver.items = {{"ITEM_MY_LETTER", 1}, {"ITEM_MY_FANG", 20}};
    deliver.talk = game::TextKey("SN_TALK_X");
    game::QuestGoal talk;
    talk.kind = game::QuestGoalKind::Talk;
    talk.npc = "NPC_CH_SMITH";
    talk.talk = {{}, "Thank you."};
    game::QuestGoal pvp;
    pvp.kind = game::QuestGoalKind::KillPlayer;
    pvp.count = 3;
    pvp.minLevel = 105;
    pvp.region = -32752;
    pvp.jobs = {"trader", "thief"};
    pvp.npc = "NPC_SD_GUILD_AGENT";
    game::QuestGoal collect;
    collect.kind = game::QuestGoalKind::Collect;
    collect.theme = "THEME_GOD_FLAME_BURNING_ABYSS";
    collect.npc = "NPC_KT_MINISTER";
    q.goals = {kill, drop, deliver, talk, pvp, collect};
    q.reward.exp = 50000;
    q.reward.skillExp = 2000;
    q.reward.sp = 7;
    q.reward.gold = 10000;
    q.reward.inventory = 4;
    q.reward.hwan = 2;
    q.reward.items = {{"ITEM_ETC_HP_POTION_03", 20}, {"ITEM_CH_SWORD_01_A", 1}};
    q.reward.choose = 1;
    q.reward.byCountry = true;
    q.reward.shown = false;
    q.reward.points = {{100, "TEMPLE_ANUBIS"}};
    q.talks = {{"NPC_CH_SMITH", "offer", {{}, "The wolves take our sheep..."}}, {"NPC_CH_POTION", "done", game::TextKey("SN_TALK_Y")}};
    q.divide = 5;
    q.differentString = 1;
    q.disabled = true;
    q.special = {"unique-dead"};
    return q;
}

} // namespace

TEST_CASE("Quests: the definition's text reads back the same") {
    const game::QuestDef q = Sample();
    const std::string text = game::WriteQuest(q);
    game::QuestDef back;
    std::string error;
    REQUIRE_MESSAGE(game::ParseQuest(text, back, error), error);
    CHECK(back == q);
    CHECK(game::WriteQuest(back) == text);
    // a few lines as a server owner writes them, comments and blank lines among them
    REQUIRE_MESSAGE(game::ParseQuest("# mine\nquest QNO_MY_1\nid 20002\n\nstart level 5-\ngoal kill 10 MOB_CH_TIGER   # tigers\n"
                                     "reward exp 100\nrepeat any\n",
                                     back, error),
                    error);
    CHECK(back.minLevel == 5);
    CHECK(back.maxLevel == 0);
    CHECK(back.repeat == 0);
    REQUIRE(back.goals.size() == 1);
    CHECK(back.goals[0].monsters == std::vector<std::string>{"MOB_CH_TIGER"});
}

TEST_CASE("Quests: a definition's mistakes are told with their line") {
    game::QuestDef q;
    std::string error;
    CHECK_FALSE(game::ParseQuest("id 1\n", q, error));
    CHECK(error.find("line 1") != std::string::npos);
    CHECK_FALSE(game::ParseQuest("quest Q\ngoal fly 3 MOB\n", q, error));
    CHECK(error.find("line 2") != std::string::npos);
    CHECK_FALSE(game::ParseQuest("quest Q\ngoal kill many MOB\n", q, error));
    CHECK_FALSE(game::ParseQuest("quest Q\ngoal drop 5 ITEM_X MOB 3%\n", q, error)); // no "from"
    CHECK_FALSE(game::ParseQuest("quest Q\ntalk NPC_X shouting @SN_X\n", q, error));
    CHECK_FALSE(game::ParseQuest("quest Q\nname \"open\n", q, error));
    CHECK_FALSE(game::ParseQuest("quest Q\nquest R\n", q, error));
}

TEST_CASE("Quests: the original's quest script read as data") {
    if (!std::filesystem::exists(ScriptPath())) {
        MESSAGE("SR_QUEST_SCRIPT (or SR_VSRO_DATA's Quest.sct) not here - skipped");
        return;
    }
    std::vector<game::QuestDef> quests;
    std::vector<std::string> notes;
    std::string error;
    REQUIRE_MESSAGE(server::DecodeQuestScript(ReadBytes(ScriptPath()), quests, notes, error), error);
    CHECK(quests.size() == 542);
    for (const std::string& n : notes) MESSAGE(n);
    CHECK(notes.empty());
    // Every definition reads back from its text.
    for (const game::QuestDef& q : quests) {
        game::QuestDef back;
        INFO(q.codename);
        REQUIRE_MESSAGE(game::ParseQuest(game::WriteQuest(q), back, error), error);
        CHECK(back == q);
        CHECK(!q.goals.empty() == (q.codename != "QNO_WC_FERRY4_1")); // its one quest without a goal
        CHECK(q.maxLevel == 0);
    }
    // Known quests, as the script writes them.
    const game::QuestDef* q = Find(quests, "QNO_CH_HORSE_1");
    REQUIRE(q);
    REQUIRE(q->goals.size() == 1);
    CHECK(q->goals[0].kind == game::QuestGoalKind::Kill);
    CHECK(q->goals[0].count == 30);
    CHECK(q->goals[0].monsters == std::vector<std::string>{"MOB_CH_WHITETIGER"});
    CHECK(q->goals[0].classes == std::vector{game::MonsterClass::Normal});
    q = Find(quests, "QNO_CA_GORIA_4");
    REQUIRE(q);
    CHECK(q->goals[0].kind == game::QuestGoalKind::Drop);
    CHECK(q->goals[0].item == "ITEM_QNO_CA_GORIA_4_01");
    CHECK(q->goals[0].count == 20);
    CHECK(q->goals[0].sources == std::vector<game::QuestGoal::Source>{{"MOB_CA_GOLEM", 20}});
    CHECK(q->afterAll == std::vector<std::string>{"QNO_CA_GORIA_3"});
    q = Find(quests, "QNO_CA_THIEF_6_1");
    REQUIRE(q);
    CHECK(q->goals[0].kind == game::QuestGoalKind::Deliver);
    CHECK(q->goals[0].items == std::vector<game::QuestGoal::Items>{{"ITEM_QNO_CA_THIEF_2_01", 10}});
    CHECK(q->minLevel == 21);
    CHECK(q->maxLevel == 0); // no upper bound (the original's offer test has none)
    q = Find(quests, "QNO_KT_PLUS_001");
    REQUIRE(q);
    CHECK(q->minLevel == 41);
    CHECK(q->maxLevel == 0);
    q = Find(quests, "QNO_SD_GU_008");
    REQUIRE(q);
    CHECK(q->goals[0].kind == game::QuestGoalKind::KillPlayer);
    CHECK(q->goals[0].count == 20);
    CHECK(q->goals[0].minLevel == 105);
    CHECK(q->goals[0].region == -32752);
    CHECK(q->goals[0].jobs == std::vector<std::string>{"thief"});
    CHECK(q->repeat == 0);
    q = Find(quests, "QNO_FW_BA1_001");
    REQUIRE(q);
    CHECK(q->goals[0].kind == game::QuestGoalKind::Collect);
    CHECK(q->goals[0].theme == "THEME_GOD_FLAME_BURNING_ABYSS");
    q = Find(quests, "QNO_WC_GENARAL_SP_1");
    REQUIRE(q);
    REQUIRE(q->goals.size() == 2);
    CHECK_FALSE(q->goals[0].hidden);
    CHECK(q->goals[1].hidden);
    q = Find(quests, "QNO_AM_FERRY1_2");
    REQUIRE(q);
    CHECK(q->givers == std::vector<std::string>{"NPC_AM_FERRY1"}); // the general only takes the delivery
    q = Find(quests, "QNO_CH_SMITH_1");
    REQUIRE(q);
    CHECK(q->givers == std::vector<std::string>{"NPC_CH_SMITH"});
    CHECK(q->startItems == std::vector<game::QuestGoal::Items>{{"ITEM_QNO_CH_SMITH_1", 1}});
    CHECK(q->goals[0].npc == "NPC_CH_SOLDIER_SO1");
    q = Find(quests, "QNO_KT_SOLDIER_WE2_1");
    REQUIRE(q);
    CHECK(q->time == "night");
    CHECK(q->quota == 20);
    q = Find(quests, "QNO_SD_MA_029");
    REQUIRE(q);
    CHECK(std::find(q->special.begin(), q->special.end(), "unique-dead") != q->special.end());
    q = Find(quests, "QNO_CA_TREASURE_1");
    REQUIRE(q);
    CHECK(q->goals[0].kind == game::QuestGoalKind::Talk);
    CHECK(!q->goals[0].npc.empty());
    // Every talk goal finds its NPC.
    for (const game::QuestDef& d : quests)
        for (const game::QuestGoal& g : d.goals) {
            INFO(d.codename);
            if (g.kind == game::QuestGoalKind::Talk || g.kind == game::QuestGoalKind::Deliver) CHECK(!g.npc.empty());
        }
    // The client's quests: those not in the script are the ones the original's server codes itself.
    const char* gameDir = std::getenv("SR_GAME_DIR");
    if (gameDir == nullptr) return;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, gameDir, error), error);
    assets::QuestTable client;
    REQUIRE_MESSAGE(client.Load(fs, error), error);
    CHECK(client.Rows().size() == 712);
    int inScript = 0;
    for (const assets::QuestRow& row : client.Rows()) inScript += Find(quests, row.codename) != nullptr;
    CHECK(inScript == 528);
}

TEST_CASE("Quests: the original's coded quests read from our definitions of them") {
    std::vector<game::QuestDef> coded;
    std::string error;
    REQUIRE_MESSAGE(server::DecodeCodedQuests(coded, error), error);
    std::map<std::string, const game::QuestDef*> byCode;
    for (const game::QuestDef& q : coded) byCode[q.codename] = &q;
    REQUIRE(byCode.contains("QSP_CH_EXINVENTORY_1"));
    const game::QuestDef& jangan = *byCode["QSP_CH_EXINVENTORY_1"];
    CHECK(jangan.minLevel == 5);
    CHECK(jangan.race == "china");
    REQUIRE(jangan.goals.size() == 1);
    CHECK(jangan.goals[0].kind == game::QuestGoalKind::Drop);
    CHECK(jangan.goals[0].count == 10);
    CHECK(jangan.goals[0].item == "ITEM_QSP_CH_EXINVENTORY_1_01");
    // Hotan's: after either race's second, two General's Tokens and 10,000 gold at the hand-in.
    const game::QuestDef& hotan = *byCode["QSP_KT_EXINVENTORY_3"];
    CHECK(hotan.afterOne == std::vector<std::string>{"QSP_WC_EXINVENTORY_2", "QSP_CA_EXINVENTORY_2"});
    CHECK(hotan.needItems == std::vector<game::QuestGoal::Items>{{"ITEM_ETC_LEVEL_TOKEN_03", 2}});
    CHECK(hotan.needGold == 10000);
    // The script's quests whose talk the script codes (CONVERSATION_SINGLE), written in our format: a god's crystal
    // swapped for its heart at the god's death in its temple room, the ferryman's story with no goal, Manina's offer
    // in two pages.
    CHECK(coded.size() == 29 + FamiliesCoded());
    // The premium's quest: its start, its drop from every monster by the level, its five rows.
    REQUIRE(byCode.contains("QSP_ALL_POTION_3"));
    const game::QuestDef& seeds = *byCode["QSP_ALL_POTION_3"];
    CHECK(seeds.premium);
    CHECK(seeds.repeat == 0);
    CHECK(seeds.givers.size() == 5);
    REQUIRE(seeds.goals.size() == 1);
    REQUIRE(seeds.goals[0].sources.size() == 1);
    CHECK(seeds.goals[0].sources[0].monster == "any");
    CHECK(seeds.goals[0].sources[0].byLevel);
    REQUIRE(seeds.tiers.size() == 5);
    CHECK(seeds.tiers[0].count == 100);
    CHECK(seeds.tiers[4].sp == 10000);
    CHECK(game::QuestView::TalksOf(seeds, "NPC_KT_POTION", "missing").size() == 1);
    REQUIRE(byCode.contains("QNO_SD_MA_029"));
    const game::QuestDef& selket = *byCode["QNO_SD_MA_029"];
    CHECK(selket.special.empty());
    CHECK(selket.startItems == std::vector<game::QuestGoal::Items>{{"ITEM_QNO_SD_MA_029_1", 1}});
    REQUIRE(selket.onUniqueDead.size() == 1);
    CHECK(selket.onUniqueDead[0].monster == "MOB_SD_SELKIS");
    CHECK(selket.onUniqueDead[0].region == std::optional<std::uint16_t>(std::uint16_t(-32758)));
    CHECK(selket.onUniqueDead[0].take == "ITEM_QNO_SD_MA_029_1");
    CHECK(selket.onUniqueDead[0].give == "ITEM_QNO_SD_MA_029_2");
    REQUIRE(byCode.contains("QNO_SD_MA_034"));
    CHECK(byCode["QNO_SD_MA_034"]->onUniqueDead[0].region == std::optional<std::uint16_t>(std::uint16_t(-32753)));
    REQUIRE(byCode.contains("QNO_WC_FERRY4_1"));
    const game::QuestDef& story = *byCode["QNO_WC_FERRY4_1"];
    CHECK(story.goals.empty());
    CHECK(game::QuestView::TalksOf(story, "NPC_WC_FERRY4", "accepted").size() == 4);
    CHECK(game::QuestView::TalksOf(story, "NPC_WC_FERRY4", "finished").size() == 1);
    REQUIRE(byCode.contains("QNO_KT_POTION_2"));
    CHECK(game::QuestView::TalksOf(*byCode["QNO_KT_POTION_2"], "NPC_KT_POTION", "offer").size() == 2);
    REQUIRE(byCode.contains("QNO_WC_FERRY3_1"));
    CHECK(byCode["QNO_WC_FERRY3_1"]->finisher == "NPC_WC_SMITH");
    // The Roc's key chain and prizes: the seven Guardrokies in a party, each finish clearing the one
    // before; a prize offered while its item is held, without a limit, its items kept when given up.
    REQUIRE(byCode.contains("QNO_RM_MEETROC_6"));
    const game::QuestDef& key = *byCode["QNO_RM_MEETROC_6"];
    CHECK(key.party);
    CHECK(key.minLevel == 80);
    CHECK(key.goals.size() == 7);
    CHECK(key.finishResets == std::vector<std::string>{"QNO_RM_MEETROC_2"});
    CHECK(key.payTake.size() == 2);
    REQUIRE(byCode.contains("QNO_RM_MEETROC_2"));
    CHECK(byCode["QNO_RM_MEETROC_2"]->finishResets == std::vector<std::string>{"QNO_RM_MEETROC_1"});
    REQUIRE(byCode.contains("QNO_RM_MEETROC_1"));
    CHECK(byCode["QNO_RM_MEETROC_1"]->goals.size() == 3);
    for (const char* code : {"QNO_RM_MEETROC_ECT_1", "QNO_RM_MEETROC_ECT_2", "QNO_RM_MEETROC_ECT_3", "QNO_RM_ARCHEMY_1", "QNO_RM_ARCHEMY_8"}) {
        REQUIRE(byCode.contains(code));
        const game::QuestDef& prize = *byCode[code];
        CHECK(prize.repeat == 0);
        CHECK(prize.giveUpKeepsItems);
        REQUIRE(prize.startHolding.size() == 1);
        REQUIRE(prize.goals.size() == 1);
        CHECK(prize.goals[0].item == prize.startHolding[0]);
        CHECK(prize.goals[0].sources.empty()); // counted in the bag, from wherever
    }
    CHECK(byCode["QNO_RM_MEETROC_ECT_3"]->goals[0].count == 15);
    CHECK(byCode["QNO_RM_ARCHEMY_4"]->givers == std::vector<std::string>{"NPC_KT_MANAGER"});
    // Every one reads back the same from its text.
    for (const game::QuestDef& q : coded) {
        game::QuestDef back;
        REQUIRE_MESSAGE(game::ParseQuest(game::WriteQuest(q), back, error), error);
        CHECK(back == q);
    }
}

TEST_CASE("Quests: imported into the database with the original's numbers and rewards") {
    TestDatabase db;
    SKIP_WITHOUT_DATABASE(db);
    std::string error;
    // A small _RefQuest and its rewards, as the original's import makes them.
    REQUIRE(db.Db()
                .Script("CREATE TABLE _refquest (service smallint, id integer, codename varchar(128), level smallint, descname varchar(128), "
                        "namestring varchar(128), paystring varchar(128), contentsstring varchar(128), paycontents varchar(128), "
                        "noticenpc varchar(128), noticecondition varchar(128));"
                        "CREATE TABLE _refquestreward (service smallint, questid integer, questcodename varchar(128), isview smallint, "
                        "ischeckcountry smallint, gold bigint, exp bigint, spexp bigint, sp integer, ap integer, aptype varchar(128), "
                        "hwan integer, inventory integer, selectioncnt integer);"
                        "CREATE TABLE _refquestrewarditems (service smallint, questid integer, questcodename varchar(128), rewardtype smallint, "
                        "itemcodename varchar(128), optionalitemcode varchar(128), optionalitemcnt integer, achievequantity integer);"
                        "INSERT INTO _refquest VALUES (1, 20, 'QNO_CH_HORSE_1', 16, '', 'SN_QNO_CH_HORSE_1', 'SN_PAY_QNO_CH_HORSE_1', 'xxx', "
                        "'SN_PAYCON_QNO_CH_HORSE_1', 'xxx', 'xxx');"
                        "INSERT INTO _refquestreward VALUES (1, 20, 'QNO_CH_HORSE_1', 1, 0, 4300, 20020, 7, 3, 0, 'xxx', 0, 0, 0);"
                        "INSERT INTO _refquestrewarditems (service, questid, questcodename, rewardtype, itemcodename, achievequantity) "
                        "VALUES (1, 20, 'QNO_CH_HORSE_1', 0, 'ITEM_ETC_HP_POTION_02', 25);")
                .Ok());
    // Our own quest is kept by the import.
    game::QuestDef mine = Sample();
    mine.disabled = false;
    REQUIRE_MESSAGE(server::SaveQuest(db.Db(), mine, server::QuestOrigin::Ours, error), error);
    if (std::filesystem::exists(ScriptPath())) {
        server::QuestImportReport report;
        REQUIRE_MESSAGE(server::ImportQuestScript(db.Db(), ReadBytes(ScriptPath()), report, error), error);
        CHECK(report.decoded == 542);
        CHECK(report.coded == 29 + FamiliesCoded());
        CHECK(report.replaced == 9); // the script's quests whose talk the script codes, written in our format
        CHECK(report.imported == 1);
        CHECK(report.unknown == 561 + FamiliesCoded()); // the script's 541, the 20 coded only in the game server and the families' this small _RefQuest lacks
        CHECK(server::HasScriptQuests(db.Db()));
    }
    std::vector<server::StoredQuest> stored;
    std::vector<std::string> problems;
    REQUIRE_MESSAGE(server::LoadQuests(db.Db(), stored, problems, error), error);
    CHECK(problems.empty());
    REQUIRE(stored.size() == (std::filesystem::exists(ScriptPath()) ? 2u : 1u));
    for (const server::StoredQuest& s : stored) {
        if (s.def.codename == mine.codename) {
            CHECK(s.origin == server::QuestOrigin::Ours);
            CHECK(s.def == mine);
            continue;
        }
        CHECK(s.origin == server::QuestOrigin::Script);
        CHECK(s.def.id == 20);
        CHECK(s.def.level == 16);
        CHECK(s.def.name == game::TextKey("SN_QNO_CH_HORSE_1"));
        CHECK(s.def.guideNpc.Empty());
        CHECK(s.def.reward.exp == 20020);
        CHECK(s.def.reward.skillExp == 7);
        CHECK(s.def.reward.sp == 3);
        CHECK(s.def.reward.gold == 4300);
        CHECK(s.def.reward.items == std::vector<game::QuestReward::Item>{{"ITEM_ETC_HP_POTION_02", 25}});
        CHECK(s.def.goals.size() == 1);
    }
    // A definition that no longer reads is named, the others still load.
    REQUIRE(db.Db().Query("INSERT INTO sr_quests (codename, id, origin, definition) VALUES ('QNO_BROKEN', 30000, 1, 'quest QNO_BROKEN\ngoal fly')").Ok());
    REQUIRE_MESSAGE(server::LoadQuests(db.Db(), stored, problems, error), error);
    CHECK(problems.size() == 1);
}

namespace {

// The quests' host for the rules: one player's bag by codename, NPCs near or not, what was sent and saved.
struct RulesHost : server::QuestHost {
    QuestHost::Player who{1, 77, "Hero", 20, "china", 0, true};
    std::map<std::string, std::uint32_t> bag;
    std::set<std::uint32_t> near;            // NPC entities within reach
    std::map<std::uint32_t, std::string> npcs; // entity -> codename
    std::vector<std::pair<proto::MessageId, std::vector<std::byte>>> sent;
    std::vector<server::QuestRecord> saved;
    std::vector<std::string> dropped;
    Reward rewarded;
    int hour = 12;
    std::set<std::string> noRoom; // items the bag has no room for
    std::vector<std::string> bound; // what QuestGiveBound gave
    bool QuestGiveBound(std::uint32_t player, const std::vector<game::QuestGoal::Items>& items) override {
        for (const auto& i : items) bound.push_back(i.item);
        return QuestGive(player, items);
    }
    std::vector<std::string> spawned; // what QuestSpawn raised
    std::optional<std::size_t> QuestSpawn(std::uint32_t, const std::string& codename, const proto::WorldPosition&) override {
        spawned.push_back(codename);
        return spawned.size();
    }
    std::optional<Player> QuestPlayer(std::uint32_t entity) override { return entity == 1 ? std::optional(who) : std::nullopt; }
    void QuestSend(std::uint64_t, proto::MessageId id, const std::vector<std::byte>& payload) override { sent.emplace_back(id, payload); }
    std::string QuestNpcNear(std::uint32_t, std::uint32_t npc) override { return near.contains(npc) ? npcs[npc] : std::string(); }
    int QuestHour() override { return hour; }
    std::uint32_t QuestItemCount(std::uint32_t, const std::string& item) override { return bag.contains(item) ? bag[item] : 0; }
    bool QuestGive(std::uint32_t, const std::vector<game::QuestGoal::Items>& items) override {
        for (const auto& i : items)
            if (noRoom.contains(i.item)) return false;
        for (const auto& i : items) bag[i.item] += i.count;
        return true;
    }
    bool QuestTake(std::uint32_t, const std::vector<game::QuestGoal::Items>& items) override {
        for (const auto& i : items)
            if (QuestItemCount(1, i.item) < i.count) return false;
        for (const auto& i : items) bag[i.item] -= i.count;
        return true;
    }
    bool QuestRoom(std::uint32_t, const std::vector<game::QuestGoal::Items>& items) override {
        return std::none_of(items.begin(), items.end(), [&](const auto& i) { return noRoom.contains(i.item); });
    }
    bool QuestTakeGold(std::uint32_t, std::uint64_t gold) override {
        if (who.gold < gold) return false;
        who.gold -= gold;
        return true;
    }
    void QuestReward(std::uint32_t, const Reward& r) override { rewarded = r; }
    void QuestDrop(std::uint32_t, std::uint32_t, const std::string& item) override { dropped.push_back(item); }
    void QuestSave(std::int64_t, const server::QuestRecord& r) override { saved.push_back(r); }
    bool QuestThemeDone(std::uint32_t, const std::string& theme) override { return themesDone.contains(theme); }
    std::set<std::string> themesDone;

    // The last message of T's id sent (none: an empty one and a failed check).
    template <typename T>
    T Last() {
        for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
            if (it->first != T::kId) continue;
            T m;
            REQUIRE(proto::Decode(it->second, m));
            return m;
        }
        FAIL("not sent: " << T::kId);
        return {};
    }
    template <typename T>
    int Count() const {
        return int(std::count_if(sent.begin(), sent.end(), [](const auto& s) { return s.first == T::kId; }));
    }
};

game::QuestDef Parse(std::string_view text) {
    game::QuestDef q;
    std::string error;
    REQUIRE_MESSAGE(game::ParseQuest(text, q, error), error);
    return q;
}

template <typename T>
void Ask(server::Quests& quests, const T& message) {
    REQUIRE(quests.Handle(1, T::kId, proto::Encode(message)));
}

constexpr std::uint32_t kGiver = 501, kFinisher = 502, kFar = 503;

} // namespace

TEST_CASE("Quests: the premium's purification seeds: offered with the premium, dropped by every monster, exchanged by rows") {
    // The level scale: 1 within -3..+10, 0.98 at +11, 0.9 at -4..-6, 1 - 0.15 x (gap - 3) from -7,
    // never under 0.1.
    CHECK(game::QuestLevelScale(50, 50) == doctest::Approx(1.0));
    CHECK(game::QuestLevelScale(50, 60) == doctest::Approx(1.0));
    CHECK(game::QuestLevelScale(50, 61) == doctest::Approx(0.98));
    CHECK(game::QuestLevelScale(50, 47) == doctest::Approx(1.0));
    CHECK(game::QuestLevelScale(50, 46) == doctest::Approx(0.9));
    CHECK(game::QuestLevelScale(50, 43) == doctest::Approx(0.4));
    CHECK(game::QuestLevelScale(50, 10) == doctest::Approx(0.1));
    CHECK(game::QuestLevelScale(1, 100) == doctest::Approx(0.1));

    struct PremiumHost : RulesHost {
        int spent = 0;
        void QuestPremiumSpent(std::uint32_t, const std::string& quest) override {
            CHECK(quest == "QSP_ALL_POTION_3");
            ++spent;
            --who.premiumQuests[quest];
        }
    } host;
    constexpr std::uint32_t kMerchant = 601;
    host.npcs = {{kMerchant, "NPC_CH_POTION"}};
    host.near = {kMerchant};
    server::Quests quests(host, {1.0, 2.0});
    std::vector<game::QuestDef> coded;
    std::string error;
    REQUIRE_MESSAGE(server::DecodeCodedQuests(coded, error), error);
    std::vector<game::QuestDef> defs;
    for (game::QuestDef& q : coded)
        if (q.codename == "QSP_ALL_POTION_3") defs.push_back(q);
    REQUIRE(defs.size() == 1);
    defs[0].id = 224;
    quests.Load(defs);
    quests.Enter(1, {});

    // No premium: not offered.
    quests.TellNpc(1, kMerchant);
    CHECK(host.Last<proto::NpcQuests>().rows.empty());
    Ask(quests, proto::QuestAccept{kMerchant, 224});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::NotOffered);
    // The premium's three uses: offered and taken.
    host.who.premiumQuests["QSP_ALL_POTION_3"] = 3;
    quests.TellNpc(1, kMerchant);
    REQUIRE(host.Last<proto::NpcQuests>().rows.size() == 1);
    Ask(quests, proto::QuestAccept{kMerchant, 224});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);

    // Every monster drops a seed at 5 % (the made-up host knows no monster's level: the scale is 1).
    const std::uint32_t me = 1;
    for (int k = 0; k < 4000; ++k) quests.Killed(9000, "MOB_ANYTHING", 0, me);
    const auto seeds = std::count(host.dropped.begin(), host.dropped.end(), std::string("ITEM_QSP_ALL_POTION_3_01"));
    CHECK(seeds > 120);
    CHECK(seeds < 300);

    // 99 held: not done yet; 250: done (the smallest row is 100), the merchant shows the rows.
    host.bag["ITEM_QSP_ALL_POTION_3_01"] = 99;
    quests.BagChanged(1);
    CHECK(quests.Record(1, 224)->status == proto::QuestStatus::Achieving);
    host.bag["ITEM_QSP_ALL_POTION_3_01"] = 250;
    quests.BagChanged(1);
    CHECK(quests.Record(1, 224)->status == proto::QuestStatus::Achieved);
    Ask(quests, proto::QuestTalk{kMerchant, 224});
    CHECK(host.Last<proto::QuestSays>().moment == "done");
    CHECK(host.Last<proto::QuestSays>().choice == proto::QuestChoice::HandIn);
    // The 300 row with 250 held: the merchant's missing line, nothing taken.
    Ask(quests, proto::QuestHandIn{kMerchant, 224, {2}});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::NotDone);
    CHECK(host.Last<proto::QuestSays>().moment == "missing");
    CHECK(host.bag["ITEM_QSP_ALL_POTION_3_01"] == 250);
    // The 200 row: 200 taken, 2,800 SP x the rate 2, the premium's use spent, the quest done once.
    Ask(quests, proto::QuestHandIn{kMerchant, 224, {1}});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    CHECK(host.bag["ITEM_QSP_ALL_POTION_3_01"] == 50);
    CHECK(host.rewarded.sp == 5600);
    CHECK(host.spent == 1);
    CHECK(quests.Record(1, 224)->status == proto::QuestStatus::Done);
    CHECK(quests.Record(1, 224)->times == 1);
    // Offered again while the premium has uses left; not once they are spent.
    quests.TellNpc(1, kMerchant);
    CHECK(host.Last<proto::NpcQuests>().rows.size() == 1);
    host.who.premiumQuests["QSP_ALL_POTION_3"] = 0;
    quests.TellNpc(1, kMerchant);
    CHECK(host.Last<proto::NpcQuests>().rows.empty());
}

TEST_CASE("Quests: a reward by country gives the player's country's items only; a pick of the other's refused (a made-up host)") {
    RulesHost host;
    host.npcs = {{kGiver, "NPC_GIVER"}};
    host.near = {kGiver};
    server::Quests quests(host, {1.0, 1.0});
    std::vector<game::QuestDef> defs;
    defs.push_back(Parse("quest QNO_T_RINGS\nid 20050\nstart level 1-\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal kill 1 MOB_WOLF\n"
                         "reward item ITEM_CH_RING_T 1\nreward item ITEM_EU_RING_T 1\nreward item ITEM_ETC_T 1\nreward by-country\n"));
    defs.push_back(Parse("quest QNO_T_PICK\nid 20051\nstart level 1-\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal kill 1 MOB_WOLF\n"
                         "reward item ITEM_CH_RING_T 1\nreward item ITEM_EU_RING_T 1\nreward choose 1\nreward by-country\n"));
    quests.Load(defs);
    quests.Enter(1, {});
    const std::uint32_t me = 1;
    // A China player (the host's): the China ring and the item of both countries, not the Europe one.
    Ask(quests, proto::QuestAccept{kGiver, 20050});
    quests.Killed(9000, "MOB_WOLF", 0, me);
    Ask(quests, proto::QuestHandIn{kGiver, 20050, {}});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    CHECK(host.bag["ITEM_CH_RING_T"] == 1);
    CHECK(host.bag["ITEM_ETC_T"] == 1);
    CHECK(host.bag["ITEM_EU_RING_T"] == 0);
    // A choice: the Europe ring refused, the China one given.
    Ask(quests, proto::QuestAccept{kGiver, 20051});
    quests.Killed(9000, "MOB_WOLF", 0, me);
    Ask(quests, proto::QuestHandIn{kGiver, 20051, {1}});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::BadChoice);
    CHECK(host.bag["ITEM_EU_RING_T"] == 0);
    Ask(quests, proto::QuestHandIn{kGiver, 20051, {0}});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    CHECK(host.bag["ITEM_CH_RING_T"] == 2);
}

TEST_CASE("Quests: offered, counted, handed in and rewarded by their rules (a made-up host)") {
    RulesHost host;
    host.npcs = {{kGiver, "NPC_GIVER"}, {kFinisher, "NPC_FINISHER"}, {kFar, "NPC_GIVER"}};
    host.near = {kGiver, kFinisher};
    server::Quests quests(host, {2.0, 3.0});
    std::vector<game::QuestDef> defs;
    defs.push_back(Parse("quest QNO_T_WOLVES\nid 20001\nstart level 15-25\nrepeat 2\ngiver NPC_GIVER\nfinish NPC_FINISHER\n"
                         "start-item ITEM_T_LETTER 1\nneed item ITEM_T_TOKEN 2\nneed gold 100\n"
                         "goal kill 2 MOB_WOLF MOB_WOLF_CLON class normal\n"
                         "goal drop 3 ITEM_T_FANG from MOB_WOLF 100% MOB_BEAR 0%\n"
                         "goal deliver NPC_FINISHER ITEM_T_LETTER 1\n"
                         "reward exp 1000\nreward skill-exp 100\nreward sp 5\nreward gold 50\nreward item ITEM_T_POTION 10\nreward inventory 4\n"));
    defs.push_back(Parse("quest QNO_T_AFTER\nid 20002\nstart level 1-\nstart after all QNO_T_WOLVES\ngiver NPC_GIVER\nfinish NPC_GIVER\n"
                         "goal talk NPC_FINISHER\nreward item ITEM_T_A 1\nreward item ITEM_T_B 1\nreward choose 1\n"));
    defs.push_back(Parse("quest QNO_T_NIGHT\nid 20003\nstart time night\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal kill 1 MOB_WOLF\n"));
    defs.push_back(Parse("quest QNO_T_TIMED\nid 20004\ntimer 1\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal kill 1 MOB_WOLF\n"));
    defs.push_back(Parse("quest QNO_T_JOB\nid 20005\nstart job trader\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal kill 1 MOB_WOLF\n"));
    defs.push_back(Parse("quest QNO_T_LOCKS\nid 20006\ngive-up locks\ngiver NPC_FAR\nfinish NPC_FAR\ngoal kill 1 MOB_WOLF\n"));
    const server::Quests::Loaded loaded = quests.Load(defs);
    CHECK(loaded.offered == 6);
    CHECK(loaded.waiting == 0); // the job's too: offered to a player in its job's suit (the offer test)
    quests.Enter(1, {});
    CHECK(host.Last<proto::QuestList>().active.empty());

    // The giver's rows: the first quest only (the second needs the first done, the night's needs the night, the job's a
    // trader's suit).
    quests.TellNpc(1, kGiver);
    auto rows = host.Last<proto::NpcQuests>().rows;
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].quest == 20001);
    CHECK(rows[0].kind == proto::NpcQuestKind::Offer);
    CHECK(rows[1].quest == 20004);
    host.hour = 22;
    quests.TellNpc(1, kGiver);
    CHECK(host.Last<proto::NpcQuests>().rows.size() == 3);
    host.hour = 12;
    // Its levels: too low, then within them.
    host.who.level = 14;
    Ask(quests, proto::QuestAccept{kGiver, 20001});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::NotOffered);
    host.who.level = 20;
    Ask(quests, proto::QuestAccept{kFar, 20001});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::TooFar);
    Ask(quests, proto::QuestTalk{kGiver, 20001});
    CHECK(host.Last<proto::QuestSays>().moment == "offer");
    CHECK(host.Last<proto::QuestSays>().choice == proto::QuestChoice::Accept);
    const int defined = host.Count<proto::QuestDefinition>(); // sent with the NPC's rows already
    CHECK(defined == 3);
    Ask(quests, proto::QuestAccept{kGiver, 20001});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    CHECK(host.Last<proto::QuestSays>().moment == "accepted");
    CHECK(host.bag["ITEM_T_LETTER"] == 1); // the start item
    CHECK(host.Count<proto::QuestDefinition>() == defined); // once a session
    REQUIRE(quests.Record(1, 20001));
    CHECK(quests.Record(1, 20001)->status == proto::QuestStatus::Achieving);

    // Kills: the class counts (a champion not), another's monster not.
    const std::uint32_t me = 1;
    quests.Killed(9000, "MOB_WOLF", 1, me); // a champion: no kill counted, its fang dropped (a drop has no class)
    CHECK(quests.Record(1, 20001)->progress[0] == 0);
    CHECK(host.dropped.size() == 1);
    quests.Killed(9001, "MOB_BEAR", 0, me);
    CHECK(quests.Record(1, 20001)->progress[0] == 0);
    CHECK(host.dropped.size() == 1); // the bear's chance is 0
    quests.Killed(9002, "MOB_WOLF_CLON", 0x10, me); // a party monster of the normal class
    quests.Killed(9003, "MOB_WOLF", 0, me);
    quests.Killed(9004, "MOB_WOLF", 0, me);
    CHECK(quests.Record(1, 20001)->progress[0] == 2); // no more than the goal
    CHECK(host.dropped.size() == 3);                  // the wolves' fangs (the clone drops none)
    CHECK(host.Last<proto::QuestChanged>().state.progress[0] == 2);
    // The fangs picked up: counted from the bag.
    host.bag["ITEM_T_FANG"] = 3;
    quests.BagChanged(1);
    CHECK(quests.Record(1, 20001)->progress[1] == 3);
    CHECK(quests.Record(1, 20001)->status == proto::QuestStatus::Achieving); // the letter not handed over yet
    quests.TellNpc(1, kFinisher);
    rows = host.Last<proto::NpcQuests>().rows;
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].kind == proto::NpcQuestKind::HandIn);
    // The delivery at the finisher, then the quest finished there: items taken, the reward by the rates.
    Ask(quests, proto::QuestTalk{kFinisher, 20001});
    CHECK(host.Last<proto::QuestSays>().choice == proto::QuestChoice::HandIn);
    // Short of what it needs besides (two tokens, 100 gold): the letter handed over, the finisher says so, nothing taken.
    host.who.gold = 50;
    Ask(quests, proto::QuestHandIn{kFinisher, 20001, {}});
    CHECK(host.Last<proto::QuestSays>().moment == "missing");
    CHECK(host.bag["ITEM_T_FANG"] == 3);
    CHECK(host.who.gold == 50);
    host.bag["ITEM_T_TOKEN"] = 2;
    host.who.gold = 150;
    host.noRoom = {"ITEM_T_POTION"};
    Ask(quests, proto::QuestHandIn{kFinisher, 20001, {}});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::BagFull);
    CHECK(host.bag["ITEM_T_FANG"] == 3); // given back
    CHECK(host.bag["ITEM_T_TOKEN"] == 2);
    CHECK(host.who.gold == 150);
    CHECK(host.bag["ITEM_T_LETTER"] == 0); // handed over, and kept so
    CHECK(quests.Record(1, 20001)->status == proto::QuestStatus::Achieved);
    CHECK(host.saved.back().progress[2] == 1);
    host.noRoom.clear();
    Ask(quests, proto::QuestHandIn{kFinisher, 20001, {}});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::Finished);
    CHECK(host.bag["ITEM_T_LETTER"] == 0);
    CHECK(host.bag["ITEM_T_FANG"] == 0);
    CHECK(host.bag["ITEM_T_TOKEN"] == 0); // the needed taken
    CHECK(host.who.gold == 50);
    CHECK(host.bag["ITEM_T_POTION"] == 10);
    CHECK(host.rewarded.exp == 2000);    // rate-exp 2
    CHECK(host.rewarded.skillExp == 300); // rate-sp 3
    CHECK(host.rewarded.sp == 15);
    CHECK(host.rewarded.gold == 50);
    CHECK(host.rewarded.inventory == 4);
    CHECK(quests.Record(1, 20001)->status == proto::QuestStatus::Done);
    CHECK(quests.Record(1, 20001)->times == 1);
    CHECK(host.saved.back().times == 1);

    // Once more (it may be done twice), then no more.
    Ask(quests, proto::QuestTalk{kGiver, 20001});
    CHECK(host.Last<proto::QuestSays>().moment == "offer-again");
    Ask(quests, proto::QuestAccept{kGiver, 20001});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    CHECK(quests.Record(1, 20001)->progress[0] == 0);
    // Given up: its quest items back to nothing, and it may be taken again.
    host.bag["ITEM_T_FANG"] = 2;
    Ask(quests, proto::QuestAbandon{20001});
    CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::Abandoned);
    CHECK(host.bag["ITEM_T_LETTER"] == 0);
    CHECK(host.bag["ITEM_T_FANG"] == 0);
    CHECK(quests.Record(1, 20001)->status == proto::QuestStatus::Abandoned);
    CHECK(quests.Offered(1, *quests.Find(20001))); // it may be taken again

    // The next one, open now the first is done: a talk elsewhere, then a reward chosen.
    Ask(quests, proto::QuestAccept{kGiver, 20002});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    Ask(quests, proto::QuestTalk{kFinisher, 20002});
    CHECK(host.Last<proto::QuestSays>().choice == proto::QuestChoice::HandIn);
    Ask(quests, proto::QuestHandIn{kFinisher, 20002, {}});
    CHECK(quests.Record(1, 20002)->status == proto::QuestStatus::Achieved);
    Ask(quests, proto::QuestTalk{kGiver, 20002});
    CHECK(host.Last<proto::QuestSays>().moment == "done");
    Ask(quests, proto::QuestHandIn{kGiver, 20002, {0, 1}});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::BadChoice);
    Ask(quests, proto::QuestHandIn{kGiver, 20002, {1}});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    CHECK(host.bag["ITEM_T_B"] == 1);
    CHECK(host.bag["ITEM_T_A"] == 0);

    // One that locks, given up, is not offered again.
    host.npcs[504] = "NPC_FAR";
    host.near.insert(504);
    Ask(quests, proto::QuestAccept{504, 20006});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    Ask(quests, proto::QuestAbandon{20006});
    CHECK_FALSE(quests.Offered(1, *quests.Find(20006)));
    Ask(quests, proto::QuestAccept{504, 20006});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::NotOffered);

    // A timer runs out: the quest is lost.
    Ask(quests, proto::QuestAccept{kGiver, 20004});
    CHECK(quests.Record(1, 20004)->status == proto::QuestStatus::Achieving);
    CHECK(host.Last<proto::QuestChanged>().state.secondsLeft >= 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    quests.Tick(server::Quests::Clock::now());
    CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::TimeOut);

    // Kept and back: the list carries what is under way and what was finished.
    const std::vector<server::QuestRecord> kept = {*quests.Record(1, 20001), *quests.Record(1, 20002)};
    quests.Leave(1);
    quests.Enter(1, kept);
    const proto::QuestList list = host.Last<proto::QuestList>();
    CHECK(list.active.empty());
    REQUIRE(list.finished.size() == 2);
    // The progress packed as _CharQuest keeps it.
    std::array<std::uint16_t, proto::kQuestGoals> progress{1, 2, 3, 4, 5, 6, 7, 65535};
    const auto [d1, d2] = server::PackQuestProgress(progress);
    CHECK(server::UnpackQuestProgress(d1, d2) == progress);
    // In a trader's suit the job's quest is offered too; in a thief's not.
    host.who.job = "trader";
    quests.TellNpc(1, kGiver);
    rows = host.Last<proto::NpcQuests>().rows;
    CHECK(std::any_of(rows.begin(), rows.end(), [](const proto::NpcQuests::Row& r) { return r.quest == 20005; }));
    host.who.job = "thief";
    quests.TellNpc(1, kGiver);
    rows = host.Last<proto::NpcQuests>().rows;
    CHECK_FALSE(std::any_of(rows.begin(), rows.end(), [](const proto::NpcQuests::Row& r) { return r.quest == 20005; }));
    host.who.job.clear();
}

namespace {

constexpr std::uint32_t kGiverModel = 2021, kFinisherModel = 2022;
constexpr std::uint32_t kLetter = 910001, kPotion = 910002;
constexpr std::uint32_t kTool = 910003, kFound = 910004, kStray = 910005; // quest items used from the bag (QuestItemUseTests' server case)

// Our game server with two NPCs by the Chinese start place and one quest of ours in the database.
struct QuestRig {
    TestDatabase& t;
    core::JobSystem jobs{2};
    std::unique_ptr<server::GameServer> game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::function<void()> pump;
    std::string error;
    explicit QuestRig(TestDatabase& db) : t(db) {
        config.listen = Local();
        config.databaseUrl = t.Url();
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
        rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
        rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
        rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
        auto items = std::make_shared<assets::ItemTable>();
        assets::ItemRow letter;
        letter.refObjId = kLetter;
        letter.codename = "ITEM_T_LETTER";
        letter.typeId = {3, 3, 9, 0};
        letter.inService = true;
        letter.maxStack = 1;
        letter.canTrade = false;
        items->Add(letter);
        assets::ItemRow potion = letter;
        potion.refObjId = kPotion;
        potion.codename = "ITEM_T_POTION";
        potion.typeId = {3, 3, 1, 1};
        potion.maxStack = 50;
        potion.canTrade = true;
        items->Add(potion);
        for (const auto& [id, code, use] : {std::tuple{kTool, "ITEM_T_TOOL", 1}, std::tuple{kFound, "ITEM_T_FOUND", 0}, std::tuple{kStray, "ITEM_T_STRAY", 1}}) {
            assets::ItemRow quest = letter;
            quest.refObjId = id;
            quest.codename = code;
            quest.canUse = use;
            quest.maxStack = 10;
            items->Add(quest);
        }
        rules->items = items;
        config.rules = rules;
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        config.rates.experience = 2.0;
        config.rates.skillExperience = 3.0;
        config.npcs.push_back({kGiverModel, "NPC_T_GIVER", {assets::RegionId(168, 98), 1000.0f, 50.0f, 458.0f}, 0.0f});
        config.npcs.push_back({kFinisherModel, "NPC_T_FINISHER", {assets::RegionId(168, 98), 1010.0f, 50.0f, 458.0f}, 0.0f});
        pump = [this] { game->Poll(2ms); };
    }
    void Start() {
        game = std::make_unique<server::GameServer>();
        REQUIRE_MESSAGE(game->Start(config, error), error);
    }
    struct Player {
        std::unique_ptr<Client> c;
        std::int64_t character = 0;
        std::uint32_t entity = 0, giver = 0, finisher = 0;
        proto::QuestList list;
        proto::InventoryList bag;
    };
    Player Join(const std::string& name) {
        Player p;
        bool ok = false;
        const std::string account = "acc" + name;
        if (!server::FindAccount(t.Db(), account, ok)) REQUIRE(server::CreateAccount(t.Db(), account, "secret1", error) == server::CreateAccountResult::Created);
        const std::int64_t id = server::FindAccount(t.Db(), account, ok)->id;
        p.c = std::make_unique<Client>(net, game->Port(), pump);
        REQUIRE(p.c->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
        p.c->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(p.c->Expect(entered));
        proto::LobbyResult r;
        if (t.Db().Query("SELECT charid FROM _char WHERE charname16 = $1", {name}).Rows() == 0) {
            proto::CharacterCreate m;
            m.name = name;
            m.model = 100;
            m.chest = 1;
            m.legs = 2;
            m.shoes = 3;
            m.weapon = 10;
            p.c->Send(m);
            REQUIRE(p.c->Expect(r));
            REQUIRE(r.status == proto::LobbyStatus::Ok);
        }
        p.character = t.Db().Query("SELECT charid FROM _char WHERE charname16 = $1", {name}).Int(0, 0);
        p.c->Send(proto::CharacterSelect{name});
        REQUIRE(p.c->Expect(r));
        p.c->Send(proto::WorldEnter{});
        proto::WorldEntered w;
        REQUIRE(p.c->Expect(w));
        REQUIRE(w.status == proto::WorldStatus::Ok);
        p.entity = w.entity;
        REQUIRE(p.c->Expect(p.bag));
        REQUIRE(p.c->Expect(p.list));
        proto::EntityAppeared seen;
        while ((p.giver == 0 || p.finisher == 0) && p.c->Expect(seen)) {
            if (seen.character.model == kGiverModel) p.giver = seen.entity;
            if (seen.character.model == kFinisherModel) p.finisher = seen.entity;
        }
        REQUIRE(p.giver != 0);
        REQUIRE(p.finisher != 0);
        return p;
    }
};

template <typename T>
T Got(QuestRig::Player& p) {
    T m;
    REQUIRE(p.c->Expect(m));
    return m;
}

} // namespace

TEST_CASE("Quests: offered while an item is held, its items kept when given up, a finish clearing another (the Roc's)") {
    RulesHost host;
    host.npcs = {{kGiver, "NPC_GIVER"}};
    host.near = {kGiver};
    host.who.level = 90;
    server::Quests quests(host, {1.0, 1.0});
    std::vector<game::QuestDef> defs;
    defs.push_back(Parse("quest QNO_T_PRIZE\nid 20301\nstart holding ITEM_T_MARK\nrepeat any\ngive-up keeps-items\ngiver NPC_GIVER\nfinish NPC_GIVER\n"
                         "goal drop 2 ITEM_T_MARK at NPC_GIVER\nreward item ITEM_T_STONE 1\n"));
    defs.push_back(Parse("quest QNO_T_FIRST\nid 20302\nrepeat 1\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal drop 1 ITEM_T_SIGN at NPC_GIVER\n"));
    defs.push_back(Parse("quest QNO_T_SECOND\nid 20303\nstart after one QNO_T_FIRST\nrepeat 1\nfinish-reset QNO_T_FIRST\ngiver NPC_GIVER\n"
                         "finish NPC_GIVER\ngoal drop 1 ITEM_T_STONE2 at NPC_GIVER\n"));
    REQUIRE(quests.Load(defs).offered == 3);
    quests.Enter(1, {});
    // No mark held: not offered; one held: offered.
    CHECK_FALSE(quests.Offered(1, *quests.Find(20301)));
    host.bag["ITEM_T_MARK"] = 1;
    CHECK(quests.Offered(1, *quests.Find(20301)));
    Ask(quests, proto::QuestAccept{kGiver, 20301});
    REQUIRE(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    // Given up: the mark stays.
    Ask(quests, proto::QuestAbandon{20301});
    CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::Abandoned);
    CHECK(host.bag["ITEM_T_MARK"] == 1);
    // Taken again, two held: met, handed in (the marks taken), and offered again while one is held.
    Ask(quests, proto::QuestAccept{kGiver, 20301});
    REQUIRE(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    host.bag["ITEM_T_MARK"] = 2;
    quests.BagChanged(1);
    CHECK(quests.Record(1, 20301)->status == proto::QuestStatus::Achieved);
    Ask(quests, proto::QuestHandIn{kGiver, 20301, {}});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    CHECK(host.bag["ITEM_T_MARK"] == 0);
    CHECK_FALSE(quests.Offered(1, *quests.Find(20301)));
    host.bag["ITEM_T_MARK"] = 1;
    CHECK(quests.Offered(1, *quests.Find(20301)));
    // The chain: the first, then the second, whose finish clears the first (offered again, the second not).
    Ask(quests, proto::QuestAccept{kGiver, 20302});
    host.bag["ITEM_T_SIGN"] = 1;
    quests.BagChanged(1);
    Ask(quests, proto::QuestHandIn{kGiver, 20302, {}});
    REQUIRE(quests.Record(1, 20302)->times == 1);
    CHECK_FALSE(quests.Offered(1, *quests.Find(20302)));
    Ask(quests, proto::QuestAccept{kGiver, 20303});
    REQUIRE(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    host.bag["ITEM_T_STONE2"] = 1;
    quests.BagChanged(1);
    Ask(quests, proto::QuestHandIn{kGiver, 20303, {}});
    CHECK(quests.Record(1, 20303)->times == 1);
    CHECK(quests.Record(1, 20302)->times == 0);
    CHECK(quests.Offered(1, *quests.Find(20302)));
    CHECK_FALSE(quests.Offered(1, *quests.Find(20303)));
    CHECK(host.saved.back().quest == 20302); // the cleared record kept
}

TEST_CASE("Quests: a temple quest's player kills counted by the victim's level, region and job; its points paid") {
    RulesHost host;
    host.npcs = {{kGiver, "NPC_GIVER"}};
    host.near = {kGiver};
    host.who.level = 110;
    server::Quests quests(host, {1.0, 1.0});
    std::vector<game::QuestDef> defs;
    defs.push_back(Parse("quest QNO_T_TEMPLE\nid 20101\ngiver NPC_GIVER\nfinish NPC_GIVER\n"
                         "goal kill-player 2 level 105 region -32752 job thief\n"
                         "reward points 3 TEMPLE_ANUBIS\n"));
    REQUIRE(quests.Load(defs).offered == 1);
    quests.Enter(1, {});
    Ask(quests, proto::QuestAccept{kGiver, 20101});
    REQUIRE(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    const std::uint16_t area = std::uint16_t(std::int16_t(-32752)); // the temples' area, 0x8010
    quests.PlayerKilled(1, 100, area, "thief");  // under the level
    quests.PlayerKilled(1, 110, 25000, "thief"); // elsewhere
    quests.PlayerKilled(1, 110, area, "trader"); // not a thief
    quests.PlayerKilled(1, 110, area, "");       // no job
    CHECK(quests.Record(1, 20101)->progress[0] == 0);
    quests.PlayerKilled(1, 110, area, "thief");
    quests.PlayerKilled(1, 105, area, "thief");
    quests.PlayerKilled(1, 106, area, "thief"); // no more than the goal
    CHECK(quests.Record(1, 20101)->progress[0] == 2);
    CHECK(quests.Record(1, 20101)->status == proto::QuestStatus::Achieved);
    Ask(quests, proto::QuestHandIn{kGiver, 20101, {}});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    REQUIRE(host.rewarded.points.size() == 1);
    CHECK(host.rewarded.points[0].amount == 3);
    CHECK(host.rewarded.points[0].name == "TEMPLE_ANUBIS");
}

TEST_CASE("Game server: a quest of ours offered at its NPC, taken, handed in, rewarded by the rates and kept") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    REQUIRE_MESSAGE(server::SaveQuest(t.Db(),
                                      Parse("quest QNO_T_LETTER\nid 20010\nname \"The Letter\"\nstart level 1-\ngiver NPC_T_GIVER\nfinish NPC_T_FINISHER\n"
                                            "start-item ITEM_T_LETTER 1\ngoal deliver NPC_T_FINISHER ITEM_T_LETTER 1\n"
                                            "reward exp 100\nreward skill-exp 400\nreward sp 3\nreward gold 50\nreward item ITEM_T_POTION 120\nreward inventory 4\n"
                                            "talk NPC_T_GIVER offer \"Take this letter to my friend.\"\n"),
                                      server::QuestOrigin::Ours, error),
                    error);
    QuestRig rig(t);
    rig.Start();
    auto p = rig.Join("Courier");
    CHECK(p.list.active.empty());
    CHECK(p.list.finished.empty());

    // The giver's row, its offer, taken: the letter in the bag.
    p.c->Send(proto::NpcTalk{p.giver});
    CHECK(Got<proto::NpcMenu>(p).entity == p.giver);
    const auto def = Got<proto::QuestDefinition>(p);
    game::QuestDef q;
    REQUIRE_MESSAGE(game::ParseQuest(def.text, q, error), error);
    CHECK(q.name.literal == "The Letter");
    auto rows = Got<proto::NpcQuests>(p);
    REQUIRE(rows.rows.size() == 1);
    CHECK(rows.rows[0].quest == 20010);
    CHECK(rows.rows[0].kind == proto::NpcQuestKind::Offer);
    p.c->Send(proto::QuestTalk{p.giver, 20010});
    CHECK(Got<proto::QuestSays>(p).moment == "offer");
    p.c->Send(proto::QuestAccept{p.giver, 20010});
    const auto letter = Got<proto::ItemMoved>(p);
    REQUIRE(letter.changed.size() == 1);
    CHECK(letter.changed[0].item == kLetter);
    CHECK(Got<proto::QuestChanged>(p).state.status == proto::QuestStatus::Achieving);
    CHECK(Got<proto::QuestAnswer>(p).code == proto::QuestCode::Ok);
    CHECK(Got<proto::QuestSays>(p).moment == "accepted");

    // At the finisher: handed in, the letter taken, the reward given (120 potions in three stacks, the rates applied).
    p.c->Send(proto::NpcTalk{p.finisher});
    CHECK(Got<proto::NpcMenu>(p).entity == p.finisher);
    rows = Got<proto::NpcQuests>(p);
    REQUIRE(rows.rows.size() == 1);
    CHECK(rows.rows[0].kind == proto::NpcQuestKind::HandIn);
    p.c->Send(proto::QuestHandIn{p.finisher, 20010, {}});
    CHECK(Got<proto::ItemMoved>(p).emptied.size() == 1); // the letter
    CHECK(Got<proto::QuestChanged>(p).state.status == proto::QuestStatus::Achieved);
    const auto potions = Got<proto::ItemMoved>(p);
    CHECK(potions.changed.size() == 3);
    CHECK(Got<proto::GoldChanged>(p).gold == 50);
    CHECK(Got<proto::InventoryGrown>(p).size == 49);
    const auto gained = Got<proto::ExperienceGained>(p);
    CHECK(gained.experience == 200);
    CHECK(gained.skillExperience == 1200);
    CHECK(Got<proto::QuestGone>(p).why == proto::QuestEnd::Finished);
    const auto rewarded = Got<proto::QuestRewarded>(p);
    CHECK(rewarded.exp == 200);
    CHECK(rewarded.sp == 9);
    CHECK(Got<proto::QuestAnswer>(p).code == proto::QuestCode::Ok);
    // Not offered again (done once, as often as it may be).
    p.c->Send(proto::NpcTalk{p.giver});
    CHECK(Got<proto::NpcQuests>(p).rows.empty());

    // Kept: _CharQuest's row, the bag's size, the potions; back in the world, the list says it was finished.
    for (int i = 0; i < 100; ++i) rig.pump();
    const server::QueryResult row = t.Db().Query("SELECT status, achievementcount FROM _charquest WHERE charid = $1 AND questid = 20010", {p.character});
    REQUIRE(row.Rows() == 1);
    CHECK(row.Int(0, 0) == 4);
    CHECK(row.Int(0, 1) == 1);
    CHECK(t.Db().Query("SELECT inventorysize FROM _char WHERE charid = $1", {p.character}).Int(0, 0) == 49);
    p.c->Send(proto::WorldLeave{});
    proto::WorldLeft left;
    REQUIRE(p.c->Expect(left));
    for (int i = 0; i < 100; ++i) rig.pump();
    CHECK(t.Db().Query("SELECT remainskillpoint FROM _char WHERE charid = $1", {p.character}).Int(0, 0) >= 9);
    p.c->Send(proto::CharacterSelect{"Courier"});
    proto::LobbyResult again;
    REQUIRE(p.c->Expect(again));
    p.c->Send(proto::WorldEnter{});
    proto::WorldEntered w;
    REQUIRE(p.c->Expect(w));
    const auto list = Got<proto::QuestList>(p);
    CHECK(list.active.empty());
    REQUIRE(list.finished.size() == 1);
    CHECK(list.finished[0].quest == 20010);
    CHECK(list.finished[0].times == 1);
    const auto bag = Got<proto::InventoryList>(p);
    CHECK(bag.size == 49);
}

TEST_CASE("Quests: a unique's death swaps the crystal held in its room for its heart; a story is told and paid in one talk") {
    RulesHost host;
    host.npcs = {{kGiver, "NPC_SENMUTE"}};
    host.near = {kGiver};
    server::Quests quests(host, {1.0, 1.0});
    std::vector<game::QuestDef> defs;
    defs.push_back(Parse("quest QNO_T_GOD\nid 20200\nstart level 1-\nrepeat 1\ngiver NPC_SENMUTE\nfinish NPC_SENMUTE\n"
                         "start-item ITEM_T_CRYSTAL 1\ngoal drop 1 ITEM_T_HEART from MOB_T_GOD 0%\n"
                         "on unique-dead MOB_T_GOD region -32758 take ITEM_T_CRYSTAL give ITEM_T_HEART\nreward exp 100\n"));
    defs.push_back(Parse("quest QNO_T_STORY\nid 20201\nstart level 1-\nrepeat 1\ngiver NPC_SENMUTE\nfinish NPC_SENMUTE\n"
                         "reward exp 650\nreward item ITEM_T_FLUTE 1\n"
                         "talk NPC_SENMUTE accepted \"Page one.\"\ntalk NPC_SENMUTE accepted \"Page two.\"\n"));
    CHECK(quests.Load(defs).offered == 2);
    quests.Enter(1, {});
    // Accepted: the crystal into the bag.
    Ask(quests, proto::QuestAccept{kGiver, 20200});
    CHECK(host.bag["ITEM_T_CRYSTAL"] == 1);
    // The god falls while the holder stands elsewhere, or another unique falls: nothing. The god in its room: the
    // crystal becomes its heart, the goal met.
    host.who.region = 25000;
    quests.UniqueDied("MOB_T_GOD");
    CHECK(host.bag["ITEM_T_CRYSTAL"] == 1);
    host.who.region = std::uint16_t(-32758);
    quests.UniqueDied("MOB_T_OTHER");
    CHECK(host.bag["ITEM_T_HEART"] == 0);
    quests.UniqueDied("MOB_T_GOD");
    CHECK(host.bag["ITEM_T_CRYSTAL"] == 0);
    CHECK(host.bag["ITEM_T_HEART"] == 1);
    CHECK(quests.Record(1, 20200)->status == proto::QuestStatus::Achieved);
    Ask(quests, proto::QuestHandIn{kGiver, 20200, {}});
    CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::Finished);
    CHECK(host.bag["ITEM_T_HEART"] == 0);

    // The story: no room for its gift refuses it; with room it is accepted (its pages told) and paid in the same talk.
    host.noRoom = {"ITEM_T_FLUTE"};
    Ask(quests, proto::QuestAccept{kGiver, 20201});
    CHECK(host.Last<proto::QuestSays>().moment == "bag-full");
    CHECK(quests.Record(1, 20201) == nullptr);
    host.noRoom.clear();
    Ask(quests, proto::QuestAccept{kGiver, 20201});
    CHECK(host.bag["ITEM_T_FLUTE"] == 1);
    CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::Finished);
    REQUIRE(quests.Record(1, 20201) != nullptr);
    CHECK(quests.Record(1, 20201)->status == proto::QuestStatus::Done);
}

TEST_CASE("Quests: a quest given by level, taken by any of its NPCs, finished there (the Beginner's Assistant)") {
    RulesHost host;
    constexpr std::uint32_t kEast = 501, kWest = 502;
    host.npcs = {{kEast, "NPC_EAST"}, {kWest, "NPC_WEST"}};
    host.near = {kEast, kWest};
    server::Quests quests(host, {1.0, 1.0});
    std::vector<game::QuestDef> defs;
    defs.push_back(Parse("quest QEV_T_GIFT\nid 20100\nstart given\nstart level 5-\nrepeat 1\nnpc NPC_EAST NPC_WEST\n"
                         "goal talk NPC_EAST NPC_WEST talk \"Here, a gift.\"\nreward item ITEM_T_GIFT 1\n"));
    CHECK(quests.Load(defs).offered == 1);
    host.who.level = 4;
    quests.Enter(1, {});
    CHECK(quests.Record(1, 20100) == nullptr);
    // Entering at a level past it gives nothing; a rise to it does, once.
    host.who.level = 5;
    quests.LevelChanged(1);
    REQUIRE(quests.Record(1, 20100) != nullptr);
    CHECK(quests.Record(1, 20100)->status == proto::QuestStatus::Achieving);
    // The west's merchant takes it as well as the east's: the talk handed in finishes it there, the gift given.
    quests.TellNpc(1, kWest);
    auto rows = host.Last<proto::NpcQuests>().rows;
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].kind == proto::NpcQuestKind::HandIn);
    Ask(quests, proto::QuestTalk{kWest, 20100});
    CHECK(host.Last<proto::QuestSays>().choice == proto::QuestChoice::HandIn);
    Ask(quests, proto::QuestHandIn{kWest, 20100, {}});
    CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::Finished);
    CHECK(host.bag["ITEM_T_GIFT"] == 1);
    CHECK(quests.Record(1, 20100)->status == proto::QuestStatus::Done);
    // Not given again.
    host.who.level = 6;
    quests.LevelChanged(1);
    CHECK(quests.Record(1, 20100)->times == 1);
}

TEST_CASE("Quests: a level-up's quest guide lists the new level's quests") {
    RulesHost host;
    server::Quests quests(host, {1.0, 1.0});
    std::vector<game::QuestDef> defs;
    defs.push_back(Parse("quest QNO_T_SAME\nid 20110\nlevel 8\nname \"Same\"\nguide-npc \"An NPC\"\nguide-condition \"Once\"\n"
                         "start race china\ngiver NPC_A\nfinish NPC_A\ngoal kill 1 MOB_WOLF\n"));
    defs.push_back(Parse("quest QNO_T_OTHER\nid 20111\nlevel 8\nstart race europe\ngiver NPC_A\nfinish NPC_A\ngoal kill 1 MOB_WOLF\n"));
    defs.push_back(Parse("quest QNO_T_BOTH\nid 20112\nlevel 8\ngiver NPC_A\nfinish NPC_A\ngoal kill 1 MOB_WOLF\n"));
    defs.push_back(Parse("quest QNO_T_LATER\nid 20113\nlevel 9\ngiver NPC_A\nfinish NPC_A\ngoal kill 1 MOB_WOLF\n"));
    defs.push_back(Parse("quest QNO_T_OFF\nid 20114\nlevel 8\ndisabled\ngiver NPC_A\nfinish NPC_A\ngoal kill 1 MOB_WOLF\n"));
    quests.Load(defs);
    host.who.level = 7;
    host.who.race = "china";
    quests.Enter(1, {});
    host.who.level = 8;
    quests.LevelChanged(1);
    // Its own race's and both races', not the other race's, a later level's or a disabled one.
    const auto rows = host.Last<proto::QuestGuide>().rows;
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].quest == 20110);
    CHECK(rows[0].name == "Same");
    CHECK(rows[0].npc == "An NPC");
    CHECK(rows[0].condition == "Once");
    CHECK(rows[1].quest == 20112);
    // Past level 40 the race no longer matters.
    defs.clear();
    defs.push_back(Parse("quest QNO_T_HIGH\nid 20120\nlevel 41\nstart race europe\ngiver NPC_A\nfinish NPC_A\ngoal kill 1 MOB_WOLF\n"));
    quests.Load(defs);
    host.who.level = 41;
    quests.LevelChanged(1);
    REQUIRE(host.Last<proto::QuestGuide>().rows.size() == 1);
}

TEST_CASE("Quests: a goal paid by the piece drops past its count and pays each piece; gold by rate-gold, its cap too") {
    RulesHost host;
    constexpr std::uint32_t kPriest = 601;
    host.npcs = {{kPriest, "NPC_PRIEST"}};
    host.near = {kPriest};
    // rate-exp 2, rate-gold 10 (the user's: at 10 a quest may pay 100,000,000).
    server::Quests quests(host, {2.0, 1.0, 10.0});
    std::vector<game::QuestDef> defs;
    defs.push_back(Parse("quest QNO_T_PIECES\nid 20130\ngiver NPC_PRIEST\nfinish NPC_PRIEST\n"
                         "goal drop 1 ITEM_T_PIECE from MOB_STONE 100% at NPC_PRIEST apiece gold 125 exp 100\nreward gold 1000\nreward exp 10\n"));
    defs.push_back(Parse("quest QNO_T_RICH\nid 20131\ngiver NPC_PRIEST\nfinish NPC_PRIEST\ngoal talk NPC_PRIEST\nreward gold 20000000\n"));
    CHECK(defs[0].goals[0].apiece);
    CHECK(defs[0].goals[0].apieceGold == 125);
    CHECK(game::WriteQuest(defs[0]).find("apiece gold 125 exp 100") != std::string::npos);
    quests.Load(defs);
    quests.Enter(1, {});
    Ask(quests, proto::QuestAccept{kPriest, 20130});
    REQUIRE(quests.Record(1, 20130) != nullptr);
    const std::uint32_t me = 1;
    quests.Killed(9000, "MOB_STONE", 0, me);
    host.bag["ITEM_T_PIECE"] = 1;
    quests.BagChanged(1);
    // Its count met, the pieces drop on.
    quests.Killed(9001, "MOB_STONE", 0, me);
    quests.Killed(9002, "MOB_STONE", 0, me);
    CHECK(host.dropped.size() == 3);
    host.bag["ITEM_T_PIECE"] = 3;
    quests.BagChanged(1);
    Ask(quests, proto::QuestTalk{kPriest, 20130});
    Ask(quests, proto::QuestHandIn{kPriest, 20130, {}});
    CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::Finished);
    CHECK(host.bag["ITEM_T_PIECE"] == 0);                      // every piece taken
    CHECK(host.rewarded.gold == (1000 + 3 * 125) * 10);         // the reward's and the pieces' gold, times rate-gold
    CHECK(host.rewarded.exp == (10 + 3 * 100) * 2);             // experience times rate-exp
    // A reward above the original's cap: 10,000,000 times rate-gold.
    Ask(quests, proto::QuestAccept{kPriest, 20131});
    Ask(quests, proto::QuestTalk{kPriest, 20131});
    Ask(quests, proto::QuestHandIn{kPriest, 20131, {}});
    CHECK(host.rewarded.gold == 100'000'000);
}

TEST_CASE("Quests: a quest finished N times first, quests never taken, a monster raised each minute") {
    RulesHost host;
    constexpr std::uint32_t kSmith = 611;
    host.npcs = {{kSmith, "NPC_SMITH"}};
    host.near = {kSmith};
    server::Quests quests(host, {1.0, 1.0});
    std::vector<game::QuestDef> defs;
    defs.push_back(Parse("quest QNO_T_FIRST\nid 20140\nrepeat 3\ngiver NPC_SMITH\nfinish NPC_SMITH\ngoal talk NPC_SMITH\n"));
    defs.push_back(Parse("quest QNO_T_SMITH\nid 20141\nstart after all QNO_T_FIRST*3\nstart never QNO_T_OTHER\ngiver NPC_SMITH\nfinish NPC_SMITH\n"
                         "goal kill 2 MOB_WING\nraise MOB_WING every 60 radius 0 20\n"));
    defs.push_back(Parse("quest QNO_T_OTHER\nid 20142\nstart after all QNO_T_FIRST\ngiver NPC_SMITH\nfinish NPC_SMITH\ngoal talk NPC_SMITH\n"));
    CHECK(defs[1].afterTimes.at("QNO_T_FIRST") == 3);
    CHECK(game::WriteQuest(defs[1]).find("start after all QNO_T_FIRST*3") != std::string::npos);
    quests.Load(defs);
    quests.Enter(1, {});
    const auto finishFirst = [&] {
        Ask(quests, proto::QuestAccept{kSmith, 20140});
        Ask(quests, proto::QuestTalk{kSmith, 20140});
        Ask(quests, proto::QuestHandIn{kSmith, 20140, {}});
    };
    const auto offered = [&](std::uint32_t id) {
        quests.TellNpc(1, kSmith);
        const auto rows = host.Last<proto::NpcQuests>().rows;
        return std::any_of(rows.begin(), rows.end(), [&](const proto::NpcQuests::Row& r) { return r.quest == id && r.kind == proto::NpcQuestKind::Offer; });
    };
    finishFirst();
    finishFirst();
    CHECK(!offered(20141)); // twice: not yet
    finishFirst();
    CHECK(offered(20141));
    CHECK(offered(20142));
    // The other taken: the smith's chain closed.
    Ask(quests, proto::QuestAccept{kSmith, 20142});
    CHECK(!offered(20141));
    // A character that took the smith's instead: its Wing Tribes raised every 60 s while it runs.
    RulesHost host2;
    host2.npcs = host.npcs;
    host2.near = host.near;
    server::Quests q2(host2, {1.0, 1.0});
    q2.Load(defs);
    q2.Enter(1, {});
    for (int i = 0; i < 3; ++i) {
        Ask(q2, proto::QuestAccept{kSmith, 20140});
        Ask(q2, proto::QuestTalk{kSmith, 20140});
        Ask(q2, proto::QuestHandIn{kSmith, 20140, {}});
    }
    Ask(q2, proto::QuestAccept{kSmith, 20141});
    REQUIRE(q2.Record(1, 20141) != nullptr);
    const auto t0 = server::Quests::Clock::now();
    q2.Tick(t0);
    CHECK(host2.spawned.empty()); // the countdown set
    q2.Tick(t0 + std::chrono::seconds(30));
    CHECK(host2.spawned.empty());
    q2.Tick(t0 + std::chrono::seconds(61));
    CHECK(host2.spawned == std::vector<std::string>{"MOB_WING"});
    host2.who.town = true; // none in a town
    q2.Tick(t0 + std::chrono::seconds(122));
    CHECK(host2.spawned.size() == 1);
}

TEST_CASE("Quests: Huhoan's drinking game: drinks poured to his mood, asked, the book for the flute") {
    RulesHost host;
    constexpr std::uint32_t kWang = 621, kHuhoan = 622;
    host.npcs = {{kWang, "NPC_WANG"}, {kHuhoan, "NPC_HUHOAN"}};
    host.near = {kWang, kHuhoan};
    server::Quests quests(host, {1.0, 1.0});
    std::vector<game::QuestDef> defs;
    defs.push_back(Parse("quest QNO_T_LOTTERY\nid 20150\ngiver NPC_WANG\nfinish NPC_WANG\ngoal deliver NPC_WANG ITEM_QNO_CH_BIGMAN_1_01 1\n"));
    defs.push_back(Parse("quest QNO_T_HUHOAN\nid 20151\nstart during QNO_T_LOTTERY\n"
                         "start holding-one ITEM_QNO_CH_EUROPE_3_04 ITEM_QNO_CH_EUROPE_3_05\ngiver NPC_HUHOAN\ntalk-game huhoan-drinks\n"));
    CHECK(game::WriteQuest(defs[1]).find("talk-game huhoan-drinks") != std::string::npos);
    quests.Load(defs);
    quests.Enter(1, {});
    Ask(quests, proto::QuestAccept{kWang, 20150});
    REQUIRE(quests.Record(1, 20150) != nullptr);
    host.bag["ITEM_QNO_WC_FERRY4_1_01"] = 1; // Asa's flute
    bool won = false;
    int lost = 0;
    for (int round = 0; round < 300 && !won; ++round) {
        host.bag["ITEM_QNO_CH_EUROPE_3_04"] = 1; // wine, 10
        host.bag["ITEM_QNO_CH_EUROPE_3_05"] = 1; // champagne, 20
        Ask(quests, proto::QuestTalk{kHuhoan, 20151});
        auto says = host.Last<proto::QuestSays>();
        REQUIRE(says.choice == proto::QuestChoice::Rows);
        CHECK(says.text == "SN_TALK_QNO_CH_BIGMAN_1_01");
        REQUIRE(says.rows == std::vector<std::string>{"SN_TALK_QNO_CH_BIGMAN_1_02", "SN_TALK_QNO_CH_BIGMAN_1_03"});
        Ask(quests, proto::QuestHandIn{kHuhoan, 20151, {0}}); // the wine
        says = host.Last<proto::QuestSays>();
        CHECK(says.text == "SN_TALK_QNO_CH_BIGMAN_1_08");
        REQUIRE(says.rows == std::vector<std::string>{"SN_TALK_QNO_CH_BIGMAN_1_03", "SN_TALK_QNO_CH_BIGMAN_1_15"});
        CHECK(host.bag["ITEM_QNO_CH_EUROPE_3_04"] == 0);
        Ask(quests, proto::QuestHandIn{kHuhoan, 20151, {0}}); // the champagne: 30
        says = host.Last<proto::QuestSays>();
        REQUIRE(says.rows == std::vector<std::string>{"SN_TALK_QNO_CH_BIGMAN_1_15"});
        Ask(quests, proto::QuestHandIn{kHuhoan, 20151, {0}}); // asked about the book
        says = host.Last<proto::QuestSays>();
        if (says.text == "SN_TALK_QNO_CH_BIGMAN_1_16") { // his mood was not 30: the drinks spent
            ++lost;
            continue;
        }
        CHECK(says.text == "SN_TALK_QNO_CH_BIGMAN_1_25"); // the flute held
        REQUIRE(says.rows == std::vector<std::string>{"SN_TALK_QNO_CH_BIGMAN_1_26", "SN_TALK_QNO_CH_BIGMAN_1_27"});
        Ask(quests, proto::QuestHandIn{kHuhoan, 20151, {0}}); // the book for the flute
        CHECK(host.Last<proto::QuestSays>().text == "SN_TALK_QNO_CH_BIGMAN_1_33");
        won = true;
    }
    REQUIRE(won);
    // (most moods, 32..38, cannot be met by drinks of tens: the original's own; `lost` counts those rounds)
    (void)lost;
    CHECK(host.bag["ITEM_QNO_CH_BIGMAN_1_01"] == 1);
    CHECK(host.bag["ITEM_QNO_WC_FERRY4_1_01"] == 0);
    REQUIRE(quests.Record(1, 20151) != nullptr);
    CHECK(quests.Record(1, 20151)->status == proto::QuestStatus::Done);
    // Afterwards: "the wine is all mine".
    Ask(quests, proto::QuestTalk{kHuhoan, 20151});
    CHECK(host.Last<proto::QuestSays>().text == "SN_TALK_QNO_CH_BIGMAN_1_42");
    // The lottery's book handed in.
    Ask(quests, proto::QuestTalk{kWang, 20150});
    Ask(quests, proto::QuestHandIn{kWang, 20150, {}});
    CHECK(quests.Record(1, 20150)->times == 1);
}

TEST_CASE("Quests: the beta quests: offered by the quests finished and the deeds, paid a bound weapon of the player's pick") {
    RulesHost host;
    constexpr std::uint32_t kHelper = 631, kOther = 632;
    const std::string helper = game::CustomNpcCodename(game::kBetaHelperName);
    CHECK(helper == "NPC_BETA_HELPER");
    host.npcs = {{kHelper, helper}, {kOther, "NPC_OTHER"}};
    host.near = {kHelper, kOther};
    server::Quests quests(host, {1.0, 1.0});
    std::vector<game::QuestDef> defs;
    const std::string text = game::BetaQuestDefinitions();
    for (std::size_t at = text.find("quest "); at != std::string::npos;) {
        const std::size_t next = text.find("\nquest ", at);
        defs.push_back(Parse(text.substr(at, next == std::string::npos ? std::string::npos : next + 1 - at)));
        at = next == std::string::npos ? next : next + 1;
    }
    REQUIRE(defs.size() == 10);
    for (const game::QuestDef& d : defs) {
        CAPTURE(d.codename);
        CHECK(d.reward.items.size() == 14);
        CHECK(d.reward.choose == 1);
        CHECK(d.reward.bound);
        CHECK(d.repeat == 1);
        game::QuestDef back;
        std::string error;
        REQUIRE_MESSAGE(game::ParseQuest(game::WriteQuest(d), back, error), error);
        CHECK(back == d);
    }
    // Three ordinary quests to finish (talks at another NPC), and a temple one.
    for (int k = 0; k < 3; ++k)
        defs.push_back(Parse(std::format("quest QNO_T_EASY_{}\nid {}\ngiver NPC_OTHER\nfinish NPC_OTHER\ngoal talk NPC_OTHER\n", k, 20160 + k)));
    quests.Load(defs);
    quests.Enter(1, {});
    const auto offers = [&] {
        quests.TellNpc(1, kHelper);
        std::set<std::uint32_t> out;
        for (const proto::NpcQuests::Row& r : host.Last<proto::NpcQuests>().rows)
            if (r.kind == proto::NpcQuestKind::Offer) out.insert(r.quest);
        return out;
    };
    // All ten offered from the start; what each asks is its goal, counted in the quest window.
    CHECK(offers().size() == 10);
    Ask(quests, proto::QuestAccept{kHelper, 40002});
    REQUIRE(quests.Record(1, 40002) != nullptr);
    CHECK(quests.Record(1, 40002)->progress[0] == 0);
    Ask(quests, proto::QuestTalk{kHelper, 40002});
    CHECK(host.Last<proto::QuestSays>().moment == "not-done"); // nothing finished yet
    for (int k = 0; k < 3; ++k) {
        Ask(quests, proto::QuestAccept{kOther, std::uint32_t(20160 + k)});
        Ask(quests, proto::QuestTalk{kOther, std::uint32_t(20160 + k)});
        Ask(quests, proto::QuestHandIn{kOther, std::uint32_t(20160 + k), {}});
        Ask(quests, proto::QuestTalk{kOther, std::uint32_t(20160 + k)});
        Ask(quests, proto::QuestHandIn{kOther, std::uint32_t(20160 + k), {}});
        CHECK(quests.Record(1, 40002)->progress[0] == k + 1); // told at each finish
    }
    CHECK(quests.Record(1, 40002)->status == proto::QuestStatus::Achieved);
    // Handed in with a pick: the chosen weapon given bound, once.
    Ask(quests, proto::QuestTalk{kHelper, 40002});
    CHECK(host.Last<proto::QuestSays>().moment == "done");
    Ask(quests, proto::QuestHandIn{kHelper, 40002, {4}}); // the fifth: China's bow
    CHECK(host.bag["ITEM_CH_BOW_02_C_RARE"] == 1);
    CHECK(host.bound == std::vector<std::string>{"ITEM_CH_BOW_02_C_RARE"});
    CHECK(host.bag["ITEM_CH_SWORD_02_C_RARE"] == 0);
    CHECK(!offers().contains(40002)); // once
    // A deed's: 10 matches; the tomb's "or": the temple quests' count would do as well.
    Ask(quests, proto::QuestAccept{kHelper, 40008});
    host.who.deeds["event-matches"] = 9;
    Ask(quests, proto::QuestTalk{kHelper, 40008});
    CHECK(host.Last<proto::QuestSays>().moment == "not-done");
    CHECK(quests.Record(1, 40008)->progress[0] == 9);
    host.who.deeds["event-matches"] = 10;
    Ask(quests, proto::QuestTalk{kHelper, 40008});
    CHECK(host.Last<proto::QuestSays>().moment == "done");
    Ask(quests, proto::QuestAccept{kHelper, 40010});
    host.who.deeds["tomb-hardest"] = 1;
    Ask(quests, proto::QuestTalk{kHelper, 40010});
    CHECK(host.Last<proto::QuestSays>().moment == "done");
}

TEST_CASE("Quests: the client's view keeps definitions, progress and the lines the HUD tells") {
    game::QuestView v;
    const game::QuestDef q = Parse("quest QNO_T_VIEW\nid 20020\nname \"View\"\ngiver NPC_A\nfinish NPC_A\ngoal kill 3 MOB_WOLF\ngoal drop 2 ITEM_T_FANG from MOB_WOLF 50% hidden\n"
                                   "talk NPC_A offer \"Help?\"\n");
    CHECK(v.Apply(proto::QuestDefinition{game::WriteQuest(q)}).empty());
    REQUIRE(v.Find(20020) != nullptr);
    CHECK(v.Find(20020)->name.literal == "View");
    CHECK(game::QuestView::TalkOf(*v.Find(20020), "NPC_A", "offer").literal == "Help?");
    CHECK(game::QuestView::TalkOf(*v.Find(20020), "NPC_B", "offer").Empty());
    v.Apply(proto::QuestList{{}, {{20030, 2}}});
    CHECK(v.Finished().at(20030) == 2);
    auto lines = v.Apply(proto::QuestChanged{{20020, proto::QuestStatus::Achieving, {0, 0}, 0}});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].kind == game::QuestView::Line::Kind::Taken);
    // A kill counted: its progress; the hidden goal's rise is not told; all met: achieved.
    lines = v.Apply(proto::QuestChanged{{20020, proto::QuestStatus::Achieving, {1, 1}, 0}});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].kind == game::QuestView::Line::Kind::Progress);
    CHECK(lines[0].value == 1);
    CHECK(lines[0].need == 3);
    lines = v.Apply(proto::QuestChanged{{20020, proto::QuestStatus::Achieved, {3, 2}, 0}});
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].kind == game::QuestView::Line::Kind::Achieved);
    lines = v.Apply(proto::QuestGone{20020, proto::QuestEnd::Finished, 1});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].kind == game::QuestView::Line::Kind::Finished);
    CHECK(v.Active().empty());
    CHECK(v.Finished().at(20020) == 1);
    // An NPC's rows and its words (taken once).
    v.Apply(proto::NpcQuests{77, {{20020, proto::NpcQuestKind::Offer}}});
    CHECK(v.NpcRows().npc == 77);
    v.Apply(proto::QuestSays{77, 20020, "offer", 0, proto::QuestChoice::Accept});
    CHECK(v.TakeSays().has_value());
    CHECK_FALSE(v.TakeSays().has_value());
    lines = v.Apply(proto::QuestAnswer{20020, proto::QuestCode::TooMany});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].code == proto::QuestCode::TooMany);
}

TEST_CASE("Game server: a quest's item used from the bag: taken by its quest's rule and used up, else refused 2 and kept") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    REQUIRE_MESSAGE(server::SaveQuest(t.Db(),
                                      Parse("quest QNO_T_SEARCH\nid 20012\nname \"The Search\"\nstart level 1-\ngiver NPC_T_GIVER\nfinish NPC_T_FINISHER\n"
                                            "start-item ITEM_T_TOOL 2\nstart-item ITEM_T_STRAY 1\non use ITEM_T_TOOL give ITEM_T_FOUND\n"
                                            "goal drop 1 ITEM_T_FOUND from MOB_T_NONE 0%\nreward gold 5\n"),
                                      server::QuestOrigin::Ours, error),
                    error);
    QuestRig rig(t);
    rig.Start();
    auto p = rig.Join("Seeker");
    p.c->Send(proto::NpcTalk{p.giver});
    CHECK(Got<proto::NpcMenu>(p).entity == p.giver);
    p.c->Send(proto::QuestAccept{p.giver, 20012});
    std::map<std::uint32_t, std::uint8_t> place;
    for (int i = 0; i < 4 && place.size() < 2; ++i) {
        const auto moved = Got<proto::ItemMoved>(p);
        for (const proto::ItemRecord& r : moved.changed) place[r.item] = r.slot;
    }
    REQUIRE(place.contains(kTool));
    REQUIRE(place.contains(kStray));
    CHECK(Got<proto::QuestChanged>(p).state.status == proto::QuestStatus::Achieving);
    CHECK(Got<proto::QuestAnswer>(p).code == proto::QuestCode::Ok);
    // The answer about an item: its refusal's code, or 0 when it was used (ItemUsed by its user).
    const auto answer = [&](std::uint32_t item) -> std::uint16_t {
        for (int i = 0; i < 400; ++i) {
            proto::ItemRefused refused;
            if (p.c->Expect(refused, 5ms) && refused.item == item) return refused.code;
            proto::ItemUsed used;
            if (p.c->Expect(used, 5ms) && used.item == item && used.entity == p.entity) return 0;
        }
        FAIL("no answer");
        return 0xFFFF;
    };
    // The tool: its quest's rule takes it (one used up), the found item given at once, the goal met.
    proto::UseItem use;
    use.slot = place[kTool];
    p.c->Send(use);
    CHECK(answer(kTool) == 0);
    bool found = false;
    std::uint16_t toolsLeft = 0xFFFF;
    for (int i = 0; i < 4 && (!found || toolsLeft == 0xFFFF); ++i) {
        const auto moved = Got<proto::ItemMoved>(p);
        for (const proto::ItemRecord& r : moved.changed) {
            if (r.item == kFound) found = true;
            if (r.item == kTool) toolsLeft = r.quantity;
        }
    }
    CHECK(found);
    CHECK(toolsLeft == 1);
    CHECK(Got<proto::QuestChanged>(p).state.status == proto::QuestStatus::Achieved);
    // The stray item: no quest's rule and no event takes it: refused 2, kept.
    use.slot = place[kStray];
    p.c->Send(use);
    CHECK(answer(kStray) == 2);
    proto::ItemMoved none;
    CHECK_FALSE(p.c->Expect(none, 200ms));
}

namespace {
// RulesHost with what a quest says outside a talk (QuestSay) and the players of a party: their places, the party's id.
struct PartyHost : RulesHost {
    std::vector<std::string> said;
    void QuestSay(std::uint32_t, const std::string& key) override { said.push_back(key); }
    std::map<std::uint32_t, proto::WorldPosition> at; // the party's members here (1 among them)
    std::uint32_t partyId = 0;                         // 0: no party sharing both
    std::optional<Player> QuestPlayer(std::uint32_t entity) override {
        if (entity != 1 && !at.contains(entity)) return std::nullopt;
        Player p = who;
        p.peer = entity;
        p.character = 70 + entity;
        return p;
    }
    KillParty QuestKillParty(std::uint32_t) override {
        KillParty k;
        k.id = partyId;
        if (partyId != 0)
            for (const auto& [entity, place] : at) k.members.push_back({entity, place});
        return k;
    }
};
server::QuestRecord UnderWayRecord(std::uint32_t quest) {
    server::QuestRecord r;
    r.quest = quest;
    r.status = proto::QuestStatus::Achieving;
    return r;
}
} // namespace

TEST_CASE("Quests: a daily quest again on a new day; a limit's words; 33 at once; the pay and the give-up take every piece") {
    PartyHost host;
    host.npcs = {{kGiver, "NPC_GIVER"}, {kFinisher, "NPC_FINISHER"}};
    host.near = {kGiver, kFinisher};
    server::Quests quests(host, {});
    std::vector<game::QuestDef> defs;
    defs.push_back(Parse("quest QNO_T_DAILY\nid 20010\nstart real-clock\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal kill 1 MOB_WOLF\n"));
    defs.push_back(Parse("quest QNO_T_QUOTA\nid 20011\nstart quota 1\nrepeat 0\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal kill 1 MOB_WOLF\n"));
    defs.push_back(Parse("quest QNO_T_GATHER\nid 20012\nrepeat 0\ngiver NPC_GIVER\nfinish NPC_GIVER\nstart-item ITEM_T_LETTER 1\n"
                         "goal drop 2 ITEM_T_FANG from MOB_WOLF 100%\ngoal drop 2 ITEM_T_CLAW from MOB_WOLF 100%\n"
                         "goal deliver NPC_FINISHER ITEM_T_LETTER 1 get ITEM_T_REPLY 1\nreward item ITEM_T_POTION 1\n"));
    defs.push_back(Parse("quest QNO_T_HWAN\nid 20014\nstart hwan 3\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal kill 1 MOB_WOLF\n"));
    for (int i = 1; i <= 34; ++i) defs.push_back(Parse(std::format("quest QNO_T_MANY_{}\nid {}\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal kill 9 MOB_BAT\n", i, i)));
    quests.Load(defs);
    // Finished three days ago at its limit: its count back to 0, and offered (the original's reset by the calendar day).
    server::QuestRecord daily;
    daily.quest = 20010;
    daily.status = proto::QuestStatus::Done;
    daily.times = 1;
    daily.ended = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() - 3 * 86400;
    quests.Enter(1, {daily});
    CHECK(quests.Record(1, 20010)->times == 0);
    Ask(quests, proto::QuestTalk{kGiver, 20010});
    CHECK(host.Last<proto::QuestSays>().moment == "offer");
    Ask(quests, proto::QuestAccept{kGiver, 20010});
    quests.Killed(9000, "MOB_WOLF", 0, 1u);
    Ask(quests, proto::QuestHandIn{kGiver, 20010, {}});
    CHECK(quests.Record(1, 20010)->times == 1);
    // Done today: still listed at its giver, its talk the count-limit words; not taken.
    quests.TellNpc(1, kGiver);
    const auto rows = host.Last<proto::NpcQuests>().rows;
    CHECK(std::any_of(rows.begin(), rows.end(), [](const proto::NpcQuests::Row& r) { return r.quest == 20010 && r.kind == proto::NpcQuestKind::Offer; }));
    Ask(quests, proto::QuestTalk{kGiver, 20010});
    CHECK(host.Last<proto::QuestSays>().moment == "count-limit");
    Ask(quests, proto::QuestAccept{kGiver, 20010});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::NotOffered);

    // The window's quota spent: count-limit; a give-up gives its start back.
    Ask(quests, proto::QuestAccept{kGiver, 20011});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    quests.Killed(9001, "MOB_WOLF", 0, 1u);
    Ask(quests, proto::QuestHandIn{kGiver, 20011, {}});
    Ask(quests, proto::QuestTalk{kGiver, 20011});
    CHECK(host.Last<proto::QuestSays>().moment == "count-limit");
    host.hour = 22; // the night's window: a new quota
    Ask(quests, proto::QuestAccept{kGiver, 20011});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    Ask(quests, proto::QuestAbandon{20011});
    Ask(quests, proto::QuestAccept{kGiver, 20011});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok); // its start given back
    Ask(quests, proto::QuestAbandon{20011});
    host.hour = 12;

    // No hwan level asked (the original's offer test reads none).
    Ask(quests, proto::QuestAccept{kGiver, 20014});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    Ask(quests, proto::QuestAbandon{20014});

    // One gathering goal a kill (the first the monster feeds); a quest item refused to the ground's pickup once every
    // gathering goal for it holds its count.
    Ask(quests, proto::QuestAccept{kGiver, 20012});
    host.dropped.clear();
    quests.Killed(9002, "MOB_WOLF", 0, 1u);
    CHECK(host.dropped == std::vector<std::string>{"ITEM_T_FANG"});
    host.bag["ITEM_T_FANG"] = 1;
    CHECK_FALSE(quests.PickUpRefused(1, "ITEM_T_FANG"));
    host.bag["ITEM_T_FANG"] = 5;
    host.bag["ITEM_T_CLAW"] = 2;
    quests.BagChanged(1);
    CHECK(quests.PickUpRefused(1, "ITEM_T_FANG"));
    CHECK_FALSE(quests.PickUpRefused(1, "ITEM_T_OTHER"));
    // The delivery's handed-back item, then the reward's room asked at the finisher's talk: none, the bag-full words.
    Ask(quests, proto::QuestHandIn{kFinisher, 20012, {}});
    CHECK(host.bag["ITEM_T_REPLY"] == 1);
    host.bag["ITEM_T_REPLY"] = 2;
    host.noRoom = {"ITEM_T_POTION"};
    Ask(quests, proto::QuestTalk{kGiver, 20012});
    CHECK(host.Last<proto::QuestSays>().moment == "bag-full");
    host.noRoom.clear();
    Ask(quests, proto::QuestTalk{kGiver, 20012});
    CHECK(host.Last<proto::QuestSays>().moment == "done");
    // The pay takes every held piece of the gathered and handed-back items.
    Ask(quests, proto::QuestHandIn{kGiver, 20012, {}});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    CHECK(host.bag["ITEM_T_FANG"] == 0);
    CHECK(host.bag["ITEM_T_CLAW"] == 0);
    CHECK(host.bag["ITEM_T_REPLY"] == 0);
    CHECK(host.bag["ITEM_T_POTION"] == 1);
    // Given up after the delivery: its handed-back item goes too.
    Ask(quests, proto::QuestAccept{kGiver, 20012});
    Ask(quests, proto::QuestHandIn{kFinisher, 20012, {}});
    CHECK(host.bag["ITEM_T_REPLY"] == 1);
    Ask(quests, proto::QuestAbandon{20012});
    CHECK(host.bag["ITEM_T_REPLY"] == 0);

    // 33 of the original's quests under way at once; ours (from 10001) do not count.
    Ask(quests, proto::QuestAccept{kGiver, 20012});
    for (std::uint32_t id = 1; id <= 33; ++id) {
        Ask(quests, proto::QuestAccept{kGiver, id});
        CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    }
    Ask(quests, proto::QuestAccept{kGiver, 34});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::TooMany);
}

TEST_CASE("Quests: a time limit runs only while its holder is in the world; its words at the end") {
    PartyHost host;
    host.npcs = {{kGiver, "NPC_GIVER"}};
    host.near = {kGiver};
    server::Quests quests(host, {});
    quests.Load({Parse("quest QNO_T_TIMER\nid 20013\ntimer 30\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal kill 1 MOB_WOLF\n"
                       "talk NPC_GIVER time-out @SN_T_TIMEOUT\n")});
    quests.Enter(1, {});
    Ask(quests, proto::QuestAccept{kGiver, 20013});
    CHECK(host.Last<proto::QuestChanged>().state.secondsLeft == 30);
    // Out of the world: its seconds left kept in the record; an hour offline does not count.
    quests.Leave(1);
    server::QuestRecord kept = host.saved.back();
    CHECK(kept.progress[server::Quests::kTimerSlot] >= 29);
    kept.started -= 3600;
    quests.Enter(1, {kept});
    const auto now = server::Quests::Clock::now();
    quests.Tick(now + std::chrono::seconds(20));
    REQUIRE(quests.Record(1, 20013));
    CHECK(quests.Record(1, 20013)->status == proto::QuestStatus::Achieving);
    CHECK(host.saved.back().progress[server::Quests::kTimerSlot] == 10); // kept as it runs
    quests.Tick(now + std::chrono::seconds(31));
    CHECK(quests.Record(1, 20013)->status == proto::QuestStatus::Abandoned);
    CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::TimeOut);
    CHECK(host.said == std::vector<std::string>{"SN_T_TIMEOUT"});
}

TEST_CASE("Quests: a kill counts for its killer; in a party sharing both it is shared by each quest's DivideType") {
    PartyHost host;
    host.npcs = {{kGiver, "NPC_GIVER"}};
    server::Quests quests(host, {});
    quests.Load({Parse("quest QNO_P_TURN\nid 20020\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal kill 9 MOB_WOLF\n"),
                 Parse("quest QNO_P_ALL\nid 20021\ndivide 5\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal kill 9 MOB_WOLF\n"),
                 Parse("quest QNO_P_KILLER\nid 20022\ndivide 2\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal kill 9 MOB_WOLF\n"),
                 Parse("quest QNO_P_PARTY\nid 20023\ndivide 1\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal kill 9 MOB_WOLF\n")});
    // Members 1 and 2 side by side, 3 far above (1200 in 3D), 4 near but holding none.
    host.at[1] = {25000, 100.0f, 0.0f, 100.0f, 0};
    host.at[2] = {25000, 150.0f, 0.0f, 100.0f, 0};
    host.at[3] = {25000, 100.0f, 1200.0f, 100.0f, 0};
    host.at[4] = {25000, 120.0f, 0.0f, 100.0f, 0};
    const std::vector<server::QuestRecord> all = {UnderWayRecord(20020), UnderWayRecord(20021), UnderWayRecord(20022), UnderWayRecord(20023)};
    for (std::uint32_t e : {1u, 2u, 3u}) quests.Enter(e, all);
    quests.Enter(4, {});
    const auto count = [&](std::uint32_t e, std::uint32_t quest) { return quests.Record(e, quest)->progress[0]; };

    // No party: the killer's own, and a party quest (DivideType 1) not at all.
    quests.Killed(9000, "MOB_WOLF", 0, 2u);
    CHECK(count(2, 20020) == 1);
    CHECK(count(2, 20021) == 1);
    CHECK(count(2, 20022) == 1);
    CHECK(count(2, 20023) == 0);
    CHECK(count(1, 20020) == 0);

    // A party sharing both: DivideType 0 one holder near at a time in turn, 5 and 1 every holder near, 2 the killer.
    host.partyId = 7;
    for (int k = 0; k < 4; ++k) quests.Killed(9001 + k, "MOB_WOLF", 0, 1u);
    CHECK(count(1, 20020) == 2);
    CHECK(count(2, 20020) == 3); // 1 before, then 2 of the 4 by turn
    CHECK(count(3, 20020) == 0); // too far
    CHECK(count(1, 20021) == 4);
    CHECK(count(2, 20021) == 5);
    CHECK(count(3, 20021) == 0);
    CHECK(count(1, 20022) == 4);
    CHECK(count(2, 20022) == 1);
    CHECK(count(1, 20023) == 4);
    CHECK(count(2, 20023) == 4);
    CHECK(count(3, 20023) == 0);

    // A player's kill goes the same way (the original's event 0x35).
    quests.Load({Parse("quest QNO_P_PK\nid 20030\ndivide 5\ngiver NPC_GIVER\nfinish NPC_GIVER\ngoal kill-player 5 level 1\n")});
    for (std::uint32_t e : {1u, 2u}) quests.Enter(e, {UnderWayRecord(20030)});
    quests.PlayerKilled(2, 50, 25000, "");
    CHECK(count(1, 20030) == 1);
    CHECK(count(2, 20030) == 1);
}
