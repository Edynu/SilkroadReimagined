// The chat (C7d): the player's chat lines (game::PlayerState), the client's
// word filter (game::NameRules), and the chat window built from the game's layouts: typing and
// the prefixes, the tabs, the sizes, the lines received, the flood rule, the whisper list.

#include <doctest/doctest.h>

#include <cstdlib>
#include <map>
#include <string>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/UiLayout.h"
#include "sr/core/Path.h"
#include "sr/core/Text.h"
#include "sr/game/CharacterRules.h"
#include "sr/game/PlayerState.h"
#include "sr/ui/Context.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

using namespace sr;
namespace proto = sr::game::proto;
namespace hud = sr::client::hud;
using sr::client::ChatWindow;
using sr::client::HudRequest;

namespace {

game::PlayerState Entered(std::uint32_t entity, const std::string& name) {
    game::PlayerState state;
    proto::WorldEntered entered;
    entered.entity = entity;
    entered.character.name = name;
    state.Apply(game::TitleEvent{entered});
    return state;
}

proto::ChatLine Line(proto::ChatKind kind, std::uint32_t sender, std::string name, std::string text) {
    proto::ChatLine line;
    line.kind = kind;
    line.sender = sender;
    line.name = std::move(name);
    line.text = std::move(text);
    return line;
}

class FakeResources final : public ui::Resources {
public:
    Texture GetTexture(const std::string&) override { return {ui::TextureRef{1}, 16.0f, 16.0f, true, false}; }
    const std::string& Text(const std::string& key) const override { return key; }
    ui::Font* GetFont() const override { return nullptr; }
};

} // namespace

TEST_CASE("the player's own chat lines come with the server's answer; refusals and others' lines as they come") {
    game::PlayerState state = Entered(5, "Hero");
    // Sent lines are numbered 0, 1, ...; an answer prints the kept line as the player's own.
    const std::uint8_t hello = state.QueueChat(proto::ChatKind::All, {}, "hello");
    const std::uint8_t psst = state.QueueChat(proto::ChatKind::Whisper, "Bob", "psst");
    const std::uint8_t lost = state.QueueChat(proto::ChatKind::Whisper, "Nobody", "anyone?");
    CHECK(hello == 0);
    CHECK(psst == 1);
    state.Apply(game::TitleEvent{proto::ChatAnswer{proto::ChatResult::Ok, proto::ChatKind::All, hello, 0}});
    REQUIRE(state.chat.size() == 1);
    CHECK(state.chat[0].own);
    CHECK(state.chat[0].name == "Hero");
    CHECK(state.chat[0].sender == 5);
    CHECK(state.chat[0].text == "hello");
    // An own whisper names the one it went to; a refusal names the target too.
    state.Apply(game::TitleEvent{proto::ChatAnswer{proto::ChatResult::Ok, proto::ChatKind::Whisper, psst, 0}});
    state.Apply(game::TitleEvent{proto::ChatAnswer{proto::ChatResult::TargetNotFound, proto::ChatKind::Whisper, lost, 0}});
    REQUIRE(state.chat.size() == 3);
    CHECK(state.chat[1].name == "Bob");
    CHECK(state.chat[1].own);
    CHECK(state.chat[2].result == proto::ChatResult::TargetNotFound);
    CHECK(state.chat[2].name == "Nobody");
    // An answer for nothing waiting (twice, or never sent) adds nothing.
    state.Apply(game::TitleEvent{proto::ChatAnswer{proto::ChatResult::Ok, proto::ChatKind::All, hello, 0}});
    state.Apply(game::TitleEvent{proto::ChatAnswer{proto::ChatResult::Ok, proto::ChatKind::All, 200, 0}});
    CHECK(state.chat.size() == 3);
    // Others' lines; the player's own echoes are not shown twice, except a global one.
    state.Apply(game::TitleEvent{Line(proto::ChatKind::All, 9, "Ann", "hi all")});
    state.Apply(game::TitleEvent{Line(proto::ChatKind::All, 5, "Hero", "echo")});
    state.Apply(game::TitleEvent{Line(proto::ChatKind::Whisper, 0, "Hero", "to myself")});
    state.Apply(game::TitleEvent{Line(proto::ChatKind::Global, 0, "Hero", "to everyone")});
    REQUIRE(state.chat.size() == 5);
    CHECK(state.chat[3].name == "Ann");
    CHECK_FALSE(state.chat[3].own);
    CHECK(state.chat[4].kind == proto::ChatKind::Global);
    CHECK(state.chat[4].own);
    CHECK(state.chatPosted == 5);
    // The counter runs 0..250 and starts again.
    for (int i = 0; i < 248; ++i) state.QueueChat(proto::ChatKind::All, {}, "x");
    CHECK(state.QueueChat(proto::ChatKind::All, {}, "x") == 251 % 251);
    CHECK(state.QueueChat(proto::ChatKind::All, {}, "x") == 1);
}

TEST_CASE("the word filter: chat words whole or anywhere, names whole or anywhere, A-Z lowered only") {
    game::NameRules rules;
    rules.Allow('0', '9');
    rules.Allow('A', 'Z');
    rules.Allow('a', 'z');
    rules.ForbidInChat("darn", false);    // a whole word
    rules.ForbidInChat("s t o p", true); // anywhere, spaces and all
    rules.Forbid("Blacksmith", false);   // a whole name
    rules.Forbid("gm", true);            // anywhere in a name
    CHECK(rules.ChatAllowed("hello there"));
    CHECK_FALSE(rules.ChatAllowed("oh DARN it"));
    CHECK(rules.ChatAllowed("darned")); // not the whole word
    CHECK(rules.ChatAllowed("darn!") ); // punctuation stays in the word
    CHECK_FALSE(rules.ChatAllowed("please s t o p now"));
    CHECK_FALSE(rules.ChatAllowed("   darn   "));
    CHECK(rules.Check("Blacksmith") == game::NameCheck::Forbidden);
    CHECK(rules.Check("Blacksmiths") == game::NameCheck::Ok);
    CHECK(rules.Check("xGMx") == game::NameCheck::Forbidden);
    CHECK(rules.ChatWords() == 2);
    CHECK(rules.ForbiddenWords() == 2);
}

TEST_CASE("the chat window from the game's layouts: typing, tabs, sizes, lines, the flood rule (game files)") {
    const char* dirVar = std::getenv("SR_GAME_DIR");
    if (dirVar == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipping");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dirVar, error), error);
    assets::UiDefines defines;
    if (const auto file = fs.Read("config\\define.txt")) defines = assets::DecodeUiDefines(core::DecodeTextFile(file->Bytes()));
    FakeResources resources;
    std::map<std::string, assets::UiLayout> layouts;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = core::NormalizeAssetPath(path);
        if (!key.starts_with("resinfo\\") || !key.ends_with(".txt")) continue;
        assets::UiLayout layout;
        if (const auto file = fs.Read(key); file && assets::DecodeUiLayout(core::DecodeTextFile(file->Bytes()), defines, layout, error)) {
            layouts[key] = std::move(layout);
        }
    }
    ui::Interface interface{resources, [&](const std::string& path) {
                                const auto it = layouts.find(path);
                                return it == layouts.end() ? nullptr : &it->second;
                            }};
    ui::RegisterControls(interface);
    game::PlayerState state = Entered(1, "Hero");
    game::NameRules words;
    words.ForbidInChat("darn", false);
    sr::client::Hud hud;
    sr::client::HudContext context;
    context.state = &state;
    context.hud = &hud;
    context.words = &words;
    const std::map<std::string, std::string> english = {
        {"UIIT_CHATERR_WHISPER_FROM_MESSAGE", "FROM"}, {"UIIT_CHATERR_WHISPER_TO_MESSAGE", "TO"}, {"UIIT_CTL_PARTY", "Party"},
        {"UIIT_CTL_GUILD", "Guild"}, {"UIIT_STT_GUILD_RESPECT_ALLY", "Union"}, {"UIIT_STT_CANT_CHATTING", "Chat Restricted: %d seconds"},
        {"UIIT_CHATERR_CANT_FIND_TARGET", "Cannot find [%s]."}, {"UIIT_STT_STARTING_MSG", "Welcome\\n to the Silkroad"}};
    context.text = [&](const std::string& key) {
        const auto it = english.find(key);
        return it != english.end() ? it->second : key;
    };
    context.hasText = [&](const std::string& key) { return english.contains(key); };
    sr::client::RegisterHudWindows(interface, context);
    hud.Bind(&state);
    constexpr ui::Vec2 kScreen{1600, 900};
    hud.Create(interface, kScreen);
    ui::Context ui;
    ui::Input input;
    double time = 0.0;
    const auto frame = [&](std::initializer_list<ui::InputEvent> events) {
        time += 1.0 / 60.0;
        context.time = time;
        input.BeginFrame(time);
        for (const ui::InputEvent& e : events) input.Feed(e);
        ui.NewFrame(input, kScreen, false, false);
        interface.Frame(ui, 1.0f, time, 1.0f / 60.0f);
        hud.Keys(ui, time);
        hud.Sync();
        ui.EndFrame();
    };
    const auto key = [&](ui::Key k) {
        ui::InputEvent down;
        down.type = ui::InputEvent::Type::KeyDown;
        down.key = k;
        frame({down});
        ui::InputEvent up = down;
        up.type = ui::InputEvent::Type::KeyUp;
        frame({up});
    };
    const auto type = [&](const std::string& text) {
        ui::InputEvent t;
        t.type = ui::InputEvent::Type::Text;
        t.text = text;
        ui::InputEvent enter;
        enter.type = ui::InputEvent::Type::KeyDown;
        enter.key = ui::Key::Enter;
        frame({t, enter});
        enter.type = ui::InputEvent::Type::KeyUp;
        frame({enter});
    };
    const auto requests = [&] {
        std::vector<HudRequest> chat;
        for (const HudRequest& r : hud.TakeRequests()) {
            if (r.kind == HudRequest::Kind::Chat) chat.push_back(r);
        }
        return chat;
    };
    frame({});
    frame({});
    auto* chat = dynamic_cast<ChatWindow*>(interface.Window(hud::kChat));
    REQUIRE(chat != nullptr);

    // At the start: level 2, on 1.188's underbar (68 high) at the left edge, eight rows; All selected;
    // the welcome text in every tab.
    CHECK(chat->level() == 2);
    CHECK(chat->rect.x == doctest::Approx(2));
    CHECK(chat->rect.y == doctest::Approx(900 - 467 + 56 * 4));
    CHECK(chat->rect.h == doctest::Approx(175));
    CHECK(chat->box(0)->visibleRows == 8);
    CHECK(chat->box(0)->visible);
    CHECK_FALSE(chat->box(1)->visible);
    CHECK(chat->Child(20)->visible);
    CHECK_FALSE(chat->Child(21)->visible);
    for (int tab = 0; tab < 4; ++tab) {
        REQUIRE(chat->box(tab)->paragraphs().size() == 1);
        CHECK(chat->box(tab)->paragraphs()[0].text == "Welcome\\n to the Silkroad");
    }

    // Enter gives the input line the keyboard; a line typed and sent goes to the people around.
    key(ui::Key::Enter);
    CHECK(interface.focus() == chat->input());
    type("hello");
    auto sent = requests();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].value == int(proto::ChatKind::All));
    CHECK(sent[0].text == "hello");
    CHECK(chat->input()->text().empty());
    CHECK(interface.focus() == chat->input()); // it keeps the keyboard
    // "$name text": a whisper; the input line keeps "$name " for the next.
    type("$Bob hi there");
    sent = requests();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].value == int(proto::ChatKind::Whisper));
    CHECK(sent[0].target == "Bob");
    CHECK(sent[0].text == "hi there");
    CHECK(chat->input()->text() == "$Bob ");
    // "#": the party's, refused without a party (a system line); "%": the union's.
    chat->input()->SetValue("");
    type("#go");
    CHECK(requests().empty());
    CHECK(state.messages.back().text == "UIIT_MSG_PARTYERR_CANT_FIND_PARTY");
    type("%allies");
    sent = requests();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].value == int(proto::ChatKind::Union));
    CHECK(sent[0].text == "allies");
    CHECK(chat->input()->text() == "%");
    // A filtered word: nothing sent, the refusal in red in the All tab.
    chat->input()->SetValue("");
    type("oh darn");
    CHECK(requests().empty());
    CHECK(chat->box(0)->paragraphs().back().text == "UIIT_MSG_CHATWND_MESSAGE_FILTER");
    CHECK(chat->box(0)->paragraphs().back().color == ui::Argb(0xFFFF0000));
    // Escape gives the keyboard back without the system menu.
    key(ui::Key::Escape);
    CHECK(interface.focus() == nullptr);
    CHECK_FALSE(hud.Shown(hud::kSystemMenu));

    // A tab puts its prefix in an empty input line; a typed one stays.
    chat->input()->SetValue("");
    chat->SelectTab(1);
    CHECK(chat->input()->text() == "#");
    CHECK(chat->box(1)->visible);
    CHECK(chat->Child(21)->visible);
    chat->input()->SetValue("abc");
    chat->SelectTab(0);
    CHECK(chat->input()->text() == "abc");

    // Lines received: a whisper in every tab with its tag, the sender a partner and the reply's;
    // a party line in All and Party; a global line in All only, in yellow.
    state.Apply(game::TitleEvent{Line(proto::ChatKind::Whisper, 0, "Bob", "psst")});
    state.Apply(game::TitleEvent{Line(proto::ChatKind::Party, 0, "Ann", "go\\nnow")});
    state.Apply(game::TitleEvent{Line(proto::ChatKind::Global, 0, "Cid", "sale!")});
    frame({});
    const auto last = [&](int tab) { return chat->box(tab)->paragraphs().back(); };
    CHECK(chat->box(0)->paragraphs().size() >= 4);
    CHECK(chat->box(0)->paragraphs()[chat->box(0)->paragraphs().size() - 3].text == "Bob(FROM):psst");
    CHECK(last(1).text == "Ann(Party):gonow");
    CHECK(last(1).color == ui::Argb(0xFF9AFFD0));
    CHECK(last(2).text == "Bob(FROM):psst");
    CHECK(last(0).text == "Cid:sale!");
    CHECK(last(0).color == ui::Argb(0xFFFFFF00));
    REQUIRE(chat->partners().size() == 1);
    CHECK(chat->partners()[0] == "Bob");
    chat->input()->SetValue("");
    chat->Reply();
    CHECK(chat->input()->text() == "$Bob ");
    // A refusal is a system line with the target's name.
    const std::uint8_t lost = state.QueueChat(proto::ChatKind::Whisper, "Nobody", "x");
    state.Apply(game::TitleEvent{proto::ChatAnswer{proto::ChatResult::TargetNotFound, proto::ChatKind::Whisper, lost, 0}});
    frame({});
    CHECK(state.messages.back().text == "Cannot find [Nobody].");
    // A line clicked: its name, without the tag, into the input line.
    chat->input()->SetValue("");
    const auto& all = chat->box(0)->paragraphs();
    chat->OnNotice({ui::Notice::Kind::RowPicked, 16, chat->box(0), int(all.size()) - 3});
    CHECK(chat->input()->text() == "$Bob ");

    // The whisper list's button (a click with the mouse) opens it above the chat; a name in it
    // addresses a whisper.
    const auto click = [&](ui::Vec2 at) {
        ui::InputEvent move;
        move.type = ui::InputEvent::Type::MouseMove;
        move.position = at;
        frame({move});
        ui::InputEvent down = move;
        down.type = ui::InputEvent::Type::MouseDown;
        frame({down});
        ui::InputEvent up = move;
        up.type = ui::InputEvent::Type::MouseUp;
        frame({up});
    };
    const ui::Rect button = chat->Child(14)->ScreenRect();
    click({button.x + 8, button.y + 10});
    REQUIRE(chat->Child(51) != nullptr);
    CHECK(chat->Child(51)->visible);
    CHECK(chat->Child(51)->ScreenRect().y == doctest::Approx(chat->rect.y - 152));

    // The sizes: 4 rows a level, the bottom kept; after 6 the input line alone, then 1.
    chat->SetLevel(6);
    CHECK(chat->rect.y == doctest::Approx(900 - 467));
    CHECK(chat->rect.h == doctest::Approx(399));
    CHECK(chat->box(0)->visibleRows == 24);
    chat->OnNotice({ui::Notice::Kind::Clicked, 15, chat->Child(15), 0});
    CHECK(chat->level() == 0);
    CHECK(chat->rect.y == doctest::Approx(900 - 89));
    CHECK(chat->rect.h == doctest::Approx(21));
    CHECK_FALSE(chat->box(0)->visible);
    CHECK_FALSE(chat->stripShown());
    chat->OnNotice({ui::Notice::Kind::Clicked, 15, chat->Child(15), 0});
    CHECK(chat->level() == 1);
    CHECK(chat->rect.h == doctest::Approx(119));
    CHECK(chat->box(0)->visible);

    // The flood rule: a fourth line within 4 s is not sent and restricts the chat for 10 s (the
    // lines above are 10 s gone).
    time += 11.0;
    chat->input()->SetValue("");
    interface.Focus(nullptr); // (Enter on an empty focused line would let the keyboard go)
    key(ui::Key::Enter);
    for (int i = 0; i < 3; ++i) type("spam");
    CHECK(requests().size() == 3);
    type("spam");
    CHECK(requests().empty());
    CHECK(chat->restricted());
    REQUIRE(chat->Child(52) != nullptr);
    CHECK(chat->Child(52)->visible);
    CHECK(chat->Child(52)->text() == "Chat Restricted: 10 seconds");
    for (int i = 0; i < 11 * 60 + 10; ++i) frame({});
    CHECK_FALSE(chat->restricted());
    CHECK_FALSE(chat->Child(52)->visible);
}
