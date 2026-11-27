// The game guide (the Help key; 1.188's CIFGameGuide from resinfo\ifgameguide.txt):
// - its start page: gd_guide.ddj (20) over UIIT_STT_GAMEGUIDE_START_1 (21, dark brown 0xFF2C0F01) and _START_2 (22,
//   orange 0xFFE24A00), 14-pixel lines, until a topic is chosen;
// - its page (the CIFPML 18, ours a copy that scrolls by the bar 19 and the wheel): the topic's text (texthelp.txt's
//   SRO_GGW_*, the quests' SN_PAYCON_*), SML2 with its pictures;
// - its menu (CIFGGMenu 13, resinfo\ifggmenu.txt, left of the window), folded away and back by the side button 12
//   (com_side_button / com_side02_button; the side tab 11 with it): three tabs (60 x 24 at 14 + 62 x i, 12;
//   UIIT_STT_GAMEGUIDE, UIIT_STT_GAMEEVENTGUIDE, UIIT_STT_QUESTGUIDE1) over the rows of gameguidedata.txt, 164 x 28 each:
//   a heading (CIFGGMainSlot: gd_index_button_open / close, its title in 0xFFEFDAA4) opens and closes its topics
//   (CIFGGSubSlot: the lamp gd_contents_lamp on the one shown, a white title), a topic shows its page.
//   The tabs: the game guide's ids under 50000; the event guide's (50000..99999) whose events the player has met
//   (eventguidedata.txt; ours reads its level and quest-taken events); the quest guide's (from 100000).

#include <algorithm>
#include <array>
#include <functional>
#include <set>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/assets/GameGuide.h"

namespace sr::client {

namespace {

constexpr int kSideTab = 11, kSideButton = 12, kMenu = 13, kPml = 18, kPageBar = 19, kStartPicture = 20, kStartText1 = 21, kStartText2 = 22;
constexpr int kTitle = 24, kPage = 118; // ours: the scrolled page in the PML's place
constexpr int kTabFirst = 100;
constexpr float kRowHeight = 28.0f, kRowWidth = 164.0f;
const ui::Color kHeadingColour = ui::Argb(0xFFEFDAA4);

// The page: the topic's rich text, scrolled by `scroll` pixels inside its rect.
class GuidePage : public ui::RichText {
public:
    using RichText::RichText;
    float scroll = 0.0f;
    void Draw(ui::Frame& frame) override {
        frame.paint.PushClip(ScreenRect());
        const ui::Rect kept = rect;
        rect.y -= scroll;
        rect.h += scroll;
        RichText::Draw(frame);
        rect = kept;
        frame.paint.PopClip();
    }
};

// A widget that draws by its owner's hand (over the children made before it).
class DrawnBy : public ui::Widget {
public:
    DrawnBy(const assets::UiLayoutNode& node, std::function<void(ui::Frame&)> draw) : Widget(node), draw_(std::move(draw)) {}
    void Draw(ui::Frame& frame) override { draw_(frame); }

private:
    std::function<void(ui::Frame&)> draw_;
};

// The menu: its tabs and rows inside the scroll manager (13 of its own layout).
class GuideMenu : public ui::FrameBorder {
public:
    GuideMenu(const assets::UiLayoutNode& node, HudContext& c) : FrameBorder(node), c_(c) {}
    std::function<void(const std::string& pageKey, const std::string& title)> onTopic;

    void Created(ui::Interface& ui) override {
        constexpr const char* kTabs[] = {"UIIT_STT_GAMEGUIDE", "UIIT_STT_GAMEEVENTGUIDE", "UIIT_STT_QUESTGUIDE1"};
        for (int i = 0; i < 3; ++i) {
            assets::UiLayoutNode tab;
            tab.type = "CIFButton";
            tab.id = kTabFirst + i;
            tab.rect = {14.0f + 62.0f * float(i), 12.0f, 60.0f, 24.0f};
            tab.ddj = "interface\\ifcommon\\com_tab_off.ddj";
            tab.hAlign = 1;
            tab.vAlign = 1;
            ui::Widget* made = Add(ui.Make(tab));
            made->SetText(c_.Text(kTabs[i]));
            tabs_[std::size_t(i)] = made;
        }
        list_ = Child(13);
        bar_ = list_ != nullptr ? ui::As<ui::ScrollBar>(list_->Child(10)) : nullptr;
        interactive = true;
        // The rows over the scroll manager's frame.
        assets::UiLayoutNode rows;
        rows.id = 900; // ours
        Add(std::make_unique<DrawnBy>(rows, [this](ui::Frame& frame) { DrawRows(frame); }));
    }

    void SelectTab(int tab) {
        tab_ = tab;
        for (int i = 0; i < 3; ++i) {
            if (auto* b = ui::As<ui::Button>(tabs_[std::size_t(i)])) b->texture = i == tab ? "interface\\ifcommon\\com_tab_on.ddj" : "interface\\ifcommon\\com_tab_off.ddj";
        }
        open_.clear();
        chosen_ = 0;
        Fill();
    }
    int tab() const { return tab_; }

    void Update(ui::Frame& frame) override {
        ui::FrameBorder::Update(frame);
        if (!filled_ && c_.gameGuide && c_.gameGuide() != nullptr) SelectTab(tab_);
        if (bar_ != nullptr) first_ = bar_->position();
    }

    void Mouse(ui::Frame& frame) override {
        if (list_ == nullptr) return;
        const ui::Rect area = Rows();
        const ui::Vec2 m = frame.pointer.position;
        if (!area.Contains(m) || frame.pointer.hovered == nullptr) return;
        if (frame.input.wheel() != 0.0f && bar_ != nullptr) bar_->SetPosition(bar_->position() + (frame.input.wheel() > 0.0f ? -1 : 1));
        if (!frame.input.Released(ui::MouseButton::Left)) return;
        const int row = first_ + int((m.y - area.y) / kRowHeight);
        if (row < 0 || std::size_t(row) >= shown_.size()) return;
        const assets::GameGuideRow& r = *shown_[std::size_t(row)];
        if (r.level == 0) {
            // A heading opens or closes its topics.
            if (!open_.erase(r.id)) open_.insert(r.id);
            Layout();
        } else {
            chosen_ = r.id;
            if (onTopic) onTopic(r.pageKey, c_.Text(r.menuKey));
        }
        frame.interface.Sound("SND_BUTTON_CLICK");
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.control >= kTabFirst && n.control < kTabFirst + 3 && n.source != nullptr && n.source->parent() == this) {
            SelectTab(n.control - kTabFirst);
            if (onTopic) onTopic({}, {}); // the start page again
            return true;
        }
        return ui::FrameBorder::OnNotice(n);
    }

    void DrawRows(ui::Frame& frame) {
        if (list_ == nullptr) return;
        const ui::Rect area = Rows();
        frame.paint.PushClip(area);
        const int visible = int(area.h / kRowHeight);
        for (int i = 0; i < visible && std::size_t(first_ + i) < shown_.size(); ++i) {
            const assets::GameGuideRow& r = *shown_[std::size_t(first_ + i)];
            const float y = area.y + float(i) * kRowHeight;
            if (r.level == 0) {
                const bool open = open_.contains(r.id);
                frame.paint.Image(open ? "interface\\guide\\gd_index_button_close.ddj" : "interface\\guide\\gd_index_button_open.ddj", {area.x + 4.0f, y + 4.0f, 24.0f, 24.0f});
                frame.paint.Text(c_.Text(r.menuKey), {area.x + 28.0f, y + 8.0f, 131.0f, 12.0f}, kHeadingColour, 0, 0, 0);
            } else {
                if (r.id == chosen_) frame.paint.Image("interface\\guide\\gd_contents_lamp.ddj", {area.x + 33.0f, y + 7.0f, 8.0f, 16.0f});
                frame.paint.Text(c_.Text(r.menuKey), {area.x + 43.0f, y + 9.0f, 107.0f, 12.0f}, ui::kWhite, 0, 0, 0);
            }
        }
        frame.paint.PopClip();
    }

private:
    // The rows' place: the scroll manager's, less its bar.
    ui::Rect Rows() const {
        const ui::Rect r = list_->ScreenRect();
        return {r.x + 6.0f, r.y + 6.0f, std::min(kRowWidth, r.w - 22.0f), r.h - 12.0f};
    }

    // The tab's rows: its headings in their order, each with the topics that name it.
    void Fill() {
        rows_.clear();
        const assets::GameGuideData* data = c_.gameGuide ? c_.gameGuide() : nullptr;
        filled_ = data != nullptr;
        if (data == nullptr) return;
        const auto inTab = [&](std::uint32_t id) { return tab_ == 0 ? id < 50000 : tab_ == 1 ? id >= 50000 && id < 100000 : id >= 100000; };
        const std::set<std::uint32_t> met = tab_ == 1 ? EventsMet(*data) : std::set<std::uint32_t>{};
        // The quest guide: the quests up to the player's level + 10 (1.188's 0x7678D0, from its quest table: a topic whose
        // quest is not in questdata.txt never comes), a topic's quest named by its page (SN_PAYCON_<codename>).
        const int most = (c_.state != nullptr ? int(c_.state->self.level) : 1) + 10;
        const auto tooHigh = [&](const assets::GameGuideRow& r) {
            constexpr std::string_view kPay = "SN_PAYCON_";
            if (tab_ != 2 || r.level != 1 || !r.pageKey.starts_with(kPay)) return false;
            const auto it = data->questLevels.find(r.pageKey.substr(kPay.size()));
            return it == data->questLevels.end() || it->second > most;
        };
        for (const assets::GameGuideRow& r : data->topics) {
            if (!r.inService || !inTab(r.id)) continue;
            if (tab_ == 1 && r.level == 1 && !met.contains(r.id)) continue;
            if (tooHigh(r)) continue;
            rows_.push_back(&r);
        }
        // An event heading with none of its topics met stays out; a quest heading with none of its quests in reach too.
        if (tab_ == 1 || tab_ == 2) {
            std::erase_if(rows_, [&](const assets::GameGuideRow* h) {
                return h->level == 0 && std::none_of(rows_.begin(), rows_.end(), [&](const assets::GameGuideRow* t) { return t->level == 1 && t->parent == h->id; });
            });
        }
        Layout();
    }

    // The event guide's pages whose events the player has met: a level reached, a quest taken (or finished), for its
    // country or anyone (ours: those two kinds).
    std::set<std::uint32_t> EventsMet(const assets::GameGuideData& data) const {
        std::set<std::uint32_t> out;
        const game::PlayerState* s = c_.state;
        if (s == nullptr) return out;
        const bool europe = s->self.race == 1;
        for (const assets::EventGuideRow& e : data.events) {
            if (!e.inService) continue;
            const bool forAnyone = e.who == "xxx", forChina = e.who == "\xEC\xA4\x91\xEA\xB5\xAD", forEurope = e.who == "\xEC\x9C\xA0\xEB\x9F\xBD";
            if (!(forAnyone || (forChina && !europe) || (forEurope && europe))) continue;
            bool done = false;
            if (e.kind == 1) done = s->self.level >= std::atoi(e.value.c_str());
            if (e.kind == 2 && c_.quests != nullptr) {
                const auto taken = [&](std::uint32_t id) {
                    const game::QuestDef* q = c_.quests->Find(id);
                    return q != nullptr && q->codename == e.value;
                };
                for (const auto& [id, state] : c_.quests->Active()) done = done || taken(id);
                for (const auto& [id, times] : c_.quests->Finished()) done = done || taken(id);
            }
            if (done) out.insert(e.guide);
        }
        return out;
    }

    // The rows shown: the headings, each open one's topics under it.
    void Layout() {
        shown_.clear();
        for (const assets::GameGuideRow* h : rows_) {
            if (h->level != 0) continue;
            shown_.push_back(h);
            if (!open_.contains(h->id)) continue;
            for (const assets::GameGuideRow* t : rows_) {
                if (t->level == 1 && t->parent == h->id) shown_.push_back(t);
            }
        }
        if (bar_ != nullptr && list_ != nullptr) {
            const int visible = int(Rows().h / kRowHeight);
            bar_->SetRange(0, std::max(0, int(shown_.size()) - visible));
            bar_->SetPosition(std::min(bar_->position(), std::max(0, int(shown_.size()) - visible)));
        }
    }

    HudContext& c_;
    std::array<ui::Widget*, 3> tabs_{};
    ui::Widget* list_ = nullptr;
    ui::ScrollBar* bar_ = nullptr;
    int tab_ = 0, first_ = 0;
    bool filled_ = false;
    std::vector<const assets::GameGuideRow*> rows_, shown_;
    std::set<std::uint32_t> open_;
    std::uint32_t chosen_ = 0;
};

class GameGuideWindow : public ui::MainFrame {
public:
    GameGuideWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        // The start page's two texts in their colours, 14-pixel lines.
        const std::pair<int, std::pair<const char*, ui::Color>> starts[] = {{kStartText1, {"UIIT_STT_GAMEGUIDE_START_1", ui::Argb(0xFF2C0F01)}},
                                                                            {kStartText2, {"UIIT_STT_GAMEGUIDE_START_2", ui::Argb(0xFFE24A00)}}};
        for (const auto& [id, text] : starts) {
            if (auto* box = ui::As<ui::TextBox>(Child(id))) {
                box->lineHeight = 14.0f;
                box->ClearParagraphs();
                box->AddParagraph(c_.Text(text.first), text.second);
            }
        }
        // The page: ours in the PML's place, scrolled.
        if (ui::Widget* pml = Child(kPml)) {
            assets::UiLayoutNode node = pml->node();
            node.type = "SRGuidePage";
            node.id = kPage;
            page_ = dynamic_cast<GuidePage*>(Add(ui.Make(node)));
            if (page_ != nullptr) pageRect_ = page_->rect;
            pml->visible = false;
        }
        bar_ = ui::As<ui::ScrollBar>(Child(kPageBar));
        if (auto* menu = dynamic_cast<GuideMenu*>(Child(kMenu))) {
            menu->onTopic = [this](const std::string& pageKey, const std::string& title) { Topic(pageKey, title); };
        }
        Topic({}, {});
    }

    void Update(ui::Frame& frame) override {
        ui::MainFrame::Update(frame);
        if (page_ == nullptr || bar_ == nullptr) return;
        const int most = std::max(0, int(page_->contentHeight - page_->rect.h));
        bar_->SetRange(0, most, 18);
        if (frame.pointer.hovered == page_ && frame.input.wheel() != 0.0f) bar_->SetPosition(bar_->position() + (frame.input.wheel() > 0.0f ? -18 : 18));
        page_->scroll = float(std::min(bar_->position(), most));
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.control == kSideButton && n.source != nullptr && n.source->parent() == this) {
            FoldMenu(!menuFolded_);
            return true;
        }
        return ui::MainFrame::OnNotice(n);
    }

private:
    // A topic's page (an empty key: the start page).
    void Topic(const std::string& pageKey, const std::string& title) {
        const bool start = pageKey.empty();
        for (int id : {kStartPicture, kStartText1, kStartText2}) {
            if (ui::Widget* w = Child(id)) w->visible = start;
        }
        if (page_ != nullptr) {
            std::string text = start ? std::string() : c_.Text(pageKey);
            if (!text.empty() && !text.starts_with("<sml2>")) text = "<sml2>" + text + "</sml2>";
            page_->SetText(text);
            page_->visible = !start;
            page_->scroll = 0.0f;
        }
        if (bar_ != nullptr) bar_->SetPosition(0);
        // The quest guide names its quest over the page (ours: the page a line lower then, their rects overlapping).
        if (ui::Widget* name = Child(kTitle)) {
            const auto* menu = dynamic_cast<GuideMenu*>(Child(kMenu));
            const bool named = !start && menu != nullptr && menu->tab() == 2;
            name->SetText(named ? title : std::string());
            if (page_ != nullptr) {
                page_->rect = pageRect_;
                if (named) page_->rect.y += 14.0f, page_->rect.h -= 14.0f;
            }
        }
    }

    // The menu folded behind the window or out again: the side tab and button with it, the button's arrow turned.
    void FoldMenu(bool folded) {
        menuFolded_ = folded;
        if (ui::Widget* menu = Child(kMenu)) menu->visible = !folded;
        ui::Widget* tab = Child(kSideTab);
        ui::Widget* button = Child(kSideButton);
        if (tab != nullptr) tab->rect.x = folded ? -19.0f : tab->node().rect.x;
        if (button != nullptr) {
            button->rect.x = folded ? -15.0f : button->node().rect.x;
            if (auto* b = ui::As<ui::Button>(button)) b->texture = folded ? "interface\\ifcommon\\com_side02_button.ddj" : "interface\\ifcommon\\com_side_button.ddj";
        }
    }

    HudContext& c_;
    GuidePage* page_ = nullptr;
    ui::Rect pageRect_;
    ui::ScrollBar* bar_ = nullptr;
    bool menuFolded_ = false;
};

} // namespace

void RegisterGameGuideWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CIFGameGuide", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<GameGuideWindow>(n, *c); });
    ui.Register("CIFGGMenu", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<GuideMenu>(n, *c); });
    ui.Register("SRGuidePage", [](const assets::UiLayoutNode& n) { return std::make_unique<GuidePage>(n); });
}

} // namespace sr::client
