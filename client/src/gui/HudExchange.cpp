// The exchange window (CIFExchange, ginterface.txt 26; resinfo\ifexchange.txt with the 1.188 client's
// UI_UPDATE_2009_FIRST and APPLY_EXCHANGE_UPDATE_1TH): the other player's 12
// places above (100..111) and the player's below (200..211), each side's gold (18, 19) with the money
// button (15) for the player's, the message box (304), Confirm (11) and Cancel (12).
//
// An item of the bag dropped on the window is offered (the original's 0x7034 op 4); a press on one of
// the player's own places takes it back out (ours: the original drags it back). Confirm fixes the
// player's offer; once both confirmed it approves (with its two messages). The window
// is shown and hidden by the game screen as the exchange opens and ends.

#include <string>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/game/Inventory.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr int kConfirm = 11, kCancel = 12, kMoneyButton = 15, kTheirGold = 18, kMyGold = 19, kTextBox = 304;
constexpr int kTheirFirst = 100, kMyFirst = 200;
constexpr int kTheirName = 400, kMyName = 401; // ours: the names on the two halves' strips
constexpr int kFigureEffect = 155, kFigureTooltip = 156, kPulse = 402;

// The name warning's red frame over the other's figure (155: ch_red.ddj, a 4 x 4 sheet of 128 x 128 frames,
// plays it) with the warning as the tooltip of the figure (156). Ours: the
// sheet's frames looped at 12 a second (the original's pace is not traced).
class WarningPulse : public ui::Widget {
public:
    WarningPulse(const assets::UiLayoutNode& node, ui::Rect tip) : Widget(node), tip_(tip) {}
    std::string text;

    void Update(ui::Frame& frame) override {
        const ui::Rect r = ScreenRect();
        const ui::Rect area{r.x + tip_.x - rect.x, r.y + tip_.y - rect.y, tip_.w, tip_.h};
        const ui::Vec2 p = frame.pointer.position;
        if (!text.empty() && p.x >= area.x && p.y >= area.y && p.x < area.Right() && p.y < area.Bottom()) frame.interface.ShowTooltip(text, area);
    }
    void Draw(ui::Frame& frame) override {
        const int f = int(frame.time * 12.0) % 12;
        const ui::Vec2 uv0{float(f % 4) * 0.25f, float(f / 4) * 0.25f};
        frame.paint.Image("interface\\exchange\\ch_red.ddj", ScreenRect(), uv0, {uv0.x + 0.25f, uv0.y + 0.25f}, ui::WithAlpha(ui::kWhite, Opacity()));
    }

private:
    ui::Rect tip_;
};

class ExchangeWindow : public ui::MainFrame {
public:
    ExchangeWindow(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        ui_ = &ui;
        for (int k = 0; k < int(proto::kExchangeSlots); ++k) {
            for (const bool mine : {false, true}) {
                auto* slot = ui::As<ui::Slot>(Child((mine ? kMyFirst : kTheirFirst) + k));
                if (slot == nullptr) continue;
                slot->pickable = false; // a press on the player's own takes it back (OnNotice)
                slot->tooltipLines = [this, k, mine] {
                    const game::ExchangeView* v = context_.exchange;
                    const auto& item = v != nullptr ? (mine ? v->Mine() : v->Theirs()).items[std::size_t(k)] : std::nullopt;
                    const assets::ItemRow* row = item && context_.items ? context_.items->Find(item->refId) : nullptr;
                    return row ? ItemTooltip(*row, *item, context_) : std::vector<ui::TooltipLine>{};
                };
            }
        }
        for (const int id : {kTheirGold, kMyGold}) {
            if (auto* gold = ui::As<ui::Static>(Child(id))) gold->textStyle |= ui::kTextShadow;
        }
        // The name warning's pieces stay hidden (Create hides 153, the other's name over its places on
        // icon_effect1.ddj); 155 and 156, the red frame over the other's figure and its tooltip,
        // are the warning's (WarningPulse in their place, shown by Warn).
        ui::Rect effect{}, tip{};
        for (const int id : {153, kFigureEffect, kFigureTooltip}) {
            ui::Widget* w = Child(id);
            if (w == nullptr) continue;
            w->visible = false;
            if (id == kFigureEffect) effect = w->rect;
            if (id == kFigureTooltip) tip = w->rect;
        }
        if (effect.w <= 0.0f) effect = {247.0f, 70.0f, 128.0f, 128.0f}; // the layout's
        if (tip.w <= 0.0f) tip = {247.0f, 70.0f, 100.0f, 104.0f};
        assets::UiLayoutNode pulse;
        pulse.type = "CIFStatic";
        pulse.id = kPulse;
        pulse.rect = {effect.x, effect.y, effect.w, effect.h};
        pulse_ = static_cast<WarningPulse*>(Add(std::make_unique<WarningPulse>(pulse, tip)));
        pulse_->visible = false;
        // Whose half is whose: each half's title on its strip (the 1.188 client sets the sub-frames 13 and 14; ours
        // draws no text on a sub-frame, so a label lies over each strip).
        std::vector<std::pair<int, ui::Rect>> strips;
        for (const auto& c : children()) {
            if (c->node().name == "GDR_EXCHANGE_OTHER_SUBFRAME") strips.push_back({kTheirName, c->rect});
            if (c->node().name == "GDR_EXCHANGE_MY_SUBFRAME") strips.push_back({kMyName, c->rect});
        }
        for (const auto& [id, r] : strips) {
            assets::UiLayoutNode label;
            label.type = "CIFStatic";
            label.id = id;
            label.rect = {r.x + 4.0f, r.y + 3.0f, r.w - 8.0f, 16.0f};
            label.hAlign = 1;
            label.vAlign = 1;
            if (auto* made = ui::As<ui::Static>(Add(ui.Make(label)))) made->textStyle |= ui::kTextShadow;
        }
        // Dropped on the window: an item of the bag offered.
        onDrop = [this](ui::Widget* source, ui::Widget*) {
            const int place = InventoryPlaceOf(source);
            if (place < game::slot::kWorn || !context_.exchangeRequest) return false;
            context_.exchangeRequest(proto::ExchangePut::kId, proto::Encode(proto::ExchangePut{std::uint8_t(place)}));
            return true;
        };
    }

    void Update(ui::Frame& frame) override {
        ui::MainFrame::Update(frame);
        const game::ExchangeView* v = context_.exchange;
        if (v == nullptr || v->Revision() == seen_) return;
        seen_ = v->Revision();
        // A new exchange: its message box starts again with whom it is (UIIT_MSG_EXCHANGE_START).
        if (v->Open() && v->Peer() != peer_) {
            peer_ = v->Peer();
            if (auto* box = ui::As<ui::TextBox>(Child(kTextBox))) box->ClearParagraphs();
            Note(FormatText(context_.Text("UIIT_MSG_EXCHANGE_START"), {v->PeerName()}));
            Warn(*v);
        }
        if (!v->Open()) {
            peer_ = 0;
            if (pulse_ != nullptr) pulse_->visible = false;
        }
        // The halves' titles: "[name - guild]'s item", or "[name]'s goods" without a guild (ours:
        // the original's choice between the two goes by a value of the player's not traced).
        const auto title = [&](const std::string& name, const std::string& guild) {
            if (name.empty()) return std::string();
            return guild.empty() ? FormatText(context_.Text("UIIT_STT_SOMEONESITEM"), {name})
                                 : FormatText(context_.Text("UIIT_STT_SOMEONESITEM_GUILD"), {name, guild});
        };
        const proto::EntityGuild* theirGuild = context_.guild != nullptr ? context_.guild->EntityTag(v->Peer()) : nullptr;
        if (ui::Widget* w = Child(kTheirName)) w->SetText(title(v->PeerName(), theirGuild != nullptr ? theirGuild->name : std::string()));
        if (ui::Widget* w = Child(kMyName); w != nullptr && context_.state != nullptr) {
            const bool guild = context_.guild != nullptr && context_.guild->InGuild();
            w->SetText(title(context_.state->self.name, guild ? context_.guild->Guild().name : std::string()));
        }
        for (const bool mine : {false, true}) {
            const game::ExchangeView::Side& side = mine ? v->Mine() : v->Theirs();
            for (int k = 0; k < int(proto::kExchangeSlots); ++k) {
                auto* slot = ui::As<ui::Slot>(Child((mine ? kMyFirst : kTheirFirst) + k));
                if (slot == nullptr) continue;
                const auto& item = side.items[std::size_t(k)];
                ShowItem(*slot, item ? &*item : nullptr, context_);
                // A side that confirmed: no more changes to its offer, the blue com_item_exchange.ddj over its
                // places from its first Confirm until the exchange ends (set on all 12 by
                // for the player's own and for the other's, cleared by on opening).
                slot->overlay = side.step != proto::ExchangeStep::Open ? "interface\\ifcommon\\com_item_exchange.ddj" : "";
            }
            if (auto* gold = ui::As<ui::Static>(Child(mine ? kMyGold : kTheirGold))) {
                gold->SetText(GoldText(side.gold));
                gold->textColor = GoldColor(side.gold);
            }
        }
        // Confirm (0xB083's): "Confirm" while the player's offer is open; then
        // "Approve", enabled once the other confirmed too; disabled once the player approved.
        if (ui::Widget* confirm = Child(kConfirm)) {
            const auto mine = v->Mine().step, theirs = v->Theirs().step;
            confirm->SetText(context_.Text(mine == proto::ExchangeStep::Open ? "UIIT_CTL_CONFIRM" : "UIIT_CTL_ACKNOWLEDGMENT"));
            confirm->enabled = mine == proto::ExchangeStep::Open || (mine == proto::ExchangeStep::Confirmed && theirs != proto::ExchangeStep::Open);
        }
        if (ui::Widget* money = Child(kMoneyButton)) money->enabled = v->Mine().step == proto::ExchangeStep::Open;
    }

    bool OnNotice(const ui::Notice& notice) override {
        const bool mine = notice.source != nullptr && notice.source->parent() == this;
        if (!mine || notice.kind != ui::Notice::Kind::Clicked) return ui::MainFrame::OnNotice(notice);
        const auto request = [&](proto::MessageId id, std::vector<std::byte> payload) {
            if (context_.exchangeRequest) context_.exchangeRequest(id, std::move(payload));
        };
        if (notice.control == kConfirm) {
            if (context_.exchangeConfirm) context_.exchangeConfirm();
            return true;
        }
        if (notice.control == kCancel || notice.control == 2) { // Cancel, the close button
            request(proto::ExchangeCancel::kId, proto::Encode(proto::ExchangeCancel{}));
            return true;
        }
        if (notice.control == kMoneyButton) {
            if (context_.hud != nullptr) context_.hud->ExchangeGoldBox();
            return true;
        }
        if (notice.control >= kMyFirst && notice.control < kMyFirst + int(proto::kExchangeSlots) && context_.exchange != nullptr) {
            const int k = notice.control - kMyFirst;
            if (context_.exchange->Mine().items[std::size_t(k)] && context_.exchange->Mine().step == proto::ExchangeStep::Open) {
                request(proto::ExchangeTake::kId, proto::Encode(proto::ExchangeTake{std::uint8_t(k)}));
            }
            return true;
        }
        return ui::MainFrame::OnNotice(notice);
    }

    // A line in the window's message box.
    void Note(const std::string& text, ui::Color color = 0xFFFFFFFF) {
        if (auto* box = ui::As<ui::TextBox>(Child(kTextBox))) {
            box->AddParagraph(text, color);
            box->ScrollToEnd();
        }
    }

    // The anti-fraud warning (APPLY_EXCHANGE_UPDATE_1TH): the other player
    // known when it is on the friend list, in the player's guild or in its union; otherwise the red line
    // "Opponent user is not listed in your Guild/Union/Friend" (in the box and the system messages) and the
    // red frame pulsing over its figure, the line its tooltip. (The original's other line, "Opponet user's
    // name is a fictitious name.", is for an object that is no player: never here.)
    void Warn(const game::ExchangeView& v) {
        bool known = false;
        if (context_.community != nullptr) {
            for (const proto::FriendRecord& f : context_.community->Friends()) known = known || f.name == v.PeerName();
        }
        const proto::EntityGuild* tag = context_.guild != nullptr ? context_.guild->EntityTag(v.Peer()) : nullptr;
        if (tag != nullptr && context_.guild->InGuild()) {
            known = known || tag->guild == context_.guild->Guild().id;
            known = known || (context_.guild->Union().id != 0 && tag->unionId == context_.guild->Union().id);
        }
        if (pulse_ != nullptr) pulse_->visible = !known;
        if (known) return;
        const std::string line = context_.Text("UIIT_MSG_EXCHANGE_DECEPTION_COMPARE");
        Note(line, ui::Argb(0xFFFF4A4A)); // the original's red
        if (pulse_ != nullptr) pulse_->text = line;
        if (context_.state != nullptr) context_.state->Post(game::GameMessage::Kind::General, line, context_.time);
    }

private:
    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    std::uint64_t seen_ = 0;
    std::uint32_t peer_ = 0;
    WarningPulse* pulse_ = nullptr;
};

} // namespace

std::unique_ptr<ui::Widget> MakeExchangeWindow(const assets::UiLayoutNode& node, HudContext& context) {
    return std::make_unique<ExchangeWindow>(node, context);
}

void ExchangeNote(ui::Interface& ui, const std::string& text) {
    if (auto* w = dynamic_cast<ExchangeWindow*>(ui.Window(hud::kExchange))) w->Note(text);
}

// The client's text for what went wrong (for the codes it words; ours for those it
// leaves silent, marked).
const char* ExchangeCodeKey(proto::ExchangeCode code) {
    using C = proto::ExchangeCode;
    switch (code) {
    case C::InvalidTarget: return "UIIT_MSG_STRGERR_INVALID_TARGET";
    case C::TooFar: return "UIIT_STT_ERR_COMMON_TOO_FAR"; // ours
    case C::NotEnoughGold: return "UIIT_MSG_STRGERR_NOT_ENOUGH_GOLD";
    case C::WornItem: return "UIIT_MSG_STRGERR_CANNOT_BE_TRADED"; // ours
    case C::Failed:
    case C::Abuse: return "UIIT_MSG_STRGERR_TRADE_FAILURE"; // ours
    case C::WrongStep:
    case C::SelfBusy:
    case C::AskerStalls:
    case C::Locked:
    case C::PeerGone: return "UIIT_MSG_STRGERR_CANNOT_DEAL_NORMALCY"; // ours
    case C::Busy: return "UIIT_MSG_STRGERR_BUSY";
    case C::BayFull: return "UIIT_MSG_STRGERR_TRADE_BAY_FULL";
    case C::BagFull: return "UIIT_MSG_STRGERR_INVENTORY_FULL_DEAL_CANCEL";
    case C::NothingOffered: return "UIIT_MSG_STRGERR_DEAL_FAIL_NON_ITEM";
    case C::Timeout: return "UIIT_MSG_STRGERR_TIMEOUT";
    case C::Refused: return "UIIT_MSG_STRGERR_TRADING_DENIED";
    case C::InvalidNumber: return "UIIT_MSG_STRGERR_SPECIFY_POSITIVE_NUMBER_ONLY";
    case C::Cancelled: return "UIIT_MSG_STRGERR_EXCHANGE_CANCEL";
    case C::CancelledByUser: return "UIIT_MSG_STRGERR_TRADING_CANCELED_BY_USER";
    case C::PeerBagFull: return "UIIT_MSG_STRGERR_NOT_ENOUGH_OPPONENT_INV_SPACE";
    case C::CannotTrade: return "UIIT_MSG_STRGERR_CANNOT_BE_TRADED";
    case C::PeerDead: return "UIIT_MSG_STRGERR_CANNOT_FIND_TARGET"; // ours
    case C::TradeGoods: return "UIIT_MSG_STRGERR_YOU_CANT_DEAL_SPECIALTY";
    case C::PetSummoned: return "UIIT_MSG_COSPETERR_CANT_TRADE";
    case C::Rented: return "UIIT_CTL_NASRUN_NOT_TRADE";
    default: return nullptr;
    }
}

} // namespace sr::client
