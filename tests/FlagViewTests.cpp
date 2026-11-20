// Capture the Flag as the client sees it (game/FlagView.h): each
// notice's line as the 1.188 client's handler shows it, the timer, the doors, the teams, So-Ok's rows;
// and the messages through the session's decode (the world's events, recorded and replayed).

#include <doctest/doctest.h>

#include <variant>

#include "sr/game/FlagView.h"
#include "sr/game/WorldRecording.h"

using namespace sr::game;

namespace {

proto::FlagWorldNotice State(FlagState s, std::uint32_t value = 0) { return {0, std::uint8_t(s), value, 0, {}}; }
proto::FlagWorldNotice Code(std::uint8_t code, std::uint32_t value = 0, std::uint8_t team = 0, std::string name = {}) {
    return {1, code, value, team, std::move(name)};
}
std::string KeyOf(const FlagView::Outcome& o) { return o.line ? o.line->key : std::string(); }

} // namespace

TEST_CASE("flag view: the state broadcasts' lines and the timer") {
    FlagView v;
    CHECK(KeyOf(v.Apply(State(FlagState::Open), 1, 0.0)) == "UIIT_MSG_FLAGWAR_STANDBY_10");
    CHECK(KeyOf(v.Apply(State(FlagState::Standby5), 1, 0.0)) == "UIIT_MSG_FLAGWAR_STANDBY_5");
    CHECK(KeyOf(v.Apply(State(FlagState::Standby1), 1, 0.0)) == "UIIT_MSG_FLAGWAR_STANDBY_1");
    CHECK_FALSE(v.Apply(State(FlagState::Closed), 1, 0.0).line);
    CHECK(KeyOf(v.Apply(State(FlagState::Started), 1, 0.0)) == "UIIT_MSG_FLAGWAR_GAME_START");
    // Every line of the common form is on the banner too.
    CHECK(v.Apply(State(FlagState::Started), 1, 0.0).line->banner);
    for (int code : {6, 7, 0x0B, 0x0C, 0x0F, 0x20, 0xFE}) CHECK_FALSE(v.Apply(State(FlagState(code)), 1, 0.0).line);

    // State 8 with another world: nothing; with the match world: the line and 20:00.
    CHECK_FALSE(v.Apply(State(FlagState::MatchWorld, 7), 1, 10.0).line);
    CHECK_FALSE(v.TimerShown(10.0));
    CHECK(KeyOf(v.Apply(State(FlagState::MatchWorld, flag::kWorld), 1, 10.0)) == "UIIT_MSG_FLAGWAR_GAME_START_MISSION");
    CHECK(v.TimerShown(10.0));
    CHECK(FlagView::TimerText(v.TimerLeft(10.0)) == "20:00");
    CHECK(FlagView::TimerText(v.TimerLeft(10.5)) == "20:00");
    CHECK(FlagView::TimerText(v.TimerLeft(11.0)) == "19:59");
    CHECK(FlagView::TimerText(v.TimerLeft(23.0)) == "19:47");
    // FF 0E: 1,200,000 - elapsed.
    CHECK_FALSE(v.Apply(Code(0x0E, 60'000), 1, 100.0).line);
    CHECK(FlagView::TimerText(v.TimerLeft(100.0)) == "19:00");
    // State 5 hides it; FF 0E shows it again; 9 hides it with its line.
    v.Apply(State(FlagState::Stopped), 1, 101.0);
    CHECK_FALSE(v.TimerShown(101.0));
    v.Apply(Code(0x0E, 1'190'000), 1, 102.0);
    CHECK(v.TimerShown(102.0));
    CHECK(FlagView::TimerText(v.TimerLeft(102.0)) == "00:10");
    CHECK(v.TimerShown(112.5)); // run out: a second more (the absent "end" picture)
    CHECK_FALSE(v.TimerShown(113.5));
    v.Apply(Code(0x0E, 0), 1, 200.0);
    CHECK(KeyOf(v.Apply(State(FlagState::Ended), 1, 201.0)) == "UIIT_MSG_FLAGWAR_GAME_END");
    CHECK_FALSE(v.TimerShown(201.0));
    // A new world without a flag team hides it; with one (a teleport into the match) it stays.
    v.Apply(Code(0x0E, 0), 1, 300.0);
    v.Entered(1);
    CHECK_FALSE(v.TimerShown(300.0));
    v.Apply(proto::EntityFlagTeam{1, 1});
    v.Apply(Code(0x0E, 0), 1, 300.0);
    v.Entered(1);
    CHECK(v.TimerShown(300.0));
    CHECK(v.InMatch(1));

    // 0x0A: the actor's bar, no line.
    const FlagView::Outcome bar = v.Apply(State(FlagState::Action, 77), 1, 0.0);
    CHECK(bar.actionBar == 77);
    CHECK_FALSE(bar.line);
}

TEST_CASE("flag view: the codes' lines") {
    FlagView v;
    v.Apply(proto::EntityFlagTeam{1, 0});
    v.Apply(proto::EntityFlagTeam{2, 1});
    // FF 00 by the row drawn: the registration's (state 1) -> accepted, the cancel's (2) -> cancelled.
    v.RowDrawn(1);
    CHECK(KeyOf(v.Apply(Code(0), 1, 0.0)) == "UIIT_MSG_FLAGWAR_GAME_JOIN_SUCCES");
    v.RowDrawn(2);
    CHECK(KeyOf(v.Apply(Code(0), 1, 0.0)) == "UIIT_MSG_FLAGWAR_GAME_JOIN_CANCEL");
    CHECK(KeyOf(v.Apply(Code(4), 1, 0.0)) == "UIIT_MSG_FLAGWAR_GAME_START_NOT_ENTER");
    CHECK(KeyOf(v.Apply(Code(5), 1, 0.0)) == "UIIT_MSG_FLAGWAR_GAME_JOIN_LEVEL_ERORR");
    CHECK(KeyOf(v.Apply(Code(6), 1, 0.0)) == "UIIT_MSG_FLAGWAR_GAME_CANCEL");
    CHECK(KeyOf(v.Apply(Code(8), 1, 0.0)) == "UIIT_MSG_FLAGWAR_FLAGZONE_CANNOT_ENTER");
    const auto ours = v.Apply(Code(9, 0, 0, "Hero"), 1, 0.0);
    CHECK(ours.line == FlagView::Line{"UIIT_MSG_FLAGWAR_PICKUP_FLAG_OURTEAM", "Hero", true});
    CHECK(KeyOf(v.Apply(Code(0x0A), 1, 0.0)) == "UIIT_MSG_FLAGWAR_PICKUP_FLAG_OTHERTEAM");
    CHECK(KeyOf(v.Apply(Code(0x0F), 1, 0.0)) == "UIIT_MSG_FLAGWAR_FLAG_DROP");
    CHECK(KeyOf(v.Apply(Code(0x11), 1, 0.0)) == "UIIT_MSG_FLAGWAR_GAME_WIN");
    CHECK(KeyOf(v.Apply(Code(0x12), 1, 0.0)) == "UIIT_MSG_FLAGWAR_PLAYER_FLAG_PICKUP");
    CHECK(KeyOf(v.Apply(Code(0x13), 1, 0.0)) == "UIIT_MSG_FLAGWAR_PLAYER_FLAG_PICKUP");
    CHECK(KeyOf(v.Apply(Code(0x16), 1, 0.0)) == "UIIT_MSG_FLAGWAR_GAME_LOSE");
    CHECK(KeyOf(v.Apply(Code(0x17), 1, 0.0)) == "UIIT_MSG_FLAGWAR_GAME_DRAW");
    // FF 14 / 15: the system line alone.
    for (std::uint8_t code : {std::uint8_t(0x14), std::uint8_t(0x15)}) {
        const auto o = v.Apply(Code(code), 1, 0.0);
        REQUIRE(o.line);
        CHECK(o.line->key == "UIIT_MSG_FLAGWAR_GAME_ENTERD_ERORR");
        CHECK_FALSE(o.line->banner);
    }
    // Nothing: 01, 02, 03, 07, 0D (the revival), 0x18 and above.
    for (int code : {1, 2, 3, 7, 0x0D, 0x18, 0x40, 0xFF}) CHECK_FALSE(v.Apply(Code(std::uint8_t(code)), 1, 0.0).line);
    // FF 0C: the card's box, no line.
    const auto card = v.Apply(Code(0x0C), 1, 0.0);
    CHECK(card.cardBox);
    CHECK_FALSE(card.line);

    // FF 0B: the door open for everyone; the line to the taker's team only.
    CHECK_FALSE(v.DoorOpen(233));
    CHECK(v.Apply(Code(0x0B, 233, 0, "Hero"), 1, 0.0).line == FlagView::Line{"UIIT_MSG_FLAGWAR_PICKUP_MASTERKEY", "Hero", true});
    CHECK(v.DoorOpen(233));
    CHECK_FALSE(v.Apply(Code(0x0B, 232, 1, "Zed"), 1, 0.0).line);
    CHECK(v.DoorOpen(232));
    // State 4 closes them all.
    v.Apply(State(FlagState::Started), 1, 0.0);
    CHECK_FALSE(v.DoorOpen(233));
    CHECK_FALSE(v.DoorOpen(232));
    // FF 10: the own team's placer by name; the other team's: the failure, no name.
    CHECK(v.Apply(Code(0x10, 0, 0, "Hero"), 1, 0.0).line == FlagView::Line{"UIIT_MSG_FLAGWAR_FLAG_MISSION_CLEAR", "Hero", true});
    CHECK(v.Apply(Code(0x10, 0, 1, "Zed"), 1, 0.0).line == FlagView::Line{"UIIT_MSG_FLAGWAR_FLAG_MISSION_FAIL", "", true});
}

TEST_CASE("flag view: the teams and So-Ok's rows") {
    FlagView v;
    CHECK_FALSE(v.InMatch(1));
    v.Apply(proto::EntityFlagTeam{1, 0});
    v.Apply(proto::EntityFlagTeam{2, 1});
    v.Apply(proto::EntityFlagTeam{3, 0});
    CHECK(v.InMatch(1));
    CHECK(v.Enemy(1, 2));
    CHECK_FALSE(v.Enemy(1, 3));
    CHECK_FALSE(v.Enemy(1, 4)); // no flag team
    CHECK_FALSE(v.Enemy(1, 1));
    v.Apply(proto::EntityFlagTeam{2, 0xFF});
    CHECK_FALSE(v.Enemy(1, 2));
    v.Forget(3);
    CHECK(v.TeamOf(3) == FlagView::kNoTeam);
    v.Entered(1); // the others forgotten, the player's own kept
    CHECK(v.InMatch(1));
    CHECK(v.TeamOf(2) == FlagView::kNoTeam);

    CHECK(FlagView::DeskRowKey(1) == "UIIT_MSG_FLAGWAR_GAME_JOIN_REQ");
    CHECK(FlagView::DeskRowKey(2) == "UIIT_MSG_FLAGWAR_GAME_JOIN_REQ_CANCEL");
    CHECK(FlagView::DeskRowKey(0).empty());
    CHECK(FlagView::RowRefusal(19)->key == "UIIT_MSG_FLAGWAR_GAME_JOIN_LEVEL_ERORR");
    CHECK_FALSE(FlagView::RowRefusal(20));
    proto::EventDesk desk;
    desk.npc = 9;
    desk.flagState = 2;
    desk.quests.push_back({10145, "SN_QEV_CH_EVENT_KISAENG_081201"});
    v.Apply(desk);
    CHECK(v.Desk().npc == 9);
    v.ClearDesk();
    CHECK(v.Desk().npc == 0);
}

TEST_CASE("flag view: the event messages are the world's, decoded and replayed") {
    CHECK(IsWorldMessage(proto::FlagWorldNotice::kId));
    CHECK(IsWorldMessage(proto::EntityFlagTeam::kId));
    CHECK_FALSE(IsWorldMessage(proto::FlagWorldRegister::kId)); // the client's own requests
    CHECK(std::string(WorldMessageName(proto::EventSays::kId)) == "EventSays");
    TitleEvent event;
    REQUIRE(DecodeWorldEvent(proto::FlagWorldNotice::kId, proto::Encode(proto::FlagWorldNotice{1, 0x09, 0, 0, "Hero"}), event));
    const auto* e = std::get_if<proto::EventEvent>(&event);
    REQUIRE(e != nullptr);
    CHECK(std::get<proto::FlagWorldNotice>(*e).name == "Hero");
    proto::EventSays says{9, 10145, "SN_TALK_QEV_CH_EVENT_KISAENG_070919_A", {"SN_TALK_QEV_CH_EVENT_KISAENG_070919_B", "SN_TALK_COMMON_EXIT"}};
    REQUIRE(DecodeWorldEvent(proto::EventSays::kId, proto::Encode(says), event));
    CHECK(std::get<proto::EventSays>(std::get<proto::EventEvent>(event)).rows.size() == 2);
}
