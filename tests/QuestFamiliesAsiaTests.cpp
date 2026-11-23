// The original's coded quests of Samarkand, Donwhang's warehouse, the Tomb of Qin Shi and Roc Mountain (server/common
// OriginalQuestsAsia.cpp): the definitions, their chains, and their play on our quest engine
// (a made-up host; one on our game server).

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include "ServerTestSupport.h"
#include "sr/core/Jobs.h"
#include "sr/assets/GameData.h"
#include "sr/game/CharacterRules.h"
#include "sr/game/Quest.h"
#include "sr/game/QuestProtocol.h"
#include "sr/server/GameServer.h"
#include "sr/server/QuestScript.h"
#include "sr/server/QuestStore.h"
#include "sr/server/Quests.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;

namespace {

// The 27 coded quests of the Asian families whose items are used from the bag, with their chains' coded quests.
const std::set<std::string> kAsia = {
    "QNO_CA_HORSE_3",       "QNO_CA_WAREHOUSE_1",   "QNO_CA_WAREHOUSE_3",   "QNO_CA_TREASURE_4",    "QNO_CA_TREASURE_5",
    "QNO_CA_GORIA_2",       "QNO_CA_GORIA_3",       "QNO_CA_GORIA_6",       "QNO_CA_GORIA_7",       "QNO_WC_WAREHOUSE_W_2",
    "QNO_WC_WAREHOUSE_W_3", "QNO_TQ_ORNAMENT_2",    "QNO_TQ_ORNAMENT_3",    "QNO_TQ_ORNAMENT_4",    "QNO_TQ_FINDTHIEF_1",
    "QNO_TQ_FINDTHIEF_2",   "QNO_TQ_BAEKRYOUNG_2",  "QNO_TQ_BAEKRYOUNG_4",  "QNO_TQ_STONEBOARD_1",  "QNO_TQ_STONEBOARD_2",
    "QNO_TQ_STONEBOARD_4",  "QNO_RM_FLYSHIP1_2",    "QNO_RM_FLYSHIP1_3",    "QNO_RM_OLDWOMAN_2",    "QNO_RM_OLDWOMAN_3",
    "QNO_RM_OLDWOMAN_4",    "QNO_RM_OLDWOMAN_5"};

// The chains, first to last; a star marks the script's quests (Quest.sct), the others are ours (the coded ones).
const std::vector<std::vector<std::string>> kChains = {
    {"*QNO_CA_HORSE_2", "QNO_CA_HORSE_3", "QNO_CA_WAREHOUSE_1"},
    {"QNO_CA_WAREHOUSE_3"},
    {"*QNO_CA_TREASURE_1", "*QNO_CA_TREASURE_2", "*QNO_CA_TREASURE_3", "QNO_CA_TREASURE_4", "QNO_CA_TREASURE_5"},
    {"*QNO_CA_GORIA_1", "QNO_CA_GORIA_2", "QNO_CA_GORIA_3", "*QNO_CA_GORIA_4", "*QNO_CA_GORIA_5", "QNO_CA_GORIA_6", "QNO_CA_GORIA_7"},
    {"*QNO_WC_WAREHOUSE_W_1", "QNO_WC_WAREHOUSE_W_2", "QNO_WC_WAREHOUSE_W_3"},
    {"*QNO_TQ_ORNAMENT_1", "QNO_TQ_ORNAMENT_2", "QNO_TQ_ORNAMENT_3", "QNO_TQ_ORNAMENT_4"},
    {"QNO_TQ_FINDTHIEF_1", "QNO_TQ_FINDTHIEF_2"},
    {"*QNO_TQ_BAEKRYOUNG_1", "QNO_TQ_BAEKRYOUNG_2", "*QNO_TQ_BAEKRYOUNG_3", "QNO_TQ_BAEKRYOUNG_4"},
    {"QNO_TQ_STONEBOARD_1", "QNO_TQ_STONEBOARD_2", "*QNO_TQ_STONEBOARD_3", "QNO_TQ_STONEBOARD_4"},
    {"*QNO_RM_FLYSHIP1_1", "QNO_RM_FLYSHIP1_2", "QNO_RM_FLYSHIP1_3"},
    {"*QNO_RM_OLDWOMAN_1", "QNO_RM_OLDWOMAN_2", "QNO_RM_OLDWOMAN_3", "QNO_RM_OLDWOMAN_4", "QNO_RM_OLDWOMAN_5"},
};

std::string Code(const std::string& step) { return step.starts_with('*') ? step.substr(1) : step; }

std::map<std::string, game::QuestDef> CodedAsia() {
    std::vector<game::QuestDef> coded;
    std::string error;
    REQUIRE_MESSAGE(server::DecodeCodedQuests(coded, error), error);
    std::map<std::string, game::QuestDef> out;
    for (game::QuestDef& q : coded)
        if (kAsia.contains(q.codename)) out[q.codename] = std::move(q);
    return out;
}

// The script's quests (SR_QUEST_SCRIPT or SR_VSRO_DATA's Quest.sct): empty without it.
std::map<std::string, game::QuestDef> AsiaScript() {
    const std::filesystem::path path = QuestScriptPath();
    std::map<std::string, game::QuestDef> out;
    if (!std::filesystem::exists(path)) return out;
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<game::QuestDef> quests;
    std::vector<std::string> notes;
    std::string error;
    REQUIRE_MESSAGE(server::DecodeQuestScript(bytes, quests, notes, error), error);
    for (game::QuestDef& q : quests) out[q.codename] = std::move(q);
    return out;
}

// One player, its bag by codename, the NPCs near it, what was sent, said and saved.
struct AsiaHost : server::QuestHost {
    Player who;
    std::map<std::string, std::uint32_t> bag;
    std::map<std::uint32_t, std::string> npcs; // entity -> codename, all within reach
    std::vector<std::pair<proto::MessageId, std::vector<std::byte>>> sent;
    std::vector<std::string> said;
    Reward rewarded;
    bool freePlace = true;
    AsiaHost() {
        who.peer = 1;
        who.character = 77;
        who.name = "Hero";
        who.level = 110;
        who.race = "china";
    }
    std::optional<Player> QuestPlayer(std::uint32_t entity) override { return entity == 1 ? std::optional(who) : std::nullopt; }
    // In a party sharing both (who.sharedParty): a kill's share among the killer alone here.
    KillParty QuestKillParty(std::uint32_t killer) override {
        if (!who.sharedParty) return {};
        return {7, {{killer, who.position}}};
    }
    void QuestSend(std::uint64_t, proto::MessageId id, const std::vector<std::byte>& payload) override { sent.emplace_back(id, payload); }
    std::string QuestNpcNear(std::uint32_t, std::uint32_t npc) override { return npcs.contains(npc) ? npcs[npc] : std::string(); }
    int QuestHour() override { return 12; }
    std::uint32_t QuestItemCount(std::uint32_t, const std::string& item) override { return bag.contains(item) ? bag[item] : 0; }
    bool QuestGive(std::uint32_t, const std::vector<game::QuestGoal::Items>& items) override {
        for (const auto& i : items) bag[i.item] += i.count;
        return true;
    }
    bool QuestTake(std::uint32_t, const std::vector<game::QuestGoal::Items>& items) override {
        for (const auto& i : items)
            if (QuestItemCount(1, i.item) < i.count) return false;
        for (const auto& i : items) bag[i.item] -= i.count;
        return true;
    }
    bool QuestRoom(std::uint32_t, const std::vector<game::QuestGoal::Items>&) override { return freePlace; }
    bool QuestTakeGold(std::uint32_t, std::uint64_t) override { return true; }
    void QuestReward(std::uint32_t, const Reward& r) override { rewarded = r; }
    void QuestDrop(std::uint32_t, std::uint32_t, const std::string& item) override { ++bag[item]; } // picked up at once
    void QuestSave(std::int64_t, const server::QuestRecord&) override {}
    bool QuestThemeDone(std::uint32_t, const std::string&) override { return false; }
    bool QuestFreePlace(std::uint32_t) override { return freePlace; }
    void QuestSay(std::uint32_t, const std::string& key) override { said.push_back(key); }

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
    std::string LastSaid() { return Last<proto::QuestSays>().moment; }
};

template <typename T>
void Ask(server::Quests& quests, const T& message) {
    REQUIRE(quests.Handle(1, T::kId, proto::Encode(message)));
}

// The quests on the engine, each given a number (the import's from _RefQuest; here the order).
struct Played {
    AsiaHost host;
    server::Quests quests{host, {}};
    std::map<std::string, std::uint32_t> id;
    std::map<std::string, std::uint32_t> npc; // codename -> entity
    explicit Played(std::vector<game::QuestDef> defs) {
        std::uint32_t next = 1;
        std::uint32_t entity = 5000;
        for (game::QuestDef& q : defs) {
            q.id = next++;
            id[q.codename] = q.id;
            const auto meet = [&](const std::string& code) {
                if (!code.empty() && !npc.contains(code)) {
                    npc[code] = entity;
                    host.npcs[entity++] = code;
                }
            };
            for (const std::string& n : q.givers) meet(n);
            for (const std::string& n : q.npcs) meet(n);
            meet(q.finisher);
            for (const game::QuestGoal& g : q.goals) meet(g.npc);
        }
        quests.Load(std::move(defs));
    }
    void Enter(std::vector<server::QuestRecord> records = {}) { quests.Enter(1, std::move(records)); }
    const server::QuestRecord* Record(const std::string& code) { return quests.Record(1, id.at(code)); }
    void Talk(const std::string& npcCode, const std::string& quest) { Ask(quests, proto::QuestTalk{npc.at(npcCode), id.at(quest)}); }
    void Accept(const std::string& npcCode, const std::string& quest) { Ask(quests, proto::QuestAccept{npc.at(npcCode), id.at(quest)}); }
    void HandIn(const std::string& npcCode, const std::string& quest) { Ask(quests, proto::QuestHandIn{npc.at(npcCode), id.at(quest), {}}); }
    server::QuestRecord Done(const std::string& code) {
        server::QuestRecord r;
        r.quest = id.at(code);
        r.status = proto::QuestStatus::Done;
        r.times = 1;
        return r;
    }
};

std::vector<game::QuestDef> Pick(const std::map<std::string, game::QuestDef>& from, std::initializer_list<const char*> codes) {
    std::vector<game::QuestDef> out;
    for (const char* c : codes) {
        const auto it = from.find(c);
        REQUIRE_MESSAGE(it != from.end(), c);
        out.push_back(it->second);
    }
    return out;
}

} // namespace

TEST_CASE("Asian coded quests: our 27 definitions read back the same, each from its class's setup") {
    const auto coded = CodedAsia();
    REQUIRE(coded.size() == kAsia.size());
    for (const auto& [code, q] : coded) {
        CAPTURE(code);
        game::QuestDef back;
        std::string error;
        REQUIRE_MESSAGE(game::ParseQuest(game::WriteQuest(q), back, error), error);
        CHECK(back == q);
        CHECK(!q.givers.empty());
        CHECK(q.repeat >= 1);
        CHECK(q.minLevel > 1);
        CHECK(!q.goals.empty());
        // Every NPC it names says something there.
        for (const std::string& npc : q.npcs)
            CHECK(std::any_of(q.talks.begin(), q.talks.end(), [&](const game::QuestTalk& t) { return t.npc == npc; }));
    }
    // The 13 whose items are used from the bag.
    for (const char* item : {"QNO_CA_WAREHOUSE_1", "QNO_CA_WAREHOUSE_3", "QNO_CA_TREASURE_5", "QNO_CA_GORIA_7", "QNO_WC_WAREHOUSE_W_2", "QNO_WC_WAREHOUSE_W_3",
                             "QNO_TQ_ORNAMENT_3", "QNO_TQ_ORNAMENT_4", "QNO_TQ_FINDTHIEF_2", "QNO_TQ_BAEKRYOUNG_4", "QNO_TQ_STONEBOARD_4", "QNO_RM_FLYSHIP1_3",
                             "QNO_RM_OLDWOMAN_5"})
        CHECK(coded.contains(item));
    // What their setups give: the start's items (a delivery's, a kind 4's, a capture's traps), the helpers, the captures.
    CHECK(coded.at("QNO_CA_HORSE_3").startItems == std::vector<game::QuestGoal::Items>{{"ITEM_QNO_CA_HORSE_2_01", 30}});
    CHECK(coded.at("QNO_CA_GORIA_2").startItems == std::vector<game::QuestGoal::Items>{{"ITEM_QNO_CA_GORIA_2_01", 70}});
    CHECK(coded.at("QNO_CA_WAREHOUSE_1").goals[1].hidden);
    CHECK(coded.at("QNO_TQ_FINDTHIEF_2").goals[0].kind == game::QuestGoalKind::Capture);
    CHECK(coded.at("QNO_TQ_ORNAMENT_2").goals[0].received == std::vector<game::QuestGoal::Items>{{"ITEM_QNO_TQ_ORNAMENT_2_02", 1}});
    CHECK(coded.at("QNO_CA_GORIA_7").party);
    CHECK(coded.at("QNO_TQ_ORNAMENT_4").onKill.size() == 1);
}

TEST_CASE("Asian coded quests: each chain in its order, the next offered only after the one before") {
    const auto coded = CodedAsia();
    const auto script = AsiaScript();
    // The chains as the definitions say them: each step's quests-first list names the one before.
    for (const auto& chain : kChains) {
        for (std::size_t i = 0; i < chain.size(); ++i) {
            const std::string code = Code(chain[i]);
            CAPTURE(code);
            const bool fromScript = chain[i].starts_with('*');
            if (fromScript && script.empty()) continue;
            const game::QuestDef& q = fromScript ? script.at(code) : coded.at(code);
            if (i == 0) {
                CHECK(q.afterAll.empty());
            } else {
                CHECK(q.afterAll == std::vector<std::string>{Code(chain[i - 1])});
            }
        }
    }
    // On the engine: every chain's quests (the script's stand-ins without the script), a level-110 player in a sharing party.
    std::vector<game::QuestDef> all;
    for (const auto& chain : kChains) {
        for (std::size_t i = 0; i < chain.size(); ++i) {
            const std::string code = Code(chain[i]);
            if (!chain[i].starts_with('*')) {
                all.push_back(coded.at(code));
            } else if (script.contains(code)) {
                all.push_back(script.at(code));
            } else {
                game::QuestDef stand;
                stand.codename = code;
                stand.givers = {"NPC_T_STAND_IN"};
                if (i > 0) stand.afterAll = {Code(chain[i - 1])};
                all.push_back(stand);
            }
        }
    }
    Played play(all);
    play.host.who.sharedParty = true;
    for (const auto& chain : kChains) {
        std::vector<server::QuestRecord> finished;
        for (std::size_t i = 0; i < chain.size(); ++i) {
            const std::string code = Code(chain[i]);
            CAPTURE(code);
            play.Enter(finished);
            const game::QuestDef* q = play.quests.Find(code);
            REQUIRE(q != nullptr);
            CHECK(play.quests.Offered(1, *q));
            // The next is not offered before this one is finished.
            if (i + 1 < chain.size()) CHECK_FALSE(play.quests.Offered(1, *play.quests.Find(Code(chain[i + 1]))));
            finished.push_back(play.Done(code));
        }
        // All done: none is offered again (each once, but WAREHOUSE_3 three times and W_2 twice).
        play.Enter(finished);
        for (const std::string& step : chain) {
            const game::QuestDef* q = play.quests.Find(Code(step));
            CHECK(play.quests.Offered(1, *q) == (q->repeat > 1));
        }
    }
}

TEST_CASE("Asian coded quests: a delivery given at the start, a helper that never holds back, a kill's change, a hand-back") {
    const auto coded = CodedAsia();

    SUBCASE("Hoyun's leather: 30 given at the start, delivered to Saesa and paid there (its chain's first stood in)") {
        std::vector<game::QuestDef> defs = Pick(coded, {"QNO_CA_HORSE_3"});
        game::QuestDef before;
        before.codename = "QNO_CA_HORSE_2";
        before.givers = {"NPC_CA_HORSE"};
        defs.insert(defs.begin(), before);
        Played play(defs);
        play.Enter({play.Done("QNO_CA_HORSE_2")});
        play.Talk("NPC_CA_HORSE", "QNO_CA_HORSE_3");
        CHECK(play.host.LastSaid() == "offer");
        play.Accept("NPC_CA_HORSE", "QNO_CA_HORSE_3");
        CHECK(play.host.bag["ITEM_QNO_CA_HORSE_2_01"] == 30);
        // At Saesa: due, handed over, paid there (no finisher of its own).
        play.HandIn("NPC_CA_WAREHOUSE", "QNO_CA_HORSE_3");
        CHECK(play.host.bag["ITEM_QNO_CA_HORSE_2_01"] == 0);
        CHECK(play.Record("QNO_CA_HORSE_3")->status == proto::QuestStatus::Done);
    }

    SUBCASE("Saesa's grass: the earwigs a helper, the grass the goal") {
        std::vector<game::QuestDef> defs = Pick(coded, {"QNO_CA_WAREHOUSE_1"});
        defs.front().afterAll.clear();
        Played play(defs);
        play.Enter();
        play.Accept("NPC_CA_WAREHOUSE", "QNO_CA_WAREHOUSE_1");
        // The archers drop earwigs (100 %, up to 5): the helper counts them.
        for (int i = 0; i < 7; ++i) play.quests.Killed(9000, "MOB_QT_01_ARCHER_CLON", 0, 1u);
        CHECK(play.host.bag["ITEM_QNO_CA_WAREHOUSE_1_02"] == 5);
        CHECK(play.Record("QNO_CA_WAREHOUSE_1")->status == proto::QuestStatus::Achieving);
        // Ten grass found and every earwig spent but two: met all the same; the hand-in takes the grass, the earwigs stay.
        play.host.bag["ITEM_QNO_CA_WAREHOUSE_1_02"] = 2;
        play.host.bag["ITEM_QNO_CA_WAREHOUSE_1_01"] = 10;
        play.quests.BagChanged(1);
        CHECK(play.Record("QNO_CA_WAREHOUSE_1")->status == proto::QuestStatus::Achieved);
        play.HandIn("NPC_CA_WAREHOUSE", "QNO_CA_WAREHOUSE_1");
        CHECK(play.Record("QNO_CA_WAREHOUSE_1")->status == proto::QuestStatus::Done);
        CHECK(play.host.bag["ITEM_QNO_CA_WAREHOUSE_1_01"] == 0);
        CHECK(play.host.bag["ITEM_QNO_CA_WAREHOUSE_1_02"] == 2);
    }

    SUBCASE("Shahad's stones: each golem changes a lost stone into an added one, 70 the goal") {
        std::vector<game::QuestDef> defs = Pick(coded, {"QNO_CA_GORIA_2"});
        defs.front().afterAll.clear();
        Played play(defs);
        play.Enter();
        play.Accept("NPC_CA_HUNTER", "QNO_CA_GORIA_2");
        CHECK(play.host.bag["ITEM_QNO_CA_GORIA_2_01"] == 70);
        // No bag place: the original's _08, nothing changed.
        play.host.freePlace = false;
        play.quests.Killed(9000, "MOB_CA_GOLEM_CLON", 0, 1u);
        CHECK(play.host.said.back() == "SN_TALK_QNO_CA_GORIA_2_08");
        CHECK(play.host.bag["ITEM_QNO_CA_GORIA_2_02"] == 0);
        play.host.freePlace = true;
        for (int i = 0; i < 69; ++i) play.quests.Killed(9000, "MOB_CA_GOLEM_CLON", 0, 1u);
        CHECK(play.host.bag["ITEM_QNO_CA_GORIA_2_02"] == 69);
        CHECK(play.host.bag["ITEM_QNO_CA_GORIA_2_01"] == 1);
        CHECK(play.Record("QNO_CA_GORIA_2")->status == proto::QuestStatus::Achieving);
        // Another monster changes nothing.
        play.quests.Killed(9000, "MOB_CA_ONG", 0, 1u);
        CHECK(play.host.bag["ITEM_QNO_CA_GORIA_2_01"] == 1);
        play.quests.Killed(9000, "MOB_CA_GOLEM_CLON", 0, 1u);
        CHECK(play.host.bag["ITEM_QNO_CA_GORIA_2_02"] == 70);
        CHECK(play.Record("QNO_CA_GORIA_2")->status == proto::QuestStatus::Achieved);
        play.HandIn("NPC_CA_HUNTER", "QNO_CA_GORIA_2");
        CHECK(play.Record("QNO_CA_GORIA_2")->status == proto::QuestStatus::Done);
        CHECK(play.host.bag["ITEM_QNO_CA_GORIA_2_02"] == 0);
    }

    SUBCASE("Tricia's materials: two drops and the water the potion merchant hands back, all taken at the finish") {
        std::vector<game::QuestDef> defs = Pick(coded, {"QNO_CA_TREASURE_4"});
        defs.front().afterAll.clear();
        Played play(defs);
        play.Enter();
        play.Accept("NPC_CA_SMITH", "QNO_CA_TREASURE_4");
        play.quests.Killed(9000, "MOB_QT_01_ONG_CLON", 0, 1u);
        CHECK(play.host.bag["ITEM_QNO_CA_TREASURE_4_01"] == 1);
        play.host.bag["ITEM_QNO_CA_TREASURE_4_02"] = 1;
        play.quests.BagChanged(1);
        // The merchant: due; no room refuses with its words; then the water.
        play.Talk("NPC_CA_POTION", "QNO_CA_TREASURE_4");
        CHECK(play.host.Last<proto::QuestSays>().choice == proto::QuestChoice::HandIn);
        play.host.freePlace = false;
        play.HandIn("NPC_CA_POTION", "QNO_CA_TREASURE_4");
        CHECK(play.host.LastSaid() == "bag-full");
        CHECK(play.host.bag["ITEM_QNO_CA_TREASURE_4_03"] == 0);
        play.host.freePlace = true;
        play.HandIn("NPC_CA_POTION", "QNO_CA_TREASURE_4");
        CHECK(play.host.bag["ITEM_QNO_CA_TREASURE_4_03"] == 1);
        CHECK(play.Record("QNO_CA_TREASURE_4")->status == proto::QuestStatus::Achieved);
        // Its words there afterwards; then Tricia takes the three.
        play.Talk("NPC_CA_POTION", "QNO_CA_TREASURE_4");
        CHECK(play.host.LastSaid() == "middle"); // its _09, "You still need something from me?"
        play.HandIn("NPC_CA_SMITH", "QNO_CA_TREASURE_4");
        CHECK(play.Record("QNO_CA_TREASURE_4")->status == proto::QuestStatus::Done);
        for (const char* item : {"ITEM_QNO_CA_TREASURE_4_01", "ITEM_QNO_CA_TREASURE_4_02", "ITEM_QNO_CA_TREASURE_4_03"}) CHECK(play.host.bag[item] == 0);
    }
}

TEST_CASE("Asian coded quests: the tools given again, taken again with its own words, a party's instance kill") {
    const auto coded = CodedAsia();

    SUBCASE("Tricia's Seal Keys: five at the start, five again when none is left") {
        std::vector<game::QuestDef> defs = Pick(coded, {"QNO_CA_TREASURE_5"});
        defs.front().afterAll.clear();
        Played play(defs);
        play.Enter();
        play.Accept("NPC_CA_SMITH", "QNO_CA_TREASURE_5");
        CHECK(play.host.bag["ITEM_QNO_CA_TREASURE_5_01"] == 5);
        play.Talk("NPC_CA_SMITH", "QNO_CA_TREASURE_5");
        CHECK(play.host.LastSaid() == "not-done");
        CHECK(play.host.bag["ITEM_QNO_CA_TREASURE_5_01"] == 5);
        play.host.bag["ITEM_QNO_CA_TREASURE_5_01"] = 0;
        play.Talk("NPC_CA_SMITH", "QNO_CA_TREASURE_5");
        CHECK(play.host.LastSaid() == "resupply");
        CHECK(play.host.bag["ITEM_QNO_CA_TREASURE_5_01"] == 5);
        // The guardian's death drops the box (100 %): met, handed in.
        play.quests.Killed(9000, "MOB_QT_01_ONG", 0, 1u);
        CHECK(play.Record("QNO_CA_TREASURE_5")->status == proto::QuestStatus::Achieved);
        play.HandIn("NPC_CA_SMITH", "QNO_CA_TREASURE_5");
        CHECK(play.Record("QNO_CA_TREASURE_5")->status == proto::QuestStatus::Done);
    }

    SUBCASE("Saesa's revenge: three times, the second offer and acceptance in its own words") {
        Played play(Pick(coded, {"QNO_CA_WAREHOUSE_3"}));
        play.Enter();
        play.Talk("NPC_CA_WAREHOUSE", "QNO_CA_WAREHOUSE_3");
        CHECK(play.host.LastSaid() == "offer");
        play.Accept("NPC_CA_WAREHOUSE", "QNO_CA_WAREHOUSE_3");
        CHECK(play.host.LastSaid() == "accepted");
        play.host.bag["ITEM_QNO_CA_WAREHOUSE_3_02"] = 20;
        play.quests.BagChanged(1);
        play.HandIn("NPC_CA_WAREHOUSE", "QNO_CA_WAREHOUSE_3");
        CHECK(play.Record("QNO_CA_WAREHOUSE_3")->times == 1);
        play.Talk("NPC_CA_WAREHOUSE", "QNO_CA_WAREHOUSE_3");
        CHECK(play.host.LastSaid() == "offer-again");
        play.Accept("NPC_CA_WAREHOUSE", "QNO_CA_WAREHOUSE_3");
        CHECK(play.host.LastSaid() == "accepted-again");
    }

    SUBCASE("JinJin's nightmares: a sharing party's, the general's death turns the calabash, the blood asked at the hand-in") {
        std::vector<game::QuestDef> defs = Pick(coded, {"QNO_TQ_ORNAMENT_4"});
        defs.front().afterAll.clear();
        Played play(defs);
        play.Enter();
        play.Talk("NPC_CH_ACCESSORY", "QNO_TQ_ORNAMENT_4");
        CHECK(play.host.LastSaid() == "not-party");
        play.host.who.sharedParty = true;
        play.Accept("NPC_CH_ACCESSORY", "QNO_TQ_ORNAMENT_4");
        CHECK(play.host.bag["ITEM_QNO_TQ_ORNAMENT_4_04"] == 1);
        CHECK(play.host.bag["ITEM_QNO_TQ_ORNAMENT_4_01"] == 1);
        play.host.bag["ITEM_QNO_TQ_ORNAMENT_4_04"] = 0; // the stone used to go in
        play.quests.Killed(9000, "MOB_QT_TQ_SNAKEGENERAL4", 3, 1u);
        CHECK(play.host.bag["ITEM_QNO_TQ_ORNAMENT_4_01"] == 0);
        CHECK(play.host.bag["ITEM_QNO_TQ_ORNAMENT_4_02"] == 1);
        CHECK(play.Record("QNO_TQ_ORNAMENT_4")->status == proto::QuestStatus::Achieved);
        // The blood bottle lost: refused with its words.
        play.host.bag["ITEM_QNO_TQ_ORNAMENT_4_02"] = 0;
        play.Talk("NPC_CH_ACCESSORY", "QNO_TQ_ORNAMENT_4");
        CHECK(play.host.LastSaid() == "missing");
        play.host.bag["ITEM_QNO_TQ_ORNAMENT_4_02"] = 1;
        play.HandIn("NPC_CH_ACCESSORY", "QNO_TQ_ORNAMENT_4");
        CHECK(play.Record("QNO_TQ_ORNAMENT_4")->status == proto::QuestStatus::Done);
        CHECK(play.host.bag["ITEM_QNO_TQ_ORNAMENT_4_02"] == 0);
    }

    SUBCASE("The script's deliveries give their items at the start (the original's start grant)") {
        const auto script = AsiaScript();
        if (script.empty()) return;
        CHECK(script.at("QNO_CA_GORIA_1").startItems == std::vector<game::QuestGoal::Items>{{"ITEM_QNO_CA_GORIA_1_01", 1}});
        CHECK(script.at("QNO_TQ_BAEKRYOUNG_1").startItems == std::vector<game::QuestGoal::Items>{{"ITEM_QNO_TQ_BAEKRYEONG_1_01", 1}});
        CHECK(script.at("QNO_TQ_STONEBOARD_3").startItems == std::vector<game::QuestGoal::Items>{{"ITEM_QNO_TQ_STONEBOARD_3_01", 1}});
        CHECK(script.at("QNO_CH_SMITH_1").startItems == std::vector<game::QuestGoal::Items>{{"ITEM_QNO_CH_SMITH_1", 1}});
    }
}

namespace {

constexpr std::uint32_t kDoctorModel = 2031, kShamanModel = 2032, kBook = 910031;

} // namespace

TEST_CASE("Game server: Hwajung's worry (QNO_TQ_FINDTHIEF_1) taken at the doctor, the book handed back by the witch dancer, paid at the doctor") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    // Our definition of the original's coded quest, kept as ours (no client's questdata asked) with a reward of its own.
    std::vector<game::QuestDef> coded;
    REQUIRE_MESSAGE(server::DecodeCodedQuests(coded, error), error);
    const auto it = std::find_if(coded.begin(), coded.end(), [](const game::QuestDef& q) { return q.codename == "QNO_TQ_FINDTHIEF_1"; });
    REQUIRE(it != coded.end());
    game::QuestDef q = *it;
    q.id = 20264;
    q.minLevel = 1;
    q.reward.exp = 100;
    REQUIRE_MESSAGE(server::SaveQuest(t.Db(), q, server::QuestOrigin::Ours, error), error);

    core::JobSystem jobs{2};
    net::NetService net{1};
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    auto rules = std::make_shared<game::CharacterRules>();
    for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
    rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
    rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
    rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
    auto items = std::make_shared<assets::ItemTable>();
    assets::ItemRow book;
    book.refObjId = kBook;
    book.codename = "ITEM_QNO_TQ_FINDTHIEF_1_01";
    book.typeId = {3, 3, 9, 0};
    book.inService = true;
    book.maxStack = 1;
    items->Add(book);
    rules->items = items;
    config.rules = rules;
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    config.npcs.push_back({kDoctorModel, "NPC_CH_DOCTOR", {assets::RegionId(168, 98), 1000.0f, 50.0f, 458.0f}, 0.0f});
    config.npcs.push_back({kShamanModel, "NPC_CH_SHAMAN", {assets::RegionId(168, 98), 1010.0f, 50.0f, 458.0f}, 0.0f});
    std::unique_ptr<server::GameServer> game = std::make_unique<server::GameServer>();
    const std::function<void()> pump = [&] { game->Poll(2ms); };
    REQUIRE_MESSAGE(game->Start(config, error), error);

    bool ok = false;
    REQUIRE(server::CreateAccount(t.Db(), "accthief", "secret1", error) == server::CreateAccountResult::Created);
    const std::int64_t account = server::FindAccount(t.Db(), "accthief", ok)->id;
    Client c(net, game->Port(), pump);
    REQUIRE(c.Greet());
    proto::LobbyEnter e;
    e.ticket = server::IssueTicket(t.Db(), account, config.id, 30, error);
    c.Send(e);
    proto::LobbyEntered entered;
    REQUIRE(c.Expect(entered));
    proto::CharacterCreate m;
    m.name = "Seeker";
    m.model = 100;
    m.chest = 1;
    m.legs = 2;
    m.shoes = 3;
    m.weapon = 10;
    c.Send(m);
    proto::LobbyResult r;
    REQUIRE(c.Expect(r));
    REQUIRE(r.status == proto::LobbyStatus::Ok);
    const std::int64_t character = t.Db().Query("SELECT charid FROM _char WHERE charname16 = $1", {std::string("Seeker")}).Int(0, 0);
    c.Send(proto::CharacterSelect{"Seeker"});
    REQUIRE(c.Expect(r));
    c.Send(proto::WorldEnter{});
    proto::WorldEntered w;
    REQUIRE(c.Expect(w));
    REQUIRE(w.status == proto::WorldStatus::Ok);
    std::uint32_t doctor = 0, shaman = 0;
    proto::EntityAppeared seen;
    while ((doctor == 0 || shaman == 0) && c.Expect(seen)) {
        if (seen.character.model == kDoctorModel) doctor = seen.entity;
        if (seen.character.model == kShamanModel) shaman = seen.entity;
    }
    REQUIRE(doctor != 0);
    REQUIRE(shaman != 0);
    const auto got = [&]<typename T>(T) {
        T msg;
        REQUIRE(c.Expect(msg));
        return msg;
    };

    // Hwajung offers it and it is taken: nothing given at the start (the delivery brings nothing).
    c.Send(proto::QuestTalk{doctor, 20264});
    CHECK(got(proto::QuestSays{}).moment == "offer");
    c.Send(proto::QuestAccept{doctor, 20264});
    CHECK(got(proto::QuestChanged{}).state.status == proto::QuestStatus::Achieving);
    CHECK(got(proto::QuestAnswer{}).code == proto::QuestCode::Ok);
    CHECK(got(proto::QuestSays{}).moment == "accepted");

    // Miaoryeong: due there; the visit hands back the account book.
    c.Send(proto::NpcTalk{shaman});
    auto rows = got(proto::NpcQuests{});
    REQUIRE(rows.rows.size() == 1);
    CHECK(rows.rows[0].kind == proto::NpcQuestKind::HandIn);
    c.Send(proto::QuestHandIn{shaman, 20264, {}});
    const auto handed = got(proto::ItemMoved{});
    REQUIRE(handed.changed.size() == 1);
    CHECK(handed.changed[0].item == kBook);
    CHECK(got(proto::QuestChanged{}).state.status == proto::QuestStatus::Achieved);

    // Hwajung takes the book and pays.
    c.Send(proto::QuestHandIn{doctor, 20264, {}});
    CHECK(got(proto::ItemMoved{}).emptied.size() == 1);
    CHECK(got(proto::QuestGone{}).why == proto::QuestEnd::Finished);
    CHECK(got(proto::QuestRewarded{}).exp >= 100);
    for (int i = 0; i < 100; ++i) pump();
    const server::QueryResult row = t.Db().Query("SELECT status, achievementcount FROM _charquest WHERE charid = $1 AND questid = 20264", {character});
    REQUIRE(row.Rows() == 1);
    CHECK(row.Int(0, 0) == 4);
    CHECK(row.Int(0, 1) == 1);
}
