// The help request (the original's customer centre, out of service in 1.188 and brought back on the system menu by the
// user's call; the build's SR_BUG_REPORTS): the original's request panel (resinfo\ifcasrequesthelp.txt, CIFCASRequestHelp)
// in the service window's frame (ifcasrequestservice.txt: the panel at 14, 60 in frameg01, the window 449 x 430, titled
// UIIT_STT_CAS_CUSTOMER_CENTER). The category (30, in the original's order), the user it is about (31), an e-mail (32) and
// the text (33); Confirm (35) checks them as the original does, asks "Register help request?" and gives them to
// HudContext::helpRequest (the server keeps it as the website's ticket of kind "help", the admin's alone); Cancel (36)
// closes it. Ours: no results tab (the answers are the staff's on the website).

#include <algorithm>
#include <string>
#include <vector>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/game/Guild.h"
#include "sr/game/SupportProtocol.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

namespace sr::client {

namespace {

constexpr float kWidth = 449.0f, kHeight = 430.0f;
constexpr int kCategory = 30, kTarget = 31, kEmail = 32, kText = 33, kConfirm = 35, kCancel = 36, kPanel = 200;
// The categories in the window's order (the original's CIFCASRequestHelp); the server's names in kHelpCategories.
constexpr const char* kCategoryKeys[] = {"UIIT_STT_CAS_CUSTOMER_CENTER_REPORT_NOT_MOVE",     "UIIT_STT_CAS_CUSTOMER_CENTER_REPORT_BAD_MANNERED",
                                         "UIIT_STT_CAS_CUSTOMER_CENTER_REPORT_ADVERTISING",  "UIIT_STT_CAS_CUSTOMER_CENTER_REPORT_ACCOUNT_RELATED",
                                         "UIIT_STT_CAS_CUSTOMER_CENTER_REPORT_BUG",          "UIIT_STT_CAS_CUSTOMER_CENTER_REPORT_ILLEGAL_SOFTWARE",
                                         "UIIT_STT_CAS_CUSTOMER_CENTER_ETC"};

class HelpRequestWindow : public ui::MainFrame {
public:
    HelpRequestWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        if (ui::Widget* title = Child(0)) title->SetText(c_.Text("UIIT_STT_CAS_CUSTOMER_CENTER"));
        // The panel in its frame, then the request's controls from their layout.
        assets::UiLayoutNode panel;
        panel.type = "CIFFrame";
        panel.id = kPanel;
        panel.rect = {14.0f, 60.0f, 421.0f, 356.0f};
        panel.ddj = "interface\\frame\\frameg01_wnd_";
        ui::Widget* frame = Add(ui.Make(panel));
        if (const assets::UiLayout* layout = ui.Layout("resinfo\\ifcasrequesthelp.txt"); layout != nullptr && frame != nullptr) {
            // Last first, as the layouts are made (the background last in the file, under the rest).
            for (const auto& section : layout->sections) {
                if (section.name != "Create") continue;
                for (auto it = section.nodes.rbegin(); it != section.nodes.rend(); ++it) frame->Add(ui.Make(*it));
            }
        }
        if (auto* combo = ui::As<ui::ComboBox>(Find(kCategory))) {
            combo->items.clear();
            for (const char* key : kCategoryKeys) combo->items.push_back(c_.Text(key));
            combo->Select(0);
        }
        // Room to type past the original's limits: it says so on Confirm (UIIT_MSG_CAS_CUSTOMER_CENTER_ERR_INPUTERR).
        if (auto* e = ui::As<ui::Edit>(Find(kTarget))) e->limit = 32;
        if (auto* e = ui::As<ui::Edit>(Find(kEmail))) e->limit = 64;
        if (auto* e = ui::As<ui::Edit>(Find(kText))) {
            e->limit = 1024;
            e->multiline = true;
        }
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.control == kCancel) {
            visible = false;
            return true;
        }
        if (n.kind == ui::Notice::Kind::Clicked && n.control == kConfirm) {
            Confirm();
            return true;
        }
        return ui::MainFrame::OnNotice(n);
    }

private:
    ui::Widget* Find(int id) {
        ui::Widget* frame = Child(kPanel);
        return frame != nullptr ? frame->Child(id) : nullptr;
    }
    std::string Value(int id) {
        ui::Widget* w = Find(id);
        std::string s = w != nullptr ? w->text() : std::string();
        const std::size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
    void Refuse(const char* key) {
        if (c_.hud != nullptr) c_.hud->Inform("UIIT_STT_CAS_CUSTOMER_CENTER", {c_.Text(key)}, nullptr);
    }

    // The original's checks in its order: the user for the categories about one, the e-mail, the text, then the lengths.
    void Confirm() {
        auto* combo = ui::As<ui::ComboBox>(Find(kCategory));
        const int category = combo != nullptr ? std::clamp(combo->selected, 0, int(std::size(kCategoryKeys)) - 1) : 0;
        const std::string target = Value(kTarget), email = Value(kEmail), text = Value(kText);
        if (game::proto::HelpNeedsTarget(std::uint8_t(category)) && target.empty()) return Refuse("UIO_MSG_ERROR_INPUT");
        if (email.empty()) return Refuse("UIIT_MSG_CAS_CUSTOMER_CENTER_ERR_EMAIL");
        if (text.empty()) return Refuse("UIIT_MSG_CAS_CUSTOMER_CENTER_ERR_NOTINPUT");
        if (game::Utf8Length(text) > game::proto::kHelpTextChars || game::Utf8Length(email) > game::proto::kHelpEmailChars ||
            game::Utf8Length(target) > game::proto::kHelpTargetChars) {
            return Refuse("UIIT_MSG_CAS_CUSTOMER_CENTER_ERR_INPUTERR");
        }
        if (c_.hud == nullptr || !c_.helpRequest) return;
        c_.hud->Confirm("UIIT_STT_CAS_CUSTOMER_CENTER",
                        {c_.Text("UIIT_STT_CAS_CUSTOMER_CENTER_HELP_REQUEST_1"), c_.Text("UIIT_STT_CAS_CUSTOMER_CENTER_HELP_REQUEST_2"),
                         c_.Text("UIIT_STT_CAS_CUSTOMER_CENTER_HELP_REQUEST_3")},
                        [this, category, target, email, text](bool yes) {
                            if (!yes || !c_.helpRequest) return;
                            c_.helpRequest(std::uint8_t(category), target, email, text);
                            visible = false;
                            if (ui::Widget* w = Find(kText)) w->SetText({});
                        });
    }

    HudContext& c_;
};

} // namespace

void RegisterHelpRequestWindow(ui::Interface& ui, HudContext& context) {
    ui.Register("SRHelpRequest", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<HelpRequestWindow>(n, *c); });
}

void OpenHelpRequest(HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    if (ui::Widget* w = ui->Window(hud::kHelpRequest)) {
        c.hud->Show(hud::kHelpRequest);
        ui->Raise(w);
        return;
    }
    assets::UiLayoutNode node;
    node.type = "SRHelpRequest";
    node.id = hud::kHelpRequest;
    node.ddj = "interface\\frame\\mframe_wnd_";
    node.rect = {std::max(0.0f, (ui->screen().x - kWidth) / 2.0f), std::max(0.0f, (ui->screen().y - kHeight) / 2.0f), kWidth, kHeight};
    ui->Open(node);
    ui->Sound("SND_WINDOW_OPEN");
}

} // namespace sr::client
