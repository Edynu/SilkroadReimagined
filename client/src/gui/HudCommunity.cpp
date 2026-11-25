// The community pages of the guild window: the 1.188 client's
// CNIFCommunityWnd, shown in the guild window's place by its Community button, with
// the Friend, Block and Msg Box tabs built from res_ui\cumunity_friend.2dt, cumunity_cut.2dt and
// cumunity_note.2dt. A view over game::CommunityView; what it asks goes to the server as community
// requests. The list of names blocked for all chat is the PC's, kept in the original's file format.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/assets/UiLayout2dt.h"
#include "sr/core/Log.h"
#include "sr/game/Community.h"
#include "sr/game/CommunityProtocol.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr int kPanelId = 30200;
constexpr const char* kSelectedBar = "interface\\ifcommon\\com_bar01select_"; // as the guild's lists
constexpr const char* kPcBlockFile = "SRChattingBlockingList.dat";             // u32 version 1, u32 count, then u8 size + name each

std::string Swap(const std::string& art, std::string_view from, std::string_view to) {
    std::string out = art;
    if (const std::size_t at = out.rfind(from); at != std::string::npos) out.replace(at, from.size(), to);
    return out;
}

std::string RaceMark(std::uint32_t model) {
    return model >= 14875 ? "interface\\ifcommon\\com_kindred_europe16.ddj" : "interface\\ifcommon\\com_kindred_china16.ddj";
}

bool BadName(const std::string& name) { return name.empty() || name.find('[') != std::string::npos || name.find(']') != std::string::npos; }

class CommunityPanel : public ui::Widget {
public:
    CommunityPanel(const assets::UiLayoutNode& node, HudContext& c) : Widget(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        Widget::Created(ui);
        ui_ = &ui;
        const auto layout = [&](const char* path) { return c_.layout2dt ? c_.layout2dt(path) : nullptr; };
        const auto* friendFile = layout("res_ui\\cumunity_friend.2dt");
        const auto* blockFile = layout("res_ui\\cumunity_cut.2dt");
        const auto* noteFile = layout("res_ui\\cumunity_note.2dt");
        if (friendFile == nullptr || friendFile->empty() || blockFile == nullptr || blockFile->empty() || noteFile == nullptr || noteFile->empty()) return;
        const assets::Ui2dtRecord& rootRecord = friendFile->front();
        BuildGuild2dt(ui, *friendFile, *this, rootRecord.rect.x, rootRecord.rect.y);
        root_ = Child(rootRecord.id);
        if (root_ == nullptr) return;
        root_->rect.w = rect.w;
        root_->rect.h = rect.h;
        friends_ = root_->Child(646);
        if (friends_ == nullptr) return;
        // The other two pages stand where the first does.
        const float pageX = friends_->rect.x, pageY = friends_->rect.y;
        BuildGuild2dt(ui, *blockFile, *root_, blockFile->front().rect.x - pageX, blockFile->front().rect.y - pageY);
        BuildGuild2dt(ui, *noteFile, *root_, noteFile->front().rect.x - pageX, noteFile->front().rect.y - pageY);
        blocks_ = root_->Child(59);
        letters_ = root_->Child(700);
        if (ui::Widget* stray = root_->Child(58)) stray->visible = false; // the note file's last record, never shown
        writer_ = letters_ != nullptr ? letters_->Child(800) : nullptr;
        many_ = letters_ != nullptr ? letters_->Child(900) : nullptr;
        if (blocks_ == nullptr || letters_ == nullptr || writer_ == nullptr || many_ == nullptr) return;
        for (int id : {339, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88}) friendRows_.push_back(friends_->Child(id));
        for (int id : {398, 47, 50, 53, 56, 60, 63, 66, 69, 72}) blockRows_.push_back(blocks_->Child(id));
        for (int id : {339, 385, 388, 391, 394, 397, 400, 403, 406, 409, 412}) letterRows_.push_back(letters_->Child(id));
        for (int id : {72, 77, 81, 85, 89, 93, 97, 101, 105, 109, 113}) manyRows_.push_back(many_->Child(id));
        if (auto* to = ui::As<ui::Edit>(writer_->Child(64))) to->limit = proto::kCommunityNameChars;
        if (auto* body = ui::As<ui::Edit>(writer_->Child(67))) {
            body->multiline = true;
            body->limit = proto::kLetterChars;
        }
        if (auto* tile = writer_->Child(53)) tile->SetText(c_.text("UIIT_STT_LETTER_LETTERWRITE"));
        if (ui::Widget* heads = friends_->Child(62)) heads->interactive = false;
        for (ui::Widget* page : {friends_, blocks_, letters_}) {
            if (auto* bar = dynamic_cast<ui::ScrollBar*>(page->Child(338))) bar->SetRange(0, 0);
        }
        if (auto* bar = dynamic_cast<ui::ScrollBar*>(many_->Child(117))) bar->SetRange(0, 0);
        LoadPcList();
        ShowBlockList(0);
        ShowTab(0);
    }

    void ShowTab(int tab) {
        tab_ = tab;
        if (friends_ == nullptr) return;
        friends_->visible = tab == 0;
        blocks_->visible = tab == 1;
        letters_->visible = tab == 2;
        CloseWriter();
        CloseMany();
        static constexpr int kTabs[] = {72, 73, 74};
        for (int i = 0; i < 3; ++i) {
            if (auto* b = dynamic_cast<ui::Button*>(root_->Child(kTabs[i]))) b->texture = i == tab ? Swap(b->node().ddj, "_off", "_on") : std::string();
        }
        if (tab == 2 && c_.community != nullptr && !c_.community->LettersListed()) Send(proto::LettersAsk{});
        seen_ = ~0ull;
    }
    void ShowBlockList(int list) {
        blockList_ = list;
        selectedBlock_ = -1;
        blockScroll_ = 0;
        if (blocks_ == nullptr) return;
        static constexpr int kTabs[] = {395, 396};
        for (int i = 0; i < 2; ++i) {
            if (auto* b = dynamic_cast<ui::Button*>(blocks_->Child(kTabs[i]))) b->texture = i == list ? Swap(b->node().ddj, "_off", "_on") : std::string();
        }
        seen_ = ~0ull;
    }
    int Tab() const { return tab_; }
    // The test options': a letter chosen and Read pressed.
    void ReadRow(int index) {
        selectedLetter_ = index;
        LetterButton(40);
    }
    // The gallery's: the writing pane, or the pane with the guild's (1) or union's (2) names ticked.
    void ShowWriter(int many) {
        ShowTab(2);
        if (many == 0) return OpenWriter("Beta");
        OpenMany(many);
        if (!manyChecked_.empty()) manyChecked_[0] = 1;
        Refresh();
    }

    // A letter being written or read: Escape closes it first.
    bool CloseOnEscape() {
        if (writer_ != nullptr && writer_->visible) {
            CloseWriter();
            CloseMany();
            return true;
        }
        return false;
    }

    void Update(ui::Frame& frame) override {
        const auto tip = [&](ui::Widget* w, const std::string& text) {
            if (w != nullptr && frame.pointer.hovered == w) frame.interface.ShowTooltip(text, w->ScreenRect());
        };
        if (root_ != nullptr) {
            tip(root_->Child(72), c_.text("UIIT_CTL_FRIEND"));
            tip(root_->Child(73), c_.text("UIIT_STT_BLOCKMAN_BLOCK"));
            tip(root_->Child(74), c_.text("UIIT_CTL_LETTER"));
        }
        if (blocks_ != nullptr && blocks_->visible) {
            tip(blocks_->Child(337), c_.text(blockList_ == 0 ? "UIIT_STT_CHATING_SHUT_SAVE_SERVER_HELP" : "UIIT_STT_CHATING_SHUT_SAVE_PC_HELP"));
            tip(blocks_->Child(395), c_.text("UIIT_STT_GET_WHISPER"));
            tip(blocks_->Child(396), c_.text("UIIT_CTL_WARENETWORK_DETAIL_TOTAL"));
        }
        if (c_.community == nullptr) return;
        const std::uint64_t revision = c_.community->Revision() + pcRevision_ * (1ull << 40);
        if (revision == seen_) return;
        seen_ = revision;
        Refresh();
    }

    bool OnNotice(const ui::Notice& n) override {
        if (c_.community == nullptr || c_.state == nullptr) return false;
        const ui::Widget* parent = n.source != nullptr ? n.source->parent() : nullptr;
        if (n.kind == ui::Notice::Kind::ScrollChanged && n.source != nullptr) {
            if (n.control == 338) {
                if (parent == friends_) friendScroll_ = n.value;
                if (parent == blocks_) blockScroll_ = n.value;
                if (parent == letters_) letterScroll_ = n.value;
                Refresh();
                return true;
            }
            if (n.control == 117 && parent == many_) {
                manyScroll_ = n.value;
                Refresh();
                return true;
            }
            return false;
        }
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr) return false;
        if (parent == root_) {
            if (n.control == 72) ShowTab(0);
            if (n.control == 73) ShowTab(1);
            if (n.control == 74) ShowTab(2);
            return true;
        }
        const auto rowOf = [&](const std::vector<ui::Widget*>& rows) {
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (rows[i] != nullptr && (n.source == rows[i] || parent == rows[i])) return int(i);
            }
            return -1;
        };
        if (const int row = rowOf(friendRows_); row >= 0) return Pick(selectedFriend_, friendScroll_ + row);
        if (const int row = rowOf(blockRows_); row >= 0) return Pick(selectedBlock_, blockScroll_ + row);
        if (const int row = rowOf(letterRows_); row >= 0) return Pick(selectedLetter_, letterScroll_ + row);
        if (const int row = rowOf(manyRows_); row >= 0) {
            const std::size_t at = std::size_t(manyScroll_ + row);
            if (at < manyChecked_.size()) manyChecked_[at] = manyChecked_[at] == 0 ? 1 : 0;
            Refresh();
            return true;
        }
        if (parent == friends_) return FriendButton(n.control);
        if (parent == blocks_) return BlockButton(n.control);
        if (parent == letters_) return LetterButton(n.control);
        if (parent == writer_) return WriterButton(n.control);
        if (parent == many_ && n.control == 115) {
            const bool all = std::all_of(manyChecked_.begin(), manyChecked_.end(), [](std::uint8_t on) { return on != 0; });
            for (std::uint8_t& on : manyChecked_) on = all ? 0 : 1;
            Refresh();
            return true;
        }
        return false;
    }

    template <typename T>
    void Send(const T& message) {
        if (c_.communityRequest) c_.communityRequest(T::kId, proto::Encode(message));
    }

    // The chat's commands and the tests.
    void AddPcBlock(const std::string& name) {
        if (BadName(name)) return Post("UIO_MSG_ERROR_CHARACTER_WRONGSTRING");
        if (c_.state == nullptr) return;
        auto& names = c_.state->chatBlocked;
        if (std::find(names.begin(), names.end(), name) != names.end()) return Post("UIIT_MSG_COSPETERR_PETNAME_SUMENESS");
        names.push_back(name);
        SavePcList();
        ++pcRevision_;
    }
    void RemovePcBlock(const std::string& name) {
        if (c_.state == nullptr) return;
        std::erase(c_.state->chatBlocked, name);
        SavePcList();
        ++pcRevision_;
    }

private:
    void Post(const char* key) {
        if (c_.state != nullptr) c_.state->Post(game::GameMessage::Kind::General, c_.text(key), c_.time);
    }
    bool Pick(int& selected, int index) {
        selected = index;
        Refresh();
        return true;
    }

    void LoadPcList() {
        if (c_.state == nullptr || !c_.state->chatBlocked.empty()) return;
        std::ifstream f(kPcBlockFile, std::ios::binary);
        std::uint32_t version = 0, count = 0;
        if (!f.read(reinterpret_cast<char*>(&version), 4) || !f.read(reinterpret_cast<char*>(&count), 4) || version != 1) return;
        for (std::uint32_t i = 0; i < count && f; ++i) {
            std::uint8_t size = 0;
            if (!f.read(reinterpret_cast<char*>(&size), 1)) break;
            std::string name(size, '\0');
            if (!f.read(name.data(), size)) break;
            if (!BadName(name)) c_.state->chatBlocked.push_back(std::move(name));
        }
    }
    void SavePcList() {
        if (c_.state == nullptr) return;
        std::ofstream f(kPcBlockFile, std::ios::binary | std::ios::trunc);
        const std::uint32_t version = 1, count = std::uint32_t(c_.state->chatBlocked.size());
        f.write(reinterpret_cast<const char*>(&version), 4);
        f.write(reinterpret_cast<const char*>(&count), 4);
        for (const std::string& name : c_.state->chatBlocked) {
            const std::uint8_t size = std::uint8_t(std::min<std::size_t>(name.size(), 255));
            f.write(reinterpret_cast<const char*>(&size), 1);
            f.write(name.data(), size);
        }
        if (!f) core::LogWarning("community: cannot write {}", kPcBlockFile);
    }

    // --- What is shown ------------------------------------------------------------------------------

    static void PutText(ui::Widget* w, const std::string& text) {
        if (w != nullptr) w->SetText(text);
    }
    static void Row(ui::Widget* row, bool on, bool selected) {
        if (row == nullptr) return;
        row->visible = true;
        if (auto* bar = dynamic_cast<ui::Bar*>(row)) bar->pieces = on && selected ? kSelectedBar : "";
        for (const auto& child : row->children()) child->visible = on;
    }
    static void SetScroll(ui::Widget* page, int id, int total, int rows, int& scroll) {
        auto* bar = page != nullptr ? dynamic_cast<ui::ScrollBar*>(page->Child(id)) : nullptr;
        const int most = std::max(0, total - rows);
        scroll = std::clamp(scroll, 0, most);
        if (bar == nullptr) return;
        bar->SetRange(0, most);
        bar->SetPosition(scroll);
    }

    void Refresh() {
        // A scroll bar set here reports the change back (ScrollChanged, on every call).
        if (c_.community == nullptr || friends_ == nullptr || refreshing_) return;
        refreshing_ = true;
        RefreshNow();
        refreshing_ = false;
    }

    void RefreshNow() {
        const game::CommunityView& view = *c_.community;
        // Friends: those online first (ours), by name.
        std::vector<const proto::FriendRecord*> people;
        for (const proto::FriendRecord& f : view.Friends()) people.push_back(&f);
        std::stable_sort(people.begin(), people.end(), [](const proto::FriendRecord* a, const proto::FriendRecord* b) {
            if ((a->online != 0) != (b->online != 0)) return a->online != 0;
            return a->name < b->name;
        });
        people_ = people;
        selectedFriend_ = std::min(selectedFriend_, int(people.size()) - 1);
        SetScroll(friends_, 338, int(people.size()), int(friendRows_.size()), friendScroll_);
        PutText(friends_->Child(18), std::format(": {} / {}", people.size(), proto::kMaxFriends));
        for (std::size_t i = 0; i < friendRows_.size(); ++i) {
            const std::size_t at = std::size_t(friendScroll_) + i;
            const proto::FriendRecord* f = at < people.size() ? people[at] : nullptr;
            Row(friendRows_[i], f != nullptr, f != nullptr && int(at) == selectedFriend_);
            if (f == nullptr || friendRows_[i] == nullptr) continue;
            ui::Widget* row = friendRows_[i];
            PutText(row->Child(341), f->name);
            if (auto* lamp = ui::As<ui::Static>(row->Child(52))) lamp->texture = f->online != 0 ? "interface\\guild\\gil_contact_on.ddj" : "interface\\guild\\gil_contact_off.ddj";
            if (auto* race = ui::As<ui::Static>(row->Child(51))) race->texture = RaceMark(f->model);
            if (auto* name = ui::As<ui::Static>(row->Child(341))) name->textColor = f->online != 0 ? 0xFFFFFFFFu : 0xFF808080u;
        }
        if (auto* b = friends_->Child(39)) b->enabled = selectedFriend_ >= 0;
        if (auto* b = friends_->Child(41)) b->enabled = selectedFriend_ >= 0;
        // The block lists.
        const std::vector<std::string> noNames;
        const std::vector<std::string>& names = blockList_ == 0 ? view.Blocked() : (c_.state != nullptr ? c_.state->chatBlocked : noNames);
        selectedBlock_ = std::min(selectedBlock_, int(names.size()) - 1);
        SetScroll(blocks_, 338, int(names.size()), int(blockRows_.size()), blockScroll_);
        PutText(blocks_->Child(336), c_.text(blockList_ == 0 ? "UIIT_STT_CHATING_SHUT_SAVE_SERVER" : "UIIT_STT_CHATING_SHUT_SAVE_PC"));
        PutText(blocks_->Child(18), blockList_ == 0 ? std::format(": {} / {}", names.size(), proto::kMaxWhisperBlocks) : std::format(": {}", names.size()));
        for (std::size_t i = 0; i < blockRows_.size(); ++i) {
            const std::size_t at = std::size_t(blockScroll_) + i;
            const bool here = at < names.size();
            Row(blockRows_[i], here, here && int(at) == selectedBlock_);
            if (here && blockRows_[i] != nullptr) PutText(blockRows_[i]->Child(400), names[at]);
            if (here && blockRows_[i] != nullptr) {
                if (auto* lamp = ui::As<ui::Static>(blockRows_[i]->Child(46))) lamp->texture = "interface\\guild\\gil_contact_off.ddj";
            }
        }
        if (auto* b = blocks_->Child(41)) b->enabled = selectedBlock_ >= 0;
        // The letters, in the order their headers chose (Date flips at each click, Sender by name; the server's at first).
        const std::vector<proto::LetterRecord>& mail = view.Letters();
        letterOrder_.resize(mail.size());
        for (std::size_t i = 0; i < mail.size(); ++i) letterOrder_[i] = i;
        if (letterSort_ != 0) {
            std::stable_sort(letterOrder_.begin(), letterOrder_.end(), [&](std::size_t a, std::size_t b) {
                const proto::LetterRecord &x = mail[a], &y = mail[b];
                const bool less = letterSort_ == 1 ? std::tie(x.year, x.month, x.day, x.hour, x.minute) < std::tie(y.year, y.month, y.day, y.hour, y.minute)
                                                   : x.from < y.from;
                const bool more = letterSort_ == 1 ? std::tie(y.year, y.month, y.day, y.hour, y.minute) < std::tie(x.year, x.month, x.day, x.hour, x.minute)
                                                   : y.from < x.from;
                return letterBackwards_ ? more : less;
            });
        }
        selectedLetter_ = std::min(selectedLetter_, int(mail.size()) - 1);
        SetScroll(letters_, 338, int(mail.size()), int(letterRows_.size()), letterScroll_);
        PutText(letters_->Child(18), std::format(": {} / {}", mail.size(), proto::kMaxLetters));
        for (std::size_t i = 0; i < letterRows_.size(); ++i) {
            const std::size_t at = std::size_t(letterScroll_) + i;
            const proto::LetterRecord* r = at < mail.size() ? &mail[letterOrder_[at]] : nullptr;
            Row(letterRows_[i], r != nullptr, r != nullptr && int(at) == selectedLetter_);
            if (r == nullptr || letterRows_[i] == nullptr) continue;
            ui::Widget* row = letterRows_[i];
            PutText(row->Child(341), r->from);
            const int hour = r->hour % 12 == 0 ? 12 : r->hour % 12;
            // yy-mm-dd Forenoon/Afternoon hh:mm.
            PutText(row->Child(304), std::format("{:02}-{:02}-{:02} {} {:02}:{:02}", r->year % 100, r->month, r->day,
                                                 c_.text(r->hour < 12 ? "UIIT_STT_LETTER_FORENOON" : "UIIT_STT_LETTER_AFTEMOON"), hour, r->minute));
            const ui::Color colour = r->read != 0 ? 0xFF808080u : 0xFFFFFFFFu;
            for (int id : {341, 304}) {
                if (auto* s = ui::As<ui::Static>(row->Child(id))) s->textColor = colour;
            }
        }
        if (auto* b = letters_->Child(40)) b->enabled = selectedLetter_ >= 0;
        if (auto* b = letters_->Child(41)) b->enabled = selectedLetter_ >= 0;
        // A letter that has come (Read asked for it).
        if (const auto& body = view.Body(); body && body->id != shownBody_ && waitingBody_ == body->id) ShowBody(*body);
        RefreshMany();
    }

    void RefreshMany() {
        if (many_ == nullptr || !many_->visible) return;
        SetScroll(many_, 117, int(manyNames_.size()), int(manyRows_.size()), manyScroll_);
        for (std::size_t i = 0; i < manyRows_.size(); ++i) {
            const std::size_t at = std::size_t(manyScroll_) + i;
            const bool here = at < manyNames_.size();
            Row(manyRows_[i], here, false);
            if (!here || manyRows_[i] == nullptr) continue;
            ui::Widget* row = manyRows_[i];
            PutText(row->Child(341), manyNames_[at].first);
            if (auto* race = ui::As<ui::Static>(row->Child(76))) race->texture = RaceMark(manyNames_[at].second);
            if (auto* box = dynamic_cast<ui::Button*>(row->Child(75))) box->texture = manyChecked_[at] != 0 ? Swap(box->node().ddj, "_off", "_on") : box->node().ddj;
        }
    }

    // --- What the buttons do ---------------------------------------------------------------------------

    bool FriendButton(int control) {
        const proto::FriendRecord* chosen = selectedFriend_ >= 0 && std::size_t(selectedFriend_) < people_.size() ? people_[std::size_t(selectedFriend_)] : nullptr;
        switch (control) {
        case 39:
            if (chosen != nullptr) {
                if (auto* chat = dynamic_cast<ChatWindow*>(ui_->Window(hud::kChat))) chat->Address(chosen->name, true);
            }
            return true;
        case 40:
            AskGuildText(c_, c_.text("UIIT_MSG_FRIEND_IDINPUT"), proto::kCommunityNameChars, [this](std::string name) { Send(proto::FriendAsk{std::move(name)}); });
            return true;
        case 41:
            if (chosen != nullptr && c_.hud != nullptr) {
                const std::uint32_t character = chosen->character;
                c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {FormatText(c_.text("UIIT_MSG_FRIEND_IDDELET"), {chosen->name})}, [this, character](bool yes) {
                    if (yes) Send(proto::FriendDelete{character});
                });
            }
            return true;
        default: return false;
        }
    }

    bool BlockButton(int control) {
        const std::vector<std::string>& names = blockList_ == 0 ? c_.community->Blocked() : c_.state->chatBlocked;
        switch (control) {
        case 395: ShowBlockList(0); return true;
        case 396: ShowBlockList(1); return true;
        case 40:
            AskGuildText(c_, c_.text("UIIT_STT_BLOCKMAN_ENTRY_WND"), proto::kCommunityNameChars, [this](std::string name) {
                if (blockList_ != 0) return AddPcBlock(name);
                if (BadName(name)) return Post("UIO_MSG_ERROR_CHARACTER_WRONGSTRING");
                Send(proto::WhisperBlock{1, std::move(name)});
            });
            return true;
        case 41:
            if (selectedBlock_ >= 0 && std::size_t(selectedBlock_) < names.size() && c_.hud != nullptr) {
                const std::string name = names[std::size_t(selectedBlock_)];
                c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {FormatText(c_.text("UIIT_STT_BLOCKMAN_DELETE_WND"), {name})}, [this, name](bool yes) {
                    if (!yes) return;
                    if (blockList_ != 0) return RemovePcBlock(name);
                    Send(proto::WhisperBlock{2, name});
                });
            }
            return true;
        default: return false;
        }
    }

    const proto::LetterRecord* ChosenLetter() const {
        const auto& mail = c_.community->Letters();
        if (selectedLetter_ < 0 || std::size_t(selectedLetter_) >= mail.size()) return nullptr;
        const std::size_t at = std::size_t(selectedLetter_) < letterOrder_.size() ? letterOrder_[std::size_t(selectedLetter_)] : std::size_t(selectedLetter_);
        return at < mail.size() ? &mail[at] : nullptr;
    }

    bool LetterButton(int control) {
        switch (control) {
        case 113: // the Date header: by time, the other way at each click
            letterBackwards_ = letterSort_ == 1 ? !letterBackwards_ : false;
            letterSort_ = 1;
            seen_ = ~0ull; // shown again
            return true;
        case 114: // the Sender header: by name
            letterSort_ = 2;
            letterBackwards_ = false;
            seen_ = ~0ull;
            return true;
        case 40:
            if (const proto::LetterRecord* r = ChosenLetter()) {
                waitingBody_ = r->id;
                shownBody_ = 0;
                Send(proto::LetterOpen{r->id});
            }
            return true;
        case 41:
            if (const proto::LetterRecord* r = ChosenLetter()) AskDelete(r->id);
            return true;
        case 55: OpenWriter(""); return true;
        case 56: OpenMany(1); return true;
        case 57: OpenMany(2); return true;
        default: return false;
        }
    }

    void AskDelete(std::uint64_t id) {
        if (c_.hud == nullptr) return;
        c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {c_.text("UIIT_MSG_LETTER_LETTERDELETE")}, [this, id](bool yes) {
            if (!yes) return;
            Send(proto::LetterDelete{id});
            CloseWriter();
        });
    }

    void CloseWriter() {
        if (writer_ != nullptr) writer_->visible = false;
        waitingBody_ = 0;
    }
    void CloseMany() {
        if (many_ != nullptr) many_->visible = false;
        manyKind_ = 0;
    }

    // Write mode: 100 and 200; read mode: 118, 119 and 120 (the same pane).
    void SetWriterMode(bool write) {
        for (int id : {100, 200}) {
            if (ui::Widget* b = writer_->Child(id)) b->visible = write;
        }
        for (int id : {118, 119, 120}) {
            if (ui::Widget* b = writer_->Child(id)) b->visible = !write;
        }
        if (auto* to = ui::As<ui::Edit>(writer_->Child(64))) to->readOnly = !write;
        if (auto* body = ui::As<ui::Edit>(writer_->Child(67))) body->readOnly = !write;
        PutText(writer_->Child(53), c_.text(write ? "UIIT_STT_LETTER_LETTERWRITE" : "UIIT_STT_LETTER_READ"));
        PutText(writer_->Child(59), c_.text(write ? "UIIT_STT_LETTER_ADDRESEE" : "UIIT_STT_LETTER_SENDER"));
    }

    void OpenWriter(const std::string& to) {
        if (writer_ == nullptr) return;
        CloseMany();
        waitingBody_ = 0;
        reading_ = 0;
        writer_->visible = true;
        SetWriterMode(true);
        if (auto* edit = ui::As<ui::Edit>(writer_->Child(64))) edit->SetValue(to);
        if (auto* edit = ui::As<ui::Edit>(writer_->Child(67))) {
            edit->SetValue("");
            if (!to.empty() && ui_ != nullptr) ui_->Focus(edit);
        }
    }

    void ShowBody(const proto::LetterBody& body) {
        shownBody_ = body.id;
        reading_ = body.id;
        from_ = body.from;
        writer_->visible = true;
        CloseMany();
        SetWriterMode(false);
        if (auto* edit = ui::As<ui::Edit>(writer_->Child(64))) edit->SetValue(body.from);
        if (auto* edit = ui::As<ui::Edit>(writer_->Child(67))) edit->SetValue(body.text);
    }

    // The guild's members or the union's masters to tick (CNIFMultiLetter), and the letter's pane.
    void OpenMany(int kind) {
        if (c_.guild == nullptr || many_ == nullptr || writer_ == nullptr) return;
        manyNames_.clear();
        if (kind == 1) {
            for (const proto::GuildMemberRecord& m : c_.guild->Guild().members) {
                if (m.name != SelfName()) manyNames_.push_back({m.name, m.model});
            }
        } else {
            for (const proto::UnionGuildRecord& g : c_.guild->Union().guilds) {
                if (g.master != SelfName()) manyNames_.push_back({g.master, g.masterModel});
            }
        }
        manyChecked_.assign(manyNames_.size(), 0);
        manyScroll_ = 0;
        manyKind_ = kind;
        many_->visible = true;
        PutText(many_->Child(71), c_.text(kind == 1 ? "UIIT_MSG_LETTERERR_GUILD" : "UIIT_MSG_LETTERERR_UNION"));
        waitingBody_ = 0;
        reading_ = 0;
        writer_->visible = true;
        SetWriterMode(true);
        if (auto* edit = ui::As<ui::Edit>(writer_->Child(64))) {
            edit->SetValue(c_.text(kind == 1 ? "UIIT_CTL_LETTERERR_GUILD" : "UIIT_CTL_LETTERERR_UNION"));
            edit->readOnly = true;
        }
        if (auto* edit = ui::As<ui::Edit>(writer_->Child(67))) edit->SetValue("");
        Refresh();
    }

    std::string SelfName() const { return c_.state != nullptr ? c_.state->self.name : std::string(); }

    bool WriterButton(int control) {
        switch (control) {
        case 200: CloseWriter(); CloseMany(); return true;
        case 120: CloseWriter(); return true;
        case 118: OpenWriter(from_); return true;
        case 119:
            if (reading_ != 0) AskDelete(reading_);
            return true;
        case 100: {
            const auto* to = ui::As<ui::Edit>(writer_->Child(64));
            const auto* body = ui::As<ui::Edit>(writer_->Child(67));
            if (to == nullptr || body == nullptr) return true;
            if (body->text().empty()) {
                Post("UIIT_MSG_LETTERERR_MESSAGEWRITE");
                return true;
            }
            if (manyKind_ != 0) {
                proto::LetterWriteMany letter;
                letter.kind = std::uint8_t(manyKind_);
                letter.text = body->text();
                for (std::size_t i = 0; i < manyNames_.size(); ++i) {
                    if (manyChecked_[i] != 0) letter.names.push_back(manyNames_[i].first);
                }
                if (letter.names.empty()) {
                    Post("UIIT_MSG_LETTERERR_IDWRITE");
                    return true;
                }
                Send(letter);
            } else {
                if (to->text().empty()) {
                    Post("UIIT_MSG_LETTERERR_IDWRITE");
                    return true;
                }
                Send(proto::LetterWrite{to->text(), body->text()});
            }
            CloseWriter();
            CloseMany();
            return true;
        }
        default: return false;
        }
    }

    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    ui::Widget* root_ = nullptr;
    ui::Widget* friends_ = nullptr;
    ui::Widget* blocks_ = nullptr;
    ui::Widget* letters_ = nullptr;
    ui::Widget* writer_ = nullptr;
    ui::Widget* many_ = nullptr;
    std::vector<ui::Widget*> friendRows_, blockRows_, letterRows_, manyRows_;
    std::vector<const proto::FriendRecord*> people_;
    std::vector<std::pair<std::string, std::uint32_t>> manyNames_;
    std::vector<std::uint8_t> manyChecked_;
    bool refreshing_ = false;
    int tab_ = 0, blockList_ = 0, manyKind_ = 0;
    int selectedFriend_ = -1, selectedBlock_ = -1, selectedLetter_ = -1;
    int friendScroll_ = 0, blockScroll_ = 0, letterScroll_ = 0, manyScroll_ = 0;
    int letterSort_ = 0;                   // the letters' order: 0 the server's, 1 by date, 2 by sender
    bool letterBackwards_ = false;         // the order the other way
    std::vector<std::size_t> letterOrder_; // the letters shown, row by row (their places in the view)
    std::uint64_t seen_ = ~0ull, pcRevision_ = 0;
    std::uint64_t waitingBody_ = 0, shownBody_ = 0, reading_ = 0;
    std::string from_;
};

CommunityPanel* PanelOf(ui::Widget* window) { return window != nullptr ? dynamic_cast<CommunityPanel*>(window->Child(kPanelId)) : nullptr; }

// CIFLetterAlarmGuide (ginterface 55): the envelope standing while letters wait unread, its two
// pictures in turn; a click opens the letters (ours: where it stands, under the event
// guides, and the pictures' pace, are not traced).
class LetterAlarm : public ui::Static {
public:
    LetterAlarm(const assets::UiLayoutNode& node, HudContext& c) : Static(node), c_(c) {}
    void Created(ui::Interface& ui) override {
        Static::Created(ui);
        interactive = true;
    }
    void Update(ui::Frame& frame) override {
        const bool second = int(c_.time * 2.0) % 2 != 0;
        texture = second ? "interface\\icon\\etc\\letteralarm_2.ddj" : "interface\\icon\\etc\\letteralarm_1.ddj";
        if (frame.pointer.hovered == this) frame.interface.ShowTooltip(c_.text("UIIT_MSG_LETTERERR_UNKNOWNLETTER"), ScreenRect());
    }
    void Mouse(ui::Frame& frame) override {
        if (frame.input.Released(ui::MouseButton::Left) && frame.pointer.hovered == this) ToggleCommunityPage(c_, 2);
    }

private:
    HudContext& c_;
};

} // namespace

ui::Widget* AddCommunityPanel(ui::Interface& ui, HudContext& context, ui::Widget& window) {
    if (context.layout2dt == nullptr) return nullptr;
    assets::UiLayoutNode node;
    node.type = "CNIFCommunityPanel";
    node.id = kPanelId;
    node.rect = {0.0f, 0.0f, window.rect.w, window.rect.h};
    auto made = ui.Make(node);
    ui::Widget* panel = window.Add(std::move(made));
    panel->visible = false;
    return panel;
}

void RegisterCommunityWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CNIFCommunityPanel", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<CommunityPanel>(n, *c); });
    ui.Register("CIFLetterAlarmGuide", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<LetterAlarm>(n, *c); });
}

void ShowCommunityTab(ui::Widget* window, int tab) {
    if (auto* panel = PanelOf(window)) {
        panel->visible = true;
        panel->ShowTab(tab);
    }
}

void ShowCommunityWriter(ui::Widget* window, int many) {
    if (auto* panel = PanelOf(window)) {
        panel->visible = true;
        panel->ShowWriter(many);
    }
}

void CommunityReadLetter(HudContext& context, int index) {
    if (auto* panel = PanelOf(context.hud != nullptr ? context.hud->Show(hud::kCommunity) : nullptr)) panel->ReadRow(index);
}

bool CloseCommunityLetter(ui::Widget* window) {
    auto* panel = PanelOf(window);
    return panel != nullptr && panel->visible && panel->CloseOnEscape();
}

bool CommunityChatCommand(HudContext& c, const std::string& line) {
    // The chat's commands: the first word against the localized ones.
    const std::size_t space = line.find(' ');
    const std::string word = line.substr(0, space);
    const std::string name = space == std::string::npos ? std::string() : line.substr(line.find_first_not_of(' ', space) == std::string::npos ? line.size() : line.find_first_not_of(' ', space));
    const auto is = [&](const char* key) { return c.hasText && c.hasText(key) && c.text(key) == word; };
    const auto send = [&](const auto& message) {
        using T = std::decay_t<decltype(message)>;
        if (c.communityRequest) c.communityRequest(T::kId, proto::Encode(message));
    };
    if (is("UIIT_STT_CHAT_COMMAND_FRIEND_INVITE") || is("UIIT_STT_CHAT_COMMAND_FRIEND_INVITE1") || is("UIIT_STT_CHAT_COMMAND_FRIEND_INVITE2")) {
        if (!name.empty()) send(proto::FriendAsk{name});
        return true;
    }
    if (is("UIIT_STT_CHAT_COMMAND_WHISPER_BLOCK")) {
        if (!name.empty()) send(proto::WhisperBlock{1, name});
        return true;
    }
    if (is("UIIT_STT_CHAT_COMMAND_WHISPER_ADMISSION")) {
        if (!name.empty()) send(proto::WhisperBlock{2, name});
        return true;
    }
    const bool pcBlock = is("UIIT_STT_CHAT_COMMAND_BLOCK");
    const bool pcAllow = is("UIIT_STT_CHAT_COMMAND_ADMISSION");
    if (pcBlock || pcAllow) {
        ui::Widget* window = c.hud != nullptr ? c.hud->Show(hud::kCommunity) : nullptr;
        if (auto* panel = PanelOf(window); panel != nullptr && !name.empty()) {
            if (pcBlock) panel->AddPcBlock(name);
            else panel->RemovePcBlock(name);
        }
        if (window != nullptr && c.hud != nullptr) c.hud->Hide(hud::kCommunity);
        return true;
    }
    // /BlockTradeInvitation, /AcceptTradeInvitation: the Option window's "Exchange Request" off or on (ours:
    // one setting for both; the original's command's own store is not traced), saved with the options.
    if (is("UIIT_STT_CHAT_COMMAND_TRADE_BLOCK") || is("UIIT_STT_CHAT_COMMAND_TRADE_ADMISSION")) {
        const bool block = is("UIIT_STT_CHAT_COMMAND_TRADE_BLOCK");
        if (c.options != nullptr) {
            c.options->game.Set("exchangeRequests", !block);
            if (c.optionsChanged) c.optionsChanged();
        }
        if (c.state != nullptr) {
            c.state->Post(game::GameMessage::Kind::General, FormatText(c.text(block ? "UIIT_MSG_CHAT_TRADE_BLOCK" : "UIIT_MSG_CHAT_TRADE_ADMISSION"), {c.state->self.name}), c.time);
        }
        return true;
    }
    if (is("UIIT_STT_CHAT_COMMAND_FRIEND_BLOCK") || is("UIIT_STT_CHAT_COMMAND_FRIEND_ADMISSION")) {
        if (c.state != nullptr) {
            const bool block = is("UIIT_STT_CHAT_COMMAND_FRIEND_BLOCK");
            c.state->friendInvitesBlocked = block;
            c.state->Post(game::GameMessage::Kind::General, FormatText(c.text(block ? "UIIT_MSG_CHAT_FRIEND_BLOCK" : "UIIT_MSG_CHAT_FRIEND_ADMISSION"), {c.state->self.name}), c.time);
        }
        return true;
    }
    return false;
}

std::string CommunityLineText(const HudContext& c, const game::CommunityView::Line& line) {
    using Kind = game::CommunityView::Line::Kind;
    using Code = proto::CommunityCode;
    const auto text = [&](const char* key) { return c.text(key); };
    switch (line.kind) {
    case Kind::FriendOnline: return FormatText(text("UIIT_MSG_FRIEND_CONNECT"), {line.name});
    case Kind::FriendOffline: return FormatText(text("UIIT_MSG_FRIEND_DISCONNECT"), {line.name});
    case Kind::FriendAdded: return FormatText(text("UIIT_MSG_CHAT_FRIEND_INVITE"), {line.name});
    case Kind::LetterArrived: return text("UIIT_MSG_LETTERERR_UNKNOWNLETTER");
    case Kind::Block:
        // 0xB66F's numbers; done says nothing, the list shows it.
        switch (line.result) {
        case 1: return text("UIIT_MSG_COSPETERR_PETNAME_SUMENESS");
        case 2: return text("UIIT_MSG_ALIAS_ACTION_ERR_NOTALLOW");
        case 3: return text("UIIT_STT_BLOCKMAN_DELETE_LISTFULL");
        default: return {};
        }
    case Kind::Answer: break;
    }
    if (line.topic == proto::CommunityTopic::Friend) {
        switch (line.code) {
        case Code::FriendListFull: return text("UIIT_MSG_FRIENDERR_FULLLIST");
        case Code::AlreadyFriend: return text("UIIT_MSG_FRIENDERR_ALREADYFRIEND");
        case Code::NoAnswer: return text("UIIT_MSG_FRIEND_SYSCANCEL");
        case Code::Refused: return text("UIIT_MSG_FRIENDERR_REFUSAL");
        case Code::UnknownUser: return text("UIIT_MSG_FRIENDERR_UNKNOWNUSER");
        case Code::InProgress: return text("UIIT_MSG_FRIEND_SYSPROGRESS");
        case Code::TargetListFull: return text("UIIT_MSG_FRIENDERR_TARGETLISTFULL");
        default: return {};
        }
    }
    switch (line.code) {
    case Code::Ok: return text("UIIT_MSG_LETTER_SENDOK");
    case Code::TargetBoxFull:
        if (line.topic == proto::CommunityTopic::LetterMany) return FormatText(text("UIIT_MSG_LETTERERR_GUILD_FULLLETTERBOX"), {line.name});
        return text("UIIT_MSG_LETTERERR_OBJECT_FULLLETTERBOX");
    case Code::LetterBoxFull: return text("UIIT_MSG_LETTERERR_FULLLETTERBOX");
    case Code::LetterUnknownUser: return text("UIIT_MSG_LETTERERR_UNKNOWNUSER");
    case Code::LetterFailed: return text("UIIT_MSG_LETTERERR_SENDERR");
    default: return {};
    }
}

} // namespace sr::client
