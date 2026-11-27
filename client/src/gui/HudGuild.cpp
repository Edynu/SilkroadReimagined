// The guild window: the 1.188 client's CNIFGuildWnd, built from its
// binary layouts (res_ui\guild.2dt; the relations page res_ui\guild_r.2dt) on the community key (U),
// where v1.150 had CIFCommunity. A view over game::GuildView: the guild's info, its members, the
// buttons each member's authority allows, the authority panel, the union page; what it asks goes to
// the server as guild requests.

#include <algorithm>
#include <format>
#include <map>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/assets/UiLayout2dt.h"
#include "sr/game/Guild.h"
#include "sr/game/GuildMercenary.h"

namespace sr::client {

namespace proto = game::proto;

void RegisterGuildBoxes(ui::Interface& ui); // the name box (below)
// The Grant name button's box (below): the member's title asked, confirmed, sent (0x7256).
void AskGuildTitle(HudContext& c, std::uint32_t character, const std::string& name);

namespace {

// The guild error codes the client has a text for (its error text map); the others
// (3, 0x4C0D, 0x4C0E, 0x4C10, 0x4C2E, 0x4C2F, 0x4C30, ...) print nothing.
const char* ErrorKey(std::uint16_t code) {
    static const std::map<std::uint16_t, const char*> kKeys = {
        {0x4C06, "UIIT_MSG_GUILDERR_TARGET_BUSY"},
        {0x4C07, "UIIT_MSG_GUILDERR_IM_DEAD"},
        {0x4C0A, "UIIT_MSG_GUILDERR_TOO_LOW_CREATOR_LEVEL"},
        {0x4C0C, "UIIT_MSG_GUILDERR_NOT_ENOUGH_GOLD"},
        {0x4C12, "UIIT_MSG_GUILDERR_EXISTING_MEMBER"},
        {0x4C13, "UIIT_MSG_GUILDERR_MEMBER_FULL"},
        {0x4C16, "UIIT_MSG_GUILDERR_JOIN_GUILD_REFUSED"},
        {0x4C17, "UIIT_MSG_GUILDERR_MEMBER_OF_ANOTHER_GUILD"},
        {0x4C18, "UIIT_MSG_GUILDERR_INVALID_GUILDNAME_LEN"},
        {0x4C19, "UIIT_MSG_GUILDERR_NOT_ALLOWED_GUILDNAME"},
        {0x4C1A, "UIIT_MSG_GUILDERR_SAME_GUILDNAME_EXIST"},
        {0x4C1B, "UIIT_MSG_GUILDERR_CANT_CREATE_GUILD_IN_DB"},
        {0x4C1D, "UIIT_MSG_GUILDERR_CANT_FIND_MEMBER"},
        {0x4C1E, "UIIT_MSG_GUILDERR_PERMISSION_DENIED"},
        {0x4C1F, "UIIT_MSG_GUILD_EXPULSION_NOTEXP"},
        {0x4C21, "UIIT_MSG_ERROR_GUILD_LEVEL_UP_FULL"},
        {0x4C22, "UIIT_MSG_GUILDERR_INVALID_MASTER_COMMENT_TITLE"},
        {0x4C23, "UIIT_MSG_GUILDERR_INVALID_MASTER_COMMENT"},
        {0x4C24, "UIIT_MSG_GUILDERR_TARGET_PERMISSION_DENIED"},
        {0x4C25, "UIIT_MSG_GUILDERR_CANT_FIND_TARGET_GUILD"},
        {0x4C26, "UIIT_MSG_GUILDERR_CANT_ALLY_TO_OWN_GUILD"},
        {0x4C27, "UIIT_MSG_GUILDERR_ALREADY_ALLIED"},
        {0x4C28, "UIIT_MSG_GUILDERR_TARGET_GUILD_HAS_ALLIANCE_ALREADY"},
        {0x4C29, "UIIT_MSG_GUILDERR_ALLIANCE_FULL"},
        {0x4C2A, "UIIT_MSG_GUILDERR_TOO_LOW_CREATOR_ALLIANCE"},
        {0x4C2B, "UIIT_MSG_GUILDERR_TOO_LOW_JOINER_ALLIANCE"},
        {0x4C31, "UIIT_MSG_ERROR_GUILD_LEVEL_UP_GOLD_DEFICIT"},
        {0x4C32, "UIIT_MSG_ERROR_GUILD_LEVEL_UP_GP_DEFICIT"},
        {0x4C36, "UIIT_MSG_GUILD_EXPULSION_NOTEXP"},
        {0x4C3D, "UIIT_MSG_GUILD_PENALTY"},
        {0x4C4B, "UIIT_MSG_GUILD_UNION_CHAT_FULL"},
        {0x4C4D, "UIIT_MSG_GUILD_ERROR_NAME_GRANT_SPECIAL_LETTER"},
        // Fortress positions (the same switch).
        {0x4C56, "UIIT_CTL_GUILD_JOIN_JOB_ERROR"},   //, banner + line
        {0x4C57, "UIIT_CTL_GUILD_CREATE_JOB_ERROR"},
        {0x4C58, "UIIT_MSG_FORT_POSITION_GRANT_FAIL_04"},
        {0x4C59, "UIIT_MSG_FORT_POSITION_GRANT_FAIL_03"},
        {0x4C5A, "UIIT_MSG_FORT_POSITION_GRANT_FAIL_02"},
        {0x4C5B, "UIIT_MSG_FORT_POSITION_GRANT_FAIL_05"},
        {0x4C5C, "UIIT_MSG_FORT_POSITION_GRANT_FAIL_06"},
        {0x4C5D, "UIIT_MSG_FORT_POSITION_GRANT_FAIL_01"},
        // The election (the same switch; 0x5008).
        {0x4C38, "UIIT_MSG_MRELEASEERR_NOTSECEDE"},
        {0x4C39, "UIIT_MSG_MRELEASEERR_NOTREMOVE"},
        {0x5008, "UIIT_MSG_MRELEASEERR_NOTVOTING"},
        // Guild wars (the same switch).
        {0x4C3A, "UIIT_MSG_GUILDWARERR_ENEMYGUILD"},
        {0x4C3E, "UIIT_MSG_GUILDWARERR_MAX_HOSTILE_GUILD"},
        {0x4C3F, "UIIT_MSG_GUILDWARERR_TARGET_MAX_HOSTILE_GUILD"},
        {0x4C41, "UIIT_MSG_GUILDWARERR_ALLYGUILD"},
        {0x4C43, "UIIT_MSG_GUILDWAR_SUGGESTIONS_02"},
        {0x4C46, "UIIT_MSG_GUILD_ERROR_BREAK_WAR"},
        {0x4C70, "UIIT_MSG_FORT_DISSOLUTION_POSFORTGUILD_ERROR"}, // the stock client prints "The behavior is not allowed." for both
        {0x4C74, "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_06"},
        {0x4C71, "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_06"},
        {0x4C72, "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_06"},
        {0x4C73, "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_06"},
        {0x4C75, "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_06"},
        {0x4C76, "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_06"},
        {0x4C77, "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_06"},
        {0x4C48, "UIIT_MSG_GUILD_WAREHOUSE_INTERRUPT"},
        {0x4C4C, "UIIT_MSG_GUILD_WAREHOUSE_USE_ING"},
        {0x4C42, "UIIT_MSG_QUESTION_GUILD_RESPECT_ALLY_NOTALLY"},
        {0x4C7A, "UIIT_MGS_ARENA_ERR_FREQUEST_WAR_IN_BATTLE_ARENA"},
        {0x4C60, "UIIT_STT_ERR_COMMON_TOO_FAR"},
    };
    const auto it = kKeys.find(code);
    return it != kKeys.end() ? it->second : nullptr;
}

// A member's race mark by its model (the Chinese figures' ids are below the Europeans', 14875 on).
std::string RaceMark(std::uint32_t model) {
    return model >= 14875 ? "interface\\ifcommon\\com_kindred_europe16.ddj" : "interface\\ifcommon\\com_kindred_china16.ddj";
}

std::string Variant(const std::string& art, std::string_view from, std::string_view to) {
    std::string out = art;
    if (const std::size_t at = out.rfind(from); at != std::string::npos) out.replace(at, from.size(), to);
    return out;
}

// A row of a list (the member rows, the union's guilds, the authority rows): a bar of three pieces
// that takes a click (CNIFBarWnd).
class GuildRow : public ui::Bar {
public:
    using Bar::Bar;
    void Created(ui::Interface& ui) override {
        Bar::Created(ui);
        interactive = true;
    }
    void Mouse(ui::Frame& frame) override {
        if (frame.input.Released(ui::MouseButton::Left) && frame.pointer.hovered == this) Notify(ui::Notice::Kind::Clicked);
    }
};

// The notice's line (CNIFGuildNotifySlot): a picture that takes a click.
class GuildLine : public ui::Static {
public:
    using Static::Static;
    void Created(ui::Interface& ui) override {
        Static::Created(ui);
        interactive = true;
    }
    void Mouse(ui::Frame& frame) override {
        if (frame.input.Released(ui::MouseButton::Left) && frame.pointer.hovered == this) Notify(ui::Notice::Kind::Clicked);
    }
};

constexpr const char* kSelectedBar = "interface\\ifcommon\\com_bar01select_";

// What the first column shows, the arrows button going through them.
constexpr const char* kFirstColumnHeads[] = {"UIIT_STT_GUILDSMAN", "UIIT_STT_TITLE", "UIIT_STT_GUILD_POSITION"};

} // namespace

std::string GuildErrorText(const HudContext& c, std::uint16_t error, std::uint32_t value) {
    // The war's reward over its limit: the limit in the line.
    if (error == 0x4C5E) return FormatText(c.text("UIIT_MSG_GUILDWARERR_LIMIT_COMPENSATION"), {"500,000,000"});
    if (error == 0x4C3C) {
        // The leave penalty: its days, hours and minutes left (UIIT_MSG_GUILD_SECESSION_PENALTY).
        const std::uint32_t minutes = (value + 59) / 60;
        return FormatText(c.text("UIIT_MSG_GUILD_SECESSION_PENALTY"),
                          {std::to_string(minutes / 1440), std::to_string(minutes / 60 % 24), std::to_string(minutes % 60)});
    }
    // No war points to spend: the original's pair of lines.
    if (error == 0x4C43) return c.text("UIIT_MSG_GUILDWAR_SUGGESTIONS_01") + "\n" + c.text("UIIT_MSG_GUILDWAR_SUGGESTIONS_02");
    const char* key = ErrorKey(error);
    return key != nullptr ? c.text(key) : std::string();
}

// Each record a widget under its parent (by the parent's id and its own parent's), its rect
// relative to the parent's; the root is the window.
void BuildGuild2dt(ui::Interface& ui, const std::vector<assets::Ui2dtRecord>& records, ui::Widget& window, float ox, float oy) {
    std::map<std::pair<int, int>, std::pair<ui::Widget*, assets::UiLayoutRect>> made;
    const assets::Ui2dtRecord& root = records.front();
    std::size_t first = 0;
    if (root.kind == 0) {
        made[{root.id, 0}] = {&window, root.rect};
        first = 1;
    }
    for (std::size_t i = first; i < records.size(); ++i) {
        const assets::Ui2dtRecord& r = records[i];
        ui::Widget* under = &window;
        assets::UiLayoutRect base{ox, oy, 0, 0};
        if (const auto it = made.find({r.parents[0], r.parents[1]}); it != made.end()) {
            under = it->second.first;
            base = it->second.second;
        } else if (r.parents[0] != 0) {
            continue; // a parent the file never made
        }
        assets::UiLayoutNode node = assets::Ui2dtNode(r, base.x, base.y);
        if (r.kind == 15) {
            node.type = "CNIFGuildRow";
            // The art names the bar's pieces: com_bar01_left.ddj is the prefix com_bar01_.
            for (std::string_view piece : {"left.ddj", "mid.ddj", "right.ddj", "Left.ddj", "Mid.ddj", "Right.ddj"}) {
                if (const std::size_t at = node.ddj.rfind(piece); at != std::string::npos) node.ddj.resize(at);
            }
        }
        if (r.id == 222) node.type = "CNIFGuildLine";
        if (r.kind == 7) node.type = "CIFTextBox";
        if (r.kind == 11) node.type = "CIFButton"; // the authority boxes: their faces set here
        std::unique_ptr<ui::Widget> w = ui.Make(node);
        if (auto* b = dynamic_cast<ui::Button*>(w.get()); b != nullptr && r.kind == 11) b->behaviour |= ui::Button::kNoFaces;
        ui::Widget* added = under->Add(std::move(w));
        made[{r.id, r.parents[0]}] = {added, r.rect};
    }
}

class GuildWindow : public ui::MainFrame {
public:
    GuildWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        SetText(c_.text ? c_.text("UIIT_STT_GUILDVIEW") : std::string("Guild"));
        MainFrame::Created(ui);
        ui_ = &ui;
        const auto* main = c_.layout2dt ? c_.layout2dt("res_ui\\guild.2dt") : nullptr;
        const auto* relations = c_.layout2dt ? c_.layout2dt("res_ui\\guild_r.2dt") : nullptr;
        if (main == nullptr || main->empty()) return;
        const assets::Ui2dtRecord& root = main->front();
        BuildGuild2dt(ui, *main, *this, root.rect.x, root.rect.y);
        info_ = Child(47);
        // The relations page (its own file, its own space) where the info page stands.
        if (relations != nullptr && !relations->empty() && info_ != nullptr) {
            BuildGuild2dt(ui, *relations, *this, relations->front().rect.x - info_->rect.x, relations->front().rect.y - info_->rect.y);
        }
        // Pages by their place in the tree (ids repeat across the files: 86 is also the info page's
        // arrow box).
        // The war status page likewise (tab 74).
        if (const auto* wars = c_.layout2dt ? c_.layout2dt("res_ui\\guild_w.2dt") : nullptr; wars != nullptr && !wars->empty() && info_ != nullptr) {
            BuildGuild2dt(ui, *wars, *this, wars->front().rect.x - info_->rect.x, wars->front().rect.y - info_->rect.y);
        }
        war_ = Child(548);
        relations_ = Child(164);
        union_ = relations_ != nullptr ? relations_->Child(86) : nullptr;
        hostile_ = relations_ != nullptr ? relations_->Child(2000) : nullptr;
        grant_ = info_ != nullptr ? info_->Child(1000) : nullptr;
        noticeView_ = info_ != nullptr ? info_->Child(1200) : nullptr;
        noticeLine_ = info_ != nullptr ? info_->Child(222) : nullptr;
        if (auto* body = noticeView_ != nullptr ? ui::As<ui::Edit>(noticeView_->Child(155)) : nullptr) {
            body->multiline = true;
            body->readOnly = true;
        }
        for (int id : {28, 232, 239, 246, 253, 260}) rows_.push_back(info_ != nullptr ? info_->Child(id) : nullptr);
        for (int id = 271; id <= 307; id += 4) unionRows_.push_back(union_ != nullptr ? union_->Child(id) : nullptr);
        for (int id = 1100; id <= 1105; ++id) grantRows_.push_back(grant_ != nullptr ? grant_->Child(id) : nullptr);
        for (int id : {225, 231, 235, 239, 243, 247, 255, 259, 263, 267}) hostileRows_.push_back(hostile_ != nullptr ? hostile_->Child(id) : nullptr);
        // By their place from the top (the file's ids do not run in order).
        for (int id : {257, 522, 524, 526, 530, 528, 532}) warRows_.push_back(war_ != nullptr ? war_->Child(id) : nullptr);
        for (int id : {250, 506, 510, 514}) contributionRows_.push_back(war_ != nullptr ? war_->Child(id) : nullptr);
        if (war_ != nullptr) {
            // Plain marks of who is in the war, not a list's head.
            for (int id : {245}) {
                if (ui::Widget* head = war_->Child(id)) head->interactive = false;
            }
        }
        if (grant_ != nullptr) {
            // Its size is the list's and the buttons' row together, its Confirm and Cancel on that row:
            // over them, not where its file has it (ours: the placing code is not traced).
            grant_->rect.x = 129.0f - 115.0f;
            grant_->rect.y = 287.0f - 144.0f;
            grant_->visible = false;
        }
        if (noticeView_ != nullptr) noticeView_->visible = false;
        if (hostile_ != nullptr) hostile_->visible = false;
        if (info_ != nullptr) {
            if (auto* bar = dynamic_cast<ui::ScrollBar*>(info_->Child(21))) bar->SetRange(0, 0);
        }
        ShowPage(0);
        community_ = AddCommunityPanel(ui, c_, *this);
    }

    // The two buttons at the window's left: the guild's pages, or the community's in
    // their place.
    void ShowCommunity(bool on) {
        showingCommunity_ = on;
        if (community_ != nullptr) community_->visible = on;
        for (int id : {72, 73, 74}) {
            if (ui::Widget* tab = Child(id)) tab->visible = !on;
        }
        if (on) {
            for (ui::Widget* page : {info_, war_, relations_}) {
                if (page != nullptr) page->visible = false;
            }
        } else {
            ShowPage(page_);
        }
        if (ui::Widget* title = Child(0)) title->SetText(c_.text(on ? "UIIT_STT_COMMUNITY" : "UIIT_STT_GUILDVIEW"));
    }
    bool ShowingCommunity() const { return showingCommunity_; }

    // 0 the guild's info, 1 its relations (the union), 2 its relations' hostile guilds (the wars).
    void ShowPage(int page) {
        if (info_ != nullptr) info_->visible = page == 0;
        if (war_ != nullptr) war_->visible = page == 3;
        if (page == 3 && page_ != 3) contributionSort_ = 0; // the war page shown: its list by name
        if (relations_ != nullptr) relations_->visible = page == 1 || page == 2;
        if (union_ != nullptr) union_->visible = page == 1;
        if (hostile_ != nullptr) hostile_->visible = page == 2;
        if (page != 0) {
            ShowGrant(false);
            ShowNotice(false);
        }
        page_ = page;
        static constexpr int kTabs[] = {72, 73, 74};
        const int tabPage = page == 2 ? 1 : page == 3 ? 2 : page; // the hostile guilds are the relations' second sub-page
        for (int i = 0; i < 3; ++i) {
            if (auto* tab = dynamic_cast<ui::Button*>(Child(kTabs[i]))) {
                tab->texture = i == tabPage ? Variant(tab->node().ddj, "_off", "_on") : std::string();
            }
        }
        if (relations_ != nullptr) {
            if (auto* tab = dynamic_cast<ui::Button*>(relations_->Child(87))) tab->texture = page == 1 ? Variant(tab->node().ddj, "_off", "_on") : std::string();
            if (auto* tab = dynamic_cast<ui::Button*>(relations_->Child(88))) tab->texture = page == 2 ? Variant(tab->node().ddj, "_off", "_on") : std::string();
        }
        seen_ = ~0ull;
    }

    void Update(ui::Frame& frame) override {
        MainFrame::Update(frame);
        const auto tip = [&](ui::Widget* w, const std::string& text) {
            if (w != nullptr && frame.pointer.hovered == w) frame.interface.ShowTooltip(text, w->ScreenRect());
        };
        // The words of the art-only buttons.
        tip(Child(72), c_.text("UIIT_STT_GUILD_INFO"));
        tip(Child(73), c_.text("UIIT_CTL_GUILD_RESPECT"));
        tip(Child(74), c_.text("UIIT_CTL_WARSIT_WARSITUATION"));
        tip(Child(87), c_.text("UIIT_CTL_WARSIT_GUILDWAR"));
        tip(Child(220), c_.text("UIIT_CTL_WK_GUILD"));
        tip(Child(221), c_.text("UIIT_STT_SILKMALL_COMMUNITY"));
        if (info_ != nullptr) tip(info_->Child(250), c_.text("UIIT_MSG_GUILD_SP_GP_TOOLTIP"));
        if (relations_ != nullptr) {
            tip(relations_->Child(87), c_.text("UIIT_STT_GUILD_RESPECT_ALLY"));
            tip(relations_->Child(88), c_.text("UIIT_STT_GUILD_RESPECT_WAR"));
        }
        // The union's head: how many may chat there, the master among them.
        if (grant_ != nullptr && grant_->visible) {
            tip(grant_->Child(93), FormatText(c_.text("UIIT_MSG_GUILD_UNION_CHAT_TOOLTIP"), {std::to_string(UnionChatters()), std::to_string(game::kUnionChatHolders)}));
        }
        if (page_ == 3) ShowWarTime();
        if (c_.guild == nullptr || c_.guild->Revision() == seen_) return;
        seen_ = c_.guild->Revision();
        warsAt_ = c_.time;
        Refresh();
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::ScrollChanged && n.control == 21) {
            scroll_ = n.value;
            Refresh();
            return true;
        }
        if (n.kind == ui::Notice::Kind::ScrollChanged && n.control == 97) {
            grantScroll_ = n.value;
            Refresh();
            return true;
        }
        if (n.kind == ui::Notice::Kind::ScrollChanged && n.source != nullptr && n.source->parent() == hostile_ && n.control == 61) {
            hostileScroll_ = n.value;
            Refresh();
            return true;
        }
        if (n.kind == ui::Notice::Kind::ScrollChanged && n.source != nullptr && n.source->parent() == war_ && (n.control == 246 || n.control == 256)) {
            (n.control == 246 ? warScroll_ : contributionScroll_) = n.value;
            Refresh();
            return true;
        }
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr) return MainFrame::OnNotice(n);
        const ui::Widget* parent = n.source->parent();
        const int parentId = parent != nullptr ? parent->id() : 0;
        if (parent == this) {
            if (n.control == 220) ShowCommunity(false);
            if (n.control == 221) ShowCommunity(true);
            if (n.control == 72) ShowPage(0);
            if (n.control == 73) ShowPage(1);
            if (n.control == 74) ShowPage(3);
            if (n.control == 2) return MainFrame::OnNotice(n);
            return true;
        }
        // A row's own children (its name, marks) belong to the row.
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i] != nullptr && (n.source == rows_[i] || parent == rows_[i])) {
                selected_ = scroll_ + int(i);
                Refresh();
                return true;
            }
        }
        for (std::size_t i = 0; i < unionRows_.size(); ++i) {
            if (unionRows_[i] != nullptr && (n.source == unionRows_[i] || parent == unionRows_[i])) {
                selectedUnion_ = int(i);
                Refresh();
                return true;
            }
        }
        for (std::size_t i = 0; i < grantRows_.size(); ++i) {
            if (grantRows_[i] != nullptr && parent == grantRows_[i] && n.control >= 105 && n.control <= 109) {
                const std::size_t member = std::size_t(grantScroll_) + i;
                if (member < grantDraft_.size()) {
                    const std::uint32_t bit = 1u << (n.control - 105);
                    // A tick past the union chat's places is undone.
                    if (bit == game::kGuildPermUnionChat && (grantDraft_[member].second & bit) == 0 && UnionChatters() >= game::kUnionChatHolders) {
                        if (c_.state != nullptr) c_.state->Post(game::GameMessage::Kind::General, c_.text("UIIT_MSG_GUILD_UNION_CHAT_FULL"), c_.time);
                        return true;
                    }
                    grantDraft_[member].second ^= bit;
                    Refresh();
                }
                return true;
            }
        }
        if (noticeLine_ != nullptr && parent == noticeLine_ && n.control == 81) {
            OpenGuildNoticeWrite(c_);
            return true;
        }
        if (noticeLine_ != nullptr && (n.source == noticeLine_ || parent == noticeLine_)) {
            ShowNotice(noticeView_ != nullptr && !noticeView_->visible);
            return true;
        }
        if (parent == relations_ && n.control == 87) {
            ShowPage(1);
            return true;
        }
        if (parent == relations_ && n.control == 88) {
            ShowPage(2);
            return true;
        }
        // The contribution list's heads: by name (248), by rank (247), by points (63).
        if (parent == war_ && (n.control == 248 || n.control == 247 || n.control == 63)) {
            contributionSort_ = n.control == 248 ? 0 : n.control == 247 ? 1 : 2;
            Refresh();
            return true;
        }
        for (std::size_t i = 0; i < warRows_.size(); ++i) {
            if (warRows_[i] != nullptr && (n.source == warRows_[i] || parent == warRows_[i])) {
                selectedWar_ = warScroll_ + int(i);
                Refresh();
                return true;
            }
        }
        if (parentId == 2000 && (n.control == 56 || n.control == 60)) {
            hostileSort_ = n.control;
            selectedHostile_ = -1;
            Refresh();
            return true;
        }
        if (parentId == 86 && (n.control == 56 || n.control == 60)) {
            unionSort_ = n.control;
            selectedUnion_ = -1;
            Refresh();
            return true;
        }
        if (parentId == 2000) return HostileClick(n);
        for (std::size_t i = 0; i < hostileRows_.size(); ++i) {
            if (hostileRows_[i] != nullptr && (n.source == hostileRows_[i] || parent == hostileRows_[i])) {
                selectedHostile_ = hostileScroll_ + int(i);
                Refresh();
                return true;
            }
        }
        if (parentId == 47) return InfoClick(n);
        if (parentId == 86) return UnionClick(n);
        if (parentId == 1000) return GrantClick(n);
        if (parentId == 1200 && n.control == 153) {
            ShowNotice(false);
            return true;
        }
        return true;
    }

private:
    void Set(ui::Widget* under, int id, const std::string& text, ui::Color colour = 0) {
        ui::Widget* w = under != nullptr ? under->Child(id) : nullptr;
        if (w == nullptr) return;
        w->SetText(text);
        if (auto* s = dynamic_cast<ui::Static*>(w); s != nullptr && colour != 0) s->textColor = colour;
    }
    void Art(ui::Widget* under, int id, const std::string& path) {
        if (auto* s = dynamic_cast<ui::Static*>(under != nullptr ? under->Child(id) : nullptr)) {
            s->texture = path;
            s->visible = !path.empty();
        }
    }
    void Enable(ui::Widget* under, int id, bool on) {
        if (ui::Widget* w = under != nullptr ? under->Child(id) : nullptr) w->enabled = on;
    }

    // What the first column says of a member: its name, its title, or its position.
    std::string FirstColumn(const proto::GuildMemberRecord& m) const {
        if (firstColumn_ == 1) return m.nickname;
        if (firstColumn_ == 2) {
            const char* key = game::GuildPositionKey(m.position);
            return key != nullptr ? c_.text(key) : std::string();
        }
        return m.name;
    }

    // The members as listed, by the head last pressed (each with its one direction;
    // ties by name are ours): the first column's text A-Z, the level and the points given highest first,
    // the rank from the master down (the first shown).
    std::vector<const proto::GuildMemberRecord*> Members() const {
        std::vector<const proto::GuildMemberRecord*> out;
        for (const auto& m : c_.guild->Guild().members) out.push_back(&m);
        std::stable_sort(out.begin(), out.end(), [this](const auto* a, const auto* b) {
            switch (sortColumn_) {
            case 400:
                if (const std::string x = FirstColumn(*a), y = FirstColumn(*b); x != y) return x < y;
                break;
            case 401:
                if (a->level != b->level) return a->level > b->level;
                break;
            case 403:
                if (a->donation != b->donation) return a->donation > b->donation;
                break;
            default:
                if (a->memberClass != b->memberClass) return a->memberClass < b->memberClass;
                break;
            }
            return a->name < b->name;
        });
        return out;
    }

    // The members holding the union chat, as the authority panel has it (the master is one).
    int UnionChatters() const {
        int n = 1;
        for (const auto& [character, bits] : grantDraft_) n += (bits & game::kGuildPermUnionChat) != 0 ? 1 : 0;
        return n;
    }

    // The notice's page over the list; the line's picture lit while it is open.
    void ShowNotice(bool on) {
        if (noticeView_ != nullptr) noticeView_->visible = on;
        if (auto* line = dynamic_cast<ui::Static*>(noticeLine_)) line->texture = on ? "interface\\guild\\gil_windo02_on.ddj" : "";
    }

    // The authority panel beside the window; the five buttons go while it is open.
    void ShowGrant(bool on) {
        if (grant_ == nullptr) return;
        grant_->visible = on;
        for (int id : {37, 38, 39, 40, 41, 21, 22, 23, 86, 400, 401, 402, 403}) {
            if (ui::Widget* b = info_ != nullptr ? info_->Child(id) : nullptr) b->visible = !on;
        }
        for (ui::Widget* row : rows_) {
            if (row != nullptr && on) row->visible = false;
        }
    }

    void Refresh() {
        if (c_.guild == nullptr) return;
        RefreshHostile();
        RefreshWar();
        const game::GuildView& view = *c_.guild;
        const proto::GuildInfo& g = view.Guild();
        const bool in = view.InGuild();
        // The info.
        Set(info_, 66, in ? g.name : c_.text("UIIT_STT_NO_GUILD"));
        Set(info_, 115, in ? "Level " + std::to_string(g.level) : std::string());
        const proto::GuildMemberRecord* master = view.Master();
        Set(info_, 124, master != nullptr ? master->name : std::string());
        Art(info_, 150, master != nullptr ? RaceMark(master->model) : std::string());
        Art(info_, 130, in ? std::string("interface\\guild\\gil_mark01.ddj") : std::string());
        Set(info_, 126, in ? std::to_string(g.members.size()) + " / " + std::to_string(game::GuildMemberCap(g.level)) : std::string());
        const std::uint32_t need = game::kGuildLevelUpPoints[std::size_t(std::clamp<int>(g.level, 1, game::kGuildMaxLevel))];
        Set(info_, 116, in ? std::to_string(g.points) + " / " + std::to_string(need) : std::string());
        const int percent = need > 0 ? int(std::min<std::uint64_t>(100, std::uint64_t(g.points) * 100 / need)) : 0;
        Set(info_, 121, in ? std::to_string(percent) + "%" : std::string());
        if (auto* gauge = dynamic_cast<ui::Gauge*>(info_ != nullptr ? info_->Child(120) : nullptr)) gauge->SetValue(in ? float(percent) / 100.0f : 0.0f, true);
        // The notice's title, or "You have no notes."
        if (ui::Widget* slot = info_ != nullptr ? info_->Child(222) : nullptr) {
            Set(slot, 117, !in ? std::string() : g.noticeTitle.empty() ? c_.text("UIIT_MSG_GUILD_COMMON_NOTEXIST") : g.noticeTitle);
            Enable(slot, 81, view.May(game::kGuildPermNotice));
        }
        if (noticeView_ != nullptr) {
            Set(noticeView_, 155, g.notice);
            if (!in && noticeView_->visible) ShowNotice(false);
        }
        Set(info_, 400, c_.text(kFirstColumnHeads[firstColumn_]));
        // The members, six at a time: the lamp, the race, the name, the level, the
        // rank and the points given; the master's text orange, those away dimmed.
        const auto members = Members();
        if (auto* bar = dynamic_cast<ui::ScrollBar*>(info_ != nullptr ? info_->Child(21) : nullptr)) {
            const int most = std::max(0, int(members.size()) - int(rows_.size()));
            if (scroll_ > most) scroll_ = most;
            bar->SetRange(0, most);
        }
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            ui::Widget* row = rows_[i];
            if (row == nullptr) continue;
            const std::size_t at = std::size_t(scroll_) + i;
            const proto::GuildMemberRecord* m = at < members.size() ? members[at] : nullptr;
            row->visible = m != nullptr && (grant_ == nullptr || !grant_->visible);
            if (m == nullptr) continue;
            const bool boss = m->memberClass == game::kGuildMaster;
            const ui::Color colour = boss ? ui::Argb(m->online ? 0xFFFFB541 : 0xFFA17233) : ui::Argb(m->online ? 0xFFFFFFFF : 0xFF808080);
            Art(row, 64, m->online ? "interface\\guild\\gil_contact_on.ddj" : "interface\\guild\\gil_contact_off.ddj");
            Art(row, 231, RaceMark(m->model));
            Set(row, 68, FirstColumn(*m), colour);
            Set(row, 69, std::to_string(m->level), colour);
            Set(row, 70, c_.text(boss ? "UIIT_STT_LEADER" : m->memberClass == game::kGuildViceMaster ? "UIIT_STT_GUILD_STAFF" : "UIIT_STT_GUILDSMAN"), colour);
            Set(row, 71, std::to_string(m->donation), colour);
            if (auto* bar = dynamic_cast<ui::Bar*>(row)) bar->pieces = int(at) == selected_ ? kSelectedBar : "";
        }
        // The buttons by authority.
        const bool isMaster = view.Self() != nullptr && view.Self()->memberClass == game::kGuildMaster;
        Enable(info_, 37, view.May(game::kGuildPermInvite));
        Enable(info_, 38, in);
        Set(info_, 38, c_.text(isMaster ? "UIIT_CTL_GUILD_UNION_CHAT_GRANT" : "UIIT_CTL_GUILD_UNION_CHAT_CONFIRM"));
        Enable(info_, 39, view.May(game::kGuildPermKick));
        Enable(info_, 40, isMaster && g.level >= game::kGuildNicknameLevel);
        Enable(info_, 41, isMaster);
        Enable(info_, 250, in);
        RefreshGrant(isMaster);
        RefreshUnion(isMaster);
    }

    void RefreshGrant(bool isMaster) {
        if (grant_ == nullptr) return;
        if (auto* bar = dynamic_cast<ui::ScrollBar*>(grant_->Child(97))) {
            const int most = std::max(0, int(grantDraft_.size()) - int(grantRows_.size()));
            grantScroll_ = std::min(grantScroll_, most);
            bar->SetRange(0, most);
        }
        for (std::size_t i = 0; i < grantRows_.size(); ++i) {
            ui::Widget* row = grantRows_[i];
            if (row == nullptr) continue;
            const std::size_t at = std::size_t(grantScroll_) + i;
            row->visible = at < grantDraft_.size();
            if (!row->visible) continue;
            const proto::GuildMemberRecord* m = c_.guild->Member(grantDraft_[at].first);
            Set(row, 68, m != nullptr ? m->name : std::string());
            Art(row, 231, m != nullptr ? RaceMark(m->model) : std::string());
            for (int bit = 0; bit < 5; ++bit) {
                if (auto* box = dynamic_cast<ui::Button*>(row->Child(105 + bit))) {
                    box->texture = Variant(box->node().ddj, "_on", (grantDraft_[at].second & (1u << bit)) != 0 ? "_on" : "_off");
                    box->enabled = isMaster;
                }
            }
        }
        Enable(grant_, 151, isMaster);
    }

    void RefreshUnion(bool isMaster) {
        if (union_ == nullptr) return;
        const proto::UnionInfo& u = c_.guild->Union();
        const bool in = u.id != 0 && !u.guilds.empty();
        const proto::UnionGuildRecord* lead = in ? &u.guilds.front() : nullptr;
        Set(union_, 158, lead != nullptr ? lead->name : std::string());
        Art(union_, 178, lead != nullptr ? std::string("interface\\guild\\gil_mark01.ddj") : std::string());
        Set(union_, 179, lead != nullptr ? lead->master : std::string());
        Art(union_, 149, lead != nullptr ? RaceMark(lead->masterModel) : std::string());
        Set(union_, 151, in ? std::to_string(u.guilds.size()) + " / " + std::to_string(game::kUnionMaxGuilds) : std::string());
        if (ui::Widget* none = union_->Child(137)) none->visible = !in;
        for (int id : {174, 175, 176, 177}) {
            if (ui::Widget* w = union_->Child(id)) w->visible = in;
        }
        if (selectedUnion_ >= int(u.guilds.size())) selectedUnion_ = -1;
        const auto listed = UnionRows();
        const proto::UnionGuildRecord* picked = selectedUnion_ >= 0 ? listed[std::size_t(selectedUnion_)] : nullptr;
        Set(union_, 155, picked != nullptr ? picked->name : std::string());
        Art(union_, 161, picked != nullptr ? std::string("interface\\guild\\gil_mark01.ddj") : std::string());
        Set(union_, 148, picked != nullptr ? picked->master : std::string());
        Art(union_, 162, picked != nullptr ? RaceMark(picked->masterModel) : std::string());
        Set(union_, 160, picked != nullptr ? std::to_string(picked->members) + " / " + std::to_string(game::GuildMemberCap(picked->level)) : std::string());
        if (ui::Widget* none = union_->Child(136)) none->visible = picked == nullptr;
        for (int id : {152, 153, 156, 159}) {
            if (ui::Widget* w = union_->Child(id)) w->visible = picked != nullptr;
        }
        for (std::size_t i = 0; i < unionRows_.size(); ++i) {
            ui::Widget* row = unionRows_[i];
            if (row == nullptr) continue;
            row->visible = i < listed.size();
            if (!row->visible) continue;
            Set(row, 127, listed[i]->name);
            Set(row, 128, std::to_string(listed[i]->level));
            if (auto* bar = dynamic_cast<ui::Bar*>(row)) bar->pieces = int(i) == selectedUnion_ ? kSelectedBar : "";
        }
        const bool leads = in && c_.guild->Guild().id == lead->id;
        Enable(union_, 51, isMaster && (!in || leads));
        Enable(union_, 52, isMaster && in);
        Enable(union_, 53, isMaster && leads && picked != nullptr && picked->id != lead->id);
    }

    template <typename T>
    void Send(const T& message) {
        if (c_.guildRequest) c_.guildRequest(T::kId, proto::Encode(message));
    }
    std::uint32_t TargetPlayer() const {
        return c_.state != nullptr && c_.state->target.kind == game::TargetStatus::Kind::Player ? c_.state->target.entity : 0;
    }

    bool InfoClick(const ui::Notice& n) {
        switch (n.control) {
        case 37: // invite the selected player (0x70F3)
            if (const std::uint32_t target = TargetPlayer()) Send(proto::GuildInvite{target});
            return true;
        case 38: { // the authority panel: the members but the master, by name
            if (grant_ == nullptr) return true;
            std::vector<const proto::GuildMemberRecord*> members;
            for (const auto& m : c_.guild->Guild().members) {
                if (m.memberClass != game::kGuildMaster) members.push_back(&m);
            }
            std::sort(members.begin(), members.end(), [](const auto* a, const auto* b) { return a->name < b->name; });
            grantDraft_.clear();
            for (const auto* m : members) grantDraft_.emplace_back(m->character, m->permission);
            grantScroll_ = 0;
            ShowGrant(true);
            Refresh();
            return true;
        }
        case 23: // what the first column shows
            firstColumn_ = (firstColumn_ + 1) % 3;
            Refresh();
            return true;
        case 400:
        case 401:
        case 402:
        case 403:
            sortColumn_ = n.control;
            selected_ = -1;
            Refresh();
            return true;
        case 250: OpenGuildDonate(c_); return true;
        case 41: { // a fortress position for the selected member
            const auto members = Members();
            if (selected_ < 0 || selected_ >= int(members.size())) return true;
            OpenGuildPosition(c_, members[std::size_t(selected_)]->character);
            return true;
        }
        case 39: { // put out the selected member, asked first
            const auto members = Members();
            if (selected_ < 0 || selected_ >= int(members.size()) || c_.hud == nullptr) return true;
            const proto::GuildMemberRecord* m = members[std::size_t(selected_)];
            const game::GuildView& view = *c_.guild;
            if (m->memberClass == game::kGuildMaster || (view.Self() != nullptr && view.Self()->character == m->character)) {
                if (c_.state != nullptr) c_.state->Post(game::GameMessage::Kind::General, c_.text("UIIT_MSG_GUILD_EXPULSION_NOTEXP"), c_.time);
                return true;
            }
            const std::string name = m->name;
            c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {FormatText(c_.text("UIIT_MSG_GUILD_QUESTION_EXPULSION_ANYONE"), {name})},
                            [this, name](bool yes) {
                                if (yes) Send(proto::GuildKick{name});
                            });
            return true;
        }
        case 40: { // Grant name: a title for the selected member (the master, level 4; 0x7256)
            const auto members = Members();
            if (selected_ < 0 || selected_ >= int(members.size())) return true;
            const proto::GuildMemberRecord* m = members[std::size_t(selected_)];
            AskGuildTitle(c_, m->character, m->name);
            return true;
        }
        default: return true;
        }
    }

    // The union's guilds and the hostile guilds, by the head last pressed: the name
    // A-Z, the level highest first; as told until one is pressed.
    template <typename T>
    std::vector<const T*> SortedRows(const std::vector<T>& rows, int head) const {
        std::vector<const T*> out;
        for (const T& r : rows) out.push_back(&r);
        if (head != 0) {
            std::stable_sort(out.begin(), out.end(), [head](const T* a, const T* b) { return head == 56 ? a->name < b->name : a->level > b->level; });
        }
        return out;
    }
    std::vector<const proto::UnionGuildRecord*> UnionRows() const { return SortedRows(c_.guild->Union().guilds, unionSort_); }

    // The war status page (guild_w.2dt): the enemies, the selected one's war, and the guild's
    // members with their war points (Contribution) and their rank by them: the most points first, equal
    // points sharing a rank, the next points the next rank (1, 1, 2, ...). The list sorted by the head
    // pressed: the name, the rank, the points (highest first); by the name when the page is shown.
    void RefreshWar() {
        if (war_ == nullptr || c_.guild == nullptr) return;
        const auto wars = SortedRows(c_.guild->Wars().wars, 0);
        if (selectedWar_ >= int(wars.size())) selectedWar_ = -1;
        if (selectedWar_ < 0 && !wars.empty()) selectedWar_ = 0;
        const int most = std::max(0, int(wars.size()) - int(warRows_.size()));
        warScroll_ = std::min(warScroll_, most);
        if (auto* bar = dynamic_cast<ui::ScrollBar*>(war_->Child(246))) bar->SetRange(0, most);
        for (std::size_t i = 0; i < warRows_.size(); ++i) {
            ui::Widget* row = warRows_[i];
            if (row == nullptr) continue;
            const std::size_t at = std::size_t(warScroll_) + i;
            row->visible = at < wars.size();
            if (!row->visible) continue;
            Set(row, 518, wars[at]->name);
            if (auto* b = dynamic_cast<ui::Bar*>(row)) b->pieces = int(at) == selectedWar_ ? kSelectedBar : "";
        }
        const proto::GuildWarRecord* w = selectedWar_ >= 0 ? wars[std::size_t(selectedWar_)] : nullptr;
        Set(war_, 541, w != nullptr ? w->name : std::string());
        Set(war_, 542, w != nullptr ? std::to_string(w->ours) : std::string());
        Set(war_, 543, w != nullptr ? std::to_string(w->theirs) : std::string());
        std::string victory;
        if (w != nullptr) {
            const std::uint32_t goal = game::kGuildWarVictoryPoints[std::min<std::size_t>(w->terms.victory, game::kGuildWarVictoryPoints.size() - 1)];
            victory = goal == 0 ? c_.text("UIIT_CTL_GUILDWAR_UNLIMITED") : std::to_string(goal);
        }
        Set(war_, 544, victory);
        Set(war_, 545, w != nullptr ? FormatText("%s %s", {std::to_string(w->terms.wager), c_.text("UIIT_STT_GOLD")}) : std::string());
        if (ui::Widget* none = war_->Child(62)) none->visible = w == nullptr;
        for (int id : {534, 535, 536, 537, 538, 539, 540, 547}) {
            if (ui::Widget* label = war_->Child(id)) label->visible = w != nullptr;
        }
        ShowWarTime();
        struct Ranked {
            const proto::GuildMemberRecord* member;
            int rank;
        };
        std::vector<Ranked> ranked;
        for (const auto& m : c_.guild->Guild().members) ranked.push_back({&m, 0});
        std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.member->contribution > b.member->contribution; });
        for (std::size_t i = 0; i < ranked.size(); ++i) {
            ranked[i].rank = i == 0 ? 1 : ranked[i - 1].rank + (ranked[i].member->contribution != ranked[i - 1].member->contribution ? 1 : 0);
        }
        if (contributionSort_ == 0) {
            std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.member->name < b.member->name; });
        }
        const int mostRanked = std::max(0, int(ranked.size()) - int(contributionRows_.size()));
        contributionScroll_ = std::min(contributionScroll_, mostRanked);
        if (auto* bar = dynamic_cast<ui::ScrollBar*>(war_->Child(256))) bar->SetRange(0, mostRanked);
        for (std::size_t i = 0; i < contributionRows_.size(); ++i) {
            ui::Widget* row = contributionRows_[i];
            if (row == nullptr) continue;
            const std::size_t at = std::size_t(contributionScroll_) + i;
            row->visible = at < ranked.size();
            if (!row->visible) continue;
            Set(row, 503, ranked[at].member->name);
            Set(row, 504, std::to_string(ranked[at].rank));
            Set(row, 505, std::to_string(ranked[at].member->contribution));
        }
    }

    // What is left of the selected war, running on from when the server last told it (546).
    void ShowWarTime() {
        if (war_ == nullptr || c_.guild == nullptr) return;
        const auto wars = SortedRows(c_.guild->Wars().wars, 0);
        const proto::GuildWarRecord* w = selectedWar_ >= 0 && selectedWar_ < int(wars.size()) ? wars[std::size_t(selectedWar_)] : nullptr;
        std::string text;
        if (w != nullptr && w->secondsLeft == 0xFFFFFFFFu) {
            text = c_.text("UIIT_CTL_GUILDWAR_UNLIMITED");
        } else if (w != nullptr) {
            const std::int64_t left = std::max<std::int64_t>(0, std::int64_t(w->secondsLeft) - std::int64_t(c_.time - warsAt_));
            text = std::format("{:02} : {:02} : {:02}", left / 3600, left / 60 % 60, left % 60);
        }
        if (ui::Widget* time = war_->Child(546); time != nullptr && time->text() != text) time->SetText(text);
    }

    bool UnionClick(const ui::Notice& n) {
        const proto::UnionInfo& u = c_.guild->Union();
        switch (n.control) {
        case 51: // propose a union to the selected player's guild (0x70FB)
            if (const std::uint32_t target = TargetPlayer()) Send(proto::UnionInvite{target});
            return true;
        case 52:
            if (c_.hud != nullptr) {
                c_.hud->Confirm("UIIT_STT_GUILD_RESPECT_ALLY_EXIT", {c_.text("UIIT_MSG_GUILD_QUESTION_ALLY_EXIT")}, [this](bool yes) {
                    if (yes) Send(proto::UnionLeave{});
                });
            }
            return true;
        case 53:
            if (c_.hud != nullptr && selectedUnion_ >= 0 && selectedUnion_ < int(u.guilds.size())) {
                const proto::UnionGuildRecord g = *UnionRows()[std::size_t(selectedUnion_)];
                c_.hud->Confirm("UIIT_STT_GUILD_RESPECT_ALLY_EXPULSION", {FormatText(c_.text("UIIT_MSG_QUESTION_GUILD_RESPECT_ALLY_EXPEL"), {g.name})},
                                [this, id = g.id](bool yes) {
                                    if (yes) Send(proto::UnionKick{id});
                                });
            }
            return true;
        default: return true;
        }
    }

    bool GrantClick(const ui::Notice& n) {
        if (n.control == 152) {
            ShowGrant(false);
            return true;
        }
        if (n.control != 151) return true;
        // Only the members whose authority changed.
        proto::GuildPermissions changed;
        for (const auto& [character, permission] : grantDraft_) {
            const proto::GuildMemberRecord* m = c_.guild->Member(character);
            if (m != nullptr && m->memberClass != game::kGuildMaster && m->permission != permission) changed.members.push_back({character, permission});
        }
        if (!changed.members.empty()) Send(changed);
        ShowGrant(false);
        return true;
    }

    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    ui::Widget *info_ = nullptr, *relations_ = nullptr, *union_ = nullptr, *hostile_ = nullptr, *grant_ = nullptr, *noticeView_ = nullptr, *noticeLine_ = nullptr;
    int sortColumn_ = 402, firstColumn_ = 0; // the list by its rank, names shown
    std::vector<ui::Widget*> rows_, unionRows_, grantRows_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> grantDraft_; // the authority panel's members and their bits
    int page_ = 0, scroll_ = 0, selected_ = -1, selectedUnion_ = -1, grantScroll_ = 0;
    std::vector<ui::Widget*> hostileRows_, warRows_, contributionRows_;
    ui::Widget* war_ = nullptr;
    ui::Widget* community_ = nullptr;
    bool showingCommunity_ = false;
    int hostileScroll_ = 0, selectedHostile_ = -1, warScroll_ = 0, selectedWar_ = -1, contributionScroll_ = 0;
    int unionSort_ = 0, hostileSort_ = 0; // the head last pressed (0: as the server listed them)
    int contributionSort_ = 0;            // the war page's list: 0 by name, 1 by rank, 2 by points
    double warsAt_ = 0.0;                 // when the wars' times were last told

    // The hostile guilds (guild_r.2dt's page 2000): ten rows of the wars' enemies (the name 127, the
    // level 128), the selected one's details (155 its name, 148 its master, 160 its members; 135 "No
    // guild is selected."), "Request to start the war" (52) and "Request to end the war" (53), the master's.
    void RefreshHostile() {
        if (hostile_ == nullptr || c_.guild == nullptr) return;
        const auto wars = SortedRows(c_.guild->Wars().wars, hostileSort_);
        if (selectedHostile_ >= int(wars.size())) selectedHostile_ = -1;
        const int most = std::max(0, int(wars.size()) - int(hostileRows_.size()));
        hostileScroll_ = std::min(hostileScroll_, most);
        if (auto* bar = dynamic_cast<ui::ScrollBar*>(hostile_->Child(61))) bar->SetRange(0, most);
        for (std::size_t i = 0; i < hostileRows_.size(); ++i) {
            ui::Widget* row = hostileRows_[i];
            if (row == nullptr) continue;
            const std::size_t at = std::size_t(hostileScroll_) + i;
            const bool shown = at < wars.size();
            for (int id : {127, 128, 131}) {
                if (ui::Widget* w = row->Child(id)) w->visible = shown;
            }
            if (!shown) continue;
            Set(row, 127, wars[at]->name);
            Set(row, 128, std::to_string(wars[at]->level));
            if (auto* bar = dynamic_cast<ui::Bar*>(row)) bar->pieces = int(at) == selectedHostile_ ? kSelectedBar : "";
        }
        const proto::GuildWarRecord* w = selectedHostile_ >= 0 ? wars[std::size_t(selectedHostile_)] : nullptr;
        Set(hostile_, 155, w != nullptr ? w->name : std::string());
        Set(hostile_, 148, w != nullptr ? w->master : std::string());
        Set(hostile_, 160, w != nullptr ? std::to_string(w->members) : std::string());
        if (ui::Widget* none = hostile_->Child(135)) none->visible = w == nullptr;
        for (int id : {152, 153, 156, 159}) {
            if (ui::Widget* label = hostile_->Child(id)) label->visible = w != nullptr;
        }
        if (ui::Widget* mark = hostile_->Child(161)) mark->visible = w != nullptr;
        const bool master = c_.guild->Self() != nullptr && c_.guild->Self()->memberClass == game::kGuildMaster;
        Enable(hostile_, 52, master);
        Enable(hostile_, 53, master && w != nullptr && w->ending == 0);
    }

    bool HostileClick(const ui::Notice& n) {
        if (n.control == 52) {
            OpenGuildWarRequest(c_);
            return true;
        }
        if (n.control == 53 && c_.guild != nullptr && selectedHostile_ >= 0 && selectedHostile_ < int(c_.guild->Wars().wars.size())) {
            // "Propose [%s]guild to end the war." / "Do you propose to end the war?"
            const proto::GuildWarRecord w = *SortedRows(c_.guild->Wars().wars, hostileSort_)[std::size_t(selectedHostile_)];
            HudContext* ctx = &c_;
            c_.hud->Confirm("UIIT_STT_CONFIRM_BOX",
                            {FormatText(c_.text("UIIT_MSG_GUILDWAR_SANCTION_ENDWAR_01"), {w.name}), c_.text("UIIT_MSG_GUILDWAR_SANCTION_ENDWAR_02")},
                            [ctx, guild = w.guild](bool yes) {
                                if (yes && ctx->guildRequest) ctx->guildRequest(proto::GuildWarEnd::kId, proto::Encode(proto::GuildWarEnd{guild}));
                            });
            return true;
        }
        return true;
    }
    std::uint64_t seen_ = ~0ull;
};

void RegisterGuildWindows(ui::Interface& ui, HudContext& context) {
    // The community key's window: the guild window in 1.188 (its root's size, frame and title).
    ui.Register("CIFCommunity", [c = &context](const assets::UiLayoutNode& n) -> std::unique_ptr<ui::Widget> {
        assets::UiLayoutNode node = n;
        node.type = "CNIFGuildWnd";
        node.text = "UIIT_STT_GUILDVIEW";
        if (const auto* main = c->layout2dt ? c->layout2dt("res_ui\\guild.2dt") : nullptr; main != nullptr && !main->empty()) {
            node.ddj = main->front().frame;
            node.rect.w = main->front().rect.w;
            node.rect.h = main->front().rect.h;
        }
        return std::make_unique<GuildWindow>(node, *c);
    });
    ui.Register("CNIFGuildRow", [](const assets::UiLayoutNode& n) { return std::make_unique<GuildRow>(n); });
    ui.Register("CNIFGuildLine", [](const assets::UiLayoutNode& n) { return std::make_unique<GuildLine>(n); });
    RegisterGuildBoxes(ui);
    RegisterCommunityWindows(ui, context);
}

void ShowGuildPage(ui::Widget* window, int page) {
    auto* w = dynamic_cast<GuildWindow*>(window);
    if (w == nullptr) return;
    if (page >= 10) {
        w->ShowCommunity(true);
        ShowCommunityTab(window, page - 10);
    } else {
        w->ShowCommunity(false);
        w->ShowPage(page);
    }
}

void ShowGuildSide(HudContext& context, bool community) {
    if (context.hud == nullptr) return;
    if (auto* window = dynamic_cast<GuildWindow*>(context.hud->Show(hud::kCommunity))) window->ShowCommunity(community);
}

void ToggleCommunityPage(HudContext& context, int tab) {
    if (context.hud == nullptr) return;
    auto* shown = dynamic_cast<GuildWindow*>(context.hud->Shown(hud::kCommunity) ? context.hud->Show(hud::kCommunity) : nullptr);
    if (shown != nullptr && shown->ShowingCommunity() && tab < 0) {
        context.hud->Hide(hud::kCommunity);
        return;
    }
    auto* window = dynamic_cast<GuildWindow*>(context.hud->Show(hud::kCommunity));
    if (window == nullptr) return;
    window->ShowCommunity(true);
    if (tab >= 0) ShowCommunityTab(window, tab);
}

namespace {

// The input box (ifmessagebox.txt's MsgBoxInsertMsg: the prompt across the top, the edit in its
// frame, Confirm and Cancel; "Input window", kind 2): Enter confirms, Escape cancels.
class NameBox : public ui::MainFrame {
public:
    using MainFrame::MainFrame;
    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
        // The message box's dark tile (ifmessagebox.txt's Create section, GDR_MSGBOX_BG), as the box's
        // catalog class has it; ours: stretched to this box's size.
        {
            assets::UiLayoutNode tile;
            tile.type = "CIFNormalTile";
            tile.name = "GDR_MSGBOX_BG";
            tile.ddj = "interface\\ifcommon\\bg_tile\\com_bg_tile_b.ddj";
            tile.rect = {16.0f, 40.0f, rect.w - 32.0f, rect.h - 56.0f};
            Add(ui.Make(tile));
        }
        if (const assets::UiLayout* layout = ui.Layout("resinfo\\ifmessagebox.txt")) {
            for (const auto& s : layout->sections) {
                if (s.name != "MsgBoxInsertMsg") continue;
                for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) Add(ui.Make(*it));
            }
        }
        if (auto* edit = ui::As<ui::Edit>(Child(15))) {
            edit->rect = {15.0f, 70.0f, 388.0f, 20.0f}; // inside its frame (ours: the code's size is not traced)
            edit->passKeys = true;
            ui.Focus(edit);
        }
    }
    void Ask(const std::string& prompt, std::size_t limit, std::function<void(std::string)> answer, const std::string& initial = {}) {
        if (ui::Widget* title = Child(10)) title->SetText(prompt);
        if (auto* edit = ui::As<ui::Edit>(Child(15))) {
            edit->limit = limit;
            if (!initial.empty()) edit->SetValue(initial);
        }
        answer_ = std::move(answer);
    }
    bool OnNotice(const ui::Notice& n) override {
        const bool ok = (n.kind == ui::Notice::Kind::Clicked && n.control == 205) || n.kind == ui::Notice::Kind::Submitted;
        const bool cancel = (n.kind == ui::Notice::Kind::Clicked && (n.control == 206 || n.control == 2)) || n.kind == ui::Notice::Kind::Cancelled;
        if (!ok && !cancel) return MainFrame::OnNotice(n);
        std::string value;
        if (const ui::Widget* edit = Child(15)) value = edit->text();
        auto answer = std::move(answer_);
        if (ui_ != nullptr) ui_->Close(this);
        if (ok && answer && !value.empty()) answer(value);
        return true;
    }

private:
    ui::Interface* ui_ = nullptr;
    std::function<void(std::string)> answer_;
};

enum GuildNpcRow { kRowCreate = 1, kRowLevelUp = 2, kRowDisband = 3, kRowLeave = 4, kRowStorage = 5, kRowTransfer = 6, kRowElection = 7, kRowVote = 8, kRowWarReward = 9, kRowGpHistory = 10 };

} // namespace

bool AnswerGuildNameBox(ui::Interface& ui, const std::string& name) {
    for (ui::Widget* w : ui.Windows()) {
        auto* box = dynamic_cast<NameBox*>(w);
        if (box == nullptr || !box->visible) continue;
        if (auto* edit = ui::As<ui::Edit>(box->Child(15))) edit->SetValue(name);
        box->OnNotice({ui::Notice::Kind::Clicked, 205, box, 0});
        return true;
    }
    return false;
}

void AskGuildText(HudContext& c, const std::string& prompt, std::size_t limit, std::function<void(std::string)> answer, const std::string& initial) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    assets::UiLayoutNode node;
    node.type = "CNIFGuildNameBox";
    node.name = "GDR_MSGBOX_INSERT";
    node.ddj = "interface\\messagebox\\msgbox2_window_";
    node.text = "UIIT_STT_INPUT_BOX";
    node.rect = {0.0f, 0.0f, 420.0f, 150.0f};
    auto* box = dynamic_cast<NameBox*>(ui->Open(node));
    if (box == nullptr) return;
    ui->Centre(*box, ui->screen());
    box->Ask(prompt, limit, std::move(answer), initial);
}

void AskGuildTitle(HudContext& c, std::uint32_t character, const std::string& name) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    assets::UiLayoutNode node;
    node.type = "CNIFGuildNameBox";
    node.name = "GDR_MSGBOX_INSERT";
    node.ddj = "interface\\messagebox\\msgbox2_window_";
    node.text = "UIIT_STT_INPUT_BOX";
    node.rect = {0.0f, 0.0f, 420.0f, 150.0f};
    auto* box = dynamic_cast<NameBox*>(ui->Open(node));
    if (box == nullptr) return;
    ui->Centre(*box, ui->screen());
    HudContext* ctx = &c;
    // "Enter the title to address [%s]", the letters of the name rules (else the special letters'
    // line, 0x4C4D), then "Will you address [%s] as [%s]?" (1.188's texts).
    box->Ask(FormatText(c.text("UIIT_MSG_GUILD_NAME_GRANT_INPUT"), {name}), game::kGuildNicknameMax, [ctx, character, name](std::string title) {
        if (ctx->words != nullptr && ctx->words->Check(title) != game::NameCheck::Ok) {
            if (ctx->state != nullptr) ctx->state->Post(game::GameMessage::Kind::General, GuildErrorText(*ctx, 0x4C4D, 0), ctx->time);
            return;
        }
        ctx->hud->Confirm("UIIT_STT_CONFIRM_BOX", {FormatText(ctx->text("UIIT_MSG_GUILD_QUESTION_NAME_GRANT"), {name, title})},
                          [ctx, character, title](bool yes) {
                              if (yes && ctx->guildRequest) ctx->guildRequest(proto::GuildNickname::kId, proto::Encode(proto::GuildNickname{character, title}));
                          });
    });
}

void RegisterGuildBoxes(ui::Interface& ui) {
    ui.Register("CNIFGuildNameBox", [](const assets::UiLayoutNode& n) -> std::unique_ptr<ui::Widget> { return std::make_unique<NameBox>(n); });
}

// The price's box holds two long lines: taller, its text with it and its buttons lower (ours: the
// original's box 0x12 is sized in its code, not traced).
void GrowConfirm(ConfirmBox& box, float extra) {
    box.rect.h += extra;
    for (const auto& child : box.children()) {
        if (child->id() == 251) {
            child->rect.h += extra;
        } else if (child->rect.y > 60.0f) {
            child->rect.y += extra;
        } else if (child->rect.y > 30.0f && child->rect.h > 60.0f) {
            child->rect.h += extra; // the dark tile under the text
        }
    }
}

void GrowConfirm(ui::Interface& ui, float extra) {
    const auto windows = ui.Windows();
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        auto* box = dynamic_cast<ConfirmBox*>(*it);
        if (box == nullptr || !box->visible) continue;
        GrowConfirm(*box, extra);
        return;
    }
}

ConfirmBox* OpenCrestBox(HudContext& c, bool ofGuild, bool complete, std::function<void(bool yes)> answer) {
    if (c.hud == nullptr || !c.text) return nullptr;
    std::vector<std::string> lines;
    if (complete) {
        // "Registering guild emblem successful." and the notice that it may take a few minutes to show.
        lines = {c.text(ofGuild ? "UIIT_MSG_GUILD_CREST_REGISTER_COMPLETE" : "UIIT_MSG_UNION_CREST_REGISTER_COMPLETE"), c.text("UIIT_MSG_CREST_REGISTER_INFORMATION_01"),
                 c.text("UIIT_MSG_CREST_REGISTER_INFORMATION_02"), c.text("UIIT_MSG_CREST_REGISTER_INFORMATION_03"), c.text("UIIT_MSG_CREST_REGISTER_INFORMATION_04")};
    } else if (ofGuild) {
        // the format asked, the file's size and kind, where it is, then the question.
        lines = {c.text("UIIT_MSG_GUILD_USER_CREST"), c.text("UIIT_MSG_GUILD_USER_CREST_FILE"), c.text("UIIT_MSG_GUILD_USER_CREST_FOLDER"),
                 c.text("UIIT_MSG_GUILD_CREST_REGISTER")};
    } else {
        // the union's: the format, where it is, "Register the union emblem.", then the question (no size line).
        lines = {c.text("UIIT_MSG_UNION_USER_CREST"), c.text("UIIT_MSG_UNION_USER_CREST_FOLDER"), c.text("UIIT_CTL_UNION_CREST_REGISTER"),
                 c.text("UIIT_MSG_UNION_CREST_REGISTER")};
    }
    ConfirmBox* box = complete ? c.hud->Inform("UIIT_STT_CONFIRM_BOX", lines, [answer = std::move(answer)] { if (answer) answer(true); })
                               : c.hud->Confirm("UIIT_STT_CONFIRM_BOX", lines, std::move(answer));
    // taller for their lines (ours: the original's boxes are wider, 359 pixels of text; ours wraps the success's five
    // lines into eight).
    if (box != nullptr) GrowConfirm(*box, complete ? 84.0f : 32.0f);
    return box;
}

std::vector<GuildNpcRowInfo> GuildNpcRows(const HudContext& c) {
    // The rows and their order,
    std::vector<GuildNpcRowInfo> rows;
    if (c.guild == nullptr || !c.text) return rows;
    const auto add = [&](const char* key, int choice, bool red = false) { rows.push_back({c.text(key), choice, red}); };
    if (!c.guild->InGuild()) {
        add("UIIT_CTL_GUILD_CREATE", kRowCreate);
        return rows;
    }
    if (c.guild->Self() != nullptr && c.guild->Self()->memberClass == game::kGuildMaster) {
        add("UIIT_MSG_GUILD_LEVEL_UP", kRowLevelUp);
        add("UIIT_CTL_GUILD_BREAK", kRowDisband);
        add("UIIT_CTL_MASTERLEAVE", kRowTransfer);
        add("UIIT_CTL_GUILD_COMPENSATION", kRowWarReward);
    } else {
        add("UIIT_CTL_GUILD_SECESSION", kRowLeave);
    }
    // The election, the master's row too: called, or joined while one runs.
    if (c.guild->Election().active != 0) {
        add("UIIT_CTL_MRELEASE_VOTE", kRowVote);
    } else {
        add("UIIT_CTL_MASTERRELEASE", kRowElection);
    }
    add("UIIT_CTL_GUILD_WAREHOUSE", kRowStorage, c.guild->Guild().level < 2); // red below level 2
    add("UIIT_STT_GUILD_GP_HISTORY", kRowGpHistory);
    // The mercenaries' abilities (a shard that runs them; the master's: ours, the original's row 0x29, its NPC not traced),
    // red below level 4.
    if (c.guild->Guild().mercenaries && c.guild->Self() != nullptr && c.guild->Self()->memberClass == game::kGuildMaster) {
        add("UIIT_CTL_GUILD_SOLDIER", kGuildRowSoldier, c.guild->Guild().level < game::kMercAbilityGuildLevel);
    }
    return rows;
}

void GuildNpcChoice(ui::Interface& ui, HudContext& c, int choice, std::uint32_t npc) {
    if (c.hud == nullptr || !c.guildRequest) return;
    HudContext* ctx = &c;
    const auto send = [ctx](auto message) { ctx->guildRequest(decltype(message)::kId, proto::Encode(message)); };
    c.guildNpc = npc;
    switch (choice) {
    case kRowCreate: {
        // The name (2-12; the client's own check is the name rules' letters, 0x4C19), then the name
        // and the price confirmed (boxes 0x11 and 0x12), then 0x70F0 with the NPC.
        assets::UiLayoutNode node;
        node.type = "CNIFGuildNameBox";
        node.name = "GDR_MSGBOX_INSERT";
        node.ddj = "interface\\messagebox\\msgbox2_window_";
        node.text = "UIIT_STT_INPUT_BOX";
        node.rect = {0.0f, 0.0f, 420.0f, 150.0f};
        auto* box = dynamic_cast<NameBox*>(ui.Open(node));
        if (box == nullptr) return;
        ui.Centre(*box, ui.screen());
        ui::Interface* uip = &ui;
        box->Ask(c.text("UIIT_MSG_GUILD_NAME_INPUT"), game::kGuildNameMax, [ctx, npc, send, uip](std::string name) {
            if (!game::GuildNameLengthOk(name)) {
                if (ctx->state != nullptr) ctx->state->Post(game::GameMessage::Kind::General, GuildErrorText(*ctx, 0x4C18, 0), ctx->time);
                return;
            }
            if (ctx->words != nullptr && ctx->words->Check(name) != game::NameCheck::Ok) {
                if (ctx->state != nullptr) ctx->state->Post(game::GameMessage::Kind::General, GuildErrorText(*ctx, 0x4C19, 0), ctx->time);
                return;
            }
            ctx->hud->Confirm("UIIT_STT_CONFIRM_BOX", {FormatText(ctx->text("UIIT_MSG_GUILD_QUESTION_NAME_CONFIRM"), {name})}, [ctx, npc, send, name, uip](bool yes) {
                if (!yes) return;
                ctx->hud->Confirm("UIIT_STT_CONFIRM_BOX",
                                  {FormatText(ctx->text("UIIT_MSG_GUILD_QUESTION_CREATE"), {name}), ctx->text("UIIT_MSG_GUILD_QUESTION_CREATE_CONFIRM")},
                                  [npc, send, name](bool sure) {
                                      if (sure) send(proto::GuildCreate{npc, name});
                                  });
                GrowConfirm(*uip, 36.0f);
            });
        });
        return;
    }
    case kRowLevelUp: OpenGuildLevelUp(c); return; // its OK sends the request
    case kRowStorage: c.hud->Request({HudRequest::Kind::GuildStorage, int(npc)}); return;
    case kRowDisband:
        c.hud->Confirm("UIIT_STT_AGREEMENT_BOX", {c.text("UIIT_MSG_GUILD_BREAK_CONFIRM")}, [npc, send](bool yes) {
            if (yes) send(proto::GuildDisband{npc});
        });
        return;
    case kRowLeave:
        c.hud->Confirm("UIIT_STT_CONFIRM_BOX", {c.text("UIIT_MSG_GUILD_SECESSION_CONFIRM")}, [npc, send](bool yes) {
            if (yes) send(proto::GuildLeave{npc});
        });
        return;
    case kRowTransfer: OpenGuildMasterTransfer(c); return;
    case kRowElection:
        // "For 10 minutes, you will participate in the vote to elect the leader. Will you proceed?"
        c.hud->Confirm("UIIT_STT_CONFIRM_BOX", {c.text("UIIT_MSG_MRELEASE_CONFIRM")}, [npc, send](bool yes) {
            if (yes) send(proto::GuildElectionStart{npc});
        });
        return;
    case kRowVote: send(proto::GuildElectionOpen{npc}); return;
    case kRowWarReward: send(proto::GuildWarRewardQuery{npc}); return;
    case kRowGpHistory: send(proto::GuildGpHistoryOpen{npc}); return;
    default: break;
    }
    // A mercenary ability's row (the 1.188 client's checks before 0x7259): the release of none held, an ability held
    // already, a third one: each its line; else the request with the NPC.
    if (choice >= kGuildRowSoldierAbility && choice < kGuildRowSoldierAbility + 0x100) {
        const auto ability = std::uint8_t(choice - kGuildRowSoldierAbility);
        const std::uint8_t held = c.guild->Guild().mercenaryAttr;
        const auto say = [&](const char* key) {
            if (c.state != nullptr) c.state->Post(game::GameMessage::Kind::General, c.text(key), c.time);
        };
        if (ability == 0 && held == 0) return say("UIIT_MSG_GUILD_SOLDIER_ABILITY_INITIALIZE_ERROR");
        if ((held & ability) != 0) return say("UIIT_MSG_GUILD_SOLDIER_ABILITY_SELECT_ERROR");
        if (ability != 0 && game::MercenaryAbilityCount(held) >= game::kMercMostAbilities) return say("UIIT_MSG_GUILD_SOLDIER_ABILITY_OVER");
        send(proto::GuildMercenaryAbility{npc, ability});
    }
}

} // namespace sr::client
