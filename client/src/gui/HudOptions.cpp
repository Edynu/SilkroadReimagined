// The Option window (CIFOption, the system menu's Option button):
// five pages over a draft of the player's options, and two of ours (Online: how many players
// and monsters the server sends, up to its own shares; Beta: the experiments a player may
// choose, the movement's pathfinding first); Default resets the
// page shown, Confirm keeps everything (saved, applied), Cancel drops the draft (the audio heard
// while moving the bars goes back), Apply keeps the video page without closing. The original's
// rows first; our Modern rows below them.

#include <algorithm>
#include <cmath>
#include <format>

#include "gui/Hud.h"
#include "gui/HudWindows.h"

namespace sr::client {

namespace {

constexpr const char* kTabNames[5] = {"UIIT_CTL_MENU_VIDEOSET", "UIIT_CTL_MENU_AUDIOSET", "UIIT_CTL_MENU_CAMERASET", "UIIT_CTL_MENU_INPUTSET",
                                      "UIIT_CTL_MENU_GAMESET"};

// Each page: the window's height, its background tile, the row of the buttons and the x of Default,
// Confirm and Cancel (Apply at 281 on the video page only). Online is ours: the
// audio page's three boxes and a fourth (64 more than its geometry).
struct Geometry {
    float height;
    ui::Rect tile;
    float row;
    float x[3];
};
constexpr int kPages = 7;
constexpr Geometry kGeometry[kPages] = {
    {413, {27, 130, 332, 230}, 379, {29, 113, 197}}, {319, {27, 100, 332, 165}, 285, {52, 155, 258}},
    {312, {27, 78, 332, 180}, 278, {52, 155, 258}},  {413, {27, 103, 332, 256}, 379, {52, 155, 258}},
    {415, {27, 78, 332, 281}, 380, {52, 155, 258}},  {383, {27, 100, 332, 229}, 349, {52, 155, 258}},
    {319, {27, 100, 332, 165}, 285, {52, 155, 258}},
};
constexpr int kOnlinePage = 5;
constexpr int kBetaPage = 6; // ours: the audio page's geometry, one box
// The original's labels on the option pages (ifoption_audio.txt's FontColor).
constexpr ui::Color kLabelColor = ui::Rgba(239, 218, 163);

bool Inside(ui::Rect r, ui::Vec2 p) { return p.x >= r.x && p.y >= r.y && p.x <= r.Right() && p.y <= r.Bottom(); }

ui::Widget* MakeNode(ui::Interface& ui, ui::Widget& parent, const std::string& type, int id, ui::Rect rect, const std::string& ddj = {},
                     int hAlign = 0) {
    assets::UiLayoutNode n;
    n.type = type;
    n.name = "GDR_OPTION_MADE";
    n.id = id;
    n.rect = {rect.x, rect.y, rect.w, rect.h};
    n.ddj = ddj;
    n.hAlign = hAlign;
    auto made = ui.Make(n);
    ui::Widget* w = made.get();
    parent.Add(std::move(made));
    return w;
}

// Our Video rows (Modern): a field of the video options and its choices.
struct FieldRow {
    int core::VideoOptions::*field;
    const char* label;
    std::vector<const char*> entries;
};
const std::vector<FieldRow>& FieldRows() {
    static const std::vector<FieldRow> kRows = {
        {&core::VideoOptions::windowMode, "Window", {"Windowed", "Borderless", "Full screen"}},
        {&core::VideoOptions::antiAliasing, "Anti-aliasing", {"Off", "FXAA", "MSAA 2x", "MSAA 4x", "MSAA 8x"}},
        {&core::VideoOptions::anisotropy, "Texture filtering", {"Trilinear", "Anisotropic 2x", "Anisotropic 4x", "Anisotropic 8x", "Anisotropic 16x"}},
        {&core::VideoOptions::sunShadows, "Sun shadows", {"Original", "Low", "Medium", "High", "Ultra"}},
        {&core::VideoOptions::shadowDistance, "Shadow distance", {"Near", "Medium", "Far", "Very far"}},
        {&core::VideoOptions::shadowCulling, "Shadow casters", {"All", "Balanced", "Fast"}},
        {&core::VideoOptions::lightEffects, "Lights from effects", {"Original", "Medium", "High"}},
        {&core::VideoOptions::lightReach, "Light reach", {"Near", "Medium", "Far"}},
        {&core::VideoOptions::ambientOcclusion, "Ambient occlusion", {"Off", "Low", "High"}},
        {&core::VideoOptions::water, "Water", {"Original", "Modern"}},
        {&core::VideoOptions::lensFlare, "Sun", {"Off", "Original", "Modern (light shafts)"}},
        {&core::VideoOptions::bloom, "Glow", {"Off", "Original", "Modern"}},
        {&core::VideoOptions::toneMapping, "Tone mapping", {"Original", "Filmic (ACES)", "AgX"}},
        {&core::VideoOptions::colourGrading, "Colour grading", {"Off", "By time of day"}},
        {&core::VideoOptions::weather, "Weather", {"Off", "Original", "Modern"}},
        {&core::VideoOptions::volumetricFog, "Volumetric fog", {"Off", "Low", "High"}},
        {&core::VideoOptions::viewDistance, "View distance", {"Original", "Far (x2)", "Very far (x3)", "Extreme (x4)"}},
        {&core::VideoOptions::detailDistance, "Detail distance", {"Near", "Medium", "Far", "Very far"}},
        {&core::VideoOptions::softParticles, "Soft particles", {"Off", "On"}},
        {&core::VideoOptions::foliageSway, "Foliage sway", {"Off", "On"}},
        {&core::VideoOptions::characterLighting, "Character lighting", {"Original", "Modern"}},
    };
    return kRows;
}

// The looks (our first row): Classic leaves every Modern row at the original's; the Modern ones set
// them by quality (core::VideoOptions::ApplyLook).
constexpr const char* kLooks[] = {"Classic", "Modern Low", "Modern High", "Modern Ultra"};
void ApplyLook(core::VideoOptions& v, int look) { v.ApplyLook(look); }
int LookOf(const core::VideoOptions& v) {
    for (int q = 0; q < 4; ++q) {
        core::VideoOptions t = v;
        ApplyLook(t, q);
        bool same = true;
        for (const FieldRow& row : FieldRows()) same = same && t.*row.field == v.*row.field;
        if (same) return q;
    }
    return -1; // the player's own mix
}

// ui::Key -> the Windows virtual-key code the key options keep (letters are their own codes).
int VirtualKey(ui::Key k) {
    const int v = int(k);
    if ((v >= 'A' && v <= 'Z') || (v >= '0' && v <= '9')) return v;
    switch (k) {
    case ui::Key::Tab: return 9;
    case ui::Key::Enter: return 13;
    case ui::Key::Space: return 32;
    case ui::Key::Backspace: return 8;
    case ui::Key::PageUp: return 33;
    case ui::Key::PageDown: return 34;
    case ui::Key::End: return 35;
    case ui::Key::Home: return 36;
    case ui::Key::Insert: return 45;
    case ui::Key::Delete: return 46;
    case ui::Key::Shift: return 16;
    case ui::Key::Ctrl: return 17;
    case ui::Key::Alt: return 18;
    case ui::Key::Semicolon: return 186;
    case ui::Key::Grave: return 192;
    default:
        if (v >= int(ui::Key::F1) && v <= int(ui::Key::F12)) return 112 + (v - int(ui::Key::F1));
        return 0;
    }
}

} // namespace

std::string BoundKeyName(const ui::KeyBindings& bindings, ui::KeyAction action) {
    const ui::Key key = bindings.Get(action);
    return key == ui::Key::None ? std::string() : core::InputOptions::KeyName(VirtualKey(key));
}

void ApplyKeyOptions(const core::InputOptions& input, ui::KeyBindings& bindings) {
    for (std::size_t a = 0; a < ui::kKeyActionCount && a < input.keys.size(); ++a) {
        ui::Key key = ui::Key::None;
        for (int k = 1; k < 400 && input.keys[a] != 0; ++k) {
            if (VirtualKey(ui::Key(k)) == input.keys[a]) {
                key = ui::Key(k);
                break;
            }
        }
        bindings.Set(static_cast<ui::KeyAction>(a), key);
    }
}

class OptionWindow : public ui::MainFrame {
public:
    OptionWindow(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}
    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& notice) override;
    // A fresh draft of the options, the video page shown.
    void Begin();

private:
    struct Row {
        ui::Widget* box = nullptr;
        ui::ComboBox* combo = nullptr;
        int slot = -1;  // an original row (its slot), or
        int field = -1; // one of ours (FieldRows), or -2 the look
    };
    struct List {
        ui::Widget* manager = nullptr;
        std::vector<ui::Widget*> rows;
        float rowHeight = 28.0f;
        int perLine = 1, visible = 5, first = -1;
    };
    void ShowPage(int page);
    void BuildVideo(ui::Interface& ui);
    void BuildInput(ui::Interface& ui);
    void BuildGame(ui::Interface& ui);
    void BuildOnline(ui::Interface& ui);
    void BuildBeta(ui::Interface& ui);
    int OnlineMax(int kind) const; // the server's share of players (0) or monsters (1)
    void Fill();          // every control from the draft
    void Scroll(List& list, ui::Frame* frame);
    void Keep(bool close); // the draft to the options (saved, applied)
    void Drop();

    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    core::Options draft_;
    int page_ = 0;
    std::vector<ui::Widget*> tabs_;
    List videoList_, keyList_, nameList_, gameList_;
    std::vector<Row> videoRows_;
    std::vector<ui::Widget*> keySlots_;
    std::vector<std::pair<ui::CheckBox*, int>> gameBoxes_; // the box and its GameOptions row
    ui::HSlider* onlineBars_[3] = {};  // players, monsters, how far
    ui::Widget* onlineValues_[3] = {}; // what each bar is at
    ui::Widget* onlineInfo_[2] = {};   // the server's limits, the traffic to expect
    ui::RadioButton* betaMovement_ = nullptr; // Classic, Pathfinding
    int capturing_ = -1; // the key slot waiting for a key
    bool filled_ = false; // filled once its controls made their own parts (the first frame)
};

std::unique_ptr<ui::Widget> MakeOptionWindow(const assets::UiLayoutNode& node, HudContext& context) {
    return std::make_unique<OptionWindow>(node, context);
}

void OptionWindow::Created(ui::Interface& ui) {
    ui::MainFrame::Created(ui);
    ui_ = &ui;
    // The tabs, made in code: the original's five, ids 100..104 at x 40 + 62 i,
    // y 40, 60 x 24, leave no room in the window's 386 for our two (105 Online, 106 Beta): seven, each
    // as wide as its caption (about 5.8 a letter in the window's font, and 12), 2 apart, centred.
    std::string captions[kPages];
    float widths[kPages] = {};
    float total = 2.0f * float(kPages - 1);
    for (int i = 0; i < kPages; ++i) {
        // The camera tab's English text in textuisystem.txt is a slip ("Set time", for the Korean
        // "set the view"): named for its page instead.
        captions[i] = i == kOnlinePage ? std::string("Online") : i == kBetaPage ? std::string("Beta") : context_.Text(kTabNames[i]);
        if (i == 2 && captions[i] == "Set time") captions[i] = "Camera";
        widths[i] = std::max(36.0f, std::round(5.8f * float(captions[i].size()) + 12.0f));
        total += widths[i];
    }
    float x = std::max(14.0f, std::round((rect.w - total) * 0.5f));
    for (int i = 0; i < kPages; ++i) {
        ui::Widget* tab = MakeNode(ui, *this, "CIFButton", 100 + i, {x, 40.0f, widths[i], 24.0f}, "interface\\ifcommon\\com_tab_off.ddj", 1);
        tab->SetText(captions[i]);
        tabs_.push_back(tab);
        x += widths[i] + 2.0f;
    }
    BuildVideo(ui);
    BuildInput(ui);
    BuildGame(ui);
    BuildOnline(ui);
    BuildBeta(ui);
    // The audio bars and the camera's choices are the layouts' own.
    if (ui::Widget* audio = Child(11)) {
        for (int id : {19, 20, 21}) {
            if (auto* bar = ui::As<ui::HSlider>(audio->Child(id))) bar->rect = {38.0f, bar->rect.y, 207.0f, 16.0f};
        }
    }
    if (ui::Widget* input = Child(13)) {
        if (auto* radio = ui::As<ui::RadioButton>(input->Child(20))) {
            // The group is already placed (29,43 on the page): its units start at its own corner,
            // inside the Mouse frame (14,13,337,80), 20 apart (set before the units are made).
            radio->origin = {0.0f, 0.0f};
            radio->labelWidth = 280.0f;
            radio->spacing = 20.0f;
            radio->SetUnits(ui, {context_.Text("UIIT_STT_USE_WHEEL_TO_CHANGE_SIGHT"), context_.Text("UIIT_STT_USE_WHEEL_TO_USE_SKILL")});
        }
    }
    Begin();
}

void OptionWindow::BuildVideo(ui::Interface& ui) {
    ui::Widget* page = Child(10);
    if (page == nullptr) return;
    // The resolution (the window as it is, then the sizes) and the brightness's five levels.
    if (auto* sizes = ui::As<ui::ComboBox>(page->Child(11))) {
        sizes->items.clear();
        for (const auto& s : core::VideoOptions::DisplaySizes()) sizes->items.push_back(s[0] == 0 ? std::string("Window") : std::format("{} x {}", s[0], s[1]));
    }
    if (auto* bright = ui::As<ui::ComboBox>(page->Child(13))) {
        bright->items.clear();
        for (const char* key : {"UILM_TEXT_BRIGHT_FIRST", "UILM_TEXT_BRIGHT_SECOND", "UILM_TEXT_BRIGHT_THIRD", "UILM_TEXT_BRIGHT_FORTH",
                                "UILM_TEXT_BRIGHT_FIFTH"}) {
            bright->items.push_back(context_.Text(key));
        }
    }
    // The detail list: the look, the original's rows, then ours (ifvideooptionslot: the label, the
    // choices; 28 high, six shown).
    ui::Widget* manager = page->Child(20);
    if (manager == nullptr) return;
    videoList_.manager = manager;
    videoList_.rowHeight = 28.0f;
    videoList_.visible = 6;
    manager->clipChildren = true;
    const auto add = [&](const std::string& label, const std::vector<std::string>& entries, int slot, int field) {
        ui::Widget* box = MakeNode(ui, *manager, "CIFStatic", 300 + int(videoRows_.size()), {6.0f, 0.0f, 308.0f, 28.0f}, "interface\\option\\opt_video_control_02.ddj");
        ui::Widget* text = MakeNode(ui, *box, "CIFStatic", 0, {4.0f, 8.0f, 146.0f, 12.0f});
        text->SetText(label);
        auto* combo = ui::As<ui::ComboBox>(MakeNode(ui, *box, "CIFComboBox", 2, {151.0f, 4.0f, 156.0f, 20.0f}));
        if (combo != nullptr) combo->items = entries;
        videoRows_.push_back({box, combo, slot, field});
        videoList_.rows.push_back(box);
    };
    std::vector<std::string> looks(std::begin(kLooks), std::end(kLooks));
    add("Look", looks, -1, -2);
    int count = 0;
    const core::VideoOptions::Row* rows = core::VideoOptions::Rows(count);
    for (int i = 0; i < count; ++i) {
        std::vector<std::string> entries;
        for (int e = 0; e < rows[i].entries; ++e) {
            const std::string name = rows[i].names[e];
            entries.push_back(name.starts_with("UI") ? context_.Text(name) : name);
        }
        add(context_.Text(rows[i].text), entries, rows[i].slot, -1);
    }
    for (std::size_t f = 0; f < FieldRows().size(); ++f) {
        add(FieldRows()[f].label, std::vector<std::string>(FieldRows()[f].entries.begin(), FieldRows()[f].entries.end()), -1, int(f));
    }
    std::vector<std::string> limits;
    for (const int l : core::VideoOptions::FrameLimits()) limits.push_back(l == 0 ? "Vertical sync" : l < 0 ? "Unlimited" : std::to_string(l));
    add("Frame limit", limits, -1, -3);
}

void OptionWindow::BuildInput(ui::Interface& ui) {
    ui::Widget* page = Child(13);
    ui::Widget* manager = page != nullptr ? page->Child(10) : nullptr;
    if (manager == nullptr) return;
    // The 34 keys in two columns (ifkeyoptionslot: the key, the command), 28 high, five lines shown.
    keyList_.manager = manager;
    keyList_.perLine = 2;
    keyList_.visible = 5;
    manager->clipChildren = true;
    for (int i = 0; i < core::InputOptions::kBindings; ++i) {
        ui::Widget* slot = MakeNode(ui, *manager, "CIFStatic", 300 + i, {6.0f + 156.0f * float(i % 2), 0.0f, 156.0f, 28.0f}, "interface\\option\\opt_key.ddj");
        slot->interactive = true;
        MakeNode(ui, *slot, "CIFStatic", 11, {6.0f, 6.0f, 56.0f, 16.0f}, {}, 1);
        MakeNode(ui, *slot, "CIFStatic", 10, {65.0f, 6.0f, 84.0f, 16.0f})->SetText(context_.Text(core::InputOptions::Label(i)));
        keySlots_.push_back(slot);
        keyList_.rows.push_back(slot);
    }
}

void OptionWindow::BuildGame(ui::Interface& ui) {
    ui::Widget* page = Child(14);
    if (page == nullptr) return;
    // Two lists of boxes (ifgameoptionslot: the name, the box), two a line, 28 high, three lines shown.
    int count = 0;
    const core::GameOptions::Row* rows = core::GameOptions::Rows(count);
    for (int group = 0; group < 2; ++group) {
        List& list = group == 0 ? nameList_ : gameList_;
        list.manager = page->Child(group + 1);
        if (list.manager == nullptr) continue;
        list.perLine = 2;
        list.visible = 3;
        list.manager->clipChildren = true;
        for (int i = 0; i < count; ++i) {
            if (rows[i].group != group) continue;
            ui::Widget* slot = MakeNode(ui, *list.manager, "CIFStatic", 300 + i, {6.0f + 160.0f * float(list.rows.size() % 2), 0.0f, 160.0f, 28.0f});
            MakeNode(ui, *slot, "CIFStatic", 1, {14.0f, 8.0f, 112.0f, 16.0f})->SetText(context_.Text(rows[i].text));
            auto* box = ui::As<ui::CheckBox>(MakeNode(ui, *slot, "CIFCheckBox", 11, {130.0f, 6.0f, 16.0f, 16.0f}, "interface\\ifcommon\\com_checkbutton_off.ddj"));
            if (box != nullptr) gameBoxes_.push_back({box, i});
            list.rows.push_back(slot);
        }
    }
}

void OptionWindow::BuildOnline(ui::Interface& ui) {
    // The page (15, where the original's sit, 11,62), drawn as the audio page is: a box (opt_inner_box)
    // for each bar with its caption, the volume board and the bar; what the bar is at where the audio
    // page has its Remove button; a third box says what the server allows and the traffic to expect.
    ui::Widget* page = MakeNode(ui, *this, "CIFStatic", 10 + kOnlinePage, {11.0f, 62.0f, 364.0f, 274.0f});
    page->visible = false;
    const char* captions[3] = {"Players shown around you", "Monsters shown around you", "How far you see them"};
    for (int k = 0; k < 4; ++k) {
        const float y = 64.0f * float(k);
        MakeNode(ui, *page, "CIFFrame", 4 + k, {11.0f, 10.0f + y, 342.0f, 62.0f}, "interface\\option\\opt_inner_box_");
        if (k == 3) {
            for (int line = 0; line < 2; ++line) {
                onlineInfo_[line] = MakeNode(ui, *page, "CIFStatic", 30 + line, {18.0f, 206.0f + 26.0f * float(line), 326.0f, 12.0f});
                if (auto* s = ui::As<ui::Static>(onlineInfo_[line])) s->textColor = line == 0 ? kLabelColor : ui::kWhite;
            }
            continue;
        }
        ui::Widget* caption = MakeNode(ui, *page, "CIFStatic", 10 + k, {18.0f, 14.0f + y, 326.0f, 12.0f});
        caption->SetText(captions[k]);
        if (auto* s = ui::As<ui::Static>(caption)) s->textColor = kLabelColor;
        MakeNode(ui, *page, "CIFStatic", 7 + k, {15.0f, 37.0f + y, 0.0f, 0.0f}, "interface\\option\\opt_volume.ddj");
        onlineBars_[k] = ui::As<ui::HSlider>(MakeNode(ui, *page, "CIFHScroll_Option", 19 + k, {38.0f, 42.0f + y, 207.0f, 16.0f}));
        onlineValues_[k] = MakeNode(ui, *page, "CIFStatic", 22 + k, {262.0f, 41.0f + y, 84.0f, 12.0f}, {}, 1);
    }
}

void OptionWindow::BuildBeta(ui::Interface& ui) {
    // One box (opt_inner_box, as the Online page's): the movement's two ways, a radio of two, and what they are.
    ui::Widget* page = MakeNode(ui, *this, "CIFStatic", 10 + kBetaPage, {11.0f, 62.0f, 364.0f, 210.0f});
    page->visible = false;
    MakeNode(ui, *page, "CIFFrame", 4, {11.0f, 10.0f, 342.0f, 166.0f}, "interface\\option\\opt_inner_box_");
    ui::Widget* caption = MakeNode(ui, *page, "CIFStatic", 10, {18.0f, 14.0f, 326.0f, 12.0f});
    caption->SetText("Movement (experimental)");
    if (auto* s = ui::As<ui::Static>(caption)) s->textColor = kLabelColor;
    if (auto* radio = ui::As<ui::RadioButton>(MakeNode(ui, *page, "CIFRadioButton", 20, {18.0f, 34.0f, 326.0f, 48.0f}))) {
        radio->origin = {0.0f, 0.0f};
        radio->labelWidth = 300.0f;
        radio->spacing = 22.0f;
        radio->SetUnits(ui, {"Classic: straight to where you click", "Pathfinding: around walls and buildings"});
        betaMovement_ = radio;
    }
    const char* lines[5] = {"Classic is the original's: a click walks straight and stops at",
                            "the first wall. A click on the sky runs that way (both).",
                            "Pathfinding searches the way on your computer and walks it",
                            "in straight steps; the server checks each step as it checks",
                            "a click. Clicks far away take a moment to find their way."};
    for (int line = 0; line < 5; ++line) {
        ui::Widget* text = MakeNode(ui, *page, "CIFStatic", 30 + line, {18.0f, 92.0f + 15.0f * float(line), 330.0f, 12.0f});
        text->SetText(lines[line]);
        if (auto* s = ui::As<ui::Static>(text)) s->textColor = ui::kWhite;
    }
}

int OptionWindow::OnlineMax(int kind) const {
    if (context_.view == nullptr) return core::OnlineOptions::kMax; // not in the world yet: not known
    return std::max(1, int(kind == 0 ? context_.view->maxPlayers : context_.view->maxMonsters));
}

void OptionWindow::Begin() {
    if (context_.options != nullptr) draft_ = *context_.options;
    capturing_ = -1;
    ShowPage(0);
    Fill();
}

void OptionWindow::ShowPage(int page) {
    page_ = std::clamp(page, 0, kPages - 1);
    const Geometry& g = kGeometry[page_];
    rect.h = g.height;
    for (int i = 0; i < kPages; ++i) {
        if (ui::Widget* w = Child(10 + i)) w->visible = i == page_;
        if (auto* tab = ui::As<ui::Static>(tabs_[std::size_t(i)])) tab->texture = i == page_ ? "interface\\ifcommon\\com_tab_on.ddj" : "";
    }
    // The tile behind the pages. The observed rects leave strips 4 to 10 pixels high that nothing
    // covers (frames draw no interior): above the pages' boards, under the main frame's top edge
    // (68 high), and between the tile and its bottom edge (48 high). Ours runs from the one edge to
    // the other, under everything, at the original's x and width.
    if (ui::Widget* tile = Child(5)) tile->rect = {g.tile.x, 68.0f, g.tile.w, std::max(g.tile.y + g.tile.h, g.height - 48.0f) - 68.0f};
    const int buttons[3] = {50, 51, 52};
    for (int b = 0; b < 3; ++b) {
        if (ui::Widget* w = Child(buttons[b])) {
            w->rect.x = g.x[b];
            w->rect.y = g.row;
        }
    }
    if (ui::Widget* apply = Child(53)) {
        apply->visible = page_ == 0;
        apply->rect.y = g.row;
    }
    capturing_ = -1;
}

void OptionWindow::Fill() {
    core::VideoOptions& v = draft_.video;
    if (ui::Widget* page = Child(10)) {
        for (int i = 0; i < 2; ++i) {
            if (auto* b = ui::As<ui::Static>(page->Child(25 + i))) b->texture = i == v.active ? "interface\\option\\opt_video_tab.ddj" : "";
        }
        if (auto* sizes = ui::As<ui::ComboBox>(page->Child(11))) {
            const auto& list = core::VideoOptions::DisplaySizes();
            int at = 0;
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (list[i][0] == v.displayWidth && list[i][1] == v.displayHeight) at = int(i);
            }
            sizes->Select(at);
        }
        if (auto* bright = ui::As<ui::ComboBox>(page->Child(13))) bright->Select(v.brightness[std::size_t(v.active)]);
    }
    for (Row& r : videoRows_) {
        if (r.combo == nullptr) continue;
        int value = 0;
        if (r.field == -2) value = LookOf(v);
        else if (r.field == -3) {
            const auto& limits = core::VideoOptions::FrameLimits();
            value = int(std::find(limits.begin(), limits.end(), v.frameLimit) - limits.begin());
        } else if (r.field >= 0) value = v.*FieldRows()[std::size_t(r.field)].field;
        else value = v.current()[std::size_t(r.slot)];
        r.combo->Select(value);
    }
    if (ui::Widget* audio = Child(11)) {
        const int levels[3] = {draft_.audio.bgm, draft_.audio.effects, draft_.audio.environment};
        const bool mutes[3] = {draft_.audio.muteBgm, draft_.audio.muteEffects, draft_.audio.muteEnvironment};
        for (int i = 0; i < 3; ++i) {
            if (auto* bar = ui::As<ui::HSlider>(audio->Child(19 + i))) {
                bar->SetValue(levels[i]);
                bar->enabled = !mutes[i];
            }
            if (auto* box = ui::As<ui::CheckBox>(audio->Child(13 + i))) box->checked = mutes[i];
        }
    }
    if (ui::Widget* camera = Child(12)) {
        for (int i = 0; i < 3; ++i) {
            if (auto* box = ui::As<ui::CheckBox>(camera->Child(10 + i))) box->checked = int(draft_.sight) == i;
        }
    }
    if (ui::Widget* input = Child(13)) {
        if (auto* radio = ui::As<ui::RadioButton>(input->Child(20))) radio->selected = draft_.input.mouseMode;
    }
    if (betaMovement_ != nullptr) betaMovement_->selected = int(draft_.beta.movement);
    for (std::size_t i = 0; i < keySlots_.size(); ++i) {
        const int vk = draft_.input.keys[i];
        if (ui::Widget* key = keySlots_[i]->Child(11)) key->SetText(int(i) == capturing_ ? std::string("...") : core::InputOptions::KeyName(vk));
        if (auto* s = ui::As<ui::Static>(keySlots_[i])) s->texture = int(i) == capturing_ ? "interface\\option\\opt_key02.ddj" : "";
    }
    int count = 0;
    const core::GameOptions::Row* rows = core::GameOptions::Rows(count);
    for (auto& [box, row] : gameBoxes_) box->checked = draft_.game.Get(rows[row].key);
    // Online: a bar's far end is everything the server's share allows (kept as 0, so a server that
    // raises its shares raises the view too); below it, the count asked for.
    const int asked[2] = {draft_.online.players, draft_.online.monsters};
    int shown[2] = {0, 0};
    for (int k = 0; k < 2; ++k) {
        const int most = OnlineMax(k);
        shown[k] = asked[k] == 0 ? most : std::min(asked[k], most);
        if (ui::HSlider* bar = onlineBars_[k]) {
            bar->min = std::min(10, most);
            bar->max = most;
            bar->SetValue(shown[k]);
        }
        if (onlineValues_[k] != nullptr) {
            const bool all = asked[k] == 0 || asked[k] >= most;
            onlineValues_[k]->SetText(!all ? std::to_string(shown[k]) : context_.view != nullptr ? std::format("All ({})", most) : std::string("All"));
        }
    }
    // How far: 200 to 600, no farther than the server's.
    {
        const int most = context_.view != nullptr ? std::clamp(int(context_.view->radius), core::OnlineOptions::kMinRadius, core::OnlineOptions::kMaxRadius)
                                                  : core::OnlineOptions::kMaxRadius;
        const int at = std::clamp(draft_.online.radius, core::OnlineOptions::kMinRadius, most);
        if (onlineBars_[2] != nullptr) {
            onlineBars_[2]->min = core::OnlineOptions::kMinRadius;
            onlineBars_[2]->max = most;
            onlineBars_[2]->SetValue(at);
        }
        if (onlineValues_[2] != nullptr) onlineValues_[2]->SetText(std::to_string(at));
    }
    if (onlineInfo_[0] != nullptr) {
        onlineInfo_[0]->SetText(context_.view != nullptr ? std::format("This server: up to {} players, {} monsters within {}", context_.view->maxPlayers,
                                                                       context_.view->maxMonsters, context_.view->radius)
                                                         : std::string("The server's limits are known once in the world"));
    }
    if (onlineInfo_[1] != nullptr) {
        // Measured with SroBots (1,000 bots in one spot, all walking and fighting): about 15 bytes a
        // second for each entity in view, plus the player's own; a quiet place sends far less.
        const bool known = context_.view != nullptr || (asked[0] != 0 && asked[1] != 0);
        const double kb = 0.8 + 0.0153 * double(shown[0] + shown[1]);
        onlineInfo_[1]->SetText(known ? std::format("In a crowd: up to about {:.0f} KB/s down", kb) : std::string("Lower these on a slow PC or connection"));
    }
}

void OptionWindow::Scroll(List& list, ui::Frame* frame) {
    if (list.manager == nullptr) return;
    auto* bar = ui::As<ui::ScrollBar>(list.manager->Child(10));
    const int lines = (int(list.rows.size()) + list.perLine - 1) / list.perLine;
    if (bar != nullptr && list.first < 0) bar->SetRange(0, std::max(0, lines - list.visible));
    // (Not when another window is under the mouse there: a combo box's open list scrolls itself.)
    ui::Widget* under = frame != nullptr ? frame->interface.under() : nullptr;
    if (frame != nullptr && bar != nullptr && frame->input.wheel() != 0.0f && Inside(list.manager->ScreenRect(), frame->pointer.position) &&
        under != nullptr && under->Root() == Root()) {
        bar->SetPosition(bar->position() + (frame->input.wheel() > 0.0f ? -1 : 1));
    }
    const int first = bar != nullptr ? bar->position() : 0;
    if (first == list.first) return;
    list.first = first;
    for (std::size_t i = 0; i < list.rows.size(); ++i) {
        const int line = int(i) / list.perLine - first;
        list.rows[i]->visible = line >= 0 && line < list.visible + 1;
        list.rows[i]->rect.y = 2.0f + list.rowHeight * float(line);
    }
}

void OptionWindow::Update(ui::Frame& frame) {
    if (!filled_) {
        filled_ = true;
        Fill();
    }
    // The lists follow their bars (and the wheel over them).
    Scroll(videoList_, &frame);
    Scroll(keyList_, &frame);
    Scroll(nameList_, &frame);
    Scroll(gameList_, &frame);
    // The mouse mode as its radio shows it.
    if (ui::Widget* input = Child(13)) {
        if (auto* radio = ui::As<ui::RadioButton>(input->Child(20))) draft_.input.mouseMode = radio->selected;
    }
    // The movement as the Beta page's radio shows it.
    if (betaMovement_ != nullptr) draft_.beta.movement = core::BetaOptions::Movement(std::clamp(betaMovement_->selected, 0, 1));
    // A key slot clicked waits for a key.
    if (page_ == 3 && frame.input.Released(ui::MouseButton::Left)) {
        for (std::size_t i = 0; i < keySlots_.size(); ++i) {
            if (keySlots_[i]->ShownOnScreen() && Inside(keySlots_[i]->ScreenRect(), frame.pointer.position)) {
                capturing_ = int(i);
                Fill();
                return;
            }
        }
    }
    // A key slot waiting: the next key pressed (the original's refusals hold), Escape gives up.
    if (capturing_ >= 0) {
        if (frame.input.KeyPressed(ui::Key::Escape)) {
            capturing_ = -1;
            Fill();
            return;
        }
        for (int k = 1; k < 400; ++k) {
            if (!frame.input.KeyPressed(ui::Key(k))) continue;
            const int vk = VirtualKey(ui::Key(k));
            if (vk != 0 && draft_.input.Capture(capturing_, vk)) {
                capturing_ = -1;
                Fill();
                break;
            }
        }
    }
}

void OptionWindow::Keep(bool close) {
    if (context_.options != nullptr) {
        // Apply keeps the video page only; Confirm everything.
        if (close) *context_.options = draft_;
        else context_.options->video = draft_.video;
        if (context_.optionsChanged) context_.optionsChanged();
    }
    if (close && ui_ != nullptr) ui_->Close(this);
}

void OptionWindow::Drop() {
    // What the bars previewed goes back to the kept levels.
    if (context_.options != nullptr && context_.audioPreview) context_.audioPreview(context_.options->audio);
    if (ui_ != nullptr) ui_->Close(this);
}

bool OptionWindow::OnNotice(const ui::Notice& notice) {
    if (notice.source == nullptr) return ui::MainFrame::OnNotice(notice);
    const int id = notice.control;
    if (notice.kind == ui::Notice::Kind::Clicked && notice.source->parent() == this) {
        if (id >= 100 && id < 100 + kPages) {
            ShowPage(id - 100);
            Fill();
            return true;
        }
        switch (id) {
        case 50: { // Default: the page shown
            if (page_ == 0) draft_.video.ResetActive();
            if (page_ == 1) {
                draft_.audio = core::AudioOptions::Defaults();
                if (context_.audioPreview) context_.audioPreview(draft_.audio);
            }
            if (page_ == 2) draft_.sight = core::Sight::Free;
            if (page_ == 3) draft_.input = core::InputOptions::Defaults();
            if (page_ == 4) draft_.game = core::GameOptions::Reset();
            if (page_ == kOnlinePage) draft_.online = {};
            if (page_ == kBetaPage) draft_.beta = {};
            Fill();
            return true;
        }
        case 51: Keep(true); return true;
        case 52: Drop(); return true;
        case 53: Keep(false); return true;
        case 2: Drop(); return true; // the close button
        default: break;
        }
    }
    // The video page.
    ui::Widget* video = Child(10);
    if (video != nullptr && notice.kind == ui::Notice::Kind::Clicked && notice.source->parent() == video && (id == 25 || id == 26)) {
        draft_.video.active = id - 25;
        Fill();
        return true;
    }
    if (notice.kind == ui::Notice::Kind::ComboPicked && video != nullptr && notice.source->parent() == video) {
        if (id == 11 && notice.value >= 0) {
            const auto& s = core::VideoOptions::DisplaySizes()[std::size_t(notice.value)];
            draft_.video.displayWidth = s[0];
            draft_.video.displayHeight = s[1];
        }
        if (id == 13 && notice.value >= 0) draft_.video.brightness[std::size_t(draft_.video.active)] = notice.value;
        return true;
    }
    if (notice.kind == ui::Notice::Kind::ComboPicked) {
        for (Row& r : videoRows_) {
            if (notice.source != r.combo || notice.value < 0) continue;
            core::VideoOptions& v = draft_.video;
            if (r.field == -2) ApplyLook(v, notice.value);
            else if (r.field == -3) v.frameLimit = core::VideoOptions::FrameLimits()[std::size_t(notice.value)];
            else if (r.field >= 0) v.*FieldRows()[std::size_t(r.field)].field = notice.value;
            else v.Change(r.slot, notice.value);
            Fill();
            return true;
        }
    }
    // The audio page: the bars heard at once, the boxes mute.
    if (ui::Widget* audio = Child(11); audio != nullptr && notice.source->parent() == audio) {
        int* levels[3] = {&draft_.audio.bgm, &draft_.audio.effects, &draft_.audio.environment};
        bool* mutes[3] = {&draft_.audio.muteBgm, &draft_.audio.muteEffects, &draft_.audio.muteEnvironment};
        if (notice.kind == ui::Notice::Kind::ValueChanged && id >= 19 && id <= 21) *levels[id - 19] = notice.value;
        if (notice.kind == ui::Notice::Kind::Clicked && id >= 13 && id <= 15) {
            if (auto* box = ui::As<ui::CheckBox>(notice.source)) *mutes[id - 13] = box->checked;
        }
        if (notice.kind == ui::Notice::Kind::Clicked && id >= 16 && id <= 18) *mutes[id - 16] = !*mutes[id - 16];
        if (context_.audioPreview) context_.audioPreview(draft_.audio);
        Fill();
        return true;
    }
    // The Online page's bars: at the far end, the server's whole share (0).
    if (ui::Widget* online = Child(10 + kOnlinePage);
        online != nullptr && notice.source->parent() == online && notice.kind == ui::Notice::Kind::ValueChanged && id >= 19 && id <= 21) {
        auto* bar = ui::As<ui::HSlider>(notice.source);
        const int v = bar != nullptr && notice.value >= bar->max ? 0 : notice.value;
        if (id == 21) {
            draft_.online.radius = notice.value; // how far: its value itself
        } else {
            (id == 19 ? draft_.online.players : draft_.online.monsters) = v;
        }
        Fill();
        return true;
    }
    // The camera page: one of three.
    if (ui::Widget* camera = Child(12); camera != nullptr && notice.source->parent() == camera && notice.kind == ui::Notice::Kind::Clicked && id >= 10 && id <= 12) {
        draft_.sight = core::Sight(id - 10);
        Fill();
        return true;
    }
    // The input page: the mouse mode; a key slot clicked waits for a key.
    if (ui::Widget* input = Child(13); input != nullptr && notice.kind == ui::Notice::Kind::Clicked) {
        if (auto* radio = ui::As<ui::RadioButton>(input->Child(20)); radio != nullptr && (notice.source == radio || notice.source->parent() == radio)) {
            draft_.input.mouseMode = radio->selected;
            return true;
        }
    }
    // The game page's boxes.
    if (notice.kind == ui::Notice::Kind::Clicked) {
        int count = 0;
        const core::GameOptions::Row* rows = core::GameOptions::Rows(count);
        for (auto& [box, row] : gameBoxes_) {
            if (notice.source == box) {
                draft_.game.Set(rows[row].key, box->checked);
                return true;
            }
        }
    }
    return ui::MainFrame::OnNotice(notice);
}

} // namespace sr::client
