// The quests' own instances entered at an NPC's talk and the last item uses of the instance quests: the `trial` line
// (the hwan army tests: the tests of strength and the rituals) and
// its run on a made-up host; IVY_3's Horn Trumpet and Punisher's Blood; the party instances' opening and the order of their
// words; EASTEU_21 on our game server with the game files (a party in, Cerberus, his death ending it).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/InstanceProtocol.h"
#include "sr/game/Party.h"
#include "sr/game/Quest.h"
#include "sr/game/QuestItems.h"
#include "sr/server/GameServer.h"
#include "sr/server/Import.h"
#include "sr/server/QuestScript.h"
#include "sr/server/QuestStore.h"
#include "sr/server/Quests.h"
#include "sr/server/Store.h"

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

std::map<std::string, game::QuestDef> Coded() {
    std::vector<game::QuestDef> coded;
    std::string error;
    REQUIRE_MESSAGE(server::DecodeCodedQuests(coded, error), error);
    std::map<std::string, game::QuestDef> out;
    for (game::QuestDef& q : coded) out[q.codename] = std::move(q);
    return out;
}

server::QuestRecord Record(std::uint32_t quest, proto::QuestStatus status, std::uint16_t times = 0) {
    server::QuestRecord r;
    r.quest = quest;
    r.status = status;
    r.times = times;
    return r;
}

// The distance on the ground between two places, their layers aside.
double Flat(proto::WorldPosition a, proto::WorldPosition b) {
    a.layer = b.layer = 0;
    const proto::FlatPosition fa = proto::ToFlat(a), fb = proto::ToFlat(b);
    return std::hypot(fa.x - fb.x, fa.z - fb.z);
}

// One player (entity 1) and its party's members: their places, the bag (everyone's), what each was told, the raised
// monsters, the layers.
struct TrialHost : server::QuestHost {
    Player who{1, 77, "Tester", 100, "china", 0, true};
    std::map<std::uint32_t, Player> others;
    std::map<std::uint32_t, std::uint32_t> layers;
    std::map<std::string, std::uint32_t> bag;
    std::map<std::uint32_t, std::string> npcs{{7, "NPC_CH_GENARAL"}, {8, "NPC_CH_SHAMAN"}, {9, "NPC_EU_GENERAL"}, {10, "NPC_CH_GENARAL_SP"}};
    std::vector<std::pair<std::uint32_t, std::string>> scenes;
    std::vector<std::pair<std::uint32_t, std::string>> told;
    std::vector<std::pair<proto::MessageId, std::vector<std::byte>>> sent;
    bool transport = false, master = true, freePlace = true;
    std::vector<std::uint32_t> party;
    std::optional<proto::WorldPosition> enteredAt;
    std::uint32_t nextLayer = 40;
    struct Raised {
        std::string monster;
        proto::WorldPosition at;
        bool gone = false, standing = true;
    };
    std::vector<Raised> raised;
    std::optional<Player> QuestPlayer(std::uint32_t entity) override {
        std::optional<Player> p;
        if (entity == 1) p = who;
        else if (others.contains(entity)) p = others[entity];
        if (p) p->position.layer = layers.contains(entity) ? layers[entity] : 0;
        return p;
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
    bool QuestRoom(std::uint32_t, const std::vector<game::QuestGoal::Items>&) override { return true; }
    bool QuestTakeGold(std::uint32_t, std::uint64_t) override { return true; }
    void QuestReward(std::uint32_t, const Reward&) override {}
    void QuestDrop(std::uint32_t, std::uint32_t, const std::string& item) override { bag[item] += 1; }
    void QuestSave(std::int64_t, const server::QuestRecord&) override {}
    bool QuestThemeDone(std::uint32_t, const std::string&) override { return false; }
    bool QuestFreePlace(std::uint32_t) override { return freePlace; }
    void QuestSay(std::uint32_t entity, const std::string& key) override { told.push_back({entity, key}); }
    std::optional<std::size_t> QuestSpawn(std::uint32_t, const std::string& codename, const proto::WorldPosition& at) override {
        raised.push_back({codename, at});
        return raised.size() - 1;
    }
    void QuestDespawn(std::size_t nest) override { raised.at(nest).gone = true; }
    int QuestSpawnStanding(std::size_t nest) override { return nest < raised.size() && !raised[nest].gone && raised[nest].standing ? 1 : 0; }
    std::vector<std::uint32_t> QuestPartyHere(std::uint32_t) override { return party; }
    bool QuestPartyMaster(std::uint32_t entity) override { return entity == 1 && master; }
    // In a party sharing both (who.sharedParty): a kill's share among the killer alone here.
    KillParty QuestKillParty(std::uint32_t killer) override {
        if (!who.sharedParty) return {};
        return {7, {{killer, who.position}}};
    }
    bool QuestTransportOut(std::uint32_t entity) override { return entity == 1 && transport; }
    std::uint32_t QuestWorldOpen(std::uint32_t, const std::string&) override { return nextLayer++; }
    bool QuestWorldEnter(std::uint32_t entity, std::uint32_t layer) override {
        layers[entity] = layer;
        return true;
    }
    bool QuestWorldEnterAt(std::uint32_t entity, std::uint32_t layer, const proto::WorldPosition& at) override {
        enteredAt = at;
        layers[entity] = layer;
        return true;
    }
    void QuestWorldLeave(std::uint32_t entity) override { layers[entity] = 0; }
    void QuestScene(std::uint32_t entity, const std::string& name) override { scenes.push_back({entity, name}); }
    struct Way {
        proto::WorldPosition at;
        std::optional<proto::WorldPosition> to;
    };
    std::vector<Way> ways;
    std::uint32_t QuestWayOut(const proto::WorldPosition& at, const std::optional<proto::WorldPosition>& to) override {
        ways.push_back({at, to});
        return std::uint32_t(ways.size());
    }

    bool Told(std::uint32_t entity, const std::string& key) const { return std::find(told.begin(), told.end(), std::pair{entity, key}) != told.end(); }
    std::size_t Count(const std::string& monster) const {
        return std::size_t(std::count_if(raised.begin(), raised.end(), [&](const Raised& r) { return r.monster == monster; }));
    }
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
bool Ask(server::Quests& quests, const T& message) {
    return quests.Handle(1, T::kId, proto::Encode(message));
}

// The coded definitions numbered (the import's _RefQuest ids).
std::vector<game::QuestDef> Numbered(const std::map<std::string, game::QuestDef>& coded) {
    std::vector<game::QuestDef> out;
    std::uint32_t id = 30000;
    for (const auto& [code, q] : coded) {
        out.push_back(q);
        out.back().id = ++id;
    }
    return out;
}

} // namespace

TEST_CASE("Quest trial: the trial line, a berserk kill and the new use parts read and write back the same; the hwan tests' trials") {
    const game::QuestDef q = ParseOne(
        "quest QNO_T_TRIAL\nid 20150\nstart level 1-\n"
        "trial INS_QNO_T at NPC_T_GENERAL row @SN_T_ROW start 22966 1043 52 1087 refuse @SN_T_REFUSE no-job after 10 boss MOB_T_BOSS 22966 893 52 1087 "
        "radius 90 100 minutes 10 warn 5 @SN_T_WARN adds MOB_T_ADD 22966 893 52 1087 radius 90 100 every 30 pass-on-time passed @SN_T_PASSED "
        "lost @SN_T_LOST berserk take ITEM_T_CHARM give ITEM_T_PIECE failed @SN_T_FAILED out 10\n"
        "on use ITEM_T_HORN status 1 7 8 party @SN_T_PARTY site 27237 1057 218 634 1000 ITEM_T_SIGN MOB_T_KEEPER radius 20 100 life 300 away @SN_T_AWAY\n"
        "on use ITEM_T_BLOOD room need ITEM_T_A ITEM_T_B give ITEM_T_KEY\n"
        "on use ITEM_T_KEY status 1 7 8 place-first party at 25183 1882 -169 632 600 instance INS_QNO_T2 unique MOB_T_U\n"
        "on talk NPC_T_GENERAL row @SN_T_ROW2 ask @SN_T_ASK start -32765 -116 -17 15035 instance INS_QNO_T3 general MOB_T_G1 says @SN_T_G1 "
        "general MOB_T_G2 dies @SN_T_G2 next 10 near 90 100 first-near 190 200 way-out 25255 594 0 233\n"
        "goal kill 1 MOB_T_BOSS class champion berserk text @SN_T_GOAL\n"
        "talk NPC_T_GENERAL trial @SN_T_SAYS\n");
    REQUIRE(q.trial);
    CHECK(q.trial->npc == "NPC_T_GENERAL");
    CHECK(q.trial->start.region == 22966);
    CHECK(q.trial->bossAt.x == 893.0f);
    CHECK(q.trial->near == 90.0f);
    REQUIRE(q.trial->adds);
    CHECK(q.trial->adds->every == 30);
    CHECK(q.trial->warnings.size() == 1);
    CHECK(q.trial->noJob);
    CHECK(q.trial->passOnTime);
    CHECK(q.trial->berserk);
    CHECK(q.trial->out == 10);
    CHECK(q.goals[0].berserk);
    REQUIRE(q.onUse.size() == 4);
    REQUIRE(q.onUse[0].sites.size() == 1);
    CHECK(q.onUse[0].sites[0].item == "ITEM_T_SIGN");
    CHECK(q.onUse[0].sites[0].spawn.monster == "MOB_T_KEEPER");
    CHECK(q.onUse[0].sites[0].spawn.seconds == 300);
    CHECK(q.onUse[1].room);
    CHECK(q.onUse[1].need == std::vector<std::string>{"ITEM_T_A", "ITEM_T_B"});
    CHECK(q.onUse[2].placeFirst);
    // The talk rule: its NPC, row, question and start; its instance's two distances and its way out.
    const game::QuestDef::UseRule& door = q.onUse[3];
    CHECK(door.item.empty());
    CHECK(door.npc == "NPC_T_GENERAL");
    CHECK(door.row.key == "SN_T_ROW2");
    CHECK(door.ask.key == "SN_T_ASK");
    REQUIRE(door.start);
    CHECK(door.start->region == 0x8003);
    REQUIRE(door.instance);
    CHECK(door.instance->generals.size() == 2);
    CHECK(door.instance->firstNear == 190.0f);
    CHECK(door.instance->firstFar == 200.0f);
    CHECK(door.instance->near == 90.0f);
    CHECK(door.instance->far == 100.0f);
    CHECK(door.instance->wayOut);
    REQUIRE(door.instance->out);
    CHECK(door.instance->out->region == 25255);
    game::QuestDef again;
    std::string error;
    REQUIRE_MESSAGE(game::ParseQuest(game::WriteQuest(q), again, error), error);
    CHECK(again == q);
    // A trial without its boss or its NPC is no trial; a talk rule without its instance, row or start is none either.
    CHECK_FALSE(game::ParseQuest("quest QNO_T\ntrial INS_QNO_T at NPC_T\n", again, error));
    CHECK_FALSE(game::ParseQuest("quest QNO_T\non talk NPC_T row @SN_T_ROW start 1 2 3 4\n", again, error));
    CHECK_FALSE(game::ParseQuest("quest QNO_T\non use ITEM_T row @SN_T_ROW instance INS_QNO_T\n", again, error));

    // The eight hwan tests (their teleports, their worlds' updates; _RefInstance_World_Start_Pos).
    const auto coded = Coded();
    struct Expect {
        const char* quest;
        const char* npc;
        const char* world;
        std::uint16_t start;
        const char* boss;
        float bossX;
        std::uint32_t after;
        bool ritual;
        std::uint32_t every;
    };
    for (const Expect& e : {Expect{"QNO_CH_HWAN_1_1", "NPC_CH_GENARAL", "INS_QNO_CH_HWAN_1_1", 22966, "MOB_QT_CH_SOLDIER_01_1", 893, 10, false, 30},
                            Expect{"QNO_EU_HWAN_1_1", "NPC_EU_GENERAL", "INS_QNO_EU_HWAN_1_1", 22454, "MOB_QT_EU_SOLDIER_01_1", 1102, 10, false, 30},
                            Expect{"QNO_CH_HWAN_2_1", "NPC_CH_GENARAL", "INS_QNO_CH_HWAN_2_1", 22966, "MOB_QT_CH_SOLDIER_02_1", 893, 10, false, 600},
                            Expect{"QNO_EU_HWAN_2_1", "NPC_EU_GENERAL", "INS_QNO_EU_HWAN_2_1", 22454, "MOB_QT_EU_SOLDIER_02_1", 1102, 10, false, 600},
                            Expect{"QNO_CH_HWAN_1_5", "NPC_CH_SHAMAN", "INS_QNO_CH_HWAN_1_5", 25253, "MOB_QT_CH_CHARGHOST_01", 177, 12, true, 0},
                            Expect{"QNO_EU_HWAN_1_5", "NPC_EU_WITCH", "INS_QNO_EU_HWAN_1_5", 27728, "MOB_QT_CH_CHARGHOST_01", 1752, 12, true, 0},
                            Expect{"QNO_CH_HWAN_2_5", "NPC_CH_SHAMAN", "INS_QNO_CH_HWAN_2_5", 25253, "MOB_QT_CH_CHARGHOST_02", 177, 12, true, 0},
                            Expect{"QNO_EU_HWAN_2_5", "NPC_EU_WITCH", "INS_QNO_EU_HWAN_2_5", 27728, "MOB_QT_CH_CHARGHOST_02", 1752, 12, true, 0}}) {
        CAPTURE(e.quest);
        REQUIRE(coded.contains(e.quest));
        const game::QuestDef& h = coded.at(e.quest);
        REQUIRE(h.trial);
        const game::QuestDef::Trial& t = *h.trial;
        CHECK(t.npc == e.npc);
        CHECK(t.world == e.world);
        CHECK(game::QuestWorldId(t.world) >= 32);
        CHECK(game::QuestWorldId(t.world) <= 39);
        CHECK(t.start.region == e.start);
        CHECK(t.boss == e.boss);
        CHECK(t.bossAt.x == e.bossX);
        CHECK(t.after == e.after);
        CHECK(t.minutes == 10);
        CHECK(t.refuse.key == "SN_TALK_QNO_CH_HWAN_1_1_10"); // every test says China's first test's words
        CHECK(t.berserk == e.ritual);
        CHECK(t.passOnTime == !e.ritual);
        CHECK(t.noJob == !e.ritual);
        CHECK(h.goals[0].berserk == e.ritual);
        CHECK(t.adds.has_value() == !e.ritual);
        if (t.adds) CHECK(t.adds->every == e.every);
        if (e.ritual) {
            CHECK(t.far == 0.0f);
            CHECK(t.out == 10);
            CHECK(t.give.ends_with("_02"));
            REQUIRE(h.needItems.size() == 1);
            CHECK(h.needItems[0].item == t.give);
        }
        CHECK(std::any_of(h.talks.begin(), h.talks.end(), [&](const game::QuestTalk& talk) { return talk.npc == t.npc && talk.moment == "trial"; }));
    }
}

TEST_CASE("Quest trial: the general's row sends the player into its arena; the general, his soldiers, the warning; passed, lost (a made-up host)") {
    const auto coded = Coded();
    TrialHost host;
    host.who.position = {proto::WorldPosition{25000, 100.0f, 0.0f, 100.0f, 0}};
    server::Quests quests(host, {});
    quests.Load(Numbered(coded));
    const std::uint32_t test = quests.Find("QNO_CH_HWAN_1_1")->id;
    quests.Enter(1, {Record(test, proto::QuestStatus::Achieving)});
    // Under way, its goal not met: the general's words and the row (ours: the hand-in's answer).
    REQUIRE(Ask(quests, proto::QuestTalk{7, test}));
    CHECK(host.Last<proto::QuestSays>().moment == "trial");
    CHECK(host.Last<proto::QuestSays>().choice == proto::QuestChoice::HandIn);
    // A transport out, a job's suit worn: refused with China's first test's words.
    host.transport = true;
    REQUIRE(Ask(quests, proto::QuestHandIn{7, test, {}}));
    CHECK(host.Told(1, "SN_TALK_QNO_CH_HWAN_1_1_10"));
    CHECK(quests.Trials().empty());
    host.transport = false;
    host.who.jobSuit = true;
    host.told.clear();
    REQUIRE(Ask(quests, proto::QuestHandIn{7, test, {}}));
    CHECK(host.Told(1, "SN_TALK_QNO_CH_HWAN_1_1_10"));
    CHECK(quests.Trials().empty());
    host.who.jobSuit = false;
    // In: alone, at the arena's start (_RefInstance_World_Start_Pos 36: region 22966 (1043, 52, 1087)).
    REQUIRE(Ask(quests, proto::QuestHandIn{7, test, {}}));
    CHECK(host.Last<proto::QuestAnswer>().code == proto::QuestCode::Ok);
    REQUIRE(quests.Trials().size() == 1);
    const std::uint32_t layer = quests.Trials()[0].second;
    CHECK(host.layers[1] == layer);
    REQUIRE(host.enteredAt);
    CHECK(host.enteredAt->region == 22966);
    CHECK(host.enteredAt->x == 1043.0f);
    // Not twice at once.
    REQUIRE(Ask(quests, proto::QuestHandIn{7, test, {}}));
    CHECK(quests.Trials().size() == 1);
    const auto start = std::chrono::steady_clock::now();
    quests.Tick(start + 9s);
    CHECK(host.raised.empty());
    // 10 s on: the general 90..100 from his place, in the layer; the gauge's ten minutes; the first soldier.
    quests.Tick(start + 11s);
    REQUIRE(host.Count("MOB_QT_CH_SOLDIER_01_1") == 1);
    const proto::WorldPosition general{22966, 893.0f, 52.0f, 1087.0f, 0};
    CHECK(host.raised[0].at.layer == layer);
    CHECK(Flat(host.raised[0].at, general) <= 100.5); // the quest service's banded spawn within a radius of 90..100
    CHECK(quests.Gauge(1, test) > 500);
    quests.Tick(start + 12s);
    CHECK(host.Count("MOB_QT_CH_SOLDIER_01_2") == 1);

    SUBCASE("the time: a soldier every 30 s for five minutes, the warning, passed at ten minutes, out") {
        for (auto at = start + 13s; at < start + 11s + 10min; at += 1s) quests.Tick(at);
        CHECK(host.Told(1, "SN_TALK_QNO_CH_HWAN_1_1_11"));
        const std::size_t soldiers = host.Count("MOB_QT_CH_SOLDIER_01_2");
        CHECK(soldiers >= 10);
        CHECK(soldiers <= 11);
        CHECK_FALSE(host.Told(1, "SN_TALK_QNO_CH_HWAN_1_1_09"));
        quests.Tick(start + 11s + 10min + 1s);
        CHECK(host.Told(1, "SN_TALK_QNO_CH_HWAN_1_1_09"));
        CHECK(quests.Record(1, test)->status == proto::QuestStatus::Achieved);
        quests.Tick(start + 11s + 10min + 2s);
        CHECK(host.layers[1] == 0);
        CHECK(quests.Trials().empty());
        CHECK(quests.Gauge(1, test) == 0);
        // Back at the general: the hand-in.
        REQUIRE(Ask(quests, proto::QuestTalk{7, test}));
        CHECK(host.Last<proto::QuestSays>().moment == "done");
    }
    SUBCASE("the general slain: passed at once, out") {
        host.raised[0].standing = false;
        quests.Tick(start + 20s);
        CHECK(host.Told(1, "SN_TALK_QNO_CH_HWAN_1_1_09"));
        CHECK(quests.Record(1, test)->status == proto::QuestStatus::Achieved);
        quests.Tick(start + 21s);
        CHECK(host.layers[1] == 0);
        CHECK(quests.Trials().empty());
    }
    SUBCASE("dead: the general taken away, the words, out 15 s later; not passed") {
        host.who.dead = true;
        quests.Tick(start + 30s);
        CHECK(host.Told(1, "SN_TALK_QNO_CH_HWAN_1_1_08"));
        CHECK(host.raised[0].gone);
        quests.Tick(start + 44s);
        CHECK(host.layers[1] == layer);
        quests.Tick(start + 46s);
        CHECK(host.layers[1] == 0);
        CHECK(quests.Record(1, test)->status == proto::QuestStatus::Achieving);
    }
    SUBCASE("given up inside: out") {
        REQUIRE(Ask(quests, proto::QuestAbandon{test}));
        quests.Tick(start + 20s);
        CHECK(host.layers[1] == 0);
        CHECK(quests.Trials().empty());
    }
}

TEST_CASE("Quest trial: BAEKRYOUNG_10's secret door: alone into the White Viper's passage, ShinMoo's scene, its talk met after 39 s, out (a made-up host)") {
    const auto coded = Coded();
    TrialHost host;
    host.who.position = {proto::WorldPosition{25000, 100.0f, 0.0f, 100.0f, 0}};
    server::Quests quests(host, {});
    quests.Load(Numbered(coded));
    const game::QuestDef* q = quests.Find("QNO_TQ_BAEKRYOUNG_10");
    REQUIRE(q != nullptr);
    REQUIRE(q->trial);
    CHECK(q->trial->scene == "qno_tq_baekryoung_10");
    CHECK(q->trial->sceneSeconds == 39);
    game::QuestDef again;
    std::string error;
    REQUIRE_MESSAGE(game::ParseQuest(game::WriteQuest(*q), again, error), error); // its line read back
    CHECK(again.trial == q->trial);
    quests.Enter(1, {Record(q->id, proto::QuestStatus::Achieving)});
    // Under way: Leebaek's "I will open the secret door now" and the row.
    REQUIRE(Ask(quests, proto::QuestTalk{10, q->id}));
    CHECK(host.Last<proto::QuestSays>().moment == "trial");
    REQUIRE(Ask(quests, proto::QuestHandIn{10, q->id, {}}));
    REQUIRE(quests.Trials().size() == 1);
    const std::uint32_t layer = quests.Trials()[0].second;
    REQUIRE(host.enteredAt);
    CHECK(host.enteredAt->region == 0x8003);
    CHECK(host.enteredAt->x == 187.0f);
    const auto start = std::chrono::steady_clock::now();

    SUBCASE("the scene played; 39 s on its talk is met and the player out; Leebaek's done talk") {
        quests.Tick(start);
        REQUIRE(host.scenes.size() == 1);
        CHECK(host.scenes[0] == std::pair<std::uint32_t, std::string>{1u, "qno_tq_baekryoung_10"});
        quests.Tick(start + 38s);
        CHECK(host.layers[1] == layer);
        CHECK(quests.Record(1, q->id)->status == proto::QuestStatus::Achieving);
        quests.Tick(start + 40s);
        CHECK(quests.Record(1, q->id)->status == proto::QuestStatus::Achieved);
        CHECK(host.layers[1] == 0);
        CHECK(quests.Trials().empty());
        REQUIRE(Ask(quests, proto::QuestTalk{10, q->id}));
        CHECK(host.Last<proto::QuestSays>().moment == "done");
    }
    SUBCASE("left before its end: nothing met") {
        quests.Tick(start);
        host.layers[1] = 0; // a return scroll, out of the game
        quests.Tick(start + 10s);
        CHECK(quests.Trials().empty());
        quests.Tick(start + 40s);
        CHECK(quests.Record(1, q->id)->status == proto::QuestStatus::Achieving);
    }
}

TEST_CASE("Quest trial: the ritual's ghost counts only in berserk; in berserk the charm becomes the piece, else failed; out 10 s on (a made-up host)") {
    const auto coded = Coded();
    TrialHost host;
    host.who.position = {proto::WorldPosition{25000, 100.0f, 0.0f, 100.0f, 0}};
    server::Quests quests(host, {});
    quests.Load(Numbered(coded));
    const std::uint32_t ritual = quests.Find("QNO_CH_HWAN_1_5")->id;
    quests.Enter(1, {Record(ritual, proto::QuestStatus::Achieving)});
    host.bag["ITEM_QNO_CH_HWAN_1_5_01"] = 1;
    // A job's suit does not hold a ritual back (it asks the transport alone).
    host.who.jobSuit = true;
    REQUIRE(Ask(quests, proto::QuestHandIn{8, ritual, {}}));
    REQUIRE(quests.Trials().size() == 1);
    REQUIRE(host.enteredAt);
    CHECK(host.enteredAt->region == 25253);
    const auto start = std::chrono::steady_clock::now();
    quests.Tick(start + 11s);
    CHECK(host.raised.empty());
    quests.Tick(start + 13s);
    REQUIRE(host.Count("MOB_QT_CH_CHARGHOST_01") == 1);
    // At its place exactly (the spawn's radius 0).
    CHECK(Flat(host.raised[0].at, proto::WorldPosition{25253, 177.0f, 233.0f, 684.0f, 0}) < 0.01);
    const std::uint32_t me = 1;

    SUBCASE("not in berserk: the kill not counted, the words, no piece, out 10 s later") {
        quests.Killed(9000, "MOB_QT_CH_CHARGHOST_01", 1, me);
        CHECK(quests.Record(1, ritual)->progress[0] == 0);
        host.raised[0].standing = false;
        quests.Tick(start + 20s);
        CHECK(host.Told(1, "SN_TALK_QNO_CH_HWAN_1_5_09"));
        CHECK(host.bag["ITEM_QNO_CH_HWAN_1_5_02"] == 0);
        CHECK(host.bag["ITEM_QNO_CH_HWAN_1_5_01"] == 1);
        quests.Tick(start + 29s);
        CHECK(host.layers[1] != 0);
        quests.Tick(start + 31s);
        CHECK(host.layers[1] == 0);
        // Again at the exorcist (the charm still held).
        REQUIRE(Ask(quests, proto::QuestTalk{8, ritual}));
        CHECK(host.Last<proto::QuestSays>().moment == "trial");
    }
    SUBCASE("in berserk: counted, the charm for the piece, handed in (the piece taken)") {
        host.who.berserk = true;
        quests.Killed(9000, "MOB_QT_CH_CHARGHOST_01", 1, me);
        CHECK(quests.Record(1, ritual)->progress[0] == 1);
        host.raised[0].standing = false;
        quests.Tick(start + 20s);
        CHECK_FALSE(host.Told(1, "SN_TALK_QNO_CH_HWAN_1_5_09"));
        CHECK(host.bag["ITEM_QNO_CH_HWAN_1_5_01"] == 0);
        CHECK(host.bag["ITEM_QNO_CH_HWAN_1_5_02"] == 1);
        quests.Tick(start + 31s);
        CHECK(host.layers[1] == 0);
        REQUIRE(Ask(quests, proto::QuestHandIn{8, ritual, {}}));
        CHECK(host.Last<proto::QuestGone>().why == proto::QuestEnd::Finished);
        CHECK(host.bag["ITEM_QNO_CH_HWAN_1_5_02"] == 0);
    }
    SUBCASE("its ten minutes over: out without a word, not passed") {
        for (auto at = start + 14s; at <= start + 13s + 10min + 1s; at += 5s) quests.Tick(at);
        CHECK(host.Told(1, "SN_TALK_QNO_CH_HWAN_1_5_10"));
        quests.Tick(start + 13s + 10min + 7s);
        CHECK(host.layers[1] == 0);
        CHECK(quests.Record(1, ritual)->status == proto::QuestStatus::Achieving);
    }
}

TEST_CASE("Quest item use: IVY_3's trumpet raises the keeper of the first site whose sign is not held; the blood makes the Key of Sign (a made-up host)") {
    const auto coded = Coded();
    TrialHost host;
    host.who.race = "europe";
    host.who.sharedParty = true;
    server::Quests quests(host, {});
    quests.Load(Numbered(coded));
    const std::uint32_t ivy = quests.Find("QNO_EU_IVY_3")->id;
    quests.Enter(1, {Record(ivy, proto::QuestStatus::Achieving)});
    host.bag["ITEM_QNO_EU_IVY_3_01"] = 5;
    // Out of an auto-share party: _26; at none of the sites: _25.
    host.who.sharedParty = false;
    host.who.position = {25954, 1661.0f, 180.0f, 210.0f, 0};
    CHECK_FALSE(quests.UseItem(1, "ITEM_QNO_EU_IVY_3_01"));
    CHECK(host.Told(1, "SN_TALK_QNO_EU_IVY_3_26"));
    host.who.sharedParty = true;
    host.who.position = {25000, 100.0f, 0.0f, 100.0f, 0};
    CHECK_FALSE(quests.UseItem(1, "ITEM_QNO_EU_IVY_3_01"));
    CHECK(host.Told(1, "SN_TALK_QNO_EU_IVY_3_25"));
    // The second site (region 25954 within 700): its keeper 20..100 away, gone after five minutes.
    host.who.position = {25954, 1661.0f, 180.0f, 210.0f, 0};
    CHECK(quests.UseItem(1, "ITEM_QNO_EU_IVY_3_01"));
    REQUIRE(host.raised.size() == 1);
    CHECK(host.raised[0].monster == "MOB_QT_03_PUNISHER_CLON");
    CHECK(Flat(host.raised[0].at, host.who.position) <= 100.5);
    const auto start = std::chrono::steady_clock::now();
    quests.Tick(start + 299s);
    CHECK_FALSE(host.raised[0].gone);
    quests.Tick(start + 301s);
    CHECK(host.raised[0].gone);
    // Its kill drops its sign (the quest's hidden gathering at 100 %); with it held that site raises nothing more.
    const std::uint32_t me = 1;
    quests.Killed(9100, "MOB_QT_03_PUNISHER_CLON", 0, me);
    CHECK(host.bag["ITEM_QNO_EU_IVY_3_03"] == 1);
    host.told.clear();
    CHECK_FALSE(quests.UseItem(1, "ITEM_QNO_EU_IVY_3_01"));
    CHECK(host.Told(1, "SN_TALK_QNO_EU_IVY_3_25"));
    // The blood: refused silently short of a sign or a place; with the three signs the key, one of each taken.
    host.bag["ITEM_QNO_EU_IVY_3_05"] = 1;
    host.told.clear();
    CHECK_FALSE(quests.UseItem(1, "ITEM_QNO_EU_IVY_3_05"));
    host.bag["ITEM_QNO_EU_IVY_3_02"] = 1;
    host.bag["ITEM_QNO_EU_IVY_3_04"] = 2;
    host.freePlace = false;
    CHECK_FALSE(quests.UseItem(1, "ITEM_QNO_EU_IVY_3_05"));
    CHECK(host.told.empty());
    host.freePlace = true;
    CHECK(quests.UseItem(1, "ITEM_QNO_EU_IVY_3_05"));
    CHECK(host.bag["ITEM_QNO_EU_IVY_3_06"] == 1);
    CHECK(host.bag["ITEM_QNO_EU_IVY_3_02"] == 0);
    CHECK(host.bag["ITEM_QNO_EU_IVY_3_03"] == 0);
    CHECK(host.bag["ITEM_QNO_EU_IVY_3_04"] == 1);
}

TEST_CASE("Quest item use: the party instances' words in the original's order; the opening's lines 5 s apart (a made-up host)") {
    const auto coded = Coded();
    TrialHost host;
    host.who.race = "europe";
    server::Quests quests(host, {});
    quests.Load(Numbered(coded));
    const std::uint32_t ivy = quests.Find("QNO_EU_IVY_3")->id;
    quests.Enter(1, {Record(ivy, proto::QuestStatus::Achieving)});
    host.bag["ITEM_QNO_EU_IVY_3_06"] = 1;
    // Away from the place and out of an auto-share party: the place's words first (EASTEU_21 and IVY_3 ask it first).
    host.who.sharedParty = false;
    host.who.position = {25000, 100.0f, 0.0f, 100.0f, 0};
    CHECK_FALSE(quests.UseItem(1, "ITEM_QNO_EU_IVY_3_06"));
    CHECK(host.Told(1, "SN_TALK_QNO_EU_IVY_3_10"));
    CHECK_FALSE(host.Told(1, "SN_TALK_QNO_EU_IVY_3_08"));
    host.who.position = {25183, 1882.0f, -169.0f, 632.0f, 0};
    CHECK_FALSE(quests.UseItem(1, "ITEM_QNO_EU_IVY_3_06"));
    CHECK(host.Told(1, "SN_TALK_QNO_EU_IVY_3_08"));
    // Inside an instance already: silently, before any words.
    host.told.clear();
    host.layers[1] = 5;
    CHECK_FALSE(quests.UseItem(1, "ITEM_QNO_EU_IVY_3_06"));
    CHECK(host.told.empty());
    host.layers[1] = 0;
    // In: Ivy 30 s on; 2 s later her line, then 5 s apart the next two, 5 s later the battle begun and its gauge.
    host.who.sharedParty = true;
    host.party = {1};
    CHECK_FALSE(quests.UseItem(1, "ITEM_QNO_EU_IVY_3_06")); // the key kept (refused 2, the original's)
    REQUIRE(quests.Worlds().size() == 1);
    const auto start = std::chrono::steady_clock::now();
    quests.Tick(start + 31s);
    REQUIRE(host.Count("MOB_QT_01_IVY") == 1);
    CHECK(host.told.empty());
    quests.Tick(start + 33s);
    CHECK(host.Told(1, "SN_TALK_QNO_EU_IVY_3_11"));
    CHECK_FALSE(host.Told(1, "SN_TALK_QNO_EU_IVY_3_12"));
    quests.Tick(start + 37s);
    CHECK_FALSE(host.Told(1, "SN_TALK_QNO_EU_IVY_3_12"));
    quests.Tick(start + 38s);
    CHECK(host.Told(1, "SN_TALK_QNO_EU_IVY_3_12"));
    quests.Tick(start + 43s);
    CHECK(host.Told(1, "SN_TALK_QNO_EU_IVY_3_13"));
    CHECK_FALSE(host.Told(1, "SN_TALK_QNO_EU_IVY_3_24"));
    CHECK(quests.Gauge(1, ivy) == 0);
    quests.Tick(start + 48s);
    CHECK(host.Told(1, "SN_TALK_QNO_EU_IVY_3_24"));
    CHECK(quests.Gauge(1, ivy) > 1100);
    // Its time from the battle's begin: 10 minutes left at 48 s + 10 min.
    quests.Tick(start + 47s + 10min);
    CHECK_FALSE(host.Told(1, "SN_TALK_QNO_EU_IVY_3_14"));
    quests.Tick(start + 49s + 10min);
    CHECK(host.Told(1, "SN_TALK_QNO_EU_IVY_3_14"));
}

TEST_CASE("Quest item use: ORNAMENT_4's stone takes its party to the four snake generals in turn; their words, the time, the last one's death (a made-up host)") {
    const auto coded = Coded();
    TrialHost host;
    host.who.race = "china";
    server::QuestHost::Player member = host.who;
    member.peer = 2;
    member.name = "Member";
    host.others[2] = member;
    server::Quests quests(host, {});
    quests.Load(Numbered(coded));
    const std::uint32_t ornament = quests.Find("QNO_TQ_ORNAMENT_4")->id;
    quests.Enter(1, {Record(ornament, proto::QuestStatus::Achieving)});
    quests.Enter(2, {Record(ornament, proto::QuestStatus::Done, 1)}); // finished before: comes along
    // Without the calabash: refused silently; away from the cave (the tomb's floor 0x8005, (184, 72, 157) within 2000): _23 before
    // the party's _21 (the handler asks the place first).
    host.bag["ITEM_QNO_TQ_ORNAMENT_4_04"] = 1;
    host.who.position = {0x8005, 5000.0f, 72.0f, 5000.0f, 0};
    CHECK_FALSE(quests.UseItem(1, "ITEM_QNO_TQ_ORNAMENT_4_04"));
    CHECK(host.told.empty());
    host.bag["ITEM_QNO_TQ_ORNAMENT_4_01"] = 1;
    CHECK_FALSE(quests.UseItem(1, "ITEM_QNO_TQ_ORNAMENT_4_04"));
    CHECK(host.Told(1, "SN_TALK_QNO_TQ_ORNAMENT_4_23"));
    CHECK_FALSE(host.Told(1, "SN_TALK_QNO_TQ_ORNAMENT_4_21"));
    host.who.position = {0x8005, 184.0f, 72.0f, 157.0f, 0};
    host.others[2].position = host.who.position;
    CHECK_FALSE(quests.UseItem(1, "ITEM_QNO_TQ_ORNAMENT_4_04"));
    CHECK(host.Told(1, "SN_TALK_QNO_TQ_ORNAMENT_4_21"));
    host.who.sharedParty = true;
    host.party = {1, 2};
    host.transport = true;
    CHECK_FALSE(quests.UseItem(1, "ITEM_QNO_TQ_ORNAMENT_4_04"));
    CHECK(host.Told(1, "SN_TALK_QNO_TQ_ORNAMENT_4_27"));
    host.transport = false;
    CHECK_FALSE(quests.UseItem(1, "ITEM_QNO_TQ_ORNAMENT_4_04")); // the stone kept (refused 2, the original's)
    REQUIRE(quests.Worlds().size() == 1);
    CHECK(host.layers[1] != 0);
    CHECK(host.layers[2] == host.layers[1]);
    const auto start = std::chrono::steady_clock::now();
    quests.Tick(start + 29s);
    CHECK(host.raised.empty());
    // The first 30 s on; its line 2 s later; its battle 5 s after: the battle begun, the gauge's 30 minutes.
    quests.Tick(start + 31s);
    REQUIRE(host.Count("MOB_QT_TQ_SNAKEGENERAL1") == 1);
    quests.Tick(start + 33s);
    CHECK(host.Told(2, "SN_TALK_QNO_TQ_ORNAMENT_4_06"));
    quests.Tick(start + 38s);
    CHECK(host.Told(1, "SN_TALK_QNO_TQ_ORNAMENT_4_26"));
    CHECK(quests.Gauge(1, ornament) > 1700);

    SUBCASE("each slain: its words, the next 10 s on; the fourth's death raises its way out, the time's end puts them out") {
        auto at = start + 40s;
        const char* generals[] = {"MOB_QT_TQ_SNAKEGENERAL1", "MOB_QT_TQ_SNAKEGENERAL2", "MOB_QT_TQ_SNAKEGENERAL3", "MOB_QT_TQ_SNAKEGENERAL4"};
        const char* dies[] = {"SN_TALK_QNO_TQ_ORNAMENT_4_07", "SN_TALK_QNO_TQ_ORNAMENT_4_10", "SN_TALK_QNO_TQ_ORNAMENT_4_13", "SN_TALK_QNO_TQ_ORNAMENT_4_16"};
        for (int g = 0; g < 4; ++g) {
            CAPTURE(g);
            REQUIRE(host.Count(generals[g]) == 1);
            host.raised.back().standing = false;
            quests.Tick(at);
            CHECK(host.Told(1, dies[g]));
            if (g == 3) break;
            quests.Tick(at + 9s);
            CHECK(host.Count(generals[g + 1]) == 0);
            quests.Tick(at + 10s);
            CHECK(host.Count(generals[g + 1]) == 1);
            quests.Tick(at + 12s);
            quests.Tick(at + 17s);
            at += 20s;
        }
        CHECK(host.told.end() == std::find(host.told.begin(), host.told.end(), std::pair<std::uint32_t, std::string>{1, "SN_TALK_QNO_TQ_ORNAMENT_4_08"}));
        // Its way out where the fourth came, 20..100 added to its x and its z, back where they came in (its link empty);
        // they stay meanwhile.
        REQUIRE(host.ways.size() == 1);
        CHECK_FALSE(host.ways[0].to.has_value());
        const double dx = host.ways[0].at.x - host.raised.back().at.x, dz = host.ways[0].at.z - host.raised.back().at.z;
        CHECK(dx >= 19.9);
        CHECK(dx <= 100.1);
        CHECK(dz >= 19.9);
        CHECK(dz <= 100.1);
        CHECK(host.ways[0].at.layer == host.layers[1]);
        quests.Tick(at + 1s);
        CHECK(host.layers[1] != 0);
        CHECK(quests.Worlds().size() == 1);
        // The time's end (30 minutes from the first battle): everyone left out 10 s later.
        quests.Tick(start + 38s + 30min + 1s);
        CHECK(host.layers[1] != 0);
        quests.Tick(start + 38s + 30min + 12s);
        CHECK(host.layers[1] == 0);
        CHECK(host.layers[2] == 0);
        CHECK(quests.Worlds().empty());
    }
    SUBCASE("its time: 10 and 5 minutes left, at 30 the general's last words, out 10 s later") {
        quests.Tick(start + 38s + 20min + 1s);
        CHECK(host.Told(1, "SN_TALK_QNO_TQ_ORNAMENT_4_24"));
        quests.Tick(start + 38s + 25min + 1s);
        CHECK(host.Told(1, "SN_TALK_QNO_TQ_ORNAMENT_4_25"));
        quests.Tick(start + 38s + 30min + 1s);
        CHECK(host.Told(1, "SN_TALK_QNO_TQ_ORNAMENT_4_08"));
        CHECK(host.raised[0].gone);
        quests.Tick(start + 38s + 30min + 12s);
        CHECK(host.layers[1] == 0);
        CHECK(quests.Worlds().empty());
    }
}

TEST_CASE("Quest talk rule: GENERALORDER_4's secret door takes the party holding the quest to the four guardians; the way out to Jangan (a made-up host)") {
    const auto coded = Coded();
    TrialHost host;
    host.who.race = "china";
    host.who.position = {25255, 590.0f, 0.0f, 290.0f, 0};
    server::QuestHost::Player member = host.who;
    member.peer = 2;
    member.name = "Member";
    host.others[2] = member;
    host.others[3] = member; // a third without the quest
    host.others[3].peer = 3;
    server::Quests quests(host, {});
    quests.Load(Numbered(coded));
    const std::uint32_t order = quests.Find("QNO_TQ_GENERALORDER_4")->id;
    quests.Enter(1, {Record(order, proto::QuestStatus::Achieving)});
    quests.Enter(2, {Record(order, proto::QuestStatus::Achieving)});
    quests.Enter(3, {});
    const auto says = [&] { return host.Last<proto::QuestSays>(); };
    // No party sharing both: the quest's not-party (_26).
    host.who.sharedParty = false;
    REQUIRE(Ask(quests, proto::QuestTalk{7, order}));
    CHECK(says().moment == "not-party");
    // A member: _30; the master with a transport out: _25.
    host.who.sharedParty = true;
    host.party = {1, 2, 3};
    host.master = false;
    REQUIRE(Ask(quests, proto::QuestTalk{7, order}));
    CHECK(says().text == "SN_TALK_QNO_TQ_GENERALORDER_4_30");
    host.master = true;
    host.transport = true;
    REQUIRE(Ask(quests, proto::QuestTalk{7, order}));
    CHECK(says().text == "SN_TALK_QNO_TQ_GENERALORDER_4_25");
    host.transport = false;
    // The master: the question _27 with its rows (_28, then the deny).
    REQUIRE(Ask(quests, proto::QuestTalk{7, order}));
    const proto::QuestSays ask = says();
    CHECK(ask.text == "SN_TALK_QNO_TQ_GENERALORDER_4_27");
    CHECK(ask.choice == proto::QuestChoice::Rows);
    REQUIRE(ask.rows.size() == 2);
    CHECK(ask.rows[0] == "SN_TALK_QNO_TQ_GENERALORDER_4_28");
    CHECK(ask.rows[1] == "SN_TALK_COMMON_DENY");
    // Denied: nothing opens.
    REQUIRE(Ask(quests, proto::QuestHandIn{7, order, {1}}));
    CHECK(quests.Worlds().empty());
    // The row: the master and the member holding the quest in, at the world's start (the tomb's 5th floor by its north
    // guardian); the third (no quest) stays.
    REQUIRE(Ask(quests, proto::QuestHandIn{7, order, {0}}));
    REQUIRE(quests.Worlds().size() == 1);
    CHECK(host.layers[1] != 0);
    CHECK(host.layers[2] == host.layers[1]);
    CHECK(host.layers[3] == 0);
    REQUIRE(host.enteredAt.has_value());
    CHECK(host.enteredAt->region == 0x8003);
    CHECK(host.enteredAt->z == doctest::Approx(15035.0f));
    // The first guardian 30 s on, within 190..200 of the master (the quest service's banded spawn: its radius drawn there,
    // its distance banded in it); the next ones within 90..100.
    const auto start = std::chrono::steady_clock::now();
    host.who.position = {0x8003, -116.0f, -17.0f, 15035.0f, 0};
    quests.Tick(start + 31s);
    REQUIRE(host.Count("MOB_QT_TQ_NORTHGUARDIAN") == 1);
    const double first = Flat(host.raised.back().at, host.who.position);
    CHECK(first <= 200.1);
    quests.Tick(start + 33s);
    CHECK(host.Told(2, "SN_TALK_QNO_TQ_GENERALORDER_4_06"));
    quests.Tick(start + 38s);
    CHECK(host.Told(1, "SN_TALK_QNO_TQ_GENERALORDER_4_24"));
    auto at = start + 40s;
    const char* guardians[] = {"MOB_QT_TQ_NORTHGUARDIAN", "MOB_QT_TQ_SOUTHGUARDIAN", "MOB_QT_TQ_WESTGUARDIAN", "MOB_QT_TQ_EASTGUARDIAN"};
    const char* dies[] = {"SN_TALK_QNO_TQ_GENERALORDER_4_07", "SN_TALK_QNO_TQ_GENERALORDER_4_10", "SN_TALK_QNO_TQ_GENERALORDER_4_13",
                          "SN_TALK_QNO_TQ_GENERALORDER_4_16"};
    for (int g = 0; g < 4; ++g) {
        CAPTURE(g);
        REQUIRE(host.Count(guardians[g]) == 1);
        if (g > 0) CHECK(Flat(host.raised.back().at, host.who.position) <= 100.1);
        host.raised.back().standing = false;
        quests.Tick(at);
        CHECK(host.Told(1, dies[g]));
        if (g == 3) break;
        quests.Tick(at + 10s);
        quests.Tick(at + 12s);
        quests.Tick(at + 17s);
        at += 20s;
    }
    // The East's death: its way out to General Sonhyeon at Jangan.
    REQUIRE(host.ways.size() == 1);
    REQUIRE(host.ways[0].to.has_value());
    CHECK(host.ways[0].to->region == 25255);
    CHECK(host.ways[0].to->x == doctest::Approx(594.0f));
    CHECK(host.ways[0].to->z == doctest::Approx(233.0f));
}

namespace {

// Our game server with the game files and a database: two players of an auto-share party at EASTEU_21's place.
struct InstanceRig {
    test::TestDatabase& t;
    core::JobSystem jobs{2};
    net::NetService net{1};
    server::GameServerConfig config;
    server::GameServer game;
    assets::FileSystem fs;
    std::shared_ptr<assets::ItemTable> items = std::make_shared<assets::ItemTable>();
    std::shared_ptr<assets::CharacterTable> characters = std::make_shared<assets::CharacterTable>();
    std::string error;
    explicit InstanceRig(test::TestDatabase& db) : t(db) {}
    bool Start(const char* dir) {
        if (!assets::MountGameArchives(fs, dir, error) || !items->Load(fs, error) || !characters->Load(fs, error)) return false;
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
        config.devCommands = true;
        return game.Start(config, error);
    }
    struct Player {
        std::unique_ptr<test::Client> client;
        std::int64_t character = 0;
        std::uint32_t entity = 0;
        proto::InventoryList bag;
    };
    Player Make(const std::string& name, const proto::WorldPosition& at, std::uint32_t quest, std::uint32_t key) {
        Player p;
        REQUIRE(server::CreateAccount(t.Db(), name + "acct", "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        const std::int64_t account = server::FindAccount(t.Db(), name + "acct", ok)->id;
        p.client = std::make_unique<test::Client>(net, game.Port(), [this] { game.Poll(2ms); });
        REQUIRE(p.client->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), account, config.id, 30, error);
        p.client->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(p.client->Expect(entered));
        proto::CharacterCreate m;
        m.name = name;
        m.model = 100;
        m.chest = 1;
        m.legs = 2;
        m.shoes = 3;
        m.weapon = 10;
        p.client->Send(m);
        proto::LobbyResult r;
        REQUIRE(p.client->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        // By its name (the import's own characters may share the account's number).
        p.character = t.Db().Query("SELECT charid FROM _char WHERE charname16 = $1", {name}).Int(0, 0);
        REQUIRE(p.character != 0);
        REQUIRE(server::SaveProgress(t.Db(), {{p.character, 30, 0, 0, 0, 40, 40, 0}}));
        REQUIRE(server::SavePlaces(t.Db(), {{p.character, {at.region, at.x, at.y, at.z, 0.0f}}}));
        REQUIRE(server::SaveVitals(t.Db(), p.character, 100'000, 10'000));
        REQUIRE(t.Db().Query("INSERT INTO _charquest (charid, questid, status, achievementcount, starttime, endtime, questdata1, questdata2) "
                             "VALUES ($1, $2, 1, 0, now(), now(), 0, 0)",
                             {p.character, std::int64_t(quest)})
                    .Ok());
        REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {account}).Ok());
        server::StoredItem k;
        k.refId = key;
        k.quantity = 1;
        k.counted = true;
        REQUIRE(server::GiveItems(t.Db(), config.id, name, {k}, 0, error) == server::GiveResult::Done);
        REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, $2)", {account, int(config.id)}).Ok());
        p.client->Send(proto::CharacterSelect{name});
        REQUIRE(p.client->Expect(r));
        p.client->Send(proto::WorldEnter{});
        proto::WorldEntered w;
        REQUIRE(p.client->Expect(w));
        REQUIRE(w.status == proto::WorldStatus::Ok);
        p.entity = w.entity;
        REQUIRE(p.client->Expect(p.bag));
        return p;
    }
};

template <typename T>
T Next(InstanceRig::Player& p) {
    T m;
    REQUIRE(p.client->Expect(m));
    return m;
}

} // namespace

TEST_CASE("Game server: EASTEU_21's key takes its master's party into Cerberus's instance; his death ends it (game files)") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    const std::filesystem::path exportDir = sr::test::ExportDir();
    if (!std::filesystem::exists(exportDir / "SRO_VT_SHARD" / "schema.tsv")) {
        MESSAGE("SR_VSRO_EXPORT missing - skipped");
        return;
    }
    test::TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    InstanceRig rig(t);
    // The original's reference tables (the instances' catalog reads them: _RefGame_World, ...).
    server::ImportOptions options;
    options.dir = exportDir;
    options.databases = {"SRO_VT_SHARD"};
    options.replace = true;
    server::ImportReport report;
    REQUIRE_MESSAGE(server::ImportDatabases(t.Db(), options, report, rig.error), rig.error);
    // The quest by hand with its words and a short timeline (ours; the original's rule for the key is the item's own when the
    // definition has none: game::AddOriginalUseRules).
    REQUIRE_MESSAGE(server::SaveQuest(t.Db(),
                                       ParseOne("quest QNO_EU_EASTEU_21\nid 20160\nstart level 1-\nstart party\ngiver NPC_T_NOBODY\n"
                                                "on use ITEM_QNO_EU_EASTEU_21_01 status 1 7 8 place-first party @SN_TALK_QNO_EU_EASTEU_21_08 "
                                                "at 24902 886 854 8 600 away @SN_TALK_QNO_EU_EASTEU_21_10 instance INS_QNO_EU_EASTEU_21 "
                                                "unique MOB_QT_01_KERBEROS after 1 minutes 20 master @SN_TALK_QNO_EU_EASTEU_21_09 "
                                                "comes @SN_TALK_QNO_EU_EASTEU_21_14 begun @SN_TALK_QNO_EU_EASTEU_21_21 slain @SN_TALK_QNO_EU_EASTEU_21_15 "
                                                "over @SN_TALK_QNO_EU_EASTEU_21_19\n"
                                                "goal kill 1 MOB_QT_01_KERBEROS\n"),
                                       server::QuestOrigin::Ours, rig.error),
                    rig.error);
    REQUIRE_MESSAGE(rig.Start(dir), rig.error);
    const assets::ItemRow* key = rig.items->Find(std::string_view("ITEM_QNO_EU_EASTEU_21_01"));
    const assets::ObjectRow* kerberos = rig.characters->Find(std::string_view("MOB_QT_01_KERBEROS"));
    REQUIRE(key != nullptr);
    REQUIRE(kerberos != nullptr);
    // Two of a party at the key's place (region 24902 (886, 854, 8)), a few steps apart.
    const proto::WorldPosition place{24902, 886.0f, 854.0f, 8.0f, 0};
    InstanceRig::Player a = rig.Make("Trialmaster", place, 20160, key->refObjId);
    InstanceRig::Player b = rig.Make("Trialmember", {24902, 896.0f, 854.0f, 8.0f, 0}, 20160, key->refObjId);
    // The party sharing experience and items (settings: both auto-share).
    a.client->Send(proto::PartyInvite{b.entity, std::uint8_t(game::kPartyExpShare | game::kPartyItemShare)});
    CHECK(Next<proto::PartyInvited>(b).from == a.entity);
    b.client->Send(proto::PartyInviteAnswer{1});
    for (int i = 0; i < 200; ++i) rig.game.Poll(2ms);
    // Watched: who comes and goes, where each lands.
    std::map<std::uint32_t, proto::EntityAppeared> seen;
    std::set<std::uint32_t> gone;
    bool aIn = false, bIn = false, aOut = false, bOut = false;
    std::vector<std::string> notices;
    const auto pump = [&](auto&& until, std::chrono::milliseconds most) {
        const auto end = std::chrono::steady_clock::now() + most;
        while (std::chrono::steady_clock::now() < end && !until()) {
            proto::EntityAppeared ap;
            if (a.client->Expect(ap, 3ms)) seen[ap.entity] = ap;
            proto::EntityGone g;
            if (a.client->Expect(g, 3ms)) gone.insert(g.entity);
            // In: the instance's state (Instances::Arrived, its world 11); out: the shared world's (0).
            proto::InstanceState st;
            if (a.client->Expect(st, 3ms)) (st.world == 11 ? aIn : aOut) = true;
            if (b.client->Expect(st, 3ms)) (st.world == 11 ? bIn : bOut) = true;
            proto::InstanceNotice n;
            if (a.client->Expect(n, 3ms)) notices.push_back(n.text);
        }
        return until();
    };
    const auto find = [&](std::uint32_t model) -> std::uint32_t {
        for (const auto& [entity, ap] : seen)
            if (ap.character.model == model && ap.kind == proto::EntityKind::Monster && !gone.contains(entity)) return entity;
        return 0;
    };
    // The master's key: both in (the key kept: refused 2, the original's).
    proto::UseItem use;
    for (const proto::ItemRecord& i : a.bag.items)
        if (i.item == key->refObjId) use.slot = i.slot;
    a.client->Send(use);
    REQUIRE(pump([&] { return aIn && bIn; }, 10s));
    // Cerberus 1 s on, then his line and the battle begun.
    REQUIRE(pump([&] { return find(kerberos->refObjId) != 0; }, 15s));
    const std::uint32_t cerberus = find(kerberos->refObjId);
    pump([&] { return std::find(notices.begin(), notices.end(), "SN_TALK_QNO_EU_EASTEU_21_21") != notices.end(); }, 15s);
    CHECK(std::find(notices.begin(), notices.end(), "SN_TALK_QNO_EU_EASTEU_21_14") != notices.end());
    // Felled (a developer's strike): his words, both out.
    a.client->Send(proto::DevStrike{cerberus, 100'000'000});
    REQUIRE(pump([&] { return aOut && bOut; }, 15s));
    CHECK(std::find(notices.begin(), notices.end(), "SN_TALK_QNO_EU_EASTEU_21_15") != notices.end());
}
