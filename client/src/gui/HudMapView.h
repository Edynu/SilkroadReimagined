#pragma once

// The world map's view, the 1.188 client's CIFWorldMap / CNIFWorldMap part that
// both the world map window (M) and the fortress map (the battle aide's) are made of: one map of the world map's
// tables drawn in a rect (the world's tiles, a place's picture or a dungeon floor's tiles), dragged with the left
// button to look around, never past its edges; a world place on it in its pixels; its labels and icons
// (worldmap_localinfo.txt), a click on a picture that opens a map.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "sr/assets/WorldMap.h"
#include "sr/ui/Painter.h"
#include "sr/ui/Widget.h"

namespace sr::client {

class MapView {
public:
    // A map by its id: 0..1999 a WLocalmap row, 2001.. a Dungeonmap row. False (nothing shown): no such map, or its
    // pictures are not in the game files (later content).
    bool Show(const assets::WorldMapData& data, int id);
    void Clear();
    bool Shown() const { return map_ != nullptr || dungeon_ != nullptr; }
    int Id() const { return Shown() ? id_ : -1; }
    const assets::WorldMapRow* Map() const { return map_; }
    const assets::DungeonMapRow* Dungeon() const { return dungeon_; }
    float Width() const;
    float Height() const;

    // A place of the world on this map, in its pixels: a region's x, z (1920 to a region; a dungeon's own space
    // for a dungeon region), the world units 192 to a region from region 135 across and 92 down; a dungeon's x
    // and z of 0 at its region 128's corner. None when nothing is shown or the place is another
    // world's (a dungeon's on the world, the world's on a dungeon).
    std::optional<ui::Vec2> Pixel(std::uint16_t region, float x, float z) const;
    // The labels' places: on the world map 32 pixels to a region.
    ui::Vec2 LabelPixel(const assets::WorldMapLabel& label) const;

    // The view: its size, and the map's pixel at its top left.
    void SetViewSize(ui::Vec2 size);
    ui::Vec2 ViewSize() const { return view_; }
    ui::Vec2 Scroll() const { return scroll_; }
    void ScrollBy(ui::Vec2 delta); // kept within the map's edges
    void CentreOn(ui::Vec2 pixel);
    ui::Vec2 ToScreen(ui::Vec2 pixel, const ui::Rect& view) const { return {view.x + pixel.x - scroll_.x, view.y + pixel.y - scroll_.y}; }
    ui::Vec2 FromScreen(ui::Vec2 at, const ui::Rect& view) const { return {at.x - view.x + scroll_.x, at.y - view.y + scroll_.y}; }

    // The map's pictures in `view` (its screen rect), clipped to it; the view's rounded corners
    // (wmap_window_edge_1..4.ddj, 4 x 4) when `corners`.
    void Draw(ui::Painter& paint, const ui::Rect& view, bool corners = true) const;
    // Its labels: texts centred on their place in their colour (shadow and bold as their row says), pictures from
    // their place at their size; `text` resolves a key.
    void DrawLabels(ui::Painter& paint, const ui::Rect& view, const std::function<std::string(const std::string&)>& text) const;
    // The picture label under `at` (screen) that opens a map (none: no such label).
    const assets::WorldMapLabel* LinkAt(ui::Vec2 at, const ui::Rect& view) const;

    // The left button held on `owner` (pressed in the view) moves the map with the mouse; true while it
    // does. A press starts it (`pressed` reports that frame), a move scrolls.
    bool Drag(const ui::Frame& frame, const ui::Widget& owner, const ui::Rect& view, bool* pressed = nullptr);

private:
    const assets::WorldMapData* data_ = nullptr;
    const assets::WorldMapRow* map_ = nullptr;
    const assets::DungeonMapRow* dungeon_ = nullptr;
    int id_ = -1;
    ui::Vec2 view_{640.0f, 384.0f};
    ui::Vec2 scroll_{0, 0};
    std::optional<ui::Vec2> last_; // the mouse while dragging
};

} // namespace sr::client
