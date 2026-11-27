// The Open Market's windows: 1.188's CNIFOpenMarket from res_ui\open_market_main.2dt, its
// Search / Register / Settle pages behind the side buttons, and the registration's price box
// (CNIFOpenMarketRegistrationPriceMsgBox, res_ui\open_market_1_1.2dt). The confirmations ask in the HUD's confirm box with
// 1.188's words (ours: the original's own boxes, open_market_1_2.2dt and 1_3.2dt, are its look of the same questions).

#include <algorithm>
#include <format>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/assets/StallNetworkData.h"
#include "sr/game/OpenMarketView.h"

namespace sr::client {

namespace proto = game::proto;
namespace om = game::openmarket;

namespace {

constexpr const char* kLayout = "res_ui\\open_market_main.2dt";
constexpr const char* kPriceLayout = "res_ui\\open_market_1_1.2dt";
// The layout's ids: the side buttons, the title, the pages.
constexpr int kSearchButton = 94, kRegisterButton = 95, kSettleButton = 96, kTitle = 10;
// The search page: its strip (5) with the combos and the seller, the results (120), the pages, the gold, Whisper, Buy.
constexpr int kSearchStrip = 5, kScan = 50, kClassCombo = 80, kTypeCombo = 81, kDegreeCombo = 82, kSellerBox = 70, kSellerEdit = 24,
              kSearchRows = 120, kSearchPages = 60, kSearchGold = 22, kWhisper = 17, kBuy = 18;
// The register page: its strip (6) with the deposit (70 -> 25) and the gold (71 -> 26), the rows (121), Register, Cancel.
constexpr int kRegisterStrip = 6, kDepositBox = 70, kDeposit = 25, kGoldBox = 71, kRegisterGold = 26, kRegisterRows = 121,
              kRegisterPages = 61, kRegister = 53, kCancel = 54;
// The settle page: its strip (7) with the sums (72..75 -> 20) and Settle (55), the rows (140).
constexpr int kSettleStrip = 7, kSettle = 55, kSettleRows = 140, kSettlePages = 62;
// Inside a row: the number, the level, the name, the count, the price, the time (the register and settle rows), the item.
constexpr int kRowNumber = 10, kRowLevel = 11, kRowName = 12, kRowCount = 13, kRowPrice = 14, kRowTime = 15, kRowItem = 20, kRowTip = 21;
constexpr int kWindowId = 190, kPriceBoxId = 194;
constexpr const char* kConfirmLayout = "res_ui\\open_market_1_3.2dt";
constexpr int kConfirmBoxId = 196;
constexpr const char* kBuyLayout = "res_ui\\open_market_1_4.2dt";
constexpr int kBuyBoxId = 199;
constexpr const char* kAlarmLayout = "res_ui\\open_market_2.2dt";
constexpr int kAlarmWindowId = 197, kAlarmGuideId = 198;

template <typename T>
void Send(HudContext& c, const T& message) {
    if (c.openMarketRequest) c.openMarketRequest(T::kId, proto::Encode(message));
}

// The time left in its largest unit ("2Day", "5Hour", "12Minute"): the Period column holds one.
std::string TimeLeft(const HudContext& c, double seconds) {
    const long long s = std::max(0LL, (long long)seconds);
    if (s >= 86400) return std::format("{}{}", s / 86400, c.Text("PARAM_DAY"));
    if (s >= 3600) return std::format("{}{}", s / 3600, c.Text("PARAM_HOUR"));
    return std::format("{}{}", std::max(1LL, s / 60), c.Text("PARAM_MINUTE"));
}

// The price box: the price of the item chosen for registration, its count shown (a stack goes whole: the original's
// server refuses a part of one, 0x7018, so the box's count field is shown, not typed in).
class PriceBox : public ui::MainFrame {
public:
    PriceBox(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}
    std::function<void(std::uint64_t price)> answer;
    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        const auto* records = c_.layout2dt ? c_.layout2dt(kPriceLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        Build2dt(ui, *this, *records, {});
        if (auto* price = ui::As<ui::Edit>(Find(16))) {
            price->numeric = true;
            price->limit = 10;
        }
        if (ui::Widget* title = Find(20)) title->SetText(c_.Text("UIIT_STT_CONFIRM_BOX"));
    }
    void Ask(const std::string& name, const std::string& icon, std::uint32_t count) {
        if (ui::Widget* w = Find(15)) w->SetText(name);
        if (auto* w = ui::As<ui::Static>(Find(10))) w->texture = icon;
        if (auto* price = ui::As<ui::Edit>(Find(16))) price->SetValue({});
        if (auto* n = ui::As<ui::Edit>(Find(17))) {
            n->SetValue(std::to_string(count));
            n->enabled = false;
        }
        visible = true;
    }
    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && (n.control == 3 || n.control == 4)) {
            const auto* e = ui::As<ui::Edit>(Find(16));
            Answer(n.control == 3, e != nullptr ? std::strtoull(e->text().c_str(), nullptr, 10) : 0);
            return true;
        }
        return MainFrame::OnNotice(n);
    }
    void Answer(bool confirmed, std::uint64_t price) {
        visible = false;
        if (confirmed && answer) answer(price);
    }

private:
    HudContext& c_;
};

// The registration's question (CNIFOpenMarketRegistrationConfirmationMsgBox, open_market_1_3.2dt): its second line the
// deposit ("The deposit comes to [%d] Gold. ..."), its fourth the warning beside a check box ticked when it opens; Confirm
// (23) registers while the box is ticked, else "UIIT_STT_OPEN_MARKET_CHECKBOX_ERROR"; either way it closes, as Cancel (24).
class RegisterConfirmBox : public ui::MainFrame {
public:
    RegisterConfirmBox(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}
    std::function<void()> confirmed;
    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
        const auto* records = c_.layout2dt ? c_.layout2dt(kConfirmLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        Build2dt(ui, *this, *records, {});
    }
    void Ask(std::uint64_t deposit) {
        if (ui::Widget* w = Find(26)) w->SetText(FormatText(c_.Text("UIIT_MSG_OPEN_MARKET_ENTER"), {std::to_string(deposit)}));
        if (ui::Widget* w = Find(28)) w->SetText(c_.Text("UIIT_MSG_OPEN_MARKET_WARNING"));
        if (auto* check = ui::As<ui::CheckBox>(Find(30))) check->checked = true;
        visible = true;
    }
    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && (n.control == 23 || n.control == 24)) {
            if (n.control == 23) {
                const auto* check = ui::As<ui::CheckBox>(Find(30));
                if (check != nullptr && check->checked) {
                    if (confirmed) confirmed();
                } else if (c_.systemLine) {
                    c_.systemLine(c_.Text("UIIT_STT_OPEN_MARKET_CHECKBOX_ERROR"));
                }
            }
            if (ui_ != nullptr) ui_->Close(this);
            return true;
        }
        return MainFrame::OnNotice(n);
    }

private:
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
};

// A search row's purchase asked (CNIFOpenMarketSearchPriceMsgBox, open_market_1_4.2dt): the item's icon (10), its name
// (15), its count (17) and price (16) shown, not typed; Confirm (3) buys, Cancel (4) closes.
class BuyBox : public ui::MainFrame {
public:
    BuyBox(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}
    std::function<void()> confirmed;
    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
        const auto* records = c_.layout2dt ? c_.layout2dt(kBuyLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        Build2dt(ui, *this, *records, {});
    }
    void Ask(const proto::OpenMarketRow& row) {
        const assets::ItemRow* item = c_.items != nullptr ? c_.items->Find(row.item.item) : nullptr;
        if (auto* w = ui::As<ui::Static>(Find(10))) w->texture = item != nullptr && !item->iconPath.empty() ? "icon\\" + item->iconPath : std::string();
        if (ui::Widget* w = Find(15)) w->SetText(item != nullptr ? c_.Text(item->nameKey) : std::string());
        if (auto* w = ui::As<ui::Edit>(Find(17))) {
            w->SetValue(std::to_string(std::max<int>(1, row.item.quantity)));
            w->enabled = false;
        }
        if (auto* w = ui::As<ui::Edit>(Find(16))) {
            w->SetValue(std::to_string(row.price));
            w->enabled = false;
        }
        visible = true;
    }
    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && (n.control == 3 || n.control == 4)) {
            if (n.control == 3 && confirmed) confirmed();
            if (ui_ != nullptr) ui_->Close(this);
            return true;
        }
        return MainFrame::OnNotice(n);
    }

private:
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
};

// The consignment notification window (CNIFOpenMarketAlramWnd, open_market_2.2dt, while a ticket runs): the player's
// listings in three groups, each a row "Sale Registered (n/room)", "Sale Complete (..)", "Deletion Pending (..)" with
// its fold button (58: gil_friend_grop_open / close), its listings under it ("- name" and the minutes left; the
// pending deletions in red). Opened, it asks the server for the listings.
class AlarmWindow : public ui::MainFrame {
public:
    AlarmWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}
    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        const auto* records = c_.layout2dt ? c_.layout2dt(kAlarmLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        const std::vector<ui::Widget*> made = Build2dt(ui, *this, *records, {});
        for (std::size_t i = 0; i < records->size(); ++i) {
            if ((*records)[i].className == "CNIFOpenMarketAlramSlot" && made[i] != nullptr) rows_.push_back(made[i]);
        }
        std::sort(rows_.begin(), rows_.end(), [](const ui::Widget* a, const ui::Widget* b) { return a->rect.y < b->rect.y; });
        bar_ = ui::As<ui::ScrollBar>(Find(80));
        Send(c_, proto::OpenMarketMine{});
    }
    void Update(ui::Frame& frame) override {
        MainFrame::Update(frame);
        if (c_.state == nullptr) return;
        const int scroll = bar_ != nullptr ? bar_->position() : 0;
        const int minute = int(c_.time / 60.0);
        if (c_.state->openMarket.version == seen_ && scroll == scroll_ && minute == minute_) return;
        seen_ = c_.state->openMarket.version;
        scroll_ = scroll;
        minute_ = minute;
        Fill();
    }
    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.control == 58 && n.source != nullptr) {
            for (std::size_t i = 0; i < rows_.size() && i < shown_.size(); ++i) {
                if (n.source->parent() == rows_[i] && shown_[i].header) {
                    folded_[std::size_t(shown_[i].group)] = !folded_[std::size_t(shown_[i].group)];
                    Fill();
                    return true;
                }
            }
        }
        return MainFrame::OnNotice(n);
    }

private:
    struct Line {
        int group = 0;
        bool header = false;
        const proto::OpenMarketListing* listing = nullptr;
        int count = 0;
    };
    void Fill() {
        const game::OpenMarketView& m = c_.state->openMarket;
        static constexpr const char* kGroups[3] = {"UIIT_STT_OPEN_MARKET_ANNOUNCING_UI_TRADE_ENTER", "UIIT_STT_OPEN_MARKET_ANNOUNCING_UI_TRADE_FINISH",
                                                   "UIIT_STT_OPEN_MARKET_ANNOUNCING_UI_DELETE_WAIT"};
        static constexpr game::openmarket::Status kStatus[3] = {game::openmarket::Status::OnSale, game::openmarket::Status::Sold,
                                                                game::openmarket::Status::Expired};
        std::vector<Line> lines;
        for (int g = 0; g < 3; ++g) {
            std::vector<const proto::OpenMarketListing*> in;
            for (const proto::OpenMarketListing& l : m.Listings()) {
                if (l.status == std::uint8_t(kStatus[g])) in.push_back(&l);
            }
            lines.push_back({g, true, nullptr, int(in.size())});
            if (!folded_[std::size_t(g)]) {
                for (const auto* l : in) lines.push_back({g, false, l, 0});
            }
        }
        const int most = std::max(0, int(lines.size()) - int(rows_.size()));
        if (bar_ != nullptr) bar_->SetRange(0, most);
        const int first = std::clamp(scroll_, 0, most);
        shown_.clear();
        const double passed = c_.time - m.ListingsAt();
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            ui::Widget* row = rows_[i];
            const std::size_t at = std::size_t(first) + i;
            row->visible = at < lines.size();
            if (!row->visible) continue;
            const Line& l = lines[at];
            shown_.push_back(l);
            ui::Widget* text = row->Child(21);
            ui::Widget* time = row->Child(41);
            auto* fold = ui::As<ui::Button>(row->Child(58));
            if (fold != nullptr) {
                fold->visible = l.header;
                fold->texture = folded_[std::size_t(l.group)] ? "interface\\guild\\gil_friend_grop_open.ddj" : "interface\\guild\\gil_friend_grop_close.ddj";
            }
            const ui::Color colour = !l.header && l.group == 2 ? ui::Argb(0xFFFF4A4A) : ui::kWhite;
            if (auto* t = ui::As<ui::Static>(text)) t->textColor = colour;
            if (auto* t = ui::As<ui::Static>(time)) t->textColor = colour;
            if (l.header) {
                if (text != nullptr) text->SetText(std::format("{} ({}/{})", c_.Text(kGroups[l.group]), l.count, int(m.State().room)));
                if (time != nullptr) time->SetText({});
            } else {
                const assets::ItemRow* item = c_.items != nullptr ? c_.items->Find(l.listing->item.item) : nullptr;
                if (text != nullptr) text->SetText("- " + (item != nullptr ? c_.Text(item->nameKey) : std::string()));
                const long long minutes = std::max(0LL, (long long)(double(l.listing->secondsLeft) - passed) / 60);
                if (time != nullptr) time->SetText(std::format("{} {}", minutes, c_.Text("UIIT_STT_QUEST_MINUTE")));
            }
        }
    }
    HudContext& c_;
    std::vector<ui::Widget*> rows_;
    std::vector<Line> shown_;
    ui::ScrollBar* bar_ = nullptr;
    std::array<bool, 3> folded_{};
    std::uint64_t seen_ = ~std::uint64_t(0);
    int scroll_ = 0, minute_ = -1;
};

// Its side icon (CIFOpenMarketAlramGuide, 198, 40 x 40) while a ticket runs: icon\\etc\\sell_icon_1 and _2 in turn (500
// ms); after a sale's or an expiry's notice it blinks (shown, hidden, every 500 ms) until the window opens. A click opens
// the window, or closes it.
class AlarmGuide : public ui::Static {
public:
    AlarmGuide(const assets::UiLayoutNode& node, HudContext& c) : Static(node), c_(c) {}
    bool blinking = false;
    void Created(ui::Interface& ui) override {
        Static::Created(ui);
        interactive = true;
    }
    void Update(ui::Frame& frame) override {
        Static::Update(frame);
        const int half = int(c_.time * 2.0);
        texture = half % 2 != 0 ? "icon\\etc\\sell_icon_2.ddj" : "icon\\etc\\sell_icon_1.ddj";
        alpha = blinking && half % 2 != 0 ? 0.0f : 1.0f;
    }
    void Mouse(ui::Frame& frame) override {
        if (!frame.input.Released(ui::MouseButton::Left) || frame.pointer.hovered != this) return;
        blinking = false;
        ToggleMarketAlarm(c_);
    }

private:
    HudContext& c_;
};

class MarketWindow : public ui::MainFrame {
public:
    MarketWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    enum class Page { Search, Register, Settle };

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
        const auto* records = c_.layout2dt ? c_.layout2dt(kLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        const std::vector<ui::Widget*> made = Build2dt(ui, *this, *records, [](const assets::Ui2dtRecord& r, assets::UiLayoutNode& n) {
            if (r.className == "CIFSlotWithHelpEx") n.type = "CIFSlotWithHelp";
            if (r.kind == 8) n.type = "CNIFStallNetworkRow"; // a row chosen by a press, lit (HudStall.cpp)
            if (r.kind == 12) n.type = "CIFComboBox";
            if (r.kind == 14) n.type = "CNIFStallNetworkPages";
            if (r.kind == 15) {
                n.type = "CNIFGuildRow";
                for (std::string_view piece : {"left.ddj", "mid.ddj", "right.ddj", "Left.ddj", "Mid.ddj", "Right.ddj"}) {
                    if (const std::size_t at = n.ddj.rfind(piece); at != std::string::npos) n.ddj.resize(at);
                }
            }
        });
        for (std::size_t i = 0; i < records->size(); ++i) {
            const std::string& cls = (*records)[i].className;
            if (cls == "CNIFOpenMarketSearchWnd") search_ = made[i];
            if (cls == "CNIFOpenMarketRegistrationWnd") register_ = made[i];
            if (cls == "CNIFOpenMarketBalanceWnd") settle_ = made[i];
        }
        if (search_ == nullptr || register_ == nullptr || settle_ == nullptr) return;
        for (const auto& [page, holder] : {std::pair{search_, kSearchRows}, std::pair{register_, kRegisterRows}, std::pair{settle_, kSettleRows}}) {
            std::vector<ui::Widget*>& rows = page == search_ ? searchRows_ : page == register_ ? registerRows_ : settleRows_;
            if (ui::Widget* list = page->Child(holder)) {
                for (const auto& child : list->children()) {
                    if (child->node().type == "CNIFStallNetworkRow") rows.push_back(child.get());
                }
            }
            std::sort(rows.begin(), rows.end(), [](const ui::Widget* a, const ui::Widget* b) { return a->rect.y < b->rect.y; });
            for (ui::Widget* row : rows) {
                if (auto* slot = ui::As<ui::Slot>(row->Child(kRowItem))) slot->pickable = false;
            }
        }
        // The search strip's combos as the stall network's (its classes, their types, the degrees).
        if (auto* classes = Combo(kClassCombo)) {
            classes->items.push_back(c_.Text("UIIT_CTL_WARENETWORK_SCAN_SELECT"));
            if (c_.stallNetwork != nullptr) {
                for (const assets::StallNetworkClass& k : c_.stallNetwork->Classes()) classes->items.push_back(c_.Text(k.textKey));
            }
            classes->Select(0);
        }
        Types();
        Show(Page::Search);
    }

    void Show(Page page) {
        page_ = page;
        chosen_ = -1;
        shownPage_ = 0;
        if (search_ != nullptr) search_->visible = page == Page::Search;
        if (register_ != nullptr) register_->visible = page == Page::Register;
        if (settle_ != nullptr) settle_->visible = page == Page::Settle;
        if (ui::Widget* t = Child(kTitle)) {
            t->SetText(c_.Text(page == Page::Search ? "UIIT_STT_OPEN_MARKET_ITEM_SEARCH" : page == Page::Register ? "UIIT_STT_OPEN_MARKET_ITEM_ENTER" : "UIIT_STT_OPEN_MARKET_ITEM_CALCULATE"));
        }
        seen_ = ~std::uint64_t(0);
    }
    Page Shown() const { return page_; }

    void Update(ui::Frame& frame) override {
        MainFrame::Update(frame);
        // The page buttons' help (1.188's: Search, Register, Settle).
        for (auto [id, key] : {std::pair{kSearchButton, "UIIT_STT_OPEN_MARKET_SEARCH"}, std::pair{kRegisterButton, "UIIT_STT_OPEN_MARKET_ENTER"},
                               std::pair{kSettleButton, "UIIT_STT_OPEN_MARKET_CALCULATE"}}) {
            if (ui::Widget* b = Child(id); b != nullptr && frame.pointer.hovered == b) frame.interface.ShowTooltip(c_.Text(key), b->ScreenRect());
        }
        game::PlayerState* s = c_.state;
        if (s == nullptr) return;
        const std::uint64_t gold = s->inventory.gold;
        for (auto [page, id] : {std::pair{search_, kSearchGold}}) {
            if (page == nullptr) continue;
            if (auto* g = ui::As<ui::Static>(page->Child(id))) {
                g->SetText(GoldText(gold));
                g->textColor = GoldColor(gold);
            }
        }
        if (register_ != nullptr) {
            if (ui::Widget* strip = register_->Child(kRegisterStrip)) {
                if (ui::Widget* box = strip->Child(kDepositBox)) Put(box->Child(kDeposit), GoldText(s->openMarket.PendingDeposit()));
                if (ui::Widget* box = strip->Child(kGoldBox)) Put(box->Child(kRegisterGold), GoldText(gold));
            }
        }
        // Each second the times move on; else on news.
        const std::uint64_t now = std::uint64_t(c_.time);
        if (s->openMarket.version == seen_ && now == tick_) return;
        seen_ = s->openMarket.version;
        tick_ = now;
        Fill(*s);
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::ComboPicked && n.control == kClassCombo) return Types(), true;
        if (n.kind == ui::Notice::Kind::ComboPicked && n.control == kTypeCombo) return Degrees(), true;
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr) return MainFrame::OnNotice(n);
        if (n.source->parent() == this) {
            switch (n.control) {
            case kSearchButton: Show(Page::Search); return true;
            case kRegisterButton:
                Show(Page::Register);
                Send(c_, proto::OpenMarketMine{});
                return true;
            case kSettleButton:
                Show(Page::Settle);
                Send(c_, proto::OpenMarketMine{});
                return true;
            case 2: // closed: the market too (the original's 0x7507)
                Send(c_, proto::OpenMarketClose{});
                if (c_.state != nullptr) c_.state->openMarket.pending.clear();
                return MainFrame::OnNotice(n);
            default: break;
            }
        }
        switch (n.control) {
        case kScan: Search(); return true;
        case kBuy: Buy(); return true;
        case kWhisper:
            if (const proto::OpenMarketRow* r = ChosenResult(); r != nullptr && ui_ != nullptr) {
                if (auto* chat = dynamic_cast<ChatWindow*>(ui_->Window(hud::kChat))) chat->Address(r->seller, true);
            }
            return true;
        case kRegister: Register(); return true;
        case kCancel: CancelChosen(); return true;
        case kSettle: Settle(); return true;
        case kSearchPages:
            Send(c_, proto::OpenMarketSearch{group_, degree_, seller_, std::uint16_t(n.value)});
            chosen_ = -1;
            return true;
        case kRegisterPages:
        case kSettlePages:
            shownPage_ = n.value;
            chosen_ = -1;
            seen_ = ~std::uint64_t(0);
            return true;
        default: break;
        }
        const std::vector<ui::Widget*>& rows = Rows();
        for (std::size_t k = 0; k < rows.size(); ++k) {
            if (n.source == rows[k] || n.source->parent() == rows[k]) {
                chosen_ = int(k);
                seen_ = ~std::uint64_t(0);
                return true;
            }
        }
        return MainFrame::OnNotice(n);
    }

    // An item of the bag chosen for registration (a right click on it while the register page shows): its price asked.
    void Put(int place) {
        game::PlayerState* s = c_.state;
        if (s == nullptr || page_ != Page::Register || ui_ == nullptr) return;
        const game::ItemInstance* item = s->inventory.At(std::uint8_t(place));
        if (item == nullptr) return;
        auto& pending = s->openMarket.pending;
        if (std::any_of(pending.begin(), pending.end(), [&](const game::OpenMarketView::Pending& p) { return p.place == place; })) return;
        // "The number of registered items allowed on this page has been exceeded" (the page's ten).
        if (int(pending.size()) >= om::kPageRows) return Line("UIIT_STT_OPEN_MARKET_ADD_ENTERD_ERROR");
        const std::uint32_t count = std::max<std::uint32_t>(1, item->count);
        const std::uint32_t refId = item->refId;
        PriceBox* box = dynamic_cast<PriceBox*>(ui_->Window(kPriceBoxId));
        if (box == nullptr) {
            auto node = RootNode2dt(c_, kPriceLayout, "CNIFOpenMarketRegistrationPriceMsgBox", kPriceBoxId);
            if (!node) return;
            node->rect.x = rect.x + 120.0f;
            node->rect.y = rect.y + 140.0f;
            box = dynamic_cast<PriceBox*>(ui_->Open(*node));
            if (box == nullptr) return;
        }
        HudContext* c = &c_;
        box->answer = [c, place, refId, count](std::uint64_t price) {
            if (c->state == nullptr) return;
            if (price < om::kPriceMin || price > om::kPriceMax) {
                if (const char* key = om::CodeKey(om::code::kPrice); *key != '\0' && c->systemLine) c->systemLine(c->Text(key));
                return;
            }
            c->state->openMarket.pending.push_back({std::uint8_t(place), refId, count, price});
            ++c->state->openMarket.version;
        };
        const assets::ItemRow* row = c_.items != nullptr ? c_.items->Find(refId) : nullptr;
        box->Ask(row != nullptr ? c_.Text(row->nameKey) : std::string(), row != nullptr ? "icon\\" + row->iconPath : std::string(), count);
    }
    // Scripts: the page's register, take back, settle and buy as their buttons.
    void Register() {
        game::PlayerState* s = c_.state;
        if (s == nullptr || s->openMarket.pending.empty() || c_.hud == nullptr) return;
        const std::uint64_t deposit = s->openMarket.PendingDeposit();
        HudContext* c = &c_;
        // The registration's own box (open_market_1_3.2dt): the deposit, the warning and its check box.
        if (ui_ == nullptr) return;
        if (ui::Widget* open = ui_->Window(kConfirmBoxId)) ui_->Close(open);
        const auto node = RootNode2dt(c_, kConfirmLayout, "CNIFOpenMarketRegistrationConfirmationMsgBox", kConfirmBoxId);
        auto* box = node ? dynamic_cast<RegisterConfirmBox*>(ui_->Open(*node)) : nullptr;
        if (box == nullptr) return;
        ui_->Centre(*box, ui_->screen());
        box->confirmed = [c] {
            if (c->state == nullptr) return;
            proto::OpenMarketRegister m;
            for (const auto& p : c->state->openMarket.pending) m.entries.push_back({p.place, p.count, p.price});
            Send(*c, m);
        };
        box->Ask(deposit);
    }
    void CancelChosen() {
        game::PlayerState* s = c_.state;
        if (s == nullptr || c_.hud == nullptr) return;
        const auto rows = RegisterRows(*s);
        HudContext* c = &c_;
        if (chosen_ < 0) {
            // None chosen: the page's rows all at once (1.188's Cancel): any of them still pending, "remove all items from
            // the registration pending list?" (those taken off it); else "cancel the registration for all items?".
            std::vector<std::uint8_t> pending;
            std::vector<std::uint32_t> listed;
            for (std::size_t i = 0; i < std::size_t(om::kPageRows); ++i) {
                const std::size_t at = std::size_t(shownPage_) * om::kPageRows + i;
                if (at >= rows.size()) break;
                if (rows[at].pending) pending.push_back(rows[at].place);
                else listed.push_back(rows[at].listing);
            }
            if (!pending.empty()) {
                c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {c_.Text("UIIT_STT_OPEN_MARKET_TEMP_ENTER_CANCEL_ALL")}, [c, pending](bool yes) {
                    if (!yes || c->state == nullptr) return;
                    std::erase_if(c->state->openMarket.pending, [&](const game::OpenMarketView::Pending& p) {
                        return std::find(pending.begin(), pending.end(), p.place) != pending.end();
                    });
                    ++c->state->openMarket.version;
                });
            } else if (!listed.empty()) {
                c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {c_.Text("UIIT_STT_OPEN_MARKET_ENTER_CANCEL_ALL")}, [c, listed](bool yes) {
                    if (yes) Send(*c, proto::OpenMarketTakeBack{listed});
                });
            }
            return;
        }
        const std::size_t at = std::size_t(shownPage_) * om::kPageRows + std::size_t(chosen_);
        if (at >= rows.size()) return;
        const RegisterRow r = rows[at];
        const std::string name = ItemName(r.refId);
        if (r.pending) {
            // Off the pending list (UIIT_STT_OPEN_MARKET_TEMP_ENTER_CANCEL "remove [%s] from the registration list?").
            c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {FormatText(c_.Text("UIIT_STT_OPEN_MARKET_TEMP_ENTER_CANCEL"), {name})}, [c, place = r.place](bool yes) {
                if (!yes || c->state == nullptr) return;
                std::erase_if(c->state->openMarket.pending, [&](const game::OpenMarketView::Pending& p) { return p.place == place; });
                ++c->state->openMarket.version;
            });
        } else {
            // "Will you cancel the registration for [%s]? Upon cancellation, the deposit will not be refunded."
            c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {FormatText(c_.Text("UIIT_STT_OPEN_MARKET_ENTER_CANCEL"), {name})}, [c, listing = r.listing](bool yes) {
                if (yes) Send(*c, proto::OpenMarketTakeBack{{listing}});
            });
        }
        chosen_ = -1;
    }
    void Settle() {
        game::PlayerState* s = c_.state;
        if (s == nullptr) return;
        proto::OpenMarketSettle m;
        for (const proto::OpenMarketListing& l : s->openMarket.Listings()) {
            if (l.status == std::uint8_t(om::Status::Sold) && int(m.listings.size()) < om::kPageRows) m.listings.push_back(l.listing);
        }
        if (!m.listings.empty()) Send(c_, m);
    }
    // The search as the combos say (scripts: class and type by index, degree 0 any; seller optional).
    void Search(int classIndex = -1, int typeIndex = -1, int degree = -1) {
        if (classIndex >= 0) {
            if (auto* classes = Combo(kClassCombo)) classes->Select(classIndex + 1);
            Types();
        }
        if (typeIndex >= 0) {
            if (auto* types = Combo(kTypeCombo)) types->Select(typeIndex);
            Degrees();
        }
        if (degree >= 0) {
            if (auto* degrees = Combo(kDegreeCombo)) degrees->Select(degree);
        }
        const assets::StallNetworkType* type = TypeChosen();
        // "Select the upper and the lower category."
        if (type == nullptr) return Line("UIIT_STT_OPEN_MARKET_FIND_STRING_ERROR");
        group_ = type->group;
        const auto* degrees = Combo(kDegreeCombo);
        degree_ = degrees != nullptr && degrees->selected > 0 ? std::uint8_t(degrees->selected) : 0;
        seller_.clear();
        if (ui::Widget* strip = search_ != nullptr ? search_->Child(kSearchStrip) : nullptr) {
            if (ui::Widget* box = strip->Child(kSellerBox)) {
                if (const auto* e = ui::As<ui::Edit>(box->Child(kSellerEdit))) seller_ = e->text();
            }
        }
        chosen_ = -1;
        Send(c_, proto::OpenMarketSearch{group_, degree_, seller_, 0});
    }
    void Buy() {
        const proto::OpenMarketRow* r = ChosenResult();
        if (r == nullptr || c_.hud == nullptr) return;
        HudContext* c = &c_;
        const proto::OpenMarketRow row = *r;
        // The market's own purchase box (open_market_1_4.2dt): the item, its count and its price.
        if (ui_ == nullptr) return;
        if (ui::Widget* open = ui_->Window(kBuyBoxId)) ui_->Close(open);
        const auto node = RootNode2dt(c_, kBuyLayout, "CNIFOpenMarketSearchPriceMsgBox", kBuyBoxId);
        auto* box = node ? dynamic_cast<BuyBox*>(ui_->Open(*node)) : nullptr;
        if (box == nullptr) return;
        ui_->Centre(*box, ui_->screen());
        box->confirmed = [c, row] { Send(*c, proto::OpenMarketBuy{row.account, row.listing}); };
        box->Ask(row);
    }
    void Choose(int row) {
        chosen_ = row;
        seen_ = ~std::uint64_t(0);
    }
    // The last search asked again (its page): a purchase made, the row it bought gone from the list.
    void SearchAgain() {
        if (group_ == 0) return;
        chosen_ = -1;
        const std::uint16_t page = c_.state != nullptr ? c_.state->openMarket.Results().page : 0;
        Send(c_, proto::OpenMarketSearch{group_, degree_, seller_, page});
    }

private:
    struct RegisterRow {
        bool pending = false;
        std::uint8_t place = 0;
        std::uint32_t listing = 0, refId = 0;
        proto::ItemRecord item;
        std::uint64_t price = 0;
        double secondsLeft = -1; // -1: waiting (pending)
        std::uint8_t status = 0;
    };
    std::vector<RegisterRow> RegisterRows(const game::PlayerState& s) const {
        std::vector<RegisterRow> out;
        const double since = c_.time - s.openMarket.ListingsAt();
        for (const proto::OpenMarketListing& l : s.openMarket.Listings()) {
            if (l.status != std::uint8_t(om::Status::OnSale) && l.status != std::uint8_t(om::Status::Expired)) continue;
            out.push_back({false, 0, l.listing, l.item.item, l.item, l.price, double(l.secondsLeft) - since, l.status});
        }
        for (const game::OpenMarketView::Pending& p : s.openMarket.pending) {
            RegisterRow r;
            r.pending = true;
            r.place = p.place;
            r.refId = p.refId;
            if (s.inventory.At(p.place) != nullptr) r.item = s.inventory.Record(p.place);
            r.price = p.price;
            out.push_back(r);
        }
        return out;
    }
    const std::vector<ui::Widget*>& Rows() const { return page_ == Page::Search ? searchRows_ : page_ == Page::Register ? registerRows_ : settleRows_; }
    ui::ComboBox* Combo(int id) {
        ui::Widget* strip = search_ != nullptr ? search_->Child(kSearchStrip) : nullptr;
        return strip != nullptr ? ui::As<ui::ComboBox>(strip->Child(id)) : nullptr;
    }
    const assets::StallNetworkType* TypeChosen() {
        auto* classes = Combo(kClassCombo);
        auto* types = Combo(kTypeCombo);
        if (classes == nullptr || types == nullptr || c_.stallNetwork == nullptr || classes->selected < 1) return nullptr;
        const auto& all = c_.stallNetwork->Classes();
        if (std::size_t(classes->selected - 1) >= all.size()) return nullptr;
        const auto& k = all[std::size_t(classes->selected - 1)];
        return types->selected >= 0 && std::size_t(types->selected) < k.types.size() ? &k.types[std::size_t(types->selected)] : nullptr;
    }
    void Types() {
        auto* classes = Combo(kClassCombo);
        auto* types = Combo(kTypeCombo);
        if (types == nullptr) return;
        types->items.clear();
        if (classes != nullptr && c_.stallNetwork != nullptr && classes->selected >= 1 && std::size_t(classes->selected - 1) < c_.stallNetwork->Classes().size()) {
            for (const assets::StallNetworkType& t : c_.stallNetwork->Classes()[std::size_t(classes->selected - 1)].types) types->items.push_back(c_.Text(t.textKey));
        }
        types->Select(types->items.empty() ? -1 : 0);
        Degrees();
    }
    void Degrees() {
        auto* degrees = Combo(kDegreeCombo);
        if (degrees == nullptr) return;
        degrees->items.clear();
        const assets::StallNetworkType* t = TypeChosen();
        if (t == nullptr || t->degrees == 0) {
            degrees->items.push_back(c_.Text("UIIT_CTL_WARENETWORK_SCAN_END"));
        } else {
            degrees->items.push_back(c_.Text("UIIT_CTL_WARENETWORK_DETAIL_TOTAL"));
            for (int d = 1; d <= t->degrees; ++d) degrees->items.push_back(std::to_string(d));
        }
        degrees->Select(0);
        degrees->enabled = t != nullptr && t->degrees != 0;
    }
    const proto::OpenMarketRow* ChosenResult() const {
        if (c_.state == nullptr || page_ != Page::Search) return nullptr;
        const auto& rows = c_.state->openMarket.Results().rows;
        return chosen_ >= 0 && std::size_t(chosen_) < rows.size() ? &rows[std::size_t(chosen_)] : nullptr;
    }
    std::string ItemName(std::uint32_t refId) const {
        const assets::ItemRow* row = c_.items != nullptr ? c_.items->Find(refId) : nullptr;
        return row != nullptr ? c_.Text(row->nameKey) : std::string();
    }
    void Line(const char* key) {
        if (c_.systemLine) c_.systemLine(c_.Text(key));
    }
    static void Put(ui::Widget* w, const std::string& text) {
        if (w != nullptr) w->SetText(text);
    }
    // A row as it is (null: empty).
    void FillRow(ui::Widget& row, int k, int number, const proto::ItemRecord* item, std::uint64_t price, const std::string& time) {
        const assets::ItemRow* it = item != nullptr && c_.items != nullptr ? c_.items->Find(item->item) : nullptr;
        const game::ItemInstance instance = item != nullptr ? game::Inventory::FromRecord(*item) : game::ItemInstance{};
        Put(row.Child(kRowNumber), it != nullptr ? std::to_string(number) : std::string());
        Put(row.Child(kRowLevel), it != nullptr ? std::to_string(it->requiredLevel) : std::string());
        Put(row.Child(kRowName), it != nullptr ? c_.Text(it->nameKey) + (instance.plus > 0 ? " (+" + std::to_string(instance.plus) + ")" : "") : std::string());
        Put(row.Child(kRowCount), it != nullptr ? std::to_string(std::max<int>(1, item->quantity)) : std::string());
        Put(row.Child(kRowPrice), it != nullptr ? GoldText(price) : std::string());
        Put(row.Child(kRowTime), it != nullptr ? time : std::string());
        if (ui::Widget* tip = row.Child(kRowTip)) tip->visible = it != nullptr; // its item's help, on a filled row
        if (auto* slot = ui::As<ui::Slot>(row.Child(kRowItem))) {
            ShowItem(*slot, it != nullptr ? &instance : nullptr, c_);
            const proto::ItemRecord copy = item != nullptr ? *item : proto::ItemRecord{};
            slot->tooltipLines = [this, copy] {
                const assets::ItemRow* r = c_.items != nullptr ? c_.items->Find(copy.item) : nullptr;
                return r != nullptr ? ItemTooltip(*r, game::Inventory::FromRecord(copy), c_) : std::vector<ui::TooltipLine>{};
            };
        }
        SetChosen(row, k == chosen_ && it != nullptr);
    }
    static void SetChosen(ui::Widget& row, bool chosen) { StallRowChosen(row, chosen); }
    void Pages(ui::Widget* page, int id, int shown, int pages) {
        if (page == nullptr) return;
        ui::Widget* w = page->Child(id);
        if (w != nullptr) StallPages(*w, shown, pages);
    }
    void Fill(const game::PlayerState& s) {
        const game::OpenMarketView& v = s.openMarket;
        if (page_ == Page::Search) {
            const auto& res = v.Results();
            for (std::size_t k = 0; k < searchRows_.size(); ++k) {
                const proto::OpenMarketRow* r = k < res.rows.size() ? &res.rows[k] : nullptr;
                FillRow(*searchRows_[k], int(k), int(res.page) * om::kPageRows + int(k) + 1, r != nullptr ? &r->item : nullptr, r != nullptr ? r->price : 0, {});
            }
            Pages(search_, kSearchPages, res.page, res.pages);
            if (ui::Widget* strip = search_ != nullptr ? search_->Child(kSearchStrip) : nullptr) {
                const proto::OpenMarketRow* chosen = ChosenResult();
                Put(strip->Child(24), chosen != nullptr ? chosen->seller : std::string()); // the seller's name (24 beside "Seller")
            }
        } else if (page_ == Page::Register) {
            const auto rows = RegisterRows(s);
            const int pages = int((rows.size() + om::kPageRows - 1) / om::kPageRows);
            for (std::size_t k = 0; k < registerRows_.size(); ++k) {
                const std::size_t at = std::size_t(shownPage_) * om::kPageRows + k;
                const RegisterRow* r = at < rows.size() ? &rows[at] : nullptr;
                const std::string time = r == nullptr                                           ? std::string()
                                         : r->pending                                           ? c_.Text("UIIT_STT_OPEN_MARKET_WAITING")
                                         : r->status == std::uint8_t(om::Status::Expired)       ? c_.Text("UIIT_STT_OPEN_MARKET_TIME_OVER")
                                                                                               : TimeLeft(c_, r->secondsLeft);
                FillRow(*registerRows_[k], int(k), int(at) + 1, r != nullptr ? &r->item : nullptr, r != nullptr ? r->price : 0, time);
            }
            Pages(register_, kRegisterPages, shownPage_, std::max(1, pages));
        } else {
            std::vector<const proto::OpenMarketListing*> sold;
            for (const proto::OpenMarketListing& l : v.Listings()) {
                if (l.status == std::uint8_t(om::Status::Sold)) sold.push_back(&l);
            }
            const double since = c_.time - v.ListingsAt();
            for (std::size_t k = 0; k < settleRows_.size(); ++k) {
                const std::size_t at = std::size_t(shownPage_) * om::kPageRows + k;
                const proto::OpenMarketListing* l = at < sold.size() ? sold[at] : nullptr;
                FillRow(*settleRows_[k], int(k), int(at) + 1, l != nullptr ? &l->item : nullptr, l != nullptr ? l->price : 0,
                        l != nullptr ? TimeLeft(c_, double(l->secondsLeft) - since) : std::string());
            }
            Pages(settle_, kSettlePages, shownPage_, std::max(1, int((sold.size() + om::kPageRows - 1) / om::kPageRows)));
            const game::OpenMarketView::Sums sums = v.Sold();
            if (ui::Widget* strip = settle_ != nullptr ? settle_->Child(kSettleStrip) : nullptr) {
                const std::uint64_t values[] = {sums.prices, sums.saleFees, sums.deposits, sums.payout};
                const int boxes[] = {72, 73, 74, 75};
                for (int k = 0; k < 4; ++k) {
                    if (ui::Widget* box = strip->Child(boxes[k])) Put(box->Child(20), GoldText(values[k]));
                }
            }
        }
    }

    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    ui::Widget *search_ = nullptr, *register_ = nullptr, *settle_ = nullptr;
    std::vector<ui::Widget*> searchRows_, registerRows_, settleRows_;
    Page page_ = Page::Search;
    int chosen_ = -1, shownPage_ = 0;
    std::uint64_t seen_ = ~std::uint64_t(0), tick_ = 0;
    std::uint32_t group_ = 0;
    std::uint8_t degree_ = 0;
    std::string seller_;
};

MarketWindow* WindowOf(HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    return ui != nullptr ? dynamic_cast<MarketWindow*>(ui->Window(kWindowId)) : nullptr;
}

} // namespace

void RegisterOpenMarketWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CNIFOpenMarket", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<MarketWindow>(n, *c); });
    ui.Register("CNIFOpenMarketRegistrationPriceMsgBox", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<PriceBox>(n, *c); });
    ui.Register("CNIFOpenMarketSearchPriceMsgBox", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<BuyBox>(n, *c); });
    ui.Register("CNIFOpenMarketRegistrationConfirmationMsgBox", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<RegisterConfirmBox>(n, *c); });
    ui.Register("CNIFOpenMarketAlramWnd", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<AlarmWindow>(n, *c); });
    ui.Register("CIFOpenMarketAlramGuide", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<AlarmGuide>(n, *c); });
}

void ToggleMarketAlarm(HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    if (ui::Widget* w = ui->Window(kAlarmWindowId)) {
        ui->Close(w);
        return;
    }
    const auto node = RootNode2dt(c, kAlarmLayout, "CNIFOpenMarketAlramWnd", kAlarmWindowId);
    if (!node) return;
    if (ui::Widget* w = ui->Open(*node)) ui->Centre(*w, ui->screen());
}

void ShowMarketAlarmGuide(HudContext& c, bool shown) {
    if (c.hud == nullptr) return;
    if (!shown) {
        c.hud->Hide(kAlarmGuideId);
        return;
    }
    if (c.hud->Shown(kAlarmGuideId)) return;
    assets::UiLayoutNode node;
    node.type = "CIFOpenMarketAlramGuide";
    node.id = kAlarmGuideId;
    node.rect = {0.0f, 0.0f, 40.0f, 40.0f};
    c.hud->ShowGuide(node);
}

void MarketAlarmNews(HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    // The icon blinks while the window is not open (the window shows the news itself).
    if (auto* guide = dynamic_cast<AlarmGuide*>(ui->Window(kAlarmGuideId)); guide != nullptr && ui->Window(kAlarmWindowId) == nullptr) guide->blinking = true;
}

void ShowOpenMarket(HudContext& c, bool shown) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    MarketWindow* w = WindowOf(c);
    if (!shown) {
        if (w != nullptr && w->visible) c.hud->Hide(kWindowId);
        if (ui::Widget* box = ui->Window(kPriceBoxId)) box->visible = false;
        return;
    }
    if (w == nullptr) {
        auto node = RootNode2dt(c, kLayout, "CNIFOpenMarket", kWindowId);
        if (!node) return;
        node->rect.x = std::max(0.0f, (ui->screen().x - node->rect.w) / 2.0f);
        node->rect.y = std::max(0.0f, (ui->screen().y - node->rect.h) / 2.0f - 60.0f);
        ui->Open(*node);
    } else {
        c.hud->Show(kWindowId);
        w->Show(MarketWindow::Page::Search);
    }
    ui->Sound("SND_WINDOW_OPEN");
    c.hud->OpenPage(hud::kPageInventory); // the bag beside it: its items are put up from there
}

bool OpenMarketTakesItems(HudContext& c) {
    MarketWindow* w = WindowOf(c);
    return w != nullptr && w->visible && w->Shown() == MarketWindow::Page::Register;
}

void OpenMarketPut(HudContext& c, int place) {
    if (MarketWindow* w = WindowOf(c); w != nullptr && w->visible) w->Put(place);
}

bool OpenMarketScript(HudContext& c, const std::string& verb, const std::string& arg) {
    MarketWindow* w = WindowOf(c);
    if (w == nullptr) return false;
    const auto number = [&](std::size_t at) {
        std::size_t start = 0;
        for (std::size_t k = 0; k < at; ++k) start = arg.find(',', start) + 1;
        return std::atoi(arg.c_str() + start);
    };
    if (verb == "om-page") {
        w->Show(arg == "register" ? MarketWindow::Page::Register : arg == "settle" ? MarketWindow::Page::Settle : MarketWindow::Page::Search);
        if (arg != "search") Send(c, proto::OpenMarketMine{});
    } else if (verb == "om-search" && arg.starts_with("g")) {
        // om-search:gGROUP[,DEGREE]: the class and type whose group that is (the stall network's group, e.g. g42 the
        // recovery potions).
        const std::uint32_t group = std::uint32_t(std::strtoul(arg.c_str() + 1, nullptr, 10));
        const std::size_t comma = arg.find(',');
        const int degree = comma != std::string::npos ? std::atoi(arg.c_str() + comma + 1) : 0;
        const auto& classes = c.stallNetwork != nullptr ? c.stallNetwork->Classes() : std::vector<assets::StallNetworkClass>{};
        for (std::size_t k = 0; k < classes.size(); ++k) {
            for (std::size_t t = 0; t < classes[k].types.size(); ++t) {
                if (classes[k].types[t].group == group) {
                    w->Search(int(k), int(t), degree);
                    return true;
                }
            }
        }
        return false;
    } else if (verb == "om-search") {
        w->Search(number(0), number(1), arg.find(',', arg.find(',') + 1) != std::string::npos ? number(2) : 0);
    } else if (verb == "om-choose") {
        w->Choose(number(0));
    } else if (verb == "om-buy") {
        w->Choose(number(0));
        w->Buy();
    } else if (verb == "om-register") {
        w->Register();
    } else if (verb == "om-cancel") {
        w->Choose(number(0));
        w->CancelChosen();
    } else if (verb == "om-settle") {
        w->Settle();
    } else {
        return false;
    }
    return true;
}

void OpenMarketSearchAgain(HudContext& c) {
    if (MarketWindow* w = WindowOf(c); w != nullptr && w->visible) w->SearchAgain();
}

bool OpenMarketPrice(HudContext& c, std::uint64_t price) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    auto* box = ui != nullptr ? dynamic_cast<PriceBox*>(ui->Window(kPriceBoxId)) : nullptr;
    if (box == nullptr || !box->visible) return false;
    if (auto* e = ui::As<ui::Edit>(box->Find(16))) e->SetValue(std::to_string(price));
    box->Answer(true, price);
    return true;
}

} // namespace sr::client
