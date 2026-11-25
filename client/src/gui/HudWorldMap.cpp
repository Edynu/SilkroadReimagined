// The world map (M): the 1.188 client's CNIFWorldMap, built from res_ui\worldmapguide.2dt
// (window 28, 650 x 422 and its bottom bar). Its view (HudMapView.h, 640 x 384 at 6, 34) shows the world, a place's map
// or a dungeon's floor; the left button drags it. Its buttons: the world (6: the world map, around the player), the pin
// (20: following the player, switching to the map the player stands in, centred on the player; any press in the view
// stops it), the floor spinner (4: up 22, down 24, its label 27; a dungeon of several floors). A place's picture on
// the world map opens that place's map. The player's arrow and the party's members are drawn on it.
// The guide (11, CNIFWorldMapGuide, worldmapguidedata.txt) opens by the side arrow (15); the bottom bar's monster box (31)
// marks the monsters near the player's level. Ours, against the original: the small window (7) and the tooltips are not
// built; the player's floor in a dungeon of several is not known yet (the shown floor is kept, else the first).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <cstring>
#include <format>
#include <unordered_set>

#include "gui/Hud.h"
#include "gui/HudMapView.h"
#include "gui/HudWindows.h"
#include "sr/assets/Texture.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr const char* kLayout = "res_ui\\worldmapguide.2dt";
constexpr int kWorldButton = 6, kPinButton = 20, kSizeButton = 7, kFloorBox = 4, kFloorUp = 22, kFloorDown = 24, kFloorLabel = 27;
constexpr int kGuide = 11, kGuideTab = 13, kGuideArrow = 15, kMonsterList = 30, kMonsterBox = 31;
constexpr float kPi = 3.14159265f;
const ui::Rect kView{6.0f, 34.0f, 640.0f, 384.0f};

const game::WorldView::Entity* Self(const HudContext& c) { return c.world != nullptr ? c.world->Find(c.world->Self()) : nullptr; }

// A square marker of `size` at `centre`, turned clockwise on the screen by `angle`.
void Turned(ui::Painter& paint, const std::string& path, ui::Vec2 centre, float size, float angle) {
    const float c = std::cos(angle), s = std::sin(angle), h = size / 2.0f;
    const ui::Vec2 corners[4] = {{-h, -h}, {h, -h}, {h, h}, {-h, h}};
    ui::Vec2 p[4];
    for (int i = 0; i < 4; ++i) p[i] = {centre.x + corners[i].x * c - corners[i].y * s, centre.y + corners[i].x * s + corners[i].y * c};
    const ui::Vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    paint.ImageQuad(path, p, uv);
}

// The map a place of the world is on: a place's (1..999), else a fortress's
// (1001..), else a dungeon floor of its region (the shown one when it is one of them), else the world.
int MapAt(const assets::WorldMapData& data, const proto::WorldPosition& at, int shown, const std::string& floor) {
    if ((at.region & 0x8000) != 0) {
        // The floor of the player's dungeon region whose name is the player's block's floor (worldmap_mapinfo's floor
        // string is the .dof's floor name: Donhwang's four floors share one region),
        // else the one shown, else the first.
        const std::vector<const assets::DungeonMapRow*> floors = data.DungeonsOf(at.region);
        const auto same = [](const std::string& a, const std::string& b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); });
        };
        for (const assets::DungeonMapRow* d : floors) {
            if (!floor.empty() && same(d->floorKey, floor)) return d->id;
        }
        for (const assets::DungeonMapRow* d : floors) {
            if (d->id == shown) return shown;
        }
        return floors.empty() ? -1 : floors.front()->id;
    }
    const int rx = at.region & 0xFF, rz = (at.region >> 8) & 0x7F;
    const double wx = at.x / 10.0 + (rx - 135) * 192.0, wz = at.z / 10.0 + (rz - 92) * 192.0;
    for (const bool fortress : {false, true}) {
        for (const assets::WorldMapRow& m : data.maps) {
            if (m.id == 0 || (m.id > 1000) != fortress || m.id >= 2000) continue;
            if (rx < m.regionLeft || rx > m.regionRight || rz < m.regionBottom || rz > m.regionTop) continue;
            if (wx >= m.Left() && wx <= m.Right() && wz >= m.Bottom() && wz <= m.Top()) return m.id;
        }
    }
    return 0;
}

class WorldMapWindow : public ui::MainFrame {
public:
    WorldMapWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override;
    void Update(ui::Frame& frame) override;
    bool OnNotice(const ui::Notice& n) override;

    void DrawMap(ui::Frame& frame, const ui::Rect& at);
    // A map shown, not following the player (the test options' and the gallery's way).
    void Open(int map) {
        following_ = false;
        choose_ = nullptr;
        if (ShowMap(map)) CentreOnPlayer();
    }
    // The reverse return scroll's choosing mode: the world, around
    // the player, with a marker at each map point; a click on one lists its destinations, a pick hands its ID to
    // `chosen` and closes the window (closing it any other way uses nothing).
    void Choose(std::function<void(std::uint32_t)> chosen) {
        following_ = false;
        choose_ = std::move(chosen);
        listFor_ = -1;
        if (ShowMap(0)) CentreOnPlayer();
    }
    // The gallery's and the tests' way: the destinations of marker `index` listed (the first marker: 0).
    void ListMarker(std::size_t index) {
        const std::vector<const assets::ReturnPointRow*> m = Markers();
        if (index < m.size()) listFor_ = m[index]->id;
    }
    void MapMouse(ui::Frame& frame, const ui::Widget& owner, const ui::Rect& at);
    // A destination picked: the window closed, its ID handed on.
    void PickPoint(std::uint32_t id);
    bool Choosing() const { return static_cast<bool>(choose_); }
    // The guide (11): shown or hidden by the side arrow (15); its panel's drawing and clicks.
    void ToggleGuide();
    void DrawGuide(ui::Frame& frame, const ui::Rect& at);
    void GuideMouse(ui::Frame& frame, const ui::Widget& owner, const ui::Rect& at);
    // The gallery's and the tests' way: the guide shown on a tab, its heading `heading` opened, that heading's entry
    // `entry` picked (-1: none).
    void GuideStep(int tab, int heading, int entry);
    // The monster box ticked or not, as a click on it.
    void TickMonsterBox(bool on) {
        if (auto* box = ui::As<ui::CheckBox>(Child(kMonsterBox))) box->checked = on;
        LevelMarks(on);
    }

private:
    // The player's dungeon floor's name (the .dof's: "DH_A01_FLOOR02"; empty outdoors or not known yet).
    std::string PlayerFloor(const proto::WorldPosition& at) const { return c_.dungeon.region == at.region ? c_.dungeon.floor : std::string(); }
    // A map shown by its id (false: none such, or later content); the title its name.
    bool ShowMap(int id) {
        if (c_.worldMap == nullptr || !view_.Show(*c_.worldMap, id)) return false;
        view_.SetViewSize({kView.w, kView.h});
        std::string title;
        if (const assets::WorldMapRow* m = view_.Map()) title = c_.Text(m->nameKey);
        if (const assets::DungeonMapRow* d = view_.Dungeon()) title = c_.Text(d->floorKey);
        if (ui::Widget* t = Child(0)) t->SetText(title);
        Floors();
        return true;
    }
    // The player's place on the shown map, centred (the pin's, the world button's).
    void CentreOnPlayer() {
        const game::WorldView::Entity* self = Self(c_);
        if (self == nullptr) return;
        if (const auto p = view_.Pixel(self->position.region, self->position.x, self->position.z)) view_.CentreOn(*p);
    }
    // The floor spinner: shown for a floor of a dungeon of several, its label the floor's.
    void Floors() {
        const assets::DungeonMapRow* d = view_.Dungeon();
        if (ui::Widget* box = Child(kFloorBox)) box->visible = d != nullptr && d->floors > 1;
        if (ui::Widget* box = Child(kFloorBox)) {
            if (ui::Widget* label = box->Child(kFloorLabel)) label->SetText(d != nullptr ? d->Label() : std::string());
        }
    }
    // Up: F floors count up, B floors toward the surface.
    void Step(bool up) {
        const assets::DungeonMapRow* d = view_.Dungeon();
        if (d == nullptr || c_.worldMap == nullptr) return;
        const int next = d->floor + ((d->letter == 'F') == up ? 1 : -1);
        if (const assets::DungeonMapRow* f = c_.worldMap->Floor(*d, next)) {
            following_ = false;
            ShowMap(f->id);
        }
    }
    // The pin's art: automatic while following, else manual.
    void PinArt() {
        if (auto* b = ui::As<ui::Button>(Child(kPinButton))) {
            b->texture = following_ ? "interface\\worldmap\\wmap_button_automatic.ddj" : "interface\\worldmap\\wmap_button_passivity.ddj";
        }
    }

    // The map points with a marker of their own (a group's flag stands for its rows).
    std::vector<const assets::ReturnPointRow*> Markers() const {
        std::vector<const assets::ReturnPointRow*> out;
        if (c_.worldMap == nullptr) return out;
        for (const assets::ReturnPointRow& r : c_.worldMap->returnPoints) {
            if (r.marker) out.push_back(&r);
        }
        return out;
    }
    // A marker's destinations: itself, or its group's rows (a cave's or a tomb's floors).
    std::vector<const assets::ReturnPointRow*> Destinations(const assets::ReturnPointRow& marker) const {
        std::vector<const assets::ReturnPointRow*> out;
        if (marker.group < 0) {
            out.push_back(&marker);
            return out;
        }
        for (const assets::ReturnPointRow& r : c_.worldMap->returnPoints) {
            if (r.group == marker.group && !r.zoneKey.empty()) out.push_back(&r);
        }
        return out;
    }
    // Where a marker and its list stand on the screen (the list's rows 24 apart beside it).
    std::optional<ui::Vec2> MarkerAt(const assets::ReturnPointRow& r, const ui::Rect& at) const {
        const auto p = view_.Pixel(r.region, r.x, r.z);
        if (!p) return std::nullopt;
        return view_.ToScreen(*p, at);
    }
    static ui::Rect ListRow(ui::Vec2 marker, std::size_t i) { return {std::floor(marker.x + 18.0f), std::floor(marker.y - 12.0f + 24.0f * float(i)), 150.0f, 24.0f}; }

    // The guide's list: its tab's rows in the player's race's order, a heading's entries shown while it is open.
    struct GuideLine {
        const assets::WorldMapGuideRow* row = nullptr;
        int group = 0; // its heading's (the headings counted from 0)
    };
    std::vector<const assets::WorldMapGuideRow*> GuideRows() const;
    std::vector<GuideLine> GuideLines() const;
    std::string GuideText(const assets::WorldMapGuideRow& row, ui::Color& colour) const;
    void GuidePick(const assets::WorldMapGuideRow& row);
    int guideTab_ = 0, guideOpen_ = -1, guideTop_ = 0;
    const assets::WorldMapGuideRow* guidePicked_ = nullptr;
    // A monster marked: picked in the guide (one), or every monster near the player's level (the monster box, 31): its
    // map, its tiles tinted in its colour and its icon there.
    struct MonsterMark {
        int map = 0;
        std::string codename, area; // its area names its tiles' pictures
        ui::Vec2 pixel;
        std::uint32_t argb = 0; // its colour (ARGB, the original's)
        std::vector<std::uint32_t> keys; // its areas' pixels in those pictures
    };
    std::vector<MonsterMark> marks_;
    // A monster's mark (none: its area has no map); `keep` the others.
    void MarkMonster(const assets::WorldMapGuideRow& row, std::uint32_t argb, bool keep);
    // The monster box ticked or not: the monsters 0..3 levels over the player's (0x80D9D2C6) and 4..6 over (0x80B63143)
    // marked, the others not (the original's on_31); unticked, none.
    void LevelMarks(bool on);
    bool levelPending_ = false; // ticked before the tables were in: marked once they are

    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    MapView view_;
    std::function<void(std::uint32_t)> choose_; // choosing mode (the reverse return scroll's)
    int listFor_ = -1;                          // the marker whose destinations are listed (its row's ID)
    bool following_ = true; // the pin (inferred: on when the window first opens)
    bool built_ = false;
    ui::Vec2 pressedAt_{-100.0f, -100.0f};
};

// The map's part of the window: drawn and dragged by the window.
class WorldMapView : public ui::Widget {
public:
    explicit WorldMapView(const assets::UiLayoutNode& node) : Widget(node) { interactive = true; }
    void Draw(ui::Frame& frame) override {
        if (auto* window = dynamic_cast<WorldMapWindow*>(Root())) window->DrawMap(frame, ScreenRect());
    }
    void Mouse(ui::Frame& frame) override {
        if (auto* window = dynamic_cast<WorldMapWindow*>(Root())) window->MapMouse(frame, *this, ScreenRect());
    }
};

// The guide's panel over its frame: drawn and clicked by the window.
class GuidePanel : public ui::Widget {
public:
    explicit GuidePanel(const assets::UiLayoutNode& node) : Widget(node) { interactive = true; }
    void Draw(ui::Frame& frame) override {
        if (auto* window = dynamic_cast<WorldMapWindow*>(Root())) window->DrawGuide(frame, ScreenRect());
    }
    void Mouse(ui::Frame& frame) override {
        if (auto* window = dynamic_cast<WorldMapWindow*>(Root())) window->GuideMouse(frame, *this, ScreenRect());
    }
};

void WorldMapWindow::Created(ui::Interface& ui) {
    MainFrame::Created(ui);
    ui_ = &ui;
    // The view first, so the 2dt's buttons over it are drawn and clicked before it.
    assets::UiLayoutNode viewNode;
    viewNode.type = "CNIFWorldMapView";
    viewNode.id = 9000;
    viewNode.rect = {kView.x, kView.y, kView.w, kView.h};
    Add(std::make_unique<WorldMapView>(viewNode));
    const auto* records = c_.layout2dt ? c_.layout2dt(kLayout) : nullptr;
    if (records == nullptr || records->empty()) return;
    BuildGuild2dt(ui, *records, *this, records->front().rect.x, records->front().rect.y);
    // Not built yet: the monster list and its box, the size button. The guide (11) hidden at the start, its side tab
    // (13) and arrow (15) shown (the original's Create); its panel made over its frame.
    for (const auto& child : children()) {
        ui::Widget* w = child.get();
        const int id = w->id();
        if (id == kSizeButton) w->visible = false;
        if (auto* box = ui::As<ui::CheckBox>(w); box != nullptr && id == kMonsterBox) box->checked = false; // unticked at the start
    }
    if (ui::Widget* guide = Child(kGuide)) {
        assets::UiLayoutNode panel;
        panel.type = "CNIFWorldMapGuidePanel";
        panel.id = 9001;
        panel.rect = {0.0f, 0.0f, guide->rect.w, guide->rect.h};
        guide->Add(std::make_unique<GuidePanel>(panel));
        guide->visible = false;
    }
    built_ = true;
    Floors();
}

void WorldMapWindow::Update(ui::Frame& frame) {
    MainFrame::Update(frame);
    if (c_.worldMap == nullptr) return;
    const game::WorldView::Entity* self = Self(c_);
    if (!view_.Shown()) {
        const int id = self != nullptr ? MapAt(*c_.worldMap, self->position, -1, PlayerFloor(self->position)) : 0;
        if (!ShowMap(id < 0 ? 0 : id)) ShowMap(0);
        CentreOnPlayer();
    }
    if (levelPending_) LevelMarks(true);
    // Following the player: the map it stands in, centred on it.
    if (following_ && self != nullptr) {
        const int id = MapAt(*c_.worldMap, self->position, view_.Id(), PlayerFloor(self->position));
        if (id >= 0 && id != view_.Id()) ShowMap(id);
        CentreOnPlayer();
    }
    PinArt();
}

bool WorldMapWindow::OnNotice(const ui::Notice& n) {
    if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr) {
        switch (n.control) {
        case kWorldButton: // the world, around the player
            following_ = false;
            ShowMap(0);
            CentreOnPlayer();
            return true;
        case kPinButton: following_ = !following_; return true;
        case kGuideArrow: ToggleGuide(); return true;
        case kMonsterBox:
            if (auto* box = ui::As<ui::CheckBox>(n.source)) LevelMarks(box->checked);
            return true;
        case kFloorUp: Step(true); return true;
        case kFloorDown: Step(false); return true;
        default: break;
        }
    }
    return MainFrame::OnNotice(n);
}

void WorldMapWindow::DrawMap(ui::Frame& frame, const ui::Rect& at) {
    view_.Draw(frame.paint, at);
    view_.DrawLabels(frame.paint, at, [this](const std::string& key) { return c_.Text(key); });
    frame.paint.PushClip(at);
    // The fortresses' crest boxes on the world: at siegefortressreward.txt's place, 32 pixels to a
    // region, the holding guild's emblem in fort_worldmap_iconbox1.ddj (28 x 32; the union's beside it in iconbox2 is
    // not drawn yet); hovered, the fortress, its guild, master and introduction.
    if (view_.Id() == 0 && c_.worldMap != nullptr && c_.fortress != nullptr && view_.Map() != nullptr) {
        for (const assets::FortressMapPlace& p : c_.worldMap->fortresses) {
            const proto::FortressRecord* fort = nullptr;
            for (const proto::FortressRecord& f : c_.fortress->Fortresses()) {
                if (f.nameKey == "SN_" + p.fortress) fort = &f;
            }
            if (fort == nullptr) continue;
            const std::uint32_t guild = fort->holder != 0 ? fort->holder : fort->owner;
            if (guild == 0) continue;
            assets::WorldMapLabel place;
            place.regionX = p.regionX;
            place.regionZ = p.regionZ;
            place.x = p.x;
            place.y = p.y;
            const ui::Vec2 s = view_.ToScreen(view_.LabelPixel(place), at);
            const ui::Rect box{std::floor(s.x), std::floor(s.y), 28.0f, 32.0f};
            frame.paint.Image("interface\\worldmap\\map\\fort_worldmap_iconbox1.ddj", box);
            if (const std::string crest = c_.guildCrest ? c_.guildCrest(guild, fort->crest) : std::string(); !crest.empty()) {
                frame.paint.Image(crest, {box.x + 6.0f, box.y + 6.0f, 16.0f, 16.0f});
            }
            const ui::Vec2 m = frame.pointer.position;
            if (m.x >= box.x && m.y >= box.y && m.x < box.Right() && m.y < box.Bottom()) {
                std::string tip = c_.Text(fort->nameKey) + "\n" + c_.Text("UIIT_STT_FORT_OFFICAL_OCCUPYGUILD") + " : " +
                                  (fort->holder != 0 ? fort->holderName : fort->ownerName) + "\n" + c_.Text("UIIT_STT_MAP_FORTRESS_GUILD_MASTER") + " : " +
                                  fort->masterName;
                if (!fort->introduction.empty()) tip += "\n" + fort->introduction;
                frame.interface.ShowTooltip(tip, box);
            }
        }
    }
    // The monsters marked (MonsterMark): each its tiles tinted in its colour (a world tile 4 x 4 regions, a dungeon's one
    // region each), then each its icon (wmap_monster_icon.ddj) at its place.
    for (const MonsterMark& mark : marks_) {
        if (view_.Id() != mark.map || c_.worldMap == nullptr) continue;
        const auto tiles = c_.worldMap->guide.monsterTiles.find(mark.codename);
        if (tiles != c_.worldMap->guide.monsterTiles.end()) {
            for (const auto& [tx, tz] : tiles->second) {
                ui::Rect px;
                if (const assets::WorldMapRow* m = view_.Map()) {
                    const float side = 32.0f * float(std::max(1, m->tileRegions));
                    px = {float(tx - m->regionLeft) * 32.0f, float(m->regionTop - tz) * 32.0f, side, side};
                } else if (const assets::DungeonMapRow* d = view_.Dungeon(); d != nullptr && d->tilesAcross > 0 && d->tilesDown > 0) {
                    const float w = float(d->drawW) / float(d->tilesAcross), h = float(d->drawH) / float(d->tilesDown);
                    px = {float(tx - d->regionLeft) * w, float(d->regionTop - tz) * h, w, h};
                } else {
                    continue;
                }
                const ui::Vec2 s = view_.ToScreen({px.x, px.y}, at);
                // The tile's picture of the areas (interface\worldmap\monster\<area>_<x>x<z>.ddj: each area's pixels one
                // 32-bit key): the monster's keys in its colour, the rest clear (KeyedPicture).
                if (!c_.keyedPicture) continue;
                const std::string source = "interface\\worldmap\\monster\\" + mark.area + "_" + std::to_string(tx) + "x" + std::to_string(tz) + ".ddj";
                if (const std::string picture = c_.keyedPicture(source, mark.keys, mark.argb); !picture.empty()) {
                    frame.paint.Image(picture, {s.x, s.y, px.w, px.h});
                }
            }
        }
    }
    for (const MonsterMark& mark : marks_) {
        if (view_.Id() != mark.map) continue;
        const ui::Vec2 s = view_.ToScreen(mark.pixel, at);
        frame.paint.Image("interface\\worldmap\\wmap_monster_icon.ddj", {std::floor(s.x - 8.0f), std::floor(s.y - 8.0f), 16.0f, 16.0f});
    }
    // The party's members, then the player's arrow (mm_sign_character.ddj, turned by its yaw).
    if (c_.state != nullptr) {
        for (const proto::PartyMemberRecord* m : c_.state->party.Others()) {
            if (m->position.region == 0) continue;
            const auto p = view_.Pixel(m->position.region, m->position.x, m->position.z);
            if (!p) continue;
            const ui::Vec2 s = view_.ToScreen(*p, at);
            frame.paint.Image("interface\\worldmap\\wmap_sign_party.ddj", {std::floor(s.x - 8.0f), std::floor(s.y - 8.0f), 16.0f, 16.0f});
        }
        // The academy's members on (wmap_sign_apprenticeship.ddj, 16 x 16; 1.188 loads it beside the party's;
        // drawn as the party's, ours).
        const game::AcademyView& academy = c_.state->academy;
        for (const proto::AcademyMember& m : academy.Info().members) {
            if (m.online == 0 || m.name == c_.state->self.name) continue;
            const auto place = academy.Places().find(m.character);
            if (place == academy.Places().end() || place->second.region == 0) continue;
            const auto p = view_.Pixel(place->second.region, place->second.x, place->second.z);
            if (!p) continue;
            const ui::Vec2 s = view_.ToScreen(*p, at);
            frame.paint.Image("interface\\worldmap\\wmap_sign_apprenticeship.ddj", {std::floor(s.x - 8.0f), std::floor(s.y - 8.0f), 16.0f, 16.0f});
        }
    }
    if (const game::WorldView::Entity* self = Self(c_)) {
        if (const auto p = view_.Pixel(self->position.region, self->position.x, self->position.z)) {
            Turned(frame.paint, "interface\\minimap\\mm_sign_character.ddj", view_.ToScreen(*p, at), 16.0f, self->yaw - kPi / 2.0f);
        }
    }
    // Choosing: the map points' markers (32 x 32, xy_returnicon.ddj and its _focus / _press forms), and
    // the listed one's destinations beside it (map_return.ddj, the zone's name).
    if (choose_) {
        const ui::Vec2 mouse = frame.pointer.position;
        for (const assets::ReturnPointRow* r : Markers()) {
            const auto s = MarkerAt(*r, at);
            if (!s) continue;
            const ui::Rect box{std::floor(s->x - 16.0f), std::floor(s->y - 16.0f), 32.0f, 32.0f};
            const bool over = mouse.x >= box.x && mouse.y >= box.y && mouse.x < box.Right() && mouse.y < box.Bottom();
            // The plain form is a strip of two 32 x 32 frames (64 x 32, the second brighter): its first drawn, never the
            // strip squeezed into one marker (the user's report 2026-11-22: the points showed split in half until
            // hovered). Inferred: whether the original alternates the two, and how fast, is not traced.
            if (r->id == listFor_) {
                frame.paint.Image("interface\\worldmap\\map\\xy_returnicon_press.ddj", box);
            } else if (over) {
                frame.paint.Image("interface\\worldmap\\map\\xy_returnicon_focus.ddj", box);
            } else {
                frame.paint.Image("interface\\worldmap\\map\\xy_returnicon.ddj", box, {0.0f, 0.0f}, {0.5f, 1.0f});
            }
        }
        for (const assets::ReturnPointRow* r : Markers()) {
            if (r->id != listFor_) continue;
            const auto s = MarkerAt(*r, at);
            if (!s) break;
            const std::vector<const assets::ReturnPointRow*> list = Destinations(*r);
            for (std::size_t i = 0; i < list.size(); ++i) {
                const ui::Rect row = ListRow(*s, i);
                frame.paint.Image("interface\\worldmap\\map\\map_return.ddj", row);
                frame.paint.Text(c_.Text(list[i]->zoneKey), row, ui::Rgba(255, 254, 216), 0, 1, 1);
            }
            break;
        }
    }
    frame.paint.PopClip();
}

// --- The guide (CNIFWorldMapGuide, 11) ----------------------------------------------------------------------------------
// Left of the map, opened and closed by the side arrow. Its tabs: the data's sections in service (the places, the
// monsters), 56 x 24 from 20 across, 60 apart, 8 down, their _on art when chosen (_off otherwise) and their label. Its
// list in the black tile (22, 43; 257 x 235): rows of 248 x 28, 30 apart from 3 down. A heading
// (gd_long_index_open.ddj, its name in tan from 34, 6, its button gd_index_button_open while open, _close shut) opens its
// entries under it, one heading at a time, a second click shutting it; an entry (gd_long_contents_event.ddj, the lamp
// gd_contents_lamp.ddj while picked) shows its place on the map. A monster's entry reads "Lv12 name", red over the
// player's level + 6, pink over + 3, light blue under the player's. Ours: the wheel scrolls the list; its scroll bar
// (gd_scroll_*) and the lamp's place (beside the name) are inferred / not drawn.
constexpr float kGuideRowX = 24.0f, kGuideRowY = 46.0f, kGuideRowW = 248.0f, kGuideRowH = 28.0f, kGuidePitch = 30.0f;
const ui::Rect kGuideList{22.0f, 43.0f, 257.0f, 235.0f};

void WorldMapWindow::ToggleGuide() {
    ui::Widget* guide = Child(kGuide);
    if (guide == nullptr) return;
    guide->visible = !guide->visible;
}

void WorldMapWindow::MarkMonster(const assets::WorldMapGuideRow& row, std::uint32_t argb, bool keep) {
    if (!keep) marks_.clear();
    if (c_.worldMap == nullptr) return;
    const int map = c_.worldMap->guide.MapOfArea(row.area);
    if (map < 0) return;
    // Its place: on the world, a region and its offset there (32 pixels to a region); on a dungeon floor, its pixel.
    MapView place;
    if (!place.Show(*c_.worldMap, map)) return;
    assets::WorldMapLabel at;
    at.map = map;
    at.regionX = row.regionX;
    at.regionZ = row.regionZ;
    at.x = row.placeX;
    at.y = row.placeY;
    marks_.push_back({map, row.codename, row.area, place.LabelPixel(at), argb, row.keys});
}

void WorldMapWindow::LevelMarks(bool on) {
    marks_.clear();
    levelPending_ = on && (c_.worldMap == nullptr || c_.characters == nullptr || c_.state == nullptr);
    if (!on || levelPending_) return;
    for (const assets::WorldMapGuideTab& t : c_.worldMap->guide.tabs) {
        if (t.section != "MONSTER") continue;
        for (const assets::WorldMapGuideRow& r : t.rows) {
            const assets::ObjectRow* monster = r.heading ? nullptr : c_.characters->Find(std::string_view(r.codename));
            if (monster == nullptr) continue;
            const int over = monster->level - int(c_.state->self.level);
            if (over >= 0 && over < 4) MarkMonster(r, 0x80D9D2C6u, true);
            else if (over >= 4 && over < 7) MarkMonster(r, 0x80B63143u, true);
        }
    }
}

std::vector<const assets::WorldMapGuideRow*> WorldMapWindow::GuideRows() const {
    std::vector<const assets::WorldMapGuideRow*> rows;
    if (c_.worldMap == nullptr || guideTab_ < 0 || guideTab_ >= int(c_.worldMap->guide.tabs.size())) return rows;
    for (const assets::WorldMapGuideRow& r : c_.worldMap->guide.tabs[std::size_t(guideTab_)].rows) rows.push_back(&r);
    const std::size_t race = c_.state != nullptr && c_.state->self.race == 1 ? 1 : 0;
    std::stable_sort(rows.begin(), rows.end(), [race](const auto* a, const auto* b) { return a->order[race] < b->order[race]; });
    return rows;
}

std::vector<WorldMapWindow::GuideLine> WorldMapWindow::GuideLines() const {
    std::vector<GuideLine> out;
    int group = -1;
    for (const assets::WorldMapGuideRow* r : GuideRows()) {
        if (r->heading) {
            out.push_back({r, ++group});
        } else if (group >= 0 && group == guideOpen_) {
            out.push_back({r, group});
        }
    }
    return out;
}

std::string WorldMapWindow::GuideText(const assets::WorldMapGuideRow& row, ui::Color& colour) const {
    colour = row.heading ? ui::Argb(0xFFEFDAA4) : ui::kWhite;
    if (row.heading || !row.nameKey.empty()) {
        if (!row.nameKey.empty() && (!c_.hasText || c_.hasText(row.nameKey))) return c_.Text(row.nameKey);
        // A monsters' heading with no text of its own (DUNGEON_TEMPLE): the places' row of that codename.
        if (c_.worldMap != nullptr) {
            for (const assets::WorldMapGuideTab& t : c_.worldMap->guide.tabs) {
                for (const assets::WorldMapGuideRow& r : t.rows) {
                    if (&r != &row && r.codename == row.codename && !r.nameKey.empty()) return c_.Text(r.nameKey);
                }
            }
        }
        return row.codename;
    }
    // A monster: "Lv" its level and its name, coloured by its level against the player's.
    const assets::ObjectRow* monster = c_.characters != nullptr ? c_.characters->Find(std::string_view(row.codename)) : nullptr;
    if (monster == nullptr) return row.codename;
    const int mine = c_.state != nullptr ? int(c_.state->self.level) : 0;
    if (monster->level > mine + 6) colour = ui::Argb(0xFFFF0000);
    else if (monster->level > mine + 3) colour = ui::Argb(0xFFEF8282);
    else if (monster->level < mine) colour = ui::Argb(0xFF80C0E5);
    return c_.Text("UIIT_STT_LEVEL_LV") + std::to_string(monster->level) + " " + c_.Text(monster->nameKey);
}

void WorldMapWindow::GuidePick(const assets::WorldMapGuideRow& row) {
    if (c_.worldMap == nullptr) return;
    guidePicked_ = &row;
    following_ = false;
    // Any pick: the monster box unticked, the marks gone (the original's pick first takes the box's back).
    if (auto* box = ui::As<ui::CheckBox>(Child(kMonsterBox))) box->checked = false;
    marks_.clear();
    const assets::WorldMapGuide& guide = c_.worldMap->guide;
    if (guideTab_ < int(guide.tabs.size()) && guide.tabs[std::size_t(guideTab_)].section == "AREA") {
        // A place: its map (a map id), or its label's map centred on it (a worldmap_localinfo row).
        if (row.linkKind == 0) {
            ShowMap(row.link);
            return;
        }
        for (const assets::WorldMapLabel& l : c_.worldMap->labels) {
            if (l.id != row.link) continue;
            if (!ShowMap(l.map)) return;
            const ui::Vec2 p = view_.LabelPixel(l);
            view_.CentreOn({p.x + float(l.width) / 2.0f, p.y + float(l.height) / 2.0f});
            return;
        }
        return;
    }
    // A monster: its area's map centred on its place, its icon there and its tiles tinted (0x80E9C624); the monster box
    // unticked, its marks gone.
    if (auto* box = ui::As<ui::CheckBox>(Child(kMonsterBox))) box->checked = false;
    MarkMonster(row, 0x80E9C624u, false);
    if (marks_.empty() || !ShowMap(marks_.back().map)) return;
    view_.CentreOn(marks_.back().pixel);
}

void WorldMapWindow::DrawGuide(ui::Frame& frame, const ui::Rect& at) {
    if (c_.worldMap == nullptr) return;
    const assets::WorldMapGuide& guide = c_.worldMap->guide;
    for (std::size_t i = 0; i < guide.tabs.size(); ++i) {
        const assets::WorldMapGuideTab& t = guide.tabs[i];
        const ui::Rect r{at.x + 20.0f + 60.0f * float(i), at.y + 8.0f, 56.0f, 24.0f};
        std::string art = t.art;
        if (int(i) != guideTab_) {
            if (const std::size_t on = art.rfind("_on.ddj"); on != std::string::npos) art.replace(on, 7, "_off.ddj");
        }
        frame.paint.Image(art, r);
        frame.paint.Text(c_.Text(t.labelKey), r, ui::kWhite, 0, 1, 1);
    }
    const ui::Rect list{at.x + kGuideList.x, at.y + kGuideList.y, kGuideList.w, kGuideList.h};
    frame.paint.PushClip(list);
    const std::vector<GuideLine> lines = GuideLines();
    for (std::size_t k = std::size_t(std::max(0, guideTop_)); k < lines.size(); ++k) {
        const float y = at.y + kGuideRowY + kGuidePitch * float(int(k) - guideTop_);
        if (y > list.y + list.h) break;
        const assets::WorldMapGuideRow& row = *lines[k].row;
        const ui::Rect r{at.x + kGuideRowX, y, kGuideRowW, kGuideRowH};
        ui::Color colour = ui::kWhite;
        const std::string text = GuideText(row, colour);
        if (row.heading) {
            frame.paint.Image("interface\\guide\\gd_long_index_open.ddj", r);
            frame.paint.Image(lines[k].group == guideOpen_ ? "interface\\guide\\gd_index_button_open.ddj" : "interface\\guide\\gd_index_button_close.ddj",
                              {r.x + 6.0f, r.y + 2.0f, 24.0f, 24.0f});
        } else {
            frame.paint.Image("interface\\guide\\gd_long_contents_event.ddj", r);
            if (&row == guidePicked_) frame.paint.Image("interface\\guide\\gd_contents_lamp.ddj", {r.x + 22.0f, r.y + 6.0f, 8.0f, 16.0f});
        }
        frame.paint.Text(text, {r.x + 34.0f, r.y + 6.0f, r.w - 38.0f, 16.0f}, colour, 0, 0, 1);
    }
    frame.paint.PopClip();
}

void WorldMapWindow::GuideMouse(ui::Frame& frame, const ui::Widget& owner, const ui::Rect& at) {
    if (c_.worldMap == nullptr || frame.pointer.hovered != &owner) return;
    const ui::Vec2 m = frame.pointer.position;
    const std::vector<GuideLine> lines = GuideLines();
    if (frame.input.wheel() != 0.0f) {
        const int most = std::max(0, int(lines.size()) - int(kGuideList.h / kGuidePitch));
        guideTop_ = std::clamp(guideTop_ + (frame.input.wheel() > 0.0f ? -1 : 1), 0, most);
    }
    if (!frame.input.Pressed(ui::MouseButton::Left)) return;
    const auto inside = [&](const ui::Rect& r) { return m.x >= r.x && m.y >= r.y && m.x < r.Right() && m.y < r.Bottom(); };
    for (std::size_t i = 0; i < c_.worldMap->guide.tabs.size(); ++i) {
        if (!inside({at.x + 20.0f + 60.0f * float(i), at.y + 8.0f, 56.0f, 24.0f})) continue;
        if (int(i) != guideTab_) GuideStep(int(i), -1, -1);
        return;
    }
    if (!inside({at.x + kGuideList.x, at.y + kGuideList.y, kGuideList.w, kGuideList.h})) return;
    for (std::size_t k = std::size_t(std::max(0, guideTop_)); k < lines.size(); ++k) {
        const ui::Rect r{at.x + kGuideRowX, at.y + kGuideRowY + kGuidePitch * float(int(k) - guideTop_), kGuideRowW, kGuideRowH};
        if (!inside(r)) continue;
        if (lines[k].row->heading) {
            guideOpen_ = guideOpen_ == lines[k].group ? -1 : lines[k].group;
        } else {
            GuidePick(*lines[k].row);
        }
        return;
    }
}

void WorldMapWindow::GuideStep(int tab, int heading, int entry) {
    if (ui::Widget* guide = Child(kGuide)) guide->visible = true;
    if (c_.worldMap == nullptr || tab < 0 || tab >= int(c_.worldMap->guide.tabs.size())) return;
    if (tab != guideTab_) {
        guideTab_ = tab;
        guideOpen_ = -1;
        guideTop_ = 0;
        guidePicked_ = nullptr;
    }
    if (heading < 0) return;
    guideOpen_ = heading;
    // The heading's line at the top, its entries under it.
    const std::vector<GuideLine> lines = GuideLines();
    for (std::size_t k = 0; k < lines.size(); ++k) {
        if (!lines[k].row->heading || lines[k].group != heading) continue;
        guideTop_ = int(k);
        if (entry >= 0 && k + 1 + std::size_t(entry) < lines.size() && !lines[k + 1 + std::size_t(entry)].row->heading) {
            GuidePick(*lines[k + 1 + std::size_t(entry)].row);
        }
        break;
    }
}

void WorldMapWindow::PickPoint(std::uint32_t id) {
    auto chosen = std::move(choose_);
    choose_ = nullptr;
    listFor_ = -1;
    if (c_.hud != nullptr) c_.hud->Hide(hud::kWorldMap);
    if (chosen) chosen(id);
}

void WorldMapWindow::MapMouse(ui::Frame& frame, const ui::Widget& owner, const ui::Rect& at) {
    bool pressed = false;
    view_.Drag(frame, owner, at, &pressed);
    if (pressed) {
        following_ = false; // a press in the view stops following
        pressedAt_ = frame.pointer.position;
    }
    // A click (no drag between) on a place's picture opens its map.
    if (frame.input.Released(ui::MouseButton::Left) && frame.pointer.hovered == &owner) {
        const ui::Vec2 m = frame.pointer.position;
        if (std::fabs(m.x - pressedAt_.x) < 4.0f && std::fabs(m.y - pressedAt_.y) < 4.0f && choose_) {
            // Choosing: a listed destination picked, else a marker's list opened.
            const auto inside = [&](const ui::Rect& r) { return m.x >= r.x && m.y >= r.y && m.x < r.Right() && m.y < r.Bottom(); };
            for (const assets::ReturnPointRow* r : Markers()) {
                if (r->id != listFor_) continue;
                const auto s = MarkerAt(*r, at);
                if (!s) break;
                const std::vector<const assets::ReturnPointRow*> list = Destinations(*r);
                for (std::size_t i = 0; i < list.size(); ++i) {
                    if (!inside(ListRow(*s, i))) continue;
                    PickPoint(list[i]->id);
                    return;
                }
                break;
            }
            for (const assets::ReturnPointRow* r : Markers()) {
                const auto s = MarkerAt(*r, at);
                if (s && inside({s->x - 16.0f, s->y - 16.0f, 32.0f, 32.0f})) {
                    listFor_ = r->id;
                    return;
                }
            }
            listFor_ = -1;
            return;
        }
        if (std::fabs(m.x - pressedAt_.x) < 4.0f && std::fabs(m.y - pressedAt_.y) < 4.0f) {
            if (const assets::WorldMapLabel* link = view_.LinkAt(m, at)) {
                if (ShowMap(link->opens)) CentreOnPlayer();
            }
        }
    }
}

} // namespace

void ShowWorldMap(HudContext& c, int map) {
    if (c.hud == nullptr) return;
    if (auto* window = dynamic_cast<WorldMapWindow*>(c.hud->Show(hud::kWorldMap))) window->Open(map);
}

std::string KeyedPicture(const assets::FileSystem& files, assets::MemorySource& made, const std::string& source,
                         const std::vector<std::uint32_t>& keys, std::uint32_t colour) {
    // Its path from the source, the keys and the colour (FNV-1a); made once.
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) h = (h ^ ((v >> (8 * i)) & 0xFFu)) * 1099511628211ull;
    };
    for (const std::uint32_t k : keys) mix(k);
    mix(colour);
    std::string base = source;
    for (char& ch : base) {
        if (ch == '\\' || ch == '/' || ch == '.') ch = '_';
    }
    const std::string path = std::format("made\\keyed\\{}_{:016x}.dds", base, h);
    static std::unordered_set<std::string> madeOnce, missing;
    if (madeOnce.contains(path)) return path;
    if (missing.contains(path)) return {};
    const auto file = files.Read(source);
    assets::TextureData texture;
    std::string error;
    if (!file || !assets::DecodeDdj(file->Bytes(), texture, error) || texture.mips.empty()) {
        missing.insert(path);
        return {};
    }
    const std::vector<std::uint8_t> rgba = assets::DecodeToRgba8(texture, 0);
    const std::uint32_t w = texture.mips[0].width, ht = texture.mips[0].height;
    // A 32-bit DDS (A8R8G8B8: B, G, R, A in memory), as the emblems are made.
    std::vector<std::byte> out(128 + std::size_t(w) * ht * 4);
    const auto put = [&](std::size_t at, std::uint32_t v) { std::memcpy(out.data() + at, &v, 4); };
    std::memcpy(out.data(), "DDS ", 4);
    put(4, 124);
    put(8, 0x1 | 0x2 | 0x4 | 0x1000);
    put(12, ht);
    put(16, w);
    put(76, 32);
    put(80, 0x40 | 0x1);
    put(88, 32);
    put(92, 0x00FF0000u);
    put(96, 0x0000FF00u);
    put(100, 0x000000FFu);
    put(104, 0xFF000000u);
    put(108, 0x1000);
    for (std::size_t i = 0; i < std::size_t(w) * ht && i * 4 + 3 < rgba.size(); ++i) {
        const std::uint32_t argb = (std::uint32_t(rgba[i * 4 + 3]) << 24) | (std::uint32_t(rgba[i * 4 + 0]) << 16) |
                                   (std::uint32_t(rgba[i * 4 + 1]) << 8) | std::uint32_t(rgba[i * 4 + 2]);
        const bool key = std::find(keys.begin(), keys.end(), argb) != keys.end();
        const std::uint32_t pixel = key ? colour : 0u;
        std::memcpy(out.data() + 128 + i * 4, &pixel, 4);
    }
    made.Put(path, std::move(out));
    madeOnce.insert(path);
    return path;
}

void WorldMapMonsterBox(HudContext& c, bool on) {
    if (c.hud == nullptr) return;
    if (auto* window = dynamic_cast<WorldMapWindow*>(c.hud->Show(hud::kWorldMap))) window->TickMonsterBox(on);
}

void WorldMapGuideStep(HudContext& c, int tab, int heading, int entry) {
    if (c.hud == nullptr) return;
    if (auto* window = dynamic_cast<WorldMapWindow*>(c.hud->Show(hud::kWorldMap))) window->GuideStep(tab, heading, entry);
}

void ChooseReturnPoint(HudContext& c, std::function<void(std::uint32_t)> chosen, int listMarker) {
    if (c.hud == nullptr) return;
    if (auto* window = dynamic_cast<WorldMapWindow*>(c.hud->Show(hud::kWorldMap))) {
        window->Choose(std::move(chosen));
        if (listMarker >= 0) window->ListMarker(std::size_t(listMarker));
    }
}

void RegisterWorldMapWindow(ui::Interface& ui, HudContext& context) {
    // The world map key's window: 1.188's CNIFWorldMap from worldmapguide.2dt (its root's size and frame).
    ui.Register("CIFWorldMap", [c = &context](const assets::UiLayoutNode& n) -> std::unique_ptr<ui::Widget> {
        assets::UiLayoutNode node = n;
        node.type = "CNIFWorldMap";
        if (const auto* records = c->layout2dt ? c->layout2dt(kLayout) : nullptr; records != nullptr && !records->empty()) {
            node.ddj = records->front().frame;
            node.rect.w = records->front().rect.w;
            node.rect.h = records->front().rect.h;
        }
        return std::make_unique<WorldMapWindow>(node, *c);
    });
}

} // namespace sr::client
