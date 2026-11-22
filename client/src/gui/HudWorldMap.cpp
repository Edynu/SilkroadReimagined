// The world map (M): the 1.188 client's CNIFWorldMap, built from res_ui\worldmapguide.2dt
// (window 28, 650 x 422 and its bottom bar). Its view (HudMapView.h, 640 x 384 at 6, 34) shows the world, a place's map
// or a dungeon's floor; the left button drags it. Its buttons: the world (6: the world map, around the player), the pin
// (20: following the player, switching to the map the player stands in, centred on the player; any press in the view
// stops it), the floor spinner (4: up 22, down 24, its label 27; a dungeon of several floors). A place's picture on
// the world map opens that place's map. The player's arrow and the party's members are drawn on it.
// Ours, against the original: the guide (11, CNIFWorldMapGuide) with its side tab, the level's monster list of the
// bottom bar and its box, the small window (7), the tooltips and the fortresses' icons on the world map are not
// built; the player's floor in a dungeon of several is not known yet (the shown floor is kept, else the first).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "gui/Hud.h"
#include "gui/HudMapView.h"
#include "gui/HudWindows.h"
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
    // Not built yet: the guide and its tab, the monster list and its box.
    for (const auto& child : children()) {
        ui::Widget* w = child.get();
        const int id = w->id();
        if (id == kGuide || id == kGuideTab || id == kGuideArrow || id == kMonsterBox || id == kSizeButton) w->visible = false;
        if (id == kMonsterList && w->rect.h < 20.0f) w->visible = false; // the list's line (the corner shares its id)
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
