// The jobs' trade on the client: the specialty deal window (1.188's CIFSpecialtyDeal,
// ifspecialtydeal.txt), the price board (CIFTradeInfoWnd, iftradeinfo.txt), the answers' and the bargain hours' lines.
// The transport's bag with its total, stars and Sell All is the COS window's (HudCos.cpp).

#include <algorithm>
#include <cmath>
#include <format>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/game/JobProtocol.h"
#include "sr/game/Jobs.h"
#include "sr/game/PlayerState.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr int kDealWindow = 67, kBoardWindow = 159; // ginterface.txt's ids (GDR_SPECIALTY_DEAL, GDR_TRADE_INFO_WND)

void Put(ui::Widget* w, const std::string& text) {
    if (w != nullptr && w->text() != text) w->SetText(text);
}

void Line(HudContext& c, const std::string& text, bool banner) {
    if (c.state == nullptr || text.empty()) return;
    c.state->Post(game::GameMessage::Kind::General, text, c.time);
    if (banner) c.state->Notify(game::Banner::Kind::Warning, text, c.time);
}

// What a good brings at the open shop (0x30E0's price, else its sale price; the thieves' buyer 75 % of its Price).
std::uint64_t SellPriceOf(const HudContext& c, const assets::ItemRow& row) {
    // Less the fortress tax of the NPC's town.
    if (c.tradeStolenBuyer) return c.TaxedSale(std::uint64_t(std::trunc(double(game::kGoodsPrice) * 0.75)));
    if (const auto it = c.tradePrices.find(row.refObjId); it != c.tradePrices.end()) return c.TaxedSale(it->second);
    return c.TaxedSale(row.sellPrice);
}

// CIFSpecialtyDeal (its map): a good's purchase into the transport (buy) or its sale from it
// (sell): 18 the name, 30 the count, 31 the purchase's sum, 33 the sale's, 32 the profit, 34 the stars (ours: the
// trade's now), 40 OK.
class DealWindow : public ui::MainFrame {
public:
    DealWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
        if (auto* edit = ui::As<ui::Edit>(Child(30))) edit->numeric = true;
    }

    void Open(const TradeDeal& deal) {
        deal_ = deal;
        Put(Child(18), deal.name);
        if (auto* edit = ui::As<ui::Edit>(Child(30))) {
            edit->SetValue(std::to_string(deal.count));
            if (ui_ != nullptr) ui_->Focus(edit);
        }
        if (ui::Widget* combo = Child(34)) combo->enabled = false;
        for (int id : {22, 23, 32, 33}) { // a purchase has no sale nor profit (their labels greyed)
            if (ui::Widget* w = Child(id)) w->enabled = !deal.buy;
        }
    }

    void Update(ui::Frame& frame) override {
        MainFrame::Update(frame);
        const int count = Count();
        const std::uint64_t bought = std::uint64_t(count) * (deal_.buy ? deal_.unitPrice : game::kGoodsPrice);
        Put(Child(31), GoldText(bought));
        if (deal_.buy) {
            Put(Child(33), {});
            Put(Child(32), {});
        } else {
            const std::uint64_t sold = std::uint64_t(count) * deal_.unitPrice;
            Put(Child(33), GoldText(sold));
            Put(Child(32), sold >= bought ? GoldText(sold - bought) : "-" + GoldText(bought - sold));
        }
        Put(Child(34), std::to_string(c_.tradeStars));
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr && n.source->parent() == this && n.control == 40) {
            Confirm();
            return true;
        }
        return MainFrame::OnNotice(n);
    }

    // OK: a purchase in steps of a stack, the next on each success; a sale at once.
    void Confirm() {
        const int count = Count();
        visible = false;
        if (count <= 0 || c_.hud == nullptr) return;
        if (deal_.buy) {
            c_.tradeBuyLeft = count;
            c_.tradeBuyTab = deal_.tab;
            c_.tradeBuySlot = deal_.slot;
            TradeBuyStep(c_);
        } else if (c_.tradeRequest) {
            c_.tradeRequest(proto::TradeSell::kId, proto::Encode(proto::TradeSell{c_.tradeNpc, std::uint8_t(deal_.place), std::uint16_t(count)}));
        }
    }

private:
    int Count() const {
        const ui::Widget* w = const_cast<DealWindow*>(this)->Child(30);
        const int typed = w != nullptr ? std::atoi(w->text().c_str()) : 0;
        return std::clamp(typed, 0, std::max(0, deal_.most));
    }

    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    TradeDeal deal_;
};

// the trade centres (21..31, specialnpcdata.txt's NPCs, named "<NPC>_T"), Refresh (41)
// asks the picked one's quotes (0x74D4), its goods and quotes in the rows (51..61: 11 the good, 13 "%d%%").
class BoardWindow : public ui::MainFrame {
public:
    BoardWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        if (const assets::UiLayout* l = ui.Layout("resinfo\\iftradeinfo.txt")) {
            for (const auto& s : l->sections) {
                if (s.name != "SlotList") continue;
                for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) Add(ui.Make(*it));
            }
        }
    }

    void Update(ui::Frame& frame) override {
        MainFrame::Update(frame);
        for (int i = 0; i < 11; ++i) {
            ui::Widget* slot = Child(21 + i);
            if (slot == nullptr) continue;
            const std::size_t k = std::size_t(i);
            std::string name;
            if (k < c_.tradeCentres.size() && c_.characters != nullptr) {
                if (const assets::ObjectRow* row = c_.characters->Find(c_.tradeCentres[k])) name = c_.Text(row->nameKey + "_T");
            }
            Put(slot->Child(0) != nullptr ? slot->Child(0) : slot, name); // a bar's caption (Bar::Created)
            if (auto* bar = dynamic_cast<ui::Bar*>(slot)) bar->pieces = int(k) == picked_ ? "interface\\ifcommon\\com_bar01select_" : std::string();
        }
        if (seen_ != c_.tradeBoardVersion) {
            seen_ = c_.tradeBoardVersion;
            for (int i = 0; i < 11; ++i) {
                ui::Widget* row = Child(51 + i);
                if (row == nullptr) continue;
                const std::size_t k = std::size_t(i);
                const bool shown = k < c_.tradeBoard.quotes.size();
                const assets::ItemRow* item = shown && c_.items != nullptr ? c_.items->Find(c_.tradeBoard.quotes[k].item) : nullptr;
                Put(row->Find(11), item != nullptr ? c_.Text(item->nameKey) : std::string());
                Put(row->Find(13), shown ? std::format("{}%", c_.tradeBoard.quotes[k].quote) : std::string());
            }
        }
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr && n.source->parent() == this) {
            if (n.control >= 21 && n.control <= 31) {
                picked_ = n.control - 21;
                return true;
            }
            if (n.control == 41 && picked_ >= 0 && std::size_t(picked_) < c_.tradeCentres.size() && c_.tradeRequest) {
                c_.tradeRequest(proto::TradeBoardQuery::kId, proto::Encode(proto::TradeBoardQuery{c_.tradeCentres[std::size_t(picked_)]}));
                return true;
            }
        }
        return MainFrame::OnNotice(n);
    }

    void Pick(int k) { picked_ = k; }

private:
    HudContext& c_;
    int picked_ = -1;
    std::uint64_t seen_ = ~0ull;
};

} // namespace

const char* TradeCodeKey(std::uint16_t code, bool* banner) {
    // The error table's group 1: a system line, the banner where marked.
    const auto with = [&](const char* key, bool shown) {
        if (banner != nullptr) *banner = shown;
        return key;
    };
    switch (proto::TradeCode(code)) {
    case proto::TradeCode::NotTrader: return with("UIIT_MSG_STRGERR_ONLY_TRADER_CAN_BUY_SPECIALTY", false);
    case proto::TradeCode::NotTradeLevel: return with("UIIT_MSG_SPECIALTY_ERR_NOTTRADE", false);
    case proto::TradeCode::QuestNotDone: return with("UIIT_MSG_SPECIALTY_ERR_QUEST_NOTCOMPLETE", false);
    case proto::TradeCode::NotSellTime: return with("UIIT_MSG_SPECIALTY_ERR_NOTSELL", false);
    case proto::TradeCode::BargainLocked: return with("UIIT_MSG_SPECIALTY_ERR_TRADESHOP", false);
    case proto::TradeCode::MaxTrading: return with("UIIT_MSG_STRGERR_MAX_TRADING_LIMIT_EXCEEDED", false);
    case proto::TradeCode::TooFarFromCart: return with("UIIT_MSG_STRGERR_CANT_DEAL_SPECIALTY_SINCE_TOO_FAR_FROM_CART", false);
    case proto::TradeCode::NotDealer: return with("UIIT_MSG_STRGERR_DEALER_DONT_TREAT_THIS_ITEM", false);
    case proto::TradeCode::StolenGoods: return with("UIIT_MSG_STRGERR_DEALER_DONT_TREAT_STOLEN_ITEM", false);
    case proto::TradeCode::NotEnoughGold: return with("UIIT_MSG_INTERACTION_FAIL_NOT_ENOUGH_MONEY", false); // ours
    case proto::TradeCode::CartFull: return with("UIIT_MSG_STRGERR_INVENTORY_FULL", false);                // ours
    default: return nullptr;
    }
}

void TradeBuyStep(HudContext& c) {
    if (c.tradeBuyLeft <= 0 || c.hud == nullptr) return;
    const int step = std::min(c.tradeBuyLeft, 40); // a stack at a time
    c.tradeBuyLeft -= step;
    HudRequest r{HudRequest::Kind::ShopBuy, c.tradeBuyTab | c.tradeBuySlot << 8};
    r.extra = step;
    c.hud->Request(r);
}

void TradeAnswered(HudContext& c, const proto::TradeAnswer& a) {
    if (a.code != proto::TradeCode::Ok) {
        c.tradeBuyLeft = 0;
        bool banner = false;
        if (const char* key = TradeCodeKey(std::uint16_t(a.code), &banner)) Line(c, c.Text(key), banner);
        return;
    }
    if (a.request == proto::TradeRequest::Buy) TradeBuyStep(c); // the next step, if any
}

void TradeNotice(HudContext& c, const proto::TradeBargainNotice& n) {
    // 0x300C {0x0C08 / 0x0C09 / 0x0C0A}: the warning banner and a system line.
    const char* key = n.moment == 1 ? "UIIT_MSG_SPECIALTY_ANNOUNCE_SELL_READY" : n.moment == 2 ? "UIIT_MSG_SPECIALTY_ANNOUNCE_SELL_START" : "UIIT_MSG_SPECIALTY_ANNOUNCE_SELL_END";
    Line(c, c.Text(key), true);
}

void OpenTradeDeal(HudContext& c, const TradeDeal& deal) {
    if (c.hud == nullptr) return;
    if (auto* w = dynamic_cast<DealWindow*>(c.hud->Show(kDealWindow))) w->Open(deal);
}

bool BuyIntoCart(HudContext& c, const assets::ShopGood& good, int tab) {
    const assets::ItemRow* row = c.items != nullptr ? c.items->Find(std::string_view(good.itemCodename)) : nullptr;
    if (row == nullptr || !game::IsSpecialtyGood(*row)) return false;
    TradeDeal deal;
    deal.buy = true;
    deal.name = c.Text(row->nameKey);
    deal.unitPrice = c.TaxedBuy(good.price); // (package + tax) x count
    const std::uint64_t gold = c.state != nullptr ? c.state->inventory.gold : 0;
    deal.most = deal.unitPrice > 0 ? int(std::min<std::uint64_t>(gold / deal.unitPrice, 10'000)) : 0;
    deal.count = std::min(40, deal.most);
    deal.tab = tab;
    deal.slot = good.slot;
    OpenTradeDeal(c, deal);
    return true;
}

bool SellFromCart(HudContext& c, const game::ItemInstance& item, int place) {
    const assets::ItemRow* row = c.items != nullptr ? c.items->Find(item.refId) : nullptr;
    if (row == nullptr || !game::IsSpecialtyGood(*row)) return false;
    TradeDeal deal;
    deal.buy = false;
    deal.name = c.Text(row->nameKey);
    deal.unitPrice = SellPriceOf(c, *row);
    deal.count = deal.most = std::min<int>(item.count, 40);
    deal.place = place;
    OpenTradeDeal(c, deal);
    return true;
}

void OpenTradeBoard(HudContext& c) {
    if (c.hud != nullptr) c.hud->Show(kBoardWindow);
}

bool RefreshTradeBoard(HudContext& c, int centre) {
    if (c.hud == nullptr || !c.hud->Shown(kBoardWindow)) return false;
    auto* w = dynamic_cast<BoardWindow*>(c.hud->Show(kBoardWindow));
    if (w == nullptr || centre < 0 || std::size_t(centre) >= c.tradeCentres.size() || !c.tradeRequest) return false;
    w->Pick(centre);
    c.tradeRequest(proto::TradeBoardQuery::kId, proto::Encode(proto::TradeBoardQuery{c.tradeCentres[std::size_t(centre)]}));
    return true;
}

bool ConfirmTradeDeal(HudContext& c, int count) {
    if (c.hud == nullptr || !c.hud->Shown(kDealWindow)) return false;
    auto* w = dynamic_cast<DealWindow*>(c.hud->Show(kDealWindow));
    if (w == nullptr) return false;
    if (count >= 0) Put(w->Child(30), std::to_string(count));
    w->Confirm();
    return true;
}

void RegisterTradeWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CIFSpecialtyDeal", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<DealWindow>(n, *c); });
    ui.Register("CIFTradeInfoWnd", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<BoardWindow>(n, *c); });
    // A trade centre's row: a bar with its name (ours: the class's own drawing not traced).
    ui.Register("CIFTradeInfoWndStoreSlot", [](const assets::UiLayoutNode& n) -> std::unique_ptr<ui::Widget> { return std::make_unique<ui::Bar>(n); });
}

} // namespace sr::client
