// The stall's windows:
//
// - The stall (CIFStall, ginterface.txt 33, resinfo\ifstall.txt; makes it, switches its
//   state): the name (10) with its change button (5, the owner's), the greeting (11) with its button (6), the
//   state's icon and word (15, 14: stl_condition_icon_01 "Operating" open for business, _02 "Modifying"), the
//   owner's Open / Modify button (4), the 10 items (ifstallslot.txt: icon, name, count, price, the edit button;
//   208 x 44 each on stl_slot_02.ddj, two to a row in the display, 12) and the chat module (3: the lines,
//   the input, the people). An item of the bag dropped on a slot of the player's own stall, while it is being
//   modified, opens the register box (the store's deal box in its stall mode); a press on an item's edit button
//   asks its new price (the same box); the owner's item dragged to the bag is taken back. A visitor drags an item
//   to the bag to buy it: the agreement box (its icon, name, count and price; Buy and Cancel).
// - The stall network (CNIFStallNetwork, ginterface.txt 150 from res_ui\stallnetwork.2dt, opens
//   it): the class, type and degree (fmncategorytreedata.txt), Search, 15 rows a page (number, level, icon,
//   name, count, price), the pages, the seller of the chosen row, the gold, Whisper and Purchase (asks first,
//   UIIT_MSG_WARENETWORK_BUY_CONFIRM). Ours: a new search at most every 3 s (the server's rule; the original's
//   button waited 10 s), pages free, every town's stalls (a row's tooltip says its town).

#include <algorithm>
#include <map>
#include <string>

#include "gui/ErrorTexts.h"
#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/game/Inventory.h"

namespace sr::client {

namespace proto = game::proto;
using Code = proto::StallCode;

namespace {

// --- The stall -------------------------------------------------------------------------------------------

constexpr int kChat = 3, kTradingState = 4, kChangeTitle = 5, kOwnerMsg = 6, kTitle = 10, kGreeting = 11, kDisplay = 12, kStateMsg = 14,
              kStateIcon = 15;
// Ours: each slot's parts as children of the window (the original nests them in a slot window, ids 200..209).
constexpr int kSlotBack = 200, kSlotIcon = 300, kSlotName = 320, kSlotCount = 340, kSlotPrice = 360, kSlotEdit = 380, kSlotVeil = 400;
// A slot's art: stl_slot_05 empty, stl_slot_02 holding an item.
constexpr const char* kSlotEmpty = "interface\\stall\\stl_slot_05.ddj";
constexpr const char* kSlotFull = "interface\\stall\\stl_slot_02.ddj";
// The chat module's parts (ifchatmodule.txt).
constexpr int kChatInput = 1, kChatLines = 2, kChatLinesScroll = 3, kChatPeople = 4, kChatPeopleScroll = 5;

ui::Widget* ChatPart(ui::Widget* window, int id) {
    ui::Widget* module = window != nullptr ? window->Child(kChat) : nullptr;
    return module != nullptr ? module->Child(id) : nullptr;
}

class StallWindow : public ui::MainFrame {
public:
    StallWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
        // The 10 slots (208 x 44, handed to the display's scroll manager, which steps them 206 across
        // in two columns and 41 down, each covers the black band along the bottom of the one above; the five
        // rows fit its 216, so ours does without the scroll).
        const ui::Widget* display = Child(kDisplay);
        const ui::Rect area = display != nullptr ? display->rect : ui::Rect{22.0f, 108.0f, 423.0f, 216.0f};
        const assets::UiLayout* slotLayout = ui.Layout("resinfo\\ifstallslot.txt");
        for (int i = 0; i < int(proto::kStallSlots); ++i) {
            const float x = area.x + 4.0f + float(i % 2) * 206.0f, y = area.y + 4.0f + float(i / 2) * 41.0f;
            assets::UiLayoutNode back;
            back.type = "CIFStatic";
            back.id = kSlotBack + i;
            back.ddj = kSlotEmpty;
            back.rect = {x, y, 208.0f, 44.0f};
            Add(ui.Make(back));
            if (slotLayout == nullptr) continue;
            for (const auto& s : slotLayout->sections) {
                if (s.name != "Create") continue;
                for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) {
                    assets::UiLayoutNode n = *it;
                    static constexpr int kBase[] = {0, kSlotIcon, kSlotName, kSlotCount, kSlotPrice, kSlotEdit};
                    if (n.id < 1 || n.id > 5) continue;
                    n.id = kBase[n.id] + i;
                    n.rect.x += x;
                    n.rect.y += y;
                    if (auto* made = Add(ui.Make(n)); made != nullptr) {
                        if (auto* slot = ui::As<ui::Slot>(made)) {
                            slot->pickable = false; // an item's (Update)
                            slot->tooltipLines = [this, i] {
                                const game::StallView* v = c_.stall;
                                const auto& item = v != nullptr ? v->Items()[std::size_t(i)] : std::nullopt;
                                const assets::ItemRow* row = item && c_.items ? c_.items->Find(item->item.item) : nullptr;
                                return row ? ItemTooltip(*row, game::Inventory::FromRecord(item->item), c_) : std::vector<ui::TooltipLine>{};
                            };
                        }
                        if (auto* text = ui::As<ui::Static>(made)) text->textStyle |= ui::kTextShadow;
                        if (n.id == kSlotEdit + i) made->SetText({});
                    }
                }
            }
            // A visitor's view while the stall is being modified: the item's box under the blue veil the exchange's
            // offers have (the user's memory of the original; sets the slots' state for a visitor
            // then: its look not traced further).
            assets::UiLayoutNode veil;
            veil.type = "CIFStatic";
            veil.id = kSlotVeil + i;
            veil.ddj = "interface\\ifcommon\\com_item_exchange.ddj";
            veil.rect = {x, y, 208.0f, 44.0f};
            if (ui::Widget* v = Add(ui.Make(veil))) v->visible = false;
        }
        // The chat module's sample text goes.
        for (const int id : {kChatLines, kChatPeople}) {
            if (auto* box = ui::As<ui::TextBox>(ChatPart(this, id))) {
                box->ClearParagraphs();
                // Each with its own vertical scroll (the module's GDR_CHAT_VSCROLL 3 for the lines, GDR_CHAT_USERVSCROLL
                // 5 for the people): without it the box only ever showed its last rows.
                box->bar = ui::As<ui::ScrollBar>(ChatPart(this, id == kChatLines ? kChatLinesScroll : kChatPeopleScroll));
            }
        }
        if (auto* input = ui::As<ui::Edit>(ChatPart(this, kChatInput))) input->limit = 120;
        // Dropped on a slot (or the window): an item of the bag put up there, its price asked (the register box).
        onDrop = [this](ui::Widget* source, ui::Widget* target) {
            const int place = InventoryPlaceOf(source);
            if (place < game::slot::kWorn || c_.stall == nullptr || !c_.stall->Mine() || c_.stall->ForBusiness()) return false;
            int slot = -1;
            for (int i = 0; i < int(proto::kStallSlots) && target != nullptr; ++i) {
                if (target == Child(kSlotIcon + i) || target == Child(kSlotBack + i)) slot = i;
            }
            if (slot < 0 || c_.stall->Items()[std::size_t(slot)]) {
                for (int i = 0; i < int(proto::kStallSlots) && slot < 0; ++i) {
                    if (!c_.stall->Items()[std::size_t(i)]) slot = i; // the first free one
                }
            }
            if (slot < 0) return false;
            StallRegister(c_, slot, place);
            return true;
        };
    }

    void Update(ui::Frame& frame) override {
        MainFrame::Update(frame);
        const game::StallView* v = c_.stall;
        if (v == nullptr) return;
        if (!v->In()) {
            if (ui_ != nullptr && opened_) ui_->Close(this);
            return;
        }
        opened_ = true;
        // The owner's buttons' help (the original's control help on 5 and 6, and each slot's price button).
        if (const ui::Widget* over = frame.pointer.hovered; over != nullptr && over->visible) {
            const char* key = over == Child(kChangeTitle) ? "UIIT_STT_TOGGLE_STORE_TITLE_CHANGE"
                              : over == Child(kOwnerMsg)  ? "UIIT_STT_TOGGLE_STORE_MASTER_MESSAGE"
                                                          : nullptr;
            for (int i = 0; key == nullptr && i < int(proto::kStallSlots); ++i) {
                if (over == Child(kSlotEdit + i)) key = "UIIT_STT_TOGGLE_STORE_PRICE_CHANGE";
            }
            if (key != nullptr) frame.interface.ShowTooltip(c_.Text(key), over->ScreenRect());
        }
        if (v->Revision() == seen_) return;
        seen_ = v->Revision();
        const bool mine = v->Mine(), open = v->ForBusiness();
        if (ui::Widget* w = Child(kTitle)) w->SetText(v->Name());
        // No greeting of the owner's: the original's own, "Welcome to [name]'s stall." (UIIT_STT_STALL_DEFAULT_OWNERMSG).
        if (ui::Widget* w = Child(kGreeting)) {
            w->SetText(!v->Greeting().empty() ? v->Greeting() : FormatText(c_.Text("UIIT_STT_STALL_DEFAULT_OWNERMSG"), {v->OwnerName()}));
        }
        // The state: open for business, or being modified; the owner's button the other way.
        if (auto* icon = ui::As<ui::Static>(Child(kStateIcon))) {
            icon->texture = open ? "interface\\stall\\stl_condition_icon_01.ddj" : "interface\\stall\\stl_condition_icon_02.ddj";
        }
        if (ui::Widget* w = Child(kStateMsg)) w->SetText(c_.Text(open ? "UIIT_STT_TRADING_NOW" : "UIIT_STT_STALL_MODIFYING"));
        if (ui::Widget* w = Child(kTradingState)) {
            w->SetText(c_.Text(open ? "UIIT_STT_END_STALL" : "UIIT_STT_START_STALL"));
            w->visible = mine;
        }
        if (ui::Widget* w = Child(kChangeTitle)) w->enabled = mine && !open;
        if (ui::Widget* w = Child(kOwnerMsg)) w->enabled = mine;
        for (int i = 0; i < int(proto::kStallSlots); ++i) {
            const auto& item = v->Items()[std::size_t(i)];
            const game::ItemInstance instance = item ? game::Inventory::FromRecord(item->item) : game::ItemInstance{};
            const assets::ItemRow* row = item && c_.items ? c_.items->Find(item->item.item) : nullptr;
            if (auto* slot = ui::As<ui::Slot>(Child(kSlotIcon + i))) {
                ShowItem(*slot, item ? &instance : nullptr, c_);
                slot->pickable = item.has_value(); // dragged to the bag: bought, or (the owner's) taken back
            }
            std::string name;
            if (row != nullptr) name = c_.Text(row->nameKey) + (instance.plus > 0 ? " (+" + std::to_string(instance.plus) + ")" : "");
            if (ui::Widget* w = Child(kSlotName + i)) w->SetText(name);
            // "%d %s" with UIIT_STT_UNIT, "%s %s" with UIIT_STT_GOLD; the price red where the
            // visitor's gold falls short (0xFFFF4A4A; ours: what asks is not traced).
            if (ui::Widget* w = Child(kSlotCount + i)) {
                w->SetText(item ? std::to_string(std::max<int>(1, item->item.quantity)) + " " + c_.Text("UIIT_STT_UNIT") : std::string());
            }
            if (auto* w = ui::As<ui::Static>(Child(kSlotPrice + i))) {
                w->SetText(item ? GoldText(item->price) + " " + c_.Text("UIIT_STT_GOLD") : std::string());
                const bool short_ = item && !mine && c_.state != nullptr && c_.state->inventory.gold < item->price;
                w->textColor = short_ ? ui::Argb(0xFFFF4A4A) : ui::kWhite;
            }
            if (auto* back = ui::As<ui::Static>(Child(kSlotBack + i))) back->texture = item ? kSlotFull : kSlotEmpty;
            if (ui::Widget* veil = Child(kSlotVeil + i)) veil->visible = !mine && !open;
            // The edit button: the owner's, on an item while modifying (with state 2).
            if (ui::Widget* w = Child(kSlotEdit + i)) w->visible = mine && !open && item.has_value();
        }
        // The chat (the module's lines) and the people (the owner first).
        if (auto* box = ui::As<ui::TextBox>(ChatPart(this, kChatLines)); box != nullptr && v->Chat().size() != chatShown_) {
            for (std::size_t k = chatShown_ < v->Chat().size() ? chatShown_ : 0; k < v->Chat().size(); ++k) {
                if (k == 0 && chatShown_ > v->Chat().size()) box->ClearParagraphs();
                const game::StallView::ChatLine& l = v->Chat()[k];
                if (l.name.empty()) box->AddParagraph(l.text, ui::Argb(l.colour != 0 ? l.colour : 0xFFFFFFFFu));
                else box->AddParagraph(l.name + ": " + l.text, ui::kWhite);
            }
            chatShown_ = v->Chat().size();
            box->ScrollToEnd();
        }
        if (auto* people = ui::As<ui::TextBox>(ChatPart(this, kChatPeople))) {
            people->ClearParagraphs();
            people->AddParagraph(v->OwnerName(), ui::Argb(0xFFFEB5FF)); // the booth's colour
            for (const proto::StallPerson& p : v->People()) people->AddParagraph(p.name, ui::kWhite);
        }
    }

    bool OnNotice(const ui::Notice& n) override {
        const game::StallView* v = c_.stall;
        if (v == nullptr || !v->In()) return MainFrame::OnNotice(n);
        // A line typed in the chat module's input.
        if (n.kind == ui::Notice::Kind::Submitted && n.source == ChatPart(this, kChatInput)) {
            auto* input = ui::As<ui::Edit>(ChatPart(this, kChatInput));
            if (input != nullptr && !input->text().empty()) {
                Send(proto::StallChat{input->text()});
                input->SetValue({});
            }
            return true;
        }
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr || n.source->parent() != this) return MainFrame::OnNotice(n);
        const bool mine = v->Mine();
        if (n.control == 2) { // the close button: the owner closes its stall, a visitor leaves (0x70B2 / 0x70B5)
            if (mine) {
                Send(proto::StallClose{});
            } else {
                Send(proto::StallLeave{});
            }
            return true;
        }
        if (n.control == kTradingState && mine) {
            if (v->ForBusiness()) {
                Send(proto::StallSetOpen{0, 0});
                return true;
            }
            // Opening for business: the items registered in the network too? (UIIT_MSG_WARENETWORK_REGIST_01..03)
            if (c_.hud != nullptr) {
                HudContext* c = &c_;
                c_.hud->Confirm("UIIT_STT_CONFIRM_BOX",
                                {c_.Text("UIIT_MSG_WARENETWORK_REGIST_01"), c_.Text("UIIT_MSG_WARENETWORK_REGIST_02"), c_.Text("UIIT_MSG_WARENETWORK_REGIST_03")},
                                [c](bool yes) {
                                    if (c->stallRequest) c->stallRequest(proto::StallSetOpen::kId, proto::Encode(proto::StallSetOpen{1, std::uint8_t(yes)}));
                                });
            }
            return true;
        }
        if (n.control == kChangeTitle && mine && !v->ForBusiness()) {
            HudContext* c = &c_;
            AskGuildText(c_, c_.Text("UIIT_STT_INSERT_STALL_NAME"), proto::kStallNameLength, [c](std::string name) {
                if (c->stallRequest) c->stallRequest(proto::StallSetText::kId, proto::Encode(proto::StallSetText{0, std::move(name)}));
            }, v->Name());
            return true;
        }
        if (n.control == kOwnerMsg && mine) {
            HudContext* c = &c_;
            // The greeting's 64 (the original's bytes; ours characters, the server checks the bytes).
            AskGuildText(c_, c_.Text("UIIT_STT_STALL"), proto::kStallGreetingMost, [c](std::string text) {
                if (c->stallRequest) c->stallRequest(proto::StallSetText::kId, proto::Encode(proto::StallSetText{1, std::move(text)}));
            }, v->Greeting());
            return true;
        }
        for (int i = 0; i < int(proto::kStallSlots); ++i) {
            const auto& item = v->Items()[std::size_t(i)];
            // The edit button: the item's new price asked (the register box), while the stall is being modified.
            if (n.control == kSlotEdit + i && mine && item && !v->ForBusiness()) {
                StallRegister(c_, i, 0);
                return true;
            }
        }
        return MainFrame::OnNotice(n);
    }

private:
    template <typename T>
    void Send(const T& m) {
        if (c_.stallRequest) c_.stallRequest(T::kId, proto::Encode(m));
    }
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::uint64_t seen_ = ~std::uint64_t(0);
    std::size_t chatShown_ = 0;
    bool opened_ = false;
};

// --- The stall network -------------------------------------------------------------------------------------

constexpr const char* kNetworkLayout = "res_ui\\stallnetwork.2dt";
constexpr int kSearch = 4, kSellerName = 14, kPages = 16, kWhisper = 17, kPurchase = 18, kGold = 22, kClassCombo = 6, kTypeCombo = 7,
              kDegreeCombo = 8;
constexpr int kGradeCombo = 9001, kSexCombo = 9002, kNameEdit = 9003, kNameHint = 9004; // ours: the detailed search
// Inside a row: the number, the level, the icon, the name, the count, the price.
constexpr int kRowNumber = 122, kRowLevel = 123, kRowIcon = 124, kRowName = 125, kRowCount = 126, kRowPrice = 127;

// A row of the results: pressed to choose it (the chosen one lit).
class NetworkRow : public ui::Static {
public:
    using Static::Static;
    bool chosen = false;
    void Created(ui::Interface& ui) override {
        Static::Created(ui);
        interactive = true;
    }
    void Draw(ui::Frame& frame) override {
        if (chosen) frame.paint.Fill(ScreenRect(), ui::Argb(0x5032A0FF));
        Static::Draw(frame);
    }
    void Mouse(ui::Frame& frame) override {
        if (frame.input.Pressed(ui::MouseButton::Left) && frame.pointer.hovered == this) Notify(ui::Notice::Kind::Clicked);
    }
};

// The page numbers as 1.188's CNIFPageManager (a .2dt's kind 14): "[" four numbers (20 wide, 14 high) "]", the page shown
// in orange (0xFFFF9951), the others white; mall_page_prev (36 x 12) 13 left of "[" while a group of four comes before,
// mall_page_next 17 right of "]" while one comes after (Prev: the last page of the group before; Next: the first after).
class NetworkPages : public ui::Static {
public:
    using Static::Static;
    int page = 0, pages = 0;
    void Draw(ui::Frame& frame) override {
        const Layout l = Lay();
        frame.paint.Text("[", {l.open, l.y, 8.0f, 14.0f}, ui::kWhite, 0, 1, 1);
        frame.paint.Text("]", {l.close, l.y, 8.0f, 14.0f}, ui::kWhite, 0, 1, 1);
        const int first = page / 4 * 4;
        for (int k = 0; k < 4 && first + k < pages; ++k) {
            frame.paint.Text(std::to_string(first + k + 1), {l.numbers + float(k) * 20.0f, l.y, 20.0f, 14.0f},
                             first + k == page ? ui::Argb(0xFFFF9951) : ui::kWhite, 0, 1, 1);
        }
        if (first > 0) frame.paint.Image("interface\\mall\\mall_page_prev.ddj", l.prev, {0, 0}, {1, 1}, ui::kWhite);
        if (first + 4 < pages) frame.paint.Image("interface\\mall\\mall_page_next.ddj", l.next, {0, 0}, {1, 1}, ui::kWhite);
    }
    void Created(ui::Interface& ui) override {
        Static::Created(ui);
        interactive = true;
    }
    bool Hit(ui::Vec2 p) const override {
        const Layout l = Lay();
        const auto in = [&](const ui::Rect& r) { return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h; };
        return in(l.prev) || in(l.next) || in({l.open, l.y, l.close + 8.0f - l.open, 14.0f});
    }
    void Mouse(ui::Frame& frame) override {
        if (!frame.input.Pressed(ui::MouseButton::Left) || frame.pointer.hovered != this) return;
        const Layout l = Lay();
        const ui::Vec2 p = frame.pointer.position;
        const auto in = [&](const ui::Rect& r) { return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h; };
        const int first = page / 4 * 4;
        if (first > 0 && in(l.prev)) {
            Notify(ui::Notice::Kind::Clicked, first - 1);
            return;
        }
        if (first + 4 < pages && in(l.next)) {
            Notify(ui::Notice::Kind::Clicked, first + 4);
            return;
        }
        const int k = int(std::floor((p.x - l.numbers) / 20.0f));
        if (p.x >= l.numbers && k >= 0 && k < 4 && first + k < pages) Notify(ui::Notice::Kind::Clicked, first + k);
    }

private:
    struct Layout {
        float numbers = 0, open = 0, close = 0, y = 0;
        ui::Rect prev, next;
    };
    Layout Lay() const {
        const ui::Rect r = ScreenRect();
        Layout l;
        l.numbers = std::floor(r.x + (r.w - 80.0f) / 2.0f);
        l.y = std::floor(r.y + (r.h - 14.0f) / 2.0f);
        l.open = l.numbers - 8.0f;
        l.close = l.numbers + 80.0f;
        l.prev = {l.open - 13.0f - 36.0f, l.y + 1.0f, 36.0f, 12.0f};
        l.next = {l.close + 8.0f + 17.0f, l.y + 1.0f, 36.0f, 12.0f};
        return l;
    }
};

// The network's 2dt as controls (as the guild windows' builder, HudGuild.cpp): its rows (kind 8,
// CNIFStallNetworkSlot), their icons (CIFSlotWithHelpEx), the combo boxes (12), the pages (14) and the bars (15)
// as ours.
void BuildNetwork2dt(ui::Interface& ui, const std::vector<assets::Ui2dtRecord>& records, ui::Widget& window) {
    std::map<std::pair<int, int>, std::pair<ui::Widget*, assets::UiLayoutRect>> made;
    const assets::Ui2dtRecord& root = records.front();
    made[{root.id, 0}] = {&window, root.rect};
    for (std::size_t i = 1; i < records.size(); ++i) {
        const assets::Ui2dtRecord& r = records[i];
        const auto it = made.find({r.parents[0], r.parents[1]});
        if (it == made.end()) continue;
        assets::UiLayoutNode node = assets::Ui2dtNode(r, it->second.second.x, it->second.second.y);
        if (r.kind == 8 && r.className == "CNIFStallNetworkSlot") node.type = "CNIFStallNetworkRow";
        if (r.kind == 18 && r.className == "CIFSlotWithHelpEx") node.type = "CIFSlotWithHelp";
        if (r.kind == 12) node.type = "CIFComboBox";
        if (r.kind == 14) node.type = "CNIFStallNetworkPages";
        if (r.kind == 7) node.type = "CIFTextBox";
        if (r.kind == 15) {
            node.type = "CNIFGuildRow";
            for (std::string_view piece : {"left.ddj", "mid.ddj", "right.ddj", "Left.ddj", "Mid.ddj", "Right.ddj"}) {
                if (const std::size_t at = node.ddj.rfind(piece); at != std::string::npos) node.ddj.resize(at);
            }
        }
        ui::Widget* added = it->second.first->Add(ui.Make(node));
        made[{r.id, r.parents[0]}] = {added, r.rect};
    }
}

// CNIFStallNetworkMsgBox (res_ui\\nifstallnetworkmsgbox.2dt): a network row's purchase asked, as the original's network
// window asks it: the item's icon (26), its name (20), the amount (21) and the price (22, the number plain), Buy (4) and
// Cancel (7). Buy sends the purchase; either closes the box.
constexpr const char* kNetworkBoxLayout = "res_ui\\nifstallnetworkmsgbox.2dt";
constexpr int kNetworkBox = 151;

class NetworkBuyBox : public ui::MainFrame {
public:
    NetworkBuyBox(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
        if (const auto* records = c_.layout2dt ? c_.layout2dt(kNetworkBoxLayout) : nullptr; records != nullptr && !records->empty()) {
            Build2dt(ui, *this, *records);
        }
    }

    void Ask(const proto::NetworkRow& row) {
        row_ = row;
        const assets::ItemRow* item = c_.items != nullptr ? c_.items->Find(row.item.item) : nullptr;
        if (auto* icon = ui::As<ui::Static>(Child(26))) icon->texture = item != nullptr && !item->iconPath.empty() ? "icon\\" + item->iconPath : std::string();
        if (ui::Widget* w = Child(20)) w->SetText(item != nullptr ? c_.Text(item->nameKey) : std::string());
        if (ui::Widget* w = Child(21)) w->SetText(std::to_string(std::max<int>(1, row.item.quantity)));
        if (ui::Widget* w = Child(22)) w->SetText(std::to_string(row.price));
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr && n.source->parent() == this && (n.control == 4 || n.control == 7)) {
            if (n.control == 4 && c_.stallRequest && c_.stall != nullptr) {
                const_cast<game::StallView*>(c_.stall)->Buying(row_.listing);
                c_.stallRequest(proto::NetworkBuy::kId, proto::Encode(proto::NetworkBuy{row_.listing, row_.revision, row_.price}));
            }
            if (ui_ != nullptr) ui_->Close(this);
            return true;
        }
        return MainFrame::OnNotice(n);
    }

private:
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    proto::NetworkRow row_;
};

class StallNetworkWindow : public ui::MainFrame {
public:
    StallNetworkWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
        const auto* records = c_.layout2dt ? c_.layout2dt(kNetworkLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        BuildNetwork2dt(ui, *records, *this);
        // The 15 rows in the file's order (their parents' ids are not evenly spaced: 120, 129, 137, 145, 154, ...).
        for (const auto& child : children()) {
            if (dynamic_cast<NetworkRow*>(child.get()) != nullptr) rows_.push_back(child.get());
        }
        std::sort(rows_.begin(), rows_.end(), [](const ui::Widget* a, const ui::Widget* b) { return a->rect.y < b->rect.y; });
        Detail(ui);
        if (auto* combo = ui::As<ui::ComboBox>(Child(kClassCombo))) {
            combo->items.push_back(c_.Text("UIIT_CTL_WARENETWORK_SCAN_SELECT"));
            if (c_.stallNetwork != nullptr) {
                for (const assets::StallNetworkClass& k : c_.stallNetwork->Classes()) combo->items.push_back(c_.Text(k.textKey));
            }
            combo->Select(0);
        }
        Types();
        if (ui::Widget* w = Child(kSellerName)) w->SetText({});
    }

    void Update(ui::Frame& frame) override {
        MainFrame::Update(frame);
        if (auto* gold = ui::As<ui::Static>(Child(kGold)); gold != nullptr && c_.state != nullptr) {
            gold->SetText(GoldText(c_.state->inventory.gold));
            gold->textColor = GoldColor(c_.state->inventory.gold);
        }
        if (ui::Widget* search = Child(kSearch)) {
            search->enabled = c_.time >= searchAgain_;
            // The search's help (1.188's network window sets it at its creation): ten seconds between searches.
            if (frame.pointer.hovered == search) frame.interface.ShowTooltip(c_.Text("UIIT_MSG_WARENETWORK_SCAN_TOOLTIP"), search->ScreenRect());
        }
        if (ui::Widget* hint = Child(kNameHint)) {
            const auto* e = ui::As<ui::Edit>(Child(kNameEdit));
            hint->visible = e != nullptr && e->text().empty() && (ui_ == nullptr || ui_->focus() != e);
        }
        const game::StallView* v = c_.stall;
        if (v == nullptr || v->NetworkRevision() == seen_) return;
        seen_ = v->NetworkRevision();
        Show(*v);
    }

    bool OnNotice(const ui::Notice& n) override {
        // A combo's choice comes as ComboPicked.
        if (n.kind == ui::Notice::Kind::ComboPicked && n.control == kClassCombo) {
            Types();
            return true;
        }
        if (n.kind == ui::Notice::Kind::ComboPicked && n.control == kTypeCombo) {
            Degrees();
            return true;
        }
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr) return MainFrame::OnNotice(n);
        // A column's header: the page's rows by it, the other way when it is clicked again (the original's).
        if (n.control >= 1393 && n.control <= 1398) {
            sortBackwards_ = sortColumn_ == n.control ? !sortBackwards_ : false;
            sortColumn_ = n.control;
            chosen_ = -1;
            if (c_.stall != nullptr) Show(*c_.stall);
            return true;
        }
        switch (n.control) {
        case kSearch: Search(); return true;
        case kPages:
            Ask(std::uint16_t(n.value));
            return true;
        case kWhisper:
            if (const proto::NetworkRow* row = Chosen(); row != nullptr && ui_ != nullptr) {
                if (auto* chat = dynamic_cast<ChatWindow*>(ui_->Window(hud::kChat))) chat->Address(row->owner, true);
            }
            return true;
        case kPurchase: Purchase(); return true;
        case 2:
            if (c_.stallRequest) c_.stallRequest(proto::NetworkClose::kId, proto::Encode(proto::NetworkClose{}));
            return MainFrame::OnNotice(n);
        default: break;
        }
        for (std::size_t k = 0; k < rows_.size(); ++k) {
            if (rows_[k] != nullptr && (n.source == rows_[k] || n.source->parent() == rows_[k])) {
                chosen_ = int(k);
                Show(*c_.stall);
                return true;
            }
        }
        return MainFrame::OnNotice(n);
    }

    // The search as the combos say (scripts: class and type by index, degree 0 any).
    void Search(int classIndex = -1, int typeIndex = -1, int degree = -1) {
        auto* classes = ui::As<ui::ComboBox>(Child(kClassCombo));
        auto* types = ui::As<ui::ComboBox>(Child(kTypeCombo));
        auto* degrees = ui::As<ui::ComboBox>(Child(kDegreeCombo));
        if (classIndex >= 0 && classes != nullptr) {
            classes->Select(classIndex + 1);
            Types();
        }
        if (typeIndex >= 0 && types != nullptr) {
            types->Select(typeIndex);
            Degrees();
        }
        if (degree >= 0 && degrees != nullptr) degrees->Select(degree);
        const assets::StallNetworkType* type = TypeChosen();
        proto::NetworkSearch m;
        if (const auto* e = ui::As<ui::Edit>(Child(kNameEdit))) m.name = e->text();
        // A type, or a name (ours: the name alone searches every type).
        if ((type == nullptr && m.name.empty()) || c_.stall == nullptr || !c_.stallRequest || c_.time < searchAgain_) return;
        m.group = type != nullptr ? type->group : 0;
        m.degree = degrees != nullptr && degrees->selected > 0 ? std::uint8_t(degrees->selected) : 0;
        if (const auto* g = ui::As<ui::ComboBox>(Child(kGradeCombo)); g != nullptr && g->selected > 0) m.grade = proto::NetworkGrade(g->selected);
        if (const auto* s = ui::As<ui::ComboBox>(Child(kSexCombo)); s != nullptr && s->selected > 0) m.sex = proto::NetworkSex(s->selected);
        const_cast<game::StallView*>(c_.stall)->NewSearch(m.group, m.degree);
        c_.stallRequest(proto::NetworkSearch::kId, proto::Encode(m));
        searchAgain_ = c_.time + proto::kNetworkSearchSeconds;
        chosen_ = -1;
    }
    // A row of the page shown chosen and bought: its purchase box (CNIFStallNetworkMsgBox).
    void Purchase() {
        const proto::NetworkRow* row = Chosen();
        if (row == nullptr || ui_ == nullptr) return;
        if (ui::Widget* open = ui_->Window(kNetworkBox)) ui_->Close(open);
        const auto node = RootNode2dt(c_, kNetworkBoxLayout, "CNIFStallNetworkMsgBox", kNetworkBox);
        if (!node) return;
        if (auto* box = dynamic_cast<NetworkBuyBox*>(ui_->Open(*node))) {
            ui_->Centre(*box, ui_->screen());
            box->Ask(*row);
        }
    }
    void Choose(int row) {
        chosen_ = row;
        if (c_.stall != nullptr) Show(*c_.stall);
    }
    std::vector<StallNetworkPick> Shown() const {
        std::vector<StallNetworkPick> out;
        const game::StallView* v = c_.stall;
        if (v == nullptr) return out;
        if (const auto* rows = v->Rows(v->Page())) {
            for (const proto::NetworkRow& r : *rows) out.push_back({r.owner, r.listing});
        }
        return out;
    }

private:
    // Ours, the detailed search (the words 1.188 carries unused: UIIT_CTL_WARENETWORK_DETAIL_*): the grade, who may
    // wear it and a part of its name, in the strip between the seller and Search.
    void Detail(ui::Interface& ui) {
        const auto combo = [&](int id, float x, float w, std::initializer_list<const char*> keys) {
            assets::UiLayoutNode n;
            n.type = "CIFComboBox";
            n.id = id;
            n.rect = {x, 96.0f, w, 20.0f};
            if (auto* box = ui::As<ui::ComboBox>(Add(ui.Make(n)))) {
                for (const char* k : keys) box->items.push_back(c_.Text(k));
                box->Select(0);
            }
        };
        combo(kGradeCombo, 171.0f, 100.0f,
              {"UIIT_CTL_WARENETWORK_DETAIL_TOTAL", "UIIT_CTL_WARENETWORK_DETAIL_NORMAL", "UIIT_CTL_WARENETWORK_DETAIL_MAGIC", "UIIT_CTL_WARENETWORK_DETAIL_RARE"});
        combo(kSexCombo, 276.0f, 90.0f, {"UIIT_CTL_WARENETWORK_DETAIL_TOTAL", "UIIT_CTL_WARENETWORK_DETAIL_MAN", "UIIT_CTL_WARENETWORK_DETAIL_WOMAN"});
        assets::UiLayoutNode square;
        square.type = "CIFFrame";
        square.ddj = "interface\\ifcommon\\com_blacksquare_";
        square.rect = {371.0f, 95.0f, 152.0f, 22.0f};
        Add(ui.Make(square));
        assets::UiLayoutNode edit;
        edit.type = "CIFEdit";
        edit.id = kNameEdit;
        edit.rect = {375.0f, 97.0f, 144.0f, 18.0f};
        if (auto* e = ui::As<ui::Edit>(Add(ui.Make(edit)))) e->limit = 40;
        // Its hint while empty: "Write name of item" in grey.
        assets::UiLayoutNode hint;
        hint.type = "CIFStatic";
        hint.id = kNameHint;
        hint.rect = {377.0f, 97.0f, 140.0f, 18.0f};
        if (auto* h = ui::As<ui::Static>(Add(ui.Make(hint)))) {
            h->SetText(c_.Text("UIIT_CTL_WARENETWORK_DETAIL_WRITENAME"));
            h->textColor = ui::Argb(0xFF808080);
        }
    }
    void Ask(std::uint16_t page) {
        game::StallView* v = const_cast<game::StallView*>(c_.stall);
        if (v == nullptr) return;
        chosen_ = -1;
        v->SetPage(page);
        // A page already received is shown as it is; another is asked for (free).
        if (v->Rows(page) == nullptr && c_.stallRequest) c_.stallRequest(proto::NetworkPageAsk::kId, proto::Encode(proto::NetworkPageAsk{page}));
    }
    const assets::StallNetworkClass* ClassChosen() const {
        auto* classes = ui::As<ui::ComboBox>(const_cast<StallNetworkWindow*>(this)->Child(kClassCombo));
        if (classes == nullptr || c_.stallNetwork == nullptr || classes->selected < 1) return nullptr;
        const auto& all = c_.stallNetwork->Classes();
        return std::size_t(classes->selected - 1) < all.size() ? &all[std::size_t(classes->selected - 1)] : nullptr;
    }
    const assets::StallNetworkType* TypeChosen() const {
        const assets::StallNetworkClass* k = ClassChosen();
        auto* types = ui::As<ui::ComboBox>(const_cast<StallNetworkWindow*>(this)->Child(kTypeCombo));
        if (k == nullptr || types == nullptr || types->selected < 0 || std::size_t(types->selected) >= k->types.size()) return nullptr;
        return &k->types[std::size_t(types->selected)];
    }
    void Types() {
        auto* types = ui::As<ui::ComboBox>(Child(kTypeCombo));
        if (types == nullptr) return;
        types->items.clear();
        if (const assets::StallNetworkClass* k = ClassChosen()) {
            for (const assets::StallNetworkType& t : k->types) types->items.push_back(c_.Text(t.textKey));
        }
        types->Select(types->items.empty() ? -1 : 0);
        Degrees();
    }
    void Degrees() {
        auto* degrees = ui::As<ui::ComboBox>(Child(kDegreeCombo));
        if (degrees == nullptr) return;
        degrees->items.clear();
        const assets::StallNetworkType* t = TypeChosen();
        if (t == nullptr || t->degrees == 0) {
            degrees->items.push_back(c_.Text("UIIT_CTL_WARENETWORK_SCAN_END")); // "None"
        } else {
            degrees->items.push_back(c_.Text("UIIT_CTL_WARENETWORK_DETAIL_TOTAL")); // "All"
            for (int d = 1; d <= t->degrees; ++d) degrees->items.push_back(std::to_string(d));
        }
        degrees->Select(0);
        degrees->enabled = t != nullptr && t->degrees != 0;
    }
    const proto::NetworkRow* Chosen() const {
        const game::StallView* v = c_.stall;
        const auto* rows = v != nullptr ? v->Rows(v->Page()) : nullptr;
        if (rows == nullptr || chosen_ < 0 || std::size_t(chosen_) >= rows->size()) return nullptr;
        const std::size_t at = std::size_t(chosen_) < order_.size() ? order_[std::size_t(chosen_)] : std::size_t(chosen_);
        return at < rows->size() ? &(*rows)[at] : nullptr;
    }
    // The page's rows in the order of the header clicked last: its number (the server's order), level, name, count or price.
    void Order(const std::vector<proto::NetworkRow>* rows) {
        order_.clear();
        if (rows == nullptr) return;
        for (std::size_t i = 0; i < rows->size(); ++i) order_.push_back(i);
        const auto level = [&](std::size_t i) {
            const assets::ItemRow* item = c_.items != nullptr ? c_.items->Find((*rows)[i].item.item) : nullptr;
            return item != nullptr ? item->requiredLevel : 0;
        };
        const auto name = [&](std::size_t i) {
            const assets::ItemRow* item = c_.items != nullptr ? c_.items->Find((*rows)[i].item.item) : nullptr;
            return item != nullptr ? c_.Text(item->nameKey) : std::string();
        };
        std::stable_sort(order_.begin(), order_.end(), [&](std::size_t a, std::size_t b) {
            const auto less = [&](std::size_t x, std::size_t y) {
                switch (sortColumn_) {
                case 1398: return level(x) < level(y);
                case 1393: return name(x) < name(y);
                case 1394: return (*rows)[x].item.quantity < (*rows)[y].item.quantity;
                case 1395: return (*rows)[x].price < (*rows)[y].price;
                default: return x < y;
                }
            };
            return sortBackwards_ ? less(b, a) : less(a, b);
        });
    }
    void Show(const game::StallView& v) {
        const auto* rows = v.Rows(v.Page());
        Order(rows);
        for (std::size_t k = 0; k < rows_.size(); ++k) {
            ui::Widget* row = rows_[k];
            if (row == nullptr) continue;
            const std::size_t at = k < order_.size() ? order_[k] : k;
            const proto::NetworkRow* r = rows != nullptr && at < rows->size() ? &(*rows)[at] : nullptr;
            if (auto* lit = dynamic_cast<NetworkRow*>(row)) lit->chosen = r != nullptr && int(k) == chosen_;
            const assets::ItemRow* item = r != nullptr && c_.items != nullptr ? c_.items->Find(r->item.item) : nullptr;
            const game::ItemInstance instance = r != nullptr ? game::Inventory::FromRecord(r->item) : game::ItemInstance{};
            const auto set = [&](int id, const std::string& text) {
                if (ui::Widget* w = row->Child(id)) w->SetText(text);
            };
            set(kRowNumber, r != nullptr ? std::to_string(std::size_t(v.Page()) * proto::kNetworkPageRows + at + 1) : std::string());
            set(kRowLevel, item != nullptr ? std::to_string(item->requiredLevel) : std::string());
            set(kRowName, item != nullptr ? c_.Text(item->nameKey) + (instance.plus > 0 ? " (+" + std::to_string(instance.plus) + ")" : "") : std::string());
            set(kRowCount, r != nullptr ? std::to_string(std::max<int>(1, r->item.quantity)) : std::string());
            set(kRowPrice, r != nullptr ? GoldText(r->price) : std::string());
            if (auto* icon = ui::As<ui::Slot>(row->Child(kRowIcon))) {
                ShowItem(*icon, r != nullptr ? &instance : nullptr, c_);
                icon->pickable = false;
                const proto::NetworkRow copy = r != nullptr ? *r : proto::NetworkRow{};
                icon->tooltipLines = [this, copy] {
                    const assets::ItemRow* it = c_.items != nullptr ? c_.items->Find(copy.item.item) : nullptr;
                    if (it == nullptr) return std::vector<ui::TooltipLine>{};
                    auto lines = ItemTooltip(*it, game::Inventory::FromRecord(copy.item), c_);
                    // Ours: where it is sold (every town's stalls are listed).
                    lines.push_back({});
                    std::string where = c_.regionKey ? c_.regionKey(copy.region) : std::string();
                    where = !where.empty() ? c_.Text(where) : c_.townName ? c_.townName(copy.town) : std::string();
                    lines.push_back({c_.Text("UIIT_STT_WARENETWORK_SELLER") + ": " + copy.owner + (where.empty() ? std::string() : " (" + where + ")"),
                                     ui::Argb(0xFFFEB5FF)});
                    return lines;
                };
            }
        }
        const proto::NetworkRow* chosen = Chosen();
        if (ui::Widget* w = Child(kSellerName)) w->SetText(chosen != nullptr ? chosen->owner : std::string());
        if (auto* pages = dynamic_cast<NetworkPages*>(Child(kPages))) {
            pages->page = v.Page();
            pages->pages = v.Pages();
        }
    }

    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::vector<ui::Widget*> rows_;
    std::uint64_t seen_ = ~std::uint64_t(0);
    int chosen_ = -1;
    double searchAgain_ = 0.0;
    int sortColumn_ = 0;             // the header clicked last (1393 name, 1394 count, 1395 price, 1397 number, 1398 level)
    bool sortBackwards_ = false;     // clicked again: the other way
    std::vector<std::size_t> order_; // the page's rows as shown
};

} // namespace

void RegisterStallWindows(ui::Interface& ui, HudContext& context) {
    HudContext* c = &context;
    ui.Register("CIFStall", [c](const assets::UiLayoutNode& n) { return std::make_unique<StallWindow>(n, *c); });
    ui.Register("CNIFStallNetworkRow", [](const assets::UiLayoutNode& n) {
        assets::UiLayoutNode node = n;
        node.ddj.clear();
        return std::make_unique<NetworkRow>(node);
    });
    ui.Register("CNIFStallNetworkPages", [](const assets::UiLayoutNode& n) { return std::make_unique<NetworkPages>(n); });
    ui.Register("CNIFStallNetworkMsgBox", [c](const assets::UiLayoutNode& n) { return std::make_unique<NetworkBuyBox>(n, *c); });
    // The network's window: 1.188's CNIFStallNetwork from its 2dt (the root's size and frame).
    ui.Register("CIFStallNetwork", [c](const assets::UiLayoutNode& n) -> std::unique_ptr<ui::Widget> {
        assets::UiLayoutNode node = n;
        node.type = "CNIFStallNetwork";
        if (const auto* records = c->layout2dt ? c->layout2dt(kNetworkLayout) : nullptr; records != nullptr && !records->empty()) {
            node.ddj = records->front().frame;
            node.rect.w = records->front().rect.w;
            node.rect.h = records->front().rect.h;
        }
        return std::make_unique<StallNetworkWindow>(node, *c);
    });
}

void StallRowChosen(ui::Widget& row, bool chosen) {
    if (auto* lit = dynamic_cast<NetworkRow*>(&row)) lit->chosen = chosen;
}

void StallPages(ui::Widget& pages, int page, int count) {
    if (auto* p = dynamic_cast<NetworkPages*>(&pages)) {
        p->page = page;
        p->pages = count;
    }
}

void StallRegister(HudContext& context, int slot, int place) {
    if (context.hud == nullptr || context.state == nullptr || context.items == nullptr) return;
    DealBox::Deal deal;
    deal.kind = DealBox::Deal::Kind::StallRegister;
    if (place == 0) {
        // A slot's item: its new price asked, the present one in the box.
        if (context.stall == nullptr || slot < 0 || slot >= int(proto::kStallSlots)) return;
        const auto& item = context.stall->Items()[std::size_t(slot)];
        const assets::ItemRow* row = item ? context.items->Find(item->item.item) : nullptr;
        if (row == nullptr) return;
        deal.icon = "icon\\" + row->iconPath;
        deal.name = context.Text(row->nameKey);
        deal.count = std::max<int>(1, item->item.quantity);
        deal.unitPrice = item->price;
    } else {
        const game::ItemInstance* item = context.state->inventory.At(std::uint8_t(place));
        const assets::ItemRow* row = item != nullptr ? context.items->Find(item->refId) : nullptr;
        if (row == nullptr) return;
        deal.icon = "icon\\" + row->iconPath;
        deal.name = context.Text(row->nameKey);
        deal.count = std::max<int>(1, item->count);
    }
    deal.most = deal.count;
    deal.place = place;
    deal.stallSlot = slot;
    context.hud->Deal(deal);
}

void StallBuyBox(HudContext& context, int slot) {
    if (context.hud == nullptr || context.items == nullptr || context.stall == nullptr || context.stall->Mine()) return;
    if (slot < 0 || slot >= int(proto::kStallSlots)) return;
    const auto& item = context.stall->Items()[std::size_t(slot)];
    const assets::ItemRow* row = item ? context.items->Find(item->item.item) : nullptr;
    if (row == nullptr) return;
    DealBox::Deal deal;
    deal.kind = DealBox::Deal::Kind::StallBuy;
    deal.icon = "icon\\" + row->iconPath;
    deal.name = context.Text(row->nameKey);
    deal.count = deal.most = std::max<int>(1, item->item.quantity);
    deal.unitPrice = item->price;
    deal.stallSlot = slot;
    context.hud->Deal(deal);
}

int StallSlotOf(const ui::Widget* slot) {
    if (slot == nullptr || dynamic_cast<const StallWindow*>(slot->parent()) == nullptr) return -1;
    const int id = slot->id() - kSlotIcon;
    return id >= 0 && id < int(proto::kStallSlots) ? id : -1;
}

void StallNetworkSearch(ui::Interface& ui, int classIndex, int typeIndex, int degree) {
    if (auto* w = dynamic_cast<StallNetworkWindow*>(ui.Window(hud::kStallNetwork))) w->Search(classIndex, typeIndex, degree);
}

void StallNetworkBuy(ui::Interface& ui, int row) {
    if (auto* w = dynamic_cast<StallNetworkWindow*>(ui.Window(hud::kStallNetwork))) {
        w->Choose(row);
        w->Purchase();
    }
}

std::vector<StallNetworkPick> StallNetworkRows(ui::Interface& ui) {
    if (auto* w = dynamic_cast<StallNetworkWindow*>(ui.Window(hud::kStallNetwork))) return w->Shown();
    return {};
}

const char* StallCodeKey(Code code) {
    switch (code) {
    case Code::NotBagItem: return "UIIT_MSG_STRGERR_CANNOT_BE_TRADED"; // ours
    case Code::BadPrice: return "UIIT_MSG_FLEAMARKET_ERR_INVALID_PRICE";
    case Code::NothingToSell: return "UIIT_MSG_FLEAMARKET_ERR_NOTHING_TO_SELL";
    case Code::Closed: return "UIIT_MSG_FLEAMARKET_ERR_MARKET_CLOSED";
    case Code::NotEnoughGold:
    case Code::NetworkNoGold: return "UIIT_MSG_FLEAMARKET_ERR_NOT_ENOUGH_GOLD";
    case Code::BagFull:
    case Code::NetworkBagFull: return "UIIT_MSG_FLEAMARKET_ERR_INVENTORY_FULL";
    case Code::HostLeft: return "UIIT_MSG_FLEAMARKET_ERR_HOST_LEFT";
    case Code::Busy: return "UIIT_MSG_FLEAMARKET_ERR_IM_BUSY";
    case Code::ClosedByHost: return "UIIT_MSG_FLEAMARKET_ERR_CLOSED_BY_HOST";
    case Code::Full: return "UIIT_MSG_FLEAMARKET_ERR_MARKET_FULL";
    case Code::Modifying:
    case Code::NotOpen: return "UIIT_MSG_FLEAMARKET_ERR_INVALID_HOST_STATE";
    case Code::Riding: return "UIIT_MSG_FLEAMARKET_ERR_CANT_OPEN_MARKET_IN_RIDESTATE";
    case Code::BadName: return "UIIT_MSG_FLEAMARKET_ERR_NOT_ALLOWED_FMARKETNAME";
    case Code::Murderer: return "UIIT_MSG_FLEAMARKET_ERR_CANT_OPEN_MARKET_MURDERER";
    case Code::JobSuit: return "UIIT_MSG_FREEMARKET_NOTUSE_JOB";
    case Code::NetworkFailed: return "UIIT_MSG_WARENETWORK_ERROR_FAIL";
    case Code::TooSoon: return "UIIT_MSG_WARENETWORK_SCAN_FAIL";
    case Code::NotTownSearch:
    case Code::NotTown: return "UIIT_MSG_WARENETWORK_USE_CITY";
    case Code::Changed: return "UIIT_MSG_WARENETWORK_BUY_ERROR";
    case Code::Rented: return "UIIT_CTL_NASRUN_NOT_TRADE";
    // Any other of the original's codes: its table's words (ErrorTexts.h); ours below 0x100 stay quiet.
    default: return std::uint16_t(code) >= 0x100 ? OriginalErrorKey(std::uint16_t(code)) : nullptr;
    }
}

} // namespace sr::client
