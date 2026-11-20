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
        // "The deposit comes to [%d] Gold. The deposit for unsold items are not refundable. Will you register the item(s)?"
        c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {FormatText(c_.Text("UIIT_MSG_OPEN_MARKET_ENTER"), {std::to_string(deposit)}), c_.Text("UIIT_MSG_OPEN_MARKET_WARNING")},
                        [c](bool yes) {
                            if (!yes || c->state == nullptr) return;
                            proto::OpenMarketRegister m;
                            for (const auto& p : c->state->openMarket.pending) m.entries.push_back({p.place, p.count, p.price});
                            Send(*c, m);
                        });
    }
    void CancelChosen() {
        game::PlayerState* s = c_.state;
        if (s == nullptr || c_.hud == nullptr || chosen_ < 0) return;
        const auto rows = RegisterRows(*s);
        const std::size_t at = std::size_t(shownPage_) * om::kPageRows + std::size_t(chosen_);
        if (at >= rows.size()) return;
        const RegisterRow r = rows[at];
        const std::string name = ItemName(r.refId);
        HudContext* c = &c_;
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
        c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {FormatText(c_.Text("UIIT_MSG_WARENETWORK_BUY_CONFIRM"), {ItemName(row.item.item), GoldText(row.price)})},
                        [c, row](bool yes) {
                            if (yes) Send(*c, proto::OpenMarketBuy{row.account, row.listing});
                        });
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
