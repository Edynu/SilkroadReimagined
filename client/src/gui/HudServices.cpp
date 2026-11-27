// The item mall's services on the client: the store's Repair and Repair all at the
// smiths and armour merchants, the repair hammer's box, the equipment gender switch tool (the next armour clicked), the
// skin change window (1.188's CIFChangePlayerModel, ifchangeplayermodel.txt, ginterface 54), the skill restoration
// potion's withdrawal window (CIFSkillWithdrawal, ginterface 43: the skill board in its withdrawal mode) and its box,
// the magic cubes (an elixir dropped on its cube, "Will you use the Magic Cube?" taking one out, the tooltip's list),
// and the answers' system lines.

#include <algorithm>
#include <cmath>
#include <format>

#include "gui/ErrorTexts.h"
#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/assets/Dress.h"
#include "sr/game/Cos.h"
#include "sr/game/CharacterRules.h"
#include "sr/game/MallProtocol.h"
#include "sr/game/MallServices.h"
#include "sr/game/PlayerState.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr int kSkinWindow = 54, kWithdrawalWindow = 43; // ginterface.txt's ids (GDR_CHANGE_PLAYER_MODEL, GDR_SKILLWITHDRAWAL)
// CIFChangePlayerModel's controls (ifchangeplayermodel.txt).
constexpr int kOk = 17, kCancel = 18, kMale = 31, kFemale = 32, kFigure = 33, kHeight = 34, kVolume = 35;
constexpr int kTurnLeft = 41, kTurnReset = 42, kTurnRight = 43, kView = 100, kTitle = 900;

template <typename T>
void Send(HudContext& c, const T& message) {
    if (c.serviceRequest) c.serviceRequest(T::kId, proto::Encode(message));
}

void Line(HudContext& c, const std::string& text) {
    if (c.state != nullptr && !text.empty()) c.state->Post(game::GameMessage::Kind::General, text, c.time);
}

// The services' codes' texts: the original's table (ErrorTexts.h), and two of ours it has none for.
const char* ServiceKey(std::uint16_t code) {
    switch (code) {
    case 0x1807: return "UIIT_MSG_STRGERR_INVENTORY_FULL";
    case 0x1C09: return "UIIT_MSG_STRGERR_THERE_IS_NO_ITEM_TO_REPAIR"; // ours: the interaction group's text not traced
    default: return OriginalErrorKey(code);
    }
}

const assets::ItemRow* RowAt(const HudContext& c, int place, const game::ItemInstance** item = nullptr) {
    if (c.state == nullptr || c.items == nullptr || place < 0 || place > 255) return nullptr;
    const game::ItemInstance* i = c.state->inventory.At(std::uint8_t(place));
    if (item != nullptr) *item = i;
    return i != nullptr ? c.items->Find(i->refId) : nullptr;
}

game::SkillBook BookOf(const HudContext& c) {
    game::SkillBook book;
    if (c.state == nullptr || c.skills == nullptr) return book;
    for (const auto& [mastery, level] : c.state->masteries) book.masteries[mastery] = level;
    for (const std::uint32_t id : c.state->skills) {
        if (const assets::SkillRow* row = c.skills->Find(id)) book.groups[row->group] = row->level;
    }
    return book;
}

// --- The skin change window (CIFChangePlayerModel) -------------------------------------------------

// Its sex buttons, the Figure / Height / Volume sliders (the create screen's ranges: the race's figures of the sex,
// 0..4 each), the view turned by its three buttons (the equipment window's view, showing the chosen look), OK sending
// the scroll's use with the model and the shape byte, Cancel.
class SkinWindow : public ui::MainFrame {
public:
    SkinWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        // Its title, a static made in code at Create (the window's first control, UIIT_PAG_CHAR_SKIN_CHANGE; its place
        // inferred: centred in the frame's top band).
        assets::UiLayoutNode title;
        title.type = "CIFStatic";
        title.id = kTitle;
        title.rect = {0.0f, 12.0f, rect.w, 16.0f};
        title.hAlign = 1;
        title.vAlign = 1;
        if (ui::Widget* t = Add(ui.Make(title))) t->SetText(c_.Text("UIIT_PAG_CHAR_SKIN_CHANGE"));
        // The view's black square and tile are drawn by the window, under the character (as the equipment window's).
        for (int id : {15, 16}) {
            if (ui::Widget* w = Child(id)) w->visible = false;
        }
        for (int id : {kFigure, kHeight, kVolume}) {
            if (auto* s = ui::As<ui::Slider>(Child(id))) {
                s->min = 0;
                s->max = 4;
                s->value = 2;
            }
        }
    }

    // Opened by the scroll in `place`: the character's own look to start from.
    void Start(int place) {
        place_ = place;
        if (c_.state == nullptr || c_.creation == nullptr) return;
        const auto figure = c_.creation->FigureOf(c_.state->self.model);
        europe_ = figure && figure->race == game::CreationRules::Race::Europe;
        male_ = !figure || figure->male;
        const auto figures = c_.creation->FiguresOf(europe_ ? game::CreationRules::Race::Europe : game::CreationRules::Race::China, male_);
        const auto at = std::find(figures.begin(), figures.end(), c_.state->self.model);
        figure_ = at != figures.end() ? int(at - figures.begin()) : 0;
        if (auto* s = ui::As<ui::Slider>(Child(kFigure))) {
            s->max = std::max(0, int(figures.size()) - 1);
            s->SetValue(figure_);
        }
        yaw_ = 0.0f;
        Shown();
    }

    void Draw(ui::Frame& frame) override {
        MainFrame::Draw(frame);
        for (int id : {15, 16}) {
            if (ui::Widget* w = Child(id)) w->Draw(frame);
        }
        if (c_.characterView.shown && c_.characterView.model != 0) {
            const ui::Vec2 screen = frame.interface.screen();
            frame.paint.SceneOverlay(c_.characterView.rect, {screen.x * frame.paint.scale(), screen.y * frame.paint.scale()});
        }
    }

    void Update(ui::Frame& frame) override {
        MainFrame::Update(frame);
        // The view: the equipment window's character view, at this window's rect, in the chosen look.
        const ui::Rect r = ScreenRect();
        ui::Widget* view = Child(kView);
        if (view != nullptr && ShownOnScreen()) {
            const ui::Rect v = view->ScreenRect();
            c_.characterView.shown = true;
            c_.characterView.rect = v;
            c_.characterView.yaw = yaw_;
            c_.characterView.model = Model();
            c_.characterView.shape = assets::PackShape(Value(kHeight), Value(kVolume));
            (void)r;
        }
        frameMs_ = frame.dt * 1000.0f;
        // The sex buttons: the chosen one pressed (its art's second state; ours: shown disabled).
        if (ui::Widget* m = Child(kMale)) m->enabled = !male_;
        if (ui::Widget* f = Child(kFemale)) f->enabled = male_;
    }

    bool OnNotice(const ui::Notice& notice) override {
        const bool mine = notice.source != nullptr && notice.source->parent() == this;
        if (mine && notice.kind == ui::Notice::Kind::Clicked) {
            switch (notice.control) {
            case kMale:
            case kFemale: {
                male_ = notice.control == kMale;
                const auto figures = Figures();
                if (auto* s = ui::As<ui::Slider>(Child(kFigure))) {
                    s->max = std::max(0, int(figures.size()) - 1);
                    s->SetValue(std::min(s->value, s->max));
                }
                return true;
            }
            case kTurnLeft: yaw_ -= frameMs_ * 0.0015f * 10.0f; return true;
            case kTurnRight: yaw_ += frameMs_ * 0.0015f * 10.0f; return true;
            case kTurnReset: yaw_ = 0.0f; return true;
            case kOk:
                if (Model() != 0) Send(c_, proto::SkinChange{std::uint8_t(place_), Model(), assets::PackShape(Value(kHeight), Value(kVolume))});
                Close();
                return true;
            case kCancel:
                Close();
                return true;
            default: break;
            }
        }
        if (notice.kind == ui::Notice::Kind::Clicked && notice.control == 2 && mine) {
            Close();
            return true;
        }
        return MainFrame::OnNotice(notice);
    }

    // The script's choice: the sex, the figure's index (0..), the height and the volume (0..4).
    void Choose(bool male, int figure, int height, int volume) {
        male_ = male;
        const auto figures = Figures();
        if (auto* s = ui::As<ui::Slider>(Child(kFigure))) {
            s->max = std::max(0, int(figures.size()) - 1);
            s->SetValue(std::clamp(figure, 0, s->max));
        }
        if (auto* s = ui::As<ui::Slider>(Child(kHeight))) s->SetValue(height);
        if (auto* s = ui::As<ui::Slider>(Child(kVolume))) s->SetValue(volume);
    }
    void Confirm() {
        if (Model() != 0) Send(c_, proto::SkinChange{std::uint8_t(place_), Model(), assets::PackShape(Value(kHeight), Value(kVolume))});
        Close();
    }

private:
    std::vector<std::uint32_t> Figures() const {
        if (c_.creation == nullptr) return {};
        return c_.creation->FiguresOf(europe_ ? game::CreationRules::Race::Europe : game::CreationRules::Race::China, male_);
    }
    int Value(int id) const {
        const auto* s = ui::As<ui::Slider>(const_cast<SkinWindow*>(this)->Child(id));
        return s != nullptr ? s->value : 2;
    }
    std::uint32_t Model() const {
        const auto figures = Figures();
        const int f = std::clamp(Value(kFigure), 0, std::max(0, int(figures.size()) - 1));
        return figures.empty() ? 0 : figures[std::size_t(f)];
    }
    void Close() {
        c_.characterView.model = 0;
        c_.characterView.shown = false;
        if (c_.hud != nullptr) c_.hud->Hide(kSkinWindow);
    }
    void Shown() {}

    HudContext& c_;
    int place_ = -1, figure_ = 0;
    bool europe_ = false, male_ = true;
    float yaw_ = 0.0f, frameMs_ = 16.0f;
};

// --- The withdrawal window (CIFSkillWithdrawal) ----------------------------------------------------

// The skill board (ifskillwithdrawal.txt: CIFSkill id 10) in its withdrawal mode: a learned skill's or the mastery's
// button asks to withdraw a level (the small window "Skill Window" / "Mastery Window", the name,
// "Withdrawn Lv.", "SP Withdrawn"). Ours: a level a time.
class WithdrawalWindow : public ui::MainFrame {
public:
    WithdrawalWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}
    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        SetText(c_.Text("UIIT_STT_CIRCULATION_SYSTEM"));
    }
    bool OnNotice(const ui::Notice& notice) override {
        if (notice.kind == ui::Notice::Kind::Clicked && notice.control == 2 && notice.source != nullptr && notice.source->parent() == this) {
            c_.withdrawPotion = -1;
            if (c_.hud != nullptr) c_.hud->Hide(kWithdrawalWindow);
            return true;
        }
        return MainFrame::OnNotice(notice);
    }

private:
    HudContext& c_;
};

// --- The removal box (CIFSkillRemovalBox, resinfo\ifskillremovalbox.txt; Create, the subject,
// its levels, its refresh, the request, its two looks) ---------------------------

// A skill's or a mastery's levels to withdraw, chosen by its arrows: down (20) one more, up (21) one less, from 0 to the
// most (the potions in hand, at most what may go); its lines refreshed at each (the level after, the count, the
// SP back, 80 % of it with the quest potion and its price); OK (22) sends the level it comes to, Cancel (23).
// The quest potion's look shows the money rows (300 x 244), the mall's hides them (300 x 208, OK and Cancel at y 167).
constexpr int kRemovalBox = 7602; // ours: a window made in code (the original's is the withdrawal window's own)
constexpr int kRbTotal = 11, kRbNeed = 12, kRbMoney = 13, kRbLevel = 14, kRbTarget = 15, kRbCount = 16, kRbPotion = 17, kRbIcon = 18,
              kRbName = 19, kRbDown = 20, kRbUp = 21, kRbOk = 22, kRbCancel = 23;

class SkillRemovalBox : public ui::MainFrame {
public:
    SkillRemovalBox(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        // (Its controls are the class's layout's, ifskillremovalbox.txt, built by the interface: ui/ClassLayouts.cpp.)
        MainFrame::Created(ui);
        // Its pictures: the 32 x 32 icon of the subject, the potion's 16 x 16 (GDR_SKLRB_WITHDRAWICON shares id 30).
        for (const char* name : {"GDR_SKLRB_SKILLICON", "GDR_SKLRB_WITHDRAWICON"}) {
            if (ui::Widget* w = Find(std::string_view(name))) w->interactive = false;
        }
    }

    // The subject: a skill's learned row (or a mastery's id), its level now, the potion in `place`.
    void Ask(const assets::SkillRow* skill, std::uint32_t mastery, int place) {
        skill_ = skill;
        mastery_ = mastery;
        place_ = place;
        count_ = 0;
        const game::SkillBook book = BookOf(c_);
        const game::ItemInstance* potion = nullptr;
        const assets::ItemRow* row = RowAt(c_, place, &potion);
        mode_ = row != nullptr ? game::WithdrawModeOf(*row) : game::WithdrawMode::None;
        level_ = skill != nullptr ? book.GroupLevel(skill->group) : book.MasteryLevel(mastery);
        // The most: the potions in hand (the server takes them from this place) and the levels that may go.
        const int held = potion != nullptr ? int(potion->count) : 0;
        most_ = 0;
        for (int n = 1; n <= level_ && n <= held; ++n) {
            if (Compute(level_ - n).code != 0) break;
            most_ = n;
        }
        // Its title and subject, its look by the potion (the quest potion's with the money rows).
        // (The frame's title is its static 0, made at Create: MainFrame.)
        if (ui::Widget* title = Child(0)) {
            title->SetText(c_.Text(skill != nullptr ? "UIIT_STT_CIRCULATION_WITHDRAW_SKILL_WND" : "UIIT_STT_CIRCULATION_WITHDRAW_MASTERY_WND"));
        }
        const bool money = mode_ == game::WithdrawMode::Quest;
        rect.w = 300.0f;
        rect.h = money ? 244.0f : 208.0f;
        if (ui::Widget* tile = Child(5)) tile->rect.h = money ? 188.0f : 152.0f;
        for (int id : {kRbNeed, kRbMoney, 32, 33, 34, 35}) {
            if (ui::Widget* w = Child(id)) w->visible = money;
        }
        if (ui::Widget* ok = Child(kRbOk)) ok->rect.y = money ? 212.0f : 167.0f;
        if (ui::Widget* cancel = Child(kRbCancel)) cancel->rect.y = money ? 212.0f : 167.0f;
        std::string name, icon;
        if (skill != nullptr) {
            name = c_.Text(skill->nameKey);
            icon = skill->iconPath.empty() ? std::string() : "icon\\" + skill->iconPath;
        } else if (c_.skills != nullptr) {
            if (const assets::MasteryRow* m = c_.skills->Mastery(mastery)) name = c_.Text(m->nameKey);
        }
        if (ui::Widget* w = Child(kRbName)) w->SetText(name);
        if (auto* s = ui::As<ui::Static>(Find(std::string_view("GDR_SKLRB_SKILLICON")))) s->texture = icon;
        if (auto* s = ui::As<ui::Static>(Find(std::string_view("GDR_SKLRB_WITHDRAWICON")))) {
            s->texture = row != nullptr && !row->iconPath.empty() ? "icon\\" + row->iconPath : std::string();
        }
        // Its fixed lines: the gold in hand, the level now, the potion's name and count ("%s %d / %d %s" with the
        // quest potion: its stack's most, inferred; else "%s %d %s").
        if (ui::Widget* w = Child(kRbMoney)) w->SetText(GoldText(c_.state != nullptr ? c_.state->inventory.gold : 0));
        if (ui::Widget* w = Child(kRbLevel)) w->SetText(std::format("{} {}", c_.Text("UIIT_STT_LEVEL_LV"), level_));
        if (ui::Widget* w = Child(kRbPotion); w != nullptr && row != nullptr) {
            const std::string potionName = c_.Text(row->nameKey), unit = c_.Text("UIIT_STT_UNIT");
            w->SetText(money ? std::format("{} {} / {} {}", potionName, held, row->maxStack, unit) : std::format("{} {} {}", potionName, held, unit));
        }
        Refresh();
    }

    // The arrows and the buttons as clicks (scripts).
    void Down() {
        if (count_ < most_) ++count_;
        Refresh();
    }
    void Up() {
        if (count_ > 0) --count_;
        Refresh();
    }
    void Answer(bool ok) {
        // The level it comes to, sent only with something chosen, the box put away either way.
        if (ok && count_ > 0 && place_ >= 0) {
            const std::uint32_t id = skill_ != nullptr ? skill_->id : mastery_;
            Send(c_, proto::Withdraw{std::uint8_t(place_), std::uint8_t(skill_ != nullptr ? 0 : 1), id, std::uint8_t(level_ - count_)});
        }
        visible = false;
    }
    int Count() const { return count_; }

    bool OnNotice(const ui::Notice& notice) override {
        if (notice.kind == ui::Notice::Kind::Clicked && notice.source != nullptr && notice.source->parent() == this) {
            switch (notice.control) {
            case kRbDown: Down(); return true;
            case kRbUp: Up(); return true;
            case kRbOk: Answer(true); return true;
            case kRbCancel:
            case 2: Answer(false); return true;
            default: break;
            }
        }
        return MainFrame::OnNotice(notice);
    }

private:
    game::Withdrawal Compute(int target) const {
        const game::SkillBook book = BookOf(c_);
        // The quest potion's price by the highest level reached (the server's GoldMin, levelgold.txt's col 1).
        std::uint32_t goldMin = 0;
        if (c_.levelGold != nullptr && !c_.levelGold->empty() && c_.state != nullptr) {
            const std::size_t at = std::clamp<std::size_t>(c_.state->self.highestLevel, 1, c_.levelGold->size() - 1);
            goldMin = (*c_.levelGold)[at].first;
        }
        if (c_.skills == nullptr) return {};
        if (skill_ != nullptr) return game::WithdrawSkill(book, *c_.skills, skill_->id, target, mode_, goldMin);
        return game::WithdrawMastery(book, *c_.skills, c_.masteryCosts != nullptr ? *c_.masteryCosts : std::vector<std::uint32_t>{}, mastery_, target,
                                     mode_, goldMin);
    }
    // the arrows enabled by the count, the level after, the count, the SP back and the price.
    void Refresh() {
        if (ui::Widget* w = Child(kRbDown)) w->enabled = count_ < most_;
        if (ui::Widget* w = Child(kRbUp)) w->enabled = count_ > 0;
        if (ui::Widget* w = Child(kRbOk)) w->enabled = count_ > 0;
        if (ui::Widget* w = Child(kRbTarget)) w->SetText(std::format("{} {}", c_.Text("UIIT_STT_LEVEL_LV"), level_ - count_));
        if (ui::Widget* w = Child(kRbCount)) w->SetText(std::to_string(count_));
        const game::Withdrawal out = count_ > 0 ? Compute(level_ - count_) : game::Withdrawal{};
        if (ui::Widget* w = Child(kRbTotal)) w->SetText(std::to_string(out.skillPoints));
        if (ui::Widget* w = Child(kRbNeed)) w->SetText(GoldText(out.gold));
    }

    HudContext& c_;
    const assets::SkillRow* skill_ = nullptr;
    std::uint32_t mastery_ = 0;
    int place_ = -1, level_ = 0, count_ = 0, most_ = 0;
    game::WithdrawMode mode_ = game::WithdrawMode::None;
};

SkillRemovalBox* RemovalBox(HudContext& c, bool open) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return nullptr;
    if (auto* box = dynamic_cast<SkillRemovalBox*>(ui->Window(kRemovalBox))) return box;
    if (!open) return nullptr;
    const ui::Vec2 screen = c.hud->Screen();
    assets::UiLayoutNode node;
    node.type = "CIFSkillRemovalBox";
    node.name = "GDR_SKILLREMOVALBOX";
    node.id = kRemovalBox;
    node.ddj = "interface\\frame\\mframe_wnd_";
    // Ours: centred (the original's place not traced).
    node.rect = {std::floor((screen.x - 300.0f) / 2.0f), std::floor((screen.y - 244.0f) / 2.0f), 300.0f, 244.0f};
    return dynamic_cast<SkillRemovalBox*>(ui->Open(node));
}

// The premium package's uses (1.188's CIFCompositeItemWnd, interface 36, resinfo\ifcompositeitemwnd.txt), opened by a right
// click on its icon on the buff board: a sys_button.ddj button for each of its entries, 150 x 25 at 31, 58 and every 34
// below, "%s(%d)" (its item's name, its uses left today; none left: disabled), then Cancel; the frame and the window as
// tall as their buttons (the frame 15 + 34 a button, the window 70 + 36). A button uses that entry (as the chat's
// premium commands) and the window closes.
class CompositeItemWindow : public ui::MainFrame {
public:
    CompositeItemWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        ui_ = &ui;
    }

    void Fill() {
        if (ui_ == nullptr || c_.state == nullptr) return;
        for (ui::Widget* b : buttons_) b->visible = false;
        entries_.clear();
        for (const proto::PremiumUses& u : c_.state->timedServices.premium) entries_.push_back(u);
        const std::size_t n = entries_.size() + 1; // and Cancel
        for (std::size_t i = 0; i < n; ++i) {
            if (i >= buttons_.size()) {
                assets::UiLayoutNode node;
                node.type = "CIFButton";
                node.id = kFirst + int(i);
                node.ddj = "interface\\system\\sys_button.ddj";
                node.hAlign = 1;
                node.vAlign = 1;
                node.rect = {31.0f, 58.0f + 34.0f * float(i), 150.0f, 25.0f};
                buttons_.push_back(Add(ui_->Make(node)));
            }
            ui::Widget* b = buttons_[i];
            b->visible = true;
            b->rect.y = 58.0f + 34.0f * float(i);
            if (i == entries_.size()) {
                b->SetText(c_.Text("UIIT_CTL_CANCEL"));
                b->enabled = true;
                continue;
            }
            const proto::PremiumUses& u = entries_[i];
            const assets::ItemRow* row = c_.items != nullptr ? c_.items->Find(u.item) : nullptr;
            b->SetText(std::format("{}({})", row != nullptr ? c_.Text(row->nameKey) : std::to_string(u.item), int(u.left)));
            b->enabled = u.left > 0;
        }
        if (ui::Widget* frame = Child(6)) frame->rect.h = 15.0f + 34.0f * float(n);
        if (ui::Widget* tile = Child(5)) tile->rect.h = 34.0f * float(n) - 8.0f; // ours: the tile under the buttons
        rect.h = 70.0f + 36.0f * float(n);
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr && n.source->parent() == this && n.control >= kFirst) {
            const std::size_t i = std::size_t(n.control - kFirst);
            if (i < entries_.size() && c_.premiumUse) c_.premiumUse(entries_[i].item);
            if (c_.hud != nullptr) c_.hud->Hide(hud::kCompositeItem);
            return true;
        }
        return ui::MainFrame::OnNotice(n);
    }

private:
    static constexpr int kFirst = 10;
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::vector<ui::Widget*> buttons_;
    std::vector<proto::PremiumUses> entries_;
};

} // namespace

bool InWithdrawalWindow(const ui::Widget* w) {
    for (; w != nullptr; w = w->parent()) {
        if (dynamic_cast<const WithdrawalWindow*>(w) != nullptr) return true;
    }
    return false;
}

void AskWithdraw(HudContext& c, const assets::SkillRow* skill, std::uint32_t mastery) {
    if (c.hud == nullptr || c.skills == nullptr || c.state == nullptr || c.withdrawPotion < 0) return;
    // A level that cannot go at all says why at once (ours; the original's box opens with its down arrow off).
    const game::SkillBook book = BookOf(c);
    const assets::ItemRow* row = RowAt(c, c.withdrawPotion);
    const game::WithdrawMode mode = row != nullptr ? game::WithdrawModeOf(*row) : game::WithdrawMode::None;
    const int level = skill != nullptr ? book.GroupLevel(skill->group) : book.MasteryLevel(mastery);
    const game::Withdrawal w = skill != nullptr ? game::WithdrawSkill(book, *c.skills, skill->id, level - 1, mode, 0)
                                               : game::WithdrawMastery(book, *c.skills, c.masteryCosts != nullptr ? *c.masteryCosts : std::vector<std::uint32_t>{},
                                                                       mastery, level - 1, mode, 0);
    if (w.code != 0) {
        if (const char* key = ServiceKey(w.code)) Line(c, c.Text(key));
        return;
    }
    if (SkillRemovalBox* box = RemovalBox(c, true)) {
        box->Ask(skill, mastery, c.withdrawPotion);
        box->visible = true;
        if (ui::Interface* ui = c.hud->interface()) ui->Raise(box);
    }
}

bool WithdrawBoxShown(HudContext& c) {
    const SkillRemovalBox* box = RemovalBox(c, false);
    return box != nullptr && box->visible;
}

bool WithdrawBoxStep(HudContext& c, int down, bool answer, bool ok) {
    SkillRemovalBox* box = RemovalBox(c, false);
    if (box == nullptr || !box->visible) return false;
    for (int k = 0; k < down; ++k) box->Down();
    if (answer) box->Answer(ok);
    return true;
}

bool ChooseSkin(HudContext& c, bool male, int figure, int height, int volume, bool confirm) {
    if (c.hud == nullptr || !c.hud->Shown(kSkinWindow)) return false;
    auto* w = dynamic_cast<SkinWindow*>(c.hud->Show(kSkinWindow));
    if (w == nullptr) return false;
    if (confirm) w->Confirm();
    else w->Choose(male, figure, height, volume);
    return true;
}

void ShowCompositeItems(HudContext& c) {
    // Made from its section of ginterface.txt (CompositeItem) on demand, as 1.188 makes it, then filled.
    if (c.hud == nullptr) return;
    if (auto* w = dynamic_cast<CompositeItemWindow*>(c.hud->Show(hud::kCompositeItem))) w->Fill();
}

void RegisterServiceWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CIFCompositeItemWnd", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<CompositeItemWindow>(n, *c); });
    ui.Register("CIFChangePlayerModel", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<SkinWindow>(n, *c); });
    ui.Register("CIFSkillWithdrawal", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<WithdrawalWindow>(n, *c); });
    ui.Register("CIFSkillRemovalBox", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<SkillRemovalBox>(n, *c); });
}

bool UseServiceItem(HudContext& c, int place) {
    const game::ItemInstance* item = nullptr;
    const assets::ItemRow* row = RowAt(c, place, &item);
    if (row == nullptr || c.hud == nullptr) return false;
    HudContext* ctx = &c;
    if (game::IsSkinChangeScroll(*row)) {
        if (auto* w = dynamic_cast<SkinWindow*>(c.hud->Show(kSkinWindow))) w->Start(place);
        return true;
    }
    if (game::WithdrawModeOf(*row) != game::WithdrawMode::None) {
        c.withdrawPotion = place;
        c.hud->Show(kWithdrawalWindow);
        return true;
    }
    // An item used on another picked next (the cursor mode 0xA6, "pick a target item"; the bag's use routine):
    // the gender switch (3/3/13/8), the Clock of Reincarnation (3/3/13/12), the potion of growth (15), the Extension Gear
    // (16), and the pets' revival (3/3/1/6, the Grass of Life). The click on the target sends the use (ServicePick).
    const bool picksTarget = game::IsGenderSwitchTool(*row) || (row->typeId[0] == 3 && row->typeId[1] == 3 && row->typeId[2] == 13 &&
                                                                (row->typeId[3] == 12 || row->typeId[3] == 15 || row->typeId[3] == 16)) ||
                             row->typeId == std::array<int, 4>{3, 3, 1, 6};
    if (picksTarget) {
        c.servicePick = 2;
        c.servicePickTool = place;
        return true;
    }
    if (game::IsRepairHammer(*row)) {
        // "When the hammer is clicked, the repair window will appear. Press the ok button and all items will be repaired."
        // (the box kind 0x0C, no price; inferred).
        const std::uint8_t at = std::uint8_t(place);
        if (ConfirmBox* box = c.hud->Confirm("UIIT_STT_CONFIRM_BOX", {c.Text("UIIT_MSG_MSGBOX_REPAIR_ITEM")}, [ctx, at](bool yes) {
                if (yes) Send(*ctx, proto::UseItem{at});
            })) {
            box->Header("UIIT_CTL_REPAIR_ALL");
        }
        return true;
    }
    if (game::IsMagicCube(*row)) {
        // Ours: one taken out to the bag by the question (the original's extract request names the cube's place; its
        // picking of the place is not traced).
        const auto it = c.cubes.find(std::uint8_t(place));
        if (it == c.cubes.end() || it->second.empty()) return true;
        const std::uint8_t at = std::uint8_t(place), slot = it->second.front().slot;
        c.hud->Confirm("UIIT_STT_CONFIRM_BOX", {c.Text("UIIT_MSG_MAGIC_CUBE_USE")}, [ctx, at, slot](bool yes) {
            if (yes) Send(*ctx, proto::CubeMove{1, 0, at, slot});
        });
        return true;
    }
    return false;
}

bool ServicePicking(const HudContext& c) { return c.servicePick != 0; }

bool ServicePick(HudContext& c, int place) {
    const int mode = c.servicePick;
    c.servicePick = 0;
    if (mode == 1 && c.servicePickNpc != 0 && place >= 0) {
        Send(c, proto::Repair{c.servicePickNpc, 1, std::uint8_t(place)});
        return true;
    }
    if (mode == 2 && c.servicePickTool >= 0 && place >= 0) {
        const std::uint8_t tool = std::uint8_t(c.servicePickTool), on = std::uint8_t(place);
        const assets::ItemRow* toolRow = RowAt(c, tool, nullptr);
        if (toolRow != nullptr && game::PetTreatmentOf(*toolRow) != game::PetTreatment::None) {
            // On a pet's item: its pet treated (the clock's time, the revival; the server checks the pet).
            if (c.cosRequest) c.cosRequest(proto::CosUseOnPet::kId, proto::Encode(proto::CosUseOnPet{tool, on}));
            return true;
        }
        if (toolRow != nullptr && toolRow->typeId == std::array<int, 4>{3, 3, 13, 16} && c.hud != nullptr) {
            // The Extension Gear asks first (UIIT_MSG_NASRUN_EXTENSION_ITEM_USE).
            HudContext* ctx = &c;
            c.hud->Confirm("", {c.Text("UIIT_MSG_NASRUN_EXTENSION_ITEM_USE")}, [ctx, tool, on](bool yes) {
                if (yes) Send(*ctx, proto::UseItemOn{tool, on});
            });
            return true;
        }
        Send(c, proto::UseItemOn{tool, on});
        return true;
    }
    return mode != 0;
}

std::uint64_t RepairAllPrice(const HudContext& c) {
    if (c.state == nullptr || c.items == nullptr) return 0;
    std::uint64_t gold = c.state->inventory.gold, price = 0;
    for (const auto& [place, item] : c.state->inventory.places) {
        if (place >= game::slot::kEarring && place < game::slot::kWorn) continue;
        const assets::ItemRow* row = c.items->Find(item.refId);
        if (row == nullptr || !game::HasDurability(*row) || !row->canRepair) continue;
        const std::uint32_t most = game::MostDurability(*row, item, c.magicOptions);
        const game::RepairQuote q = game::QuoteRepair(*row, item.durability, most, gold);
        price += q.gold;
        gold -= std::min(gold, q.gold);
    }
    return price;
}

void AskRepairAll(HudContext& c, std::uint32_t npc) {
    if (c.hud == nullptr) return;
    HudContext* ctx = &c;
    // The original's box 0x0C: "Repair All" on the name strip, the question, the price (ours: a line; the original's in a
    // black box beside "Gold").
    if (ConfirmBox* box = c.hud->Confirm("UIIT_STT_CONFIRM_BOX", {c.Text("UIIT_MSG_MSGBOX_REPAIR_ITEM"), std::format("{} : {}", c.Text("UIIT_STT_GOLD"), RepairAllPrice(c))},
                                         [ctx, npc](bool yes) {
                                             if (yes) Send(*ctx, proto::Repair{npc, 2, 0});
                                         })) {
        box->Header("UIIT_CTL_REPAIR_ALL");
    }
}

bool CubeDrop(HudContext& c, int from, int to) {
    const assets::ItemRow* cube = RowAt(c, to);
    const assets::ItemRow* item = RowAt(c, from);
    if (cube == nullptr || item == nullptr || !game::IsMagicCube(*cube) || from == to) return false;
    if (!game::CubeTakes(*cube, *item)) return false; // an ordinary move
    Send(c, proto::CubeMove{0, std::uint8_t(from), std::uint8_t(to), 0});
    return true;
}

std::vector<std::string> CubeLines(const HudContext& c, int place) {
    std::vector<std::string> out;
    const assets::ItemRow* cube = RowAt(c, place);
    if (cube == nullptr || !game::IsMagicCube(*cube) || c.items == nullptr) return out;
    const auto it = c.cubes.find(std::uint8_t(place));
    const std::size_t held = it != c.cubes.end() ? it->second.size() : 0;
    out.push_back(std::format("{} / {}", held, game::CubePlaces(*cube)));
    if (it != c.cubes.end()) {
        for (const proto::ItemRecord& r : it->second) {
            const assets::ItemRow* row = c.items->Find(r.item);
            if (row != nullptr) out.push_back(r.quantity > 1 ? std::format("{} x{}", c.Text(row->nameKey), r.quantity) : c.Text(row->nameKey));
        }
    }
    return out;
}

void ServiceEvent(HudContext& c, const proto::MallEvent& event) {
    if (const auto* a = std::get_if<proto::ServiceAnswer>(&event)) {
        if (a->code == 0) {
            if (a->request == proto::ServiceRequest::Skin) Line(c, c.Text("UIIT_MSG_CHAR_SKIN_CHANGE_SUCCESS"));
            return;
        }
        if (const char* key = ServiceKey(a->code)) Line(c, c.Text(key));
        return;
    }
    if (const auto* cube = std::get_if<proto::CubeContents>(&event)) {
        c.cubes[cube->cube] = cube->items;
        return;
    }
}

} // namespace sr::client
