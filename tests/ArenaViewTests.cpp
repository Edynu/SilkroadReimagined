// The Battle Arena as the client sees it (game/ArenaView.h): each notice's lines as
// the 1.188 client's handlers (the broadcasts, the FF codes) show them, the arena state and its
// windows, the scores and the top five, the result, the timer, the manager's rows and the rule window's kinds; and the
// messages through the session's decode.

#include <doctest/doctest.h>

#include <variant>

#include "sr/game/ArenaView.h"
#include "sr/game/WorldRecording.h"

using namespace sr::game;

namespace {

proto::ArenaNotice Broadcast(std::uint8_t code, std::uint8_t mode = 0, std::uint16_t kinds = 0, std::uint8_t kind = 0, std::uint32_t value = 0) {
    proto::ArenaNotice n;
    n.code = code;
    n.mode = mode;
    n.kinds = kinds;
    n.kind = kind;
    n.value = value;
    return n;
}
proto::ArenaNotice Code(ArenaCode code, std::uint32_t value = 0, std::uint8_t team = 0, std::string name = {}, std::uint32_t value2 = 0,
                        std::uint8_t mode = 0, std::uint8_t kind = 0) {
    proto::ArenaNotice n;
    n.personal = 1;
    n.code = std::uint8_t(code);
    n.value = value;
    n.value2 = value2;
    n.team = team;
    n.name = std::move(name);
    n.mode = mode;
    n.kind = kind;
    return n;
}
std::vector<std::string> Keys(const ArenaView::Outcome& o) {
    std::vector<std::string> keys;
    for (const auto& l : o.lines) keys.push_back(l.key);
    return keys;
}
using Arg = ArenaView::Arg;

} // namespace

TEST_CASE("arena view: the broadcasts' lines") {
    ArenaView v;
    // Registration open with both kinds: a line for each (score first), the big notice, the way to take part.
    const auto open = v.Apply(Broadcast(2, 0, 0x60), 0.0);
    REQUIRE(open.lines.size() == 4);
    CHECK(open.lines[0].key == "UIIT_MGS_ARENA_JOIN_GUIDANCE_DISPLAY");
    CHECK(open.lines[0].args == std::vector<Arg>{{"UIIT_STT_ARENA_RANDOMMATCH", true}, {"UIIT_STT_ARENA_SCORE", true}});
    CHECK(open.lines[1].args[1].text == "UIIT_STT_ARENA_FLAG");
    CHECK(open.lines[2].key == "UIIT_MGS_ARENA_JOIN_GUIDANCE_DISPLAY_NOTIFY");
    CHECK(open.lines[2].notice);
    CHECK_FALSE(open.lines[0].notice);
    CHECK(open.lines[3].key == "UIIT_MGS_ARENA_JOIN_GUIDANCE");
    // The guild mode with score alone.
    CHECK(v.Apply(Broadcast(2, 2, 0x20), 0.0).lines[0].args[0].text == "UIIT_STT_ARENA_GUILD");
    CHECK(v.Apply(Broadcast(2, 2, 0x20), 0.0).lines.size() == 3);

    CHECK(Keys(v.Apply(Broadcast(3), 0.0)) == std::vector<std::string>{"UIIT_MGS_ARENA_JOIN_WINDOW_COMPLETION"});
    const auto warn5 = v.Apply(Broadcast(0x0D), 0.0);
    CHECK(Keys(warn5) == std::vector<std::string>{"UIIT_MGS_ARENA_START_ALARM", "UIIT_MSG_ARENA_GAME_ENTERD_ERORR"});
    CHECK(warn5.lines[0].args[0].text == "5");
    CHECK(v.Apply(Broadcast(0x0E), 0.0).lines[0].args[0].text == "1");
    const auto remind = v.Apply(Broadcast(0x0F, 3, 0x40), 0.0);
    CHECK(Keys(remind) == std::vector<std::string>{"UIIT_MGS_ARENA_JOIN_ING2", "UIIT_MGS_ARENA_JOIN_GUIDANCE"});
    CHECK(remind.lines[0].args == std::vector<Arg>{{"UIIT_STT_ARENA_JOB", true}, {"UIIT_STT_ARENA_FLAG", true}});
    CHECK(Keys(v.Apply(Broadcast(0x0B), 0.0)) == std::vector<std::string>{"UIIT_MGS_ARENA_JOIN_ING"});
    for (std::uint8_t quiet : {6, 7, 0x10, 0x11, 0x20}) CHECK(v.Apply(Broadcast(quiet), 0.0).lines.empty());
}

TEST_CASE("arena view: the state, its windows and the timer") {
    ArenaView v;
    CHECK_FALSE(v.InArena());
    // A score match: "start in 10 sec" makes the state with both windows, hidden.
    CHECK(v.Apply(Broadcast(4, 0, 0, arena::kScore), 0.0).lines.empty());
    REQUIRE(v.InArena());
    CHECK(v.Kind() == arena::kScore);
    CHECK(v.RankMade());
    CHECK_FALSE(v.ScoreShown());
    CHECK_FALSE(v.RankShown());
    CHECK_FALSE(v.TimerShown(0.0));
    // The play: shown, the timer at the length, its line.
    CHECK(Keys(v.Apply(Broadcast(8, 0, 0, arena::kScore, 270'000), 10.0)) == std::vector<std::string>{"UIIT_MGS_ARENA_START"});
    CHECK(v.ScoreShown());
    CHECK(v.RankShown());
    CHECK(v.TimerShown(10.0));
    CHECK(v.TimerLeft(10.0) == doctest::Approx(270.0));
    // The clock: length - elapsed.
    CHECK(v.Apply(Code(ArenaCode::Clock, 270'000, 0, {}, 30'000, 0, arena::kScore), 50.0).lines.empty());
    CHECK(v.TimerLeft(50.0) == doctest::Approx(240.0));
    // The scores and the top five (more than five kept at five).
    proto::ArenaScores s{12, 7, {}};
    for (int i = 0; i < 6; ++i) s.top.push_back({std::uint8_t(i % 2), "P" + std::to_string(i), std::uint32_t(10 - i)});
    v.Apply(s);
    CHECK(v.Score(0) == 12);
    CHECK(v.Score(1) == 7);
    CHECK(v.Top().size() == 5);
    CHECK(v.Top()[0].name == "P0");
    // The result, then the stop: everything gone, its line.
    const auto result = v.Apply(proto::ArenaResult{0, 1, 5, 186'760, 12, 7});
    CHECK(result.result);
    REQUIRE(v.Result());
    CHECK(v.Result()->coins == 5);
    CHECK(Keys(v.Apply(Broadcast(5), 400.0)) == std::vector<std::string>{"UIIT_MGS_ARENA_END"});
    CHECK_FALSE(v.InArena());
    CHECK_FALSE(v.Result());
    CHECK_FALSE(v.ScoreShown());
    CHECK_FALSE(v.TimerShown(400.0));
    CHECK(v.Score(0) == 0);

    // A flag match: no rank window; FF 88 sets the score window, shown.
    ArenaView f;
    f.Apply(Broadcast(4, 0, 0, arena::kFlag), 0.0);
    CHECK_FALSE(f.RankMade());
    f.Apply(proto::ArenaScores{1000, 3, {}});
    CHECK(f.ScoreShown());
    CHECK_FALSE(f.RankShown());
    CHECK(f.Score(0) == 1000);

    // A late arrival: the clock makes the state (its kind) and shows everything.
    ArenaView late;
    late.Apply(Code(ArenaCode::Clock, 600'000, 0, {}, 100'000, 0, arena::kFlag), 5.0);
    CHECK(late.InArena());
    CHECK(late.Kind() == arena::kFlag);
    CHECK(late.ScoreShown());
    CHECK(late.TimerLeft(5.0) == doctest::Approx(500.0));
    // Scores without the state fill nothing.
    ArenaView none;
    none.Apply(proto::ArenaScores{4, 4, {}});
    CHECK_FALSE(none.ScoreShown());
    CHECK(none.Score(0) == 0);
}

TEST_CASE("arena view: the codes' lines") {
    ArenaView v;
    const auto done = v.Apply(Code(ArenaCode::Done, 1, 0, {}, 0, 1), 0.0);
    CHECK(Keys(done) == std::vector<std::string>{"UIIT_MGS_ARENA_JOIN_WINDOW_COMPLETION_DISPLAY"});
    CHECK(done.lines[0].args[0].text == "UIIT_STT_ARENA_PARTY");
    CHECK(Keys(v.Apply(Code(ArenaCode::Done, 2), 0.0)) == std::vector<std::string>{"UIIT_MGS_ARENA_CANCEL_DISPLAY"});
    const std::pair<ArenaCode, const char*> lines[] = {
        {ArenaCode::Already, "UIIT_MGS_ARENA_ALREADY_JOIN_ERR"},
        {ArenaCode(3), "UIIT_MGS_ARENA_UNKNOWN_USER"},
        {ArenaCode::NotNow, "UIIT_MGS_ARENA_CANT_PARTICIPATE_RIGHT_NOW"},
        {ArenaCode::Level, "UIIT_MSG_FLAGWAR_GAME_JOIN_LEVEL_ERORR"},
        {ArenaCode::TooFew, "UIIT_MSG_FLAGWAR_GAME_CANCEL"},
        {ArenaCode::EnterFailed, "UIIT_MGS_ARENA_ENTRANCE_FAIL"},
        {ArenaCode::Conditions, "UIIT_MSG_ARENA_GAME_ENTERD_ERORR"},
        {ArenaCode::NotLeader, "UIIT_MGS_ARENA_CANT_PARTICIPATE_ENTRANCE_CONDITION_PARTY"},
        {ArenaCode::NotMaster, "UIIT_MGS_ARENA_CANT_PARTICIPATE_ENTRANCE_CONDITION_GUILD"},
        {ArenaCode::JobSuit, "UIIT_MGS_ARENA_CANT_PARTICIPATE_ENTRANCE_CONDITION"},
        {ArenaCode::TheirCarrier, "UIIT_MSG_FLAGWAR_PICKUP_FLAG_OTHERTEAM"},
        {ArenaCode::Dropped, "UIIT_MSG_FLAGWAR_FLAG_DROP"},
        {ArenaCode::TakeBusy, "UIIT_MSG_FLAGWAR_PLAYER_FLAG_PICKUP"},
        {ArenaCode::PlaceBusy, "UIIT_MGS_ARENA_FLAGWAR_PLAYER_PUTDOWN_FLAG"},
        {ArenaCode::FlagBlocked, "UIIT_MGS_ARENA_FLAGWAR_ERR_CANT_PICKING_FLAG_IN_SAFEZONE"},
        {ArenaCode::HasFlag, "UIIT_MGS_ARENA_ERR_FLAG_PICKUP"},
        {ArenaCode::BagFull, "UIIT_MSG_ARENA_REWARD_MESSAGE_ERR"},
    };
    for (const auto& [code, key] : lines) CHECK(Keys(v.Apply(Code(code), 0.0)) == std::vector<std::string>{key});
    CHECK(v.Apply(Code(ArenaCode::NotRegistered), 0.0).lines.empty()); // FF 01: no line

    const auto points = v.Apply(Code(ArenaCode::Points, 8), 0.0);
    CHECK(points.points == 8);
    CHECK(points.lines[0].key == "UIIT_MGS_ARENA_SCORE_ACQUISITION");
    CHECK(points.lines[0].args[0].text == "8");
    CHECK(v.Apply(Code(ArenaCode::OurCarrier, 0, 0, "Hero"), 0.0).lines[0].args[0].text == "Hero");
    // FF 84: team 0 the Red Team, else the Blue Team.
    CHECK(v.Apply(Code(ArenaCode::Placed, 0, 0, "Hero"), 0.0).lines[0].args == std::vector<Arg>{{"UIIT_MGS_ARENA_FLAGWAR_RED", true}, {"Hero", false}});
    CHECK(v.Apply(Code(ArenaCode::Placed, 0, 1, "Walker"), 0.0).lines[0].args[0].text == "UIIT_MGS_ARENA_FLAGWAR_BLUE");
    const auto bar = v.Apply(Code(ArenaCode::Action, 4242), 0.0);
    CHECK(bar.actionBar == 4242);
    CHECK(bar.lines.empty());
    CHECK(v.Apply(Code(ArenaCode::Revival), 0.0).lines[0].args[0].text == "10");
}

TEST_CASE("arena view: the manager's rows and the rule window") {
    // Registered: the cancel row; else the open mode's name with its kinds; mode 4: none.
    const auto cancel = ArenaView::DeskRowOf(arena::kRegistered);
    REQUIRE(cancel);
    CHECK(cancel->cancel);
    CHECK(cancel->key == "UIIT_STT_ARENA_NPC_CANCEL");
    const auto random = ArenaView::DeskRowOf(0x60);
    REQUIRE(random);
    CHECK_FALSE(random->cancel);
    CHECK(random->key == "UIIT_STT_ARENA_RANDOMMATCH");
    CHECK(random->kinds == 0x60);
    CHECK(ArenaView::DeskRowOf(0x22)->mode == 2);
    CHECK_FALSE(ArenaView::DeskRowOf(4));
    CHECK_FALSE(ArenaView::DeskRowOf(0x24));
    CHECK(ArenaView::ModeTalk(0) == "SN_NPC_BATTLE_ARENA_MANAGER_RANDOMMATCH_BS");
    CHECK(ArenaView::ModeTalk(3) == "SN_NPC_BATTLE_ARENA_MANAGER_JOB_BS");

    const auto both = ArenaView::KindRows(0x60);
    REQUIRE(both.size() == 2);
    CHECK(both[0].kind == arena::kScore);
    CHECK(both[1].kind == arena::kFlag);
    CHECK(both[1].description == "UIIT_MSG_ARENA_JOIN_WINDOW_DESCRIPTION_FLAG");
    CHECK(ArenaView::KindRows(0x40).size() == 1);
    CHECK(ArenaView::KindRows(0).empty());
    const auto confirm = ArenaView::Confirmation(0, arena::kFlag);
    CHECK(confirm.key == "UIIT_MSG_ARENA_JOIN_WINDOW_CONFIRMATION");
    CHECK(confirm.args == std::vector<Arg>{{"UIIT_STT_ARENA_RANDOMMATCH", true}, {"UIIT_STT_ARENA_FLAG", true}});

    // The request: the guild's members only in the guild mode.
    const auto random1 = ArenaView::Register(0, arena::kScore, {7, 8});
    CHECK(random1.action == 1);
    CHECK(random1.members.empty());
    const auto guild = ArenaView::Register(2, arena::kFlag, {7, 8});
    CHECK(guild.mode == 2);
    CHECK(guild.kind == arena::kFlag);
    CHECK(guild.members == std::vector<std::uint32_t>{7, 8});

    // The desk kept until the next menu.
    ArenaView v;
    v.Apply(proto::ArenaDesk{55, 0x20});
    CHECK(v.Desk().npc == 55);
    v.ClearDesk();
    CHECK(v.Desk().npc == 0);
}

TEST_CASE("arena view: the arena's messages are the world's, decoded and replayed") {
    CHECK(IsWorldMessage(proto::ArenaNotice::kId));
    CHECK(IsWorldMessage(proto::ArenaScores::kId));
    CHECK(IsWorldMessage(proto::ArenaResult::kId));
    CHECK(IsWorldMessage(proto::ArenaDesk::kId));
    CHECK_FALSE(IsWorldMessage(proto::ArenaRegister::kId)); // the client's own requests
    CHECK_FALSE(IsWorldMessage(proto::ArenaDev::kId));
    TitleEvent event;
    proto::ArenaScores s{3, 9, {{1, "Walker", 6}}};
    REQUIRE(DecodeWorldEvent(proto::ArenaScores::kId, proto::Encode(s), event));
    const auto* e = std::get_if<proto::EventEvent>(&event);
    REQUIRE(e != nullptr);
    CHECK(std::get<proto::ArenaScores>(*e).top[0].name == "Walker");
    REQUIRE(DecodeWorldEvent(proto::ArenaResult::kId, proto::Encode(proto::ArenaResult{2, 3, 2, 99, 5, 5}), event));
    CHECK(std::get<proto::ArenaResult>(std::get<proto::EventEvent>(event)).outcome == 3);
}
