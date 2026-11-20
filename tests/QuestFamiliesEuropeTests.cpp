// The original's coded quests of Constantinople's East Europe chain and the hwan army tests (server/common
// OriginalQuestsEurope.cpp): the definitions, their chains, and their play on our quest engine.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include "ServerTestSupport.h"
#include "sr/core/Jobs.h"
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

// Whether the towns' file defines the quest (its line "quest CODE", the name ending at a space or the line's end).
bool InTowns(const std::string& code) {
    const std::string_view text = server::CodedQuestsTowns();
    const std::string line = "\nquest " + code;
    for (std::size_t at = text.find(line); at != std::string_view::npos; at = text.find(line, at + 1)) {
        const std::size_t end = at + line.size();
        if (end == text.size() || text[end] == ' ' || text[end] == '\r' || text[end] == '\n') return true;
    }
    return false;
}

bool Ours(const std::string& code) {
    // OriginalQuestsTowns.cpp's have their own test (QuestTownsTests.cpp).
    if (InTowns(code)) return false;
    for (const char* family : {"QNO_EU_EASTEU_", "QNO_EU_ADVENTURER_", "QNO_EU_GENERAL_", "QNO_EU_IVY_", "QNO_EU_WITCH_", "QNO_CH_HWAN_", "QNO_EU_HWAN_"})
        if (code.starts_with(family)) return true;
    return false;
}

std::map<std::string, game::QuestDef> CodedEurope() {
    std::vector<game::QuestDef> coded;
    std::string error;
    REQUIRE_MESSAGE(server::DecodeCodedQuests(coded, error), error);
    std::map<std::string, game::QuestDef> out;
    for (game::QuestDef& q : coded)
        if (Ours(q.codename)) out[q.codename] = std::move(q);
    return out;
}

// The script's quests (SR_QUEST_SCRIPT or SR_VSRO_DATA's Quest.sct): empty without it.
std::map<std::string, game::QuestDef> Script() {
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

// A quest's chain to its start: every quest it needs first (finished or under way), depth first, the quest itself last.
std::vector<std::string> ChainOf(const std::string& code, const std::map<std::string, game::QuestDef>& coded,
                                 const std::map<std::string, game::QuestDef>& script, std::set<std::string>& missing) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    const std::function<void(const std::string&)> walk = [&](const std::string& c) {
        if (!seen.insert(c).second) return;
        const auto it = coded.find(c);
        const game::QuestDef* q = it != coded.end() ? &it->second : nullptr;
        if (q == nullptr) {
            const auto s = script.find(c);
            if (s == script.end()) {
                missing.insert(c);
                out.push_back(c);
                return;
            }
            q = &s->second;
        }
        for (const auto* list : {&q->afterAll, &q->afterOne, &q->during})
            for (const std::string& before : *list) walk(before);
        out.push_back(c);
    };
    walk(code);
    return out;
}

} // namespace

TEST_CASE("Quest families (Europe): the East Europe chain and the hwan tests read from our definitions, and back the same") {
    const std::map<std::string, game::QuestDef> coded = CodedEurope();
    int europe = 0, hwan = 0;
    for (const auto& [code, q] : coded) (code.find("_HWAN_") != std::string::npos ? hwan : europe) += 1;
    CHECK(europe == 20);
    CHECK(hwan == 28);
    std::string error;
    for (const auto& [code, q] : coded) {
        game::QuestDef back;
        REQUIRE_MESSAGE(game::ParseQuest(game::WriteQuest(q), back, error), std::string(code + ": " + error));
        CHECK_MESSAGE(back == q, code);
        CHECK_MESSAGE(q.givers.size() == 1, code);
        CHECK_MESSAGE(!q.finisher.empty(), code);
    }
    // The original's third list: taken while another runs.
    CHECK(coded.at("QNO_EU_EASTEU_2").during == std::vector<std::string>{"QNO_EU_EASTEU_1"});
    CHECK(coded.at("QNO_EU_EASTEU_3").during == std::vector<std::string>{"QNO_EU_EASTEU_1"});
    CHECK(coded.at("QNO_EU_EASTEU_3").afterAll == std::vector<std::string>{"QNO_EU_EASTEU_2"});
    CHECK(coded.at("QNO_EU_IVY_2").during == std::vector<std::string>{"QNO_EU_IVY_1"});
    // The party quests (start flag 0x10000), each with its not-party talk.
    for (const char* code : {"QNO_EU_GENERAL_2", "QNO_EU_EASTEU_21", "QNO_EU_IVY_3"}) {
        CHECK_MESSAGE(coded.at(code).party, code);
        CHECK(std::any_of(coded.at(code).talks.begin(), coded.at(code).talks.end(), [](const game::QuestTalk& t) { return t.moment == "not-party"; }));
    }
    // The goals' kinds as the setups make them: a change of items (kind 10) counted from the bag without sources; the
    // captures whose catch follows its catcher; the champion of an instance; a helper that never holds the quest back.
    const game::QuestDef& stable = coded.at("QNO_EU_EASTEU_1");
    REQUIRE(stable.goals.size() == 2);
    CHECK(stable.goals[0].kind == game::QuestGoalKind::Kill);
    CHECK(stable.goals[0].count == 25);
    CHECK(stable.goals[1].kind == game::QuestGoalKind::Drop);
    CHECK(stable.goals[1].item == "ITEM_QNO_EU_EASTEU_1_02");
    CHECK(stable.goals[1].sources.empty());
    for (const auto& [code, cos] : std::map<std::string, std::string>{{"QNO_EU_EASTEU_5", "MOB_QT_01_DOWB_COS"},
                                                                       {"QNO_EU_EASTEU_11_1", "MOB_QT_01_LADON_COS"},
                                                                       {"QNO_EU_EASTEU_17_1", "MOB_QT_01_SIREN_CLON_COS"}}) {
        const game::QuestDef& q = coded.at(code);
        REQUIRE(q.goals.size() == 1);
        CHECK(q.goals[0].kind == game::QuestGoalKind::Capture);
        CHECK(q.goals[0].monsters == std::vector<std::string>{cos});
        CHECK(q.goals[0].npc == "NPC_EU_WITCH");
        REQUIRE(q.startItems.size() == 1);
        CHECK(q.startItems[0].count == 5);
    }
    const game::QuestDef& test1 = coded.at("QNO_CH_HWAN_1_1");
    CHECK(test1.race == "china");
    CHECK(test1.minLevel == 95);
    REQUIRE(test1.goals.size() == 1);
    CHECK(test1.goals[0].classes == std::vector<game::MonsterClass>{game::MonsterClass::Champion});
    CHECK(coded.at("QNO_EU_HWAN_1_1").race == "europe");
    CHECK(coded.at("QNO_CH_HWAN_2_1").minLevel == 100);
    const game::QuestDef& curse = coded.at("QNO_EU_WITCH_1");
    CHECK(curse.startItems == std::vector<game::QuestGoal::Items>{{"ITEM_QNO_EU_GENERAL_5_01", 60}});
    REQUIRE(curse.goals.size() == 2);
    CHECK(curse.goals[0].count == 60);
    CHECK(curse.goals[1].hidden); // the wolves' blood, up to 5,000
    CHECK(coded.at("QNO_EU_GENERAL_2").goals.size() == 2);
    CHECK(!coded.at("QNO_EU_GENERAL_2").goals[0].hidden);
    CHECK(coded.at("QNO_EU_GENERAL_2").goals[1].hidden);
    // The hwan levels come with _RefQuestReward at the import (1 and 2); the deliveries that end each test.
    CHECK(coded.at("QNO_CH_HWAN_1_6").goals[0].kind == game::QuestGoalKind::Deliver);
    CHECK(coded.at("QNO_EU_HWAN_2_6").finisher == "NPC_EU_GENERAL");
}

TEST_CASE("Quest families (Europe): each item quest's chain, to its start") {
    const std::map<std::string, game::QuestDef> coded = CodedEurope();
    const std::map<std::string, game::QuestDef> script = Script();
    // The chains, the script's links among them; without the script its quests stand alone.
    const std::map<std::string, std::vector<std::string>> chains = {
        {"QNO_EU_EASTEU_1", {"QNO_EU_EASTEU_1"}},
        {"QNO_EU_EASTEU_3", {"QNO_EU_EASTEU_1", "QNO_EU_EASTEU_2", "QNO_EU_EASTEU_3"}},
        {"QNO_EU_EASTEU_5", {"QNO_EU_EASTEU_4", "QNO_EU_EASTEU_5"}},
        {"QNO_EU_EASTEU_11_1", {"QNO_EU_EASTEU_9", "QNO_EU_EASTEU_10", "QNO_EU_EASTEU_11", "QNO_EU_EASTEU_11_1"}},
        {"QNO_EU_EASTEU_17_1", {"QNO_EU_EASTEU_15", "QNO_EU_EASTEU_16", "QNO_EU_EASTEU_17", "QNO_EU_EASTEU_17_1"}},
        {"QNO_EU_EASTEU_19", {"QNO_EU_EASTEU_19"}},
        {"QNO_EU_EASTEU_21", {"QNO_EU_EASTEU_19", "QNO_EU_EASTEU_20", "QNO_EU_EASTEU_21"}},
        {"QNO_EU_ADVENTURER_1", {"QNO_EU_ADVENTURER_1"}},
        {"QNO_EU_GENERAL_2", {"QNO_EU_GENERAL_1", "QNO_EU_GENERAL_2"}},
        {"QNO_EU_IVY_2", {"QNO_EU_PRIEST_1", "QNO_EU_GENERAL_6", "QNO_CA_HUNTER_1", "QNO_EU_IVY_1", "QNO_EU_IVY_2"}},
        {"QNO_EU_IVY_3", {"QNO_EU_PRIEST_1", "QNO_EU_GENERAL_6", "QNO_CA_HUNTER_1", "QNO_EU_IVY_1", "QNO_EU_IVY_3"}},
        {"QNO_EU_WITCH_1", {"QNO_EU_GENERAL_4", "QNO_EU_GENERAL_5", "QNO_EU_WITCH_1"}},
        {"QNO_CH_HWAN_1_3_1", {"QNO_CH_HWAN_1_1", "QNO_CH_HWAN_1_2", "QNO_CH_HWAN_1_3", "QNO_CH_HWAN_1_3_1"}},
        {"QNO_EU_HWAN_2_4", {"QNO_EU_HWAN_1_1", "QNO_EU_HWAN_1_2", "QNO_EU_HWAN_1_3", "QNO_EU_HWAN_1_3_1", "QNO_EU_HWAN_1_4", "QNO_EU_HWAN_1_4_1",
                             "QNO_EU_HWAN_1_5", "QNO_EU_HWAN_1_6", "QNO_EU_HWAN_2_1", "QNO_EU_HWAN_2_2", "QNO_EU_HWAN_2_3", "QNO_EU_HWAN_2_4"}},
        {"QNO_CH_HWAN_2_6", {"QNO_CH_HWAN_1_1", "QNO_CH_HWAN_1_2", "QNO_CH_HWAN_1_3", "QNO_CH_HWAN_1_3_1", "QNO_CH_HWAN_1_4", "QNO_CH_HWAN_1_4_1",
                             "QNO_CH_HWAN_1_5", "QNO_CH_HWAN_1_6", "QNO_CH_HWAN_2_1", "QNO_CH_HWAN_2_2", "QNO_CH_HWAN_2_3", "QNO_CH_HWAN_2_4",
                             "QNO_CH_HWAN_2_5", "QNO_CH_HWAN_2_6"}},
    };
    for (const auto& [code, want] : chains) {
        std::set<std::string> missing;
        const std::vector<std::string> chain = ChainOf(code, coded, script, missing);
        if (script.empty()) {
            // Without the script: our part of the chain, up to the first script quest.
            for (const std::string& c : chain) CHECK_MESSAGE((coded.contains(c) || missing.contains(c)), c);
            continue;
        }
        CHECK_MESSAGE(missing.empty(), code);
        CHECK_MESSAGE(chain == want, code);
        // Each chain's start needs nothing first.
        const std::string& first = chain.front();
        const game::QuestDef& start = coded.contains(first) ? coded.at(first) : script.at(first);
        CHECK_MESSAGE((start.afterAll.empty() && start.afterOne.empty() && start.during.empty()), first);
    }
}

namespace {

// A made-up host for the rules: one European player, its bag by codename, NPCs near by entity.
struct Host : server::QuestHost {
    Player who{1, 77, "Hero", 20, "europe", 0, true};
    std::map<std::string, std::uint32_t> bag;
    std::map<std::uint32_t, std::string> npcs;
    std::vector<std::pair<proto::MessageId, std::vector<std::byte>>> sent;
    Reward rewarded;
    std::optional<Player> QuestPlayer(std::uint32_t entity) override { return entity == 1 ? std::optional(who) : std::nullopt; }
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
    bool QuestRoom(std::uint32_t, const std::vector<game::QuestGoal::Items>&) override { return true; }
    bool QuestTakeGold(std::uint32_t, std::uint64_t) override { return true; }
    void QuestReward(std::uint32_t, const Reward& r) override { rewarded = r; }
    void QuestDrop(std::uint32_t, std::uint32_t, const std::string& item) override { bag[item] += 1; }
    void QuestSave(std::int64_t, const server::QuestRecord&) override {}
    bool QuestThemeDone(std::uint32_t, const std::string&) override { return false; }

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
};

template <typename T>
void Ask(server::Quests& quests, const T& message) {
    REQUIRE(quests.Handle(1, T::kId, proto::Encode(message)));
}

constexpr std::uint32_t kWitch = 601, kPriest = 602, kUvetino = 603, kGeneral = 604, kAdventurer = 605;

// Our definitions with numbers, as the import gives them (_RefQuest's ids).
std::vector<game::QuestDef> Numbered(const std::map<std::string, game::QuestDef>& coded) {
    std::vector<game::QuestDef> out;
    std::uint32_t id = 30000;
    for (const auto& [code, q] : coded) {
        out.push_back(q);
        out.back().id = ++id;
    }
    return out;
}

bool Offers(Host& host, server::Quests& quests, std::uint32_t npc, std::uint32_t quest) {
    quests.TellNpc(1, npc);
    for (const auto& row : host.Last<proto::NpcQuests>().rows)
        if (row.quest == quest && row.kind == proto::NpcQuestKind::Offer) return true;
    return false;
}

} // namespace

TEST_CASE("Quest families (Europe): the stable's purification taken, the holy water's two quests while it runs, all three finished") {
    const std::map<std::string, game::QuestDef> coded = CodedEurope();
    Host host;
    host.who.level = 11;
    host.npcs = {{kWitch, "NPC_EU_WITCH"}, {kPriest, "NPC_EU_PRIEST"}, {kUvetino, "NPC_EU_UNION2"}};
    server::Quests quests(host, {1.0, 1.0});
    const std::vector<game::QuestDef> defs = Numbered(coded);
    quests.Load(defs);
    const auto id = [&](const char* code) { return quests.Find(code)->id; };
    const std::uint32_t stable = id("QNO_EU_EASTEU_1"), hat = id("QNO_EU_EASTEU_2"), horns = id("QNO_EU_EASTEU_3");
    quests.Enter(1, {});

    // The priest offers nothing before the stable's quest runs (its list); the witch offers it.
    CHECK(!Offers(host, quests, kPriest, hat));
    CHECK(Offers(host, quests, kWitch, stable));
    Ask(quests, proto::QuestAccept{kWitch, stable});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    // Now the priest's: Uvetino's hat into the bag, brought to him, done there.
    CHECK(Offers(host, quests, kPriest, hat));
    CHECK(!Offers(host, quests, kUvetino, horns)); // needs the hat's quest finished
    Ask(quests, proto::QuestAccept{kPriest, hat});
    CHECK(host.bag["ITEM_QNO_EU_EASTEU_2_01"] == 1);
    Ask(quests, proto::QuestTalk{kUvetino, hat});
    CHECK(host.Last<proto::QuestSays>().choice == proto::QuestChoice::HandIn);
    Ask(quests, proto::QuestHandIn{kUvetino, hat, {}});
    CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::Finished);
    CHECK(host.bag["ITEM_QNO_EU_EASTEU_2_01"] == 0);
    // Uvetino's horns, then his holy water (the import's reward; here put in the bag by hand).
    CHECK(Offers(host, quests, kUvetino, horns));
    Ask(quests, proto::QuestAccept{kUvetino, horns});
    const std::uint32_t me[] = {1};
    for (int i = 0; i < 20; ++i) quests.Killed(9000 + i, "MOB_EU_DOWB_CLON", 0, me); // 20 % each: the drops land in the bag
    host.bag["ITEM_QNO_EU_EASTEU_3_01"] = 20;
    quests.BagChanged(1);
    CHECK(quests.Record(1, horns)->status == proto::QuestStatus::Achieved);
    Ask(quests, proto::QuestHandIn{kUvetino, horns, {}});
    CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::Finished);
    CHECK(host.bag["ITEM_QNO_EU_EASTEU_3_01"] == 0);
    host.bag["ITEM_QNO_EU_EASTEU_3_02"] = 10;

    // The stable: 25 sentries and 5 filths (the holy water's search gives them: the item rule's, here by hand).
    for (int i = 0; i < 25; ++i) quests.Killed(9100 + i, "MOB_QT_01_BAROI", 0, me);
    CHECK(quests.Record(1, stable)->progress[0] == 25);
    CHECK(quests.Record(1, stable)->status == proto::QuestStatus::Achieving);
    host.bag["ITEM_QNO_EU_EASTEU_1_02"] = 5;
    quests.BagChanged(1);
    CHECK(quests.Record(1, stable)->status == proto::QuestStatus::Achieved);
    Ask(quests, proto::QuestTalk{kWitch, stable});
    CHECK(host.Last<proto::QuestSays>().moment == "done");
    Ask(quests, proto::QuestHandIn{kWitch, stable, {}});
    CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::Finished);
    CHECK(host.bag["ITEM_QNO_EU_EASTEU_1_02"] == 0);
    // Once the stable's quest is done, the priest's is no longer offered (it runs only while the stable's does).
    CHECK(!Offers(host, quests, kPriest, hat));
}

TEST_CASE("Quest families (Europe): a party quest's not-party words, a helper gathering that never holds a quest back") {
    const std::map<std::string, game::QuestDef> coded = CodedEurope();
    Host host;
    host.who.level = 30;
    host.npcs = {{kGeneral, "NPC_EU_GENERAL"}, {kAdventurer, "NPC_EU_ADVENTURER"}};
    server::Quests quests(host, {1.0, 1.0});
    std::vector<server::QuestRecord> records;
    const std::vector<game::QuestDef> defs = Numbered(coded);
    quests.Load(defs);
    const std::uint32_t pirates = quests.Find("QNO_EU_GENERAL_1")->id, spoil = quests.Find("QNO_EU_GENERAL_2")->id,
                        crab = quests.Find("QNO_EU_ADVENTURER_1")->id;
    server::QuestRecord done;
    done.quest = pirates;
    done.status = proto::QuestStatus::Done;
    done.times = 1;
    quests.Enter(1, {done});

    // Alone: the general lists it, says the not-party words, refuses it.
    quests.TellNpc(1, kGeneral);
    const auto rows = host.Last<proto::NpcQuests>().rows;
    CHECK(std::any_of(rows.begin(), rows.end(), [&](const auto& r) { return r.quest == spoil; }));
    Ask(quests, proto::QuestTalk{kGeneral, spoil});
    CHECK(host.Last<proto::QuestSays>().moment == "not-party");
    Ask(quests, proto::QuestAccept{kGeneral, spoil});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::NotOffered);
    // In a party sharing experience and items: offered and taken.
    host.who.sharedParty = true;
    Ask(quests, proto::QuestTalk{kGeneral, spoil});
    CHECK(host.Last<proto::QuestSays>().moment == "offer");
    Ask(quests, proto::QuestAccept{kGeneral, spoil});
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    // The commander's spoil alone finishes it; the pirate flag (a helper) is neither needed nor taken.
    host.bag["ITEM_QNO_EU_GENERAL_2_01"] = 1;
    host.bag["ITEM_QNO_EU_GENERAL_2_03"] = 1;
    quests.BagChanged(1);
    CHECK(quests.Record(1, spoil)->status == proto::QuestStatus::Achieved);
    Ask(quests, proto::QuestHandIn{kGeneral, spoil, {}});
    CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::Finished);
    CHECK(host.bag["ITEM_QNO_EU_GENERAL_2_03"] == 0);

    // Demetri's crabs: the five eggs finish it without the shiny moss (the helper the eggs' crabs need), three times.
    Ask(quests, proto::QuestAccept{kAdventurer, crab});
    host.bag["ITEM_QNO_EU_ADVENTURER_1_02"] = 5;
    quests.BagChanged(1);
    CHECK(quests.Record(1, crab)->status == proto::QuestStatus::Achieved);
    Ask(quests, proto::QuestHandIn{kAdventurer, crab, {}});
    CHECK(quests.Record(1, crab)->times == 1);
    Ask(quests, proto::QuestTalk{kAdventurer, crab});
    CHECK(host.Last<proto::QuestSays>().moment == "offer-again");
}

TEST_CASE("Quest families (Europe): the hwan tests in turn; a test's end raises the hwan level") {
    const std::map<std::string, game::QuestDef> coded = CodedEurope();
    Host host;
    host.who.level = 100;
    host.who.race = "china";
    constexpr std::uint32_t kGeneralCh = 701, kShaman = 702;
    host.npcs = {{kGeneralCh, "NPC_CH_GENARAL"}, {kShaman, "NPC_CH_SHAMAN"}};
    server::Quests quests(host, {1.0, 1.0});
    std::vector<game::QuestDef> defs = Numbered(coded);
    for (game::QuestDef& q : defs) { // _RefQuestReward's Hwan, as the import puts it in
        if (q.codename.ends_with("_HWAN_1_6")) q.reward.hwan = 1;
        if (q.codename.ends_with("_HWAN_2_6")) q.reward.hwan = 2;
    }
    quests.Load(defs);
    const auto id = [&](const char* code) { return quests.Find(code)->id; };
    // A European does not get China's first test; a Chinese does.
    host.who.race = "europe";
    quests.Enter(1, {});
    CHECK(!Offers(host, quests, kGeneralCh, id("QNO_CH_HWAN_1_1")));
    host.who.race = "china";
    CHECK(Offers(host, quests, kGeneralCh, id("QNO_CH_HWAN_1_1")));
    // The first test: its champion beaten (in its instance), finished at the general; the next offered.
    Ask(quests, proto::QuestAccept{kGeneralCh, id("QNO_CH_HWAN_1_1")});
    const std::uint32_t me[] = {1};
    quests.Killed(9500, "MOB_QT_CH_SOLDIER_01_1", 0, me); // a normal one does not count
    CHECK(quests.Record(1, id("QNO_CH_HWAN_1_1"))->progress[0] == 0);
    quests.Killed(9501, "MOB_QT_CH_SOLDIER_01_1", 1, me);
    Ask(quests, proto::QuestHandIn{kGeneralCh, id("QNO_CH_HWAN_1_1"), {}});
    CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::Finished);
    CHECK(Offers(host, quests, kGeneralCh, id("QNO_CH_HWAN_1_2")));
    CHECK(!Offers(host, quests, kGeneralCh, id("QNO_CH_HWAN_2_1")));
    // The second test's last step: the piece of spirit handed over at the start, given to the general: hwan level 2.
    std::vector<server::QuestRecord> before;
    for (const char* code : {"QNO_CH_HWAN_1_1", "QNO_CH_HWAN_1_2", "QNO_CH_HWAN_1_3", "QNO_CH_HWAN_1_3_1", "QNO_CH_HWAN_1_4", "QNO_CH_HWAN_1_4_1",
                             "QNO_CH_HWAN_1_5", "QNO_CH_HWAN_1_6", "QNO_CH_HWAN_2_1", "QNO_CH_HWAN_2_2", "QNO_CH_HWAN_2_3", "QNO_CH_HWAN_2_4",
                             "QNO_CH_HWAN_2_5"}) {
        server::QuestRecord r;
        r.quest = id(code);
        r.status = proto::QuestStatus::Done;
        r.times = 1;
        before.push_back(r);
    }
    host.who.hwan = 1;
    quests.Leave(1);
    quests.Enter(1, before);
    const std::uint32_t last = id("QNO_CH_HWAN_2_6");
    CHECK(Offers(host, quests, kShaman, last));
    Ask(quests, proto::QuestAccept{kShaman, last});
    CHECK(host.bag["ITEM_QNO_CH_HWAN_2_5_02"] == 1);
    Ask(quests, proto::QuestTalk{kGeneralCh, last});
    CHECK(host.Last<proto::QuestSays>().choice == proto::QuestChoice::HandIn);
    Ask(quests, proto::QuestHandIn{kGeneralCh, last, {}});
    CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::Finished);
    CHECK(host.rewarded.hwan == 2);
}

namespace {

constexpr std::uint32_t kGeneralModel = 2031, kShamanModel = 2032, kPiece = 920001;

// Our game server with China's general and shaman by the start place.
struct HwanRig {
    TestDatabase& t;
    core::JobSystem jobs{2};
    std::unique_ptr<server::GameServer> game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::function<void()> pump;
    std::string error;
    explicit HwanRig(TestDatabase& db) : t(db) {
        config.listen = Local();
        config.databaseUrl = t.Url();
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
        rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
        rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
        rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
        auto items = std::make_shared<assets::ItemTable>();
        assets::ItemRow piece;
        piece.refObjId = kPiece;
        piece.codename = "ITEM_QNO_CH_HWAN_2_5_02";
        piece.typeId = {3, 3, 9, 0};
        piece.inService = true;
        piece.maxStack = 1;
        items->Add(piece);
        rules->items = items;
        config.rules = rules;
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        config.npcs.push_back({kGeneralModel, "NPC_CH_GENARAL", {assets::RegionId(168, 98), 1000.0f, 50.0f, 458.0f}, 0.0f});
        config.npcs.push_back({kShamanModel, "NPC_CH_SHAMAN", {assets::RegionId(168, 98), 1010.0f, 50.0f, 458.0f}, 0.0f});
        pump = [this] { game->Poll(2ms); };
    }
};

template <typename T>
T Next(Client& c) {
    T m;
    REQUIRE(c.Expect(m));
    return m;
}

} // namespace

TEST_CASE("Game server: China's second hwan test finished at the general raises the hwan level, kept") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    // Our definitions in the database with their numbers, the two tests' ends with _RefQuestReward's Hwan.
    std::map<std::string, std::uint32_t> ids;
    {
        std::uint32_t id = 30000;
        for (auto [code, q] : CodedEurope()) {
            q.id = ++id;
            ids[code] = q.id;
            if (code.ends_with("_HWAN_1_6")) q.reward.hwan = 1;
            if (code.ends_with("_HWAN_2_6")) q.reward.hwan = 2;
            REQUIRE_MESSAGE(server::SaveQuest(t.Db(), q, server::QuestOrigin::Script, error), error);
        }
    }
    HwanRig rig(t);
    rig.game = std::make_unique<server::GameServer>();
    REQUIRE_MESSAGE(rig.game->Start(rig.config, error), error);

    // A character at level 100, hwan level 1, every step before the last finished.
    bool ok = false;
    REQUIRE(server::CreateAccount(t.Db(), "accTester", "secret1", error) == server::CreateAccountResult::Created);
    const std::int64_t account = server::FindAccount(t.Db(), "accTester", ok)->id;
    Client c(rig.net, rig.game->Port(), rig.pump);
    REQUIRE(c.Greet());
    proto::LobbyEnter enter;
    enter.ticket = server::IssueTicket(t.Db(), account, rig.config.id, 30, error);
    c.Send(enter);
    Next<proto::LobbyEntered>(c);
    proto::CharacterCreate create;
    create.name = "Tester";
    create.model = 100;
    create.chest = 1;
    create.legs = 2;
    create.shoes = 3;
    create.weapon = 10;
    c.Send(create);
    REQUIRE(Next<proto::LobbyResult>(c).status == proto::LobbyStatus::Ok);
    const std::int64_t character = t.Db().Query("SELECT charid FROM _char WHERE charname16 = 'Tester'").Int(0, 0);
    REQUIRE(t.Db().Query("UPDATE _char SET curlevel = 100, maxlevel = 100, hwanlevel = 1 WHERE charid = $1", {character}).Ok());
    for (const auto& [code, id] : ids) {
        if (!code.starts_with("QNO_CH_HWAN_") || code == "QNO_CH_HWAN_2_6") continue;
        REQUIRE(t.Db().Query("INSERT INTO _charquest (charid, questid, status, achievementcount, starttime, endtime, questdata1, questdata2) "
                             "VALUES ($1, $2, 4, 1, now(), now(), 0, 0)",
                             {character, std::int64_t(id)})
                    .Ok());
    }
    c.Send(proto::CharacterSelect{"Tester"});
    Next<proto::LobbyResult>(c);
    c.Send(proto::WorldEnter{});
    REQUIRE(Next<proto::WorldEntered>(c).status == proto::WorldStatus::Ok);
    Next<proto::InventoryList>(c);
    const auto list = Next<proto::QuestList>(c);
    CHECK(list.finished.size() == 13);
    std::uint32_t general = 0, shaman = 0;
    proto::EntityAppeared seen;
    while ((general == 0 || shaman == 0) && c.Expect(seen)) {
        if (seen.character.model == kGeneralModel) general = seen.entity;
        if (seen.character.model == kShamanModel) shaman = seen.entity;
    }
    REQUIRE(general != 0);
    REQUIRE(shaman != 0);

    // Taken at the shaman (the piece into the bag), handed to the general: the hwan level 2, saved.
    const std::uint32_t last = ids.at("QNO_CH_HWAN_2_6");
    c.Send(proto::QuestAccept{shaman, last});
    CHECK(Next<proto::ItemMoved>(c).changed.at(0).item == kPiece);
    CHECK(Next<proto::QuestAnswer>(c).code == proto::QuestCode::Ok);
    c.Send(proto::QuestHandIn{general, last, {}});
    CHECK(Next<proto::QuestGone>(c).why == proto::QuestEnd::Finished);
    for (int i = 0; i < 100; ++i) rig.pump();
    CHECK(t.Db().Query("SELECT hwanlevel FROM _char WHERE charid = $1", {character}).Int(0, 0) == 2);
    const server::QueryResult row = t.Db().Query("SELECT status, achievementcount FROM _charquest WHERE charid = $1 AND questid = $2",
                                                 {character, std::int64_t(last)});
    REQUIRE(row.Rows() == 1);
    CHECK(row.Int(0, 0) == 4);
}
