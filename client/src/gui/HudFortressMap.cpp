// CIFFortressMap (ginterface.txt 301 GDR_FORTRESS_MAP, resinfo\iffortressmap.txt): the
// battle aide's "Structure management". The fortress's local world map with an icon on each structure's place,
// a click selecting one; the panel beside it (its name, state, step, defence, durability) and the Build, Repair and
// Upgrade buttons, each asking first. A level-1 guard tower's upgrade asks its element first (CIFCheckConfirmWnd,
// ginterface.txt 101). The map is the world map's view (HudMapView.h; CIFFortressMapFrame is a CIFWorldMap): the
// fortress's map of worldmap_mapinfo.txt with its labels, dragged with the left button to look around; a structure's
// tooltip on hover.

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include "gui/Hud.h"
#include "gui/HudMapView.h"
#include "gui/HudWindows.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

namespace sr::client {

namespace proto = game::proto;

// icon\ + a structure's icon: whole as it is, _r damaged, _d destroyed (the battle aide's map and the world map's
// fortress maps, as 1.188's).
std::string FortressStructureIcon(const HudContext& c, const proto::FortressStructureRecord& r) {
    const assets::ObjectRow* row = c.characters != nullptr ? c.characters->Find(r.refObjId) : nullptr;
    if (row == nullptr || row->iconPath.empty()) return {};
    std::string path = "icon\\" + row->iconPath;
    if (r.hp < r.maxHp && r.maxHp > 0) {
        const std::size_t dot = path.rfind('.');
        path.insert(dot == std::string::npos ? path.size() : dot, (r.state & 1) != 0 || r.hp == 0 ? "_d" : "_r");
    }
    return path;
}

namespace {

constexpr int kMapWindow = 301, kTypeWindow = 101;
constexpr int kName = 50, kState = 52, kStep = 54, kDefence = 56, kDurability = 58, kFeatureLabel = 59, kFeature = 60;
constexpr int kBuild = 61, kRepair = 62, kUpgrade = 63;
constexpr const char* kLayout = "resinfo\\iffortressmap.txt";

// Each fortress's map: worldmap_mapinfo.txt's rows 1001..1003 (their instance codes INS_FORT_JA, _BJ, _HT).
int MapOf(std::uint32_t fortress) { return fortress == 1 ? 1001 : fortress == 6 ? 1002 : fortress == 3 ? 1003 : -1; }
// The view in the frame: 608 x 386 at its 3, 2.
constexpr float kViewW = 608.0f, kViewH = 386.0f, kViewX = 3.0f, kViewY = 2.0f;

// "<sml2>line<br>line</sml2>" into its lines.
std::vector<std::string> Lines(std::string text) {
    for (const char* tag : {"<sml2>", "</sml2>"}) {
        for (std::size_t at; (at = text.find(tag)) != std::string::npos;) text.erase(at, std::char_traits<char>::length(tag));
    }
    std::vector<std::string> out;
    for (std::size_t at = 0;;) {
        const std::size_t br = text.find("<br>", at);
        out.push_back(text.substr(at, br == std::string::npos ? std::string::npos : br - at));
        if (br == std::string::npos) break;
        at = br + 4;
    }
    return out;
}

class FortressMapWindow;

// CIFFortressMapFrame: the map's part of the window (drawn and clicked by the window).
class MapFrame : public ui::Widget {
public:
    explicit MapFrame(const assets::UiLayoutNode& node) : Widget(node) { interactive = true; }
    void Draw(ui::Frame& frame) override;
    void Mouse(ui::Frame& frame) override;
};

class FortressMapWindow : public ui::MainFrame {
public:
    FortressMapWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
        if (const assets::UiLayout* l = ui.Layout(kLayout)) {
            for (const auto& s : l->sections) {
                if (s.name != "StructureInfo") continue;
                for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) Add(ui.Make(*it));
            }
        }
        if (ui::Widget* b = Child(kBuild)) b->SetText(c_.Text("UIIT_CTL_FORT_BATTLEAIDE_CONSTRUCTION"));
        if (ui::Widget* b = Child(kRepair)) b->SetText(c_.Text("UIIT_CTL_WK_ETC_REPAIR"));
        if (ui::Widget* b = Child(kUpgrade)) b->SetText(c_.Text("UIIT_STT_FORT_BATTLEAIDE_UPGRADE"));
    }

    void Update(ui::Frame& frame) override {
        MainFrame::Update(frame);
        if (c_.fortress == nullptr || c_.fortress->Revision() == seen_) return;
        seen_ = c_.fortress->Revision();
        const proto::FortressStructureList& list = c_.fortress->StructureList();
        if (list.fortress != fortress_) {
            fortress_ = list.fortress;
            selected_ = 0;
            Centre(list);
        }
        Fill();
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr && n.source->parent() == this) {
            if (n.control == kRepair || n.control == kUpgrade || n.control == kBuild) {
                Press(n.control);
                return true;
            }
        }
        return MainFrame::OnNotice(n);
    }

    // The map (its labels with it), then an icon on each place.
    void DrawMap(ui::Frame& frame, const ui::Rect& at) {
        if (c_.fortress == nullptr) return;
        const ui::Rect view{at.x + kViewX, at.y + kViewY, kViewW, kViewH};
        view_.Draw(frame.paint, view, false);
        view_.DrawLabels(frame.paint, view, [this](const std::string& key) { return c_.Text(key); });
        frame.paint.PushClip(view);
        for (const proto::FortressStructureRecord& r : c_.fortress->StructureList().structures) {
            const auto p = Place(r);
            if (!p) continue;
            const ui::Vec2 s = view_.ToScreen(*p, view);
            if (r.zone == selected_) frame.paint.Image("icon\\strut_effect.ddj", {std::floor(s.x - 20.0f), std::floor(s.y - 20.0f), 40.0f, 40.0f});
            const std::string icon = Icon(r);
            if (!icon.empty()) frame.paint.Image(icon, {std::floor(s.x - 12.0f), std::floor(s.y - 12.0f), 24.0f, 24.0f});
        }
        frame.paint.PopClip();
    }
    // The left button drags the map (the world map's); a click (no drag) on an icon selects it (not a
    // war camp), anywhere else clears the selection.
    void MapMouse(ui::Frame& frame, const ui::Widget& owner, const ui::Rect& at) {
        if (c_.fortress == nullptr) return;
        const ui::Rect view{at.x + kViewX, at.y + kViewY, kViewW, kViewH};
        bool pressed = false;
        view_.Drag(frame, owner, view, &pressed);
        if (pressed) pressedAt_ = frame.pointer.position;
        if (frame.pointer.hovered == &owner) Tooltip(frame, view);
        if (!frame.input.Released(ui::MouseButton::Left) || frame.pointer.hovered != &owner) return;
        const ui::Vec2 mouse = frame.pointer.position;
        if (std::fabs(mouse.x - pressedAt_.x) >= 4.0f || std::fabs(mouse.y - pressedAt_.y) >= 4.0f) return; // a drag
        std::uint32_t picked = 0;
        for (const proto::FortressStructureRecord& r : c_.fortress->StructureList().structures) {
            const auto p = Place(r);
            if (!p) continue;
            const ui::Vec2 s = view_.ToScreen(*p, view);
            if (std::fabs(mouse.x - s.x) <= 12.0f && std::fabs(mouse.y - s.y) <= 12.0f && r.kind != std::uint8_t(game::StructureKind::Headquarters)) picked = r.zone;
        }
        selected_ = picked;
        Fill();
    }

private:
    // The structure under the mouse in a tooltip, the 1.188 client's lines in its order: the name ((MAX) without a next
    // level), "Current status : ...", "Upgraded : N step", "Durability : hp/most", "Defense power : N"; with a next level
    // "Next level condition", "Required level N", "Upgrade cost N Gold", "Upgrading GP N GP" and, while it waits, "Allowed
    // after H hours and M minutes"; between them "Features:" and its skill's description (the structure's DefaultSkill_1:
    // SN_SKILL_STRUCTURE_*_TT_DESC). Ours: no installation guild (a camp's guild is not sent).
    void Tooltip(ui::Frame& frame, const ui::Rect& view) {
        const ui::Vec2 mouse = frame.pointer.position;
        for (const proto::FortressStructureRecord& r : c_.fortress->StructureList().structures) {
            const auto p = Place(r);
            if (!p) continue;
            const ui::Vec2 s = view_.ToScreen(*p, view);
            if (std::fabs(mouse.x - s.x) > 12.0f || std::fabs(mouse.y - s.y) > 12.0f) continue;
            const assets::ObjectRow* row = Row(r);
            if (row == nullptr) return;
            std::vector<ui::TooltipLine> lines;
            const auto line = [&](std::string text) { lines.push_back({std::move(text)}); };
            const bool next = HasNext(r);
            line(c_.Text(row->nameKey) + (next ? std::string() : " " + c_.Text("UIIT_STT_FORT_BATTLEAIDE_UPGRADE_MAX")));
            std::string state;
            if (r.hp > 0 && r.hp >= r.maxHp) {
                state = c_.Text("UIIT_STT_FORT_BATTLEAIDE_STRUCTSTATUS_01");
            } else {
                state = c_.Text(r.hp == 0 || (r.state & 1) != 0 ? "UIIT_STT_FORT_BATTLEAIDE_STRUCTSTATUS_03" : "UIIT_STT_FORT_BATTLEAIDE_STRUCTSTATUS_02");
            }
            line(c_.Text("UIIT_MSG_GUILD_CONDITION") + " : " + state);
            if (r.kind != std::uint8_t(game::StructureKind::Headquarters)) {
                line(c_.Text("UIIT_STT_FORT_BATTLEAIDE_UPGRADE") + " : " + FormatText(c_.Text("UIIT_STT_RANK"), {std::to_string(row->level)}));
            }
            line(c_.Text("PARAM_DUR") + " : " + std::to_string(r.hp) + "/" + std::to_string(r.maxHp));
            line(c_.Text("UIIT_STT_FORT_BATTLEAIDE_DEFENSE") + " : " + std::to_string(int(row->phyDefence)));
            if (const assets::SkillRow* skill = c_.skills != nullptr && row->skills[0] != 0 ? c_.skills->Find(row->skills[0]) : nullptr;
                skill != nullptr && !skill->descKey.empty() && c_.hasText && c_.hasText(skill->descKey)) {
                line(c_.Text("UIIT_STT_FORT_BATTLEAIDE_FEATURE"));
                AddTooltipDescription(lines, c_.Text(skill->descKey));
            }
            if (next && r.kind != std::uint8_t(game::StructureKind::Headquarters)) {
                line(c_.Text("PARAM_CONDITION_OF_NEXT_LEVEL"));
                if (r.upgradeLevel != 0) line(c_.Text("PARAM_REQ_LV") + " " + std::to_string(r.upgradeLevel));
                line(c_.Text("UIIT_STT_FORT_BATTLEAIDE_UPGRADE_GOLD") + " " + std::to_string(r.upgradeGold) + " " + c_.Text("UIIT_STT_GOLD"));
                line(c_.Text("UIIT_STT_FORT_BATTLEAIDE_UPGRADE_GP") + " " + std::to_string(r.upgradePoints) + " " + c_.Text("UIIT_STT_GUILD_POINT_GP"));
                if (r.waitMinutes != 0) {
                    line(FormatText(c_.Text("UIIT_STT_FORT_BATTLEAIDE_UPGRADE_TIME"), {std::to_string(r.waitMinutes / 60), std::to_string(r.waitMinutes % 60)}));
                }
            }
            frame.interface.ShowTooltip(std::move(lines), {std::floor(s.x - 12.0f), std::floor(s.y - 12.0f), 24.0f, 24.0f}, {});
            return;
        }
    }
    // Where a place is on the map (the world map's conversion).
    std::optional<ui::Vec2> Place(const proto::FortressStructureRecord& r) const { return view_.Pixel(r.region, r.x, r.z); }
    // The fortress's map shown, centred on the player when it stands on it (the world map's way), else on the
    // places (ours).
    void Centre(const proto::FortressStructureList& list) {
        if (c_.worldMap == nullptr || !view_.Show(*c_.worldMap, MapOf(list.fortress))) {
            view_.Clear();
            return;
        }
        view_.SetViewSize({kViewW, kViewH});
        if (const game::WorldView::Entity* self = c_.world != nullptr ? c_.world->Find(c_.world->Self()) : nullptr) {
            if (const auto p = view_.Pixel(self->position.region, self->position.x, self->position.z);
                p && p->x >= 0 && p->y >= 0 && p->x <= view_.Width() && p->y <= view_.Height()) {
                view_.CentreOn(*p);
                return;
            }
        }
        ui::Vec2 sum{0, 0};
        int n = 0;
        for (const proto::FortressStructureRecord& r : list.structures) {
            if (const auto p = Place(r)) {
                sum.x += p->x;
                sum.y += p->y;
                ++n;
            }
        }
        if (n > 0) view_.CentreOn({sum.x / float(n), sum.y / float(n)});
    }
    const assets::ObjectRow* Row(const proto::FortressStructureRecord& r) const {
        return c_.characters != nullptr ? c_.characters->Find(r.refObjId) : nullptr;
    }
    std::string Icon(const proto::FortressStructureRecord& r) const { return FortressStructureIcon(c_, r); }
    const proto::FortressStructureRecord* Selected() const {
        if (c_.fortress == nullptr) return nullptr;
        for (const proto::FortressStructureRecord& r : c_.fortress->StructureList().structures) {
            if (r.zone == selected_ && selected_ != 0) return &r;
        }
        return nullptr;
    }
    bool HasNext(const proto::FortressStructureRecord& r) const {
        return std::any_of(r.upgrades.begin(), r.upgrades.end(), [](std::uint32_t id) { return id != 0; });
    }

    // The panel and the buttons.
    void Fill() {
        const proto::FortressStructureRecord* r = Selected();
        const assets::ObjectRow* row = r != nullptr ? Row(*r) : nullptr;
        const auto put = [&](int id, const std::string& text) {
            if (ui::Widget* w = Child(id)) w->SetText(text);
        };
        if (r == nullptr || row == nullptr) {
            for (int id : {kName, kState, kStep, kDefence, kDurability, kFeature}) put(id, {});
        } else {
            put(kName, c_.Text(row->nameKey) + (HasNext(*r) ? std::string() : " " + c_.Text("UIIT_STT_FORT_BATTLEAIDE_UPGRADE_MAX")));
            std::string state;
            if (r->hp > 0 && r->hp >= r->maxHp) {
                state = c_.Text("UIIT_STT_FORT_BATTLEAIDE_STRUCTSTATUS_01");
            } else if (row->level != 0 && row->phyDefence != 0.0f) {
                state = c_.Text(r->hp == 0 || (r->state & 1) != 0 ? "UIIT_STT_FORT_BATTLEAIDE_STRUCTSTATUS_03" : "UIIT_STT_FORT_BATTLEAIDE_STRUCTSTATUS_02");
            }
            put(kState, state);
            put(kStep, FormatText(c_.Text("UIIT_STT_RANK"), {std::to_string(row->level)}));
            put(kDefence, std::to_string(int(row->phyDefence)));
            put(kDurability, std::to_string(r->hp) + "/" + std::to_string(r->maxHp));
            put(kFeature, {});
        }
        for (int id : {kFeatureLabel, kFeature}) {
            if (ui::Widget* w = Child(id)) w->visible = false; // the features (DefaultSkill_1) are not read yet
        }
        // Off without a selection, or unless the player commands the war or administers it (1 or 4).
        const proto::GuildMemberRecord* me = c_.guild != nullptr ? c_.guild->Self() : nullptr;
        const bool allowed = r != nullptr && row != nullptr && me != nullptr && (me->position & (game::kPositionCommander | game::kPositionWar)) != 0;
        const bool foundation = r != nullptr && r->kind == std::uint8_t(game::StructureKind::Defensive) && row != nullptr && row->level == 0;
        const auto enable = [&](int id, bool on) {
            if (ui::Widget* w = Child(id)) w->enabled = on;
        };
        enable(kBuild, allowed && foundation);
        enable(kRepair, allowed && !foundation && (r->hp < r->maxHp || (r->state & 1) != 0));
        enable(kUpgrade, allowed && !foundation && r->hp >= r->maxHp && (r->state & 1) == 0 && HasNext(*r));
    }

    // A button: its question, then the request (the upgrade of a level-1 guard tower asks its element first).
    void Press(int button) {
        const proto::FortressStructureRecord* r = Selected();
        const assets::ObjectRow* row = r != nullptr ? Row(*r) : nullptr;
        if (r == nullptr || row == nullptr || c_.hud == nullptr || c_.fortress == nullptr) return;
        const std::string name = c_.Text(row->nameKey);
        const std::uint32_t npc = c_.fortress->StructureList().npc, fortress = fortress_, zone = r->zone;
        HudContext* ctx = &c_;
        if (button == kRepair) {
            const std::vector<std::string> lines = Lines(FormatText(c_.Text("UIIT_MSG_FORT_BATTLEAIDE_REPAIR_WINDOW"), {name, std::to_string(r->repairCost)}));
            // The original's box 0x27: "Repair of structures" on the name strip.
            if (ConfirmBox* box = c_.hud->Confirm("UIIT_STT_AGREEMENT_BOX", lines, [ctx, npc, fortress, zone](bool yes) {
                    if (yes && ctx->fortressRequest) ctx->fortressRequest(proto::FortressRepair::kId, proto::Encode(proto::FortressRepair{npc, fortress, zone}));
                })) {
                box->Header("UIIT_STT_FORT_BATTLEAIDE_REPAIR_TITLE");
            }
            return;
        }
        const std::string key = button == kBuild ? "UIIT_MSG_FORT_BATTLEAIDE_CONSTRUCTION_WINDOW" : "UIIT_MSG_FORT_BATTLEAIDE_UPGRADE_WINDOW";
        const std::vector<std::string> lines = Lines(FormatText(c_.Text(key), {name, std::to_string(r->upgradeGold), std::to_string(r->upgradePoints)}));
        // The original's boxes 0x28 (build) and 0x29 (upgrade): the structures' name (SN_FORTRESS_BATTLEAIDE_STRUCTURE) on
        // the name strip.
        const auto ask = [ctx, npc, fortress, zone, lines](std::uint8_t branch) {
            if (ConfirmBox* box = ctx->hud->Confirm("UIIT_STT_AGREEMENT_BOX", lines, [ctx, npc, fortress, zone, branch](bool yes) {
                    if (yes && ctx->fortressRequest) ctx->fortressRequest(proto::FortressUpgrade::kId, proto::Encode(proto::FortressUpgrade{npc, fortress, zone, branch}));
                })) {
                box->Header("SN_FORTRESS_BATTLEAIDE_STRUCTURE");
            }
        };
        if (button == kUpgrade && r->kind == std::uint8_t(game::StructureKind::GuardTower) && row->level == 1) {
            AskUpgradeType(c_, ask);
            return;
        }
        ask(0);
    }

    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::uint64_t seen_ = ~0ull;
    std::uint32_t fortress_ = 0, selected_ = 0;
    MapView view_;
    ui::Vec2 pressedAt_{-100.0f, -100.0f};

public:
    static void AskUpgradeType(HudContext& c, std::function<void(std::uint8_t)> answer);
};

void MapFrame::Draw(ui::Frame& frame) {
    if (auto* window = dynamic_cast<FortressMapWindow*>(Root())) window->DrawMap(frame, ScreenRect());
}

void MapFrame::Mouse(ui::Frame& frame) {
    if (auto* window = dynamic_cast<FortressMapWindow*>(Root())) window->MapMouse(frame, *this, ScreenRect());
}

// CIFCheckConfirmWnd (ginterface.txt 101, resinfo\ifcheckconfirmwnd.txt): "Upgrade type", four rows Lightning, Blaze,
// Freezing, Darkness (fixed texts); its layout has two, the other two are made 30 lower and the window
// grows by 60. The first starts checked; OK answers the checked row's index, the branch.
class UpgradeTypeWindow : public ui::MainFrame {
public:
    UpgradeTypeWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
        static constexpr const char* kTexts[] = {"UIIT_STT_FORT_BATTLEAIDE_UPGRADE_LIGTHNING", "UIIT_STT_FORT_BATTLEAIDE_UPGRADE_BLAZE",
                                                 "UIIT_STT_FORT_BATTLEAIDE_UPGRADE_FREEZING", "UIIT_STT_FORT_BATTLEAIDE_UPGRADE_DARK"};
        // Rows 3 and 4: copies of the layout's two, 60 lower.
        for (int k = 0; k < 2; ++k) {
            for (int base : {720, 711}) {
                const ui::Widget* source = Child(base + k);
                if (source == nullptr) continue;
                assets::UiLayoutNode n = source->node();
                n.id = base + 2 + k;
                n.rect.y += 60.0f;
                Add(ui.Make(n));
            }
        }
        for (int id : {701, 702, 703}) {
            if (ui::Widget* w = Child(id)) w->rect.h += 60.0f;
        }
        for (int id : {704, 705}) {
            if (ui::Widget* w = Child(id)) w->rect.y += 60.0f;
        }
        rect.h += 60.0f;
        if (ui::Widget* title = Child(0)) title->SetText(c_.Text("UIIT_PAG_FORT_BATTLEAIDE_UPGRADETYPE"));
        for (int k = 0; k < 4; ++k) {
            if (ui::Widget* label = Child(711 + k)) label->SetText(c_.Text(kTexts[k]));
        }
        if (ui::Widget* ok = Child(704)) ok->SetText(c_.Text("UIIT_CTL_CONFIRM"));
        if (ui::Widget* cancel = Child(705)) cancel->SetText(c_.Text("UIIT_CTL_CANCEL"));
        Check(0);
    }

    void Ask(std::function<void(std::uint8_t)> answer) {
        answer_ = std::move(answer);
        Check(0);
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr || n.source->parent() != this) return MainFrame::OnNotice(n);
        if (n.control >= 720 && n.control <= 723) {
            Check(n.control - 720);
            return true;
        }
        if (n.control == 704 || n.control == 705) {
            auto answer = std::move(answer_);
            answer_ = nullptr;
            if (ui_ != nullptr) ui_->Close(this);
            if (n.control == 704 && answer) answer(std::uint8_t(checked_));
            return true;
        }
        return MainFrame::OnNotice(n);
    }

private:
    void Check(int row) {
        checked_ = row;
        for (int k = 0; k < 4; ++k) {
            if (auto* box = ui::As<ui::CheckBox>(Child(720 + k))) box->checked = k == row;
        }
    }

    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    int checked_ = 0;
    std::function<void(std::uint8_t)> answer_;
};

void FortressMapWindow::AskUpgradeType(HudContext& c, std::function<void(std::uint8_t)> answer) {
    if (c.hud == nullptr) return;
    if (auto* window = dynamic_cast<UpgradeTypeWindow*>(c.hud->Show(kTypeWindow))) window->Ask(std::move(answer));
}

} // namespace

void RegisterFortressMapWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CIFFortressMap", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<FortressMapWindow>(n, *c); });
    ui.Register("CIFFortressMapFrame", [](const assets::UiLayoutNode& n) { return std::make_unique<MapFrame>(n); });
    ui.Register("CIFCheckConfirmWnd", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<UpgradeTypeWindow>(n, *c); });
}

void OpenFortressMap(HudContext& c) {
    if (c.hud != nullptr) c.hud->Show(kMapWindow);
}

} // namespace sr::client
