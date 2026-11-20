// Guild wars on the client, from the 1.188 client's layouts: the war's
// request window (CIFRequestGuildWar, ginterface.txt 59, ifguildwarrequest.txt: the victory points,
// the reward each master stakes, the period in days, hours and minutes, the guild's name), its
// confirmation ("Proclaim war against [%s]guild. ... Will you proclaim war?"), the other master's
// agreement ("[%s]guild has proclaimed war. ... Will you accept it?") and the reward's box at a guild
// NPC ("War refundable rewards is [%d]. Would you like to receive rewards?"). The confirmation and
// the agreement are the original's own boxes (ifguildwarconfirm.txt / ifguildwaragree.txt, built below).

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/game/Guild.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr int kRequestWindow = 59;
constexpr int kOk = 5, kCancel = 6, kPointBox = 23, kDayBox = 24, kHourBox = 25, kMinuteBox = 26, kMoneyButton = 28, kMoney = 40, kName = 55;
constexpr int kMinuteSteps[] = {0, 10, 20, 30, 40, 50};

void AddSection(ui::Interface& ui, ui::Widget& to, const char* layout, const char* section) {
    const assets::UiLayout* l = ui.Layout(layout);
    if (l == nullptr) return;
    for (const auto& s : l->sections) {
        if (s.name != section) continue;
        for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) to.Add(ui.Make(*it));
    }
}

std::string Period(const HudContext& c, const proto::GuildWarTerms& t) {
    return t.unlimited != 0 ? c.text("UIIT_CTL_GUILDWAR_UNLIMITED")
                            : std::format("{} {} {} {} {} {}", t.days, c.text("PARAM_DAY"), t.hours, c.text("UIIT_STT_HOUR"), t.minutes, c.text("UIIT_STT_MINUTE"));
}

std::string Points(const HudContext& c, std::uint8_t victory) {
    const std::uint32_t p = game::kGuildWarVictoryPoints[std::min<std::size_t>(victory, game::kGuildWarVictoryPoints.size() - 1)];
    return p == 0 ? c.text("UIIT_CTL_GUILDWAR_UNLIMITED") : std::to_string(p);
}

// The war's two questions, the terms shown (CIFRequestGuildWar's modes 2 and 3):
// the declaring master's confirmation and the other master's agreement.
class WarTermsWindow : public ui::FrameBorder {
public:
    WarTermsWindow(const assets::UiLayoutNode& node, HudContext& c) : FrameBorder(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        FrameBorder::Created(ui);
        ui_ = &ui;
    }

    void Begin(const char* layout, const char* titleKey, const std::string& first, const std::string& second, const proto::GuildWarTerms& t, std::function<void(bool)> answer) {
        if (ui_ == nullptr) return;
        answer_ = std::move(answer);
        for (const char* section : {"Create", "Message", "Condition"}) AddSection(*ui_, *this, layout, section);
        assets::UiLayoutNode title;
        title.type = "CIFStatic";
        title.rect = {20.0f, 12.0f, rect.w - 40.0f, 14.0f};
        title.hAlign = 1;
        title.text = titleKey;
        Add(ui_->Make(title));
        const auto put = [&](int id, const std::string& text) {
            if (ui::Widget* w = Child(id)) w->SetText(text);
        };
        put(kFirst, first);
        put(kSecond, second);
        put(kPoint, Points(c_, t.victory));
        put(kMoney, std::to_string(t.wager));
        put(kPeriod, Period(c_, t));
        // Ours: the English "Compensation" reaches over this picture; here it is no button anyway.
        if (ui::Widget* icon = Child(kMoneyIcon)) icon->visible = false;
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr || n.source->parent() != this) return FrameBorder::OnNotice(n);
        if (n.control == kYes || n.control == kNo) Press(n.control == kYes);
        return true;
    }

    void Press(bool yes) {
        auto answer = std::move(answer_);
        if (ui_ != nullptr) ui_->Close(this);
        if (answer) answer(yes);
    }

private:
    static constexpr int kYes = 5, kNo = 6, kFirst = 10, kSecond = 11, kMoneyIcon = 26, kPoint = 31, kPeriod = 32, kMoney = 40;
    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::function<void(bool)> answer_;
};

class RequestWindow : public ui::MainFrame {
public:
    RequestWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        SetText(c_.text ? c_.text("UIIT_STT_GUILD_RESPECT_WAR_REPORT") : std::string("Declare war"));
        MainFrame::Created(ui);
        ui_ = &ui;
        for (const char* section : {"Create", "Message", "Condition", "InputGuild"}) AddSection(ui, *this, "resinfo\\ifguildwarrequest.txt", section);
        if (auto* box = ui::As<ui::ComboBox>(Child(kPointBox))) {
            for (std::uint8_t i = 0; i < game::kGuildWarVictoryPoints.size(); ++i) box->items.push_back(Points(c_, i));
            box->Select(0);
        }
        if (auto* box = ui::As<ui::ComboBox>(Child(kDayBox))) {
            for (int d = 0; d <= 30; ++d) box->items.push_back(std::to_string(d) + " " + c_.text("PARAM_DAY"));
            box->Select(0);
        }
        if (auto* box = ui::As<ui::ComboBox>(Child(kHourBox))) {
            for (int h = 0; h <= 23; ++h) box->items.push_back(std::to_string(h) + " " + c_.text("UIIT_STT_HOUR"));
            box->Select(1);
        }
        if (auto* box = ui::As<ui::ComboBox>(Child(kMinuteBox))) {
            for (int m : kMinuteSteps) box->items.push_back(std::to_string(m) + " " + c_.text("UIIT_STT_MINUTE"));
            box->Select(0);
        }
        if (auto* edit = ui::As<ui::Edit>(Child(kName))) edit->limit = game::kGuildNameMax;
        ShowWager();
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr || n.source->parent() != this) return MainFrame::OnNotice(n);
        if (n.control == kCancel) {
            visible = false;
            return true;
        }
        if (n.control == kMoneyButton || n.control == kMoney) {
            AskWager();
            return true;
        }
        if (n.control == kOk) {
            Confirm();
            return true;
        }
        return MainFrame::OnNotice(n);
    }

    // The test options' way: the terms and the name set, then Confirm.
    void Fill(const std::string& name, const proto::GuildWarTerms& t) {
        if (auto* edit = ui::As<ui::Edit>(Child(kName))) edit->SetValue(name);
        if (auto* box = ui::As<ui::ComboBox>(Child(kPointBox))) box->Select(t.victory);
        if (auto* box = ui::As<ui::ComboBox>(Child(kDayBox))) box->Select(t.days);
        if (auto* box = ui::As<ui::ComboBox>(Child(kHourBox))) box->Select(t.hours);
        if (auto* box = ui::As<ui::ComboBox>(Child(kMinuteBox))) {
            const auto it = std::find(std::begin(kMinuteSteps), std::end(kMinuteSteps), int(t.minutes));
            box->Select(it != std::end(kMinuteSteps) ? int(it - std::begin(kMinuteSteps)) : 0);
        }
        wager_ = t.wager;
        ShowWager();
    }

    void Confirm() {
        std::string name;
        if (auto* edit = ui::As<ui::Edit>(Child(kName))) name = edit->text();
        const auto post = [&](const std::string& line) {
            if (c_.state != nullptr) c_.state->Post(game::GameMessage::Kind::General, line, c_.time);
        };
        // The window's own checks: a name, not its own guild, the reward's limit.
        if (name.empty()) return post(c_.text("UIIT_MSG_GUILDWARERR_ERRINPUT"));
        if (c_.guild != nullptr && name == c_.guild->Guild().name) return post(c_.text("UIIT_MSG_GUILDWARERR_SELFWAR"));
        if (wager_ > game::kGuildWarMaxWager) return post(FormatText(c_.text("UIIT_MSG_GUILDWARERR_LIMIT_COMPENSATION"), {"500,000,000"}));
        const proto::GuildWarTerms terms = Terms();
        HudContext* ctx = &c_;
        AskWarTerms(c_, "resinfo\\ifguildwarconfirm.txt", "UIIT_STT_CONFIRM_BOX", FormatText(c_.text("UIIT_MSG_GUILDWAR_SANCTIONWAR_01"), {name}), c_.text("UIIT_MSG_GUILDWAR_SANCTIONWAR_02"), terms, [ctx, name, terms](bool yes) {
            if (yes && ctx->guildRequest) ctx->guildRequest(proto::GuildWarDeclare::kId, proto::Encode(proto::GuildWarDeclare{name, terms}));
        });
        visible = false;
    }

private:
    proto::GuildWarTerms Terms() const {
        proto::GuildWarTerms t;
        const auto chosen = [&](int id) {
            const auto* box = ui::As<ui::ComboBox>(const_cast<RequestWindow*>(this)->Child(id));
            return box != nullptr ? std::max(0, box->selected) : 0;
        };
        t.victory = std::uint8_t(chosen(kPointBox));
        t.days = std::uint8_t(chosen(kDayBox));
        t.hours = std::uint8_t(chosen(kHourBox));
        t.minutes = std::uint8_t(kMinuteSteps[std::min(chosen(kMinuteBox), 5)]);
        t.unlimited = t.days == 0 && t.hours == 0 && t.minutes == 0 ? 1 : 0; // no period asked: none
        t.wager = wager_;
        return t;
    }
    void ShowWager() {
        if (ui::Widget* money = Child(kMoney)) money->SetText(std::to_string(wager_));
    }
    void AskWager() {
        // Ours: the amount typed in the input box (the original's money button opens its own pad).
        if (c_.hud == nullptr || ui_ == nullptr) return;
        AskGuildText(c_, c_.text("UIIT_STT_WARSIT_WARCOMPAY"), 10, [this](std::string typed) {
            wager_ = std::uint32_t(std::min<unsigned long long>(std::strtoull(typed.c_str(), nullptr, 10), 0xFFFFFFFFull));
            ShowWager();
        });
    }

    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::uint32_t wager_ = 0;
};

} // namespace

std::vector<std::string> GuildWarTermsLines(const HudContext& c, const proto::GuildWarTerms& t) {
    std::vector<std::string> lines;
    lines.push_back(c.text("UIIT_MSG_GUILDWAR_POINTBAR") + " : " + Points(c, t.victory));
    lines.push_back(c.text("UIIT_STT_WARSIT_WARCOMPAY") + " : " + std::to_string(t.wager) + " " + c.text("UIIT_STT_GOLD"));
    lines.push_back(c.text("UIIT_MSG_GUILDWAR_DAYBAR") + " : " + Period(c, t));
    return lines;
}

void RegisterGuildWarWindows(ui::Interface& ui, HudContext& context) {
    // Its frame and size as its code gives them (the message box's frame, 350 x 257).
    ui.Register("CIFRequestGuildWar", [c = &context](const assets::UiLayoutNode& n) {
        assets::UiLayoutNode node = n;
        node.ddj = "interface\\messagebox\\msgbox2_window_";
        node.rect.w = 350.0f;
        node.rect.h = 257.0f;
        return std::make_unique<RequestWindow>(node, *c);
    });
    ui.Register("CNIFGuildWarTerms", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<WarTermsWindow>(n, *c); });
}

void OpenGuildWarRequest(HudContext& c) {
    if (c.hud != nullptr) c.hud->Show(kRequestWindow);
}

bool FillGuildWarRequest(HudContext& c, const std::string& name, const proto::GuildWarTerms& terms, bool confirm) {
    auto* window = c.hud != nullptr ? dynamic_cast<RequestWindow*>(c.hud->Show(kRequestWindow)) : nullptr;
    if (window == nullptr) return false;
    window->Fill(name, terms);
    if (confirm) window->Confirm();
    return true;
}

void AskWarAnswer(HudContext& c, const proto::GuildWarProposal& p, std::function<void(bool)> answer) {
    if (c.hud == nullptr) return;
    AskWarTerms(c, "resinfo\\ifguildwaragree.txt", "UIIT_STT_AGREEMENT_BOX", FormatText(c.text("UIIT_MSG_GUILDWAR_CONSENTWAR_01"), {p.guildName}), c.text("UIIT_MSG_GUILDWAR_CONSENTWAR_02"), p.terms, std::move(answer));
}

void AskWarTerms(HudContext& c, const char* layout, const char* titleKey, const std::string& first, const std::string& second, const proto::GuildWarTerms& terms, std::function<void(bool)> answer) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    assets::UiLayoutNode node;
    node.type = "CNIFGuildWarTerms";
    node.ddj = "interface\\messagebox\\msgbox2_window_";
    node.rect = {0.0f, 0.0f, 350.0f, 225.0f}; // ours: its layouts' tile with the frame's margins
    auto* window = dynamic_cast<WarTermsWindow*>(ui->Open(node));
    if (window == nullptr) return;
    ui->Centre(*window, ui->screen());
    window->Begin(layout, titleKey, first, second, terms, std::move(answer));
}

bool AnswerWarTerms(HudContext& c, bool yes) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return false;
    for (ui::Widget* w : ui->Windows()) {
        if (auto* window = dynamic_cast<WarTermsWindow*>(w); window != nullptr && window->visible) {
            window->Press(yes);
            return true;
        }
    }
    return false;
}

void AskWarReward(HudContext& c, std::uint64_t gold) {
    if (c.hud == nullptr) return;
    HudContext* ctx = &c;
    const std::uint32_t npc = c.guildNpc;
    c.hud->Confirm("UIIT_STT_CONFIRM_BOX",
                   {FormatText(c.text("UIIT_CTL_GUILDWAR_COMPENSATION_01"), {std::to_string(gold)}), c.text("UIIT_CTL_GUILDWAR_COMPENSATION_02")},
                   [ctx, npc](bool yes) {
                       if (yes && ctx->guildRequest) ctx->guildRequest(proto::GuildWarRewardClaim::kId, proto::Encode(proto::GuildWarRewardClaim{npc}));
                   });
}

} // namespace sr::client
