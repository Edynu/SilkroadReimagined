// The alchemy window: 1.188's CNIFEnchantWnd (window 168) from res_ui\nifenchantwnd.2dt, opened
// by the Menu's row 114, the key Y and the Action page's 1010. Its Alchemy page (5) has four tabs:
// Disjoint (9) and Dismantle (10) on the Type2 page (28: the hidden Rondo slot 70, eight material slots), Manufacture
// (7) and Strengthen (8) on the Type1 page (12: the main slot 17, four lower slots 26..29); the action button (22), the
// gauge (21), the help text (18) and the effect sprite over a slot. Its Socket page (72):
// the item's slot (70) and name (68), three sockets (83..85 under their closed marks 73, 79, 80), the gauge (211), the
// help line (354), Make (56: pick mode 1, cursor 174, a click on an unmade socket, 400 ms, the request) and Remove (57:
// pick mode 2, cursor 175, a click on a filled socket, the confirm box, 400 ms with alcm_socket_fx02, the request); a
// socket stone dropped on a socket asks, then goes in on the server's 4th tick (the gauge 2000 ms with
// alcm_socket_fx01; Remove is its Cancel meanwhile).
//
// A right click on a bag item (or a drop) puts it in the slot its kind goes to, with the client's own checks
// (codes 2..17); the action button sends the request (AlchemyProtocol.h) and runs the gauge (0.5 s, 3 s
// for Strengthen) and the prepare effect; the server's answer plays the success or the failure effect and its sound,
// and says what happened in the system lines. While the window shows, the bag is held (the inventory's moves do
// nothing). The Dismantle tab asks first (the alert box 172, res_ui\nifenchantalchemymsgbox.2dt).

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/game/Alchemy.h"
#include "sr/game/AlchemyProtocol.h"
#include "sr/core/Log.h"
#include "sr/game/Inventory.h"

namespace sr::client {

namespace proto = game::proto;
namespace alchemy = game::alchemy;

namespace {

constexpr const char* kLayout = "res_ui\\nifenchantwnd.2dt";
constexpr const char* kAlertLayout = "res_ui\\nifenchantalchemymsgbox.2dt";
constexpr int kAlchemyButton = 4, kSocketButton = 3, kAlchemyPage = 5, kSocketPage = 72;
constexpr int kType1 = 12, kType2 = 28, kHelp = 18, kGauge = 21, kAction = 22;
constexpr int kTabIds[4] = {9, 10, 7, 8}; // Disjoint, Dismantle, Manufacture, Strengthen
constexpr int kType1Slots[5] = {17, 26, 27, 28, 29};
constexpr int kType2Slots[9] = {70, 66, 67, 68, 65, 62, 63, 64, 69};
enum Tab { kDisjoint = 0, kDismantle = 1, kManufacture = 2, kStrengthen = 3 };
constexpr const char* kTabKeys[4] = {"UIIT_CTL_ALCHEMYBOX_TAP_DISJOINTING", "UIIT_CTL_ALCHEMYBOX_TAP_DISMANTLING", "UIIT_CTL_ALCHEMYBOX_TAP_MANUFACTURING",
                                     "UIIT_CTL_ALCHEMYBOX_TAP_STRENGTHENING"};
constexpr const char* kClosed = "interface\\skill\\skl_mastery_nothing.ddj";
// The Socket page's controls (the same 2dt) and its pick modes' cursors (the client's resources 174, 175).
constexpr int kSocketTarget = 70, kSocketName = 68, kSocketHelp = 354, kSocketGauge = 211, kSocketMake = 56, kSocketRemove = 57;
constexpr int kSocketSlots[3] = {83, 84, 85};
constexpr int kSocketMarks[3] = {73, 79, 80};
constexpr int kCursorMake = 174, kCursorRemove = 175;
constexpr double kSocketPunchSeconds = 0.4, kSocketInsertSeconds = 2.0;
// The tab shown, kept between openings (1.188's global byte, Strengthen at first).
int g_tab = kStrengthen;

// The client's refusals and their texts; 1 (the same bag item twice) says nothing.
const char* RefusalKey(int code) {
    switch (code) {
    case 2: return "UIIT_MSG_ENCHANT_LOAD_RONDO1_FIRST_2";
    case 3: return "UIIT_MSG_ENCHANT_NOT_LOAD_MATERIAL";
    case 4: return "UIIT_MSG_ENCHANT_LOAD_RONDO2_FIRST_2";
    case 5: return "UIIT_MSG_ENCHANT_NOT_LOAD_EQUIP";
    case 6: return "UIIT_MSG_ENCHANT_LOAD_TABLET_FIRST";
    case 7: return "UIIT_MSG_ENCHANT_LOAD_ELEMENT_FIRST";
    case 8: return "UIIT_MSG_ENCHANT_LOAD_EQUIP_FIRST";
    case 9: return "UIIT_MSG_REINFORCE_NOT_LOAD_INGREDIENT";
    case 10: return "UIIT_MSG_REINFORCERR_RECIPE_MISMATCH";
    case 11: return "UIIT_MSG_ENCHANT_NOT_LOAD_INGREDIENT2";
    case 12: return "UIIT_MSG_ENCHANT_EQUIPCLASS_MISMATCH";
    case 13: return "UIIT_STRGERR_CANNOT_BE_USED_TO_TARGET";
    case 15: return "UIIT_MSG_REINFORCERR_RECIPE_NOT_LOADED";
    case 16: return "UIIT_MSG_REINFORCERR_RECIPE_LEVEL_MISMATCH";
    case 17: return "UIIT_MSG_REINFORCERR_ALREADY_DONE";
    default: return nullptr;
    }
}

// The server's codes (categories 0x12 and 0x17).
const char* ServerKey(std::uint16_t code) {
    switch (code) {
    case alchemy::code::kTargetMissing: return "UIIT_MSG_REINFORCERR_TARGET_EQUIP_NOT_LOADED";
    case alchemy::code::kRecipeMissing: return "UIIT_MSG_REINFORCERR_RECIPE_NOT_LOADED";
    case alchemy::code::kOnlyEquipment: return "UIIT_MSG_REINFORCERR_ONLY_EQUIPITEM_CAN_BE_REINFORCED";
    case alchemy::code::kRecipeMismatch:
    case 0x5427: return "UIIT_MSG_REINFORCERR_RECIPE_MISMATCH";
    case alchemy::code::kPowderDegree: return "UIIT_MSG_REINFORCERR_EQUIPCLASS_MISMATCH_PROB_UP";
    case alchemy::code::kCannotReinforce: return "UIIT_MSG_REINFORCERR_TARGETEQUIP_CANT_BE_REINFORCED";
    case alchemy::code::kNotMaterial: return "UIIT_MSG_REINFORCERR_IS_NOT_REINFORCE_STUFF";
    case alchemy::code::kTwice: return "UIIT_MSG_REINFORCERR_STUFF_CANT_BE_LOADED_MULTIPLE_TIME";
    case alchemy::code::kNothing: return "UIIT_MSG_REINFORCERR_NO_ITEM_LOADED";
    case alchemy::code::kBadSlot: return "UIIT_MSG_REINFORCERR_INVALID_ITEM_SLOT";
    case 0x5419: return "UIIT_MSG_REINFORCERR_CANNOT_TRADE";
    case alchemy::code::kStoneMismatch: return "UIIT_MSG_ENCHANT_EQUIPCLASS_MISMATCH";
    case alchemy::code::kChargesFull: return "UIIT_MSG_STRGERR_MAGICSTONE2_MAX";
    case alchemy::code::kFailed: return "UIIT_MSG_REINFORCERR_FAIL";
    case alchemy::code::kAstral:
    case 0x5420: return "UIIT_MSG_STRGERR_ASTRAL";
    case alchemy::code::kHonor: return "UIIT_MSG_TC_ERROR_ENCHANT_HONOR_ITEM";
    case alchemy::code::kAlreadyDone: return "UIIT_MSG_REINFORCERR_ALREADY_DONE";
    case alchemy::code::kNotEnough: return "UIIT_MSG_ENCHANT_NOT_ENOUGH_ITEM";
    case alchemy::code::kNoRoom: return "UIIT_MSG_ALCHEMY_NOT_ENOUGH_INVENTORY";
    case alchemy::code::kCannotTarget: return "UIIT_STRGERR_CANNOT_BE_USED_TO_TARGET";
    case alchemy::code::kRepairInvalid: return "UIIT_MSG_ENCHANTERR_RONDO2_UNREPAIRABLE_ITEM";
    case alchemy::code::kHonorRondo: return "UIIT_MSG_TC_ERROR_NOT_RONDO_HONOR_ITEM";
    case alchemy::code::kAvatar: return "UIIT_MSG_ENCHANT_SLOT_MISMATCH_REINFORCE";
    default: return "UIIT_MSG_ENCHANT_FAILED";
    }
}

// A socket's text for the server's codes (by socket family).
const char* SocketKey(std::uint16_t code) {
    switch (code) {
    case alchemy::code::kSocketNoMaterial: return "UIIT_STT_SOCKETERR_NOT_EXIST_RECIPE";
    case alchemy::code::kSocketsFull: return "UIIT_STT_SOCKETERR_OVERFLOW_RESERVED_SOCKET_HOLE_COUNT";
    case alchemy::code::kSocketNotAllowed: return "UIIT_STT_SOCKETERR_CANT_HOLE_PUNCHING_THIS_ITEM";
    case alchemy::code::kSocketMade: return "UIIT_STT_SOCKETERR_THIS_HOLE_PUNCHED_ALREADY";
    case alchemy::code::kSocketSameStone: return "UIIT_STT_SOCKETERR_SAME_SOCKET_STONE_IS_INSERTED";
    case alchemy::code::kSocketMissing: return "UIIT_STT_SOCKETERR_THERE_IS_NO_EMPTY_HOLE";
    case alchemy::code::kSocketStoneKind: return "UIIT_STT_SOCKETERR_THE_ITEM_ISNT_PARING_THIS_SOCKET_STONE";
    case alchemy::code::kSocketInUse: return "UIIT_STT_SOCKETERR_THIS_HOLE_USED_ALREADY";
    case alchemy::code::kSocketDegree: return "UIIT_STT_SOCKETERR_ITEM_LEVEL_LESS_THAN_THIS_SOCKET_STONE";
    case alchemy::code::kSocketEmpty: return "UIIT_STT_SOCKETERR_THIS_HOLE_IS_EMPTY";
    case alchemy::code::kSocketTwice: return "UIIT_STT_SOCKETERR_CANT_BE_LOADED_MULTIPLE_TIME";
    case alchemy::code::kSocketNotStuff: return "UIIT_STT_SOCKETERR_ITS_NOT_SOCKET_STUFF";
    default: return ServerKey(code);
    }
}

// A socket's effect (the Socket page): 128 x 128 frames, 24 to a 512 x 768 sheet (4 x 6), 100 ms each, once, drawn
// additively over the socket: alcm_socket_fx01 putting a stone in, alcm_socket_fx02 taking one out.
class SocketEffect : public ui::Widget {
public:
    using Widget::Widget;
    std::string art;
    double started = 0.0;
    void Play(const char* path, double now) {
        art = path;
        started = now;
        visible = true;
    }
    void Update(ui::Frame& frame) override {
        Widget::Update(frame);
        if (!art.empty() && frame.time - started >= 2.4) {
            art.clear();
            visible = false;
        }
    }
    void Draw(ui::Frame& frame) override {
        if (art.empty()) return;
        const int f = std::clamp(int((frame.time - started) / 0.1), 0, 23);
        const ui::Vec2 uv0{float(f % 4) * 0.25f, float(f / 4) / 6.0f};
        frame.paint.PushAdditive();
        frame.paint.Image(art, ScreenRect(), uv0, {uv0.x + 0.25f, uv0.y + 1.0f / 6.0f}, ui::WithAlpha(ui::kWhite, Opacity()));
        frame.paint.PopAdditive();
    }
};

// The effect over a slot: 64 x 64 frames, 16 to a 256 x 256 sheet, 100 ms each, drawn 96 x 96;
// the prepare effect loops, success and failure play once.
class AlchemyEffect : public ui::Widget {
public:
    using Widget::Widget;
    enum class Kind { None, Prepare, Success, Fail };
    Kind kind = Kind::None;
    double started = 0.0;
    void Play(Kind k, double now) {
        kind = k;
        started = now;
        visible = k != Kind::None;
    }
    // The frame the effect is at (-1: over).
    int FrameAt(double now) const {
        const int f = int((now - started) / 0.1);
        if (kind == Kind::Prepare) return f % 16;
        return f < 16 ? f : -1;
    }
    void Draw(ui::Frame& frame) override {
        if (kind == Kind::None) return;
        const int f = FrameAt(frame.time);
        if (f < 0) return;
        const char* art = kind == Kind::Prepare   ? "interface\\alchemy\\alcm_effect_prepare.ddj"
                          : kind == Kind::Success ? "interface\\alchemy\\alcm_effect_success.ddj"
                                                  : "interface\\alchemy\\alcm_effect_fail_1.ddj";
        const ui::Vec2 uv0{float(f % 4) * 0.25f, float(f / 4) * 0.25f};
        // Added onto the window (the sheets are drawn on black; the original's sprite blends additively).
        frame.paint.PushAdditive();
        frame.paint.Image(art, ScreenRect(), uv0, {uv0.x + 0.25f, uv0.y + 0.25f}, ui::WithAlpha(ui::kWhite, Opacity()));
        frame.paint.PopAdditive();
    }
};

// The alert box (172, CNIFMiniConfirm): the item's icon (9), its "name +N" and the question (8), OK (3), Cancel (4).
class AlchemyAlert : public ui::MainFrame {
public:
    AlchemyAlert(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}
    std::function<void(bool)> answer;
    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        const auto* records = c_.layout2dt ? c_.layout2dt(kAlertLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        Build2dt(ui, *this, *records, [](const assets::Ui2dtRecord& r, assets::UiLayoutNode& n) {
            if (r.className == "CIFSlotWithHelpEx") n.type = "CIFSlotWithHelp";
        });
    }
    void Fill(const std::string& title, const std::string& icon, const std::string& text, const std::string& ok, const std::string& cancel) {
        if (ui::Widget* t = Child(0)) t->SetText(title);
        if (auto* slot = ui::As<ui::Slot>(Child(9))) {
            slot->icon = icon;
            slot->pickable = false;
        }
        if (ui::Widget* w = Child(8)) w->SetText(text);
        if (ui::Widget* w = Child(3)) w->SetText(ok);
        if (ui::Widget* w = Child(4)) w->SetText(cancel);
    }
    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr && (n.source->id() == 3 || n.source->id() == 4)) {
            const bool yes = n.source->id() == 3;
            visible = false;
            if (answer) answer(yes);
            return true;
        }
        return ui::MainFrame::OnNotice(n);
    }

private:
    HudContext& c_;
};

class AlchemyWindow : public ui::MainFrame {
public:
    AlchemyWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        ui_ = &ui;
        const auto* records = c_.layout2dt ? c_.layout2dt(kLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        if (ui::Widget* title = Child(0)) title->SetText(c_.Text("UIIT_CTL_ALCHEMYBOX"));
        const std::vector<ui::Widget*> made = Build2dt(ui, *this, *records, [](const assets::Ui2dtRecord& r, assets::UiLayoutNode& n) {
            if (r.className == "CIFSlotWithHelpEx") n.type = "CIFSlotWithHelp";
            if (r.className == "CIFPMLEx") n.type = "CIFPML";
        });
        for (std::size_t i = 0; i < records->size(); ++i) {
            const int id = (*records)[i].id;
            if (id == kAlchemyPage && made[i] != this) alchemyPage_ = made[i];
            if (id == kSocketPage) socketPage_ = made[i];
            if (id == kType1 && (*records)[i].className == "CNIFAlchemySubWndType1") type1_ = made[i];
            if (id == kType2 && (*records)[i].className == "CNIFAlchemySubWndType2") type2_ = made[i];
        }
        if (alchemyPage_ == nullptr || type1_ == nullptr || type2_ == nullptr) return;
        // Create: the Alchemy page shown, the Socket page hidden (the buttons' tooltips: Update).
        if (socketPage_ != nullptr) socketPage_->visible = false;
        if (ui::Widget* rondo = type2_->Child(kType2Slots[0])) rondo->visible = false; // the Rondo's, filled from the bag
        // Every slot: no picking up (a right click takes it out), its item's tooltip.
        for (ui::Widget* page : {type1_, type2_}) {
            for (const auto& child : page->children()) {
                auto* slot = ui::As<ui::Slot>(child.get());
                if (slot == nullptr) continue;
                slot->pickable = false;
                slot->tooltipLines = [this, slot] {
                    const int key = KeyOf(slot);
                    const game::ItemInstance* item = key >= 0 ? ItemAt(key) : nullptr;
                    const assets::ItemRow* row = item != nullptr && c_.items ? c_.items->Find(item->refId) : nullptr;
                    return row != nullptr ? ItemTooltip(*row, *item, c_) : std::vector<ui::TooltipLine>{};
                };
            }
        }
        // The effect, made in code (96 x 96), on the Alchemy page.
        assets::UiLayoutNode fx;
        fx.type = "CIFStatic";
        fx.id = 1004;
        fx.rect = {0, 0, 96, 96};
        effect_ = static_cast<AlchemyEffect*>(alchemyPage_->Add(std::make_unique<AlchemyEffect>(fx)));
        effect_->visible = false;
        SocketCreated();
        onDrop = [this](ui::Widget* source, ui::Widget* target) {
            const int place = InventoryPlaceOf(source);
            if (place < game::slot::kWorn) return false;
            // A stone dropped on a socket (the Socket page).
            if (socketPage_ != nullptr && socketPage_->visible) {
                for (int k = 0; k < game::kSockets; ++k) {
                    if (target != nullptr && (target == SocketSlot(k) || target == SocketMark(k))) {
                        SocketDropStone(place, k);
                        return true;
                    }
                }
            }
            Put(place);
            return true;
        };
        ShowTab(g_tab);
    }

    // A bag item put in (a right click on it, or dropped): the slot its kind goes to, if the tab takes it.
    void Put(int place) {
        if (socketPage_ != nullptr && socketPage_->visible) {
            SocketPut(place);
            return;
        }
        if (running_ || alert_ != nullptr && alert_->visible) return;
        const game::ItemInstance* item = c_.state != nullptr ? c_.state->inventory.At(std::uint8_t(place)) : nullptr;
        const assets::ItemRow* row = item != nullptr && c_.items ? c_.items->Find(item->refId) : nullptr;
        if (row == nullptr) return;
        for (const Placed& p : slots_) {
            if (p.place == place) return; // code 1: the same bag item twice
        }
        int key = -1;
        const int code = Accepts(*row, *item, key);
        if (code != 0) {
            if (const char* k = RefusalKey(code); k != nullptr && c_.systemLine) c_.systemLine(c_.Text(k));
            return;
        }
        if (key == 0 && (g_tab == kStrengthen || g_tab == kManufacture)) slots_.fill({});
        slots_[std::size_t(key)] = {place, item->serial};
        Refresh();
    }

    // The server's answer (the bag changed just before it).
    void Result(const proto::AlchemyResult& r) {
        const auto say = [&](const std::string& line) {
            if (c_.systemLine && !line.empty()) c_.systemLine(line);
        };
        const double now = c_.time;
        if (r.operation == proto::AlchemyOperation::SocketMake || r.operation == proto::AlchemyOperation::SocketInsert ||
            r.operation == proto::AlchemyOperation::SocketErase) {
            SocketResult(r);
            return;
        }
        if (r.outcome == proto::AlchemyOutcome::Cancelled) {
            say(c_.Text("UIIT_MSG_ALCHEMY_CANCELED_COMPOUND"));
            Reset(false);
            return;
        }
        if (r.outcome == proto::AlchemyOutcome::Refused) {
            say(c_.Text(ServerKey(r.code)));
            if (r.operation == proto::AlchemyOperation::Strengthen) PlayEffect(AlchemyEffect::Kind::Fail, now, "SND_ELIXIR_FAILURE");
            Reset(r.operation == proto::AlchemyOperation::Strengthen);
            return;
        }
        if (r.operation == proto::AlchemyOperation::Strengthen) {
            const game::ItemInstance* item = c_.state != nullptr ? c_.state->inventory.At(r.place) : nullptr;
            const assets::ItemRow* row = item != nullptr && c_.items ? c_.items->Find(item->refId) : nullptr;
            const std::string name = row != nullptr ? c_.Text(row->nameKey) : std::string();
            if (r.outcome == proto::AlchemyOutcome::Success) {
                PlayEffect(AlchemyEffect::Kind::Success, now, "SND_ELIXIR_SUCCESS");
                if (stoneKind_ == 0) {
                    say(FormatText(c_.Text("UIIT_MSG_REINFORCERR_SUCCESS"), {std::to_string(r.plusAfter)}));
                } else if (item != nullptr && row != nullptr) {
                    for (const std::string& line : StoneLines(*row, *item, r.apeUsed)) say(line);
                }
            } else if (r.outcome == proto::AlchemyOutcome::Destroyed) {
                PlayEffect(AlchemyEffect::Kind::Fail, now, "SND_ELIXIR_DESTROY");
                say(c_.Text("UIIT_MSG_REINFORCERR_BREAKDOWN"));
                slots_[0] = {};
            } else {
                PlayEffect(AlchemyEffect::Kind::Fail, now, "SND_ELIXIR_FAILURE");
                if (r.plusBefore > 0 && r.plusAfter == 0) say(c_.Text("UIIT_MSG_REINFORCERR_FAIL_RESULT_OPTLV_ZERO"));
                else if (r.plusAfter < r.plusBefore)
                    say(FormatText(c_.Text("UIIT_MSG_REINFORCERR_FAIL_RESULT_OPTLV_DOWN"), {std::to_string(r.plusAfter), std::to_string(r.plusBefore - r.plusAfter)}));
                else if (!r.durabilityLost) say(c_.Text("UIIT_MSG_REINFORCERR_FAIL"));
                if (r.durabilityLost) say(c_.Text("UIIT_MSG_REINFORCERR_FAIL"));
            }
            // The elixir and powder slots empty, the equipment stays.
            running_ = false;
            cancelRequested_ = false;
            for (int k = 1; k < 9; ++k) slots_[std::size_t(k)] = {};
            Refresh();
            return;
        }
        // Disjoint, Dismantle, Manufacture: the prepare effect then success, the 0.5 s gauge.
        PlayEffect(AlchemyEffect::Kind::Prepare, now, "SND_ELIXIR_USE");
        successAt_ = now + 0.5;
        StartGauge(now, 0.5);
        if (r.operation == proto::AlchemyOperation::Manufacture) say(c_.Text("UIIT_MSG_ENCHANT_SUCCEED"));
        if (r.operation != proto::AlchemyOperation::Disjoint || r.remaining == 0) finishAt_ = now + 0.5;
    }

    // The script's (the live tests'): a tab shown, the button pressed, the alert answered.
    void PickTab(int tab) { ShowTab(std::clamp(tab, 0, 3)); }
    void Press() { Act(); }
    bool Answer(bool yes) {
        if (socketAsking_ && c_.hud != nullptr) {
            socketAsking_ = false;
            return c_.hud->AnswerConfirm(yes);
        }
        if (alert_ == nullptr || !alert_->visible) return false;
        alert_->visible = false;
        if (alert_->answer) alert_->answer(yes);
        return true;
    }
    // The Socket page's (the scripts' and the tests'): shown, Make then a click on socket `index`, Remove then a click on
    // it, a stone dropped on it, the insertion's Cancel.
    void ShowSocket(bool socket) { ShowPage(socket); }
    void SocketMakeAt(int index) {
        if (pickMode_ != 1) SocketMakePressed();
        if (pickMode_ == 1) SocketPicked(index);
    }
    void SocketRemoveAt(int index) {
        if (pickMode_ != 2) SocketRemovePressed();
        if (pickMode_ == 2) SocketPicked(index);
    }
    void SocketStoneAt(int stonePlace, int index) { SocketDropStone(stonePlace, index); }
    void SocketCancel() {
        if (socketRun_.active && socketRun_.op == proto::AlchemyOperation::SocketInsert) SocketRemovePressed();
    }
    int SocketPickMode() const { return pickMode_; }

    // Closed: what runs is cancelled.
    void Closing() {
        if (running_ && !cancelRequested_) SendCancel();
        if (alert_ != nullptr) alert_->visible = false;
        SocketReset(true);
    }

    void Update(ui::Frame& frame) override {
        ui::MainFrame::Update(frame);
        const double now = frame.time;
        // The gauge.
        if (auto* gauge = Gauge()) gauge->SetValue(gaugeStart_ >= 0.0 ? float(std::clamp((now - gaugeStart_) / gaugeSeconds_, 0.0, 1.0)) : 0.0f, true);
        if (successAt_ > 0.0 && now >= successAt_) {
            successAt_ = 0.0;
            PlayEffect(AlchemyEffect::Kind::Success, now, "SND_ELIXIR_SUCCESS");
        }
        if (finishAt_ > 0.0 && now >= finishAt_) {
            finishAt_ = 0.0;
            running_ = false;
            gaugeStart_ = -1.0;
            Revalidate();
        }
        // The prepare loop replays its sound each round while the answer is awaited.
        if (effect_ != nullptr && effect_->kind == AlchemyEffect::Kind::Prepare && running_ && g_tab == kStrengthen) {
            const int round = int((now - effect_->started) / 1.6);
            if (round != prepareRound_) {
                prepareRound_ = round;
                if (round > 0 && ui_ != nullptr) ui_->Sound("SND_ELIXIR_USE");
            }
        }
        if (effect_ != nullptr && effect_->kind != AlchemyEffect::Kind::Prepare && effect_->FrameAt(now) < 0) effect_->Play(AlchemyEffect::Kind::None, now);
        // The page buttons' and the tabs' tooltips (Create's: "Socket Alchemy", "Alchemy", the tabs' names).
        if (const ui::Widget* over = frame.pointer.hovered; over != nullptr) {
            const char* key = nullptr;
            if (over->parent() == this && over->id() == kSocketButton) key = "UIIT_STT_SOCKET_TAP";
            if (over->parent() == this && over->id() == kAlchemyButton) key = "SN_TAB_ALCHEMY";
            for (int t = 0; t < 4; ++t) {
                if (over->parent() == alchemyPage_ && over->id() == kTabIds[t]) key = kTabKeys[t];
            }
            if (key != nullptr) frame.interface.ShowTooltip(c_.Text(key), over->ScreenRect());
        }
        // The slots follow the bag (a stack used up empties its slot).
        Revalidate();
        Refresh();
        SocketUpdate(frame);
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.source == nullptr) return ui::MainFrame::OnNotice(n);
        if (n.kind == ui::Notice::Kind::RightClicked && socketPage_ != nullptr && n.source->parent() == socketPage_ && n.source->id() == kSocketTarget) {
            // The item taken out of the Socket page.
            if (!socketRun_.active) {
                socketItem_ = {};
                pickMode_ = 0;
                RefreshSocket();
            }
            return true;
        }
        if (n.kind == ui::Notice::Kind::RightClicked) {
            const int key = KeyOf(n.source);
            if (key >= 0 && !running_) {
                slots_[std::size_t(key)] = {};
                if (key == 0 && (g_tab == kStrengthen || g_tab == kManufacture)) slots_.fill({});
                Refresh();
                return true;
            }
        }
        if (n.kind != ui::Notice::Kind::Clicked) return ui::MainFrame::OnNotice(n);
        const int id = n.source->id();
        if (n.source->parent() == this && id == 2 && ui::As<ui::Button>(n.source) != nullptr) {
            Closing(); // the close button
            return ui::MainFrame::OnNotice(n);
        }
        if (n.source->parent() == this && (id == kAlchemyButton || id == kSocketButton)) {
            ShowPage(id == kSocketButton);
            return true;
        }
        if (socketPage_ != nullptr && n.source->parent() == socketPage_) {
            if (id == kSocketMake) {
                SocketMakePressed();
                return true;
            }
            if (id == kSocketRemove) {
                SocketRemovePressed();
                return true;
            }
        }
        if (n.source->parent() == alchemyPage_) {
            for (int t = 0; t < 4; ++t) {
                if (id == kTabIds[t]) {
                    if (running_ && !cancelRequested_) SendCancel();
                    ShowTab(t);
                    return true;
                }
            }
            if (id == kAction) {
                Act();
                return true;
            }
        }
        return ui::MainFrame::OnNotice(n);
    }

private:
    struct Placed {
        int place = -1;
        std::int64_t serial = 0;
    };

    // --- The Socket page (72) ---------------------------------------------------------------

    ui::Slot* SocketSlot(int k) const {
        if (socketPage_ == nullptr) return nullptr;
        return ui::As<ui::Slot>(socketPage_->Child(k < 0 ? kSocketTarget : kSocketSlots[std::size_t(k)]));
    }
    ui::Widget* SocketMark(int k) const { return socketPage_ != nullptr ? socketPage_->Child(kSocketMarks[std::size_t(k)]) : nullptr; }
    const game::ItemInstance* SocketItem() const {
        if (socketItem_.place < 0 || c_.state == nullptr) return nullptr;
        const game::ItemInstance* item = c_.state->inventory.At(std::uint8_t(socketItem_.place));
        return item != nullptr && item->serial == socketItem_.serial ? item : nullptr;
    }
    const assets::ItemRow* SocketRow() const {
        const game::ItemInstance* item = SocketItem();
        return item != nullptr && c_.items ? c_.items->Find(item->refId) : nullptr;
    }
    // How many of a kind the bag holds (the socket maker, the Destruction Rondo).
    int HeldKind(bool (*kind)(const assets::ItemRow&)) const {
        if (c_.state == nullptr || c_.items == nullptr) return 0;
        int n = 0;
        for (const auto& [place, item] : c_.state->inventory.places) {
            if (place < game::slot::kWorn) continue;
            if (const assets::ItemRow* row = c_.items->Find(item.refId); row != nullptr && kind(*row)) n += item.count;
        }
        return n;
    }
    void Say(const std::string& line) const {
        if (c_.systemLine && !line.empty()) c_.systemLine(line);
    }

    void SocketCreated() {
        if (socketPage_ == nullptr) return;
        if (ui::Slot* target = SocketSlot(-1)) {
            target->pickable = false;
            target->tooltipLines = [this] {
                const game::ItemInstance* item = SocketItem();
                const assets::ItemRow* row = SocketRow();
                return item != nullptr && row != nullptr ? ItemTooltip(*row, *item, c_) : std::vector<ui::TooltipLine>{};
            };
        }
        // A filled socket shows its stone; its tooltip is its skill's.
        for (int k = 0; k < game::kSockets; ++k) {
            ui::Slot* slot = SocketSlot(k);
            if (slot == nullptr) continue;
            slot->pickable = false;
            slot->tooltipSigned = false;
            slot->tooltipLines = [this, k] {
                const game::ItemInstance* item = SocketItem();
                const game::ItemSocket* s = item != nullptr ? &item->sockets[std::size_t(k)] : nullptr;
                const assets::SkillRow* skill = s != nullptr && s->Filled() && c_.skills != nullptr ? c_.skills->Find(s->skill) : nullptr;
                return skill != nullptr ? SkillTooltip(*skill, c_) : std::vector<ui::TooltipLine>{};
            };
        }
        assets::UiLayoutNode fx;
        fx.type = "CIFStatic";
        fx.id = 1005;
        fx.rect = {0, 0, 128, 128};
        socketFx_ = static_cast<SocketEffect*>(socketPage_->Add(std::make_unique<SocketEffect>(fx)));
        socketFx_->visible = false;
        RefreshSocket();
    }

    // The Alchemy page or the Socket page; leaving the Socket page ends its pick mode and cancels an insertion.
    void ShowPage(bool socket) {
        if (!socket) SocketReset(true);
        if (alchemyPage_ != nullptr) alchemyPage_->visible = !socket;
        if (socketPage_ != nullptr) socketPage_->visible = socket;
        RefreshSocket();
    }

    void SocketReset(bool cancel) {
        if (cancel && socketRun_.active && socketRun_.op == proto::AlchemyOperation::SocketInsert && socketRun_.sent && !socketCancel_) {
            Send(proto::AlchemyCancel::kId, proto::Encode(proto::AlchemyCancel{}));
        }
        socketRun_ = {};
        socketCancel_ = false;
        pickMode_ = 0;
        c_.cursor = 0;
        if (socketFx_ != nullptr) {
            socketFx_->art.clear();
            socketFx_->visible = false;
        }
    }

    // An item put in: armour or a weapon of degree 11 or more with socket places (1.188's page;
    // 1.7). A socket stone goes in only dropped on a socket (the original's); put in so it is no socket material.
    void SocketPut(int place) {
        if (socketRun_.active) return;
        const game::ItemInstance* item = c_.state != nullptr ? c_.state->inventory.At(std::uint8_t(place)) : nullptr;
        const assets::ItemRow* row = item != nullptr && c_.items ? c_.items->Find(item->refId) : nullptr;
        if (row == nullptr) return;
        const int tid3 = row->typeId[2];
        const bool weapon = tid3 == 6, armour = tid3 == 1 || tid3 == 2 || tid3 == 3 || tid3 == 9 || tid3 == 10 || tid3 == 11;
        if (alchemy::KindOf(*row) != alchemy::Kind::Equipment || !(weapon || armour)) {
            Say(c_.Text("UIIT_STT_SOCKETERR_ITS_NOT_SOCKET_STUFF"));
            return;
        }
        if (alchemy::Degree(*row) < 11 || alchemy::SocketCount(*row) <= 0) {
            Say(c_.Text("UIIT_MSG_SOCKET_LEVEL_MISMATCH"));
            return;
        }
        socketItem_ = {place, item->serial};
        pickMode_ = 0;
        RefreshSocket();
    }

    // Make (56): pick mode 1 (an unmade socket), enabled with one not made and a maker in the bag; again: off.
    void SocketMakePressed() {
        if (socketRun_.active) return;
        const game::ItemInstance* item = SocketItem();
        const assets::ItemRow* row = SocketRow();
        if (item == nullptr || row == nullptr) return;
        if (pickMode_ == 1) {
            pickMode_ = 0;
            return;
        }
        if (alchemy::MadeSockets(*item) >= alchemy::SocketCount(*row) || HeldKind(alchemy::IsSocketMaker) <= 0) return;
        pickMode_ = 1;
    }
    // Remove (57): pick mode 2 (a filled socket), enabled with one filled and a Destruction Rondo in the bag; while an
    // insertion runs, its Cancel.
    void SocketRemovePressed() {
        if (socketRun_.active) {
            if (socketRun_.op == proto::AlchemyOperation::SocketInsert && socketRun_.sent && !socketCancel_) {
                socketCancel_ = true;
                Send(proto::AlchemyCancel::kId, proto::Encode(proto::AlchemyCancel{}));
            }
            return;
        }
        const game::ItemInstance* item = SocketItem();
        if (item == nullptr) return;
        if (pickMode_ == 2) {
            pickMode_ = 0;
            return;
        }
        const bool filled = std::any_of(item->sockets.begin(), item->sockets.end(), [](const game::ItemSocket& s) { return s.Filled(); });
        if (!filled || HeldKind(alchemy::IsSocketEraser) <= 0) return;
        pickMode_ = 2;
    }

    // A socket clicked in a pick mode: making an unmade one (the gauge, then the request); erasing a filled one (asked
    // first, then the gauge with fx02, then the request).
    void SocketPicked(int index) {
        const game::ItemInstance* item = SocketItem();
        if (item == nullptr || index < 0 || index >= game::kSockets) return;
        const game::ItemSocket& s = item->sockets[std::size_t(index)];
        if (pickMode_ == 1) {
            if (s.made) return;
            pickMode_ = 0;
            StartSocketRun(proto::AlchemyOperation::SocketMake, index, kSocketPunchSeconds, false, {});
            return;
        }
        if (pickMode_ != 2 || !s.Filled()) return;
        pickMode_ = 0;
        const assets::ItemRow* stone = c_.items != nullptr ? c_.items->Find(s.stone) : nullptr;
        const std::string name = stone != nullptr ? c_.Text(stone->nameKey) : std::string();
        Ask(FormatText(c_.Text("UIIT_STT_SOCKET_OUT"), {name}), [this, index, name](bool yes) {
            if (!yes || SocketItem() == nullptr) return;
            StartSocketRun(proto::AlchemyOperation::SocketErase, index, kSocketPunchSeconds, false, name);
            PlaySocketFx(index, "interface\\alchemy\\alcm_socket_fx02.ddj");
        });
    }

    // A socket stone dropped on socket `index`: the client's checks (the kind, the socket made and empty, not the same
    // stone twice), the question, then the request at once and the 2-second gauge with fx01.
    void SocketDropStone(int stonePlace, int index) {
        if (socketRun_.active || index < 0 || index >= game::kSockets) return;
        pickMode_ = 0;
        const game::ItemInstance* item = SocketItem();
        const assets::ItemRow* row = SocketRow();
        if (item == nullptr || row == nullptr) {
            Say(c_.Text("UIIT_MSG_ENCHANT_LOAD_EQUIP_FIRST"));
            return;
        }
        const game::ItemInstance* stone = c_.state != nullptr ? c_.state->inventory.At(std::uint8_t(stonePlace)) : nullptr;
        const assets::ItemRow* stoneRow = stone != nullptr && c_.items ? c_.items->Find(stone->refId) : nullptr;
        if (stoneRow == nullptr || !alchemy::IsSocketStone(*stoneRow)) {
            Say(c_.Text("UIIT_STT_SOCKETERR_ITS_NOT_SOCKET_STUFF"));
            return;
        }
        const int tid3 = row->typeId[2];
        const bool chest = (tid3 == 1 || tid3 == 2 || tid3 == 3 || tid3 == 9 || tid3 == 10 || tid3 == 11) && row->typeId[3] == 3;
        const bool fits = stoneRow->params[1] == 2 ? chest : stoneRow->params[1] == 1 && tid3 == 6;
        if (!fits || alchemy::SocketStoneDisabled(*stoneRow)) { // ours: the DMRETURN stone kept out
            Say(c_.Text("UIIT_STT_SOCKETERR_THE_ITEM_ISNT_PARING_THIS_SOCKET_STONE"));
            return;
        }
        const game::ItemSocket& s = item->sockets[std::size_t(index)];
        if (!s.made) {
            Say(c_.Text("UIIT_STT_SOCKETERR_THERE_IS_NO_EMPTY_HOLE"));
            return;
        }
        if (s.Filled()) {
            Say(c_.Text("UIIT_STT_SOCKETERR_THIS_HOLE_USED_ALREADY"));
            return;
        }
        for (const game::ItemSocket& other : item->sockets) {
            if (other.Filled() && other.stone == stoneRow->refObjId) {
                Say(c_.Text("UIIT_STT_SOCKETERR_SAME_SOCKET_STONE_IS_INSERTED"));
                return;
            }
        }
        const std::string name = c_.Text(stoneRow->nameKey);
        const std::uint8_t target = std::uint8_t(socketItem_.place), at = std::uint8_t(stonePlace), i = std::uint8_t(index);
        Ask(FormatText(c_.Text("UIIT_STT_SOCKET_IN"), {name}), [this, target, at, i, name](bool yes) {
            if (!yes || SocketItem() == nullptr || socketRun_.active) return;
            Send(proto::SocketInsert::kId, proto::Encode(proto::SocketInsert{target, at, i}));
            StartSocketRun(proto::AlchemyOperation::SocketInsert, i, kSocketInsertSeconds, true, name);
            PlaySocketFx(i, "interface\\alchemy\\alcm_socket_fx01.ddj");
        });
    }

    // The confirm box (CIFMessageBox) for the socket page's questions.
    void Ask(const std::string& text, std::function<void(bool)> answer) {
        if (c_.hud == nullptr) {
            answer(true);
            return;
        }
        socketAsking_ = true;
        c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {text}, [this, answer = std::move(answer)](bool yes) {
            socketAsking_ = false;
            answer(yes);
        });
    }

    void StartSocketRun(proto::AlchemyOperation op, int index, double seconds, bool sent, std::string stone) {
        socketRun_.active = true;
        socketRun_.op = op;
        socketRun_.index = index;
        socketRun_.start = c_.time;
        socketRun_.seconds = seconds;
        socketRun_.sent = sent;
        socketRun_.stone = std::move(stone);
        socketCancel_ = false;
        RefreshSocket();
    }

    void PlaySocketFx(int index, const char* art) {
        ui::Slot* slot = SocketSlot(index);
        if (socketFx_ == nullptr || slot == nullptr || socketPage_ == nullptr) return;
        const ui::Rect s = slot->ScreenRect(), page = socketPage_->ScreenRect();
        socketFx_->rect = {s.x - page.x + s.w / 2.0f - 64.0f, s.y - page.y + s.h / 2.0f - 64.0f, 128.0f, 128.0f};
        socketFx_->Play(art, c_.time);
    }

    // The server's answer to a socket request (the bag's places came just before it).
    void SocketResult(const proto::AlchemyResult& r) {
        std::string stone = socketRun_.stone;
        if (r.stone != 0 && c_.items != nullptr) {
            if (const assets::ItemRow* row = c_.items->Find(r.stone)) stone = c_.Text(row->nameKey);
        }
        socketRun_ = {};
        socketCancel_ = false;
        if (r.outcome == proto::AlchemyOutcome::Cancelled) {
            Say(c_.Text("UIIT_MSG_ALCHEMY_CANCELED_COMPOUND"));
        } else if (r.outcome != proto::AlchemyOutcome::Success) {
            Say(c_.Text(SocketKey(r.code)));
        } else if (r.operation == proto::AlchemyOperation::SocketMake) {
            if (ui_ != nullptr) ui_->Sound("SND_SOCKET_PUNCH");
        } else if (r.operation == proto::AlchemyOperation::SocketInsert) {
            if (ui_ != nullptr) ui_->Sound("SND_SOCKET_SETIN");
            Say(FormatText(c_.Text("UIIT_MSG_SOCKET_ALCHEMY_SUCCESS"), {stone, std::to_string(r.value)}));
        } else {
            Say(FormatText(c_.Text("UIIT_MSG_SOCKET_DELETE_SUCCESS"), {stone}));
            if (ui_ != nullptr) ui_->Sound("SND_SOCKET_REMOVE");
        }
        RefreshSocket();
    }

    void SocketUpdate(ui::Frame& frame) {
        if (socketPage_ == nullptr) return;
        const double now = frame.time;
        // A run: its gauge; making and erasing send when theirs is full (400 ms).
        double fill = 0.0;
        if (socketRun_.active) {
            fill = std::clamp((now - socketRun_.start) / std::max(0.001, socketRun_.seconds), 0.0, 1.0);
            if (!socketRun_.sent && fill >= 1.0 && socketItem_.place >= 0) {
                socketRun_.sent = true;
                const std::uint8_t target = std::uint8_t(socketItem_.place), index = std::uint8_t(socketRun_.index);
                if (socketRun_.op == proto::AlchemyOperation::SocketMake) Send(proto::SocketMake::kId, proto::Encode(proto::SocketMake{target, index}));
                else Send(proto::SocketErase::kId, proto::Encode(proto::SocketErase{target, index}));
            }
        }
        if (auto* gauge = ui::As<ui::Gauge>(socketPage_->Child(kSocketGauge))) gauge->SetValue(float(fill), true);
        // The item gone or moved (not while a run waits for its answer): out of the page.
        if (!socketRun_.active && socketItem_.place >= 0 && SocketItem() == nullptr) {
            socketItem_ = {};
            pickMode_ = 0;
        }
        // A pick mode's click on a socket; its cursor while the page shows.
        if (pickMode_ != 0 && socketPage_->ShownOnScreen() && frame.input.Released(ui::MouseButton::Left)) {
            for (int k = 0; k < game::kSockets; ++k) {
                const ui::Slot* slot = SocketSlot(k);
                if (slot != nullptr && slot->ScreenRect().Contains(frame.pointer.position)) {
                    SocketPicked(k);
                    break;
                }
            }
        }
        c_.cursor = socketPage_->ShownOnScreen() ? (pickMode_ == 1 ? kCursorMake : pickMode_ == 2 ? kCursorRemove : 0) : 0;
        // The buttons' help texts (UIIT_STT_SOCKET_BUTTON_SOCKET_MAKE, _REMOVE).
        if (const ui::Widget* over = frame.pointer.hovered; over != nullptr && over->parent() == socketPage_) {
            if (over->id() == kSocketMake) frame.interface.ShowTooltip(c_.Text("UIIT_STT_SOCKET_BUTTON_SOCKET_MAKE"), over->ScreenRect());
            if (over->id() == kSocketRemove) frame.interface.ShowTooltip(c_.Text("UIIT_STT_SOCKET_BUTTON_SOCKET_REMOVE"), over->ScreenRect());
        }
        RefreshSocket();
    }

    // The page as it is now: the item, its name in its colour (SoX gold, blue items blue, else white), the sockets
    // (closed marks over the unmade ones, a filled one's stone), the buttons, the help line by mode.
    void RefreshSocket() {
        if (socketPage_ == nullptr) return;
        const game::ItemInstance* item = SocketItem();
        const assets::ItemRow* row = SocketRow();
        if (ui::Slot* target = SocketSlot(-1)) {
            if (item != nullptr && row != nullptr) {
                target->icon = row->iconPath.empty() ? std::string("icon\\icon_default.ddj") : "icon\\" + row->iconPath;
                target->rare = row->Rare();
            } else {
                target->icon.clear();
                target->rare = false;
            }
        }
        if (ui::Widget* name = socketPage_->Child(kSocketName)) {
            std::string text;
            if (item != nullptr && row != nullptr) {
                text = c_.Text(row->nameKey);
                if (game::TotalPlus(*item) > 0) text += " +" + std::to_string(game::TotalPlus(*item));
            }
            name->SetText(text);
            if (auto* s = ui::As<ui::Static>(name); s != nullptr && row != nullptr) {
                s->textColor = row->Rare() ? ui::Argb(0xFFFFD953) : !item->options.empty() ? ui::Argb(0xFF00EAFF) : ui::Argb(0xFFFFFFFF);
            }
        }
        for (int k = 0; k < game::kSockets; ++k) {
            const game::ItemSocket* s = item != nullptr ? &item->sockets[std::size_t(k)] : nullptr;
            if (ui::Widget* mark = SocketMark(k)) mark->visible = s == nullptr || !s->made;
            ui::Slot* slot = SocketSlot(k);
            if (slot == nullptr) continue;
            const assets::ItemRow* stone = s != nullptr && s->Filled() && c_.items != nullptr ? c_.items->Find(s->stone) : nullptr;
            slot->icon = stone != nullptr ? (stone->iconPath.empty() ? std::string("icon\\icon_default.ddj") : "icon\\" + stone->iconPath) : std::string();
        }
        const bool insertion = socketRun_.active && socketRun_.op == proto::AlchemyOperation::SocketInsert;
        if (auto* make = ui::As<ui::Button>(socketPage_->Child(kSocketMake))) {
            make->SetText(c_.Text("UIIT_CTL_SOCKET_BUTTON_MAKE"));
            make->enabled = !socketRun_.active && item != nullptr && row != nullptr && alchemy::MadeSockets(*item) < alchemy::SocketCount(*row) &&
                            HeldKind(alchemy::IsSocketMaker) > 0;
        }
        if (auto* remove = ui::As<ui::Button>(socketPage_->Child(kSocketRemove))) {
            remove->SetText(c_.Text(insertion ? "UIIT_CTL_ALCHEMYBOX_BUTTON_DISMANTLING_CANCEL" : "UIIT_CTL_SOCKET_BUTTON_REMOVE"));
            const bool filled = item != nullptr && std::any_of(item->sockets.begin(), item->sockets.end(), [](const game::ItemSocket& s) { return s.Filled(); });
            remove->enabled = insertion ? socketRun_.sent && !socketCancel_ : !socketRun_.active && filled && HeldKind(alchemy::IsSocketEraser) > 0;
        }
        // The help line by mode (which key a mode shows is ours: the keys' names and texts differ).
        if (ui::Widget* help = socketPage_->Child(kSocketHelp)) {
            const char* key = pickMode_ == 1    ? "UIIT_STT_SOCKET_HELP_SOCKET_ADD"
                              : pickMode_ == 2  ? "UIIT_STT_SOCKET_HELP_SOCKET_MAKE"
                              : item != nullptr ? "UIIT_STT_SOCKET_HELP_SOCKET_REMOVE"
                                                : "UIIT_STT_SOCKET_HELP_SOCKET_DEFAULT";
            help->SetText(c_.Text(key));
        }
    }

    Placed socketItem_;
    int pickMode_ = 0; // 1 making (cursor 174), 2 removing (cursor 175)
    struct SocketRun {
        bool active = false;
        proto::AlchemyOperation op = proto::AlchemyOperation::SocketMake;
        int index = -1;
        double start = 0.0, seconds = 0.0;
        bool sent = false; // the request went (an insertion's at once, making's and erasing's after their gauge)
        std::string stone; // its stone's name (the result's line)
    } socketRun_;
    bool socketCancel_ = false, socketAsking_ = false;
    SocketEffect* socketFx_ = nullptr;

    const game::ItemInstance* ItemAt(int key) const {
        const Placed& p = slots_[std::size_t(key)];
        if (p.place < 0 || c_.state == nullptr) return nullptr;
        const game::ItemInstance* item = c_.state->inventory.At(std::uint8_t(p.place));
        return item != nullptr && item->serial == p.serial ? item : nullptr;
    }
    const assets::ItemRow* RowAt(int key) const {
        const game::ItemInstance* item = ItemAt(key);
        return item != nullptr && c_.items ? c_.items->Find(item->refId) : nullptr;
    }
    ui::Widget* Page() const { return g_tab == kStrengthen || g_tab == kManufacture ? type1_ : type2_; }
    const int* SlotIds() const { return g_tab == kStrengthen || g_tab == kManufacture ? kType1Slots : kType2Slots; }
    int SlotCount() const { return g_tab == kStrengthen || g_tab == kManufacture ? 5 : 9; }
    ui::Gauge* Gauge() const { return Page() != nullptr ? ui::As<ui::Gauge>(Page()->Child(kGauge)) : nullptr; }
    int KeyOf(const ui::Widget* slot) const {
        if (slot == nullptr || slot->parent() != Page()) return -1;
        for (int k = 0; k < SlotCount(); ++k) {
            if (SlotIds()[k] == slot->id()) return k;
        }
        return -1;
    }
    // How many of an item the bag holds (the Rondos).
    int Held(const char* code) const {
        const assets::ItemRow* row = c_.items ? c_.items->Find(std::string_view(code)) : nullptr;
        if (row == nullptr || c_.state == nullptr) return 0;
        int n = 0;
        for (const auto& [place, item] : c_.state->inventory.places) {
            if (place >= game::slot::kWorn && item.refId == row->refObjId) n += item.count;
        }
        return n;
    }

    // The tab's checks (and its siblings): 0 and the slot, or the refusal's code.
    int Accepts(const assets::ItemRow& row, const game::ItemInstance& item, int& key) const {
        const alchemy::Kind kind = alchemy::KindOf(row);
        switch (g_tab) {
        case kDisjoint:
            if (kind != alchemy::Kind::ByProduct) return 3;
            for (int k = 1; k < 9; ++k) {
                if (slots_[std::size_t(k)].place < 0) {
                    key = k;
                    return 0;
                }
            }
            return 100;
        case kDismantle:
            if (kind != alchemy::Kind::Equipment) return 5;
            key = 1;
            return 0;
        case kManufacture: {
            if (kind == alchemy::Kind::Tablet) {
                key = 0;
                return 0;
            }
            const assets::ItemRow* tablet = RowAt(0);
            if (tablet == nullptr) return 6;
            alchemy::Recipe recipe;
            if (kind != alchemy::Kind::Element || c_.items == nullptr || !alchemy::TabletRecipe(*c_.items, *tablet, recipe)) return 7;
            for (std::size_t i = 0; i < recipe.elements.size() && i < 4; ++i) {
                if (recipe.elements[i].first->refObjId == row.refObjId) {
                    key = int(i) + 1;
                    return 0;
                }
            }
            return 7;
        }
        default: break;
        }
        // Strengthen.
        if (kind == alchemy::Kind::Equipment) {
            key = 0;
            return 0;
        }
        const assets::ItemRow* target = RowAt(0);
        if (target == nullptr) return 8;
        if (kind == alchemy::Kind::Elixir) {
            if (!alchemy::ElixirFits(row, *target)) return 10;
            key = 1;
            return 0;
        }
        if (kind == alchemy::Kind::AdvancedElixir) {
            if (alchemy::Degree(*target) > row.itemClass) return 16;
            if (!alchemy::ElixirFits(row, *target)) return 10;
            if (const game::ItemInstance* equipment = ItemAt(0); equipment != nullptr && equipment->advanced != 0) return 17;
            if ((target->rarity == 2 || target->rarity == 6) != (row.rarity == 2 || row.rarity == 6)) return 10;
            key = 1;
            return 0;
        }
        if (kind == alchemy::Kind::MagicStone || kind == alchemy::Kind::AttrStone) {
            if (row.params[0] != alchemy::Degree(*target)) return 12;
            if (kind == alchemy::Kind::AttrStone && alchemy::CheckAttrStone(*target, row) != 0) return 13;
            if (kind == alchemy::Kind::MagicStone && c_.magicOptions != nullptr &&
                !alchemy::OptionFits(*c_.magicOptions, row.paramTexts[0], *target)) {
                return 13;
            }
            key = 1;
            return 0;
        }
        if (kind == alchemy::Kind::LuckyPowder) {
            const assets::ItemRow* elixir = RowAt(1);
            if (elixir == nullptr || alchemy::KindOf(*elixir) != alchemy::Kind::Elixir) return 9;
            if (row.params[0] != alchemy::Degree(*target)) return 11;
            key = 2;
            return 0;
        }
        (void)item;
        return 9;
    }

    void ShowTab(int tab) {
        g_tab = tab;
        Reset(false);
        if (alchemyPage_ == nullptr) return;
        for (int t = 0; t < 4; ++t) {
            if (auto* b = ui::As<ui::Button>(alchemyPage_->Child(kTabIds[t]))) b->texture = Variant(b->node().ddj, t == tab ? "_on" : "_off");
        }
        if (type1_ != nullptr) type1_->visible = tab == kStrengthen || tab == kManufacture;
        if (type2_ != nullptr) type2_->visible = tab == kDisjoint || tab == kDismantle;
        Refresh();
    }

    // The tab back to its start (keep: the equipment stays, as after a strengthening).
    void Reset(bool keepEquipment) {
        const Placed kept = slots_[0];
        slots_.fill({});
        if (keepEquipment) slots_[0] = kept;
        running_ = false;
        cancelRequested_ = false;
        gaugeStart_ = -1.0;
        successAt_ = finishAt_ = 0.0;
        if (effect_ != nullptr && effect_->kind == AlchemyEffect::Kind::Prepare) effect_->Play(AlchemyEffect::Kind::None, c_.time);
    }

    // The slots matched to the bag again: an item gone or moved leaves its slot.
    void Revalidate() {
        for (int k = 0; k < 9; ++k) {
            if (slots_[std::size_t(k)].place >= 0 && ItemAt(k) == nullptr && !running_) slots_[std::size_t(k)] = {};
        }
    }

    void StartGauge(double now, double seconds) {
        gaugeStart_ = now;
        gaugeSeconds_ = seconds;
    }

    void PlayEffect(AlchemyEffect::Kind kind, double now, const char* sound) {
        if (effect_ == nullptr || Page() == nullptr) return;
        // Over its anchor slot: Disjoint the first filled material, Dismantle key 1, the others key 0.
        int key = g_tab == kDismantle ? 1 : 0;
        if (g_tab == kDisjoint) {
            for (int k = 1; k < 9; ++k) {
                if (slots_[std::size_t(k)].place >= 0) {
                    key = k;
                    break;
                }
            }
        }
        if (ui::Widget* slot = Page()->Child(SlotIds()[key])) {
            const ui::Rect s = slot->ScreenRect(), page = alchemyPage_->ScreenRect();
            effect_->rect = {s.x - page.x - 32.0f, s.y - page.y - 32.0f, 96.0f, 96.0f};
        }
        effect_->Play(kind, now);
        prepareRound_ = 0;
        if (ui_ != nullptr && sound != nullptr) ui_->Sound(sound);
    }

    void Send(proto::MessageId id, std::vector<std::byte> payload) {
        if (c_.alchemyRequest) c_.alchemyRequest(id, std::move(payload));
    }
    void SendCancel() {
        cancelRequested_ = true;
        Send(proto::AlchemyCancel::kId, proto::Encode(proto::AlchemyCancel{}));
    }

    // The action button.
    void Act() {
        const double now = c_.time;
        if (running_) {
            if (!cancelRequested_ && (g_tab == kStrengthen || g_tab == kDisjoint)) SendCancel();
            return;
        }
        if (!Ready()) return;
        const auto place = [&](int key) { return std::uint8_t(std::max(0, slots_[std::size_t(key)].place)); };
        switch (g_tab) {
        case kStrengthen:
            if (const assets::ItemRow* material = RowAt(1); material != nullptr && alchemy::KindOf(*material) == alchemy::Kind::AdvancedElixir) {
                stoneKind_ = 0;
                Ask(false);
                break;
            }
            stoneKind_ = 0;
            if (const assets::ItemRow* material = RowAt(1); material != nullptr && ItemAt(0) != nullptr) {
                const alchemy::Kind kind = alchemy::KindOf(*material);
                stoneKind_ = kind == alchemy::Kind::MagicStone ? 1 : kind == alchemy::Kind::AttrStone ? 2 : 0;
                stoneOption_ = material->paramTexts[0];
                before_ = *ItemAt(0);
            }
            Send(proto::AlchemyStrengthen::kId, proto::Encode(proto::AlchemyStrengthen{place(0), place(1), slots_[2].place >= 0 ? place(2) : std::uint8_t(0)}));
            running_ = true;
            StartGauge(now, 3.0);
            PlayEffect(AlchemyEffect::Kind::Prepare, now, "SND_ELIXIR_USE");
            break;
        case kManufacture: {
            proto::AlchemyManufacture m;
            m.tablet = place(0);
            for (int k = 1; k < 5; ++k) {
                if (slots_[std::size_t(k)].place >= 0) m.elements.push_back(place(k));
            }
            Send(proto::AlchemyManufacture::kId, proto::Encode(m));
            running_ = true;
            break;
        }
        case kDisjoint: {
            proto::AlchemyDisjoint m;
            for (int k = 1; k < 9; ++k) {
                if (slots_[std::size_t(k)].place >= 0) m.materials.push_back(place(k));
            }
            Send(proto::AlchemyDisjoint::kId, proto::Encode(m));
            running_ = true;
            break;
        }
        case kDismantle: Ask(true); break;
        default: break;
        }
    }

    // The alert box (172): before dismantling (mode 1) and before an advanced elixir (mode 3).
    void Ask(bool dismantle) {
        if (ui_ == nullptr) return;
        const int key = dismantle ? 1 : 0;
        const game::ItemInstance* item = ItemAt(key);
        const assets::ItemRow* row = RowAt(key);
        if (item == nullptr || row == nullptr) return;
        if (alert_ == nullptr) {
            const auto node = RootNode2dt(c_, kAlertLayout, "CNIFMiniConfirm", hud::kAlchemyAlert);
            if (!node) return;
            assets::UiLayoutNode n = *node;
            const ui::Rect me = ScreenRect();
            n.rect = {me.x + 50.0f, me.y + 50.0f, n.rect.w, n.rect.h};
            alert_ = dynamic_cast<AlchemyAlert*>(ui_->Open(n));
            if (alert_ == nullptr) return;
        } else {
            alert_->visible = true;
            ui_->Raise(alert_);
        }
        std::string name = c_.Text(row->nameKey);
        if (game::TotalPlus(*item) > 0) name += " +" + std::to_string(game::TotalPlus(*item));
        const std::string icon = row->iconPath.empty() ? std::string() : "icon\\" + row->iconPath;
        if (dismantle) {
            alert_->Fill(c_.Text("UIIT_PAG_ALCHEMYBOX_TITLE_DISMANTLING"), icon, name + "\n" + c_.Text("UIIT_STT_ALCHEMYBOX_NOTICE_DISMANTLING"),
                         c_.Text("UIIT_CTL_ALCHEMYBOX_BUTTON_DISMANTLING_OK"), c_.Text("UIIT_CTL_ALCHEMYBOX_BUTTON_DISMANTLING_CANCEL"));
        } else {
            alert_->Fill(c_.Text("UIIT_PAG_ALCHEMYBOX_TITLE_REINFORCE"), icon, name + "\n" + c_.Text("UIIT_STT_SPECIAL_ENCHANT_WARNING"),
                         c_.Text("UIIT_CTL_ALCHEMYBOX_BUTTON_STRENGTHENING"), c_.Text("UIIT_CTL_ALCHEMYBOX_BUTTON_DISMANTLING_CANCEL"));
        }
        const std::uint8_t at = std::uint8_t(slots_[std::size_t(key)].place), recipe = std::uint8_t(std::max(0, slots_[1].place));
        alert_->answer = [this, at, recipe, dismantle](bool yes) {
            if (!yes) {
                Reset(!dismantle);
                return;
            }
            running_ = true;
            if (dismantle) {
                Send(proto::AlchemyDismantle::kId, proto::Encode(proto::AlchemyDismantle{at}));
                return;
            }
            // OK: the request, the effect, the gauge and the sound, as a strengthening (mode 3).
            Send(proto::AlchemyStrengthen::kId, proto::Encode(proto::AlchemyStrengthen{at, recipe, 0}));
            StartGauge(c_.time, 3.0);
            PlayEffect(AlchemyEffect::Kind::Prepare, c_.time, "SND_ELIXIR_USE");
        };
    }

    // Whether the button may start (1.188's 0xC8 per tab).
    bool Ready() const {
        switch (g_tab) {
        case kStrengthen: return ItemAt(0) != nullptr && ItemAt(1) != nullptr;
        case kManufacture: {
            const assets::ItemRow* tablet = RowAt(0);
            alchemy::Recipe recipe;
            if (tablet == nullptr || c_.items == nullptr || !alchemy::TabletRecipe(*c_.items, *tablet, recipe)) return false;
            for (std::size_t i = 0; i < recipe.elements.size() && i < 4; ++i) {
                const game::ItemInstance* e = ItemAt(int(i) + 1);
                if (e == nullptr || int(e->count) < recipe.elements[i].second) return false;
            }
            return true;
        }
        case kDisjoint: {
            int need = 0, placed = 0;
            for (int k = 1; k < 9; ++k) {
                const game::ItemInstance* m = ItemAt(k);
                const assets::ItemRow* row = RowAt(k);
                if (m == nullptr || row == nullptr) continue;
                ++placed;
                need += alchemy::VoidRondos(*row, m->count);
            }
            return placed > 0 && Held("ITEM_ETC_ARCHEMY_RONDO_01") >= need;
        }
        case kDismantle: {
            const assets::ItemRow* row = RowAt(1);
            return row != nullptr && Held("ITEM_ETC_ARCHEMY_RONDO_02") >= alchemy::DestroyerRondos(*row);
        }
        default: return false;
        }
    }

    // The slots, the button and the help text as they are now.
    void Refresh() {
        ui::Widget* page = Page();
        if (page == nullptr) return;
        const int count = SlotCount();
        for (int k = 0; k < count; ++k) {
            auto* slot = ui::As<ui::Slot>(page->Child(SlotIds()[k]));
            if (slot == nullptr) continue;
            const game::ItemInstance* item = ItemAt(k);
            const assets::ItemRow* row = RowAt(k);
            if (item != nullptr && row != nullptr) {
                slot->icon = row->iconPath.empty() ? std::string("icon\\icon_default.ddj") : "icon\\" + row->iconPath;
                slot->count = item->count;
                slot->countAlways = game::MaxStack(*row) > 1;
                slot->rare = row->Rare();
            } else {
                slot->icon = Open(k) ? std::string() : std::string(kClosed);
                slot->count = 0;
                slot->rare = false;
            }
        }
        if (auto* button = alchemyPage_ != nullptr ? ui::As<ui::Button>(alchemyPage_->Child(kAction)) : nullptr) {
            const char* key = g_tab == kDisjoint     ? "UIIT_CTL_ALCHEMYBOX_BUTTON_DISJOINTING_ALLSLOT"
                              : g_tab == kDismantle   ? "UIIT_CTL_ALCHEMYBOX_BUTTON_DISMANTLING"
                              : g_tab == kManufacture ? "UIIT_CTL_ALCHEMYBOX_BUTTON_MANUFACTURING"
                                                      : "UIIT_CTL_ALCHEMYBOX_BUTTON_STRENGTHENING";
            const bool cancel = running_ && (g_tab == kStrengthen || g_tab == kDisjoint);
            button->SetText(c_.Text(cancel ? "UIIT_CTL_ALCHEMYBOX_BUTTON_DISMANTLING_CANCEL" : key));
            button->enabled = cancel ? !cancelRequested_ : (!running_ && Ready());
        }
        if (ui::Widget* help = page->Child(kHelp)) {
            const std::string text = HelpText();
            if (text != helpShown_) {
                helpShown_ = text;
                help->SetText(text);
            }
        }
    }

    // A slot open to an item (a closed one shows skl_mastery_nothing).
    bool Open(int key) const {
        switch (g_tab) {
        case kStrengthen: return key == 0 || (key == 1 && ItemAt(0) != nullptr) || (key == 2 && RowAt(1) != nullptr && alchemy::KindOf(*RowAt(1)) == alchemy::Kind::Elixir);
        case kManufacture: return key == 0 || ItemAt(0) != nullptr;
        case kDismantle: return key == 1;
        default: return key >= 1;
        }
    }

    std::string HelpText() const {
        switch (g_tab) {
        case kDisjoint: {
            int need = 0, placed = 0;
            for (int k = 1; k < 9; ++k) {
                if (const game::ItemInstance* m = ItemAt(k); m != nullptr && RowAt(k) != nullptr) {
                    ++placed;
                    need += alchemy::VoidRondos(*RowAt(k), m->count);
                }
            }
            const int held = Held("ITEM_ETC_ARCHEMY_RONDO_01");
            if (placed > 0) return FormatText(c_.Text("UIIT_STT_ALCHEMYBOX_HELP_DISJOINTING_1"), {std::to_string(held), std::to_string(need)});
            return c_.Text(held > 0 ? "UIIT_STT_ALCHEMYBOX_HELP_DISJOINTING_2" : "UIIT_STT_ALCHEMYBOX_HELP_DISJOINTING_3");
        }
        case kDismantle: {
            const int held = Held("ITEM_ETC_ARCHEMY_RONDO_02");
            if (const assets::ItemRow* row = RowAt(1))
                return FormatText(c_.Text("UIIT_STT_ALCHEMYBOX_HELP_DISMANTLING_1"), {std::to_string(held), std::to_string(alchemy::DestroyerRondos(*row))});
            return c_.Text(held > 0 ? "UIIT_STT_ALCHEMYBOX_HELP_DISMANTLING_2" : "UIIT_STT_ALCHEMYBOX_HELP_DISMANTLING_3");
        }
        case kManufacture: {
            const assets::ItemRow* tablet = RowAt(0);
            alchemy::Recipe recipe;
            if (tablet == nullptr || c_.items == nullptr || !alchemy::TabletRecipe(*c_.items, *tablet, recipe)) return c_.Text("UIIT_STT_ALCHEMYBOX_HELP_MANUFACTURING_1");
            std::vector<std::string> args;
            for (std::size_t i = 0; i < 4; ++i) {
                args.push_back(i < recipe.elements.size() ? c_.Text(recipe.elements[i].first->nameKey) : std::string());
                args.push_back(i < recipe.elements.size() ? std::to_string(recipe.elements[i].second) : std::string());
            }
            return FormatText(c_.Text("UIIT_STT_ALCHEMYBOX_HELP_MANUFACTURING_2"), {args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]});
        }
        default: return c_.Text("UIIT_STT_ALCHEMYBOX_HELP_STRENGTHENING4");
        }
    }

    // A stone's result lines: the white stats (the stone's own field as a
    // change, held to the end; another that changed as assimilation), the other options assimilation changed (or the
    // APE charge that stopped it), then the stone's option, granted or changed. The first %s is the item's name.
    std::vector<std::string> StoneLines(const assets::ItemRow& row, const game::ItemInstance& after, bool apeUsed) const {
        std::vector<std::string> lines;
        const std::string item = c_.Text(row.nameKey), arrow = " -> ";
        const auto line = [&](const char* key, const std::string& name, const std::string& values) {
            lines.push_back(FormatText(c_.Text(key), {item, name, values}));
        };
        // 1. The white stats in the original's order.
        const game::ItemStats was = game::ComputeStats(row, before_, c_.magicOptions), now = game::ComputeStats(row, after, c_.magicOptions);
        const int tid3 = row.typeId[2];
        const bool weapon = tid3 == 6, shield = tid3 == 4, accessory = tid3 == 5 || tid3 == 12, armour = !weapon && !shield && !accessory;
        const auto whole = [](double v) { return std::to_string(int(std::lround(v))); };
        const auto tenth = [](double v) { return std::format("{:.1f}", v); };
        struct Stat {
            const char* field;
            const char* key;
            bool applies;
            std::string old, now;
        };
        const std::vector<Stat> stats = {
            {"DUR", "PARAM_DUR", true, whole(was.durability.high), whole(now.durability.high)},
            {"PA", "PARAM_PA", weapon, "(" + whole(was.phyAttack.low) + " ~ " + whole(was.phyAttack.high) + ")", "(" + whole(now.phyAttack.low) + " ~ " + whole(now.phyAttack.high) + ")"},
            {"PASTR", "PARAM_PHYSICAL_SPECIALIZE", weapon, "(" + tenth(was.phyReinforce.low) + " % ~ " + tenth(was.phyReinforce.high) + " %)",
             "(" + tenth(now.phyReinforce.low) + " % ~ " + tenth(now.phyReinforce.high) + " %)"},
            {"MA", "PARAM_MA", weapon, "(" + whole(was.magAttack.low) + " ~ " + whole(was.magAttack.high) + ")", "(" + whole(now.magAttack.low) + " ~ " + whole(now.magAttack.high) + ")"},
            {"MAINT", "PARAM_MAGICAL_SPECIALIZE", weapon, "(" + tenth(was.magReinforce.low) + " % ~ " + tenth(was.magReinforce.high) + " %)",
             "(" + tenth(now.magReinforce.low) + " % ~ " + tenth(now.magReinforce.high) + " %)"},
            {"CRITICAL", "PARAM_CRITICAL", weapon, whole(was.critical.low), whole(now.critical.low)},
            {"HR", "PARAM_HR", weapon, whole(was.attackRating.low), whole(now.attackRating.low)},
            {"PD", "PARAM_PD", armour || shield, tenth(was.phyDefence.low), tenth(now.phyDefence.low)},
            {"PDSTR", "PARAM_PHYSICAL_SPECIALIZE", armour || shield, tenth(was.phyReinforce.low), tenth(now.phyReinforce.low)},
            {"MD", "PARAM_MD", armour || shield, tenth(was.magDefence.low), tenth(now.magDefence.low)},
            {"MDINT", "PARAM_MAGICAL_SPECIALIZE", armour || shield, tenth(was.magReinforce.low), tenth(now.magReinforce.low)},
            {"ER", "PARAM_ER", armour, whole(was.parry.low), whole(now.parry.low)},
            {"BR", "PARAM_BLOCKING", shield, whole(was.blocking.low), whole(now.blocking.low)},
            {"PAR", "PARAM_PR", accessory, tenth(was.phyAbsorption.low), tenth(now.phyAbsorption.low)},
            {"MAR", "PARAM_MR", accessory, tenth(was.magAbsorption.low), tenth(now.magAbsorption.low)},
        };
        std::string held;
        for (const Stat& s : stats) {
            if (!s.applies) continue;
            if (stoneOption_ == std::string("NATTR_") + s.field) {
                held = FormatText(c_.Text("UIIT_MSG_ALCHEMY_CHANGE_CATTR"), {item, c_.Text(s.key), s.old + arrow + s.now});
            } else if (s.old != s.now && !(std::string_view(s.field) == "DUR" && stoneOption_ == "MATTR_DUR")) {
                line("UIIT_MSG_ALCHEMY_ATTR_ASSIMILATION", c_.Text(s.key), s.old + arrow + s.now);
            }
        }
        if (!held.empty()) lines.push_back(held);
        if (c_.magicOptions == nullptr) return lines;
        const auto valueOf = [](const game::ItemInstance& i, std::uint32_t id) -> std::int64_t {
            for (const auto& o : i.options)
                if (o.id == id) return o.value;
            return -1;
        };
        const auto values = [](std::int64_t a, std::int64_t b, bool percent) {
            return percent ? std::format("{} % -> {} %", a, b) : std::format("{} -> {}", a, b);
        };
        // 2. The others assimilation changed (the old item's '+' options in id order); APE's charge.
        std::vector<game::ItemInstance::Option> old = before_.options;
        std::sort(old.begin(), old.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        for (const auto& o : old) {
            const assets::MagicOptionRow* m = game::FindOption(*c_.magicOptions, o.id);
            if (m == nullptr || m->paramName.find('+') == std::string::npos || m->optionName == stoneOption_) continue;
            const std::int64_t later = valueOf(after, o.id);
            if (m->optionName == "MATTR_APE") continue;
            if (o.value > 0 && later >= 0 && later != std::int64_t(o.value)) {
                const MagicOptionNames names = MagicOptionAlchemyNames(c_, m->optionName, std::uint32_t(later));
                line("UIIT_MSG_ALCHEMY_ATTR_ASSIMILATION", names.change, values(o.value, later, names.percent));
            }
        }
        if (apeUsed) lines.push_back(c_.Text("UIIT_MSG_ALCHEMY_ASSIMILATION_PREVENTION_SUCCESS"));
        // 3. The stone's option: granted when the item had none of it, else changed (printed even unchanged).
        if (stoneKind_ == 1) {
            const bool repair = stoneOption_ == "MATTR_REPAIR";
            bool had = false;
            for (const auto& o : before_.options) {
                const assets::MagicOptionRow* m = game::FindOption(*c_.magicOptions, o.id);
                had = had || (m != nullptr && m->optionName.find(stoneOption_) != std::string::npos);
            }
            for (const auto& o : after.options) {
                const assets::MagicOptionRow* m = game::FindOption(*c_.magicOptions, o.id);
                if (m == nullptr || m->paramName.find('+') == std::string::npos || m->optionName.find(stoneOption_) == std::string::npos) continue;
                const MagicOptionNames names = MagicOptionAlchemyNames(c_, m->optionName, o.value);
                if (!had && o.value > (repair ? 1u : 0u)) {
                    lines.push_back(FormatText(c_.Text("UIIT_MSG_ALCHEMY_APPEND_ATTR"), {item, names.append}));
                } else if (const std::int64_t before = valueOf(before_, o.id); before > (repair ? 1 : 0)) {
                    line("UIIT_MSG_ALCHEMY_CHANGE_CATTR", names.change, values(repair ? before - 1 : before, repair ? std::int64_t(o.value) - 1 : o.value, names.percent));
                }
            }
        }
        return lines;
    }

    static std::string Variant(const std::string& path, const char* suffix) {
        for (const char* from : {"_on.ddj", "_off.ddj"}) {
            const std::size_t at = path.rfind(from);
            if (at != std::string::npos) return path.substr(0, at) + suffix + ".ddj";
        }
        return path;
    }

    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    ui::Widget *alchemyPage_ = nullptr, *socketPage_ = nullptr, *type1_ = nullptr, *type2_ = nullptr;
    AlchemyEffect* effect_ = nullptr;
    AlchemyAlert* alert_ = nullptr;
    std::array<Placed, 9> slots_{};
    bool running_ = false, cancelRequested_ = false;
    double gaugeStart_ = -1.0, gaugeSeconds_ = 1.0, successAt_ = 0.0, finishAt_ = 0.0;
    int prepareRound_ = 0;
    std::string helpShown_;
    // Strengthen pressed with a stone: its kind (1 magic, 2 attribute; 0 an elixir), its option (Desc1) and the
    // equipment as it was (the result lines compare; 1.188 keeps the same three).
    int stoneKind_ = 0;
    std::string stoneOption_;
    game::ItemInstance before_;
};

AlchemyWindow* WindowOf(HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    return ui != nullptr ? dynamic_cast<AlchemyWindow*>(ui->Window(hud::kEnchant)) : nullptr;
}

} // namespace

void RegisterAlchemyWindow(ui::Interface& ui, HudContext& context) {
    ui.Register("CNIFEnchantWnd", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<AlchemyWindow>(n, *c); });
    ui.Register("CNIFMiniConfirm", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<AlchemyAlert>(n, *c); });
}

void ToggleAlchemyWindow(HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    if (AlchemyWindow* w = WindowOf(c); w != nullptr && w->visible) {
        w->Closing();
        c.hud->Hide(hud::kEnchant);
        ui->Sound("SND_WINDOW_CLOSE");
        return;
    }
    // Not over the stall, the exchange, the stall network.
    if (c.hud->Shown(hud::kStall) || c.hud->Shown(hud::kExchange) || c.hud->Shown(hud::kStallNetwork)) return;
    if (AlchemyWindow* w = WindowOf(c)) {
        c.hud->Show(hud::kEnchant);
    } else {
        auto node = RootNode2dt(c, kLayout, "CNIFEnchantWnd", hud::kEnchant);
        if (!node) return;
        node->rect.x = 50.0f;
        node->rect.y = std::max(0.0f, ui->screen().y - 609.0f);
        ui->Open(*node);
    }
    ui->Sound("SND_WINDOW_OPEN");
    c.hud->OpenPage(hud::kPageInventory); // the main window on its inventory
}

bool AlchemyShown(HudContext& c) {
    AlchemyWindow* w = WindowOf(c);
    return w != nullptr && w->visible;
}

void AlchemyPut(HudContext& c, int place) {
    if (AlchemyWindow* w = WindowOf(c); w != nullptr && w->visible) w->Put(place);
}

void AlchemyShowTab(HudContext& c, int tab) {
    if (AlchemyWindow* w = WindowOf(c)) w->PickTab(tab);
}

void AlchemyPress(HudContext& c) {
    if (AlchemyWindow* w = WindowOf(c)) w->Press();
}

bool AlchemyAnswer(HudContext& c, bool yes) {
    AlchemyWindow* w = WindowOf(c);
    return w != nullptr && w->Answer(yes);
}

void AlchemyShowSocket(HudContext& c, bool socket) {
    if (AlchemyWindow* w = WindowOf(c)) w->ShowSocket(socket);
}

void AlchemySocketMake(HudContext& c, int index) {
    if (AlchemyWindow* w = WindowOf(c)) w->SocketMakeAt(index);
}

void AlchemySocketInsert(HudContext& c, int stonePlace, int index) {
    if (AlchemyWindow* w = WindowOf(c)) w->SocketStoneAt(stonePlace, index);
}

void AlchemySocketErase(HudContext& c, int index) {
    if (AlchemyWindow* w = WindowOf(c)) w->SocketRemoveAt(index);
}

void AlchemySocketCancel(HudContext& c) {
    if (AlchemyWindow* w = WindowOf(c)) w->SocketCancel();
}

int AlchemySocketPickMode(HudContext& c) {
    AlchemyWindow* w = WindowOf(c);
    return w != nullptr ? w->SocketPickMode() : 0;
}

void AlchemyResultArrived(HudContext& c, const proto::AlchemyResult& result) {
    core::LogInfo("game: alchemy: operation {} outcome {} code 0x{:04x} place {} plus {} -> {}", int(result.operation), int(result.outcome), result.code,
                  result.place, result.plusBefore, result.plusAfter);
    if (AlchemyWindow* w = WindowOf(c)) w->Result(result);
}

} // namespace sr::client
