// Magic POP on the client: the machine's window (1.188's CIFGhaCha, ginterface 140,
// resinfo\ifghacha.txt sections Create and Inventory) with its selection window on top (CIFGhachaSelectWnd, section
// GhachaSelect, its controls in ifghachaselectwnd.txt), the prize strip, the card's slot, Play and the roll; the play's
// and the exchange's answers, the announce's line and the coupon's tooltip line.
//
// The states: 0 no prize chosen "Choose item of desire.", 1 no card "Register Magic POP card.", 2
// "Click PLAY button." (Play enabled). Play: the reels turn every 100 ms, the request goes after 4 s (the server answers
// at once), the centre's effect runs 41 steps of 100 ms; a won play shows the success strip.

#include <algorithm>
#include <format>
#include <map>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/game/MagicPop.h"
#include "sr/game/MallProtocol.h"

namespace sr::client {

namespace proto = game::proto;
namespace pop = game::pop;

namespace {

constexpr int kWindow = 140;
// The main window (ifghacha.txt Create): Play, the reels, the centre's effect and the play's / win's effect, the message
// line, the prize strip, the card's slot, the strip's page.
constexpr int kPlay = 10, kReelLeft = 20, kReelRight = 21, kCentre = 50, kEffect = 51, kMessage = 36, kFirstPrize = 100, kCard = 112, kPage = 700;
constexpr int kPrizes = 12;
// Section Inventory: the money box (its gold) and the bag's 32 slots made in code (ids 200 + i, 4 a row from 494,91, 36
// apart); the bag's page tabs (the original's 500 + i; ours 520 + i, 500 being a tile's id of Create).
constexpr int kMoney = 40, kFirstBag = 200, kBagSlots = 32, kFirstTab = 520;
// The selection window (section GhachaSelect, id 600): the three combos and Start.
constexpr int kSelect = 600, kBig = 41, kMiddle = 42, kDegree = 43, kStart = 10;

constexpr double kStep = 0.1;     // the timers' 100 ms
constexpr double kSendAt = 4.0;   // timer 2: the request after 4 s
constexpr int kCentreSteps = 41;  // timer 3's steps

void Put(ui::Widget* w, const std::string& text) {
    if (w != nullptr && w->text() != text) w->SetText(text);
}

std::string Unescaped(std::string s) {
    for (std::size_t at = 0; (at = s.find("\\n", at)) != std::string::npos;) s.replace(at, 2, "\n");
    return s;
}

// The prize's name and count line ("[%s][%d]") of an item.
std::string PrizeLine(const HudContext& c, const std::string& key, std::uint32_t item, int count) {
    const assets::ItemRow* row = c.items != nullptr ? c.items->Find(item) : nullptr;
    return FormatText(c.Text(key), {row != nullptr ? c.Text(row->nameKey) : std::to_string(item), std::to_string(count)});
}

// The selection window: big category, middle category, degree, Start.
class SelectWindow : public ui::FrameBorder {
public:
    SelectWindow(const assets::UiLayoutNode& node, HudContext& c) : FrameBorder(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        ui::FrameBorder::Created(ui);
        if (const assets::UiLayout* l = ui.Layout("resinfo\\ifghachaselectwnd.txt")) {
            for (const auto& s : l->sections) {
                if (s.name != "Create") continue;
                for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) Add(ui.Make(*it));
            }
        }
        FillBig();
    }

    // The tree's big categories, in its order.
    void FillBig() {
        bigs_.clear();
        if (c_.gacha != nullptr) {
            for (const assets::GachaTreeRow& r : c_.gacha->tree()) {
                if (std::find(bigs_.begin(), bigs_.end(), r.bigText) == bigs_.end()) bigs_.push_back(r.bigText);
            }
        }
        if (auto* box = ui::As<ui::ComboBox>(Child(kBig))) {
            box->items = {c_.Text("UIIT_CTL_WARENETWORK_SCAN_SELECT")};
            for (const std::string& b : bigs_) box->items.push_back(c_.Text(b));
            box->Select(0);
        }
        FillMiddle();
    }
    void FillMiddle() {
        middles_.clear();
        auto* big = ui::As<ui::ComboBox>(Child(kBig));
        auto* box = ui::As<ui::ComboBox>(Child(kMiddle));
        if (box == nullptr) return;
        const int b = big != nullptr ? big->selected - 1 : -1;
        if (b < 0 || c_.gacha == nullptr) {
            box->items = {c_.Text("UIIT_CTL_WARENETWORK_SCAN_END")};
        } else {
            box->items = {c_.Text("UIIT_CTL_WARENETWORK_SCAN_SELECT")};
            for (const assets::GachaTreeRow& r : c_.gacha->tree()) {
                if (r.bigText != bigs_[std::size_t(b)]) continue;
                middles_.push_back(&r);
                box->items.push_back(c_.Text(r.midText));
            }
        }
        box->Select(0);
        FillDegree();
    }
    void FillDegree() {
        auto* box = ui::As<ui::ComboBox>(Child(kDegree));
        if (box == nullptr) return;
        const assets::GachaTreeRow* node = Node();
        box->items = {c_.Text(node != nullptr && node->maxDegree > 0 ? "UIIT_CTL_WARENETWORK_SCAN_SELECT" : "UIIT_CTL_WARENETWORK_SCAN_END")};
        if (node != nullptr) {
            for (int d = 1; d <= node->maxDegree; ++d) box->items.push_back(FormatText("%d %s", {std::to_string(d), c_.Text("UIIT_STT_CLASS")}));
        }
        box->Select(0);
    }
    const assets::GachaTreeRow* Node() const {
        const auto* box = ui::As<ui::ComboBox>(const_cast<SelectWindow*>(this)->Child(kMiddle));
        const int m = box != nullptr ? box->selected - 1 : -1;
        return m >= 0 && std::size_t(m) < middles_.size() ? middles_[std::size_t(m)] : nullptr;
    }
    int Degree() {
        const auto* box = ui::As<ui::ComboBox>(Child(kDegree));
        return box != nullptr && box->selected > 0 ? box->selected : 0;
    }
    // The script's choice: the node and degree an item falls under (false: none).
    bool ChooseFor(const assets::ItemRow& row) {
        auto* big = ui::As<ui::ComboBox>(Child(kBig));
        if (big == nullptr || c_.gacha == nullptr) return false;
        for (std::size_t b = 0; b < bigs_.size(); ++b) {
            big->Select(int(b) + 1);
            FillMiddle();
            for (std::size_t m = 0; m < middles_.size(); ++m) {
                if (!pop::InFilter(row, middles_[m]->typeId, 0)) continue;
                if (auto* mid = ui::As<ui::ComboBox>(Child(kMiddle))) mid->Select(int(m) + 1);
                FillDegree();
                if (middles_[m]->maxDegree > 0) {
                    if (auto* deg = ui::As<ui::ComboBox>(Child(kDegree))) deg->Select(std::clamp(pop::Degree(row), 0, middles_[m]->maxDegree));
                }
                return true;
            }
        }
        return false;
    }

    bool OnNotice(const ui::Notice& notice) override {
        if (notice.kind == ui::Notice::Kind::ComboPicked && notice.control == kBig) {
            FillMiddle();
            return true;
        }
        if (notice.kind == ui::Notice::Kind::ComboPicked && notice.control == kMiddle) {
            FillDegree();
            return true;
        }
        if (notice.kind == ui::Notice::Kind::Clicked && notice.control == kStart) {
            if (onStart && Node() != nullptr) onStart(*Node(), Degree());
            return true;
        }
        return ui::FrameBorder::OnNotice(notice);
    }

    std::function<void(const assets::GachaTreeRow&, int degree)> onStart;

private:
    HudContext& c_;
    std::vector<std::string> bigs_;
    std::vector<const assets::GachaTreeRow*> middles_;
};

// The chosen prize's mark over its slot: alcm_effect_prepare.ddj, 4 x 4 frames of 64 x 64 (the prize slot's selected look,
// its flag and case 4; the pace ours, 100 ms).
class Pulse : public ui::Widget {
public:
    using Widget::Widget;
    std::string texture = "interface\\alchemy\\alcm_effect_prepare.ddj";
    int columns = 4, rows = 4, frames = 16;
    float cellW = 0.25f, cellH = 0.25f;
    double started = 0;
    bool once = false; // the success strip: its frames once, then hidden
    bool additive = true; // its black is nothing (the effects' sheets are drawn added)
    void Draw(ui::Frame& frame) override {
        int f = int((frame.time - started) / kStep);
        if (once && f >= frames) {
            visible = false;
            return;
        }
        f %= frames;
        const ui::Vec2 uv0{float(f % columns) * cellW, float(f / columns) * cellH};
        if (additive) frame.paint.PushAdditive();
        frame.paint.Image(texture, ScreenRect(), uv0, {uv0.x + cellW, uv0.y + cellH}, ui::WithAlpha(ui::kWhite, Opacity()));
        if (additive) frame.paint.PopAdditive();
    }
};

class PopWindow : public ui::MainFrame {
public:
    PopWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        ui_ = &ui;
        if (const assets::UiLayout* l = ui.Layout("resinfo\\ifghacha.txt")) {
            for (const auto& s : l->sections) {
                if (s.name != "Inventory") continue;
                for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) Add(ui.Make(*it));
            }
        }
        // The bag's 32 slots and the page tabs.
        for (int i = 0; i < kBagSlots; ++i) {
            assets::UiLayoutNode n;
            n.type = "CIFSlotWithHelp";
            n.id = kFirstBag + i;
            n.rect = {494.0f + 36.0f * float(i % 4), 91.0f + 36.0f * float(i / 4), 32.0f, 32.0f};
            if (auto* slot = ui::As<ui::Slot>(Add(ui.Make(n)))) {
                slot->tooltipLines = [this, i] {
                    const game::ItemInstance* item = BagItem(i);
                    const assets::ItemRow* row = item != nullptr && c_.items ? c_.items->Find(item->refId) : nullptr;
                    return row ? ItemTooltip(*row, *item, c_) : std::vector<ui::TooltipLine>{};
                };
            }
        }
        for (int i = 0; i < 3; ++i) {
            assets::UiLayoutNode n;
            n.type = "CIFSelectableArea";
            n.id = kFirstTab + i;
            n.rect = {475.0f + 62.0f * float(i), 55.0f, 60.0f, 24.0f};
            n.ddj = "interface\\ifcommon\\com_tab_off.ddj";
            n.hAlign = 1;
            n.vAlign = 1;
            Add(ui.Make(n));
        }
        // The prize strip's tooltips; the card's.
        for (int k = 0; k < kPrizes; ++k) {
            if (auto* slot = ui::As<ui::Slot>(Child(kFirstPrize + k))) {
                slot->pickable = false;
                slot->tooltipLines = [this, k] {
                    const assets::GachaEntry* e = StripEntry(k);
                    const assets::ItemRow* row = e != nullptr && c_.items ? c_.items->Find(e->item) : nullptr;
                    if (row == nullptr) return std::vector<ui::TooltipLine>{};
                    game::ItemInstance item;
                    item.refId = e->item;
                    item.count = std::uint16_t(std::max(1, e->count));
                    return ItemTooltip(*row, item, c_);
                };
            }
        }
        if (auto* card = ui::As<ui::Slot>(Child(kCard))) {
            card->pickable = false;
            card->tooltipLines = [this] {
                const game::ItemInstance* item = cardPlace_ >= 0 && c_.state ? c_.state->inventory.At(std::uint8_t(cardPlace_)) : nullptr;
                const assets::ItemRow* row = item != nullptr && c_.items ? c_.items->Find(item->refId) : nullptr;
                return row ? ItemTooltip(*row, *item, c_) : std::vector<ui::TooltipLine>{};
            };
        }
        if (ui::Widget* effect = Child(kEffect)) effect->visible = false; // hidden at Create
        ResetReels(); // gacha_roll_original.ddj on both at Create
        if (ui::Widget* play = Child(kPlay)) play->enabled = false;     // disabled at Create
        if (auto* m = ui::As<ui::Static>(Child(kMessage))) m->textStyle |= ui::kTextShadow;
        // The play's animation and the win's strip over 51 (ours: a widget of its own in its place).
        if (ui::Widget* effect = Child(kEffect)) {
            assets::UiLayoutNode n;
            n.type = "CIFStatic";
            n.id = 800;
            n.rect = {effect->rect.x, effect->rect.y, effect->rect.w, effect->rect.h};
            effect_ = static_cast<Pulse*>(Add(std::make_unique<Pulse>(n)));
            effect_->visible = false;
        }
        assets::UiLayoutNode mark;
        mark.type = "CIFStatic";
        mark.id = 801;
        mark.rect = {0, 0, 48, 48};
        mark_ = static_cast<Pulse*>(Add(std::make_unique<Pulse>(mark)));
        mark_->visible = false;
        // The selection window on top, centred over the strip (Create shows it).
        if (const assets::UiLayout* l = ui.Layout("resinfo\\ifghacha.txt")) {
            for (const auto& s : l->sections) {
                if (s.name != "GhachaSelect" || s.nodes.empty()) continue;
                select_ = dynamic_cast<SelectWindow*>(Add(ui.Make(s.nodes.front())));
            }
        }
        if (select_ != nullptr) {
            select_->rect.x = (rect.w - select_->rect.w) * 0.5f;
            select_->rect.y = (rect.h - select_->rect.h) * 0.5f;
            select_->onStart = [this](const assets::GachaTreeRow& node, int degree) { Start(node.typeId, degree); };
        }
        // A card dropped on the window (its own bag's or the inventory's): onto the card's slot.
        onDrop = [this](ui::Widget* source, ui::Widget*) {
            const int place = PlaceOf(source);
            if (place < 0 || !Register(place)) return false;
            resultShown_ = false;
            return true;
        };
        Put(Child(kMessage), c_.Text("UIIT_MSG_GACHA_CHOICE"));
    }

    // Opened at the machine `npc`: the selection first, nothing chosen.
    void Open(std::uint32_t npc) {
        npc_ = npc;
        filtered_ = false;
        strip_.clear();
        chosen_ = -1;
        cardPlace_ = -1;
        rolling_ = false;
        page_ = 0;
        if (select_ != nullptr) {
            select_->visible = true;
            select_->FillBig();
        }
    }

    // Start: the strip filtered by the node's type and the degree (0 none).
    void Start(const std::array<int, 4>& typeId, int degree) {
        strip_.clear();
        const assets::GachaMachine* machine = Machine();
        if (machine != nullptr && c_.gacha != nullptr && c_.items != nullptr) {
            for (const assets::GachaEntry& e : c_.gacha->entries()) {
                const assets::ItemRow* row = c_.items->Find(e.item);
                if (e.set == machine->selection && row != nullptr && pop::InFilter(*row, typeId, degree)) strip_.push_back(&e);
            }
        }
        filtered_ = true;
        stripPage_ = 0;
        chosen_ = -1;
        if (select_ != nullptr) select_->visible = false;
    }

    const assets::GachaMachine* Machine() const {
        const game::WorldView::Entity* e = c_.world != nullptr ? c_.world->Find(npc_) : nullptr;
        if (c_.gacha == nullptr) return nullptr;
        // The machine's row (gachanpcmap.txt) by its model; ours with no view of it (a gallery): the first machine.
        if (e == nullptr) return c_.gacha->machines().empty() ? nullptr : &c_.gacha->machines().front();
        return c_.gacha->Machine(e->look.model);
    }
    const assets::GachaEntry* StripEntry(int k) const {
        const std::size_t i = std::size_t(stripPage_ * kPrizes + k);
        return i < strip_.size() ? strip_[i] : nullptr;
    }
    const game::ItemInstance* BagItem(int i) const {
        return c_.state != nullptr ? c_.state->inventory.At(std::uint8_t(game::slot::kWorn + page_ * kBagSlots + i)) : nullptr;
    }
    int PlaceOf(const ui::Widget* source) const {
        if (source != nullptr && source->parent() == this && source->id() >= kFirstBag && source->id() < kFirstBag + kBagSlots) {
            return game::slot::kWorn + page_ * kBagSlots + source->id() - kFirstBag;
        }
        return InventoryPlaceOf(source);
    }

    // A card on the card's slot: with a prize chosen, never while rolling, only a card.
    bool Register(int place) {
        if (rolling_ || chosen_ < 0 || c_.state == nullptr) return false;
        const game::ItemInstance* item = c_.state->inventory.At(std::uint8_t(place));
        const assets::ItemRow* row = item != nullptr && c_.items ? c_.items->Find(item->refId) : nullptr;
        if (row == nullptr || !pop::IsCard(*row)) return false;
        cardPlace_ = place;
        return true;
    }
    // A prize of the strip chosen by its GachaID (scripts: the strip's page turned to it).
    bool Choose(std::uint32_t entry) {
        for (std::size_t i = 0; i < strip_.size(); ++i) {
            if (strip_[i]->id != entry) continue;
            stripPage_ = int(i) / kPrizes;
            chosen_ = int(i);
            return true;
        }
        return false;
    }
    bool ChooseFilterFor(std::uint32_t entry, bool pick) {
        const assets::GachaEntry* e = c_.gacha != nullptr ? c_.gacha->Find(entry) : nullptr;
        const assets::ItemRow* row = e != nullptr && c_.items ? c_.items->Find(e->item) : nullptr;
        if (row == nullptr || select_ == nullptr || !select_->ChooseFor(*row)) return false;
        if (select_->Node() == nullptr) return false;
        Start(select_->Node()->typeId, select_->Degree());
        return !pick || Choose(entry);
    }

    int State() const { return chosen_ < 0 ? 0 : cardPlace_ < 0 ? 1 : 2; }

    // Play: rolling; the request after 4 s.
    bool Play(double now) {
        if (rolling_ || State() != 2 || chosen_ < 0) return false;
        rolling_ = true;
        sent_ = false;
        answered_ = false;
        won_ = false;
        resultShown_ = false;
        rollStart_ = -1.0; // the window's own clock, at its next frame
        (void)now;
        if (effect_ != nullptr) {
            effect_->texture = "interface\\alchemy\\alcm_effect_prepare.ddj";
            effect_->columns = 4;
            effect_->rows = 4;
            effect_->frames = 16;
            effect_->cellW = effect_->cellH = 0.25f;
            effect_->once = false;
            effect_->visible = true;
        }
        if (ui_ != nullptr) {
            ui_->Sound("SND_GACHA_MOVE");
            ui_->Sound("SND_GACHA_TURN");
        }
        return true;
    }

    void Played(const proto::PopPlayed& a, double now) {
        answered_ = true;
        if (a.code != 0) {
            // As the original: the reels go on rolling; only code 3 says so ("invalid target"), the others nothing.
            if (a.code == 3) Put(Child(kMessage), c_.Text("UIIT_MSG_STRGERR_INVALID_TARGET"));
            return;
        }
        won_ = a.won != 0;
        prize_ = a.prize;
        prizeCount_ = a.count;
        (void)now;
    }

    void Update(ui::Frame& frame) override {
        ui::MainFrame::Update(frame);
        const double now = frame.time;
        // The card gone from its place (used, moved): unregistered, unless it is the coupon of the play.
        if (cardPlace_ >= 0 && c_.state != nullptr && !rolling_) {
            const game::ItemInstance* item = c_.state->inventory.At(std::uint8_t(cardPlace_));
            const assets::ItemRow* row = item != nullptr && c_.items ? c_.items->Find(item->refId) : nullptr;
            if (row == nullptr || (!pop::IsCard(*row) && !pop::IsCoupon(*row))) cardPlace_ = -1;
        }
        // The prize strip (twelve a page) and the chosen one's mark.
        for (int k = 0; k < kPrizes; ++k) {
            auto* slot = ui::As<ui::Slot>(Child(kFirstPrize + k));
            if (slot == nullptr) continue;
            const assets::GachaEntry* e = StripEntry(k);
            game::ItemInstance item;
            if (e != nullptr) {
                item.refId = e->item;
                item.count = std::uint16_t(std::max(1, e->count));
            }
            ShowItem(*slot, e != nullptr ? &item : nullptr, c_);
            if (mark_ != nullptr && chosen_ == stripPage_ * kPrizes + k) {
                mark_->rect = {slot->rect.x - 16.0f, slot->rect.y - 16.0f, 64.0f, 64.0f};
                mark_->visible = true;
            }
        }
        if (mark_ != nullptr && (chosen_ < 0 || chosen_ / kPrizes != stripPage_)) mark_->visible = false;
        if (auto* spin = ui::As<ui::Spin>(Child(kPage))) {
            spin->max = std::max(0, int((strip_.size() + kPrizes - 1) / kPrizes) - 1);
            if (spin->value != stripPage_) stripPage_ = std::clamp(spin->value, 0, spin->max);
        }
        // The card's slot shows the bag's item there (a coupon once played).
        if (auto* card = ui::As<ui::Slot>(Child(kCard))) {
            const game::ItemInstance* item = cardPlace_ >= 0 && c_.state ? c_.state->inventory.At(std::uint8_t(cardPlace_)) : nullptr;
            ShowItem(*card, item, c_);
        }
        // The window's own bag: its page, the tabs, the gold.
        const int bagSize = c_.state != nullptr ? int(c_.state->inventory.size) - game::slot::kWorn : 0;
        const int pages = std::max(1, (bagSize + kBagSlots - 1) / kBagSlots);
        page_ = std::min(page_, pages - 1);
        static constexpr const char* kTabText[3] = {"UIIT_CTL_BELOINGING", "UIIT_STT_INVENTORY_EXTENSION_TEB", "UIIT_STT_INVENTORY_EXTENSION_TEB2"};
        for (int i = 0; i < 3; ++i) {
            auto* tab = ui::As<ui::SelectableArea>(Child(kFirstTab + i));
            if (tab == nullptr) continue;
            tab->visible = i < pages;
            tab->selected = i == page_;
            tab->texture = i == page_ ? "interface\\ifcommon\\com_tab_on.ddj" : "interface\\ifcommon\\com_tab_off.ddj";
            Put(tab, c_.Text(kTabText[i]));
        }
        for (int i = 0; i < kBagSlots; ++i) {
            auto* slot = ui::As<ui::Slot>(Child(kFirstBag + i));
            if (slot == nullptr) continue;
            const bool there = page_ * kBagSlots + i < bagSize;
            slot->usable = there;
            ShowItem(*slot, there ? BagItem(i) : nullptr, c_);
            slot->overlay = there && game::slot::kWorn + page_ * kBagSlots + i == cardPlace_ ? "interface\\ifcommon\\com_item_exchange.ddj" : "";
        }
        if (c_.state != nullptr) {
            if (auto* money = ui::As<ui::Static>(Child(kMoney))) {
                Put(money, GoldText(c_.state->inventory.gold));
                money->textColor = GoldColor(c_.state->inventory.gold);
            }
        }
        // The roll.
        if (rolling_ && rollStart_ < 0.0) {
            rollStart_ = now;
            if (effect_ != nullptr) effect_->started = now;
        }
        if (rolling_) {
            const double t = now - rollStart_;
            if (!sent_) {
                // Timer 1: both reels gacha_roll%04d.ddj, 0000..0020 by two, every 100 ms.
                const int f = (int(t / kStep) % 11) * 2;
                for (const int id : {kReelLeft, kReelRight}) {
                    if (auto* reel = ui::As<ui::Static>(Child(id))) reel->texture = std::format("interface\\game\\gacha_roll{:04d}.ddj", f);
                }
            }
            if (!sent_ && t >= kSendAt) {
                // Timer 2: the request; the reels back.
                sent_ = true;
                ResetReels();
                const assets::GachaEntry* e = chosen_ >= 0 && std::size_t(chosen_) < strip_.size() ? strip_[std::size_t(chosen_)] : nullptr;
                if (e != nullptr && c_.popRequest) {
                    c_.popRequest(proto::PopPlay::kId, proto::Encode(proto::PopPlay{npc_, e->id, std::uint8_t(cardPlace_)}));
                }
            }
            // Timer 3: the centre's ga_effect_%02d, held through steps 7..15 and 23..35.
            const int step = int(t / kStep);
            int effect = 1;
            for (int s = 1; s <= std::min(step, kCentreSteps); ++s) {
                if (!((s >= 7 && s <= 15) || (s >= 23 && s <= 35))) ++effect;
            }
            if (auto* centre = ui::As<ui::Static>(Child(kCentre))) centre->texture = std::format("interface\\game\\ga_effect_{:02d}.ddj", std::clamp(effect, 1, 19));
            if (step >= kCentreSteps && sent_ && answered_) Finish(now);
        }
        // The state's line and Play, unless a result is shown.
        if (!resultShown_) {
            static constexpr const char* kLines[3] = {"UIIT_MSG_GACHA_CHOICE", "UIIT_MSG_GACHA_ENTRY", "UIIT_MSG_GACHA_BUTTON_CLICK"};
            Put(Child(kMessage), c_.Text(kLines[State()]));
        }
        if (ui::Widget* play = Child(kPlay)) play->enabled = !rolling_ && State() == 2;
    }

    void Finish(double now) {
        rolling_ = false;
        if (auto* centre = ui::As<ui::Static>(Child(kCentre))) centre->texture = "interface\\game\\ga_effect_01.ddj";
        if (effect_ != nullptr) effect_->visible = false;
        resultShown_ = true;
        if (won_) {
            // The success strip: ga_effect_sucess.ddj, 15 frames of 64 x 81 four to a row (256 x 512), 100 ms each.
            if (effect_ != nullptr) {
                effect_->texture = "interface\\game\\ga_effect_sucess.ddj";
                effect_->columns = 4;
                effect_->frames = 15;
                effect_->cellW = 64.0f / 256.0f;
                effect_->cellH = 81.0f / 512.0f;
                effect_->once = true;
                effect_->started = now;
                effect_->visible = true;
            }
            Put(Child(kMessage), PrizeLine(c_, "UIIT_MSG_GACHA_WIN", prize_, prizeCount_));
            if (ui_ != nullptr) ui_->Sound("SND_GACHA_WIN");
        } else {
            Put(Child(kMessage), Unescaped(c_.Text("UIIT_MSG_GACHA_LOSE")));
            if (ui_ != nullptr) ui_->Sound("SND_GACHA_END");
        }
        // The coupon is no card: the slot empties for the next card (ours: the original keeps showing the coupon).
        cardPlace_ = -1;
    }

    void ResetReels() {
        for (const int id : {kReelLeft, kReelRight}) {
            if (auto* reel = ui::As<ui::Static>(Child(id))) reel->texture = "interface\\game\\gacha_roll_original.ddj";
        }
    }

    bool OnNotice(const ui::Notice& notice) override {
        const bool mine = notice.source != nullptr && notice.source->parent() == this;
        if (rolling_ && mine && notice.control == 2) return true; // no closing while rolling
        if (!mine || notice.kind != ui::Notice::Kind::Clicked) return ui::MainFrame::OnNotice(notice);
        if (notice.control == kPlay) {
            Play(lastTime_);
            return true;
        }
        if (notice.control >= kFirstPrize && notice.control < kFirstPrize + kPrizes && !rolling_) {
            // A prize chosen; a new choice clears the result's line.
            if (StripEntry(notice.control - kFirstPrize) != nullptr) {
                chosen_ = stripPage_ * kPrizes + notice.control - kFirstPrize;
                resultShown_ = false;
            }
            return true;
        }
        if (notice.control >= kFirstTab && notice.control < kFirstTab + 3) {
            page_ = notice.control - kFirstTab;
            return true;
        }
        if (notice.control == kCard && !rolling_) {
            cardPlace_ = -1; // a press takes the card back out
            return true;
        }
        // A card of the window's bag is picked up and dropped on the card's slot (onDrop), as in the original: a press
        // alone registers nothing.
        return ui::MainFrame::OnNotice(notice);
    }

    void Draw(ui::Frame& frame) override {
        lastTime_ = frame.time;
        ui::MainFrame::Draw(frame);
    }

    bool Rolling() const { return rolling_; }
    double lastTime_ = 0;

private:
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    SelectWindow* select_ = nullptr;
    Pulse* effect_ = nullptr;
    Pulse* mark_ = nullptr;
    std::uint32_t npc_ = 0;
    bool filtered_ = false;
    std::vector<const assets::GachaEntry*> strip_;
    int stripPage_ = 0, chosen_ = -1, cardPlace_ = -1, page_ = 0;
    bool rolling_ = false, sent_ = false, answered_ = false, won_ = false, resultShown_ = false;
    double rollStart_ = 0;
    std::uint32_t prize_ = 0;
    int prizeCount_ = 0;
};

PopWindow* Shown(HudContext& c) {
    return c.hud != nullptr && c.hud->Shown(kWindow) ? dynamic_cast<PopWindow*>(c.hud->Show(kWindow)) : nullptr;
}

} // namespace

void RegisterMagicPopWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CIFGhaCha", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<PopWindow>(n, *c); });
    ui.Register("CIFGhachaSelectWnd", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<SelectWindow>(n, *c); });
}

void OpenMagicPop(HudContext& c, std::uint32_t npc) {
    if (c.hud == nullptr) return;
    if (auto* w = dynamic_cast<PopWindow*>(c.hud->Show(kWindow))) w->Open(npc);
}

bool MagicPopHasCoupon(const HudContext& c) {
    if (c.state == nullptr || c.items == nullptr) return false;
    for (const auto& [place, item] : c.state->inventory.places) {
        const assets::ItemRow* row = place >= game::slot::kWorn ? c.items->Find(item.refId) : nullptr;
        if (row != nullptr && pop::IsCoupon(*row)) return true;
    }
    return false;
}

void MagicPopPlayed(HudContext& c, const proto::PopPlayed& a) {
    if (PopWindow* w = Shown(c)) {
        w->Played(a, c.time);
    } else if (a.code != 0 && c.systemLine) {
        c.systemLine(c.Text("UIIT_MSG_STRGERR_TRADE_FAILURE"));
    }
}

void MagicPopExchanged(HudContext& c, const proto::PopExchanged& a) {
    if (c.state == nullptr) return;
    if (a.code != 0) {
        // Ours: the original prints nothing for an exchange's failure.
        c.state->Post(game::GameMessage::Kind::General, c.Text("UIIT_MSG_STRGERR_TRADE_FAILURE"), c.time);
        return;
    }
    // "Has been changed to [%s][%d]." a coupon (the original's from each 0x3040).
    for (const proto::ItemRecord& r : a.items) {
        c.state->Post(game::GameMessage::Kind::General, PrizeLine(c, "UIIT_MSG_GACHA_EXCHANGE", r.item, std::max<int>(1, r.quantity)), c.time);
    }
    if (!a.items.empty() && c.hud != nullptr && c.hud->interface() != nullptr) c.hud->interface()->Sound("SND_GACHA_CHANGE");
}

void MagicPopAnnounced(HudContext& c, const proto::PopAnnounce& a) {
    if (c.state != nullptr) c.state->Post(game::GameMessage::Kind::General, PrizeLine(c, "UIIT_MSG_GACHA_ANNOUNCE", a.item, a.count), c.time);
}

std::vector<std::string> MagicPopCouponLine(const HudContext& c, const assets::ItemRow& row, const game::ItemInstance& item) {
    if (!pop::IsCoupon(row)) return {};
    const auto params = pop::ParamsOf(item);
    const auto prize = pop::ReadCoupon(params);
    if (!prize) return {};
    return {PrizeLine(c, "UIIT_STT_GACHA_WIN_ITEM", prize->item, prize->count)};
}

bool MagicPopChoose(HudContext& c, std::uint32_t entry, bool pick) {
    PopWindow* w = Shown(c);
    return w != nullptr && w->ChooseFilterFor(entry, pick);
}

bool MagicPopCard(HudContext& c, int place) {
    PopWindow* w = Shown(c);
    return w != nullptr && w->Register(place);
}

bool MagicPopPlay(HudContext& c) {
    PopWindow* w = Shown(c);
    return w != nullptr && w->Play(c.time);
}

bool MagicPopRolling(HudContext& c) {
    PopWindow* w = Shown(c);
    return w != nullptr && w->Rolling();
}

} // namespace sr::client
