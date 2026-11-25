// The chat: the chat window with its tabs, sizes, input line and whisper list,
// and the global chat window.

#include <algorithm>
#include <cmath>
#include <string_view>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/core/Log.h"

namespace sr::client {

namespace {

namespace proto = game::proto;

bool Inside(ui::Rect r, ui::Vec2 p) { return p.x >= r.x && p.y >= r.y && p.x <= r.Right() && p.y <= r.Bottom(); }

// The lines' colours (the original's ARGB; observed).
constexpr ui::Color kWhite = ui::Argb(0xFFFFFFFF), kWhisperCyan = ui::Argb(0xFF9FFFFE), kGmPink = ui::Argb(0xFFFFAEC3),
                    kPartyMint = ui::Argb(0xFF9AFFD0), kGuildOrange = ui::Argb(0xFFFFB541), kGlobalYellow = ui::Argb(0xFFFFFF00),
                    kUnionLime = ui::Argb(0xFFC2F573), kAcademyBlue = ui::Argb(0xFF64C7FF), kWelcomeTan = ui::Argb(0xFFDBC99B), kRefusedRed = ui::Argb(0xFFFF0000),
                    kTabGrey = ui::Argb(0xFFB4B4B4);

// The input line's prefix for each tab.
constexpr const char* kTabPrefix[] = {"", "#", "@", "%"};

std::size_t Characters(std::string_view s) {
    std::size_t n = 0;
    for (char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}

} // namespace

std::string ChatText(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' || text[i] == '\r') continue;
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') {
            ++i;
            continue;
        }
        out += text[i];
    }
    return out;
}

// --- Lines --------------------------------------------------------------------------------------

ChatWindow::Shown ChatWindow::Format(const game::ChatEntry& e, const HudContext& context) {
    // Name, tag and text with nothing between them.
    const auto tag = [&](const char* key) { return "(" + context.Text(key) + "):"; };
    const std::string text = ChatText(e.text);
    switch (e.kind) {
    case proto::ChatKind::All: return {kAll, kWhite, e.name + ":" + text};
    case proto::ChatKind::Whisper:
        return {kWhisper, kWhisperCyan, e.name + tag(e.own ? "UIIT_CHATERR_WHISPER_TO_MESSAGE" : "UIIT_CHATERR_WHISPER_FROM_MESSAGE") + text};
    case proto::ChatKind::Gm:
        // A game master's line starting with ';' is shown as anyone's, without it.
        if (text.starts_with(';')) return {kAll, kWhite, e.name + ":" + text.substr(1)};
        return {kGm, kGmPink, e.name + ":" + text};
    case proto::ChatKind::Party: return {kParty, kPartyMint, e.name + tag("UIIT_CTL_PARTY") + text};
    case proto::ChatKind::Guild: return {kGuild, kGuildOrange, e.name + tag("UIIT_CTL_GUILD") + text};
    case proto::ChatKind::Global: return {kGlobal, kGlobalYellow, e.name + ":" + text};
    case proto::ChatKind::Notice: return {kGm, kGmPink, tag("UIIT_MSG_NOTIFY") + text};
    case proto::ChatKind::Union: return {kUnion, kUnionLime, e.name + tag("UIIT_STT_GUILD_RESPECT_ALLY") + text};
    // The academy's: "(Academy):" in its blue (colour 0xFF64C7FF), in All alone.
    case proto::ChatKind::Academy: return {kAcademy, kAcademyBlue, e.name + tag("UIIT_CTL_TC_TRAININGCAMP") + text};
    default: break;
    }
    return {0, 0, {}}; // the stall's chat is its window's (C7j)
}

void ChatWindow::AddLine(int type, ui::Color colour, const std::string& text, bool allOnly) {
    // Every line to All; then by its type.
    if (boxes_[0] != nullptr) boxes_[0]->AddParagraph(text, colour, ui::kTextShadow);
    if (allOnly) return;
    const auto into = [&](int tab) {
        if (boxes_[std::size_t(tab)] != nullptr) boxes_[std::size_t(tab)]->AddParagraph(text, colour, ui::kTextShadow);
    };
    switch (type) {
    case kParty: into(1); break;
    case kGuild: into(2); break;
    case kUnion: into(3); break;
    case kWhisper:
    case kGm:
    case kSystem:
        into(1);
        into(2);
        into(3);
        break;
    default: break;
    }
}

void ChatWindow::Post(game::GameMessage::Kind kind, const std::string& text) {
    if (context_.state != nullptr) context_.state->Post(kind, text, context_.time);
}

void ChatWindow::TakeEntries() {
    game::PlayerState* state = context_.state;
    if (state == nullptr) return;
    const std::uint64_t fresh = std::min<std::uint64_t>(state->chatPosted - seen_, state->chat.size());
    for (std::size_t i = state->chat.size() - std::size_t(fresh); i < state->chat.size(); ++i) {
        const game::ChatEntry& e = state->chat[i];
        if (e.result != proto::ChatResult::Ok) {
            // The server's refusal, as a system line.
            std::string why;
            switch (e.result) {
            case proto::ChatResult::TargetNotFound: why = FormatText(context_.Text("UIIT_CHATERR_CANT_FIND_TARGET"), {e.name}); break;
            case proto::ChatResult::WhispersOff: why = FormatText(context_.Text("UIIT_CHATERR_YOU_ARE_SQUELCHED"), {e.name}); break;
            case proto::ChatResult::InvalidCommand: why = context_.Text("UIIT_CHATERR_INVALID_COMMAND"); break;
            case proto::ChatResult::NotInParty: why = context_.Text("UIIT_CHATERR_NOT_A_PARTY_MEMBER"); break;
            case proto::ChatResult::NotAlly:
            case proto::ChatResult::NotInUnion: why = context_.Text("UIIT_CHATERR_ALLIANCE_PERMISSION_DENIED"); break;
            // The original passes no number for this one; ours has the seconds.
            case proto::ChatResult::Restricted: why = FormatText(context_.Text("UIIT_STT_CANT_CHATTING"), {std::to_string(e.seconds)}); break;
            case proto::ChatResult::NoUnionRight: why = context_.Text("UIIT_MSG_GUILD_UNION_CHAT_LIMIT"); break;
            case proto::ChatResult::Ok: break;
            }
            if (!why.empty()) Post(game::GameMessage::Kind::General, why);
            continue;
        }
        const Shown shown = Format(e, context_);
        if (shown.text.empty()) continue;
        AddLine(shown.type, shown.colour, shown.text);
        // A notice is also the banner across the top (with the bare text).
        if (e.kind == proto::ChatKind::Notice) state->Notify(game::Banner::Kind::Notice, ChatText(e.text), context_.time);
        if (e.kind == proto::ChatKind::Whisper) NotePartner(e.name);
    }
    seen_ = state->chatPosted;
}

void ChatWindow::NotePartner(const std::string& name) {
    // Every whisper partner once, the newest at the bottom, 30 at most; the latest is the reply
    // key's.
    if (name.empty() || (context_.state != nullptr && name == context_.state->self.name)) return;
    replyTarget_ = name;
    if (std::find(partners_.begin(), partners_.end(), name) != partners_.end()) return;
    partners_.push_back(name);
    while (partners_.size() > kPartners) partners_.pop_front();
    if (whisperBox_ != nullptr) {
        whisperBox_->ClearParagraphs();
        for (const std::string& p : partners_) whisperBox_->AddParagraph(p, kWhite, ui::kTextShadow);
    }
}

// --- The window -----------------------------------------------------------------------------------

void ChatWindow::Created(ui::Interface& ui) {
    ui_ = &ui;
    // The four tabs' text boxes: 14-pixel rows, a hundred lines, the newest at the bottom, wrapped
    // rows indented; a hovered line on a 40 % white bar as wide as its text.
    for (int i = 0; i < 4; ++i) {
        auto* box = ui::As<ui::TextBox>(Child(16 + i));
        boxes_[std::size_t(i)] = box;
        if (box != nullptr) {
            box->lineHeight = 14.0f;
            box->maxParagraphs = 100;
            box->bottomAnchored = true;
            box->hangingIndent = true;
            box->hoverTracking = true;
            box->hoverBar = ui::Argb(0x66FFFFFF);
        }
        lamps_[std::size_t(i)] = Child(20 + i);
    }
    input_ = ui::As<ui::Edit>(Child(11));
    bar_ = ui::As<ui::ScrollBar>(Child(24));
    penalty_ = ui::As<ui::Static>(Child(52));
    if (penalty_ != nullptr) {
        penalty_->textStyle |= ui::kTextShadow;
        penalty_->visible = false;
    }
    // The whisper list: 18-pixel rows, seven shown, thirty kept, the hovered one on an orange bar
    // across the row; hidden at the start.
    whispers_ = Child(51);
    if (whispers_ != nullptr) {
        whispers_->visible = false;
        whisperBox_ = ui::As<ui::TextBox>(whispers_->Child(11));
        if (whisperBox_ != nullptr) {
            whisperBox_->rect.x += 6.0f; // its text 6, 3 in
            whisperBox_->rect.y += 3.0f;
            whisperBox_->rect.w -= 6.0f;
            whisperBox_->lineHeight = 18.0f;
            whisperBox_->visibleRows = 7;
            whisperBox_->maxParagraphs = kPartners;
            whisperBox_->bottomAnchored = false;
            whisperBox_->hoverTracking = true;
            whisperBox_->hoverFullRow = true;
            whisperBox_->hoverBar = ui::Argb(0x66FF8400);
            whisperBox_->bar = ui::As<ui::ScrollBar>(whispers_->Child(12));
        }
    }
    // The tabs are made here: All, Party, Guild, Union at 45 + 51 i, overlapping by a pixel. Made after the layout's
    // controls, they stand behind them (the lamps
    // show in their slots).
    constexpr const char* kCaptions[] = {"UIIT_CTL_CHAT_ALL", "UIIT_CTL_PARTY", "UIIT_CTL_GUILD", "UIIT_STT_GUILD_RESPECT_ALLY"};
    for (int i = 0; i < 4; ++i) {
        assets::UiLayoutNode node;
        node.type = "CIFSelectableArea";
        node.id = 100 + i;
        node.rect = {45.0f + 51.0f * float(i), 0.0f, 52.0f, 20.0f};
        node.ddj = "interface\\chattingwnd\\chat_tab.ddj";
        node.hasClientRect = true;
        node.clientRect = {5.0f, 6.0f, 0.0f, 4.0f};
        node.hAlign = 1; // after the lamp's slot: centred (the screenshots)
        node.vAlign = 1;
        auto tab = ui.Make(node);
        tab->SetText(context_.Text(kCaptions[i]));
        Widget* added = Add(std::move(tab));
        added->SendToBack();
        tabs_[std::size_t(i)] = ui::As<ui::SelectableArea>(added);
    }
    SelectTab(0);
}

void ChatWindow::SetLevel(int level) {
    // Observed: 1.150: the same 16 higher, its bottom on 1.188's underbar (CNIFUnderMenuBar,
    // H - 68; 1.150's stood on H - 52). The window stays at x 2.
    level_ = std::clamp(level, 0, 6);
    const float H = placedFor_.y > 0 ? placedFor_.y : 768.0f;
    rect.x = 2.0f;
    if (level_ == 0) {
        // The input line alone, at the top of a 21-pixel window; the size button beside it.
        rect.y = H - 89.0f;
        rect.h = 21.0f;
        if (input_ != nullptr) input_->rect.y = 0.0f;
        if (Widget* size = Child(15)) size->rect.y = 0.0f;
        if (penalty_ != nullptr) penalty_->rect.y = 2.0f;
        for (int id : {5, 6, 7, 12, 14, 24}) {
            if (Widget* w = Child(id)) w->visible = false;
        }
        for (ui::TextBox* box : boxes_) {
            if (box != nullptr) box->visible = false;
        }
        ShowStrip(false);
        ShowWhispers(false);
        return;
    }
    const float n = float(level_);
    rect.y = H - 467.0f + 56.0f * (6.0f - n);
    rect.h = 56.0f * n + 63.0f;
    for (ui::TextBox* box : boxes_) {
        if (box == nullptr) continue;
        box->rect.h = 56.0f * n + 7.0f;
        box->visibleRows = 4 * level_;
        box->ScrollToEnd();
    }
    if (bar_ != nullptr) {
        bar_->rect.h = bar_->track = 56.0f * n - 28.0f;
        bar_->visible = true;
    }
    float bottom = 25.0f + 56.0f * n + 12.0f;
    if (Widget* top = Child(5)) top->visible = true;
    if (Widget* mid = Child(6)) {
        mid->visible = true;
        mid->rect.h = 56.0f * n + 12.0f;
    }
    if (Widget* low = Child(7)) {
        low->visible = true;
        low->rect.y = bottom;
    }
    if (Widget* size = Child(15)) size->rect.y = bottom + 5.0f;
    if (input_ != nullptr) input_->rect.y = bottom + 5.0f;
    if (penalty_ != nullptr) penalty_->rect.y = bottom + 7.0f;
    for (int id : {12, 14}) {
        if (Widget* w = Child(id)) w->visible = true;
    }
    // Back from level 0 the tabs show again (the original leaves them hidden until the hide
    // button; we do not keep that slip).
    if (stripHidden_ && level_ >= 1) ShowStrip(true);
    SelectTab(tab_);
}

void ChatWindow::ShowStrip(bool shown) {
    // The hide button (12): the four tabs and the lamp; the lines, the input and the background stay.
    stripHidden_ = !shown;
    for (ui::SelectableArea* tab : tabs_) {
        if (tab != nullptr) tab->visible = shown;
    }
    for (std::size_t i = 0; i < lamps_.size(); ++i) {
        if (lamps_[i] != nullptr) lamps_[i]->visible = shown && int(i) == tab_;
    }
}

void ChatWindow::ShowWhispers(bool shown) {
    whispersOpen_ = shown && level_ > 0;
    if (whispers_ != nullptr) whispers_->visible = whispersOpen_;
}

void ChatWindow::SelectTab(int tab) {
    // Observed:. Only the tab's lines and lamp show; the scroll bar follows its lines.
    tab_ = std::clamp(tab, 0, 3);
    for (std::size_t i = 0; i < 4; ++i) {
        const bool mine = int(i) == tab_;
        if (lamps_[i] != nullptr) lamps_[i]->visible = mine && !stripHidden_;
        if (boxes_[i] != nullptr) {
            boxes_[i]->visible = mine && level_ > 0;
            boxes_[i]->bar = mine ? bar_ : nullptr;
            if (mine) boxes_[i]->ScrollToEnd();
        }
        if (tabs_[i] != nullptr) {
            // The selected caption bold in white, the others grey.
            tabs_[i]->selected = mine;
            tabs_[i]->textColor = mine ? kWhite : kTabGrey;
        }
    }
    // An empty input line (or one holding only a prefix) takes the tab's prefix.
    if (input_ != nullptr) {
        const std::string& now = input_->text();
        if (now.empty() || (now.size() == 1 && std::string_view("#$%/@^~").find(now[0]) != std::string_view::npos)) {
            input_->SetValue(kTabPrefix[tab_]);
        }
    }
}

void ChatWindow::FocusInput() {
    if (ui_ == nullptr || input_ == nullptr || !visible) return;
    input_->SetValue(input_->text()); // the caret at its end
    ui_->Focus(input_);
}

void ChatWindow::Release() {
    if (ui_ != nullptr && ui_->focus() == input_) ui_->Focus(nullptr);
}

void ChatWindow::Address(const std::string& name, bool focus) {
    // Never one's own name.
    if (input_ == nullptr || name.empty() || (context_.state != nullptr && name == context_.state->self.name)) return;
    input_->SetValue("$" + name + " ");
    if (focus) FocusInput();
}

void ChatWindow::Reply() {
    if (!replyTarget_.empty()) Address(replyTarget_, false); // the text only; Enter then types
}

void ChatWindow::Restrict(int seconds) {
    restricted_ = true;
    restrictLeft_ = seconds;
    restrictTick_ = context_.time + 1.0;
    if (penalty_ != nullptr) {
        penalty_->SetText(FormatText(context_.Text("UIIT_STT_CANT_CHATTING"), {std::to_string(restrictLeft_)}));
        penalty_->visible = true;
    }
}

bool ChatWindow::Command(const std::string& line) {
    // "/word ...": never sent as chat; the first word against the localized commands. What they do comes with their
    // features (trades, parties, stalls, pets).
    if (CommunityChatCommand(context_, line)) return true;
    const std::size_t space = line.find(' ');
    const std::string word = line.substr(0, space);
    constexpr const char* kCommands[] = {"UIIT_STT_CHAT_COMMAND_TRADE",        "UIIT_STT_CHAT_COMMAND_TRADE2",
                                         "UIIT_STT_CHAT_COMMAND_PARTY_INVITE", "UIIT_STT_CHAT_COMMAND_PARTY_INVITE2",
                                         "UIIT_STT_CHAT_COMMAND_PARTY_EXPEL",  "UIIT_STT_CHAT_COMMAND_PARTY_EXIT",
                                         "UIIT_STT_CHAT_COMMAND_STREETSTORE",  "UIIT_STT_CHAT_COMMAND_SITDOWN",
                                         "UIIT_STT_CHAT_COMMAND_REPLY1",       "UIIT_STT_CHAT_COMMAND_REPLY2",
                                         "UIIT_STT_CHAT_COMMAND_REPLY3",       "UIIT_STT_CHAT_COMMAND_REPLY4",
                                         "UIIT_STT_CHAT_COMMAND_WHISPER1",     "UIIT_STT_CHAT_COMMAND_WHISPER2",
                                         "UIIT_STT_CHAT_COMMAND_WHISPER3",     "UIIT_STT_CHAT_COMMAND_WHISPER4",
                                         "UIIT_STT_CHAT_COMMAND_WHISPER5",     "UIIT_STT_CHAT_COMMAND_SECRET_TALK",
                                         "UIIT_STT_CHAT_COMMAND_GUILD_INVITE", "UIIT_STT_CHAT_COMMAND_GUILD_INVITE1",
                                         "UIIT_STT_CHAT_COMMAND_GUILD_INVITE2", "UIIT_STT_CHAT_COMMAND_PVP_PROPOSAL",
                                         "UIIT_STT_CHAT_COMMAND_FINDUSER",     "UIIT_KEY_COS_ATTACK",
                                         "UIIT_KEY_COS_FOLLOW",                "UIIT_KEY_COS_RIDING",
                                         "UIIT_KEY_COS_DISMOUNT",              "UIIT_KEY_COS_RETURN",
                                         "UIIT_KEY_COS_LEAVEOUT",              "UIIT_KEY_COS_AGGRESSIVE",
                                         "UIIT_KEY_COS_DEFENSIVE",             "UIIT_KEY_COS_CHARM",
                                         "UIIT_STT_PREMIUM_COMMAND_RETURN_HIGH_SPEED", "UIIT_STT_PREMIUM_COMMAND_REVERSE_RETURN",
                                         "UIIT_STT_PREMIUM_COMMAND_RESURRECTION_100%"};
    for (const char* key : kCommands) {
        const bool premium = std::string_view(key).starts_with("UIIT_STT_PREMIUM_COMMAND");
        if (context_.hasText && context_.hasText(key) && context_.Text(key) == (premium ? line : word)) {
            if (context_.hud != nullptr) context_.hud->Request({HudRequest::Kind::ChatCommand, 0, line, key});
            return true;
        }
    }
    // Ours: any other goes to the server, a game master's command (/give, /notice, ...: GameServer's GmCommand; its answer
    // comes back as a GM line). The server says nothing of it to anyone else.
    return false;
}

void ChatWindow::Submit(const std::string& line) {
    if (input_ == nullptr) return;
    // While restricted every Enter clears the line and says so.
    if (restricted_) {
        input_->SetValue("");
        Post(game::GameMessage::Kind::General, context_.Text("UIIT_MSG_CANT_CHATTING"));
        return;
    }
    if (line.empty()) return Release();
    // The first character picks the kind.
    proto::ChatKind kind = proto::ChatKind::All;
    std::string body, target;
    const char first = line[0];
    const bool inParty = context_.state != nullptr && context_.state->self.party != 0;
    const bool inGuild = context_.state != nullptr && context_.state->self.guild != 0;
    switch (first) {
    case '#':
        if (!inParty) {
            Post(game::GameMessage::Kind::Party, context_.Text("UIIT_MSG_PARTYERR_CANT_FIND_PARTY"));
            input_->SetValue("");
            return;
        }
        kind = proto::ChatKind::Party;
        body = line.substr(1);
        break;
    case '@':
        if (!inGuild) {
            Post(game::GameMessage::Kind::Party, context_.Text("UIIT_MSG_GUILD_NON_EXISTING"));
            input_->SetValue("");
            return;
        }
        kind = proto::ChatKind::Guild;
        body = line.substr(1);
        break;
    case '%':
        kind = proto::ChatKind::Union;
        body = line.substr(1);
        break;
    case '&':
        // The academy (type 0x10): outside one "You are not registered" and the line cleared.
        if (context_.state == nullptr || !context_.state->academy.InAcademy()) {
            Post(game::GameMessage::Kind::General, context_.Text("UIIT_MSG_TC_ERROR_NOT_POSITION"));
            input_->SetValue("");
            return;
        }
        kind = proto::ChatKind::Academy;
        body = line.substr(1);
        break;
    case '$': {
        // "$name text": nothing goes without both.
        const std::size_t space = line.find(' ', 1);
        target = line.substr(1, space == std::string::npos ? std::string::npos : space - 1);
        body = space == std::string::npos ? std::string() : line.substr(space + 1);
        if (target.empty() || body.empty()) return Release();
        kind = proto::ChatKind::Whisper;
        break;
    }
    case '/':
        if (Command(line)) {
            input_->SetValue("");
            return;
        }
        body = line; // a game master's command, sent as a line of the chat with those around
        break;
    default: {
        // The flood rule, for chat with the people around: a fourth line within 4 s or a sixth
        // within 10 s is not sent, and the chat is restricted for 10 s.
        sends_.push_back(context_.time);
        while (sends_.size() > 7) sends_.pop_front();
        const std::size_t n = sends_.size();
        if ((n >= 4 && context_.time - sends_[n - 4] < 4.0) || (n >= 6 && context_.time - sends_[n - 6] < 10.0)) {
            sends_.clear();
            input_->SetValue("");
            Restrict(10);
            return;
        }
        body = line;
        break;
    }
    }
    if (body.empty()) return Release();
    // The client's own word filter (abusefilter.txt's chat words): the line is refused here.
    if (context_.words != nullptr && !context_.words->ChatAllowed(body)) {
        AddLine(kAll, kRefusedRed, context_.Text("UIIT_MSG_CHATWND_MESSAGE_FILTER"), true);
        input_->SetValue("");
        return;
    }
    if (context_.hud != nullptr) context_.hud->Request({HudRequest::Kind::Chat, int(kind), body, target});
    // The input line keeps the keyboard and is ready for the same kind again; a tab's prefix wins.
    std::string next;
    switch (kind) {
    case proto::ChatKind::Whisper: next = "$" + target + " "; break;
    case proto::ChatKind::Party: next = "#"; break;
    case proto::ChatKind::Guild: next = "@"; break;
    case proto::ChatKind::Union: next = "%"; break;
    case proto::ChatKind::Academy: next = "&"; break;
    default: break;
    }
    if (tab_ > 0) next = kTabPrefix[tab_];
    input_->SetValue(next);
}

void ChatWindow::Update(ui::Frame& frame) {
    const ui::Vec2 screen = frame.interface.screen();
    if (screen.x != placedFor_.x || screen.y != placedFor_.y) {
        placedFor_ = screen;
        SetLevel(level_);
    }
    // The welcome text in every tab when the game starts.
    if (!welcomed_) {
        welcomed_ = true;
        if (context_.hasText && context_.hasText("UIIT_STT_STARTING_MSG")) AddLine(kSystem, kWelcomeTan, context_.Text("UIIT_STT_STARTING_MSG"));
    }
    TakeEntries();
    // The restriction's countdown, a second at a time; on the tick after 0 the chat is free again
    // and the input line empty.
    if (restricted_ && context_.time >= restrictTick_) {
        restrictTick_ += 1.0;
        if (--restrictLeft_ < 0) {
            restricted_ = false;
            if (penalty_ != nullptr) penalty_->visible = false;
            if (input_ != nullptr) input_->SetValue("");
            Release();
        } else if (penalty_ != nullptr) {
            penalty_->SetText(FormatText(context_.Text("UIIT_STT_CANT_CHATTING"), {std::to_string(restrictLeft_)}));
        }
    }
    // The wheel: a row a notch, over the whisper list its names, anywhere else over the window
    // the tab's lines.
    const ui::Vec2 mouse = frame.pointer.position;
    if (frame.input.wheel() != 0.0f) {
        const int step = frame.input.wheel() > 0.0f ? -1 : 1;
        if (whispersOpen_ && whisperBox_ != nullptr && whispers_ != nullptr && Inside(whispers_->ScreenRect(), mouse)) {
            whisperBox_->ScrollBy(step);
        } else if (level_ > 0 && Inside(ScreenRect(), mouse) && boxes_[std::size_t(tab_)] != nullptr) {
            boxes_[std::size_t(tab_)]->ScrollBy(step);
        }
    }
    // The buttons' help texts.
    if (context_.text) {
        constexpr std::pair<int, const char*> kHelp[] = {
            {12, "UIIT_STT_CHATING_VIEW_OFF"}, {14, "UIIT_CTL_WHISPER_LIST"}, {15, "UIIT_STT_TOGGLE_CHATING_WINDOW_SETING"}};
        for (const auto& [id, key] : kHelp) {
            if (Widget* b = Child(id); b != nullptr && frame.pointer.hovered == b) frame.interface.ShowTooltip(context_.Text(key), b->ScreenRect());
        }
    }
}

bool ChatWindow::OnNotice(const ui::Notice& notice) {
    if (notice.source == nullptr) return false;
    using K = ui::Notice::Kind;
    if (notice.source == input_) {
        if (notice.kind == K::Submitted) {
            Submit(input_->text());
            return true;
        }
        if (notice.kind == K::Cancelled) {
            if (restricted_) {
                input_->SetValue("");
                Post(game::GameMessage::Kind::General, context_.Text("UIIT_MSG_CANT_CHATTING"));
            }
            Release(); // the text stays
            return true;
        }
        return notice.kind == K::TextChanged;
    }
    if (notice.kind == K::Clicked && notice.source->parent() == this) {
        switch (notice.control) {
        case 12: ShowStrip(stripHidden_); return true;
        case 14: ShowWhispers(!whispersOpen_); return true;
        case 15: SetLevel(level_ == 6 ? 0 : level_ + 1); return true;
        case 100:
        case 101:
        case 102:
        case 103: SelectTab(notice.control - 100); return true;
        default: break;
        }
    }
    if (notice.kind == K::RowPicked) {
        if (notice.source == whisperBox_ && notice.value >= 0 && std::size_t(notice.value) < partners_.size()) {
            Address(partners_[std::size_t(notice.value)], true); // the list stays open
            return true;
        }
        const auto* box = dynamic_cast<const ui::TextBox*>(notice.source);
        if (box != nullptr && box->parent() == this && notice.value >= 0 && std::size_t(notice.value) < box->paragraphs().size()) {
            // A line clicked: the name before its first ':', without a whisper's, party's, guild's
            // or union's tag; 2 to 12 characters.
            const std::string& text = box->paragraphs()[std::size_t(notice.value)].text;
            const std::size_t colon = text.find(':');
            if (colon == std::string::npos) return true;
            std::string name = text.substr(0, colon);
            if (const std::size_t open = name.find('('); open != std::string::npos) {
                const std::string tag = name.substr(open);
                bool known = false;
                for (const char* key : {"UIIT_CHATERR_WHISPER_FROM_MESSAGE", "UIIT_CHATERR_WHISPER_TO_MESSAGE", "UIIT_CTL_GUILD", "UIIT_CTL_PARTY",
                                        "UIIT_STT_GUILD_RESPECT_ALLY"}) {
                    known |= tag == "(" + context_.Text(key) + ")";
                }
                if (!known) return true;
                name.resize(open);
            }
            if (Characters(name) >= 2 && Characters(name) <= 12) Address(name, true);
            return true;
        }
    }
    return false;
}

// --- Global chat ----------------------------------------------------------------------------------

void GlobalChatWindow::Created(ui::Interface& ui) {
    ui_ = &ui;
    // The title and the strip to drag it by are made in code.
    assets::UiLayoutNode title;
    title.type = "CIFStatic";
    title.id = 0xC9;
    title.rect = {10.0f, 12.0f, rect.w - 21.0f, 12.0f};
    title.hAlign = 1;
    auto t = ui.Make(title);
    t->SetText(context_.Text("UIIT_STT_WHOLECHAT"));
    Add(std::move(t));
    assets::UiLayoutNode drag;
    drag.type = "CIFDragableArea";
    drag.id = 0xCA;
    drag.rect = {20.0f, 12.0f, rect.w - 21.0f, 34.0f};
    Add(ui.Make(drag));
    if (auto* edit = ui::As<ui::Edit>(Child(36))) {
        edit->passKeys = true; // Enter: Use; Escape: Exit
        edit->limit = proto::kChatCharacters;
    }
}

void GlobalChatWindow::Recount() {
    const game::ItemInstance* item = context_.state != nullptr ? context_.state->inventory.At(place_) : nullptr;
    count = item != nullptr && item->refId == refId_ ? int(item->count) : 0;
}

void GlobalChatWindow::Update(ui::Frame& frame) {
    // The count left, and Use only while there is one.
    Recount();
    if (Widget* left = Child(35)) {
        left->SetText(std::to_string(count));
        // "Remaining turns" is wider than the 55 pixels the layout gives it: the number goes after it.
        if (const Widget* label = Child(34)) left->rect.x = std::max(left->node().rect.x, label->rect.x + frame.paint.TextWidth(label->text()) + 6.0f);
    }
    if (Widget* use = Child(31)) use->enabled = count > 0;
}

void GlobalChatWindow::Use() {
    auto* edit = ui::As<ui::Edit>(Child(36));
    if (edit == nullptr) return;
    // Observed:. The window stays open; the line is cleared whatever happens.
    const std::string text = edit->text();
    edit->SetValue("");
    const auto post = [&](const char* key) {
        if (context_.state != nullptr) context_.state->Post(game::GameMessage::Kind::General, context_.Text(key), context_.time);
    };
    Recount();
    if (count < 1) return;
    if (text.empty()) return post("UIIT_STT_WHOLECHAT_INPUTMSG");
    if (context_.words != nullptr && !context_.words->ChatAllowed(text)) return post("UIIT_MSG_WHOLECHATERR_NOTINUSEMSG");
    if (context_.hud != nullptr) {
        HudRequest request{HudRequest::Kind::Chat, int(proto::ChatKind::Global), text};
        request.extra = place_;
        context_.hud->Request(request);
    }
}

void GlobalChatWindow::Say(const std::string& text) {
    if (auto* edit = ui::As<ui::Edit>(Child(36))) edit->SetValue(text);
    Use();
}

bool GlobalChatWindow::OnNotice(const ui::Notice& notice) {
    if (notice.source == nullptr) return false;
    const bool edit = notice.source->id() == 36 && notice.source->parent() == this;
    const bool close = (notice.kind == ui::Notice::Kind::Clicked && notice.control == 32 && notice.source->parent() == this) ||
                       (edit && notice.kind == ui::Notice::Kind::Cancelled);
    if (close) {
        if (context_.hud != nullptr) context_.hud->Hide(56);
        return true;
    }
    if ((notice.kind == ui::Notice::Kind::Clicked && notice.control == 31 && notice.source->parent() == this) ||
        (edit && notice.kind == ui::Notice::Kind::Submitted)) {
        Use();
        return true;
    }
    return edit;
}

} // namespace sr::client
