#include "gui/Hud.h"

#include <algorithm>
#include <cmath>

#include "gui/HudWindows.h"
#include "sr/core/Log.h"

namespace sr::client {

namespace {

constexpr const char* kMaster = "resinfo\\ginterface.txt";

// Shown when the game starts; the other windows of the Create section wait hidden until their
// key, button or the game opens them.
// (ginterface.txt's CIFUnderBar is made but not shown: 1.188 shows its CNIFUnderMenuBar there,.)
constexpr int kShownAtStart[] = {hud::kChat,       hud::kMinimap,        hud::kPlayerInfo,  hud::kBuffs,
                                 hud::kQuickParty, hud::kSystemMessages, hud::kDelayGauges, hud::kNotice,   hud::kWarning,
                                 hud::kQuestNotice, hud::kRegionView};

// Always in front, raised again in this order after every open and raise.
const std::vector<int> kOnTop = {hud::kSystemMenu, hud::kGameGuide, hud::kEventGuide, hud::kHelpBubble, hud::kNotice,
                                 hud::kWarning,    hud::kQuestNotice, hud::kEventGuideSecond};

// Escape closes the first of these that is shown (the dialogs it also closes come with their windows).
constexpr int kEscapeOrder[] = {hud::kSystemMenu, hud::kItemMall,  hud::kGameGuide, hud::kWorldMap,
                                hud::kStall,      hud::kExchange,  hud::kCommunity, hud::kEnchant,
                                hud::kMainPopup,  hud::kPetWindow, hud::kStallNetwork, hud::kPartyMatch};

} // namespace

const assets::UiLayoutNode* Hud::NodeOf(int id, std::string* section) const {
    if (master_ == nullptr) return nullptr;
    for (const auto& s : master_->sections) {
        for (const auto& n : s.nodes) {
            if (n.id != id) continue;
            if (section) *section = s.name;
            return &n;
        }
    }
    return nullptr;
}

void Hud::Create(ui::Interface& ui, ui::Vec2 screen) {
    ui_ = &ui;
    master_ = ui.Layout(kMaster);
    if (master_ == nullptr) {
        core::LogWarning("interface: no {}; no HUD", kMaster);
        return;
    }
    const auto create = std::find_if(master_->sections.begin(), master_->sections.end(), [](const auto& s) { return s.name == "Create"; });
    if (create == master_->sections.end()) return;
    // 1.188's underbar (CNIFUnderMenuBar, res_ui\nifundermenubar.2dt), made first so the others stand in
    // front of it; it builds itself from its layout (MenuBar::Created).
    assets::UiLayoutNode bar;
    bar.type = "CNIFUnderMenuBar";
    bar.name = "CNIFUnderMenuBar";
    bar.id = hud::kMenuBar;
    bar.rect = {0.0f, 0.0f, 800.0f, 68.0f};
    ui.Open(bar);
    // The HUD manager is the parent of these windows: its first node is in front, so the last is
    // made first.
    for (auto it = create->nodes.rbegin(); it != create->nodes.rend(); ++it) {
        if (it->id == hud::kDragged || it->id == hud::kLoading) continue; // the held item and the loading screen are drawn by others
        ui::Widget* w = ui.Open(*it);
        if (w == nullptr) continue;
        w->visible = std::find(std::begin(kShownAtStart), std::end(kShownAtStart), it->id) != std::end(kShownAtStart);
    }
    // The extra quick slots are always there.
    if (const assets::UiLayoutNode* extra = NodeOf(hud::kExtQuickSlots)) ui.Open(*extra);
    // Ours: the performance panel, collapsed, left of the minimap and the COS's icons (it places itself), the bug report's
    // button at its left.
    assets::UiLayoutNode performance;
    performance.type = "SRPerformancePanel";
    performance.name = "SRPerformancePanel";
    performance.id = hud::kPerformance;
    performance.rect = {0.0f, 0.0f, PerformancePanel::kWidth, PerformancePanel::kHeader};
    ui.Open(performance);
    ui.KeepOnTop(kOnTop);
    Place(screen);
}

void Hud::Sync() {
    if (ui_ == nullptr || state_ == nullptr) return;
    // A new target shows the window (its close button only hides it until the next one); none
    // hides it.
    if (state_->target.entity != shownTarget_) {
        shownTarget_ = state_->target.entity;
        if (shownTarget_ != 0) {
            ShowWithInterface(hud::kTarget); // under F12, with the rest on its return
        } else {
            Hide(hud::kTarget);
        }
    }
    // The NPC window's close closes the fortress NPCs' windows opened beside it: the tax window (300), the
    // structure map (301), the jobs (302), the official's (303), the forge (304), the guild's introduction (185) and its
    // GP history (186).
    const ui::Widget* npc = ui_->Window(hud::kNpc);
    const bool npcShown = npc != nullptr && npc->visible;
    if (npcShown_ && !npcShown) {
        for (int id : {300, 301, 302, 303, 304, 185, 186}) Hide(id);
    }
    npcShown_ = npcShown;
}

void Hud::UseQuickSlot(int index, double now, bool frame) {
    if (state_ != nullptr && frame && index >= 1 && index <= game::QuickSlots::kPages * game::QuickSlots::kPerPage) {
        state_->quickSlots.usedColumn = (index - 1) % game::QuickSlots::kPerPage;
        state_->quickSlots.usedAt = now;
    }
    Request({HudRequest::Kind::UseQuickSlot, index});
}

void Hud::Place(ui::Vec2 screen) {
    // A minimised window's 0 x 0 (an Alt+Tab out of full screen): nothing placed for it. Pulled onto that screen
    // every window went to the left of and above it, and the ones kept where they stand (the player's bars, the item
    // mall's button) never came back on the restore (the user's report 2026-11-22).
    if (screen.x < 64.0f || screen.y < 64.0f) return;
    screen_ = screen;
    if (ui_ == nullptr) return;
    const float W = screen.x, H = screen.y;
    // As the 1.188 client places them where they differ from the older one (the main window higher, over
    // 1.188's underbar). The rest keep ginterface.txt's place from the top left.
    for (ui::Widget* w : ui_->Windows()) {
        ui::Rect& r = w->rect;
        switch (w->id()) {
        case hud::kMinimap: r.x = W - 129.0f, r.y = 0.0f; break;
        case hud::kPerformance: break; // ours: it places itself, left of the minimap and the COS out (PerformancePanel::Update)
        case hud::kTimer: r.x = W - 129.0f - r.w, r.y = 68.0f; break;
        case hud::kMainPopup: r.x = W - 44.0f - r.w, r.y = H - 200.0f - r.h; break; // 1.188 (1.150: H - 96)
        case hud::kUnderBar: r.x = std::floor((W - 800.0f) / 2.0f), r.y = H - 52.0f; break;
        case hud::kMenuBar: r.x = std::floor((W - 800.0f) / 2.0f), r.y = H - 68.0f; break;
        case hud::kQuestQuick: r.x = W - 324.0f; break; //, under the minimap, its y kept
        case hud::kDurability: r.x = W - 42.0f, r.y = 224.0f; break;
        case hud::kDelayGauges: r.x = std::floor((W - 192.0f) / 2.0f), r.y = H - 165.0f; break;
        case hud::kExtQuickSlots: r.x = W - 26.0f - r.w, r.y = 181.0f; break;
        case hud::kBuffs: r.x = 218.0f, r.y = 10.0f; break;
        case hud::kStore:
        case hud::kStorage:
        case hud::kExchange:
        case hud::kCommunity:
        case hud::kStall:
        case hud::kSystemMenu:
        case hud::kNpc:
        case hud::kGameGuide:
        case hud::kPractice:
        case hud::kWorldMap: ui_->Centre(*w, screen); break;
        default: break;
        }
    }
    LayoutGuides();
    // The chat stands on the underbar at the left edge, by its size (ChatWindow::SetLevel).
    if (auto* chat = dynamic_cast<ChatWindow*>(ui_->Window(hud::kChat))) chat->SetLevel(chat->level());
    ui_->PullOntoScreen(screen);
}

void Hud::Confirm(const std::string& titleKey, const std::vector<std::string>& lines, std::function<void(bool yes)> answer) {
    if (ui_ == nullptr) return;
    // The message box's frame and size (ours: the size of each kind is set in code, not traced;
    // this one holds the layout's dark tile, 16, 40, 284 x 122).
    assets::UiLayoutNode node;
    node.type = "CIFMessageBox";
    node.name = "GDR_MSGBOX";
    node.ddj = "interface\\messagebox\\msgbox2_window_";
    node.text = titleKey;
    node.rect = {0.0f, 0.0f, 316.0f, 178.0f};
    ui::Widget* w = ui_->Open(node);
    auto* box = dynamic_cast<ConfirmBox*>(w);
    if (box == nullptr) return;
    ui_->Centre(*box, screen_);
    box->Ask(lines, std::move(answer));
}

void Hud::ReverseReturn(std::function<void(int)> answer) {
    if (ui_ == nullptr) return;
    for (ui::Widget* w : ui_->Windows()) {
        if (dynamic_cast<ReverseReturnBox*>(w) != nullptr && w->visible) ui_->Close(w); // one at a time
    }
    assets::UiLayoutNode node;
    node.type = "CIFMessageBox";
    node.name = "GDR_MSGBOX_REVERSE_RETURN";
    node.ddj = "interface\\messagebox\\msgbox2_window_";
    node.text = "UIIT_STT_CONFIRM_BOX";
    node.subSection = "ReverseReturn";
    node.rect = {0.0f, 0.0f, 300.0f, 204.0f}; // kind 0x21's
    auto* box = dynamic_cast<ReverseReturnBox*>(ui_->Open(node));
    if (box == nullptr) return;
    ui_->Centre(*box, screen_);
    box->Ask(std::move(answer));
}

void Hud::Divide(const std::string& icon, const std::string& name, int count, std::function<void(int)> answer) {
    if (ui_ == nullptr) return;
    assets::UiLayoutNode node;
    node.type = "CIFMessageBox";
    node.name = "GDR_MSGBOX_DIVIDE";
    node.ddj = "interface\\messagebox\\msgbox2_window_";
    node.text = "UIIT_STT_CONFIRM_BOX";
    node.subSection = "MsgBoxDivideCount"; // DivideBox's part of ifmessagebox.txt
    node.rect = {0.0f, 0.0f, 300.0f, 185.0f}; // ours: its size is set in code, not traced
    auto* box = dynamic_cast<DivideBox*>(ui_->Open(node));
    if (box == nullptr) return;
    ui_->Centre(*box, screen_);
    box->Ask(icon, name, count, std::move(answer));
}

void Hud::Deal(const DealBox::Deal& deal) {
    if (ui_ == nullptr) return;
    for (ui::Widget* w : ui_->Windows()) {
        if (dynamic_cast<DealBox*>(w) != nullptr && w->visible) ui_->Close(w); // one at a time
    }
    // ConfirmBuy 327 x 155 and one price row (22) for buying; MsgBoxStoreConfirm resized to 327 x 175
    // for selling and buying back.
    assets::UiLayoutNode node;
    node.type = "CIFStoreDeal";
    node.name = "GDR_STORE_DEAL";
    node.ddj = "interface\\messagebox\\msgbox2_window_";
    node.subSection = deal.kind == DealBox::Deal::Kind::Buy ? "Buy" : deal.kind == DealBox::Deal::Kind::StallRegister ? "StallRegister" : "Confirm";
    node.text = deal.kind == DealBox::Deal::Kind::Buy ? "" : "UIIT_STT_CONFIRM_BOX";
    node.rect = {0.0f, 0.0f, 327.0f, deal.kind == DealBox::Deal::Kind::Buy ? 177.0f : 175.0f};
    auto* box = dynamic_cast<DealBox*>(ui_->Open(node));
    if (box == nullptr) return;
    ui_->Centre(*box, screen_);
    box->Set(deal);
}

void Hud::StorageGold(bool deposit, bool guild) {
    if (ui_ == nullptr) return;
    for (ui::Widget* w : ui_->Windows()) {
        if (dynamic_cast<GoldBox*>(w) != nullptr && w->visible) return; // one at a time
    }
    assets::UiLayoutNode node;
    node.type = "CIFStorageGold";
    node.name = "GDR_STORAGE_GOLD";
    node.ddj = "interface\\messagebox\\msgbox2_window_";
    node.text = "UIIT_STT_CONFIRM_BOX";
    node.id = (deposit ? 1 : 2) + (guild ? 3 : 0); // 1, 2 the account's; 4, 5 the guild's
    node.rect = {0.0f, 0.0f, 308.0f, 148.0f}; // the money box
    if (ui::Widget* box = ui_->Open(node)) ui_->Centre(*box, screen_);
}

void Hud::ExchangeGoldBox() {
    if (ui_ == nullptr) return;
    for (ui::Widget* w : ui_->Windows()) {
        if (dynamic_cast<GoldBox*>(w) != nullptr && w->visible) return;
    }
    assets::UiLayoutNode node;
    node.type = "CIFStorageGold";
    node.name = "GDR_EXCHANGE_GOLD";
    node.ddj = "interface\\messagebox\\msgbox2_window_";
    node.text = "UIIT_STT_CONFIRM_BOX";
    node.id = 6; // GoldBox: the exchange's
    node.rect = {0.0f, 0.0f, 308.0f, 148.0f};
    if (ui::Widget* box = ui_->Open(node)) ui_->Centre(*box, screen_);
}

void Hud::DropGoldBox() {
    if (ui_ == nullptr) return;
    for (ui::Widget* w : ui_->Windows()) {
        if (dynamic_cast<GoldBox*>(w) != nullptr && w->visible) return;
    }
    assets::UiLayoutNode node;
    node.type = "CIFStorageGold";
    node.name = "GDR_DROP_GOLD";
    node.ddj = "interface\\messagebox\\msgbox2_window_";
    node.text = "UIIT_STT_CONFIRM_BOX";
    node.id = 3; // GoldBox: on the ground
    node.rect = {0.0f, 0.0f, 308.0f, 148.0f};
    if (ui::Widget* box = ui_->Open(node)) ui_->Centre(*box, screen_);
}

void Hud::Rebirth(int level, std::function<void(int kind)> answer, const std::vector<std::string>& warLines, double riseAt, std::string countdown,
                  const std::string& firstButton) {
    if (ui_ == nullptr || RebirthShown()) return;
    assets::UiLayoutNode node;
    node.type = "CIFMessageBox";
    node.name = "GDR_MSGBOX_REBIRTH";
    node.ddj = "interface\\messagebox\\msgbox2_window_";
    node.text = "UIIT_STT_CONFIRM_BOX";
    node.subSection = "Rebirth"; // RebirthBox
    node.rect = {std::floor(screen_.x / 2.0f) - 154.0f, std::floor(screen_.y / 2.0f) - 74.0f, 400.0f, warLines.empty() ? 210.0f : 242.0f};
    if (auto* box = dynamic_cast<RebirthBox*>(ui_->Open(node))) {
        box->Ask(level, std::move(answer));
        if (!warLines.empty()) box->WarLines(warLines, riseAt, std::move(countdown));
        if (!firstButton.empty()) {
            if (ui::Widget* b = box->Child(200)) b->SetText(ui_->resources().Text(firstButton));
        }
    }
}

void Hud::CloseRebirth() {
    if (ui_ == nullptr) return;
    for (ui::Widget* w : ui_->Windows()) {
        if (auto* box = dynamic_cast<RebirthBox*>(w)) ui_->Close(box);
    }
}

bool Hud::PressRebirth(int button) {
    if (ui_ == nullptr) return false;
    for (ui::Widget* w : ui_->Windows()) {
        auto* box = dynamic_cast<RebirthBox*>(w);
        if (box == nullptr || !box->visible) continue;
        if (ui::Widget* b = box->Child(button)) return box->OnNotice({ui::Notice::Kind::Clicked, button, b, 0});
    }
    return false;
}

bool Hud::RebirthShown() const {
    if (ui_ == nullptr) return false;
    for (ui::Widget* w : ui_->Windows()) {
        if (dynamic_cast<RebirthBox*>(w) != nullptr && w->visible) return true;
    }
    return false;
}

void Hud::Practice(const SkillPracticeBox::Question& q, std::function<void()> confirmed) {
    if (auto* box = dynamic_cast<SkillPracticeBox*>(Show(hud::kPractice))) box->Ask(q, std::move(confirmed));
}

bool Hud::AnswerConfirm(bool yes) {
    if (ui_ == nullptr) return false;
    for (ui::Widget* w : ui_->Windows()) {
        if (auto* box = dynamic_cast<SkillPracticeBox*>(w); box != nullptr && box->visible) {
            box->Answer(yes);
            return true;
        }
    }
    for (ui::Widget* w : ui_->Windows()) {
        if (auto* box = dynamic_cast<ConfirmBox*>(w); box != nullptr && box->visible) {
            box->Answer(yes);
            return true;
        }
    }
    return false;
}

bool Hud::Shown(int id) const { return ui_ != nullptr && ui_->Window(id) != nullptr; }

// The side icons' ids: the letters' envelope, the quest info, the event guides, the temples' board, the
// server's event, the Open Market's.
bool Hud::IsGuide(int id) {
    return id == hud::kLetterAlarm || id == 46 || id == hud::kEventGuide || id == hud::kTempleGuide || id == 179 || id == 198 ||
           id == hud::kEventGuideSecond;
}

// from the manager's place (W - 6, 180): each icon 42 to the left of the one before, four to
// a row, the next row 44 lower from the right again.
void Hud::LayoutGuides() {
    if (ui_ == nullptr) return;
    std::erase_if(guides_, [&](int id) { return ui_->Window(id) == nullptr; });
    const float right = screen_.x - 6.0f;
    float x = right, y = 180.0f;
    int n = 1;
    for (const int id : guides_) {
        ui::Widget* w = ui_->Window(id);
        if (w == nullptr || !w->visible) continue;
        x -= 42.0f;
        w->rect.x = x;
        w->rect.y = y;
        if (n % 4 == 0) {
            y += 44.0f;
            x = right;
        }
        ++n;
    }
}

ui::Widget* Hud::Show(int id) {
    if (ui_ == nullptr) return nullptr;
    const auto guide = [&](ui::Widget* w) {
        if (w != nullptr && IsGuide(id)) {
            w->visible = true; // a side icon's node is made hidden (style 0x40); the row's show puts it up
            if (std::find(guides_.begin(), guides_.end(), id) == guides_.end()) guides_.push_back(id);
            LayoutGuides();
        }
        return w;
    };
    for (ui::Widget* w : ui_->Windows()) {
        if (w->id() != id) continue;
        // A window of the Create section: shown where it was, in front.
        w->visible = true;
        ui_->Raise(w);
        return guide(w);
    }
    // Made on demand from its section, at its kept place or centred.
    std::string section;
    const assets::UiLayoutNode* node = NodeOf(id, &section);
    if (node == nullptr || section == "Create") return nullptr;
    ui::Widget* w = ui_->Open(*node);
    if (w != nullptr) ui_->Place(*w, screen_);
    return guide(w);
}

ui::Widget* Hud::ShowGuide(const assets::UiLayoutNode& node) {
    if (ui_ == nullptr) return nullptr;
    ui::Widget* w = ui_->Window(node.id);
    if (w == nullptr) w = ui_->Open(node);
    if (w == nullptr) return nullptr;
    w->visible = true;
    if (std::find(guides_.begin(), guides_.end(), node.id) == guides_.end()) guides_.push_back(node.id);
    LayoutGuides();
    return w;
}

void Hud::ShowWithInterface(int id) {
    if (!f12Hidden_) {
        Show(id);
        return;
    }
    if (std::find(hiddenByF12_.begin(), hiddenByF12_.end(), id) == hiddenByF12_.end()) {
        // Made (not shown) now, so the next F12 has a window to show.
        if (ui::Widget* w = Show(id)) w->visible = false;
        hiddenByF12_.push_back(id);
    }
}

bool Hud::ShownOrWaiting(int id) const {
    return Shown(id) || (f12Hidden_ && std::find(hiddenByF12_.begin(), hiddenByF12_.end(), id) != hiddenByF12_.end());
}

void Hud::Hide(int id) {
    if (ui_ == nullptr) return;
    if (f12Hidden_) std::erase(hiddenByF12_, id); // put away meanwhile: not back with F12
    for (ui::Widget* w : ui_->Windows()) {
        if (w->id() != id || !w->visible) continue;
        std::string section;
        NodeOf(id, &section);
        if (section == "Create") {
            ui_->Remember(*w);
            w->visible = false;
        } else {
            ui_->Close(w); // made on demand: thrown away (its place kept)
        }
        break;
    }
    if (IsGuide(id)) {
        // Out of the side icons' row, the others closing up (then).
        std::erase(guides_, id);
        LayoutGuides();
    }
}

ui::Widget* Hud::Toggle(int id) {
    if (Shown(id)) {
        Hide(id);
        Sound(false);
        return nullptr;
    }
    ui::Widget* w = Show(id);
    if (w != nullptr) Sound(true);
    return w;
}

void Hud::Sound(bool opened) {
    // A window opened or closed by the player through the HUD manager.
    if (ui_ != nullptr) ui_->Sound(opened ? "SND_WINDOW_OPEN" : "SND_WINDOW_CLOSE");
}

void Hud::ShowPage(ui::Widget& main, int page) {
    // The pages are the windows nested in it; the inventory comes with the equipment beside it.
    for (const auto& c : main.children()) {
        const int id = c->id();
        if (id < hud::kPageInventory || id > hud::kPageAcademy) continue;
        c->visible = id == page || (page == hud::kPageInventory && id == hud::kPageEquipment);
    }
    // The background tile (3): a 9-pixel strip between the inventory and the equipment, the action
    // page's backdrop at its own place, hidden elsewhere; the title by the page.
    if (ui::Widget* tile = main.Child(3)) {
        tile->visible = page == hud::kPageInventory || page == hud::kPageActions;
        tile->rect = page == hud::kPageInventory ? ui::Rect{189, 68, 9, 292}
                                                 : ui::Rect{tile->node().rect.x, tile->node().rect.y, tile->node().rect.w, tile->node().rect.h};
    }
    constexpr std::pair<int, const char*> kTitles[] = {
        {hud::kPageCharacter, "UIIT_STT_CHARACTER"}, {hud::kPageInventory, "UIIT_STT_INVENTORY"}, {hud::kPageSkills, "UIIT_STT_SKILL"},
        {hud::kPageActions, "UIIT_STT_ACTION"},      {hud::kPageParty, "UIIT_STT_PARTY"},         {hud::kPageQuests, "UIIT_STT_QUEST"},
        {hud::kPageAcademy, "UIIT_CTL_TC_TRAININGCAMP"}};
    for (auto it = main.children().rbegin(); it != main.children().rend(); ++it) {
        if ((*it)->id() != 0 || ui::As<ui::Static>(it->get()) == nullptr || ui_ == nullptr) continue;
        for (const auto& [id, key] : kTitles) {
            if (id == page) (*it)->SetText(ui_->resources().Text(key));
        }
        break;
    }
    page_ = page;
}

void Hud::OpenPage(int page) {
    if (ui::Widget* main = Show(hud::kMainPopup)) ShowPage(*main, page);
}

void Hud::ToggleMain() {
    if (Shown(hud::kMainPopup)) {
        Hide(hud::kMainPopup);
        Sound(false);
    } else {
        OpenPage(page_);
        Sound(true);
    }
}

void Hud::TogglePage(int page) {
    // Shown on that page: hidden; otherwise shown on it.
    if (ui::Widget* main = ui_ ? ui_->Window(hud::kMainPopup) : nullptr; main != nullptr && page_ == page) {
        Hide(hud::kMainPopup);
        Sound(false);
        return;
    }
    const bool shown = Shown(hud::kMainPopup);
    if (ui::Widget* main = Show(hud::kMainPopup)) {
        ShowPage(*main, page);
        if (!shown) Sound(true);
    }
}

void Hud::OpenOptions() {
    if (ui_ == nullptr) return;
    // (The section opens as a plain window holding the CIFOption.)
    for (ui::Widget* w : ui_->Windows()) {
        const bool option = w->node().type == "CIFOption" ||
                            std::any_of(w->children().begin(), w->children().end(), [](const auto& c) { return c->node().type == "CIFOption"; });
        if (option && w->visible) {
            ui_->Raise(w);
            return;
        }
    }
    if (ui::Widget* w = ui_->Open("resinfo\\ifsystemwnd.txt", "OptionWnd")) ui_->Centre(*w, screen_);
}

void Hud::Escape() {
    if (ui_ != nullptr && Shown(hud::kCommunity) && CloseCommunityLetter(ui_->Window(hud::kCommunity))) return;
    if (ui_ == nullptr) return;
    // A picked-up item goes back first.
    if (ui_->picked() != nullptr) {
        ui_->Pick(nullptr);
        return;
    }
    for (int id : kEscapeOrder) {
        if (Shown(id)) {
            Hide(id);
            Sound(false);
            return;
        }
    }
    if (Show(hud::kSystemMenu) != nullptr) Sound(true);
}

void Hud::Keys(const ui::Context& context, double now) {
    if (ui_ == nullptr || context.typing() || context.keyboardBlocked()) return;
    const ui::Input& in = context.input();
    using ui::Key;
    // An open question first: Enter answers yes, Escape no (the message boxes' keys).
    if ((in.KeyPressed(Key::Enter) || in.KeyPressed(Key::Escape)) && AnswerConfirm(in.KeyPressed(Key::Enter))) return;
    if (in.KeyPressed(Key::Escape)) Escape();
    // Enter: the keyboard to the chat's input line.
    if (in.KeyPressed(Key::Enter)) {
        if (auto* chat = dynamic_cast<ChatWindow*>(ui_->Window(hud::kChat)); chat != nullptr && chat->visible) {
            chat->FocusInput();
        } else {
            Request({HudRequest::Kind::ChatInput});
        }
    }
    // The fixed keys.
    for (int i = 0; i < 4; ++i) {
        if (in.KeyPressed(Key(int(Key::F1) + i)) && state_ != nullptr) state_->quickSlots.page = i;
    }
    for (int i = 0; i < 10; ++i) {
        const Key digit = Key(i == 9 ? int('0') : int('1') + i);
        if (!in.KeyPressed(digit)) continue;
        if (in.ctrl()) {
            if (i < 8) Request({HudRequest::Kind::TargetPartyMember, i}); // Ctrl + 1: the player; 2..8: the party
        } else if (in.KeyDown(Key::Space)) {
            UseQuickSlot(game::QuickSlots::kExtraFirst + i, now, false); // the extra bar's
        } else {
            UseQuickSlot(game::QuickSlots::Index(state_ ? state_->quickSlots.page : 0, i), now);
        }
    }
    if (in.KeyPressed(Key::F9)) Request({HudRequest::Kind::NextView});
    if (in.KeyPressed(Key::F10)) onItemMall ? onItemMall() : void(Toggle(hud::kItemMall));
    if (in.KeyPressed(Key::F12)) {
        // Every shown window hidden; again: the same ones shown.
        if (!f12Hidden_) {
            hiddenByF12_.clear();
            for (ui::Widget* w : ui_->Windows()) {
                if (w->visible) {
                    hiddenByF12_.push_back(w->id());
                    w->visible = false;
                }
            }
        } else {
            for (ui::Widget* w : ui_->Windows()) {
                if (std::find(hiddenByF12_.begin(), hiddenByF12_.end(), w->id()) != hiddenByF12_.end()) w->visible = true;
            }
        }
        f12Hidden_ = !f12Hidden_;
    }
    // The keys the player binds (section 6.1).
    using A = ui::KeyAction;
    // The two keys that work while held say when they go down and when they come up.
    const auto held = [&](A action, bool& was, HudRequest::Kind kind) {
        const Key key = bindings_.Get(action);
        const bool down = key != Key::None && in.KeyDown(key);
        if (down != was) Request({kind, down ? 1 : 0});
        was = down;
    };
    held(A::ShowDroppedItems, showingDropped_, HudRequest::Kind::ShowDroppedItems);
    held(A::HideOthers, hidingOthers_, HudRequest::Kind::HideOthers);
    for (int k = 1; k < int(Key::Count); ++k) {
        const Key key = Key(k);
        if (!in.KeyPressed(key)) continue; // repeats ignored
        const auto action = bindings_.Find(key);
        if (!action) continue;
        switch (*action) {
        case A::Character: TogglePage(hud::kPageCharacter); break;
        case A::Inventory: TogglePage(hud::kPageInventory); break;
        case A::Skills: TogglePage(hud::kPageSkills); break;
        case A::Actions: TogglePage(hud::kPageActions); break;
        case A::Party: TogglePage(hud::kPageParty); break;
        case A::Quests: Request({HudRequest::Kind::ToggleQuests}); break; // 1.188: its own window (res_ui\questlist.2dt)
        case A::Academy: TogglePage(hud::kPageAcademy); break;
        case A::Community: Toggle(hud::kCommunity); break;
        case A::WorldMap: Toggle(hud::kWorldMap); break;
        case A::Help: Toggle(hud::kGameGuide); break;
        case A::AutoPotion: Toggle(hud::kAutoPotion); break;
        case A::PartyMatching: Toggle(hud::kPartyMatch); break;
        case A::Alchemy: Request({HudRequest::Kind::ToggleAlchemy}); break; // 1.188: its own window (res_ui\nifenchantwnd.2dt)
        case A::StallNetwork: Toggle(hud::kStallNetwork); break;
        case A::Berserk: Request({HudRequest::Kind::Berserk}); break;
        case A::ShowDroppedItems:
        case A::HideOthers: break; // above
        case A::RightButtonSkill: UseQuickSlot(game::QuickSlots::kRightButton, now, false); break;
        case A::SitStand: Request({HudRequest::Kind::SitStand}); break;
        case A::PickUp: Request({HudRequest::Kind::PickUp}); break;
        case A::PetInfo: Request({HudRequest::Kind::PetCommand, 0}); break;
        case A::PetRide: Request({HudRequest::Kind::PetCommand, 1}); break;
        case A::PetAttack: Request({HudRequest::Kind::PetCommand, 2}); break;
        case A::PetFollow: Request({HudRequest::Kind::PetCommand, 3}); break;
        case A::PetBehaviour: Request({HudRequest::Kind::PetCommand, 6}); break;
        case A::PetUnsummon: Request({HudRequest::Kind::PetCommand, 100}); break;
        case A::NextPet: Request({HudRequest::Kind::PetCommand, 101}); break;
        case A::Reply:
            // The last whisper partner into the chat's input line.
            if (auto* chat = dynamic_cast<ChatWindow*>(ui_->Window(hud::kChat))) chat->Reply();
            break;
        case A::TargetEnemyPlayer: Request({HudRequest::Kind::TargetEnemyPlayer}); break;
        case A::LastTarget: Request({HudRequest::Kind::LastTarget}); break;
        case A::AssistTarget: Request({HudRequest::Kind::AssistTarget}); break;
        case A::FaceTarget: Request({HudRequest::Kind::FaceTarget}); break;
        case A::HideAllies: Request({HudRequest::Kind::HideAllies}); break;
        case A::HideEnemies: Request({HudRequest::Kind::HideEnemies}); break;
        case A::Count: break;
        }
    }
}

} // namespace sr::client
