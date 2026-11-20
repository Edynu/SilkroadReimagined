// The free PvP window: 1.188's CNIFFreeBattlePvp from res_ui\frpvp.2dt (opens
// and closes it from the Action page's 1017): its top strip (frpvp_frame01) with the fold button (8), and under it
// (frpvp_frame02) the five teams' boxes (9..13) beside their icons (14 red, 16 gray, 15 blue, 17 white, 18 yellow),
// and Cancel (7). A box chosen asks for that team (0x7516, the boxes act as one group); Cancel, or
// the chosen box again, leaves the mode (0). The box of the player's team is the chosen one; while a change runs
// every box waits.

#include <string>

#include "gui/Hud.h"
#include "gui/HudWindows.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr const char* kLayout = "res_ui\\frpvp.2dt";
constexpr int kFold = 8, kCancel = 7, kBody = 2, kFirstBox = 9; // boxes 9..13: teams 1..5 (red, gray, blue, white, yellow)

// A team's box: com_radiobutton_on when chosen, _off otherwise; a press tells the window.
class TeamBox : public ui::Widget {
public:
    using Widget::Widget;
    bool chosen = false;
    void Created(ui::Interface&) override { interactive = true; }
    void Draw(ui::Frame& frame) override {
        alpha = enabled ? 1.0f : 128.0f / 255.0f;
        frame.paint.Image(chosen ? "interface\\ifcommon\\com_radiobutton_on.ddj" : "interface\\ifcommon\\com_radiobutton_off.ddj", ScreenRect(),
                          {0.0f, 0.0f}, {1.0f, 1.0f}, ui::WithAlpha(ui::kWhite, Opacity()));
    }
    void Mouse(ui::Frame& frame) override {
        if (enabled && frame.input.Released(ui::MouseButton::Left) && frame.pointer.hovered == this) Notify(ui::Notice::Kind::Clicked);
    }
};

class FreePvpWindow : public ui::Widget {
public:
    FreePvpWindow(const assets::UiLayoutNode& node, HudContext& c) : Widget(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        const auto* records = c_.layout2dt ? c_.layout2dt(kLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        const assets::Ui2dtRecord& root = records->front();
        // Every control placed from the root's corner; the body's controls in a body of their own (folded together).
        assets::UiLayoutNode bodyNode;
        bodyNode.type = "CIFStatic";
        bodyNode.id = kBody;
        ui::Widget* body = Add(ui.Make(bodyNode));
        for (std::size_t i = 1; i < records->size(); ++i) {
            const assets::Ui2dtRecord& r = (*records)[i];
            if (r.id == kBody) continue;
            assets::UiLayoutNode n = assets::Ui2dtNode(r, root.rect.x, root.rect.y);
            if (r.kind == 18) { // the teams' icons: their art
                n.type = "CIFStatic";
                n.ddj = r.art;
            }
            ui::Widget* under = r.parents[0] == kBody && body != nullptr ? body : this;
            if (r.kind == 11) {
                auto box = std::make_unique<TeamBox>(n);
                under->Add(std::move(box));
                continue;
            }
            under->Add(ui.Make(n));
        }
    }

    void Update(ui::Frame& frame) override {
        Widget::Update(frame);
        for (int k = 0; k < int(proto::kFreePvpTeams); ++k) {
            if (auto* box = dynamic_cast<TeamBox*>(Find(kFirstBox + k))) {
                box->chosen = c_.freePvpTeam == k + 1;
                box->enabled = !c_.freePvpChanging;
            }
        }
        if (ui::Widget* cancel = Find(kCancel)) cancel->enabled = c_.freePvpTeam != 0 || c_.freePvpChanging;
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr) return false;
        const int id = n.source->id();
        if (id == kFold) { // the body folded away or back
            if (ui::Widget* body = Child(kBody)) body->visible = !body->visible;
            return true;
        }
        if (id == kCancel) {
            if (c_.freePvpRequest) c_.freePvpRequest(0);
            return true;
        }
        if (id >= kFirstBox && id < kFirstBox + int(proto::kFreePvpTeams)) {
            const std::uint8_t team = std::uint8_t(id - kFirstBox + 1);
            if (c_.freePvpRequest) c_.freePvpRequest(team == c_.freePvpTeam ? 0 : team);
            return true;
        }
        return false;
    }

private:
    // A control by id, in the window or its body.
    ui::Widget* Find(int id) {
        if (ui::Widget* w = Child(id)) return w;
        ui::Widget* body = Child(kBody);
        return body != nullptr ? body->Child(id) : nullptr;
    }
    HudContext& c_;
};

} // namespace

void RegisterFreePvpWindow(ui::Interface& ui, HudContext& context) {
    ui.Register("CNIFFreeBattlePvp", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<FreePvpWindow>(n, *c); });
}

void OpenFreePvpWindow(HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    // Under the minimap (the 2dt's own place: 817, 76 of 1024 across, kept from the right edge).
    assets::UiLayoutNode node;
    node.type = "CNIFFreeBattlePvp";
    node.id = hud::kFreePvp;
    node.rect = {ui->screen().x - (1024.0f - 810.0f), 76.0f, 84.0f, 217.0f};
    ui->Open(node);
}

} // namespace sr::client
