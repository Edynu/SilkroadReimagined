// The coded quests' captures over time and their traps:
// a caught COS kept in the quest's record (its minutes and mark, the original's record bytes) and brought back on the world
// entry; the traps set from the bag whose catch is the quest's event (GORIA_6, GENERAL_1, IVY_1: a caught item, its minutes,
// its holder's death, IVY_1's guardians); the daily resupply ("come again tomorrow") and the tools taken at the hand-in. A
// made-up host with a record store (a relog is a Leave and an Enter with the saved records, as on either game server).

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
#include "sr/game/CharacterRules.h"
#include "sr/game/Quest.h"
#include "sr/game/SpecialItems.h"
#include "sr/game/QuestItems.h"
#include "sr/game/QuestProtocol.h"
#include "sr/server/GameServer.h"
#include "sr/server/QuestScript.h"
#include "sr/server/QuestStore.h"
#include "sr/server/Quests.h"
#include "sr/server/Skills.h"
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

// One player (entity 1) at NPC 9 (NPC_T_GIVER); its bag by codename; its records as saved (the database).
struct CaptureHost : server::QuestHost {
    Player who{1, 77, "Trapper", 40, "europe", 0, true};
    std::map<std::string, std::uint32_t> bag;
    bool freePlace = true;
    std::vector<std::string> said;
    std::map<std::uint32_t, server::QuestRecord> saved;
    std::vector<proto::QuestSays> says;
    std::uint16_t day = 100;
    std::optional<Player> QuestPlayer(std::uint32_t entity) override { return entity == 1 ? std::optional(who) : std::nullopt; }
    void QuestSend(std::uint64_t, proto::MessageId id, const std::vector<std::byte>& payload) override {
        proto::QuestSays s;
        if (id == proto::QuestSays::kId && proto::Decode(payload, s)) says.push_back(s);
    }
    std::string QuestNpcNear(std::uint32_t, std::uint32_t npc) override { return npc == 9 ? "NPC_T_GIVER" : std::string(); }
    int QuestHour() override { return 12; }
    std::uint32_t QuestItemCount(std::uint32_t, const std::string& item) override { return bag.contains(item) ? bag[item] : 0; }
    bool QuestGive(std::uint32_t, const std::vector<game::QuestGoal::Items>& items) override {
        if (!freePlace) return false;
        for (const auto& i : items) bag[i.item] += i.count;
        return true;
    }
    bool QuestTake(std::uint32_t, const std::vector<game::QuestGoal::Items>& items) override {
        for (const auto& i : items)
            if (bag[i.item] < i.count) return false;
        for (const auto& i : items) bag[i.item] -= i.count;
        return true;
    }
    bool QuestTakeGold(std::uint32_t, std::uint64_t) override { return true; }
    bool QuestRoom(std::uint32_t, const std::vector<game::QuestGoal::Items>&) override { return freePlace; }
    void QuestReward(std::uint32_t, const Reward&) override {}
    void QuestDrop(std::uint32_t, std::uint32_t, const std::string&) override {}
    void QuestSave(std::int64_t, const server::QuestRecord& record) override { saved[record.quest] = record; }
    bool QuestThemeDone(std::uint32_t, const std::string&) override { return false; }
    bool QuestFreePlace(std::uint32_t) override { return freePlace; }
    void QuestSay(std::uint32_t, const std::string& key) override { said.push_back(key); }
    std::string LastSaid() const { return said.empty() ? std::string() : said.back(); }
    std::uint16_t QuestDay() override { return day; }
    // The catches: the selection, the COS made (caught or brought back), the trapped monsters taken, the raised monsters.
    std::optional<Selected> selected;
    std::optional<Selected> QuestSelected(std::uint32_t) override { return selected; }
    std::map<std::uint32_t, bool> coses; // standing
    std::uint32_t nextCos = 500;
    std::uint32_t QuestCatch(std::uint32_t, std::uint32_t, const std::string&) override {
        coses[nextCos] = true;
        return nextCos++;
    }
    std::vector<std::string> resummoned;
    std::uint32_t QuestResummon(std::uint32_t, const std::string& cos) override {
        resummoned.push_back(cos);
        coses[nextCos] = true;
        return nextCos++;
    }
    bool QuestCosStanding(std::uint32_t cos) override { return coses.contains(cos) && coses[cos]; }
    void QuestCosRemove(std::uint32_t cos) override { coses.erase(cos); }
    std::vector<std::uint32_t> taken;
    bool QuestTakeMonster(std::uint32_t monster) override {
        taken.push_back(monster);
        return true;
    }
    std::vector<std::pair<std::string, proto::WorldPosition>> raised;
    std::optional<std::size_t> QuestSpawn(std::uint32_t, const std::string& codename, const proto::WorldPosition& at) override {
        raised.push_back({codename, at});
        return raised.size() - 1;
    }
    std::vector<server::QuestRecord> Records() const {
        std::vector<server::QuestRecord> out;
        for (const auto& [id, r] : saved) out.push_back(r);
        return out;
    }
    std::string LastMoment() const { return says.empty() ? std::string() : says.back().moment; }
};

server::QuestRecord UnderWay(std::uint32_t quest) {
    server::QuestRecord r;
    r.quest = quest;
    r.status = proto::QuestStatus::Achieving;
    return r;
}

double Flat(const proto::WorldPosition& a, const proto::WorldPosition& b) {
    const proto::FlatPosition fa = proto::ToFlat(a), fb = proto::ToFlat(b);
    return std::hypot(fa.x - fb.x, fa.z - fb.z);
}

} // namespace

TEST_CASE("Quest captures: the trap's and daily resupply's parts read and write back the same; the original's three trap rules") {
    const game::QuestDef q = ParseOne(
        "quest QNO_T_NET\nid 20300\nstart level 1-\ngiver NPC_T_GIVER\nstart-item ITEM_T_NET 5\nresupply ITEM_T_NET 5\nresupply-daily start\n"
        "pay-take ITEM_T_NET\npay-take ITEM_T_CAUGHT\n"
        "on use ITEM_T_NET trap status 1 7 target MOB_T_ARCHER full @SN_T_FULL failed @SN_T_FAILED capture-item ITEM_T_CAUGHT minutes 30 roll 50 "
        "step 3 below raise MOB_T_GUARD 0 20 died-wait 5 already @SN_T_ALREADY warn 10 @SN_T_W10 ran @SN_T_RAN owner-died @SN_T_DIED\n"
        "goal drop 1 ITEM_T_CAUGHT at NPC_T_GIVER\ntalk NPC_T_GIVER resupply-spent @SN_T_TOMORROW\n");
    CHECK(q.resupplyDaily);
    CHECK(q.resupplyFromStart);
    CHECK(q.payTake == std::vector<std::string>{"ITEM_T_NET", "ITEM_T_CAUGHT"});
    REQUIRE(q.onUse.size() == 1);
    const game::QuestDef::UseRule& u = q.onUse.front();
    CHECK(u.trap);
    REQUIRE(u.capture);
    CHECK(u.capture->item == "ITEM_T_CAUGHT");
    CHECK(u.capture->cos.empty());
    CHECK(u.capture->raise == "MOB_T_GUARD");
    CHECK(u.capture->raiseFar == 20.0f);
    CHECK(u.capture->diedMinutes == 5);
    game::QuestDef back;
    std::string error;
    REQUIRE_MESSAGE(game::ParseQuest(game::WriteQuest(q), back, error), error);
    CHECK(back == q);

    // The original's: GORIA_6's trap (the 18 "one held", its death's 5 minutes), GENERAL_1's net (no "one held"), IVY_1's
    // informant by IVY_2's trap (its guardians), each a trap rule joined to its quest at the load.
    std::vector<game::QuestDef> coded;
    REQUIRE_MESSAGE(server::DecodeCodedQuests(coded, error), error);
    std::map<std::string, game::QuestDef> byCode;
    for (game::QuestDef& c : coded) byCode[c.codename] = c;
    for (const char* code : {"QNO_CA_GORIA_6", "QNO_EU_GENERAL_1", "QNO_EU_IVY_1"}) {
        REQUIRE(byCode.contains(code));
        game::QuestDef& d = byCode[code];
        CHECK(game::AddOriginalUseRules(d) >= 1);
        const auto trap = std::find_if(d.onUse.begin(), d.onUse.end(), [](const game::QuestDef::UseRule& r) { return r.trap; });
        REQUIRE(trap != d.onUse.end());
        REQUIRE(trap->capture);
        CHECK(trap->capture->roll == 50);
        CHECK(trap->capture->cos.empty()); // no COS record: the level term 0
    }
    const auto ruleOf = [&](const char* code) { return *std::find_if(byCode[code].onUse.begin(), byCode[code].onUse.end(), [](const auto& r) { return r.trap; }); };
    CHECK(ruleOf("QNO_CA_GORIA_6").item == "ITEM_QNO_CA_GORIA_6_01");
    CHECK(ruleOf("QNO_CA_GORIA_6").targets == std::vector<std::string>{"MOB_QT_01_HUNARCHER_CLON"});
    CHECK(ruleOf("QNO_CA_GORIA_6").capture->item == "ITEM_QNO_CA_GORIA_6_02");
    CHECK(ruleOf("QNO_CA_GORIA_6").capture->diedMinutes == 5);
    CHECK(ruleOf("QNO_EU_GENERAL_1").targets == std::vector<std::string>{"MOB_AM_ROGUE"});
    CHECK(ruleOf("QNO_EU_GENERAL_1").capture->already.Empty());
    CHECK(ruleOf("QNO_EU_IVY_1").item == "ITEM_QNO_EU_IVY_2_01");
    CHECK(ruleOf("QNO_EU_IVY_1").capture->minutes == 30);
    CHECK(ruleOf("QNO_EU_IVY_1").capture->raise == "MOB_QT_02_PUNISHER_CLON");
    // The daily resupplies and the hand-in's takes in the definitions.
    CHECK(byCode["QNO_EU_EASTEU_5"].resupplyDaily);
    CHECK(byCode["QNO_EU_EASTEU_5"].resupplyFromStart);
    CHECK(byCode["QNO_TQ_FINDTHIEF_2"].resupplyDaily);
    CHECK_FALSE(byCode["QNO_TQ_FINDTHIEF_2"].resupplyFromStart); // its start keeps no day
    CHECK_FALSE(byCode["QNO_RM_FLYSHIP1_3"].resupplyDaily);       // its talk asks no day
    CHECK_FALSE(byCode["QNO_CA_TREASURE_5"].resupplyDaily);
    CHECK(std::find(byCode["QNO_CA_GORIA_6"].payTake.begin(), byCode["QNO_CA_GORIA_6"].payTake.end(), "ITEM_QNO_CA_GORIA_6_01") !=
          byCode["QNO_CA_GORIA_6"].payTake.end());
}

TEST_CASE("Quest captures: a caught COS kept in the record, brought back on the world entry, its minutes running only in the world") {
    CaptureHost host;
    server::Quests quests(host, {});
    quests.Load({ParseOne("quest QNO_T_TRAP\nid 20301\nstart level 1-\n"
                          "on use ITEM_T_TRAP status 1 7 target MOB_T_DOWB within 300 none @SN_T_NONE failed @SN_T_FAILED capture MOB_T_DOWB_COS "
                          "minutes 20 roll 101 step 3 already @SN_T_ALREADY warn 10 @SN_T_W10 ran @SN_T_RAN owner-died @SN_T_OWNER cos-died @SN_T_COSDIED\n"
                          "goal capture MOB_T_DOWB_COS at NPC_T_GIVER\n")});
    host.who.position = {25000, 100.0f, 0.0f, 100.0f, 0};
    quests.Enter(1, {UnderWay(20301)});
    host.selected = server::QuestHost::Selected{40, "MOB_T_DOWB", {25000, 150.0f, 0.0f, 100.0f, 0}};
    REQUIRE(quests.UseItem(1, "ITEM_T_TRAP"));
    const std::uint32_t first = quests.Caught(1, 20301).first;
    REQUIRE(first != 0);
    // Kept as the original keeps it: the record's byte +4 the minutes (word 2), word +0xA 10 (word 5).
    REQUIRE(host.saved.contains(20301));
    CHECK(host.saved[20301].progress[server::Quests::kCatchMinutesSlot] == 20);
    CHECK(host.saved[20301].progress[server::Quests::kCatchMarkSlot] == server::Quests::kCaught);
    CHECK(quests.Gauge(1, 20301) > 19 * 60); // the display's 20 minutes

    // Each minute counted and kept; the owner leaves (a relog, or a move to the other game server): its COS goes with it.
    quests.Tick(std::chrono::steady_clock::now() + 3min + 1s);
    CHECK(quests.Caught(1, 20301).second == 17);
    CHECK(host.saved[20301].progress[server::Quests::kCatchMinutesSlot] == 17);
    quests.Leave(1);
    CHECK_FALSE(host.coses.contains(first));

    SUBCASE("back in the world (this server or the other: the same record): the COS beside it, its 17 minutes kept") {
        server::Quests other(host, {}); // the other game server's quests, reading the same record
        other.Load({ParseOne("quest QNO_T_TRAP\nid 20301\nstart level 1-\n"
                             "on use ITEM_T_TRAP status 1 7 target MOB_T_DOWB within 300 failed @SN_T_FAILED capture MOB_T_DOWB_COS minutes 20 "
                             "roll 101 step 3 already @SN_T_ALREADY ran @SN_T_RAN cos-died @SN_T_COSDIED\ngoal capture MOB_T_DOWB_COS at NPC_T_GIVER\n")});
        other.Enter(1, host.Records());
        REQUIRE(host.resummoned == std::vector<std::string>{"MOB_T_DOWB_COS"});
        const auto [cos, minutes] = other.Caught(1, 20301);
        CHECK(cos != 0);
        CHECK(host.coses.contains(cos));
        CHECK(minutes == 17);
        CHECK(other.Gauge(1, 20301) > 16 * 60);
        // Still one held: another trap is refused with its words.
        CHECK_FALSE(other.UseItem(1, "ITEM_T_TRAP"));
        CHECK(host.LastSaid() == "SN_T_ALREADY");
        // Handed in there: the goal met, the COS taken, nothing more counted.
        other.Handle(1, proto::QuestHandIn::kId, proto::Encode(proto::QuestHandIn{9, 20301, {}}));
        CHECK(other.Record(1, 20301)->status == proto::QuestStatus::Done);
        CHECK_FALSE(host.coses.contains(cos));
    }
    SUBCASE("its minutes out: it runs away, the record marked lost; no COS back at the next entry") {
        quests.Enter(1, host.Records());
        REQUIRE(host.resummoned.size() == 1);
        quests.Tick(std::chrono::steady_clock::now() + 17min + 1s);
        CHECK(host.LastSaid() == "SN_T_RAN");
        CHECK(quests.Caught(1, 20301).first == 0);
        CHECK(host.saved[20301].progress[server::Quests::kCatchMinutesSlot] == 0);
        CHECK(host.saved[20301].progress[server::Quests::kCatchMarkSlot] == server::Quests::kLost);
        quests.Leave(1);
        quests.Enter(1, host.Records());
        CHECK(host.resummoned.size() == 1);
        CHECK(quests.Caught(1, 20301).first == 0);
        // Lost, the quest is under way with no goal progressed again (the original's state 1): the trap works again.
        CHECK(quests.UseItem(1, "ITEM_T_TRAP"));
        CHECK(quests.Caught(1, 20301).first != 0);
    }
    SUBCASE("its own death (the bandits' hits) and its owner's: its words, the record marked lost") {
        quests.Enter(1, host.Records());
        host.coses[quests.Caught(1, 20301).first] = false;
        quests.Tick(std::chrono::steady_clock::now() + 2s);
        CHECK(host.LastSaid() == "SN_T_COSDIED");
        CHECK(host.saved[20301].progress[server::Quests::kCatchMarkSlot] == server::Quests::kLost);
        CHECK(quests.UseItem(1, "ITEM_T_TRAP"));
        quests.Died(1);
        CHECK(host.LastSaid() == "SN_T_OWNER");
        CHECK(quests.Caught(1, 20301).first == 0);
    }
}

TEST_CASE("Quest captures: a trap's catch as the quest's event: the item, its minutes, its holder's death, the guardians (a made-up host)") {
    CaptureHost host;
    server::Quests quests(host, {});
    quests.Load({ParseOne("quest QNO_T_GORIA\nid 20310\nstart level 1-\n"
                          "on use ITEM_T_TRAPS trap status 1 7 target MOB_T_ARCHER full @SN_T_FULL failed @SN_T_FAILED capture-item ITEM_T_HUN "
                          "minutes 20 roll 101 step 3 below died-wait 5 already @SN_T_ALREADY warn 10 @SN_T_W10 warn 5 @SN_T_W5 ran @SN_T_RAN "
                          "owner-died @SN_T_DIED\ngoal drop 1 ITEM_T_HUN at NPC_T_GIVER\n"),
                 ParseOne("quest QNO_T_NET\nid 20311\nstart level 1-\n"
                          "on use ITEM_T_NETS trap status 1 7 target MOB_T_ROGUE full @SN_T_FULL failed @SN_T_FAILED capture-item ITEM_T_PIRATE "
                          "minutes 20 roll 0 step 3 below ran @SN_T_RAN\ngoal drop 1 ITEM_T_PIRATE at NPC_T_GIVER\n"),
                 ParseOne("quest QNO_T_IVY\nid 20312\nstart level 1-\n"
                          "on use ITEM_T_IVYTRAP trap status 1 7 target MOB_T_INFORMANT full @SN_T_FULL got @SN_T_GOT capture-item ITEM_T_INFORMANT "
                          "minutes 30 roll 101 step 3 below raise MOB_T_GUARDIAN 0 20 already @SN_T_ALREADY ran @SN_T_RAN owner-died @SN_T_RAN\n"
                          "goal drop 1 ITEM_T_INFORMANT at NPC_T_GIVER\ngoal kill 15 MOB_T_GUARDIAN\n")});
    host.who.position = {25000, 100.0f, 0.0f, 100.0f, 0};

    SUBCASE("GORIA_6's: silent without the quest or for another monster; a full bag; caught; one held; its death's five minutes") {
        quests.Enter(1, {});
        quests.Trapped(1, 55, "MOB_T_ARCHER");
        CHECK(host.said.empty());
        CHECK(host.taken.empty());
        quests.Enter(1, {UnderWay(20310)});
        quests.Trapped(1, 55, "MOB_T_WOLF");
        CHECK(host.said.empty());
        host.freePlace = false;
        quests.Trapped(1, 55, "MOB_T_ARCHER");
        CHECK(host.LastSaid() == "SN_T_FULL");
        CHECK(host.taken.empty());
        host.freePlace = true;
        quests.Trapped(1, 55, "MOB_T_ARCHER");
        CHECK(host.bag["ITEM_T_HUN"] == 1);
        CHECK(host.taken == std::vector<std::uint32_t>{55}); // out of the world without dying
        CHECK(quests.Caught(1, 20310) == std::pair<std::uint32_t, std::uint32_t>{0, 20});
        CHECK(host.saved[20310].progress[server::Quests::kCatchMarkSlot] == server::Quests::kCaught);
        CHECK(quests.Record(1, 20310)->status == proto::QuestStatus::Achieved); // its gathering met by the caught item
        // Its goal met (state 8, not 7 or 1): another catch is silent (the handler asks the state first).
        host.said.clear();
        quests.Trapped(1, 56, "MOB_T_ARCHER");
        CHECK(host.said.empty());
        CHECK(host.taken.size() == 1);
        // Its holder's death: every one taken, its words, the minutes left at 5 unmarked; the trap refused till they run out,
        // then the time's words again (the original's quirk).
        quests.Died(1);
        CHECK(host.LastSaid() == "SN_T_DIED");
        CHECK(host.bag["ITEM_T_HUN"] == 0);
        CHECK(quests.Caught(1, 20310).second == 5);
        CHECK(host.saved[20310].progress[server::Quests::kCatchMarkSlot] == 0);
        quests.Trapped(1, 57, "MOB_T_ARCHER");
        CHECK(host.LastSaid() == "SN_T_ALREADY");
        quests.Tick(std::chrono::steady_clock::now() + 5min + 1s);
        CHECK(host.LastSaid() == "SN_T_RAN");
        CHECK(host.saved[20310].progress[server::Quests::kCatchMarkSlot] == server::Quests::kLost);
        quests.Trapped(1, 57, "MOB_T_ARCHER");
        CHECK(host.bag["ITEM_T_HUN"] == 1);
        // Its minutes out: the warnings, then every caught one taken.
        quests.Tick(std::chrono::steady_clock::now() + 10min + 1s);
        CHECK(host.LastSaid() == "SN_T_W10");
        quests.Tick(std::chrono::steady_clock::now() + 20min + 1s);
        CHECK(host.LastSaid() == "SN_T_RAN");
        CHECK(host.bag["ITEM_T_HUN"] == 0);
    }
    SUBCASE("a caught item's minutes kept over a relog (its display only), still counted in the world") {
        quests.Enter(1, {UnderWay(20310)});
        quests.Trapped(1, 55, "MOB_T_ARCHER");
        quests.Tick(std::chrono::steady_clock::now() + 4min + 1s);
        quests.Leave(1);
        quests.Enter(1, host.Records());
        CHECK(host.resummoned.empty());
        CHECK(quests.Caught(1, 20310).second == 16);
        CHECK(quests.Gauge(1, 20310) > 15 * 60);
    }
    SUBCASE("GENERAL_1's net: a lost roll's words, the monster left") {
        quests.Enter(1, {UnderWay(20311)});
        quests.Trapped(1, 60, "MOB_T_ROGUE");
        CHECK(host.LastSaid() == "SN_T_FAILED");
        CHECK(host.taken.empty());
        CHECK(host.bag["ITEM_T_PIRATE"] == 0);
    }
    SUBCASE("IVY_1's informant: caught with its words; each minute a guardian within 20 of its holder, none in a town") {
        quests.Enter(1, {UnderWay(20312)});
        quests.Trapped(1, 70, "MOB_T_INFORMANT");
        CHECK(host.LastSaid() == "SN_T_GOT");
        CHECK(host.bag["ITEM_T_INFORMANT"] == 1);
        quests.Tick(std::chrono::steady_clock::now() + 3min + 1s);
        REQUIRE(host.raised.size() == 3);
        for (const auto& [monster, at] : host.raised) {
            CHECK(monster == "MOB_T_GUARDIAN");
            CHECK(Flat(at, host.who.position) <= 20.1);
        }
        host.who.town = true;
        quests.Tick(std::chrono::steady_clock::now() + 5min + 1s);
        CHECK(host.raised.size() == 3);
        CHECK(quests.Caught(1, 20312).second == 25);
    }
}

TEST_CASE("Quest captures: the daily resupply at the giver, \"come again tomorrow\", and the tools taken at the hand-in (a made-up host)") {
    CaptureHost host;
    server::Quests quests(host, {});
    quests.Load({ParseOne("quest QNO_T_DAILY\nid 20320\nstart level 1-\ngiver NPC_T_GIVER\nfinish NPC_T_GIVER\nstart-item ITEM_T_NET 5\n"
                          "resupply ITEM_T_NET 5\nresupply-daily start\npay-take ITEM_T_NET\npay-take ITEM_T_CAUGHT\n"
                          "goal drop 1 ITEM_T_CAUGHT at NPC_T_GIVER\n"
                          "talk NPC_T_GIVER resupply @SN_T_MORE\ntalk NPC_T_GIVER resupply-spent @SN_T_TOMORROW\n"),
                 ParseOne("quest QNO_T_ROPES\nid 20321\nstart level 1-\ngiver NPC_T_GIVER\nfinish NPC_T_GIVER\nstart-item ITEM_T_ROPE 5\n"
                          "resupply ITEM_T_ROPE 5\nresupply-daily\ngoal drop 1 ITEM_T_THIEF at NPC_T_GIVER\n"),
                 ParseOne("quest QNO_T_EVERY\nid 20322\nstart level 1-\ngiver NPC_T_GIVER\nfinish NPC_T_GIVER\nstart-item ITEM_T_FEED 5\n"
                          "resupply ITEM_T_FEED 5\ngoal drop 1 ITEM_T_GOAT at NPC_T_GIVER\n")});
    quests.Enter(1, {});
    const auto talk = [&](std::uint32_t quest) {
        quests.Handle(1, proto::QuestTalk::kId, proto::Encode(proto::QuestTalk{9, quest}));
        return host.LastMoment();
    };
    SUBCASE("its start counts as the day's: none held the same day, come again tomorrow; the next day five, once") {
        quests.Handle(1, proto::QuestAccept::kId, proto::Encode(proto::QuestAccept{9, 20320}));
        REQUIRE(host.bag["ITEM_T_NET"] == 5);
        CHECK(host.saved[20320].progress[server::Quests::kSupplyDaySlot] == 100);
        host.bag["ITEM_T_NET"] = 0;
        CHECK(talk(20320) == "resupply-spent");
        CHECK(host.bag["ITEM_T_NET"] == 0);
        host.day = 101;
        CHECK(talk(20320) == "resupply");
        CHECK(host.bag["ITEM_T_NET"] == 5);
        CHECK(host.saved[20320].progress[server::Quests::kSupplyDaySlot] == 101);
        host.bag["ITEM_T_NET"] = 0;
        CHECK(talk(20320) == "resupply-spent");
        // A full bag spends the day all the same (the day recorded as they are offered, EASTEU_5's).
        host.day = 102;
        host.freePlace = false;
        CHECK(talk(20320) == "bag-full");
        host.freePlace = true;
        CHECK(talk(20320) == "resupply-spent");
        // The hand-in takes every net left and every caught one.
        host.bag["ITEM_T_NET"] = 3;
        host.bag["ITEM_T_CAUGHT"] = 2;
        quests.BagChanged(1);
        quests.Handle(1, proto::QuestHandIn::kId, proto::Encode(proto::QuestHandIn{9, 20320, {}}));
        CHECK(quests.Record(1, 20320)->status == proto::QuestStatus::Done);
        CHECK(host.bag["ITEM_T_NET"] == 0);
        CHECK(host.bag["ITEM_T_CAUGHT"] == 0);
    }
    SUBCASE("its start not counted: the same day once more; one taken every time without `resupply-daily`") {
        quests.Handle(1, proto::QuestAccept::kId, proto::Encode(proto::QuestAccept{9, 20321}));
        host.bag["ITEM_T_ROPE"] = 0;
        CHECK(talk(20321) == "resupply");
        CHECK(host.bag["ITEM_T_ROPE"] == 5);
        host.bag["ITEM_T_ROPE"] = 0;
        CHECK(talk(20321) == "resupply-spent");
        quests.Handle(1, proto::QuestAccept::kId, proto::Encode(proto::QuestAccept{9, 20322}));
        for (int i = 0; i < 3; ++i) {
            host.bag["ITEM_T_FEED"] = 0;
            CHECK(talk(20322) == "resupply");
            CHECK(host.bag["ITEM_T_FEED"] == 5);
        }
    }
}

TEST_CASE("Game server: a trap set from the bag catches the monster that comes into it; the quest's event gives the caught item (game files)") {
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
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    const assets::ItemRow* trap = items->Find(std::string_view("ITEM_QNO_CA_GORIA_6_01"));
    const assets::ItemRow* caught = items->Find(std::string_view("ITEM_QNO_CA_GORIA_6_02"));
    const assets::ObjectRow* archer = characters->Find(std::string_view("MOB_QT_01_HUNARCHER_CLON"));
    REQUIRE(trap != nullptr);
    REQUIRE(caught != nullptr);
    REQUIRE(archer != nullptr);
    const assets::SkillRow* skill = skills->Find(std::string_view(trap->paramTexts[0]));
    REQUIRE(skill != nullptr);
    CHECK(server::Skills::IsQuestTrap(*skill)); // SKILL_QNO_CA_GORIA_6_01_01: 'qest' 1 14922, 'trap'
    // GORIA_6's trap by hand with a sure roll (the original's 50 in 101 is the made-up host's case above).
    REQUIRE_MESSAGE(server::SaveQuest(t.Db(),
                                      ParseOne("quest QNO_T_GTRAP\nid 20340\nstart level 1-\ngiver NPC_T_NOBODY\n"
                                               "on use ITEM_QNO_CA_GORIA_6_01 trap status 1 7 target MOB_QT_01_HUNARCHER_CLON full @SN_TALK_QNO_CA_GORIA_6_13 "
                                               "capture-item ITEM_QNO_CA_GORIA_6_02 minutes 20 roll 101 step 3 below ran @SN_TALK_QNO_CA_GORIA_6_15\n"
                                               "goal drop 1 ITEM_QNO_CA_GORIA_6_02 at NPC_T_NOBODY\n"),
                                      server::QuestOrigin::Ours, error),
                    error);
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
    config.skills = skills;
    // The Huns' garrison (region 26991); its Hun magic archers put out by the developer's spawn (a still lab monster).
    const proto::WorldPosition place{26991, 1154.0f, 142.0f, 1506.0f, 0};
    config.devCommands = true;
    server::GameServer game;
    REQUIRE_MESSAGE(game.Start(config, error), error);
    REQUIRE(server::CreateAccount(t.Db(), "Trapperacct", "secret1", error) == server::CreateAccountResult::Created);
    bool ok = false;
    const std::int64_t account = server::FindAccount(t.Db(), "Trapperacct", ok)->id;
    test::Client c(net, game.Port(), [&] { game.Poll(2ms); });
    REQUIRE(c.Greet());
    proto::LobbyEnter e;
    e.ticket = server::IssueTicket(t.Db(), account, config.id, 30, error);
    c.Send(e);
    proto::LobbyEntered entered;
    REQUIRE(c.Expect(entered));
    proto::CharacterCreate m;
    m.name = "Trapper";
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
    REQUIRE(server::SaveProgress(t.Db(), {{character, 40, 0, 0, 0, 40, 40, 0}}));
    REQUIRE(server::SavePlaces(t.Db(), {{character, {place.region, place.x, place.y, place.z, 0.0f}}}));
    REQUIRE(server::SaveVitals(t.Db(), character, 1'000'000, 100'000));
    REQUIRE(t.Db().Query("INSERT INTO _charquest (charid, questid, status, achievementcount, starttime, endtime, questdata1, questdata2) "
                         "VALUES ($1, 20340, 1, 0, now(), now(), 0, 0)",
                         {character})
                .Ok());
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {account}).Ok());
    server::StoredItem traps;
    traps.refId = trap->refObjId;
    traps.quantity = 5;
    traps.counted = true;
    REQUIRE(server::GiveItems(t.Db(), config.id, "Trapper", {traps}, 0, error) == server::GiveResult::Done);
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, $2)", {account, int(config.id)}).Ok());
    c.Send(proto::CharacterSelect{"Trapper"});
    REQUIRE(c.Expect(r));
    c.Send(proto::WorldEnter{});
    proto::WorldEntered w;
    REQUIRE(c.Expect(w));
    REQUIRE(w.status == proto::WorldStatus::Ok);
    proto::InventoryList bag;
    REQUIRE(c.Expect(bag));
    std::uint8_t slot = 0;
    for (const proto::ItemRecord& i : bag.items)
        if (i.item == trap->refObjId) slot = i.slot;
    REQUIRE(slot != 0);
    std::map<std::uint32_t, proto::EntityAppeared> seen;
    std::set<std::uint32_t> gone;
    std::vector<proto::ItemRecord> changed;
    const auto pump = [&](auto&& until, std::chrono::milliseconds most) {
        const auto end = std::chrono::steady_clock::now() + most;
        while (std::chrono::steady_clock::now() < end && !until()) {
            proto::EntityAppeared a;
            if (c.Expect(a, 5ms)) seen[a.entity] = a;
            proto::EntityGone g;
            if (c.Expect(g, 5ms)) gone.insert(g.entity);
            proto::ItemMoved moved;
            if (c.Expect(moved, 5ms)) changed.insert(changed.end(), moved.changed.begin(), moved.changed.end());
        }
        return until();
    };
    // The navigation around the player in, then an archer 3 away: the trap refused (0x3038: a monster within 5 times its
    // radius), kept.
    const auto spawn = [&](std::uint32_t tag) {
        proto::DevSpawned made;
        for (int i = 0; i < 100 && made.entity == 0; ++i) {
            c.Send(proto::DevSpawn{archer->codename, 3.0f, 0.0f, proto::LabBehaviour::Still, 0, tag});
            REQUIRE(c.Expect(made, 5s));
            if (made.entity == 0) pump([] { return false; }, 100ms);
        }
        return made.entity;
    };
    const std::uint32_t near = spawn(1);
    REQUIRE(near != 0);
    proto::UseItem use;
    use.slot = slot;
    c.Send(use);
    proto::ItemRefused tooNear;
    REQUIRE(c.Expect(tooNear, 5s));
    CHECK(tooNear.code == 0x3038);
    // That archer gone, the trap set; an archer at its feet then: taken out of the world without dying, the caught Hun in
    // the bag, one trap used up.
    c.Send(proto::DevRemove{near});
    pump([] { return false; }, 500ms); // (its removal: it may not have been seen yet, so no EntityGone)
    c.Send(use);
    pump([] { return false; }, 200ms);
    const std::uint32_t target = spawn(2);
    REQUIRE(target != 0);
    const auto holds = [&](std::uint32_t item) {
        return std::any_of(changed.begin(), changed.end(), [&](const proto::ItemRecord& i) { return i.item == item; });
    };
    // (Caught perhaps before the client saw it: then no EntityGone; seen, it is gone.)
    REQUIRE(pump([&] { return (!seen.contains(target) || gone.contains(target)) && holds(caught->refObjId); }, 10s));
    // Its COOLTIME (60 s): a second trap at once is refused.
    c.Send(use);
    proto::ItemRefused refused;
    REQUIRE(c.Expect(refused, 5s));
    CHECK(refused.code == game::kCoolTimeRefused);
    // The record keeps the catch (20 minutes, caught).
    const auto data = [&](const char* column) {
        return t.Db().Query(std::string("SELECT ") + column + " FROM _charquest WHERE charid = $1 AND questid = 20340", {character}).Int(0, 0);
    };
    bool kept = false;
    for (int i = 0; i < 200 && !kept; ++i) {
        game.Poll(2ms);
        kept = ((std::uint64_t(data("questdata1")) >> 32) & 0xFFFF) == 20 && ((std::uint64_t(data("questdata2")) >> 16) & 0xFFFF) == server::Quests::kCaught;
    }
    CHECK(kept);
}

TEST_CASE("Quest captures: one roll, several catches (the hwan tests' 1_4_1): the first under its bound, the monster stays") {
    CaptureHost host;
    server::Quests quests(host, {});
    // Bounds 101 (always) and 0 (never): every catch the first's; a second quest with bounds 0 and 0: always `failed`.
    quests.Load({ParseOne("quest QNO_T_SPIRIT\nid 20320\nstart level 1-\n"
                          "on use ITEM_T_SPIRITTRAP trap status 1 7 target MOB_T_BONEGENERAL full @SN_T_FULL failed @SN_T_NONE "
                          "capture-outcomes outcome ITEM_T_NORMAL 101 @SN_T_NORMAL outcome ITEM_T_FLUTTER 0 @SN_T_FLUTTER stays\n"
                          "goal drop 1 ITEM_T_NORMAL at NPC_T_GIVER\n"),
                 ParseOne("quest QNO_T_MISS\nid 20321\nstart level 1-\n"
                          "on use ITEM_T_MISSTRAP trap status 1 7 target MOB_T_GHOST failed @SN_T_NONE "
                          "capture-outcomes outcome ITEM_T_NORMAL 0 @SN_T_NORMAL outcome ITEM_T_FLUTTER 0 @SN_T_FLUTTER\n"
                          "goal drop 1 ITEM_T_GHOSTLY at NPC_T_GIVER\n")}); // a goal not met (a quest with none counts as met: state 8)
    host.who.position = {25000, 100.0f, 0.0f, 100.0f, 0};
    quests.Enter(1, {UnderWay(20320), UnderWay(20321)});
    // The text round trip.
    CHECK(ParseOne(game::WriteQuest(*quests.Find(20320))) == *quests.Find(20320));
    // A full bag: its words, nothing given.
    host.freePlace = false;
    quests.Trapped(1, 70, "MOB_T_BONEGENERAL");
    CHECK(host.LastSaid() == "SN_T_FULL");
    CHECK(host.bag["ITEM_T_NORMAL"] == 0);
    host.freePlace = true;
    // Caught: the first outcome's item and words; the general stays in the world.
    quests.Trapped(1, 70, "MOB_T_BONEGENERAL");
    CHECK(host.bag["ITEM_T_NORMAL"] == 1);
    CHECK(host.bag["ITEM_T_FLUTTER"] == 0);
    CHECK(host.LastSaid() == "SN_T_NORMAL");
    CHECK(host.taken.empty());
    // No outcome under its bound: `failed`, nothing given.
    quests.Trapped(1, 71, "MOB_T_GHOST");
    CHECK(host.LastSaid() == "SN_T_NONE");
    CHECK(host.bag["ITEM_T_FLUTTER"] == 0);
    // The original's two rules, one a country.
    for (const char* code : {"QNO_CH_HWAN_1_4_1", "QNO_EU_HWAN_1_4_1"}) {
        CAPTURE(code);
        const auto it = std::find_if(game::OriginalUseRules().begin(), game::OriginalUseRules().end(), [&](const game::OriginalUseRule& r) { return r.quest == code; });
        REQUIRE(it != game::OriginalUseRules().end());
        game::QuestDef def = ParseOne(std::string("quest ") + code + "\nid 1\n");
        CHECK(game::AddOriginalUseRules(def) == 1);
        REQUIRE(def.onUse.size() == 1);
        REQUIRE(def.onUse[0].capture);
        CHECK(def.onUse[0].capture->stays);
        REQUIRE(def.onUse[0].capture->outcomes.size() == 2);
        CHECK(def.onUse[0].capture->outcomes[0].item == "ITEM_QNO_CH_HWAN_1_4_01");
        CHECK(def.onUse[0].capture->outcomes[0].below == 2);
        CHECK(def.onUse[0].capture->outcomes[1].item == "ITEM_QNO_CH_HWAN_1_4_02");
        CHECK(def.onUse[0].capture->outcomes[1].below == 30);
    }
}
