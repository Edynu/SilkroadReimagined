#pragma once

// Region streaming bookkeeping for the whole world (no GPU, no file access).
// The world is a 256 x 256 grid of regions (kRegionSize units each). Positions
// are kept relative to an "origin region" so float precision stays high far
// from the map origin; the origin moves when the camera crosses into another
// region (Rebase).

#include <cstdint>
#include <vector>

#include "sr/assets/Terrain.h"

namespace sr::assets {

constexpr int kWorldRegions = 256;

struct RegionCoord {
    int x = 0; // column
    int y = 0; // row
    bool operator==(const RegionCoord&) const = default;
};

constexpr bool InWorld(RegionCoord c) { return c.x >= 0 && c.y >= 0 && c.x < kWorldRegions && c.y < kWorldRegions; }

// World-grid streaming window around a camera.
class WorldGrid {
public:
    // `loadRadius`: regions within this many regions (Chebyshev) of the camera's
    // region are wanted. `unloadRadius` (>= loadRadius) adds hysteresis so regions
    // at the border do not thrash while the camera hovers there.
    WorldGrid(int loadRadius, int unloadRadius) : loadRadius_(loadRadius), unloadRadius_(unloadRadius) {}

    RegionCoord origin() const { return origin_; }
    void SetOrigin(RegionCoord origin) { origin_ = origin; }

    // Offset of a region's south-west corner from the origin region's, in world units.
    float OffsetX(RegionCoord c) const { return static_cast<float>(c.x - origin_.x) * kRegionSize; }
    float OffsetZ(RegionCoord c) const { return static_cast<float>(c.y - origin_.y) * kRegionSize; }

    // Region containing the origin-relative position (x, z). May be outside the world.
    RegionCoord RegionAt(float x, float z) const;

    // If the origin-relative position left the origin region, moves the origin to the
    // region it is in and subtracts the shift from x/z. Returns true when it moved.
    bool Rebase(float& x, float& z);

    // Regions to load (inside the world, within loadRadius, not in `loaded`), nearest
    // to (x, z) first.
    std::vector<RegionCoord> WantedRegions(float x, float z, const std::vector<RegionCoord>& loaded) const;
    // Entries of `loaded` farther than unloadRadius from (x, z)'s region.
    std::vector<RegionCoord> UnwantedRegions(float x, float z, const std::vector<RegionCoord>& loaded) const;

private:
    int loadRadius_;
    int unloadRadius_;
    RegionCoord origin_{};
};

} // namespace sr::assets
