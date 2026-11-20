// The Forgotten World's windows of the 1.188 client, from their res_ui layouts:
// - CNIFSummonPartyUserWnd (res_ui\nifsummonpartyuserwnd.2dt, the party recall pillar's "Summon Party member"): eight
//   rows (CNIFSummonPartyUserSlot) of a member's name, its state ("Complete" 0xFF80DA11 inside already,
//   "Possible" 0xFF02DA11, "Impossible" 0xFFD40000 with the reason as its tooltip), its level and the minutes left
//   ("%d %s", UIIT_STT_MINUTE); a row chosen and the Summon button (5 s between summons, UIIT_STT_PARTY_SUMMON_BUTTON).
// - CNIFCollectionWnd (res_ui\nifcollectionwnd.2dt, "Collection book"): the themes' rows on the left
//   (CNIFCollectionThemeSlot), the chosen theme's description under them, its talismans on the right in two rows of four
//   (CNIFCollectionItemSlot, 88 x 128): a registered one its picture (collectionbook_item.txt), an empty one
//   com_collection_deco_window.ddj; a registered one clicked opens its story. A registration
//   answered opens the book on its theme and plays the slot's flip-book: control
//   1003 (128 x 168 over the slot) shown, a 100 ms timer stepping it through com_collection_effect.ddj (1664 x 256,
//   frames of 128 x 168 side by side; sets twelve, u = i * 128 / 1664, v to 0.65625), then hidden.
// - CNIFCollectionItemStoryWnd (res_ui\nifcollectionitemstory.2dt): the talisman's picture, its name, its story.
// - CNIFCollectionMsgBox (res_ui\nifcollectionmsgbox.2dt): a talisman used from the bag: its picture, its
//   name, "Do you want to register talisman into your collection?", Confirm and Cancel.

#include <algorithm>
#include <format>

#include "gui/Hud.h"
#include "gui/HudWindows.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr const char* kSummonLayout = "res_ui\\nifsummonpartyuserwnd.2dt";
constexpr const char* kBookLayout = "res_ui\\nifcollectionwnd.2dt";
constexpr const char* kStoryLayout = "res_ui\\nifcollectionitemstory.2dt";
constexpr const char* kBoxLayout = "res_ui\\nifcollectionmsgbox.2dt";
constexpr const char* kEmptySlot = "interface\\ifcommon\\com_collection_deco_window.ddj";
constexpr double kSummonPause = 5.0;
constexpr const char* kFlipBook = "interface\\ifcommon\\com_collection_effect.ddj";
constexpr double kFlipStep = 0.1; // the timer's 100 ms
constexpr int kFlipFrames = 12;

std::string Format(const HudContext& c, const char* key, const std::string& value) {
    std::string pattern = c.Text(key);
    const std::size_t at = pattern.find("%s");
    return at == std::string::npos ? pattern : pattern.replace(at, 2, value);
}

// A layout's records built under the window; the CIFEdit records of its text boxes made text boxes (kind 6 there).
std::vector<ui::Widget*> BuildWindow(ui::Interface& ui, ui::Widget& window, const std::vector<assets::Ui2dtRecord>& records) {
    return Build2dt(ui, window, records, [](const assets::Ui2dtRecord& r, assets::UiLayoutNode& n) {
        if (r.kind == 6) n.type = "CIFTextBox";
    });
}

void Put(ui::Widget* w, const std::string& text, std::optional<ui::Color> colour = std::nullopt) {
    if (w == nullptr) return;
    w->SetText(text);
    if (auto* s = dynamic_cast<ui::Static*>(w); s != nullptr && colour) s->textColor = *colour;
}

// --- The summon window -------------------------------------------------------------------------------------------

class SummonWindow : public ui::MainFrame {
public:
    SummonWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        const auto* records = c_.layout2dt ? c_.layout2dt(kSummonLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        if (ui::Widget* title = Child(0)) title->SetText(c_.Text(records->front().textKey));
        const std::vector<ui::Widget*> made = BuildWindow(ui, *this, *records);
        for (std::size_t i = 0; i < records->size(); ++i) {
            if ((*records)[i].className == "CNIFSummonPartyUserSlot") rows_.push_back({made[i]});
            if ((*records)[i].id == 7) summon_ = made[i];
        }
        // A row's parts (nifsummonpartyuserwnd.2dt): 14 the name, 15 the state, 16 the level, 17 the minutes.
        for (Row& r : rows_) {
            r.name = r.slot->Child(14);
            r.state = r.slot->Child(15);
            r.level = r.slot->Child(16);
            r.minutes = r.slot->Child(17);
        }
    }

    void Update(ui::Frame& frame) override {
        ui::MainFrame::Update(frame);
        if (c_.summonList != nullptr && c_.summonRevision != revision_) {
            revision_ = c_.summonRevision;
            chosen_ = -1;
            Fill();
        }
        // The reason of an impossible row, under the mouse.
        for (const Row& r : rows_) {
            if (r.row != nullptr && r.row->state == 3 && r.state != nullptr && frame.pointer.hovered != nullptr &&
                r.slot->ScreenRect().Contains(frame.pointer.position)) {
                frame.interface.ShowTooltip(c_.Text(std::format("UIIT_STT_PARTY_SUMMON_IMPOSSILBE_{}", r.row->reason)), r.state->ScreenRect());
            }
        }
    }

    void Mouse(ui::Frame& frame) override {
        if (frame.input.Pressed(ui::MouseButton::Left)) {
            for (std::size_t i = 0; i < rows_.size(); ++i) {
                if (rows_[i].row != nullptr && rows_[i].slot->visible && rows_[i].slot->ScreenRect().Contains(frame.pointer.position)) Choose(int(i));
            }
        }
        ui::MainFrame::Mouse(frame);
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.source == summon_) {
            Summon();
            return true;
        }
        return ui::MainFrame::OnNotice(n);
    }

    bool ChooseName(const std::string& name) {
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].row != nullptr && rows_[i].row->name == name) {
                Choose(int(i));
                return true;
            }
        }
        return false;
    }

    void Summon() {
        if (chosen_ < 0 || std::size_t(chosen_) >= rows_.size() || rows_[std::size_t(chosen_)].row == nullptr || !c_.instanceRequest) return;
        if (c_.time < nextSummon_) {
            if (c_.systemLine) c_.systemLine(c_.Text("UIIT_STT_PARTY_SUMMON_BUTTON"));
            return;
        }
        nextSummon_ = c_.time + kSummonPause;
        c_.instanceRequest(proto::SummonMember::kId, proto::Encode(proto::SummonMember{rows_[std::size_t(chosen_)].row->character}));
    }

private:
    struct Row {
        ui::Widget* slot = nullptr;
        ui::Widget *name = nullptr, *state = nullptr, *level = nullptr, *minutes = nullptr;
        const proto::SummonList::Row* row = nullptr;
    };

    void Choose(int i) {
        chosen_ = i;
        for (std::size_t k = 0; k < rows_.size(); ++k) {
            if (auto* s = dynamic_cast<ui::Static*>(rows_[k].name)) s->textColor = int(k) == chosen_ ? ui::Argb(0xFFFFD953) : ui::Argb(0xFFFFFFFF);
        }
    }

    void Fill() {
        const auto& list = c_.summonList->rows;
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            Row& r = rows_[i];
            r.row = i < list.size() ? &list[i] : nullptr;
            r.slot->visible = true;
            if (r.row == nullptr) {
                for (ui::Widget* w : {r.name, r.state, r.level, r.minutes}) Put(w, "");
                continue;
            }
            Put(r.name, r.row->name, ui::Argb(0xFFFFFFFF));
            switch (r.row->state) {
            case 1: Put(r.state, c_.Text("UIIT_STT_FORT_SMITH_COMPLETE"), ui::Argb(0xFF80DA11)); break;
            case 2: Put(r.state, c_.Text("UIIT_CTL_PARTY_SUMMON_POSSILBE"), ui::Argb(0xFF02DA11)); break;
            default: Put(r.state, c_.Text("UIIT_CTL_PARTY_SUMMON_IMPOSSILBE"), ui::Argb(0xFFD40000)); break;
            }
            Put(r.level, std::to_string(r.row->level));
            Put(r.minutes, std::format("{} {}", r.row->minutes, c_.Text("UIIT_STT_MINUTE")));
        }
    }

    HudContext& c_;
    std::vector<Row> rows_;
    ui::Widget* summon_ = nullptr;
    int revision_ = -1, chosen_ = -1;
    double nextSummon_ = 0.0;
};

// --- The collection book ---------------------------------------------------------------------------------------

class StoryWindow : public ui::MainFrame {
public:
    StoryWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}
    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        const auto* records = c_.layout2dt ? c_.layout2dt(kStoryLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        if (ui::Widget* title = Child(0)) title->SetText(c_.Text(records->front().textKey));
        BuildWindow(ui, *this, *records);
    }
    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.control == 6) { // UIIT_CTL_LETTER_WINDOWSCLOSE
            visible = false;
            return true;
        }
        return ui::MainFrame::OnNotice(n);
    }
    // nifcollectionitemstory.2dt: 28 the picture (80 x 120), 27 the name, 14 the story.
    void Show(const assets::CollectionItemRow& item) {
        if (auto* picture = dynamic_cast<ui::Static*>(Find(28))) picture->texture = item.picture;
        const assets::ItemRow* row = c_.items != nullptr ? c_.items->Find(std::string_view(item.item)) : nullptr;
        Put(Find(27), row != nullptr ? c_.Text(row->nameKey) : item.item);
        if (auto* story = dynamic_cast<ui::TextBox*>(Find(14))) {
            story->ClearParagraphs();
            std::string text = c_.Text(item.storyKey);
            for (std::size_t at = text.find("\\n"); at != std::string::npos; at = text.find("\\n")) text.replace(at, 2, "\n");
            story->AddParagraph(text, ui::Argb(0xFFFFFFFF));
            story->ScrollBy(-1000); // from its first line
        }
    }

private:
    HudContext& c_;
};

class CollectionWindow : public ui::MainFrame {
public:
    CollectionWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        const auto* records = c_.layout2dt ? c_.layout2dt(kBookLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        if (ui::Widget* title = Child(0)) title->SetText(c_.Text(records->front().textKey));
        const std::vector<ui::Widget*> made = BuildWindow(ui, *this, *records);
        for (std::size_t i = 0; i < records->size(); ++i) {
            const auto& r = (*records)[i];
            if (r.className == "CNIFCollectionThemeSlot") themes_.push_back({made[i], made[i]->Child(1082)});
            if (r.className == "CNIFCollectionItemSlot") {
                slots_.push_back({made[i], dynamic_cast<ui::Static*>(made[i]->Child(1002)), dynamic_cast<ui::Static*>(made[i]->Child(1003))});
            }
            if (r.id == 53) description_ = dynamic_cast<ui::TextBox*>(made[i]);
        }
        for (const Slot& s : slots_) {
            if (s.effect == nullptr) continue;
            s.effect->visible = false;
            s.effect->texture = kFlipBook;
            s.effect->customUv = true;
        }
    }

    void Update(ui::Frame& frame) override {
        ui::MainFrame::Update(frame);
        const std::size_t book = c_.book != nullptr ? c_.book->size() : 0;
        if (book != bookSize_ || dirty_) {
            bookSize_ = book;
            dirty_ = false;
            Fill();
        }
        Flip(frame.time);
    }

    // A talisman registered (TalismanRegistered): its theme shown and its slot's flip-book played.
    void ShowRegistered(std::uint32_t theme, int slot) {
        if (c_.collection == nullptr) return;
        const auto& themes = c_.collection->Themes();
        for (std::size_t i = 0; i < themes.size(); ++i) {
            if (themes[i].id == theme) PickTheme(int(i));
        }
        if (slot < 1 || std::size_t(slot) > slots_.size()) return;
        flipSlot_ = slot - 1;
        flipStart_ = -1.0; // from the next frame's time
    }

    void Mouse(ui::Frame& frame) override {
        if (frame.input.Pressed(ui::MouseButton::Left)) {
            for (std::size_t i = 0; i < themes_.size(); ++i) {
                if (themes_[i].row->visible && themes_[i].row->ScreenRect().Contains(frame.pointer.position)) PickTheme(int(i));
            }
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                if (slots_[i].slot->ScreenRect().Contains(frame.pointer.position)) OpenStory(int(i) + 1);
            }
        }
        ui::MainFrame::Mouse(frame);
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.control == 992) { // UIIT_CTL_LETTER_WINDOWSCLOSE
            visible = false;
            return true;
        }
        return ui::MainFrame::OnNotice(n);
    }

    bool PickTheme(int row) {
        if (c_.collection == nullptr || row < 0 || std::size_t(row) >= c_.collection->Themes().size()) return false;
        theme_ = row;
        dirty_ = true;
        return true;
    }

    // The theme's talisman in that slot (1..) clicked: its story when registered.
    bool OpenStory(int slot) {
        const assets::CollectionThemeRow* theme = Theme();
        const assets::CollectionItemRow* item = theme != nullptr ? c_.collection->Item(theme->codename, slot) : nullptr;
        if (item == nullptr || !Registered(theme->id, slot) || c_.hud == nullptr || c_.hud->interface() == nullptr) return false;
        ui::Interface& ui = *c_.hud->interface();
        ui::Widget* w = ui.Window(hud::kCollectionStory);
        if (w == nullptr) {
            const auto node = RootNode2dt(c_, kStoryLayout, "CNIFCollectionItemStoryWnd", hud::kCollectionStory);
            if (!node) return false;
            w = ui.Open(*node);
        } else {
            w->visible = true;
        }
        if (auto* story = dynamic_cast<StoryWindow*>(w)) story->Show(*item);
        return true;
    }

private:
    struct ThemeRow {
        ui::Widget *row = nullptr, *text = nullptr;
    };
    struct Slot {
        ui::Widget* slot = nullptr;
        ui::Static* picture = nullptr;
        ui::Static* effect = nullptr;
    };

    // The flip-book's step: the first frame shown at the start and again at the first tick (shows the
    // control on the frame left), then one a tick; hidden after the twelfth (the thirteenth frame is
    // one never set).
    void Flip(double now) {
        if (flipSlot_ < 0) return;
        if (flipStart_ < 0.0) flipStart_ = now;
        ui::Static* effect = slots_[std::size_t(flipSlot_)].effect;
        const int step = std::max(0, int((now - flipStart_) / kFlipStep) - 1);
        if (effect == nullptr || step >= kFlipFrames) {
            if (effect != nullptr) effect->visible = false;
            flipSlot_ = -1;
            return;
        }
        effect->visible = true;
        effect->uv0 = {float(step * 128) / 1664.0f, 0.0f};
        effect->uv1 = {float((step + 1) * 128) / 1664.0f, 0.65625f};
    }
    const assets::CollectionThemeRow* Theme() const {
        if (c_.collection == nullptr || std::size_t(theme_) >= c_.collection->Themes().size()) return nullptr;
        return &c_.collection->Themes()[std::size_t(theme_)];
    }
    bool Registered(std::uint32_t theme, int slot) const { return c_.book != nullptr && c_.book->contains(theme << 8 | std::uint32_t(slot)); }

    void Fill() {
        if (c_.collection == nullptr) return;
        const auto& themes = c_.collection->Themes();
        for (std::size_t i = 0; i < themes_.size(); ++i) {
            themes_[i].row->visible = i < themes.size();
            if (i >= themes.size()) continue;
            Put(themes_[i].text, c_.Text(themes[i].nameKey), int(i) == theme_ ? ui::Argb(0xFFFFD953) : ui::Argb(0xFFFFFFFF));
        }
        const assets::CollectionThemeRow* theme = Theme();
        if (description_ != nullptr) {
            description_->ClearParagraphs();
            if (theme != nullptr) {
                std::string text = c_.Text(theme->descKey);
                for (std::size_t at = text.find("\\n"); at != std::string::npos; at = text.find("\\n")) text.replace(at, 2, "\n");
                std::size_t start = 0;
                for (std::size_t end = text.find('\n'); ; end = text.find('\n', start)) {
                    description_->AddParagraph(text.substr(start, end == std::string::npos ? std::string::npos : end - start), ui::Argb(0xFFFFFFFF));
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
                description_->ScrollBy(-1000); // from its first line
            }
        }
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const int slot = int(i) + 1;
            const assets::CollectionItemRow* item = theme != nullptr ? c_.collection->Item(theme->codename, slot) : nullptr;
            if (slots_[i].picture == nullptr) continue;
            slots_[i].picture->texture = item != nullptr && Registered(theme->id, slot) ? item->picture : std::string(kEmptySlot);
        }
    }

    HudContext& c_;
    std::vector<ThemeRow> themes_;
    std::vector<Slot> slots_;
    ui::TextBox* description_ = nullptr;
    int theme_ = 0;
    std::size_t bookSize_ = ~std::size_t(0);
    bool dirty_ = true;
    int flipSlot_ = -1;
    double flipStart_ = -1.0;
};

class CollectionBox : public ui::MainFrame {
public:
    CollectionBox(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}
    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        const auto* records = c_.layout2dt ? c_.layout2dt(kBoxLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        if (ui::Widget* title = Child(0)) title->SetText(c_.Text(records->front().textKey));
        BuildWindow(ui, *this, *records);
    }
    // nifcollectionmsgbox.2dt: 9 the picture, 270 the name, 271 the question, 268 Confirm, 269 Cancel.
    void Show(std::uint8_t place, const assets::ItemRow& item) {
        place_ = place;
        const assets::CollectionItemRow* row = c_.collection != nullptr ? c_.collection->Item(std::string_view(item.codename)) : nullptr;
        if (auto* picture = dynamic_cast<ui::Static*>(Find(9))) picture->texture = row != nullptr ? row->picture : std::string();
        Put(Find(270), c_.Text(item.nameKey));
    }
    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && (n.control == 268 || n.control == 269)) {
            if (n.control == 268 && c_.instanceRequest) c_.instanceRequest(proto::RegisterTalisman::kId, proto::Encode(proto::RegisterTalisman{place_}));
            visible = false;
            return true;
        }
        return ui::MainFrame::OnNotice(n);
    }
    bool Answer(bool yes) {
        if (!visible) return false;
        if (yes && c_.instanceRequest) c_.instanceRequest(proto::RegisterTalisman::kId, proto::Encode(proto::RegisterTalisman{place_}));
        visible = false;
        return true;
    }

private:
    HudContext& c_;
    std::uint8_t place_ = 0;
};

ui::Widget* OpenWindow(HudContext& c, const char* layout, const char* type, int id) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return nullptr;
    if (ui::Widget* w = ui->Window(id)) {
        w->visible = true;
        return w;
    }
    const auto node = RootNode2dt(c, layout, type, id);
    return node ? ui->Open(*node) : nullptr;
}

} // namespace

void RegisterInstanceWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CNIFSummonPartyUserWnd", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<SummonWindow>(n, *c); });
    ui.Register("CNIFCollectionWnd", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<CollectionWindow>(n, *c); });
    ui.Register("CNIFCollectionItemStoryWnd", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<StoryWindow>(n, *c); });
    ui.Register("CNIFCollectionMsgBox", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<CollectionBox>(n, *c); });
}

void OpenSummonWindow(HudContext& c) { OpenWindow(c, kSummonLayout, "CNIFSummonPartyUserWnd", hud::kSummon); }

void ToggleCollectionBook(HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    if (ui::Widget* w = ui->Window(hud::kCollection); w != nullptr && w->visible) {
        w->visible = false;
        return;
    }
    OpenWindow(c, kBookLayout, "CNIFCollectionWnd", hud::kCollection);
}

void ShowCollectionBook(HudContext& c) {
    ui::Widget* w = OpenWindow(c, kBookLayout, "CNIFCollectionWnd", hud::kCollection);
    if (w != nullptr && c.hud->interface() != nullptr) c.hud->interface()->Raise(w);
}

void ShowTalismanRegistered(HudContext& c, std::uint32_t theme, int slot) {
    ui::Widget* w = OpenWindow(c, kBookLayout, "CNIFCollectionWnd", hud::kCollection);
    if (w == nullptr) return;
    if (ui::Interface* ui = c.hud->interface()) ui->Raise(w);
    if (auto* book = dynamic_cast<CollectionWindow*>(w)) book->ShowRegistered(theme, slot);
}

void OpenCollectionBox(HudContext& c, std::uint8_t place, const assets::ItemRow& item) {
    if (auto* box = dynamic_cast<CollectionBox*>(OpenWindow(c, kBoxLayout, "CNIFCollectionMsgBox", hud::kCollectionBox))) box->Show(place, item);
}

bool SummonByName(HudContext& c, const std::string& name) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    auto* w = ui != nullptr ? dynamic_cast<SummonWindow*>(ui->Window(hud::kSummon)) : nullptr;
    if (w == nullptr || !w->ChooseName(name)) return false;
    w->Summon();
    return true;
}

bool PickCollectionTheme(HudContext& c, int row) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    auto* w = ui != nullptr ? dynamic_cast<CollectionWindow*>(ui->Window(hud::kCollection)) : nullptr;
    return w != nullptr && w->PickTheme(row);
}

bool OpenCollectionStory(HudContext& c, int slot) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    auto* w = ui != nullptr ? dynamic_cast<CollectionWindow*>(ui->Window(hud::kCollection)) : nullptr;
    return w != nullptr && w->OpenStory(slot);
}

bool AnswerCollectionBox(HudContext& c, bool yes) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    auto* w = ui != nullptr ? dynamic_cast<CollectionBox*>(ui->Window(hud::kCollectionBox)) : nullptr;
    return w != nullptr && w->Answer(yes);
}

} // namespace sr::client
