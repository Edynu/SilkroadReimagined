#include <doctest/doctest.h>

#include "sr/assets/WorldGrid.h"

using namespace sr::assets;

TEST_CASE("WorldGrid finds the region under a position") {
    WorldGrid grid(1, 2);
    grid.SetOrigin({78, 105});
    CHECK(grid.RegionAt(10.0f, 10.0f) == RegionCoord{78, 105});
    CHECK(grid.RegionAt(kRegionSize + 1.0f, -1.0f) == RegionCoord{79, 104});
    CHECK(grid.OffsetX({80, 105}) == 2.0f * kRegionSize);
    CHECK(grid.OffsetZ({78, 103}) == -2.0f * kRegionSize);
}

TEST_CASE("WorldGrid rebases the origin when the camera crosses a region edge") {
    WorldGrid grid(1, 2);
    grid.SetOrigin({78, 105});
    float x = 100.0f, z = 100.0f;
    CHECK_FALSE(grid.Rebase(x, z));
    x = kRegionSize + 50.0f;
    z = -30.0f;
    CHECK(grid.Rebase(x, z));
    CHECK(grid.origin() == RegionCoord{79, 104});
    CHECK(x == doctest::Approx(50.0f));
    CHECK(z == doctest::Approx(kRegionSize - 30.0f));
    // The camera still sits in the same world place: region 79\104 offset + local == old position.
    CHECK(grid.RegionAt(x, z) == RegionCoord{79, 104});
}

TEST_CASE("WorldGrid wants the nearest unloaded regions first and clips to the world") {
    WorldGrid grid(1, 2);
    grid.SetOrigin({78, 105});
    auto wanted = grid.WantedRegions(kRegionSize * 0.5f, kRegionSize * 0.5f, {});
    REQUIRE(wanted.size() == 9);
    CHECK(wanted.front() == RegionCoord{78, 105}); // the camera's own region first

    wanted = grid.WantedRegions(kRegionSize * 0.5f, kRegionSize * 0.5f, {{78, 105}, {79, 105}});
    CHECK(wanted.size() == 7);

    WorldGrid corner(1, 2);
    corner.SetOrigin({0, 0});
    CHECK(corner.WantedRegions(10.0f, 10.0f, {}).size() == 4); // x,y in 0..1 only

    WorldGrid edge(1, 2);
    edge.SetOrigin({255, 255});
    CHECK(edge.WantedRegions(10.0f, 10.0f, {}).size() == 4);
}

TEST_CASE("WorldGrid unloads only beyond the hysteresis radius") {
    WorldGrid grid(1, 2);
    grid.SetOrigin({78, 105});
    const std::vector<RegionCoord> loaded{{78, 105}, {80, 105}, {81, 105}, {78, 102}};
    const auto unwanted = grid.UnwantedRegions(10.0f, 10.0f, loaded);
    REQUIRE(unwanted.size() == 2);
    CHECK(unwanted[0] == RegionCoord{81, 105});
    CHECK(unwanted[1] == RegionCoord{78, 102});
}
