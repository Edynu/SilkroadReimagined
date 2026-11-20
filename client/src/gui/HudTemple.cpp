// The temples' reputation board: 1.188's CNIFConfirmReputation from res_ui\reputation.2dt, opened
// by the answer to "Temple area Point" (0x74DF / 0xB4DF). Its two CNIFConfirmReputationSlot rows, Anubis's
// pair and Haroeris's (the sanctums' names), each filled so: who may enter (id 8, under the
// title 7; the trader and hunter guilds, the thief guild, everyone, nobody), the next opening (10, under 9), since
// when the reputation is gathered (12, under 11 "AP"), the two alliances' reputation as a split gauge (14: green the traders
// and hunters, orange the thieves) with their numbers (18, 19). While shown it asks for the numbers again every 10 seconds
// (its timer 1, 10000 ms, 0x74E0, answered by 0xB4E0).
//
// The board's icon (CIFConfirmReputationGuide): the side icon 0xA5 (made at the server's
// 0x34E1) with com_grad_icon.ddj and the tooltip "AP confirm" (its slot 10); a click asks for
// the board without an NPC (0x74DF's kind 1).

#include <algorithm>
#include <ctime>
#include <format>

#include "gui/Hud.h"
#include "gui/HudWindows.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr const char* kLayout = "res_ui\\reputation.2dt";
constexpr int kWindowId = 196;
constexpr int kRowTitle = 6, kUnionTitle = 7, kUnion = 8, kTimeTitle = 9, kTime = 10, kPeriodTitle = 11, kPeriod = 12, kGauge = 14,
              kTraderHunter = 18, kThief = 19;
// The rows' sanctums: Anubis's and Isis's, Haroeris's and Seth's.
constexpr std::array<std::array<const char*, 2>, 2> kNames{{{"SN_ZONE_21837_2", "SN_ZONE_21836_2"}, {"SN_ZONE_21836_4", "SN_ZONE_21836_5"}}};

class ReputationWindow : public ui::MainFrame {
public:
    ReputationWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        const auto* records = c_.layout2dt ? c_.layout2dt(kLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        const std::vector<ui::Widget*> made = Build2dt(ui, *this, *records, [](const assets::Ui2dtRecord& r, assets::UiLayoutNode& n) {
            if (r.kind == 17) n.type = "CIFGauge"; // the two-coloured gauge: its art the green part
        });
        for (std::size_t i = 0; i < records->size(); ++i) {
            if ((*records)[i].className == "CNIFConfirmReputationSlot") rows_.push_back(made[i]);
        }
        std::sort(rows_.begin(), rows_.end(), [](const ui::Widget* a, const ui::Widget* b) { return a->rect.y < b->rect.y; });
        if (ui::Widget* title = Find(166)) title->SetText(c_.Text("UIIT_PAG_EGYPT_SHRINE_REPUTATION"));
    }

    void Fill(const std::vector<proto::TempleReputationRow>& rows) {
        for (std::size_t k = 0; k < rows_.size() && k < rows.size() && k < kNames.size(); ++k) {
            ui::Widget* row = rows_[k];
            const proto::TempleReputationRow& r = rows[k];
            Put(row, kRowTitle, c_.Text(kNames[k][0]) + ", " + c_.Text(kNames[k][1]));
            Put(row, kUnionTitle, c_.Text("UIIT_STT_EGYPT_SHRINE_ENTRANCE_UNION"));
            Put(row, kUnion, Union(r.allowed));
            Put(row, kTimeTitle, c_.Text("UIIT_STT_EGYPT_SHRINE_ENTRANCE_TIME"));
            Put(row, kTime, When(r.nextOpening));
            Put(row, kPeriodTitle, c_.Text("UIIT_STT_EGYPT_SHRINE_REPUTATION_PERIOD"));
            Put(row, kPeriod, When(r.since));
            Points(row, r.traderHunter, r.thief);
        }
        asked_ = c_.time;
    }

    // the numbers and the gauges again.
    void Refresh(const proto::TemplePoints& p) {
        if (rows_.size() >= 1) Points(rows_[0], p.anubisTraderHunter, p.anubisThief);
        if (rows_.size() >= 2) Points(rows_[1], p.haroerisTraderHunter, p.haroerisThief);
    }

    void Update(ui::Frame& frame) override {
        MainFrame::Update(frame);
        if (!visible || c_.time - asked_ < 10.0) return;
        asked_ = c_.time;
        if (c_.instanceRequest) c_.instanceRequest(proto::TemplePointsAsk::kId, proto::Encode(proto::TemplePointsAsk{}));
    }

private:
    void Put(ui::Widget* row, int id, const std::string& text) {
        if (ui::Widget* w = row->Child(id)) w->SetText(text);
    }
    void Points(ui::Widget* row, std::uint32_t traderHunter, std::uint32_t thief) {
        Put(row, kTraderHunter, std::to_string(traderHunter));
        Put(row, kThief, std::to_string(thief));
        if (auto* gauge = ui::As<ui::Gauge>(row->Child(kGauge))) {
            const double total = double(traderHunter) + double(thief);
            gauge->SetValue(total > 0.0 ? float(double(traderHunter) / total) : 0.5f, true);
            gauge->rest = "interface\\ifcommon\\com_orange_grad_gage.ddj"; // the thieves' part
        }
    }
    std::string Union(std::uint8_t allowed) const {
        switch (allowed) {
        case proto::TempleNotice::kTraderHunter: return c_.Text("UIIT_PAG_COMPANY_MERCHANT") + ", " + c_.Text("UIIT_PAG_COMPANY_HENTER");
        case proto::TempleNotice::kThief: return c_.Text("UIIT_PAG_COMPANY_THIEF");
        case proto::TempleNotice::kEveryone: return c_.Text("UIIT_STT_EGYPT_SHRINE_ENTRANCE_UNION_ALL");
        default: return c_.Text("UIIT_STT_EGYPT_SHRINE_ENTRANCE_UNION_EMPTY");
        }
    }
    // "%d-%d  %d:%d" (UIIT_STT_EGYPT_SHRINE_REPUTATION_ACQUIRE_TIME): the month, the day, the hour, the minute.
    std::string When(std::int64_t unix) const {
        if (unix <= 0) return "-";
        std::time_t t = std::time_t(unix);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &t);
#else
        localtime_r(&t, &local);
#endif
        std::string line = c_.Text("UIIT_STT_EGYPT_SHRINE_REPUTATION_ACQUIRE_TIME");
        for (const int v : {local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min}) {
            const std::size_t at = line.find("%d");
            if (at == std::string::npos) break;
            line.replace(at, 2, std::format("{:02}", v));
        }
        return line;
    }

    HudContext& c_;
    std::vector<ui::Widget*> rows_;
    double asked_ = 0.0;
};

// The board's icon (CIFConfirmReputationGuide).
class ReputationGuide : public ui::Static {
public:
    ReputationGuide(const assets::UiLayoutNode& node, HudContext& c) : Static(node), c_(c) {}
    void Created(ui::Interface& ui) override {
        Static::Created(ui);
        interactive = true;
        texture = "interface\\ifcommon\\com_grad_icon.ddj";
    }
    void Update(ui::Frame& frame) override {
        if (frame.pointer.hovered == this) frame.interface.ShowTooltip(c_.Text("UIIT_STT_EGYPT_REPUTATION_ICON_TOOLTIP"), ScreenRect());
    }
    void Mouse(ui::Frame& frame) override {
        if (!frame.input.Released(ui::MouseButton::Left) || frame.pointer.hovered != this) return;
        if (c_.instanceRequest) c_.instanceRequest(proto::TempleReputationAsk::kId, proto::Encode(proto::TempleReputationAsk{0}));
    }

private:
    HudContext& c_;
};

ReputationWindow* WindowOf(HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    return ui != nullptr ? dynamic_cast<ReputationWindow*>(ui->Window(kWindowId)) : nullptr;
}

} // namespace

void RegisterTempleWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CNIFConfirmReputation", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<ReputationWindow>(n, *c); });
    ui.Register("CIFConfirmReputationGuide", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<ReputationGuide>(n, *c); });
}

void UpdateTemplePoints(HudContext& c, const proto::TemplePoints& points) {
    if (ReputationWindow* w = WindowOf(c); w != nullptr && w->visible) w->Refresh(points);
}

void ShowTempleGuide(HudContext& c, bool show) {
    if (c.hud == nullptr) return;
    if (!show) {
        c.hud->Hide(hud::kTempleGuide);
        return;
    }
    // 40 x 40.
    assets::UiLayoutNode node;
    node.type = "CIFConfirmReputationGuide";
    node.id = hud::kTempleGuide;
    node.rect = {0.0f, 0.0f, 40.0f, 40.0f};
    c.hud->ShowGuide(node);
}

void ShowTempleReputation(HudContext& c, const std::vector<proto::TempleReputationRow>& rows) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    if (WindowOf(c) == nullptr) {
        auto node = RootNode2dt(c, kLayout, "CNIFConfirmReputation", kWindowId);
        if (!node) return;
        node->rect.x = std::max(0.0f, (ui->screen().x - node->rect.w) / 2.0f);
        node->rect.y = std::max(0.0f, (ui->screen().y - node->rect.h) / 2.0f - 40.0f);
        ui->Open(*node);
    } else {
        c.hud->Show(kWindowId);
    }
    if (ReputationWindow* w = WindowOf(c)) w->Fill(rows);
    ui->Sound("SND_WINDOW_OPEN");
}

} // namespace sr::client
