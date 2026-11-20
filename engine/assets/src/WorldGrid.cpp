#include "sr/assets/WorldGrid.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sr::assets {

namespace {

bool Contains(const std::vector<RegionCoord>& list, RegionCoord c) {
    return std::find(list.begin(), list.end(), c) != list.end();
}

int Chebyshev(RegionCoord a, RegionCoord b) { return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)); }

} // namespace

RegionCoord WorldGrid::RegionAt(float x, float z) const {
    return {origin_.x + static_cast<int>(std::floor(x / kRegionSize)),
            origin_.y + static_cast<int>(std::floor(z / kRegionSize))};
}

bool WorldGrid::Rebase(float& x, float& z) {
    const RegionCoord at = RegionAt(x, z);
    if (at == origin_) return false;
    x -= OffsetX(at);
    z -= OffsetZ(at);
    origin_ = at;
    return true;
}

std::vector<RegionCoord> WorldGrid::WantedRegions(float x, float z, const std::vector<RegionCoord>& loaded) const {
    const RegionCoord center = RegionAt(x, z);
    // Distance from the camera to each candidate region's centre, in world units.
    const auto distance = [&](RegionCoord c) {
        const float dx = OffsetX(c) + kRegionSize * 0.5f - x;
        const float dz = OffsetZ(c) + kRegionSize * 0.5f - z;
        return dx * dx + dz * dz;
    };
    std::vector<RegionCoord> wanted;
    for (int dy = -loadRadius_; dy <= loadRadius_; ++dy) {
        for (int dx = -loadRadius_; dx <= loadRadius_; ++dx) {
            const RegionCoord c{center.x + dx, center.y + dy};
            if (InWorld(c) && !Contains(loaded, c)) wanted.push_back(c);
        }
    }
    std::sort(wanted.begin(), wanted.end(), [&](RegionCoord a, RegionCoord b) { return distance(a) < distance(b); });
    return wanted;
}

std::vector<RegionCoord> WorldGrid::UnwantedRegions(float x, float z, const std::vector<RegionCoord>& loaded) const {
    const RegionCoord center = RegionAt(x, z);
    std::vector<RegionCoord> unwanted;
    for (const RegionCoord c : loaded) {
        if (Chebyshev(c, center) > unloadRadius_) unwanted.push_back(c);
    }
    return unwanted;
}

} // namespace sr::assets
