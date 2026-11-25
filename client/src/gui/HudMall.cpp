// The item mall on the client: 1.188's CIFNewItemMall (ginterface 167, resinfo\ifnewitemmall.txt
// sections Create and MyInfo) with its shop panel (the menu from mallitemmenulistdata.txt, the tabs, the goods as
// CIFNewItemMallShopItemSlot), the slot's popup (CIFNewItemSlotPopupWnd), MyInfo (the name, the level, silk, gift silk,
// points; "Buy silk" and "Coupon" off), the confirm window (CNewItemMallConfirmBuy), the mall's own boxes
// (CIFNewItemMallMessageBox: Zzim, AllBuy, AllBuyZzim), the avatar try-on on the preview (123), the basket and the
// silk history (CNIFSilkBuyList, res_ui\silksearch.2dt); the "Enter Item Mall" box (kind 0x1E) before it opens.
//
// Control ids, hidden controls and the states are the 1.188 client's (the note's tables); what the note left open is
// marked ours where it is drawn.

#include <algorithm>
#include <format>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "gui/PortraitTexture.h"
#include "sr/game/Mall.h"
#include "sr/game/MallProtocol.h"

namespace sr::client {

namespace proto = game::proto;
namespace code = proto::mallcode;

namespace {

constexpr int kWindow = hud::kItemMall;
// ifnewitemmall.txt Create.
constexpr int kPopup = 100, kEventOff = 101, kEventPanel = 103, kMainPage = 104, kSearch = 106, kFind = 107, kOriginal = 109, kBuyWorn = 110,
              kPreview = 123, kBuyList = 131, kReceiveList = 132, kMyInfo = 120;
// MyInfo (ifnewitemmallmyinfo.txt; 1.2).
constexpr int kInfoPointRow[] = {200, 203, 206}, kInfoGiftIcon = 204, kInfoPoints = 206, kInfoGift = 207, kInfoSilk = 208, kInfoLevel = 209,
              kInfoName = 210, kCoupon = 211, kBuySilk = 212, kMoneyButton = 213;
// The popup's rows (1.5).
constexpr int kRowWear = 10, kRowBuy = 11, kRowInfo = 12, kRowReserve = 13;
// Ours: the shop panel (the original's 105 made in code at 13,72,551,513), its menu rows, the goods' slots, the confirm
// window (the original's control 1200), a box (1300), the history (1400), the basket's two rows.
constexpr int kPanel = 105, kFirstMain = 3000, kFirstSub = 3100, kFirstGood = 4000, kConfirm = 1200, kBox = 1300, kHistory = 1400;
constexpr int kBasketBuy = 3200, kBasketEmpty = 3201, kScrollUp = 3300, kScrollDown = 3301;
// A goods slot (ifnewitemmallshopitemslot.txt): name, arrow, old price, price, silk icon, count, icon.
constexpr int kSlotName = 801, kSlotArrow = 802, kSlotOld = 803, kSlotPrice = 805, kSlotCount = 807, kSlotIcon = 900;
constexpr float kMenuWidth = 124.0f, kGoodW = 199.0f, kGoodH = 81.0f, kGoodGap = 4.0f;
// The confirm window (ifnewitemmallconfirmbuy.txt).
constexpr int kConfirmBuy = 61, kConfirmCancel = 62, kCountUp = 50, kCountDown = 51, kCountEdit = 42, kConfirmName = 40, kConfirmIcon = 11;
constexpr int kConfirmSilk = 70; // ours: the silk row's value (the original's CIFNewItemMallConfirmSlot)
// The boxes (ifnewitemmallmessagebox.txt).
constexpr int kZzimYes = 1609, kZzimNo = 1608, kAllBuyYes = 1818, kAllBuyNo = 1817, kZzimAllYes = 2017, kZzimAllNo = 2016, kNoticeOk = 1390;

std::string Grouped(std::uint64_t n) {
    std::string s = std::to_string(n);
    for (int at = int(s.size()) - 3; at > 0; at -= 3) s.insert(std::size_t(at), ",");
    return s;
}

void Put(ui::Widget* w, const std::string& text) {
    if (w != nullptr && w->text() != text) w->SetText(text);
}

std::string IconOf(const assets::ItemRow* row) {
    return row == nullptr || row->iconPath.empty() ? std::string("icon\\icon_default.ddj") : "icon\\" + row->iconPath;
}

// Children of a layout's section added to `parent` (the first node topmost), placed from `origin`.
void AddSection(ui::Interface& ui, ui::Widget& parent, const char* path, const char* section, ui::Vec2 origin = {}) {
    const assets::UiLayout* l = ui.Layout(path);
    if (l == nullptr) return;
    for (const auto& s : l->sections) {
        if (s.name != section) continue;
        for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) {
            assets::UiLayoutNode n = *it;
            n.rect.x += origin.x;
            n.rect.y += origin.y;
            parent.Add(ui.Make(n));
        }
    }
}

// A good where the window found it: its tab (refshoptab's id) and slot.
struct GoodRef {
    std::uint32_t tab = 0;
    int slot = 0;
    bool operator==(const GoodRef&) const = default;
};

// One goods slot (CIFNewItemMallShopItemSlot, 199 x 81, mall_item_wnd.ddj): click selects, a right click opens the popup.
class GoodSlot : public ui::Widget {
public:
    GoodSlot(const assets::UiLayoutNode& node, HudContext& c) : Widget(node), c_(c) { interactive = true; }
    GoodRef ref;
    const assets::MallGood* good = nullptr;
    bool selected = false;
    void Mouse(ui::Frame& frame) override {
        if (frame.input.Pressed(ui::MouseButton::Left) && frame.pointer.hovered == this) Notify(ui::Notice::Kind::Clicked);
        if (frame.input.Released(ui::MouseButton::Right) && frame.pointer.held == this && Hit(frame.pointer.position)) Notify(ui::Notice::Kind::RightClicked);
    }
    bool OnNotice(const ui::Notice& n) override {
        // The icon's own press and right click are the slot's.
        if (n.source != this && n.control == kSlotIcon && (n.kind == ui::Notice::Kind::Clicked || n.kind == ui::Notice::Kind::RightClicked)) {
            Notify(n.kind);
            return true;
        }
        return false;
    }
    void Draw(ui::Frame& frame) override {
        frame.paint.Image("interface\\mall\\mall_item_wnd.ddj", ScreenRect(), {0, 0}, {1, 1}, ui::WithAlpha(selected ? ui::Rgba(255, 224, 128) : ui::kWhite, Opacity()));
    }

private:
    HudContext& c_;
};

// A menu row: a main slot (116 x 32, mall_item_tab.ddj / _on) or a sub-tab's line.
class MenuRow : public ui::Widget {
public:
    MenuRow(const assets::UiLayoutNode& node) : Widget(node) { interactive = true; }
    bool main = true, selected = false;
    void Mouse(ui::Frame& frame) override {
        if (frame.input.Pressed(ui::MouseButton::Left) && frame.pointer.hovered == this) Notify(ui::Notice::Kind::Clicked);
    }
    void Draw(ui::Frame& frame) override {
        if (main) {
            frame.paint.Image(selected ? "interface\\mall\\mall_item_tab_on.ddj" : "interface\\mall\\mall_item_tab.ddj", ScreenRect(), {0, 0}, {1, 1},
                              ui::WithAlpha(ui::kWhite, Opacity()));
        }
    }
};

class MallWindow : public ui::MainFrame {
public:
    MallWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        ui_ = &ui;
        AddSection(ui, *this, "resinfo\\ifnewitemmall.txt", "Create");
        // MyInfo (120) with its own layout's controls.
        assets::UiLayoutNode info;
        info.type = "CIFStatic";
        info.id = kMyInfo;
        info.rect = {562, 449, 223, 135};
        ui::Widget* my = Add(ui.Make(info));
        AddSection(ui, *my, "resinfo\\ifnewitemmallmyinfo.txt", "Create");
        // The popup's rows (its own layout).
        if (ui::Widget* popup = Child(kPopup)) {
            AddSection(ui, *popup, "resinfo\\ifnewitemslotpopupwnd.txt", "Create");
            popup->visible = false; // hidden at Create
        }
        // Hidden or disabled at Create: the event's button and panel (Mall_Open "0"), the money
        // button; the Obtained list never enabled; Buy silk and Coupon off while their links are shorter than 5 (ours:
        // no payment: always off).
        for (int id : {kEventOff, kEventPanel}) {
            if (ui::Widget* w = Child(id)) w->visible = false;
        }
        if (ui::Widget* w = my->Child(kMoneyButton)) w->visible = false;
        if (ui::Widget* w = Child(kReceiveList)) w->enabled = false;
        for (int id : {kBuySilk, kCoupon}) {
            if (ui::Widget* w = my->Child(id)) w->enabled = false;
        }
        // The preview (123), shown with no event: the character's figure (frame/Portraits' whole figure).
        assets::UiLayoutNode preview;
        preview.type = "CIFStatic";
        preview.id = kPreview;
        preview.rect = {567, 46, 215, 332};
        if (auto* p = ui::As<ui::Static>(Add(ui.Make(preview)))) p->texture = PortraitTexture(kMallPreview);
        // The shop panel: the menu's column and the goods' (two columns of 124 and 424 in the original's scroll view).
        assets::UiLayoutNode panel;
        panel.type = "CIFStatic";
        panel.id = kPanel;
        panel.rect = {13, 72, 551, 513};
        panel_ = Add(ui.Make(panel));
        panel_->clipChildren = true;
        if (auto* e = ui::As<ui::Edit>(Child(kSearch))) {
            e->passKeys = true;
            e->limit = 32;
        }
        view_ = View::Front;
        Rebuild();
    }

    enum class View : std::uint8_t { Front, Shop, Hot, Basket, Search };

    void Open() {
        if (c_.mall == nullptr) return;
        c_.mall->basket.clear(); // emptied when the mall is made and closed
        c_.mall->worn.clear();
        selected_ = {};
        view_ = View::Front;
        scroll_ = 0;
        Rebuild();
    }

    // The goods of the view (its tab, the hot store's tags, the basket, the search's matches).
    std::vector<GoodRef> Goods() const {
        std::vector<GoodRef> out;
        const assets::MallTable* cat = c_.mall != nullptr ? c_.mall->catalog : nullptr;
        if (cat == nullptr) return out;
        const auto each = [&](auto f) {
            for (const assets::MallShop& s : cat->shops()) {
                for (const assets::MallTab& t : s.tabs) {
                    for (const assets::MallGood& g : t.goods) f(t, g);
                }
            }
        };
        switch (view_) {
        case View::Shop:
            if (const assets::MallTab* t = cat->Tab(tab_)) {
                for (const assets::MallGood& g : t->goods) out.push_back({t->id, g.slot});
            }
            break;
        case View::Front:
        case View::Hot: {
            // The hot store (with the in-service tags 1, 2, 16, 8): one list each; the front page ours the same.
            for (const assets::MallHotRow& h : cat->hot()) {
                if (!h.service) continue;
                each([&](const assets::MallTab& t, const assets::MallGood& g) {
                    if ((g.saleTag & h.tag) == 0) return;
                    const GoodRef r{t.id, g.slot};
                    if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
                });
            }
            break;
        }
        case View::Basket:
            for (const auto& [tab, slot] : c_.mall->basket) out.push_back({tab, slot});
            break;
        case View::Search: {
            each([&](const assets::MallTab& t, const assets::MallGood& g) {
                const std::string name = c_.Text(g.nameKey);
                if (!search_.empty() && name.find(search_) != std::string::npos) out.push_back({t.id, g.slot});
            });
            break;
        }
        }
        return out;
    }

    const assets::MallGood* GoodOf(const GoodRef& r) const {
        const assets::MallTable* cat = c_.mall != nullptr ? c_.mall->catalog : nullptr;
        return cat != nullptr ? cat->Find(r.tab, r.slot) : nullptr;
    }
    const assets::ItemRow* RowOf(const assets::MallGood* g) const {
        return g != nullptr && c_.items != nullptr ? c_.items->Find(std::string_view(g->itemCodename)) : nullptr;
    }

    // The panel's rows made again for the view (the menu, the goods from the scroll's row).
    void Rebuild() {
        if (panel_ == nullptr || ui_ == nullptr) return;
        panel_->ClearChildren();
        const assets::MallTable* cat = c_.mall != nullptr ? c_.mall->catalog : nullptr;
        float y = 0.0f;
        int main = 0;
        if (cat != nullptr) {
            for (const assets::MallMenuRow& m : cat->menu()) {
                if (!m.service) continue;
                const assets::MallShop* shop = m.shop.empty() ? nullptr : cat->Shop(m.shop);
                const bool hot = m.nameKey == "UIIT_STT_SILKMALL_HOT_STORE", basket = m.nameKey == "UIIT_STT_SILKMALL_SHOPPING_BASKET_LIST";
                if (shop == nullptr && !hot && !basket) continue; // a shop with no goods (Package in 1.188)
                assets::UiLayoutNode n;
                n.type = "CIFStatic";
                n.id = kFirstMain + main;
                n.rect = {4.0f, y, 116.0f, 32.0f};
                auto row = std::make_unique<MenuRow>(n);
                MenuRow* r = static_cast<MenuRow*>(panel_->Add(std::move(row)));
                r->selected = (shop != nullptr && view_ == View::Shop && shopCode_ == shop->code) || (hot && view_ == View::Hot) || (basket && view_ == View::Basket);
                // Its title (600) and icon (601).
                assets::UiLayoutNode t;
                t.type = "CIFStatic";
                t.id = 600;
                t.rect = {32, 10, 77, 16};
                t.text = m.nameKey;
                r->Add(ui_->Make(t));
                assets::UiLayoutNode icon;
                icon.type = "CIFStatic";
                icon.id = 601;
                icon.rect = {7, 6, 20, 20};
                icon.ddj = m.icon;
                r->Add(ui_->Make(icon));
                menuRows_[n.id] = {shop != nullptr ? shop->code : std::string(), hot ? 1 : basket ? 2 : 0};
                y += 34.0f;
                ++main;
                // The open shop's tabs under it (a selected one yellow).
                if (shop != nullptr && shopCode_ == shop->code) {
                    for (std::size_t k = 0; k < shop->tabs.size(); ++k) {
                        assets::UiLayoutNode s;
                        s.type = "CIFStatic";
                        s.id = kFirstSub + int(k);
                        s.rect = {14.0f, y, 100.0f, 22.0f};
                        auto sub = std::make_unique<MenuRow>(s);
                        sub->main = false;
                        MenuRow* sr = static_cast<MenuRow*>(panel_->Add(std::move(sub)));
                        assets::UiLayoutNode l;
                        l.type = "CIFStatic";
                        l.id = 700;
                        l.rect = {-1, 5, 93, 15};
                        l.text = shop->tabs[k].labelKey;
                        if (auto* label = ui::As<ui::Static>(sr->Add(ui_->Make(l)))) {
                            const bool on = view_ == View::Shop && tab_ == shop->tabs[k].id;
                            label->textColor = on ? ui::Rgba(255, 255, 0) : ui::kWhite;
                        }
                        subTabs_[s.id] = shop->tabs[k].id;
                        y += 22.0f;
                    }
                }
            }
        }
        // The basket's own rows: the lump buy and Empty.
        float top = 0.0f;
        if (view_ == View::Basket) {
            for (int k = 0; k < 2; ++k) {
                assets::UiLayoutNode b;
                b.type = "CIFButton";
                b.id = k == 0 ? kBasketBuy : kBasketEmpty;
                b.rect = {kMenuWidth + 8.0f + 100.0f * float(k), 2.0f, 0, 0};
                b.ddj = "interface\\mall\\mall_button.ddj";
                b.text = k == 0 ? "UIIT_STT_BUY" : "UIIT_CTL_SILKMALL_DELETE_BASKET_ALL";
                b.hAlign = 1;
                b.vAlign = 1;
                panel_->Add(ui_->Make(b));
            }
            top = 30.0f;
        }
        // The goods, two to a row, from the scroll's row.
        const std::vector<GoodRef> goods = Goods();
        const int rows = (int(goods.size()) + 1) / 2;
        const int shown = int((513.0f - top) / (kGoodH + kGoodGap));
        scroll_ = std::clamp(scroll_, 0, std::max(0, rows - shown));
        for (int i = scroll_ * 2; i < int(goods.size()) && i < (scroll_ + shown) * 2; ++i) {
            const assets::MallGood* g = GoodOf(goods[std::size_t(i)]);
            if (g == nullptr) continue;
            const int k = i - scroll_ * 2;
            assets::UiLayoutNode n;
            n.type = "CIFStatic";
            n.id = kFirstGood + i;
            n.rect = {kMenuWidth + 6.0f + float(k % 2) * (kGoodW + 8.0f), top + float(k / 2) * (kGoodH + kGoodGap), kGoodW, kGoodH};
            auto slot = std::make_unique<GoodSlot>(n, c_);
            GoodSlot* s = static_cast<GoodSlot*>(panel_->Add(std::move(slot)));
            s->ref = goods[std::size_t(i)];
            s->good = g;
            s->selected = s->ref == selected_;
            AddSection(*ui_, *s, "resinfo\\ifnewitemmallshopitemslot.txt", "Create");
            AddSection(*ui_, *s, "resinfo\\ifnewitemmallshopitemslot.txt", "NewShopSlotIconForPackage");
            FillSlot(*s);
        }
        // The scroll's buttons (the view's mall_scroll art).
        if (rows > shown) {
            for (int k = 0; k < 2; ++k) {
                assets::UiLayoutNode b;
                b.type = "CIFButton";
                b.id = k == 0 ? kScrollUp : kScrollDown;
                b.rect = {535.0f, k == 0 ? top : 493.0f, 16.0f, 20.0f};
                b.ddj = k == 0 ? "interface\\mall\\mall_scroll_up.ddj" : "interface\\mall\\mall_scroll_down.ddj";
                panel_->Add(ui_->Make(b));
            }
        }
    }

    // A slot's fields: the name, the price grouped by three with the silk icon, the old price and its arrow
    // only for a sale (tag 4), the count ("N Unit") or the stock left / Sold out (tag 16), the tag icons from 9,60.
    void FillSlot(GoodSlot& s) {
        const assets::MallGood& g = *s.good;
        const assets::ItemRow* row = RowOf(&g);
        if (auto* name = ui::As<ui::TextBox>(s.Child(kSlotName))) {
            name->lineHeight = 13.0f; // two rows in its 26 (wrapped)
            name->visibleRows = 2;
            name->ClearParagraphs();
            name->AddParagraph(c_.Text(g.nameKey), ui::kWhite);
        }
        Put(s.Child(kSlotPrice), Grouped(g.price));
        const bool sale = (g.saleTag & 4) != 0;
        if (ui::Widget* w = s.Child(kSlotArrow)) w->visible = sale;
        if (ui::Widget* w = s.Child(kSlotOld)) {
            w->visible = sale;
            Put(w, sale ? Grouped(g.previousPrice) : std::string());
        }
        std::string count;
        if ((g.saleTag & 16) != 0) {
            const auto it = c_.mall->stock.find(g.packageId);
            count = it != c_.mall->stock.end() && it->second <= 0 ? c_.Text("UIIT_STT_SILKMALL_ITEM_SOLDOUT")
                                                                   : std::to_string(it != c_.mall->stock.end() ? it->second : 0);
        } else {
            // Tag 32 (a period): ours on sale always (open question 12); the count as any other good's.
            const int n = row != nullptr && game::MaxStack(*row) > 1 && g.data > 0 ? g.data : 1;
            count = std::to_string(n) + " " + c_.Text("UIIT_STT_UNIT");
        }
        Put(s.Child(kSlotCount), count);
        if (auto* icon = ui::As<ui::Slot>(s.Child(kSlotIcon))) {
            icon->pickable = false;
            icon->icon = IconOf(row);
            icon->tooltipLines = [this, gp = &g] {
                const assets::ItemRow* r = RowOf(gp);
                if (r == nullptr) return std::vector<ui::TooltipLine>{};
                const std::vector<game::ItemInstance> items = game::MallItems(*gp, *r, 1, std::int64_t(std::time(nullptr)));
                return items.empty() ? std::vector<ui::TooltipLine>{} : ItemTooltip(*r, items.front(), c_);
            };
        }
        // The tag icons: new, limited, sale, popular, recommendation (stock and period).
        static constexpr struct {
            int bit;
            const char* art;
        } kTags[] = {{1, "interface\\mall\\mall_icon_new.ddj"},
                     {2, "interface\\mall\\mall_icon_limited.ddj"},
                     {4, "interface\\mall\\mall_icon_sale.ddj"},
                     {8, "interface\\mall\\mall_icon_popular.ddj"},
                     {16, "interface\\mall\\mall_icon_recommendation .ddj"},
                     {32, "interface\\mall\\mall_icon_recommendation .ddj"}};
        float x = 9.0f;
        for (const auto& t : kTags) {
            if ((g.saleTag & t.bit) == 0) continue;
            assets::UiLayoutNode n;
            n.type = "CIFStatic";
            n.id = 950 + t.bit;
            n.rect = {x, 60.0f, 0, 0};
            n.ddj = t.art;
            s.Add(ui_->Make(n));
            x += 34.0f;
        }
    }

    void Update(ui::Frame& frame) override {
        ui::MainFrame::Update(frame);
        lastTime_ = frame.time;
        // MyInfo follows the balance and the level by itself.
        if (ui::Widget* my = Child(kMyInfo); my != nullptr && c_.state != nullptr) {
            Put(my->Child(kInfoName), c_.state->self.name);
            Put(my->Child(kInfoLevel), FormatText(c_.Text("UIIT_STT_LV_LEVEL"), {std::to_string(c_.state->self.level)}));
            Put(my->Child(kInfoSilk), std::to_string(c_.state->silk));
            Put(my->Child(kInfoGift), std::to_string(c_.state->giftSilk));
            Put(my->Child(kInfoPoints), std::to_string(c_.state->silkPoints));
        }
        // The wheel over the panel scrolls the goods a row.
        if (panel_ != nullptr && frame.input.wheel() != 0.0f && panel_->Hit(frame.pointer.position)) {
            scroll_ += frame.input.wheel() > 0.0f ? -1 : 1;
            Rebuild();
        }
        // The worn-set Buy only with something worn, every button idle while a purchase runs.
        if (ui::Widget* b = Child(kBuyWorn)) b->enabled = c_.mall != nullptr && !c_.mall->worn.empty() && c_.mall->dealing == 0;
        if (c_.mall != nullptr && version_ != c_.mall->version) {
            version_ = c_.mall->version;
            Rebuild();
        }
        // The popup hides when the mouse leaves it and its slot (a few frames' grace between the two).
        mouse_ = frame.pointer.position;
        if (ui::Widget* popup = Child(kPopup); popup != nullptr && popup->visible && !popup->Hit(mouse_) && !popupSlot_.Contains(mouse_)) {
            if (++popupAway_ > 30) popup->visible = false;
        } else {
            popupAway_ = 0;
        }
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.source == nullptr) return ui::MainFrame::OnNotice(n);
        const int id = n.source->id();
        ui::Widget* parent = n.source->parent();
        const bool clicked = n.kind == ui::Notice::Kind::Clicked;
        if (n.kind == ui::Notice::Kind::Submitted && id == kSearch) return DoSearch(), true;
        if (parent == panel_ && clicked) {
            if (const auto it = menuRows_.find(id); it != menuRows_.end()) {
                const auto& [shop, special] = it->second;
                if (special == 1) {
                    view_ = View::Hot;
                } else if (special == 2) {
                    view_ = View::Basket;
                } else if (shopCode_ == shop) {
                    shopCode_.clear(); // a second click folds its tabs
                } else {
                    shopCode_ = shop;
                    const assets::MallShop* s = c_.mall->catalog->Shop(shop);
                    if (s != nullptr && !s->tabs.empty()) {
                        tab_ = s->tabs.front().id;
                        view_ = View::Shop;
                    }
                }
                scroll_ = 0;
                Rebuild();
                return true;
            }
            if (const auto it = subTabs_.find(id); it != subTabs_.end()) {
                tab_ = it->second;
                view_ = View::Shop;
                scroll_ = 0;
                Rebuild();
                return true;
            }
            if (id == kScrollUp || id == kScrollDown) {
                scroll_ += id == kScrollUp ? -1 : 1;
                Rebuild();
                return true;
            }
            if (id == kBasketBuy) return AskBasket(), true;
            if (id == kBasketEmpty) {
                c_.mall->basket.clear();
                ++c_.mall->version;
                return true;
            }
        }
        if (auto* slot = dynamic_cast<GoodSlot*>(n.source); slot != nullptr && (clicked || n.kind == ui::Notice::Kind::RightClicked)) {
            selected_ = slot->ref;
            for (const auto& child : panel_->children()) {
                if (auto* g = dynamic_cast<GoodSlot*>(child.get())) g->selected = g->ref == selected_;
            }
            if (n.kind == ui::Notice::Kind::RightClicked) ShowPopup(*slot);
            return true;
        }
        if (parent == Child(kPopup) && clicked) {
            if (c_.mall == nullptr || c_.mall->dealing > 0) return true;
            Child(kPopup)->visible = false;
            if (id == kRowBuy) OpenConfirm(selected_);
            if (id == kRowWear) TryOn(selected_);
            if (id == kRowReserve) {
                if (view_ == View::Basket) {
                    std::erase(c_.mall->basket, std::pair{selected_.tab, selected_.slot});
                    ++c_.mall->version;
                } else {
                    AskReserve(selected_);
                }
            }
            return true;
        }
        if (clicked) {
            switch (id) {
            case kMainPage:
                view_ = View::Front;
                shopCode_.clear();
                scroll_ = 0;
                Rebuild();
                return true;
            case kFind: DoSearch(); return true;
            case kOriginal:
                if (c_.mall != nullptr) c_.mall->worn.clear(); // the preview from the character's own
                return true;
            case kBuyWorn: AskWorn(); return true;
            case kBuyList: OpenHistory(); return true;
            default: break;
            }
        }
        return ui::MainFrame::OnNotice(n);
    }

    void DoSearch() {
        auto* e = ui::As<ui::Edit>(Child(kSearch));
        if (e == nullptr || e->text().empty() || e->text() == c_.Text("UIIT_STT_SILKMALL_SEARCH_DEFAULT")) return;
        search_ = e->text();
        if (ui::Widget* popup = Child(kPopup)) popup->visible = false;
        view_ = View::Search;
        scroll_ = 0;
        Rebuild();
    }

    void ShowPopup(GoodSlot& slot) {
        ui::Widget* popup = Child(kPopup);
        if (popup == nullptr) return;
        // Beside the slot where it was clicked (ours: at the mouse, the slot's middle for a script), inside the window and
        // over everything in it (the preview and the goods' panel are made after it). The user's report 2026-11-14: put
        // at the slot's right edge it went under the preview, out of reach.
        const ui::Rect r = slot.ScreenRect();
        const ui::Vec2 o = Origin();
        const bool overSlot = slot.Hit(mouse_);
        const ui::Vec2 at = overSlot ? mouse_ : ui::Vec2{r.x + r.w / 2.0f, r.y + r.h / 2.0f};
        popup->rect.x = std::clamp(at.x - o.x - 6.0f, 10.0f, std::max(10.0f, rect.w - popup->rect.w - 10.0f));
        popup->rect.y = std::clamp(at.y - o.y - 6.0f, 10.0f, std::max(10.0f, rect.h - popup->rect.h - 10.0f));
        popup->BringToFront();
        popup->visible = true;
        popupSlot_ = r;
        popupAway_ = 0;
        const assets::ItemRow* row = RowOf(slot.good);
        if (ui::Widget* w = popup->Child(kRowWear)) w->enabled = row != nullptr && game::MallAvatarKind(*row) != 0; // avatars only
        if (ui::Widget* w = popup->Child(kRowInfo)) w->enabled = false; // a package of one item (every one in 1.188)
        Put(popup->Child(kRowReserve), c_.Text(view_ == View::Basket ? "UIIT_STT_SILKMALL_DEL" : "UIIT_STT_SILKMALL_ZZIM"));
    }

    // "Wear Item": the good on the preview in place of the same kind.
    bool TryOn(const GoodRef& r) {
        const assets::ItemRow* row = RowOf(GoodOf(r));
        if (row == nullptr || game::MallAvatarKind(*row) == 0 || c_.mall == nullptr) {
            if (c_.systemLine) c_.systemLine(c_.Text("UIIT_MSG_STRGERR_EQUIPITEM"));
            return false;
        }
        c_.mall->worn[game::MallAvatarKind(*row)] = {r.tab, r.slot};
        return true;
    }

    // --- The confirm window (CNewItemMallConfirmBuy) ---------------------------------------------------------

    void OpenConfirm(const GoodRef& r) {
        const assets::MallGood* g = GoodOf(r);
        const assets::ItemRow* row = RowOf(g);
        if (g == nullptr || row == nullptr) return;
        ui::Widget* box = Fresh(kConfirm, {(rect.w - 338.0f) * 0.5f, (rect.h - 212.0f) * 0.5f, 338.0f, 212.0f}, "interface\\frame\\mall_ms_wnd_");
        AddSection(*ui_, *box, "resinfo\\ifnewitemmallconfirmbuy.txt", "Create");
        // Buy and Cancel are placed in code (ours: under the frames, the box's pair).
        if (ui::Widget* b = box->Child(kConfirmBuy)) b->rect = {73, 172, 88, 24};
        if (ui::Widget* b = box->Child(kConfirmCancel)) b->rect = {177, 172, 88, 24};
        if (auto* icon = ui::As<ui::Static>(box->Child(kConfirmIcon))) icon->texture = IconOf(row);
        Put(box->Child(kConfirmName), c_.Text(g->nameKey));
        // The silk row (CIFNewItemMallConfirmSlot: label, value, unit; ours at the foot of the first frame).
        assets::UiLayoutNode l;
        l.type = "CIFStatic";
        l.id = kConfirmSilk - 1;
        l.rect = {60, 124, 116, 20};
        l.text = "UIIT_STT_SILKMALL_SILK";
        box->Add(ui_->Make(l));
        assets::UiLayoutNode v;
        v.type = "CIFStatic";
        v.id = kConfirmSilk;
        v.rect = {182, 124, 116, 20};
        v.ddj = "interface\\mall\\mall_box_02.ddj";
        box->Add(ui_->Make(v));
        assets::UiLayoutNode u;
        u.type = "CIFStatic";
        u.id = kConfirmSilk + 1;
        u.rect = {301, 124, 21, 20};
        u.text = "UIIT_STT_ROLL_OF_CLOTH";
        box->Add(ui_->Make(u));
        if (auto* e = ui::As<ui::Edit>(box->Child(kCountEdit))) {
            e->numeric = true;
            e->limit = 5;
        }
        confirm_ = r;
        count_ = 1;
        ShowCount();
    }
    void ShowCount() {
        ui::Widget* box = Child(kConfirm);
        const assets::MallGood* g = GoodOf(confirm_);
        const assets::ItemRow* row = RowOf(g);
        if (box == nullptr || g == nullptr || row == nullptr) return;
        count_ = game::MallClampCount(*g, *row, count_);
        if (auto* e = ui::As<ui::Edit>(box->Child(kCountEdit))) e->SetValue(std::to_string(count_));
        Put(box->Child(kConfirmSilk), Grouped(game::MallPrice(*g, count_)));
    }
    // Buy: the bag, then the silk (own + gift + points), then the request.
    bool ConfirmBuy() {
        ui::Widget* box = Child(kConfirm);
        const assets::MallGood* g = GoodOf(confirm_);
        const assets::ItemRow* row = RowOf(g);
        if (box == nullptr || g == nullptr || row == nullptr || c_.state == nullptr || c_.mall == nullptr) return false;
        if (auto* e = ui::As<ui::Edit>(box->Child(kCountEdit)); e != nullptr && !e->text().empty()) count_ = std::atoi(e->text().c_str());
        ShowCount();
        if (FreePlaces() < game::MallPlacesNeeded(*g, *row, count_)) return Refused(code::kBagFull), false;
        if (Silk() < game::MallPrice(*g, count_)) return Refused(code::kNoSilk), false;
        c_.mall->dealing = 1;
        dealtGoods_ = {confirm_};
        if (c_.mallRequest) c_.mallRequest(proto::MallBuy::kId, proto::Encode(proto::MallBuy{confirm_.tab, std::uint8_t(confirm_.slot), g->packageId, std::uint16_t(count_), 0}));
        CloseChild(kConfirm);
        return true;
    }
    int FreePlaces() const {
        if (c_.state == nullptr) return 0;
        int free = 0;
        for (int p = game::slot::kWorn; p < c_.state->inventory.size; ++p) free += c_.state->inventory.At(std::uint8_t(p)) == nullptr;
        return free;
    }
    std::uint64_t Silk() const {
        return c_.state != nullptr ? std::uint64_t(c_.state->silk) + c_.state->giftSilk + c_.state->silkPoints : 0;
    }

    // --- The mall's boxes (CIFNewItemMallMessageBox) ----------------------------------------------------------------------

    ui::Widget* OpenBox(const char* section, ui::Vec2 size, const std::string& title) {
        ui::Widget* box = Fresh(kBox, {(rect.w - size.x) * 0.5f, (rect.h - size.y) * 0.5f, size.x, size.y}, "interface\\frame\\mall_ms_wnd_");
        AddSection(*ui_, *box, "resinfo\\ifnewitemmallmessagebox.txt", section);
        assets::UiLayoutNode t;
        t.type = "CIFStatic";
        t.id = 1399;
        t.rect = {0, 12, size.x, 14};
        t.hAlign = 1;
        t.text = title;
        box->Add(ui_->Make(t));
        boxKind_ = section;
        return box;
    }
    // A refusal over the mall while it is open: its text and OK.
    void Refused(std::uint16_t c) {
        const char* key = c == code::kBagFull                       ? "UIIT_MSG_STRGERR_INVENTORY_FULL"
                          : c == code::kNoSilk                      ? "UIIT_MSG_STRGERR_NOT_ENOUGH_SILK_TO_BUY_ITEM"
                          : c == code::kSoldOut || c == 0x18AA      ? "UIIT_MSG_SILKMALL_SOLDOUT_ITEM"
                          : c == code::kBusy                        ? "UIIT_MSG_STRGERR_CANT_USEITEM_WHILE_INTERACT"
                                                                    : "UIIT_MSG_STRGERR_TRADE_FAILURE";
        ui::Widget* box = OpenBox("Create", {338, 160}, c_.Text("UIIT_STT_SILKMALL"));
        assets::UiLayoutNode t;
        t.type = "CIFStatic";
        t.id = 1391;
        t.rect = {20, 60, 298, 20};
        t.hAlign = 1;
        t.text = key;
        box->Add(ui_->Make(t));
        assets::UiLayoutNode ok;
        ok.type = "CIFButton";
        ok.id = kNoticeOk;
        ok.rect = {125, 115, 88, 24};
        ok.ddj = "interface\\mall\\mall_coice_button.ddj";
        ok.text = "UIIS_CTL_OK";
        ok.hAlign = ok.vAlign = 1;
        box->Add(ui_->Make(ok));
        if (c_.systemLine) c_.systemLine(c_.Text(key));
    }
    // "Reserve": the Zzim box (its name, icon, price and the PML question; 1609 adds it).
    void AskReserve(const GoodRef& r) {
        const assets::MallGood* g = GoodOf(r);
        if (g == nullptr) return;
        ui::Widget* box = OpenBox("Zzim", {400, 250}, c_.Text("UIIT_STT_SILKMALL_ZZIM"));
        Put(box->Child(1603), c_.Text(g->nameKey));
        if (auto* icon = ui::As<ui::Static>(box->Child(1604))) icon->texture = IconOf(RowOf(g));
        Put(box->Child(1601), Grouped(g->price));
        if (auto* pml = box->Child(1607)) pml->SetText(c_.Text("UIIT_MSG_SILKMALL_ZZIM_CONFIRM"));
        boxGood_ = r;
    }
    void AskWorn() {
        if (c_.mall == nullptr || c_.mall->worn.empty()) return;
        std::vector<GoodRef> goods;
        for (const auto& [kind, g] : c_.mall->worn) goods.push_back({g.first, g.second});
        AskLump("AllBuy", {370, 345}, goods, 1810, 1809, -1);
    }
    void AskBasket() {
        if (c_.mall == nullptr || c_.mall->basket.empty()) return;
        std::vector<GoodRef> goods;
        for (const auto& [tab, slot] : c_.mall->basket) goods.push_back({tab, slot});
        AskLump("AllBuyZzim", {375, 268}, goods, 2013, 2012, 2011);
    }
    // A lump buy's box: the total, the places left after it, the count of goods (the basket's).
    void AskLump(const char* section, ui::Vec2 size, const std::vector<GoodRef>& goods, int total, int space, int count) {
        std::uint64_t price = 0;
        int places = 0;
        for (const GoodRef& r : goods) {
            const assets::MallGood* g = GoodOf(r);
            const assets::ItemRow* row = RowOf(g);
            if (g == nullptr || row == nullptr) continue;
            price += g->price;
            places += game::MallPlacesNeeded(*g, *row, 1);
        }
        ui::Widget* box = OpenBox(section, size, c_.Text(std::string_view(section) == "AllBuy" ? "UIIT_STT_BUY" : "UIIT_STT_SILKMALL_ZZIM"));
        Put(box->Child(total), Grouped(price));
        Put(box->Child(space), std::to_string(std::max(0, FreePlaces() - places)));
        if (count >= 0) Put(box->Child(count), std::to_string(goods.size()));
        if (ui::Widget* list = box->Child(1801)) {
            // The worn goods with their prices (ours: lines in the list's place).
            float y = 4.0f;
            for (const GoodRef& r : goods) {
                const assets::MallGood* g = GoodOf(r);
                if (g == nullptr) continue;
                assets::UiLayoutNode line;
                line.type = "CIFStatic";
                line.id = 1830;
                line.rect = {14, y, 300, 16};
                line.text = c_.Text(g->nameKey) + "  " + Grouped(g->price);
                list->Add(ui_->Make(line));
                y += 18.0f;
            }
        }
        lump_ = goods;
        lumpPrice_ = price;
        lumpPlaces_ = places;
    }
    // A lump buy (the worn set's or the basket's), as the original's: silk against all and the bag against all first, then
    // one purchase per good, the next sent when the one before is answered (each bought good off the basket at once); a
    // refusal stops the rest.
    bool BuyLump() {
        if (c_.mall == nullptr || lump_.empty()) return false;
        if (FreePlaces() < lumpPlaces_) return Refused(code::kBagFull), false;
        if (Silk() < lumpPrice_) return Refused(code::kNoSilk), false;
        lumpQueue_ = lump_;
        lumpFromBasket_ = boxKind_ == "AllBuyZzim" || view_ == View::Basket;
        CloseChild(kBox);
        return BuyNextOfLump();
    }
    bool BuyNextOfLump() {
        while (!lumpQueue_.empty()) {
            const GoodRef r = lumpQueue_.front();
            const assets::MallGood* g = GoodOf(r);
            if (g == nullptr) {
                lumpQueue_.erase(lumpQueue_.begin());
                continue;
            }
            c_.mall->dealing = 1;
            dealtGoods_ = {r};
            if (c_.mallRequest) c_.mallRequest(proto::MallBuy::kId, proto::Encode(proto::MallBuy{r.tab, std::uint8_t(r.slot), g->packageId, 1, 0}));
            return true;
        }
        return false;
    }

    // A purchase's answer: the dealing over; done, the basket's bought good out of it and a lump's next sent; refused, the
    // box (and a lump stopped).
    void Bought(const proto::MallBought& a) {
        if (c_.mall == nullptr) return;
        c_.mall->dealing = 0;
        const bool inLump = !lumpQueue_.empty();
        if (a.code != code::kOk) {
            lumpQueue_.clear();
            return Refused(a.code);
        }
        if (inLump ? lumpFromBasket_ : (boxKind_ == "AllBuyZzim" || view_ == View::Basket)) {
            for (const GoodRef& r : dealtGoods_) std::erase(c_.mall->basket, std::pair{r.tab, r.slot});
        }
        dealtGoods_.clear();
        ++c_.mall->version;
        if (ui_ != nullptr) ui_->Sound("SND_ITEM_BUY");
        if (inLump) {
            lumpQueue_.erase(lumpQueue_.begin());
            BuyNextOfLump();
        }
    }

    bool BoxNotice(const ui::Notice& n) {
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr) return false;
        const int id = n.source->id();
        if (id == kZzimYes && c_.mall != nullptr) {
            const std::pair<std::uint32_t, int> g{boxGood_.tab, boxGood_.slot};
            if (std::find(c_.mall->basket.begin(), c_.mall->basket.end(), g) == c_.mall->basket.end()) c_.mall->basket.push_back(g);
            ++c_.mall->version;
            CloseChild(kBox);
            return true;
        }
        if (id == kAllBuyYes || id == kZzimAllYes) return BuyLump(), true;
        if (id == kZzimNo || id == kAllBuyNo || id == kZzimAllNo || id == kNoticeOk) return CloseChild(kBox), true;
        return false;
    }
    bool ConfirmNotice(const ui::Notice& n) {
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr) return false;
        const int id = n.source->id();
        if (id == kConfirmBuy) return ConfirmBuy(), true;
        if (id == kConfirmCancel) return CloseChild(kConfirm), true;
        if (id == kCountUp || id == kCountDown) {
            count_ += id == kCountUp ? 1 : -1;
            ShowCount();
            return true;
        }
        return false;
    }

    // --- The silk history (CNIFSilkBuyList, res_ui\silksearch.2dt) --------------------------------------------------

    void OpenHistory() {
        if (c_.layout2dt == nullptr) return;
        const auto* records = c_.layout2dt("res_ui\\silksearch.2dt");
        if (records == nullptr || records->empty()) return;
        const assets::Ui2dtRecord& root = records->front();
        // The root a window of its own (its title, its close button; kind 0 = CIFMainFrame), its combo (kind 12) a combo.
        ui::Widget* w = Fresh(kHistory, {(rect.w - root.rect.w) * 0.5f, (rect.h - root.rect.h) * 0.5f, root.rect.w, root.rect.h}, root.frame,
                              "CIFMainFrame", root.textKey);
        if (!historyBuilt_) {
            historyBuilt_ = true;
            Build2dt(*ui_, *w, *records, [](const assets::Ui2dtRecord& r, assets::UiLayoutNode& n) {
                if (r.kind == 12) n.type = "CIFComboBox";
            });
        }
        if (auto* combo = ui::As<ui::ComboBox>(w->Find(10))) {
            combo->items = {c_.Text("UIIT_CTL_SILK_INQUIRY_ONEDAY"), c_.Text("UIIT_CTL_SILK_INQUIRY_WEEK"), c_.Text("UIIT_CTL_SILK_INQUIRY_MONTH"),
                            c_.Text("UIIT_CTL_SILK_INQUIRY_THREE_MONTH")};
            combo->Select(0);
        }
        if (ui::Widget* status = w->Find(194)) status->SetText({});
        if (c_.mall != nullptr) c_.mall->historyState = 0;
        historyShown_ = 0;
    }
    bool HistoryNotice(const ui::Notice& n) {
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr || n.source->id() != 12) return false;
        ui::Widget* w = Child(kHistory);
        auto* combo = w != nullptr ? ui::As<ui::ComboBox>(w->Find(10)) : nullptr;
        if (combo == nullptr || combo->selected < 0 || c_.mall == nullptr) return true;
        Put(w->Find(194), c_.Text("UIIT_MSG_SILK_INQUIRY_WAIT"));
        c_.mall->historyState = 1;
        if (c_.mallRequest) c_.mallRequest(proto::SilkHistoryQuery::kId, proto::Encode(proto::SilkHistoryQuery{1, std::uint8_t(combo->selected + 1)}));
        return true;
    }
    void ShowHistory() {
        ui::Widget* w = Child(kHistory);
        if (w == nullptr || c_.mall == nullptr || c_.mall->historyState != 2 || historyShown_ == c_.mall->historyVersion) return;
        historyShown_ = c_.mall->historyVersion;
        const proto::SilkHistory& h = c_.mall->history;
        Put(w->Find(194), h.rows.empty() ? c_.Text("UIIT_MSG_SILK_INQUIRY_NOT_INQUIRY") : std::string());
        // The 11 rows: the number (%03d), the silk, the date (%4d/%02d/%02d %02d:%02d).
        static constexpr int kRows[] = {28, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144};
        std::uint64_t total = 0;
        for (const proto::SilkHistoryRecord& r : h.rows) total += r.silk;
        for (std::size_t k = 0; k < std::size(kRows); ++k) {
            ui::Widget* row = w->Find(kRows[k]);
            if (row == nullptr) continue;
            const bool has = k < h.rows.size();
            row->visible = has;
            if (!has) continue;
            const proto::SilkHistoryRecord& r = h.rows[k];
            Put(row->Child(25), std::format("{:03}", k + 1));
            Put(row->Child(26), Grouped(r.silk));
            Put(row->Child(27), std::format("{:4}/{:02}/{:02} {:02}:{:02}", 2000 + (r.when & 0x3F), (r.when >> 6) & 0xF, (r.when >> 10) & 0x1F,
                                            (r.when >> 15) & 0x1F, (r.when >> 20) & 0x3F));
        }
        Put(w->Find(401), Grouped(total) + " " + c_.Text("UIIT_STT_ROLL_OF_CLOTH"));
    }

    void Draw(ui::Frame& frame) override {
        ShowHistory();
        ui::MainFrame::Draw(frame);
    }

    // A sub-window (the confirm window, a box, the history) shown afresh: made once, its controls made again each time.
    ui::Widget* Fresh(int id, ui::Rect where, const std::string& frame, const char* type = "CIFFrame", const std::string& title = {}) {
        ui::Widget* w = Child(id);
        if (w == nullptr) {
            assets::UiLayoutNode n;
            n.type = type;
            n.id = id;
            n.ddj = frame;
            n.text = title;
            n.rect = {where.x, where.y, where.w, where.h};
            w = Add(ui_->Make(n));
        } else {
            if (std::string_view(type) == "CIFFrame") w->ClearChildren(); // a window of its own keeps its controls
            w->rect = where;
        }
        w->visible = true;
        w->background = ui::Rgba(18, 15, 10, 240); // ours: the window's own backdrop under the frame's art (not traced)
        return w;
    }
    void CloseChild(int id) {
        if (ui::Widget* w = Child(id)) w->visible = false;
        if (id == kBox) boxKind_.clear();
    }

    View view_ = View::Front;
    std::string shopCode_, search_;
    std::uint32_t tab_ = 0;
    int scroll_ = 0;
    GoodRef selected_, confirm_, boxGood_;
    int count_ = 1;
    std::vector<GoodRef> lump_, dealtGoods_;
    std::vector<GoodRef> lumpQueue_; // a lump buy's goods still to buy, the first one's purchase awaited
    bool lumpFromBasket_ = false;    // the lump is the basket's (its goods leave it as they are bought)
    std::uint64_t lumpPrice_ = 0;
    int lumpPlaces_ = 0;
    std::string boxKind_;
    std::uint64_t historyShown_ = 0;
    bool historyBuilt_ = false;
    HudContext& c_;

private:
    ui::Interface* ui_ = nullptr;
    ui::Widget* panel_ = nullptr;
    std::map<int, std::pair<std::string, int>> menuRows_;
    std::map<int, std::uint32_t> subTabs_;
    std::uint64_t version_ = 0;
    int popupAway_ = 0;
    ui::Rect popupSlot_{};   // the slot the popup was opened from (on screen)
    ui::Vec2 mouse_{};       // the mouse at the last update
    double lastTime_ = 0;
};

// The window's sub-windows' notices go to it first (they are its children; their ids repeat the main's).
class MallWindowRouter {
public:
    static bool Route(MallWindow& w, const ui::Notice& n) {
        for (ui::Widget* p = n.source; p != nullptr && p != &w; p = p->parent()) {
            if (p->id() == kConfirm) return w.ConfirmNotice(n);
            if (p->id() == kBox) return w.BoxNotice(n);
            if (p->id() == kHistory) return w.HistoryNotice(n);
        }
        return false;
    }
};

class RoutedMallWindow : public MallWindow {
public:
    using MallWindow::MallWindow;
    bool OnNotice(const ui::Notice& n) override {
        if (MallWindowRouter::Route(*this, n)) return true;
        return MallWindow::OnNotice(n);
    }
};

MallWindow* Shown(HudContext& c) {
    return c.hud != nullptr && c.hud->Shown(kWindow) ? dynamic_cast<MallWindow*>(c.hud->Show(kWindow)) : nullptr;
}

} // namespace

void RegisterMallWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CIFNewItemMall", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<RoutedMallWindow>(n, *c); });
    ui.Register("CIFNewItemSlotPopupWnd", [](const assets::UiLayoutNode& n) { return std::make_unique<ui::FrameBorder>(n); });
}

void AskItemMall(HudContext& c) {
    if (c.hud == nullptr) return;
    if (c.hud->Shown(kWindow)) {
        CloseItemMall(c);
        return;
    }
    // The box kind 0x1E: "Enter Item Mall", the picture, the three warnings; Yes opens the mall.
    c.hud->Confirm("UIIT_STT_SILKMALL_ENTER", {c.Text("UIIT_STT_SILKMALL_WARNING_1"), c.Text("UIIT_STT_SILKMALL_WARNING_2"), c.Text("UIIT_STT_SILKMALL_WARNING_3")},
                   [cp = &c](bool yes) {
                       if (yes) OpenItemMall(*cp);
                   });
    if (ui::Interface* ui = c.hud->interface()) {
        GrowConfirm(*ui, 32.0f);
        const auto windows = ui->Windows();
        for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
            auto* box = dynamic_cast<ConfirmBox*>(*it);
            if (box == nullptr || !box->visible) continue;
            // 400 x 210 (the kind's size): widened, the picture at 128,43 (148 x 52), the warnings under it. The frame's
            // own controls on its right (the close button) move with the edge, the rest stays centred (the user's
            // report 2026-11-15: the X short of the corner, the first line on the picture: a text box drops empty
            // lines, so spacing it by them did nothing).
            const float wider = 400.0f - box->rect.w;
            box->rect.w = 400.0f;
            for (const auto& child : box->children()) {
                if (child->id() == 2) child->rect.x += wider;          // the close button
                else if (child->rect.w > 200.0f) child->rect.w += wider; // the title, the drag strip, the text, the tile
                else child->rect.x += wider * 0.5f;
            }
            if (ui::Widget* text = box->Child(251)) {
                text->rect.y = 100.0f;
                text->rect.h = 3.0f * 18.0f + 6.0f;
            }
            assets::UiLayoutNode pic;
            pic.type = "CIFStatic";
            pic.id = 260;
            pic.rect = {128, 43, 148, 52};
            pic.ddj = "interface\\mall\\mall_pic.ddj";
            box->Add(ui->Make(pic));
            ui->Centre(*box, c.hud->Screen());
            break;
        }
    }
}

void OpenItemMall(HudContext& c) {
    if (c.hud == nullptr) return;
    if (auto* w = dynamic_cast<MallWindow*>(c.hud->Show(kWindow))) {
        w->Open();
        if (c.mallRequest) c.mallRequest(proto::MallOpen::kId, proto::Encode(proto::MallOpen{}));
    }
}

void CloseItemMall(HudContext& c) {
    MallWindow* w = Shown(c);
    if (w == nullptr) return;
    // Refused while a purchase runs; the basket emptied, the history's inquiry cancelled.
    if (c.mall != nullptr && c.mall->dealing > 0) return;
    if (c.mall != nullptr) {
        if (c.mall->historyState == 1 && c.mallRequest) c.mallRequest(proto::SilkHistoryQuery::kId, proto::Encode(proto::SilkHistoryQuery{1, 0xFF}));
        c.mall->basket.clear();
        c.mall->worn.clear();
    }
    c.hud->Hide(kWindow);
    if (ui::Interface* ui = c.hud->interface()) ui->Sound("SND_WINDOW_CLOSE");
}

void ItemMallAnswer(HudContext& c, const proto::MallEvent& event) {
    if (c.mall == nullptr) return;
    if (const auto* state = std::get_if<proto::MallState>(&event)) {
        c.mall->stock.clear();
        for (const proto::MallStock& s : state->stock) c.mall->stock[s.package] = s.left;
        ++c.mall->version;
    } else if (const auto* bought = std::get_if<proto::MallBought>(&event)) {
        if (MallWindow* w = Shown(c)) {
            w->Bought(*bought);
        } else {
            c.mall->dealing = 0;
        }
    } else if (const auto* history = std::get_if<proto::SilkHistory>(&event)) {
        c.mall->history = *history;
        c.mall->historyState = 2;
        ++c.mall->historyVersion;
    } else if (const auto* line = std::get_if<proto::SilkLine>(&event); line != nullptr && c.state != nullptr) {
        // The silk line: reasons 0..5 their texts with the value, 0xFF the trade's failure.
        static constexpr const char* kKeys[] = {"UIIT_STT_SILK_BUY_RESULT",  "UIIT_STT_SILK_BUY_CANCLE",        "UIIT_STT_SILK_BUY_RECEIVE",
                                                "UIIT_STT_SILK_SEND_RESULT", "UIIT_STT_SILK_POINT_BUY_RECEIVE", "UIIT_STT_SILK_POINT_USE_RECEIVE"};
        const std::string text = line->reason < 6 ? FormatText(c.Text(kKeys[line->reason]), {std::to_string(line->value)})
                                                  : c.Text("UIIT_MSG_STRGERR_TRADE_FAILURE");
        c.state->Post(game::GameMessage::Kind::General, text, c.time);
    }
}

bool ItemMallStep(HudContext& c, const std::string& step) {
    // Scripts (PlayMall.cpp): the window's controls as a player's clicks would work them.
    MallWindow* w = Shown(c);
    const auto arg = [&](std::size_t at) { return step.substr(at); };
    if (step == "mall") {
        if (w == nullptr) OpenItemMall(c);
        return true;
    }
    if (w == nullptr || c.mall == nullptr || c.mall->catalog == nullptr) return false;
    if (step.starts_with("mall-shop:")) { // a menu row by its shop code, HOT or BASKET
        const std::string code = arg(10);
        w->view_ = code == "HOT" ? MallWindow::View::Hot : code == "BASKET" ? MallWindow::View::Basket : MallWindow::View::Shop;
        if (w->view_ == MallWindow::View::Shop) {
            const assets::MallShop* s = c.mall->catalog->Shop(code);
            if (s == nullptr || s->tabs.empty()) return false;
            w->shopCode_ = code;
            w->tab_ = s->tabs.front().id;
        }
        w->scroll_ = 0;
        w->Rebuild();
        return true;
    }
    if (step.starts_with("mall-tab:")) { // a tab of the open shop by its index
        const assets::MallShop* s = c.mall->catalog->Shop(w->shopCode_);
        const int k = std::atoi(arg(9).c_str());
        if (s == nullptr || k < 0 || k >= int(s->tabs.size())) return false;
        w->tab_ = s->tabs[std::size_t(k)].id;
        w->view_ = MallWindow::View::Shop;
        w->scroll_ = 0;
        w->Rebuild();
        return true;
    }
    if (step.starts_with("mall-search:")) {
        if (auto* e = ui::As<ui::Edit>(w->Child(kSearch))) e->SetValue(arg(12));
        w->DoSearch();
        return true;
    }
    // The good of an item's codename or a package's code (its first place in the catalogue).
    const auto good = [&](const std::string& name) -> std::optional<GoodRef> {
        for (const assets::MallShop& s : c.mall->catalog->shops()) {
            for (const assets::MallTab& t : s.tabs) {
                for (const assets::MallGood& g : t.goods) {
                    if (g.itemCodename == name || g.package == name) return GoodRef{t.id, g.slot};
                }
            }
        }
        return std::nullopt;
    };
    if (step.starts_with("mall-buy:")) { // mall-buy:CODE[,COUNT]: the popup's Buy, the count, the confirm's Buy
        std::string name = arg(9);
        int count = 1;
        if (const auto comma = name.find(','); comma != std::string::npos) {
            count = std::atoi(name.substr(comma + 1).c_str());
            name.resize(comma);
        }
        const auto r = good(name);
        if (!r) return false;
        w->selected_ = *r;
        w->OpenConfirm(*r);
        w->count_ = count;
        w->ShowCount();
        return true;
    }
    if (step == "mall-yes") { // the open confirm's Buy, or the open box's yes
        if (w->Child(kConfirm) != nullptr && w->Child(kConfirm)->visible) return w->ConfirmBuy() || true;
        if (w->boxKind_ == "Zzim") return w->BoxNotice({ui::Notice::Kind::Clicked, kZzimYes, w->Child(kBox)->Child(kZzimYes)}) || true;
        if (w->boxKind_ == "AllBuy" || w->boxKind_ == "AllBuyZzim") return w->BuyLump() || true;
        if (!w->boxKind_.empty()) return w->CloseChild(kBox), true;
        return false;
    }
    if (step.starts_with("mall-wear:")) { // "Wear Item" of a good
        const auto r = good(arg(10));
        return r && w->TryOn(*r);
    }
    if (step == "mall-original") {
        c.mall->worn.clear();
        return true;
    }
    if (step == "mall-buy-worn") return w->AskWorn(), true;
    if (step.starts_with("mall-reserve:")) { // the popup's Reserve: its box
        const auto r = good(arg(13));
        if (!r) return false;
        w->AskReserve(*r);
        return true;
    }
    if (step == "mall-basket-buy") return w->AskBasket(), true;
    if (step.starts_with("mall-popup:")) { // a good's right click
        const auto r = good(arg(11));
        if (!r) return false;
        w->selected_ = *r;
        for (const auto& child : w->Child(kPanel)->children()) {
            if (auto* s = dynamic_cast<GoodSlot*>(child.get()); s != nullptr && s->ref == *r) {
                w->ShowPopup(*s);
                return true;
            }
        }
        return false;
    }
    if (step == "mall-history") return w->OpenHistory(), true;
    if (step.starts_with("mall-inquire:")) { // the history's period (0..3) and Inquire
        ui::Widget* h = w->Child(kHistory);
        auto* combo = h != nullptr ? ui::As<ui::ComboBox>(h->Find(10)) : nullptr;
        if (combo == nullptr) return false;
        combo->Select(std::atoi(arg(13).c_str()));
        return w->HistoryNotice({ui::Notice::Kind::Clicked, 12, h->Find(12)});
    }
    if (step == "mall-close") return CloseItemMall(c), true;
    return false;
}

float ItemMallPreviewYaw(const HudContext& c) { return c.mall != nullptr ? c.mall->previewYaw : 0.0f; }

} // namespace sr::client
